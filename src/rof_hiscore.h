/* rof_hiscore.h — the high-score save block ($3700-$37FF) the disk game read over SIO.
 *
 * rof.xex is the disk game converted to an executable, and the conversion NOPped out both
 * sector reads that filled the block (see docs/high-score-initials.md).  With the block all
 * zeros, validate_save_state $5D0D never passes, so the high-score table and the initials
 * entry have been dead in the binary we port.  This module supplies the block again:
 * the factory-fresh contents on first run, whatever the player last saved after that.
 *
 * The seam is the SIO call itself — rof_sio_block() is hooked in at the two NOPped-out
 * `JSR $5DBC` sites ($5D86 read, $5D9D write; tools/transpile.py PRE_INSN_HOOKS), so the
 * game's own DCB setup, its read at boot and its write from name_entry_loop are all the
 * original 6502 code.  See docs/high-score-restore-plan.md.
 */
#ifndef ROF_HISCORE_H
#define ROF_HISCORE_H

#if !defined(ROF_PLATFORM_AMIGA)
#include <stdint.h>   /* Amiga: types come from cpu.h / the force-included SASCCompat.h,
                         whose compat <stdint.h> would clash — same rule as platform_c.h. */
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Size of the block, and of one SIO sector's worth of it. */
#define ROF_HISCORE_BLOCK_SIZE  256
#define ROF_HISCORE_SECTOR_SIZE 128

/* Seed the in-memory block: the factory table, then the running binary's own copyright
 * signature out of mem[$7BDA+1] (the two v4.1 builds disagree on the year, so it must be
 * copied, never hardcoded), then whatever platform_hiscore_load() supplies.
 * mem[] must already hold the loaded game image.  Call once, before game_entry(). */
void rof_hiscore_init(void);

/* The patched-out SIO call.  Reads the DCB the 6502 just built ($0301 DUNIT, $0302 DCOMND,
 * $0304/$0305 DBUF, $030A/$030B DAUX = sector) and moves 128 bytes between mem[] and the
 * block, then sets DSTATS $0303.  A sector other than $02CE/$02CF gets an error status —
 * nothing in the binary asks for one. */
void rof_sio_block(void);

/* The block, and whether the game has written to it since the last successful save.
 * A platform that defers persistence (the plain Amiga build saves after it has given the
 * display and multitasking back) flushes from here on the way out. */
const unsigned char *rof_hiscore_block(void);
int  rof_hiscore_dirty(void);
void rof_hiscore_clear_dirty(void);

/* Report that nCells cells of the high-score screen were written, starting at Atari screen-RAM
 * address `addr` ($3700-$37C7).  Called from the transpiled writers themselves
 * (tools/transpile.py PRE_INSN_HOOKS at the ten `STA $37xx,X` sites in name_entry_loop and
 * render_text_cell) — the choke point, not the callers.  A bitplane-mirroring platform grows a
 * cell range so render() redraws only what moved; a direct call, not a virtual through the
 * platform bridge, because the row shift issues one per byte.  No-op on the host/SDL build,
 * which renders mem[] directly. */
void rof_hiscore_screen_dirty(unsigned short addr, unsigned char nCells);

/* Save now if there is anything to save; clears the dirty flag when the platform reports
 * the write went through.  Safe to call when nothing is dirty. */
void rof_hiscore_flush(void);

#ifdef __cplusplus
}
#endif

#endif /* ROF_HISCORE_H */
