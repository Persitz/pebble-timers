// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// SET screen adapter: h:mm:ss spinner. Up/Down = +-1 on
// the active field (native repeating-click + pure lib/set_accel);
// Select = field right (h->m->s->start); Back = field left (->cancel).
#pragma once

#include <pebble.h>
#include <stdint.h>

#include "lib/timer.h"

Window *screen_set_create(void);
void screen_set_destroy(void);

// Re-initializes the spinner to h:mm:ss = 0:00:00, cursor on minutes --
// called by nav.c every time SET is (re-)entered from HOME (create mode).
void screen_set_reset(void);

// Edit interval: re-initializes the spinner from `cookie`'s CURRENT duration_s
// instead of zeroing, and switches SET into edit mode -- Select-through-to-
// START commits via lib/timer's timer_edit_interval (mutating that specific
// timer) instead of creating a new one; Back-cancel returns to ACTIONS
// instead of HOME. Called by nav.c's nav_go_edit_interval, the ACTIONS
// "Edit interval" row's entry point (the repeat hero's own Up-hold gesture
// triggers Restart instead, not this edit mode).
void screen_set_start_edit(int32_t cookie);

// Presets: opens SET fresh
// (countdown default) to dial a brand-new preset's kind+duration -- Select-
// through-to-START commits the new preset (does NOT start a timer) instead
// of the create-mode timer commit. Called by nav.c's nav_go_preset_new()
// (screen_library.c's "Save new" row).
void screen_set_start_preset_new(void);

// Presets: re-initializes the spinner from an EXISTING preset's {kind,
// duration_s} at `index` (presets are index-addressed, not cookie-addressed)
// -- Select-through-to-START UPDATES that preset in place (does NOT start a
// timer) instead of creating/editing a live timer. Called by nav.c's
// nav_go_preset_edit(index) (the preset actions sheet's "Edit" row).
void screen_set_start_preset_edit(int index);

// Re-initializes the spinner from a
// voice parse's {duration_s, is_repeat} via set_machine_init_from_seconds_kind
// (the SAME helper screen_set_start_preset_edit uses above) -- Select-
// through-to-START commits via the normal CREATE path (start_timer ->
// nav_go_hero), NOT a preset/edit commit; this is an alternate initializer
// for SET_MODE_CREATE, not a new mode. Called by nav.c's
// nav_go_set_from_voice(), screen_voice.c's "Edit on dial" action
// -- the primed counterpart to nav_go_set()'s plain, unprimed dial.
void screen_set_start_from_voice(int32_t duration_s, bool is_repeat);

// Voice-into-Library: the preset-target sibling
// of screen_set_start_from_voice() above -- primes the SAME set_machine_init_
// from_seconds_kind seed, but switches SET into SET_MODE_PRESET_NEW (mirroring
// screen_set_start_preset_new()'s own mode-entry shape) instead of leaving it
// in SET_MODE_CREATE -- Select-through-to-START commits via commit_preset_new()
// (appends to the PresetStore), and Back-cancel already returns to LIBRARY
// (the existing SET_MODE_PRESET_NEW case in back_click_handler, unchanged).
// Called by nav.c's nav_go_preset_new_from_voice(), screen_voice.c's "Edit on
// dial" row when the current session's target is VOICE_TARGET_PRESET.
void screen_set_start_preset_new_from_voice(int32_t duration_s, bool is_repeat);

#ifdef GALLERY_MODE
// Debug-only entry point for the screen gallery (src/c/gallery.c): resets
// the spinner (screen_set_reset()'s real init) then drives it to an exact
// h:mm:ss via repeated real set_machine_adjust() calls (the SAME pure lib
// function Up/Down presses call) -- never a hand-set struct field -- so a
// gallery scene can land on a design-matched duration (e.g. "0:12:00")
// deterministically instead of scripting dozens of real button presses.
// Leaves the cursor on the minutes field, matching set_machine_init's own
// default (minutes is the field a fresh countdown timer is most likely to
// need adjusted). Never compiled into a release build.
void screen_set_debug_dial(int h, int m, int s);

// Debug-only entry point for the screen gallery
// -- drives the REAL kind toggle (set_machine_toggle_kind), not a debug
// variable, so gallery scenes render through the shipped state machine. See
// screen_set.c's own doc comment on this function for the full contract.
// Never compiled into a release build.
void screen_set_debug_kind(int h, int m, int s, bool is_repeat);
#endif
