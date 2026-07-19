// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

#include "nav.h"

#include <pebble.h>
#include <stdio.h>

#include "app_context.h"
#include "dictation_am.h"
#include "hold.h"
#include "input_guard.h"
#include "lib/nav_stack.h"
#include "screen_actions.h"
#include "screen_alarm.h"
#include "screen_clear_confirm.h"
#include "screen_hero.h"
#include "screen_home.h"
#include "screen_laps.h"
#include "screen_library.h"
#include "screen_preset_actions.h"
#include "screen_ringing.h"
#include "screen_set.h"
#include "screen_voice.h"
#include "testlog_am.h"

static Window *s_home;
static Window *s_set;
static Window *s_hero;
static Window *s_actions;
static Window *s_alarm;
static Window *s_alarm_options;
static Window *s_laps;
static Window *s_ringing_list;
static Window *s_clear_confirm;
static Window *s_library;
static Window *s_preset_actions;
static Window *s_voice;

static ViewDesc s_current = {.kind = VD_HOME, .cookie = 0};
static ViewDesc s_resume;
static bool s_has_resume;

// -1 = no pending focus (HOME's own window_appear falls back to row 0 when
// there is none). Only ever set by nav_go_home_from_hero(); consumed
// (read-and-clear) by nav_take_home_focus_cookie().
static int32_t s_home_focus_cookie = -1;

// The LIBRARY-scoped counterpart of s_home_focus_cookie above -- -1 = no
// pending focus (Library's own window_appear falls back to row 0/top when
// there is none). Only ever set by nav_go_library_focus_row(); consumed
// (read-and-clear) by nav_take_library_focus_row(). A plain row index, not a
// cookie -- Library's presets are index-addressed, so no dead-reference
// translation is needed the way s_home_focus_cookie needs store_home_row_
// for_cookie.
static int s_library_focus_row = -1;

// What render_stack() believes is PHYSICALLY on the OS window stack right
// now -- kept in sync with every diff it applies (see nav_diff_stack's
// no-op guarantee for identical breadcrumbs). Starts empty; the very first
// render_stack() call (from nav_init) diffs against this and pushes HOME.
static NavBreadcrumb s_stack_bc = {.len = 0};

static Window *window_for_kind(ViewDescKind kind) {
  switch (kind) {
    case VD_HOME: return s_home;
    case VD_SET: return s_set;
    case VD_HERO: return s_hero;
    case VD_ACTIONS: return s_actions;
    case VD_ALARM: return s_alarm;
    case VD_ALARM_OPTIONS: return s_alarm_options;
    case VD_LAPS: return s_laps;
    case VD_RINGING_LIST: return s_ringing_list;
    case VD_CLEAR_CONFIRM: return s_clear_confirm;
    case VD_LIBRARY: return s_library;
    case VD_PRESET_ACTIONS: return s_preset_actions;
    case VD_VOICE: return s_voice;
  }
  return NULL;
}

// Updates the target screen's cookie-scoped content BEFORE it's pushed --
// mirrors what the old render_stack() did immediately before each push.
static void apply_cookie(ViewDesc v) {
  switch (v.kind) {
    case VD_HERO:
      screen_hero_set_cookie(v.cookie);
      break;
    case VD_ACTIONS:
      screen_actions_set_cookie(v.cookie);
      break;
    case VD_ALARM:
    case VD_ALARM_OPTIONS:
      screen_alarm_set_cookie(v.cookie);
      break;
    case VD_LAPS:
      screen_laps_set_cookie(v.cookie);
      break;
    case VD_HOME:
    case VD_SET:
    case VD_RINGING_LIST:
      // RINGING_LIST has no per-cookie priming -- it reads AppState's
      // ringing[] directly at draw time (see screen_ringing.c).
      break;
    case VD_CLEAR_CONFIRM:
      // No per-cookie priming -- it reads app_context_store()->count
      // directly at draw time (screen_clear_confirm.c).
      break;
    case VD_LIBRARY:
      // No per-cookie priming -- it reads the PresetStore live at draw time
      // (screen_library.c), same shape as VD_CLEAR_CONFIRM above.
      break;
    case VD_PRESET_ACTIONS:
      // v.cookie carries the PRESET INDEX (ViewDesc's documented reuse) --
      // prime the sheet's target before it's
      // pushed, mirroring screen_actions_set_cookie/screen_laps_set_cookie's
      // own "prime right before push" shape above.
      screen_preset_actions_set_index((int)v.cookie);
      break;
    case VD_VOICE:
      // No per-cookie priming here -- nav_go_voice() itself primes
      // screen_voice.c's held state (via screen_voice_show_unavailable())
      // BEFORE setting s_current, same timing nav_go_preset_new()/
      // nav_go_edit_interval() already use for their own priming calls (the
      // REAL dictation_am_start() call happens after render_stack(), see
      // nav_go_voice()'s own comment for why). Same "reads live state, not
      // cookie-scoped" shape as VD_LIBRARY/VD_CLEAR_CONFIRM above.
      break;
  }
}

// This diffs `s_current`'s target breadcrumb against `s_stack_bc`
// (lib/nav_stack.h, pure + unit-tested) and issues only the minimal
// window_stack_pop/push calls needed -- push only what's new, pop only what
// leaves. A naive approach would unconditionally do window_stack_pop_all()
// then re-push the WHOLE breadcrumb from HOME on every transition -- e.g.
// HERO->ACTIONS would be pop_all(); push(HOME); push(HERO); push(ACTIONS),
// three back-to-back window_stack_push calls hitting PebbleOS's fragile
// force-complete-in-flight-animation path (window_stack.c:483-486,
// PBL-37477 TODO) and reproducing a firmware assertion crash on ~100% of
// single navigating presses. The dead-reference guard is unaffected:
// nav_policy already computes the CORRECT target ViewDesc (nav_on_resolve
// etc.) regardless of how it's physically mirrored onto the stack;
// nav_diff_stack always converges the window stack to exactly that target,
// the same end state a full rebuild would produce.
//
// render_stack() is stated, below, as the one place every real navigation
// goes through -- that invariant has exactly ONE deliberate exception,
// nav_exit_to_watchface() (see its own comment for why a direct pop_all
// instead of this diffed push/pop is correct there, not an oversight).
static void render_stack(void) {
  NavBreadcrumb target = nav_breadcrumb_for(s_current);
  NavStackDiff diff = nav_diff_stack(s_stack_bc, target);

  if (diff.pop_count == 0 && diff.push_count == 0) return;  // no-op: zero stack calls

  // A pending hold must never survive a window transition (the phantom-fire
  // guard) -- structural now: every real navigation goes through this one
  // place, so every nav_go_*/nav_on_timer_fired/nav_alarm_resolved caller
  // gets this for free rather than needing its own hold_cancel() call site.
  hold_cancel();
  // Same structural-cancel reasoning applies to
  // ACTIONS' held-Down carry-through auto-scroll AppTimer -- must never fire
  // against an off-screen menu (Back, a Select that navigates away, or an
  // alarm pre-emption popping the stack all funnel through here). No-op if
  // nothing is in progress; does not disturb the carry-scroll ARM flag a
  // fresh hero->ACTIONS transition may have just set (see
  // screen_actions_cancel_carry_scroll's own doc comment for why those two
  // are kept orthogonal).
  screen_actions_cancel_carry_scroll();
  // Same structural-cancel reasoning applies
  // to an in-progress dictation attempt -- an alarm pre-empting mid-
  // dictation (the tick/wakeup handlers run independently of button input,
  // so this can happen while the system dictation UI owns the screen) must
  // stop the stale session rather than let its eventual callback fire
  // against a screen nobody's looking at anymore. No-op if nothing is in
  // progress (dictation_am_cancel()'s own contract, mirroring hold_cancel()
  // above).
  dictation_am_cancel();

  for (int i = 0; i < diff.pop_count; i++) {
    window_stack_pop(false);
  }
  for (int i = 0; i < diff.push_count; i++) {
    apply_cookie(diff.push[i]);
    window_stack_push(window_for_kind(diff.push[i].kind), false);
  }

  s_stack_bc = target;
}

void nav_init(void) {
  s_home = screen_home_create();
  s_set = screen_set_create();
  s_hero = screen_hero_create();
  s_actions = screen_actions_create();
  s_alarm = screen_alarm_create();
  s_alarm_options = screen_alarm_options_create();
  s_laps = screen_laps_create();
  s_ringing_list = screen_ringing_create();
  s_clear_confirm = screen_clear_confirm_create();
  s_library = screen_library_create();
  s_preset_actions = screen_preset_actions_create();
  s_voice = screen_voice_create();

  s_current = (ViewDesc){.kind = VD_HOME, .cookie = 0};
  s_has_resume = false;
  render_stack();
}

void nav_go_home(void) {
  // Explicitly clear any pending hero-focus cookie -- every OTHER path back
  // to HOME (Library Back, Clear-all confirm/cancel, SET cancel, ...) must
  // never reuse a stale one from an earlier hero visit.
  s_home_focus_cookie = -1;
  s_current = (ViewDesc){.kind = VD_HOME, .cookie = 0};
  render_stack();
}

void nav_go_home_from_hero(int32_t cookie) {
  s_home_focus_cookie = cookie;
  s_current = (ViewDesc){.kind = VD_HOME, .cookie = 0};
  render_stack();
}

int32_t nav_take_home_focus_cookie(void) {
  int32_t cookie = s_home_focus_cookie;
  s_home_focus_cookie = -1;
  return cookie;
}

void nav_go_home_focus_voice_row(void) {
  s_home_focus_cookie = NAV_HOME_FOCUS_VOICE_ROW;
  s_current = (ViewDesc){.kind = VD_HOME, .cookie = 0};
  render_stack();
}

void nav_go_set(void) {
  screen_set_reset();
  s_current = (ViewDesc){.kind = VD_SET, .cookie = 0};
  render_stack();
}

void nav_go_hero(int32_t cookie) {
  s_current = (ViewDesc){.kind = VD_HERO, .cookie = cookie};
  render_stack();
}

void nav_go_actions(int32_t cookie) {
  s_current = (ViewDesc){.kind = VD_ACTIONS, .cookie = cookie};
  render_stack();
}

void nav_go_alarm_options(int32_t cookie) {
  s_current = (ViewDesc){.kind = VD_ALARM_OPTIONS, .cookie = cookie};
  render_stack();
}

void nav_back_from_alarm_options(void) {
  s_current = (ViewDesc){.kind = VD_ALARM, .cookie = s_current.cookie};
  render_stack();
}

void nav_go_laps(int32_t cookie) {
  s_current = (ViewDesc){.kind = VD_LAPS, .cookie = cookie};
  render_stack();
}

void nav_go_clear_confirm(void) {
  s_current = (ViewDesc){.kind = VD_CLEAR_CONFIRM, .cookie = 0};
  render_stack();
}

void nav_go_edit_interval(int32_t cookie) {
  screen_set_start_edit(cookie);
  s_current = (ViewDesc){.kind = VD_SET, .cookie = cookie};
  render_stack();
}

void nav_go_library(void) {
  // Explicitly clear any pending voice-focus row -- every OTHER path back to
  // LIBRARY (preset-actions sheet's Back/Delete, SET's preset-new/edit
  // commit/cancel, ...) must never reuse a stale one from an earlier voice
  // session (same "clear it here" discipline as nav_go_home()'s own
  // s_home_focus_cookie clear).
  s_library_focus_row = -1;
  s_current = (ViewDesc){.kind = VD_LIBRARY, .cookie = 0};
  render_stack();
}

void nav_go_library_focus_row(int row) {
  s_library_focus_row = row;
  s_current = (ViewDesc){.kind = VD_LIBRARY, .cookie = 0};
  render_stack();
}

int nav_take_library_focus_row(void) {
  int row = s_library_focus_row;
  s_library_focus_row = -1;
  return row;
}

// Mirrors nav_go_edit_interval's own shape -- prime SET's adapter state
// BEFORE render_stack() switches the logical view, so an alarm take-over
// mid-dial (nav_on_fire) captures the already-primed SET session as its
// resume target, and nav_on_resolve's dead-reference guard restores it
// untouched on return (VD_SET(0) rides the same "always coherent,
// create-mode" path a preset dial reuses).
void nav_go_preset_new(void) {
  screen_set_start_preset_new();
  s_current = (ViewDesc){.kind = VD_SET, .cookie = 0};
  render_stack();
}

void nav_go_preset_edit(int index) {
  screen_set_start_preset_edit(index);
  s_current = (ViewDesc){.kind = VD_SET, .cookie = 0};
  render_stack();
}

void nav_go_preset_actions(int index) {
  // VD_PRESET_ACTIONS repurposes ViewDesc.cookie to carry the preset index
  // (documented reuse, lib/nav_policy.h) -- apply_cookie primes
  // screen_preset_actions_set_index() from it right before this pushes.
  s_current = (ViewDesc){.kind = VD_PRESET_ACTIONS, .cookie = index};
  render_stack();
}

void nav_go_voice(void) {
  // Primes a SAFE PLACEHOLDER before the switch (mirroring
  // nav_go_preset_new()'s own "prime, then flip s_current" shape), but the
  // real dictation attempt itself starts AFTER render_stack(), not before.
  // This ordering is deliberate:
  //  - render_stack() calls dictation_am_cancel() unconditionally (the same
  //    structural per-transition funnel hold_cancel() already uses) to stop
  //    a STALE session left over from whatever screen was showing before.
  //    If dictation_am_start() ran BEFORE render_stack(), that same
  //    unconditional cancel would immediately kill the session THIS call
  //    just started -- a self-inflicted bug that would show up in the log
  //    as an immediate "start" then "cancel" pair. hold.c's own bindings
  //    avoid this the same way: they're (re)established from the window's
  //    click_config_provider, which only ever runs AFTER window_stack_push,
  //    i.e. always after render_stack()'s own cancel call, never before.
  //  - screen_voice_show_unavailable() primes a SAFE placeholder (not a
  //    real dictation attempt) so a nav pre-emption landing BEFORE the real
  //    attempt's outcome is known (an alarm firing mid-dictation) has sane
  //    content to fall back to instead of stale/zero-default state -- see
  //    that function's own doc comment in screen_voice.h.
  //
  // On the emulator, dictation_session_create() actually succeeds (rather
  // than returning NULL): a connected transport (the same channel scripted
  // input and structured logging already ride) satisfies PebbleOS's "is a
  // companion connected" check, so create() succeeds and
  // dictation_session_start() genuinely begins a session-setup handshake
  // over that channel -- one nothing on the other end ever answers (there's
  // no real phone-side companion). Pressing Back while that handshake is pending
  // delivers a real Rejected status, which dictation_am.c routes to a
  // silent nav_go_home_focus_voice_row() (not plain nav_go_home()); an
  // alarm firing during the same window pre-empts exactly like any other
  // screen, but no longer restores this placeholder once it resolves:
  // nav_on_resolve excludes VD_VOICE as a resume target unconditionally,
  // resolving straight to HOME instead (see lib/nav_policy.c's own comment
  // for why).
  screen_voice_set_target(VOICE_TARGET_TIMER);
  screen_voice_show_unavailable();
  s_current = (ViewDesc){.kind = VD_VOICE, .cookie = 0};
  render_stack();
  dictation_am_start(DICT_SCOPE_FULL);
}

// Byte-for-byte the same
// prime/switch/start ordering as nav_go_voice() above (see that function's
// own comment for exactly why the split matters) -- the only difference is
// screen_voice_set_target(VOICE_TARGET_PRESET) up front, which flips the
// confirm screen's primary action to "Save" and every cancel/dial-escape
// path to LIBRARY instead of HOME (screen_voice.c's own target-branching).
void nav_go_voice_preset(void) {
  screen_voice_set_target(VOICE_TARGET_PRESET);
  screen_voice_show_unavailable();
  s_current = (ViewDesc){.kind = VD_VOICE, .cookie = 0};
  render_stack();
  dictation_am_start(DICT_SCOPE_FULL);
}

void nav_go_set_from_voice(int32_t duration_s, bool is_repeat) {
  screen_set_start_from_voice(duration_s, is_repeat);
  s_current = (ViewDesc){.kind = VD_SET, .cookie = 0};
  render_stack();
}

void nav_go_preset_new_from_voice(int32_t duration_s, bool is_repeat) {
  screen_set_start_preset_new_from_voice(duration_s, is_repeat);
  s_current = (ViewDesc){.kind = VD_SET, .cookie = 0};
  render_stack();
}

void nav_go_alarm(int32_t cookie) {
  // Select on a ringing-list row -- opens that specific alarm's own
  // hero. Does NOT touch s_resume/s_has_resume (those still hold whatever
  // was showing before the FIRST fire ever took over -- the same invariant
  // nav_back_from_alarm_options already relies on).
  s_current = (ViewDesc){.kind = VD_ALARM, .cookie = cookie};
  render_stack();
}

// Sets s_current to nav_policy's alarm-family routing decision (single hero
// vs. the ringing list) and logs which one, so this decision can be
// verified directly rather than depending solely on OCR-reading "N RINGING"
// on screen -- OCR misreads of on-screen digit counts are a known
// false-positive risk for that kind of check.
static void set_alarm_target(int ringing_count, int32_t solo_cookie) {
  s_current = nav_alarm_target(ringing_count, solo_cookie);
  char data[24];
  if (s_current.kind == VD_RINGING_LIST) {
    snprintf(data, sizeof data, "list:%d", ringing_count);
  } else {
    snprintf(data, sizeof data, "solo:%d", (int)s_current.cookie);
  }
  testlog("alarm_target", data);
  // This is the SOLE app-initiated alarm-nav funnel (both call sites below
  // are app-driven -- a timer firing, or the display stepping to the next
  // still-ringing alarm) -- arming here, and nowhere else, covers every
  // app-initiated alarm-family transition without ever swallowing a
  // deliberate Back on a user-initiated nav (nav_go_alarm, the alarm
  // options window's own Back, etc. never call this function).
  input_guard_arm();
}

void nav_on_timer_fired(int32_t cookie) {
  bool take_over = nav_on_fire(s_current);
  AppState *app_state = app_context_app_state();
  if (take_over) {
    s_resume = s_current;
    s_has_resume = true;
    set_alarm_target(app_state->ringing_count, cookie);
  } else if (s_current.kind != VD_RINGING_LIST) {
    // A second/third alarm just stacked into ringing[] while a single alarm
    // hero (or its options) was already showing -- when several fire at
    // once they should stack into a ringing list. Re-route to the list now
    // rather than leaving the display pinned on the first cookie only.
    set_alarm_target(app_state->ringing_count, cookie);
  }
  // else: already on the ringing list -- nothing to change here; the list's
  // own per-tick redraw (screen_ringing_mark_dirty) re-syncs its row count
  // from AppState directly.
  render_stack();
}

// The just-resolved cookie was store_remove()d (or, for keepCounting, kept
// but no longer ringing) by the caller before this runs, so leaving
// s_current.cookie unchanged would strand the alarm hero pointed at a dead
// timer (blank screen, dead buttons). Step up to the next still-ringing
// cookie via the pure nav_policy decision. Shared by both resolution paths
// below -- the stacking priority is identical regardless of HOW the
// just-resolved alarm was resolved. Routes through nav_alarm_target too --
// "clearing one steps the next up" lands back on the list if >=2 still
// ring, or drops to that lone timer's own hero if only one remains.
static void resolve_to_next_ringing(void) {
  AppState *app_state = app_context_app_state();
  int32_t next_cookie = nav_pick_ringing_cookie(app_state);
  set_alarm_target(app_state->ringing_count, next_cookie);
  render_stack();
}

// Arms the settling window on the app-initiated RESUME/restore transition
// too, symmetrically with
// set_alarm_target's arming on the app-initiated TAKE-OVER transition above.
// This is the sole nav.c funnel every alarm-resolve resume (HOME/HERO/
// ACTIONS/LAPS/SET/CLEAR_CONFIRM) converges through, so arming here covers
// all of them structurally, without a per-screen special case. It matters
// concretely for VD_CLEAR_CONFIRM: unlike every other resume target, its
// Select is a single, un-gated `window_single_click_subscribe` with no row
// to be mis-positioned on and no hold threshold to fail to reach -- a stray/
// carried-over Select landing in the instant the display flips back onto it
// would otherwise wipe the whole store with one press, no undo
// (screen_clear_confirm.c's select_click_handler now checks
// input_guard_blocking()). Harmless for every other resume target: nothing
// else currently consults input_guard_blocking() outside the alarm/ringing
// Back handlers and this new confirm-screen check, so arming it here doesn't
// swallow anything on HOME/HERO/ACTIONS/LAPS/SET.
static void resolve_to(ViewDesc target) {
  s_has_resume = false;
  s_current = target;
  input_guard_arm();
  render_stack();
}

void nav_alarm_resolved(bool ringing_now_empty) {
  if (!ringing_now_empty) {
    resolve_to_next_ringing();
    return;
  }
  resolve_to(nav_on_resolve(s_resume, s_has_resume, app_context_store()));
}

void nav_alarm_resolved_keep_counting(int32_t cookie, bool ringing_now_empty) {
  if (!ringing_now_empty) {
    resolve_to_next_ringing();
    return;
  }
  // Pass the captured resume through -- nav_keep_counting_target
  // itself decides whether to honor it (only when it's a SET session worth
  // protecting) or fall back to the just-converted stopwatch's own hero.
  ViewDesc target =
      nav_keep_counting_target(cookie, s_resume, s_has_resume, app_context_store());
  resolve_to(target);
}

ViewDesc nav_current(void) {
  return s_current;
}

void nav_exit_to_watchface(void) {
  // Back rule: "watchface (all keep ringing)" -- exits straight to the
  // watchface, leaving every ringing timer exactly as-is (fired=true, still
  // in AppState's ringing set; literal post-exit vibration is
  // unimplementable, so this only needs to leave state untouched, not
  // simulate ongoing alerts). A direct pop_all, NOT render_stack()'s diff --
  // this isn't a transition between two logical views, it's the app
  // terminating (pebble.h's own doc on window_stack_remove: "If there are no
  // windows for the app left on the stack, the app will be killed by the
  // system, shortly").
  //
  // This is therefore the ONE deliberate exception to render_stack()'s
  // "every real navigation goes
  // through this one place" invariant -- s_current/s_stack_bc are left
  // stale (still pointing at whatever was on top a moment ago) instead of
  // being updated to reflect the now-empty stack. That's safe ONLY because
  // the OS genuinely kills this process shortly after an empty window stack
  // (observed directly: on exit-then-relaunch, a fresh app_init:home
  // marker always follows, never a redraw against the stale state) -- if a
  // future change ever let the app survive past this call (a debug build
  // suppressing the kill, a platform change, etc.), s_current/s_stack_bc
  // would need reconciling here too, the same way every other nav_go_*/
  // nav_on_timer_fired/nav_alarm_resolved* caller gets via render_stack().
  window_stack_pop_all(false);
}

void nav_deinit(void) {
  window_stack_pop_all(false);
  input_guard_deinit();
  screen_voice_destroy();
  screen_preset_actions_destroy();
  screen_library_destroy();
  screen_clear_confirm_destroy();
  screen_ringing_destroy();
  screen_laps_destroy();
  screen_alarm_options_destroy();
  screen_alarm_destroy();
  screen_actions_destroy();
  screen_hero_destroy();
  screen_set_destroy();
  screen_home_destroy();
}
