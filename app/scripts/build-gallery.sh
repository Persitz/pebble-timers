#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Herman Persitz

# Builds the app with the #ifdef-gated screen gallery in src/c/gallery.c.
# Gallery mode boots directly into the scripted screen-state scene table.
# Release builds do not set PEBBLE_GALLERY_MODE, so wscript leaves the
# gallery define disabled and the plain `pebble build` output unchanged.
#
# Usage: cd app && ./scripts/build-gallery.sh
# Equivalent to: cd app && PEBBLE_GALLERY_MODE=1 pebble build
set -euo pipefail

APP_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$APP_DIR"

echo "[build-gallery] building with GALLERY_MODE=1 (src/c/gallery.c active)..." >&2
PEBBLE_GALLERY_MODE=1 pebble build "$@"
