// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

#include "app_state.h"

#include <string.h>

void app_state_init(AppState *a) {
  memset(a, 0, sizeof(*a));
}

bool app_state_is_ringing(const AppState *a, int32_t cookie) {
  for (int i = 0; i < a->ringing_count; i++) {
    if (a->ringing[i] == cookie) return true;
  }
  return false;
}

void app_state_ringing_add(AppState *a, int32_t cookie) {
  if (app_state_is_ringing(a, cookie)) return;
  if (a->ringing_count >= MAX_TIMERS) return;  // bounded by the store's own cap
  a->ringing[a->ringing_count++] = cookie;
}

void app_state_ringing_remove(AppState *a, int32_t cookie) {
  for (int i = 0; i < a->ringing_count; i++) {
    if (a->ringing[i] == cookie) {
      for (int j = i; j < a->ringing_count - 1; j++) {
        a->ringing[j] = a->ringing[j + 1];
      }
      a->ringing_count--;
      return;
    }
  }
}

int app_state_scan_firings(AppState *a, TimerStore *store, time_t now, int32_t *out_new_fires,
                            int max_out) {
  int written = 0;
  for (int i = 0; i < store->count; i++) {
    Timer *t = &store->timers[i];
    if (!timer_is_firing(t, now)) continue;
    t->fired = true;
    app_state_ringing_add(a, t->cookie);
    if (written < max_out) {
      out_new_fires[written++] = t->cookie;
    }
  }
  return written;
}

bool app_state_should_rebuzz(time_t alerted_at, time_t now, int32_t overrun_s) {
  int32_t interval =
      (overrun_s >= APP_STATE_REALERT_CAP_S) ? APP_STATE_REALERT_DECAY_S : APP_STATE_REALERT_S;
  return (now - alerted_at) >= interval;
}

bool app_state_realert_pulses_backlight(int32_t overrun_s) {
  return overrun_s < APP_STATE_REALERT_CAP_S;
}

bool app_state_needs_catchup_nav(bool was_fired_before_scan, bool is_fired_after_scan,
                                  bool scan_fired_any_new) {
  // Single-ownership check FIRST, unconditionally -- if the scan already
  // routed navigation for anything this call (possibly a different
  // cookie), that navigation already owns the one startup nav decision; a
  // second call here would silently override it.
  if (scan_fired_any_new) return false;
  return is_fired_after_scan && was_fired_before_scan;
}
