// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// Fixture-drift guard for src/c/gallery.c's 18 literal-NlParse voice scenes.
// gallery.c excludes lib/parse_nl.c from GALLERY_MODE builds (to save binary
// size) and instead hand-types each voice scene's body as a literal NlParse
// struct whose values are hand-verified to match the real parser's output
// for that scene's transcript, rather than calling parse_nl() live. Nothing
// at build/test time
// previously linked those two representations together -- a future grammar
// change (parser correctness is covered by parse_nl_test.c, which gets
// updated to match the new, correct behavior) could silently leave gallery.c
// rendering a STALE, now-incorrect confirm screen with no failure anywhere.
// This file hardcodes the SAME 18 {transcript, expected NlParse fields}
// pairs gallery.c's scene bodies carry and asserts them against the REAL
// parse_nl()/nl_clock_target_to_duration() -- when a grammar change alters
// output for any of these 18 transcripts, THIS test fails and points
// whoever's touching the parser at the matching gallery.c scene (cited by
// scene number/name below) before the fixture can go stale unnoticed.
//
// A new file (not a parse_nl_test.c addition) because gallery.c's fixtures
// are a distinct artifact from parse_nl_test.c's own corpus -- this file's
// job is specifically "does gallery.c's frozen copy still match reality,"
// not "is the parser correct" (parse_nl_test.c's job, unchanged) -- keeping
// them separate means a failure here always means "go update gallery.c,"
// never "go find which of two overlapping test files to fix." Deliberately
// duplicates parse_nl_test.c's tiny assert_parse/assert_clock_target shape
// rather than #including that file (which would also pull in every one of
// its own RUN_TEST-registered test_ functions/its own main()).
#include "parse_nl.h"
#include "unity.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

// Same fixed "now" gallery.c's own GALLERY_CLOCK_TARGET_FIXED_NOW_SOD uses
// for every clock-target scene (09:00:00 local) -- see gallery.c's own
// comment above scene #86 for why 09:00 was chosen (a spoken 12h "six"
// target resolves to its PM occurrence, demonstrating nearest-future AM/PM
// resolution).
#define FIXED_NOW_SOD (9 * 3600)

// Mirrors parse_nl_test.c's assert_parse (duration-shaped scenes: kind,
// duration_s, has_duration/ok, name).
static void assert_duration_fixture(const char *transcript, TimerKind exp_kind,
                                     int32_t exp_duration_s, bool exp_has_duration, bool exp_ok,
                                     const char *exp_name) {
  NlParse r;
  parse_nl(transcript, &r);

  char msg[96];
  snprintf(msg, sizeof msg, "transcript: \"%s\"", transcript);

  TEST_ASSERT_EQUAL_INT_MESSAGE(exp_ok, r.ok, msg);
  TEST_ASSERT_EQUAL_INT_MESSAGE(exp_kind, r.kind, msg);
  TEST_ASSERT_EQUAL_INT32_MESSAGE(exp_duration_s, r.duration_s, msg);
  TEST_ASSERT_EQUAL_INT_MESSAGE(exp_has_duration, r.has_duration, msg);
  TEST_ASSERT_EQUAL_STRING_MESSAGE(exp_name, r.name, msg);
  TEST_ASSERT_EQUAL_INT_MESSAGE(strlen(exp_name) > 0, r.has_name, msg);
}

// Mirrors parse_nl_test.c's assert_clock_target, plus resolves the target
// through nl_clock_target_to_duration() at FIXED_NOW_SOD (gallery.c's own
// scenes precompute this same resolution once, by hand, into their literal
// duration_s/clock_target_sod fields -- see gallery.c's comment above scene
// #86) and checks THAT output too, since it's the second half of what each
// clock-target scene's fixture hardcodes.
static void assert_clock_target_fixture(const char *transcript, int exp_hour, int exp_minute,
                                         bool exp_hour24_known, int32_t exp_duration_s,
                                         int32_t exp_clock_target_sod) {
  NlParse r;
  parse_nl(transcript, &r);

  char msg[96];
  snprintf(msg, sizeof msg, "transcript: \"%s\"", transcript);

  TEST_ASSERT_TRUE_MESSAGE(r.clock.has_clock_target, msg);
  TEST_ASSERT_EQUAL_INT_MESSAGE(exp_hour, r.clock.hour, msg);
  TEST_ASSERT_EQUAL_INT_MESSAGE(exp_minute, r.clock.minute, msg);
  TEST_ASSERT_EQUAL_INT_MESSAGE(exp_hour24_known, r.clock.hour24_known, msg);
  TEST_ASSERT_TRUE_MESSAGE(r.ok, msg);
  TEST_ASSERT_FALSE_MESSAGE(r.has_duration, msg);
  TEST_ASSERT_EQUAL_INT_MESSAGE(KIND_COUNTDOWN, r.kind, msg);

  int32_t target_sod = 0;
  int32_t duration_s = nl_clock_target_to_duration(&r.clock, FIXED_NOW_SOD, &target_sod);
  TEST_ASSERT_EQUAL_INT32_MESSAGE(exp_duration_s, duration_s, msg);
  TEST_ASSERT_EQUAL_INT32_MESSAGE(exp_clock_target_sod, target_sod, msg);
}

// -- Plain-duration scenes (12 of the 18) ------------------------------------

// Scene 52 (voice_confirm_countdown) + scene 65 (voice_add_name_result,
// same fixture, see its own comment) -- app/test/parse_nl_test.c:38.
void test_scene_52_65_voice_confirm_countdown(void) {
  assert_duration_fixture("five minutes", KIND_COUNTDOWN, 300, true, true, "");
}

// Scene 53 (voice_confirm_repeat_es) -- app/test/parse_nl_test.c:83.
void test_scene_53_voice_confirm_repeat_es(void) {
  assert_duration_fixture("cada 20 minutos", KIND_REPEAT, 1200, true, true, "");
}

// Scene 54 (voice_confirm_stopwatch) -- app/test/parse_nl_test.c:143.
void test_scene_54_voice_confirm_stopwatch(void) {
  assert_duration_fixture("cronómetro", KIND_STOPWATCH, 0, false, true, "");
}

// Scene 55 (voice_confirm_compound) -- app/test/parse_nl_test.c:150.
void test_scene_55_voice_confirm_compound(void) {
  assert_duration_fixture("one minute thirty seconds", KIND_COUNTDOWN, 90, true, true, "");
}

// Scene 56 (voice_confirm_named) -- app/test/parse_nl_test.c:161.
void test_scene_56_voice_confirm_named(void) {
  assert_duration_fixture("pasta for eight minutes", KIND_COUNTDOWN, 480, true, true, "pasta");
}

// Scene 57 (voice_parse_fail) -- gallery.c's own comment documents the
// real parse_nl() output for this transcript (no corpus line citation --
// this is a "everything falls into name residue" fallback case).
void test_scene_57_voice_parse_fail(void) {
  assert_duration_fixture("set a timer for when the pasta is done", KIND_COUNTDOWN, 0, false,
                           false, "when the pasta is done");
}

// Scene 70 (voice_confirm_pair_es) -- app/test/parse_nl_test.c:733.
void test_scene_70_voice_confirm_pair_es(void) {
  assert_duration_fixture("seis cuarenta y cinco", KIND_COUNTDOWN, 405, true, true, "");
}

// Scene 71 (voice_confirm_fraction_es) -- app/test/parse_nl_test.c:977.
void test_scene_71_voice_confirm_fraction_es(void) {
  assert_duration_fixture("hora y media", KIND_COUNTDOWN, 5400, true, true, "");
}

// Scene 78 (voice_preset_confirm) -- app/test/parse_nl_test.c:44.
void test_scene_78_voice_preset_confirm(void) {
  assert_duration_fixture("fifteen minutes", KIND_COUNTDOWN, 900, true, true, "");
}

// Scene 79 (voice_preset_stopwatch_unsupported) -- app/test/parse_nl_test.c:139.
void test_scene_79_voice_preset_stopwatch_unsupported(void) {
  assert_duration_fixture("stopwatch", KIND_STOPWATCH, 0, false, true, "");
}

// Scene 85 (voice_confirm_minutes_trailing_es) -- app/test/parse_nl_test.c:850.
void test_scene_85_voice_confirm_minutes_trailing_es(void) {
  assert_duration_fixture("6 minutos 45", KIND_COUNTDOWN, 405, true, true, "");
}

// Scene 91 (voice_confirm_namedrop) -- app/test/parse_nl_test.c:1832.
void test_scene_91_voice_confirm_namedrop(void) {
  assert_duration_fixture("give me 15 minutes", KIND_COUNTDOWN, 900, true, true, "");
}

// -- Clock-target scenes (6 of the 18) ---------------------------------------

// Scene 86 (voice_confirm_clock_target_en) + scene 88
// (voice_confirm_clock_target_12h, same underlying parse -- only the forced
// device style differs, not tested here) -- app/test/parse_nl_test.c:1379.
void test_scene_86_88_voice_confirm_clock_target_en(void) {
  assert_clock_target_fixture("at six forty five", 6, 45, false, 35100, 67500);
}

// Scene 87 (voice_confirm_clock_target_es) -- app/test/parse_nl_test.c:1386.
void test_scene_87_voice_confirm_clock_target_es(void) {
  assert_clock_target_fixture("a las seis y media", 6, 30, false, 34200, 66600);
}

// Scene 89 (voice_confirm_colon_clock_target) -- app/test/parse_nl_test.c:1495.
void test_scene_89_voice_confirm_colon_clock_target(void) {
  assert_clock_target_fixture("a las 18:30", 18, 30, true, 34200, 66600);
}

// Scene 90 (voice_confirm_subtractive_clock_target) -- gallery.c's own
// comment documents the real parse_nl() output for this transcript (the
// subtractive-clock-target family, structurally same as
// parse_nl_test.c:1682's "las seis menos cuarto" one hour down).
void test_scene_90_voice_confirm_subtractive_clock_target(void) {
  assert_clock_target_fixture("a las siete menos cuarto", 6, 45, false, 35100, 67500);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_scene_52_65_voice_confirm_countdown);
  RUN_TEST(test_scene_53_voice_confirm_repeat_es);
  RUN_TEST(test_scene_54_voice_confirm_stopwatch);
  RUN_TEST(test_scene_55_voice_confirm_compound);
  RUN_TEST(test_scene_56_voice_confirm_named);
  RUN_TEST(test_scene_57_voice_parse_fail);
  RUN_TEST(test_scene_70_voice_confirm_pair_es);
  RUN_TEST(test_scene_71_voice_confirm_fraction_es);
  RUN_TEST(test_scene_78_voice_preset_confirm);
  RUN_TEST(test_scene_79_voice_preset_stopwatch_unsupported);
  RUN_TEST(test_scene_85_voice_confirm_minutes_trailing_es);
  RUN_TEST(test_scene_91_voice_confirm_namedrop);
  RUN_TEST(test_scene_86_88_voice_confirm_clock_target_en);
  RUN_TEST(test_scene_87_voice_confirm_clock_target_es);
  RUN_TEST(test_scene_89_voice_confirm_colon_clock_target);
  RUN_TEST(test_scene_90_voice_confirm_subtractive_clock_target);
  return UNITY_END();
}
