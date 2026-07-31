// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

#include "persist_adapter.h"

#include <pebble.h>
#include <stdio.h>

#include "lib/serialize.h"
#include "testlog_am.h"

#define PERSIST_KEY_COUNT 0
#define PERSIST_KEY_NEXT_COOKIE 1
#define PERSIST_KEY_TIMER_BASE 2

void persist_adapter_load(TimerStore *out) {
  store_init(out);

  if (!persist_exists(PERSIST_KEY_COUNT)) return;  // nothing persisted yet -- empty store

  int32_t count = persist_read_int(PERSIST_KEY_COUNT);
  if (count < 0 || count > MAX_TIMERS) return;  // corrupt manifest -- stay empty (tolerant)

  int written = 0;
  int32_t max_cookie_seen = 0;
  for (int32_t i = 0; i < count; i++) {
    uint8_t buf[SERIALIZE_TIMER_WIRE_SIZE];
    int len = persist_read_data(PERSIST_KEY_TIMER_BASE + i, buf, sizeof buf);
    if (len <= 0) continue;  // missing/short slot -- skip, don't abort the whole load

    Timer t;
    bool laps_dropped = false;
    if (!deserialize_timer_with_store(buf, (size_t)len, out, &t, &laps_dropped)) continue;

    // A failed shrinking/reindexing save can leave an old manifest pointing
    // at two slots containing the same shifted timer. Never restore duplicate
    // cookies: the store's cookie is its identity, and the first valid slot
    // remains the recovery winner.
    bool duplicate = false;
    for (int j = 0; j < written; j++) {
      if (out->timers[j].cookie == t.cookie) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) continue;

    out->timers[written++] = t;
    if (laps_dropped) testlog("laps_dropped", "load");
    if (t.cookie > max_cookie_seen) max_cookie_seen = t.cookie;
  }
  out->count = written;

  int32_t next_cookie = persist_exists(PERSIST_KEY_NEXT_COOKIE) ? persist_read_int(PERSIST_KEY_NEXT_COOKIE) : 1;
  if (next_cookie <= max_cookie_seen) next_cookie = max_cookie_seen + 1;  // never reissue a loaded cookie
  out->next_cookie = next_cookie > 0 ? next_cookie : 1;
}

void persist_adapter_save(const TimerStore *store) {
  bool saved = true;

  // The count is the commit marker: publish it only after records and the
  // cookie high-water mark have all been written successfully.
  for (int i = 0; i < store->count; i++) {
    uint8_t buf[SERIALIZE_TIMER_WIRE_SIZE];
    size_t n = serialize_timer_with_store(store, &store->timers[i], buf, sizeof buf);
    if (n == 0 || persist_write_data(PERSIST_KEY_TIMER_BASE + i, buf, n) != (int)n) saved = false;
  }
  // persist_write_int() returns status_t (an int32_t typedef), but its own
  // documented contract (pebble.h: "@return The number of bytes written if
  // successful, a value from \ref StatusCode otherwise") is the SAME
  // byte-count-on-success shape as persist_write_data()/persist_write_string()
  // -- NOT literally 0 on success. On real qemu-pebble firmware, a genuine
  // successful persist_write_int(..., <int32>) returns 4, not 0. Checking
  // `!= 0` therefore treated EVERY successful write as a failure. The host
  // suite did not catch this when its fake also returned 0 on success (a
  // fake calibrated to match the buggy assumption, not real firmware; see
  // that file's fake_persist_write_int() for the corrected
  // convention). `< 0` is the correct check: every documented StatusCode
  // error is negative, and a successful byte count is never negative.
  if (persist_write_int(PERSIST_KEY_NEXT_COOKIE, store->next_cookie) < 0) saved = false;

  // Publish the new bounds before clearing stale slots. A failed delete now
  // cannot tear the old manifest: it is no longer authoritative.
  if (saved && persist_write_int(PERSIST_KEY_COUNT, store->count) < 0) saved = false;
  if (saved) {
    for (int i = store->count; i < MAX_TIMERS; i++) {
      if (persist_exists(PERSIST_KEY_TIMER_BASE + i) &&
          persist_delete(PERSIST_KEY_TIMER_BASE + i) <= 0) {
        saved = false;
      }
    }
  }

  if (!saved) {
    testlog("persist_failed", "write");
    return;
  }

  char data[8];
  snprintf(data, sizeof data, "%d", store->count);
  testlog("persist", data);  // makes the saved count observable to automated tests, on every save
}
