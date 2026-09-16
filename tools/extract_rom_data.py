#!/usr/bin/env python3
"""Extract the data package used by the Amiga port from an XEGS RoF cartridge.

The input is the user-supplied 65,536-byte v5.0 ``rof.rom``.  The output is not
redistributable and is therefore ignored by git.  It is used only by
``make standalone ROM=...``; the WHDLoad slave reads the same three ranges
directly from the installed complete ROM.
"""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path

from rof_data_layout import PACKAGE_SIZE, ROM_RANGES, ROM_SHA256, ROM_SIZE


def checked_rom(path: Path) -> bytes:
    data = path.read_bytes()
    if len(data) != ROM_SIZE:
        raise SystemExit(f"{path}: expected {ROM_SIZE} bytes, got {len(data)}")
    digest = hashlib.sha256(data).hexdigest()
    if digest != ROM_SHA256:
        raise SystemExit(
            f"{path}: unsupported cartridge image\n"
            f"  expected sha256 {ROM_SHA256}\n"
            f"       got sha256 {digest}"
        )
    return data


def extract(data: bytes) -> bytes:
    package = b"".join(data[off:off + size] for off, size in ROM_RANGES)
    assert len(package) == PACKAGE_SIZE
    return package


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rom", type=Path, help="the 64 KB XEGS v5.0 cartridge image")
    parser.add_argument("output", nargs="?", type=Path,
                        default=Path("amiga/generated/rof_data.bin"))
    parser.add_argument("--check", action="store_true",
                        help="verify that OUTPUT already equals the extraction")
    parser.add_argument("--describe", action="store_true",
                        help="print the range layout without writing OUTPUT")
    args = parser.parse_args()

    package = extract(checked_rom(args.rom))
    out_off = 0
    for rom_off, size in ROM_RANGES:
        print(f"ROM ${rom_off:04X}-${rom_off + size - 1:04X} -> package "
              f"${out_off:04X}-${out_off + size - 1:04X} ({size} bytes)")
        out_off += size
    print(f"package: {len(package)} bytes, sha256 {hashlib.sha256(package).hexdigest()}")

    if args.describe:
        return 0
    if args.check:
        current = args.output.read_bytes()
        if current != package:
            raise SystemExit(f"{args.output}: stale or made from another ROM")
        print(f"verified {args.output}")
        return 0

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(package)
    print(f"wrote {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
