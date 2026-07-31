// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

#include "screen_home.h"

#include "app_context.h"
#include "lib/app_state.h"
#include "lib/format.h"
#include "lib/store.h"
#include "lib/timer.h"
#include "lib/view.h"
#include "nav.h"
#include "render.h"
#include "vibes_adapter.h"

// Kind/paper palette -- paper background #EFECE4, ink text #2A2620.
// Aliased onto the shared constant (render.h, already included) instead of
// duplicating the literal -- several files were each repeating a value
// render.h already exported.
#define COLOR_PAPER RENDER_COLOR_PAPER
#define COLOR_INK GColorFromHEX(0x2A2620)

// Injected at build time from package.json's version (wscript) -- single
// source of truth. Fallback keeps the file compilable if ever built without
// the define (e.g. a standalone compile); a real build always overrides it.
#ifndef APP_VERSION_STR
#define APP_VERSION_STR "dev"
#endif

// The empty-state hero row ("New timer") and the Library row below it form
// a single designed two-action group, not a hero with a plain footer row
// bolted underneath it. All Y constants below are LOCAL to the hero row's
// own cell bounds (a MenuLayer draw_row's `layer_get_bounds(cell_layer)` is
// cell-relative, origin (0,0)) -- the hero row is fixed at HOME_EMPTY_HERO_H
// tall (see get_cell_height's empty-state branch), so local-to-absolute is
// always `+HOME_HEADER_H` (46).
#define RING_CENTER_X 100
// Ring center sits at absolute y=96 (96-46=50 local), leaving room for the
// divider and Library group below it within the hero row's fixed height.
#define RING_CENTER_Y 50
#define RING_RADIUS 20
// Stroke width is 3px -- heavy enough to read clearly against the paper
// background without overwhelming the "+" glyph inside it.
#define RING_STROKE_WIDTH 3
#define PLUS_ARM_LEN 7       // half-length of each plus bar, px
#define PLUS_THICKNESS 3

// "New timer" label sits at abs y=126 (126-46=80 local).
#define LABEL_Y 80
#define LABEL_HEIGHT 20

// The hero row's own fixed height -- the divider/"seat" for the composed
// Library row below sits exactly at this row's bottom edge (abs
// 46+112=158). Fixed (not dynamically stretched to fill the viewport) -- a
// deliberate choice: this is a fixed two-part composition, not a content
// list whose last row bottom-anchors. See draw_library_row below for the
// divider + the rest of the composition.
#define HOME_EMPTY_HERO_H 112

// --- composed "Library" row (row 2 when count == 0) -------------------------
// Glyph + "Library" centered as one group on the x=100 axis, echoing "New
// timer"'s own centered composition. Cursor is a text-sized centered black
// chip (a pill sized to content, radius = h/2), not a full-width pill.
//
// This row's geometry is derived from its actual cell height (see
// draw_library_row below) rather than fixed absolute offsets, because
// inserting the "Say a timer" row between the hero and this row leaves this
// row a much smaller share of the empty-state viewport than before -- not
// enough room for a two-line layout (group line plus a separate version-
// marker line). The version marker now sits on the same line as the group,
// right-aligned to the content rail's own right edge.
//
// Row 1 (\"Say a timer\") uses the same centered composed treatment as this
// row (rather than a left-aligned full-width rail) so all three rows --
// hero, \"Say a timer\", and Library -- share the same x~99-100 visual center;
// a short centered \"seat\" divider sits at row 1's own top edge in place of a
// full-width hairline.
//
// The composed-group and seat-divider drawing helpers live in render.h/
// render.c (render_composed_group()/render_composed_seat_divider()) rather
// than as local statics here, so the Library screen's own empty-state \"Say a
// preset\" row can share the identical geometry instead of a second
// hand-tuned copy.
//
// The populated HOME view is a real MenuLayer (native scrolling/cursor/
// select/highlight) with a custom draw_row. draw_row delegates to the shared
// render_row() component (a rounded pill inset with a hairline separator,
// rather than an invisible fill), plus a thin scroll-indicator overlay Layer
// (draw_row can only paint inside a cell, so the indicator needs its own
// sibling Layer) and a clock-strip Layer above the menu.
#define MAX_VISIBLE_ROWS 4  // header + 4 rows visible before the list scrolls
// The indicator sits at x=194 (200-3-3=194), not x=195 (200-3-2) -- see
// render.h's RENDER_SCROLL_INDICATOR_WIDTH/_MARGIN.

static Window *s_window;
static Layer *s_clock_strip;    // top clock strip above the menu, always visible
static MenuLayer *s_menu;       // the row list, both empty and populated states
static Layer *s_scroll_indicator;

// --- empty-state hero row (row 0 when count == 0) ---------------------------
// HOME is unified onto one MenuLayer in both states -- this used to be
// `s_empty_layer`, a separate full-root-layer custom Layer, toggled
// hidden/visible against the populated MenuLayer. Deleted; the ring+plus
// affordance is now drawn as row 0's own content when count==0 (see draw_row
// below), and the MenuLayer already owns Up/Down/Select input in both states
// (unchanged -- menu_layer_set_click_config_onto_window routes clicks
// regardless of the row's visual content).

// Vector-drawn ring + plus glyph (never a font glyph) -- avoids relying on
// the fullwidth "+" character being present in the system font's glyph
// tables. `cell_bounds` is the hero row's own cell-local bounds (origin
// (0,0)) -- the constants above are calibrated against this local space so
// the ring lands at the same on-screen pixel position it always has.
//
// The MenuLayer cursor pill is deliberately suppressed for this custom draw
// (see the row's own dispatch comment above), so `highlighted` instead picks
// muted gray (quiet) vs full orange (selected) to give the hero row a
// visible selected state, matching the quiet-vs-cursor idiom every other
// HOME row already has.
static void draw_new_timer_hero(GContext *ctx, GRect cell_bounds, bool highlighted) {
  GPoint center = GPoint(RING_CENTER_X, RING_CENTER_Y);
  GColor ring_color = highlighted ? GColorOrange : GColorDarkGray;

  graphics_context_set_stroke_color(ctx, ring_color);
  graphics_context_set_stroke_width(ctx, RING_STROKE_WIDTH);
  graphics_draw_circle(ctx, center, RING_RADIUS);

  graphics_context_set_fill_color(ctx, ring_color);
  GRect h_bar = GRect(center.x - PLUS_ARM_LEN, center.y - PLUS_THICKNESS / 2, 2 * PLUS_ARM_LEN,
                       PLUS_THICKNESS);
  GRect v_bar = GRect(center.x - PLUS_THICKNESS / 2, center.y - PLUS_ARM_LEN, PLUS_THICKNESS,
                       2 * PLUS_ARM_LEN);
  graphics_fill_rect(ctx, h_bar, 0, GCornerNone);
  graphics_fill_rect(ctx, v_bar, 0, GCornerNone);

  GRect label_box = GRect(0, LABEL_Y, cell_bounds.size.w, LABEL_HEIGHT);
  graphics_context_set_text_color(ctx, RENDER_COLOR_LABEL);
  graphics_draw_text(ctx, "New timer", fonts_get_system_font(FONT_KEY_GOTHIC_14), label_box,
                      GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  // The build-version marker no longer lives in this row -- it moved off
  // the center axis, into the composed Library row below (see
  // draw_library_row) so it stops crowding this centered composition.
}

// Composed "Library" row (row 2 when count == 0) -- see this section's own
// constants doc comment above for the full geometry rationale. `cell_bounds`
// is this row's own cell-local bounds; both the group's vertical center and
// the version marker are DERIVED from `cell_bounds.size.h` rather than fixed
// absolute offsets, so this reads correctly at the row's own height.
static void draw_library_row(GContext *ctx, GRect cell_bounds, bool highlighted) {
  render_composed_group(ctx, cell_bounds, highlighted, render_stack_glyph, "Library");

  // Version marker -- on the SAME line as the group (this row no longer has
  // a second line's worth of room, see this section's own top comment),
  // right-aligned to the content rail's own right edge, vertically centered
  // alongside the group, muted.
  int16_t w = cell_bounds.size.w;
  int16_t center_y = (int16_t)(cell_bounds.size.h / 2);
  GFont ver_font = fonts_get_system_font(FONT_KEY_GOTHIC_14);
  const char *ver_text = "v" APP_VERSION_STR;
  GSize ver_size = graphics_text_layout_get_content_size(ver_text, ver_font, GRect(0, 0, 60, 20),
                                                          GTextOverflowModeFill, GTextAlignmentRight);
  GRect ver_box = GRect(0, (int16_t)(center_y - ver_size.h / 2), (int16_t)(w - RENDER_MARGIN_X),
                         ver_size.h);
  // Text tier darkened one step to RENDER_COLOR_MUTED_TEXT (black) -- see
  // that constant's own doc comment (render.h).
  graphics_context_set_text_color(ctx, RENDER_COLOR_MUTED_TEXT);
  graphics_draw_text(ctx, ver_text, ver_font, ver_box, GTextOverflowModeTrailingEllipsis,
                      GTextAlignmentRight, NULL);
}

// Row 1 ("Say a timer") in the EMPTY-HOME composition uses the same centered
// composed idiom draw_library_row uses (see this section's own top comment
// for the idiom-clash this replaces), preceded by the short "seat" divider
// that used to sit at this same spot before the voice row briefly retired it
// in favor of a (mismatched) full-width rail row.
static void draw_voice_row_empty(GContext *ctx, GRect cell_bounds, bool highlighted) {
  render_composed_seat_divider(ctx, cell_bounds);
  render_composed_group(ctx, cell_bounds, highlighted, render_mic_glyph, "Say a timer");
}

// --- MenuLayer callbacks (both states) --------------------------------------

static uint16_t get_num_rows(MenuLayer *menu_layer, uint16_t section_index, void *context) {
  (void)menu_layer;
  (void)section_index;
  (void)context;
  int n = app_context_store()->count;
  // The real-behavior row-count arithmetic -- including the count==0
  // "+ New timer" + "Library" floor (no Clear-all) and the populated count+3
  // shape ("+ New timer" + "Library" + "Clear all") -- lives in lib/view.c,
  // host-tested at n=0/1/2/15/16. See view_home_row_count's own doc comment
  // for the full row model.
  return (uint16_t)view_home_row_count(n);
}

// The busy/scroll (3+) rung keeps a shorter, compact row pitch instead of
// RENDER_ROW_H's 46px -- the count-based number-size
// rung already shrinks the font at this count (view_home_value_size), but
// the ROW itself used to stay full height regardless, leaving needless air
// around each small number. Single source of truth for both get_cell_height
// and the scroll indicator's visible-row estimate below, so they never
// disagree.
static int16_t row_height_for_count(int n) {
  return (view_home_value_size(n) == VIEW_HOME_SIZE_SMALL) ? RENDER_ROW_H_COMPACT : RENDER_ROW_H;
}

// Every screen fills the full 228px display height: the LAST row of the
// populated shape -- "Clear all" -- stretches to fill any spare viewport
// space when the list is short, so it visually bottom-anchors instead of
// leaving dead white space below it. "+ New timer" and "Library" take a
// fixed RENDER_MENU_ROW_H pitch (38px, the same pitch every other
// action-row context in this app uses), not the timer rows' own
// row_height_for_count pitch -- this keeps the footer rows visually
// consistent with the rest of the app instead of inheriting the timer
// rows' own pitch. Only "Clear all" (the group's last row) stretches.
//
// The EMPTY-state shape is handled by its own branch: row 0 (the hero) is a
// fixed HOME_EMPTY_HERO_H -- a designed two-part composition, not a
// stretch-to-fill layout -- and row 1 (composed Library) takes whatever
// remains of the viewport.
static int16_t get_cell_height(MenuLayer *menu_layer, MenuIndex *cell_index, void *context) {
  (void)context;
  int n = app_context_store()->count;
  int16_t viewport_h = (int16_t)layer_get_bounds(menu_layer_get_layer(menu_layer)).size.h;

  if (n == 0) {
    if (cell_index->row == 0) return HOME_EMPTY_HERO_H;
    // Row 1 = "Say a timer", inserted directly under the hero (both HOME
    // states) -- fixed RENDER_MENU_ROW_H, the same action-row pitch the
    // populated footer already uses. Row 2 (composed "Library") absorbs the
    // shrink.
    if (cell_index->row == 1) return RENDER_MENU_ROW_H;
    int16_t remaining = (int16_t)(viewport_h - HOME_EMPTY_HERO_H - RENDER_MENU_ROW_H);
    return (remaining > 0) ? remaining : RENDER_MENU_ROW_H;  // row 2 = composed "Library"
  }

  if (cell_index->row < n) return row_height_for_count(n);

  // n = "+ New timer", n+1 = "Say a timer", n+2 = "Library", n+3 =
  // "Clear all" -- the grouped footer block, fixed RENDER_MENU_ROW_H pitch
  // except the last row, which stretches to fill any spare viewport space
  // below the timer rows.
  int clearall_row = n + 3;
  if (cell_index->row == clearall_row) {
    int16_t before_clearall = (int16_t)(n * row_height_for_count(n) + 3 * RENDER_MENU_ROW_H);
    int16_t natural_total = (int16_t)(before_clearall + RENDER_MENU_ROW_H);
    if (natural_total < viewport_h) return (int16_t)(viewport_h - before_clearall);
  }
  return RENDER_MENU_ROW_H;
}

// Delegates to the shared render_row() component -- a pill inset with
// rounded corners and a hairline separator (rather than an invisible fill),
// a count-based number-size rung, and a draining underline bar for
// countdown/repeat rows (stopwatch omits it, since a fired stopwatch has no
// deadline to drain toward).
//
// `label` holds the timer's own name via view_row_label() (NULL name -> no
// label text; glyph + color + magnitude carry identity alone when there is
// no name) rather than a generic kind word ("Countdown"/"Stopwatch") -- a
// generic kind word has no short-name guarantee the way a real timer name
// does, and was prone to truncating mid-word at HOME's row width.
static void draw_row(GContext *ctx, const Layer *cell_layer, MenuIndex *cell_index, void *context) {
  (void)context;
  GRect bounds = layer_get_bounds(cell_layer);
  bool highlighted = menu_cell_layer_is_highlighted(cell_layer);

  TimerStore *store = app_context_store();
  int n = store->count;

  if (n == 0) {
    // Composed empty state: row 0 = the "New timer" hero (no pill -- a
    // custom draw, so there is nothing for the MenuLayer's cursor highlight
    // to show through, same "suppress the cursor pill" rule as before);
    // row 1 = "Say a timer" (the SAME centered composed idiom as row 2, see
    // draw_voice_row_empty); row 2 = the composed "Library" row (glyph+label
    // group, centered chip cursor -- see draw_library_row).
    if (cell_index->row == 0) {
      draw_new_timer_hero(ctx, bounds, highlighted);
      return;
    }
    if (cell_index->row == 1) {
      draw_voice_row_empty(ctx, bounds, highlighted);
      return;
    }
    draw_library_row(ctx, bounds, highlighted);
    return;
  }

  // >= n covers all four trailing rows: "+ New timer" (== n), "Say a timer"
  // (== n+1), "Library" (== n+2), and "Clear all" (== n+3, the locked
  // row+confirm hybrid -- always present, no gate). The "grouped footer
  // block": all four share the render_action_row rail (13px inset, matching
  // render_row's own content-row rail -- fixing an earlier "5px rail jump"
  // between the old is_footer path and render_menu_row's narrower rail) with
  // NO inter-row hairlines, set off from the live timer list by ONE leading
  // divider (drawn by the first footer row, "+ New timer").
  if (cell_index->row >= n) {
    if (cell_index->row == n) {
      render_group_divider(ctx, bounds);
      // Driven by the REAL store->count, never a debug lever.
      // The "16" below is a literal, not MAX_TIMERS itself (there's no
      // string-building at this call site) -- the _Static_assert right after
      // this function pins it, so a future change to MAX_TIMERS fails the
      // build loudly here instead of leaving this label silently stale.
      if (n >= MAX_TIMERS) {
        render_action_row_disabled(ctx, bounds, highlighted, "All 16 timers in use",
                                    "Delete one to add more", false);
      } else {
        render_action_row(ctx, bounds, highlighted, "+ New timer", false);
      }
      return;
    }
    if (cell_index->row == n + 1) {
      // "Say a timer" -- like "+ New timer" above, an ADD-class action that
      // would waste a full round-trip (system dictation UI + parse) only to
      // be refused at Start's own store_add() -- gated at MAX_TIMERS the
      // same way, for the same reason. This row's own label used to stay
      // unchanged ("Say a timer") whether quiet or highlighted, reasoning
      // that "+ New timer"'s message directly above it already covered the
      // whole ADD-class block -- but that reads as an unexplained silent
      // buzz to a user who reaches this row first (it's one Down-press from
      // the newest timer, the default landing spot). So the HIGHLIGHTED
      // state still mirrors "+ New timer"'s own disabled explanation
      // ("Delete one to add more") -- a user who lands directly on this row
      // still gets the reason. The QUIET state, however, keeps this row's
      // own plain label ("Say a timer", muted/disabled-styled) rather than
      // repeating "+ New timer"'s "All 16 timers in use" verbatim -- when
      // BOTH footer rows are quiet at once (cursor elsewhere, e.g. resting
      // on the last timer row), repeating the identical sentence on two
      // consecutive rows reads as a duplicated/broken row, not
      // reinforcement. "+ New timer" (row n, checked
      // first above) remains the one row that states the reason while
      // quiet.
      if (n >= MAX_TIMERS) {
        render_action_row_disabled(ctx, bounds, highlighted, "Say a timer", "Delete one to add more",
                                    false);
      } else {
        render_action_row_mic(ctx, bounds, highlighted, "Say a timer", false);
      }
      return;
    }
    if (cell_index->row == n + 2) {
      render_action_row(ctx, bounds, highlighted, "Library", false);
      return;
    }
    // "Clear all" row (n + 3) -- stays functional even when the store is at
    // MAX_TIMERS -- only the ADD-class rows above are gated.
    render_action_row(ctx, bounds, highlighted, "Clear all", false);
    return;
  }

  // Pause-aware ordering (lib/store's 3-group rule) -- recomputed per draw
  // rather than cached, matching the codebase's established "cheap enough at
  // MAX_TIMERS=16" pattern used elsewhere for other per-redraw
  // recomputations (e.g. view_home_value_size(), lib/view.c).
  int32_t order[MAX_TIMERS];
  store_home_order(store, time(NULL), order, MAX_TIMERS);
  const Timer *t = store_find_const(store, order[cell_index->row]);
  if (t == NULL) return;  // defensive -- store mutated out from under a stale row index

  time_t now = time(NULL);
  ViewHeroBranch branch = view_hero_branch(t, app_state_is_ringing(app_context_app_state(), t->cookie));
  ViewColorToken color_token = view_kind_color_token(t->kind, branch);
  GColor kind_color = render_kind_color(color_token);
  // HOME's row number is the same "kind-colored value text on paper" defect
  // the hero values and the clock strip's own contrast fix both share --
  // darkened via the same render_kind_color_deep() resolver
  // (countdown/stopwatch only; alarm/repeat already dark). The row's glyph
  // and draining bar keep the full-saturation identity color (kind_color)
  // above -- only the number itself darkens.
  GColor value_color = render_kind_color_deep(color_token);

  char value[16];
  format_home_row_value(value, sizeof value, t, now);

  // The starting font tier is picked here from the count-based rung
  // (render_home_value_font); if the formatted value is still too wide for
  // the row (e.g. a 10h+ countdown), render_row() shrinks it further via its
  // own fit_row_number_font() helper, applied unconditionally to every
  // render_row() caller rather than as a per-call-site guard -- this avoids
  // a starting tier below the largest one silently skipping the
  // shrink-to-fit check.
  GFont number_font = render_home_value_font(view_home_value_size(n));

  // Draining underline: countdown/repeat only, and only while still running
  // toward a deadline (a fired timer's remaining() is clamped 0 -- an empty
  // bar is not useful information at row scale).
  bool show_bar = (t->kind == KIND_COUNTDOWN || t->kind == KIND_REPEAT) && !t->fired;
  // Remaining fraction as per-mille -- exact, `1000 - pct*10` (integer math,
  // was `1.0f - pct/100.0f`).
  int32_t bar_fraction_pm = show_bar ? (RENDER_FRACTION_SCALE - timer_progress_pct(t, now) * 10) : 0;

  // A snoozed repeat's row/number/bar already show the real remaining-time
  // values (timer_remaining_s() during a snooze reflects "back in M:SS") --
  // only the label needs the "snoozing" distinguisher, via
  // timer_is_snoozing(). The timer's own name flows into that same label
  // slot when present. Precedence: "snoozing" wins over a name -- a snoozed
  // repeat's re-alert state is a transient, higher-priority signal than its
  // permanent, cosmetic display name, and the label slot only ever holds one
  // string. A named-but-not-snoozing row shows its name; a name-less row
  // shows nothing.
  RenderRowSpec spec = {
      .highlighted = highlighted,
      .kind = t->kind,
      .kind_color = kind_color,
      .value_color = value_color,
      .label = timer_is_snoozing(t) ? "snoozing" : view_row_label(t->name),
      .number = value,
      .number_font = number_font,
      .number_prefix = VIEW_PREFIX_NONE,
      .show_bar = show_bar,
      .bar_fraction_pm = bar_fraction_pm,
  };
  render_row(ctx, bounds, &spec);
}

// Pins the "16" hardcoded into draw_row()'s "All 16 timers in use" label
// (above) to the real MAX_TIMERS constant -- cheaper than building the
// string with snprintf (zero image bytes, no runtime formatting) and, unlike
// a comment alone, breaks the build loudly if MAX_TIMERS ever changes
// instead of leaving the label to go stale silently.
_Static_assert(MAX_TIMERS == 16, "update the \"All 16 timers in use\" label in draw_row() above");

// Never reached under GALLERY_MODE (screen_home_create() below wires
// gallery_click_config_provider onto the window itself in that build,
// bypassing menu_layer_set_click_config_onto_window's Select handling
// entirely) -- no presets-offset bookkeeping needed here, unlike draw_row.
static void select_click(MenuLayer *menu_layer, MenuIndex *cell_index, void *context) {
  (void)menu_layer;
  (void)context;
  TimerStore *store = app_context_store();
  int n = store->count;

  if (n == 0) {
    // Unified empty state: row 0 = "+ New timer" hero (never MAX-gated -- an
    // empty store can't be full), row 1 = "Say a timer" (also never
    // MAX-gated here, same reason), row 2 = "Library".
    if ((int)cell_index->row == 0) {
      nav_go_set();
      return;
    }
    if ((int)cell_index->row == 1) {
      nav_go_voice();
      return;
    }
    nav_go_library();
    return;
  }

  if ((int)cell_index->row < n) {
    int32_t order[MAX_TIMERS];
    store_home_order(store, time(NULL), order, MAX_TIMERS);
    nav_go_hero(order[cell_index->row]);
    return;
  }
  if ((int)cell_index->row == n) {
    // Gate the ADD action itself at MAX_TIMERS -- a reject-vibe and no
    // navigation, so the user never dials a whole timer only to be refused
    // at SET's own commit (screen_set.c's store:full guard stays as
    // defense-in-depth).
    if (n >= MAX_TIMERS) {
      vibes_adapter_reject();
      return;
    }
    nav_go_set();
    return;
  }
  if ((int)cell_index->row == n + 1) {
    // "Say a timer" -- same MAX_TIMERS gate as "+ New timer" above, for the
    // same reason (a full dictation round-trip only to be refused at
    // screen_voice.c's own Start-time store_add() would be an even worse
    // waste than the dial's case). screen_voice.c's own do_start() also
    // defends against a full store (vibes_adapter_reject + testlog), same
    // "gate the entry, keep the commit-time guard as defense-in-depth"
    // pattern as the dial.
    if (n >= MAX_TIMERS) {
      vibes_adapter_reject();
      return;
    }
    nav_go_voice();
    return;
  }
  if ((int)cell_index->row == n + 2) {
    // "Library" row, between "Say a timer" and "Clear all".
    nav_go_library();
    return;
  }
  // "Clear all" row (n + 3) -- stays functional even at MAX_TIMERS (only the
  // ADD-class rows above are gated).
  nav_go_clear_confirm();
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
  MenuIndex idx = menu_layer_get_selected_index(s_menu);
  TimerStore *store = app_context_store();
  if ((int)idx.row >= store->count) return;
  int32_t order[MAX_TIMERS];
  store_home_order(store, time(NULL), order, MAX_TIMERS);
  nav_go_actions(order[idx.row]);
}

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

static void back_click_handler(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
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

// draw_row can only paint inside a cell -- the thin 3px scroll indicator
// (shown only once the list actually overflows) needs its own sibling Layer,
// reading menu_layer_get_selected_index + the total row count, drawn only
// once the list actually overflows MAX_VISIBLE_ROWS.
// Delegates to the shared render_scroll_indicator (explicit GColorBlack
// thumb / GColorLightGray track -- the track used to be unpainted
// entirely).
static void scroll_indicator_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  int n = app_context_store()->count;
  // This used to hand-derive `n + 1` here, which undercounted (it ignored
  // the Clear-all row). Fixed by reusing the SAME lib/view.c row-count
  // helper get_num_rows already calls, so the two can never drift apart
  // again.
  int total = view_home_row_count(n);
  // Visible-row count is derived from the ACTUAL row pitch for this count
  // (row_height_for_count) instead of the flat MAX_VISIBLE_ROWS=4 -- the
  // busy/scroll rung's compact rows fit more than 4 in the same viewport,
  // and a stale flat count under-sizes the scroll thumb.
  int16_t row_h = row_height_for_count(n);
  int visible = (row_h > 0) ? (int)(bounds.size.h / row_h) : MAX_VISIBLE_ROWS;
  if (visible < 1) visible = 1;
  MenuIndex sel = menu_layer_get_selected_index(s_menu);
  render_scroll_indicator(ctx, bounds, total, visible, (int)sel.row);
}

// HOME's own header (clock + the header hairline separating it from the row
// list) -- taller than the generic render_clock_strip_update_proc other list
// screens use (RENDER_CLOCK_STRIP_HEIGHT=24), since the hairline sits at a
// fixed y=39 and the row list itself starts at y=46 -- HOME is the one
// screen that needs both in the same header band.
#define HOME_HEADER_H 46

static void home_header_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, COLOR_PAPER);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  render_clock(ctx, bounds, clock_is_24h_style());
  render_header_hairline(ctx, bounds);
}

// --- window lifecycle (everything created ONCE, here) ----------------------

// Factored out of window_appear (below) so the screen gallery tool (see
// gallery.c, GALLERY_MODE builds only) can re-run the same MenuLayer
// reload/cursor-reset outside of a real window-stack transition --
// window_appear only fires on an actual push, but the gallery rebuilds its
// fixture and re-targets HOME across consecutive scenes without HOME ever
// leaving the top of the stack. Without this, a later scene's rebuilt store
// could silently keep whatever cursor state an earlier scene left behind,
// since the MenuLayer's own cached row-count/selected-index is never
// otherwise revalidated against the new data.
static void home_refresh_appearance(void) {
  Layer *menu_l = menu_layer_get_layer(s_menu);

  // HOME is unified onto one MenuLayer in both states -- the clock strip,
  // menu, and scroll indicator are ALWAYS visible (the old empty/populated
  // `layer_set_hidden` toggle, together with `s_empty_layer` itself, is
  // deleted). get_num_rows/get_cell_height/draw_row above already branch on
  // count==0 to draw the "+ New timer" hero + "Library" instead of the timer
  // list + footer.

  // This reload+reset used to run ONLY inside `if (populated)`, so a
  // populated -> EMPTY transition (Clear-all, or deleting the last
  // remaining timer via Actions) left the
  // MenuLayer's own cached row-count/selected-index exactly where they were
  // an instant before the store emptied -- e.g. pointing at the "Clear all"
  // row. The MenuLayer's click routing dispatches Select using that CACHED
  // index directly, with no current-row-count re-validation of its own, so
  // the next Select press (meant to hit the empty state's "+ New timer"
  // affordance, MenuIndex(0,0) == the footer row when count==0) instead
  // silently re-ran whatever the stale row happened to be -- reproduced live
  // as a same-empty-store "clear:all" re-firing on every subsequent Select,
  // never reaching SET. Reload+reset now runs unconditionally: harmless when
  // count==0 (MenuIndex(0,0) IS the footer row there too, matching the
  // already-documented empty-state contract), and it keeps the MenuLayer's
  // cached state honest with the real store on every transition, not just
  // the populated ones.
  menu_layer_reload_data(s_menu);

  // "Back from a hero -> HOME must land the cursor on that timer's own row"
  // -- nav_take_home_focus_cookie() is a one-shot read-and-clear (nav.c), so
  // this only ever fires for the specific appearance right after
  // screen_hero.c's Back handler set it; every other HOME appearance
  // (Clear-all, Library Back, SET cancel, ...) sees -1 here and keeps the
  // pre-existing "always land on the top (soonest-deadline) row" default. A
  // cookie that no longer exists (the timer was deleted while its hero was
  // open, e.g. via Actions -> Delete before Back) maps to
  // store_home_row_for_cookie's own -1 -- falls back to row 0 exactly like
  // the default case, never indexes a dead row (the same class of staleness
  // the fix above guards against).
  // This SAME one-shot mechanism is extended with a second sentinel value,
  // NAV_HOME_FOCUS_VOICE_ROW (nav.h) -- "cancelling out of the voice arc
  // must land on the 'Say a timer' row," the same "return to where you
  // launched from" idea as the hero fix, for a row that isn't a store-linked
  // cookie at all.
  int32_t focus_cookie = nav_take_home_focus_cookie();
  int target_row = 0;
  MenuRowAlign align = MenuRowAlignTop;
  TimerStore *store = app_context_store();
  if (focus_cookie == NAV_HOME_FOCUS_VOICE_ROW) {
    // Cancelling out of the voice-input flow lands the cursor back on the
    // "Say a timer" row (the row it was launched from), not row 0 -- reuses
    // the same one-shot focus-cookie mechanism above via a sentinel value
    // distinct from any real cookie. The row index is derived from
    // lib/view.c's view_home_voice_row() (host-tested), the same source of
    // truth draw_row/select_click's own row dispatch already uses, so the
    // two can never drift apart.
    // Aligned to Center rather than Top for defensive parity with
    // screen_library.c's own equivalent fix: MenuRowAlignTop can silently
    // fail to scroll the cursor into view when the selected row is the
    // list's own trailing row on an overflowing MenuLayer. HOME's "Say a
    // timer" row isn't actually in that situation -- "Library"/"Clear all"
    // always render after it, so MenuLayer's own scroll clamp keeps it in
    // view even under Top -- but Center is kept anyway: no worse than Top,
    // and cheap insurance if HOME's footer row order ever changes.
    target_row = view_home_voice_row(store->count);
    align = MenuRowAlignCenter;
  } else if (store->count > 0 && focus_cookie >= 0) {
    int row = store_home_row_for_cookie(store, focus_cookie, time(NULL));
    if (row >= 0) {
      target_row = row;
      // Center (not top-align) a restored deep-list row -- top-aligning a
      // mid/bottom-scroll row would scroll PAST it to make it the new top,
      // which is not "land on this row," it's "land at this scroll offset."
      // No explicit row==0 special case: center-aligning row 0 clamps to the
      // same offset Top would give (PebbleOS's ScrollLayer can't scroll past
      // content-top -- verified against menu_layer.c's own clamping, not
      // special-cased here), so the common single-timer/first-row case is
      // unaffected without this code doing anything extra for it.
      align = MenuRowAlignCenter;
    }
  }
  menu_layer_set_selected_index(s_menu, MenuIndex(0, (uint16_t)target_row), align, false);
  layer_mark_dirty(menu_l);
  layer_mark_dirty(s_clock_strip);
  layer_mark_dirty(s_scroll_indicator);
}

static void window_appear(Window *window) {
  (void)window;
  home_refresh_appearance();
}

Window *screen_home_create(void) {
  // HOME is the one screen that normally has NO custom click_config_provider
  // at all under GALLERY_MODE -- render_build_menu_screen's own internal
  // GALLERY_MODE swap covers that uniformly now (see its doc comment).
  // Hand-rolled like Library so Select-long can open ACTIONS; the explicit
  // Back subscription preserves HOME's exit-to-watchface behavior.
  RenderMenuScreen screen = render_build_menu_screen(&(RenderMenuScreenSpec){
      .window_bg = COLOR_PAPER,
      // HOME is the one screen whose click_config_provider is wired via this
      // shared builder BEFORE its MenuLayer/header layers are constructed
      // (render_build_menu_screen calls window_set_click_config_provider()
      // right after window_create(), then builds screen.menu/screen.header
      // afterwards -- see render.c's render_build_menu_screen()), rather
      // than after, which is how HOME wired it before adopting the shared
      // builder. This is safe and does not need "fixing": the SDK only
      // STORES the provider pointer at window_set_click_config_provider()
      // time and INVOKES it later, when the window is shown (on the
      // ClickManager reconfigure that happens on window appear) -- by then
      // every layer this window needs already exists, regardless of which
      // order the two calls happened in at create() time.
      .click_config_provider = click_config_provider,
      .window_handlers = (WindowHandlers){.appear = window_appear},
      .header_height = HOME_HEADER_H,
      .header_update_proc = home_header_update_proc,
      // MenuLayer's default pad_bottom adds bottom padding to the scroll
      // content, letting the offset over-scroll past the real content
      // bottom. At HOME's structural max-scroll that clips the top row's
      // numeral to a mangled bottom sliver. HOME's last row ("Clear all")
      // already bottom-anchors via get_cell_height's stretch, so no bottom
      // padding is wanted here.
      .pad_bottom_disable = true,
      .get_num_rows = get_num_rows,
      .get_cell_height = get_cell_height,
      .draw_row = draw_row,
      .with_scroll_indicator = true,
      .scroll_indicator_update_proc = scroll_indicator_update_proc,
  });
  s_window = screen.window;
  s_clock_strip = screen.header;
  s_menu = screen.menu;
  s_scroll_indicator = screen.scroll_indicator;
  return s_window;
}

void screen_home_destroy(void) {
  render_destroy_menu_screen(&(RenderMenuScreen){.window = s_window,
                                                   .header = s_clock_strip,
                                                   .menu = s_menu,
                                                   .scroll_indicator = s_scroll_indicator});
  s_scroll_indicator = NULL;
  s_menu = NULL;
  s_clock_strip = NULL;
  s_window = NULL;
}

// Ticks the row list live (remaining/elapsed change every second, both
// states -- the empty state's own live wall clock lives in the shared header
// strip now too) and the scroll indicator (cursor-driven, cheap to redraw
// unconditionally) -- not just the clock text; the exported name is kept
// since main.c already calls it every second tick. Marking the whole menu
// every second is intentional and HOME-specific (unlike the "no per-second
// menu-dirty on static rows" rule for Library/preset-actions,
// screen_library.c/screen_preset_actions.c's own strip-only ticks) --
// HOME's timer rows are the one list in this app whose CONTENT genuinely
// changes every second, so keeping menu_layer_pad_bottom_enable false
// (above) is what keeps this safe, not avoiding the redraw itself.
void screen_home_mark_clock_dirty(void) {
  if (s_clock_strip != NULL) layer_mark_dirty(s_clock_strip);
  if (s_menu != NULL) layer_mark_dirty(menu_layer_get_layer(s_menu));
}

#ifdef GALLERY_MODE
MenuLayer *screen_home_debug_menu(void) {
  return s_menu;
}

void screen_home_debug_refresh(void) {
  home_refresh_appearance();
}
#endif
