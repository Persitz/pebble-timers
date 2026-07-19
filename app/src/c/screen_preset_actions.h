// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// PRESET ACTIONS screen adapter: the small {Edit, Rename, Delete} sheet opened by holding Select
// on a Library row. Mirrors screen_actions.c's structure at a smaller scale
// (3 static rows -- see screen_preset_actions.c's own top-of-file
// comment -- no per-tick clock refresh needed).
//
// Reachable via hold-Select on a Library row
// (VD_PRESET_ACTIONS + nav.c's real window creation/apply_cookie wiring).
#pragma once

#include <pebble.h>

Window *screen_preset_actions_create(void);
void screen_preset_actions_destroy(void);

// Sets which preset (by index into PresetStore, not a cookie -- presets are
// index-addressed) this sheet's Edit/Rename/Delete rows act on. nav.c's
// apply_cookie() calls this (VD_PRESET_ACTIONS reuses ViewDesc.cookie to
// carry the preset index, documented reuse mirroring VD_SET's own cookie
// reuse for edit-interval) immediately before pushing this window.
void screen_preset_actions_set_index(int index);

// Applies `name` (taken VERBATIM, no parse_nl
// pass -- same contract as screen_voice_set_name) to the sheet's target
// preset -- preset_store_set_name() + persist_preset_adapter_save() +
// testlog("preset_rename", "ok") + a menu reload so the sheet's own row
// label (only meaningful if the sheet later grows a name display) and any
// already-drawn Library row behind it pick up the change. Called by
// dictation_am.c's deliver() on a successful DICT_SCOPE_PRESET_NAME
// re-dictation -- mirrors screen_voice_set_name's role for DICT_SCOPE_NAME
// one-for-one (dictation adapter routes the verbatim transcript to the
// screen that owns the target; the screen applies+persists+refreshes).
void screen_preset_actions_apply_name(const char *name);
