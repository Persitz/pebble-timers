// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

#include "sched.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

// sched_plan()/SchedPlan/sched_is_unschedulable() and their tests were
// removed because that algorithm has had no production consumer since
// screen_hero.c switched to wakeup_adapter_is_armed(). Keeping a dead pure
// planner and its own ~16 tests risked a future maintainer mistaking it for
// scheduling or UI truth, the same split-brain shape that caused the earlier
// priority defects. sched_candidates() (below) is the only real
// candidate-selection algorithm now; its own tests cover the equivalent
// kind/paused/fired/min-lead filtering this removed suite used to.

// --- sched_negative_rung_eligible / sched_fallback_at ---------------------

void test_negative_rung_eligible_with_ample_lead(void) {
  TEST_ASSERT_TRUE(sched_negative_rung_eligible(1000 + 3600, 1000));
}

void test_negative_rung_eligible_exactly_at_lead_boundary(void) {
  // requested_at - 60 == now + SCHED_MIN_LEAD_S exactly -- >=, so eligible.
  time_t now = 1000;
  time_t requested_at = now + 60 + SCHED_MIN_LEAD_S;
  TEST_ASSERT_TRUE(sched_negative_rung_eligible(requested_at, now));
}

void test_negative_rung_ineligible_one_second_short(void) {
  time_t now = 1000;
  time_t requested_at = now + 60 + SCHED_MIN_LEAD_S - 1;
  TEST_ASSERT_FALSE(sched_negative_rung_eligible(requested_at, now));
}

void test_fallback_at_uses_negative_first_ladder_when_eligible(void) {
  time_t now = 1000;
  time_t requested_at = now + 3600;
  TEST_ASSERT_EQUAL(requested_at - 60, sched_fallback_at(requested_at, now, 0));
  TEST_ASSERT_EQUAL(requested_at + 60, sched_fallback_at(requested_at, now, 1));
}

void test_fallback_at_uses_forward_only_ladder_when_negative_ineligible(void) {
  time_t now = 1000;
  time_t requested_at = now + 60 + SCHED_MIN_LEAD_S - 1;  // negative rung 1s short
  TEST_ASSERT_EQUAL(requested_at + 60, sched_fallback_at(requested_at, now, 0));
  TEST_ASSERT_EQUAL(requested_at + 120, sched_fallback_at(requested_at, now, 1));
}

// --- sched_spacing_allows --------------------------------------------------

void test_spacing_allows_with_no_accepted_entries(void) {
  TEST_ASSERT_TRUE(sched_spacing_allows(5000, NULL, 0));
}

void test_spacing_allows_exactly_60s_away(void) {
  time_t accepted[] = {1000};
  TEST_ASSERT_TRUE(sched_spacing_allows(1060, accepted, 1));
  TEST_ASSERT_TRUE(sched_spacing_allows(940, accepted, 1));
}

void test_spacing_rejects_59s_away_either_side(void) {
  time_t accepted[] = {1000};
  TEST_ASSERT_FALSE(sched_spacing_allows(1059, accepted, 1));
  TEST_ASSERT_FALSE(sched_spacing_allows(941, accepted, 1));
}

void test_spacing_checks_every_accepted_entry(void) {
  time_t accepted[] = {1000, 2000, 3000};
  TEST_ASSERT_TRUE(sched_spacing_allows(1500, accepted, 3));   // clear of all three
  TEST_ASSERT_FALSE(sched_spacing_allows(2030, accepted, 3));  // 30s from 2000
}

// --- sched_candidates() -- no local-spacing pre-filter --------------------

void test_candidates_empty_store_produces_empty_result(void) {
  TimerStore s;
  store_init(&s);
  SchedCandidates c;
  sched_candidates(&s, 1000, &c);
  TEST_ASSERT_EQUAL_INT(0, c.candidate_count);
  TEST_ASSERT_EQUAL_INT(0, c.excluded_count);
}

void test_candidates_closely_spaced_timers_are_BOTH_candidates(void) {
  // The defining difference from sched_plan(): two of our own timers only
  // 30s apart (well under SCHED_MIN_SPACING_S) must BOTH become real
  // candidates here -- sched_plan() would have greedily excluded the later
  // one. Whether they can really coexist is now a question for real
  // wakeup_schedule() outcomes (the adapter), not this pure layer's guess.
  TimerStore s;
  store_init(&s);
  int32_t c1, c2;
  store_add(&s, KIND_COUNTDOWN, 100, 1000, &c1);  // deadline 1100
  store_add(&s, KIND_COUNTDOWN, 130, 1000, &c2);  // deadline 1130 -- only 30s later
  SchedCandidates out;
  sched_candidates(&s, 1000, &out);
  TEST_ASSERT_EQUAL_INT(2, out.candidate_count);
  TEST_ASSERT_EQUAL_INT32(c1, out.candidates[0].cookie);
  TEST_ASSERT_EQUAL_INT32(c2, out.candidates[1].cookie);
  TEST_ASSERT_EQUAL_INT(0, out.excluded_count);
}

void test_candidates_below_min_lead_is_excluded(void) {
  TimerStore s;
  store_init(&s);
  int32_t cookie;
  store_add(&s, KIND_COUNTDOWN, 3, 1000, &cookie);  // lead=3s < 5s floor
  SchedCandidates out;
  sched_candidates(&s, 1000, &out);
  TEST_ASSERT_EQUAL_INT(0, out.candidate_count);
  TEST_ASSERT_EQUAL_INT(1, out.excluded_count);
  TEST_ASSERT_EQUAL_INT32(cookie, out.excluded[0]);
}

void test_candidates_exactly_min_lead_is_admitted(void) {
  // The exact primary lead boundary (at == now + SCHED_MIN_LEAD_S) was
  // pinned by the deleted sched_plan()
  // suite's test_exactly_min_lead_is_schedulable but had no equivalent on
  // sched_candidates() -- only the below-boundary (excluded) side was
  // covered. sched_candidates()'s own exclusion check is `at <
  // min_lead_boundary`, so exactly-at-boundary must be admitted, not
  // excluded; this fails if that comparison is ever loosened to `<=`.
  TimerStore s;
  store_init(&s);
  int32_t cookie;
  store_add(&s, KIND_COUNTDOWN, SCHED_MIN_LEAD_S, 1000, &cookie);  // deadline exactly now+SCHED_MIN_LEAD_S
  SchedCandidates out;
  sched_candidates(&s, 1000, &out);
  TEST_ASSERT_EQUAL_INT(1, out.candidate_count);
  TEST_ASSERT_EQUAL_INT(0, out.excluded_count);
  TEST_ASSERT_EQUAL_INT32(cookie, out.candidates[0].cookie);
}

void test_candidates_repeat_kind_is_admitted_like_countdown(void) {
  // The deleted sched_plan() suite's
  // test_repeat_kind_scheduled_same_as_countdown proved KIND_REPEAT is
  // admitted via the same `kind in {COUNTDOWN, REPEAT}` condition as
  // KIND_COUNTDOWN. sched_candidates()'s own healthy-timer coverage used
  // only KIND_COUNTDOWN fixtures, so a future edit that narrowed the kind
  // filter to COUNTDOWN-only (or otherwise excluded a healthy repeat) would
  // not fail any current sched_candidates() test without this one.
  TimerStore s;
  store_init(&s);
  int32_t cookie;
  store_add(&s, KIND_REPEAT, 300, 1000, &cookie);  // deadline 1300
  SchedCandidates out;
  sched_candidates(&s, 1000, &out);
  TEST_ASSERT_EQUAL_INT(1, out.candidate_count);
  TEST_ASSERT_EQUAL_INT(0, out.excluded_count);
  TEST_ASSERT_EQUAL_INT32(cookie, out.candidates[0].cookie);
  TEST_ASSERT_EQUAL(1300, out.candidates[0].at);
}

void test_candidates_paused_fired_stopwatch_excluded_entirely(void) {
  TimerStore s;
  store_init(&s);
  int32_t c1, c2, c3;
  store_add(&s, KIND_COUNTDOWN, 300, 1000, &c1);
  timer_pause(store_find(&s, c1), 1010);
  store_add(&s, KIND_COUNTDOWN, 300, 1000, &c2);
  store_find(&s, c2)->fired = true;
  store_add(&s, KIND_STOPWATCH, 0, 1000, &c3);
  SchedCandidates out;
  sched_candidates(&s, 1000, &out);
  TEST_ASSERT_EQUAL_INT(0, out.candidate_count);
  TEST_ASSERT_EQUAL_INT(0, out.excluded_count);  // silently dropped, not "excluded"
}

void test_candidates_no_cap_nine_lead_eligible_timers_all_preserved(void) {
  // This test used to assert that sched_candidates() truncated at
  // SCHED_MAX_WAKEUPS(8) -- that was the
  // BUG (admitting candidates on a count, not on real accepted outcomes),
  // not the desired behavior. Firmware's 8-entry cap is on ACCEPTED
  // entries; a 9th (or up to MAX_TIMERS-th) lead-eligible, non-conflicting
  // timer is perfectly schedulable and must remain a real candidate. Only
  // the adapter (wakeup_adapter.c), watching real accepted-entry counts as
  // it attempts candidates in this same deadline order, may ever decide one
  // is structurally unreachable -- never this pure planning layer.
  TimerStore s;
  store_init(&s);
  int32_t cookies[9];
  for (int i = 0; i < 9; i++) {
    store_add(&s, KIND_COUNTDOWN, 100 + i * 100, 1000, &cookies[i]);
  }
  SchedCandidates out;
  sched_candidates(&s, 1000, &out);
  TEST_ASSERT_EQUAL_INT(9, out.candidate_count);
  TEST_ASSERT_EQUAL_INT(0, out.excluded_count);
  TEST_ASSERT_EQUAL_INT32(cookies[8], out.candidates[8].cookie);
}

void test_candidates_max_timers_lead_eligible_timers_all_preserved(void) {
  // The true ceiling is MAX_TIMERS (the store's own hard size cap), not
  // SCHED_MAX_WAKEUPS -- fill the store to its limit and confirm every one
  // survives as a candidate.
  TimerStore s;
  store_init(&s);
  int32_t cookies[MAX_TIMERS];
  for (int i = 0; i < MAX_TIMERS; i++) {
    store_add(&s, KIND_COUNTDOWN, 100 + i * 100, 1000, &cookies[i]);
  }
  SchedCandidates out;
  sched_candidates(&s, 1000, &out);
  TEST_ASSERT_EQUAL_INT(MAX_TIMERS, out.candidate_count);
  TEST_ASSERT_EQUAL_INT(0, out.excluded_count);
}

void test_candidates_sorted_ascending_by_deadline_regardless_of_insertion_order(void) {
  TimerStore s;
  store_init(&s);
  int32_t c_far, c_near;
  store_add(&s, KIND_COUNTDOWN, 500, 1000, &c_far);
  store_add(&s, KIND_COUNTDOWN, 100, 1000, &c_near);
  SchedCandidates out;
  sched_candidates(&s, 1000, &out);
  TEST_ASSERT_EQUAL_INT(2, out.candidate_count);
  TEST_ASSERT_EQUAL_INT32(c_near, out.candidates[0].cookie);
  TEST_ASSERT_EQUAL_INT32(c_far, out.candidates[1].cookie);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_negative_rung_eligible_with_ample_lead);
  RUN_TEST(test_negative_rung_eligible_exactly_at_lead_boundary);
  RUN_TEST(test_negative_rung_ineligible_one_second_short);
  RUN_TEST(test_fallback_at_uses_negative_first_ladder_when_eligible);
  RUN_TEST(test_fallback_at_uses_forward_only_ladder_when_negative_ineligible);
  RUN_TEST(test_spacing_allows_with_no_accepted_entries);
  RUN_TEST(test_spacing_allows_exactly_60s_away);
  RUN_TEST(test_spacing_rejects_59s_away_either_side);
  RUN_TEST(test_spacing_checks_every_accepted_entry);
  RUN_TEST(test_candidates_empty_store_produces_empty_result);
  RUN_TEST(test_candidates_closely_spaced_timers_are_BOTH_candidates);
  RUN_TEST(test_candidates_below_min_lead_is_excluded);
  RUN_TEST(test_candidates_exactly_min_lead_is_admitted);
  RUN_TEST(test_candidates_repeat_kind_is_admitted_like_countdown);
  RUN_TEST(test_candidates_paused_fired_stopwatch_excluded_entirely);
  RUN_TEST(test_candidates_no_cap_nine_lead_eligible_timers_all_preserved);
  RUN_TEST(test_candidates_max_timers_lead_eligible_timers_all_preserved);
  RUN_TEST(test_candidates_sorted_ascending_by_deadline_regardless_of_insertion_order);
  return UNITY_END();
}
