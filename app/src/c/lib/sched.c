// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

#include "sched.h"

#include <string.h>

typedef struct {
  int32_t cookie;
  time_t at;
} Candidate;

// Stable insertion sort ascending by `at` -- MAX_TIMERS is small (16).
static void sort_candidates(Candidate *c, int n) {
  for (int i = 1; i < n; i++) {
    Candidate cur = c[i];
    int j = i - 1;
    while (j >= 0 && c[j].at > cur.at) {
      c[j + 1] = c[j];
      j--;
    }
    c[j + 1] = cur;
  }
}

bool sched_negative_rung_eligible(time_t requested_at, time_t now) {
  return (requested_at - SCHED_MIN_SPACING_S) >= now + SCHED_MIN_LEAD_S;
}

time_t sched_fallback_at(time_t requested_at, time_t now, int fallback_index) {
  if (sched_negative_rung_eligible(requested_at, now)) {
    return (fallback_index == 0) ? requested_at - SCHED_MIN_SPACING_S
                                  : requested_at + SCHED_MIN_SPACING_S;
  }
  // Forward-only substitute: {+60, +120}. fallback_index 0/1 -> *1/*2.
  return requested_at + (time_t)(fallback_index + 1) * SCHED_MIN_SPACING_S;
}

time_t sched_redundant_at(time_t armed_at, time_t known_failed_at) {
  time_t at = armed_at + SCHED_MIN_SPACING_S;
  if (at == known_failed_at) at += SCHED_MIN_SPACING_S;
  return at;
}

bool sched_spacing_allows(time_t at, const time_t *accepted, int accepted_count) {
  for (int i = 0; i < accepted_count; i++) {
    time_t diff = at - accepted[i];
    if (diff < 0) diff = -diff;
    if (diff < SCHED_MIN_SPACING_S) return false;
  }
  return true;
}

void sched_candidates(const TimerStore *store, time_t now, SchedCandidates *out) {
  memset(out, 0, sizeof(*out));

  Candidate candidates[MAX_TIMERS];
  int n_candidates = 0;
  time_t min_lead_boundary = now + SCHED_MIN_LEAD_S;

  for (int i = 0; i < store->count; i++) {
    const Timer *t = &store->timers[i];
    if (t->kind != KIND_COUNTDOWN && t->kind != KIND_REPEAT) continue;
    if (t->is_paused || t->fired) continue;

    time_t at = timer_deadline(t);
    if (at < min_lead_boundary) {
      out->excluded[out->excluded_count++] = t->cookie;
      continue;
    }
    candidates[n_candidates].cookie = t->cookie;
    candidates[n_candidates].at = at;
    n_candidates++;
  }

  sort_candidates(candidates, n_candidates);

  // No cap here at all -- every lead-eligible candidate is
  // preserved, up to MAX_TIMERS. Only the adapter, watching REAL
  // accepted-entry counts as it attempts them in this same deadline order,
  // can correctly decide when the firmware's 8-entry cap is genuinely full.
  for (int i = 0; i < n_candidates; i++) {
    out->candidates[out->candidate_count].cookie = candidates[i].cookie;
    out->candidates[out->candidate_count].at = candidates[i].at;
    out->candidate_count++;
  }
}
