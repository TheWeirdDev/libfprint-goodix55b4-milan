/*
 * End-to-end tests of the Milan engine on real 55b4 frames, driven like the
 * driver does (goodix55b4-state.c, goodix55b4-enroll.c, milan/runtime.c,
 * milan/print.c, goodix55b4-study.c): enroll, verify, identify, impostors,
 * study.
 *
 * Fingerprints are personal data and are not in the repository. The tests
 * are skipped unless GOODIX55B4_DATASET points to a directory captured with
 * tools/capture_dataset_55b4.py (frames: <finger>-NN.raw, 108x88 LE u16;
 * no-finger frames: setup-<run>-NN.raw). GOODIX55B4_DATASET_MANIFEST may
 * name a file of "<finger> <setup file> <first touch> <last touch>" lines;
 * the default matches the reference set.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>

#include "goodix55b4.h"
#include "goodix55b4-enroll.h"
#include "goodix55b4-state.h"
#include "goodix55b4-study.h"
#include "milan/print.h"
#include "milan/runtime.h"

#define ENROLL_STAGES (12) /* nr_enroll_stages in goodix55b4.c */
#define FRAME_BYTES   (GOODIX55B4_PIXELS * 2)

/* Floors, below the reference set (21/21 genuine and identified, 0/120
 * false accepts; see the test log): a regression
 * in the engine or the glue shows up as a drop under them. */
#define MIN_GENUINE_ACCEPT_RATE (0.85)
#define MIN_IDENTIFY_RATE       (0.85)

static const char *default_manifest =
  /* right-index-00 is from an earlier session whose setup frame was not
   * kept per run; -01..-20 are from run 1792311381. */
  "right-index  setup-1792311381-02.raw 1 20\n"
  "right-middle setup-1792311618-02.raw 0 19\n"
  "left-index   setup-1792311683-02.raw 0 19\n";

typedef struct
{
  gchar     *name;
  guint16   *setup;
  GPtrArray *frames;      /* guint16[GOODIX55B4_PIXELS] */
  guint      enroll_used; /* frames [0, enroll_used) built the template */
  GBytes    *template;
} Finger;

typedef struct
{
  GPtrArray             *fingers;
  Goodix55b4EngineState *engine;
  guint64                epoch;
  const guint16         *setup;  /* setup frame of the current "open" */
  gboolean               opened;
} Dataset;

static Dataset *dataset;

/* ---- loading ---- */

static guint16 *
load_frame (const char *dir, const char *name)
{
  g_autofree gchar *path = g_build_filename (dir, name, NULL);
  g_autofree gchar *data = NULL;
  g_autoptr(GError) error = NULL;
  gsize len;
  guint16 *frame;

  if (!g_file_get_contents (path, &data, &len, &error))
    g_error ("%s", error->message);
  if (len != FRAME_BYTES)
    g_error ("%s: %zu bytes, expected %d", path, len, FRAME_BYTES);
  frame = g_new (guint16, GOODIX55B4_PIXELS);
  for (guint i = 0; i < GOODIX55B4_PIXELS; i++)
    frame[i] = (guint8) data[2 * i] | ((guint8) data[2 * i + 1] << 8);
  return frame;
}

static void
finger_free (Finger *f)
{
  g_free (f->name);
  g_free (f->setup);
  g_ptr_array_unref (f->frames);
  g_clear_pointer (&f->template, g_bytes_unref);
  g_free (f);
}

static Dataset *
dataset_load (const char *dir)
{
  const char *manifest_path = g_getenv ("GOODIX55B4_DATASET_MANIFEST");
  g_autofree gchar *manifest = NULL;
  g_auto(GStrv) lines = NULL;
  Dataset *d = g_new0 (Dataset, 1);

  if (manifest_path)
    {
      g_autoptr(GError) error = NULL;

      if (!g_file_get_contents (manifest_path, &manifest, NULL, &error))
        g_error ("%s", error->message);
    }
  lines = g_strsplit (manifest ? manifest : default_manifest, "\n", -1);

  d->fingers = g_ptr_array_new_with_free_func ((GDestroyNotify) finger_free);
  for (guint l = 0; lines[l]; l++)
    {
      g_auto(GStrv) f = g_strsplit_set (g_strstrip (lines[l]), " \t", -1);
      g_autoptr(GPtrArray) fields = g_ptr_array_new ();
      Finger *finger;
      guint first, last;

      for (guint i = 0; f[i]; i++)
        if (*f[i])
          g_ptr_array_add (fields, f[i]);
      if (fields->len == 0 || *(char *) fields->pdata[0] == '#')
        continue;
      g_assert_cmpuint (fields->len, ==, 4);
      first = atoi (fields->pdata[2]);
      last = atoi (fields->pdata[3]);

      finger = g_new0 (Finger, 1);
      finger->name = g_strdup (fields->pdata[0]);
      finger->setup = load_frame (dir, fields->pdata[1]);
      finger->frames = g_ptr_array_new_with_free_func (g_free);
      for (guint t = first; t <= last; t++)
        {
          g_autofree gchar *name = g_strdup_printf ("%s-%02u.raw", finger->name, t);

          g_ptr_array_add (finger->frames, load_frame (dir, name));
        }
      g_ptr_array_add (d->fingers, finger);
    }
  g_assert_cmpuint (d->fingers->len, >=, 2);

  d->engine = g_new0 (Goodix55b4EngineState, 1);
  goodix55b4_state_init (d->engine);
  return d;
}

/* ---- the driver's engine flow ---- */

/* dev_open: a new no-finger frame is a new setup generation (engine state
 * persisted across opens, as with /var/lib/fprint/goodix55b4). */
static void
sim_open (Dataset *d, const Finger *session)
{
  if (d->setup == session->setup)
    return;
  if (d->opened)
    goodix55b4_state_new_generation (d->engine);
  d->opened = TRUE;
  d->setup = session->setup;
}

/* engine_run + engine_task_done. */
static GoodixMilanRuntimeOutput *
sim_run (Dataset *d, GoodixMilanPreprocessPurpose purpose,
         const guint16 *frame, GPtrArray *gallery)
{
  GoodixMilanRuntimeInput *input;
  GoodixMilanRuntimeOutput *out;

  input = goodix_milan_runtime_input_new (
    ++d->epoch, 1, purpose, &d->engine->state, &d->engine->profile,
    d->setup, frame, GOODIX55B4_TCODE, GOODIX55B4_DAC_HIGH,
    GOODIX55B4_DAC_LOW, GOODIX55B4_SENSOR_SUBTYPE,
    gallery ? (GoodixMilanRuntimeGalleryInput **) gallery->pdata : NULL,
    gallery ? gallery->len : 0);
  g_assert_nonnull (input);
  out = goodix_milan_runtime_run (input);
  goodix_milan_runtime_input_free (input);
  g_assert_nonnull (out);
  g_assert_cmpuint (out->action_epoch, ==, d->epoch);
  goodix55b4_state_apply_output (d->engine, out);
  return out;
}

/* dev_enroll .. enroll_finish, then the print storage round trip. */
static void
sim_enroll (Dataset *d, Finger *f)
{
  Goodix55b4Enroll e = { 0 };
  g_autoptr(GBytes) tpl = NULL;
  g_autoptr(GVariant) stored = NULL;
  g_autoptr(GError) error = NULL;
  guint i;

  sim_open (d, f);
  goodix55b4_enroll_start (&e);
  for (i = 0; i < f->frames->len && goodix55b4_enroll_count (&e) < ENROLL_STAGES; i++)
    {
      GoodixMilanRuntimeOutput *out =
        sim_run (d, GOODIX_MILAN_PURPOSE_ENROLL, f->frames->pdata[i], NULL);

      g_assert_cmpint (out->status, !=, GOODIX_MILAN_RUNTIME_CANCELLED);
      goodix55b4_enroll_step (&e, out);
      goodix_milan_runtime_output_free (out);
    }
  g_test_message ("%s: %u stages from %u touches", f->name,
                  goodix55b4_enroll_count (&e), i);
  g_assert_cmpuint (goodix55b4_enroll_count (&e), ==, ENROLL_STAGES);
  f->enroll_used = i;

  tpl = goodix55b4_enroll_publish (&e);
  goodix55b4_enroll_clear (&e);
  g_assert_nonnull (tpl);

  /* As fprintd stores it (fpi-data) and the driver reads it back. */
  stored = goodix_milan_print_build_data (tpl, &error);
  g_assert_no_error (error);
  g_assert_true (goodix_milan_print_parse_data (stored, &f->template, &error));
  g_assert_no_error (error);
  g_assert_true (g_bytes_equal (tpl, f->template));
}

static GPtrArray *
gallery_of (GBytes *const *templates, guint n)
{
  GPtrArray *g = g_ptr_array_new_with_free_func (
    (GDestroyNotify) goodix_milan_runtime_gallery_input_free);

  for (guint i = 0; i < n; i++)
    g_ptr_array_add (g, goodix_milan_runtime_gallery_input_new (i, templates[i]));
  return g;
}

static Dataset *
get_dataset (void)
{
  const char *dir = g_getenv ("GOODIX55B4_DATASET");

  if (!dir || !*dir)
    {
      g_test_skip ("GOODIX55B4_DATASET not set");
      return NULL;
    }
  if (!dataset)
    {
      dataset = dataset_load (dir);
      for (guint i = 0; i < dataset->fingers->len; i++)
        sim_enroll (dataset, dataset->fingers->pdata[i]);
    }
  return dataset;
}

/* ---- tests ---- */

static void
test_enroll (void)
{
  Dataset *d = get_dataset ();

  if (!d)
    return;
  for (guint i = 0; i < d->fingers->len; i++)
    {
      Finger *f = d->fingers->pdata[i];
      GoodixMilanPrintTemplateInfo info;
      g_autoptr(GError) error = NULL;

      g_assert_true (goodix_milan_print_validate_template (f->template, &info,
                                                           &error));
      g_assert_no_error (error);
      g_assert_cmpuint (info.feature_count, >=, 1);
      /* Some touches must be left to verify with. */
      g_assert_cmpuint (f->enroll_used, <, f->frames->len);
    }
}

static void
test_verify_genuine (void)
{
  Dataset *d = get_dataset ();
  guint match = 0, no_match = 0, retry = 0;

  if (!d)
    return;
  for (guint i = 0; i < d->fingers->len; i++)
    {
      Finger *f = d->fingers->pdata[i];
      g_autoptr(GPtrArray) gallery = gallery_of (&f->template, 1);

      sim_open (d, f);
      for (guint t = f->enroll_used; t < f->frames->len; t++)
        {
          GoodixMilanRuntimeOutput *out =
            sim_run (d, GOODIX_MILAN_PURPOSE_IDENTIFY, f->frames->pdata[t], gallery);

          if (out->status == GOODIX_MILAN_RUNTIME_MATCH)
            match++;
          else if (out->status == GOODIX_MILAN_RUNTIME_RETRY)
            retry++;
          else
            {
              g_assert_cmpint (out->status, ==, GOODIX_MILAN_RUNTIME_NO_MATCH);
              no_match++;
            }
          goodix_milan_runtime_output_free (out);
        }
    }
  g_test_message ("genuine: %u match, %u no match, %u retry", match,
                  no_match, retry);
  g_assert_cmpuint (match + no_match, >, 0);
  g_assert_cmpfloat ((double) match / (match + no_match), >=,
                     MIN_GENUINE_ACCEPT_RATE);
}

static void
test_impostors (void)
{
  Dataset *d = get_dataset ();
  guint tried = 0;

  if (!d)
    return;
  /* Every touch of every other finger against each template: no false
   * accept may happen. */
  for (guint i = 0; i < d->fingers->len; i++)
    {
      Finger *owner = d->fingers->pdata[i];
      g_autoptr(GPtrArray) gallery = gallery_of (&owner->template, 1);

      for (guint j = 0; j < d->fingers->len; j++)
        {
          Finger *probe = d->fingers->pdata[j];

          if (j == i)
            continue;
          sim_open (d, probe);
          for (guint t = 0; t < probe->frames->len; t++)
            {
              GoodixMilanRuntimeOutput *out =
                sim_run (d, GOODIX_MILAN_PURPOSE_IDENTIFY,
                         probe->frames->pdata[t], gallery);

              if (out->status == GOODIX_MILAN_RUNTIME_MATCH)
                g_error ("false accept: %s-%02u matched %s (score %d)",
                         probe->name, t, owner->name, out->score);
              tried++;
              goodix_milan_runtime_output_free (out);
            }
        }
    }
  g_test_message ("impostor attempts: %u, false accepts: 0", tried);
}

static void
test_identify (void)
{
  Dataset *d = get_dataset ();
  g_autofree GBytes **templates = NULL;
  g_autoptr(GPtrArray) gallery = NULL;
  guint right = 0, none = 0, total = 0;

  if (!d)
    return;
  templates = g_new (GBytes *, d->fingers->len);
  for (guint i = 0; i < d->fingers->len; i++)
    templates[i] = ((Finger *) d->fingers->pdata[i])->template;
  gallery = gallery_of (templates, d->fingers->len);

  for (guint i = 0; i < d->fingers->len; i++)
    {
      Finger *f = d->fingers->pdata[i];

      sim_open (d, f);
      for (guint t = f->enroll_used; t < f->frames->len; t++)
        {
          GoodixMilanRuntimeOutput *out =
            sim_run (d, GOODIX_MILAN_PURPOSE_IDENTIFY, f->frames->pdata[t], gallery);

          if (out->status == GOODIX_MILAN_RUNTIME_MATCH)
            {
              /* Never someone else's finger. */
              if (out->winner_index != i)
                g_error ("%s-%02u identified as %s", f->name, t,
                         ((Finger *) d->fingers->pdata[out->winner_index])->name);
              right++;
            }
          else if (out->status != GOODIX_MILAN_RUNTIME_RETRY)
            none++;
          if (out->status != GOODIX_MILAN_RUNTIME_RETRY)
            total++;
          goodix_milan_runtime_output_free (out);
        }
    }
  g_test_message ("identify: %u right, %u not found, 0 wrong", right, none);
  g_assert_cmpuint (total, >, 0);
  g_assert_cmpfloat ((double) right / total, >=, MIN_IDENTIFY_RATE);
}

static void
test_study (void)
{
  Dataset *d = get_dataset ();
  Finger *f, *other;
  g_autoptr(GPtrArray) gallery = NULL;
  g_autoptr(GBytes) studied = NULL;
  g_autoptr(GBytes) loaded = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *dir = NULL;
  g_autofree gchar *key = NULL;
  g_autofree gchar *entry = NULL;

  if (!d)
    return;
  f = d->fingers->pdata[0];
  other = d->fingers->pdata[1];
  gallery = gallery_of (&f->template, 1);

  /* The first genuine match yields the template with the probe studied in
   * (study_store in goodix55b4.c). */
  sim_open (d, f);
  for (guint t = f->enroll_used; t < f->frames->len && !studied; t++)
    {
      GoodixMilanRuntimeOutput *out =
        sim_run (d, GOODIX_MILAN_PURPOSE_IDENTIFY, f->frames->pdata[t], gallery);

      if (out->status == GOODIX_MILAN_RUNTIME_MATCH && out->final_candidate)
        studied = g_bytes_ref (out->final_candidate);
      goodix_milan_runtime_output_free (out);
    }
  if (!studied)
    {
      g_test_skip ("no genuine match produced a studied template");
      return;
    }

  dir = g_dir_make_tmp ("goodix55b4-dataset-XXXXXX", &error);
  g_assert_no_error (error);
  key = goodix55b4_study_key (f->template);
  g_assert_true (goodix55b4_study_save (dir, key, studied, &error));
  g_assert_no_error (error);
  loaded = goodix55b4_study_load (dir, key, &error);
  g_assert_no_error (error);
  g_assert_true (g_bytes_equal (loaded, studied));

  /* The studied template still rejects the other fingers. */
  {
    g_autoptr(GPtrArray) studied_gallery = gallery_of (&loaded, 1);

    sim_open (d, other);
    for (guint t = 0; t < other->frames->len; t++)
      {
        GoodixMilanRuntimeOutput *out =
          sim_run (d, GOODIX_MILAN_PURPOSE_IDENTIFY, other->frames->pdata[t],
                   studied_gallery);

        if (out->status == GOODIX_MILAN_RUNTIME_MATCH)
          g_error ("false accept after study: %s-%02u", other->name, t);
        goodix_milan_runtime_output_free (out);
      }
  }

  entry = g_strconcat (dir, G_DIR_SEPARATOR_S, key, ".bin", NULL);
  g_unlink (entry);
  g_rmdir (dir);
}

int
main (int argc, char **argv)
{
  int ret;

  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/dataset/enroll", test_enroll);
  g_test_add_func ("/dataset/verify-genuine", test_verify_genuine);
  g_test_add_func ("/dataset/impostors", test_impostors);
  g_test_add_func ("/dataset/identify", test_identify);
  g_test_add_func ("/dataset/study", test_study);
  ret = g_test_run ();

  if (dataset)
    {
      g_ptr_array_unref (dataset->fingers);
      g_free (dataset->engine);
      g_free (dataset);
    }
  return ret;
}
