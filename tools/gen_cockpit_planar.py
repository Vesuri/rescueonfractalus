#!/usr/bin/env python3
"""Generate and prove the faithful native-planar cockpit lookup asset.

The input is the committed sparse boot image, replayed to the final 64K Atari RAM
state.  The output contains no character codes and requires no pixel conversion:
every entry is already an Amiga bitplane byte sequence.  Runtime code only selects
an entry and copies bytes to its destination surface.

Usage:
  python3 tools/gen_cockpit_planar.py amiga/assets/cockpit_planar.bin
  python3 tools/gen_cockpit_planar.py --check amiga/assets/cockpit_planar.bin

When the local, intentionally-untracked a800dumps/*.a8s captures are available,
the script additionally renders representative Standby, Doors, Tunnel, Planet,
Flight, pause, and low-energy states through both the old decoder and the generated
tables and requires byte-for-byte equality.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from extract_a8s_ram import extract  # noqa: E402


MAGIC = b"CPLN"
VERSION = 1
HEADER_SIZE = 32
MODED_OFFSET = HEADER_SIZE
MODED_SIZE = 256 * 2
MODE4_OFFSET = MODED_OFFSET + MODED_SIZE
MODE4_SIZE = 256 * 8 * 3
COMPASS_OFFSET = MODE4_OFFSET + MODE4_SIZE
COMPASS_SIZE = 128 * 8 * 2
TEXT_MASK_OFFSET = COMPASS_OFFSET + COMPASS_SIZE
TEXT_MASK_SIZE = 64 * 8 * 2
NORMAL_MODE4_OFFSET = TEXT_MASK_OFFSET + TEXT_MASK_SIZE
NORMAL_MODE4_SIZE = 256 * 8 * 4
ASSET_SIZE = NORMAL_MODE4_OFFSET + NORMAL_MODE4_SIZE

COCKPIT_CHARSET_SHA256 = "6f8e858500724a07abc6232f95f4b826037482af6b1bbf26a64a6018368eec9d"
TEXT_CHARSET_SHA256 = "fdb3eb5435c594b506e7a56560f249212640b403cdbb64ca0660eafd112b426d"

CAPTURES = (
    "standby.a8s",
    "launch_2_doors.a8s",
    "launch_3_tunnel.a8s",
    "launch_5_planet.a8s",
    "launch_6_gameplay.a8s",
    "flight.a8s",
    "pause.a8s",
    "lowenergy.a8s",
)


def replay_sparse(path: str) -> bytearray:
    blob = open(path, "rb").read()
    mem = bytearray(65536)
    pos = 0
    while pos < len(blob):
        if pos + 4 > len(blob):
            raise SystemExit(f"{path}: truncated sparse chunk header at {pos}")
        addr, size = struct.unpack_from(">HH", blob, pos)
        pos += 4
        if pos + size > len(blob):
            raise SystemExit(f"{path}: truncated sparse chunk at {pos - 4}")
        end = addr + size
        if end > len(mem):
            raise SystemExit(f"{path}: sparse chunk crosses 64K at ${addr:04X}")
        mem[addr:end] = blob[pos:pos + size]
        pos += size
    return mem


def decode_2bpp(src: int) -> tuple[int, int]:
    p1 = p2 = 0
    for pixel_index in range(4):
        pixel = (src >> (6 - pixel_index * 2)) & 3
        mask = 0xC0 >> (pixel_index * 2)
        if pixel & 1:
            p1 |= mask
        if pixel & 2:
            p2 |= mask
    return p1, p2


def decode_mode4(src: int, alternate: int) -> tuple[int, int, int]:
    p1 = p2 = p3 = 0
    for pixel_index in range(4):
        pixel = (src >> (6 - pixel_index * 2)) & 3
        mask = 0xC0 >> (pixel_index * 2)
        if pixel == 0:
            p1 |= mask
        elif pixel == 2:
            p2 |= mask
        elif pixel == 3:
            p3 |= mask
            if alternate:
                p1 |= mask
    return p1, p2, p3


def decode_mode4_normal(src: int, alternate: int) -> tuple[int, int, int, int]:
    """Normal-playfield pens preserving the old dual-playfield visible meanings."""
    planes = [0, 0, 0, 0]
    for pixel_index in range(4):
        pixel = (src >> (6 - pixel_index * 2)) & 3
        # 0=dark grey, 1=region bg, 2=light-grey stencil, 3=bright salmon, 4=dark red.
        pen = (1, 0, 2, 3 if alternate else 4)[pixel]
        mask = 0xC0 >> (pixel_index * 2)
        for plane in range(4):
            if pen & (1 << plane):
                planes[plane] |= mask
    return tuple(planes)


def double_bits(src: int) -> tuple[int, int]:
    value = 0
    for bit in range(8):
        if src & (0x80 >> bit):
            value |= 0xC000 >> (bit * 2)
    return value >> 8, value & 0xFF


def build_asset(mem: bytes) -> bytes:
    cockpit_charset = mem[0x3800:0x3C00]
    text_charset = mem[0x0400:0x0600]
    if hashlib.sha256(cockpit_charset).hexdigest() != COCKPIT_CHARSET_SHA256:
        raise SystemExit("cockpit charset differs from the audited $3800-$3BFF source")
    if hashlib.sha256(text_charset).hexdigest() != TEXT_CHARSET_SHA256:
        raise SystemExit("text charset differs from the audited $0400-$05FF source")

    out = bytearray(HEADER_SIZE)
    struct.pack_into(">4s8H", out, 0, MAGIC, VERSION, HEADER_SIZE, MODED_OFFSET,
                     MODE4_OFFSET, COMPASS_OFFSET, TEXT_MASK_OFFSET, NORMAL_MODE4_OFFSET,
                     ASSET_SIZE)

    for src in range(256):
        out.extend(decode_2bpp(src))

    for char_byte in range(256):
        glyph = cockpit_charset[(char_byte & 0x7F) * 8:(char_byte & 0x7F) * 8 + 8]
        for src in glyph:
            out.extend(decode_mode4(src, char_byte >> 7))

    for char_code in range(128):
        glyph = cockpit_charset[char_code * 8:char_code * 8 + 8]
        for src in glyph:
            out.extend(decode_2bpp(src))

    for char_code in range(64):
        glyph = text_charset[char_code * 8:char_code * 8 + 8]
        for src in glyph:
            out.extend(double_bits(src))

    for char_byte in range(256):
        glyph = cockpit_charset[(char_byte & 0x7F) * 8:(char_byte & 0x7F) * 8 + 8]
        for src in glyph:
            out.extend(decode_mode4_normal(src, char_byte >> 7))

    if len(out) != ASSET_SIZE:
        raise AssertionError(f"asset size {len(out)} != {ASSET_SIZE}")
    return bytes(out)


def old_cockpit(mem: bytes) -> bytes:
    out = bytearray(88 * 120)
    for entry in range(4):
        for col in range(40):
            p1, p2 = decode_2bpp(mem[0x350D + entry * 48 + 4 + col])
            for row in (entry * 2, entry * 2 + 1):
                base = row * 120 + col
                out[base], out[base + 40], out[base + 80] = p1, p2, 0
    charset = mem[0x3800:0x3C00]
    for entry in range(10):
        for col in range(40):
            char_byte = mem[0x332D + entry * 48 + 4 + col]
            glyph = charset[(char_byte & 0x7F) * 8:(char_byte & 0x7F) * 8 + 8]
            for scan, src in enumerate(glyph):
                base = (8 + entry * 8 + scan) * 120 + col
                p1, p2, p3 = decode_mode4(src, char_byte >> 7)
                out[base], out[base + 40], out[base + 80] = p1, p2, p3
    return bytes(out)


def new_cockpit(mem: bytes, asset: bytes) -> bytes:
    out = bytearray(88 * 120)
    for entry in range(4):
        for col in range(40):
            src = mem[0x350D + entry * 48 + 4 + col]
            p1, p2 = asset[MODED_OFFSET + src * 2:MODED_OFFSET + src * 2 + 2]
            for row in (entry * 2, entry * 2 + 1):
                base = row * 120 + col
                out[base], out[base + 40], out[base + 80] = p1, p2, 0
    for entry in range(10):
        for col in range(40):
            char_byte = mem[0x332D + entry * 48 + 4 + col]
            tile = asset[MODE4_OFFSET + char_byte * 24:MODE4_OFFSET + char_byte * 24 + 24]
            for scan in range(8):
                base = (8 + entry * 8 + scan) * 120 + col
                out[base], out[base + 40], out[base + 80] = tile[scan * 3:scan * 3 + 3]
    return bytes(out)


def old_top(mem: bytes) -> bytes:
    out = bytearray(42 * 80)
    for col, char_byte in enumerate(mem[0x32B7:0x32CB]):
        glyph = mem[0x0400 + (char_byte & 0x3F) * 8:0x0408 + (char_byte & 0x3F) * 8]
        use_p2 = (char_byte >> 6) == 1
        for scan, src in enumerate(glyph):
            hi, lo = double_bits(src)
            base = (21 + scan) * 80 + col * 2
            plane = 40 if use_p2 else 0
            out[base + plane:base + plane + 2] = bytes((hi, lo))
    charset = mem[0x3800:0x3C00]
    for col, char_byte in enumerate(mem[0x32E3:0x32E7]):
        glyph = charset[(char_byte & 0x7F) * 8:(char_byte & 0x7F) * 8 + 8]
        for scan, src in enumerate(glyph):
            base = (33 + scan) * 80 + 18 + col
            out[base], out[base + 40] = decode_2bpp(src)
    return bytes(out)


def new_top(mem: bytes, asset: bytes) -> bytes:
    out = bytearray(42 * 80)
    for col, char_byte in enumerate(mem[0x32B7:0x32CB]):
        mask = asset[TEXT_MASK_OFFSET + (char_byte & 0x3F) * 16:
                     TEXT_MASK_OFFSET + (char_byte & 0x3F) * 16 + 16]
        plane = 40 if (char_byte >> 6) == 1 else 0
        for scan in range(8):
            base = (21 + scan) * 80 + col * 2 + plane
            out[base:base + 2] = mask[scan * 2:scan * 2 + 2]
    for col, char_byte in enumerate(mem[0x32E3:0x32E7]):
        tile = asset[COMPASS_OFFSET + (char_byte & 0x7F) * 16:
                     COMPASS_OFFSET + (char_byte & 0x7F) * 16 + 16]
        for scan in range(8):
            base = (33 + scan) * 80 + 18 + col
            out[base], out[base + 40] = tile[scan * 2:scan * 2 + 2]
    return bytes(out)


def prove_tables(mem: bytes, asset: bytes) -> None:
    for src in range(256):
        got = tuple(asset[MODED_OFFSET + src * 2:MODED_OFFSET + src * 2 + 2])
        assert got == decode_2bpp(src), ("mode D", src, got)
    for char_byte in range(256):
        tile = asset[MODE4_OFFSET + char_byte * 24:MODE4_OFFSET + char_byte * 24 + 24]
        glyph = mem[0x3800 + (char_byte & 0x7F) * 8:0x3808 + (char_byte & 0x7F) * 8]
        expected = bytes(v for src in glyph for v in decode_mode4(src, char_byte >> 7))
        assert tile == expected, ("mode 4", char_byte)
    for char_code in range(128):
        tile = asset[COMPASS_OFFSET + char_code * 16:COMPASS_OFFSET + char_code * 16 + 16]
        glyph = mem[0x3800 + char_code * 8:0x3808 + char_code * 8]
        assert tile == bytes(v for src in glyph for v in decode_2bpp(src)), ("compass", char_code)
    for char_code in range(64):
        mask = asset[TEXT_MASK_OFFSET + char_code * 16:TEXT_MASK_OFFSET + char_code * 16 + 16]
        glyph = mem[0x0400 + char_code * 8:0x0408 + char_code * 8]
        assert mask == bytes(v for src in glyph for v in double_bits(src)), ("text", char_code)
    for char_byte in range(256):
        tile = asset[NORMAL_MODE4_OFFSET + char_byte * 32:
                     NORMAL_MODE4_OFFSET + char_byte * 32 + 32]
        glyph = mem[0x3800 + (char_byte & 0x7F) * 8:0x3808 + (char_byte & 0x7F) * 8]
        expected = bytes(v for src in glyph for v in decode_mode4_normal(src, char_byte >> 7))
        assert tile == expected, ("normal mode 4", char_byte)
        # Compare displayed meanings, not representation: dual-PF source combinations map to
        # the same normal-playfield pen contract for every two-bit source pixel.
        for src in glyph:
            old = decode_mode4(src, char_byte >> 7)
            new = decode_mode4_normal(src, char_byte >> 7)
            for mask in (0xC0, 0x30, 0x0C, 0x03):
                old_bits = tuple(bool(p & mask) for p in old)
                old_pen = {(False, False, False): 0,
                           (True, False, False): 1,
                           (False, True, False): 2,
                           (False, False, True): 4,
                           (True, False, True): 3}[old_bits]
                new_pen = sum((1 << p) for p in range(4) if new[p] & mask)
                assert new_pen == old_pen, ("visible pen", char_byte, src, mask)


def prove_captures(asset: bytes) -> int:
    count = 0
    for name in CAPTURES:
        path = os.path.join("a800dumps", name)
        if not os.path.exists(path):
            continue
        _, mem = extract(path)
        old_ck, new_ck = old_cockpit(mem), new_cockpit(mem, asset)
        old_top_bytes, new_top_bytes = old_top(mem), new_top(mem, asset)
        if old_ck != new_ck or old_top_bytes != new_top_bytes:
            raise SystemExit(f"{path}: native-planar rendering differs from legacy decoder")
        digest = hashlib.sha256(old_top_bytes + old_ck).hexdigest()[:16]
        print(f"  {name:24s} {digest}")
        count += 1
    return count


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="verify output instead of replacing it")
    parser.add_argument("output", nargs="?", default="amiga/assets/cockpit_planar.bin")
    args = parser.parse_args()

    mem = replay_sparse("amiga/assets/rof_boot_image.bin")
    asset = build_asset(mem)
    prove_tables(mem, asset)

    if args.check:
        current = open(args.output, "rb").read()
        if current != asset:
            raise SystemExit(f"{args.output}: stale; regenerate with this script")
    else:
        with open(args.output, "wb") as output:
            output.write(asset)

    capture_count = prove_captures(asset)
    digest = hashlib.sha256(asset).hexdigest()
    verb = "verified" if args.check else "wrote"
    print(f"{verb} {args.output}: {len(asset)} bytes, sha256 {digest}")
    print("exhaustive tables: 256 mode-D bytes, 256 dual-PF cells, 256 normal-4bp cells, "
          "128 compass cells, 64 text glyphs")
    print(f"representative captured surfaces: {capture_count} byte-exact")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
