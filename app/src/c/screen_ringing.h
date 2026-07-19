// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// Multi-alarm ringing list: shown instead of a single alarm hero once >=2
// timers are simultaneously ringing (several firing at once stack into a
// ringing list, like HOME, in red). Routing between this screen and the
// single-hero VD_ALARM view is a pure nav_policy decision (nav_alarm_target);
// this file only draws the list and reacts to its own buttons. Up/Down move
// the cursor, Select opens that row's own alarm hero (nav_go_alarm), Back
// exits straight to the watchface leaving every ringing timer exactly as-is
// (nav_exit_to_watchface).
#pragma once

#include <pebble.h>

// Creates the ringing-list window (once -- same allocate-once discipline as
// every other screen).
Window *screen_ringing_create(void);

// Destroys the ringing-list window. Call once at app exit.
void screen_ringing_destroy(void);

// Re-syncs the row count/content from AppState and repaints (call from the
// tick handler -- matches every other screen's "cheap enough to always mark
// dirty" pattern, extended here to also menu_layer_reload_data() every tick
// since ringing membership can grow while this screen is already on top,
// with no window-stack transition to hook a reload into otherwise -- see
// nav.c's nav_on_timer_fired "already on the ringing list" branch).
void screen_ringing_mark_dirty(void);
