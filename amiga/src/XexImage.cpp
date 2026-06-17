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
//
// The XEX-format walk + OS-ROM layout are shared with the SDL backend in
// xex_load.h; this TU supplies the Amiga data source (incbin'd .rodata) and the
// mem[] writer (endian-specific 32-bit block stores — a boot-speed win).

#include "../../src/xex_load.h"   // xex_parse / xex_overlay_osrom (shared format walk)

extern "C" volatile uint8_t mem[65536];

// Amiga mem[] writer: mem[] is even-aligned (see RoF.map) so 32-bit writes are legal on
// the 68000 (only ODD addresses fault).  Align the dest to even (≤1 head byte), then store
// the body 4 bytes at a time — 4x fewer volatile stores than a per-byte loop, on the boot
// path whose cost is directly the user-visible black-screen startup delay.  The source is
// read by bytes (arbitrary alignment) and packed big-endian (68000), so mem[start+k]=src[k].
static void amiga_mem_write(uint16_t s, const uint8_t* src, uint32_t cnt)
{
    if ((uint32_t)s + cnt <= 0x10000u) {
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
}

// The original Atari 8-bit segmented load file, embedded by incbin.s.
extern "C" uint8_t rof_xex[];
extern "C" uint8_t rof_xex_end[];

// The Atari OS ROM (platform ROM), embedded by incbin.s: $C000-$CFFF (4 KB) then
// $D800-$FFFF (10 KB) — the $D000-$D7FF hardware range is skipped.  Includes the
// internal character set at $E000 the game's text renderer reads.
extern "C" uint8_t atari_osrom[];
extern "C" uint8_t atari_osrom_end[];

// (XEX format + OS-ROM layout documented in xex_load.h.)  The entry point is
// invoked from C (RescueOnFractalus::run -> game_entry), not honoured here.
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

    // Place each XEX segment, then overlay the OS ROM — both via the shared walk in
    // xex_load.h, with amiga_mem_write doing the 68000 block stores.
    xex_parse(rof_xex, (uint32_t)(rof_xex_end - rof_xex), amiga_mem_write);
    xex_overlay_osrom(atari_osrom, (uint32_t)(atari_osrom_end - atari_osrom), amiga_mem_write);
}
