// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// LAPS screen adapter: the stopwatch laps list, reachable only from its
// timer's ACTIONS ("Laps (N)" row). Kept intentionally minimal: a MenuLayer
// list, Up/Down scroll (native), Back -> actions, Select is a no-op.
#pragma once

#include <pebble.h>
#include <stdint.h>

Window *screen_laps_create(void);
void screen_laps_destroy(void);
void screen_laps_set_cookie(int32_t cookie);

// Repaints the top-right clock strip (call from the tick handler while this
// window is on top).
void screen_laps_mark_clock_dirty(void);
