// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

#include "screen_ringing.h"

#include <stdio.h>

#include "app_context.h"
#include "input_guard.h"
#include "lib/app_state.h"
#include "lib/format.h"
#include "lib/store.h"
#include "lib/timer.h"
#include "lib/view.h"
#include "nav.h"
#include "render.h"
#include "testlog_am.h"

// ASCII "~" substitutes "≈" -- this codebase's established
// avoid-untested-glyph convention (vector-drawn "+" for HOME's "New timer"
// affordance, ASCII "-"/"x" for the repeat meta line's "·"/"×").
//
// The window used to be a full-height RED flood (window bg + menu normal-bg
// both alarm-red). Fix: window/menu bg is now PAPER; only this HEADER_HEIGHT-tall
// strip (RENDER_STRIP_H, matching the floating-strip component used everywhere
// else) is red.
#define HEADER_HEIGHT RENDER_STRIP_H
#define ROW_HEIGHT RENDER_ROW_H

static Window *s_window;
static Layer *s_header;
static MenuLayer *s_menu;

// --- header (red strip: "~N RINGING" + the light-on-red wall clock) --------

// Not render_floating_strip -- that component always draws a
// single kind glyph, which doesn't fit a header summarizing a MIXED list of
// kinds. Same 30px strip height/geometry, bespoke content: title left +
// real wall clock right (not a kind value).
static void header_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  GColor red = render_kind_color(VIEW_COLOR_ALARM);
  graphics_context_set_fill_color(ctx, red);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  char title[20];
  snprintf(title, sizeof title, "~%d RINGING", app_context_app_state()->ringing_count);
  GRect title_rect = GRect(RENDER_MARGIN_X, 7, bounds.size.w - 74, 18);
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, title, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD), title_rect,
                      GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  render_clock_colored(ctx, bounds, clock_is_24h_style(), RENDER_COLOR_RINGING_TEXT);
}

// --- row list (one row per app_context_app_state()->ringing[] member) ------

static uint16_t get_num_rows(MenuLayer *menu_layer, uint16_t section_index, void *context) {
  (void)menu_layer;
  (void)section_index;
  (void)context;
  return (uint16_t)app_context_app_state()->ringing_count;
}

static int16_t get_cell_height(MenuLayer *menu_layer, MenuIndex *cell_index, void *context) {
  (void)menu_layer;
  (void)cell_index;
  (void)context;
  return ROW_HEIGHT;
}

// Rows reuse the shared render_row() component -- cursor pill / hairline-separated
// quiet rows, same as HOME, replacing the old full-bleed-red/white-swap cell fill.
// One-off members render in alarm-red, the repeat member stays indigo (view_kind_color_token's
// own ruling); each number now carries the "^" caret prefix.
// `label` is routed through lib/view's view_row_label() for consistency:
// this list shares render_row()/RenderRowSpec with HOME (same primitive, same
// "identity among peers" purpose), so the unnamed-timer-is-unlabeled fix applies
// here too, keeping both consumers of the one shared row component behaving the
// same way. See screen_home.c's matching call site / lib/view.h's view_row_label
// doc for the full reasoning.
static void draw_row(GContext *ctx, const Layer *cell_layer, MenuIndex *cell_index, void *context) {
  (void)context;
  GRect bounds = layer_get_bounds(cell_layer);
  bool highlighted = menu_cell_layer_is_highlighted(cell_layer);

  AppState *app_state = app_context_app_state();
  if ((int)cell_index->row >= app_state->ringing_count) return;  // defensive
  int32_t cookie = app_state->ringing[cell_index->row];
  const Timer *t = store_find_const(app_context_store(), cookie);
  if (t == NULL) return;  // defensive -- ringing set mutated out from under a stale row index

  time_t now = time(NULL);
  ViewColorToken token = view_kind_color_token(t->kind, VIEW_HERO_ALARM);
  char value[16];
  format_hms(value, sizeof value, timer_overrun_s(t, now));

  RenderRowSpec spec = {
      .highlighted = highlighted,
      .kind = t->kind,
      .kind_color = render_kind_color(token),
      // Same darkened-value-text fix as HOME's row (this list shares render_row()/
      // RenderRowSpec with HOME, same primitive) -- the glyph above stays full-saturation
      // identity color; only the number darkens.
      .value_color = render_kind_color_deep(token),
      .label = view_row_label(t->name),
      .number = value,
      .number_font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
      .number_prefix = VIEW_PREFIX_CARET,
      .show_bar = false,
      .bar_fraction_pm = 0,
      // The ringing list's cursor row goes glyph+number WHITE on the dark pill
      // -- alarm-red (GColorDarkCandyAppleRed) on black is near-illegible at
      // arm's length; quiet rows keep kind color (red/indigo) for identity.
      // HOME never sets this (its kind colors all read fine on black).
      .cursor_value_white = true,
  };
  render_row(ctx, bounds, &spec);
}

static void select_click(MenuLayer *menu_layer, MenuIndex *cell_index, void *context) {
  (void)menu_layer;
  (void)context;
  AppState *app_state = app_context_app_state();
  if ((int)cell_index->row >= app_state->ringing_count) return;  // defensive
  nav_go_alarm(app_state->ringing[cell_index->row]);
}

// --- buttons -----------------------------------------------------------
// Same non-reentrant, single window_set_click_config_provider() pattern as
// screen_actions.c/screen_laps.c/screen_alarm.c's options window (see
// screen_actions.c's click_config_provider comment for the full
// click-config-reentrancy history this pattern exists to avoid) -- Back
// needs a custom override here (exit-to-watchface, not a plain pop), so
// menu_layer_set_click_config_onto_window() is deliberately never called.

static void up_click_handler(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
  menu_layer_set_selected_next(s_menu, true, MenuRowAlignCenter, true);
}

static void down_click_handler(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
  menu_layer_set_selected_next(s_menu, false, MenuRowAlignCenter, true);
}

static void select_click_handler(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
  MenuIndex idx = menu_layer_get_selected_index(s_menu);
  select_click(s_menu, &idx, NULL);
}

static void select_long_click_handler(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
  AppState *app_state = app_context_app_state();
  MenuIndex idx = menu_layer_get_selected_index(s_menu);
  if ((int)idx.row >= app_state->ringing_count) return;
  nav_go_alarm_options(app_state->ringing[idx.row]);
}

// Guard the ringing-list Back to avoid the "app changed my screen under my finger"
// hazard -- a stale press landing the instant this list appears (e.g. stepping here
// from a just-resolved alarm) would otherwise exit straight to the watchface.
// Not destructive to timer state (alarms keep ringing either way), but guarded for
// consistency.
static void back_click_handler(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
  if (input_guard_blocking()) {
    testlog("ringing_back", "guard");
    return;
  }
  nav_exit_to_watchface();
}

static void click_config_provider(void *context) {
  (void)context;
  window_single_repeating_click_subscribe(BUTTON_ID_UP, 100, up_click_handler);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 100, down_click_handler);
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click_handler);
  window_long_click_subscribe(BUTTON_ID_SELECT, 700, select_long_click_handler, NULL);
  window_single_click_subscribe(BUTTON_ID_BACK, back_click_handler);
}

// --- window lifecycle (MenuLayer/header created ONCE, here) -----------

static void window_appear(Window *window) {
  (void)window;
  menu_layer_reload_data(s_menu);
  // Always land on the top (earliest-fired) row -- matches HOME/ACTIONS/
  // LAPS's own "top row pre-selected" convention (MenuRowAlignTop, not
  // MenuRowAlignNone -- None resets the logical selection but leaves the scroll
  // viewport stuck).
  menu_layer_set_selected_index(s_menu, MenuIndex(0, 0), MenuRowAlignTop, false);
  layer_mark_dirty(s_header);
}

Window *screen_ringing_create(void) {
  // Not wired via menu_layer_set_click_config_onto_window (see
  // click_config_provider above) -- select_click_handler calls
  // select_click() directly instead. No .select_click callback below: a
  // registered but never-invoked .select_click reads as live and has caused
  // bugs (screen_actions.c/screen_alarm.c).
  RenderMenuScreen screen = render_build_menu_screen(&(RenderMenuScreenSpec){
      // Use PAPER, not alarm-red -- only the header strip (HEADER_HEIGHT) is
      // red; the body is a normal paper list with a red header strip only.
      .window_bg = RENDER_COLOR_PAPER,
      .click_config_provider = click_config_provider,
      .window_handlers = (WindowHandlers){.appear = window_appear},
      .header_height = HEADER_HEIGHT,
      .header_update_proc = header_update_proc,
      // Root-caused via pebble.h's own doc comment on
      // menu_layer_set_normal_colors ("if this function is not explicitly
      // called ... defaults to white background") -- an un-called MenuLayer
      // paints WHITE for any viewport space beyond its actual row content
      // AND for its default bottom padding (menu_layer_pad_bottom_enable's
      // doc). Now explicitly PAPER (was alarm-red, the other half of the
      // red-flood bug) -- render_row() draws its own cursor-pill/hairline
      // per row, same as HOME, so the menu's own background just needs to
      // be the paper the hairlines sit on.
      .set_normal_colors = true,
      .normal_bg = RENDER_COLOR_PAPER,
      .normal_fg = RENDER_COLOR_INK,
      .get_num_rows = get_num_rows,
      .get_cell_height = get_cell_height,
      .draw_row = draw_row,
      // RINGING is the one converted screen with no scroll indicator overlay
      // (its list is bounded by the live ringing set, not MAX_TIMERS).
      .with_scroll_indicator = false,
  });
  s_window = screen.window;
  s_header = screen.header;
  s_menu = screen.menu;
  return s_window;
}

void screen_ringing_destroy(void) {
  render_destroy_menu_screen(
      &(RenderMenuScreen){.window = s_window, .header = s_header, .menu = s_menu});
  s_menu = NULL;
  s_header = NULL;
  s_window = NULL;
}

void screen_ringing_mark_dirty(void) {
  if (s_menu != NULL) {
    // Re-syncs row COUNT too, not just repainting existing rows -- see this
    // file's header doc: ringing membership can grow while this screen is
    // already the top of the stack (a 3rd alarm fires while showing 2),
    // which produces a no-op nav diff (nav.c's own "already on the ringing
    // list" branch) with no window-stack transition to hook a reload into
    // otherwise. Cheap at MAX_TIMERS<=16, matching this codebase's
    // established "recompute, don't cache" pattern elsewhere (e.g.
    // view_home_value_size()'s per-redraw recomputation from the current
    // timer count -- see lib/view.c).
    menu_layer_reload_data(s_menu);
    layer_mark_dirty(menu_layer_get_layer(s_menu));
  }
  if (s_header != NULL) layer_mark_dirty(s_header);
}
