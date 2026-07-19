// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

#include "fonts_adapter.h"

static GFont s_jbm_42;
static GFont s_jbm_32;
static GFont s_jbm_56;

void fonts_adapter_init(void) {
  // Exactly one fonts_load_custom_font() call per raster, at app
  // start -- never per-window, never per-frame. A failed load (NULL) is
  // handled by the accessor functions below, not here.
  s_jbm_42 = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_JBM_BOLD_42));
  s_jbm_32 = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_JBM_BOLD_32));
  // The 42px raster's actual digit cap height (31px) reads one size class
  // smaller than intended for the hero numerals -- a 56px raster is the
  // app's single highest-visibility numeral site (running/overtime/
  // stopwatch heroes), so it's bundled despite the flash cost (see
  // fonts_adapter.h's doc for the font cost accounting).
  s_jbm_56 = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_JBM_BOLD_56));
}

void fonts_adapter_deinit(void) {
  if (s_jbm_42 != NULL) {
    fonts_unload_custom_font(s_jbm_42);
    s_jbm_42 = NULL;
  }
  if (s_jbm_32 != NULL) {
    fonts_unload_custom_font(s_jbm_32);
    s_jbm_32 = NULL;
  }
  if (s_jbm_56 != NULL) {
    fonts_unload_custom_font(s_jbm_56);
    s_jbm_56 = NULL;
  }
}

GFont fonts_adapter_numeral_42(void) {
  return (s_jbm_42 != NULL) ? s_jbm_42 : fonts_get_system_font(FONT_KEY_LECO_42_NUMBERS);
}

GFont fonts_adapter_numeral_32(void) {
  return (s_jbm_32 != NULL) ? s_jbm_32 : fonts_get_system_font(FONT_KEY_LECO_32_BOLD_NUMBERS);
}

GFont fonts_adapter_numeral_56(void) {
  // No system LECO raster reaches 56px -- fall back to the next-largest
  // bundled/system numeral font rather than drawing nothing if the resource
  // failed to load.
  return (s_jbm_56 != NULL) ? s_jbm_56 : fonts_adapter_numeral_42();
}
