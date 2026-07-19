// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

#include "vibes_adapter.h"

#include <pebble.h>

#include "testlog_am.h"

// countdown/one-off: two short pulses. repeat: three, slightly longer --
// distinct-enough patterns without needing audio.
static const uint32_t kCountdownPattern[] = {200, 150, 200};
static const uint32_t kRepeatPattern[] = {250, 120, 250, 120, 250};

void vibes_adapter_alarm(bool is_repeat) {
  VibePattern pattern = is_repeat
                             ? (VibePattern){.durations = kRepeatPattern,
                                              .num_segments = ARRAY_LENGTH(kRepeatPattern)}
                             : (VibePattern){.durations = kCountdownPattern,
                                              .num_segments = ARRAY_LENGTH(kCountdownPattern)};
  vibes_enqueue_custom_pattern(pattern);
  testlog("vibe", is_repeat ? "repeatAlarm" : "countdownAlarm");
}

void vibes_adapter_confirm(void) {
  vibes_short_pulse();
  testlog("vibe", "shortConfirm");
}

// two short pulses -- distinct from confirm's single pulse and from both
// alarm patterns (kCountdownPattern/kRepeatPattern above), so it can never be
// mistaken for "it worked."
static const uint32_t kRejectPattern[] = {60, 80, 60};

void vibes_adapter_reject(void) {
  VibePattern pattern = {.durations = kRejectPattern, .num_segments = ARRAY_LENGTH(kRejectPattern)};
  vibes_enqueue_custom_pattern(pattern);
  testlog("vibe", "reject");
}
