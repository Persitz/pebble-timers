// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// Nav crash fix: pure tests for the breadcrumb/diff logic that replaces
// nav.c's render_stack() pop-all+rebuild-from-HOME pattern. See
// lib/nav_stack.h's header comment for the full rationale.
#include "nav_stack.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static ViewDesc vd(ViewDescKind kind, int32_t cookie) {
  return (ViewDesc){.kind = kind, .cookie = cookie};
}

static void assert_breadcrumb(NavBreadcrumb bc, int expected_len, const ViewDesc *expected) {
  TEST_ASSERT_EQUAL_INT(expected_len, bc.len);
  for (int i = 0; i < expected_len; i++) {
    TEST_ASSERT_EQUAL_INT(expected[i].kind, bc.items[i].kind);
    TEST_ASSERT_EQUAL_INT32(expected[i].cookie, bc.items[i].cookie);
  }
}

// --- nav_breadcrumb_for: mirrors the old render_stack() switch exactly ----

void test_breadcrumb_home(void) {
  ViewDesc expect[] = {vd(VD_HOME, 0)};
  assert_breadcrumb(nav_breadcrumb_for(vd(VD_HOME, 0)), 1, expect);
}

void test_breadcrumb_set(void) {
  ViewDesc expect[] = {vd(VD_HOME, 0), vd(VD_SET, 0)};
  assert_breadcrumb(nav_breadcrumb_for(vd(VD_SET, 0)), 2, expect);
}

void test_breadcrumb_set_with_cookie_for_edit_interval(void) {
  // nav_go_edit_interval(cookie) sets s_current = {VD_SET, cookie} --
  // the breadcrumb must carry that cookie through (not hardcode 0 the way it
  // used to, back when VD_SET only ever meant "create mode") or a
  // create-mode<->edit-mode transition that happens to share SET's
  // ViewDescKind would diff as a no-op and apply_cookie would never re-run
  // for the second entry. Cookie is otherwise inert to the PHYSICAL stack
  // (still just one SET window) -- this only matters for equality-diffing.
  ViewDesc expect[] = {vd(VD_HOME, 0), vd(VD_SET, 7)};
  assert_breadcrumb(nav_breadcrumb_for(vd(VD_SET, 7)), 2, expect);
}

void test_breadcrumb_hero(void) {
  ViewDesc expect[] = {vd(VD_HOME, 0), vd(VD_HERO, 7)};
  assert_breadcrumb(nav_breadcrumb_for(vd(VD_HERO, 7)), 2, expect);
}

void test_breadcrumb_actions(void) {
  ViewDesc expect[] = {vd(VD_HOME, 0), vd(VD_HERO, 7), vd(VD_ACTIONS, 7)};
  assert_breadcrumb(nav_breadcrumb_for(vd(VD_ACTIONS, 7)), 3, expect);
}

void test_breadcrumb_alarm(void) {
  ViewDesc expect[] = {vd(VD_HOME, 0), vd(VD_ALARM, 9)};
  assert_breadcrumb(nav_breadcrumb_for(vd(VD_ALARM, 9)), 2, expect);
}

void test_breadcrumb_alarm_options(void) {
  ViewDesc expect[] = {vd(VD_HOME, 0), vd(VD_ALARM, 9), vd(VD_ALARM_OPTIONS, 9)};
  assert_breadcrumb(nav_breadcrumb_for(vd(VD_ALARM_OPTIONS, 9)), 3, expect);
}

void test_breadcrumb_laps(void) {
  // LAPS is reachable only through ACTIONS -- HOME, HERO, ACTIONS,
  // LAPS (the deepest breadcrumb the app has, NAV_BREADCRUMB_MAX=4).
  ViewDesc expect[] = {vd(VD_HOME, 0), vd(VD_HERO, 7), vd(VD_ACTIONS, 7), vd(VD_LAPS, 7)};
  assert_breadcrumb(nav_breadcrumb_for(vd(VD_LAPS, 7)), 4, expect);
}

void test_breadcrumb_ringing_list(void) {
  // HOME, RINGING_LIST -- a top-level alarm-family screen, never nested
  // (cookie always 0/ignored -- the list reads AppState directly).
  ViewDesc expect[] = {vd(VD_HOME, 0), vd(VD_RINGING_LIST, 0)};
  assert_breadcrumb(nav_breadcrumb_for(vd(VD_RINGING_LIST, 0)), 2, expect);
}

void test_breadcrumb_clear_confirm(void) {
  // HOME, CLEAR_CONFIRM -- a top-level screen, never nested (cookie
  // ignored), same shape as RINGING_LIST above.
  ViewDesc expect[] = {vd(VD_HOME, 0), vd(VD_CLEAR_CONFIRM, 0)};
  assert_breadcrumb(nav_breadcrumb_for(vd(VD_CLEAR_CONFIRM, 0)), 2, expect);
}

// --- LIBRARY/PRESET_ACTIONS breadcrumbs (Presets) ---------------------------

void test_breadcrumb_library(void) {
  ViewDesc expect[] = {vd(VD_HOME, 0), vd(VD_LIBRARY, 0)};
  assert_breadcrumb(nav_breadcrumb_for(vd(VD_LIBRARY, 0)), 2, expect);
}

void test_breadcrumb_preset_actions(void) {
  // Reachable only through LIBRARY -- HOME, LIBRARY, PRESET_ACTIONS(index).
  // Cookie carries the preset INDEX (ViewDesc's documented reuse).
  ViewDesc expect[] = {vd(VD_HOME, 0), vd(VD_LIBRARY, 0), vd(VD_PRESET_ACTIONS, 2)};
  assert_breadcrumb(nav_breadcrumb_for(vd(VD_PRESET_ACTIONS, 2)), 3, expect);
}

// --- VOICE breadcrumb -------------------------------------------------------

void test_breadcrumb_voice(void) {
  ViewDesc expect[] = {vd(VD_HOME, 0), vd(VD_VOICE, 0)};
  assert_breadcrumb(nav_breadcrumb_for(vd(VD_VOICE, 0)), 2, expect);
}

// --- nav_diff_stack ---------------------------------------------------------

void test_diff_init_from_empty_pushes_whole_breadcrumb(void) {
  NavBreadcrumb current = {.len = 0};
  NavBreadcrumb target = nav_breadcrumb_for(vd(VD_HOME, 0));
  NavStackDiff diff = nav_diff_stack(current, target);
  TEST_ASSERT_EQUAL_INT(0, diff.pop_count);
  TEST_ASSERT_EQUAL_INT(1, diff.push_count);
  TEST_ASSERT_EQUAL_INT(VD_HOME, diff.push[0].kind);
}

void test_diff_identical_breadcrumbs_is_a_no_op(void) {
  // The whole point of the fix: an alarm re-fire while already showing the
  // alarm (s_current unchanged) must issue ZERO window_stack calls, not a
  // full pop-all+rebuild.
  NavBreadcrumb bc = nav_breadcrumb_for(vd(VD_ALARM, 4));
  NavStackDiff diff = nav_diff_stack(bc, bc);
  TEST_ASSERT_EQUAL_INT(0, diff.pop_count);
  TEST_ASSERT_EQUAL_INT(0, diff.push_count);
}

void test_diff_home_to_actions_pushes_hero_then_actions_only(void) {
  // The exact repro from the nav crash fix: HOME -> HERO ->
  // (Down) -> ACTIONS. HOME is already on the stack -- must not be touched.
  NavBreadcrumb current = nav_breadcrumb_for(vd(VD_HERO, 3));
  NavBreadcrumb target = nav_breadcrumb_for(vd(VD_ACTIONS, 3));
  NavStackDiff diff = nav_diff_stack(current, target);
  TEST_ASSERT_EQUAL_INT(0, diff.pop_count);
  TEST_ASSERT_EQUAL_INT(1, diff.push_count);
  TEST_ASSERT_EQUAL_INT(VD_ACTIONS, diff.push[0].kind);
  TEST_ASSERT_EQUAL_INT32(3, diff.push[0].cookie);
}

void test_diff_actions_back_to_hero_pops_actions_only(void) {
  NavBreadcrumb current = nav_breadcrumb_for(vd(VD_ACTIONS, 3));
  NavBreadcrumb target = nav_breadcrumb_for(vd(VD_HERO, 3));
  NavStackDiff diff = nav_diff_stack(current, target);
  TEST_ASSERT_EQUAL_INT(1, diff.pop_count);
  TEST_ASSERT_EQUAL_INT(0, diff.push_count);
}

void test_diff_actions_to_home_pops_actions_and_hero(void) {
  NavBreadcrumb current = nav_breadcrumb_for(vd(VD_ACTIONS, 3));
  NavBreadcrumb target = nav_breadcrumb_for(vd(VD_HOME, 0));
  NavStackDiff diff = nav_diff_stack(current, target);
  TEST_ASSERT_EQUAL_INT(2, diff.pop_count);
  TEST_ASSERT_EQUAL_INT(0, diff.push_count);
}

void test_diff_set_to_hero_pops_set_pushes_hero_home_untouched(void) {
  NavBreadcrumb current = nav_breadcrumb_for(vd(VD_SET, 0));
  NavBreadcrumb target = nav_breadcrumb_for(vd(VD_HERO, 5));
  NavStackDiff diff = nav_diff_stack(current, target);
  TEST_ASSERT_EQUAL_INT(1, diff.pop_count);
  TEST_ASSERT_EQUAL_INT(1, diff.push_count);
  TEST_ASSERT_EQUAL_INT(VD_HERO, diff.push[0].kind);
  TEST_ASSERT_EQUAL_INT32(5, diff.push[0].cookie);
}

void test_diff_actions_to_edit_interval_pushes_set_pops_actions(void) {
  // ACTIONS' "Every N" row -> nav_go_edit_interval -- pops ACTIONS+HERO,
  // pushes SET (with the edit cookie carried, per the breadcrumb fix above).
  NavBreadcrumb current = nav_breadcrumb_for(vd(VD_ACTIONS, 7));
  NavBreadcrumb target = nav_breadcrumb_for(vd(VD_SET, 7));
  NavStackDiff diff = nav_diff_stack(current, target);
  TEST_ASSERT_EQUAL_INT(2, diff.pop_count);
  TEST_ASSERT_EQUAL_INT(1, diff.push_count);
  TEST_ASSERT_EQUAL_INT(VD_SET, diff.push[0].kind);
  TEST_ASSERT_EQUAL_INT32(7, diff.push[0].cookie);
}

void test_diff_edit_interval_cookie_change_is_a_real_transition(void) {
  // Proves the breadcrumb fix actually matters: two DIFFERENT edit-interval
  // sessions must NOT diff as a no-op just because both are VD_SET.
  NavBreadcrumb current = nav_breadcrumb_for(vd(VD_SET, 7));
  NavBreadcrumb target = nav_breadcrumb_for(vd(VD_SET, 9));
  NavStackDiff diff = nav_diff_stack(current, target);
  TEST_ASSERT_EQUAL_INT(1, diff.pop_count);
  TEST_ASSERT_EQUAL_INT(1, diff.push_count);
  TEST_ASSERT_EQUAL_INT32(9, diff.push[0].cookie);
}

void test_diff_home_to_hero_pushes_hero_only(void) {
  NavBreadcrumb current = nav_breadcrumb_for(vd(VD_HOME, 0));
  NavBreadcrumb target = nav_breadcrumb_for(vd(VD_HERO, 8));
  NavStackDiff diff = nav_diff_stack(current, target);
  TEST_ASSERT_EQUAL_INT(0, diff.pop_count);
  TEST_ASSERT_EQUAL_INT(1, diff.push_count);
}

void test_diff_alarm_to_next_ringing_cookie_pops_and_pushes_alarm_only(void) {
  // nav_alarm_resolved's restack-to-next-ringing-cookie branch: same
  // ViewDescKind (ALARM), different cookie, at the top of the stack. Must be
  // treated as a divergence (HOME stays untouched, ALARM is popped+re-pushed
  // with the new cookie) -- not silently left stale.
  NavBreadcrumb current = nav_breadcrumb_for(vd(VD_ALARM, 1));
  NavBreadcrumb target = nav_breadcrumb_for(vd(VD_ALARM, 2));
  NavStackDiff diff = nav_diff_stack(current, target);
  TEST_ASSERT_EQUAL_INT(1, diff.pop_count);
  TEST_ASSERT_EQUAL_INT(1, diff.push_count);
  TEST_ASSERT_EQUAL_INT(VD_ALARM, diff.push[0].kind);
  TEST_ASSERT_EQUAL_INT32(2, diff.push[0].cookie);
}

void test_diff_alarm_to_ringing_list_pops_alarm_pushes_list(void) {
  // A 2nd fire re-routes a showing single hero to the ringing list.
  NavBreadcrumb current = nav_breadcrumb_for(vd(VD_ALARM, 1));
  NavBreadcrumb target = nav_breadcrumb_for(vd(VD_RINGING_LIST, 0));
  NavStackDiff diff = nav_diff_stack(current, target);
  TEST_ASSERT_EQUAL_INT(1, diff.pop_count);
  TEST_ASSERT_EQUAL_INT(1, diff.push_count);
  TEST_ASSERT_EQUAL_INT(VD_RINGING_LIST, diff.push[0].kind);
}

void test_diff_ringing_list_to_alarm_pops_list_pushes_alarm(void) {
  // Select on a ringing-list row opens that alarm's own hero.
  NavBreadcrumb current = nav_breadcrumb_for(vd(VD_RINGING_LIST, 0));
  NavBreadcrumb target = nav_breadcrumb_for(vd(VD_ALARM, 3));
  NavStackDiff diff = nav_diff_stack(current, target);
  TEST_ASSERT_EQUAL_INT(1, diff.pop_count);
  TEST_ASSERT_EQUAL_INT(1, diff.push_count);
  TEST_ASSERT_EQUAL_INT(VD_ALARM, diff.push[0].kind);
  TEST_ASSERT_EQUAL_INT32(3, diff.push[0].cookie);
}

void test_diff_ringing_list_to_ringing_list_is_a_no_op(void) {
  // A 3rd/4th simultaneous fire while the list is already up must issue ZERO
  // window_stack calls (the list re-syncs its row count from AppState
  // directly, on every tick -- see screen_ringing.c).
  NavBreadcrumb bc = nav_breadcrumb_for(vd(VD_RINGING_LIST, 0));
  NavStackDiff diff = nav_diff_stack(bc, bc);
  TEST_ASSERT_EQUAL_INT(0, diff.pop_count);
  TEST_ASSERT_EQUAL_INT(0, diff.push_count);
}

void test_diff_home_to_clear_confirm_pushes_confirm_only(void) {
  NavBreadcrumb current = nav_breadcrumb_for(vd(VD_HOME, 0));
  NavBreadcrumb target = nav_breadcrumb_for(vd(VD_CLEAR_CONFIRM, 0));
  NavStackDiff diff = nav_diff_stack(current, target);
  TEST_ASSERT_EQUAL_INT(0, diff.pop_count);
  TEST_ASSERT_EQUAL_INT(1, diff.push_count);
  TEST_ASSERT_EQUAL_INT(VD_CLEAR_CONFIRM, diff.push[0].kind);
}

void test_diff_clear_confirm_to_home_pops_confirm_only(void) {
  NavBreadcrumb current = nav_breadcrumb_for(vd(VD_CLEAR_CONFIRM, 0));
  NavBreadcrumb target = nav_breadcrumb_for(vd(VD_HOME, 0));
  NavStackDiff diff = nav_diff_stack(current, target);
  TEST_ASSERT_EQUAL_INT(1, diff.pop_count);
  TEST_ASSERT_EQUAL_INT(0, diff.push_count);
}

void test_diff_alarm_options_back_to_alarm_pops_options_only(void) {
  NavBreadcrumb current = nav_breadcrumb_for(vd(VD_ALARM_OPTIONS, 6));
  NavBreadcrumb target = nav_breadcrumb_for(vd(VD_ALARM, 6));
  NavStackDiff diff = nav_diff_stack(current, target);
  TEST_ASSERT_EQUAL_INT(1, diff.pop_count);
  TEST_ASSERT_EQUAL_INT(0, diff.push_count);
}

void test_diff_alarm_to_alarm_options_pushes_options_only(void) {
  NavBreadcrumb current = nav_breadcrumb_for(vd(VD_ALARM, 6));
  NavBreadcrumb target = nav_breadcrumb_for(vd(VD_ALARM_OPTIONS, 6));
  NavStackDiff diff = nav_diff_stack(current, target);
  TEST_ASSERT_EQUAL_INT(0, diff.pop_count);
  TEST_ASSERT_EQUAL_INT(1, diff.push_count);
  TEST_ASSERT_EQUAL_INT(VD_ALARM_OPTIONS, diff.push[0].kind);
}

void test_diff_hero_to_hero_different_cookie_pops_and_pushes(void) {
  // Not reachable via the current call graph (nav_go_hero is only ever
  // called while sitting on HOME/SET/ACTIONS with the SAME cookie already),
  // but the diff must handle it correctly regardless -- robustness, not a
  // hypothetical the algorithm gets to assume away.
  NavBreadcrumb current = nav_breadcrumb_for(vd(VD_HERO, 1));
  NavBreadcrumb target = nav_breadcrumb_for(vd(VD_HERO, 2));
  NavStackDiff diff = nav_diff_stack(current, target);
  TEST_ASSERT_EQUAL_INT(1, diff.pop_count);
  TEST_ASSERT_EQUAL_INT(1, diff.push_count);
  TEST_ASSERT_EQUAL_INT32(2, diff.push[0].cookie);
}

void test_diff_actions_to_actions_same_cookie_is_a_no_op(void) {
  NavBreadcrumb bc = nav_breadcrumb_for(vd(VD_ACTIONS, 3));
  NavStackDiff diff = nav_diff_stack(bc, bc);
  TEST_ASSERT_EQUAL_INT(0, diff.pop_count);
  TEST_ASSERT_EQUAL_INT(0, diff.push_count);
}

// --- LAPS diffs --------------------------------------------------------------

void test_diff_actions_to_laps_pushes_laps_only(void) {
  NavBreadcrumb current = nav_breadcrumb_for(vd(VD_ACTIONS, 3));
  NavBreadcrumb target = nav_breadcrumb_for(vd(VD_LAPS, 3));
  NavStackDiff diff = nav_diff_stack(current, target);
  TEST_ASSERT_EQUAL_INT(0, diff.pop_count);
  TEST_ASSERT_EQUAL_INT(1, diff.push_count);
  TEST_ASSERT_EQUAL_INT(VD_LAPS, diff.push[0].kind);
  TEST_ASSERT_EQUAL_INT32(3, diff.push[0].cookie);
}

void test_diff_laps_back_to_actions_pops_laps_only(void) {
  NavBreadcrumb current = nav_breadcrumb_for(vd(VD_LAPS, 3));
  NavBreadcrumb target = nav_breadcrumb_for(vd(VD_ACTIONS, 3));
  NavStackDiff diff = nav_diff_stack(current, target);
  TEST_ASSERT_EQUAL_INT(1, diff.pop_count);
  TEST_ASSERT_EQUAL_INT(0, diff.push_count);
}

void test_diff_laps_to_home_pops_all_three(void) {
  // Interruption-model take-over: a fire while on LAPS must be able to
  // collapse the whole HERO/ACTIONS/LAPS breadcrumb down to HOME in one diff
  // (the dead-reference-guard path, via nav_on_resolve).
  NavBreadcrumb current = nav_breadcrumb_for(vd(VD_LAPS, 3));
  NavBreadcrumb target = nav_breadcrumb_for(vd(VD_HOME, 0));
  NavStackDiff diff = nav_diff_stack(current, target);
  TEST_ASSERT_EQUAL_INT(3, diff.pop_count);
  TEST_ASSERT_EQUAL_INT(0, diff.push_count);
}

void test_diff_laps_to_laps_same_cookie_is_a_no_op(void) {
  NavBreadcrumb bc = nav_breadcrumb_for(vd(VD_LAPS, 3));
  NavStackDiff diff = nav_diff_stack(bc, bc);
  TEST_ASSERT_EQUAL_INT(0, diff.pop_count);
  TEST_ASSERT_EQUAL_INT(0, diff.push_count);
}

// --- LIBRARY/PRESET_ACTIONS diffs --------------------------------------------

void test_diff_home_to_library_pushes_library_only(void) {
  NavBreadcrumb current = nav_breadcrumb_for(vd(VD_HOME, 0));
  NavBreadcrumb target = nav_breadcrumb_for(vd(VD_LIBRARY, 0));
  NavStackDiff diff = nav_diff_stack(current, target);
  TEST_ASSERT_EQUAL_INT(0, diff.pop_count);
  TEST_ASSERT_EQUAL_INT(1, diff.push_count);
  TEST_ASSERT_EQUAL_INT(VD_LIBRARY, diff.push[0].kind);
}

void test_diff_library_to_home_pops_library_only(void) {
  NavBreadcrumb current = nav_breadcrumb_for(vd(VD_LIBRARY, 0));
  NavBreadcrumb target = nav_breadcrumb_for(vd(VD_HOME, 0));
  NavStackDiff diff = nav_diff_stack(current, target);
  TEST_ASSERT_EQUAL_INT(1, diff.pop_count);
  TEST_ASSERT_EQUAL_INT(0, diff.push_count);
}

void test_diff_library_to_preset_actions_pushes_actions_only(void) {
  NavBreadcrumb current = nav_breadcrumb_for(vd(VD_LIBRARY, 0));
  NavBreadcrumb target = nav_breadcrumb_for(vd(VD_PRESET_ACTIONS, 1));
  NavStackDiff diff = nav_diff_stack(current, target);
  TEST_ASSERT_EQUAL_INT(0, diff.pop_count);
  TEST_ASSERT_EQUAL_INT(1, diff.push_count);
  TEST_ASSERT_EQUAL_INT(VD_PRESET_ACTIONS, diff.push[0].kind);
  TEST_ASSERT_EQUAL_INT32(1, diff.push[0].cookie);
}

void test_diff_preset_actions_back_to_library_pops_actions_only(void) {
  NavBreadcrumb current = nav_breadcrumb_for(vd(VD_PRESET_ACTIONS, 1));
  NavBreadcrumb target = nav_breadcrumb_for(vd(VD_LIBRARY, 0));
  NavStackDiff diff = nav_diff_stack(current, target);
  TEST_ASSERT_EQUAL_INT(1, diff.pop_count);
  TEST_ASSERT_EQUAL_INT(0, diff.push_count);
}

void test_diff_preset_actions_different_index_is_a_real_transition(void) {
  // Proves the cookie(=index) preservation actually matters -- two DIFFERENT
  // preset-actions sheets must not diff as a no-op just because both are
  // VD_PRESET_ACTIONS (same reasoning as edit-interval's cookie-change test).
  NavBreadcrumb current = nav_breadcrumb_for(vd(VD_PRESET_ACTIONS, 1));
  NavBreadcrumb target = nav_breadcrumb_for(vd(VD_PRESET_ACTIONS, 4));
  NavStackDiff diff = nav_diff_stack(current, target);
  TEST_ASSERT_EQUAL_INT(1, diff.pop_count);
  TEST_ASSERT_EQUAL_INT(1, diff.push_count);
  TEST_ASSERT_EQUAL_INT32(4, diff.push[0].cookie);
}

void test_diff_preset_actions_to_home_pops_both(void) {
  // Interruption-model take-over shape: a fire while on the preset-actions
  // sheet must collapse LIBRARY+PRESET_ACTIONS down to HOME in one diff (same
  // shape as test_diff_laps_to_home_pops_all_three).
  NavBreadcrumb current = nav_breadcrumb_for(vd(VD_PRESET_ACTIONS, 1));
  NavBreadcrumb target = nav_breadcrumb_for(vd(VD_HOME, 0));
  NavStackDiff diff = nav_diff_stack(current, target);
  TEST_ASSERT_EQUAL_INT(2, diff.pop_count);
  TEST_ASSERT_EQUAL_INT(0, diff.push_count);
}

void test_diff_set_preset_new_to_library_pops_set_pushes_nothing_new(void) {
  // Preset-dial cancel/save shape: SET(cookie=0, preset mode) -> LIBRARY.
  // VD_SET always breadcrumbs as [HOME, SET] regardless of SetMode (the
  // breadcrumb layer doesn't know about SetMode, only nav_policy's ViewDesc
  // -- preset dials ride the same VD_SET(0) breadcrumb create-mode uses).
  NavBreadcrumb current = nav_breadcrumb_for(vd(VD_SET, 0));
  NavBreadcrumb target = nav_breadcrumb_for(vd(VD_LIBRARY, 0));
  NavStackDiff diff = nav_diff_stack(current, target);
  TEST_ASSERT_EQUAL_INT(1, diff.pop_count);
  TEST_ASSERT_EQUAL_INT(1, diff.push_count);
  TEST_ASSERT_EQUAL_INT(VD_LIBRARY, diff.push[0].kind);
}

// --- VOICE diffs --------------------------------------------------------------

void test_diff_home_to_voice_pushes_voice_only(void) {
  NavBreadcrumb current = nav_breadcrumb_for(vd(VD_HOME, 0));
  NavBreadcrumb target = nav_breadcrumb_for(vd(VD_VOICE, 0));
  NavStackDiff diff = nav_diff_stack(current, target);
  TEST_ASSERT_EQUAL_INT(0, diff.pop_count);
  TEST_ASSERT_EQUAL_INT(1, diff.push_count);
  TEST_ASSERT_EQUAL_INT(VD_VOICE, diff.push[0].kind);
}

void test_diff_voice_to_home_pops_voice_only(void) {
  NavBreadcrumb current = nav_breadcrumb_for(vd(VD_VOICE, 0));
  NavBreadcrumb target = nav_breadcrumb_for(vd(VD_HOME, 0));
  NavStackDiff diff = nav_diff_stack(current, target);
  TEST_ASSERT_EQUAL_INT(1, diff.pop_count);
  TEST_ASSERT_EQUAL_INT(0, diff.push_count);
}

void test_diff_actions_to_different_actions_diverges_at_hero(void) {
  // Same shape as the HERO case above, one level deeper: kind matches at
  // both HERO and ACTIONS positions but the cookie differs -- must diverge
  // at the FIRST mismatching position (HERO), not just at ACTIONS.
  NavBreadcrumb current = nav_breadcrumb_for(vd(VD_ACTIONS, 1));
  NavBreadcrumb target = nav_breadcrumb_for(vd(VD_ACTIONS, 2));
  NavStackDiff diff = nav_diff_stack(current, target);
  TEST_ASSERT_EQUAL_INT(2, diff.pop_count);
  TEST_ASSERT_EQUAL_INT(2, diff.push_count);
  TEST_ASSERT_EQUAL_INT(VD_HERO, diff.push[0].kind);
  TEST_ASSERT_EQUAL_INT32(2, diff.push[0].cookie);
  TEST_ASSERT_EQUAL_INT(VD_ACTIONS, diff.push[1].kind);
  TEST_ASSERT_EQUAL_INT32(2, diff.push[1].cookie);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_breadcrumb_home);
  RUN_TEST(test_breadcrumb_set);
  RUN_TEST(test_breadcrumb_set_with_cookie_for_edit_interval);
  RUN_TEST(test_breadcrumb_hero);
  RUN_TEST(test_breadcrumb_actions);
  RUN_TEST(test_breadcrumb_alarm);
  RUN_TEST(test_breadcrumb_alarm_options);
  RUN_TEST(test_breadcrumb_laps);
  RUN_TEST(test_breadcrumb_ringing_list);
  RUN_TEST(test_breadcrumb_clear_confirm);
  RUN_TEST(test_breadcrumb_library);
  RUN_TEST(test_breadcrumb_preset_actions);
  RUN_TEST(test_breadcrumb_voice);
  RUN_TEST(test_diff_init_from_empty_pushes_whole_breadcrumb);
  RUN_TEST(test_diff_identical_breadcrumbs_is_a_no_op);
  RUN_TEST(test_diff_home_to_actions_pushes_hero_then_actions_only);
  RUN_TEST(test_diff_actions_back_to_hero_pops_actions_only);
  RUN_TEST(test_diff_actions_to_home_pops_actions_and_hero);
  RUN_TEST(test_diff_set_to_hero_pops_set_pushes_hero_home_untouched);
  RUN_TEST(test_diff_actions_to_edit_interval_pushes_set_pops_actions);
  RUN_TEST(test_diff_edit_interval_cookie_change_is_a_real_transition);
  RUN_TEST(test_diff_home_to_hero_pushes_hero_only);
  RUN_TEST(test_diff_alarm_to_next_ringing_cookie_pops_and_pushes_alarm_only);
  RUN_TEST(test_diff_alarm_to_ringing_list_pops_alarm_pushes_list);
  RUN_TEST(test_diff_ringing_list_to_alarm_pops_list_pushes_alarm);
  RUN_TEST(test_diff_ringing_list_to_ringing_list_is_a_no_op);
  RUN_TEST(test_diff_home_to_clear_confirm_pushes_confirm_only);
  RUN_TEST(test_diff_clear_confirm_to_home_pops_confirm_only);
  RUN_TEST(test_diff_alarm_options_back_to_alarm_pops_options_only);
  RUN_TEST(test_diff_alarm_to_alarm_options_pushes_options_only);
  RUN_TEST(test_diff_hero_to_hero_different_cookie_pops_and_pushes);
  RUN_TEST(test_diff_actions_to_actions_same_cookie_is_a_no_op);
  RUN_TEST(test_diff_actions_to_different_actions_diverges_at_hero);
  RUN_TEST(test_diff_actions_to_laps_pushes_laps_only);
  RUN_TEST(test_diff_laps_back_to_actions_pops_laps_only);
  RUN_TEST(test_diff_laps_to_home_pops_all_three);
  RUN_TEST(test_diff_laps_to_laps_same_cookie_is_a_no_op);
  RUN_TEST(test_diff_home_to_library_pushes_library_only);
  RUN_TEST(test_diff_library_to_home_pops_library_only);
  RUN_TEST(test_diff_library_to_preset_actions_pushes_actions_only);
  RUN_TEST(test_diff_preset_actions_back_to_library_pops_actions_only);
  RUN_TEST(test_diff_preset_actions_different_index_is_a_real_transition);
  RUN_TEST(test_diff_preset_actions_to_home_pops_both);
  RUN_TEST(test_diff_set_preset_new_to_library_pops_set_pushes_nothing_new);
  RUN_TEST(test_diff_home_to_voice_pushes_voice_only);
  RUN_TEST(test_diff_voice_to_home_pops_voice_only);
  return UNITY_END();
}
