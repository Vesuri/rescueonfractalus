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

static inline uint8_t bus_read(uint16_t addr) {
    if (addr >= 0xD000 && addr < 0xD800)
        return platform_hw_read(addr);
    return mem[addr];
}

static inline void bus_write(uint16_t addr, uint8_t val) {
    if (addr >= 0xD000 && addr < 0xD800) {
        platform_hw_write(addr, val);
        return;
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
