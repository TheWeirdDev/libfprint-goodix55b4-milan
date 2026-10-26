/*
 * Unit tests for the persisted engine state
 * (driver/goodix55b4/goodix55b4-state.c).
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>

#include "goodix55b4-state.h"

#define SUBTYPE (12)

typedef struct
{
  gchar                 *dir;
  gchar                 *path;
  Goodix55b4EngineState *a;
  Goodix55b4EngineState *b;
} Fixture;

static void
fixture_setup (Fixture *f, gconstpointer data)
{
  g_autoptr(GError) error = NULL;

  f->dir = g_dir_make_tmp ("goodix55b4-state-XXXXXX", &error);
  g_assert_no_error (error);
  f->path = g_build_filename (f->dir, "sub", "engine-state.bin", NULL);
  /* ~212 KB each: heap, as in the driver. */
  f->a = g_new0 (Goodix55b4EngineState, 1);
  f->b = g_new0 (Goodix55b4EngineState, 1);
  goodix55b4_state_init (f->a);
  f->a->state.sample_count = 7;
  f->a->state.stable_count = 3;
  f->a->state.calibration_map[0] = 1234;
  f->a->profile.setup_initialized = 1;
}

static void
fixture_teardown (Fixture *f, gconstpointer data)
{
  g_autofree gchar *sub = g_path_get_dirname (f->path);

  g_unlink (f->path);
  g_rmdir (sub);
  g_rmdir (f->dir);
  g_free (f->path);
  g_free (f->dir);
  g_free (f->a);
  g_free (f->b);
}

static void
save_a (Fixture *f)
{
  g_autoptr(GError) error = NULL;

  g_assert_true (goodix55b4_state_save (f->a, f->path, SUBTYPE, &error));
  g_assert_no_error (error);
}

static void
rewrite (Fixture *f, gssize offset, gssize truncate_to)
{
  g_autofree gchar *data = NULL;
  gsize len;
  g_autoptr(GError) error = NULL;

  g_assert_true (g_file_get_contents (f->path, &data, &len, &error));
  if (offset >= 0)
    data[offset] ^= 0x01;
  if (truncate_to >= 0)
    len = truncate_to;
  g_assert_true (g_file_set_contents (f->path, data, len, &error));
}

static void
assert_rejected (Fixture *f, guint16 subtype)
{
  g_autoptr(GError) error = NULL;

  memset (f->b, 0x77, sizeof (*f->b));
  g_assert_false (goodix55b4_state_load (f->b, f->path, subtype, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  /* Left untouched on failure. */
  for (gsize i = 0; i < sizeof (*f->b); i += 4096)
    g_assert_cmpuint (((guint8 *) f->b)[i], ==, 0x77);
}

static void
test_roundtrip (Fixture *f, gconstpointer data)
{
  g_autoptr(GError) error = NULL;
  struct stat st;

  save_a (f);
  g_assert_true (goodix55b4_state_load (f->b, f->path, SUBTYPE, &error));
  g_assert_no_error (error);
  g_assert_cmpmem (&f->b->state, sizeof (f->b->state), &f->a->state,
                   sizeof (f->a->state));
  g_assert_cmpmem (&f->b->profile, sizeof (f->b->profile), &f->a->profile,
                   sizeof (f->a->profile));

  /* Root-only: the state describes the enrolled user's skin. */
  g_assert_cmpint (g_stat (f->path, &st), ==, 0);
  g_assert_cmpuint (st.st_mode & 0777, ==, 0600);
}

static void
test_missing (Fixture *f, gconstpointer data)
{
  g_autoptr(GError) error = NULL;

  g_assert_false (goodix55b4_state_load (f->b, f->path, SUBTYPE, &error));
  g_assert_error (error, G_FILE_ERROR, G_FILE_ERROR_NOENT);
}

static void
test_corrupted (Fixture *f, gconstpointer data)
{
  save_a (f);
  rewrite (f, 200, -1); /* a byte of the state body */
  assert_rejected (f, SUBTYPE);
}

static void
test_bad_magic (Fixture *f, gconstpointer data)
{
  save_a (f);
  rewrite (f, 0, -1);
  assert_rejected (f, SUBTYPE);
}

static void
test_truncated (Fixture *f, gconstpointer data)
{
  save_a (f);
  rewrite (f, -1, 100);
  assert_rejected (f, SUBTYPE);
}

static void
test_wrong_subtype (Fixture *f, gconstpointer data)
{
  save_a (f);
  assert_rejected (f, SUBTYPE + 1);
}

static void
test_new_generation (Fixture *f, gconstpointer data)
{
  f->a->profile.setup_refresh_pending = 0;
  f->a->profile.setup_not_ready = 1;
  goodix55b4_state_new_generation (f->a);

  /* Calibration and process-owned state survive a new setup frame. */
  g_assert_cmpuint (f->a->state.sample_count, ==, 7);
  g_assert_cmpuint (f->a->state.stable_count, ==, 3);
  g_assert_cmpuint (f->a->state.calibration_map[0], ==, 1234);
  g_assert_cmpuint (f->a->profile.setup_initialized, ==, 1);
  /* ...and the engine is asked to re-admit the setup. */
  g_assert_cmpuint (f->a->profile.setup_refresh_pending, ==, 1);
  g_assert_cmpuint (f->a->profile.setup_not_ready, ==, 0);
}

/* ---- background saver ---- */

static void
assert_file_holds (Fixture *f, const Goodix55b4EngineState *expected)
{
  g_autoptr(GError) error = NULL;

  g_assert_true (goodix55b4_state_load (f->b, f->path, SUBTYPE, &error));
  g_assert_no_error (error);
  g_assert_cmpmem (f->b, sizeof (*f->b), expected, sizeof (*expected));
}

static void
test_saver_writes (Fixture *f, gconstpointer data)
{
  g_autoptr(Goodix55b4StateSaver) saver = goodix55b4_state_saver_new (f->path, SUBTYPE);

  goodix55b4_state_saver_queue (saver, f->a);
  /* The caller's state may change at once: the saver has its own copy. */
  f->a->state.sample_count = 999;
  goodix55b4_state_saver_flush (saver);
  g_assert_cmpuint (goodix55b4_state_saver_writes (saver), ==, 1);
  f->a->state.sample_count = 7;
  assert_file_holds (f, f->a);
}

static void
test_saver_skips_unchanged (Fixture *f, gconstpointer data)
{
  {
    g_autoptr(Goodix55b4StateSaver) saver = goodix55b4_state_saver_new (f->path, SUBTYPE);

    goodix55b4_state_saver_queue (saver, f->a);
    goodix55b4_state_saver_flush (saver);
    goodix55b4_state_saver_queue (saver, f->a);
    goodix55b4_state_saver_flush (saver);
    g_assert_cmpuint (goodix55b4_state_saver_writes (saver), ==, 1);
  }
  {
    /* As at open: the loaded state is known to be on disk. */
    g_autoptr(Goodix55b4StateSaver) saver = goodix55b4_state_saver_new (f->path, SUBTYPE);

    goodix55b4_state_saver_mark_saved (saver, f->a);
    goodix55b4_state_saver_queue (saver, f->a);
    goodix55b4_state_saver_flush (saver);
    g_assert_cmpuint (goodix55b4_state_saver_writes (saver), ==, 0);

    f->a->state.stable_count++;
    goodix55b4_state_saver_queue (saver, f->a);
    goodix55b4_state_saver_flush (saver);
    g_assert_cmpuint (goodix55b4_state_saver_writes (saver), ==, 1);
    assert_file_holds (f, f->a);
  }
}

static void
test_saver_latest_wins (Fixture *f, gconstpointer data)
{
  g_autoptr(Goodix55b4StateSaver) saver = goodix55b4_state_saver_new (f->path, SUBTYPE);
  guint writes;

  /* Faster than the disk: requests not yet started are replaced. */
  for (guint i = 1; i <= 50; i++)
    {
      f->a->state.sample_count = i;
      goodix55b4_state_saver_queue (saver, f->a);
    }
  goodix55b4_state_saver_flush (saver);
  writes = goodix55b4_state_saver_writes (saver);
  g_test_message ("50 requests, %u writes", writes);
  g_assert_cmpuint (writes, >=, 1);
  g_assert_cmpuint (writes, <=, 50);
  assert_file_holds (f, f->a);
}

static void
test_saver_error (Fixture *f, gconstpointer data)
{
  g_autoptr(GError) error = NULL;
  g_autofree gchar *blocker = g_path_get_dirname (f->path);
  g_autoptr(Goodix55b4StateSaver) saver = NULL;

  /* The state directory is a file: the write fails, is logged, and the
   * next request retries. */
  g_assert_true (g_file_set_contents (blocker, "x", 1, &error));
  saver = goodix55b4_state_saver_new (f->path, SUBTYPE);
  g_test_expect_message ("libfprint-goodix55b4", G_LOG_LEVEL_WARNING,
                         "cannot save engine state:*");
  goodix55b4_state_saver_queue (saver, f->a);
  goodix55b4_state_saver_flush (saver);
  g_test_assert_expected_messages ();
  g_assert_cmpuint (goodix55b4_state_saver_writes (saver), ==, 0);

  g_assert_cmpint (g_unlink (blocker), ==, 0);
  goodix55b4_state_saver_queue (saver, f->a);
  goodix55b4_state_saver_flush (saver);
  g_assert_cmpuint (goodix55b4_state_saver_writes (saver), ==, 1);
  assert_file_holds (f, f->a);
}

static void
test_saver_free_flushes (Fixture *f, gconstpointer data)
{
  Goodix55b4StateSaver *saver = goodix55b4_state_saver_new (f->path, SUBTYPE);

  /* Freed right after queueing, as at close: the state still lands. */
  goodix55b4_state_saver_queue (saver, f->a);
  goodix55b4_state_saver_free (saver);
  assert_file_holds (f, f->a);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
#define ADD(name, fn) \
  g_test_add ("/state/" name, Fixture, NULL, fixture_setup, fn, fixture_teardown)
  ADD ("roundtrip", test_roundtrip);
  ADD ("missing", test_missing);
  ADD ("corrupted", test_corrupted);
  ADD ("bad-magic", test_bad_magic);
  ADD ("truncated", test_truncated);
  ADD ("wrong-subtype", test_wrong_subtype);
  ADD ("new-generation", test_new_generation);
  ADD ("saver/writes", test_saver_writes);
  ADD ("saver/skips-unchanged", test_saver_skips_unchanged);
  ADD ("saver/latest-wins", test_saver_latest_wins);
  ADD ("saver/error", test_saver_error);
  ADD ("saver/free-flushes", test_saver_free_flushes);
#undef ADD
  return g_test_run ();
}
