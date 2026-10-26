/*
 * Goodix 55b4 — enrollment bookkeeping around the Milan transaction
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#pragma once

#include <glib.h>

#include "milan/match/match.h"
#include "milan/runtime.h"

typedef struct
{
  GoodixMilanEnrollmentTransaction *tx;
  guint                             bad_records;
  guint                             bad_continue;
} Goodix55b4Enroll;

typedef enum {
  GOODIX55B4_ENROLL_REJECTED,     /* sample not admitted: ask again */
  GOODIX55B4_ENROLL_ACCEPTED,     /* one more stage done */
  GOODIX55B4_ENROLL_RETRY_CENTER, /* admitted but not taken: center */
  GOODIX55B4_ENROLL_RETRY_REMOVE, /* admitted but not taken: lift */
} Goodix55b4EnrollStep;

void goodix55b4_enroll_start (Goodix55b4Enroll *e);
void goodix55b4_enroll_clear (Goodix55b4Enroll *e);

/* Feeds the engine output of one enroll touch (not cancelled). */
Goodix55b4EnrollStep goodix55b4_enroll_step (Goodix55b4Enroll               *e,
                                             const GoodixMilanRuntimeOutput *out);

guint goodix55b4_enroll_count (const Goodix55b4Enroll *e);

/* The template to store, or NULL. */
GBytes *goodix55b4_enroll_publish (const Goodix55b4Enroll *e);
