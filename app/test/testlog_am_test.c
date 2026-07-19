// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// Exercises testlog_am.c's retry mechanism (self-scheduled AppTimer retry on
// a SYNCHRONOUS app_message_outbox_begin()/_send() failure) against FFF fakes
// of the tiny AppMessage/AppTimer surface testlog_am.c actually calls
// (test/fakes/pebble.h), compiled via test/Makefile's dedicated
// testlog_am_test rule (NOT the generic lib/-only pattern rule -- this is the
// one adapter file allowed into the host build, specifically for this test).
#include "testlog_am.h"

#include <string.h>

#include "fff.h"
#include "pebble.h"  // test/fakes/pebble.h -- see test/Makefile's -I ordering
#include "unity.h"

DEFINE_FFF_GLOBALS;

FAKE_VALUE_FUNC(AppTimer *, app_timer_register, uint32_t, AppTimerCallback, void *);
FAKE_VALUE_FUNC(bool, app_timer_cancel, AppTimer *);
FAKE_VALUE_FUNC(AppMessageResult, app_message_outbox_begin, DictionaryIterator **);
FAKE_VALUE_FUNC(DictionaryResult, dict_write_cstring, DictionaryIterator *, uint32_t, const char *);
FAKE_VALUE_FUNC(uint32_t, dict_write_end, DictionaryIterator *);
FAKE_VALUE_FUNC(AppMessageResult, app_message_outbox_send);
FAKE_VOID_FUNC(app_message_register_outbox_sent, AppMessageOutboxSent);
FAKE_VOID_FUNC(app_message_register_outbox_failed, AppMessageOutboxFailed);

void setUp(void) {
  RESET_FAKE(app_timer_register);
  RESET_FAKE(app_timer_cancel);
  RESET_FAKE(app_message_outbox_begin);
  RESET_FAKE(dict_write_cstring);
  RESET_FAKE(dict_write_end);
  RESET_FAKE(app_message_outbox_send);
  RESET_FAKE(app_message_register_outbox_sent);
  RESET_FAKE(app_message_register_outbox_failed);
  FFF_RESET_HISTORY();

  // Default: everything succeeds unless a test overrides it below --
  // RESET_FAKE zeroes return_val, and APP_MSG_OK == 0, but set explicitly so
  // this isn't relying on that coincidence.
  app_message_outbox_begin_fake.return_val = APP_MSG_OK;
  app_message_outbox_send_fake.return_val = APP_MSG_OK;

  testlog_init();  // resets testlog_am.c's static queue/in-flight/retry state
}

void tearDown(void) {}

// Simulates the pending retry AppTimer firing: grabs the callback/data
// testlog_am.c actually registered via app_timer_register() and invokes it
// directly -- exactly what the real firmware does when RETRY_DELAY_MS
// elapses, without needing a real AppTimer.
static void fire_pending_retry_timer(void) {
  AppTimerCallback cb = app_timer_register_fake.arg1_val;
  void *data = app_timer_register_fake.arg2_val;
  TEST_ASSERT_NOT_NULL(cb);
  cb(data);
}

// --- happy path (no retry involved) -----------------------------------------

void test_happy_path_sends_immediately_no_retry_scheduled(void) {
  testlog("tag", NULL);

  TEST_ASSERT_EQUAL_INT(1, app_message_outbox_begin_fake.call_count);
  TEST_ASSERT_EQUAL_INT(1, dict_write_cstring_fake.call_count);
  TEST_ASSERT_EQUAL_STRING("tag", dict_write_cstring_fake.arg2_val);
  TEST_ASSERT_EQUAL_INT(1, app_message_outbox_send_fake.call_count);
  TEST_ASSERT_EQUAL_INT(0, app_timer_register_fake.call_count);
}

// --- synchronous begin() failure ---------------------------------------

void test_synchronous_begin_failure_self_schedules_retry(void) {
  AppMessageResult begin_seq[] = {APP_MSG_BUSY, APP_MSG_OK};
  SET_RETURN_SEQ(app_message_outbox_begin, begin_seq, 2);

  testlog("tag", "data");

  // begin() failed synchronously -- no sent_callback/failed_callback will
  // EVER fire for a begin that never actually opened, so nothing but a
  // self-scheduled retry can un-stick this line (the no-stall half of the
  // retry mechanism's guarantee).
  TEST_ASSERT_EQUAL_INT(1, app_message_outbox_begin_fake.call_count);
  TEST_ASSERT_EQUAL_INT(0, dict_write_cstring_fake.call_count);
  TEST_ASSERT_EQUAL_INT(0, app_message_outbox_send_fake.call_count);
  TEST_ASSERT_EQUAL_INT(1, app_timer_register_fake.call_count);

  fire_pending_retry_timer();

  // The retry re-attempts the SAME queued line (no-drop) -- this time begin()
  // succeeds and the send actually goes out.
  TEST_ASSERT_EQUAL_INT(2, app_message_outbox_begin_fake.call_count);
  TEST_ASSERT_EQUAL_INT(1, dict_write_cstring_fake.call_count);
  TEST_ASSERT_EQUAL_STRING("tag:data", dict_write_cstring_fake.arg2_val);
  TEST_ASSERT_EQUAL_INT(1, app_message_outbox_send_fake.call_count);
}

// --- synchronous send() failure -----------------------------------------

void test_synchronous_send_failure_self_schedules_retry(void) {
  AppMessageResult send_seq[] = {APP_MSG_BUSY, APP_MSG_OK};
  SET_RETURN_SEQ(app_message_outbox_send, send_seq, 2);

  testlog("tag", NULL);

  // begin() succeeded but send() itself was rejected synchronously -- same
  // "no callback is coming" reasoning as the begin-failure path.
  TEST_ASSERT_EQUAL_INT(1, app_message_outbox_begin_fake.call_count);
  TEST_ASSERT_EQUAL_INT(1, app_message_outbox_send_fake.call_count);
  TEST_ASSERT_EQUAL_INT(1, app_timer_register_fake.call_count);

  fire_pending_retry_timer();

  TEST_ASSERT_EQUAL_INT(2, app_message_outbox_begin_fake.call_count);
  TEST_ASSERT_EQUAL_INT(2, app_message_outbox_send_fake.call_count);
}

// --- the retry-scheduling guard itself ---------------------------------

void test_repeated_synchronous_failures_do_not_double_schedule_retry_timer(void) {
  app_message_outbox_begin_fake.return_val = APP_MSG_BUSY;

  testlog("first", NULL);
  TEST_ASSERT_EQUAL_INT(1, app_timer_register_fake.call_count);

  // A second testlog() call while a retry is already pending re-attempts
  // try_send_next() immediately (pre-existing behaviour, unrelated to the
  // retry-scheduling fix) and fails synchronously again -- schedule_retry()'s own
  // s_retry_scheduled guard must still refuse to register a SECOND AppTimer.
  testlog("second", NULL);
  TEST_ASSERT_EQUAL_INT(2, app_message_outbox_begin_fake.call_count);
  TEST_ASSERT_EQUAL_INT(1, app_timer_register_fake.call_count);

  // Once the pending retry fires, a subsequent failure is free to schedule
  // its own new retry again (the guard is "one at a time", not "ever").
  app_message_outbox_begin_fake.return_val = APP_MSG_BUSY;
  fire_pending_retry_timer();
  TEST_ASSERT_EQUAL_INT(2, app_timer_register_fake.call_count);
}

// --- outbox_failed (async) still retries the same, un-dropped line ---------

void test_async_outbox_failed_retries_same_line_without_new_timer(void) {
  testlog("tag", NULL);
  TEST_ASSERT_EQUAL_INT(1, app_message_outbox_send_fake.call_count);
  TEST_ASSERT_EQUAL_INT(0, app_timer_register_fake.call_count);

  // Simulate the OS calling back with a failure for the message it DID
  // accept -- this path (pre-existing, not part of the synchronous-failure
  // retry mechanism) drives its retry straight from the callback, no
  // AppTimer needed.
  AppMessageOutboxFailed failed_cb = app_message_register_outbox_failed_fake.arg0_val;
  TEST_ASSERT_NOT_NULL(failed_cb);
  failed_cb(NULL, APP_MSG_BUSY, NULL);

  TEST_ASSERT_EQUAL_INT(2, app_message_outbox_begin_fake.call_count);
  TEST_ASSERT_EQUAL_INT(2, app_message_outbox_send_fake.call_count);
  TEST_ASSERT_EQUAL_INT(0, app_timer_register_fake.call_count);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_happy_path_sends_immediately_no_retry_scheduled);
  RUN_TEST(test_synchronous_begin_failure_self_schedules_retry);
  RUN_TEST(test_synchronous_send_failure_self_schedules_retry);
  RUN_TEST(test_repeated_synchronous_failures_do_not_double_schedule_retry_timer);
  RUN_TEST(test_async_outbox_failed_retries_same_line_without_new_timer);
  return UNITY_END();
}
