// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// Pure formatting helpers. NEVER #include <pebble.h> -- this file must stay
// host-compilable for unit testing.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "timer.h"

// Formats a wall-clock time into `out` (NUL-terminated, truncated to fit
// `out_size`): "H:MM" (no leading zero on the hour) in 12-hour mode, or
// "HH:MM" (zero-padded) in 24-hour mode -- this is the top-right clock's
// format (e.g. "9:41", "17:46"). `hour24` is 0-23, `minute` is 0-59; both are clamped into range
// defensively (a malformed `struct tm` must not corrupt the buffer).
// Returns the number of characters written, excluding the NUL terminator.
size_t format_clock(char *out, size_t out_size, int hour24, int minute, bool is_24h);

// Same as format_clock, plus a trailing " am"/" pm" marker whenever `is_24h`
// is false (24h mode is untouched -- "18:45" stays "18:45"). The voice confirm
// screen's clock-target annotation exists specifically to make the
// resolver's AM/PM pick visible, which plain format_clock's bare "6:45"
// cannot do in 12h mode (identical string for a 6 AM and a 6 PM target).
// Scoped to that ONE call site deliberately -- every other format_clock
// caller (the always-on current-time clock, start/end labels) keeps its
// existing behavior; those displays' ambiguity is resolved trivially by the
// user's own physical context (day vs. night), unlike a resolver-made guess
// this annotation exists to surface. Truncates safely like every other
// formatter here (never overflows `out_size`).
size_t format_clock_ampm(char *out, size_t out_size, int hour24, int minute, bool is_24h);

// Splits a (clamped-to->=0) total-seconds duration into h/m/s. The single
// shared derivation every formatter below uses, so rounding/flooring is
// consistent everywhere.
void format_split_hms(int32_t total_s, int *out_h, int *out_m, int *out_s);

// Numeric-ONLY big-value string (no "^" baked in -- LECO numeral fonts
// can't render it; the adapter draws that one separately via lib/view's
// ViewPrefixKind). "H:MM:SS" if h>0, else "M:SS" (minutes NOT zero-padded,
// seconds always 2 digits): "2:38", "4:30", "15:38", "0:07". Negative input
// is clamped to 0 before formatting (a caller must not show a negative
// countdown; overrun/elapsed callers are already non-negative by
// construction).
size_t format_hms(char *out, size_t out_size, int32_t total_s);

// Same as format_hms, but prepends a literal "+" when `plus` is true --
// the bundled JBM numeral rasters are subset with "[0-9:+ ]", so a "+" identity
// (overtime, stopwatch hero, HOME stopwatch rows) is baked directly into the
// string instead of vector-drawn (cheaper, and reads as part of the numeral
// rather than a disconnected mark at a fixed corner offset). Caller derives
// `plus` from lib/view's ViewPrefixKind (VIEW_PREFIX_PLUS -> true). Truncates
// safely like format_hms/format_clock (never overflows `out`).
size_t format_hms_prefixed(char *out, size_t out_size, int32_t total_s, bool plus);

// HOME row value, kind-branched. A kind-agnostic `remaining()` read
// freezes a post-timer stopwatch's row at a permanently-clamped "0:00" once
// its effective_elapsed exceeds duration. Countdown/repeat -> remaining
// (clamped >=0 for display), no prefix. Stopwatch (incl. post-timer) -> elapsed,
// no prefix (note: stopwatch values no longer carry a baked-in "+" prefix in
// MAIN displays). Never reads `fired` to decide the branch -- kind is the only
// correct discriminant. Used across 3 UI surfaces: HOME rows, the
// ACTIONS-stopwatch strip, and the LAPS strip (all three format their value
// through this function).
size_t format_home_row_value(char *out, size_t out_size, const Timer *t, time_t now);
