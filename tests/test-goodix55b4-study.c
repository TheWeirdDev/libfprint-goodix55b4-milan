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

/* ---- policy ---- */

static GoodixMilanRuntimeOutput
match_output (GBytes *candidate, GoodixMilanStudyAction action, gint score)
{
  GoodixMilanRuntimeOutput out;

  memset (&out, 0, sizeof (out));
  out.status = GOODIX_MILAN_RUNTIME_MATCH;
  out.final_candidate = candidate;
  out.study_action = action;
  out.score = score;
  return out;
}

static void
test_should_store (void)
{
  static const guint8 a[] = { 1 }, b[] = { 2 };
  g_autoptr(GBytes) matched = g_bytes_new_static (a, sizeof (a));
  g_autoptr(GBytes) studied = g_bytes_new_static (b, sizeof (b));
  g_autoptr(GBytes) same = g_bytes_new_static (a, sizeof (a));
  GoodixMilanRuntimeOutput out;
  const Goodix55b4StudyPolicy N = GOODIX55B4_STUDY_NATIVE;
  const Goodix55b4StudyPolicy B = GOODIX55B4_STUDY_BOUNDED;
  const gint min = GOODIX55B4_STUDY_BOUNDED_MIN_SCORE;

  /* Never: off, no match, no candidate, unchanged. */
  out = match_output (studied, GOODIX_MILAN_STUDY_APPEND, 90);
  g_assert_false (goodix55b4_study_should_store (GOODIX55B4_STUDY_OFF, &out, matched));
  out.status = GOODIX_MILAN_RUNTIME_NO_MATCH;
  g_assert_false (goodix55b4_study_should_store (N, &out, matched));
  out = match_output (NULL, GOODIX_MILAN_STUDY_APPEND, 90);
  g_assert_false (goodix55b4_study_should_store (N, &out, matched));
  out = match_output (same, GOODIX_MILAN_STUDY_APPEND, 90);
  g_assert_false (goodix55b4_study_should_store (N, &out, matched));
  g_assert_false (goodix55b4_study_should_store (B, &out, matched));

  /* Native keeps every changed candidate, replacement included. */
  for (int action = GOODIX_MILAN_STUDY_NONE; action <= GOODIX_MILAN_STUDY_REPLACE; action++)
    {
      out = match_output (studied, action, 1);
      g_assert_true (goodix55b4_study_should_store (N, &out, matched));
    }

  /* Bounded: appends from confident matches only. */
  out = match_output (studied, GOODIX_MILAN_STUDY_APPEND, min);
  g_assert_true (goodix55b4_study_should_store (B, &out, matched));
  out = match_output (studied, GOODIX_MILAN_STUDY_APPEND, min - 1);
  g_assert_false (goodix55b4_study_should_store (B, &out, matched));
  for (int action = GOODIX_MILAN_STUDY_NONE; action <= GOODIX_MILAN_STUDY_REPLACE; action++)
    {
      if (action == GOODIX_MILAN_STUDY_APPEND)
        continue;
      out = match_output (studied, action, 100);
      g_assert_false (goodix55b4_study_should_store (B, &out, matched));
    }
}

static void
test_policy_from_env (void)
{
  static const struct { const char *policy, *no_study; Goodix55b4StudyPolicy want; } cases[] = {
    { NULL, NULL, GOODIX55B4_STUDY_NATIVE },
    { "", NULL, GOODIX55B4_STUDY_NATIVE },
    { "native", NULL, GOODIX55B4_STUDY_NATIVE },
    { "bounded", NULL, GOODIX55B4_STUDY_BOUNDED },
    { "off", NULL, GOODIX55B4_STUDY_OFF },
    { NULL, "1", GOODIX55B4_STUDY_OFF },
    { "bounded", "1", GOODIX55B4_STUDY_OFF }, /* NO_STUDY wins */
    { "bounded", "0", GOODIX55B4_STUDY_BOUNDED },
  };

  for (guint i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      if (cases[i].policy)
        g_setenv ("GOODIX55B4_STUDY_POLICY", cases[i].policy, TRUE);
      else
        g_unsetenv ("GOODIX55B4_STUDY_POLICY");
      if (cases[i].no_study)
        g_setenv ("GOODIX55B4_NO_STUDY", cases[i].no_study, TRUE);
      else
        g_unsetenv ("GOODIX55B4_NO_STUDY");
      g_assert_cmpint (goodix55b4_study_policy_from_env (), ==, cases[i].want);
    }

  g_setenv ("GOODIX55B4_STUDY_POLICY", "aggressive", TRUE);
  g_unsetenv ("GOODIX55B4_NO_STUDY");
  g_test_expect_message ("libfprint-goodix55b4", G_LOG_LEVEL_WARNING,
                         "unknown GOODIX55B4_STUDY_POLICY 'aggressive'*");
  g_assert_cmpint (goodix55b4_study_policy_from_env (), ==, GOODIX55B4_STUDY_NATIVE);
  g_test_assert_expected_messages ();
  g_unsetenv ("GOODIX55B4_STUDY_POLICY");
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
  g_test_add_func ("/study/should-store", test_should_store);
  g_test_add_func ("/study/policy-from-env", test_policy_from_env);
  return g_test_run ();
}
