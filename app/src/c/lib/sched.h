// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// Pure wakeup-scheduling policy: 8-cap + strictly-future single-wakeup min
// (firmware service.c:381); 60s inter-wakeup spacing (wakeup.h:14); app keeps
// a small SCHED_MIN_LEAD_S safety margin, not a hardware floor.
// NEVER #include <pebble.h>.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "store.h"

#define SCHED_MAX_WAKEUPS 8
#define SCHED_MIN_LEAD_S 5   // firmware floor is strictly >0 (service.c:381); 5s is a
                             // reliability margin over the schedule-time race + relaunch
                             // latency, NOT a hardware limit. Separate from the 60s
                             // spacing rule below.
#define SCHED_MIN_SPACING_S 60

typedef struct {
  int32_t cookie;
  time_t at;
} SchedRequest;

// Retry ladder for a primary request that firmware rejected with E_RANGE
// (another app's wakeup entry sits within 60s -- confirmed empirically on
// real stock emery firmware, strict `<`, exactly +/-60 legal). Two fallback
// attempts follow the original `requested_at`. The ladder is {-60, +60}
// when the negative rung still clears SCHED_MIN_LEAD_S over `now`, and the
// forward-only {+60, +120} otherwise (a short-lead primary, or a replan
// shortly after an early-launch wakeup whose own T-60 has already passed).
// Both ladders keep exactly 3 total attempts (1 original + 2 fallbacks) and
// clear any single foreign collider.
#define SCHED_FALLBACK_COUNT 2

// True when `requested_at - SCHED_MIN_SPACING_S` is still a real future
// wakeup (clears SCHED_MIN_LEAD_S over `now`) -- the gate for whether the
// negative-first ladder is eligible at all.
bool sched_negative_rung_eligible(time_t requested_at, time_t now);

// The `fallback_index`'th (0 or 1) retry deadline for `requested_at`, per
// the ladder selected by sched_negative_rung_eligible() above.
time_t sched_fallback_at(time_t requested_at, time_t now, int fallback_index);

// One post-deadline insurance rung for an accepted wakeup. Normally this is
// `armed_at + 60`; when that would repeat the primary timestamp that already
// returned E_RANGE, skip it and use the next legal rung instead. This keeps
// the policy pure and makes a retry recovered at T-60 useful insurance rather
// than a known-doomed repeat at T.
time_t sched_redundant_at(time_t armed_at, time_t known_failed_at);

// True if `at` is at least SCHED_MIN_SPACING_S away from every timestamp in
// `accepted[0..accepted_count)` -- the runtime local-spacing check the
// adapter applies before each shifted fallback attempt, so a retry for one
// timer can never land on another timer's already-accepted on-time slot.
bool sched_spacing_allows(time_t at, const time_t *accepted, int accepted_count);

// Every lead-eligible {COUNTDOWN,REPEAT}, non-paused, non-fired timer, up to
// MAX_TIMERS, sorted ascending by deadline. Deliberately applies NO local-
// spacing pre-filter and NO SCHED_MAX_WAKEUPS(8) cap of any kind: an
// earlier version of this function greedily discarded a later timer for
// being close to an EARLIER one's ORIGINAL timestamp (or once candidates
// exceeded 8), before anyone knew whether firmware would even accept the
// earlier primary -- both were the same class of bug (excluding before a
// real outcome is known), just at different layers. A candidate
// is only EVER structurally excluded here for lacking lead time (a hard,
// timing-only fact); the adapter alone decides, from real accepted-entry
// counts, when the 8-cap is genuinely full.
typedef struct {
  SchedRequest candidates[MAX_TIMERS];
  int candidate_count;
  int32_t excluded[MAX_TIMERS];  // below-min-lead only -- never attempted at all
  int excluded_count;
} SchedCandidates;

void sched_candidates(const TimerStore *store, time_t now, SchedCandidates *out);
