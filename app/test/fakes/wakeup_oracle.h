// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// A host-side ADMISSION MODEL of PebbleOS's wakeup-service admission rule,
// not a general firmware oracle. wakeup_property_test.c drives
// src/c/wakeup_adapter.c against this model through the FFF custom fakes in
// test/fakes/pebble.h instead of hand-written per-scenario return values.
// See "CONFORMANCE PIN" below for the model's precise scope.
//
// The rule modeled here is EMPIRICALLY PINNED, not assumed. All five claims
// below passed against the SDK's bundled emery (Pebble Time 2) QEMU
// EMULATOR, firmware 4.17; this is not physical-hardware evidence:
//   - collision is a strict `<` on both sides of a 60s window, exactly ±60
//     legal, ±59 rejected (WAKEUP_EVENT_WINDOW=60).
//   - collision is CROSS-APP: any entry in the shared table, regardless of
//     owning UUID, blocks a nearby request.
//   - the 8-entry cap is PER-UUID (per calling app), independent of how many
//     entries OTHER apps hold live at the same time.
//   - wakeup_cancel_all() cancels only the CALLING app's own entries.
// Independently, PebbleOS v4.23.0 source
// (src/fw/services/wakeup/service.c:477-479 for the strict test, :503-504
// for the cap, :548-556 for UUID-scoped cancel-all; constants at
// include/pbl/services/wakeup.h:14-17) shows the SAME rule -- so this is
// both empirically confirmed (4.17, emulator) AND source-confirmed (4.23.0).
// Version 4.23.0 is the affected hardware's firmware version, though the
// rule has not YET been confirmed directly on that physical device.
//
// CONFORMANCE PIN: this model covers ONLY strict-<60s cross-app collision,
// the per-UUID 8-cap, and query/cancel/cancel-all semantics. It deliberately
// does NOT model past/now rejection, settings-file open/write failures,
// WakeupId allocation behavior, status precedence when two rejection
// reasons could both apply (see wakeup_oracle_schedule()'s own doc),
// significant clock-change rewriting, or delivery/consumption. Re-verify
// this pin by re-checking the admission claims enumerated above against
// the current SDK/emulator firmware, or re-deriving them from source,
// whenever the targeted SDK/emulator firmware version changes, a real
// device's reported firmware version changes, or these scenarios become a
// shipping gate. A real 4.23.0 physical-device (or CloudPebble) run
// remains the one gap this pin cannot close locally.
//
// This model is only as good as that empirical pin. If a future
// real-firmware scenario ever disagrees
// with this oracle, the oracle is wrong, not the adapter -- say so loudly
// rather than "fixing" the adapter to match a possibly-stale model.
//
// Deliberately NOT keyed uniquely by cookie: a single cookie legitimately
// holding MULTIPLE live entries is a real near-term possibility (a
// redundant post-deadline fallback ladder -- T, T+60, T+120 all armed
// simultaneously for one timer). This
// oracle's entry table is a flat array with no cookie-uniqueness constraint
// baked in anywhere, specifically so it keeps modeling that world without a
// rewrite -- only wakeup_adapter.c's OWN current one-entry-per-cookie
// behavior (which this test suite verifies, not assumes) makes that the
// observed case today.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define WAKEUP_ORACLE_MAX_ENTRIES 64  // generous: our own <=8 + up to 8 foreign colliders +
                                       // headroom; the property test asserts well under this.
#define WAKEUP_ORACLE_WINDOW_S 60     // WAKEUP_EVENT_WINDOW, empirically pinned strict `<`.
#define WAKEUP_ORACLE_CAP 8           // per-UUID cap, empirically pinned.

typedef struct {
  time_t at;
  int32_t cookie;   // meaningful only when is_ours; foreign entries don't need one.
  int32_t id;       // the WakeupId handed back by wakeup_oracle_schedule().
  bool is_ours;
  bool live;         // false once cancelled -- slots are reused (compacted), not just marked.
  bool phantom;      // query() reports false for this entry even though
                      // it's still live/occupying a slot ("a transient E_INTERNAL settings-file
                      // reopen failure on an entry that in fact still exists" --
                      // wakeup_adapter.c's own try_schedule() doc comment). An explicit
                      // wakeup_oracle_cancel() removes the entry.
} WakeupOracleEntry;

typedef struct {
  WakeupOracleEntry entries[WAKEUP_ORACLE_MAX_ENTRIES];
  int count;
  int32_t next_id;

  // Compact trace of every wakeup_oracle_schedule() call this scenario made,
  // in call order -- the ground truth the property test's invariants read,
  // rather than re-deriving "what happened, in what order, with what
  // outcome" from FFF's own call-history arrays (which this oracle doesn't
  // depend on the exact vendored fff.h version's field set to expose).
  struct {
    time_t at;
    int32_t cookie;
    int32_t result;  // the id/status returned, exactly as wakeup_schedule() would.
  } trace[WAKEUP_ORACLE_MAX_ENTRIES];
  int trace_count;
} WakeupOracle;

void wakeup_oracle_reset(WakeupOracle *o);

// Seeds a foreign (not-ours) live entry directly, bypassing admission
// checks entirely -- models another app's already-live wakeup-table entry,
// as produced by a second app's own prior, successful wakeup_schedule()
// call. The oracle doesn't need to re-derive that app's own admission path;
// the property test's generator places these at adversarial offsets
// directly.
//
// REACHABILITY WARNING: because this bypasses admission, calling it
// twice with two `at` values less than WAKEUP_ORACLE_WINDOW_S apart creates
// a table state REAL FIRMWARE COULD NEVER ACTUALLY REACH -- the second
// foreign app's own wakeup_schedule() call would itself have been rejected
// by the same cross-app collision rule at the moment it tried to schedule.
// This function does not defend against that itself (it would defeat the
// point of a fast, admission-bypassing seed); callers MUST check
// wakeup_oracle_would_admit() against the oracle's current state before
// calling this for each new foreign entry if the caller cares about
// reachability (wakeup_property_test.c's generator does exactly this).
// Testing an adapter against an unreachable table state is not adversarial,
// it is testing against an impossible world.
void wakeup_oracle_add_foreign(WakeupOracle *o, time_t at);

// Mimics wakeup_schedule(at, cookie, notify_if_missed): strict cross-app
// `|at - other.at| < WAKEUP_ORACLE_WINDOW_S` collision against EVERY live
// entry (ours + foreign) checked first; else the per-UUID cap
// (WAKEUP_ORACLE_CAP, ours-only) checked second -- see this header's own
// top comment for why the RELATIVE priority between these two checks, when
// both would independently reject, is a stated modeling assumption rather
// than an empirically confirmed fact. Each rejection was tested in
// isolation, never as a simultaneous-violation case. It does not
// affect this test suite's invariants either way: wakeup_adapter.c never
// attempts a call once ITS OWN accepted-count bookkeeping already knows the
// cap is full (see wakeup_adapter.c's `accepted_set_is_full` pre-check), so
// a live simultaneous-violation call from the adapter under test essentially
// never happens; this oracle still resolves the case deterministically
// (collision wins) so the generator/invariants never see an ambiguous
// result. Returns a positive id (and records a live "ours" entry + a trace
// row) on success, or a negative StatusCode (E_RANGE=-8 /
// E_OUT_OF_RESOURCES=-7, matching test/fakes/pebble.h's values) on
// rejection. Every call, success or failure, appends one trace row.
int32_t wakeup_oracle_schedule(WakeupOracle *o, time_t at, int32_t cookie);

// Mimics wakeup_query(id, &at): true (+ *out_at set) iff `id` is a live,
// non-phantom "ours" entry. A phantom entry (see WakeupOracleEntry's own
// doc) reports false here while REMAINING in the table (still occupying a
// cap slot, still able to collide with a later request) until an explicit
// wakeup_oracle_cancel() removes it -- modeling the real firmware behavior
// wakeup_adapter.c's own comments describe.
bool wakeup_oracle_query(const WakeupOracle *o, int32_t id, time_t *out_at);

// Mimics wakeup_cancel(id): removes that entry (ours or not, though the
// adapter only ever cancels its own ids) if live. No-op otherwise.
void wakeup_oracle_cancel(WakeupOracle *o, int32_t id);

// Mimics wakeup_cancel_all(): removes every LIVE "ours" entry. Foreign
// entries are untouched -- wakeup_cancel_all() only ever cancels the
// CALLING app's own entries; a real cross-app cancellation would be a
// serious platform bug and is deliberately not modeled.
void wakeup_oracle_cancel_all(WakeupOracle *o);

// Marks the given already-scheduled id as phantom (see WakeupOracleEntry's
// doc) -- called by the property test generator, at most once per scenario,
// on an id it just received back from wakeup_oracle_schedule(), to exercise
// wakeup_adapter.c's query-false handling inside the fuzzer rather than
// only in the hand-written wakeup_adapter_test.c cases.
void wakeup_oracle_mark_phantom(WakeupOracle *o, int32_t id);

// -- Introspection for invariant-checking (ground truth for I1-I6) --------

int wakeup_oracle_our_count(const WakeupOracle *o);

// True + *out_at set iff a LIVE "ours" entry for this cookie exists. Ours
// entries are never deduplicated by cookie (see this header's top comment),
// so if more than one existed this would report the FIRST found -- today's
// wakeup_adapter.c never creates more than one per cookie per replan, which
// I3 (this test's UI-truth invariant) is what actually verifies.
bool wakeup_oracle_has_our_cookie(const WakeupOracle *o, int32_t cookie, time_t *out_at);

// True iff no LIVE entry (ours or foreign) sits within WAKEUP_ORACLE_WINDOW_S
// of `at` -- i.e. whether firmware would accept a fresh request at `at`
// against the table exactly as it stands right now. Used by the property
// test to build ground truth for spacing/collision-integrity checks (I2).
bool wakeup_oracle_would_admit(const WakeupOracle *o, time_t at);
