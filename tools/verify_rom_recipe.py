#!/usr/bin/env python3
"""Prove that rof_data_recipe.h recreates the four audited v4.1 boot stages."""

from __future__ import annotations

import hashlib
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from extract_rom_data import checked_rom, extract  # noqa: E402


STAGE_SHA256 = (
    "0715852214212de851ff030910f1e40c0b6948cfcc4d5552e4328d38c2a0a3c1",
    "9ae152d133a93d4bf52a6a690995f7bae81613c785c1aefde7c0554dad34aea4",
    "05f0cdb9700c920f4d96c38d72112885c0eb2974d6a600a4742afeb55708786b",
    "016b5942fc9e6e8d2b29a57d8c26f4ab778363ec96f4d02ac89a92803d27f760",
)
COCKPIT_SHA256 = "1579ecdc5e923d009a36edc1dd57a0db4ef63d815bfe247a76c3a951d56cf653"
LOGO_FIELD_SHA256 = "1629bd60371e8fa0fc748c76feb3e4d22ec67dace38159a818b7809dd1a754b5"
LOGO_GAMES_SHA256 = "4eab386fbf3105621bba7630671217f5ea8347e6c7e0ae1928695435b83305a5"


def decode2(src: int) -> tuple[int, int]:
    p = [0, 0]
    for x in range(4):
        v, mask = (src >> (6 - x * 2)) & 3, 0xC0 >> (x * 2)
        if v & 1: p[0] |= mask
        if v & 2: p[1] |= mask
    return tuple(p)


def cockpit_asset(mem: bytes) -> bytes:
    offsets = (32, 544, 6688, 8736, 9760, 17952)
    out = bytearray(17952)
    out[:20] = b"CPLN" + b"".join(v.to_bytes(2, "big") for v in (1, 32, *offsets))
    for src in range(256): out[32 + src * 2:34 + src * 2] = bytes(decode2(src))
    for ch in range(256):
        for scan in range(8):
            src = mem[0x3800 + (ch & 127) * 8 + scan]
            dual, normal = [0, 0, 0], [0, 0, 0, 0]
            for x in range(4):
                v, mask = (src >> (6 - x * 2)) & 3, 0xC0 >> (x * 2)
                pen = (1, 0, 2, 3 if ch & 128 else 4)[v]
                for plane in range(4):
                    if pen & (1 << plane): normal[plane] |= mask
                if v == 0: dual[0] |= mask
                elif v == 2: dual[1] |= mask
                elif v == 3:
                    dual[2] |= mask
                    if ch & 128: dual[0] |= mask
            out[544 + ch * 24 + scan * 3:547 + ch * 24 + scan * 3] = bytes(dual)
            out[9760 + ch * 32 + scan * 4:9764 + ch * 32 + scan * 4] = bytes(normal)
    for ch in range(128):
        for scan in range(8):
            pos = 6688 + ch * 16 + scan * 2
            out[pos:pos + 2] = bytes(decode2(mem[0x3800 + ch * 8 + scan]))
    for ch in range(64):
        for scan in range(8):
            src, doubled = mem[0x0400 + ch * 8 + scan], 0
            for bit in range(8):
                if src & (0x80 >> bit): doubled |= 0xC000 >> (bit * 2)
            pos = 8736 + ch * 16 + scan * 2
            out[pos:pos + 2] = doubled.to_bytes(2, "big")
    return bytes(out)


def logo_fields(stage1: bytes) -> tuple[bytes, bytes]:
    mem = bytearray(stage1)
    def poke(col, row, shade):
        a = 0x6053 + row * 40 + (col >> 1)
        mem[a] = ((mem[a] & 0xF0) | shade) if col & 1 else ((mem[a] & 0x0F) | shade << 4)
    def pair(col, row, shade): poke(col, row - 1, shade); poke(col, row, shade)
    def brush(col, row, shade, fat):
        if not fat: pair(col, row, shade)
        else:
            pair(col, row - 2, shade); pair(col + 1, row - 2, shade)
            pair(col + 1, row, shade); pair(col, row, shade)
    def cell(pattern, col, row, draw_pass, fat):
        shade = (2 if draw_pass == 2 else (7 if row >> 2 == 8 else row >> 2)) if draw_pass else (((row - 2) & 255) >> 2) ^ 15
        x = col + 8
        for _ in range(8):
            x -= 1
            if pattern & 1: brush(x, row, shade, fat)
            pattern >>= 1
        return row + 2
    def word(count, stream, col, row, draw_pass, fat):
        for _ in range(count):
            pattern, stream, run, strip_row = mem[0x525F + stream], stream + 1, 1, row
            while True:
                for _ in range(run): strip_row = cell(pattern, col, strip_row, draw_pass, fat)
                code = mem[0x525F + stream]
                if not code: stream += 1; break
                run, high = code & 31, code >> 5
                if high: pattern, stream = mem[0x52DF + high], stream + 1
                else: pattern, stream = mem[0x5260 + stream], stream + 2
            col += 8
    mem[0x60A3:0x60A3 + 2480] = bytes(2480)
    word(9, 0, 5, 4, 2, True); word(9, 0, 4, 3, 1, True); word(9, 0, 4, 3, 0, False)
    field = bytes(mem[0x60A3:0x60A3 + 2480])
    word(4, 0x66, 20, 0x36, 2, False); word(4, 0x66, 19, 0x35, 1, False)
    games = b"".join(mem[0x60A3 + r * 40 + 13:0x60A3 + r * 40 + 26] for r in range(50, 61))
    return field, games


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit(f"usage: {sys.argv[0]} rof.rom")
    package = extract(checked_rom(Path(sys.argv[1])))
    header = Path("src/rof_data_recipe.h").read_text()
    ops = [tuple(int(v, 16) for v in row) for row in
           re.findall(r"\{0x([0-9A-F]{4}), 0x([0-9A-F]{4}), 0x([0-9A-F]{4})\}", header)]
    ends_match = re.search(r"kRofDataStageEnds\[4\] = \{ ([^}]+) \}", header)
    if not ends_match:
        raise SystemExit("stage ends not found")
    ends = [int(v.rstrip("u")) for v in ends_match.group(1).split(", ")]
    mem = bytearray(65536)
    stage1 = None
    start = 0
    for stage, end in enumerate(ends):
        for dst, src, size in ops[start:end]:
            mem[dst:dst + size] = package[src:src + size]
        digest = hashlib.sha256(mem).hexdigest()
        if digest != STAGE_SHA256[stage]:
            raise SystemExit(f"stage {stage + 1}: {digest}, expected {STAGE_SHA256[stage]}")
        print(f"stage {stage + 1}: {digest} PASS")
        if stage == 0: stage1 = bytes(mem)
        start = end
    cockpit = cockpit_asset(mem)
    if hashlib.sha256(cockpit).hexdigest() != COCKPIT_SHA256:
        raise SystemExit("runtime cockpit atlas differs from the retired proven asset")
    logo, games = logo_fields(stage1)
    if hashlib.sha256(logo).hexdigest() != LOGO_FIELD_SHA256 or hashlib.sha256(games).hexdigest() != LOGO_GAMES_SHA256:
        raise SystemExit("runtime Logo plot differs from the retired proven fields")
    print("runtime cockpit atlas and both Logo phases: PASS")
    print(f"PASS: {len(ops)} copyright-free copy operations reproduce all four stages")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
