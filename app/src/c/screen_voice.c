// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// The voice confirm/edit preview + parse-fail/dictation-error one-liner
// variants, reachable via the real VD_VOICE nav node, backed by a real
// dictation_am.c adapter for both entry and "Say again".
//
// Skeleton copied from screen_actions.c/screen_library.c (allocation
// discipline: header Layer + MenuLayer + scroll indicator, all created ONCE;
// a single non-reentrant custom click_config_provider -- NEVER
// menu_layer_set_click_config_onto_window(), see screen_actions.c's own long
// comment for the dead-Select reentrancy history this avoids). Action rows
// use the shared render_action_row rail (this app's standard action-row
// idiom), NOT the older render_menu_row rail, to match this app's shared
// design language (rails, rules, fonts, action rows, kind accents) rather
// than re-derive a one-off idiom for a brand-new screen.
//
// Notes on a few non-obvious behaviors:
//  1. The name line + "Add name"/"Rename" row are real. `Timer.name` exists
//     (lib/timer.h), so the confirm preview's held NlParse.has_name/name
//     drives both: build_rows() below inserts an "Add name" (has_name
//     false) / "Rename" (has_name true) row for every VOICE_OK outcome
//     (stopwatch included -- a stopwatch can carry a name residue too, per
//     parse_nl's own grammar), and info_update_proc() draws the name as its
//     own line above the verbatim echo when present. The row's action
//     (screen_voice_set_name(), driven by dictation_am's DICT_SCOPE_NAME
//     arc) mutates the SAME held NlParse in place -- an in-place edit of the
//     current preview, not a fresh screen_voice_show_parse() outcome (which
//     would also reset the menu selection/echo needlessly).
//  2. "Say again" calls the real `dictation_am_start(DICT_SCOPE_FULL)` -- a
//     genuine re-attempt, not a no-op. It only appears on an outcome whose
//     row set includes it (VOICE_OK/VOICE_PARSE_FAIL/VOICE_NO_SPEECH/
//     VOICE_ERROR). A cancelled dictation (VOICE_CANCELLED) never shows a
//     screen_voice content state at all -- it navigates straight home
//     instead (see dictation_am.c), and the same is true if an alarm
//     interrupts a pending dictation before it resolves: resolution goes
//     straight home rather than resuming back onto this screen.
//  3. "Edit on dial" calls the PRIMED
//     `nav_go_set_from_voice(s_parse.duration_s, s_parse.kind==KIND_REPEAT)`
//     (screen_set.c's `screen_set_start_from_voice`, reusing
//     `set_machine_init_from_seconds_kind`). "Set on dial"/"Use dial" (the
//     parse-fail/dictation-error escapes) deliberately stay on the plain,
//     unprimed `nav_go_set()` -- there is no parsed duration/kind to prime
//     FROM in either failure state (a "graceful floor" for those cases).
#include "screen_voice.h"

#include <stdio.h>

#include "app_context.h"
#include "dictation_am.h"
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

// Same band height for BOTH the identity strip (parse succeeded, a real kind
// to color) and the neutral message band (parse/dictation failure, no kind)
// -- the layout skeleton below (info region, action rows) never shifts
// between content states. Taller than RENDER_STRIP_H's plain 30px -- the
// longest dictation-error message, "Turn on voice in the Pebble app",
// ellipsizes to "...Pebble ..." at 30px/one line -- so the message band gets
// room to wrap to 2 lines;
// render_floating_strip still only ever paints its own fixed RENDER_STRIP_H
// (30px) sub-band regardless of the taller Layer it's given (see its own
// doc comment), so the identity-strip state explicitly papers the remainder
// itself (see header_update_proc) rather than leaving stale pixels.
#define VOICE_HEADER_H 44
#define VOICE_INFO_H 40
// Matches dictation_am's buffer_size (dictation_am.{c,h} doc: "sized for the
// longest expected phrase, e.g. 64 bytes") -- kept in sync so a real
// transcript this app will ever actually receive always fits without
// truncation surprises here.
#define VOICE_TRANSCRIPT_MAX 64

#define SCROLL_INDICATOR_WIDTH 3
#define SCROLL_INDICATOR_MARGIN 3

// The largest row count any outcome ever produces: VOICE_OK non-stopwatch
// (Start / Add name|Rename / Say again / Edit on dial) -- 4, exactly. The
// Add-name/Rename row landed into the one spare slot this constant was
// already sized for; no resize needed.
#define VOICE_MAX_ROWS 4

static Window *s_window;
static Layer *s_header;
static Layer *s_info;
static MenuLayer *s_menu;
static Layer *s_scroll_indicator;

static NlParse s_parse;
static VoiceOutcome s_outcome = VOICE_OK;
static char s_transcript[VOICE_TRANSCRIPT_MAX];
// Voice-into-Library: VOICE_TARGET_TIMER is the safe default (matches every
// pre-existing nav_go_voice() caller, and this static's zero-init) --
// nav_go_voice()/nav_go_voice_preset() both set it explicitly before every
// real session regardless, per screen_voice_set_target's own contract, so
// this default is never actually relied on in practice, only a defensive
// baseline.
static VoiceTarget s_target = VOICE_TARGET_TIMER;

void screen_voice_set_target(VoiceTarget target) {
  s_target = target;
}

#ifdef GALLERY_MODE
// v0.1.11 gallery-only lever: forces the clock-target annotation's am/pm
// device-style read, mirroring hold_debug_force()'s/screen_hero_debug_force_
// up_hint()'s own "force the visual state deterministically" shape -- a real
// device's clock_is_24h_style() is a genuine SDK read of a system setting
// this app has no in-app toggle for, so a 12h-style CT scene needs this
// lever to be reproducible on any device the gallery build happens to run
// on. -1 = not forced (fall through to the real clock_is_24h_style()); reset
// by gallery.c's gallery_reset_fixture() like every other forcing lever, so
// it never leaks into a scene that never armed it itself. Never compiled
// into a release build.
static int s_debug_forced_is_24h = -1;

void screen_voice_debug_force_24h_style(int is_24h) {
  s_debug_forced_is_24h = is_24h;
}

static bool voice_clock_is_24h_style(void) {
  if (s_debug_forced_is_24h >= 0) return s_debug_forced_is_24h != 0;
  return clock_is_24h_style();
}
#else
static bool voice_clock_is_24h_style(void) {
  return clock_is_24h_style();
}
#endif

// Presets support countdown/repeat only (lib/preset_store.h's own
// preset_fields_valid) -- a preset-target session whose parse genuinely
// SUCCEEDED with kind==KIND_STOPWATCH has nothing valid to Save. This is a
// computed read, not a stored VoiceOutcome (the parse itself is genuinely
// VOICE_OK; only the PRESET TARGET makes the parsed kind unusable) -- checked
// at the two content sites (header_update_proc, build_rows) that need to
// degrade to the fail-soft treatment, so there is only ever ONE place this
// rule is expressed, never a second copy of outcome state to keep in sync.
static bool voice_preset_kind_unsupported(void) {
  return s_target == VOICE_TARGET_PRESET && s_outcome == VOICE_OK && s_parse.kind == KIND_STOPWATCH;
}

void screen_voice_cancel_to_launcher(void) {
  if (s_target == VOICE_TARGET_PRESET) {
    // Cursor back on "Say a preset" -- the row the user launched from, same
    // "return to where you came from" idea as the TIMER-target case below,
    // via the row-index single source of truth screen_library.c's own
    // dispatch already shares (lib/view.h's view_library_voice_row).
    nav_go_library_focus_row(view_library_voice_row(app_context_presets()->count));
  } else {
    nav_go_home_focus_voice_row();
  }
}

void screen_voice_show_parse(const char *transcript, const NlParse *parse, VoiceOutcome outcome) {
  if (transcript != NULL) {
    snprintf(s_transcript, sizeof s_transcript, "%s", transcript);
  } else {
    s_transcript[0] = '\0';
  }
  s_parse = (parse != NULL) ? *parse : (NlParse){0};
  s_outcome = outcome;

  if (s_menu != NULL) {
    menu_layer_reload_data(s_menu);
    menu_layer_set_selected_index(s_menu, MenuIndex(0, 0), MenuRowAlignTop, false);
    layer_mark_dirty(menu_layer_get_layer(s_menu));
  }
  if (s_header != NULL) layer_mark_dirty(s_header);
  if (s_info != NULL) layer_mark_dirty(s_info);
  if (s_scroll_indicator != NULL) layer_mark_dirty(s_scroll_indicator);
}

void screen_voice_show_unavailable(void) {
  screen_voice_show_parse(NULL, NULL, voice_outcome_for_status(VOICE_STATUS_CREATE_FAILED, false));
}

// See screen_voice.h's doc comment. In-place edit of the CURRENT
// preview -- does not touch s_transcript/s_outcome, does not reset the menu
// selection (unlike screen_voice_show_parse()). Only meaningful for a
// VOICE_OK preview (the only outcome whose row set ever offers "Add name"/
// "Rename" in the first place); a no-op guard against a stray call is
// deliberately NOT added here -- dictation_am.c's own DICT_SCOPE_NAME arc
// is only ever started from this screen's own "Add name"/"Rename" row
// (select_click's VOICE_ACTION_ADD_NAME case), which by construction only
// exists while s_outcome == VOICE_OK.
void screen_voice_set_name(const char *name) {
  timer_name_copy(s_parse.name, sizeof s_parse.name, name);
  s_parse.has_name = s_parse.name[0] != '\0';

  if (s_menu != NULL) {
    // Row COUNT is unchanged (has_name only flips the row's LABEL, "Add
    // name" <-> "Rename") -- reload the data so draw_row picks up the new
    // label, but deliberately do NOT reset menu_layer_set_selected_index()
    // the way screen_voice_show_parse() does: the user just used the
    // Add-name row and should land back on the same preview, not row 0.
    menu_layer_reload_data(s_menu);
    layer_mark_dirty(menu_layer_get_layer(s_menu));
  }
  if (s_info != NULL) layer_mark_dirty(s_info);  // the name line lives here
}

// --- action row model --------------------------------------------------

typedef enum {
  VOICE_ACTION_START,
  VOICE_ACTION_ADD_NAME,
  VOICE_ACTION_SAY_AGAIN,
  VOICE_ACTION_EDIT_DIAL,
  VOICE_ACTION_SET_DIAL,
  VOICE_ACTION_USE_DIAL,
} VoiceRowAction;

typedef struct {
  const char *label;
  VoiceRowAction action;
} VoiceRow;

// Outcome -> action-row table. Stopwatch has no duration to dial, so
// "Edit on dial" is omitted for it -- but it can still carry a name residue
// (parse_nl's own grammar), so the Add-name/Rename row is not
// stopwatch-gated. VOICE_CANCELLED is never actually shown here --
// dictation_am.c routes it straight home instead of ever calling this
// screen's show_parse() -- so this case exists only as a defensive
// fallthrough to the VOICE_ERROR row set, ensuring this function never
// returns 0 rows if that invariant is ever violated.
static int build_rows(VoiceRow *rows) {
  switch (s_outcome) {
    case VOICE_OK: {
      if (voice_preset_kind_unsupported()) {
        // Honest fail-soft: the parse genuinely succeeded, but a stopwatch
        // can't become a preset
        // (lib/preset_store.h's preset_fields_valid) -- same two-row shape
        // VOICE_PARSE_FAIL uses below (Say again / a dial escape), rather
        // than showing a "Save" row that would have nothing valid to commit.
        // The dial escape routes to an UNPRIMED preset dial (VOICE_ACTION_
        // SET_DIAL -- select_click's target-aware routing sends this to
        // SET_MODE_PRESET_NEW, never SET_MODE_CREATE, for this target) --
        // there is no valid {duration_s, kind} to prime FROM, matching
        // VOICE_PARSE_FAIL's own "graceful floor" framing.
        rows[0] = (VoiceRow){"Say again", VOICE_ACTION_SAY_AGAIN};
        rows[1] = (VoiceRow){"Set on dial", VOICE_ACTION_SET_DIAL};
        return 2;
      }
      int n = 0;
      rows[n++] = (VoiceRow){(s_target == VOICE_TARGET_PRESET) ? "Save" : "Start", VOICE_ACTION_START};
      rows[n++] = (VoiceRow){s_parse.has_name ? "Rename" : "Add name", VOICE_ACTION_ADD_NAME};
      rows[n++] = (VoiceRow){"Say again", VOICE_ACTION_SAY_AGAIN};
      if (s_parse.kind != KIND_STOPWATCH) {
        rows[n++] = (VoiceRow){"Edit on dial", VOICE_ACTION_EDIT_DIAL};
      }
      return n;
    }
    case VOICE_PARSE_FAIL:
      rows[0] = (VoiceRow){"Set on dial", VOICE_ACTION_SET_DIAL};
      rows[1] = (VoiceRow){"Say again", VOICE_ACTION_SAY_AGAIN};
      return 2;
    case VOICE_DATA_UNAVAILABLE:
      rows[0] = (VoiceRow){"Use dial", VOICE_ACTION_USE_DIAL};
      return 1;
    case VOICE_NO_PHONE:
    case VOICE_DISABLED:
      rows[0] = (VoiceRow){"Use dial", VOICE_ACTION_USE_DIAL};
      return 1;
    case VOICE_NO_SPEECH:
      rows[0] = (VoiceRow){"Say again", VOICE_ACTION_SAY_AGAIN};
      return 1;
    case VOICE_ERROR:
    case VOICE_CANCELLED:
    default:
      rows[0] = (VoiceRow){"Say again", VOICE_ACTION_SAY_AGAIN};
      rows[1] = (VoiceRow){"Use dial", VOICE_ACTION_USE_DIAL};
      return 2;
  }
}

// --- header (identity strip / neutral message band) ---------------------

static const char *voice_message_for_outcome(VoiceOutcome outcome) {
  switch (outcome) {
    case VOICE_PARSE_FAIL:
      return "Didn't catch a time";
    case VOICE_DATA_UNAVAILABLE:
      return "Voice data unavailable";
    case VOICE_NO_PHONE:
      return "Voice needs your phone";
    case VOICE_NO_SPEECH:
      return "Didn't hear you";
    case VOICE_DISABLED:
      return "Turn on voice in the Pebble app";
    case VOICE_CANCELLED:
      return "";  // never actually drawn -- the real adapter returns to HOME silently
    case VOICE_ERROR:
    default:
      return "Voice didn't work";
  }
}

static void header_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);

  if (s_outcome == VOICE_OK && !voice_preset_kind_unsupported()) {
    // Same identity language as every other screen in this app (ACTIONS'
    // strip_update_proc): kind glyph + kind colour (DEEP variant,
    // white text on a saturated fill) + the parsed duration. Stopwatch has
    // no duration to show -- empty value string.
    // render_floating_strip only paints its own fixed 30px sub-band -- paper
    // the rest of this (taller) header first so no stale pixels show below
    // the color fill.
    graphics_context_set_fill_color(ctx, RENDER_COLOR_PAPER);
    graphics_fill_rect(ctx, bounds, 0, GCornerNone);
    TimerKind kind = s_parse.kind;
    GColor color = render_kind_color_deep(view_kind_color_token(kind, VIEW_HERO_RUNNING));
    char value[16] = "";
    if (kind != KIND_STOPWATCH) format_hms(value, sizeof value, s_parse.duration_s);
    render_floating_strip(ctx, bounds, color, kind, render_kind_label(kind), value);

    // The clock-target confirm annotation -- render_floating_strip only
    // ever paints its own fixed RENDER_STRIP_H (30px) sub-band, leaving the
    // rest of this (taller, VOICE_HEADER_H) header as blank papered
    // background (the fill_rect above) -- exactly enough room for one more
    // small text line. Makes BOTH residual clock-target ambiguities visible
    // in one glance (an AM/PM mis-guess, or a target that's already passed
    // today) -- that visibility is the whole point of this annotation.
    // "->" (ASCII), not the Unicode "->" glyph,
    // since this app's bundled system fonts don't carry arrow glyphs (the
    // same reason render_row_prefix draws a vector triangle for "^" instead
    // of a glyph: "no glyph in the subset numeral font").
    // v0.1.11 fix: format_clock_ampm, not plain format_clock -- a bare 12h
    // "6:45" cannot distinguish the resolver's AM from PM pick, which is
    // the ONE thing this annotation exists to make visible. 24h devices are
    // unchanged ("-> 18:45").
    if (s_parse.clock.has_clock_target) {
      int target_hour = (int)(s_parse.clock_target_sod / 3600);
      int target_minute = (int)((s_parse.clock_target_sod / 60) % 60);
      char clock_str[12];
      format_clock_ampm(clock_str, sizeof clock_str, target_hour, target_minute,
                         voice_clock_is_24h_style());
      char annotation[20];
      snprintf(annotation, sizeof annotation, "-> %s", clock_str);
      GRect target_rect = GRect(RENDER_MARGIN_X, RENDER_STRIP_H,
                                 (int16_t)(bounds.size.w - 2 * RENDER_MARGIN_X),
                                 (int16_t)(VOICE_HEADER_H - RENDER_STRIP_H));
      graphics_context_set_text_color(ctx, RENDER_COLOR_LABEL);
      graphics_draw_text(ctx, annotation, fonts_get_system_font(FONT_KEY_GOTHIC_14), target_rect,
                          GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
    }
    return;
  }

  // Fail-soft states (parse miss, a dictation status failure, or a
  // preset-target parse whose kind can't become a preset): no kind to
  // color -- a neutral dark-gray band, never alarm-red (a gentle,
  // informational header rather than an error-red alarm treatment),
  // carrying the message, wrapped up to 2
  // lines (the longest message, "Turn on voice in the Pebble app",
  // ellipsizes at one line -- see this macro's own doc comment on
  // VOICE_HEADER_H).
  graphics_context_set_fill_color(ctx, GColorDarkGray);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  GRect msg_rect = GRect(RENDER_MARGIN_X, 4, (int16_t)(bounds.size.w - 2 * RENDER_MARGIN_X),
                          (int16_t)(bounds.size.h - 8));
  graphics_context_set_text_color(ctx, RENDER_COLOR_STRIP_TEXT);
  // Honest message, not a reused "Didn't catch a time" (a time WAS caught --
  // only the kind can't become a preset, per lib/preset_store.h's
  // preset_fields_valid) -- pick the honest one, rather than the
  // dishonest-but-simpler reuse of VOICE_PARSE_FAIL's copy.
  const char *message =
      voice_preset_kind_unsupported() ? "Presets can't be a stopwatch" : voice_message_for_outcome(s_outcome);
  graphics_draw_text(ctx, message, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD), msg_rect,
                      GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}

// --- info band (verbatim transcript echo) --------------------------------

static void info_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, RENDER_COLOR_PAPER);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  int16_t y = 4;
  int16_t remaining_h = (int16_t)(bounds.size.h - 8);

  // A heard/added name gets its own line, above the verbatim echo -- an
  // optional name line, only when has_name. Only ever drawn for VOICE_OK
  // (the only outcome with a live, nameable NlParse that Start actually
  // persists) -- a VOICE_PARSE_FAIL's own residue name, if any, is
  // parser-preserved metadata (a name-only sub-case) not surfaced here
  // (no timer exists yet on that floor to name).
  bool show_name = s_outcome == VOICE_OK && s_parse.has_name && s_parse.name[0] != '\0';
  if (show_name) {
    GRect name_rect = GRect(RENDER_MARGIN_X, y, (int16_t)(bounds.size.w - 2 * RENDER_MARGIN_X), 16);
    graphics_context_set_text_color(ctx, RENDER_COLOR_INK);
    graphics_draw_text(ctx, s_parse.name, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD), name_rect,
                        GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
    y = (int16_t)(y + 16);
    remaining_h = (int16_t)(remaining_h - 16);
  }

  // Only states that reached a real Success dictation status ever have a
  // transcript to echo (OK / PARSE_FAIL) -- the dictation-status error
  // states (no phone / no speech / disabled / error) never got one, so the
  // rest of the band stays blank paper.
  bool has_transcript =
      (s_outcome == VOICE_OK || s_outcome == VOICE_PARSE_FAIL) && s_transcript[0] != '\0';
  if (!has_transcript) return;

  char echo[VOICE_TRANSCRIPT_MAX + 16];
  snprintf(echo, sizeof echo, "heard: \"%s\"", s_transcript);
  GRect echo_rect = GRect(RENDER_MARGIN_X, y, (int16_t)(bounds.size.w - 2 * RENDER_MARGIN_X),
                           remaining_h);
  // The "heard:" transcript uses RENDER_COLOR_MUTED_TEXT (black) rather than
  // RENDER_COLOR_MUTED for readability -- see that constant's own doc
  // comment (render.h) for the other 5 text sites darkened alongside this
  // one.
  graphics_context_set_text_color(ctx, RENDER_COLOR_MUTED_TEXT);
  graphics_draw_text(ctx, echo, fonts_get_system_font(FONT_KEY_GOTHIC_14), echo_rect,
                      GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}

// --- action rows (MenuLayer) ---------------------------------------------

static uint16_t get_num_rows(MenuLayer *menu_layer, uint16_t section_index, void *context) {
  (void)menu_layer;
  (void)section_index;
  (void)context;
  VoiceRow rows[VOICE_MAX_ROWS];
  return (uint16_t)build_rows(rows);
}

static int16_t get_cell_height(MenuLayer *menu_layer, MenuIndex *cell_index, void *context) {
  (void)menu_layer;
  (void)cell_index;
  (void)context;
  return RENDER_MENU_ROW_H;
}

static void draw_row(GContext *ctx, const Layer *cell_layer, MenuIndex *cell_index, void *context) {
  (void)context;
  VoiceRow rows[VOICE_MAX_ROWS];
  int n = build_rows(rows);
  if ((int)cell_index->row >= n) return;  // defensive -- stale index across a state change
  bool highlighted = menu_cell_layer_is_highlighted(cell_layer);
  // Shared action-row rail (render_action_row), per-row hairline
  // (show_hairline=true) -- these rows are the screen's WHOLE content, not a
  // trailing group under something else (screen_library.c's "Save new" row
  // uses this same standalone shape).
  render_action_row(ctx, layer_get_bounds(cell_layer), highlighted, rows[cell_index->row].label,
                     true);
}

// The Start verb -- byte-for-byte the same create-tail every other create
// surface in this app uses (screen_set.c's start_timer, screen_library.c's
// launch_preset), plus an optional store_set_name when the confirm preview
// carries a name (heard inline, or added/renamed via the Add-name row).
static void do_start(void) {
  TimerStore *store = app_context_store();
  time_t now = time(NULL);
  int32_t cookie;
  if (!store_add(store, s_parse.kind, s_parse.duration_s, now, &cookie)) {
    testlog("store", "full");
    vibes_adapter_reject();
    return;
  }
  if (s_parse.has_name) {
    store_set_name(store, cookie, s_parse.name);
  }
  persist_adapter_save(store);
  wakeup_adapter_replan(store, now);
  testlog("voice_create", s_parse.kind == KIND_REPEAT     ? "repeat"
                           : s_parse.kind == KIND_STOPWATCH ? "stopwatch"
                                                             : "countdown");
  nav_go_hero(cookie);
}

// Voice-into-Library: the preset-target sibling of do_start() above --
// appends to the PresetStore instead of the TimerStore, never starts
// anything, and returns to LIBRARY with the cursor ON the freshly-saved row
// (nav_go_library_focus_row, the cursor-restore principle) instead of
// nav_go_hero(). Only ever reached for kind == COUNTDOWN|REPEAT --
// build_rows() routes a preset-target STOPWATCH parse through the
// fail-soft branch instead (voice_preset_kind_unsupported()), so
// preset_store_add's own kind check is defense-in-depth here, never the
// live guard.
static void do_save(void) {
  PresetStore *presets = app_context_presets();
  if (!preset_store_add(presets, s_parse.kind, s_parse.duration_s)) {
    // Mirrors screen_set.c's commit_preset_new() full-store refusal shape
    // EXACTLY -- MAX_PRESETS(12) reached: testlog and stay on THIS confirm
    // screen, no navigation, no reject vibe (commit_preset_new has none
    // either, unlike the TimerStore's start_timer()/launch_preset()
    // refusals -- a preset-store-full refusal has never used a vibe in
    // this codebase). Deliberately NOT HOME's own "Say a timer" row-level
    // MAX_TIMERS gating (screen_home.c's render_action_row_disabled) --
    // this mirrors LIBRARY's OWN "Save new" precedent (ungated row, refuse
    // at commit), not HOME's.
    testlog("preset", "full");
    return;
  }
  int new_index = presets->count - 1;  // preset_store_add always appends
  if (s_parse.has_name) {
    // preset_store_set_name caps to PRESET_NAME_MAX(16) via timer_name_copy
    // regardless of NlParse.name's longer PARSE_NL_NAME_MAX(24) buffer --
    // this call is always a safe, silent re-cap, never a truncation bug.
    preset_store_set_name(presets, new_index, s_parse.name);
  }
  persist_preset_adapter_save(presets);
  testlog("voice_preset_save", s_parse.kind == KIND_REPEAT ? "repeat" : "countdown");
  nav_go_library_focus_row(new_index);
}

static void select_click(int row) {
  VoiceRow rows[VOICE_MAX_ROWS];
  int n = build_rows(rows);
  if (row < 0 || row >= n) return;
  switch (rows[row].action) {
    case VOICE_ACTION_START:
      if (s_target == VOICE_TARGET_PRESET) {
        do_save();
      } else {
        do_start();
      }
      return;
    case VOICE_ACTION_ADD_NAME:
      // A second, NAME-scoped dictation arc -- the transcript is taken
      // VERBATIM as the candidate name (no parse_nl pass); dictation_am.c's
      // deliver() routes a successful attempt to screen_voice_set_name()
      // below, an in-place edit of this SAME preview (not a fresh outcome).
      dictation_am_start(DICT_SCOPE_NAME);
      return;
    case VOICE_ACTION_SAY_AGAIN:
      // A genuine re-attempt -- dictation_am_start() has its own
      // re-entrancy guard, so this is safe even if somehow tapped while an
      // attempt is already in flight (not reachable via a real button
      // press: the system dictation UI owns input while an attempt is
      // running).
      dictation_am_start(DICT_SCOPE_FULL);
      return;
    case VOICE_ACTION_EDIT_DIAL:
      // The primed variant -- reuses the parsed {duration_s, kind} as-is.
      // Only reachable for VOICE_OK non-stopwatch (build_rows omits this
      // row for stopwatch -- no duration to dial). Voice-into-Library: a
      // preset-target session primes SET_MODE_PRESET_NEW instead of
      // SET_MODE_CREATE (nav_go_preset_new_from_voice) -- same parsed seed,
      // different commit target; Back-cancel from there already returns to
      // LIBRARY (screen_set.c's existing SET_MODE_PRESET_NEW case).
      if (s_target == VOICE_TARGET_PRESET) {
        nav_go_preset_new_from_voice(s_parse.duration_s, s_parse.kind == KIND_REPEAT);
      } else {
        nav_go_set_from_voice(s_parse.duration_s, s_parse.kind == KIND_REPEAT);
      }
      return;
    case VOICE_ACTION_SET_DIAL:
    case VOICE_ACTION_USE_DIAL:
      // These are the "graceful floor" escapes from a parse-fail,
      // dictation-error, or (voice_preset_kind_unsupported())
      // preset-target-stopwatch state -- there is no parsed duration/kind
      // to prime FROM in any of these cases, so both stay an UNPRIMED
      // dial -- just a preset-target session's unprimed dial is
      // nav_go_preset_new() (SET_MODE_PRESET_NEW, Back already returns to
      // LIBRARY) instead of nav_go_set() (SET_MODE_CREATE, Back returns to
      // HOME).
      if (s_target == VOICE_TARGET_PRESET) {
        nav_go_preset_new();
      } else {
        nav_go_set();
      }
      return;
  }
}

static void select_click_handler(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
  MenuIndex idx = menu_layer_get_selected_index(s_menu);
  select_click((int)idx.row);
}

static void up_click_handler(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
  // Unlike the "Up-at-top reverses the entry" convention used on other
  // screens, this screen's Up-at-top is a no-op/clamp, not a cancel: a
  // discrete Up press at row 0 does not cancel out to the launcher, since
  // doing so could silently discard a composed named timer on a single
  // stray press. Back remains the only cancel path on this screen
  // (back_click_handler, unchanged). The clamp applies uniformly to every
  // Up press here, whether discrete or held/auto-repeating.
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
  // Back = cancel, everywhere, on every content state. Uses
  // screen_voice_cancel_to_launcher(), not a fixed nav_go_home_focus_voice_
  // row() -- lands the cursor back on whichever row the user launched THIS
  // session from ("Say a timer" or "Say a preset"), instead of resetting to
  // row 0.
  screen_voice_cancel_to_launcher();
}

// Non-reentrant, single window_set_click_config_provider() call -- see
// screen_actions.c's long top-of-file comment for the dead-Select
// reentrancy trap this shape avoids (menu_layer_set_click_config_onto_
// window() is never called on this window).
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
  menu_layer_set_selected_index(s_menu, MenuIndex(0, 0), MenuRowAlignTop, false);
  if (s_scroll_indicator != NULL) layer_mark_dirty(s_scroll_indicator);
}

static void scroll_indicator_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  VoiceRow rows[VOICE_MAX_ROWS];
  int total = build_rows(rows);
  int visible = (int)(bounds.size.h / RENDER_MENU_ROW_H);
  if (visible < 1) visible = 1;
  MenuIndex sel = menu_layer_get_selected_index(s_menu);
  render_scroll_indicator(ctx, bounds, total, visible, (int)sel.row);
}

Window *screen_voice_create(void) {
  s_window = window_create();
  window_set_background_color(s_window, RENDER_COLOR_PAPER);
#ifdef GALLERY_MODE
  // Screen gallery (inner loop, src/c/gallery.c) -- see screen_set.c's
  // matching comment. Never compiled into a release build.
  window_set_click_config_provider(s_window, gallery_click_config_provider);
#else
  window_set_click_config_provider(s_window, click_config_provider);
#endif
  window_set_window_handlers(s_window, (WindowHandlers){
                                            .appear = window_appear,
                                        });

  Layer *root = window_get_root_layer(s_window);
  GRect bounds = layer_get_bounds(root);

  s_header = layer_create(GRect(0, 0, bounds.size.w, VOICE_HEADER_H));
  layer_set_update_proc(s_header, header_update_proc);
  layer_add_child(root, s_header);

  s_info = layer_create(GRect(0, VOICE_HEADER_H, bounds.size.w, VOICE_INFO_H));
  layer_set_update_proc(s_info, info_update_proc);
  layer_add_child(root, s_info);

  int16_t menu_y = (int16_t)(VOICE_HEADER_H + VOICE_INFO_H);
  s_menu = menu_layer_create(GRect(0, menu_y, bounds.size.w, (int16_t)(bounds.size.h - menu_y)));
  // Same MenuLayer fixes every other MenuLayer in this app carries.
  menu_layer_pad_bottom_enable(s_menu, false);
  menu_layer_set_normal_colors(s_menu, RENDER_COLOR_PAPER, RENDER_COLOR_INK);
  menu_layer_set_highlight_colors(s_menu, RENDER_COLOR_PAPER, GColorBlack);
  menu_layer_set_callbacks(s_menu, NULL,
                            (MenuLayerCallbacks){
                                .get_num_rows = get_num_rows,
                                .get_cell_height = get_cell_height,
                                .draw_row = draw_row,
                            });
  layer_add_child(root, menu_layer_get_layer(s_menu));

  s_scroll_indicator =
      layer_create(GRect((int16_t)(bounds.size.w - SCROLL_INDICATOR_WIDTH - SCROLL_INDICATOR_MARGIN),
                          menu_y, SCROLL_INDICATOR_WIDTH, (int16_t)(bounds.size.h - menu_y)));
  layer_set_update_proc(s_scroll_indicator, scroll_indicator_update_proc);
  layer_add_child(root, s_scroll_indicator);

  return s_window;
}

void screen_voice_destroy(void) {
  layer_destroy(s_scroll_indicator);
  s_scroll_indicator = NULL;
  menu_layer_destroy(s_menu);
  s_menu = NULL;
  layer_destroy(s_info);
  s_info = NULL;
  layer_destroy(s_header);
  s_header = NULL;
  window_destroy(s_window);
  s_window = NULL;
}

#ifdef GALLERY_MODE
MenuLayer *screen_voice_debug_menu(void) {
  return s_menu;
}
#endif
