#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Herman Persitz

# Builds the app WITH the screen gallery compiled in (src/c/gallery.c, an
# #ifdef-gated tool that boots straight into a scripted tour of app screen
# states for fast visual verification -- one gallery boot that then
# navigates through the full scene table, instead of navigating there by
# hand). Never used for a release build; the plain
# `pebble build` (no env var) is untouched by this and produces a
# byte-identical app_elf compile (wscript's `defines` list is empty unless
# PEBBLE_GALLERY_MODE=1 is set -- see wscript's build() for the toggle).
#
# Usage: cd app && ./scripts/build-gallery.sh
# Equivalent to: cd app && PEBBLE_GALLERY_MODE=1 pebble build
set -euo pipefail

APP_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$APP_DIR"

echo "[build-gallery] building with GALLERY_MODE=1 (src/c/gallery.c active)..." >&2
PEBBLE_GALLERY_MODE=1 pebble build "$@"
