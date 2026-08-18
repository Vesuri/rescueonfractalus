/* hiscore_block_test — host proof for the restored high-score save block (src/rof_hiscore.c).
 *
 * There is no 6502 oracle for this: the code it replaces was DELETED from rof.xex (both SIO
 * sector reads are three NOPs), so `make validate` structurally cannot reach it.  What CAN be
 * proved on the host is the only thing that matters — that the block the port supplies is one
 * the ORIGINAL 6502 accepts.  So the checks below are transcriptions of the game's own code:
 *
 *   validate_save_state $5D0D   mem[$3700]==$28, mem[$3714]==$EE, and
 *                               mem[$37C7+Y]==mem[$7BDA+Y] for Y=$26..1
 *   game_init_5D50 $5D50        the DCB the 6502 builds before each of the two calls
 *   name_entry_loop $5C54       the same, with DCOMND 'W'
 *
 * Proves: the factory block passes the gate; a round trip through the write path is lossless;
 * a saved block replaces the factory one; and a foreign sector is refused rather than aliased
 * onto the table.
 */
#include <stdio.h>
#include <string.h>

#include "cpu/cpu.h"

/* --- the mem[] the transliteration would own, and the platform seam, stubbed here ---------- */
ROF_MEM_QUAL uint8_t mem[65536];
Cpu6502 cpu;

static uint8_t g_saved[256];
static int     g_savedValid = 0;   /* what platform_hiscore_load hands back */
static int     g_saveCalls   = 0;
static int     g_saveAccepts = 1;  /* 0 = the platform DECLINES (the plain Amiga arm) */

int platform_hiscore_load(uint8_t blk[256])
{
    if (!g_savedValid) return 0;
    memcpy(blk, g_saved, 256);
    return 1;
}

int platform_hiscore_save(const uint8_t blk[256])
{
    g_saveCalls++;
    if (!g_saveAccepts) return 0;
    memcpy(g_saved, blk, 256);
    g_savedValid = 1;
    return 1;
}

#include "../src/rof_hiscore.c"

/* --- the signature the running binary carries, verbatim from rof.xex $7BDA --------------- */
static const char kSignature[] = "XCopyright (c) 1987 Lucasfilm Ltd. v4.1";

static int fails = 0;
static void check(int ok, const char* what)
{
    if (!ok) { printf("FAIL: %s\n", what); fails++; }
}

/* validate_save_state $5D0D, transcribed.  Returns 1 when the 6502 would take the Z branch. */
static int validate_save_state(void)
{
    int y;
    if (mem[0x3700] != 0x28) return 0;                    /* $5D0D-$5D12 */
    if (mem[0x3714] != 0xEE) return 0;                    /* $5D14-$5D19 */
    for (y = 0x26; y >= 1; y--)                           /* $5D1B-$5D26: Y=$26..1, never 0 */
        if (mem[0x7BDA + y] != mem[0x37C7 + y]) return 0;
    return 1;
}

/* game_init_5D50 $5D50 / name_entry_loop $5C54: build each DCB and make the call. */
static void sio_both_sectors(uint8_t cmd)
{
    mem[0x0302] = cmd;  mem[0x0301] = 0x01;
    mem[0x0304] = 0x00; mem[0x0305] = 0x37;               /* $5D74-$5D79: DBUF = $3700 */
    mem[0x030A] = 0xCE; mem[0x030B] = 0x02;               /* $5D7C-$5D83: sector $02CE  */
    rof_sio_block();
    mem[0x0304] = 0x80; mem[0x0305] = 0x37;               /* $5D89-$5D90: DBUF = $3780 */
    mem[0x030A] = 0xCF; mem[0x030B] = 0x02;               /* $5D93-$5D9A: sector $02CF  */
    rof_sio_block();
}

static void reset_mem(void)
{
    memset((void*)mem, 0, sizeof mem);
    for (int s = 0; s < (int)(sizeof kSignature - 1); s++) mem[0x7BDA + s] = (uint8_t)kSignature[s];
}

int main(void)
{
    int i;
    uint8_t after[256];

    /* ---- 1. factory block, boot read: the ORIGINAL gate accepts it -------------------- */
    reset_mem();
    g_savedValid = 0;
    rof_hiscore_init();
    sio_both_sectors(0x52);                                /* 'R' */
    check(mem[0x0303] == 0x01, "boot read leaves DSTATS = success");
    check(validate_save_state(), "factory block passes validate_save_state $5D0D");
    check(mem[0x3700] == 0x28, "$3700 = 'H' of HIGHEST SCORING ACES");
    check(mem[0x3714] == 0xEE, "$3714 = 'N' of the NAME column header");
    check(mem[0x37EE] == 0x10, "level_progress $37EE = $10");
    /* The signature must be the RUNNING binary's, copied — not a hardcoded string. */
    for (i = 0; i < 38; i++)
        if (mem[0x37C8 + i] != mem[0x7BDA + 1 + i]) { check(0, "signature copied from mem[$7BDA+1]"); break; }
    /* Top row: FOX, level 6, 35000 — cell = (byte & $3F) + $20 in ATASCII. */
    check(((mem[0x3728] & 0x3F) + 0x20) == 'F' &&
          ((mem[0x3729] & 0x3F) + 0x20) == 'O' &&
          ((mem[0x372A] & 0x3F) + 0x20) == 'X', "row 0 name reads FOX");
    check(((mem[0x3732] & 0x3F) + 0x20) == '6', "row 0 level digit reads 6");
    check(((mem[0x3737] & 0x3F) + 0x20) == '3' &&
          ((mem[0x3738] & 0x3F) + 0x20) == '5', "row 0 score starts 35");

    /* ---- 2. the game's own write ($5C54, 'W') is lossless and offers the block --------- */
    mem[0x3728] = 0xA1; mem[0x3729] = 0xA3; mem[0x372A] = 0xA5;               /* type three initials into row 0 */
    g_saveCalls = 0; g_saveAccepts = 1;
    sio_both_sectors(0x57);                                /* 'W' */
    check(mem[0x0303] == 0x01, "write leaves DSTATS = success");
    check(g_saveCalls == 2, "each written sector is offered to the platform");
    check(!rof_hiscore_dirty(), "an ACCEPTED save clears the dirty flag");
    for (i = 0; i < 256; i++) after[i] = mem[0x3700 + i];
    check(memcmp(rof_hiscore_block(), after, 256) == 0, "write captures mem[$3700-$37FF] verbatim");

    /* ---- 3. a DECLINED save (the plain Amiga arm) keeps the block for the exit flush --- */
    g_saveAccepts = 0; g_saveCalls = 0;
    sio_both_sectors(0x57);
    check(rof_hiscore_dirty(), "a DECLINED save leaves the block dirty");
    g_saveAccepts = 1;
    rof_hiscore_flush();
    check(!rof_hiscore_dirty(), "rof_hiscore_flush clears it once the platform accepts");
    check(memcmp(g_saved, after, 256) == 0, "the flushed bytes are the ones the game wrote");

    /* ---- 4. next run: the saved block replaces the factory one, and still validates ---- */
    reset_mem();
    rof_hiscore_init();
    sio_both_sectors(0x52);
    check(validate_save_state(), "a reloaded saved block passes the gate");
    check(memcmp((const void*)&mem[0x3700], after, 256) == 0, "the reloaded table is what was saved");
    check(((mem[0x3728] & 0x3F) + 0x20) == 'A' &&
          ((mem[0x3729] & 0x3F) + 0x20) == 'C' &&
          ((mem[0x372A] & 0x3F) + 0x20) == 'E', "the typed initials survived the round trip");

    /* ---- 5. a foreign sector is refused, not aliased onto the table -------------------- */
    reset_mem();
    g_savedValid = 0;
    rof_hiscore_init();
    mem[0x0302] = 0x52; mem[0x0301] = 0x01;
    mem[0x0304] = 0x00; mem[0x0305] = 0x40;
    mem[0x030A] = 0x01; mem[0x030B] = 0x00;                /* sector $0001 — nothing asks for it */
    rof_sio_block();
    check(mem[0x0303] != 0x01, "an unknown sector returns an error status");
    for (i = 0; i < 128; i++)
        if (mem[0x4000 + i] != 0) { check(0, "an unknown sector transfers nothing"); break; }
    /* An unknown COMMAND likewise. */
    mem[0x0302] = 0x21; mem[0x030A] = 0xCE; mem[0x030B] = 0x02;
    rof_sio_block();
    check(mem[0x0303] != 0x01, "an unknown command returns an error status");

    if (fails) { printf("hiscore_block_test: %d FAILED\n", fails); return 1; }
    printf("all checks passed\n");
    return 0;
}
