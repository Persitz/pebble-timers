// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

#include "persist_kind_adapter.h"

#include <pebble.h>

#define PERSIST_KEY_LAST_DIAL_KIND 31

static TimerKind s_last_kind = KIND_COUNTDOWN;

void persist_kind_adapter_load(void) {
  s_last_kind = KIND_COUNTDOWN;
  uint8_t value;
  if (!persist_exists(PERSIST_KEY_LAST_DIAL_KIND)) return;
  if (persist_read_data(PERSIST_KEY_LAST_DIAL_KIND, &value, sizeof value) != sizeof value) return;
  if (value == KIND_REPEAT) s_last_kind = KIND_REPEAT;
}

TimerKind persist_kind_adapter_last(void) {
  return s_last_kind;
}

void persist_kind_adapter_save(TimerKind kind) {
  if (kind != KIND_COUNTDOWN && kind != KIND_REPEAT) return;
  uint8_t value = (uint8_t)kind;
  persist_write_data(PERSIST_KEY_LAST_DIAL_KIND, &value, sizeof value);
  s_last_kind = kind;
}
