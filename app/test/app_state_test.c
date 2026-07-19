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
  return UNITY_END();
}
