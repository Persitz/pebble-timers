// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

#include "store.h"

#include <string.h>

static LapSet *lap_set_find(TimerStore *s, int32_t cookie) {
  for (int i = 0; i < MAX_LAP_SETS; i++) {
    if (s->lap_sets[i].owner_cookie == cookie) return &s->lap_sets[i];
  }
  return NULL;
}

static const LapSet *lap_set_find_const(const TimerStore *s, int32_t cookie) {
  for (int i = 0; i < MAX_LAP_SETS; i++) {
    if (s->lap_sets[i].owner_cookie == cookie) return &s->lap_sets[i];
  }
  return NULL;
}

static LapSet *lap_set_claim(TimerStore *s, int32_t cookie) {
  LapSet *set = lap_set_find(s, cookie);
  if (set != NULL) return set;
  for (int i = 0; i < MAX_LAP_SETS; i++) {
    if (s->lap_sets[i].owner_cookie == 0) {
      s->lap_sets[i].owner_cookie = cookie;
      return &s->lap_sets[i];
    }
  }
  return NULL;
}

static void lap_set_release(TimerStore *s, int32_t cookie) {
  LapSet *set = lap_set_find(s, cookie);
  if (set != NULL) memset(set, 0, sizeof(*set));
}

void store_init(TimerStore *s) {
  memset(s, 0, sizeof(*s));
  s->next_cookie = 1;  // cookie 0 is never assigned -- reserved as "no timer"
}

bool store_add(TimerStore *s, TimerKind kind, int32_t duration_s, time_t now, int32_t *out_cookie) {
  if (s->count >= MAX_TIMERS) return false;

  Timer *t = &s->timers[s->count];
  memset(t, 0, sizeof(*t));
  t->cookie = s->next_cookie++;
  t->kind = kind;
  t->duration_s = duration_s;
  // The dedicated "what the user actually dialed" field -- every create path
  // (dial, voice, preset launch) goes through this one store_add(), so this
  // is the ONE write site that establishes it (timer_edit_interval() is the
  // only other writer, for a deliberate later re-dial).
  t->set_total_s = duration_s;
  t->started_at = now;

  s->count++;
  if (out_cookie != NULL) *out_cookie = t->cookie;
  return true;
}

bool store_remove(TimerStore *s, int32_t cookie) {
  for (int i = 0; i < s->count; i++) {
    if (s->timers[i].cookie == cookie) {
      lap_set_release(s, cookie);
      for (int j = i; j < s->count - 1; j++) {
        s->timers[j] = s->timers[j + 1];
      }
      s->count--;
      memset(&s->timers[s->count], 0, sizeof(s->timers[s->count]));
      return true;
    }
  }
  return false;
}

bool store_set_name(TimerStore *s, int32_t cookie, const char *name) {
  Timer *t = store_find(s, cookie);
  if (t == NULL) return false;
  timer_name_copy(t->name, sizeof t->name, name);
  return true;
}

void store_clear(TimerStore *s) {
  memset(s->timers, 0, sizeof(s->timers));
  memset(s->lap_sets, 0, sizeof(s->lap_sets));
  s->count = 0;
  // next_cookie is deliberately left untouched -- see store.h's doc comment.
}

bool store_add_lap(TimerStore *s, Timer *t, time_t now) {
  if (t == NULL || t->kind != KIND_STOPWATCH || t->lap_count >= MAX_LAPS) return false;
  LapSet *set = lap_set_claim(s, t->cookie);
  if (set == NULL) return false;
  set->values[t->lap_count++] = timer_elapsed_s(t, now);
  return true;
}

bool store_lap_can_add(const TimerStore *s, const Timer *t) {
  if (t == NULL || t->kind != KIND_STOPWATCH || t->lap_count >= MAX_LAPS) return false;
  if (lap_set_find_const(s, t->cookie) != NULL) return true;
  for (int i = 0; i < MAX_LAP_SETS; i++) {
    if (s->lap_sets[i].owner_cookie == 0) return true;
  }
  return false;
}

void store_restart(TimerStore *s, Timer *t, time_t now) {
  if (t == NULL) return;
  lap_set_release(s, t->cookie);
  timer_restart(t, now);
}

bool store_lap_get(const TimerStore *s, const Timer *t, int index, int32_t *out_value) {
  if (t == NULL || out_value == NULL || index < 0 || index >= t->lap_count) return false;
  const LapSet *set = lap_set_find_const(s, t->cookie);
  if (set == NULL) return false;
  *out_value = set->values[index];
  return true;
}

bool store_restore_laps(TimerStore *s, Timer *t, const int32_t *values, uint8_t count) {
  if (t == NULL || values == NULL || count > MAX_LAPS) return false;
  if (count == 0) {
    t->lap_count = 0;
    return true;
  }
  LapSet *set = lap_set_claim(s, t->cookie);
  if (set == NULL) return false;
  memcpy(set->values, values, (size_t)count * sizeof(values[0]));
  t->lap_count = count;
  return true;
}

Timer *store_find(TimerStore *s, int32_t cookie) {
  for (int i = 0; i < s->count; i++) {
    if (s->timers[i].cookie == cookie) return &s->timers[i];
  }
  return NULL;
}

const Timer *store_find_const(const TimerStore *s, int32_t cookie) {
  for (int i = 0; i < s->count; i++) {
    if (s->timers[i].cookie == cookie) return &s->timers[i];
  }
  return NULL;
}

typedef struct {
  int32_t key;
  int idx;  // index into s->timers
} SortEntry;

// Stable insertion sort (ascending key) -- MAX_TIMERS is small (16), so O(n^2)
// is irrelevant and this avoids pulling in qsort's non-stable, non-reentrant
// comparator-via-global-state pattern.
static void stable_sort(SortEntry *entries, int n) {
  for (int i = 1; i < n; i++) {
    SortEntry cur = entries[i];
    int j = i - 1;
    while (j >= 0 && entries[j].key > cur.key) {
      entries[j + 1] = entries[j];
      j--;
    }
    entries[j + 1] = cur;
  }
}

int store_home_order(const TimerStore *s, time_t now, int32_t *out_cookies, int max_out) {
  SortEntry running[MAX_TIMERS], paused[MAX_TIMERS], stopwatch[MAX_TIMERS];
  int n_running = 0, n_paused = 0, n_stopwatch = 0;

  for (int i = 0; i < s->count; i++) {
    const Timer *t = &s->timers[i];
    if (t->kind == KIND_COUNTDOWN || t->kind == KIND_REPEAT) {
      int32_t key = timer_remaining_s(t, now);
      if (t->is_paused) {
        paused[n_paused].key = key;
        paused[n_paused].idx = i;
        n_paused++;
      } else {
        running[n_running].key = key;
        running[n_running].idx = i;
        n_running++;
      }
    } else {
      stopwatch[n_stopwatch].key = timer_elapsed_s(t, now);
      stopwatch[n_stopwatch].idx = i;
      n_stopwatch++;
    }
  }

  stable_sort(running, n_running);
  stable_sort(paused, n_paused);
  stable_sort(stopwatch, n_stopwatch);

  int written = 0;
  for (int i = 0; i < n_running && written < max_out; i++) {
    out_cookies[written++] = s->timers[running[i].idx].cookie;
  }
  for (int i = 0; i < n_paused && written < max_out; i++) {
    out_cookies[written++] = s->timers[paused[i].idx].cookie;
  }
  for (int i = 0; i < n_stopwatch && written < max_out; i++) {
    out_cookies[written++] = s->timers[stopwatch[i].idx].cookie;
  }
  return written;
}

// Maps a remembered timer cookie onto its live row index in the current
// HOME ordering. This exists because navigating Back from a hero screen to
// HOME must land the cursor on that timer's own row, but row indices can
// shift between navigations as timers are added, removed, or reordered --
// the adapter (screen_home.c) only holds onto the cookie it remembered, not
// a row index. The mapping is implemented here, pure and host-testable,
// rather than reimplemented ad hoc in the adapter. Returns the cookie's
// 0-based index in the current store_home_order() ranking, or -1 if the
// cookie no longer exists (e.g. its timer was deleted while its hero screen
// was open) -- the adapter's own fallback-to-row-0 rule applies to that -1
// case, not this function.
int store_home_row_for_cookie(const TimerStore *s, int32_t cookie, time_t now) {
  int32_t order[MAX_TIMERS];
  int n = store_home_order(s, now, order, MAX_TIMERS);
  for (int i = 0; i < n; i++) {
    if (order[i] == cookie) return i;
  }
  return -1;
}
