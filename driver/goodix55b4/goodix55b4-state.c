/*
 * Goodix 55b4 — Milan engine state: generation handling and persistence
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define FP_COMPONENT "goodix55b4"

#include "goodix55b4-state.h"
#include "goodix55b4-state-layout.h"

#include <errno.h>
#include <gio/gio.h>
#include <pthread.h>
#include <string.h>

#include "fpi-log.h"

/* "G55BST" + format version; bump on any layout change. */
static const char STATE_MAGIC[8] = { 'G', '5', '5', 'B', 'S', 'T', '0', '1' };

/* The layout STATE_MAGIC "01" files were written with. If this fails, the
 * engine state structs changed: bump STATE_MAGIC (old files are then
 * ignored and the engine starts afresh) and set the new value, printed by
 * printf ("0x%08xu", GOODIX55B4_STATE_LAYOUT). */
_Static_assert (GOODIX55B4_STATE_LAYOUT == 0x8a03c60bu,
                "engine state layout changed: bump STATE_MAGIC and update "
                "the pinned layout value");

typedef struct __attribute__((packed))
{
  char    magic[8];
  guint32 state_size;
  guint32 profile_size;
  guint16 subtype;
  guint16 reserved;
  guint8  sha256[32];   /* over the two structs that follow */
} StateHeader;

void
goodix55b4_state_init (Goodix55b4EngineState *s)
{
  memset (s, 0, sizeof (*s));
  goodix_milan_preprocess_reset (&s->state);
}

void
goodix55b4_state_new_generation (Goodix55b4EngineState *s)
{
  /* heap copy: the state is ~212 KB */
  g_autofree Goodix55b4EngineState *old = g_memdup2 (s, sizeof (*s));
  GoodixMilanPreprocessState *d = &s->state;
  const GoodixMilanPreprocessState *o = &old->state;

  goodix55b4_state_init (s);

  /* 1. Persisted calibration ("setup save"), as upstream
   *    goodix_milan_persistence_restore() reloads it on every setup refresh. */
  d->profile9_history_mask_threshold = 60; /* native import defaults */
  d->profile9_history_mask_average = 60;
  d->sample_count = o->sample_count;
  memcpy (d->calibration_map, o->calibration_map, sizeof (d->calibration_map));
  d->extraction_classification.retained_count =
    o->extraction_classification.retained_count;
  d->extraction_persistence.retained_count =
    o->extraction_persistence.retained_count;
  memcpy (d->extraction_classification.retained_class_planes,
          o->extraction_classification.retained_class_planes,
          sizeof (d->extraction_classification.retained_class_planes));
  memcpy (d->extraction_persistence.retained_class_planes,
          o->extraction_persistence.retained_class_planes,
          sizeof (d->extraction_persistence.retained_class_planes));
  d->profile9_history_update_count = o->profile9_history_update_count;
  d->profile9_history_count = o->profile9_history_count;
  memcpy (d->profile9_component_age, o->profile9_component_age,
          sizeof (d->profile9_component_age));
  memcpy (d->profile9_reference_age, o->profile9_reference_age,
          sizeof (d->profile9_reference_age));
  memcpy (d->profile9_history_reference, o->profile9_history_reference,
          sizeof (d->profile9_history_reference));

  /* 2. Process-owned state (goodix_milan_generation_transfer_process_state). */
  d->stable_count = o->stable_count;
  d->auxiliary_sample_count = o->auxiliary_sample_count;
  d->application_gain_initialized = o->application_gain_initialized;
  memcpy (d->coarse_reference, o->coarse_reference, sizeof (d->coarse_reference));
  memcpy (d->auxiliary_gain_map, o->auxiliary_gain_map, sizeof (d->auxiliary_gain_map));
  memcpy (d->secondary_auxiliary_gain_map, o->secondary_auxiliary_gain_map,
          sizeof (d->secondary_auxiliary_gain_map));
  memcpy (d->application_gain_map, o->application_gain_map,
          sizeof (d->application_gain_map));
  if (o->profile9_classifier_initialized)
    {
      d->profile9_history_count = o->profile9_history_count;
      d->profile9_history_update_count = o->profile9_history_update_count;
      d->profile9_history_mask_threshold = o->profile9_history_mask_threshold;
      d->profile9_history_mask_average = o->profile9_history_mask_average;
      memcpy (d->profile9_history_reference, o->profile9_history_reference,
              sizeof (d->profile9_history_reference));
      memcpy (d->profile9_reference_age, o->profile9_reference_age,
              sizeof (d->profile9_reference_age));
      memcpy (d->profile9_component_age, o->profile9_component_age,
              sizeof (d->profile9_component_age));
      d->extraction_classification = o->extraction_classification;
      d->profile9_classifier_initialized = 1;
    }
  s->profile = old->profile;
  s->profile.setup_refresh_pending = 1;
  s->profile.setup_not_ready = 0;
}

void
goodix55b4_state_apply_output (Goodix55b4EngineState          *s,
                               const GoodixMilanRuntimeOutput *out)
{
  if (out->preprocess_state_valid &&
      out->status != GOODIX_MILAN_RUNTIME_CANCELLED)
    {
      s->state = out->preprocess_state;
      s->profile = out->profile_state;
    }
  else if (out->setup_state_valid)
    {
      s->profile.setup_initialized = out->profile_state.setup_initialized;
      s->profile.setup_refresh_pending =
        out->profile_state.setup_refresh_pending;
      s->profile.setup_not_ready = out->profile_state.setup_not_ready;
    }
}

static void
digest (const Goodix55b4EngineState *s, guint8 out[32])
{
  g_autoptr(GChecksum) c = g_checksum_new (G_CHECKSUM_SHA256);
  gsize len = 32;

  g_checksum_update (c, (const guchar *) &s->state, sizeof (s->state));
  g_checksum_update (c, (const guchar *) &s->profile, sizeof (s->profile));
  g_checksum_get_digest (c, out, &len);
}

gboolean
goodix55b4_state_load (Goodix55b4EngineState *s, const char *path,
                       guint16 subtype, GError **error)
{
  g_autofree gchar *data = NULL;
  gsize len = 0;
  StateHeader h;
  g_autofree Goodix55b4EngineState *tmp = g_new0 (Goodix55b4EngineState, 1);
  guint8 sum[32];

  if (!g_file_get_contents (path, &data, &len, error))
    return FALSE;
  if (len != sizeof (h) + sizeof (tmp->state) + sizeof (tmp->profile))
    goto invalid;
  memcpy (&h, data, sizeof (h));
  if (memcmp (h.magic, STATE_MAGIC, sizeof (STATE_MAGIC)) != 0 ||
      h.state_size != sizeof (tmp->state) ||
      h.profile_size != sizeof (tmp->profile) || h.subtype != subtype)
    goto invalid;
  memcpy (&tmp->state, data + sizeof (h), sizeof (tmp->state));
  memcpy (&tmp->profile, data + sizeof (h) + sizeof (tmp->state), sizeof (tmp->profile));
  digest (tmp, sum);
  if (memcmp (sum, h.sha256, sizeof (sum)) != 0)
    goto invalid;
  *s = *tmp;
  return TRUE;

invalid:
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
               "%s: not a valid engine state for this build", path);
  return FALSE;
}

gboolean
goodix55b4_state_save (const Goodix55b4EngineState *s, const char *path,
                       guint16 subtype, GError **error)
{
  g_autofree gchar *dir = g_path_get_dirname (path);
  g_autofree guint8 *buf = NULL;
  gsize len = sizeof (StateHeader) + sizeof (s->state) + sizeof (s->profile);
  StateHeader h = { 0 };

  if (g_mkdir_with_parents (dir, 0700) != 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "Cannot create %s", dir);
      return FALSE;
    }
  memcpy (h.magic, STATE_MAGIC, sizeof (STATE_MAGIC));
  h.state_size = sizeof (s->state);
  h.profile_size = sizeof (s->profile);
  h.subtype = subtype;
  digest (s, h.sha256);

  buf = g_malloc (len);
  memcpy (buf, &h, sizeof (h));
  memcpy (buf + sizeof (h), &s->state, sizeof (s->state));
  memcpy (buf + sizeof (h) + sizeof (s->state), &s->profile, sizeof (s->profile));
  /* g_file_set_contents writes to a temp file and renames: no torn state. */
  return g_file_set_contents_full (path, (const gchar *) buf, len,
                                   G_FILE_SET_CONTENTS_CONSISTENT, 0600, error);
}

struct _Goodix55b4StateSaver
{
  gchar                 *path;
  guint16                subtype;

  /* pthread rather than GMutex/GCond: GLib's futex-based ones are
   * invisible to ThreadSanitizer, which then reports false races here. */
  pthread_mutex_t        lock;
  pthread_cond_t         idle_cond;
  Goodix55b4EngineState *pending;      /* next state to write, owned */
  gboolean               busy;         /* a worker thread is running */
  guint8                 last[32];     /* digest of the file's contents */
  gboolean               have_last;
  guint                  writes;
};

Goodix55b4StateSaver *
goodix55b4_state_saver_new (const char *path, guint16 subtype)
{
  Goodix55b4StateSaver *saver = g_new0 (Goodix55b4StateSaver, 1);

  saver->path = g_strdup (path);
  saver->subtype = subtype;
  pthread_mutex_init (&saver->lock, NULL);
  pthread_cond_init (&saver->idle_cond, NULL);
  return saver;
}

void
goodix55b4_state_saver_flush (Goodix55b4StateSaver *saver)
{
  pthread_mutex_lock (&saver->lock);
  while (saver->busy)
    pthread_cond_wait (&saver->idle_cond, &saver->lock);
  pthread_mutex_unlock (&saver->lock);
}

void
goodix55b4_state_saver_free (Goodix55b4StateSaver *saver)
{
  if (!saver)
    return;
  /* The worker owns no reference: wait until it has let go. */
  goodix55b4_state_saver_flush (saver);
  g_free (saver->pending);
  pthread_mutex_destroy (&saver->lock);
  pthread_cond_destroy (&saver->idle_cond);
  g_free (saver->path);
  g_free (saver);
}

void
goodix55b4_state_saver_mark_saved (Goodix55b4StateSaver        *saver,
                                   const Goodix55b4EngineState *s)
{
  guint8 sum[32];

  digest (s, sum);
  pthread_mutex_lock (&saver->lock);
  memcpy (saver->last, sum, sizeof (sum));
  saver->have_last = TRUE;
  pthread_mutex_unlock (&saver->lock);
}

guint
goodix55b4_state_saver_writes (Goodix55b4StateSaver *saver)
{
  guint writes;

  pthread_mutex_lock (&saver->lock);
  writes = saver->writes;
  pthread_mutex_unlock (&saver->lock);
  return writes;
}

static gpointer
saver_worker (gpointer data)
{
  Goodix55b4StateSaver *saver = data;

  pthread_mutex_lock (&saver->lock);
  while (saver->pending)
    {
      g_autofree Goodix55b4EngineState *s = g_steal_pointer (&saver->pending);
      g_autoptr(GError) error = NULL;
      guint8 sum[32];
      gboolean unchanged;

      pthread_mutex_unlock (&saver->lock);

      digest (s, sum);
      pthread_mutex_lock (&saver->lock);
      unchanged = saver->have_last && memcmp (sum, saver->last, sizeof (sum)) == 0;
      pthread_mutex_unlock (&saver->lock);

      if (unchanged)
        fp_dbg ("engine state unchanged, not saved");
      else if (!goodix55b4_state_save (s, saver->path, saver->subtype, &error))
        fp_warn ("cannot save engine state: %s", error->message);

      pthread_mutex_lock (&saver->lock);
      if (!unchanged && !error)
        {
          memcpy (saver->last, sum, sizeof (sum));
          saver->have_last = TRUE;
          saver->writes++;
        }
    }
  saver->busy = FALSE;
  pthread_cond_broadcast (&saver->idle_cond);
  pthread_mutex_unlock (&saver->lock);
  return NULL;
}

void
goodix55b4_state_saver_queue (Goodix55b4StateSaver        *saver,
                              const Goodix55b4EngineState *s)
{
  /* A copy: the caller keeps changing its state. ~212 KB, heap only. */
  Goodix55b4EngineState *copy = g_memdup2 (s, sizeof (*s));
  g_autoptr(GError) error = NULL;
  GThread *thread;

  pthread_mutex_lock (&saver->lock);
  g_free (saver->pending);
  saver->pending = copy;
  if (saver->busy)
    {
      /* The running worker picks it up. */
      pthread_mutex_unlock (&saver->lock);
      return;
    }
  saver->busy = TRUE;
  pthread_mutex_unlock (&saver->lock);

  thread = g_thread_try_new ("goodix55b4-state", saver_worker, saver, &error);
  if (thread)
    {
      g_thread_unref (thread);
      return;
    }
  /* No thread: write here rather than lose the state. */
  fp_warn ("cannot start the state writer (%s); saving synchronously",
           error->message);
  saver_worker (saver);
}
