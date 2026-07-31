#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Herman Persitz
#
# Artifact-integrity gate for the built .pbw.
#
# Why this exists: LTO (see wscript's CFLAGS/LINKFLAGS append in the platform
# build loop) can delete the generated PebbleProcessInfo header object
# (`__pbl_app_info`, in build/emery/appinfo.auto.c) before the linker
# script's `KEEP(*(.pbl_header))` ever protects it, because no C code
# references that object. When that happens, `pebble build` still exits 0,
# `inject_metadata.py` still runs and writes fields at fixed struct offsets
# without ever validating what it's writing into, and a `.pbw` is still
# produced -- silently corrupt (wrong/missing header, garbage UUID) rather
# than failing loudly. `-Wl,-u,__pbl_app_info` in wscript is the fix for
# that specific class; THIS script is the independent verification that the
# fix (or any future equivalent) actually held, checked directly against the
# produced artifact rather than trusted by construction. It is deliberately
# NOT gated on whether LTO is enabled -- a build-system regression of any
# kind (not just LTO) that drops the header object should fail here too.
#
# Checks performed against each platform binary extracted from the .pbw
# (not just the intermediate .elf -- the .pbw is the actual shipped
# artifact), per the struct layout in the installed SDK's own
# sdk-core/pebble/common/tools/inject_metadata.py (PebbleProcessInfo,
# src/fw/app_management/pebble_process_info.h):
#
#   1. extract the binary from the .pbw itself, not just the .elf.
#   2. assert the header's first 8 bytes are exactly b"PBLAPP\x00\x00".
#   3. assert the header UUID (offset 0x68, 16 bytes) matches appinfo.json's
#      `uuid`.
#   4. parse load_size (0xE, u16 LE), num_reloc_entries (0x64, u32 LE), and
#      virtual_size (0x80, u16 LE) out of the header.
#   5. assert the binary's total length equals
#      load_size + 4*num_reloc_entries (inject_metadata.py appends the
#      relocation table as raw u32 LE words directly after the load image).
#   6. assert every relocation-table entry is 4-byte aligned and falls
#      within [0, virtual_size) -- catches a case like
#      `-flto-partition=none`'s observed num_reloc_entries==0, and any
#      relocation pointing outside the loaded/virtual image.
#   7. assert virtual_size >= load_size. virtual_size is the end of .bss and
#      load_size is the end of the loaded image, so a virtual_size BELOW
#      load_size means firmware would carve the app heap out of the app's own
#      .text/.data. This needs no external input, so it holds for every
#      invocation, including one against a downloaded .pbw with no build tree.
#   8. cross-check virtual_size against the AUTHORITATIVE value recomputed
#      from the matching .elf, and cross-check manifest.json's `size` field
#      against the extracted binary length.
#
# Any violation exits non-zero with a clear message -- this must fail the
# build loudly, never degrade to a warning, for every check except the
# manifest-size cross-check (a WARN; it is informational bookkeeping, not a
# loadability property).
#
# -- On check 8, and why it is now a hard FAIL rather than the WARN the
#    original version of this file described -----------------------------
#
# The original text here promised a comparison against `arm-none-eabi-size`'s
# Berkeley `dec` column, called it "not a failure by itself" because the two
# "can differ by a few bytes under LTO," and then never implemented the
# --elf flag at all: the flag was documented, `argparse` never defined it,
# and invoking it as documented
# errored with `unrecognized arguments: --elf`). So the one header field
# whose corruption this script exists to catch had NO semantic validation
# beyond "relocations fall below it," which does not constrain the end of
# .bss at all -- exactly what virtual_size means and what firmware uses to
# decide where the app's heap starts. A negative control proved the hole:
# lowering only virtual_size in an otherwise intact PBW passed.
#
# The observed few-byte discrepancy was real but was an artifact of the
# WRONG COMPARAND. `size`'s `dec` sums section SIZES; the SDK's own
# inject_metadata.py `get_virtual_size()` takes the .bss section's
# ADDRESS + SIZE (falling back to .data's when there is no .bss), which
# additionally includes any alignment padding between sections. On this
# project's current build those differ by exactly 1 byte (dec 59,483 vs.
# virtual_size 59,484). Recomputing what inject_metadata.py itself computes
# makes the comparison EXACT, so it can be -- and is -- an equality check
# and a hard failure, with no tolerance to tune and nothing to explain away.
#
# The ELF is parsed here directly (pure `struct` reads of the ELF32 section
# header table) rather than shelling out to arm-none-eabi-readelf, so this
# check needs no toolchain on PATH and runs anywhere the .pbw and .elf are.
#
# Because check 8 is the ONLY independent validation of virtual_size, it is
# required by default: the ELF is auto-discovered next to the .pbw, and its
# absence is a failure rather than a silent skip. `--skip-elf-check` is the
# explicit, auditable escape hatch for checking a .pbw with no build tree.
#
# Usage:
#   check_pbw_integrity.py build/app.pbw [--elf build/emery/pebble-app.elf]
#                                        [--skip-elf-check]
#
# Exit 0 on a clean artifact, non-zero (with a specific error printed) on
# any structural problem.

import argparse
import os
import struct
import sys
import zipfile

HEADER_ADDR = 0x0
HEADER_LEN = 8
EXPECTED_HEADER = b"PBLAPP\x00\x00"

LOAD_SIZE_ADDR = 0xE
NUM_RELOC_ENTRIES_ADDR = 0x64
UUID_ADDR = 0x68
UUID_LEN = 16
VIRTUAL_SIZE_ADDR = 0x80
STRUCT_SIZE_BYTES = 0x82


class IntegrityError(Exception):
    pass


def elf_virtual_size(elf_path):
    """The authoritative virtual_size for `elf_path`, recomputed exactly the
    way the SDK's own inject_metadata.py get_virtual_size() does: the end
    address (sh_addr + sh_size) of `.bss`, or of `.data` when there is no
    `.bss` section.

    Deliberately NOT arm-none-eabi-size's Berkeley `dec` column -- see this
    file's header comment for why that comparand produced a spurious
    few-byte discrepancy and turned a hard check into a WARN that was then
    never implemented.
    """
    with open(elf_path, "rb") as f:
        data = f.read()

    if len(data) < 52 or data[:4] != b"\x7fELF":
        raise IntegrityError("%s is not an ELF file" % elf_path)
    if data[4] != 1 or data[5] != 1:
        raise IntegrityError(
            "%s is not a 32-bit little-endian ELF (EI_CLASS=%d, EI_DATA=%d) -- "
            "this parser matches the SDK's arm-none-eabi output only"
            % (elf_path, data[4], data[5])
        )

    e_shoff, = struct.unpack_from("<L", data, 0x20)
    e_shentsize, = struct.unpack_from("<H", data, 0x2E)
    e_shnum, = struct.unpack_from("<H", data, 0x30)
    e_shstrndx, = struct.unpack_from("<H", data, 0x32)
    if e_shoff == 0 or e_shnum == 0:
        raise IntegrityError("%s has no section header table" % elf_path)

    def shdr(i):
        off = e_shoff + i * e_shentsize
        name, _type, _flags, addr, _offset, size = struct.unpack_from("<LLLLLL", data, off)
        return name, addr, size

    # The section-name string table's own file offset (sh_offset, at +0x10
    # within its section header).
    strtab_off, = struct.unpack_from("<L", data, e_shoff + e_shstrndx * e_shentsize + 0x10)

    def name_at(name_off):
        end = data.index(b"\0", strtab_off + name_off)
        return data[strtab_off + name_off : end].decode("ascii", "replace")

    sections = {}
    for i in range(e_shnum):
        name_off, addr, size = shdr(i)
        sections[name_at(name_off)] = (addr, size)

    for candidate in (".bss", ".data"):
        if candidate in sections:
            addr, size = sections[candidate]
            return addr + size, candidate

    raise IntegrityError(
        "%s has neither a .bss nor a .data section -- cannot recompute "
        "virtual_size the way inject_metadata.py does" % elf_path
    )


def format_uuid(raw16):
    if len(raw16) != 16:
        raise IntegrityError("UUID field is not 16 bytes: %r" % raw16)
    hexs = raw16.hex()
    return "-".join(
        [hexs[0:8], hexs[8:12], hexs[12:16], hexs[16:20], hexs[20:32]]
    )


def check_binary(platform, data, expected_uuid, manifest_size=None, elf_path=None):
    problems = []

    if len(data) < STRUCT_SIZE_BYTES:
        raise IntegrityError(
            "[%s] binary is only %d bytes, smaller than the %d-byte "
            "PebbleProcessInfo struct -- not a valid app image at all"
            % (platform, len(data), STRUCT_SIZE_BYTES)
        )

    header = data[HEADER_ADDR : HEADER_ADDR + HEADER_LEN]
    if header != EXPECTED_HEADER:
        raise IntegrityError(
            "[%s] header sentinel mismatch: expected %r, got %r -- this is "
            "exactly the silent-corruption signature seen when LTO deletes "
            "__pbl_app_info before the linker's KEEP(*(.pbl_header)) can "
            "save it (see wscript's -Wl,-u,__pbl_app_info comment). The "
            "build reported success; the artifact is not loadable firmware."
            % (platform, EXPECTED_HEADER, header)
        )

    (load_size,) = struct.unpack_from("<H", data, LOAD_SIZE_ADDR)
    (num_reloc_entries,) = struct.unpack_from("<L", data, NUM_RELOC_ENTRIES_ADDR)
    (virtual_size,) = struct.unpack_from("<H", data, VIRTUAL_SIZE_ADDR)
    uuid_raw = data[UUID_ADDR : UUID_ADDR + UUID_LEN]
    uuid = format_uuid(uuid_raw)

    if expected_uuid is not None and uuid.lower() != expected_uuid.lower():
        raise IntegrityError(
            "[%s] header UUID %s does not match appinfo.json's %s"
            % (platform, uuid, expected_uuid)
        )

    expected_total = load_size + 4 * num_reloc_entries
    if len(data) != expected_total:
        raise IntegrityError(
            "[%s] binary length %d != load_size(%d) + 4*num_reloc_entries"
            "(%d)=%d -- the relocation table is truncated, oversized, or "
            "the header's own load_size/num_reloc_entries fields are wrong"
            % (platform, len(data), load_size, num_reloc_entries, expected_total)
        )

    reloc_bytes = data[load_size : load_size + 4 * num_reloc_entries]
    for i in range(num_reloc_entries):
        (entry,) = struct.unpack_from("<L", reloc_bytes, i * 4)
        if entry % 4 != 0:
            raise IntegrityError(
                "[%s] relocation entry #%d (0x%08x) is not 4-byte aligned"
                % (platform, i, entry)
            )
        if entry >= virtual_size:
            raise IntegrityError(
                "[%s] relocation entry #%d (0x%08x) falls outside the "
                "loaded/virtual image (virtual_size=0x%x) -- points past "
                "the end of the app's own memory"
                % (platform, i, entry, virtual_size)
            )

    # A relocatable app with an empty relocation table is the exact shape
    # `-flto-partition=none` produced in testing (superficially
    # valid header + UUID, but num_reloc_entries collapsed to 0, then an
    # immediate runtime crash). A valid header/UUID/length triple is
    # therefore NOT sufficient on its own; flag a zero-relocation binary
    # loudly rather than silently accepting it, since this check guards
    # against exactly that class of "everything else checked out" false
    # pass. This project's own binary has always carried relocations
    # (PIE code with static/global data), so zero here is a strong signal
    # something upstream (partitioning, a future toolchain change) silently
    # broke relocation generation.
    if num_reloc_entries == 0:
        raise IntegrityError(
            "[%s] num_reloc_entries is 0 -- this app has always required "
            "PIE relocations (static/global data + PIE code); a zero-length "
            "relocation table is the same failure signature "
            "-flto-partition=none produced (valid sentinel/UUID, then an "
            "immediate on-device crash). Treating this as corrupt."
            % (platform,)
        )

    # Check 7 -- ELF-free, so it holds on every invocation. virtual_size is
    # the end of .bss; load_size is the end of the loaded image, which .bss
    # sits at or after. A virtual_size below load_size means firmware would
    # begin the app's heap INSIDE the app's own .text/.data.
    if virtual_size < load_size:
        raise IntegrityError(
            "[%s] virtual_size (%d) is BELOW load_size (%d) -- firmware would "
            "carve this app's heap out of its own loaded image. The header's "
            "virtual_size field is corrupt."
            % (platform, virtual_size, load_size)
        )

    # Check 8 -- the only independent validation of virtual_size there is.
    if elf_path is not None:
        expected_vsize, source_section = elf_virtual_size(elf_path)
        if expected_vsize != virtual_size:
            raise IntegrityError(
                "[%s] header virtual_size (%d) != the value recomputed from "
                "%s (%d, = end of %s). These are computed the same way by the "
                "SDK's own inject_metadata.py, so they must match EXACTLY; a "
                "mismatch means the packed header no longer describes this "
                "binary's real end-of-static-memory and firmware would split "
                "loaded image from heap in the wrong place."
                % (platform, virtual_size, elf_path, expected_vsize, source_section)
            )

    if manifest_size is not None and manifest_size != len(data):
        problems.append(
            "[%s] WARN: manifest.json application.size (%d) != extracted "
            "binary length (%d)" % (platform, manifest_size, len(data))
        )

    print(
        "[%s] OK: PBLAPP sentinel valid, uuid=%s, load_size=%d, "
        "num_reloc_entries=%d, virtual_size=%d, total=%d bytes"
        % (platform, uuid, load_size, num_reloc_entries, virtual_size, len(data))
    )
    for p in problems:
        print(p, file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pbw", help="Path to the built .pbw bundle")
    parser.add_argument(
        "--elf",
        action="append",
        default=[],
        metavar="[PLATFORM=]PATH",
        help="ELF to cross-check virtual_size against. Repeatable, and may be "
        "prefixed with PLATFORM= when the .pbw carries more than one. "
        "Defaults to <pbw dir>/<platform>/pebble-app.elf.",
    )
    parser.add_argument(
        "--skip-elf-check",
        action="store_true",
        help="Explicitly skip the virtual_size cross-check (check 8). Only for "
        "checking a .pbw with no build tree beside it -- it disables the ONLY "
        "independent validation of the virtual_size header field.",
    )
    args = parser.parse_args()

    explicit_elfs = {}
    for spec in args.elf:
        if "=" in spec:
            plat, _, path = spec.partition("=")
            explicit_elfs[plat] = path
        else:
            explicit_elfs[None] = spec

    def resolve_elf(platform):
        if args.skip_elf_check:
            return None
        if platform in explicit_elfs:
            path = explicit_elfs[platform]
        elif None in explicit_elfs:
            path = explicit_elfs[None]
        else:
            path = os.path.join(
                os.path.dirname(os.path.abspath(args.pbw)), platform, "pebble-app.elf"
            )
        if not os.path.exists(path):
            raise IntegrityError(
                "[%s] no ELF found at %s, so the virtual_size cross-check "
                "(check 8) cannot run -- and it is the only independent "
                "validation of that field. Pass --elf [PLATFORM=]PATH, or "
                "--skip-elf-check to waive it deliberately." % (platform, path)
            )
        return path

    with zipfile.ZipFile(args.pbw) as z:
        names = z.namelist()
        if "appinfo.json" not in names:
            raise IntegrityError("%s has no appinfo.json" % args.pbw)

        import json

        appinfo = json.loads(z.read("appinfo.json").decode("utf8"))
        expected_uuid = appinfo.get("uuid")
        platforms = appinfo.get("targetPlatforms") or []
        if not platforms:
            raise IntegrityError(
                "appinfo.json has no targetPlatforms -- nothing to check"
            )

        for platform in platforms:
            bin_name = "%s/pebble-app.bin" % platform
            manifest_name = "%s/manifest.json" % platform
            if bin_name not in names:
                raise IntegrityError(
                    "%s is missing %s (declared in targetPlatforms)"
                    % (args.pbw, bin_name)
                )
            data = z.read(bin_name)

            manifest_size = None
            if manifest_name in names:
                manifest = json.loads(z.read(manifest_name).decode("utf8"))
                manifest_size = manifest.get("application", {}).get("size")

            check_binary(platform, data, expected_uuid, manifest_size, resolve_elf(platform))

    if args.skip_elf_check:
        print(
            "check_pbw_integrity: WARN: --skip-elf-check was passed; virtual_size "
            "was NOT independently validated.",
            file=sys.stderr,
        )
    print("check_pbw_integrity: all platform binaries structurally valid.")


if __name__ == "__main__":
    try:
        main()
    except IntegrityError as e:
        print("check_pbw_integrity: FAIL: %s" % e, file=sys.stderr)
        sys.exit(1)
    except (zipfile.BadZipFile, FileNotFoundError, KeyError, struct.error) as e:
        print("check_pbw_integrity: FAIL: %s: %s" % (type(e).__name__, e), file=sys.stderr)
        sys.exit(1)
