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
#ifdef __cplusplus
extern "C" uint8_t rof_pokey_random(void);
#else
extern uint8_t rof_pokey_random(void);
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
           skip the (virtual) platform call entirely.  Behaviourally identical otherwise — the
           call had no side effect — but it drops ~20 dead GTIA/ANTIC virtual calls per
           flight-VBI firing (and elsewhere).  (The flight VBI never writes $D400, so the
           extra compare is off the hot path.) */
        if ((addr >= 0xD200 && addr < 0xD210) || addr == 0xD400)
            platform_hw_write(addr, val);
        return;
#else
        platform_hw_write(addr, val);
        return;
#endif
    }
    mem[addr] = val;
    /* Notify platform about writes to key OS shadow registers so it
       can react immediately (e.g. display list pointer changes).
       This is optional in Phase 2 / stub — kept for later phases. */
    if (addr >= 0x0200 && addr < 0x0300)
        platform_shadow_write(addr, val);
}

/* Helpers for zero-page direct access (no hardware routing needed) */
static inline uint8_t  zp_read(uint8_t addr)              { return mem[addr]; }
static inline void     zp_write(uint8_t addr, uint8_t val) { mem[addr] = val; }

#endif /* BUS_H */
