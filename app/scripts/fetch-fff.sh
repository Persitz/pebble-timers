#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Herman Persitz

# Vendors meekrosoft/fff's single header (fff.h) into test/vendor/ at a
# pinned, checksum-verified version. Non-root, no system packages, idempotent
# -- same pattern as fetch-unity.sh / harness/bin/setup-libs.sh. FFF is the
# adapter-seam fake-function library: most lib/ units are pure C with no
# Pebble SDK calls to fake, but the handful of adapter files that do call
# into the SDK (e.g. AppMessage/AppTimer/click-config) use FFF to fake those
# calls in their host-side unit tests.
set -euo pipefail
FFF_VERSION="v1.1"
DEST="$(cd "$(dirname "$0")/.." && pwd)/test/vendor"
URL="https://raw.githubusercontent.com/meekrosoft/fff/${FFF_VERSION}/fff.h"
SHA="61daabe2b8897b5b84a2486d295f7401009f7b805f7031fe3c8b174dd7646d40"

verify() { echo "$SHA  $DEST/fff.h" | sha256sum -c --status; }

if [ -f "$DEST/fff.h" ] && verify; then
  echo "fff $FFF_VERSION already vendored + verified at $DEST/fff.h"
  exit 0
fi

mkdir -p "$DEST"
curl -LsSf "$URL" -o "$DEST/fff.h"
verify || { echo "CHECKSUM MISMATCH for fff.h" >&2; exit 1; }
echo "vendored + verified fff $FFF_VERSION into $DEST/fff.h"
