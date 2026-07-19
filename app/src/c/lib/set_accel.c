// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

#include "set_accel.h"

// Total number of +-1 steps that should have been applied by `elapsed_ms`
// since the button went down, given the delay->rate->accel curve. Step 1 is
// "due" at the moment the delay elapses (apply one adjust immediately on
// down, then repeat kicks in after the dwell).
static int32_t total_due(int32_t elapsed_ms) {
  if (elapsed_ms < SET_REPEAT_DELAY_MS) return 0;

  if (elapsed_ms < SET_ACCEL_AFTER_MS) {
    int32_t t = elapsed_ms - SET_REPEAT_DELAY_MS;
    return 1 + t / SET_REPEAT_MS;
  }

  int32_t normal_span = SET_ACCEL_AFTER_MS - SET_REPEAT_DELAY_MS;
  int32_t steps_at_accel_boundary = 1 + normal_span / SET_REPEAT_MS;
  int32_t fast_elapsed = elapsed_ms - SET_ACCEL_AFTER_MS;
  return steps_at_accel_boundary + fast_elapsed / SET_REPEAT_FAST_MS;
}

int32_t set_accel_step(int32_t hold_elapsed_ms, int32_t tick_count) {
  int32_t due = total_due(hold_elapsed_ms);
  int32_t delta = due - tick_count;
  return delta > 0 ? delta : 0;
}
