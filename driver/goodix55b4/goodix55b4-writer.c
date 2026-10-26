/*
 * Goodix 55b4 — background file writer
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define FP_COMPONENT "goodix55b4"

#include "goodix55b4-writer.h"

#include <pthread.h>

#include "fpi-log.h"

typedef struct
{
  gpointer       item;
  GDestroyNotify item_free;
} Pending;

struct _Goodix55b4Writer
{
  gchar              *what;
  Goodix55b4WriteFunc write;
  gpointer            user_data;
  GDestroyNotify      user_data_free;

  /* pthread rather than GMutex/GCond: GLib's futex-based ones are
   * invisible to ThreadSanitizer, which then reports false races. */
  pthread_mutex_t     lock;
  pthread_cond_t      idle_cond;
  GHashTable         *pending;   /* key -> Pending */
  gboolean            busy;      /* a worker thread is running */
  guint               writes;
};

static void
pending_free (Pending *p)
{
  if (p->item_free)
    p->item_free (p->item);
  g_free (p);
}

Goodix55b4Writer *
goodix55b4_writer_new (const char *what, Goodix55b4WriteFunc write,
                       gpointer user_data, GDestroyNotify user_data_free)
{
  Goodix55b4Writer *writer = g_new0 (Goodix55b4Writer, 1);

  writer->what = g_strdup (what);
  writer->write = write;
  writer->user_data = user_data;
  writer->user_data_free = user_data_free;
  pthread_mutex_init (&writer->lock, NULL);
  pthread_cond_init (&writer->idle_cond, NULL);
  writer->pending = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                           (GDestroyNotify) pending_free);
  return writer;
}

void
goodix55b4_writer_flush (Goodix55b4Writer *writer)
{
  pthread_mutex_lock (&writer->lock);
  while (writer->busy)
    pthread_cond_wait (&writer->idle_cond, &writer->lock);
  pthread_mutex_unlock (&writer->lock);
}

void
goodix55b4_writer_free (Goodix55b4Writer *writer)
{
  if (!writer)
    return;
  /* The worker holds no reference: wait until it has let go. */
  goodix55b4_writer_flush (writer);
  g_hash_table_unref (writer->pending);
  if (writer->user_data_free)
    writer->user_data_free (writer->user_data);
  pthread_mutex_destroy (&writer->lock);
  pthread_cond_destroy (&writer->idle_cond);
  g_free (writer->what);
  g_free (writer);
}

guint
goodix55b4_writer_writes (Goodix55b4Writer *writer)
{
  guint writes;

  pthread_mutex_lock (&writer->lock);
  writes = writer->writes;
  pthread_mutex_unlock (&writer->lock);
  return writes;
}

static gpointer
writer_worker (gpointer data)
{
  Goodix55b4Writer *writer = data;

  pthread_mutex_lock (&writer->lock);
  while (g_hash_table_size (writer->pending) > 0)
    {
      GHashTableIter iter;
      gpointer key_ptr, value;
      g_autofree gchar *key = NULL;
      Pending *p;
      g_autoptr(GError) error = NULL;
      gboolean written = FALSE;
      gboolean ok;

      g_hash_table_iter_init (&iter, writer->pending);
      g_hash_table_iter_next (&iter, &key_ptr, &value);
      g_hash_table_iter_steal (&iter);
      key = key_ptr;
      p = value;
      pthread_mutex_unlock (&writer->lock);

      ok = writer->write (key, p->item, writer->user_data, &written, &error);
      if (!ok)
        fp_warn ("cannot save %s: %s", writer->what,
                 error ? error->message : "unknown error");
      else if (!written)
        fp_dbg ("%s unchanged, not saved", writer->what);
      pending_free (p);

      pthread_mutex_lock (&writer->lock);
      if (ok && written)
        writer->writes++;
    }
  writer->busy = FALSE;
  pthread_cond_broadcast (&writer->idle_cond);
  pthread_mutex_unlock (&writer->lock);
  return NULL;
}

void
goodix55b4_writer_queue (Goodix55b4Writer *writer, const char *key,
                         gpointer item, GDestroyNotify item_free)
{
  Pending *p = g_new (Pending, 1);
  g_autoptr(GError) error = NULL;
  GThread *thread;

  p->item = item;
  p->item_free = item_free;

  pthread_mutex_lock (&writer->lock);
  /* Replaces (and frees) a request for the same key not yet started. */
  g_hash_table_replace (writer->pending, g_strdup (key), p);
  if (writer->busy)
    {
      /* The running worker picks it up. */
      pthread_mutex_unlock (&writer->lock);
      return;
    }
  writer->busy = TRUE;
  pthread_mutex_unlock (&writer->lock);

  thread = g_thread_try_new ("goodix55b4-write", writer_worker, writer, &error);
  if (thread)
    {
      g_thread_unref (thread);
      return;
    }
  /* No thread: write here rather than lose the data. */
  fp_warn ("cannot start the %s writer (%s); saving synchronously",
           writer->what, error->message);
  writer_worker (writer);
}
