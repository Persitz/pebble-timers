// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

#include "screen_clear_confirm.h"

#include <stdio.h>

#include "app_context.h"
#include "input_guard.h"
#include "lib/store.h"
#include "nav.h"
#include "persist_adapter.h"
#include "render.h"
#include "testlog_am.h"
#include "vibes_adapter.h"
#include "wakeup_adapter.h"
#ifdef GALLERY_MODE
#include "gallery.h"
#endif

#define COLOR_PAPER RENDER_COLOR_PAPER
#define TITLE_RECT GRect(0, 80, 200, 60)

static Window *s_window;
static Layer *s_root;

static void root_layer_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, COLOR_PAPER);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  render_clock(ctx, bounds, clock_is_24h_style());

  // Singular phrasing: "Clear all 1 timer?" reads grammatically odd
  // ("all 1"), so count == 1 shows "Clear this timer?" instead; the
  // plural form (count >= 2, "Clear all N timers?") is unchanged.
  int count = app_context_store()->count;
  char title[32];
  if (count == 1) {
    snprintf(title, sizeof title, "Clear this timer?");
  } else {
    snprintf(title, sizeof title, "Clear all %d timers?", count);
  }
  graphics_context_set_text_color(ctx, RENDER_COLOR_INK);
  graphics_draw_text(ctx, title, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD), TITLE_RECT,
                      GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);

  // render_hold_footer_no_more, not render_hold_footer -- this screen has no
  // "more" destination to reveal (same reasoning as HOME's own retired
  // hold-fill design option). hold_active=false: Select here is a plain
  // single-press confirm, not a hold-to-apply gesture -- show_check=true
  // draws the same vector "checkmark before the label" treatment the alarm
  // hero's real "Done" pills use.
  render_hold_footer_no_more(ctx, bounds, "Done", RENDER_COLOR_ACCENT, false, 0, true);
}

// Select confirms: store_clear + persist + replan + confirm vibe +
// testlog("clear", "all"), then back to HOME -- now empty. Mirrors the
// established mutate -> persist_and_replan -> testlog -> nav_go_home()
// shape used elsewhere (e.g. the Delete rows in screen_actions.c).
//
// Guarded by input_guard_blocking(), mirroring screen_alarm.c's
// hero_back_click_handler / screen_ringing.c's back_click_handler: this
// screen can be resumed to (nav.c's resolve_to arms the guard on resume,
// the same mechanism set_alarm_target uses for take-overs) after an alarm
// interruption resolves, and unlike every other resume target, this is a
// single un-gated press that irreversibly wipes the whole store. A
// carried-over or stray Select landing inside the settling window is
// swallowed instead.
static void select_click_handler(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
  if (input_guard_blocking()) {
    testlog("clearconfirm_select", "guard");
    return;
  }
  TimerStore *store = app_context_store();
  store_clear(store);
  persist_adapter_save(store);
  wakeup_adapter_replan(store, time(NULL));
  vibes_adapter_confirm();
  testlog("clear", "all");
  nav_go_home();
}

// Back cancels -- HOME unchanged, nothing cleared.
static void back_click_handler(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
  nav_go_home();
}

static void click_config_provider(void *context) {
  (void)context;
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click_handler);
  window_single_click_subscribe(BUTTON_ID_BACK, back_click_handler);
}

static void window_load(Window *window) {
  s_root = window_get_root_layer(window);
  layer_set_update_proc(s_root, root_layer_update_proc);
}

static void window_unload(Window *window) {
  (void)window;
  s_root = NULL;
}

Window *screen_clear_confirm_create(void) {
  s_window = window_create();
  window_set_background_color(s_window, COLOR_PAPER);
#ifdef GALLERY_MODE
  // Screen gallery (inner loop, src/c/gallery.c): every real screen's
  // click_config_provider is swapped for the gallery's own at create time --
  // see screen_set.c's matching comment. Never compiled into a release build.
  window_set_click_config_provider(s_window, gallery_click_config_provider);
#else
  window_set_click_config_provider(s_window, click_config_provider);
#endif
  window_set_window_handlers(s_window, (WindowHandlers){
                                            .load = window_load,
                                            .unload = window_unload,
                                        });
  return s_window;
}

void screen_clear_confirm_destroy(void) {
  window_destroy(s_window);
  s_window = NULL;
}
