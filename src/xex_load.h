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

/* Walk ONE STAGE of the same file: place segments starting at byte offset `from`
 * and stop after the segment that writes INITAD ($02E2-$02E3), returning the offset
 * to resume from.  That is exactly what the Atari OS loader does — it places
 * segments until one sets INITAD, JSRs through it, then carries on — and it is the
 * only way the boot scenes can see the memory they were written for: rof.xex's later
 * segments OVERWRITE the earlier ones (segment 16, $3CDE-$B7FF, buries both the logo
 * code at $5000 and the station image staged at $4000), so a single full load leaves
 * NO instant at which either scene's data is in RAM.  See docs/logo-station-plan.md §3.
 *
 * rof.xex has four INITAD segments and therefore four stages:
 *   1..7   -> INITAD $5000  the Lucasfilm logo
 *   8..11  -> INITAD $1A97  screen page swap + the station cinematic
 *   12..14 -> INITAD $B800  display init (CHBAS/SDLSTL/colours/DMACTL)
 *   15..20 -> INITAD $3CDE  game_entry — the real program entry
 * Returns `len` once the walk is done, so `while (off < len)` terminates.
 * Pass from = 0 for the first stage (the leading $FFFF magic is skipped here).      */
static inline uint32_t xex_parse_stage(const uint8_t* data, uint32_t len,
                                       uint32_t from, RofMemWrite write)
{
    uint32_t i = from;
    if (i == 0 && i + 1 < len && data[i] == 0xFF && data[i + 1] == 0xFF) i += 2;
    while (i + 4 <= len) {
        if (data[i] == 0xFF && data[i + 1] == 0xFF) { i += 2; continue; } /* seg marker */
        uint16_t s = (uint16_t)(data[i] | (data[i + 1] << 8));
        uint16_t e = (uint16_t)(data[i + 2] | (data[i + 3] << 8));
        i += 4;
        uint32_t cnt = (uint32_t)e - (uint32_t)s + 1u;
        if (cnt > len - i) cnt = len - i;                  /* truncated segment guard */
        write(s, data + i, cnt);
        i += cnt;
        /* INITAD is $02E2-$02E3; a segment covering either byte ends the stage. */
        if (s <= 0x02E3u && e >= 0x02E2u) return i;
    }
    return len;
}

/* ---------------------------------------------------------------------------------------
 * The SPARSE boot image (tools/make_xex_sparse.py) — what the Amiga embeds instead of the
 * whole rof.xex.  The port transliterates the 6502 to C, so the image's instruction bytes
 * are never executed from mem[]; omitting the ~23 KB that no data read ever touches takes
 * the asset from 43,066 B to 20,051 B.
 *
 *   chunk  := u16 load_addr, u16 len, len bytes      (big-endian, 68000-native)
 *   stream := chunk*
 *
 * There is deliberately no stage marker: a stage ends at the chunk covering INITAD
 * ($02E2-$02E3), the SAME rule xex_parse_stage uses for segments.  That keeps this function
 * a structural twin of it and means rof_boot.c's stage walk is unchanged.  Bytes not
 * mentioned by any chunk stay at whatever rof_load_stage_reset left, i.e. 0 — which is
 * exactly the state the removal was validated against.
 *
 * ⚠ The generator emits a write wherever its SIMULATED mem[] disagrees with the target, not
 * merely where a byte is live.  That is load-bearing: segment 16 buries the logo staged at
 * $5000 and the station image at $4000, so a dead byte there must still be written (as 0) to
 * CLOBBER the earlier data.  Do not "optimise" the asset by dropping zero runs.            */
static inline uint32_t xex_sparse_stage(const uint8_t* data, uint32_t len,
                                        uint32_t from, RofMemWrite write)
{
    uint32_t i = from;
    while (i + 4 <= len) {
        uint16_t a = (uint16_t)(((uint16_t)data[i] << 8) | data[i + 1]);
        uint32_t n = (uint32_t)(((uint16_t)data[i + 2] << 8) | data[i + 3]);
        i += 4;
        if (n > len - i) n = len - i;                  /* truncated-asset guard */
        write(a, data + i, n);
        i += n;
        /* INITAD is $02E2-$02E3; a chunk covering either byte ends the stage. */
        if (a <= 0x02E3u && (uint32_t)a + n > 0x02E2u) return i;
    }
    return len;
}

/* Place the WHOLE sparse image in one go (the non-staged path). */
static inline void xex_sparse_all(const uint8_t* data, uint32_t len, RofMemWrite write)
{
    uint32_t i = 0;
    while (i < len) {
        uint32_t next = xex_sparse_stage(data, len, i, write);
        if (next <= i) break;                          /* never spin on a malformed asset */
        i = next;
    }
}

/* Overlay JUST the Atari internal character set at $E000-$E3FF (1 KB, 128 glyphs x 8
 * rows).  This is the ONLY thing the port ever reads out of the Atari OS ROM, so it is
 * what the Amiga build ships instead of the whole 14 KB ROM.  Two readers, both of which
 * build the pointer $0084/$0085 and blit through it via blit_glyph_8rows:
 *   glyph_ptr_from_index $6773  $E000 + (code<<3), codes from the $6E23 table ("LEVEL",
 *                               "DROID") -> $E120-$E1B7
 *   set_coord_y_e0       $6805  $0084 = X+$80, $0085 = $E0            -> $E080-$E106
 * Neither can leave this page: the pointer is built with three ASLs and a SINGLE ROL to
 * capture the last carry, so the high byte is $E0 or $E1 and never $E2+ — $E000-$E1FF
 * bounds every reachable glyph, and one 1 KB page is a strict superset of that.
 * The rest of the ROM is not needed at all: the only two non-charset ROM references in the
 * whole binary are JSR $E45C (SETVBV) and JMP $E462 (XITVBV), which are OS *calls*, and the
 * port implements both as no-op stubs (os_setvbv / os_xitvbv in rof_gen.c) because VBI
 * dispatch belongs to the platform layer.  `len` bounds short assets. */
static inline void xex_overlay_charset(const uint8_t* cs, uint32_t len, RofMemWrite write)
{
    if (len > 0x400u) len = 0x400u;
    write(0xE000u, cs, len);
}

/* Overlay a WHOLE Atari OS ROM asset instead.  Kept for the SDL dev backend, which will
 * use a full ROM if one is lying around: it places the ranges the charset-only asset
 * cannot, so it stays available as a divergence detector if a build is ever suspected of
 * reading ROM outside the charset (the port would then behave differently under the two
 * assets).  Layout: [0..$1000) -> $C000-$CFFF, [$1000..$3800) -> $D800-$FFFF.  The
 * $D000-$D7FF hardware range is intentionally skipped, so it never overwrites mem[$D01F]
 * (the keyboard-maintained CONSOL) or other HW shadows.  `romlen` bounds short assets. */
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
