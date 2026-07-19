// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

#include "preset_store.h"
#include "unity.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

void test_init_is_empty(void) {
  PresetStore s;
  preset_store_init(&s);
  TEST_ASSERT_EQUAL_INT(0, s.count);
}

void test_add_appends(void) {
  PresetStore s;
  preset_store_init(&s);
  TEST_ASSERT_TRUE(preset_store_add(&s, KIND_COUNTDOWN, 300));
  TEST_ASSERT_EQUAL_INT(1, s.count);
  TEST_ASSERT_EQUAL(KIND_COUNTDOWN, s.presets[0].kind);
  TEST_ASSERT_EQUAL_INT32(300, s.presets[0].duration_s);
}

void test_add_refuses_past_max_presets(void) {
  PresetStore s;
  preset_store_init(&s);
  for (int i = 0; i < MAX_PRESETS; i++) {
    TEST_ASSERT_TRUE(preset_store_add(&s, KIND_COUNTDOWN, 60));
  }
  TEST_ASSERT_EQUAL_INT(MAX_PRESETS, s.count);
  // The 13th add must be refused, not silently dropped/grown.
  TEST_ASSERT_FALSE(preset_store_add(&s, KIND_COUNTDOWN, 60));
  TEST_ASSERT_EQUAL_INT(MAX_PRESETS, s.count);
}

void test_add_refuses_stopwatch_kind(void) {
  PresetStore s;
  preset_store_init(&s);
  TEST_ASSERT_FALSE(preset_store_add(&s, KIND_STOPWATCH, 60));
  TEST_ASSERT_EQUAL_INT(0, s.count);
}

void test_add_refuses_zero_duration(void) {
  PresetStore s;
  preset_store_init(&s);
  TEST_ASSERT_FALSE(preset_store_add(&s, KIND_COUNTDOWN, 0));
  TEST_ASSERT_EQUAL_INT(0, s.count);
}

void test_add_refuses_negative_duration(void) {
  PresetStore s;
  preset_store_init(&s);
  TEST_ASSERT_FALSE(preset_store_add(&s, KIND_REPEAT, -1));
  TEST_ASSERT_EQUAL_INT(0, s.count);
}

// --- duration upper bound ---

void test_add_refuses_duration_over_max(void) {
  PresetStore s;
  preset_store_init(&s);
  TEST_ASSERT_FALSE(preset_store_add(&s, KIND_COUNTDOWN, TIMER_MAX_DURATION_S + 1));
  TEST_ASSERT_EQUAL_INT(0, s.count);
}

void test_add_accepts_duration_at_max_boundary(void) {
  PresetStore s;
  preset_store_init(&s);
  TEST_ASSERT_TRUE(preset_store_add(&s, KIND_COUNTDOWN, TIMER_MAX_DURATION_S));
  TEST_ASSERT_EQUAL_INT32(TIMER_MAX_DURATION_S, s.presets[0].duration_s);
}

void test_edit_rejects_duration_over_max(void) {
  PresetStore s;
  preset_store_init(&s);
  preset_store_add(&s, KIND_COUNTDOWN, 60);
  TEST_ASSERT_FALSE(preset_store_edit(&s, 0, KIND_COUNTDOWN, TIMER_MAX_DURATION_S + 1));
  // Original untouched by the refused edit.
  TEST_ASSERT_EQUAL_INT32(60, s.presets[0].duration_s);
}

void test_add_accepts_repeat_kind(void) {
  PresetStore s;
  preset_store_init(&s);
  TEST_ASSERT_TRUE(preset_store_add(&s, KIND_REPEAT, 45));
  TEST_ASSERT_EQUAL_INT(1, s.count);
  TEST_ASSERT_EQUAL(KIND_REPEAT, s.presets[0].kind);
}

void test_remove_compacts_array(void) {
  PresetStore s;
  preset_store_init(&s);
  preset_store_add(&s, KIND_COUNTDOWN, 10);
  preset_store_add(&s, KIND_COUNTDOWN, 20);
  preset_store_add(&s, KIND_COUNTDOWN, 30);
  TEST_ASSERT_TRUE(preset_store_remove(&s, 1));  // removes the 20s middle preset
  TEST_ASSERT_EQUAL_INT(2, s.count);
  TEST_ASSERT_EQUAL_INT32(10, s.presets[0].duration_s);
  TEST_ASSERT_EQUAL_INT32(30, s.presets[1].duration_s);
}

void test_remove_out_of_range_returns_false(void) {
  PresetStore s;
  preset_store_init(&s);
  preset_store_add(&s, KIND_COUNTDOWN, 10);
  TEST_ASSERT_FALSE(preset_store_remove(&s, -1));
  TEST_ASSERT_FALSE(preset_store_remove(&s, 1));
  TEST_ASSERT_EQUAL_INT(1, s.count);
}

void test_remove_last_zeroes_the_vacated_slot(void) {
  PresetStore s;
  preset_store_init(&s);
  preset_store_add(&s, KIND_COUNTDOWN, 10);
  preset_store_remove(&s, 0);
  TEST_ASSERT_EQUAL_INT(0, s.count);
  TEST_ASSERT_EQUAL_INT32(0, s.presets[0].duration_s);
}

void test_edit_updates_kind_and_duration(void) {
  PresetStore s;
  preset_store_init(&s);
  preset_store_add(&s, KIND_COUNTDOWN, 60);
  TEST_ASSERT_TRUE(preset_store_edit(&s, 0, KIND_REPEAT, 90));
  TEST_ASSERT_EQUAL(KIND_REPEAT, s.presets[0].kind);
  TEST_ASSERT_EQUAL_INT32(90, s.presets[0].duration_s);
}

void test_edit_out_of_range_returns_false(void) {
  PresetStore s;
  preset_store_init(&s);
  TEST_ASSERT_FALSE(preset_store_edit(&s, 0, KIND_COUNTDOWN, 60));
}

void test_edit_rejects_invalid_kind_or_duration(void) {
  PresetStore s;
  preset_store_init(&s);
  preset_store_add(&s, KIND_COUNTDOWN, 60);
  TEST_ASSERT_FALSE(preset_store_edit(&s, 0, KIND_STOPWATCH, 60));
  TEST_ASSERT_FALSE(preset_store_edit(&s, 0, KIND_COUNTDOWN, 0));
  // Original untouched by the refused edits.
  TEST_ASSERT_EQUAL(KIND_COUNTDOWN, s.presets[0].kind);
  TEST_ASSERT_EQUAL_INT32(60, s.presets[0].duration_s);
}

void test_get_returns_pointer_for_existing(void) {
  PresetStore s;
  preset_store_init(&s);
  preset_store_add(&s, KIND_COUNTDOWN, 90);
  const Preset *p = preset_store_get(&s, 0);
  TEST_ASSERT_NOT_NULL(p);
  TEST_ASSERT_EQUAL_INT32(90, p->duration_s);
}

void test_get_returns_null_out_of_range(void) {
  PresetStore s;
  preset_store_init(&s);
  TEST_ASSERT_NULL(preset_store_get(&s, 0));
  TEST_ASSERT_NULL(preset_store_get(&s, -1));
}

void test_sequence_add_remove_get_integrity(void) {
  PresetStore s;
  preset_store_init(&s);
  preset_store_add(&s, KIND_COUNTDOWN, 60);
  preset_store_add(&s, KIND_REPEAT, 30);
  preset_store_add(&s, KIND_COUNTDOWN, 120);
  TEST_ASSERT_TRUE(preset_store_remove(&s, 1));  // removes the repeat/30
  TEST_ASSERT_EQUAL_INT(2, s.count);
  const Preset *p0 = preset_store_get(&s, 0);
  const Preset *p1 = preset_store_get(&s, 1);
  TEST_ASSERT_EQUAL(KIND_COUNTDOWN, p0->kind);
  TEST_ASSERT_EQUAL_INT32(60, p0->duration_s);
  TEST_ASSERT_EQUAL(KIND_COUNTDOWN, p1->kind);
  TEST_ASSERT_EQUAL_INT32(120, p1->duration_s);
}

// --- preset_store_set_name -------------------------------------------------

void test_new_preset_defaults_name_less(void) {
  PresetStore s;
  preset_store_init(&s);
  preset_store_add(&s, KIND_COUNTDOWN, 60);
  TEST_ASSERT_EQUAL_STRING("", s.presets[0].name);
}

void test_set_name_found_sets_the_name(void) {
  PresetStore s;
  preset_store_init(&s);
  preset_store_add(&s, KIND_COUNTDOWN, 300);
  TEST_ASSERT_TRUE(preset_store_set_name(&s, 0, "Tea"));
  TEST_ASSERT_EQUAL_STRING("Tea", s.presets[0].name);
}

void test_set_name_out_of_range_returns_false(void) {
  PresetStore s;
  preset_store_init(&s);
  preset_store_add(&s, KIND_COUNTDOWN, 60);
  TEST_ASSERT_FALSE(preset_store_set_name(&s, -1, "Tea"));
  TEST_ASSERT_FALSE(preset_store_set_name(&s, 1, "Tea"));
}

void test_set_name_delegates_cap_truncation_to_timer_name_copy(void) {
  // Not re-deriving the UTF-8-safe truncation rule here (already exhaustively
  // covered by test/timer_test.c's own timer_name_copy tests) -- just
  // proving preset_store_set_name actually routes through it, capped at
  // PRESET_NAME_MAX (16, shorter than Timer.name's 24 -- persisted-message
  // size constraints), rather than a raw strncpy.
  PresetStore s;
  preset_store_init(&s);
  preset_store_add(&s, KIND_COUNTDOWN, 60);
  char long_name[64];
  memset(long_name, 'a', sizeof long_name - 1);
  long_name[sizeof long_name - 1] = '\0';
  TEST_ASSERT_TRUE(preset_store_set_name(&s, 0, long_name));
  TEST_ASSERT_EQUAL_INT(PRESET_NAME_MAX - 1, (int)strlen(s.presets[0].name));
}

void test_set_name_utf8_safe_does_not_split_a_multibyte_char_at_the_cap(void) {
  // 15 ASCII 'a's (fills PRESET_NAME_MAX-1) followed by a 2-byte UTF-8
  // codepoint that cannot fully fit -- must be dropped whole, never split
  // into a dangling lead byte (same contract as timer_test.c's
  // test_name_copy_utf8_safe_does_not_split_a_multibyte_char, re-proven at
  // this store's own, shorter cap).
  char src[32];
  memset(src, 'a', 15);
  src[15] = '\xc3';  // 2-byte UTF-8 lead byte ('é')
  src[16] = '\xa9';
  src[17] = '\0';

  PresetStore s;
  preset_store_init(&s);
  preset_store_add(&s, KIND_COUNTDOWN, 60);
  TEST_ASSERT_TRUE(preset_store_set_name(&s, 0, src));
  char expected[16];
  memset(expected, 'a', 15);
  expected[15] = '\0';
  TEST_ASSERT_EQUAL_STRING(expected, s.presets[0].name);
}

void test_set_name_empty_clears_an_existing_name(void) {
  PresetStore s;
  preset_store_init(&s);
  preset_store_add(&s, KIND_COUNTDOWN, 60);
  preset_store_set_name(&s, 0, "Tea");
  TEST_ASSERT_TRUE(preset_store_set_name(&s, 0, ""));
  TEST_ASSERT_EQUAL_STRING("", s.presets[0].name);
}

void test_name_persists_across_edit(void) {
  // A name is display-only metadata, independent of {kind, duration_s} --
  // preset_store_edit (which only touches kind/duration) must not clear it,
  // matching Timer.name's own "persists across every mutation" contract.
  PresetStore s;
  preset_store_init(&s);
  preset_store_add(&s, KIND_COUNTDOWN, 60);
  preset_store_set_name(&s, 0, "Tea");
  TEST_ASSERT_TRUE(preset_store_edit(&s, 0, KIND_REPEAT, 90));
  TEST_ASSERT_EQUAL_STRING("Tea", s.presets[0].name);
}

void test_name_cleared_on_remove(void) {
  // The other half of the lifecycle rule: a name is cleared only when the
  // preset itself is removed -- proven by re-adding a NEW preset into the
  // same now-vacant slot and confirming it starts name-less
  // (preset_store_remove's own compaction fully zeroes the vacated slot).
  PresetStore s;
  preset_store_init(&s);
  preset_store_add(&s, KIND_COUNTDOWN, 60);
  preset_store_set_name(&s, 0, "Tea");
  preset_store_remove(&s, 0);
  preset_store_add(&s, KIND_COUNTDOWN, 60);
  TEST_ASSERT_EQUAL_STRING("", s.presets[0].name);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_init_is_empty);
  RUN_TEST(test_add_appends);
  RUN_TEST(test_add_refuses_past_max_presets);
  RUN_TEST(test_add_refuses_stopwatch_kind);
  RUN_TEST(test_add_refuses_zero_duration);
  RUN_TEST(test_add_refuses_negative_duration);
  RUN_TEST(test_add_refuses_duration_over_max);
  RUN_TEST(test_add_accepts_duration_at_max_boundary);
  RUN_TEST(test_edit_rejects_duration_over_max);
  RUN_TEST(test_add_accepts_repeat_kind);
  RUN_TEST(test_remove_compacts_array);
  RUN_TEST(test_remove_out_of_range_returns_false);
  RUN_TEST(test_remove_last_zeroes_the_vacated_slot);
  RUN_TEST(test_edit_updates_kind_and_duration);
  RUN_TEST(test_edit_out_of_range_returns_false);
  RUN_TEST(test_edit_rejects_invalid_kind_or_duration);
  RUN_TEST(test_get_returns_pointer_for_existing);
  RUN_TEST(test_get_returns_null_out_of_range);
  RUN_TEST(test_sequence_add_remove_get_integrity);
  RUN_TEST(test_new_preset_defaults_name_less);
  RUN_TEST(test_set_name_found_sets_the_name);
  RUN_TEST(test_set_name_out_of_range_returns_false);
  RUN_TEST(test_set_name_delegates_cap_truncation_to_timer_name_copy);
  RUN_TEST(test_set_name_utf8_safe_does_not_split_a_multibyte_char_at_the_cap);
  RUN_TEST(test_set_name_empty_clears_an_existing_name);
  RUN_TEST(test_name_persists_across_edit);
  RUN_TEST(test_name_cleared_on_remove);
  return UNITY_END();
}
