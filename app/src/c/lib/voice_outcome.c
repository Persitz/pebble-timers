// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

#include "voice_outcome.h"

// DictationSessionStatus numeric values (pebble.h:2095-2194), documented
// here as plain ints since this file may not include <pebble.h>:
//   0 Success
//   1 FailureTranscriptionRejected
//   2 FailureTranscriptionRejectedWithError
//   3 FailureSystemAborted
//   4 FailureNoSpeechDetected
//   5 FailureConnectivityError
//   6 FailureDisabled
//   7 FailureInternalError
//   8 FailureRecognizerError
enum {
  DICT_STATUS_SUCCESS = 0,
  DICT_STATUS_REJECTED = 1,
  DICT_STATUS_REJECTED_WITH_ERROR = 2,
  DICT_STATUS_SYSTEM_ABORTED = 3,
  DICT_STATUS_NO_SPEECH = 4,
  DICT_STATUS_CONNECTIVITY = 5,
  DICT_STATUS_DISABLED = 6,
  DICT_STATUS_INTERNAL_ERROR = 7,
  DICT_STATUS_RECOGNIZER_ERROR = 8,
};

VoiceOutcome voice_outcome_for_status(int dictation_status, bool parse_ok) {
  switch (dictation_status) {
    case DICT_STATUS_SUCCESS:
      return parse_ok ? VOICE_OK : VOICE_PARSE_FAIL;
    case VOICE_STATUS_CREATE_FAILED:
    case DICT_STATUS_CONNECTIVITY:
      return VOICE_NO_PHONE;
    case DICT_STATUS_NO_SPEECH:
      return VOICE_NO_SPEECH;
    case DICT_STATUS_DISABLED:
      return VOICE_DISABLED;
    case DICT_STATUS_REJECTED:
    case DICT_STATUS_REJECTED_WITH_ERROR:
      return VOICE_CANCELLED;
    case DICT_STATUS_SYSTEM_ABORTED:
    case DICT_STATUS_INTERNAL_ERROR:
    case DICT_STATUS_RECOGNIZER_ERROR:
    default:
      return VOICE_ERROR;
  }
}
