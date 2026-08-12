#!/usr/bin/env python3
"""Render the Amiga station/logo GTIA-9 field bitmap dumped by amiga/station_dump.gdb.

Reconstructs exactly what the copper is showing: 4 interleaved bitplanes, 320 px wide, one
pen per pixel through the same 16-luminance palette the copper list holds (tools/
gen_gtia9_pal.py).  Beats eyeballing an emulator window — it makes the DECODE checkable
against the SDL/atari800 render pixel for pixel.

  python3 tools/station_bmp.py amiga/.run/station_field.bin out.png [--row N] [--rows N]

--row defaults to the winRow in amiga/.run/station_meta.txt (so the output is the visible
frame), --rows to 192 (the station's window; the logo shows 62).
"""
import sys
import os
from PIL import Image

PAL = {
    0: [0x000, 0x111, 0x222, 0x222, 0x333, 0x444, 0x555, 0x666,
        0x666, 0x777, 0x888, 0xAAA, 0xBBB, 0xCCC, 0xDDD, 0xEEE],
    1: [0x200, 0x310, 0x410, 0x420, 0x530, 0x640, 0x740, 0x851,
        0x962, 0xA73, 0xB84, 0xC95, 0xDA6, 0xEC7, 0xFD9, 0xFEA],
}

W, PLANES = 320, 4
LINE = W // 8            # 40 bytes per plane per row
ROW = LINE * PLANES      # 160 bytes per interleaved row


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    opts = dict()
    for a in sys.argv[1:]:
        if a.startswith('--'):
            k, _, v = a[2:].partition('=')
            opts[k] = v
    if len(args) < 2:
        print(__doc__)
        return 1
    src, dst = args[0], args[1]
    data = open(src, 'rb').read()

    meta = {}
    mp = os.path.join(os.path.dirname(src), 'station_meta.txt')
    if os.path.exists(mp):
        for line in open(mp):
            p = line.split()
            if len(p) == 2 and p[1].isdigit():
                meta[p[0]] = int(p[1])

    row0 = int(opts.get('row', meta.get('winRow', 0)))
    nrows = int(opts.get('rows', 192))
    hue = int(opts.get('hue', 0))
    pal = [((c >> 8) * 17, ((c >> 4) & 15) * 17, (c & 15) * 17) for c in PAL[hue]]

    im = Image.new('RGB', (W, nrows))
    px = im.load()
    for y in range(nrows):
        base = (row0 + y) * ROW
        if base + ROW > len(data):
            break
        for x in range(W):
            byte, bit = x >> 3, 7 - (x & 7)
            pen = 0
            for p in range(PLANES):
                if data[base + p * LINE + byte] >> bit & 1:
                    pen |= 1 << p
            px[x, y] = pal[pen]
    im.save(dst)
    print('%s: row0=%d rows=%d hue=%d -> %s' % (src, row0, nrows, hue, dst))
    return 0


if __name__ == '__main__':
    sys.exit(main())
