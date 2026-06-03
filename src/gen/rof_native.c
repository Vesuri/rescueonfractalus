/* Native reimplementations of selected 6502 routines.
 *
 * Each function here REPLACES the transliterated version: its address is listed
 * in transpile.py's VALIDATE_FUNCS, so the transpiler emits the faithful
 * transliteration under a `__t6502` suffix and leaves the plain name to us.
 * All call sites invoke the plain name, so these are live in the real build.
 *
 * The contract is the OBSERVABLE MACHINE STATE at the call boundary — primarily
 * mem[] (zero-page + RAM). tools/validate_native.c proves each function here is
 * equivalent to its `__t6502` twin across a large random input corpus before it
 * ships. This is the regen-safe seam for the eventual Amiga reimplementation:
 * everything not listed in VALIDATE_FUNCS stays transliterated and regenerable.
 */
#include <stdint.h>
#include "../cpu/cpu.h"

/* divide_16x16 @ $9D6F — restoring 16-bit divide.
 *
 * Inputs : dividend $00B0(lo)/$00B1(hi), divisor $00AE(lo)/$00AF(hi),
 *          quotient accumulator $00B2 (its 8 bits are fully shifted out).
 * Outputs: remainder -> $00B0/$00B1, quotient -> $00B2,
 *          divisor left shifted by the normalization count -> $00AE/$00AF.
 * Domain : divisor in [0x0001, 0x7FFF] (the normalization shifts the divisor's
 *          top set bit to bit14; divisor 0 or >=0x8000 would not terminate —
 *          the original game never calls it outside this range).
 *
 * This mirrors the 6502 shift-subtract structure in clean C so the memory
 * effects are bit-identical to the transliteration; the 6502 register/flag
 * exit state is intentionally NOT reproduced because both call sites save Y
 * through $009F and overwrite A/flags immediately (verified — incidental/dead).
 */
void divide_16x16(void) {
    uint8_t b0 = mem[0x00B0], b1 = mem[0x00B1];   /* dividend lo / hi */
    uint8_t ae = mem[0x00AE], af = mem[0x00AF];   /* divisor  lo / hi */
    uint8_t b2 = mem[0x00B2];                     /* quotient accumulator */

    /* Normalize: shift dividend and divisor left until bit14 (bit6 of the
       high byte) of the divisor is set — matches BIT $AF / loop. */
    while (!(af & 0x40)) {
        b1 = (uint8_t)((b1 << 1) | (b0 >> 7)); b0 = (uint8_t)(b0 << 1);
        af = (uint8_t)((af << 1) | (ae >> 7)); ae = (uint8_t)(ae << 1);
    }

    for (int i = 0; i < 8; i++) {
        /* dividend <<= 1; carry-out (old bit15) is the quotient bit used when
           the subtract is skipped — same as the carry left by ROL $B1. */
        uint8_t c = b1 >> 7;
        b1 = (uint8_t)((b1 << 1) | (b0 >> 7));
        b0 = (uint8_t)(b0 << 1);

        /* Attempt the subtract only when bit15 or bit14 of the dividend is set
           (the BIT $B1 / branch optimization — divisor's top bit is bit14). */
        if (b1 & 0xC0) {
            uint8_t borrow = (b0 < ae) ? 1 : 0;
            uint8_t nb0 = (uint8_t)(b0 - ae);
            uint8_t nb1 = (uint8_t)(b1 - af - borrow);
            c = (b1 >= (uint8_t)(af + borrow)) ? 1 : 0;   /* no borrow -> C=1 */
            if (c) { b0 = nb0; b1 = nb1; }                /* restoring: keep iff success */
        }

        b2 = (uint8_t)((b2 << 1) | c);                    /* ROL quotient, bit0 = C */
    }

    mem[0x00B0] = b0; mem[0x00B1] = b1; mem[0x00B2] = b2;
    mem[0x00AE] = ae; mem[0x00AF] = af;
}
