// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// hold.h documents that on_fire runs "either via the fire AppTimer or --
// deterministically -- an up event whose measured duration already exceeds
// HOLD_MS." Exercises hold.c against FFF fakes of the tiny click-config/
// AppTimer surface it calls (test/fakes/pebble.h), compiled via
// test/Makefile's dedicated hold_test rule (the second adapter file, after
// testlog_am.c, let into the host test build for exactly this reason).
//
// hold.c derives every elapsed-time decision from counting actual AppTimer
// poll firings, never from wall-clock time_ms() reads, since time_ms() is
// not guaranteed monotonic on device. These tests drive that directly:
// `fire_last_timer()` invokes whichever AppTimer callback was most recently
// registered (fire timer, fill timer, or a SET repeat poll -- each test only
// exercises one mechanism at a time, so "most recent registration" is
// unambiguous), simulating a real firmware event loop dispatching it. The
// fake wall clock (`g_now_sec`/`g_now_ms`) is kept only for the handful of
// assertions that deliberately prove immunity to a jumping clock -- it is
// not load-bearing for the ordinary-path assertions.
#include "hold.h"

#include "fff.h"
#include "lib/set_accel.h"
#include "unity.h"

DEFINE_FFF_GLOBALS;

FAKE_VALUE_FUNC(AppTimer *, app_timer_register, uint32_t, AppTimerCallback, void *);
FAKE_VALUE_FUNC(bool, app_timer_cancel, AppTimer *);
FAKE_VOID_FUNC(window_raw_click_subscribe, ButtonId, ClickHandler, ClickHandler, void *);
FAKE_VOID_FUNC(window_single_repeating_click_subscribe, ButtonId, uint16_t, ClickHandler);
FAKE_VALUE_FUNC(ButtonId, click_recognizer_get_button_id, ClickRecognizerRef);
FAKE_VALUE_FUNC(uint8_t, click_number_of_clicks_counted, ClickRecognizerRef);
FAKE_VALUE_FUNC(Layer *, window_get_root_layer, Window *);
FAKE_VOID_FUNC(layer_mark_dirty, Layer *);
FAKE_VOID_FUNC(time_ms, time_t *, uint16_t *);
FAKE_VOID_FUNC(vibes_adapter_confirm);
FAKE_VOID_FUNC(on_fire_spy);
FAKE_VOID_FUNC(on_step_spy, int);
// A second hold-bound button gets its own on_long spy (distinct from
// on_fire_spy, which stays SELECT's); on_short ("early release") is a spy
// shared across whichever button a test binds it to.
FAKE_VOID_FUNC(on_fire_up_spy);
FAKE_VOID_FUNC(on_short_spy);

// Test-controlled "wall clock" the time_ms() fake reports. Only load-bearing
// for the "s_held_since_ms != 0" sentinel now (hold.c never differences two
// reads of it for a decision) plus the explicit clock-jump-immunity tests
// below, which deliberately jerk it around to prove it no longer matters.
static time_t g_now_sec;
static uint16_t g_now_ms;

static void time_ms_custom_fake(time_t *tick_time, uint16_t *out_ms) {
  *tick_time = g_now_sec;
  *out_ms = g_now_ms;
}

static void set_now_ms(uint32_t total_ms) {
  g_now_sec = (time_t)(total_ms / 1000u);
  g_now_ms = (uint16_t)(total_ms % 1000u);
}

// A nonzero baseline -- now_ms() == 0 collides with hold.c's own
// "not currently holding" sentinel (s_held_since_ms == 0), a real-but-
// irrelevant epoch-zero edge case this constant simply avoids.
#define BASE_MS 100000u

static ClickHandler s_down_handler;
static ClickHandler s_up_handler;

// Invokes whichever AppTimer was most recently registered -- the standard
// way these tests simulate "time passes and the firmware dispatches the
// pending AppTimer callback" without any real sleeping. A callback that
// reschedules itself (fill_timer_cb / repeat_poll_cb, both while still held)
// becomes the new "most recent" registration, so calling this in a loop
// correctly simulates N sequential firings.
static void fire_last_timer(void) {
  TEST_ASSERT_TRUE(app_timer_register_fake.call_count > 0);
  unsigned idx = app_timer_register_fake.call_count - 1;
  AppTimerCallback cb = app_timer_register_fake.arg1_history[idx];
  void *data = app_timer_register_fake.arg2_history[idx];
  TEST_ASSERT_NOT_NULL(cb);
  cb(data);
}

static void fire_last_timer_n(int n) {
  for (int i = 0; i < n; i++) fire_last_timer();
}

void setUp(void) {
  RESET_FAKE(app_timer_register);
  RESET_FAKE(app_timer_cancel);
  RESET_FAKE(window_raw_click_subscribe);
  RESET_FAKE(window_single_repeating_click_subscribe);
  RESET_FAKE(click_recognizer_get_button_id);
  RESET_FAKE(click_number_of_clicks_counted);
  RESET_FAKE(window_get_root_layer);
  RESET_FAKE(layer_mark_dirty);
  RESET_FAKE(time_ms);
  RESET_FAKE(vibes_adapter_confirm);
  RESET_FAKE(on_fire_spy);
  RESET_FAKE(on_step_spy);
  RESET_FAKE(on_fire_up_spy);
  RESET_FAKE(on_short_spy);
  FFF_RESET_HISTORY();

  time_ms_fake.custom_fake = time_ms_custom_fake;
  set_now_ms(BASE_MS);
  // A non-NULL dummy handle -- reset_hold_state()'s app_timer_cancel() calls
  // are guarded on "!= NULL", so a real handle is needed for those calls to
  // actually happen (never dereferenced, only compared/passed through).
  app_timer_register_fake.return_val = (AppTimer *)1;

  // hold.c's raw_down_handler now identifies WHICH button fired via
  // click_recognizer_get_button_id(recognizer) (previously unused -- this
  // file only ever bound one button). Every existing test predating that
  // change drives hold_bind_select's SELECT binding via
  // s_down_handler(NULL, NULL), so a fixed default of BUTTON_ID_SELECT here
  // reproduces that behavior exactly; tests exercising a second bound button
  // override this before invoking that button's handlers.
  click_recognizer_get_button_id_fake.return_val = BUTTON_ID_SELECT;

  // hold_bind_select must run inside a click config provider per hold.h's
  // contract; NULL is a fine stand-in Window* here -- hold.c only compares
  // it against NULL before touching it (skips window_get_root_layer/
  // layer_mark_dirty, which these tests don't need to exercise).
  hold_bind_select(NULL, on_fire_spy);
  TEST_ASSERT_EQUAL_INT(1, window_raw_click_subscribe_fake.call_count);
  s_down_handler = window_raw_click_subscribe_fake.arg1_val;
  s_up_handler = window_raw_click_subscribe_fake.arg2_val;
  TEST_ASSERT_NOT_NULL(s_down_handler);
  TEST_ASSERT_NOT_NULL(s_up_handler);
}

void tearDown(void) {}

// --- pre-existing behaviour: release before HOLD_MS cancels, no fire -------

void test_release_before_threshold_cancels_without_firing(void) {
  s_down_handler(NULL, NULL);
  // No fill polls fired -- an ordinary short tap, released well before any
  // poll would even reach the threshold.
  s_up_handler(NULL, NULL);

  TEST_ASSERT_EQUAL_INT(0, on_fire_spy_fake.call_count);
  TEST_ASSERT_EQUAL_INT(0, vibes_adapter_confirm_fake.call_count);
  TEST_ASSERT_FALSE(hold_is_active());
}

// --- release AT/PAST the threshold fires deterministically -----------------
// HOLD_MS=700, FILL_REDRAW_MS=50 -> 14 fill polls reach exactly 700ms.

void test_release_past_threshold_fires_even_though_fire_apptimer_never_dispatched(void) {
  s_down_handler(NULL, NULL);
  // Simulates the fill AppTimer having fired 15 times (750ms of real elapsed
  // press time) while the PRIMARY fire AppTimer (700ms, registered
  // separately on down) has NOT yet been dispatched -- the physical release
  // winning the race against the fire timer's own dispatch.
  fire_last_timer_n(15);
  s_up_handler(NULL, NULL);

  TEST_ASSERT_EQUAL_INT(1, on_fire_spy_fake.call_count);
  TEST_ASSERT_EQUAL_INT(1, vibes_adapter_confirm_fake.call_count);  // confirm pulse
  TEST_ASSERT_FALSE(hold_is_active());
}

void test_release_exactly_at_threshold_fires(void) {
  // hold.h's contract is ">= HOLD_MS", not "> HOLD_MS" -- the boundary itself
  // must fire, not cancel. 14 polls * 50ms = exactly 700ms.
  s_down_handler(NULL, NULL);
  fire_last_timer_n(14);
  s_up_handler(NULL, NULL);

  TEST_ASSERT_EQUAL_INT(1, on_fire_spy_fake.call_count);
}

void test_release_one_poll_before_threshold_does_not_fire(void) {
  // 13 polls * 50ms = 650ms, still short of HOLD_MS -- must cancel, matching
  // an ordinary release that happened just shy of the threshold.
  s_down_handler(NULL, NULL);
  fire_last_timer_n(13);
  s_up_handler(NULL, NULL);

  TEST_ASSERT_EQUAL_INT(0, on_fire_spy_fake.call_count);
  TEST_ASSERT_FALSE(hold_is_active());
}

void test_fire_on_release_cancels_the_now_redundant_pending_timers(void) {
  // Firing directly from the up-handler must still tear down the
  // fire/fill AppTimers that were registered on press -- otherwise a stale
  // AppTimer could fire a SECOND time later against already-reset state.
  s_down_handler(NULL, NULL);
  TEST_ASSERT_EQUAL_INT(2, app_timer_register_fake.call_count);  // fire timer + fill timer

  fire_last_timer_n(14);
  s_up_handler(NULL, NULL);

  // fill_timer_cb stops rescheduling itself once its own monotonic estimate
  // reaches HOLD_MS (the 14th poll, elapsed_est=700, is the one that crosses
  // the threshold AND the one that leaves s_fill_timer NULL rather than
  // re-arming it) -- so by the time the up-handler's threshold check fires,
  // only the original fire_timer (armed on down, never dispatched, never
  // cancelled yet) is still outstanding. reset_hold_state() cancels it --
  // 1 cancel, not 2, since there is never a separately-pending fill timer
  // left to also cancel once the fill chain has stopped rescheduling itself.
  TEST_ASSERT_EQUAL_INT(1, app_timer_cancel_fake.call_count);
}

void test_a_subsequent_press_after_a_threshold_release_fire_works_normally(void) {
  // The threshold-release fire path must leave hold.c's state exactly as
  // clean as a normal AppTimer-driven fire -- a fresh press right after
  // must behave like any other fresh press, not carry over stale state.
  s_down_handler(NULL, NULL);
  fire_last_timer_n(14);
  s_up_handler(NULL, NULL);
  TEST_ASSERT_EQUAL_INT(1, on_fire_spy_fake.call_count);

  s_down_handler(NULL, NULL);
  // Released well before HOLD_MS elapses again -- no polls fired at all.
  s_up_handler(NULL, NULL);

  TEST_ASSERT_EQUAL_INT(1, on_fire_spy_fake.call_count);  // still just the one fire
  TEST_ASSERT_FALSE(hold_is_active());
}

// --- the threshold decision is immune to a jumping wall clock --------------
// raw_up_handler must never derive its fire/cancel decision by comparing
// now_ms() at down-time against now_ms() at up-time. This test asserts a
// wildly jumped wall clock, with NO fill polls having actually fired, must
// NOT fire the hold -- an ordinary short tap must never trigger a hold-gated
// action (e.g. alarm "Done") no matter what time_ms() claims.

void test_wall_clock_jump_with_no_fill_polls_does_not_fire(void) {
  s_down_handler(NULL, NULL);
  // Simulates exactly the non-monotonic jump proven in the real environment
  // (an elapsed reading that jumped forward by >HOLD_MS between two
  // notionally-close events) -- but since NO fill poll has actually fired,
  // the monotonic estimate is still 0.
  set_now_ms(BASE_MS + HOLD_MS + 5000);
  s_up_handler(NULL, NULL);

  TEST_ASSERT_EQUAL_INT(0, on_fire_spy_fake.call_count);
  TEST_ASSERT_FALSE(hold_is_active());
}

void test_wall_clock_jump_during_a_short_tap_does_not_fire(void) {
  // A more realistic shape: a handful of fill polls fire (a normal short
  // tap held maybe 100-150ms), but the wall clock itself jumps wildly
  // between polls -- the fill-poll-count estimate must still reflect only
  // the REAL number of 50ms quanta that elapsed (3 * 50 = 150ms), not
  // whatever time_ms() claims.
  s_down_handler(NULL, NULL);
  set_now_ms(BASE_MS + 900000);  // absurd forward jump
  fire_last_timer();
  set_now_ms(BASE_MS - 500000);  // absurd backward jump
  fire_last_timer();
  set_now_ms(BASE_MS + 1);
  fire_last_timer();
  s_up_handler(NULL, NULL);

  TEST_ASSERT_EQUAL_INT(0, on_fire_spy_fake.call_count);
  TEST_ASSERT_FALSE(hold_is_active());
}

// --- rebind-cancel (avoiding a latent resource leak) ------------------------
//
// Both binders must app_timer_cancel any pending timer from a prior binding
// (s_select_window/s_on_fire, s_repeat_timer[]) before adopting a new one --
// otherwise a stale AppTimer would still fire later, invoking whichever
// callback happens to be bound by then (a phantom fire against the wrong
// window's state).

void test_rebind_select_cancels_pending_hold_from_prior_binding(void) {
  s_down_handler(NULL, NULL);
  TEST_ASSERT_EQUAL_INT(2, app_timer_register_fake.call_count);  // fire timer + fill timer armed
  TEST_ASSERT_TRUE(hold_is_active());

  // A second click-config-provider invocation (e.g. the window re-appearing)
  // rebinds while the first hold is still pending.
  hold_bind_select(NULL, on_fire_spy);

  TEST_ASSERT_EQUAL_INT(2, app_timer_cancel_fake.call_count);  // both cancelled, not leaked
  TEST_ASSERT_FALSE(hold_is_active());
  TEST_ASSERT_EQUAL_INT(0, on_fire_spy_fake.call_count);  // cancelled, never phantom-fired
}

void test_bind_set_repeat_cancels_pending_repeat_timer_from_prior_binding(void) {
  hold_bind_set_repeat(NULL, on_step_spy);

  ClickHandler up_down_handler = NULL;
  for (unsigned i = 0; i < window_raw_click_subscribe_fake.call_count; i++) {
    if (window_raw_click_subscribe_fake.arg0_history[i] == BUTTON_ID_UP) {
      up_down_handler = window_raw_click_subscribe_fake.arg1_history[i];
    }
  }
  TEST_ASSERT_NOT_NULL(up_down_handler);

  up_down_handler(NULL, NULL);  // press-and-hold Up -- arms a repeat AppTimer
  TEST_ASSERT_EQUAL_INT(1, app_timer_register_fake.call_count);
  TEST_ASSERT_EQUAL_INT(0, app_timer_cancel_fake.call_count);

  // Rebind (e.g. SET's window re-appearing) while the repeat timer from the
  // held Up button is still pending -- must cancel it, not just null it out.
  hold_bind_set_repeat(NULL, on_step_spy);

  TEST_ASSERT_EQUAL_INT(1, app_timer_cancel_fake.call_count);
}

// --- SET auto-repeat discrete-tap and accel-curve behavior -----------------
// set_accel constants: DELAY=400, RATE=150,
// FAST_RATE=65 (SET_REPEAT_FAST_MS -- the poll cadence itself), ACCEL_AFTER
// =2000. (RATE/FAST_RATE were 200/100 before v0.1.7.)

static ClickHandler s_up_down_handler;
static ClickHandler s_up_up_handler;

static void bind_set_repeat(void) {
  hold_bind_set_repeat(NULL, on_step_spy);
  s_up_down_handler = NULL;
  s_up_up_handler = NULL;
  for (unsigned i = 0; i < window_raw_click_subscribe_fake.call_count; i++) {
    if (window_raw_click_subscribe_fake.arg0_history[i] == BUTTON_ID_UP) {
      s_up_down_handler = window_raw_click_subscribe_fake.arg1_history[i];
      s_up_up_handler = window_raw_click_subscribe_fake.arg2_history[i];
    }
  }
  TEST_ASSERT_NOT_NULL(s_up_down_handler);
  TEST_ASSERT_NOT_NULL(s_up_up_handler);
}

// Sums every signed delta on_step_spy was called with (FFF's arg0_history).
static int total_on_step_delta(void) {
  int total = 0;
  for (unsigned i = 0; i < on_step_spy_fake.call_count; i++) {
    total += on_step_spy_fake.arg0_history[i];
  }
  return total;
}

void test_n_discrete_taps_with_no_poll_firings_yield_exactly_n_steps(void) {
  // The shortest possible taps: released before even the FIRST 65ms poll
  // fires. Each press's own immediate "apply one adjust" is the only
  // source of steps.
  bind_set_repeat();
  for (int i = 0; i < 8; i++) {
    s_up_down_handler(NULL, NULL);
    s_up_up_handler(NULL, NULL);
  }

  TEST_ASSERT_EQUAL_INT(8, on_step_spy_fake.call_count);
  TEST_ASSERT_EQUAL_INT(8, total_on_step_delta());
}

void test_n_discrete_taps_each_with_one_incidental_poll_yield_exactly_n_steps(void) {
  // A slightly slower discrete tap: exactly one 65ms poll fires while held
  // (elapsed=65 < SET_REPEAT_DELAY_MS=400, so it's a legitimate no-op) --
  // this is the shape a "200ms hold, back-to-back" tap sequence actually
  // produces. This class of tap must yield exactly N steps, never an
  // overshoot from double-counting elapsed time.
  bind_set_repeat();
  for (int i = 0; i < 8; i++) {
    s_up_down_handler(NULL, NULL);
    fire_last_timer();  // one 65ms poll while held -- still below the 400ms delay
    s_up_up_handler(NULL, NULL);
  }

  TEST_ASSERT_EQUAL_INT(8, total_on_step_delta());
}

void test_fourteen_taps_do_not_overshoot(void) {
  // Directly reproduces the overshoot shape produced by 14 quick taps, with
  // a couple of incidental polls per tap (the closest host-testable analogue
  // of "80ms hold, back-to-back") -- must be exactly 14, not 54.
  bind_set_repeat();
  for (int i = 0; i < 14; i++) {
    s_up_down_handler(NULL, NULL);
    fire_last_timer();
    s_up_up_handler(NULL, NULL);
  }

  TEST_ASSERT_EQUAL_INT(14, total_on_step_delta());
}

void test_genuine_hold_produces_accel_curve(void) {
  // A real hold (never released): matches the delay->rate->accel curve.
  // Polls before the 400ms delay (polls 1-6, elapsed 65..390ms) apply 0.
  // Poll 8 (elapsed=520) still applies 0 (due=1, tick_count already 1 from
  // the immediate down-step). Poll 9 (elapsed=585) is the first to apply +1
  // (due=1+185/150=2). Total after 9 polls: 1 (immediate) + 1 = 2.
  bind_set_repeat();
  s_up_down_handler(NULL, NULL);
  TEST_ASSERT_EQUAL_INT(1, total_on_step_delta());  // immediate down-step

  fire_last_timer_n(9);  // elapsed = 585ms
  TEST_ASSERT_EQUAL_INT(2, total_on_step_delta());

  fire_last_timer_n(5);  // elapsed = 910ms -> due = 1 + 510/150 = 4
  TEST_ASSERT_EQUAL_INT(4, total_on_step_delta());

  s_up_up_handler(NULL, NULL);  // release -- leaves no dangling state for the next test
}

void test_genuine_hold_accel_curve_is_immune_to_a_jumping_wall_clock(void) {
  // Identical shape to the test above, but the wall clock is jerked around
  // between every poll -- the resulting step count must be BYTE-IDENTICAL,
  // proving the accel decision does not depend on time_ms() at all.
  bind_set_repeat();
  s_up_down_handler(NULL, NULL);
  set_now_ms(BASE_MS + 777777);
  TEST_ASSERT_EQUAL_INT(1, total_on_step_delta());

  for (int i = 0; i < 9; i++) {
    set_now_ms((i % 2 == 0) ? (BASE_MS + 500000) : (BASE_MS - 90000));
    fire_last_timer();
  }
  TEST_ASSERT_EQUAL_INT(2, total_on_step_delta());

  for (int i = 0; i < 5; i++) {
    set_now_ms(BASE_MS + (uint32_t)i * 123456u);
    fire_last_timer();
  }
  TEST_ASSERT_EQUAL_INT(4, total_on_step_delta());

  set_now_ms(BASE_MS);
  s_up_up_handler(NULL, NULL);  // release -- leaves no dangling state for the next test
}

void test_release_stops_the_poll_chain_no_further_steps_after_up(void) {
  bind_set_repeat();
  s_up_down_handler(NULL, NULL);
  fire_last_timer_n(9);  // elapsed=585 -> 2 total steps so far
  TEST_ASSERT_EQUAL_INT(2, total_on_step_delta());

  s_up_up_handler(NULL, NULL);
  TEST_ASSERT_EQUAL_INT(1, app_timer_cancel_fake.call_count);

  // A straggler poll that had already been dispatched before the up-handler
  // ran (the "cancel can't stop an in-flight callback" firmware race) must
  // still no-op via the s_repeat_held guard, not add a phantom step.
  fire_last_timer();
  TEST_ASSERT_EQUAL_INT(2, total_on_step_delta());
}

// --- hold_cancel() also cancels SET's own auto-repeat -----------------------
// (the Interruption Model's per-view "set: auto-repeat cancelled" rule).

void test_hold_cancel_cancels_a_pending_set_repeat(void) {
  bind_set_repeat();
  s_up_down_handler(NULL, NULL);
  TEST_ASSERT_EQUAL_INT(1, app_timer_register_fake.call_count);

  hold_cancel();

  TEST_ASSERT_EQUAL_INT(1, app_timer_cancel_fake.call_count);

  // A straggler poll dispatched just before hold_cancel() ran must no-op,
  // not apply a phantom step against a window that's no longer showing.
  fire_last_timer();
  TEST_ASSERT_EQUAL_INT(1, total_on_step_delta());  // just the original immediate step
}

void test_hold_cancel_is_a_no_op_when_nothing_is_held(void) {
  bind_set_repeat();
  hold_cancel();  // nothing pressed at all
  TEST_ASSERT_EQUAL_INT(0, app_timer_cancel_fake.call_count);
}

// --- hold_bind_button generalization ----------------------------------------
//
// hold_bind_button lets more than one button be raw-click-bound on the same
// window (the stopwatch's SELECT+UP binding is the first real consumer). Since
// hold.c registers the SAME raw_down_handler/raw_up_handler for every button
// (disambiguated internally via click_recognizer_get_button_id), these tests
// find the (already-identical) handler pointers via the button id recorded
// in window_raw_click_subscribe's own call history, then drive them with
// click_recognizer_get_button_id_fake.return_val set to the button under
// test -- simulating which physical button the firmware says was pressed.

static void find_handlers_for_button(ButtonId button, ClickHandler *out_down,
                                      ClickHandler *out_up) {
  *out_down = NULL;
  *out_up = NULL;
  for (unsigned i = 0; i < window_raw_click_subscribe_fake.call_count; i++) {
    if (window_raw_click_subscribe_fake.arg0_history[i] == button) {
      *out_down = window_raw_click_subscribe_fake.arg1_history[i];
      *out_up = window_raw_click_subscribe_fake.arg2_history[i];
    }
  }
  TEST_ASSERT_NOT_NULL(*out_down);
  TEST_ASSERT_NOT_NULL(*out_up);
}

void test_hold_bind_button_select_matches_hold_bind_select_behavior(void) {
  // Regression: binding SELECT directly through hold_bind_button (bypassing
  // the hold_bind_select wrapper) must behave identically to setUp's own
  // hold_bind_select call -- same threshold-release-fires contract as
  // test_release_past_threshold_fires_even_though_fire_apptimer_never_dispatched.
  hold_bind_button(NULL, BUTTON_ID_SELECT, NULL, on_fire_spy, HOLD_MS);
  click_recognizer_get_button_id_fake.return_val = BUTTON_ID_SELECT;

  s_down_handler(NULL, NULL);
  fire_last_timer_n(15);
  s_up_handler(NULL, NULL);

  TEST_ASSERT_EQUAL_INT(1, on_fire_spy_fake.call_count);
  TEST_ASSERT_EQUAL_INT(1, vibes_adapter_confirm_fake.call_count);
  TEST_ASSERT_FALSE(hold_is_active());
}

void test_second_bound_button_fires_its_own_on_long_not_the_first_buttons(void) {
  hold_bind_button(NULL, BUTTON_ID_UP, NULL, on_fire_up_spy, HOLD_MS);
  ClickHandler up_down, up_up;
  find_handlers_for_button(BUTTON_ID_UP, &up_down, &up_up);

  click_recognizer_get_button_id_fake.return_val = BUTTON_ID_UP;
  up_down(NULL, NULL);
  fire_last_timer_n(14);
  up_up(NULL, NULL);

  TEST_ASSERT_EQUAL_INT(1, on_fire_up_spy_fake.call_count);
  TEST_ASSERT_EQUAL_INT(0, on_fire_spy_fake.call_count);  // SELECT's own on_long untouched
}

void test_on_short_fires_on_early_release_when_bound(void) {
  hold_bind_button(NULL, BUTTON_ID_UP, on_short_spy, on_fire_up_spy, HOLD_MS);
  ClickHandler up_down, up_up;
  find_handlers_for_button(BUTTON_ID_UP, &up_down, &up_up);

  click_recognizer_get_button_id_fake.return_val = BUTTON_ID_UP;
  up_down(NULL, NULL);
  up_up(NULL, NULL);  // released immediately -- well before HOLD_MS

  TEST_ASSERT_EQUAL_INT(1, on_short_spy_fake.call_count);
  TEST_ASSERT_EQUAL_INT(0, on_fire_up_spy_fake.call_count);  // no on_long fire
}

void test_on_short_null_is_a_no_op_on_early_release(void) {
  // hold_bind_select's own wrapper always passes on_short=NULL (setUp's
  // binding) -- an ordinary early-release tap must not crash or call
  // anything extra.
  s_down_handler(NULL, NULL);
  s_up_handler(NULL, NULL);

  TEST_ASSERT_EQUAL_INT(0, on_fire_spy_fake.call_count);
  TEST_ASSERT_FALSE(hold_is_active());
}

void test_hold_active_button_reports_held_button_and_idle_sentinel(void) {
  TEST_ASSERT_EQUAL_INT(NUM_BUTTONS, hold_active_button());  // idle before any press

  s_down_handler(NULL, NULL);  // SELECT, per setUp's fake button id
  TEST_ASSERT_EQUAL_INT(BUTTON_ID_SELECT, hold_active_button());

  s_up_handler(NULL, NULL);  // early release -- back to idle
  TEST_ASSERT_EQUAL_INT(NUM_BUTTONS, hold_active_button());
}

void test_hold_active_button_reports_the_second_bound_button_too(void) {
  hold_bind_button(NULL, BUTTON_ID_UP, NULL, on_fire_up_spy, HOLD_MS);
  ClickHandler up_down, up_up;
  find_handlers_for_button(BUTTON_ID_UP, &up_down, &up_up);

  click_recognizer_get_button_id_fake.return_val = BUTTON_ID_UP;
  up_down(NULL, NULL);
  TEST_ASSERT_EQUAL_INT(BUTTON_ID_UP, hold_active_button());

  up_up(NULL, NULL);
  TEST_ASSERT_EQUAL_INT(NUM_BUTTONS, hold_active_button());
}

void test_hold_long_ms_binding_requires_more_polls_to_cross_threshold(void) {
  hold_bind_button(NULL, BUTTON_ID_UP, NULL, on_fire_up_spy, HOLD_LONG_MS);
  ClickHandler up_down, up_up;
  find_handlers_for_button(BUTTON_ID_UP, &up_down, &up_up);
  click_recognizer_get_button_id_fake.return_val = BUTTON_ID_UP;

  // 14 polls * 50ms = 700ms -- crosses HOLD_MS but not HOLD_LONG_MS (1200ms).
  up_down(NULL, NULL);
  fire_last_timer_n(14);
  up_up(NULL, NULL);
  TEST_ASSERT_EQUAL_INT(0, on_fire_up_spy_fake.call_count);

  // A fresh press held past HOLD_LONG_MS (24 polls * 50ms = 1200ms) fires.
  up_down(NULL, NULL);
  fire_last_timer_n(24);
  up_up(NULL, NULL);
  TEST_ASSERT_EQUAL_INT(1, on_fire_up_spy_fake.call_count);
}

void test_rebinding_one_button_does_not_disturb_a_different_buttons_pending_hold(void) {
  // hold_bind_button supports binding more than one button on the same
  // window: a rebind must only ever cancel a pending hold that belongs to
  // the SAME button being rebound, never an unrelated button's in-flight
  // hold (hold.c's hold_bind_button doc comment).
  hold_bind_button(NULL, BUTTON_ID_UP, NULL, on_fire_up_spy, HOLD_MS);
  ClickHandler up_down, up_up;
  find_handlers_for_button(BUTTON_ID_UP, &up_down, &up_up);

  click_recognizer_get_button_id_fake.return_val = BUTTON_ID_UP;
  up_down(NULL, NULL);  // UP hold now in progress
  TEST_ASSERT_TRUE(hold_is_active());

  // SELECT's own binding is rebound (e.g. its window re-appearing) while the
  // UP hold is still pending.
  hold_bind_select(NULL, on_fire_spy);

  TEST_ASSERT_TRUE(hold_is_active());
  TEST_ASSERT_EQUAL_INT(BUTTON_ID_UP, hold_active_button());

  // Let the UP hold resolve normally so no state leaks into the next test.
  fire_last_timer_n(14);
  up_up(NULL, NULL);
  TEST_ASSERT_EQUAL_INT(1, on_fire_up_spy_fake.call_count);
}

// --- overlapping SELECT+UP presses ------------------------------------------
//
// The interleaving the prior two-button tests above don't cover (all of
// which fully resolve one button's down->up before ever touching the
// other's handlers): down SELECT, down UP *while SELECT is still physically
// held*, up SELECT, up UP. raw_up_handler must identify WHICH button
// released -- otherwise releasing SELECT (a stale release -- its hold was
// already silently reassigned to UP at step 2) would misfire UP's own
// on_short/on_long, and UP's later genuine release would then no-op against
// already-reset state. SELECT's stale release must fire nothing at all, and
// UP's genuine release must still resolve normally against its OWN
// threshold/callbacks.

void test_overlapping_press_select_then_up_drops_selects_hold_with_no_misfire(void) {
  hold_bind_button(NULL, BUTTON_ID_UP, on_short_spy, on_fire_up_spy, HOLD_MS);
  ClickHandler up_down, up_up;
  find_handlers_for_button(BUTTON_ID_UP, &up_down, &up_up);

  // T0: down SELECT (setUp's own binding) -- claims the channel for SELECT.
  click_recognizer_get_button_id_fake.return_val = BUTTON_ID_SELECT;
  s_down_handler(NULL, NULL);
  TEST_ASSERT_EQUAL_INT(BUTTON_ID_SELECT, hold_active_button());

  // T1: down UP, SELECT still physically held -- reassigns the shared
  // channel to UP (raw_down_handler's own unconditional reset_hold_state(),
  // unchanged/intended behavior -- SELECT's hold is silently dropped, no
  // callback, per this file's "one action at a time" exclusivity model).
  click_recognizer_get_button_id_fake.return_val = BUTTON_ID_UP;
  up_down(NULL, NULL);
  TEST_ASSERT_EQUAL_INT(BUTTON_ID_UP, hold_active_button());

  // Let a few fill polls fire -- an ordinary mid-hold moment, past neither
  // button's own threshold yet (2 * 50ms = 100ms).
  fire_last_timer_n(2);

  // T2: up SELECT -- a STALE release (SELECT no longer owns the channel).
  // A release must be evaluated against the button that actually owns the
  // channel, never blindly against whichever threshold/callbacks happen to
  // be current -- this must fire NOTHING and must NOT disturb the
  // still-in-progress UP hold.
  click_recognizer_get_button_id_fake.return_val = BUTTON_ID_SELECT;
  s_up_handler(NULL, NULL);

  TEST_ASSERT_EQUAL_INT(0, on_fire_spy_fake.call_count);     // SELECT's own on_long: never
  TEST_ASSERT_EQUAL_INT(0, on_fire_up_spy_fake.call_count);  // UP's on_long: not yet (below threshold)
  TEST_ASSERT_EQUAL_INT(0, on_short_spy_fake.call_count);    // UP's on_short: not yet (channel still active)
  TEST_ASSERT_TRUE(hold_is_active());                        // UP's hold must survive the stale release
  TEST_ASSERT_EQUAL_INT(BUTTON_ID_UP, hold_active_button());  // still UP's channel

  // T3: up UP -- the genuine release, still below HOLD_MS (100ms < 700ms) --
  // must resolve as UP's own early release (on_short), not silently no-op.
  click_recognizer_get_button_id_fake.return_val = BUTTON_ID_UP;
  up_up(NULL, NULL);

  TEST_ASSERT_EQUAL_INT(1, on_short_spy_fake.call_count);    // UP's genuine release WAS honored
  TEST_ASSERT_EQUAL_INT(0, on_fire_up_spy_fake.call_count);  // released early -- no on_long
  TEST_ASSERT_EQUAL_INT(0, on_fire_spy_fake.call_count);     // SELECT's on_long: still never
  TEST_ASSERT_FALSE(hold_is_active());
}

void test_overlapping_press_select_then_up_up_releases_up_past_threshold_fires_up_only(void) {
  hold_bind_button(NULL, BUTTON_ID_UP, NULL, on_fire_up_spy, HOLD_MS);
  ClickHandler up_down, up_up;
  find_handlers_for_button(BUTTON_ID_UP, &up_down, &up_up);

  click_recognizer_get_button_id_fake.return_val = BUTTON_ID_SELECT;
  s_down_handler(NULL, NULL);  // down SELECT

  click_recognizer_get_button_id_fake.return_val = BUTTON_ID_UP;
  up_down(NULL, NULL);  // down UP, SELECT still held -- channel reassigned to UP

  fire_last_timer_n(14);  // 700ms -- crosses HOLD_MS for UP's own binding

  // Stale SELECT release, past what would be SELECT's OWN threshold too --
  // must still fire nothing (button-identity check happens before the
  // threshold check).
  click_recognizer_get_button_id_fake.return_val = BUTTON_ID_SELECT;
  s_up_handler(NULL, NULL);
  TEST_ASSERT_EQUAL_INT(0, on_fire_spy_fake.call_count);
  TEST_ASSERT_EQUAL_INT(0, on_fire_up_spy_fake.call_count);
  TEST_ASSERT_TRUE(hold_is_active());

  // Genuine UP release, past threshold -- fires UP's on_long, exactly once.
  click_recognizer_get_button_id_fake.return_val = BUTTON_ID_UP;
  up_up(NULL, NULL);
  TEST_ASSERT_EQUAL_INT(1, on_fire_up_spy_fake.call_count);
  TEST_ASSERT_EQUAL_INT(0, on_fire_spy_fake.call_count);
  TEST_ASSERT_FALSE(hold_is_active());
}

// --- hold_progress()'s per-mille integer math coverage ---------------------
// hold_progress() computes `elapsed_ms * FRACTION_SCALE_PM / active_hold_ms`
// using fixed-point integer math. Per-mille outputs at 0/mid/exact-full/
// over-full elapsed values, at both of this app's two real active_hold_ms
// values (HOLD_MS=700, HOLD_LONG_MS=1200).

void test_hold_progress_per_mille_at_hold_ms_boundaries(void) {
  s_down_handler(NULL, NULL);  // HOLD_MS=700, setUp's SELECT binding
  TEST_ASSERT_EQUAL_INT(0, hold_progress());  // 0 polls -- elapsed=0

  fire_last_timer_n(7);  // elapsed=350ms -- mid
  TEST_ASSERT_EQUAL_INT(500, hold_progress());

  fire_last_timer_n(7);  // elapsed=700ms (14 total) -- exact-full
  TEST_ASSERT_EQUAL_INT(1000, hold_progress());

  fire_last_timer_n(6);  // elapsed=1000ms (20 total) -- past HOLD_MS, clamped
  TEST_ASSERT_EQUAL_INT(1000, hold_progress());

  s_up_handler(NULL, NULL);  // release -- leaves no dangling state for the next test
}

void test_hold_progress_per_mille_at_hold_long_ms_boundaries(void) {
  hold_bind_button(NULL, BUTTON_ID_UP, NULL, on_fire_up_spy, HOLD_LONG_MS);
  ClickHandler up_down, up_up;
  find_handlers_for_button(BUTTON_ID_UP, &up_down, &up_up);
  click_recognizer_get_button_id_fake.return_val = BUTTON_ID_UP;

  up_down(NULL, NULL);
  TEST_ASSERT_EQUAL_INT(0, hold_progress());  // 0 polls -- elapsed=0

  fire_last_timer_n(12);  // elapsed=600ms -- mid
  TEST_ASSERT_EQUAL_INT(500, hold_progress());

  fire_last_timer_n(12);  // elapsed=1200ms (24 total) -- exact-full
  TEST_ASSERT_EQUAL_INT(1000, hold_progress());

  fire_last_timer_n(6);  // elapsed=1500ms (30 total) -- past HOLD_LONG_MS, clamped
  TEST_ASSERT_EQUAL_INT(1000, hold_progress());

  up_up(NULL, NULL);  // release -- leaves no dangling state for the next test
}

void test_hold_progress_is_zero_when_idle(void) {
  TEST_ASSERT_FALSE(hold_is_active());
  TEST_ASSERT_EQUAL_INT(0, hold_progress());
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_release_before_threshold_cancels_without_firing);
  RUN_TEST(test_release_past_threshold_fires_even_though_fire_apptimer_never_dispatched);
  RUN_TEST(test_release_exactly_at_threshold_fires);
  RUN_TEST(test_release_one_poll_before_threshold_does_not_fire);
  RUN_TEST(test_fire_on_release_cancels_the_now_redundant_pending_timers);
  RUN_TEST(test_a_subsequent_press_after_a_threshold_release_fire_works_normally);
  RUN_TEST(test_wall_clock_jump_with_no_fill_polls_does_not_fire);
  RUN_TEST(test_wall_clock_jump_during_a_short_tap_does_not_fire);
  RUN_TEST(test_rebind_select_cancels_pending_hold_from_prior_binding);
  RUN_TEST(test_bind_set_repeat_cancels_pending_repeat_timer_from_prior_binding);
  RUN_TEST(test_n_discrete_taps_with_no_poll_firings_yield_exactly_n_steps);
  RUN_TEST(test_n_discrete_taps_each_with_one_incidental_poll_yield_exactly_n_steps);
  RUN_TEST(test_fourteen_taps_do_not_overshoot);
  RUN_TEST(test_genuine_hold_produces_accel_curve);
  RUN_TEST(test_genuine_hold_accel_curve_is_immune_to_a_jumping_wall_clock);
  RUN_TEST(test_release_stops_the_poll_chain_no_further_steps_after_up);
  RUN_TEST(test_hold_cancel_cancels_a_pending_set_repeat);
  RUN_TEST(test_hold_cancel_is_a_no_op_when_nothing_is_held);
  RUN_TEST(test_hold_bind_button_select_matches_hold_bind_select_behavior);
  RUN_TEST(test_second_bound_button_fires_its_own_on_long_not_the_first_buttons);
  RUN_TEST(test_on_short_fires_on_early_release_when_bound);
  RUN_TEST(test_on_short_null_is_a_no_op_on_early_release);
  RUN_TEST(test_hold_active_button_reports_held_button_and_idle_sentinel);
  RUN_TEST(test_hold_active_button_reports_the_second_bound_button_too);
  RUN_TEST(test_hold_long_ms_binding_requires_more_polls_to_cross_threshold);
  RUN_TEST(test_rebinding_one_button_does_not_disturb_a_different_buttons_pending_hold);
  RUN_TEST(test_overlapping_press_select_then_up_drops_selects_hold_with_no_misfire);
  RUN_TEST(test_overlapping_press_select_then_up_up_releases_up_past_threshold_fires_up_only);
  RUN_TEST(test_hold_progress_per_mille_at_hold_ms_boundaries);
  RUN_TEST(test_hold_progress_per_mille_at_hold_long_ms_boundaries);
  RUN_TEST(test_hold_progress_is_zero_when_idle);
  return UNITY_END();
}
