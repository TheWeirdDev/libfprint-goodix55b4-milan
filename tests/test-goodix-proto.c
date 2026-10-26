/*
 * Unit tests for the USB framing (driver/goodix55b4/goodix_proto.c): pack
 * and protocol encode/decode, and the pack reader that reassembles packs
 * from IN transfers. Expected bytes follow tools/capture_dataset_55b4.py.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <glib.h>
#include <string.h>

#include "goodix_proto.h"

/* Command 0xa8 (firmware version) with payload 00 00, as the device gets it:
 * pack a0 | len 06 00 | sum a6, then cmd a8 | len 03 00 | 00 00 | ff. */
static const guint8 fw_request[] = {
  0xa0, 0x06, 0x00, 0xa6, 0xa8, 0x03, 0x00, 0x00, 0x00, 0xff,
};

static void
test_encode_matches_reference (void)
{
  static const guint8 payload[] = { 0x00, 0x00 };
  g_autofree guint8 *proto = NULL;
  g_autofree guint8 *pack = NULL;
  guint32 proto_len, pack_len;

  goodix_encode_protocol (0xa8, (guint8 *) payload, sizeof (payload), TRUE,
                          FALSE, &proto, &proto_len);
  g_assert_cmpmem (proto, proto_len, fw_request + 4, sizeof (fw_request) - 4);

  goodix_encode_pack (GOODIX_FLAGS_MSG_PROTOCOL, proto, proto_len, TRUE, &pack,
                      &pack_len);
  /* Padded to the 64-byte OUT endpoint size with zeros. */
  g_assert_cmpuint (pack_len, ==, GOODIX_EP_OUT_MAX_BUF_SIZE);
  g_assert_cmpmem (pack, sizeof (fw_request), fw_request, sizeof (fw_request));
  for (guint i = sizeof (fw_request); i < pack_len; i++)
    g_assert_cmpuint (pack[i], ==, 0);
}

static void
test_encode_null_checksum (void)
{
  static const guint8 payload[] = { 0x01, 0x02, 0x03 };
  g_autofree guint8 *proto = NULL;
  g_autofree guint8 *decoded = NULL;
  guint32 len;
  guint16 decoded_len;
  guint8 cmd;
  gboolean valid, valid_null;

  goodix_encode_protocol (0x20, (guint8 *) payload, sizeof (payload), FALSE,
                          FALSE, &proto, &len);
  g_assert_cmpuint (proto[len - 1], ==, GOODIX_NULL_CHECKSUM);
  g_assert_true (goodix_decode_protocol (proto, len, &cmd, &decoded,
                                         &decoded_len, &valid, &valid_null));
  g_assert_cmpuint (cmd, ==, 0x20);
  g_assert_cmpmem (decoded, decoded_len, payload, sizeof (payload));
  g_assert_true (valid_null);
}

static void
test_protocol_roundtrip (void)
{
  for (guint n = 0; n <= 300; n += 37)
    {
      g_autofree guint8 *payload = g_malloc (n + 1);
      g_autofree guint8 *proto = NULL;
      g_autofree guint8 *decoded = NULL;
      guint32 len;
      guint16 decoded_len;
      guint8 cmd;
      gboolean valid, valid_null;

      for (guint i = 0; i < n; i++)
        payload[i] = i * 7;
      goodix_encode_protocol (0x82, payload, n, TRUE, FALSE, &proto, &len);
      g_assert_true (goodix_decode_protocol (proto, len, &cmd, &decoded,
                                             &decoded_len, &valid,
                                             &valid_null));
      g_assert_cmpuint (cmd, ==, 0x82);
      g_assert_cmpuint (decoded_len, ==, n);
      if (n)
        g_assert_cmpmem (decoded, decoded_len, payload, n);
      g_assert_true (valid);

      /* A flipped payload byte breaks the checksum. */
      if (n)
        {
          g_autofree guint8 *again = NULL;

          proto[3] ^= 0x10;
          g_assert_true (goodix_decode_protocol (proto, len, &cmd, &again,
                                                 &decoded_len, &valid,
                                                 &valid_null));
          g_assert_false (valid);
        }
    }
}

static void
test_decode_rejects_short (void)
{
  /* Protocol length field 0 would underflow to 0xffff. */
  static const guint8 zero_len[] = { 0x20, 0x00, 0x00, 0x00 };
  static const guint8 long_len[] = { 0x20, 0x10, 0x00, 0x00, 0x00 };
  g_autofree guint8 *payload = NULL;
  guint16 payload_len;
  guint8 cmd, flags;
  gboolean valid, valid_null;

  g_assert_false (goodix_decode_protocol ((guint8 *) zero_len, sizeof (zero_len),
                                          &cmd, &payload, &payload_len, &valid,
                                          &valid_null));
  g_assert_false (goodix_decode_protocol ((guint8 *) long_len, sizeof (long_len),
                                          &cmd, &payload, &payload_len, &valid,
                                          &valid_null));
  g_assert_false (goodix_decode_protocol ((guint8 *) long_len, 2, &cmd,
                                          &payload, &payload_len, &valid,
                                          &valid_null));
  /* Pack header announcing more than is there. */
  g_assert_false (goodix_decode_pack ((guint8 *) fw_request, 9, &flags,
                                      &payload, &payload_len, &valid));
  g_assert_false (goodix_decode_pack ((guint8 *) fw_request, 3, &flags,
                                      &payload, &payload_len, &valid));
  g_assert_null (payload);
}

/* ---- pack reader ---- */

static GBytes *
make_pack (guint8 flags, const guint8 *payload, guint16 len)
{
  guint8 *data;
  guint32 data_len;

  goodix_encode_pack (flags, (guint8 *) payload, len, FALSE, &data, &data_len);
  return g_bytes_new_take (data, data_len);
}

static void
push_bytes (GoodixPackReader *r, GBytes *b)
{
  goodix_pack_reader_push (r, g_bytes_get_data (b, NULL), g_bytes_get_size (b));
}

static void
expect_pack (GoodixPackReader *r, guint8 flags, const guint8 *payload,
             guint16 len)
{
  g_autofree guint8 *got = NULL;
  guint8 got_flags;
  guint16 got_len;
  gboolean valid;
  gsize dropped;

  g_assert_cmpint (goodix_pack_reader_next (r, &got_flags, &got, &got_len,
                                            &valid, &dropped), ==,
                   GOODIX_PACK_READY);
  g_assert_cmpuint (got_flags, ==, flags);
  g_assert_cmpmem (got, got_len, payload, len);
  g_assert_true (valid);
}

static GoodixPackStatus
reader_next (GoodixPackReader *r, gsize *dropped)
{
  g_autofree guint8 *payload = NULL;
  guint8 flags;
  guint16 len;
  gboolean valid;

  return goodix_pack_reader_next (r, &flags, &payload, &len, &valid, dropped);
}

static void
test_reader_split (void)
{
  static const guint8 payload[] = "a TLS record split over transfers";
  g_autoptr(GBytes) pack = make_pack (GOODIX_FLAGS_TLS, payload, sizeof (payload));
  const guint8 *p = g_bytes_get_data (pack, NULL);
  gsize n = g_bytes_get_size (pack);
  GoodixPackReader r;
  gsize dropped;

  goodix_pack_reader_init (&r);
  for (gsize i = 0; i + 1 < n; i++)
    {
      goodix_pack_reader_push (&r, p + i, 1);
      g_assert_cmpint (reader_next (&r, &dropped), ==, GOODIX_PACK_INCOMPLETE);
    }
  goodix_pack_reader_push (&r, p + n - 1, 1);
  expect_pack (&r, GOODIX_FLAGS_TLS, payload, sizeof (payload));
  g_assert_cmpuint (goodix_pack_reader_buffered (&r), ==, 0);
  goodix_pack_reader_clear (&r);
}

static void
test_reader_two_packs_one_transfer (void)
{
  static const guint8 a[] = { 0xa8, 0x03, 0x00, 0x01, 0x02, 0x55 };
  static const guint8 b[] = { 0x17, 0x03, 0x03, 0x00, 0x01, 0x42 };
  g_autoptr(GBytes) pa = make_pack (GOODIX_FLAGS_MSG_PROTOCOL, a, sizeof (a));
  g_autoptr(GBytes) pb = make_pack (GOODIX_FLAGS_TLS_DATA, b, sizeof (b));
  GoodixPackReader r;
  gsize dropped;

  goodix_pack_reader_init (&r);
  /* Both packs and part of a third in one transfer: the old code kept the
   * first pack only. */
  push_bytes (&r, pa);
  push_bytes (&r, pb);
  goodix_pack_reader_push (&r, g_bytes_get_data (pa, NULL), 2);
  expect_pack (&r, GOODIX_FLAGS_MSG_PROTOCOL, a, sizeof (a));
  expect_pack (&r, GOODIX_FLAGS_TLS_DATA, b, sizeof (b));
  g_assert_cmpint (reader_next (&r, &dropped), ==, GOODIX_PACK_INCOMPLETE);
  g_assert_cmpuint (goodix_pack_reader_buffered (&r), ==, 2);
  goodix_pack_reader_push (&r, (const guint8 *) g_bytes_get_data (pa, NULL) + 2,
                           g_bytes_get_size (pa) - 2);
  expect_pack (&r, GOODIX_FLAGS_MSG_PROTOCOL, a, sizeof (a));
  goodix_pack_reader_clear (&r);
}

static void
test_reader_zero_padding (void)
{
  static const guint8 a[] = { 0xb0, 0x03, 0x00, 0x00, 0x00, 0x00 };
  g_autoptr(GBytes) pa = make_pack (GOODIX_FLAGS_MSG_PROTOCOL, a, sizeof (a));
  static const guint8 zeros[64] = { 0 };
  GoodixPackReader r;
  gsize dropped = 99;

  goodix_pack_reader_init (&r);
  push_bytes (&r, pa);
  goodix_pack_reader_push (&r, zeros, sizeof (zeros));
  expect_pack (&r, GOODIX_FLAGS_MSG_PROTOCOL, a, sizeof (a));
  /* Padding is dropped silently. */
  g_assert_cmpint (reader_next (&r, &dropped), ==, GOODIX_PACK_INCOMPLETE);
  g_assert_cmpuint (dropped, ==, 0);
  g_assert_cmpuint (goodix_pack_reader_buffered (&r), ==, 0);
  goodix_pack_reader_clear (&r);
}

static void
test_reader_resync (void)
{
  static const guint8 garbage[] = { 0xff, 0x13, 0x00, 0x37 };
  static const guint8 a[] = { 0x01, 0x02, 0x03 };
  g_autoptr(GBytes) pa = make_pack (GOODIX_FLAGS_TLS, a, sizeof (a));
  GoodixPackReader r;
  gsize dropped = 0;

  goodix_pack_reader_init (&r);
  goodix_pack_reader_push (&r, garbage, sizeof (garbage));
  push_bytes (&r, pa);
  g_assert_cmpint (reader_next (&r, &dropped), ==, GOODIX_PACK_DROPPED);
  g_assert_cmpuint (dropped, ==, sizeof (garbage));
  expect_pack (&r, GOODIX_FLAGS_TLS, a, sizeof (a));
  goodix_pack_reader_clear (&r);
}

static void
test_reader_reset_drops_partial_reply (void)
{
  static const guint8 a[] = { 0x11, 0x22, 0x33, 0x44 };
  static const guint8 b[] = { 0x55 };
  g_autoptr(GBytes) pa = make_pack (GOODIX_FLAGS_TLS_DATA, a, sizeof (a));
  g_autoptr(GBytes) pb = make_pack (GOODIX_FLAGS_MSG_PROTOCOL, b, sizeof (b));
  GoodixPackReader r;
  gsize dropped;

  goodix_pack_reader_init (&r);
  /* Half of the reply to a command that is then aborted... */
  goodix_pack_reader_push (&r, g_bytes_get_data (pa, NULL), 5);
  g_assert_cmpint (reader_next (&r, &dropped), ==, GOODIX_PACK_INCOMPLETE);
  goodix_pack_reader_reset (&r);
  /* ...must not be taken for the start of the next reply. */
  push_bytes (&r, pb);
  expect_pack (&r, GOODIX_FLAGS_MSG_PROTOCOL, b, sizeof (b));
  goodix_pack_reader_clear (&r);
}

/* A pack with a corrupted header checksum, then a good one. */
static void
push_bad_then_good (GoodixPackReader *r, GBytes *good)
{
  static const guint8 a[] = { 0x01, 0x02, 0x03 };
  g_autoptr(GBytes) pa = make_pack (GOODIX_FLAGS_TLS, a, sizeof (a));
  g_autofree guint8 *bad = g_memdup2 (g_bytes_get_data (pa, NULL),
                                      g_bytes_get_size (pa));

  bad[3] ^= 0xff;
  goodix_pack_reader_push (r, bad, g_bytes_get_size (pa));
  push_bytes (r, good);
}

static void
test_reader_bad_header_checksum_strict (void)
{
  static const guint8 b[] = { 0x42, 0x43 };
  g_autoptr(GBytes) pb = make_pack (GOODIX_FLAGS_MSG_PROTOCOL, b, sizeof (b));
  GoodixPackReader r;
  gsize dropped = 0;

  goodix_pack_reader_init (&r);
  g_assert_false (r.lax_checksum);
  push_bad_then_good (&r, pb);
  /* The bad pack is skipped as garbage (header + 3-byte body = 7 bytes;
   * no byte inside it starts a plausible pack). */
  g_assert_cmpint (reader_next (&r, &dropped), ==, GOODIX_PACK_DROPPED);
  g_assert_cmpuint (dropped, ==, 7);
  expect_pack (&r, GOODIX_FLAGS_MSG_PROTOCOL, b, sizeof (b));
  goodix_pack_reader_clear (&r);
}

static void
test_reader_bad_header_checksum_lax (void)
{
  static const guint8 b[] = { 0x42 };
  g_autoptr(GBytes) pb = make_pack (GOODIX_FLAGS_MSG_PROTOCOL, b, sizeof (b));
  g_autofree guint8 *payload = NULL;
  GoodixPackReader r;
  guint8 flags;
  guint16 len;
  gboolean valid = TRUE;
  gsize dropped;

  goodix_pack_reader_init (&r);
  r.lax_checksum = TRUE;
  push_bad_then_good (&r, pb);
  g_assert_cmpint (goodix_pack_reader_next (&r, &flags, &payload, &len, &valid,
                                            &dropped), ==, GOODIX_PACK_READY);
  g_assert_cmpuint (flags, ==, GOODIX_FLAGS_TLS);
  g_assert_false (valid);
  expect_pack (&r, GOODIX_FLAGS_MSG_PROTOCOL, b, sizeof (b));
  goodix_pack_reader_clear (&r);
}

static void
test_reader_header_split_before_checksum (void)
{
  static const guint8 a[] = { 0x07 };
  g_autoptr(GBytes) pa = make_pack (GOODIX_FLAGS_TLS_DATA, a, sizeof (a));
  const guint8 *p = g_bytes_get_data (pa, NULL);
  GoodixPackReader r;
  gsize dropped;

  /* Fewer than 4 bytes: the checksum cannot be judged yet, so wait. */
  goodix_pack_reader_init (&r);
  goodix_pack_reader_push (&r, p, 3);
  g_assert_cmpint (reader_next (&r, &dropped), ==, GOODIX_PACK_INCOMPLETE);
  g_assert_cmpuint (goodix_pack_reader_buffered (&r), ==, 3);
  goodix_pack_reader_push (&r, p + 3, g_bytes_get_size (pa) - 3);
  expect_pack (&r, GOODIX_FLAGS_TLS_DATA, a, sizeof (a));
  goodix_pack_reader_clear (&r);
}

/* ---- 0xe4 preset PSK read reply ---- */

static void
test_psk_read_reply (void)
{
  /* status 00 | flags 07 00 02 bb | length 03 00 00 00 | psk aa bb cc */
  static const guint8 ok[] = { 0x00, 0x07, 0x00, 0x02, 0xbb, 0x03, 0x00, 0x00,
                               0x00, 0xaa, 0xbb, 0xcc };
  static const guint8 failed[] = { 0x01 };
  guint8 bad[sizeof (ok)];
  const guint8 *psk = NULL;
  guint32 flags = 0, psk_len = 0;

  g_assert_cmpint (goodix_parse_preset_psk_read (ok, sizeof (ok), &flags, &psk,
                                                 &psk_len), ==, GOODIX_PSK_READ_OK);
  g_assert_cmphex (flags, ==, 0xbb020007);
  g_assert_cmpmem (psk, psk_len, ok + 9, 3);

  /* A 3-byte PSK was rejected by the old 13-byte header check. */
  g_assert_cmpuint (sizeof (ok), <, 13);

  g_assert_cmpint (goodix_parse_preset_psk_read (failed, sizeof (failed), &flags,
                                                 &psk, &psk_len), ==,
                   GOODIX_PSK_READ_FAILED);
  g_assert_cmpint (goodix_parse_preset_psk_read (ok, 0, &flags, &psk, &psk_len),
                   ==, GOODIX_PSK_READ_INVALID);
  g_assert_cmpint (goodix_parse_preset_psk_read (ok, 8, &flags, &psk, &psk_len),
                   ==, GOODIX_PSK_READ_INVALID);
  /* Length field disagreeing with the reply. */
  memcpy (bad, ok, sizeof (ok));
  bad[5] = 0x04;
  g_assert_cmpint (goodix_parse_preset_psk_read (bad, sizeof (bad), &flags, &psk,
                                                 &psk_len), ==,
                   GOODIX_PSK_READ_INVALID);
  bad[5] = 0xff;
  bad[8] = 0xff; /* huge: must not wrap */
  g_assert_cmpint (goodix_parse_preset_psk_read (bad, sizeof (bad), &flags, &psk,
                                                 &psk_len), ==,
                   GOODIX_PSK_READ_INVALID);
}

/* ---- image CRC ---- */

#define PACKED (108 * 88 / 4 * 6)

static void
test_crc32_mpeg2 (void)
{
  /* The CRC-32/MPEG-2 check value. */
  g_assert_cmphex (goodix_crc32_mpeg2 ((const guint8 *) "123456789", 9), ==,
                   0x0376e6e7);
  g_assert_cmphex (goodix_crc32_mpeg2 (NULL, 0), ==, 0xffffffff);
}

static void
test_image_crc (void)
{
  g_autofree guint8 *img = g_malloc (PACKED + 4);
  guint32 crc;

  for (guint i = 0; i < PACKED; i++)
    img[i] = (guint8) (i * 131 + 17);
  crc = goodix_crc32_mpeg2 (img, PACKED);
  /* As sent by the sensor: crc 9e30b1d1 arrives as b1 d1 9e 30. */
  img[PACKED + 0] = crc >> 8;
  img[PACKED + 1] = crc;
  img[PACKED + 2] = crc >> 24;
  img[PACKED + 3] = crc >> 16;
  g_assert_cmpint (goodix_image_check_crc (img, PACKED + 4, PACKED), ==,
                   GOODIX_IMAGE_CRC_OK);

  /* No trailer: accepted by the caller as before. */
  g_assert_cmpint (goodix_image_check_crc (img, PACKED, PACKED), ==,
                   GOODIX_IMAGE_CRC_ABSENT);
  g_assert_cmpint (goodix_image_check_crc (img, PACKED + 3, PACKED), ==,
                   GOODIX_IMAGE_CRC_ABSENT);

  /* One flipped bit in the pixels or the trailer. */
  img[1234] ^= 0x01;
  g_assert_cmpint (goodix_image_check_crc (img, PACKED + 4, PACKED), ==,
                   GOODIX_IMAGE_CRC_MISMATCH);
  img[1234] ^= 0x01;
  img[PACKED + 2] ^= 0x80;
  g_assert_cmpint (goodix_image_check_crc (img, PACKED + 4, PACKED), ==,
                   GOODIX_IMAGE_CRC_MISMATCH);

  /* The other plausible byte orders do not pass. */
  img[PACKED + 0] = crc;
  img[PACKED + 1] = crc >> 8;
  img[PACKED + 2] = crc >> 16;
  img[PACKED + 3] = crc >> 24;
  g_assert_cmpint (goodix_image_check_crc (img, PACKED + 4, PACKED), ==,
                   GOODIX_IMAGE_CRC_MISMATCH);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/proto/encode-matches-reference",
                   test_encode_matches_reference);
  g_test_add_func ("/proto/encode-null-checksum", test_encode_null_checksum);
  g_test_add_func ("/proto/protocol-roundtrip", test_protocol_roundtrip);
  g_test_add_func ("/proto/decode-rejects-short", test_decode_rejects_short);
  g_test_add_func ("/proto/reader/split", test_reader_split);
  g_test_add_func ("/proto/reader/two-packs-one-transfer",
                   test_reader_two_packs_one_transfer);
  g_test_add_func ("/proto/reader/zero-padding", test_reader_zero_padding);
  g_test_add_func ("/proto/reader/resync", test_reader_resync);
  g_test_add_func ("/proto/reader/reset-drops-partial-reply",
                   test_reader_reset_drops_partial_reply);
  g_test_add_func ("/proto/reader/bad-header-checksum-strict",
                   test_reader_bad_header_checksum_strict);
  g_test_add_func ("/proto/reader/bad-header-checksum-lax",
                   test_reader_bad_header_checksum_lax);
  g_test_add_func ("/proto/reader/header-split-before-checksum",
                   test_reader_header_split_before_checksum);
  g_test_add_func ("/proto/psk-read-reply", test_psk_read_reply);
  g_test_add_func ("/proto/crc32-mpeg2", test_crc32_mpeg2);
  g_test_add_func ("/proto/image-crc", test_image_crc);
  return g_test_run ();
}
