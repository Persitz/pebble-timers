// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// See dictation_am.h for the module's scope/contract. Skeleton mirrors
// testlog_am.c's own shape (one static "channel" of SDK state, a handful of
// small functions, adapter-tier only -- lib/parse_nl and lib/voice_outcome
// stay pure and are simply called from here).
//
// The int-cast boundary lives HERE (`(int)status` below), matching lib/
// voice_outcome.h's own doc comment: "The adapter (dictation_am.c)
// is the only file that ever sees the real enum; it casts to int at the
// call site." -- keeps voice_outcome.{c,h} pebble.h-free and host-testable.
#include "dictation_am.h"

#include <pebble.h>
#include <stdio.h>

#include "lib/parse_nl.h"
#include "lib/voice_outcome.h"
#include "nav.h"
#include "screen_actions.h"
#include "screen_preset_actions.h"
#include "screen_voice.h"
#include "testlog_am.h"

// Sized for the longest expected phrase (a name + a duration clause) --
// screen_voice.c's own VOICE_TRANSCRIPT_MAX is kept in sync with this value
// (see that file's comment on VOICE_TRANSCRIPT_MAX).
#define DICTATION_BUFFER_SIZE 64

static DictationSession *s_session;
static bool s_in_progress;  // re-entrancy guard -- one attempt at a time
static DictScope s_scope;   // which scope the in-flight/last attempt used
static char s_transcript[DICTATION_BUFFER_SIZE];

// testlog tag for every VoiceOutcome this adapter can ever deliver -- one
// literal per error class, so every arc's outcome is observable to automated
// tests via its own testlog marker.
static const char *outcome_tag(VoiceOutcome outcome) {
  switch (outcome) {
    case VOICE_OK: return "success";
    case VOICE_PARSE_FAIL: return "parse_fail";
    case VOICE_NO_PHONE: return "no_phone";
    case VOICE_NO_SPEECH: return "no_speech";
    case VOICE_DISABLED: return "disabled";
    case VOICE_CANCELLED: return "cancelled";
    case VOICE_ERROR:
    default: return "error";
  }
}

// Routes one resolved {status, transcript} pair to screen_voice, clearing
// the re-entrancy guard first (this is the ONE place every attempt -- real
// callback, a synchronous create-failure, or a synchronous
// dictation_session_start() failure -- ends up). `transcript` is only ever
// read when `status` is Success; every other status routes through
// voice_outcome_for_status with parse_ok=false without touching it.
static void deliver(int status, const char *transcript) {
  s_in_progress = false;

  // A NAME-scope Add-name/Rename re-dictation must never blow away the
  // FULL-scope confirm preview it was launched from, a PRESET_NAME-scope
  // Rename re-dictation must never disturb the {Edit, Rename, Delete} sheet
  // it was launched from, and an ACTIONS_NAME-scope Rename re-dictation must
  // never disturb the ACTIONS menu it was launched from -- on Success the
  // transcript IS the name, taken verbatim (no parse_nl pass); on any OTHER
  // status (cancelled, no speech, a synchronous start() failure, ...) this
  // is a documented no-op for ALL THREE scopes: testlog the outcome to make
  // it observable to automated tests, then leave the calling screen's held
  // state exactly as it was.
  // This scope check must come BEFORE the generic non-Success branch below,
  // which would otherwise route a failure through
  // nav_go_home()/screen_voice_show_parse() and destroy that same
  // preview/sheet/menu.
  if (s_scope == DICT_SCOPE_NAME || s_scope == DICT_SCOPE_PRESET_NAME || s_scope == DICT_SCOPE_ACTIONS_NAME) {
    if (status == 0) {  // DictationSessionStatusSuccess
      if (s_scope == DICT_SCOPE_NAME) {
        testlog("dictation_outcome", "name_set");
        screen_voice_set_name(transcript);
      } else if (s_scope == DICT_SCOPE_PRESET_NAME) {
        testlog("dictation_outcome", "preset_name_set");
        screen_preset_actions_apply_name(transcript);
      } else {
        testlog("dictation_outcome", "actions_name_set");
        screen_actions_apply_name(transcript);
      }
    } else {
      testlog("dictation_outcome", outcome_tag(voice_outcome_for_status(status, false)));
    }
    return;
  }

  if (status != 0) {  // != DictationSessionStatusSuccess (or CREATE_FAILED)
    VoiceOutcome outcome = voice_outcome_for_status(status, false);
    testlog("dictation_outcome", outcome_tag(outcome));
    // A user-rejected dictation (Rejected/RejectedWithError) is a SILENT
    // return to HOME, not a screen_voice content state -- screen_voice.c's
    // build_rows() never actually draws a VOICE_CANCELLED row set live (it
    // falls through to the VOICE_ERROR rows defensively, for exactly this
    // reason: this outcome is never supposed to reach it). Pressing Back
    // while the system's own dictation UI is showing delivers exactly this
    // status.
    // Target-aware: screen_voice_cancel_to_launcher(), not a fixed
    // nav_go_home_focus_voice_row() -- this IS "Back from system UI"
    // cancelling the voice arc, so the cursor should land back on whichever
    // row launched THIS session ("Say a timer" or "Say a preset"), not row 0
    // (and not always HOME).
    if (outcome == VOICE_CANCELLED) {
      screen_voice_cancel_to_launcher();
    } else {
      screen_voice_show_parse(NULL, NULL, outcome);
    }
    return;
  }

  NlParse parse;
  bool ok = parse_nl(transcript, &parse);
  // This is the ONE place lib/parse_nl's pure clock-target recognition gets
  // resolved into an actual countdown -- reads the real clock (time()/
  // localtime(), the same now()-boundary screen_voice.c's own do_start()/
  // header_update_proc already cross) ONLY here in the adapter tier, never
  // in lib/ (parse_nl.h stays pebble.h-free). Overwrites the
  // has_duration=false/duration_s=0 lib left it at;
  // `ok` was already true for a recognized clock target (NlParse.ok's own
  // doc comment), so this doesn't change the outcome, only fills in the
  // duration + the confirm-screen's target annotation (NlParse.
  // clock_target_sod).
  if (parse.clock.has_clock_target) {
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    int now_sod = (lt != NULL) ? (lt->tm_hour * 3600 + lt->tm_min * 60 + lt->tm_sec) : 0;
    parse.duration_s = nl_clock_target_to_duration(&parse.clock, now_sod, &parse.clock_target_sod);
    parse.has_duration = true;
  }
  VoiceOutcome outcome = voice_outcome_for_status(status, ok);
  testlog("dictation_outcome", outcome_tag(outcome));
  screen_voice_show_parse(transcript, &parse, outcome);
}

static void status_callback(DictationSession *session, DictationSessionStatus status,
                             char *transcription, void *context) {
  (void)session;
  (void)context;
  // pebble.h's own documented contract: `transcription` is freed after this
  // callback returns -- copy it first, unconditionally, before doing
  // anything else. May be NULL for a failure status; snprintf("%s", NULL)
  // is undefined, so guard explicitly rather than relying on a particular
  // libc's tolerance.
  if (transcription != NULL) {
    snprintf(s_transcript, sizeof s_transcript, "%s", transcription);
  } else {
    s_transcript[0] = '\0';
  }
  deliver((int)status, s_transcript);
}

// Attempts to (re)create the session. Idempotent to call repeatedly while it
// keeps failing -- every caller below only invokes it when s_session is
// already NULL.
static bool try_create_session(void) {
  s_session = dictation_session_create(DICTATION_BUFFER_SIZE, status_callback, NULL);
  if (s_session != NULL) {
    // Confirmation is left OFF: screen_voice's own confirm/edit preview
    // replaces the system's built-in accept/reject screen. Error dialogs are
    // left ON, so the system owns transient retries instead of a dedicated
    // screen here for each one -- this was checked against the alternative:
    // in an environment where the "companion connected" check passes but
    // nothing ever answers the session-setup handshake,
    // dictation_session_start() begins a handshake that never resolves on
    // its own. Turning error dialogs OFF made that worse, not better: the
    // system shows a silent "listening" placeholder forever with no
    // terminal status ever delivered. Left ON, the same no-answer handshake
    // still doesn't resolve by itself, but a real user/script action --
    // pressing Back while the system UI is showing -- reliably delivers a
    // real DictationSessionStatusFailure TranscriptionRejected(WithError)
    // callback, which this file routes to VOICE_CANCELLED ->
    // screen_voice_cancel_to_launcher() below. On a real device the
    // recognizer answers promptly and never reaches this no-answer state at
    // all -- it is an artifact of environments where a session partner is
    // connected but never actually drives the dictation flow, not something
    // a real user hits.
    dictation_session_enable_confirmation(s_session, false);
    dictation_session_enable_error_dialogs(s_session, true);
  }
  return s_session != NULL;
}

void dictation_am_init(void) {
  try_create_session();
  s_in_progress = false;
}

void dictation_am_deinit(void) {
  if (s_session != NULL) {
    dictation_session_destroy(s_session);
    s_session = NULL;
  }
  s_in_progress = false;
}

void dictation_am_start(DictScope scope) {
  if (s_in_progress) return;  // re-entrancy guard

  if (s_session == NULL && !try_create_session()) {
    // Still no session -- no phone / no voice support / no mic, or an
    // internal SDK error (pebble.h's own documented create() failure
    // modes). NOT reachable live on THIS emulator (its own
    // dictation_session_create() genuinely succeeds -- see
    // try_create_session()'s comment for the empirical finding); this is a
    // real device's own no-phone-paired case, host-tested instead (test/
    // dictation_am_test.c). Delivered synchronously, through the exact same
    // path a real terminal DictationSessionStatus would use, so screen_voice
    // and every caller (nav_go_voice, "Say again") never need to
    // special-case "the session never existed" vs. "it existed and came
    // back with a real failure."
    s_scope = scope;
    testlog("dictation", "create_failed");
    deliver(VOICE_STATUS_CREATE_FAILED, NULL);
    return;
  }

  s_scope = scope;
  s_in_progress = true;
  // PRESET_NAME and ACTIONS_NAME share the "start_name" tag with
  // NAME -- all three are a verbatim-name re-dictation, distinguished by
  // which screen's seam the eventual Success routes to (deliver()'s own
  // scope switch above), not by a separate testlog tag.
  testlog("dictation", scope == DICT_SCOPE_FULL ? "start_full" : "start_name");
  // pebble.h documents dictation_session_start() can fail SYNCHRONOUSLY
  // ("already in progress or is invalid") -- a case this
  // module's own s_in_progress guard cannot rule out by construction (it's
  // this module's OWN bookkeeping, not the SDK's internal session state).
  // If it fails, no status callback will ever fire for this attempt (nothing
  // was actually started), so s_in_progress must be cleared here and now --
  // deliver() (the same synchronous-delivery path try_create_session()
  // failure already uses, just above) does exactly that as its first
  // statement, rather than leaving the guard stuck until an unrelated nav
  // transition happens to clear it via render_stack()'s cancel funnel.
  // Reuses the VOICE_STATUS_CREATE_FAILED sentinel (-> VOICE_NO_PHONE) as
  // the fail-soft outcome rather than inventing a new, narrower VoiceOutcome
  // for this rare SDK-edge-case path (matches this file's existing
  // create()==NULL handling one branch up).
  if (dictation_session_start(s_session) != DictationSessionStatusSuccess) {
    testlog("dictation", "start_failed");
    deliver(VOICE_STATUS_CREATE_FAILED, NULL);
  }
}

void dictation_am_cancel(void) {
  if (!s_in_progress) return;
  s_in_progress = false;
  testlog("dictation", "cancelled");
  if (s_session != NULL) {
    // pebble.h: "no status callbacks will be received after the session is
    // stopped" -- the in-flight attempt's eventual outcome is deliberately
    // never delivered. This is a genuine ABORT (the hold.c analogue is the
    // phantom-fire guard, not a fire with a synthesized "cancelled"
    // result) -- screen_voice's held state is left exactly as it was before
    // this attempt started.
    dictation_session_stop(s_session);
  }
}

#ifdef EMU_TEST_HOOKS
// See dictation_am.h's doc comment. This test-only injection hook needs two
// things to behave like a real terminal outcome:
// (1) On a real device, a terminal status_callback only ever fires once the
// system's own dictation UI has already closed itself. In a test
// environment where that UI never resolves on its own (see
// try_create_session()'s own comment), injecting a fake Success straight
// into deliver() without first dismissing the overlay leaves the system's
// dictation overlay stuck on screen forever, hiding
// screen_voice_show_parse()'s confirm menu underneath it.
// dictation_session_stop() is the SAME call dictation_am_cancel() already
// uses to dismiss that overlay during a real alarm pre-emption (see its own
// comment) -- pebble.h documents it delivers no callback of its own, so it
// can never race with or clobber the {status, transcript} pair `deliver()`
// is about to process.
// (2) deliver() itself is the single funnel every real terminal outcome
// (status_callback, a synchronous create-failure, a synchronous
// start()-failure) already ends at, so injecting here plays exactly the
// role the phone's status_callback would.
void dictation_am_inject(int status, const char *transcript) {
  if (s_session != NULL) {
    dictation_session_stop(s_session);
  }
  deliver(status, transcript);
}
#endif
