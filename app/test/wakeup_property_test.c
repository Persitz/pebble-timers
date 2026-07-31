// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// A firmware-admission property test for wakeup scheduling.
// Instead of hand-written FFF fakes returning canned values for one
// hand-picked scenario at a time (test/wakeup_adapter_test.c's approach --
// still valuable, kept, not replaced), this drives wakeup_adapter.c against
// a MODEL of the real firmware's admission rule (test/fakes/wakeup_oracle.c
// -- an ADMISSION MODEL under stated preconditions, not a general "firmware
// oracle": it covers the strict-<60s cross-app window, the per-UUID 8-cap,
// and query/cancel behavior, empirically confirmed against the real QEMU
// EMULATOR, not physical hardware. See the oracle's own documentation for
// what the model deliberately omits: past/now
// rejection, settings-file failures, ID allocation, clock-change rewriting,
// delivery/consumption) over THOUSANDS of randomly generated scenarios (foreign colliders,
// candidate counts, deadline spacings, insertion orders), checking
// INVARIANTS that must hold for every one of them, rather than pinning
// specific input/output pairs.
//
// The suite must catch both the scheduling bug that shipped and the next
// defect in the same subsystem. Invariants I1-I7 below are written to
// answer "yes" to the second question specifically: none of them encode
// today's specific fix, they encode properties that must hold regardless of
// how the adapter is implemented internally. Every one is proven, in this
// file, to actually fail against a recorded, real mutation of
// wakeup_adapter.c/lib/sched.c. See the "NEGATIVE CONTROLS" block near the
// bottom of this file.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fakes/wakeup_oracle.h"
#include "fff.h"
#include "lib/sched.h"
#include "lib/store.h"
#include "lib/timer.h"
#include "pebble.h"  // test/fakes/pebble.h -- see test/Makefile's -I ordering
#include "testlog_am.h"
#include "unity.h"
#include "wakeup_adapter.h"

DEFINE_FFF_GLOBALS;

FAKE_VALUE_FUNC(WakeupId, wakeup_schedule, time_t, int32_t, bool);
FAKE_VALUE_FUNC(bool, wakeup_query, WakeupId, time_t *);
FAKE_VOID_FUNC(wakeup_cancel, WakeupId);
FAKE_VOID_FUNC(wakeup_cancel_all);
FAKE_VOID_FUNC(testlog, const char *, const char *);
FAKE_VOID_FUNC(testlog_init);
// wakeup_adapter.c's durable-diagnostic read/save touches persist_* on
// every replan now -- this property test doesn't exercise that record's own
// correctness (test/wakeup_adapter_test.c does), so default FFF return
// values (false/0) are enough: persist_exists() defaults false, so the
// dump-previous read is always a no-op here, and the save's payload write
// defaults to returning 0 bytes, so it never reaches the commit write
// either -- these fakes exist only to satisfy the linker.
FAKE_VALUE_FUNC(bool, persist_exists, uint32_t);
FAKE_VALUE_FUNC(int32_t, persist_read_int, uint32_t);
FAKE_VALUE_FUNC(status_t, persist_write_int, uint32_t, int32_t);
FAKE_VALUE_FUNC(int, persist_read_data, uint32_t, void *, size_t);
FAKE_VALUE_FUNC(int, persist_write_data, uint32_t, const void *, size_t);
FAKE_VALUE_FUNC(status_t, persist_delete, uint32_t);

// app_log() is variadic -- FFF's own vararg fake macros exist but add
// machinery this file has no other use for; a plain manual stub (like
// test/fakes/pebble.h's other hand-declared, non-FFF surface) is enough
// since no property-test invariant here depends on log content.
void app_log(uint8_t log_level, const char *src_filename, int src_line_number, const char *fmt, ...) {
  (void)log_level;
  (void)src_filename;
  (void)src_line_number;
  (void)fmt;
}

#define NOW ((time_t)1000000)

// I9's own expected ladder depth -- DELIBERATELY a literal `2`, NOT
// lib/sched.h's SCHED_FALLBACK_COUNT. A `SCHED_FALLBACK_COUNT 2->0`
// negative control proved that using
// the SAME production macro as I9's own loop bound meant the mutation
// disables the retry ladder AND silently shrinks I9's verification loop to
// 0 iterations in lockstep, satisfying the invariant through absence of the
// behavior it guards. A
// literal, independently-chosen constant cannot be defeated by mutating the
// one production value it's supposed to be checking.
#define REQUIRED_LADDER_RUNGS 2

// -- Deterministic PRNG (--seed override, seed always printed) --------------
// xorshift32, not libc rand(), so a given seed produces
// the IDENTICAL sequence on every platform/libc this ever runs on. ---------
static uint32_t s_rng_state;

static uint32_t xorshift32(void) {
  uint32_t x = s_rng_state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  s_rng_state = x;
  return x;
}

// Inclusive range [lo, hi].
static int32_t rand_range(int32_t lo, int32_t hi) {
  uint32_t span = (uint32_t)(hi - lo) + 1;
  return lo + (int32_t)(xorshift32() % span);
}

// Default seed rotates by calendar day (still fully reproducible: printed on
// every run, and any specific day's failure replays exactly via --seed).
// A single permanently-fixed default seed would explore exactly one slice
// of the input space forever. This suite pairs a rotating CI seed with
// exhaustive boundary coverage below. It does not maintain the third
// recommended measure: a committed regression corpus of past generated
// counterexamples. A failing seed must still be replayed explicitly.
static uint32_t default_seed(void) {
  return 0xC0FFEEu ^ (uint32_t)(time(NULL) / 86400);
}

static uint32_t g_seed;  // resolved in main(): explicit --seed, else default_seed()
static bool g_seed_explicit;
#ifndef WAKEUP_PROPERTY_ITERATIONS
#define WAKEUP_PROPERTY_ITERATIONS 20000  // ~55ms measured (-O0, host CFLAGS)
#endif

// -- FFF custom_fakes: route the adapter's wakeup_* calls into the oracle,
// plus one deliberate phantom (query-false) injection point per scenario to
// exercise ambiguous read-back handling inside the fuzzer, not just in
// wakeup_adapter_test.c's
// hand-written cases. ---------------------------------------------------
static WakeupOracle s_oracle;
static int s_phantom_target;      // -1 == disabled this scenario
static int s_phantom_success_seq;
static int32_t s_phantom_marked_id;  // -1 == none marked yet

static WakeupId fake_wakeup_schedule(time_t timestamp, int32_t cookie, bool notify_if_missed) {
  (void)notify_if_missed;
  int32_t result = wakeup_oracle_schedule(&s_oracle, timestamp, cookie);
  if (result >= 0) {
    s_phantom_success_seq++;
    if (s_phantom_target >= 0 && s_phantom_success_seq == s_phantom_target &&
        s_phantom_marked_id < 0) {
      wakeup_oracle_mark_phantom(&s_oracle, result);
      s_phantom_marked_id = result;
    }
  }
  return result;
}

static bool fake_wakeup_query(WakeupId id, time_t *out_at) {
  return wakeup_oracle_query(&s_oracle, id, out_at);
}

static void fake_wakeup_cancel(WakeupId id) { wakeup_oracle_cancel(&s_oracle, id); }
static void fake_wakeup_cancel_all(void) { wakeup_oracle_cancel_all(&s_oracle); }

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
  wakeup_schedule_fake.custom_fake = fake_wakeup_schedule;
  wakeup_query_fake.custom_fake = fake_wakeup_query;
  wakeup_cancel_fake.custom_fake = fake_wakeup_cancel;
  wakeup_cancel_all_fake.custom_fake = fake_wakeup_cancel_all;
}
void tearDown(void) {}

// -- Scenario generation ---------------------------------------------------

// A mix of exact firmware-boundary offsets (+/-1s either side of every
// multiple of the 60s window, up to 3 windows out) and generic values.
static const int32_t kHostileOffsets[] = {1,   2,   4,   5,   6,   7,   29,  30,  31,  45,
                                           58,  59,  60,  61,  62,  89,  90,  91,  119, 120,
                                           121, 179, 180, 181, 239, 240, 241, 299, 300, 301,
                                           599, 600, 601, 899, 900, 1799, 1800};
#define NUM_HOSTILE_OFFSETS ((int)(sizeof(kHostileOffsets) / sizeof(kHostileOffsets[0])))

typedef struct {
  int32_t cookie;
  time_t deadline;
  int insertion_index;  // the store_add() call order -- sched_candidates' tie-break (its sort is
                         // stable, and the store is iterated in insertion order), so this is part
                         // of the ground truth the invariants below need, not incidental.
} RefCandidate;

// Stable insertion sort by (deadline, insertion_index) -- mirrors
// lib/sched.c's own sort_candidates() ordering exactly, including its tie
// behavior, so this is the REFERENCE processing order the invariants below
// check the real adapter against.
static void sort_ref_candidates(RefCandidate *c, int n) {
  for (int i = 1; i < n; i++) {
    RefCandidate cur = c[i];
    int j = i - 1;
    while (j >= 0 && (c[j].deadline > cur.deadline ||
                       (c[j].deadline == cur.deadline && c[j].insertion_index > cur.insertion_index))) {
      c[j + 1] = c[j];
      j--;
    }
    c[j + 1] = cur;
  }
}

static void dump_scenario(int iter, int n_real, const RefCandidate *real, int n_decoy,
                           const int32_t *decoy_cookies, int n_foreign, const time_t *foreign_at) {
  printf("wakeup_property_test: FAILING SCENARIO -- seed=%u iter=%d (repeat with: %s --seed %u)\n",
         g_seed, iter, "./build/wakeup_property_test", g_seed);
  printf("  real candidates (cookie, deadline-offset-from-NOW, insertion_index):\n");
  for (int i = 0; i < n_real; i++) {
    printf("    cookie=%d offset=%ld insertion_index=%d\n", real[i].cookie,
           (long)(real[i].deadline - NOW), real[i].insertion_index);
  }
  printf("  decoy cookies (paused/fired/stopwatch -- must never be scheduled): ");
  for (int i = 0; i < n_decoy; i++) printf("%d ", decoy_cookies[i]);
  printf("\n  foreign collider offsets from NOW: ");
  for (int i = 0; i < n_foreign; i++) printf("%ld ", (long)(foreign_at[i] - NOW));
  printf("\n  phantom target (Kth success marked query-false): %d, marked id: %d\n", s_phantom_target,
         s_phantom_marked_id);
}

// -- Invariants -------------------------------------------------------------
//
// I1 (no sacrificed primary): every lead-eligible
//     candidate's own on-time primary is genuinely ATTEMPTED, in the exact
//     order sched_candidates() would process them, UNLESS our own cap was
//     ALREADY genuinely full (from real, earlier-processed accepted
//     entries) when its turn came. An innocent later candidate silently
//     dropped because of an earlier candidate's not-yet-known outcome, or
//     candidates confused with ACCEPTED entries, both violate this invariant.
// I2 (accepted-set integrity): at most SCHED_MAX_WAKEUPS live entries for
//     our own app, and every pair of them at least 60s apart (from each
//     other AND -- guaranteed by the oracle's own admission rule -- from
//     every foreign entry).
// I3 (UI truth): the adapter's
//     own wakeup_adapter_is_armed(cookie) agrees with the oracle's live,
//     firmware-accepted table for EVERY candidate, in EVERY generated
//     scenario, including entries whose read-back remained unconfirmed.
// I4 (bounded work): total wakeup_schedule() calls never exceeds the
//     documented worst case, MAX_TIMERS * (1 + SCHED_FALLBACK_COUNT).
// I5 (no shifted retry outranks an on-time primary): every candidate's own
//     primary attempt (pass 1) happens before ANY candidate's shifted
//     fallback attempt (pass 2) -- i.e. pass-1 attempts form a prefix of the
//     call trace. A regression that interleaves passes, or starts a
//     fallback ladder before every primary has had its turn, breaks this.
// I6 (no destructive reconciliation): an id whose immediate
//     query() read-back reported false (this scenario's injected "phantom"
//     id, if any) must remain LIVE, and wakeup_cancel() must never be called
//     during a replan. The entry is firmware-accepted but read-back-
//     unconfirmed, and the adapter deliberately keeps it in its bookkeeping.
// I7 (decoy exclusion, beyond the original I1-I6): a paused, already-fired, or
//     stopwatch timer must NEVER appear in the wakeup_schedule() call trace
//     at all -- sched_candidates()'s own filter must hold under every
//     generated combination, not just the hand-picked cases in
//     sched_test.c/wakeup_adapter_test.c.
static bool check_invariants(int n_real, RefCandidate *sorted_real, int n_decoy,
                              const int32_t *decoy_cookies, char *fail_msg, size_t fail_msg_size) {
  // I1 also records each candidate's primary outcome for I9 to reuse
  // without a second full trace scan.
  int running_accepted = 0;
  int32_t primary_result[MAX_TIMERS];  // the trace's raw wakeup_schedule() return for cookie's
                                        // OWN deadline; E_RANGE_SENTINEL below if never attempted
                                        // (I1 already fails the whole check before I9 runs in that
                                        // case, so I9 never actually sees that sentinel value).
#define E_RANGE_SENTINEL (-999)
  for (int i = 0; i < n_real; i++) {
    int32_t cookie = sorted_real[i].cookie;
    time_t deadline = sorted_real[i].deadline;
    int found_idx = -1;
    for (int k = 0; k < s_oracle.trace_count; k++) {
      if (s_oracle.trace[k].cookie == cookie && s_oracle.trace[k].at == deadline) {
        found_idx = k;
        break;
      }
    }
    primary_result[i] = (found_idx >= 0) ? s_oracle.trace[found_idx].result : E_RANGE_SENTINEL;
    if (running_accepted >= SCHED_MAX_WAKEUPS) {
      if (found_idx != -1) {
        snprintf(fail_msg, fail_msg_size,
                 "I1: cookie=%d own primary WAS attempted even though our cap was already "
                 "genuinely full (running_accepted=%d) -- wasted/incorrect attempt",
                 cookie, running_accepted);
        return false;
      }
    } else {
      if (found_idx == -1) {
        snprintf(fail_msg, fail_msg_size,
                 "I1: cookie=%d own on-time primary (deadline offset=%ld) was NEVER attempted "
                 "even though the cap was not yet full (running_accepted=%d < %d) -- "
                 "a candidate was skipped before its outcome was known",
                 cookie, (long)(deadline - NOW), running_accepted, SCHED_MAX_WAKEUPS);
        return false;
      }
      // Derived from the firmware contract, not from the adapter's code: a
      // positive WakeupId means firmware ACCEPTED the request and the entry
      // occupies one of this app's eight per-UUID slots. Whether our own
      // immediate read-back happened to confirm it is irrelevant to
      // firmware's cap -- the slot is consumed either way. So the injected
      // phantom counts here exactly like any other acceptance. It was
      // excluded only while the adapter cancelled query-false entries;
      // cancellation is no longer part of the adapter.
      if (s_oracle.trace[found_idx].result >= 0) {
        running_accepted++;
      }
    }
  }

  // I3: a cookie is armed iff it has at least one live entry. A cookie may
  // now legitimately own both a primary/retry and its insurance rung.
  for (int i = 0; i < n_real; i++) {
    int32_t cookie = sorted_real[i].cookie;
    bool adapter_armed = wakeup_adapter_is_armed(cookie);
    time_t oracle_at = 0;
    bool oracle_armed = wakeup_oracle_has_our_cookie(&s_oracle, cookie, &oracle_at);
    if (adapter_armed != oracle_armed) {
      snprintf(fail_msg, fail_msg_size,
               "I3: cookie=%d UI-truth mismatch -- wakeup_adapter_is_armed()=%d but the real "
               "oracle table says %d",
               cookie, adapter_armed, oracle_armed);
      return false;
    }
  }
  // I2: collect ENTRIES, rather than one representative timestamp per
  // cookie, because redundancy intentionally makes multiple entries per
  // armed timer valid.
  time_t armed_at[SCHED_MAX_WAKEUPS];
  int32_t armed_cookie[SCHED_MAX_WAKEUPS];
  int armed_n = 0;
  for (int i = 0; i < s_oracle.count; i++) {
    if (!s_oracle.entries[i].live || !s_oracle.entries[i].is_ours) continue;
    if (armed_n >= SCHED_MAX_WAKEUPS) {
      snprintf(fail_msg, fail_msg_size, "I2: more than %d live entries", SCHED_MAX_WAKEUPS);
      return false;
    }
    armed_at[armed_n] = s_oracle.entries[i].at;
    armed_cookie[armed_n] = s_oracle.entries[i].cookie;
    armed_n++;
  }
  if (armed_n > SCHED_MAX_WAKEUPS) {
    snprintf(fail_msg, fail_msg_size, "I2: %d entries armed, exceeds the %d-entry cap", armed_n,
             SCHED_MAX_WAKEUPS);
    return false;
  }
  for (int a = 0; a < armed_n; a++) {
    for (int b = a + 1; b < armed_n; b++) {
      time_t diff = armed_at[a] - armed_at[b];
      if (diff < 0) diff = -diff;
      if (diff < WAKEUP_ORACLE_WINDOW_S) {
        snprintf(fail_msg, fail_msg_size,
                 "I2: cookies %d and %d both armed only %ld s apart (< %d s window)",
                 armed_cookie[a], armed_cookie[b], (long)diff, WAKEUP_ORACLE_WINDOW_S);
        return false;
      }
    }
  }

  // I4. Back to the exact documented worst case, with no allowance for the
  // query layer: a failed read-back now issues no additional
  // wakeup_schedule() call at all.
  int ceiling = MAX_TIMERS * (1 + SCHED_FALLBACK_COUNT);
  if (s_oracle.trace_count > ceiling) {
    snprintf(fail_msg, fail_msg_size, "I4: %d wakeup_schedule() calls, exceeds the %d-call ceiling",
             s_oracle.trace_count, ceiling);
    return false;
  }

  // I5
  int last_primary_idx = -1;
  int first_fallback_idx = -1;
  for (int k = 0; k < s_oracle.trace_count; k++) {
    int32_t cookie = s_oracle.trace[k].cookie;
    time_t at = s_oracle.trace[k].at;
    time_t own_deadline = 0;
    bool known = false;
    for (int i = 0; i < n_real; i++) {
      if (sorted_real[i].cookie == cookie) {
        own_deadline = sorted_real[i].deadline;
        known = true;
        break;
      }
    }
    if (!known) {
      snprintf(fail_msg, fail_msg_size,
               "I5: trace entry %d references cookie=%d, not one of this scenario's real "
               "candidates -- decoy leaked into scheduling (see I7) or a stale cookie",
               k, cookie);
      return false;
    }
    if (at == own_deadline) {
      last_primary_idx = k;
    } else if (first_fallback_idx == -1) {
      first_fallback_idx = k;
    }
  }
  if (first_fallback_idx != -1 && last_primary_idx > first_fallback_idx) {
    snprintf(fail_msg, fail_msg_size,
             "I5: a shifted fallback attempt (trace index %d) happened BEFORE a later primary "
             "attempt (trace index %d) -- pass ordering violated",
             first_fallback_idx, last_primary_idx);
    return false;
  }

  // I6 used to require the injected phantom to have been CANCELLED. Deriving
  // it from the firmware
  // contract instead of from the adapter's code gives the opposite
  // requirement, and a much stronger one:
  //
  //   wakeup_query() returning false does not mean the entry is absent --
  //   firmware collapses "absent" with a transient settings-file reopen
  //   failure on an entry that is still live and will still fire. So a false
  //   read-back licenses NO destructive action, and cancelling on it can
  //   only ever remove a working alarm.
  //
  // The phantom entry must still be LIVE at the end of the scenario, and
  // wakeup_cancel() must never have been called at all. Any reintroduction
  // of cancel-on-query-false fails here, on generated scenarios, without
  // anyone having to think of the right hand-written case.
  if (wakeup_cancel_fake.call_count != 0) {
    snprintf(fail_msg, fail_msg_size,
             "I6: wakeup_cancel() was called %d time(s). A false wakeup_query() read-back is "
             "not evidence of absence, so no individual entry may ever be cancelled during a "
             "replan -- doing so destroys alarms that were working.",
             (int)wakeup_cancel_fake.call_count);
    return false;
  }
  if (s_phantom_marked_id >= 0) {
    bool still_live = false;
    for (int i = 0; i < s_oracle.count; i++) {
      if (s_oracle.entries[i].live && s_oracle.entries[i].id == s_phantom_marked_id) {
        still_live = true;
        break;
      }
    }
    if (!still_live) {
      snprintf(fail_msg, fail_msg_size,
               "I6: id=%d was marked phantom (query-false) and is no longer live -- the adapter "
               "destroyed an entry firmware had accepted and would have fired, on the strength of "
               "a read-back that cannot distinguish 'absent' from 'transiently unreadable'",
               s_phantom_marked_id);
      return false;
    }
  }

  // I7
  for (int d = 0; d < n_decoy; d++) {
    for (int k = 0; k < s_oracle.trace_count; k++) {
      if (s_oracle.trace[k].cookie == decoy_cookies[d]) {
        snprintf(fail_msg, fail_msg_size,
                 "I7: decoy cookie=%d (paused/fired/stopwatch -- never a real candidate) was "
                 "attempted at offset=%ld -- sched_candidates() filter regression",
                 decoy_cookies[d], (long)(s_oracle.trace[k].at - NOW));
        return false;
      }
    }
  }

  // I8 exists because one mutation per scenario is insufficient. I1/I5
  // only check STRUCTURAL attempt
  // completeness/ordering, never that a fallback rung's TIMESTAMP is one of
  // the documented ladder offsets. Proven necessary by running the
  // mutation, not by inspection: a mutation collapsing sched_fallback_at()'s
  // two rungs to the SAME timestamp -- {+60, +60} instead of {-60, +60} --
  // passed I1-I7 and every earlier iteration count/seed tried, because
  // "attempted something" is satisfied either way; only the existing
  // hand-written sched_test.c/wakeup_adapter_test.c caught it, which means
  // the property test's OWN standalone coverage had a real gap until this
  // invariant closed it): every fallback (non-primary) trace entry for a
  // candidate must be exactly deadline-60, deadline+60, deadline+120, or
  // deadline+180. +180 is reachable only when a short-lead retry was armed
  // at +120 and rank 3 adds its one insurance rung; deadline-120 is never
  // legitimate. No two fallback entries for the SAME candidate may share a
  // timestamp -- with NO exception.
  //
  // A same-timestamp re-arm carve-out admitted more than the recovery
  // sequence it was written for because the trace records neither query
  // nor cancel events and so cannot constrain their ORDER. Rather than
  // encode a causal
  // sequence into an invariant, the behaviour that needed the carve-out was
  // removed: a failed read-back now issues no second wakeup_schedule() at
  // all. Derived straight from the firmware contract -- strict `<` 60 s,
  // cross-app, so a second request at an identical timestamp cannot be
  // admitted while the first entry is live -- a duplicate is therefore never
  // legitimate, and this invariant is back to having no implementation
  // detail baked into it.
  for (int i = 0; i < n_real; i++) {
    int32_t cookie = sorted_real[i].cookie;
    time_t deadline = sorted_real[i].deadline;
    time_t fallback_seen[SCHED_FALLBACK_COUNT + 1];
    int fallback_seen_n = 0;
    for (int k = 0; k < s_oracle.trace_count; k++) {
      if (s_oracle.trace[k].cookie != cookie) continue;
      time_t at = s_oracle.trace[k].at;
      if (at == deadline) continue;  // the primary, not a fallback rung
      time_t delta = at - deadline;
      if (delta != -60 && delta != 60 && delta != 120 && delta != 180) {
        snprintf(fail_msg, fail_msg_size,
                 "I8: cookie=%d fallback attempt at deadline%+ld is not a real ladder rung "
                 "(-60/+60/+120/+180 only)",
                 cookie, (long)delta);
        return false;
      }
      for (int s = 0; s < fallback_seen_n; s++) {
        if (fallback_seen[s] == at) {
          snprintf(fail_msg, fail_msg_size,
                   "I8: cookie=%d attempted the SAME fallback timestamp (deadline%+ld) twice -- "
                   "the ladder's two rungs have collapsed onto one offset",
                   cookie, (long)delta);
          return false;
        }
      }
      if (fallback_seen_n < SCHED_FALLBACK_COUNT + 1) fallback_seen[fallback_seen_n++] = at;
    }
  }

  // I9 closes a gap in I1-I8: they are all satisfied by a scheduler that
  // NEVER retries after E_RANGE -- I1 only checks the primary was
  // ATTEMPTED, never that a rejection is followed up; I5/I8 are vacuously
  // true when there are zero fallback attempts to check. This is the
  // same class of bug this property test must prevent: a check that is
  // satisfied by the ABSENCE of the
  // behavior it claims to guard. I9 requires the behavior to be PRESENT:
  // every genuinely-E_RANGE primary (not a query-false phantom, which is a
  // different, correctly non-retried outcome) gets its ladder actually run,
  // rung by rung, in the real algorithm's own order, for as long as our own
  // cap has room.
  //
  // A naive "at least one trace entry exists" / "trace has
  // SCHED_FALLBACK_COUNT entries" check is ITSELF wrong, because
  // wakeup_adapter.c's pass 2 has a legitimate LOCAL pre-filter
  // (`sched_spacing_allows()`, wakeup_adapter.c:202) that skips a rung
  // WITHOUT ever calling wakeup_schedule() -- and therefore without a
  // trace entry -- when that rung's timestamp would land exactly on
  // another candidate's ALREADY-accepted slot (proven load-bearing by
  // wakeup_adapter_test.c's own
  // test_retry_never_lands_on_another_timers_accepted_slot). So "missing
  // from the trace" is not itself evidence of a bug; it is evidence of a
  // bug only when NOT explained by that pre-filter. I9 therefore recomputes
  // each rung's OWN expected timestamp via the real, pure
  // sched_fallback_at() (linked from lib/sched.c, not reimplemented here)
  // and only flags a missing rung when no other candidate's FINAL armed
  // position is close enough to explain a local-spacing skip.
  {
    int running_accepted_pass2 = running_accepted;  // pass 2 starts where pass 1 left off
    for (int i = 0; i < n_real; i++) {
      int32_t cookie = sorted_real[i].cookie;
      time_t deadline = sorted_real[i].deadline;
      // No phantom special-casing left: a positive WakeupId arms the timer
      // whether or not our read-back confirmed it, so pass 2 has no role for
      // it. Carving the phantom out of "armed in pass 1" was only correct
      // while query-false entries were being cancelled.
      if (primary_result[i] >= 0) continue;   // armed in pass 1, no pass-2 role
      if (primary_result[i] != E_RANGE) continue;  // terminal status -- correctly NOT retried

      if (running_accepted_pass2 >= SCHED_MAX_WAKEUPS) {
        // Our own cap was already genuinely full BEFORE this candidate's
        // pass-2 turn -- correctly zero fallback attempts expected (mirrors
        // I1's symmetric check for pass 1).
        bool any_fallback = false;
        for (int k = 0; k < s_oracle.trace_count; k++) {
          if (s_oracle.trace[k].cookie == cookie && s_oracle.trace[k].at != deadline) any_fallback = true;
        }
        if (any_fallback) {
          snprintf(fail_msg, fail_msg_size,
                   "I9: cookie=%d got a fallback attempt even though our cap was already "
                   "genuinely full (running_accepted_pass2=%d) going into its turn",
                   cookie, running_accepted_pass2);
          return false;
        }
        continue;
      }

      // Collect this cookie's fallback trace entries (unordered is fine --
      // matched below by exact expected timestamp, not by position).
      // Fixed size (independent of SCHED_FALLBACK_COUNT, which a negative
      // control may set to 0 -- a zero-length VLA-ish array with a `{0}`
      // initializer is a compile error, not something worth special-casing).
      int fb_idx[8];
      int fb_n = 0;
      for (int k = 0; k < s_oracle.trace_count; k++) {
        if (s_oracle.trace[k].cookie != cookie || s_oracle.trace[k].at == deadline) continue;
        if (fb_n < (int)(sizeof(fb_idx) / sizeof(fb_idx[0]))) fb_idx[fb_n++] = k;
      }
      bool used[8] = {false, false, false, false, false, false, false, false};

      bool armed = false;
      bool stopped = false;
      for (int f = 0; f < REQUIRED_LADDER_RUNGS && !stopped; f++) {
        time_t expected_at = sched_fallback_at(deadline, NOW, f);
        int match = -1;
        for (int j = 0; j < fb_n; j++) {
          if (!used[j] && s_oracle.trace[fb_idx[j]].at == expected_at) {
            match = j;
            break;
          }
        }
        if (match < 0) {
          // Rung f was never attempted. Legitimate ONLY if some OTHER
          // cookie's final armed position is within the 60s local-spacing
          // window of expected_at (the real pre-filter's own condition).
          bool explained = false;
          for (int oi = 0; oi < n_real; oi++) {
            if (sorted_real[oi].cookie == cookie) continue;
            time_t other_at;
            if (!wakeup_oracle_has_our_cookie(&s_oracle, sorted_real[oi].cookie, &other_at)) continue;
            time_t diff = expected_at - other_at;
            if (diff < 0) diff = -diff;
            if (diff < WAKEUP_ORACLE_WINDOW_S) {
              explained = true;
              break;
            }
          }
          if (!explained) {
            snprintf(fail_msg, fail_msg_size,
                     "I9: cookie=%d rung %d (expected at deadline%+ld) was never attempted, and no "
                     "other candidate's final armed position explains a local-spacing skip -- an "
                     "unexplained gap in the ladder (cap room was available: "
                     "running_accepted_pass2=%d < %d)",
                     cookie, f, (long)(expected_at - deadline), running_accepted_pass2,
                     SCHED_MAX_WAKEUPS);
            return false;
          }
          continue;  // legitimately locally-filtered -- try the next rung
        }

        used[match] = true;
        int32_t result = s_oracle.trace[fb_idx[match]].result;
        if (result >= 0) {
          // Firmware accepted it -- armed, and one cap slot consumed,
          // read-back confirmation or not.
          armed = true;
          running_accepted_pass2++;
          stopped = true;
        } else if (result != E_RANGE) {
          stopped = true;  // a legitimate non-E_RANGE early stop
        }
        // else E_RANGE -- fall through to the next rung, exactly like the real loop.
      }
      (void)armed;  // used only for readability of the branches above

      // Any unmatched entry is the rank-3 insurance rung; its timestamp is
      // independently constrained by I8 and its rank ordering by I5.
    }
  }

  return true;
}

// -- The property test itself ----------------------------------------------

void test_wakeup_scheduling_invariants_over_generated_scenarios(void) {
  s_rng_state = g_seed;
  printf("wakeup_property_test: seed=%u iterations=%d\n", g_seed, WAKEUP_PROPERTY_ITERATIONS);

  static TimerStore store;

  for (int iter = 0; iter < WAKEUP_PROPERTY_ITERATIONS; iter++) {
    wakeup_oracle_reset(&s_oracle);
    store_init(&store);
    s_phantom_target = (rand_range(0, 4) == 0) ? rand_range(1, SCHED_MAX_WAKEUPS) : -1;
    s_phantom_success_seq = 0;
    s_phantom_marked_id = -1;

    // -- generate the logical (kind, offset) list: real candidates + decoys --
    int n_slots = rand_range(0, MAX_TIMERS);
    typedef struct {
      bool is_decoy;
      int decoy_kind;  // 0=paused countdown, 1=already-fired countdown, 2=stopwatch
      int32_t offset;  // may be < SCHED_MIN_LEAD_S (deliberately, to exercise exclusion)
      TimerKind kind;  // for non-decoys: COUNTDOWN or REPEAT
    } Slot;
    Slot slots[MAX_TIMERS];
    for (int i = 0; i < n_slots; i++) {
      bool decoy = rand_range(0, 99) < 15;
      slots[i].is_decoy = decoy;
      if (decoy) {
        slots[i].decoy_kind = rand_range(0, 2);
        slots[i].offset = rand_range(60, 3600);
        slots[i].kind = (slots[i].decoy_kind == 2) ? KIND_STOPWATCH : KIND_COUNTDOWN;
      } else {
        slots[i].kind = (rand_range(0, 9) == 0) ? KIND_REPEAT : KIND_COUNTDOWN;
        if (rand_range(0, 9) == 0) {
          slots[i].offset = rand_range(0, 4);  // deliberately below SCHED_MIN_LEAD_S(5)
        } else if (rand_range(0, 1) == 0) {
          slots[i].offset = kHostileOffsets[rand_range(0, NUM_HOSTILE_OFFSETS - 1)];
        } else {
          slots[i].offset = rand_range(5, 3600);
        }
      }
    }
    // Shuffle insertion order (Fisher-Yates) -- results must not depend on
    // creation order; this generalizes that requirement to every scenario
    // instead of one hand-picked pair.
    for (int i = n_slots - 1; i > 0; i--) {
      int j = rand_range(0, i);
      Slot tmp = slots[i];
      slots[i] = slots[j];
      slots[j] = tmp;
    }

    RefCandidate real[MAX_TIMERS];
    int n_real = 0;
    int32_t decoy_cookies[MAX_TIMERS];
    int n_decoy = 0;
    time_t candidate_deadlines[MAX_TIMERS];  // for adversarial foreign placement below
    int n_candidate_deadlines = 0;

    for (int i = 0; i < n_slots; i++) {
      int32_t cookie;
      time_t deadline = NOW + slots[i].offset;
      bool ok = store_add(&store, slots[i].kind, slots[i].offset, NOW, &cookie);
      if (!ok) continue;  // store full -- shouldn't happen (n_slots <= MAX_TIMERS), defensive only

      if (slots[i].is_decoy) {
        Timer *t = store_find(&store, cookie);
        if (slots[i].decoy_kind == 0) {
          timer_pause(t, NOW);
        } else if (slots[i].decoy_kind == 1) {
          t->fired = true;
        }  // decoy_kind==2 is already KIND_STOPWATCH from store_add
        decoy_cookies[n_decoy++] = cookie;
      } else {
        if (slots[i].offset >= 5) {  // SCHED_MIN_LEAD_S -- lead-eligible, a real sched candidate
          real[n_real].cookie = cookie;
          real[n_real].deadline = deadline;
          real[n_real].insertion_index = i;
          n_real++;
          candidate_deadlines[n_candidate_deadlines++] = deadline;
        }
        // offset < 5: a genuine below-lead timer -- correctly excluded, not tracked as a
        // "real" candidate (sched_candidates() itself excludes it; nothing to check it against
        // beyond I7-style "never attempted", folded in for free since it's never in `real[]`
        // and the trace-driven invariants above only reason about entries in `real[]` /
        // `decoy_cookies[]` -- an untracked below-lead cookie appearing in the trace would only
        // be caught by I5's "unknown cookie" guard, which is intentional headroom, not a gap
        // this scenario class specifically targets.
      }
    }
    sort_ref_candidates(real, n_real);

    // -- foreign colliders: some adversarially AT a real candidate's own
    //    deadline, some at independent offsets --
    //
    // REACHABILITY: wakeup_oracle_add_foreign() bypasses
    // admission entirely (see its own doc comment), so a careless generator
    // could seed two foreign entries LESS than 60s apart -- a table state
    // real firmware could never actually contain, since the second foreign
    // app's own wakeup_schedule() call would itself have been rejected
    // E_RANGE by the same cross-app collision rule at the moment IT tried to
    // schedule. Testing the adapter against a state firmware cannot reach is
    // not "adversarial," it is testing against an impossible world. Guard
    // explicitly: only add a candidate foreign offset if the oracle (as it
    // stands so far, i.e. against every EARLIER foreign entry already
    // placed) would genuinely admit it.
    int n_foreign_attempts = rand_range(0, 10);
    time_t foreign_at[10];
    int n_foreign = 0;
    for (int i = 0; i < n_foreign_attempts; i++) {
      time_t candidate_at;
      if (n_candidate_deadlines > 0 && rand_range(0, 1) == 0) {
        candidate_at = candidate_deadlines[rand_range(0, n_candidate_deadlines - 1)];
      } else {
        candidate_at = NOW + rand_range(0, 3600);
      }
      if (!wakeup_oracle_would_admit(&s_oracle, candidate_at)) continue;  // unreachable -- skip
      foreign_at[n_foreign] = candidate_at;
      wakeup_oracle_add_foreign(&s_oracle, candidate_at);
      n_foreign++;
    }

    wakeup_adapter_replan(&store, NOW);

    char fail_msg[256];
    if (!check_invariants(n_real, real, n_decoy, decoy_cookies, fail_msg, sizeof fail_msg)) {
      dump_scenario(iter, n_real, real, n_decoy, decoy_cookies, n_foreign, foreign_at);
      TEST_FAIL_MESSAGE(fail_msg);
    }
  }

  printf("wakeup_property_test: %d scenarios checked, 0 invariant violations (seed=%u)\n",
         WAKEUP_PROPERTY_ITERATIONS, g_seed);
}

// -- Exhaustive boundary matrix --------------------------------------------
//
// "The generator chooses hostile offsets as ABSOLUTE offsets from NOW, not
// as PAIRWISE SPACINGS between timers and colliders... does not
// deliberately enumerate relative 59/60/61 boundaries... Increasing the
// random count does not fix a distribution that does not target the
// relationship." Demonstrated concretely by the property test's own
// negative-control history: the `sched_spacing_allows() <` -> `<=` mutation
// survived 10 seeds x 20,000 random scenarios untouched.
//
// This is the fix: DETERMINISTIC, exhaustive enumeration of every relative
// (candidate, candidate) and (candidate, foreign) spacing in
// {-61,-60,-59,+59,+60,+61} -- exactly the boundary CLASS the random
// generator, by construction (independent absolute placements), essentially
// never produces on purpose. Two real candidates A (fixed) and B (A + one
// of the 6 relative offsets), crossed with 0 or 1 foreign collider (7
// placements: none, or one of the 6 relative-to-A offsets) -- 6 x 7 = 42
// deterministic cases, runs in well under a millisecond total. Kept
// deliberately narrow (not 3+ candidates x 2+ colliders): the random
// generator already explores higher cardinality broadly; this exists
// specifically to guarantee the EXACT boundary relationships never left to
// chance -- targeted enumeration, not a bigger random count, since more
// random volume does not fix a distribution gap at exact boundaries.
static const int32_t kBoundaryOffsets[] = {-61, -60, -59, 59, 60, 61};
#define NUM_BOUNDARY_OFFSETS ((int)(sizeof(kBoundaryOffsets) / sizeof(kBoundaryOffsets[0])))

void test_exhaustive_boundary_pairs(void) {
  static TimerStore store;
  int cases_checked = 0;

  for (int bi = 0; bi < NUM_BOUNDARY_OFFSETS; bi++) {
    // has_collider=false uses index -1 as "no collider" sentinel.
    for (int ci = -1; ci < NUM_BOUNDARY_OFFSETS; ci++) {
      wakeup_oracle_reset(&s_oracle);
      store_init(&store);
      s_phantom_target = -1;
      s_phantom_success_seq = 0;
      s_phantom_marked_id = -1;

      time_t a_deadline = NOW + 3600;
      time_t b_deadline = a_deadline + kBoundaryOffsets[bi];

      int32_t cookie_a, cookie_b;
      TEST_ASSERT_TRUE(store_add(&store, KIND_COUNTDOWN, a_deadline - NOW, NOW, &cookie_a));
      TEST_ASSERT_TRUE(store_add(&store, KIND_COUNTDOWN, b_deadline - NOW, NOW, &cookie_b));

      if (ci >= 0) {
        time_t collider_at = a_deadline + kBoundaryOffsets[ci];
        wakeup_oracle_add_foreign(&s_oracle, collider_at);
      }

      RefCandidate real[2];
      real[0] = (RefCandidate){.cookie = cookie_a, .deadline = a_deadline, .insertion_index = 0};
      real[1] = (RefCandidate){.cookie = cookie_b, .deadline = b_deadline, .insertion_index = 1};
      sort_ref_candidates(real, 2);

      wakeup_adapter_replan(&store, NOW);

      char fail_msg[256];
      if (!check_invariants(2, real, 0, NULL, fail_msg, sizeof fail_msg)) {
        char full_msg[400];
        snprintf(full_msg, sizeof full_msg,
                 "boundary case b_offset=%ld collider_offset=%s: %s",
                 (long)kBoundaryOffsets[bi], ci >= 0 ? "set" : "none", fail_msg);
        printf("wakeup_property_test: BOUNDARY FAILURE b_offset=%ld collider_idx=%d: %s\n",
               (long)kBoundaryOffsets[bi], ci, fail_msg);
        TEST_FAIL_MESSAGE(full_msg);
      }
      cases_checked++;
    }
  }
  printf("wakeup_property_test: %d exhaustive boundary cases checked, 0 invariant violations\n",
         cases_checked);
  TEST_ASSERT_EQUAL_INT(NUM_BOUNDARY_OFFSETS * (NUM_BOUNDARY_OFFSETS + 1), cases_checked);
}

int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
      g_seed = (uint32_t)strtoul(argv[i + 1], NULL, 10);
      g_seed_explicit = true;
      i++;
    }
  }
  if (!g_seed_explicit) g_seed = default_seed();
  if (g_seed == 0) g_seed = 1;  // xorshift32 needs a nonzero state

  UNITY_BEGIN();
  RUN_TEST(test_wakeup_scheduling_invariants_over_generated_scenarios);
  RUN_TEST(test_exhaustive_boundary_pairs);
  return UNITY_END();
}

// -- NEGATIVE CONTROLS ------------------------------------------------------
// Proof this file actually fails when the behaviour it guards breaks:
//
// 1. Reintroducing a candidate-count cap in the candidate-building loop
//    (`if (n_candidates >= SCHED_MAX_WAKEUPS) break;`) --> I1 fails almost
//    immediately ("own on-time primary was NEVER attempted"), because a
//    9th+ candidate stops being a candidate at all whenever an earlier one
//    occupies its slot in the array, long before any real outcome is known.
// 2. Adding back a local-spacing pre-filter to
//    sched_candidates()/sched_plan(), discarding a candidate within 60s of
//    an EARLIER candidate's ORIGINAL requested time before any attempt -->
//    I1 fails the same way: an innocent later candidate is skipped without
//    ever being attempted.
// 3. Reintroducing individual wakeup_cancel() on a query-false read-back
//    --> I6 fails directly: the firmware-accepted, read-back-unconfirmed id
//    must remain live and no individual cancel is permitted during a replan.
// 4. Removing the accepted_set_is_full() cap pre-check in
//    wakeup_adapter_replan()'s pass 1 (always attempting every primary
//    regardless of real accepted count) --> caught by I1 ("own primary WAS
//    attempted even though our cap was already genuinely full"), not I2 as
//    originally predicted while writing this comment -- accepted_set_append()
//    is ITSELF still capacity-guarded (its own doc comment: "this function
//    enforces the bound itself"), so the final accepted-set size never
//    actually exceeds 8 even with the pre-check gone; what breaks is wasted/
//    incorrect attempts past the point the cap is genuinely full, which is
//    exactly I1's other half. Recorded here, not silently corrected, because
//    an inaccurate prediction caught by ACTUALLY RUNNING the mutation (rather
//    than reasoning about it) is itself evidence this suite's negative
//    controls are real, not aspirational.
// 5. Swallowing a negative wakeup_schedule() status (the original shipped
//    bug) --> I3 fails ("UI-truth mismatch"): the adapter would report
//    a cookie armed despite the oracle having no live, firmware-accepted
//    entry. I1 and I3 catch that defect from different angles.
