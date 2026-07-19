// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

#include "wakeup_adapter.h"

#include <pebble.h>
#include <stdio.h>

#include "lib/sched.h"
#include "testlog_am.h"

void wakeup_adapter_replan(const TimerStore *store, time_t now) {
  SchedPlan plan;
  sched_plan(store, now, &plan);

  wakeup_cancel_all();  // no cookie<->WakeupId map needed -- the plan is idempotent.

  for (int i = 0; i < plan.request_count; i++) {
    char data[32];
    WakeupId id = wakeup_schedule(plan.requests[i].at, plan.requests[i].cookie, true);
    if (id < 0) {
      // Pre-filtered by lib/sched, so this is a rare backstop (e.g. a
      // platform-level E_INTERNAL) -- C returns the negative code directly,
      // no try/catch needed.
      snprintf(data, sizeof data, "%ld:%d", (long)plan.requests[i].cookie, (int)id);
      testlog("wakeup:throw", data);
    } else {
      snprintf(data, sizeof data, "%ld@%ld", (long)plan.requests[i].cookie,
                (long)plan.requests[i].at);
      testlog("wakeup:schedule", data);
    }
  }
  for (int i = 0; i < plan.unschedulable_count; i++) {
    char data[16];
    snprintf(data, sizeof data, "%ld", (long)plan.unschedulable[i]);
    testlog("wakeup:unschedulable", data);
  }
}
