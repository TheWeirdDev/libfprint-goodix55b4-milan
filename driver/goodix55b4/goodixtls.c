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

#define FP_COMPONENT "goodixtls"

#include <limits.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <string.h>

#include "fp-device.h"
#include "fpi-log.h"
#include "goodixtls.h"

/* Was an SSL_accept() thread on a socketpair: the main loop blocked in
 * read() on it during the handshake (forever if the handshake failed), and
 * teardown freed the SSL object while the thread could still be using it. */
struct _GoodixTlsServer
{
  SSL_CTX *ctx;
  SSL     *ssl;
  BIO     *rbio;        /* device -> engine, owned by ssl */
  BIO     *wbio;        /* engine -> device, owned by ssl */
  gboolean established;
  gboolean failed;      /* a fatal error happened; the session is unusable */
};

/* Takes the first queued OpenSSL error (the root cause) and clears the
 * queue, so a stale entry is never reported for a later call. */
static void
set_tls_error (GError **error, const char *what, int ssl_error)
{
  unsigned long code = ERR_peek_error ();
  const char *reason = code ? ERR_reason_error_string (code) : NULL;

  if (code)
    g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                 "TLS %s failed: %s (0x%lx)", what,
                 reason ? reason : "unknown", code);
  else
    g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                 "TLS %s failed (SSL error %d)", what, ssl_error);
  ERR_clear_error ();
}

static unsigned int
psk_server_callback (SSL           *ssl,
                     const char    *identity,
                     unsigned char *psk,
                     unsigned int   max_psk_len)
{
  if (max_psk_len < GOODIX_TLS_PSK_LENGTH)
    {
      fp_warn ("PSK buffer too small (%u bytes)", max_psk_len);
      return 0;
    }
  memset (psk, 0, GOODIX_TLS_PSK_LENGTH);
  return GOODIX_TLS_PSK_LENGTH;
}

GoodixTlsServer *
goodix_tls_server_new (GError **error)
{
  g_autoptr(GoodixTlsServer) self = g_new0 (GoodixTlsServer, 1);

  ERR_clear_error ();
  self->ctx = SSL_CTX_new (TLS_server_method ());
  if (!self->ctx)
    {
      set_tls_error (error, "context creation", 0);
      return NULL;
    }
  /* Same settings as the previous server: TLS 1.2 only, every cipher (the
   * device picks its PSK suite), the all-zero PSK. */
  SSL_CTX_set_dh_auto (self->ctx, 1);
  SSL_CTX_set_options (self->ctx, SSL_OP_NO_RENEGOTIATION);
  if (!SSL_CTX_set_min_proto_version (self->ctx, TLS1_2_VERSION) ||
      !SSL_CTX_set_max_proto_version (self->ctx, TLS1_2_VERSION) ||
      !SSL_CTX_set_cipher_list (self->ctx, "ALL"))
    {
      set_tls_error (error, "context setup", 0);
      return NULL;
    }
  SSL_CTX_set_psk_server_callback (self->ctx, psk_server_callback);

  self->ssl = SSL_new (self->ctx);
  if (!self->ssl)
    {
      set_tls_error (error, "session creation", 0);
      return NULL;
    }
  self->rbio = BIO_new (BIO_s_mem ());
  self->wbio = BIO_new (BIO_s_mem ());
  if (!self->rbio || !self->wbio)
    {
      BIO_free (self->rbio);
      BIO_free (self->wbio);
      self->rbio = self->wbio = NULL;
      set_tls_error (error, "BIO creation", 0);
      return NULL;
    }
  /* An empty input BIO means "wait for more", not EOF. */
  BIO_set_mem_eof_return (self->rbio, -1);
  SSL_set_bio (self->ssl, self->rbio, self->wbio);
  SSL_set_accept_state (self->ssl);

  return g_steal_pointer (&self);
}

void
goodix_tls_server_free (GoodixTlsServer *self)
{
  if (!self)
    return;
  /* SSL_free also frees both BIOs. No close_notify: the device never reads
   * one, and the session is dropped with the USB interface anyway. */
  SSL_free (self->ssl);
  SSL_CTX_free (self->ctx);
  g_free (self);
}

static gboolean
feed (GoodixTlsServer *self, const guint8 *data, gsize length, GError **error)
{
  if (length == 0)
    return TRUE;
  if (length > INT_MAX || BIO_write (self->rbio, data, (int) length) != (int) length)
    {
      self->failed = TRUE;
      set_tls_error (error, "input buffering", 0);
      return FALSE;
    }
  return TRUE;
}

/* Everything the engine wrote for the device, or NULL. */
static GBytes *
drain_output (GoodixTlsServer *self)
{
  size_t pending = BIO_ctrl_pending (self->wbio);
  guint8 *buf;
  int n;

  if (pending == 0 || pending > INT_MAX)
    return NULL;
  buf = g_malloc (pending);
  n = BIO_read (self->wbio, buf, (int) pending);
  if (n <= 0)
    {
      g_free (buf);
      return NULL;
    }
  return g_bytes_new_take (buf, n);
}

gboolean
goodix_tls_server_handshake (GoodixTlsServer *self,
                             const guint8    *data,
                             gsize            length,
                             GBytes         **output,
                             gboolean        *established,
                             GError         **error)
{
  g_return_val_if_fail (self != NULL, FALSE);
  g_return_val_if_fail (data != NULL || length == 0, FALSE);
  g_return_val_if_fail (output != NULL && established != NULL, FALSE);

  *output = NULL;
  *established = FALSE;
  if (self->failed)
    {
      g_set_error_literal (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                           "TLS session already failed");
      return FALSE;
    }
  if (!feed (self, data, length, error))
    return FALSE;

  if (!self->established)
    {
      int ret;

      ERR_clear_error ();
      ret = SSL_do_handshake (self->ssl);
      if (ret == 1)
        {
          self->established = TRUE;
          fp_dbg ("TLS handshake done, cipher %s",
                  SSL_get_cipher_name (self->ssl));
        }
      else
        {
          int ssl_error = SSL_get_error (self->ssl, ret);

          if (ssl_error != SSL_ERROR_WANT_READ &&
              ssl_error != SSL_ERROR_WANT_WRITE)
            {
              self->failed = TRUE;
              /* Hand out the alert OpenSSL queued for the device. */
              *output = drain_output (self);
              set_tls_error (error, "handshake", ssl_error);
              return FALSE;
            }
        }
    }

  *output = drain_output (self);
  *established = self->established;
  return TRUE;
}

gboolean
goodix_tls_server_is_established (GoodixTlsServer *self)
{
  g_return_val_if_fail (self != NULL, FALSE);

  return self->established && !self->failed;
}

GBytes *
goodix_tls_server_decrypt (GoodixTlsServer *self,
                           const guint8    *data,
                           gsize            length,
                           GError         **error)
{
  g_autoptr(GByteArray) plain = NULL;

  g_return_val_if_fail (self != NULL, NULL);
  g_return_val_if_fail (data != NULL || length == 0, NULL);

  if (!goodix_tls_server_is_established (self))
    {
      g_set_error_literal (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                           "TLS session is not established");
      return NULL;
    }
  if (!feed (self, data, length, error))
    return NULL;

  plain = g_byte_array_new ();
  /* One SSL_read returns at most one record; read until the input is used. */
  for (;;)
    {
      const guint chunk = 16384; /* maximum TLS record plaintext */
      guint used = plain->len;
      int n;

      g_byte_array_set_size (plain, used + chunk);
      ERR_clear_error ();
      n = SSL_read (self->ssl, plain->data + used, chunk);
      if (n > 0)
        {
          g_byte_array_set_size (plain, used + n);
          continue;
        }
      g_byte_array_set_size (plain, used);

      int ssl_error = SSL_get_error (self->ssl, n);
      if (ssl_error == SSL_ERROR_WANT_READ)
        break;
      self->failed = TRUE;
      if (ssl_error == SSL_ERROR_ZERO_RETURN)
        g_set_error_literal (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                             "TLS session closed by the device");
      else
        set_tls_error (error, "decryption", ssl_error);
      return NULL;
    }

  /* A record cut short would desynchronise every later read. */
  if (SSL_has_pending (self->ssl) || BIO_ctrl_pending (self->rbio) > 0)
    {
      self->failed = TRUE;
      g_set_error_literal (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                           "Truncated TLS record from the device");
      return NULL;
    }
  if (plain->len == 0)
    {
      g_set_error_literal (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                           "No TLS application data from the device");
      return NULL;
    }
  return g_byte_array_free_to_bytes (g_steal_pointer (&plain));
}
