// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// Screen gallery: the #ifdef-gated, fast inner-loop visual-verification
// tool -- fixture-controlled, one emulator boot that navigates through
// every screen state. A complementary outer loop drives the app via real
// button/touch navigation on stock firmware to prove each screen is
// actually reachable, rather than relying on this gallery's fixture
// shortcuts.
//
// Compiled in ONLY when GALLERY_MODE is defined (app/scripts/build-gallery.sh
// / `PEBBLE_GALLERY_MODE=1 pebble build`, see wscript's build()) -- NEVER
// part of a release build. When GALLERY_MODE is undefined this header is an
// empty translation unit (nothing declared), so an accidental unguarded call
// site anywhere else in the app fails to compile rather than silently
// linking against a stub.
//
// What it does: boots straight into a fixed, ordered list of ~20 SCENES, one
// per canonical UI state the app can be in (+ a few bonus variants -- paused,
// mid-hold-fill, cursor position). Each scene's fixture is built ONLY through
// the real lib/ mutation functions this app already ships (store_add,
// timer_pause, timer_keep_counting, timer_advance_cycle, app_state_scan_
// firings, ...) -- see gallery.c's own scene functions, never a hand-set
// Timer/TimerStore struct field. Up/Down cycle through scenes (wrapping);
// Select jumps forward one scene too (fast Down-equivalent, kept simple);
// Back jumps to scene 0. This is achieved by swapping every real screen's
// click_config_provider for gallery_click_config_provider below (see e.g.
// screen_hero.c's screen_hero_create()) -- the real screens are reused
// UNMODIFIED for rendering; only input ownership changes.
#pragma once

#ifdef GALLERY_MODE

#include <pebble.h>

// Builds scene 0's fixture and navigates to it. Call once from main.c's
// init(), after nav_init() (every real screen window must already exist).
void gallery_init(void);

// The click_config_provider every real screen's window is redirected to
// under GALLERY_MODE: Up = previous scene, Down/Select = next scene, Back =
// jump to scene 0 (all wrapping/clamping via gallery.c's own scene table).
void gallery_click_config_provider(void *context);

#endif  // GALLERY_MODE
