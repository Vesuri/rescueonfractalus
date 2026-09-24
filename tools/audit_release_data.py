#!/usr/bin/env python3
"""Reject an Amiga release executable which carries the user-owned game package.

This reads ELF metadata directly, so the release gate does not depend on the cross-toolchain
being in PATH. With ``--rom`` it also checks byte signatures from the local cartridge.
"""

from __future__ import annotations

import argparse
import hashlib
import struct
from pathlib import Path

from extract_rom_data import checked_rom, extract


EXPECTED = {
    "rof_game_data": ".bss",
    "rof_game_data_end": ".bss",
    "rof_data_descriptor": ".data",
}
RETIRED = (
    "amiga/assets/rof_boot_image.bin",
    "amiga/assets/cockpit_planar.bin",
    "src/rof_logo_field.h",
    "src/rof_hiscore_factory.h",
)


def cstr(blob: bytes, off: int) -> str:
    end = blob.find(b"\0", off)
    return blob[off:end].decode("ascii")


def elf_symbols(path: Path) -> tuple[dict[str, tuple[str, int]], bytes]:
    blob = path.read_bytes()
    if blob[:6] != b"\x7fELF\x01\x02":
        raise SystemExit(f"{path}: expected a 32-bit big-endian ELF")
    hdr = struct.unpack_from(">16sHHIIIIIHHHHHH", blob, 0)
    shoff, shentsize, shnum, shstrndx = hdr[6], hdr[11], hdr[12], hdr[13]
    sections = [struct.unpack_from(">IIIIIIIIII", blob, shoff + i * shentsize)
                for i in range(shnum)]
    shstr = sections[shstrndx]
    names = blob[shstr[4]:shstr[4] + shstr[5]]
    section_names = [cstr(names, sh[0]) for sh in sections]
    result: dict[str, tuple[str, int]] = {}
    for sh in sections:
        if sh[1] != 2:  # SHT_SYMTAB
            continue
        strings = sections[sh[6]]
        strtab = blob[strings[4]:strings[4] + strings[5]]
        entsize = sh[9] or 16
        for off in range(sh[4], sh[4] + sh[5], entsize):
            name, value, _size, _info, _other, sec = struct.unpack_from(">IIIBBH", blob, off)
            if name and sec < len(sections):
                result[cstr(strtab, name)] = (section_names[sec], value)
    section, value = result.get("rof_data_descriptor", ("", 0))
    if section not in section_names:
        raise SystemExit(f"{path}: rof_data_descriptor is missing")
    sh = sections[section_names.index(section)]
    desc_off = sh[4] + value - sh[3]
    return result, blob[desc_off:desc_off + 24]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    parser.add_argument("hunk", type=Path)
    parser.add_argument("--repo", type=Path, default=Path("."))
    parser.add_argument("--rom", type=Path,
                        help="optional local ROM for additional signature checks")
    args = parser.parse_args()

    symbols, descriptor = elf_symbols(args.elf)
    for name, wanted in EXPECTED.items():
        got = symbols.get(name, ("missing", 0))[0]
        if got != wanted:
            raise SystemExit(f"{args.elf}: {name} is in {got}, expected {wanted}")

    # incbin.s only reserves the cockpit atlas for ROF_ENHANCED_GRAPHICS.
    # Both symbols may be absent; a present atlas must still be entirely BSS.
    cockpit = ("rof_cockpit_planar", "rof_cockpit_planar_end")
    if any(name in symbols for name in cockpit):
        for name in cockpit:
            got = symbols.get(name, ("missing", 0))[0]
            if got != ".bss":
                raise SystemExit(f"{args.elf}: {name} is in {got}, expected .bss")

    magic0, magic1, version, _reserved, ready, _pointer, size = struct.unpack(">IIHHIII", descriptor)
    if (magic0, magic1, version, ready, size) != (0x526F4621, 0x44415441, 1, 0, 0xA6E6):
        raise SystemExit(f"{args.elf}: external-data descriptor is not the release form")

    for rel in RETIRED:
        if (args.repo / rel).exists():
            raise SystemExit(f"retired compiled asset is still present: {rel}")

    hunk = args.hunk.read_bytes()
    if args.rom:
        package = extract(checked_rom(args.rom))
        probes = (package, package[:128],
                  package[len(package) // 2:len(package) // 2 + 128], package[-128:])
        if any(probe in hunk for probe in probes):
            raise SystemExit(f"{args.hunk}: cartridge-data signature found")
        print(f"ROM-assisted signatures: PASS ({hashlib.sha256(package).hexdigest()})")

    print("release data audit: PASS (package is BSS; cockpit storage is absent or BSS; descriptor is unready)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
