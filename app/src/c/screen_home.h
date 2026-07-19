// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// HOME screen adapter (the only tier that #includes <pebble.h> for this
// screen). Started as an empty state (wall clock + centred [+] "New timer"
// affordance), then a minimal populated-row list (Up/Down cursor + Select),
// then a real MenuLayer + custom draw_row: rounded pills (dark inset cursor
// / white "quiet-card" unselected), kind glyph + colour, per-count value
// sizing, and a thin scroll-indicator overlay Layer -- native scrolling/
// cursor/select/highlight, no more hand-rolled cursor math.
#pragma once

#include <pebble.h>

// Creates the HOME window (once -- windows are created once at app_init
// and reused, never destroyed/recreated across navigation).
Window *screen_home_create(void);

// Destroys the HOME window and its layers. Call once at app exit.
void screen_home_destroy(void);

// Marks the HOME screen's clock dirty so it repaints against the current
// wall-clock time (call from the tick handler).
void screen_home_mark_clock_dirty(void);

#ifdef GALLERY_MODE
// Debug-only accessor for the screen gallery (src/c/gallery.c): exposes the
// real MenuLayer so the gallery can force a reload (consecutive gallery
// scenes may both target HOME on a freshly-rebuilt fixture, which is NOT a
// real window-stack transition, so window_appear -- the normal reload
// trigger -- never fires) and set the cursor to a specific row (scroll/
// cursor-position variants). Never compiled into a release build.
MenuLayer *screen_home_debug_menu(void);

// Debug-only entry point: re-runs window_appear()'s reload/cursor-reset
// OUTSIDE of a real window-stack transition -- see screen_home.c's
// home_refresh_appearance() doc comment for why this is necessary
// (consecutive gallery scenes can both target HOME without it ever leaving
// the top of the stack). Never compiled into a release build.
void screen_home_debug_refresh(void);
#endif
