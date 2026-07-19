// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// Pure fixed-capacity timer collection: add/remove/find + the 3-group home
// ordering. NEVER #include <pebble.h>. No malloc/realloc anywhere -- the store is a static array, sized MAX_TIMERS.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "timer.h"

// MAX_TIMERS rationale: 8 background
// wakeup slots + paused/stopwatch timers needing no wakeup; bounds both the
// static store (16 * ~64B ~= 1KB) and persist (well under persist_get_max_size
// ~4KB). A 17th add is refused (store_add returns false) -- a bounded, tested
// rule, never a realloc.
#define MAX_TIMERS 16
#define MAX_LAP_SETS 4

typedef struct {
  int32_t owner_cookie;
  int32_t values[MAX_LAPS];
} LapSet;

typedef struct TimerStore {
  Timer timers[MAX_TIMERS];
  // Four independently-owned lap histories. Existing histories are never
  // evicted: a stopwatch without a set cannot record its first lap until a
  // set is released by deleting/clearing its owner.
  LapSet lap_sets[MAX_LAP_SETS];
  int count;
  int32_t next_cookie;  // monotonically increasing; cookies are never reused
} TimerStore;

void store_init(TimerStore *s);

// Appends a new timer (kind, duration_s, started now, all other fields
// zeroed/defaulted). Returns false (no-op, *out_cookie unset) if the store is
// already at MAX_TIMERS -- the chosen overflow surface for a 17th timer: the
// caller is expected to refuse the create and testLog "store:full" rather
// than silently dropping or growing (see adapter's screen_set.c).
bool store_add(TimerStore *s, TimerKind kind, int32_t duration_s, time_t now, int32_t *out_cookie);

// Removes the timer with this cookie (compacts the array). Returns false if
// not found.
bool store_remove(TimerStore *s, int32_t cookie);

// Appends one elapsed lap. Returns false when the timer is full or all four
// sets belong to other timers; no existing history is evicted.
bool store_add_lap(TimerStore *s, Timer *t, time_t now);
bool store_lap_get(const TimerStore *s, const Timer *t, int index, int32_t *out_value);
bool store_lap_can_add(const TimerStore *s, const Timer *t);
void store_restart(TimerStore *s, Timer *t, time_t now);

// Restores a persisted lap sequence into an available set. Used only by the
// tolerant deserializer; it refuses rather than evicting another timer's set.
bool store_restore_laps(TimerStore *s, Timer *t, const int32_t *values, uint8_t count);

// Sets/clears the display-only
// Timer.name of the timer with this cookie -- finds it (store_find), then
// routes `name` through timer.h's timer_name_copy() for the trim+cap+
// UTF-8-safe-truncation contract. `name` NULL, "", or all-whitespace clears
// the name back to unnamed. Returns false (no-op) if `cookie` isn't found.
bool store_set_name(TimerStore *s, int32_t cookie, const char *name);

// HOME's
// "Clear all" -- resets count to 0 and zeroes every slot, same cookie-reuse
// discipline as store_remove/store_add (`next_cookie` is NEVER reset here,
// unlike store_init -- a clear must not let the next store_add() hand out a
// cookie value some already-resolved/gone timer used to hold).
void store_clear(TimerStore *s);

// Returns a pointer into the store's own array (valid until the next
// add/remove), or NULL if not found. Callers mutate through this pointer via
// timer.h's mutation functions, then the adapter persists.
Timer *store_find(TimerStore *s, int32_t cookie);
const Timer *store_find_const(const TimerStore *s, int32_t cookie);

// Home ordering (3-group):
//   group 0: RUNNING countdown/repeat  -- key = remaining(t, now) ascending (soonest first)
//   group 1: PAUSED  countdown/repeat  -- key = remaining(t, now) ascending (after all running)
//   group 2: STOPWATCH (incl. post-timer) -- key = elapsed(t, now) ascending (youngest first)
// Stable within a group. Writes up to MAX_TIMERS cookies into out_cookies;
// returns the count written.
int store_home_order(const TimerStore *s, time_t now, int32_t *out_cookies, int max_out);

// Nav fix: the 0-based row index `cookie` occupies in the
// CURRENT store_home_order() ranking (timer rows only, not the footer), or
// -1 if `cookie` no longer exists in the store (e.g. deleted while its hero
// was open) -- callers restoring a "land on this row" cursor position must
// treat -1 as "fall back to row 0," never index it directly.
int store_home_row_for_cookie(const TimerStore *s, int32_t cookie, time_t now);
