// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

#include "app_context.h"

static TimerStore s_store;
static AppState s_app_state;
static PresetStore s_presets;

void app_context_init(void) {
  store_init(&s_store);
  app_state_init(&s_app_state);
  preset_store_init(&s_presets);
}

TimerStore *app_context_store(void) {
  return &s_store;
}

AppState *app_context_app_state(void) {
  return &s_app_state;
}

PresetStore *app_context_presets(void) {
  return &s_presets;
}
