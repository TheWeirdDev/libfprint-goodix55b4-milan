/*
 * Unit tests for the memory-BIO TLS server (driver/goodix55b4/goodixtls.c).
 *
 * An OpenSSL client configured like the sensor (TLS 1.2,
 * PSK-AES128-CBC-SHA256, all-zero PSK, no extended master secret, no
 * encrypt-then-MAC) plays the device. Build and run: scripts/run-tests.sh
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <glib.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <string.h>

#include "fp-device.h"
#include "goodixtls.h"

#define IMAGE_BYTES (108 * 88 / 4 * 6 + 4) /* GOODIX55B4_IMAGE_BYTES */

typedef struct
{
  SSL_CTX *ctx;
  SSL     *ssl;
  BIO     *rbio;
  BIO     *wbio;
} Device;

static guint8 device_psk_fill;
static const char *device_ciphers = "PSK-AES128-CBC-SHA256";

static unsigned int
device_psk_cb (SSL *ssl, const char *hint, char *identity,
               unsigned int max_identity_len, unsigned char *psk,
               unsigned int max_psk_len)
{
  g_assert_cmpuint (max_psk_len, >=, GOODIX_TLS_PSK_LENGTH);
  identity[0] = '\0';
  memset (psk, device_psk_fill, GOODIX_TLS_PSK_LENGTH);
  return GOODIX_TLS_PSK_LENGTH;
}

static void
device_init (Device *d, guint8 psk_fill)
{
  device_psk_fill = psk_fill;
  d->ctx = SSL_CTX_new (TLS_client_method ());
  g_assert_nonnull (d->ctx);
  g_assert_true (SSL_CTX_set_min_proto_version (d->ctx, TLS1_2_VERSION));
  g_assert_true (SSL_CTX_set_max_proto_version (d->ctx, TLS1_2_VERSION));
  g_assert_true (SSL_CTX_set_cipher_list (d->ctx, device_ciphers));
  SSL_CTX_set_options (d->ctx, SSL_OP_NO_EXTENDED_MASTER_SECRET |
                       SSL_OP_NO_ENCRYPT_THEN_MAC | SSL_OP_NO_TICKET);
  SSL_CTX_set_psk_client_callback (d->ctx, device_psk_cb);
  d->ssl = SSL_new (d->ctx);
  d->rbio = BIO_new (BIO_s_mem ());
  d->wbio = BIO_new (BIO_s_mem ());
  BIO_set_mem_eof_return (d->rbio, -1);
  SSL_set_bio (d->ssl, d->rbio, d->wbio);
  SSL_set_connect_state (d->ssl);
}

static void
device_clear (Device *d)
{
  SSL_free (d->ssl);
  SSL_CTX_free (d->ctx);
}

static GBytes *
device_output (Device *d)
{
  size_t n = BIO_ctrl_pending (d->wbio);
  guint8 *buf = g_malloc (n ? n : 1);

  if (n)
    g_assert_cmpint (BIO_read (d->wbio, buf, n), ==, (int) n);
  return g_bytes_new_take (buf, n);
}

static void
device_input (Device *d, GBytes *data)
{
  gsize n;
  const guint8 *p = g_bytes_get_data (data, &n);

  if (n)
    g_assert_cmpint (BIO_write (d->rbio, p, n), ==, (int) n);
}

/* Runs the device's side; returns its flight (possibly empty). */
static GBytes *
device_step (Device *d, GBytes *from_server, gboolean *done)
{
  int ret;

  if (from_server)
    device_input (d, from_server);
  ret = SSL_do_handshake (d->ssl);
  *done = ret == 1;
  if (ret != 1)
    g_assert_cmpint (SSL_get_error (d->ssl, ret), ==, SSL_ERROR_WANT_READ);
  return device_output (d);
}

static GBytes *
device_encrypt (Device *d, const guint8 *plain, gsize len)
{
  gsize written = 0;

  g_assert_true (SSL_write_ex (d->ssl, plain, len, &written));
  g_assert_cmpuint (written, ==, len);
  return device_output (d);
}

/* Feeds `data` to the server in pieces of `chunk` bytes (0: at once), as the
 * device may split a flight over packs. Returns the server's last output. */
static GBytes *
server_feed (GoodixTlsServer *s, GBytes *data, gsize chunk,
             gboolean *established)
{
  gsize n;
  const guint8 *p = g_bytes_get_data (data, &n);
  g_autoptr(GByteArray) out = g_byte_array_new ();
  gsize off = 0;

  *established = FALSE;
  do
    {
      gsize len = chunk ? MIN (chunk, n - off) : n;
      g_autoptr(GBytes) o = NULL;
      g_autoptr(GError) error = NULL;

      g_assert_true (goodix_tls_server_handshake (s, p + off, len, &o,
                                                  established, &error));
      g_assert_no_error (error);
      if (o)
        {
          /* Nothing but the last piece may produce output. */
          g_assert_cmpuint (off + len, ==, n);
          g_byte_array_append (out, g_bytes_get_data (o, NULL),
                               g_bytes_get_size (o));
        }
      off += len;
    }
  while (off < n);
  return g_byte_array_free_to_bytes (g_steal_pointer (&out));
}

static GoodixTlsServer *
connect_pair (Device *d, gsize chunk)
{
  g_autoptr(GError) error = NULL;
  GoodixTlsServer *s = goodix_tls_server_new (&error);
  g_autoptr(GBytes) hello = NULL;
  g_autoptr(GBytes) server_hello = NULL;
  g_autoptr(GBytes) client_flight = NULL;
  g_autoptr(GBytes) server_finished = NULL;
  g_autoptr(GBytes) rest = NULL;
  gboolean established, done;

  g_assert_no_error (error);
  g_assert_nonnull (s);
  device_init (d, 0x00);

  hello = device_step (d, NULL, &done);
  g_assert_false (done);
  server_hello = server_feed (s, hello, 0, &established);
  g_assert_false (established);
  g_assert_cmpuint (g_bytes_get_size (server_hello), >, 0);
  g_assert_false (goodix_tls_server_is_established (s));

  client_flight = device_step (d, server_hello, &done);
  g_assert_false (done);
  server_finished = server_feed (s, client_flight, chunk, &established);
  g_assert_true (established);
  g_assert_cmpuint (g_bytes_get_size (server_finished), >, 0);

  rest = device_step (d, server_finished, &done);
  g_assert_true (done);
  g_assert_cmpuint (g_bytes_get_size (rest), ==, 0);
  g_assert_true (goodix_tls_server_is_established (s));
  g_assert_cmpstr (SSL_get_cipher_name (d->ssl), ==, "PSK-AES128-CBC-SHA256");
  return s;
}

static guint8 *
pattern (gsize len)
{
  guint8 *p = g_malloc (len);

  for (gsize i = 0; i < len; i++)
    p[i] = (guint8) (i * 31 + 7);
  return p;
}

static void
check_roundtrip (GoodixTlsServer *s, Device *d, gsize len)
{
  g_autofree guint8 *plain = pattern (len);
  g_autoptr(GBytes) records = device_encrypt (d, plain, len);
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) out = goodix_tls_server_decrypt (
    s, g_bytes_get_data (records, NULL), g_bytes_get_size (records), &error);

  g_assert_no_error (error);
  g_assert_nonnull (out);
  g_assert_cmpmem (g_bytes_get_data (out, NULL), g_bytes_get_size (out),
                   plain, len);
}

static void
test_handshake_and_image (void)
{
  Device d;
  g_autoptr(GoodixTlsServer) s = connect_pair (&d, 0);

  /* A frame fits one record; several frames on one session. */
  check_roundtrip (s, &d, IMAGE_BYTES);
  check_roundtrip (s, &d, IMAGE_BYTES);
  device_clear (&d);
}

static void
test_handshake_fragmented (void)
{
  /* The client flight arriving in many small packs. */
  for (gsize chunk = 1; chunk <= 64; chunk *= 4)
    {
      Device d;
      g_autoptr(GoodixTlsServer) s = connect_pair (&d, chunk);

      check_roundtrip (s, &d, 100);
      device_clear (&d);
    }
}

static void
test_multi_record (void)
{
  Device d;
  g_autoptr(GoodixTlsServer) s = connect_pair (&d, 0);

  /* > 16 KiB: several records in one pack, all returned (the old code
   * returned the first record only). */
  check_roundtrip (s, &d, 40000);
  device_clear (&d);
}

static void
test_empty_feed (void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GoodixTlsServer) s = goodix_tls_server_new (&error);
  g_autoptr(GBytes) out = NULL;
  gboolean established = TRUE;

  g_assert_true (goodix_tls_server_handshake (s, NULL, 0, &out, &established,
                                              &error));
  g_assert_no_error (error);
  g_assert_null (out);
  g_assert_false (established);
}

static void
test_decrypt_before_handshake (void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GoodixTlsServer) s = goodix_tls_server_new (&error);
  static const guint8 junk[16] = { 0x17, 0x03, 0x03, 0x00, 0x0b };

  g_assert_null (goodix_tls_server_decrypt (s, junk, sizeof (junk), &error));
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO);
}

static void
test_garbage_hello (void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GoodixTlsServer) s = goodix_tls_server_new (&error);
  g_autofree guint8 *junk = pattern (300);
  g_autoptr(GBytes) out = NULL;
  g_autoptr(GBytes) again = NULL;
  gboolean established;

  g_assert_false (goodix_tls_server_handshake (s, junk, 300, &out,
                                               &established, &error));
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO);
  g_assert_false (established);
  g_clear_error (&error);

  /* The session stays failed. */
  g_assert_false (goodix_tls_server_handshake (s, NULL, 0, &again,
                                               &established, &error));
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO);
  g_assert_null (again);
}

static void
test_wrong_psk (void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GoodixTlsServer) s = goodix_tls_server_new (&error);
  g_autoptr(GBytes) hello = NULL;
  g_autoptr(GBytes) server_hello = NULL;
  g_autoptr(GBytes) flight = NULL;
  g_autoptr(GBytes) alert = NULL;
  gboolean established, done;
  Device d;

  device_init (&d, 0x41);
  hello = device_step (&d, NULL, &done);
  server_hello = server_feed (s, hello, 0, &established);
  flight = device_step (&d, server_hello, &done);

  g_assert_false (goodix_tls_server_handshake (
    s, g_bytes_get_data (flight, NULL), g_bytes_get_size (flight), &alert,
    &established, &error));
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO);
  g_assert_false (established);
  g_assert_false (goodix_tls_server_is_established (s));
  /* The alert for the device: one TLS alert record. */
  g_assert_nonnull (alert);
  g_assert_cmpuint (((const guint8 *) g_bytes_get_data (alert, NULL))[0], ==, 21);
  device_clear (&d);
}

static void
test_truncated_record (void)
{
  Device d;
  g_autoptr(GoodixTlsServer) s = connect_pair (&d, 0);
  g_autofree guint8 *plain = pattern (IMAGE_BYTES);
  g_autoptr(GBytes) records = device_encrypt (&d, plain, IMAGE_BYTES);
  g_autoptr(GError) error = NULL;

  g_assert_null (goodix_tls_server_decrypt (
    s, g_bytes_get_data (records, NULL), g_bytes_get_size (records) - 10,
    &error));
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO);
  g_assert_false (goodix_tls_server_is_established (s));
  device_clear (&d);
}

static void
test_corrupted_record (void)
{
  Device d;
  g_autoptr(GoodixTlsServer) s = connect_pair (&d, 0);
  g_autofree guint8 *plain = pattern (IMAGE_BYTES);
  g_autoptr(GBytes) records = device_encrypt (&d, plain, IMAGE_BYTES);
  gsize n = g_bytes_get_size (records);
  g_autofree guint8 *bad = g_memdup2 (g_bytes_get_data (records, NULL), n);
  g_autoptr(GError) error = NULL;

  bad[n / 2] ^= 0x01;
  g_assert_null (goodix_tls_server_decrypt (s, bad, n, &error));
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO);
  /* An empty queue: the error was not left for the next caller. */
  g_assert_cmpuint (ERR_peek_error (), ==, 0);
  device_clear (&d);
}

/* A client offering only another suite: refused by default, accepted when
 * GOODIX55B4_TLS_CIPHERS allows it. */
static gboolean
handshake_with_other_suite (void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GoodixTlsServer) s = goodix_tls_server_new (&error);
  g_autoptr(GBytes) hello = NULL;
  g_autoptr(GBytes) out = NULL;
  gboolean established, done, ok;
  Device d;

  g_assert_no_error (error);
  device_ciphers = "PSK-AES256-CBC-SHA384";
  device_init (&d, 0x00);
  hello = device_step (&d, NULL, &done);
  ok = goodix_tls_server_handshake (s, g_bytes_get_data (hello, NULL),
                                    g_bytes_get_size (hello), &out,
                                    &established, &error);
  device_clear (&d);
  device_ciphers = "PSK-AES128-CBC-SHA256";
  return ok;
}

static void
test_cipher_restricted (void)
{
  g_unsetenv ("GOODIX55B4_TLS_CIPHERS");
  g_assert_false (handshake_with_other_suite ());

  g_setenv ("GOODIX55B4_TLS_CIPHERS", "PSK-AES256-CBC-SHA384", TRUE);
  g_test_expect_message ("libfprint-goodixtls", G_LOG_LEVEL_WARNING,
                         "GOODIX55B4_TLS_CIPHERS=PSK-AES256-CBC-SHA384 overrides*");
  g_assert_true (handshake_with_other_suite ());
  g_test_assert_expected_messages ();
  g_unsetenv ("GOODIX55B4_TLS_CIPHERS");
}

static void
test_free_null (void)
{
  goodix_tls_server_free (NULL);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/goodixtls/handshake-and-image", test_handshake_and_image);
  g_test_add_func ("/goodixtls/handshake-fragmented", test_handshake_fragmented);
  g_test_add_func ("/goodixtls/multi-record", test_multi_record);
  g_test_add_func ("/goodixtls/empty-feed", test_empty_feed);
  g_test_add_func ("/goodixtls/decrypt-before-handshake",
                   test_decrypt_before_handshake);
  g_test_add_func ("/goodixtls/garbage-hello", test_garbage_hello);
  g_test_add_func ("/goodixtls/wrong-psk", test_wrong_psk);
  g_test_add_func ("/goodixtls/truncated-record", test_truncated_record);
  g_test_add_func ("/goodixtls/corrupted-record", test_corrupted_record);
  g_test_add_func ("/goodixtls/free-null", test_free_null);
  g_test_add_func ("/goodixtls/cipher-restricted", test_cipher_restricted);
  return g_test_run ();
}
