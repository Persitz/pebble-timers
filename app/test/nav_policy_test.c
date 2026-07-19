// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

#include "nav_policy.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

// --- nav_on_fire -----------------------------------------------------------
// nav_on_fire returns only the take-over decision, as a bool. render_stack()
// (nav.c) unconditionally hold_cancel()s on every real window-stack
// transition, so a separate cancel-hold flag would have no independent code
// path.

void test_on_fire_from_home_takes_over(void) {
  TEST_ASSERT_TRUE(nav_on_fire((ViewDesc){.kind = VD_HOME}));
}

void test_on_fire_from_hero_takes_over(void) {
  TEST_ASSERT_TRUE(nav_on_fire((ViewDesc){.kind = VD_HERO, .cookie = 3}));
}

void test_on_fire_from_set_takes_over(void) {
  TEST_ASSERT_TRUE(nav_on_fire((ViewDesc){.kind = VD_SET}));
}

void test_on_fire_while_already_in_alarm_does_not_take_over_or_recapture(void) {
  // A second fire while already alarmed just merges into ringing[] -- no
  // new take-over.
  TEST_ASSERT_FALSE(nav_on_fire((ViewDesc){.kind = VD_ALARM, .cookie = 1}));
}

void test_on_fire_while_in_alarm_options_also_does_not_take_over(void) {
  TEST_ASSERT_FALSE(nav_on_fire((ViewDesc){.kind = VD_ALARM_OPTIONS, .cookie = 1}));
}

void test_on_fire_while_in_ringing_list_also_does_not_take_over(void) {
  // A 3rd/4th simultaneous fire while the ringing list is already up
  // just grows its row count -- no take-over.
  TEST_ASSERT_FALSE(nav_on_fire((ViewDesc){.kind = VD_RINGING_LIST}));
}

// The voice preview is NOT an alarm view -- a fire while on it must pre-empt
// (take over), same as every other non-alarm-family screen (HOME/HERO/SET/
// ACTIONS/LAPS/LIBRARY/...). Unlike every other non-alarm-family screen, the
// captured VD_VOICE resume is then EXCLUDED at resolve time -- see
// test_resolve_voice_resume_excluded_returns_home below -- so the take-over
// still happens (this test is unaffected), only the RESOLVE destination
// changed.
void test_on_fire_from_voice_takes_over(void) {
  TEST_ASSERT_TRUE(nav_on_fire((ViewDesc){.kind = VD_VOICE}));
}

// --- nav_on_resolve (dead-reference guard) ----------------------------------

void test_resolve_no_resume_goes_home(void) {
  TimerStore s;
  store_init(&s);
  ViewDesc target = nav_on_resolve((ViewDesc){0}, false, &s);
  TEST_ASSERT_EQUAL(VD_HOME, target.kind);
}

void test_resolve_home_resume_is_always_coherent(void) {
  TimerStore s;
  store_init(&s);
  ViewDesc target = nav_on_resolve((ViewDesc){.kind = VD_HOME}, true, &s);
  TEST_ASSERT_EQUAL(VD_HOME, target.kind);
}

void test_resolve_hero_resume_alive_timer_is_preserved(void) {
  TimerStore s;
  store_init(&s);
  int32_t cookie;
  store_add(&s, KIND_COUNTDOWN, 60, 1000, &cookie);
  ViewDesc target = nav_on_resolve((ViewDesc){.kind = VD_HERO, .cookie = cookie}, true, &s);
  TEST_ASSERT_EQUAL(VD_HERO, target.kind);
  TEST_ASSERT_EQUAL_INT32(cookie, target.cookie);
}

void test_resolve_hero_resume_deleted_timer_falls_back_to_home(void) {
  // The dead-reference guard: the resume target was itself the timer
  // resolved (Done/removed) while the alarm was open.
  TimerStore s;
  store_init(&s);
  ViewDesc target = nav_on_resolve((ViewDesc){.kind = VD_HERO, .cookie = 42}, true, &s);
  TEST_ASSERT_EQUAL(VD_HOME, target.kind);
}

void test_resolve_actions_resume_deleted_timer_falls_back_to_home(void) {
  TimerStore s;
  store_init(&s);
  ViewDesc target = nav_on_resolve((ViewDesc){.kind = VD_ACTIONS, .cookie = 42}, true, &s);
  TEST_ASSERT_EQUAL(VD_HOME, target.kind);
}

// The dead-reference guard extends uniformly to LAPS: the interruption model
// applies the same way to actions and laps.

void test_resolve_laps_resume_alive_timer_is_preserved(void) {
  TimerStore s;
  store_init(&s);
  int32_t cookie;
  store_add(&s, KIND_STOPWATCH, 0, 1000, &cookie);
  ViewDesc target = nav_on_resolve((ViewDesc){.kind = VD_LAPS, .cookie = cookie}, true, &s);
  TEST_ASSERT_EQUAL(VD_LAPS, target.kind);
  TEST_ASSERT_EQUAL_INT32(cookie, target.cookie);
}

void test_resolve_laps_resume_deleted_timer_falls_back_to_home(void) {
  TimerStore s;
  store_init(&s);
  ViewDesc target = nav_on_resolve((ViewDesc){.kind = VD_LAPS, .cookie = 42}, true, &s);
  TEST_ASSERT_EQUAL(VD_HOME, target.kind);
}

void test_resolve_set_resume_preserved_regardless_of_store(void) {
  // Create mode: cookie 0, never store-linked -- always coherent.
  TimerStore s;
  store_init(&s);
  ViewDesc target = nav_on_resolve((ViewDesc){.kind = VD_SET}, true, &s);
  TEST_ASSERT_EQUAL(VD_SET, target.kind);
  TEST_ASSERT_EQUAL_INT32(0, target.cookie);
}

// nav_go_edit_interval gives VD_SET a real store-linked cookie (Edit
// interval); the dead-reference guard covers this case too: if the timer
// being edited fires mid-edit (take-over captures VD_SET(cookie) as the
// resume target) and is then removed via Stop before the alarm resolves,
// the guard falls back to HOME rather than returning the stale
// VD_SET(cookie) resume, which would otherwise reach screen_set.c's
// commit_edit_interval against a deleted timer.

void test_resolve_set_edit_mode_resume_alive_timer_is_preserved(void) {
  TimerStore s;
  store_init(&s);
  int32_t cookie;
  store_add(&s, KIND_REPEAT, 300, 1000, &cookie);
  ViewDesc target = nav_on_resolve((ViewDesc){.kind = VD_SET, .cookie = cookie}, true, &s);
  TEST_ASSERT_EQUAL(VD_SET, target.kind);
  TEST_ASSERT_EQUAL_INT32(cookie, target.cookie);
}

void test_resolve_set_edit_mode_resume_deleted_timer_falls_back_to_home(void) {
  TimerStore s;
  store_init(&s);
  ViewDesc target = nav_on_resolve((ViewDesc){.kind = VD_SET, .cookie = 42}, true, &s);
  TEST_ASSERT_EQUAL(VD_HOME, target.kind);
}

// VD_LIBRARY and VD_PRESET_ACTIONS are "always coherent" (they read the
// PresetStore live at draw time; a preset can't be removed while alarmed,
// unlike a live timer) -- nav_on_resolve has no special case for either, so
// it must fall through to the default "return resume unchanged" branch.

void test_resolve_library_resume_is_always_coherent(void) {
  TimerStore s;
  store_init(&s);
  ViewDesc target = nav_on_resolve((ViewDesc){.kind = VD_LIBRARY}, true, &s);
  TEST_ASSERT_EQUAL(VD_LIBRARY, target.kind);
}

void test_resolve_preset_actions_resume_is_always_coherent_regardless_of_store(void) {
  TimerStore s;
  store_init(&s);  // empty store -- irrelevant, PRESET_ACTIONS never store-linked
  ViewDesc target = nav_on_resolve((ViewDesc){.kind = VD_PRESET_ACTIONS, .cookie = 3}, true, &s);
  TEST_ASSERT_EQUAL(VD_PRESET_ACTIONS, target.kind);
  TEST_ASSERT_EQUAL_INT32(3, target.cookie);
}

// Unlike VD_LIBRARY/VD_PRESET_ACTIONS, VD_VOICE is excluded UNCONDITIONALLY
// from the "resume is always coherent" set (regardless of the store's
// contents, and regardless of whether this specific resume was ever
// mid-dictation -- nav_policy has no visibility into that adapter-tier
// state) -- always resolves to HOME instead. Reason: an alarm pre-empting a
// PENDING dictation attempt aborts it (render_stack()'s structural
// dictation_am_cancel(), nav.c), so resuming into VD_VOICE afterward would
// show a stale safe placeholder ("Voice needs your phone") with no relation
// to why the user was seeing it, forcing an extra, unexplained Back press.
// See nav_on_resolve's own doc comment
// (lib/nav_policy.h) for why "HOME" and "the pre-voice screen" are the same
// destination in this app by construction (VD_VOICE's breadcrumb is always
// exactly [HOME, VOICE]), so no information is being discarded by not
// distinguishing them.
void test_resolve_voice_resume_excluded_returns_home(void) {
  TimerStore s;
  store_init(&s);
  ViewDesc target = nav_on_resolve((ViewDesc){.kind = VD_VOICE, .cookie = 0}, true, &s);
  TEST_ASSERT_EQUAL(VD_HOME, target.kind);
  TEST_ASSERT_EQUAL_INT32(0, target.cookie);
}

// --- nav_pick_ringing_cookie -------------------------------------------------
// Decides which cookie the alarm view shows once the currently-displayed
// ringer resolves but ringing[] is still non-empty.

void test_pick_ringing_cookie_returns_zero_when_empty(void) {
  AppState a;
  app_state_init(&a);
  TEST_ASSERT_EQUAL_INT32(0, nav_pick_ringing_cookie(&a));
}

void test_pick_ringing_cookie_single_member(void) {
  AppState a;
  app_state_init(&a);
  app_state_ringing_add(&a, 7);
  TEST_ASSERT_EQUAL_INT32(7, nav_pick_ringing_cookie(&a));
}

void test_pick_ringing_cookie_after_resolving_first_of_two_ascending_order(void) {
  // Fired in order A(1) then B(2), A shown first (stacked, not re-captured)
  // -- resolving A leaves B as the sole remaining ringer.
  AppState a;
  app_state_init(&a);
  app_state_ringing_add(&a, 1);     // A fires first
  app_state_ringing_add(&a, 2);     // B fires second, stacks
  app_state_ringing_remove(&a, 1);  // A resolved (Done/keep-counting/snooze)
  TEST_ASSERT_EQUAL_INT32(2, nav_pick_ringing_cookie(&a));
}

void test_pick_ringing_cookie_after_resolving_first_of_two_descending_cookie_values(void) {
  // Same scenario, cookies assigned in the OPPOSITE numeric order (cookies
  // are monotonically increasing per timer creation, not per fire order) --
  // proves the pick is fire/insertion order, never cookie-value order.
  AppState a;
  app_state_init(&a);
  app_state_ringing_add(&a, 9);     // A fires first (higher cookie value)
  app_state_ringing_add(&a, 3);     // B fires second (lower cookie value)
  app_state_ringing_remove(&a, 9);  // A resolved
  TEST_ASSERT_EQUAL_INT32(3, nav_pick_ringing_cookie(&a));
}

void test_pick_ringing_cookie_resolve_middle_of_three(void) {
  AppState a;
  app_state_init(&a);
  app_state_ringing_add(&a, 1);  // A -- displayed (first-fired)
  app_state_ringing_add(&a, 2);  // B -- stacked
  app_state_ringing_add(&a, 3);  // C -- stacked
  // Resolving the MIDDLE-fired one (B) via its own alarm-options row, while A
  // (not B) is what's actually on-screen -- the displayed cookie must NOT
  // change just because some other ringing timer resolved.
  app_state_ringing_remove(&a, 2);
  TEST_ASSERT_EQUAL_INT32(1, nav_pick_ringing_cookie(&a));
  // Now resolve A (the one actually displayed) -- steps up to C, skipping
  // over the already-removed B.
  app_state_ringing_remove(&a, 1);
  TEST_ASSERT_EQUAL_INT32(3, nav_pick_ringing_cookie(&a));
}

// --- nav_keep_counting_target ------------------------------------------------
// keepCounting's post-resolution target is the converted timer's OWN hero,
// never the generic pre-emption `resume` -- EXCEPT when `resume` is an
// in-progress SET session, which would otherwise be silently, permanently
// destroyed the next time SET is opened fresh (unlike HOME/HERO/ACTIONS/LAPS
// resumes, always still reachable via HOME regardless of whether they're
// restored now).

void test_keep_counting_target_is_that_timers_own_hero_when_no_resume(void) {
  TimerStore s;
  store_init(&s);
  ViewDesc target = nav_keep_counting_target(5, (ViewDesc){0}, false, &s);
  TEST_ASSERT_EQUAL(VD_HERO, target.kind);
  TEST_ASSERT_EQUAL_INT32(5, target.cookie);
}

void test_keep_counting_target_is_independent_of_cookie_value(void) {
  TimerStore s;
  store_init(&s);
  ViewDesc target = nav_keep_counting_target(999, (ViewDesc){0}, false, &s);
  TEST_ASSERT_EQUAL(VD_HERO, target.kind);
  TEST_ASSERT_EQUAL_INT32(999, target.cookie);
}

void test_keep_counting_target_ignores_home_resume_shows_own_hero(void) {
  // HOME costs nothing to skip -- still reachable any time, behavior
  // unchanged.
  TimerStore s;
  store_init(&s);
  ViewDesc target =
      nav_keep_counting_target(5, (ViewDesc){.kind = VD_HOME}, true, &s);
  TEST_ASSERT_EQUAL(VD_HERO, target.kind);
  TEST_ASSERT_EQUAL_INT32(5, target.cookie);
}

void test_keep_counting_target_ignores_other_hero_resume_shows_own_hero(void) {
  // A different timer's HERO is likewise always reachable again via HOME --
  // behavior unchanged (this is the case this guard protects).
  TimerStore s;
  store_init(&s);
  int32_t other_cookie;
  store_add(&s, KIND_COUNTDOWN, 60, 1000, &other_cookie);
  ViewDesc target = nav_keep_counting_target(
      5, (ViewDesc){.kind = VD_HERO, .cookie = other_cookie}, true, &s);
  TEST_ASSERT_EQUAL(VD_HERO, target.kind);
  TEST_ASSERT_EQUAL_INT32(5, target.cookie);
}

// An interrupted mid-dial CREATE-mode SET (cookie == 0) is protected --
// keepCounting restores SET instead of jumping to the converted stopwatch.
void test_keep_counting_target_protects_create_mode_set_resume(void) {
  TimerStore s;
  store_init(&s);
  ViewDesc target =
      nav_keep_counting_target(5, (ViewDesc){.kind = VD_SET, .cookie = 0}, true, &s);
  TEST_ASSERT_EQUAL(VD_SET, target.kind);
  TEST_ASSERT_EQUAL_INT32(0, target.cookie);
}

// An interrupted mid-dial EDIT-mode SET (cookie != 0, via
// nav_go_edit_interval) for a STILL-LIVE timer is likewise protected.
void test_keep_counting_target_protects_edit_mode_set_resume_when_alive(void) {
  TimerStore s;
  store_init(&s);
  int32_t edit_cookie;
  store_add(&s, KIND_REPEAT, 300, 1000, &edit_cookie);
  ViewDesc target = nav_keep_counting_target(
      5, (ViewDesc){.kind = VD_SET, .cookie = edit_cookie}, true, &s);
  TEST_ASSERT_EQUAL(VD_SET, target.kind);
  TEST_ASSERT_EQUAL_INT32(edit_cookie, target.cookie);
}

// The dead-reference guard still applies to a protected SET resume -- if
// the timer being edited was ITSELF removed while the (different, keep-
// counting) alarm was open, fall back to HOME rather than SET(dead cookie).
void test_keep_counting_target_set_resume_dead_reference_falls_back_home(void) {
  TimerStore s;
  store_init(&s);
  ViewDesc target = nav_keep_counting_target(
      5, (ViewDesc){.kind = VD_SET, .cookie = 42}, true, &s);
  TEST_ASSERT_EQUAL(VD_HOME, target.kind);
}

// --- nav_alarm_target --------------------------------------------------------

void test_alarm_target_zero_ringing_shows_solo_hero(void) {
  // Not really reachable in practice (nav_alarm_target is only ever called
  // once a fire/resolve has already established at least one ringer), but
  // the decision itself should degrade to the single-hero shape, not crash
  // or pick an arbitrary screen.
  ViewDesc target = nav_alarm_target(0, 5);
  TEST_ASSERT_EQUAL(VD_ALARM, target.kind);
  TEST_ASSERT_EQUAL_INT32(5, target.cookie);
}

void test_alarm_target_single_ringing_shows_solo_hero(void) {
  ViewDesc target = nav_alarm_target(1, 7);
  TEST_ASSERT_EQUAL(VD_ALARM, target.kind);
  TEST_ASSERT_EQUAL_INT32(7, target.cookie);
}

void test_alarm_target_two_ringing_shows_the_list(void) {
  ViewDesc target = nav_alarm_target(2, 7);
  TEST_ASSERT_EQUAL(VD_RINGING_LIST, target.kind);
}

void test_alarm_target_many_ringing_still_shows_the_list(void) {
  ViewDesc target = nav_alarm_target(5, 7);
  TEST_ASSERT_EQUAL(VD_RINGING_LIST, target.kind);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_on_fire_from_home_takes_over);
  RUN_TEST(test_on_fire_from_hero_takes_over);
  RUN_TEST(test_on_fire_from_set_takes_over);
  RUN_TEST(test_on_fire_while_already_in_alarm_does_not_take_over_or_recapture);
  RUN_TEST(test_on_fire_while_in_alarm_options_also_does_not_take_over);
  RUN_TEST(test_on_fire_while_in_ringing_list_also_does_not_take_over);
  RUN_TEST(test_on_fire_from_voice_takes_over);
  RUN_TEST(test_resolve_no_resume_goes_home);
  RUN_TEST(test_resolve_home_resume_is_always_coherent);
  RUN_TEST(test_resolve_hero_resume_alive_timer_is_preserved);
  RUN_TEST(test_resolve_hero_resume_deleted_timer_falls_back_to_home);
  RUN_TEST(test_resolve_actions_resume_deleted_timer_falls_back_to_home);
  RUN_TEST(test_resolve_laps_resume_alive_timer_is_preserved);
  RUN_TEST(test_resolve_laps_resume_deleted_timer_falls_back_to_home);
  RUN_TEST(test_resolve_set_resume_preserved_regardless_of_store);
  RUN_TEST(test_resolve_set_edit_mode_resume_alive_timer_is_preserved);
  RUN_TEST(test_resolve_set_edit_mode_resume_deleted_timer_falls_back_to_home);
  RUN_TEST(test_resolve_library_resume_is_always_coherent);
  RUN_TEST(test_resolve_preset_actions_resume_is_always_coherent_regardless_of_store);
  RUN_TEST(test_resolve_voice_resume_excluded_returns_home);
  RUN_TEST(test_pick_ringing_cookie_returns_zero_when_empty);
  RUN_TEST(test_pick_ringing_cookie_single_member);
  RUN_TEST(test_pick_ringing_cookie_after_resolving_first_of_two_ascending_order);
  RUN_TEST(test_pick_ringing_cookie_after_resolving_first_of_two_descending_cookie_values);
  RUN_TEST(test_pick_ringing_cookie_resolve_middle_of_three);
  RUN_TEST(test_keep_counting_target_is_that_timers_own_hero_when_no_resume);
  RUN_TEST(test_keep_counting_target_is_independent_of_cookie_value);
  RUN_TEST(test_keep_counting_target_ignores_home_resume_shows_own_hero);
  RUN_TEST(test_keep_counting_target_ignores_other_hero_resume_shows_own_hero);
  RUN_TEST(test_keep_counting_target_protects_create_mode_set_resume);
  RUN_TEST(test_keep_counting_target_protects_edit_mode_set_resume_when_alive);
  RUN_TEST(test_keep_counting_target_set_resume_dead_reference_falls_back_home);
  RUN_TEST(test_alarm_target_zero_ringing_shows_solo_hero);
  RUN_TEST(test_alarm_target_single_ringing_shows_solo_hero);
  RUN_TEST(test_alarm_target_two_ringing_shows_the_list);
  RUN_TEST(test_alarm_target_many_ringing_still_shows_the_list);
  return UNITY_END();
}
