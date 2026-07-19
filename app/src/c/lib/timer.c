// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

#include "timer.h"

#include <string.h>

static int32_t max_i32(int32_t a, int32_t b) {
  return a > b ? a : b;
}

// ASCII-only whitespace check (deliberately not <ctype.h>'s isspace, whose
// behavior is locale-dependent and undefined for negative char values on a
// platform where char is signed -- the trim below only ever needs to
// recognize plain space/tab/CR/LF, never a locale's own notion of
// "whitespace").
static bool is_ascii_space(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

void timer_name_copy(char *dst, size_t dst_size, const char *src) {
  if (dst_size == 0) return;
  memset(dst, 0, dst_size);
  if (src == NULL) return;

  size_t start = 0;
  while (src[start] != '\0' && is_ascii_space(src[start])) start++;
  size_t end = strlen(src);
  while (end > start && is_ascii_space(src[end - 1])) end--;

  size_t cap = dst_size - 1;  // room for the NUL
  // Walk forward codepoint-by-codepoint (by the UTF-8 lead byte's own
  // declared sequence length) so the cut point never lands mid-character --
  // a codepoint that wouldn't fully fit within `cap` is dropped whole,
  // rather than truncated into an invalid dangling continuation byte.
  size_t safe_len = 0;
  size_t i = start;
  while (i < end) {
    unsigned char c = (unsigned char)src[i];
    size_t seqlen;
    if ((c & 0x80) == 0x00) seqlen = 1;       // ASCII
    else if ((c & 0xE0) == 0xC0) seqlen = 2;  // 2-byte lead
    else if ((c & 0xF0) == 0xE0) seqlen = 3;  // 3-byte lead
    else if ((c & 0xF8) == 0xF0) seqlen = 4;  // 4-byte lead
    else seqlen = 1;  // stray/invalid byte -- treat as one byte, never loop forever
    if (i - start + seqlen > cap) break;
    i += seqlen;
    safe_len = i - start;
  }

  memcpy(dst, &src[start], safe_len);
  // dst[safe_len] is already '\0' from the memset above.
}

static int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

bool timer_is_paused(const Timer *t) {
  return t->is_paused;
}

int32_t timer_effective_elapsed_s(const Timer *t, time_t now) {
  time_t reference = t->is_paused ? t->paused_at : now;
  int64_t raw = (int64_t)reference - (int64_t)t->started_at - (int64_t)t->accumulated_pause_s;
  return (int32_t)max_i32((int32_t)raw, 0);
}

int32_t timer_remaining_s(const Timer *t, time_t now) {
  return t->duration_s - timer_effective_elapsed_s(t, now);
}

int32_t timer_elapsed_s(const Timer *t, time_t now) {
  return timer_effective_elapsed_s(t, now);
}

int32_t timer_overrun_s(const Timer *t, time_t now) {
  return max_i32(timer_effective_elapsed_s(t, now) - t->duration_s, 0);
}

time_t timer_deadline(const Timer *t) {
  return t->started_at + t->accumulated_pause_s + t->duration_s;
}

bool timer_is_firing(const Timer *t, time_t now) {
  if (t->kind != KIND_COUNTDOWN && t->kind != KIND_REPEAT) return false;
  if (t->is_paused || t->fired) return false;
  return timer_effective_elapsed_s(t, now) >= t->duration_s;
}

int timer_progress_pct(const Timer *t, time_t now) {
  if (t->duration_s <= 0) return 0;
  int32_t eff = timer_effective_elapsed_s(t, now);
  int pct = (int)((int64_t)eff * 100 / t->duration_s);
  return clamp_i32(pct, 0, 100);
}

void timer_pause(Timer *t, time_t now) {
  if (t->is_paused) return;
  t->is_paused = true;
  t->paused_at = now;
}

void timer_resume(Timer *t, time_t now) {
  if (!t->is_paused) return;
  t->accumulated_pause_s += (int32_t)(now - t->paused_at);
  t->is_paused = false;
}

void timer_add_minute(Timer *t) {
  t->duration_s += 60;
  t->fired = false;
}

void timer_restart(Timer *t, time_t now) {
  t->started_at = now;
  t->accumulated_pause_s = 0;
  t->is_paused = false;
  t->fired = false;
  t->cycles = 0;
  t->lap_count = 0;
  // Restart/re-arm returns duration_s to set_total_s -- the dialed SET amount --
  // dropping any transient growth (+1:00 / add-1/4 / snooze) that ran since.
  // Scoped to COUNTDOWN and REPEAT only -- KIND_STOPWATCH's "Reset to 0"
  // contract is unrelated (duration_s there is either 0 for a plain
  // stopwatch, or the post-timer stopwatch's own load-bearing overrun-calc
  // value; neither has "set_total_s" semantics, and both stay untouched).
  if (t->kind == KIND_COUNTDOWN || t->kind == KIND_REPEAT) {
    t->duration_s = t->set_total_s;
  }
  // For a repeat timer on restart: if it was mid-snooze (original_total_s set),
  // that marker must be cleared so timer_is_snoozing() returns false afterward.
  // Scoped to KIND_REPEAT only: for a plain/post-timer STOPWATCH this field
  // is the "from a X timer" provenance value ("Reset to 0" must not erase
  // that lineage). This mirrors timer_advance_cycle()'s identical clear on
  // every natural re-arm.
  if (t->kind == KIND_REPEAT) {
    t->original_total_s = 0;
  }
}

void timer_snooze(Timer *t, int minutes, time_t now) {
  t->kind = KIND_COUNTDOWN;
  t->duration_s = minutes * 60;
  t->started_at = now;
  t->accumulated_pause_s = 0;
  t->is_paused = false;
  t->fired = false;
  t->original_total_s = 0;  // fresh countdown -- see this function's header doc
}

void timer_keep_counting(Timer *t, time_t now) {
  (void)now;
  t->original_total_s = t->duration_s;
  t->kind = KIND_STOPWATCH;
  // started_at / accumulated_pause_s / duration_s are intentionally kept:
  // elapsed() must continue seamlessly through the zero-crossing.
  t->fired = false;
}

void timer_advance_cycle(Timer *t, time_t now) {
  t->cycles += 1;
  t->started_at = now;
  t->accumulated_pause_s = 0;
  t->is_paused = false;
  t->fired = false;
  t->original_total_s = 0;  // clears the repeat-snoozing marker, if one was set
}

void timer_toggle_repeat(Timer *t, time_t now) {
  // The clock (started_at/paused_at/accumulated_pause_s/is_paused) is
  // preserved across a kind flip.
  (void)now;
  t->kind = (t->kind == KIND_REPEAT) ? KIND_COUNTDOWN : KIND_REPEAT;
  t->fired = false;
  t->cycles = 0;
  t->original_total_s = 0;  // kind flip -- see this function's header doc
}

void timer_edit_interval(Timer *t, int32_t new_duration_s, time_t now) {
  t->duration_s = new_duration_s;
  // A deliberate interval edit re-dials -- the new interval IS the new "set"
  // amount a later timer_restart() targets.
  t->set_total_s = new_duration_s;
  t->started_at = now;
  t->accumulated_pause_s = 0;
  t->is_paused = false;
  t->fired = false;
  t->original_total_s = 0;  // ends any repeat-snoozing window -- see header doc
}


int32_t timer_quarter_amount(const Timer *t) {
  // A repeat's interval never grows on its own -- its original_total_s is
  // the repeat-snoozing marker, not a "set amount" -- so this reads duration_s
  // DIRECTLY for KIND_REPEAT. For KIND_COUNTDOWN this reads set_total_s
  // directly too -- always populated by create time, so no lazy-cache
  // fallback is needed.
  if (t->kind == KIND_REPEAT) return t->duration_s / 4;
  return t->set_total_s / 4;
}

void timer_add_quarter(Timer *t, time_t now) {
  (void)now;
  // set_total_s (read via timer_quarter_amount() above) is already the
  // stable SET-amount base -- no lazy-caching-on-first-call needed.
  //
  // Use int64_t intermediate so the addition itself is never the overflow
  // (both operands are always well within int64_t range) -- then saturate,
  // don't reject.
  int64_t next = (int64_t)t->duration_s + (int64_t)timer_quarter_amount(t);
  if (next > TIMER_MAX_DURATION_S) next = TIMER_MAX_DURATION_S;
  // Never let the "add" gesture shrink the timer.
  if (next < t->duration_s) next = t->duration_s;
  t->duration_s = (int32_t)next;
  t->fired = false;
}

bool timer_add_quarter_available(const Timer *t) {
  // The quarter-amount gate's noise-floor check alone was true even AT/ABOVE
  // TIMER_MAX_DURATION_S. But timer_add_quarter()'s never-shrink + saturation
  // guards make the mutation a permanent, silent no-op there, while this
  // predicate kept reporting "available." Requiring the mutation to actually
  // be EFFECTIVE (current duration strictly below the cap) hides the gesture
  // once it can no longer do anything, matching "available means it does
  // something."
  return timer_quarter_amount(t) > 10 && t->duration_s < TIMER_MAX_DURATION_S;
}

void timer_snooze_repeat(Timer *t, time_t now) {
  int32_t quarter = timer_quarter_amount(t);
  t->started_at = now - (t->duration_s - quarter);
  t->accumulated_pause_s = 0;
  t->is_paused = false;
  t->fired = false;
  // original_total_s: nonzero => snoozing. See this function's header doc.
  t->original_total_s = t->duration_s;
}

void timer_snooze_seconds(Timer *t, uint32_t secs, time_t now) {
  t->kind = KIND_COUNTDOWN;
  t->duration_s = (int32_t)secs;
  t->started_at = now;
  t->accumulated_pause_s = 0;
  t->is_paused = false;
  t->fired = false;
  t->original_total_s = 0;  // fresh countdown -- see this function's header doc
}

bool timer_is_snoozing(const Timer *t) {
  return t->kind == KIND_REPEAT && t->original_total_s != 0;
}
