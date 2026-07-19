// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

#include "nav_policy.h"

#include <stddef.h>

bool nav_on_fire(ViewDesc current) {
  if (current.kind == VD_ALARM || current.kind == VD_ALARM_OPTIONS ||
      current.kind == VD_RINGING_LIST) {
    // Already looking at an alarm -- this is stacking, not a
    // pre-emption: no re-capture.
    return false;
  }
  return true;
}

ViewDesc nav_on_resolve(ViewDesc resume, bool has_resume, const TimerStore *store) {
  if (!has_resume) {
    return (ViewDesc){.kind = VD_HOME, .cookie = 0};
  }
  if (resume.kind == VD_HERO || resume.kind == VD_ACTIONS || resume.kind == VD_LAPS) {
    if (store_find_const(store, resume.cookie) == NULL) {
      return (ViewDesc){.kind = VD_HOME, .cookie = 0};
    }
  }
  // SET's cookie is meaningful (edit-mode, via
  // nav_go_edit_interval) only when nonzero -- cookie 0 is create mode,
  // never store-linked, and store_find_const(store, 0) would always miss
  // (cookie 0 is reserved as "no timer," store.c's own contract) even though
  // create-mode SET is always coherent regardless of the store's contents.
  if (resume.kind == VD_SET && resume.cookie != 0) {
    if (store_find_const(store, resume.cookie) == NULL) {
      return (ViewDesc){.kind = VD_HOME, .cookie = 0};
    }
  }
  // VD_VOICE is excluded as a resume target, unconditionally. An alarm
  // pre-empting a pending dictation attempt cancels it, so by the time
  // this resolves, the voice screen would show a stale placeholder
  // ("Voice needs your phone") that has nothing to do with why the user
  // is looking at it -- confusing, and it would take a second, otherwise
  // unexplained Back press to actually reach HOME. Excluding VD_VOICE
  // entirely (rather than threading an "was a real attempt actually
  // in-progress" flag through this policy just to conditionally allow the
  // idle/already-resolved case) is also the structurally simplest option:
  // VD_VOICE's navigation breadcrumb is always [HOME, VOICE] (it is never
  // nested under anything else) -- "the screen before voice" and "HOME"
  // are the same answer in this app, by construction, so there is no
  // second option to weigh here, only one destination under either
  // framing.
  if (resume.kind == VD_VOICE) {
    return (ViewDesc){.kind = VD_HOME, .cookie = 0};
  }
  return resume;
}

int32_t nav_pick_ringing_cookie(const AppState *app_state) {
  if (app_state->ringing_count == 0) return 0;
  return app_state->ringing[0];
}

ViewDesc nav_keep_counting_target(int32_t cookie, ViewDesc resume, bool has_resume,
                                   const TimerStore *store) {
  // An in-progress SET session is the one resume target that's
  // actually at risk of being lost forever if skipped -- protect it (subject
  // to the same dead-reference guard every other resume honors). Any other
  // resume (or none at all, e.g. a cold wakeup-relaunch straight into the
  // alarm) keeps the original behavior: show the just-converted
  // stopwatch immediately.
  if (has_resume && resume.kind == VD_SET) {
    return nav_on_resolve(resume, has_resume, store);
  }
  return (ViewDesc){.kind = VD_HERO, .cookie = cookie};
}

ViewDesc nav_alarm_target(int ringing_count, int32_t solo_cookie) {
  if (ringing_count >= 2) {
    return (ViewDesc){.kind = VD_RINGING_LIST, .cookie = 0};
  }
  return (ViewDesc){.kind = VD_ALARM, .cookie = solo_cookie};
}
