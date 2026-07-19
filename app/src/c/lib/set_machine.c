// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

#include "set_machine.h"

#include "format.h"

static int wrap60(int v) {
  v %= 60;
  if (v < 0) v += 60;
  return v;
}

static int clamp_h(int v) {
  if (v < 0) return 0;
  if (v > SET_MACHINE_MAX_HOURS) return SET_MACHINE_MAX_HOURS;
  return v;
}

void set_machine_init(SetState *s) {
  s->h = 0;
  s->m = 0;
  s->s = 0;
  s->field = SET_FIELD_M;
  s->is_repeat = false;
}

void set_machine_init_from_seconds(SetState *s, int32_t total_s) {
  int h, m, sec;
  format_split_hms(total_s, &h, &m, &sec);
  s->h = clamp_h(h);
  s->m = wrap60(m);
  s->s = wrap60(sec);
  s->field = SET_FIELD_M;
  // Edit-interval mode is governed by screen_set.c's own s_edit_cookie, not
  // this flag -- the toggle is disabled there regardless, so false is correct.
  s->is_repeat = false;
}

void set_machine_init_from_seconds_kind(SetState *s, int32_t total_s, bool is_repeat) {
  set_machine_init_from_seconds(s, total_s);
  s->is_repeat = is_repeat;
}

void set_machine_adjust(SetState *s, int delta) {
  switch (s->field) {
    case SET_FIELD_H:
      s->h = clamp_h(s->h + delta);
      break;
    case SET_FIELD_M:
      s->m = wrap60(s->m + delta);
      break;
    case SET_FIELD_S:
      s->s = wrap60(s->s + delta);
      break;
  }
}

SetNavResult set_machine_field_right(SetState *s) {
  switch (s->field) {
    case SET_FIELD_H:
      s->field = SET_FIELD_M;
      return SET_NAV_MOVED;
    case SET_FIELD_M:
      s->field = SET_FIELD_S;
      return SET_NAV_MOVED;
    case SET_FIELD_S:
    default:
      return SET_NAV_START;
  }
}

SetNavResult set_machine_field_left(SetState *s) {
  switch (s->field) {
    case SET_FIELD_S:
      s->field = SET_FIELD_M;
      return SET_NAV_MOVED;
    case SET_FIELD_M:
      s->field = SET_FIELD_H;
      return SET_NAV_MOVED;
    case SET_FIELD_H:
    default:
      return SET_NAV_CANCEL;
  }
}

int32_t set_machine_duration_s(const SetState *s) {
  return (int32_t)s->h * 3600 + (int32_t)s->m * 60 + (int32_t)s->s;
}

time_t set_machine_end_clock(const SetState *s, time_t now) {
  return now + set_machine_duration_s(s);
}

bool set_machine_kind_toggle_available(const SetState *s) {
  return set_machine_duration_s(s) >= 1;
}

void set_machine_toggle_kind(SetState *s) {
  if (!set_machine_kind_toggle_available(s)) return;  // no-op at 0:00
  s->is_repeat = !s->is_repeat;
}

bool set_machine_is_repeat(const SetState *s) {
  return s->is_repeat && set_machine_kind_toggle_available(s);
}

bool set_machine_is_stopwatch(const SetState *s) {
  return !set_machine_is_repeat(s) && set_machine_duration_s(s) == 0;
}
