/*
 * Unit tests for the PSK key file (driver/goodix55b4/goodix55b4-secrets.c):
 * parsing, and the fall-back to the zero PSK when the file is missing or
 * malformed. Build and run: scripts/run-tests.sh
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "goodix55b4.h"
#include "goodix55b4-secrets.h"

#define LOG_DOMAIN "libfprint-goodix55b4"

/* 00 01 .. 1f and a0 a1 .. bf */
#define PSK_HEX  "000102030405060708090a0b0c0d0e0f" \
  "101112131415161718191a1b1c1d1e1f"
#define HASH_HEX "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf" \
  "b0b1b2b3b4b5b6b7b8b9babbbcbdbebf"

typedef struct
{
  char *dir;
  char *path;
} Fixture;

static void
fixture_setup (Fixture *f, gconstpointer data)
{
  g_autoptr(GError) error = NULL;

  f->dir = g_dir_make_tmp ("goodix55b4-secrets-XXXXXX", &error);
  g_assert_no_error (error);
  f->path = g_build_filename (f->dir, "psk.hex", NULL);
}

static void
fixture_teardown (Fixture *f, gconstpointer data)
{
  g_unlink (f->path);
  g_rmdir (f->dir);
  g_free (f->path);
  g_free (f->dir);
}

static void
write_file (Fixture *f, const char *contents)
{
  g_autoptr(GError) error = NULL;

  g_file_set_contents (f->path, contents, -1, &error);
  g_assert_no_error (error);
}

static void
assert_expected_key (const guint8 *psk, const guint8 *hash)
{
  for (guint i = 0; i < GOODIX55B4_PSK_LEN; i++)
    {
      g_assert_cmpuint (psk[i], ==, i);
      g_assert_cmpuint (hash[i], ==, 0xa0 + i);
    }
}

static void
assert_zero_key (const guint8 *psk, const guint8 *hash)
{
  static const guint8 zero[GOODIX55B4_PSK_LEN] = { 0 };

  g_assert_cmpmem (psk, GOODIX55B4_PSK_LEN, zero, sizeof (zero));
  g_assert_cmpmem (hash, GOODIX55B4_PSK_LEN, goodix55b4_zero_psk_hash,
                   sizeof (goodix55b4_zero_psk_hash));
}

/* A file that fails to load leaves the caller's buffers untouched. */
static void
assert_rejected (Fixture *f, const char *contents)
{
  g_autoptr(GError) error = NULL;
  guint8 psk[GOODIX55B4_PSK_LEN], hash[GOODIX55B4_PSK_LEN];

  memset (psk, 0x5a, sizeof (psk));
  memset (hash, 0x5a, sizeof (hash));
  write_file (f, contents);
  g_assert_false (goodix55b4_psk_load_file (f->path, psk, hash, &error));
  g_assert_error (error, G_FILE_ERROR, G_FILE_ERROR_INVAL);
  for (guint i = 0; i < GOODIX55B4_PSK_LEN; i++)
    {
      g_assert_cmpuint (psk[i], ==, 0x5a);
      g_assert_cmpuint (hash[i], ==, 0x5a);
    }
}

static void
assert_loads (Fixture *f, const char *contents)
{
  g_autoptr(GError) error = NULL;
  guint8 psk[GOODIX55B4_PSK_LEN], hash[GOODIX55B4_PSK_LEN];

  write_file (f, contents);
  g_assert_true (goodix55b4_psk_load_file (f->path, psk, hash, &error));
  g_assert_no_error (error);
  assert_expected_key (psk, hash);
}

static void
test_load_valid (Fixture *f, gconstpointer data)
{
  /* As written by tools/recover_windows_psk.py. */
  assert_loads (f, PSK_HEX "\n" HASH_HEX "\n");
}

static void
test_load_variants (Fixture *f, gconstpointer data)
{
  assert_loads (f, PSK_HEX "\n" HASH_HEX);                 /* no final newline */
  assert_loads (f, PSK_HEX "\r\n" HASH_HEX "\r\n");        /* CRLF */
  assert_loads (f, PSK_HEX "\n" HASH_HEX "\nextra line\n"); /* trailing text */
  assert_loads (f, "  " PSK_HEX "  \n\t" HASH_HEX "\n");   /* padding */
  /* Upper case. */
  assert_loads (f, "000102030405060708090A0B0C0D0E0F"
                "101112131415161718191A1B1C1D1E1F\n"
                "A0A1A2A3A4A5A6A7A8A9AAABACADAEAF"
                "B0B1B2B3B4B5B6B7B8B9BABBBCBDBEBF\n");
}

static void
test_load_malformed (Fixture *f, gconstpointer data)
{
  assert_rejected (f, "");
  assert_rejected (f, "\n\n");
  assert_rejected (f, PSK_HEX "\n");                       /* hash missing */
  assert_rejected (f, PSK_HEX);                            /* one line */
  assert_rejected (f, PSK_HEX "00\n" HASH_HEX "\n");       /* 33 bytes */
  assert_rejected (f, "0001\n" HASH_HEX "\n");             /* 2 bytes */
  assert_rejected (f, PSK_HEX "\n" "b0" HASH_HEX "\n");    /* long hash */
  assert_rejected (f, "0" PSK_HEX "\n" HASH_HEX "\n");     /* odd digits */
  assert_rejected (f, "zz" PSK_HEX "\n" HASH_HEX "\n");    /* non-hex */
  /* A nibble pair split by a space. */
  assert_rejected (f, "0 0" PSK_HEX "\n" HASH_HEX "\n");
  /* PSK and hash on one line. */
  assert_rejected (f, PSK_HEX HASH_HEX "\n" HASH_HEX "\n");
}

static void
test_load_missing (Fixture *f, gconstpointer data)
{
  g_autoptr(GError) error = NULL;
  guint8 psk[GOODIX55B4_PSK_LEN], hash[GOODIX55B4_PSK_LEN];

  g_assert_false (goodix55b4_psk_load_file (f->path, psk, hash, &error));
  g_assert_error (error, G_FILE_ERROR, G_FILE_ERROR_NOENT);
}

static void
test_select_valid (Fixture *f, gconstpointer data)
{
  guint8 psk[GOODIX55B4_PSK_LEN], hash[GOODIX55B4_PSK_LEN];

  write_file (f, PSK_HEX "\n" HASH_HEX "\n");
  g_assert_true (goodix55b4_psk_select (f->path, FALSE, psk, hash));
  assert_expected_key (psk, hash);
  g_assert_true (goodix55b4_psk_select (f->path, TRUE, psk, hash));
  assert_expected_key (psk, hash);
}

/* No file at the default path: the zero PSK, without a warning (fatal in
 * GTest, so a warning would fail the test). */
static void
test_select_default_missing (Fixture *f, gconstpointer data)
{
  guint8 psk[GOODIX55B4_PSK_LEN], hash[GOODIX55B4_PSK_LEN];

  memset (psk, 0x5a, sizeof (psk));
  memset (hash, 0x5a, sizeof (hash));
  g_assert_false (goodix55b4_psk_select (f->path, FALSE, psk, hash));
  assert_zero_key (psk, hash);
}

/* A path named by the environment that does not exist is a mistake. */
static void
test_select_explicit_missing (Fixture *f, gconstpointer data)
{
  guint8 psk[GOODIX55B4_PSK_LEN], hash[GOODIX55B4_PSK_LEN];

  g_test_expect_message (LOG_DOMAIN, G_LOG_LEVEL_WARNING,
                         "ignoring PSK file *; falling back to the zero PSK");
  g_assert_false (goodix55b4_psk_select (f->path, TRUE, psk, hash));
  g_test_assert_expected_messages ();
  assert_zero_key (psk, hash);
}

static void
test_select_malformed (Fixture *f, gconstpointer data)
{
  guint8 psk[GOODIX55B4_PSK_LEN], hash[GOODIX55B4_PSK_LEN];

  /* A valid PSK with a short hash: nothing of it may be used. */
  write_file (f, PSK_HEX "\n" "a0a1\n");
  memset (psk, 0x5a, sizeof (psk));
  g_test_expect_message (LOG_DOMAIN, G_LOG_LEVEL_WARNING,
                         "ignoring PSK file *expected two lines*");
  g_assert_false (goodix55b4_psk_select (f->path, FALSE, psk, hash));
  g_test_assert_expected_messages ();
  assert_zero_key (psk, hash);
}

/* The cached accessors read GOODIX55B4_PSK_FILE once per process. */
static void
test_accessors_use_env (Fixture *f, gconstpointer data)
{
  write_file (f, PSK_HEX "\n" HASH_HEX "\n");
  g_setenv ("GOODIX55B4_PSK_FILE", f->path, TRUE);
  g_assert_true (goodix55b4_psk_is_custom ());
  assert_expected_key (goodix55b4_psk (), goodix55b4_psk_hash ());

  /* Cached: a later change of the file is not seen. */
  g_unlink (f->path);
  g_assert_true (goodix55b4_psk_is_custom ());
  assert_expected_key (goodix55b4_psk (), goodix55b4_psk_hash ());
  g_unsetenv ("GOODIX55B4_PSK_FILE");
}

static void
add (const char *name, void (*fn)(Fixture *, gconstpointer))
{
  g_test_add (name, Fixture, NULL, fixture_setup, fn, fixture_teardown);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  add ("/goodix55b4-secrets/load-valid", test_load_valid);
  add ("/goodix55b4-secrets/load-variants", test_load_variants);
  add ("/goodix55b4-secrets/load-malformed", test_load_malformed);
  add ("/goodix55b4-secrets/load-missing", test_load_missing);
  add ("/goodix55b4-secrets/select-valid", test_select_valid);
  add ("/goodix55b4-secrets/select-default-missing",
       test_select_default_missing);
  add ("/goodix55b4-secrets/select-explicit-missing",
       test_select_explicit_missing);
  add ("/goodix55b4-secrets/select-malformed", test_select_malformed);
  add ("/goodix55b4-secrets/accessors-use-env", test_accessors_use_env);
  return g_test_run ();
}
