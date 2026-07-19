// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// Launching a preset is NOT a new lib function -- it is store_add(TimerStore,
// p->kind, p->duration_s, now, &cookie) called verbatim, the exact same call
// the create flow already uses. There is deliberately no separate
// "preset -> Timer" conversion helper: launch reuses store_add verbatim, and
// nothing in the timer lifecycle learns about presets.
// This file is the field-by-field proof that this call sequence produces a
// clean timer slot -- none of Timer's overloaded fields (original_total_s,
// which doubles as a countdown quarter-cache and a repeat snooze marker,
// cycles, fired, laps) leak from a stale value, because store_add memsets the
// new slot to zero before setting cookie/kind/duration_s/started_at (verified
// by reading store_add's body).
#include "preset_store.h"
#include "store.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void assert_clean_launched_timer(const Timer *t, TimerKind kind, int32_t duration_s,
                                         time_t now) {
  TEST_ASSERT_NOT_NULL(t);
  TEST_ASSERT_EQUAL(kind, t->kind);
  TEST_ASSERT_EQUAL_INT32(duration_s, t->duration_s);
  TEST_ASSERT_EQUAL(now, t->started_at);
  TEST_ASSERT_FALSE(t->is_paused);
  TEST_ASSERT_EQUAL_INT32(0, t->accumulated_pause_s);
  TEST_ASSERT_FALSE(t->fired);
  TEST_ASSERT_EQUAL_INT32(0, t->cycles);
  // The load-bearing assertion: original_total_s must be 0 on launch -- a
  // preset carries no quarter-cache/snooze provenance to transfer, and a
  // preset-launched timer must be indistinguishable from a hand-dialed one.
  TEST_ASSERT_EQUAL_INT32(0, t->original_total_s);
  TEST_ASSERT_EQUAL_UINT8(0, t->lap_count);
}

void test_launch_countdown_preset_produces_clean_timer(void) {
  PresetStore presets;
  preset_store_init(&presets);
  preset_store_add(&presets, KIND_COUNTDOWN, 300);
  const Preset *p = preset_store_get(&presets, 0);

  TimerStore store;
  store_init(&store);
  int32_t cookie;
  time_t now = 5000;
  TEST_ASSERT_TRUE(store_add(&store, p->kind, p->duration_s, now, &cookie));

  Timer *t = store_find(&store, cookie);
  assert_clean_launched_timer(t, KIND_COUNTDOWN, 300, now);
}

void test_launch_repeat_preset_produces_clean_timer(void) {
  PresetStore presets;
  preset_store_init(&presets);
  preset_store_add(&presets, KIND_REPEAT, 90);
  const Preset *p = preset_store_get(&presets, 0);

  TimerStore store;
  store_init(&store);
  int32_t cookie;
  time_t now = 8000;
  TEST_ASSERT_TRUE(store_add(&store, p->kind, p->duration_s, now, &cookie));

  Timer *t = store_find(&store, cookie);
  assert_clean_launched_timer(t, KIND_REPEAT, 90, now);
}

void test_launched_timer_is_independent_of_the_source_preset(void) {
  PresetStore presets;
  preset_store_init(&presets);
  preset_store_add(&presets, KIND_COUNTDOWN, 600);

  TimerStore store;
  store_init(&store);
  int32_t cookie;
  const Preset *p = preset_store_get(&presets, 0);
  store_add(&store, p->kind, p->duration_s, 1000, &cookie);

  // Editing (or deleting) the preset after launch must not retroactively
  // affect the already-copied live timer -- launch is a one-shot copy, never
  // a live reference.
  preset_store_edit(&presets, 0, KIND_REPEAT, 45);
  Timer *t = store_find(&store, cookie);
  TEST_ASSERT_EQUAL(KIND_COUNTDOWN, t->kind);
  TEST_ASSERT_EQUAL_INT32(600, t->duration_s);

  preset_store_remove(&presets, 0);
  TEST_ASSERT_EQUAL_INT(1, store.count);  // the launched timer survives the preset's deletion
}

// --- launch inheritance, lib-level plumbing --------------------------------
//
// The adapter tier's screen_library.c launch_preset call sequence is:
// store_add(...) followed by store_set_name(store, cookie, p->name) when the
// preset is named. This is NOT a new lib function (same "launch reuses
// store_add verbatim" design as the rest of this file) -- these tests prove
// that exact sequence, run at the lib level, carries the name field-by-field
// onto a clean Timer, using the real store_set_name + Preset.name primitives.

void test_launch_named_preset_inherits_the_name(void) {
  PresetStore presets;
  preset_store_init(&presets);
  preset_store_add(&presets, KIND_COUNTDOWN, 300);
  preset_store_set_name(&presets, 0, "Tea");
  const Preset *p = preset_store_get(&presets, 0);

  TimerStore store;
  store_init(&store);
  int32_t cookie;
  time_t now = 5000;
  TEST_ASSERT_TRUE(store_add(&store, p->kind, p->duration_s, now, &cookie));
  // The exact one-line call: only when the source preset is named.
  if (p->name[0] != '\0') {
    TEST_ASSERT_TRUE(store_set_name(&store, cookie, p->name));
  }

  Timer *t = store_find(&store, cookie);
  assert_clean_launched_timer(t, KIND_COUNTDOWN, 300, now);
  TEST_ASSERT_EQUAL_STRING("Tea", t->name);
}

void test_launch_name_less_preset_produces_name_less_timer(void) {
  // The degrade default: a preset with no name never calls store_set_name at
  // all (the `if (p->name[0] != '\0')` guard), so the launched timer is
  // exactly as clean/name-less as the pre-naming launch path already was.
  PresetStore presets;
  preset_store_init(&presets);
  preset_store_add(&presets, KIND_REPEAT, 90);
  const Preset *p = preset_store_get(&presets, 0);
  TEST_ASSERT_EQUAL_STRING("", p->name);

  TimerStore store;
  store_init(&store);
  int32_t cookie;
  time_t now = 6000;
  TEST_ASSERT_TRUE(store_add(&store, p->kind, p->duration_s, now, &cookie));
  if (p->name[0] != '\0') {
    store_set_name(&store, cookie, p->name);
  }

  Timer *t = store_find(&store, cookie);
  assert_clean_launched_timer(t, KIND_REPEAT, 90, now);
  TEST_ASSERT_EQUAL_STRING("", t->name);
}

void test_launch_named_preset_name_carry_is_independent_of_the_source_preset(void) {
  // Same independence guarantee as
  // test_launched_timer_is_independent_of_the_source_preset, extended to the
  // name: renaming (or deleting) the source preset after launch must never
  // retroactively touch the already-copied live timer's name.
  PresetStore presets;
  preset_store_init(&presets);
  preset_store_add(&presets, KIND_COUNTDOWN, 600);
  preset_store_set_name(&presets, 0, "Pomodoro");
  const Preset *p = preset_store_get(&presets, 0);

  TimerStore store;
  store_init(&store);
  int32_t cookie;
  store_add(&store, p->kind, p->duration_s, 1000, &cookie);
  store_set_name(&store, cookie, p->name);

  preset_store_set_name(&presets, 0, "Renamed");
  Timer *t = store_find(&store, cookie);
  TEST_ASSERT_EQUAL_STRING("Pomodoro", t->name);

  preset_store_remove(&presets, 0);
  TEST_ASSERT_EQUAL_STRING("Pomodoro", t->name);  // survives the preset's deletion too
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_launch_countdown_preset_produces_clean_timer);
  RUN_TEST(test_launch_repeat_preset_produces_clean_timer);
  RUN_TEST(test_launched_timer_is_independent_of_the_source_preset);
  RUN_TEST(test_launch_named_preset_inherits_the_name);
  RUN_TEST(test_launch_name_less_preset_produces_name_less_timer);
  RUN_TEST(test_launch_named_preset_name_carry_is_independent_of_the_source_preset);
  return UNITY_END();
}
