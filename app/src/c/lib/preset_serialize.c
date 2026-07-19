// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

#include "preset_serialize.h"

#include <string.h>  // memcpy (the name field)

// Additive, tolerant version bump -- exactly the Timer V1/V2 pattern
// (lib/serialize.c) applied to presets. V1 is the original shipped layout
// (name-less); V2 adds a fixed PRESET_NAME_MAX-byte name section per record.
// serialize_presets() always WRITES V2; deserialize_presets() reads EITHER
// tag.
#define PRESET_WIRE_TAG_V1 0xB7
#define PRESET_WIRE_TAG_V2 0xB8

static void put_u8(uint8_t *p, uint8_t v) {
  p[0] = v;
}

static void put_i32(uint8_t *p, int32_t v) {
  uint32_t u = (uint32_t)v;
  p[0] = (uint8_t)(u & 0xFF);
  p[1] = (uint8_t)((u >> 8) & 0xFF);
  p[2] = (uint8_t)((u >> 16) & 0xFF);
  p[3] = (uint8_t)((u >> 24) & 0xFF);
}

static int32_t get_i32(const uint8_t *p) {
  uint32_t u = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
               ((uint32_t)p[3] << 24);
  return (int32_t)u;
}

size_t serialize_presets(const PresetStore *s, uint8_t *out, size_t out_size) {
  size_t needed =
      PRESET_SERIALIZE_HEADER_SIZE + PRESET_SERIALIZE_RECORD_SIZE_V2 * (size_t)s->count;
  if (out == NULL || out_size < needed) return 0;

  put_u8(&out[0], PRESET_WIRE_TAG_V2);
  put_u8(&out[1], (uint8_t)s->count);
  for (int i = 0; i < s->count; i++) {
    uint8_t *rec = &out[PRESET_SERIALIZE_HEADER_SIZE + PRESET_SERIALIZE_RECORD_SIZE_V2 * i];
    put_u8(&rec[0], (uint8_t)s->presets[i].kind);
    put_i32(&rec[1], s->presets[i].duration_s);
    // s->presets[i].name is always a fully NUL-padded fixed buffer
    // (timer_name_copy()/zero-init both guarantee this), so a straight
    // fixed-width copy is sufficient (no length prefix needed) -- same
    // reasoning as serialize_timer()'s own name-section write.
    memcpy(&rec[5], s->presets[i].name, PRESET_NAME_MAX);
  }
  return needed;
}

// Per-record tolerance, matching persist_adapter_load's own pattern
// (persist_adapter.c) of skipping only the corrupt/short slot and keeping
// every other timer. A single flipped byte in one record (partial flash
// write, format bug) must not discard an entire preset library -- only
// structural failures (bad tag, a buffer too short for even the declared
// header/count, or a declared count > MAX_PRESETS -- none of which can be
// attributed to a single record) still fail the whole call. An invalid
// record (bad kind, duration_s < 1, or duration_s > TIMER_MAX_DURATION_S)
// is simply skipped; `tmp.count` ends up being however many records
// actually validated, which may be less than the blob's own declared
// count.
//
// The tag alone selects record width (V1=5B, no name; V2=21B,
// name(PRESET_NAME_MAX)) before the count math runs, so a legacy V1 blob
// loads every preset with name=="" (PresetStore tmp's own zero-init --
// preset_store_init()), never a rejected record.
bool deserialize_presets(const uint8_t *in, size_t in_size, PresetStore *out) {
  if (in == NULL || in_size < PRESET_SERIALIZE_HEADER_SIZE) return false;

  uint8_t tag = in[0];
  if (tag != PRESET_WIRE_TAG_V1 && tag != PRESET_WIRE_TAG_V2) return false;

  uint8_t declared_count = in[1];
  if (declared_count > MAX_PRESETS) return false;

  size_t record_size =
      (tag == PRESET_WIRE_TAG_V2) ? PRESET_SERIALIZE_RECORD_SIZE_V2 : PRESET_SERIALIZE_RECORD_SIZE_V1;
  size_t needed = PRESET_SERIALIZE_HEADER_SIZE + record_size * (size_t)declared_count;
  if (in_size < needed) return false;

  PresetStore tmp;
  preset_store_init(&tmp);
  for (int i = 0; i < declared_count; i++) {
    const uint8_t *rec = &in[PRESET_SERIALIZE_HEADER_SIZE + record_size * i];
    TimerKind kind = (TimerKind)rec[0];
    int32_t duration_s = get_i32(&rec[1]);
    // Calls preset_store.h's shared preset_fields_valid() instead of
    // re-deriving the same {kind, duration_s} bound inline, so the two
    // checks can't drift out of sync.
    if (!preset_fields_valid(kind, duration_s)) continue;  // skip: corrupt record

    tmp.presets[tmp.count].kind = kind;
    tmp.presets[tmp.count].duration_s = duration_s;
    if (tag == PRESET_WIRE_TAG_V2) {
      // Defensive re-cap via timer_name_copy() rather than a raw memcpy of
      // the blob's own name bytes straight into tmp.presets[...].name: a
      // corrupt/foreign blob could carry a non-NUL-terminated
      // PRESET_NAME_MAX-byte run, which would read past the field if
      // trusted directly. Buffering it into a local, explicitly-NUL-
      // terminated scratch array first, then running it through the SAME
      // trim/UTF-8-safe-cap contract every other name writer uses, makes a
      // corrupt name section degrade to a truncated-but-valid string, never
      // undefined behavior. Mirrors deserialize_timer()'s identical pattern.
      char raw_name[PRESET_NAME_MAX + 1] = {0};
      memcpy(raw_name, &rec[5], PRESET_NAME_MAX);
      timer_name_copy(tmp.presets[tmp.count].name, sizeof tmp.presets[tmp.count].name, raw_name);
    }
    // V1 records leave name=="" -- tmp.presets[tmp.count] came from
    // preset_store_init()'s zero-init and was never otherwise written.
    tmp.count++;
  }

  *out = tmp;
  return true;
}
