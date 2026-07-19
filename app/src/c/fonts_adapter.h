// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// Loads the app's custom JetBrains Mono Bold numeral rasters ONCE, at app_init
// and unloads them once at app_deinit. Never allocated per-window or per-frame.
// Falls back to the nearest system LECO font automatically if a raster failed
// to load, so a resource problem degrades gracefully rather than crashing.
//
// Originally only the app's 3 most-visible numeral sites were bundled: the hero
// value and SET's active field both at 42px (LECO_42_NUMBERS), and HOME's
// 1-timer row value at 32px (LECO_32_BOLD_NUMBERS). A 56px raster was added
// for the running/overtime/stopwatch heroes, whose measured digit cap height
// (31px) was one size class smaller than the intended size (42px). See
// fonts_adapter.c for the final bundled sizes.
#pragma once

#include <pebble.h>

void fonts_adapter_init(void);
void fonts_adapter_deinit(void);

// JetBrains Mono Bold at 42px (hero value, SET active field) -- falls back
// to FONT_KEY_LECO_42_NUMBERS if the raster isn't loaded.
GFont fonts_adapter_numeral_42(void);

// JetBrains Mono Bold at 32px (HOME 2-timer row value, SET inactive
// field -- both already drew at this exact system-font size, so this raster
// covers both for free) -- falls back to FONT_KEY_LECO_32_BOLD_NUMBERS if
// the raster isn't loaded.
GFont fonts_adapter_numeral_32(void);

// JetBrains Mono Bold at 56px: the running/overtime/stopwatch hero value --
// the app's single highest-visibility numeral site. No system LECO font
// reaches this size, so the fallback chain is fonts_adapter_numeral_42() (not
// a raw system font).
GFont fonts_adapter_numeral_56(void);
