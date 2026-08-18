/* rof_hiscore.c — the high-score save block the disk game read over SIO.  See rof_hiscore.h. */
#include "cpu/cpu.h"          /* mem[] + the integer types */
#include "platform/platform_c.h"
#include "rof_hiscore.h"
#include "rof_hiscore_factory.h"   /* kHiScoreFactory[200] — the ORIGINAL table */

/* Atari DCB (device control block) fields the 6502 fills in before the SIO call. */
#define DCB_DUNIT   0x0301
#define DCB_DCOMND  0x0302
#define DCB_DSTATS  0x0303
#define DCB_DBUFLO  0x0304
#define DCB_DBUFHI  0x0305
#define DCB_DAUX1   0x030A
#define DCB_DAUX2   0x030B

/* The two sectors the block lives in, and where each lands inside it. */
#define SECTOR_LO   0x02CE    /* -> block[0]   -> mem[$3700] */
#define SECTOR_HI   0x02CF    /* -> block[128] -> mem[$3780] */

/* SIO status codes: $01 = success, $8A = device timeout (what a missing disk returns). */
#define SIO_OK      0x01
#define SIO_ERROR   0x8A

/* Where validate_save_state $5D0D reads its expected signature from.  The compare runs
 * $37C7+Y against $7BDA+Y for Y = $26..1, so the 38 bytes we must place at $37C8 are
 * mem[$7BDA + 1] onwards, and $37C7 itself is never compared. */
#define SIGNATURE_SRC   0x7BDA
#define SIGNATURE_OFF   0x00C8    /* $37C8 - $3700 */
#define SIGNATURE_LEN   38

/* level_progress, persisted alongside the table; game_entry forces it to $10 when the
 * block fails to validate, so that is also the factory value. */
#define LEVEL_PROGRESS_OFF  0x00EE   /* $37EE - $3700 */

static unsigned char g_block[ROF_HISCORE_BLOCK_SIZE];
static int           g_dirty;

/* Outcome counters, read by amiga/name_entry.gdb.  This path is invisible from the outside —
 * a screen that never appears looks identical whether the loader failed, the gate rejected the
 * block or the game simply did not reach it — so each step gets its own tally.  Six words. */
#ifdef ROF_PLATFORM_AMIGA
extern volatile unsigned short g_hsSioRead, g_hsSioWrite, g_hsSioErr, g_hsDirty;
#define HS_COUNT(v)  do { (v)++; } while (0)
#define HS_DIRTY(v)  do { g_hsDirty = (unsigned short)(v); } while (0)
#else
#define HS_COUNT(v)  do { } while (0)
#define HS_DIRTY(v)  do { } while (0)
#endif

void rof_hiscore_init(void)
{
    int i;

    for (i = 0; i < ROF_HISCORE_BLOCK_SIZE; i++)
        g_block[i] = 0;
    for (i = 0; i < (int)sizeof(kHiScoreFactory); i++)
        g_block[i] = kHiScoreFactory[i];
    for (i = 0; i < SIGNATURE_LEN; i++)
        g_block[SIGNATURE_OFF + i] = mem[SIGNATURE_SRC + 1 + i];
    g_block[LEVEL_PROGRESS_OFF] = 0x10;

    /* A saved block replaces the factory one wholesale — including its signature, which
     * the player's own build wrote, so a block carried between builds still validates
     * only if the signatures agree.  That is the original's rule, not ours. */
    platform_hiscore_load(g_block);
    g_dirty = 0;
}

void rof_sio_block(void)
{
    uint16_t sector = (uint16_t)(mem[DCB_DAUX1] | (mem[DCB_DAUX2] << 8));
    uint16_t buf    = (uint16_t)(mem[DCB_DBUFLO] | (mem[DCB_DBUFHI] << 8));
    uint8_t  cmd    = mem[DCB_DCOMND];
    int      off;
    int      i;

    if      (sector == SECTOR_LO) off = 0;
    else if (sector == SECTOR_HI) off = ROF_HISCORE_SECTOR_SIZE;
    else { HS_COUNT(g_hsSioErr); mem[DCB_DSTATS] = SIO_ERROR; return; }

    if (cmd == 0x52) {                      /* 'R' — read sector into mem[] */
        for (i = 0; i < ROF_HISCORE_SECTOR_SIZE; i++)
            mem[(uint16_t)(buf + i)] = g_block[off + i];
        HS_COUNT(g_hsSioRead);
    } else if (cmd == 0x57 || cmd == 0x50) {   /* 'W' / 'P' — write mem[] into the block */
        for (i = 0; i < ROF_HISCORE_SECTOR_SIZE; i++)
            g_block[off + i] = mem[(uint16_t)(buf + i)];
        g_dirty = 1;
        /* Offer it to the platform straight away.  A backend that can save here (WHDLoad's
         * resload_SaveFile) takes it; one that cannot (a plain Amiga run, mid-Forbid with
         * the display hijacked) declines and the dirty flag survives for the exit flush. */
        HS_COUNT(g_hsSioWrite);
        if (platform_hiscore_save(g_block))
            g_dirty = 0;
        HS_DIRTY(g_dirty);
    } else {
        HS_COUNT(g_hsSioErr);
        mem[DCB_DSTATS] = SIO_ERROR;
        return;
    }

    mem[DCB_DSTATS] = SIO_OK;
}

const unsigned char *rof_hiscore_block(void) { return g_block; }
int  rof_hiscore_dirty(void)       { return g_dirty; }
void rof_hiscore_clear_dirty(void) { g_dirty = 0; }

void rof_hiscore_flush(void)
{
    if (!g_dirty) return;
    if (platform_hiscore_save(g_block)) g_dirty = 0;
    HS_DIRTY(g_dirty);
}

#ifndef ROF_PLATFORM_AMIGA
/* The host/SDL build renders mem[] straight through the display list, so there is nothing to
 * mark.  The Amiga definition lives in RescueOnFractalus.cpp with the rest of the render state. */
void rof_hiscore_screen_dirty(unsigned short addr, unsigned char nCells)
{
    (void)addr; (void)nCells;
}
#endif
