// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// Hold-to-apply mechanism: a raw-click down starts a 250ms fill-redraw
// AppTimer plus a 700ms fire AppTimer; a raw-click up before 700ms cancels
// both. This is the primary hold mechanism, deliberately not native
// long-click, because it's the only primitive that gives both a press-start
// event (needed to drive the fill bar) and a cancellable pending timer (the
// phantom-fire guard the interruption model needs -- nav take-over and
// focus-loss both call hold_cancel()).
//
// The same raw-click + AppTimer primitive also drives SET auto-repeat, via
// the pure lib/set_accel decision, rather than a separate
// window_single_repeating_click_subscribe wiring -- one fewer distinct
// click-subscription mechanism to reason about.
//
// The mechanism is generalized from a SELECT-only version to a
// button-parameterized one (hold_bind_button) so every hero hold (stopwatch
// pause/lap, countdown/repeat Up-hold, alarm snooze-1/4, ...) can share it
// instead of each reinventing a raw-click+AppTimer path. hold_bind_select is
// a thin wrapper over hold_bind_button (BUTTON_ID_SELECT, on_short=NULL,
// hold_ms=HOLD_MS), so existing callers (hero pause/resume, alarm
// hold-to-resolve) see no behavior change. Only one button's hold can be in
// progress at a time (a user physically holds one button), so the fire/fill
// AppTimer state stays a single shared "active channel" claimed by whichever
// bound button is currently pressed (identified via
// click_recognizer_get_button_id in the raw-click handlers) -- see hold.c's
// per-button binding table.
//
// Every elapsed-time decision in this file (the SET auto-repeat accel curve,
// the hold-to-apply threshold-on-release check) is derived from a monotonic
// AppTimer poll count, never from Pebble's wall-clock time_ms(). That clock
// was found to be non-monotonic on this platform -- an ordinary ~100-200ms
// tap was observed with an elapsed reading that jumped to 1100ms between two
// 100ms-apart polls, and separately to -800ms -- and using it for these
// decisions produced both a SET-digit overshoot (8 taps registering as 15)
// and a phantom hold-fire that deleted a timer from an ordinary short tap.
// now_ms()/time_ms() are still used, but only for the "is a hold currently
// in progress" nonzero/zero sentinel (s_held_since_ms) -- a value that is
// merely compared to 0, never differenced -- so a clock jump can no longer
// corrupt a decision.
#pragma once

#include <pebble.h>

#define HOLD_MS 700       // hold-to-apply threshold
#define HOLD_LONG_MS 1200 // countdown add-¼ "longer hold + preview" threshold (heavier commit)

typedef void (*HoldFireCallback)(void);

// The generalized hold-to-apply mechanism: wires raw-click `button` on
// `window` to the hold primitive. `on_short` (may be NULL) runs once, on an
// early release -- i.e. the button was tapped and let go before `hold_ms`
// elapsed (the stopwatch Up short-press hint is the first real consumer).
// `on_long` runs once the hold crosses `hold_ms` (either via the fire
// AppTimer or -- deterministically -- an up event whose
// measured duration already exceeds `hold_ms`, measured via the monotonic
// fill-poll count above, not wall-clock ms); this is the same "hold-to-apply"
// action `hold_bind_select`'s `on_fire` always was. Must be called from
// inside `window`'s ClickConfigProvider (SDK requirement for
// window_raw_click_subscribe). Binding a second button (e.g. both SELECT and
// UP) on the same window is supported -- each button gets its own click
// subscription; only one can be physically held at a time, so they share one
// underlying fire/fill AppTimer channel (hold.c's "active" button). If a
// SECOND bound button is pressed while the FIRST is still physically held
// (both buttons genuinely down at once -- plausible for adjacent SELECT/UP),
// the first hold is silently interrupted with no callback (dropped, one
// action at a time) and the channel is reassigned to the second button; the
// first button's eventual release is then a no-op (`raw_up_handler` checks the
// releasing button against the channel's current owner before evaluating/
// firing anything, so an interrupted button's stale release can never
// misattribute onto the button that now owns the channel).
void hold_bind_button(Window *window, ButtonId button, HoldFireCallback on_short,
                       HoldFireCallback on_long, uint32_t hold_ms);

// hold_bind_select(win, cb) === hold_bind_button(win, BUTTON_ID_SELECT, NULL, cb, HOLD_MS) --
// kept as a thin wrapper so no existing caller (hero pause/resume, alarm
// hold-to-resolve) needs to change; behavior is byte-identical to before this
// generalization.
void hold_bind_select(Window *window, HoldFireCallback on_fire);

// The button whose hold is currently in progress, or NUM_BUTTONS (the SDK's
// own "count" sentinel, never itself a real button) when idle. Screens with
// more than one hold-bound button (stopwatch: SELECT + UP) read this to pick
// which hold's footer label/fill to draw.
ButtonId hold_active_button(void);

// Cancels any pending hold-to-apply Select gesture AND any in-progress SET
// Up/Down auto-repeat, WITHOUT firing/applying either -- the phantom-fire/
// phantom-step guard for when navigation takes over the screen mid-hold
// (leaving the SET spinner's own auto-repeat cancelled too) and for
// focus-loss (willFocus(false)). Safe to call when neither is in progress
// (no-op).
void hold_cancel(void);

// 0-1000 per-mille fill fraction of the currently in-progress hold (0 if
// none) -- screens read this directly in their update_proc to draw the fill
// bar. Per-mille (RENDER_FRACTION_SCALE, render.h), not float -- removing the
// app's only float usage lets the libgcc softfloat helpers drop out of the
// link. `int32_t`, matching every render.h signature that consumes this same
// value (was plain `int`, a no-op difference on this 32-bit target but an
// avoidable type mismatch across the two files that jointly own this value's
// contract).
int32_t hold_progress(void);

bool hold_is_active(void);

// SET auto-repeat: wires native repeating-click on Up/Down for `window`.
// `on_step` is called with the signed delta (+N for Up, -N for Down) each
// time lib/set_accel decides more steps are due. Must be called from inside
// `window`'s ClickConfigProvider.
typedef void (*SetStepCallback)(int delta);
void hold_bind_set_repeat(Window *window, SetStepCallback on_step);

#ifdef GALLERY_MODE
// Debug-only override for the screen gallery (src/c/gallery.c): forces
// hold_is_active()/hold_progress() to report a fixed per-mille fraction
// (0-1000) WITHOUT any real AppTimer/click involved -- lets a gallery scene
// depict "mid-hold, N% filled" as a static, reproducible screenshot rather
// than needing a precisely-timed real button hold. Pass fraction_pm < 0 to
// clear the override (hold_is_active()/hold_progress() go back to
// reflecting the real raw-click state). Never compiled into a release
// build. `int32_t` (see hold_progress()'s doc comment).
void hold_debug_force(int32_t fraction_pm);

// Pairs with hold_debug_force() above -- a
// forced fraction alone makes hold_is_active()/hold_progress() report "some
// hold is in progress," but hold_active_button() still reads the real (idle)
// active-hold state, since no real raw-click ever ran under a forced
// fraction. A scene depicting the stopwatch's Up-hold (vs. its Select-hold)
// needs hold_active_button() to report BUTTON_ID_UP too, so its footer label
// picks the right text. Defaults to BUTTON_ID_SELECT (gallery_reset_fixture()
// resets it back to that between scenes); only scenes depicting a held UP
// need to call this. Never compiled into a release build.
void hold_debug_force_button(ButtonId button);
#endif
