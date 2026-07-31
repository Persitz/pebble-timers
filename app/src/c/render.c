// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

#include "render.h"

#include <stdio.h>

#include "fonts_adapter.h"
// Unconditional include -- gallery.h is self-guarded (its declarations and
// body are `#ifdef GALLERY_MODE`-gated internally), so this compiles to
// nothing in a normal release build. Still a real (if compiled-out) coupling
// from this generic render layer to a test-fixture module: render_build_menu_
// screen() below calls gallery_click_config_provider() under GALLERY_MODE.
#include "gallery.h"
#include "lib/format.h"

#define CLOCK_PAD_X 13
#define CLOCK_PAD_TOP 10
#define CLOCK_HEIGHT 20

GColor render_kind_color(ViewColorToken token) {
  switch (token) {
    case VIEW_COLOR_ALARM:
      return GColorDarkCandyAppleRed;
    case VIEW_COLOR_STOPWATCH:
      return GColorTiffanyBlue;
    case VIEW_COLOR_REPEAT:
      return GColorIndigo;
    case VIEW_COLOR_COUNTDOWN:
    default:
      return GColorOrange;
  }
}

GColor render_kind_color_deep(ViewColorToken token) {
  switch (token) {
    case VIEW_COLOR_STOPWATCH:
      return RENDER_COLOR_STOPWATCH_DEEP;
    case VIEW_COLOR_COUNTDOWN:
      return RENDER_COLOR_COUNTDOWN_DEEP;
    case VIEW_COLOR_ALARM:
    case VIEW_COLOR_REPEAT:
    default:
      // Already comfortably dark against white text (7.8:1 / 10.7:1) -- see
      // render.h's doc comment on RENDER_COLOR_COUNTDOWN_DEEP/_STOPWATCH_DEEP.
      return render_kind_color(token);
  }
}

const char *render_kind_label(TimerKind kind) {
  switch (kind) {
    case KIND_STOPWATCH:
      return "Stopwatch";
    case KIND_REPEAT:
      return "Repeat";
    case KIND_COUNTDOWN:
    default:
      return "Countdown";
  }
}

// --- Allocation discipline: one lazily-created, reused GPath for every
// small filled triangle this file draws (hourglass halves, the repeat loop's
// arrowhead, SET's active-field up/down arrows) -- gpath_create() runs AT
// MOST ONCE (the first triangle ever drawn), never per-frame; every
// subsequent call only overwrites the shared points buffer's 3 GPoints and
// redraws ("no per-frame allocs; GPaths/fonts created once"). Also used to
// replace render_hero_value's own prefix triangle, which previously called
// gpath_create/gpath_destroy on every single draw (a pre-existing violation
// of the same rule, fixed opportunistically here since this file already
// owns the pattern).
static GPoint s_tri_points[3];
static GPath *s_tri_path;

void render_fill_triangle(GContext *ctx, GPoint p0, GPoint p1, GPoint p2, GColor color) {
  s_tri_points[0] = p0;
  s_tri_points[1] = p1;
  s_tri_points[2] = p2;
  if (s_tri_path == NULL) {
    static const GPathInfo info = {.num_points = 3, .points = s_tri_points};
    s_tri_path = gpath_create(&info);
  }
  graphics_context_set_fill_color(ctx, color);
  gpath_draw_filled(ctx, s_tri_path);
}

GFont render_home_value_font(ViewHomeValueSize size) {
  switch (size) {
    case VIEW_HOME_SIZE_XLARGE:
      // HOME's XLARGE rung -- was sharing LARGE's 32px with the two-timer
      // row, one size class too small for a single-timer HOME row (~46px
      // target). Reuses the
      // hero's already-bundled 42px raster (free -- no new resource) rather
      // than adding a dedicated size.
      return fonts_adapter_numeral_42();
    case VIEW_HOME_SIZE_LARGE:
      // HOME's 2-timer row -- JetBrains Mono at the same 32px this tier
      // already drew at.
      return fonts_adapter_numeral_32();
    case VIEW_HOME_SIZE_MEDIUM:
      return fonts_get_system_font(FONT_KEY_LECO_20_BOLD_NUMBERS);
    case VIEW_HOME_SIZE_SMALL:
    default:
      return fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
  }
}

void render_clock(GContext *ctx, GRect bounds, bool is_24h) {
  render_clock_colored(ctx, bounds, is_24h, RENDER_COLOR_INK);
}

void render_clock_colored(GContext *ctx, GRect bounds, bool is_24h, GColor text_color) {
  time_t now = time(NULL);
  struct tm *tm_now = localtime(&now);

  char clock_text[8];
  if (tm_now != NULL) {
    format_clock(clock_text, sizeof clock_text, tm_now->tm_hour, tm_now->tm_min, is_24h);
  } else {
    format_clock(clock_text, sizeof clock_text, 0, 0, is_24h);
  }

  GRect clock_box = GRect(0, CLOCK_PAD_TOP, bounds.size.w - CLOCK_PAD_X, CLOCK_HEIGHT);
  graphics_context_set_text_color(ctx, text_color);
  graphics_draw_text(ctx, clock_text, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD), clock_box,
                      GTextOverflowModeTrailingEllipsis, GTextAlignmentRight, NULL);
}

void render_bar(GContext *ctx, GRect rect, int32_t fraction_pm, GColor fill, GColor track,
                 int16_t radius) {
  if (fraction_pm < 0) fraction_pm = 0;
  if (fraction_pm > RENDER_FRACTION_SCALE) fraction_pm = RENDER_FRACTION_SCALE;

  graphics_context_set_fill_color(ctx, track);
  graphics_fill_rect(ctx, rect, radius, GCornersAll);

  GRect fill_rect = rect;
  // Integer truncation toward zero, same direction the old `(int16_t)(w *
  // fraction)` float cast truncated -- see render.h's RENDER_FRACTION_SCALE
  // doc comment for why this reproduces the old pixel output exactly for
  // every percent-derived fraction in this app.
  fill_rect.size.w = (int16_t)((int32_t)rect.size.w * fraction_pm / RENDER_FRACTION_SCALE);
  if (fill_rect.size.w > 0) {
    graphics_context_set_fill_color(ctx, fill);
    graphics_fill_rect(ctx, fill_rect, radius, GCornersAll);
  }
}

void render_segmented_bar(GContext *ctx, GRect rect, int completed_segments,
                           int32_t current_fraction_pm, GColor fill, GColor track) {
  if (current_fraction_pm < 0) current_fraction_pm = 0;
  if (current_fraction_pm > RENDER_FRACTION_SCALE) current_fraction_pm = RENDER_FRACTION_SCALE;
  if (completed_segments < 0) completed_segments = 0;

  // Growing tally (render.h's own doc comment has the full spec): the
  // current round is completed_segments+1. Rounds 1..RENDER_SEGMENT_CAP draw
  // that many EQUAL segments across the whole rect (round 1 = one
  // full-width segment), the first (n-1) solid, the last filling to
  // current_fraction_pm. PAST the cap, the bar does NOT freeze at
  // RENDER_SEGMENT_CAP segments -- it drops back to a SINGLE full-width
  // filling bar, visually identical to round 1 -- the separate "done xN"
  // text (screen_hero.c) is still what carries the true completed count past
  // the cap.
  int round = completed_segments + 1;
  int n, solid;
  if (round > RENDER_SEGMENT_CAP) {
    n = 1;
    solid = 0;
  } else {
    n = round;
    solid = n - 1;
  }

  // Plain `(rect.size.w - gap*(n-1)) / n` integer division leaves an
  // unpainted remainder at the rail's right edge for every N that doesn't
  // divide evenly into `available` (e.g. rect.size.w=174, gap=4: N=3 paints
  // 173/174, N=4 172/174, N=5 171/174, N=6 170/174).
  //
  // Widening the last/current segment to absorb the remainder would reach
  // the full rail, but breaks the "N equal segments" invariant: at N=6 a
  // widened current segment (29px vs 25px for each completed one) would
  // make a 90% current fill (floor(29*0.9)=26px) read as LONGER than a
  // fully completed segment, the opposite of what "90% of one round"
  // should communicate. Instead the remainder is absorbed into the GAPS:
  // every segment (including the current one) stays EXACTLY `seg_w`,
  // always, and the leftover 0..(n-1) pixels are distributed one-per-gap
  // across the first `remainder` gaps (the rest stay the nominal 4px),
  // which still reaches the exact same 174px rail:
  //
  //   N | available (174-gap*(N-1)) | seg_w=floor(avail/N) | remainder | gaps (nominal 4, `remainder` of them +1px) | total
  //   --|---------------------------|------------------------|-----------|---------------------------------------------|-------
  //   1 |            174            |           174           |     0     |                 (no gaps)                     |  174
  //   2 |            170            |            85           |     0     |                4                              |  174
  //   3 |            166            |            55           |     1     |              5+4=9                            |  174
  //   4 |            162            |            40           |     2     |            5+5+4=14                           |  174
  //   5 |            158            |            31           |     3     |          5+5+5+4=19                           |  174
  //   6 |            154            |            25           |     4     |        5+5+5+5+4=24                          |  174
  //
  // e.g. N=6: seg_w=25 for all 6 segments (150px total, no segment ever
  // differs from any other), remainder=4 -> the first 4 of the 5 gaps widen
  // to 5px, the 5th stays 4px (24px total) -> 150+24=174, the full rail,
  // with the "N equal segments" invariant held exactly, not just approximately.
  const int16_t base_gap = (n > 1) ? 4 : 0;
  int16_t available = (int16_t)(rect.size.w - base_gap * (n - 1));
  int16_t seg_w = (int16_t)(available / n);
  int16_t remainder = (int16_t)(available - (int32_t)seg_w * n);  // 0..n-1, absorbed into gaps below

  int16_t x = rect.origin.x;
  for (int i = 0; i < n; i++) {
    GRect seg = GRect(x, rect.origin.y, seg_w, rect.size.h);
    graphics_context_set_fill_color(ctx, track);
    graphics_fill_rect(ctx, seg, 2, GCornersAll);

    int32_t seg_fill_pm = (i < solid) ? RENDER_FRACTION_SCALE : current_fraction_pm;
    if (seg_fill_pm > 0) {
      GRect fill_rect = seg;
      fill_rect.size.w = (int16_t)((int32_t)seg_w * seg_fill_pm / RENDER_FRACTION_SCALE);
      if (fill_rect.size.w > 0) {
        graphics_context_set_fill_color(ctx, fill);
        graphics_fill_rect(ctx, fill_rect, 2, GCornersAll);
      }
    }

    x = (int16_t)(x + seg_w);
    if (i < n - 1) {
      int16_t this_gap = (int16_t)(base_gap + (i < remainder ? 1 : 0));
      x = (int16_t)(x + this_gap);
    }
  }
}

// 10h-duration shrink-to-fit (render.h's own doc comment has the full
// rationale/citation) -- measured with a generously wide, never-wrapping box
// so the returned size reflects the string's true single-line width at
// `preferred`, then compared against `rect`'s own width.
GFont render_fit_numeral_font(const char *value, GRect rect, GFont preferred, GFont fallback) {
  GSize size = graphics_text_layout_get_content_size(
      value, preferred, GRect(0, 0, 400, rect.size.h), GTextOverflowModeFill, GTextAlignmentLeft);
  return (size.w > rect.size.w) ? fallback : preferred;
}

void render_hero_value(GContext *ctx, GRect rect, const char *value, ViewPrefixKind prefix,
                        GColor color, GFont font) {
  graphics_context_set_text_color(ctx, color);

  if (prefix != VIEW_PREFIX_CARET) {
    // NONE, or PLUS with the "+" already baked into `value` by the caller
    // (lib/format's format_hms_prefixed) -- either way just draw the
    // string, centered.
    graphics_draw_text(ctx, value, font, rect, GTextOverflowModeTrailingEllipsis,
                        GTextAlignmentCenter, NULL);
    return;
  }

  // The "^" alarm prefix has no glyph in the numeral font -- stays a
  // vector, but drawn INLINE immediately left of the first digit, at digit
  // scale, with the (triangle + digits) pair centered together as one group
  // within `rect` -- previously a small fixed-size triangle anchored at a
  // flat top-left offset, visually disconnected from the (separately)
  // centered digits.
  GSize text_size = graphics_text_layout_get_content_size(
      value, font, GRect(0, 0, rect.size.w, rect.size.h), GTextOverflowModeFill,
      GTextAlignmentLeft);
  // Integer percent scale (42%, 90%) -- exact for these small pixel
  // magnitudes (see render.h's RENDER_FRACTION_SCALE doc comment for the
  // same truncation-equivalence argument); avoiding float here too keeps the
  // link free of the libgcc softfloat helpers a single float op would pull
  // in.
  int16_t tri_h = (int16_t)((int32_t)text_size.h * 42 / 100);  // ~digit cap-height scale
  if (tri_h < 8) tri_h = 8;
  int16_t tri_w = (int16_t)((int32_t)tri_h * 9 / 10);
  const int16_t gap = 6;
  int16_t total_w = (int16_t)(tri_w + gap + text_size.w);
  int16_t start_x = (int16_t)(rect.origin.x + (rect.size.w - total_w) / 2);
  if (start_x < rect.origin.x) start_x = rect.origin.x;
  int16_t mid_y = (int16_t)(rect.origin.y + rect.size.h / 2);

  int16_t tri_cx = (int16_t)(start_x + tri_w / 2);
  render_fill_triangle(ctx, GPoint(tri_cx, (int16_t)(mid_y - tri_h / 2)),
                        GPoint((int16_t)(tri_cx - tri_w / 2), (int16_t)(mid_y + tri_h / 2)),
                        GPoint((int16_t)(tri_cx + tri_w / 2), (int16_t)(mid_y + tri_h / 2)), color);

  GRect text_rect = GRect((int16_t)(start_x + tri_w + gap), rect.origin.y,
                           (int16_t)(text_size.w + 4), rect.size.h);
  graphics_draw_text(ctx, value, font, text_rect, GTextOverflowModeFill, GTextAlignmentLeft, NULL);
}

void render_kind_marker(GContext *ctx, GPoint center, int radius, GColor color) {
  graphics_context_set_fill_color(ctx, color);
  graphics_fill_circle(ctx, center, radius);
}

// The "Composed" library-row glyph: three stacked 2px horizontal bars, 12px
// wide, pitch 5px (bar + 3px gap) --
// total glyph height 12px (2 + 3 + 2 + 3 + 2). `center` is the visual center
// of the whole 3-bar glyph (matches render_kind_marker's own center-point
// contract) -- allocation-free, plain graphics_fill_rect calls.
#define STACK_GLYPH_BAR_W 12
#define STACK_GLYPH_BAR_H 2
#define STACK_GLYPH_PITCH 5
void render_stack_glyph(GContext *ctx, GPoint center, GColor color) {
  graphics_context_set_fill_color(ctx, color);
  for (int i = -1; i <= 1; i++) {
    int16_t y = (int16_t)(center.y + i * STACK_GLYPH_PITCH - STACK_GLYPH_BAR_H / 2);
    graphics_fill_rect(ctx, GRect((int16_t)(center.x - STACK_GLYPH_BAR_W / 2), y, STACK_GLYPH_BAR_W,
                                   STACK_GLYPH_BAR_H),
                        0, GCornerNone);
  }
}

// Mic glyph: mic head (a rounded-rect capsule) + a stand arc cradling its
// base (a symmetric ~140-degree arc centered on the BOTTOM of its bounding
// circle, open at the top -- Pebble's angle 0 = top,
// clockwise, so 110..250 sweeps lower-right -> bottom -> lower-left) + a
// short stem + a base line. Total vertical extent center.y-6..center.y+6 --
// the same 12px-tall glyph-grid box render_stack_glyph above uses.
#define MIC_GLYPH_HEAD_W 6
#define MIC_GLYPH_HEAD_H 7
#define MIC_GLYPH_STAND_R 5
void render_mic_glyph(GContext *ctx, GPoint center, GColor color) {
  graphics_context_set_fill_color(ctx, color);
  GRect head = GRect((int16_t)(center.x - MIC_GLYPH_HEAD_W / 2), (int16_t)(center.y - 6),
                      MIC_GLYPH_HEAD_W, MIC_GLYPH_HEAD_H);
  graphics_fill_rect(ctx, head, (int16_t)(MIC_GLYPH_HEAD_W / 2), GCornersAll);

  graphics_context_set_stroke_color(ctx, color);
  graphics_context_set_stroke_width(ctx, 2);
  GRect stand_rect = GRect((int16_t)(center.x - MIC_GLYPH_STAND_R), (int16_t)(center.y - 6),
                            (int16_t)(MIC_GLYPH_STAND_R * 2), (int16_t)(MIC_GLYPH_STAND_R * 2));
  graphics_draw_arc(ctx, stand_rect, GOvalScaleModeFitCircle, DEG_TO_TRIGANGLE(110),
                     DEG_TO_TRIGANGLE(250));

  graphics_draw_line(ctx, GPoint(center.x, (int16_t)(center.y + 4)),
                      GPoint(center.x, (int16_t)(center.y + 6)));
  graphics_draw_line(ctx, GPoint((int16_t)(center.x - 3), (int16_t)(center.y + 6)),
                      GPoint((int16_t)(center.x + 3), (int16_t)(center.y + 6)));
}

void render_no_bg_alarm_indicator(GContext *ctx, GPoint center, int radius, GColor color) {
  graphics_context_set_stroke_color(ctx, color);
  graphics_context_set_stroke_width(ctx, 1);
  graphics_draw_circle(ctx, center, radius);
  // Diagonal slash through the circle -- reads as "disabled/won't fire"
  // without depending on a bell (or any other) shape being recognizable at
  // this small a radius.
  // Integer percent scale (70%) -- see render_hero_value's identical
  // integer-percent-scale note above; was `(float)radius * 0.7f`.
  int16_t dx = (int16_t)((int32_t)radius * 7 / 10);
  graphics_draw_line(ctx, GPoint(center.x - dx, center.y - dx), GPoint(center.x + dx, center.y + dx));
}

void render_kind_glyph(GContext *ctx, GPoint center, int radius, GColor color, TimerKind kind) {
  switch (kind) {
    case KIND_STOPWATCH: {
      // Ring + crown-nub + hand: a stroked circle (nudged 1px down to leave
      // room for the nub above it, so the ring reads slightly below
      // center), a small filled nub at 12 o'clock, and a
      // short hand from the center. All immediate-mode primitives -- no
      // GPath, no allocation.
      graphics_context_set_stroke_color(ctx, color);
      graphics_context_set_stroke_width(ctx, 2);
      graphics_draw_circle(ctx, GPoint(center.x, center.y + 1), radius - 1);
      graphics_context_set_fill_color(ctx, color);
      graphics_fill_rect(ctx, GRect(center.x - 2, center.y - radius - 1, 4, 3), 1, GCornersAll);
      graphics_fill_rect(ctx, GRect(center.x - 1, center.y - 2, 2, 5), 0, GCornerNone);
      return;
    }
    case KIND_REPEAT: {
      // Open loop arc (gap at upper-right) + a small filled arrowhead at the
      // open end. graphics_draw_arc is an immediate-mode
      // primitive (no allocation); the arrowhead reuses the shared triangle
      // helper (also allocation-free after its first call anywhere).
      GRect arc_rect = GRect(center.x - radius, center.y - radius, radius * 2, radius * 2);
      graphics_context_set_stroke_color(ctx, color);
      graphics_context_set_stroke_width(ctx, 2);
      graphics_draw_arc(ctx, arc_rect, GOvalScaleModeFitCircle, DEG_TO_TRIGANGLE(35),
                         DEG_TO_TRIGANGLE(325));
      int32_t angle = DEG_TO_TRIGANGLE(35);
      GPoint tip = GPoint((int16_t)(center.x + radius * sin_lookup(angle) / TRIG_MAX_RATIO),
                           (int16_t)(center.y - radius * cos_lookup(angle) / TRIG_MAX_RATIO));
      render_fill_triangle(ctx, GPoint(tip.x - 4, tip.y - 3), GPoint(tip.x + 4, tip.y - 1),
                    GPoint(tip.x, tip.y + 4), color);
      return;
    }
    case KIND_COUNTDOWN:
    default: {
      // Hourglass: two opposed filled triangles sharing the center pinch --
      // both draws reuse the same shared triangle GPath
      // (its points buffer is simply overwritten between the two calls, no
      // second allocation).
      int16_t r = (int16_t)radius;
      render_fill_triangle(ctx, GPoint(center.x - r, center.y - r), GPoint(center.x + r, center.y - r),
                    GPoint(center.x, center.y), color);
      render_fill_triangle(ctx, GPoint(center.x - r, center.y + r), GPoint(center.x + r, center.y + r),
                    GPoint(center.x, center.y), color);
      return;
    }
  }
}

void render_clock_strip_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, RENDER_COLOR_PAPER);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  render_clock(ctx, bounds, clock_is_24h_style());
}

#define CHIP_FONT FONT_KEY_GOTHIC_14_BOLD
#define CHIP_PLUS_W 12   // reserved width for the vector "+" incl. its own padding
#define CHIP_PAD_X 6
#define CHIP_PAD_Y 1

static GSize chip_text_size(const char *value_text) {
  return graphics_text_layout_get_content_size(value_text, fonts_get_system_font(CHIP_FONT),
                                                GRect(0, 0, 100, 20), GTextOverflowModeFill,
                                                GTextAlignmentLeft);
}

GSize render_overrun_chip_size(const char *value_text) {
  GSize text_size = chip_text_size(value_text);
  return GSize(CHIP_PLUS_W + text_size.w + 2 * CHIP_PAD_X, text_size.h + 2 * CHIP_PAD_Y);
}

void render_overrun_chip(GContext *ctx, GPoint origin, const char *value_text, GColor bg,
                          GColor text_color) {
  GSize size = render_overrun_chip_size(value_text);
  GRect pill = GRect(origin.x, origin.y, size.w, size.h);

  graphics_context_set_fill_color(ctx, bg);
  graphics_fill_rect(ctx, pill, size.h / 2, GCornersAll);

  // Vector "+" (never a font glyph), scaled for this chip's small text
  // rather than render_hero_value's LECO-sized mark.
  GPoint plus_center = GPoint((int16_t)(origin.x + CHIP_PAD_X + 3), (int16_t)(origin.y + size.h / 2));
  graphics_context_set_fill_color(ctx, text_color);
  graphics_fill_rect(ctx, GRect(plus_center.x - 3, plus_center.y - 1, 6, 2), 0, GCornerNone);
  graphics_fill_rect(ctx, GRect(plus_center.x - 1, plus_center.y - 3, 2, 6), 0, GCornerNone);

  GRect text_rect = GRect((int16_t)(origin.x + CHIP_PAD_X + CHIP_PLUS_W - 4), origin.y,
                           (int16_t)(size.w - CHIP_PAD_X - CHIP_PLUS_W + 4), size.h);
  graphics_context_set_text_color(ctx, text_color);
  graphics_draw_text(ctx, value_text, fonts_get_system_font(CHIP_FONT), text_rect,
                      GTextOverflowModeFill, GTextAlignmentLeft, NULL);
}

// =====================================================================
// Global layout + shared components.
// =====================================================================

// --- HOME header hairline ------------------------------------------

void render_header_hairline(GContext *ctx, GRect bounds) {
  (void)bounds;
  graphics_context_set_stroke_color(ctx, RENDER_COLOR_ROW_HAIRLINE);
  graphics_context_set_stroke_width(ctx, 1);
  graphics_draw_line(ctx, GPoint(RENDER_MARGIN_X, 39), GPoint(200 - RENDER_MARGIN_X, 39));
}

// --- Hero header band ------------------------------------------------

void render_hero_header(GContext *ctx, GRect bounds, TimerKind kind, GColor kind_color,
                         const char *suffix, GColor suffix_color, bool is_24h) {
  GPoint glyph_c = GPoint(RENDER_MARGIN_X + 7, RENDER_HERO_HEADER_Y + 8);
  render_kind_glyph(ctx, glyph_c, 7, kind_color, kind);

  const char *label = render_kind_label(kind);
  GFont label_font = fonts_get_system_font(FONT_KEY_GOTHIC_14);
  // Label sits at x=34 (glyph at x=13 + 21, not +18).
  GRect label_rect = GRect(RENDER_MARGIN_X + 21, RENDER_HERO_HEADER_Y, 90, RENDER_HERO_HEADER_H);
  graphics_context_set_text_color(ctx, RENDER_COLOR_LABEL);
  graphics_draw_text(ctx, label, label_font, label_rect, GTextOverflowModeTrailingEllipsis,
                      GTextAlignmentLeft, NULL);

  if (suffix != NULL && suffix[0] != '\0') {
    GSize label_size = graphics_text_layout_get_content_size(
        label, label_font, GRect(0, 0, 100, RENDER_HERO_HEADER_H), GTextOverflowModeFill,
        GTextAlignmentLeft);
    GRect suffix_rect = GRect((int16_t)(RENDER_MARGIN_X + 21 + label_size.w), RENDER_HERO_HEADER_Y,
                               70, RENDER_HERO_HEADER_H);
    graphics_context_set_text_color(ctx, suffix_color);
    graphics_draw_text(ctx, suffix, label_font, suffix_rect, GTextOverflowModeTrailingEllipsis,
                        GTextAlignmentLeft, NULL);
  }

  render_clock(ctx, bounds, is_24h);
}

// --- start/end clock row ----------------------------------------------

void render_start_end_labels(GContext *ctx, GRect bounds, time_t start, time_t end, bool is_24h) {
  (void)bounds;
  GFont font = fonts_get_system_font(FONT_KEY_GOTHIC_14);
  graphics_context_set_text_color(ctx, RENDER_COLOR_LABEL);

  struct tm *tm_start = localtime(&start);
  char start_str[24];
  if (tm_start != NULL) {
    char clock_str[8];
    format_clock(clock_str, sizeof clock_str, tm_start->tm_hour, tm_start->tm_min, is_24h);
    snprintf(start_str, sizeof start_str, "%s start", clock_str);
  } else {
    start_str[0] = '\0';
  }
  // 87px (not 90) so the two boxes meet at x=100 instead of overlapping by
  // 6px in the middle (13+87=100, 200-13-87=100).
  GRect start_rect = GRect(RENDER_MARGIN_X, 120, 87, 14);
  graphics_draw_text(ctx, start_str, font, start_rect, GTextOverflowModeTrailingEllipsis,
                      GTextAlignmentLeft, NULL);

  struct tm *tm_end = localtime(&end);
  char end_str[24];
  if (tm_end != NULL) {
    char clock_str[8];
    format_clock(clock_str, sizeof clock_str, tm_end->tm_hour, tm_end->tm_min, is_24h);
    snprintf(end_str, sizeof end_str, "end %s", clock_str);
  } else {
    end_str[0] = '\0';
  }
  GRect end_rect = GRect(200 - RENDER_MARGIN_X - 87, 120, 87, 14);
  graphics_draw_text(ctx, end_str, font, end_rect, GTextOverflowModeTrailingEllipsis,
                      GTextAlignmentRight, NULL);
}

// --- hold-footer / hfoot -------------------------------------------

#define HOLD_PILL_RECT GRect(12, 176, 176, 34)
#define HOLD_BAR_RECT GRect(12, 213, 176, 3)
#define HFOOT_RULE_Y 200
#define HFOOT_LABEL_Y 206

// The "more" reachability affordance used to be the literal text "v more" --
// "v" as a stand-in chevron, since Pebble's system fonts have no real
// down-arrow glyph (same "no glyph in the font" reasoning as the "✓"/
// "▲"/"+" vector-only convention already established for render_check_glyph/
// render_row_prefix/the hero value prefix). Drawn here as a small filled
// triangle instead, via render_fill_triangle (the one-shared-GPath
// discipline -- no new allocation primitive).
#define MORE_GLYPH_W 8
#define MORE_GLYPH_H 6
#define MORE_GLYPH_GAP 4  // gap between the glyph and the "more" text

void render_more_glyph(GContext *ctx, GPoint tip, GColor color) {
  render_fill_triangle(ctx, tip,
                        GPoint((int16_t)(tip.x - MORE_GLYPH_W / 2), (int16_t)(tip.y - MORE_GLYPH_H)),
                        GPoint((int16_t)(tip.x + MORE_GLYPH_W / 2), (int16_t)(tip.y - MORE_GLYPH_H)),
                        color);
}

void render_up_glyph(GContext *ctx, GPoint tip, GColor color) {
  render_fill_triangle(ctx, tip,
                        GPoint((int16_t)(tip.x - MORE_GLYPH_W / 2), (int16_t)(tip.y + MORE_GLYPH_H)),
                        GPoint((int16_t)(tip.x + MORE_GLYPH_W / 2), (int16_t)(tip.y + MORE_GLYPH_H)),
                        color);
}

// "more" (plus the glyph drawn just left of it) gets a fixed, reserved
// column on the right (measured once, not a magic number -- accommodates the
// font's actual width) so a long hold label (e.g. "Hold to keep going")
// ellipsizes INSIDE its own column instead of visually overlapping "more"
// (both used to share nearly the full pill width) -- observed on the repeat
// alarm hero.
static int16_t hold_footer_more_col_w(void) {
  GSize size = graphics_text_layout_get_content_size(
      "more", fonts_get_system_font(FONT_KEY_GOTHIC_14), GRect(0, 0, 80, 20),
      GTextOverflowModeFill, GTextAlignmentLeft);
  return (int16_t)(size.w + MORE_GLYPH_W + MORE_GLYPH_GAP + 6);
}

// The pill's right slot used to show a single "v more" line, always
// present, while the stopwatch separately drew its own "^ new lap / v more"
// stacked legend next to the laps band -- two different UI elements both
// teaching the Down="more" affordance, duplicated and crowded together on
// screen. Consolidated into ONE place: this column, which now stacks TWO
// rows (up-hold hint over "more") whenever a hero passes a non-NULL
// `up_label` to render_hold_footer -- the row height/gap below mirror the
// stopwatch's own retired external-legend geometry, sized to fit inside
// HOLD_PILL_RECT's 34px height (2px top pad + 2x14px rows + 2px inter-row
// gap + 2px bottom pad = 34). A hero with NO up-hold (show_check heroes,
// the gated-off countdown, overtime/post-timer's own render_hfoot which
// doesn't call this at all) passes NULL and gets the single
// vertically-centered "more" line exactly as before -- zero visual change
// for those callers.
//
// The stacked block originally used a 3px top pad (3px top / 34-3-30=1px
// bottom -- asymmetric), which read as sitting slightly below center on
// device. FOOTER_LEGEND_TOP_PAD is now 2, splitting the 4px of slack
// (34 - 2*14 - 2 = 4) evenly top/bottom -- a 1px nudge up, confirmed against
// every two-line-legend hero (running countdown w/ add-1/4 legend, repeat,
// stopwatch). The single-line "v more"-only variant (below) already centers
// on the FULL pill height via its own rect, independently confirmed still
// centered -- left unchanged.
#define FOOTER_LEGEND_ROW_H 14
#define FOOTER_LEGEND_ROW_GAP 2
#define FOOTER_LEGEND_TOP_PAD 2
// Caps how wide the stacked legend column can grow for a long up_label (e.g.
// a countdown's "+5:00" quarter-add amount) -- measured-not-guessed like
// hold_footer_more_col_w() above, but capped so the LEFT hold-action label
// always keeps a legible minimum width even if a future up_label were
// unexpectedly long (GTextOverflowModeTrailingEllipsis on this column's own
// text draw handles anything wider than the cap, same safety net the left
// label already relies on).
#define FOOTER_LEGEND_MAX_COL_W 56

static int16_t footer_legend_col_w(const char *up_label) {
  int16_t w = hold_footer_more_col_w();
  if (up_label != NULL) {
    GSize up_size = graphics_text_layout_get_content_size(
        up_label, fonts_get_system_font(FONT_KEY_GOTHIC_14), GRect(0, 0, 150, 20),
        GTextOverflowModeFill, GTextAlignmentLeft);
    int16_t up_col_w = (int16_t)(up_size.w + MORE_GLYPH_W + MORE_GLYPH_GAP + 6);
    if (up_col_w > w) w = up_col_w;
    if (w > FOOTER_LEGEND_MAX_COL_W) w = FOOTER_LEGEND_MAX_COL_W;
  }
  return w;
}

// One legend row: icon at the column's left edge (tip vertically centered
// on `rect`), label right-aligned within the same rect -- the exact draw
// shape render_hold_footer's own single "more" line always used, now shared
// by both the stacked up/more rows and the no-up-hold single-row case below.
static void draw_footer_legend_row(GContext *ctx, GRect rect, bool up_icon, const char *label) {
  graphics_context_set_text_color(ctx, GColorLightGray);
  graphics_draw_text(ctx, label, fonts_get_system_font(FONT_KEY_GOTHIC_14), rect,
                      GTextOverflowModeTrailingEllipsis, GTextAlignmentRight, NULL);
  GPoint tip = GPoint((int16_t)(rect.origin.x + MORE_GLYPH_W / 2),
                       (int16_t)(rect.origin.y + rect.size.h / 2 + MORE_GLYPH_H / 2));
  if (up_icon) {
    render_up_glyph(ctx, tip, GColorLightGray);
  } else {
    render_more_glyph(ctx, tip, GColorLightGray);
  }
}

// The "✓ Done" hold-footer copy -- "✓" is not a Pebble system-font
// glyph (same reasoning as the "▲"/"+" value prefixes' established
// vector-only convention), so it's drawn here as two stroked line segments,
// the same allocation-free style as render_no_bg_alarm_indicator's
// prohibition slash.
#define CHECK_GLYPH_COL_W 20  // reserved column left of the label, incl. gap
static void render_check_glyph(GContext *ctx, GPoint center, GColor color) {
  graphics_context_set_stroke_color(ctx, color);
  graphics_context_set_stroke_width(ctx, 2);
  graphics_draw_line(ctx, GPoint((int16_t)(center.x - 6), center.y),
                      GPoint((int16_t)(center.x - 2), (int16_t)(center.y + 4)));
  graphics_draw_line(ctx, GPoint((int16_t)(center.x - 2), (int16_t)(center.y + 4)),
                      GPoint((int16_t)(center.x + 6), (int16_t)(center.y - 5)));
}

void render_hold_footer(GContext *ctx, GRect bounds, const char *label, GColor color,
                         bool hold_active, int32_t hold_fraction_pm, bool show_check,
                         const char *up_label) {
  (void)bounds;
  GRect pill = HOLD_PILL_RECT;
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, pill, 8, GCornersAll);

  int16_t legend_col_w = footer_legend_col_w(up_label);
  int16_t check_col_w = show_check ? CHECK_GLYPH_COL_W : 0;
  // The LEFT hold-action label stays ONE line, vertically centered across
  // the pill's FULL height (pill.size.h) regardless of whether the right
  // slot below is one line or two -- unchanged from before this
  // consolidation.
  GRect label_rect = GRect((int16_t)(pill.origin.x + 10 + check_col_w), pill.origin.y,
                            (int16_t)(pill.size.w - 14 - legend_col_w - check_col_w), pill.size.h);
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, label, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD), label_rect,
                      GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  if (show_check) {
    render_check_glyph(
        ctx, GPoint((int16_t)(pill.origin.x + 18), (int16_t)(pill.origin.y + pill.size.h / 2)),
        GColorWhite);
  }

  int16_t legend_x = (int16_t)(pill.origin.x + pill.size.w - 10 - legend_col_w);
  if (up_label != NULL) {
    // Stacked two-line legend: up-hold hint on top, "more" below -- see the
    // footer-legend-consolidation doc comment above footer_legend_col_w().
    GRect up_rect = GRect(legend_x, (int16_t)(pill.origin.y + FOOTER_LEGEND_TOP_PAD), legend_col_w,
                          FOOTER_LEGEND_ROW_H);
    GRect more_rect =
        GRect(legend_x,
              (int16_t)(pill.origin.y + FOOTER_LEGEND_TOP_PAD + FOOTER_LEGEND_ROW_H +
                        FOOTER_LEGEND_ROW_GAP),
              legend_col_w, FOOTER_LEGEND_ROW_H);
    draw_footer_legend_row(ctx, up_rect, true, up_label);
    draw_footer_legend_row(ctx, more_rect, false, "more");
  } else {
    // No up-hold on this hero (or it's gated off) -- single "more" line,
    // vertically centered on the full pill height, byte-identical to the
    // pre-consolidation rendering.
    GRect more_rect = GRect(legend_x, pill.origin.y, legend_col_w, pill.size.h);
    draw_footer_legend_row(ctx, more_rect, false, "more");
  }

  // The hold-fill bar's radius is 2 (not the hero draining bar's 3).
  render_bar(ctx, HOLD_BAR_RECT, hold_active ? hold_fraction_pm : 0, color, GColorDarkGray, 2);
}

// Same pill + hold-fill bar as render_hold_footer() above, but WITHOUT the
// "v more" reachability column -- that affordance only makes sense on a
// hero screen (it teaches Down="more" content beyond the pill); the confirm
// screen has no further content to reveal, so the column is simply omitted
// here. Used by screen_clear_confirm.c's "Clear all N timers?" confirm
// screen (a plain single-press Select, not a hold -- hold_active is always
// false there). A separate function (not a new render_hold_footer
// parameter) keeps the real hero/alarm/stopwatch Pause/Resume/Done/Lap
// footers byte-for-byte unchanged.
void render_hold_footer_no_more(GContext *ctx, GRect bounds, const char *label, GColor color,
                                 bool hold_active, int32_t hold_fraction_pm, bool show_check) {
  // HOME's own MenuLayer draws its rows independently of this sibling
  // overlay Layer (see screen_home.c's hold_overlay_update_proc) -- unlike
  // the hero's single full-screen Layer, HOME's real row content extends
  // behind this whole footer band, so without this mask the quiet
  // "+ New timer" footer text peeked out below the pill. RENDER_COLOR_PAPER
  // matches HOME's own background everywhere else.
  GRect mask = GRect(bounds.origin.x, HOLD_PILL_RECT.origin.y, bounds.size.w,
                      (int16_t)(bounds.origin.y + bounds.size.h - HOLD_PILL_RECT.origin.y));
  graphics_context_set_fill_color(ctx, RENDER_COLOR_PAPER);
  graphics_fill_rect(ctx, mask, 0, GCornerNone);

  GRect pill = HOLD_PILL_RECT;
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, pill, 8, GCornersAll);

  int16_t check_col_w = show_check ? CHECK_GLYPH_COL_W : 0;
  GRect label_rect = GRect((int16_t)(pill.origin.x + 10 + check_col_w), pill.origin.y,
                            (int16_t)(pill.size.w - 20 - check_col_w), pill.size.h);
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, label, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD), label_rect,
                      GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  if (show_check) {
    render_check_glyph(
        ctx, GPoint((int16_t)(pill.origin.x + 18), (int16_t)(pill.origin.y + pill.size.h / 2)),
        GColorWhite);
  }

  render_bar(ctx, HOLD_BAR_RECT, hold_active ? hold_fraction_pm : 0, color, GColorDarkGray, 2);
}

void render_hfoot(GContext *ctx, GRect bounds, const char *label, bool show_check) {
  (void)bounds;
  graphics_context_set_stroke_color(ctx, RENDER_COLOR_ROW_HAIRLINE);
  graphics_context_set_stroke_width(ctx, 1);
  graphics_draw_line(ctx, GPoint(RENDER_MARGIN_X, HFOOT_RULE_Y),
                      GPoint(200 - RENDER_MARGIN_X, HFOOT_RULE_Y));

  // Reserve the same CHECK_GLYPH_COL_W column render_hold_footer's Done pill
  // uses, only when show_check asks for it (overtime Done only -- the "✓
  // Done" state).
  int16_t check_col_w = show_check ? CHECK_GLYPH_COL_W : 0;
  GRect label_rect = GRect((int16_t)(RENDER_MARGIN_X + check_col_w), HFOOT_LABEL_Y,
                            (int16_t)(100 - check_col_w), 18);
  graphics_context_set_text_color(ctx, RENDER_COLOR_INK);
  graphics_draw_text(ctx, label, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD), label_rect,
                      GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  if (show_check) {
    render_check_glyph(
        ctx, GPoint((int16_t)(RENDER_MARGIN_X + 8), (int16_t)(HFOOT_LABEL_Y + 9)), RENDER_COLOR_INK);
  }

  GRect more_rect = GRect(200 - RENDER_MARGIN_X - 90, HFOOT_LABEL_Y + 2, 90, 16);
  // Routed through RENDER_COLOR_LABEL (not a flat GColorDarkGray) so this
  // hint matches every sibling quiet-text element on the same screens
  // (post-timer Pause/Resume, stopwatch Lap, overtime Done) instead of
  // reading one shade lighter than them.
  graphics_context_set_text_color(ctx, RENDER_COLOR_LABEL);
  graphics_draw_text(ctx, "more", fonts_get_system_font(FONT_KEY_GOTHIC_14), more_rect,
                      GTextOverflowModeTrailingEllipsis, GTextAlignmentRight, NULL);
  render_more_glyph(ctx,
                     GPoint((int16_t)(more_rect.origin.x + MORE_GLYPH_W / 2),
                            (int16_t)(more_rect.origin.y + more_rect.size.h / 2 + MORE_GLYPH_H / 2)),
                     RENDER_COLOR_LABEL);
}

// --- floating strip -----------------------------------------------------

void render_floating_strip(GContext *ctx, GRect bounds, GColor color, TimerKind kind,
                            const char *label, const char *value) {
  GRect strip = GRect(bounds.origin.x, bounds.origin.y, bounds.size.w, RENDER_STRIP_H);
  graphics_context_set_fill_color(ctx, color);
  graphics_fill_rect(ctx, strip, 0, GCornerNone);

  render_kind_glyph(ctx, GPoint(RENDER_MARGIN_X + 6, 15), 6, RENDER_COLOR_STRIP_TEXT, kind);

  GRect label_rect = GRect(RENDER_MARGIN_X + 16, 7, 90, 18);
  graphics_context_set_text_color(ctx, RENDER_COLOR_STRIP_TEXT);
  graphics_draw_text(ctx, label, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD), label_rect,
                      GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  // The strip's own value -- the live timer's H:MM/M:SS -- was the same
  // GOTHIC_14 (non-bold) as everything
  // else here, and was too small and low-contrast against the (previously
  // lighter) kind-color fill on-device, worst on the
  // stopwatch ACTIONS strip. Bumped one tier to GOTHIC_18_BOLD (matches the
  // weight already used for the pill/menu-row labels elsewhere in this file)
  // -- both bigger and visually heavier, on top of the darker DEEP fill
  // callers now pass in (see render_kind_color_deep). Widened from 100 to
  // 120px to keep pace with the larger glyphs (right edge unchanged --
  // still comfortably clear of the left-side label's actual rendered width,
  // "Stopwatch" being the longest kind label).
  GRect value_rect = GRect(bounds.size.w - RENDER_MARGIN_X - 120, 5, 120, 22);
  graphics_context_set_text_color(ctx, RENDER_COLOR_STRIP_TEXT);
  graphics_draw_text(ctx, value, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD), value_rect,
                      GTextOverflowModeTrailingEllipsis, GTextAlignmentRight, NULL);
}

// --- row + "+ New timer" footer row ---------------------------------

#define ROW_PILL_PAD_Y 2
#define ROW_PILL_RADIUS 9
#define ROW_PILL_PAD_X 8
#define ROW_GLYPH_RADIUS 7
// The glyph column's own reserved width (glyph diameter + its own left/right
// padding) -- a label OR a number is never allowed to start before
// RENDER_MARGIN_X + this. Single source of truth for ROW_LABEL_X below AND
// render_row()'s own internal numeral-fit check (fit_row_number_font()) --
// previously this reservation was only known to a CALLER (screen_home.c's
// XLARGE-only fit-check against a same-shaped public constant), which meant
// every OTHER render_row() consumer (HOME's LARGE/MEDIUM/SMALL count tiers,
// named Library preset rows, the ringing list) could bypass the invariant
// entirely by never running that caller-side check (a second HOME timer, at
// the LARGE tier, once regressed the same collision this way). Moved fully
// inside render.c/render_row() so NO caller can bypass it again.
#define ROW_GLYPH_COL_W 31
#define ROW_LABEL_X (RENDER_MARGIN_X + ROW_GLYPH_COL_W)
// The maximum width render_row()'s own NUMBER may occupy while still leaving
// the glyph column clear -- RENDER_CONTENT_W minus the glyph column (above)
// minus the pill's own right inset (ROW_PILL_PAD_X). Used by
// fit_row_number_font() (below) so the numeral-width invariant applies to
// EVERY render_row() caller uniformly, regardless of which font tier it
// originally asked for.
#define ROW_NUMBER_MAX_W (RENDER_CONTENT_W - ROW_GLYPH_COL_W - ROW_PILL_PAD_X)
#define ROW_BAR_H 4
#define ROW_BAR_BOTTOM_PAD 2  // gap between the draining bar's bottom edge and the row's own bottom
#define ROW_NUMBER_BAR_GAP 3  // min gap between number box bottom and draining bar top
// The VIEW_PREFIX_CARET glyph's own horizontal reach to the LEFT of number_left
// -- render_row_prefix()'s triangle is anchored at (number_left -
// ROW_PREFIX_ANCHOR_OFFSET) and spans +/-ROW_PREFIX_HALF_W from that anchor
// at its widest (base) row, so its leftmost painted pixel is at
// number_left - (ROW_PREFIX_ANCHOR_OFFSET + ROW_PREFIX_HALF_W). The ringing
// list (the only render_row() caller passing VIEW_PREFIX_CARET) must reserve
// this FULL reach in its label's own max-width computation, not just a
// flat gap measured from number_left -- a prior fixed "-6" gap left the
// caret's own leftmost 6px painting directly over the label's allowed rect
// for any name long enough to reach that edge.
#define ROW_PREFIX_ANCHOR_OFFSET 7
#define ROW_PREFIX_HALF_W 5
#define ROW_PREFIX_RESERVE_W (ROW_PREFIX_ANCHOR_OFFSET + ROW_PREFIX_HALF_W)

// Vector "^" prefix at row scale, drawn inline immediately left of the
// number (no glyph in the subset numeral font). Bumped from ~7px tall to
// ~9px (~60-70% of the row number's cap height) to fix the multi-alarm
// ringing list's "tiny/misplaced ^" appearance. PLUS is no longer drawn here
// at all -- every row-level "+"
// consumer (HOME stopwatch rows) now gets it baked into the number string
// instead (lib/format's format_hms_prefixed), same as the hero value.
static void render_row_prefix(GContext *ctx, GPoint anchor, GColor color) {
  render_fill_triangle(ctx, GPoint(anchor.x, (int16_t)(anchor.y - 5)),
                        GPoint((int16_t)(anchor.x - ROW_PREFIX_HALF_W), (int16_t)(anchor.y + 4)),
                        GPoint((int16_t)(anchor.x + ROW_PREFIX_HALF_W), (int16_t)(anchor.y + 4)), color);
}

// The "Composed" Library preset rows' small right-pointing chevron -- the
// row's own "start this" affordance, replacing the right-aligned number when
// RenderRowSpec.trailing_glyph is set. `tip` is the glyph's rightmost point
// (mirrors render_more_glyph's/render_up_glyph's own "tip is the pointed
// end" contract). Reuses the one shared triangle GPath, no new allocation.
#define ROW_CHEVRON_REACH 6
#define ROW_CHEVRON_SPAN 8
// The chevron's own left reach (ROW_CHEVRON_REACH) plus a small gap,
// matching the same "reserve the full glyph/prefix reach, not just its tip"
// pattern ROW_PREFIX_RESERVE_W already established for the caret -- the
// number_left_align branch below (below's own doc comment has the full
// story) must not let its number's natural width run into this reservation.
#define ROW_CHEVRON_RESERVE_W (ROW_CHEVRON_REACH + 6)
static void render_chevron_glyph(GContext *ctx, GPoint tip, GColor color) {
  render_fill_triangle(ctx, tip,
                        GPoint((int16_t)(tip.x - ROW_CHEVRON_REACH), (int16_t)(tip.y - ROW_CHEVRON_SPAN / 2)),
                        GPoint((int16_t)(tip.x - ROW_CHEVRON_REACH), (int16_t)(tip.y + ROW_CHEVRON_SPAN / 2)),
                        color);
}

// Centralized numeral-vs-glyph-column fit -- called from INSIDE
// render_row() itself (below) so every caller (HOME's XLARGE/LARGE/MEDIUM/
// SMALL count tiers, named Library preset rows, unnamed Library preset
// rows, the ringing list) is protected uniformly, regardless of which font
// tier it originally asked for. Ladder is largest->smallest, matching every
// numeral font any render_row() caller passes today (`spec->number_font`).
//
// `preferred` is measured FIRST, unconditionally, before any ladder search
// -- if it already fits, it is returned immediately, regardless of whether
// it happens to be one of the four ladder rungs. This matters because the
// function must never silently UPSIZE a smaller, already-fitting,
// unrecognized font (defaulting to the largest rung when `preferred` isn't
// found in the ladder would do exactly that, up to 42px) -- that would
// contradict its own "never upsize past what the caller asked for"
// contract. (No current caller triggers this: every render_row() caller
// today passes one of these four fonts, but `RenderRowSpec.number_font` is
// a public plain `GFont`, not an enum restricted to this ladder, so a
// future caller could.) Only once `preferred` is confirmed not to fit does
// the ladder search begin -- starting one rung past `preferred` if it was
// found in the ladder (already measured above, no need to re-check the
// same font), or from the top if it's some other, unrecognized font (the
// only reasonable default once we know it doesn't fit). The final rung
// (GOTHIC_18_BOLD, HOME's own SMALL tier) is always returned if nothing
// smaller fits, since there is no smaller numeral font left to try.
static GFont fit_row_number_font(const char *number, GFont preferred, int16_t max_w, int16_t h) {
  GSize preferred_size = graphics_text_layout_get_content_size(
      number, preferred, GRect(0, 0, 400, h), GTextOverflowModeFill, GTextAlignmentLeft);
  if (preferred_size.w <= max_w) return preferred;

  GFont ladder[] = {
      fonts_adapter_numeral_42(),
      fonts_adapter_numeral_32(),
      fonts_get_system_font(FONT_KEY_LECO_20_BOLD_NUMBERS),
      fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
  };
  const int len = (int)(sizeof(ladder) / sizeof(ladder[0]));
  int start = 0;
  for (int i = 0; i < len; i++) {
    if (ladder[i] == preferred) {
      start = i + 1;  // already measured above and confirmed NOT to fit
      break;
    }
  }
  for (int i = start; i < len; i++) {
    GSize size = graphics_text_layout_get_content_size(number, ladder[i], GRect(0, 0, 400, h),
                                                        GTextOverflowModeFill, GTextAlignmentLeft);
    if (size.w <= max_w || i == len - 1) return ladder[i];
  }
  return ladder[len - 1];  // preferred unrecognized + didn't fit + ladder exhausted
}

void render_row(GContext *ctx, GRect cell_bounds, const RenderRowSpec *spec) {
  int16_t ox = cell_bounds.origin.x;
  int16_t oy = cell_bounds.origin.y;
  int16_t h = cell_bounds.size.h;
  bool cursor = spec->highlighted;

  GRect pill = GRect((int16_t)(ox + RENDER_MARGIN_X), (int16_t)(oy + ROW_PILL_PAD_Y),
                      RENDER_CONTENT_W, (int16_t)(h - 2 * ROW_PILL_PAD_Y));

  if (cursor) {
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_rect(ctx, pill, ROW_PILL_RADIUS, GCornersAll);
  } else {
    graphics_context_set_stroke_color(ctx, RENDER_COLOR_ROW_HAIRLINE);
    graphics_context_set_stroke_width(ctx, 1);
    graphics_draw_line(ctx, GPoint(pill.origin.x, (int16_t)(oy + h - 1)),
                        GPoint((int16_t)(pill.origin.x + pill.size.w), (int16_t)(oy + h - 1)));
  }

  GPoint glyph_c = GPoint((int16_t)(pill.origin.x + ROW_PILL_PAD_X + ROW_GLYPH_RADIUS),
                           (int16_t)(oy + h / 2));
  // The ringing list's cursor row draws its glyph white too (legibility fix
  // -- see RenderRowSpec's own doc on cursor_value_white).
  bool white_value = cursor && spec->cursor_value_white;
  // ONE background-keyed tier for EVERY kind-colored element in this row --
  // glyph, value/number, draining/segmented bar -- so they never mismatch
  // hue-lightness within the same row (previously the glyph always stayed
  // spec->kind_color/bright while the number alone darkened, a visible
  // "icon and time are different colors" split on a QUIET/paper row).
  // white_value (ringing's cursor
  // override) still wins outright; otherwise a CURSOR (black pill) row uses
  // the BRIGHT tier (spec->kind_color, safe-on-black); a QUIET (paper) row
  // uses the DEEP tier (spec->value_color, the safe-on-white variant).
  // Computed once, shared by the glyph below, both the number_left_align and
  // ordinary number branches further down, and the draining/segmented bar.
  GColor accent = white_value ? GColorWhite : (cursor ? spec->kind_color : spec->value_color);
  render_kind_glyph(ctx, glyph_c, ROW_GLYPH_RADIUS, accent, spec->kind);

  // The "Composed" Library preset rows: the duration moves into the label's
  // own slot (left-aligned @ROW_LABEL_X) and a trailing chevron takes the
  // number's usual right-aligned spot -- kills the "lonely row" void (glyph
  // .... void .... number) these rows had. `label`/`show_bar`/`number_prefix`
  // are never set alongside this by any caller today (presets have no
  // label, are never draining, never show a caret) so this branch does not
  // need to reconcile with them.
  //
  // This branch used to measure/draw `spec->number_font` directly and
  // RETURN before ever reaching fit_row_number_font() (below, in the
  // ordinary right-aligned branch) -- an unnamed 10:00:00 preset bypassed
  // the centralized invariant entirely and its 32px numeral, drawn
  // left-aligned from ROW_LABEL_X, ran into the trailing chevron's own
  // reserved column. Fixed by running the SAME fit_row_number_font() here
  // too, with the available width computed for THIS layout: ROW_NUMBER_MAX_W
  // (the row's overall glyph-safe budget) minus the chevron's own reserved
  // column when trailing_glyph is set (no
  // caller sets number_left_align without trailing_glyph today, but this is
  // written generically rather than assuming that always holds).
  if (spec->number_left_align) {
    int16_t left_align_max_w = ROW_NUMBER_MAX_W;
    if (spec->trailing_glyph) {
      left_align_max_w = (int16_t)(left_align_max_w - ROW_CHEVRON_RESERVE_W);
    }
    GFont number_font = fit_row_number_font(spec->number, spec->number_font, left_align_max_w, h);
    GSize num_size = graphics_text_layout_get_content_size(
        spec->number, number_font, GRect(0, 0, RENDER_CONTENT_W, h), GTextOverflowModeFill,
        GTextAlignmentLeft);
    int16_t number_y = (int16_t)(oy + (h - num_size.h) / 2);
    GRect number_rect = GRect((int16_t)(ox + ROW_LABEL_X), number_y, (int16_t)(num_size.w + 2), num_size.h);
    graphics_context_set_text_color(ctx, accent);
    graphics_draw_text(ctx, spec->number, number_font, number_rect, GTextOverflowModeFill,
                        GTextAlignmentLeft, NULL);

    if (spec->trailing_glyph) {
      // Same right-edge inset as the ordinary right-aligned number
      // (ROW_PILL_PAD_X) -- the chevron sits exactly where that number would
      // have ended, keeping this row on the same right-margin rhythm as
      // every other row.
      int16_t tip_x = (int16_t)(pill.origin.x + pill.size.w - ROW_PILL_PAD_X);
      GColor chevron_color = cursor ? GColorWhite : RENDER_COLOR_LABEL;
      render_chevron_glyph(ctx, GPoint(tip_x, (int16_t)(oy + h / 2)), chevron_color);
    }
    return;
  }

  // Label + number both VERTICALLY CENTERED within the row -- previously
  // top-anchored (Pebble's
  // graphics_draw_text does not itself center a single line inside a tall
  // box), which read as "label hugs the row top, number sits lower," not
  // one coherent line.
  //
  // Number geometry is computed FIRST (before the label) so the label can
  // reserve exactly the space actually left before it -- the 42/56px
  // numeral rasters are noticeably wider than the row's old fixed 90px
  // label allowance, which let the number overlap mid-word into "Countdown"
  // once the font grew.
  int16_t number_right = (int16_t)(pill.origin.x + pill.size.w - ROW_PILL_PAD_X);
  // fit_row_number_font() (above) picks whichever font in its ladder both
  // (a) is no larger than the caller's own spec->number_font and (b) keeps
  // the number's natural width within ROW_NUMBER_MAX_W (the width left after
  // the glyph column) -- applied HERE, unconditionally, for every
  // render_row() caller, not just the one HOME count-tier this was
  // originally special-cased for. A number that already fits at the
  // caller's own font is returned unchanged (verified: the ladder search
  // starts AT that font).
  GFont number_font = fit_row_number_font(spec->number, spec->number_font, ROW_NUMBER_MAX_W, h);
  GSize num_size = graphics_text_layout_get_content_size(
      spec->number, number_font, GRect(0, 0, RENDER_CONTENT_W, h), GTextOverflowModeFill,
      GTextAlignmentLeft);
  int16_t number_left = (int16_t)(number_right - num_size.w);
  int16_t number_y = (int16_t)(oy + (h - num_size.h) / 2);

  // These numeral fonts sit low in their content-size box (asymmetric
  // ascent/descent), so box-centering reads bottom-heavy. Nudge up by a
  // fraction of the box height to optically center the ink. Divisor tuned
  // empirically (2-timer stopwatch measurement: 17px above/6px below in a
  // 46px cell, 34px box).
  number_y = (int16_t)(number_y - num_size.h / 8);

  // Keep the number clear of the bottom-anchored draining bar. Box-
  // centering (above) overlaps the bar at more than just the XLARGE rung:
  // pixel measurement shows the clamp also actively binds at SMALL/compact
  // (RENDER_ROW_H_COMPACT's shorter row height leaves the same kind of
  // shortfall as XLARGE's taller numeral does), landing the gap exactly at
  // ROW_NUMBER_BAR_GAP in both cases -- the signature of the clamp doing
  // real work, not a no-op. Only LARGE/MEDIUM's larger row height leaves
  // enough slack that box-centering alone already clears the bar (measured
  // unclamped gap there, e.g. ~10px vs the ~3-4px floor at SMALL/XLARGE).
  // Written generically (gated on spec->show_bar, not per-rung), so it
  // correctly binds wherever the geometry needs it -- do not special-case
  // or remove this for "rungs that don't need it" without re-measuring.
  if (spec->show_bar) {
    int16_t bar_top = (int16_t)(oy + h - ROW_BAR_H - ROW_BAR_BOTTOM_PAD);
    int16_t max_bottom = (int16_t)(bar_top - ROW_NUMBER_BAR_GAP);
    if (number_y + num_size.h > max_bottom) {
      number_y = (int16_t)(max_bottom - num_size.h);
    }
  }

  GRect number_rect = GRect(number_left, number_y, (int16_t)(num_size.w + 2), num_size.h);

  // NULL/"" (lib/view's view_row_label()) draws no label text at all --
  // an unnamed timer's identity is glyph + kind colour + magnitude only.
  // Skipping the whole measure-and-draw block (rather than drawing an
  // empty string) also sidesteps feeding
  // graphics_text_layout_get_content_size/graphics_draw_text a NULL
  // pointer.
  // At the XLARGE tier a wide numeral can push number_left before where
  // the label would even start (ox+ROW_LABEL_X) -- an unconditional 20px
  // floor would force a label rect there anyway, which visually collides
  // with the kind glyph/leading digit (a genuinely illegible overlap, not
  // just a short label). There is no honest room for a label in that case,
  // so this skips the whole label draw rather than forcing a floored width
  // that overlaps the number -- fit_row_number_font()'s shrink-to-fit
  // (above) already reclaims most of this width back for the common
  // 10h+long-name case, but this guard is the backstop for whatever
  // collision remains (a sufficiently long name at any numeral width) --
  // fixing the rect, not just the one numeral-width symptom.
  //
  // The label must clear the CARET PREFIX's own left reach too, not just
  // number_left itself -- the ringing list (the only caller passing
  // VIEW_PREFIX_CARET) draws that caret's leftmost pixel at
  // number_left - ROW_PREFIX_RESERVE_W (see that constant's own doc comment),
  // which is FURTHER LEFT than number_left by 12px -- the previous fixed "-6"
  // gap left the caret's own leftmost 6px painting directly over the label's
  // allowed rect for any name long enough to reach it. `cluster_left` is the
  // single left edge the label must clear, covering the caret (when present)
  // plus the number together.
  int16_t cluster_left = number_left;
  if (spec->number_prefix == VIEW_PREFIX_CARET) {
    cluster_left = (int16_t)(cluster_left - ROW_PREFIX_RESERVE_W);
  }
  int16_t label_max_w = (int16_t)(cluster_left - 6 - (ox + ROW_LABEL_X));
  if (spec->label != NULL && spec->label[0] != '\0' && label_max_w >= 12) {
    GFont label_font = fonts_get_system_font(FONT_KEY_GOTHIC_14);
    // Measure the label's natural SINGLE-LINE height using a generously wide
    // box (independent of label_max_w) -- measuring with the narrow clamped
    // width instead would let GTextOverflowModeFill word-WRAP the label
    // across multiple lines ("Countdown" -> "Cou/ntdo/wn") to compute its
    // size, and a draw rect tall enough to fit that wrap renders it wrapped
    // instead of the intended single-line ellipsis (observed with the wider
    // 42px numeral). The draw rect below reuses this single-line height but
    // the CLAMPED width, so GTextOverflowModeTrailingEllipsis has no
    // vertical room to wrap into and truncates on one line instead -- kept
    // as a defensive fallback even though named-timer names are short by
    // construction and aren't expected to ever hit it (see view_row_label's
    // own doc).
    GSize label_size = graphics_text_layout_get_content_size(
        spec->label, label_font, GRect(0, 0, 200, h), GTextOverflowModeFill, GTextAlignmentLeft);
    int16_t label_y = (int16_t)(oy + (h - label_size.h) / 2);
    GColor label_color = cursor ? GColorWhite : RENDER_COLOR_LABEL;
    GRect label_rect = GRect((int16_t)(ox + ROW_LABEL_X), label_y, label_max_w, label_size.h);
    graphics_context_set_text_color(ctx, label_color);
    graphics_draw_text(ctx, spec->label, label_font, label_rect, GTextOverflowModeTrailingEllipsis,
                        GTextAlignmentLeft, NULL);
  }
  // The number stays kind-colored on the cursor row too -- only the LABEL
  // above goes white on the cursor row -- UNLESS cursor_value_white overrides it
  // (ringing list only, see above). This is `accent`, the SAME
  // background-keyed tier the glyph above (and the draining/segmented bar
  // below) also use -- see this function's own top-of-body comment on
  // `accent` for the full rationale.
  graphics_context_set_text_color(ctx, accent);
  graphics_draw_text(ctx, spec->number, number_font, number_rect, GTextOverflowModeFill,
                      GTextAlignmentLeft, NULL);

  if (spec->number_prefix == VIEW_PREFIX_CARET) {
    render_row_prefix(
        ctx, GPoint((int16_t)(number_left - ROW_PREFIX_ANCHOR_OFFSET), (int16_t)(oy + h / 2)), accent);
  }

  if (spec->show_bar) {
    // Bottom-anchored within the row's OWN height rather than a flat offset
    // tuned for RENDER_ROW_H alone -- the busy/scroll rung now uses a
    // shorter RENDER_ROW_H_COMPACT row, and a fixed 36px offset would place
    // the bar outside a 32px-tall row.
    int16_t bar_y = (int16_t)(oy + h - ROW_BAR_H - ROW_BAR_BOTTOM_PAD);
    GRect bar_rect = GRect(number_left, bar_y, num_size.w, ROW_BAR_H);
    GColor track = cursor ? GColorDarkGray : RENDER_COLOR_ROW_HAIRLINE;
    // This row's draining-bar radius is 2 (not the hero draining
    // bar's 3). The bar fill is `accent` too (was unconditionally
    // spec->kind_color/bright before) -- same background-keyed tier as the
    // glyph/number above.
    render_bar(ctx, bar_rect, spec->bar_fraction_pm, accent, track, 2);
  }
}

// --- scroll indicator ---------------------------------------------------

void render_scroll_indicator(GContext *ctx, GRect bounds, int total_rows, int visible_rows,
                              int selected_row) {
  if (total_rows <= visible_rows) return;

  int track_h = bounds.size.h;
  graphics_context_set_fill_color(ctx, RENDER_COLOR_ROW_HAIRLINE);
  graphics_fill_rect(ctx, GRect(bounds.origin.x, bounds.origin.y, bounds.size.w, (int16_t)track_h),
                      1, GCornersAll);

  int thumb_h = track_h * visible_rows / total_rows;
  if (thumb_h < 8) thumb_h = 8;
  int max_thumb_y = track_h - thumb_h;
  int thumb_y = (total_rows > 1) ? max_thumb_y * selected_row / (total_rows - 1) : 0;
  if (thumb_y > max_thumb_y) thumb_y = max_thumb_y;
  if (thumb_y < 0) thumb_y = 0;

  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx,
                      GRect(bounds.origin.x, (int16_t)(bounds.origin.y + thumb_y), bounds.size.w,
                            (int16_t)thumb_h),
                      1, GCornersAll);
}

// --- ACTIONS/options/LAPS menu-cell primitive -------------------------------

#define MENU_ROW_PILL_INSET 8
#define MENU_ROW_PILL_RADIUS 8

void render_menu_row(GContext *ctx, GRect cell_bounds, bool highlighted, const char *label) {
  int16_t ox = cell_bounds.origin.x;
  int16_t oy = cell_bounds.origin.y;
  int16_t w = cell_bounds.size.w;
  int16_t h = cell_bounds.size.h;

  GRect pill = GRect((int16_t)(ox + MENU_ROW_PILL_INSET), (int16_t)(oy + 2),
                      (int16_t)(w - 2 * MENU_ROW_PILL_INSET), (int16_t)(h - 4));
  if (highlighted) {
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_rect(ctx, pill, MENU_ROW_PILL_RADIUS, GCornersAll);
  } else {
    graphics_context_set_stroke_color(ctx, RENDER_COLOR_ROW_HAIRLINE);
    graphics_context_set_stroke_width(ctx, 1);
    graphics_draw_line(ctx, GPoint(pill.origin.x, (int16_t)(oy + h - 1)),
                        GPoint((int16_t)(pill.origin.x + pill.size.w), (int16_t)(oy + h - 1)));
  }

  // VERTICALLY CENTER the label within `cell_bounds` -- previously
  // top-anchored the same way
  // render_row's label was (Pebble's graphics_draw_text does not itself
  // center a line within a tall box).
  GFont font = fonts_get_system_font(highlighted ? FONT_KEY_GOTHIC_18_BOLD : FONT_KEY_GOTHIC_18);
  GSize text_size = graphics_text_layout_get_content_size(
      label, font, GRect(0, 0, (int16_t)(pill.size.w - 16), h), GTextOverflowModeFill,
      GTextAlignmentLeft);
  int16_t text_y = (int16_t)(oy + (h - text_size.h) / 2);
  GRect text_rect =
      GRect((int16_t)(pill.origin.x + 10), text_y, (int16_t)(pill.size.w - 16), text_size.h);
  graphics_context_set_text_color(ctx, highlighted ? GColorWhite : RENDER_COLOR_LABEL);
  graphics_draw_text(ctx, label, font, text_rect, GTextOverflowModeTrailingEllipsis,
                      GTextAlignmentLeft, NULL);
}

void render_menu_row_kv(GContext *ctx, GRect cell_bounds, bool highlighted, const char *left,
                         const char *right) {
  int16_t ox = cell_bounds.origin.x;
  int16_t oy = cell_bounds.origin.y;
  int16_t w = cell_bounds.size.w;
  int16_t h = cell_bounds.size.h;

  GRect pill = GRect((int16_t)(ox + MENU_ROW_PILL_INSET), (int16_t)(oy + 2),
                      (int16_t)(w - 2 * MENU_ROW_PILL_INSET), (int16_t)(h - 4));
  if (highlighted) {
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_rect(ctx, pill, MENU_ROW_PILL_RADIUS, GCornersAll);
  } else {
    graphics_context_set_stroke_color(ctx, RENDER_COLOR_ROW_HAIRLINE);
    graphics_context_set_stroke_width(ctx, 1);
    graphics_draw_line(ctx, GPoint(pill.origin.x, (int16_t)(oy + h - 1)),
                        GPoint((int16_t)(pill.origin.x + pill.size.w), (int16_t)(oy + h - 1)));
  }

  GColor color = highlighted ? GColorWhite : RENDER_COLOR_LABEL;
  GFont font = fonts_get_system_font(highlighted ? FONT_KEY_GOTHIC_18_BOLD : FONT_KEY_GOTHIC_18);
  graphics_context_set_text_color(ctx, color);
  // Same vertical-centering fix as render_menu_row() above -- both columns
  // share one font, so one measured height centers both.
  GSize text_size = graphics_text_layout_get_content_size(
      left, font, GRect(0, 0, (int16_t)(pill.size.w / 2), h), GTextOverflowModeFill,
      GTextAlignmentLeft);
  int16_t text_y = (int16_t)(oy + (h - text_size.h) / 2);
  GRect left_rect =
      GRect((int16_t)(pill.origin.x + 10), text_y, (int16_t)(pill.size.w / 2), text_size.h);
  graphics_draw_text(ctx, left, font, left_rect, GTextOverflowModeTrailingEllipsis,
                      GTextAlignmentLeft, NULL);
  GRect right_rect = GRect((int16_t)(pill.origin.x + pill.size.w / 2), text_y,
                            (int16_t)(pill.size.w / 2 - 10), text_size.h);
  graphics_draw_text(ctx, right, font, right_rect, GTextOverflowModeTrailingEllipsis,
                      GTextAlignmentRight, NULL);
}

// --- "Composed" action-row rail ---------------------------------------------
//
// Shared geometry with render_row's own content-row pill (RENDER_MARGIN_X
// inset, ROW_PILL_RADIUS/PAD_X/PAD_Y) -- action rows and content rows share
// the same padding-scale constants ("ONE consistent sizing system"), not
// just matching numbers kept in sync by hand. Deliberately a SEPARATE
// geometry from render_menu_row's own 8px/radius-8 rail (ACTIONS/LAPS/
// alarm-options/preset-actions sheet) -- see render_action_row's own doc
// comment in render.h for why that separation is the point, not an
// oversight.
// The mic-glyph geometry a `with_mic_glyph` row reserves inside the pill's
// own left padding -- 12px glyph-grid box (matching render_mic_glyph/
// render_stack_glyph's own 12px convention) + an 8px gap before the label
// (matching screen_home.c's LIB_GROUP_GAP, the same glyph-to-label spacing
// used elsewhere in this app).
#define ACTION_ROW_GLYPH_W 12
#define ACTION_ROW_GLYPH_GAP 8

static void render_action_row_impl(GContext *ctx, GRect cell_bounds, bool highlighted,
                                    const char *label, GColor quiet_text_color, GColor pill_fill,
                                    bool show_hairline, bool with_mic_glyph) {
  int16_t ox = cell_bounds.origin.x;
  int16_t oy = cell_bounds.origin.y;
  int16_t h = cell_bounds.size.h;

  GRect pill = GRect((int16_t)(ox + RENDER_MARGIN_X), (int16_t)(oy + ROW_PILL_PAD_Y), RENDER_CONTENT_W,
                      (int16_t)(h - 2 * ROW_PILL_PAD_Y));

  if (highlighted) {
    graphics_context_set_fill_color(ctx, pill_fill);
    graphics_fill_rect(ctx, pill, ROW_PILL_RADIUS, GCornersAll);
  } else if (show_hairline) {
    graphics_context_set_stroke_color(ctx, RENDER_COLOR_ROW_HAIRLINE);
    graphics_context_set_stroke_width(ctx, 1);
    graphics_draw_line(ctx, GPoint(pill.origin.x, (int16_t)(oy + h - 1)),
                        GPoint((int16_t)(pill.origin.x + pill.size.w), (int16_t)(oy + h - 1)));
  }

  GColor content_color = highlighted ? GColorWhite : quiet_text_color;
  int16_t text_x = (int16_t)(pill.origin.x + ROW_PILL_PAD_X);
  int16_t text_w = (int16_t)(pill.size.w - 2 * ROW_PILL_PAD_X);
  if (with_mic_glyph) {
    render_mic_glyph(ctx, GPoint((int16_t)(text_x + ACTION_ROW_GLYPH_W / 2), (int16_t)(oy + h / 2)),
                      content_color);
    text_x = (int16_t)(text_x + ACTION_ROW_GLYPH_W + ACTION_ROW_GLYPH_GAP);
    text_w = (int16_t)(text_w - ACTION_ROW_GLYPH_W - ACTION_ROW_GLYPH_GAP);
  }

  GFont font = fonts_get_system_font(highlighted ? FONT_KEY_GOTHIC_18_BOLD : FONT_KEY_GOTHIC_18);
  GSize text_size = graphics_text_layout_get_content_size(label, font, GRect(0, 0, text_w, h),
                                                           GTextOverflowModeFill, GTextAlignmentLeft);
  int16_t text_y = (int16_t)(oy + (h - text_size.h) / 2);
  GRect text_rect = GRect(text_x, text_y, text_w, text_size.h);
  graphics_context_set_text_color(ctx, content_color);
  graphics_draw_text(ctx, label, font, text_rect, GTextOverflowModeTrailingEllipsis,
                      GTextAlignmentLeft, NULL);
}

void render_action_row(GContext *ctx, GRect cell_bounds, bool highlighted, const char *label,
                        bool show_hairline) {
  render_action_row_impl(ctx, cell_bounds, highlighted, label, RENDER_COLOR_LABEL, GColorBlack,
                          show_hairline, false);
}

void render_action_row_disabled(GContext *ctx, GRect cell_bounds, bool highlighted,
                                 const char *quiet_label, const char *highlighted_label,
                                 bool show_hairline) {
  // The one deliberate delta vs render_action_row: dark GRAY, not black --
  // reads as "present but not the everyday create action" (unchanged
  // rationale from the pre-unification is_max pill).
  render_action_row_impl(ctx, cell_bounds, highlighted, highlighted ? highlighted_label : quiet_label,
                          RENDER_COLOR_MUTED_TEXT, GColorDarkGray, show_hairline, false);
}

void render_action_row_mic(GContext *ctx, GRect cell_bounds, bool highlighted, const char *label,
                            bool show_hairline) {
  render_action_row_impl(ctx, cell_bounds, highlighted, label, RENDER_COLOR_LABEL, GColorBlack,
                          show_hairline, true);
}

void render_group_divider(GContext *ctx, GRect cell_bounds) {
  int16_t y = (int16_t)(cell_bounds.origin.y + 3);
  graphics_context_set_stroke_color(ctx, RENDER_COLOR_ROW_HAIRLINE);
  graphics_context_set_stroke_width(ctx, 1);
  graphics_draw_line(ctx, GPoint((int16_t)(cell_bounds.origin.x + RENDER_MARGIN_X), y),
                      GPoint((int16_t)(cell_bounds.origin.x + RENDER_MARGIN_X + RENDER_CONTENT_W), y));
}

// See render.h's doc comment (RENDER_GOTHIC_CHIP_TEXT_NUDGE_Y) for the
// measured asymmetry this corrects for.
int16_t render_gothic_text_y_for_glyph_center(int16_t glyph_center_y, int16_t text_h) {
  return (int16_t)(glyph_center_y - text_h / 2 - RENDER_GOTHIC_CHIP_TEXT_NUDGE_Y);
}

// --- composed-group idiom (extracted from screen_home.c) --------------------

// Measured geometry, byte-for-byte -- see render.h's own doc comment on
// render_composed_group for the reuse rationale.
#define COMPOSED_GLYPH_W 12  // render_stack_glyph's/render_mic_glyph's own width
#define COMPOSED_GLYPH_H 12
#define COMPOSED_GROUP_GAP 8       // gap between the glyph and the label
#define COMPOSED_CHIP_PAD_X 10     // symmetric cursor-chip padding
#define COMPOSED_CHIP_PAD_Y 5
#define COMPOSED_DIVIDER_W 60      // the "seat" divider's width

void render_composed_group(GContext *ctx, GRect cell_bounds, bool highlighted,
                            void (*draw_glyph)(GContext *, GPoint, GColor), const char *label) {
  int16_t w = cell_bounds.size.w;
  int16_t center_y = (int16_t)(cell_bounds.size.h / 2);

  GFont font = fonts_get_system_font(highlighted ? FONT_KEY_GOTHIC_18_BOLD : FONT_KEY_GOTHIC_18);
  GSize text_size = graphics_text_layout_get_content_size(label, font, GRect(0, 0, 150, 30),
                                                            GTextOverflowModeFill, GTextAlignmentLeft);
  int16_t content_h = text_size.h > COMPOSED_GLYPH_H ? text_size.h : COMPOSED_GLYPH_H;
  int16_t content_w = (int16_t)(COMPOSED_GLYPH_W + COMPOSED_GROUP_GAP + text_size.w);
  int16_t group_left = (int16_t)(w / 2 - content_w / 2);

  if (highlighted) {
    // Symmetric padding on all four sides, radius = height/2 --
    // render_overrun_chip's own "pill sized to content" capsule model,
    // applied here to a glyph+label group instead of a plain string.
    GRect chip = GRect((int16_t)(group_left - COMPOSED_CHIP_PAD_X),
                        (int16_t)(center_y - content_h / 2 - COMPOSED_CHIP_PAD_Y),
                        (int16_t)(content_w + 2 * COMPOSED_CHIP_PAD_X),
                        (int16_t)(content_h + 2 * COMPOSED_CHIP_PAD_Y));
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_rect(ctx, chip, (int16_t)(chip.size.h / 2), GCornersAll);
  }

  GColor content_color = highlighted ? GColorWhite : RENDER_COLOR_LABEL;
  draw_glyph(ctx, GPoint((int16_t)(group_left + COMPOSED_GLYPH_W / 2), center_y), content_color);

  // Accounts for font cap-height vs box, glyph baseline --
  // render_gothic_text_y_for_glyph_center's own measured nudge.
  GRect text_rect = GRect((int16_t)(group_left + COMPOSED_GLYPH_W + COMPOSED_GROUP_GAP),
                           render_gothic_text_y_for_glyph_center(center_y, text_size.h),
                           (int16_t)(text_size.w + 2), text_size.h);
  graphics_context_set_text_color(ctx, content_color);
  graphics_draw_text(ctx, label, font, text_rect, GTextOverflowModeFill, GTextAlignmentLeft, NULL);
}

void render_composed_seat_divider(GContext *ctx, GRect cell_bounds) {
  int16_t y = cell_bounds.origin.y;
  int16_t center_x = (int16_t)(cell_bounds.origin.x + cell_bounds.size.w / 2);
  graphics_context_set_stroke_color(ctx, RENDER_COLOR_ROW_HAIRLINE);
  graphics_context_set_stroke_width(ctx, 1);
  graphics_draw_line(ctx, GPoint((int16_t)(center_x - COMPOSED_DIVIDER_W / 2), y),
                      GPoint((int16_t)(center_x + COMPOSED_DIVIDER_W / 2), y));
}

// --- alarm header band -------------------------------------------------------

// The status line used to reserve a flat 74px for the clock regardless of
// its actual rendered width (~40-50px) -- fine for the
// short one-off status ("TIME'S UP - one-off") but not the repeat variant's
// longer "CHECK - buzzes again in N:00", which truncated. Measure the
// clock's actual width instead (same "measure, don't guess" pattern
// hold_footer_more_col_w() already uses for "more") to reclaim the unused
// margin for the status text.
static int16_t alarm_header_clock_col_w(bool is_24h) {
  time_t now = time(NULL);
  struct tm *tm_now = localtime(&now);
  char clock_text[8];
  if (tm_now != NULL) {
    format_clock(clock_text, sizeof clock_text, tm_now->tm_hour, tm_now->tm_min, is_24h);
  } else {
    format_clock(clock_text, sizeof clock_text, 0, 0, is_24h);
  }
  GSize size = graphics_text_layout_get_content_size(
      clock_text, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD), GRect(0, 0, 80, 20),
      GTextOverflowModeFill, GTextAlignmentLeft);
  return (int16_t)(size.w + CLOCK_PAD_X + 6);  // clock's own right pad + a small gap
}

void render_alarm_header_band(GContext *ctx, GRect bounds, GColor band_color,
                               const char *status_line, const char *title, GFont title_font,
                               bool is_24h) {
  GRect band = GRect(bounds.origin.x, bounds.origin.y, bounds.size.w, RENDER_ALARM_BAND_H);
  graphics_context_set_fill_color(ctx, band_color);
  graphics_fill_rect(ctx, band, 0, GCornerNone);

  int16_t clock_col_w = alarm_header_clock_col_w(is_24h);
  GRect status_rect =
      GRect(RENDER_MARGIN_X, 8, (int16_t)(bounds.size.w - RENDER_MARGIN_X - clock_col_w), 16);
  graphics_context_set_text_color(ctx, RENDER_COLOR_STRIP_TEXT);
  graphics_draw_text(ctx, status_line, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD), status_rect,
                      GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  render_clock_colored(ctx, bounds, is_24h, RENDER_COLOR_RINGING_TEXT);

  // Caller picks the title font -- the one-off (~30px target) and repeat
  // (~26px target) variants are visually distinguishable
  // system-font sizes (GOTHIC_28_BOLD / GOTHIC_24_BOLD), not one shared size
  // for both.
  GRect title_rect = GRect(RENDER_MARGIN_X, 20, bounds.size.w - 2 * RENDER_MARGIN_X, 32);
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, title, title_font, title_rect, GTextOverflowModeTrailingEllipsis,
                      GTextAlignmentLeft, NULL);
}

// --- shared MenuLayer-window builder -----------------------------------------
// See render.h's own doc comment on RenderMenuScreenSpec for what this does
// and does not factor.

RenderMenuScreen render_build_menu_screen(const RenderMenuScreenSpec *spec) {
  RenderMenuScreen screen = {0};

  screen.window = window_create();
  window_set_background_color(screen.window, spec->window_bg);
#ifdef GALLERY_MODE
  // Screen gallery (inner loop, src/c/gallery.c) -- every converted screen
  // used to repeat this exact #ifdef at its own create() call site. Never
  // compiled into a release build.
  window_set_click_config_provider(screen.window, gallery_click_config_provider);
#else
  window_set_click_config_provider(screen.window, spec->click_config_provider);
#endif
  window_set_window_handlers(screen.window, spec->window_handlers);

  Layer *root = window_get_root_layer(screen.window);
  GRect bounds = layer_get_bounds(root);
  int16_t header_h = spec->header_height;

  if (header_h > 0) {
    screen.header = layer_create(GRect(0, 0, bounds.size.w, header_h));
    layer_set_update_proc(screen.header, spec->header_update_proc);
    layer_add_child(root, screen.header);
  }

  screen.menu = menu_layer_create(GRect(0, header_h, bounds.size.w, bounds.size.h - header_h));
  if (spec->pad_bottom_disable) {
    menu_layer_pad_bottom_enable(screen.menu, false);
  }
  if (spec->set_normal_colors) {
    menu_layer_set_normal_colors(screen.menu, spec->normal_bg, spec->normal_fg);
  }
  // Every converted screen set this to the same pair -- see RenderMenuScreenSpec's
  // own doc comment (render.h).
  menu_layer_set_highlight_colors(screen.menu, RENDER_COLOR_PAPER, GColorBlack);
  menu_layer_set_callbacks(screen.menu, NULL,
                            (MenuLayerCallbacks){
                                .get_num_rows = spec->get_num_rows,
                                .get_cell_height = spec->get_cell_height,
                                .draw_row = spec->draw_row,
                            });
  layer_add_child(root, menu_layer_get_layer(screen.menu));

  if (spec->with_scroll_indicator) {
    screen.scroll_indicator = layer_create(
        GRect(bounds.size.w - RENDER_SCROLL_INDICATOR_WIDTH - RENDER_SCROLL_INDICATOR_MARGIN,
              header_h, RENDER_SCROLL_INDICATOR_WIDTH, bounds.size.h - header_h));
    layer_set_update_proc(screen.scroll_indicator, spec->scroll_indicator_update_proc);
    layer_add_child(root, screen.scroll_indicator);
  }

  return screen;
}

void render_destroy_menu_screen(const RenderMenuScreen *screen) {
  if (screen->scroll_indicator != NULL) layer_destroy(screen->scroll_indicator);
  menu_layer_destroy(screen->menu);
  if (screen->header != NULL) layer_destroy(screen->header);
  window_destroy(screen->window);
}
