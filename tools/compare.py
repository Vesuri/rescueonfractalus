#!/usr/bin/env python3
"""Compare a rof screenshot with the matching atari000/atari001/... reference.

Usage:
  python3 tools/compare.py rof000.png atari000.png
  python3 tools/compare.py rof000.png          # auto-matches atari000.png

Outputs a side-by-side diff image: compare_NNN.png
Pixel differences are highlighted in magenta; identical pixels are halved.
"""

import sys, struct, zlib, os
from pathlib import Path

def load_png_rgb(path):
    """Decode any PNG to a flat list of (r,g,b) tuples, width, height."""
    with open(path, 'rb') as f:
        data = f.read()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError(f"Not a PNG: {path}")
    i = 8
    chunks = {}
    while i < len(data):
        length = struct.unpack('>I', data[i:i+4])[0]
        ctype  = data[i+4:i+8].decode('ascii')
        cdata  = data[i+8:i+8+length]
        chunks.setdefault(ctype, []).append(cdata)
        i += 12 + length
    ihdr = chunks['IHDR'][0]
    w, h, bd, ct = struct.unpack('>IIBB', ihdr[:10])
    assert bd == 8, "only 8-bit PNGs supported"

    raw = zlib.decompress(b''.join(chunks['IDAT']))

    palette = None
    if ct == 3:  # indexed
        plte = chunks['PLTE'][0]
        palette = [(plte[i*3], plte[i*3+1], plte[i*3+2]) for i in range(len(plte)//3)]
        channels = 1
    elif ct == 2:  # RGB
        channels = 3
    elif ct == 6:  # RGBA
        channels = 4
    elif ct == 0:  # grey
        channels = 1
    else:
        raise ValueError(f"Unsupported color type {ct}")

    row_stride = 1 + w * channels
    pixels = []
    prev = bytes(w * channels)
    for row in range(h):
        filt = raw[row * row_stride]
        line = bytearray(raw[row * row_stride + 1 : row * row_stride + 1 + w * channels])
        # Apply filter
        if filt == 1:   # Sub
            for x in range(channels, len(line)):
                line[x] = (line[x] + line[x - channels]) & 0xFF
        elif filt == 2: # Up
            for x in range(len(line)):
                line[x] = (line[x] + prev[x]) & 0xFF
        elif filt == 3: # Average
            for x in range(len(line)):
                a = line[x - channels] if x >= channels else 0
                b = prev[x]
                line[x] = (line[x] + (a + b) // 2) & 0xFF
        elif filt == 4: # Paeth
            def paeth(a, b, c):
                p = a + b - c
                pa, pb, pc = abs(p-a), abs(p-b), abs(p-c)
                return a if pa <= pb and pa <= pc else (b if pb <= pc else c)
            for x in range(len(line)):
                a = line[x - channels] if x >= channels else 0
                b = prev[x]
                c = prev[x - channels] if x >= channels else 0
                line[x] = (line[x] + paeth(a, b, c)) & 0xFF
        prev = bytes(line)

        for x in range(w):
            if palette:
                idx = line[x]
                pixels.append(palette[idx])
            elif channels == 3:
                r, g, b = line[x*3], line[x*3+1], line[x*3+2]
                pixels.append((r, g, b))
            elif channels == 4:
                r, g, b = line[x*4], line[x*4+1], line[x*4+2]
                pixels.append((r, g, b))
            else:
                v = line[x]
                pixels.append((v, v, v))
    return pixels, w, h


def write_png_rgb(path, pixels, w, h):
    """Write a flat list of (r,g,b) tuples as an RGB PNG."""
    def make_chunk(ctype, data):
        c = ctype.encode() + data
        crc = zlib.crc32(c) & 0xFFFFFFFF
        return struct.pack('>I', len(data)) + c + struct.pack('>I', crc)

    ihdr_data = struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)
    # Build raw image (filter 0 = None for each row)
    raw = bytearray()
    for row in range(h):
        raw.append(0)  # filter type None
        for x in range(w):
            r, g, b = pixels[row * w + x]
            raw += bytes([r, g, b])
    compressed = zlib.compress(bytes(raw), 6)

    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n')
        f.write(make_chunk('IHDR', ihdr_data))
        f.write(make_chunk('IDAT', compressed))
        f.write(make_chunk('IEND', b''))


def compare(ours_path, ref_path):
    print(f"Loading {ours_path} ...")
    ours, ow, oh = load_png_rgb(ours_path)
    print(f"  {ow}×{oh}")
    print(f"Loading {ref_path} ...")
    ref, rw, rh = load_png_rgb(ref_path)
    print(f"  {rw}×{rh}")

    # Use the smaller of the two as the common canvas
    w = min(ow, rw)
    h = min(oh, rh)

    diff_pixels = 0
    max_dist = 0
    out_left  = []   # ours (dimmed)
    out_right = []   # reference (dimmed)
    out_diff  = []   # difference highlight

    for row in range(h):
        for x in range(w):
            pr = ref [row * rw + x]
            po = ours[row * ow + x]
            dr = abs(pr[0] - po[0])
            dg = abs(pr[1] - po[1])
            db = abs(pr[2] - po[2])
            dist = dr + dg + db
            if dist > 12:   # small tolerance for palette rounding
                diff_pixels += 1
                max_dist = max(max_dist, dist)
                out_left .append(( min(po[0] + 80, 255), po[1]//2, po[2]//2 ))
                out_right.append(( min(pr[0] + 80, 255), pr[1]//2, pr[2]//2 ))
                out_diff .append((255, 0, 220))  # magenta = differs
            else:
                half_o = (po[0]//2, po[1]//2, po[2]//2)
                half_r = (pr[0]//2, pr[1]//2, pr[2]//2)
                out_left .append(half_o)
                out_right.append(half_r)
                out_diff .append((0, 0, 0))      # black = same

    total = w * h
    pct   = 100.0 * diff_pixels / total
    print(f"\nDiff pixels: {diff_pixels}/{total} ({pct:.1f}%)  max_channel_dist={max_dist}")

    # Side-by-side: ours | 4px gap | reference | 4px gap | diff
    gap = 4
    out_w = w * 3 + gap * 2
    out_h = h
    combined = []
    for row in range(h):
        for x in range(w):
            combined.append(out_left[row * w + x])
        for _ in range(gap):
            combined.append((180, 180, 180))
        for x in range(w):
            combined.append(out_right[row * w + x])
        for _ in range(gap):
            combined.append((180, 180, 180))
        for x in range(w):
            combined.append(out_diff[row * w + x])

    stem = Path(ours_path).stem
    out_path = f"compare_{stem}.png"
    write_png_rgb(out_path, combined, out_w, out_h)
    print(f"Written: {out_path}  ({out_w}×{out_h})")
    print("Layout: [ours (dimmed)] | [reference (dimmed)] | [diff: magenta=differs, black=same]")
    return diff_pixels


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    ours = sys.argv[1]
    if len(sys.argv) >= 3:
        ref = sys.argv[2]
    else:
        # Auto-match: rof000.png → atari000.png
        stem = Path(ours).stem          # e.g. "rof000"
        num  = stem.lstrip('rof').lstrip('0') or '0'
        ref  = f"atari{int(num):03d}.png"
        print(f"Auto-matching reference: {ref}")

    compare(ours, ref)
