// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

#include "voice_outcome.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

// DictationSessionStatus numeric values (pebble.h:2095-2194) -- see
// voice_outcome.c's own doc comment for the full enum-to-int mapping this
// mirrors. Spelled out again here so the test's intent reads standalone.
#define STATUS_SUCCESS 0
#define STATUS_REJECTED 1
#define STATUS_REJECTED_WITH_ERROR 2
#define STATUS_SYSTEM_ABORTED 3
#define STATUS_NO_SPEECH 4
#define STATUS_CONNECTIVITY 5
#define STATUS_DISABLED 6
#define STATUS_INTERNAL_ERROR 7
#define STATUS_RECOGNIZER_ERROR 8

void test_success_with_good_parse_is_voice_ok(void) {
  TEST_ASSERT_EQUAL_INT(VOICE_OK, voice_outcome_for_status(STATUS_SUCCESS, true));
}

void test_success_with_failed_parse_is_parse_fail(void) {
  TEST_ASSERT_EQUAL_INT(VOICE_PARSE_FAIL, voice_outcome_for_status(STATUS_SUCCESS, false));
}

void test_create_failed_sentinel_is_no_phone(void) {
  TEST_ASSERT_EQUAL_INT(VOICE_NO_PHONE, voice_outcome_for_status(VOICE_STATUS_CREATE_FAILED, false));
}

void test_create_failed_sentinel_ignores_parse_ok(void) {
  // parse_ok is only meaningful for status Success -- verify the sentinel
  // path doesn't accidentally key off it.
  TEST_ASSERT_EQUAL_INT(VOICE_NO_PHONE, voice_outcome_for_status(VOICE_STATUS_CREATE_FAILED, true));
}

void test_connectivity_error_is_no_phone(void) {
  TEST_ASSERT_EQUAL_INT(VOICE_NO_PHONE, voice_outcome_for_status(STATUS_CONNECTIVITY, false));
}

void test_no_speech_detected(void) {
  TEST_ASSERT_EQUAL_INT(VOICE_NO_SPEECH, voice_outcome_for_status(STATUS_NO_SPEECH, false));
}

void test_disabled(void) {
  TEST_ASSERT_EQUAL_INT(VOICE_DISABLED, voice_outcome_for_status(STATUS_DISABLED, false));
}

void test_rejected_is_cancelled(void) {
  TEST_ASSERT_EQUAL_INT(VOICE_CANCELLED, voice_outcome_for_status(STATUS_REJECTED, false));
}

void test_rejected_with_error_is_cancelled(void) {
  TEST_ASSERT_EQUAL_INT(VOICE_CANCELLED, voice_outcome_for_status(STATUS_REJECTED_WITH_ERROR, false));
}

void test_system_aborted_is_error(void) {
  TEST_ASSERT_EQUAL_INT(VOICE_ERROR, voice_outcome_for_status(STATUS_SYSTEM_ABORTED, false));
}

void test_internal_error_is_error(void) {
  TEST_ASSERT_EQUAL_INT(VOICE_ERROR, voice_outcome_for_status(STATUS_INTERNAL_ERROR, false));
}

void test_recognizer_error_is_error(void) {
  TEST_ASSERT_EQUAL_INT(VOICE_ERROR, voice_outcome_for_status(STATUS_RECOGNIZER_ERROR, false));
}

void test_unexpected_status_value_fails_soft_to_error(void) {
  // Never crash / never misreport on a value outside the documented 0-8 (or
  // the sentinel) range.
  TEST_ASSERT_EQUAL_INT(VOICE_ERROR, voice_outcome_for_status(99, false));
  TEST_ASSERT_EQUAL_INT(VOICE_ERROR, voice_outcome_for_status(-99, true));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_success_with_good_parse_is_voice_ok);
  RUN_TEST(test_success_with_failed_parse_is_parse_fail);
  RUN_TEST(test_create_failed_sentinel_is_no_phone);
  RUN_TEST(test_create_failed_sentinel_ignores_parse_ok);
  RUN_TEST(test_connectivity_error_is_no_phone);
  RUN_TEST(test_no_speech_detected);
  RUN_TEST(test_disabled);
  RUN_TEST(test_rejected_is_cancelled);
  RUN_TEST(test_rejected_with_error_is_cancelled);
  RUN_TEST(test_system_aborted_is_error);
  RUN_TEST(test_internal_error_is_error);
  RUN_TEST(test_recognizer_error_is_error);
  RUN_TEST(test_unexpected_status_value_fails_soft_to_error);
  return UNITY_END();
}
