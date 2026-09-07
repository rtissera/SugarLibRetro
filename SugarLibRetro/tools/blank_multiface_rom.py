#!/usr/bin/env python3
"""Blank the Multiface II ROM byte array in the CPCCore submodule.

CPCCoreEmu/MultifaceII.cpp #includes a header holding Romantic Robot's
commercial Multiface II firmware as a C byte array, and CPCCoreEmu's
CMakeLists globs every *.cpp -- so the ROM is compiled into any binary built
from this tree, including ours, even though the Multiface is not wired up to
anything. CPCCore is MIT, but MIT cannot relicense somebody else's firmware,
and the libretro buildbot distributes binaries publicly.

The class cannot simply be dropped: Motherboard and EmulatorEngine both hold a
MultifaceII by value, so removing the translation unit breaks the link. What
this does instead is replace the array's contents with zeros while keeping its
name and length, so MultifaceII::Init()'s
    memcpy(rom_ram_, MultiROMV05, sizeof(MultiROMV05))
still compiles and still copies the right number of bytes -- just no longer
anyone's copyrighted ones.

Idempotent: running it again on an already-blanked header is a no-op. Run with
--check to test without writing (exit 1 if the ROM is still present).
"""
import argparse
import pathlib
import re
import sys

HEADER = ("Multiface Two (Romantic Robot) (8D) (AmsDOS v05)_rom.h")
MARKER = "/* Firmware blanked by tools/blank_multiface_rom.py"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--cpccore", required=True, help="path to CPCCore/CPCCoreEmu")
    ap.add_argument("--check", action="store_true", help="report only, do not modify")
    args = ap.parse_args()

    path = pathlib.Path(args.cpccore) / HEADER
    if not path.is_file():
        # Nothing to do -- upstream may have removed it.
        print(f"blank_multiface_rom: {HEADER} not present, nothing to do")
        return 0

    text = path.read_text(encoding="utf-8", errors="replace")
    if MARKER in text:
        print("blank_multiface_rom: already blanked")
        return 0

    m = re.search(r"unsigned char\s+(\w+)\s*\[\]\s*=\s*\{(.*?)\};", text, re.S)
    if not m:
        print("blank_multiface_rom: ERROR: could not find the ROM array", file=sys.stderr)
        return 2

    name = m.group(1)
    count = len(re.findall(r"0x[0-9a-fA-F]{2}", m.group(2)))
    if count == 0:
        print("blank_multiface_rom: ERROR: array parsed as empty", file=sys.stderr)
        return 2

    if args.check:
        print(f"blank_multiface_rom: {name}[] still contains {count} bytes of firmware")
        return 1

    rows = []
    for start in range(0, count, 12):
        rows.append("  " + " ".join("0x00," for _ in range(min(12, count - start))))
    blanked = (
        f"#pragma once\n\n"
        f"{MARKER}\n"
        f"   Romantic Robot's Multiface II firmware was here. It is not ours to\n"
        f"   redistribute, and CPCCoreEmu compiles this header into every binary\n"
        f"   built from the tree. Name and length are preserved so\n"
        f"   MultifaceII::Init()'s memcpy still compiles; the contents are zeros.\n"
        f"   A user who owns a Multiface II can restore their own dump here. */\n\n"
        f"unsigned char {name}[] = {{\n" + "\n".join(rows) + "\n};\n"
    )
    path.write_text(blanked, encoding="utf-8")
    print(f"blank_multiface_rom: blanked {name}[] ({count} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
