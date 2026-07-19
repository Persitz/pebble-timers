// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// GALLERY_MODE-only stub for lib/parse_nl's parse_nl()/
// nl_clock_target_to_duration(): the gallery build excludes lib/parse_nl.c
// (wscript's build(), the single largest TU at 9748 B of .text) because
// rendering-fidelity screenshots don't need the live parser -- gallery.c's
// own voice scenes build literal NlParse fixtures instead (values captured
// from a real, unmodified parse_nl() run and cross-checked against
// app/test/parse_nl_test.c's host corpus; see each scene's own comment).
// Parser correctness stays fully covered by that host corpus, independent of
// this build.
//
// This file supplies the two symbols the rest of the gallery image still
// references so it links: dictation_am.c's one call site (adapter tier,
// reached only if a real dictation session ever DELIVERS a transcript).
// That never happens in a gallery capture run -- every voice scene calls the
// real dictation_am_start() via nav_go_voice() but then immediately calls
// dictation_am_cancel() before any delivery callback can fire (see gallery.c's
// own comment on that convention) -- so these stub bodies
// are always fail-soft and are never exercised with real user input in this
// build. In a normal (non-gallery) build this whole file is empty (`#ifdef
// GALLERY_MODE` around everything) and lib/parse_nl.c supplies the real
// symbols instead -- no behavior change, no symbol clash either way.
#ifdef GALLERY_MODE

#include "lib/parse_nl.h"

#include <string.h>

bool parse_nl(const char *transcript, NlParse *out) {
  (void)transcript;
  memset(out, 0, sizeof(*out));
  return false;
}

int32_t nl_clock_target_to_duration(const NlClockTarget *target, int now_sod,
                                     int32_t *out_target_sod) {
  (void)target;
  (void)now_sod;
  if (out_target_sod) {
    *out_target_sod = 0;
  }
  // parse_nl.h's own contract: "Always returns a value in 1..86400." Never
  // actually reached with a real target in this build (see file comment).
  return 1;
}

#endif  // GALLERY_MODE
