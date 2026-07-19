// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

#include "store.h"
#include "unity.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

void test_init_is_empty(void) {
  TimerStore s;
  store_init(&s);
  TEST_ASSERT_EQUAL_INT(0, s.count);
}

void test_add_assigns_increasing_cookies_never_zero(void) {
  TimerStore s;
  store_init(&s);
  int32_t c1, c2;
  TEST_ASSERT_TRUE(store_add(&s, KIND_COUNTDOWN, 60, 1000, &c1));
  TEST_ASSERT_TRUE(store_add(&s, KIND_COUNTDOWN, 120, 1000, &c2));
  TEST_ASSERT_NOT_EQUAL(0, c1);
  TEST_ASSERT_TRUE(c2 > c1);
}

void test_add_refuses_past_max_timers(void) {
  TimerStore s;
  store_init(&s);
  int32_t cookie;
  for (int i = 0; i < MAX_TIMERS; i++) {
    TEST_ASSERT_TRUE(store_add(&s, KIND_COUNTDOWN, 60, 1000, &cookie));
  }
  TEST_ASSERT_EQUAL_INT(MAX_TIMERS, s.count);
  // The 17th add must be refused, not silently dropped/grown/overflowed.
  TEST_ASSERT_FALSE(store_add(&s, KIND_COUNTDOWN, 60, 1000, &cookie));
  TEST_ASSERT_EQUAL_INT(MAX_TIMERS, s.count);
}

void test_find_returns_pointer_for_existing(void) {
  TimerStore s;
  store_init(&s);
  int32_t cookie;
  store_add(&s, KIND_COUNTDOWN, 90, 1000, &cookie);
  Timer *t = store_find(&s, cookie);
  TEST_ASSERT_NOT_NULL(t);
  TEST_ASSERT_EQUAL_INT32(90, t->duration_s);
}

// Every create path (dial, voice, preset launch) funnels through this one
// store_add(), so this is the ONE write site that establishes set_total_s --
// "what the user actually dialed" starts out identical to duration_s.
void test_add_sets_set_total_s_to_duration(void) {
  TimerStore s;
  store_init(&s);
  int32_t cookie;
  store_add(&s, KIND_COUNTDOWN, 90, 1000, &cookie);
  Timer *t = store_find(&s, cookie);
  TEST_ASSERT_NOT_NULL(t);
  TEST_ASSERT_EQUAL_INT32(90, t->set_total_s);
}

void test_find_returns_null_for_missing(void) {
  TimerStore s;
  store_init(&s);
  TEST_ASSERT_NULL(store_find(&s, 999));
}

void test_remove_compacts_array(void) {
  TimerStore s;
  store_init(&s);
  int32_t c1, c2, c3;
  store_add(&s, KIND_COUNTDOWN, 10, 1000, &c1);
  store_add(&s, KIND_COUNTDOWN, 20, 1000, &c2);
  store_add(&s, KIND_COUNTDOWN, 30, 1000, &c3);
  TEST_ASSERT_TRUE(store_remove(&s, c2));
  TEST_ASSERT_EQUAL_INT(2, s.count);
  TEST_ASSERT_NOT_NULL(store_find(&s, c1));
  TEST_ASSERT_NULL(store_find(&s, c2));
  TEST_ASSERT_NOT_NULL(store_find(&s, c3));
}

void test_remove_missing_returns_false(void) {
  TimerStore s;
  store_init(&s);
  TEST_ASSERT_FALSE(store_remove(&s, 42));
}

void test_add_after_remove_reaches_capacity_again(void) {
  TimerStore s;
  store_init(&s);
  int32_t cookie;
  for (int i = 0; i < MAX_TIMERS; i++) store_add(&s, KIND_COUNTDOWN, 60, 1000, &cookie);
  int32_t first_cookie = s.timers[0].cookie;
  store_remove(&s, first_cookie);
  TEST_ASSERT_TRUE(store_add(&s, KIND_COUNTDOWN, 60, 1000, &cookie));
  TEST_ASSERT_EQUAL_INT(MAX_TIMERS, s.count);
}

// --- store_set_name ----------------------------------------------------

void test_new_timer_defaults_name_less(void) {
  TimerStore s;
  store_init(&s);
  int32_t cookie;
  store_add(&s, KIND_COUNTDOWN, 60, 1000, &cookie);
  Timer *t = store_find(&s, cookie);
  TEST_ASSERT_EQUAL_STRING("", t->name);
}

void test_set_name_found_sets_the_name(void) {
  TimerStore s;
  store_init(&s);
  int32_t cookie;
  store_add(&s, KIND_COUNTDOWN, 300, 1000, &cookie);
  TEST_ASSERT_TRUE(store_set_name(&s, cookie, "Tea"));
  TEST_ASSERT_EQUAL_STRING("Tea", store_find(&s, cookie)->name);
}

void test_set_name_missing_cookie_returns_false(void) {
  TimerStore s;
  store_init(&s);
  TEST_ASSERT_FALSE(store_set_name(&s, 999, "Tea"));
}

void test_set_name_delegates_cap_truncation_to_timer_name_copy(void) {
  // Not re-deriving the UTF-8-safe truncation rule here (already exhaustively
  // covered by test/timer_test.c's own timer_name_copy tests) -- just proving
  // store_set_name actually routes through it rather than a raw strncpy.
  TimerStore s;
  store_init(&s);
  int32_t cookie;
  store_add(&s, KIND_COUNTDOWN, 60, 1000, &cookie);
  char long_name[64];
  memset(long_name, 'a', sizeof long_name - 1);
  long_name[sizeof long_name - 1] = '\0';
  TEST_ASSERT_TRUE(store_set_name(&s, cookie, long_name));
  TEST_ASSERT_EQUAL_INT(TIMER_NAME_MAX - 1, (int)strlen(store_find(&s, cookie)->name));
}

void test_set_name_empty_clears_an_existing_name(void) {
  TimerStore s;
  store_init(&s);
  int32_t cookie;
  store_add(&s, KIND_COUNTDOWN, 60, 1000, &cookie);
  store_set_name(&s, cookie, "Tea");
  TEST_ASSERT_TRUE(store_set_name(&s, cookie, ""));
  TEST_ASSERT_EQUAL_STRING("", store_find(&s, cookie)->name);
}

void test_name_survives_pause_resume(void) {
  // Lifecycle rule: a name is pure display metadata, untouched by every
  // run-state mutation -- proven directly against store_add()+store_set_name()
  // (the real create path), not just timer.h's mutation functions in
  // isolation (test/timer_test.c covers the rest of the mutation surface).
  TimerStore s;
  store_init(&s);
  int32_t cookie;
  store_add(&s, KIND_COUNTDOWN, 300, 1000, &cookie);
  store_set_name(&s, cookie, "Tea");
  Timer *t = store_find(&s, cookie);
  timer_pause(t, 1010);
  timer_resume(t, 1020);
  TEST_ASSERT_EQUAL_STRING("Tea", t->name);
}

void test_name_cleared_on_remove(void) {
  // The other half of the lifecycle rule: a name is cleared only when the
  // Timer itself is removed -- proven by re-adding a NEW timer into the same
  // now-vacant slot and confirming it starts name-less (store_remove's own
  // compaction fully zeroes the vacated slot, not just count--).
  TimerStore s;
  store_init(&s);
  int32_t cookie;
  store_add(&s, KIND_COUNTDOWN, 60, 1000, &cookie);
  store_set_name(&s, cookie, "Tea");
  store_remove(&s, cookie);

  int32_t new_cookie;
  store_add(&s, KIND_COUNTDOWN, 60, 1000, &new_cookie);
  TEST_ASSERT_EQUAL_STRING("", store_find(&s, new_cookie)->name);
}

// --- store_clear -------------------------------------------------------

void test_clear_resets_count_to_zero(void) {
  TimerStore s;
  store_init(&s);
  int32_t cookie;
  store_add(&s, KIND_COUNTDOWN, 60, 1000, &cookie);
  store_add(&s, KIND_STOPWATCH, 0, 1000, &cookie);
  store_clear(&s);
  TEST_ASSERT_EQUAL_INT(0, s.count);
}

void test_clear_zeroes_the_array(void) {
  TimerStore s;
  store_init(&s);
  int32_t cookie;
  store_add(&s, KIND_COUNTDOWN, 60, 1000, &cookie);
  store_clear(&s);
  // Every slot fully zeroed, not just logically empty via count -- guards
  // against a stale Timer struct being read through a dangling index.
  for (int i = 0; i < MAX_TIMERS; i++) {
    TEST_ASSERT_EQUAL_INT32(0, s.timers[i].cookie);
    TEST_ASSERT_EQUAL_INT32(0, s.timers[i].duration_s);
  }
}

void test_clear_preserves_next_cookie(void) {
  TimerStore s;
  store_init(&s);
  int32_t c1, c2;
  store_add(&s, KIND_COUNTDOWN, 60, 1000, &c1);
  store_add(&s, KIND_COUNTDOWN, 60, 1000, &c2);
  int32_t next_before = s.next_cookie;
  store_clear(&s);
  TEST_ASSERT_EQUAL_INT32(next_before, s.next_cookie);
}

void test_add_after_clear_yields_a_fresh_non_reused_cookie(void) {
  TimerStore s;
  store_init(&s);
  int32_t c1, c2, c3;
  store_add(&s, KIND_COUNTDOWN, 60, 1000, &c1);
  store_add(&s, KIND_COUNTDOWN, 60, 1000, &c2);
  store_clear(&s);
  TEST_ASSERT_TRUE(store_add(&s, KIND_COUNTDOWN, 60, 1000, &c3));
  // The new cookie must never collide with a pre-clear cookie (cookie-reuse
  // discipline -- see store.h's store_clear doc comment).
  TEST_ASSERT_NOT_EQUAL(c1, c3);
  TEST_ASSERT_NOT_EQUAL(c2, c3);
  TEST_ASSERT_TRUE(c3 > c2);
}

void test_lap_pool_keeps_four_stopwatch_histories_independent(void) {
  TimerStore s;
  store_init(&s);
  int32_t cookies[MAX_LAP_SETS];
  for (int i = 0; i < MAX_LAP_SETS; i++) {
    TEST_ASSERT_TRUE(store_add(&s, KIND_STOPWATCH, 0, 1000, &cookies[i]));
    TEST_ASSERT_TRUE(store_add_lap(&s, store_find(&s, cookies[i]), 1010 + i));
  }
  for (int i = 0; i < MAX_LAP_SETS; i++) {
    int32_t lap;
    TEST_ASSERT_TRUE(store_lap_get(&s, store_find(&s, cookies[i]), 0, &lap));
    TEST_ASSERT_EQUAL_INT32(10 + i, lap);
  }
}

void test_lap_pool_refuses_fifth_without_evicting_existing_history(void) {
  TimerStore s;
  store_init(&s);
  int32_t cookies[MAX_LAP_SETS + 1];
  for (int i = 0; i < MAX_LAP_SETS + 1; i++) {
    store_add(&s, KIND_STOPWATCH, 0, 1000, &cookies[i]);
  }
  for (int i = 0; i < MAX_LAP_SETS; i++) {
    TEST_ASSERT_TRUE(store_add_lap(&s, store_find(&s, cookies[i]), 1010 + i));
  }
  TEST_ASSERT_FALSE(store_add_lap(&s, store_find(&s, cookies[MAX_LAP_SETS]), 1020));
  int32_t lap;
  TEST_ASSERT_TRUE(store_lap_get(&s, store_find(&s, cookies[0]), 0, &lap));
  TEST_ASSERT_EQUAL_INT32(10, lap);
}

void test_remove_and_clear_release_lap_sets(void) {
  TimerStore s;
  store_init(&s);
  int32_t first, next;
  store_add(&s, KIND_STOPWATCH, 0, 1000, &first);
  TEST_ASSERT_TRUE(store_add_lap(&s, store_find(&s, first), 1010));
  TEST_ASSERT_TRUE(store_remove(&s, first));
  for (int i = 0; i < MAX_LAP_SETS; i++) {
    store_add(&s, KIND_STOPWATCH, 0, 1000, &next);
    TEST_ASSERT_TRUE(store_add_lap(&s, store_find(&s, next), 1010));
  }
  store_clear(&s);
  store_add(&s, KIND_STOPWATCH, 0, 1000, &next);
  TEST_ASSERT_TRUE(store_add_lap(&s, store_find(&s, next), 1010));
}

void test_restart_releases_a_lap_set_and_can_lap_again(void) {
  TimerStore s;
  store_init(&s);
  int32_t cookies[MAX_LAP_SETS + 1];
  for (int i = 0; i < MAX_LAP_SETS + 1; i++) {
    store_add(&s, KIND_STOPWATCH, 0, 1000, &cookies[i]);
  }
  for (int i = 0; i < MAX_LAP_SETS; i++) {
    TEST_ASSERT_TRUE(store_add_lap(&s, store_find(&s, cookies[i]), 1010));
  }
  store_restart(&s, store_find(&s, cookies[0]), 1020);
  TEST_ASSERT_EQUAL_UINT8(0, store_find(&s, cookies[0])->lap_count);
  TEST_ASSERT_TRUE(store_add_lap(&s, store_find(&s, cookies[MAX_LAP_SETS]), 1030));
  TEST_ASSERT_TRUE(store_remove(&s, cookies[MAX_LAP_SETS]));
  TEST_ASSERT_TRUE(store_add_lap(&s, store_find(&s, cookies[0]), 1040));
}

// --- home_order --------------------------------------------------------------

void test_home_order_running_before_paused_before_stopwatch(void) {
  TimerStore s;
  store_init(&s);
  int32_t running_cookie, paused_cookie, stopwatch_cookie;
  store_add(&s, KIND_COUNTDOWN, 60, 1000, &running_cookie);
  store_add(&s, KIND_COUNTDOWN, 60, 1000, &paused_cookie);
  timer_pause(store_find(&s, paused_cookie), 1010);
  store_add(&s, KIND_STOPWATCH, 0, 1000, &stopwatch_cookie);

  int32_t order[MAX_TIMERS];
  int n = store_home_order(&s, 1020, order, MAX_TIMERS);
  TEST_ASSERT_EQUAL_INT(3, n);
  TEST_ASSERT_EQUAL_INT32(running_cookie, order[0]);
  TEST_ASSERT_EQUAL_INT32(paused_cookie, order[1]);
  TEST_ASSERT_EQUAL_INT32(stopwatch_cookie, order[2]);
}

void test_home_order_running_group_sorted_by_soonest_remaining(void) {
  TimerStore s;
  store_init(&s);
  int32_t far_cookie, soon_cookie;
  store_add(&s, KIND_COUNTDOWN, 300, 1000, &far_cookie);   // 5:00
  store_add(&s, KIND_COUNTDOWN, 60, 1000, &soon_cookie);   // 1:00

  int32_t order[MAX_TIMERS];
  int n = store_home_order(&s, 1000, order, MAX_TIMERS);
  TEST_ASSERT_EQUAL_INT(2, n);
  TEST_ASSERT_EQUAL_INT32(soon_cookie, order[0]);
  TEST_ASSERT_EQUAL_INT32(far_cookie, order[1]);
}

void test_home_order_stopwatch_group_sorted_by_youngest_elapsed(void) {
  TimerStore s;
  store_init(&s);
  int32_t old_cookie, young_cookie;
  store_add(&s, KIND_STOPWATCH, 0, 1000, &old_cookie);    // started earlier -> more elapsed
  store_add(&s, KIND_STOPWATCH, 0, 1500, &young_cookie);  // started later -> less elapsed

  int32_t order[MAX_TIMERS];
  int n = store_home_order(&s, 2000, order, MAX_TIMERS);
  TEST_ASSERT_EQUAL_INT(2, n);
  TEST_ASSERT_EQUAL_INT32(young_cookie, order[0]);
  TEST_ASSERT_EQUAL_INT32(old_cookie, order[1]);
}

void test_home_order_empty_store(void) {
  TimerStore s;
  store_init(&s);
  int32_t order[MAX_TIMERS];
  int n = store_home_order(&s, 1000, order, MAX_TIMERS);
  TEST_ASSERT_EQUAL_INT(0, n);
}

// --- home_row_for_cookie -------------------------------------------------

void test_home_row_for_cookie_finds_the_middle_row(void) {
  TimerStore s;
  store_init(&s);
  int32_t soon_cookie, mid_cookie, far_cookie;
  store_add(&s, KIND_COUNTDOWN, 60, 1000, &soon_cookie);   // 1:00
  store_add(&s, KIND_COUNTDOWN, 180, 1000, &mid_cookie);   // 3:00
  store_add(&s, KIND_COUNTDOWN, 300, 1000, &far_cookie);   // 5:00
  TEST_ASSERT_EQUAL_INT(0, store_home_row_for_cookie(&s, soon_cookie, 1000));
  TEST_ASSERT_EQUAL_INT(1, store_home_row_for_cookie(&s, mid_cookie, 1000));
  TEST_ASSERT_EQUAL_INT(2, store_home_row_for_cookie(&s, far_cookie, 1000));
}

void test_home_row_for_cookie_missing_returns_negative_one(void) {
  TimerStore s;
  store_init(&s);
  int32_t cookie;
  store_add(&s, KIND_COUNTDOWN, 60, 1000, &cookie);
  store_remove(&s, cookie);  // deleted -- e.g. via Actions while its hero was open
  TEST_ASSERT_EQUAL_INT(-1, store_home_row_for_cookie(&s, cookie, 1000));
}

void test_home_row_for_cookie_empty_store_returns_negative_one(void) {
  TimerStore s;
  store_init(&s);
  TEST_ASSERT_EQUAL_INT(-1, store_home_row_for_cookie(&s, 1, 1000));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_init_is_empty);
  RUN_TEST(test_add_assigns_increasing_cookies_never_zero);
  RUN_TEST(test_add_refuses_past_max_timers);
  RUN_TEST(test_find_returns_pointer_for_existing);
  RUN_TEST(test_add_sets_set_total_s_to_duration);
  RUN_TEST(test_find_returns_null_for_missing);
  RUN_TEST(test_remove_compacts_array);
  RUN_TEST(test_remove_missing_returns_false);
  RUN_TEST(test_add_after_remove_reaches_capacity_again);
  RUN_TEST(test_clear_resets_count_to_zero);
  RUN_TEST(test_clear_zeroes_the_array);
  RUN_TEST(test_clear_preserves_next_cookie);
  RUN_TEST(test_add_after_clear_yields_a_fresh_non_reused_cookie);
  RUN_TEST(test_lap_pool_keeps_four_stopwatch_histories_independent);
  RUN_TEST(test_lap_pool_refuses_fifth_without_evicting_existing_history);
  RUN_TEST(test_remove_and_clear_release_lap_sets);
  RUN_TEST(test_restart_releases_a_lap_set_and_can_lap_again);
  RUN_TEST(test_new_timer_defaults_name_less);
  RUN_TEST(test_set_name_found_sets_the_name);
  RUN_TEST(test_set_name_missing_cookie_returns_false);
  RUN_TEST(test_set_name_delegates_cap_truncation_to_timer_name_copy);
  RUN_TEST(test_set_name_empty_clears_an_existing_name);
  RUN_TEST(test_name_survives_pause_resume);
  RUN_TEST(test_name_cleared_on_remove);
  RUN_TEST(test_home_order_running_before_paused_before_stopwatch);
  RUN_TEST(test_home_order_running_group_sorted_by_soonest_remaining);
  RUN_TEST(test_home_order_stopwatch_group_sorted_by_youngest_elapsed);
  RUN_TEST(test_home_order_empty_store);
  RUN_TEST(test_home_row_for_cookie_finds_the_middle_row);
  RUN_TEST(test_home_row_for_cookie_missing_returns_negative_one);
  RUN_TEST(test_home_row_for_cookie_empty_store_returns_negative_one);
  return UNITY_END();
}
