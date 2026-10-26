/*
 * Goodix 27c6:55b4 / 27c6:5584 driver for libfprint — native Milan matching
 *
 * Copyright (C) 2021 Alireza S.N. <alireza6677@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#pragma once

#include <glib.h>

/* USB */
#define GOODIX55B4_INTERFACE (0)
#define GOODIX55B4_EP_IN (0x82 | FPI_USB_ENDPOINT_IN)
#define GOODIX55B4_EP_OUT (0x1 | FPI_USB_ENDPOINT_OUT)

/* Device identity */
#define GOODIX55B4_FIRMWARE_PREFIX "GF3268_RTSEC_APP_"
#define GOODIX55B4_PSK_FLAGS (0xbb020007)
#define GOODIX55B4_RESET_NUMBER (1024)
/* Chip-ID register 0, decoded b0*0x100 + b1 + b2*0x1000000 + b3*0x10000.
 * Both 55b4 and 5584 read 0x00220ca1: the same Milan die as the Dell 53x5
 * readers, which the vendored engine supports as profile 9 / sensor subtype
 * 12. */
#define GOODIX55B4_CHIP_FAMILY_MASK (0xffffff00u)
#define GOODIX55B4_CHIP_FAMILY (0x00220c00u)
#define GOODIX55B4_SENSOR_SUBTYPE (12)

/* Milan anti-spoof inputs. Only the anti-spoof sensor offset uses them; on
 * real 55b4 frames three different value sets gave identical decisions
 * (milan-port N2/N3), so the 53x5 defaults are used. */
#define GOODIX55B4_TCODE (8)
#define GOODIX55B4_DAC_HIGH (0x97)
#define GOODIX55B4_DAC_LOW (0xd0)

/* Sensor raster: 88 rows x 108 columns, 12-bit, packed 6 bytes per 4 pixels */
#define GOODIX55B4_WIDTH (108)
#define GOODIX55B4_HEIGHT (88)
#define GOODIX55B4_PIXELS (GOODIX55B4_WIDTH * GOODIX55B4_HEIGHT)
#define GOODIX55B4_PACKED_BYTES (GOODIX55B4_PIXELS / 4 * 6)
#define GOODIX55B4_IMAGE_BYTES (GOODIX55B4_PACKED_BYTES + 4) /* + checksum */
#define GOODIX55B4_PPMM (500.0 / 25.4)

/* Finger detection (FDT-55b4.md): 12 channels; table entry = thr << 8 | 0x80 */
#define GOODIX55B4_FDT_CHANNELS (12)
#define GOODIX55B4_FDT_UP_WATCHDOG_MS (3000)
#define GOODIX55B4_FDT_OP_DOWN (0x0c)
#define GOODIX55B4_FDT_OP_MODE (0x0d)
#define GOODIX55B4_FDT_OP_UP (0x0e)
#define GOODIX55B4_IRQ_TOUCH (0x0002)
#define GOODIX55B4_IRQ_BASELINE (0x0080)
#define GOODIX55B4_IRQ_UP (0x0200)
#define GOODIX55B4_FDT_MAX_REARMS (50)

/* Device was provisioned with the all-zero PSK; this is its PMK hash. */
static const guint8 goodix55b4_zero_psk_hash[32] = {
  0x81, 0xb8, 0xff, 0x49, 0x06, 0x12, 0x02, 0x2a, 0x12, 0x1a, 0x94,
  0x49, 0xee, 0x3a, 0xad, 0x27, 0x92, 0xf3, 0x2b, 0x9f, 0x31, 0x41,
  0x18, 0x2c, 0xd0, 0x10, 0x19, 0x94, 0x5e, 0xe5, 0x03, 0x61
};
