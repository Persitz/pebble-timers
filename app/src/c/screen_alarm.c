// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

#include "screen_alarm.h"

#include <stdio.h>

#include "app_context.h"
#include "fonts_adapter.h"
#include "hold.h"
#include "input_guard.h"
#include "lib/app_state.h"
#include "lib/format.h"
#include "lib/store.h"
#include "lib/timer.h"
#include "lib/view.h"
#include "nav.h"
#include "persist_adapter.h"
#include "render.h"
#include "testlog_am.h"
#include "wakeup_adapter.h"
#ifdef GALLERY_MODE
#include "gallery.h"
#endif

// Alias onto the shared constant (render.h, already included) -- see
// screen_home.c's matching fix.
#define COLOR_PAPER RENDER_COLOR_PAPER
// The 52px alarm header band (render_alarm_header_band, render.h) replaces
// the old plain-clock top -- the defining visual element for a ringing
// alarm. Sub-header ("ringing - N others") sits between the band and the
// big value; the segmented bar (repeat only) sits between the value and
// the hold-footer.
#define SUBHEADER_Y 88
#define VALUE_RECT GRect(0, 104, 200, 52)

// A windsor-tan GOTHIC_14 reference line in the band->value gap (52-88px)
// -- names the original countdown target (one-off) or the just-expired
// interval (repeat). See hero_root_layer_update_proc below for the two
// copy variants.
#define ALARM_REF_Y 56

// Same caption idiom as the running hero (screen_hero.c's
// draw_hero_name_caption) -- centered, neutral ink, GOTHIC_14, ellipsis on
// overflow. Lands in the otherwise-dead 16px slot between the reference
// line (ALARM_REF_Y=56, ends 72) and the "ringing - N others" subheader
// (SUBHEADER_Y=88) -- a fixed slot regardless of whether the subheader
// itself draws (others==0 leaves it free anyway, so there's no conditional
// layout to reconcile).
#define ALARM_NAME_CAPTION_Y 72

#define SCROLL_INDICATOR_WIDTH 3
#define SCROLL_INDICATOR_MARGIN 3

static Window *s_hero_window;
static Layer *s_hero_root;
static Window *s_options_window;
static Layer *s_options_strip;
static MenuLayer *s_options_menu;
static Layer *s_options_scroll_indicator;
static int32_t s_cookie;

static Timer *current_timer_mut(void) {
  return store_find(app_context_store(), s_cookie);
}

static const Timer *current_timer(void) {
  return store_find_const(app_context_store(), s_cookie);
}

static void persist_and_replan(void) {
  TimerStore *store = app_context_store();
  persist_adapter_save(store);
  wakeup_adapter_replan(store, time(NULL));
}

// "Keep going" re-arms a repeat timer (timer_advance_cycle) from three call
// sites: the hero's hold-to-fire gesture, its Back button, and the options
// list's Keep-going entry. Centralized here as a plain function call rather
// than shared window click-config wiring, which has been an error-prone
// pattern elsewhere in this app. The re-armed cycle count is logged
// directly here so every call site produces the same unambiguous signal,
// instead of relying on parsing the rendered "done x_" meta text, which is
// prone to misreads.
static void do_advance_cycle(Timer *t, time_t now) {
  timer_advance_cycle(t, now);
  char data[16];
  snprintf(data, sizeof data, "%d", (int)t->cycles);
  testlog("repeat_rearm", data);
}

static void resolve_after_mutation(void) {
  AppState *app_state = app_context_app_state();
  app_state_ringing_remove(app_state, s_cookie);
  persist_and_replan();
  nav_alarm_resolved(app_state->ringing_count == 0);
}

// "Keep counting" (one-off Back / the Keep counting options row) converts
// the timer to a stopwatch that must be shown immediately -- see
// nav_policy.h's doc on nav_keep_counting_target for why this can't reuse
// the generic resolve_after_mutation()/nav_alarm_resolved() resume-restore
// path.
static void resolve_after_keep_counting(void) {
  AppState *app_state = app_context_app_state();
  int32_t cookie = s_cookie;
  app_state_ringing_remove(app_state, cookie);
  persist_and_replan();
  testlog("keep_counting", "ok");
  nav_alarm_resolved_keep_counting(cookie, app_state->ringing_count == 0);
}

// The alarm hero's Up-hold right-slot hint -- same measure-then-fallback
// discipline screen_hero.c's up_legend_label/repeat_edit_legend_label
// already established (LEGEND_UP_MAX_TEXT_W-sized threshold, word-level
// fallback rather than mid-word ellipsis). Falls back to just the quarter
// value alone ("0:15") when "snooze 0:15" doesn't comfortably fit the
// pill's stacked-legend column. `buf`/`buf_len` back the returned pointer
// (unlike screen_hero.c's siblings, which return static literals, this
// one's "full" string is caller-built per frame).
#define ALARM_LEGEND_MAX_TEXT_W 42
static const char *alarm_snooze_legend_label(char *buf, size_t buf_len, const char *qstr,
                                              GFont font) {
  snprintf(buf, buf_len, "snooze %s", qstr);
  GSize size = graphics_text_layout_get_content_size(buf, font, GRect(0, 0, 100, 16),
                                                       GTextOverflowModeFill, GTextAlignmentLeft);
  if (size.w <= ALARM_LEGEND_MAX_TEXT_W) return buf;
  snprintf(buf, buf_len, "%s", qstr);
  return buf;
}

// --- hero window -------------------------------------------------------------

static void hero_root_layer_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, COLOR_PAPER);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  const Timer *t = current_timer();
  if (t == NULL) {
    render_clock(ctx, bounds, clock_is_24h_style());
    return;  // defensive
  }

  time_t now = time(NULL);
  ViewColorToken token = view_kind_color_token(t->kind, VIEW_HERO_ALARM);
  GColor color = render_kind_color(token);
  // The big overrun VALUE below is darkened via render_kind_color_deep() --
  // same treatment as screen_hero.c's hero values. `color` above stays the
  // plain identity color for the header band background and the
  // reference-line accent (RENDER_COLOR_ACCENT, unaffected either way),
  // neither of which render_kind_color_deep touches. In practice this only
  // affects one-off alarms (red, already 7.8:1 -- render_kind_color_deep is
  // a no-op for VIEW_COLOR_ALARM); a repeat alarm's indigo is likewise
  // unchanged (10.7:1).
  GColor value_color = render_kind_color_deep(token);
  bool is_24h = clock_is_24h_style();

  // The 52px alarm header band -- the defining visual element for a
  // ringing alarm. The header states the trigger nature -- one-off's is a
  // static label, repeat names the re-fire interval ("buzzes again in N").
  // Repeat alarms stay INDIGO, not red -- kind identity beats alert-red
  // (view_kind_color_token's own ruling).
  char status[64];
  if (t->kind == KIND_REPEAT) {
    char interval[16];
    format_hms(interval, sizeof interval, t->duration_s);
    // Repeat alarms show a trigger-nature prefix (\"CHECK - \") before the
    // re-fire interval; the originally intended phrasing was
    // \"CHECK - buzzes again in N:00\", but that full phrasing (29 chars)
    // does not fit the header band's available width (~137px at
    // GOTHIC_14_BOLD, even after this file's
    // dynamic clock-width reclaim) and ellipsizes away the interval itself,
    // leaving only the nature word -- worse than showing the interval without
    // it. Shortened to \"CHECK - again in N:00\", which keeps both the
    // trigger-nature word and the interval visible within the available
    // width.
    // Reframes the earlier forward-looking "CHECK - again in N" band to
    // the backward-looking "the interval just expired" framing -- paired
    // with the reference line below ("%s interval - over") and the
    // already-shown big overrun value, this reads as one sentence: interval
    // expired (band) -> named (reference line) -> now X into overtime (the
    // big value). Deduped: this status line does NOT also say "repeat" --
    // the kind title (render_alarm_header_band's own "Repeat", drawn right
    // below) already names the kind, so repeating the word here would be a
    // duplicate.
    (void)interval;  // still used by the reference-line snprintf below
    snprintf(status, sizeof status, "INTERVAL UP");
  } else {
    snprintf(status, sizeof status, "TIME'S UP - one-off");
  }
  // One-off gets the larger system font (GOTHIC_28_BOLD, ~30px), repeat
  // keeps the smaller one (GOTHIC_24_BOLD, ~26px) -- previously one shared
  // GOTHIC_24_BOLD for both, undocumented.
  GFont title_font =
      fonts_get_system_font(t->kind == KIND_REPEAT ? FONT_KEY_GOTHIC_24_BOLD : FONT_KEY_GOTHIC_28_BOLD);
  render_alarm_header_band(ctx, bounds, color, status, render_kind_label(t->kind), title_font,
                            is_24h);

  // See ALARM_REF_Y's own doc comment: one-off names the original target,
  // repeat names the just-expired interval.
  char ref[32];
  if (t->kind == KIND_REPEAT) {
    char interval_ref[16];
    format_hms(interval_ref, sizeof interval_ref, t->duration_s);
    snprintf(ref, sizeof ref, "%s interval - over", interval_ref);
  } else {
    char target[16];
    format_hms(target, sizeof target, t->duration_s);
    snprintf(ref, sizeof ref, "%s target", target);
  }
  GRect ref_rect = GRect(0, ALARM_REF_Y, bounds.size.w, 16);
  graphics_context_set_text_color(ctx, RENDER_COLOR_ACCENT);
  graphics_draw_text(ctx, ref, fonts_get_system_font(FONT_KEY_GOTHIC_14), ref_rect,
                      GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

  // The ringing hero's own name caption -- unnamed alarms hit the early
  // return, zero layout shift.
  if (t->name[0] != '\0') {
    GRect name_rect = GRect(0, ALARM_NAME_CAPTION_Y, bounds.size.w, 16);
    graphics_context_set_text_color(ctx, RENDER_COLOR_LABEL);
    graphics_draw_text(ctx, t->name, fonts_get_system_font(FONT_KEY_GOTHIC_14), name_rect,
                        GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  }

  // Reassurance that other timers keep running underneath while this one
  // alarms -- only drawn when there ARE others (no point stating "0
  // others"). Reduced to a count (see lib/view.h's doc on
  // view_others_active_count) rather than listing each one by name --
  // several named alarms wouldn't fit this one-line subheader.
  int others = view_others_active_count(t->cookie, app_context_store());
  if (others > 0) {
    char subheader[48];  // sized past GCC's -Wformat-truncation worst case for a bare %d
    if (others == 1) {
      snprintf(subheader, sizeof subheader, "ringing - 1 other still on");
    } else {
      snprintf(subheader, sizeof subheader, "ringing - %d others still on", others);
    }
    GRect subheader_rect = GRect(0, SUBHEADER_Y, bounds.size.w, 16);
    graphics_context_set_text_color(ctx, RENDER_COLOR_LABEL);
    graphics_draw_text(ctx, subheader, fonts_get_system_font(FONT_KEY_GOTHIC_14), subheader_rect,
                        GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  }

  char value[16];
  format_hms(value, sizeof value, timer_overrun_s(t, now));
  // Keeps the 42px raster here rather than the 56px hero tier -- 42px is
  // passable, while 56px would likely be too big for the ▲0:07 value
  // alongside this hero's header band.
  render_hero_value(ctx, VALUE_RECT, value, VIEW_PREFIX_CARET, value_color, fonts_adapter_numeral_42());

  // General rule: no progress bar in overtime. This hero only ever renders
  // while ringing -- i.e. already past the deadline, always in overtime --
  // so the segmented recurrence bar that used to sit here is dropped
  // entirely (matches VIEW_HERO_OVERTIME's own screen_hero.c branch, which
  // has never drawn one either). The interval itself is still visible, in
  // the reference line above.

  // Done/Keep-going stay hold actions (not a tap): a still-ringing alarm's
  // dismiss action must not fire from a stray press.
  // "Hold to keep going" (the longest hold label in the app) truncates
  // inside the pill column, so it reuses the shorter "Keep going" copy this
  // screen's own options list already uses for the identical action.
  // The one-off variant relies on a vector checkmark (never a font glyph)
  // rather than literal "Hold: " text, keeping this app's checkmark
  // convention for "Done".
  // A short "Hold: " prefix is restored for the repeat case: relying on the
  // pill shape and fill bar alone to signal "this is a hold" meant a plain
  // quick press produced no feedback at all (no vibration, no navigation) on
  // exactly the screen where reliable dismissal matters most. "Hold: " fits
  // on one line at the narrowest legend-column width, unlike "Hold to keep
  // going", which does not.
  const char *label = (t->kind == KIND_REPEAT) ? "Hold: Keep going" : "Hold: Done";
  // The alarm hero's own Up-hold -- snooze 1/4 of duration_s, gated the
  // same >10s way as the countdown/repeat Up-holds
  // (timer_add_quarter_available reused verbatim -- it's already a pure
  // function of duration_s alone). Below the gate the gesture, the
  // right-slot hint, AND the left hold-text-explains swap are all simply
  // absent -- up_label stays NULL, the same "hidden together" shape
  // established for the countdown add-1/4 gate.
  bool quarter_available = timer_add_quarter_available(t);
  char qstr[16] = {0};
  if (quarter_available) {
    format_hms(qstr, sizeof qstr, timer_quarter_amount(t));
  }
  char up_label_buf[24];
  const char *up_label = NULL;
  if (quarter_available) {
    up_label = alarm_snooze_legend_label(up_label_buf, sizeof up_label_buf, qstr,
                                          fonts_get_system_font(FONT_KEY_GOTHIC_14));
  }
  bool up_held = quarter_available && hold_active_button() == BUTTON_ID_UP;
  char label_buf[32];
  if (up_held) {
    snprintf(label_buf, sizeof label_buf, "Snooze %s", qstr);
    label = label_buf;
  }
  // The vector checkmark is the "✓ Done" convention specifically -- must not
  // ride along onto the swapped "Snooze M:SS" hold-text: a checkmark next to
  // "Snooze 0:15" would read as a stray affirmative on the wrong action.
  bool show_check = (t->kind != KIND_REPEAT) && !up_held;
  render_hold_footer(ctx, bounds, label, color, hold_is_active(), hold_progress(), show_check,
                      up_label);
}

static void hero_hold_fire(void) {
  Timer *t = current_timer_mut();
  if (t == NULL) return;
  time_t now = time(NULL);
  if (t->kind == KIND_REPEAT) {
    do_advance_cycle(t, now);  // Select = "Keep going"
  } else {
    store_remove(app_context_store(), t->cookie);  // Done
  }
  resolve_after_mutation();
}

// Back now DISMISSES a ringing alarm -- the same action the options list
// already exposes as one-off "Done" and repeat "Stop", both of which are
// byte-identical (store_remove + resolve_after_mutation) -- so the kind
// branch collapses to one unconditional path. Does NOT call
// timer_keep_counting/do_advance_cycle any more (those stay live, still
// referenced by the options rows and the hold-fire path above).
// Gated by the input guard: a Back that was already committed to the
// PREVIOUS screen and lands within INPUT_GUARD_MS of an app-initiated
// screen change (nav.c's set_alarm_target -- the take-over armer;
// resolve_to -- the resume armer -- also arms it, see input_guard.h's doc
// comment for both) is swallowed rather than destroying the freshly-shown
// alarm.
static void hero_back_click_handler(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
  if (input_guard_blocking()) {
    testlog("alarm_back", "guard");
    return;
  }
  Timer *t = current_timer_mut();
  if (t == NULL) return;
  store_remove(app_context_store(), t->cookie);  // dismiss == options "Done"/"Stop"
  testlog("alarm_back", "dismiss");
  resolve_after_mutation();
}

static void hero_down_click_handler(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
  nav_go_alarm_options(s_cookie);
}

// Up has no documented function on the alarm hero either (keymap: "UP --")
// -- a safe explicit no-op subscription. See screen_hero.c's
// click_config_provider for the full two-bug writeup (missing subscription
// here -- FIXED -- plus a separate, deeper, NOT fully resolved
// probabilistic crash under sustained interaction).
// This stays the Up handler ONLY when the snooze-1/4 gate (below) is
// closed -- see hero_click_config_provider.
static void hero_up_click_handler(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
}

static void log_snooze_quarter(int32_t quarter_s) {
  char data[16];
  snprintf(data, sizeof data, "%ld", (long)quarter_s);
  testlog("snooze_quarter", data);
}

// The alarm hero's Up-hold -- snooze 1/4 of duration_s. Repeat stays
// KIND_REPEAT (timer_snooze_repeat, non-destructive); one-off converts to
// KIND_COUNTDOWN (timer_snooze_seconds), same as the existing options-menu
// snooze rows. Snooze controls resolve the countdown, not pause it -- this
// calls resolve_after_mutation() exactly like hero_hold_fire/
// do_advance_cycle above, leaving the ringing surface the same way
// Done/Keep going do. Re-checks timer_add_quarter_available itself
// (matching do_advance_cycle/resolve_after_keep_counting's own "don't
// trust the binding alone" defensiveness, and screen_hero.c's
// cd_add_quarter/sw_add_lap precedent).
static void alarm_snooze_quarter(void) {
  Timer *t = current_timer_mut();
  if (t == NULL || !timer_add_quarter_available(t)) return;
  time_t now = time(NULL);
  int32_t quarter = timer_quarter_amount(t);
  if (t->kind == KIND_REPEAT) {
    timer_snooze_repeat(t, now);
  } else {
    timer_snooze_seconds(t, (uint32_t)quarter, now);
  }
  log_snooze_quarter(quarter);
  resolve_after_mutation();
}

static void hero_click_config_provider(void *context) {
  (void)context;
  window_single_click_subscribe(BUTTON_ID_BACK, hero_back_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, hero_down_click_handler);
  hold_bind_select(s_hero_window, hero_hold_fire);

  // Up is either the snooze-1/4 hold (gated, >10s) or the plain no-op
  // established above -- never both bound at once, same "single-click XOR
  // raw-click on this button" discipline screen_hero.c's RUNNING branch
  // already established for its own gated Up-holds.
  const Timer *t = current_timer();
  if (t != NULL && timer_add_quarter_available(t)) {
    hold_bind_button(s_hero_window, BUTTON_ID_UP, NULL, alarm_snooze_quarter, HOLD_MS);
  } else {
    window_single_click_subscribe(BUTTON_ID_UP, hero_up_click_handler);
  }
}

static void hero_window_load(Window *window) {
  s_hero_root = window_get_root_layer(window);
  layer_set_update_proc(s_hero_root, hero_root_layer_update_proc);
}

static void hero_window_unload(Window *window) {
  (void)window;
  s_hero_root = NULL;
}

Window *screen_alarm_create(void) {
  s_hero_window = window_create();
  window_set_background_color(s_hero_window, COLOR_PAPER);
#ifdef GALLERY_MODE
  // Screen gallery (inner loop, src/c/gallery.c) -- see screen_set.c's
  // matching comment. Never compiled into a release build.
  window_set_click_config_provider(s_hero_window, gallery_click_config_provider);
#else
  window_set_click_config_provider(s_hero_window, hero_click_config_provider);
#endif
  window_set_window_handlers(s_hero_window, (WindowHandlers){
                                                 .load = hero_window_load,
                                                 .unload = hero_window_unload,
                                             });
  return s_hero_window;
}

void screen_alarm_destroy(void) {
  window_destroy(s_hero_window);
  s_hero_window = NULL;
}

void screen_alarm_mark_dirty(void) {
  if (s_hero_root != NULL) layer_mark_dirty(s_hero_root);
}

// --- options window (collapsed, Down from the alarm hero) -------------------

typedef enum {
  OPTION_DONE,
  OPTION_KEEP_COUNTING,
  OPTION_KEEP_GOING,
  OPTION_RESTART,
  OPTION_SNOOZE_1,
  OPTION_SNOOZE_5,
  OPTION_SNOOZE_QUARTER,
  OPTION_DELETE,
} AlarmOption;

static bool options_is_repeat(void) {
  const Timer *t = current_timer();
  return t != NULL && t->kind == KIND_REPEAT;
}

// Same >10s gate as the hero's own Up-hold (timer_add_quarter_available
// reused verbatim) -- the menu row and the gesture must never disagree
// about when snooze-1/4 is available.
static bool options_quarter_available(void) {
  const Timer *t = current_timer();
  return t != NULL && timer_add_quarter_available(t);
}

static uint16_t options_get_num_rows(MenuLayer *menu_layer, uint16_t section_index, void *context) {
  (void)menu_layer;
  (void)section_index;
  (void)context;
  return (uint16_t)((options_is_repeat() ? 4 : 6) + (options_quarter_available() ? 1 : 0));
}

// Menus are catalogs: retain each kind's existing non-snooze order, sort the
// contiguous snooze group by its actual duration, and reserve Delete for last.
// A quarter exactly equal to a fixed snooze follows that fixed row (60 then
// quarter, or 300 then quarter), giving the duplicate durations a stable order.
static AlarmOption option_at_row(uint16_t row) {
  const Timer *t = current_timer();
  bool repeat = options_is_repeat();
  bool quarter = options_quarter_available();
  int32_t quarter_s = (quarter && t != NULL) ? timer_quarter_amount(t) : 0;
  if (repeat) {
    if (row == 0) return OPTION_KEEP_GOING;
    if (row == 1) {
      if (quarter && quarter_s < 5 * 60) return OPTION_SNOOZE_QUARTER;
      return OPTION_SNOOZE_5;
    }
    if (row == 2 && quarter) return quarter_s < 5 * 60 ? OPTION_SNOOZE_5 : OPTION_SNOOZE_QUARTER;
    if (row == (uint16_t)(1 + (quarter ? 2 : 1))) return OPTION_RESTART;
    return OPTION_DELETE;
  }
  if (row == 0) return OPTION_DONE;
  if (row <= (uint16_t)(quarter ? 3 : 2)) {
    if (!quarter) return row == 1 ? OPTION_SNOOZE_1 : OPTION_SNOOZE_5;
    uint16_t quarter_row = quarter_s < 60 ? 1 : quarter_s < 5 * 60 ? 2 : 3;
    if (row == quarter_row) return OPTION_SNOOZE_QUARTER;
    return row == 1 || (row == 2 && quarter_row == 1) ? OPTION_SNOOZE_1 : OPTION_SNOOZE_5;
  }
  if (row == (uint16_t)(1 + (quarter ? 3 : 2))) return OPTION_KEEP_COUNTING;
  if (row == (uint16_t)(2 + (quarter ? 3 : 2))) return OPTION_RESTART;
  return OPTION_DELETE;
}

// "Snooze 1/4 (M:SS)" -- plain ASCII "1/4" (this codebase's established
// no-non-ASCII-glyph convention, e.g. "x" for "×" elsewhere) rather than
// a unicode fraction glyph.
static void snooze_quarter_row_label(char *buf, size_t buf_len, const Timer *t) {
  char qstr[16];
  format_hms(qstr, sizeof qstr, timer_quarter_amount(t));
  snprintf(buf, buf_len, "Snooze 1/4 (%s)", qstr);
}

// Explicit fixed cell height -- see RENDER_MENU_ROW_H's doc comment
// (render.h). At the old, SDK-defaulted row pitch, the one-off options
// list's mandated 4th row ("Keep counting") sat below the fold on this
// screen.
static int16_t options_get_cell_height(MenuLayer *menu_layer, MenuIndex *cell_index, void *context) {
  (void)menu_layer;
  (void)cell_index;
  (void)context;
  return RENDER_MENU_ROW_H;
}

static void options_draw_row(GContext *ctx, const Layer *cell_layer, MenuIndex *cell_index,
                              void *context) {
  (void)context;
  const Timer *t = current_timer();
  char label_buf[32];
  const char *title = "";
  switch (option_at_row(cell_index->row)) {
    case OPTION_DONE: title = "Done"; break;
    case OPTION_KEEP_COUNTING: title = "Keep counting"; break;
    case OPTION_KEEP_GOING: title = "Keep going"; break;
    case OPTION_RESTART: title = "Restart"; break;
    case OPTION_SNOOZE_1: title = "Snooze 1:00"; break;
    case OPTION_SNOOZE_5: title = "Snooze 5:00"; break;
    case OPTION_SNOOZE_QUARTER:
      if (t != NULL) {
        snooze_quarter_row_label(label_buf, sizeof label_buf, t);
        title = label_buf;
      }
      break;
    case OPTION_DELETE: title = "Delete"; break;
  }
  // Shared pill/hairline menu-row primitive, replacing
  // `menu_cell_basic_draw`'s full-bleed highlight.
  render_menu_row(ctx, layer_get_bounds(cell_layer), menu_cell_layer_is_highlighted(cell_layer),
                   title);
}

static void options_select_click(MenuLayer *menu_layer, MenuIndex *cell_index, void *context) {
  (void)menu_layer;
  (void)context;
  Timer *t = current_timer_mut();
  if (t == NULL) return;
  time_t now = time(NULL);

  if (options_is_repeat()) {
    switch (option_at_row(cell_index->row)) {
      case OPTION_KEEP_GOING:
        do_advance_cycle(t, now);
        break;
      case OPTION_SNOOZE_5:
        timer_snooze(t, 5, now);
        break;
      case OPTION_DELETE:
        store_remove(app_context_store(), t->cookie);
        break;
      case OPTION_RESTART:
        // Same mutation as the RUNNING repeat hero's own Restart
        // (timer_restart -- cycles=0, snooze-marker clear, stays
        // KIND_REPEAT), resolved the same way Snooze5/Keep-going already are
        // (resolve_after_mutation() below) -- Restart's data shape (mutate
        // into a running, non-fired instance of the SAME kind) matches
        // Snooze's, not Keep-counting's kind-flip, so no special nav target
        // is warranted here.
        store_restart(app_context_store(), t, now);
        testlog("restart", "repeat_alarm");
        break;
      case OPTION_SNOOZE_QUARTER: {
        if (!timer_add_quarter_available(t)) break;
        int32_t quarter = timer_quarter_amount(t);
        timer_snooze_repeat(t, now);
        log_snooze_quarter(quarter);
        break;
      }
      default:
        break;
    }
    resolve_after_mutation();
    return;
  }

  switch (option_at_row(cell_index->row)) {
    case OPTION_DONE:
      store_remove(app_context_store(), t->cookie);
      break;
    case OPTION_SNOOZE_1:
      timer_snooze(t, 1, now);
      break;
    case OPTION_SNOOZE_5:
      timer_snooze(t, 5, now);
      break;
    case OPTION_KEEP_COUNTING:
      // Same "show the just-converted stopwatch, not the generic resume"
      // treatment as the hero's own Back handler above.
      timer_keep_counting(t, now);
      resolve_after_keep_counting();
      return;
    case OPTION_RESTART:
      // Restart restores the dialed set_total_s from now and clears fired
      // state. It resolves like Snooze, not like Keep counting's kind flip.
      store_restart(app_context_store(), t, now);
      testlog("restart", "countdown_alarm");
      break;
    case OPTION_SNOOZE_QUARTER: {
      if (!timer_add_quarter_available(t)) break;
      int32_t quarter = timer_quarter_amount(t);
      timer_snooze_seconds(t, (uint32_t)quarter, now);
      log_snooze_quarter(quarter);
      break;
    }
    case OPTION_DELETE:
      store_remove(app_context_store(), t->cookie);
      break;
    default:
      break;
  }
  resolve_after_mutation();
}

static void options_back_click_handler(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
  nav_back_from_alarm_options();
}

// Pitfall (see the matching note on screen_actions.c's click_config_provider):
// scroll_layer_set_callbacks() does a FULL STRUCT ASSIGNMENT, so wiring an
// extra Back handler that way silently REPLACES MenuLayer's own internal
// click_config_provider (the one that wires Select to `.select_click` and
// Up/Down to real menu-selection navigation) -- Select on this window would
// become a 100%-reproducible dead button as a result. Avoided the same way
// as screen_actions.c: one ordinary, non-reentrant window_set_click_config_
// provider() using only the public MenuLayer API, no menu_layer_set_click_
// config_onto_window()/scroll_layer_set_callbacks() at all.
static void options_up_click_handler(ClickRecognizerRef recognizer, void *context) {
  (void)context;
  // MenuLayer clamps (no wrap) at row 0, so Up on the top row used to be
  // a silent no-op -- the only way back to the alarm hero was Back.
  // Up-at-top now reverses the Down that opened this menu, mirroring
  // options_back_click_handler. Non-destructive: only re-points the view.
  // Only a DISCRETE press at row 0 reverses -- a held/auto-repeating Up
  // landing on row 0 falls through to the menu_layer_set_selected_next()
  // clamp below instead of cancelling out.
  if (menu_layer_get_selected_index(s_options_menu).row == 0 &&
      !click_recognizer_is_repeating(recognizer)) {
    nav_back_from_alarm_options();
    return;
  }
  menu_layer_set_selected_next(s_options_menu, true, MenuRowAlignCenter, true);
  if (s_options_scroll_indicator != NULL) layer_mark_dirty(s_options_scroll_indicator);
}

static void options_down_click_handler(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
  menu_layer_set_selected_next(s_options_menu, false, MenuRowAlignCenter, true);
  if (s_options_scroll_indicator != NULL) layer_mark_dirty(s_options_scroll_indicator);
}

static void options_select_click_handler(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
  MenuIndex idx = menu_layer_get_selected_index(s_options_menu);
  options_select_click(s_options_menu, &idx, NULL);
}

static void options_click_config_provider(void *context) {
  (void)context;
  window_single_repeating_click_subscribe(BUTTON_ID_UP, 100, options_up_click_handler);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 100, options_down_click_handler);
  window_single_click_subscribe(BUTTON_ID_SELECT, options_select_click_handler);
  window_single_click_subscribe(BUTTON_ID_BACK, options_back_click_handler);
}

// The floating dark-red (or indigo, repeat) strip -- kind label +
// "ringing ^m:ss" value, replacing the plain paper clock-strip. The
// leading "^" is a plain ASCII stand-in for "▲" (this codebase's
// established convention for glyphs with no safe font coverage at small
// sizes, same as the "-"/"x" ASCII substitutes for "·"/"×" elsewhere).
static void options_strip_update_proc(Layer *layer, GContext *ctx) {
  const Timer *t = current_timer();
  if (t == NULL) return;  // defensive
  ViewColorToken token = view_kind_color_token(t->kind, VIEW_HERO_ALARM);
  char overrun[16];
  format_hms(overrun, sizeof overrun, timer_overrun_s(t, time(NULL)));
  char value[32];  // sized past GCC's -Wformat-truncation worst case for overrun[16]
  snprintf(value, sizeof value, "ringing ^%s", overrun);
  // Same DEEP-variant fix as ACTIONS' identical strip component (see
  // render.h's doc comment on render_kind_color_deep) -- a no-op for the two
  // tokens reachable here (ALARM/REPEAT, both already dark), kept for
  // consistency with every other render_floating_strip() call site.
  render_floating_strip(ctx, layer_get_bounds(layer), render_kind_color_deep(token), t->kind,
                         render_kind_label(t->kind), value);
}

static void options_window_appear(Window *window) {
  (void)window;
  menu_layer_reload_data(s_options_menu);
  // See screen_actions.c's matching fix for the root cause
  // (MenuRowAlignNone leaves the scroll offset stuck) -- same class of
  // window (repeat's 3-row list never scrolls, but the one-off 4-row list
  // can).
  menu_layer_set_selected_index(s_options_menu, MenuIndex(0, 0), MenuRowAlignTop, false);
  if (s_options_scroll_indicator != NULL) layer_mark_dirty(s_options_scroll_indicator);
}

// Both option lists (4-row one-off, 3-row repeat) fit within one screen at
// the current row pitch, so render_scroll_indicator self-skips today --
// wired anyway for uniform capability across every menu screen.
static void options_scroll_indicator_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  int total = options_get_num_rows(s_options_menu, 0, NULL);
  int visible = (int)(bounds.size.h / RENDER_MENU_ROW_H);
  if (visible < 1) visible = 1;
  MenuIndex sel = menu_layer_get_selected_index(s_options_menu);
  render_scroll_indicator(ctx, bounds, total, visible, (int)sel.row);
}

// MenuLayer created ONCE here, never in a .load handler -- see the same
// note in screen_actions.c.
Window *screen_alarm_options_create(void) {
  s_options_window = window_create();
  window_set_background_color(s_options_window, COLOR_PAPER);
#ifdef GALLERY_MODE
  // Screen gallery (inner loop, src/c/gallery.c) -- see screen_set.c's
  // matching comment. Never compiled into a release build.
  window_set_click_config_provider(s_options_window, gallery_click_config_provider);
#else
  window_set_click_config_provider(s_options_window, options_click_config_provider);
#endif
  window_set_window_handlers(s_options_window, (WindowHandlers){
                                                    .appear = options_window_appear,
                                                });

  Layer *root = window_get_root_layer(s_options_window);
  GRect bounds = layer_get_bounds(root);

  // Floating colored strip, same pattern as screen_actions.c -- this
  // window's content is a MenuLayer, whose draw_row can only paint inside
  // a cell.
  s_options_strip = layer_create(GRect(0, 0, bounds.size.w, RENDER_STRIP_H));
  layer_set_update_proc(s_options_strip, options_strip_update_proc);
  layer_add_child(root, s_options_strip);

  s_options_menu =
      menu_layer_create(GRect(0, RENDER_STRIP_H, bounds.size.w, bounds.size.h - RENDER_STRIP_H));
  // See screen_home.c's matching fix -- the SDK's MenuLayer defaults
  // highlight background to full-cell black; this makes the un-inset
  // margin around render_menu_row()'s own smaller pill read as paper
  // instead.
  menu_layer_set_highlight_colors(s_options_menu, RENDER_COLOR_PAPER, GColorBlack);
  // NO .select_click here -- see screen_actions.c's matching fix/comment
  // for why this now-dead field is actively misleading to leave in, not
  // just harmless.
  menu_layer_set_callbacks(s_options_menu, NULL,
                            (MenuLayerCallbacks){
                                .get_num_rows = options_get_num_rows,
                                .get_cell_height = options_get_cell_height,
                                .draw_row = options_draw_row,
                            });
  layer_add_child(root, menu_layer_get_layer(s_options_menu));

  s_options_scroll_indicator =
      layer_create(GRect(bounds.size.w - SCROLL_INDICATOR_WIDTH - SCROLL_INDICATOR_MARGIN,
                          RENDER_STRIP_H, SCROLL_INDICATOR_WIDTH, bounds.size.h - RENDER_STRIP_H));
  layer_set_update_proc(s_options_scroll_indicator, options_scroll_indicator_update_proc);
  layer_add_child(root, s_options_scroll_indicator);

  return s_options_window;
}

void screen_alarm_options_destroy(void) {
  layer_destroy(s_options_scroll_indicator);
  s_options_scroll_indicator = NULL;
  menu_layer_destroy(s_options_menu);
  s_options_menu = NULL;
  layer_destroy(s_options_strip);
  s_options_strip = NULL;
  window_destroy(s_options_window);
  s_options_window = NULL;
}

void screen_alarm_options_mark_clock_dirty(void) {
  if (s_options_strip != NULL) layer_mark_dirty(s_options_strip);
}

void screen_alarm_set_cookie(int32_t cookie) {
  s_cookie = cookie;
  if (s_hero_root != NULL) layer_mark_dirty(s_hero_root);
}
