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
  persist_write_int(PERSIST_KEY_COUNT, store->count);
  persist_write_int(PERSIST_KEY_NEXT_COOKIE, store->next_cookie);

  for (int i = 0; i < store->count; i++) {
    uint8_t buf[SERIALIZE_TIMER_WIRE_SIZE];
    size_t n = serialize_timer_with_store(store, &store->timers[i], buf, sizeof buf);
    if (n > 0) {
      persist_write_data(PERSIST_KEY_TIMER_BASE + i, buf, n);
    }
  }
  // Clear stale slots left over from a previously-larger store.
  for (int i = store->count; i < MAX_TIMERS; i++) {
    if (persist_exists(PERSIST_KEY_TIMER_BASE + i)) {
      persist_delete(PERSIST_KEY_TIMER_BASE + i);
    }
  }

  char data[8];
  snprintf(data, sizeof data, "%d", store->count);
  testlog("persist", data);  // makes the saved count observable to automated tests, on every save
}
