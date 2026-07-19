// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// HERO screen adapter: the running/paused countdown, OVERTIME (fired,
// silenced), plain stopwatch, and post-timer stopwatch hero (its own branch
// -- see lib/view.h's ViewHeroBranch doc). Same window reused for all
// branches, dispatched via lib/view's ViewHeroBranch (never a kind-agnostic
// `fired` read).
#pragma once

#include <pebble.h>
#include <stdint.h>

Window *screen_hero_create(void);
void screen_hero_destroy(void);

// Which timer this window currently shows -- set by nav.c before pushing.
void screen_hero_set_cookie(int32_t cookie);

// Repaints against the current wall clock/timer state (call from the tick
// handler while the hero window is on top).
void screen_hero_mark_dirty(void);

#ifdef GALLERY_MODE
// Debug-only entry point for the screen gallery: forces the transient
// short-Up-press footer hint ("Hold to add lap") on/off WITHOUT running the
// real ~2s AppTimer, so a gallery scene can depict it as a static,
// reproducible screenshot. Mirrors hold_debug_force()'s own "force the
// visual state, skip the real timer" shape (hold.h). Never compiled into a
// release build.
void screen_hero_debug_force_up_hint(bool active);

#endif
