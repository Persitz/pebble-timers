// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

#include "format.h"

#include <stdio.h>

static int clamp_int(int value, int lo, int hi) {
  if (value < lo) return lo;
  if (value > hi) return hi;
  return value;
}

size_t format_clock(char *out, size_t out_size, int hour24, int minute, bool is_24h) {
  if (out == NULL || out_size == 0) return 0;

  hour24 = clamp_int(hour24, 0, 23);
  minute = clamp_int(minute, 0, 59);

  int written;
  if (is_24h) {
    written = snprintf(out, out_size, "%02d:%02d", hour24, minute);
  } else {
    int hour12 = hour24 % 12;
    if (hour12 == 0) hour12 = 12;
    written = snprintf(out, out_size, "%d:%02d", hour12, minute);
  }

  if (written < 0) {
    out[0] = '\0';
    return 0;
  }
  size_t written_len = (size_t)written;
  return written_len < out_size ? written_len : out_size - 1;
}

size_t format_clock_ampm(char *out, size_t out_size, int hour24, int minute, bool is_24h) {
  if (out == NULL || out_size == 0) return 0;
  if (is_24h) return format_clock(out, out_size, hour24, minute, is_24h);

  // Build the plain "H:MM" numeric part first (same clamping/formatting
  // format_clock already does), then compose it with the am/pm suffix --
  // mirrors format_hms_prefixed's own "build numeric, then compose" shape.
  char numeric[8];
  format_clock(numeric, sizeof numeric, hour24, minute, false);

  int clamped_hour = clamp_int(hour24, 0, 23);
  const char *suffix = (clamped_hour < 12) ? "am" : "pm";

  int written = snprintf(out, out_size, "%s %s", numeric, suffix);
  if (written < 0) {
    out[0] = '\0';
    return 0;
  }
  size_t written_len = (size_t)written;
  return written_len < out_size ? written_len : out_size - 1;
}

void format_split_hms(int32_t total_s, int *out_h, int *out_m, int *out_s) {
  if (total_s < 0) total_s = 0;
  *out_h = (int)(total_s / 3600);
  *out_m = (int)((total_s % 3600) / 60);
  *out_s = (int)(total_s % 60);
}

size_t format_hms(char *out, size_t out_size, int32_t total_s) {
  if (out == NULL || out_size == 0) return 0;

  int h, m, s;
  format_split_hms(total_s, &h, &m, &s);

  int written;
  if (h > 0) {
    written = snprintf(out, out_size, "%d:%02d:%02d", h, m, s);
  } else {
    written = snprintf(out, out_size, "%d:%02d", m, s);
  }

  if (written < 0) {
    out[0] = '\0';
    return 0;
  }
  size_t written_len = (size_t)written;
  return written_len < out_size ? written_len : out_size - 1;
}

size_t format_hms_prefixed(char *out, size_t out_size, int32_t total_s, bool plus) {
  if (out == NULL || out_size == 0) return 0;
  if (!plus) return format_hms(out, out_size, total_s);

  // Format the numeric part into a local buffer, then prepend "+" --
  // keeps truncation safe (both stages are independently NUL-terminated)
  // even if `out_size` is smaller than the full "+M:SS" string.
  char numeric[16];
  format_hms(numeric, sizeof numeric, total_s);
  int written = snprintf(out, out_size, "+%s", numeric);
  if (written < 0) {
    out[0] = '\0';
    return 0;
  }
  size_t written_len = (size_t)written;
  return written_len < out_size ? written_len : out_size - 1;
}

size_t format_home_row_value(char *out, size_t out_size, const Timer *t, time_t now) {
  int32_t seconds = (t->kind == KIND_STOPWATCH) ? timer_elapsed_s(t, now)
                                                 : timer_remaining_s(t, now);
  if (seconds < 0) seconds = 0;
  // Stopwatch-kind rows do not carry a baked-in "+" prefix on their main
  // value. This one formatter is shared across the HOME rows, the
  // ACTIONS-stopwatch strip, and the LAPS strip -- see
  // screen_home.c/screen_actions.c/screen_laps.c for the call sites.
  return format_hms(out, out_size, seconds);
}
