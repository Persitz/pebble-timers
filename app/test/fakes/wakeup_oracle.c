// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// See wakeup_oracle.h for what this is and the empirical basis for the rule
// it models. Host-test-only; never shipped.
#include "wakeup_oracle.h"

#include <string.h>

void wakeup_oracle_reset(WakeupOracle *o) {
  memset(o, 0, sizeof *o);
  o->next_id = 1;
}

static int find_free_slot(WakeupOracle *o) {
  // Slots are compacted on removal (see remove_at), so `count` is always the
  // first free index while count < capacity.
  return o->count;
}

static bool collides_with_any_live(const WakeupOracle *o, time_t at) {
  for (int i = 0; i < o->count; i++) {
    if (!o->entries[i].live) continue;
    time_t diff = at - o->entries[i].at;
    if (diff < 0) diff = -diff;
    if (diff < WAKEUP_ORACLE_WINDOW_S) return true;
  }
  return false;
}

int wakeup_oracle_our_count(const WakeupOracle *o) {
  int n = 0;
  for (int i = 0; i < o->count; i++) {
    if (o->entries[i].live && o->entries[i].is_ours) n++;
  }
  return n;
}

void wakeup_oracle_add_foreign(WakeupOracle *o, time_t at) {
  int slot = find_free_slot(o);
  if (slot >= WAKEUP_ORACLE_MAX_ENTRIES) return;  // test fixture bug, not modeled further
  o->entries[slot] = (WakeupOracleEntry){
      .at = at, .cookie = 0, .id = 0, .is_ours = false, .live = true, .phantom = false};
  o->count++;
}

int32_t wakeup_oracle_schedule(WakeupOracle *o, time_t at, int32_t cookie) {
  int32_t result;
  if (collides_with_any_live(o, at)) {
    result = -8;  // E_RANGE
  } else if (wakeup_oracle_our_count(o) >= WAKEUP_ORACLE_CAP) {
    result = -7;  // E_OUT_OF_RESOURCES
  } else {
    int slot = find_free_slot(o);
    if (slot >= WAKEUP_ORACLE_MAX_ENTRIES) {
      result = -7;  // out of oracle table space -- treat like the real cap; the property
                     // test's generator is sized so this is never actually hit.
    } else {
      int32_t id = o->next_id++;
      o->entries[slot] = (WakeupOracleEntry){
          .at = at, .cookie = cookie, .id = id, .is_ours = true, .live = true, .phantom = false};
      o->count++;
      result = id;
    }
  }
  if (o->trace_count < WAKEUP_ORACLE_MAX_ENTRIES) {
    o->trace[o->trace_count].at = at;
    o->trace[o->trace_count].cookie = cookie;
    o->trace[o->trace_count].result = result;
    o->trace_count++;
  }
  return result;
}

static int find_by_id(WakeupOracle *o, int32_t id) {
  for (int i = 0; i < o->count; i++) {
    if (o->entries[i].live && o->entries[i].id == id) return i;
  }
  return -1;
}

bool wakeup_oracle_query(const WakeupOracle *o, int32_t id, time_t *out_at) {
  for (int i = 0; i < o->count; i++) {
    if (o->entries[i].live && o->entries[i].id == id) {
      if (o->entries[i].phantom) return false;
      if (out_at) *out_at = o->entries[i].at;
      return true;
    }
  }
  return false;
}

static void remove_at_index(WakeupOracle *o, int idx) {
  o->entries[idx] = o->entries[o->count - 1];
  o->count--;
}

void wakeup_oracle_cancel(WakeupOracle *o, int32_t id) {
  int idx = find_by_id(o, id);
  if (idx >= 0) remove_at_index(o, idx);
}

void wakeup_oracle_cancel_all(WakeupOracle *o) {
  // Remove every live "ours" entry. Iterate from the end so the
  // swap-with-last compaction in remove_at_index never skips an
  // about-to-be-checked element.
  for (int i = o->count - 1; i >= 0; i--) {
    if (o->entries[i].is_ours && o->entries[i].live) remove_at_index(o, i);
  }
}

void wakeup_oracle_mark_phantom(WakeupOracle *o, int32_t id) {
  int idx = find_by_id(o, id);
  if (idx >= 0) o->entries[idx].phantom = true;
}

bool wakeup_oracle_has_our_cookie(const WakeupOracle *o, int32_t cookie, time_t *out_at) {
  for (int i = 0; i < o->count; i++) {
    if (o->entries[i].live && o->entries[i].is_ours && o->entries[i].cookie == cookie) {
      if (out_at) *out_at = o->entries[i].at;
      return true;
    }
  }
  return false;
}

bool wakeup_oracle_would_admit(const WakeupOracle *o, time_t at) {
  return !collides_with_any_live(o, at);
}
