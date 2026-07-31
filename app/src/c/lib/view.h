// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// Pure view-derivation seam: the OVERTIME-vs-alarm branch selector, the
// kind->colour-token map, and the big-value prefix-kind enum (format.c
// returns numeric-only strings; render.c vector-draws the caret/plus this
// module selects -- the LECO numeral fonts have no glyph for them). NEVER
// #include <pebble.h> -- the adapter resolves a ViewColorToken to a real
// GColor (see the palette-token table in src/c/render.h).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "store.h"
#include "timer.h"

// Takes `is_ringing` as an explicit input rather than reading AppState
// directly -- keeps this module a pure function of (Timer, bool),
// independently testable without an AppState fixture.
// VIEW_HERO_STOPWATCH covers a plain stopwatch (Select=Lap, single press) as
// one branch. A post-timer stopwatch (original_total_s > 0, where Select
// should be Pause instead) gets its own POST_TIMER branch rather than being
// handled via a runtime `t->original_total_s > 0` check scattered through the
// adapter. Making the distinction a first-class, host-testable part of this
// pure view seam also lets view_value_prefix (below) give the two branches
// their correct, different prefix answers -- stopwatch needs "+", post-timer
// does not.
typedef enum {
  VIEW_HERO_RUNNING,     // countdown/repeat, not fired (running or paused)
  VIEW_HERO_OVERTIME,    // countdown/repeat, fired && !is_ringing
  VIEW_HERO_ALARM,       // countdown/repeat, fired && is_ringing
  VIEW_HERO_STOPWATCH,   // stopwatch kind, plain (original_total_s == 0)
  VIEW_HERO_POST_TIMER,  // stopwatch kind, converted from a fired countdown (original_total_s > 0)
} ViewHeroBranch;

ViewHeroBranch view_hero_branch(const Timer *t, bool is_ringing);

// Kind->colour-token map (palette ruling): resolved to a
// real named GColor by the adapter (src/c/render.h's token table), never a
// raw GColorFromHEX quantization (that's what collided orange/red under
// naive rendering). A one-off alarm (branch==ALARM, kind !=
// REPEAT) swaps to the dedicated alarm/red token; a repeat alarm keeps its
// own indigo rather than switching to red.
typedef enum {
  VIEW_COLOR_COUNTDOWN,
  VIEW_COLOR_ALARM,
  VIEW_COLOR_STOPWATCH,
  VIEW_COLOR_REPEAT,
} ViewColorToken;

ViewColorToken view_kind_color_token(TimerKind kind, ViewHeroBranch branch);

// The bundled numeral rasters are subset with the glyph set "[0-9:+ ]", which
// includes a plus sign but not a caret. A PLUS prefix is therefore baked as a
// literal '+' into the formatted numeric string (lib/format's
// format_hms_prefixed) rather than vector-drawn -- cheaper, and it reads as
// part of the numeral, for a compact inline "+0:26"/"+4:30" look.
// CARET ("^", standing in for an up-arrow glyph) has no glyph in the subset
// font, so it stays vector-drawn, laid out inline immediately before the
// digits rather than floating at a fixed corner offset (see render.c's
// render_hero_value/render_row_prefix).
typedef enum {
  VIEW_PREFIX_NONE,
  VIEW_PREFIX_CARET,  // alarm value, e.g. "^0:07" -- vector, inline
  VIEW_PREFIX_PLUS,   // overtime/stopwatch value, e.g. "+0:26" -- baked into the string
} ViewPrefixKind;

ViewPrefixKind view_value_prefix(ViewHeroBranch branch);

// The no-background-alarm indicator is driven by wakeup_adapter_is_armed()
// (wakeup_adapter.h) -- real OS-confirmed state from the most recent
// replan -- called directly by screen_hero.c. There is no pure-layer
// predicate for it here: unlike this module's other functions, "is this
// cookie in the last replan's real armed set" is adapter state, not a pure
// function of (Timer, bool).

// HOME's populated state uses a real MenuLayer (native scrolling/cursor/
// select) rather than hand-rolled windowed-scroll math.
//
// HOME's per-row value font rung: fewer timers on screen means roomier
// digits. This is re-derived here for HOME's row value rather than the
// hero's, and is a pure function of the TOTAL timer count (not just the
// on-screen visible subset) -- every row on a given HOME redraw uses the SAME
// rung, so the list reads as one consistent size, not a per-row jumble.
// MAX_VISIBLE_ROWS (4, the "header + 4 rows before scrolling" floor) is the
// MEDIUM/SMALL boundary. A single timer and exactly two timers each get their
// own dedicated rung (XLARGE and LARGE) rather than sharing one, since a lone
// timer is meant to get a noticeably larger face that steps down once a
// second timer appears.
typedef enum {
  VIEW_HOME_SIZE_XLARGE,  // exactly 1 timer -- the roomiest rung
  VIEW_HOME_SIZE_LARGE,   // exactly 2 timers -- one step down
  VIEW_HOME_SIZE_MEDIUM,  // 3-4 timers -- everything still fits, one size down
  VIEW_HOME_SIZE_SMALL,   // 5+ timers -- compact, scrolling territory
} ViewHomeValueSize;

ViewHomeValueSize view_home_value_size(int timer_count);

// HOME's row count: extracted from the row-count arithmetic that used to
// live directly in the screen's row-count callback -- the same pattern as
// `view_home_value_size` above (a pure function of the timer count,
// host-testable, no MenuLayer/Pebble dependency). This arithmetic previously
// had a boundary bug where an un-floored `n+2` made an invisible-but-
// reachable "Clear all" row selectable from an empty store; pulling it into
// `lib/` and pinning it with host tests (n=0/1/2/15/16) guards against a
// repeat of that class of off-by-one, rather than relying solely on an
// emulator flow script.
//
// Row model (HOME now unified onto one MenuLayer in both states
// -- `s_empty_layer` is deleted, with "Say a timer" inserted directly under
// "+ New timer" in BOTH states):
//   timer_count == 0: 3 rows -- the "+ New timer" ring+plus hero (row 0),
//     "Say a timer" (row 1), and "Library" (row 2). NO "Clear all" (nothing
//     to clear) -- this is a deliberately re-grounded `count==0` special
//     case, replacing the old un-gated formula's latent "Clear all" row that
//     used to exist even at n==0 (reachable via Down, though nothing
//     selected it in practice).
//   timer_count >= 1: timer_count + 4 -- the timer rows, then "+ New timer"
//     (row n), "Say a timer" (row n+1), "Library" (row n+2), "Clear all"
//     (row n+3, a locked row+confirm hybrid). Does NOT account for
//     GALLERY_MODE's now-removed debug preset-shelf rows (that scaffold was
//     deleted in favor of the real Library screen this row count now
//     serves).
int view_home_row_count(int timer_count);

// The 0-based row index the "Say a timer" row
// occupies for a given timer count -- pulled out as its own pure, host-tested
// function (mirroring view_home_row_count's own extraction) so
// screen_home.c's cursor-restore logic (nav_go_home_focus_voice_row(), the
// destination for every "cancel out of the voice arc" path) and draw_row's
// own row-dispatch arithmetic can never drift apart. Row model (see
// view_home_row_count's own doc comment above): row 1 when the store is
// empty (row 0 is the "+ New timer" hero), row `timer_count + 1` once
// populated (row `timer_count` is "+ New timer").
int view_home_voice_row(int timer_count);

// Voice-into-Library (mirrors view_home_row_count/view_home_voice_row's own
// extraction+reuse rationale one-for-one):
// LIBRARY's row model -- preset rows (0..preset_count-1), then "Save new"
// (row preset_count), then "Say a preset" (row preset_count+1). Unlike HOME,
// there is no separate count==0 special case in the ARITHMETIC (Library has
// no "Clear all"-style row that only exists once populated) -- the empty
// state still logically has both a "Save new" target (row 0, drawn as the
// composed hero) and a "Say a preset" target (row 1); only the DRAWING
// differs (screen_library.c's draw_row/get_cell_height branch on count==0
// to render row 0 as the full hero), never the row-count/row-index math.
// Host-tested single source of truth shared by screen_library.c's own
// get_num_rows/draw_row/select_click dispatch AND screen_voice.c's preset-
// target voice-cancel cursor-restore (nav_go_library_focus_row), so the two
// can never drift apart -- the exact class of off-by-one found once already
// for HOME's own row count.
int view_library_row_count(int preset_count);
int view_library_voice_row(int preset_count);

// Supports the ringing-screen alarm subheader's "N still on" count: count of
// timers OTHER than `cookie` currently in `store` that are "still on" --
// i.e. NOT paused (the subheader's own wording is "still on," which a
// paused sibling contradicts by definition; a fired-but-not-yet-re-buzzing
// OVERTIME sibling IS still counted, since it's still counting up). Reduced
// to a count rather than showing each other timer's own name+value (e.g.
// "moka +6:45") because the ringing subheader identifies siblings by count
// only -- a count-only reassurance ("others keep running underneath") is
// consistent with how this screen identifies other timers (kind+colour+
// magnitude+position, not by name).
int view_others_active_count(int32_t cookie, const TimerStore *store);

// The HOME/ringing-list ROW label (render.h's shared row primitive) is
// distinct from render_kind_label(), which stays a page-TITLE substitution
// for the single-timer detail screens (hero header, ACTIONS/LAPS floating
// strip, ALARM band) and is unaffected by this function.
//
// An unnamed timer's row renders NO label text -- glyph, kind colour, and
// magnitude carry identity alone. There is no generic kind-word fallback
// ("Countdown"/"Stopwatch"), since such a fallback has no short-name
// guarantee and truncates on the row's limited width. A named timer returns
// its name UNCHANGED -- no ellipsis/clamp logic here, since real names (from
// a preset or from voice dictation) are short by design and are never
// truncated.
//
// `name`: NULL or "" means unnamed. Returns NULL for "no label," or `name`
// unchanged otherwise.
const char *view_row_label(const char *name);
