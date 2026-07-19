// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// Alarm screen adapter: renders the alarm hero (fired + ringing) and its
// collapsed options list, opened via the Down button.
//
// Button behavior:
//   one-off alarms: Select = Done (hold-to-confirm), Back = dismiss,
//                   Down = open options
//   repeat alarms:  Select = keep going, Back = dismiss (Stop)
#pragma once

#include <pebble.h>
#include <stdint.h>

Window *screen_alarm_create(void);          // the alarm hero
Window *screen_alarm_options_create(void);   // the collapsed options list
void screen_alarm_destroy(void);
void screen_alarm_options_destroy(void);

// Shared cookie for both windows -- only one alarm is ever visible at a time
// currently (multi-alarm stacking UI is a possible future enhancement).
void screen_alarm_set_cookie(int32_t cookie);

// Repaints the alarm hero against the current wall clock/overrun (call from
// the tick handler while the alarm window is on top).
void screen_alarm_mark_dirty(void);

// Repaints the options window's clock strip (call from the tick
// handler while the options window is on top).
void screen_alarm_options_mark_clock_dirty(void);
