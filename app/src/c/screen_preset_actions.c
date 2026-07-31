// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// The preset actions sheet. Structurally mirrors screen_actions.c at a much smaller
// scale (a fixed 3-row menu with a non-reentrant custom click_config_provider).
//
// Menu rows: {Edit, Rename, Delete}. Edit opens the SET dial. Rename starts a
// preset name re-dictation, with the transcript landing asynchronously via
// screen_preset_actions_apply_name(). Delete removes the preset.
//
// The floating strip shows a PRESET's static duration (a template never counts down),
// so there is no per-tick clock refresh to wire. window_appear's reload keeps the
// rows/strip correct across any preset mutation that could re-enter this screen.
#include "screen_preset_actions.h"

#include "app_context.h"
#include "dictation_am.h"
#include "lib/format.h"
#include "lib/preset_store.h"
#include "lib/timer.h"
#include "nav.h"
#include "persist_preset_adapter.h"
#include "render.h"
#include "testlog_am.h"

#define ROW_EDIT 0
#define ROW_RENAME 1
#define ROW_DELETE 2
#define NUM_ROWS 3

static Window *s_window;
static Layer *s_strip;
static MenuLayer *s_menu;
static Layer *s_scroll_indicator;
static int s_index;  // preset index this sheet acts on (index-addressed, no cookie)

// Bug fix: this sheet's Rename row never navigates away, so when the OS reveals
// this window again after the dictation UI closes (Success or cancel), it used to
// always reset the cursor to row 0 via window_appear's unconditional MenuIndex(0,0).
// Solution: -1 = no pending restore; set only by ROW_RENAME's own select_click
// branch, cleared by screen_preset_actions_set_index() on every genuine fresh entry
// to prevent a stale pending row from a previous Rename attempt leaking onto an
// unrelated later visit.
static int s_pending_row = -1;

static uint16_t get_num_rows(MenuLayer *menu_layer, uint16_t section_index, void *context) {
  (void)menu_layer;
  (void)section_index;
  (void)context;
  return NUM_ROWS;
}

static int16_t get_cell_height(MenuLayer *menu_layer, MenuIndex *cell_index, void *context) {
  (void)menu_layer;
  (void)cell_index;
  (void)context;
  return RENDER_MENU_ROW_H;
}

static const char *row_title(uint16_t row) {
  switch (row) {
    case ROW_EDIT: return "Edit";
    case ROW_RENAME: return "Rename";
    default: return "Delete";  // ROW_DELETE
  }
}

static void draw_row(GContext *ctx, const Layer *cell_layer, MenuIndex *cell_index, void *context) {
  (void)context;
  render_menu_row(ctx, layer_get_bounds(cell_layer), menu_cell_layer_is_highlighted(cell_layer),
                   row_title(cell_index->row));
}

// Select dispatch: Edit opens the SET dial pre-populated from this preset.
// Rename starts a DICT_SCOPE_PRESET_NAME re-dictation, with the transcript
// landing asynchronously via screen_preset_actions_apply_name(). Delete removes
// it from the PresetStore and returns to Library. No operation touches the
// TimerStore (presets never enter the timer lifecycle).
static void select_click(MenuLayer *menu_layer, MenuIndex *cell_index, void *context) {
  (void)menu_layer;
  (void)context;
  if (cell_index->row == ROW_EDIT) {
    nav_go_preset_edit(s_index);
    return;
  }
  if (cell_index->row == ROW_RENAME) {
    // Remember this row so the eventual re-appearance restores the cursor
    // here instead of resetting to row 0 (see s_pending_row's doc comment
    // above).
    s_pending_row = cell_index->row;
    dictation_am_start(DICT_SCOPE_PRESET_NAME);
    return;
  }
  // ROW_DELETE
  preset_store_remove(app_context_presets(), s_index);
  persist_preset_adapter_save(app_context_presets());
  testlog("preset_delete", "ok");
  nav_go_library();
}

static void select_click_handler(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
  MenuIndex idx = menu_layer_get_selected_index(s_menu);
  select_click(s_menu, &idx, NULL);
}

// ALARM_OPTIONS-style up-at-top (screen_alarm.c's options_up_click_handler):
// this sheet is reached via a hold gesture from Library, same shape as
// ALARM_OPTIONS being reached via Down from an alarm hero -- Up on the top
// row reverses that, returning to Library instead of leaving Up a silent
// clamp-at-row-0 no-op.
static void up_click_handler(ClickRecognizerRef recognizer, void *context) {
  (void)context;
  // Only a DISCRETE press at row 0 reverses -- a held/auto-repeating Up landing
  // on row 0 falls through to the menu_layer_set_selected_next() clamp instead
  // of cancelling out.
  if (menu_layer_get_selected_index(s_menu).row == 0 &&
      !click_recognizer_is_repeating(recognizer)) {
    nav_go_library();
    return;
  }
  menu_layer_set_selected_next(s_menu, true, MenuRowAlignCenter, true);
  if (s_scroll_indicator != NULL) layer_mark_dirty(s_scroll_indicator);
}

static void down_click_handler(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
  menu_layer_set_selected_next(s_menu, false, MenuRowAlignCenter, true);
  if (s_scroll_indicator != NULL) layer_mark_dirty(s_scroll_indicator);
}

static void back_click_handler(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
  nav_go_library();
}

static void click_config_provider(void *context) {
  (void)context;
  window_single_repeating_click_subscribe(BUTTON_ID_UP, 100, up_click_handler);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 100, down_click_handler);
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click_handler);
  window_single_click_subscribe(BUTTON_ID_BACK, back_click_handler);
}

static void window_appear(Window *window) {
  (void)window;
  menu_layer_reload_data(s_menu);
  // Use MenuRowAlignCenter for the restore, matching screen_actions.c's own fix
  // and the reasoning for preferring Center over Top on a restored selection.
  if (s_pending_row >= 0) {
    menu_layer_set_selected_index(s_menu, MenuIndex(0, (uint16_t)s_pending_row), MenuRowAlignCenter,
                                   false);
    s_pending_row = -1;
  } else {
    menu_layer_set_selected_index(s_menu, MenuIndex(0, 0), MenuRowAlignTop, false);
  }
  if (s_scroll_indicator != NULL) layer_mark_dirty(s_scroll_indicator);
}

// The floating colored strip header (same primitive/height as ACTIONS' own,
// RENDER_STRIP_H) showing the preset's own kind glyph+colour
// (GColorOrange/GColorIndigo, same recipe as Library's rows) + kind label
// (render_kind_label -- the strip shows the kind label here, not the preset's
// own name, same layout HOME/ACTIONS already use) + its static duration. No
// clock, no per-tick refresh (see this file's own top-of-file comment).
static void strip_update_proc(Layer *layer, GContext *ctx) {
  const Preset *p = preset_store_get(app_context_presets(), s_index);
  if (p == NULL) return;  // defensive -- stale index (e.g. reached mid-mutation)
  // Use DEEP-variant color for countdown presets. Indigo (repeat) is already
  // dark enough. Presets are only ever countdown/repeat, never stopwatch/alarm.
  GColor kind_color = (p->kind == KIND_REPEAT) ? GColorIndigo : RENDER_COLOR_COUNTDOWN_DEEP;
  char value[16];
  format_hms(value, sizeof value, p->duration_s);
  render_floating_strip(ctx, layer_get_bounds(layer), kind_color, p->kind,
                         render_kind_label(p->kind), value);
}

// Scroll indicator overlay, wired for uniformity with every other menu screen
// even though it never overflows the viewport at RENDER_MENU_ROW_H's pitch --
// render_scroll_indicator self-skips when total_rows <= visible_rows.
static void scroll_indicator_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  int visible = (int)(bounds.size.h / RENDER_MENU_ROW_H);
  if (visible < 1) visible = 1;
  MenuIndex sel = menu_layer_get_selected_index(s_menu);
  render_scroll_indicator(ctx, bounds, NUM_ROWS, visible, (int)sel.row);
}

Window *screen_preset_actions_create(void) {
  RenderMenuScreen screen = render_build_menu_screen(&(RenderMenuScreenSpec){
      .window_bg = RENDER_COLOR_PAPER,
      .click_config_provider = click_config_provider,
      .window_handlers = (WindowHandlers){.appear = window_appear},
      .header_height = RENDER_STRIP_H,
      .header_update_proc = strip_update_proc,
      // Disable the SDK's default pad_bottom (required for all MenuLayer instances).
      .pad_bottom_disable = true,
      .get_num_rows = get_num_rows,
      .get_cell_height = get_cell_height,
      .draw_row = draw_row,
      .with_scroll_indicator = true,
      .scroll_indicator_update_proc = scroll_indicator_update_proc,
  });
  s_window = screen.window;
  s_strip = screen.header;
  s_menu = screen.menu;
  s_scroll_indicator = screen.scroll_indicator;
  return s_window;
}

void screen_preset_actions_destroy(void) {
  render_destroy_menu_screen(&(RenderMenuScreen){
      .window = s_window, .header = s_strip, .menu = s_menu, .scroll_indicator = s_scroll_indicator});
  s_scroll_indicator = NULL;
  s_menu = NULL;
  s_strip = NULL;
  s_window = NULL;
}

void screen_preset_actions_set_index(int index) {
  s_index = index;
  // Only called right before a genuine fresh push onto this sheet -- clearing
  // here means a stale pending row from an earlier interrupted Rename attempt
  // can never leak onto an unrelated later visit.
  s_pending_row = -1;
}

// The Rename production seam -- reused naming logic (preset_store_set_name
// routes through timer_name_copy, the same trim+UTF-8-safe-cap primitive
// screen_voice_set_name uses). Persisted immediately (unlike
// screen_voice_set_name's in-preview staging), then a menu reload so this
// sheet's rows reflect the new name. The Library screen behind it shows the
// new name on return.
void screen_preset_actions_apply_name(const char *name) {
  preset_store_set_name(app_context_presets(), s_index, name);
  persist_preset_adapter_save(app_context_presets());
  testlog("preset_rename", "ok");
  if (s_menu != NULL) menu_layer_reload_data(s_menu);
}
