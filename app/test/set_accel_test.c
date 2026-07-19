// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

#include "set_accel.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

void test_no_step_before_delay_elapses(void) {
  TEST_ASSERT_EQUAL_INT32(0, set_accel_step(0, 0));
  TEST_ASSERT_EQUAL_INT32(0, set_accel_step(399, 0));
}

void test_first_step_due_exactly_at_delay(void) {
  TEST_ASSERT_EQUAL_INT32(1, set_accel_step(400, 0));
}

void test_no_new_step_before_next_normal_interval(void) {
  TEST_ASSERT_EQUAL_INT32(0, set_accel_step(549, 1));  // 1 already applied, next due at 550
}

void test_second_step_due_at_normal_rate_boundary(void) {
  TEST_ASSERT_EQUAL_INT32(1, set_accel_step(550, 1));
}

void test_multiple_steps_due_at_once_if_caller_polled_late(void) {
  // due at 400 = step1, 550 = step2, 700 = step3; if tick_count is still 0 at
  // elapsed=700, all 3 are due in one call (tick-rate independence).
  TEST_ASSERT_EQUAL_INT32(3, set_accel_step(700, 0));
}

void test_accel_switches_to_fast_rate_after_2000ms(void) {
  // steps_at_accel_boundary = 1 + (2000-400)/150 = 1 + 10 = 11, due exactly at elapsed=2000
  TEST_ASSERT_EQUAL_INT32(11, set_accel_step(2000, 0));
  // one more fast-rate (65ms) step due at 2065
  TEST_ASSERT_EQUAL_INT32(1, set_accel_step(2065, 11));
  TEST_ASSERT_EQUAL_INT32(0, set_accel_step(2129, 12));
  TEST_ASSERT_EQUAL_INT32(1, set_accel_step(2130, 12));
}

void test_already_caught_up_returns_zero(void) {
  TEST_ASSERT_EQUAL_INT32(0, set_accel_step(400, 1));
  TEST_ASSERT_EQUAL_INT32(0, set_accel_step(1000, 100));  // tick_count ahead of due -- never negative
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_no_step_before_delay_elapses);
  RUN_TEST(test_first_step_due_exactly_at_delay);
  RUN_TEST(test_no_new_step_before_next_normal_interval);
  RUN_TEST(test_second_step_due_at_normal_rate_boundary);
  RUN_TEST(test_multiple_steps_due_at_once_if_caller_polled_late);
  RUN_TEST(test_accel_switches_to_fast_rate_after_2000ms);
  RUN_TEST(test_already_caught_up_returns_zero);
  return UNITY_END();
}
