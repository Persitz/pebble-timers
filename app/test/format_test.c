// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

#include "format.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

void test_12h_morning_no_leading_zero(void) {
  char buf[8];
  format_clock(buf, sizeof buf, 9, 41, false);
  TEST_ASSERT_EQUAL_STRING("9:41", buf);
}

void test_12h_noon_is_12(void) {
  char buf[8];
  format_clock(buf, sizeof buf, 12, 0, false);
  TEST_ASSERT_EQUAL_STRING("12:00", buf);
}

void test_12h_midnight_is_12(void) {
  char buf[8];
  format_clock(buf, sizeof buf, 0, 5, false);
  TEST_ASSERT_EQUAL_STRING("12:05", buf);
}

void test_12h_afternoon_wraps(void) {
  char buf[8];
  format_clock(buf, sizeof buf, 17, 46, false);
  TEST_ASSERT_EQUAL_STRING("5:46", buf);
}

void test_24h_zero_padded(void) {
  char buf[8];
  format_clock(buf, sizeof buf, 7, 3, true);
  TEST_ASSERT_EQUAL_STRING("07:03", buf);
}

void test_24h_late_evening(void) {
  char buf[8];
  format_clock(buf, sizeof buf, 17, 46, true);
  TEST_ASSERT_EQUAL_STRING("17:46", buf);
}

void test_24h_midnight(void) {
  char buf[8];
  format_clock(buf, sizeof buf, 0, 0, true);
  TEST_ASSERT_EQUAL_STRING("00:00", buf);
}

void test_out_of_range_hour_and_minute_are_clamped(void) {
  char buf[8];
  format_clock(buf, sizeof buf, 99, 99, true);
  TEST_ASSERT_EQUAL_STRING("23:59", buf);
}

// clamp_int's lower-bound branch had zero coverage -- every
// out-of-range case only exercised value > hi. Close it explicitly.
void test_negative_hour_and_minute_are_clamped_to_zero(void) {
  char buf[8];
  format_clock(buf, sizeof buf, -1, -1, true);
  TEST_ASSERT_EQUAL_STRING("00:00", buf);
}

void test_truncates_to_fit_small_buffer(void) {
  char buf[3];
  size_t written = format_clock(buf, sizeof buf, 12, 34, true);
  TEST_ASSERT_EQUAL_size_t(2, written);
  TEST_ASSERT_EQUAL_STRING("12", buf);
}

void test_zero_size_buffer_is_a_no_op(void) {
  char buf[1] = {'x'};
  size_t written = format_clock(buf, 0, 12, 34, true);
  TEST_ASSERT_EQUAL_size_t(0, written);
  TEST_ASSERT_EQUAL_CHAR('x', buf[0]);
}

// --- format_clock_ampm (12h annotation needs an am/pm
// marker since a bare "6:45" can't distinguish the resolver's AM/PM pick) --

void test_ampm_12h_morning_gets_am_suffix(void) {
  char buf[16];
  format_clock_ampm(buf, sizeof buf, 9, 41, false);
  TEST_ASSERT_EQUAL_STRING("9:41 am", buf);
}

void test_ampm_12h_evening_gets_pm_suffix(void) {
  char buf[16];
  format_clock_ampm(buf, sizeof buf, 17, 46, false);
  TEST_ASSERT_EQUAL_STRING("5:46 pm", buf);
}

void test_ampm_12h_noon_is_pm(void) {
  char buf[16];
  format_clock_ampm(buf, sizeof buf, 12, 0, false);
  TEST_ASSERT_EQUAL_STRING("12:00 pm", buf);
}

void test_ampm_12h_midnight_is_am(void) {
  char buf[16];
  format_clock_ampm(buf, sizeof buf, 0, 5, false);
  TEST_ASSERT_EQUAL_STRING("12:05 am", buf);
}

void test_ampm_24h_morning_has_no_suffix(void) {
  char buf[16];
  format_clock_ampm(buf, sizeof buf, 9, 41, true);
  TEST_ASSERT_EQUAL_STRING("09:41", buf);
}

void test_ampm_24h_evening_has_no_suffix(void) {
  char buf[16];
  format_clock_ampm(buf, sizeof buf, 17, 46, true);
  TEST_ASSERT_EQUAL_STRING("17:46", buf);
}

void test_ampm_24h_noon_has_no_suffix(void) {
  char buf[16];
  format_clock_ampm(buf, sizeof buf, 12, 0, true);
  TEST_ASSERT_EQUAL_STRING("12:00", buf);
}

void test_ampm_24h_midnight_has_no_suffix(void) {
  char buf[16];
  format_clock_ampm(buf, sizeof buf, 0, 0, true);
  TEST_ASSERT_EQUAL_STRING("00:00", buf);
}

void test_ampm_out_of_range_hour_is_clamped_before_suffix_pick(void) {
  char buf[16];
  format_clock_ampm(buf, sizeof buf, 99, 30, false);  // clamps to 23 -> pm
  TEST_ASSERT_EQUAL_STRING("11:30 pm", buf);
}

void test_ampm_truncates_to_fit_small_buffer(void) {
  char buf[6];
  size_t written = format_clock_ampm(buf, sizeof buf, 17, 46, false);
  TEST_ASSERT_EQUAL_size_t(5, written);
  TEST_ASSERT_EQUAL_STRING("5:46 ", buf);
}

void test_ampm_zero_size_buffer_is_a_no_op(void) {
  char buf[1] = {'x'};
  size_t written = format_clock_ampm(buf, 0, 17, 46, false);
  TEST_ASSERT_EQUAL_size_t(0, written);
  TEST_ASSERT_EQUAL_CHAR('x', buf[0]);
}

// --- format_split_hms --------------------------------------------------------

void test_split_hms_basic(void) {
  int h, m, s;
  format_split_hms(3723, &h, &m, &s);
  TEST_ASSERT_EQUAL_INT(1, h);
  TEST_ASSERT_EQUAL_INT(2, m);
  TEST_ASSERT_EQUAL_INT(3, s);
}

void test_split_hms_negative_clamps_to_zero(void) {
  int h, m, s;
  format_split_hms(-5, &h, &m, &s);
  TEST_ASSERT_EQUAL_INT(0, h);
  TEST_ASSERT_EQUAL_INT(0, m);
  TEST_ASSERT_EQUAL_INT(0, s);
}

// --- format_hms (numeric-only, no prefix) -----------------------------------

void test_hms_minutes_seconds_no_hour(void) {
  char buf[16];
  format_hms(buf, sizeof buf, 158);  // 2:38
  TEST_ASSERT_EQUAL_STRING("2:38", buf);
}

void test_hms_small_seconds_keeps_leading_zero_on_seconds_only(void) {
  char buf[16];
  format_hms(buf, sizeof buf, 7);
  TEST_ASSERT_EQUAL_STRING("0:07", buf);
}

void test_hms_includes_hour_when_nonzero(void) {
  char buf[16];
  format_hms(buf, sizeof buf, 938);  // 15:38 -- no hour digit, h=0
  TEST_ASSERT_EQUAL_STRING("15:38", buf);
  format_hms(buf, sizeof buf, 3723);  // 1:02:03
  TEST_ASSERT_EQUAL_STRING("1:02:03", buf);
}

void test_hms_negative_clamps_to_zero(void) {
  char buf[16];
  format_hms(buf, sizeof buf, -42);
  TEST_ASSERT_EQUAL_STRING("0:00", buf);
}

// --- format_home_row_value (kind-branched) ---------------------------------

void test_home_row_value_countdown_uses_remaining(void) {
  Timer t = {0};
  t.kind = KIND_COUNTDOWN;
  t.duration_s = 300;
  t.started_at = 1000;
  char buf[16];
  format_home_row_value(buf, sizeof buf, &t, 1060);  // 240s remaining
  TEST_ASSERT_EQUAL_STRING("4:00", buf);
}

void test_home_row_value_stopwatch_uses_elapsed_not_remaining(void) {
  // A post-timer stopwatch's remaining() clamps to a frozen 0:00 forever --
  // the row must use elapsed() instead. Stopwatch rows briefly carried a
  // baked-in "+" prefix, which was reverted -- the format is "15:38", not
  // "+15:38".
  Timer t = {0};
  t.kind = KIND_STOPWATCH;
  t.duration_s = 900;  // original_total_s-equivalent context; irrelevant to elapsed()
  t.started_at = 1000;
  char buf[16];
  format_home_row_value(buf, sizeof buf, &t, 1938);  // 938s elapsed, well past duration_s
  TEST_ASSERT_EQUAL_STRING("15:38", buf);
}

void test_home_row_value_never_negative(void) {
  Timer t = {0};
  t.kind = KIND_COUNTDOWN;
  t.duration_s = 60;
  t.started_at = 1000;
  char buf[16];
  format_home_row_value(buf, sizeof buf, &t, 1200);  // long past zero
  TEST_ASSERT_EQUAL_STRING("0:00", buf);
}

// --- format_hms_prefixed -----------------------------------------------------

void test_hms_prefixed_no_plus_matches_format_hms(void) {
  char buf[16];
  format_hms_prefixed(buf, sizeof buf, 158, false);
  TEST_ASSERT_EQUAL_STRING("2:38", buf);
}

void test_hms_prefixed_with_plus_prepends_plus(void) {
  char buf[16];
  format_hms_prefixed(buf, sizeof buf, 26, true);
  TEST_ASSERT_EQUAL_STRING("+0:26", buf);
}

void test_hms_prefixed_with_plus_and_hours(void) {
  char buf[16];
  format_hms_prefixed(buf, sizeof buf, 3723, true);
  TEST_ASSERT_EQUAL_STRING("+1:02:03", buf);
}

void test_hms_prefixed_truncates_safely_in_small_buffer(void) {
  char buf[4];
  size_t written = format_hms_prefixed(buf, sizeof buf, 3723, true);
  TEST_ASSERT_EQUAL_INT(3, written);
  TEST_ASSERT_EQUAL_STRING("+1:", buf);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_12h_morning_no_leading_zero);
  RUN_TEST(test_12h_noon_is_12);
  RUN_TEST(test_12h_midnight_is_12);
  RUN_TEST(test_12h_afternoon_wraps);
  RUN_TEST(test_24h_zero_padded);
  RUN_TEST(test_24h_late_evening);
  RUN_TEST(test_24h_midnight);
  RUN_TEST(test_out_of_range_hour_and_minute_are_clamped);
  RUN_TEST(test_negative_hour_and_minute_are_clamped_to_zero);
  RUN_TEST(test_truncates_to_fit_small_buffer);
  RUN_TEST(test_zero_size_buffer_is_a_no_op);
  RUN_TEST(test_ampm_12h_morning_gets_am_suffix);
  RUN_TEST(test_ampm_12h_evening_gets_pm_suffix);
  RUN_TEST(test_ampm_12h_noon_is_pm);
  RUN_TEST(test_ampm_12h_midnight_is_am);
  RUN_TEST(test_ampm_24h_morning_has_no_suffix);
  RUN_TEST(test_ampm_24h_evening_has_no_suffix);
  RUN_TEST(test_ampm_24h_noon_has_no_suffix);
  RUN_TEST(test_ampm_24h_midnight_has_no_suffix);
  RUN_TEST(test_ampm_out_of_range_hour_is_clamped_before_suffix_pick);
  RUN_TEST(test_ampm_truncates_to_fit_small_buffer);
  RUN_TEST(test_ampm_zero_size_buffer_is_a_no_op);
  RUN_TEST(test_split_hms_basic);
  RUN_TEST(test_split_hms_negative_clamps_to_zero);
  RUN_TEST(test_hms_minutes_seconds_no_hour);
  RUN_TEST(test_hms_small_seconds_keeps_leading_zero_on_seconds_only);
  RUN_TEST(test_hms_includes_hour_when_nonzero);
  RUN_TEST(test_hms_negative_clamps_to_zero);
  RUN_TEST(test_home_row_value_countdown_uses_remaining);
  RUN_TEST(test_home_row_value_stopwatch_uses_elapsed_not_remaining);
  RUN_TEST(test_home_row_value_never_negative);
  RUN_TEST(test_hms_prefixed_no_plus_matches_format_hms);
  RUN_TEST(test_hms_prefixed_with_plus_prepends_plus);
  RUN_TEST(test_hms_prefixed_with_plus_and_hours);
  RUN_TEST(test_hms_prefixed_truncates_safely_in_small_buffer);
  return UNITY_END();
}
