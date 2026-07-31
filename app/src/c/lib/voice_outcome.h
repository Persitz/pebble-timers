// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// Pure Dictation-status + parse-result -> screen-outcome mapping. NEVER #include <pebble.h> -- takes a plain
// `int` status (documented to equal the numeric values of the SDK's
// DictationSessionStatus enum, pebble.h:2095-2194) rather than the enum
// type itself, so this stays lib/-pure and host-testable without a phone.
// The adapter (dictation_am.c) is the only file that ever sees the
// real enum; it casts to int at the call site.
#pragma once

#include <stdbool.h>

typedef enum {
  VOICE_OK,          // status Success + parse ok        -> confirm/edit preview
  VOICE_PARSE_FAIL,  // status Success + parse !ok        -> "Didn't catch a time"
  VOICE_DATA_UNAVAILABLE, // parser resource unavailable -> "Voice data unavailable"
  VOICE_NO_PHONE,    // create()==NULL or Connectivity(5) -> "Voice needs your phone"
  VOICE_NO_SPEECH,   // NoSpeechDetected(4)               -> "Didn't hear you"
  VOICE_DISABLED,    // Disabled(6)                       -> "Turn on voice in the Pebble app"
  VOICE_CANCELLED,   // Rejected(1)/RejectedWithError(2)  -> silent return to HOME
  VOICE_ERROR,       // SystemAborted(3)/Internal(7)/Recognizer(8) -> "Voice didn't work"
} VoiceOutcome;

// Sentinel passed by the adapter when dictation_session_create() itself
// returned NULL (no phone / no voice support / no mic) -- there is no real
// DictationSessionStatus value for "the session never got created at all".
#define VOICE_STATUS_CREATE_FAILED (-1)

// Maps a Dictation status (0-8, or VOICE_STATUS_CREATE_FAILED) + whether
// parse_nl succeeded (only meaningful when status is Success) to the screen
// state screen_voice should show. `parse_ok` is ignored for every status
// other than Success(0). Any status value outside the documented set
// degrades to VOICE_ERROR rather than crashing (fail-soft, mirrors
// parse_nl's own "never crash on unexpected input" contract).
VoiceOutcome voice_outcome_for_status(int dictation_status, bool parse_ok);
