#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Herman Persitz

# Size gate for the on-watch build.
# Measures build/emery/pebble-app.elf via the SDK-bundled arm-none-eabi-size
# and reports flash (.text+.rodata) and static RAM (.data+.bss) footprint
# against emery's ~128 KB (131072-byte) app code+heap budget, PLUS the real
# build-breaking wall: `virtual_size` (.text+.data+.got+.got.plt+.bss), which the SDK's
# `inject_metadata.py` packs into a `uint16` -- crossing 65535 crashes the
# build (`struct.error`, no `.pbw`) regardless of how healthy the static-RAM
# guardrail below looks. This is now the PRIMARY, hard gate; the static-RAM
# check is a secondary tracked metric only (see its own comment below).
#
# Gate shape is otherwise measure-and-ratchet, not a hard byte cap -- C has
# no XS symbol-count wall. The concrete initial guardrail is static
# .data+.bss < ~24 KB (leaving >100 KB
# of heap headroom): crossing it is a REVIEW TRIGGER (warning, exit 0), not
# an auto-fail. The only hard failure on THAT metric is static RAM alone
# exceeding the platform's absolute 128 KB ceiling, which would mean zero
# heap left for any app at all -- a sanity backstop, not the day-to-day
# gate. The virtual_size check above is different: it IS the day-to-day
# gate now, since it protects the wall this app actually sits closest to.
set -euo pipefail

APP_DIR="$(cd "$(dirname "$0")/.." && pwd)"
ELF="${1:-$APP_DIR/build/emery/pebble-app.elf}"

SIZE_TOOL="${ARM_NONE_EABI_SIZE:-}"
if [ -z "$SIZE_TOOL" ]; then
  if command -v arm-none-eabi-size >/dev/null 2>&1; then
    SIZE_TOOL="$(command -v arm-none-eabi-size)"
  else
    SIZE_TOOL="$HOME/.local/share/pebble-sdk/SDKs/current/toolchain/arm-none-eabi/bin/arm-none-eabi-size"
  fi
fi

if [ ! -x "$SIZE_TOOL" ]; then
  echo "error: arm-none-eabi-size not found at '$SIZE_TOOL' -- is the Pebble SDK installed? (run 'pebble sdk install latest')" >&2
  exit 1
fi

if [ ! -f "$ELF" ]; then
  echo "error: $ELF not found -- run 'pebble build' first." >&2
  exit 1
fi

STATIC_RAM_BUDGET_WARN=$((24 * 1024))
APP_HARD_CEILING=$((128 * 1024))

# --- virtual_size wall (the hard uint16 build-breaker) ---
# vsize = .text + .data + .got + .got.plt + .bss, exactly the plain Berkeley
# `size <elf>` `dec` column -- NOT reconstructed by summing individual named
# sections out of `-A`'s per-section listing: Berkeley `size` buckets
# sections by ELF flags (SEC_CODE/SEC_READONLY -> "text", writable+alloc ->
# "data", alloc-no-contents -> "bss"), not by name, so its "text"/"data"
# buckets also silently fold in a handful of small non-`.text`/`.data`-named
# sections (`.header`, `.note.gnu.build-id`, `.init`, `.fini`,
# `.ARM.exidx.*`, on this toolchain) that a literal `.text|.data|...` name
# match on `-A` output would miss -- on this toolchain's builds that gap runs
# to roughly 184 B off the real `dec` value. `size <elf>`'s own dec column is
# exact by construction (same computation `inject_metadata.py` packs into the
# uint16), so this calls it directly rather than re-deriving it.
# Written by the SDK's inject_metadata.py as struct.pack("<H", vsize);
# vsize > 65535 crashes the build and no .pbw is produced.
VSIZE_WALL=65535
VSIZE_FAIL=65200      # < 335 B to the wall -> reclaim now
VSIZE_WARN=64512      # < ~1 KB headroom  -> budget every new feature

RAW_OUTPUT="$("$SIZE_TOOL" -A "$ELF")"
echo "$RAW_OUTPUT"
echo

vsize="$("$SIZE_TOOL" "$ELF" | awk 'NR==2 {print $4}')"

flash_bytes=0
static_ram_bytes=0
while read -r name size _addr; do
  case "$name" in
    .text|.rodata) flash_bytes=$((flash_bytes + size)) ;;
    .data|.bss) static_ram_bytes=$((static_ram_bytes + size)) ;;
  esac
done < <(echo "$RAW_OUTPUT" | awk 'NF==3 && $2 ~ /^[0-9]+$/ {print $1, $2, $3}')

echo "-------------------------------------------------------"
echo "SIZE GATE (${ELF#"$APP_DIR"/})"
echo "  virtual_size (uint16 wall):  $vsize bytes   (wall: $VSIZE_WALL; headroom: $((VSIZE_WALL - vsize)) B)"
echo "  flash (.text+.rodata):      $flash_bytes bytes   (tracked, non-binding)"
echo "  static RAM (.data+.bss):    $static_ram_bytes bytes   (guardrail: < $STATIC_RAM_BUDGET_WARN bytes; hard ceiling: $APP_HARD_CEILING bytes)"
echo "-------------------------------------------------------"

status=0

if [ "$vsize" -ge "$VSIZE_FAIL" ]; then
  echo "FAIL: virtual_size ($vsize) is within $((VSIZE_WALL - vsize)) B of the $VSIZE_WALL-byte uint16 wall. The build breaks (inject_metadata crashes, no .pbw) at >$VSIZE_WALL. Reclaim now." >&2
  status=1
elif [ "$vsize" -ge "$VSIZE_WARN" ]; then
  echo "REVIEW TRIGGER: virtual_size ($vsize) is within $((VSIZE_WALL - vsize)) B of the $VSIZE_WALL-byte wall -- every new feature now needs a byte budget." >&2
else
  echo "PASS: virtual_size within the healthy band."
fi

if [ "$static_ram_bytes" -ge "$APP_HARD_CEILING" ]; then
  echo "FAIL: static RAM ($static_ram_bytes bytes) meets/exceeds the platform's absolute $APP_HARD_CEILING-byte app budget -- zero heap headroom would remain." >&2
  status=1
elif [ "$static_ram_bytes" -ge "$STATIC_RAM_BUDGET_WARN" ]; then
  echo "REVIEW TRIGGER: static RAM ($static_ram_bytes bytes) has crossed the $STATIC_RAM_BUDGET_WARN-byte guardrail -- not an auto-fail, but worth a closer look at what grew." >&2
else
  echo "PASS: static RAM within guardrail (secondary metric)."
fi

exit $status
