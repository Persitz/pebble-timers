// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// Thin executor of lib/nav_policy's decisions -- owns the logical view model
// (mirrored to window_stack_push/pop on the pre-created, once-only Windows)
// and reconstructs the exact stack for every transition rather than
// relying on raw push/pop semantics to "reveal the right thing". No policy
// decisions live here -- those are lib/nav_policy's, called from here.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lib/nav_policy.h"

void nav_init(void);
void nav_deinit(void);  // destroys all screens -- call once at app exit

void nav_go_home(void);

// Like nav_go_home(), but remembers `cookie` (the hero being left) so HOME's
// own window_appear can land the cursor on that timer's row instead of always
// resetting to row 0 -- see nav_take_home_focus_cookie() below. The only
// call site is screen_hero.c's own Back handler; every other nav_go_home()
// caller (Library Back, Clear-all confirm/cancel, SET cancel, ...) keeps the
// plain function, which explicitly clears any pending focus cookie so a
// later, unrelated HOME appearance never reuses a stale one.
void nav_go_home_from_hero(int32_t cookie);

// One-shot read-and-clear: returns the cookie set by the most recent
// nav_go_home_from_hero() call, or -1 if none is pending (the plain
// nav_go_home() case, or already consumed by an earlier window_appear).
// screen_home.c's window_appear is the only intended caller -- consuming it
// here (not peeking) means a SECOND, unrelated HOME appearance can never
// accidentally reapply a stale focus target.
int32_t nav_take_home_focus_cookie(void);

// Sentinel value nav_take_home_focus_cookie() can return besides -1 (no
// pending focus) or a real timer cookie (store.c's next_cookie starts at 1,
// never zero/negative) -- signals "land the cursor on the 'Say a timer' row"
// rather than a specific timer's row. Set by
// nav_go_home_focus_voice_row() below; consumed by screen_home.c's
// home_refresh_appearance() using the same one-shot read-and-clear mechanism
// as hero-focus cookies.
#define NAV_HOME_FOCUS_VOICE_ROW (-2)

// Like nav_go_home(), but primes the cursor to land on the "Say a timer" row
// instead of row 0 -- fixing cursor position after cancelling from the voice
// arc. Call sites: dictation_am.c's VOICE_CANCELLED routing (Back from the
// system dictation UI) and screen_voice.c's own Back/Up-at-row-0 handlers
// (Back from the confirm/edit-preview/error screen). Creating a timer via
// voice uses nav_go_hero(cookie) directly, never this function.
void nav_go_home_focus_voice_row(void);

void nav_go_set(void);
void nav_go_hero(int32_t cookie);
void nav_go_actions(int32_t cookie);
void nav_go_alarm_options(int32_t cookie);
void nav_back_from_alarm_options(void);  // returns to the alarm hero for the same cookie
void nav_go_laps(int32_t cookie);        // Opens the laps list, reachable from ACTIONS only

// Opens HOME's "Clear all N timers?" confirm screen, reached only from
// HOME's "Clear all" row. The confirm screen itself calls nav_go_home() on
// both outcomes (Select confirms + clears, Back cancels) -- same shape as
// ACTIONS' Delete rows.
void nav_go_clear_confirm(void);

// Opens the SET spinner pre-filled with `cookie`'s current interval
// (screen_set.c's screen_set_start_edit primes the fields once, here,
// mirroring nav_go_set()'s screen_set_reset() priming -- NOT via apply_cookie,
// so an alarm take-over mid-edit leaves the dialed state untouched on return,
// same guarantee create-mode's dial already has). Select-through-to-START
// commits via lib/timer's timer_edit_interval instead of creating a new timer;
// Back-cancel returns to ACTIONS (screen_set.c's own edit-mode branch). Called
// by ACTIONS' own "Edit interval" row.
void nav_go_edit_interval(int32_t cookie);

// Opens the Library screen (VD_LIBRARY) -- reached from HOME's "Library" row,
// and the common cancel/save-return target for the preset-actions sheet and
// both preset SET dials.
void nav_go_library(void);

// Mirrors the one-shot read-and-clear mechanism for the home-focus cookie,
// but targets LIBRARY's row cursor (Library's rows are index-addressed, not
// cookie-addressed) -- lands the cursor on a specific row instead of the
// default row 0/top every OTHER nav_go_library() caller keeps. Two call sites:
// screen_voice.c's do_save() (cursor on the freshly-saved preset's own row)
// and its cancel-to-launcher path for a preset-target session (cursor back on
// "Say a preset").
void nav_go_library_focus_row(int row);

// One-shot read-and-clear: returns the row set by the most recent
// nav_go_library_focus_row() call, or -1 if none is pending (the plain
// nav_go_library() case, or already consumed by an earlier window_appear).
// screen_library.c's window_appear is the only intended caller.
int nav_take_library_focus_row(void);

// Opens SET fresh (screen_set_start_preset_new) to dial a brand-new preset --
// Select-through-to-START saves it (does not start a timer). Reached from
// screen_library.c's "Save new" row.
void nav_go_preset_new(void);

// Opens SET pre-populated from the preset at `index` (screen_set_start_preset_edit)
// -- Select-through-to-START UPDATES that preset in place. Reached from the
// preset-actions sheet's "Edit" row.
void nav_go_preset_edit(int index);

// Opens the {Edit, Delete} preset-actions sheet (VD_PRESET_ACTIONS, `index`
// carried as ViewDesc.cookie) for the preset at `index` -- reached via
// hold-Select on a Library row.
void nav_go_preset_actions(int index);

// Opens the voice confirm/edit preview (VD_VOICE) -- reached from HOME's
// "Say a timer" row. Primes with a safe placeholder BEFORE the push
// (mirroring nav_go_preset_new()'s own "prime, then switch" shape), then
// starts the REAL dictation attempt (dictation_am_start(DICT_SCOPE_FULL))
// AFTER render_stack() -- see nav_go_voice()'s own comment in nav.c for why
// that split/ordering matters and the cancellation bug it prevents.
void nav_go_voice(void);

// Like nav_go_voice() above but for preset target -- primes screen_voice.c's
// target to VOICE_TARGET_PRESET first (screen_voice_set_target()) so the
// confirm screen's primary action reads "Save" (not "Start"), Save writes into
// the PresetStore instead of the TimerStore, and every cancel/dial-escape path
// returns to LIBRARY instead of HOME. Reached from screen_library.c's own
// "Say a preset" row.
void nav_go_voice_preset(void);

// Opens SET fresh but primed from a voice parse's {duration_s, is_repeat} --
// screen_voice.c's "Edit on dial" action, reusing set_machine_init_from_seconds_kind
// via screen_set_start_from_voice (mirrors nav_go_preset_edit's own "prime SET's
// adapter state before the push" shape). Select-through-to-START commits via the
// normal CREATE path (start_timer -> nav_go_hero), same as nav_go_set()'s plain
// dial -- this is NOT a new SetMode, just an alternate initializer for
// SET_MODE_CREATE.
void nav_go_set_from_voice(int32_t duration_s, bool is_repeat);

// The preset-target sibling of nav_go_set_from_voice() -- primes SET into
// SET_MODE_PRESET_NEW (screen_set_start_preset_new_from_voice) from a parsed
// {duration_s, is_repeat} instead of SET_MODE_CREATE. Select-through-to-START
// commits via commit_preset_new() (appends to the PresetStore, never the
// TimerStore); Back-cancel already returns to LIBRARY (screen_set.c's existing
// SET_MODE_PRESET_NEW case, unchanged). Called by screen_voice.c's "Edit on
// dial" row when the current session's target is VOICE_TARGET_PRESET.
void nav_go_preset_new_from_voice(int32_t duration_s, bool is_repeat);

// Opens the specific alarm's own hero from a ringing-list row. Does not
// disturb the pre-emption target captured at the first fire.
void nav_go_alarm(int32_t cookie);

// Exits the app straight to the watchface, leaving every ringing timer
// exactly as-is (all keep ringing).
void nav_exit_to_watchface(void);

// A timer just fired. Applies the take-over decision and switches to the
// alarm view for `cookie` (or the ringing list if >=2 timers are now
// simultaneously ringing).
void nav_on_timer_fired(int32_t cookie);

// Call after a ringing-set mutation (Done/snooze/stack-clear/repeat's "Keep
// going"). If the ringing set is now empty, resolves `resume` (dead-reference
// guard) and navigates there; otherwise stays on the alarm view (showing the
// next ringing timer).
void nav_alarm_resolved(bool ringing_now_empty);

// Call after a one-off alarm resolves via "Keep counting" specifically
// (timer_keep_counting -> stopwatch), instead of nav_alarm_resolved. If
// ringing is still non-empty, behaves identically (showing the next ringing
// timer takes priority over the just-converted stopwatch). If ringing is now
// empty, navigates to `cookie`'s own hero (nav_keep_counting_target) instead
// of the generic pre-emption `resume`. Exception: if the pre-empted resume
// was an in-progress SET session, nav_keep_counting_target restores THAT
// instead, since it's the only resume target losable forever rather than
// merely deferred.
void nav_alarm_resolved_keep_counting(int32_t cookie, bool ringing_now_empty);

ViewDesc nav_current(void);
