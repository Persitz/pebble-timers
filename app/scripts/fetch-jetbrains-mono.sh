#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Herman Persitz

# Vendors JetBrains Mono Bold (SIL OFL-1.1 -- freely bundleable) into
# resources/fonts/ at a pinned, checksum-verified version. Non-root, no
# system packages, idempotent -- same pattern as fetch-unity.sh/fetch-fff.sh
# (pinned tag + content hash, durable + reproducible via
# raw.githubusercontent.com at an immutable tag ref).
#
# Used by fonts_adapter.c to swap the LECO segmented-LCD numerals for JBM at
# the app's 3 most-visible sizes (hero value, SET active field, HOME 1-timer
# row).
set -euo pipefail
JBM_VERSION="v2.304"
DEST="$(cd "$(dirname "$0")/.." && pwd)/resources/fonts"
BASE="https://raw.githubusercontent.com/JetBrains/JetBrainsMono/${JBM_VERSION}"
declare -A SHA=(
  [JetBrainsMono-Bold.ttf]="5590990c82e097397517f275f430af4546e1c45cff408bde4255dad142479dcb"
  [OFL.txt]="30f0c136e3c88e422d0791acd97238870f9054a9729bc34cf2ff0d4ed8cac4ad"
)
declare -A SRC=(
  [JetBrainsMono-Bold.ttf]="fonts/ttf/JetBrainsMono-Bold.ttf"
  [OFL.txt]="OFL.txt"
)
verify() { echo "${SHA[$1]}  $DEST/$1" | sha256sum -c --status; }

all_ok=1
for f in "${!SHA[@]}"; do { [ -f "$DEST/$f" ] && verify "$f"; } || all_ok=0; done
if [ "$all_ok" = 1 ]; then
  echo "JetBrains Mono $JBM_VERSION already vendored + verified at $DEST"
  exit 0
fi

mkdir -p "$DEST"
for f in "${!SHA[@]}"; do
  curl -LsSf "$BASE/${SRC[$f]}" -o "$DEST/$f"
  verify "$f" || { echo "CHECKSUM MISMATCH for $f" >&2; exit 1; }
done
echo "vendored + verified JetBrains Mono Bold $JBM_VERSION into $DEST"
