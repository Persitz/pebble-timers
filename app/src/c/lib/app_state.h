// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// Pure app-level state layered on top of TimerStore: the "ringing" set
// (actively-buzzing subset of fired timers -- distinction between fired and
// actively buzzing) + fire detection (must run every tick regardless of the
// current screen) + the re-alert decision.
// NEVER #include <pebble.h>.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "store.h"

// Re-buzz every 10s while ringing, stop nagging (visual persists) after 3 min unacknowledged.
#define APP_STATE_REALERT_S 10
#define APP_STATE_REALERT_CAP_S 180

typedef struct {
  int32_t ringing[MAX_TIMERS];
  int ringing_count;
  time_t alerted_at;  // wall-clock of the last re-alert buzz (alarm view's own cadence baseline)
} AppState;

void app_state_init(AppState *a);

// For every timer in `store` that is newly firing (timer_is_firing), marks
// fired=true and adds its cookie to `ringing` (dedup). Writes newly-fired
// cookies into out_new_fires (up to max_out) and returns how many. MUST be
// called on every tick/wakeup/launch regardless of the current screen --
// firing detection is never view-dependent (only effect emission/display is,
// at the call site).
int app_state_scan_firings(AppState *a, TimerStore *store, time_t now, int32_t *out_new_fires,
                            int max_out);

bool app_state_is_ringing(const AppState *a, int32_t cookie);
void app_state_ringing_add(AppState *a, int32_t cookie);  // dedup
void app_state_ringing_remove(AppState *a, int32_t cookie);

// True if it's time to re-buzz: now - alerted_at >= REALERT_S AND
// overrun_s < REALERT_CAP_S (the cap silences the haptic; the red screen
// still persists -- that's the caller's/view's concern, not this decision's).
bool app_state_should_rebuzz(time_t alerted_at, time_t now, int32_t overrun_s);
