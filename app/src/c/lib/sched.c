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

void sched_plan(const TimerStore *store, time_t now, SchedPlan *out) {
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
      out->unschedulable[out->unschedulable_count++] = t->cookie;
      continue;
    }
    candidates[n_candidates].cookie = t->cookie;
    candidates[n_candidates].at = at;
    n_candidates++;
  }

  sort_candidates(candidates, n_candidates);

  bool has_last_accepted = false;
  time_t last_accepted_at = 0;
  for (int i = 0; i < n_candidates; i++) {
    bool spacing_ok = !has_last_accepted || candidates[i].at >= last_accepted_at + SCHED_MIN_SPACING_S;
    bool cap_ok = out->request_count < SCHED_MAX_WAKEUPS;
    if (spacing_ok && cap_ok) {
      out->requests[out->request_count].cookie = candidates[i].cookie;
      out->requests[out->request_count].at = candidates[i].at;
      out->request_count++;
      last_accepted_at = candidates[i].at;
      has_last_accepted = true;
    } else {
      out->unschedulable[out->unschedulable_count++] = candidates[i].cookie;
    }
  }
}

bool sched_is_unschedulable(const SchedPlan *plan, int32_t cookie) {
  for (int i = 0; i < plan->unschedulable_count; i++) {
    if (plan->unschedulable[i] == cookie) return true;
  }
  return false;
}
