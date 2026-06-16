// Boot memory image — load the pristine rof.xex into mem[].
//
// This is the faithful initial state: instead of booting from a hand-captured
// mid-Standby RAM snapshot (which baked in runtime state such as $00E7=1, the music
// gate), we reproduce what the Atari OS loader does — zero RAM, then place each XEX
// segment at its load address.  All runtime state then starts at genuine power-on
// values and the original setup code (the INITAD chain ending at game_entry $3CDE)
// establishes everything itself.
//
// rof.xex is embedded in .rodata by incbin.s (rof_xex / rof_xex_end).
//
// Integer types come from the project's force-included framework/SASCCompat.h
// (CPPFLAGS -include), like the other Amiga TUs — no <stdint.h> here.

extern "C" volatile uint8_t mem[65536];

// The original Atari 8-bit segmented load file, embedded by incbin.s.
extern "C" uint8_t rof_xex[];
extern "C" uint8_t rof_xex_end[];

// The Atari OS ROM (platform ROM), embedded by incbin.s: $C000-$CFFF (4 KB) then
// $D800-$FFFF (10 KB) — the $D000-$D7FF hardware range is skipped.  Includes the
// internal character set at $E000 the game's text renderer reads.
extern "C" uint8_t atari_osrom[];
extern "C" uint8_t atari_osrom_end[];

// XEX format: optional leading $FFFF magic, then segments of
//   [startLo, startHi, endLo, endHi, data...]
// A repeated $FFFF before a segment is a (skippable) header marker.  The INITAD
// pseudo-segments ($02E2-$02E3) are loaded harmlessly; the entry point is invoked
// from C (RescueOnFractalus::run -> game_entry) rather than honoured here.
extern "C" void load_xex_image(void)
{
    // Zero RAM.  mem[] is even-aligned (see RoF.map) so 32-bit writes are legal on the
    // 68000 (only ODD addresses fault) — clear by longs, 4x fewer iterations than the old
    // per-byte loop.  This runs on the boot path before anything is visible, so its cost
    // is directly the user-visible black-screen startup delay.  64 KB = whole # of longs.
    {
        volatile uint32_t* m32 = (volatile uint32_t*)mem;
        for (uint32_t i = 0; i < 65536u / 4u; i++) m32[i] = 0u;
    }

    const uint8_t* d   = rof_xex;
    const uint32_t len = (uint32_t)(rof_xex_end - rof_xex);
    uint32_t i = 0;
    if (i + 1 < len && d[i] == 0xFF && d[i + 1] == 0xFF) i += 2;   // skip $FFFF magic
    while (i + 4 <= len) {
        if (d[i] == 0xFF && d[i + 1] == 0xFF) { i += 2; continue; } // segment-header marker
        uint16_t s = (uint16_t)(d[i] | (d[i + 1] << 8));
        uint16_t e = (uint16_t)(d[i + 2] | (d[i + 3] << 8));
        i += 4;
        uint32_t seglen = (uint32_t)e - (uint32_t)s + 1u;
        uint32_t cnt = seglen;
        if (cnt > len - i) cnt = len - i;             // truncated segment guard (was i<len)
        const uint8_t* src = d + i;
        if ((uint32_t)s + cnt <= 0x10000u) {
            // mem[] is even-aligned, so long writes are legal on the 68000 at any even Atari
            // address (only ODD faults).  Align the dest to even (≤1 head byte), then store
            // the body 4 bytes at a time — 4x fewer volatile stores than the per-byte loop.
            // The XEX data (src) is read by bytes (arbitrary alignment) and packed big-endian
            // (68000) so mem[s+k]=src[k].
            uint32_t k = 0;
            if (cnt && (s & 1u)) { mem[s] = src[0]; k = 1; }
            for (; k + 4u <= cnt; k += 4u)
                *(volatile uint32_t*)(mem + (uint16_t)(s + k)) =
                    ((uint32_t)src[k] << 24) | ((uint32_t)src[k + 1] << 16) |
                    ((uint32_t)src[k + 2] << 8) | (uint32_t)src[k + 3];
            for (; k < cnt; k++) mem[(uint16_t)(s + k)] = src[k];
        } else {
            for (uint32_t k = 0; k < cnt; k++) mem[(uint16_t)(s + k)] = src[k];  // wraps $FFFF
        }
        i += cnt;
    }

    // Overlay the Atari OS ROM (the platform ROM the game reads — e.g. the $E000
    // character set the "LEVEL nn" text renderer uses).  Layout in the asset:
    // [0..$1000) -> $C000-$CFFF, [$1000..$3800) -> $D800-$FFFF.  The $D000-$D7FF
    // hardware range is intentionally NOT covered, so it never overwrites mem[$D01F]
    // (the keyboard-maintained CONSOL) or other HW shadows.
    // atari_osrom is .balign 4 (incbin.s) and the dest offsets ($C000/$D800) are even, so
    // copy by longs (both blocks are whole # of longs).
    {
        const uint32_t*   rs = (const uint32_t*)atari_osrom;
        volatile uint32_t* d0 = (volatile uint32_t*)(mem + 0xC000u);
        for (uint32_t k = 0; k < 0x1000u / 4u; k++) d0[k] = rs[k];
        volatile uint32_t* d1 = (volatile uint32_t*)(mem + 0xD800u);
        const uint32_t*    r1 = (const uint32_t*)(atari_osrom + 0x1000u);
        for (uint32_t k = 0; k < 0x2800u / 4u; k++) d1[k] = r1[k];
    }
}
