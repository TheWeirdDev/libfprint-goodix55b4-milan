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

#include "goodix55b4-writer.h"
#include "milan/runtime.h"

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

/* Which studied templates are kept (GOODIX55B4_STUDY_POLICY).
 *
 * NATIVE (default): every one the engine produces, as the Windows driver:
 *   features are appended up to the template capacity (40), then replaced
 *   by better ones, enrolled features included. Adapts indefinitely.
 * BOUNDED: only while features are appended, and only from matches with
 *   score >= GOODIX55B4_STUDY_BOUNDED_MIN_SCORE. The enrolled features are
 *   never replaced and the template stops changing at capacity, which
 *   bounds what a wrong-finger accept could add; adapts less.
 * OFF: match the enrolled templates only (also GOODIX55B4_NO_STUDY=1). */
typedef enum {
  GOODIX55B4_STUDY_NATIVE,
  GOODIX55B4_STUDY_BOUNDED,
  GOODIX55B4_STUDY_OFF,
} Goodix55b4StudyPolicy;

/* Genuine matches on the reference set score 22..100 (median 45); 11%
 * are below 30. */
#define GOODIX55B4_STUDY_BOUNDED_MIN_SCORE (30)

/* From GOODIX55B4_STUDY_POLICY (native, bounded, off) and
 * GOODIX55B4_NO_STUDY=1; unknown values fall back to native, logged. */
Goodix55b4StudyPolicy goodix55b4_study_policy_from_env (void);

const char *goodix55b4_study_policy_name (Goodix55b4StudyPolicy policy);

/* Whether to keep out->final_candidate after a match against `matched`
 * (the template that was in the gallery). A candidate equal to it is never
 * kept: there is nothing new to write. */
gboolean goodix55b4_study_should_store (Goodix55b4StudyPolicy           policy,
                                        const GoodixMilanRuntimeOutput *out,
                                        GBytes                         *matched);

/* Writes studied templates in the background (goodix55b4-writer.h): the
 * key is the study key, the item a GBytes. */
Goodix55b4Writer *goodix55b4_study_writer_new (const char *dir);
