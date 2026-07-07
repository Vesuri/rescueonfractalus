#!/usr/bin/env python3
"""Map the Atari PMG (player/missile) allocation from a 64KB RAM dump.

Given a flight-scene RAM image (from extract_a8s_ram.py), report which scanline
ranges each player P0-P3 and each missile M0-M3 buffer occupies, plus the
relevant HPOS/COLPM/SIZE *shadows* the game keeps in RAM (the GTIA write regs
themselves are not stored in a RAM dump).

PMBASE = $0800 (single-line res): missiles $0B00, P0 $0C00, P1 $0D00,
P2 $0E00, P3 $0F00.  Player scanline S <-> buffer offset $32+S region..

Usage:  pmg_map.py <ram.bin> [label]
"""
import sys

MISS = 0x0B00
P = [0x0C00, 0x0D00, 0x0E00, 0x0F00]


def nz_runs(mem, base):
    """Return list of (start_off, end_off, sample_bytes) non-zero runs in a 256-byte page."""
    runs = []
    off = 0
    while off < 256:
        if mem[base + off]:
            s = off
            while off < 256 and mem[base + off]:
                off += 1
            runs.append((s, off - 1, bytes(mem[base + s:base + off])))
        else:
            off += 1
    return runs


def miss_runs(mem):
    """Missile page: each byte packs M0(b0-1) M1(b2-3) M2(b4-5) M3(b6-7). Report per-missile runs."""
    per = {0: [], 1: [], 2: [], 3: []}
    for m in range(4):
        shift = m * 2
        off = 0
        while off < 256:
            v = (mem[MISS + off] >> shift) & 3
            if v:
                s = off
                while off < 256 and ((mem[MISS + off] >> shift) & 3):
                    off += 1
                per[m].append((s, off - 1))
            else:
                off += 1
    return per


def main():
    ram = open(sys.argv[1], "rb").read()
    label = sys.argv[2] if len(sys.argv) > 2 else sys.argv[1]
    m = ram
    print(f"===== PMG map: {label} =====")
    print(f"$0036 shot-active={m[0x36]:02x}  $0037 shotCol/COLPM2={m[0x37]:02x}  "
          f"$0038 shotX={m[0x38]:02x}  $00CB HPOSP2sh={m[0xcb]:02x}  $00CD SIZEP2={m[0xcd]:02x}  "
          f"$286e={m[0x286e]:02x}")
    print(f"$2840 missileX-base={m[0x2840]:02x}  $2870 HPOSP3sh={m[0x2870]:02x}  "
          f"$281A/$281B altim={m[0x281a]:02x}/{m[0x281b]:02x}")
    print(f"colour shadows: $00D5={m[0xd5]:02x} $00D6={m[0xd6]:02x} $00D9={m[0xd9]:02x} "
          f"$00DA-DD={m[0xda]:02x}/{m[0xdb]:02x}/{m[0xdc]:02x}/{m[0xdd]:02x}")
    for i, base in enumerate(P):
        runs = nz_runs(m, base)
        print(f"\n-- P{i} (${base:04x}) --")
        for s, e, data in runs:
            print(f"   off ${s:02x}-${e:02x} ({e-s+1:3d} rows)  first bytes: "
                  + " ".join(f"{b:02x}" for b in data[:12]))
        if not runs:
            print("   (empty)")
    print("\n-- Missiles ($0B00, 2bpp packed) --")
    for mi, runs in miss_runs(m).items():
        if runs:
            rs = ", ".join(f"${s:02x}-${e:02x}" for s, e in runs)
            print(f"   M{mi}: {rs}")
        else:
            print(f"   M{mi}: (empty)")


if __name__ == "__main__":
    main()
