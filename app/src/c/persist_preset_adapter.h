// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// persist_* wiring for the PresetStore: lib/preset_serialize <->
// persist_write_data/read_data. Key layout: a single key (30) holds the
// whole store as one blob (unlike persist_adapter.h's per-timer keys -- a
// preset is 5 bytes and the whole store fits in 62 bytes, far under
// PERSIST_DATA_MAX_LENGTH). Persist key 30 is otherwise unused (verified:
// persist_adapter.c uses 0=count, 1=next_cookie, 2..17=timers). Called on
// EVERY preset mutation (add/edit/delete), same immediate-persist discipline
// as persist_adapter.h -- never touches the TimerStore or its own keys.
#pragma once

#include "lib/preset_store.h"

#define PERSIST_KEY_PRESETS 30

// Loads the persisted PresetStore into *out (empty store if the key is
// absent/corrupt -- tolerant, never crashes). Call once at boot.
void persist_preset_adapter_load(PresetStore *out);

// Rewrites the single blob from `store`. Call after every preset mutation
// (add/edit/delete).
void persist_preset_adapter_save(const PresetStore *store);
