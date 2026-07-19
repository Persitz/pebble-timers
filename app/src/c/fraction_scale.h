// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// Single source of truth for the 0-1000 per-mille fraction scale shared by
// render.h's bar-fill drawing (RENDER_FRACTION_SCALE) and hold.c's
// hold_progress()/hold_debug_force() (previously HOLD_FRACTION_SCALE, an
// independent `#define 1000` in hold.c) -- the two were documented as one
// shared contract (hold.h's own hold_progress() doc comment already said so)
// but nothing enforced them staying numerically identical. Deliberately a
// standalone header with NO <pebble.h> dependency (unlike render.h, which
// needs GContext/GRect for its draw-call signatures) so hold.c can include
// it without pulling render.h's full graphics-type surface into the host
// test build (hold.c is one of the handful of adapter files host-tested,
// which doesn't cover GContext/GRect).
#pragma once

#define FRACTION_SCALE_PM 1000
