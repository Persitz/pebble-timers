// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

#include "hold.h"

#include <stdint.h>

#include "fraction_scale.h"  // FRACTION_SCALE_PM is the one shared source of
                              // truth for the 0-1000 per-mille fraction
                              // contract hold_progress()/render_bar() both
                              // depend on -- previously a second, independent
                              // `#define HOLD_FRACTION_SCALE 1000` in this
                              // file, silently required to stay numerically
                              // identical to render.h's own
                              // RENDER_FRACTION_SCALE with nothing enforcing
                              // it. A plain, pebble.h-independent header (not
                              // render.h itself) so this file's host test
                              // build (fakes/pebble.h, no GContext/GRect)
                              // stays unaffected.
#include "lib/set_accel.h"
#include "vibes_adapter.h"

#define FILL_REDRAW_MS 50

// Per-button binding table (see hold.h's doc comment): only SELECT and UP are
// ever actually bound in this app, but indexing directly by ButtonId (size
// NUM_BUTTONS) is simpler than a bespoke 2-slot map and mirrors
// hold_bind_set_repeat's existing per-button-array precedent below.
typedef struct {
  HoldFireCallback on_short;
  HoldFireCallback on_long;
  uint32_t hold_ms;
} ButtonHoldBinding;

static Window *s_hold_window;
static ButtonHoldBinding s_bindings[NUM_BUTTONS];

// The single "active hold" channel -- whichever bound button is physically
// held right now (a user holds one button at a time). s_active_button is
// only meaningful while s_held_since_ms != 0; hold_active_button() gates on
// that same sentinel rather than also resetting this field in
// reset_hold_state(), matching this file's existing "stale-but-irrelevant
// until the next down press" pattern for s_on_fire.
static ButtonId s_active_button = NUM_BUTTONS;
static HoldFireCallback s_on_short;  // active hold's on_short
static HoldFireCallback s_on_fire;   // active hold's on_long
static uint32_t s_active_hold_ms;    // active hold's threshold (HOLD_MS or HOLD_LONG_MS, ...)
static uint32_t s_held_since_ms;  // 0 = not holding

#ifdef GALLERY_MODE
// -1 = no override (normal behavior); >= 0 = forced per-mille fraction
// (0-1000), see hold.h.
static int32_t s_debug_forced_fraction_pm = -1;
// hold_debug_force() alone can't depict "Up-hold in progress" vs.
// "Select-hold in progress" -- both force hold_is_active()/hold_progress()
// true, but hold_active_button() still reads the real (idle) s_active_button,
// since no real raw click ever ran. The stopwatch footer's label picks its
// text via hold_active_button(), so gallery mode needs its own forced button
// to go with the forced fraction. Defaults to SELECT (the common case: only
// scenes depicting a held UP need to override it).
static ButtonId s_debug_forced_button = BUTTON_ID_SELECT;
#endif
static AppTimer *s_fire_timer;
static AppTimer *s_fill_timer;
// Monotonic AppTimer-poll count, NOT wall-clock ms -- see the long comment on
// s_repeat_poll_count below for why. Every fill_timer_cb firing represents
// one real FILL_REDRAW_MS quantum (AppTimer's own scheduling, which is
// consistently reliable in practice); the elapsed estimate derived from it
// is immune to the same wall-clock non-monotonicity that broke the SET
// auto-repeat.
static uint32_t s_fill_poll_count;

static SetStepCallback s_on_step;
// index 0 = BUTTON_ID_UP, index 1 = BUTTON_ID_DOWN
static int32_t s_repeat_tick_count[2];
static bool s_repeat_held[2];
static AppTimer *s_repeat_timer[2];
// Root cause: repeat_poll_cb used to derive
// `elapsed_ms` from `now_ms() - s_repeat_since_ms[idx]`, i.e. two reads of
// Pebble's wall-clock time_ms(). Empirically proven non-monotonic in this
// environment (instrumented repro: a single ~100-200ms discrete tap produced
// an `elapsed` reading that jumped from the expected ~100ms to 1100ms
// between two consecutive 100ms-apart polls -- and a DIFFERENT tap in the
// same run produced elapsed=-800, i.e. the clock ran backward; PebbleOS's own
// `lin_algorithm.c` firmware log even names this "time travel"). Feeding that
// jump into set_accel_step() as if the button had genuinely been held ~1s
// injects several phantom accelerated steps from one ordinary tap -- the
// exact overshoot measured (8 taps -> 15, 14 taps -> 54). Root-fixed by
// never reading the wall clock for this decision at all: each repeat_poll_cb
// firing increments this monotonic counter, and `elapsed_ms` is derived as
// `poll_count * SET_REPEAT_FAST_MS` -- tied to AppTimer's own re-scheduling
// cadence, which is NOT wall-clock-based and empirically reliable.
static int32_t s_repeat_poll_count[2];

static uint32_t now_ms(void) {
  time_t sec;
  uint16_t ms;
  time_ms(&sec, &ms);
  return (uint32_t)sec * 1000u + ms;
}

static void reset_hold_state(void) {
  if (s_fire_timer != NULL) {
    app_timer_cancel(s_fire_timer);
    s_fire_timer = NULL;
  }
  if (s_fill_timer != NULL) {
    app_timer_cancel(s_fill_timer);
    s_fill_timer = NULL;
  }
  s_held_since_ms = 0;
  s_fill_poll_count = 0;
}

static void fill_timer_cb(void *data) {
  (void)data;
  if (s_held_since_ms == 0) return;
  s_fill_poll_count++;
  if (s_hold_window != NULL) {
    layer_mark_dirty(window_get_root_layer(s_hold_window));
  }
  uint32_t elapsed_est = s_fill_poll_count * FILL_REDRAW_MS;
  if (elapsed_est < s_active_hold_ms) {
    s_fill_timer = app_timer_register(FILL_REDRAW_MS, fill_timer_cb, NULL);
  } else {
    s_fill_timer = NULL;
  }
}

static void fire_timer_cb(void *data) {
  (void)data;
  if (s_held_since_ms == 0) return;  // already cancelled
  HoldFireCallback cb = s_on_fire;
  reset_hold_state();
  vibes_adapter_confirm();  // eyes-free/gloved confirmation on every hold-fire
  if (cb != NULL) cb();
}

static void raw_down_handler(ClickRecognizerRef recognizer, void *context) {
  (void)context;
  ButtonId button = click_recognizer_get_button_id(recognizer);
  reset_hold_state();  // defensive: clear any stray prior state
  s_active_button = button;
  s_on_short = s_bindings[button].on_short;
  s_on_fire = s_bindings[button].on_long;
  s_active_hold_ms = s_bindings[button].hold_ms;
  s_held_since_ms = now_ms();  // only ever compared to 0 now -- see fields above
  s_fire_timer = app_timer_register(s_active_hold_ms, fire_timer_cb, NULL);
  s_fill_timer = app_timer_register(FILL_REDRAW_MS, fill_timer_cb, NULL);
}

static void raw_up_handler(ClickRecognizerRef recognizer, void *context) {
  (void)context;
  if (s_held_since_ms == 0) return;  // already fired (or cancelled) -- no-op

  // SELECT and UP can share this ONE active-
  // hold channel (two-button holds, e.g. the stopwatch), but a release is only
  // meaningful for the button that actually OWNS the currently-active hold.
  // Without this check, physically overlapping presses (down SELECT, down UP
  // *before releasing SELECT*, up SELECT, up UP -- both buttons are adjacent
  // and this is a realistic press pattern) misattribute SELECT's release to
  // UP's threshold/callbacks (raw_down_handler's own unconditional
  // reset_hold_state() already silently reassigned the shared channel to UP
  // at step 2) and then swallow UP's own
  // genuine release entirely (state already zeroed by then). A release for
  // any button that ISN'T the current active-hold owner is stale/irrelevant
  // -- that button's hold was already interrupted (dropped, by design: "a
  // user physically holds one action at a time" is this file's stated
  // exclusivity model, hold_bind_button's own doc comment) when the other
  // button's down claimed the channel -- so it must just no-op, never
  // evaluate the threshold or fire a callback.
  if (click_recognizer_get_button_id(recognizer) != s_active_button) return;

  // The header's documented
  // guarantee -- an up event whose duration already exceeds HOLD_MS fires
  // deterministically, not just via the fire AppTimer. This used to compare
  // `now_ms() - s_held_since_ms` against HOLD_MS -- two wall-clock reads,
  // the same non-monotonic clock issue described above. A false-positive jump
  // here doesn't just miscount a digit, it fires a HOLD-GATED action (e.g.
  // "Done" on a fired alarm, deleting the timer) from an ORDINARY short tap
  // that never came close to physically holding HOLD_MS -- reproduced this
  // exact mechanism empirically (hero_hold_fire -> store_remove firing,
  // `vibe:shortConfirm` logged, from a single plain "select" tap that never
  // held anywhere near HOLD_MS). Uses the same monotonic fill-poll-count
  // estimate as fill_timer_cb -- immune to the same wall-clock defect.
  uint32_t elapsed_est = s_fill_poll_count * FILL_REDRAW_MS;
  if (elapsed_est >= s_active_hold_ms) {
    fire_timer_cb(NULL);
    return;
  }

  // Released before the threshold: cancel, no on_long fire -- but run
  // on_short (if bound) as the "early release" affordance (the
  // stopwatch Up short-press hint is the first real consumer). Captured
  // before reset_hold_state() the same way fire_timer_cb captures s_on_fire,
  // so the callback sees a clean, already-reset hold state (consistent with
  // the existing on_long-fire ordering) rather than running mid-teardown.
  HoldFireCallback short_cb = s_on_short;
  reset_hold_state();
  if (s_hold_window != NULL) {
    layer_mark_dirty(window_get_root_layer(s_hold_window));
  }
  if (short_cb != NULL) short_cb();
}

void hold_bind_button(Window *window, ButtonId button, HoldFireCallback on_short,
                       HoldFireCallback on_long, uint32_t hold_ms) {
  // Guards against a latent leak: a rebind of
  // THIS button while ITS OWN hold is still pending (fire/fill AppTimers
  // armed) used to just overwrite the bound state without cancelling it --
  // the stale timers would still fire later, invoking the new on_long against
  // state left over from whichever window bound last. Only reset the shared
  // active channel if it's currently claimed by *this* button -- a rebind of
  // one button (e.g. its window re-appearing) must not cancel an unrelated
  // button's in-flight hold (the stopwatch's SELECT+UP dual-hold case).
  if (s_active_button == button && s_held_since_ms != 0) {
    reset_hold_state();
  }
  s_hold_window = window;
  s_bindings[button].on_short = on_short;
  s_bindings[button].on_long = on_long;
  s_bindings[button].hold_ms = hold_ms;
  window_raw_click_subscribe(button, raw_down_handler, raw_up_handler, NULL);
}

void hold_bind_select(Window *window, HoldFireCallback on_fire) {
  hold_bind_button(window, BUTTON_ID_SELECT, NULL, on_fire, HOLD_MS);
}

ButtonId hold_active_button(void) {
#ifdef GALLERY_MODE
  if (s_debug_forced_fraction_pm >= 0) return s_debug_forced_button;
#endif
  return s_held_since_ms != 0 ? s_active_button : NUM_BUTTONS;
}

// The Interruption Model's per-view
// contract (the "set (spinner)" state) requires "auto-repeat
// cancelled" on any pre-emption/focus-loss -- but this previously only ever
// cancelled the SELECT hold-to-apply mechanism (s_fire_timer/s_fill_timer),
// never SET's own Up/Down auto-repeat (s_repeat_timer[]). If a nav
// pre-emption (an alarm taking over) or a focus loss landed while a user was
// PHYSICALLY holding Up/Down on SET, the repeat AppTimer chain was left
// dangling: still armed, still calling s_on_step (still bound to the just-
// popped SET window's on_repeat_step) every SET_REPEAT_FAST_MS, mutating
// SET's dialed value invisibly while a different screen was on top. Folded
// into hold_cancel() itself (not a second call site every caller has to
// remember) since every existing caller (nav.c's structural per-transition
// cancel, the HERO/ALARM take-over cancel, main.c's focus-loss cancel) is a
// legitimate place to also want any in-progress auto-repeat stopped.
void hold_cancel(void) {
  if (s_held_since_ms != 0) {
    reset_hold_state();
  }
  for (int i = 0; i < 2; i++) {
    if (s_repeat_timer[i] != NULL) {
      app_timer_cancel(s_repeat_timer[i]);
      s_repeat_timer[i] = NULL;
    }
    // Guards any already-dispatched (un-cancellable) repeat_poll_cb still in
    // flight -- its own `if (!s_repeat_held[idx]) return;` no-ops it.
    s_repeat_held[i] = false;
    s_repeat_poll_count[i] = 0;
  }
}

#ifdef GALLERY_MODE
void hold_debug_force(int32_t fraction_pm) {
  s_debug_forced_fraction_pm = fraction_pm;
}

void hold_debug_force_button(ButtonId button) {
  s_debug_forced_button = button;
}
#endif

// Per-mille (0-FRACTION_SCALE_PM) fixed-point fraction -- see hold.h's doc
// comment. FILL_REDRAW_MS-quantized elapsed time over the active hold's
// threshold, same ratio the old `(float)elapsed / (float)active_hold_ms`
// computed, now via plain integer division. Reuses fraction_scale.h's shared
// FRACTION_SCALE_PM directly rather than a second,
// independent `#define` -- see the #include comment above.
int32_t hold_progress(void) {
#ifdef GALLERY_MODE
  if (s_debug_forced_fraction_pm >= 0) return s_debug_forced_fraction_pm;
#endif
  if (s_held_since_ms == 0) return 0;
  uint32_t elapsed_ms = s_fill_poll_count * FILL_REDRAW_MS;
  uint32_t pm = elapsed_ms * FRACTION_SCALE_PM / s_active_hold_ms;
  if (pm > FRACTION_SCALE_PM) pm = FRACTION_SCALE_PM;
  return (int32_t)pm;
}

bool hold_is_active(void) {
#ifdef GALLERY_MODE
  if (s_debug_forced_fraction_pm >= 0) return true;
#endif
  return s_held_since_ms != 0;
}

// This
// replaces window_single_repeating_click_subscribe with the same
// raw-click + AppTimer primitive hold_bind_select already uses successfully
// for the hold-to-apply gesture. This is a real robustness improvement
// (one fewer distinct click-subscription mechanism in the app, same proven
// primitive everywhere), but a residual risk remains: the deeper
// "Click subscribe must be set from click config provider" crash under
// sustained rapid interaction is not fully eliminated by this change alone.
static void repeat_poll_cb(void *data) {
  int idx = (int)(intptr_t)data;
  s_repeat_timer[idx] = NULL;
  if (!s_repeat_held[idx]) return;  // released while this poll was in flight

  s_repeat_poll_count[idx]++;
  int32_t elapsed = s_repeat_poll_count[idx] * SET_REPEAT_FAST_MS;
  int32_t delta = set_accel_step(elapsed, s_repeat_tick_count[idx]);
  if (delta > 0) {
    s_repeat_tick_count[idx] += delta;
    int sign = (idx == 0) ? 1 : -1;
    if (s_on_step != NULL) s_on_step(sign * delta);
  }
  s_repeat_timer[idx] = app_timer_register(SET_REPEAT_FAST_MS, repeat_poll_cb, (void *)(intptr_t)idx);
}

static void repeat_down(int idx, int sign) {
  s_repeat_held[idx] = true;
  s_repeat_poll_count[idx] = 0;
  // Fresh press: apply one adjust immediately -- the accel curve
  // governs only subsequent repeats.
  s_repeat_tick_count[idx] = 1;
  if (s_on_step != NULL) s_on_step(sign);
  if (s_repeat_timer[idx] != NULL) {
    app_timer_cancel(s_repeat_timer[idx]);  // defensive: clear any stray prior timer
  }
  s_repeat_timer[idx] = app_timer_register(SET_REPEAT_FAST_MS, repeat_poll_cb, (void *)(intptr_t)idx);
}

static void repeat_up(int idx) {
  s_repeat_held[idx] = false;
  if (s_repeat_timer[idx] != NULL) {
    app_timer_cancel(s_repeat_timer[idx]);
    s_repeat_timer[idx] = NULL;
  }
}

static void repeat_up_down_handler(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
  repeat_down(0, 1);
}

static void repeat_up_up_handler(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
  repeat_up(0);
}

static void repeat_down_down_handler(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
  repeat_down(1, -1);
}

static void repeat_down_up_handler(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
  repeat_up(1);
}

void hold_bind_set_repeat(Window *window, SetStepCallback on_step) {
  (void)window;
  s_on_step = on_step;
  for (int i = 0; i < 2; i++) {
    // Guards against a latent leak, same shape as
    // hold_bind_select's above: a rebind while a repeat timer from a PRIOR
    // binding is still pending used to null the handle out without
    // cancelling it -- the stale AppTimer would still fire later and call
    // s_on_step, which by then belongs to whichever window rebound last.
    if (s_repeat_timer[i] != NULL) {
      app_timer_cancel(s_repeat_timer[i]);
    }
    s_repeat_poll_count[i] = 0;
    s_repeat_tick_count[i] = 0;
    s_repeat_held[i] = false;
    s_repeat_timer[i] = NULL;
  }
  window_raw_click_subscribe(BUTTON_ID_UP, repeat_up_down_handler, repeat_up_up_handler, NULL);
  window_raw_click_subscribe(BUTTON_ID_DOWN, repeat_down_down_handler, repeat_down_up_handler,
                              NULL);
}
