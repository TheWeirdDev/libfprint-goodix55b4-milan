/*
 * Goodix 55b4 — store for Milan templates updated by study (adaptive learning)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define FP_COMPONENT "goodix55b4"

#include "goodix55b4-study.h"

#include "milan/print.h"

#include <errno.h>
#include <gio/gio.h>
#include <string.h>

#include "fpi-log.h"

/* "G55BSD" + format version; bump on any layout change. */
static const char STUDY_MAGIC[8] = { 'G', '5', '5', 'B', 'S', 'D', '0', '1' };

typedef struct __attribute__((packed))
{
  char    magic[8];
  guint32 size;         /* of the template that follows */
  guint32 reserved;
  gchar   key[64];      /* hex SHA-256 of the enrolled template */
  guint8  sha256[32];   /* over the header key and the template */
} StudyHeader;

static gchar *
entry_path (const char *dir, const char *key)
{
  g_autofree gchar *name = g_strconcat (key, ".bin", NULL);

  return g_build_filename (dir, name, NULL);
}

static void
digest (const gchar key[64], const guint8 *tpl, gsize size, guint8 out[32])
{
  g_autoptr(GChecksum) c = g_checksum_new (G_CHECKSUM_SHA256);
  gsize len = 32;

  g_checksum_update (c, (const guchar *) key, 64);
  g_checksum_update (c, tpl, size);
  g_checksum_get_digest (c, out, &len);
}

gchar *
goodix55b4_study_key (GBytes *enrolled)
{
  return g_compute_checksum_for_bytes (G_CHECKSUM_SHA256, enrolled);
}

GBytes *
goodix55b4_study_load (const char *dir, const char *key, GError **error)
{
  g_autofree gchar *path = entry_path (dir, key);
  g_autofree gchar *data = NULL;
  g_autoptr(GBytes) tpl = NULL;
  gsize len = 0;
  StudyHeader h;
  guint8 sum[32];

  if (strlen (key) != sizeof (h.key))
    goto invalid;
  if (!g_file_get_contents (path, &data, &len, error))
    return NULL;
  if (len < sizeof (h))
    goto invalid;
  memcpy (&h, data, sizeof (h));
  if (memcmp (h.magic, STUDY_MAGIC, sizeof (STUDY_MAGIC)) != 0 ||
      h.size != len - sizeof (h) || h.size > GOODIX_MILAN_PRINT_MAX_SIZE ||
      memcmp (h.key, key, sizeof (h.key)) != 0)
    goto invalid;
  digest (h.key, (const guint8 *) data + sizeof (h), h.size, sum);
  if (memcmp (sum, h.sha256, sizeof (sum)) != 0)
    goto invalid;
  tpl = g_bytes_new (data + sizeof (h), h.size);
  if (!goodix_milan_print_validate_template (tpl, NULL, error))
    return NULL;
  return g_steal_pointer (&tpl);

invalid:
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
               "%s: not a valid studied template", path);
  return NULL;
}

gboolean
goodix55b4_study_save (const char *dir, const char *key, GBytes *studied,
                       GError **error)
{
  g_autofree gchar *path = entry_path (dir, key);
  g_autofree guint8 *buf = NULL;
  gsize size = 0;
  const guint8 *tpl = g_bytes_get_data (studied, &size);
  StudyHeader h = { 0 };

  if (strlen (key) != sizeof (h.key) || size > GOODIX_MILAN_PRINT_MAX_SIZE)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                           "Invalid studied template");
      return FALSE;
    }
  if (g_mkdir_with_parents (dir, 0700) != 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "Cannot create %s", dir);
      return FALSE;
    }
  memcpy (h.magic, STUDY_MAGIC, sizeof (STUDY_MAGIC));
  h.size = size;
  memcpy (h.key, key, sizeof (h.key));
  digest (h.key, tpl, size, h.sha256);

  buf = g_malloc (sizeof (h) + size);
  memcpy (buf, &h, sizeof (h));
  memcpy (buf + sizeof (h), tpl, size);
  /* temp file + rename: a crash leaves the previous entry intact */
  return g_file_set_contents_full (path, (const gchar *) buf, sizeof (h) + size,
                                   G_FILE_SET_CONTENTS_CONSISTENT, 0600, error);
}

Goodix55b4StudyPolicy
goodix55b4_study_policy_from_env (void)
{
  const char *policy = g_getenv ("GOODIX55B4_STUDY_POLICY");

  if (g_strcmp0 (g_getenv ("GOODIX55B4_NO_STUDY"), "1") == 0)
    return GOODIX55B4_STUDY_OFF;
  if (!policy || !*policy || g_strcmp0 (policy, "native") == 0)
    return GOODIX55B4_STUDY_NATIVE;
  if (g_strcmp0 (policy, "bounded") == 0)
    return GOODIX55B4_STUDY_BOUNDED;
  if (g_strcmp0 (policy, "off") == 0)
    return GOODIX55B4_STUDY_OFF;
  fp_warn ("unknown GOODIX55B4_STUDY_POLICY '%s', using native", policy);
  return GOODIX55B4_STUDY_NATIVE;
}

const char *
goodix55b4_study_policy_name (Goodix55b4StudyPolicy policy)
{
  switch (policy)
    {
    case GOODIX55B4_STUDY_BOUNDED:
      return "bounded";
    case GOODIX55B4_STUDY_OFF:
      return "off";
    case GOODIX55B4_STUDY_NATIVE:
    default:
      return "native";
    }
}

gboolean
goodix55b4_study_should_store (Goodix55b4StudyPolicy           policy,
                               const GoodixMilanRuntimeOutput *out,
                               GBytes                         *matched)
{
  if (policy == GOODIX55B4_STUDY_OFF ||
      out->status != GOODIX_MILAN_RUNTIME_MATCH || !out->final_candidate)
    return FALSE;
  /* Was written after every match, changed or not. */
  if (matched && g_bytes_equal (out->final_candidate, matched))
    return FALSE;
  if (policy == GOODIX55B4_STUDY_NATIVE)
    return TRUE;

  /* Bounded: a confident match that only adds a feature. Replacement
   * (REPLACE, REPLACE_NO_RELATION, GEOMETRIC) and lifecycle-only updates
   * (NONE) are not kept, so nothing enrolled is ever overwritten and the
   * template is final once full. */
  return out->study_action == GOODIX_MILAN_STUDY_APPEND &&
         out->score >= GOODIX55B4_STUDY_BOUNDED_MIN_SCORE;
}

/* On the writer's thread. */
static gboolean
study_write (const char *key, gpointer item, gpointer user_data,
             gboolean *written, GError **error)
{
  *written = goodix55b4_study_save (user_data, key, item, error);
  return *written;
}

Goodix55b4Writer *
goodix55b4_study_writer_new (const char *dir)
{
  return goodix55b4_writer_new ("studied template", study_write,
                                g_strdup (dir), g_free);
}
