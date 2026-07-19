// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// Pure window-stack DIFFING: nav_policy already computes the TARGET logical
// view (ViewDesc); this seam turns a target ViewDesc into the fixed
// hierarchical breadcrumb of ViewDescs that must be on the window stack to
// show it, and diffs that against whatever breadcrumb is currently believed
// to be on the stack. NEVER #include <pebble.h> -- operates over
// ViewDesc/breadcrumbs only, no real Window pointers; src/c/nav.c is the
// executor that maps the resulting plan's ViewDescKinds to real Windows and
// issues the actual window_stack_push/pop calls.
//
// Why this exists: a naive renderer could implement every transition as
// window_stack_pop_all() followed by re-pushing the entire breadcrumb from
// HOME (e.g. HERO->ACTIONS becoming pop_all(); push(HOME); push(HERO);
// push(ACTIONS)) -- three back-to-back window_stack_push calls for what's
// logically a single-window push. PebbleOS's window_stack.c:483-486 has an
// open TODO (PBL-37477) flagging its own click-config-install path as
// ambiguous between two callers when a transition starts before the
// previous one's animation naturally completes -- exactly what repeated
// back-to-back pushes force via prv_transition_to()'s force-complete-in-
// flight-animation branch, and this has been observed to reproduce the
// firmware's "Click subscribe must be set from click config provider" assert
// on ~100% of single navigating presses. Since every ViewDescKind's
// breadcrumb is a FIXED, hierarchical sequence (never reordered -- ACTIONS
// is always HOME,HERO,ACTIONS; ALARM_OPTIONS is always HOME,ALARM,
// ALARM_OPTIONS), a plain common-prefix/differing-suffix diff is exact, not
// an approximation: popping the current breadcrumb down to the longest
// shared prefix and pushing the target's remaining suffix always leaves the
// window stack in exactly the state a full pop-all+rebuild would have --
// the same dead-reference-guard guarantee nav_policy's nav_on_resolve
// already provides (it decides the CORRECT target ViewDesc; this module
// only changes how nav.c physically mirrors that target onto the stack, not
// which target is chosen).
#pragma once

#include "nav_policy.h"

// Max breadcrumb depth: HOME, then at most one of {SET, HERO, ALARM}, then at
// most one of {ACTIONS, ALARM_OPTIONS} layered on that, then LAPS one level
// deeper still, ONLY reachable through ACTIONS (HOME,HERO,ACTIONS, LAPS).
// The preset-library views add LIBRARY (depth 2, HOME, LIBRARY) and
// PRESET_ACTIONS (depth 3, HOME, LIBRARY, PRESET_ACTIONS) -- both fit
// comfortably under the existing max, set by LAPS's depth-4 case.
#define NAV_BREADCRUMB_MAX 4

typedef struct {
  ViewDesc items[NAV_BREADCRUMB_MAX];  // bottom (items[0], always HOME) to top
  int len;
} NavBreadcrumb;

// The fixed, hierarchical breadcrumb for a given "leaf" (deepest-visible)
// ViewDesc -- mirrors the previous render_stack()'s switch exactly, just as
// data instead of push calls.
NavBreadcrumb nav_breadcrumb_for(ViewDesc leaf);

typedef struct {
  int pop_count;                       // trailing entries to pop off `current`, top-down
  ViewDesc push[NAV_BREADCRUMB_MAX];    // entries to push, in order (bottom-to-top of the new suffix)
  int push_count;
} NavStackDiff;

// Computes the minimal pop/push plan to turn `current` into `target`. Exact
// (not heuristic) because breadcrumbs are always hierarchical prefixes of
// each other or diverge outright -- see the header comment above. Two equal
// breadcrumbs (identical kind+cookie at every position) produce a no-op
// plan (pop_count == 0, push_count == 0), which also means a navigation
// call that doesn't actually change the visible view (e.g. a second timer
// firing while already showing the alarm) now issues ZERO
// window_stack calls instead of a full rebuild.
NavStackDiff nav_diff_stack(NavBreadcrumb current, NavBreadcrumb target);
