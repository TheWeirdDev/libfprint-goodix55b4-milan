/*
 * Unit tests for the firmware-name gate (goodix55b4_firmware_supported in
 * driver/goodix55b4/goodix55b4.h). Some 27c6:55b4 units report
 * GF3208_RTSEC_APP_10042 instead of GF3268_RTSEC_APP_*; both must pass, and
 * anything else must be refused before the chip-ID check.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <glib.h>

#include "goodix55b4.h"

static void
test_accepts_gf3268 (void)
{
  g_assert_true (goodix55b4_firmware_supported ("GF3268_RTSEC_APP_10041"));
  g_assert_true (goodix55b4_firmware_supported ("GF3268_RTSEC_APP_"));
}

static void
test_accepts_gf3208 (void)
{
  g_assert_true (goodix55b4_firmware_supported ("GF3208_RTSEC_APP_10042"));
  g_assert_true (goodix55b4_firmware_supported ("GF3208_RTSEC_APP_"));
}

static void
test_rejects_other_names (void)
{
  g_assert_false (goodix55b4_firmware_supported (""));
  g_assert_false (goodix55b4_firmware_supported ("MILAN_RTSEC_IAP_10027"));
  g_assert_false (goodix55b4_firmware_supported ("GF3258_RTSEC_APP_10041"));
  g_assert_false (goodix55b4_firmware_supported ("GF3208_RTSEC_IAP_10042"));
  g_assert_false (goodix55b4_firmware_supported ("GF3208"));
}

static void
test_prefix_is_anchored_and_case_sensitive (void)
{
  g_assert_false (goodix55b4_firmware_supported (" GF3208_RTSEC_APP_10042"));
  g_assert_false (goodix55b4_firmware_supported ("XGF3268_RTSEC_APP_10041"));
  g_assert_false (goodix55b4_firmware_supported ("gf3208_rtsec_app_10042"));
}

static void
test_null_is_rejected (void)
{
  g_assert_false (goodix55b4_firmware_supported (NULL));
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/goodix55b4/firmware/gf3268", test_accepts_gf3268);
  g_test_add_func ("/goodix55b4/firmware/gf3208", test_accepts_gf3208);
  g_test_add_func ("/goodix55b4/firmware/other-names", test_rejects_other_names);
  g_test_add_func ("/goodix55b4/firmware/anchored",
                   test_prefix_is_anchored_and_case_sensitive);
  g_test_add_func ("/goodix55b4/firmware/null", test_null_is_rejected);
  return g_test_run ();
}
