// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

#include "preset_serialize.h"
#include "unity.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

// Local little-endian int32 writer for hand-building raw wire buffers (the
// same throwaway helper serialize_test.c uses for its own hand-built V1
// blobs) -- kept file-local so this test never depends on preset_serialize.c's
// own (intentionally static) put_i32.
static void put_test_i32(uint8_t *p, int32_t v) {
  uint32_t u = (uint32_t)v;
  p[0] = (uint8_t)(u & 0xFF);
  p[1] = (uint8_t)((u >> 8) & 0xFF);
  p[2] = (uint8_t)((u >> 16) & 0xFF);
  p[3] = (uint8_t)((u >> 24) & 0xFF);
}

void test_round_trip_empty_store(void) {
  PresetStore s;
  preset_store_init(&s);

  uint8_t buf[PRESET_SERIALIZE_WIRE_SIZE];
  size_t written = serialize_presets(&s, buf, sizeof buf);
  TEST_ASSERT_EQUAL_size_t(PRESET_SERIALIZE_HEADER_SIZE, written);

  PresetStore out;
  memset(&out, 0xAA, sizeof out);  // poison, to prove deserialize actually resets count
  TEST_ASSERT_TRUE(deserialize_presets(buf, written, &out));
  TEST_ASSERT_EQUAL_INT(0, out.count);
}

void test_round_trip_one_preset(void) {
  PresetStore s;
  preset_store_init(&s);
  preset_store_add(&s, KIND_COUNTDOWN, 300);

  uint8_t buf[PRESET_SERIALIZE_WIRE_SIZE];
  size_t written = serialize_presets(&s, buf, sizeof buf);
  TEST_ASSERT_EQUAL_size_t(PRESET_SERIALIZE_HEADER_SIZE + PRESET_SERIALIZE_RECORD_SIZE_V2, written);

  PresetStore out;
  TEST_ASSERT_TRUE(deserialize_presets(buf, written, &out));
  TEST_ASSERT_EQUAL_INT(1, out.count);
  TEST_ASSERT_EQUAL(KIND_COUNTDOWN, out.presets[0].kind);
  TEST_ASSERT_EQUAL_INT32(300, out.presets[0].duration_s);
  TEST_ASSERT_EQUAL_STRING("", out.presets[0].name);
}

void test_round_trip_max_presets_both_kinds(void) {
  PresetStore s;
  preset_store_init(&s);
  for (int i = 0; i < MAX_PRESETS; i++) {
    TimerKind k = (i % 2 == 0) ? KIND_COUNTDOWN : KIND_REPEAT;
    TEST_ASSERT_TRUE(preset_store_add(&s, k, 60 + i));
  }

  uint8_t buf[PRESET_SERIALIZE_WIRE_SIZE];
  size_t written = serialize_presets(&s, buf, sizeof buf);
  TEST_ASSERT_EQUAL_size_t(PRESET_SERIALIZE_WIRE_SIZE, written);

  PresetStore out;
  TEST_ASSERT_TRUE(deserialize_presets(buf, written, &out));
  TEST_ASSERT_EQUAL_INT(MAX_PRESETS, out.count);
  for (int i = 0; i < MAX_PRESETS; i++) {
    TimerKind k = (i % 2 == 0) ? KIND_COUNTDOWN : KIND_REPEAT;
    TEST_ASSERT_EQUAL(k, out.presets[i].kind);
    TEST_ASSERT_EQUAL_INT32(60 + i, out.presets[i].duration_s);
  }
}

void test_serialize_includes_version_tag(void) {
  // serialize_presets always writes the CURRENT (V2, name-bearing) tag.
  PresetStore s;
  preset_store_init(&s);
  preset_store_add(&s, KIND_COUNTDOWN, 60);

  uint8_t buf[PRESET_SERIALIZE_WIRE_SIZE];
  serialize_presets(&s, buf, sizeof buf);
  TEST_ASSERT_EQUAL_HEX8(0xB8, buf[0]);  // PRESET_WIRE_TAG_V2 (preset_serialize.c's own private constant)
  TEST_ASSERT_EQUAL_UINT8(1, buf[1]);
}

void test_serialize_refuses_too_small_buffer(void) {
  PresetStore s;
  preset_store_init(&s);
  preset_store_add(&s, KIND_COUNTDOWN, 60);
  preset_store_add(&s, KIND_COUNTDOWN, 60);

  uint8_t buf[PRESET_SERIALIZE_HEADER_SIZE + PRESET_SERIALIZE_RECORD_SIZE_V2];  // room for only 1
  TEST_ASSERT_EQUAL_size_t(0, serialize_presets(&s, buf, sizeof buf));
}

void test_deserialize_rejects_bad_tag(void) {
  uint8_t buf[PRESET_SERIALIZE_WIRE_SIZE] = {0};
  buf[0] = 0x00;  // wrong tag
  buf[1] = 0;
  PresetStore out;
  TEST_ASSERT_FALSE(deserialize_presets(buf, sizeof buf, &out));
}

void test_deserialize_rejects_short_buffer(void) {
  uint8_t buf[1] = {0xB8};  // missing the count byte
  PresetStore out;
  TEST_ASSERT_FALSE(deserialize_presets(buf, sizeof buf, &out));
}

void test_deserialize_rejects_count_over_max(void) {
  uint8_t buf[PRESET_SERIALIZE_WIRE_SIZE] = {0};
  buf[0] = 0xB8;
  buf[1] = MAX_PRESETS + 1;
  PresetStore out;
  TEST_ASSERT_FALSE(deserialize_presets(buf, sizeof buf, &out));
}

void test_deserialize_rejects_short_for_declared_count(void) {
  PresetStore s;
  preset_store_init(&s);
  preset_store_add(&s, KIND_COUNTDOWN, 60);
  preset_store_add(&s, KIND_COUNTDOWN, 60);

  uint8_t buf[PRESET_SERIALIZE_WIRE_SIZE];
  size_t written = serialize_presets(&s, buf, sizeof buf);
  PresetStore out;
  // Truncate to one record short of what the header claims.
  TEST_ASSERT_FALSE(deserialize_presets(buf, written - 1, &out));
}

// --- per-record tolerance: a deliberate deviation from an "any bad record ->
// empty store" design -- a single corrupt record is skipped, not fatal to
// the whole blob; see preset_serialize.c/.h's updated doc comments.
// Re-verified here against the CURRENT (V2, 21-byte) record layout to
// confirm a later format bump didn't regress this --------------------------

void test_deserialize_skips_bad_kind_record_keeps_others(void) {
  PresetStore s;
  preset_store_init(&s);
  preset_store_add(&s, KIND_COUNTDOWN, 60);  // record 0 -- will be corrupted
  preset_store_add(&s, KIND_REPEAT, 90);     // record 1 -- must survive
  preset_store_set_name(&s, 1, "Focus");

  uint8_t buf[PRESET_SERIALIZE_WIRE_SIZE];
  size_t written = serialize_presets(&s, buf, sizeof buf);
  buf[PRESET_SERIALIZE_HEADER_SIZE] = 99;  // corrupt the kind byte of record 0

  PresetStore out;
  TEST_ASSERT_TRUE(deserialize_presets(buf, written, &out));
  TEST_ASSERT_EQUAL_INT(1, out.count);  // only record 1 survived
  TEST_ASSERT_EQUAL(KIND_REPEAT, out.presets[0].kind);
  TEST_ASSERT_EQUAL_INT32(90, out.presets[0].duration_s);
  TEST_ASSERT_EQUAL_STRING("Focus", out.presets[0].name);
}

void test_deserialize_skips_zero_duration_record_keeps_others(void) {
  PresetStore s;
  preset_store_init(&s);
  preset_store_add(&s, KIND_COUNTDOWN, 60);  // record 0 -- will be corrupted
  preset_store_add(&s, KIND_REPEAT, 90);     // record 1 -- must survive

  uint8_t buf[PRESET_SERIALIZE_WIRE_SIZE];
  size_t written = serialize_presets(&s, buf, sizeof buf);
  // Zero out the duration_s bytes of record 0.
  buf[PRESET_SERIALIZE_HEADER_SIZE + 1] = 0;
  buf[PRESET_SERIALIZE_HEADER_SIZE + 2] = 0;
  buf[PRESET_SERIALIZE_HEADER_SIZE + 3] = 0;
  buf[PRESET_SERIALIZE_HEADER_SIZE + 4] = 0;

  PresetStore out;
  TEST_ASSERT_TRUE(deserialize_presets(buf, written, &out));
  TEST_ASSERT_EQUAL_INT(1, out.count);  // only record 1 survived
  TEST_ASSERT_EQUAL(KIND_REPEAT, out.presets[0].kind);
  TEST_ASSERT_EQUAL_INT32(90, out.presets[0].duration_s);
}

// --- duration upper bound ---------------------------------------------------

void test_deserialize_skips_over_max_duration_record_keeps_others(void) {
  PresetStore s;
  preset_store_init(&s);
  preset_store_add(&s, KIND_COUNTDOWN, 60);  // record 0 -- will be corrupted
  preset_store_add(&s, KIND_REPEAT, 90);     // record 1 -- must survive

  uint8_t buf[PRESET_SERIALIZE_WIRE_SIZE];
  size_t written = serialize_presets(&s, buf, sizeof buf);
  // Overwrite record 0's duration_s with TIMER_MAX_DURATION_S + 1 (little-endian).
  int32_t bad = TIMER_MAX_DURATION_S + 1;
  buf[PRESET_SERIALIZE_HEADER_SIZE + 1] = (uint8_t)(bad & 0xFF);
  buf[PRESET_SERIALIZE_HEADER_SIZE + 2] = (uint8_t)((bad >> 8) & 0xFF);
  buf[PRESET_SERIALIZE_HEADER_SIZE + 3] = (uint8_t)((bad >> 16) & 0xFF);
  buf[PRESET_SERIALIZE_HEADER_SIZE + 4] = (uint8_t)((bad >> 24) & 0xFF);

  PresetStore out;
  TEST_ASSERT_TRUE(deserialize_presets(buf, written, &out));
  TEST_ASSERT_EQUAL_INT(1, out.count);  // only record 1 survived
  TEST_ASSERT_EQUAL(KIND_REPEAT, out.presets[0].kind);
  TEST_ASSERT_EQUAL_INT32(90, out.presets[0].duration_s);
}

void test_deserialize_accepts_duration_at_max_boundary(void) {
  PresetStore s;
  preset_store_init(&s);
  preset_store_add(&s, KIND_COUNTDOWN, TIMER_MAX_DURATION_S);

  uint8_t buf[PRESET_SERIALIZE_WIRE_SIZE];
  size_t written = serialize_presets(&s, buf, sizeof buf);

  PresetStore out;
  TEST_ASSERT_TRUE(deserialize_presets(buf, written, &out));
  TEST_ASSERT_EQUAL_INT(1, out.count);
  TEST_ASSERT_EQUAL_INT32(TIMER_MAX_DURATION_S, out.presets[0].duration_s);
}

// --- naming: V1/V2 tolerant load, named round-trip, wire-size fit ----------

void test_round_trip_with_name(void) {
  PresetStore s;
  preset_store_init(&s);
  preset_store_add(&s, KIND_COUNTDOWN, 300);
  preset_store_set_name(&s, 0, "Tea");

  uint8_t buf[PRESET_SERIALIZE_WIRE_SIZE];
  size_t written = serialize_presets(&s, buf, sizeof buf);
  TEST_ASSERT_EQUAL_size_t(PRESET_SERIALIZE_HEADER_SIZE + PRESET_SERIALIZE_RECORD_SIZE_V2, written);

  PresetStore out;
  TEST_ASSERT_TRUE(deserialize_presets(buf, written, &out));
  TEST_ASSERT_EQUAL_INT(1, out.count);
  TEST_ASSERT_EQUAL_STRING("Tea", out.presets[0].name);
}

void test_round_trip_name_less_stays_name_less(void) {
  PresetStore s;
  preset_store_init(&s);
  preset_store_add(&s, KIND_COUNTDOWN, 60);

  uint8_t buf[PRESET_SERIALIZE_WIRE_SIZE];
  size_t written = serialize_presets(&s, buf, sizeof buf);

  PresetStore out;
  memset(&out, 0xAA, sizeof out);  // poison first -- a lingering nonzero name would show
  TEST_ASSERT_TRUE(deserialize_presets(buf, written, &out));
  TEST_ASSERT_EQUAL_STRING("", out.presets[0].name);
}

// A hand-built, byte-for-byte V1 buffer (not derived from serialize_presets,
// which can no longer produce one) standing in for a real pre-0.1.9
// on-flash record.
void test_deserialize_legacy_v1_blob_loads_name_less(void) {
  uint8_t buf[PRESET_SERIALIZE_HEADER_SIZE + 2 * PRESET_SERIALIZE_RECORD_SIZE_V1];
  buf[0] = 0xB7;  // PRESET_WIRE_TAG_V1
  buf[1] = 2;     // count

  // Record 0: countdown, 300s.
  uint8_t *rec0 = &buf[PRESET_SERIALIZE_HEADER_SIZE];
  rec0[0] = KIND_COUNTDOWN;
  put_test_i32(&rec0[1], 300);
  // Record 1: repeat, 90s.
  uint8_t *rec1 = &buf[PRESET_SERIALIZE_HEADER_SIZE + PRESET_SERIALIZE_RECORD_SIZE_V1];
  rec1[0] = KIND_REPEAT;
  put_test_i32(&rec1[1], 90);

  PresetStore out;
  TEST_ASSERT_TRUE(deserialize_presets(buf, sizeof buf, &out));
  TEST_ASSERT_EQUAL_INT(2, out.count);
  TEST_ASSERT_EQUAL(KIND_COUNTDOWN, out.presets[0].kind);
  TEST_ASSERT_EQUAL_INT32(300, out.presets[0].duration_s);
  // The load-bearing assertion: an old, name-less blob loads name-less, not
  // garbage/corrupt/crashed.
  TEST_ASSERT_EQUAL_STRING("", out.presets[0].name);
  TEST_ASSERT_EQUAL(KIND_REPEAT, out.presets[1].kind);
  TEST_ASSERT_EQUAL_INT32(90, out.presets[1].duration_s);
  TEST_ASSERT_EQUAL_STRING("", out.presets[1].name);
}

// Mixed-validity tolerance still works on a hand-built V1 blob too: a
// corrupt record among legacy (name-less) records is skipped, the rest load.
void test_deserialize_legacy_v1_blob_mixed_validity_skips_bad_record(void) {
  uint8_t buf[PRESET_SERIALIZE_HEADER_SIZE + 2 * PRESET_SERIALIZE_RECORD_SIZE_V1];
  buf[0] = 0xB7;
  buf[1] = 2;

  uint8_t *rec0 = &buf[PRESET_SERIALIZE_HEADER_SIZE];
  rec0[0] = 99;  // invalid kind
  put_test_i32(&rec0[1], 60);
  uint8_t *rec1 = &buf[PRESET_SERIALIZE_HEADER_SIZE + PRESET_SERIALIZE_RECORD_SIZE_V1];
  rec1[0] = KIND_COUNTDOWN;
  put_test_i32(&rec1[1], 45);

  PresetStore out;
  TEST_ASSERT_TRUE(deserialize_presets(buf, sizeof buf, &out));
  TEST_ASSERT_EQUAL_INT(1, out.count);
  TEST_ASSERT_EQUAL(KIND_COUNTDOWN, out.presets[0].kind);
  TEST_ASSERT_EQUAL_INT32(45, out.presets[0].duration_s);
  TEST_ASSERT_EQUAL_STRING("", out.presets[0].name);
}

void test_deserialize_v2_rejects_buffer_too_short_for_the_name_section(void) {
  // A V2-tagged blob truncated before its own name section is a structural
  // short-buffer failure, not a partial/garbled name -- refused outright,
  // same rule as deserialize_timer()'s equivalent V2 check.
  uint8_t buf[PRESET_SERIALIZE_HEADER_SIZE + 5] = {0};  // tag says V2, too short for it
  buf[0] = 0xB8;
  buf[1] = 1;
  PresetStore out;
  TEST_ASSERT_FALSE(deserialize_presets(buf, sizeof buf, &out));
}

void test_deserialize_v2_non_terminated_name_is_capped_not_overrun(void) {
  // Defense in depth: a corrupt/foreign blob's name bytes might carry no NUL
  // terminator at all -- must load as a capped, still-valid C string, never
  // read past the field.
  uint8_t buf[PRESET_SERIALIZE_HEADER_SIZE + PRESET_SERIALIZE_RECORD_SIZE_V2] = {0};
  buf[0] = 0xB8;
  buf[1] = 1;
  uint8_t *rec = &buf[PRESET_SERIALIZE_HEADER_SIZE];
  rec[0] = KIND_COUNTDOWN;
  put_test_i32(&rec[1], 60);
  memset(&rec[5], 'x', PRESET_NAME_MAX);  // no NUL anywhere in the field

  PresetStore out;
  TEST_ASSERT_TRUE(deserialize_presets(buf, sizeof buf, &out));
  TEST_ASSERT_EQUAL_INT(1, out.count);
  TEST_ASSERT_EQUAL_size_t(PRESET_NAME_MAX - 1, strlen(out.presets[0].name));  // capped, still valid
}

// The test above only exercises all-ASCII ('x') filler with no NUL -- this
// one exercises an ACTUALLY invalid/incomplete multi-byte UTF-8 lead byte
// sitting right at the field's own truncation boundary (a byte declaring a
// 4-byte sequence, 0xF0, with only 1 byte of room left before
// PRESET_NAME_MAX's own cap) -- the same "drop the whole codepoint, never
// emit a dangling lead byte" contract timer_name_copy() already guarantees
// generically (timer_test.c's own
// test_name_copy_utf8_safe_does_not_split_a_multibyte_char), now proven
// through THIS reader's own defensive re-cap path specifically.
void test_deserialize_v2_incomplete_utf8_lead_byte_at_cap_boundary_is_dropped_whole(void) {
  uint8_t buf[PRESET_SERIALIZE_HEADER_SIZE + PRESET_SERIALIZE_RECORD_SIZE_V2] = {0};
  buf[0] = 0xB8;
  buf[1] = 1;
  uint8_t *rec = &buf[PRESET_SERIALIZE_HEADER_SIZE];
  rec[0] = KIND_COUNTDOWN;
  put_test_i32(&rec[1], 60);
  uint8_t *name = &rec[5];
  memset(name, 'n', PRESET_NAME_MAX);  // fill all 16 bytes with ASCII 'n'
  name[PRESET_NAME_MAX - 1] = 0xF0;    // last byte: a 4-byte UTF-8 lead, zero continuation bytes

  PresetStore out;
  TEST_ASSERT_TRUE(deserialize_presets(buf, sizeof buf, &out));
  TEST_ASSERT_EQUAL_INT(1, out.count);
  // dst_size=PRESET_NAME_MAX(16), cap=15: the first 15 'n' bytes fit; the
  // trailing 0xF0 lead byte claims a 4-byte sequence that cannot fit within
  // cap at all -- dropped whole, never truncated into a dangling lead byte.
  TEST_ASSERT_EQUAL_size_t(PRESET_NAME_MAX - 1, strlen(out.presets[0].name));
  for (size_t i = 0; i < PRESET_NAME_MAX - 1; i++) {
    TEST_ASSERT_EQUAL_INT('n', out.presets[0].name[i]);
  }
}

// The load-bearing wire-fit assertion: 12 max-length-name presets must
// serialize to <= PERSIST_DATA_MAX_LENGTH (256B) -- the constraint that set
// PRESET_NAME_MAX at 16 in the first place. Re-derived: 2 + 12*21 = 254B.
void test_wire_size_at_max_presets_with_full_length_names_fits_the_persist_key(void) {
  PresetStore s;
  preset_store_init(&s);
  char name[PRESET_NAME_MAX + 8];
  memset(name, 'n', sizeof name - 1);
  name[sizeof name - 1] = '\0';  // longer than PRESET_NAME_MAX -- exercises the real cap

  for (int i = 0; i < MAX_PRESETS; i++) {
    TEST_ASSERT_TRUE(preset_store_add(&s, KIND_COUNTDOWN, 60 + i));
    preset_store_set_name(&s, i, name);
    TEST_ASSERT_EQUAL_INT(PRESET_NAME_MAX - 1, (int)strlen(s.presets[i].name));
  }

  uint8_t buf[PRESET_SERIALIZE_WIRE_SIZE];
  size_t written = serialize_presets(&s, buf, sizeof buf);
  TEST_ASSERT_EQUAL_size_t(PRESET_SERIALIZE_WIRE_SIZE, written);
  TEST_ASSERT_EQUAL_size_t(254, written);
  TEST_ASSERT_TRUE(written <= 256);  // PERSIST_DATA_MAX_LENGTH -- the hard fit guarantee

  PresetStore out;
  TEST_ASSERT_TRUE(deserialize_presets(buf, written, &out));
  TEST_ASSERT_EQUAL_INT(MAX_PRESETS, out.count);
  for (int i = 0; i < MAX_PRESETS; i++) {
    TEST_ASSERT_EQUAL_STRING(s.presets[i].name, out.presets[i].name);
  }
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_round_trip_empty_store);
  RUN_TEST(test_round_trip_one_preset);
  RUN_TEST(test_round_trip_max_presets_both_kinds);
  RUN_TEST(test_serialize_includes_version_tag);
  RUN_TEST(test_serialize_refuses_too_small_buffer);
  RUN_TEST(test_deserialize_rejects_bad_tag);
  RUN_TEST(test_deserialize_rejects_short_buffer);
  RUN_TEST(test_deserialize_rejects_count_over_max);
  RUN_TEST(test_deserialize_rejects_short_for_declared_count);
  RUN_TEST(test_deserialize_skips_bad_kind_record_keeps_others);
  RUN_TEST(test_deserialize_skips_zero_duration_record_keeps_others);
  RUN_TEST(test_deserialize_skips_over_max_duration_record_keeps_others);
  RUN_TEST(test_deserialize_accepts_duration_at_max_boundary);
  RUN_TEST(test_round_trip_with_name);
  RUN_TEST(test_round_trip_name_less_stays_name_less);
  RUN_TEST(test_deserialize_legacy_v1_blob_loads_name_less);
  RUN_TEST(test_deserialize_legacy_v1_blob_mixed_validity_skips_bad_record);
  RUN_TEST(test_deserialize_v2_rejects_buffer_too_short_for_the_name_section);
  RUN_TEST(test_deserialize_v2_non_terminated_name_is_capped_not_overrun);
  RUN_TEST(test_deserialize_v2_incomplete_utf8_lead_byte_at_cap_boundary_is_dropped_whole);
  RUN_TEST(test_wire_size_at_max_presets_with_full_length_names_fits_the_persist_key);
  return UNITY_END();
}
