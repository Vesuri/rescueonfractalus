#!/usr/bin/env python3
"""Convert the supplied 16-colour logo PNG into WHDLoad planar assets.

The 288x100 source is centred in the game's 320-pixel field.  GAMES occupies an
88x30 source rectangle (x=104..191, y=65..94): those pixels are cleared from the
initial field and emitted separately for the phase-2 copy in RoFSlave.s.
"""

from pathlib import Path
import struct
import sys

from PIL import Image


CANVAS_WIDTH = 320
SOURCE_SIZE = (288, 100)
SOURCE_X = (CANVAS_WIDTH - SOURCE_SIZE[0]) // 2
GAMES_BOX = (104, 65, 192, 95)  # right/bottom exclusive; 88x30


def ocs_word(rgb, used):
    """Nearest 12-bit OCS colour, nudged only when two source pens collide."""
    r, g, b = (round(c / 17) for c in rgb)
    candidates = []
    for dr in (-1, 0, 1):
        for dg in (-1, 0, 1):
            for db in (-1, 0, 1):
                rr, gg, bb = r + dr, g + dg, b + db
                if 0 <= rr < 16 and 0 <= gg < 16 and 0 <= bb < 16:
                    word = (rr << 8) | (gg << 4) | bb
                    restored = (rr * 17, gg * 17, bb * 17)
                    error = sum((restored[i] - rgb[i]) ** 2 for i in range(3))
                    candidates.append((error, word))
    for _, word in sorted(candidates):
        if word not in used:
            used.add(word)
            return word
    raise ValueError("could not assign a distinct OCS colour")


def encode_planar(pens, x, y, width, height, stride):
    if x % 8 or width % 8:
        raise ValueError("planar region must be byte aligned")
    out = bytearray()
    for yy in range(y, y + height):
        for plane in range(4):
            for bx in range(x, x + width, 8):
                value = 0
                for bit in range(8):
                    value |= ((pens[yy * stride + bx + bit] >> plane) & 1) << (7 - bit)
                out.append(value)
    return bytes(out)


def decode_planar(data, width, height):
    """Round-trip oracle for the full interleaved field."""
    plane_bytes = width // 8
    pens = [0] * (width * height)
    pos = 0
    for y in range(height):
        for plane in range(4):
            for bx in range(plane_bytes):
                value = data[pos]
                pos += 1
                for bit in range(8):
                    pens[y * width + bx * 8 + bit] |= ((value >> (7 - bit)) & 1) << plane
    return pens


def main():
    if len(sys.argv) != 3:
        raise SystemExit(f"usage: {Path(sys.argv[0]).name} SOURCE.png OUTPUT_DIR")

    source = Path(sys.argv[1])
    output = Path(sys.argv[2])
    image = Image.open(source).convert("RGB")
    if image.size != SOURCE_SIZE:
        raise SystemExit(f"expected {SOURCE_SIZE[0]}x{SOURCE_SIZE[1]}, got {image.size}")

    colours = sorted({image.getpixel((x, y)) for y in range(image.height)
                      for x in range(image.width)}, key=lambda c: (sum(c), c))
    background = image.getpixel((0, image.height - 1))
    if len(colours) != 16 or colours[0] != background:
        raise SystemExit("source must have exactly 16 colours with the darkest corner as pen 0")
    pen_for = {rgb: pen for pen, rgb in enumerate(colours)}

    full = [0] * (CANVAS_WIDTH * image.height)
    for y in range(image.height):
        for x in range(image.width):
            full[y * CANVAS_WIDTH + SOURCE_X + x] = pen_for[image.getpixel((x, y))]

    base = full.copy()
    gx0, gy0, gx1, gy1 = GAMES_BOX
    canvas_gx0 = SOURCE_X + gx0
    for y in range(gy0, gy1):
        for x in range(canvas_gx0, SOURCE_X + gx1):
            base[y * CANVAS_WIDTH + x] = 0

    base_data = encode_planar(base, 0, 0, CANVAS_WIDTH, image.height, CANVAS_WIDTH)
    full_data = encode_planar(full, 0, 0, CANVAS_WIDTH, image.height, CANVAS_WIDTH)
    games_data = encode_planar(full, canvas_gx0, gy0, gx1 - gx0, gy1 - gy0, CANVAS_WIDTH)
    if len(base_data) != 16000 or len(games_data) != 1320:
        raise AssertionError("unexpected planar asset size")
    if decode_planar(base_data, CANVAS_WIDTH, image.height) != base:
        raise AssertionError("planar round trip failed")
    composited = bytearray(base_data)
    span = (gx1 - gx0) // 8
    pos = 0
    for y in range(gy0, gy1):
        for plane in range(4):
            dst = y * 160 + plane * 40 + canvas_gx0 // 8
            composited[dst:dst + span] = games_data[pos:pos + span]
            pos += span
    if bytes(composited) != full_data:
        raise AssertionError("GAMES overlay does not reconstruct the full planar image")

    used = set()
    palette = [ocs_word(rgb, used) for rgb in colours]
    palette_data = b"".join(struct.pack(">H", word) for word in palette)

    output.mkdir(parents=True, exist_ok=True)
    (output / "enhanced_logo.bin").write_bytes(base_data)
    (output / "enhanced_games.bin").write_bytes(games_data)
    (output / "enhanced_logo.pal").write_bytes(palette_data)

    # Reviewable previews use the quantised OCS colours actually shown by the Amiga.
    preview_palette = [(word >> 8 & 15, word >> 4 & 15, word & 15) for word in palette]
    preview_palette = [(r * 17, g * 17, b * 17) for r, g, b in preview_palette]
    for name, pixels in (("enhanced_logo_base.png", base), ("enhanced_logo_full.png", full)):
        preview = Image.new("RGB", (CANVAS_WIDTH, image.height))
        preview.putdata([preview_palette[p] for p in pixels])
        preview.save(output / name)

    print(f"base={len(base_data)} games={len(games_data)} palette={len(palette_data)}")
    print("palette=" + " ".join(f"{word:03x}" for word in palette))


if __name__ == "__main__":
    main()
