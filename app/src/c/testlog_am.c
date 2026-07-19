// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

#include "testlog_am.h"

#include <pebble.h>

#include "lib/testlog.h"

#ifdef EMU_TEST_HOOKS
#include "dictation_am.h"
#endif

// Matches the harness's default EMU_TEST_LOG_KEY -- a documented convention,
// not a harness requirement; this app opts into it on this literal key.
#define TEST_LOG_KEY 10000

#ifdef EMU_TEST_HOOKS
// Test-only inbound channel, compiled ONLY into a PEBBLE_EMU_TEST_HOOKS=1
// build (see wscript's build()) -- release builds never register this inbox
// handler. A harness action script's app-agnostic `send_appmessage` action
// sends {INJECT_TRANSCRIPT_KEY: "<transcript>"[, INJECT_STATUS_KEY: <int>]};
// this handler reads it and funnels it through dictation_am_inject() ->
// dictation_am.c's real deliver(), the same path a real phone
// status_callback uses. Keys are literals (like TEST_LOG_KEY above), not
// package.json messageKeys, and are private to this app -- the harness
// itself has no knowledge of them.
#define INJECT_TRANSCRIPT_KEY 10001
#define INJECT_STATUS_KEY 10002
#endif

// If app_message_outbox_begin()/_send() fails SYNCHRONOUSLY (no sent/failed
// callback will ever fire for a message that was never actually accepted),
// nothing else re-drives the queue until the next testlog() call -- a genuine
// stall, not just "eventually sent". Re-drive it ourselves via a short
// AppTimer retry so the guarantee is actually unconditional
// no-drop-AND-no-stall, not "no-drop, may need a nudge".
#define RETRY_DELAY_MS 50

static TestLogQueue s_queue;
static bool s_send_in_flight;
static bool s_retry_scheduled;
static AppTimer *s_retry_timer;

static void try_send_next(void);

static void retry_timer_cb(void *data) {
  (void)data;
  s_retry_scheduled = false;
  s_retry_timer = NULL;
  try_send_next();
}

static void schedule_retry(void) {
  if (s_retry_scheduled) return;
  s_retry_scheduled = true;
  s_retry_timer = app_timer_register(RETRY_DELAY_MS, retry_timer_cb, NULL);
}

static void try_send_next(void) {
  if (s_send_in_flight) return;
  const char *line = testlog_queue_peek(&s_queue);
  if (line == NULL) return;

  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) != APP_MSG_OK) {
    // Outbox unavailable right now -- no callback will fire for a begin that
    // never happened, so self-schedule the retry (no stall).
    schedule_retry();
    return;
  }
  dict_write_cstring(iter, TEST_LOG_KEY, line);
  dict_write_end(iter);
  if (app_message_outbox_send() == APP_MSG_OK) {
    s_send_in_flight = true;
  } else {
    // Synchronous send rejection: same reasoning -- no sent/failed callback
    // is coming for a send that was never actually queued by the OS.
    schedule_retry();
  }
}

static void outbox_sent_handler(DictionaryIterator *iterator, void *context) {
  (void)iterator;
  (void)context;
  s_send_in_flight = false;
  testlog_queue_pop(&s_queue);
  try_send_next();
}

static void outbox_failed_handler(DictionaryIterator *iterator, AppMessageResult reason,
                                   void *context) {
  (void)iterator;
  (void)reason;
  (void)context;
  s_send_in_flight = false;
  try_send_next();  // retry the same front line -- not popped, no-drop.
}

#ifdef EMU_TEST_HOOKS
static void inbox_received_handler(DictionaryIterator *iterator, void *context) {
  (void)context;
  Tuple *transcript_tuple = dict_find(iterator, INJECT_TRANSCRIPT_KEY);
  if (transcript_tuple == NULL || transcript_tuple->type != TUPLE_CSTRING) return;
  Tuple *status_tuple = dict_find(iterator, INJECT_STATUS_KEY);
  int status = (status_tuple != NULL && status_tuple->type == TUPLE_INT)
                    ? (int)status_tuple->value->int32
                    : 0;  // default: DictationSessionStatusSuccess
  dictation_am_inject(status, transcript_tuple->value->cstring);
}
#endif

void testlog_init(void) {
  testlog_queue_init(&s_queue);
  s_send_in_flight = false;
  s_retry_scheduled = false;
  s_retry_timer = NULL;
  app_message_register_outbox_sent(outbox_sent_handler);
  app_message_register_outbox_failed(outbox_failed_handler);
#ifdef EMU_TEST_HOOKS
  app_message_register_inbox_received(inbox_received_handler);
#endif
}

void testlog(const char *tag, const char *data) {
  char line[TESTLOG_MAX_LINE_LEN + 1];
  testlog_format_line(line, sizeof line, tag, data);
  if (testlog_queue_push(&s_queue, line)) {
    try_send_next();
  }
}
