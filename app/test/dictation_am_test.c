// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// Exercises dictation_am.c's session
// lifecycle (create/start/callback/destroy, the re-entrancy guard, the
// lazy-retry-on-create-failure path, and dictation_am_cancel()'s abort
// semantics) against FFF fakes of the Dictation surface it calls
// (test/fakes/pebble.h), compiled via test/Makefile's dedicated
// dictation_am_test rule (mirroring testlog_am_test.c's/hold_test.c's own
// explicit-rule pattern -- the fourth adapter file let into the host build).
//
// The REAL lib/parse_nl.c + lib/voice_outcome.c are linked in (not faked) --
// same convention as hold_test.c linking the real lib/set_accel.c -- so the
// FULL-scope success tests below double as a (small) integration check that
// dictation_am.c wires the parser correctly, using transcripts already
// exercised in test/parse_nl_test.c, not invented here.
//
// Not covered here (device-only):
// the real system dictation UI's pixels and real (successful) transcripts
// from an actual recognizer. "Alarm pre-empts mid-dictation" is also proven
// live, end to end, against the real emulator: dictation_session_create()
// genuinely succeeds there (see dictation_am.c's own comment), so
// s_in_progress really does become true on the emulator, and a real timer
// firing while it's true really does exercise dictation_am_cancel() through
// nav.c's structural funnel. This host suite additionally covers every
// individual branch (every status class, the retry-on-failure path, the
// re-entrancy guard, truncation) more exhaustively/deterministically than
// racing a live firmware retry loop in an emulator script ever could.
#include "dictation_am.h"

#include <stdio.h>
#include <string.h>

#include "fff.h"
#include "pebble.h"  // test/fakes/pebble.h -- see test/Makefile's -I ordering
#include "nav.h"
#include "screen_actions.h"
#include "screen_preset_actions.h"
#include "screen_voice.h"
#include "testlog_am.h"
#include "unity.h"

// Only for NL_TABLES_TOTAL, to size/reload the real vocabulary blob this
// file's linked-in (not faked) lib/parse_nl.c needs -- see
// test_callback_success_full_scope_when_vocabulary_unavailable_shows_data_unavailable's
// own comment below.
#include "../resources/generated/nl_tables.h"

DEFINE_FFF_GLOBALS;

FAKE_VALUE_FUNC(DictationSession *, dictation_session_create, uint32_t,
                DictationSessionStatusCallback, void *);
FAKE_VOID_FUNC(dictation_session_destroy, DictationSession *);
FAKE_VALUE_FUNC(DictationSessionStatus, dictation_session_start, DictationSession *);
FAKE_VALUE_FUNC(DictationSessionStatus, dictation_session_stop, DictationSession *);
FAKE_VOID_FUNC(dictation_session_enable_confirmation, DictationSession *, bool);
FAKE_VOID_FUNC(dictation_session_enable_error_dialogs, DictationSession *, bool);
FAKE_VOID_FUNC(testlog, const char *, const char *);
FAKE_VOID_FUNC(screen_voice_show_parse, const char *, const NlParse *, VoiceOutcome);
FAKE_VOID_FUNC(screen_voice_set_name, const char *);
// The PRESET_NAME-scope sink, mirroring
// screen_voice_set_name's fake above one-for-one.
FAKE_VOID_FUNC(screen_preset_actions_apply_name, const char *);
// The ACTIONS_NAME-scope sink, mirroring the
// PRESET_NAME fake above one-for-one (a live Timer instead of a Preset).
FAKE_VOID_FUNC(screen_actions_apply_name, const char *);
// dictation_am.c's VOICE_CANCELLED routing calls
// screen_voice_cancel_to_launcher() (screen_voice.c's own shared
// "return to launcher" seam, target-branching internally) -- see
// screen_voice.h's own doc comment. nav_go_home_focus_voice_row() is not
// referenced by this file, so no fake is declared for it.
FAKE_VOID_FUNC(screen_voice_cancel_to_launcher);

// A dummy, never-dereferenced session handle (dictation_am.c only ever
// compares it against NULL / passes it through) -- same convention as
// hold_test.c's/input_guard_test.c's dummy AppTimer* handles.
static DictationSession *const kSession = (DictationSession *)1;

// Grabs the callback dictation_session_create() was actually invoked with --
// always the same static function pointer (dictation_am.c's status_
// callback), captured fresh from whichever create() call most recently
// succeeded.
static DictationSessionStatusCallback registered_callback(void) {
  TEST_ASSERT_TRUE(dictation_session_create_fake.call_count > 0);
  unsigned idx = dictation_session_create_fake.call_count - 1;
  DictationSessionStatusCallback cb = dictation_session_create_fake.arg1_history[idx];
  TEST_ASSERT_NOT_NULL(cb);
  return cb;
}

// Full reset: tears down whatever a PRIOR test left (dictation_am.c's own
// statics persist across tests in this one process/binary), resets every
// fake to a clean call-count-0 state, configures dictation_session_create()'s
// return value, then calls dictation_am_init() fresh -- so every test starts
// from a known, fully-reset module state regardless of test order.
static void reset_module(bool create_succeeds) {
  dictation_am_deinit();

  RESET_FAKE(dictation_session_create);
  RESET_FAKE(dictation_session_destroy);
  RESET_FAKE(dictation_session_start);
  RESET_FAKE(dictation_session_stop);
  RESET_FAKE(dictation_session_enable_confirmation);
  RESET_FAKE(dictation_session_enable_error_dialogs);
  RESET_FAKE(testlog);
  RESET_FAKE(screen_voice_show_parse);
  RESET_FAKE(screen_voice_set_name);
  RESET_FAKE(screen_preset_actions_apply_name);
  RESET_FAKE(screen_actions_apply_name);
  RESET_FAKE(screen_voice_cancel_to_launcher);
  FFF_RESET_HISTORY();

  dictation_session_create_fake.return_val = create_succeeds ? kSession : NULL;
  dictation_am_init();
}

void setUp(void) {
  reset_module(/*create_succeeds=*/true);
}

void tearDown(void) {}

// --- dictation_am_init() ----------------------------------------------------

void test_init_creates_session_and_configures_confirmation_off_error_dialogs_on(void) {
  // setUp's own reset_module(true) call already exercised this -- assert on
  // exactly what it did. Confirmation OFF, error dialogs ON -- see
  // dictation_am.c's own try_create_session() comment for why error dialogs
  // stay ON.
  TEST_ASSERT_EQUAL_INT(1, dictation_session_create_fake.call_count);
  TEST_ASSERT_EQUAL_INT(1, dictation_session_enable_confirmation_fake.call_count);
  TEST_ASSERT_EQUAL_PTR(kSession, dictation_session_enable_confirmation_fake.arg0_val);
  TEST_ASSERT_FALSE(dictation_session_enable_confirmation_fake.arg1_val);
  TEST_ASSERT_EQUAL_INT(1, dictation_session_enable_error_dialogs_fake.call_count);
  TEST_ASSERT_EQUAL_PTR(kSession, dictation_session_enable_error_dialogs_fake.arg0_val);
  TEST_ASSERT_TRUE(dictation_session_enable_error_dialogs_fake.arg1_val);
}

void test_init_when_create_fails_does_not_configure_a_null_session(void) {
  reset_module(/*create_succeeds=*/false);

  TEST_ASSERT_EQUAL_INT(1, dictation_session_create_fake.call_count);
  TEST_ASSERT_EQUAL_INT(0, dictation_session_enable_confirmation_fake.call_count);
  TEST_ASSERT_EQUAL_INT(0, dictation_session_enable_error_dialogs_fake.call_count);
}

// --- dictation_am_start() -- the happy path ---------------------------------

void test_start_full_calls_dictation_session_start_and_logs_start_full(void) {
  dictation_am_start(DICT_SCOPE_FULL);

  TEST_ASSERT_EQUAL_INT(1, dictation_session_start_fake.call_count);
  TEST_ASSERT_EQUAL_PTR(kSession, dictation_session_start_fake.arg0_val);
  TEST_ASSERT_EQUAL_INT(1, testlog_fake.call_count);
  TEST_ASSERT_EQUAL_STRING("dictation", testlog_fake.arg0_val);
  TEST_ASSERT_EQUAL_STRING("start_full", testlog_fake.arg1_val);
  // No outcome delivered yet -- a real attempt is asynchronous (the system
  // dictation UI callback hasn't fired).
  TEST_ASSERT_EQUAL_INT(0, screen_voice_show_parse_fake.call_count);
}

// pebble.h documents dictation_session_start() can fail
// SYNCHRONOUSLY ("already in progress or is invalid") -- a case no status
// callback will ever resolve, so s_in_progress must be cleared here and now
// (via the same synchronous deliver() path try_create_session() failure
// already uses), not left stuck until an unrelated nav transition happens
// to clear it via render_stack()'s cancel funnel.
void test_start_when_session_start_fails_synchronously_clears_guard_and_delivers_no_phone(void) {
  dictation_session_start_fake.return_val = DictationSessionStatusFailureInternalError;

  dictation_am_start(DICT_SCOPE_FULL);

  TEST_ASSERT_EQUAL_INT(1, dictation_session_start_fake.call_count);
  TEST_ASSERT_EQUAL_INT(1, screen_voice_show_parse_fake.call_count);
  TEST_ASSERT_NULL(screen_voice_show_parse_fake.arg0_val);
  TEST_ASSERT_EQUAL_INT(VOICE_NO_PHONE, screen_voice_show_parse_fake.arg2_val);
  TEST_ASSERT_EQUAL_STRING("start_failed", testlog_fake.arg1_history[testlog_fake.call_count - 2]);
  TEST_ASSERT_EQUAL_STRING("dictation_outcome", testlog_fake.arg0_history[testlog_fake.call_count - 1]);
  TEST_ASSERT_EQUAL_STRING("no_phone", testlog_fake.arg1_history[testlog_fake.call_count - 1]);

  // The guard is clear again -- a fresh attempt works normally, not
  // permanently wedged by the failed one. RESET_FAKE also resets
  // return_val back to 0 (DictationSessionStatusSuccess), so this next
  // attempt succeeds.
  RESET_FAKE(dictation_session_start);
  FFF_RESET_HISTORY();
  dictation_am_start(DICT_SCOPE_FULL);
  TEST_ASSERT_EQUAL_INT(1, dictation_session_start_fake.call_count);
}

void test_start_name_calls_dictation_session_start_and_logs_start_name(void) {
  dictation_am_start(DICT_SCOPE_NAME);

  TEST_ASSERT_EQUAL_INT(1, dictation_session_start_fake.call_count);
  TEST_ASSERT_EQUAL_STRING("start_name", testlog_fake.arg1_val);
}

// PRESET_NAME shares the "start_name" tag with
// NAME (dictation_am.c's own comment on why -- one testlog tag per verbatim-
// name re-dictation, the eventual routing distinguishes the two).
void test_start_preset_name_calls_dictation_session_start_and_logs_start_name(void) {
  dictation_am_start(DICT_SCOPE_PRESET_NAME);

  TEST_ASSERT_EQUAL_INT(1, dictation_session_start_fake.call_count);
  TEST_ASSERT_EQUAL_STRING("start_name", testlog_fake.arg1_val);
}

// ACTIONS_NAME shares the "start_name" tag too.
void test_start_actions_name_calls_dictation_session_start_and_logs_start_name(void) {
  dictation_am_start(DICT_SCOPE_ACTIONS_NAME);

  TEST_ASSERT_EQUAL_INT(1, dictation_session_start_fake.call_count);
  TEST_ASSERT_EQUAL_STRING("start_name", testlog_fake.arg1_val);
}

// --- re-entrancy guard -------------------------------------------------------

void test_start_is_a_reentrancy_guard_second_call_before_callback_is_a_noop(void) {
  dictation_am_start(DICT_SCOPE_FULL);
  TEST_ASSERT_EQUAL_INT(1, dictation_session_start_fake.call_count);

  // A second attempt while the first is still in flight -- not reachable via
  // a real button press (the system dictation UI owns input), but a
  // defensive guard, not an assumption.
  dictation_am_start(DICT_SCOPE_FULL);
  dictation_am_start(DICT_SCOPE_NAME);

  TEST_ASSERT_EQUAL_INT(1, dictation_session_start_fake.call_count);  // still just the one
  TEST_ASSERT_EQUAL_INT(1, testlog_fake.call_count);                  // no extra "start_*" logged
}

void test_callback_clears_the_reentrancy_guard_allowing_a_new_start(void) {
  dictation_am_start(DICT_SCOPE_FULL);
  DictationSessionStatusCallback cb = registered_callback();
  cb(kSession, DictationSessionStatusSuccess, (char *)"five minutes", NULL);

  RESET_FAKE(dictation_session_start);
  FFF_RESET_HISTORY();

  dictation_am_start(DICT_SCOPE_FULL);
  TEST_ASSERT_EQUAL_INT(1, dictation_session_start_fake.call_count);
}

// --- create-failure / lazy retry ---------------------------------------------

void test_start_delivers_no_phone_synchronously_when_no_session_exists(void) {
  reset_module(/*create_succeeds=*/false);

  dictation_am_start(DICT_SCOPE_FULL);

  // reset_module()'s own dictation_am_init() call already burned one failed
  // create() attempt; dictation_am_start() retries once more (per its own
  // contract), still fails -- the emulator's own permanent case.
  TEST_ASSERT_EQUAL_INT(2, dictation_session_create_fake.call_count);
  TEST_ASSERT_EQUAL_INT(0, dictation_session_start_fake.call_count);
  TEST_ASSERT_EQUAL_INT(1, screen_voice_show_parse_fake.call_count);
  TEST_ASSERT_NULL(screen_voice_show_parse_fake.arg0_val);   // no transcript
  TEST_ASSERT_NULL(screen_voice_show_parse_fake.arg1_val);   // no parse
  TEST_ASSERT_EQUAL_INT(VOICE_NO_PHONE, screen_voice_show_parse_fake.arg2_val);
  // Both markers present: the create-failure itself, and the routed outcome.
  TEST_ASSERT_EQUAL_INT(2, testlog_fake.call_count);
  TEST_ASSERT_EQUAL_STRING("dictation", testlog_fake.arg0_history[0]);
  TEST_ASSERT_EQUAL_STRING("create_failed", testlog_fake.arg1_history[0]);
  TEST_ASSERT_EQUAL_STRING("dictation_outcome", testlog_fake.arg0_history[1]);
  TEST_ASSERT_EQUAL_STRING("no_phone", testlog_fake.arg1_history[1]);
}

void test_start_after_a_failed_delivery_is_not_stuck_by_the_reentrancy_guard(void) {
  // A synchronous create-failure must clear s_in_progress just like a real
  // callback does -- otherwise the FIRST ever failed attempt would
  // permanently wedge every future "Say a timer"/"Say again" press.
  reset_module(/*create_succeeds=*/false);
  dictation_am_start(DICT_SCOPE_FULL);
  TEST_ASSERT_EQUAL_INT(1, screen_voice_show_parse_fake.call_count);

  dictation_session_create_fake.return_val = NULL;
  dictation_am_start(DICT_SCOPE_FULL);

  // reset_module() (1) + this test's two dictation_am_start() retries (2)
  // = 3, each one retried again, never skipped once it started failing.
  TEST_ASSERT_EQUAL_INT(3, dictation_session_create_fake.call_count);
  TEST_ASSERT_EQUAL_INT(2, screen_voice_show_parse_fake.call_count);
}

void test_start_retries_creation_and_succeeds_once_a_phone_connects(void) {
  reset_module(/*create_succeeds=*/false);
  dictation_am_start(DICT_SCOPE_FULL);
  TEST_ASSERT_EQUAL_INT(0, dictation_session_start_fake.call_count);

  // Simulate the phone connecting after the first failed attempt.
  dictation_session_create_fake.return_val = kSession;
  dictation_am_start(DICT_SCOPE_FULL);

  // reset_module() (1) + this test's own two retries (2) = 3.
  TEST_ASSERT_EQUAL_INT(3, dictation_session_create_fake.call_count);
  TEST_ASSERT_EQUAL_INT(1, dictation_session_start_fake.call_count);
  TEST_ASSERT_EQUAL_INT(1, dictation_session_enable_confirmation_fake.call_count);
}

// --- the status callback: success ---------------------------------------------

void test_callback_success_full_scope_parses_and_shows_confirm_preview(void) {
  // Case already exercised in test/parse_nl_test.c: "five minutes" -> ok=true,
  // KIND_COUNTDOWN, 300s, no name.
  dictation_am_start(DICT_SCOPE_FULL);
  DictationSessionStatusCallback cb = registered_callback();

  cb(kSession, DictationSessionStatusSuccess, (char *)"five minutes", NULL);

  TEST_ASSERT_EQUAL_INT(1, screen_voice_show_parse_fake.call_count);
  TEST_ASSERT_EQUAL_STRING("five minutes", screen_voice_show_parse_fake.arg0_val);
  const NlParse *parse = screen_voice_show_parse_fake.arg1_val;
  TEST_ASSERT_NOT_NULL(parse);
  TEST_ASSERT_TRUE(parse->ok);
  TEST_ASSERT_EQUAL_INT(KIND_COUNTDOWN, parse->kind);
  TEST_ASSERT_EQUAL_INT(300, parse->duration_s);
  TEST_ASSERT_EQUAL_INT(VOICE_OK, screen_voice_show_parse_fake.arg2_val);
  TEST_ASSERT_EQUAL_STRING("success", testlog_fake.arg1_history[testlog_fake.call_count - 1]);
}

// A recognized clock target must be
// resolved through the REAL clock (time()/localtime()) before reaching
// screen_voice -- has_duration flips from lib/parse_nl's own false to true,
// duration_s/clock_target_sod are filled in, and `ok`/`kind` are unaffected
// (both were already true/COUNTDOWN straight out of parse_nl() for a
// recognized clock target). This test deliberately does NOT fake time() --
// the real host clock runs, so it asserts the CONTRACT (duration_s in
// 1..86400, clock_target_sod in 0..86399, has_duration true) rather than an
// exact value, which would otherwise make this test flaky depending on
// wall-clock time when it happens to run.
void test_callback_success_full_scope_clock_target_resolves_via_real_clock(void) {
  dictation_am_start(DICT_SCOPE_FULL);
  DictationSessionStatusCallback cb = registered_callback();

  cb(kSession, DictationSessionStatusSuccess, (char *)"at six thirty", NULL);

  TEST_ASSERT_EQUAL_INT(1, screen_voice_show_parse_fake.call_count);
  const NlParse *parse = screen_voice_show_parse_fake.arg1_val;
  TEST_ASSERT_NOT_NULL(parse);
  TEST_ASSERT_TRUE(parse->ok);
  TEST_ASSERT_EQUAL_INT(KIND_COUNTDOWN, parse->kind);
  TEST_ASSERT_TRUE(parse->clock.has_clock_target);
  TEST_ASSERT_TRUE(parse->has_duration);
  TEST_ASSERT_TRUE(parse->duration_s >= 1 && parse->duration_s <= 86400);
  TEST_ASSERT_TRUE(parse->clock_target_sod >= 0 && parse->clock_target_sod <= 86399);
  TEST_ASSERT_EQUAL_INT(VOICE_OK, screen_voice_show_parse_fake.arg2_val);
}

void test_callback_success_full_scope_unparseable_shows_parse_fail(void) {
  // Fallback case already exercised in test/parse_nl_test.c: no N-U pair, no kind keyword -> ok=false.
  const char *transcript = "set a timer for when the pasta is done";
  dictation_am_start(DICT_SCOPE_FULL);
  DictationSessionStatusCallback cb = registered_callback();

  cb(kSession, DictationSessionStatusSuccess, (char *)transcript, NULL);

  TEST_ASSERT_EQUAL_INT(1, screen_voice_show_parse_fake.call_count);
  const NlParse *parse = screen_voice_show_parse_fake.arg1_val;
  TEST_ASSERT_FALSE(parse->ok);
  TEST_ASSERT_EQUAL_INT(VOICE_PARSE_FAIL, screen_voice_show_parse_fake.arg2_val);
  TEST_ASSERT_EQUAL_STRING("parse_fail", testlog_fake.arg1_history[testlog_fake.call_count - 1]);
}

// dictation_am.c:117-124's VOICE_DATA_UNAVAILABLE branch needs direct
// automated coverage. This imitates
// test/parse_nl_test.c:76-96's corrupt->assert->restore pattern: this file
// links the REAL lib/parse_nl.c (not faked -- see this file's header
// comment), and a host build starts with its vocabulary already installed
// (parse_nl.h's own doc comment on parse_nl_vocabulary_available()), so the
// vocabulary is deliberately cleared with parse_nl_vocabulary_set(NULL, 0)
// -- the exact condition dictation_am.c's own guard checks -- then restored
// from the same generated nl_tables.bin every other test in this process
// still needs available (the FULL-scope success tests above, and any test
// that runs after this one, exercise the real parser).
void test_callback_success_full_scope_when_vocabulary_unavailable_shows_data_unavailable(void) {
  static uint8_t vocab_backup[NL_TABLES_TOTAL];
  FILE *fp = fopen("../resources/generated/nl_tables.bin", "rb");
  if (fp == NULL) fp = fopen("resources/generated/nl_tables.bin", "rb");
  TEST_ASSERT_NOT_NULL_MESSAGE(fp, "could not open generated nl_tables.bin");
  size_t read_size = fread(vocab_backup, 1, sizeof vocab_backup, fp);
  TEST_ASSERT_EQUAL_UINT32(NL_TABLES_TOTAL, read_size);
  fclose(fp);

  TEST_ASSERT_FALSE(parse_nl_vocabulary_set(NULL, 0));
  TEST_ASSERT_FALSE(parse_nl_vocabulary_available());

  dictation_am_start(DICT_SCOPE_FULL);
  DictationSessionStatusCallback cb = registered_callback();
  cb(kSession, DictationSessionStatusSuccess, (char *)"five minutes", NULL);

  TEST_ASSERT_EQUAL_INT(1, screen_voice_show_parse_fake.call_count);
  TEST_ASSERT_EQUAL_STRING("five minutes", screen_voice_show_parse_fake.arg0_val);
  TEST_ASSERT_NULL(screen_voice_show_parse_fake.arg1_val);
  TEST_ASSERT_EQUAL_INT(VOICE_DATA_UNAVAILABLE, screen_voice_show_parse_fake.arg2_val);
  TEST_ASSERT_EQUAL_STRING("dictation_outcome", testlog_fake.arg0_history[testlog_fake.call_count - 1]);
  TEST_ASSERT_EQUAL_STRING("data_unavailable", testlog_fake.arg1_history[testlog_fake.call_count - 1]);

  // Restore -- see this test's own header comment.
  TEST_ASSERT_TRUE(parse_nl_vocabulary_set(vocab_backup, sizeof vocab_backup));
  TEST_ASSERT_TRUE(parse_nl_vocabulary_available());
}

// A successful NAME-scope
// re-dictation is the Add-name/Rename arc -- the transcript is taken
// VERBATIM as the candidate name (no parse_nl pass) and handed to
// screen_voice_set_name(), never screen_voice_show_parse() (which would
// discard the FULL-scope preview being named).
void test_callback_success_name_scope_sets_the_name_via_screen_voice(void) {
  dictation_am_start(DICT_SCOPE_NAME);
  DictationSessionStatusCallback cb = registered_callback();

  cb(kSession, DictationSessionStatusSuccess, (char *)"Tea", NULL);

  TEST_ASSERT_EQUAL_INT(1, screen_voice_set_name_fake.call_count);
  TEST_ASSERT_EQUAL_STRING("Tea", screen_voice_set_name_fake.arg0_val);
  TEST_ASSERT_EQUAL_INT(0, screen_voice_show_parse_fake.call_count);
  TEST_ASSERT_EQUAL_INT(0, screen_voice_cancel_to_launcher_fake.call_count);
  TEST_ASSERT_EQUAL_STRING("dictation_outcome", testlog_fake.arg0_history[testlog_fake.call_count - 1]);
  TEST_ASSERT_EQUAL_STRING("name_set", testlog_fake.arg1_history[testlog_fake.call_count - 1]);
}

// A FAILED/cancelled NAME-scope attempt must never disturb the FULL-scope
// preview it was launched from -- no screen_voice_show_parse(), no
// nav_go_home_focus_voice_row(), just a testlog marker recording the
// outcome; this is what distinguishes it from the identical status on a
// FULL-scope attempt, which DOES route to one or the other -- see the
// rejected/cancelled and every-failure-status tests below.
void test_callback_failed_name_scope_leaves_the_preview_untouched(void) {
  dictation_am_start(DICT_SCOPE_NAME);
  DictationSessionStatusCallback cb = registered_callback();

  cb(kSession, DictationSessionStatusFailureNoSpeechDetected, NULL, NULL);

  TEST_ASSERT_EQUAL_INT(0, screen_voice_set_name_fake.call_count);
  TEST_ASSERT_EQUAL_INT(0, screen_voice_show_parse_fake.call_count);
  TEST_ASSERT_EQUAL_INT(0, screen_voice_cancel_to_launcher_fake.call_count);
  TEST_ASSERT_EQUAL_STRING("dictation_outcome", testlog_fake.arg0_history[testlog_fake.call_count - 1]);
  TEST_ASSERT_EQUAL_STRING("no_speech", testlog_fake.arg1_history[testlog_fake.call_count - 1]);
}

// Same rule for a Back-during-NAME-dictation (Rejected) -- FULL-scope routes
// this to nav_go_home_focus_voice_row() (see
// test_callback_rejected_status_cancels_to_launcher_not_screen_voice
// below); NAME-scope must NOT, since the user only backed out of the naming
// sub-step, not the whole confirm flow.
void test_callback_rejected_name_scope_does_not_navigate_home(void) {
  dictation_am_start(DICT_SCOPE_NAME);
  DictationSessionStatusCallback cb = registered_callback();

  cb(kSession, DictationSessionStatusFailureTranscriptionRejected, NULL, NULL);

  TEST_ASSERT_EQUAL_INT(0, screen_voice_cancel_to_launcher_fake.call_count);
  TEST_ASSERT_EQUAL_INT(0, screen_voice_set_name_fake.call_count);
  TEST_ASSERT_EQUAL_STRING("cancelled", testlog_fake.arg1_history[testlog_fake.call_count - 1]);
}

// A successful PRESET_NAME-scope re-dictation is
// the Library Rename arc -- same verbatim-transcript contract as NAME, but
// routed to screen_preset_actions_apply_name() (a different sink, a Preset
// by index) instead of screen_voice_set_name().
void test_callback_success_preset_name_scope_calls_apply_name(void) {
  dictation_am_start(DICT_SCOPE_PRESET_NAME);
  DictationSessionStatusCallback cb = registered_callback();

  cb(kSession, DictationSessionStatusSuccess, (char *)"Tea", NULL);

  TEST_ASSERT_EQUAL_INT(1, screen_preset_actions_apply_name_fake.call_count);
  TEST_ASSERT_EQUAL_STRING("Tea", screen_preset_actions_apply_name_fake.arg0_val);
  TEST_ASSERT_EQUAL_INT(0, screen_voice_set_name_fake.call_count);
  TEST_ASSERT_EQUAL_INT(0, screen_voice_show_parse_fake.call_count);
  TEST_ASSERT_EQUAL_INT(0, screen_voice_cancel_to_launcher_fake.call_count);
  TEST_ASSERT_EQUAL_STRING("dictation_outcome", testlog_fake.arg0_history[testlog_fake.call_count - 1]);
  TEST_ASSERT_EQUAL_STRING("preset_name_set", testlog_fake.arg1_history[testlog_fake.call_count - 1]);
}

// A FAILED/cancelled PRESET_NAME attempt must never disturb the {Edit,
// Rename, Delete} sheet it was launched from -- the same silent no-op,
// testlog-only handling NAME-scope uses.
void test_callback_failed_preset_name_scope_leaves_the_sheet_untouched(void) {
  dictation_am_start(DICT_SCOPE_PRESET_NAME);
  DictationSessionStatusCallback cb = registered_callback();

  cb(kSession, DictationSessionStatusFailureNoSpeechDetected, NULL, NULL);

  TEST_ASSERT_EQUAL_INT(0, screen_preset_actions_apply_name_fake.call_count);
  TEST_ASSERT_EQUAL_INT(0, screen_voice_show_parse_fake.call_count);
  TEST_ASSERT_EQUAL_INT(0, screen_voice_cancel_to_launcher_fake.call_count);
  TEST_ASSERT_EQUAL_STRING("dictation_outcome", testlog_fake.arg0_history[testlog_fake.call_count - 1]);
  TEST_ASSERT_EQUAL_STRING("no_speech", testlog_fake.arg1_history[testlog_fake.call_count - 1]);
}

// Same rule for a Back-during-PRESET_NAME-dictation (Rejected) -- FULL-scope
// routes this to nav_go_home_focus_voice_row(); PRESET_NAME must NOT, since
// the user only backed out of the Rename sub-step, not the whole sheet.
void test_callback_rejected_preset_name_scope_does_not_navigate_home(void) {
  dictation_am_start(DICT_SCOPE_PRESET_NAME);
  DictationSessionStatusCallback cb = registered_callback();

  cb(kSession, DictationSessionStatusFailureTranscriptionRejected, NULL, NULL);

  TEST_ASSERT_EQUAL_INT(0, screen_voice_cancel_to_launcher_fake.call_count);
  TEST_ASSERT_EQUAL_INT(0, screen_preset_actions_apply_name_fake.call_count);
  TEST_ASSERT_EQUAL_STRING("cancelled", testlog_fake.arg1_history[testlog_fake.call_count - 1]);
}

// A successful ACTIONS_NAME-scope re-dictation
// is the ACTIONS Rename arc (running/live timers) -- same verbatim-transcript
// contract as NAME/PRESET_NAME, routed to screen_actions_apply_name() (a
// live Timer, addressed by ACTIONS' own held cookie -- a third sink).
void test_callback_success_actions_name_scope_calls_apply_name(void) {
  dictation_am_start(DICT_SCOPE_ACTIONS_NAME);
  DictationSessionStatusCallback cb = registered_callback();

  cb(kSession, DictationSessionStatusSuccess, (char *)"Focus", NULL);

  TEST_ASSERT_EQUAL_INT(1, screen_actions_apply_name_fake.call_count);
  TEST_ASSERT_EQUAL_STRING("Focus", screen_actions_apply_name_fake.arg0_val);
  TEST_ASSERT_EQUAL_INT(0, screen_voice_set_name_fake.call_count);
  TEST_ASSERT_EQUAL_INT(0, screen_preset_actions_apply_name_fake.call_count);
  TEST_ASSERT_EQUAL_INT(0, screen_voice_show_parse_fake.call_count);
  TEST_ASSERT_EQUAL_INT(0, screen_voice_cancel_to_launcher_fake.call_count);
  TEST_ASSERT_EQUAL_STRING("dictation_outcome", testlog_fake.arg0_history[testlog_fake.call_count - 1]);
  TEST_ASSERT_EQUAL_STRING("actions_name_set", testlog_fake.arg1_history[testlog_fake.call_count - 1]);
}

// A FAILED/cancelled ACTIONS_NAME attempt must never disturb the ACTIONS menu
// it was launched from -- same silent no-op, testlog-only handling
// NAME/PRESET_NAME both use.
void test_callback_failed_actions_name_scope_leaves_the_menu_untouched(void) {
  dictation_am_start(DICT_SCOPE_ACTIONS_NAME);
  DictationSessionStatusCallback cb = registered_callback();

  cb(kSession, DictationSessionStatusFailureNoSpeechDetected, NULL, NULL);

  TEST_ASSERT_EQUAL_INT(0, screen_actions_apply_name_fake.call_count);
  TEST_ASSERT_EQUAL_INT(0, screen_voice_show_parse_fake.call_count);
  TEST_ASSERT_EQUAL_INT(0, screen_voice_cancel_to_launcher_fake.call_count);
  TEST_ASSERT_EQUAL_STRING("dictation_outcome", testlog_fake.arg0_history[testlog_fake.call_count - 1]);
  TEST_ASSERT_EQUAL_STRING("no_speech", testlog_fake.arg1_history[testlog_fake.call_count - 1]);
}

// Same rule for a Back-during-ACTIONS_NAME-dictation (Rejected) -- FULL-scope
// routes this to screen_voice_cancel_to_launcher(); ACTIONS_NAME must NOT,
// since the user only backed out of the Rename sub-step, not left ACTIONS.
void test_callback_rejected_actions_name_scope_does_not_cancel_to_launcher(void) {
  dictation_am_start(DICT_SCOPE_ACTIONS_NAME);
  DictationSessionStatusCallback cb = registered_callback();

  cb(kSession, DictationSessionStatusFailureTranscriptionRejected, NULL, NULL);

  TEST_ASSERT_EQUAL_INT(0, screen_voice_cancel_to_launcher_fake.call_count);
  TEST_ASSERT_EQUAL_INT(0, screen_actions_apply_name_fake.call_count);
  TEST_ASSERT_EQUAL_STRING("cancelled", testlog_fake.arg1_history[testlog_fake.call_count - 1]);
}

// --- the status callback: a rejected/cancelled dictation -------------------
// lib/voice_outcome.h's own doc comment: Rejected(1)/
// RejectedWithError(2) is a SILENT return to the launcher, never a
// screen_voice content state -- pressing Back while the real system
// dictation UI is showing delivers exactly this status. The destination is
// screen_voice_cancel_to_launcher() (screen_voice.c's own shared seam, which
// internally decides HOME vs. LIBRARY based on the session's target); this
// test only proves dictation_am.c calls the shared seam -- the target
// decision itself is screen_voice.c's own responsibility, out of this
// file's scope.

void test_callback_rejected_status_cancels_to_launcher_not_screen_voice(void) {
  DictationSessionStatus rejected_statuses[] = {
      DictationSessionStatusFailureTranscriptionRejected,
      DictationSessionStatusFailureTranscriptionRejectedWithError,
  };
  for (size_t i = 0; i < sizeof rejected_statuses / sizeof rejected_statuses[0]; i++) {
    reset_module(/*create_succeeds=*/true);
    dictation_am_start(DICT_SCOPE_FULL);
    DictationSessionStatusCallback cb = registered_callback();

    cb(kSession, rejected_statuses[i], NULL, NULL);

    TEST_ASSERT_EQUAL_INT(1, screen_voice_cancel_to_launcher_fake.call_count);
    TEST_ASSERT_EQUAL_INT(0, screen_voice_show_parse_fake.call_count);
    TEST_ASSERT_EQUAL_STRING("dictation_outcome",
                              testlog_fake.arg0_history[testlog_fake.call_count - 1]);
    TEST_ASSERT_EQUAL_STRING("cancelled", testlog_fake.arg1_history[testlog_fake.call_count - 1]);
  }
}

// --- the status callback: every OTHER dictation-level failure class --------
// Every failure-status case, already covered host-side by voice_outcome_test.c --
// this proves dictation_am.c's own INT-CAST BOUNDARY (lib/voice_outcome.h's
// own doc comment: "The adapter ... is the only file that ever sees the real
// enum; it casts to int at the call site") routes each real
// DictationSessionStatus enum value through correctly, end to end.

typedef struct {
  DictationSessionStatus status;
  VoiceOutcome expected_outcome;
  const char *expected_tag;
} StatusCase;

static const StatusCase kStatusCases[] = {
    {DictationSessionStatusFailureSystemAborted, VOICE_ERROR, "error"},
    {DictationSessionStatusFailureNoSpeechDetected, VOICE_NO_SPEECH, "no_speech"},
    {DictationSessionStatusFailureConnectivityError, VOICE_NO_PHONE, "no_phone"},
    {DictationSessionStatusFailureDisabled, VOICE_DISABLED, "disabled"},
    {DictationSessionStatusFailureInternalError, VOICE_ERROR, "error"},
    {DictationSessionStatusFailureRecognizerError, VOICE_ERROR, "error"},
};

void test_callback_every_dictation_failure_status_routes_to_its_documented_outcome(void) {
  for (size_t i = 0; i < sizeof kStatusCases / sizeof kStatusCases[0]; i++) {
    reset_module(/*create_succeeds=*/true);
    dictation_am_start(DICT_SCOPE_FULL);
    DictationSessionStatusCallback cb = registered_callback();

    cb(kSession, kStatusCases[i].status, NULL, NULL);

    TEST_ASSERT_EQUAL_INT(1, screen_voice_show_parse_fake.call_count);
    TEST_ASSERT_NULL(screen_voice_show_parse_fake.arg0_val);
    TEST_ASSERT_NULL(screen_voice_show_parse_fake.arg1_val);
    TEST_ASSERT_EQUAL_INT(kStatusCases[i].expected_outcome, screen_voice_show_parse_fake.arg2_val);
    TEST_ASSERT_EQUAL_STRING("dictation_outcome",
                              testlog_fake.arg0_history[testlog_fake.call_count - 1]);
    TEST_ASSERT_EQUAL_STRING(kStatusCases[i].expected_tag,
                              testlog_fake.arg1_history[testlog_fake.call_count - 1]);
  }
}

void test_callback_transcription_null_on_a_failure_status_does_not_crash(void) {
  dictation_am_start(DICT_SCOPE_FULL);
  DictationSessionStatusCallback cb = registered_callback();

  cb(kSession, DictationSessionStatusFailureNoSpeechDetected, NULL, NULL);

  TEST_ASSERT_EQUAL_INT(1, screen_voice_show_parse_fake.call_count);
  TEST_ASSERT_EQUAL_INT(VOICE_NO_SPEECH, screen_voice_show_parse_fake.arg2_val);
}

void test_callback_transcript_longer_than_buffer_is_truncated_safely(void) {
  // DICTATION_BUFFER_SIZE is 64 -- feed a much longer string and confirm the
  // copy is still a valid, safely-terminated C string (no overflow/crash),
  // matching pebble.h's own "transcript longer than buffer_size is silently
  // truncated" contract.
  char long_transcript[200];
  memset(long_transcript, 'a', sizeof long_transcript - 1);
  long_transcript[sizeof long_transcript - 1] = '\0';

  dictation_am_start(DICT_SCOPE_FULL);
  DictationSessionStatusCallback cb = registered_callback();
  cb(kSession, DictationSessionStatusSuccess, long_transcript, NULL);

  TEST_ASSERT_EQUAL_INT(1, screen_voice_show_parse_fake.call_count);
  const char *copied = screen_voice_show_parse_fake.arg0_val;
  TEST_ASSERT_NOT_NULL(copied);
  TEST_ASSERT_TRUE(strlen(copied) < 64);
}

// --- dictation_am_cancel() ----------------------------------------------------

void test_cancel_stops_an_in_progress_session_and_clears_the_guard(void) {
  dictation_am_start(DICT_SCOPE_FULL);
  TEST_ASSERT_EQUAL_INT(0, dictation_session_stop_fake.call_count);

  dictation_am_cancel();

  TEST_ASSERT_EQUAL_INT(1, dictation_session_stop_fake.call_count);
  TEST_ASSERT_EQUAL_PTR(kSession, dictation_session_stop_fake.arg0_val);
  // No outcome is ever delivered for a cancelled attempt -- an abort, not a
  // resolved (even if "cancelled") outcome.
  TEST_ASSERT_EQUAL_INT(0, screen_voice_show_parse_fake.call_count);

  // The guard is clear again -- a fresh attempt works normally.
  RESET_FAKE(dictation_session_start);
  FFF_RESET_HISTORY();
  dictation_am_start(DICT_SCOPE_FULL);
  TEST_ASSERT_EQUAL_INT(1, dictation_session_start_fake.call_count);
}

void test_cancel_is_a_noop_when_nothing_is_in_progress(void) {
  dictation_am_cancel();  // nothing started at all

  TEST_ASSERT_EQUAL_INT(0, dictation_session_stop_fake.call_count);
}

void test_a_stale_callback_after_cancel_is_never_actually_invoked_by_this_fake(void) {
  // pebble.h documents "no status callbacks will be received after the
  // session is stopped" -- this test exists to record that dictation_am.c
  // relies on that OS-level guarantee rather than tracking its own
  // generation counter (the fake here never invokes a callback unless the
  // TEST does so explicitly, which is the same trust boundary the real SDK
  // gives dictation_am.c). Documented, not separately re-proven, since
  // there's nothing left to fake past dictation_session_stop() itself.
  dictation_am_start(DICT_SCOPE_FULL);
  dictation_am_cancel();
  TEST_ASSERT_EQUAL_INT(1, dictation_session_stop_fake.call_count);
}

// --- dictation_am_deinit() ----------------------------------------------------

void test_deinit_destroys_the_session_and_a_later_start_recreates_it(void) {
  // setUp's own reset_module(true) already created a live session -- clear
  // just the create-call history (not the module's internal state) so this
  // test's own counts start from a clean, easy-to-reason-about zero.
  RESET_FAKE(dictation_session_create);
  RESET_FAKE(dictation_session_destroy);
  FFF_RESET_HISTORY();

  dictation_am_deinit();

  TEST_ASSERT_EQUAL_INT(1, dictation_session_destroy_fake.call_count);
  TEST_ASSERT_EQUAL_PTR(kSession, dictation_session_destroy_fake.arg0_val);

  dictation_session_create_fake.return_val = kSession;
  dictation_am_start(DICT_SCOPE_FULL);

  TEST_ASSERT_EQUAL_INT(1, dictation_session_create_fake.call_count);  // recreated, lazily
  TEST_ASSERT_EQUAL_INT(1, dictation_session_start_fake.call_count);
}

void test_deinit_when_no_session_exists_does_not_call_destroy(void) {
  reset_module(/*create_succeeds=*/false);

  dictation_am_deinit();

  TEST_ASSERT_EQUAL_INT(0, dictation_session_destroy_fake.call_count);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_init_creates_session_and_configures_confirmation_off_error_dialogs_on);
  RUN_TEST(test_init_when_create_fails_does_not_configure_a_null_session);
  RUN_TEST(test_start_full_calls_dictation_session_start_and_logs_start_full);
  RUN_TEST(test_start_when_session_start_fails_synchronously_clears_guard_and_delivers_no_phone);
  RUN_TEST(test_start_name_calls_dictation_session_start_and_logs_start_name);
  RUN_TEST(test_start_preset_name_calls_dictation_session_start_and_logs_start_name);
  RUN_TEST(test_start_actions_name_calls_dictation_session_start_and_logs_start_name);
  RUN_TEST(test_start_is_a_reentrancy_guard_second_call_before_callback_is_a_noop);
  RUN_TEST(test_callback_clears_the_reentrancy_guard_allowing_a_new_start);
  RUN_TEST(test_start_delivers_no_phone_synchronously_when_no_session_exists);
  RUN_TEST(test_start_after_a_failed_delivery_is_not_stuck_by_the_reentrancy_guard);
  RUN_TEST(test_start_retries_creation_and_succeeds_once_a_phone_connects);
  RUN_TEST(test_callback_success_full_scope_parses_and_shows_confirm_preview);
  RUN_TEST(test_callback_success_full_scope_clock_target_resolves_via_real_clock);
  RUN_TEST(test_callback_success_full_scope_unparseable_shows_parse_fail);
  RUN_TEST(test_callback_success_full_scope_when_vocabulary_unavailable_shows_data_unavailable);
  RUN_TEST(test_callback_success_name_scope_sets_the_name_via_screen_voice);
  RUN_TEST(test_callback_failed_name_scope_leaves_the_preview_untouched);
  RUN_TEST(test_callback_rejected_name_scope_does_not_navigate_home);
  RUN_TEST(test_callback_success_preset_name_scope_calls_apply_name);
  RUN_TEST(test_callback_failed_preset_name_scope_leaves_the_sheet_untouched);
  RUN_TEST(test_callback_rejected_preset_name_scope_does_not_navigate_home);
  RUN_TEST(test_callback_success_actions_name_scope_calls_apply_name);
  RUN_TEST(test_callback_failed_actions_name_scope_leaves_the_menu_untouched);
  RUN_TEST(test_callback_rejected_actions_name_scope_does_not_cancel_to_launcher);
  RUN_TEST(test_callback_rejected_status_cancels_to_launcher_not_screen_voice);
  RUN_TEST(test_callback_every_dictation_failure_status_routes_to_its_documented_outcome);
  RUN_TEST(test_callback_transcription_null_on_a_failure_status_does_not_crash);
  RUN_TEST(test_callback_transcript_longer_than_buffer_is_truncated_safely);
  RUN_TEST(test_cancel_stops_an_in_progress_session_and_clears_the_guard);
  RUN_TEST(test_cancel_is_a_noop_when_nothing_is_in_progress);
  RUN_TEST(test_a_stale_callback_after_cancel_is_never_actually_invoked_by_this_fake);
  RUN_TEST(test_deinit_destroys_the_session_and_a_later_start_recreates_it);
  RUN_TEST(test_deinit_when_no_session_exists_does_not_call_destroy);
  return UNITY_END();
}
