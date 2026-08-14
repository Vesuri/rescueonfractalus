#!/usr/bin/env python3
"""Fail the build if any object puts a NONZERO byte into `.MEMF_CHIP`.

`.MEMF_CHIP` is linked as a chip BSS hunk (amiga/memf_chip_bss.ld), which means the
linker DISCARDS its contents.  A static initialiser on a `__chip` variable therefore
compiles, links and runs -- with the variable silently zero.  Nothing at runtime can
distinguish that from "the buffer has not been filled yet", and the symptom (a waveform
buffer of zeros = silence) is exactly what an audio bug looks like, so it is worth a
build-time stop rather than a comment.

Usage:  check_chip_bss.py <object files...>
Reads the ELF section headers directly -- the toolchain ships no objcopy.
"""
import struct
import sys

SEC = ".MEMF_CHIP"


def sections(path):
    """Yield (name, sh_type, data) for each section of a big-endian ELF32 object."""
    with open(path, "rb") as f:
        elf = f.read()
    if elf[:4] != b"\x7fELF" or elf[4] != 1 or elf[5] != 2:
        return  # not a 32-bit big-endian ELF (vasm/gas both emit one, but be safe)
    e_shoff, = struct.unpack_from(">I", elf, 0x20)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from(">HHH", elf, 0x2E)
    def hdr(i):
        return struct.unpack_from(">10I", elf, e_shoff + i * e_shentsize)
    stroff = hdr(e_shstrndx)[4]
    for i in range(e_shnum):
        name_off, sh_type, _flags, _addr, offset, size = hdr(i)[:6]
        end = elf.index(b"\0", stroff + name_off)
        name = elf[stroff + name_off:end].decode("ascii", "replace")
        data = b"" if sh_type == 8 else elf[offset:offset + size]   # 8 = SHT_NOBITS
        yield name, sh_type, data


def main(argv):
    bad = []
    for obj in argv:
        try:
            for name, sh_type, data in sections(obj):
                if name != SEC:
                    continue
                nz = [i for i, b in enumerate(data) if b]
                if nz:
                    bad.append((obj, len(data), nz[:8], len(nz)))
        except (OSError, struct.error, ValueError) as e:
            print(f"check_chip_bss: cannot read {obj}: {e}", file=sys.stderr)
            return 1
    if not bad:
        return 0
    print(f"error: a static INITIALISER reached the {SEC} section.", file=sys.stderr)
    print(f"       {SEC} is linked as a chip BSS hunk (amiga/memf_chip_bss.ld), so its",
          file=sys.stderr)
    print("       contents are DISCARDED — the variable would come up zero at runtime.",
          file=sys.stderr)
    print("       Fix: drop the initialiser and fill the buffer at runtime instead",
          file=sys.stderr)
    print("       (PlatformAmiga::audioInit does this for wave_pure).", file=sys.stderr)
    for obj, size, first, count in bad:
        print(f"       {obj}: {count} nonzero of {size} bytes, first at offsets {first}",
              file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
