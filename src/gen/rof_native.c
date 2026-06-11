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
#include "../cpu/bus.h"  /* bus_read/bus_write + ZP_IND_Y for indirect bitmap access */
#include "rof_decl.h"   /* declarations for transpiled routines native code calls */

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

/* sine_table_lookup @ $9C55 — quarter-wave sine/cosine table lookup.
 *
 * Inputs : $0075 = angle (0..255 = full circle).  Tables in mem[]:
 *          $9B98[quad] = sign flag (0 = positive, else negate the result),
 *          $9B9C[quad] = index-reflect mask (EOR) for the descending quadrants,
 *          $4EB9[idx]  = result hi byte, $4EFA[idx] = result lo byte.
 * Outputs: $280E = quadrant (angle >> 6), $0076/$0077/$0078 = signed 24-bit value.
 *
 * The angle's top 2 bits select the quadrant; the low 6 bits index a 64-entry
 * quarter-wave table, reflected (idx ^ mask) for the two descending quadrants.
 * Negative quadrants two's-complement the 24-bit {0078:0077:0076} value.
 *
 * Contract: memory only.  Its only callers (inside trig_interp_lookup) reload
 * A/flags from $0076.. immediately after, so the 6502 exit registers are dead.
 */
void sine_table_lookup(void) {
    uint8_t angle = mem[0x0075];
    uint8_t quad  = (uint8_t)(angle >> 6);            /* ASL;ROL x2 -> top 2 bits */
    uint8_t idx   = (uint8_t)(angle & 0x3F);          /* ASLx2;LSRx2 -> low 6 bits */
    mem[0x280E] = quad;

    uint8_t y    = (uint8_t)(idx ^ mem[0x9B9C + quad]);   /* reflect per quadrant */
    uint8_t sign = mem[0x9B98 + quad];

    if (sign == 0) {                                  /* 9C70 BNE not taken: positive */
        mem[0x0078] = 0x00;
        mem[0x0077] = mem[0x4EB9 + y];
        mem[0x0076] = mem[0x4EFA + y];
    } else {                                          /* negate 24-bit (SEC; 0-x...) */
        uint8_t c = 1, acc;
        #define N_SBC(v) do { uint16_t _t = (uint16_t)acc + (uint8_t)~(uint8_t)(v) + c; \
                              c = (_t > 0xFF) ? 1 : 0; acc = (uint8_t)_t; } while (0)
        acc = 0x00; N_SBC(mem[0x4EFA + y]); mem[0x0076] = acc;
        acc = 0x00; N_SBC(mem[0x4EB9 + y]); mem[0x0077] = acc;
        acc = 0x00; N_SBC(0x00);            mem[0x0078] = acc;
        #undef N_SBC
    }
}

/* trig_interp_lookup @ $9BDB — interpolate the sine table between angle & angle+1.
 *
 * Inputs : $0075 = angle, $280D = 3-bit octant fraction.
 * Outputs: $0076/$0077/$0078 = interpolated signed 24-bit value.  Scratch:
 *          $2813-$2815 (angle sample, doubled each step), $2816-$2818 (angle+1
 *          sample, doubled each step), $280E/$280F.
 *
 * Looks up sine at angle+1 ($2816-) and at angle ($2813-, also the running
 * accumulator $0076-), then blends over 3 fraction bits: each step adds whichever
 * sample the next bit selects, then doubles both samples (24-bit <<1).
 *
 * Contract: memory only (both callers reload A/flags from $0077 after the call).
 * Calls the native sine_table_lookup (above), itself validated byte-identical.
 */
void trig_interp_lookup(void) {
    mem[0x0075]++;                       /* INC $0075 — sample angle+1 */
    sine_table_lookup();
    mem[0x2816] = mem[0x0076];
    mem[0x2817] = mem[0x0077];
    mem[0x2818] = mem[0x0078];

    mem[0x0075]--;                       /* DEC $0075 — sample angle (the base) */
    sine_table_lookup();
    mem[0x2813] = mem[0x0076];
    mem[0x2814] = mem[0x0077];
    mem[0x2815] = mem[0x0078];

    mem[0x280F] = mem[0x280D];           /* octant fraction bits, consumed lo->hi */

    for (int step = 3; step > 0; step--) {
        uint8_t bit = mem[0x280F] & 1;   /* LSR $280F -> carry = fraction bit */
        mem[0x280F] >>= 1;
        uint16_t src = bit ? 0x2816 : 0x2813;   /* select angle+1 or angle sample */

        /* CLC; 24-bit ADC chain: $0076-$0078 += sample. */
        uint8_t c = 0;
        #define T_ADC(dst, v) do { uint16_t _t = (uint16_t)mem[dst] + (uint8_t)(v) + c; \
                                   c = (_t > 0xFF) ? 1 : 0; mem[dst] = (uint8_t)_t; } while (0)
        T_ADC(0x0076, mem[src + 0]);
        T_ADC(0x0077, mem[src + 1]);
        T_ADC(0x0078, mem[src + 2]);
        #undef T_ADC

        /* Double both samples: ASL lo; ROL mid; ROL hi (24-bit <<1). */
        #define T_SHL(lo) do { uint8_t _c = mem[lo] >> 7; \
            mem[lo] = (uint8_t)(mem[lo] << 1); \
            { uint8_t _n = mem[(lo)+1] >> 7; mem[(lo)+1] = (uint8_t)((mem[(lo)+1] << 1) | _c); _c = _n; } \
            mem[(lo)+2] = (uint8_t)((mem[(lo)+2] << 1) | _c); } while (0)
        T_SHL(0x2813);
        T_SHL(0x2816);
        #undef T_SHL
    }
}

/* compute_row_xspans @ $AD2B — per-row horizontal span endpoints.
 *
 * Inputs : $00A6 = centre seed, $00A4/$00A5 = 16-bit step (int/frac).
 * Outputs: $271E = seed; $271F..$272D (15 entries) = seed + cumulative +step
 *          going up; $270E..$271D (16 entries) = seed - cumulative step going
 *          down.  $00B5 = fractional accumulator (scratch).
 *
 * Two fixed-point ramps from the seed: a 16-bit {A4:A5} step is accumulated in
 * {value:B5}, the integer byte stored per row.  Contract: memory only (the
 * caller reloads A immediately; the exit X=$FF is dead).
 */
void compute_row_xspans(void) {
    mem[0x271E] = mem[0x00A6];                 /* AD2B: centre seed */

    uint8_t b5 = 0x00; mem[0x00B5] = 0x00;     /* AD30-AD32 */
    for (uint8_t x = 0; x < 0x0F; x++) {       /* AD34: upward, X=0..0x0E */
        uint16_t t = (uint16_t)b5 + mem[0x00A5];          /* CLC; B5 += A5 */
        uint8_t c = (t > 0xFF) ? 1 : 0; b5 = (uint8_t)t; mem[0x00B5] = b5;
        t = (uint16_t)mem[0x271E + x] + mem[0x00A4] + c;  /* (271E+X) + A4 + carry */
        mem[0x271F + x] = (uint8_t)t;
    }

    b5 = 0x00; mem[0x00B5] = 0x00;             /* AD48-AD4A */
    for (int xi = 0x0F; xi >= 0; xi--) {       /* AD4C: downward, X=0x0F..0 */
        uint8_t x = (uint8_t)xi;
        uint16_t t = (uint16_t)b5 + (uint8_t)~mem[0x00A5] + 1;   /* SEC; B5 -= A5 */
        uint8_t c = (t > 0xFF) ? 1 : 0; b5 = (uint8_t)t; mem[0x00B5] = b5;
        t = (uint16_t)mem[0x270F + x] + (uint8_t)~mem[0x00A4] + c;  /* (270F+X) - A4 - borrow */
        mem[0x270E + x] = (uint8_t)t;
    }
}

/* check_target_in_window @ $AC42 — latch a target index after 2 consecutive hits.
 *
 * Gated on $0036==0 and $004A!=0.  When $0063 is negative it range-tests the
 * screen coords $2912/$2913 (a wrap-around [0x0C,0xF5) band); otherwise it tests
 * world coords $0064 in [0x48,0x98) and $0066 in [0x24,0x60).  A passing frame
 * increments the hit counter $2837; the second consecutive hit latches the index
 * ($2910 in the negative branch, else 1) into $2838 and resets the counter.  Any
 * failing frame resets $2837.  Contract: memory only (caller reloads A).
 */
void check_target_in_window(void) {
    if (mem[0x0036] != 0) return;              /* AC42 BNE end */
    if (mem[0x004A] == 0) return;              /* AC46 BEQ end */

    uint8_t x = 0;
    int latch = 0, reset = 0;

    if (mem[0x0063] & 0x80) {                  /* AC4A BPL -> positive; here negative */
        uint8_t a = mem[0x2912];
        if (a >= 0x0C && a < 0xF5) reset = 1;
        else {
            a = mem[0x2913];
            if (a >= 0x0C && a < 0xF5) reset = 1;
            else { x = mem[0x2910]; latch = 1; }       /* AC64 */
        }
    } else {                                   /* AC6A: $0063 >= 0 */
        uint8_t a = mem[0x0064];
        if (a < 0x48 || a >= 0x98) reset = 1;
        else {
            a = mem[0x0066];
            if (a < 0x24 || a >= 0x60) reset = 1;
            else { x = 0x01; latch = 1; }              /* AC7E */
        }
    }

    if (latch) {                               /* AC80 */
        uint8_t cnt = (uint8_t)(mem[0x2837] + 1); mem[0x2837] = cnt;
        if (cnt < 0x02) return;                /* AC88 BCC end (counter kept) */
        mem[0x2838] = x;                       /* AC8A latch, then fall through to reset */
        reset = 1;
    }
    if (reset) mem[0x2837] = 0x00;             /* AC8D */
}

/* obj_table_set_active @ $4E58 — activate the first eligible object slot.
 *
 * Scans all 256 entries of the object-flag table $0A00 in steps of $43 (which,
 * being coprime to 256, visits every index exactly once).  The first entry that
 * equals 1 AND whose index does not appear in the active-index table $2276..$22A2
 * (45 entries) is promoted to $80 and the scan returns.  $281F holds the current
 * index (scratch).  Contract: memory only (caller reloads A; exit X dead).
 */
void obj_table_set_active(void) {
    uint8_t idx = 0x00;
    do {
        mem[0x281F] = idx;                     /* 4E5A */
        if (mem[0x0A00 + idx] == 0x01) {       /* 4E5D-4E62 */
            int found = 0;
            for (int xi = 0x2C; xi >= 0; xi--) /* 4E66: search active table */
                if (mem[0x2276 + xi] == mem[0x281F]) { found = 1; break; }
            if (!found) { mem[0x0A00 + idx] = 0x80; return; }   /* 4E71-4E79 */
        }
        idx = (uint8_t)(idx + 0x43);           /* 4E7A: CLC; ADC #$43; TAX */
    } while (idx != 0x00);                     /* 4E81 BNE */
}

/* ring_push_0719 @ $55FF — push A into the $0719 ring buffer; restore caller's X.
 *
 * The tail of game_sub_55FC: store A at $0719+head, decrement the head modulo
 * $20 (wrapping $00->$1F, clamping a head >= $20 to $1F first), then PULL the X
 * that game_sub_55FC saved off the 6502 stack (PLA; TAX) before its RTS.
 *
 * Inputs : cpu.A = byte to push, $0073 = ring head, and the 6502 stack
 *          (cpu.S + mem[$0100+S]) holding the saved X.
 * Outputs: mem[$0719+head], $0073 = new head; cpu.A = cpu.X = pulled value;
 *          cpu.S incremented.  UNLIKE the other leaves the CPU state IS part of
 *          the contract here — the pulled X is handed back to game_sub_55FC's
 *          caller — so the harness checks cpu.A/X/S, not just mem[].
 */
void ring_push_0719(void) {
    uint8_t x = mem[0x0073];                   /* 55FF LDX $0073 */
    if (x >= 0x20) x = 0x1F;                   /* 5601 CPX #$20; BCC; LDX #$1F  */
    mem[0x0719 + x] = cpu.A;                    /* 5607 STA $0719,X */
    x = (uint8_t)(x - 1);                       /* 560A DEX */
    if (x & 0x80) x = 0x1F;                     /* 560B BPL; LDX #$1F (wrap $FF) */
    mem[0x0073] = x;                            /* 560F STX $0073 */

    cpu.S++; cpu.A = mem[0x0100 | cpu.S];       /* 5611 PLA */
    cpu.X = cpu.A;                              /* 5612 TAX */
    cpu.N = (cpu.A >> 7) & 1; cpu.Z = (cpu.A == 0) ? 1 : 0;
}

/* compute_heading_sincos @ $9BA0 — sin/cos of the 16-bit heading $2885/$2886.
 *
 * Forms an 8-bit angle in $0075 = ($2886 << 2) | ($2885 >> 6) and the octant
 * fraction $280D = ($2885 >> 3) & 7, then calls trig_interp_lookup at the angle
 * (sin -> $2809/$280A) and at angle+$40 = +90deg (cos -> $280B/$280C).
 * Contract: memory only (callers reload A).  Calls native trig_interp_lookup.
 */
void compute_heading_sincos(void) {
    uint8_t a75 = mem[0x2886], h = mem[0x2885], c;
    c = h >> 7; h = (uint8_t)(h << 1); a75 = (uint8_t)((a75 << 1) | c);   /* ASL;ROL $75 */
    c = h >> 7;                          a75 = (uint8_t)((a75 << 1) | c); /* ASL;ROL $75 */
    mem[0x0075] = a75;
    mem[0x280D] = (uint8_t)((mem[0x2885] >> 3) & 0x07);

    trig_interp_lookup();                /* sin */
    mem[0x2809] = mem[0x0077];
    mem[0x280A] = mem[0x0078];

    mem[0x0075] = (uint8_t)(mem[0x0075] + 0x40);   /* +90 deg */
    trig_interp_lookup();                /* cos */
    mem[0x280B] = mem[0x0077];
    mem[0x280C] = mem[0x0078];
}

/* build_view_transform_matrix @ $A0A3 — two rotated view-space components.
 *
 * With sin/cos in $0089/$0087 and a 16-bit vector in $00A0:$00A1 / $00A2:$00A3,
 * computes (via four signed_mul_8x16 calls and 16-bit add/sub/negate):
 *   $22A3:$22D1 = [A1:A0]*$89 - [A3:A2]*$87
 *   $22FF:$232D = -([A3:A2]*$89 + [A1:A0]*$87)
 *
 * CARRY THREADING: native signed_mul_8x16 reads the entry carry (-> its $AC byte)
 * and does NOT modify cpu.C.  The 6502 here leaves carry untouched before the
 * first two muls (so they see build_view's entry carry) and feeds the step-2
 * subtract's carry into the last two muls.  We mirror that exactly by writing
 * cpu.C after each add/sub block.  Contract: memory (caller reloads A; final
 * carry is incidental).  Calls native signed_mul_8x16.
 */
void build_view_transform_matrix(void) {
    mem[0x00AA] = mem[0x00A0]; mem[0x00AB] = mem[0x00A1];
    cpu.A = mem[0x0089]; signed_mul_8x16();
    mem[0x22A3] = mem[0x00A8]; mem[0x22D1] = mem[0x00A9];

    mem[0x00AA] = mem[0x00A2]; mem[0x00AB] = mem[0x00A3];
    cpu.A = mem[0x0087]; signed_mul_8x16();
    {   /* SEC; $22A3:$22D1 -= $A8:$A9 */
        uint8_t c = 1; uint16_t t;
        t = (uint16_t)mem[0x22A3] + (uint8_t)~mem[0x00A8] + c; c = t >> 8; mem[0x22A3] = (uint8_t)t;
        t = (uint16_t)mem[0x22D1] + (uint8_t)~mem[0x00A9] + c; c = t >> 8; mem[0x22D1] = (uint8_t)t;
        cpu.C = c & 1;                   /* feeds the next signed_mul's $AC byte */
    }

    mem[0x00AA] = mem[0x00A2]; mem[0x00AB] = mem[0x00A3];
    cpu.A = mem[0x0089]; signed_mul_8x16();
    mem[0x22FF] = mem[0x00A8]; mem[0x232D] = mem[0x00A9];

    mem[0x00AA] = mem[0x00A0]; mem[0x00AB] = mem[0x00A1];
    cpu.A = mem[0x0087]; signed_mul_8x16();
    {   /* CLC; $22FF:$232D += $A8:$A9 */
        uint8_t c = 0; uint16_t t;
        t = (uint16_t)mem[0x00A8] + mem[0x22FF] + c; c = t >> 8; mem[0x22FF] = (uint8_t)t;
        t = (uint16_t)mem[0x00A9] + mem[0x232D] + c; c = t >> 8; mem[0x232D] = (uint8_t)t;
        cpu.C = c & 1;
    }
    {   /* SEC; $22FF:$232D = 0 - $22FF:$232D (16-bit negate) */
        uint8_t c = 1; uint16_t t;
        t = (uint16_t)0 + (uint8_t)~mem[0x22FF] + c; c = t >> 8; mem[0x22FF] = (uint8_t)t;
        t = (uint16_t)0 + (uint8_t)~mem[0x232D] + c; c = t >> 8; mem[0x232D] = (uint8_t)t;
        cpu.C = c & 1;
    }
}

/* setup_projection_params @ $AC93 — per-frame projection/view setup.
 *
 * Builds {$0088:$0087}={$2888:$2887}>>4 and {$008A:$0089}={$288A:$2889}>>4
 * (16-bit logical >>4); sign-extends/<<2 the pitch delta {$0034:$0033} into
 * {$008C:$008B} (clamped to $FF when $0034>=$40); forms $0092=$2886<<2; calls
 * compute_heading_sincos and loads the sin/cos view vector into $00A0-$00A3;
 * derives the row step {$00A4:$00A5} = signed({$0024:$0023})>>1, the span seed
 * $00A6 = 6 - ({$0028:$0029}<<2 hi), and clamps an altitude index to $2822 in 0..8.
 *
 * Faithful transliteration: carry is threaded through a local `c` (the result
 * bytes $0092/$00A6 depend on it).  The PHA;PLA pair is replayed against the
 * 6502 stack (it leaves the pushed $0024 byte at mem[$0100+S]) for byte-identity.
 * Native compute_heading_sincos leaves carry untouched (matches the 6502 here).
 * Contract: memory only (the sole caller, terrain_gen_1, reloads A via build_view).
 */
void setup_projection_params(void) {
    uint8_t A, c = cpu.C;
    #define LSRA()  do { c = A & 1; A = (uint8_t)(A >> 1); } while (0)
    #define RORM(a) do { uint8_t v = mem[a], nc = v & 1; mem[a] = (uint8_t)((v >> 1) | (c << 7)); c = nc; } while (0)
    #define ASLM(a) do { uint8_t v = mem[a]; c = v >> 7; mem[a] = (uint8_t)(v << 1); } while (0)
    #define ROLA()  do { uint8_t nc = A >> 7; A = (uint8_t)((A << 1) | c); c = nc; } while (0)
    #define ROLM(a) do { uint8_t v = mem[a], nc = v >> 7; mem[a] = (uint8_t)((v << 1) | c); c = nc; } while (0)
    #define RORA()  do { uint8_t nc = A & 1; A = (uint8_t)((A >> 1) | (c << 7)); c = nc; } while (0)

    A = mem[0x2887]; mem[0x0087] = A;            /* {0088:0087} = {2888:2887} >> 4 */
    A = mem[0x2888];
    LSRA(); RORM(0x0087); LSRA(); RORM(0x0087);
    LSRA(); RORM(0x0087); LSRA(); RORM(0x0087);
    mem[0x0088] = A;

    A = mem[0x2889]; mem[0x0089] = A;            /* {008A:0089} = {288A:2889} >> 4 */
    A = mem[0x288A];
    LSRA(); RORM(0x0089); LSRA(); RORM(0x0089);
    LSRA(); RORM(0x0089); LSRA(); RORM(0x0089);
    mem[0x008A] = A;

    mem[0x008B] = mem[0x0033];                   /* {008C:008B} = sign/<<2 of {0034:0033} */
    A = mem[0x0034];
    c = (A >= 0x40) ? 1 : 0;                     /* CMP #$40 */
    if (A >= 0x40) { mem[0x008C] = 0xFF; }       /* >=$40: clamp hi to $FF (negative) */
    else { ASLM(0x008B); ROLA(); ROLM(0x008B); ROLA(); mem[0x008C] = A; }

    A = mem[0x2886]; ROLA(); ROLA();             /* $0092 = $2886 <<2 (carry-threaded) */
    mem[0x0092] = A;

    compute_heading_sincos();
    mem[0x00A0] = mem[0x2809]; mem[0x00A1] = mem[0x280A];   /* sin/cos view vector */
    mem[0x00A2] = mem[0x280B]; mem[0x00A3] = mem[0x280C];

    mem[0x00A6] = mem[0x0029];                   /* $00A6 = 6 - ({0028:0029}<<2 hi) */
    A = mem[0x0028]; ROLA(); ROLM(0x00A6); ROLA(); ROLM(0x00A6);
    A = 0x06; c = 1;                             /* SEC; SBC $00A6 */
    { uint16_t t = (uint16_t)A + (uint8_t)~mem[0x00A6] + c; c = t >> 8; A = (uint8_t)t; }
    mem[0x00A6] = A;

    A = mem[0x0024];                             /* {00A4:00A5} = signed({0024:0023})>>1 */
    mem[0x0100 | cpu.S] = A; cpu.S--;            /* PHA (leaves the byte at $0100+S) */
    c = (A >= 0x80) ? 1 : 0;                     /* CMP #$80 -> sign bit into carry */
    RORA(); mem[0x00A4] = A;                     /* ROR A (arithmetic >>1, hi) */
    A = mem[0x0023]; RORA(); mem[0x00A5] = A;    /* ROR A (lo, carry from hi) */
    c = 0;                                       /* CLC */
    cpu.S++; A = mem[0x0100 | cpu.S];            /* PLA -> A = original $0024 */
    { uint16_t t = (uint16_t)A + 0x04 + c; c = t >> 8; A = (uint8_t)t; }   /* ADC #$04 */
    if (A & 0x80) A = 0x00;                      /* BPL: negative -> 0 */
    else if (A >= 0x09) A = 0x08;                /* else clamp >=9 -> 8 */
    mem[0x2822] = A;

    #undef LSRA
    #undef RORM
    #undef ASLM
    #undef ROLA
    #undef ROLM
    #undef RORA
}

/* set_plot_mask_and_halve_step @ $AB7B — pick a plot base ptr + quarter the step.
 *
 * Inputs : cpu.A (low 2 bits select an entry in tables $A7E9/$A7ED).
 * Effect : $28DC/$28DD = base ptr from the tables; if the index is 0, $0058=$FF
 *          (full plot mask); {$0051:$0050} >>= 2 (step / 4).
 * Contract: memory only (callers reload A/flags).
 */
void set_plot_mask_and_halve_step(void) {
    uint8_t x = cpu.A & 0x03;
    mem[0x28DC] = mem[0xA7E9 + x];
    mem[0x28DD] = mem[0xA7ED + x];
    uint8_t a = x;                             /* TXA */
    if (x == 0) { mem[0x0058] = 0xFF; a = 0xFF; }   /* idx 0: full mask, A=$FF */
    uint8_t c = 0;
    for (int i = 0; i < 2; i++) {              /* LSR $51; ROR $50 (x2) = >>2 */
        c = mem[0x0051] & 1; mem[0x0051] = (uint8_t)(mem[0x0051] >> 1);
        uint8_t v = mem[0x0050]; uint8_t nc = v & 1;
        mem[0x0050] = (uint8_t)((v >> 1) | (c << 7));
        c = nc;
    }
    /* Replicate the transpiled exit registers: unlike the other leaves a caller
       (terrain_sub_A822 path) may use the index X without reloading it. */
    cpu.X = x; cpu.A = a; cpu.C = c;
    cpu.N = (mem[0x0050] >> 7) & 1; cpu.Z = (mem[0x0050] == 0) ? 1 : 0;
}

/* terrain_point_distance @ $A8AF — Manhattan distance of a point to screen centre.
 *
 * Inputs : cpu.A (saved to $290E), $004F/$004E (point), $0051 (bias), $2915 (best).
 * Effect : computes |$004F-$80| + |$80-$004E| (+$0051); if it overflows, exceeds
 *          $2915, or carries, returns early (via terrain_distance_clamp_return,
 *          left transpiled — an empty RTS).  Otherwise latches the new nearest
 *          point into $2912-$2915 + $2910/$2911 and its distance into $2914.
 * Contract: memory only (callers reload A; SEC/CLC set carry before each op so
 *           entry carry is irrelevant).
 */
void terrain_point_distance(void) {
    uint8_t A, c;
    mem[0x290E] = cpu.A;

    A = mem[0x004F]; c = 1;                    /* SEC; SBC #$80 */
    { uint16_t t = (uint16_t)A + (uint8_t)~0x80 + c; c = t >> 8; A = (uint8_t)t; }
    mem[0x290B] = A;
    if (A & 0x80) {                            /* negate -> |$004F-$80| */
        c = 1; uint16_t t = (uint16_t)0 + (uint8_t)~mem[0x290B] + c; c = t >> 8; A = (uint8_t)t;
    }
    mem[0x290D] = A;

    A = 0x80; c = 1;                           /* SEC; A=$80; SBC $004E */
    { uint16_t t = (uint16_t)A + (uint8_t)~mem[0x004E] + c; c = t >> 8; A = (uint8_t)t; }
    mem[0x290C] = A;
    if (A & 0x80) {                            /* negate -> |$80-$004E| */
        c = 1; uint16_t t = (uint16_t)0 + (uint8_t)~mem[0x290C] + c; c = t >> 8; A = (uint8_t)t;
    }

    c = 0; { uint16_t t = (uint16_t)A + mem[0x290D] + c; c = t >> 8; A = (uint8_t)t; }  /* CLC; ADC $290D */
    if (c) { terrain_distance_clamp_return(); return; }
    mem[0x290D] = A;
    c = 0; { uint16_t t = (uint16_t)A + mem[0x0051] + c; c = t >> 8; A = (uint8_t)t; }  /* CLC; ADC $0051 */
    if (c) { terrain_distance_clamp_return(); return; }
    if (A >= mem[0x2915]) { terrain_distance_clamp_return(); return; }  /* CMP $2915; BCS */

    mem[0x2915] = A;
    mem[0x2914] = mem[0x290D];
    mem[0x2912] = mem[0x290B];
    mem[0x2913] = mem[0x290C];
    mem[0x2910] = mem[0x290E];
    mem[0x2911] = mem[0x290F];
    terrain_distance_clamp_return();
}

/* terrain_midpoint_displace @ $B2CC — midpoint of two view-space points (the
 * fractal subdivision step).  Indexed by cpu.X into delta tables $25B4/$25D2/
 * $25F0/$24E2/$23E2; adds them to $0082-$0086, halves with sign-extension into
 * {$008E:$008D} and {$0090:$008F}, and (when the $0086 sum is negative) offsets
 * {$0090:$008F} by +-((midpoint-$0082:$0083)>>1) depending on the final carry.
 * Contract: memory only.  Faithful carry threading (the ROR/CMP idioms and the
 * 16-bit add/sub borrow chains are carry-sensitive).
 */
void terrain_midpoint_displace(void) {
    uint8_t A, c;
    uint8_t x = cpu.X;
    #define D_ADC(v)  do { uint16_t _t = (uint16_t)A + (uint8_t)(v) + c; c = _t >> 8; A = (uint8_t)_t; } while (0)
    #define D_SBC(v)  D_ADC((uint8_t)~(uint8_t)(v))
    #define D_RORA()  do { uint8_t _n = A & 1; A = (uint8_t)((A >> 1) | (c << 7)); c = _n; } while (0)
    #define D_RORM(a) do { uint8_t _v = mem[a], _n = _v & 1; mem[a] = (uint8_t)((_v >> 1) | (c << 7)); c = _n; } while (0)
    #define D_LSRA()  do { c = A & 1; A = (uint8_t)(A >> 1); } while (0)

    c = 1; A = mem[0x0082]; D_ADC(mem[0x25B4 + x]); mem[0x008D] = A;   /* B2CC */
    A = mem[0x0083]; D_ADC(mem[0x25D2 + x]);
    c = (A >= 0x80) ? 1 : 0; D_RORA(); mem[0x008E] = A; D_RORM(0x008D); /* CMP #$80; ROR */

    c = 1; A = mem[0x0084]; D_ADC(mem[0x25F0 + x]); mem[0x008F] = A;   /* B2E0 */
    A = mem[0x0085]; D_ADC(mem[0x24E2 + x]);
    c = (A >= 0x80) ? 1 : 0; D_RORA(); mem[0x0090] = A; D_RORM(0x008F);

    A = mem[0x0086]; c = 1; D_ADC(mem[0x23E2 + x]); mem[0x0091] = A;   /* B2F4 */

    if (!(A & 0x80)) goto done;                /* B2FC BPL: return */
    if (c) {                                   /* B2FF BCS -> add branch (B31F) */
        A = mem[0x008D]; c = 1; D_SBC(mem[0x0082]); mem[0x00B5] = A;
        A = mem[0x008E]; D_SBC(mem[0x0083]); D_LSRA(); mem[0x00B6] = A; D_RORM(0x00B5);
        A = mem[0x008F]; c = 0; D_ADC(mem[0x00B5]); mem[0x008F] = A;
        A = mem[0x0090]; D_ADC(mem[0x00B6]); mem[0x0090] = A;
    } else {                                   /* subtract branch (B301) */
        A = mem[0x008D]; c = 1; D_SBC(mem[0x0082]); mem[0x00B5] = A;
        A = mem[0x008E]; D_SBC(mem[0x0083]); D_LSRA(); mem[0x00B6] = A; D_RORM(0x00B5);
        A = mem[0x008F]; c = 1; D_SBC(mem[0x00B5]); mem[0x008F] = A;
        A = mem[0x0090]; D_SBC(mem[0x00B6]); mem[0x0090] = A;
    }
done:
    #undef D_ADC
    #undef D_SBC
    #undef D_RORA
    #undef D_RORM
    #undef D_LSRA
    return;
}

/* terrain_plot_pixel @ $A6D3 — OR a 2-bit voxel mask into the terrain bitmap.
 *
 * Inputs : cpu.Y = scanline (clipped to < $97), cpu.X = mask index.
 * Effect : bitmap ptr {$0081:$0080} from row tables $28FA/$28CA[Y]; mask from
 *          $BC00[X] | ($BC00[X]>>1) ANDed with the plot-mask $0058, ORed into
 *          (ptr),$BD00[X].  Restores Y from $28E2.
 * Contract: memory (the bitmap write).  X and Y are preserved (callers read
 *           them via INX/DEY); exit A/flags are dead at every call site, so
 *           left incidental.  Calls the empty transpiled terrain_plot_skip_return.
 */
void terrain_plot_pixel(void) {
    if (cpu.Y >= 0x97) { terrain_plot_skip_return(); return; }   /* CPY #$97; BCS skip */
    uint8_t savedY = cpu.Y;
    mem[0x28E2] = savedY;
    mem[0x0080] = mem[0x28CA + savedY];
    mem[0x0081] = mem[0x28FA + savedY];
    cpu.Y = mem[0xBD00 + cpu.X];            /* LDY $BD00,X (the sub-x within the byte) */
    uint8_t a = mem[0xBC00 + cpu.X];        /* LDA $BC00,X (pixel mask) */
    mem[0x00B5] = a;
    a = (uint8_t)(a >> 1);                  /* LSR A */
    a |= mem[0x00B5];                       /* ORA $B5 -> 2-bit mask */
    a &= mem[0x0058];                       /* AND plot mask */
    a |= bus_read(ZP_IND_Y(0x80));          /* ORA ($80),Y */
    bus_write(ZP_IND_Y(0x80), a);           /* STA ($80),Y */
    cpu.Y = savedY;                         /* LDY $28E2 (restore) */
    terrain_plot_skip_return();
}

/* terrain_clip_row_top @ $A6CB — clip a column's top against the per-column row
 * limit $260E[X]; plot the pixel only when the scanline Y is strictly above it.
 * Contract: memory.  X/Y preserved; exit A/flags dead (callers reload).
 */
void terrain_clip_row_top(void) {
    uint8_t y = cpu.Y;                       /* TYA */
    uint8_t lim = mem[0x260E + cpu.X];       /* CMP $260E,X */
    if (y < lim)  { terrain_plot_skip_return(); return; }   /* BCC skip */
    if (y == lim) { terrain_plot_skip_return(); return; }   /* BEQ skip */
    terrain_plot_pixel();
}

/* raster_fill_region @ $AB9A — fill a 12x32 cell grid, plotting set bits.
 *
 * Ensures the step {$0051:$0050} is nonzero; subtracts it from $1000 (counting
 * into $004F) to find the start row; then walks 12 rows x 32 columns, advancing
 * the {$0053:$0052}/{$0055:$0054} fixed-point accumulators by the step.  For each
 * cell it tests the bit AC3A[col&7] in the source buffer at ($00C3),col>>3 (the
 * column index reflected via $1F-col when $28DF is set) and, when set, plots it
 * through terrain_clip_row_top (native) at ($004F,$004E).
 *
 * Contract: memory (the bitmap writes + the accumulators).  Exit X=$28E1 is
 * restored (callers read it); A/Y/flags are dead.  Faithful carry threading:
 * the start-row subtract chains carry ACROSS iterations (one SEC before the
 * loop, none inside), a 6502 quirk reproduced exactly.
 */
void raster_fill_region(void) {
    uint8_t A, c;

    if (mem[0x0051] == 0) { mem[0x0050] = 0x00; mem[0x0051]++; }   /* AB9A nonzero step */

    mem[0x00C4] = 0x10; mem[0x00C3] = 0x00;                        /* ABA4: $C4:$C3 = $1000 */
    c = 1;                                                         /* SEC (once, chains) */
    do {                                                           /* ABAD subtract loop */
        uint16_t t;
        t = (uint16_t)mem[0x00C3] + (uint8_t)~mem[0x0050] + c; c = t >> 8; mem[0x00C3] = (uint8_t)t;
        t = (uint16_t)mem[0x00C4] + (uint8_t)~mem[0x0051] + c; c = t >> 8; mem[0x00C4] = (uint8_t)t;
        mem[0x004F]--;                                            /* DEC $4F (no carry effect) */
    } while (c);                                                  /* BCS */

    mem[0x28DE] = mem[0x004F];                                    /* ABBD */
    mem[0x0054] = 0x00; mem[0x0055] = 0x00;

    do {                                                          /* ABC8 outer (12 rows) */
        mem[0x0052] = 0x00; mem[0x0053] = 0x00; mem[0x00C4] = 0x00;
        A = mem[0x0055];                                          /* {$C4:A} = $55 << 2 */
        c = A >> 7; A = (uint8_t)(A << 1); mem[0x00C4] = (uint8_t)((mem[0x00C4] << 1) | c);
        c = A >> 7; A = (uint8_t)(A << 1); mem[0x00C4] = (uint8_t)((mem[0x00C4] << 1) | c);
        c = 0; { uint16_t t = (uint16_t)A + mem[0x28DC] + c; c = t >> 8; A = (uint8_t)t; } mem[0x00C3] = A;
        { uint16_t t = (uint16_t)mem[0x00C4] + mem[0x28DD] + c; mem[0x00C4] = (uint8_t)t; }
        mem[0x004F] = mem[0x28DE];

        do {                                                      /* ABEA inner (32 cols) */
            A = mem[0x0053];                                      /* LDA $53 */
            if (mem[0x28DF] != 0) {                               /* LDX $28DF; BNE -> reflect */
                c = 1; uint16_t t = (uint16_t)0x1F + (uint8_t)~mem[0x0053] + c; c = t >> 8; A = (uint8_t)t;
            }
            uint8_t bx = (uint8_t)(A & 0x07);                     /* bit within byte */
            cpu.Y = (uint8_t)(A >> 3);                            /* byte offset (for ($C3),Y) */
            if (bus_read(ZP_IND_Y(0xC3)) & mem[0xAC3A + bx]) {    /* cell bit set? */
                cpu.X = mem[0x004F]; cpu.Y = mem[0x004E];
                terrain_clip_row_top();
            }
            mem[0x004F]++;                                        /* INC $4F */
            c = 0; { uint16_t t = (uint16_t)mem[0x0052] + mem[0x0050] + c; c = t >> 8; mem[0x0052] = (uint8_t)t; }
            { uint16_t t = (uint16_t)mem[0x0053] + mem[0x0051] + c; mem[0x0053] = (uint8_t)t; }
        } while (mem[0x0053] < 0x20);                             /* CMP #$20; BCC */

        mem[0x004E]--;                                            /* DEC $4E */
        c = 0; { uint16_t t = (uint16_t)mem[0x0054] + mem[0x0050] + c; c = t >> 8; mem[0x0054] = (uint8_t)t; }
        { uint16_t t = (uint16_t)mem[0x0055] + mem[0x0051] + c; mem[0x0055] = (uint8_t)t; }
    } while (mem[0x0055] < 0x0C);                                 /* CMP #$0C; BCS done */

    cpu.X = mem[0x28E1];                                          /* AC36: LDX $28E1 */
}

/* terrain_sub_A822 @ $A822 — plot one terrain object (gun tower etc.), variant A.
 *
 * Indexed by cpu.X into the object tables.  Early-outs if the slot is busy
 * ($2487/$242D nonzero).  Sets the source ptr $28DC/$28DD, step {$0051:$0050},
 * screen pos $004E/$004F; if the object's flag byte $0A00[$2276[X]] >= $FA it is
 * a "mask" object (set_plot_mask), else it plots a marker pixel (point_distance +
 * clip_row_top) and may latch a targeting record (gated on $0034/$006A/$003E/
 * RANDOM/$28ED).  Finally, when the step's hi byte < $0D, fills via
 * raster_fill_region.  Reads POKEY RANDOM once.  All callees native/empty.
 * Contract: memory; exit X=$28E1 (restored by the tail raster_fill_region or the
 * explicit a868 path); other regs dead.
 */
void terrain_sub_A822(void) {
    if (mem[0x2487 + cpu.X] != 0) { terrain_obj_skip_return(); return; }   /* A822 */
    if (mem[0x242D + cpu.X] != 0) { terrain_obj_skip_return(); return; }   /* A827 */
    mem[0x28DC] = 0xF9; mem[0x28DD] = 0xA6;
    mem[0x0058] = 0xFF;
    mem[0x0051] = mem[0x232E + cpu.X];
    mem[0x0050] = mem[0x2300 + cpu.X];
    mem[0x004E] = mem[0x245A + cpu.X];
    cpu.Y = mem[0x004E];                          /* TAY */
    mem[0x004F] = mem[0x2400 + cpu.X];
    cpu.X = mem[0x2276 + cpu.X];                  /* LDA $2276,X; TAX */
    cpu.A = mem[0x0A00 + cpu.X];

    if (cpu.A >= 0xFA) {                           /* CMP #$FA; BCC a860 */
        set_plot_mask_and_halve_step();
    } else {                                       /* a860 */
        cpu.A = cpu.Y;                             /* TYA */
        cpu.X = mem[0x004F];                       /* LDX $4F */
        if (cpu.A < mem[0x260E + cpu.X]) { cpu.X = mem[0x28E1]; return; }  /* CMP 260E,X; BCC a868 */
        cpu.A = 0x80; terrain_point_distance();    /* a86c-a86e */
        cpu.X--; terrain_clip_row_top(); cpu.X++;  /* DEX; a872; INX */
        if (mem[0x0034] < 0x37) {                  /* CMP #$37; BCS a8a1 */
            mem[0x28FC] = 0x01; mem[0x28FB] = 0x01;
            /* gates: $6A negative, $3E==0, RANDOM negative, $28ED==0 (short-circuit
               matches the 6502: the $D20A read only happens if the prior gates pass) */
            if ((mem[0x006A] & 0x80) && mem[0x003E] == 0 &&
                (bus_read(0xD20A) & 0x80) && mem[0x28ED] == 0) {
                mem[0x28EB] = cpu.X; mem[0x28EC] = cpu.Y;
                mem[0x28ED] = mem[0x0051];
            }
        }
    }
    /* L_a8a1 */
    if (mem[0x0051] >= 0x0D) { cpu.X = mem[0x28E1]; return; }   /* CMP #$0D; BCS a868 */
    mem[0x28DF] = 0x00;
    raster_fill_region();
}

/* terrain_sub_A90A @ $A90A — plot one terrain object, variant B (4 plot points).
 *
 * Like A822 but: column-reflect flag $28DF/$290F from $23B5[X]&1; uses point at
 * ($004E vs $260E[$004F]); after the fill it may additionally stamp a 2x2 cross of
 * pixels (clip_row_top x4 around $2400[X]/$245A[X]) when $00A7==0 && $003E==0 and
 * the object flag is in [2,$F8).  No RANDOM read.  All callees native/empty.
 * Contract: memory; exit X=$28E1; other regs dead.
 */
void terrain_sub_A90A(void) {
    if (mem[0x2487 + cpu.X] != 0) { terrain_distance_clamp_return(); return; }   /* A90A */
    if (mem[0x242D + cpu.X] != 0) { terrain_distance_clamp_return(); return; }   /* A90F */
    mem[0x28DC] = 0xF1; mem[0x28DD] = 0xA7;
    cpu.A = (uint8_t)(mem[0x23B5 + cpu.X] & 0x01);            /* LDA $23B5,X; AND #1 */
    mem[0x290F] = cpu.A; mem[0x28DF] = cpu.A;
    mem[0x004F] = mem[0x2400 + cpu.X];
    mem[0x004E] = mem[0x245A + cpu.X];
    mem[0x0051] = mem[0x232E + cpu.X];
    mem[0x0050] = mem[0x2300 + cpu.X];
    cpu.Y = mem[0x2276 + cpu.X];                  /* LDY $2276,X */
    cpu.A = mem[0x0A00 + cpu.Y];
    if (cpu.A >= 0xFA) set_plot_mask_and_halve_step();         /* CMP #$FA; BCC a94a */

    cpu.Y = mem[0x004F];                          /* L_a94a: LDY $4F */
    cpu.A = mem[0x004E];                          /* LDA $4E */
    if (cpu.A >= mem[0x260E + cpu.Y]) {           /* CMP 260E,Y; BCC a965 */
        cpu.A = 0x00; terrain_point_distance();   /* a953-a955 */
        mem[0x0058] = 0xAA;
        if (mem[0x0051] < 0x0D) raster_fill_region();          /* CMP #$0D; BCS a965 */
    }

    /* L_a965 */
    if (mem[0x00A7] == 0 && mem[0x003E] == 0) {   /* BNE a998 (both) */
        cpu.Y = mem[0x2276 + cpu.X];              /* LDY $2276,X */
        cpu.A = mem[0x0A00 + cpu.Y];
        if (cpu.A >= 0x02 && cpu.A < 0xF8) {      /* CMP #2 BCC; CMP #$F8 BCS -> [2,$F8) */
            mem[0x0058] = 0xFF;
            cpu.Y = mem[0x245A + cpu.X];          /* LDY $245A,X */
            cpu.X = mem[0x2400 + cpu.X];          /* LDA $2400,X; TAX */
            terrain_clip_row_top();               /* a986 */
            cpu.X--; cpu.Y--; terrain_clip_row_top();   /* DEX; DEY; a98b */
            cpu.X--; cpu.Y++; terrain_clip_row_top();   /* DEX; INY; a990 */
            cpu.X++; cpu.Y++; terrain_clip_row_top();   /* INX; INY; a995 */
        }
    }
    /* L_a998 */
    cpu.X = mem[0x28E1];
}

/* terrain_plot_object @ $A63B — per-object terrain raster dispatch.
 *
 * Indexed by entry cpu.X.  Early-outs on an empty object ($0A00[$2276[X]]==0) or
 * a too-near zero-distance object.  Saves X to $28E1.  If the object's $0900 flag
 * is negative it routes to the A822 plotter; otherwise (for object codes in
 * [2,$F8) with $00A7==0) it computes the half-width into $0059/$005A (ABS via the
 * sign bit, rounded) and the span coords $28E4-$28EA/$0079, then falls through to
 * the A90A plotter.  Tail-calls native terrain_sub_A90A (the trailing
 * terrain_clip_row_top in the transpile is dead code after the return).
 * Contract: memory; carry threaded through the width arithmetic.
 */
void terrain_plot_object(void) {
    uint8_t A, c;
    cpu.Y = mem[0x2276 + cpu.X];                              /* LDY $2276,X */
    if (mem[0x0A00 + cpu.Y] == 0) { terrain_plot_return(); return; }   /* a641 BEQ */
    if (mem[0x232E + cpu.X] == 0 && mem[0x2300 + cpu.X] < 0x22) {      /* a646/a64b */
        terrain_plot_return(); return;
    }
    mem[0x28E1] = cpu.X;                                      /* a64f STX $28E1 */
    if (mem[0x0900 + cpu.Y] & 0x80) { terrain_sub_A822(); return; }    /* a655 BPL -> N set: A822 */

    int go = 0;                                              /* a65a: take the a66c path? */
    if (mem[0x00A7] == 0) {
        uint8_t v = mem[0x0A00 + cpu.Y];
        if (v >= 0x02 && v < 0xF8) go = 1;                   /* CMP #2 BCC; CMP #$F8 BCC a66c */
    }
    if (go) {
        A = mem[0x22D2 + cpu.X];                             /* a66c half-width */
        mem[0x0059] = A; mem[0x005A] = A;
        c = A >> 7;                                          /* ASL A -> carry = sign bit */
        if (c) {                                             /* negative: $5A = $FF - $5A */
            uint16_t t = (uint16_t)0xFF + (uint8_t)~mem[0x005A] + c; c = t >> 8; mem[0x005A] = (uint8_t)t;
            c = 1;                                           /* SEC */
        }
        { uint8_t v = mem[0x0059], nc = v & 1;               /* ROR $59 */
          mem[0x0059] = (uint8_t)((v >> 1) | (c << 7)); c = nc; }
        if (c) mem[0x0059]++;                                /* round */

        A = mem[0x232E + cpu.X]; mem[0x28E4] = A;            /* a683 */
        if (A >= mem[0x005A]) {                              /* CMP $5A; BCC a6c8 */
            c = 0; { uint16_t t = (uint16_t)A + mem[0x005A] + c; c = t >> 8; A = (uint8_t)t; }  /* CLC ADC $5A */
            if (A < mem[0x28EA]) {                            /* CMP $28EA; BCS a6c8 */
                mem[0x28EA] = A;
                mem[0x005A] = (uint8_t)(mem[0x005A] >> 1);    /* LSR $5A */
                c = 1; { uint16_t t = (uint16_t)A + (uint8_t)~mem[0x005A] + c; c = t >> 8; A = (uint8_t)t; }
                mem[0x0079] = A;                              /* SEC; SBC $5A; STA $79 */
                mem[0x28E7] = mem[0x0059];
                mem[0x28E8] = mem[0x28E4];
                mem[0x28E6] = mem[0x2276 + cpu.X];
                mem[0x0040] = 0x64;
                if (mem[0x242D + cpu.X] == 0 && mem[0x2487 + cpu.X] == 0) {   /* both BNE a6c8 */
                    mem[0x003F] = mem[0x2400 + cpu.X];
                    mem[0x0040] = mem[0x245A + cpu.X];
                }
            }
        }
    }
    terrain_sub_A90A();                                      /* a6c8 tail */
}

/* terrain_column_rasterize @ $B33D — THE fractal column renderer.
 *
 * Walks columns from $0082 up to $D4, interpolating per-segment heights ($00EA[]/
 * $00F4[]) and midpoint-refining the column position toward $0095[Y]; for each
 * column it clamps the height to $97, updates the max-height array $260E[X], and
 * OR-plots the silhouette pixel into the bitmap at ($0080),$BD00[X] using the
 * $BC00 bit table and the $28CA/$28FA row-address tables.
 *
 * Self-contained (no calls), so transliterated goto-faithfully with purely local
 * A/X/Y + carry c; the bitmap pointer is computed manually (= ZP_IND_Y($80)).
 * Contract: memory; cpu.X is preserved (saved to $0060, restored on exit — and
 * since we never touch cpu.X it stays at entry anyway).  Validated against a real
 * in-flight RAM snapshot (random mem[] would not terminate — $0095[] must be a
 * realistic increasing column array, else the midpoint loop has a fixed point).
 */
void terrain_column_rasterize(void) {
    uint8_t A, X, Y, c = 0;
    #define ADC_(v)  do { uint16_t _t=(uint16_t)A+(uint8_t)(v)+c; c=(uint8_t)(_t>>8); A=(uint8_t)_t; } while(0)
    #define SBC_(v)  ADC_((uint8_t)~(uint8_t)(v))
    #define RORA_()  do { uint8_t _n=A&1; A=(uint8_t)((A>>1)|(c<<7)); c=_n; } while(0)
    #define LSRA_()  do { c=A&1; A=(uint8_t)(A>>1); } while(0)
    #define PLOT()   do { mem[0x00B5]=Y; uint8_t _ai=A; \
        mem[0x0080]=mem[0x28CA+_ai]; mem[0x0081]=mem[0x28FA+_ai]; \
        uint8_t _bo=mem[0xBD00+X]; \
        uint16_t _ad=(uint16_t)(mem[0x0080]|(mem[0x0081]<<8))+_bo; \
        bus_write(_ad,(uint8_t)(bus_read(_ad)|mem[0xBC00+X])); \
        Y=mem[0x00B5]; } while(0)

    X = cpu.X; mem[0x0060] = X;                          /* b33d STX $60 */
    A = mem[0x0095];                                     /* b33f */
    if (A < 0x2D) goto done;                             /* b341 CMP #$2D; BCC b37f */
    if (A < mem[0x0082]) goto done;                      /* b345 CMP $82; BCC b37f */
    if (A != mem[0x0082]) goto L_b380;                   /* b349 BNE b380 */
    X = mem[0x0095];                                     /* b34b LDX $95 */
    A = mem[0x00EA];                                     /* b34d */
    if (A < mem[0x260E + X]) goto done;                  /* b352 BCC b37d (->LDX$60;ret) */
    if (A == mem[0x260E + X]) goto done;                 /* b354 BEQ b37d */
    mem[0x260E + X] = A;
    if (A >= 0x97) { mem[0x260E + X] = 0xFF; A = 0x97; } /* b359 CMP #$97; BCC b364 */
    PLOT();                                              /* b364 */
    goto done;                                           /* b37d LDX $60; b37f return */

L_b380:
    Y = 0x00;                                            /* b380 LDY #0 */
L_b382:
    A = mem[0x0082];                                     /* b382 */
    if (A < 0x2C) goto L_b38c;                           /* CMP #$2C; BCC b38c */
    X = A; goto L_b446;                                  /* TAX; goto b446 */
L_b38c:
    c = 0; ADC_(mem[0x0095 + Y]); RORA_();               /* CLC; ADC $95,Y; ROR */
    if (A < 0x2C) goto L_b397;                           /* CMP #$2C; BCC b397 */
    if (A != 0x2C) goto L_b3e9;                          /* BNE b3e9 */
L_b397:
    mem[0x0082] = A;                                     /* b397 STA $82 */
    A = mem[0x0086]; c = 1; ADC_(mem[0x00F4 + Y]); mem[0x0086] = A;  /* LDA $86; SEC; ADC F4,Y */
    if (A & 0x80) goto L_b3af;                           /* BMI b3af */
    c = 0; A = mem[0x0084]; ADC_(mem[0x00EA + Y]); RORA_(); mem[0x0084] = A;  /* CLC LDA84 ADC EA,Y ROR */
    goto L_b382;
L_b3af:
    if (c) goto L_b3cd;                                  /* BCS b3cd (c from the $86 ADC) */
    A = mem[0x0095 + Y]; c = 1; SBC_(mem[0x0082]); LSRA_(); mem[0x00B5] = A;
    c = 0; A = mem[0x0084]; ADC_(mem[0x00EA + Y]); RORA_(); c = 1; SBC_(mem[0x00B5]);
    if (!c) A = 0x00;                                    /* b3c4 BCC b3c8 (skip LDA#0) */
    mem[0x0084] = A; goto L_b382;
L_b3cd:
    A = mem[0x0095 + Y]; c = 1; SBC_(mem[0x0082]); LSRA_(); mem[0x00B5] = A;
    c = 0; A = mem[0x0084]; ADC_(mem[0x00EA + Y]); RORA_(); c = 0; ADC_(mem[0x00B5]);
    if (c) A = 0xFF;                                     /* b3e0 BCC b3e4 (skip LDA#$FF) */
    mem[0x0084] = A; goto L_b382;
L_b3e9:
    mem[0x0096 + Y] = A;                                 /* b3e9 STA $96,Y */
    A = mem[0x0086]; c = 1; ADC_(mem[0x00F4 + Y]); mem[0x00F5 + Y] = A;  /* LDA $86; SEC; ADC F4,Y; STA F5,Y */
    if (A & 0x80) goto L_b405;                           /* BMI b405 */
    c = 0; A = mem[0x0084]; ADC_(mem[0x00EA + Y]); RORA_(); mem[0x00EB + Y] = A;
    Y++; goto L_b382;                                    /* INY */
L_b405:
    if (c) goto L_b425;                                  /* BCS b425 (c from the $86 ADC) */
    A = mem[0x0096 + Y]; c = 1; SBC_(mem[0x0082]); LSRA_(); mem[0x00B5] = A;
    c = 0; A = mem[0x0084]; ADC_(mem[0x00EA + Y]); RORA_(); c = 1; SBC_(mem[0x00B5]);
    if (!c) A = 0x00;                                    /* b41a BCC b41e */
    mem[0x00EB + Y] = A; Y++; goto L_b382;
L_b425:
    A = mem[0x0096 + Y]; c = 1; SBC_(mem[0x0082]); LSRA_(); mem[0x00B5] = A;
    c = 0; A = mem[0x0084]; ADC_(mem[0x00EA + Y]); RORA_(); c = 0; ADC_(mem[0x00B5]);
    if (c) A = 0xFF;                                     /* b438 BCC b43c */
    mem[0x00EB + Y] = A; Y++; goto L_b382;

L_b443:
    goto done;                                           /* b443 LDX $60; return */
L_b446:
    if (X >= 0xD4) goto L_b443;                          /* CPX #$D4; BCS b443 */
    A = X; mem[0x0082] = X;                              /* TXA; STX $82 */
    c = 1; SBC_(mem[0x0095 + Y]);                        /* SEC; SBC $95,Y */
    c = (A >= 0xFE) ? 1 : 0;                             /* CMP #$FE (carry used at b4cc) */
    if (A != 0xFE) goto L_b4cc;                          /* BNE b4cc */
    A = mem[0x00EA + Y]; ADC_(mem[0x0084]); RORA_();     /* LDA EA,Y; ADC $84 (c=1); ROR */
    if (A < mem[0x260E + X]) goto L_b489;                /* CMP 260E,X; BCC b489 */
    if (A == mem[0x260E + X]) goto L_b489;               /* BEQ b489 */
    mem[0x260E + X] = A;
    if (A >= 0x97) { mem[0x260E + X] = 0xFF; A = 0x97; } /* CMP #$97; BCC b470 */
    PLOT();                                              /* b470 */
L_b489:
    X++;                                                 /* INX */
    A = mem[0x00EA + Y]; mem[0x0084] = A;                /* LDA EA,Y; STA $84 */
    if (A < mem[0x260E + X]) goto L_b4bd;                /* CMP 260E,X; BCC b4bd */
    if (A == mem[0x260E + X]) goto L_b4bd;               /* BEQ b4bd */
    mem[0x260E + X] = A;
    if (A >= 0x97) { mem[0x260E + X] = 0xFF; A = 0x97; }
    PLOT();                                              /* b4a4 */
L_b4bd:
    Y--; if (Y & 0x80) goto L_b4c9;                      /* DEY; BMI b4c9 */
    X++;                                                 /* INX */
    A = mem[0x00F5 + Y]; mem[0x0086] = A;                /* LDA F5,Y; STA $86 */
    goto L_b446;
L_b4c9:
    goto done;                                           /* b4c9 LDX $60; return */
L_b4cc:
    if (!c) goto L_b50d;                                 /* BCC b50d (c from CMP #$FE) */
    A = mem[0x00EA + Y]; mem[0x0084] = A;                /* LDA EA,Y; STA $84 */
    if (A < mem[0x260E + X]) goto L_b501;                /* CMP 260E,X; BCC b501 */
    if (A == mem[0x260E + X]) goto L_b501;               /* BEQ b501 */
    mem[0x260E + X] = A;
    if (A >= 0x97) { mem[0x260E + X] = 0xFF; A = 0x97; }
    PLOT();                                              /* b4e8 */
L_b501:
    Y--; if (Y & 0x80) goto L_b4c9;                      /* DEY; BMI b4c9 */
    X++;                                                 /* INX */
    A = mem[0x00F5 + Y]; mem[0x0086] = A;                /* LDA F5,Y; STA $86 */
    goto L_b446;
L_b50d:
    A = X; ADC_(mem[0x0095 + Y]); RORA_(); mem[0x0096 + Y] = A;  /* TXA; ADC $95,Y (c=0); ROR; STA 96,Y */
    A = mem[0x0086]; c = 1; ADC_(mem[0x00F4 + Y]); mem[0x00F5 + Y] = A;  /* LDA $86; SEC; ADC F4,Y; STA F5,Y */
    if (A & 0x80) goto L_b52e;                           /* BMI b52e */
    c = 0; A = mem[0x0084]; ADC_(mem[0x00EA + Y]); RORA_(); mem[0x00EB + Y] = A;
    Y++; goto L_b446;                                    /* INY */
L_b52e:
    if (c) goto L_b54c;                                  /* BCS b54c (c from the $86 ADC) */
    A = mem[0x0096 + Y]; SBC_(mem[0x0082]); LSRA_(); mem[0x00B5] = A;   /* SBC $82 (c=0, no SEC); LSR */
    c = 0; A = mem[0x0084]; ADC_(mem[0x00EA + Y]); RORA_(); SBC_(mem[0x00B5]);  /* CLC ADC EA,Y ROR; SBC B5 (c from ROR) */
    if (!c) A = 0x00;                                    /* b541 BCC b545 */
    mem[0x00EB + Y] = A; Y++; goto L_b446;
L_b54c:
    A = mem[0x0096 + Y]; SBC_(mem[0x0082]); LSRA_(); mem[0x00B5] = A;   /* SBC $82 (c=1, no SEC); LSR */
    c = 0; A = mem[0x0084]; ADC_(mem[0x00EA + Y]); RORA_(); ADC_(mem[0x00B5]);  /* CLC ADC EA,Y ROR; ADC B5 (c from ROR) */
    if (c) A = 0xFF;                                     /* b55d BCC b561 */
    mem[0x00EB + Y] = A; Y++; goto L_b446;

done:
    #undef ADC_
    #undef SBC_
    #undef RORA_
    #undef LSRA_
    #undef PLOT
    return;
}

/* terrain_sub_B172 @ $B172 — fractal terrain subdivision driver.
 *
 * Recursively midpoint-subdivides a span (calling native terrain_midpoint_displace
 * per split, storing the 5-byte sub-point into the $25B5/$25D3/$25F1/$24E3/$23E3[X]
 * stacks), bottoming out into a column rasterize (native terrain_column_rasterize)
 * per leaf segment, then unwinds (b2aa: DEX, reload span from the stacks).  The
 * recursion depth is bounded by the $009F budget (=$14) and X < $0F.
 *
 * cpu.X is the working stack index (threaded through the native sub-calls, which
 * preserve it); cpu.Y is left untouched (terrain_column_rasterize reads it as the
 * caller's value).  Carry threaded locally; most branches re-derive from CMP.
 * Contract: memory.  Validated with the real flight snapshot (it tail-drives
 * terrain_column_rasterize, which random mem[] can't terminate).
 */
void terrain_sub_B172(void) {
    uint8_t A, c;
    #define ADC_(v) do { uint16_t _t=(uint16_t)A+(uint8_t)(v)+c; c=(uint8_t)(_t>>8); A=(uint8_t)_t; } while(0)
    #define SBC_(v) ADC_((uint8_t)~(uint8_t)(v))

    uint8_t b5 = (uint8_t)(mem[0x25D2] ^ 0x80); mem[0x00B5] = b5;   /* b172 (signed cmp) */
    A = (uint8_t)(mem[0x0083] ^ 0x80);
    c = (A >= b5) ? 1 : 0;                                /* CMP $B5 */
    if (A == b5) { A = mem[0x0082]; c = (A >= mem[0x25B4]) ? 1 : 0; }   /* BNE b186; else CMP 25B4 */
    if (c) return;                                        /* b186 BCS b1c1 */
    mem[0x009F] = 0x14;                                   /* b188 */

L_b18c:
    if (!(mem[0x0083] & 0x80)) goto L_b1d9;               /* LDA $83; BPL b1d9 */
    mem[0x009F]--; if (mem[0x009F] & 0x80) return;        /* DEC $9F; BMI b1c1 */
    terrain_midpoint_displace();                          /* b194 (uses cpu.X) */
    A = mem[0x008E];
    if (A & 0x80) goto L_b1c2;                            /* BMI b1c2 */
    if (A == 0 && mem[0x008D] < 0x28) goto L_b1c2;        /* BNE b1a3; else $8D<$28 -> b1c2 */
    mem[0x25B5 + cpu.X] = mem[0x008D];                    /* b1a3 store sub-point */
    mem[0x25D3 + cpu.X] = mem[0x008E];
    mem[0x25F1 + cpu.X] = mem[0x008F];
    mem[0x24E3 + cpu.X] = mem[0x0090];
    mem[0x23E3 + cpu.X] = mem[0x0091];
    cpu.X++;
    if (cpu.X < 0x0F) goto L_b18c;                        /* CPX #$0F; BCC b18c */
    return;
L_b1c2:
    mem[0x0082] = mem[0x008D]; mem[0x0083] = mem[0x008E];
    mem[0x0084] = mem[0x008F]; mem[0x0085] = mem[0x0090]; mem[0x0086] = mem[0x0091];
    goto L_b18c;
L_b1d9:
    if (mem[0x0083] != 0) return;                         /* LDA $83; BNE b1c1 */
    if (mem[0x0082] >= 0xD8) return;                      /* CMP #$D8; BCS b1c1 */
L_b1e3:
    if (mem[0x25D2 + cpu.X] == 0) goto L_b211;            /* LDA 25D2,X; BEQ b211 */
L_b1e8:
    mem[0x009F]--; if (mem[0x009F] & 0x80) return;        /* DEC $9F; BMI b210 */
    terrain_midpoint_displace();
    mem[0x25B5 + cpu.X] = mem[0x008D];
    mem[0x25D3 + cpu.X] = mem[0x008E];
    mem[0x25F1 + cpu.X] = mem[0x008F];
    mem[0x24E3 + cpu.X] = mem[0x0090];
    mem[0x23E3 + cpu.X] = mem[0x0091];
    cpu.X++;
    if (cpu.X >= 0x0F) return;                            /* CPX #$0F; BCS b210 */
    goto L_b1e3;
L_b211:
    A = mem[0x0085];
    if (A & 0x80) goto L_b21d;                            /* BMI b21d */
    if (A != 0) goto L_b22d;                              /* BNE b22d */
    if (mem[0x0084] >= 0x6C) goto L_b22d;                 /* CMP #$6C; BCS b22d */
L_b21d:
    A = mem[0x24E2 + cpu.X];
    if (A & 0x80) goto L_b23e;                            /* BMI b23e */
    if (A != 0) goto L_b241;                              /* BNE b241 */
    if (mem[0x25F0 + cpu.X] < 0x6C) goto L_b23e;          /* CMP #$6C; BCC b23e */
    goto L_b241;
L_b22d:
    A = mem[0x24E2 + cpu.X];
    if (A & 0x80) goto L_b25d;                            /* BMI b25d */
    if (A != 0) goto L_b27b;                              /* BNE b27b */
    if (mem[0x25F0 + cpu.X] < 0x6C) goto L_b25d;          /* CMP #$6C; BCC b25d */
    goto L_b27b;
L_b23e:
    goto L_b2aa;
L_b241:
    c = 1; A = mem[0x25B4 + cpu.X]; SBC_(mem[0x0082]);    /* SEC; LDA 25B4,X; SBC $82 */
    if (A < 0x14) goto L_b27b;                            /* CMP #$14; BCC b27b */
    A = (uint8_t)(A >> 2); mem[0x00B5] = A;               /* LSR;LSR; STA $B5 */
    A = mem[0x0084]; c = 1; SBC_(mem[0x00B5]);            /* LDA $84; SEC; SBC $B5 */
    A = mem[0x0085]; SBC_(0x00);                          /* LDA $85; SBC #0 (16-bit) */
    if (!(A & 0x80)) goto L_b27b;                         /* BPL b27b */
    goto L_b1e8;
L_b25d:
    c = 1; A = mem[0x25B4 + cpu.X]; SBC_(mem[0x0082]);
    if (A < 0x14) goto L_b27b;
    A = (uint8_t)(A >> 2); mem[0x00B5] = A;
    A = mem[0x25F0 + cpu.X]; c = 1; SBC_(mem[0x00B5]);
    A = mem[0x24E2 + cpu.X]; SBC_(0x00);
    if (!(A & 0x80)) goto L_b27b;
    goto L_b1e8;
L_b27b:
    A = mem[0x0085];                                      /* clamp $84 by $85 sign */
    if (A != 0) mem[0x0084] = (A & 0x80) ? 0x00 : 0xFF;
    A = mem[0x24E2 + cpu.X];                              /* select/clamp $EA */
    if (A == 0) A = mem[0x25F0 + cpu.X];
    else A = (A & 0x80) ? 0x00 : 0xFF;
    mem[0x00EA] = A;
    mem[0x0095] = mem[0x25B4 + cpu.X];
    mem[0x00F4] = mem[0x23E2 + cpu.X];
    terrain_column_rasterize();                           /* b2a7 (uses cpu.X, cpu.Y) */
L_b2aa:
    if (cpu.X == 0) return;                               /* CPX #0; BEQ b2cb */
    mem[0x0082] = mem[0x25B4 + cpu.X];                    /* reload span from the stacks */
    mem[0x0083] = mem[0x25D2 + cpu.X];
    mem[0x0084] = mem[0x25F0 + cpu.X];
    mem[0x0085] = mem[0x24E2 + cpu.X];
    mem[0x0086] = mem[0x23E2 + cpu.X];
    cpu.X--;                                              /* DEX */
    goto L_b1d9;
    #undef ADC_
    #undef SBC_
}

/* terrain_gen_A613 @ $A613 — per-frame random terrain/object jitter (2+1 RANDOM).
 *
 * $2829 = RANDOM + RANDOM (with the add's carry rolled into $0068, EOR'd $FF when
 * the ship column $0064 >= $6C); $282C = RANDOM - $80 with $0069 = -1 on borrow
 * (a signed offset).  Tail-calls the empty terrain_plot_return.
 * Contract: memory.  Reads POKEY RANDOM (harness seeds it identically per run).
 */
void terrain_gen_A613(void) {
    uint8_t A, c, Y;
    c = 0; A = bus_read(0xD20A);                          /* CLC; LDA $D20A */
    { uint16_t t = (uint16_t)A + bus_read(0xD20A) + c; c = (uint8_t)(t >> 8); A = (uint8_t)t; }  /* ADC $D20A */
    mem[0x2829] = A;
    A = c;                                                /* LDA #0; ROL A -> A = carry */
    if (mem[0x0064] < 0x6C) A ^= 0xFF;                    /* LDY $64; CPY #$6C; BCS skip; EOR #$FF when <$6C */
    mem[0x0068] = A;

    c = 1; Y = 0x00; A = bus_read(0xD20A);                /* SEC; LDY #0; LDA $D20A */
    { uint16_t t = (uint16_t)A + (uint8_t)~0x80 + c; c = (uint8_t)(t >> 8); A = (uint8_t)t; }  /* SBC #$80 */
    if (!c) Y = (uint8_t)(Y - 1);                         /* BCS skip; DEY (borrow -> -1) */
    mem[0x0069] = Y;
    mem[0x282C] = A;
    terrain_plot_return();
}

/* terrain_gen_1 @ $9E54 — terrain/level generation step 1 (flight top #2).
 *
 * Calls native setup_projection_params + build_view_transform_matrix, then runs
 * two loops over the per-column transform tables ($22A3/$22D1/$22FF/$232D etc.):
 *  - LOOP 1 (Y=0..$2C): per terrain map cell read via ($80),Y, rotate the cell's
 *    bit pattern ($B5) to pick one of six rotate/translate updates of the column
 *    vectors, derive the screen-X ($2388/$235B) and visibility class ($24B4).
 *  - LOOP 2 (Y=0..$0B): walk the object draw order $B67C[], collapse adjacent
 *    visible/hidden pairs (clear $232E/set $2300=$20/clear $24B4) per the class.
 *
 * Goto-faithful transliteration: locals A/X/Y/c; X carries cross-iteration state
 * in loop 1 (rebuilt at $9FB9 as (X&$0F)|$B6).  Carry into the top ROL A chain and
 * into ROL $B5 is irrelevant (masked / discarded low bits) but threaded anyway.
 * Contract: memory only (the main flight loop reloads regs after the call).
 */
void terrain_gen_1(void) {
    uint8_t A, X, Y, c;
    #define ADC_(v)  do { uint16_t _t=(uint16_t)A+(uint8_t)(v)+c; c=(uint8_t)(_t>>8); A=(uint8_t)_t; } while(0)
    #define SBC_(v)  ADC_((uint8_t)~(uint8_t)(v))
    #define ROLA_()  do { uint8_t _n=A>>7; A=(uint8_t)((A<<1)|c); c=_n; } while(0)
    #define LSRA_()  do { c=A&1; A=(uint8_t)(A>>1); } while(0)
    #define ROLM_(a) do { uint8_t _v=mem[a],_n=_v>>7; mem[a]=(uint8_t)((_v<<1)|c); c=_n; } while(0)
    #define RORM_(a) do { uint8_t _v=mem[a],_n=_v&1; mem[a]=(uint8_t)((_v>>1)|(c<<7)); c=_n; } while(0)

    setup_projection_params();                           /* 9e54 */
    build_view_transform_matrix();                       /* 9e57 */
    c = cpu.C;
    A = mem[0x0092];                                     /* 9e5a */
    if (A & 0x80) goto L_9e85;                            /* 9e5c BMI */
    ROLA_();                                             /* 9e5e */
    if (A & 0x80) goto L_9e7f;                            /* 9e5f BMI */
    mem[0x22A4] = mem[0x22A3];                            /* 9e61 (only this bit7=0,bit6=0 path copies) */
    mem[0x22D2] = mem[0x22D1];
    mem[0x2300] = mem[0x22FF];
    mem[0x232E] = mem[0x232D];
    X = 0x22; Y = 0xB6;                                  /* 9e79/9e7b */
    goto L_9e92;                                         /* 9e7d BNE (always) */
L_9e7f:
    X = 0x4F; Y = 0xB6;                                  /* 9e7f/9e81 */
    goto L_9e92;                                         /* 9e83 BNE */
L_9e85:
    ROLA_();                                             /* 9e85 */
    if (A & 0x80) goto L_9e8e;                            /* 9e86 BMI */
    X = 0xF5; Y = 0xB5;                                  /* 9e88/9e8a */
    goto L_9e92;                                         /* 9e8c BNE */
L_9e8e:
    X = 0xC8; Y = 0xB5;                                  /* 9e8e/9e90 */
L_9e92:
    mem[0x0080] = X; mem[0x0081] = Y;                    /* 9e92/9e94 (ptr for ($80),Y) */
    A = mem[0x008A]; ROLA_(); ROLA_(); ROLA_(); ROLA_(); /* 9e96-9e9b */
    A &= 0xF0; mem[0x00B6] = A;                          /* 9e9c/9e9e */
    A = mem[0x0088]; A &= 0x0F; X = A;                   /* 9ea0-9ea4 (TAX) */
    A |= mem[0x00B5]; mem[0x00B4] = A;                   /* 9ea5/9ea7 */
    Y = 0x00;                                            /* 9ea9 */
L_9eab:
    A = bus_read((uint16_t)(mem[0x0080] | (mem[0x0081] << 8)) + Y);  /* 9eab LDA ($80),Y */
    mem[0x00B5] = A;                                     /* 9ead */
    if (!(A & 0x80)) goto L_9edd;                         /* 9eaf BPL */
    A = mem[0x00B6]; c = 0; ADC_(0xF0); mem[0x00B6] = A;  /* 9eb1-9eb6 */
    c = 0; A = mem[0x22A3 + Y]; ADC_(mem[0x00A0]); mem[0x22A4 + Y] = A;  /* 9eb8-9ebe */
    A = mem[0x22D1 + Y]; ADC_(mem[0x00A1]); mem[0x22D2 + Y] = A;         /* 9ec1-9ec6 */
    c = 1; A = mem[0x22FF + Y]; SBC_(mem[0x00A2]); mem[0x2300 + Y] = A;  /* 9ec9-9ecf */
    A = mem[0x232D + Y]; SBC_(mem[0x00A3]); mem[0x232E + Y] = A;         /* 9ed2-9ed7 */
    goto L_9f61;                                         /* 9eda */
L_9edd:
    ROLM_(0x00B5);                                       /* 9edd */
    if (!(mem[0x00B5] & 0x80)) goto L_9f0d;               /* 9edf BPL */
    A = mem[0x00B6]; c = 0; ADC_(0x10); mem[0x00B6] = A;  /* 9ee1-9ee6 */
    c = 1; A = mem[0x22A3 + Y]; SBC_(mem[0x00A0]); mem[0x22A4 + Y] = A;  /* 9ee8-9eee */
    A = mem[0x22D1 + Y]; SBC_(mem[0x00A1]); mem[0x22D2 + Y] = A;         /* 9ef1-9ef6 */
    c = 0; A = mem[0x22FF + Y]; ADC_(mem[0x00A2]); mem[0x2300 + Y] = A;  /* 9ef9-9eff */
    A = mem[0x232D + Y]; ADC_(mem[0x00A3]); mem[0x232E + Y] = A;         /* 9f02-9f07 */
    goto L_9f61;                                         /* 9f0a */
L_9f0d:
    ROLM_(0x00B5);                                       /* 9f0d */
    if (!(mem[0x00B5] & 0x80)) goto L_9f37;               /* 9f0f BPL */
    X = (uint8_t)(X - 1);                                /* 9f11 DEX */
    c = 1; A = mem[0x22A3 + Y]; SBC_(mem[0x00A2]); mem[0x22A4 + Y] = A;  /* 9f12-9f18 */
    A = mem[0x22D1 + Y]; SBC_(mem[0x00A3]); mem[0x22D2 + Y] = A;         /* 9f1b-9f20 */
    c = 1; A = mem[0x22FF + Y]; SBC_(mem[0x00A0]); mem[0x2300 + Y] = A;  /* 9f23-9f29 */
    A = mem[0x232D + Y]; SBC_(mem[0x00A1]); mem[0x232E + Y] = A;         /* 9f2c-9f31 */
    goto L_9fb2;                                         /* 9f34 */
L_9f37:
    ROLM_(0x00B5);                                       /* 9f37 */
    if (!(mem[0x00B5] & 0x80)) goto L_9fb2;               /* 9f39 BPL */
    X = (uint8_t)(X + 1);                                /* 9f3b INX */
    c = 0; A = mem[0x22A3 + Y]; ADC_(mem[0x00A2]); mem[0x22A4 + Y] = A;  /* 9f3c-9f42 */
    A = mem[0x22D1 + Y]; ADC_(mem[0x00A3]); mem[0x22D2 + Y] = A;         /* 9f45-9f4a */
    c = 0; A = mem[0x22FF + Y]; ADC_(mem[0x00A0]); mem[0x2300 + Y] = A;  /* 9f4d-9f53 */
    A = mem[0x232D + Y]; ADC_(mem[0x00A1]); mem[0x232E + Y] = A;         /* 9f56-9f5b */
    goto L_9fb2;                                         /* 9f5e */
L_9f61:
    ROLM_(0x00B5);                                       /* 9f61 */
    if (!(mem[0x00B5] & 0x80)) goto L_9f8b;               /* 9f63 BPL */
    X = (uint8_t)(X - 1);                                /* 9f65 DEX */
    c = 1; A = mem[0x22A4 + Y]; SBC_(mem[0x00A2]); mem[0x22A4 + Y] = A;  /* 9f66-9f6c */
    A = mem[0x22D2 + Y]; SBC_(mem[0x00A3]); mem[0x22D2 + Y] = A;         /* 9f6f-9f74 */
    c = 1; A = mem[0x2300 + Y]; SBC_(mem[0x00A0]); mem[0x2300 + Y] = A;  /* 9f77-9f7d */
    A = mem[0x232E + Y]; SBC_(mem[0x00A1]); mem[0x232E + Y] = A;         /* 9f80-9f85 */
    goto L_9fb2;                                         /* 9f88 */
L_9f8b:
    ROLM_(0x00B5);                                       /* 9f8b */
    if (!(mem[0x00B5] & 0x80)) goto L_9fb2;               /* 9f8d BPL */
    X = (uint8_t)(X + 1);                                /* 9f8f INX */
    c = 0; A = mem[0x22A4 + Y]; ADC_(mem[0x00A2]); mem[0x22A4 + Y] = A;  /* 9f90-9f96 */
    A = mem[0x22D2 + Y]; ADC_(mem[0x00A3]); mem[0x22D2 + Y] = A;         /* 9f99-9f9e */
    c = 0; A = mem[0x2300 + Y]; ADC_(mem[0x00A0]); mem[0x2300 + Y] = A;  /* 9fa1-9fa7 */
    A = mem[0x232E + Y]; ADC_(mem[0x00A1]); mem[0x232E + Y] = A;         /* 9faa-9faf */
L_9fb2:
    c = 1; A = 0x00; SBC_(mem[0x008B]); mem[0x00B5] = A;  /* 9fb2-9fb7 */
    A = X; A &= 0x0F; A |= mem[0x00B6]; X = A;            /* 9fb9-9fbe (TXA;AND;ORA;TAX) */
    A = mem[0x0900 + X]; mem[0x23B5 + Y] = A;             /* 9fbf-9fc2 */
    SBC_(mem[0x008C]);                                   /* 9fc5 (carry from 9fb2 chain) */
    if (!c) goto L_9fd8;                                  /* 9fc7 BCC */
    LSRA_(); RORM_(0x00B5); LSRA_(); RORM_(0x00B5);       /* 9fc9-9fcd */
    LSRA_(); RORM_(0x00B5); LSRA_(); RORM_(0x00B5);       /* 9fcf-9fd3 */
    goto L_9fe6;                                          /* 9fd5 */
L_9fd8:
    LSRA_(); RORM_(0x00B5); LSRA_(); RORM_(0x00B5);       /* 9fd8-9fdc */
    LSRA_(); RORM_(0x00B5); LSRA_(); RORM_(0x00B5);       /* 9fde-9fe2 */
    A ^= 0xF0;                                            /* 9fe4 */
L_9fe6:
    mem[0x2388 + Y] = A;                                 /* 9fe6 */
    A = mem[0x00B5]; mem[0x235B + Y] = A;                /* 9fe9-9feb */
    A = X; mem[0x2276 + Y] = A;                          /* 9fee (TXA) */
    A = mem[0x232E + Y];                                 /* 9ff2 */
    if (A & 0x80) goto L_a03b;                            /* 9ff5 BMI */
    if (A != 0) goto L_a000;                              /* 9ff7 BNE */
    A = mem[0x2300 + Y];                                 /* 9ff9 */
    if (A < 0x20) goto L_a03b;                            /* 9ffc CMP #$20; 9ffe BCC */
L_a000:
    A = mem[0x22D2 + Y];                                 /* a000 */
    if (!(A & 0x80)) goto L_a024;                         /* a003 BPL */
    A = 0x00; c = 1; SBC_(mem[0x22A4 + Y]); mem[0x00B5] = A;  /* a005-a00b */
    A = 0x00; SBC_(mem[0x22D2 + Y]);                     /* a00d-a00f */
    {   uint8_t m = mem[0x232E + Y];                     /* a012 CMP $232E,Y */
        if (A < m) goto L_a037;                          /* a015 BCC */
        if (A != m) goto L_a020;                         /* a017 BNE */
    }
    A = mem[0x00B5];                                     /* a019 */
    if (A < mem[0x2300 + Y]) goto L_a037;                /* a01b CMP; a01e BCC */
L_a020:
    A = 0x40; goto L_a03d;                                /* a020/a022 */
L_a024:
    {   uint8_t m = mem[0x232E + Y];                     /* a024 CMP $232E,Y (A = $22D2,Y) */
        if (A < m) goto L_a037;                          /* a027 BCC */
        if (A != m) goto L_a033;                         /* a029 BNE */
    }
    A = mem[0x22A4 + Y];                                 /* a02b */
    if (A < mem[0x2300 + Y]) goto L_a037;                /* a02e CMP; a031 BCC */
L_a033:
    A = 0x20; goto L_a03d;                                /* a033/a035 */
L_a037:
    A = 0x00; goto L_a03d;                                /* a037/a039 (BEQ always) */
L_a03b:
    A = 0x80;                                             /* a03b */
L_a03d:
    mem[0x24B4 + Y] = A;                                 /* a03d */
    Y = (uint8_t)(Y + 1);                                /* a040 INY */
    if (Y != 0x2D) goto L_9eab;                           /* a041 CPY #$2D; a043 BEQ a048 */

    Y = 0x00;                                            /* a048 */
L_a04a:
    X = mem[0xB67C + Y];                                 /* a04a LDX $B67C,Y */
    mem[0x28DB] = X;                                     /* a04d */
    Y = (uint8_t)(Y + 1);                                /* a050 INY */
    A = mem[0x24B4 + X];                                 /* a051 */
    if (A & 0x80) goto L_a079;                            /* a054 BMI */
    if (A != 0) goto L_a09a;                              /* a056 BNE */
    X = mem[0xB67C + Y];                                 /* a058 */
    Y = (uint8_t)(Y + 1);                                /* a05b INY */
    A = mem[0x24B4 + X];                                 /* a05c */
    if (!(A & 0x80)) goto L_a09b;                         /* a05f BPL */
    mem[0x00B5] = Y;                                     /* a061 STY $B5 */
    Y = mem[0x28DB];                                     /* a063 */
    mem[0x232E + X] = 0x00;                              /* a066-a068 */
    mem[0x2300 + X] = 0x20;                              /* a06b-a06d */
    mem[0x24B4 + X] = 0x00;                              /* a070-a072 */
    Y = mem[0x00B5];                                     /* a075 */
    if (Y != 0) goto L_a09b;                              /* a077 BNE */
L_a079:
    X = mem[0xB67C + Y];                                 /* a079 */
    Y = (uint8_t)(Y + 1);                                /* a07c INY */
    A = mem[0x24B4 + X];                                 /* a07d */
    if (A != 0) goto L_a09b;                              /* a080 BNE */
    mem[0x00B5] = Y;                                     /* a082 STY $B5 */
    Y = mem[0x28DB];                                     /* a084 */
    mem[0x232E + Y] = 0x00;                              /* a087-a089 (indexed by Y=$28DB) */
    mem[0x2300 + Y] = 0x20;                              /* a08c-a08e */
    mem[0x24B4 + Y] = 0x00;                              /* a091-a093 */
    Y = mem[0x00B5];                                     /* a096 */
    if (Y != 0) goto L_a09b;                              /* a098 BNE */
L_a09a:
    Y = (uint8_t)(Y + 1);                                /* a09a INY */
L_a09b:
    if (Y >= 0x0C) return;                                /* a09b CPY #$0C; a09d BCS a0a2 */
    goto L_a04a;                                         /* a09f */

    #undef ADC_
    #undef SBC_
    #undef ROLA_
    #undef LSRA_
    #undef ROLM_
    #undef RORM_
}
