// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

#include "timer.h"
#include "store.h"
#include "unity.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static Timer mk_countdown(int32_t duration_s, time_t started_at) {
  Timer t = {0};
  t.kind = KIND_COUNTDOWN;
  t.duration_s = duration_s;
  // Mirrors what the real create path (lib/store.c's store_add()) always
  // does -- every Timer this helper builds stands in for "as if just
  // dialed," so set_total_s starts equal to duration_s, same as a real
  // create.
  t.set_total_s = duration_s;
  t.started_at = started_at;
  return t;
}

// --- derivations -------------------------------------------------------------

void test_effective_elapsed_running(void) {
  Timer t = mk_countdown(300, 1000);
  TEST_ASSERT_EQUAL_INT32(50, timer_effective_elapsed_s(&t, 1050));
}

void test_effective_elapsed_clamped_at_zero(void) {
  Timer t = mk_countdown(300, 1000);
  // now before started_at (shouldn't happen in practice, but must not go negative)
  TEST_ASSERT_EQUAL_INT32(0, timer_effective_elapsed_s(&t, 900));
}

void test_effective_elapsed_subtracts_pause_accum(void) {
  Timer t = mk_countdown(300, 1000);
  t.accumulated_pause_s = 20;
  TEST_ASSERT_EQUAL_INT32(30, timer_effective_elapsed_s(&t, 1050));
}

void test_effective_elapsed_while_paused_uses_paused_at(void) {
  Timer t = mk_countdown(300, 1000);
  t.is_paused = true;
  t.paused_at = 1040;
  // now keeps advancing but effective_elapsed must freeze at paused_at
  TEST_ASSERT_EQUAL_INT32(40, timer_effective_elapsed_s(&t, 5000));
}

void test_remaining_can_go_negative(void) {
  Timer t = mk_countdown(60, 1000);
  TEST_ASSERT_EQUAL_INT32(-10, timer_remaining_s(&t, 1070));
}

void test_overrun_zero_while_not_overrun(void) {
  Timer t = mk_countdown(60, 1000);
  TEST_ASSERT_EQUAL_INT32(0, timer_overrun_s(&t, 1030));
}

void test_overrun_positive_past_deadline(void) {
  Timer t = mk_countdown(60, 1000);
  TEST_ASSERT_EQUAL_INT32(10, timer_overrun_s(&t, 1070));
}

void test_elapsed_stopwatch(void) {
  Timer t = {0};
  t.kind = KIND_STOPWATCH;
  t.started_at = 1000;
  TEST_ASSERT_EQUAL_INT32(45, timer_elapsed_s(&t, 1045));
}

void test_deadline(void) {
  Timer t = mk_countdown(60, 1000);
  t.accumulated_pause_s = 5;
  TEST_ASSERT_EQUAL(1065, timer_deadline(&t));
}

void test_is_firing_true_at_boundary(void) {
  Timer t = mk_countdown(60, 1000);
  TEST_ASSERT_TRUE(timer_is_firing(&t, 1060));
}

void test_is_firing_false_before_boundary(void) {
  Timer t = mk_countdown(60, 1000);
  TEST_ASSERT_FALSE(timer_is_firing(&t, 1059));
}

void test_is_firing_false_if_already_fired(void) {
  Timer t = mk_countdown(60, 1000);
  t.fired = true;
  TEST_ASSERT_FALSE(timer_is_firing(&t, 1060));
}

void test_is_firing_false_if_paused(void) {
  Timer t = mk_countdown(60, 1000);
  t.is_paused = true;
  t.paused_at = 1060;
  TEST_ASSERT_FALSE(timer_is_firing(&t, 1060));
}

void test_is_firing_false_for_stopwatch(void) {
  // A stopwatch (incl. a post-timer stopwatch with fired left true by a bug)
  // must NEVER be reported as firing -- kind gate is unconditional.
  Timer t = {0};
  t.kind = KIND_STOPWATCH;
  t.started_at = 1000;
  t.duration_s = 60;
  t.fired = true;
  TEST_ASSERT_FALSE(timer_is_firing(&t, 999999));
}

void test_progress_pct_clamped(void) {
  Timer t = mk_countdown(100, 1000);
  TEST_ASSERT_EQUAL_INT(0, timer_progress_pct(&t, 1000));
  TEST_ASSERT_EQUAL_INT(50, timer_progress_pct(&t, 1050));
  TEST_ASSERT_EQUAL_INT(100, timer_progress_pct(&t, 1200));  // past boundary, clamped
}

// --- mutations -----------------------------------------------------------

void test_pause_sets_paused_at_once(void) {
  Timer t = mk_countdown(60, 1000);
  timer_pause(&t, 1030);
  TEST_ASSERT_TRUE(t.is_paused);
  TEST_ASSERT_EQUAL(1030, t.paused_at);
}

void test_pause_idempotent(void) {
  Timer t = mk_countdown(60, 1000);
  timer_pause(&t, 1030);
  timer_pause(&t, 1040);  // must not move paused_at
  TEST_ASSERT_EQUAL(1030, t.paused_at);
}

void test_resume_accumulates_pause_and_clears_flag(void) {
  Timer t = mk_countdown(60, 1000);
  timer_pause(&t, 1030);
  timer_resume(&t, 1050);
  TEST_ASSERT_FALSE(t.is_paused);
  TEST_ASSERT_EQUAL_INT32(20, t.accumulated_pause_s);
}

void test_resume_no_op_when_not_paused(void) {
  Timer t = mk_countdown(60, 1000);
  timer_resume(&t, 1050);
  TEST_ASSERT_EQUAL_INT32(0, t.accumulated_pause_s);
}

void test_pause_then_resume_preserves_remaining(void) {
  Timer t = mk_countdown(60, 1000);
  int32_t before = timer_remaining_s(&t, 1020);  // 40
  timer_pause(&t, 1020);
  timer_resume(&t, 1200);  // paused for 180s
  TEST_ASSERT_EQUAL_INT32(before, timer_remaining_s(&t, 1200));
}

void test_add_minute_extends_duration_and_clears_fired(void) {
  Timer t = mk_countdown(60, 1000);
  t.fired = true;
  timer_add_minute(&t);
  TEST_ASSERT_EQUAL_INT32(120, t.duration_s);
  TEST_ASSERT_FALSE(t.fired);
}

void test_add_minute_on_fired_rearms(void) {
  Timer t = mk_countdown(60, 1000);
  TEST_ASSERT_TRUE(timer_is_firing(&t, 1060));
  t.fired = true;
  timer_add_minute(&t);
  TEST_ASSERT_FALSE(timer_is_firing(&t, 1060));  // no longer at/past the new deadline
  TEST_ASSERT_TRUE(timer_is_firing(&t, 1120));   // fires again at the new deadline
}

void test_restart_resets_everything(void) {
  Timer t = mk_countdown(60, 1000);
  t.accumulated_pause_s = 5;
  t.is_paused = true;
  t.fired = true;
  t.cycles = 3;
  t.lap_count = 2;
  timer_restart(&t, 2000);
  TEST_ASSERT_EQUAL(2000, t.started_at);
  TEST_ASSERT_EQUAL_INT32(0, t.accumulated_pause_s);
  TEST_ASSERT_FALSE(t.is_paused);
  TEST_ASSERT_FALSE(t.fired);
  TEST_ASSERT_EQUAL_INT32(0, t.cycles);
  TEST_ASSERT_EQUAL_UINT8(0, t.lap_count);
}

// Supersedes the old ruling that a Restart leaves duration_s unchanged
// after add-1/4 growth. add_quarter() no longer caches anything into
// original_total_s (that caching role is retired -- see lib/timer.h's own
// doc); original_total_s is simply never written for a countdown any more,
// so this test now pins that Restart leaves it untouched (still 0) rather
// than the old "cache survives" framing.
void test_restart_countdown_leaves_original_total_s_untouched(void) {
  Timer t = mk_countdown(1200, 1000);
  timer_add_quarter(&t, 2000);  // grows duration_s; no longer touches original_total_s
  TEST_ASSERT_EQUAL_INT32(0, t.original_total_s);
  timer_restart(&t, 3000);
  TEST_ASSERT_EQUAL_INT32(0, t.original_total_s);
}

// "+1:00 -> Restart": Restart drops the growth, returning to set_total_s
// (the dialed SET amount) -- fixed by routing timer_restart through the new
// dedicated field.
void test_restart_countdown_restores_set_total_s_after_add_minute(void) {
  Timer t = mk_countdown(120, 1000);  // dialed 2:00
  timer_add_minute(&t);
  TEST_ASSERT_EQUAL_INT32(180, t.duration_s);  // grown to 3:00
  timer_restart(&t, 5000);
  TEST_ASSERT_EQUAL_INT32(120, t.duration_s);  // back to the dialed 2:00, not 3:00
}

// "add-1/4 -> Restart": reverses the old ruling that a Restart deliberately
// did NOT re-dial after add-1/4 growth -- that was wrong; Restart now drops
// the ¼ growth too.
void test_restart_countdown_restores_set_total_s_after_add_quarter(void) {
  Timer t = mk_countdown(1200, 1000);  // dialed 20:00
  timer_add_quarter(&t, 2000);
  TEST_ASSERT_EQUAL_INT32(1500, t.duration_s);  // grown to 25:00 (+5:00)
  timer_restart(&t, 3000);
  TEST_ASSERT_EQUAL_INT32(1200, t.duration_s);  // back to the dialed 20:00
}

// "create -> Restart identity": a Restart with no prior growth at all is a
// no-op on duration_s (set_total_s == duration_s from create, so there's
// nothing to drop).
void test_restart_immediately_after_create_is_identity(void) {
  Timer t = mk_countdown(300, 1000);  // as if freshly store_add()'d
  timer_restart(&t, 5000);
  TEST_ASSERT_EQUAL_INT32(300, t.duration_s);
}

// "snooze (fixed) -> Restart": a snooze is a TRANSIENT adjustment (does not
// touch set_total_s) -- Restarting the resulting snoozed countdown returns
// to the PRE-snooze SET amount, not the snooze length.
void test_restart_after_snooze_restores_pre_snooze_set_total_s(void) {
  Timer t = mk_countdown(1200, 1000);  // dialed 20:00
  timer_snooze(&t, 5, 5000);           // fired, now running a fresh 5:00 snooze
  TEST_ASSERT_EQUAL_INT32(300, t.duration_s);
  TEST_ASSERT_EQUAL_INT32(1200, t.set_total_s);  // untouched by snooze
  timer_restart(&t, 6000);
  TEST_ASSERT_EQUAL_INT32(1200, t.duration_s);  // back to the ORIGINAL 20:00, not the 5:00 snooze
}

// "snooze-1/4 -> Restart" (one-off alarm's seconds-precise snooze): same
// transient-adjustment rule as the fixed-minute snooze above.
void test_restart_after_snooze_seconds_restores_pre_snooze_set_total_s(void) {
  Timer t = mk_countdown(1200, 1000);  // dialed 20:00
  timer_snooze_seconds(&t, 75, 5000);  // fired, now running a 1:15 snooze-1/4
  TEST_ASSERT_EQUAL_INT32(75, t.duration_s);
  TEST_ASSERT_EQUAL_INT32(1200, t.set_total_s);
  timer_restart(&t, 6000);
  TEST_ASSERT_EQUAL_INT32(1200, t.duration_s);  // back to the ORIGINAL 20:00
}

// "edit-interval (repeat) -> Restart": "the new interval IS the new
// original" -- a deliberate re-dial re-sets set_total_s, so Restarting
// after an edit uses the NEW interval, not whatever came before.
void test_restart_after_edit_interval_uses_new_interval_as_set_total_s(void) {
  Timer t = mk_countdown(300, 1000);
  t.kind = KIND_REPEAT;
  timer_edit_interval(&t, 120, 2000);
  TEST_ASSERT_EQUAL_INT32(120, t.set_total_s);
  timer_restart(&t, 3000);
  TEST_ASSERT_EQUAL_INT32(120, t.duration_s);  // the NEW interval, not the original 300
}

// A post-timer stopwatch's "from a X timer" provenance (original_total_s)
// must also survive ACTIONS' "Reset to 0" (timer_restart reused verbatim
// for STOPWATCH) -- the same non-REPEAT preservation rule.
void test_restart_stopwatch_preserves_provenance(void) {
  Timer t = mk_countdown(900, 1000);
  timer_keep_counting(&t, 1938);  // -> post-timer stopwatch, original_total_s=900
  TEST_ASSERT_EQUAL_INT32(900, t.original_total_s);
  timer_restart(&t, 2000);
  TEST_ASSERT_EQUAL_INT32(900, t.original_total_s);
  TEST_ASSERT_EQUAL(KIND_STOPWATCH, t.kind);
}

// Repeat's own "Restart" -- reuses timer_restart verbatim: cycles reset to
// 0 (distinct from timer_advance_cycle()'s "Keep going", which increments),
// kind stays REPEAT, fired/pause/clock all reset exactly like every other
// kind.
void test_restart_repeat_resets_cycles_and_stays_repeat(void) {
  Timer t = mk_countdown(300, 1000);
  t.kind = KIND_REPEAT;
  t.cycles = 5;
  t.fired = true;
  timer_restart(&t, 2000);
  TEST_ASSERT_EQUAL(KIND_REPEAT, t.kind);
  TEST_ASSERT_EQUAL_INT32(0, t.cycles);
  TEST_ASSERT_EQUAL(2000, t.started_at);
  TEST_ASSERT_FALSE(t.fired);
  TEST_ASSERT_EQUAL_INT32(300, timer_remaining_s(&t, 2000));  // fresh full interval
}

// Restarting a repeat that was mid timer_snooze_repeat() (the snoozing
// marker set) must end the snooze window -- a just-restarted timer is not
// "snoozing" by any definition, mirroring timer_advance_cycle()'s own
// identical clear on a natural re-arm
// (test_snooze_repeat_non_destructive_next_cycle_clears_marker above).
// "snooze-1/4 (repeat) -> Restart": timer_snooze_repeat() never touches
// duration_s/set_total_s (the interval itself doesn't change while
// snoozing) -- Restart lands back on the SAME interval either way, but
// pinned explicitly here.
void test_restart_repeat_clears_snoozing_marker(void) {
  Timer t = mk_countdown(300, 1000);
  t.kind = KIND_REPEAT;
  timer_snooze_repeat(&t, 5000);
  TEST_ASSERT_TRUE(timer_is_snoozing(&t));
  timer_restart(&t, 6000);
  TEST_ASSERT_FALSE(timer_is_snoozing(&t));
  TEST_ASSERT_EQUAL_INT32(0, t.original_total_s);
  TEST_ASSERT_EQUAL_INT32(300, t.duration_s);  // still the interval/SET amount
}

void test_snooze_creates_fresh_countdown(void) {
  Timer t = mk_countdown(60, 1000);
  t.fired = true;
  timer_snooze(&t, 5, 2000);
  TEST_ASSERT_EQUAL(KIND_COUNTDOWN, t.kind);
  TEST_ASSERT_EQUAL_INT32(300, t.duration_s);
  TEST_ASSERT_EQUAL(2000, t.started_at);
  TEST_ASSERT_FALSE(t.fired);
}

void test_keep_counting_becomes_stopwatch_with_fired_false(void) {
  // A load-bearing regression: a stopwatch must never carry fired=true.
  Timer t = mk_countdown(900, 1000);  // 15:00 countdown
  t.fired = true;
  timer_keep_counting(&t, 1938);
  TEST_ASSERT_EQUAL(KIND_STOPWATCH, t.kind);
  TEST_ASSERT_FALSE(t.fired);
  TEST_ASSERT_EQUAL_INT32(900, t.original_total_s);
}

void test_keep_counting_elapsed_continues_seamlessly_through_zero(void) {
  Timer t = mk_countdown(900, 1000);  // 15:00 countdown, started at t=1000
  timer_keep_counting(&t, 1938);
  // 938s after start = 15:38 total elapsed, NOT 900+938.
  TEST_ASSERT_EQUAL_INT32(938, timer_elapsed_s(&t, 1938));
}

void test_keep_counting_never_reports_firing(void) {
  Timer t = mk_countdown(60, 1000);
  timer_keep_counting(&t, 1200);
  TEST_ASSERT_FALSE(timer_is_firing(&t, 999999));
}

void test_advance_cycle_increments_and_resets(void) {
  Timer t = mk_countdown(60, 1000);
  t.kind = KIND_REPEAT;
  t.fired = true;
  timer_advance_cycle(&t, 2000);
  TEST_ASSERT_EQUAL_INT32(1, t.cycles);
  TEST_ASSERT_EQUAL(2000, t.started_at);
  TEST_ASSERT_FALSE(t.fired);
}

// Supersedes the old "Toggle RESETS the clock in both directions" ruling.
// The running clock is now preserved across a kind flip -- only `kind`
// changes (plus cycles/fired/the snoozing marker); started_at/paused_at/
// accumulated_pause_s/is_paused are now left exactly as they were.
void test_toggle_repeat_countdown_to_repeat_preserves_elapsed_and_deadline(void) {
  Timer t = mk_countdown(300, 1000);  // 5:00 countdown
  timer_pause(&t, 1200);  // paused at t=1200 -- 2:00 elapsed, 3:00 (100s... see below) remaining
  timer_toggle_repeat(&t, 5000);
  TEST_ASSERT_EQUAL(KIND_REPEAT, t.kind);
  TEST_ASSERT_EQUAL(1000, t.started_at);       // UNCHANGED -- clock preserved
  TEST_ASSERT_TRUE(t.is_paused);               // still paused -- preserved
  TEST_ASSERT_EQUAL(1200, t.paused_at);        // preserved
  TEST_ASSERT_EQUAL_INT32(300, t.duration_s);  // duration UNCHANGED, still true
  TEST_ASSERT_EQUAL_INT32(0, t.cycles);
  // 200s elapsed (1200-1000) -> 100s remaining, regardless of `now` (paused)
  // -- not reset to a fresh 300s.
  TEST_ASSERT_EQUAL_INT32(100, timer_remaining_s(&t, 9999));
}

void test_toggle_repeat_back_to_countdown(void) {
  Timer t = mk_countdown(300, 1000);
  t.kind = KIND_REPEAT;
  t.cycles = 4;
  timer_toggle_repeat(&t, 5000);
  TEST_ASSERT_EQUAL(KIND_COUNTDOWN, t.kind);
  TEST_ASSERT_EQUAL_INT32(0, t.cycles);
  TEST_ASSERT_EQUAL(1000, t.started_at);  // UNCHANGED -- clock preserved
}

// Supersedes the old ruling for the REVERSE direction too ("becomes a
// fresh 5:00 countdown starting NOW -- not a 3:00-remaining countdown").
// That framing is now exactly backwards: the whole point of this fix is
// that the 3:00 remaining MUST survive the flip.
void test_toggle_repeat_repeat_to_countdown_preserves_elapsed_mid_cycle(void) {
  // Closes the rule from the other side: a 5:00 repeat interval with
  // 2:00 elapsed (3:00 remaining), toggled off, must still show 3:00
  // remaining as a countdown -- not reset to a fresh 5:00.
  Timer t = mk_countdown(300, 1000);
  t.kind = KIND_REPEAT;
  t.cycles = 2;
  // 2:00 elapsed (now=1120), still running (not paused) -- 3:00 remaining.
  TEST_ASSERT_EQUAL_INT32(180, timer_remaining_s(&t, 1120));
  timer_toggle_repeat(&t, 1120);
  TEST_ASSERT_EQUAL(KIND_COUNTDOWN, t.kind);
  TEST_ASSERT_EQUAL(1000, t.started_at);       // UNCHANGED
  TEST_ASSERT_EQUAL_INT32(300, t.duration_s);  // duration UNCHANGED
  TEST_ASSERT_EQUAL_INT32(0, t.cycles);
  TEST_ASSERT_EQUAL_INT32(0, t.accumulated_pause_s);
  TEST_ASSERT_FALSE(t.is_paused);
  TEST_ASSERT_FALSE(t.fired);
  // The SAME 3:00 remaining as before the toggle -- elapsed preserved, not
  // the fresh full interval the old ruling used to produce.
  TEST_ASSERT_EQUAL_INT32(180, timer_remaining_s(&t, 1120));
}

// Toggling a countdown that's already past its deadline preserves the
// (already-past) elapsed too -- the flipped-to timer can become immediately
// eligible to fire again. This is acceptable, not a defect.
void test_toggle_repeat_preserves_overtime_elapsed_may_immediately_refire(void) {
  Timer t = mk_countdown(60, 1000);  // 1:00 countdown
  TEST_ASSERT_TRUE(timer_is_firing(&t, 1070));  // 10s past deadline
  timer_toggle_repeat(&t, 1070);
  TEST_ASSERT_EQUAL(KIND_REPEAT, t.kind);
  TEST_ASSERT_EQUAL(1000, t.started_at);  // UNCHANGED -- still 70s elapsed
  TEST_ASSERT_TRUE(timer_is_firing(&t, 1070));  // immediately eligible again
}

void test_edit_interval_resets_clock_keeps_cycles(void) {
  Timer t = mk_countdown(300, 1000);
  t.kind = KIND_REPEAT;
  t.cycles = 4;
  timer_edit_interval(&t, 120, 5000);
  TEST_ASSERT_EQUAL_INT32(120, t.duration_s);
  TEST_ASSERT_EQUAL(5000, t.started_at);
  TEST_ASSERT_EQUAL_INT32(4, t.cycles);  // unchanged
}

// --- countdown Up-hold add-1/4 -------------------------------------------

void test_add_quarter_extends_duration_and_clears_fired(void) {
  Timer t = mk_countdown(1200, 1000);  // 20:00
  t.fired = true;
  timer_add_quarter(&t, 2000);
  TEST_ASSERT_EQUAL_INT32(1500, t.duration_s);  // 20:00 + 5:00 = 25:00
  TEST_ASSERT_FALSE(t.fired);
}

void test_add_quarter_leaves_run_state_untouched(void) {
  // Extends the current run rather than restarting it -- started_at,
  // accumulated_pause_s, and cycles must be unaffected.
  Timer t = mk_countdown(1200, 1000);
  t.accumulated_pause_s = 7;
  t.cycles = 3;
  timer_add_quarter(&t, 9999);
  TEST_ASSERT_EQUAL(1000, t.started_at);
  TEST_ASSERT_EQUAL_INT32(7, t.accumulated_pause_s);
  TEST_ASSERT_EQUAL_INT32(3, t.cycles);
}

void test_add_quarter_on_small_duration_uses_integer_division(void) {
  // Documents the pure math for a range of durations, incl. small ones --
  // the >10s AVAILABILITY gate is enforced by the caller (adapter), not
  // this function itself (see timer_add_quarter's own doc comment).
  Timer t = mk_countdown(40, 1000);  // 40/4 = 10
  timer_add_quarter(&t, 1000);
  TEST_ASSERT_EQUAL_INT32(50, t.duration_s);

  Timer t2 = mk_countdown(3, 1000);  // 3/4 = 0 (integer division)
  timer_add_quarter(&t2, 1000);
  TEST_ASSERT_EQUAL_INT32(3, t2.duration_s);
}

void test_add_quarter_available_false_at_exactly_40s(void) {
  Timer t = mk_countdown(40, 1000);  // 40/4 = 10, NOT > 10
  TEST_ASSERT_FALSE(timer_add_quarter_available(&t));
}

void test_add_quarter_available_false_just_below_44s(void) {
  Timer t = mk_countdown(43, 1000);  // 43/4 = 10 (integer division), still gated off
  TEST_ASSERT_FALSE(timer_add_quarter_available(&t));
}

void test_add_quarter_available_true_at_44s(void) {
  Timer t = mk_countdown(44, 1000);  // 44/4 = 11 > 10 -- first available duration
  TEST_ASSERT_TRUE(timer_add_quarter_available(&t));
}

void test_add_quarter_available_true_for_long_duration(void) {
  Timer t = mk_countdown(1200, 1000);  // 20:00 -- comfortably available
  TEST_ASSERT_TRUE(timer_add_quarter_available(&t));
}

// At TIMER_MAX_DURATION_S, the quarter-amount is well above the >10s noise
// floor (359999/4=89999) but the mutation itself is a permanent no-op
// (never-shrink + saturation guards) -- available() must say so, hiding a
// gesture that can no longer do anything.
void test_add_quarter_available_false_at_the_dial_max(void) {
  Timer t = mk_countdown(359999, 1000);  // TIMER_MAX_DURATION_S, the dial's own ceiling
  TEST_ASSERT_FALSE(timer_add_quarter_available(&t));
}

void test_add_quarter_available_true_just_below_the_dial_max(void) {
  Timer t = mk_countdown(359998, 1000);  // one second short of the cap -- still effective
  TEST_ASSERT_TRUE(timer_add_quarter_available(&t));
}

// --- saturating cap ---------------------------------------------------------
// TIMER_MAX_DURATION_S is now derived from the SET dial's own ceiling
// (SET_MACHINE_MAX_HOURS=99h -> 359999s), not an independent 24h constant --
// see lib/timer.h's own doc comment.

void test_add_quarter_saturates_near_cap(void) {
  // 300000 + 300000/4 = 375000, which is > TIMER_MAX_DURATION_S (359999) --
  // must saturate at the cap, not land on the raw computed value. (Single
  // call, so the constant-vs-compounding distinction doesn't matter here --
  // original_total_s is lazily seeded to 300000 on this very call.)
  Timer t = mk_countdown(300000, 1000);
  timer_add_quarter(&t, 2000);
  TEST_ASSERT_EQUAL_INT32(359999, t.duration_s);
}

void test_add_quarter_at_cap_is_a_no_op_on_duration(void) {
  Timer t = mk_countdown(359999, 1000);
  timer_add_quarter(&t, 2000);
  TEST_ASSERT_EQUAL_INT32(359999, t.duration_s);
}

void test_add_quarter_no_overflow_across_many_repeated_holds(void) {
  // Regression for an int32_t overflow via compounding growth -- the growth
  // is now LINEAR (a constant 15s/call off a 60s base), so reaching the cap
  // takes many more calls than the old compounding scheme did; run
  // comfortably past that point and assert it lands exactly on the cap with
  // no UB/wraparound (a wrapped/overflowed value would go negative or
  // wildly large, neither of which equals TIMER_MAX_DURATION_S).
  Timer t = mk_countdown(60, 1000);
  for (int i = 0; i < 25000; i++) {
    timer_add_quarter(&t, 2000);
  }
  TEST_ASSERT_EQUAL_INT32(359999, t.duration_s);
  TEST_ASSERT_TRUE(t.duration_s > 0);
}

// --- never shrink a timer dialed above the cap -----------------------------
// The SET dial (lib/set_machine.c) has always allowed up to
// SET_MACHINE_MAX_HOURS=99h, well above the OLD, independent 24h cap -- a
// timer dialed in that gap used to get its duration SHRUNK by the very
// first add-¼ hold instead of grown. These tests dial ABOVE the cap
// (something the old test suite never did -- every prior saturation test
// started at or below it and grew INTO it) and pin the fixed, never-shrink
// behavior.

void test_add_quarter_duration_above_new_cap_still_never_shrinks(void) {
  // Defensive: even if duration_s somehow exceeds the (now dial-matched)
  // cap -- e.g. state persisted from before this fix landed -- the
  // never-shrink guard must still hold: the raw saturation clamp alone
  // would pull 500000 DOWN to the 359999 cap, a decrease; the guard must
  // override that and leave duration_s exactly where it started instead.
  Timer t = mk_countdown(400000, 1000);  // above TIMER_MAX_DURATION_S (359999)
  timer_add_quarter(&t, 2000);
  TEST_ASSERT_EQUAL_INT32(400000, t.duration_s);  // unchanged, never shrunk to the cap
}

void test_add_quarter_dialed_above_old_cap_grows_not_shrinks(void) {
  // The concrete repro: a 50h countdown (180000s), comfortably above
  // the OLD 24h/86400s cap that used to exist before this fix -- the old
  // code would have clamped 180000+45000=225000 DOWN to 86400, a ~26-hour
  // DECREASE from a gesture whose entire contract is "add time". Fixed
  // behavior: duration_s must only ever grow (or stay put once truly
  // capped), never fall below its pre-mutation value.
  Timer t = mk_countdown(180000, 1000);  // 50:00:00
  int32_t before = t.duration_s;
  timer_add_quarter(&t, 2000);
  TEST_ASSERT_TRUE(t.duration_s >= before);
  TEST_ASSERT_EQUAL_INT32(225000, t.duration_s);  // 180000 + 180000/4, well under the new cap
}

void test_add_quarter_near_new_cap_saturates_exactly(void) {
  // 350000 + 350000/4 = 437500, well past the 359999 cap -- must land
  // exactly on the cap, not overshoot or undershoot it.
  Timer t = mk_countdown(350000, 1000);
  timer_add_quarter(&t, 2000);
  TEST_ASSERT_EQUAL_INT32(359999, t.duration_s);
}

void test_add_quarter_available_false_for_zero_duration(void) {
  Timer t = mk_countdown(0, 1000);
  TEST_ASSERT_FALSE(timer_add_quarter_available(&t));
}

// --- quarter = the ORIGINAL set amount, not the current (possibly grown)
// duration_s -----------------------------------------------------------
// The ORIGINAL set amount is now the dedicated set_total_s field, read
// directly -- no more lazy "marker" caching into original_total_s (see
// lib/timer.h's own doc for the retirement).

void test_quarter_amount_countdown_reads_set_total_s(void) {
  Timer t = mk_countdown(1200, 1000);  // dialed 20:00 -- set_total_s=1200 (mk_countdown)
  TEST_ASSERT_EQUAL_INT32(300, timer_quarter_amount(&t));  // 1200/4
}

// The old lazy original_total_s marker no longer has any role in a
// countdown's quarter math, so this seeds set_total_s directly instead,
// simulating a countdown dialed at 20:00 and since grown (by some earlier
// add) to 25:00 -- quarter must still read the ORIGINAL 20:00, not the
// current 25:00.
void test_quarter_amount_countdown_reads_set_total_s_not_grown_duration(void) {
  Timer t = mk_countdown(1500, 1000);  // current (grown) duration
  t.set_total_s = 1200;                // the ORIGINAL dialed amount
  TEST_ASSERT_EQUAL_INT32(300, timer_quarter_amount(&t));  // 1200/4, NOT 1500/4=375
}

void test_add_quarter_is_constant_per_hold_not_compounding(void) {
  // 20:00 -> +5:00 -> 25:00 -> +5:00 -> 30:00 -> +5:00 -> 35:00 (linear).
  // The old compounding scheme would have produced 1200 -> 1500 -> 1875 ->
  // 2343 instead.
  Timer t = mk_countdown(1200, 1000);
  timer_add_quarter(&t, 2000);
  TEST_ASSERT_EQUAL_INT32(1500, t.duration_s);
  timer_add_quarter(&t, 2000);
  TEST_ASSERT_EQUAL_INT32(1800, t.duration_s);
  timer_add_quarter(&t, 2000);
  TEST_ASSERT_EQUAL_INT32(2100, t.duration_s);
}

// add_quarter() no longer caches anything into original_total_s (that
// caching role is retired); this pins the invariant: set_total_s stays
// exactly as it was (the stable base timer_quarter_amount() reads), and
// original_total_s is never touched at all for a countdown.
void test_add_quarter_does_not_touch_set_total_s_or_original_total_s(void) {
  Timer t = mk_countdown(1200, 1000);
  TEST_ASSERT_EQUAL_INT32(0, t.original_total_s);
  timer_add_quarter(&t, 2000);
  TEST_ASSERT_EQUAL_INT32(1200, t.set_total_s);       // unchanged
  TEST_ASSERT_EQUAL_INT32(0, t.original_total_s);     // never written
  timer_add_quarter(&t, 2000);
  TEST_ASSERT_EQUAL_INT32(1200, t.set_total_s);       // still unchanged by a 2nd call
  TEST_ASSERT_EQUAL_INT32(0, t.original_total_s);
}

// The gate must read the ORIGINAL set amount, not the grown current
// duration, via set_total_s directly instead of the retired
// original_total_s marker.
void test_add_quarter_available_uses_set_total_s_not_grown_duration(void) {
  // Current duration_s alone would say "available" (1500/4=375>10), but the
  // ORIGINAL set amount says otherwise (40/4=10, NOT > 10) -- the gate must
  // read set_total_s, matching the mutation.
  Timer t = mk_countdown(1500, 1000);
  t.set_total_s = 40;
  TEST_ASSERT_FALSE(timer_add_quarter_available(&t));
}

// Same reverse case, via set_total_s.
void test_add_quarter_available_true_via_set_total_s_even_if_current_tiny(void) {
  // The reverse: current duration_s alone would say "NOT available"
  // (5/4=1), but the ORIGINAL set amount says it is (1200/4=300>10).
  Timer t = mk_countdown(5, 1000);
  t.set_total_s = 1200;
  TEST_ASSERT_TRUE(timer_add_quarter_available(&t));
}

// add_quarter() no longer seeds original_total_s, so this seeds it directly
// (simulating a stray/legacy nonzero value) to keep pinning snooze's
// unconditional "always clear original_total_s" contract. set_total_s is
// untouched by snooze (a transient adjustment) -- see timer_snooze()'s own
// header doc.
void test_snooze_clears_original_total_s_leaves_set_total_s_untouched(void) {
  Timer t = mk_countdown(1200, 1000);
  t.original_total_s = 1200;  // synthetic stray/legacy value
  timer_snooze(&t, 5, 3000);
  TEST_ASSERT_EQUAL_INT32(0, t.original_total_s);
  TEST_ASSERT_EQUAL_INT32(1200, t.set_total_s);  // pre-snooze SET amount preserved
}

// Same change as the fixed-minute snooze above.
void test_snooze_seconds_clears_original_total_s_leaves_set_total_s_untouched(void) {
  Timer t = mk_countdown(1200, 1000);
  t.original_total_s = 1200;  // synthetic stray/legacy value
  timer_snooze_seconds(&t, 75, 3000);
  TEST_ASSERT_EQUAL_INT32(0, t.original_total_s);
  TEST_ASSERT_EQUAL_INT32(1200, t.set_total_s);
}

// Seeds original_total_s directly rather than via the retired add_quarter
// lazy-cache side effect.
void test_toggle_repeat_clears_original_total_s_and_snooze_marker(void) {
  // countdown -> repeat: a stale original_total_s value must not leak in as
  // a false "snoozing" marker.
  Timer t = mk_countdown(1200, 1000);
  t.original_total_s = 1200;  // synthetic stray/legacy value
  timer_toggle_repeat(&t, 3000);
  TEST_ASSERT_EQUAL(KIND_REPEAT, t.kind);
  TEST_ASSERT_EQUAL_INT32(0, t.original_total_s);
  TEST_ASSERT_FALSE(timer_is_snoozing(&t));
}

void test_edit_interval_ends_snooze_window(void) {
  Timer t = mk_countdown(300, 1000);
  t.kind = KIND_REPEAT;
  timer_snooze_repeat(&t, 2000);
  TEST_ASSERT_TRUE(timer_is_snoozing(&t));
  timer_edit_interval(&t, 120, 3000);
  TEST_ASSERT_FALSE(timer_is_snoozing(&t));
}

// --- repeat snooze ---------------------------------------------------------

void test_snooze_repeat_remaining_becomes_quarter_stays_repeat_cycles_unchanged(void) {
  Timer t = mk_countdown(300, 1000);  // 5:00 interval
  t.kind = KIND_REPEAT;
  t.cycles = 3;
  t.fired = true;
  timer_snooze_repeat(&t, 5000);
  TEST_ASSERT_EQUAL(KIND_REPEAT, t.kind);
  TEST_ASSERT_EQUAL_INT32(3, t.cycles);  // unchanged
  TEST_ASSERT_FALSE(t.fired);
  TEST_ASSERT_FALSE(t.is_paused);
  TEST_ASSERT_EQUAL_INT32(0, t.accumulated_pause_s);
  TEST_ASSERT_EQUAL_INT32(75, timer_remaining_s(&t, 5000));  // 300/4 = 75 = 1:15
}

void test_snooze_repeat_sets_snoozing_marker(void) {
  Timer t = mk_countdown(300, 1000);
  t.kind = KIND_REPEAT;
  TEST_ASSERT_FALSE(timer_is_snoozing(&t));
  timer_snooze_repeat(&t, 5000);
  TEST_ASSERT_TRUE(timer_is_snoozing(&t));
}

void test_snooze_repeat_non_destructive_next_cycle_clears_marker(void) {
  Timer t = mk_countdown(300, 1000);
  t.kind = KIND_REPEAT;
  t.cycles = 3;
  timer_snooze_repeat(&t, 5000);
  TEST_ASSERT_TRUE(timer_is_snoozing(&t));
  // The next NATURAL re-arm ("Keep going") ends the snooze window.
  timer_advance_cycle(&t, 5100);
  TEST_ASSERT_FALSE(timer_is_snoozing(&t));
  TEST_ASSERT_EQUAL_INT32(4, t.cycles);
  TEST_ASSERT_EQUAL_INT32(300, timer_remaining_s(&t, 5100));  // fresh full interval
}

// Snoozing a PAUSED repeat is unreachable via the real UI today (both the
// Up-hold gesture and the menu row live only on the
// fired/ringing alarm surface, and timer_is_firing() excludes a paused
// timer from ever firing -- the alarm screen offers no pause control) --
// this pins the otherwise-implicit "safe if it ever weren't unreachable"
// property: even called directly on a paused timer, the function forces
// is_paused=false/accumulated_pause_s=0 (its existing unconditional
// contract), producing sane output rather than a negative remaining or a
// stale pause bleeding through.
void test_snooze_repeat_on_paused_timer_clears_pause_state(void) {
  Timer t = mk_countdown(300, 1000);  // 5:00 interval
  t.kind = KIND_REPEAT;
  t.is_paused = true;
  t.paused_at = 4000;
  t.accumulated_pause_s = 50;
  timer_snooze_repeat(&t, 5000);
  TEST_ASSERT_FALSE(t.is_paused);
  TEST_ASSERT_EQUAL_INT32(0, t.accumulated_pause_s);
  TEST_ASSERT_EQUAL_INT32(75, timer_remaining_s(&t, 5000));  // 300/4 = 75, no stale pause bleed-through
}

void test_snooze_seconds_converts_to_countdown(void) {
  Timer t = mk_countdown(300, 1000);
  t.kind = KIND_REPEAT;
  t.fired = true;
  timer_snooze_seconds(&t, 75, 5000);
  TEST_ASSERT_EQUAL(KIND_COUNTDOWN, t.kind);
  TEST_ASSERT_EQUAL_INT32(75, t.duration_s);
  TEST_ASSERT_EQUAL(5000, t.started_at);
  TEST_ASSERT_FALSE(t.fired);
}

void test_is_snoozing_false_for_normal_running_repeat(void) {
  Timer t = mk_countdown(300, 1000);
  t.kind = KIND_REPEAT;
  t.cycles = 2;
  TEST_ASSERT_FALSE(timer_is_snoozing(&t));
}

void test_is_snoozing_false_for_non_repeat_even_with_marker_set(void) {
  // Guards against the snooze-marker reuse being confused with a post-timer
  // stopwatch's OWN, unrelated use of original_total_s (timer_keep_counting).
  Timer t = {0};
  t.kind = KIND_STOPWATCH;
  t.original_total_s = 900;
  TEST_ASSERT_FALSE(timer_is_snoozing(&t));
}

// timer_quarter_amount() must read a repeat's duration_s DIRECTLY, never its
// original_total_s (the snooze marker) --
// deliberately mismatch the two fields (something that can't happen via the
// real UI today, since timer_edit_interval is the only writer of a repeat's
// duration_s and always clears the marker in the same call) to pin the
// hardened, by-construction behavior rather than relying on that "one
// writer" coincidence.
void test_quarter_amount_repeat_reads_duration_directly_even_if_marker_mismatched(void) {
  Timer t = mk_countdown(400, 1000);  // interval == 400s
  t.kind = KIND_REPEAT;
  t.original_total_s = 4000;  // deliberately mismatched marker (would give 1000 if used)
  TEST_ASSERT_EQUAL_INT32(100, timer_quarter_amount(&t));  // 400/4, NOT 4000/4=1000
}

void test_add_quarter_available_repeat_reads_duration_directly_even_if_marker_mismatched(void) {
  Timer t = mk_countdown(20, 1000);  // 20/4=5, NOT > 10 -- gate should read this
  t.kind = KIND_REPEAT;
  t.original_total_s = 4000;  // mismatched marker would say available (4000/4=1000>10)
  TEST_ASSERT_FALSE(timer_add_quarter_available(&t));
}

void test_add_lap_appends_current_elapsed(void) {
  TimerStore s;
  store_init(&s);
  int32_t cookie;
  store_add(&s, KIND_STOPWATCH, 0, 1000, &cookie);
  Timer *t = store_find(&s, cookie);
  store_add_lap(&s, t, 1030);
  store_add_lap(&s, t, 1075);
  TEST_ASSERT_EQUAL_UINT8(2, t->lap_count);
  int32_t lap;
  TEST_ASSERT_TRUE(store_lap_get(&s, t, 0, &lap));
  TEST_ASSERT_EQUAL_INT32(30, lap);
  TEST_ASSERT_TRUE(store_lap_get(&s, t, 1, &lap));
  TEST_ASSERT_EQUAL_INT32(75, lap);
}

void test_add_lap_refuses_past_capacity(void) {
  TimerStore s;
  store_init(&s);
  int32_t cookie;
  store_add(&s, KIND_STOPWATCH, 0, 0, &cookie);
  Timer *t = store_find(&s, cookie);
  for (int i = 0; i < MAX_LAPS; i++) {
    TEST_ASSERT_TRUE(store_add_lap(&s, t, (time_t)(i + 1)));
  }
  TEST_ASSERT_EQUAL_UINT8(MAX_LAPS, t->lap_count);
  TEST_ASSERT_FALSE(store_add_lap(&s, t, 999));
  TEST_ASSERT_EQUAL_UINT8(MAX_LAPS, t->lap_count);
}

void test_keep_counting_can_record_a_lap(void) {
  TimerStore s;
  store_init(&s);
  int32_t cookie;
  store_add(&s, KIND_COUNTDOWN, 60, 1000, &cookie);
  Timer *t = store_find(&s, cookie);
  timer_keep_counting(t, 1060);
  TEST_ASSERT_TRUE(store_add_lap(&s, t, 1070));
  TEST_ASSERT_EQUAL_UINT8(1, t->lap_count);
}

// --- timer_name_copy -------------------------------------------------------

void test_name_copy_basic(void) {
  char dst[TIMER_NAME_MAX];
  timer_name_copy(dst, sizeof dst, "Tea");
  TEST_ASSERT_EQUAL_STRING("Tea", dst);
}

void test_name_copy_null_src_is_name_less(void) {
  char dst[TIMER_NAME_MAX];
  timer_name_copy(dst, sizeof dst, NULL);
  TEST_ASSERT_EQUAL_STRING("", dst);
}

void test_name_copy_empty_src_is_name_less(void) {
  char dst[TIMER_NAME_MAX];
  timer_name_copy(dst, sizeof dst, "");
  TEST_ASSERT_EQUAL_STRING("", dst);
}

void test_name_copy_trims_leading_and_trailing_ascii_whitespace(void) {
  char dst[TIMER_NAME_MAX];
  timer_name_copy(dst, sizeof dst, "  Tea  ");
  TEST_ASSERT_EQUAL_STRING("Tea", dst);
}

void test_name_copy_all_whitespace_src_is_name_less(void) {
  char dst[TIMER_NAME_MAX];
  timer_name_copy(dst, sizeof dst, "   \t  ");
  TEST_ASSERT_EQUAL_STRING("", dst);
}

void test_name_copy_preserves_interior_whitespace(void) {
  char dst[32];
  timer_name_copy(dst, sizeof dst, "Pomodoro Focus");
  TEST_ASSERT_EQUAL_STRING("Pomodoro Focus", dst);
}

void test_name_copy_does_not_accent_fold(void) {
  // A display name is stored raw UTF-8, unlike the parser's own
  // matching-only accent fold -- "Té" must round-trip unchanged.
  char dst[TIMER_NAME_MAX];
  timer_name_copy(dst, sizeof dst, "T\xc3\xa9");  // "Té", 2-byte UTF-8 e-acute
  TEST_ASSERT_EQUAL_STRING("T\xc3\xa9", dst);
}

void test_name_copy_caps_at_dst_size_minus_one(void) {
  char dst[8];  // room for 7 chars + NUL
  timer_name_copy(dst, sizeof dst, "abcdefghij");
  TEST_ASSERT_EQUAL_STRING("abcdefg", dst);
}

void test_name_copy_utf8_safe_does_not_split_a_multibyte_char(void) {
  // "café" = c,a,f,é(0xC3 0xA9) -- 5 bytes total. A dst that only leaves
  // room for the FIRST byte of the trailing 2-byte 'é' sequence must drop
  // that whole codepoint, not emit a dangling lead byte.
  char dst[5];  // cap=4: "caf" (3 bytes) fits, the 2-byte 'é' doesn't -> dropped whole
  timer_name_copy(dst, sizeof dst, "caf\xc3\xa9");
  TEST_ASSERT_EQUAL_STRING("caf", dst);
  // A dst with exactly enough room keeps the full codepoint.
  char dst2[6];  // cap=5: all 5 bytes fit
  timer_name_copy(dst2, sizeof dst2, "caf\xc3\xa9");
  TEST_ASSERT_EQUAL_STRING("caf\xc3\xa9", dst2);
}

void test_name_copy_zeroes_stale_bytes_past_a_shorter_new_name(void) {
  char dst[TIMER_NAME_MAX];
  timer_name_copy(dst, sizeof dst, "Pomodoro Focus");
  timer_name_copy(dst, sizeof dst, "Tea");  // shorter -- must not leave "doro Focus" leftovers
  for (size_t i = strlen(dst) + 1; i < sizeof dst; i++) {
    TEST_ASSERT_EQUAL_UINT8(0, (uint8_t)dst[i]);
  }
}

// --- name lifecycle ("persists across every run-state mutation, cleared
// only on delete" -- store_remove()'s own contract, not this file's) --
// spot-checks the mutation functions THIS file owns.

void test_name_persists_across_restart(void) {
  Timer t = mk_countdown(300, 1000);
  strncpy(t.name, "Tea", sizeof t.name - 1);
  timer_restart(&t, 5000);
  TEST_ASSERT_EQUAL_STRING("Tea", t.name);
}

void test_name_persists_across_snooze(void) {
  Timer t = mk_countdown(300, 1000);
  strncpy(t.name, "Tea", sizeof t.name - 1);
  timer_snooze(&t, 5, 5000);
  TEST_ASSERT_EQUAL_STRING("Tea", t.name);
}

void test_name_persists_across_keep_counting(void) {
  Timer t = mk_countdown(300, 1000);
  strncpy(t.name, "Tea", sizeof t.name - 1);
  timer_keep_counting(&t, 5000);
  TEST_ASSERT_EQUAL_STRING("Tea", t.name);
}

void test_name_persists_across_toggle_repeat(void) {
  Timer t = mk_countdown(300, 1000);
  strncpy(t.name, "Tea", sizeof t.name - 1);
  timer_toggle_repeat(&t, 5000);
  TEST_ASSERT_EQUAL_STRING("Tea", t.name);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_effective_elapsed_running);
  RUN_TEST(test_effective_elapsed_clamped_at_zero);
  RUN_TEST(test_effective_elapsed_subtracts_pause_accum);
  RUN_TEST(test_effective_elapsed_while_paused_uses_paused_at);
  RUN_TEST(test_remaining_can_go_negative);
  RUN_TEST(test_overrun_zero_while_not_overrun);
  RUN_TEST(test_overrun_positive_past_deadline);
  RUN_TEST(test_elapsed_stopwatch);
  RUN_TEST(test_deadline);
  RUN_TEST(test_is_firing_true_at_boundary);
  RUN_TEST(test_is_firing_false_before_boundary);
  RUN_TEST(test_is_firing_false_if_already_fired);
  RUN_TEST(test_is_firing_false_if_paused);
  RUN_TEST(test_is_firing_false_for_stopwatch);
  RUN_TEST(test_progress_pct_clamped);
  RUN_TEST(test_pause_sets_paused_at_once);
  RUN_TEST(test_pause_idempotent);
  RUN_TEST(test_resume_accumulates_pause_and_clears_flag);
  RUN_TEST(test_resume_no_op_when_not_paused);
  RUN_TEST(test_pause_then_resume_preserves_remaining);
  RUN_TEST(test_add_minute_extends_duration_and_clears_fired);
  RUN_TEST(test_add_minute_on_fired_rearms);
  RUN_TEST(test_restart_resets_everything);
  RUN_TEST(test_restart_countdown_leaves_original_total_s_untouched);
  RUN_TEST(test_restart_countdown_restores_set_total_s_after_add_minute);
  RUN_TEST(test_restart_countdown_restores_set_total_s_after_add_quarter);
  RUN_TEST(test_restart_immediately_after_create_is_identity);
  RUN_TEST(test_restart_after_snooze_restores_pre_snooze_set_total_s);
  RUN_TEST(test_restart_after_snooze_seconds_restores_pre_snooze_set_total_s);
  RUN_TEST(test_restart_after_edit_interval_uses_new_interval_as_set_total_s);
  RUN_TEST(test_restart_stopwatch_preserves_provenance);
  RUN_TEST(test_restart_repeat_resets_cycles_and_stays_repeat);
  RUN_TEST(test_restart_repeat_clears_snoozing_marker);
  RUN_TEST(test_snooze_creates_fresh_countdown);
  RUN_TEST(test_keep_counting_becomes_stopwatch_with_fired_false);
  RUN_TEST(test_keep_counting_elapsed_continues_seamlessly_through_zero);
  RUN_TEST(test_keep_counting_never_reports_firing);
  RUN_TEST(test_advance_cycle_increments_and_resets);
  RUN_TEST(test_toggle_repeat_countdown_to_repeat_preserves_elapsed_and_deadline);
  RUN_TEST(test_toggle_repeat_back_to_countdown);
  RUN_TEST(test_toggle_repeat_repeat_to_countdown_preserves_elapsed_mid_cycle);
  RUN_TEST(test_toggle_repeat_preserves_overtime_elapsed_may_immediately_refire);
  RUN_TEST(test_edit_interval_resets_clock_keeps_cycles);
  RUN_TEST(test_add_quarter_extends_duration_and_clears_fired);
  RUN_TEST(test_add_quarter_leaves_run_state_untouched);
  RUN_TEST(test_add_quarter_on_small_duration_uses_integer_division);
  RUN_TEST(test_add_quarter_available_false_at_exactly_40s);
  RUN_TEST(test_add_quarter_available_false_just_below_44s);
  RUN_TEST(test_add_quarter_available_true_at_44s);
  RUN_TEST(test_add_quarter_available_true_for_long_duration);
  RUN_TEST(test_add_quarter_available_false_at_the_dial_max);
  RUN_TEST(test_add_quarter_available_true_just_below_the_dial_max);
  RUN_TEST(test_add_quarter_saturates_near_cap);
  RUN_TEST(test_add_quarter_at_cap_is_a_no_op_on_duration);
  RUN_TEST(test_add_quarter_no_overflow_across_many_repeated_holds);
  RUN_TEST(test_add_quarter_duration_above_new_cap_still_never_shrinks);
  RUN_TEST(test_add_quarter_dialed_above_old_cap_grows_not_shrinks);
  RUN_TEST(test_add_quarter_near_new_cap_saturates_exactly);
  RUN_TEST(test_add_quarter_available_false_for_zero_duration);
  RUN_TEST(test_quarter_amount_countdown_reads_set_total_s);
  RUN_TEST(test_quarter_amount_countdown_reads_set_total_s_not_grown_duration);
  RUN_TEST(test_add_quarter_is_constant_per_hold_not_compounding);
  RUN_TEST(test_add_quarter_does_not_touch_set_total_s_or_original_total_s);
  RUN_TEST(test_add_quarter_available_uses_set_total_s_not_grown_duration);
  RUN_TEST(test_add_quarter_available_true_via_set_total_s_even_if_current_tiny);
  RUN_TEST(test_snooze_clears_original_total_s_leaves_set_total_s_untouched);
  RUN_TEST(test_snooze_seconds_clears_original_total_s_leaves_set_total_s_untouched);
  RUN_TEST(test_toggle_repeat_clears_original_total_s_and_snooze_marker);
  RUN_TEST(test_edit_interval_ends_snooze_window);
  RUN_TEST(test_snooze_repeat_remaining_becomes_quarter_stays_repeat_cycles_unchanged);
  RUN_TEST(test_snooze_repeat_sets_snoozing_marker);
  RUN_TEST(test_snooze_repeat_non_destructive_next_cycle_clears_marker);
  RUN_TEST(test_snooze_repeat_on_paused_timer_clears_pause_state);
  RUN_TEST(test_snooze_seconds_converts_to_countdown);
  RUN_TEST(test_is_snoozing_false_for_normal_running_repeat);
  RUN_TEST(test_is_snoozing_false_for_non_repeat_even_with_marker_set);
  RUN_TEST(test_quarter_amount_repeat_reads_duration_directly_even_if_marker_mismatched);
  RUN_TEST(test_add_quarter_available_repeat_reads_duration_directly_even_if_marker_mismatched);
  RUN_TEST(test_add_lap_appends_current_elapsed);
  RUN_TEST(test_add_lap_refuses_past_capacity);
  RUN_TEST(test_keep_counting_can_record_a_lap);
  RUN_TEST(test_name_copy_basic);
  RUN_TEST(test_name_copy_null_src_is_name_less);
  RUN_TEST(test_name_copy_empty_src_is_name_less);
  RUN_TEST(test_name_copy_trims_leading_and_trailing_ascii_whitespace);
  RUN_TEST(test_name_copy_all_whitespace_src_is_name_less);
  RUN_TEST(test_name_copy_preserves_interior_whitespace);
  RUN_TEST(test_name_copy_does_not_accent_fold);
  RUN_TEST(test_name_copy_caps_at_dst_size_minus_one);
  RUN_TEST(test_name_copy_utf8_safe_does_not_split_a_multibyte_char);
  RUN_TEST(test_name_copy_zeroes_stale_bytes_past_a_shorter_new_name);
  RUN_TEST(test_name_persists_across_restart);
  RUN_TEST(test_name_persists_across_snooze);
  RUN_TEST(test_name_persists_across_keep_counting);
  RUN_TEST(test_name_persists_across_toggle_repeat);
  return UNITY_END();
}
