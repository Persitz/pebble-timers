// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// Structurally mirrors screen_actions.c (MenuLayer + header + scroll
// indicator created ONCE, a non-reentrant custom click_config_provider
// -- NEVER menu_layer_set_click_config_onto_window(), see screen_actions.c's
// long comment for the reentrancy/dead-Select history this avoids) and reuses
// screen_set.c's already-proven short+long-Select-on-the-same-button
// coexistence (window_single_click_subscribe + window_long_click_subscribe)
// instead of the fill-bar hold_bind_select primitive.
#include "screen_library.h"

#include "app_context.h"
#include "lib/format.h"
#include "lib/preset_store.h"
#include "lib/store.h"
#include "lib/timer.h"
#include "lib/view.h"
#include "nav.h"
#include "persist_adapter.h"
#include "persist_preset_adapter.h"
#include "render.h"
#include "testlog_am.h"
#include "vibes_adapter.h"
#include "wakeup_adapter.h"
#ifdef GALLERY_MODE
#include "gallery.h"
#endif

// Reuses HOME's own header shape (paper + top-right clock + hairline at
// y=39, render_header_hairline) rather than ACTIONS'/the alarm-hero's
// colored floating strip -- Library isn't scoped to one timer/kind, so
// there's no single kind-color to band it in. Adds a small "LIBRARY" title
// ("LIBRARY" vs "PRESETS" is a cosmetic, non-blocking naming choice --
// "LIBRARY" chosen to match HOME's own "Library" row wording).
#define LIBRARY_HEADER_H 46

// The empty-library state is composed like HOME's own empty state -- muted
// ring+plus + "Save new" label. The old top-black-slab (\"Save new\"
// forced-selected, MenuLayer's own highlight) + orphaned hint (a separate
// sibling Layer floating mid-void) was diagnosed as unbalanced and stays
// retired.
// The empty state is now TWO rows, not one -- row 0 (this composed hero,
// "Save new") + row 1 ("Say a preset") -- mirroring HOME's own empty-state
// shape (fixed-height hero + a sibling action row below it) instead of the
// old single full-viewport row. LIBRARY_EMPTY_HERO_H/LIBRARY_RING_CENTER_Y/
// LABEL_Y are now IDENTICAL to HOME's own HOME_EMPTY_HERO_H/RING_CENTER_Y/
// LABEL_Y (screen_home.c) by deliberate choice -- the two hero rows occupy
// the same box and place their ring/label at the same local offsets.
// Row 1 draws via draw_library_voice_row_empty -- the SAME seat-divider +
// centered-composed-group idiom HOME's own draw_voice_row_empty uses (now
// shared, render.h's render_composed_group/render_composed_seat_divider),
// confined to HOME's own RENDER_MENU_ROW_H pitch, not the taller stretch-to-
// fill cell this row actually gets. "No presets yet" moves out of the hero
// (which never had one on HOME, matching that precedent) into the leftover
// space directly under "Say a preset".
#define LIBRARY_RING_CENTER_X 100
#define LIBRARY_RING_CENTER_Y 50  // local; abs 46+50=96 -- matches HOME's own RING_CENTER_Y exactly
#define LIBRARY_RING_RADIUS 20
#define LIBRARY_RING_STROKE_WIDTH 3
#define LIBRARY_PLUS_ARM_LEN 7
#define LIBRARY_PLUS_THICKNESS 3
#define LIBRARY_LABEL_Y 80  // matches HOME's own LABEL_Y exactly
#define LIBRARY_LABEL_HEIGHT 20
// Fixed hero height (matches HOME_EMPTY_HERO_H exactly) --
// row 1 ("Say a preset") absorbs whatever viewport remains below this, same
// "fixed hero + stretch-to-fill sibling" shape HOME's own empty state uses.
#define LIBRARY_EMPTY_HERO_H 112

#define SCROLL_INDICATOR_WIDTH 3
#define SCROLL_INDICATOR_MARGIN 3

// Hold-Select-for-options is undiscoverable on a preset row. The approach
// uses a transient hint (appears briefly, reverts, no permanent layout cost),
// similar to the stopwatch hero's up-hint pattern. The transient idiom is
// substituted into the header's title slot (the "LIBRARY" text) rather than
// a hero footer this screen doesn't have. Only shown once the list has >=1
// preset (an empty library has nothing to hold) -- see arm_library_hint()
// below.
#define LIBRARY_HINT_TRANSIENT_MS 2200

static Window *s_window;
static Layer *s_header;
static MenuLayer *s_menu;
static Layer *s_scroll_indicator;
// Transient discoverability hint: mirrors screen_hero.c's s_up_hint_active/
// s_up_hint_timer pair.
static AppTimer *s_hint_timer;
static bool s_hint_active;
// Hint arms once per app launch instead of once per window_appear, to avoid
// repetitive re-showing to users who already know the gesture. The hint shown
// flag remains independent of s_hint_active (used by gallery debug lever
// under GALLERY_MODE).
static bool s_hint_shown_this_launch;

static int preset_count(void) {
  return app_context_presets()->count;
}

static uint16_t get_num_rows(MenuLayer *menu_layer, uint16_t section_index, void *context) {
  (void)menu_layer;
  (void)section_index;
  (void)context;
  // +2 = "Save new" + "Say a preset" (view_library_row_count is the
  // single source of truth this file's draw_row/select_click dispatch and
  // screen_voice.c's preset-target voice-cancel cursor-restore both share).
  return (uint16_t)view_library_row_count(preset_count());
}

// Preset rows use render_row's larger pill (matching HOME's own timer rows);
// the trailing "Save new" row uses render_action_row's plain action-pill pitch
// instead (matching HOME's own footer rows) -- two legitimately different row
// shapes, not a stretch/footer special case. For a list whose every row is
// real content (like LAPS), use a plain fixed pitch per row, no viewport-
// filling stretch. The empty state (0 presets) is a single composed hero
// occupying the whole viewport instead (see draw_library_empty_hero) -- no
// separate hint overlay needed.
static int16_t get_cell_height(MenuLayer *menu_layer, MenuIndex *cell_index, void *context) {
  (void)context;
  int count = preset_count();
  if (count == 0) {
    // Row 0 = the fixed-height composed hero ("Save new"); row 1 = "Say a
    // preset," absorbing whatever viewport remains below it (the same "fixed
    // hero + stretch-to-fill sibling" shape HOME's own empty state uses for
    // its own trailing row).
    if (cell_index->row == 0) return LIBRARY_EMPTY_HERO_H;
    int16_t viewport_h = (int16_t)layer_get_bounds(menu_layer_get_layer(menu_layer)).size.h;
    int16_t remaining = (int16_t)(viewport_h - LIBRARY_EMPTY_HERO_H);
    return (remaining > RENDER_MENU_ROW_H) ? remaining : RENDER_MENU_ROW_H;
  }
  return (cell_index->row < (uint16_t)count) ? RENDER_ROW_H : RENDER_MENU_ROW_H;
}

// Composed empty-library hero -- see this file's own LIBRARY_RING_* constants
// doc comment above for the full geometry rationale (echoes HOME's own
// empty-hero formula). `cell_bounds` is this row's own cell-local bounds (the
// full viewport, per get_cell_height above).
// Same fix as HOME's own draw_new_timer_hero -- this custom draw suppresses
// the cursor pill, so it needs its own selection affordance. This hero's
// "full color" was already deliberately muted (GColorDarkGray, not orange --
// a secondary create affordance), so the unselected/dim state steps one shade
// lighter (GColorLightGray) instead of introducing a second color scale.
static void draw_library_empty_hero(GContext *ctx, GRect cell_bounds, bool highlighted) {
  GPoint center = GPoint(LIBRARY_RING_CENTER_X, LIBRARY_RING_CENTER_Y);
  GColor ring_color = highlighted ? GColorDarkGray : GColorLightGray;

  // Muted (GColorDarkGray when selected), not orange -- this is a SECONDARY
  // create affordance (Library's own "Save new"), not HOME's primary "New
  // timer" -- a muted ring + "+" echoing HOME's own empty hero, deliberately
  // less prominent than the primary create action.
  graphics_context_set_stroke_color(ctx, ring_color);
  graphics_context_set_stroke_width(ctx, LIBRARY_RING_STROKE_WIDTH);
  graphics_draw_circle(ctx, center, LIBRARY_RING_RADIUS);

  graphics_context_set_fill_color(ctx, ring_color);
  GRect h_bar = GRect(center.x - LIBRARY_PLUS_ARM_LEN, center.y - LIBRARY_PLUS_THICKNESS / 2,
                       2 * LIBRARY_PLUS_ARM_LEN, LIBRARY_PLUS_THICKNESS);
  GRect v_bar = GRect(center.x - LIBRARY_PLUS_THICKNESS / 2, center.y - LIBRARY_PLUS_ARM_LEN,
                       LIBRARY_PLUS_THICKNESS, 2 * LIBRARY_PLUS_ARM_LEN);
  graphics_fill_rect(ctx, h_bar, 0, GCornerNone);
  graphics_fill_rect(ctx, v_bar, 0, GCornerNone);

  GRect label_box = GRect(0, LIBRARY_LABEL_Y, cell_bounds.size.w, LIBRARY_LABEL_HEIGHT);
  graphics_context_set_text_color(ctx, RENDER_COLOR_LABEL);
  graphics_draw_text(ctx, "Save new", fonts_get_system_font(FONT_KEY_GOTHIC_14), label_box,
                      GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  // "No presets yet" no longer lives in this hero -- it moved to sit
  // directly under "Say a preset" (see draw_library_voice_row_empty below),
  // now that the empty state has a real sibling row to anchor it to (mirroring
  // HOME's own hero, which carries no hint text at all).
}

// Pixel-matches HOME's own empty-state "Say a timer" treatment
// (screen_home.c's draw_voice_row_empty) -- the seat divider at this row's
// own top edge, then the SAME render_composed_group() call (shared primitive,
// render.h) confined to a RENDER_MENU_ROW_H-tall sub-cell (HOME's own fixed
// pitch for this row, reused verbatim rather than centering the group within
// the FULL, taller-than-HOME's stretch-to-fill cell this row actually gets).
// "No presets yet" fills the leftover space below that sub-cell, centered on
// the same x-axis, muted -- directly under the action it explains.
static void draw_library_voice_row_empty(GContext *ctx, GRect cell_bounds, bool highlighted) {
  render_composed_seat_divider(ctx, cell_bounds);
  GRect group_bounds =
      GRect(cell_bounds.origin.x, cell_bounds.origin.y, cell_bounds.size.w, RENDER_MENU_ROW_H);
  render_composed_group(ctx, group_bounds, highlighted, render_mic_glyph, "Say a preset");

  int16_t hint_y = (int16_t)(cell_bounds.origin.y + RENDER_MENU_ROW_H);
  int16_t hint_h = (int16_t)(cell_bounds.size.h - RENDER_MENU_ROW_H);
  if (hint_h > 0) {
    GRect hint_box = GRect(cell_bounds.origin.x, hint_y, cell_bounds.size.w, hint_h);
    // Text tier darkened to RENDER_COLOR_MUTED_TEXT (black) -- see render.h's
    // own doc comment on that constant.
    graphics_context_set_text_color(ctx, RENDER_COLOR_MUTED_TEXT);
    graphics_draw_text(ctx, "No presets yet", fonts_get_system_font(FONT_KEY_GOTHIC_14), hint_box,
                        GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  }
}

static void draw_row(GContext *ctx, const Layer *cell_layer, MenuIndex *cell_index, void *context) {
  (void)context;
  GRect bounds = layer_get_bounds(cell_layer);
  bool highlighted = menu_cell_layer_is_highlighted(cell_layer);
  int count = preset_count();

  if (count == 0) {
    // Row 0 = the composed hero ("Save new"); row 1 = "Say a preset," via the
    // SAME centered composed idiom HOME's own empty state uses for "Say a
    // timer" (draw_library_voice_row_empty, above) -- NOT the full-width-pill
    // render_action_row_mic the populated list's own trailing "Say a preset"
    // row uses just below. Two legitimately different treatments, matching
    // HOME's own populated-vs-empty split.
    if (cell_index->row == 0) {
      draw_library_empty_hero(ctx, bounds, highlighted);
    } else {
      draw_library_voice_row_empty(ctx, bounds, highlighted);
    }
    return;
  }

  if ((int)cell_index->row == count) {
    // "Save new" now shares the same content-rail pill as the preset rows
    // above it (render_row's own inset/radius) instead of render_menu_row's
    // narrower rail.
    render_action_row(ctx, bounds, highlighted, "Save new", true);
    return;
  }

  if ((int)cell_index->row > count) {
    // "Say a preset" -- deliberately ungated at MAX_PRESETS, mirroring
    // "Save new"'s own precedent. A preset dial costs nothing to open even
    // when full, unlike HOME's MAX_TIMERS-gated "Say a timer" row.
    render_action_row_mic(ctx, bounds, highlighted, "Say a preset", true);
    return;
  }

  const Preset *p = preset_store_get(app_context_presets(), (int)cell_index->row);
  if (p == NULL) return;  // defensive -- store mutated out from under a stale row index

  // Preset rows show glyph + kind color + duration only, no label text
  // (avoids truncation defect a generic kind-word label would reproduce at
  // this row width). Unnamed presets get LEFT-aligned duration with trailing
  // chevron. Named presets instead reuse the exact label-slot treatment
  // HOME's own named timer rows use (screen_home.c's draw_row) -- label left,
  // number right-aligned, no chevron. This asymmetry is required since the
  // left-aligned+chevron layout has no room for a label. Unnamed rows stay
  // byte-identical to the initial layout.
  GColor kind_color = (p->kind == KIND_REPEAT) ? GColorIndigo : GColorOrange;
  // Same darkened-value-text fix as HOME/the ringing list -- this row's
  // "kind color" is hardcoded here (not routed through render_kind_color(),
  // a pre-existing local pattern) rather than resolved via a ViewColorToken.
  // Presets are only ever countdown or repeat -- repeat's indigo is already
  // dark enough; countdown's orange gets the same GColorWindsorTan swap as
  // everywhere else.
  GColor value_color = (p->kind == KIND_REPEAT) ? GColorIndigo : GColorWindsorTan;
  char value[16];
  format_hms(value, sizeof value, p->duration_s);
  const char *label = view_row_label(p->name);  // NULL for "" -- the degrade default
  bool name_less = (label == NULL);
  RenderRowSpec spec = {
      .highlighted = highlighted,
      .kind = p->kind,
      .kind_color = kind_color,
      .value_color = value_color,
      .label = label,
      .number = value,
      .number_font = render_home_value_font(VIEW_HOME_SIZE_LARGE),
      .number_prefix = VIEW_PREFIX_NONE,
      .show_bar = false,  // a preset is a template, never counting down
      .bar_fraction_pm = 0,
      .number_left_align = name_less,
      .trailing_glyph = name_less,
  };
  render_row(ctx, bounds, &spec);
}

// Launching a preset calls store_add + the same persist/replan/nav shape
// every other mutate-and-navigate call site uses (screen_actions.c's
// persist_and_replan, screen_set.c's start_timer). On refusal (TimerStore
// already at MAX_TIMERS), stays put and reuses the create flow's own
// "store","full" testlog tag + reject-vibe (same as HOME's own MAX-reached
// footer).
static void launch_preset(int index) {
  const Preset *p = preset_store_get(app_context_presets(), index);
  if (p == NULL) return;  // defensive -- stale row index
  TimerStore *store = app_context_store();
  time_t now = time(NULL);
  int32_t cookie;
  if (!store_add(store, p->kind, p->duration_s, now, &cookie)) {
    testlog("store", "full");
    vibes_adapter_reject();
    return;
  }
  // A named preset's name flows onto the freshly-created Timer, before
  // persisting it -- Preset.name(16) always fits Timer.name(24). A name-less
  // preset never calls store_set_name, so its launched timer stays name-less.
  if (p->name[0] != '\0') store_set_name(store, cookie, p->name);
  persist_adapter_save(store);
  wakeup_adapter_replan(store, now);
  testlog("preset_launch", p->kind == KIND_REPEAT ? "repeat" : "countdown");
  nav_go_hero(cookie);
}

static void select_click(MenuLayer *menu_layer, MenuIndex *cell_index, void *context) {
  (void)menu_layer;
  (void)context;
  int count = preset_count();
  if ((int)cell_index->row < count) {
    launch_preset((int)cell_index->row);
    return;
  }
  if ((int)cell_index->row == count) {
    nav_go_preset_new();  // "Save new" row (also the empty-state hero, row 0)
    return;
  }
  nav_go_voice_preset();  // "Say a preset" row
}

static void select_click_handler(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
  MenuIndex idx = menu_layer_get_selected_index(s_menu);
  select_click(s_menu, &idx, NULL);
}

// Hold Select (700ms) opens the {Edit, Delete} actions sheet for the row
// under the cursor. A no-op on the "Save new" row (nothing to manage there)
// -- an explicit no-op branch matching the codebase's established convention.
static void select_long_click_handler(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
  MenuIndex idx = menu_layer_get_selected_index(s_menu);
  int count = preset_count();
  // "Save new" (row count) or "Say a preset" (row count+1) -- neither
  // is a preset row to manage; this check already excludes both.
  if ((int)idx.row >= count) return;
  nav_go_preset_actions((int)idx.row);
}

static void up_click_handler(ClickRecognizerRef recognizer, void *context) {
  (void)context;
  // MenuLayer clamps (no wrap) at row 0 -- Up-at-top reverses the Down that
  // opened this screen, mirroring back_click_handler below. Only a discrete
  // press at row 0 reverses -- a held/auto-repeating Up landing on row 0 falls
  // through to the menu_layer_set_selected_next() clamp instead.
  if (menu_layer_get_selected_index(s_menu).row == 0 &&
      !click_recognizer_is_repeating(recognizer)) {
    nav_go_home();
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
  nav_go_home();
}

static void click_config_provider(void *context) {
  (void)context;
  window_single_repeating_click_subscribe(BUTTON_ID_UP, 100, up_click_handler);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 100, down_click_handler);
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click_handler);
  window_long_click_subscribe(BUTTON_ID_SELECT, 700, select_long_click_handler, NULL);
  window_single_click_subscribe(BUTTON_ID_BACK, back_click_handler);
}

// Transient discoverability hint: mirrors screen_hero.c's
// cancel_up_hint()/up_hint_timer_cb()/sw_up_short_hint() shape, substituted
// into this screen's header title slot instead of a hero footer. `#ifdef
// GALLERY_MODE` exposes screen_library_debug_force_hint() so a dedicated
// gallery scene can freeze this state for a screenshot.
static void cancel_library_hint(void) {
  if (s_hint_timer != NULL) {
    app_timer_cancel(s_hint_timer);
    s_hint_timer = NULL;
  }
  s_hint_active = false;
}

static void library_hint_timer_cb(void *data) {
  (void)data;
  s_hint_timer = NULL;
  s_hint_active = false;
  if (s_header != NULL) layer_mark_dirty(s_header);
}

static void arm_library_hint(void) {
  cancel_library_hint();
  if (preset_count() == 0) return;  // nothing to hold -- an empty library has no rows to manage
  // Once per app launch, not once per visit -- an empty-Library
  // visit (the check just above) deliberately does NOT consume this, so the
  // hint still shows on the first visit that actually has >=1 preset to hold,
  // whenever that turns out to be, rather than being silently burned by an
  // earlier empty visit that showed nothing.
  if (s_hint_shown_this_launch) return;
  s_hint_shown_this_launch = true;
  s_hint_active = true;
  s_hint_timer = app_timer_register(LIBRARY_HINT_TRANSIENT_MS, library_hint_timer_cb, NULL);
}

static void window_appear(Window *window) {
  (void)window;
  // Mirrors screen_actions.c's own reload-on-appear reasoning: a return
  // from Save-new/Edit/Delete must reflect the mutated list -- reload +
  // MenuRowAlignTop resets both the logical selection AND the scroll
  // viewport to follow it (MenuRowAlignNone would reset only the former).
  menu_layer_reload_data(s_menu);

  // Voice-into-Library: a Save/cancel-from-preset-target-voice
  // restores the cursor onto a specific row (nav_go_library_focus_row) --
  // one-shot read-and-clear, mirroring screen_home.c's own nav_take_home_
  // focus_cookie() consumption. -1 (no pending focus, every OTHER path back
  // to Library) keeps the pre-existing "always land on row 0/top" default.
  // Defensively re-clamped against the CURRENT row count -- a focus row is
  // only ever set to a value valid at the instant it was set, but re-check
  // here anyway rather than trust that invariant blindly (the same
  // dead-reference caution store_home_row_for_cookie's own -1 contract
  // documents for HOME's cookie-based focus).
  int focus_row = nav_take_library_focus_row();
  int total_rows = view_library_row_count(preset_count());
  bool has_focus = focus_row >= 0 && focus_row < total_rows;
  MenuIndex idx = MenuIndex(0, (uint16_t)(has_focus ? focus_row : 0));
  MenuRowAlign align = has_focus ? MenuRowAlignCenter : MenuRowAlignTop;
  menu_layer_set_selected_index(s_menu, idx, align, false);
  if (s_scroll_indicator != NULL) layer_mark_dirty(s_scroll_indicator);

#ifndef GALLERY_MODE
  // Real app only -- gated so every existing/new gallery scene's pixels stay
  // deterministic (a wall-clock AppTimer firing mid-capture would otherwise
  // make an ARBITRARY prior scene's transition decide whether THIS scene's
  // screenshot shows "LIBRARY" or the transient hint, see this file's own
  // constants-block comment). A dedicated gallery scene instead calls
  // screen_library_debug_force_hint() to show the hint state on demand.
  arm_library_hint();
#endif
}

// "LIBRARY" uses GOTHIC_18_BOLD to read as a real screen title instead of
// a caption sitting smaller than the "Save new" row beneath it -- echoes the
// hero header's own bold label weight.
static void header_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, RENDER_COLOR_PAPER);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  render_clock(ctx, bounds, clock_is_24h_style());
  GRect title_rect = GRect(RENDER_MARGIN_X, 12, 130, 22);
  // While active, temporarily substitutes "hold a preset for options"
  // (small, muted) into this same title slot instead of "LIBRARY" (bold, ink)
  // -- reverts automatically (library_hint_timer_cb) after
  // LIBRARY_HINT_TRANSIENT_MS. Chosen over a persistent second line (no spare
  // vertical room in this fixed 46px header once the clock+title+hairline
  // already occupy it) or a per-row hint (would crowd every preset row).
  const char *title = s_hint_active ? "hold a preset for options" : "LIBRARY";
  GFont title_font = fonts_get_system_font(s_hint_active ? FONT_KEY_GOTHIC_14 : FONT_KEY_GOTHIC_18_BOLD);
  // Text tier darkened to RENDER_COLOR_MUTED_TEXT (black) -- see render.h's
  // own doc comment.
  GColor title_color = s_hint_active ? RENDER_COLOR_MUTED_TEXT : RENDER_COLOR_INK;
  graphics_context_set_text_color(ctx, title_color);
  graphics_draw_text(ctx, title, title_font, title_rect, GTextOverflowModeTrailingEllipsis,
                      GTextAlignmentLeft, NULL);
  render_header_hairline(ctx, bounds);
}

// A thin 3px scroll indicator, same shared component as every other list
// screen (HOME/ACTIONS/LAPS/RINGING). `visible` uses RENDER_ROW_H as the
// representative per-row pitch -- Library's rows are NOT uniform height
// (preset rows are RENDER_ROW_H=46, the trailing "Save new" row is
// RENDER_MENU_ROW_H=38), so this slightly under-counts how many rows actually
// fit (by at most one, only when "Save new" is on-screen) -- a cosmetic-only
// imprecision, same kind of minor tolerance used elsewhere in this codebase.
static void scroll_indicator_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  int total = preset_count() + 1;
  int visible = (int)(bounds.size.h / RENDER_ROW_H);
  if (visible < 1) visible = 1;
  MenuIndex sel = menu_layer_get_selected_index(s_menu);
  render_scroll_indicator(ctx, bounds, total, visible, (int)sel.row);
}

// Everything is created ONCE here, never re-allocated across navigation
// (avoids MenuLayer churn, consistent with other list screens).
Window *screen_library_create(void) {
  s_window = window_create();
  window_set_background_color(s_window, RENDER_COLOR_PAPER);
#ifdef GALLERY_MODE
  // Screen gallery mode -- never compiled into a release build.
  window_set_click_config_provider(s_window, gallery_click_config_provider);
#else
  window_set_click_config_provider(s_window, click_config_provider);
#endif
  window_set_window_handlers(s_window, (WindowHandlers){
                                            .appear = window_appear,
                                        });

  Layer *root = window_get_root_layer(s_window);
  GRect bounds = layer_get_bounds(root);

  s_header = layer_create(GRect(0, 0, bounds.size.w, LIBRARY_HEADER_H));
  layer_set_update_proc(s_header, header_update_proc);
  layer_add_child(root, s_header);

  s_menu = menu_layer_create(
      GRect(0, LIBRARY_HEADER_H, bounds.size.w, bounds.size.h - LIBRARY_HEADER_H));
  // Disable the SDK's default pad_bottom (an un-disabled pad_bottom lets
  // the scroll offset over-scroll 20px past the real content bottom, garbling
  // the top row's numeral at max-scroll) -- mirroring screen_home.c's own fix.
  menu_layer_pad_bottom_enable(s_menu, false);
  // An un-called MenuLayer defaults its BEYOND-CONTENT background to white,
  // not this window's paper -- override it. Kept even though the composed
  // empty hero fills its own row's full viewport height, since a populated
  // library scrolled to a short list can still show real BEYOND-CONTENT space
  // beneath the "Save new" row.
  menu_layer_set_normal_colors(s_menu, RENDER_COLOR_PAPER, RENDER_COLOR_INK);
  // The SDK's own highlight default is a full-cell black fill, which would
  // show as a black band around render_row()'s/render_menu_row()'s own smaller
  // inset pill -- override it (same fix as every other MenuLayer).
  menu_layer_set_highlight_colors(s_menu, RENDER_COLOR_PAPER, GColorBlack);
  menu_layer_set_callbacks(s_menu, NULL,
                            (MenuLayerCallbacks){
                                .get_num_rows = get_num_rows,
                                .get_cell_height = get_cell_height,
                                .draw_row = draw_row,
                            });
  layer_add_child(root, menu_layer_get_layer(s_menu));

  s_scroll_indicator = layer_create(GRect(bounds.size.w - SCROLL_INDICATOR_WIDTH - SCROLL_INDICATOR_MARGIN,
                                           LIBRARY_HEADER_H, SCROLL_INDICATOR_WIDTH,
                                           bounds.size.h - LIBRARY_HEADER_H));
  layer_set_update_proc(s_scroll_indicator, scroll_indicator_update_proc);
  layer_add_child(root, s_scroll_indicator);

  return s_window;
}

void screen_library_destroy(void) {
  cancel_library_hint();  // defensive cleanup
  layer_destroy(s_scroll_indicator);
  s_scroll_indicator = NULL;
  menu_layer_destroy(s_menu);
  s_menu = NULL;
  layer_destroy(s_header);
  s_header = NULL;
  window_destroy(s_window);
  s_window = NULL;
}

void screen_library_mark_clock_dirty(void) {
  if (s_header != NULL) layer_mark_dirty(s_header);
}

#ifdef GALLERY_MODE
MenuLayer *screen_library_debug_menu(void) {
  return s_menu;
}

// Debug-only lever for the screen gallery: mirrors
// screen_hero_debug_force_up_hint()'s contract -- sets the transient-hint
// state directly (bypassing the real AppTimer) so a scene can show it
// deterministically. Never compiled into a release build.
void screen_library_debug_force_hint(bool active) {
  s_hint_active = active;
  if (s_header != NULL) layer_mark_dirty(s_header);
}
#endif
