#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Herman Persitz

# Vendors ThrowTheSwitch/Unity's 3 source files into test/vendor/unity/ at a
# pinned, checksum-verified version. Non-root, no system packages, idempotent
# -- same pattern as harness/bin/setup-libs.sh (pinned version + content
# hash, durable + reproducible).
set -euo pipefail
UNITY_VERSION="v2.6.1"
DEST="$(cd "$(dirname "$0")/.." && pwd)/test/vendor/unity"
BASE="https://raw.githubusercontent.com/ThrowTheSwitch/Unity/${UNITY_VERSION}/src"
declare -A SHA=(
  [unity.c]="b90e735a54cf3b3765ab6caa955d11a1488ee73d9c6152cdc98576c2d17cb871"
  [unity.h]="9db174d3c2c6424fd35c0980c5941d124c5ebb0f48e8172f997a2aa9554b64ea"
  [unity_internals.h]="fcd8b3f6b412ac0ab599547eb8a30b6d7f3f0af77aab31f7a1822a2a8fc9a2b2"
)
verify() { echo "${SHA[$1]}  $DEST/$1" | sha256sum -c --status; }

all_ok=1
for f in "${!SHA[@]}"; do { [ -f "$DEST/$f" ] && verify "$f"; } || all_ok=0; done
if [ "$all_ok" = 1 ]; then echo "unity $UNITY_VERSION already vendored + verified at $DEST"; exit 0; fi

mkdir -p "$DEST"
for f in "${!SHA[@]}"; do
  curl -LsSf "$BASE/$f" -o "$DEST/$f"
  verify "$f" || { echo "CHECKSUM MISMATCH for $f" >&2; exit 1; }
done
echo "vendored + verified Unity $UNITY_VERSION into $DEST"
