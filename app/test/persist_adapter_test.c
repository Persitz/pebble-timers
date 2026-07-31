// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// Stateful fake-flash tests for persist_adapter_save()'s final-manifest
// commit rule. They ensure a failed status or short record write produces a
// failure marker, never the success marker derived from the in-memory store.
#include "persist_adapter.h"

#include <string.h>

#include "fff.h"
#include "lib/serialize.h"
#include "lib/store.h"
#include "lib/timer.h"
#include "pebble.h"  // test/fakes/pebble.h -- see test/Makefile's -I ordering
#include "testlog_am.h"
#include "unity.h"

DEFINE_FFF_GLOBALS;

FAKE_VALUE_FUNC(bool, persist_exists, uint32_t);
FAKE_VALUE_FUNC(int32_t, persist_read_int, uint32_t);
FAKE_VALUE_FUNC(status_t, persist_write_int, uint32_t, int32_t);
FAKE_VALUE_FUNC(int, persist_read_data, uint32_t, void *, size_t);
FAKE_VALUE_FUNC(int, persist_write_data, uint32_t, const void *, size_t);
FAKE_VALUE_FUNC(status_t, persist_delete, uint32_t);
FAKE_VOID_FUNC(testlog, const char *, const char *);
FAKE_VOID_FUNC(testlog_init);

#define PERSIST_KEY_COUNT 0
#define PERSIST_KEY_NEXT_COOKIE 1
#define PERSIST_KEY_TIMER_BASE 2

// -- A small stateful fake flash, so a save's failure and a later load's
// consequence are connected through something resembling real persistent
// storage, not independent canned return values. ---------------------------
#define FAKE_FLASH_MAX_KEY 32
static bool s_flash_exists[FAKE_FLASH_MAX_KEY];
static int32_t s_flash_int[FAKE_FLASH_MAX_KEY];
static uint8_t s_flash_data[FAKE_FLASH_MAX_KEY][SERIALIZE_TIMER_WIRE_SIZE];
static int s_flash_data_len[FAKE_FLASH_MAX_KEY];

// Failure injection: set true for a key to make its NEXT write silently fail
// (return an error, do NOT touch the fake flash's stored value) -- exactly
// modeling a real settings-file write failure, which persist_adapter.c never
// checks for.
static bool s_fail_write_int[FAKE_FLASH_MAX_KEY];
static bool s_fail_write_data[FAKE_FLASH_MAX_KEY];
static bool s_short_write_data[FAKE_FLASH_MAX_KEY];
static bool s_fail_delete[FAKE_FLASH_MAX_KEY];

static void flash_reset(void) {
  memset(s_flash_exists, 0, sizeof s_flash_exists);
  memset(s_flash_int, 0, sizeof s_flash_int);
  memset(s_flash_data_len, 0, sizeof s_flash_data_len);
  memset(s_fail_write_int, 0, sizeof s_fail_write_int);
  memset(s_fail_write_data, 0, sizeof s_fail_write_data);
  memset(s_short_write_data, 0, sizeof s_short_write_data);
  memset(s_fail_delete, 0, sizeof s_fail_delete);
}

static bool fake_persist_exists(uint32_t key) {
  return key < FAKE_FLASH_MAX_KEY && s_flash_exists[key];
}

static int32_t fake_persist_read_int(uint32_t key) {
  return (key < FAKE_FLASH_MAX_KEY) ? s_flash_int[key] : 0;
}

static status_t fake_persist_write_int(uint32_t key, int32_t value) {
  if (key >= FAKE_FLASH_MAX_KEY) return -1;
  if (s_fail_write_int[key]) return -1;  // simulated failure -- old value (if any) unchanged
  s_flash_int[key] = value;
  s_flash_exists[key] = true;
  // Real firmware returns the byte count on success (4, for an int32_t), not
  // literally 0 -- confirmed empirically against real qemu-pebble firmware
  // (see persist_adapter.c's own comment at its two
  // persist_write_int() call sites). This fake previously returned 0 here,
  // which matched persist_adapter.c's THEN-buggy `!= 0` check closely enough
  // that no host test ever caught the mismatch -- returning the real value
  // is what makes this fake an honest stand-in again.
  return (status_t)sizeof(int32_t);
}

static int fake_persist_read_data(uint32_t key, void *buffer, size_t buffer_size) {
  if (key >= FAKE_FLASH_MAX_KEY || !s_flash_exists[key]) return -1;
  int len = s_flash_data_len[key];
  size_t copy_len = (size_t)len > buffer_size ? buffer_size : (size_t)len;
  memcpy(buffer, s_flash_data[key], copy_len);
  return len;
}

static int fake_persist_write_data(uint32_t key, const void *data, size_t size) {
  if (key >= FAKE_FLASH_MAX_KEY || size > sizeof(s_flash_data[0])) return -1;
  if (s_fail_write_data[key]) return -1;  // simulated failure -- old slot (if any) unchanged
  if (s_short_write_data[key]) return (int)size - 1;
  memcpy(s_flash_data[key], data, size);
  s_flash_data_len[key] = (int)size;
  s_flash_exists[key] = true;
  return (int)size;
}

static status_t fake_persist_delete(uint32_t key) {
  if (key >= FAKE_FLASH_MAX_KEY || s_fail_delete[key]) return -1;
  if (key < FAKE_FLASH_MAX_KEY) {
    s_flash_exists[key] = false;
    s_flash_data_len[key] = 0;
  }
  return 1;  // S_TRUE, the documented persist_delete() success result
}

// testlog()'s `data` argument points at a STACK-local buffer in the caller
// (persist_adapter.c's own `char data[8]`) that goes out of scope the
// instant testlog() returns -- FFF's own arg1_history only records the raw
// pointer VALUE at call time, not a copy of what it pointed to, so reading
// it back later (after the stack has been reused) reads garbage. Capture the
// content HERE, inside the fake, while the pointer is still valid.
static bool s_persist_marker_seen;
static bool s_persist_failure_seen;
static char s_last_persist_data[16];

static void capture_testlog(const char *tag, const char *data) {
  if (tag != NULL && strcmp(tag, "persist") == 0) {
    s_persist_marker_seen = true;
    snprintf(s_last_persist_data, sizeof s_last_persist_data, "%s", data != NULL ? data : "");
  }
  if (tag != NULL && strcmp(tag, "persist_failed") == 0) s_persist_failure_seen = true;
}

void setUp(void) {
  RESET_FAKE(persist_exists);
  RESET_FAKE(persist_read_int);
  RESET_FAKE(persist_write_int);
  RESET_FAKE(persist_read_data);
  RESET_FAKE(persist_write_data);
  RESET_FAKE(persist_delete);
  RESET_FAKE(testlog);
  RESET_FAKE(testlog_init);
  FFF_RESET_HISTORY();
  flash_reset();
  s_persist_marker_seen = false;
  s_persist_failure_seen = false;
  s_last_persist_data[0] = '\0';
  persist_exists_fake.custom_fake = fake_persist_exists;
  persist_read_int_fake.custom_fake = fake_persist_read_int;
  persist_write_int_fake.custom_fake = fake_persist_write_int;
  persist_read_data_fake.custom_fake = fake_persist_read_data;
  persist_write_data_fake.custom_fake = fake_persist_write_data;
  persist_delete_fake.custom_fake = fake_persist_delete;
  testlog_fake.custom_fake = capture_testlog;
}
void tearDown(void) {}

#define NOW ((time_t)1000000)

static void add_timer(TimerStore *s, TimerKind kind, int32_t duration_s) {
  int32_t cookie;
  TEST_ASSERT_TRUE(store_add(s, kind, duration_s, NOW, &cookie));
}

static void assert_unique_cookies(const TimerStore *s) {
  for (int i = 0; i < s->count; i++) {
    for (int j = i + 1; j < s->count; j++) {
      TEST_ASSERT_NOT_EQUAL_INT32(s->timers[i].cookie, s->timers[j].cookie);
    }
  }
}

// -- Sanity baseline: a clean round trip with every write succeeding -------

void test_round_trip_all_writes_succeed(void) {
  TimerStore s;
  store_init(&s);
  add_timer(&s, KIND_COUNTDOWN, 300);
  add_timer(&s, KIND_REPEAT, 600);

  persist_adapter_save(&s);
  TEST_ASSERT_TRUE(s_persist_marker_seen);
  TEST_ASSERT_EQUAL_STRING("2", s_last_persist_data);

  TimerStore reloaded;
  persist_adapter_load(&reloaded);

  TEST_ASSERT_EQUAL_INT(2, reloaded.count);
  TEST_ASSERT_EQUAL(KIND_COUNTDOWN, reloaded.timers[0].kind);
  TEST_ASSERT_EQUAL_INT32(300, reloaded.timers[0].duration_s);
  TEST_ASSERT_EQUAL(KIND_REPEAT, reloaded.timers[1].kind);
  TEST_ASSERT_EQUAL_INT32(600, reloaded.timers[1].duration_s);
}

// -- Failed writes must never emit the success marker ------------------------

void test_failed_writes_emit_failure_not_success_marker(void) {
  TimerStore s;
  store_init(&s);
  add_timer(&s, KIND_COUNTDOWN, 300);
  add_timer(&s, KIND_COUNTDOWN, 60);

  // Fail EVERY underlying write this save will attempt.
  s_fail_write_int[PERSIST_KEY_COUNT] = true;
  s_fail_write_int[PERSIST_KEY_NEXT_COOKIE] = true;
  s_fail_write_data[PERSIST_KEY_TIMER_BASE + 0] = true;
  s_fail_write_data[PERSIST_KEY_TIMER_BASE + 1] = true;

  persist_adapter_save(&s);

  // Nothing actually landed in the fake flash...
  TEST_ASSERT_FALSE(fake_persist_exists(PERSIST_KEY_COUNT));
  TEST_ASSERT_FALSE(fake_persist_exists(PERSIST_KEY_TIMER_BASE + 0));
  TEST_ASSERT_FALSE(fake_persist_exists(PERSIST_KEY_TIMER_BASE + 1));

  TEST_ASSERT_FALSE(s_persist_marker_seen);
  TEST_ASSERT_TRUE(s_persist_failure_seen);
}

// -- A failed append leaves the prior committed manifest authoritative ------

void test_failed_new_slot_keeps_prior_committed_store_on_reload(void) {
  TimerStore s;
  store_init(&s);
  add_timer(&s, KIND_COUNTDOWN, 300);
  persist_adapter_save(&s);
  TEST_ASSERT_TRUE(s_persist_marker_seen);

  s_persist_marker_seen = false;
  add_timer(&s, KIND_COUNTDOWN, 60);  // this new slot write will fail

  s_fail_write_data[PERSIST_KEY_TIMER_BASE + 1] = true;

  persist_adapter_save(&s);
  TEST_ASSERT_FALSE(s_persist_marker_seen);
  TEST_ASSERT_TRUE(s_persist_failure_seen);

  TimerStore reloaded;
  persist_adapter_load(&reloaded);

  // The old manifest count remains authoritative, so the incomplete append
  // cannot publish a partially populated two-timer store.
  TEST_ASSERT_EQUAL_INT(1, reloaded.count);
  TEST_ASSERT_EQUAL_INT32(300, reloaded.timers[0].duration_s);
}

void test_short_slot_write_emits_failure_not_success_marker(void) {
  TimerStore s;
  store_init(&s);
  add_timer(&s, KIND_COUNTDOWN, 100);

  s_short_write_data[PERSIST_KEY_TIMER_BASE] = true;

  persist_adapter_save(&s);
  TEST_ASSERT_FALSE(s_persist_marker_seen);
  TEST_ASSERT_TRUE(s_persist_failure_seen);
}

void test_failed_next_cookie_does_not_publish_manifest(void) {
  TimerStore s;
  store_init(&s);
  add_timer(&s, KIND_COUNTDOWN, 100);

  s_fail_write_int[PERSIST_KEY_NEXT_COOKIE] = true;
  persist_adapter_save(&s);

  TEST_ASSERT_FALSE(s_persist_marker_seen);
  TEST_ASSERT_TRUE(s_persist_failure_seen);

  TimerStore reloaded;
  persist_adapter_load(&reloaded);
  TEST_ASSERT_EQUAL_INT(0, reloaded.count);
}

void test_failed_manifest_does_not_report_success(void) {
  TimerStore s;
  store_init(&s);
  add_timer(&s, KIND_COUNTDOWN, 100);

  s_fail_write_int[PERSIST_KEY_COUNT] = true;
  persist_adapter_save(&s);

  TEST_ASSERT_FALSE(s_persist_marker_seen);
  TEST_ASSERT_TRUE(s_persist_failure_seen);
}

void test_successful_shrinking_save_clears_stale_slots(void) {
  TimerStore s;
  store_init(&s);
  add_timer(&s, KIND_COUNTDOWN, 100);
  add_timer(&s, KIND_COUNTDOWN, 200);
  persist_adapter_save(&s);

  TEST_ASSERT_TRUE(store_remove(&s, s.timers[0].cookie));
  s_persist_marker_seen = false;
  persist_adapter_save(&s);

  TEST_ASSERT_TRUE(s_persist_marker_seen);
  TEST_ASSERT_FALSE(fake_persist_exists(PERSIST_KEY_TIMER_BASE + 1));
}

void test_failed_stale_delete_suppresses_success_marker(void) {
  TimerStore s;
  store_init(&s);
  add_timer(&s, KIND_COUNTDOWN, 100);
  add_timer(&s, KIND_COUNTDOWN, 200);
  persist_adapter_save(&s);

  TEST_ASSERT_TRUE(store_remove(&s, s.timers[0].cookie));
  s_persist_marker_seen = false;
  s_fail_delete[PERSIST_KEY_TIMER_BASE + 1] = true;
  persist_adapter_save(&s);

  TEST_ASSERT_FALSE(s_persist_marker_seen);
  TEST_ASSERT_TRUE(s_persist_failure_seen);
}

void test_failed_shrinking_reindex_save_never_restores_duplicate_cookie(void) {
  TimerStore s;
  store_init(&s);
  add_timer(&s, KIND_COUNTDOWN, 100);
  add_timer(&s, KIND_COUNTDOWN, 200);
  add_timer(&s, KIND_COUNTDOWN, 300);
  persist_adapter_save(&s);

  int32_t removed_cookie = s.timers[0].cookie;
  TEST_ASSERT_TRUE(store_remove(&s, removed_cookie));
  s_persist_marker_seen = false;
  s_fail_write_int[PERSIST_KEY_NEXT_COOKIE] = true;

  persist_adapter_save(&s);
  TEST_ASSERT_FALSE(s_persist_marker_seen);
  TEST_ASSERT_TRUE(s_persist_failure_seen);

  TimerStore reloaded;
  persist_adapter_load(&reloaded);

  // The old count still exposes stale slot 2, which duplicates the timer
  // shifted from slot 2 to slot 1. Loader recovery retains it only once.
  TEST_ASSERT_EQUAL_INT(2, reloaded.count);
  TEST_ASSERT_EQUAL_INT32(200, reloaded.timers[0].duration_s);
  TEST_ASSERT_EQUAL_INT32(300, reloaded.timers[1].duration_s);
  assert_unique_cookies(&reloaded);
}

void test_failed_middle_slot_write_keeps_unique_recovery_store(void) {
  TimerStore s;
  store_init(&s);
  add_timer(&s, KIND_COUNTDOWN, 100);
  add_timer(&s, KIND_COUNTDOWN, 200);
  add_timer(&s, KIND_COUNTDOWN, 300);
  persist_adapter_save(&s);

  s.timers[0].duration_s = 101;
  s.timers[1].duration_s = 201;
  s.timers[2].duration_s = 301;
  s_persist_marker_seen = false;
  s_fail_write_data[PERSIST_KEY_TIMER_BASE + 1] = true;

  persist_adapter_save(&s);
  TEST_ASSERT_FALSE(s_persist_marker_seen);
  TEST_ASSERT_TRUE(s_persist_failure_seen);

  TimerStore reloaded;
  persist_adapter_load(&reloaded);

  // Writes either side of the failed middle slot may have landed, but load
  // keeps all surviving identities unique and in slot order.
  TEST_ASSERT_EQUAL_INT(3, reloaded.count);
  TEST_ASSERT_EQUAL_INT32(101, reloaded.timers[0].duration_s);
  TEST_ASSERT_EQUAL_INT32(200, reloaded.timers[1].duration_s);
  TEST_ASSERT_EQUAL_INT32(301, reloaded.timers[2].duration_s);
  assert_unique_cookies(&reloaded);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_round_trip_all_writes_succeed);
  RUN_TEST(test_failed_writes_emit_failure_not_success_marker);
  RUN_TEST(test_failed_new_slot_keeps_prior_committed_store_on_reload);
  RUN_TEST(test_short_slot_write_emits_failure_not_success_marker);
  RUN_TEST(test_failed_next_cookie_does_not_publish_manifest);
  RUN_TEST(test_failed_manifest_does_not_report_success);
  RUN_TEST(test_successful_shrinking_save_clears_stale_slots);
  RUN_TEST(test_failed_stale_delete_suppresses_success_marker);
  RUN_TEST(test_failed_shrinking_reindex_save_never_restores_duplicate_cookie);
  RUN_TEST(test_failed_middle_slot_write_keeps_unique_recovery_store);
  return UNITY_END();
}
