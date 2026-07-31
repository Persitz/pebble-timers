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

// Registers the AppMessage outbox sent/failed callbacks that drive the
// retry queue, and (EMU_TEST_HOOKS builds only) the inbox handler for the
// transcript-injection test hook. Call once after app_message_open().
void testlog_init(void);

// Formats and enqueues a test-log line ("tag" or "tag:data"); sends
// immediately if the outbox is free, otherwise the queue drains it as soon
// as the in-flight send completes. `data` may be NULL.
void testlog(const char *tag, const char *data);
