// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

#include "serialize.h"

#include <string.h>  // memcpy (the name field)

// Additive, tolerant version bump: V1 is the original, name-less wire format;
// V2 adds a fixed TIMER_NAME_MAX-byte name section; V3 additively appends a
// 4-byte set_total_s field after V2's name section.
// serialize_timer() always WRITES the current format (V3);
// deserialize_timer() reads ANY of the three tags, so an old, already-
// persisted V1 or V2 blob still loads correctly (V1: name defaults to ""
// via Timer's zero-init; V1 and V2 both: set_total_s falls back to
// duration_s, the best available approximation for old records).
#define WIRE_TAG_V1 0xA5
#define WIRE_TAG_V2 0xA6
#define WIRE_TAG_V3 0xA7
#define WIRE_TAG_V4 0xA8

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

static void put_i64(uint8_t *p, int64_t v) {
  uint64_t u = (uint64_t)v;
  for (int i = 0; i < 8; i++) {
    p[i] = (uint8_t)((u >> (8 * i)) & 0xFF);
  }
}

static int64_t get_i64(const uint8_t *p) {
  uint64_t u = 0;
  for (int i = 0; i < 8; i++) {
    u |= (uint64_t)p[i] << (8 * i);
  }
  return (int64_t)u;
}

size_t serialize_timer_with_store(const TimerStore *store, const Timer *t, uint8_t *out, size_t out_size) {
  size_t needed = SERIALIZE_TIMER_HEADER_SIZE_V4 + 4 * (size_t)t->lap_count;
  if (store == NULL || t == NULL || out == NULL || out_size < needed) return 0;

  put_u8(&out[0], WIRE_TAG_V4);
  put_u8(&out[1], (uint8_t)t->kind);
  put_u8(&out[2], t->is_paused ? 1 : 0);
  put_u8(&out[3], t->lap_count);
  put_i32(&out[4], t->cookie);
  put_i32(&out[8], t->duration_s);
  put_i64(&out[12], (int64_t)t->started_at);
  put_i64(&out[20], (int64_t)t->paused_at);
  put_i32(&out[28], t->accumulated_pause_s);
  put_u8(&out[32], t->fired ? 1 : 0);
  put_i32(&out[33], t->cycles);
  put_i32(&out[37], t->original_total_s);
  // Fixed TIMER_NAME_MAX-byte name section, right after the legacy
  // 41-byte header -- t->name is always a fully NUL-padded fixed buffer
  // (timer_name_copy()/zero-init both guarantee this), so a straight
  // fixed-width copy is sufficient (no length prefix needed).
  memcpy(&out[SERIALIZE_TIMER_HEADER_SIZE], t->name, TIMER_NAME_MAX);
  // Fixed 4-byte set_total_s field, right after the V2 (name-bearing)
  // header and before the (still variable-length) lap section.
  put_i32(&out[SERIALIZE_TIMER_HEADER_SIZE_V2], t->set_total_s);
  for (int i = 0; i < t->lap_count; i++) {
    int32_t lap;
    if (!store_lap_get(store, t, i, &lap)) return 0;
    put_i32(&out[SERIALIZE_TIMER_HEADER_SIZE_V4 + 4 * i], lap);
  }
  return needed;
}

bool deserialize_timer_with_store(const uint8_t *in, size_t in_size, TimerStore *store, Timer *t,
                                  bool *out_laps_dropped) {
  if (out_laps_dropped != NULL) *out_laps_dropped = false;
  if (in == NULL || in_size < SERIALIZE_TIMER_HEADER_SIZE) return false;

  uint8_t tag = in[0];
  if (store == NULL ||
      (tag != WIRE_TAG_V1 && tag != WIRE_TAG_V2 && tag != WIRE_TAG_V3 && tag != WIRE_TAG_V4)) return false;

  uint8_t lap_count = in[3];
  if (lap_count > MAX_LAPS) return false;
  if (in[1] != KIND_COUNTDOWN && in[1] != KIND_STOPWATCH && in[1] != KIND_REPEAT) return false;

  // V1 (legacy, no name section) vs. V2 (name, no set_total_s) vs. V3
  // (current) header size -- this is the whole tolerant-load mechanism: an
  // old blob's own declared/actual size never included the sections added
  // after it was written, so requiring only ITS OWN header size here
  // (rather than always the newest one) is what lets it keep loading
  // correctly after every additive bump.
  size_t header_size = SERIALIZE_TIMER_HEADER_SIZE;
  if (tag == WIRE_TAG_V2) header_size = SERIALIZE_TIMER_HEADER_SIZE_V2;
  else if (tag == WIRE_TAG_V3 || tag == WIRE_TAG_V4) header_size = SERIALIZE_TIMER_HEADER_SIZE_V3;
  size_t needed = header_size + 4 * (size_t)lap_count;
  if (in_size < needed) return false;

  Timer tmp = {0};
  tmp.kind = (TimerKind)in[1];
  tmp.is_paused = in[2] != 0;
  tmp.lap_count = lap_count;
  tmp.cookie = get_i32(&in[4]);
  tmp.duration_s = get_i32(&in[8]);
  tmp.started_at = (time_t)get_i64(&in[12]);
  tmp.paused_at = (time_t)get_i64(&in[20]);
  tmp.accumulated_pause_s = get_i32(&in[28]);
  tmp.fired = in[32] != 0;
  tmp.cycles = get_i32(&in[33]);
  tmp.original_total_s = get_i32(&in[37]);
  // tmp.name stays "" (Timer tmp = {0} zero-init) for a V1 blob -- the
  // tolerant-load contract this bump exists for.
  if (tag == WIRE_TAG_V2 || tag == WIRE_TAG_V3 || tag == WIRE_TAG_V4) {
    // Defensive re-cap via timer_name_copy() rather than a raw memcpy of the
    // blob's own name bytes straight into tmp.name: a corrupt/foreign blob
    // could carry a non-NUL-terminated TIMER_NAME_MAX-byte run, which would
    // read past the field if trusted directly. Buffering it into a local,
    // explicitly-NUL-terminated scratch array first, then running it
    // through the SAME trim/UTF-8-safe-cap contract every other name writer
    // uses, makes a corrupt name section degrade to a truncated-but-valid
    // string, never undefined behavior.
    char raw_name[TIMER_NAME_MAX + 1] = {0};
    memcpy(raw_name, &in[SERIALIZE_TIMER_HEADER_SIZE], TIMER_NAME_MAX);
    timer_name_copy(tmp.name, sizeof tmp.name, raw_name);
  }
  // A V3 blob carries its own set_total_s; a V1/V2 blob (persisted before
  // this field existed) falls back to duration_s -- the best available
  // approximation (a record that old was never grown by +1:00/add-1/4 in a
  // way this load can recover, so "what's currently running" is the closest
  // available stand-in for "what was originally dialed").
  if (tag == WIRE_TAG_V3 || tag == WIRE_TAG_V4) {
    tmp.set_total_s = get_i32(&in[SERIALIZE_TIMER_HEADER_SIZE_V2]);
  } else {
    tmp.set_total_s = tmp.duration_s;
  }
  int32_t laps[MAX_LAPS];
  for (int i = 0; i < lap_count; i++) laps[i] = get_i32(&in[header_size + 4 * i]);
  if (!store_restore_laps(store, &tmp, laps, lap_count)) {
    tmp.lap_count = 0;
    if (out_laps_dropped != NULL) *out_laps_dropped = true;
  }

  *t = tmp;
  return true;
}
