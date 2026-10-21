/*
 * Goodix 55b4 — per-unit sensor configuration from the OTP
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#pragma once

#include <glib.h>

#define GOODIX55B4_CONFIG_SIZE (256)

typedef struct
{
  gboolean from_otp;   /* FALSE: OTP unusable, template values are used */
  guint16  tcode;      /* config tag 0x5c (sections 2 and 4) */
  guint8   fdt_delta;  /* config tag 0x82 (section 2) = fdt_delta << 8 | 0x80;
                        * also the FDT finger-up delta; untouched channels
                        * use fdt_delta - 2 */
} Goodix55b4Calib;

/* Derive the calibration from the 32-byte OTP read with command 0xa6. */
void goodix55b4_calib_from_otp (const guint8    *otp,
                                gsize            len,
                                Goodix55b4Calib *calib);

/* The config to upload with command 0x90: the base config with the
 * calibration applied and the checksum recomputed. */
void goodix55b4_config_build (const Goodix55b4Calib *calib,
                              guint8                 out[GOODIX55B4_CONFIG_SIZE]);
