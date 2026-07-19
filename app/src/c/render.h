// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// Shared adapter draw helpers (the only tier that #includes <pebble.h> for
// rendering) + the palette-token table.
//
// PALETTE RULING: reference hexes are NOT fed to GColorFromHEX because the
// quantization can collapse distinct colors: #E0700C (countdown orange) and
// #C8442A (alarm red) both truncate into RGB bucket (3,1,0) via
// GColorFromRGBA's >>6 floor. Fix: use the platform's own NAMED GColor constants,
// chosen as the nearest visually-distinct match to each reference hex, never a
// raw hex quantization:
//
//   kind       | reference hex | named GColor         | why
//   -----------|------------|------------------------|-----------------------------------------
//   countdown  | #E0700C    | GColorOrange           | direct visual match, distinct from red family
//   alarm      | #C8442A    | GColorDarkCandyAppleRed | a deep, distinct red -- never quantizes toward orange
//   stopwatch  | #1F7A6B    | GColorTiffanyBlue       | teal-family, nearest visually-distinct named teal
//   repeat     | #4F5BB0    | GColorIndigo            | indigo-family, direct visual match
//
// A repeat alarm keeps GColorIndigo (a repeat alarm is indigo, not red) --
// lib/view.c's view_kind_color_token already encodes this; this table is
// purely the ViewColorToken -> GColor resolution.
#pragma once

#include <pebble.h>

#include "fraction_scale.h"
#include "lib/view.h"

#define RENDER_COLOR_PAPER GColorFromHEX(0xEFECE4)
#define RENDER_COLOR_INK GColorFromHEX(0x2A2620)
// Darkened one 64-color bucket from dark-gray-85 to black. The prior 0x7A756A value floors to
// (85,85,85) -- WCAG-fine on paper (~7.5:1) but it read too light
// on the real reflective MiP panel at the small GOTHIC_14 sizes this tier is
// used at ("New timer" footer, SET labels, "ends HH:MM"). Black is the only
// darker step available on the 4-tone grid; WCAG contrast is no longer the
// relevant metric for this tier -- on-device legibility is the criterion, not the ratio.
#define RENDER_COLOR_LABEL GColorFromHEX(0x3A362E)
// Darkened from light-gray-170 (2.3:1, below WCAG's own 3:1 large-text floor) to
// dark-gray-85 (7.5:1). Now the app's dimmest text tier
// (LABEL above collapsed into black), used for genuinely tertiary/
// de-emphasized text (paused hero value, version label, disabled/max-reached
// copy) and the noBgAlarm indicator.
#define RENDER_COLOR_MUTED GColorFromHEX(0x6E6A60)  // noBgAlarm indicator
// Darkens the TEXT half of the RENDER_COLOR_MUTED tier one more step to black
// (the comment above already named black as the only
// darker step left on the 4-tone grid) -- the voice "heard:" transcript, the
// paused-stopwatch value, the version label, "No presets yet", the LIBRARY
// discoverability-hint title, and the disabled/max-reached action-row label
// all use this. The two NON-text MUTED consumers (the paused-hero chip
// FILL, screen_hero.c; the no-bg-alarm indicator GLYPH, screen_hero.c) stay
// on RENDER_COLOR_MUTED unchanged -- darkening a fill/stroke isn't the same
// legibility problem a small font's TEXT ink is, so the darkening is
// scoped to text sites only.
#define RENDER_COLOR_MUTED_TEXT GColorBlack
// The unselected "quiet-card" pill used to be RENDER_COLOR_CARD = GColorWhite, which is INVISIBLE on the
// GColorWhite paper background (a cream-vs-white contrast doesn't survive the
// watch's 64-color palette) -- this made every non-cursor row disappear, leaving
// only the cursor's dark pill floating with no visible neighbors (the "black
// band" misdiagnosis). Retired: no caller should fill a quiet row
// with RENDER_COLOR_CARD any more -- render_row()/render_menu_row() draw a
// 1px RENDER_COLOR_ROW_HAIRLINE separator instead of a card fill.
#define RENDER_COLOR_ROW_HAIRLINE GColorLightGray
// Light text-on-red, for the ringing list's header clock/title (the ringing
// list's header uses a light near-white for its wall clock, since
// RENDER_COLOR_INK would be low-contrast against the red background).
#define RENDER_COLOR_RINGING_TEXT GColorFromHEX(0xFFD9C9)
// Light text-on-kind-color for the alarm header band and the floating strip
// -- both draw white/light text over a saturated kind-color fill, same reasoning
// as RENDER_COLOR_RINGING_TEXT.
#define RENDER_COLOR_STRIP_TEXT GColorWhite
// The SET screen's active-field arrows + windsor-tan accents.
#define RENDER_COLOR_ACCENT GColorWindsorTan
// Post-timer overrun chip -- background is countdown orange blended ~12% over
// the paper background (GColor8 has no alpha blending for graphics_fill_rect,
// so this is the pre-blended solid equivalent); text uses a matching, darker
// orange for contrast against the light chip fill.
#define RENDER_COLOR_OVERRUN_CHIP_BG GColorFromHEX(0xEDDDCA)
#define RENDER_COLOR_OVERRUN_CHIP_TEXT GColorFromHEX(0xB5560F)

// RENDER_COLOR_STRIP_TEXT above is white drawn
// directly on a kind color fill -- WCAG contrast ratio is symmetric
// (contrast(white, X) == contrast(X, white)), so the exact same GColorOrange
// (3.2:1) / GColorTiffanyBlue (2.9:1) figures already flagged as marginal/low
// FOR kind-colored text ON white paper are equally marginal/low for white
// text ON those same kind colors -- confirmed on the real MiP panel, worst
// on the stopwatch (teal) ACTIONS strip. Fix is a DARKER same-hue-family
// variant (never a raw hex -- both
// are named GColor8 64-color swatches, so no GColorFromHEX floor-
// quantization risk):
//   countdown: GColorOrange    (255,85,0) 3.2:1 -> GColorWindsorTan   (170,85,0) 5.2:1
//   stopwatch: GColorTiffanyBlue (0,170,170) 2.9:1 -> GColorMidnightGreen (0,85,85) 8.6:1
// Alarm (GColorDarkCandyAppleRed, 7.8:1) and repeat (GColorIndigo, 10.7:1)
// are already comfortably dark against white -- no deep variant needed;
// render_kind_color_deep() falls through to render_kind_color() for those
// two tokens.
//
// This tier now ALSO covers the BIG hero/row VALUE text drawn directly in kind
// color on paper (the mirror-image case of the strip fix above -- kind color
// as foreground text ink instead of a background fill; the WCAG ratio is the
// same symmetric number either way, so no new colors were needed).
//
// The DEEP tier originally scoped to ONLY the value/number text, leaving glyphs/bars/meta-line accents
// on the SAME paper background at the bright tier -- on real hardware this
// reads as a same-kind color MISMATCH ("the icon and the time...
// are different colors"). The rule is now background-keyed, not
// element-keyed: EVERY kind-colored element sitting on paper/white (hero
// glyph, hero draining/segmented bar, hero value, the stopwatch-laps-band's
// newest-lap value, render_row's glyph/number/bar on a QUIET row, a repeat
// hero's meta-line accent) uses this DEEP tier together; every kind-colored
// element sitting on a genuinely dark background (a cursor/selected black
// pill, a hold-footer's black pill) uses the BRIGHT tier
// (render_kind_color()) together instead -- see render_row()'s own `accent`
// local for the row-level mechanism, and screen_hero.c/screen_alarm.c's
// hero draws for the hero-level one. The floating-strip/alarm-header-band
// FILL case is a third, already-self-consistent idiom (white/light TEXT
// drawn over a kind-colored FILL, never kind-colored text on that fill) --
// unaffected by this rule either way.
#define RENDER_COLOR_COUNTDOWN_DEEP GColorWindsorTan
#define RENDER_COLOR_STOPWATCH_DEEP GColorMidnightGreen

// Resolves a ViewColorToken to the DEEP (safe-for-white-text-background and
// safe-as-kind-colored-text-on-paper) variant -- use this instead of render_kind_color() for EVERY kind-colored
// element (glyph, value/number, draining/segmented bar, meta accent) that
// sits on a light/paper background, or fills the background behind
// RENDER_COLOR_STRIP_TEXT (every render_floating_strip() call site --
// ACTIONS/alarm-options/LAPS/preset-actions). Kind-colored elements sitting
// on a genuinely DARK background (a cursor/selected black pill, a
// hold-footer's black pill) keep render_kind_color() (the bright tier)
// instead -- the tier follows the BACKGROUND, uniformly, never a per-element
// choice within the same context.
GColor render_kind_color_deep(ViewColorToken token);

// Global canvas-layout constants, shared by every screen that adopts the new row/hero/strip components below.
#define RENDER_MARGIN_X 13
#define RENDER_CONTENT_W 174  // 200 - 2*13
#define RENDER_ROW_H 46
// For busy/scroll (3+) rung with smaller font: keeps ROW_H's 46px pitch even though its number font shrinks -- this
// tier targets a tighter ~30-34px row pitch. The get_cell_height implementation selects
// this instead of RENDER_ROW_H once view_home_value_size() reports
// VIEW_HOME_SIZE_SMALL (5+ timers, already-scrolling territory).
#define RENDER_ROW_H_COMPACT 32
#define RENDER_HERO_HEADER_Y 11
#define RENDER_HERO_HEADER_H 16

// Resolves a pure ViewColorToken to the named GColor per the table above.
GColor render_kind_color(ViewColorToken token);

// Allocation discipline: fills a triangle via one lazily-created,
// module-static GPath shared by every triangle this file (and its callers)
// ever draw -- gpath_create() runs at most once; every call after the first
// only overwrites the shared points buffer and redraws (see render.c). Used
// for the hourglass glyph, the repeat loop's arrowhead, the hero value's
// caret prefix, and the SET screen's active-field up/down arrows.
void render_fill_triangle(GContext *ctx, GPoint p0, GPoint p1, GPoint p2, GColor color);

// The kind label ("Countdown"/"Stopwatch"/"Repeat") used as a page-TITLE
// substitution for single-timer detail screens (hero header, ACTIONS/LAPS
// floating strip, ALARM band) -- this is drawn regardless of whether the
// timer has its own name; a named timer's name is shown as the row label
// instead (see lib/view.h's view_row_label), not swapped in here.
const char *render_kind_label(TimerKind kind);

// Resolves lib/view's ViewHomeValueSize to a real system GFont for HOME's row
// value text -- fewer timers, roomier digits.
GFont render_home_value_font(ViewHomeValueSize size);

// Draws the small top-right wall clock, same position on every screen. `bounds`
// is the layer's full bounds. Ink-on-paper text color -- see render_clock_colored
// for a parameterized-color variant.
void render_clock(GContext *ctx, GRect bounds, bool is_24h);

// Same clock, parameterized text color -- the ringing list's header is red, not
// paper, so RENDER_COLOR_INK would be illegible; render_clock is a thin wrapper
// over this with RENDER_COLOR_INK, kept as the single source of truth for the
// clock's text/layout logic (DRY over a second near-duplicate implementation).
void render_clock_colored(GContext *ctx, GRect bounds, bool is_24h, GColor text_color);

// HOME's header hairline -- a 1px GColorLightGray line from (13,39) to (187,39),
// separating the clock from the row list. `bounds` is the layer's full bounds
// (the line is drawn at a fixed y=39 regardless of the layer's own height,
// matching every other screen-relative constant here).
void render_header_hairline(GContext *ctx, GRect bounds);

// The hero header band -- kind glyph + generic kind label (+ optional colored
// suffix, e.g. "- over") + the wall clock, right-aligned. `bounds` is the
// window's full root-layer bounds. `suffix` may be NULL (no suffix drawn).
void render_hero_header(GContext *ctx, GRect bounds, TimerKind kind, GColor kind_color,
                         const char *suffix, GColor suffix_color, bool is_24h);

// The start/end wall-clock row under a hero's draining bar -- "7:00 start" left,
// "end 7:12" right, same box.
void render_start_end_labels(GContext *ctx, GRect bounds, time_t start, time_t end, bool is_24h);

// A small drawn down-arrow glyph (▽), replacing the old literal "v" character
// every "v more"/"more" footer used to draw -- "v" was never a real chevron,
// just the closest ASCII approximation, and reads poorly at this size. `tip`
// is the triangle's bottom (downward-pointing) vertex; the glyph occupies
// MORE_GLYPH_W wide by MORE_GLYPH_H tall, immediately above-and-including `tip`.
// Reuses render_fill_triangle (no new drawing primitive) -- see render.c for
// the shared geometry constants.
void render_more_glyph(GContext *ctx, GPoint tip, GColor color);

// Mirror of render_more_glyph, pointing up (△) -- for the stopwatch "new lap" legend.
// `tip` is the triangle's top (upward-pointing) vertex.
void render_up_glyph(GContext *ctx, GPoint tip, GColor color);

// The universal hold-to-apply hero footer -- a bottom-anchored selected-row pill
// (label left, a drawn down-arrow + "more" right) plus a thin hold-fill bar
// underneath. `hold_active`/`hold_fraction_pm` mirror hold.h's hold_is_active()/
// hold_progress() -- pass hold_fraction_pm=0 with hold_active=false to show the
// bar at rest (empty track, still visible as an affordance). `show_check` --
// pass true to draw a small vector checkmark before `label` instead of baking a
// "Hold: " text prefix into it; false reproduces the old plain-label layout
// exactly. `up_label` -- NULL keeps the pre-existing single "more" line
// (vertically centered, byte-identical rendering); non-NULL stacks a second
// line ABOVE "more" naming the hero's own Up-hold gesture (e.g. "new lap",
// "+5:00", "edit interval") using render_up_glyph instead of render_more_glyph.
// This is now the ONE place a hero's Up-hold hint is drawn -- callers must not
// also draw their own separate legend alongside this footer.
void render_hold_footer(GContext *ctx, GRect bounds, const char *label, GColor color,
                         bool hold_active, int32_t hold_fraction_pm, bool show_check,
                         const char *up_label);

// Same pill/bar as render_hold_footer() above, minus the "more" glyph+text column
// -- shared by HOME's own hold-fill preview scenes (now retired) and the
// real "Clear all N timers?" confirm screen, neither of which has a "more"
// screen to reveal. `hold_active`/`hold_fraction_pm` are passed through
// unchanged from render_hold_footer's own contract (the confirm screen passes
// hold_active=false -- Select there is a plain single-press confirm, not a
// hold-to-apply gesture).
void render_hold_footer_no_more(GContext *ctx, GRect bounds, const char *label, GColor color,
                                 bool hold_active, int32_t hold_fraction_pm, bool show_check);

// The single-press hero footer (top hairline, label left, a drawn down-arrow +
// "more" right, no pill/hold-bar) -- overtime Done, stopwatch Lap, post-timer
// Pause. `show_check` draws the same vector check glyph render_hold_footer's Done
// pill already uses (render_check_glyph) in a reserved column left of the label --
// "✓ Done" for the overtime hfoot specifically; every other hfoot caller
// (stopwatch Lap, post-timer Pause/Resume) passes false.
void render_hfoot(GContext *ctx, GRect bounds, const char *label, bool show_check);

// The floating colored strip header for ACTIONS/options/LAPS -- kind glyph
// (white) + label (white bold) left, current value (light) right. `bounds` is
// the window's full root-layer bounds; the strip itself is the top 30px. Returns
// the strip height so callers can size the sibling MenuLayer frame below it
// (`RENDER_STRIP_H`, exposed directly too).
#define RENDER_STRIP_H 30
void render_floating_strip(GContext *ctx, GRect bounds, GColor color, TimerKind kind,
                            const char *label, const char *value);

// The list-row primitive shared by HOME and the ringing list -- glyph . label .
// [draining bar] . number, in a 13px-inset pill (dark fill on the cursor row, a
// 1px hairline separator otherwise). `cell_bounds` is the MenuLayer cell's own
// local bounds (the standard `layer_get_bounds(cell_layer)` -- local-origin, NOT
// the window's absolute coordinates).
typedef struct {
  bool highlighted;
  TimerKind kind;
  // render_row() resolves ONE background-keyed accent color per row from `kind_color`
  // (BRIGHT tier) and `value_color` (DEEP tier) together, and uses that SAME color
  // for the glyph, the value/number, AND the draining/segmented bar -- never a
  // per-element mix. A CURSOR (black-pill) row uses `kind_color` (bright, safe-on-black);
  // a QUIET (paper) row uses `value_color` (deep, safe-on-white). The earlier
  // per-element split (glyph always bright, value darkened on quiet rows only) read as
  // a same-kind color MISMATCH on-device -- fixed by making the whole kind cluster
  // move together with its background.
  GColor kind_color;  // the BRIGHT tier -- see render_row()'s own `accent` for how it's chosen
  // The DEEP tier -- callers pass render_kind_color_deep(token)'s darker variant
  // here (countdown/stopwatch), or the plain identity color for alarm/repeat
  // (already dark enough -- see render_kind_color_deep's doc). Every RenderRowSpec
  // caller sets this explicitly (no implicit fallback to kind_color) so a future
  // caller can't silently regress to an undarkened value by omission.
  GColor value_color;
  // NULL or "" (lib/view's view_row_label()) draws NO label text at all -- identity
  // via glyph + kind colour + magnitude only, matching an unnamed timer. Non-NULL/
  // non-empty draws it as before -- a named timer's own name. Ignored
  // when number_left_align is set (see below -- the two are mutually exclusive layouts).
  const char *label;
  const char *number;
  GFont number_font;
  ViewPrefixKind number_prefix;  // drawn small, just left of the number (CARET only -- see enum doc)
  bool show_bar;                 // draining bar under the number (countdown/repeat only)
  int32_t bar_fraction_pm;       // 0-RENDER_FRACTION_SCALE, ignored if !show_bar
  // The ringing list's cursor row draws its glyph+number WHITE instead of
  // kind-colored -- red-on-black (GColorDarkCandyAppleRed on the cursor pill)
  // is near-illegible at arm's length; HOME's cursor row does NOT set this
  // (its kind colors all read fine on black) -- number there stays kind-colored.
  // Quiet rows are never affected (kind color always, on both screens).
  bool cursor_value_white;
  // Kills the "lonely row" void (glyph .... number) by moving `number` into
  // the label's usual slot (left-aligned @ROW_LABEL_X, replacing `label`)
  // instead of right-aligning it against the pill's edge. Default false
  // reproduces the pre-existing right-aligned layout byte-for-byte (every other
  // caller leaves this unset). A name-less preset row sets this true (label is
  // NULL there, so this branch is the only one that ever draws); a NAMED preset
  // row sets this false instead, so it falls through to the ordinary label+
  // right-aligned-number layout below (the same one HOME's named rows use).
  bool number_left_align;
  // Paired with number_left_align above: draws a small right-pointing
  // chevron near the pill's right edge (exactly where the right-aligned
  // number used to end) instead of a number there -- the row's own "start
  // this" affordance. Meaningless (ignored) unless number_left_align is
  // also set.
  bool trailing_glyph;
} RenderRowSpec;
void render_row(GContext *ctx, GRect cell_bounds, const RenderRowSpec *spec);

// The HOME/ringing scroll indicator -- thumb GColorBlack, track GColorLightGray
// (both now explicit; the track used to be unpainted, reading as invisible against
// white paper). `total_rows`/`visible_rows` size the thumb; `selected_row`
// positions it. Only draws if total_rows > visible_rows (caller's job to skip the
// call otherwise, matching the existing early-return convention).
void render_scroll_indicator(GContext *ctx, GRect bounds, int total_rows, int visible_rows,
                              int selected_row);

// The ACTIONS/alarm-options/LAPS menu-cell primitive -- a dark inset pill (8px,
// radius 8) on the cursor row, a 1px hairline separator otherwise (replacing the
// SDK's own full-bleed `menu_cell_basic_draw` highlight, same "widen + hairline,
// don't card-fill" fix as the row primitive). Draws its label VERTICALLY CENTERED
// within `cell_bounds` -- content is centered within whatever height `cell_bounds`
// actually is, so a caller's get_cell_height stretching a cell taller than
// RENDER_MENU_ROW_H still reads correctly (no longer a strict "anchored to the
// bottom" slice).
void render_menu_row(GContext *ctx, GRect cell_bounds, bool highlighted, const char *label);

// The root-cause fix for the "5px rail jump" -- HOME's footer ("+ New timer"/
// "Library"/"Clear all") and Library's own "Save new" row now share render_row's
// own content-row rail (RENDER_MARGIN_X inset, ROW_PILL_RADIUS, ROW_PILL_PAD_X/Y
// padding) instead of render_menu_row's narrower ACTIONS/LAPS/alarm-options rail
// (MENU_ROW_PILL_INSET). This is a DELIBERATELY SEPARATE primitive from
// render_menu_row (not a shared-geometry parameter on it): ACTIONS/LAPS/alarm-
// options/the preset-actions sheet keep render_menu_row's own rail untouched, so
// this addition carries zero collateral risk to those screens. `show_hairline`
// lets a grouped block (one leading divider via render_group_divider() below, no
// per-row separator) suppress the resting-state hairline; a standalone action row
// not part of a group always passes true.
void render_action_row(GContext *ctx, GRect cell_bounds, bool highlighted, const char *label,
                        bool show_hairline);

// Same rail/geometry as render_action_row() above, with a leading render_mic_glyph()
// drawn inside the pill's own left padding and the label indented past it -- HOME's
// "Say a timer" row is the one action row that carries an icon. A deliberately narrow
// addition (one more row variant, not a generic icon parameter) since voice is the
// only action row with an icon today -- generalize only if/when a second one needs it.
void render_action_row_mic(GContext *ctx, GRect cell_bounds, bool highlighted, const char *label,
                            bool show_hairline);

// Same rail as render_action_row() above, disabled/max-reached styling -- a dark-gray pill
// instead of black, plus separate quiet/highlighted copy ("All 16 timers in
// use" / "Delete one to add more"). Replaces the old bespoke dashed-hairline
// "+ New timer"/is_max render path -- now unified onto the same rail as
// every other action row instead of its own one-off geometry.
void render_action_row_disabled(GContext *ctx, GRect cell_bounds, bool highlighted,
                                 const char *quiet_label, const char *highlighted_label,
                                 bool show_hairline);

// One leading divider (full content-rail width, x=13..187) drawn at the seam
// between the live timer list and the control block (3px below) -- called by the
// block's FIRST row only; the block's own rows suppress their individual hairlines
// instead (see render_action_row's show_hairline).
void render_group_divider(GContext *ctx, GRect cell_bounds);

// GOTHIC-in-chip optical centering: GOTHIC's font line-box reserves more room
// above cap-height than below the baseline, so box-centering a line of GOTHIC
// text against a SHORT reference (e.g. a small vector glyph drawn at its own
// true center) reads ~5px low. This is the GOTHIC-text analogue of a similar
// fix for numeral fonts (a different constant, for a different font family) --
// named and exposed here, not left as a per-call-site magic number, so the next
// glyph+GOTHIC pairing (e.g. a future icon rail) reuses the measured value
// instead of rediscovering/re-measuring it.
#define RENDER_GOTHIC_CHIP_TEXT_NUDGE_Y 5

// Returns the y-origin for a `text_h`-tall GRect of GOTHIC text so its ink
// optically centers on a short glyph's own true center at `glyph_center_y`
// (box-centers `text_h`, then applies RENDER_GOTHIC_CHIP_TEXT_NUDGE_Y).
int16_t render_gothic_text_y_for_glyph_center(int16_t glyph_center_y, int16_t text_h);

// Two-column variant (LAPS: "Lap N" left, split time right). Same vertical-
// centering behavior as render_menu_row() above.
void render_menu_row_kv(GContext *ctx, GRect cell_bounds, bool highlighted, const char *left,
                         const char *right);

// The ACTIONS/alarm-options/LAPS menu row's pitch -- was 44px (previously
// measured ~61px pitch once the SDK's real default cell metrics and font
// ascent/descent were accounted for), well past the ~36-40px pitch this row's
// content (padding + a 15px font + a small gap) actually needs. At the old
// pitch, the ALARM-options list's mandated 4th row ("Keep counting") sat below
// the fold -- this is the single fix that restores it (all related screens now set
// an explicit get_cell_height returning exactly this constant, replacing the
// SDK's own un-overridden default). LAPS previously stretched its first row's
// cell to fill leftover viewport space and bottom-anchored content inside it --
// for a list whose EVERY row is real content
// (unlike HOME's dedicated last-row-is-always-the-footer case), that made a
// short list's one real row visually float near the screen's BOTTOM instead
// of sitting under the strip where row 0 belongs.
// Fixed by dropping that stretch: LAPS now uses
// this same fixed pitch as ACTIONS, no special-casing, and its rows read as
// a plain top-down list under the strip, matching every other list in this app.
#define RENDER_MENU_ROW_H 38

// The 52px alarm header band (fired hero only) -- a full-width colored band
// with a top status line ("~ TIME'S UP - one-off" / "~ CHECK - buzzes again in N")
// + the light wall clock, and a big kind-label title below. `title_font` is
// the caller's choice (the one-off/repeat variants use different sizes, ~30px/~26px
// -- the caller picks the nearest system font per variant rather than this function
// hardcoding one for both). The status line's available width is computed from
// the CLOCK's actual rendered width (not a flat reservation) -- same "measure,
// don't guess" pattern hold_footer_more_col_w() already uses for "more".
#define RENDER_ALARM_BAND_H 52
void render_alarm_header_band(GContext *ctx, GRect bounds, GColor band_color,
                               const char *status_line, const char *title, GFont title_font,
                               bool is_24h);

// A reusable full-width top strip -- paper background + the standard top-right
// clock -- for windows whose main content is a MenuLayer. A MenuLayer's own
// draw_row callback can only paint INSIDE a cell (same reasoning as the
// scroll-indicator overlay below), so these screens give the MenuLayer a frame
// that starts BELOW a small sibling Layer sized to this height, with this as its
// update_proc, rather than trying to draw the clock from inside a cell.
#define RENDER_CLOCK_STRIP_HEIGHT 24
void render_clock_strip_update_proc(Layer *layer, GContext *ctx);

// Per-mille fixed-point scale for every bar-fill fraction in this file (and
// hold.h's hold_progress()/hold_debug_force()) -- 0 = empty, 1000 = full.
// Replaces the app's only float usage (a 0.0-1.0 `float fraction` threaded
// through render_bar/render_segmented_bar/render_hold_footer*, hold_progress(),
// and every bar-drawing screen) with this integer representation so the libgcc
// softfloat helpers drop out of the link. Callers holding a percent (0-100,
// e.g. timer_progress_pct()) convert with `pct * 10` -- exact, no precision loss
// (the old float path's pixel output is exactly floor(width * pct / 100) at these
// magnitudes, which integer `width * (pct*10) / RENDER_FRACTION_SCALE` reproduces
// exactly). Sourced from fraction_scale.h, the single shared definition with
// hold.c's own use of the same scale -- see that header's own comment for why
// it's a separate, pebble.h-independent header.
#define RENDER_FRACTION_SCALE FRACTION_SCALE_PM

// Draws a horizontal bar (draining/hold-fill -- the continuous rounded-rect
// primitive, native graphics_fill_rect corner_radius). `fraction_pm` is
// 0-RENDER_FRACTION_SCALE (see above). `radius` is the caller's corner radius
// (the hero draining bar uses 3, the row/hold-footer bars use 2
// -- this used to be hardcoded to 3 for every caller).
void render_bar(GContext *ctx, GRect rect, int32_t fraction_pm, GColor fill, GColor track,
                 int16_t radius);

// The REPEAT hero's segmented interval bar. The bar's own SEGMENT COUNT
// tracks the current round number, not a fixed row
// -- round N (this timer's (`completed_segments` + 1)-th interval) draws exactly
// N equal segments left-to-right, N-1 solid + the rightmost filling over the
// CURRENT interval's own progress (round 1 draws as a single full-width
// filling bar, i.e. a plain progress bar), for rounds
// 1..RENDER_SEGMENT_CAP. **PAST the cap: the bar does NOT freeze at
// RENDER_SEGMENT_CAP segments** -- it drops back to a SINGLE full-width filling
// bar, visually identical to round 1 -- the separate "done xN" text is still what
// carries the TRUE completed count past the cap; this bar's own segment count is
// a bounded visual only, same "no wraparound indicated" MVP simplification the old
// fixed-5 implementation already accepted. `current_fraction_pm` (0-RENDER_FRACTION_SCALE,
// timer_progress_pct()*10 -- the CURRENT interval's own progress) is what the
// last (rightmost, or only, past the cap) segment fills to; this single formula
// naturally covers both a running repeat (partial fill on the in-progress interval)
// and the moment it fires (current_fraction_pm reaches RENDER_FRACTION_SCALE, so
// that segment reads as fully filled too, without a special case).
#define RENDER_SEGMENT_CAP 6
void render_segmented_bar(GContext *ctx, GRect rect, int completed_segments,
                           int32_t current_fraction_pm, GColor fill, GColor track);

// 10h-duration shrink-to-fit: a real 10:00:00 countdown clips to "10:0..." at
// the hero's 56px numeral, even though most durations fit at that size fine.
// No new font is bundled: every
// hero call site already has a smaller EXISTING numeral raster one tier down
// (56->42, 42->32) that was simply never considered as a fallback. Measures
// `value` at `preferred` (unbounded width, so the measurement itself never wraps/
// clips) and returns `preferred` if it fits within `rect`'s width, `fallback`
// otherwise -- callers pass the returned font straight into render_hero_value()'s
// own font parameter in place of the tier they used to hardcode. render_row()'s
// own equivalent numeral-vs-glyph-column fit is now handled INSIDE render_row()
// itself (render.c's private fit_row_number_font()), not via a caller-side
// pre-check using this function -- a per-caller guard left every OTHER
// render_row() consumer (HOME's LARGE/MEDIUM/SMALL count tiers, named Library
// preset rows, the ringing list) able to bypass the invariant entirely. This
// function remains the public helper for HERO screens, which don't go through
// render_row() at all.
GFont render_fit_numeral_font(const char *value, GRect rect, GFont preferred, GFont fallback);

// Draws the big hero value string in `font` (caller picks the raster -- added a
// 56px tier alongside the existing 42px one, so this is no longer hardcoded to
// one size) plus, for VIEW_PREFIX_CARET only, a vector "^" prefix (the subset
// numeral fonts have no "^" glyph) drawn INLINE immediately left of the first
// digit, sized to the font's own scale, with the (prefix + digits) pair centered
// together as one group within `rect` (previously a small fixed-size triangle
// floating at a flat top-left offset, visually disconnected from the centered
// digits). VIEW_PREFIX_PLUS draws NO vector at all -- the caller is expected to
// have already baked a literal "+" into `value` (lib/format's format_hms_prefixed) --
// VIEW_PREFIX_NONE and VIEW_PREFIX_PLUS are therefore handled identically here
// (both just draw `value` centered).
void render_hero_value(GContext *ctx, GRect rect, const char *value, ViewPrefixKind prefix,
                        GColor color, GFont font);

// A simple vector kind marker (filled circle) -- used for HOME's countdown/
// repeat row glyph and the ringing list's row glyph, neither of which has
// its own distinct shape requirement (unlike render_kind_glyph's
// stopwatch-ring special case).
void render_kind_marker(GContext *ctx, GPoint center, int radius, GColor color);

// The noBgAlarm indicator used to be a plain render_kind_marker() filled dot,
// but a bare dot carries no cue about WHAT it means. A
// prohibition sign (open circle + diagonal slash) is chosen deliberately: it's a
// shape independently recognized as "this won't happen" without depending on any
// font glyph (vector-only convention already established for the value prefixes)
// or on new copy/naming this app doesn't have room to add. Small vector primitives
// only (a stroked circle + one line) -- legible at the same small scale the dot was.
void render_no_bg_alarm_indicator(GContext *ctx, GPoint center, int radius, GColor color);

// The real per-kind glyph shapes -- countdown = hourglass (two opposed filled
// triangles), stopwatch = ring + crown-nub + hand, repeat = open loop arc +
// arrowhead. These supersede earlier placeholder shapes (dot/plain-ring, kept
// only as render_kind_marker, still used where no shape identity is required).
// All vector: any triangle needed (hourglass, repeat's arrowhead) reuses one
// lazily-created, module-static GPath whose points buffer is overwritten per
// shape rather than gpath_create/destroy'd per frame (see render.c's triangle
// helper) -- circles/arcs/rects are Pebble immediate-mode primitives and never
// allocate.
void render_kind_glyph(GContext *ctx, GPoint center, int radius, GColor color, TimerKind kind);

// A small "list/stack" mark -- three stacked 2px horizontal bars in a 12px-tall
// box -- used for empty-HOME's secondary "Library" action (glyph+label centered
// as one group under the New-timer hero). Vector-only, allocation-free (plain
// graphics_fill_rect calls, no GPath). `center` is the glyph's own visual center
// (matches render_kind_glyph's/render_kind_marker's own center-point contract).
void render_stack_glyph(GContext *ctx, GPoint center, GColor color);

// A small "microphone" vector mark -- a rounded-rect capsule (the mic head) +
// a stand arc cradling its base + a short stem + a base line -- fits the same
// 12px-tall glyph-grid box render_stack_glyph uses above (`center` is the glyph's
// own visual center, same contract). Vector-only, allocation-free (fill_rect/
// draw_arc/draw_line, no GPath). Used by render_action_row_mic() below (HOME's
// "Say a timer" row); exposed publicly for symmetry with every other glyph
// primitive here, in case a future caller needs it standalone.
void render_mic_glyph(GContext *ctx, GPoint center, GColor color);

// A small rounded pill showing how far a "keep counting" stopwatch has run past
// its original countdown -- "from a 15:00 timer" + this chip's "+0:38". `value_text`
// is numeric-only (format_hms's contract -- no "+" baked in); the "+" is
// vector-drawn at a scale sized for this chip's small text, separately from
// render_hero_value's vector prefix. render_overrun_chip_size lets a caller
// measure the chip BEFORE drawing (to center a multi-element row, e.g. label +
// chip together) without duplicating the text-measurement math.
GSize render_overrun_chip_size(const char *value_text);
void render_overrun_chip(GContext *ctx, GPoint origin, const char *value_text, GColor bg,
                          GColor text_color);

// Centered composed group idiom, extracted for reuse -- LIBRARY's own empty
// state now pixel-matches this exact idiom instead of approximating it:
// glyph+label centered as ONE group on the cell's own x-midpoint; the cursor is
// a text-sized centered black capsule (render_overrun_chip's own "pill sized to
// content, radius = h/2" capsule model), NOT a full-width pill. `draw_glyph` is
// any glyph function sharing render_stack_glyph's/render_mic_glyph's own (ctx,
// center, color) contract. Was local to HOME's empty-state rows; now shared so
// LIBRARY's own empty-state "Say a preset" row uses the IDENTICAL geometry
// (margins, chip padding, gothic text nudge), not a second hand-tuned copy.
void render_composed_group(GContext *ctx, GRect cell_bounds, bool highlighted,
                            void (*draw_glyph)(GContext *, GPoint, GColor), const char *label);

// The short centered "seat" divider separating a composed hero from the
// composed row(s) below it (60px, centered on the CELL's own x-midpoint, not
// a hardcoded screen constant -- both HOME and LIBRARY are 200px wide, so
// this generalizes without change). Extracted alongside render_composed_group above for reuse.
void render_composed_seat_divider(GContext *ctx, GRect cell_bounds);
