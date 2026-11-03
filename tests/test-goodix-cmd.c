/*
 * Tests for the command wrappers in goodix_cmd.c: each goodix_send_<cmd>
 * must hand goodix_send_protocol() the command id, payload bytes, timeout,
 * reply flag and reply adapter the device expects, with or without a
 * callback. Linked against goodix_cmd.c alone; goodix_send_protocol() and
 * the goodix_receive_* adapters are stubs here that record their use.
 *
 * Expected payloads come from the driver as it ran on hardware and, where
 * the capture tool sends the same command, from
 * tools/capture_dataset_55b4.py (e.g. reset "0514", chip id read
 * "0000000400", PSK write "020001bb0e000000...").
 */

#include <glib.h>
#include <gio/gio.h>
#include <string.h>

#include "goodix.h"
#include "goodix_proto.h"

/* ---- stubs ---- */

#define STUB_ADAPTER(name)                                                   \
  void name (FpDevice *dev, guint8 *data, guint16 length, gpointer user_data, \
             GError *error)                                                  \
  {                                                                          \
    g_assert_not_reached ();                                                 \
  }

STUB_ADAPTER (goodix_receive_none)
STUB_ADAPTER (goodix_receive_default)
STUB_ADAPTER (goodix_receive_success)
STUB_ADAPTER (goodix_receive_reset)
STUB_ADAPTER (goodix_receive_preset_psk_read)
STUB_ADAPTER (goodix_receive_preset_psk_write)
STUB_ADAPTER (goodix_receive_firmware_version)

typedef struct
{
  guint             calls;
  guint8            cmd;
  GByteArray       *payload;
  GDestroyNotify    free_func;
  gboolean          calc_checksum;
  guint             timeout_ms;
  gboolean          reply;
  GoodixCmdCallback adapter;
  gboolean          has_info;
  GoodixCallbackInfo info;
} Sent;

static Sent sent;

static void
sent_reset (void)
{
  if (sent.payload)
    g_byte_array_unref (sent.payload);
  memset (&sent, 0, sizeof (sent));
}

/* Records the call, then does with payload and user_data what the real one
 * does on success: frees the payload with free_func once encoded, and
 * leaves user_data to the adapter (freed here, as the adapter would). */
void
goodix_send_protocol (FpDevice *dev, guint8 cmd, guint8 *payload,
                      guint16 length, GDestroyNotify free_func,
                      gboolean calc_checksum, guint timeout_ms, gboolean reply,
                      GoodixCmdCallback callback, gpointer user_data)
{
  sent.calls++;
  sent.cmd = cmd;
  sent.payload = g_byte_array_new ();
  g_byte_array_append (sent.payload, payload, length);
  sent.free_func = free_func;
  sent.calc_checksum = calc_checksum;
  sent.timeout_ms = timeout_ms;
  sent.reply = reply;
  sent.adapter = callback;
  if (free_func)
    free_func (payload);
  if (callback)
    {
      sent.has_info = TRUE;
      sent.info = *(GoodixCallbackInfo *) user_data;
      g_free (user_data);
    }
  else
    {
      g_assert_null (user_data);
    }
}

/* ---- per-command senders ---- */

/* Owned, freed by the wrapper through free_func (g_free). */
static guint8 *
dup_hex (const gchar *hex, guint16 *len)
{
  gsize n = strlen (hex) / 2;
  guint8 *out = g_malloc (n);

  for (gsize i = 0; i < n; i++)
    out[i] = (guint8) g_ascii_strtoull ((gchar[]){ hex[2 * i], hex[2 * i + 1], 0 },
                                        NULL, 16);
  *len = (guint16) n;
  return out;
}

#define MODE_HEX "0d011c0115011e0114011e0116011b0117011b01180120"
#define CONFIG_HEX "701160712c9d2c"
#define PSK_HEX "4141414142424242434343434444"

typedef void (*Sender) (GCallback cb, gpointer ud);

#define SEND_MODE(fn)                                                        \
  static void send_##fn (GCallback cb, gpointer ud)                          \
  {                                                                          \
    guint16 len;                                                             \
    guint8 *mode = dup_hex (MODE_HEX, &len);                                 \
    goodix_send_##fn (NULL, mode, len, g_free,                               \
                      (GoodixDefaultCallback) cb, ud);                       \
  }

SEND_MODE (mcu_switch_to_fdt_down)
SEND_MODE (mcu_switch_to_fdt_up)
SEND_MODE (mcu_switch_to_fdt_up_no_reply)
SEND_MODE (mcu_switch_to_fdt_mode)

static void
send_mcu_get_image (GCallback cb, gpointer ud)
{
  goodix_send_mcu_get_image (NULL, (GoodixImageCallback) cb, ud);
}

static void
send_nav_0 (GCallback cb, gpointer ud)
{
  goodix_send_nav_0 (NULL, (GoodixDefaultCallback) cb, ud);
}

static void
send_idle (GCallback cb, gpointer ud)
{
  goodix_send_mcu_switch_to_idle_mode (NULL, 0x14, (GoodixNoneCallback) cb, ud);
}

static void
send_sleep (GCallback cb, gpointer ud)
{
  goodix_send_mcu_switch_to_sleep_mode (NULL, 0x14, (GoodixNoneCallback) cb, ud);
}

static void
send_sleep_realtek (GCallback cb, gpointer ud)
{
  goodix_send_mcu_switch_to_sleep_mode_realtek (NULL, 0x5a,
                                                (GoodixSuccessCallback) cb, ud);
}

static void
send_write_register (GCallback cb, gpointer ud)
{
  goodix_send_write_sensor_register (NULL, 0x0102, 0x0304,
                                     (GoodixNoneCallback) cb, ud);
}

static void
send_read_register (GCallback cb, gpointer ud)
{
  goodix_send_read_sensor_register (NULL, 0x0000, 4,
                                    (GoodixDefaultCallback) cb, ud);
}

static void
send_upload_config (GCallback cb, gpointer ud)
{
  guint16 len;
  guint8 *config = dup_hex (CONFIG_HEX, &len);

  goodix_send_upload_config_mcu (NULL, config, len, g_free,
                                 (GoodixSuccessCallback) cb, ud);
}

static void
send_powerdown (GCallback cb, gpointer ud)
{
  goodix_send_set_powerdown_scan_frequency (NULL, 0x0064,
                                            (GoodixSuccessCallback) cb, ud);
}

static void
send_enable_chip (GCallback cb, gpointer ud)
{
  goodix_send_enable_chip (NULL, TRUE, (GoodixNoneCallback) cb, ud);
}

static void
send_reset (GCallback cb, gpointer ud)
{
  goodix_send_reset (NULL, TRUE, 20, (GoodixResetCallback) cb, ud);
}

static void
send_firmware_version (GCallback cb, gpointer ud)
{
  goodix_send_firmware_version (NULL, (GoodixFirmwareVersionCallback) cb, ud);
}

static void
send_query_mcu_state (GCallback cb, gpointer ud)
{
  goodix_send_query_mcu_state (NULL, (GoodixDefaultCallback) cb, ud);
}

static void
send_request_tls (GCallback cb, gpointer ud)
{
  goodix_send_request_tls_connection (NULL, (GoodixDefaultCallback) cb, ud);
}

static void
send_tls_established (GCallback cb, gpointer ud)
{
  goodix_send_tls_successfully_established (NULL, (GoodixNoneCallback) cb, ud);
}

static void
send_read_otp (GCallback cb, gpointer ud)
{
  goodix_send_read_otp (NULL, (GoodixDefaultCallback) cb, ud);
}

static void
send_psk_write (GCallback cb, gpointer ud)
{
  guint16 len;
  guint8 *psk = dup_hex (PSK_HEX, &len);

  goodix_send_preset_psk_write (NULL, 0xbb010002, psk, len, g_free,
                                (GoodixSuccessCallback) cb, ud);
}

static void
send_psk_read (GCallback cb, gpointer ud)
{
  goodix_send_preset_psk_read (NULL, 0xbb020007, 32,
                               (GoodixPresetPskReadCallback) cb, ud);
}

/* ---- expectations ---- */

typedef struct
{
  const gchar      *name;
  Sender            send;
  guint8            cmd;
  const gchar      *payload_hex;
  gboolean          frees_payload; /* wrapper passes free_func on */
  guint             timeout_ms;
  gboolean          reply;
  GoodixCmdCallback adapter;
} Case;

static const Case cases[] = {
  { "mcu-get-image", send_mcu_get_image, 0x20, "0100", FALSE,
    GOODIX_TIMEOUT, TRUE, goodix_receive_default },
  { "fdt-down", send_mcu_switch_to_fdt_down, 0x32, MODE_HEX, TRUE, 0, TRUE,
    goodix_receive_default },
  { "fdt-up", send_mcu_switch_to_fdt_up, 0x34, MODE_HEX, TRUE, 0, TRUE,
    goodix_receive_default },
  { "fdt-up-no-reply", send_mcu_switch_to_fdt_up_no_reply, 0x34, MODE_HEX,
    TRUE, 0, FALSE, goodix_receive_default },
  { "fdt-mode", send_mcu_switch_to_fdt_mode, 0x36, MODE_HEX, TRUE, 0, TRUE,
    goodix_receive_default },
  { "nav-0", send_nav_0, 0x50, "0100", FALSE, GOODIX_TIMEOUT, TRUE,
    goodix_receive_default },
  { "idle-mode", send_idle, 0x70, "1400", FALSE, GOODIX_TIMEOUT, FALSE,
    goodix_receive_none },
  { "sleep-mode", send_sleep, 0x60, "1400", FALSE, GOODIX_TIMEOUT, FALSE,
    goodix_receive_none },
  { "sleep-mode-realtek", send_sleep_realtek, 0x92, "5a", FALSE,
    GOODIX_TIMEOUT, TRUE, goodix_receive_success },
  { "write-sensor-register", send_write_register, 0x80, "0002010403", FALSE,
    GOODIX_TIMEOUT, FALSE, goodix_receive_none },
  { "read-sensor-register", send_read_register, 0x82, "0000000400", FALSE,
    GOODIX_TIMEOUT, TRUE, goodix_receive_default },
  { "upload-config", send_upload_config, 0x90, CONFIG_HEX, TRUE,
    GOODIX_TIMEOUT, TRUE, goodix_receive_success },
  { "powerdown-scan-frequency", send_powerdown, 0x94, "6400", FALSE,
    GOODIX_TIMEOUT, TRUE, goodix_receive_success },
  { "enable-chip", send_enable_chip, 0x96, "0100", FALSE, GOODIX_TIMEOUT,
    FALSE, goodix_receive_none },
  { "reset", send_reset, 0xa2, "0514", FALSE, GOODIX_TIMEOUT, TRUE,
    goodix_receive_reset },
  { "read-otp", send_read_otp, 0xa6, "0000", FALSE, GOODIX_TIMEOUT, TRUE,
    goodix_receive_default },
  { "firmware-version", send_firmware_version, 0xa8, "0000", FALSE,
    GOODIX_TIMEOUT, TRUE, goodix_receive_firmware_version },
  { "query-mcu-state", send_query_mcu_state, 0xae, "55", FALSE,
    GOODIX_TIMEOUT, TRUE, goodix_receive_default },
  { "request-tls-connection", send_request_tls, 0xd0, "0000", FALSE,
    GOODIX_TLS_HANDSHAKE_TIMEOUT_MS, TRUE, goodix_receive_default },
  { "tls-successfully-established", send_tls_established, 0xd4, "0000",
    FALSE, GOODIX_TIMEOUT, FALSE, goodix_receive_none },
  { "preset-psk-write", send_psk_write, 0xe0, "020001bb0e000000" PSK_HEX,
    TRUE, GOODIX_TIMEOUT, TRUE, goodix_receive_preset_psk_write },
  { "preset-psk-read", send_psk_read, 0xe4, "070002bb2000000000000000",
    FALSE, GOODIX_TIMEOUT, TRUE, goodix_receive_preset_psk_read },
};

static void
user_callback (void)
{
}

static gchar *
hex_of (GByteArray *bytes)
{
  GString *s = g_string_new (NULL);

  for (guint i = 0; i < bytes->len; i++)
    g_string_append_printf (s, "%02x", bytes->data[i]);
  return g_string_free (s, FALSE);
}

static void
check_common (const Case *c)
{
  g_autofree gchar *hex = NULL;

  g_assert_cmpuint (sent.calls, ==, 1);
  g_assert_cmphex (sent.cmd, ==, c->cmd);
  hex = hex_of (sent.payload);
  g_assert_cmpstr (hex, ==, c->payload_hex);
  g_assert_true (sent.calc_checksum);
  g_assert_cmpuint (sent.timeout_ms, ==, c->timeout_ms);
  g_assert_cmpint (sent.reply, ==, c->reply);
  if (c->frees_payload)
    g_assert_true (sent.free_func == g_free);
  else
    g_assert_null (sent.free_func);
}

static void
test_with_callback (gconstpointer data)
{
  const Case *c = data;
  gint marker;

  sent_reset ();
  c->send (G_CALLBACK (user_callback), &marker);
  check_common (c);
  g_assert_true (sent.adapter == c->adapter);
  g_assert_true (sent.has_info);
  g_assert_true (sent.info.callback == G_CALLBACK (user_callback));
  g_assert_true (sent.info.user_data == &marker);
  sent_reset ();
}

static void
test_without_callback (gconstpointer data)
{
  const Case *c = data;

  sent_reset ();
  c->send (NULL, NULL);
  check_common (c);
  g_assert_null (sent.adapter);
  g_assert_false (sent.has_info);
  sent_reset ();
}

/* ---- PSK write bounds ---- */

static guint psk_cb_calls;
static gboolean psk_cb_success;
static GError *psk_cb_error;

static void
psk_write_cb (FpDevice *dev, gboolean success, gpointer user_data,
              GError *error)
{
  psk_cb_calls++;
  psk_cb_success = success;
  psk_cb_error = error;
}

static void
test_psk_write_too_long (void)
{
  /* Header (8) + data + checksum must fit the 16-bit length field. */
  const guint16 len = G_MAXUINT16 - sizeof (GoodixPresetPskWrite);
  guint8 *psk = g_malloc0 (len);

  sent_reset ();
  psk_cb_calls = 0;
  g_test_expect_message ("libfprint-goodixtls", G_LOG_LEVEL_WARNING,
                         "*do not fit*");
  goodix_send_preset_psk_write (NULL, 0xbb010003, psk, len, g_free,
                                psk_write_cb, NULL);
  g_test_assert_expected_messages ();
  g_assert_cmpuint (sent.calls, ==, 0);
  g_assert_cmpuint (psk_cb_calls, ==, 1);
  g_assert_false (psk_cb_success);
  g_assert_error (psk_cb_error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error (&psk_cb_error);

  /* Without a callback: logged, nothing sent, nothing leaked. */
  psk = g_malloc0 (len);
  g_test_expect_message ("libfprint-goodixtls", G_LOG_LEVEL_WARNING,
                         "*do not fit*");
  goodix_send_preset_psk_write (NULL, 0xbb010003, psk, len, g_free, NULL,
                                NULL);
  g_test_assert_expected_messages ();
  g_assert_cmpuint (sent.calls, ==, 0);
}

static void
test_psk_write_largest (void)
{
  /* The largest that fits is sent whole. */
  const guint16 len = G_MAXUINT16 - sizeof (GoodixPresetPskWrite) - 1;
  guint8 *psk = g_malloc (len);

  memset (psk, 0xa5, len);
  sent_reset ();
  goodix_send_preset_psk_write (NULL, 0xbb010003, psk, len, g_free, NULL,
                                NULL);
  g_assert_cmpuint (sent.calls, ==, 1);
  g_assert_cmpuint (sent.payload->len, ==, sizeof (GoodixPresetPskWrite) + len);
  g_assert_cmphex (sent.payload->data[4], ==, len & 0xff);
  g_assert_cmphex (sent.payload->data[5], ==, len >> 8);
  g_assert_cmphex (sent.payload->data[sent.payload->len - 1], ==, 0xa5);
  sent_reset ();
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  for (gsize i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      g_autofree gchar *with = g_strdup_printf ("/cmd/%s/callback",
                                                cases[i].name);
      g_autofree gchar *without = g_strdup_printf ("/cmd/%s/no-callback",
                                                   cases[i].name);

      g_test_add_data_func (with, &cases[i], test_with_callback);
      g_test_add_data_func (without, &cases[i], test_without_callback);
    }
  g_test_add_func ("/cmd/preset-psk-write/too-long", test_psk_write_too_long);
  g_test_add_func ("/cmd/preset-psk-write/largest", test_psk_write_largest);
  return g_test_run ();
}
