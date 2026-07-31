// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

#include "wakeup_adapter.h"

#include <pebble.h>
#include <stdio.h>

#include "lib/sched.h"
#include "testlog_am.h"

// -- Durable diagnostic of the last wakeup scheduling outcome ---------------
// A prior prototype for this was
// removed: it persisted the REPLAN timestamp instead of the actually
// attempted one, could record a rung that was never really attempted, never
// checked its own write results, and had no reader anywhere. The record
// below fixes those problems and can be read from a real watch.
//
// Keys 32/33 verified free before use: persist_adapter.h's timer manifest/
// slots use 0/1/2..17, persist_preset_adapter.h's PERSIST_KEY_PRESETS is 30,
// persist_kind_adapter.c's PERSIST_KEY_LAST_DIAL_KIND is 31 -- 32/33 are the
// first keys no adapter in this codebase has ever used.
#define PERSIST_KEY_WAKEUP_DIAG_DATA 32
#define PERSIST_KEY_WAKEUP_DIAG_COMMIT 33
// A schema tag, not a plain boolean -- a future incompatible record layout
// bumps this so an old-format leftover on an OTA'd device is never misread
// as the new one. Deliberately outside the range of any real WakeupId,
// documented StatusCode, or WAKEUP_QUERY_FAILED, none of which this constant
// could ever collide with (it is only ever read from/written to key 33,
// which holds nothing else).
#define WAKEUP_DIAG_COMMIT_MAGIC ((int32_t)0x57444731)  // "WDG1"

typedef struct {
  int32_t status;  // WakeupId (>=0), a documented negative StatusCode, or
                    // WAKEUP_QUERY_FAILED -- try_schedule()'s own *out_status
                    // encoding, reused as-is. No separate "query result"
                    // field is needed: WAKEUP_QUERY_FAILED alone already
                    // means "wakeup_schedule() succeeded, the immediate
                    // wakeup_query() read-back said otherwise."
  int32_t at;       // the timestamp THIS specific attempt requested -- never
                     // `now`/replan time (the removed prototype's defect).
} WakeupDiagRecord;

// Commits `rec` with an INVALIDATE-then-write-then-republish discipline.
//
// The obvious shape -- write payload, then publish marker -- is what this
// used to do, and its own comment claimed a failed/short payload write
// "leaves the PRIOR marker pointing at the PRIOR (still self-consistent)
// record." That claim is false because key 33 holds ONE
// permanent schema magic with no generation identity, so once any record
// has ever committed on this device the marker stays valid forever and
// vouches for whatever bytes key 32 currently holds -- including a
// half-old/half-new payload from a torn write. The reader would then log a
// mixed {status, at} pair as a real scheduling fact, which is worse than
// logging nothing: it sends whoever is holding the reporter's watch after
// a rung or a timestamp that was never attempted together.
//
// So: delete the marker FIRST. The window in which key 32 can be torn is
// now a window in which key 33 does not vouch for anything, and
// wakeup_diag_dump_previous()'s existing "no marker -> no record" branch
// rejects it. The cost is that a torn write loses the PRIOR record too
// (it is no longer readable) -- deliberate, and the right trade: an absent
// diagnostic is honest, a mixed one is actively misleading. Both the
// delete's and the republish's own results stay unchecked -- best-effort. A
// failed delete leaves the old marker over an about-to-be-overwritten payload
// (the
// pre-existing exposure, not a new one), and a failed republish leaves no
// marker, which reads as "no record" -- the safe direction.
//
// This is still not journal-grade atomicity: it converts "a torn write can
// publish a mixed record" into "a torn write publishes nothing," which is
// the property the comment above previously claimed and did not have.
// Regression: test_diag_torn_write_over_an_existing_record_publishes_nothing.
static void wakeup_diag_save(int32_t status, time_t at) {
  WakeupDiagRecord rec = {.status = status, .at = (int32_t)at};
  persist_delete(PERSIST_KEY_WAKEUP_DIAG_COMMIT);
  if (persist_write_data(PERSIST_KEY_WAKEUP_DIAG_DATA, &rec, sizeof rec) != (int)sizeof rec) return;
  persist_write_int(PERSIST_KEY_WAKEUP_DIAG_COMMIT, WAKEUP_DIAG_COMMIT_MAGIC);
}

// Reads back and surfaces the PREVIOUS process's record exactly once, before
// this process's own first replan can overwrite it. Rejects anything the
// commit marker doesn't vouch for -- absent, wrong magic, or a short/failed
// payload read -- rather than guessing. Otherwise a torn or future-version
// record would be returned as valid rather than unavailable.
//
// Readable off a REAL watch: APP_LOG goes out over the same Bluetooth
// developer-log channel `pebble logs` reads on real hardware -- pebble.h's
// own Logging module doc: "It is no longer necessary to enable app logging
// output from the settings->about menu... use the pebble logs command...
// to activate logs" -- no companion phone app or PebbleKit JS relay needed,
// just Developer Connection enabled in the Pebble/Rebble phone app. testlog()
// is ALSO emitted here, on the emulator-oriented AppMessage test-log channel,
// but that channel has no listener on an unpaired real watch, so it reaches
// no user on a normal watch. It is not this record's production reader;
// APP_LOG is.
static bool s_diag_dumped;

static void wakeup_diag_dump_previous(void) {
  if (s_diag_dumped) return;
  s_diag_dumped = true;

  if (!persist_exists(PERSIST_KEY_WAKEUP_DIAG_COMMIT) ||
      persist_read_int(PERSIST_KEY_WAKEUP_DIAG_COMMIT) != WAKEUP_DIAG_COMMIT_MAGIC) {
    return;  // no record yet, or one an earlier torn/partial write never completed
  }

  WakeupDiagRecord rec;
  if (persist_read_data(PERSIST_KEY_WAKEUP_DIAG_DATA, &rec, sizeof rec) != (int)sizeof rec) return;

  // The suffix is not decoration: WAKEUP_QUERY_FAILED (-100) means "armed,
  // read-back unconfirmed", NOT "failed to schedule", and this line is read
  // by a human holding a real watch who has no access to this source file.
  // A negative number read without its meaning manufactures a false alarm
  // during hardware verification, so the meaning travels with the number.
  APP_LOG(APP_LOG_LEVEL_INFO, "wakeup_diag last_status=%ld at=%ld%s", (long)rec.status,
          (long)rec.at, rec.status == WAKEUP_QUERY_FAILED ? " (armed, unconfirmed)" : "");
  char diag_data[24];
  snprintf(diag_data, sizeof diag_data, "%ld@%ld", (long)rec.status, (long)rec.at);
  testlog("wakeup:diag_prev", diag_data);
}

#ifdef WAKEUP_ADAPTER_TEST_HOOKS
void wakeup_adapter_test_reset_diag_dump(void) {
  s_diag_dumped = false;
}
#endif

// The outcome of the single most recent LOAD-BEARING wakeup attempt this
// process has made, across every replan so far -- captured inside
// try_schedule() below (the ONE choke point every primary/fallback/redundant
// attempt already goes through), so no call site needs its own bookkeeping.
// Persisted once at the end of a replan that made at least one real attempt
// (see wakeup_adapter_replan()).
//
// "Load-bearing" means rank 1 (primary) and rank 2 (the E_RANGE retry
// ladder) ONLY -- rank 3's optional insurance rung is deliberately excluded
// (`is_redundant`). Including it is incorrect because rank 3 is by
// construction the LAST try_schedule() call of every replan that armed
// anything, so with a foreign collider sitting after the deadline the
// sequence "primary E_RANGE -> retry arms T-60 (the alarm works) ->
// insurance rung hits the same collider at T+60" persisted
// {status=E_RANGE, at=T+60}. Whoever ran the physical-hardware
// verification would read `last_status=-8` off `pebble logs` and conclude
// the schedule failed when it had in fact succeeded -- a manufactured false
// alarm, in the single most likely real scenario, in the one artifact that
// is readable off a real watch.
//
// Why gating on `!is_redundant` is SUFFICIENT, not just necessary (checked
// rather than assumed): the only three call sites are pass 1
// (is_redundant=false), pass 2's ladder (is_redundant=false), and rank 3
// (is_redundant=true). Within one cookie, pass 2 only runs for a cookie
// that FAILED pass 1, and its ladder stops on the rung that succeeds, so
// the last non-redundant attempt for a cookie is that cookie's own final,
// true outcome. No non-redundant call can therefore overwrite an earlier
// SUCCESS for the same cookie with a later failure.
//
// Across MULTIPLE timers it remains whichever load-bearing attempt happened
// last in schedule order (deadline-ascending) -- a deliberate, documented
// simplification rather than a per-cookie diagnostic, to stay within budget.
// Deliberately NOT changed to "retain the confirmed armed timestamp
// whenever any rank succeeds": that would make
// a two-timer replan in which the second timer genuinely failed report the
// first one's success, hiding a real failure -- strictly worse than
// reporting the last real attempt.
// Regression: test_diag_ignores_a_failed_insurance_rung_after_a_good_primary.
static int32_t s_last_attempt_status;
static time_t s_last_attempt_at;
static bool s_had_attempt;

// The single write point for the statics above -- see their comment for why
// rank 3 is excluded.
static void record_attempt(int32_t status, time_t at, bool is_redundant) {
  if (is_redundant) return;
  s_last_attempt_status = status;
  s_last_attempt_at = at;
  s_had_attempt = true;
}

static void log_scheduled(int32_t cookie, time_t at, const char *tag) {
  char data[32];
  snprintf(data, sizeof data, "%ld@%ld", (long)cookie, (long)at);
  testlog(tag, data);
}

// One combined marker per real, attempted-and-failed outcome (including a
// candidate structurally skipped because our own 8-entry cap is already
// genuinely full -- see wakeup_adapter_replan()) -- every lead-eligible
// candidate gets at most one testlog line, never two. `status` is the final
// documented StatusCode (or WAKEUP_QUERY_FAILED) this cookie ended unarmed
// with.
static void log_unschedulable(int32_t cookie, int32_t status) {
  char data[24];
  snprintf(data, sizeof data, "%ld:%ld", (long)cookie, (long)status);
  testlog("wakeup:unschedulable", data);
}

// Below-min-lead candidates (SchedCandidates.excluded[]) are deliberately
// NOT testlog'd, individually or aggregated: they were never attempted at
// all (a hard, pre-known fact derivable from the TimerStore's own contents,
// not new runtime information) -- see lib/testlog.h's TESTLOG_QUEUE_CAPACITY
// comment for the full burst-sizing argument.

// The single source of truth for "how many wakeup entries firmware has
// accepted right now, and which cookies/timestamps they are."
//
// "Accepted" means wakeup_schedule() returned a positive WakeupId. Where
// the immediate wakeup_query() read-back also succeeded -- the overwhelming
// majority of entries -- that acceptance is additionally confirmed. Where it
// did not, the entry is still kept, because a false read-back is not
// evidence of absence (see try_schedule()). This set therefore holds
// firmware-accepted state, not read-back-confirmed state, and every consumer
// below is written against that weaker-but-honest meaning. Everything that
// needs to know the current armed state -- the local spacing check
// (sched_spacing_allows), the cap check (is it safe to attempt another?),
// and the UI-truth query (wakeup_adapter_is_armed()) -- reads the SAME object
// with a SINGLE count. accepted_set_append() is the ONLY function that ever
// writes into it, and is itself capacity-guarded, so no future call site (a
// new retry path or redundant-launch addition) can silently write past
// SCHED_MAX_WAKEUPS just by forgetting an external pre-check.
// This invariant regressed twice when callers had to check before appending,
// with two SEPARATELY incremented counters -- a local `accepted_count` and a
// global `s_armed_count` -- backing what was conceptually one fact.
typedef struct {
  time_t at[SCHED_MAX_WAKEUPS];
  int32_t cookie[SCHED_MAX_WAKEUPS];
  int count;
} AcceptedSet;

static void accepted_set_reset(AcceptedSet *set) {
  set->count = 0;
}

static bool accepted_set_is_full(const AcceptedSet *set) {
  return set->count >= SCHED_MAX_WAKEUPS;
}

// The ONLY place `count`/`at`/`cookie` are ever written. Returns false
// (no-op, does not write) if already full -- callers still check
// accepted_set_is_full() first to avoid a wasted wakeup_schedule() call,
// but correctness of the fixed-size arrays never depends on that check
// being present or correct: this function enforces the bound itself.
static bool accepted_set_append(AcceptedSet *set, int32_t cookie, time_t at) {
  if (accepted_set_is_full(set)) return false;
  set->at[set->count] = at;
  set->cookie[set->count] = cookie;
  set->count++;
  return true;
}

// The most recent replan's real, firmware-accepted armed set -- backs
// wakeup_adapter_is_armed(), the UI-truth query API. Reset at the start of
// every replan, then used as
// the SAME accepted set the replan itself schedules against -- there is no
// separate "local" copy: whatever this replan accepts IS the new armed
// set, so tracking it twice would just reintroduce the dual-counter problem
// this type exists to remove.
static AcceptedSet s_accepted;

#ifdef EMU_TEST_HOOKS
static WakeupId s_test_primary_id = E_INVALID_ARGUMENT;
#endif

bool wakeup_adapter_is_armed(int32_t cookie) {
  for (int i = 0; i < s_accepted.count; i++) {
    if (s_accepted.cookie[i] == cookie) return true;
  }
  return false;
}

// Schedules `at` for `cookie` and reads the result back. Returns true,
// and appends to `set`, whenever firmware ACCEPTED the request -- i.e.
// whenever wakeup_schedule() returned a positive WakeupId. Returns false only
// when wakeup_schedule() itself refused with a negative StatusCode.
//
// -- Why a failed read-back does not change what we do ----------------------
//
// `wakeup_query()` returns a bare boolean, and firmware collapses "entry
// absent" together with a transient `E_INTERNAL` settings-file-reopen
// failure on an entry that in fact still exists, still occupies a per-app
// slot, and can still launch the app. **A false read-back is therefore not
// evidence of absence.** Everything below follows from that one sentence.
//
// Cancelling the entry and re-arming at the same timestamp cannot safely
// establish ownership of the original id when the re-arm returns E_RANGE.
// That inference is **unavailable**: E_RANGE proves that SOME live entry
// sits within the 60 s
// window, and the API exposes neither the collider's id nor its owner. The
// schedule/query/cancel/re-arm sequence is not one atomic firmware
// transaction, so a *successful* cancel followed by any other producer
// taking the freed window yields the identical E_RANGE -- and we would then
// record an id that no longer exists.
//
// The decisive part of that counterexample is worth stating separately,
// because it condemns the CANCEL and not just the inference: with a foreign
// entry landing at `T+1` after our cancel took, our nominal insurance rung at
// `T+60` is 59 s away and is refused too, so the timer ends with ZERO
// entries. With no query layer at all, our original entry at `T` would have
// stayed live, would itself have REJECTED that foreign request, and its
// `T+60` insurance rung would have been exactly 60 s away and legal. So the
// cancel does not merely fail to help; it manufactures the very
// zero-entry outcome this code must prevent. That is true whether or not the
// E_RANGE admission is kept, which is why the cancel is gone entirely rather
// than merely un-relied-upon.
//
// What remains is the only response that needs no inference at all:
//
//   Rung 0  Re-read. A second wakeup_query(). Non-destructive, one syscall,
//           and it resolves the documented transient when it succeeds. A
//           second false read leaves the entry firmware-accepted but
//           read-back-unconfirmed; wakeup_adapter_is_armed() deliberately
//           remains true, while the diagnostic records the uncertainty.
//   else    Accept the entry on firmware's own positive WakeupId, exactly as
//           this adapter did before any query existed. Cancel nothing.
//           Schedule nothing extra. Record the un-confirmed read-back for
//           diagnostics only (WAKEUP_QUERY_FAILED, see wakeup_adapter.h).
//
// **The safety argument is now by construction rather than by case
// analysis.** On the query-false path this function performs exactly the
// same mutating firmware operations, in the same order, with the same
// accepted-set bookkeeping, as the pre-query code did with a positive id:
// one wakeup_schedule(), then append. There is no branch on which it can do
// less. The binding constraint
// -- never leave a timer with fewer firmware entries than it would have had
// with no query at all -- is therefore satisfied identically, not merely
// case-by-case, and no interleaving can change it. This construction avoids
// relying on a case table that could omit an interleaving.
//
// An indeterminate live entry left UNTRACKED makes this adapter's
// bookkeeping disagree with firmware about an entry firmware will act on.
// That harm is real, and it is closed here by TRACKING
// the entry -- the direct fix -- rather than by destroying it. Cancelling
// closed the disagreement by removing the alarm, which is strictly worse
// than closing it by remembering the alarm.
//
// Regression guards: test_query_false_never_cancels_anything_and_still_arms,
// test_a_transient_query_failure_never_ends_a_replan_with_zero_entries, and
// wakeup_property_test's I6 (which now requires the injected phantom to be
// still LIVE and wakeup_cancel() never to have been called at all).
static bool try_schedule(int32_t cookie, time_t at, bool is_redundant, AcceptedSet *set,
                         WakeupId *out_status) {
  WakeupId id = wakeup_schedule(at, cookie, true);
  if (id < 0) {
    record_attempt(id, at, is_redundant);
    *out_status = id;
    return false;
  }

  // Rung 0: re-read once before believing a false read-back (see above).
  time_t queried_at = 0;
  bool confirmed = wakeup_query(id, &queried_at) || wakeup_query(id, &queried_at);

  // Accepted either way -- firmware said yes. `confirmed` only decides how
  // this is DESCRIBED, never whether the entry is kept.
  const char *tag;
  if (is_redundant) {
    tag = "wakeup:fallback_armed";
  } else {
    tag = confirmed ? "wakeup:schedule" : "wakeup:schedule_unverified";
  }
  record_attempt(confirmed ? id : WAKEUP_QUERY_FAILED, at, is_redundant);
  *out_status = id;

  if (!accepted_set_append(set, cookie, at)) {
    // Unreachable in practice given the caller's own is_full() pre-check
    // below (real firmware never accepts a 9th entry for this app anyway),
    // but this is the actual enforcement point, not just documentation: if
    // a future caller ever DOES call this without checking first, the
    // over-capacity entry is silently not recorded rather than corrupting
    // the fixed-size arrays.
    return false;
  }

  log_scheduled(cookie, at, tag);
  return true;
}

static int accepted_set_find_cookie(const AcceptedSet *set, int32_t cookie, int limit) {
  for (int i = 0; i < limit; i++) {
    if (set->cookie[i] == cookie) return i;
  }
  return -1;
}

void wakeup_adapter_replan(const TimerStore *store, time_t now) {
  // Surfaces the PREVIOUS process's durable diagnostic exactly once, before
  // this replan's own work can overwrite it -- a no-op after the first call
  // in this process (see wakeup_diag_dump_previous()).
  wakeup_diag_dump_previous();

  SchedCandidates plan;
  sched_candidates(store, now, &plan);

  // Total, not incremental: every replan cancels EVERY previously-scheduled
  // wakeup and rebuilds the whole desired set from scratch, rather than
  // reconciling a delta against retained state. This is what makes a
  // fallback placement heal automatically -- a timer degraded to a shifted
  // rung by a foreign collider is retried at its own ideal deadline again on
  // the very next replan, once that collider clears, with no persisted
  // cookie<->WakeupId map, orphan IDs, or stale priority to reconcile.
  // "The very next replan" is load-bearing and is NOT time-driven: replans
  // happen on launch, on firing, and on a store mutation only -- there is no
  // periodic or exit replan and no background worker to host one. The full
  // statement of what that leaves open is in wakeup_adapter.h's
  // wakeup_adapter_replan() comment.
  // Regression: test_degraded_placement_heals_once_the_collider_clears
  // (test/wakeup_adapter_test.c) fails if this call is skipped when the
  // desired candidate set looks unchanged.
  wakeup_cancel_all();  // no cookie<->WakeupId map needed -- the plan is idempotent.

  accepted_set_reset(&s_accepted);
  s_had_attempt = false;
#ifdef EMU_TEST_HOOKS
  s_test_primary_id = E_INVALID_ARGUMENT;
#endif

  // Status bookkeeping is sized to MAX_TIMERS, deliberately SEPARATE from
  // the SCHED_MAX_WAKEUPS(8)-bounded `s_accepted`: there can be up to
  // MAX_TIMERS
  // real CANDIDATES (sched_candidates() no longer caps at 8), even though
  // at most 8 of them can ever end up ACCEPTED.
  WakeupId status[MAX_TIMERS];

  // Pass 1: attempt every lead-eligible candidate's own on-time deadline, in
  // ascending deadline order. A NEW primary attempt is only skipped once our
  // own 8-entry-per-app cap is GENUINELY full -- i.e. `s_accepted` (real,
  // firmware-accepted entries from THIS replan) has already reached
  // SCHED_MAX_WAKEUPS -- never based on how many CANDIDATES exist. This is
  // required because firmware's cap is on accepted entries, not on
  // attempts, so a schedulable 9th+ timer must still get a real try whenever
  // an earlier one failed and left a slot free.
  for (int i = 0; i < plan.candidate_count; i++) {
    if (accepted_set_is_full(&s_accepted)) {
      // Our own cap is already genuinely full (real accepted entries, not a
      // guess) -- any further attempt, primary or fallback, is certain to
      // return E_OUT_OF_RESOURCES. Record that fact without spending a call.
      status[i] = E_OUT_OF_RESOURCES;
      continue;
    }
#ifdef EMU_TEST_HOOKS
    bool armed = try_schedule(plan.candidates[i].cookie, plan.candidates[i].at, false, &s_accepted,
                              &status[i]);
    if (armed) s_test_primary_id = status[i];
#else
    try_schedule(plan.candidates[i].cookie, plan.candidates[i].at, false, &s_accepted, &status[i]);
#endif
  }

  // Pass 2: retry only the E_RANGE primaries, in the SAME deadline order (so
  // an earlier timer's retry still cannot preempt a LATER, not-yet-tried
  // timer's own on-time primary -- pass 1 above already gave every primary
  // priority). Re-check the real cap once per candidate before starting its
  // ladder -- nothing else in this candidate's OWN retry loop can change
  // `s_accepted` except a success that immediately stops that loop, so one
  // check per candidate (not per rung) is sufficient.
  for (int i = 0; i < plan.candidate_count; i++) {
    if (status[i] >= 0) continue;  // armed in pass 1
    if (status[i] != E_RANGE) {
      // "Cap already full" (from pass 1 above) and every documented
      // non-E_RANGE StatusCode (E_INVALID_ARGUMENT/E_OUT_OF_RESOURCES/
      // E_INTERNAL/unknown) -- none are retried HERE, because this pass
      // exists to move the TIMESTAMP and a timestamp shift addresses none of
      // those causes.
      //
      // WAKEUP_QUERY_FAILED can no longer reach this array at all. A failed
      // read-back is not evidence of absence (see try_schedule()), so it no
      // longer produces an unarmed outcome: firmware accepted the request,
      // `status[i]` carries the positive WakeupId, and this loop skips the
      // cookie as already-armed one branch above. The sentinel survives only
      // as a DIAGNOSTIC value. There is no longer a path on which a
      // read-back failure costs a timer its
      // entry.
      log_unschedulable(plan.candidates[i].cookie, status[i]);
      continue;
    }
    if (accepted_set_is_full(&s_accepted)) {
      status[i] = E_OUT_OF_RESOURCES;
      log_unschedulable(plan.candidates[i].cookie, status[i]);
      continue;
    }

    int32_t cookie = plan.candidates[i].cookie;
    time_t original_at = plan.candidates[i].at;
    bool armed = false;
    bool stopped = false;
    for (int f = 0; f < SCHED_FALLBACK_COUNT && !stopped; f++) {
      time_t at = sched_fallback_at(original_at, now, f);
      if (!sched_spacing_allows(at, s_accepted.at, s_accepted.count)) continue;

      WakeupId fstatus;
      if (try_schedule(cookie, at, false, &s_accepted, &fstatus)) {
        // Keep status[i] truthful on the ARMED path too. It used to be left
        // holding the stale E_RANGE from pass 1 for a
        // timer that is now armed. Nothing reads it after this loop today,
        // so this was inert -- but it is a trap for the next person who
        // adds a post-pass consumer, and one assignment removes it.
        status[i] = fstatus;
        armed = true;
        stopped = true;
      } else if (fstatus != E_RANGE) {
        // A non-E_RANGE refusal mid-ladder (E_INTERNAL, E_INVALID_ARGUMENT,
        // or an unrecognized code) stops the ladder distinctly, same as a
        // primary's own non-E_RANGE status. A failed read-back cannot land
        // here any more -- try_schedule() returns true for it.
        status[i] = fstatus;
        log_unschedulable(cookie, fstatus);
        stopped = true;
      }
      // else: fstatus == E_RANGE -- fall through to the next rung.
    }
    if (!armed && !stopped) {
      status[i] = E_RANGE;
      log_unschedulable(cookie, E_RANGE);
    }
  }

  // Rank 3: one insurance rung per timer that was armed by the primary or
  // retry ranks. `plan.candidates` remains the ordering authority here: the
  // accepted set is append-ordered, so a retry-recovered early deadline must
  // not sort behind later pass-1 successes. Snapshot the count before this
  // rank so a newly accepted insurance entry can never become a base for a
  // second rung. The same try_schedule()/AcceptedSet path is used for every
  // rank; only the pure-policy target and rank differ.
  int armed_before_redundancy = s_accepted.count;
  for (int i = 0; i < plan.candidate_count; i++) {
    if (accepted_set_is_full(&s_accepted)) break;

    int accepted_index = accepted_set_find_cookie(&s_accepted, plan.candidates[i].cookie,
                                                   armed_before_redundancy);
    if (accepted_index < 0) continue;

    time_t at = sched_redundant_at(s_accepted.at[accepted_index], plan.candidates[i].at);
    if (!sched_spacing_allows(at, s_accepted.at, s_accepted.count)) continue;

    WakeupId ignored_status;
    try_schedule(plan.candidates[i].cookie, at, true, &s_accepted, &ignored_status);
  }

  // Persist the diagnostic only when this replan actually attempted
  // something -- an empty-plan replan (nothing lead-eligible right now)
  // leaves the last real scheduling fact in place rather than erasing it,
  // matching the original plan's "do not overwrite on an empty replan" rule.
  if (s_had_attempt) {
    wakeup_diag_save(s_last_attempt_status, s_last_attempt_at);
  }
}

#ifdef EMU_TEST_HOOKS
bool wakeup_adapter_test_drop_primary(void) {
  if (s_test_primary_id < 0) return false;
  wakeup_cancel(s_test_primary_id);
  s_test_primary_id = E_INVALID_ARGUMENT;
  return true;
}
#endif

#ifdef GALLERY_MODE
void wakeup_adapter_gallery_reset_armed(void) {
  accepted_set_reset(&s_accepted);
}

void wakeup_adapter_gallery_seed_armed(int32_t cookie) {
  // `at` is never read by wakeup_adapter_is_armed() (cookie-only match), and
  // nothing else consults `s_accepted`'s timestamps in a gallery build --
  // wakeup_adapter_replan() (the only other reader) is never called here --
  // so 0 is fine. Silently no-ops past SCHED_MAX_WAKEUPS, same as a real
  // replan would (no gallery scene seeds anywhere near that many).
  accepted_set_append(&s_accepted, cookie, 0);
}
#endif
