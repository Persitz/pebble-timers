// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

#include "serialize.h"
#include "unity.h"

#include <string.h>

static TimerStore s_store;
static TimerStore s_loaded;

#define serialize_timer(t, out, size) serialize_timer_with_store(&s_store, t, out, size)
#define deserialize_timer(in, size, t) deserialize_timer_with_store(in, size, &s_loaded, t, NULL)

void setUp(void) {
  store_init(&s_store);
  store_init(&s_loaded);
}
void tearDown(void) {}

void test_round_trip_basic_countdown(void) {
  Timer t = {0};
  t.cookie = 7;
  t.kind = KIND_COUNTDOWN;
  t.duration_s = 300;
  t.started_at = 123456;
  t.accumulated_pause_s = 12;
  t.fired = false;

  uint8_t buf[SERIALIZE_TIMER_WIRE_SIZE];
  size_t written = serialize_timer(&t, buf, sizeof buf);
  TEST_ASSERT_TRUE(written > 0);

  Timer out = {0};
  TEST_ASSERT_TRUE(deserialize_timer(buf, written, &out));
  TEST_ASSERT_EQUAL_INT32(7, out.cookie);
  TEST_ASSERT_EQUAL(KIND_COUNTDOWN, out.kind);
  TEST_ASSERT_EQUAL_INT32(300, out.duration_s);
  TEST_ASSERT_EQUAL(123456, out.started_at);
  TEST_ASSERT_EQUAL_INT32(12, out.accumulated_pause_s);
  TEST_ASSERT_FALSE(out.fired);
}

void test_round_trip_paused_fired_flags(void) {
  Timer t = {0};
  t.cookie = 3;
  t.kind = KIND_REPEAT;
  t.is_paused = true;
  t.paused_at = 999;
  t.fired = true;
  t.cycles = 5;
  t.original_total_s = 900;

  uint8_t buf[SERIALIZE_TIMER_WIRE_SIZE];
  size_t written = serialize_timer(&t, buf, sizeof buf);
  Timer out = {0};
  TEST_ASSERT_TRUE(deserialize_timer(buf, written, &out));
  TEST_ASSERT_TRUE(out.is_paused);
  TEST_ASSERT_EQUAL(999, out.paused_at);
  TEST_ASSERT_TRUE(out.fired);
  TEST_ASSERT_EQUAL_INT32(5, out.cycles);
  TEST_ASSERT_EQUAL_INT32(900, out.original_total_s);
}

void test_round_trip_with_laps(void) {
  Timer t = {0};
  t.cookie = 1;
  t.kind = KIND_STOPWATCH;
  int32_t laps[] = {10, 25, 61};
  TEST_ASSERT_TRUE(store_restore_laps(&s_store, &t, laps, 3));

  uint8_t buf[SERIALIZE_TIMER_WIRE_SIZE];
  size_t written = serialize_timer(&t, buf, sizeof buf);
  // serialize_timer always writes the CURRENT (V3, name-bearing +
  // set_total_s) header size now, not an older one.
  TEST_ASSERT_EQUAL_size_t(SERIALIZE_TIMER_HEADER_SIZE_V4 + 4 * 3, written);

  Timer out = {0};
  TEST_ASSERT_TRUE(deserialize_timer(buf, written, &out));
  TEST_ASSERT_EQUAL_UINT8(3, out.lap_count);
  int32_t lap;
  TEST_ASSERT_TRUE(store_lap_get(&s_loaded, &out, 0, &lap));
  TEST_ASSERT_EQUAL_INT32(10, lap);
  TEST_ASSERT_TRUE(store_lap_get(&s_loaded, &out, 1, &lap));
  TEST_ASSERT_EQUAL_INT32(25, lap);
  TEST_ASSERT_TRUE(store_lap_get(&s_loaded, &out, 2, &lap));
  TEST_ASSERT_EQUAL_INT32(61, lap);
}

void test_serialize_refuses_too_small_buffer(void) {
  Timer t = {0};
  t.lap_count = 5;
  uint8_t buf[SERIALIZE_TIMER_HEADER_SIZE];  // too small once laps are counted
  TEST_ASSERT_EQUAL_size_t(0, serialize_timer(&t, buf, sizeof buf));
}

void test_deserialize_rejects_bad_tag(void) {
  uint8_t buf[SERIALIZE_TIMER_WIRE_SIZE] = {0};
  buf[0] = 0x00;  // wrong tag
  Timer out;
  TEST_ASSERT_FALSE(deserialize_timer(buf, sizeof buf, &out));
}

void test_deserialize_rejects_short_buffer(void) {
  uint8_t buf[4] = {0xA5, 0, 0, 0};
  Timer out;
  TEST_ASSERT_FALSE(deserialize_timer(buf, sizeof buf, &out));
}

void test_deserialize_rejects_invalid_kind(void) {
  Timer t = {0};
  t.kind = KIND_COUNTDOWN;
  uint8_t buf[SERIALIZE_TIMER_WIRE_SIZE];
  serialize_timer(&t, buf, sizeof buf);
  buf[1] = 99;  // corrupt the kind byte
  Timer out;
  TEST_ASSERT_FALSE(deserialize_timer(buf, sizeof buf, &out));
}

void test_deserialize_rejects_lap_count_over_max(void) {
  Timer t = {0};
  uint8_t buf[SERIALIZE_TIMER_WIRE_SIZE];
  serialize_timer(&t, buf, sizeof buf);
  buf[3] = MAX_LAPS + 1;  // corrupt lap_count past capacity
  Timer out;
  TEST_ASSERT_FALSE(deserialize_timer(buf, sizeof buf, &out));
}

// original_total_s's raw field value already round-trips
// (test_round_trip_paused_fired_flags above) -- these two go one level up
// and assert the higher-level meanings built on top of that field (the
// snooze marker, and set_total_s as the quarter-amount base) survive a real
// serialize/deserialize cycle too, not just the bytes.

void test_round_trip_preserves_snoozing_repeat(void) {
  Timer t = {0};
  t.kind = KIND_REPEAT;
  t.duration_s = 300;  // 5:00 interval
  t.started_at = 1000;
  timer_snooze_repeat(&t, 5000);  // marks the timer as snoozing (original_total_s != 0)
  TEST_ASSERT_TRUE(timer_is_snoozing(&t));

  uint8_t buf[SERIALIZE_TIMER_WIRE_SIZE];
  size_t written = serialize_timer(&t, buf, sizeof buf);
  Timer out = {0};
  TEST_ASSERT_TRUE(deserialize_timer(buf, written, &out));

  TEST_ASSERT_TRUE(timer_is_snoozing(&out));
  TEST_ASSERT_EQUAL_INT32(75, timer_remaining_s(&out, 5000));  // 300/4, unchanged after load
}

// timer_add_quarter() no longer seeds original_total_s as a quarter-amount
// cache -- the base timer_quarter_amount() reads set_total_s instead (see
// lib/timer.h's own doc), which must itself survive a real persist/load
// round trip (the wire format's set_total_s field) so every future quarter
// computation/gate stays correct after a reload, not re-derived from the
// (now-grown) live duration_s.
void test_round_trip_preserves_set_total_s_for_quarter_amount(void) {
  Timer t = {0};
  t.kind = KIND_COUNTDOWN;
  t.duration_s = 1200;    // 20:00
  t.set_total_s = 1200;   // as if freshly store_add()'d
  t.started_at = 1000;
  timer_add_quarter(&t, 2000);  // grows duration_s; set_total_s untouched
  TEST_ASSERT_EQUAL_INT32(1500, t.duration_s);
  TEST_ASSERT_EQUAL_INT32(1200, t.set_total_s);
  TEST_ASSERT_EQUAL_INT32(300, timer_quarter_amount(&t));

  uint8_t buf[SERIALIZE_TIMER_WIRE_SIZE];
  size_t written = serialize_timer(&t, buf, sizeof buf);
  Timer out = {0};
  TEST_ASSERT_TRUE(deserialize_timer(buf, written, &out));

  TEST_ASSERT_EQUAL_INT32(1200, out.set_total_s);
  TEST_ASSERT_EQUAL_INT32(300, timer_quarter_amount(&out));
  TEST_ASSERT_TRUE(timer_add_quarter_available(&out));
}

// --- Timer naming (Timer.name) --------------------------------------------
// Local little-endian helpers matching serialize.c's own private encoding
// (needed to hand-build a legacy V1 blob byte-for-byte, standing in for a
// real pre-upgrade on-flash record -- serialize_timer() itself only ever
// writes the CURRENT format now, so it can't produce one).

static void put_test_i32(uint8_t *p, int32_t v) {
  uint32_t u = (uint32_t)v;
  p[0] = (uint8_t)(u & 0xFF);
  p[1] = (uint8_t)((u >> 8) & 0xFF);
  p[2] = (uint8_t)((u >> 16) & 0xFF);
  p[3] = (uint8_t)((u >> 24) & 0xFF);
}

static void put_test_i64(uint8_t *p, int64_t v) {
  uint64_t u = (uint64_t)v;
  for (int i = 0; i < 8; i++) p[i] = (uint8_t)((u >> (8 * i)) & 0xFF);
}

void test_serialize_always_writes_current_v3_tag(void) {
  Timer t = {0};
  uint8_t buf[SERIALIZE_TIMER_WIRE_SIZE];
  TEST_ASSERT_TRUE(serialize_timer(&t, buf, sizeof buf) > 0);
  TEST_ASSERT_EQUAL_UINT8(0xA8, buf[0]);  // WIRE_TAG_V4 (serialize.c's own private constant)
}

// The current (V3) wire format's own new field round-trips, independent of
// duration_s -- the whole point is that these two CAN legitimately differ (a
// grown countdown) and must not be conflated on load.
void test_round_trip_v3_preserves_set_total_s_distinct_from_duration(void) {
  Timer t = {0};
  t.kind = KIND_COUNTDOWN;
  t.duration_s = 1500;    // grown (25:00)
  t.set_total_s = 1200;   // originally dialed (20:00)

  uint8_t buf[SERIALIZE_TIMER_WIRE_SIZE];
  size_t written = serialize_timer(&t, buf, sizeof buf);
  Timer out = {0};
  TEST_ASSERT_TRUE(deserialize_timer(buf, written, &out));
  TEST_ASSERT_EQUAL_INT32(1500, out.duration_s);
  TEST_ASSERT_EQUAL_INT32(1200, out.set_total_s);
}

void test_round_trip_with_name(void) {
  Timer t = {0};
  t.cookie = 11;
  t.kind = KIND_COUNTDOWN;
  t.duration_s = 300;
  strncpy(t.name, "Tea", sizeof t.name - 1);

  uint8_t buf[SERIALIZE_TIMER_WIRE_SIZE];
  size_t written = serialize_timer(&t, buf, sizeof buf);
  TEST_ASSERT_TRUE(written > 0);

  Timer out = {0};
  TEST_ASSERT_TRUE(deserialize_timer(buf, written, &out));
  TEST_ASSERT_EQUAL_STRING("Tea", out.name);
}

void test_round_trip_name_less_stays_name_less(void) {
  Timer t = {0};
  t.kind = KIND_STOPWATCH;
  uint8_t buf[SERIALIZE_TIMER_WIRE_SIZE];
  size_t written = serialize_timer(&t, buf, sizeof buf);

  Timer out;
  memset(&out, 0xAA, sizeof out);  // poison first -- a lingering nonzero name would show
  TEST_ASSERT_TRUE(deserialize_timer(buf, written, &out));
  TEST_ASSERT_EQUAL_STRING("", out.name);
}

// The one genuinely load-bearing case here: it rides the path that drives
// real alarms -- a real, already-persisted V1 (tag 0xA5) blob, the exact
// 41-byte layout with no name section at all, must still load correctly
// and name-less through the same deserialize_timer() that also reads V2.
void test_deserialize_legacy_v1_blob_loads_name_less(void) {
  uint8_t buf[SERIALIZE_TIMER_HEADER_SIZE] = {0};
  buf[0] = 0xA5;  // WIRE_TAG_V1
  buf[1] = KIND_REPEAT;
  buf[2] = 1;  // is_paused
  buf[3] = 0;  // lap_count
  put_test_i32(&buf[4], 42);      // cookie
  put_test_i32(&buf[8], 900);     // duration_s
  put_test_i64(&buf[12], 1000);   // started_at
  put_test_i64(&buf[20], 1500);   // paused_at
  put_test_i32(&buf[28], 30);     // accumulated_pause_s
  buf[32] = 1;                    // fired
  put_test_i32(&buf[33], 7);      // cycles
  put_test_i32(&buf[37], 225);    // original_total_s

  Timer out;
  TEST_ASSERT_TRUE(deserialize_timer(buf, sizeof buf, &out));
  TEST_ASSERT_EQUAL_INT32(42, out.cookie);
  TEST_ASSERT_EQUAL(KIND_REPEAT, out.kind);
  TEST_ASSERT_TRUE(out.is_paused);
  TEST_ASSERT_EQUAL_INT32(900, out.duration_s);
  TEST_ASSERT_EQUAL_INT32(30, out.accumulated_pause_s);
  TEST_ASSERT_TRUE(out.fired);
  TEST_ASSERT_EQUAL_INT32(7, out.cycles);
  TEST_ASSERT_EQUAL_INT32(225, out.original_total_s);
  // The load-bearing assertion: an old blob with no name section at all
  // loads name-less, not garbage/corrupt/crashed.
  TEST_ASSERT_EQUAL_STRING("", out.name);
  // Migration fallback: a V1 blob predates set_total_s entirely -- the best
  // available approximation is duration_s itself.
  TEST_ASSERT_EQUAL_INT32(900, out.set_total_s);
}

// Migration fallback: a V2 blob (name-bearing) ALSO predates set_total_s --
// same fallback applies. Hand-built byte-for-byte, standing in for a real
// pre-upgrade persisted record (serialize_timer() itself only ever writes
// the CURRENT, V3 format now).
void test_deserialize_legacy_v2_blob_falls_back_set_total_s_to_duration(void) {
  uint8_t buf[SERIALIZE_TIMER_HEADER_SIZE_V2] = {0};
  buf[0] = 0xA6;  // WIRE_TAG_V2
  buf[1] = KIND_COUNTDOWN;
  put_test_i32(&buf[4], 5);     // cookie
  put_test_i32(&buf[8], 600);   // duration_s (10:00)
  memcpy(&buf[SERIALIZE_TIMER_HEADER_SIZE], "Tea", 3);

  Timer out;
  TEST_ASSERT_TRUE(deserialize_timer(buf, sizeof buf, &out));
  TEST_ASSERT_EQUAL_INT32(600, out.duration_s);
  TEST_ASSERT_EQUAL_STRING("Tea", out.name);
  TEST_ASSERT_EQUAL_INT32(600, out.set_total_s);  // fallback: duration_s
}

void test_deserialize_legacy_v1_blob_with_laps_loads_correctly(void) {
  uint8_t buf[SERIALIZE_TIMER_HEADER_SIZE + 4 * 2] = {0};
  buf[0] = 0xA5;  // WIRE_TAG_V1
  buf[1] = KIND_STOPWATCH;
  buf[3] = 2;  // lap_count
  put_test_i32(&buf[SERIALIZE_TIMER_HEADER_SIZE + 0], 15);
  put_test_i32(&buf[SERIALIZE_TIMER_HEADER_SIZE + 4], 40);

  Timer out;
  TEST_ASSERT_TRUE(deserialize_timer(buf, sizeof buf, &out));
  TEST_ASSERT_EQUAL_UINT8(2, out.lap_count);
  int32_t lap;
  TEST_ASSERT_TRUE(store_lap_get(&s_loaded, &out, 0, &lap));
  TEST_ASSERT_EQUAL_INT32(15, lap);
  TEST_ASSERT_TRUE(store_lap_get(&s_loaded, &out, 1, &lap));
  TEST_ASSERT_EQUAL_INT32(40, lap);
  TEST_ASSERT_EQUAL_STRING("", out.name);
}

void test_fifth_persisted_lap_history_drops_laps_not_timer(void) {
  TimerStore loaded;
  store_init(&loaded);
  for (int i = 0; i < MAX_LAP_SETS + 1; i++) {
    TimerStore source;
    store_init(&source);
    source.next_cookie = i + 1;
    int32_t cookie;
    TEST_ASSERT_TRUE(store_add(&source, KIND_STOPWATCH, 0, 1000, &cookie));
    Timer *timer = store_find(&source, cookie);
    TEST_ASSERT_TRUE(store_add_lap(&source, timer, 1010 + i));
    uint8_t buf[SERIALIZE_TIMER_WIRE_SIZE];
    size_t written = serialize_timer_with_store(&source, timer, buf, sizeof buf);
    Timer out;
    bool dropped = false;
    TEST_ASSERT_TRUE(deserialize_timer_with_store(buf, written, &loaded, &out, &dropped));
    TEST_ASSERT_EQUAL_INT32(cookie, out.cookie);
    TEST_ASSERT_EQUAL_UINT8(i < MAX_LAP_SETS ? 1 : 0, out.lap_count);
    TEST_ASSERT_EQUAL(i >= MAX_LAP_SETS, dropped);
  }
}

void test_deserialize_v2_rejects_buffer_too_short_for_the_name_section(void) {
  // A V2-tagged blob truncated before its own name section is a structural
  // short-buffer failure, not a partial/garbled name -- refused outright,
  // same discipline as every other structural check here.
  uint8_t buf[SERIALIZE_TIMER_HEADER_SIZE + 5] = {0};  // tag says V2, too short for it
  buf[0] = 0xA6;  // WIRE_TAG_V2
  buf[1] = KIND_COUNTDOWN;
  Timer out;
  TEST_ASSERT_FALSE(deserialize_timer(buf, sizeof buf, &out));
}

void test_deserialize_v2_non_terminated_name_is_capped_not_overrun(void) {
  // Defense in depth: a corrupt/foreign blob's name bytes might carry no NUL
  // at all within TIMER_NAME_MAX. deserialize_timer must still produce a
  // valid, safely-terminated string, never read past the field.
  Timer t = {0};
  t.kind = KIND_COUNTDOWN;
  uint8_t buf[SERIALIZE_TIMER_WIRE_SIZE];
  size_t written = serialize_timer(&t, buf, sizeof buf);
  memset(&buf[SERIALIZE_TIMER_HEADER_SIZE], 'x', TIMER_NAME_MAX);  // no NUL anywhere in the field

  Timer out;
  TEST_ASSERT_TRUE(deserialize_timer(buf, written, &out));
  TEST_ASSERT_EQUAL_size_t(TIMER_NAME_MAX - 1, strlen(out.name));  // capped, still a valid C string
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_round_trip_basic_countdown);
  RUN_TEST(test_round_trip_paused_fired_flags);
  RUN_TEST(test_round_trip_with_laps);
  RUN_TEST(test_serialize_refuses_too_small_buffer);
  RUN_TEST(test_deserialize_rejects_bad_tag);
  RUN_TEST(test_deserialize_rejects_short_buffer);
  RUN_TEST(test_deserialize_rejects_invalid_kind);
  RUN_TEST(test_deserialize_rejects_lap_count_over_max);
  RUN_TEST(test_round_trip_preserves_snoozing_repeat);
  RUN_TEST(test_round_trip_preserves_set_total_s_for_quarter_amount);
  RUN_TEST(test_serialize_always_writes_current_v3_tag);
  RUN_TEST(test_round_trip_v3_preserves_set_total_s_distinct_from_duration);
  RUN_TEST(test_round_trip_with_name);
  RUN_TEST(test_round_trip_name_less_stays_name_less);
  RUN_TEST(test_deserialize_legacy_v1_blob_loads_name_less);
  RUN_TEST(test_deserialize_legacy_v1_blob_with_laps_loads_correctly);
  RUN_TEST(test_fifth_persisted_lap_history_drops_laps_not_timer);
  RUN_TEST(test_deserialize_legacy_v2_blob_falls_back_set_total_s_to_duration);
  RUN_TEST(test_deserialize_v2_rejects_buffer_too_short_for_the_name_section);
  RUN_TEST(test_deserialize_v2_non_terminated_name_is_capped_not_overrun);
  return UNITY_END();
}
