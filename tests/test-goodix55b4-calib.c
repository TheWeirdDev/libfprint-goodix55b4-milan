/*
 * Unit tests for the OTP calibration and config build
 * (driver/goodix55b4/goodix55b4-calib.c).
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <glib.h>
#include <string.h>

#include "goodix55b4-calib.h"

#define TEMPLATE_TCODE (0x0080)
#define TEMPLATE_DELTA (0x15)

static void
otp_with_code (guint8 otp[32], guint8 code, guint8 check)
{
  memset (otp, 0x5a, 32);
  otp[22] = code;
  otp[23] = check;
}

static void
assert_template (const Goodix55b4Calib *c)
{
  g_assert_false (c->from_otp);
  g_assert_cmpuint (c->tcode, ==, TEMPLATE_TCODE);
  g_assert_cmpuint (c->fdt_delta, ==, TEMPLATE_DELTA);
}

static void
test_otp_unusable (void)
{
  Goodix55b4Calib c;
  guint8 otp[32];

  goodix55b4_calib_from_otp (NULL, 0, &c);
  assert_template (&c);

  otp_with_code (otp, 0x35, 0xca);
  goodix55b4_calib_from_otp (otp, 23, &c); /* too short for byte 23 */
  assert_template (&c);

  otp_with_code (otp, 0x00, 0xff); /* code 0 */
  goodix55b4_calib_from_otp (otp, sizeof (otp), &c);
  assert_template (&c);

  otp_with_code (otp, 0x35, 0xcb); /* byte 23 not the complement */
  goodix55b4_calib_from_otp (otp, sizeof (otp), &c);
  assert_template (&c);

  /* Delta out of range: 0x0f gives tcode 0x10, delta 0x236. */
  otp_with_code (otp, 0x0f, 0xf0);
  goodix55b4_calib_from_otp (otp, sizeof (otp), &c);
  assert_template (&c);
}

static void
test_otp_values (void)
{
  /* code -> (tcode, delta): tcode = (hi + 1) << 4,
   * delta = ((lo + 2) * 0x6400 / tcode / 3) >> 4 */
  static const struct { guint8 code; guint16 tcode; guint8 delta; } cases[] = {
    { 0x35, 0x40, 0x3a },
    { 0x47, 0x50, 0x3c },
    { 0xf0, 0x100, 0x04 }, /* smallest accepted delta */
  };

  for (guint i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      Goodix55b4Calib c;
      guint8 otp[32];

      otp_with_code (otp, cases[i].code, 0xff - cases[i].code);
      goodix55b4_calib_from_otp (otp, sizeof (otp), &c);
      g_assert_true (c.from_otp);
      g_assert_cmpuint (c.tcode, ==, cases[i].tcode);
      g_assert_cmpuint (c.fdt_delta, ==, cases[i].delta);
    }
}

/* Value of the first `tag` entry in config section `section`, or -1. */
static gint
config_tag (const guint8 *cfg, guint section, guint16 tag)
{
  guint base = cfg[1 + section * 2];
  guint size = cfg[2 + section * 2];

  for (guint e = base; e + 4 <= base + size && e + 4 <= GOODIX55B4_CONFIG_SIZE - 2; e += 4)
    if ((cfg[e] | (cfg[e + 1] << 8)) == tag)
      return cfg[e + 2] | (cfg[e + 3] << 8);
  return -1;
}

static void
assert_checksum (const guint8 *cfg)
{
  guint16 sum = 0xa5a5;

  for (guint i = 0; i < GOODIX55B4_CONFIG_SIZE; i += 2)
    sum += cfg[i] | (cfg[i + 1] << 8);
  g_assert_cmpuint (sum, ==, 0);
}

static void
test_config_build (void)
{
  Goodix55b4Calib tmpl, otp_calib;
  guint8 a[GOODIX55B4_CONFIG_SIZE], b[GOODIX55B4_CONFIG_SIZE];
  guint8 otp[32];
  guint diffs = 0;

  goodix55b4_calib_from_otp (NULL, 0, &tmpl);
  otp_with_code (otp, 0x35, 0xca);
  goodix55b4_calib_from_otp (otp, sizeof (otp), &otp_calib);
  goodix55b4_config_build (&tmpl, a);
  goodix55b4_config_build (&otp_calib, b);

  assert_checksum (a);
  assert_checksum (b);

  g_assert_cmpint (config_tag (a, 4, 0x5c), ==, TEMPLATE_TCODE);
  g_assert_cmpint (config_tag (a, 2, 0x82), ==, (TEMPLATE_DELTA << 8) | 0x80);
  g_assert_cmpint (config_tag (b, 2, 0x5c), ==, 0x40);
  g_assert_cmpint (config_tag (b, 4, 0x5c), ==, 0x40);
  g_assert_cmpint (config_tag (b, 2, 0x82), ==, (0x3a << 8) | 0x80);

  /* Nothing but the calibration entries and the checksum changes. */
  for (guint i = 0; i < GOODIX55B4_CONFIG_SIZE - 2; i++)
    if (a[i] != b[i])
      diffs++;
  g_assert_cmpuint (diffs, >, 0);
  g_assert_cmpuint (diffs, <=, 6); /* 3 tag values, 2 bytes each */
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/calib/otp-unusable", test_otp_unusable);
  g_test_add_func ("/calib/otp-values", test_otp_values);
  g_test_add_func ("/calib/config-build", test_config_build);
  return g_test_run ();
}
