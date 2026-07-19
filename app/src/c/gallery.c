// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

#include "gallery.h"

#ifdef GALLERY_MODE

#include <stdio.h>

#include "app_context.h"
#include "dictation_am.h"
#include "hold.h"
#include "lib/app_state.h"
#include "lib/format.h"
#include "lib/parse_nl.h"
#include "lib/preset_store.h"
#include "lib/store.h"
#include "lib/timer.h"
#include "lib/voice_outcome.h"
#include "nav.h"
#include "render.h"
#include "screen_actions.h"
#include "screen_alarm.h"
#include "screen_hero.h"
#include "screen_home.h"
#include "screen_library.h"
#include "screen_preset_actions.h"
#include "screen_set.h"
#include "screen_voice.h"
#include "testlog_am.h"

// --- shared helpers, reused by several scenes below -------------------------

// Clears the fixture down to empty via the REAL store_remove() mutation
// (never store_init(), which would also reset TimerStore.next_cookie back to
// 1 -- consecutive gallery scenes that land on the SAME ViewDescKind would
// then often reuse the exact same cookie value the previous scene used,
// which nav_diff_stack (lib/nav_stack.c) would read as "already showing
// this exact view" and skip the window transition entirely, silently
// leaving a real screen's window_appear-driven MenuLayer reload/cursor-
// reset from firing). Monotonically-increasing cookies across scenes avoid
// that class of bug outright. Also resets AppState via its own real init.
static void gallery_reset_fixture(void) {
  TimerStore *store = app_context_store();
  while (store->count > 0) {
    store_remove(store, store->timers[0].cookie);
  }
  app_state_init(app_context_app_state());
  hold_debug_force(-1);  // clear any forced hold-fill left by a prior scene
  // Same leak-guard reasoning as the resets above -- hold_debug_force_button
  // defaults to SELECT (hold.h's own doc comment), and the transient
  // short-Up-press hint must not leak into a scene that never armed it
  // itself.
  hold_debug_force_button(BUTTON_ID_SELECT);
  screen_hero_debug_force_up_hint(false);
  // Library's own transient discoverability-hint lever
  // (screen_library_debug_force_hint()) is the SAME leak-guard shape as
  // screen_hero_debug_force_up_hint() immediately above -- it must be reset
  // here too, or a scene that never armed it itself can inherit whatever a
  // prior scene left forced on.
  screen_library_debug_force_hint(false);
  // The real PresetStore is a module-static singleton in app_context.c, same
  // leak-guard reasoning as the resets above -- reset it via its own real
  // init, never a hand-set struct field.
  preset_store_init(app_context_presets());
  // screen_voice's clock-target-annotation am/pm style lever, same
  // leak-guard shape as every forcing lever above -- must not leak into a
  // scene that never armed it itself. -1 = not forced (falls through to the
  // real clock_is_24h_style()).
  screen_voice_debug_force_24h_style(-1);
  // VD_VOICE is a real nav node (nav_init() creates s_voice unconditionally)
  // -- no gallery-local window/reset scaffolding is needed here. Every
  // voice_confirm_*/voice_parse_fail/voice_no_*/voice_disabled scene below
  // calls the real screen_voice_show_parse() with a FRESH parse/outcome
  // right after nav_go_voice(), so there is no partial NlParse/VoiceOutcome
  // state to separately zero here.
}

// Equivalent to scan_and_handle_firings (main.c), MINUS the side effects that
// don't belong in a static fixture builder (persist_adapter_save,
// wakeup_adapter_replan, vibes_adapter_alarm) -- the TIMER-STATE mutation
// (fired=true, ringing add) and the NAV ROUTING (solo hero vs. ringing list)
// are real, unmodified lib/app_state.c + nav.c logic, exactly matching
// production's own loop shape in main.c's scan_and_handle_firings.
static void gallery_fire_all_due(void) {
  TimerStore *store = app_context_store();
  AppState *app_state = app_context_app_state();
  int32_t fired[MAX_TIMERS];
  int n = app_state_scan_firings(app_state, store, time(NULL), fired, MAX_TIMERS);
  for (int i = 0; i < n; i++) {
    nav_on_timer_fired(fired[i]);
  }
}

// Forces a MenuLayer's reload+cursor-reset+redraw directly -- the real
// window_appear() does this too, but ONLY on an actual window-stack
// transition (nav_diff_stack's no-op guarantee, see nav.c); consecutive
// gallery scenes that both land on the same ViewDescKind with a genuinely
// different cookie (e.g. two ACTIONS scenes in a row) still need this
// refreshed explicitly. Public SDK MenuLayer calls only -- no fixture data
// touched here, this is UI-widget state, not the Timer/TimerStore model.
static void gallery_refresh_menu(MenuLayer *menu, int selected_row) {
  menu_layer_reload_data(menu);
  menu_layer_set_selected_index(menu, MenuIndex(0, selected_row), MenuRowAlignTop, false);
  layer_mark_dirty(menu_layer_get_layer(menu));
}

static void mark_scene(int index, const char *slug) {
  char data[40];
  snprintf(data, sizeof data, "%d:%s", index, slug);
  testlog("gallery_scene", data);
}

// --- HOME family --------------------------------------------------------

// Every HOME-family scene below calls screen_home_debug_refresh() right
// after nav_go_home() -- HOME's ViewDesc ignores cookie entirely (always
// kind=VD_HOME, cookie=0), so two HOME-targeting scenes in a row are
// IDENTICAL ViewDescs and nav_diff_stack (lib/nav_stack.c) treats the
// second nav_go_home() as a no-op transition -- window_appear (the normal
// trigger for HOME's empty/populated visibility toggle + MenuLayer reload)
// never fires. Without this, a scene built with a freshly-populated store
// can still render the empty-state affordance, because window_appear hasn't
// fired since an earlier empty-store scene. See screen_home.c's
// home_refresh_appearance() doc comment.
static void scene_home_empty(void) {
  gallery_reset_fixture();
  nav_go_home();
  screen_home_debug_refresh();
  mark_scene(0, "home_empty");
}

static void scene_home_1_countdown(void) {
  gallery_reset_fixture();
  store_add(app_context_store(), KIND_COUNTDOWN, 405, time(NULL), NULL);  // 6:45
  nav_go_home();
  screen_home_debug_refresh();
  mark_scene(1, "home_1_countdown");
}

// Two timers, mixed kinds -- a countdown (with its progress bar) alongside a
// stopwatch (without one), exercising the bar/no-bar + 2-timer-row contrast.
static void scene_home_2_timers(void) {
  gallery_reset_fixture();
  time_t now = time(NULL);
  store_add(app_context_store(), KIND_COUNTDOWN, 372, now, NULL);      // 6:12 remaining
  store_add(app_context_store(), KIND_STOPWATCH, 0, now - 30, NULL);   // +0:30 elapsed
  nav_go_home();
  screen_home_debug_refresh();
  mark_scene(2, "home_2_timers");
}

// Top-of-scroll variant: 5 timers with distinct magnitudes, scrolled to the
// top of the list.
static void scene_home_scroll_top(void) {
  gallery_reset_fixture();
  time_t now = time(NULL);
  store_add(app_context_store(), KIND_COUNTDOWN, 1, now, NULL);    // 0:01
  store_add(app_context_store(), KIND_COUNTDOWN, 54, now, NULL);   // 0:54
  store_add(app_context_store(), KIND_COUNTDOWN, 108, now, NULL);  // 1:48
  store_add(app_context_store(), KIND_COUNTDOWN, 200, now, NULL);  // 3:20
  store_add(app_context_store(), KIND_COUNTDOWN, 300, now, NULL);  // 5:00
  nav_go_home();
  screen_home_debug_refresh();
  mark_scene(3, "home_scroll_top");
}

// Cursor-moved-down variant (bonus): same 5-timer fixture, cursor moved so
// the scroll thumb sits visibly lower.
static void scene_home_scroll_cursor(void) {
  gallery_reset_fixture();
  time_t now = time(NULL);
  store_add(app_context_store(), KIND_COUNTDOWN, 1, now, NULL);
  store_add(app_context_store(), KIND_COUNTDOWN, 54, now, NULL);
  store_add(app_context_store(), KIND_COUNTDOWN, 108, now, NULL);
  store_add(app_context_store(), KIND_COUNTDOWN, 200, now, NULL);
  store_add(app_context_store(), KIND_COUNTDOWN, 300, now, NULL);
  nav_go_home();
  screen_home_debug_refresh();
  gallery_refresh_menu(screen_home_debug_menu(), 3);  // move cursor down after the base refresh
  mark_scene(4, "home_scroll_cursor");
}

// --- CREATE ---------------------------------------------------------------

static void scene_set_build_duration(void) {
  gallery_reset_fixture();
  nav_go_set();
  screen_set_debug_dial(0, 12, 0);
  mark_scene(5, "set_build_duration");
}

// --- RUNNING HEROES ---------------------------------------------------------

static void scene_countdown_hero_running(void) {
  gallery_reset_fixture();
  int32_t cookie;
  store_add(app_context_store(), KIND_COUNTDOWN, 600, time(NULL) - 442, &cookie);  // remaining 2:38
  nav_go_hero(cookie);
  mark_scene(6, "countdown_hero_running");
}

// Paused variant (bonus).
static void scene_countdown_hero_paused(void) {
  gallery_reset_fixture();
  time_t now = time(NULL);
  int32_t cookie;
  store_add(app_context_store(), KIND_COUNTDOWN, 600, now - 200, &cookie);
  Timer *t = store_find(app_context_store(), cookie);
  timer_pause(t, now);
  nav_go_hero(cookie);
  mark_scene(7, "countdown_hero_paused");
}

// Mid-hold-fill variant (bonus): forces hold.h's debug override rather than
// timing a real button hold, so the fraction is exact and reproducible.
static void scene_countdown_hero_holdfill(void) {
  gallery_reset_fixture();
  int32_t cookie;
  store_add(app_context_store(), KIND_COUNTDOWN, 600, time(NULL) - 100, &cookie);
  nav_go_hero(cookie);
  hold_debug_force(600);  // 60% -- per-mille (RENDER_FRACTION_SCALE), was 0.6f
  screen_hero_mark_dirty();
  mark_scene(8, "countdown_hero_holdfill");
}

// Fired via the real app_state_scan_firings detection, then taken OUT of the
// ringing set (app_state_ringing_remove) -- fired && !ringing is exactly
// view_hero_branch's OVERTIME condition (lib/view.c).
static void scene_countdown_overtime(void) {
  gallery_reset_fixture();
  time_t now = time(NULL);
  int32_t cookie;
  store_add(app_context_store(), KIND_COUNTDOWN, 300, now - 326, &cookie);  // overrun 0:26
  int32_t fired[MAX_TIMERS];
  app_state_scan_firings(app_context_app_state(), app_context_store(), now, fired, MAX_TIMERS);
  app_state_ringing_remove(app_context_app_state(), cookie);
  nav_go_hero(cookie);
  mark_scene(9, "countdown_overtime");
}

static void scene_stopwatch_hero_running(void) {
  gallery_reset_fixture();
  int32_t cookie;
  store_add(app_context_store(), KIND_STOPWATCH, 0, time(NULL) - 270, &cookie);  // +4:30
  nav_go_hero(cookie);
  mark_scene(10, "stopwatch_hero_running");
}

// timer_advance_cycle is the ONLY mutation that increments cycles -- called
// 3x (real function, matching three real "Keep going" presses), backdating
// the LAST call's `now` to land mid-interval instead of at cycle-start.
static void scene_repeat_hero_running(void) {
  gallery_reset_fixture();
  time_t now = time(NULL);
  int32_t cookie;
  store_add(app_context_store(), KIND_REPEAT, 300, now, &cookie);  // 5:00 interval
  Timer *t = store_find(app_context_store(), cookie);
  timer_advance_cycle(t, now);
  timer_advance_cycle(t, now);
  timer_advance_cycle(t, now - 8);  // cycles=3, 8s into this cycle -> remaining 4:52
  nav_go_hero(cookie);
  mark_scene(11, "repeat_hero_running");
}

// timer_keep_counting is the real countdown->stopwatch mutation (preserves
// started_at/duration_s so elapsed() continues seamlessly through the
// zero-crossing, per its own doc in lib/timer.h).
static void scene_post_timer_stopwatch(void) {
  gallery_reset_fixture();
  time_t now = time(NULL);
  int32_t cookie;
  store_add(app_context_store(), KIND_COUNTDOWN, 900, now - 938, &cookie);  // 15:00 duration
  Timer *t = store_find(app_context_store(), cookie);
  timer_keep_counting(t, now);  // total elapsed 15:38, overrun chip +0:38
  nav_go_hero(cookie);
  mark_scene(12, "post_timer_stopwatch");
}

// --- ACTIONS (collapsed lists) ----------------------------------------------

static void scene_actions_countdown(void) {
  gallery_reset_fixture();
  int32_t cookie;
  store_add(app_context_store(), KIND_COUNTDOWN, 600, time(NULL) - 442, &cookie);
  nav_go_actions(cookie);
  gallery_refresh_menu(screen_actions_debug_menu(), 0);
  mark_scene(13, "actions_countdown");
}

// Options-cursor variant (bonus) -- cursor on row 2 ("+1:00").
static void scene_actions_countdown_cursor(void) {
  gallery_reset_fixture();
  int32_t cookie;
  store_add(app_context_store(), KIND_COUNTDOWN, 600, time(NULL) - 442, &cookie);
  nav_go_actions(cookie);
  gallery_refresh_menu(screen_actions_debug_menu(), 2);
  mark_scene(14, "actions_countdown_cursor");
}

static void scene_actions_stopwatch(void) {
  gallery_reset_fixture();
  int32_t cookie;
  store_add(app_context_store(), KIND_STOPWATCH, 0, time(NULL) - 270, &cookie);
  nav_go_actions(cookie);
  gallery_refresh_menu(screen_actions_debug_menu(), 0);
  mark_scene(15, "actions_stopwatch");
}

// Same fixture recipe as scene_repeat_hero_running above.
static void scene_actions_repeat(void) {
  gallery_reset_fixture();
  time_t now = time(NULL);
  int32_t cookie;
  store_add(app_context_store(), KIND_REPEAT, 300, now, &cookie);
  Timer *t = store_find(app_context_store(), cookie);
  timer_advance_cycle(t, now);
  timer_advance_cycle(t, now);
  timer_advance_cycle(t, now - 8);
  nav_go_actions(cookie);
  gallery_refresh_menu(screen_actions_debug_menu(), 0);
  mark_scene(16, "actions_repeat");
}

// timer_add_lap is the real mutation; backdating started_at before the lap
// call lands the recorded split at exactly 1:12.
static void scene_laps_one_lap(void) {
  gallery_reset_fixture();
  time_t now = time(NULL);
  int32_t cookie;
  store_add(app_context_store(), KIND_STOPWATCH, 0, now - 72, &cookie);
  Timer *t = store_find(app_context_store(), cookie);
  store_add_lap(app_context_store(), t, now);
  nav_go_laps(cookie);
  mark_scene(17, "laps_one_lap");
}

// --- ALERT (fired alarms) ---------------------------------------------------

// A running stopwatch sibling is added so the "ringing - N others still on"
// subheader has something real to count (lib/view.c's
// view_others_active_count) -- count-only, per this app's no-per-timer-naming
// substitution, already documented in lib/view.h.
static void scene_alarm_oneoff_solo(void) {
  gallery_reset_fixture();
  nav_go_home();
  time_t now = time(NULL);
  store_add(app_context_store(), KIND_STOPWATCH, 0, now - 405, NULL);  // sibling still running
  int32_t cookie;
  store_add(app_context_store(), KIND_COUNTDOWN, 60, now - 67, &cookie);  // overrun 0:07
  gallery_fire_all_due();
  mark_scene(18, "alarm_oneoff_v1_solo");
}

static void scene_alarm_oneoff_options(void) {
  gallery_reset_fixture();
  nav_go_home();
  time_t now = time(NULL);
  int32_t cookie;
  store_add(app_context_store(), KIND_COUNTDOWN, 60, now - 67, &cookie);
  gallery_fire_all_due();
  nav_go_alarm_options(cookie);
  mark_scene(19, "alarm_oneoff_v2_options");
}

static void scene_alarm_repeat_fired(void) {
  gallery_reset_fixture();
  nav_go_home();
  time_t now = time(NULL);
  int32_t cookie;
  store_add(app_context_store(), KIND_REPEAT, 300, now - 304, &cookie);  // overrun 0:04
  gallery_fire_all_due();
  mark_scene(20, "alarm_repeat_fired");
}

// Three timers fired in one gallery_fire_all_due() pass -- matches main.c's
// real scan_and_handle_firings loop shape exactly (a single
// app_state_scan_firings() call, then nav_on_timer_fired() per newly-fired
// cookie).
static void scene_multi_alarm_ringing_list(void) {
  gallery_reset_fixture();
  nav_go_home();
  time_t now = time(NULL);
  store_add(app_context_store(), KIND_COUNTDOWN, 60, now - 67, NULL);    // egg, overrun 0:07
  store_add(app_context_store(), KIND_COUNTDOWN, 120, now - 140, NULL);  // pasta, overrun 0:20
  store_add(app_context_store(), KIND_REPEAT, 300, now - 305, NULL);     // oven, overrun 0:05
  gallery_fire_all_due();
  mark_scene(21, "multi_alarm_ringing_list");
}

// --- CREATE-REPEAT --------------------------------------------------------
//
// The shipped shape: dial UNCHANGED (cursor stays on minutes,
// screen_set_debug_dial's own default), repeat toggled on via the REAL
// set_machine_toggle_kind (screen_set_debug_kind's contract) -- indigo
// recolor + a small passive "Repeat" chip + gesture hint, no active-field
// emphasis on the chip itself. Kind selection lives entirely in
// set_machine_toggle_kind: there is no appended spinner field for kind and
// no separate kind-picker screen -- both would be redundant nav destinations
// for a value this toggle already owns.
static void scene_set_repeat(void) {
  gallery_reset_fixture();
  nav_go_set();
  screen_set_debug_kind(0, 5, 0, true);
  mark_scene(22, "set_repeat");
}

// --- RINGING/OVERTIME/SNOOZE -------------------------------------------------
//
// The alarm hero's target/reference-line + hidden-segbar framing is
// unconditional now (screen_alarm.c's own hero render, no debug option to
// select) -- scene_alarm_oneoff_solo ("with others") and
// scene_alarm_repeat_fired already exercise those real paths, so this
// section only keeps the genuinely DISTINCT scenes: the "no others"
// reference-line check, the repeat options menu, the alarm Up-hold
// snooze-1/4 gesture (resting + gated-off), and the real snooze fixture (via
// timer_snooze_repeat, not a forced debug branch).

// Variant with NO running sibling (others == 0) -- checks the reference
// reads correctly when the "others" subheader is absent.
static void scene_alarm_oneoff_ringing_solo(void) {
  gallery_reset_fixture();
  nav_go_home();
  time_t now = time(NULL);
  int32_t cookie;
  store_add(app_context_store(), KIND_COUNTDOWN, 60, now - 67, &cookie);  // overrun 0:07, no sibling
  gallery_fire_all_due();
  mark_scene(23, "alarm_oneoff_ringing_solo");
}

// The repeat alarm's OWN options menu (Down from a fired repeat). Duration
// 300s -> 1/4 = 75s (> the 10s gate), so the "Snooze 1/4 (1:15)" row appears
// alongside the three fixed rows.
static void scene_alarm_repeat_options(void) {
  gallery_reset_fixture();
  nav_go_home();
  time_t now = time(NULL);
  int32_t cookie;
  store_add(app_context_store(), KIND_REPEAT, 300, now - 304, &cookie);  // overrun 0:04
  gallery_fire_all_due();
  nav_go_alarm_options(cookie);
  mark_scene(24, "alarm_repeat_options");
}

// The alarm hero's own Up-hold (snooze 1/4), MID-HOLD -- same duration
// as scene_alarm_oneoff_solo (60s -> 1/4 = 15s, gated ON); Up forced held at
// 60% fill so the LEFT hold-action label swaps to "Snooze 0:15" and the
// right slot shows the stacked "snooze 0:15"/"more" legend (footer-legend
// rule, same idiom as the countdown/repeat Up-hold holdfill scenes).
static void scene_alarm_snooze_quarter_holdfill(void) {
  gallery_reset_fixture();
  nav_go_home();
  time_t now = time(NULL);
  int32_t cookie;
  store_add(app_context_store(), KIND_COUNTDOWN, 60, now - 67, &cookie);
  gallery_fire_all_due();
  hold_debug_force_button(BUTTON_ID_UP);
  hold_debug_force(600);  // 60% -- per-mille (RENDER_FRACTION_SCALE), was 0.6f
  screen_alarm_mark_dirty();
  mark_scene(25, "alarm_snooze_quarter_holdfill");
}

// The alarm hero's Up-hold, GATED OFF (duration 0:30 -> 1/4 = 7s, below
// the 10s gate) -- proves the gesture AND its hint are both absent
// (defensively forced-held too, mirroring scene_countdown_addquarter_
// gated_off's own proof shape).
static void scene_alarm_snooze_quarter_gated_off(void) {
  gallery_reset_fixture();
  nav_go_home();
  time_t now = time(NULL);
  int32_t cookie;
  store_add(app_context_store(), KIND_COUNTDOWN, 30, now - 35, &cookie);  // 0:30, overrun 0:05, well under gate
  gallery_fire_all_due();
  hold_debug_force_button(BUTTON_ID_UP);
  hold_debug_force(600);  // 60% -- per-mille (RENDER_FRACTION_SCALE), was 0.6f
  screen_alarm_mark_dirty();
  mark_scene(26, "alarm_snooze_quarter_gated_off");
}

// Snoozed-repeat display, via the REAL timer_snooze_repeat mutation (not a
// forced debug branch): 5:00 interval, done x3, then snoozed --
// duration_s/4 = 75s = 1:15 remaining, which timer_is_snoozing() now reports
// true for.
static void scene_repeat_hero_snoozing(void) {
  gallery_reset_fixture();
  time_t now = time(NULL);
  int32_t cookie;
  store_add(app_context_store(), KIND_REPEAT, 300, now, &cookie);  // 5:00 interval
  Timer *t = store_find(app_context_store(), cookie);
  timer_advance_cycle(t, now);
  timer_advance_cycle(t, now);
  timer_advance_cycle(t, now);  // cycles=3 ("done x3")
  timer_snooze_repeat(t, now);  // real mutation: remaining=1:15, marker set
  nav_go_hero(cookie);
  mark_scene(27, "repeat_hero_snoozing");
}

// A sibling timer is added (n=2 -> LARGE 32px numeral, per
// view_home_value_size) rather than showing the snoozed repeat alone (n=1
// would force XLARGE's 42px numeral, which leaves so little row width that
// even a short "snoozing" label ellipsizes to "sno..."). Two timers is also
// the more representative case (a solo snoozed repeat with nothing else
// running is the less common shape).
static void scene_home_repeat_snoozing(void) {
  gallery_reset_fixture();
  time_t now = time(NULL);
  store_add(app_context_store(), KIND_COUNTDOWN, 600, now - 442, NULL);  // sibling, 2:38 remaining
  int32_t cookie;
  store_add(app_context_store(), KIND_REPEAT, 300, now, &cookie);
  Timer *t = store_find(app_context_store(), cookie);
  timer_advance_cycle(t, now);
  timer_advance_cycle(t, now);
  timer_advance_cycle(t, now);
  timer_snooze_repeat(t, now);  // real mutation, same as scene_repeat_hero_snoozing
  nav_go_home();
  screen_home_debug_refresh();
  mark_scene(28, "home_repeat_snoozing");
}

// --- STOPWATCH LAPS + CONTROL-MODEL -----------------------------------------
//
// Laps rendered directly on the main hero screen, plus the
// Select-hold=Pause/Up-hold=Add-lap control model. No debug gate on the
// render path itself -- these scenes still use
// hold_debug_force()/hold_debug_force_button()/screen_hero_debug_force_up_
// hint() to depict transient/held states as static, reproducible
// screenshots, same "force the visual state, skip the real timer" shape
// every other holdfill scene in this file already uses. Fixture: a 5-lap
// stopwatch built entirely through real store_add()/timer_add_lap() calls
// with synthetic `now` arguments (never a hand-set Timer field) -- laps_s
// ends up 0:41/1:35/2:38/3:25/4:22 (cumulative), current elapsed ~5:00 at
// render time.
static int32_t stopwatch_5lap_fixture(void) {
  int32_t cookie;
  time_t base = time(NULL) - 300;  // started 5:00 ago
  store_add(app_context_store(), KIND_STOPWATCH, 0, base, &cookie);
  Timer *t = store_find(app_context_store(), cookie);
  store_add_lap(app_context_store(), t, base + 41);
  store_add_lap(app_context_store(), t, base + 95);
  store_add_lap(app_context_store(), t, base + 158);
  store_add_lap(app_context_store(), t, base + 205);
  store_add_lap(app_context_store(), t, base + 262);
  return cookie;
}

// Resting state: the last-two-laps list + the stacked "^ new
// lap"/"v more" legend + the resting "Hold to pause" footer pill.
static void scene_stopwatch_running(void) {
  gallery_reset_fixture();
  int32_t cookie = stopwatch_5lap_fixture();
  nav_go_hero(cookie);
  mark_scene(29, "stopwatch_running");
}

// Empty state (0 laps, no timer_add_lap calls at all) -- the "Hold Up to add
// a lap" discovery hint, no icon legend (see render_stopwatch_laps_band's
// own doc comment for why).
static void scene_stopwatch_laps_empty(void) {
  gallery_reset_fixture();
  time_t now = time(NULL);
  int32_t cookie;
  store_add(app_context_store(), KIND_STOPWATCH, 0, now - 300, &cookie);
  nav_go_hero(cookie);
  mark_scene(30, "stopwatch_laps_empty");
}

// Transient short-Up-press hint ("Hold to add lap") -- forced via
// screen_hero_debug_force_up_hint() rather than waiting out the real ~2s
// AppTimer.
static void scene_stopwatch_up_hint(void) {
  gallery_reset_fixture();
  int32_t cookie = stopwatch_5lap_fixture();
  nav_go_hero(cookie);
  screen_hero_debug_force_up_hint(true);
  screen_hero_mark_dirty();
  mark_scene(31, "stopwatch_up_hint");
}

// Select-hold = Pause mid-fill (pill label "Hold to pause"). Same
// hold_debug_force(600) forced-fraction technique as
// scene_countdown_hero_holdfill above; hold_debug_force_button defaults
// to SELECT (hold.h), so no override needed here.
static void scene_stopwatch_pause_holdfill(void) {
  gallery_reset_fixture();
  int32_t cookie = stopwatch_5lap_fixture();
  nav_go_hero(cookie);
  hold_debug_force(600);  // 60% -- per-mille (RENDER_FRACTION_SCALE), was 0.6f
  screen_hero_mark_dirty();
  mark_scene(32, "stopwatch_pause_holdfill");
}

// Up-hold = Add lap mid-fill (pill label swaps to "Hold for lap").
static void scene_stopwatch_lap_holdfill(void) {
  gallery_reset_fixture();
  int32_t cookie = stopwatch_5lap_fixture();
  nav_go_hero(cookie);
  hold_debug_force_button(BUTTON_ID_UP);
  hold_debug_force(600);  // 60% -- per-mille (RENDER_FRACTION_SCALE), was 0.6f
  screen_hero_mark_dirty();
  mark_scene(33, "stopwatch_lap_holdfill");
}

// --- PAUSED-STOPWATCH --------------------------------------------------------
//
// The shipped combined treatment: frozen value drawn muted + a "Paused" chip
// + a LIVE "paused M:SS" counter replacing the meta line's "since HH:MM"
// (pause-elapsed = now - paused_at). Reuses the same 5-lap fixture, then
// pauses the timer via the REAL timer_pause() mutation (never a hand-set
// is_paused field) so the is_paused-driven parts of the rendering are the
// SAME real code path a real Actions->Pause already exercises today.
static void scene_stopwatch_paused(void) {
  gallery_reset_fixture();
  int32_t cookie = stopwatch_5lap_fixture();
  Timer *t = store_find(app_context_store(), cookie);
  timer_pause(t, time(NULL));
  nav_go_hero(cookie);
  mark_scene(39, "stopwatch_paused");
}

// --- CLEAR-ALL + MAX-REACHED -------------------------------------------------
//
// The shipped shape combines a dedicated "Clear all" row with a confirm
// screen (deliberately NOT a hold gesture -- a destructive action must
// never fire off a bare click on HOME without an explicit confirm step) --
// reached through the REAL
// screen_home.c row + the REAL screen_clear_confirm.c window
// (nav_go_clear_confirm), not a debug lever or a gallery-only window pushed
// outside nav.c. The max-reached state shows "Delete one to add more",
// also through the real `render_action_row_disabled` path (screen_home.c's
// draw_row). Every fixture below is real store_add()/store_clear() -- never
// a hand-set Timer/TimerStore field, per the gallery's fixture-purity rule.

// Shared 3-timer HOME fixture -- kind mix + a short countdown near the
// footer.
static void home_3timer_fixture(void) {
  time_t now = time(NULL);
  store_add(app_context_store(), KIND_COUNTDOWN, 372, now, NULL);     // 6:12
  store_add(app_context_store(), KIND_STOPWATCH, 0, now - 30, NULL);  // +0:30
  store_add(app_context_store(), KIND_COUNTDOWN, 108, now, NULL);    // 1:48
}

// The real "Clear all" row, cursor on it (row n+3: n=timers, n=New timer,
// n+1=Say a timer, n+2=Library, n+3=Clear all -- this index shifts whenever
// a row gets added ahead of it, so it's worth double-checking it against
// screen_home.c's row model if this ever renders on the wrong row).
static void scene_home_clearall_row(void) {
  gallery_reset_fixture();
  home_3timer_fixture();
  nav_go_home();
  screen_home_debug_refresh();
  // 3 timers + New timer(3) + Say a timer(4) + Library(5) + Clear all(6).
  gallery_refresh_menu(screen_home_debug_menu(), 6);
  mark_scene(34, "home_clearall_row");
}

// The real confirm screen (screen_clear_confirm.c), reached via the real
// nav_go_clear_confirm() route, not a gallery-only window.
static void scene_home_clearall_confirm(void) {
  gallery_reset_fixture();
  home_3timer_fixture();
  nav_go_clear_confirm();
  mark_scene(35, "home_clearall_confirm");
}

// 16 real store_add() calls (never a hand-set store->count) so the footer's
// real `store->count >= MAX_TIMERS` gate is genuinely true.
static void home_max_fixture(void) {
  time_t now = time(NULL);
  for (int i = 0; i < MAX_TIMERS; i++) {
    store_add(app_context_store(), KIND_COUNTDOWN, 60 + i * 30, now, NULL);
  }
}

// Highlighted: cursor on the footer row (index MAX_TIMERS) -- gray "Delete
// one to add more" pill.
static void scene_home_max_footer_highlighted(void) {
  gallery_reset_fixture();
  home_max_fixture();
  nav_go_home();
  screen_home_debug_refresh();
  gallery_refresh_menu(screen_home_debug_menu(), MAX_TIMERS);
  mark_scene(36, "home_max_footer_highlighted");
}

// Resting: cursor on the last TIMER row instead, so the footer renders its
// non-highlighted muted "All 16 timers in use" state.
static void scene_home_max_footer_resting(void) {
  gallery_reset_fixture();
  home_max_fixture();
  nav_go_home();
  screen_home_debug_refresh();
  gallery_refresh_menu(screen_home_debug_menu(), MAX_TIMERS - 1);
  mark_scene(37, "home_max_footer_resting");
}

// "Say a timer" at MAX_TIMERS mirrors "+ New timer"'s own disabled treatment
// exactly (see draw_row's own comment) -- cursor ON the voice row (index
// MAX_TIMERS + 1, view_home_voice_row(MAX_TIMERS)), showing the same "Delete
// one to add more" highlighted message the footer row above it shows.
static void scene_home_max_voice_highlighted(void) {
  gallery_reset_fixture();
  home_max_fixture();
  nav_go_home();
  screen_home_debug_refresh();
  gallery_refresh_menu(screen_home_debug_menu(), MAX_TIMERS + 1);
  mark_scene(66, "home_max_voice_highlighted");
}

// `gallery_refresh_menu`'s forced MenuRowAlignTop is not equivalent to real
// Down-button navigation at the n=2 exact-fit MenuLayer edge -- it can
// render a garbled quiet top row there as a harness-only artifact, not a
// real HOME rendering bug. This scene proves the equivalent on-screen state
// stays legible through the REAL production path instead: two timers,
// cursor moved via the SAME `gallery_refresh_menu` mechanism other
// cursor-position scenes here use (e.g. scene_home_scroll_cursor) -- if this
// harness artifact ever becomes visible here, it points at
// `gallery_refresh_menu`'s alignment forcing, not at HOME's real rendering.
static void scene_home_2timers_cursor_row1(void) {
  gallery_reset_fixture();
  time_t now = time(NULL);
  store_add(app_context_store(), KIND_COUNTDOWN, 58, now, NULL);   // 0:58
  store_add(app_context_store(), KIND_COUNTDOWN, 114, now, NULL);  // 1:54
  nav_go_home();
  screen_home_debug_refresh();
  gallery_refresh_menu(screen_home_debug_menu(), 1);  // cursor -> row1, row0 goes quiet
  mark_scene(38, "home_2timers_cursor_row1_legible");
}

// --- HOME + LIBRARY ----------------------------------------------------

// Every scene below drives the REAL screen_home.c/screen_library.c through
// the REAL nav_go_library()/preset_store_add() calls, never a hand-drawn
// fixture.

// Populated HOME with the real "Library" footer row, cursor on it (between
// "Say a timer" and "Clear all") -- shows the full 4-action-row footer,
// "+ New timer" / "Say a timer" / "Library" / "Clear all".
static void scene_home_with_library_footer(void) {
  gallery_reset_fixture();
  time_t now = time(NULL);
  store_add(app_context_store(), KIND_COUNTDOWN, 372, now, NULL);     // 6:12
  store_add(app_context_store(), KIND_STOPWATCH, 0, now - 30, NULL);  // +0:30
  nav_go_home();
  screen_home_debug_refresh();
  // 2 timers + New timer(2) + Say a timer(3) + Library(4).
  gallery_refresh_menu(screen_home_debug_menu(), 4);
  mark_scene(40, "home_with_library_footer");
}

// The real Library screen with a mixed countdown+repeat list, built via
// real preset_store_add() calls (never a hand-set PresetStore field).
static void scene_library_mixed_list(void) {
  gallery_reset_fixture();
  preset_store_add(app_context_presets(), KIND_COUNTDOWN, 180);  // 3:00
  preset_store_add(app_context_presets(), KIND_REPEAT, 1500);    // 25:00
  nav_go_library();
  mark_scene(41, "library_mixed_list");
}

// The real Library screen, empty (0 presets): the "No presets yet"
// hint + the lone "Save new" row.
static void scene_library_empty(void) {
  gallery_reset_fixture();
  nav_go_library();
  mark_scene(42, "library_empty");
}

// --- RUNNING-HERO UP-HOLD GESTURES ------------------------------------------
//
// Countdown's "add 1/4 of duration" (gated: 1/4 > 10s) and repeat's "edit
// interval" (ungated) Up-holds. Appended here rather than inserted near the
// existing countdown/repeat hero scenes above, to keep new scenes additive
// rather than renumbering everything after them.
//
// All four scenes reuse hold_debug_force()/hold_debug_force_button() (the
// same forced-fraction technique used elsewhere in this file) to depict a
// mid-hold footer as a static, reproducible screenshot -- no real timed
// button hold needed. Two are deliberately RESTING (no forced hold) to prove
// the gate/legend behavior on its own: the up-hold hint (stacked in the
// hold-footer pill's right slot, render_hold_footer's `up_label`) is
// present or absent purely from a real store_add()'d duration, no debug
// override involved for that part.

// Countdown Up-hold, RESTING, GATED ON (duration 20:00 -> 1/4 = 5:00,
// well past the 10s gate). Proves the pill's right slot shows the stacked
// "+5:00"/"more" legend even without holding anything -- the legend is a
// static function of duration_s, not of hold state.
static void scene_countdown_addquarter_resting(void) {
  gallery_reset_fixture();
  int32_t cookie;
  store_add(app_context_store(), KIND_COUNTDOWN, 1200, time(NULL) - 100, &cookie);  // 20:00, running
  nav_go_hero(cookie);
  mark_scene(43, "countdown_addquarter_resting");
}

// Countdown Up-hold, MID-HOLD, GATED ON -- same fixture as the resting
// variant above, Up forced held at 60% fill: the LEFT hold-action label
// swaps to the live "+5:00 -> 25:00" preview (hold-text-explains) while the
// right slot keeps showing the same static "+5:00"/"more" legend.
static void scene_countdown_addquarter_holdfill(void) {
  gallery_reset_fixture();
  int32_t cookie;
  store_add(app_context_store(), KIND_COUNTDOWN, 1200, time(NULL) - 100, &cookie);  // 20:00, running
  nav_go_hero(cookie);
  hold_debug_force_button(BUTTON_ID_UP);
  hold_debug_force(600);  // 60% -- per-mille (RENDER_FRACTION_SCALE), was 0.6f
  screen_hero_mark_dirty();
  mark_scene(44, "countdown_addquarter_holdfill");
}

// Countdown Up-hold, GATED OFF (duration 0:30 -> 1/4 = 7s, below the
// 10s gate). Below the gate, the gesture AND its hint are BOTH simply
// absent -- the resting frame alone shows this (no legend to force a
// hold to hide), but Up is additionally forced held here too, to show the
// LEFT label defensively falls back to the plain resting text even under a
// forced hold-active render state (screen_hero.c's `quarter_available`
// guard on the hold-text-explains branch, not just on whether the real
// click_config_provider ever binds Up raw for this duration).
static void scene_countdown_addquarter_gated_off(void) {
  gallery_reset_fixture();
  int32_t cookie;
  store_add(app_context_store(), KIND_COUNTDOWN, 30, time(NULL) - 5, &cookie);  // 0:30, well under the gate
  nav_go_hero(cookie);
  hold_debug_force_button(BUTTON_ID_UP);
  hold_debug_force(600);  // 60% -- per-mille (RENDER_FRACTION_SCALE), was 0.6f
  screen_hero_mark_dirty();
  mark_scene(45, "countdown_addquarter_gated_off");
}

// Repeat Up-hold, MID-HOLD: this gesture is "Restart" (edit-interval lives
// ACTIONS-only, screen_actions.c's REPEAT_ROW_EDIT_INTERVAL; see
// screen_hero.c's rp_restart). Same 3-cycle fixture as
// scene_repeat_hero_running, kept unchanged on purpose -- it's the
// PRE-commit hold-fill preview, so cycles=3 is still showing on screen
// (Restart's cycles=0 effect only lands once the hold actually commits, not
// shown mid-fill); Up forced held so the LEFT label reads "Restart" and the
// right slot shows "restart"/"more" stacked (repeat_restart_legend_label).
static void scene_repeat_hero_restart_holdfill(void) {
  gallery_reset_fixture();
  time_t now = time(NULL);
  int32_t cookie;
  store_add(app_context_store(), KIND_REPEAT, 300, now, &cookie);  // 5:00 interval
  Timer *t = store_find(app_context_store(), cookie);
  timer_advance_cycle(t, now);
  timer_advance_cycle(t, now);
  timer_advance_cycle(t, now - 8);  // cycles=3, matches #11's own recipe
  nav_go_hero(cookie);
  hold_debug_force_button(BUTTON_ID_UP);
  hold_debug_force(600);  // 60% -- per-mille (RENDER_FRACTION_SCALE), was 0.6f
  screen_hero_mark_dirty();
  mark_scene(46, "repeat_hero_restart_holdfill");
}

// --- PRESET ACTIONS SHEET + PRESET DIAL MODES ------------------------------
//
// Appended here rather than inserted near the HOME/Library scenes above, to
// keep new scenes additive rather than renumbering everything after them.

// The real {Edit, Rename, Delete} preset-actions sheet, reached the real way
// (hold-Select on a Library row -> nav_go_preset_actions(index)).
static void scene_preset_actions_sheet(void) {
  gallery_reset_fixture();
  preset_store_add(app_context_presets(), KIND_COUNTDOWN, 300);  // 5:00
  nav_go_preset_actions(0);
  mark_scene(47, "preset_actions_sheet");
}

// SET's PRESET_NEW mode ("SAVE PRESET" title, orange, no kind chip yet at
// 0:00), reached the real way (Library's "Save new" row ->
// nav_go_preset_new()). Deliberately NOT paired with screen_set_debug_dial()
// here -- that helper calls screen_set_reset() internally (CREATE mode),
// which would silently clobber the PRESET_NEW mode this scene exists to
// prove.
static void scene_set_save_preset(void) {
  gallery_reset_fixture();
  nav_go_preset_new();
  mark_scene(48, "set_save_preset");
}

// SET's PRESET_EDIT mode ("EDIT PRESET" title), prefilled from an
// EXISTING repeat preset (indigo dial, kind toggle already effective) --
// reached the real way (the actions sheet's Edit row ->
// nav_go_preset_edit(index)). screen_set_start_preset_edit primes the dial
// directly from the PresetStore (set_machine_init_from_seconds_kind), so no
// debug-dial call is needed or wanted here either.
static void scene_set_edit_preset(void) {
  gallery_reset_fixture();
  preset_store_add(app_context_presets(), KIND_REPEAT, 1500);  // 25:00
  nav_go_preset_edit(0);
  mark_scene(49, "set_edit_preset");
}

// --- Library/HOME "composed" leftovers --------------------------------------

// A 1-preset library, no scroll -- distinct from the 2-preset mixed list and
// the 0-preset empty state covered elsewhere.
static void scene_library_one_countdown(void) {
  gallery_reset_fixture();
  preset_store_add(app_context_presets(), KIND_COUNTDOWN, 300);  // 5:00
  nav_go_library();
  mark_scene(50, "library_one_countdown");
}

// A FULL library (MAX_PRESETS=12), cursor on the trailing "Save new" row
// (the last, bottom-most row) -- proves clean scroll-to-bottom
// (menu_layer_pad_bottom_enable(false) in screen_library.c, applies here
// exactly as it does on HOME). Alternates kind so the list isn't a visually
// uniform single-color column.
static void scene_library_full_scroll(void) {
  gallery_reset_fixture();
  for (int i = 0; i < MAX_PRESETS; i++) {
    preset_store_add(app_context_presets(), (i % 2 == 0) ? KIND_COUNTDOWN : KIND_REPEAT,
                      60 + i * 30);
  }
  nav_go_library();
  // Cursor -> row MAX_PRESETS (the trailing "Save new" row, one past the
  // last real preset) -- gallery_refresh_menu also re-syncs the MenuLayer's
  // own cached row count/selection, defensive against this scene landing
  // right after another Library-targeting scene, which nav_diff_stack could
  // treat as a no-op transition (the same class of staleness
  // home_refresh_appearance works around for HOME).
  gallery_refresh_menu(screen_library_debug_menu(), MAX_PRESETS);
  mark_scene(51, "library_full_scroll");
}

// --- VOICE -------------------------------------------------------------

// Every voice_confirm_*/voice_parse_fail scene below would ideally fixture
// its state through the REAL parse_nl() on a canned transcript, matching
// this file's own fixture-purity rule -- but lib/parse_nl.c (9748 B, the
// single largest translation unit in this app) is EXCLUDED from
// GALLERY_MODE builds (wscript's build()) because a gallery build would
// otherwise overflow the 65535-byte virtual_size wall the SDK's metadata
// packer imposes. screen_voice_show_parse() only READS the NlParse struct
// it's handed -- rendering is pixel-identical whether that struct came from
// a live parse or a literal one -- so each scene below instead constructs a
// literal `NlParse r = {...}` fixture. Every literal value was captured by
// actually running the real, unmodified parse_nl() on that exact transcript
// and cross-checked against its corresponding app/test/parse_nl_test.c
// assert_parse()/assert_clock_target() call (cited per scene below), so
// these fixtures track the parser's own test suite, not hand-guessed
// values. Parser correctness stays fully covered by that host test suite,
// completely independent of this build. Every voice_no_*/voice_disabled
// scene fixtures through the REAL voice_outcome_for_status(), unaffected by
// any of this (those scenes never called parse_nl() to begin with).
//
// Each scene calls the REAL nav_go_voice() (VD_VOICE is a real nav node)
// before overwriting the fixture with screen_voice_show_parse().
//
// screen_voice itself has no window_appear-driven refresh to race (unlike
// the MenuLayer-reload staleness gallery_refresh_menu works around
// elsewhere) -- screen_voice_show_parse() unconditionally reloads/redraws
// every sibling layer itself, so no extra refresh call is needed here.
//
// nav_go_voice() calls the REAL dictation_am_start() underneath.
// dictation_session_create() does NOT return NULL on this emulator -- the
// harness's own libpebble2/QemuTransport connection satisfies PebbleOS's
// "companion connected" check, so create() genuinely SUCCEEDS and
// dictation_session_start() genuinely begins a real session-setup handshake
// that nothing here ever answers (see nav.c's nav_go_voice() comment /
// dictation_am.c's try_create_session() comment for the full trace). That
// real, in-flight attempt is what each scene's own explicit
// dictation_am_cancel() call (immediately below, every scene) actually
// cancels -- it is NOT a no-op cleanup for an already-NULL session. Each
// scene's canned screen_voice_show_parse() call then overwrites the fixture
// regardless -- no scene's final pixels depend on dictation_am's transient
// intermediate state.

// Matches app/test/parse_nl_test.c:38's basic en N-U pair
// (assert_parse("five minutes", true, KIND_COUNTDOWN, 300, "")).
static void scene_voice_confirm_countdown(void) {
  gallery_reset_fixture();
  const char *transcript = "five minutes";
  NlParse r = {
      .kind = KIND_COUNTDOWN, .duration_s = 300, .has_duration = true, .ok = true,
  };
  nav_go_voice();
  // Cancel the real (genuine, not NULL-create) attempt nav_go_voice() just
  // kicked off -- consecutive voice scenes never navigate AWAY from
  // VD_VOICE in between, so render_stack()'s own structural per-transition
  // dictation_am_cancel() call never fires here (a no-op diff, zero stack
  // calls). Without this explicit cancel, each scene's firmware-level
  // session-setup handshake lingers and keeps retrying in the background
  // (~8s cadence) for the rest of the capture run, eventually interfering
  // with this same connection's own AppMessage testlog traffic -- a later
  // scene's own marker can stop arriving within its assertion timeout.
  dictation_am_cancel();
  screen_voice_show_parse(transcript, &r, voice_outcome_for_status(0, r.ok));
  mark_scene(52, "voice_confirm_countdown");
}

// es repeat-kind ("cada N U" template), proves the
// language-agnostic identity strip (indigo repeat glyph + "20:00" reads the
// same whether the transcript was es or en).
static void scene_voice_confirm_repeat_es(void) {
  gallery_reset_fixture();
  const char *transcript = "cada 20 minutos";
  // app/test/parse_nl_test.c:83, assert_parse(transcript, true, KIND_REPEAT, 1200, "").
  NlParse r = {
      .kind = KIND_REPEAT, .duration_s = 1200, .has_duration = true, .ok = true,
  };
  nav_go_voice();
  // Cancel the real attempt nav_go_voice() just kicked off -- see
  // scene_voice_confirm_countdown's own comment (first use, above) for why
  // this explicit call is needed on every voice scene.
  dictation_am_cancel();
  screen_voice_show_parse(transcript, &r, voice_outcome_for_status(0, r.ok));
  mark_scene(53, "voice_confirm_repeat_es");
}

// Stopwatch keyword alone (accented "cronómetro") -- no
// duration to show, and "Edit on dial" is correctly absent (build_rows'
// stopwatch branch).
static void scene_voice_confirm_stopwatch(void) {
  gallery_reset_fixture();
  const char *transcript = "cronómetro";
  // app/test/parse_nl_test.c:143, assert_parse(transcript, true, KIND_STOPWATCH, 0, "").
  NlParse r = {
      .kind = KIND_STOPWATCH, .duration_s = 0, .has_duration = false, .ok = true,
  };
  nav_go_voice();
  // Cancel the real attempt nav_go_voice() just kicked off -- see
  // scene_voice_confirm_countdown's own comment (first use, above) for why
  // this explicit call is needed on every voice scene.
  dictation_am_cancel();
  screen_voice_show_parse(transcript, &r, voice_outcome_for_status(0, r.ok));
  mark_scene(54, "voice_confirm_stopwatch");
}

// Compound duration sum ("N U [and] N U", e.g. "one minute thirty seconds").
static void scene_voice_confirm_compound(void) {
  gallery_reset_fixture();
  const char *transcript = "one minute thirty seconds";
  // app/test/parse_nl_test.c:150, assert_parse(transcript, true, KIND_COUNTDOWN, 90, "").
  NlParse r = {
      .kind = KIND_COUNTDOWN, .duration_s = 90, .has_duration = true, .ok = true,
  };
  nav_go_voice();
  // Cancel the real attempt nav_go_voice() just kicked off -- see
  // scene_voice_confirm_countdown's own comment (first use, above) for why
  // this explicit call is needed on every voice scene.
  dictation_am_cancel();
  screen_voice_show_parse(transcript, &r, voice_outcome_for_status(0, r.ok));
  mark_scene(55, "voice_confirm_compound");
}

// Name-residue parse ("pasta for eight minutes" -> name="pasta"). Proves the
// name genuinely RENDERS -- its own bold line above the "heard: ..." echo,
// and the action row reading "Rename" instead of "Add name" (has_name ->
// "Rename", since this transcript already carries a name inline).
static void scene_voice_confirm_named(void) {
  gallery_reset_fixture();
  const char *transcript = "pasta for eight minutes";
  // app/test/parse_nl_test.c:161, assert_parse(transcript, true, KIND_COUNTDOWN, 480, "pasta").
  NlParse r = {
      .kind = KIND_COUNTDOWN, .duration_s = 480, .has_duration = true, .ok = true,
      .name = "pasta", .has_name = true,
  };
  nav_go_voice();
  // Cancel the real attempt nav_go_voice() just kicked off -- see
  // scene_voice_confirm_countdown's own comment (first use, above) for why
  // this explicit call is needed on every voice scene.
  dictation_am_cancel();
  screen_voice_show_parse(transcript, &r, voice_outcome_for_status(0, r.ok));
  mark_scene(56, "voice_confirm_named");
}

// A free-form sentence with no N-U pair and no kind
// keyword -- parse_nl's fallback (ok=false). Renders the "Didn't catch a
// time" neutral-gray header + the verbatim echo (the whole point -- the user
// sees why it missed) + Set on dial / Say again.
static void scene_voice_parse_fail(void) {
  gallery_reset_fixture();
  const char *transcript = "set a timer for when the pasta is done";
  // Real parse_nl() output for this transcript (no KIND_COUNTDOWN token,
  // no N-U pair -- everything falls into the name residue, ok=false):
  // kind=KIND_COUNTDOWN(default) duration_s=0 has_duration=false
  // name="when the pasta is done" has_name=true ok=false.
  NlParse r = {
      .kind = KIND_COUNTDOWN, .duration_s = 0, .has_duration = false, .ok = false,
      .name = "when the pasta is done", .has_name = true,
  };
  nav_go_voice();
  // Cancel the real attempt nav_go_voice() just kicked off -- see
  // scene_voice_confirm_countdown's own comment (first use, above) for why
  // this explicit call is needed on every voice scene.
  dictation_am_cancel();
  screen_voice_show_parse(transcript, &r, voice_outcome_for_status(0, r.ok));
  mark_scene(57, "voice_parse_fail");
}

// VOICE_NO_PHONE's rendering, fixtured via the canned
// VOICE_STATUS_CREATE_FAILED sentinel below -- NOT a live-reproduced
// dictation_session_create()==NULL condition: create() genuinely succeeds
// on this emulator (the harness's own libpebble2/QemuTransport connection
// satisfies PebbleOS's "companion connected" check), so a real
// create()==NULL VOICE_NO_PHONE is a REAL DEVICE's own no-phone-paired case,
// not something this emulator can produce live. This scene still proves the
// outcome's pixels correctly, through the real voice_outcome_for_status()
// mapping, same as every other voice_no_*/voice_disabled scene here.
static void scene_voice_no_phone(void) {
  gallery_reset_fixture();
  NlParse empty = {0};
  nav_go_voice();
  // Cancel the real attempt nav_go_voice() just kicked off -- see
  // scene_voice_confirm_countdown's own comment (first use, above) for why
  // this explicit call is needed on every voice scene.
  dictation_am_cancel();
  screen_voice_show_parse(NULL, &empty, voice_outcome_for_status(VOICE_STATUS_CREATE_FAILED, false));
  mark_scene(58, "voice_no_phone");
}

// DictationSessionStatusNoSpeechDetected (4).
static void scene_voice_no_speech(void) {
  gallery_reset_fixture();
  NlParse empty = {0};
  nav_go_voice();
  // Cancel the real attempt nav_go_voice() just kicked off -- see
  // scene_voice_confirm_countdown's own comment (first use, above) for why
  // this explicit call is needed on every voice scene.
  dictation_am_cancel();
  screen_voice_show_parse(NULL, &empty, voice_outcome_for_status(4, false));
  mark_scene(59, "voice_no_speech");
}

// DictationSessionStatusDisabled (6).
static void scene_voice_disabled(void) {
  gallery_reset_fixture();
  NlParse empty = {0};
  nav_go_voice();
  // Cancel the real attempt nav_go_voice() just kicked off -- see
  // scene_voice_confirm_countdown's own comment (first use, above) for why
  // this explicit call is needed on every voice scene.
  dictation_am_cancel();
  screen_voice_show_parse(NULL, &empty, voice_outcome_for_status(6, false));
  mark_scene(60, "voice_disabled");
}

// "Edit on dial" reuse proof: calls the REAL nav_go_set_from_voice()
// production entry point directly -- this exercises the actual shipped
// priming path (nav_go_set_from_voice -> screen_set_start_from_voice ->
// set_machine_init_from_seconds_kind), the same one screen_voice.c's real
// "Edit on dial" row calls. Still not screen_voice.c's own click handler
// (GALLERY_MODE swaps every real screen's click_config_provider for its own,
// so no screen's real Select/Up/Down/Back handler ever runs inside the
// gallery tour, structurally, for any screen -- see screen_home.c's own
// GALLERY_MODE comment) -- but this is a direct call to the exact function
// that handler invokes, not a visually-matching but unrelated debug lever.
static void scene_voice_edit_prefilled(void) {
  gallery_reset_fixture();
  nav_go_set_from_voice(1200, true);  // 20:00 repeat -- fixed value for a reproducible screenshot
  mark_scene(61, "voice_edit_prefilled");
}

// Populated HOME, cursor ON "Say a timer" (row n+1) -- the
// highlighted/selected pill state of that row, between "+ New timer" and
// "Library". Same 2-timer fixture as scene_home_with_library_footer so the
// two scenes are directly comparable crops of the same footer block.
static void scene_home_voice_row_populated(void) {
  gallery_reset_fixture();
  time_t now = time(NULL);
  store_add(app_context_store(), KIND_COUNTDOWN, 372, now, NULL);     // 6:12
  store_add(app_context_store(), KIND_STOPWATCH, 0, now - 30, NULL);  // +0:30
  nav_go_home();
  screen_home_debug_refresh();
  gallery_refresh_menu(screen_home_debug_menu(), 3);  // 2 timers + New timer(2) + Say a timer(3)
  mark_scene(62, "home_voice_row_populated");
}

// Empty HOME, cursor ON "Say a timer" (row 1, between the "+ New timer"
// hero and the composed "Library" row) -- the "two-choice hero moment"
// pairing, the empty-state counterpart to the scene above.
static void scene_home_empty_voice_row(void) {
  gallery_reset_fixture();
  nav_go_home();
  screen_home_debug_refresh();
  gallery_refresh_menu(screen_home_debug_menu(), 1);
  mark_scene(63, "home_empty_voice_row");
}

// The named HOME-row scene -- proves both halves of the render flip in one
// shot: (a) a plain named countdown row (glyph . name . duration, the
// identity-preserving label-slot flip) coexisting with a name-less
// stopwatch row (named and name-less coexist coherently), and (b) the
// "snoozing" precedence rule (screen_home.c's draw_row: `timer_is_snoozing(t) ?
// "snoozing" : view_row_label(t->name)`) -- the third timer is BOTH named
// ("Focus") AND mid-snooze, and must show "snoozing", never "Focus". All
// three built through the REAL store_add/store_set_name/timer_snooze_repeat
// mutations (this file's own fixture-purity rule), never a hand-set
// Timer.name field.
static void scene_home_named(void) {
  gallery_reset_fixture();
  time_t now = time(NULL);

  int32_t tea_cookie;
  store_add(app_context_store(), KIND_COUNTDOWN, 300, now - 1, &tea_cookie);  // 4:59
  store_set_name(app_context_store(), tea_cookie, "Tea");

  store_add(app_context_store(), KIND_STOPWATCH, 0, now - 30, NULL);  // +0:30, name-less

  int32_t focus_cookie;
  store_add(app_context_store(), KIND_REPEAT, 300, now, &focus_cookie);
  store_set_name(app_context_store(), focus_cookie, "Focus");
  timer_snooze_repeat(store_find(app_context_store(), focus_cookie), now);  // must show "snoozing", not "Focus"

  nav_go_home();
  screen_home_debug_refresh();
  mark_scene(64, "home_named");
}

// The Add-name state scene -- proves the un-gated "Add name" row +
// dictation_am's second-purpose (DICT_SCOPE_NAME) arc land on the SAME
// confirm preview, in place, via the real screen_voice_set_name() production
// seam (never a hand-set NlParse field) -- simulating exactly what
// dictation_am.c's deliver() calls on a successful NAME-scope re-dictation
// (the transcript taken VERBATIM, no parse_nl pass). Starts from a
// NAME-LESS parse ("five minutes") so the row-label flip is visible in the
// screenshot set: "Add name" -> "Rename" + a name line (this scene), same
// underlying confirm preview.
static void scene_voice_add_name_result(void) {
  gallery_reset_fixture();
  const char *transcript = "five minutes";
  // Same fixture as scene_voice_confirm_countdown -- see its comment
  // (app/test/parse_nl_test.c:38) for the citation.
  NlParse r = {
      .kind = KIND_COUNTDOWN, .duration_s = 300, .has_duration = true, .ok = true,
  };
  nav_go_voice();
  // Cancel the real attempt nav_go_voice() just kicked off -- see
  // scene_voice_confirm_countdown's own comment (first use, above) for why
  // this explicit call is needed on every voice scene.
  dictation_am_cancel();
  screen_voice_show_parse(transcript, &r, voice_outcome_for_status(0, r.ok));
  screen_voice_set_name("Pomodoro");  // the real Add-name arc's own seam
  mark_scene(65, "voice_add_name_result");
}

// --- Named presets -----------------------------------------------------

// A Library list mixing named and name-less rows of BOTH kinds, so the
// degrade rule (a name-less row renders BYTE-IDENTICAL to the earlier
// left-aligned-duration+chevron layout) and the named-row layout (HOME's own
// label-left/number-right treatment, reused verbatim -- no new geometry) sit
// side by side for a direct pixel comparison. Built entirely through the
// real preset_store_add/preset_store_set_name mutations (never a hand-set
// Preset.name field).
static void scene_library_named_mixed(void) {
  gallery_reset_fixture();
  preset_store_add(app_context_presets(), KIND_COUNTDOWN, 180);  // 3:00
  preset_store_set_name(app_context_presets(), 0, "Tea");
  preset_store_add(app_context_presets(), KIND_REPEAT, 1500);  // 25:00, name-less (degrade rule)
  preset_store_add(app_context_presets(), KIND_COUNTDOWN, 300);  // 5:00
  preset_store_set_name(app_context_presets(), 2, "Pomodoro");
  nav_go_library();
  mark_scene(67, "library_named_mixed");
}

// The Rename arc's end state, proven the same way the Add-name scene above
// was: reach the real {Edit, Rename, Delete} sheet via real navigation,
// apply a name through the REAL production seam
// (screen_preset_actions_apply_name -- exactly what dictation_am.c's
// deliver() calls on a successful DICT_SCOPE_PRESET_NAME re-dictation, never
// a hand-set Preset.name), then return to Library the same way the sheet's
// own Delete/Edit paths already do, so the screenshot shows the renamed row
// live in place. No dictation_am_cancel() needed here (unlike the voice
// scenes above) -- nav_go_preset_actions() itself never starts a live
// dictation attempt; only an explicit Rename row select does, and this scene
// calls the apply seam directly instead of going through a real Select
// click. The REAL mic round-trip is device-only (dictation never resolves
// live on this emulator) -- this scene is the sanctioned real-stack proof of
// the render+persist+refresh path, not a substitute for that device-only
// round-trip.
static void scene_preset_rename_result(void) {
  gallery_reset_fixture();
  preset_store_add(app_context_presets(), KIND_COUNTDOWN, 300);  // 5:00, name-less
  nav_go_preset_actions(0);
  screen_preset_actions_apply_name("Tea");
  nav_go_library();
  mark_scene(68, "preset_rename_result");
}

// --- Cursor-row contrast regression coverage --------------------------------

// A LONE stopwatch at HOME row 0. Pebble's MenuLayer highlights row 0 by
// default with zero button presses, so this is the exact zero-navigation
// repro for a cursor-row/black-pill contrast defect: a darkened
// `value_color` tier is safe on a WHITE row background but, by the same
// symmetric WCAG logic, LESS safe on the cursor row's own BLACK pill -- for
// stopwatch specifically this dropped from TiffanyBlue's 7.33:1 to
// MidnightGreen's 2.43:1 against black, failing the 3:1 large-text floor
// outright. Permanent regression coverage for this exact defect class, now
// that render_row()'s cursor-row number color reverts to the bright
// spec->kind_color (render.c's own fix).
static void scene_home_stopwatch_cursor(void) {
  gallery_reset_fixture();
  store_add(app_context_store(), KIND_STOPWATCH, 0, time(NULL) - 1, NULL);  // +0:01
  nav_go_home();
  screen_home_debug_refresh();
  mark_scene(69, "home_stopwatch_cursor");
}

// "seis cuarenta y cinco", a unit-less spoken M:SS pair -- resolves via the
// pair rule to 6:45 (405s) instead of the earlier fail-soft. Added as its
// OWN new scene (not a replacement of an existing one) so earlier scenes'
// pixels stay the drift-check baseline.
static void scene_voice_confirm_pair_es(void) {
  gallery_reset_fixture();
  const char *transcript = "seis cuarenta y cinco";
  // app/test/parse_nl_test.c:733, assert_parse(transcript, true, KIND_COUNTDOWN, 405, "").
  NlParse r = {
      .kind = KIND_COUNTDOWN, .duration_s = 405, .has_duration = true, .ok = true,
  };
  nav_go_voice();
  // Cancel the real attempt nav_go_voice() just kicked off -- see
  // scene_voice_confirm_countdown's own comment (first use, above) for why
  // this explicit call is needed on every voice scene.
  dictation_am_cancel();
  screen_voice_show_parse(transcript, &r, voice_outcome_for_status(0, r.ok));
  mark_scene(70, "voice_confirm_pair_es");
}

// The fractions engine, one representative pattern-2 form ("hora y media",
// implicit N=1, trailing "y media") -- a literal NlParse fixture (parse_nl.c
// is excluded from gallery builds, see this file's VOICE section header)
// whose values match what the real parser resolves it to, 1:30:00 (5400s):
// 1:30:00 reads unambiguously as the intended "an hour and a half" on the
// real confirm screen. Added as its OWN new scene (not a replacement) so
// earlier scenes' pixels stay the drift-check baseline.
static void scene_voice_confirm_fraction_es(void) {
  gallery_reset_fixture();
  const char *transcript = "hora y media";
  // app/test/parse_nl_test.c:977, assert_parse(transcript, true, KIND_COUNTDOWN, 5400, "").
  NlParse r = {
      .kind = KIND_COUNTDOWN, .duration_s = 5400, .has_duration = true, .ok = true,
  };
  nav_go_voice();
  // Cancel the real attempt nav_go_voice() just kicked off -- see
  // scene_voice_confirm_countdown's own comment (first use, above) for why
  // this explicit call is needed on every voice scene.
  dictation_am_cancel();
  screen_voice_show_parse(transcript, &r, voice_outcome_for_status(0, r.ok));
  mark_scene(71, "voice_confirm_fraction_es");
}

// --- Additional coverage scenes ---------------------------------------------

// A maximal-length (PRESET_NAME_MAX-1 = 15 chars) preset name in Library --
// this render-fidelity risk had previously only ever been exercised with
// short names ("Tea"/"Pomodoro"/"Focus", none over 9 characters). Paired
// with a name-less repeat preset so the boundary case sits next to the
// already-proven degrade-rule pixels for direct comparison.
static void scene_library_max_name(void) {
  gallery_reset_fixture();
  preset_store_add(app_context_presets(), KIND_COUNTDOWN, 300);  // 5:00
  preset_store_set_name(app_context_presets(), 0, "Weekly Standup!");  // 15 chars = PRESET_NAME_MAX-1
  preset_store_add(app_context_presets(), KIND_REPEAT, 1500);  // 25:00, name-less (degrade rule)
  nav_go_library();
  mark_scene(72, "library_max_name");
}

// Cursor on a KIND_REPEAT row at HOME. No prior HOME scene put the cursor
// there (scene_home_repeat_snoozing's repeat sibling always sorts after a
// shorter-remaining countdown -- store_home_order() sorts running timers by
// remaining time regardless of kind -- landing at row 1, never selected).
// Structurally low-risk (Indigo is a no-op for the bright/deep accent
// split), but a genuinely uncaptured combo until now.
static void scene_home_repeat_cursor(void) {
  gallery_reset_fixture();
  time_t now = time(NULL);
  store_add(app_context_store(), KIND_COUNTDOWN, 60, now, NULL);  // 1:00, sorts first -> row 0
  store_add(app_context_store(), KIND_REPEAT, 300, now, NULL);    // 5:00, sorts second -> row 1
  nav_go_home();
  screen_home_debug_refresh();
  gallery_refresh_menu(screen_home_debug_menu(), 1);  // cursor -> row1, the repeat timer
  mark_scene(73, "home_repeat_cursor");
}

// Cursor on a KIND_REPEAT row in Library. scene_library_mixed_list has a
// repeat preset at row 1 but never moves the cursor there; this scene is
// that same combo with the cursor explicitly placed on it.
static void scene_library_repeat_cursor(void) {
  gallery_reset_fixture();
  preset_store_add(app_context_presets(), KIND_COUNTDOWN, 180);  // 3:00, row 0
  preset_store_add(app_context_presets(), KIND_REPEAT, 1500);    // 25:00, row 1
  nav_go_library();
  gallery_refresh_menu(screen_library_debug_menu(), 1);  // cursor -> row1, the repeat preset
  mark_scene(74, "library_repeat_cursor");
}

// --- Voice-into-Library ------------------------------------------------

// Populated Library, cursor ON "Say a preset" (row count+1, between
// "Save new" and off the end of the list) -- the highlighted/selected pill
// state of that row, mirroring scene_home_voice_row_populated's own shot for
// HOME's own voice row.
static void scene_library_voice_row_populated(void) {
  gallery_reset_fixture();
  preset_store_add(app_context_presets(), KIND_COUNTDOWN, 180);  // 3:00, row 0
  preset_store_add(app_context_presets(), KIND_REPEAT, 1500);    // 25:00, row 1
  nav_go_library();
  gallery_refresh_menu(screen_library_debug_menu(), 3);  // 2 presets + Save new(2) + Say a preset(3)
  mark_scene(75, "library_voice_row_populated");
}

// EMPTY Library, cursor ON "Say a preset" (row 1, between the fixed-height
// hero and off the end) -- the empty-state counterpart to the scene above,
// mirroring scene_home_empty_voice_row's own shot. Also the render-fidelity
// check for the empty-hero layout (LIBRARY_EMPTY_HERO_H, dropped "No
// presets yet" hint) -- row 0's hero sits ABOVE the fold here too, so both
// the hero's own geometry and its sibling row are visible in one
// screenshot.
static void scene_library_empty_voice_row(void) {
  gallery_reset_fixture();
  nav_go_library();
  gallery_refresh_menu(screen_library_debug_menu(), 1);
  mark_scene(76, "library_empty_voice_row");
}

// Library at MAX_PRESETS (12), cursor ON "Say a preset" -- the direct pixel
// proof that "Say a preset" is deliberately UNGATED at MAX_PRESETS
// (screen_library.c's draw_row comment on the `row > count` branch),
// mirroring "Save new"'s own precedent rather than HOME's MAX_TIMERS-gated
// "Say a timer" row. Reuses scene_library_full_scroll's own MAX-preset
// fixture recipe.
static void scene_library_max_presets_voice_row(void) {
  gallery_reset_fixture();
  for (int i = 0; i < MAX_PRESETS; i++) {
    preset_store_add(app_context_presets(), (i % 2 == 0) ? KIND_COUNTDOWN : KIND_REPEAT,
                      60 + i * 30);
  }
  nav_go_library();
  // Not gallery_refresh_menu() (always MenuRowAlignTop): the LAST row of an
  // overflowing list can't satisfy a top-align (there is no content below
  // it to fill the resulting gap), which silently leaves the
  // scroll/selection at whatever a PRIOR scene left behind instead of
  // erroring. MenuRowAlignBottom is the one alignment always satisfiable
  // for the trailing row.
  MenuLayer *menu = screen_library_debug_menu();
  menu_layer_reload_data(menu);
  menu_layer_set_selected_index(menu, MenuIndex(0, MAX_PRESETS + 1), MenuRowAlignBottom, false);
  layer_mark_dirty(menu_layer_get_layer(menu));
  mark_scene(77, "library_max_presets_voice_row");
}

// The preset-target confirm screen: a literal NlParse fixture (see body;
// parse_nl.c is excluded from gallery builds) whose values match the real
// parser's output, driven through the REAL nav_go_voice_preset() entry
// point (screen_voice.c's target-aware screen_voice_set_target
// (VOICE_TARGET_PRESET), exercised through production code, never a debug
// lever) -- proves the primary action row reads "Save," not "Start"
// (build_rows' target-aware label), the one pixel difference this whole arc
// hinges on.
static void scene_voice_preset_confirm(void) {
  gallery_reset_fixture();
  const char *transcript = "fifteen minutes";
  // app/test/parse_nl_test.c:44, assert_parse(transcript, true, KIND_COUNTDOWN, 900, "").
  NlParse r = {
      .kind = KIND_COUNTDOWN, .duration_s = 900, .has_duration = true, .ok = true,
  };
  nav_go_voice_preset();
  // Cancel the real attempt nav_go_voice_preset() just kicked off -- see
  // scene_voice_confirm_countdown's own comment for why this explicit call
  // is needed on every voice scene.
  dictation_am_cancel();
  screen_voice_show_parse(transcript, &r, voice_outcome_for_status(0, r.ok));
  mark_scene(78, "voice_preset_confirm");
}

// The honest fail-soft: a preset-target session whose parse genuinely
// succeeded with kind==KIND_STOPWATCH (presets support countdown/repeat
// only) -- proves the specific "Presets can't be a stopwatch" message (NOT a
// dishonest reuse of "Didn't catch a time," which would misreport that
// nothing was heard) and the Say-again/Set-on-dial fail-soft row set, via
// voice_preset_kind_unsupported()'s real branch.
static void scene_voice_preset_stopwatch_unsupported(void) {
  gallery_reset_fixture();
  const char *transcript = "stopwatch";
  // app/test/parse_nl_test.c:139, assert_parse(transcript, true, KIND_STOPWATCH, 0, "").
  NlParse r = {
      .kind = KIND_STOPWATCH, .duration_s = 0, .has_duration = false, .ok = true,
  };
  nav_go_voice_preset();
  dictation_am_cancel();
  screen_voice_show_parse(transcript, &r, voice_outcome_for_status(0, r.ok));
  mark_scene(79, "voice_preset_stopwatch_unsupported");
}

// The Save arc's END STATE: proven the same way earlier Add-name and Rename
// scenes above were -- compose the exact same production primitives
// do_save() itself calls (preset_store_add,
// preset_store_set_name, nav_go_library_focus_row -- do_save() itself is
// screen_voice.c-static and unreachable from here, per this app's
// "GALLERY_MODE bypasses every real click handler" convention, so this scene
// calls its own real building blocks directly instead of faking the outcome)
// -- proving the cursor-restore principle: the new named preset's OWN row
// is visible AND selected on return to Library, not row 0.
static void scene_voice_preset_save_result(void) {
  gallery_reset_fixture();
  preset_store_add(app_context_presets(), KIND_COUNTDOWN, 180);  // an existing preset, row 0
  int new_index = 1;
  preset_store_add(app_context_presets(), KIND_REPEAT, 900);  // the "saved" preset, row 1
  preset_store_set_name(app_context_presets(), new_index, "Tea");
  nav_go_library_focus_row(new_index);
  mark_scene(80, "voice_preset_save_result");
}

// The Library discoverability hint: forces the transient "hold a preset for
// options" state via the GALLERY_MODE debug lever
// (screen_library_debug_force_hint(), mirroring
// screen_hero_debug_force_up_hint()'s own contract) so this reads
// deterministically instead of racing a real 2.2s AppTimer mid-capture.
static void scene_library_hint_active(void) {
  gallery_reset_fixture();
  preset_store_add(app_context_presets(), KIND_COUNTDOWN, 180);  // >=1 preset -- the hint's own gate
  nav_go_library();
  screen_library_debug_force_hint(true);
  mark_scene(81, "library_hint_active");
}

// --- ACTIONS Rename for running timers ---------------------------------

// Countdown ACTIONS with the Rename row (above the destructive Delete,
// mirroring the preset-actions sheet's own {Edit, Rename, Delete}
// placement) -- cursor placed ON Rename so it's unambiguously in frame,
// same "cursor on the new row" convention other new-row scenes above use.
static void scene_actions_countdown_with_rename(void) {
  gallery_reset_fixture();
  int32_t cookie;
  store_add(app_context_store(), KIND_COUNTDOWN, 600, time(NULL) - 442, &cookie);
  nav_go_actions(cookie);
  gallery_refresh_menu(screen_actions_debug_menu(), 4);  // ROW_RENAME
  mark_scene(82, "actions_countdown_with_rename");
}

// The Rename arc's END STATE on HOME: proven the same way
// scene_preset_rename_result proved the Library Rename arc -- reach ACTIONS
// via real navigation, apply a name through the REAL production seam
// (screen_actions_apply_name -- exactly what dictation_am.c's deliver() calls
// on a successful DICT_SCOPE_ACTIONS_NAME re-dictation), then return to HOME
// to show the renamed row live -- screen_home.c's existing named-row render
// path, unchanged, confirmed here on real pixels.
static void scene_actions_rename_result(void) {
  gallery_reset_fixture();
  int32_t cookie;
  store_add(app_context_store(), KIND_COUNTDOWN, 300, time(NULL), &cookie);  // 5:00, name-less
  nav_go_actions(cookie);
  screen_actions_apply_name("Focus");
  nav_go_home();
  screen_home_debug_refresh();
  mark_scene(83, "actions_rename_result");
}

// The preset-target "Edit on dial" reuse proof, mirroring
// scene_voice_edit_prefilled's own shape one-for-one: calls the REAL
// production entry point nav_go_preset_new_from_voice() directly
// (GALLERY_MODE bypasses every real click handler, so this is the exact
// function screen_voice.c's own target-aware VOICE_ACTION_EDIT_DIAL case
// calls when s_target == VOICE_TARGET_PRESET, not a debug lever) -- proves
// SET lands in SET_MODE_PRESET_NEW ("SAVE PRESET" title, screen_set.c's
// set_mode_title), primed from the parsed {duration_s, is_repeat}, not
// SET_MODE_CREATE ("NEW TIMER").
static void scene_voice_preset_edit_prefilled(void) {
  gallery_reset_fixture();
  nav_go_preset_new_from_voice(1200, true);  // 20:00 repeat -- same seed as scene_voice_edit_prefilled
  mark_scene(84, "voice_preset_edit_prefilled");
}

// A real-world case: "6 minutos 45" was creating a 6:00 timer NAMED "45"
// instead of 6:45 -- resolves via the generalized unit+trailing-sub-count
// rule (the hour rule's own structural extension, one level down) to 6:45
// (405s), AND the leftover-that-would-have-been-a-name is correctly absent
// ("Add name", not "Rename"/a name line) because the trailing "45" is fully
// consumed into the duration, never reaching the residue at all. Added as
// its OWN new scene (not a replacement) so earlier scenes' pixels stay the
// drift-check baseline.
static void scene_voice_confirm_minutes_trailing_es(void) {
  gallery_reset_fixture();
  const char *transcript = "6 minutos 45";
  // app/test/parse_nl_test.c:850, assert_parse(transcript, true, KIND_COUNTDOWN, 405, "").
  NlParse r = {
      .kind = KIND_COUNTDOWN, .duration_s = 405, .has_duration = true, .ok = true,
  };
  nav_go_voice();
  // Cancel the real attempt nav_go_voice() just kicked off -- see
  // scene_voice_confirm_countdown's own comment (first use, above) for why
  // this explicit call is needed on every voice scene.
  dictation_am_cancel();
  screen_voice_show_parse(transcript, &r, voice_outcome_for_status(0, r.ok));
  mark_scene(85, "voice_confirm_minutes_trailing_es");
}

// All clock-target scenes below show a target resolved against this FIXED
// "now" -- 09:00:00 local -- chosen so a spoken 12h "six" target resolves to
// its PM occurrence (18:xx, ~9h away), demonstrating the nearest-future
// AM/PM resolution on real pixels, not just the host math table. These
// scenes would ideally call the real nl_clock_target_to_duration() against
// this constant at capture time (exactly the call dictation_am.c's own
// deliver() makes, with `now` fixed instead of a real device's time(NULL)
// read, so the gallery stays deterministic); since lib/parse_nl.c is
// excluded from gallery builds, each scene's duration_s/clock_target_sod are
// instead the literal result of that SAME resolution, precomputed once (see
// each scene's own comment) -- kept here as the documented basis for those
// literals, and to recompute them by hand if the clock-target math ever
// changes.
#define GALLERY_CLOCK_TARGET_FIXED_NOW_SOD (9 * 3600)

// en clock-target confirm: "at six forty five" -- a bare 12h "six"
// (hour24_known=false) at the fixed 09:00 "now" resolves to its PM
// occurrence (18:45, ~9h45m away), proving both the duration AND the "->
// 18:45" target annotation render correctly for a resolved clock target.
// (The fixture is a literal NlParse -- captured from a real parse_nl() +
// nl_clock_target_to_duration() run, see the scene body's own comment --
// rather than a live in-scene call.)
static void scene_voice_confirm_clock_target_en(void) {
  gallery_reset_fixture();
  const char *transcript = "at six forty five";
  // app/test/parse_nl_test.c:1379, assert_clock_target(transcript, true, 6, 45, false);
  // duration_s/clock_target_sod are the real nl_clock_target_to_duration()
  // result for that clock target resolved against
  // GALLERY_CLOCK_TARGET_FIXED_NOW_SOD (09:00) -- 6:45 12h-ambiguous resolves
  // to its nearest-future PM occurrence, 18:45 (67500s-of-day), 35100s away.
  NlParse r = {
      .kind = KIND_COUNTDOWN, .duration_s = 35100, .has_duration = true, .ok = true,
      .clock = {.has_clock_target = true, .hour = 6, .minute = 45, .hour24_known = false},
      .clock_target_sod = 67500,
  };
  nav_go_voice();
  // Cancel the real attempt nav_go_voice() just kicked off -- see
  // scene_voice_confirm_countdown's own comment (first use, above) for why
  // this explicit call is needed on every voice scene.
  dictation_am_cancel();
  screen_voice_show_parse(transcript, &r, voice_outcome_for_status(0, r.ok));
  mark_scene(86, "voice_confirm_clock_target_en");
}

// es clock-target confirm: "a las seis y media" -- proves the SAME
// discriminator + sub-grammar + annotation for the Spanish "a las ... y
// media" form (6:30, PM occurrence at the same fixed 09:00 "now").
static void scene_voice_confirm_clock_target_es(void) {
  gallery_reset_fixture();
  const char *transcript = "a las seis y media";
  // app/test/parse_nl_test.c:1386, assert_clock_target(transcript, true, 6, 30, false);
  // duration_s/clock_target_sod: real nl_clock_target_to_duration() result
  // against the fixed 09:00 "now" -- 6:30 12h-ambiguous resolves to its
  // nearest-future PM occurrence, 18:30 (66600s-of-day), 34200s away.
  NlParse r = {
      .kind = KIND_COUNTDOWN, .duration_s = 34200, .has_duration = true, .ok = true,
      .clock = {.has_clock_target = true, .hour = 6, .minute = 30, .hour24_known = false},
      .clock_target_sod = 66600,
  };
  nav_go_voice();
  // Cancel the real attempt nav_go_voice() just kicked off -- see
  // scene_voice_confirm_countdown's own comment (first use, above) for why
  // this explicit call is needed on every voice scene.
  dictation_am_cancel();
  screen_voice_show_parse(transcript, &r, voice_outcome_for_status(0, r.ok));
  mark_scene(87, "voice_confirm_clock_target_es");
}

// The SAME "at six forty five" parse as the en clock-target scene above,
// forced into 12h clock-style via screen_voice_debug_force_24h_style(0)
// (real emulators/devices don't expose an in-app way to flip this, so this
// is a gallery-only lever -- see the lever's own doc comment in
// screen_voice.h) -- proves the annotation reads "-> 6:45 pm" (not a bare
// "-> 6:45" indistinguishable from an AM target) on the ONE device style
// where the en/es scenes above (both rendered in this emulator's 24h style)
// couldn't demonstrate it at all.
static void scene_voice_confirm_clock_target_12h(void) {
  gallery_reset_fixture();
  screen_voice_debug_force_24h_style(0);
  const char *transcript = "at six forty five";
  // Same fixture as scene_voice_confirm_clock_target_en -- see its
  // comment (app/test/parse_nl_test.c:1379) for the citation; only
  // the forced 12h device style differs here.
  NlParse r = {
      .kind = KIND_COUNTDOWN, .duration_s = 35100, .has_duration = true, .ok = true,
      .clock = {.has_clock_target = true, .hour = 6, .minute = 45, .hour24_known = false},
      .clock_target_sod = 67500,
  };
  nav_go_voice();
  // Cancel the real attempt nav_go_voice() just kicked off -- see
  // scene_voice_confirm_countdown's own comment (first use, above) for why
  // this explicit call is needed on every voice scene.
  dictation_am_cancel();
  screen_voice_show_parse(transcript, &r, voice_outcome_for_status(0, r.ok));
  mark_scene(88, "voice_confirm_clock_target_12h");
}

// "a las 18:30", the colon ITN rendering of a Spanish clock phrase.
// match_clock_expr reads a CLS_COLONTIME token directly as HH:MM inside
// clock context (match_clock_target's own "las" article trigger), so this
// transcript resolves to a REAL recognized clock target (18:30,
// 24h-known), never a guarded fail-soft -- this scene shows the real
// confirm screen (duration + "-> 18:30" annotation), the same
// literal-clock-target fixture pattern as the earlier clock-target scenes
// above (see the en scene's own note on why this is a literal NlParse
// rather than a live in-scene parse_nl() call).
static void scene_voice_confirm_colon_clock_target(void) {
  gallery_reset_fixture();
  const char *transcript = "a las 18:30";
  // app/test/parse_nl_test.c:1495, assert_clock_target(transcript, true, 18, 30, true);
  // 24h-known (colon token), so duration_s/clock_target_sod resolve
  // unambiguously to today's 18:30 (66600s-of-day), 34200s from the fixed
  // 09:00 "now" -- same numeric target as the "a las seis y media" scene
  // above, this time via the colon-clock routing.
  NlParse r = {
      .kind = KIND_COUNTDOWN, .duration_s = 34200, .has_duration = true, .ok = true,
      .clock = {.has_clock_target = true, .hour = 18, .minute = 30, .hour24_known = true},
      .clock_target_sod = 66600,
  };
  nav_go_voice();
  // Cancel the real attempt nav_go_voice() just kicked off -- see
  // scene_voice_confirm_countdown's own comment (first use, above) for why
  // this explicit call is needed on every voice scene.
  dictation_am_cancel();
  screen_voice_show_parse(transcript, &r, voice_outcome_for_status(0, r.ok));
  mark_scene(89, "voice_confirm_colon_clock_target");
}

// The subtractive clock family: "a las siete menos cuarto" -- shape
// "<HOUR> menos <N|cuarto>", the family's own headline example. H=7,
// N=15(cuarto) -> 6:45, 12h-ambiguous; at the fixed 09:00 "now" the AM
// occurrence (6:45) has already passed today, so nearest-future resolution
// picks the PM occurrence (18:45, ~9h45m away) -- the same "->18:45"-style
// annotation the earlier clock-target scenes already demonstrate for the
// additive forms, now proven for the subtractive family too. Guards against
// the failure mode where "menos cuarto" is dropped as unparsed residue,
// which would silently resolve to the bare hour (7:00) instead of the
// correct 6:45/18:45 target -- this scene is the real-pixels proof the
// subtractive modifier is actually applied.
static void scene_voice_confirm_subtractive_clock_target(void) {
  gallery_reset_fixture();
  const char *transcript = "a las siete menos cuarto";
  // Real parse_nl() output: the subtractive family resolves "siete
  // menos cuarto" to the same 6:45 12h-ambiguous clock target as the en
  // clock-target scene's additive "at six forty five" (clock.hour=6,
  // clock.minute=45, hour24_known=false) -- structurally the same family
  // app/test/parse_nl_test.c:1682's "las seis menos cuarto" ->(5,45) locks
  // (H-1:45), one hour up. duration_s/clock_target_sod: nearest-future PM
  // occurrence against the fixed 09:00 "now", 18:45 (67500s-of-day), 35100s
  // away -- same numeric target as the earlier clock-target scenes.
  NlParse r = {
      .kind = KIND_COUNTDOWN, .duration_s = 35100, .has_duration = true, .ok = true,
      .clock = {.has_clock_target = true, .hour = 6, .minute = 45, .hour24_known = false},
      .clock_target_sod = 67500,
  };
  nav_go_voice();
  // Cancel the real attempt nav_go_voice() just kicked off -- see
  // scene_voice_confirm_countdown's own comment (first use, above) for why
  // this explicit call is needed on every voice scene.
  dictation_am_cancel();
  screen_voice_show_parse(transcript, &r, voice_outcome_for_status(0, r.ok));
  // Slug shortened (vs the function name) to fit mark_scene's 40-byte
  // buffer alongside the "90:" index prefix -- the fuller
  // "..._clock_target" spelling silently truncated mid-word in the
  // appmessage marker (caught by an assert_log_contains check, not a
  // guess).
  mark_scene(90, "voice_confirm_subtractive_clock");
}

// CLS_NAMEDROP: "give me 15 minutes", the headline name-pollution case.
// Without NAMEDROP handling, "give"/"me" were plain CLS_WORD tokens sitting
// BEFORE the "15 minutes" match -- the parser's own name-residue left-walk
// would have stopped there, but the name residue still would have kept
// "give me" as name text (empty-name was never guaranteed for this
// phrasing). The NAMEDROP vocabulary drops both tokens from the residue
// (while staying opaque to kind resolution, so that stays unaffected) --
// this scene shows the real confirm screen with a genuinely EMPTY name line,
// the visible before/after this whole family exists to fix. Contrast with
// scene_voice_confirm_named (true-name lock, "pasta for eight minutes")
// which proves the OPPOSITE direction: a real scenario noun must survive.
static void scene_voice_confirm_namedrop(void) {
  gallery_reset_fixture();
  const char *transcript = "give me 15 minutes";
  // app/test/parse_nl_test.c:1832, assert_parse(transcript, true, KIND_COUNTDOWN, 900, "");
  NlParse r = {
      .kind = KIND_COUNTDOWN, .duration_s = 900, .has_duration = true, .ok = true,
  };
  nav_go_voice();
  // Cancel the real attempt nav_go_voice() just kicked off -- see
  // scene_voice_confirm_countdown's own comment (first use, above) for why
  // this explicit call is needed on every voice scene.
  dictation_am_cancel();
  screen_voice_show_parse(transcript, &r, voice_outcome_for_status(0, r.ok));
  mark_scene(91, "voice_confirm_namedrop");
}

// --- hero-NAME caption -------------------------------------------------
//
// The REAL, unconditional name caption (screen_hero.c's
// draw_hero_name_caption(), no debug lever any more) on the countdown/repeat
// RUNNING hero, via real store_add()/store_set_name()/timer_advance_cycle()
// mutations (never a hand-set Timer field). Short name "pasta"; long name a
// 26-char Spanish phrase that lib/timer.c's timer_name_copy() itself
// truncates to 23 chars ("cuenta regresiva del as") at store_set_name()
// time, BEFORE any render-level ellipsis gets a chance to run -- exercises
// truncation at both the input layer and (defensively) the render layer.
#define NAME_OPT_SHORT "pasta"
#define NAME_OPT_LONG "cuenta regresiva del asado"

// Same fixture recipe as scene_countdown_hero_running: 600s duration,
// started 442s ago -> remaining 2:38.
static int32_t name_option_countdown_fixture(const char *name) {
  int32_t cookie;
  store_add(app_context_store(), KIND_COUNTDOWN, 600, time(NULL) - 442, &cookie);
  store_set_name(app_context_store(), cookie, name);
  return cookie;
}

// Same fixture recipe as scene_repeat_hero_running: 5:00 interval,
// cycles=3, ~8s into the current cycle -> remaining 4:52.
static int32_t name_option_repeat_fixture(const char *name) {
  time_t now = time(NULL);
  int32_t cookie;
  store_add(app_context_store(), KIND_REPEAT, 300, now, &cookie);
  Timer *t = store_find(app_context_store(), cookie);
  timer_advance_cycle(t, now);
  timer_advance_cycle(t, now);
  timer_advance_cycle(t, now - 8);
  store_set_name(app_context_store(), cookie, name);
  return cookie;
}

// The REAL, shipped caption -- no debug lever any more (screen_hero.c's
// draw_hero_name_caption() always draws when the timer has a name). These 3
// scenes exercise exactly 3 states: short name (countdown), long name
// (countdown, truncation), and a named repeat.
static void scene_name_cd_short(void) {
  gallery_reset_fixture();
  int32_t cookie = name_option_countdown_fixture(NAME_OPT_SHORT);
  nav_go_hero(cookie);
  mark_scene(92, "name_cd_short");
}

static void scene_name_cd_long(void) {
  gallery_reset_fixture();
  int32_t cookie = name_option_countdown_fixture(NAME_OPT_LONG);
  nav_go_hero(cookie);
  mark_scene(93, "name_cd_long");
}

static void scene_name_rp(void) {
  gallery_reset_fixture();
  int32_t cookie = name_option_repeat_fixture(NAME_OPT_SHORT);
  nav_go_hero(cookie);
  mark_scene(94, "name_rp");
}

// --- Ringing hero + multi-alarm list name captions --------------------------
//
// screen_alarm.c's ringing hero name caption, and screen_ringing.c's
// multi-alarm ringing list row name -- both scenes below prove the real
// timer name renders here, never a hardcoded NULL placeholder.

// No sibling (others == 0) -- same shape as scene_alarm_oneoff_ringing_solo,
// named, so the caption's own dead-slot placement (ALARM_NAME_
// CAPTION_Y=72..88) is checked with the "ringing - N others" subheader
// ABSENT first.
static void scene_ringing_named_solo(void) {
  gallery_reset_fixture();
  nav_go_home();
  time_t now = time(NULL);
  int32_t cookie;
  store_add(app_context_store(), KIND_COUNTDOWN, 60, now - 67, &cookie);  // overrun 0:07, no sibling
  store_set_name(app_context_store(), cookie, "pasta");
  gallery_fire_all_due();
  mark_scene(95, "ringing_named_solo");
}

// Same 3-alarm fixture as scene_multi_alarm_ringing_list -- one named,
// two unnamed, so the row's name/no-name coexistence is visible in one
// screenshot (mirrors scene_home_named's own "named + name-less coexist"
// shape for HOME).
static void scene_multi_alarm_ringing_named(void) {
  gallery_reset_fixture();
  nav_go_home();
  time_t now = time(NULL);
  int32_t egg_cookie;
  store_add(app_context_store(), KIND_COUNTDOWN, 60, now - 67, &egg_cookie);  // egg, overrun 0:07
  store_set_name(app_context_store(), egg_cookie, "egg");
  store_add(app_context_store(), KIND_COUNTDOWN, 120, now - 140, NULL);  // pasta, overrun 0:20 (unnamed)
  store_add(app_context_store(), KIND_REPEAT, 300, now - 305, NULL);     // oven, overrun 0:05 (unnamed)
  gallery_fire_all_due();
  mark_scene(96, "multi_alarm_ringing_named");
}

// --- Growing-tally segmented bar ---------------------------------------
//
// Round N (= cycles+1) draws N equal segments, N-1 solid + the rightmost
// filling, for N up to RENDER_SEGMENT_CAP=6. Three scenes: round 1 (a plain
// full-width progress bar), round 3 (2 solid + 1 filling), round 7 (past the
// cap -- the bar drops back to a SINGLE full-width filling bar, visually
// identical to round 1, NOT frozen at 6 segments; the separate "done x6"
// text still carries the true count).

static void scene_repeat_tally_round1(void) {
  gallery_reset_fixture();
  time_t now = time(NULL);
  int32_t cookie;
  store_add(app_context_store(), KIND_REPEAT, 300, now - 8, &cookie);  // cycles=0, 8s in -> remaining 4:52
  nav_go_hero(cookie);
  mark_scene(97, "repeat_tally_round1");
}

static void scene_repeat_tally_round3(void) {
  gallery_reset_fixture();
  time_t now = time(NULL);
  int32_t cookie;
  store_add(app_context_store(), KIND_REPEAT, 300, now, &cookie);
  Timer *t = store_find(app_context_store(), cookie);
  timer_advance_cycle(t, now);
  timer_advance_cycle(t, now - 8);  // cycles=2, 8s into round 3 -> remaining 4:52
  nav_go_hero(cookie);
  mark_scene(98, "repeat_tally_round3");
}

// Past RENDER_SEGMENT_CAP the bar no longer freezes at 6 segments -- it
// drops back to a SINGLE full-width filling bar, visually identical to
// round 1. Same cycles=6 fixture as before (round 7, one past the cap) --
// only the render's own past-cap behavior changed, not this scene's setup.
// "done x6" (the true count) is still visible in the meta line even though
// the bar itself now reads just like round 1's.
static void scene_repeat_tally_round7_singlebar(void) {
  gallery_reset_fixture();
  time_t now = time(NULL);
  int32_t cookie;
  store_add(app_context_store(), KIND_REPEAT, 300, now, &cookie);
  Timer *t = store_find(app_context_store(), cookie);
  for (int i = 0; i < 5; i++) {
    timer_advance_cycle(t, now);
  }
  timer_advance_cycle(t, now - 8);  // cycles=6, 8s into round 7 -> single full-width bar, done x6
  nav_go_hero(cookie);
  mark_scene(99, "repeat_tally_round7_singlebar");
}

// --- Empty-state hero, unselected/dimmed --------------------------------
//
// scene_home_empty/scene_library_empty already show the SELECTED state
// (cursor lands on row 0 by default); these two show the UNSELECTED/dimmed
// state (cursor moved to row 1) for each screen's own empty-state "+" hero.

static void scene_home_empty_hero_unselected(void) {
  gallery_reset_fixture();
  nav_go_home();
  screen_home_debug_refresh();
  gallery_refresh_menu(screen_home_debug_menu(), 1);  // "Say a timer" row -> hero unselected
  mark_scene(100, "home_empty_hero_unselected");
}

static void scene_library_empty_hero_unselected(void) {
  gallery_reset_fixture();
  nav_go_library();
  gallery_refresh_menu(screen_library_debug_menu(), 1);  // "Say a preset" row -> hero unselected
  mark_scene(101, "library_empty_hero_unselected");
}

// --- 10h-duration shrink-to-fit + XLARGE name/digit collision ----------

// A real 10:00:00 countdown used to clip to "10:0..." at the hero's 56px
// numeral tier -- render_fit_numeral_font now falls back to 42px here.
static void scene_countdown_hero_10h(void) {
  gallery_reset_fixture();
  int32_t cookie;
  store_add(app_context_store(), KIND_COUNTDOWN, 36000, time(NULL), &cookie);  // 10:00:00
  nav_go_hero(cookie);
  mark_scene(102, "countdown_hero_10h");
}

// HOME's XLARGE (1-timer) row at a 10h duration AND a maximal-length name
// together -- another named clip site, plus the XLARGE name/digit
// collision fix (render_row's label_max_w floor).
static void scene_home_1_10h_xlarge(void) {
  gallery_reset_fixture();
  int32_t cookie;
  store_add(app_context_store(), KIND_COUNTDOWN, 36000, time(NULL), &cookie);  // 10:00:00, XLARGE tier
  store_set_name(app_context_store(), cookie, "cuenta regresiva del asado");  // truncates to 23 chars
  nav_go_home();
  screen_home_debug_refresh();
  mark_scene(103, "home_1_10h_xlarge");
}

// --- Numeral-fit coverage at other count tiers ------------------------------
//
// The XLARGE-only fit check above never protects HOME's LARGE tier
// (2 timers) or a named Library preset row -- both go through the SAME
// render_row() numeral-vs-glyph-column collision the XLARGE fix was built
// for, just at a different count-based starting font. Now that the fit is
// centralized INSIDE render_row() itself (render.c's fit_row_number_font),
// these two scenes prove the invariant holds at those other sites too, not
// just the one HOME count-tier the original fix covered.
static void scene_home_2timers_10h(void) {
  gallery_reset_fixture();
  time_t now = time(NULL);
  store_add(app_context_store(), KIND_COUNTDOWN, 36000, now, NULL);   // 10:00:00 -- the at-risk sibling
  store_add(app_context_store(), KIND_COUNTDOWN, 372, now, NULL);     // 6:12, ordinary second row
  nav_go_home();
  screen_home_debug_refresh();
  mark_scene(104, "home_2timers_10h");
}

static void scene_library_10h_maxname(void) {
  gallery_reset_fixture();
  preset_store_add(app_context_presets(), KIND_COUNTDOWN, 36000);  // 10:00:00
  preset_store_set_name(app_context_presets(), 0, "Weekly Standup!");  // 15 chars = PRESET_NAME_MAX-1
  nav_go_library();
  mark_scene(105, "library_10h_maxname");
}

// The ringing list's own "egg" fixture above never reaches the caret's own
// 12px left reach -- a maximal (TIMER_NAME_MAX-1 = 23 char) name does,
// ellipsizing right up against where the caret now paints.
static void scene_multi_alarm_ringing_maxname(void) {
  gallery_reset_fixture();
  nav_go_home();
  time_t now = time(NULL);
  int32_t cookie;
  store_add(app_context_store(), KIND_COUNTDOWN, 60, now - 67, &cookie);  // overrun 0:07
  store_set_name(app_context_store(), cookie, "cuenta regresiva del asado");  // truncates to 23 chars
  store_add(app_context_store(), KIND_COUNTDOWN, 120, now - 140, NULL);  // pasta, unnamed sibling
  gallery_fire_all_due();
  mark_scene(106, "multi_alarm_ringing_maxname");
}

// scene_library_10h_maxname above deliberately has a NAME, which routes it
// through the ordinary right-aligned branch -- it can never exercise the
// number_left_align/chevron branch unnamed presets actually use. This scene
// is the same 10:00:00 duration, UNNAMED, so `name_less` stays true
// (screen_library.c's draw_row) and it hits the branch that had been
// bypassing the centralized fit.
static void scene_library_10h_unnamed(void) {
  gallery_reset_fixture();
  preset_store_add(app_context_presets(), KIND_COUNTDOWN, 36000);  // 10:00:00, unnamed
  nav_go_library();
  mark_scene(107, "library_10h_unnamed");
}

// --- scene table + navigation -----------------------------------------------

typedef void (*GallerySceneBuilder)(void);

// Base states are followed immediately by a few bonus variant scenes
// (paused/holdfill/cursor-position variants interleaved right after the
// state they vary) -- this table's order and indices are part of the
// gallery's public contract: anything that walks scenes by index (or
// verifies the total scene count) depends on this exact ordering staying
// stable, so treat reordering/removing an entry as a breaking change
// (gallery_init's "gallery_count" testlog marker publishes the current
// total for exactly that kind of drift check).
static const GallerySceneBuilder s_scenes[] = {
    scene_home_empty,               // 00
    scene_home_1_countdown,         // 01
    scene_home_2_timers,            // 02
    scene_home_scroll_top,          // 03
    scene_home_scroll_cursor,       // 04 (bonus variant)
    scene_set_build_duration,       // 05
    scene_countdown_hero_running,   // 06
    scene_countdown_hero_paused,    // 07 (bonus variant)
    scene_countdown_hero_holdfill,  // 08 (bonus variant)
    scene_countdown_overtime,       // 09
    scene_stopwatch_hero_running,   // 10
    scene_repeat_hero_running,      // 11
    scene_post_timer_stopwatch,     // 12
    scene_actions_countdown,        // 13
    scene_actions_countdown_cursor, // 14 (bonus variant)
    scene_actions_stopwatch,        // 15
    scene_actions_repeat,           // 16
    scene_laps_one_lap,             // 17
    scene_alarm_oneoff_solo,        // 18
    scene_alarm_oneoff_options,     // 19
    scene_alarm_repeat_fired,       // 20
    scene_multi_alarm_ringing_list, // 21
    scene_set_repeat,               // 22 (create-repeat, shipped shape)
    scene_alarm_oneoff_ringing_solo,       // 23 (no-others variant)
    scene_alarm_repeat_options,            // 24 (repeat alarm options menu, "Snooze 1/4" row)
    scene_alarm_snooze_quarter_holdfill,   // 25 (alarm Up-hold, gated ON, mid-hold)
    scene_alarm_snooze_quarter_gated_off,  // 26 (alarm Up-hold, gated OFF)
    scene_repeat_hero_snoozing,     // 27 (real timer_snooze_repeat mutation)
    scene_home_repeat_snoozing,     // 28 (HOME row label)
    scene_stopwatch_running,           // 29 (laps on the hero screen + legend + resting footer)
    scene_stopwatch_laps_empty,        // 30 (empty state)
    scene_stopwatch_up_hint,           // 31 (transient short-Up-press hint)
    scene_stopwatch_pause_holdfill,    // 32 (Select held mid-fill)
    scene_stopwatch_lap_holdfill,      // 33 (Up held mid-fill)
    scene_home_clearall_row,               // 34 (real "Clear all" row)
    scene_home_clearall_confirm,           // 35 (real confirm screen)
    scene_home_max_footer_highlighted,     // 36 (max-reached footer, highlighted)
    scene_home_max_footer_resting,         // 37 (max-reached footer, resting)
    scene_home_2timers_cursor_row1,        // 38 (2-timer HOME, cursor row1, legibility check)
    scene_stopwatch_paused,                // 39 (paused-stopwatch, shipped combined treatment)
    scene_home_with_library_footer,        // 40 (populated HOME, cursor on "Library" row)
    scene_library_mixed_list,              // 41 (real Library, countdown+repeat presets)
    scene_library_empty,                   // 42 (real Library, 0 presets)
    scene_countdown_addquarter_resting,       // 43 (countdown Up-hold, gated ON, resting)
    scene_countdown_addquarter_holdfill,      // 44 (countdown Up-hold, gated ON, mid-hold preview)
    scene_countdown_addquarter_gated_off,     // 45 (countdown Up-hold, gated OFF)
    scene_repeat_hero_restart_holdfill,     // 46 (repeat Up-hold=Restart, mid-hold)
    scene_preset_actions_sheet,            // 47 (real {Edit, Rename, Delete} sheet)
    scene_set_save_preset,                 // 48 (real SET PRESET_NEW dial)
    scene_set_edit_preset,                 // 49 (real SET PRESET_EDIT dial, prefilled)
    scene_library_one_countdown,           // 50 (1-preset Library, no-scroll)
    scene_library_full_scroll,             // 51 (12-preset Library, scroll-to-bottom check)
    scene_voice_confirm_countdown,          // 52 (literal NlParse fixture, see body, en countdown)
    scene_voice_confirm_repeat_es,          // 53 (literal NlParse fixture, see body, es repeat)
    scene_voice_confirm_stopwatch,          // 54 (literal NlParse fixture, see body, accented stopwatch keyword)
    scene_voice_confirm_compound,           // 55 (literal NlParse fixture, see body, compound duration sum)
    scene_voice_confirm_named,              // 56 (literal NlParse fixture, see body, name RENDERS -- name line + "Rename" row)
    scene_voice_parse_fail,                 // 57 (literal NlParse fixture, see body, fallback ok=false)
    scene_voice_no_phone,                   // 58 (real voice_outcome_for_status, create()==NULL)
    scene_voice_no_speech,                  // 59 (real voice_outcome_for_status, status 4)
    scene_voice_disabled,                   // 60 (real voice_outcome_for_status, status 6)
    scene_voice_edit_prefilled,             // 61 (Edit-on-dial reuse proof, real SET state machine)
    scene_home_voice_row_populated,         // 62 (populated footer, cursor on voice row)
    scene_home_empty_voice_row,             // 63 (empty-state, cursor on voice row)
    scene_home_named,                       // 64 (named + name-less coexist; "snoozing" precedence)
    scene_voice_add_name_result,            // 65 (real screen_voice_set_name() Add-name/Rename arc)
    scene_home_max_voice_highlighted,       // 66 ("Say a timer" MAX message parity, highlighted)
    scene_library_named_mixed,             // 67 (named+name-less mixed list, degrade rule pixels)
    scene_preset_rename_result,             // 68 (real screen_preset_actions_apply_name() Rename arc)
    scene_home_stopwatch_cursor,            // 69 (cursor-on-stopwatch-row contrast, regression coverage)
    scene_voice_confirm_pair_es,            // 70 (literal NlParse fixture, see body, spoken M:SS pair rule)
    scene_voice_confirm_fraction_es,        // 71 (literal NlParse fixture, see body, fractions engine)
    scene_library_max_name,                 // 72 (maximal-length preset name, ellipsis clamp)
    scene_home_repeat_cursor,               // 73 (cursor on a KIND_REPEAT row at HOME)
    scene_library_repeat_cursor,            // 74 (cursor on a KIND_REPEAT row in Library)
    scene_library_voice_row_populated,      // 75 (populated Library, cursor on "Say a preset")
    scene_library_empty_voice_row,          // 76 (empty Library, cursor on "Say a preset" + empty-hero layout)
    scene_library_max_presets_voice_row,    // 77 (MAX_PRESETS, "Say a preset" deliberately ungated)
    scene_voice_preset_confirm,             // 78 (preset-target confirm, "Save" primary action)
    scene_voice_preset_stopwatch_unsupported, // 79 (honest fail-soft, "Presets can't be a stopwatch")
    scene_voice_preset_save_result,          // 80 (Save arc end state, cursor-restore principle)
    scene_library_hint_active,               // 81 (discoverability hint, forced via debug lever)
    scene_actions_countdown_with_rename,      // 82 (ACTIONS Rename row, running-timer)
    scene_actions_rename_result,              // 83 (ACTIONS Rename end state, real seam, HOME check)
    scene_voice_preset_edit_prefilled,        // 84 (preset-target Edit-on-dial -> SET_MODE_PRESET_NEW)
    scene_voice_confirm_minutes_trailing_es,  // 85 (literal NlParse fixture, see body, generalized unit+trailing-sub-count rule)
    scene_voice_confirm_clock_target_en,      // 86 (literal NlParse fixture, see body, matches real parse_nl + nl_clock_target_to_duration, en)
    scene_voice_confirm_clock_target_es,      // 87 (literal NlParse fixture, see body, matches real parse_nl + nl_clock_target_to_duration, es)
    scene_voice_confirm_clock_target_12h,     // 88 (same parse, forced 12h device style)
    scene_voice_confirm_colon_clock_target,   // 89 (literal NlParse fixture, see body, colon-clock routing)
    scene_voice_confirm_subtractive_clock_target,  // 90 ("a las siete menos cuarto" subtractive family)
    scene_voice_confirm_namedrop,                  // 91 ("give me 15 minutes" NAMEDROP, real empty name)
    scene_name_cd_short,                    // 92 (real hero-name caption, countdown short name)
    scene_name_cd_long,                     // 93 (real hero-name caption, countdown long name -- truncation)
    scene_name_rp,                          // 94 (real hero-name caption, repeat)
    scene_ringing_named_solo,               // 95 (ringing hero caption, named one-off)
    scene_multi_alarm_ringing_named,        // 96 (multi-alarm ringing list, named row)
    scene_repeat_tally_round1,              // 97 (tally bar, round 1 -- single full-width bar)
    scene_repeat_tally_round3,              // 98 (tally bar, round 3 -- 2 solid + 1 filling)
    scene_repeat_tally_round7_singlebar,    // 99 (tally bar, round 7 -- past cap, single full-width bar)
    scene_home_empty_hero_unselected,       // 100 (HOME "+" hero, unselected/dimmed)
    scene_library_empty_hero_unselected,    // 101 (Library "+" hero, unselected/dimmed)
    scene_countdown_hero_10h,               // 102 (10h countdown hero, shrink-to-fit)
    scene_home_1_10h_xlarge,                // 103 (HOME XLARGE row, 10h duration + long name)
    scene_home_2timers_10h,                 // 104 (HOME LARGE tier, 10h sibling)
    scene_library_10h_maxname,              // 105 (named Library preset, 10h + max name)
    scene_multi_alarm_ringing_maxname,       // 106 (ringing list, max name vs caret)
    scene_library_10h_unnamed,               // 107 (unnamed Library preset, 10h, chevron branch)
};
#define GALLERY_SCENE_COUNT ((int)(sizeof(s_scenes) / sizeof(s_scenes[0])))

static int s_scene_index;

static void gallery_show_scene(int index) {
  if (index < 0) index = GALLERY_SCENE_COUNT - 1;
  if (index >= GALLERY_SCENE_COUNT) index = 0;
  s_scene_index = index;
  s_scenes[index]();
}

static void up_click_handler(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
  gallery_show_scene(s_scene_index - 1);
}

static void down_click_handler(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
  gallery_show_scene(s_scene_index + 1);
}

static void select_click_handler(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
  gallery_show_scene(s_scene_index + 1);
}

static void back_click_handler(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
  gallery_show_scene(0);
}

void gallery_click_config_provider(void *context) {
  (void)context;
  window_single_click_subscribe(BUTTON_ID_UP, up_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, down_click_handler);
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click_handler);
  window_single_click_subscribe(BUTTON_ID_BACK, back_click_handler);
}

void gallery_init(void) {
  char count_data[8];
  snprintf(count_data, sizeof count_data, "%d", GALLERY_SCENE_COUNT);
  testlog("gallery_count", count_data);
  gallery_show_scene(0);
}

#endif  // GALLERY_MODE
