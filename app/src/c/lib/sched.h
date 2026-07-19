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

typedef struct {
  SchedRequest requests[SCHED_MAX_WAKEUPS];
  int request_count;
  int32_t unschedulable[MAX_TIMERS];
  int unschedulable_count;
} SchedPlan;

// Pure, deterministic algorithm:
//  1. candidates = timers with kind in {COUNTDOWN, REPEAT}, !is_paused, !fired; at = deadline(t)
//  2. drop at < now + SCHED_MIN_LEAD_S           -> unschedulable
//  3. sort candidates ascending by at
//  4. greedy in time order: accept if at >= last_accepted.at + SCHED_MIN_SPACING_S; else -> unschedulable
//  5. cap accepted at SCHED_MAX_WAKEUPS; overflow (latest)  -> unschedulable
void sched_plan(const TimerStore *store, time_t now, SchedPlan *out);

bool sched_is_unschedulable(const SchedPlan *plan, int32_t cookie);
