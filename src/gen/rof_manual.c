/* Hand-written implementations for self-modifying / special-case routines.
   These are NOT auto-generated; edit this file freely. */
#include "../cpu/cpu.h"
#include "../cpu/bus.h"
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
