#!/usr/bin/env python3
"""Extract the launch-cinematic TUNNEL bitmap to a raw GTIA-mode-10 asset.

The tunnel is a concentric rectangular pattern (nested rectangles, pens cycling
1..6 from the outer frame inward to a centre vanishing point) drawn in the
mode-F viewport at $1000 (stride $2E=46) of the doors/tunnel display list.  We
take the central 40 bytes of each of the 86 viewport rows (same +4 wide-playfield
crop as the terrain) and write them as raw GTIA nibbles.  StandbyScene decodes
these to an interleaved 3-bitplane Amiga bitmap at init (nibble value = pen),
and cycles the 6-colour ring ($08D4-$08D9) into COLOR01-06 for the motion.

Usage:  extract_tunnel.py [launch_3_tunnel.a8s] [out.raw]
Defaults to a800dumps/launch_3_tunnel.a8s -> amiga/assets/tunnel.raw.
"""
import os, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC  = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "a800dumps/launch_3_tunnel.a8s")
OUT  = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "amiga/assets/tunnel.raw")

VIEWPORT_BASE = 0x1000   # first mode-F LMS row of the tunnel viewport
STRIDE        = 0x2E     # 46 bytes/row (wide playfield)
XCROP         = 4        # skip 4 left overscan bytes (matches terrain)
WIDTH         = 40       # central bytes kept
ROWS          = 86       # terrain/viewport height

def main():
    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as tf:
        ram_path = tf.name
    try:
        subprocess.run([sys.executable, os.path.join(ROOT, "tools/extract_a8s_ram.py"),
                        SRC, ram_path], check=True)
        ram = open(ram_path, "rb").read()
    finally:
        os.unlink(ram_path)

    out = bytearray()
    for row in range(ROWS):
        p = VIEWPORT_BASE + row * STRIDE + XCROP
        out += ram[p:p + WIDTH]
    assert len(out) == ROWS * WIDTH, len(out)

    # Sanity: the tunnel uses GTIA pens 1..6 (nibbles); flag anything unexpected.
    nibs = set()
    for b in out:
        nibs.add((b >> 4) & 0xF); nibs.add(b & 0xF)
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, "wb").write(out)
    print(f"{SRC} -> {OUT} ({len(out)} bytes, {ROWS}x{WIDTH}); nibbles used: {sorted(nibs)}")

if __name__ == "__main__":
    main()
