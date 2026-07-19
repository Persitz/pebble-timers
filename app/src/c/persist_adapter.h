// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// persist_* wiring: lib/serialize <-> persist_write_data/read_data. Key
// layout: key 0 = manifest (count + next_cookie); keys 1..count = one
// serialized Timer each, in store order. Called on EVERY mutation (the
// immediate-persist discipline -- simple
// rewrite-everything rather than diffing, since MAX_TIMERS=16 keeps the
// worst case cheap and mutation frequency is button-driven (low).
#pragma once

#include "lib/store.h"

// Loads the persisted store into *out (empty store if nothing/garbage is
// persisted -- tolerant, never crashes on a missing/corrupt key). Call once
// at boot.
void persist_adapter_load(TimerStore *out);

// Rewrites the manifest + every timer slot from `store`. Call after every
// mutation (create/pause/resume/fire/done/keep-counting/snooze/...).
void persist_adapter_save(const TimerStore *store);
