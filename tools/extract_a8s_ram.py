#!/usr/bin/env python3
"""Extract the flat 64 KB RAM image from an atari800 savestate (.a8s).

The .a8s is a gzip-compressed "ATARI800" statesave whose RAM module sits at a
header-dependent offset.  A naive byte-offset extraction is fragile and once
produced a dump shifted +1 (RAM base 0x85 instead of 0x86), which silently
corrupted every absolute address the ported 6502 code reads.

To make that class of bug impossible, we do NOT trust a hard-coded offset:
we locate the RAM base by anchoring on an invariant ROM code signature whose
address is fixed by the disassembly, then verify a second anchor before
writing.  Offset = (signature position in savestate) - (its known address).

Usage:  extract_a8s_ram.py <in.a8s> <out.bin>
"""
import gzip
import sys

# Invariant ROM code, address fixed by disasm/listing.txt:
#   $7148  AE 3C 07  LDX $073C / E8 INX / BD DB 71 LDA $71DB,X   (sfx_seq_step)
#   $70F9  CE 3A 07  DEC $073A / 10 03 BPL / 20 48 71 JSR $7148  (sfx_voice_tick)
ANCHORS = [
    (0x7148, bytes([0xAE, 0x3C, 0x07, 0xE8, 0xBD, 0xDB, 0x71])),
    (0x70F9, bytes([0xCE, 0x3A, 0x07, 0x10, 0x03, 0x20, 0x48, 0x71])),
]


def extract(path):
    raw = open(path, "rb").read()
    if raw[:2] == b"\x1f\x8b":
        raw = gzip.decompress(raw)
    if raw[:8] != b"ATARI800":
        raise SystemExit(f"{path}: not an ATARI800 statesave")

    addr0, sig0 = ANCHORS[0]
    pos = raw.find(sig0)
    if pos < 0:
        raise SystemExit(f"{path}: ROM anchor not found (unexpected scene?)")
    base = pos - addr0
    if raw.find(sig0, pos + 1) != -1:
        raise SystemExit(f"{path}: ROM anchor is ambiguous")

    for addr, sig in ANCHORS[1:]:
        if raw[base + addr : base + addr + len(sig)] != sig:
            raise SystemExit(f"{path}: second anchor mismatch at ${addr:04X} "
                             f"(base 0x{base:X} is wrong)")

    img = raw[base : base + 0x10000]
    if len(img) != 0x10000:
        raise SystemExit(f"{path}: RAM image truncated ({len(img)} bytes)")
    return base, img


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    base, img = extract(sys.argv[1])
    open(sys.argv[2], "wb").write(img)
    print(f"{sys.argv[1]}: RAM base 0x{base:X} -> {sys.argv[2]} ({len(img)} bytes)")


if __name__ == "__main__":
    main()
