/*
 * Goodix 27c6:55b4 driver for libfprint — native Milan matching
 *
 * Protocol layer: goodix.c / goodix_proto.c / goodixtls.c from the
 * goodixtls55x4 driver. Matching: the native Milan engine (milan/, vendored
 * from goodix53x5, profile 9 / subtype 12 — same sensor die, chip 0x00220ca1).
 *
 * Copyright (C) 2021 Alireza S.N. <alireza6677@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define FP_COMPONENT "goodix55b4"

#include "drivers_api.h"
#include "goodix.h"
#include "goodix_proto.h"
#include "goodix55b4.h"
#include "goodix55b4-calib.h"
#include "goodix55b4-state.h"
#include "goodix55b4-study.h"
#include "milan/match/match.h"
#include "milan/print.h"
#include "milan/runtime.h"

#include <string.h>

/* Engine state (calibration, history) survives across actions and fprintd
 * restarts; without it single-attempt acceptance drops from 95% to ~70%
 * (milan-port N3). Override the directory with GOODIX55B4_STATE_DIR. */
#define GOODIX55B4_STATE_DIR_DEFAULT "/var/lib/fprint/goodix55b4"
#define GOODIX55B4_STATE_FILE "engine-state.bin"
/* Studied templates (goodix55b4-study.h); GOODIX55B4_NO_STUDY=1 matches
 * against the enrolled templates only and learns nothing. */
#define GOODIX55B4_STUDY_SUBDIR "studied"

struct _FpiDeviceGoodix55b4
{
  FpiDeviceGoodixTls parent;

  guint32            chip_id;
  Goodix55b4Calib    calib;      /* from the OTP of this unit */
  guint8             config[GOODIX55B4_CONFIG_SIZE];
  guint8             fdt_down[GOODIX55B4_FDT_CHANNELS];
  guint8             fdt_up[GOODIX55B4_FDT_CHANNELS];
  guint16           *setup;      /* no-finger frame of this activation */
  guint16           *frame;      /* last finger frame */
  gboolean           finger_on;  /* last FDT reading said "touched" */
  guint              rearms;
  guint              clear_tries;
  FpiSsm            *scan_ssm;
  GSource           *up_watchdog;

  /* Milan engine. Heap-allocated: the state is ~212 KB and GObject
   * instance structs must stay below 64 KB. */
  Goodix55b4EngineState *engine;
  gchar                *state_path;
  guint64               epoch;
  GTask                *engine_task;
  GoodixMilanEnrollmentTransaction *enroll_tx;
  guint                 enroll_bad_records;
  guint                 enroll_bad_continue;
  GPtrArray            *gallery_prints; /* FpPrint per gallery index (identify) */
  GPtrArray            *gallery_keys;   /* study key per gallery index, or NULL */
};

G_DECLARE_FINAL_TYPE (FpiDeviceGoodix55b4, fpi_device_goodix55b4, FPI,
                      DEVICE_GOODIX55B4, FpiDeviceGoodixTls);
G_DEFINE_TYPE (FpiDeviceGoodix55b4, fpi_device_goodix55b4,
               FPI_TYPE_DEVICE_GOODIXTLS);

static const FpIdEntry id_table[] = {
  {.vid = 0x27c6, .pid = 0x55b4},
  {.vid = 0, .pid = 0, .driver_data = 0},
};

/* ------------------------------------------------------------------------ */
/* Frames                                                                   */

static gboolean
decode_frame (const guint8 *data, guint16 len, guint16 *out)
{
  if (!data || len < GOODIX55B4_PACKED_BYTES)
    return FALSE;

  /* Same 6-bytes -> 4-pixels unpack as goodixtls55x4 decode_frame(). */
  for (guint i = 0, p = 0; i < GOODIX55B4_PACKED_BYTES; i += 6)
    {
      const guint8 *c = data + i;
      out[p++] = ((c[0] & 0x0f) << 8) | c[1];
      out[p++] = (c[3] << 4) | (c[0] >> 4);
      out[p++] = ((c[5] & 0x0f) << 8) | c[2];
      out[p++] = (c[4] << 4) | (c[5] >> 4);
    }
  return TRUE;
}

static int
cmp_int (const void *a, const void *b)
{
  return *(const int *) a - *(const int *) b;
}

/* setup - frame, stretched between the 2nd and 98th percentile (capture only;
 * matching uses the raw frames). */
static FpImage *
frame_to_image (const guint16 *setup, const guint16 *frame)
{
  FpImage *img = fp_image_new (GOODIX55B4_WIDTH, GOODIX55B4_HEIGHT);
  g_autofree int *diff = g_new (int, GOODIX55B4_PIXELS);
  g_autofree int *sorted = g_new (int, GOODIX55B4_PIXELS);
  int lo, hi;

  for (int i = 0; i < GOODIX55B4_PIXELS; i++)
    diff[i] = sorted[i] = (int) setup[i] - (int) frame[i];
  qsort (sorted, GOODIX55B4_PIXELS, sizeof (int), cmp_int);
  lo = sorted[GOODIX55B4_PIXELS * 2 / 100];
  hi = sorted[GOODIX55B4_PIXELS * 98 / 100];
  if (hi <= lo)
    hi = lo + 1;
  /* ridges (large diff) dark, valleys light */
  for (int i = 0; i < GOODIX55B4_PIXELS; i++)
    img->data[i] = 255 - CLAMP ((diff[i] - lo) * 255 / (hi - lo), 0, 255);
  img->ppmm = GOODIX55B4_PPMM;
  return img;
}

/* ------------------------------------------------------------------------ */
/* FDT helpers                                                              */

typedef struct
{
  guint16 irq;
  guint16 touch;
  guint8  values[GOODIX55B4_FDT_CHANNELS]; /* raw >> 1 */
} FdtReading;

static gboolean
parse_fdt (const guint8 *data, guint16 len, FdtReading *r)
{
  if (!data || len < 4 + 2 * GOODIX55B4_FDT_CHANNELS)
    return FALSE;
  r->irq = data[0] | (data[1] << 8);
  r->touch = data[2] | (data[3] << 8);
  for (int i = 0; i < GOODIX55B4_FDT_CHANNELS; i++)
    {
      guint v = (data[4 + 2 * i] | (data[5 + 2 * i] << 8)) >> 1;
      r->values[i] = CLAMP (v, 1, 0xfe); /* 0x00 / 0xff are invalid thresholds */
    }
  return TRUE;
}

static guint8 *
fdt_payload (guint8 op, const guint8 *thr, guint16 *len)
{
  guint8 *p = g_malloc (2 + 2 * GOODIX55B4_FDT_CHANNELS);

  p[0] = op;
  p[1] = 0x01;
  for (int i = 0; i < GOODIX55B4_FDT_CHANNELS; i++)
    {
      p[2 + 2 * i] = 0x80;
      p[3 + 2 * i] = thr[i];
    }
  *len = 2 + 2 * GOODIX55B4_FDT_CHANNELS;
  return p;
}

static void
log_fdt (const char *what, const FdtReading *r)
{
  fp_dbg ("%s: irq 0x%04x touch 0x%03x values %02x %02x %02x %02x %02x %02x "
          "%02x %02x %02x %02x %02x %02x", what, r->irq, r->touch,
          r->values[0], r->values[1], r->values[2], r->values[3], r->values[4],
          r->values[5], r->values[6], r->values[7], r->values[8], r->values[9],
          r->values[10], r->values[11]);
}

/* ------------------------------------------------------------------------ */
/* Activation (open)                                                        */

enum activate_states {
  ACT_READ_AND_NOP,
  ACT_ENABLE_CHIP,
  ACT_NOP,
  ACT_CHECK_FW,
  ACT_CHECK_PSK,
  ACT_RESET,
  ACT_CHIP_ID,
  ACT_OTP,
  ACT_IDLE,
  ACT_CONFIG,
  ACT_TLS,
  ACT_BASELINE,
  ACT_SETUP_FRAME,
  ACT_NUM_STATES,
};

static void
act_next (FpDevice *dev, gpointer ssm, GError *error)
{
  if (error)
    fpi_ssm_mark_failed (ssm, error);
  else
    fpi_ssm_next_state (ssm);
}

static void
act_check_fw (FpDevice *dev, gchar *firmware, gpointer ssm, GError *error)
{
  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }
  fp_dbg ("firmware: %s", firmware);
  if (!g_str_has_prefix (firmware, GOODIX55B4_FIRMWARE_PREFIX))
    {
      fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (
                             FP_DEVICE_ERROR_NOT_SUPPORTED,
                             "Unsupported firmware: %s", firmware));
      return;
    }
  fpi_ssm_next_state (ssm);
}

static void
act_check_psk (FpDevice *dev, gboolean success, guint32 flags, guint8 *psk,
               guint16 length, gpointer ssm, GError *error)
{
  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }
  if (!success || flags != GOODIX55B4_PSK_FLAGS ||
      length != sizeof (goodix55b4_zero_psk_hash) ||
      memcmp (psk, goodix55b4_zero_psk_hash, length) != 0)
    {
      fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (
                             FP_DEVICE_ERROR_NOT_SUPPORTED,
                             "Sensor is not provisioned with the zero PSK "
                             "(flags 0x%08x); provision it first", flags));
      return;
    }
  fpi_ssm_next_state (ssm);
}

static void
act_check_reset (FpDevice *dev, gboolean success, guint16 number,
                 gpointer ssm, GError *error)
{
  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }
  if (!success || number != GOODIX55B4_RESET_NUMBER)
    {
      fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (
                             FP_DEVICE_ERROR_PROTO,
                             "Sensor reset failed (number %u)", number));
      return;
    }
  fpi_ssm_next_state (ssm);
}

static void
act_check_chip_id (FpDevice *dev, guint8 *data, guint16 len, gpointer ssm,
                   GError *error)
{
  FpiDeviceGoodix55b4 *self = FPI_DEVICE_GOODIX55B4 (dev);

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }
  if (len < 4)
    {
      fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (
                             FP_DEVICE_ERROR_PROTO, "Short chip ID reply"));
      return;
    }
  self->chip_id = data[0] * 0x100u + data[1] + data[2] * 0x1000000u +
                  data[3] * 0x10000u;
  fp_dbg ("chip id 0x%08x", self->chip_id);
  if ((self->chip_id & GOODIX55B4_CHIP_FAMILY_MASK) != GOODIX55B4_CHIP_FAMILY)
    {
      fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (
                             FP_DEVICE_ERROR_NOT_SUPPORTED,
                             "Unsupported sensor chip 0x%08x", self->chip_id));
      return;
    }
  fpi_ssm_next_state (ssm);
}

static void
act_otp (FpDevice *dev, guint8 *data, guint16 len, gpointer ssm, GError *error)
{
  FpiDeviceGoodix55b4 *self = FPI_DEVICE_GOODIX55B4 (dev);

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }
  /* Windows (Milan_DlCfg) calibrates the config from the OTP: tcode and
   * the finger-detection delta. See goodix55b4-calib.c. */
  goodix55b4_calib_from_otp (data, len, &self->calib);
  if (self->calib.from_otp)
    fp_dbg ("OTP calibration: tcode 0x%x, FDT delta 0x%x", self->calib.tcode,
            self->calib.fdt_delta);
  else
    fp_warn ("No calibration in the sensor OTP (%u bytes), using template "
             "values", len);
  goodix55b4_config_build (&self->calib, self->config);
  fpi_ssm_next_state (ssm);
}

static void
act_check_success (FpDevice *dev, gboolean success, gpointer ssm, GError *error)
{
  if (error)
    fpi_ssm_mark_failed (ssm, error);
  else if (!success)
    fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (
                           FP_DEVICE_ERROR_PROTO, "Config upload failed"));
  else
    fpi_ssm_next_state (ssm);
}

static void
act_baseline (FpDevice *dev, guint8 *data, guint16 len, gpointer ssm,
              GError *error)
{
  FpiDeviceGoodix55b4 *self = FPI_DEVICE_GOODIX55B4 (dev);
  FdtReading r;

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }
  if (!parse_fdt (data, len, &r))
    {
      fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (
                             FP_DEVICE_ERROR_PROTO, "Bad FDT reply"));
      return;
    }
  log_fdt ("baseline", &r);
  if ((r.irq & GOODIX55B4_IRQ_TOUCH) || r.touch)
    {
      /* A finger on the sensor would poison the no-finger frame: wait. */
      if (++self->clear_tries > 25)
        {
          fpi_ssm_mark_failed (ssm, fpi_device_retry_new_msg (
                                 FP_DEVICE_RETRY_REMOVE_FINGER,
                                 "Remove the finger from the sensor"));
          return;
        }
      fpi_device_report_finger_status_changes (dev, FP_FINGER_STATUS_PRESENT,
                                               FP_FINGER_STATUS_NONE);
      fpi_ssm_jump_to_state_delayed (ssm, ACT_BASELINE, 200);
      return;
    }
  memcpy (self->fdt_down, r.values, sizeof (self->fdt_down));
  self->finger_on = FALSE;
  fpi_ssm_next_state (ssm);
}

static void
act_setup_frame (FpDevice *dev, guint8 *data, guint16 len, gpointer ssm,
                 GError *error)
{
  FpiDeviceGoodix55b4 *self = FPI_DEVICE_GOODIX55B4 (dev);

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }
  g_clear_pointer (&self->setup, g_free);
  self->setup = g_new (guint16, GOODIX55B4_PIXELS);
  if (!decode_frame (data, len, self->setup))
    {
      fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (
                             FP_DEVICE_ERROR_PROTO,
                             "Short setup image (%u bytes)", len));
      return;
    }
  fpi_ssm_next_state (ssm);
}

static void
act_tls_done (FpDevice *dev, gpointer ssm, GError *error)
{
  act_next (dev, ssm, error);
}

static void
activate_run_state (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceGoodix55b4 *self = FPI_DEVICE_GOODIX55B4 (dev);
  guint16 len;
  guint8 *payload;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case ACT_READ_AND_NOP:
      goodix_start_read_loop (dev);
      goodix_send_nop (dev, act_next, ssm);
      break;

    case ACT_ENABLE_CHIP:
      goodix_send_enable_chip (dev, TRUE, act_next, ssm);
      break;

    case ACT_NOP:
      goodix_send_nop (dev, act_next, ssm);
      break;

    case ACT_CHECK_FW:
      goodix_send_firmware_version (dev, act_check_fw, ssm);
      break;

    case ACT_CHECK_PSK:
      goodix_send_preset_psk_read (dev, GOODIX55B4_PSK_FLAGS, 32,
                                   act_check_psk, ssm);
      break;

    case ACT_RESET:
      goodix_send_reset (dev, TRUE, 20, act_check_reset, ssm);
      break;

    case ACT_CHIP_ID:
      goodix_send_read_sensor_register (dev, 0x0000, 4, act_check_chip_id, ssm);
      break;

    case ACT_OTP:
      goodix_send_read_otp (dev, act_otp, ssm);
      break;

    case ACT_IDLE:
      goodix_send_mcu_switch_to_idle_mode (dev, 20, act_next, ssm);
      break;

    case ACT_CONFIG:
      goodix_send_upload_config_mcu (dev, self->config, sizeof (self->config),
                                     NULL,
                                     act_check_success, ssm);
      break;

    case ACT_TLS:
      goodix_tls (dev, act_tls_done, ssm);
      break;

    case ACT_BASELINE:
      {
        static const guint8 any[GOODIX55B4_FDT_CHANNELS] = {
          0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80
        };
        payload = fdt_payload (GOODIX55B4_FDT_OP_MODE, any, &len);
        goodix_send_mcu_switch_to_fdt_mode (dev, payload, len, g_free,
                                            act_baseline, ssm);
      }
      break;

    case ACT_SETUP_FRAME:
      goodix_tls_read_image (dev, act_setup_frame, ssm);
      break;
    }
  (void) self;
}

static const char *
state_dir (void)
{
  const char *d = g_getenv ("GOODIX55B4_STATE_DIR");

  return d && *d ? d : GOODIX55B4_STATE_DIR_DEFAULT;
}

/* The open just captured a fresh no-finger frame, i.e. a new setup
 * generation: load the persisted engine state and apply the generation rule. */
static void
engine_state_open (FpiDeviceGoodix55b4 *self)
{
  g_autoptr(GError) error = NULL;

  g_free (self->state_path);
  self->state_path = g_build_filename (state_dir (), GOODIX55B4_STATE_FILE, NULL);
  if (goodix55b4_state_load (self->engine, self->state_path,
                             GOODIX55B4_SENSOR_SUBTYPE, &error))
    {
      fp_dbg ("restored engine state (%u calibration samples)",
              self->engine->state.sample_count);
      goodix55b4_state_new_generation (self->engine);
      return;
    }
  if (!g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
    fp_warn ("ignoring engine state: %s", error->message);
  goodix55b4_state_init (self->engine);
}

static void
engine_state_save (FpiDeviceGoodix55b4 *self)
{
  g_autoptr(GError) error = NULL;

  if (self->state_path &&
      !goodix55b4_state_save (self->engine, self->state_path,
                              GOODIX55B4_SENSOR_SUBTYPE, &error))
    fp_warn ("cannot save engine state: %s", error->message);
}

static gboolean
study_enabled (void)
{
  return g_strcmp0 (g_getenv ("GOODIX55B4_NO_STUDY"), "1") != 0;
}

static gchar *
study_dir (void)
{
  return g_build_filename (state_dir (), GOODIX55B4_STUDY_SUBDIR, NULL);
}

static void
activate_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  if (error)
    {
      GError *deinit_error = NULL;

      fp_warn ("activation failed: %s", error->message);
      goodix_dev_deinit (dev, &deinit_error);
      g_clear_error (&deinit_error);
    }
  else
    {
      engine_state_open (FPI_DEVICE_GOODIX55B4 (dev));
    }
  fpi_device_open_complete (dev, error);
}

static void
dev_open (FpDevice *dev)
{
  FpiDeviceGoodix55b4 *self = FPI_DEVICE_GOODIX55B4 (dev);
  GError *error = NULL;

  if (!goodix_dev_init (dev, &error))
    {
      fpi_device_open_complete (dev, error);
      return;
    }
  self->clear_tries = 0;
  fpi_ssm_start (fpi_ssm_new (dev, activate_run_state, ACT_NUM_STATES),
                 activate_complete);
}

/* ------------------------------------------------------------------------ */
/* Scan: wait finger down (dynamic FDT) -> image -> pressed reading.        */
/* The lift is awaited at the start of the next scan if the finger is still */
/* on, so results are reported without waiting for the user to lift.       */

enum scan_states {
  SCAN_WAIT_UP,
  SCAN_WAIT_DOWN,
  SCAN_IMAGE,
  SCAN_PRESSED,
  SCAN_NUM_STATES,
};

static void scan_up_cb (FpDevice *dev, guint8 *data, guint16 len,
                        gpointer ssm, GError *error);

static void
up_watchdog_cb (FpDevice *dev, gpointer user_data)
{
  FpiDeviceGoodix55b4 *self = FPI_DEVICE_GOODIX55B4 (dev);
  guint16 len;
  guint8 *payload;

  self->up_watchdog = NULL;
  if (!self->scan_ssm || fpi_ssm_get_cur_state (self->scan_ssm) != SCAN_WAIT_UP)
    return;

  /* Safety net: no lift event although the finger should be long gone.
   * Re-arm finger-up with all thresholds at the "untouched" level, which the
   * MCU reports at once; Windows also re-sends 0x34 while one is armed. If a
   * finger is in fact still resting, its values become the DOWN table and
   * the lift shows up later as IRQ 0x0080 (baseline refresh). */
  fp_warn ("No finger-up event in %d ms, forcing it", GOODIX55B4_FDT_UP_WATCHDOG_MS);
  goodix_reset_state (dev);
  memset (self->fdt_up, self->calib.fdt_delta - 2, sizeof (self->fdt_up));
  payload = fdt_payload (GOODIX55B4_FDT_OP_UP, self->fdt_up, &len);
  goodix_send_mcu_switch_to_fdt_up (dev, payload, len, g_free, scan_up_cb,
                                    self->scan_ssm);
}

static void
scan_up_cb (FpDevice *dev, guint8 *data, guint16 len, gpointer ssm,
            GError *error)
{
  FpiDeviceGoodix55b4 *self = FPI_DEVICE_GOODIX55B4 (dev);
  FdtReading r;

  g_clear_pointer (&self->up_watchdog, g_source_destroy);
  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }
  if (!parse_fdt (data, len, &r))
    {
      fpi_ssm_mark_failed (ssm, fpi_device_error_new (FP_DEVICE_ERROR_PROTO));
      return;
    }
  log_fdt ("up", &r);
  if (r.irq & GOODIX55B4_IRQ_UP)
    {
      /* Windows: the lift reading is the next DOWN baseline. */
      memcpy (self->fdt_down, r.values, sizeof (self->fdt_down));
      self->finger_on = FALSE;
      fpi_device_report_finger_status_changes (dev, FP_FINGER_STATUS_NONE,
                                               FP_FINGER_STATUS_PRESENT);
      fpi_ssm_next_state (ssm);
      return;
    }
  if (++self->rearms > GOODIX55B4_FDT_MAX_REARMS)
    {
      fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (
                             FP_DEVICE_ERROR_PROTO, "No finger-up event"));
      return;
    }
  fpi_ssm_jump_to_state (ssm, SCAN_WAIT_UP);
}

static void
scan_down_cb (FpDevice *dev, guint8 *data, guint16 len, gpointer ssm,
              GError *error)
{
  FpiDeviceGoodix55b4 *self = FPI_DEVICE_GOODIX55B4 (dev);
  FdtReading r;

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }
  if (!parse_fdt (data, len, &r))
    {
      fpi_ssm_mark_failed (ssm, fpi_device_error_new (FP_DEVICE_ERROR_PROTO));
      return;
    }
  log_fdt ("down", &r);
  if (r.irq & GOODIX55B4_IRQ_TOUCH)
    {
      /* Up table, as the second 0x34 of the Windows driver: from this event,
       * value + delta on touched channels and delta - 2 on the untouched
       * ones (a baseline + delta threshold there may never be crossed, and
       * the lift is never reported). delta is the unit's OTP calibration. */
      for (int i = 0; i < GOODIX55B4_FDT_CHANNELS; i++)
        self->fdt_up[i] = (r.touch & (1u << i)) ?
                          MIN (0xfe, r.values[i] + self->calib.fdt_delta) :
                          self->calib.fdt_delta - 2;
      fpi_device_report_finger_status_changes (dev, FP_FINGER_STATUS_PRESENT,
                                               FP_FINGER_STATUS_NEEDED);
      fpi_ssm_next_state (ssm);
      return;
    }
  if (r.irq & GOODIX55B4_IRQ_BASELINE)
    /* Baseline drift, no finger: refresh DOWN from this event and re-arm
     * (OEM behaviour; see FDT-55b4.md). */
    memcpy (self->fdt_down, r.values, sizeof (self->fdt_down));

  if (++self->rearms > GOODIX55B4_FDT_MAX_REARMS)
    {
      fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (
                             FP_DEVICE_ERROR_PROTO, "Finger detection keeps "
                             "re-arming without a touch"));
      return;
    }
  fpi_ssm_jump_to_state (ssm, SCAN_WAIT_DOWN);
}

static void
scan_image_cb (FpDevice *dev, guint8 *data, guint16 len, gpointer ssm,
               GError *error)
{
  FpiDeviceGoodix55b4 *self = FPI_DEVICE_GOODIX55B4 (dev);

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }
  g_clear_pointer (&self->frame, g_free);
  self->frame = g_new (guint16, GOODIX55B4_PIXELS);
  if (!decode_frame (data, len, self->frame))
    {
      fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (
                             FP_DEVICE_ERROR_PROTO,
                             "Short image (%u bytes)", len));
      return;
    }
  fpi_ssm_next_state (ssm);
}

static void
scan_pressed_cb (FpDevice *dev, guint8 *data, guint16 len, gpointer ssm,
                 GError *error)
{
  FpiDeviceGoodix55b4 *self = FPI_DEVICE_GOODIX55B4 (dev);
  FdtReading r;

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }
  if (!parse_fdt (data, len, &r))
    {
      fpi_ssm_mark_failed (ssm, fpi_device_error_new (FP_DEVICE_ERROR_PROTO));
      return;
    }
  log_fdt ("pressed", &r);
  if ((r.irq & GOODIX55B4_IRQ_TOUCH) && r.touch)
    {
      self->finger_on = TRUE;
    }
  else
    {
      /* Finger already gone: this reading is a fresh baseline. */
      memcpy (self->fdt_down, r.values, sizeof (self->fdt_down));
      self->finger_on = FALSE;
    }
  fpi_ssm_next_state (ssm);
}

static void
scan_run_state (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceGoodix55b4 *self = FPI_DEVICE_GOODIX55B4 (dev);
  guint16 len;
  guint8 *payload;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case SCAN_WAIT_UP:
      if (!self->finger_on)
        {
          fpi_ssm_next_state (ssm);
          break;
        }
      fpi_device_report_finger_status_changes (dev, FP_FINGER_STATUS_PRESENT,
                                               FP_FINGER_STATUS_NONE);
      payload = fdt_payload (GOODIX55B4_FDT_OP_UP, self->fdt_up, &len);
      goodix_send_mcu_switch_to_fdt_up (dev, payload, len, g_free, scan_up_cb,
                                        ssm);
      g_clear_pointer (&self->up_watchdog, g_source_destroy);
      self->up_watchdog = fpi_device_add_timeout (dev,
                                                  GOODIX55B4_FDT_UP_WATCHDOG_MS,
                                                  up_watchdog_cb, NULL, NULL);
      /* count from now, not from the main loop's cached time */
      g_source_set_ready_time (self->up_watchdog, g_get_monotonic_time () +
                               GOODIX55B4_FDT_UP_WATCHDOG_MS * G_GINT64_CONSTANT (1000));
      break;

    case SCAN_WAIT_DOWN:
      fpi_device_report_finger_status_changes (dev, FP_FINGER_STATUS_NEEDED,
                                               FP_FINGER_STATUS_NONE);
      payload = fdt_payload (GOODIX55B4_FDT_OP_DOWN, self->fdt_down, &len);
      goodix_send_mcu_switch_to_fdt_down (dev, payload, len, g_free,
                                          scan_down_cb, ssm);
      break;

    case SCAN_IMAGE:
      goodix_tls_read_image (dev, scan_image_cb, ssm);
      break;

    case SCAN_PRESSED:
      payload = fdt_payload (GOODIX55B4_FDT_OP_MODE, self->fdt_down, &len);
      goodix_send_mcu_switch_to_fdt_mode (dev, payload, len, g_free,
                                          scan_pressed_cb, ssm);
      break;
    }
}

typedef void (*ScanDoneFunc) (FpDevice *dev, GError *error);

static void
scan_start (FpDevice *dev, FpiSsmCompletedCallback done)
{
  FpiDeviceGoodix55b4 *self = FPI_DEVICE_GOODIX55B4 (dev);

  g_assert (self->scan_ssm == NULL);
  self->rearms = 0;
  self->scan_ssm = fpi_ssm_new (dev, scan_run_state, SCAN_NUM_STATES);
  fpi_ssm_start (self->scan_ssm, done);
}

/* ------------------------------------------------------------------------ */
/* Capture (diagnostic: setup - frame as an 8-bit image)                    */

static void
capture_done (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceGoodix55b4 *self = FPI_DEVICE_GOODIX55B4 (dev);

  self->scan_ssm = NULL;
  if (error)
    {
      fpi_device_capture_complete (dev, NULL, error);
      return;
    }
  fpi_device_capture_complete (dev, frame_to_image (self->setup, self->frame),
                               NULL);
}

static void
dev_capture (FpDevice *dev)
{
  scan_start (dev, capture_done);
}

/* ------------------------------------------------------------------------ */
/* Milan engine: runs in a worker thread on copies of the frames/state.     */

typedef void (*EngineDone) (FpDevice                 *dev,
                            GoodixMilanRuntimeOutput *out,
                            GError                   *error);

typedef struct
{
  GoodixMilanRuntimeInput *input;
  EngineDone               done;
} EngineJob;

static void
engine_job_free (EngineJob *job)
{
  g_clear_pointer (&job->input, goodix_milan_runtime_input_free);
  g_free (job);
}

static gboolean
engine_cancel_check (GoodixMilanRuntimeCheckpoint checkpoint,
                     gsize gallery_position, gpointer user_data)
{
  return g_cancellable_is_cancelled (G_CANCELLABLE (user_data));
}

static void
engine_worker (GTask *task, gpointer source, gpointer task_data,
               GCancellable *cancellable)
{
  EngineJob *job = task_data;
  GoodixMilanRuntimeOutput *out = goodix_milan_runtime_run (job->input);

  if (out)
    g_task_return_pointer (task, out,
                           (GDestroyNotify) goodix_milan_runtime_output_free);
  else
    g_task_return_new_error (task, FP_DEVICE_ERROR, FP_DEVICE_ERROR_GENERAL,
                             "Milan engine returned no result");
}

static void
engine_task_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
  FpDevice *dev = FP_DEVICE (source);
  FpiDeviceGoodix55b4 *self = FPI_DEVICE_GOODIX55B4 (dev);
  EngineJob *job = g_task_get_task_data (G_TASK (res));
  EngineDone done = job->done;
  GError *error = NULL;
  GoodixMilanRuntimeOutput *out;

  out = g_task_propagate_pointer (G_TASK (res), &error);
  g_clear_object (&self->engine_task);

  if (out && out->action_epoch == self->epoch)
    {
      /* Carry the engine state forward exactly as upstream device/enroll.c. */
      if (out->preprocess_state_valid &&
          out->status != GOODIX_MILAN_RUNTIME_CANCELLED)
        {
          self->engine->state = out->preprocess_state;
          self->engine->profile = out->profile_state;
        }
      else if (out->setup_state_valid)
        {
          self->engine->profile.setup_initialized =
            out->profile_state.setup_initialized;
          self->engine->profile.setup_refresh_pending =
            out->profile_state.setup_refresh_pending;
          self->engine->profile.setup_not_ready =
            out->profile_state.setup_not_ready;
        }
      engine_state_save (self);
      fp_dbg ("engine: status %d score %d winner %d quality %d coverage %d%s%s",
              out->status, out->score,
              out->status == GOODIX_MILAN_RUNTIME_MATCH ? (int) out->winner_index : -1,
              out->quality, out->coverage, out->error ? " error: " : "",
              out->error ? out->error->message : "");
    }
  done (dev, out, error);
}

/* The runtime copies `gallery`; the caller keeps ownership. */
static void
engine_run (FpDevice *dev, GoodixMilanPreprocessPurpose purpose,
            GPtrArray *gallery, EngineDone done)
{
  FpiDeviceGoodix55b4 *self = FPI_DEVICE_GOODIX55B4 (dev);
  GCancellable *cancellable = fpi_device_get_cancellable (dev);
  EngineJob *job = g_new0 (EngineJob, 1);

  g_assert (self->engine_task == NULL);
  job->done = done;
  job->input = goodix_milan_runtime_input_new (
    ++self->epoch, 1, purpose, &self->engine->state, &self->engine->profile,
    self->setup, self->frame, GOODIX55B4_TCODE, GOODIX55B4_DAC_HIGH,
    GOODIX55B4_DAC_LOW, GOODIX55B4_SENSOR_SUBTYPE,
    gallery ? (GoodixMilanRuntimeGalleryInput **) gallery->pdata : NULL,
    gallery ? gallery->len : 0);
  if (!job->input)
    {
      engine_job_free (job);
      done (dev, NULL, fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                 "Cannot build engine input"));
      return;
    }
  if (cancellable)
    goodix_milan_runtime_input_set_cancel_check (job->input, engine_cancel_check,
                                                 g_object_ref (cancellable),
                                                 g_object_unref);
  self->engine_task = g_task_new (dev, cancellable, engine_task_done, NULL);
  g_task_set_check_cancellable (self->engine_task, FALSE);
  g_task_set_task_data (self->engine_task, job, (GDestroyNotify) engine_job_free);
  g_task_run_in_thread (self->engine_task, engine_worker);
}

static GError *
retry_for (const GoodixMilanRuntimeOutput *out)
{
  /* Low coverage means a partial touch; otherwise ask for another press. */
  if (out->coverage < GOODIX_MILAN_ENROLL_MIN_COVERAGE)
    return fpi_device_retry_new (FP_DEVICE_RETRY_CENTER_FINGER);
  return fpi_device_retry_new (FP_DEVICE_RETRY_GENERAL);
}

static GError *
cancelled_error (void)
{
  return g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED,
                              "Operation cancelled");
}

/* ------------------------------------------------------------------------ */
/* Enroll                                                                   */

static void enroll_scan_done (FpiSsm *ssm, FpDevice *dev, GError *error);

static void
enroll_finish (FpDevice *dev, GError *error)
{
  FpiDeviceGoodix55b4 *self = FPI_DEVICE_GOODIX55B4 (dev);
  g_autoptr(GBytes) tpl = NULL;
  GVariant *data;
  FpPrint *print = NULL;

  if (error)
    {
      g_clear_pointer (&self->enroll_tx, goodix_milan_enrollment_transaction_free);
      fpi_device_enroll_complete (dev, NULL, error);
      return;
    }

  tpl = goodix_milan_enrollment_transaction_publish (self->enroll_tx);
  g_clear_pointer (&self->enroll_tx, goodix_milan_enrollment_transaction_free);
  data = tpl ? goodix_milan_print_build_data (tpl, &error) : NULL;
  if (!data)
    {
      fpi_device_enroll_complete (dev, NULL, error ? error :
                                  fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                            "Failed to build the Milan template"));
      return;
    }
  fpi_device_get_enroll_data (dev, &print);
  fpi_print_set_type (print, FPI_PRINT_RAW);
  g_object_set (print, "fpi-data", data, NULL);
  g_variant_unref (data); /* build_data returns an owned reference */
  fp_info ("enrolled Milan template (%zu bytes)", g_bytes_get_size (tpl));
  fpi_device_enroll_complete (dev, g_object_ref (print), NULL);
}

static void
enroll_engine_done (FpDevice *dev, GoodixMilanRuntimeOutput *out, GError *error)
{
  FpiDeviceGoodix55b4 *self = FPI_DEVICE_GOODIX55B4 (dev);
  guint stages = FP_DEVICE_GET_CLASS (dev)->nr_enroll_stages;
  GoodixMilanEnrollmentResult result;
  GoodixMilanEnrollmentAttemptStatus st;
  gboolean admitted;
  guint done;

  if (error)
    {
      g_clear_pointer (&out, goodix_milan_runtime_output_free);
      enroll_finish (dev, error);
      return;
    }
  if (out->status == GOODIX_MILAN_RUNTIME_CANCELLED)
    {
      goodix_milan_runtime_output_free (out);
      enroll_finish (dev, cancelled_error ());
      return;
    }

  admitted = goodix_milan_runtime_enrollment_admitted (out);
  /* As upstream device/enroll.c: an admitted sample that fails the
   * enrollment gate breaks an unsaturated rollback streak. */
  if (out->enrollment_sample_admitted && !admitted &&
      self->enroll_bad_continue < 3)
    self->enroll_bad_continue = 0;

  done = goodix_milan_enrollment_transaction_count (self->enroll_tx);
  if (!admitted)
    {
      fp_dbg ("enroll sample rejected (quality %d, coverage %d)",
              out->quality, out->coverage);
      fpi_device_enroll_progress (dev, done, NULL, retry_for (out));
      goodix_milan_runtime_output_free (out);
      scan_start (dev, enroll_scan_done);
      return;
    }

  memset (&result, 0, sizeof (result));
  st = goodix_milan_enrollment_transaction_attempt (
    &self->enroll_tx, out->probe_template, &self->enroll_bad_records,
    &self->enroll_bad_continue, &result);
  goodix_milan_runtime_output_free (out);
  done = goodix_milan_enrollment_transaction_count (self->enroll_tx);

  if (st == GOODIX_MILAN_ENROLLMENT_ACCEPTED)
    {
      fpi_device_enroll_progress (dev, done, NULL, NULL);
      if (done >= stages)
        {
          enroll_finish (dev, NULL);
          return;
        }
    }
  else
    {
      fpi_device_enroll_progress (dev, done, NULL, fpi_device_retry_new (
                                    st == GOODIX_MILAN_ENROLLMENT_RETRY_CENTER ?
                                    FP_DEVICE_RETRY_CENTER_FINGER :
                                    FP_DEVICE_RETRY_REMOVE_FINGER));
    }
  scan_start (dev, enroll_scan_done);
}

static void
enroll_scan_done (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FPI_DEVICE_GOODIX55B4 (dev)->scan_ssm = NULL;
  if (error)
    {
      enroll_finish (dev, error);
      return;
    }
  engine_run (dev, GOODIX_MILAN_PURPOSE_ENROLL, NULL, enroll_engine_done);
}

static void
dev_enroll (FpDevice *dev)
{
  FpiDeviceGoodix55b4 *self = FPI_DEVICE_GOODIX55B4 (dev);

  g_clear_pointer (&self->enroll_tx, goodix_milan_enrollment_transaction_free);
  self->enroll_tx = goodix_milan_enrollment_transaction_new ();
  self->enroll_bad_records = 0;
  self->enroll_bad_continue = 0;
  scan_start (dev, enroll_scan_done);
}

/* ------------------------------------------------------------------------ */
/* Verify / identify                                                        */

static GBytes *
print_template (FpPrint *print)
{
  g_autoptr(GVariant) data = NULL;
  g_autoptr(GError) error = NULL;
  GBytes *tpl = NULL;

  g_object_get (print, "fpi-data", &data, NULL);
  if (!data || !goodix_milan_print_parse_data (data, &tpl, &error))
    {
      fp_warn ("unusable print: %s", error ? error->message : "no data");
      return NULL;
    }
  return tpl;
}

/* Keep the winner template with this probe studied into it, so the print
 * follows the finger over time (e.g. damp skin after washing). Stock
 * fprintd cannot store an updated print, hence the driver-side store. */
static void
study_store (const GoodixMilanRuntimeOutput *out, GPtrArray *keys)
{
  g_autoptr(GError) error = NULL;
  g_autofree gchar *dir = NULL;
  const char *key;

  if (out->learning_error)
    fp_dbg ("study: %s", out->learning_error->message);
  if (!out->final_candidate || !keys || out->winner_index >= keys->len)
    return;
  key = g_ptr_array_index (keys, out->winner_index);
  if (!key)
    return;
  dir = study_dir ();
  if (!goodix55b4_study_save (dir, key, out->final_candidate, &error))
    fp_warn ("cannot save studied template: %s", error->message);
  else
    fp_dbg ("study: action %d, template now %u features",
            (int) out->study_action, out->final_candidate_info.feature_count);
}

static void
match_engine_done (FpDevice *dev, GoodixMilanRuntimeOutput *out, GError *error)
{
  FpiDeviceGoodix55b4 *self = FPI_DEVICE_GOODIX55B4 (dev);
  gboolean identify = fpi_device_get_current_action (dev) ==
                      FPI_DEVICE_ACTION_IDENTIFY;
  g_autoptr(GPtrArray) prints = g_steal_pointer (&self->gallery_prints);
  g_autoptr(GPtrArray) keys = g_steal_pointer (&self->gallery_keys);

  if (!error && out->status == GOODIX_MILAN_RUNTIME_CANCELLED)
    error = cancelled_error ();
  else if (!error && out->status == GOODIX_MILAN_RUNTIME_INVALID_DATA)
    error = fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_INVALID, "%s",
                                      out->error ? out->error->message :
                                      "Milan engine rejected the input");
  if (error)
    {
      g_clear_pointer (&out, goodix_milan_runtime_output_free);
      fpi_device_action_error (dev, error);
      return;
    }

  if (out->status == GOODIX_MILAN_RUNTIME_MATCH)
    study_store (out, keys);

  if (out->status == GOODIX_MILAN_RUNTIME_RETRY)
    {
      if (identify)
        fpi_device_identify_report (dev, NULL, NULL, retry_for (out));
      else
        fpi_device_verify_report (dev, FPI_MATCH_ERROR, NULL, retry_for (out));
    }
  else if (identify)
    {
      FpPrint *match = NULL;

      if (out->status == GOODIX_MILAN_RUNTIME_MATCH &&
          out->winner_index < prints->len)
        match = g_ptr_array_index (prints, out->winner_index);
      fpi_device_identify_report (dev, match, NULL, NULL);
    }
  else
    {
      fpi_device_verify_report (dev, out->status == GOODIX_MILAN_RUNTIME_MATCH ?
                                FPI_MATCH_SUCCESS : FPI_MATCH_FAIL, NULL, NULL);
    }
  goodix_milan_runtime_output_free (out);

  if (identify)
    fpi_device_identify_complete (dev, NULL);
  else
    fpi_device_verify_complete (dev, NULL);
}

static void
match_scan_done (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceGoodix55b4 *self = FPI_DEVICE_GOODIX55B4 (dev);
  g_autoptr(GPtrArray) gallery = NULL;

  self->scan_ssm = NULL;
  if (error)
    {
      g_clear_pointer (&self->gallery_prints, g_ptr_array_unref);
      g_clear_pointer (&self->gallery_keys, g_ptr_array_unref);
      fpi_device_action_error (dev, error);
      return;
    }

  gallery = g_ptr_array_new_with_free_func (
    (GDestroyNotify) goodix_milan_runtime_gallery_input_free);
  g_clear_pointer (&self->gallery_keys, g_ptr_array_unref);
  self->gallery_keys = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; i < self->gallery_prints->len; i++)
    {
      g_autoptr(GBytes) tpl = print_template (g_ptr_array_index (self->gallery_prints, i));
      g_autoptr(GBytes) studied = NULL;
      gchar *key = NULL;

      if (tpl && study_enabled ())
        {
          g_autoptr(GError) study_error = NULL;
          g_autofree gchar *dir = study_dir ();

          key = goodix55b4_study_key (tpl);
          studied = goodix55b4_study_load (dir, key, &study_error);
          if (!studied && !g_error_matches (study_error, G_FILE_ERROR,
                                            G_FILE_ERROR_NOENT))
            fp_warn ("ignoring studied template: %s", study_error->message);
        }
      g_ptr_array_add (self->gallery_keys, key);

      /* Index i keeps the winner index aligned with gallery_prints; an
       * unusable print simply never matches. */
      if (tpl)
        g_ptr_array_add (gallery, goodix_milan_runtime_gallery_input_new (
                           i, studied ? studied : tpl));
    }
  if (gallery->len == 0)
    {
      g_clear_pointer (&self->gallery_prints, g_ptr_array_unref);
      g_clear_pointer (&self->gallery_keys, g_ptr_array_unref);
      if (fpi_device_get_current_action (dev) == FPI_DEVICE_ACTION_IDENTIFY)
        {
          /* Empty gallery (or only foreign prints): nothing can match. */
          fpi_device_identify_report (dev, NULL, NULL, NULL);
          fpi_device_identify_complete (dev, NULL);
        }
      else
        {
          fpi_device_action_error (dev, fpi_device_error_new_msg (
                                     FP_DEVICE_ERROR_DATA_INVALID,
                                     "Not a Milan template"));
        }
      return;
    }
  engine_run (dev, GOODIX_MILAN_PURPOSE_IDENTIFY, gallery, match_engine_done);
}

static void
dev_verify (FpDevice *dev)
{
  FpiDeviceGoodix55b4 *self = FPI_DEVICE_GOODIX55B4 (dev);
  FpPrint *print = NULL;

  fpi_device_get_verify_data (dev, &print);
  g_clear_pointer (&self->gallery_prints, g_ptr_array_unref);
  self->gallery_prints = g_ptr_array_new_with_free_func (g_object_unref);
  g_ptr_array_add (self->gallery_prints, g_object_ref (print));
  scan_start (dev, match_scan_done);
}

static void
dev_identify (FpDevice *dev)
{
  FpiDeviceGoodix55b4 *self = FPI_DEVICE_GOODIX55B4 (dev);
  GPtrArray *prints = NULL;

  fpi_device_get_identify_data (dev, &prints);
  g_clear_pointer (&self->gallery_prints, g_ptr_array_unref);
  self->gallery_prints = g_ptr_array_new_with_free_func (g_object_unref);
  for (guint i = 0; i < prints->len; i++)
    g_ptr_array_add (self->gallery_prints,
                     g_object_ref (g_ptr_array_index (prints, i)));
  /* Scan even with nothing to match: fprintd's duplicate check before an
   * enrollment counts this touch as the first enroll stage. */
  scan_start (dev, match_scan_done);
}

/* ------------------------------------------------------------------------ */
/* Close. From the Windows captures (55b4_init_login, _lock_unlock, ...):   */
/*  - 0x60 "01 00" ends an operation (MCU sleep mode; ACK only). The old    */
/*    goodixtls55x4 driver sent "14 00" (the 0x70 idle payload) instead.    */
/*  - 0x92 "6c" (Realtek, ACK + "01 00") is sent ~4-6 s after the last      */
/*    activity, right before WinUSB selective suspend. After close the      */
/*    device is idle and Linux autosuspends it, so it is sent here.         */

enum close_states {
  CLOSE_SLEEP,
  CLOSE_SLEEP_REALTEK,
  CLOSE_NUM_STATES,
};

static void
close_next (FpDevice *dev, gpointer ssm, GError *error)
{
  if (error)
    {
      /* A late reply to this command must not be taken for the next one's:
       * give it time to arrive (and be discarded) before continuing. */
      fp_warn ("sleep command failed: %s", error->message);
      g_clear_error (&error);
      fpi_ssm_next_state_delayed (ssm, 100);
      return;
    }
  fpi_ssm_next_state (ssm);
}

static void
close_realtek_cb (FpDevice *dev, gboolean success, gpointer ssm, GError *error)
{
  close_next (dev, ssm, error);
}

static void
close_run_state (FpiSsm *ssm, FpDevice *dev)
{
  switch (fpi_ssm_get_cur_state (ssm))
    {
    case CLOSE_SLEEP:
      goodix_reset_state (dev);
      /* the helper's "sleep_time" byte is the first payload byte: 01 00 */
      goodix_send_mcu_switch_to_sleep_mode (dev, 0x01, close_next, ssm);
      break;

    case CLOSE_SLEEP_REALTEK:
      goodix_send_mcu_switch_to_sleep_mode_realtek (dev, 0x6c, close_realtek_cb,
                                                    ssm);
      break;
    }
}

static void
close_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceGoodix55b4 *self = FPI_DEVICE_GOODIX55B4 (dev);
  GError *deinit_error = NULL;

  g_clear_error (&error);
  g_clear_pointer (&self->up_watchdog, g_source_destroy);
  g_clear_pointer (&self->setup, g_free);
  g_clear_pointer (&self->frame, g_free);
  g_clear_pointer (&self->enroll_tx, goodix_milan_enrollment_transaction_free);
  g_clear_pointer (&self->gallery_prints, g_ptr_array_unref);
  g_clear_pointer (&self->gallery_keys, g_ptr_array_unref);
  goodix_dev_deinit (dev, &deinit_error);
  fpi_device_close_complete (dev, deinit_error);
}

static void
dev_close (FpDevice *dev)
{
  fpi_ssm_start (fpi_ssm_new (dev, close_run_state, CLOSE_NUM_STATES),
                 close_complete);
}

/* ------------------------------------------------------------------------ */
/* Cancel                                                                   */

static void
dev_cancel (FpDevice *dev)
{
  FpiDeviceGoodix55b4 *self = FPI_DEVICE_GOODIX55B4 (dev);

  /* Drop the pending FDT wait (its reply will be ignored) and fail the scan;
   * the scan's completion reports the cancellation to libfprint. */
  g_clear_pointer (&self->up_watchdog, g_source_destroy);
  goodix_reset_state (dev);
  if (self->scan_ssm)
    fpi_ssm_mark_failed (self->scan_ssm,
                         g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                              "Operation cancelled"));
}

/* ------------------------------------------------------------------------ */

static void
fpi_device_goodix55b4_init (FpiDeviceGoodix55b4 *self)
{
  self->engine = g_new0 (Goodix55b4EngineState, 1);
  goodix55b4_state_init (self->engine);
}

static void
dev_finalize (GObject *object)
{
  FpiDeviceGoodix55b4 *self = FPI_DEVICE_GOODIX55B4 (object);

  g_clear_pointer (&self->setup, g_free);
  g_clear_pointer (&self->frame, g_free);
  g_clear_pointer (&self->state_path, g_free);
  g_clear_pointer (&self->engine, g_free);
  g_clear_pointer (&self->enroll_tx, goodix_milan_enrollment_transaction_free);
  g_clear_pointer (&self->gallery_prints, g_ptr_array_unref);
  g_clear_pointer (&self->gallery_keys, g_ptr_array_unref);
  G_OBJECT_CLASS (fpi_device_goodix55b4_parent_class)->finalize (object);
}

static void
fpi_device_goodix55b4_class_init (FpiDeviceGoodix55b4Class *klass)
{
  FpiDeviceGoodixTlsClass *gx_class = FPI_DEVICE_GOODIXTLS_CLASS (klass);
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);

  G_OBJECT_CLASS (klass)->finalize = dev_finalize;

  gx_class->interface = GOODIX55B4_INTERFACE;
  gx_class->ep_in = GOODIX55B4_EP_IN;
  gx_class->ep_out = GOODIX55B4_EP_OUT;

  dev_class->id = "goodix55b4";
  dev_class->full_name = "Goodix 55b4 Fingerprint Sensor (Milan)";
  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;
  dev_class->id_table = id_table;
  dev_class->nr_enroll_stages = 12; /* upstream GOODIX_ENROLL_SAMPLES */
  dev_class->temp_hot_seconds = -1;

  dev_class->open = dev_open;
  dev_class->close = dev_close;
  dev_class->enroll = dev_enroll;
  dev_class->verify = dev_verify;
  dev_class->identify = dev_identify;
  dev_class->capture = dev_capture;
  dev_class->cancel = dev_cancel;

  fpi_device_class_auto_initialize_features (dev_class);
}
