#!/usr/bin/env python3
"""Parse an atari800 -pokeyrec-ascii capture of the Rescue on Fractalus launch
sequence and segment it into audio phases.

pokeyrec ascii format: one line per sample, 18 hex chars = 9 POKEY registers:
  [0:2]=AUDF1 [2:4]=AUDC1 [4:6]=AUDF2 [6:8]=AUDC2 [8:10]=AUDF3 [10:12]=AUDC3
  [12:14]=AUDF4 [14:16]=AUDC4 [16:18]=AUDCTL

Usage: parse_pokeyrec.py <pokeyrec.dat> [--interval SCANLINES]
Prints run-length-encoded per-channel activity and flags the doors window
(AUDC2==0x44) plus the surrounding voice activity.
"""
import sys

REG = ["AUDF1", "AUDC1", "AUDF2", "AUDC2", "AUDF3", "AUDC3", "AUDF4", "AUDC4", "AUDCTL"]


def load(path):
    rows = []
    with open(path) as f:
        for ln in f:
            ln = ln.strip()
            if len(ln) < 18:
                continue
            rows.append([int(ln[i:i+2], 16) for i in range(0, 18, 2)])
    return rows


def rle(vals):
    out = []
    for v in vals:
        if out and out[-1][0] == v:
            out[-1][1] += 1
        else:
            out.append([v, 1])
    return out


def main():
    path = sys.argv[1]
    rows = load(path)
    n = len(rows)
    print(f"{n} samples")
    if not n:
        return
    # Locate the doors window: AUDC2 == 0x44 (poly4 door buzz)
    door = [i for i, r in enumerate(rows) if r[3] == 0x44]
    if door:
        a, b = door[0], door[-1]
        print(f"\n== DOORS window (AUDC2==$44): samples {a}..{b} ({b-a+1} samples) ==")
        print("AUDF2 contour (value x count):")
        for v, c in rle([rows[i][2] for i in range(a, b+1)]):
            print(f"  ${v:02X} x{c}")
        print("AUDC2 contour:")
        for v, c in rle([rows[i][3] for i in range(a, b+1)]):
            print(f"  ${v:02X} x{c}")
    else:
        print("\n(no AUDC2==$44 door window found)")
    # Show全 per-channel AUDC RLE to eyeball phases (sampled every ~Kth row)
    print("\n== whole-capture AUDC (voice on/off) RLE per channel ==")
    for ch, ci in [(1, 1), (2, 3), (3, 5), (4, 7)]:
        runs = rle([r[ci] for r in rows])
        runs = [(v, c) for v, c in runs if c >= 3]  # drop blips
        s = " ".join(f"${v:02X}x{c}" for v, c in runs[:40])
        print(f"  ch{ch} AUDC: {s}")


if __name__ == "__main__":
    main()
