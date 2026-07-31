// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

#include "app_state.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

void test_init_is_empty(void) {
  AppState a;
  app_state_init(&a);
  TEST_ASSERT_EQUAL_INT(0, a.ringing_count);
}

void test_ringing_add_dedup(void) {
  AppState a;
  app_state_init(&a);
  app_state_ringing_add(&a, 5);
  app_state_ringing_add(&a, 5);
  TEST_ASSERT_EQUAL_INT(1, a.ringing_count);
  TEST_ASSERT_TRUE(app_state_is_ringing(&a, 5));
}

void test_ringing_remove(void) {
  AppState a;
  app_state_init(&a);
  app_state_ringing_add(&a, 5);
  app_state_ringing_add(&a, 6);
  app_state_ringing_remove(&a, 5);
  TEST_ASSERT_FALSE(app_state_is_ringing(&a, 5));
  TEST_ASSERT_TRUE(app_state_is_ringing(&a, 6));
  TEST_ASSERT_EQUAL_INT(1, a.ringing_count);
}

// --- scan_firings ------------------------------------------------------------

void test_scan_firings_detects_new_fire_and_marks_fired(void) {
  TimerStore s;
  store_init(&s);
  int32_t cookie;
  store_add(&s, KIND_COUNTDOWN, 60, 1000, &cookie);
  AppState a;
  app_state_init(&a);

  int32_t fired[MAX_TIMERS];
  int n = app_state_scan_firings(&a, &s, 1060, fired, MAX_TIMERS);
  TEST_ASSERT_EQUAL_INT(1, n);
  TEST_ASSERT_EQUAL_INT32(cookie, fired[0]);
  TEST_ASSERT_TRUE(store_find(&s, cookie)->fired);
  TEST_ASSERT_TRUE(app_state_is_ringing(&a, cookie));
}

void test_scan_firings_no_op_when_nothing_firing(void) {
  TimerStore s;
  store_init(&s);
  int32_t cookie;
  store_add(&s, KIND_COUNTDOWN, 60, 1000, &cookie);
  AppState a;
  app_state_init(&a);

  int32_t fired[MAX_TIMERS];
  int n = app_state_scan_firings(&a, &s, 1030, fired, MAX_TIMERS);
  TEST_ASSERT_EQUAL_INT(0, n);
}

void test_scan_firings_does_not_redetect_already_fired(void) {
  TimerStore s;
  store_init(&s);
  int32_t cookie;
  store_add(&s, KIND_COUNTDOWN, 60, 1000, &cookie);
  AppState a;
  app_state_init(&a);

  int32_t fired[MAX_TIMERS];
  app_state_scan_firings(&a, &s, 1060, fired, MAX_TIMERS);
  int n = app_state_scan_firings(&a, &s, 1200, fired, MAX_TIMERS);  // same timer, later tick
  TEST_ASSERT_EQUAL_INT(0, n);
}

void test_scan_firings_stacks_a_second_fire_into_ringing(void) {
  // Two timers firing on different ticks both end up in `ringing` --
  // scanFirings runs every tick regardless of view.
  TimerStore s;
  store_init(&s);
  int32_t c1, c2;
  store_add(&s, KIND_COUNTDOWN, 60, 1000, &c1);
  store_add(&s, KIND_COUNTDOWN, 100, 1000, &c2);
  AppState a;
  app_state_init(&a);

  int32_t fired[MAX_TIMERS];
  app_state_scan_firings(&a, &s, 1060, fired, MAX_TIMERS);  // c1 fires
  TEST_ASSERT_EQUAL_INT(1, a.ringing_count);
  int n = app_state_scan_firings(&a, &s, 1100, fired, MAX_TIMERS);  // c2 fires
  TEST_ASSERT_EQUAL_INT(1, n);
  TEST_ASSERT_EQUAL_INT32(c2, fired[0]);
  TEST_ASSERT_EQUAL_INT(2, a.ringing_count);
  TEST_ASSERT_TRUE(app_state_is_ringing(&a, c1));
  TEST_ASSERT_TRUE(app_state_is_ringing(&a, c2));
}

void test_scan_firings_ignores_paused_and_stopwatch(void) {
  TimerStore s;
  store_init(&s);
  int32_t paused_cookie, sw_cookie;
  store_add(&s, KIND_COUNTDOWN, 60, 1000, &paused_cookie);
  timer_pause(store_find(&s, paused_cookie), 1010);
  store_add(&s, KIND_STOPWATCH, 0, 1000, &sw_cookie);
  AppState a;
  app_state_init(&a);

  int32_t fired[MAX_TIMERS];
  int n = app_state_scan_firings(&a, &s, 999999, fired, MAX_TIMERS);
  TEST_ASSERT_EQUAL_INT(0, n);
}

void test_scan_firings_detects_repeat_kind_fire(void) {
  // Fire DETECTION itself must be kind-agnostic (timer_is_firing already
  // treats COUNTDOWN/REPEAT identically). Mirrors
  // test_scan_firings_detects_new_fire_and_marks_fired exactly, just
  // KIND_REPEAT -- advancing the CYCLE itself is a separate, explicit user
  // action (Keep going), never automatic on fire (timer_test.c's
  // test_advance_cycle_increments_and_resets covers that mutation directly;
  // this only covers that a repeat crossing its deadline is detected and
  // marked fired+ringing the same way a countdown is).
  TimerStore s;
  store_init(&s);
  int32_t cookie;
  store_add(&s, KIND_REPEAT, 60, 1000, &cookie);
  AppState a;
  app_state_init(&a);

  int32_t fired[MAX_TIMERS];
  int n = app_state_scan_firings(&a, &s, 1060, fired, MAX_TIMERS);
  TEST_ASSERT_EQUAL_INT(1, n);
  TEST_ASSERT_EQUAL_INT32(cookie, fired[0]);
  TEST_ASSERT_TRUE(store_find(&s, cookie)->fired);
  TEST_ASSERT_TRUE(app_state_is_ringing(&a, cookie));
  // cycles is untouched by mere fire-detection -- only advanceRepeatCycle
  // (a distinct, explicit "Keep going" mutation) increments it.
  TEST_ASSERT_EQUAL_INT32(0, store_find(&s, cookie)->cycles);
}

// --- should_rebuzz -------------------------------------------------------

void test_should_rebuzz_false_before_interval(void) {
  TEST_ASSERT_FALSE(app_state_should_rebuzz(1000, 1009, 0));
}

void test_should_rebuzz_true_at_interval(void) {
  TEST_ASSERT_TRUE(app_state_should_rebuzz(1000, 1010, 0));
}

void test_should_rebuzz_false_once_capped(void) {
  TEST_ASSERT_FALSE(app_state_should_rebuzz(1000, 1010, APP_STATE_REALERT_CAP_S));
}

void test_should_rebuzz_true_just_under_cap(void) {
  TEST_ASSERT_TRUE(app_state_should_rebuzz(1000, 1010, APP_STATE_REALERT_CAP_S - 1));
}

// --- decaying cadence past the cap -----------------------------------------
//
// The "last 10s-cadence re-alert before decay kicks in" case is already
// pinned by test_should_rebuzz_true_just_under_cap above. The two cases were
// assertion-identical: the same app_state_should_rebuzz(1000, 1010,
// APP_STATE_REALERT_CAP_S - 1) call and the same TRUE assertion. The
// duplicate was removed rather than kept as a second name for one fact.

void test_should_rebuzz_false_10s_after_alert_once_capped(void) {
  // At/after the cap, a mere 10s since the last alert is NOT enough anymore
  // -- cadence has decayed to 60s. This is the behavioral difference from
  // the old "stop forever at the cap" rule: it no longer returns false
  // forever, but it also no longer re-buzzes at the old 10s rate.
  TEST_ASSERT_FALSE(app_state_should_rebuzz(1000, 1010, APP_STATE_REALERT_CAP_S));
}

void test_should_rebuzz_true_at_first_60s_interval_once_capped(void) {
  // At/after the cap, re-alert resumes at the decayed 60s cadence --
  // indefinitely, never permanently silent.
  TEST_ASSERT_TRUE(app_state_should_rebuzz(1000, 1000 + APP_STATE_REALERT_DECAY_S,
                                            APP_STATE_REALERT_CAP_S));
}

void test_should_rebuzz_true_far_past_cap_still_decays_forever(void) {
  // Long after the cap (e.g. an hour of overrun): still re-alerts every 60s
  // -- no upper bound cuts it off, unlike the old cap-stops-nagging rule.
  TEST_ASSERT_TRUE(app_state_should_rebuzz(1000, 1000 + APP_STATE_REALERT_DECAY_S,
                                            APP_STATE_REALERT_CAP_S + 3600));
}

// --- realert_pulses_backlight ----------------------------------------------

void test_realert_pulses_backlight_true_under_cap(void) {
  TEST_ASSERT_TRUE(app_state_realert_pulses_backlight(APP_STATE_REALERT_CAP_S - 1));
}

void test_realert_pulses_backlight_false_at_cap(void) {
  // The boundary itself: exactly at the cap, the backlight must not pulse
  // -- matches should_rebuzz's own >= cap boundary for switching cadence.
  TEST_ASSERT_FALSE(app_state_realert_pulses_backlight(APP_STATE_REALERT_CAP_S));
}

void test_realert_pulses_backlight_false_well_past_cap(void) {
  TEST_ASSERT_FALSE(app_state_realert_pulses_backlight(APP_STATE_REALERT_CAP_S + 3600));
}

// --- app_state_needs_catchup_nav -------------------------------------------

void test_catchup_nav_true_when_already_fired_before_scan_and_scan_fired_nothing(void) {
  // The genuine catch-up case: fired before AND after (the scan didn't
  // touch THIS cookie, since it was already fired) AND the scan fired
  // nothing else either -- this is the ONE place that should navigate for
  // it; nothing else has claimed the single startup navigation decision.
  TEST_ASSERT_TRUE(app_state_needs_catchup_nav(true, true, false));
}

void test_catchup_nav_false_when_scan_just_fired_it(void) {
  // The double-navigation regression: NOT fired before, IS fired after,
  // scan DID
  // fire something (this same cookie) -- the scan itself already navigated
  // internally. A second navigation here would double-navigate.
  TEST_ASSERT_FALSE(app_state_needs_catchup_nav(false, true, true));
}

void test_catchup_nav_false_when_not_fired_at_all(void) {
  // Not fired before or after, scan fired nothing -- a legitimately early
  // wakeup; the per-second tick will ring it exactly on time. Never
  // navigate here.
  TEST_ASSERT_FALSE(app_state_needs_catchup_nav(false, false, false));
}

void test_catchup_nav_false_when_impossible_state(void) {
  // Fired before but somehow not after cannot happen in practice (nothing
  // un-fires a timer) -- still must not navigate, since the predicate
  // requires BOTH before/after conditions regardless of scan_fired_any_new.
  TEST_ASSERT_FALSE(app_state_needs_catchup_nav(true, false, false));
}

// --- sibling-firing navigation ownership ----------------------------------

void test_catchup_nav_false_when_scan_fired_a_sibling_not_the_launch_cookie(void) {
  // The launch cookie (A) was ALREADY
  // fired before this launch even started (was_fired_before=true) and
  // remains fired after the scan (is_fired_after=true) -- by the OLD
  // two-argument predicate, this alone would return true. But the scan DID
  // fire something this call (scan_fired_any_new=true) -- a DIFFERENT,
  // newly-due SIBLING timer (B), not A -- and already navigated internally
  // for B. Routing again here for A would silently override B's correct
  // navigation with A, a cookie that (being fired in a PRIOR session, not
  // this scan) is not even in the real RAM-only ringing set. Single
  // ownership means this MUST be false regardless of A's own before/after
  // state once the scan has already routed navigation for anything.
  TEST_ASSERT_FALSE(app_state_needs_catchup_nav(true, true, true));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_init_is_empty);
  RUN_TEST(test_ringing_add_dedup);
  RUN_TEST(test_ringing_remove);
  RUN_TEST(test_scan_firings_detects_new_fire_and_marks_fired);
  RUN_TEST(test_scan_firings_no_op_when_nothing_firing);
  RUN_TEST(test_scan_firings_does_not_redetect_already_fired);
  RUN_TEST(test_scan_firings_stacks_a_second_fire_into_ringing);
  RUN_TEST(test_scan_firings_ignores_paused_and_stopwatch);
  RUN_TEST(test_scan_firings_detects_repeat_kind_fire);
  RUN_TEST(test_should_rebuzz_false_before_interval);
  RUN_TEST(test_should_rebuzz_true_at_interval);
  RUN_TEST(test_should_rebuzz_false_once_capped);
  RUN_TEST(test_should_rebuzz_true_just_under_cap);
  RUN_TEST(test_should_rebuzz_false_10s_after_alert_once_capped);
  RUN_TEST(test_should_rebuzz_true_at_first_60s_interval_once_capped);
  RUN_TEST(test_should_rebuzz_true_far_past_cap_still_decays_forever);
  RUN_TEST(test_realert_pulses_backlight_true_under_cap);
  RUN_TEST(test_realert_pulses_backlight_false_at_cap);
  RUN_TEST(test_realert_pulses_backlight_false_well_past_cap);
  RUN_TEST(test_catchup_nav_true_when_already_fired_before_scan_and_scan_fired_nothing);
  RUN_TEST(test_catchup_nav_false_when_scan_just_fired_it);
  RUN_TEST(test_catchup_nav_false_when_not_fired_at_all);
  RUN_TEST(test_catchup_nav_false_when_impossible_state);
  RUN_TEST(test_catchup_nav_false_when_scan_fired_a_sibling_not_the_launch_cookie);
  return UNITY_END();
}
