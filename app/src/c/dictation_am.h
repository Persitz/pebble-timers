// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// The ONLY file that ever touches `dictation_session_*` -- the testlog_am.c
// precedent for a single adapter-tier seam
// around one SDK subsystem, applied to Dictation. Owns the app's one reusable
// DictationSession, copies the freed transcript into a fixed buffer before
// the callback returns (pebble.h's own documented contract: "the
// transcribed string will be freed after this call returns"), and hands the
// result to screen_voice via its real content seam, screen_voice_show_parse().
#pragma once

#include <stdbool.h>

// Whether an attempt should parse the transcript (FULL, the timer-create
// path) or take it verbatim as a name -- NAME routes to screen_voice_set_
// name() (for Add-name/Rename on the voice-create confirm preview), while
// PRESET_NAME routes to screen_preset_actions_apply_name() (for renaming a
// Preset in the Library, addressed by index, a different sink), and
// ACTIONS_NAME routes to screen_actions_apply_name() (for renaming a live
// Timer, addressed by ACTIONS' own held cookie, yet another sink). A separate
// enum value per sink rather than reusing DICT_SCOPE_NAME with a hidden mode
// flag keeps dictation_am.c's deliver() a clean switch -- see its own doc
// comment for the full routing.
typedef enum {
  DICT_SCOPE_FULL,
  DICT_SCOPE_NAME,
  DICT_SCOPE_PRESET_NAME,
  DICT_SCOPE_ACTIONS_NAME,
} DictScope;

// Creates the one DictationSession this app ever has. Confirmation stays
// OFF (screen_voice's own confirm/edit preview replaces the system's
// built-in accept/reject screen); error dialogs stay ON so the system owns
// transient-retry UI and this adapter only ever sees TERMINAL outcomes.
// A NULL session back from the SDK (no phone / no voice support / no mic,
// or an internal error -- pebble.h's own documented create() failure modes)
// is not treated as fatal here: dictation_am_start() retries creation
// lazily on every attempt for as long as it keeps failing, so a phone
// connected AFTER app launch is not permanently locked out until relaunch.
// Call once, any time before the first dictation_am_start() -- no ordering
// dependency on nav_init()/screen_voice_create().
void dictation_am_init(void);

// Destroys the session, if one exists. Call once at app exit.
void dictation_am_deinit(void);

// Starts (or restarts) a dictation attempt. No-op (re-entrancy guard) if an
// attempt is already in progress -- one at a time, matching hold.c's own
// "a user does one thing at a time" exclusivity model. If no live session
// exists (creation has never succeeded), retries creation once before
// synchronously delivering the VOICE_NO_PHONE outcome to screen_voice.
// FULL routes a successful transcript through parse_nl + screen_voice_
// show_parse (the timer-create preview); NAME routes it verbatim to
// screen_voice_set_name() (for Add-name/Rename on that same preview);
// PRESET_NAME routes it verbatim to screen_preset_actions_apply_name()
// (for Rename on a Library preset); ACTIONS_NAME routes it verbatim to
// screen_actions_apply_name() (for Rename on a live/running timer) -- see
// dictation_am.c's deliver() for the full routing, including the shared
// "failure/cancel is a silent no-op" rule NAME/PRESET_NAME/ACTIONS_NAME all
// follow.
void dictation_am_start(DictScope scope);

// Cancels any in-progress attempt WITHOUT delivering an outcome -- the
// dictation analogue of hold_cancel() (hold.h). Called from nav.c's
// render_stack(), the SAME structural per-transition funnel that already
// calls hold_cancel() on every real navigation, so an alarm pre-empting
// mid-dictation (the tick/wakeup handlers that detect a firing timer run
// independently of button input, so this can happen while the system
// dictation UI owns the whole screen) stops the stale session -- which both
// suppresses its now-irrelevant callback (pebble.h: "no status callbacks
// will be received after the session is stopped") and hides the system UI,
// revealing whatever nav.c just pushed underneath it (the alarm). Safe to
// call when nothing is in progress (no-op), same contract as hold_cancel().
void dictation_am_cancel(void);

#ifdef EMU_TEST_HOOKS
// Test-only funnel into deliver(), compiled ONLY into a PEBBLE_EMU_TEST_
// HOOKS=1 build (release binaries never see this symbol -- see wscript's
// build()). Called by testlog_am.c's inbox handler when a harness action
// script injects a {transcript[, status]} AppMessage, letting an emulator
// script drive the REAL parse->confirm-menu path (deliver() is the exact
// same funnel the real status_callback() uses) without the real phone/
// dictation service having to answer. Must follow a real dictation_am_start()
// call (e.g. a "Say a timer" tap) so s_scope/s_in_progress are set the way
// a genuine attempt would set them.
void dictation_am_inject(int status, const char *transcript);
#endif
