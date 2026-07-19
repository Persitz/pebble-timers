// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

#include "sched.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

void test_empty_store_produces_empty_plan(void) {
  TimerStore s;
  store_init(&s);
  SchedPlan plan;
  sched_plan(&s, 1000, &plan);
  TEST_ASSERT_EQUAL_INT(0, plan.request_count);
  TEST_ASSERT_EQUAL_INT(0, plan.unschedulable_count);
}

void test_running_countdown_is_scheduled(void) {
  TimerStore s;
  store_init(&s);
  int32_t cookie;
  store_add(&s, KIND_COUNTDOWN, 300, 1000, &cookie);  // deadline 1300
  SchedPlan plan;
  sched_plan(&s, 1000, &plan);
  TEST_ASSERT_EQUAL_INT(1, plan.request_count);
  TEST_ASSERT_EQUAL_INT32(cookie, plan.requests[0].cookie);
  TEST_ASSERT_EQUAL(1300, plan.requests[0].at);
}

void test_paused_timer_is_excluded_entirely(void) {
  TimerStore s;
  store_init(&s);
  int32_t cookie;
  store_add(&s, KIND_COUNTDOWN, 300, 1000, &cookie);
  timer_pause(store_find(&s, cookie), 1010);
  SchedPlan plan;
  sched_plan(&s, 1000, &plan);
  TEST_ASSERT_EQUAL_INT(0, plan.request_count);
  TEST_ASSERT_FALSE(sched_is_unschedulable(&plan, cookie));  // excluded, not marked unschedulable
}

void test_fired_timer_is_excluded_entirely(void) {
  TimerStore s;
  store_init(&s);
  int32_t cookie;
  store_add(&s, KIND_COUNTDOWN, 300, 1000, &cookie);
  store_find(&s, cookie)->fired = true;
  SchedPlan plan;
  sched_plan(&s, 1000, &plan);
  TEST_ASSERT_EQUAL_INT(0, plan.request_count);
  TEST_ASSERT_FALSE(sched_is_unschedulable(&plan, cookie));
}

void test_stopwatch_is_excluded_entirely(void) {
  TimerStore s;
  store_init(&s);
  int32_t cookie;
  store_add(&s, KIND_STOPWATCH, 0, 1000, &cookie);
  SchedPlan plan;
  sched_plan(&s, 1000, &plan);
  TEST_ASSERT_EQUAL_INT(0, plan.request_count);
  TEST_ASSERT_FALSE(sched_is_unschedulable(&plan, cookie));
}

void test_below_min_lead_is_unschedulable(void) {
  TimerStore s;
  store_init(&s);
  int32_t cookie;
  store_add(&s, KIND_COUNTDOWN, 3, 1000, &cookie);  // deadline 1003, now=1000 -> lead=3s < 5s floor
  SchedPlan plan;
  sched_plan(&s, 1000, &plan);
  TEST_ASSERT_EQUAL_INT(0, plan.request_count);
  TEST_ASSERT_TRUE(sched_is_unschedulable(&plan, cookie));
}

void test_exactly_min_lead_is_schedulable(void) {
  TimerStore s;
  store_init(&s);
  int32_t cookie;
  store_add(&s, KIND_COUNTDOWN, SCHED_MIN_LEAD_S, 1000, &cookie);  // deadline exactly now+SCHED_MIN_LEAD_S
  SchedPlan plan;
  sched_plan(&s, 1000, &plan);
  TEST_ASSERT_EQUAL_INT(1, plan.request_count);
}

void test_spacing_conflict_marks_the_later_one_unschedulable(void) {
  TimerStore s;
  store_init(&s);
  int32_t c1, c2;
  store_add(&s, KIND_COUNTDOWN, 100, 1000, &c1);  // deadline 1100
  store_add(&s, KIND_COUNTDOWN, 130, 1000, &c2);  // deadline 1130 -- only 30s after c1, < 60s spacing
  SchedPlan plan;
  sched_plan(&s, 1000, &plan);
  TEST_ASSERT_EQUAL_INT(1, plan.request_count);
  TEST_ASSERT_EQUAL_INT32(c1, plan.requests[0].cookie);
  TEST_ASSERT_TRUE(sched_is_unschedulable(&plan, c2));
}

void test_spacing_exactly_60s_apart_both_schedule(void) {
  TimerStore s;
  store_init(&s);
  int32_t c1, c2;
  store_add(&s, KIND_COUNTDOWN, 100, 1000, &c1);  // deadline 1100
  store_add(&s, KIND_COUNTDOWN, 160, 1000, &c2);  // deadline 1160 -- exactly 60s later
  SchedPlan plan;
  sched_plan(&s, 1000, &plan);
  TEST_ASSERT_EQUAL_INT(2, plan.request_count);
}

void test_eight_cap_marks_overflow_unschedulable(void) {
  TimerStore s;
  store_init(&s);
  int32_t cookies[9];
  // 9 candidates spaced 100s apart -- none conflict on spacing, only the cap bites.
  for (int i = 0; i < 9; i++) {
    store_add(&s, KIND_COUNTDOWN, 100 + i * 100, 1000, &cookies[i]);
  }
  SchedPlan plan;
  sched_plan(&s, 1000, &plan);
  TEST_ASSERT_EQUAL_INT(SCHED_MAX_WAKEUPS, plan.request_count);
  // the earliest 8 (soonest deadlines) are accepted; the 9th (latest) overflows.
  TEST_ASSERT_TRUE(sched_is_unschedulable(&plan, cookies[8]));
  TEST_ASSERT_FALSE(sched_is_unschedulable(&plan, cookies[0]));
}

void test_repeat_kind_scheduled_same_as_countdown(void) {
  // sched_plan's own candidate filter already reads `kind in {COUNTDOWN,
  // REPEAT}` (identical treatment, no repeat-specific branch); this test
  // exercises that generic acceptance path with KIND_REPEAT specifically.
  // Mirrors test_running_countdown_is_scheduled exactly, just KIND_REPEAT.
  TimerStore s;
  store_init(&s);
  int32_t cookie;
  store_add(&s, KIND_REPEAT, 300, 1000, &cookie);  // deadline 1300
  SchedPlan plan;
  sched_plan(&s, 1000, &plan);
  TEST_ASSERT_EQUAL_INT(1, plan.request_count);
  TEST_ASSERT_EQUAL_INT32(cookie, plan.requests[0].cookie);
  TEST_ASSERT_EQUAL(1300, plan.requests[0].at);
}

void test_repeat_kind_sub_min_lead_next_cycle_is_unschedulable(void) {
  // Same min-lead floor applies regardless of kind: a repeat whose CURRENT
  // cycle is about to re-fire in under SCHED_MIN_LEAD_S (e.g. right after
  // advanceCycle on a short interval) is treated as unschedulable, same as
  // a countdown would be.
  TimerStore s;
  store_init(&s);
  int32_t cookie;
  store_add(&s, KIND_REPEAT, 3, 1000, &cookie);  // deadline 1003, lead=3s < 5s floor
  SchedPlan plan;
  sched_plan(&s, 1000, &plan);
  TEST_ASSERT_EQUAL_INT(0, plan.request_count);
  TEST_ASSERT_TRUE(sched_is_unschedulable(&plan, cookie));
}

void test_short_countdown_15s_is_scheduled(void) {
  // Directly encodes the fixed bug: a backgrounded 15s countdown must now get
  // a scheduled wakeup (previously dropped by the old 30s floor).
  TimerStore s;
  store_init(&s);
  int32_t cookie;
  store_add(&s, KIND_COUNTDOWN, 15, 1000, &cookie);  // deadline 1015
  SchedPlan plan;
  sched_plan(&s, 1000, &plan);
  TEST_ASSERT_EQUAL_INT(1, plan.request_count);
  TEST_ASSERT_EQUAL_INT32(cookie, plan.requests[0].cookie);
  TEST_ASSERT_EQUAL(1015, plan.requests[0].at);
}

void test_short_repeat_15s_is_scheduled(void) {
  // Same fix, KIND_REPEAT -- identical acceptance path.
  TimerStore s;
  store_init(&s);
  int32_t cookie;
  store_add(&s, KIND_REPEAT, 15, 1000, &cookie);  // deadline 1015
  SchedPlan plan;
  sched_plan(&s, 1000, &plan);
  TEST_ASSERT_EQUAL_INT(1, plan.request_count);
  TEST_ASSERT_EQUAL_INT32(cookie, plan.requests[0].cookie);
  TEST_ASSERT_EQUAL(1015, plan.requests[0].at);
}

void test_two_short_countdowns_within_spacing_only_earliest(void) {
  // Proves the min-lead fix does NOT reintroduce a firmware E_RANGE by
  // over-scheduling coexisting shorts: two countdowns 10s apart (both well
  // above the 5s floor, but only 10s apart, well under the 60s spacing rule)
  // must still yield only the earliest scheduled, later marked unschedulable.
  TimerStore s;
  store_init(&s);
  int32_t c_near, c_far;
  store_add(&s, KIND_COUNTDOWN, 15, 1000, &c_near);  // deadline 1015
  store_add(&s, KIND_COUNTDOWN, 25, 1000, &c_far);   // deadline 1025, 10s after c_near, < 60s spacing
  SchedPlan plan;
  sched_plan(&s, 1000, &plan);
  TEST_ASSERT_EQUAL_INT(1, plan.request_count);
  TEST_ASSERT_EQUAL_INT32(c_near, plan.requests[0].cookie);
  TEST_ASSERT_TRUE(sched_is_unschedulable(&plan, c_far));
}

void test_requests_sorted_ascending_by_deadline(void) {
  TimerStore s;
  store_init(&s);
  int32_t c_far, c_near;
  store_add(&s, KIND_COUNTDOWN, 500, 1000, &c_far);
  store_add(&s, KIND_COUNTDOWN, 100, 1000, &c_near);
  SchedPlan plan;
  sched_plan(&s, 1000, &plan);
  TEST_ASSERT_EQUAL_INT(2, plan.request_count);
  TEST_ASSERT_EQUAL_INT32(c_near, plan.requests[0].cookie);
  TEST_ASSERT_EQUAL_INT32(c_far, plan.requests[1].cookie);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_empty_store_produces_empty_plan);
  RUN_TEST(test_running_countdown_is_scheduled);
  RUN_TEST(test_paused_timer_is_excluded_entirely);
  RUN_TEST(test_fired_timer_is_excluded_entirely);
  RUN_TEST(test_stopwatch_is_excluded_entirely);
  RUN_TEST(test_below_min_lead_is_unschedulable);
  RUN_TEST(test_exactly_min_lead_is_schedulable);
  RUN_TEST(test_spacing_conflict_marks_the_later_one_unschedulable);
  RUN_TEST(test_spacing_exactly_60s_apart_both_schedule);
  RUN_TEST(test_eight_cap_marks_overflow_unschedulable);
  RUN_TEST(test_repeat_kind_scheduled_same_as_countdown);
  RUN_TEST(test_repeat_kind_sub_min_lead_next_cycle_is_unschedulable);
  RUN_TEST(test_short_countdown_15s_is_scheduled);
  RUN_TEST(test_short_repeat_15s_is_scheduled);
  RUN_TEST(test_two_short_countdowns_within_spacing_only_earliest);
  RUN_TEST(test_requests_sorted_ascending_by_deadline);
  return UNITY_END();
}
