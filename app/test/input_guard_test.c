// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// Exercises input_guard.c against FFF fakes of the tiny AppTimer surface it
// calls (test/fakes/pebble.h), compiled via test/Makefile's dedicated
// input_guard_test rule (mirroring hold_test.c's own explicit-rule pattern).
// FFF captures the registered AppTimerCallback so these tests can invoke it
// directly -- simulating "the OS event loop dispatches the pending guard-
// expiry AppTimer" without any real sleeping, the same technique hold_test.c
// uses via its own fire_last_timer() helper.
#include "input_guard.h"

#include "fff.h"
#include "unity.h"

DEFINE_FFF_GLOBALS;

FAKE_VALUE_FUNC(AppTimer *, app_timer_register, uint32_t, AppTimerCallback, void *);
FAKE_VALUE_FUNC(bool, app_timer_cancel, AppTimer *);

void setUp(void) {
  // Clear any leftover static state from the PREVIOUS test first, using
  // whatever fakes are still in effect from it -- its own app_timer_cancel
  // call (if s_timer was left pending) must not count toward THIS test's
  // assertions, so the fakes are only reset to a clean slate afterward.
  input_guard_deinit();

  RESET_FAKE(app_timer_register);
  RESET_FAKE(app_timer_cancel);
  FFF_RESET_HISTORY();
  // A non-NULL dummy handle -- input_guard_arm()'s re-arm/deinit app_timer_
  // cancel() calls are guarded on "!= NULL", so a real handle is needed for
  // those calls to actually happen (never dereferenced, only compared/
  // passed through) -- same convention as hold_test.c's setUp().
  app_timer_register_fake.return_val = (AppTimer *)1;
}

void tearDown(void) {}

static AppTimerCallback last_registered_cb(void) {
  TEST_ASSERT_TRUE(app_timer_register_fake.call_count > 0);
  unsigned idx = app_timer_register_fake.call_count - 1;
  return app_timer_register_fake.arg1_history[idx];
}

void test_arm_sets_blocking_and_registers_one_timer_at_input_guard_ms(void) {
  TEST_ASSERT_FALSE(input_guard_blocking());

  input_guard_arm();

  TEST_ASSERT_TRUE(input_guard_blocking());
  TEST_ASSERT_EQUAL_INT(1, app_timer_register_fake.call_count);
  TEST_ASSERT_EQUAL_UINT32(INPUT_GUARD_MS, app_timer_register_fake.arg0_val);
}

void test_expiry_callback_clears_blocking(void) {
  input_guard_arm();
  TEST_ASSERT_TRUE(input_guard_blocking());

  AppTimerCallback cb = last_registered_cb();
  TEST_ASSERT_NOT_NULL(cb);
  cb(NULL);

  TEST_ASSERT_FALSE(input_guard_blocking());
}

void test_rearm_while_blocking_cancels_prior_timer_and_reregisters(void) {
  input_guard_arm();
  TEST_ASSERT_EQUAL_INT(1, app_timer_register_fake.call_count);
  TEST_ASSERT_EQUAL_INT(0, app_timer_cancel_fake.call_count);

  input_guard_arm();  // a second app-initiated transition inside the window

  TEST_ASSERT_EQUAL_INT(1, app_timer_cancel_fake.call_count);   // prior timer cancelled
  TEST_ASSERT_EQUAL_INT(2, app_timer_register_fake.call_count);  // window restarted
  TEST_ASSERT_TRUE(input_guard_blocking());
}

void test_rearm_after_expiry_does_not_cancel_a_stale_timer(void) {
  // Once the guard has already expired (s_timer cleared by its own expiry
  // callback), a fresh arm must not call app_timer_cancel against a NULL/
  // already-fired handle.
  input_guard_arm();
  AppTimerCallback cb = last_registered_cb();
  cb(NULL);
  TEST_ASSERT_FALSE(input_guard_blocking());

  input_guard_arm();

  TEST_ASSERT_EQUAL_INT(0, app_timer_cancel_fake.call_count);
  TEST_ASSERT_EQUAL_INT(2, app_timer_register_fake.call_count);
  TEST_ASSERT_TRUE(input_guard_blocking());
}

void test_deinit_cancels_pending_timer_and_clears_blocking(void) {
  input_guard_arm();
  TEST_ASSERT_TRUE(input_guard_blocking());

  input_guard_deinit();

  TEST_ASSERT_EQUAL_INT(1, app_timer_cancel_fake.call_count);
  TEST_ASSERT_FALSE(input_guard_blocking());
}

void test_deinit_is_a_no_op_when_nothing_is_armed(void) {
  input_guard_deinit();  // setUp already called this once; a second call must not crash
  TEST_ASSERT_EQUAL_INT(0, app_timer_cancel_fake.call_count);
  TEST_ASSERT_FALSE(input_guard_blocking());
}

void test_blocking_is_false_before_any_arm(void) {
  TEST_ASSERT_FALSE(input_guard_blocking());
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_arm_sets_blocking_and_registers_one_timer_at_input_guard_ms);
  RUN_TEST(test_expiry_callback_clears_blocking);
  RUN_TEST(test_rearm_while_blocking_cancels_prior_timer_and_reregisters);
  RUN_TEST(test_rearm_after_expiry_does_not_cancel_a_stale_timer);
  RUN_TEST(test_deinit_cancels_pending_timer_and_clears_blocking);
  RUN_TEST(test_deinit_is_a_no_op_when_nothing_is_armed);
  RUN_TEST(test_blocking_is_false_before_any_arm);
  return UNITY_END();
}
