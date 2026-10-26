/*
 * Unit tests for the studied-template store
 * (driver/goodix55b4/goodix55b4-study.c). No valid Milan template can be
 * made without sensor frames, so these cover the store's own checks; a
 * well-formed entry holding an invalid template must still be rejected by
 * template validation.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>

#include "goodix55b4-study.h"
#include "milan/print.h"

typedef struct
{
  gchar  *dir;
  GBytes *enrolled;
  gchar  *key;
} Fixture;

static void
fixture_setup (Fixture *f, gconstpointer data)
{
  g_autoptr(GError) error = NULL;
  static const guint8 enrolled[] = "an enrolled template";

  f->dir = g_dir_make_tmp ("goodix55b4-study-XXXXXX", &error);
  g_assert_no_error (error);
  f->enrolled = g_bytes_new_static (enrolled, sizeof (enrolled));
  f->key = goodix55b4_study_key (f->enrolled);
}

static void
fixture_teardown (Fixture *f, gconstpointer data)
{
  g_autoptr(GDir) d = g_dir_open (f->dir, 0, NULL);
  const gchar *name;

  while (d && (name = g_dir_read_name (d)))
    {
      g_autofree gchar *p = g_build_filename (f->dir, name, NULL);
      g_unlink (p);
    }
  g_rmdir (f->dir);
  g_free (f->dir);
  g_free (f->key);
  g_bytes_unref (f->enrolled);
}

static gchar *
entry (Fixture *f, const char *key)
{
  g_autofree gchar *name = g_strconcat (key, ".bin", NULL);

  return g_build_filename (f->dir, name, NULL);
}

static void
save_junk (Fixture *f)
{
  static const guint8 junk[] = { 1, 2, 3, 4, 5, 6, 7, 8 };
  g_autoptr(GBytes) tpl = g_bytes_new_static (junk, sizeof (junk));
  g_autoptr(GError) error = NULL;

  g_assert_true (goodix55b4_study_save (f->dir, f->key, tpl, &error));
  g_assert_no_error (error);
}

static void
test_key (Fixture *f, gconstpointer data)
{
  g_assert_cmpuint (strlen (f->key), ==, 64);
  for (const char *c = f->key; *c; c++)
    g_assert_true (g_ascii_isxdigit (*c));
}

static void
test_missing (Fixture *f, gconstpointer data)
{
  g_autoptr(GError) error = NULL;

  g_assert_null (goodix55b4_study_load (f->dir, f->key, &error));
  g_assert_error (error, G_FILE_ERROR, G_FILE_ERROR_NOENT);
}

static void
test_invalid_template_rejected (Fixture *f, gconstpointer data)
{
  g_autoptr(GError) error = NULL;

  /* Envelope, key and digest are fine; the template itself is not. */
  save_junk (f);
  g_assert_null (goodix55b4_study_load (f->dir, f->key, &error));
  g_assert_error (error, GOODIX_MILAN_PRINT_ERROR,
                  GOODIX_MILAN_PRINT_ERROR_INVALID);
}

static void
test_corrupted (Fixture *f, gconstpointer data)
{
  g_autofree gchar *path = entry (f, f->key);
  g_autofree gchar *bytes = NULL;
  gsize len;
  g_autoptr(GError) error = NULL;

  save_junk (f);
  g_assert_true (g_file_get_contents (path, &bytes, &len, &error));
  bytes[len - 1] ^= 0x01; /* template byte: digest mismatch */
  g_assert_true (g_file_set_contents (path, bytes, len, &error));
  g_assert_null (goodix55b4_study_load (f->dir, f->key, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

static void
test_other_key (Fixture *f, gconstpointer data)
{
  static const guint8 other[] = "another enrolled template";
  g_autoptr(GBytes) other_bytes = g_bytes_new_static (other, sizeof (other));
  g_autofree gchar *other_key = goodix55b4_study_key (other_bytes);
  g_autofree gchar *from = entry (f, f->key);
  g_autofree gchar *to = entry (f, other_key);
  g_autoptr(GError) error = NULL;

  /* An entry renamed to another print's key must not be used for it. */
  save_junk (f);
  g_assert_cmpint (g_rename (from, to), ==, 0);
  g_assert_null (goodix55b4_study_load (f->dir, other_key, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

static void
test_bad_arguments (Fixture *f, gconstpointer data)
{
  g_autoptr(GBytes) huge = g_bytes_new_take (g_malloc0 (GOODIX_MILAN_PRINT_MAX_SIZE + 1),
                                             GOODIX_MILAN_PRINT_MAX_SIZE + 1);
  g_autoptr(GError) error = NULL;

  g_assert_false (goodix55b4_study_save (f->dir, "short-key", f->enrolled, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error (&error);
  g_assert_false (goodix55b4_study_save (f->dir, f->key, huge, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error (&error);
  g_assert_null (goodix55b4_study_load (f->dir, "short-key", &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
#define ADD(name, fn) \
  g_test_add ("/study/" name, Fixture, NULL, fixture_setup, fn, fixture_teardown)
  ADD ("key", test_key);
  ADD ("missing", test_missing);
  ADD ("invalid-template-rejected", test_invalid_template_rejected);
  ADD ("corrupted", test_corrupted);
  ADD ("other-key", test_other_key);
  ADD ("bad-arguments", test_bad_arguments);
#undef ADD
  return g_test_run ();
}
