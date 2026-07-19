// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// Minimal host-side stand-in for <pebble.h>, covering ONLY the AppMessage /
// AppTimer / click-config / Dictation surface that src/c/testlog_am.c,
// src/c/hold.c, src/c/input_guard.c, and src/c/dictation_am.c actually call.
// NOT a general pebble.h shim -- built specifically to let these adapter
// files (normally excluded from the host test build's lib/-only purity rule)
// compile against FFF fakes, for testlog_am's self-driven retry path,
// hold.c's up-event-past-threshold fire, input_guard.c's settling window,
// and dictation_am.c's session lifecycle. See
// test/testlog_am_test.c / test/hold_test.c / test/input_guard_test.c /
// test/dictation_am_test.c and test/Makefile's dedicated rules for each.
//
// Deliberately NOT a byte-for-byte subset of the real SDK header -- just
// enough matching declarations (names, signatures, the values these modules
// actually branch on) for them to compile and link against FFF fakes.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

// -- AppTimer -----------------------------------------------------------------

typedef struct AppTimer AppTimer;
typedef void (*AppTimerCallback)(void *data);

AppTimer *app_timer_register(uint32_t timeout_ms, AppTimerCallback callback, void *callback_data);
bool app_timer_cancel(AppTimer *timer_handle);

// -- Click config (hold.c only) ------------------------------------------------

typedef struct Window Window;
typedef struct Layer Layer;
typedef struct ClickRecognizer *ClickRecognizerRef;

typedef enum {
  BUTTON_ID_BACK = 0,
  BUTTON_ID_UP,
  BUTTON_ID_SELECT,
  BUTTON_ID_DOWN,
  NUM_BUTTONS,
} ButtonId;

typedef void (*ClickHandler)(ClickRecognizerRef recognizer, void *context);

void window_raw_click_subscribe(ButtonId button_id, ClickHandler down_handler,
                                 ClickHandler up_handler, void *context);
void window_single_repeating_click_subscribe(ButtonId button_id, uint16_t repeat_interval_ms,
                                              ClickHandler handler);

ButtonId click_recognizer_get_button_id(ClickRecognizerRef recognizer);
uint8_t click_number_of_clicks_counted(ClickRecognizerRef recognizer);

Layer *window_get_root_layer(Window *window);
void layer_mark_dirty(Layer *layer);

// hold.c's own now_ms() helper reads the current time through this.
void time_ms(time_t *tick_time, uint16_t *out_ms);

// -- AppMessage -----------------------------------------------------------------

typedef struct DictionaryIterator DictionaryIterator;

typedef enum {
  APP_MSG_OK = 0,
  APP_MSG_BUSY = 1,  // arbitrary non-OK code -- testlog_am.c only branches on == APP_MSG_OK
} AppMessageResult;

typedef enum {
  DICT_OK = 0,
} DictionaryResult;

DictionaryResult dict_write_cstring(DictionaryIterator *iter, uint32_t key, const char *cstring);
uint32_t dict_write_end(DictionaryIterator *iter);

AppMessageResult app_message_outbox_begin(DictionaryIterator **iter);
AppMessageResult app_message_outbox_send(void);

typedef void (*AppMessageOutboxSent)(DictionaryIterator *iterator, void *context);
typedef void (*AppMessageOutboxFailed)(DictionaryIterator *iterator, AppMessageResult reason,
                                        void *context);

void app_message_register_outbox_sent(AppMessageOutboxSent callback);
void app_message_register_outbox_failed(AppMessageOutboxFailed callback);

// -- Dictation (dictation_am.c only) --------------------------------------
// Matches pebble.h:2095-2194 (SDK 4.17, emery) -- see dictation_am.c's own
// top-of-file comment for the citation. Numeric values matter (voice_
// outcome.c's own doc comment documents them independently; kept identical
// here so a test driving this fake's callback with a real enum value and
// asserting on voice_outcome_for_status's behavior stays meaningful).

typedef struct DictationSession DictationSession;

typedef enum {
  DictationSessionStatusSuccess = 0,
  DictationSessionStatusFailureTranscriptionRejected = 1,
  DictationSessionStatusFailureTranscriptionRejectedWithError = 2,
  DictationSessionStatusFailureSystemAborted = 3,
  DictationSessionStatusFailureNoSpeechDetected = 4,
  DictationSessionStatusFailureConnectivityError = 5,
  DictationSessionStatusFailureDisabled = 6,
  DictationSessionStatusFailureInternalError = 7,
  DictationSessionStatusFailureRecognizerError = 8,
} DictationSessionStatus;

// transcription is freed after this callback returns -- dictation_am.c's own
// status_callback() copies it before doing anything else.
typedef void (*DictationSessionStatusCallback)(DictationSession *session,
                                                DictationSessionStatus status,
                                                char *transcription, void *context);

DictationSession *dictation_session_create(uint32_t buffer_size,
                                            DictationSessionStatusCallback callback,
                                            void *callback_context);
void dictation_session_destroy(DictationSession *session);
DictationSessionStatus dictation_session_start(DictationSession *session);
DictationSessionStatus dictation_session_stop(DictationSession *session);
void dictation_session_enable_confirmation(DictationSession *session, bool is_enabled);
void dictation_session_enable_error_dialogs(DictationSession *session, bool is_enabled);
