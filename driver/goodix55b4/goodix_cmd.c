// Goodix Tls driver for libfprint: protocol command wrappers

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

/* Each goodix_send_<command> builds its payload and hands it to
 * goodix_send_command(). They only depend on goodix_send_protocol(), so
 * tests/test-goodix-cmd.c links this file against a recording stub of it
 * and pins every command's bytes, timeout, reply flag and adapter. */

#define FP_COMPONENT "goodixtls"

#include <string.h>

#include "drivers_api.h"
#include "goodix.h"
#include "goodix_proto.h"

/* Sends cmd with a checksum (every command here has one). A non-NULL
 * callback is packed with user_data into a GoodixCallbackInfo for adapter,
 * the goodix_receive_* function that decodes the reply for that callback
 * type and frees the GoodixCallbackInfo. goodix_send_protocol() calls the
 * adapter in every case, also when the command cannot be sent, so the
 * info never leaks. Without a callback the reply is dropped. The payload is
 * freed with free_func (if any) once encoded. */
static void
goodix_send_command (FpDevice         *dev,
                     guint8            cmd,
                     guint8           *payload,
                     guint16           length,
                     GDestroyNotify    free_func,
                     guint             timeout_ms,
                     gboolean          reply,
                     GoodixCmdCallback adapter,
                     GCallback         callback,
                     gpointer          user_data)
{
  GoodixCallbackInfo *cb_info = NULL;

  if (callback)
    {
      cb_info = g_new (GoodixCallbackInfo, 1);
      cb_info->callback = callback;
      cb_info->user_data = user_data;
    }

  goodix_send_protocol (dev, cmd, payload, length, free_func, TRUE,
                        timeout_ms, reply, cb_info ? adapter : NULL, cb_info);
}

void
goodix_send_mcu_get_image (FpDevice *dev, GoodixImageCallback callback,
                           gpointer user_data)
{
  GoodixDefault payload = {.unused_flags = 0x01};

  goodix_send_command (dev, GOODIX_CMD_MCU_GET_IMAGE, (guint8 *) &payload,
                       sizeof (payload), NULL, GOODIX_TIMEOUT, TRUE,
                       goodix_receive_default, G_CALLBACK (callback),
                       user_data);
}

void
goodix_send_mcu_switch_to_fdt_down (FpDevice *dev, guint8 *mode,
                                    guint16 length, GDestroyNotify free_func,
                                    GoodixDefaultCallback callback,
                                    gpointer user_data)
{
  goodix_send_command (dev, GOODIX_CMD_MCU_SWITCH_TO_FDT_DOWN, mode, length,
                       free_func, 0, TRUE, goodix_receive_default,
                       G_CALLBACK (callback), user_data);
}

void
goodix_send_mcu_switch_to_fdt_up (FpDevice *dev, guint8 *mode,
                                  guint16 length, GDestroyNotify free_func,
                                  GoodixDefaultCallback callback,
                                  gpointer user_data)
{
  goodix_send_command (dev, GOODIX_CMD_MCU_SWITCH_TO_FDT_UP, mode, length,
                       free_func, 0, TRUE, goodix_receive_default,
                       G_CALLBACK (callback), user_data);
}

void
goodix_send_mcu_switch_to_fdt_up_no_reply (FpDevice *dev, guint8 *mode,
                                           guint16 length,
                                           GDestroyNotify free_func,
                                           GoodixDefaultCallback callback,
                                           gpointer user_data)
{
  goodix_send_command (dev, GOODIX_CMD_MCU_SWITCH_TO_FDT_UP, mode, length,
                       free_func, 0, FALSE, goodix_receive_default,
                       G_CALLBACK (callback), user_data);
}

void
goodix_send_mcu_switch_to_fdt_mode (FpDevice *dev, guint8 *mode,
                                    guint16 length, GDestroyNotify free_func,
                                    GoodixDefaultCallback callback,
                                    gpointer user_data)
{
  goodix_send_command (dev, GOODIX_CMD_MCU_SWITCH_TO_FDT_MODE, mode, length,
                       free_func, 0, TRUE, goodix_receive_default,
                       G_CALLBACK (callback), user_data);
}

void
goodix_send_nav_0 (FpDevice *dev, GoodixDefaultCallback callback,
                   gpointer user_data)
{
  GoodixDefault payload = {.unused_flags = 0x01};

  goodix_send_command (dev, GOODIX_CMD_NAV_0, (guint8 *) &payload,
                       sizeof (payload), NULL, GOODIX_TIMEOUT, TRUE,
                       goodix_receive_default, G_CALLBACK (callback),
                       user_data);
}

void
goodix_send_mcu_switch_to_idle_mode (FpDevice *dev, guint8 sleep_time,
                                     GoodixNoneCallback callback,
                                     gpointer user_data)
{
  GoodixMcuSwitchToIdleMode payload = {.sleep_time = sleep_time};

  goodix_send_command (dev, GOODIX_CMD_MCU_SWITCH_TO_IDLE_MODE,
                       (guint8 *) &payload, sizeof (payload), NULL,
                       GOODIX_TIMEOUT, FALSE, goodix_receive_none,
                       G_CALLBACK (callback), user_data);
}

void
goodix_send_mcu_switch_to_sleep_mode (FpDevice *dev, guint8 sleep_time,
                                      GoodixNoneCallback callback,
                                      gpointer user_data)
{
  GoodixMcuSwitchToIdleMode payload = {.sleep_time = sleep_time};

  goodix_send_command (dev, GOODIX_CMD_MCU_SWITCH_TO_SLEEP_MODE,
                       (guint8 *) &payload, sizeof (payload), NULL,
                       GOODIX_TIMEOUT, FALSE, goodix_receive_none,
                       G_CALLBACK (callback), user_data);
}

void
goodix_send_mcu_switch_to_sleep_mode_realtek (FpDevice *dev, guint8 value,
                                              GoodixSuccessCallback callback,
                                              gpointer user_data)
{
  GoodixMcuSwitchToSleepModeRealtek payload = {.value = value};

  goodix_send_command (dev, GOODIX_CMD_MCU_SWITCH_TO_SLEEP_MODE_REALTEK,
                       (guint8 *) &payload, sizeof (payload), NULL,
                       GOODIX_TIMEOUT, TRUE, goodix_receive_success,
                       G_CALLBACK (callback), user_data);
}

void
goodix_send_write_sensor_register (FpDevice *dev, guint16 address,
                                   guint16 value, GoodixNoneCallback callback,
                                   gpointer user_data)
{
  /* One address and one value only. Was hardcoded to register 556 with a
   * broken initializer (wrote 0x05), ignoring the address and value. */
  GoodixWriteSensorRegister payload = {.multiples = FALSE,
                                       .address = GUINT16_TO_LE (address),
                                       .value = GUINT16_TO_LE (value)};

  goodix_send_command (dev, GOODIX_CMD_WRITE_SENSOR_REGISTER,
                       (guint8 *) &payload, sizeof (payload), NULL,
                       GOODIX_TIMEOUT, FALSE, goodix_receive_none,
                       G_CALLBACK (callback), user_data);
}

void
goodix_send_read_sensor_register (FpDevice *dev, guint16 address,
                                  guint8 length,
                                  GoodixDefaultCallback callback,
                                  gpointer user_data)
{
  /* One address only. */
  GoodixReadSensorRegister payload = {
    .multiples = FALSE, .address = GUINT16_TO_LE (address), .length = length
  };

  goodix_send_command (dev, GOODIX_CMD_READ_SENSOR_REGISTER,
                       (guint8 *) &payload, sizeof (payload), NULL,
                       GOODIX_TIMEOUT, TRUE, goodix_receive_default,
                       G_CALLBACK (callback), user_data);
}

void
goodix_send_upload_config_mcu (FpDevice *dev, guint8 *config, guint16 length,
                               GDestroyNotify free_func,
                               GoodixSuccessCallback callback,
                               gpointer user_data)
{
  goodix_send_command (dev, GOODIX_CMD_UPLOAD_CONFIG_MCU, config, length,
                       free_func, GOODIX_TIMEOUT, TRUE, goodix_receive_success,
                       G_CALLBACK (callback), user_data);
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

  goodix_send_command (dev, GOODIX_CMD_SET_POWERDOWN_SCAN_FREQUENCY,
                       (guint8 *) &payload, sizeof (payload), NULL,
                       GOODIX_TIMEOUT, TRUE, goodix_receive_success,
                       G_CALLBACK (callback), user_data);
}

void
goodix_send_enable_chip (FpDevice *dev, gboolean enable,
                         GoodixNoneCallback callback, gpointer user_data)
{
  GoodixEnableChip payload = {.enable = enable ? TRUE : FALSE};

  goodix_send_command (dev, GOODIX_CMD_ENABLE_CHIP, (guint8 *) &payload,
                       sizeof (payload), NULL, GOODIX_TIMEOUT, FALSE,
                       goodix_receive_none, G_CALLBACK (callback), user_data);
}

void
goodix_send_reset (FpDevice *dev, gboolean reset_sensor, guint8 sleep_time,
                   GoodixResetCallback callback, gpointer user_data)
{
  /* Sensor reset only. */
  GoodixReset payload = {.soft_reset_mcu = FALSE,
                         .reset_sensor = reset_sensor ? TRUE : FALSE,
                         .other = 1,
                         .sleep_time = sleep_time};

  goodix_send_command (dev, GOODIX_CMD_RESET, (guint8 *) &payload,
                       sizeof (payload), NULL, GOODIX_TIMEOUT, TRUE,
                       goodix_receive_reset, G_CALLBACK (callback), user_data);
}

void
goodix_send_firmware_version (FpDevice                     *dev,
                              GoodixFirmwareVersionCallback callback,
                              gpointer                      user_data)
{
  GoodixNone payload = {};

  goodix_send_command (dev, GOODIX_CMD_FIRMWARE_VERSION, (guint8 *) &payload,
                       sizeof (payload), NULL, GOODIX_TIMEOUT, TRUE,
                       goodix_receive_firmware_version, G_CALLBACK (callback),
                       user_data);
}

void
goodix_send_query_mcu_state (FpDevice *dev, GoodixDefaultCallback callback,
                             gpointer user_data)
{
  GoodixQueryMcuState payload = {.unused_flags = 0x55};

  goodix_send_command (dev, GOODIX_CMD_QUERY_MCU_STATE, (guint8 *) &payload,
                       sizeof (payload), NULL, GOODIX_TIMEOUT, TRUE,
                       goodix_receive_default, G_CALLBACK (callback),
                       user_data);
}

void
goodix_send_request_tls_connection (FpDevice             *dev,
                                    GoodixDefaultCallback callback,
                                    gpointer              user_data)
{
  GoodixNone payload = {};

  /* Was 0: no ClientHello meant waiting forever. (Without a callback it
   * used GOODIX_TIMEOUT; nothing sends it without one.) */
  goodix_send_command (dev, GOODIX_CMD_REQUEST_TLS_CONNECTION,
                       (guint8 *) &payload, sizeof (payload), NULL,
                       GOODIX_TLS_HANDSHAKE_TIMEOUT_MS, TRUE,
                       goodix_receive_default, G_CALLBACK (callback),
                       user_data);
}

void
goodix_send_tls_successfully_established (FpDevice          *dev,
                                          GoodixNoneCallback callback,
                                          gpointer           user_data)
{
  GoodixNone payload = {};

  goodix_send_command (dev, GOODIX_CMD_TLS_SUCCESSFULLY_ESTABLISHED,
                       (guint8 *) &payload, sizeof (payload), NULL,
                       GOODIX_TIMEOUT, FALSE, goodix_receive_none,
                       G_CALLBACK (callback), user_data);
}

void
goodix_send_read_otp (FpDevice *dev, GoodixDefaultCallback callback,
                      gpointer user_data)
{
  GoodixNone payload = {};

  goodix_send_command (dev, GOODIX_CMD_READ_OTP, (guint8 *) &payload,
                       sizeof (payload), NULL, GOODIX_TIMEOUT, TRUE,
                       goodix_receive_default, G_CALLBACK (callback),
                       user_data);
}

void
goodix_send_preset_psk_write (FpDevice *dev, guint32 flags, guint8 *psk,
                              guint16 length, GDestroyNotify free_func,
                              GoodixSuccessCallback callback,
                              gpointer user_data)
{
  /* One flags, one payload and one length only. The frame is flags,
   * length, then the data, as tools/capture_dataset_55b4.py writes it. Was
   * laid out with GoodixPresetPsk (12 bytes, with an offset field) but sent
   * sizeof (pointer) + length bytes: 4 uninitialized bytes after the
   * header and the data's last 4 bytes cut off. */
  const gsize header_len = sizeof (GoodixPresetPskWrite);
  GoodixPresetPskWrite header = {.flags = GUINT32_TO_LE (flags),
                                 .length = GUINT32_TO_LE (length)};
  guint8 *payload;

  /* The protocol length field is 16 bits and also counts the checksum. */
  if (length > G_MAXUINT16 - header_len - sizeof (guint8))
    {
      GError *error = g_error_new (G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                   "Preset PSK write: %u bytes do not fit "
                                   "one command", length);

      fp_warn ("%s", error->message);
      if (free_func)
        free_func (psk);
      if (callback)
        callback (dev, FALSE, user_data, error);
      else
        g_error_free (error);
      return;
    }

  payload = g_malloc (header_len + length);
  memcpy (payload, &header, header_len);
  memcpy (payload + header_len, psk, length);
  if (free_func)
    free_func (psk);

  goodix_send_command (dev, GOODIX_CMD_PRESET_PSK_WRITE, payload,
                       header_len + length, g_free, GOODIX_TIMEOUT, TRUE,
                       goodix_receive_preset_psk_write, G_CALLBACK (callback),
                       user_data);
}

void
goodix_send_preset_psk_read (FpDevice *dev, guint32 flags, guint16 length,
                             GoodixPresetPskReadCallback callback,
                             gpointer user_data)
{
  GoodixPresetPsk payload = {.flags = GUINT32_TO_LE (flags),
                             .length = GUINT32_TO_LE (length)};

  goodix_send_command (dev, GOODIX_CMD_PRESET_PSK_READ, (guint8 *) &payload,
                       sizeof (payload), NULL, GOODIX_TIMEOUT, TRUE,
                       goodix_receive_preset_psk_read, G_CALLBACK (callback),
                       user_data);
}
