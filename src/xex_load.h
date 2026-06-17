/* xex_load.h — shared Atari segmented load-file (XEX) parser.
 *
 * The boot memory image is the same on both backends: zero RAM, place each XEX
 * segment at its load address, then overlay the Atari OS ROM.  The FORMAT walk
 * below is identical everywhere, so it lives here; what differs per platform is
 * (a) where the bytes come from — incbin'd .rodata on the Amiga, a file on SDL —
 * and (b) how they are stored into mem[], because the Amiga uses endian-specific
 * 32-bit block writes (a measured boot-speed win on the user-visible startup
 * delay) that would byte-swap on a little-endian host.  Both of those stay in the
 * platform TUs; each passes its own RofMemWrite to the parser.
 *
 * Type source (same convention as rof_native.h): includers must already provide
 * uint8_t/uint16_t/uint32_t.  This header does NOT pull <stdint.h> — the Amiga
 * framework's SASCCompat.h defines the fixed-width types incompatibly, so a second
 * definition clashes.  PlatformSDL.cpp gets them from the C++ stdlib; XexImage.cpp
 * from the force-included framework headers.
 */
#ifndef ROF_XEX_LOAD_H
#define ROF_XEX_LOAD_H

/* Copy `count` bytes from `src` into Atari memory starting at `start`, wrapping
 * the 16-bit address space.  Each platform supplies its mem[]-optimal store. */
typedef void (*RofMemWrite)(uint16_t start, const uint8_t* src, uint32_t count);

/* Walk an Atari segmented XEX load file (`data`, `len` bytes), invoking `write`
 * once per segment.  Format: optional leading $FFFF magic, then segments of
 * [startLo, startHi, endLo, endHi, data...]; a repeated $FFFF before a segment is
 * a (skippable) header marker.  The INITAD pseudo-segments ($02E2-$02E3) are
 * written harmlessly; the entry point is invoked from C (run -> game_entry), not
 * honoured here.  Truncated trailing segments are clamped to the remaining bytes. */
static inline void xex_parse(const uint8_t* data, uint32_t len, RofMemWrite write)
{
    uint32_t i = 0;
    if (i + 1 < len && data[i] == 0xFF && data[i + 1] == 0xFF) i += 2;   /* $FFFF magic */
    while (i + 4 <= len) {
        if (data[i] == 0xFF && data[i + 1] == 0xFF) { i += 2; continue; } /* seg marker */
        uint16_t s = (uint16_t)(data[i] | (data[i + 1] << 8));
        uint16_t e = (uint16_t)(data[i + 2] | (data[i + 3] << 8));
        i += 4;
        uint32_t cnt = (uint32_t)e - (uint32_t)s + 1u;
        if (cnt > len - i) cnt = len - i;                  /* truncated segment guard */
        write(s, data + i, cnt);
        i += cnt;
    }
}

/* Overlay the Atari OS ROM asset (the platform ROM the game reads — e.g. the
 * $E000 character set the "LEVEL nn" text renderer uses).  Layout in the asset:
 * [0..$1000) -> $C000-$CFFF, [$1000..$3800) -> $D800-$FFFF.  The $D000-$D7FF
 * hardware range is intentionally skipped, so it never overwrites mem[$D01F] (the
 * keyboard-maintained CONSOL) or other HW shadows.  `romlen` bounds short assets. */
static inline void xex_overlay_osrom(const uint8_t* rom, uint32_t romlen, RofMemWrite write)
{
    uint32_t n0 = romlen < 0x1000u ? romlen : 0x1000u;
    write(0xC000u, rom, n0);
    if (romlen > 0x1000u) {
        uint32_t n1 = romlen - 0x1000u;
        if (n1 > 0x2800u) n1 = 0x2800u;
        write(0xD800u, rom + 0x1000u, n1);
    }
}

#endif /* ROF_XEX_LOAD_H */
