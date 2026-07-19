// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

#include "set_machine.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

void test_init_starts_on_minutes_zero(void) {
  SetState s;
  set_machine_init(&s);
  TEST_ASSERT_EQUAL(SET_FIELD_M, s.field);
  TEST_ASSERT_EQUAL_INT(0, s.h);
  TEST_ASSERT_EQUAL_INT(0, s.m);
  TEST_ASSERT_EQUAL_INT(0, s.s);
}

void test_adjust_increments_active_field_only(void) {
  SetState s;
  set_machine_init(&s);
  set_machine_adjust(&s, 1);
  TEST_ASSERT_EQUAL_INT(1, s.m);
  TEST_ASSERT_EQUAL_INT(0, s.s);
  TEST_ASSERT_EQUAL_INT(0, s.h);
}

void test_adjust_minutes_wraps_at_60(void) {
  SetState s;
  set_machine_init(&s);
  s.m = 59;
  set_machine_adjust(&s, 1);
  TEST_ASSERT_EQUAL_INT(0, s.m);
}

void test_adjust_minutes_wraps_below_zero(void) {
  SetState s;
  set_machine_init(&s);
  s.m = 0;
  set_machine_adjust(&s, -1);
  TEST_ASSERT_EQUAL_INT(59, s.m);
}

void test_adjust_seconds_wraps(void) {
  SetState s;
  set_machine_init(&s);
  s.field = SET_FIELD_S;
  s.s = 59;
  set_machine_adjust(&s, 1);
  TEST_ASSERT_EQUAL_INT(0, s.s);
}

void test_adjust_hours_clamps_not_wraps(void) {
  SetState s;
  set_machine_init(&s);
  s.field = SET_FIELD_H;
  s.h = 99;
  set_machine_adjust(&s, 1);
  TEST_ASSERT_EQUAL_INT(99, s.h);
  s.h = 0;
  set_machine_adjust(&s, -1);
  TEST_ASSERT_EQUAL_INT(0, s.h);
}

void test_field_right_walks_h_to_m_to_s_to_start(void) {
  SetState s;
  set_machine_init(&s);
  s.field = SET_FIELD_H;
  TEST_ASSERT_EQUAL(SET_NAV_MOVED, set_machine_field_right(&s));
  TEST_ASSERT_EQUAL(SET_FIELD_M, s.field);
  TEST_ASSERT_EQUAL(SET_NAV_MOVED, set_machine_field_right(&s));
  TEST_ASSERT_EQUAL(SET_FIELD_S, s.field);
  TEST_ASSERT_EQUAL(SET_NAV_START, set_machine_field_right(&s));
  TEST_ASSERT_EQUAL(SET_FIELD_S, s.field);  // stays put at the sentinel
}

void test_field_left_walks_s_to_m_to_h_to_cancel(void) {
  SetState s;
  set_machine_init(&s);
  s.field = SET_FIELD_S;
  TEST_ASSERT_EQUAL(SET_NAV_MOVED, set_machine_field_left(&s));
  TEST_ASSERT_EQUAL(SET_FIELD_M, s.field);
  TEST_ASSERT_EQUAL(SET_NAV_MOVED, set_machine_field_left(&s));
  TEST_ASSERT_EQUAL(SET_FIELD_H, s.field);
  TEST_ASSERT_EQUAL(SET_NAV_CANCEL, set_machine_field_left(&s));
}

void test_duration_s_computes_from_all_fields(void) {
  SetState s = {.h = 1, .m = 2, .s = 3, .field = SET_FIELD_S};
  TEST_ASSERT_EQUAL_INT32(3723, set_machine_duration_s(&s));
}

void test_end_clock_adds_duration_to_now(void) {
  SetState s = {.h = 0, .m = 5, .s = 0, .field = SET_FIELD_M};
  TEST_ASSERT_EQUAL(1300, set_machine_end_clock(&s, 1000));
}

void test_is_stopwatch_true_only_at_zero(void) {
  SetState s;
  set_machine_init(&s);
  TEST_ASSERT_TRUE(set_machine_is_stopwatch(&s));
  s.s = 1;
  TEST_ASSERT_FALSE(set_machine_is_stopwatch(&s));
}

// --- init_from_seconds (Edit interval) --------------------------------------

void test_init_from_seconds_splits_hms_and_starts_on_minutes(void) {
  SetState s;
  set_machine_init_from_seconds(&s, 3723);  // 1:02:03
  TEST_ASSERT_EQUAL_INT(1, s.h);
  TEST_ASSERT_EQUAL_INT(2, s.m);
  TEST_ASSERT_EQUAL_INT(3, s.s);
  TEST_ASSERT_EQUAL(SET_FIELD_M, s.field);  // same "cursor starts on minutes" convention as init()
}

void test_init_from_seconds_zero_matches_plain_init(void) {
  SetState s;
  set_machine_init_from_seconds(&s, 0);
  TEST_ASSERT_EQUAL_INT(0, s.h);
  TEST_ASSERT_EQUAL_INT(0, s.m);
  TEST_ASSERT_EQUAL_INT(0, s.s);
}

void test_init_from_seconds_clamps_absurd_hours(void) {
  SetState s;
  set_machine_init_from_seconds(&s, 999999);  // >99h -- must clamp, not overflow the field
  TEST_ASSERT_EQUAL_INT(99, s.h);
}

void test_init_from_seconds_round_trips_through_duration_s(void) {
  // Priming from a stored duration and reading it straight back must be
  // lossless (this is exactly Edit interval's own "prefill -> unchanged ->
  // commit" no-op path).
  SetState s;
  set_machine_init_from_seconds(&s, 300);  // 5:00, a typical repeat interval
  TEST_ASSERT_EQUAL_INT32(300, set_machine_duration_s(&s));
}

// --- init_from_seconds_kind (presets library: repeat-intent priming) -------

void test_init_from_seconds_kind_splits_hms_and_sets_repeat_intent(void) {
  SetState s;
  set_machine_init_from_seconds_kind(&s, 300, true);  // 5:00, a repeat preset
  TEST_ASSERT_EQUAL_INT(0, s.h);
  TEST_ASSERT_EQUAL_INT(5, s.m);
  TEST_ASSERT_EQUAL_INT(0, s.s);
  TEST_ASSERT_EQUAL(SET_FIELD_M, s.field);
  TEST_ASSERT_TRUE(s.is_repeat);
  TEST_ASSERT_TRUE(set_machine_is_repeat(&s));
}

void test_init_from_seconds_kind_false_matches_plain_init_from_seconds(void) {
  SetState s;
  set_machine_init_from_seconds_kind(&s, 3723, false);  // 1:02:03, a countdown preset
  TEST_ASSERT_EQUAL_INT(1, s.h);
  TEST_ASSERT_EQUAL_INT(2, s.m);
  TEST_ASSERT_EQUAL_INT(3, s.s);
  TEST_ASSERT_FALSE(s.is_repeat);
  TEST_ASSERT_FALSE(set_machine_is_repeat(&s));
}

void test_init_from_seconds_kind_repeat_intent_stays_duration_gated_at_zero(void) {
  // A legitimately-stored preset's duration_s is always >=1 (lib/preset_
  // store.h's own validation), but the duration gate must still hold
  // defensively even if primed with is_repeat=true at 0:00.
  SetState s;
  set_machine_init_from_seconds_kind(&s, 0, true);
  TEST_ASSERT_TRUE(s.is_repeat);                  // raw intent preserved...
  TEST_ASSERT_FALSE(set_machine_is_repeat(&s));   // ...but not effective at 0:00
  TEST_ASSERT_TRUE(set_machine_is_stopwatch(&s));
}

// --- kind toggle (create-repeat) ---------------------------------------------

void test_kind_toggle_available_false_at_zero(void) {
  SetState s;
  set_machine_init(&s);
  TEST_ASSERT_FALSE(set_machine_kind_toggle_available(&s));
}

void test_kind_toggle_available_true_once_duration_ge_1s(void) {
  SetState s;
  set_machine_init(&s);
  s.s = 1;
  TEST_ASSERT_TRUE(set_machine_kind_toggle_available(&s));
}

void test_default_kind_is_countdown_not_repeat(void) {
  SetState s;
  set_machine_init(&s);
  TEST_ASSERT_FALSE(s.is_repeat);
  s.s = 1;
  TEST_ASSERT_FALSE(set_machine_is_repeat(&s));

  SetState s2;
  set_machine_init_from_seconds(&s2, 300);
  TEST_ASSERT_FALSE(s2.is_repeat);
  TEST_ASSERT_FALSE(set_machine_is_repeat(&s2));
}

void test_toggle_kind_is_noop_at_zero(void) {
  SetState s;
  set_machine_init(&s);
  set_machine_toggle_kind(&s);
  TEST_ASSERT_FALSE(set_machine_is_repeat(&s));
  TEST_ASSERT_TRUE(set_machine_is_stopwatch(&s));
}

void test_toggle_kind_flips_effective_repeat_at_ge_1s(void) {
  SetState s;
  set_machine_init(&s);
  s.s = 1;
  TEST_ASSERT_FALSE(set_machine_is_repeat(&s));
  set_machine_toggle_kind(&s);
  TEST_ASSERT_TRUE(set_machine_is_repeat(&s));
  set_machine_toggle_kind(&s);
  TEST_ASSERT_FALSE(set_machine_is_repeat(&s));
}

void test_raw_intent_preserved_across_dial_down_to_zero(void) {
  SetState s;
  set_machine_init(&s);
  s.field = SET_FIELD_S;
  set_machine_adjust(&s, 1);  // duration = 1s
  set_machine_toggle_kind(&s);
  TEST_ASSERT_TRUE(set_machine_is_repeat(&s));

  set_machine_adjust(&s, -1);  // dial back down to 0:00
  TEST_ASSERT_EQUAL_INT32(0, set_machine_duration_s(&s));
  TEST_ASSERT_FALSE(set_machine_is_repeat(&s));
  TEST_ASSERT_TRUE(set_machine_is_stopwatch(&s));

  set_machine_adjust(&s, 1);  // dial back up to 1s -- raw intent restored
  TEST_ASSERT_TRUE(set_machine_is_repeat(&s));
  TEST_ASSERT_FALSE(set_machine_is_stopwatch(&s));
}

void test_is_stopwatch_false_when_effective_repeat(void) {
  SetState s;
  set_machine_init(&s);
  s.s = 1;
  set_machine_toggle_kind(&s);
  TEST_ASSERT_TRUE(set_machine_is_repeat(&s));
  TEST_ASSERT_FALSE(set_machine_is_stopwatch(&s));
}

void test_kind_toggle_does_not_disturb_field_walk_or_duration(void) {
  SetState s;
  set_machine_init(&s);
  s.m = 5;
  set_machine_toggle_kind(&s);  // is_repeat now true (duration = 5:00)
  TEST_ASSERT_EQUAL(SET_FIELD_M, s.field);
  TEST_ASSERT_EQUAL_INT32(300, set_machine_duration_s(&s));
  TEST_ASSERT_EQUAL(SET_NAV_MOVED, set_machine_field_right(&s));
  TEST_ASSERT_EQUAL(SET_FIELD_S, s.field);
  TEST_ASSERT_EQUAL_INT32(300, set_machine_duration_s(&s));
  TEST_ASSERT_TRUE(set_machine_is_repeat(&s));  // toggling kind isn't disturbed by field-walk either
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_init_starts_on_minutes_zero);
  RUN_TEST(test_adjust_increments_active_field_only);
  RUN_TEST(test_adjust_minutes_wraps_at_60);
  RUN_TEST(test_adjust_minutes_wraps_below_zero);
  RUN_TEST(test_adjust_seconds_wraps);
  RUN_TEST(test_adjust_hours_clamps_not_wraps);
  RUN_TEST(test_field_right_walks_h_to_m_to_s_to_start);
  RUN_TEST(test_field_left_walks_s_to_m_to_h_to_cancel);
  RUN_TEST(test_duration_s_computes_from_all_fields);
  RUN_TEST(test_end_clock_adds_duration_to_now);
  RUN_TEST(test_is_stopwatch_true_only_at_zero);
  RUN_TEST(test_init_from_seconds_splits_hms_and_starts_on_minutes);
  RUN_TEST(test_init_from_seconds_zero_matches_plain_init);
  RUN_TEST(test_init_from_seconds_clamps_absurd_hours);
  RUN_TEST(test_init_from_seconds_round_trips_through_duration_s);
  RUN_TEST(test_init_from_seconds_kind_splits_hms_and_sets_repeat_intent);
  RUN_TEST(test_init_from_seconds_kind_false_matches_plain_init_from_seconds);
  RUN_TEST(test_init_from_seconds_kind_repeat_intent_stays_duration_gated_at_zero);
  RUN_TEST(test_kind_toggle_available_false_at_zero);
  RUN_TEST(test_kind_toggle_available_true_once_duration_ge_1s);
  RUN_TEST(test_default_kind_is_countdown_not_repeat);
  RUN_TEST(test_toggle_kind_is_noop_at_zero);
  RUN_TEST(test_toggle_kind_flips_effective_repeat_at_ge_1s);
  RUN_TEST(test_raw_intent_preserved_across_dial_down_to_zero);
  RUN_TEST(test_is_stopwatch_false_when_effective_repeat);
  RUN_TEST(test_kind_toggle_does_not_disturb_field_walk_or_duration);
  return UNITY_END();
}
