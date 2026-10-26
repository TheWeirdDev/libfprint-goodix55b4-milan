// Goodix Tls driver for libfprint

// Copyright (C) 2021 Alexander Meiler <alex.meiler@protonmail.com>
// Copyright (C) 2021 Matthieu CHARETTE <matthieu.charette@gmail.com>

// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.

// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.

// You should have received a copy of the GNU Lesser General Public
// License along with this library; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

#include "fpi-log.h"
#include "fpi-ssm.h"
#include "fpi-usb-transfer.h"
#define FP_COMPONENT "goodixtls"

#include <gio/gio.h>
#include <glib.h>
#include <gusb.h>
#include <stdio.h>
#include <string.h>

#include "drivers_api.h"
#include "goodix.h"
#include "goodix_proto.h"
#include "goodixtls.h"

typedef struct
{
  GoodixTlsServer    *tls_hop;

  GSource            *timeout;

  guint8              cmd;

  gboolean            ack;
  gboolean            reply;

  GoodixCmdCallback   callback;
  gpointer            user_data;
  /* Frees user_data if the command is dropped without its callback (it
   * is a GoodixCallbackInfo the callback would have consumed). */
  GDestroyNotify      user_data_destroy;

  GoodixPackReader    reader;
  guint               rx_errors;    /* consecutive IN transfer errors */
  GSource            *rx_retry;
  gboolean            rx_stopped;   /* the read loop gave up */

  GoodixNoneCallback  tls_callback;      /* pending handshake, or NULL */
  gpointer            tls_user_data;

  GCancellable       *transfer_cancel_tkn;
  gboolean            inited;
} FpiDeviceGoodixTlsPrivate;

/* Consecutive IN errors tolerated before the read loop gives up; retries
 * back off by GOODIX_RX_RETRY_STEP_MS per error. */
#define GOODIX_RX_MAX_ERRORS (5)
#define GOODIX_RX_RETRY_STEP_MS (20)

/* Bound on each wait for the device's TLS packs. */
#define GOODIX_TLS_HANDSHAKE_TIMEOUT_MS (2000)

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE (FpiDeviceGoodixTls, fpi_device_goodixtls,
                                     FP_TYPE_DEVICE);

// TODO remove every GDestroyNotify
// TODO add cmd timeouts

gchar *
data_to_str (guint8 *data, guint32 length)
{
  gchar *string = g_malloc ((length * 2) + 1);

  for (guint32 i = 0; i < length; i++)
    sprintf (string + i * 2, "%02x", data[i]);

  return string;
}

// ---- GOODIX RECEIVE SECTION START ----

void
goodix_receive_done (FpDevice *dev, guint8 *data, guint16 length,
                     GError *error)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);
  GoodixCmdCallback callback = priv->callback;
  gpointer user_data = priv->user_data;

  if (!(priv->ack || priv->reply))
    {
      /* Nothing waits for it (e.g. a late timeout): don't leak the error. */
      g_clear_error (&error);
      return;
    }

  /* user_data goes to the callback, which consumes it. */
  priv->user_data_destroy = NULL;
  goodix_reset_state (dev);
  if (!error)
    fp_dbg ("Completed command: 0x%02x", priv->cmd);

  if (callback)
    callback (dev, data, length, user_data, error);
  else
    g_clear_error (&error);
}

void
goodix_receive_none (FpDevice *dev, guint8 *data, guint16 length,
                     gpointer user_data, GError *error)
{
  g_autofree GoodixCallbackInfo *cb_info = user_data;
  GoodixNoneCallback callback = (GoodixNoneCallback) cb_info->callback;

  callback (dev, cb_info->user_data, error);
}

void
goodix_receive_default (FpDevice *dev, guint8 *data, guint16 length,
                        gpointer user_data, GError *error)
{
  g_autofree GoodixCallbackInfo *cb_info = user_data;
  GoodixDefaultCallback callback = (GoodixDefaultCallback) cb_info->callback;

  callback (dev, data, length, cb_info->user_data, error);
}

void
goodix_receive_success (FpDevice *dev, guint8 *data, guint16 length,
                        gpointer user_data, GError *error)
{
  g_autofree GoodixCallbackInfo *cb_info = user_data;
  GoodixSuccessCallback callback = (GoodixSuccessCallback) cb_info->callback;

  if (error)
    {
      callback (dev, FALSE, cb_info->user_data, error);
      return;
    }

  if (length != sizeof (guint8) * 2)
    {
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Invalid success reply length: %d", length);
      callback (dev, FALSE, cb_info->user_data, error);
      return;
    }

  callback (dev, data[0] == 0x00 ? FALSE : TRUE, cb_info->user_data, NULL);
}

void
goodix_receive_reset (FpDevice *dev, guint8 *data, guint16 length,
                      gpointer user_data, GError *error)
{
  g_autofree GoodixCallbackInfo *cb_info = user_data;
  GoodixResetCallback callback = (GoodixResetCallback) cb_info->callback;

  if (error)
    {
      callback (dev, FALSE, 0, cb_info->user_data, error);
      return;
    }

  if (length != sizeof (guint8) + sizeof (guint16))
    {
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Invalid reset reply length: %d", length);
      callback (dev, FALSE, 0, cb_info->user_data, error);
      return;
    }

  /* Byte-wise: data + 1 is unaligned (was a misaligned guint16 load). */
  callback (dev, data[0] == 0x00 ? FALSE : TRUE,
            (guint16) (data[1] | (data[2] << 8)),
            cb_info->user_data, NULL);
}

void
goodix_receive_preset_psk_read (FpDevice *dev, guint8 *data, guint16 length,
                                gpointer user_data, GError *error)
{
  g_autofree GoodixCallbackInfo *cb_info = user_data;
  GoodixPresetPskReadCallback callback =
    (GoodixPresetPskReadCallback) cb_info->callback;
  const guint8 *psk = NULL;
  guint32 flags = 0, psk_len = 0;

  if (error)
    {
      callback (dev, FALSE, 0x00000000, NULL, 0, cb_info->user_data, error);
      return;
    }

  /* Was checked against a 13-byte header with an "offset" field the reply
   * does not have, while the PSK was read from byte 9: a valid reply with
   * a PSK shorter than 4 bytes was rejected. */
  switch (goodix_parse_preset_psk_read (data, length, &flags, &psk, &psk_len))
    {
    case GOODIX_PSK_READ_FAILED:
      callback (dev, FALSE, 0x00000000, NULL, 0, cb_info->user_data, NULL);
      return;

    case GOODIX_PSK_READ_INVALID:
      fp_warn ("Preset PSK read: invalid reply (%u bytes)", length);
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Invalid preset PSK read reply length: %d", length);
      callback (dev, FALSE, 0x00000000, NULL, 0, cb_info->user_data, error);
      return;

    case GOODIX_PSK_READ_OK:
      break;
    }

  fp_dbg ("preset PSK: length %u, flags 0x%08x", psk_len, flags);
  callback (dev, TRUE, flags, (guint8 *) psk, psk_len, cb_info->user_data,
            NULL);
}

void
goodix_receive_preset_psk_write (FpDevice *dev, guint8 *data,
                                 guint16 length, gpointer user_data,
                                 GError *error)
{
  g_autofree GoodixCallbackInfo *cb_info = user_data;
  GoodixSuccessCallback callback = (GoodixSuccessCallback) cb_info->callback;

  if (error)
    {
      callback (dev, FALSE, cb_info->user_data, error);
      return;
    }

  if (length < sizeof (guint8))
    {
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Invalid preset PSK write reply length: %d", length);
      callback (dev, FALSE, cb_info->user_data, error);
      return;
    }

  callback (dev, data[0] == 0x00 ? TRUE : FALSE, cb_info->user_data, NULL);
}

void
goodix_receive_firmware_version (FpDevice *dev, guint8 *data,
                                 guint16 length, gpointer user_data,
                                 GError *error)
{
  g_autofree gchar *payload = g_malloc (length + sizeof (gchar));
  g_autofree GoodixCallbackInfo *cb_info = user_data;
  GoodixFirmwareVersionCallback callback =
    (GoodixFirmwareVersionCallback) cb_info->callback;

  if (error)
    {
      callback (dev, NULL, cb_info->user_data, error);
      return;
    }

  memcpy (payload, data, length);

  // Some device send the firmware without the null terminator
  payload[length] = 0x00;

  callback (dev, payload, cb_info->user_data, NULL);
}

void
goodix_receive_ack (FpDevice *dev, guint8 *data, guint16 length,
                    gpointer user_data, GError *error)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);
  GoodixAck *ack = (GoodixAck *) data;
  guint8 cmd;

  if (length != sizeof (GoodixAck))
    {
      fp_warn ("Invalid ACK length: %d", length);
      return;
    }

  if (!ack->always_true)
    {
      // Warn about error.
      fp_warn ("Invalid ACK flags: 0x%02x", data[sizeof (guint8)]);
      return;
    }

  cmd = ack->cmd;

  if (ack->has_no_config)
    fp_warn ("MCU has no config");

  if (priv->cmd != cmd)
    {
      fp_warn ("Invalid ACK command: 0x%02x", cmd);
      return;
    }

  if (!priv->ack)
    {
      fp_warn ("Didn't excpect an ACK for command: 0x%02x", priv->cmd);
      return;
    }

  if (!priv->reply)
    {
      G_DEBUG_HERE ();
      goodix_receive_done (dev, NULL, 0, NULL);
      return;
    }

  priv->ack = FALSE;
}

void
goodix_receive_protocol (FpDevice *dev, guint8 *data, guint32 length)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);
  guint8 cmd;
  g_autofree guint8 *payload = NULL;
  guint16 payload_len;
  gboolean valid_checksum, valid_null_checksum; // TODO implement checksum.

  if (!goodix_decode_protocol (data, length, &cmd, &payload, &payload_len,
                               &valid_checksum, &valid_null_checksum))
    {
      fp_err ("Incomplete, size: %d", length);
      // Protocol is not full, we still need data.
      // TODO implement protocol assembling.
      return;
    }

  if (cmd == GOODIX_CMD_ACK)
    {
      fp_dbg ("got ack");
      goodix_receive_ack (dev, payload, payload_len, NULL, NULL);
      return;
    }

  if (priv->cmd != cmd)
    {
      fp_warn ("Invalid protocol command: 0x%02x", cmd);
      return;
    }

  if (!priv->reply)
    {
      fp_warn ("Didn't excpect a reply for command: 0x%02x", priv->cmd);
      return;
    }

  if (priv->ack)
    fp_warn ("Didn't get ACK for command: 0x%02x", priv->cmd);

  goodix_receive_done (dev, payload, payload_len, NULL);
}

static void
goodix_dispatch_pack (FpDevice *dev, guint8 flags, guint8 *payload,
                      guint16 payload_len)
{
  switch (flags)
    {
    case GOODIX_FLAGS_MSG_PROTOCOL:
      fp_dbg ("Got protocol msg");
      goodix_receive_protocol (dev, payload, payload_len);
      break;

    case GOODIX_FLAGS_TLS:
      fp_dbg ("Got TLS msg");
      goodix_receive_done (dev, payload, payload_len, NULL);
      break;

    case GOODIX_FLAGS_TLS_DATA:
        fp_dbg("Got TLS data msg");
        // GOODIX 55x4: Remove first 9 to get valid TLS content
        if (payload_len < 9)
          {
            /* Was passed on unchecked: payload_len - 9 wrapped around and
             * the TLS layer read far past the payload. */
            fp_warn ("Short TLS data pack: %u bytes", payload_len);
            goodix_receive_done (dev, NULL, 0,
                                 fpi_device_error_new_msg (
                                   FP_DEVICE_ERROR_PROTO,
                                   "Short TLS data pack (%u bytes)",
                                   payload_len));
            break;
          }
        goodix_receive_done(dev, payload+9, payload_len-9, NULL);
        break;

    default:
      fp_warn ("Unknown flags: 0x%02x", flags);
      break;
    }
}

/* Was a single accumulator decoded once per transfer: bytes after the first
 * pack were dropped, and the partial reply of an aborted command was
 * prepended to the next one. */
void
goodix_receive_pack (FpDevice *dev, guint8 *data, guint32 length)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);

  goodix_pack_reader_push (&priv->reader, data, length);
  for (;;)
    {
      g_autofree guint8 *payload = NULL;
      guint8 flags;
      guint16 payload_len;
      gboolean valid_checksum;
      gsize dropped;

      switch (goodix_pack_reader_next (&priv->reader, &flags, &payload,
                                       &payload_len, &valid_checksum,
                                       &dropped))
        {
        case GOODIX_PACK_INCOMPLETE:
          if (goodix_pack_reader_buffered (&priv->reader))
            fp_dbg ("not full packet (%zu bytes buffered)",
                    goodix_pack_reader_buffered (&priv->reader));
          return;

        case GOODIX_PACK_DROPPED:
          fp_warn ("Dropped %zu bytes of unparseable data", dropped);
          continue;

        case GOODIX_PACK_READY:
          if (!valid_checksum)
            fp_warn ("Bad header checksum on pack 0x%02x (%u bytes)", flags,
                     payload_len);
          /* The callback may send commands or abort; the reader stays
           * consistent since only whole packs have been taken out. */
          goodix_dispatch_pack (dev, flags, payload, payload_len);
          continue;
        }
    }
}

static void goodix_receive_retry_cb (FpDevice *dev,
                                     gpointer  user_data);

void
goodix_receive_data_cb (FpiUsbTransfer *transfer, FpDevice *dev,
                        gpointer user_data, GError *error)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);

  g_autoptr(GError) owned_error = error; /* the callback owns it */

  /* The token is replaced by goodix_dev_init, so a transfer cancelled by
   * goodix_dev_deinit can complete after a re-init: check the error too. */
  if (g_cancellable_is_cancelled (priv->transfer_cancel_tkn) ||
      g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      fp_dbg ("transfer cancelled, aborting read loop...");
      return;
    }
  if (error)
    {
      /* Was resubmitted at once, forever: a busy loop once the device is
       * gone. Stop on a vanished device or repeated errors, failing the
       * pending command; otherwise retry after a short delay. */
      if (g_error_matches (error, G_USB_DEVICE_ERROR,
                           G_USB_DEVICE_ERROR_NO_DEVICE) ||
          ++priv->rx_errors > GOODIX_RX_MAX_ERRORS)
        {
          fp_warn ("Receive data error: %s; stopping the read loop",
                   error->message);
          priv->inited = FALSE;
          priv->rx_stopped = TRUE;
          goodix_pack_reader_reset (&priv->reader);
          goodix_receive_done (dev, NULL, 0, g_error_copy (error));
          return;
        }
      fp_warn ("Receive data error (%u/%u): %s; retrying", priv->rx_errors,
               GOODIX_RX_MAX_ERRORS, error->message);
      g_clear_pointer (&priv->rx_retry, g_source_destroy);
      priv->rx_retry = fpi_device_add_timeout (
        dev, priv->rx_errors * GOODIX_RX_RETRY_STEP_MS,
        goodix_receive_retry_cb, NULL, NULL);
      return;
    }
  priv->rx_errors = 0;

  goodix_receive_pack (dev, transfer->buffer, transfer->actual_length);

  goodix_receive_data (dev);
}

void
goodix_receive_timeout_cb (FpDevice *dev, gpointer user_data)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);
  GError *error = NULL;

  /* The source is being dispatched and goes away on return. */
  priv->timeout = NULL;
  g_set_error (&error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
               "Command timed out: 0x%02x", priv->cmd);
  /* A partial reply would be taken for the start of the next one. */
  goodix_pack_reader_reset (&priv->reader);
  goodix_receive_done (dev, NULL, 0, error);
}

static void
goodix_receive_retry_cb (FpDevice *dev, gpointer user_data)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);

  priv->rx_retry = NULL;
  if (!priv->inited || g_cancellable_is_cancelled (priv->transfer_cancel_tkn))
    return;
  goodix_receive_data (dev);
}

void
goodix_start_read_loop (FpDevice *dev)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);

  if (priv->inited)
    // Already going
    return;
  else
    priv->inited = TRUE;
  priv->rx_errors = 0;
  priv->rx_stopped = FALSE;
  if (g_cancellable_is_cancelled (priv->transfer_cancel_tkn))
    g_cancellable_reset (priv->transfer_cancel_tkn);

  goodix_receive_data (dev);
}

void
goodix_receive_data (FpDevice *dev)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsClass *class = FPI_DEVICE_GOODIXTLS_GET_CLASS (self);
  FpiUsbTransfer *transfer = fpi_usb_transfer_new (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);

  transfer->short_is_error = FALSE;

  fpi_usb_transfer_fill_bulk (transfer, class->ep_in,
                              GOODIX_EP_IN_MAX_BUF_SIZE);

  fpi_usb_transfer_submit (transfer, 0, priv->transfer_cancel_tkn,
                           goodix_receive_data_cb, NULL);
}

// ---- GOODIX RECEIVE SECTION END ----

// -----------------------------------------------------------------------------

// ---- GOODIX SEND SECTION START ----

gboolean
goodix_send_data (FpDevice *dev, guint8 *data, guint32 length,
                  GDestroyNotify free_func, GError **error)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsClass *class = FPI_DEVICE_GOODIXTLS_GET_CLASS (self);

  for (guint32 i = 0; i < length; i += GOODIX_EP_OUT_MAX_BUF_SIZE)
    {
      FpiUsbTransfer *transfer = fpi_usb_transfer_new (dev);
      transfer->short_is_error = TRUE;

      fpi_usb_transfer_fill_bulk_full (transfer, class->ep_out, data + i,
                                       GOODIX_EP_OUT_MAX_BUF_SIZE, NULL);

      if (!fpi_usb_transfer_submit_sync (transfer, GOODIX_TIMEOUT,
                                         error))
        {
          if (free_func)
            free_func (data);
          fpi_usb_transfer_unref (transfer);
          return FALSE;
        }
      fpi_usb_transfer_unref (transfer);
    }

  if (free_func)
    free_func (data);
  return TRUE;
}

gboolean
goodix_send_pack (FpDevice *dev, guint8 flags, guint8 *payload,
                  guint16 length, GDestroyNotify free_func,
                  GError **error)
{
  guint8 *data;
  guint32 data_len;

  goodix_encode_pack (flags, payload, length, TRUE, &data, &data_len);
  if (free_func)
    free_func (payload);

  return goodix_send_data (dev, data, data_len, g_free, error);
}

/* Starts a command. Returns FALSE with error set, and without calling the
 * callback, if it could not be sent (another command is running, the read
 * loop stopped, or the USB write failed). The payload is freed with
 * free_func in every case. With a callback, user_data is freed with
 * user_data_destroy if the command is dropped (goodix_reset_state)
 * instead of completed. */
static gboolean
goodix_send_protocol_full (FpDevice *dev, guint8 cmd, guint8 *payload,
                           guint16 length, GDestroyNotify free_func,
                           gboolean calc_checksum, guint timeout_ms,
                           gboolean reply, GoodixCmdCallback callback,
                           gpointer user_data, GDestroyNotify user_data_destroy,
                           GError **error)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);
  GError *send_error = NULL;
  guint8 *data;
  guint32 data_len;

  if (priv->ack || priv->reply || priv->timeout)
    {
      fp_warn ("A command is already running: 0x%02x (new: 0x%02x)",
               priv->cmd, cmd);
      if (free_func)
        free_func (payload);
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_BUSY,
                   "Command 0x%02x sent while 0x%02x is running", cmd,
                   priv->cmd);
      return FALSE;
    }
  if (priv->rx_stopped)
    {
      if (free_func)
        free_func (payload);
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                   "Command 0x%02x: the device stopped answering", cmd);
      return FALSE;
    }

  fp_dbg ("Running command: 0x%02x", cmd);

  if (timeout_ms)
    {
      priv->timeout = fpi_device_add_timeout (
        dev, timeout_ms, goodix_receive_timeout_cb, NULL, NULL);
      /* fpi_device_add_timeout() counts from the main loop's cached time
       * (g_source_get_time), which is stale if the loop was blocked before
       * this command (e.g. a client waiting on stdin); the timeout would
       * then fire at once although the device answers in ~3 ms. Re-arm it
       * from the real monotonic clock (the same clock GLib uses). */
      g_source_set_ready_time (priv->timeout,
                               g_get_monotonic_time () +
                               (gint64) timeout_ms * 1000);
    }
  priv->cmd = cmd;
  priv->ack = TRUE;
  priv->reply = reply;
  priv->callback = callback;
  priv->user_data = user_data;
  priv->user_data_destroy = callback ? user_data_destroy : NULL;

  goodix_encode_protocol (cmd, payload, length, calc_checksum, FALSE,
                          &data, &data_len);
  if (free_func)
    free_func (payload);

  if (!goodix_send_pack (dev, GOODIX_FLAGS_MSG_PROTOCOL, data, data_len,
                         g_free, &send_error))
    {
      /* Not started: the caller reports it, user_data stays the caller's. */
      priv->user_data_destroy = NULL;
      goodix_reset_state (dev);
      g_propagate_error (error, send_error);
      return FALSE;
    }
  return TRUE;
}

/* Every non-NULL callback passed here is a goodix_receive_* adapter whose
 * user_data is a GoodixCallbackInfo it consumes; the callback is always
 * called, also when the command cannot be sent. */
void
goodix_send_protocol (
  FpDevice *dev, guint8 cmd, guint8 *payload, guint16 length,
  GDestroyNotify free_func, gboolean calc_checksum, guint timeout_ms,
  gboolean reply, GoodixCmdCallback callback, gpointer user_data)
{
  GError *error = NULL;

  if (goodix_send_protocol_full (dev, cmd, payload, length, free_func,
                                 calc_checksum, timeout_ms, reply, callback,
                                 user_data, g_free, &error))
    return;

  /* Was dropped silently when busy: the caller's state machine hung. */
  if (callback)
    callback (dev, NULL, 0, user_data, error);
  else
    {
      fp_warn ("%s", error->message);
      g_error_free (error);
    }
}

/* Sends a command the device only ACKs, if at all, and completes it at
 * once (as before: a late ACK is logged and ignored). Returns FALSE with
 * error set if it could not be sent. */
static gboolean
goodix_send_unacked (FpDevice *dev, guint8 cmd, guint8 *payload,
                     guint16 length, gboolean calc_checksum, GError **error)
{
  if (!goodix_send_protocol_full (dev, cmd, payload, length, NULL,
                                  calc_checksum, 0, FALSE, NULL, NULL, NULL,
                                  error))
    return FALSE;
  goodix_receive_done (dev, NULL, 0, NULL);
  return TRUE;
}

/* Logs and frees an error nobody can be told about. */
static void
goodix_log_unreported (GError *error)
{
  if (error)
    fp_warn ("%s", error->message);
  g_clear_error (&error);
}

void
goodix_send_nop (FpDevice *dev, GoodixNoneCallback callback,
                 gpointer user_data)
{
  GoodixNop payload = {.unknown = 0x00000000};
  GError *error = NULL;

  goodix_send_unacked (dev, GOODIX_CMD_NOP, (guint8 *) &payload,
                       sizeof (payload), FALSE, &error);
  if (callback)
    callback (dev, user_data, error);
  else
    goodix_log_unreported (error);
}

/* Was: completed a command it might not have sent (when busy, the running
 * command instead), reported success whatever happened, and leaked a
 * GoodixCallbackInfo. */
void goodix_send_drv_state(FpDevice *dev, GoodixSuccessCallback callback,
                     gpointer user_data) {
  GoodixSetDrvState payload = {.unknown = TRUE};
  GError *error = NULL;
  gboolean sent;

  sent = goodix_send_unacked (dev, GOODIX_CMD_SET_DRV_STATE,
                              (guint8 *) &payload, sizeof (payload), FALSE,
                              &error);
  if (callback)
    callback (dev, sent, user_data, error);
  else
    goodix_log_unreported (error);
}

void goodix_send_mcu_get_pov_image(FpDevice *dev, GoodixSuccessCallback callback,
                     gpointer user_data) {
  GoodixSetDrvState payload = {.unknown = FALSE};
  GError *error = NULL;
  gboolean sent;

  sent = goodix_send_unacked (dev, GOODIX_CMD_MCU_GET_POV_IMAGE,
                              (guint8 *) &payload, sizeof (payload), FALSE,
                              &error);
  if (callback)
    callback (dev, sent, user_data, error);
  else
    goodix_log_unreported (error);
}

void
goodix_send_mcu_get_image (FpDevice *dev, GoodixImageCallback callback,
                           gpointer user_data)
{
  GoodixDefault payload = {.unused_flags = 0x01};
  GoodixCallbackInfo *cb_info;

  if (callback)
    {
      cb_info = malloc (sizeof (GoodixCallbackInfo));

      cb_info->callback = G_CALLBACK (callback);
      cb_info->user_data = user_data;

      goodix_send_protocol (dev, GOODIX_CMD_MCU_GET_IMAGE, (guint8 *) &payload,
                            sizeof (payload), NULL, TRUE, GOODIX_TIMEOUT, TRUE,
                            goodix_receive_default, cb_info);
      return;
    }

  goodix_send_protocol (dev, GOODIX_CMD_MCU_GET_IMAGE, (guint8 *) &payload,
                        sizeof (payload), NULL, TRUE, GOODIX_TIMEOUT, TRUE,
                        NULL, NULL);
}

void
goodix_send_mcu_switch_to_fdt_down (FpDevice *dev, guint8 *mode,
                                    guint16 length,
                                    GDestroyNotify free_func,
                                    GoodixDefaultCallback callback,
                                    gpointer user_data)
{
  GoodixCallbackInfo *cb_info;

  if (callback)
    {
      cb_info = malloc (sizeof (GoodixCallbackInfo));

      cb_info->callback = G_CALLBACK (callback);
      cb_info->user_data = user_data;

      goodix_send_protocol (dev, GOODIX_CMD_MCU_SWITCH_TO_FDT_DOWN, mode, length,
                            free_func, TRUE, 0, TRUE, goodix_receive_default,
                            cb_info);
      return;
    }

  goodix_send_protocol (dev, GOODIX_CMD_MCU_SWITCH_TO_FDT_DOWN, mode, length,
                        free_func, TRUE, 0, TRUE, NULL, NULL);
}

void
goodix_send_mcu_switch_to_fdt_up (FpDevice *dev, guint8 *mode,
                                  guint16 length, GDestroyNotify free_func,
                                  GoodixDefaultCallback callback,
                                  gpointer user_data)
{
  GoodixCallbackInfo *cb_info;

  if (callback)
    {
      cb_info = malloc (sizeof (GoodixCallbackInfo));

      cb_info->callback = G_CALLBACK (callback);
      cb_info->user_data = user_data;

      goodix_send_protocol (dev, GOODIX_CMD_MCU_SWITCH_TO_FDT_UP, mode, length,
                            free_func, TRUE, 0, TRUE, goodix_receive_default,
                            cb_info);
      return;
    }

  goodix_send_protocol (dev, GOODIX_CMD_MCU_SWITCH_TO_FDT_UP, mode, length,
                        free_func, TRUE, 0, TRUE, NULL, NULL);
}

void
goodix_send_mcu_switch_to_fdt_up_no_reply (FpDevice *dev, guint8 *mode,
                                  guint16 length, GDestroyNotify free_func,
                                  GoodixDefaultCallback callback,
                                  gpointer user_data)
{
  GoodixCallbackInfo *cb_info;

  if (callback)
    {
      cb_info = malloc (sizeof (GoodixCallbackInfo));

      cb_info->callback = G_CALLBACK (callback);
      cb_info->user_data = user_data;

      goodix_send_protocol (dev, GOODIX_CMD_MCU_SWITCH_TO_FDT_UP, mode, length,
                            free_func, TRUE, 0, FALSE, goodix_receive_default,
                            cb_info);
      return;
    }

  goodix_send_protocol (dev, GOODIX_CMD_MCU_SWITCH_TO_FDT_UP, mode, length,
                        free_func, TRUE, 0, FALSE, NULL, NULL);
}

void
goodix_send_mcu_switch_to_fdt_mode (FpDevice *dev, guint8 *mode,
                                    guint16 length,
                                    GDestroyNotify free_func,
                                    GoodixDefaultCallback callback,
                                    gpointer user_data)
{
  GoodixCallbackInfo *cb_info;

  if (callback)
    {
      cb_info = malloc (sizeof (GoodixCallbackInfo));

      cb_info->callback = G_CALLBACK (callback);
      cb_info->user_data = user_data;

      goodix_send_protocol (dev, GOODIX_CMD_MCU_SWITCH_TO_FDT_MODE, mode, length,
                            free_func, TRUE, 0, TRUE, goodix_receive_default,
                            cb_info);
      return;
    }

  goodix_send_protocol (dev, GOODIX_CMD_MCU_SWITCH_TO_FDT_MODE, mode, length,
                        free_func, TRUE, 0, TRUE, NULL, NULL);
}

void
goodix_send_nav_0 (FpDevice *dev, GoodixDefaultCallback callback,
                   gpointer user_data)
{
  GoodixDefault payload = {.unused_flags = 0x01};
  GoodixCallbackInfo *cb_info;

  if (callback)
    {
      cb_info = malloc (sizeof (GoodixCallbackInfo));

      cb_info->callback = G_CALLBACK (callback);
      cb_info->user_data = user_data;

      goodix_send_protocol (dev, GOODIX_CMD_NAV_0, (guint8 *) &payload,
                            sizeof (payload), NULL, TRUE, GOODIX_TIMEOUT, TRUE,
                            goodix_receive_default, cb_info);
      return;
    }

  goodix_send_protocol (dev, GOODIX_CMD_NAV_0, (guint8 *) &payload,
                        sizeof (payload), NULL, TRUE, GOODIX_TIMEOUT, TRUE, NULL,
                        NULL);
}

void
goodix_send_mcu_switch_to_idle_mode (FpDevice *dev, guint8 sleep_time,
                                     GoodixNoneCallback callback,
                                     gpointer user_data)
{
  GoodixMcuSwitchToIdleMode payload = {.sleep_time = sleep_time};
  GoodixCallbackInfo *cb_info;

  if (callback)
    {
      cb_info = malloc (sizeof (GoodixCallbackInfo));

      cb_info->callback = G_CALLBACK (callback);
      cb_info->user_data = user_data;

      goodix_send_protocol (dev, GOODIX_CMD_MCU_SWITCH_TO_IDLE_MODE,
                            (guint8 *) &payload, sizeof (payload), NULL, TRUE,
                            GOODIX_TIMEOUT, FALSE, goodix_receive_none, cb_info);
      return;
    }

  goodix_send_protocol (dev, GOODIX_CMD_MCU_SWITCH_TO_IDLE_MODE,
                        (guint8 *) &payload, sizeof (payload), NULL, TRUE,
                        GOODIX_TIMEOUT, FALSE, NULL, NULL);
}

void
goodix_send_mcu_switch_to_sleep_mode (FpDevice *dev, guint8 sleep_time,
                                     GoodixNoneCallback callback,
                                     gpointer user_data)
{
  GoodixMcuSwitchToIdleMode payload = {.sleep_time = sleep_time};
  GoodixCallbackInfo *cb_info;

  if (callback)
    {
      cb_info = malloc (sizeof (GoodixCallbackInfo));

      cb_info->callback = G_CALLBACK (callback);
      cb_info->user_data = user_data;

      goodix_send_protocol (dev, GOODIX_CMD_MCU_SWITCH_TO_SLEEP_MODE,
                            (guint8 *) &payload, sizeof (payload), NULL, TRUE,
                            GOODIX_TIMEOUT, FALSE, goodix_receive_none, cb_info);
      return;
    }

  goodix_send_protocol (dev, GOODIX_CMD_MCU_SWITCH_TO_SLEEP_MODE,
                        (guint8 *) &payload, sizeof (payload), NULL, TRUE,
                        GOODIX_TIMEOUT, FALSE, NULL, NULL);
}

void
goodix_send_mcu_switch_to_sleep_mode_realtek (FpDevice *dev, guint8 value,
                                     GoodixSuccessCallback callback,
                                     gpointer user_data)
{
  GoodixMcuSwitchToSleepModeRealtek payload = {.value = value};
  GoodixCallbackInfo *cb_info;

  if (callback)
    {
      cb_info = malloc (sizeof (GoodixCallbackInfo));

      cb_info->callback = G_CALLBACK (callback);
      cb_info->user_data = user_data;

      goodix_send_protocol (dev, GOODIX_CMD_MCU_SWITCH_TO_SLEEP_MODE_REALTEK,
                            (guint8 *) &payload, sizeof (payload), NULL, TRUE,
                            GOODIX_TIMEOUT, TRUE, goodix_receive_success, cb_info);
      return;
    }

  goodix_send_protocol (dev, GOODIX_CMD_MCU_SWITCH_TO_SLEEP_MODE_REALTEK,
                        (guint8 *) &payload, sizeof (payload), NULL, TRUE,
                        GOODIX_TIMEOUT, TRUE, NULL, NULL);
}

void
goodix_send_write_sensor_register (FpDevice *dev, guint16 address,
                                   guint16 value,
                                   GoodixNoneCallback callback,
                                   gpointer user_data)
{
  // Only support one address and one value

  /* Was hardcoded to register 556 with a broken initializer (wrote 0x05),
   * ignoring the address and value arguments. */
  GoodixWriteSensorRegister payload = {.multiples = FALSE,
                                       .address = GUINT16_TO_LE (address),
                                       .value = GUINT16_TO_LE (value)};
  GoodixCallbackInfo *cb_info;

  if (callback)
    {
      cb_info = malloc (sizeof (GoodixCallbackInfo));

      cb_info->callback = G_CALLBACK (callback);
      cb_info->user_data = user_data;

    goodix_send_protocol(dev, GOODIX_CMD_WRITE_SENSOR_REGISTER,
                         (guint8 *)&payload, sizeof(payload), NULL, TRUE,
                         GOODIX_TIMEOUT, FALSE, goodix_receive_none, cb_info);
    return;
  }

  goodix_send_protocol (dev, GOODIX_CMD_WRITE_SENSOR_REGISTER,
                        (guint8 *) &payload, sizeof (payload), NULL, TRUE,
                        GOODIX_TIMEOUT, FALSE, NULL, NULL);
}

void
goodix_send_read_sensor_register (FpDevice *dev, guint16 address,
                                  guint8 length,
                                  GoodixDefaultCallback callback,
                                  gpointer user_data)
{
  // Only support one address

  GoodixReadSensorRegister payload = {
    .multiples = FALSE, .address = GUINT16_TO_LE (address), .length = length
  };
  GoodixCallbackInfo *cb_info;

  if (callback)
    {
      cb_info = malloc (sizeof (GoodixCallbackInfo));

      cb_info->callback = G_CALLBACK (callback);
      cb_info->user_data = user_data;

      goodix_send_protocol (dev, GOODIX_CMD_READ_SENSOR_REGISTER,
                            (guint8 *) &payload, sizeof (payload), NULL, TRUE,
                            GOODIX_TIMEOUT, TRUE, goodix_receive_default, cb_info);
      return;
    }

  goodix_send_protocol (dev, GOODIX_CMD_READ_SENSOR_REGISTER, (guint8 *) &payload,
                        sizeof (payload), NULL, TRUE, GOODIX_TIMEOUT, TRUE, NULL,
                        NULL);
}

void
goodix_send_upload_config_mcu (FpDevice *dev, guint8 *config,
                               guint16 length, GDestroyNotify free_func,
                               GoodixSuccessCallback callback,
                               gpointer user_data)
{
  GoodixCallbackInfo *cb_info;

  if (callback)
    {
      cb_info = malloc (sizeof (GoodixCallbackInfo));

      cb_info->callback = G_CALLBACK (callback);
      cb_info->user_data = user_data;

      goodix_send_protocol (dev, GOODIX_CMD_UPLOAD_CONFIG_MCU, config, length,
                            free_func, TRUE, GOODIX_TIMEOUT, TRUE,
                            goodix_receive_success, cb_info);
      return;
    }

  goodix_send_protocol (dev, GOODIX_CMD_UPLOAD_CONFIG_MCU, config, length,
                        free_func, TRUE, GOODIX_TIMEOUT, TRUE, NULL, NULL);
}

void
goodix_send_set_powerdown_scan_frequency (FpDevice             *dev,
                                          guint16               powerdown_scan_frequency,
                                          GoodixSuccessCallback callback,
                                          gpointer              user_data)
{
  GoodixSetPowerdownScanFrequency payload = {
    .powerdown_scan_frequency = GUINT16_TO_LE (powerdown_scan_frequency)
  };
  GoodixCallbackInfo *cb_info;

  if (callback)
    {
      cb_info = malloc (sizeof (GoodixCallbackInfo));

      cb_info->callback = G_CALLBACK (callback);
      cb_info->user_data = user_data;

      goodix_send_protocol (dev, GOODIX_CMD_SET_POWERDOWN_SCAN_FREQUENCY,
                            (guint8 *) &payload, sizeof (payload), NULL, TRUE,
                            GOODIX_TIMEOUT, TRUE, goodix_receive_success, cb_info);
      return;
    }

  goodix_send_protocol (dev, GOODIX_CMD_SET_POWERDOWN_SCAN_FREQUENCY,
                        (guint8 *) &payload, sizeof (payload), NULL, TRUE,
                        GOODIX_TIMEOUT, TRUE, NULL, NULL);
}

void
goodix_send_enable_chip (FpDevice *dev, gboolean enable,
                         GoodixNoneCallback callback, gpointer user_data)
{
  GoodixEnableChip payload = {.enable = enable ? TRUE : FALSE};
  GoodixCallbackInfo *cb_info;

  if (callback)
    {
      cb_info = malloc (sizeof (GoodixCallbackInfo));

      cb_info->callback = G_CALLBACK (callback);
      cb_info->user_data = user_data;

      goodix_send_protocol (dev, GOODIX_CMD_ENABLE_CHIP, (guint8 *) &payload,
                            sizeof (payload), NULL, TRUE, GOODIX_TIMEOUT, FALSE,
                            goodix_receive_none, cb_info);
      return;
    }

  goodix_send_protocol (dev, GOODIX_CMD_ENABLE_CHIP, (guint8 *) &payload,
                        sizeof (payload), NULL, TRUE, GOODIX_TIMEOUT, FALSE, NULL,
                        NULL);
}

void
goodix_send_reset (FpDevice *dev, gboolean reset_sensor, guint8 sleep_time,
                   GoodixResetCallback callback, gpointer user_data)
{
  // Only support reset sensor

  GoodixReset payload = {.soft_reset_mcu = FALSE,
                         .reset_sensor = reset_sensor ? TRUE : FALSE,
                         .other = 1,
                         .sleep_time = sleep_time};
  GoodixCallbackInfo *cb_info;

  if (callback)
    {
      cb_info = malloc (sizeof (GoodixCallbackInfo));

      cb_info->callback = G_CALLBACK (callback);
      cb_info->user_data = user_data;

      goodix_send_protocol (dev, GOODIX_CMD_RESET, (guint8 *) &payload,
                            sizeof (payload), NULL, TRUE, GOODIX_TIMEOUT, TRUE,
                            goodix_receive_reset, cb_info);
      return;
    }

  goodix_send_protocol (dev, GOODIX_CMD_RESET, (guint8 *) &payload,
                        sizeof (payload), NULL, TRUE, GOODIX_TIMEOUT, TRUE, NULL,
                        NULL);
}

void
goodix_send_firmware_version (FpDevice                     *dev,
                              GoodixFirmwareVersionCallback callback,
                              gpointer                      user_data)
{
  GoodixNone payload = {};
  GoodixCallbackInfo *cb_info;

  if (callback)
    {
      cb_info = malloc (sizeof (GoodixCallbackInfo));

      cb_info->callback = G_CALLBACK (callback);
      cb_info->user_data = user_data;

      goodix_send_protocol (dev, GOODIX_CMD_FIRMWARE_VERSION, (guint8 *) &payload,
                            sizeof (payload), NULL, TRUE, GOODIX_TIMEOUT, TRUE,
                            goodix_receive_firmware_version, cb_info);
      return;
    }

  goodix_send_protocol (dev, GOODIX_CMD_FIRMWARE_VERSION, (guint8 *) &payload,
                        sizeof (payload), NULL, TRUE, GOODIX_TIMEOUT, TRUE, NULL,
                        NULL);
}

void
goodix_send_query_mcu_state (FpDevice *dev, GoodixDefaultCallback callback,
                             gpointer user_data)
{
  GoodixQueryMcuState payload = {.unused_flags = 0x55};
  GoodixCallbackInfo *cb_info;

  if (callback)
    {
      cb_info = malloc (sizeof (GoodixCallbackInfo));

      cb_info->callback = G_CALLBACK (callback);
      cb_info->user_data = user_data;

      goodix_send_protocol (dev, GOODIX_CMD_QUERY_MCU_STATE, (guint8 *) &payload,
                            sizeof (payload), NULL, TRUE, GOODIX_TIMEOUT, TRUE,
                            goodix_receive_default, cb_info);
      return;
    }

  goodix_send_protocol (dev, GOODIX_CMD_QUERY_MCU_STATE, (guint8 *) &payload,
                        sizeof (payload), NULL, TRUE, GOODIX_TIMEOUT, TRUE, NULL,
                        NULL);
}

void
goodix_send_request_tls_connection (FpDevice             *dev,
                                    GoodixDefaultCallback callback,
                                    gpointer              user_data)
{
  GoodixNone payload = {};
  GoodixCallbackInfo *cb_info;

  if (callback)
    {
      cb_info = malloc (sizeof (GoodixCallbackInfo));

      cb_info->callback = G_CALLBACK (callback);
      cb_info->user_data = user_data;

      /* Was 0: no ClientHello meant waiting forever. */
      goodix_send_protocol (dev, GOODIX_CMD_REQUEST_TLS_CONNECTION,
                            (guint8 *) &payload, sizeof (payload), NULL, TRUE,
                            GOODIX_TLS_HANDSHAKE_TIMEOUT_MS, TRUE,
                            goodix_receive_default, cb_info);
      return;
    }

  goodix_send_protocol (dev, GOODIX_CMD_REQUEST_TLS_CONNECTION,
                        (guint8 *) &payload, sizeof (payload), NULL, TRUE,
                        GOODIX_TIMEOUT, TRUE, NULL, NULL);
}

void
goodix_send_tls_successfully_established (FpDevice          *dev,
                                          GoodixNoneCallback callback,
                                          gpointer           user_data)
{
  GoodixNone payload = {};
  GoodixCallbackInfo *cb_info;

  if (callback)
    {
      cb_info = malloc (sizeof (GoodixCallbackInfo));

      cb_info->callback = G_CALLBACK (callback);
      cb_info->user_data = user_data;

      goodix_send_protocol (dev, GOODIX_CMD_TLS_SUCCESSFULLY_ESTABLISHED,
                            (guint8 *) &payload, sizeof (payload), NULL, TRUE,
                            GOODIX_TIMEOUT, FALSE, goodix_receive_none, cb_info);
      return;
    }

  goodix_send_protocol (dev, GOODIX_CMD_TLS_SUCCESSFULLY_ESTABLISHED,
                        (guint8 *) &payload, sizeof (payload), NULL, TRUE,
                        GOODIX_TIMEOUT, FALSE, NULL, NULL);
}

void
goodix_send_read_otp (FpDevice *dev, GoodixDefaultCallback callback,
                      gpointer user_data)
{
  GoodixNone payload = {};
  GoodixCallbackInfo *cb_info;

  if (callback)
    {
      cb_info = malloc (sizeof (GoodixCallbackInfo));

      cb_info->callback = G_CALLBACK (callback);
      cb_info->user_data = user_data;

      goodix_send_protocol (dev, GOODIX_CMD_READ_OTP, (guint8 *) &payload,
                            sizeof (payload), NULL, TRUE, GOODIX_TIMEOUT, TRUE,
                            goodix_receive_default, cb_info);
      return;
    }

  goodix_send_protocol (dev, GOODIX_CMD_READ_OTP, (guint8 *) &payload,
                        sizeof (payload), NULL, TRUE, GOODIX_TIMEOUT, TRUE,
                        NULL, NULL);
}

void
goodix_send_preset_psk_write (FpDevice *dev, guint32 flags, guint8 *psk,
                              guint16 length, GDestroyNotify free_func,
                              GoodixSuccessCallback callback,
                              gpointer user_data)
{
  // Only support one flags, one payload and one length

  guint8 *payload = g_malloc (sizeof (GoodixPresetPsk) + length);
  GoodixPresetPsk *preset_psk = (GoodixPresetPsk *) payload;
  GoodixCallbackInfo *cb_info;

  preset_psk->flags = GUINT32_TO_LE (flags);
  preset_psk->length = GUINT32_TO_LE (length);
  memcpy (payload + sizeof (GoodixPresetPsk), psk, length);
  if (free_func)
    free_func (psk);

  if (callback)
    {
      cb_info = malloc (sizeof (GoodixCallbackInfo));

      cb_info->callback = G_CALLBACK (callback);
      cb_info->user_data = user_data;

      goodix_send_protocol (dev, GOODIX_CMD_PRESET_PSK_WRITE, payload,
                            sizeof (payload) + length, g_free, TRUE, GOODIX_TIMEOUT,
                            TRUE, goodix_receive_preset_psk_write, cb_info);
      return;
    }

  goodix_send_protocol (dev, GOODIX_CMD_PRESET_PSK_WRITE, payload,
                        sizeof (payload) + length, g_free, TRUE, GOODIX_TIMEOUT,
                        TRUE, NULL, NULL);
}

void goodix_send_preset_psk_read(FpDevice *dev, guint32 flags, guint16 length,
                                 GoodixPresetPskReadCallback callback,
                                 gpointer user_data) {
  GoodixPresetPsk payload = {.flags = GUINT32_TO_LE(flags),
                             .length = GUINT32_TO_LE(length)};
  GoodixCallbackInfo *cb_info;

  if (callback)
    {
      cb_info = malloc (sizeof (GoodixCallbackInfo));

      cb_info->callback = G_CALLBACK (callback);
      cb_info->user_data = user_data;

      goodix_send_protocol (dev, GOODIX_CMD_PRESET_PSK_READ, (guint8 *) &payload,
                            sizeof (payload), NULL, TRUE, GOODIX_TIMEOUT, TRUE,
                            goodix_receive_preset_psk_read, cb_info);
      return;
    }

  goodix_send_protocol (dev, GOODIX_CMD_PRESET_PSK_READ, (guint8 *) &payload,
                        sizeof (payload), NULL, TRUE, GOODIX_TIMEOUT, TRUE, NULL,
                        NULL);
}

// ---- GOODIX SEND SECTION END ----

// -----------------------------------------------------------------------------

// ---- DEV SECTION START ----

gboolean
goodix_dev_init (FpDevice *dev, GError **error)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsClass *class = FPI_DEVICE_GOODIXTLS_GET_CLASS (self);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);

  priv->timeout = NULL;
  priv->ack = FALSE;
  priv->reply = FALSE;
  priv->callback = NULL;
  priv->user_data = NULL;
  priv->user_data_destroy = NULL;
  priv->rx_errors = 0;
  priv->rx_stopped = FALSE;
  if (!priv->reader.buffer)
    goodix_pack_reader_init (&priv->reader);
  goodix_pack_reader_reset (&priv->reader);
  /* Packs with a bad header checksum are dropped (all of 70+ logged
   * replies were valid); GOODIX55B4_LAX_CHECKSUM=1 restores the old
   * accept-and-warn behaviour should some firmware not set it. */
  priv->reader.lax_checksum =
    g_strcmp0 (g_getenv ("GOODIX55B4_LAX_CHECKSUM"), "1") == 0;
  if (priv->reader.lax_checksum)
    fp_warn ("GOODIX55B4_LAX_CHECKSUM=1: accepting packs with bad header "
             "checksums");
  /* Was replaced without unref on every open. */
  g_clear_object (&priv->transfer_cancel_tkn);
  priv->transfer_cancel_tkn = g_cancellable_new ();

  return g_usb_device_claim_interface (fpi_device_get_usb_device (dev),
                                       class->interface, 0, error);
}
void
goodix_reset_state (FpDevice *dev)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);

  GDestroyNotify destroy = priv->user_data_destroy;
  gpointer user_data = priv->user_data;

  if (priv->timeout)
    g_clear_pointer (&priv->timeout, g_source_destroy);
  priv->ack = FALSE;
  priv->reply = FALSE;
  priv->callback = NULL;
  priv->user_data = NULL;
  priv->user_data_destroy = NULL;
  /* A dropped command's GoodixCallbackInfo was leaked. */
  if (destroy && user_data)
    destroy (user_data);
}

void
goodix_abort_command (FpDevice *dev)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);

  if (priv->ack || priv->reply)
    fp_dbg ("Aborting command: 0x%02x", priv->cmd);
  goodix_reset_state (dev);
  goodix_pack_reader_reset (&priv->reader);
}

void
goodix_cancel_receive(FpDevice *dev)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);
  g_cancellable_cancel (priv->transfer_cancel_tkn);
}

gboolean
goodix_dev_deinit (FpDevice *dev, GError **error)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsClass *class = FPI_DEVICE_GOODIXTLS_GET_CLASS (self);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);

  if (priv->timeout)
    g_clear_pointer (&priv->timeout, g_source_destroy);
  g_clear_pointer (&priv->rx_retry, g_source_destroy);
  goodix_pack_reader_reset (&priv->reader);
  g_cancellable_cancel (priv->transfer_cancel_tkn);
  goodix_shutdown_tls (dev, error);

  goodix_reset_state (dev);
  priv->inited = FALSE;

  return g_usb_device_release_interface (fpi_device_get_usb_device (dev),
                                         class->interface, 0, error);
}

// ---- DEV SECTION END ----

// -----------------------------------------------------------------------------

// ---- TLS SECTION START ----

void
goodix_read_tls (FpDevice *dev, GoodixTlsCallback callback,
                 gpointer user_data)
{

  fp_dbg ("goodix_read_tls()");
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);
  priv->callback = callback;
  priv->user_data = user_data;
  priv->user_data_destroy = NULL;
  priv->reply = TRUE;
  priv->cmd = 0;
}

/* The handshake is event driven: every TLS pack from the device is fed to
 * the engine, whatever the engine produces is sent back, and the next pack
 * is awaited (with a timeout) until the engine reports the session up. The
 * device sends ClientHello (reply to 0xd0), then ClientKeyExchange,
 * ChangeCipherSpec and Finished in however many packs it likes. */

static void tls_handshake_feed (FpDevice     *dev,
                                const guint8 *data,
                                guint16       length);

/* Reports the handshake result exactly once. */
static void
tls_handshake_finish (FpDevice *dev, GError *error)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);
  GoodixNoneCallback callback = priv->tls_callback;
  gpointer user_data = priv->tls_user_data;

  priv->tls_callback = NULL;
  priv->tls_user_data = NULL;
  if (error)
    {
      fp_warn ("TLS handshake failed: %s", error->message);
      g_clear_pointer (&priv->tls_hop, goodix_tls_server_free);
    }
  else
    {
      fp_dbg ("TLS connection established");
    }

  if (callback)
    callback (dev, user_data, error);
  else
    g_clear_error (&error);
}

static void
on_tls_successfully_established (FpDevice *dev, gpointer user_data,
                                 GError *error)
{
  tls_handshake_finish (dev, error);
}

static void
on_tls_handshake_read (FpDevice *dev, guint8 *data, guint16 length,
                       gpointer user_data, GError *error)
{
  if (error)
    {
      tls_handshake_finish (dev, error);
      return;
    }
  tls_handshake_feed (dev, data, length);
}

static void
tls_handshake_read_next (FpDevice *dev)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);

  goodix_read_tls (dev, on_tls_handshake_read, NULL);
  g_clear_pointer (&priv->timeout, g_source_destroy);
  priv->timeout = fpi_device_add_timeout (dev, GOODIX_TLS_HANDSHAKE_TIMEOUT_MS,
                                          goodix_receive_timeout_cb, NULL,
                                          NULL);
  /* count from now, not from the main loop's cached time */
  g_source_set_ready_time (priv->timeout,
                           g_get_monotonic_time () +
                           GOODIX_TLS_HANDSHAKE_TIMEOUT_MS * G_GINT64_CONSTANT (1000));
}

static void
tls_handshake_feed (FpDevice *dev, const guint8 *data, guint16 length)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);
  g_autoptr(GBytes) output = NULL;
  GError *error = NULL;
  gboolean established = FALSE;
  gboolean ok;

  if (!priv->tls_hop)
    {
      tls_handshake_finish (dev, fpi_device_error_new_msg (
                              FP_DEVICE_ERROR_PROTO, "No TLS session"));
      return;
    }

  ok = goodix_tls_server_handshake (priv->tls_hop, data, length, &output,
                                    &established, &error);
  if (output)
    {
      gsize size = 0;
      const guint8 *bytes = g_bytes_get_data (output, &size);
      GError *send_error = NULL;

      fp_dbg ("TLS: %u bytes from the device, sending %zu", length, size);
      if (size > G_MAXUINT16)
        send_error = fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                               "TLS flight too long (%zu bytes)",
                                               size);
      else
        goodix_send_pack (dev, GOODIX_FLAGS_TLS, (guint8 *) bytes, size, NULL,
                          &send_error);
      if (send_error)
        {
          if (ok)
            {
              tls_handshake_finish (dev, send_error);
              return;
            }
          /* Only an alert was lost; the handshake error is the cause. */
          fp_warn ("Cannot send the TLS alert: %s", send_error->message);
          g_clear_error (&send_error);
        }
    }

  if (!ok)
    tls_handshake_finish (dev, error);
  else if (established)
    goodix_send_tls_successfully_established (dev,
                                              on_tls_successfully_established,
                                              NULL);
  else
    tls_handshake_read_next (dev);
}

static void
on_goodix_request_tls_connection (FpDevice *dev, guint8 *data,
                                  guint16 length, gpointer user_data,
                                  GError *error)
{
  if (error)
    {
      tls_handshake_finish (dev, error);
      return;
    }
  /* The reply to 0xd0 is the ClientHello. */
  tls_handshake_feed (dev, data, length);
}

void
goodix_tls (FpDevice *dev, GoodixNoneCallback callback, gpointer user_data)
{
  fp_dbg ("Starting up goodix tls server");
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);
  GError *error = NULL;

  g_return_if_fail (callback != NULL);

  if (priv->tls_hop || priv->tls_callback)
    {
      callback (dev, user_data, fpi_device_error_new_msg (
                  FP_DEVICE_ERROR_GENERAL, "TLS session already started"));
      return;
    }
  priv->tls_hop = goodix_tls_server_new (&error);
  if (!priv->tls_hop)
    {
      fp_err ("failed to init tls server: %s", error->message);
      callback (dev, user_data, error);
      return;
    }
  priv->tls_callback = callback;
  priv->tls_user_data = user_data;

  goodix_send_request_tls_connection (dev, on_goodix_request_tls_connection,
                                      NULL);
}

gboolean
goodix_shutdown_tls (FpDevice *dev, GError **error)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);

  /* Synchronous now: no thread can still be using the session. */
  g_clear_pointer (&priv->tls_hop, goodix_tls_server_free);
  priv->tls_callback = NULL;
  priv->tls_user_data = NULL;
  return TRUE;
}

static void
goodix_tls_ready_image_handler (FpDevice *dev, guint8 *data,
                                guint16 length, gpointer user_data,
                                GError *error)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);
  g_autofree GoodixCallbackInfo *cb_info = user_data;
  GoodixImageCallback callback = (GoodixImageCallback) cb_info->callback;
  g_autoptr(GBytes) plain = NULL;
  const guint8 *bytes;
  gsize size = 0;

  if (error)
    {
      callback (dev, NULL, 0, cb_info->user_data, error);
      return;
    }
  if (!priv->tls_hop)
    {
      callback (dev, NULL, 0, cb_info->user_data,
                fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                          "Image received without a TLS session"));
      return;
    }

  plain = goodix_tls_server_decrypt (priv->tls_hop, data, length, &error);
  if (!plain)
    {
      callback (dev, NULL, 0, cb_info->user_data, error);
      return;
    }
  bytes = g_bytes_get_data (plain, &size);
  if (size > G_MAXUINT16)
    {
      callback (dev, NULL, 0, cb_info->user_data,
                fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                          "Image too long (%zu bytes)", size));
      return;
    }
  /* The callback copies what it keeps; plain is freed on return. */
  callback (dev, (guint8 *) bytes, size, cb_info->user_data, NULL);
}

void
goodix_tls_read_image (FpDevice *dev, GoodixImageCallback callback,
                       gpointer user_data)
{
  g_assert (callback);
  GoodixCallbackInfo *cb_info = g_new (GoodixCallbackInfo, 1);

  cb_info->callback = G_CALLBACK (callback);
  cb_info->user_data = user_data;

  goodix_send_mcu_get_image (dev, goodix_tls_ready_image_handler, cb_info);
}

// ---- TLS SECTION END ----

static void
fpi_device_goodixtls_init (FpiDeviceGoodixTls *self)
{
}

static void
fpi_device_goodixtls_finalize (GObject *object)
{
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (FPI_DEVICE_GOODIXTLS (object));

  goodix_pack_reader_clear (&priv->reader);
  g_clear_object (&priv->transfer_cancel_tkn);
  g_clear_pointer (&priv->tls_hop, goodix_tls_server_free);
  G_OBJECT_CLASS (fpi_device_goodixtls_parent_class)->finalize (object);
}

static void
fpi_device_goodixtls_class_init (FpiDeviceGoodixTlsClass *class)
{
  G_OBJECT_CLASS (class)->finalize = fpi_device_goodixtls_finalize;
}
