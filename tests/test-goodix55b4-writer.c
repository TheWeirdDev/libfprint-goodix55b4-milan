/*
 * Unit tests for the background writer (driver/goodix55b4/goodix55b4-writer.c).
 * The write function records what it was given; a gate lets a test hold the
 * worker inside a write while it queues more.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <gio/gio.h>
#include <pthread.h>
#include <string.h>

#include "goodix55b4-writer.h"

typedef struct
{
  pthread_mutex_t lock;
  pthread_cond_t  cond;
  gboolean        hold;      /* writes wait while TRUE */
  gboolean        entered;   /* a write is waiting at the gate */
  GPtrArray      *log;       /* "key=value" per write call */
  const char     *fail_key;  /* this key fails */
  const char     *skip_key;  /* this key is "unchanged" */
} Sink;

static void
sink_init (Sink *s)
{
  memset (s, 0, sizeof (*s));
  pthread_mutex_init (&s->lock, NULL);
  pthread_cond_init (&s->cond, NULL);
  s->log = g_ptr_array_new_with_free_func (g_free);
}

static void
sink_clear (Sink *s)
{
  g_ptr_array_unref (s->log);
  pthread_mutex_destroy (&s->lock);
  pthread_cond_destroy (&s->cond);
}

static gboolean
sink_write (const char *key, gpointer item, gpointer user_data,
            gboolean *written, GError **error)
{
  Sink *s = user_data;

  pthread_mutex_lock (&s->lock);
  s->entered = TRUE;
  pthread_cond_broadcast (&s->cond);
  while (s->hold)
    pthread_cond_wait (&s->cond, &s->lock);
  g_ptr_array_add (s->log, g_strdup_printf ("%s=%s", key, (char *) item));
  pthread_mutex_unlock (&s->lock);

  if (g_strcmp0 (key, s->fail_key) == 0)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "disk full");
      return FALSE;
    }
  *written = g_strcmp0 (key, s->skip_key) != 0;
  return TRUE;
}

/* Holds the worker inside its next write until sink_release. */
static void
sink_hold (Sink *s)
{
  pthread_mutex_lock (&s->lock);
  s->hold = TRUE;
  s->entered = FALSE;
  pthread_mutex_unlock (&s->lock);
}

static void
sink_wait_entered (Sink *s)
{
  pthread_mutex_lock (&s->lock);
  while (!s->entered)
    pthread_cond_wait (&s->cond, &s->lock);
  pthread_mutex_unlock (&s->lock);
}

static void
sink_release (Sink *s)
{
  pthread_mutex_lock (&s->lock);
  s->hold = FALSE;
  pthread_cond_broadcast (&s->cond);
  pthread_mutex_unlock (&s->lock);
}

static gboolean
logged (Sink *s, const char *entry)
{
  for (guint i = 0; i < s->log->len; i++)
    if (g_strcmp0 (s->log->pdata[i], entry) == 0)
      return TRUE;
  return FALSE;
}

static void
test_writes_and_frees_items (void)
{
  Sink s;

  sink_init (&s);
  {
    g_autoptr(Goodix55b4Writer) w = goodix55b4_writer_new ("test", sink_write, &s, NULL);

    goodix55b4_writer_queue (w, "a", g_strdup ("1"), g_free);
    goodix55b4_writer_flush (w);
    g_assert_cmpuint (goodix55b4_writer_writes (w), ==, 1);
    g_assert_true (logged (&s, "a=1"));
  }
  sink_clear (&s);
}

static void
test_latest_wins_per_key (void)
{
  Sink s;

  sink_init (&s);
  {
    g_autoptr(Goodix55b4Writer) w = goodix55b4_writer_new ("test", sink_write, &s, NULL);

    /* Hold the worker in the write of a=1, then queue more. */
    sink_hold (&s);
    goodix55b4_writer_queue (w, "a", g_strdup ("1"), g_free);
    sink_wait_entered (&s);
    goodix55b4_writer_queue (w, "a", g_strdup ("2"), g_free);
    goodix55b4_writer_queue (w, "b", g_strdup ("1"), g_free);
    goodix55b4_writer_queue (w, "a", g_strdup ("3"), g_free);
    sink_release (&s);
    goodix55b4_writer_flush (w);

    /* a=2 was replaced before it started; other keys are independent. */
    g_assert_cmpuint (s.log->len, ==, 3);
    g_assert_true (logged (&s, "a=1"));
    g_assert_false (logged (&s, "a=2"));
    g_assert_true (logged (&s, "a=3"));
    g_assert_true (logged (&s, "b=1"));
    g_assert_cmpuint (goodix55b4_writer_writes (w), ==, 3);
  }
  sink_clear (&s);
}

static void
test_unchanged_not_counted (void)
{
  Sink s;

  sink_init (&s);
  s.skip_key = "same";
  {
    g_autoptr(Goodix55b4Writer) w = goodix55b4_writer_new ("test", sink_write, &s, NULL);

    goodix55b4_writer_queue (w, "same", g_strdup ("x"), g_free);
    goodix55b4_writer_flush (w);
    g_assert_cmpuint (s.log->len, ==, 1);
    g_assert_cmpuint (goodix55b4_writer_writes (w), ==, 0);
  }
  sink_clear (&s);
}

static void
test_error_logged (void)
{
  Sink s;

  sink_init (&s);
  s.fail_key = "bad";
  {
    g_autoptr(Goodix55b4Writer) w = goodix55b4_writer_new ("test file", sink_write, &s, NULL);

    g_test_expect_message ("libfprint-goodix55b4", G_LOG_LEVEL_WARNING,
                           "cannot save test file: disk full");
    goodix55b4_writer_queue (w, "bad", g_strdup ("x"), g_free);
    goodix55b4_writer_flush (w);
    g_test_assert_expected_messages ();
    g_assert_cmpuint (goodix55b4_writer_writes (w), ==, 0);

    /* The worker carries on with other keys. */
    goodix55b4_writer_queue (w, "good", g_strdup ("y"), g_free);
    goodix55b4_writer_flush (w);
    g_assert_cmpuint (goodix55b4_writer_writes (w), ==, 1);
  }
  sink_clear (&s);
}

static void
test_free_flushes (void)
{
  Sink s;
  Goodix55b4Writer *w;

  sink_init (&s);
  w = goodix55b4_writer_new ("test", sink_write, &s, NULL);
  sink_hold (&s);
  goodix55b4_writer_queue (w, "a", g_strdup ("1"), g_free);
  sink_wait_entered (&s);
  goodix55b4_writer_queue (w, "b", g_strdup ("2"), g_free);
  sink_release (&s);
  /* Freed with a write running and one queued: both still happen. */
  goodix55b4_writer_free (w);
  g_assert_true (logged (&s, "a=1"));
  g_assert_true (logged (&s, "b=2"));
  sink_clear (&s);
}

static void
test_user_data_freed (void)
{
  /* LeakSanitizer fails the run if the directory string is not freed. */
  Goodix55b4Writer *w = goodix55b4_writer_new ("test", sink_write,
                                               g_strdup ("dir"), g_free);

  goodix55b4_writer_free (w);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/writer/writes-and-frees-items", test_writes_and_frees_items);
  g_test_add_func ("/writer/latest-wins-per-key", test_latest_wins_per_key);
  g_test_add_func ("/writer/unchanged-not-counted", test_unchanged_not_counted);
  g_test_add_func ("/writer/error-logged", test_error_logged);
  g_test_add_func ("/writer/free-flushes", test_free_flushes);
  g_test_add_func ("/writer/user-data-freed", test_user_data_freed);
  return g_test_run ();
}
