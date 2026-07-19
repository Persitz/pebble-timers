// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// VOICE screen adapter: the editable confirm/edit preview + the parse-fail / dictation-
// error one-liner variants -- ONE window, multiple content states, mirroring
// screen_actions.c's/screen_library.c's non-reentrant custom
// click_config_provider + MenuLayer-created-once shape (see screen_actions.c's
// own long comment for the reentrancy history this avoids).
//
// This window renders every state through the REAL parse_nl/
// voice_outcome_for_status outputs (screen_voice_show_parse is the one seam
// both the gallery and dictation_am.c call), plus the full action
// wiring: Start / Say again (dictation_am_start) / Edit on dial
// (primed) / Set-on-dial / Use-dial (all reuse existing nav.c/lib
// entry points). See screen_voice.c's top-of-file comment for the one
// remaining deferred piece (the name line / Add-name row).
#pragma once

#include <pebble.h>

#include "lib/parse_nl.h"
#include "lib/voice_outcome.h"

// What a voice session's eventual Start/Save row commits INTO -- a live Timer (via lib/store.h's
// store_add(), the original arc) or a Preset (via lib/preset_store.h's
// preset_store_add(), reached from LIBRARY's "Say a preset" row). Set ONCE
// per session by nav.c's nav_go_voice()/nav_go_voice_preset() (screen_voice_
// set_target(), below) BEFORE the window is pushed -- the same "prime, then
// switch" timing nav_go_voice() already uses for screen_voice_show_
// unavailable() -- and read by build_rows() (row label + which action fires),
// the header (the preset-unsupported-kind fail-soft message), and every
// cancel/dial-escape path (screen_voice_cancel_to_launcher(), nav_go_set_
// from_voice() vs nav_go_preset_new_from_voice()). Untouched by "Say again"/
// Add-name re-dictation -- the target describes the SESSION, not one
// attempt.
typedef enum {
  VOICE_TARGET_TIMER,
  VOICE_TARGET_PRESET,
} VoiceTarget;

Window *screen_voice_create(void);
void screen_voice_destroy(void);

// Sets which target the CURRENT voice session commits into -- see VoiceTarget's
// own doc comment above for the full contract. Call before the window is
// pushed (nav.c's nav_go_voice()/nav_go_voice_preset(), mirroring screen_
// voice_show_unavailable()'s own "prime before push" timing).
void screen_voice_set_target(VoiceTarget target);

// Cancels the voice arc back to whichever screen launched it -- HOME's "Say a
// timer" row (VOICE_TARGET_TIMER, cursor restored via nav_go_home_focus_
// voice_row()) or LIBRARY's "Say a preset" row (VOICE_TARGET_PRESET, cursor
// restored via nav_go_library_focus_row()) -- based on the target primed for
// the CURRENT session. The one shared "return to launcher" seam used by BOTH
// this window's own Back/Up-at-top click handlers AND dictation_am.c's
// VOICE_CANCELLED routing (a system-UI Back during the dictation handshake,
// which arrives asynchronously through dictation_am.c, not through this
// window's own click handlers, so it needs the same target-aware decision
// made from a shared seam rather than duplicated inline).
void screen_voice_cancel_to_launcher(void);

// The one content seam this window exposes: `transcript` is the verbatim
// heard string for the "heard: '...'" echo (NULL/"" when dictation itself
// never produced one -- the dictation-status error states), `parse` is the
// real parse_nl() result (identity strip content, ignored when outcome has
// no kind to show), and `outcome` selects which of the OK / PARSE_FAIL /
// dictation-error content states to draw. Called by the gallery (a canned
// transcript run through the real parse_nl/voice_outcome_for_status) AND
// by dictation_am.c's own deliver() with a real transcript/status
// (this is why it was never named/gated as a GALLERY_MODE-only "debug" lever
// like this app's other debug accessors -- it was always meant to be real
// production API).
void screen_voice_show_parse(const char *transcript, const NlParse *parse, VoiceOutcome outcome);

// The Add-name/Rename dictation arc's own content seam -- called by dictation_am.c's
// deliver() on a successful DICT_SCOPE_NAME re-dictation. `name` is the
// TRANSCRIPT TAKEN VERBATIM (no parse_nl pass -- the whole utterance IS the
// name, not a duration clause to parse); this function trims/caps it via
// lib/timer.h's timer_name_copy() and updates the held NlParse's name/
// has_name fields in place, then refreshes the name line + the "Add name"/
// "Rename" row label (does NOT reset the menu selection or the transcript
// echo -- unlike screen_voice_show_parse(), this is an in-place edit of the
// CURRENT confirm preview, not a fresh outcome). A no-op if called with no
// live VOICE_OK preview showing (defensive; not reachable via a real
// dictation_am.c call site today, since DICT_SCOPE_NAME is only ever started
// from screen_voice's own "Add name"/"Rename" row).
void screen_voice_set_name(const char *name);

// A safe, neutral placeholder -- the exact VOICE_NO_PHONE content
// (screen_voice_show_parse(NULL, NULL, voice_outcome_for_status(
// VOICE_STATUS_CREATE_FAILED, false))) -- for the brief window before a real
// dictation attempt's outcome is known. Called by nav.c's nav_go_voice()
// BEFORE the window is pushed: a real attempt's own
// content update only ever arrives asynchronously (dictation_am.c's
// deliver(), from the status callback), so without this, a nav pre-emption
// (an alarm firing) landing BEFORE that callback fires would leave this
// window showing whatever STALE content a previous visit left behind (or,
// on the very first-ever visit, the zero-initialized default -- a
// nonsensical "Countdown 0:00 / Start" row that would create a degenerate
// zero-duration timer if tapped). Priming with this placeholder is honest enough
// in practice: the real system dictation UI covers this window within
// milliseconds on any successful attempt, so this content is only ever
// actually SEEN in the pre-emption-before-resolution case, where "voice
// needs your phone" (never "0:00 target") is the safe reading either way --
// the real escape row ("Use dial") still works, and re-entering voice
// starts a fresh attempt.
void screen_voice_show_unavailable(void);

#ifdef GALLERY_MODE
// Debug-only accessor for the screen gallery -- mirrors screen_actions_debug_
// menu()'s/screen_library_debug_menu()'s own contract. Never compiled into a
// release build.
MenuLayer *screen_voice_debug_menu(void);

// Forces the clock-target annotation's am/pm device-style read
// (0 = 24h style, 1 = 12h style, -1 = not forced -- falls through to the
// real clock_is_24h_style()) so a gallery scene can depict either style
// deterministically, independent of whatever clock style the emulator/
// device this build happens to run on is actually set to. Mirrors hold_
// debug_force()'s/screen_hero_debug_force_up_hint()'s own "force the visual
// state" shape; reset by gallery.c's gallery_reset_fixture(). Never compiled
// into a release build.
void screen_voice_debug_force_24h_style(int is_24h);
#endif
