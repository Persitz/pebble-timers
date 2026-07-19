// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

#include "nav_stack.h"

static ViewDesc view_desc(ViewDescKind kind, int32_t cookie) {
  return (ViewDesc){.kind = kind, .cookie = cookie};
}

static bool view_desc_eq(ViewDesc a, ViewDesc b) {
  return a.kind == b.kind && a.cookie == b.cookie;
}

NavBreadcrumb nav_breadcrumb_for(ViewDesc leaf) {
  NavBreadcrumb bc;
  bc.items[0] = view_desc(VD_HOME, 0);
  bc.len = 1;

  switch (leaf.kind) {
    case VD_HOME:
      break;
    case VD_SET:
      // preserve leaf.cookie (0 = create mode; nonzero = Edit interval
      // for that repeat timer, nav_go_edit_interval) rather than hardcoding
      // 0 -- otherwise a create->edit (or edit-A->edit-B) transition would
      // diff as a no-op (identical breadcrumb) and apply_cookie would never
      // re-run for the second entry. screen_set.c's own state priming is
      // NOT driven by apply_cookie though (see nav.c's apply_cookie, which
      // still has no VD_SET case) -- it's primed once by nav_go_set/
      // nav_go_edit_interval themselves, specifically so an interrupted
      // mid-dial (create OR edit) session survives a take-over unchanged.
      bc.items[bc.len++] = view_desc(VD_SET, leaf.cookie);
      break;
    case VD_HERO:
      bc.items[bc.len++] = view_desc(VD_HERO, leaf.cookie);
      break;
    case VD_ACTIONS:
      bc.items[bc.len++] = view_desc(VD_HERO, leaf.cookie);
      bc.items[bc.len++] = view_desc(VD_ACTIONS, leaf.cookie);
      break;
    case VD_ALARM:
      bc.items[bc.len++] = view_desc(VD_ALARM, leaf.cookie);
      break;
    case VD_ALARM_OPTIONS:
      bc.items[bc.len++] = view_desc(VD_ALARM, leaf.cookie);
      bc.items[bc.len++] = view_desc(VD_ALARM_OPTIONS, leaf.cookie);
      break;
    case VD_LAPS:
      // LAPS is reachable only through ACTIONS -- HOME, HERO,
      // ACTIONS, LAPS. Back from LAPS goes to ACTIONS (adapter calls
      // nav_go_actions directly, not a dedicated nav_policy decision -- it's
      // a plain "go up one level," the same shape as SET's field_left/CANCEL).
      bc.items[bc.len++] = view_desc(VD_HERO, leaf.cookie);
      bc.items[bc.len++] = view_desc(VD_ACTIONS, leaf.cookie);
      bc.items[bc.len++] = view_desc(VD_LAPS, leaf.cookie);
      break;
    case VD_RINGING_LIST:
      // HOME, RINGING_LIST -- a top-level alarm-family screen just like
      // ALARM, never nested under anything else (cookie is always 0/ignored;
      // the list reads AppState's ringing[] directly, not a single cookie).
      bc.items[bc.len++] = view_desc(VD_RINGING_LIST, 0);
      break;
    case VD_CLEAR_CONFIRM:
      // HOME, CLEAR_CONFIRM -- a top-level
      // screen just like RINGING_LIST above (cookie ignored, never nested).
      bc.items[bc.len++] = view_desc(VD_CLEAR_CONFIRM, 0);
      break;
    case VD_LIBRARY:
      // HOME,
      // LIBRARY -- one level deep, cookie ignored (reads PresetStore live).
      bc.items[bc.len++] = view_desc(VD_LIBRARY, 0);
      break;
    case VD_PRESET_ACTIONS:
      // HOME, LIBRARY, PRESET_ACTIONS(index) -- reachable
      // only through LIBRARY, one level deeper still (depth 3, fits
      // NAV_BREADCRUMB_MAX=4). Preserve leaf.cookie (the preset INDEX, per
      // ViewDesc's documented reuse) through to the breadcrumb entry -- same
      // reasoning as VD_SET's own cookie preservation above: a
      // different-index transition must NOT diff as a no-op, or
      // apply_cookie's screen_preset_actions_set_index() would never re-run
      // for the second entry.
      bc.items[bc.len++] = view_desc(VD_LIBRARY, 0);
      bc.items[bc.len++] = view_desc(VD_PRESET_ACTIONS, leaf.cookie);
      break;
    case VD_VOICE:
      // HOME, VOICE -- one level
      // deep, cookie ignored (reads screen_voice.c's own held state), same
      // shape as VD_LIBRARY above.
      bc.items[bc.len++] = view_desc(VD_VOICE, 0);
      break;
  }
  return bc;
}

NavStackDiff nav_diff_stack(NavBreadcrumb current, NavBreadcrumb target) {
  int max_common = current.len < target.len ? current.len : target.len;
  int common = 0;
  while (common < max_common && view_desc_eq(current.items[common], target.items[common])) {
    common++;
  }

  NavStackDiff diff;
  diff.pop_count = current.len - common;
  diff.push_count = target.len - common;
  for (int i = 0; i < diff.push_count; i++) {
    diff.push[i] = target.items[common + i];
  }
  return diff;
}
