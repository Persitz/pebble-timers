// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

#include "persist_kind_adapter.h"

#include "fff.h"
#include "pebble.h"
#include "unity.h"

DEFINE_FFF_GLOBALS;

FAKE_VALUE_FUNC(bool, persist_exists, uint32_t);
FAKE_VALUE_FUNC(int, persist_read_data, uint32_t, void *, size_t);
FAKE_VALUE_FUNC(int, persist_write_data, uint32_t, const void *, size_t);

#define PERSIST_KEY_LAST_DIAL_KIND 31

static bool s_flash_exists;
static uint8_t s_flash_value;
static int s_write_result;

static bool fake_persist_exists(uint32_t key) {
  return key == PERSIST_KEY_LAST_DIAL_KIND && s_flash_exists;
}

static int fake_persist_read_data(uint32_t key, void *buffer, size_t buffer_size) {
  if (key != PERSIST_KEY_LAST_DIAL_KIND || !s_flash_exists || buffer_size < sizeof s_flash_value) return -1;
  *(uint8_t *)buffer = s_flash_value;
  return sizeof s_flash_value;
}

static int fake_persist_write_data(uint32_t key, const void *data, size_t size) {
  if (key != PERSIST_KEY_LAST_DIAL_KIND || size != sizeof s_flash_value || s_write_result != 0) {
    return s_write_result;
  }
  s_flash_value = *(const uint8_t *)data;
  s_flash_exists = true;
  return (int)size;
}

void setUp(void) {
  RESET_FAKE(persist_exists);
  RESET_FAKE(persist_read_data);
  RESET_FAKE(persist_write_data);
  FFF_RESET_HISTORY();
  s_flash_exists = false;
  s_flash_value = 0;
  s_write_result = 0;
  persist_exists_fake.custom_fake = fake_persist_exists;
  persist_read_data_fake.custom_fake = fake_persist_read_data;
  persist_write_data_fake.custom_fake = fake_persist_write_data;
  persist_kind_adapter_load();
}
void tearDown(void) {}

void test_successful_save_updates_runtime_default_and_reloads(void) {
  persist_kind_adapter_save(KIND_REPEAT);

  TEST_ASSERT_EQUAL(KIND_REPEAT, persist_kind_adapter_last());
  persist_kind_adapter_load();
  TEST_ASSERT_EQUAL(KIND_REPEAT, persist_kind_adapter_last());
}

void test_failed_write_still_honours_the_choice_for_this_session(void) {
  // This used to assert the opposite (a failed write left the
  // in-memory value at the PREVIOUS kind, for flash-consistency). That made a
  // flash failure produce a visible wrong answer -- the user picks Countdown
  // and the very next SET screen in the same session opens on Repeat --
  // trading an unobservable inconsistency for an observable one. The
  // in-memory value is a UX convenience ("open the dial on the kind you last
  // used"), so it now follows the user's actual choice regardless of flash.
  persist_kind_adapter_save(KIND_REPEAT);
  s_write_result = -1;
  persist_kind_adapter_save(KIND_COUNTDOWN);

  TEST_ASSERT_EQUAL(KIND_COUNTDOWN, persist_kind_adapter_last());
}

void test_failed_write_leaves_the_persisted_value_untouched(void) {
  // The other half, unchanged: a failed write must not corrupt what is on
  // disk, so a relaunch still reads the last value that really committed.
  persist_kind_adapter_save(KIND_REPEAT);
  s_write_result = -1;
  persist_kind_adapter_save(KIND_COUNTDOWN);

  persist_kind_adapter_load();  // "next launch"
  TEST_ASSERT_EQUAL(KIND_REPEAT, persist_kind_adapter_last());
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_successful_save_updates_runtime_default_and_reloads);
  RUN_TEST(test_failed_write_still_honours_the_choice_for_this_session);
  RUN_TEST(test_failed_write_leaves_the_persisted_value_untouched);
  return UNITY_END();
}
