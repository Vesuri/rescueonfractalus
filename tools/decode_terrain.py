#!/usr/bin/env python3
"""Decode a dumped flight terrainBitmap (Stage-1 verifier).

amiga/dump_terrain.sh writes .run/terrain.bin = 47 interleaved scanlines of the
3-bitplane terrainBitmap (320px => 40 bytes/plane; per scanline: plane1[40] plane2[40]
plane3[40] = 120 bytes).  The flight viewport is ANTIC mode-D 2bpp pixel-doubled
horizontally, so Amiga pen = plane1bit | plane2bit<<1 (| plane3bit<<2); for terrain
plane3 is 0 so pen 0-3 = field value 0-3 (body/sky/dots/—).

Usage: decode_terrain.py [terrain.bin]   (default amiga/.run/terrain.bin)
Prints a per-row value histogram + a downsampled ASCII map (every 2nd Amiga px =
the 160 logical mode-D px).  Compare against the a800dumps/flight1.bin reference.
"""
import sys

ROWS, STRIDE, PLANE = 47, 120, 40
CH = {0: ".", 1: "S", 2: "o", 3: "#", 4: "4", 5: "5", 6: "6", 7: "7"}

def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "amiga/.run/terrain.bin"
    data = open(path, "rb").read()
    print(f"{path}: {len(data)} bytes ({len(data)//STRIDE} scanlines)")
    print("per-row value histogram (v0=body v1=sky v2=dots v3=hi/band) + ASCII (160 logical px):")
    for r in range(min(ROWS, len(data) // STRIDE)):
        so = r * STRIDE
        hist = [0] * 8
        row = []
        for px in range(0, 320, 2):                 # every 2nd px = 1 logical mode-D px
            bidx, bit = px >> 3, 7 - (px & 7)
            p1 = (data[so + bidx] >> bit) & 1
            p2 = (data[so + PLANE + bidx] >> bit) & 1
            p3 = (data[so + 2 * PLANE + bidx] >> bit) & 1
            v = p1 | (p2 << 1) | (p3 << 2)
            hist[v] += 1
            row.append(CH.get(v, "?"))
        print(f"r{r:2d} v0={hist[0]:3d} v1={hist[1]:3d} v2={hist[2]:3d} v3={hist[3]:3d} |{''.join(row)}")

if __name__ == "__main__":
    main()
