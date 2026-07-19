// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// Pure SET auto-repeat acceleration seam. NEVER #include <pebble.h>. This is the ONE authorized ms
// boundary into lib/: sub-second gesture timing is measured by the
// adapter's AppTimer/raw-click clock and passed in as an explicit scalar; no
// other lib/ module reasons in ms.
//
// window_single_repeating_click_subscribe gives a single fixed interval with
// no initial-delay/acceleration; the adapter (hold.c) subscribes at the FAST
// base interval and, on every firing, asks this pure decision unit "how many
// +-1 steps are due by now" given how long the button has been held and how
// many steps have already been applied -- exactly the acceleration curve: 400ms
// initial delay -> 150ms/step -> 65ms/step after the button has been held
// 2000ms total.
#pragma once

#include <stdint.h>

#define SET_REPEAT_DELAY_MS 400
#define SET_REPEAT_MS 150
#define SET_REPEAT_FAST_MS 65
#define SET_ACCEL_AFTER_MS 2000

// hold_elapsed_ms: milliseconds since the button went down (adapter-measured).
// tick_count: number of +-1 steps already applied since down (adapter-owned
//   counter; the button's own immediate down-press step counts as
//   tick_count=1 by the time this is first called for the repeat).
// Returns the number of ADDITIONAL steps due now (0 if not yet due) -- pure,
// deterministic, tick-rate-independent (computing "total due" from
// hold_elapsed_ms rather than accumulating per-call avoids drift if the
// adapter's own timer fires late).
int32_t set_accel_step(int32_t hold_elapsed_ms, int32_t tick_count);
