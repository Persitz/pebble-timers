// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

#include "view.h"

#include <stddef.h>  // NULL (view_row_label)

ViewHeroBranch view_hero_branch(const Timer *t, bool is_ringing) {
  if (t->kind == KIND_STOPWATCH) {
    return t->original_total_s > 0 ? VIEW_HERO_POST_TIMER : VIEW_HERO_STOPWATCH;
  }
  if (t->fired) return is_ringing ? VIEW_HERO_ALARM : VIEW_HERO_OVERTIME;
  return VIEW_HERO_RUNNING;
}

ViewColorToken view_kind_color_token(TimerKind kind, ViewHeroBranch branch) {
  if (branch == VIEW_HERO_ALARM && kind != KIND_REPEAT) return VIEW_COLOR_ALARM;
  switch (kind) {
    case KIND_STOPWATCH:
      return VIEW_COLOR_STOPWATCH;
    case KIND_REPEAT:
      return VIEW_COLOR_REPEAT;
    case KIND_COUNTDOWN:
    default:
      return VIEW_COLOR_COUNTDOWN;
  }
}

ViewPrefixKind view_value_prefix(ViewHeroBranch branch) {
  switch (branch) {
    case VIEW_HERO_ALARM:
      return VIEW_PREFIX_CARET;
    case VIEW_HERO_OVERTIME:
      return VIEW_PREFIX_PLUS;
    case VIEW_HERO_STOPWATCH:
      // By design, the "+" prefix is not shown for stopwatch main value.
      // A plain stopwatch's big value reads bare ("4:31", not "+4:31"),
      // matching the post-timer stopwatch's big value (which has no "+" --
      // its overrun lives on the separate chip). Countdown-family overrun
      // (VIEW_HERO_OVERTIME, just above) explicitly keeps its "+".
      return VIEW_PREFIX_NONE;
    case VIEW_HERO_POST_TIMER:
    case VIEW_HERO_RUNNING:
    default:
      return VIEW_PREFIX_NONE;
  }
}

ViewHomeValueSize view_home_value_size(int timer_count) {
  if (timer_count <= 1) return VIEW_HOME_SIZE_XLARGE;
  if (timer_count <= 2) return VIEW_HOME_SIZE_LARGE;
  if (timer_count <= 4) return VIEW_HOME_SIZE_MEDIUM;
  return VIEW_HOME_SIZE_SMALL;
}

int view_home_row_count(int timer_count) {
  // +4 = "+ New timer" + "Say a timer" + "Library" + "Clear all" once
  // populated; the empty state is a flat 3 ("+ New timer" hero + "Say a
  // timer" + "Library", no Clear-all) -- see this function's own header
  // doc for the full row model.
  return (timer_count >= 1) ? timer_count + 4 : 3;
}

int view_home_voice_row(int timer_count) {
  return (timer_count == 0) ? 1 : timer_count + 1;
}

int view_library_row_count(int preset_count) {
  return preset_count + 2;  // presets + "Save new" + "Say a preset"
}

int view_library_voice_row(int preset_count) {
  return preset_count + 1;
}

const char *view_row_label(const char *name) {
  if (name == NULL || name[0] == '\0') return NULL;
  return name;
}

int view_others_active_count(int32_t cookie, const TimerStore *store) {
  int count = 0;
  for (int i = 0; i < store->count; i++) {
    const Timer *t = &store->timers[i];
    if (t->cookie == cookie) continue;
    // A PAUSED sibling is not "still on" -- the subheader this feeds
    // ("ringing - N other(s) still on") reads as "still running/counting",
    // so counting a paused timer actively misinforms the user. A
    // fired-but-not-ringing (OVERTIME) sibling is intentionally still
    // counted -- it IS still counting up, just not currently buzzing.
    if (t->is_paused) continue;
    count++;
  }
  return count;
}
