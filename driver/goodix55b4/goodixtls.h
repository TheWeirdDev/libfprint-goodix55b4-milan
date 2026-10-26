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

#pragma once

#include <glib.h>

/* TLS 1.2 PSK server for the sensor (the device is the TLS client). It runs
 * on memory BIOs: the caller moves the bytes between the device and the
 * engine, so nothing blocks and no thread or socket is involved. */

/* The sensor is provisioned with the all-zero PSK. */
#define GOODIX_TLS_PSK_LENGTH (32)

/* The suite the 55b4/5584 firmware negotiates (0x00AE). The server offered
 * every OpenSSL suite ("ALL"); GOODIX55B4_TLS_CIPHERS (an OpenSSL cipher
 * string) overrides this for firmware that asks for another one. */
#define GOODIX_TLS_CIPHERS "PSK-AES128-CBC-SHA256"

typedef struct _GoodixTlsServer GoodixTlsServer;

GoodixTlsServer *goodix_tls_server_new (GError **error);

void goodix_tls_server_free (GoodixTlsServer *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (GoodixTlsServer, goodix_tls_server_free)

/* Feeds handshake bytes received from the device and advances the
 * handshake. *output receives the bytes to send to the device (NULL when
 * there are none) and *established is TRUE once the handshake is complete;
 * *output must be sent before *established is acted on. Feeding with
 * length 0 is allowed. Returns FALSE with error set if the handshake
 * failed (*output may still hold an alert for the device). */
gboolean goodix_tls_server_handshake (GoodixTlsServer *self,
                                      const guint8    *data,
                                      gsize            length,
                                      GBytes         **output,
                                      gboolean        *established,
                                      GError         **error);

gboolean goodix_tls_server_is_established (GoodixTlsServer *self);

/* Decrypts the application-data records in data, which must hold whole
 * records. Returns the plaintext, or NULL with error set. */
GBytes *goodix_tls_server_decrypt (GoodixTlsServer *self,
                                   const guint8    *data,
                                   gsize            length,
                                   GError         **error);
