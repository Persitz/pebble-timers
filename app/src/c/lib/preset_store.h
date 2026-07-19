// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// Pure fixed-capacity preset (saved timer template) collection: init/add/
// remove/edit/get. NEVER #include <pebble.h>. No malloc anywhere (static
// allocation discipline) -- the store is a static array, sized MAX_PRESETS.
// Separate from lib/store.h's TimerStore by design: a template must never enter
// store_home_order, wakeup_adapter_replan, MAX_TIMERS, ringing, or the interruption
// model -- this file has zero dependency on TimerStore and no function here ever
// touches one.
//
// Presets are index-addressed, not cookie-addressed like Timer -- templates
// need no cross-session stable identity (no wakeup, no cross-reference from
// another persisted structure), unlike timers.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "timer.h"

// 12 keeps the Library list scannable and keeps the whole PresetStore blob
// comfortably under PERSIST_DATA_MAX_LENGTH (256B) -- see preset_serialize.h for
// the wire-size math.
#define MAX_PRESETS 12

// Presets serialize into ONE 256-byte persist key (PERSIST_DATA_MAX_LENGTH),
// so Preset.name is capped shorter than Timer.name's 24 -- solving
// `2 + MAX_PRESETS*(5+K) <= 256` for K gives K <= 16.16, so K=16 (worst case
// 2 + 12*21 = 254B, a 2-byte margin). This also guarantees Preset.name always
// fits in Timer.name (24) unchanged for launch inheritance. See preset_serialize.h
// for the full wire-size derivation.
#define PRESET_NAME_MAX 16

typedef struct {
  TimerKind kind;        // KIND_COUNTDOWN | KIND_REPEAT only -- never KIND_STOPWATCH
                          // (a plain stopwatch has no duration to template).
  int32_t duration_s;    // 1 .. TIMER_MAX_DURATION_S, same domain SET dials.
  char name[PRESET_NAME_MAX];  // display-only; "" = unnamed (the degrade
                                // default). NOT part of preset_fields_valid --
                                // an empty name is always valid.
} Preset;

typedef struct {
  Preset presets[MAX_PRESETS];
  int count;
} PresetStore;

// Shared {kind, duration_s} validation predicate -- preset_store_add/edit
// below AND lib/preset_serialize.c's per-record deserialize loop both call this
// one definition to ensure consistency. `kind` must be countdown/repeat (never
// stopwatch -- a plain stopwatch has no duration to template); `duration_s` must
// be in `1 .. TIMER_MAX_DURATION_S` (same domain SET's own dial clamps to).
bool preset_fields_valid(TimerKind kind, int32_t duration_s);

void preset_store_init(PresetStore *s);

// Appends a new preset. Refuses (returns false, no-op) if the store is
// already at MAX_PRESETS, or if `{kind, duration_s}` fails
// preset_fields_valid (not countdown/repeat, or duration_s outside
// `1 .. TIMER_MAX_DURATION_S`); see preset_fields_valid's own doc comment
// above for the full domain.
bool preset_store_add(PresetStore *s, TimerKind kind, int32_t duration_s);

// Removes the preset at `index` (compacts the array). Returns false (no-op)
// if `index` is out of range ([0, count)).
bool preset_store_remove(PresetStore *s, int index);

// Overwrites the preset at `index` in place (no reorder). Returns false
// (no-op) if `index` is out of range or `{kind, duration_s}` fails
// preset_fields_valid -- same validation as preset_store_add, including the
// upper bound, not just `duration_s < 1`.
bool preset_store_edit(PresetStore *s, int index, TimerKind kind, int32_t duration_s);

// Returns a pointer into the store's own array (valid until the next
// add/remove/edit), or NULL if `index` is out of range.
const Preset *preset_store_get(const PresetStore *s, int index);

// Sets/clears the name at `index` (index-addressed, mirroring the rest of
// PresetStore). Routes through timer_name_copy() (lib/timer.h -- the same
// trim + UTF-8-safe cap primitive store_set_name() uses for Timer.name, now
// shared rather than duplicated) capped to PRESET_NAME_MAX. ""/whitespace-
// only clears back to unnamed. Returns false (no-op) on out-of-range index.
// Preset kind/duration_s are untouched -- name is not part of
// preset_fields_valid.
bool preset_store_set_name(PresetStore *s, int index, const char *name);
