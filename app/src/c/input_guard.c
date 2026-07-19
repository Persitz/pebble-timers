// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

#include "input_guard.h"

static bool s_blocking;
static AppTimer *s_timer;

static void guard_expire_cb(void *data) {
  (void)data;
  s_timer = NULL;
  s_blocking = false;
}

void input_guard_arm(void) {
  if (s_timer != NULL) app_timer_cancel(s_timer);  // re-arm: restart the window
  s_blocking = true;
  s_timer = app_timer_register(INPUT_GUARD_MS, guard_expire_cb, NULL);
}

bool input_guard_blocking(void) {
  return s_blocking;
}

void input_guard_deinit(void) {
  if (s_timer != NULL) {
    app_timer_cancel(s_timer);
    s_timer = NULL;
  }
  s_blocking = false;
}
