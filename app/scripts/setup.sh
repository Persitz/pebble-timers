#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Herman Persitz

# Canonical, idempotent, one-command bootstrap for this project from a clean
# clone: initializes the harness/ git submodule, installs uv, a uv-managed
# Python (system python3 lacks ensurepip), pebble-tool (excluding its
# Linux/aarch64-incompatible pypkjs dependency -- pypkjs depends on stpyv8, a
# V8 binding with no published Linux/aarch64 wheels, and pebble-tool has no
# flag to skip it), the Pebble SDK, npm install (keeps package-lock.json
# current; app/package.json itself declares no devDependencies), and the
# emulator harness's shared-library workaround.
#
# Safe to re-run: every step checks whether its target already exists/works
# before doing anything.
#
# This automates exactly the manual install steps below -- each numbered
# section explains the reasoning for that step inline.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
APP_DIR="$REPO_ROOT/app"

PEBBLE_TOOL_VERSION="5.0.39"
PYTHON_VERSION="3.11"

step() { echo; echo "=== $* ==="; }

# --- 0. harness/ (git submodule -- published standalone at ---------------
#        github.com/Persitz/pebble-testpilot; NOT in-tree source) ----------
# Every later step (setup-libs.sh, setup-ocr.sh, emu-verify.sh, ...) shells
# out into harness/bin/*.sh -- if the submodule isn't populated yet, those
# fail with a confusing "no such file or directory" deep into the run. Do
# this check first and fail loudly with an actionable message instead.
step "harness/ (git submodule)"
HARNESS_MARKER="$REPO_ROOT/harness/bin/emu-verify.sh"
if [ -e "$HARNESS_MARKER" ]; then
  echo "harness/ already populated (found harness/bin/emu-verify.sh)."
elif [ -e "$REPO_ROOT/.git" ] && [ -f "$REPO_ROOT/.gitmodules" ]; then
  echo "harness/ not populated yet -- running 'git submodule update --init --recursive'..."
  if ! git -C "$REPO_ROOT" submodule update --init --recursive; then
    echo "error: 'git submodule update --init --recursive' failed -- harness/ could not be" >&2
    echo "populated. Check network access / the submodule URL in .gitmodules" \
      "(github.com/Persitz/pebble-testpilot) and re-run." >&2
    exit 1
  fi
  if [ ! -e "$HARNESS_MARKER" ]; then
    echo "error: ran 'git submodule update --init --recursive' but harness/bin/emu-verify.sh" >&2
    echo "still doesn't exist -- the submodule checkout is incomplete or broken. Inspect" \
      "'git -C \"$REPO_ROOT\" submodule status' by hand before continuing." >&2
    exit 1
  fi
  echo "harness/ populated."
else
  echo "error: harness/ is empty (no harness/bin/emu-verify.sh) and this doesn't look like a" >&2
  echo "normal git clone with a .gitmodules file (got this from a source archive/zip export?)." >&2
  echo "Fetch the harness yourself before re-running this script -- either re-clone this repo" \
    "with 'git clone --recurse-submodules', or run 'git submodule update --init --recursive'" \
    "from a proper git checkout, or manually clone https://github.com/Persitz/pebble-testpilot" \
    "into '$REPO_ROOT/harness'." >&2
  exit 1
fi

# --- 1. uv --------------------------------------------------------------
step "uv"
if command -v uv >/dev/null 2>&1; then
  echo "uv already installed: $(uv --version)"
else
  curl -LsSf https://astral.sh/uv/install.sh | sh
  export PATH="$HOME/.local/bin:$PATH"
fi
export PATH="$HOME/.local/bin:$PATH"
if ! command -v uv >/dev/null 2>&1; then
  echo "error: uv installed but not on PATH -- add ~/.local/bin to PATH and re-run." >&2
  exit 1
fi

# --- 2. uv-managed Python (system python3 lacks ensurepip/venv) -------------
step "uv python install $PYTHON_VERSION"
uv python install "$PYTHON_VERSION"

# --- 3. pebble-tool, excluding pypkjs (no aarch64 stpyv8 wheels) ------------
step "pebble-tool $PEBBLE_TOOL_VERSION"
if command -v pebble >/dev/null 2>&1 && pebble --version 2>/dev/null | grep -q "$PEBBLE_TOOL_VERSION"; then
  echo "pebble-tool $PEBBLE_TOOL_VERSION already installed: $(pebble --version)"
else
  EXCLUDES_FILE="$(mktemp)"
  echo pypkjs > "$EXCLUDES_FILE"
  uv tool install "pebble-tool==$PEBBLE_TOOL_VERSION" --python "$PYTHON_VERSION" --excludes "$EXCLUDES_FILE"
  rm -f "$EXCLUDES_FILE"
fi
export PATH="$HOME/.local/bin:$PATH"
if ! command -v pebble >/dev/null 2>&1; then
  echo "error: pebble-tool installed but 'pebble' not on PATH -- add ~/.local/bin to PATH and re-run." >&2
  exit 1
fi

# --- 4. Pebble SDK (toolchain + QEMU) ---------------------------------------
step "pebble sdk install latest"
if pebble sdk list 2>/dev/null | grep -q '(active)'; then
  echo "a Pebble SDK is already installed/active: $(pebble sdk list 2>/dev/null | grep '(active)')"
else
  pebble sdk install latest
fi

# --- 5. npm install (keeps package-lock.json current; no devDependencies) ---
step "npm install"
( cd "$APP_DIR" && npm install )

# --- 6. Emulator harness shared libraries (non-root, aarch64-specific) ------
# Run with cwd = app/ so .emu-libs lands in app/, matching the harness's
# cwd-default project-root convention (see harness/README.md).
step "harness/bin/setup-libs.sh"
( cd "$APP_DIR" && "$REPO_ROOT/harness/bin/setup-libs.sh" )

# --- 7. Host unit-test stack: vendor Unity + FFF (non-root, pinned + checksum-verified) ---
# Both are committed (test/vendor/), so this step is normally a no-op verify;
# it's the provenance/update mechanism.
step "vendor Unity + FFF (test/vendor/)"
( cd "$APP_DIR" && ./scripts/fetch-unity.sh && ./scripts/fetch-fff.sh )

# --- 8. Custom font resource: JetBrains Mono Bold (non-root, pinned + checksum-verified) ---
# Committed (resources/fonts/), so this step is normally a no-op verify --
# see fetch-jetbrains-mono.sh for the provenance/update mechanism.
step "vendor JetBrains Mono Bold (resources/fonts/)"
( cd "$APP_DIR" && ./scripts/fetch-jetbrains-mono.sh )

echo
echo "=== setup.sh complete ==="
echo "Next: cd app && pebble build"
echo "Then: cd app && ../harness/bin/emu-verify.sh --out emulator-runs/screenshot.png"
echo "Host tests: cd app && npm run test:c"
