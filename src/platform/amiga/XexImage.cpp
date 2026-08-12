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

#include "../../xex_load.h"   // xex_parse / xex_overlay_osrom (shared format walk)

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

// rof_load_stage_reset(): power-on RAM — zero mem[], then overlay the OS ROM.  Shared by
// the one-shot full load below and by the STAGED boot walk (rof_boot.c), which redoes it
// so the Logo stage starts from exactly the RAM the Atari's loader starts from.
extern "C" void rof_load_stage_reset(void)
{
    // Zero RAM.  mem[] is even-aligned (see RoF.map) so 32-bit writes are legal on the
    // 68000 (only ODD addresses fault) — clear by longs, 4x fewer iterations than the old
    // per-byte loop.  This runs on the boot path before anything is visible, so its cost
    // is directly the user-visible black-screen startup delay.  64 KB = whole # of longs.
    {
        volatile uint32_t* m32 = (volatile uint32_t*)mem;
        for (uint32_t i = 0; i < 65536u / 4u; i++) m32[i] = 0u;
    }
    xex_overlay_osrom(atari_osrom, (uint32_t)(atari_osrom_end - atari_osrom), amiga_mem_write);
}

// rof_load_stage(): place the next stage's segments (up to and including the one that sets
// INITAD) and return the offset to resume from; == len when the whole file is placed.
extern "C" uint32_t rof_load_stage(uint32_t from)
{
    return xex_parse_stage(rof_xex, (uint32_t)(rof_xex_end - rof_xex), from, amiga_mem_write);
}

// (XEX format + OS-ROM layout documented in xex_load.h.)  The entry point is
// invoked from C (RescueOnFractalus::run -> game_entry), not honoured here.
//
// NOTE the ROM overlay comes AFTER the segments here (it did before this split too), so a
// segment landing in $C000-$CFFF / $D800-$FFFF loses to the ROM.  rof_load_stage_reset does
// it FIRST because the staged walk has no single "after all segments" moment — no rof.xex
// segment touches either range (tools/xex_map.py), so the two orders agree.
extern "C" void load_xex_image(void)
{
    rof_load_stage_reset();
    xex_parse(rof_xex, (uint32_t)(rof_xex_end - rof_xex), amiga_mem_write);
    xex_overlay_osrom(atari_osrom, (uint32_t)(atari_osrom_end - atari_osrom), amiga_mem_write);
}
