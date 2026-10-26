/*
 * Goodix 55b4 — layout fingerprint of the persisted engine state
 *
 * The state file is a raw dump of GoodixMilanPreprocessState and
 * GoodixMilanProfileState, checked on load by their sizes only. A field
 * moved, retyped or resized within the same total size would load as
 * garbage, so goodix55b4-state.c pins this constant expression over the
 * offset and size of every field: any layout change fails the build until
 * STATE_MAGIC is bumped and the pinned value updated.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#pragma once

#include <stddef.h>

#include "milan/preprocess/state.h"

/* Position-weighted, so reordering two equal fields changes it too. */
#define GOODIX55B4_LAYOUT_FIELD(i, T, f) \
  ((guint32) (i) * ((guint32) offsetof (T, f) * 65599u + \
                    (guint32) sizeof (((T *) 0)->f)))

#define GOODIX55B4_LAYOUT_PROFILE \
  (GOODIX55B4_LAYOUT_FIELD (1, GoodixMilanProfileState, calibration_ready) + \
   GOODIX55B4_LAYOUT_FIELD (2, GoodixMilanProfileState, setup_initialized) + \
   GOODIX55B4_LAYOUT_FIELD (3, GoodixMilanProfileState, setup_refresh_pending) + \
   GOODIX55B4_LAYOUT_FIELD (4, GoodixMilanProfileState, setup_not_ready) + \
   (guint32) sizeof (GoodixMilanProfileState))

#define GOODIX55B4_LAYOUT_NESTED \
  (GOODIX55B4_LAYOUT_FIELD (1, GoodixMilanProfile9ClassCounts, profile9_class1_count) + \
   GOODIX55B4_LAYOUT_FIELD (2, GoodixMilanProfile9ClassCounts, profile9_class2_count) + \
   GOODIX55B4_LAYOUT_FIELD (3, GoodixMilanProfile9ClassCounts, profile9_class3_count) + \
   GOODIX55B4_LAYOUT_FIELD (4, GoodixMilanPostRenderObservations, primary_metric) + \
   GOODIX55B4_LAYOUT_FIELD (5, GoodixMilanPostRenderObservations, fallback_metric) + \
   GOODIX55B4_LAYOUT_FIELD (6, GoodixMilanPostRenderObservations, disagreement) + \
   GOODIX55B4_LAYOUT_FIELD (7, GoodixMilanPostRenderObservations, component_score) + \
   GOODIX55B4_LAYOUT_FIELD (8, GoodixMilanPostRenderObservations, component_flag) + \
   GOODIX55B4_LAYOUT_FIELD (9, GoodixMilanPostRenderObservations, quality_gate) + \
   GOODIX55B4_LAYOUT_FIELD (10, GoodixMilanPostRenderObservations, update_applied) + \
   GOODIX55B4_LAYOUT_FIELD (11, GoodixMilanPostRenderObservations, status) + \
   GOODIX55B4_LAYOUT_FIELD (12, GoodixMilanExtractionClassificationState, retained_class_planes) + \
   GOODIX55B4_LAYOUT_FIELD (13, GoodixMilanExtractionClassificationState, retained_count) + \
   GOODIX55B4_LAYOUT_FIELD (14, GoodixMilanExtractionClassificationState, high_class_hysteresis) + \
   GOODIX55B4_LAYOUT_FIELD (15, GoodixMilanExtractionPersistenceState, retained_class_planes) + \
   GOODIX55B4_LAYOUT_FIELD (16, GoodixMilanExtractionPersistenceState, retained_count) + \
   GOODIX55B4_LAYOUT_FIELD (17, GoodixMilanExtractionAuxiliaryState, primary_histogram_state) + \
   GOODIX55B4_LAYOUT_FIELD (18, GoodixMilanExtractionAuxiliaryState, promoted_secondary_histogram_state))

#define GOODIX55B4_LAYOUT_PREPROCESS_A \
  (GOODIX55B4_LAYOUT_FIELD (1, GoodixMilanPreprocessState, setup_map) + \
   GOODIX55B4_LAYOUT_FIELD (2, GoodixMilanPreprocessState, calibration_map) + \
   GOODIX55B4_LAYOUT_FIELD (3, GoodixMilanPreprocessState, coarse_reference) + \
   GOODIX55B4_LAYOUT_FIELD (4, GoodixMilanPreprocessState, auxiliary_gain_map) + \
   GOODIX55B4_LAYOUT_FIELD (5, GoodixMilanPreprocessState, secondary_auxiliary_gain_map) + \
   GOODIX55B4_LAYOUT_FIELD (6, GoodixMilanPreprocessState, application_gain_map) + \
   GOODIX55B4_LAYOUT_FIELD (7, GoodixMilanPreprocessState, sample_count) + \
   GOODIX55B4_LAYOUT_FIELD (8, GoodixMilanPreprocessState, stable_count) + \
   GOODIX55B4_LAYOUT_FIELD (9, GoodixMilanPreprocessState, update_state) + \
   GOODIX55B4_LAYOUT_FIELD (10, GoodixMilanPreprocessState, auxiliary_sample_count) + \
   GOODIX55B4_LAYOUT_FIELD (11, GoodixMilanPreprocessState, profile9_history_count) + \
   GOODIX55B4_LAYOUT_FIELD (12, GoodixMilanPreprocessState, profile9_history_update_count) + \
   GOODIX55B4_LAYOUT_FIELD (13, GoodixMilanPreprocessState, profile9_history_mask_threshold) + \
   GOODIX55B4_LAYOUT_FIELD (14, GoodixMilanPreprocessState, profile9_history_mask_average) + \
   GOODIX55B4_LAYOUT_FIELD (15, GoodixMilanPreprocessState, profile9_history_reference))

#define GOODIX55B4_LAYOUT_PREPROCESS_B \
  (GOODIX55B4_LAYOUT_FIELD (16, GoodixMilanPreprocessState, profile9_reference_age) + \
   GOODIX55B4_LAYOUT_FIELD (17, GoodixMilanPreprocessState, profile9_component_age) + \
   GOODIX55B4_LAYOUT_FIELD (18, GoodixMilanPreprocessState, profile9_class_counts) + \
   GOODIX55B4_LAYOUT_FIELD (19, GoodixMilanPreprocessState, primary_contrast) + \
   GOODIX55B4_LAYOUT_FIELD (20, GoodixMilanPreprocessState, primary_contrast_valid) + \
   GOODIX55B4_LAYOUT_FIELD (21, GoodixMilanPreprocessState, selected_refined) + \
   GOODIX55B4_LAYOUT_FIELD (22, GoodixMilanPreprocessState, post_render) + \
   GOODIX55B4_LAYOUT_FIELD (23, GoodixMilanPreprocessState, extraction_classification) + \
   GOODIX55B4_LAYOUT_FIELD (24, GoodixMilanPreprocessState, extraction_persistence) + \
   GOODIX55B4_LAYOUT_FIELD (25, GoodixMilanPreprocessState, extraction_auxiliary) + \
   GOODIX55B4_LAYOUT_FIELD (26, GoodixMilanPreprocessState, application_gain_initialized) + \
   GOODIX55B4_LAYOUT_FIELD (27, GoodixMilanPreprocessState, extraction_auxiliary_valid) + \
   GOODIX55B4_LAYOUT_FIELD (28, GoodixMilanPreprocessState, extraction_auxiliary_workspace) + \
   GOODIX55B4_LAYOUT_FIELD (29, GoodixMilanPreprocessState, profile9_classifier_initialized) + \
   (guint32) sizeof (GoodixMilanPreprocessState))

#define GOODIX55B4_STATE_LAYOUT \
  ((guint32) (GOODIX55B4_LAYOUT_PROFILE * 3u + GOODIX55B4_LAYOUT_NESTED * 5u + \
              GOODIX55B4_LAYOUT_PREPROCESS_A * 7u + \
              GOODIX55B4_LAYOUT_PREPROCESS_B * 11u))
