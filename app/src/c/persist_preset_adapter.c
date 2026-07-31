// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

#include "persist_preset_adapter.h"

#include <pebble.h>
#include <stdio.h>

#include "lib/preset_serialize.h"
#include "testlog_am.h"

// Boot-time load-count marker emitted once at the single exit point,
// regardless of which branch below produced the final `out->count`
// (empty/never-persisted, missing/short blob, corrupt blob, or a real
// reload). A flow asserting "preset_load:<N>" reflects the count
// actually handed back to the caller.
void persist_preset_adapter_load(PresetStore *out) {
  preset_store_init(out);

  if (persist_exists(PERSIST_KEY_PRESETS)) {
    uint8_t buf[PRESET_SERIALIZE_WIRE_SIZE];
    int len = persist_read_data(PERSIST_KEY_PRESETS, buf, sizeof buf);
    if (len > 0 && !deserialize_presets(buf, (size_t)len, out)) {
      preset_store_init(out);  // corrupt/foreign blob -- degrade to empty, never crash
    }
    // len <= 0 (missing/short blob) -- stay empty (tolerant), same as before.
  }

  char data[8];
  snprintf(data, sizeof data, "%d", out->count);
  testlog("preset_load", data);
}

void persist_preset_adapter_save(const PresetStore *store) {
  uint8_t buf[PRESET_SERIALIZE_WIRE_SIZE];
  size_t n = serialize_presets(store, buf, sizeof buf);
  if (n == 0) {
    // Serialize_presets() returning 0 means `buf` was too small for the current
    // store->count. Structurally unreachable today (preset_store_add caps at MAX_PRESETS
    // and this buffer is always sized at PRESET_SERIALIZE_WIRE_SIZE), but represents
    // enforcement of the 256-byte hard fit guarantee. If a future change (e.g. bumping
    // PRESET_NAME_MAX or MAX_PRESETS) blows that budget, the math must be re-derived.
    testlog("preset_persist_fail", "overflow");
    return;
  }

  // pebble.h: "The number of bytes written if successful, a value from
  // StatusCode otherwise." A complete write is the only successful save.
  if (persist_write_data(PERSIST_KEY_PRESETS, buf, n) != (int)n) {
    testlog("preset_persist_fail", "write");
    return;
  }

  char data[8];
  snprintf(data, sizeof data, "%d", store->count);
  testlog("preset_persist", data);  // Makes the persisted count observable to automated tests
}
