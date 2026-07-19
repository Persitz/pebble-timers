// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// HOME's "Clear all N timers?" confirm screen -- reached
// only from HOME's "Clear all" row (nav_go_clear_confirm). A plain
// single-press Select confirms (store_clear + persist + replan + confirm
// vibe + testlog("clear","all")) and returns to the now-empty HOME; Back
// cancels back to HOME unchanged. No hold, no MenuLayer -- modeled on
// screen_alarm.c's hero window (a plain full-root-layer Window with a
// direct window_set_click_config_provider, no menu wiring to fight).
#pragma once

#include <pebble.h>

// Creates the confirm window (once -- allocation discipline, same as
// every other screen).
Window *screen_clear_confirm_create(void);

// Destroys the confirm window. Call once at app exit.
void screen_clear_confirm_destroy(void);
