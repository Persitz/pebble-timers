// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// A general, reusable input guard -- after any APP-INITIATED screen
// change, swallow destructive discrete presses (Back) for a short settling
// window, so a Back the user had already committed to for the PREVIOUS
// screen can't land on a freshly app-popped screen and act on it. The
// alarm hero's Back is destructive (store_remove), which creates this
// hazard.
//
// Monotonic, AppTimer-based -- mirrors hold.h/hold.c's design: every
// elapsed-time decision in this codebase comes from an AppTimer expiry/poll
// count, never from differencing two time_ms() reads (proven non-monotonic
// in this environment). The guard needs exactly one such decision -- "has the
// settling window elapsed since the most recent arm?" -- so a single
// AppTimer expiry callback flipping a boolean is the whole mechanism; no
// fill-poll counting needed (unlike hold.c, this has no fill-bar UI to
// redraw mid-window).
#pragma once

#include <pebble.h>

// Settling window (ms) after an app-initiated screen change during which
// destructive discrete presses (Back) are swallowed. Range considered was
// 300-500ms; must stay well under hold.c's HOLD_MS (700ms).
#define INPUT_GUARD_MS 350

// Arm the settling window. Call at EVERY app-initiated screen change
// (never from a user-initiated nav) -- nav.c has two armers:
// set_alarm_target() (the app-initiated alarm TAKE-OVER transition) and
// resolve_to() (the app-initiated alarm-resolve RESUME/restore transition;
// see resolve_to's own comment for why arming on resume is safe everywhere
// else). Idempotent / re-armable: re-arming restarts the window and cancels
// any pending expiry, so a second app-initiated transition inside an
// already-open window extends it rather than expiring early.
void input_guard_arm(void);

// True while inside the settling window after the most recent arm.
// Destructive input handlers (screen_alarm.c's hero_back_click_handler,
// screen_ringing.c's back_click_handler) check this and early-return when
// true.
bool input_guard_blocking(void);

// Cancels any pending guard timer and clears the blocking state. Called from
// app deinit for tidiness (the OS frees AppTimers on app exit regardless).
void input_guard_deinit(void);
