// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// ACTIONS screen adapter: per-kind action list opened via Down from a hero.
// Built with countdown action set (Pause/Resume, +1:00, Restart, Delete) via
// a MenuLayer with the SDK's default menu_cell_basic_draw rows. Stopwatch
// includes Lap + "Laps (N)" entry point. Includes top-right clock strip.
// Custom-cell pixel fidelity (rounded pills, etc.) is deferred; ACTIONS keeps
// the SDK's default menu_cell_basic_draw rows.
#pragma once

#include <pebble.h>
#include <stdint.h>

Window *screen_actions_create(void);
void screen_actions_destroy(void);
void screen_actions_set_cookie(int32_t cookie);

// Applies the provided name (taken VERBATIM, no parse_nl pass -- same
// contract as screen_voice_set_name()/screen_preset_actions_apply_name()) to
// the ACTIONS menu's current cookie. Calls store_set_name() +
// persist_and_replan() + a menu reload. The name shows on HOME screen
// too. Called by dictation_am.c's deliver() on a
// successful DICT_SCOPE_ACTIONS_NAME re-dictation. Works for both named
// (replaces) and unnamed (sets) timers, every kind (store_set_name has no
// kind gate).
void screen_actions_apply_name(const char *name);

// Repaints the top-right clock strip against the current wall clock (call
// from the tick handler while this window is on top).
void screen_actions_mark_clock_dirty(void);

// Arm a one-shot flag for held-Down carry-through: call from a HERO's
// down_click_handler immediately before nav_go_actions(). This window's
// window_appear consumes the flag on its next appearance (starting an
// auto-scroll AppTimer if the physical Down press is still held).
void screen_actions_arm_carry_scroll(void);

// Cancels any in-progress carry-through auto-scroll AppTimer (no-op if
// none). Called structurally from nav.c's render_stack() on every real
// navigation transition -- the choke point every exit from ACTIONS goes
// through, mirroring hold_cancel()'s structural per-transition cancel.
void screen_actions_cancel_carry_scroll(void);

#ifdef GALLERY_MODE
// Debug-only accessor for the screen gallery -- see screen_actions.c's doc
// comment on the definition. Never compiled into a release build.
MenuLayer *screen_actions_debug_menu(void);
#endif
