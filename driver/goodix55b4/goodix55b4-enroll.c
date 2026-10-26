/*
 * Goodix 55b4 — enrollment bookkeeping around the Milan transaction
 *
 * Kept free of libfprint so that tests can drive the same steps as the
 * driver (tests/test-goodix55b4-dataset.c).
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "goodix55b4-enroll.h"

#include <string.h>

void
goodix55b4_enroll_start (Goodix55b4Enroll *e)
{
  goodix55b4_enroll_clear (e);
  e->tx = goodix_milan_enrollment_transaction_new ();
}

void
goodix55b4_enroll_clear (Goodix55b4Enroll *e)
{
  g_clear_pointer (&e->tx, goodix_milan_enrollment_transaction_free);
  e->bad_records = 0;
  e->bad_continue = 0;
}

Goodix55b4EnrollStep
goodix55b4_enroll_step (Goodix55b4Enroll *e, const GoodixMilanRuntimeOutput *out)
{
  GoodixMilanEnrollmentResult result;
  GoodixMilanEnrollmentAttemptStatus st;
  gboolean admitted;

  g_return_val_if_fail (e->tx != NULL, GOODIX55B4_ENROLL_REJECTED);

  admitted = goodix_milan_runtime_enrollment_admitted (out);
  /* As upstream device/enroll.c: an admitted sample that fails the
   * enrollment gate breaks an unsaturated rollback streak. */
  if (out->enrollment_sample_admitted && !admitted && e->bad_continue < 3)
    e->bad_continue = 0;
  if (!admitted)
    return GOODIX55B4_ENROLL_REJECTED;

  memset (&result, 0, sizeof (result));
  st = goodix_milan_enrollment_transaction_attempt (
    &e->tx, out->probe_template, &e->bad_records, &e->bad_continue, &result);
  switch (st)
    {
    case GOODIX_MILAN_ENROLLMENT_ACCEPTED:
      return GOODIX55B4_ENROLL_ACCEPTED;
    case GOODIX_MILAN_ENROLLMENT_RETRY_CENTER:
      return GOODIX55B4_ENROLL_RETRY_CENTER;
    case GOODIX_MILAN_ENROLLMENT_RETRY_REMOVE:
    default:
      return GOODIX55B4_ENROLL_RETRY_REMOVE;
    }
}

guint
goodix55b4_enroll_count (const Goodix55b4Enroll *e)
{
  return e->tx ? goodix_milan_enrollment_transaction_count (e->tx) : 0;
}

GBytes *
goodix55b4_enroll_publish (const Goodix55b4Enroll *e)
{
  return e->tx ? goodix_milan_enrollment_transaction_publish (e->tx) : NULL;
}
