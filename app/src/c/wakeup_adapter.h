// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// lib/sched's plan <-> wakeup_cancel_all/wakeup_schedule (cancel-all +
// reschedule adapter pattern). Because
// the plan is always recomputed from the store, this is idempotent, needs no
// persisted cookie<->WakeupId map, and self-heals on relaunch.
#pragma once

#include <time.h>

#include "lib/store.h"

// Cancels every previously-scheduled wakeup, replans from `store`/`now`, and
// schedules the result. Emits a testLog marker per scheduled/unschedulable
// timer (wakeup:schedule / wakeup:unschedulable). Call on any
// schedule-affecting mutation and on background/exit.
void wakeup_adapter_replan(const TimerStore *store, time_t now);
