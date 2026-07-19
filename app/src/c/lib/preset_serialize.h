// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// Pure pack/unpack of a whole PresetStore <-> ONE fixed-layout byte blob
// (persist key 30 -- see persist_preset_adapter.h -- PERSIST_DATA_MAX_LENGTH
// = 256B/key). NEVER #include <pebble.h> -- no persist_* call lives here,
// only the byte layout; src/c/persist_preset_adapter.c does the actual
// persist_write_data/read_data. Mirrors lib/serialize.c's fixed-width
// put_i32/get_i32 discipline (no raw struct memcpy -- see that file's doc for
// why: cross-toolchain width safety).
//
// Additive, tolerant version bump -- exactly the
// Timer V1/V2 pattern (lib/serialize.{c,h}) applied here. V1 is the
// original, name-less wire format (the originally shipped layout, untouched in
// the reader); V2 adds a fixed PRESET_NAME_MAX-byte name section per record.
// serialize_presets() always WRITES the current format (V2);
// deserialize_presets() reads EITHER tag, so an already-persisted V1 blob
// still loads correctly (every name defaults to "" -- Preset's zero-init).
//
// Wire format:
//   [0]    version tag (PRESET_WIRE_TAG_V1 or _V2)
//   [1]    count
//   [2..]  `count` records:
//            V1 (legacy, 5 bytes):  kind(1) + duration_s(4)
//            V2 (current, 21 bytes): kind(1) + duration_s(4) +
//                                    name(PRESET_NAME_MAX, NUL-padded)
//
// Wire-size math: worst case is
// PRESET_SERIALIZE_HEADER_SIZE(2) + MAX_PRESETS(12) *
// PRESET_SERIALIZE_RECORD_SIZE_V2(21) = 2 + 252 = 254 bytes -- comfortably
// under PERSIST_DATA_MAX_LENGTH (256 bytes/key), a 2-byte margin. This is
// the hard fit guarantee that set PRESET_NAME_MAX at 16, not a soft target.
#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "preset_store.h"

#define PRESET_SERIALIZE_HEADER_SIZE 2
#define PRESET_SERIALIZE_RECORD_SIZE_V1 5
#define PRESET_SERIALIZE_RECORD_SIZE_V2 (5 + PRESET_NAME_MAX)
// Worst-case (MAX_PRESETS records, current V2 record width) buffer size
// callers must provide, same role as serialize.h's SERIALIZE_TIMER_WIRE_SIZE.
#define PRESET_SERIALIZE_WIRE_SIZE \
  (PRESET_SERIALIZE_HEADER_SIZE + PRESET_SERIALIZE_RECORD_SIZE_V2 * MAX_PRESETS)

// Packs `s` into `out` (must be >= PRESET_SERIALIZE_WIRE_SIZE bytes in the
// worst case; the actual encoded length is
// PRESET_SERIALIZE_HEADER_SIZE + PRESET_SERIALIZE_RECORD_SIZE_V2*s->count --
// every fresh write is always the CURRENT (V2, name-bearing) format).
// Returns the number of bytes actually written, or 0 if `out_size` is too
// small for this store's own count.
size_t serialize_presets(const PresetStore *s, uint8_t *out, size_t out_size);

// Unpacks `in` into `*out`. Returns false (out param left untouched) only on
// a STRUCTURAL failure: the format tag is neither PRESET_WIRE_TAG_V1 nor _V2,
// `in_size` is too short for the declared header/count (at the tag's own
// record width), or the declared `count` exceeds MAX_PRESETS -- none of
// these can be attributed to a single record, so there is nothing partial to
// salvage. The caller (persist_preset_adapter_load) is expected to fall back
// to an already-initialized empty store on false, same tolerance discipline
// as persist_adapter_load.
//
// A V1-tagged blob (5-byte
// records, no name section) loads every preset with `name == ""` -- the
// tag alone selects record width (5 vs. 21 bytes) before the count math, so
// an old, already-persisted V1 blob is never rejected. A V2 record's name
// bytes are defensively re-capped through timer_name_copy() (via a NUL-
// terminated scratch buffer) rather than trusted raw, so a corrupt/foreign
// name section degrades to a truncated-but-valid string, never UB.
//
// A per-record failure -- an invalid kind (not countdown/repeat), or `duration_s` outside
// `1 .. TIMER_MAX_DURATION_S` -- is NOT a whole-call failure.
// That single record is silently skipped and every other valid record still
// loads, matching persist_adapter_load's own per-slot tolerance (a corrupt/
// short TIMER slot doesn't discard every other timer). `out->count` on a
// true return may therefore be less than the blob's own declared count, if
// any records were skipped.
bool deserialize_presets(const uint8_t *in, size_t in_size, PresetStore *out);
