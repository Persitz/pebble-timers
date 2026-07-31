// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// Exercises wakeup_adapter.c's priority-safe, admission-correct,
// query-verified retry scheduling against FFF fakes of the
// wakeup_schedule()/wakeup_query()/wakeup_cancel()/wakeup_cancel_all()
// surface (test/fakes/pebble.h) plus fakes of testlog()/testlog_init()
// (declared by testlog_am.h) -- faked here rather than linking
// testlog_am.c, since this test is about SCHEDULING decisions, not the
// test-log channel (covered by its own dedicated test). Compiled via
// test/Makefile's dedicated wakeup_adapter_test rule (NOT the generic
// lib/-only pattern rule), linking the real lib/sched.c + lib/store.c +
// lib/timer.c so the retry-ladder/spacing/priority/admission logic under
// test is the actual production code, not a stand-in.
#include "wakeup_adapter.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "fff.h"
#include "lib/store.h"
#include "pebble.h"  // test/fakes/pebble.h -- see test/Makefile's -I ordering
#include "testlog_am.h"
#include "unity.h"

DEFINE_FFF_GLOBALS;

FAKE_VALUE_FUNC(WakeupId, wakeup_schedule, time_t, int32_t, bool);
FAKE_VALUE_FUNC(bool, wakeup_query, WakeupId, time_t *);
FAKE_VOID_FUNC(wakeup_cancel, WakeupId);
FAKE_VOID_FUNC(wakeup_cancel_all);
FAKE_VOID_FUNC(testlog, const char *, const char *);
FAKE_VOID_FUNC(testlog_init);
// wakeup_adapter.c's durable diagnostic (the last wakeup scheduling outcome,
// keys 32/33) touches persist_* on every replan now -- faked here so this
// stays link-complete; the diagnostic's own correctness is exercised below
// against a small fake flash scoped to just those two keys.
FAKE_VALUE_FUNC(bool, persist_exists, uint32_t);
FAKE_VALUE_FUNC(int32_t, persist_read_int, uint32_t);
FAKE_VALUE_FUNC(status_t, persist_write_int, uint32_t, int32_t);
FAKE_VALUE_FUNC(int, persist_read_data, uint32_t, void *, size_t);
FAKE_VALUE_FUNC(int, persist_write_data, uint32_t, const void *, size_t);
FAKE_VALUE_FUNC(status_t, persist_delete, uint32_t);

// app_log() is variadic -- a plain manual definition (matching the
// declaration test/fakes/pebble.h provides) rather than FFF's own vararg
// macros, since no assertion here needs more than "did it fire" and "what
// did it say".
static int s_app_log_call_count;
static char s_last_app_log_line[64];

void app_log(uint8_t log_level, const char *src_filename, int src_line_number, const char *fmt, ...) {
  (void)log_level;
  (void)src_filename;
  (void)src_line_number;
  s_app_log_call_count++;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(s_last_app_log_line, sizeof s_last_app_log_line, fmt, ap);
  va_end(ap);
}

// -- Fake flash for the durable diagnostic's two keys (32 data / 33 commit
// marker) only -- full round-trip coverage of persist_adapter.h's OWN keys
// is persist_adapter_test.c's job, not this file's.
#define DIAG_DATA_KEY 32u
#define DIAG_COMMIT_KEY 33u

static bool s_diagflash_data_exists;
static uint8_t s_diagflash_data[8];  // sizeof wakeup_adapter.c's private WakeupDiagRecord
static int s_diagflash_data_len;
static bool s_diagflash_commit_exists;
static int32_t s_diagflash_commit_value;
static bool s_diagflash_fail_data_write;
static bool s_diagflash_short_data_write;

static void diagflash_reset(void) {
  s_diagflash_data_exists = false;
  memset(s_diagflash_data, 0, sizeof s_diagflash_data);
  s_diagflash_data_len = 0;
  s_diagflash_commit_exists = false;
  s_diagflash_commit_value = 0;
  s_diagflash_fail_data_write = false;
  s_diagflash_short_data_write = false;
}

static bool fake_diag_persist_exists(uint32_t key) {
  return key == DIAG_COMMIT_KEY && s_diagflash_commit_exists;
}

static int32_t fake_diag_persist_read_int(uint32_t key) {
  return (key == DIAG_COMMIT_KEY) ? s_diagflash_commit_value : 0;
}

static status_t fake_diag_persist_write_int(uint32_t key, int32_t value) {
  if (key != DIAG_COMMIT_KEY) return -1;
  s_diagflash_commit_value = value;
  s_diagflash_commit_exists = true;
  return (status_t)sizeof(int32_t);
}

static int fake_diag_persist_read_data(uint32_t key, void *buffer, size_t buffer_size) {
  if (key != DIAG_DATA_KEY || !s_diagflash_data_exists) return -1;
  size_t n = (size_t)s_diagflash_data_len > buffer_size ? buffer_size : (size_t)s_diagflash_data_len;
  memcpy(buffer, s_diagflash_data, n);
  return s_diagflash_data_len;
}

static int fake_diag_persist_write_data(uint32_t key, const void *data, size_t size) {
  if (key != DIAG_DATA_KEY || size > sizeof s_diagflash_data) return -1;
  if (s_diagflash_fail_data_write) return -1;
  if (s_diagflash_short_data_write) {
    // A REAL torn write, not a no-op that merely reports a short count. The
    // old fake returned size-1 without mutating a single byte, so it could
    // not model the acknowledged torn condition at all. It proved only that
    // the marker is not published, never that the PAYLOAD is left mixed.
    // Copy a prefix, leave the tail as the previous record's bytes, and
    // report the short count.
    size_t torn = size - 1;
    memcpy(s_diagflash_data, data, torn);
    s_diagflash_data_len = (int)size;
    s_diagflash_data_exists = true;
    return (int)torn;
  }
  memcpy(s_diagflash_data, data, size);
  s_diagflash_data_len = (int)size;
  s_diagflash_data_exists = true;
  return (int)size;
}

// The marker key's own delete -- wakeup_diag_save() now invalidates key 33
// BEFORE overwriting key 32, so a torn payload write leaves no marker
// vouching for the mixed bytes.
static status_t fake_diag_persist_delete(uint32_t key) {
  if (key != DIAG_COMMIT_KEY) return -1;
  s_diagflash_commit_exists = false;
  s_diagflash_commit_value = 0;
  return 1;  // S_TRUE -- emery 4.17's documented success value for persist_delete
}

// testlog()'s `data` argument points at a STACK-local buffer in the caller
// that goes out of scope the instant testlog() returns (same hazard
// test/persist_adapter_test.c's own top comment documents) -- capture it
// HERE, inside the fake, while the pointer is still valid.
static bool s_diag_prev_seen;
static char s_diag_prev_data[24];

static void capture_diag_prev_testlog(const char *tag, const char *data) {
  if (tag != NULL && strcmp(tag, "wakeup:diag_prev") == 0) {
    s_diag_prev_seen = true;
    snprintf(s_diag_prev_data, sizeof s_diag_prev_data, "%s", data != NULL ? data : "");
  }
}

// An hour out -- plenty of lead for every rung of every ladder tested here.
#define NOW ((time_t)1000000)
#define T (NOW + 3600)

static TimerStore s_store;

void setUp(void) {
  RESET_FAKE(wakeup_schedule);
  RESET_FAKE(wakeup_query);
  RESET_FAKE(wakeup_cancel);
  RESET_FAKE(wakeup_cancel_all);
  RESET_FAKE(testlog);
  RESET_FAKE(testlog_init);
  RESET_FAKE(persist_exists);
  RESET_FAKE(persist_read_int);
  RESET_FAKE(persist_write_int);
  RESET_FAKE(persist_read_data);
  RESET_FAKE(persist_write_data);
  RESET_FAKE(persist_delete);
  FFF_RESET_HISTORY();
  store_init(&s_store);
  // Every positive WakeupId verifies true by default -- the query step is
  // opt-out (a test explicitly returning false) rather than opt-in, so the
  // many tests below that only care about wakeup_schedule() behavior don't
  // all need to remember to arm it.
  wakeup_query_fake.return_val = true;
  diagflash_reset();
  persist_exists_fake.custom_fake = fake_diag_persist_exists;
  persist_read_int_fake.custom_fake = fake_diag_persist_read_int;
  persist_write_int_fake.custom_fake = fake_diag_persist_write_int;
  persist_read_data_fake.custom_fake = fake_diag_persist_read_data;
  persist_write_data_fake.custom_fake = fake_diag_persist_write_data;
  persist_delete_fake.custom_fake = fake_diag_persist_delete;
  testlog_fake.custom_fake = capture_diag_prev_testlog;
  s_app_log_call_count = 0;
  s_last_app_log_line[0] = '\0';
  s_diag_prev_seen = false;
  s_diag_prev_data[0] = '\0';
  wakeup_adapter_test_reset_diag_dump();
}

void tearDown(void) {}

// Searches wakeup_schedule()'s call history for a call at exactly `at` with
// exactly `cookie` -- used below instead of a fixed call-index assertion
// where the whole point is that a call must happen AT ALL
// (an index assertion alone couldn't distinguish "happened at the wrong
// index" from "never happened").
static bool schedule_was_called_with(time_t at, int32_t cookie) {
  for (size_t i = 0; i < wakeup_schedule_fake.call_count; i++) {
    if (wakeup_schedule_fake.arg0_history[i] == at && wakeup_schedule_fake.arg1_history[i] == cookie) {
      return true;
    }
  }
  return false;
}

// Defined further down (with the healing tests that first needed it).
static int count_calls_with(time_t at, int32_t cookie);

// --- happy path --------------------------------------------------------

void test_single_timer_accepted_on_time_then_gets_redundant_rung(void) {
  int32_t cookie;
  store_add(&s_store, KIND_COUNTDOWN, T - NOW, NOW, &cookie);
  wakeup_schedule_fake.return_val = 42;

  wakeup_adapter_replan(&s_store, NOW);

  TEST_ASSERT_EQUAL_INT(1, wakeup_cancel_all_fake.call_count);
  TEST_ASSERT_EQUAL_INT(2, wakeup_schedule_fake.call_count);
  TEST_ASSERT_EQUAL(T, wakeup_schedule_fake.arg0_history[0]);
  TEST_ASSERT_EQUAL_INT32(cookie, wakeup_schedule_fake.arg1_history[0]);
  TEST_ASSERT_EQUAL(T + 60, wakeup_schedule_fake.arg0_history[1]);
  TEST_ASSERT_EQUAL_INT32(cookie, wakeup_schedule_fake.arg1_history[1]);
  TEST_ASSERT_EQUAL_INT(2, wakeup_query_fake.call_count);
  TEST_ASSERT_EQUAL(42, wakeup_query_fake.arg0_history[1]);
  TEST_ASSERT_EQUAL_STRING("wakeup:fallback_armed", testlog_fake.arg0_history[1]);
}

void test_empty_store_cancels_but_schedules_nothing(void) {
  wakeup_adapter_replan(&s_store, NOW);

  TEST_ASSERT_EQUAL_INT(1, wakeup_cancel_all_fake.call_count);
  TEST_ASSERT_EQUAL_INT(0, wakeup_schedule_fake.call_count);
  TEST_ASSERT_EQUAL_INT(0, wakeup_query_fake.call_count);
}

// --- E_RANGE retry ladder -----------------------------------------------

static WakeupId only_negative_rung_accepts(time_t at, int32_t cookie, bool notify) {
  (void)cookie;
  (void)notify;
  return (at == T - 60) ? 7 : E_RANGE;
}

void test_e_range_negative_rung_accepted_when_eligible(void) {
  int32_t cookie;
  store_add(&s_store, KIND_COUNTDOWN, T - NOW, NOW, &cookie);
  wakeup_schedule_fake.custom_fake = only_negative_rung_accepts;

  wakeup_adapter_replan(&s_store, NOW);

  TEST_ASSERT_EQUAL_INT(3, wakeup_schedule_fake.call_count);
  TEST_ASSERT_EQUAL(T, wakeup_schedule_fake.arg0_history[0]);
  TEST_ASSERT_EQUAL(T - 60, wakeup_schedule_fake.arg0_history[1]);
  TEST_ASSERT_EQUAL(T + 60, wakeup_schedule_fake.arg0_history[2]);
}

static WakeupId only_positive_rung_accepts(time_t at, int32_t cookie, bool notify) {
  (void)cookie;
  (void)notify;
  return (at == T + 60) ? 7 : E_RANGE;
}

void test_e_range_ladder_tries_negative_then_positive_rung(void) {
  // A collider exactly AT T blocks both
  // T and T-60 (distance 60 is legal, but this fake models a case where the
  // negative rung is ALSO rejected, e.g. a second collider) -- the ladder
  // must still fall through to T+60 rather than giving up after one retry.
  int32_t cookie;
  store_add(&s_store, KIND_COUNTDOWN, T - NOW, NOW, &cookie);
  wakeup_schedule_fake.custom_fake = only_positive_rung_accepts;

  wakeup_adapter_replan(&s_store, NOW);

  TEST_ASSERT_EQUAL_INT(4, wakeup_schedule_fake.call_count);
  TEST_ASSERT_EQUAL(T, wakeup_schedule_fake.arg0_history[0]);
  TEST_ASSERT_EQUAL(T - 60, wakeup_schedule_fake.arg0_history[1]);
  TEST_ASSERT_EQUAL(T + 60, wakeup_schedule_fake.arg0_history[2]);
  TEST_ASSERT_EQUAL(T + 120, wakeup_schedule_fake.arg0_history[3]);
}

void test_e_range_both_fallbacks_rejected_leaves_unarmed(void) {
  int32_t cookie;
  store_add(&s_store, KIND_COUNTDOWN, T - NOW, NOW, &cookie);
  wakeup_schedule_fake.return_val = E_RANGE;  // every attempt rejected

  wakeup_adapter_replan(&s_store, NOW);

  // T, T-60, T+60 -- 3 attempts total, none accepted, then stop (no 4th).
  TEST_ASSERT_EQUAL_INT(3, wakeup_schedule_fake.call_count);
  TEST_ASSERT_EQUAL_INT(0, wakeup_query_fake.call_count);  // never called -- nothing ever succeeded
}

void test_short_lead_uses_forward_only_ladder(void) {
  // requested_at - 60 == now + 4, one second short of the SCHED_MIN_LEAD_S
  // (5s) floor -- the negative rung is not a real future wakeup, so the
  // ladder must substitute {+60, +120} rather than {-60, +60}.
  int32_t cookie;
  time_t short_t = NOW + 64;
  store_add(&s_store, KIND_COUNTDOWN, short_t - NOW, NOW, &cookie);
  wakeup_schedule_fake.return_val = E_RANGE;

  wakeup_adapter_replan(&s_store, NOW);

  TEST_ASSERT_EQUAL_INT(3, wakeup_schedule_fake.call_count);
  TEST_ASSERT_EQUAL(short_t, wakeup_schedule_fake.arg0_history[0]);
  TEST_ASSERT_EQUAL(short_t + 60, wakeup_schedule_fake.arg0_history[1]);
  TEST_ASSERT_EQUAL(short_t + 120, wakeup_schedule_fake.arg0_history[2]);
}

void test_exactly_at_lead_boundary_negative_rung_still_eligible(void) {
  // requested_at - 60 == now + 5 exactly -- sched_negative_rung_eligible
  // uses >=, so this boundary case IS eligible (mirrors sched_test.c's own
  // test_exactly_min_lead_is_schedulable convention for the primary).
  int32_t cookie;
  time_t boundary_t = NOW + 65;
  store_add(&s_store, KIND_COUNTDOWN, boundary_t - NOW, NOW, &cookie);
  wakeup_schedule_fake.return_val = E_RANGE;

  wakeup_adapter_replan(&s_store, NOW);

  TEST_ASSERT_EQUAL(boundary_t - 60, wakeup_schedule_fake.arg0_history[1]);
}

// --- non-E_RANGE statuses: never retried ---------------------------------

void test_e_invalid_argument_is_not_retried(void) {
  int32_t cookie;
  store_add(&s_store, KIND_COUNTDOWN, T - NOW, NOW, &cookie);
  wakeup_schedule_fake.return_val = E_INVALID_ARGUMENT;

  wakeup_adapter_replan(&s_store, NOW);

  TEST_ASSERT_EQUAL_INT(1, wakeup_schedule_fake.call_count);
}

void test_e_out_of_resources_is_not_retried(void) {
  int32_t cookie;
  store_add(&s_store, KIND_COUNTDOWN, T - NOW, NOW, &cookie);
  wakeup_schedule_fake.return_val = E_OUT_OF_RESOURCES;

  wakeup_adapter_replan(&s_store, NOW);

  TEST_ASSERT_EQUAL_INT(1, wakeup_schedule_fake.call_count);
}

void test_e_internal_is_not_retried(void) {
  int32_t cookie;
  store_add(&s_store, KIND_COUNTDOWN, T - NOW, NOW, &cookie);
  wakeup_schedule_fake.return_val = E_INTERNAL;

  wakeup_adapter_replan(&s_store, NOW);

  TEST_ASSERT_EQUAL_INT(1, wakeup_schedule_fake.call_count);
}

void test_unrecognized_negative_status_is_not_retried(void) {
  // An undocumented/future negative code: do not guess, do not retry.
  int32_t cookie;
  store_add(&s_store, KIND_COUNTDOWN, T - NOW, NOW, &cookie);
  wakeup_schedule_fake.return_val = -99;

  wakeup_adapter_replan(&s_store, NOW);

  TEST_ASSERT_EQUAL_INT(1, wakeup_schedule_fake.call_count);
}

void test_e_range_ladder_stops_on_non_e_range_fallback_status(void) {
  // First fallback (T-60) hits a DIFFERENT failure (E_INTERNAL -- a
  // plausible platform-level fault on a retry, e.g. a settings-file-open
  // failure per the plan's Component 2) -- must stop the ladder
  // immediately, not fall through to the +60 rung.
  int32_t cookie;
  store_add(&s_store, KIND_COUNTDOWN, T - NOW, NOW, &cookie);
  WakeupId seq[] = {E_RANGE, E_INTERNAL};
  SET_RETURN_SEQ(wakeup_schedule, seq, 2);

  wakeup_adapter_replan(&s_store, NOW);

  TEST_ASSERT_EQUAL_INT(2, wakeup_schedule_fake.call_count);
}

static int32_t s_retry_candidate_cookie;
static time_t s_retry_candidate_deadline;

// Every candidate's primary succeeds EXCEPT the designated "last" one,
// which fails only at its own original deadline (modeling one foreign
// collider affecting only that single timer).
static WakeupId last_candidate_collision_fake(time_t at, int32_t cookie, bool notify) {
  (void)notify;
  if (cookie == s_retry_candidate_cookie && at == s_retry_candidate_deadline) return E_RANGE;
  return 7;
}

void test_eighth_candidate_retries_while_one_slot_remains(void) {
  // The invariant still holds at exactly 8 candidates: 7 are accepted in
  // pass 1 and the 8th collides. Its OWN fallback attempt must still happen,
  // since accepted_count is genuinely 7 (not 8) when its ladder starts.
  int32_t cookies[8];
  time_t base = T;
  for (int i = 0; i < 8; i++) {
    // Spaced 200s apart -- clear of SCHED_MIN_SPACING_S, so all 8 are real
    // candidates and only the designated one below fails pass 1.
    store_add(&s_store, KIND_COUNTDOWN, (base + i * 200) - NOW, NOW, &cookies[i]);
  }
  s_retry_candidate_cookie = cookies[7];
  s_retry_candidate_deadline = base + 7 * 200;
  wakeup_schedule_fake.custom_fake = last_candidate_collision_fake;

  wakeup_adapter_replan(&s_store, NOW);

  // 7 primaries accepted (7 calls) + the 8th's own primary rejected (1
  // call, E_RANGE) + the 8th's own fallback accepted (1 call) = 9 total.
  TEST_ASSERT_EQUAL_INT(9, wakeup_schedule_fake.call_count);
  TEST_ASSERT_TRUE(schedule_was_called_with(s_retry_candidate_deadline - 60, cookies[7]));
}

// --- admission is on ACCEPTED entries, not attempts ------------------------

static int32_t s_failed_primary_cookies[8];
static int s_failed_primary_count;

// The 8 designated cookies always fail (modeling a persistent block that
// never actually claims a real accepted slot); every other cookie succeeds.
static WakeupId failed_primary_blackhole_fake(time_t at, int32_t cookie, bool notify) {
  (void)at;
  (void)notify;
  for (int i = 0; i < s_failed_primary_count; i++) {
    if (s_failed_primary_cookies[i] == cookie) return E_RANGE;
  }
  return 7;
}

void test_ninth_timer_not_starved_by_eight_earlier_failures(void) {
  // The exact counterexample: A0..A7 (eight closely-packed,
  // earlier-deadline timers) fail EVERY attempt and never actually consume
  // a real accepted slot. B, ninth by deadline and far away, is clearly
  // schedulable -- but the PRE-fix sched_candidates() truncated the
  // CANDIDATE list at 8 before any real outcome was known, so B was never
  // even a candidate, let alone attempted. Load-bearing assertion: B's own
  // primary attempt actually happens and succeeds. The paired UI-truth
  // assertion requires the no-background indicator to agree: B must read
  // as genuinely armed, not "unschedulable" (what the pre-fix sched_plan()-
  // driven UI would have wrongly shown, or worse, what a STILL-guessing UI
  // could show for the WRONG reason after only the candidate filter changed).
  int32_t cookie_b;
  time_t base = T;
  for (int i = 0; i < 8; i++) {
    store_add(&s_store, KIND_COUNTDOWN, (base + i) - NOW, NOW, &s_failed_primary_cookies[i]);
  }
  s_failed_primary_count = 8;
  store_add(&s_store, KIND_COUNTDOWN, (base + 1000) - NOW, NOW, &cookie_b);

  wakeup_schedule_fake.custom_fake = failed_primary_blackhole_fake;

  wakeup_adapter_replan(&s_store, NOW);

  TEST_ASSERT_TRUE(schedule_was_called_with(base + 1000, cookie_b));
  TEST_ASSERT_TRUE(wakeup_adapter_is_armed(cookie_b));  // UI-truth assertion
}

void test_ninth_candidate_skipped_once_cap_genuinely_full(void) {
  // The complementary case: 8 EARLIER candidates that all genuinely
  // succeed (real accepted entries, not a black hole) -- the 9th, later by
  // deadline, must be skipped WITHOUT a wasted wakeup_schedule() call, since
  // our own bookkeeping already knows for certain it would return
  // E_OUT_OF_RESOURCES. This is the honest, structural exclusion: unlike
  // the pre-fix bug, it applies only once 8 REAL entries are confirmed,
  // never as a guess.
  int32_t cookies[9];
  time_t base = T;
  for (int i = 0; i < 9; i++) {
    store_add(&s_store, KIND_COUNTDOWN, (base + i * 200) - NOW, NOW, &cookies[i]);
  }
  wakeup_schedule_fake.return_val = 7;  // every attempt succeeds

  wakeup_adapter_replan(&s_store, NOW);

  TEST_ASSERT_EQUAL_INT(8, wakeup_schedule_fake.call_count);  // the 9th never attempted
  TEST_ASSERT_FALSE(schedule_was_called_with(base + 8 * 200, cookies[8]));
  TEST_ASSERT_TRUE(wakeup_adapter_is_armed(cookies[0]));    // one of the 8 real accepts
  TEST_ASSERT_FALSE(wakeup_adapter_is_armed(cookies[8]));   // UI-truth assertion: correctly unarmed
}

// --- the true worst-case call ceiling is 48, with bounded counts even at
//     full MAX_TIMERS cardinality and zero successes -----------------------

void test_max_cardinality_no_success_hits_true_48_call_ceiling(void) {
  // MAX_TIMERS(16) candidates, every attempt (primary + both fallback
  // rungs) always rejected -- the TRUE worst-case call volume is
  // MAX_TIMERS * 3 = 48, not "3 x SCHED_MAX_WAKEUPS = 24", which
  // incorrectly assumes candidates are still capped at 8. This also proves
  // the accepted set stays correctly empty (bounded, not overflowed)
  // throughout -- nothing here should ever read as armed.
  int32_t cookies[MAX_TIMERS];
  time_t base = T;
  for (int i = 0; i < MAX_TIMERS; i++) {
    store_add(&s_store, KIND_COUNTDOWN, (base + i * 1000) - NOW, NOW, &cookies[i]);
  }
  wakeup_schedule_fake.return_val = E_RANGE;  // every attempt, every candidate, always rejected

  wakeup_adapter_replan(&s_store, NOW);

  TEST_ASSERT_EQUAL_INT(MAX_TIMERS * 3, wakeup_schedule_fake.call_count);  // 48
  for (int i = 0; i < MAX_TIMERS; i++) {
    TEST_ASSERT_FALSE(wakeup_adapter_is_armed(cookies[i]));
  }
}

// --- immediate wakeup_query() verification -------------------------------

void test_query_false_never_cancels_anything_and_still_arms(void) {
  // EVERY read-back fails -- the worst case for the query layer. Firmware
  // accepted the
  // request, so the timer must end ARMED, nothing may be cancelled, and no
  // extra schedule call may be made. This is byte-for-byte what the adapter
  // did before any query existed, which is exactly the point: on this path
  // the query layer must be behaviourally inert.
  int32_t cookie;
  store_add(&s_store, KIND_COUNTDOWN, T - NOW, NOW, &cookie);
  wakeup_schedule_fake.return_val = 7;
  wakeup_query_fake.return_val = false;

  wakeup_adapter_replan(&s_store, NOW);

  TEST_ASSERT_TRUE(wakeup_adapter_is_armed(cookie));
  TEST_ASSERT_EQUAL_INT(0, wakeup_cancel_fake.call_count);  // nothing destroyed, ever
  // Primary at T, then rank 3's insurance rung at T+60. No re-arm, no probe.
  TEST_ASSERT_EQUAL_INT(2, wakeup_schedule_fake.call_count);
  TEST_ASSERT_EQUAL(T, wakeup_schedule_fake.arg0_history[0]);
  TEST_ASSERT_EQUAL(T + 60, wakeup_schedule_fake.arg0_history[1]);
  // Two reads per attempt (rung 0's re-read), and that is the whole ladder.
  TEST_ASSERT_EQUAL_INT(4, wakeup_query_fake.call_count);
  // Distinguishable via the wakeup:schedule_unverified testlog tag from a
  // confirmed accept.
  TEST_ASSERT_EQUAL_STRING("wakeup:schedule_unverified", testlog_fake.arg0_history[0]);
}

// The documented spurious case: a transient settings-file reopen failure
// that clears. The FIRST read-back says gone, the second says present.
static int s_query_false_budget;
static bool query_false_for_the_first_n_calls(WakeupId id, time_t *out_at) {
  (void)id;
  if (out_at != NULL) *out_at = 0;
  if (s_query_false_budget > 0) {
    s_query_false_budget--;
    return false;
  }
  return true;
}

void test_transient_query_false_recovers_on_the_second_read(void) {
  // Rung 0 -- the entire ladder, and the only thing the query layer does
  // beyond confirming. One extra read, no side effects, and the entry is
  // genuinely confirmed rather than merely accepted.
  int32_t cookie;
  store_add(&s_store, KIND_COUNTDOWN, T - NOW, NOW, &cookie);
  wakeup_schedule_fake.return_val = 7;
  s_query_false_budget = 1;
  wakeup_query_fake.custom_fake = query_false_for_the_first_n_calls;

  wakeup_adapter_replan(&s_store, NOW);

  TEST_ASSERT_TRUE(wakeup_adapter_is_armed(cookie));
  TEST_ASSERT_EQUAL_INT(0, wakeup_cancel_fake.call_count);
  TEST_ASSERT_EQUAL_INT(2, wakeup_schedule_fake.call_count);
  // Confirmed, so the ordinary marker -- not the unverified one.
  TEST_ASSERT_EQUAL_STRING("wakeup:schedule", testlog_fake.arg0_history[0]);
}

void test_a_transient_query_failure_never_ends_a_replan_with_zero_entries(void) {
  // The binding constraint, stated as an invariant rather than a call
  // count: however many read-backs fail, a single countdown
  // whose request firmware ACCEPTED must not come out of the replan with
  // nothing armed.
  int32_t cookie;
  store_add(&s_store, KIND_COUNTDOWN, T - NOW, NOW, &cookie);
  wakeup_schedule_fake.return_val = 7;
  wakeup_query_fake.custom_fake = query_false_for_the_first_n_calls;

  // 0 failing reads = confirmed on the first read; 1 = recovered by rung 0's
  // re-read; 2 = both reads failed, accepted on firmware's acceptance alone.
  // Each replan starts from a fresh accepted set, so all three are real,
  // independent checks.
  for (int failing_reads = 0; failing_reads <= 2; failing_reads++) {
    s_query_false_budget = failing_reads;

    wakeup_adapter_replan(&s_store, NOW);

    TEST_ASSERT_TRUE(wakeup_adapter_is_armed(cookie));
    TEST_ASSERT_EQUAL_INT(0, wakeup_cancel_fake.call_count);
  }
}

void test_query_false_on_a_fallback_rung_arms_and_does_not_shift_further(void) {
  // T is refused with E_RANGE; T-60 is ACCEPTED by firmware but its
  // read-backs both fail. The rung is armed anyway, so the timestamp ladder
  // stops there -- it must never fall through to T+60 for this timer's own
  // deadline. (T+60 does get requested, but as rank 3's insurance rung off
  // the armed T-60, which is a different rank with a different purpose.)
  int32_t cookie;
  store_add(&s_store, KIND_COUNTDOWN, T - NOW, NOW, &cookie);
  wakeup_schedule_fake.custom_fake = only_negative_rung_accepts;  // only T-60 succeeds
  wakeup_query_fake.return_val = false;

  wakeup_adapter_replan(&s_store, NOW);

  TEST_ASSERT_TRUE(wakeup_adapter_is_armed(cookie));
  TEST_ASSERT_EQUAL_INT(0, wakeup_cancel_fake.call_count);
  TEST_ASSERT_EQUAL_INT(3, wakeup_schedule_fake.call_count);  // T, T-60, then T+60 (rank 3)
  TEST_ASSERT_EQUAL(T, wakeup_schedule_fake.arg0_history[0]);
  TEST_ASSERT_EQUAL(T - 60, wakeup_schedule_fake.arg0_history[1]);
  TEST_ASSERT_EQUAL(T + 60, wakeup_schedule_fake.arg0_history[2]);
  // The rung was attempted exactly once -- no same-timestamp re-arm.
  TEST_ASSERT_EQUAL_INT(1, count_calls_with(T - 60, cookie));
}

void test_unconfirmed_entry_occupies_its_slot_but_does_not_block_a_legal_sibling(void) {
  // This test used to assert the opposite premise ("a query-false phantom
  // must never enter the local accepted set"). An entry firmware ACCEPTED
  // is kept whether or not the
  // read-back confirmed it, so it does occupy its slot for spacing -- which
  // is exactly what it did before any query existed, and is the honest
  // reading of firmware's own state.
  //
  // What still must hold is that this costs no OTHER timer a legal attempt:
  // B's deadline is 90 s away, comfortably outside the 60 s window, so B's
  // own on-time primary must still be tried.
  int32_t cookie_a, cookie_b;
  store_add(&s_store, KIND_COUNTDOWN, T - NOW, NOW, &cookie_a);         // A: T
  store_add(&s_store, KIND_COUNTDOWN, (T + 90) - NOW, NOW, &cookie_b);  // B: T+90

  wakeup_schedule_fake.return_val = 7;   // every schedule call succeeds
  wakeup_query_fake.return_val = false;  // ...but no read-back ever confirms

  wakeup_adapter_replan(&s_store, NOW);

  TEST_ASSERT_TRUE(wakeup_adapter_is_armed(cookie_a));
  TEST_ASSERT_TRUE(wakeup_adapter_is_armed(cookie_b));
  TEST_ASSERT_EQUAL_INT(0, wakeup_cancel_fake.call_count);
  TEST_ASSERT_TRUE(schedule_was_called_with(T, cookie_a));
  TEST_ASSERT_TRUE(schedule_was_called_with(T + 90, cookie_b));
  // Each primary attempted exactly once -- no re-arm for either.
  TEST_ASSERT_EQUAL_INT(1, count_calls_with(T, cookie_a));
  TEST_ASSERT_EQUAL_INT(1, count_calls_with(T + 90, cookie_b));
}

// --- the best-effort cancel is intentionally absent -----------------------
//
// An indeterminate live entry must not sit untracked while firmware acts on
// it, but a best-effort wakeup_cancel() is worse than the ambiguity: the
// cancel can succeed, another producer can take the freed
// window, and the timer ends with ZERO entries where trusting firmware's
// acceptance would have left two. Tracking the entry closes the
// bookkeeping gap without destroying it.
//
// These two tests previously asserted the cancel happened. They now assert
// it does NOT -- they are the direct regression guard against reintroducing
// it.

void test_query_false_on_primary_never_cancels_the_returned_id(void) {
  int32_t cookie;
  store_add(&s_store, KIND_COUNTDOWN, T - NOW, NOW, &cookie);
  wakeup_schedule_fake.return_val = 77;
  wakeup_query_fake.return_val = false;

  wakeup_adapter_replan(&s_store, NOW);

  TEST_ASSERT_EQUAL_INT(0, wakeup_cancel_fake.call_count);
  TEST_ASSERT_TRUE(wakeup_adapter_is_armed(cookie));
}

void test_query_false_on_fallback_never_cancels_the_returned_id(void) {
  int32_t cookie;
  store_add(&s_store, KIND_COUNTDOWN, T - NOW, NOW, &cookie);
  wakeup_schedule_fake.custom_fake = only_negative_rung_accepts;  // T-60 -> id 7
  wakeup_query_fake.return_val = false;

  wakeup_adapter_replan(&s_store, NOW);

  TEST_ASSERT_EQUAL_INT(0, wakeup_cancel_fake.call_count);
  TEST_ASSERT_TRUE(wakeup_adapter_is_armed(cookie));
}

void test_query_true_never_cancels(void) {
  int32_t cookie;
  store_add(&s_store, KIND_COUNTDOWN, T - NOW, NOW, &cookie);
  wakeup_schedule_fake.return_val = 7;
  wakeup_query_fake.return_val = true;

  wakeup_adapter_replan(&s_store, NOW);

  TEST_ASSERT_EQUAL_INT(0, wakeup_cancel_fake.call_count);
}

// --- a foreign collision must not cost an innocent timer its on-time slot --

// Models a single foreign collider at COLLIDER: any request within 60s of
// it (strict, matching the confirmed firmware boundary) is rejected.
#define COLLIDER (T - 59)

static WakeupId collider_distance_fake(time_t at, int32_t cookie, bool notify) {
  (void)cookie;
  (void)notify;
  time_t diff = at - COLLIDER;
  if (diff < 0) diff = -diff;
  return (diff < 60) ? E_RANGE : 7;
}

void test_foreign_collider_does_not_defeat_an_innocent_later_timer(void) {
  // Foreign collider at T-59. Timer A's primary (T) is 59s from the
  // collider -> E_RANGE. Timer B's primary (T+30) is 89s from the collider
  // -> would be ACCEPTED -- but the OLD sched_plan() greedy local-spacing
  // pre-filter discarded B before pass 1 ever ran, purely because B's
  // ORIGINAL T (T+30) is only 30s after A's ORIGINAL T, without knowing A
  // was about to fail. sched_candidates() must make B a real candidate
  // regardless of A's fate, and pass 1 must actually attempt B's own T.
  int32_t cookie_a, cookie_b;
  store_add(&s_store, KIND_COUNTDOWN, T - NOW, NOW, &cookie_a);         // A: T
  store_add(&s_store, KIND_COUNTDOWN, (T + 30) - NOW, NOW, &cookie_b);  // B: T+30
  wakeup_schedule_fake.custom_fake = collider_distance_fake;

  wakeup_adapter_replan(&s_store, NOW);

  // The load-bearing assertion: B's own on-time primary was actually
  // attempted. Under the pre-fix code this call never happened at all.
  TEST_ASSERT_TRUE(schedule_was_called_with(T + 30, cookie_b));
  // And it must have been genuinely accepted+armed (not just attempted) --
  // A's own negative rung (T-60, distance 1 from the collider) is rejected
  // by firmware, so if B never landed in the accepted[] set as an ARMED
  // entry, A's positive rung (T+60) would be free to land there instead;
  // asserting A's OWN ladder never reaches T+60 confirms B's slot was
  // treated as real by the local spacing guard, not just "attempted."
  TEST_ASSERT_TRUE(schedule_was_called_with(T - 60, cookie_a));  // A's negative rung: attempted
  TEST_ASSERT_FALSE(schedule_was_called_with(T + 60, cookie_a));  // A's positive rung: blocked by B's real slot (30s away)
}

void test_foreign_collider_behavior_is_independent_of_store_insertion_order(void) {
  // Same geometry as above, but B is added to the store BEFORE A -- proves
  // sched_candidates() orders by DEADLINE, not insertion order, so the
  // fix does not depend on which timer the user happened to create first.
  int32_t cookie_a, cookie_b;
  store_add(&s_store, KIND_COUNTDOWN, (T + 30) - NOW, NOW, &cookie_b);  // B added first
  store_add(&s_store, KIND_COUNTDOWN, T - NOW, NOW, &cookie_a);         // A added second
  wakeup_schedule_fake.custom_fake = collider_distance_fake;

  wakeup_adapter_replan(&s_store, NOW);

  TEST_ASSERT_TRUE(schedule_was_called_with(T + 30, cookie_b));
  TEST_ASSERT_TRUE(schedule_was_called_with(T - 60, cookie_a));
}

// --- priority safety: an accepted on-time primary is never displaced -----

static time_t s_priority_test_b_deadline;

// B's own on-time primary always succeeds; A's only succeeds at A_T+60
// (everything else, including A's own T and A's T-60, which lands exactly
// on B's slot, is rejected).
static WakeupId priority_test_fake(time_t at, int32_t cookie, bool notify) {
  (void)cookie;
  (void)notify;
  if (at == s_priority_test_b_deadline) return 7;
  if (at == T + 60) return 7;
  return E_RANGE;
}

void test_retry_never_lands_on_another_timers_accepted_slot(void) {
  // B is exactly 60s before A -- both are valid, spaced-apart candidates. A
  // collides at its own T; A's negative rung (A_T - 60) lands EXACTLY on
  // B's already-accepted on-time slot, so the adapter's runtime spacing
  // check must skip it (no wasted/colliding call) and fall through to
  // A_T + 60 instead -- B's on-time schedule is never touched.
  int32_t cookie_a, cookie_b;
  time_t deadline_b = T - 60;
  s_priority_test_b_deadline = deadline_b;
  store_add(&s_store, KIND_COUNTDOWN, deadline_b - NOW, NOW, &cookie_b);
  store_add(&s_store, KIND_COUNTDOWN, T - NOW, NOW, &cookie_a);

  wakeup_schedule_fake.custom_fake = priority_test_fake;

  wakeup_adapter_replan(&s_store, NOW);

  // Pass 1: B's T (accept), A's T (E_RANGE) -- 2 calls.
  // Pass 2: A's negative rung (== B's accepted slot) is filtered out by the
  // local spacing check BEFORE any wakeup_schedule call; A's positive rung
  // (A_T+60) is attempted and accepted -- 1 more call. Rank 3 then tries
  // one insurance rung for each armed timer, so there are two additional
  // attempts. Neither can displace B's already-accepted on-time slot.
  TEST_ASSERT_EQUAL_INT(5, wakeup_schedule_fake.call_count);
  TEST_ASSERT_EQUAL(deadline_b, wakeup_schedule_fake.arg0_history[0]);
  TEST_ASSERT_EQUAL(T, wakeup_schedule_fake.arg0_history[1]);
  TEST_ASSERT_EQUAL(T + 60, wakeup_schedule_fake.arg0_history[2]);
  // B's own slot was requested exactly once, for cookie_b, and never retried.
  TEST_ASSERT_EQUAL_INT32(cookie_b, wakeup_schedule_fake.arg1_history[0]);
}

// --- rank 3: post-deadline insurance --------------------------------------

static int32_t s_recovered_cookie;
static time_t s_recovered_primary;

static WakeupId recovered_primary_fake(time_t at, int32_t cookie, bool notify) {
  (void)notify;
  if (cookie == s_recovered_cookie && at == s_recovered_primary) return E_RANGE;
  return 7;
}

void test_redundant_rungs_follow_deadline_order_not_accept_append_order(void) {
  // A is earliest but its primary fails and is recovered at T-60 during rank
  // 2, after B's primary was appended. Rank 3 must nevertheless allocate A's
  // rung before B's, following the deadline-sorted candidate model.
  int32_t cookie_a, cookie_b;
  store_add(&s_store, KIND_COUNTDOWN, T - NOW, NOW, &cookie_a);
  store_add(&s_store, KIND_COUNTDOWN, (T + 200) - NOW, NOW, &cookie_b);
  s_recovered_cookie = cookie_a;
  s_recovered_primary = T;
  wakeup_schedule_fake.custom_fake = recovered_primary_fake;

  wakeup_adapter_replan(&s_store, NOW);

  TEST_ASSERT_EQUAL_INT(5, wakeup_schedule_fake.call_count);
  TEST_ASSERT_EQUAL(T, wakeup_schedule_fake.arg0_history[0]);
  TEST_ASSERT_EQUAL(T + 200, wakeup_schedule_fake.arg0_history[1]);
  TEST_ASSERT_EQUAL(T - 60, wakeup_schedule_fake.arg0_history[2]);
  // A's accepted T-60 would normally yield T, but T is the known failed
  // primary. Its ONE insurance rung skips to T+60, before B's T+260 rung.
  TEST_ASSERT_EQUAL(T + 60, wakeup_schedule_fake.arg0_history[3]);
  TEST_ASSERT_EQUAL_INT32(cookie_a, wakeup_schedule_fake.arg1_history[3]);
  TEST_ASSERT_EQUAL(T + 260, wakeup_schedule_fake.arg0_history[4]);
  TEST_ASSERT_EQUAL_INT32(cookie_b, wakeup_schedule_fake.arg1_history[4]);
  TEST_ASSERT_EQUAL_STRING("wakeup:fallback_armed", testlog_fake.arg0_history[3]);
}

void test_replan_after_primary_fire_cancels_and_drops_its_redundant_rung(void) {
  // The primary's normal scan sets fired=true. The next replan must cancel
  // both live entries and sched_candidates() must skip the fired timer, so a
  // healthy primary can never produce a second buzz from its insurance rung.
  int32_t cookie;
  store_add(&s_store, KIND_COUNTDOWN, T - NOW, NOW, &cookie);
  wakeup_schedule_fake.return_val = 7;
  wakeup_adapter_replan(&s_store, NOW);
  TEST_ASSERT_EQUAL_INT(2, wakeup_schedule_fake.call_count);
  TEST_ASSERT_TRUE(wakeup_adapter_is_armed(cookie));

  Timer *timer = store_find(&s_store, cookie);
  TEST_ASSERT_NOT_NULL(timer);
  timer->fired = true;
  wakeup_adapter_replan(&s_store, T);

  TEST_ASSERT_EQUAL_INT(2, wakeup_cancel_all_fake.call_count);
  TEST_ASSERT_EQUAL_INT(2, wakeup_schedule_fake.call_count);
  TEST_ASSERT_FALSE(wakeup_adapter_is_armed(cookie));
}

// --- below-min-lead exclusion ----------------------------------------------

void test_below_lead_timer_is_excluded_without_a_schedule_attempt(void) {
  int32_t cookie;
  store_add(&s_store, KIND_COUNTDOWN, 3, NOW, &cookie);  // deadline NOW+3 < NOW+SCHED_MIN_LEAD_S(5)

  wakeup_adapter_replan(&s_store, NOW);

  TEST_ASSERT_EQUAL_INT(0, wakeup_schedule_fake.call_count);
}

// --- cancel-all shape ----------------------------------------------------

void test_cancel_all_called_exactly_once_regardless_of_plan_size(void) {
  int32_t cookies[3];
  store_add(&s_store, KIND_COUNTDOWN, 100, NOW, &cookies[0]);
  store_add(&s_store, KIND_COUNTDOWN, 300, NOW, &cookies[1]);
  store_add(&s_store, KIND_COUNTDOWN, 500, NOW, &cookies[2]);
  wakeup_schedule_fake.return_val = 1;

  wakeup_adapter_replan(&s_store, NOW);

  TEST_ASSERT_EQUAL_INT(1, wakeup_cancel_all_fake.call_count);
}

// --- UI truth: wakeup_adapter_is_armed() ----------------------------------

void test_is_armed_true_for_a_just_armed_cookie(void) {
  int32_t cookie;
  store_add(&s_store, KIND_COUNTDOWN, T - NOW, NOW, &cookie);
  wakeup_schedule_fake.return_val = 7;

  wakeup_adapter_replan(&s_store, NOW);

  TEST_ASSERT_TRUE(wakeup_adapter_is_armed(cookie));
}

void test_is_armed_false_for_an_unknown_cookie(void) {
  TEST_ASSERT_FALSE(wakeup_adapter_is_armed(999));
}

void test_is_armed_false_once_a_later_replan_no_longer_includes_it(void) {
  // The armed set must not leak across replans -- e.g. a timer that was
  // armed, then deleted (or the store simply changed), must stop reading as
  // armed once a real replan has run without it.
  int32_t cookie;
  store_add(&s_store, KIND_COUNTDOWN, T - NOW, NOW, &cookie);
  wakeup_schedule_fake.return_val = 7;
  wakeup_adapter_replan(&s_store, NOW);
  TEST_ASSERT_TRUE(wakeup_adapter_is_armed(cookie));

  store_init(&s_store);
  wakeup_adapter_replan(&s_store, NOW);

  TEST_ASSERT_FALSE(wakeup_adapter_is_armed(cookie));
}

void test_is_armed_false_for_a_candidate_the_adapter_actually_excluded(void) {
  // Direct proof that the UI-truth query reflects the REAL adapter outcome,
  // not a re-derived guess: a candidate FIRMWARE ITSELF REFUSED must read as
  // not armed, even though it was a perfectly ordinary lead-eligible
  // candidate the pure planner would happily have claimed.
  //
  // The refusal has to come from wakeup_schedule(), not from wakeup_query().
  // This test used to drive it with a query-false read-back, which no longer
  // means "not armed" -- firmware accepted that request, so the timer IS
  // armed and reporting otherwise would be the UI falsehood, not the truth.
  // See wakeup_adapter.h's note on wakeup_adapter_is_armed().
  int32_t cookie;
  store_add(&s_store, KIND_COUNTDOWN, T - NOW, NOW, &cookie);
  wakeup_schedule_fake.return_val = E_INTERNAL;  // firmware refuses, every rung

  wakeup_adapter_replan(&s_store, NOW);

  TEST_ASSERT_FALSE(wakeup_adapter_is_armed(cookie));
}

void test_is_armed_true_when_firmware_accepted_but_the_read_back_never_confirmed(void) {
  // The other side of the same contract, made explicit so nobody "restores"
  // the assertion above by reintroducing query-false-means-unarmed.
  int32_t cookie;
  store_add(&s_store, KIND_COUNTDOWN, T - NOW, NOW, &cookie);
  wakeup_schedule_fake.return_val = 7;
  wakeup_query_fake.return_val = false;

  wakeup_adapter_replan(&s_store, NOW);

  TEST_ASSERT_TRUE(wakeup_adapter_is_armed(cookie));
}

// --- replan-as-healing: the emergent "total, not incremental" invariant ---
// wakeup_adapter_replan() always
// wakeup_cancel_all()s and rebuilds from scratch rather than reconciling a
// delta. This is the reason a fallback placement is not a permanent
// demotion: the NEXT replan retries every candidate at its own ideal
// deadline again, so a timer degraded to a shifted rung by a foreign
// collider heals back to its ideal time as soon as that collider clears.
// Pin it here: a future "skip the replan when the desired set looks
// unchanged" optimization would freeze the degraded placement forever
// instead, and this test must fail against exactly that mutation. The
// negative-control mutation skips the replan when the desired set is
// unchanged.

static bool s_healing_collider_present;

// Models a single foreign collider that blocks ONLY the exact ideal
// deadline T -- present on the first replan, cleared by the second.
static WakeupId healing_collider_fake(time_t at, int32_t cookie, bool notify) {
  (void)cookie;
  (void)notify;
  return (s_healing_collider_present && at == T) ? E_RANGE : 7;
}

static int count_calls_with(time_t at, int32_t cookie) {
  int n = 0;
  for (size_t i = 0; i < wakeup_schedule_fake.call_count; i++) {
    if (wakeup_schedule_fake.arg0_history[i] == at && wakeup_schedule_fake.arg1_history[i] == cookie) {
      n++;
    }
  }
  return n;
}

void test_degraded_placement_heals_once_the_collider_clears(void) {
  int32_t cookie;
  store_add(&s_store, KIND_COUNTDOWN, T - NOW, NOW, &cookie);
  s_healing_collider_present = true;
  wakeup_schedule_fake.custom_fake = healing_collider_fake;

  // First replan: the ideal deadline T collides and is rejected; the timer
  // degrades to the negative fallback rung T-60 instead.
  wakeup_adapter_replan(&s_store, NOW);
  TEST_ASSERT_EQUAL_INT(1, count_calls_with(T, cookie));
  TEST_ASSERT_TRUE(schedule_was_called_with(T - 60, cookie));

  // The collider clears. A reconciler that only armed the DELTA between the
  // old and new desired sets -- or an "optimization" that skips the whole
  // replan because the store's own desired candidates look unchanged --
  // would never call wakeup_schedule() at T again: nothing about the STORE
  // changed, only external firmware state the adapter cannot see in
  // advance. Because replan is instead total (unconditional
  // wakeup_cancel_all() + full rebuild every time), the ideal deadline is
  // genuinely retried.
  s_healing_collider_present = false;
  wakeup_adapter_replan(&s_store, NOW);

  TEST_ASSERT_EQUAL_INT(2, wakeup_cancel_all_fake.call_count);
  // Load-bearing assertion: T was attempted a SECOND time -- this is what a
  // "skip if unchanged" mutation would defeat. schedule_was_called_with()
  // alone would not catch that mutation, since it already returns true after
  // replan 1's own (rejected) primary attempt at T.
  TEST_ASSERT_EQUAL_INT(2, count_calls_with(T, cookie));
  // And the retry actually healed: T is now the timer's own armed slot,
  // recovered from the degraded T-60 placement.
  TEST_ASSERT_TRUE(wakeup_adapter_is_armed(cookie));
}

// --- durable diagnostic: the last wakeup scheduling outcome ---------------
// This fixes the removed prototype's exact defects: wrong (replan-time)
// timestamp, a torn/foreign record accepted as valid, and no reader.

void test_diag_saves_the_actual_attempted_timestamp_not_replan_time(void) {
  int32_t cookie;
  store_add(&s_store, KIND_COUNTDOWN, T - NOW, NOW, &cookie);
  wakeup_schedule_fake.return_val = 7;  // primary at T, then its own redundant rung at T+60

  wakeup_adapter_replan(&s_store, NOW);  // saves {status=7, at=T} at the end of this call

  wakeup_adapter_test_reset_diag_dump();  // simulate the next process launching
  store_init(&s_store);  // nothing to attempt this time -- isolates the DUMP from a new SAVE

  wakeup_adapter_replan(&s_store, NOW);  // dumps the record the FIRST replan saved

  TEST_ASSERT_TRUE(s_diag_prev_seen);
  // `at` is T, not T+60: rank 3's insurance rung does not write this record.
  // The test's own point is unaffected:
  // T != NOW still proves the attempted timestamp is recorded rather than
  // replan time, which is the removed prototype's exact defect.
  char expected[24];
  snprintf(expected, sizeof expected, "%ld@%ld", (long)7, (long)T);
  TEST_ASSERT_EQUAL_STRING(expected, s_diag_prev_data);
  TEST_ASSERT_EQUAL_INT(1, s_app_log_call_count);
  TEST_ASSERT_TRUE(strstr(s_last_app_log_line, "7") != NULL);
  TEST_ASSERT_NOT_EQUAL(NOW, T);
}

// --- the durable diagnostic must not report FAILURE for a successful
//     schedule. The existing test above pins the `at` half; `status` is the
//     half that can manufacture a false alarm. -----------------------------

// One foreign collider at T+30: everything within 60 s of it is refused.
// This is the ordinary shape of the scheduling bug.
static WakeupId foreign_collider_at_t_plus_30(time_t at, int32_t cookie, bool notify) {
  (void)cookie;
  (void)notify;
  time_t diff = at - (T + 30);
  if (diff < 0) diff = -diff;
  return (diff < 60) ? E_RANGE : (WakeupId)7;
}

void test_diag_ignores_a_failed_insurance_rung_after_a_good_primary(void) {
  // Primary at T -> E_RANGE (collider 30 s away).
  // Retry rung T-60 -> ACCEPTED. The alarm is armed and will fire.
  // Rank 3's insurance rung: sched_redundant_at(T-60, T) == T+60 -> E_RANGE
  //   (same collider, 30 s away).
  // The record must describe the LOAD-BEARING outcome {7, T-60}, never the
  // optional rung's refusal {E_RANGE, T+60}. Reading `last_status=-8` off
  // `pebble logs` for a healthy schedule is exactly what would send the
  // hardware-verification pass chasing a collider that did no harm.
  int32_t cookie;
  store_add(&s_store, KIND_COUNTDOWN, T - NOW, NOW, &cookie);
  wakeup_schedule_fake.custom_fake = foreign_collider_at_t_plus_30;

  wakeup_adapter_replan(&s_store, NOW);

  // Precondition: the scenario really did play out as described -- the alarm
  // IS armed, and the insurance rung really WAS attempted and really DID
  // fail. Without this the assertion below could pass vacuously.
  TEST_ASSERT_TRUE(wakeup_adapter_is_armed(cookie));
  TEST_ASSERT_TRUE(schedule_was_called_with(T + 60, cookie));

  wakeup_adapter_test_reset_diag_dump();
  store_init(&s_store);
  wakeup_adapter_replan(&s_store, NOW);

  TEST_ASSERT_TRUE(s_diag_prev_seen);
  char expected[24];
  snprintf(expected, sizeof expected, "%ld@%ld", (long)7, (long)(T - 60));
  TEST_ASSERT_EQUAL_STRING(expected, s_diag_prev_data);
  // The precise thing that used to be persisted, spelled out so a future
  // regression is legible rather than just "string mismatch".
  char regressed[24];
  snprintf(regressed, sizeof regressed, "%ld@%ld", (long)E_RANGE, (long)(T + 60));
  TEST_ASSERT_TRUE(strcmp(regressed, s_diag_prev_data) != 0);
}

void test_diag_records_a_successful_insurance_rungs_base_not_the_rung_itself(void) {
  // The single-timer happy path, stated as the status half rather than the
  // timestamp half: a replan where EVERY rank succeeds must still describe
  // the primary, because that is the entry the user's alarm depends on.
  int32_t cookie;
  store_add(&s_store, KIND_COUNTDOWN, T - NOW, NOW, &cookie);
  wakeup_schedule_fake.return_val = 42;

  wakeup_adapter_replan(&s_store, NOW);

  wakeup_adapter_test_reset_diag_dump();
  store_init(&s_store);
  wakeup_adapter_replan(&s_store, NOW);

  char expected[24];
  snprintf(expected, sizeof expected, "%ld@%ld", (long)42, (long)T);
  TEST_ASSERT_EQUAL_STRING(expected, s_diag_prev_data);
}

void test_diag_records_armed_but_unconfirmed_distinctly_from_a_refusal(void) {
  // The durable field record must be able to say "firmware accepted this and
  // the timer IS armed, but the read-back never confirmed it". This must
  // NOT be confused with a scheduling refusal by whoever reads `pebble logs` off a
  // real watch. WAKEUP_QUERY_FAILED (-100) is that value; every real
  // StatusCode is in -3..-8, so the two can never collide.
  int32_t cookie;
  store_add(&s_store, KIND_COUNTDOWN, T - NOW, NOW, &cookie);
  wakeup_schedule_fake.return_val = 7;
  wakeup_query_fake.return_val = false;

  wakeup_adapter_replan(&s_store, NOW);

  // Precondition: the timer really is armed. Without this the record below
  // would be indistinguishable from a genuine failure.
  TEST_ASSERT_TRUE(wakeup_adapter_is_armed(cookie));

  wakeup_adapter_test_reset_diag_dump();
  store_init(&s_store);
  wakeup_adapter_replan(&s_store, NOW);

  TEST_ASSERT_TRUE(s_diag_prev_seen);
  char expected[24];
  snprintf(expected, sizeof expected, "%ld@%ld", (long)WAKEUP_QUERY_FAILED, (long)T);
  TEST_ASSERT_EQUAL_STRING(expected, s_diag_prev_data);
  // ...and the human-readable line says so in words, so the number cannot be
  // misread as a failure by someone without this source file to hand.
  TEST_ASSERT_TRUE(strstr(s_last_app_log_line, "armed, unconfirmed") != NULL);
}

void test_diag_dump_treats_a_missing_commit_marker_as_no_record(void) {
  // Payload data exists (an earlier write reached key 32), but the commit
  // marker (key 33) never landed -- the exact torn shape the removed
  // prototype mis-handled ("a torn or future-version record is therefore
  // returned as valid rather than unavailable").
  s_diagflash_data_exists = true;
  s_diagflash_data_len = 8;

  wakeup_adapter_replan(&s_store, NOW);  // empty store -- isolates the dump path

  TEST_ASSERT_FALSE(s_diag_prev_seen);
  TEST_ASSERT_EQUAL_INT(0, s_app_log_call_count);
}

void test_diag_dump_rejects_a_commit_marker_that_does_not_match_the_schema_tag(void) {
  s_diagflash_data_exists = true;
  s_diagflash_data_len = 8;
  s_diagflash_commit_exists = true;
  s_diagflash_commit_value = 0x1234;  // a foreign/leftover value, not the real magic

  wakeup_adapter_replan(&s_store, NOW);

  TEST_ASSERT_FALSE(s_diag_prev_seen);
}

void test_diag_dump_fires_at_most_once_per_process(void) {
  int32_t cookie;
  store_add(&s_store, KIND_COUNTDOWN, T - NOW, NOW, &cookie);
  wakeup_schedule_fake.return_val = 7;
  wakeup_adapter_replan(&s_store, NOW);  // first "process": nothing to dump yet, then saves

  // Same process, another real replan -- must NOT re-dump.
  wakeup_adapter_replan(&s_store, NOW);
  TEST_ASSERT_FALSE(s_diag_prev_seen);
  TEST_ASSERT_EQUAL_INT(0, s_app_log_call_count);
}

void test_diag_commit_marker_is_never_published_after_a_failed_payload_write(void) {
  int32_t cookie;
  store_add(&s_store, KIND_COUNTDOWN, T - NOW, NOW, &cookie);
  wakeup_schedule_fake.return_val = 7;
  s_diagflash_fail_data_write = true;

  wakeup_adapter_replan(&s_store, NOW);  // the payload write fails

  TEST_ASSERT_FALSE(s_diagflash_commit_exists);  // never published on a failed payload write

  // A later boot must therefore see no record at all, never a stale/mixed one.
  wakeup_adapter_test_reset_diag_dump();
  store_init(&s_store);
  wakeup_adapter_replan(&s_store, NOW);
  TEST_ASSERT_FALSE(s_diag_prev_seen);
}

void test_diag_short_payload_write_never_publishes_the_commit_marker(void) {
  int32_t cookie;
  store_add(&s_store, KIND_COUNTDOWN, T - NOW, NOW, &cookie);
  wakeup_schedule_fake.return_val = 7;
  s_diagflash_short_data_write = true;  // simulates a torn/incomplete flash write

  wakeup_adapter_replan(&s_store, NOW);

  TEST_ASSERT_FALSE(s_diagflash_commit_exists);
}

void test_diag_torn_write_over_an_existing_record_publishes_nothing(void) {
  // This covers a case the two tests above cannot reach: a device that
  // ALREADY has a committed record. Key 33 holds one permanent
  // schema magic with no generation identity, so under the old
  // write-payload-then-publish-marker order the marker stayed valid across
  // the torn write and vouched for the half-old/half-new bytes -- the reader
  // then logged a mixed {status, at} pair as a real scheduling fact.
  int32_t cookie;
  store_add(&s_store, KIND_COUNTDOWN, T - NOW, NOW, &cookie);
  wakeup_schedule_fake.return_val = 7;

  wakeup_adapter_replan(&s_store, NOW);  // commits a good record {7, T}
  TEST_ASSERT_TRUE(s_diagflash_commit_exists);

  // Now a second replan whose payload write tears, over that existing record.
  s_diagflash_short_data_write = true;
  wakeup_schedule_fake.return_val = 9;
  wakeup_adapter_replan(&s_store, NOW);

  // The marker must be gone -- not left standing over mixed bytes.
  TEST_ASSERT_FALSE(s_diagflash_commit_exists);
  // ...and the payload really WAS mutated, so this is a genuine torn write
  // and not a fake that merely reported a short count without touching
  // anything (which is exactly why the old test could not catch this).
  TEST_ASSERT_EQUAL_INT32(9, *(const int32_t *)(const void *)s_diagflash_data);

  // A later boot therefore reads no record at all, rather than a mixed one.
  wakeup_adapter_test_reset_diag_dump();
  store_init(&s_store);
  wakeup_adapter_replan(&s_store, NOW);
  TEST_ASSERT_FALSE(s_diag_prev_seen);
  TEST_ASSERT_EQUAL_INT(0, s_app_log_call_count);
}

void test_diag_empty_plan_replan_does_not_overwrite_the_prior_record(void) {
  int32_t cookie;
  store_add(&s_store, KIND_COUNTDOWN, T - NOW, NOW, &cookie);
  wakeup_schedule_fake.return_val = 7;
  wakeup_adapter_replan(&s_store, NOW);  // saves {status=7, at=T}

  store_init(&s_store);  // now empty -- nothing lead-eligible, no new attempt
  wakeup_adapter_replan(&s_store, NOW);  // must not touch the diagnostic record

  wakeup_adapter_test_reset_diag_dump();
  wakeup_adapter_replan(&s_store, NOW);  // "next boot" dump -- still empty, no new save either

  TEST_ASSERT_TRUE(s_diag_prev_seen);
  char expected[24];
  snprintf(expected, sizeof expected, "%ld@%ld", (long)7, (long)T);
  TEST_ASSERT_EQUAL_STRING(expected, s_diag_prev_data);
}

void test_diag_records_a_terminal_failure_status_not_just_successes(void) {
  // A candidate that never gets armed at all is exactly the case this
  // diagnostic exists for -- the record must show the real negative status,
  // not silently omit it.
  int32_t cookie;
  store_add(&s_store, KIND_COUNTDOWN, T - NOW, NOW, &cookie);
  wakeup_schedule_fake.return_val = E_INVALID_ARGUMENT;  // terminal, no retry

  wakeup_adapter_replan(&s_store, NOW);

  wakeup_adapter_test_reset_diag_dump();
  store_init(&s_store);
  wakeup_adapter_replan(&s_store, NOW);

  TEST_ASSERT_TRUE(s_diag_prev_seen);
  char expected[24];
  snprintf(expected, sizeof expected, "%ld@%ld", (long)E_INVALID_ARGUMENT, (long)T);
  TEST_ASSERT_EQUAL_STRING(expected, s_diag_prev_data);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_single_timer_accepted_on_time_then_gets_redundant_rung);
  RUN_TEST(test_empty_store_cancels_but_schedules_nothing);
  RUN_TEST(test_e_range_negative_rung_accepted_when_eligible);
  RUN_TEST(test_e_range_ladder_tries_negative_then_positive_rung);
  RUN_TEST(test_e_range_both_fallbacks_rejected_leaves_unarmed);
  RUN_TEST(test_short_lead_uses_forward_only_ladder);
  RUN_TEST(test_exactly_at_lead_boundary_negative_rung_still_eligible);
  RUN_TEST(test_e_invalid_argument_is_not_retried);
  RUN_TEST(test_e_out_of_resources_is_not_retried);
  RUN_TEST(test_e_internal_is_not_retried);
  RUN_TEST(test_unrecognized_negative_status_is_not_retried);
  RUN_TEST(test_e_range_ladder_stops_on_non_e_range_fallback_status);
  RUN_TEST(test_eighth_candidate_retries_while_one_slot_remains);
  RUN_TEST(test_ninth_timer_not_starved_by_eight_earlier_failures);
  RUN_TEST(test_ninth_candidate_skipped_once_cap_genuinely_full);
  RUN_TEST(test_max_cardinality_no_success_hits_true_48_call_ceiling);
  RUN_TEST(test_query_false_never_cancels_anything_and_still_arms);
  RUN_TEST(test_transient_query_false_recovers_on_the_second_read);
  RUN_TEST(test_a_transient_query_failure_never_ends_a_replan_with_zero_entries);
  RUN_TEST(test_query_false_on_a_fallback_rung_arms_and_does_not_shift_further);
  RUN_TEST(test_unconfirmed_entry_occupies_its_slot_but_does_not_block_a_legal_sibling);
  RUN_TEST(test_query_false_on_primary_never_cancels_the_returned_id);
  RUN_TEST(test_query_false_on_fallback_never_cancels_the_returned_id);
  RUN_TEST(test_query_true_never_cancels);
  RUN_TEST(test_foreign_collider_does_not_defeat_an_innocent_later_timer);
  RUN_TEST(test_foreign_collider_behavior_is_independent_of_store_insertion_order);
  RUN_TEST(test_retry_never_lands_on_another_timers_accepted_slot);
  RUN_TEST(test_redundant_rungs_follow_deadline_order_not_accept_append_order);
  RUN_TEST(test_replan_after_primary_fire_cancels_and_drops_its_redundant_rung);
  RUN_TEST(test_below_lead_timer_is_excluded_without_a_schedule_attempt);
  RUN_TEST(test_cancel_all_called_exactly_once_regardless_of_plan_size);
  RUN_TEST(test_is_armed_true_for_a_just_armed_cookie);
  RUN_TEST(test_is_armed_false_for_an_unknown_cookie);
  RUN_TEST(test_is_armed_false_once_a_later_replan_no_longer_includes_it);
  RUN_TEST(test_is_armed_false_for_a_candidate_the_adapter_actually_excluded);
  RUN_TEST(test_is_armed_true_when_firmware_accepted_but_the_read_back_never_confirmed);
  RUN_TEST(test_degraded_placement_heals_once_the_collider_clears);
  RUN_TEST(test_diag_saves_the_actual_attempted_timestamp_not_replan_time);
  RUN_TEST(test_diag_ignores_a_failed_insurance_rung_after_a_good_primary);
  RUN_TEST(test_diag_records_a_successful_insurance_rungs_base_not_the_rung_itself);
  RUN_TEST(test_diag_records_armed_but_unconfirmed_distinctly_from_a_refusal);
  RUN_TEST(test_diag_dump_treats_a_missing_commit_marker_as_no_record);
  RUN_TEST(test_diag_dump_rejects_a_commit_marker_that_does_not_match_the_schema_tag);
  RUN_TEST(test_diag_dump_fires_at_most_once_per_process);
  RUN_TEST(test_diag_commit_marker_is_never_published_after_a_failed_payload_write);
  RUN_TEST(test_diag_short_payload_write_never_publishes_the_commit_marker);
  RUN_TEST(test_diag_torn_write_over_an_existing_record_publishes_nothing);
  RUN_TEST(test_diag_empty_plan_replan_does_not_overwrite_the_prior_record);
  RUN_TEST(test_diag_records_a_terminal_failure_status_not_just_successes);
  return UNITY_END();
}
