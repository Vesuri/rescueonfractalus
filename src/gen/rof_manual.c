/* Hand-written implementations for self-modifying / special-case routines.
   These are NOT auto-generated; edit this file freely. */
#include "../cpu/cpu.h"
#include "../cpu/bus.h"
#include "../platform/platform_c.h"
#include "rof_decl.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/* DLI colour-setting subroutines ($4A0C–$4ACD, $6D0E–$6DA1)         */
/* These are called via platform_indirect_jmp from dli_handler_game/  */
/* dli_handler_game2.  On real hardware they tail-call $4A05          */
/* (INC $C7 + RTI); in our port they return normally and the caller   */
/* (dli_handler_game) does the INC.  The last slot ($4ACD) sets       */
/* $C7=0xFF so the caller's INC wraps it to 0, resetting for the next */
/* frame — equivalent to the original JMP $4A07 that skips $4A05.    */
/* ------------------------------------------------------------------ */

/* dli2s0 ($4A0C): write COLPF1 from $D4, then fall through to dli1s0 */
void dli_sub_4a0c(void) {
    bus_write(0xD017, mem[0x00D4]);   /* COLPF1 */
    /* fall through to $4A11 — write COLPF0 from $CF */
    bus_write(0xD409, 0x38);          /* WSYNC (busy-wait until HBLANK) */
    bus_write(0xD016, mem[0x00CF]);   /* COLPF0 */
}

/* dli1s0 ($4A11): WSYNC, then write COLPF0 from $CF */
void dli_sub_4a11(void) {
    bus_write(0xD409, 0x38);          /* WSYNC */
    bus_write(0xD016, mem[0x00CF]);   /* COLPF0 */
}

/* dli1s1 ($4A1F): set player H-positions and cockpit row colours */
void dli_sub_4a1f(void) {
    bus_write(0xD000, 0x39);          /* HPOSP0 */
    bus_write(0xD001, 0xBF);          /* HPOSP1 */
    bus_write(0xD017, mem[0x00DA]);   /* COLPF1 */
    bus_write(0xD016, mem[0x00DD]);   /* COLPF0 */
    bus_write(0xD018, mem[0x00DB]);   /* COLPF2 */
    bus_write(0xD01A, mem[0x00DC]);   /* COLBK  */
}

/* dli1s2 ($4A40): terrain row — players + colours */
void dli_sub_4a40(void) {
    bus_write(0xD00C, 0xC0);          /* P0PL collision clear */
    bus_write(0xD005, 0xBE);          /* HPOSP1 */
    cpu.X = mem[0x00D4];
    bus_write(0xD012, mem[0x00CF]);   /* COLPM0 */
    bus_write(0xD008, 0x01);          /* M0PL */
    bus_write(0xD013, mem[0x00CF]);   /* COLPM1 */
    bus_write(0xD000, 0x30);          /* HPOSP0 */
    bus_write(0xD018, cpu.X);         /* COLPF2 from X=$D4 */
    bus_write(0xD01B, 0x02);          /* PRIOR */
    bus_write(0xD009, 0x01);          /* M1PL */
    bus_write(0xD001, 0xC0);          /* HPOSP1 */
    bus_write(0xD00A, 0x01);          /* M2PL */
}

/* dli1s3 ($4A78): main terrain colours */
void dli_sub_4a78(void) {
    bus_write(0xD014, mem[0x00D0]);   /* COLPM2 */
    bus_write(0xD40A, mem[0x00D4]);   /* WSYNC-like */
    bus_write(0xD016, mem[0x00CF]);   /* COLPF0 */
    bus_write(0xD017, mem[0x00D4]);   /* COLPF1 */
    bus_write(0xD018, mem[0x00D1]);   /* COLPF2 */
    bus_write(0xD01A, mem[0x00D2]);   /* COLBK  */
    bus_write(0xD002, 0x4C);          /* HPOSP2 */
    bus_write(0xD000, 0x5C);          /* HPOSP0 */
    bus_write(0xD012, mem[0x00D5]);   /* COLPM0 */
    bus_write(0xD015, mem[0x00D6]);   /* COLPM3 */
}

/* dli1s4 ($4ACD): final slot — restore COLBK, reset $C7 to 0xFF
   (our INC wraps 0xFF→0, equivalent to original's skip of INC $C7) */
void dli_sub_4acd(void) {
    bus_write(0xD01A, mem[0x00D3]);   /* COLBK  */
    mem[0x00C7] = 0xFF;               /* caller's INC wraps to 0 */
}

/* dli2s1 ($6D0E): sky/overlay row colours */
void dli_sub_6d0e(void) {
    bus_write(0xD40A, 0x2A);          /* WSYNC */
    bus_write(0xD01A, mem[0x00DC]);   /* COLBK  */
    bus_write(0xD018, 0x2A);          /* COLPF2 */
    bus_write(0xD017, 0x28);          /* COLPF1 */
    bus_write(0xD016, 0x24);          /* COLPF0 */
}

/* dli2s2 ($6D28): player positions for cockpit frame */
void dli_sub_6d28(void) {
    bus_write(0xD006, 0x3E);          /* HPOSM2 */
    bus_write(0xD007, 0x3C);          /* HPOSM3 */
    bus_write(0xD004, 0xC2);          /* HPOSM0 */
    bus_write(0xD005, 0xC0);          /* HPOSM1 */
}

/* dli2s3 ($6D42): more player H-positions */
void dli_sub_6d42(void) {
    bus_write(0xD007, 0x3F);          /* HPOSM3 */
    bus_write(0xD004, 0xBF);          /* HPOSM0 */
}

/* dli2s4 ($6D67): landscape colours */
void dli_sub_6d67(void) {
    bus_write(0xD017, 0x06);          /* COLPF1 */
    bus_write(0xD016, 0x04);          /* COLPF0 */
    bus_write(0xD019, 0x26);          /* COLPF3 */
}

/* dli2s5 ($6D99): set PRIOR */
void dli_sub_6d99(void) {
    bus_write(0xD01B, 0x04);          /* PRIOR */
}

/* dli2s6 ($6DA1): final cockpit colours — reset $C7 to 0xFF */
void dli_sub_6da1(void) {
    bus_write(0xD001, 0x94);          /* HPOSP1 */
    bus_write(0xD013, mem[0x00DE]);   /* COLPM1 */
    bus_write(0xD018, 0x2C);          /* COLPF2 */
    bus_write(0xD01A, 0x90);          /* COLBK  */
    mem[0x00C7] = 0xFF;               /* caller's INC wraps to 0 */
}

/* ------------------------------------------------------------------ */
/* DLI dispatch helpers — $49EE (terrain) and $6CC2/$6CAD (cockpit)   */
/* Each fires the sub-handler via platform_indirect_jmp then increments*/
/* $C7 (the per-scanline slot counter).  In the 6502, sub-handlers    */
/* tail-call $4A05 which does INC $C7; RTI; here that increment is    */
/* done in the dispatcher so the manual sub-handler bodies just return. */
/* ------------------------------------------------------------------ */

/* Shared helper for interleaved tables: lo_table and hi_table are adjacent
   bytes in a lo0,hi0,lo1,hi1,... array.  Cockpit and game2 DLI tables use
   this layout ($6DBB/$6DBC, $6DCF/$6DD0).  Stride = 2. */
static void dli_dispatch(uint16_t lo_table, uint16_t hi_table) {
    uint8_t idx = mem[0x00C7];
    uint8_t lo  = mem[lo_table + (uint16_t)idx * 2];
    uint8_t hi  = mem[hi_table + (uint16_t)idx * 2];
    uint16_t target = (uint16_t)lo | ((uint16_t)hi << 8);
    platform_indirect_jmp(target);
    mem[0x00C7]++;   /* INC $C7 — was JMP $4A05 in 6502 */
}

/* dli_handler_game ($49EE): terrain-area DLI, 5 slots max.
   Tables $4AD9 (lo bytes) and $4ADE (hi bytes) are two SEPARATE 5-byte
   arrays — stride 1, unlike the interleaved cockpit tables.              */
void dli_handler_game(void) {
    uint8_t idx = mem[0x00C7];
    if (idx < 5) {
        uint8_t lo = mem[0x4AD9 + idx];   /* stride 1 in separate lo array */
        uint8_t hi = mem[0x4ADE + idx];   /* stride 1 in separate hi array */
        platform_indirect_jmp((uint16_t)lo | ((uint16_t)hi << 8));
        mem[0x00C7]++;
    }
    /* when idx >= 5: no dispatch, $C7 not incremented — handler chain ends */
}

/* dli_handler_game2 ($6CC2): cockpit DLI (layout 1), uses table $6DCF/$6DD0. */
void dli_handler_game2(void) {
    dli_dispatch(0x6DCF, 0x6DD0);
}

/* dli_handler_cockpit ($6CAD): cockpit DLI (layout 2), uses table $6DBB/$6DBC.
   Installed as VDSLST = $6CAD by display_setup; registered so indirectJmp
   can dispatch to it from the renderer.                                      */
void dli_handler_cockpit(void) {
    dli_dispatch(0x6DBB, 0x6DBC);
}

/* ------------------------------------------------------------------ */
/* Additional DLI sub-handlers used only by dli_handler_cockpit        */
/* ------------------------------------------------------------------ */

/* dli_sub_6cd7: set player colours + PRIOR=$94 */
void dli_sub_6cd7(void) {
    bus_write(0xD014, mem[0x08D8]);  /* COLPM2 */
    bus_write(0xD013, mem[0x08D7]);  /* COLPM1 */
    bus_write(0xD015, mem[0x08D9]);  /* COLPM3 */
    bus_write(0xD01B, 0x94);         /* PRIOR  */
}

/* dli_sub_6cf1: WSYNC, then sky row colours */
void dli_sub_6cf1(void) {
    bus_write(0xD40A, 0x00);         /* WSYNC  */
    bus_write(0xD01A, mem[0x0071]);  /* COLBK  */
    bus_write(0xD016, mem[0x08D4]);  /* COLPF0 */
    bus_write(0xD017, mem[0x08D5]);  /* COLPF1 */
    bus_write(0xD018, mem[0x08D6]);  /* COLPF2 */
}

/* dli_sub_6d4f: fixed sky/land colours */
void dli_sub_6d4f(void) {
    bus_write(0xD017, 0x06);         /* COLPF1 */
    bus_write(0xD016, 0x04);         /* COLPF0 */
    bus_write(0xD018, 0x2C);         /* COLPF2 */
    bus_write(0xD015, mem[0x08D8]);  /* COLPM3 */
}

/* dli_sub_6d7c: WSYNC, reset player/playfield colours for panel */
void dli_sub_6d7c(void) {
    bus_write(0xD40A, 0x04);         /* WSYNC  */
    bus_write(0xD01B, 0x04);         /* PRIOR  */
    bus_write(0xD01A, 0x00);         /* COLBK  */
    bus_write(0xD012, mem[0x0071]);  /* COLPM0 */
    bus_write(0xD013, mem[0x0071]);  /* COLPM1 */
    bus_write(0xD019, 0x26);         /* COLPF3 */
}

/* screen_page_swap ($1A62): swaps 5 x 256-byte pages between $40xx and $06xx.
   The original code is self-modifying: it patches the high bytes of its own
   LDA/STA instructions to cycle through pages $40–$44 and $06–$0A.
   The semantics are straightforward so we translate the intent directly. */
void screen_page_swap(void) {
    int page;
    for (page = 0; page < 5; page++) {
        uint8_t *a = (uint8_t*)mem + ((0x40 + page) << 8);
        uint8_t *b = (uint8_t*)mem + ((0x06 + page) << 8);
        uint8_t tmp[256];
        memcpy(tmp, a,   256);
        memcpy(a,   b,   256);
        memcpy(b,   tmp, 256);
    }
}
