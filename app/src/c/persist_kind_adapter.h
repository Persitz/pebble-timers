// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// Persists the last non-stopwatch kind committed from a dial form. Key 31 is
// outside the TimerStore manifest/slots (0..17) and PresetStore blob (30).
#pragma once

#include "lib/timer.h"

// Loads the remembered kind, defaulting safely to countdown for a missing or
// corrupt one-byte record. Call once during app initialization.
void persist_kind_adapter_load(void);

// Returns the remembered create/preset-new dial intent.
TimerKind persist_kind_adapter_last(void);

// Saves a countdown or repeat dial commit. Stopwatch is intentionally ignored
// by the caller because it is not a kind-toggle preference.
void persist_kind_adapter_save(TimerKind kind);
