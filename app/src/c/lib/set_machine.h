// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// Pure SET-spinner state machine (h:mm:ss cursor, field navigation). NEVER
// #include <pebble.h>. Re-derived for C from an earlier TypeScript
// implementation (no ms -- duration is computed straight into seconds).
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

// The dial's own hour-field ceiling (clamp_h in set_machine.c) -- named so
// lib/timer.h's TIMER_MAX_DURATION_S can derive its own cap from the SAME
// number instead of an independent magic constant that could silently drift
// out of sync with what this dial actually allows a user to create. (Note:
// TIMER_MAX_DURATION_S used to be a fresh, unrelated 24h constant while
// this dial has always allowed up to 99h, letting timer_add_quarter()
// saturate DOWN to 24h on a timer dialed above it.)
#define SET_MACHINE_MAX_HOURS 99

typedef enum {
  SET_FIELD_H,
  SET_FIELD_M,
  SET_FIELD_S,
} SetField;

typedef struct {
  int h;  // 0-99 (clamped)
  int m;  // 0-59 (wraps)
  int s;  // 0-59 (wraps)
  SetField field;
  bool is_repeat;  // RAW create-time repeat intent (gated by duration for
                    // effect -- see set_machine_is_repeat/_kind_toggle_available)
} SetState;

// One shared outcome enum for both field_right/field_left -- field_right only
// ever returns {MOVED, START}, field_left only ever returns {MOVED, CANCEL}.
typedef enum {
  SET_NAV_MOVED,
  SET_NAV_START,
  SET_NAV_CANCEL,
} SetNavResult;

// Cursor starts on minutes (field=SET_FIELD_M), 0:00:00 -- "most timers are
// minutes".
void set_machine_init(SetState *s);

// Edit interval -- primes h/m/s from an EXISTING duration (a repeat
// timer's current interval) instead of zeroing. Splits via format_split_hms
// (lib/format's shared flooring helper, so the rounding matches every other
// caller); h is clamped 0-99 the same as set_machine_adjust's own bound (a
// crafted-but-absurd duration must not corrupt the field). Cursor still
// starts on minutes -- same convention as a fresh create (set_machine_init),
// no reason to special-case edit mode here.
void set_machine_init_from_seconds(SetState *s, int32_t total_s);

// Like set_machine_init_from_seconds, but ALSO primes the raw create-time
// repeat intent -- so editing a saved repeat preset restores its indigo/toggle
// state instead of always landing on countdown (screen_set_start_preset_edit
// uses this; screen_set_start_edit's edit-INTERVAL mode does not -- that
// mode never shows/uses the kind toggle at all). Equivalent to calling
// set_machine_init_from_seconds(s, total_s) then setting s->is_repeat --
// still duration-gated at READ time by set_machine_is_repeat, so a 0:00
// total_s still reports effective repeat=false regardless of `is_repeat`
// (moot in practice: a legitimately-stored preset's duration_s is always
// >=1, lib/preset_store.h's own validation).
void set_machine_init_from_seconds_kind(SetState *s, int32_t total_s, bool is_repeat);

// +-1 on the active field. m/s wrap 0-59 (borrow/carry does NOT roll into the
// next field -- each field is independent; there is no auto-carry from
// m->h or s->m). h clamps 0-99 (no wrap).
void set_machine_adjust(SetState *s, int delta);

// h -> m -> s -> START. Mutates s->field when the result is SET_NAV_MOVED.
SetNavResult set_machine_field_right(SetState *s);

// s -> m -> h -> CANCEL. Mutates s->field when the result is SET_NAV_MOVED.
SetNavResult set_machine_field_left(SetState *s);

int32_t set_machine_duration_s(const SetState *s);
time_t set_machine_end_clock(const SetState *s, time_t now);

// Create-time kind toggle: the toggle is only available -- and repeat is
// only *effective* -- once a real duration is dialed. This unifies the
// "0:00 = stopwatch" shortcut with the toggle into one rule: at 0:00 there
// is no kind to toggle.
bool set_machine_kind_toggle_available(const SetState *s);  // == (duration_s(s) >= 1)

// Self-guarding: flips the RAW is_repeat intent ONLY when the toggle is
// available (a no-op at 0:00 -- nothing to toggle when the timer can only
// ever be a stopwatch).
void set_machine_toggle_kind(SetState *s);

// EFFECTIVE repeat = raw intent AND toggle available. Render + commit read
// this, never the raw `is_repeat` field directly, so a dial-down to 0:00
// hides the repeat state (without erasing it -- dialing back up to >= 1s
// restores it, the "forgiving, no-surprise" edge behavior).
bool set_machine_is_repeat(const SetState *s);

// duration_s == 0 -- "0:00 = a stopwatch". Since a repeat is never a
// stopwatch, at 0:00 effective repeat is always false (see
// set_machine_is_repeat), so this stays equivalent to plain duration_s(s) == 0
// whenever kind isn't repeat.
bool set_machine_is_stopwatch(const SetState *s);
