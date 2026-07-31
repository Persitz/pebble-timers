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

// Re-buzz every 10s while ringing; after REALERT_CAP_S unacknowledged, decay
// to every 60s, INDEFINITELY, rather than stopping. Unbounded 10s alerts are
// a real cost: every re-alert also pulses the backlight -- see
// app_state_realert_pulses_backlight below -- so "forever at 10s" is a ~30%
// backlight duty cycle with no OS timeout, plus battery. But a missed alarm
// going permanently SILENT after 3 minutes is worse for a timer app. A user
// who was away, asleep, or whose watch was RunLevel_Stationary (vibe motor
// disabled) would never learn the timer fired at all. Decaying to 60s keeps
// it discoverable at a cost sustainable overnight.
#define APP_STATE_REALERT_S 10
#define APP_STATE_REALERT_CAP_S 180
#define APP_STATE_REALERT_DECAY_S 60

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

// True if it's time to re-buzz: before overrun_s reaches REALERT_CAP_S, every
// REALERT_S (10s); at/after the cap, every REALERT_DECAY_S (60s), forever --
// there is no state where this permanently returns false while still
// ringing. The red screen persisting regardless is the caller's/view's
// concern, not this decision's.
//
// KNOWN LIMITATION: the ringing set this decision is driven from (AppState's
// `ringing[]`, via the caller's alerted_at/overrun_s) is RAM-only -- none of
// it survives a cold relaunch. A process killed while an alarm is ringing
// loses the re-alert cadence (decaying or not) entirely; this change only
// improves the app-stays-alive case, not the killed-and-relaunched one.
bool app_state_should_rebuzz(time_t alerted_at, time_t now, int32_t overrun_s);

// True iff a re-buzz decided by app_state_should_rebuzz above should ALSO
// pulse the backlight (the caller's light_enable_interaction() call) --
// true only before overrun_s reaches REALERT_CAP_S. Past the cap, the buzz
// alone is the signal; deliberately NOT lighting the screen on every 60s
// re-alert avoids lighting it all night with no OS timeout of its own.
bool app_state_realert_pulses_backlight(int32_t overrun_s);

// True iff a wakeup-launch cold-launch handler should run its own catch-up
// navigation for a cookie -- i.e. the timer was ALREADY fired (persisted)
// BEFORE this launch's own due-firing scan ran, so the scan itself did
// nothing for THIS cookie (a process killed between firing and the user
// seeing it, then relaunched later).
//
// This function ESTABLISHES single ownership of the one startup navigation
// decision -- it does not just assume it. `scan_fired_any_new` (pass
// scan_and_handle_firings()'s own return value) says whether the scan
// ALREADY routed navigation internally, for some cookie, this same call --
// possibly a DIFFERENT, newly-due sibling timer, not this one. If it did,
// that navigation is already the single correct owner of the startup nav
// decision: routing again for a separately-already-fired cookie that isn't
// even in the real (RAM-only) ringing set would silently override the
// correct target with a wrong one. The superseded two-argument predicate
// used only was_fired_before_scan/is_fired_after_scan, so it could not
// distinguish "the scan fired MY cookie" from "the scan fired a SIBLING
// while I was independently already fired." The scan_fired_any_new argument
// makes navigation ownership an input the current predicate proves rather
// than an assumption outside it.
//
// False whenever: the scan already routed navigation for anything this call
// (single-ownership -- deliberately checked FIRST, unconditionally, so it
// can never be bypassed by reordering the other two conditions); OR the
// cookie never fired at all (a legitimately early wakeup -- the per-second
// tick will ring it on time, per the separate t->fired launch-navigation
// guard); OR the scan itself just fired this exact cookie (its own internal
// call already navigated for it).
//
// Extracted as a pure, host-testable predicate because its caller
// (main.c's init()) has no host-test seam of its own. Testing at this
// decision point requires all three inputs that determine navigation
// ownership.
bool app_state_needs_catchup_nav(bool was_fired_before_scan, bool is_fired_after_scan,
                                  bool scan_fired_any_new);
