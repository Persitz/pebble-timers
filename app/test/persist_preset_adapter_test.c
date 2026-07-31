// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// Stateful fake-flash tests for the preset adapter's one-blob save. A failed
// or short write must produce a failure marker, never a persisted-count marker.
#include "persist_preset_adapter.h"

#include <string.h>

#include "fff.h"
#include "lib/preset_serialize.h"
#include "lib/preset_store.h"
#include "pebble.h"
#include "testlog_am.h"
#include "unity.h"

DEFINE_FFF_GLOBALS;

FAKE_VALUE_FUNC(bool, persist_exists, uint32_t);
FAKE_VALUE_FUNC(int, persist_read_data, uint32_t, void *, size_t);
FAKE_VALUE_FUNC(int, persist_write_data, uint32_t, const void *, size_t);
FAKE_VOID_FUNC(testlog, const char *, const char *);

static bool s_flash_exists;
static uint8_t s_flash[PRESET_SERIALIZE_WIRE_SIZE];
static int s_flash_len;
static int s_write_result;
static bool s_success_seen;
static bool s_failure_seen;

static bool fake_persist_exists(uint32_t key) {
  return key == PERSIST_KEY_PRESETS && s_flash_exists;
}

static int fake_persist_read_data(uint32_t key, void *buffer, size_t buffer_size) {
  if (key != PERSIST_KEY_PRESETS || !s_flash_exists) return -1;
  size_t copy_len = (size_t)s_flash_len < buffer_size ? (size_t)s_flash_len : buffer_size;
  memcpy(buffer, s_flash, copy_len);
  return s_flash_len;
}

static int fake_persist_write_data(uint32_t key, const void *data, size_t size) {
  if (key != PERSIST_KEY_PRESETS || s_write_result != 0) {
    return s_write_result;
  }
  memcpy(s_flash, data, size);
  s_flash_len = (int)size;
  s_flash_exists = true;
  return (int)size;
}

static void capture_testlog(const char *tag, const char *data) {
  (void)data;
  if (tag != NULL && strcmp(tag, "preset_persist") == 0) s_success_seen = true;
  if (tag != NULL && strcmp(tag, "preset_persist_fail") == 0) s_failure_seen = true;
}

void setUp(void) {
  RESET_FAKE(persist_exists);
  RESET_FAKE(persist_read_data);
  RESET_FAKE(persist_write_data);
  RESET_FAKE(testlog);
  FFF_RESET_HISTORY();
  s_flash_exists = false;
  s_flash_len = 0;
  s_write_result = 0;
  s_success_seen = false;
  s_failure_seen = false;
  persist_exists_fake.custom_fake = fake_persist_exists;
  persist_read_data_fake.custom_fake = fake_persist_read_data;
  persist_write_data_fake.custom_fake = fake_persist_write_data;
  testlog_fake.custom_fake = capture_testlog;
}
void tearDown(void) {}

static void add_preset(PresetStore *store, TimerKind kind, int32_t duration_s) {
  TEST_ASSERT_TRUE(preset_store_add(store, kind, duration_s));
}

void test_successful_save_round_trips_and_emits_success(void) {
  PresetStore store;
  preset_store_init(&store);
  add_preset(&store, KIND_COUNTDOWN, 300);
  add_preset(&store, KIND_REPEAT, 600);

  persist_preset_adapter_save(&store);
  TEST_ASSERT_TRUE(s_success_seen);
  TEST_ASSERT_FALSE(s_failure_seen);

  PresetStore reloaded;
  persist_preset_adapter_load(&reloaded);
  TEST_ASSERT_EQUAL_INT(2, reloaded.count);
  TEST_ASSERT_EQUAL(KIND_COUNTDOWN, reloaded.presets[0].kind);
  TEST_ASSERT_EQUAL_INT32(300, reloaded.presets[0].duration_s);
  TEST_ASSERT_EQUAL(KIND_REPEAT, reloaded.presets[1].kind);
  TEST_ASSERT_EQUAL_INT32(600, reloaded.presets[1].duration_s);
}

void test_failed_write_keeps_previous_blob_and_emits_failure_not_success(void) {
  PresetStore store;
  preset_store_init(&store);
  add_preset(&store, KIND_COUNTDOWN, 300);
  persist_preset_adapter_save(&store);

  s_success_seen = false;
  add_preset(&store, KIND_REPEAT, 600);
  s_write_result = -1;
  persist_preset_adapter_save(&store);

  TEST_ASSERT_FALSE(s_success_seen);
  TEST_ASSERT_TRUE(s_failure_seen);
  PresetStore reloaded;
  persist_preset_adapter_load(&reloaded);
  TEST_ASSERT_EQUAL_INT(1, reloaded.count);
  TEST_ASSERT_EQUAL_INT32(300, reloaded.presets[0].duration_s);
}

void test_short_write_emits_failure_not_success(void) {
  PresetStore store;
  preset_store_init(&store);
  add_preset(&store, KIND_COUNTDOWN, 300);
  s_write_result = 1;

  persist_preset_adapter_save(&store);

  TEST_ASSERT_FALSE(s_success_seen);
  TEST_ASSERT_TRUE(s_failure_seen);
  TEST_ASSERT_FALSE(s_flash_exists);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_successful_save_round_trips_and_emits_success);
  RUN_TEST(test_failed_write_keeps_previous_blob_and_emits_failure_not_success);
  RUN_TEST(test_short_write_emits_failure_not_success);
  return UNITY_END();
}
