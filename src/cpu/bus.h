#ifndef BUS_H
#define BUS_H
#include <stdint.h>

/* Memory bus — routes hardware register accesses through the platform
   layer and everything else straight to mem[].

   Hardware range $D000–$D7FF is not backed by mem[]; reads/writes go
   directly to the platform implementation.

   OS page-2 shadow registers ($0200–$02FF) stay in mem[]; the platform
   queries them during VBI / DLI callbacks.  The only exception is
   WSYNC ($D40A) which stalls the CPU — handled by discarding the write.
*/

#include "cpu.h"
#include "../platform/platform_c.h"

#ifdef ROF_PLATFORM_AMIGA
/* Direct, non-virtual POKEY RANDOM ($D20A) LFSR step (PlatformAmiga.cpp).  The generic
   platform_hw_read path is a C bridge + a VIRTUAL hwRead dispatch — ~2 call frames + a
   vtable lookup per read.  RANDOM is read in tight RNG loops (fill_terrain_columns reads
   it ~360x in one go → ~35ms of pure call-chain overhead), so route $D20A straight to the
   LFSR here.  Amiga-only: the SDL build keeps the platform_hw_read path so `make validate`
   still exercises the same reference LFSR as the 6502 oracle. */
/* SIZEP3 ($D00B) shadow.  SIZEP2 has a real one in the game's own RAM (mem[$00CD]), but the
   player-3 width does NOT: draw_player3_object writes it straight to GTIA ($42AE = 0 for the
   distant/1× branch, $42C0 = table $4566[mem[$006A]] = $03/$01/$01 for the three closest frames)
   and update_p3_indicator_stripe clears it again at $44C3.  The Amiga sprite mirror has to know
   how wide to draw the Main-Window object — a 4× P3 is 64 lores px, four sprite segments — so
   latch the value at the one seam every writer passes through.  Call sites with a CONSTANT
   address (all of them, in the generated code) fold the test away at compile time. */
#ifdef __cplusplus
extern "C" volatile uint8_t g_sizep3_shadow;
extern "C" uint8_t rof_pokey_random(void);
/* Direct, non-virtual POKEY register write ($D200-$D20F → Paula).  Same rationale as
   rof_pokey_random above: the generic platform_hw_write path is a C bridge + a VIRTUAL
   hwWrite dispatch, paid on EVERY POKEY write — and the 50Hz SFX envelope engine issues
   many per firing (per-voice AUDF + sfx_engine_step's 5 regs).  Route them straight to the
   shadow + Paula update here; the change-detect (skip unchanged) lives inside. */
extern "C" void rof_pokey_write(uint8_t reg, uint8_t val);
#else
extern volatile uint8_t g_sizep3_shadow;
extern uint8_t rof_pokey_random(void);
extern void rof_pokey_write(uint8_t reg, uint8_t val);
#endif
#endif

static inline uint8_t bus_read(uint16_t addr) {
    if (addr >= 0xD000 && addr < 0xD800) {
#ifdef ROF_PLATFORM_AMIGA
        if (addr == 0xD20Au) return rof_pokey_random();
#endif
        return platform_hw_read(addr);
    }
    return mem[addr];
}

static inline void bus_write(uint16_t addr, uint8_t val) {
    if (addr >= 0xD000 && addr < 0xD800) {
#ifdef ROF_HW_WRITE_POKEY_ONLY
        /* Amiga: hwWrite only acts on the POKEY audio range ($D200-$D20F → Paula) plus
           DMACTL ($D400 — the death-cinematic teardown $4F76 writes it =0 to blank the
           playfield, latched as g_flightBlank); every other hardware write is ignored, so
           skip the (virtual) platform call entirely.  POKEY writes go through the direct,
           non-virtual rof_pokey_write() fast-path (no C-bridge + no vtable); only $D400 (rare,
           off the hot path) still takes the virtual platform call. */
        if (addr >= 0xD200 && addr < 0xD210) { rof_pokey_write((uint8_t)(addr - 0xD200), val); return; }
        if (addr == 0xD00B) { g_sizep3_shadow = val; return; }   /* SIZEP3 — see the note above */
        /* HPOSP0-3 / HPOSM0-3 ($D000-$D007) shadow.  A deliberate exception to "the $D000-$D7FF
           range is not backed by mem[]": these eight are write-only on the Atari and the game
           relies on that, so nothing reads them back and the shadow is invisible to the 6502 —
           but the Amiga sprite mirrors need to know where a player/missile was put.  Written for
           the station cinematic, whose missile-dot X comes from station_missile_drift ($1910)
           writing HPOSM0-3 to GTIA (which the Amiga otherwise drops); the routine's own "park
           them" path ALREADY stores mem[$D004+Y] = 0 directly, so this just makes the two agree. */
        if (addr <= 0xD007) { mem[addr] = val; return; }
        if (addr == 0xD400) platform_hw_write(addr, val);
        return;
#else
        platform_hw_write(addr, val);
        return;
#endif
    }
    mem[addr] = val;
#ifndef ROF_PLATFORM_AMIGA
    /* Notify platform about writes to key OS shadow registers so it
       can react immediately (e.g. display list pointer changes).
       Amiga does NOT override shadowWrite (the copper owns the display and the
       VBI reads the shadow cells straight from mem[]), so there the notify is a
       C-bridge + null-check + virtual dispatch to an EMPTY base method — pure
       overhead paid on every $0200-$02FF write.  Compile it out on Amiga; the
       SDL/validation build keeps it (PlatformSDL::shadowWrite drives its display). */
    if (addr >= 0x0200 && addr < 0x0300)
        platform_shadow_write(addr, val);
#endif
}

/* Helpers for zero-page direct access (no hardware routing needed) */
static inline uint8_t  zp_read(uint8_t addr)              { return mem[addr]; }
static inline void     zp_write(uint8_t addr, uint8_t val) { mem[addr] = val; }

#endif /* BUS_H */
