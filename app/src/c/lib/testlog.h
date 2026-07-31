// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// Pure test-log line formatting + a bounded no-drop-while-pending retry
// queue. NEVER #include <pebble.h> -- the actual AppMessage write (key
// 10000, matching harness/src/pebble_testpilot/emulator_session.py's
// TEST_LOG_APPMESSAGE_KEY) lives in the adapter, src/c/testlog_am.c.
#pragma once

#include <stdbool.h>
#include <stddef.h>

// Line length: sized against the longest test-log line this app has ever
// emitted (31 chars, "edit_interval_open:hero_up_hold") -- 60 keeps ~2x
// headroom, not the prior 4x-overprovisioned 120. Measured reclaim: -484 B
// `.bss` for this half alone.
//
// Queue capacity: read the derivation below before changing this number.
//
// A single wakeup_adapter_replan() call pushes lines SYNCHRONOUSLY (nothing
// pops until the first async AppMessage round-trip completes). The honest
// worst-case burst is 16 (one replan alone, up to MAX_TIMERS candidates,
// since sched_candidates()'s old 8-cap was removed) or 20 across a full
// production call chain (a cold wakeup launch that immediately fires --
// persist(1) + [replan+vibe lines, provably <= MAX_TIMERS combined] +
// alarm_target(<=2) + app_init(1)). MEASURED cost: 8->16 is +488 B; 8->20
// exceeds the hard uint16 wall outright (no .pbw).
//
// Capacity is 8. Overflow drops AppMessage test-log evidence only -- it
// never touches timer state, OS scheduling, persistence, vibration, or
// navigation (grep this codebase's `testlog(...)` call sites: none of their
// return values gate production behavior). Capacity 16 would still lose the
// full-chain/pre-existing-backlog case. A future diagnostics increment
// should aggregate per-replan outcomes or emit an explicit overflow summary
// rather than scaling a 61-byte-per-line queue toward a bound that still
// would not be complete.
#define TESTLOG_MAX_LINE_LEN 60
#define TESTLOG_QUEUE_CAPACITY 8

// Formats a test-log line into `out` (NUL-terminated, truncated to fit
// `out_size`): "tag" if `data` is NULL, else "tag:data". Returns the number
// of characters written, excluding the NUL terminator.
size_t testlog_format_line(char *out, size_t out_size, const char *tag, const char *data);

// Fixed-capacity FIFO of formatted lines awaiting send. A line is popped
// only once the adapter confirms it was sent -- entries already accepted
// into the queue are never dropped; `testlog_queue_push` simply refuses
// once the queue is full (the adapter's own send loop keeps it short).
// No-drop AND no-stall: the adapter (src/c/testlog_am.c) self-schedules a
// retry via AppTimer whenever a send attempt fails synchronously (no
// sent/failed callback would otherwise ever re-drive it).
typedef struct {
  char lines[TESTLOG_QUEUE_CAPACITY][TESTLOG_MAX_LINE_LEN + 1];
  int head;
  int count;
} TestLogQueue;

void testlog_queue_init(TestLogQueue *queue);
bool testlog_queue_is_empty(const TestLogQueue *queue);
bool testlog_queue_is_full(const TestLogQueue *queue);
// Copies `line` into the queue. Returns false (no-op) if the queue is full.
bool testlog_queue_push(TestLogQueue *queue, const char *line);
// Returns the line at the front of the queue (the one currently being sent),
// or NULL if the queue is empty. The pointer is valid until the next push/pop.
const char *testlog_queue_peek(const TestLogQueue *queue);
// Removes the front line -- call only after the adapter confirms it sent.
void testlog_queue_pop(TestLogQueue *queue);
