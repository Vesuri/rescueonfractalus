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

/* terrain_gen_3 @ $AD5F — clear one terrain column band + its object-table cells.
 *
 * Inputs : cpu.X = starting column offset into the terrain/object buffers.
 * Effect : zeroes 44 terrain rows (base $1010, stride $60) across 42 columns
 *          starting at X (the X index runs as a 6502 byte, so it WRAPS at $FF —
 *          replicated here for bit-identity), then zeroes a scattered set of
 *          object-table cells indexed by the ORIGINAL X (restored from $0094).
 * Outputs: mem[$0094] = original X; the 42x44 terrain band and the listed object
 *          cells set to 0.
 * Contract: memory only.  Both call sites (flight loop $3EBF / $3EFA) reload X
 *          and A immediately after the call, so the 6502 exit register/flag state
 *          is dead.  We still reproduce it (A=0, Y=0, X=original, N/Z per LDX X)
 *          so the validation harness shows zero incidental CPU drift.
 */
void terrain_gen_3(void) {
    uint8_t x0 = mem[0x0094] = cpu.X;            /* $AD5F: STX $0094 (save column) */

    /* $AD61-$ADEF: 42 columns ($2A) x 44 rows (base $1010, stride $60), all 0. */
    for (uint8_t i = 0; i < 0x2A; i++) {
        uint8_t x = (uint8_t)(x0 + i);           /* INX wraps as a byte */
        for (uint16_t row = 0x1010; row <= 0x2030; row += 0x60)
            mem[row + x] = 0x00;
    }

    /* $ADF0-$AE52: scattered object-table cells, indexed by the ORIGINAL X. */
    static const uint16_t cells[] = {
        0x2090, 0x2091, 0x2092, 0x2093, 0x2094,
        0x20BA, 0x20B9, 0x20B8, 0x20B7, 0x20B6, 0x20B5,
        0x20F0, 0x20F1, 0x20F2, 0x20F3,
        0x211A, 0x2119, 0x2118, 0x2117, 0x2116,
        0x2150, 0x2151, 0x2152,
        0x217A, 0x2179, 0x2178, 0x2177,
        0x21B0, 0x21DA, 0x21B1, 0x21D9, 0x21D8,
    };
    for (unsigned k = 0; k < sizeof cells / sizeof cells[0]; k++)
        mem[cells[k] + x0] = 0x00;

    /* Incidental exit state (dead at all call sites; matched for a clean diff). */
    cpu.A = 0x00; cpu.Y = 0x00; cpu.X = x0;
    cpu.Z = (x0 == 0) ? 1 : 0; cpu.N = (x0 >> 7) & 1;
}

/* signed_mul_8x16 @ $9C97 — fixed-point signed multiply.
 *
 * Inputs : cpu.A      = 8-bit multiplier (treated as an unsigned fraction),
 *          $00AA/$00AB = 16-bit signed multiplicand (lo/hi).
 *          cpu.C      = entry carry (see note below).
 * Outputs: $00A8/$00A9 = signed 16-bit product (lo/hi), $00AC = fractional byte,
 *          $00AA/$00AB = |multiplicand| (negated in place when input was < 0),
 *          $00AD = original sign byte (= input $00AB).
 *
 * Algorithm (faithful to the 6502): take |multiplicand| via $00AB's sign, then an
 * 8-step shift-add where the multiplier byte lives in $00AC and is rotated out one
 * bit per step into a 24-bit {A9:A8:AC} register; re-apply the sign at the end.
 *
 * ENTRY CARRY: the first `ROR $AC` ($9CB2) rotates the caller's carry into bit7 of
 * $00AC.  That injected bit is shifted back out across the 8 rotations and never
 * reaches an add decision, so it does NOT affect $00A8/$00A9 — but it does land in
 * the (functionally dead) final $00AC, so we thread it to stay byte-identical.
 *
 * Contract: memory only.  All four call sites (a0ad/a0c4/a0e2/a0f9) reload A and
 * the flags immediately after the call, so the 6502 exit register state is dead.
 */
void signed_mul_8x16(void) {
    uint8_t c = cpu.C;                    /* entry carry feeds the first ROR $AC */

    /* 6502-faithful, carry-threaded primitives over the local carry `c`. */
    #define M_ADC(acc, v) do { uint16_t _t = (uint16_t)(acc) + (uint8_t)(v) + c; \
                               c = (_t > 0xFF) ? 1 : 0; (acc) = (uint8_t)_t; } while (0)
    #define M_SBC(acc, v) M_ADC(acc, (uint8_t)~(uint8_t)(v))
    #define M_RORM(a)     do { uint8_t _v = mem[a], _nc = _v & 1; \
                               mem[a] = (uint8_t)((_v >> 1) | (c << 7)); c = _nc; } while (0)
    #define M_LSRM(a)     do { uint8_t _v = mem[a]; c = _v & 1; mem[a] = (uint8_t)(_v >> 1); } while (0)

    mem[0x00AC] = cpu.A;                  /* 9C97: multiplier -> shift register   */
    mem[0x00A9] = 0x00;                   /* 9C9B: clear 16-bit accumulator       */
    mem[0x00A8] = 0x00;                   /* 9C9D                                  */
    mem[0x00AD] = mem[0x00AB];            /* 9C9F/9CA1: save sign byte (= hi)      */

    if (mem[0x00AB] & 0x80) {             /* 9CA3 BMI: negate multiplicand -> |x|  */
        uint8_t acc;
        c = 1;                            /* 9CA5 SEC                              */
        acc = 0x00; M_SBC(acc, mem[0x00AA]); mem[0x00AA] = acc;   /* 9CA6-9CAA */
        acc = 0x00; M_SBC(acc, mem[0x00AB]); mem[0x00AB] = acc;   /* 9CAC-9CB0 */
    }

    /* 9CB2: first step LOADS the accumulator (it is still zero) instead of adding. */
    M_RORM(0x00AC);
    if (c) { mem[0x00A8] = mem[0x00AA]; mem[0x00A9] = mem[0x00AB]; }

    /* 9CBE..9D3C: 7 shift-add steps (multiplier bits 1..7). */
    for (int step = 0; step < 7; step++) {
        M_LSRM(0x00A9); M_RORM(0x00A8); M_RORM(0x00AC);
        if (c) {
            uint8_t acc;
            c = 0;                                                    /* CLC */
            acc = mem[0x00A8]; M_ADC(acc, mem[0x00AA]); mem[0x00A8] = acc;
            acc = mem[0x00A9]; M_ADC(acc, mem[0x00AB]); mem[0x00A9] = acc;
        }
    }

    /* 9D51: final shift, then re-apply the original sign. */
    M_LSRM(0x00A9); M_RORM(0x00A8);
    if (mem[0x00AD] & 0x80) {             /* 9D55 BIT / 9D57 BMI */
        uint8_t acc;
        c = 1;                            /* SEC */
        acc = 0x00; M_SBC(acc, mem[0x00A8]); mem[0x00A8] = acc;
        acc = 0x00; M_SBC(acc, mem[0x00A9]); mem[0x00A9] = acc;
    }

    #undef M_ADC
    #undef M_SBC
    #undef M_RORM
    #undef M_LSRM
}
