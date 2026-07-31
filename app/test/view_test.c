// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

#include "view.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static Timer mk(TimerKind kind, bool fired) {
  Timer t = {0};
  t.kind = kind;
  t.fired = fired;
  return t;
}

void test_running_countdown_branch(void) {
  Timer t = mk(KIND_COUNTDOWN, false);
  TEST_ASSERT_EQUAL(VIEW_HERO_RUNNING, view_hero_branch(&t, false));
}

void test_fired_and_ringing_is_alarm(void) {
  Timer t = mk(KIND_COUNTDOWN, true);
  TEST_ASSERT_EQUAL(VIEW_HERO_ALARM, view_hero_branch(&t, true));
}

void test_fired_and_not_ringing_is_overtime(void) {
  Timer t = mk(KIND_COUNTDOWN, true);
  TEST_ASSERT_EQUAL(VIEW_HERO_OVERTIME, view_hero_branch(&t, false));
}

void test_stopwatch_always_stopwatch_branch_even_if_fired_bug(void) {
  // Defends the keepCounting-must-clear-fired invariant at the render layer
  // too: even if `fired` were somehow left true on a stopwatch, the branch
  // selector must never read it as ALARM/OVERTIME.
  Timer t = mk(KIND_STOPWATCH, true);
  TEST_ASSERT_EQUAL(VIEW_HERO_STOPWATCH, view_hero_branch(&t, true));
}

// A stopwatch
// with original_total_s > 0 (converted from a fired countdown via "keep
// counting") is its OWN branch now, distinct from a plain stopwatch --
// this is what lets the adapter give it a different primary action
// (Pause, not Lap) and a different value prefix (none, not "+").
void test_post_timer_stopwatch_is_its_own_branch(void) {
  Timer t = mk(KIND_STOPWATCH, false);
  t.original_total_s = 900;
  TEST_ASSERT_EQUAL(VIEW_HERO_POST_TIMER, view_hero_branch(&t, false));
}

void test_plain_stopwatch_stays_stopwatch_branch(void) {
  Timer t = mk(KIND_STOPWATCH, false);
  t.original_total_s = 0;
  TEST_ASSERT_EQUAL(VIEW_HERO_STOPWATCH, view_hero_branch(&t, false));
}

void test_repeat_fired_ringing_is_alarm(void) {
  Timer t = mk(KIND_REPEAT, true);
  TEST_ASSERT_EQUAL(VIEW_HERO_ALARM, view_hero_branch(&t, true));
}

void test_running_repeat_branch(void) {
  // The RUNNING branch (indigo hero, segmented bar) must be reached the
  // same kind-agnostic way a countdown's is -- fired is the only
  // discriminant, never kind (this is the branch selector's contract; the
  // adapter's own kind-specific segmented-bar-vs-draining-bar choice happens
  // downstream of this same branch, not by adding a new one here).
  Timer t = mk(KIND_REPEAT, false);
  TEST_ASSERT_EQUAL(VIEW_HERO_RUNNING, view_hero_branch(&t, false));
}

// --- color token ---------------------------------------------------------

void test_color_token_countdown_running(void) {
  TEST_ASSERT_EQUAL(VIEW_COLOR_COUNTDOWN, view_kind_color_token(KIND_COUNTDOWN, VIEW_HERO_RUNNING));
}

void test_color_token_overtime_stays_countdown_orange(void) {
  TEST_ASSERT_EQUAL(VIEW_COLOR_COUNTDOWN, view_kind_color_token(KIND_COUNTDOWN, VIEW_HERO_OVERTIME));
}

void test_color_token_oneoff_alarm_is_alarm_red(void) {
  TEST_ASSERT_EQUAL(VIEW_COLOR_ALARM, view_kind_color_token(KIND_COUNTDOWN, VIEW_HERO_ALARM));
}

void test_color_token_repeat_alarm_stays_indigo_not_red(void) {
  TEST_ASSERT_EQUAL(VIEW_COLOR_REPEAT, view_kind_color_token(KIND_REPEAT, VIEW_HERO_ALARM));
}

void test_color_token_stopwatch(void) {
  TEST_ASSERT_EQUAL(VIEW_COLOR_STOPWATCH, view_kind_color_token(KIND_STOPWATCH, VIEW_HERO_STOPWATCH));
}

// --- prefix kind -----------------------------------------------------------

void test_prefix_none_for_running(void) {
  TEST_ASSERT_EQUAL(VIEW_PREFIX_NONE, view_value_prefix(VIEW_HERO_RUNNING));
}

void test_prefix_caret_for_alarm(void) {
  TEST_ASSERT_EQUAL(VIEW_PREFIX_CARET, view_value_prefix(VIEW_HERO_ALARM));
}

void test_prefix_plus_for_overtime(void) {
  TEST_ASSERT_EQUAL(VIEW_PREFIX_PLUS, view_value_prefix(VIEW_HERO_OVERTIME));
}

void test_prefix_none_for_stopwatch(void) {
  // A plain stopwatch's MAIN value never carries a "+" prefix. Countdown-
  // family overrun (OVERTIME, above) is unaffected and keeps
  // VIEW_PREFIX_PLUS.
  TEST_ASSERT_EQUAL(VIEW_PREFIX_NONE, view_value_prefix(VIEW_HERO_STOPWATCH));
}

void test_prefix_none_for_post_timer(void) {
  // A post-timer stopwatch's big value is a plain elapsed total, not a
  // delta -- its "+" lives on the separate overrun chip instead.
  TEST_ASSERT_EQUAL(VIEW_PREFIX_NONE, view_value_prefix(VIEW_HERO_POST_TIMER));
}

// --- view_row_label ----------------------------------------------------

void test_row_label_null_name_is_unlabeled(void) {
  // An unnamed timer (name == NULL) has NO label text -- glyph, kind
  // colour, and magnitude carry its identity alone.
  TEST_ASSERT_NULL(view_row_label(NULL));
}

void test_row_label_empty_name_is_unlabeled(void) {
  // "" is treated the same as NULL, not drawn as a blank label.
  TEST_ASSERT_NULL(view_row_label(""));
}

void test_row_label_named_timer_returns_name_unchanged(void) {
  // A named timer (from a preset or from voice dictation) returns its name
  // as-is -- no ellipsis/clamp logic here, since such names are short by
  // construction and never need truncation.
  TEST_ASSERT_EQUAL_STRING("egg", view_row_label("egg"));
}

// view_no_bg_alarm() and its test were removed because recomputing armed
// state in the view could disagree with firmware. See lib/view.h's comment
// at that removed declaration's old location for why.

// --- HOME per-count value sizing --------------------------------------------
// `view_home_scroll_offset` (the hand-rolled windowed-scroll math) has been
// removed -- HOME's populated state is a real MenuLayer, whose own scroll
// layer supersedes it entirely (see lib/view.h's removal note). This
// section is the new per-count sizing seam.

void test_home_value_size_xlarge_for_zero_and_one(void) {
  // 1 and 2 timers used to share one rung
  // (both 32px) -- this sizing ramp makes HOME-1 bigger and steps DOWN
  // once a 2nd timer appears. 0 shares the same top rung (moot in practice
  // -- HOME shows no rows at all when empty -- but keeps the function total).
  TEST_ASSERT_EQUAL(VIEW_HOME_SIZE_XLARGE, view_home_value_size(0));
  TEST_ASSERT_EQUAL(VIEW_HOME_SIZE_XLARGE, view_home_value_size(1));
}

void test_home_value_size_large_for_two(void) {
  TEST_ASSERT_EQUAL(VIEW_HOME_SIZE_LARGE, view_home_value_size(2));
}

void test_home_value_size_medium_for_three_and_four(void) {
  TEST_ASSERT_EQUAL(VIEW_HOME_SIZE_MEDIUM, view_home_value_size(3));
  TEST_ASSERT_EQUAL(VIEW_HOME_SIZE_MEDIUM, view_home_value_size(4));
}

void test_home_value_size_small_once_scrolling_starts(void) {
  // 5 is the first count that overflows MAX_VISIBLE_ROWS (4) --
  // the boundary where HOME starts scrolling.
  TEST_ASSERT_EQUAL(VIEW_HOME_SIZE_SMALL, view_home_value_size(5));
  TEST_ASSERT_EQUAL(VIEW_HOME_SIZE_SMALL, view_home_value_size(16));  // MAX_TIMERS
}

// --- view_home_row_count -----------------------------------------------------
// Host-pinning the exact boundary: an off-by-one bug could let n==0 fail to
// floor to a SMALL flat row count (no "Clear all"), instead using an
// un-gated formula that left a hidden Clear-all row reachable from an empty
// store. The Presets feature later inserted a "Library" row into both the
// empty and populated shapes -- see view_home_row_count's own doc comment
// for the exact row model this pins.

void test_home_row_count_zero_is_new_timer_and_library_only(void) {
  // Empty HOME: row 0 = "+ New timer" hero, row 1 = "Say a timer", row 2 =
  // "Library" -- no "Clear all" (nothing to clear). This grew over time as
  // features were added: originally floored to 1 (footer-only), then the
  // Library row was added, then the voice row.
  TEST_ASSERT_EQUAL_INT(3, view_home_row_count(0));
}

void test_home_row_count_one_timer(void) {
  // 1 hero row + "+ New timer" + "Say a timer" + "Library" + "Clear all".
  TEST_ASSERT_EQUAL_INT(5, view_home_row_count(1));
}

void test_home_row_count_two_timers(void) {
  TEST_ASSERT_EQUAL_INT(6, view_home_row_count(2));
}

void test_home_row_count_fifteen_timers(void) {
  TEST_ASSERT_EQUAL_INT(19, view_home_row_count(15));
}

void test_home_row_count_sixteen_timers_max(void) {
  // MAX_TIMERS -- Clear-all (and Library) stay functional at max (only the
  // ADD footer/voice rows above are gated, an unrelated concern this pure
  // row-count doesn't decide).
  TEST_ASSERT_EQUAL_INT(20, view_home_row_count(16));
}

// --- view_home_voice_row -----------------------------------------------------
// Host-pins the row-index arithmetic screen_home.c's cursor-restore path
// (nav_go_home_focus_voice_row()'s consumer, home_refresh_appearance()) and
// draw_row/select_click's own row dispatch must never drift apart on --
// the same class of off-by-one as view_home_row_count above.

void test_home_voice_row_empty_store_is_row_one(void) {
  // Empty HOME: row 0 = "+ New timer" hero, row 1 = "Say a timer".
  TEST_ASSERT_EQUAL_INT(1, view_home_voice_row(0));
}

void test_home_voice_row_one_timer_is_row_two(void) {
  // Populated: row 1 (== timer_count) = "+ New timer", row 2 = "Say a timer".
  TEST_ASSERT_EQUAL_INT(2, view_home_voice_row(1));
}

void test_home_voice_row_sixteen_timers_max(void) {
  TEST_ASSERT_EQUAL_INT(17, view_home_voice_row(16));
}

// --- view_library_row_count / view_library_voice_row (mirrors the HOME
// extraction pair's own test shape) ------------------------------------------

void test_library_row_count_zero_presets(void) {
  // "Save new" (row 0) + "Say a preset" (row 1) -- the empty state's row 0
  // draws as the composed hero, but the ROW COUNT is unaffected.
  TEST_ASSERT_EQUAL_INT(2, view_library_row_count(0));
}

void test_library_row_count_one_preset(void) {
  TEST_ASSERT_EQUAL_INT(3, view_library_row_count(1));
}

void test_library_row_count_max_presets(void) {
  TEST_ASSERT_EQUAL_INT(14, view_library_row_count(12));  // MAX_PRESETS
}

void test_library_voice_row_zero_presets_is_row_one(void) {
  TEST_ASSERT_EQUAL_INT(1, view_library_voice_row(0));
}

void test_library_voice_row_one_preset_is_row_two(void) {
  TEST_ASSERT_EQUAL_INT(2, view_library_voice_row(1));
}

void test_library_voice_row_max_presets(void) {
  TEST_ASSERT_EQUAL_INT(13, view_library_voice_row(12));  // MAX_PRESETS
}

// --- view_others_active_count ("ringing · ... still on" subheader) ---------

void test_others_active_count_zero_for_lone_timer(void) {
  TimerStore s;
  store_init(&s);
  int32_t cookie;
  store_add(&s, KIND_COUNTDOWN, 60, 1000, &cookie);
  TEST_ASSERT_EQUAL_INT(0, view_others_active_count(cookie, &s));
}

void test_others_active_count_excludes_self(void) {
  TimerStore s;
  store_init(&s);
  int32_t a, b, c;
  store_add(&s, KIND_COUNTDOWN, 60, 1000, &a);
  store_add(&s, KIND_STOPWATCH, 0, 1000, &b);
  store_add(&s, KIND_REPEAT, 300, 1000, &c);
  TEST_ASSERT_EQUAL_INT(2, view_others_active_count(a, &s));
  TEST_ASSERT_EQUAL_INT(2, view_others_active_count(b, &s));
  TEST_ASSERT_EQUAL_INT(2, view_others_active_count(c, &s));
}

void test_others_active_count_zero_for_empty_store(void) {
  TimerStore s;
  store_init(&s);
  TEST_ASSERT_EQUAL_INT(0, view_others_active_count(999, &s));
}

// A paused sibling must NOT count toward "still on" -- previously, this
// counted every other timer regardless of is_paused, so the subheader could
// read "1 other still on" while that other timer was sitting paused, not
// running.
void test_others_active_count_excludes_paused_sibling(void) {
  TimerStore s;
  store_init(&s);
  int32_t a, b;
  store_add(&s, KIND_COUNTDOWN, 60, 1000, &a);
  store_add(&s, KIND_COUNTDOWN, 60, 1000, &b);
  Timer *paused = store_find(&s, b);
  timer_pause(paused, 1000);
  TEST_ASSERT_EQUAL_INT(0, view_others_active_count(a, &s));
}

// A mix of a paused sibling and a running one only counts the running one.
void test_others_active_count_mixed_paused_and_running(void) {
  TimerStore s;
  store_init(&s);
  int32_t a, b, c;
  store_add(&s, KIND_COUNTDOWN, 60, 1000, &a);
  store_add(&s, KIND_COUNTDOWN, 60, 1000, &b);
  store_add(&s, KIND_STOPWATCH, 0, 1000, &c);
  Timer *paused = store_find(&s, b);
  timer_pause(paused, 1000);
  TEST_ASSERT_EQUAL_INT(1, view_others_active_count(a, &s));
}

// A fired-but-not-ringing (OVERTIME) sibling IS still counted -- it's still
// counting up, just not currently buzzing (distinct from paused, which has
// genuinely stopped).
void test_others_active_count_includes_fired_not_paused_sibling(void) {
  TimerStore s;
  store_init(&s);
  int32_t a, b;
  store_add(&s, KIND_COUNTDOWN, 60, 1000, &a);
  store_add(&s, KIND_COUNTDOWN, 60, 1000, &b);
  Timer *overtime = store_find(&s, b);
  overtime->fired = true;
  TEST_ASSERT_EQUAL_INT(1, view_others_active_count(a, &s));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_running_countdown_branch);
  RUN_TEST(test_fired_and_ringing_is_alarm);
  RUN_TEST(test_fired_and_not_ringing_is_overtime);
  RUN_TEST(test_stopwatch_always_stopwatch_branch_even_if_fired_bug);
  RUN_TEST(test_post_timer_stopwatch_is_its_own_branch);
  RUN_TEST(test_plain_stopwatch_stays_stopwatch_branch);
  RUN_TEST(test_repeat_fired_ringing_is_alarm);
  RUN_TEST(test_running_repeat_branch);
  RUN_TEST(test_color_token_countdown_running);
  RUN_TEST(test_color_token_overtime_stays_countdown_orange);
  RUN_TEST(test_color_token_oneoff_alarm_is_alarm_red);
  RUN_TEST(test_color_token_repeat_alarm_stays_indigo_not_red);
  RUN_TEST(test_color_token_stopwatch);
  RUN_TEST(test_prefix_none_for_running);
  RUN_TEST(test_prefix_caret_for_alarm);
  RUN_TEST(test_prefix_plus_for_overtime);
  RUN_TEST(test_prefix_none_for_stopwatch);
  RUN_TEST(test_prefix_none_for_post_timer);
  RUN_TEST(test_row_label_null_name_is_unlabeled);
  RUN_TEST(test_row_label_empty_name_is_unlabeled);
  RUN_TEST(test_row_label_named_timer_returns_name_unchanged);
  RUN_TEST(test_home_value_size_xlarge_for_zero_and_one);
  RUN_TEST(test_home_value_size_large_for_two);
  RUN_TEST(test_home_value_size_medium_for_three_and_four);
  RUN_TEST(test_home_value_size_small_once_scrolling_starts);
  RUN_TEST(test_home_row_count_zero_is_new_timer_and_library_only);
  RUN_TEST(test_home_row_count_one_timer);
  RUN_TEST(test_home_row_count_two_timers);
  RUN_TEST(test_home_row_count_fifteen_timers);
  RUN_TEST(test_home_row_count_sixteen_timers_max);
  RUN_TEST(test_home_voice_row_empty_store_is_row_one);
  RUN_TEST(test_home_voice_row_one_timer_is_row_two);
  RUN_TEST(test_home_voice_row_sixteen_timers_max);
  RUN_TEST(test_library_row_count_zero_presets);
  RUN_TEST(test_library_row_count_one_preset);
  RUN_TEST(test_library_row_count_max_presets);
  RUN_TEST(test_library_voice_row_zero_presets_is_row_one);
  RUN_TEST(test_library_voice_row_one_preset_is_row_two);
  RUN_TEST(test_library_voice_row_max_presets);
  // NOTE: these 6 cases (3 pre-existing + 3 added later) were not wired
  // into RUN_TEST at all -- silently never executed. Wired in now.
  RUN_TEST(test_others_active_count_zero_for_lone_timer);
  RUN_TEST(test_others_active_count_excludes_self);
  RUN_TEST(test_others_active_count_zero_for_empty_store);
  RUN_TEST(test_others_active_count_excludes_paused_sibling);
  RUN_TEST(test_others_active_count_mixed_paused_and_running);
  RUN_TEST(test_others_active_count_includes_fired_not_paused_sibling);
  return UNITY_END();
}
