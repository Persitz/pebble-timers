// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

#include "screen_set.h"

#include <stdio.h>

#include "app_context.h"
#include "fonts_adapter.h"
#include "hold.h"
#include "lib/format.h"
#include "lib/preset_store.h"
#include "lib/set_machine.h"
#include "lib/store.h"
#include "nav.h"
#include "persist_adapter.h"
#include "persist_kind_adapter.h"
#include "persist_preset_adapter.h"
#include "render.h"
#include "testlog_am.h"
#include "vibes_adapter.h"
#include "wakeup_adapter.h"
#ifdef GALLERY_MODE
#include "gallery.h"
#endif

// Alias onto the shared constant (render.h, already included) -- see
// screen_home.c's matching fix.
#define COLOR_PAPER RENDER_COLOR_PAPER
#define COLOR_INK GColorFromHEX(0x2A2620)

// The colon-separated h:mm:sec group, per-field labels, active-field
// emphasis (size+color+underline, NOT an inverted block) + up/down arrows --
// replaces the old 3-isolated-LECO-block "tofu box" layout entirely, which
// was the weakest-reading layout this screen had used. Field horizontal
// centers land at hr~40/min~100/sec~160 (min is the group's horizontal
// center).
#define TITLE_RECT GRect(13, 12, 140, 16)
#define HR_CX 38
#define COLON1_CX 68
#define MIN_CX 100
#define COLON2_CX 132
#define SEC_CX 162
#define NUMBER_CY 100    // vertical center of the number text
#define ARROW_UP_Y 72    // active field's up-arrow apex y
#define UNDERLINE_Y 122  // active field's underline y
#define ARROW_DOWN_Y 130 // active field's down-arrow base y
#define LABEL_Y 136
// Bottom-anchored near the bottom of the display, not directly under the
// unit labels -- was y=154, right under the unit labels (LABEL_Y=136+14=150,
// a 4px gap), leaving ~45px of dead white space below it and leaving most of
// this 228px-tall screen unused. y=200 + 20px box height leaves an 8px
// bottom margin, close to the intended ~y=207 footer position.
#define FOOTER_Y 200

// Presets: replaces the old 2-valued s_edit_cookie==0/!=0 gate -- a fourth
// mode on a 2-valued int would be unreadable and error-prone. CREATE and
// EDIT_INTERVAL preserve their exact prior behavior byte-for-byte (mapped
// onto this enum 1:1, see every s_mode-branching site below); PRESET_NEW/
// PRESET_EDIT are new.
typedef enum {
  SET_MODE_CREATE,
  SET_MODE_EDIT_INTERVAL,
  SET_MODE_PRESET_NEW,
  SET_MODE_PRESET_EDIT,
} SetMode;

static Window *s_window;
static Layer *s_root_layer;
static SetState s_set;
static SetMode s_mode;         // SET_MODE_CREATE (0) by default
static int32_t s_edit_cookie;  // meaningful only in SET_MODE_EDIT_INTERVAL
static int s_preset_index;     // meaningful only in SET_MODE_PRESET_EDIT
static bool s_seeded_from_voice;
// SET_MODE_EDIT_INTERVAL has exactly one entry point -- the ACTIONS screen's
// "Edit interval" row. The repeat hero's Up-hold performs an in-place
// Restart (screen_hero.c's rp_restart) instead of opening SET, so it never
// reaches this mode. Back-cancel below therefore always targets ACTIONS
// unconditionally, the same fixed-target pattern every other mode uses
// (HOME for CREATE, LIBRARY for both PRESET_* modes).

// The kind toggle (long-press Select / tap-on-chip) is available in every
// mode EXCEPT editing an existing repeat's interval (that mode has an
// already-fixed kind -- REPEAT -- with nothing to toggle to).
static bool set_mode_allows_kind_toggle(void) {
  return s_mode != SET_MODE_EDIT_INTERVAL;
}

static const char *set_mode_title(void) {
  switch (s_mode) {
    case SET_MODE_EDIT_INTERVAL: return "EDIT INTERVAL";
    case SET_MODE_PRESET_NEW: return "SAVE PRESET";
    case SET_MODE_PRESET_EDIT: return "EDIT PRESET";
    case SET_MODE_CREATE:
    default: return "NEW TIMER";
  }
}

// The hour field is NOT zero-padded ("0", not "00") -- matching this app's
// own format_hms convention elsewhere ("%d:%02d:%02d", hours never
// zero-padded). Minutes/seconds keep their zero-padded "%02d".
static void field_text(SetField field, const SetState *s, char *out, size_t out_size) {
  if (field == SET_FIELD_H) {
    snprintf(out, out_size, "%d", s->h);
    return;
  }
  int v = (field == SET_FIELD_M) ? s->m : s->s;
  snprintf(out, out_size, "%02d", v);
}

// Active field: 44px (LECO_42, nearest system size), kind-colored (digits,
// underline, AND arrows), NOT an inverted black block (this was the
// previous design). Arrows used to hardcode windsor-tan regardless of kind;
// they now flip to indigo on the repeat toggle along with everything else
// in this field.
static void draw_active_field(GContext *ctx, int16_t cx, const char *text, GColor kind_color) {
  render_fill_triangle(ctx, GPoint(cx, ARROW_UP_Y - 5), GPoint(cx - 6, ARROW_UP_Y + 5),
                        GPoint(cx + 6, ARROW_UP_Y + 5), kind_color);
  render_fill_triangle(ctx, GPoint(cx, ARROW_DOWN_Y + 5), GPoint(cx - 6, ARROW_DOWN_Y - 5),
                        GPoint(cx + 6, ARROW_DOWN_Y - 5), kind_color);

  // SET's active field is one of the app's 3 most-visible numeral sites --
  // JetBrains Mono at the same 42px LECO_42_NUMBERS already drew at (shared
  // with render_hero_value's raster -- both drew at this exact system-font
  // size).
  GFont font = fonts_adapter_numeral_42();
  GSize text_size = graphics_text_layout_get_content_size(text, font, GRect(0, 0, 80, 56),
                                                            GTextOverflowModeFill,
                                                            GTextAlignmentLeft);
  GRect rect = GRect((int16_t)(cx - 40), (int16_t)(NUMBER_CY - 28), 80, 56);
  graphics_context_set_text_color(ctx, kind_color);
  graphics_draw_text(ctx, text, font, rect, GTextOverflowModeFill, GTextAlignmentCenter, NULL);

  GRect underline = GRect((int16_t)(cx - text_size.w / 2), UNDERLINE_Y, text_size.w, 4);
  graphics_context_set_fill_color(ctx, kind_color);
  graphics_fill_rect(ctx, underline, 2, GCornersAll);
}

// Inactive field: 34px (LECO_32_BOLD), light-gray -- no arrows, no underline.
static void draw_inactive_field(GContext *ctx, int16_t cx, const char *text) {
  // Not one of the 3 named most-visible sites, but this tier draws at the
  // EXACT same 32px system-font size HOME's 1-timer row already got a JBM
  // raster for (fonts_adapter_numeral_32) -- reusing it here is zero
  // additional flash/RAM (the raster is already loaded) and avoids mixing
  // JBM (active field) with LECO (inactive fields) within the same SET
  // screen.
  GFont font = fonts_adapter_numeral_32();
  GRect rect = GRect((int16_t)(cx - 35), (int16_t)(NUMBER_CY - 22), 70, 44);
  // GColorLightGray quantizes to (170,170,170) on the white-quantized SET
  // paper -- ~2.3:1 contrast, below WCAG's 3:1 large-text floor.
  // RENDER_COLOR_LABEL is the app's dimmed/secondary-text token, darkened to
  // black (a prior dark-gray-85 value read at only ~7.5:1 and looked washed
  // out on-device) -- and already used by the per-field unit labels directly
  // below, so the inactive digits match this screen's existing secondary-text
  // color instead of introducing a new gray.
  graphics_context_set_text_color(ctx, RENDER_COLOR_LABEL);
  graphics_draw_text(ctx, text, font, rect, GTextOverflowModeFill, GTextAlignmentCenter, NULL);
}

static void draw_field(GContext *ctx, int16_t cx, SetField field, GColor kind_color,
                        const char *label) {
  bool active = s_set.field == field;
  char text[4];
  field_text(field, &s_set, text, sizeof text);
  if (active) {
    draw_active_field(ctx, cx, text, kind_color);
  } else {
    draw_inactive_field(ctx, cx, text);
  }

  GRect label_rect = GRect((int16_t)(cx - 25), LABEL_Y, 50, 14);
  graphics_context_set_text_color(ctx, RENDER_COLOR_LABEL);
  graphics_draw_text(ctx, label, fonts_get_system_font(FONT_KEY_GOTHIC_14), label_rect,
                      GTextOverflowModeFill, GTextAlignmentCenter, NULL);
}

static void draw_colon(GContext *ctx, int16_t cx) {
  GRect rect = GRect((int16_t)(cx - 10), (int16_t)(NUMBER_CY - 20), 20, 40);
  // Match the inactive-field fix above -- keep the colons in the same
  // legible mid-gray as the digits they sit between.
  graphics_context_set_text_color(ctx, RENDER_COLOR_LABEL);
  graphics_draw_text(ctx, ":", fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD), rect,
                      GTextOverflowModeFill, GTextAlignmentCenter, NULL);
}

// The band above the footer and below the sec/min/hr unit labels (roughly
// y=160-185) is the kind chip's home, always rendered and driven by
// set_machine_is_repeat. The chip is deliberately passive -- a label plus a
// gesture hint -- with no arrows or underline of its own; an active-field
// look (up/down arrows, an underline on the chip itself) was considered and
// rejected, since the cursor never actually leaves a numeric field.
#define KIND_LABEL_Y 168

// Hit-rect for the touch trigger -- must stay in sync with the drawn band
// above (KIND_LABEL_Y +- the label/hint lines below).
//
// The rect covers the entire area below the dial's unit labels, not just
// the chip's own rendered text: top edge at LABEL_Y + 14 (exactly the unit
// labels' bottom edge), full 200px screen width, down to the bottom of the
// display (228, emery's fixed screen height -- this window has no status
// bar, so root-layer bounds equal the full display). A narrower rect
// hugging just the chip label and hint text left a visually-ambiguous dead
// band directly above the chip where a tap could land as a near-miss;
// widening the top edge to the unit labels' own bottom edge removes that
// gap entirely. Width matches draw_kind_chip's own chip_rect and hint_rect,
// both of which already reserve the full bounds width (GTextAlignmentCenter).
// Dial taps (strictly above LABEL_Y + 14 -- the h:mm:ss fields, arrows, and
// the unit labels themselves) are unaffected, since this rect starts
// exactly at the unit labels' bottom edge and never higher.
#define KIND_CHIP_HIT GRect(0, LABEL_Y + 14, 200, 228 - (LABEL_Y + 14))

static const char *kind_label(bool is_repeat) {
  return is_repeat ? "Repeat" : "Countdown";
}

// A small PASSIVE chip (no arrows/underline -- the cursor never leaves a
// numeric field) showing the current kind + a discoverable gesture hint.
// Drawn only when the kind toggle is available (duration >= 1s) and not in
// edit-interval mode (root_layer_update_proc's own gate).
static void draw_kind_chip(GContext *ctx, GRect bounds, GColor kind_color, bool is_repeat) {
  const char *label = kind_label(is_repeat);
  GRect chip_rect = GRect(0, (int16_t)(KIND_LABEL_Y - 10), bounds.size.w, 20);
  graphics_context_set_text_color(ctx, kind_color);
  graphics_draw_text(ctx, label, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD), chip_rect,
                      GTextOverflowModeFill, GTextAlignmentCenter, NULL);

  GRect hint_rect = GRect(0, (int16_t)(KIND_LABEL_Y + 12), bounds.size.w, 16);
  graphics_context_set_text_color(ctx, RENDER_COLOR_LABEL);
  // "= repeat" mis-read on the Repeat state itself (the chip already reads
  // "Repeat" -- the hint then redundantly/confusingly said "= repeat"
  // again). Directional "to switch" reads correctly regardless of which
  // kind is currently shown. Text-only: this hint names two triggers ("hold
  // or tap"), not a single directional hold-Up/Down gesture, so the
  // up/down hold glyphs (render_up_glyph/render_more_glyph) don't have a
  // natural single meaning here, and the 16px single-line centered hint has
  // no clean spot to add one without crowding.
  graphics_draw_text(ctx, "hold or tap to switch", fonts_get_system_font(FONT_KEY_GOTHIC_14),
                      hint_rect, GTextOverflowModeFill, GTextAlignmentCenter, NULL);
}

static void root_layer_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, COLOR_PAPER);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  render_clock(ctx, bounds, clock_is_24h_style());

  // "NEW TIMER" title (top-left) -- "EDIT INTERVAL" in the edit-interval
  // variant, added beyond the screen's original create-only scope. Presets:
  // "SAVE PRESET"/"EDIT PRESET" for the two new modes (set_mode_title()'s
  // title table).
  graphics_context_set_text_color(ctx, RENDER_COLOR_LABEL);
  graphics_draw_text(ctx, set_mode_title(), fonts_get_system_font(FONT_KEY_GOTHIC_14), TITLE_RECT,
                      GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  // Create/preset modes have no established kind yet (a fresh countdown/
  // stopwatch) -- orange, matching HOME's own "+ New timer" affordance,
  // UNLESS the real kind toggle (long-press Select / tap-on-chip) has set
  // effective repeat -- then it recolors indigo exactly like the shipped
  // edit-interval variant, reading as the same kind everywhere. Edit-
  // INTERVAL mode is always a repeat interval -- indigo, matching that
  // kind's token everywhere else. At 0:00, set_machine_is_repeat is always
  // false (duration-gated), so the dial stays orange there regardless of any
  // preserved raw intent.
  bool dial_is_repeat = set_mode_allows_kind_toggle() && set_machine_is_repeat(&s_set);
  // The active field's digits/underline and the kind chip's label are
  // kind-colored text drawn directly on paper (COLOR_PAPER above) -- the
  // same "value reads on a light background" context as hero/row values, so
  // this uses the DEEP tier (RENDER_COLOR_COUNTDOWN_DEEP/WindsorTan) instead
  // of the bright GColorOrange, matching the rest of the app. Repeat/indigo
  // is unchanged (render_kind_color_deep is a no-op for that token --
  // already dark enough).
  GColor kind_color =
      (s_mode == SET_MODE_EDIT_INTERVAL || dial_is_repeat) ? GColorIndigo : RENDER_COLOR_COUNTDOWN_DEEP;

  draw_field(ctx, HR_CX, SET_FIELD_H, kind_color, "hr");
  draw_colon(ctx, COLON1_CX);
  draw_field(ctx, MIN_CX, SET_FIELD_M, kind_color, "min");
  draw_colon(ctx, COLON2_CX);
  draw_field(ctx, SEC_CX, SET_FIELD_S, kind_color, "sec");

  // Chip is visible whenever the toggle is available (duration >= 1s) and
  // the mode allows it (every mode except edit-interval, reusing the
  // create-repeat toggle) -- so the gesture is discoverable on every
  // countdown/repeat create/preset-dial, not just once already toggled.
  bool chip_visible = set_mode_allows_kind_toggle() && set_machine_kind_toggle_available(&s_set);
  if (chip_visible) {
    draw_kind_chip(ctx, bounds, kind_color, dial_is_repeat);
  }

  char footer[32];
  bool show_footer = true;
  if (s_mode == SET_MODE_EDIT_INTERVAL) {
    snprintf(footer, sizeof footer, "Saving restarts the interval.");
  } else if (dial_is_repeat) {
    // Editing an existing repeat's interval, or any mode once kind is
    // effectively repeat -- the create-mode "0:00 = stopwatch"/"ends HH:MM"
    // footers don't apply (an edit-interval session never creates a
    // stopwatch, and a repeat kind has no "end clock" meaning until/unless
    // it's committed).
    snprintf(footer, sizeof footer, "repeat interval");
  } else if (s_mode == SET_MODE_PRESET_NEW || s_mode == SET_MODE_PRESET_EDIT) {
    // A preset dial is never "started," so it never shows "ends HH:MM"/"0:00 =
    // stopwatch" -- just the kind it will save as.
    //
    // Once the kind chip above is visible (duration >= 1s), it already names
    // the kind ("Countdown") once; repeating the same word here as a plain
    // lowercase "countdown" line would be a redundant duplicate. The REPEAT
    // branch above doesn't have this problem: "repeat interval" is genuinely
    // additional information (what this number means), not just the kind name
    // again. Match the plain-create dial's own single-label rule -- one
    // on-screen kind indicator at a time -- by only showing this word when
    // there's no chip yet (duration < 1s).
    if (chip_visible) {
      show_footer = false;
      footer[0] = '\0';
    } else {
      snprintf(footer, sizeof footer, "countdown");
    }
  } else if (set_machine_is_stopwatch(&s_set)) {
    snprintf(footer, sizeof footer, "0:00 = stopwatch");
  } else {
    time_t end = set_machine_end_clock(&s_set, time(NULL));
    struct tm *tm_end = localtime(&end);
    char clock_str[8];
    if (tm_end != NULL) {
      format_clock(clock_str, sizeof clock_str, tm_end->tm_hour, tm_end->tm_min,
                   clock_is_24h_style());
    } else {
      clock_str[0] = '\0';
    }
    snprintf(footer, sizeof footer, "ends %s", clock_str);
  }
  if (show_footer) {
    GRect footer_rect = GRect(0, FOOTER_Y, bounds.size.w, 20);
    graphics_context_set_text_color(ctx, RENDER_COLOR_LABEL);
    graphics_draw_text(ctx, footer, fonts_get_system_font(FONT_KEY_GOTHIC_14), footer_rect,
                        GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  }
}

static void start_timer(void) {
  TimerStore *store = app_context_store();
  int32_t duration_s = set_machine_duration_s(&s_set);
  // 0:00 is ALWAYS a stopwatch (any repeat intent is gated off by
  // set_machine_is_repeat itself at 0:00, so there is no "repeat + 0:00"
  // state to refuse here -- it reinterprets as stopwatch, matching the
  // existing 0:00 shortcut).
  TimerKind kind;
  if (duration_s == 0) {
    kind = KIND_STOPWATCH;
  } else if (set_machine_is_repeat(&s_set)) {
    kind = KIND_REPEAT;
  } else {
    kind = KIND_COUNTDOWN;
  }
  time_t now = time(NULL);

  int32_t cookie;
  if (!store_add(store, kind, duration_s, now, &cookie)) {
    // The chosen 17th-timer overflow surface -- refuse the create, testLog a
    // marker, and stay on SET (no navigation, no crash). A user-facing toast
    // is future polish; MAX_TIMERS=16 makes this a rare edge in practice.
    // HOME's own MAX-reached footer and Library's launch_preset() both give
    // a reject-vibe on this exact refusal; this guard was the one silent
    // holdout -- added for 3-way parity (same "the store is full" feel
    // everywhere it's refused).
    testlog("store", "full");
    vibes_adapter_reject();
    return;
  }

  persist_adapter_save(store);
  wakeup_adapter_replan(store, now);
  if (!s_seeded_from_voice && kind != KIND_STOPWATCH) persist_kind_adapter_save(kind);
  testlog("timer:create", kind == KIND_REPEAT     ? "repeat"
                           : kind == KIND_STOPWATCH ? "stopwatch"
                                                     : "countdown");
  nav_go_hero(cookie);
}

// Edit interval's commit -- mirrors start_timer()'s shape (mutate, persist,
// replan, testlog, navigate) but mutates the EXISTING timer via lib/timer's
// timer_edit_interval instead of store_add-ing a new one.
static void commit_edit_interval(void) {
  TimerStore *store = app_context_store();
  Timer *t = store_find(store, s_edit_cookie);
  if (t == NULL) {
    // Defensive dead-ref fallback (same shape as screen_hero.c's
    // current_timer() callers). The timer being edited can fire mid-edit --
    // take-over pre-empts SET just like any other screen, capturing
    // VD_SET(cookie) as the resume target -- and then get resolved away
    // (Stop/Done/keep counting) while the user is still mid-dial. What
    // actually prevents reaching this branch today is lib/nav_policy's
    // nav_on_resolve dead-reference guard, which covers VD_SET(cookie != 0)
    // and redirects that stale resume to HOME before the user can press
    // Select against a deleted timer's edit session. Kept as defense-in-depth,
    // since this fallback's safety depends on nav_on_resolve's guard staying
    // correctly wired for SET.
    nav_go_home();
    return;
  }
  int32_t duration_s = set_machine_duration_s(&s_set);
  if (duration_s <= 0) {
    // A 0:00 repeat interval is meaningless (unlike create-mode's 0:00 =
    // stopwatch escape hatch) -- refuse and stay on SET, same "refuse loudly,
    // don't silently misbehave" shape as start_timer()'s store-full guard.
    testlog("edit_interval", "invalid");
    return;
  }
  int32_t cookie = t->cookie;
  timer_edit_interval(t, duration_s, time(NULL));
  persist_adapter_save(store);
  wakeup_adapter_replan(store, time(NULL));
  testlog("edit_interval", "ok");
  nav_go_hero(cookie);
}

// Presets: SAVE PRESET's commit -- unlike start_timer()/commit_edit_interval(),
// this NEVER touches the TimerStore; it appends to the separate PresetStore
// and returns to the Library, without starting anything (a preset is
// read-only template data until launched from the Library, which is a
// wholly separate call site, screen_library.c).
static void commit_preset_new(void) {
  int32_t d = set_machine_duration_s(&s_set);
  if (d <= 0) {
    // No stopwatch presets (lib/preset_store.h's own add() validation would
    // refuse this too) -- refuse and stay on SET, same "refuse loudly, don't
    // silently misbehave" shape as start_timer()'s store-full guard /
    // commit_edit_interval()'s 0:00 guard.
    testlog("preset_new", "invalid");
    return;
  }
  TimerKind k = set_machine_is_repeat(&s_set) ? KIND_REPEAT : KIND_COUNTDOWN;
  if (!preset_store_add(app_context_presets(), k, d)) {
    // Full at MAX_PRESETS=12, refuse and stay, same shape as the timer
    // store's own full-refusal surface.
    testlog("preset", "full");
    return;
  }
  persist_preset_adapter_save(app_context_presets());
  if (!s_seeded_from_voice) persist_kind_adapter_save(k);
  testlog("preset_new", k == KIND_REPEAT ? "repeat" : "countdown");
  nav_go_library();  // NOT nav_go_hero -- a preset save never starts a timer
}

// Presets: EDIT PRESET's commit -- overwrites the preset at
// s_preset_index in place (does NOT start a timer, unlike commit_edit_
// interval's live-timer mutation). preset_store_edit's return is
// deliberately not checked -- a false (stale/OOB index, e.g. the preset was
// deleted from elsewhere while this dial was open) is defensive-only in
// today's single-window app (nothing else can mutate the PresetStore
// concurrently); returning to the Library either way is always correct.
static void commit_preset_edit(void) {
  int32_t d = set_machine_duration_s(&s_set);
  if (d <= 0) {
    testlog("preset_edit", "invalid");
    return;
  }
  TimerKind k = set_machine_is_repeat(&s_set) ? KIND_REPEAT : KIND_COUNTDOWN;
  preset_store_edit(app_context_presets(), s_preset_index, k, d);
  persist_preset_adapter_save(app_context_presets());
  testlog("preset_edit", "ok");
  nav_go_library();
}

static void select_click_handler(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
  SetNavResult r = set_machine_field_right(&s_set);
  if (r == SET_NAV_START) {
    switch (s_mode) {
      case SET_MODE_EDIT_INTERVAL:
        commit_edit_interval();
        break;
      case SET_MODE_PRESET_NEW:
        commit_preset_new();
        break;
      case SET_MODE_PRESET_EDIT:
        commit_preset_edit();
        break;
      case SET_MODE_CREATE:
      default:
        start_timer();
        break;
    }
    return;
  }
  layer_mark_dirty(s_root_layer);
}

static void back_click_handler(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
  SetNavResult r = set_machine_field_left(&s_set);
  if (r == SET_NAV_CANCEL) {
    switch (s_mode) {
      case SET_MODE_EDIT_INTERVAL:
        // Edit mode is reached only from ACTIONS' own "Edit interval" row --
        // cancel returns there. (The repeat hero's own Up-hold used to be a
        // second origin needing a per-visit "which one" flag; removed -- see
        // s_edit_cookie's own comment above.)
        nav_go_actions(s_edit_cookie);
        break;
      case SET_MODE_PRESET_NEW:
      case SET_MODE_PRESET_EDIT:
        // Presets: a preset dial is reached FROM the Library -- cancel
        // returns there (nav_go_library(), the real VD_LIBRARY target).
        nav_go_library();
        break;
      case SET_MODE_CREATE:
      default:
        nav_go_home();
        break;
    }
    return;
  }
  layer_mark_dirty(s_root_layer);
}

static void on_repeat_step(int delta) {
  int step = delta > 0 ? 1 : -1;
  int count = delta > 0 ? delta : -delta;
  for (int i = 0; i < count; i++) {
    set_machine_adjust(&s_set, step);
  }
  layer_mark_dirty(s_root_layer);
}

// The single code path both triggers (long-press Select, tap-on-chip) call.
// Self-guards on edit mode and the duration gate, so it is a true no-op at
// 0:00 (no vibe, no testlog) -- matching the chip's own visibility rule.
static void on_kind_toggle(void) {
  if (!set_mode_allows_kind_toggle()) return;               // no kind change while editing an interval
  if (!set_machine_kind_toggle_available(&s_set)) return;   // inert at 0:00 (stopwatch, no chip)
  set_machine_toggle_kind(&s_set);
  vibes_adapter_confirm();  // haptic ack (native long-click/a silent tap give none on their own)
  testlog("set_kind", set_machine_is_repeat(&s_set) ? "repeat" : "countdown");
  if (s_root_layer != NULL) layer_mark_dirty(s_root_layer);
}

static void kind_long_click_handler(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
  on_kind_toggle();
}

#ifndef GALLERY_MODE
// Touch is app-global (not window-routed), so the app must subscribe on SET
// appear and unsubscribe on SET disappear -- see window_appear/
// window_disappear below. SDK 4.17's app header exposes only
// raw Touchdown/Liftoff/PositionUpdate events, no tap gesture, so a tap is
// synthesized here: latch the touchdown point, and on liftoff treat it as a
// tap only if BOTH touchdown and liftoff fell inside the chip hit-rect
// (rejects swipes crossing the chip). on_kind_toggle() self-guards on the
// duration gate, so a tap in the chip band at 0:00 (where the chip isn't
// even drawn) is inert -- no special-casing needed here.
static GPoint s_touch_down;
static bool s_touch_down_valid;

static void touch_handler(const TouchEvent *event, void *context) {
  (void)context;
  GRect hit = KIND_CHIP_HIT;
  switch (event->type) {
    case TouchEvent_Touchdown:
      s_touch_down = GPoint(event->x, event->y);
      s_touch_down_valid = grect_contains_point(&hit, &s_touch_down);
      break;
    case TouchEvent_Liftoff: {
      GPoint up = GPoint(event->x, event->y);
      if (s_touch_down_valid && grect_contains_point(&hit, &up)) on_kind_toggle();
      s_touch_down_valid = false;
      break;
    }
    case TouchEvent_PositionUpdate:
    default:
      break;  // no drag semantics needed
  }
}
#endif  // GALLERY_MODE

static void click_config_provider(void *context) {
  (void)context;
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click_handler);
  window_long_click_subscribe(BUTTON_ID_SELECT, 700, kind_long_click_handler, NULL);
  window_single_click_subscribe(BUTTON_ID_BACK, back_click_handler);
  hold_bind_set_repeat(s_window, on_repeat_step);  // auto-repeat on Up/Down
}

static void window_load(Window *window) {
  s_root_layer = window_get_root_layer(window);
  layer_set_update_proc(s_root_layer, root_layer_update_proc);
}

static void window_unload(Window *window) {
  (void)window;
  s_root_layer = NULL;
}

// Subscribe/unsubscribe strictly on appear/disappear -- the touch service is
// app-global, not window-routed, so leaving this subscribed while another
// screen (e.g. an alarm preempting SET) is on top would let a tap silently
// toggle SET's hidden kind. Guarded under GALLERY_MODE so gallery renders
// stay byte-identical (the gallery already swaps the whole click provider at
// create time; touch is likewise gallery-irrelevant).
static void window_appear(Window *window) {
  (void)window;
#ifndef GALLERY_MODE
  if (touch_service_is_enabled()) touch_service_subscribe(touch_handler, NULL);
#endif
}

static void window_disappear(Window *window) {
  (void)window;
#ifndef GALLERY_MODE
  touch_service_unsubscribe();
#endif
}

Window *screen_set_create(void) {
  s_window = window_create();
  window_set_background_color(s_window, COLOR_PAPER);
#ifdef GALLERY_MODE
  // Screen gallery (inner loop, src/c/gallery.c): every real screen's
  // click_config_provider is swapped for the gallery's own at CREATE time --
  // rebinds Up/Down to scene-cycling instead of each screen's normal
  // behavior, so the SAME real windows can be reused for rendering without
  // fighting the gallery for button ownership. Never compiled into a
  // release build.
  window_set_click_config_provider(s_window, gallery_click_config_provider);
#else
  window_set_click_config_provider(s_window, click_config_provider);
#endif
  window_set_window_handlers(s_window, (WindowHandlers){
                                            .load = window_load,
                                            .unload = window_unload,
                                            .appear = window_appear,
                                            .disappear = window_disappear,
                                        });
  return s_window;
}

void screen_set_destroy(void) {
  window_destroy(s_window);
  s_window = NULL;
}

void screen_set_reset(void) {
  s_mode = SET_MODE_CREATE;
  s_edit_cookie = 0;
  s_preset_index = 0;
  s_seeded_from_voice = false;
  set_machine_init_from_seconds_kind(&s_set, 0, persist_kind_adapter_last() == KIND_REPEAT);
  if (s_root_layer != NULL) layer_mark_dirty(s_root_layer);
}

void screen_set_start_edit(int32_t cookie) {
  s_mode = SET_MODE_EDIT_INTERVAL;
  s_edit_cookie = cookie;
  const Timer *t = store_find_const(app_context_store(), cookie);
  set_machine_init_from_seconds(&s_set, t != NULL ? t->duration_s : 0);
  s_seeded_from_voice = false;
  if (s_root_layer != NULL) layer_mark_dirty(s_root_layer);
}

// Presets: opens SET fresh (countdown default, same as a brand-new create)
// to dial a NEW preset's kind+duration -- Select-through-to-START commits
// via commit_preset_new() instead of starting a timer. Called by nav.c's
// nav_go_preset_new(), reached from screen_library.c's "Save new" row.
void screen_set_start_preset_new(void) {
  s_mode = SET_MODE_PRESET_NEW;
  s_edit_cookie = 0;
  s_seeded_from_voice = false;
  set_machine_init_from_seconds_kind(&s_set, 0, persist_kind_adapter_last() == KIND_REPEAT);
  if (s_root_layer != NULL) layer_mark_dirty(s_root_layer);
}

// Presets: re-initializes the spinner from an EXISTING preset's {kind,
// duration_s} (index-addressed -- presets have no cookie, unlike
// screen_set_start_edit's timer lookup) via set_machine_init_from_seconds_
// kind, so the kind toggle/dial colour land on the preset's own kind
// instead of always defaulting to countdown. Select-through-to-START commits
// via commit_preset_edit(), which UPDATES the preset in place (does not
// start a timer). Called by nav.c's nav_go_preset_edit(index), reached from
// the preset actions sheet's "Edit" row.
void screen_set_start_preset_edit(int index) {
  s_mode = SET_MODE_PRESET_EDIT;
  s_preset_index = index;
  const Preset *p = preset_store_get(app_context_presets(), index);
  int32_t duration_s = p != NULL ? p->duration_s : 0;
  bool is_repeat = p != NULL && p->kind == KIND_REPEAT;
  set_machine_init_from_seconds_kind(&s_set, duration_s, is_repeat);
  s_seeded_from_voice = false;
  if (s_root_layer != NULL) layer_mark_dirty(s_root_layer);
}

// Mirrors screen_set_start_preset_edit's own priming shape, but stays in
// SET_MODE_CREATE -- Select-through-to-START commits via the normal
// start_timer()/nav_go_hero() create path (screen_voice.c's parsed
// kind/duration is inert metadata to SET beyond this one prime; SET itself
// doesn't know it came from voice). Called by nav.c's
// nav_go_set_from_voice().
void screen_set_start_from_voice(int32_t duration_s, bool is_repeat) {
  s_mode = SET_MODE_CREATE;
  s_edit_cookie = 0;
  set_machine_init_from_seconds_kind(&s_set, duration_s, is_repeat);
  s_seeded_from_voice = true;
  if (s_root_layer != NULL) layer_mark_dirty(s_root_layer);
}

// Voice-into-Library: mirrors screen_set_start_preset_new()'s own
// mode-entry shape, primed from a voice parse instead of a blank dial (the
// same seed screen_set_start_from_voice() uses above, just landing in
// SET_MODE_PRESET_NEW instead of SET_MODE_CREATE).
void screen_set_start_preset_new_from_voice(int32_t duration_s, bool is_repeat) {
  s_mode = SET_MODE_PRESET_NEW;
  s_edit_cookie = 0;
  set_machine_init_from_seconds_kind(&s_set, duration_s, is_repeat);
  s_seeded_from_voice = true;
  if (s_root_layer != NULL) layer_mark_dirty(s_root_layer);
}

#ifdef GALLERY_MODE
void screen_set_debug_dial(int h, int m, int s) {
  screen_set_reset();  // real: set_machine_init(&s_set) -- create mode, field=M
  s_set.field = SET_FIELD_H;
  for (int i = 0; i < h; i++) set_machine_adjust(&s_set, 1);
  s_set.field = SET_FIELD_M;
  for (int i = 0; i < m; i++) set_machine_adjust(&s_set, 1);
  s_set.field = SET_FIELD_S;
  for (int i = 0; i < s; i++) set_machine_adjust(&s_set, 1);
  s_set.field = SET_FIELD_M;  // land back on minutes, this screen's own default field
  if (s_root_layer != NULL) layer_mark_dirty(s_root_layer);
}

// Debug-only entry point for the screen gallery: drives the real h:m:s dial
// via screen_set_debug_dial() (same real set_machine_adjust() mechanism
// Up/Down presses use), then drives kind through the REAL toggle
// (set_machine_toggle_kind) -- the same function long-press-Select/tap-chip
// call -- instead of a gallery-only debug variable, so this scene renders
// through the actual shipped state machine (fixture-purity). Self-guards
// on the duration gate exactly like
// the real gesture, so requesting repeat at 0:00 is correctly a no-op.
void screen_set_debug_kind(int h, int m, int s, bool is_repeat) {
  screen_set_debug_dial(h, m, s);
  if (is_repeat) set_machine_toggle_kind(&s_set);
  if (s_root_layer != NULL) layer_mark_dirty(s_root_layer);
}
#endif
