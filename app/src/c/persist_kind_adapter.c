// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

#include "persist_kind_adapter.h"

#include <pebble.h>

#define PERSIST_KEY_LAST_DIAL_KIND 31

static TimerKind s_last_kind = KIND_COUNTDOWN;

void persist_kind_adapter_load(void) {
  s_last_kind = KIND_COUNTDOWN;
  uint8_t value;
  if (!persist_exists(PERSIST_KEY_LAST_DIAL_KIND)) return;
  if (persist_read_data(PERSIST_KEY_LAST_DIAL_KIND, &value, sizeof value) != sizeof value) return;
  if (value == KIND_REPEAT) s_last_kind = KIND_REPEAT;
}

TimerKind persist_kind_adapter_last(void) {
  return s_last_kind;
}

void persist_kind_adapter_save(TimerKind kind) {
  if (kind != KIND_COUNTDOWN && kind != KIND_REPEAT) return;

  // The in-memory value is updated FIRST, unconditionally, and deliberately
  // does NOT depend on the flash write succeeding. The old order kept the
  // in-memory default unchanged on failure.
  //
  // The argument for the old order was flash-consistency: don't let RAM claim
  // something disk doesn't have. But this field is not a record of anything;
  // it is a UX convenience -- "open the SET dial on the kind you last used"
  // (screen_set.c:652/673 are its only readers). Under the old order a failed
  // flash write meant the user picked Repeat, and then the very next SET
  // screen IN THE SAME SESSION opened on Countdown. That is a visible wrong
  // answer to a question the user just answered. A stale byte on disk is not
  // observable by anyone; the wrong dial kind is, immediately.
  //
  // So on a flash failure this now degrades to "correct for this session,
  // stale after a relaunch" rather than "wrong immediately and after".
  s_last_kind = kind;

  // pebble.h: "The number of bytes written if successful, a value from
  // StatusCode otherwise." Still checked rather than discarded, so the
  // distinction stays visible to anyone reading or extending this -- there is
  // simply no recovery action available for a failed one-byte convenience
  // write, and no caller that could use one. Unlike durable writes with a
  // success marker, this adapter has no marker that could falsely claim a
  // failed write succeeded.
  uint8_t value = (uint8_t)kind;
  if (persist_write_data(PERSIST_KEY_LAST_DIAL_KIND, &value, sizeof value) != (int)sizeof value) {
    return;
  }
}
