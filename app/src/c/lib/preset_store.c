// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

#include "preset_store.h"

#include <string.h>

// preset_store.h's own doc comment already claims the domain is `1 ..
// TIMER_MAX_DURATION_S` (same domain SET dials) -- this was previously
// enforced only at the LOW end. Closing the upper bound here (not just at
// the SET dial, which already clamps h to 0-99 and so can never itself
// produce a duration above TIMER_MAX_DURATION_S) matters because
// preset_store_add/edit are the GENERAL entry points -- a corrupted/foreign
// persist blob's per-record duration_s flows through here too (via
// deserialize_presets, which now shares this exact bound, see
// preset_serialize.c). Without this, a duration_s near INT32_MAX could
// round-trip into a live Timer via a future launch path and overflow
// timer_deadline()'s plain time_t addition (UB) -- closed now.
//
// Exposed via preset_store.h (was `static` here) so lib/preset_serialize.c's
// per-record deserialize loop calls this SAME definition instead of
// independently re-deriving the identical bound -- prevents the consistency
// risk that multiple independent implementations face when field validation
// logic drifts across copies.
bool preset_fields_valid(TimerKind kind, int32_t duration_s) {
  if (kind != KIND_COUNTDOWN && kind != KIND_REPEAT) return false;
  if (duration_s < 1 || duration_s > TIMER_MAX_DURATION_S) return false;
  return true;
}

void preset_store_init(PresetStore *s) {
  memset(s, 0, sizeof(*s));
}

bool preset_store_add(PresetStore *s, TimerKind kind, int32_t duration_s) {
  if (s->count >= MAX_PRESETS) return false;
  if (!preset_fields_valid(kind, duration_s)) return false;

  Preset *p = &s->presets[s->count];
  p->kind = kind;
  p->duration_s = duration_s;
  s->count++;
  return true;
}

bool preset_store_remove(PresetStore *s, int index) {
  if (index < 0 || index >= s->count) return false;

  for (int i = index; i < s->count - 1; i++) {
    s->presets[i] = s->presets[i + 1];
  }
  s->count--;
  memset(&s->presets[s->count], 0, sizeof(s->presets[s->count]));
  return true;
}

bool preset_store_edit(PresetStore *s, int index, TimerKind kind, int32_t duration_s) {
  if (index < 0 || index >= s->count) return false;
  if (!preset_fields_valid(kind, duration_s)) return false;

  s->presets[index].kind = kind;
  s->presets[index].duration_s = duration_s;
  return true;
}

const Preset *preset_store_get(const PresetStore *s, int index) {
  if (index < 0 || index >= s->count) return NULL;
  return &s->presets[index];
}

// Reuses lib/timer.h's timer_name_copy() -- the shared trim + UTF-8-safe-cap
// primitive that store_set_name() uses for Timer.name -- rather than
// duplicating multi-byte-safety logic here. preset_store.h already includes
// timer.h (for TimerKind), so no new include is needed.
bool preset_store_set_name(PresetStore *s, int index, const char *name) {
  if (index < 0 || index >= s->count) return false;
  timer_name_copy(s->presets[index].name, sizeof s->presets[index].name, name);
  return true;
}
