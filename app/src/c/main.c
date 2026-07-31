// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

#include <pebble.h>
#include <stdio.h>
#include <stdlib.h>

#include "app_context.h"
#include "dictation_am.h"
#include "fonts_adapter.h"
#include "hold.h"
#include "lib/app_state.h"
#include "lib/parse_nl.h"
#include "lib/store.h"
#include "lib/timer.h"
#include "nav.h"
#include "persist_adapter.h"
#include "persist_kind_adapter.h"
#include "persist_preset_adapter.h"
#include "screen_actions.h"
#include "screen_alarm.h"
#include "screen_hero.h"
#include "screen_home.h"
#include "screen_laps.h"
#include "screen_library.h"
#include "screen_ringing.h"
#include "testlog_am.h"
#include "vibes_adapter.h"
#include "wakeup_adapter.h"
#ifdef GALLERY_MODE
#include "gallery.h"
#endif

#ifndef GALLERY_MODE
static uint8_t *s_nl_vocabulary;

static void nl_vocabulary_init(void) {
  size_t expected_size = parse_nl_vocabulary_size();
  ResHandle handle = resource_get_handle(RESOURCE_ID_NL_TABLES);
  if (handle == NULL) {
    testlog("voice_data", "missing");
    return;
  }

  size_t resource_bytes = resource_size(handle);
  if (resource_bytes != expected_size) {
    testlog("voice_data", "size");
    return;
  }

  uint8_t *vocabulary = malloc(resource_bytes);
  if (vocabulary == NULL) {
    testlog("voice_data", "malloc");
    return;
  }

  if (resource_load(handle, vocabulary, resource_bytes) != resource_bytes) {
    free(vocabulary);
    testlog("voice_data", "read");
    return;
  }
  if (!parse_nl_vocabulary_set(vocabulary, resource_bytes)) {
    free(vocabulary);
    testlog("voice_data", "checksum");
    return;
  }

  s_nl_vocabulary = vocabulary;
  testlog("voice_data", "ready");
}

static void nl_vocabulary_deinit(void) {
  parse_nl_vocabulary_set(NULL, 0);
  free(s_nl_vocabulary);
  s_nl_vocabulary = NULL;
}
#endif

// Marks whichever screens are currently on-stack dirty -- each screen's
// mark_dirty guards on its own cached root-layer pointer (NULL while that
// window isn't loaded), so calling all of them unconditionally is a cheap,
// always-correct "redraw whatever's visible" -- no need to track which
// screen is on top here too (nav.c already owns that).
static void redraw_visible_screens(void) {
  screen_home_mark_clock_dirty();
  screen_hero_mark_dirty();
  screen_alarm_mark_dirty();
  // The MenuLayer-only windows (ACTIONS, alarm OPTIONS, LAPS) also draw a
  // top-right clock strip -- keep it ticking live the same way every other
  // screen's clock already does.
  screen_actions_mark_clock_dirty();
  screen_alarm_options_mark_clock_dirty();
  screen_laps_mark_clock_dirty();
  screen_ringing_mark_dirty();
  // Wired now even though screen_library_create() isn't called until later
  // in startup -- guards on its own cached layer pointer (NULL until then),
  // same "cheap, always-correct" no-op-until-loaded pattern every other
  // screen here already relies on.
  screen_library_mark_clock_dirty();
}

// Runs on every tick/wakeup/launch, regardless of the current screen.
// Newly-fired timers get an alarm vibe + take-over (per the interruption
// model) + a schedule replan. Returns true iff anything fired (and
// therefore already replanned) -- callers that would otherwise
// unconditionally replan right afterward (the cold wakeup-launch path in
// init(), below) must skip that second replan when this returns true: a
// second cancel-all+reschedule of every wakeup entry is not just wasted
// work, it re-opens the exact collision window the retry ladder exists to
// close.
static bool scan_and_handle_firings(time_t now) {
  TimerStore *store = app_context_store();
  AppState *app_state = app_context_app_state();

  int32_t fired[MAX_TIMERS];
  int n = app_state_scan_firings(app_state, store, now, fired, MAX_TIMERS);
  if (n == 0) return false;

  persist_adapter_save(store);
  wakeup_adapter_replan(store, now);

  for (int i = 0; i < n; i++) {
    const Timer *t = store_find_const(store, fired[i]);
    bool is_repeat = t != NULL && t->kind == KIND_REPEAT;
    vibes_adapter_alarm(is_repeat);
    // A brief, self-timed backlight pulse alongside the fire vibe --
    // light_enable_interaction() auto-fades on the user's own backlight
    // timeout, never a continuous light_enable(true).
    light_enable_interaction();
    app_state->alerted_at = now;
    nav_on_timer_fired(fired[i]);
  }
  return true;
}

// Re-alert cadence: only while an alarm-family screen is on top with a
// non-empty ringing set (re-alert pauses under any other screen/overlay and
// resumes on regain). Extended to VD_RINGING_LIST -- >=2 simultaneously-
// ringing timers are just as unacknowledged while the list is showing as a
// single one is while its own hero is showing; the cap/cadence gate on the
// EARLIEST-still-firing member (nav_pick_ringing_cookie), matching the
// list's own "clear one, the next steps up" ordering.
static int s_realert_count;  // counter for the "realert:N" testlog marker below

static void maybe_realert(time_t now) {
  ViewDesc v = nav_current();
  if (v.kind != VD_ALARM && v.kind != VD_RINGING_LIST) return;

  AppState *app_state = app_context_app_state();
  if (app_state->ringing_count == 0) return;

  int32_t cookie = (v.kind == VD_RINGING_LIST) ? nav_pick_ringing_cookie(app_state) : v.cookie;
  const Timer *t = store_find_const(app_context_store(), cookie);
  if (t == NULL) return;

  int32_t overrun = timer_overrun_s(t, now);
  if (app_state_should_rebuzz(app_state->alerted_at, now, overrun)) {
    vibes_adapter_alarm(t->kind == KIND_REPEAT);
    // Pulse the light too, but only within the first REALERT_CAP_S -- past
    // that, cadence decays to every 60s (app_state_should_rebuzz) and the
    // buzz alone is the signal; lighting the screen every minute all night
    // is not (see app_state.h's decaying-cadence rationale).
    if (app_state_realert_pulses_backlight(overrun)) {
      light_enable_interaction();
    }
    app_state->alerted_at = now;
    // Distinguishable from the initial-fire "vibe:countdownAlarm" marker and
    // from every OTHER re-alert -- a per-call counter (rather than a fixed
    // string) lets a log reader confirm the re-alert CADENCE (repeated
    // re-alerts over ~25s), not just that a single re-buzz happened.
    s_realert_count++;
    char data[16];
    snprintf(data, sizeof data, "%d", s_realert_count);
    testlog("realert", data);
  }
}

static void handle_second_tick(struct tm *tick_time, TimeUnits units_changed) {
  (void)tick_time;
  (void)units_changed;
#ifdef GALLERY_MODE
  // Screen gallery (inner loop, src/c/gallery.c): every scene's fixture is
  // built deliberately (including already-fired-and-ringing scenes, via
  // gallery.c's own gallery_fire_all_due()) and must stay EXACTLY as built
  // until the next Up/Down press -- a real scan here could fire a
  // not-yet-due timer while a scene is on screen (time elapsing past a
  // short countdown's deadline) or re-buzz/re-log an
  // already-ringing one (re-alert), neither of which the gallery
  // wants. redraw_visible_screens() still runs, so live-derived values
  // (remaining/elapsed, computed from real `now` at DRAW time regardless of
  // this scan) keep ticking normally. Never compiled into a release build.
#else
  time_t now = time(NULL);
  scan_and_handle_firings(now);
  maybe_realert(now);
#endif
  redraw_visible_screens();
}

// Under the interruption model, willFocus(false) is the earliest loss
// edge -- cancel any hold in progress before a covered-tick could fire it.
// didFocus(true) is the settled regain edge -- force a redraw and
// immediately rescan for anything that fired while covered.
static void handle_will_focus(bool in_focus) {
  if (!in_focus) {
    hold_cancel();
  }
}

static void handle_did_focus(bool in_focus) {
  if (in_focus) {
#ifndef GALLERY_MODE
    scan_and_handle_firings(time(NULL));
#endif
    redraw_visible_screens();
  }
}

static void handle_wakeup(WakeupId id, int32_t cookie) {
  (void)id;
  (void)cookie;
  // Firing while already running: the same tick-driven scan already covers
  // it (a wakeup that lands while foreground still calls this handler, but
  // scan_and_handle_firings on the next tick -- or immediately here -- picks
  // it up identically; no special-casing needed).
  scan_and_handle_firings(time(NULL));
  redraw_visible_screens();
}

static void init(void) {
  app_message_open(app_message_inbox_size_maximum(), app_message_outbox_size_maximum());
  testlog_init();

  app_context_init();
#ifdef GALLERY_MODE
  // Screen gallery (inner loop, src/c/gallery.c): the gallery builds its OWN
  // deterministic fixture per scene -- loading any real persisted timers
  // here would make scene 0 depend on whatever this emulator's flash
  // happened to have from a prior run, contradicting the "fixture store
  // built only through real lib mutation functions, from a known baseline"
  // convention. Never compiled into a release build.
#else
  persist_adapter_load(app_context_store());
  persist_preset_adapter_load(app_context_presets());
  persist_kind_adapter_load();
#endif

  // Load the custom JBM numeral rasters once, before any screen (nav_init
  // below creates every window up front) ever draws a number with them.
  fonts_adapter_init();

#ifndef GALLERY_MODE
  // Load the parser's one raw vocabulary resource once, beside the existing
  // one-time font loads. parse_nl borrows this buffer until deinit; voice
  // attempts never perform flash I/O on their user-visible path.
  nl_vocabulary_init();
#endif

  // Creates the app's one DictationSession before nav_init() creates any
  // window -- no ordering dependency either way (screen_voice_create()
  // doesn't touch dictation_am state), grouped here with the other
  // one-time subsystem setup.
  dictation_am_init();

  nav_init();

#ifdef GALLERY_MODE
  // Builds scene 0's fixture and navigates to it -- see gallery.h. Skips the
  // real wakeup-relaunch/replan handling below entirely: the gallery never
  // schedules background wakeups (nothing it builds needs to survive the app
  // closing) and a wakeup-triggered cold launch is not a state the gallery
  // needs to reproduce.
  gallery_init();
#else
  time_t now = time(NULL);
  bool already_replanned = false;
  if (launch_reason() == APP_LAUNCH_WAKEUP) {
    WakeupId id;
    int32_t cookie;
    if (wakeup_get_launch_event(&id, &cookie)) {
      // Capture fired-state BEFORE the scan -- distinguishes "this scan is
      // about to fire it for the first time" (scan_and_handle_firings()
      // below already calls nav_on_timer_fired() internally for every
      // cookie it newly fires) from "this timer was already fired and
      // persisted before this launch even started" (a catch-up cold
      // launch -- e.g. the process was killed between firing and the user
      // seeing it; the scan below does nothing for an already-fired timer,
      // so nothing has routed nav to it yet). Calling nav_on_timer_fired()
      // unconditionally after the scan double-navigated (and double-logged,
      // and re-armed the input guard) for the "scan just fired it" case.
      const Timer *t_before = store_find_const(app_context_store(), cookie);
      bool was_fired_before = (t_before != NULL && t_before->fired);

      // Run the due scan -- a wakeup delivered at/after T needs this to
      // actually fire (and persist) the timer, setting t->fired, without
      // waiting for the per-second tick's first firing (unnecessary
      // latency on a real, on-time alarm). If anything fired,
      // scan_and_handle_firings() has ALREADY replanned as part of its own
      // firing handling, so the unconditional replan
      // below must be skipped in that case, or every wakeup entry gets
      // cancelled and rescheduled a second time for no behavioral gain,
      // re-opening the exact collision window the retry ladder exists to
      // close.
      already_replanned = scan_and_handle_firings(now);

      const Timer *t = store_find_const(app_context_store(), cookie);
      bool is_fired_after = (t != NULL && t->fired);
      // `already_replanned` doubles as "the scan fired something new this
      // call" (scan_and_handle_firings() returns true iff n > 0) -- passed
      // through so app_state_needs_catchup_nav() can establish SINGLE
      // ownership of the one startup navigation decision, not just this
      // cookie's own before/after state. Without it, a persisted-fired
      // launch cookie sitting alongside a newly-due SIBLING that the scan
      // just fired (and already navigated to internally) would get a
      // second, wrong navigation call here: `ringing[]` only contains the
      // sibling, so re-routing to this cookie would show a cookie the app isn't
      // actually ringing, hiding the real alarm behind it.
      if (app_state_needs_catchup_nav(was_fired_before, is_fired_after, already_replanned)) {
        // Catch-up case only -- the scan just above neither touched this
        // cookie NOR navigated for anything else, so this is the one and
        // only place that routes nav to it.
        nav_on_timer_fired(cookie);
      }
      // else: either not fired at all yet (a legitimately early wakeup --
      // do not navigate, the per-second tick scan will ring it exactly on
      // time), or the scan above fired THIS cookie itself (already
      // navigated internally), or the scan fired a DIFFERENT cookie
      // (already navigated internally, to the right target). All three
      // cases: do not navigate.
    }
  }

  if (!already_replanned) {
    wakeup_adapter_replan(app_context_store(), now);
  }
#endif

  tick_timer_service_subscribe(SECOND_UNIT, handle_second_tick);
  app_focus_service_subscribe_handlers((AppFocusHandlers){
      .will_focus = handle_will_focus,
      .did_focus = handle_did_focus,
  });
  wakeup_service_subscribe(handle_wakeup);

  testlog("app_init", "home");
}

static void deinit(void) {
  tick_timer_service_unsubscribe();
  app_focus_service_unsubscribe();
  wakeup_service_subscribe(NULL);
  nav_deinit();
  dictation_am_deinit();
#ifndef GALLERY_MODE
  nl_vocabulary_deinit();
#endif
  fonts_adapter_deinit();
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
