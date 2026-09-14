#!/usr/bin/env python3
"""Export Rescue on Fractalus character sets as fixed-slot PNG strips.

Every output is a single horizontal row.  Slot order is the game's native
character-code order; blank source glyphs and unsupported semantic slots remain
transparent.  Pixels are enlarged with nearest-neighbour scaling only.
"""

from pathlib import Path

from PIL import Image


ROOT = Path(__file__).resolve().parents[1]
OUT = Path.cwd()
SCALE = 4


def glyph_1bpp(rows):
    image = Image.new("RGBA", (8, 8), (0, 0, 0, 0))
    pixels = image.load()
    for y, row in enumerate(rows):
        for x in range(8):
            if row & (0x80 >> x):
                pixels[x, y] = (255, 255, 255, 255)
    return image


def glyph_mode4(rows):
    """Decode an ANTIC mode-4 glyph: four 2-bit pixels, each 2 px wide."""
    palette = {
        0: (0, 0, 0, 0),
        1: (105, 105, 105, 255),
        2: (180, 180, 180, 255),
        3: (255, 255, 255, 255),
    }
    image = Image.new("RGBA", (8, 8), (0, 0, 0, 0))
    pixels = image.load()
    for y, row in enumerate(rows):
        for pair in range(4):
            value = (row >> (6 - pair * 2)) & 3
            pixels[pair * 2, y] = palette[value]
            pixels[pair * 2 + 1, y] = palette[value]
    return image


def strip(glyphs, slot_width, slot_height, filename):
    image = Image.new("RGBA", (len(glyphs) * slot_width, slot_height), (0, 0, 0, 0))
    for index, glyph in enumerate(glyphs):
        if glyph is not None:
            image.alpha_composite(glyph, (index * slot_width, 0))
    image = image.resize((image.width * SCALE, image.height * SCALE), Image.Resampling.NEAREST)
    image.save(OUT / filename, optimize=True)


def main():
    memory = (ROOT / "disasm" / "rof_mem.bin").read_bytes()
    atari = (ROOT / "amiga" / "assets" / "atari_charset.bin").read_bytes()

    if len(memory) != 0x10000:
        raise SystemExit("disasm/rof_mem.bin must be a 64 KiB runtime RAM image")
    if len(atari) != 128 * 8:
        raise SystemExit("atari_charset.bin must contain 128 8-byte glyphs")

    text = memory[0x0400:0x0600]
    cockpit = memory[0x3800:0x3C00]
    if len(text) != 64 * 8 or len(cockpit) != 128 * 8:
        raise SystemExit("runtime character-set ranges have unexpected lengths")

    strip([glyph_1bpp(atari[i * 8:i * 8 + 8]) for i in range(128)], 8, 8,
          "atari-system-font.png")
    strip([glyph_1bpp(text[i * 8:i * 8 + 8]) for i in range(64)], 8, 8,
          "rof-text-font.png")
    strip([glyph_mode4(cockpit[i * 8:i * 8 + 8]) for i in range(128)], 8, 8,
          "rof-cockpit-tiles.png")

    # $4AE3 stores ten 2x2 tile metaglyphs.  Each group of four bytes is laid
    # out top-left, top-right, bottom-left, bottom-right and indexes $3800.
    digit_map = memory[0x4AE3:0x4AE3 + 10 * 4]
    digits = []
    for digit in range(10):
        composite = Image.new("RGBA", (16, 16), (0, 0, 0, 0))
        for part, code in enumerate(digit_map[digit * 4:digit * 4 + 4]):
            tile = glyph_mode4(cockpit[(code & 0x7F) * 8:(code & 0x7F) * 8 + 8])
            composite.alpha_composite(tile, ((part & 1) * 8, (part >> 1) * 8))
        digits.append(composite)
    digits.extend([None] * 6)  # Fixed hexadecimal-sized row; A-F are unsupported.
    strip(digits, 16, 16, "rof-cockpit-digits.png")

    for filename in (
        "atari-system-font.png",
        "rof-text-font.png",
        "rof-cockpit-tiles.png",
        "rof-cockpit-digits.png",
    ):
        path = OUT / filename
        print(f"{path}: {Image.open(path).size[0]}x{Image.open(path).size[1]}")


if __name__ == "__main__":
    main()
