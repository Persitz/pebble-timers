// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// LIBRARY screen adapter: the preset list -- glyph+colour+duration rows
// (each row shows its preset's name, when set) + a "Save new" row, always
// last. Short Select on a preset LAUNCHES it (store_add + nav_go_hero, the
// exact create-flow path); hold Select opens the {Edit, Rename, Delete}
// actions sheet (screen_preset_actions.c), the surface for managing a
// preset's identity including its name; Back/Up-at-top return to HOME.
//
// Reachable from HOME's "Library" row via nav_init()/window_for_kind()/
// nav_go_library() wiring. See screen_library.c's own top-of-file comment
// for implementation history.
#pragma once

#include <pebble.h>

Window *screen_library_create(void);
void screen_library_destroy(void);

// Repaints the top-right clock strip (LIBRARY's header, live wall clock)
// against the current wall clock -- call from the tick handler while this
// window is on top (mirrors screen_actions_mark_clock_dirty's contract).
// Safe to call unconditionally even before this window is created
// (guards on its own cached layer pointer being non-NULL, same pattern
// main.c's redraw_visible_screens() already relies on for every screen).
void screen_library_mark_clock_dirty(void);

#ifdef GALLERY_MODE
// Debug-only accessor for the screen gallery (src/c/gallery.c) -- mirrors
// screen_home_debug_menu()'s own contract exactly (exposes the real
// MenuLayer so the gallery can force a reload/cursor-position, since
// consecutive gallery scenes targeting the same ViewDeskind are not always a
// real window-stack transition). Never compiled into a release build.
MenuLayer *screen_library_debug_menu(void);

// Debug function for gallery mode: forces the transient "hold a preset
// for options" hint state directly, so gallery scenes can show it
// deterministically without racing AppTimer. Never compiled into a release build.
void screen_library_debug_force_hint(bool active);
#endif
