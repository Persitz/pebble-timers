// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// Pure navigation-policy seam: implements the interruption model's EXCEPTIONS
// to the native window-stack take-over
// (dead-reference guard, hold-cancel-on-fire, no-re-capture-while-already-
// alarmed). NEVER #include <pebble.h> -- operates over an abstract ViewDesc,
// not the real OS window stack; src/c/nav.c is a thin executor that mirrors
// its own logical view model to window_stack_push/pop using these decisions.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_state.h"
#include "store.h"

typedef enum {
  VD_HOME,
  VD_SET,
  VD_HERO,
  VD_ACTIONS,
  VD_ALARM,
  VD_ALARM_OPTIONS,
  VD_LAPS,          // stopwatch laps list, reachable from ACTIONS
  VD_RINGING_LIST,  // >=2 simultaneously-ringing timers stack into this list
  // HOME's "Clear all N timers?" confirm screen -- a top-level screen
  // reachable only from HOME's "Clear all" row (like VD_RINGING_LIST, never
  // nested under anything else; cookie ignored). Select there clears the
  // store and returns to (now-empty) HOME; Back cancels back to HOME
  // unchanged.
  VD_CLEAR_CONFIRM,
  // The Library screen (screen_library.c) -- reached from HOME's "Library"
  // row, one level deep (HOME, LIBRARY). Cookie ignored -- it reads the
  // PresetStore live at draw time, not scoped to one preset.
  VD_LIBRARY,
  // The {Edit, Delete} preset-actions sheet
  // (screen_preset_actions.c) -- reached via hold-Select on a Library row,
  // nested one level deeper still (HOME, LIBRARY, PRESET_ACTIONS). `cookie`
  // is repurposed to carry the PRESET INDEX (documented reuse, like VD_SET's
  // own cookie reuse for the edit-interval timer) -- presets are
  // index-addressed, not cookie-addressed (no cross-session stable identity
  // needed for a template).
  VD_PRESET_ACTIONS,
  // The voice confirm/edit preview + error screens (screen_voice.c) --
  // reached from HOME's "Say a timer" row, one level deep (HOME, VOICE),
  // mirroring VD_LIBRARY/VD_ACTIONS' own shape. Cookie ignored -- like
  // VD_LIBRARY, it reads state held by screen_voice.c itself (the current
  // NlParse/VoiceOutcome), never store-linked.
  //
  // Unlike VD_LIBRARY, VD_VOICE is NOT "always coherent" as a resume target:
  // nav_on_resolve() always resolves an alarm-pre-empted voice screen to
  // HOME instead, rather than back to the voice screen itself. Rationale: a
  // dictation attempt that gets aborted mid-attempt by an incoming alarm
  // (the system dictation UI pre-empted while up) leaves screen_voice
  // showing a stale safe placeholder ("Voice needs your phone") that has no
  // relation to why the user is seeing it -- see nav_on_resolve's own doc
  // comment for the full reasoning.
  VD_VOICE,
} ViewDescKind;

typedef struct {
  ViewDescKind kind;
  // Meaningful for HERO/ACTIONS/ALARM/ALARM_OPTIONS/LAPS (a timer cookie) and
  // for SET when mid-edit-interval (also a timer cookie); ignored for
  // HOME/RINGING_LIST/CLEAR_CONFIRM/LIBRARY/SET-create/SET-preset-dial.
  // VD_PRESET_ACTIONS repurposes this field to carry a
  // PRESET INDEX instead of a timer cookie (documented reuse -- see this
  // struct's own ViewDescKind doc above).
  int32_t cookie;
} ViewDesc;

// Interruption model take-over rule: a fire pre-empts whatever the user is
// doing UNLESS they are already looking at the alarm (a second/third fire
// just stacks into `ringing[]` -- no re-capture). Returns true if the
// alarm view should pre-empt `current` right now. VD_RINGING_LIST
// counts as "already looking at the alarm" too -- a third/fourth simultaneous
// fire while the ringing list is already up just grows its row count
// (screen_ringing.c re-syncs from AppState every tick), no take-over.
//
// This used to also return a `cancel_hold` flag (via the now-removed
// `nav_should_cancel_hold`), but nav.c's render_stack() unconditionally
// calls hold_cancel() before EVERY real window-stack transition -- any
// take-over is, by construction, a real transition, so the separate flag
// was always redundant with that blanket cancel and never had an
// independent code path. Removed rather than kept as a silently-dead
// affordance; re-add only if a future caller needs to cancel a hold
// WITHOUT a window-stack transition happening in the same call.
bool nav_on_fire(ViewDesc current);

// Dead-reference guard: when `ringing` empties and the app wants to restore
// the pre-empted view, `resume` is only honored if it's still coherent --
// HOME always is; SET is coherent when cookie==0 (create mode, never
// store-linked) but subject to the SAME guard as HERO/ACTIONS/LAPS when
// cookie!=0 (nav_go_edit_interval gives SET a real store-linked cookie for
// Edit interval -- an edited repeat can fire mid-edit, take over, and get
// resolved away while the user was still mid-dial, exactly like a
// HERO/ACTIONS/LAPS resume target can go stale -- the guard list must cover
// SET whenever edit-mode carries a real cookie, exactly as it does for
// LAPS's own store-linked cookie).
// HERO/ACTIONS/LAPS are honored only if their cookie still exists in `store`
// (the resume target may have been the very timer resolved -- Done/removed
// -- while alarmed). Otherwise HOME. `has_resume=false` (nothing was
// captured, e.g. a cold wakeup-relaunch straight into the alarm) also
// resolves to HOME.
//
// VD_VOICE is EXCLUDED as a resume target, unconditionally, regardless of
// the store's contents or whether a real dictation attempt was ever in
// progress -- always resolves to HOME instead, never back to VD_VOICE
// itself. Rationale: an alarm aborting a PENDING dictation attempt
// (render_stack()'s structural dictation_am_cancel(), see nav.c) leaves
// screen_voice showing whatever it last held -- the safe placeholder it was
// primed with, if the attempt never resolved -- which reads as a
// confusing, unrelated "Voice needs your phone" screen right after
// dismissing an alarm, requiring an extra unexplained Back press to
// actually reach HOME. Excluding VD_VOICE unconditionally (rather than
// threading an "was this specific resume actually mid-dictation" flag
// through the pure nav_policy layer, which has no visibility into
// dictation_am's adapter-tier state anyway) is also the structurally
// consistent choice: VD_VOICE's own nav_breadcrumb_for() is always
// [HOME, VOICE] (never nested under anything else, unlike HERO/ACTIONS/LAPS
// which can sit under a real timer) -- "resolve to the pre-voice screen" and
// "resolve to HOME" are the SAME destination in this app by construction, so
// there is no second, more-honoring option being given up here.
ViewDesc nav_on_resolve(ViewDesc resume, bool has_resume, const TimerStore *store);

// Answers: "the currently-displayed alarm just resolved, but `ringing`
// still holds other members (a second/third fire merged in
// while the first was on-screen) -- which cookie should the alarm view show
// next?" Deterministic: `ringing` is itself insertion-ordered
// (app_state_ringing_add appends), so this always returns the
// earliest-still-firing member, never an arbitrary pick, regardless of
// cookie value or which position was just removed. Returns 0 (store.h's own
// "unused slot" sentinel -- never a real cookie) if `ringing` is empty;
// callers are expected to only call this when they already know it isn't
// (nav_alarm_resolved's `!ringing_now_empty` branch).
int32_t nav_pick_ringing_cookie(const AppState *app_state);

// Back on a resolving one-off alarm ("Keep counting") transforms the timer
// into a stopwatch that the design wants shown IMMEDIATELY -- this is a
// deliberate destination the user just chose FOR this specific timer, not
// the generic interruption-model `resume` restore (which represents
// whatever the user happened to be doing BEFORE this alarm pre-empted
// them -- HOME, a different timer's hero, unrelated to this choice). Using
// the generic `resume` target for keepCounting would silently revert the
// timer to wherever the user was before the fire, dumping the
// freshly-converted stopwatch straight to the background, invisible,
// unless `resume` happened to already equal this same timer's own hero
// (only true if the user was already looking at this timer's own
// hero/alarm when it fired -- the common single-timer flow).
//
// The "always this timer's own hero, never resume" rule above has exactly
// ONE exception, added here -- a pre-empted VD_SET session. Every other
// resume target (HOME, a different timer's HERO/ACTIONS/LAPS) costs the
// user NOTHING to skip: it's still sitting in the store/on HOME, reachable
// again at any time. A pre-empted SET session is different -- it is
// ephemeral, in-memory, uncommitted state that the very next
// `nav_go_set()` (screen_set_reset(), reached from HOME's "+ New timer")
// destroys unconditionally: dialing timer B mid-ring of timer A, then
// tapping Back (keepCounting) to get A out of the way, would otherwise
// silently and permanently lose the dialed value for B the moment HOME's
// "+ New timer" is opened again -- not because the value is ever
// overwritten, but because nothing ever routes the user back to it. Fixing
// this means keepCounting must still restore the pre-empted SET session
// (exactly like the generic resume path -- same dead-reference guard, via
// nav_on_resolve) whenever that's what was interrupted, while leaving the
// jump-straight-to-the-just-converted-stopwatch behavior intact for every
// other resume target -- those still have nothing to lose by being
// skipped.
ViewDesc nav_keep_counting_target(int32_t cookie, ViewDesc resume, bool has_resume,
                                   const TimerStore *store);

// Multi-alarm ringing list: decides which
// alarm-family screen should be showing given how many timers are
// SIMULTANEOUSLY ringing right now -- "several fire at once -> stack into a
// ringing list (like HOME, in red); clear one -> the next steps up." A
// single ringer shows its own hero directly (unchanged single-alarm behavior); two or
// more stack into the list instead. `solo_cookie` is only meaningful when
// `ringing_count <= 1` (ignored otherwise -- the list itself reads
// AppState's ringing[] directly at draw time, it doesn't need a specific
// cookie threaded through the ViewDesc). Called both when a fire first takes
// over (nav.c's nav_on_timer_fired) and when one alarm resolves while others
// remain ringing (nav.c's resolve_to_next_ringing) -- the same rule governs
// both directions of the 1<->2+ transition.
ViewDesc nav_alarm_target(int ringing_count, int32_t solo_cookie);
