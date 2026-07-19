// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

#include "screen_actions.h"

#include <stdio.h>

#include "app_context.h"
#include "dictation_am.h"
#include "lib/format.h"
#include "lib/store.h"
#include "lib/timer.h"
#include "nav.h"
#include "persist_adapter.h"
#include "render.h"
#include "testlog_am.h"
#include "vibes_adapter.h"
#include "wakeup_adapter.h"
#ifdef GALLERY_MODE
#include "gallery.h"
#endif

// Countdown action rows. The repeat-toggle row switches the timer to a
// repeating timer via timer_toggle_repeat. Rename sits directly above the
// destructive Delete row, matching the placement used by every other timer
// kind's action row set below.
#define ROW_PAUSE_RESUME 0
#define ROW_REPEAT_TOGGLE 1
#define ROW_ADD_MINUTE 2
#define ROW_RESTART 3
#define ROW_RENAME 4
#define ROW_DELETE 5
#define NUM_ROWS 6

// Stopwatch action rows: Pause/Resume, "Laps (N)" (opens the full lap
// history), Reset to 0, Rename, Lap, Delete. Adding a lap is also available
// as a hero-screen gesture (long-press Up); duplicating it here as a menu
// row is a deliberate choice for discoverability. Row order convention:
// primary/unique actions come first, a gesture-duplicate row sits near the
// end, and Delete is always last. Pause/Resume keeps the top slot as a
// long-standing exception to this ordering, consistent across every timer
// kind.
#define STOPWATCH_ROW_PAUSE_RESUME 0
#define STOPWATCH_ROW_LAPS 1
#define STOPWATCH_ROW_RESET 2
#define STOPWATCH_ROW_RENAME 3
#define STOPWATCH_ROW_LAP 4
#define STOPWATCH_ROW_DELETE 5
#define STOPWATCH_NUM_ROWS 6

// Repeat action rows: Pause/Resume, Repeat toggle, Edit interval, Rename,
// Restart, Delete. Deliberately not the same row set as countdown: there is
// no "+1:00" here, since a repeat timer's interval doesn't grow the way a
// countdown's remaining duration does. Edit interval reuses the duration-set
// spinner (nav_go_edit_interval); the actual mutation happens on that
// screen's own commit, not here. Restart reuses timer_restart verbatim
// (cycles reset to 0, a fresh start, distinct from "Keep going", which
// increments cycles) and matches the hero screen's own long-press-Up
// gesture; duplicating a gesture as a menu row here is the same
// discoverability choice as the stopwatch's Lap row. Row order: Pause/Resume
// keeps its top slot; Repeat toggle, Edit interval, and Rename are unique,
// non-gesture actions and stay ahead; Restart, the gesture duplicate, sits
// just above Delete.
#define REPEAT_ROW_PAUSE_RESUME 0
#define REPEAT_ROW_TOGGLE 1
#define REPEAT_ROW_EDIT_INTERVAL 2
#define REPEAT_ROW_RENAME 3
#define REPEAT_ROW_RESTART 4
#define REPEAT_ROW_DELETE 5
#define REPEAT_NUM_ROWS 6

// Explicit fixed cell height -- see RENDER_MENU_ROW_H's doc comment
// (render.h) for why this can no longer rely on the SDK's own
// un-overridden default (measured taller than intended, and the
// cause of the ALARM-options 4th row sitting below the fold).
#define SCROLL_INDICATOR_WIDTH 3
#define SCROLL_INDICATOR_MARGIN 3

// Held-Down carry-through: a Down hold begun on a HERO carries through
// into ACTIONS and keeps auto-scrolling down (use case: hold Down to zip
// to Delete at the bottom). See screen_actions_arm_carry_scroll's/
// window_appear's/prv_autoscroll_cb's own comments below for the mechanism;
// screen_actions_cancel_carry_scroll's own comment for the lifecycle
// discipline.
#define CARRY_SCROLL_INITIAL_MS 300
#define CARRY_SCROLL_STEP_MS 100
static bool s_carry_scroll_armed;
static AppTimer *s_autoscroll_timer;

static Window *s_window;
static Layer *s_strip;
static MenuLayer *s_menu;
static Layer *s_scroll_indicator;
static int32_t s_cookie;

// -1 means no pending cursor restore is needed. Starting a Rename does not
// navigate away: the row's handler returns immediately, staying on this
// screen while the system dictation UI is in flight. When that UI closes
// (whether the rename succeeded or was cancelled), it reveals this window
// again and the SDK fires window_appear on it directly, with no explicit
// navigation transition of our own involved. window_appear used to
// unconditionally reset the selected row to 0 on every appearance, which is
// why a cancelled Rename used to leave the cursor off the row it was
// launched from. Recording the launching row here (set only by the Rename
// branches below) and restoring it on the very next appearance, then
// clearing it, fixes that without disturbing the other case this same reset
// serves: a genuine fresh push from the hero screen, or a real return from a
// child screen such as the laps list, both of which still land on row 0 as
// before, since screen_actions_set_cookie() clears this back to -1 first on
// every one of those paths (never on the dictation-close reveal).
static int s_pending_row = -1;

static void persist_and_replan(void) {
  TimerStore *store = app_context_store();
  persist_adapter_save(store);
  wakeup_adapter_replan(store, time(NULL));
}

static bool actions_is_stopwatch(void) {
  const Timer *t = store_find_const(app_context_store(), s_cookie);
  return t != NULL && t->kind == KIND_STOPWATCH;
}

static bool actions_is_repeat(void) {
  const Timer *t = store_find_const(app_context_store(), s_cookie);
  return t != NULL && t->kind == KIND_REPEAT;
}

static uint16_t get_num_rows(MenuLayer *menu_layer, uint16_t section_index, void *context) {
  (void)menu_layer;
  (void)section_index;
  (void)context;
  if (actions_is_stopwatch()) return STOPWATCH_NUM_ROWS;
  if (actions_is_repeat()) return REPEAT_NUM_ROWS;
  return NUM_ROWS;
}

static int16_t get_cell_height(MenuLayer *menu_layer, MenuIndex *cell_index, void *context) {
  (void)menu_layer;
  (void)cell_index;
  (void)context;
  return RENDER_MENU_ROW_H;
}

static void draw_row(GContext *ctx, const Layer *cell_layer, MenuIndex *cell_index, void *context) {
  (void)context;
  const Timer *t = store_find_const(app_context_store(), s_cookie);
  char title[32] = "";
  if (t != NULL && t->kind == KIND_STOPWATCH) {
    switch (cell_index->row) {
      case STOPWATCH_ROW_PAUSE_RESUME:
        snprintf(title, sizeof title, "%s", t->is_paused ? "Resume" : "Pause");
        break;
      case STOPWATCH_ROW_LAPS:
        snprintf(title, sizeof title, "Laps (%d)", t->lap_count);
        break;
      case STOPWATCH_ROW_RESET:
        snprintf(title, sizeof title, "Reset to 0");
        break;
      case STOPWATCH_ROW_RENAME:
        snprintf(title, sizeof title, "Rename");
        break;
      case STOPWATCH_ROW_LAP:
        snprintf(title, sizeof title, "Lap");
        break;
      case STOPWATCH_ROW_DELETE:
        snprintf(title, sizeof title, "Delete");
        break;
    }
  } else if (t != NULL && t->kind == KIND_REPEAT) {
    switch (cell_index->row) {
      case REPEAT_ROW_PAUSE_RESUME:
        snprintf(title, sizeof title, "%s", t->is_paused ? "Resume" : "Pause");
        break;
      case REPEAT_ROW_TOGGLE:
        snprintf(title, sizeof title, "Repeat: On");
        break;
      case REPEAT_ROW_EDIT_INTERVAL: {
        char interval[16];
        format_hms(interval, sizeof interval, t->duration_s);
        snprintf(title, sizeof title, "Edit interval %s", interval);
        break;
      }
      case REPEAT_ROW_RENAME:
        snprintf(title, sizeof title, "Rename");
        break;
      case REPEAT_ROW_RESTART:
        snprintf(title, sizeof title, "Restart");
        break;
      case REPEAT_ROW_DELETE:
        snprintf(title, sizeof title, "Delete");
        break;
    }
  } else {
    switch (cell_index->row) {
      case ROW_PAUSE_RESUME:
        snprintf(title, sizeof title, "%s", (t != NULL && t->is_paused) ? "Resume" : "Pause");
        break;
      case ROW_REPEAT_TOGGLE:
        // Turns Repeat on -- select_click below now actually calls
        // timer_toggle_repeat.
        snprintf(title, sizeof title, "Repeat: Off");
        break;
      case ROW_ADD_MINUTE:
        snprintf(title, sizeof title, "+1:00");
        break;
      case ROW_RESTART:
        snprintf(title, sizeof title, "Restart");
        break;
      case ROW_RENAME:
        snprintf(title, sizeof title, "Rename");
        break;
      case ROW_DELETE:
        snprintf(title, sizeof title, "Delete");
        break;
    }
  }
  // The shared pill/hairline menu-row primitive -- replaces the SDK's own
  // `menu_cell_basic_draw`, whose highlight is a full-bleed cell fill, same
  // "widen + hairline" fix as HOME's row component.
  render_menu_row(ctx, layer_get_bounds(cell_layer), menu_cell_layer_is_highlighted(cell_layer),
                   title);
}

static void select_click(MenuLayer *menu_layer, MenuIndex *cell_index, void *context) {
  (void)menu_layer;
  (void)context;
  Timer *t = store_find(app_context_store(), s_cookie);
  if (t == NULL) return;
  time_t now = time(NULL);

  if (t->kind == KIND_STOPWATCH) {
    switch (cell_index->row) {
      case STOPWATCH_ROW_PAUSE_RESUME:
        if (t->is_paused) {
          timer_resume(t, now);
        } else {
          timer_pause(t, now);
        }
        persist_and_replan();
        break;
      case STOPWATCH_ROW_LAPS:
        // Opens the laps list -- does NOT fall through to nav_go_hero
        // below.
        nav_go_laps(s_cookie);
        return;
      case STOPWATCH_ROW_RESET:
        // "Reset to 0": timer_restart's contract (started_at=now, zeroed
        // accumulated pause/cycles/laps) is exactly a stopwatch zero-reset --
        // elapsed() reads 0 again on the next redraw.
        store_restart(app_context_store(), t, now);
        persist_and_replan();
        break;
      case STOPWATCH_ROW_RENAME:
        // Starts the rename dictation flow. The eventual transcript, if any,
        // lands via screen_actions_apply_name() below, never inline here,
        // since dictation is asynchronous. Stays on this screen while the
        // attempt is in flight -- does not fall through to nav_go_hero()
        // below. Remember this row so the eventual re-appearance (see
        // s_pending_row's comment) restores the cursor here instead of
        // resetting to row 0.
        s_pending_row = cell_index->row;
        dictation_am_start(DICT_SCOPE_ACTIONS_NAME);
        return;
      case STOPWATCH_ROW_LAP:
        // Catalog-completeness return of the row removed when Lap became a
        // hero-only gesture (Up-hold, sw_add_lap in screen_hero.c) -- same
        // mutation, same testlog shape (lap_count after the add), just
        // reachable from ACTIONS too now.
        if (!store_add_lap(app_context_store(), t, now)) {
          vibes_adapter_reject();
          testlog("lap_refused", "pool");
          return;
        }
        persist_and_replan();
        {
          char data[8];
          snprintf(data, sizeof data, "%d", t->lap_count);
          testlog("lap", data);
        }
        break;
      case STOPWATCH_ROW_DELETE: {
        int32_t cookie = t->cookie;
        store_remove(app_context_store(), cookie);
        persist_and_replan();
        // Mirrors the existing timer:create/persist/lap testlog markers --
        // emitted only once a real ACTIONS Delete has actually executed, so
        // it's a reliable signal that this specific mutation ran (unlike a
        // marker emitted once at app startup, which stays in the log buffer
        // regardless of what happens afterward).
        testlog("delete", "stopwatch");
        nav_go_home();
        return;
      }
    }
  } else if (t->kind == KIND_REPEAT) {
    switch (cell_index->row) {
      case REPEAT_ROW_PAUSE_RESUME:
        if (t->is_paused) {
          timer_resume(t, now);
        } else {
          timer_pause(t, now);
        }
        persist_and_replan();
        break;
      case REPEAT_ROW_TOGGLE:
        timer_toggle_repeat(t, now);  // repeat -> countdown; preserves elapsed clock
        persist_and_replan();
        testlog("repeat", "off");
        break;
      case REPEAT_ROW_EDIT_INTERVAL:
        // Reuses the SET spinner (nav_go_edit_interval) -- the mutation
        // happens on SET's own commit, not here; does NOT fall through to
        // nav_go_hero below (mirrors STOPWATCH_ROW_LAPS's own early return
        // -- a different navigation target).
        nav_go_edit_interval(s_cookie);
        return;
      case REPEAT_ROW_RENAME:
        // Same async Rename arc as every other kind -- stays on ACTIONS.
        // See STOPWATCH_ROW_RENAME's comment above.
        s_pending_row = cell_index->row;
        dictation_am_start(DICT_SCOPE_ACTIONS_NAME);
        return;
      case REPEAT_ROW_RESTART:
        // Reuses timer_restart verbatim (lib/timer.h's doc) -- cycles=0,
        // "begin fresh", the repeat-kind-conditional original_total_s
        // snooze-marker clear included. Same mutation the hero's own new Up-hold gesture
        // (rp_restart, screen_hero.c) invokes -- duplicating it here as a
        // menu row is the locked, wanted convention.
        store_restart(app_context_store(), t, now);
        persist_and_replan();
        testlog("restart", "repeat_actions");
        break;
      case REPEAT_ROW_DELETE: {
        int32_t cookie = t->cookie;
        store_remove(app_context_store(), cookie);
        persist_and_replan();
        testlog("delete", "repeat");
        nav_go_home();
        return;
      }
    }
  } else {
    switch (cell_index->row) {
      case ROW_PAUSE_RESUME:
        if (t->is_paused) {
          timer_resume(t, now);
        } else {
          timer_pause(t, now);
        }
        persist_and_replan();
        break;
      case ROW_REPEAT_TOGGLE:
        timer_toggle_repeat(t, now);  // countdown -> repeat; preserves elapsed clock
        persist_and_replan();
        testlog("repeat", "on");
        break;
      case ROW_ADD_MINUTE:
        timer_add_minute(t);
        persist_and_replan();
        break;
      case ROW_RESTART:
        store_restart(app_context_store(), t, now);
        persist_and_replan();
        break;
      case ROW_RENAME:
        // Same async Rename arc as every other kind -- stays on ACTIONS.
        // See STOPWATCH_ROW_RENAME's comment above.
        s_pending_row = cell_index->row;
        dictation_am_start(DICT_SCOPE_ACTIONS_NAME);
        return;
      case ROW_DELETE: {
        int32_t cookie = t->cookie;
        store_remove(app_context_store(), cookie);
        persist_and_replan();
        testlog("delete", "countdown");
        nav_go_home();
        return;
      }
    }
  }
  nav_go_hero(s_cookie);
}

static void back_click_handler(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
  nav_go_hero(s_cookie);
}

// Do not wire an extra Back handler via scroll_layer_set_callbacks()'s
// click_config_provider hook. menu_layer_init() wires the menu's own
// internal click handling through scroll_layer_set_callbacks(), which
// subscribes Up/Down to real menu-selection navigation and Select to this
// file's own select_click callback. scroll_layer_set_callbacks() does a full
// struct assignment, not a merge, so calling it again to add a Back handler
// silently replaces the menu's own click-config provider with a Back-only
// one. The result: Up/Down fall back to the window-level generic scroll
// handlers (raw content-offset scrolling, not the menu's own selection
// index), which looks like it works because the list still visibly
// scrolls, but Select is never subscribed to anything -- a silent, fully
// reproducible dead button rather than a crash, easy to miss without a
// script that actually presses Select while on this menu.
//
// Fixed by not using menu_layer_set_click_config_onto_window() or
// scroll_layer_set_callbacks() at all. Instead, one single, ordinary,
// non-reentrant window_set_click_config_provider() (wired once, at create
// time, the same as every other window in this app) replicates menu
// navigation using only the public MenuLayer API (menu_layer_set_selected_
// next() for Up/Down, menu_layer_get_selected_index() plus this file's own
// select_click() for Select) plus the extra Back handler -- all four
// buttons configured in one provider call, so there is no second, nested
// click-config setup call for this reentrancy bug to recur.
static void up_click_handler(ClickRecognizerRef recognizer, void *context) {
  (void)context;
  // MenuLayer clamps (no wrap) at row 0, so Up on the top row used to be a
  // silent no-op -- the only way back to the hero screen was Back. Up at the
  // top row now reverses the Down press that opened this menu, mirroring
  // back_click_handler. Only a discrete press at row 0 reverses -- a held or
  // auto-repeating Up landing on row 0 falls through to the
  // menu_layer_set_selected_next() clamp below instead of cancelling out.
  if (menu_layer_get_selected_index(s_menu).row == 0 &&
      !click_recognizer_is_repeating(recognizer)) {
    nav_go_hero(s_cookie);
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

static void select_click_handler(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
  MenuIndex idx = menu_layer_get_selected_index(s_menu);
  select_click(s_menu, &idx, NULL);
}

// Held-Down carry-through: steps the selection down one row every
// CARRY_SCROLL_STEP_MS while the physical Down press (begun on the HERO,
// before ACTIONS even appeared) is still held, stopping at the last row --
// reschedules itself, mirroring hold.c's own "AppTimer reschedules itself
// while still active" fill-timer shape. Guarded (no-op) if it's ever
// dispatched with no armed/active reason to run -- window_appear only
// starts it once, and it self-clears s_autoscroll_timer on its own last
// step, so a stray call here would mean a genuine lifecycle bug elsewhere,
// not a normal path; the guard is defensive, not load-bearing.
static void prv_autoscroll_cb(void *data) {
  (void)data;
  s_autoscroll_timer = NULL;
  int num_rows = (int)get_num_rows(s_menu, 0, NULL);
  MenuIndex idx = menu_layer_get_selected_index(s_menu);
  if ((int)idx.row >= num_rows - 1) return;  // already at the bottom -- stop
  menu_layer_set_selected_next(s_menu, false, MenuRowAlignCenter, true);
  if (s_scroll_indicator != NULL) layer_mark_dirty(s_scroll_indicator);
  MenuIndex new_idx = menu_layer_get_selected_index(s_menu);
  if ((int)new_idx.row >= num_rows - 1) return;  // reached the bottom -- stop
  s_autoscroll_timer = app_timer_register(CARRY_SCROLL_STEP_MS, prv_autoscroll_cb, NULL);
}

// The Down button's raw-click UP handler -- a release is reliably delivered
// here (confirmed on real firmware) even though ACTIONS never saw the
// matching down edge (the hero's own down_click_handler consumed it). This
// is the "user let go" signal that stops the auto-scroll; a harmless no-op
// on an ordinary genuine Down tap/release on ACTIONS itself (ordinary
// repeat navigation is untouched -- window_raw_click_subscribe's down
// callback is NULL here, so only the existing window_single_repeating_click_
// subscribe above reacts to the down edge, a disjoint-config-fields
// coexistence).
static void prv_down_raw_up(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
  screen_actions_cancel_carry_scroll();
}

// One-shot arm flag, set by the HERO's down_click_handler immediately BEFORE
// nav_go_actions() -- consumed (cleared) by window_appear below on this
// window's very next appearance, whether that's a fresh push from the hero
// (armed -- starts the auto-scroll) or a return from a child screen like LAPS
// (already consumed, so no spurious auto-scroll -- an arm flag is required
// to avoid triggering on that return). Deliberately does NOT touch
// s_autoscroll_timer -- see screen_actions_cancel_carry_scroll's own doc
// comment for why the two are kept orthogonal.
void screen_actions_arm_carry_scroll(void) {
  s_carry_scroll_armed = true;
}

// The auto-scroll AppTimer must never fire against an off-screen actions
// menu. Called structurally from the navigation stack's render step on
// every real navigation transition, not just from a disappear handler, so
// it is caught on Back, any Select that navigates away, and an alarm
// pre-emption alike. Deliberately does not touch s_carry_scroll_armed: this
// is called on the same transition that arms a fresh hero-to-actions
// carry-through (the hero screen's Down handler arms the flag immediately
// before navigating here, which is what triggers this same render step) --
// if this function cleared that flag too, the freshly armed flag would be
// wiped before window_appear (which runs synchronously inside that same
// navigation) ever gets to consume it. Only the actual AppTimer needs
// defensive cancellation across transitions; the one-shot arm flag's own
// lifetime (set immediately before, consumed synchronously within the same
// press dispatch) has no staleness window to defend against.
void screen_actions_cancel_carry_scroll(void) {
  if (s_autoscroll_timer != NULL) {
    app_timer_cancel(s_autoscroll_timer);
    s_autoscroll_timer = NULL;
  }
}

static void click_config_provider(void *context) {
  (void)context;
  window_single_repeating_click_subscribe(BUTTON_ID_UP, 100, up_click_handler);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 100, down_click_handler);
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click_handler);
  window_single_click_subscribe(BUTTON_ID_BACK, back_click_handler);
  // Coexists with the repeating Down subscribe above (disjoint ClickConfig
  // fields) -- passing NULL for the raw DOWN callback means a genuine
  // press-while-on-ACTIONS still only fires the existing repeating handler;
  // this raw UP handler is purely the "release" signal for the
  // carried-through hold.
  window_raw_click_subscribe(BUTTON_ID_DOWN, NULL, prv_down_raw_up, NULL);
}

static void window_appear(Window *window) {
  (void)window;
  menu_layer_reload_data(s_menu);
  // A pending Rename row (set by select_click's RENAME branches, cleared by
  // screen_actions_set_cookie on every genuine fresh entry) means this
  // appearance is the OS revealing ACTIONS again after the system dictation
  // UI closed -- restore the cursor to the row that launched it instead of
  // resetting to row 0 (MenuRowAlignCenter, not Top -- MenuRowAlignTop can
  // fail to scroll a trailing/off-screen row into view, which applies here
  // too even though no ACTIONS row set currently overflows the viewport).
  if (s_pending_row >= 0) {
    menu_layer_set_selected_index(s_menu, MenuIndex(0, (uint16_t)s_pending_row), MenuRowAlignCenter,
                                   false);
    s_pending_row = -1;
  } else {
    // MenuRowAlignNone resets the logical selected index to row 0 but
    // explicitly leaves the viewport's scroll offset untouched (per the
    // SDK's own documentation) -- root cause of a bug where the cursor
    // resets to row 0 but the visible scroll position doesn't follow, once a
    // menu has more rows than fit on screen. MenuRowAlignTop does scroll the
    // viewport to follow the selection, so it's used here instead.
    menu_layer_set_selected_index(s_menu, MenuIndex(0, 0), MenuRowAlignTop, false);
  }
  if (s_scroll_indicator != NULL) layer_mark_dirty(s_scroll_indicator);

  // Held-Down carry-through: consume the one-shot arm flag on THIS
  // appearance only -- a fresh hero->ACTIONS push (armed) starts the
  // delayed auto-scroll; any other appearance (a child screen popping back)
  // finds it already false and does nothing.
  if (s_carry_scroll_armed) {
    s_carry_scroll_armed = false;
    if (s_autoscroll_timer != NULL) {
      app_timer_cancel(s_autoscroll_timer);
    }
    s_autoscroll_timer = app_timer_register(CARRY_SCROLL_INITIAL_MS, prv_autoscroll_cb, NULL);
  }
}

// Thin scroll indicator overlay, needed because a MenuLayer's draw_row can
// only paint inside its own cell, the same reasoning as the home screen's
// equivalent overlay. In practice no action-row set here currently has more
// rows than fit on one screen, so render_scroll_indicator self-skips
// (total rows <= visible rows) -- wired anyway so the capability is
// uniformly present on every menu screen, not just the ones that happen to
// overflow today.
static void scroll_indicator_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  int total = get_num_rows(s_menu, 0, NULL);
  int visible = (int)(bounds.size.h / RENDER_MENU_ROW_H);
  if (visible < 1) visible = 1;
  MenuIndex sel = menu_layer_get_selected_index(s_menu);
  render_scroll_indicator(ctx, bounds, total, visible, (int)sel.row);
}

// The floating colored strip -- kind glyph+label left, current value right
// -- replacing the plain paper clock-strip.
static void strip_update_proc(Layer *layer, GContext *ctx) {
  const Timer *t = store_find_const(app_context_store(), s_cookie);
  if (t == NULL) return;  // defensive
  ViewColorToken token = view_kind_color_token(t->kind, VIEW_HERO_RUNNING);
  char value[16];
  format_home_row_value(value, sizeof value, t, time(NULL));
  // The strip is white text ON the kind color -- use the DEEP
  // (darkened) variant, not the identity render_kind_color() (see
  // render.h's doc comment on render_kind_color_deep).
  render_floating_strip(ctx, layer_get_bounds(layer), render_kind_color_deep(token), t->kind,
                         render_kind_label(t->kind), value);
}

// The MenuLayer is created once here, not in a load handler, and is never
// destroyed or recreated across navigation. The navigation stack's
// diff-based render step unloads/reloads this Window only on the
// transitions that actually touch it (cheap: it only re-attaches an
// already-live layer), but must never re-allocate the MenuLayer itself --
// repeated MenuLayer create/destroy churn is a classic source of heap
// fragmentation on this platform's limited RAM.
Window *screen_actions_create(void) {
  s_window = window_create();
  window_set_background_color(s_window, RENDER_COLOR_PAPER);
  // Our OWN complete click config provider (see the long comment above),
  // wired the ordinary way -- NOT via menu_layer_set_click_
  // config_onto_window(), which this window deliberately does not call.
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

  // Floating colored strip, replacing an earlier plain clock strip -- still
  // the same reasoning as elsewhere in this file: a MenuLayer's draw_row can
  // only paint inside a cell, so this needs its own sibling Layer above the
  // menu.
  s_strip = layer_create(GRect(0, 0, bounds.size.w, RENDER_STRIP_H));
  layer_set_update_proc(s_strip, strip_update_proc);
  layer_add_child(root, s_strip);

  s_menu = menu_layer_create(GRect(0, RENDER_STRIP_H, bounds.size.w, bounds.size.h - RENDER_STRIP_H));
  // See screen_home.c's matching fix -- the SDK's MenuLayer defaults
  // highlight background to full-cell black; this makes the un-inset
  // margin around render_menu_row()'s own smaller pill read as paper
  // instead.
  menu_layer_set_highlight_colors(s_menu, RENDER_COLOR_PAPER, GColorBlack);
  // Deliberately no .select_click callback here: menu_layer_set_click_
  // config_onto_window() is never called on this window (see the click
  // config provider's comment above), so this field would never be invoked
  // by anything. This looks exactly like the live, wired-up pattern shown in
  // typical MenuLayer usage, which makes it tempting to "simplify" by
  // deleting select_click_handler's explicit call above as apparently
  // redundant -- doing so would silently reintroduce a dead-Select
  // regression, since Select is only ever driven through the custom click
  // config provider, not through this callback.
  menu_layer_set_callbacks(s_menu, NULL,
                            (MenuLayerCallbacks){
                                .get_num_rows = get_num_rows,
                                .get_cell_height = get_cell_height,
                                .draw_row = draw_row,
                            });
  layer_add_child(root, menu_layer_get_layer(s_menu));

  s_scroll_indicator = layer_create(GRect(bounds.size.w - SCROLL_INDICATOR_WIDTH - SCROLL_INDICATOR_MARGIN,
                                           RENDER_STRIP_H, SCROLL_INDICATOR_WIDTH,
                                           bounds.size.h - RENDER_STRIP_H));
  layer_set_update_proc(s_scroll_indicator, scroll_indicator_update_proc);
  layer_add_child(root, s_scroll_indicator);

  return s_window;
}

void screen_actions_destroy(void) {
  layer_destroy(s_scroll_indicator);
  s_scroll_indicator = NULL;
  menu_layer_destroy(s_menu);
  s_menu = NULL;
  layer_destroy(s_strip);
  s_strip = NULL;
  window_destroy(s_window);
  s_window = NULL;
}

void screen_actions_set_cookie(int32_t cookie) {
  s_cookie = cookie;
  // Only ever called (via nav.c's apply_cookie()) right before a genuine
  // fresh push/return onto this window -- never on the OS-level reveal a
  // Rename dictation's own close triggers (see s_pending_row's doc comment
  // above). Clearing here means a stale pending row from an earlier,
  // possibly-different timer's interrupted Rename attempt (e.g. an alarm
  // pre-empting mid-dictation) can never leak onto an unrelated later visit.
  s_pending_row = -1;
}

// Applies a rename once the dictation transcript is available. Mirrors the
// preset-actions screen's own apply-name function one-for-one: store_set_
// name() is the Timer-store analogue of the preset store's equivalent,
// using the same trim-and-UTF-8-safe-cap primitive underneath. Persisted
// and replanned immediately, since a rename mutates a live, possibly
// wakeup-scheduled timer and there is no staging step here, the same
// reasoning persist_and_replan's other call sites already follow. The menu
// reload afterward is for parity with the preset sheet's own reload; no
// action row currently renders a timer's name itself, so this has no
// visible effect today, but is kept for consistency if that changes.
void screen_actions_apply_name(const char *name) {
  Timer *t = store_find(app_context_store(), s_cookie);
  if (t == NULL) return;  // defensive -- timer gone (e.g. fired+resolved) while dictation was in flight
  store_set_name(app_context_store(), s_cookie, name);
  persist_and_replan();
  testlog("actions_rename", "ok");
  if (s_menu != NULL) menu_layer_reload_data(s_menu);
}

void screen_actions_mark_clock_dirty(void) {
  if (s_strip != NULL) layer_mark_dirty(s_strip);
}

#ifdef GALLERY_MODE
// Debug-only accessor for the screen gallery (src/c/gallery.c): exposes the
// real MenuLayer so the gallery can force a reload (consecutive gallery
// scenes may both target ACTIONS on a freshly-rebuilt fixture, which is NOT
// a real window-stack transition, so window_appear -- the normal reload
// trigger -- never fires) and set the cursor to a specific row
// (options-cursor-position variants). Never compiled into a release build.
MenuLayer *screen_actions_debug_menu(void) {
  return s_menu;
}
#endif
