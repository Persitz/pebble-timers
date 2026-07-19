// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// Shared, single-instance adapter state: the timer store + the fired/ringing
// set. Adapter-tier only (screens, nav.c, wakeup_adapter.c, etc. reach these
// accessors instead of threading pointers through every screen's API);
// lib/ modules never see this directly -- they take a TimerStore*/AppState*
// argument explicitly, which is exactly what these accessors hand out.
#pragma once

#include "lib/app_state.h"
#include "lib/preset_store.h"
#include "lib/store.h"

void app_context_init(void);
TimerStore *app_context_store(void);
AppState *app_context_app_state(void);
PresetStore *app_context_presets(void);
