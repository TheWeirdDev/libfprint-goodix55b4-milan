/*
 * Goodix 55b4 — store for Milan templates updated by study (adaptive learning)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#pragma once

#include <glib.h>

/* After a positive match the engine returns the winner template with the
 * probe studied into it (as the Windows driver keeps it). fprintd cannot
 * store an updated print, so the driver keeps it here, keyed by the enrolled
 * template: <dir>/<sha256 of the enrolled template>.bin. Re-enrolling gives
 * a new key, so a stale entry is simply never used again. */

/* Hex SHA-256 of the enrolled template; the caller frees it. */
gchar *goodix55b4_study_key (GBytes *enrolled);

/* The studied template for `key`, or NULL (G_FILE_ERROR_NOENT when there is
 * none). Entries for another key or failing template validation are
 * rejected, so the caller falls back to the enrolled template. */
GBytes *goodix55b4_study_load (const char *dir,
                               const char *key,
                               GError    **error);
gboolean goodix55b4_study_save (const char *dir,
                                const char *key,
                                GBytes     *studied,
                                GError    **error);
