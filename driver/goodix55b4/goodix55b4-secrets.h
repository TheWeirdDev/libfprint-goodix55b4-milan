/*
 * Goodix 55b4 - pairing-key (PSK) selection.
 *
 * The sensor speaks TLS-PSK. By default it is provisioned with the all-zero
 * PSK and this returns zeros, so nothing changes. If a key file is present
 * (GOODIX55B4_PSK_FILE, else /etc/goodix55b4/psk.hex) it returns that key
 * instead, which lets a sensor keep the random PSK Windows paired it with
 * rather than being reprovisioned to zero.
 *
 * The file is two lines of hex: the 32-byte PSK, then its 32-byte PMK hash
 * (the value the sensor reports for TLV 0xbb020007). Both accessors return
 * 32-byte buffers and never fail; a malformed file is reported once and then
 * treated as "no file" (zero PSK).
 */
#pragma once

#include <glib.h>

/* The 32-byte PSK for the TLS handshake. */
const guint8 *goodix55b4_psk (void);

/* The expected 32-byte PMK hash the activation check compares against. */
const guint8 *goodix55b4_psk_hash (void);

/* TRUE if a key file was loaded (the sensor keeps a non-zero Windows PSK). */
gboolean goodix55b4_psk_is_custom (void);
