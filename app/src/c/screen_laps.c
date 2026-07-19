// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

#include "screen_laps.h"

#include <stdio.h>

#include "app_context.h"
#include "lib/format.h"
#include "lib/store.h"
#include "lib/timer.h"
#include "nav.h"
#include "render.h"
#ifdef GALLERY_MODE
#include "gallery.h"
#endif

#define SCROLL_INDICATOR_WIDTH 3
#define SCROLL_INDICATOR_MARGIN 3

static Window *s_window;
static Layer *s_strip;
static MenuLayer *s_menu;
static Layer *s_scroll_indicator;
static int32_t s_cookie;

static const Timer *current_timer(void) {
  return store_find_const(app_context_store(), s_cookie);
}

static int laps_row_count(void) {
  const Timer *t = current_timer();
  int n = (t != NULL) ? t->lap_count : 0;
  return n > 0 ? n : 1;  // 1 = the "No laps yet" placeholder row
}

static uint16_t get_num_rows(MenuLayer *menu_layer, uint16_t section_index, void *context) {
  (void)menu_layer;
  (void)section_index;
  (void)context;
  return (uint16_t)laps_row_count();
}

// This fix stretched the FIRST row's cell to fill any leftover viewport
// space and bottom-anchored its content inside it, intending to avoid dead
// white space below a short list. But LAPS's row 0 is real content (newest
// lap, draw_row's own "most-recent lap on top" ordering), not a dedicated
// footer like HOME's "+ New timer" row -- bottom-anchoring it inside a
// STRETCHED cell instead pushed a short list's one real row down near the
// screen's BOTTOM, with empty space ABOVE it between the strip and the row.
// Fixed by dropping the stretch entirely: LAPS now uses the same flat
// RENDER_MENU_ROW_H pitch as ACTIONS/alarm-options, so its rows read as a
// plain top-down list under the strip, and a short list simply leaves
// (harmless, expected) blank paper below it.
static int16_t get_cell_height(MenuLayer *menu_layer, MenuIndex *cell_index, void *context) {
  (void)menu_layer;
  (void)cell_index;
  (void)context;
  return RENDER_MENU_ROW_H;
}

// Two-column pill/hairline row (lap# left, split right), replacing
// `menu_cell_basic_draw`'s full-bleed highlight -- same fix as
// screen_actions.c's menu rows.
static void draw_row(GContext *ctx, const Layer *cell_layer, MenuIndex *cell_index, void *context) {
  (void)context;
  bool highlighted = menu_cell_layer_is_highlighted(cell_layer);
  const Timer *t = current_timer();
  if (t == NULL || t->lap_count == 0) {
    render_menu_row(ctx, layer_get_bounds(cell_layer), highlighted, "No laps yet");
    return;
  }

  // Most-recent lap on top -- a glance at the list is for the split just
  // made, not the oldest one (index lap_count-1 is the newest, per
  // timer_add_lap's append-only contract).
  int lap_index = (int)t->lap_count - 1 - (int)cell_index->row;
  if (lap_index < 0) return;  // defensive -- row count can't exceed lap_count

  char title[16];
  snprintf(title, sizeof title, "Lap %d", lap_index + 1);
  char value[16];
  int32_t lap = 0;
  if (!store_lap_get(app_context_store(), t, lap_index, &lap)) return;
  format_hms(value, sizeof value, lap);
  render_menu_row_kv(ctx, layer_get_bounds(cell_layer), highlighted, title, value);
}

// The floating tiffany/kind-colored strip -- kind label +
// current elapsed value, matching ACTIONS's own strip.
static void strip_update_proc(Layer *layer, GContext *ctx) {
  const Timer *t = current_timer();
  if (t == NULL) return;  // defensive
  ViewColorToken token = view_kind_color_token(t->kind, VIEW_HERO_RUNNING);
  char value[16];
  format_home_row_value(value, sizeof value, t, time(NULL));
  // Same DEEP-variant fix as ACTIONS' identical strip component (see
  // render.h's doc comment on render_kind_color_deep).
  render_floating_strip(ctx, layer_get_bounds(layer), render_kind_color_deep(token), t->kind,
                         render_kind_label(t->kind), value);
}

static void back_click_handler(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
  nav_go_actions(s_cookie);  // Back -> actions (not hero)
}

static void select_noop_click_handler(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
}

// scroll_layer_set_callbacks()-based "extra Back handler" wiring silently
// clobbers MenuLayer's own internal click config (Select/Up/Down), via a
// full struct overwrite. Not observable here the same way (Select is a
// documented no-op regardless), but Up/Down would have silently fallen back
// to raw content-offset scrolling instead of real menu-selection navigation.
// Fixed by using one ordinary, non-reentrant
// window_set_click_config_provider() using only the public MenuLayer API --
// no menu_layer_set_click_config_onto_window()/scroll_layer_set_callbacks()
// at all.
static void up_click_handler(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
  menu_layer_set_selected_next(s_menu, true, MenuRowAlignCenter, true);
  if (s_scroll_indicator != NULL) layer_mark_dirty(s_scroll_indicator);
}

static void down_click_handler(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
  menu_layer_set_selected_next(s_menu, false, MenuRowAlignCenter, true);
  if (s_scroll_indicator != NULL) layer_mark_dirty(s_scroll_indicator);
}

// LAPS can hold up to MAX_LAPS=16 rows -- the menu screen most likely to
// actually overflow and show a scroll indicator.
static void scroll_indicator_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  int total = laps_row_count();
  int visible = (int)(bounds.size.h / RENDER_MENU_ROW_H);
  if (visible < 1) visible = 1;
  MenuIndex sel = menu_layer_get_selected_index(s_menu);
  render_scroll_indicator(ctx, bounds, total, visible, (int)sel.row);
}

static void click_config_provider(void *context) {
  (void)context;
  window_single_repeating_click_subscribe(BUTTON_ID_UP, 100, up_click_handler);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 100, down_click_handler);
  // Select intentionally has NO subscription -- documented no-op (not
  // merely "unhandled": a button with ZERO click subscription is itself a
  // deterministic firmware assert -- so this still needs an explicit no-op
  // handler, not silence).
  window_single_click_subscribe(BUTTON_ID_SELECT, select_noop_click_handler);
  window_single_click_subscribe(BUTTON_ID_BACK, back_click_handler);
}

static void window_appear(Window *window) {
  (void)window;
  menu_layer_reload_data(s_menu);
  // Cursor-reset bug fix (see screen_actions.c's matching fix for root
  // cause) -- LAPS can hold up to MAX_LAPS=16 rows, the most exposed of the
  // affected windows.
  menu_layer_set_selected_index(s_menu, MenuIndex(0, 0), MenuRowAlignTop, false);
  if (s_scroll_indicator != NULL) layer_mark_dirty(s_scroll_indicator);
}

// MenuLayer created ONCE here, never in a .load handler -- see the
// matching note in screen_actions.c.
Window *screen_laps_create(void) {
  s_window = window_create();
  window_set_background_color(s_window, RENDER_COLOR_PAPER);
#ifdef GALLERY_MODE
  // Gallery mode for design verification -- never compiled into a
  // release build.
  window_set_click_config_provider(s_window, gallery_click_config_provider);
#else
  window_set_click_config_provider(s_window, click_config_provider);
#endif
  window_set_window_handlers(s_window, (WindowHandlers){
                                            .appear = window_appear,
                                        });

  Layer *root = window_get_root_layer(s_window);
  GRect bounds = layer_get_bounds(root);

  s_strip = layer_create(GRect(0, 0, bounds.size.w, RENDER_STRIP_H));
  layer_set_update_proc(s_strip, strip_update_proc);
  layer_add_child(root, s_strip);

  s_menu =
      menu_layer_create(GRect(0, RENDER_STRIP_H, bounds.size.w, bounds.size.h - RENDER_STRIP_H));
  // The SDK's MenuLayer defaults highlight background to full-cell black;
  // this makes the un-inset margin around render_menu_row()'s own smaller
  // pill read as paper instead.
  menu_layer_set_highlight_colors(s_menu, RENDER_COLOR_PAPER, GColorBlack);
  menu_layer_set_callbacks(s_menu, NULL,
                            (MenuLayerCallbacks){
                                .get_num_rows = get_num_rows,
                                .get_cell_height = get_cell_height,
                                .draw_row = draw_row,
                                // Select is a documented no-op -- no
                                // .select_click callback registered.
                            });
  layer_add_child(root, menu_layer_get_layer(s_menu));

  s_scroll_indicator = layer_create(GRect(bounds.size.w - SCROLL_INDICATOR_WIDTH - SCROLL_INDICATOR_MARGIN,
                                           RENDER_STRIP_H, SCROLL_INDICATOR_WIDTH,
                                           bounds.size.h - RENDER_STRIP_H));
  layer_set_update_proc(s_scroll_indicator, scroll_indicator_update_proc);
  layer_add_child(root, s_scroll_indicator);

  return s_window;
}

void screen_laps_destroy(void) {
  layer_destroy(s_scroll_indicator);
  s_scroll_indicator = NULL;
  menu_layer_destroy(s_menu);
  s_menu = NULL;
  layer_destroy(s_strip);
  s_strip = NULL;
  window_destroy(s_window);
  s_window = NULL;
}

void screen_laps_set_cookie(int32_t cookie) {
  s_cookie = cookie;
}

void screen_laps_mark_clock_dirty(void) {
  if (s_strip != NULL) layer_mark_dirty(s_strip);
}
