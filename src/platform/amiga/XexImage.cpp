// Boot memory image — load the pristine rof.xex into mem[].
//
// This is the faithful initial state: instead of booting from a hand-captured
// mid-Standby RAM snapshot (which baked in runtime state such as $00E7=1, the music
// gate), we reproduce what the Atari OS loader does — zero RAM, then place each XEX
// segment at its load address.  All runtime state then starts at genuine power-on
// values and the original setup code (the INITAD chain ending at game_entry $3CDE)
// establishes everything itself.
//
// The boot image is embedded in .rodata by incbin.s (rof_boot_image / _end); see below
// for the sparse-vs-full-xex choice.
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

// The boot memory image, embedded by incbin.s.  By default this is the SPARSE image
// (assets/rof_boot_image.bin, 27,872 B) — the same mem[] the original loader produces with
// 15,467 never-read 6502 instruction bytes omitted.  `make FULLXEX=1` embeds the original
// 43,066-byte rof.xex here instead and takes the segment-walk branch below, which is the A/B
// to reach for if the sparse image is ever suspected.  Both loaders are proven equivalent on
// the host by tools/test_xex_sparse.c (all four stage boundaries + the full load).
extern "C" uint8_t rof_boot_image[];
extern "C" uint8_t rof_boot_image_end[];

#ifdef ROF_FULL_XEX
#define ROF_BOOT_STAGE(d, l, f, w) xex_parse_stage((d), (l), (f), (w))
#define ROF_BOOT_ALL(d, l, w)      xex_parse((d), (l), (w))
#else
#define ROF_BOOT_STAGE(d, l, f, w) xex_sparse_stage((d), (l), (f), (w))
#define ROF_BOOT_ALL(d, l, w)      xex_sparse_all((d), (l), (w))
#endif

// The Atari internal CHARACTER SET at $E000-$E3FF (1 KB), embedded by incbin.s — the only
// part of the Atari OS ROM the port ever reads.  It replaced the whole 14 KB ROM: see
// xex_overlay_charset in xex_load.h for the two readers and why neither can leave the page.
extern "C" uint8_t atari_charset[];
extern "C" uint8_t atari_charset_end[];

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
    xex_overlay_charset(atari_charset, (uint32_t)(atari_charset_end - atari_charset),
                        amiga_mem_write);
}

// rof_load_stage(): place the next stage's segments (up to and including the one that sets
// INITAD) and return the offset to resume from; == len when the whole file is placed.
extern "C" uint32_t rof_load_stage(uint32_t from)
{
    return ROF_BOOT_STAGE(rof_boot_image, (uint32_t)(rof_boot_image_end - rof_boot_image),
                          from, amiga_mem_write);
}

// (XEX format + charset layout documented in xex_load.h.)  The entry point is
// invoked from C (RescueOnFractalus::run -> game_entry), not honoured here.
//
// NOTE the charset overlay comes AFTER the segments here (as the ROM overlay did before it),
// so a segment landing in $E000-$E3FF would lose to the charset.  rof_load_stage_reset does
// it FIRST because the staged walk has no single "after all segments" moment — no rof.xex
// segment touches that range (tools/xex_map.py), so the two orders agree.
extern "C" void load_xex_image(void)
{
    rof_load_stage_reset();
    ROF_BOOT_ALL(rof_boot_image, (uint32_t)(rof_boot_image_end - rof_boot_image),
                 amiga_mem_write);
    xex_overlay_charset(atari_charset, (uint32_t)(atari_charset_end - atari_charset),
                        amiga_mem_write);
}
