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
