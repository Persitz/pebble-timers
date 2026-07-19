// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

#include "screen_hero.h"

#include <stdio.h>

#include "app_context.h"
#include "fonts_adapter.h"
#include "hold.h"
#include "lib/app_state.h"
#include "lib/format.h"
#include "lib/sched.h"
#include "lib/store.h"
#include "lib/timer.h"
#include "lib/view.h"
#include "nav.h"
#include "persist_adapter.h"
#include "render.h"
#include "screen_actions.h"
#include "testlog_am.h"
#include "vibes_adapter.h"
#include "wakeup_adapter.h"
#ifdef GALLERY_MODE
#include "gallery.h"
#endif

// Alias onto the shared constant (render.h, already included) -- see
// screen_home.c's matching alias.
#define COLOR_PAPER RENDER_COLOR_PAPER
// Vertical rhythm: value at y=48 (running/repeat) or y=60 (overtime/
// stopwatch/post-timer, which carry a header band the same height but no
// draining bar below the value, needing a touch more room above their meta
// line), draining bar at y=112, start/end row at y=120, footers bottom-
// anchored (render.h) rather than centered mid-screen -- replaces the old
// top-anchored VALUE_RECT(0,70)/BAR_RECT(20,140)/ACTION_LABEL_Y(160)/
// META_Y(190) layout that left ~30-40px of dead white space below (the
// "black band" misdiagnosis).
#define VALUE_RECT_RUNNING GRect(0, 48, 200, 56)
#define VALUE_RECT_TALL GRect(0, 60, 200, 56)
#define BAR_RECT GRect(13, 112, 174, 6)
#define META_Y 120

static Window *s_window;
static Layer *s_root_layer;
static int32_t s_cookie;

// The stopwatch's transient short-Up-press footer hint -- read by
// root_layer_update_proc's STOPWATCH case below, armed/cleared by
// sw_up_short_hint/up_hint_timer_cb (defined further down, near the other
// stopwatch control-model adapter callbacks). Declared up here (rather than
// just above their definitions) purely so the update_proc, which appears
// earlier in this file, can read s_up_hint_active.
static AppTimer *s_up_hint_timer;
static bool s_up_hint_active;

// Paused stopwatch rendering combines: the frozen value drawn muted, a
// small "Paused" chip, and a live "paused M:SS" counter that replaces the
// meta line's "since HH:MM" text. This is the one paused rendering,
// unconditional on t->is_paused, not a debug/selector branch.

// The paused-stopwatch "Paused" chip -- a plain rounded pill (no "+"/vector
// glyph, unlike render_overrun_chip which is purpose-built for the overrun
// number). Centered horizontally in the header-to-value gap (y=27-60 for the
// TALL-value heroes -- otherwise-dead space for the stopwatch branch).
#define PAUSED_CHIP_Y 34
#define PAUSED_CHIP_H 18
#define PAUSED_CHIP_PAD_X 10
static void draw_paused_chip(GContext *ctx, GRect bounds) {
  const char *text = "Paused";
  GFont font = fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
  GSize text_size = graphics_text_layout_get_content_size(
      text, font, GRect(0, 0, bounds.size.w, PAUSED_CHIP_H), GTextOverflowModeFill,
      GTextAlignmentLeft);
  int16_t chip_w = (int16_t)(text_size.w + 2 * PAUSED_CHIP_PAD_X);
  int16_t chip_x = (int16_t)((bounds.size.w - chip_w) / 2);
  GRect chip = GRect(chip_x, PAUSED_CHIP_Y, chip_w, PAUSED_CHIP_H);
  graphics_context_set_fill_color(ctx, RENDER_COLOR_MUTED);
  graphics_fill_rect(ctx, chip, PAUSED_CHIP_H / 2, GCornersAll);
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, text, font, GRect(chip_x, PAUSED_CHIP_Y, chip_w, PAUSED_CHIP_H),
                      GTextOverflowModeFill, GTextAlignmentCenter, NULL);
}

// --- Stopwatch laps-on-main. This is the shipped, unconditional rendering
// for a stopwatch's lap list (no debug/selector gate) -- a compact last-N
// list rather than a separate laps screen.
//
// Footer-legend consolidation: the stacked "^ new lap / v more" legend that
// used to live in a narrow column beside these two rows has been RETIRED --
// it duplicated the hold-footer pill's own "more" line (both taught the
// same Down="more" affordance, crowding the right edge). The Up-hold hint
// now lives in ONE place, the pill's right slot
// (render_hold_footer's `up_label` param, wired up in click_config_provider/
// the STOPWATCH branch below) -- so this band no longer reserves any right-
// side column and the two lap rows use the full content width.
#define LAPS_A_ROW0_Y 142
#define LAPS_A_ROW1_Y 157
#define LAPS_A_ROW_H 15
#define LAPS_EMPTY_HINT_Y 150

// Draws `left` left-aligned and `right` right-aligned within the SAME rect
// (two draws, mirroring render_menu_row_kv's two-column logic but as plain
// static text -- this isn't a MenuLayer cell).
static void draw_kv_line(GContext *ctx, GRect rect, const char *left, GColor left_color,
                          GFont left_font, const char *right, GColor right_color,
                          GFont right_font) {
  graphics_context_set_text_color(ctx, left_color);
  graphics_draw_text(ctx, left, left_font, rect, GTextOverflowModeTrailingEllipsis,
                      GTextAlignmentLeft, NULL);
  graphics_context_set_text_color(ctx, right_color);
  graphics_draw_text(ctx, right, right_font, rect, GTextOverflowModeTrailingEllipsis,
                      GTextAlignmentRight, NULL);
}

// The Up-hold hint's fallback wording ("new lap", falling back to "+lap" if
// tight) -- consumed by the hold-footer pill's `up_label` slot
// (click_config_provider/update_proc below) rather than an external legend
// column. Measured (not guessed) against the pill's own bounded legend
// column width (render.c's FOOTER_LEGEND_MAX_COL_W), same "measure, don't
// guess" discipline as render.c's hold_footer_more_col_w.
#define LEGEND_UP_MAX_TEXT_W 42
static const char *up_legend_label(GFont font) {
  GSize size = graphics_text_layout_get_content_size("new lap", font, GRect(0, 0, 100, 16),
                                                       GTextOverflowModeFill, GTextAlignmentLeft);
  return (size.w <= LEGEND_UP_MAX_TEXT_W) ? "new lap" : "+lap";
}

// Supersedes the old repeat_edit_legend_label -- the running repeat hero's
// Up-hold gesture is now Restart (edit-interval moved to the ACTIONS menu
// only, see screen_actions.c's REPEAT_ROW_EDIT_INTERVAL), so this is the
// legend hint for THAT gesture instead. Same measure-then-fallback scaffold
// this file already established (up_legend_label) -- "restart" (7 chars, no
// space) measured comfortably under LEGEND_UP_MAX_TEXT_W at this font, same
// ballpark as "new lap"'s own already-fitting 7 chars -- kept as a real
// runtime measurement rather than an assumption, matching this codebase's
// "measure, don't guess" convention. No shorter honest fallback exists for a
// single already-short word (unlike "edit interval"->"edit"), so the
// fallback branch is unreachable in practice but kept for the same
// defensive reason every sibling legend function keeps one.
static const char *repeat_restart_legend_label(GFont font) {
  GSize size = graphics_text_layout_get_content_size("restart", font, GRect(0, 0, 100, 16),
                                                       GTextOverflowModeFill, GTextAlignmentLeft);
  return (size.w <= LEGEND_UP_MAX_TEXT_W) ? "restart" : "rstrt";
}

static void render_stopwatch_laps_band(GContext *ctx, GRect bounds, const Timer *t, time_t now,
                                        GColor accent) {
  (void)now;  // This band is cumulative-only (no live split).
  int lap_count = t->lap_count;

  if (lap_count == 0) {
    // The empty-state hint already teaches "Up = lap" by name --
    // full-width centered, matching every layout's shared empty state.
    GRect hint_rect = GRect(0, LAPS_EMPTY_HINT_Y, bounds.size.w, 16);
    graphics_context_set_text_color(ctx, RENDER_COLOR_LABEL);
    const char *hint = store_lap_can_add(app_context_store(), t) ? "Hold Up to add a lap"
                                                               : "Lap capacity reached";
    graphics_draw_text(ctx, hint, fonts_get_system_font(FONT_KEY_GOTHIC_14),
                        hint_rect, GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
    return;
  }

  GFont label_font = fonts_get_system_font(FONT_KEY_GOTHIC_14);
  int16_t lap_content_w = (int16_t)(bounds.size.w - 2 * RENDER_MARGIN_X);

  // Last-N compact list, cumulative, newest first (matches the
  // full laps list's own cumulative reading, screen_laps.c).
  char lbl0[8];
  snprintf(lbl0, sizeof lbl0, "Lap %d", lap_count);
  char val0[16];
  int32_t lap0 = 0;
  if (!store_lap_get(app_context_store(), t, lap_count - 1, &lap0)) return;
  format_hms(val0, sizeof val0, lap0);
  GRect row0 = GRect(RENDER_MARGIN_X, LAPS_A_ROW0_Y, lap_content_w, LAPS_A_ROW_H);
  draw_kv_line(ctx, row0, lbl0, RENDER_COLOR_LABEL, label_font, val0, accent, label_font);

  if (lap_count >= 2) {
    char lbl1[8];
    snprintf(lbl1, sizeof lbl1, "Lap %d", lap_count - 1);
    char val1[16];
    int32_t lap1 = 0;
    if (!store_lap_get(app_context_store(), t, lap_count - 2, &lap1)) return;
    format_hms(val1, sizeof val1, lap1);
    GRect row1 = GRect(RENDER_MARGIN_X, LAPS_A_ROW1_Y, lap_content_w, LAPS_A_ROW_H);
    draw_kv_line(ctx, row1, lbl1, RENDER_COLOR_LABEL, label_font, val1, RENDER_COLOR_INK,
                 label_font);
  }
}

static const Timer *current_timer(void) {
  return store_find_const(app_context_store(), s_cookie);
}

// Hero name caption: a small centered caption above the digits, shown for
// all three kinds with proper ellipsis rules; unnamed timers unchanged.
// This is the one real, unconditional draw, not a debug lever choosing
// among options.
//
// Placement: a small centered caption at y=30 (16px tall) in the
// header-to-value gap -- the header band ends y=27; the RUNNING branch's
// value starts y=48, the TALL-value branches' (stopwatch/post-timer/
// overtime) at y=60 -- either way this slot sits entirely inside the
// otherwise-dead space between them. Unnamed timers (`t->name[0] == '\0'`)
// hit the early return below -- zero layout shift, byte-for-byte the same
// draw as before this feature existed.
//
// One documented exception: a PAUSED stopwatch draws its own "Paused" chip
// at PAUSED_CHIP_Y=34..52 (draw_paused_chip, above) -- rather than spend a
// second layout slot for this one narrow combination (named AND paused AND
// stopwatch-kind), the caption is simply skipped there; the chip alone
// still communicates "paused."
static void draw_hero_name_caption(GContext *ctx, GRect bounds, const Timer *t,
                                    ViewHeroBranch branch) {
  if (t->name[0] == '\0') return;
  if (branch == VIEW_HERO_STOPWATCH && t->is_paused) return;
  GRect cap_rect = GRect(0, 30, bounds.size.w, 16);
  graphics_context_set_text_color(ctx, RENDER_COLOR_LABEL);
  // TrailingEllipsis at the full 200px content width: TIMER_NAME_MAX (24,
  // already input-capped by timer_name_copy()) comfortably fits at GOTHIC_14
  // in practice, so this ellipsis is a defensive backstop, not the expected
  // common case.
  graphics_draw_text(ctx, t->name, fonts_get_system_font(FONT_KEY_GOTHIC_14), cap_rect,
                      GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
}

static void root_layer_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, COLOR_PAPER);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  const Timer *t = current_timer();
  if (t == NULL) {
    // Defensive -- nav's dead-ref guard should prevent this. Still draw the
    // header's clock (via a NULL-suffix hero header would need a kind, which
    // we don't have here) -- fall back to the plain clock, matching every
    // other defensive-empty branch in this codebase.
    render_clock(ctx, bounds, clock_is_24h_style());
    return;
  }

  time_t now = time(NULL);
  bool ringing = app_state_is_ringing(app_context_app_state(), t->cookie);
  ViewHeroBranch branch = view_hero_branch(t, ringing);
  ViewColorToken color_token = view_kind_color_token(t->kind, branch);
  // This whole hero screen sits on COLOR_PAPER (light background) -- per the
  // background-keyed tier rule, EVERY kind-colored element drawn directly on
  // it (header glyph, draining/segmented bar, the stopwatch-laps-band's
  // newest-lap value, a repeat hero's meta-line accent, the big value text)
  // now shares the SAME `value_color` (DEEP tier) below, instead of the
  // header glyph/bars staying at the bright `color` while only the value
  // darkened -- that per-element split read as a same-kind color mismatch
  // on-device: the header glyph and the value text ended up in visibly
  // different shades of the same kind color. `color`
  // (BRIGHT tier) survives ONLY for the hold-footer's own hold-fill bar,
  // which sits on a genuinely dark background (the footer's own black
  // pill, render_hold_footer's GColorBlack fill) -- the one place on this
  // screen where the bright tier is still the CORRECT (safe-on-black)
  // choice, not a leftover.
  GColor color = render_kind_color(color_token);
  // The big hero VALUE text is darkened via render_kind_color_deep() --
  // countdown/stopwatch's identity color reads too light for text at this
  // size on the real reflective panel (same symmetric-WCAG-ratio reasoning
  // already applied to the strip background case).
  GColor value_color = render_kind_color_deep(color_token);
  ViewPrefixKind prefix = view_value_prefix(branch);
  bool is_24h = clock_is_24h_style();

  // Hero header band -- kind glyph + generic kind label + clock, every
  // branch. Overtime gets the "- over" suffix (windsor tan). Leading space
  // added -- render_hero_header draws the suffix immediately after the
  // label with no gap of its own, so "Countdown" + "- over" ran together as
  // "Countdown- over".
  const char *suffix = (branch == VIEW_HERO_OVERTIME) ? " - over" : NULL;
  render_hero_header(ctx, bounds, t->kind, value_color, suffix, RENDER_COLOR_ACCENT, is_24h);

  char value[16];
  switch (branch) {
    case VIEW_HERO_STOPWATCH: {
      GRect value_rect = VALUE_RECT_TALL;
      // view_value_prefix(STOPWATCH) now answers VIEW_PREFIX_NONE -- the
      // "+" this branch briefly carried is removed, bare "4:31" not
      // "+4:31". Still routed through format_hms_prefixed/`prefix` (not
      // format_hms directly) so this call site keeps tracking lib/view's
      // seam rather than hardcoding the answer twice.
      format_hms_prefixed(value, sizeof value, timer_elapsed_s(t, now), prefix == VIEW_PREFIX_PLUS);
      // The combined, shipped paused rendering -- frozen value drawn muted
      // instead of the kind accent, a "not running" tell
      // independent of the footer label. The non-paused case uses the
      // darkened value_color (function-scope, above) instead of the plain
      // identity color.
      // This is the TEXT half of the RENDER_COLOR_MUTED tier -- darkened to
      // RENDER_COLOR_MUTED_TEXT (black), render.h's own doc comment.
      GColor stopwatch_value_color = t->is_paused ? RENDER_COLOR_MUTED_TEXT : value_color;
      // 10h shrink-to-fit: fall back one numeral tier if the value string
      // is too wide for this rect at the preferred 56px raster.
      GFont sw_font = render_fit_numeral_font(value, value_rect, fonts_adapter_numeral_56(),
                                               fonts_adapter_numeral_42());
      render_hero_value(ctx, value_rect, value, VIEW_PREFIX_NONE, stopwatch_value_color, sw_font);
      // A small "Paused" chip in the header-to-value gap.
      if (t->is_paused) {
        draw_paused_chip(ctx, bounds);
      }

      // A plain stopwatch (SET 0:00) gets a "since HH:MM" wall-clock-start
      // meta line -- it previously had no meta line at all, leaving no
      // context for when it started or how it's controlled.
      // The post-timer variant's own "from a X timer" + overrun-chip meta
      // lives in its own branch below now (see VIEW_HERO_POST_TIMER). While
      // paused, this becomes a LIVE "paused M:SS" counter (pause-elapsed =
      // now - t->paused_at) instead -- the meta line keeps ticking, just
      // measuring a different thing, rather than going stale-looking.
      char meta[40];
      if (t->is_paused) {
        int32_t paused_s = (int32_t)(now - t->paused_at);
        if (paused_s < 0) paused_s = 0;
        char paused_val[16];
        format_hms(paused_val, sizeof paused_val, paused_s);
        snprintf(meta, sizeof meta, "paused %s", paused_val);
      } else {
        struct tm *start_tm = localtime(&t->started_at);
        char clock_str[8];
        if (start_tm != NULL) {
          format_clock(clock_str, sizeof clock_str, start_tm->tm_hour, start_tm->tm_min,
                       clock_is_24h_style());
        } else {
          clock_str[0] = '\0';
        }
        snprintf(meta, sizeof meta, "since %s", clock_str);
      }
      GRect meta_rect = GRect(0, META_Y, bounds.size.w, 20);
      graphics_context_set_text_color(ctx, RENDER_COLOR_LABEL);
      graphics_draw_text(ctx, meta, fonts_get_system_font(FONT_KEY_GOTHIC_14), meta_rect,
                          GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

      // Laps-on-main, with the hold-footer control
      // model: Select-hold = Pause/Resume, Up-hold = Add lap, both via
      // hold_bind_button (see click_config_provider below); footer label
      // picks between the resting/paused label, the actively-held label,
      // and the transient short-Up-press hint.
      render_stopwatch_laps_band(ctx, bounds, t, now, value_color);
      ButtonId held_button = hold_active_button();
      const char *label;
      if (held_button == BUTTON_ID_UP) {
        label = "Hold for lap";
      } else if (held_button == BUTTON_ID_SELECT) {
        label = t->is_paused ? "Hold to resume" : "Hold to pause";
      } else if (s_up_hint_active) {
        // Short (non-hold) Up-press hint -- teaches that Up is a HOLD
        // gesture for ~UP_HINT_MS, then reverts (sw_up_short_hint below).
        label = "Hold to add lap";
      } else {
        label = t->is_paused ? "Hold to resume" : "Hold to pause";
      }
      // Footer-legend consolidation: the pill's right slot now carries the
      // stopwatch's own Up-hold hint ("new lap"/"+lap" fallback) stacked
      // over "more" -- replacing the external legend column
      // render_stopwatch_laps_band used to draw.
      //
      // At lap-pool exhaustion the body already says "Lap capacity reached"
      // (render_stopwatch_laps_band's empty-state branch above) -- the
      // pill's own hint must agree
      // rather than keep advertising "new lap" for a gesture that will just
      // reject-vibe. Matches the same gated-hint idiom the RUNNING
      // countdown's add-1/4 hint already uses (up_add_quarter/up_label ==
      // NULL below): hide the hint row entirely (render_hold_footer falls
      // back to its single centered "more" line) rather than invent a
      // second right-slot string.
      render_hold_footer(ctx, bounds, label, color, hold_is_active(), hold_progress(), false,
                          store_lap_can_add(app_context_store(), t)
                              ? up_legend_label(fonts_get_system_font(FONT_KEY_GOTHIC_14))
                              : NULL);
      break;
    }
    case VIEW_HERO_POST_TIMER: {
      // A post-timer stopwatch is now its OWN branch (lib/view's
      // ViewHeroBranch split) -- its primary action is Pause, single-press,
      // not Lap (the app had reused the plain-stopwatch hero wholesale).
      // See
      // post_timer_pause_click_handler/click_config_provider below for the
      // button-map half of this fix.
      GRect value_rect = VALUE_RECT_TALL;
      format_hms(value, sizeof value, timer_elapsed_s(t, now));
      // Keeps the 42px raster here (not the new 56px hero tier) -- this
      // hero's meta row already carries a two-element label+chip layout
      // that needs the room; the 48px/52px tiers don't leave enough space
      // for it, 42px does, same reasoning as the alarm hero.
      // 10h shrink-to-fit: fall back to the 32px tier if even 42px
      // overflows (a keep-counting stopwatch can run for 10h+ same as any
      // other stopwatch).
      GFont pt_font = render_fit_numeral_font(value, value_rect, fonts_adapter_numeral_42(),
                                               fonts_adapter_numeral_32());
      render_hero_value(ctx, value_rect, value, VIEW_PREFIX_NONE, value_color, pt_font);

      // "from a X timer" provenance label + the small overrun chip
      // ("+0:38") next to it -- how far past the original countdown this
      // keepCounting stopwatch has run. Left-aligned-as-a-group (measure
      // both pieces, center the pair), with a 7px gap between label and
      // chip.
      char meta[32];
      char orig[16];
      format_hms(orig, sizeof orig, t->original_total_s);
      snprintf(meta, sizeof meta, "from a %s timer", orig);

      GFont meta_font = fonts_get_system_font(FONT_KEY_GOTHIC_14);
      GSize label_size = graphics_text_layout_get_content_size(
          meta, meta_font, GRect(0, 0, bounds.size.w, 20), GTextOverflowModeFill,
          GTextAlignmentLeft);
      char chip_value[16];
      format_hms(chip_value, sizeof chip_value, timer_overrun_s(t, now));
      GSize chip_size = render_overrun_chip_size(chip_value);
      const int16_t gap = 7;
      int16_t total_w = (int16_t)(label_size.w + gap + chip_size.w);
      int16_t start_x = (int16_t)((bounds.size.w - total_w) / 2);
      if (start_x < 0) start_x = 0;

      GRect label_rect = GRect(start_x, META_Y, (int16_t)(label_size.w + 2), 20);
      graphics_context_set_text_color(ctx, RENDER_COLOR_LABEL);
      graphics_draw_text(ctx, meta, meta_font, label_rect, GTextOverflowModeFill,
                          GTextAlignmentLeft, NULL);
      render_overrun_chip(ctx, GPoint((int16_t)(start_x + label_size.w + gap), META_Y), chip_value,
                           RENDER_COLOR_OVERRUN_CHIP_BG, RENDER_COLOR_OVERRUN_CHIP_TEXT);

      // Single-press Pause/Resume hfoot -- Select directly IS the primary
      // action here, so (unlike the plain stopwatch above) no separate
      // "Down: Pause" reachability hint is needed.
      render_hfoot(ctx, bounds, t->is_paused ? "Resume" : "Pause", false);
      break;
    }
    case VIEW_HERO_OVERTIME: {
      GRect value_rect = VALUE_RECT_TALL;
      format_hms_prefixed(value, sizeof value, timer_overrun_s(t, now), prefix == VIEW_PREFIX_PLUS);
      // The running/overtime/stopwatch heroes' own 56px tier.
      // 10h shrink-to-fit: a countdown can overrun past 10h too.
      GFont ot_font = render_fit_numeral_font(value, value_rect, fonts_adapter_numeral_56(),
                                               fonts_adapter_numeral_42());
      render_hero_value(ctx, value_rect, value, VIEW_PREFIX_NONE, value_color, ot_font);

      // "<target> target - overrunning" sub-meta -- the ORIGINAL countdown
      // duration (duration_s), not the overrun value already shown as the
      // big number above. Windsor-tan, y=120.
      char target[16];
      format_hms(target, sizeof target, t->duration_s);
      char meta[40];  // sized to the worst case GCC's -Wformat-truncation computes from target[16]
      snprintf(meta, sizeof meta, "%s target - overrunning", target);
      GRect meta_rect = GRect(0, META_Y, bounds.size.w, 20);
      graphics_context_set_text_color(ctx, RENDER_COLOR_ACCENT);
      graphics_draw_text(ctx, meta, fonts_get_system_font(FONT_KEY_GOTHIC_14), meta_rect,
                          GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

      // Overtime Done is a SINGLE PRESS (.hfoot), diverging from the
      // alarm-family Done's hold (anti-fumble -- Done here only stops
      // OVERTIME counting-up, not a still-ringing alarm, so the anti-fumble
      // hold isn't warranted). The alarm-family Done pill already carries a
      // check glyph (render_hold_footer's show_check); this single-press
      // hfoot variant didn't, until now.
      render_hfoot(ctx, bounds, "Done", true);
      break;
    }
    case VIEW_HERO_ALARM:
      // Defensive fallback only -- nav routes fired&&ringing timers to
      // screen_alarm.c, not here. Render identically to OVERTIME rather than
      // showing nothing if this is ever reached.
      format_hms(value, sizeof value, timer_overrun_s(t, now));
      render_hero_value(ctx, VALUE_RECT_TALL, value, VIEW_PREFIX_CARET, value_color,
                         fonts_adapter_numeral_42());
      break;
    case VIEW_HERO_RUNNING:
    default: {
      GRect value_rect = VALUE_RECT_RUNNING;
      format_hms(value, sizeof value, timer_remaining_s(t, now) < 0 ? 0 : timer_remaining_s(t, now));
      // The running/overtime/stopwatch heroes' own 56px tier.
      // 10h shrink-to-fit (a real 10:00:00 countdown clipped to "10:0..."
      // at this exact site): fall back to the 42px tier when the formatted
      // duration is too wide for VALUE_RECT_RUNNING at 56px.
      GFont value_font = render_fit_numeral_font(value, value_rect, fonts_adapter_numeral_56(),
                                                  fonts_adapter_numeral_42());
      render_hero_value(ctx, value_rect, value, VIEW_PREFIX_NONE, value_color, value_font);
      if (t->kind == KIND_REPEAT) {
        // REPEAT hero -- a segmented bar instead of the countdown's
        // continuous draining bar, plus the "every <interval> - done
        // x<cycles> - next <clock>" meta line (rendered here as plain ASCII
        // "-"/"x", matching this codebase's existing avoid-non-ASCII-glyph
        // convention, see screen_home.c's vector-drawn "+" for the same
        // reasoning). `timer_deadline` is only valid while RUNNING (its own
        // header doc) -- this branch also covers the PAUSED sub-state
        // (view_hero_branch doesn't split on is_paused), so "next" is
        // guarded and swapped for "paused" rather than showing a stale
        // deadline (same is_paused guard screen_home.c's own paused-state
        // indicator uses for the same reason). Track color is now
        // RENDER_COLOR_ROW_HAIRLINE (GColorLightGray), not the invisible
        // white 0xCFC9DC this call used to pass. Completed-cycles fill
        // semantics -- see render_segmented_bar's doc (render.h).
        // Percent (0-100) -> per-mille (0-1000): exact, `* 10` (was
        // `/ 100.0f`).
        render_segmented_bar(ctx, BAR_RECT, (int)t->cycles, timer_progress_pct(t, now) * 10,
                              value_color, RENDER_COLOR_ROW_HAIRLINE);
        char interval[16];
        format_hms(interval, sizeof interval, t->duration_s);
        char meta[48];
        // A fourth meta-line variant, parallel to the shipped paused one --
        // checked before is_paused since a snoozed repeat is running, not
        // paused (timer_snooze_repeat leaves is_paused=false), so it would
        // otherwise fall into the "next HH:MM" branch below.
        if (timer_is_snoozing(t)) {
          snprintf(meta, sizeof meta, "every %s - done x%d - snoozing", interval, (int)t->cycles);
        } else if (t->is_paused) {
          snprintf(meta, sizeof meta, "every %s - done x%d - paused", interval, (int)t->cycles);
        } else {
          time_t deadline = timer_deadline(t);
          struct tm *tm_next = localtime(&deadline);
          char next_clock[8];
          if (tm_next != NULL) {
            format_clock(next_clock, sizeof next_clock, tm_next->tm_hour, tm_next->tm_min,
                         clock_is_24h_style());
          } else {
            next_clock[0] = '\0';
          }
          snprintf(meta, sizeof meta, "every %s - done x%d - next %s", interval, (int)t->cycles,
                   next_clock);
        }
        // Neutral-meta rule: kind color is reserved for IDENTITY elements
        // only (the big digits, the segmented bar, the header's kind
        // label/glyph). Every meta/detail line, including this one, uses the
        // same neutral RENDER_COLOR_LABEL ink the countdown branch's own
        // start/end row uses below (render_start_end_labels), never a
        // kind-derived color. RENDER_COLOR_ACCENT's own uses elsewhere (the
        // "- over" header suffix, the OVERTIME "target - overrunning" meta,
        // the alarm hero's reference line) are a fixed, kind-independent
        // accent, not a value_color/kind_color leak, so they stay outside
        // this rule's scope.
        GRect meta_rect = GRect(0, META_Y, bounds.size.w, 20);
        graphics_context_set_text_color(ctx, RENDER_COLOR_LABEL);
        graphics_draw_text(ctx, meta, fonts_get_system_font(FONT_KEY_GOTHIC_14), meta_rect,
                            GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
      } else {
        // Draining bar: fraction of duration REMAINING (starts full,
        // drains to 0). Track is now RENDER_COLOR_ROW_HAIRLINE
        // (GColorLightGray), not the invisible white 0xD8D3C8 this call
        // used to pass. Start/end clock row underneath.
        // The hero draining bar keeps radius 3 (row/hold-footer bars use 2
        // -- see render.c's other render_bar call sites).
        // Remaining fraction as per-mille -- exact, `1000 - pct*10` (was
        // `1.0f - pct/100.0f`).
        render_bar(ctx, BAR_RECT, RENDER_FRACTION_SCALE - timer_progress_pct(t, now) * 10,
                   value_color, RENDER_COLOR_ROW_HAIRLINE, 3);
        render_start_end_labels(ctx, bounds, t->started_at, timer_deadline(t), clock_is_24h_style());
      }
      // The noBgAlarm indicator: a muted-grey "won't fire in the
      // background" mark if this timer is in the scheduler's unschedulable
      // set. Recomputed on every redraw rather than cached -- MAX_TIMERS=16
      // keeps sched_plan() cheap enough for a button-driven, low-refresh-rate
      // UI. A plain filled dot here left blind users unable to tell what it
      // meant -- render_no_bg_alarm_indicator's prohibition-sign shape (open
      // circle + slash) is minimally self-explanatory without any new
      // text/naming.
      if (!t->is_paused) {
        SchedPlan plan;
        sched_plan(app_context_store(), now, &plan);
        if (view_no_bg_alarm(t->cookie, &plan)) {
          render_no_bg_alarm_indicator(ctx, GPoint(bounds.size.w - 15, value_rect.origin.y + 7), 5,
                                        RENDER_COLOR_MUTED);
        }
      }
      // The hold-footer: bottom-anchored selected pill + hold-fill bar,
      // replacing the old centered "Hold: X" text + mid-screen bar overlay.
      // This RUNNING hero's own Up-hold -- countdown gets "add 1/4 of
      // duration" (gated: only when timer_add_quarter_available, i.e. 1/4 >
      // 10s -- below the gate there is no gesture and no hint at all, both
      // hidden together); repeat gets "Restart" (ungated -- see
      // click_config_provider below for the real bindings; supersedes the
      // original "edit interval" gesture, see rp_restart's own comment).
      // Footer-legend consolidation: `up_label` (the pill's stacked
      // right-slot hint) is populated whenever the gesture is available,
      // independent of whether it's actually being held right now; the
      // LEFT `label` additionally switches to the hold-text-explains string
      // (a live "+M:SS -> total" preview for countdown, "Restart" for
      // repeat) only while Up is the button actually being held
      // (hold_active_button()) -- release before HOLD_LONG_MS/HOLD_MS
      // cancels, native to hold_bind_button.
      // Locked product decision: the quantity is 1/4 of the timer's
      // ORIGINAL SET amount, via the shared timer_quarter_amount() helper --
      // see lib/timer.h's doc. Constant per hold (20:00 -> +5:00 -> 25:00 ->
      // +5:00 -> 30:00), never a compounding 1/4-of-current.
      bool quarter_available = (t->kind == KIND_COUNTDOWN) && timer_add_quarter_available(t);
      char qstr[16] = {0};
      int32_t quarter_s = 0;
      if (quarter_available) {
        quarter_s = timer_quarter_amount(t);
        format_hms(qstr, sizeof qstr, quarter_s);
      }
      char up_label_buf[24];
      const char *up_label = NULL;
      if (quarter_available) {
        snprintf(up_label_buf, sizeof up_label_buf, "+%s", qstr);
        up_label = up_label_buf;
      } else if (t->kind == KIND_REPEAT) {
        up_label = repeat_restart_legend_label(fonts_get_system_font(FONT_KEY_GOTHIC_14));
      }
      ButtonId held_button = hold_active_button();
      // Sized to the worst case GCC's -Wformat-truncation computes from two
      // format_hms()-sized[16] operands (same reasoning as this file's
      // VIEW_HERO_OVERTIME meta[40] buffer above).
      char label_buf[40];
      const char *label;
      if (held_button == BUTTON_ID_UP && quarter_available) {
        // Saturate the PREVIEW at the same cap timer_add_quarter() itself
        // enforces -- the preview must never promise a total the real
        // mutation won't actually reach. Also never let the preview show a
        // SMALLER total than the current duration -- mirrors the same
        // never-shrink guard the real mutation now has, so a timer dialed
        // above the cap can never show a "+M:SS -> total" preview that
        // contradicts what's already on screen.
        int64_t total_s = (int64_t)t->duration_s + (int64_t)quarter_s;
        if (total_s > TIMER_MAX_DURATION_S) total_s = TIMER_MAX_DURATION_S;
        if (total_s < t->duration_s) total_s = t->duration_s;
        char total[16];
        format_hms(total, sizeof total, (int32_t)total_s);
        snprintf(label_buf, sizeof label_buf, "+%s -> %s", qstr, total);
        label = label_buf;
      } else if (held_button == BUTTON_ID_UP && t->kind == KIND_REPEAT) {
        label = "Restart";
      } else {
        label = t->is_paused ? "Hold to resume" : "Hold to pause";
      }
      render_hold_footer(ctx, bounds, label, color, hold_is_active(), hold_progress(), false,
                          up_label);
      break;
    }
  }
  draw_hero_name_caption(ctx, bounds, t, branch);
}

static void persist_and_replan(void) {
  TimerStore *store = app_context_store();
  persist_adapter_save(store);
  wakeup_adapter_replan(store, time(NULL));
}

static void hero_hold_fire(void) {
  Timer *t = store_find(app_context_store(), s_cookie);
  if (t == NULL) return;
  time_t now = time(NULL);
  bool ringing = app_state_is_ringing(app_context_app_state(), t->cookie);
  ViewHeroBranch branch = view_hero_branch(t, ringing);

  switch (branch) {
    case VIEW_HERO_RUNNING:
      if (t->is_paused) {
        timer_resume(t, now);
      } else {
        timer_pause(t, now);
      }
      persist_and_replan();
      break;
    case VIEW_HERO_OVERTIME:
    case VIEW_HERO_ALARM: {
      int32_t cookie = t->cookie;
      // Test-log marker: distinguishes the OVERTIME single-click path from
      // the defensive ALARM hold path (unreachable in practice -- nav
      // routes fired&&ringing timers to screen_alarm.c's own hero instead),
      // same "testlog at the exact mutation" pattern as screen_alarm.c's
      // do_advance_cycle/resolve_after_keep_counting markers.
      testlog("hero_done", branch == VIEW_HERO_OVERTIME ? "overtime" : "alarm");
      store_remove(app_context_store(), cookie);
      persist_and_replan();
      nav_go_home();
      return;
    }
    case VIEW_HERO_STOPWATCH:
    case VIEW_HERO_POST_TIMER:
      // Unreachable in practice -- click_config_provider below binds
      // sw_pause_resume/sw_add_lap (via hold_bind_button) for STOPWATCH and
      // post_timer_pause_click_handler for POST_TIMER instead
      // of hold_bind_select for either stopwatch-kind branch, so this
      // hold-fire callback is never invoked while viewing one. Kept as
      // explicit no-op cases (not folded into a `default`) purely so the
      // switch stays exhaustive under -Wswitch.
      break;
  }
  if (s_root_layer != NULL) layer_mark_dirty(s_root_layer);
}

// The new stopwatch control model's Select-hold action -- Pause/Resume,
// bound via hold_bind_button (replacing the old single-press "Lap on
// Select" -- see click_config_provider below).
static void sw_pause_resume(void) {
  Timer *t = store_find(app_context_store(), s_cookie);
  if (t == NULL || t->kind != KIND_STOPWATCH || t->original_total_s > 0) return;
  time_t now = time(NULL);
  if (t->is_paused) {
    timer_resume(t, now);
  } else {
    timer_pause(t, now);
  }
  persist_and_replan();
  testlog("sw_pause", t->is_paused ? "paused" : "resumed");
  if (s_root_layer != NULL) layer_mark_dirty(s_root_layer);
}

// The new stopwatch control model's Up-hold action -- Add lap (replacing
// STOPWATCH_ROW_LAP's old Actions-menu entry point; the hero itself is now
// the only place to add a lap). Same mutation/testlog body the old
// single-press Select handler had.
static void sw_add_lap(void) {
  Timer *t = store_find(app_context_store(), s_cookie);
  if (t == NULL || t->kind != KIND_STOPWATCH || t->original_total_s > 0) return;
  if (!store_add_lap(app_context_store(), t, time(NULL))) {
    vibes_adapter_reject();
    testlog("lap_refused", "pool");
    return;
  }
  persist_and_replan();
  char data[8];
  snprintf(data, sizeof data, "%d", t->lap_count);
  testlog("lap", data);
  if (s_root_layer != NULL) layer_mark_dirty(s_root_layer);
}

// A bare Up PRESS (released before HOLD_MS -- hold_bind_button's
// `on_short`) arms a transient ~2s footer-label swap teaching that Up is a
// HOLD gesture, then reverts. Pure adapter-tier AppTimer + a module-static
// flag the update_proc reads
// (s_up_hint_active, declared up top with s_cookie) -- mirrors this app's
// existing "adapter owns its own transient-UI timers" shape (e.g. hold.c's
// own fill timer), nothing here touches lib/.
#define UP_HINT_MS 2000

// Unlike hold.c's channel (structurally reset by hold_cancel() from
// nav.c's render_stack()) and the stopwatch control model's own
// carry-scroll AppTimer (screen_actions_cancel_carry_scroll(), the same
// structural treatment), this transient-hint pair had NO cancellation hook
// at all -- the single shared hero Window is reused across every timer the
// user views, so a bare Up-tap on stopwatch A left `s_up_hint_active`/
// `s_up_hint_timer` armed, and switching to a DIFFERENT stopwatch B within
// the ~2s window showed B a "Hold to add lap" hint it never earned (self-
// corrected once the orphaned timer fired). Fixed by cancelling here,
// called from screen_hero_set_cookie() below -- the same choke point every
// real navigation INTO a hero already funnels through
// (nav.c's apply_cookie(), itself only reached via render_stack()'s
// diffed push list), so this now resets on exactly "a hero is about to show
// a (possibly different) timer," mirroring hold_cancel()'s own "never
// survive a transition" discipline for this file's one remaining unguarded
// transient state.
static void cancel_up_hint(void) {
  if (s_up_hint_timer != NULL) {
    app_timer_cancel(s_up_hint_timer);
    s_up_hint_timer = NULL;
  }
  s_up_hint_active = false;
}

static void up_hint_timer_cb(void *data) {
  (void)data;
  s_up_hint_timer = NULL;
  s_up_hint_active = false;
  if (s_root_layer != NULL) layer_mark_dirty(s_root_layer);
}

static void sw_up_short_hint(void) {
  s_up_hint_active = true;
  if (s_up_hint_timer != NULL) {
    app_timer_cancel(s_up_hint_timer);
  }
  s_up_hint_timer = app_timer_register(UP_HINT_MS, up_hint_timer_cb, NULL);
  if (s_root_layer != NULL) layer_mark_dirty(s_root_layer);
}

#ifdef GALLERY_MODE
void screen_hero_debug_force_up_hint(bool active) {
  s_up_hint_active = active;
  if (s_root_layer != NULL) layer_mark_dirty(s_root_layer);
}
#endif

// The post-timer hero's primary action, SINGLE-PRESS (not hold, matching
// its hfoot footer style) -- toggles pause/resume and stays on the hero,
// same shape as the plain
// stopwatch's Lap handler above. Bound in place of hold_bind_select
// whenever the current timer is a post-timer stopwatch (original_total_s >
// 0) -- see click_config_provider below.
static void post_timer_pause_click_handler(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
  Timer *t = store_find(app_context_store(), s_cookie);
  if (t == NULL || t->kind != KIND_STOPWATCH || t->original_total_s <= 0) return;
  time_t now = time(NULL);
  if (t->is_paused) {
    timer_resume(t, now);
  } else {
    timer_pause(t, now);
  }
  persist_and_replan();
  testlog("post_timer_pause", t->is_paused ? "paused" : "resumed");
  if (s_root_layer != NULL) layer_mark_dirty(s_root_layer);
}

static void down_click_handler(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
  // Held-Down carry-through: arm the one-shot flag BEFORE nav_go_actions --
  // ACTIONS' own window_appear runs SYNCHRONOUSLY inside this same press
  // dispatch, so by the time this handler returns, ACTIONS has already
  // consumed the flag and (if the physical press is still held) started
  // its auto-scroll AppTimer.
  screen_actions_arm_carry_scroll();
  nav_go_actions(s_cookie);
}

static void back_click_handler(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
  // Nav fix: remembers this hero's own cookie so HOME's window_appear can
  // land the cursor back on its row instead of always resetting to row 0.
  nav_go_home_from_hero(s_cookie);
}

// Up has no documented function on any hero, so this is a safe explicit
// no-op subscription (leaving a button with NO click subscription at all
// is its own, separate, deterministic bug -- see click_config_provider
// below).
static void up_click_handler(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
}

// Two distinct bugs are involved here:
//  1. Up had ZERO click subscription of any kind. That alone is enough to
//     crash: pressing Up once on a freshly-created hero, with nothing else
//     going on, triggers the "Click subscribe must be set from click config
//     provider" ASSERT. Fixed by the explicit no-op subscription above --
//     every button a window's click config provider is responsible for must
//     be subscribed, even to a no-op.
//  2. A separate, deeper issue remains NOT FULLY RESOLVED: the same ASSERT
//     can resurface, probabilistically, in sessions with enough cumulative
//     button presses/window transitions (the exact count varies run to
//     run -- not a clean threshold). Left as a documented residual risk,
//     not silently dropped.

// OVERTIME's Done is a SINGLE PRESS -- a hold pill+fill would be wrong here
// since Done only stops OVERTIME counting-up, not a still-ringing alarm --
// and render_hfoot already draws it that way. But click_config_provider
// (below) still bound Select via hold_bind_select for every non-stopwatch
// branch, so a short tap on OVERTIME's Done did nothing (matched a 700ms
// hold in practice, contradicting its own single-press visual). This
// handler reuses hero_hold_fire's OVERTIME/ALARM body directly (it
// recomputes the branch fresh from `s_cookie` each call, so there's no
// duplicated resolve logic here).
static void overtime_select_click_handler(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
  hero_hold_fire();
}

// The countdown RUNNING hero's Up-hold -- adds 1/4 of the CURRENT duration
// and re-arms (timer_add_quarter). Bound
// (click_config_provider below) only when timer_add_quarter_available()
// gates it in, but re-checked here too, matching sw_add_lap/sw_pause_resume's
// own "don't trust the binding alone" defensiveness above.
static void cd_add_quarter(void) {
  Timer *t = store_find(app_context_store(), s_cookie);
  if (t == NULL || t->kind != KIND_COUNTDOWN || !timer_add_quarter_available(t)) return;
  timer_add_quarter(t, time(NULL));
  persist_and_replan();
  char data[16];
  snprintf(data, sizeof data, "%ld", (long)t->duration_s);
  testlog("add_quarter", data);
  if (s_root_layer != NULL) layer_mark_dirty(s_root_layer);
}

// Supersedes rp_edit_interval -- the repeat RUNNING hero's Up-hold is now
// Restart, a same-screen commit mutation (standard hold-to-apply w/ fill,
// HOLD_MS, matching the lap/pause convention), not a navigation to SET.
// Edit-interval stays reachable, menu-only, via ACTIONS' own "Edit
// interval" row (screen_actions.c's REPEAT_ROW_EDIT_INTERVAL,
// nav_go_edit_interval) -- this scope change is RUNNING-only (a
// fired/ringing repeat's own Up-hold stays snooze-1/4, unchanged,
// screen_alarm.c). Reuses timer_restart verbatim (lib/timer.h's doc):
// cycles=0 ("begin fresh"), distinct from "Keep going" (cycles+=1) -- same
// mutation ACTIONS' new REPEAT_ROW_RESTART row now also exposes
// (gesture-duplicates-menu-row is the locked, wanted convention here, not
// an accident).
static void rp_restart(void) {
  Timer *t = store_find(app_context_store(), s_cookie);
  if (t == NULL || t->kind != KIND_REPEAT) return;
  time_t now = time(NULL);
  store_restart(app_context_store(), t, now);
  persist_and_replan();
  testlog("restart", "repeat_hero");
  if (s_root_layer != NULL) layer_mark_dirty(s_root_layer);
}

static void click_config_provider(void *context) {
  (void)context;
  window_single_click_subscribe(BUTTON_ID_DOWN, down_click_handler);
  window_single_click_subscribe(BUTTON_ID_BACK, back_click_handler);

  // Select is SINGLE-PRESS Pause for a POST-TIMER stopwatch hero,
  // SINGLE-PRESS Done for OVERTIME, hold-to-apply for every other branch
  // (Pause/Resume on RUNNING; the defensive ALARM fallback keeps its hold
  // too -- nav routes fired&&ringing timers to screen_alarm.c's own hero
  // instead, which is unaffected by this branch and still hold-only). A
  // PLAIN stopwatch hero gets TWO hold-bound buttons (Select=Pause/Resume,
  // Up=Add lap) via hold_bind_button -- the app's first real two-button
  // hold consumer. `s_cookie` is already set by nav.c's apply_cookie()
  // before this window is pushed (the
  // same invariant screen_actions.c/screen_alarm.c rely on for their own
  // kind-branched row counts), so it's safe to read the timer's state here.
  const Timer *t = current_timer();
  bool ringing = t != NULL && app_state_is_ringing(app_context_app_state(), t->cookie);
  ViewHeroBranch branch = (t != NULL) ? view_hero_branch(t, ringing) : VIEW_HERO_RUNNING;
  bool plain_stopwatch = (t != NULL && t->kind == KIND_STOPWATCH && branch != VIEW_HERO_POST_TIMER);

  // The RUNNING hero's own Up-hold -- countdown's "add 1/4" (gated: only
  // when timer_add_quarter_available, i.e. 1/4 > 10s -- below the gate the
  // gesture AND its footer hint are both simply absent, no separate
  // "disabled" state) and repeat's "Restart" (ungated -- supersedes the
  // original "edit interval" gesture here, scoped to RUNNING only; a
  // fired/ringing repeat's own Up-hold is untouched, see screen_alarm.c).
  // Computed once here so both the no-op-Up-subscription skip immediately
  // below and the real binding stay in sync.
  bool up_add_quarter =
      (t != NULL && branch == VIEW_HERO_RUNNING && t->kind == KIND_COUNTDOWN &&
       timer_add_quarter_available(t));
  bool up_restart = (t != NULL && branch == VIEW_HERO_RUNNING && t->kind == KIND_REPEAT);

  // Every button a window's click config provider is responsible for must
  // be subscribed, even to a no-op -- Up has no documented function on any
  // OTHER hero branch. A plain stopwatch and a gated-in RUNNING Up-hold are
  // the exceptions: Up is raw-click-bound below
  // (hold_bind_button), not single-click, so the no-op subscription is
  // skipped for those (single-click and raw write different ClickConfig
  // fields and would otherwise coexist harmlessly, but there is no reason to
  // bind both here).
  if (!plain_stopwatch && !up_add_quarter && !up_restart) {
    window_single_click_subscribe(BUTTON_ID_UP, up_click_handler);
  }

  if (branch == VIEW_HERO_POST_TIMER) {
    window_single_click_subscribe(BUTTON_ID_SELECT, post_timer_pause_click_handler);
  } else if (plain_stopwatch) {
    hold_bind_button(s_window, BUTTON_ID_SELECT, NULL, sw_pause_resume, HOLD_MS);
    hold_bind_button(s_window, BUTTON_ID_UP, sw_up_short_hint, sw_add_lap, HOLD_MS);
  } else if (branch == VIEW_HERO_OVERTIME) {
    window_single_click_subscribe(BUTTON_ID_SELECT, overtime_select_click_handler);
  } else {
    hold_bind_button(s_window, BUTTON_ID_SELECT, NULL, hero_hold_fire, HOLD_MS);
    if (up_add_quarter) {
      // Longer hold (HOLD_LONG_MS) than the standard HOLD_MS -- a
      // deliberately heavier commit for a duration-extending gesture
      // (HOLD_LONG_MS's own comment).
      hold_bind_button(s_window, BUTTON_ID_UP, NULL, cd_add_quarter, HOLD_LONG_MS);
    } else if (up_restart) {
      // Standard hold-to-apply threshold (HOLD_MS), not the "heavier commit"
      // HOLD_LONG_MS above -- Restart is a same-screen commit action, same
      // convention as the lap/pause holds (sw_add_lap/sw_pause_resume/
      // hero_hold_fire), not a duration-extending one.
      hold_bind_button(s_window, BUTTON_ID_UP, NULL, rp_restart, HOLD_MS);
    }
  }
}

static void window_load(Window *window) {
  s_root_layer = window_get_root_layer(window);
  layer_set_update_proc(s_root_layer, root_layer_update_proc);
}

static void window_unload(Window *window) {
  (void)window;
  s_root_layer = NULL;
}

Window *screen_hero_create(void) {
  s_window = window_create();
  window_set_background_color(s_window, COLOR_PAPER);
#ifdef GALLERY_MODE
  // Screen gallery (inner loop, src/c/gallery.c) -- see screen_set.c's
  // matching comment. Never compiled into a release build.
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

void screen_hero_destroy(void) {
  window_destroy(s_window);
  s_window = NULL;
}

void screen_hero_set_cookie(int32_t cookie) {
  // See cancel_up_hint()'s own doc comment: must not let a stale
  // short-Up-press hint from a PREVIOUS cookie's hero bleed into this
  // (possibly different) one.
  cancel_up_hint();
  s_cookie = cookie;
  if (s_root_layer != NULL) layer_mark_dirty(s_root_layer);
}

void screen_hero_mark_dirty(void) {
  if (s_root_layer != NULL) layer_mark_dirty(s_root_layer);
}
