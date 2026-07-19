// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// Haptic wiring (Vibes, pebble.h Part 1.10) + the testLog "vibe" markers
// used to make each vibration pattern observable to automated tests.
#pragma once

#include <stdbool.h>

// The alarm buzz -- a longer/more insistent pattern for a repeat alarm than
// a one-off (both distinct from the hold-confirm pulse). Also fires the
// re-alert cadence (same pattern, re-emitted by app_state's
// should_rebuzz decision).
void vibes_adapter_alarm(bool is_repeat);

// A short "it took" confirmation pulse on any hold-fire (Pause/Resume,
// alarm Done/Keep-going) -- an eyes-free/gloved acknowledgement.
void vibes_adapter_confirm(void);

// A distinct "denied" double-pulse for a press that does nothing -- currently the HOME "+ New timer" footer at
// MAX_TIMERS. Deliberately a different shape from both vibes_adapter_confirm
// (one short pulse -- "it took") and the alarm patterns, so a user can tell
// "nothing happened" from "it worked" without looking at the screen.
void vibes_adapter_reject(void);
