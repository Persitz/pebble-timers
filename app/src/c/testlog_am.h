// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// Adapter half of the test-log channel: the AppMessage write on the literal
// key 10000 (matching the harness's default EMU_TEST_LOG_KEY convention --
// do not route this through package.json messageKeys). This file, and
// testlog_am.c, are the only places allowed to touch AppMessage for this
// channel; the pure line formatter + retry queue live in src/c/lib/testlog.{c,h}.
// Under EMU_TEST_HOOKS (a PEBBLE_EMU_TEST_HOOKS=1 build only -- see wscript's
// build()), this same file also owns the ONE inbound key this app ever reads
// (10001/10002/10003, sent via a harness action script's generic `send_appmessage`
// action), routing it to dictation_am.c's dictation_am_inject(). Release builds
// register no inbox handler.
#pragma once

// Compiled in ONLY when TESTLOG_ENABLED is defined (PEBBLE_TESTLOG=1, or any
// GALLERY_MODE / EMU_TEST_HOOKS build -- see wscript's build()). A plain
// release build sends NO AppMessages at all: on a real phone, AppMessages
// from this pkjs-less app trigger the Pebble app's LegacyPebbleKit
// permission prompt. In that build both functions below are empty inline
// stubs, so every call site stays unconditional.
#ifdef TESTLOG_ENABLED

// Registers the AppMessage outbox sent/failed callbacks that drive the
// retry queue, and (EMU_TEST_HOOKS builds only) the inbox handler for the
// transcript-injection test hook. Call once after app_message_open(), which
// main.c also calls only under TESTLOG_ENABLED.
void testlog_init(void);

// Formats and enqueues a test-log line ("tag" or "tag:data"); sends
// immediately if the outbox is free, otherwise the queue drains it as soon
// as the in-flight send completes. `data` may be NULL.
void testlog(const char *tag, const char *data);
#else
static inline void testlog_init(void) {}
static inline void testlog(const char *tag, const char *data) {
  (void)tag;
  (void)data;
}
#endif
