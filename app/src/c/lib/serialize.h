// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// Pure pack/unpack of a single Timer <-> a fixed-layout byte buffer for
// persist_* (PERSIST_DATA_MAX_LENGTH = 256B/key). NEVER #include <pebble.h>
// -- no persist_* call lives here, only the byte layout; src/c/
// persist_adapter.c does the actual persist_write_data/read_data.
//
// The wire format uses ONLY fixed-width types (int32_t/int64_t/uint8_t), not
// a raw struct memcpy: `time_t`'s width is platform-dependent (differs
// between the host gcc build these Unity tests run under and the
// arm-none-eabi-gcc on-watch build), so a straight `memcpy(&Timer, ...)`
// would silently produce a different-sized/incompatible layout on each
// toolchain. Packing field-by-field into an explicit little bit of a
// `#pragma pack`ed struct keeps both builds' round-trips internally
// consistent regardless of `time_t`'s native width.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "store.h"

// Fixed 41-byte LEGACY header (tag/kind/flags/cookie/duration/started_at/
// paused_at/accumulated_pause_s/fired/cycles/original_total_s) -- the
// original wire format (WIRE_TAG_V1 in serialize.c), before Timer.name
// existed. Kept as its own named constant (rather than folded away) because
// deserialize_timer() must still recognize a V1 blob's shorter header size
// to load an old, name-less persisted timer tolerantly.
#define SERIALIZE_TIMER_HEADER_SIZE 41

// The CURRENT wire format's header -- the legacy header plus a fixed
// TIMER_NAME_MAX-byte name field (NUL-padded), additive right after it and
// before the (still variable-length) lap section. Every NEW write always
// uses this layout (serialize_timer's own WIRE_TAG_V2); only an OLD,
// already-persisted V1 blob is missing this section (deserialize_timer
// detects the tag and loads it name-less, `name == ""`).
#define SERIALIZE_TIMER_HEADER_SIZE_V2 (SERIALIZE_TIMER_HEADER_SIZE + TIMER_NAME_MAX)

// The V2 header plus a fixed 4-byte set_total_s field, additive right after
// it and before the (still variable-length) lap section -- uses the same
// additive-bump pattern applied when the V2 name section was added. Every
// NEW write always uses this layout (serialize_timer's own WIRE_TAG_V3); an
// OLD V1 or V2 blob is missing this field (deserialize_timer detects the tag
// and falls back to `set_total_s = duration_s` -- the best available
// approximation for a timer persisted before this field existed).
#define SERIALIZE_TIMER_HEADER_SIZE_V3 (SERIALIZE_TIMER_HEADER_SIZE_V2 + 4)
#define SERIALIZE_TIMER_HEADER_SIZE_V4 SERIALIZE_TIMER_HEADER_SIZE_V3

// 4 bytes per lap actually present (variable-length -- most timers carry
// lap_count=0). Worst case (MAX_LAPS laps, current V3 header) is the buffer
// size callers must provide; still comfortably under Pebble's
// PERSIST_DATA_MAX_LENGTH (256 bytes/key).
#define SERIALIZE_TIMER_WIRE_SIZE (SERIALIZE_TIMER_HEADER_SIZE_V4 + 4 * MAX_LAPS)

// Packs `t` into `out` (must be >= SERIALIZE_TIMER_WIRE_SIZE bytes -- the
// worst-case/MAX_LAPS size; the actual encoded length is
// SERIALIZE_TIMER_HEADER_SIZE + 4*t->lap_count and is what gets written to
// persist). Returns the number of bytes actually written, or 0 if `out_size`
// is too small for this timer's own lap_count.
size_t serialize_timer_with_store(const TimerStore *store, const Timer *t, uint8_t *out, size_t out_size);

// Unpacks `in` into `*t`. Returns false (out param left untouched) if
// `in_size` is too small or the format tag doesn't match -- a corrupt/
// foreign-version persist slot must never be trusted as a valid Timer.
bool deserialize_timer_with_store(const uint8_t *in, size_t in_size, TimerStore *store, Timer *t,
                                  bool *out_laps_dropped);
