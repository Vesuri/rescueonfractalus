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
#include "rof_native.h" /* typed cores shared with the hand-written Amiga ports */

/* ---------------------------------------------------------------------------
 * Idiomatic-C migration seam.
 *
 * Each VALIDATE_FUNCS routine is split into two halves:
 *   - a TYPED CORE `<canonical>_core` (e.g. divide_16x16_core,
 *     clear_terrain_column_core) that takes real C parameters / returns real
 *     values — the form we want all native code to converge on; and
 *   - a 6502-ABI SHIM keeping the transpiler-mandated `void name(void)` symbol
 *     that transpiled callers (rof_gen.c), the validate harness, and the
 *     register/memory calling convention bind to.  The shim just marshals
 *     mem[]/cpu <-> the core.
 * Hand-written native callers (the Amiga C++ ports, and the cores here) call
 * the TYPED CORE directly.  When a routine's last transpiled caller is shed, its
 * shim + VALIDATE_FUNCS entry can be deleted, leaving pure C.
 * ------------------------------------------------------------------------- */
static inline uint16_t rd16(uint16_t a) { return (uint16_t)(mem[a] | (mem[a + 1] << 8)); }
static inline void     wr16(uint16_t a, uint16_t v) { mem[a] = (uint8_t)v; mem[a + 1] = (uint8_t)(v >> 8); }

/* divide_16x16 @ $9D6F — restoring 16-bit divide, expressed as one native divide.
 *
 * Inputs : dividend $00B0(lo)/$00B1(hi), divisor $00AE(lo)/$00AF(hi),
 *          quotient accumulator $00B2 (its 8 bits are fully shifted out).
 * Outputs: remainder -> $00B0/$00B1, quotient -> $00B2,
 *          divisor left shifted by the normalization count -> $00AE/$00AF.
 * Domain : divisor in [0x0001, 0x7FFF] AND dividend < divisor.  The sole caller
 *          (project_terrain_points) scales the divisor up until it strictly
 *          exceeds the dividend before calling, so this always holds in-game; it
 *          is also exactly the domain in which the 8-bit quotient does not overflow.
 *
 * The 6502 routine normalizes by shifting BOTH operands left k times until the
 * divisor's top set bit reaches bit14, then runs 8 ROL-quotient (restoring
 * shift-subtract) steps.  Because dividend < divisor, the normalization scales
 * both by 2^k with no bit loss, so the whole thing reduces to:
 *
 *     quotient  $B2      = (dividend * 256) / divisor          (< 256)
 *     remainder $B0/$B1  = ((dividend * 256) % divisor) << k   (the 6502 leaves
 *                          the remainder still scaled by the normalization count)
 *     divisor   $AE/$AF  = divisor << k                        (shifted in place)
 *
 * Both operands are non-negative (divisor in [1,0x7FFF]; the caller pre-absolutes
 * the dividend and tracks sign separately), so this is an UNSIGNED divide: the
 * division/modulo is a single 68000 DIVU (not DIVS).  Only the tiny normalization
 * count loop remains (it just positions the side outputs).  The 6502 register/flag
 * exit state is intentionally NOT reproduced — both call sites save Y through
 * $009F and overwrite A/flags immediately (verified incidental/dead).
 */
/* Typed core: see rof_native.h for the contract. */
DivResult divide_16x16_core(uint16_t dividend, uint16_t divisor) {
    /* Normalization count k: divisor << k brings its top set bit to bit14. */
    unsigned k = 0;
    while (!((divisor << k) & 0x4000)) k++;

    uint32_t numerator = (uint32_t)dividend << 8;
    DivResult r;
    r.quotient        = (uint8_t)(numerator / divisor);            /* < 256 */
    r.remainder       = (uint16_t)((numerator % divisor) << k);
    r.shifted_divisor = (uint16_t)(divisor << k);
    return r;
}

/* 6502-ABI shim: dividend $B0/$B1, divisor $AE/$AF in; remainder -> $B0/$B1,
 * quotient -> $B2, shifted divisor -> $AE/$AF out.  (Exit cpu state is dead at
 * both call sites — see header note above — so it is intentionally untouched.) */
void divide_16x16(void) {
    DivResult r = divide_16x16_core(rd16(0x00B0), rd16(0x00AE));
    wr16(0x00B0, r.remainder);
    mem[0x00B2] = r.quotient;
    wr16(0x00AE, r.shifted_divisor);
}

/* clear_terrain_column @ $AD5F — clear one terrain column band + its object-table cells.
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
/* Typed core: see rof_native.h. */
void clear_terrain_column_core(uint8_t startCol) {
    mem[0x0094] = startCol;                       /* $AD5F: STX $0094 (save column) */

    /* $AD61-$ADEF: 42 columns ($2A) x 44 rows (base $1010, stride $60), all 0. */
    for (uint8_t i = 0; i < 0x2A; i++) {
        uint8_t x = (uint8_t)(startCol + i);      /* INX wraps as a byte */
        for (uint16_t row = 0x1010; row <= 0x2030; row += 0x60)
            mem[row + x] = 0x00;
    }

    /* $ADF0-$AE52: scattered object-table cells, indexed by the ORIGINAL column. */
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
        mem[cells[k] + startCol] = 0x00;
}

/* 6502-ABI shim: cpu.X = starting column in.  Exit cpu state (A=0, Y=0,
 * X=original, N/Z per LDX X) is dead at both call sites but reproduced so the
 * validation harness shows zero incidental CPU drift. */
void clear_terrain_column(void) {
    uint8_t x0 = cpu.X;
    clear_terrain_column_core(x0);
    cpu.A = 0x00; cpu.Y = 0x00; cpu.X = x0;
    cpu.Z = (x0 == 0) ? 1 : 0; cpu.N = (x0 >> 7) & 1;
}

/* render_bcd_counter @ $49A0 — render the 3-byte packed-BCD score ($0601-$0603,
 * 6 digits) to the top text line $32C5..$32CA with leading-zero suppression.
 * Flight ISR routine; the first transpiled-on-the-VBI-path fn ported native.
 *
 * Faithfully reproduces the self-contained fall-through chain $49A0-$49ED
 * (render_bcd_top_byte/_low_bytes/emit_bcd_byte_digits/plot_char_bounded): a
 * running X flag ($0600 entry) suppresses leading zeros until the first nonzero
 * digit; digit column Y < threshold $0619(=5) keeps a zero blank, else it is
 * forced (so a zero score still shows one "0").  Char code = digit + $50.
 * Contract: memory only — exit cpu is dead (called from flight_vbi_native).  The
 * 6502's emit_bcd_byte_digits PHA/PLA leaves a dead scribble at $01FF (S=$FF in
 * the harness); excluded from this fn's contract via set_ignore (validate_native.c).
 */
void render_bcd_counter(void) {
    mem[0x00C5] = 0xC5; mem[0x00C6] = 0x32;      /* $49A0: dest ptr $32C5 */
    uint8_t x = mem[0x0600];                      /* $49C2: LDX $0600 (running suppress flag) */
    mem[0x0619] = 0x05;                           /* $49C5: STY $0619 (zero-suppress threshold) */
    mem[0x0045] = 0x00; mem[0x0046] = 0x00;       /* $49C8-$49CC: clear 16-bit delta */
    uint8_t y = 0;
    static const uint16_t bcd[3] = { 0x0601, 0x0602, 0x0603 };
    for (int i = 0; i < 3; i++) {
        uint8_t v = mem[bcd[i]];
        uint8_t nib[2] = { (uint8_t)(v >> 4), (uint8_t)(v & 0x0F) };  /* hi then lo nibble */
        for (int n = 0; n < 2; n++) {
            uint8_t a = nib[n];
            int write;                            /* plot_char_bounded $49D9 */
            if      (x != 0)              write = 1;            /* nonzero seen -> always write */
            else if (a != 0)           { x++; write = 1; }      /* first nonzero digit */
            else if (y < mem[0x0619])    write = 0;             /* leading zero below thresh -> blank */
            else                       { x++; write = 1; }      /* forced write at/after thresh */
            if (write) mem[0x32C5 + y] = (uint8_t)(a + 0x50);
            y++;
        }
    }
}

/* init_proj_scratch_pointers @ $9B87 — set game_state=1 and 3 ZP scratch/ptr bytes.
 * Trivial leaf of the update_terrain_scanline_proj subtree.  Memory-only contract. */
void init_proj_scratch_pointers(void) {
    mem[0x0041] = 0x01;   /* game_state = 1 */
    mem[0x00DD] = 0x3C;
    mem[0x00DC] = 0x38;
    mem[0x00DA] = 0x34;
}

/* ring_push_marked @ $5815 — push (entry X)|$80 into the $0719 event ring; X preserved.
 * game_sub_55FC @ $55FC — push entry Y into the ring; X preserved.
 * Both are stack-aware: the 6502 PHAs the saved index, then ring_push_0719 (native)
 * does PLA;TAX to hand it back, so cpu.A/X/S AND the $01xx stack byte are part of the
 * contract.  Mirroring the exact 6502 ops via the cpu.h macros keeps them bit-identical
 * (these are 3-7 byte routines — the perf win is in the larger subtree members). */
void ring_push_marked(void) {
    TXA(); PHA(); ORA(0x80); ring_push_0719();   /* $5815-$5819 */
}
void game_sub_55FC(void) {
    TXA(); PHA(); TYA(); ring_push_0719();        /* $55FC-$55FE -> $55FF */
}

/* sample_terrain_height_bilerp @ $9A36 — bilinear-sample the 16x16 height map $0900.
 *
 * Fetch 4 corner heights into $27F0-$27F3 (row = $2800<<4, col = $27FE low nibble,
 * X wraps within the nibble, row +$10 for the second), then 3 bit-serial fractional
 * blends ($27FD horizontally twice -> $27F4/$27F5, then $27FF vertically -> R), then
 * scale R += R>>4 + R>>5 into $0062.  $27F0-$27F5/$27FA/$27FB are shifted in place
 * (scratch, part of the mem contract).  Bounded 8-iter loops -> random mem is safe.
 *
 * The blend's ADC adds the byte AFTER an in-place LSR plus the bit that LSR shifted
 * out (rounding) — reproduced exactly. */
static uint8_t terr_blend(uint16_t fa, uint16_t lo, uint16_t hi) {
    uint8_t A = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t f = mem[fa];
        uint8_t bit = (uint8_t)(f >> 7);
        mem[fa] = (uint8_t)(f << 1);                 /* ASL fraction -> carry=bit */
        if (bit) {                                    /* B1: LSR lo; LSR hi; ADC hi */
            mem[lo] = (uint8_t)(mem[lo] >> 1);        /* LSR lo (carry discarded) */
            uint8_t h = mem[hi], c = (uint8_t)(h & 1);
            mem[hi] = (uint8_t)(h >> 1);
            A = (uint8_t)(A + mem[hi] + c);           /* ADC hi (+ shifted-out bit) */
        } else {                                      /* bit0: LSR hi; LSR lo; ADC lo */
            mem[hi] = (uint8_t)(mem[hi] >> 1);
            uint8_t l = mem[lo], c = (uint8_t)(l & 1);
            mem[lo] = (uint8_t)(l >> 1);
            A = (uint8_t)(A + mem[lo] + c);           /* ADC lo */
        }
    }
    return A;
}
void sample_terrain_height_bilerp(void) {
    uint8_t row = (uint8_t)(mem[0x2800] << 4);                       /* $9A36: $0061 */
    mem[0x0061] = row;
    uint8_t y = (uint8_t)((mem[0x27FE] & 0x0F) | row);
    mem[0x27F0] = mem[0x0900 + y];
    y = (uint8_t)(((uint8_t)(y + 1) & 0x0F) | row);
    mem[0x27F1] = mem[0x0900 + y];
    row = (uint8_t)(row + 0x10); mem[0x0061] = row;                  /* $0061 += $10 */
    y = (uint8_t)((mem[0x27FE] & 0x0F) | row);
    mem[0x27F2] = mem[0x0900 + y];
    y = (uint8_t)(((uint8_t)(y + 1) & 0x0F) | row);
    mem[0x27F3] = mem[0x0900 + y];

    mem[0x27FA] = mem[0x27FD]; mem[0x27F4] = terr_blend(0x27FA, 0x27F0, 0x27F1);
    mem[0x27FA] = mem[0x27FD]; mem[0x27F5] = terr_blend(0x27FA, 0x27F2, 0x27F3);
    mem[0x27FB] = mem[0x27FF]; uint8_t R = terr_blend(0x27FB, 0x27F4, 0x27F5);

    mem[0x0062] = R;                                                 /* $9AFA */
    uint8_t hi4 = (uint8_t)(R >> 4);                                 /* LSRx4 / TAY */
    mem[0x0062] = (uint8_t)(hi4 + mem[0x0062]);                      /* CLC; ADC $0062 */
    uint8_t c = (uint8_t)(hi4 & 1);                                  /* TYA; LSR -> carry */
    mem[0x0062] = (uint8_t)((hi4 >> 1) + mem[0x0062] + c);           /* ADC $0062 */
}

/* game_sub_451d @ $451D — fill 14 cells of $2159+Y / $2189+Y from table $4553[X],
 * with X/$BD evolving per the $BB threshold.  Self-contained loop (no callees).
 * Inputs: cpu.A/X/Y.  Memory-only contract (validated with random entry regs).
 */
void game_sub_451d(void) {
    uint8_t A = cpu.A, X = cpu.X, Y = cpu.Y;
    mem[0x00BB] = A;                                   /* $451D STA $BB (entry A) */
    mem[0x00BD] = X;                                   /* $451F STX $BD */
    A = (uint8_t)((A & 0x03) | mem[0x00BD]);           /* $4521 AND#3; ORA $BD */
    mem[0x00BD] = A;                                   /* $4525 STA $BD */
    A = (uint8_t)((A & 0x04) ^ 0x04);                  /* $4527 AND#4; EOR#4 */
    X = A;                                             /* $452B TAX */
    mem[0x00BB] = (uint8_t)(mem[0x00BB] >> 2);         /* $452C LSR $BB; LSR $BB */
    A = (uint8_t)(Y | mem[0x00BB]);                    /* $4530 TYA; ORA $BB */
    mem[0x00BB] = A;                                   /* $4533 STA $BB */
    mem[0x00BF] = 0x0E;                                /* $4535 loop count 14 */
    do {
        if (Y >= mem[0x00BB]) {                        /* $4539 CPY $BB; BCC skip */
            X = mem[0x00BD];                           /* $453D LDX $BD */
            mem[0x00BD] = (uint8_t)(X & 0x04);         /* $453F TXA; AND#4; STA $BD */
        }
        uint8_t v = mem[0x4553 + X];                   /* $4544 LDA $4553,X */
        mem[0x2159 + Y] = v;                           /* $4547 STA $2159,Y */
        mem[0x2189 + Y] = v;                           /* $454A STA $2189,Y */
        Y = (uint8_t)(Y + 1);                          /* $454D INY */
        mem[0x00BF] = (uint8_t)(mem[0x00BF] - 1);      /* $454E DEC $BF */
    } while (mem[0x00BF] != 0);                        /* $4550 BNE */
}

/* enter_terrain_special_state @ $9B0D — set $2877/$3355 + (gated on $062F/$066C)
 * seed flags $0688/$0689/$0696/$0697/$06A4/$06A5 and push 2 ring events, then
 * tail ring_push_marked(X=$13).  exit_terrain_special_state @ $9B4C is the inverse.
 * Both call the native game_sub_55FC/ring_push_marked (cpu.X preserved across them,
 * ring content = cpu.Y).  Memory contract; validated with random entry regs + mem. */
void enter_terrain_special_state(void) {
    mem[0x2877] = (uint8_t)(mem[0x002D] | 0x40);          /* $9B0D */
    if (mem[0x062F] == 0) return;                         /* $9B14 BNE; else RTS */
    mem[0x3355] = 0x34;                                   /* $9B1A */
    if (mem[0x066C] != 0x0A) {                            /* $9B1F CMP $066C; BEQ skip */
        mem[0x0696] = 0x0A; mem[0x0697] = 0x0A;           /* $9B26 */
        mem[0x0688] = 1; mem[0x0689] = 1;                 /* $9B2E */
        mem[0x06A4] = 1; mem[0x06A5] = 1;                 /* $9B34 */
        cpu.Y = 1; game_sub_55FC();                       /* $9B3A push Y=1 */
        cpu.Y = (uint8_t)(cpu.Y + 1); game_sub_55FC();    /* $9B3D INY; $9B3E push Y=2 */
    }
    if (mem[0x002E] != 0) mem[0x002E] = (uint8_t)(mem[0x002E] - 1);  /* $9B41 */
    cpu.X = 0x13; ring_push_marked();                     /* $9B47 LDX #$13; JMP $5815 */
}
void exit_terrain_special_state(void) {
    uint8_t s = mem[0x066C];                              /* $9B4C */
    if (s == 0) return;                                   /* BEQ RTS */
    if (s < 0x02) {                                       /* $9B51 CMP #2; BCS skip-gate */
        if (mem[0x06A4] == 0) return;                     /* $9B55 */
        if (mem[0x0696] == 0) return;                     /* $9B5A */
    }
    mem[0x3355] = 0xB4;                                   /* $9B5F */
    mem[0x2877] = 0; mem[0x0696] = 0; mem[0x0697] = 0;    /* $9B64 */
    mem[0x0688] = 0xFF; mem[0x0689] = 0xFF;               /* $9B6F */
    mem[0x06A4] = 1; mem[0x06A5] = 1;                     /* $9B77 */
    cpu.Y = 1; game_sub_55FC();                           /* $9B7F push Y=1 */
    cpu.Y = (uint8_t)(cpu.Y + 1); game_sub_55FC();        /* $9B82 INY; $9B83 push Y=2 */
}

/* {hi:mem[lo]} = signed({hi:mem[lo]}) >> 4 (arithmetic), via 4x (CMP #$80; ROR A;
 * ROR lo) — the 6502 sign-extending shift used by update_terrain_horizon_lr. */
static uint8_t asr16_4(uint8_t hi, uint16_t lo) {
    for (int i = 0; i < 4; i++) {
        uint8_t cin  = (hi >= 0x80) ? 1 : 0;        /* CMP #$80 -> C = sign */
        uint8_t cout = (uint8_t)(hi & 1);           /* ROR A: bit0 out */
        hi = (uint8_t)((cin << 7) | (hi >> 1));     /* ROR A */
        uint8_t l = mem[lo];
        mem[lo] = (uint8_t)((cout << 7) | (l >> 1)); /* ROR lo */
    }
    return hi;
}

/* update_terrain_horizon_lr @ $992D — alternately ($2834/$2833 toggles) update the
 * left ($282E) or right ($282F) horizon: build map coords {$27FD..$2800} from
 * {$2801..$2804} +/- the >>4 deltas {$2805..$2808} (from $2809-$280C), sample the
 * height map, derive the horizon row, and on a change cache it ($2841/$2842) and
 * repaint via game_sub_451d.  All callees native; bounded loops -> random mem safe. */
void update_terrain_horizon_lr(void) {
    /* $992D: $2834 every-other gate */
    uint8_t v = mem[0x2834], c = (uint8_t)(v & 1);
    mem[0x2834] = (uint8_t)(v >> 1);
    if (!c) { mem[0x2834] = (uint8_t)(mem[0x2834] + 1); return; }

    /* $9936: {$2806:$2805} = asr4({$280A:$2809}); {$2808:$2807} = asr4({$280C:$280B}) */
    mem[0x2805] = mem[0x2809]; mem[0x2806] = asr16_4(mem[0x280A], 0x2805);
    mem[0x2807] = mem[0x280B]; mem[0x2808] = asr16_4(mem[0x280C], 0x2807);

    /* $9964: $2833 toggle -> second (right) half if odd */
    uint8_t v2 = mem[0x2833], c2 = (uint8_t)(v2 & 1);
    mem[0x2833] = (uint8_t)(v2 >> 1);
    int second = c2;
    if (!second) mem[0x2833] = (uint8_t)(mem[0x2833] + 1);

    uint16_t c2801 = (uint16_t)(mem[0x2801] | (mem[0x2802] << 8));
    uint16_t c2803 = (uint16_t)(mem[0x2803] | (mem[0x2804] << 8));
    uint16_t d05   = (uint16_t)(mem[0x2805] | (mem[0x2806] << 8));
    uint16_t d07   = (uint16_t)(mem[0x2807] | (mem[0x2808] << 8));

    if (!second) {
        /* $996C: coords = {2801}-{d07}, {2803}+{d05} */
        uint16_t m = (uint16_t)(c2801 - d07), p = (uint16_t)(c2803 + d05);
        mem[0x27FD] = (uint8_t)m; mem[0x27FE] = (uint8_t)(m >> 8);
        mem[0x27FF] = (uint8_t)p; mem[0x2800] = (uint8_t)(p >> 8);
        sample_terrain_height_bilerp();                 /* $9992 */
        mem[0x0062] = (uint8_t)(mem[0x0062] >> 1);       /* $9995 LSR $0062 */
        mem[0x27F7] = (uint8_t)(mem[0x0062] >> 1);       /* $27F7 = $0062>>1 */
        uint8_t A = (uint8_t)(mem[0x2275] >> 1);         /* $999D */
        A = (uint8_t)(A + mem[0x0024]);                  /* CLC; ADC $0024 */
        A = (uint8_t)(A + 0x0F);                         /* CLC; ADC #$0F */
        uint8_t sub = mem[0x0062];
        uint8_t r = (A >= sub) ? (uint8_t)(A - sub) : 0; /* SEC; SBC $0062; BCS/clamp0 */
        mem[0x27F6] = r;
        uint8_t h = (0x38 >= r) ? (uint8_t)(0x38 - r) : 0;  /* $38 - $27F6, clamp0 */
        mem[0x282E] = h;
        if (h == mem[0x2841]) return;                    /* $99BE CMP $2841; BEQ */
        mem[0x2841] = h;
        cpu.A = h; cpu.X = 0; cpu.Y = 0; game_sub_451d(); /* $99C6 X=0,Y=0; JMP $451D */
    } else {
        /* $99CE: coords = {2801}+{d07}, {2803}-{d05} */
        uint16_t m = (uint16_t)(c2801 + d07), p = (uint16_t)(c2803 - d05);
        mem[0x27FD] = (uint8_t)m; mem[0x27FE] = (uint8_t)(m >> 8);
        mem[0x27FF] = (uint8_t)p; mem[0x2800] = (uint8_t)(p >> 8);
        sample_terrain_height_bilerp();                 /* $99F4 */
        mem[0x0062] = (uint8_t)(mem[0x0062] >> 1);       /* $99F7 LSR $0062 */
        mem[0x27F8] = (uint8_t)(mem[0x0062] >> 1);       /* $27F8 = $0062>>1 */
        uint8_t A = (uint8_t)(mem[0x2275] >> 1);         /* $99FF */
        A = (uint8_t)(A - mem[0x0024]);                  /* SEC; SBC $0024 */
        A = (uint8_t)(A + 0x0F);                         /* CLC; ADC #$0F */
        uint8_t sub = mem[0x0062];
        uint8_t r = (A >= sub) ? (uint8_t)(A - sub) : 0; /* SEC; SBC $0062; clamp0 */
        mem[0x27F6] = r;
        uint8_t h = (0x38 >= r) ? (uint8_t)(0x38 - r) : 0;
        mem[0x282F] = h;
        if (h == mem[0x2842]) return;                    /* $9A20 CMP $2842; BEQ */
        mem[0x2842] = h;
        cpu.X = 4; cpu.Y = 0x10;                          /* $9A28 X=4,Y=$10 */
        cpu.A = (uint8_t)(0x38 - mem[0x282F]);            /* SEC; LDA #$38; SBC $282F */
        game_sub_451d();                                  /* JMP $451D */
    }
}

/* update_terrain_scanline_proj @ $9833 — TOP of the flight-VBI projection subtree.
 * Build 16-bit map coords {$27FD..$2800}/{$2801..$2804} from world pos $2887-$288A
 * (>>4) and depth $0033/$0034 (<<2 into $2274/$2275), sample the height, advance the
 * depth row with enter/exit terrain-special-state transitions, finalize the visible
 * span $281A/$281B, update the L/R horizon, and run the $066C-gated state machine.
 * All callees native; bounded -> random mem safe.  Memory contract (no entry regs read). */
void update_terrain_scanline_proj(void) {
    /* $9833: map X = {$2888:$2887} >> 4 ; map Z = {$288A:$2889} >> 4 (logical) */
    uint16_t sx = (uint16_t)((mem[0x2887] | (mem[0x2888] << 8)) >> 4);
    uint8_t sx_lo = (uint8_t)sx, sx_hi = (uint8_t)(sx >> 8);
    mem[0x2270] = sx_lo; mem[0x27FD] = sx_lo; mem[0x2801] = sx_lo;
    mem[0x2271] = sx_hi; mem[0x27FE] = sx_hi; mem[0x2802] = sx_hi;
    uint16_t sz = (uint16_t)((mem[0x2889] | (mem[0x288A] << 8)) >> 4);
    uint8_t sz_lo = (uint8_t)sz, sz_hi = (uint8_t)(sz >> 8);
    mem[0x2272] = sz_lo; mem[0x27FF] = sz_lo; mem[0x2803] = sz_lo;
    mem[0x2273] = sz_hi; mem[0x2800] = sz_hi; mem[0x2804] = sz_hi;

    /* $9889: depth {$2275:$2274} = ({clamp($0034,$3F)}:$0033) << 2 */
    uint8_t d34 = mem[0x0034]; if (d34 >= 0x40) d34 = 0x3F;
    uint8_t a33 = mem[0x0033], cc;
    cc = (uint8_t)(a33 >> 7); a33 = (uint8_t)(a33 << 1); d34 = (uint8_t)((d34 << 1) | cc);
    cc = (uint8_t)(a33 >> 7); a33 = (uint8_t)(a33 << 1); d34 = (uint8_t)((d34 << 1) | cc);
    mem[0x2275] = d34; mem[0x2274] = a33;

    /* $98A1: sample + $27F9 = round(round($0062/2)/2) */
    sample_terrain_height_bilerp();
    { uint8_t a = mem[0x0062];
      a = (uint8_t)((a >> 1) + (a & 1));
      a = (uint8_t)((a >> 1) + (a & 1));
      mem[0x27F9] = a; }
    { uint8_t r9 = mem[0x27F9]; mem[0x281A] = (0x37 >= r9) ? (uint8_t)(0x37 - r9) : 0; }

    /* $98BC: depth advance + special-state transitions */
    uint8_t v2275 = mem[0x2275], v62 = mem[0x0062];
    if (v2275 >= v62) {                                  /* no borrow */
        mem[0x0070] = (uint8_t)(v2275 - v62);
        if (mem[0x283C] == 0) exit_terrain_special_state();   /* $98C9 BNE skip */
    } else {                                             /* $98D1 borrow */
        if (mem[0x0072] != 0) enter_terrain_special_state();  /* $98D5 BEQ skip */
        mem[0x0034] = (uint8_t)(mem[0x0034] + 1);
        mem[0x0033] = 0; mem[0x0070] = 0;
        uint8_t a29 = mem[0x0029];
        if (a29 & 0x80) { mem[0x0029] = (uint8_t)(a29 + 1); mem[0x0028] = 0; }
    }

    /* $98EC: visible span $281A/$281B from depth $0034 */
    { uint8_t d = mem[0x0034];
      if (0x37 >= d) {
          mem[0x281B] = (uint8_t)(0x37 - d);
      } else {
          uint8_t A = (uint8_t)(~(uint8_t)(0x37 - d));          /* EOR #$FF of wrapped SBC */
          A = (uint8_t)((uint16_t)A + mem[0x281A] + 1);          /* SEC; ADC $281A */
          if (A >= 0x38) A = 0x38;
          mem[0x281A] = A; mem[0x281B] = 0;
      } }

    update_terrain_horizon_lr();                          /* $9907 */

    /* $990A: $066C-gated state machine */
    uint8_t v66c = mem[0x066C];
    if (v66c >= 0x08) {                                   /* CMP #8; >=8 */
        mem[0x2879] = 1;
        init_proj_scratch_pointers();                     /* sets $0041=1 */
        return;
    }
    if (mem[0x2879] == 0) return;                         /* LDY $2879; BEQ */
    if (v66c >= 0x04) return;                             /* CMP #4; BCS */
    mem[0x2879] = 0; mem[0x0041] = 0;
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
    uint8_t  m       = cpu.A;                 /* 9C97: multiplier                      */
    uint8_t  signhi  = mem[0x00AB];           /* entry hi byte = sign byte             */
    mem[0x00AD] = signhi;                     /* 9C9F/9CA1: save sign byte             */

    /* |multiplicand|: the 6502 negates {$AA:$AB} in place when it is negative.  A
       16-bit two's-complement negate of a negative value always leaves the carry
       clear, so the bit the first `ROR $AC` injects into $AC is 0 in that case;
       for a non-negative multiplicand it is the caller's entry carry. */
    uint16_t mag;
    uint8_t  ac_bit0;
    if (signhi & 0x80) {                      /* 9CA3 BMI */
        mag = (uint16_t)(-(int)(uint16_t)(mem[0x00AA] | (signhi << 8)));
        mem[0x00AA] = (uint8_t)mag; mem[0x00AB] = (uint8_t)(mag >> 8);
        ac_bit0 = 0;
    } else {
        mag = (uint16_t)(mem[0x00AA] | (signhi << 8));
        ac_bit0 = cpu.C;                      /* entry carry -> first ROR $AC          */
    }

    /* Although this is a signed multiply, the 6502 does NOT do it with a signed
       product: the multiplier ($AC, the entry A) is an UNSIGNED 8-bit fraction
       (0..255), so a signed multiply would mis-handle multipliers >= 0x80.  It
       multiplies the magnitudes and re-applies the sign separately.  Hence the
       core is an unsigned 8x16 product (one 68000 MULU, NOT MULS):
           P = multiplier * |multiplicand|   (24-bit)
       $A9/$A8 hold P>>8.  The fractional byte $AC ends up as the product's low 7
       bits (P bits 6..0) shifted up into bits 7..1, with the injected carry left
       in bit0 (P's bit7 is shifted out, never stored).  The original sign is then
       re-applied to the 16-bit {$A9:$A8}. */
    uint32_t P      = (uint32_t)m * mag;
    uint16_t prod16 = (uint16_t)(P >> 8);
    if (signhi & 0x80) prod16 = (uint16_t)(-(int)prod16);   /* 9D55/9D57 re-apply sign */

    mem[0x00A8] = (uint8_t)prod16;  mem[0x00A9] = (uint8_t)(prod16 >> 8);
    mem[0x00AC] = (uint8_t)(((P & 0x7F) << 1) | ac_bit0);
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
 * Contract: memory only (the sole caller, terrain_frame_setup, reloads A via build_view).
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
       (terrain_plot_object_a path) may use the index X without reloading it. */
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

    if (A & 0x80) {                            /* B2FC BPL: else return */
        uint8_t add_branch = c;                /* B2FF BCS: carry from the $0091 ADC above */
        /* common to both branches: displacement = (mid - base) >> 1 in $B5/$B6 */
        A = mem[0x008D]; c = 1; D_SBC(mem[0x0082]); mem[0x00B5] = A;
        A = mem[0x008E]; D_SBC(mem[0x0083]); D_LSRA(); mem[0x00B6] = A; D_RORM(0x00B5);
        if (add_branch) {                      /* add branch (B31F) */
            A = mem[0x008F]; c = 0; D_ADC(mem[0x00B5]); mem[0x008F] = A;
            A = mem[0x0090]; D_ADC(mem[0x00B6]); mem[0x0090] = A;
        } else {                               /* subtract branch (B301) */
            A = mem[0x008F]; c = 1; D_SBC(mem[0x00B5]); mem[0x008F] = A;
            A = mem[0x0090]; D_SBC(mem[0x00B6]); mem[0x0090] = A;
        }
    }
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

/* terrain_plot_object_a @ $A822 — plot one terrain object (gun tower etc.), variant A.
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
void terrain_plot_object_a(void) {
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

/* terrain_plot_object_b @ $A90A — plot one terrain object, variant B (4 plot points).
 *
 * Like A822 but: column-reflect flag $28DF/$290F from $23B5[X]&1; uses point at
 * ($004E vs $260E[$004F]); after the fill it may additionally stamp a 2x2 cross of
 * pixels (clip_row_top x4 around $2400[X]/$245A[X]) when $00A7==0 && $003E==0 and
 * the object flag is in [2,$F8).  No RANDOM read.  All callees native/empty.
 * Contract: memory; exit X=$28E1; other regs dead.
 */
void terrain_plot_object_b(void) {
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
 * the A90A plotter.  Tail-calls native terrain_plot_object_b (the trailing
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
    if (mem[0x0900 + cpu.Y] & 0x80) { terrain_plot_object_a(); return; }    /* a655 BPL -> N set: A822 */

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
    terrain_plot_object_b();                                      /* a6c8 tail */
}

/* terrain_column_rasterize @ $B33D — THE fractal column renderer.
 *
 * Walks columns from $0082 up to $D4, interpolating per-segment heights ($00EA[]/
 * $00F4[]) and midpoint-refining the column position toward $0095[Y]; for each
 * column it clamps the height to $97, updates the max-height array $260E[X], and
 * OR-plots the silhouette pixel into the bitmap at ($0080),$BD00[X] using the
 * $BC00 bit table and the $28CA/$28FA row-address tables.
 *
 * Self-contained (no calls); structured C with purely local A/X/Y + carry c
 * (preserving the 6502 op order so carry threads identically); the bitmap pointer
 * is computed manually (= ZP_IND_Y($80)).
 * Contract: memory; cpu.X is preserved (saved to $0060, restored on exit — and
 * since we never touch cpu.X it stays at entry anyway).  Validated against a real
 * in-flight RAM snapshot (random mem[] would not terminate — $0095[] must be a
 * realistic increasing column array, else the midpoint loop has a fixed point).
 */
void terrain_column_rasterize(void) {
    uint8_t A, X, c = 0;
    uint8_t Y = cpu.Y;   /* entry Y; the $B33F (A==$82) path plots before reassigning Y */
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
    if (A < 0x2D) return;                                /* b341 CMP #$2D; BCC b37f */
    if (A < mem[0x0082]) return;                         /* b345 CMP $82; BCC b37f */
    if (A == mem[0x0082]) {                              /* b349 BNE b380 -> else */
        X = mem[0x0095];                                 /* b34b LDX $95 */
        A = mem[0x00EA];                                 /* b34d */
        if (A > mem[0x260E + X]) {                       /* b352 BCC / b354 BEQ -> skip(return) */
            mem[0x260E + X] = A;
            if (A >= 0x97) { mem[0x260E + X] = 0xFF; A = 0x97; }  /* b359 CMP #$97; BCC b364 */
            PLOT();                                      /* b364 */
        }
        return;                                          /* b37d LDX $60; b37f return */
    }

    /* b380 — interpolation: refine $82/$84/$86, emit sub-points into $96/$EB[] */
    Y = 0x00;                                            /* b380 LDY #0 */
    for (;;) {                                           /* L_b382 */
        A = mem[0x0082];                                 /* b382 */
        if (A >= 0x2C) { X = A; break; }                 /* BCC b38c else TAX; goto b446 */
        c = 0; ADC_(mem[0x0095 + Y]); RORA_();           /* b38c CLC; ADC $95,Y; ROR */
        if (A <= 0x2C) {                                 /* BCC b397 / BNE b3e9 -> A <= 0x2C */
            mem[0x0082] = A;                             /* b397 STA $82 */
            A = mem[0x0086]; c = 1; ADC_(mem[0x00F4 + Y]); mem[0x0086] = A;  /* LDA $86; SEC; ADC F4,Y */
            if (!(A & 0x80)) {                           /* BMI b3af -> else */
                c = 0; A = mem[0x0084]; ADC_(mem[0x00EA + Y]); RORA_(); mem[0x0084] = A;  /* CLC LDA84 ADC EA,Y ROR */
            } else if (c) {                              /* b3af BCS b3cd (c from $86 ADC) */
                A = mem[0x0095 + Y]; c = 1; SBC_(mem[0x0082]); LSRA_(); mem[0x00B5] = A;
                c = 0; A = mem[0x0084]; ADC_(mem[0x00EA + Y]); RORA_(); c = 0; ADC_(mem[0x00B5]);
                if (c) A = 0xFF;                         /* b3e0 BCC b3e4 (skip LDA#$FF) */
                mem[0x0084] = A;
            } else {                                     /* b3af (c clear) */
                A = mem[0x0095 + Y]; c = 1; SBC_(mem[0x0082]); LSRA_(); mem[0x00B5] = A;
                c = 0; A = mem[0x0084]; ADC_(mem[0x00EA + Y]); RORA_(); c = 1; SBC_(mem[0x00B5]);
                if (!c) A = 0x00;                        /* b3c4 BCC b3c8 (skip LDA#0) */
                mem[0x0084] = A;
            }
        } else {                                         /* b3e9: A > 0x2C */
            mem[0x0096 + Y] = A;                         /* b3e9 STA $96,Y */
            A = mem[0x0086]; c = 1; ADC_(mem[0x00F4 + Y]); mem[0x00F5 + Y] = A;  /* LDA $86; SEC; ADC F4,Y; STA F5,Y */
            if (!(A & 0x80)) {                           /* BMI b405 -> else */
                c = 0; A = mem[0x0084]; ADC_(mem[0x00EA + Y]); RORA_(); mem[0x00EB + Y] = A;
            } else if (c) {                              /* b405 BCS b425 (c from $86 ADC) */
                A = mem[0x0096 + Y]; c = 1; SBC_(mem[0x0082]); LSRA_(); mem[0x00B5] = A;
                c = 0; A = mem[0x0084]; ADC_(mem[0x00EA + Y]); RORA_(); c = 0; ADC_(mem[0x00B5]);
                if (c) A = 0xFF;                         /* b438 BCC b43c */
                mem[0x00EB + Y] = A;
            } else {                                     /* b405 (c clear) */
                A = mem[0x0096 + Y]; c = 1; SBC_(mem[0x0082]); LSRA_(); mem[0x00B5] = A;
                c = 0; A = mem[0x0084]; ADC_(mem[0x00EA + Y]); RORA_(); c = 1; SBC_(mem[0x00B5]);
                if (!c) A = 0x00;                        /* b41a BCC b41e */
                mem[0x00EB + Y] = A;
            }
            Y = (uint8_t)(Y + 1);                        /* INY */
        }
    }

    /* b446 — rasterize each leaf column into the silhouette bitmap */
    for (;;) {                                           /* L_b446 */
        if (X >= 0xD4) return;                            /* CPX #$D4; BCS b443 */
        A = X; mem[0x0082] = X;                          /* TXA; STX $82 */
        c = 1; SBC_(mem[0x0095 + Y]);                    /* SEC; SBC $95,Y */
        c = (A >= 0xFE) ? 1 : 0;                         /* CMP #$FE (carry used at b4cc) */
        if (A == 0xFE) {                                 /* BNE b4cc -> else */
            A = mem[0x00EA + Y]; ADC_(mem[0x0084]); RORA_();   /* LDA EA,Y; ADC $84 (c=1); ROR */
            if (A > mem[0x260E + X]) {                   /* b489 skip (BCC/BEQ) */
                mem[0x260E + X] = A;
                if (A >= 0x97) { mem[0x260E + X] = 0xFF; A = 0x97; } /* CMP #$97; BCC b470 */
                PLOT();                                  /* b470 */
            }
            X++;                                         /* b489 INX */
            A = mem[0x00EA + Y]; mem[0x0084] = A;        /* LDA EA,Y; STA $84 */
            if (A > mem[0x260E + X]) {                   /* b4bd skip (BCC/BEQ) */
                mem[0x260E + X] = A;
                if (A >= 0x97) { mem[0x260E + X] = 0xFF; A = 0x97; }
                PLOT();                                  /* b4a4 */
            }
            Y = (uint8_t)(Y - 1); if (Y & 0x80) return;  /* b4bd DEY; BMI b4c9 */
            X++;                                         /* INX */
            A = mem[0x00F5 + Y]; mem[0x0086] = A;        /* LDA F5,Y; STA $86 */
            continue;                                    /* goto b446 */
        } else if (c) {                                  /* b4cc, carry set (A == 0xFF) */
            A = mem[0x00EA + Y]; mem[0x0084] = A;        /* LDA EA,Y; STA $84 */
            if (A > mem[0x260E + X]) {                   /* b501 skip (BCC/BEQ) */
                mem[0x260E + X] = A;
                if (A >= 0x97) { mem[0x260E + X] = 0xFF; A = 0x97; }
                PLOT();                                  /* b4e8 */
            }
            Y = (uint8_t)(Y - 1); if (Y & 0x80) return;  /* b501 DEY; BMI b4c9 */
            X++;                                         /* INX */
            A = mem[0x00F5 + Y]; mem[0x0086] = A;        /* LDA F5,Y; STA $86 */
            continue;                                    /* goto b446 */
        } else {                                         /* b50d, carry clear (A < 0xFE) */
            A = X; ADC_(mem[0x0095 + Y]); RORA_(); mem[0x0096 + Y] = A;  /* TXA; ADC $95,Y (c=0); ROR; STA 96,Y */
            A = mem[0x0086]; c = 1; ADC_(mem[0x00F4 + Y]); mem[0x00F5 + Y] = A;  /* LDA $86; SEC; ADC F4,Y; STA F5,Y */
            if (!(A & 0x80)) {                           /* BMI b52e -> else */
                c = 0; A = mem[0x0084]; ADC_(mem[0x00EA + Y]); RORA_(); mem[0x00EB + Y] = A;
            } else if (c) {                              /* b52e BCS b54c (c from $86 ADC) */
                A = mem[0x0096 + Y]; SBC_(mem[0x0082]); LSRA_(); mem[0x00B5] = A;   /* SBC $82 (c=1, no SEC); LSR */
                c = 0; A = mem[0x0084]; ADC_(mem[0x00EA + Y]); RORA_(); ADC_(mem[0x00B5]);  /* CLC ADC EA,Y ROR; ADC B5 (c from ROR) */
                if (c) A = 0xFF;                         /* b55d BCC b561 */
                mem[0x00EB + Y] = A;
            } else {                                     /* b52e (c clear) */
                A = mem[0x0096 + Y]; SBC_(mem[0x0082]); LSRA_(); mem[0x00B5] = A;   /* SBC $82 (c=0, no SEC); LSR */
                c = 0; A = mem[0x0084]; ADC_(mem[0x00EA + Y]); RORA_(); SBC_(mem[0x00B5]);  /* CLC ADC EA,Y ROR; SBC B5 (c from ROR) */
                if (!c) A = 0x00;                        /* b541 BCC b545 */
                mem[0x00EB + Y] = A;
            }
            Y = (uint8_t)(Y + 1);                        /* INY */
            continue;                                    /* goto b446 */
        }
    }

    #undef ADC_
    #undef SBC_
    #undef RORA_
    #undef LSRA_
    #undef PLOT
    return;
}

/* terrain_subdivide_column @ $B172 — fractal terrain subdivision driver.
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
void terrain_subdivide_column(void) {
    uint8_t A, c;
    #define ADC_(v) do { uint16_t _t=(uint16_t)A+(uint8_t)(v)+c; c=(uint8_t)(_t>>8); A=(uint8_t)_t; } while(0)
    #define SBC_(v) ADC_((uint8_t)~(uint8_t)(v))

    uint8_t b5 = (uint8_t)(mem[0x25D2] ^ 0x80); mem[0x00B5] = b5;   /* b172 (signed cmp) */
    A = (uint8_t)(mem[0x0083] ^ 0x80);
    c = (A >= b5) ? 1 : 0;                                /* CMP $B5 */
    if (A == b5) { A = mem[0x0082]; c = (A >= mem[0x25B4]) ? 1 : 0; }   /* BNE b186; else CMP 25B4 */
    if (c) return;                                        /* b186 BCS b1c1 */
    mem[0x009F] = 0x14;                                   /* b188 */

    /* b18c — descend: midpoint-split the span, pushing sub-points until $83 turns +ve */
    for (;;) {
        if (!(mem[0x0083] & 0x80)) break;                 /* LDA $83; BPL b1d9 */
        mem[0x009F]--; if (mem[0x009F] & 0x80) return;    /* DEC $9F; BMI b1c1 */
        terrain_midpoint_displace();                      /* b194 (uses cpu.X) */
        A = mem[0x008E];
        if ((A & 0x80) || (A == 0 && mem[0x008D] < 0x28)) {  /* BMI b1c2 / (BNE b1a3; $8D<$28 -> b1c2) */
            mem[0x0082] = mem[0x008D]; mem[0x0083] = mem[0x008E];   /* b1c2 update span, loop */
            mem[0x0084] = mem[0x008F]; mem[0x0085] = mem[0x0090]; mem[0x0086] = mem[0x0091];
        } else {
            mem[0x25B5 + cpu.X] = mem[0x008D];            /* b1a3 push sub-point */
            mem[0x25D3 + cpu.X] = mem[0x008E];
            mem[0x25F1 + cpu.X] = mem[0x008F];
            mem[0x24E3 + cpu.X] = mem[0x0090];
            mem[0x23E3 + cpu.X] = mem[0x0091];
            cpu.X++;
            if (cpu.X >= 0x0F) return;                    /* CPX #$0F; BCC b18c -> else return */
        }
    }

    /* b1d9 — leaf + unwind: rasterize leaf segments, then pop the stack and repeat */
    for (;;) {                                            /* re-entered from b2aa */
        if (mem[0x0083] != 0) return;                     /* LDA $83; BNE b1c1 */
        if (mem[0x0082] >= 0xD8) return;                  /* CMP #$D8; BCS b1c1 */

        int rasterize = 0;          /* b211 cascade outcome: 1 = b27b (rasterize), 0 = b2aa (skip) */
        int force_b1e8 = 0;         /* b241/b25d re-enter the b1e8 body without the b1e3 test */
        for (;;) {                                        /* b1e3 / b1e8 inner loop */
            int cascade;
            if (force_b1e8) { force_b1e8 = 0; cascade = 0; }
            else cascade = (mem[0x25D2 + cpu.X] == 0);    /* b1e3 LDA 25D2,X; BEQ b211 */
            if (!cascade) {
                /* b1e8 */
                mem[0x009F]--; if (mem[0x009F] & 0x80) return;   /* DEC $9F; BMI b210 */
                terrain_midpoint_displace();
                mem[0x25B5 + cpu.X] = mem[0x008D];
                mem[0x25D3 + cpu.X] = mem[0x008E];
                mem[0x25F1 + cpu.X] = mem[0x008F];
                mem[0x24E3 + cpu.X] = mem[0x0090];
                mem[0x23E3 + cpu.X] = mem[0x0091];
                cpu.X++;
                if (cpu.X >= 0x0F) return;                /* CPX #$0F; BCS b210 */
                continue;                                  /* goto b1e3 */
            }
            /* b211 cascade — choose b2aa (skip) / b1e8 (recurse) / b27b (rasterize) */
            int use21d;
            A = mem[0x0085];
            if (A & 0x80) use21d = 1;                     /* BMI b21d */
            else if (A != 0) use21d = 0;                  /* BNE b22d */
            else if (mem[0x0084] >= 0x6C) use21d = 0;     /* CMP #$6C; BCS b22d */
            else use21d = 1;
            if (use21d) {
                /* b21d */
                A = mem[0x24E2 + cpu.X];
                int b241;
                if (A & 0x80) b241 = 0;                   /* BMI b23e -> b2aa (rasterize stays 0) */
                else if (A != 0) b241 = 1;                /* BNE b241 */
                else if (mem[0x25F0 + cpu.X] < 0x6C) b241 = 0;  /* BCC b23e -> b2aa */
                else b241 = 1;
                if (b241) {
                    /* b241 */
                    c = 1; A = mem[0x25B4 + cpu.X]; SBC_(mem[0x0082]);   /* SEC; LDA 25B4,X; SBC $82 */
                    if (A < 0x14) rasterize = 1;          /* CMP #$14; BCC b27b */
                    else {
                        A = (uint8_t)(A >> 2); mem[0x00B5] = A;          /* LSR;LSR; STA $B5 */
                        A = mem[0x0084]; c = 1; SBC_(mem[0x00B5]);       /* LDA $84; SEC; SBC $B5 */
                        A = mem[0x0085]; SBC_(0x00);                     /* LDA $85; SBC #0 (16-bit) */
                        if (!(A & 0x80)) rasterize = 1;    /* BPL b27b */
                        else { force_b1e8 = 1; continue; } /* goto b1e8 */
                    }
                }
            } else {
                /* b22d */
                A = mem[0x24E2 + cpu.X];
                int b25d;
                if (A & 0x80) b25d = 1;                   /* BMI b25d */
                else if (A != 0) { rasterize = 1; b25d = 0; }  /* BNE b27b */
                else if (mem[0x25F0 + cpu.X] < 0x6C) b25d = 1; /* BCC b25d */
                else { rasterize = 1; b25d = 0; }         /* b27b */
                if (b25d) {
                    /* b25d */
                    c = 1; A = mem[0x25B4 + cpu.X]; SBC_(mem[0x0082]);
                    if (A < 0x14) rasterize = 1;
                    else {
                        A = (uint8_t)(A >> 2); mem[0x00B5] = A;
                        A = mem[0x25F0 + cpu.X]; c = 1; SBC_(mem[0x00B5]);
                        A = mem[0x24E2 + cpu.X]; SBC_(0x00);
                        if (!(A & 0x80)) rasterize = 1;
                        else { force_b1e8 = 1; continue; } /* goto b1e8 */
                    }
                }
            }
            break;   /* cascade resolved to b2aa or b27b; leave the inner loop */
        }
        if (rasterize) {
            /* b27b — clamp the leaf and rasterize it */
            A = mem[0x0085];                              /* clamp $84 by $85 sign */
            if (A != 0) mem[0x0084] = (A & 0x80) ? 0x00 : 0xFF;
            A = mem[0x24E2 + cpu.X];                      /* select/clamp $EA */
            if (A == 0) A = mem[0x25F0 + cpu.X];
            else A = (A & 0x80) ? 0x00 : 0xFF;
            mem[0x00EA] = A;
            mem[0x0095] = mem[0x25B4 + cpu.X];
            mem[0x00F4] = mem[0x23E2 + cpu.X];
            terrain_column_rasterize();                   /* b2a7 (uses cpu.X, cpu.Y) */
        }
        /* b2aa */
        if (cpu.X == 0) return;                           /* CPX #0; BEQ b2cb */
        mem[0x0082] = mem[0x25B4 + cpu.X];                /* reload span from the stacks */
        mem[0x0083] = mem[0x25D2 + cpu.X];
        mem[0x0084] = mem[0x25F0 + cpu.X];
        mem[0x0085] = mem[0x24E2 + cpu.X];
        mem[0x0086] = mem[0x23E2 + cpu.X];
        cpu.X--;                                          /* DEX; goto b1d9 (outer loop) */
    }
    #undef ADC_
    #undef SBC_
}

/* terrain_jitter_column @ $A613 — per-frame random terrain/object jitter (2+1 RANDOM).
 *
 * $2829 = RANDOM + RANDOM (with the add's carry rolled into $0068, EOR'd $FF when
 * the ship column $0064 >= $6C); $282C = RANDOM - $80 with $0069 = -1 on borrow
 * (a signed offset).  Tail-calls the empty terrain_plot_return.
 * Contract: memory.  Reads POKEY RANDOM (harness seeds it identically per run).
 */
void terrain_jitter_column(void) {
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

/* terrain_frame_setup @ $9E54 — terrain/level generation step 1 (flight top #2).
 *
 * Calls native setup_projection_params + build_view_transform_matrix, then runs
 * two loops over the per-column transform tables ($22A3/$22D1/$22FF/$232D etc.):
 *  - LOOP 1 (Y=0..$2C): per terrain map cell read via ($80),Y, rotate the cell's
 *    bit pattern ($B5) to pick one of six rotate/translate updates of the column
 *    vectors, derive the screen-X ($2388/$235B) and visibility class ($24B4).
 *  - LOOP 2 (Y=0..$0B): walk the object draw order $B67C[], collapse adjacent
 *    visible/hidden pairs (clear $232E/set $2300=$20/clear $24B4) per the class.
 *
 * Structured C, locals A/X/Y/c (op order preserved so carry threads as on the 6502);
 * X carries cross-iteration state
 * in loop 1 (rebuilt at $9FB9 as (X&$0F)|$B6).  Carry into the top ROL A chain and
 * into ROL $B5 is irrelevant (masked / discarded low bits) but threaded anyway.
 * Contract: memory only (the main flight loop reloads regs after the call).
 */
void terrain_frame_setup(void) {
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
    if (!(A & 0x80)) {                                    /* 9e5c BMI -> else (bit7 set) */
        ROLA_();                                         /* 9e5e */
        if (A & 0x80) {                                  /* 9e5f BMI */
            X = 0x4F; Y = 0xB6;                          /* 9e7f/9e81 */
        } else {
            mem[0x22A4] = mem[0x22A3];                   /* 9e61 (only bit7=0,bit6=0 path copies) */
            mem[0x22D2] = mem[0x22D1];
            mem[0x2300] = mem[0x22FF];
            mem[0x232E] = mem[0x232D];
            X = 0x22; Y = 0xB6;                          /* 9e79/9e7b */
        }
    } else {                                             /* 9e85 */
        ROLA_();                                         /* 9e85 */
        if (A & 0x80) {                                  /* 9e86 BMI */
            X = 0xC8; Y = 0xB5;                          /* 9e8e/9e90 */
        } else {
            X = 0xF5; Y = 0xB5;                          /* 9e88/9e8a */
        }
    }
    /* 9e92 */
    mem[0x0080] = X; mem[0x0081] = Y;                    /* 9e92/9e94 (ptr for ($80),Y) */
    A = mem[0x008A]; ROLA_(); ROLA_(); ROLA_(); ROLA_(); /* 9e96-9e9b */
    A &= 0xF0; mem[0x00B6] = A;                          /* 9e9c/9e9e */
    A = mem[0x0088]; A &= 0x0F; X = A;                   /* 9ea0-9ea4 (TAX) */
    A |= mem[0x00B5]; mem[0x00B4] = A;                   /* 9ea5/9ea7 */

    Y = 0x00;                                            /* 9ea9 */
    do {                                                 /* L_9eab — per terrain cell */
        A = bus_read((uint16_t)(mem[0x0080] | (mem[0x0081] << 8)) + Y);  /* 9eab LDA ($80),Y */
        mem[0x00B5] = A;                                 /* 9ead */

        /* Examine $B5 MSB-first (up to four ROL $B5) to pick one of six
           rotate/translate updates of the column vectors.  The first two hits
           continue into the 9f61 pair (E/F); later hits land directly at 9fb2. */
        int via_9f61 = 0;
        if (A & 0x80) {                                  /* 9eaf BPL -> else; case A */
            A = mem[0x00B6]; c = 0; ADC_(0xF0); mem[0x00B6] = A;  /* 9eb1-9eb6 */
            c = 0; A = mem[0x22A3 + Y]; ADC_(mem[0x00A0]); mem[0x22A4 + Y] = A;  /* 9eb8-9ebe */
            A = mem[0x22D1 + Y]; ADC_(mem[0x00A1]); mem[0x22D2 + Y] = A;         /* 9ec1-9ec6 */
            c = 1; A = mem[0x22FF + Y]; SBC_(mem[0x00A2]); mem[0x2300 + Y] = A;  /* 9ec9-9ecf */
            A = mem[0x232D + Y]; SBC_(mem[0x00A3]); mem[0x232E + Y] = A;         /* 9ed2-9ed7 */
            via_9f61 = 1;                                /* 9eda goto 9f61 */
        } else {
            ROLM_(0x00B5);                               /* 9edd */
            if (mem[0x00B5] & 0x80) {                     /* 9edf BPL -> else; case B */
                A = mem[0x00B6]; c = 0; ADC_(0x10); mem[0x00B6] = A;  /* 9ee1-9ee6 */
                c = 1; A = mem[0x22A3 + Y]; SBC_(mem[0x00A0]); mem[0x22A4 + Y] = A;  /* 9ee8-9eee */
                A = mem[0x22D1 + Y]; SBC_(mem[0x00A1]); mem[0x22D2 + Y] = A;         /* 9ef1-9ef6 */
                c = 0; A = mem[0x22FF + Y]; ADC_(mem[0x00A2]); mem[0x2300 + Y] = A;  /* 9ef9-9eff */
                A = mem[0x232D + Y]; ADC_(mem[0x00A3]); mem[0x232E + Y] = A;         /* 9f02-9f07 */
                via_9f61 = 1;                            /* 9f0a goto 9f61 */
            } else {
                ROLM_(0x00B5);                           /* 9f0d */
                if (mem[0x00B5] & 0x80) {                 /* 9f0f BPL -> else; case C */
                    X = (uint8_t)(X - 1);                /* 9f11 DEX */
                    c = 1; A = mem[0x22A3 + Y]; SBC_(mem[0x00A2]); mem[0x22A4 + Y] = A;  /* 9f12-9f18 */
                    A = mem[0x22D1 + Y]; SBC_(mem[0x00A3]); mem[0x22D2 + Y] = A;         /* 9f1b-9f20 */
                    c = 1; A = mem[0x22FF + Y]; SBC_(mem[0x00A0]); mem[0x2300 + Y] = A;  /* 9f23-9f29 */
                    A = mem[0x232D + Y]; SBC_(mem[0x00A1]); mem[0x232E + Y] = A;         /* 9f2c-9f31 */
                } else {
                    ROLM_(0x00B5);                       /* 9f37 */
                    if (mem[0x00B5] & 0x80) {             /* 9f39 BPL -> 9fb2; case D */
                        X = (uint8_t)(X + 1);            /* 9f3b INX */
                        c = 0; A = mem[0x22A3 + Y]; ADC_(mem[0x00A2]); mem[0x22A4 + Y] = A;  /* 9f3c-9f42 */
                        A = mem[0x22D1 + Y]; ADC_(mem[0x00A3]); mem[0x22D2 + Y] = A;         /* 9f45-9f4a */
                        c = 0; A = mem[0x22FF + Y]; ADC_(mem[0x00A0]); mem[0x2300 + Y] = A;  /* 9f4d-9f53 */
                        A = mem[0x232D + Y]; ADC_(mem[0x00A1]); mem[0x232E + Y] = A;         /* 9f56-9f5b */
                    }
                }
            }
        }
        if (via_9f61) {                                  /* L_9f61 (reached from case A/B) */
            ROLM_(0x00B5);                               /* 9f61 */
            if (mem[0x00B5] & 0x80) {                     /* 9f63 BPL -> else; case E */
                X = (uint8_t)(X - 1);                    /* 9f65 DEX */
                c = 1; A = mem[0x22A4 + Y]; SBC_(mem[0x00A2]); mem[0x22A4 + Y] = A;  /* 9f66-9f6c */
                A = mem[0x22D2 + Y]; SBC_(mem[0x00A3]); mem[0x22D2 + Y] = A;         /* 9f6f-9f74 */
                c = 1; A = mem[0x2300 + Y]; SBC_(mem[0x00A0]); mem[0x2300 + Y] = A;  /* 9f77-9f7d */
                A = mem[0x232E + Y]; SBC_(mem[0x00A1]); mem[0x232E + Y] = A;         /* 9f80-9f85 */
            } else {
                ROLM_(0x00B5);                           /* 9f8b */
                if (mem[0x00B5] & 0x80) {                 /* 9f8d BPL -> 9fb2; case F */
                    X = (uint8_t)(X + 1);                /* 9f8f INX */
                    c = 0; A = mem[0x22A4 + Y]; ADC_(mem[0x00A2]); mem[0x22A4 + Y] = A;  /* 9f90-9f96 */
                    A = mem[0x22D2 + Y]; ADC_(mem[0x00A3]); mem[0x22D2 + Y] = A;         /* 9f99-9f9e */
                    c = 0; A = mem[0x2300 + Y]; ADC_(mem[0x00A0]); mem[0x2300 + Y] = A;  /* 9fa1-9fa7 */
                    A = mem[0x232E + Y]; ADC_(mem[0x00A1]); mem[0x232E + Y] = A;         /* 9faa-9faf */
                }
            }
        }

        /* 9fb2 — derive screen-X ($2388) and the $235B/$2276 columns */
        c = 1; A = 0x00; SBC_(mem[0x008B]); mem[0x00B5] = A;  /* 9fb2-9fb7 */
        A = X; A &= 0x0F; A |= mem[0x00B6]; X = A;            /* 9fb9-9fbe (TXA;AND;ORA;TAX) */
        A = mem[0x0900 + X]; mem[0x23B5 + Y] = A;             /* 9fbf-9fc2 */
        SBC_(mem[0x008C]);                                   /* 9fc5 (carry from 9fb2 chain) */
        if (c) {                                             /* 9fc7 BCC -> else */
            LSRA_(); RORM_(0x00B5); LSRA_(); RORM_(0x00B5);  /* 9fc9-9fcd */
            LSRA_(); RORM_(0x00B5); LSRA_(); RORM_(0x00B5);  /* 9fcf-9fd3 */
        } else {
            LSRA_(); RORM_(0x00B5); LSRA_(); RORM_(0x00B5);  /* 9fd8-9fdc */
            LSRA_(); RORM_(0x00B5); LSRA_(); RORM_(0x00B5);  /* 9fde-9fe2 */
            A ^= 0xF0;                                       /* 9fe4 */
        }
        /* 9fe6 */
        mem[0x2388 + Y] = A;                                 /* 9fe6 */
        A = mem[0x00B5]; mem[0x235B + Y] = A;                /* 9fe9-9feb */
        A = X; mem[0x2276 + Y] = A;                          /* 9fee (TXA) */

        /* visibility class -> $24B4 (one of $80/$40/$20/$00) */
        uint8_t cls;
        A = mem[0x232E + Y];                                 /* 9ff2 */
        if (A & 0x80) {                                      /* 9ff5 BMI */
            cls = 0x80;                                      /* a03b */
        } else if (A == 0 && mem[0x2300 + Y] < 0x20) {       /* 9ff7 BNE / 9ffc CMP #$20; BCC */
            cls = 0x80;                                      /* a03b */
        } else {
            /* a000 */
            A = mem[0x22D2 + Y];                             /* a000 */
            if (A & 0x80) {                                  /* a003 BPL -> else */
                A = 0x00; c = 1; SBC_(mem[0x22A4 + Y]); mem[0x00B5] = A;  /* a005-a00b */
                A = 0x00; SBC_(mem[0x22D2 + Y]);             /* a00d-a00f */
                uint8_t m = mem[0x232E + Y];                 /* a012 CMP $232E,Y */
                if (A < m) cls = 0x00;                       /* a015 BCC -> a037 */
                else if (A != m) cls = 0x40;                 /* a017 BNE -> a020 */
                else cls = (mem[0x00B5] < mem[0x2300 + Y]) ? 0x00 : 0x40;  /* a019-a01e / a020 */
            } else {
                /* a024 (A = $22D2,Y) */
                uint8_t m = mem[0x232E + Y];                 /* a024 CMP $232E,Y */
                if (A < m) cls = 0x00;                       /* a027 BCC -> a037 */
                else if (A != m) cls = 0x20;                 /* a029 BNE -> a033 */
                else cls = (mem[0x22A4 + Y] < mem[0x2300 + Y]) ? 0x00 : 0x20;  /* a02b-a031 / a033 */
            }
        }
        /* a03d */
        mem[0x24B4 + Y] = cls;                               /* a03d */
        Y = (uint8_t)(Y + 1);                                /* a040 INY */
    } while (Y != 0x2D);                                     /* a041 CPY #$2D; a043 BEQ a048 */

    /* a048 — walk the object draw order $B67C[], collapsing visible/hidden pairs */
    Y = 0x00;                                                /* a048 */
    for (;;) {                                               /* L_a04a */
        X = mem[0xB67C + Y];                                 /* a04a LDX $B67C,Y */
        mem[0x28DB] = X;                                     /* a04d */
        Y = (uint8_t)(Y + 1);                                /* a050 INY */
        A = mem[0x24B4 + X];                                 /* a051 */

        int do_a079 = 0, do_a09a = 0;
        if (A & 0x80) {                                      /* a054 BMI -> a079 */
            do_a079 = 1;
        } else if (A != 0) {                                 /* a056 BNE -> a09a */
            do_a09a = 1;
        } else {
            X = mem[0xB67C + Y];                             /* a058 */
            Y = (uint8_t)(Y + 1);                            /* a05b INY */
            A = mem[0x24B4 + X];                             /* a05c */
            if (A & 0x80) {                                  /* a05f BPL -> a09b (skip) */
                mem[0x00B5] = Y;                             /* a061 STY $B5 */
                Y = mem[0x28DB];                             /* a063 */
                mem[0x232E + X] = 0x00;                      /* a066-a068 */
                mem[0x2300 + X] = 0x20;                      /* a06b-a06d */
                mem[0x24B4 + X] = 0x00;                      /* a070-a072 */
                Y = mem[0x00B5];                             /* a075 */
                if (Y == 0) do_a079 = 1;                     /* a077 BNE -> a09b; else fall to a079 */
            }
        }
        if (do_a079) {                                       /* L_a079 */
            X = mem[0xB67C + Y];                             /* a079 */
            Y = (uint8_t)(Y + 1);                            /* a07c INY */
            A = mem[0x24B4 + X];                             /* a07d */
            if (A == 0) {                                    /* a080 BNE -> a09b (skip) */
                mem[0x00B5] = Y;                             /* a082 STY $B5 */
                Y = mem[0x28DB];                             /* a084 */
                mem[0x232E + Y] = 0x00;                      /* a087-a089 (indexed by Y=$28DB) */
                mem[0x2300 + Y] = 0x20;                      /* a08c-a08e */
                mem[0x24B4 + Y] = 0x00;                      /* a091-a093 */
                Y = mem[0x00B5];                             /* a096 */
                if (Y == 0) do_a09a = 1;                     /* a098 BNE -> a09b; else fall to a09a */
            }
        }
        if (do_a09a) Y = (uint8_t)(Y + 1);                   /* L_a09a INY */
        /* a09b */
        if (Y >= 0x0C) return;                               /* a09b CPY #$0C; a09d BCS a0a2 */
    }                                                        /* a09f goto a04a */

    #undef ADC_
    #undef SBC_
    #undef ROLA_
    #undef LSRA_
    #undef ROLM_
    #undef RORM_
}

/* project_terrain_points @ $A11F — per-object world->screen projection (flight top #3).
 *
 * Input: cpu.X = object/column index.  Two near-identical halves each normalize a
 * 16-bit numerator (the doubling loop that scales {$AF:$AE} until it exceeds the
 * value), call divide_16x16, then fold the quotient ($B2) into a screen coordinate:
 *   - half 1: {$22A4:$22D2}[X] / {$2300:$232E}[X]  ->  {$2400:$242D}[X]
 *   - half 2: {$235B:$2388}[X] / {$2300:$232E}[X]  ->  {$245A:$2487}[X]
 * Tail ($A2DD): add a per-screen-band scroll offset ($270E[]/$272D) into the
 * {$245A:$2487}[X] pair.  The $A31C fall-through into terrain_draw_frame is dead code
 * (the preceding BMI/BPL are exhaustive) but kept to mirror the oracle.
 *
 * divide_16x16 is pure-memory (preserves cpu.X), so the entry X threads through as
 * a constant local; Y is saved/restored via $009F around the divide (as the 6502
 * does).  Contract: memory only (caller reloads regs).
 *
 * This calls the typed divide_16x16_core(): only the QUOTIENT feeds the coordinate
 * fold below (written to $B2, which the fold reads/RMWs).  The 6502 divide also left
 * a remainder in $B0/$B1 and the shifted divisor in $AE/$AF, but those — like $B2's
 * final value — are DEAD after this function: disasm/zeropage.csv shows $AE/$AF/$B0/
 * $B1/$B2 are touched by no routine except divide_16x16 + this one, so nothing reads
 * them before the next call overwrites them.  The validate harness excludes those 5
 * cells from this function's contract (see set_ignore in validate_native.c), so the
 * core's results don't need to be marshalled back into 6502 divide scratch.
 */
void project_terrain_points(void) {
    uint8_t A, Y, c, X = cpu.X;
    #define ADC_(v)  do { uint16_t _t=(uint16_t)A+(uint8_t)(v)+c; c=(uint8_t)(_t>>8); A=(uint8_t)_t; } while(0)
    #define SBC_(v)  ADC_((uint8_t)~(uint8_t)(v))
    #define ASLM_(a) do { uint8_t _v=mem[a]; c=_v>>7; mem[a]=(uint8_t)(_v<<1); } while(0)
    #define ROLM_(a) do { uint8_t _v=mem[a],_n=_v>>7; mem[a]=(uint8_t)((_v<<1)|c); c=_n; } while(0)
    #define RORM_(a) do { uint8_t _v=mem[a],_n=_v&1; mem[a]=(uint8_t)((_v>>1)|(c<<7)); c=_n; } while(0)
    #define ROLA_()  do { uint8_t _n=A>>7; A=(uint8_t)((A<<1)|c); c=_n; } while(0)
    #define RORA_()  do { uint8_t _n=A&1; A=(uint8_t)((A>>1)|(c<<7)); c=_n; } while(0)
    #define LSRA_()  do { c=A&1; A=(uint8_t)(A>>1); } while(0)

    A = mem[0x24B4 + X]; A |= 0x10; mem[0x24B4 + X] = A;  /* a11f-a124 */

    /* --- half 1: {$22A4:$22D2}/{$2300:$232E} -> {$2400:$242D} --- */
    Y = 0x00;                                            /* a127 */
    A = mem[0x2300 + X]; mem[0x00AE] = A;                /* a129-a12c */
    A = mem[0x232E + X]; mem[0x00AF] = A;                /* a12e-a131 */
    A = mem[0x22D2 + X];                                /* a133 */
    if (A & 0x80) {                                      /* a136 BMI */
        A = 0x00; c = 1; SBC_(mem[0x22A4 + X]); mem[0x00B0] = A;  /* a142-a148 */
        A = 0x00; SBC_(mem[0x22D2 + X]); mem[0x00B1] = A;         /* a14a-a14f */
    } else {
        mem[0x00B1] = A;                                 /* a138 */
        A = mem[0x22A4 + X]; mem[0x00B0] = A;            /* a13a-a13d */
    }
    /* L_a151 — normalize {$AF:$AE} until it exceeds {$B1:$B0}, or 8 doublings overflow */
    int half1_divide = 0;
    for (;;) {
        A = mem[0x00B1];                                 /* a151 */
        if (A < mem[0x00AF]) { half1_divide = 1; break; }   /* a153/a155 BCC -> a187 */
        if (A == mem[0x00AF]) {                          /* a157 BNE -> skip */
            A = mem[0x00B0];                             /* a159 */
            if (A < mem[0x00AE]) { half1_divide = 1; break; }  /* a15b/a15d BCC -> a187 */
        }
        ASLM_(0x00AE); ROLM_(0x00AF);                    /* a15f-a161 */
        Y = (uint8_t)(Y + 1);                            /* a163 INY */
        if (Y == 0x08) break;                            /* a164 CPY #8; a166 BNE -> a168 */
    }
    if (!half1_divide) {
        /* a168 — overflow: numerator never fit, clamp to screen edge */
        A = mem[0x22D2 + X];                            /* a168 */
        if (A & 0x80) { mem[0x242D + X] = 0xC0; mem[0x2400 + X] = 0x00; }  /* a17a */
        else          { mem[0x242D + X] = 0x40; mem[0x2400 + X] = 0x00; }  /* a16d */
    } else {
        /* a187 — divide and fold quotient into the screen coordinate.  $9F save/
           restore kept (it is shared scratch, in the contract); the core preserves
           our local Y anyway.  Only the quotient ($B2) feeds the fold. */
        mem[0x009F] = Y;                                 /* a187 STY $9F */
        mem[0x00B2] = divide_16x16_core(rd16(0x00B0), rd16(0x00AE)).quotient;  /* a189 */
        Y = mem[0x009F];                                 /* a18c LDY $9F */
        Y = (uint8_t)(Y - 1);                            /* a18e DEY */
        if (Y & 0x80) {                                  /* a18f BMI -> a1d9 */
            mem[0x242D + X] = 0x00;                      /* a1d9-a1db */
            A = mem[0x22D2 + X];                        /* a1de */
            if (A & 0x80) {                              /* a1e1 BMI -> a1f2 */
                A = mem[0x00B2]; LSRA_(); c = 1; ADC_(mem[0x00B2]);  /* a1f2-a1f6 */
                RORA_(); LSRA_(); A ^= 0xFF;             /* a1f8-a1fa */
                c = 1; ADC_(0x80); mem[0x2400 + X] = A;  /* a1fc-a1ff */
            } else {
                A = mem[0x00B2]; LSRA_(); c = 1; ADC_(mem[0x00B2]);  /* a1e3-a1e7 (LSR;SEC;ADC) */
                RORA_(); LSRA_(); A |= 0x80; mem[0x2400 + X] = A;    /* a1e9-a1ed */
                /* a1f0 BNE always taken (A|=0x80 => nonzero) -> a202 */
            }
        } else {
            A = 0x00;                                    /* a191 */
            do { ASLM_(0x00B2); ROLA_(); Y = (uint8_t)(Y - 1); }     /* a193-a195 */
            while (!(Y & 0x80));                          /* a196-a197 DEY; BPL */
            mem[0x00B5] = A;                             /* a199 */
            LSRA_(); Y = A;                              /* a19b-a19c (LSR;TAY) */
            A = mem[0x00B2]; RORA_();                    /* a19d-a19f */
            c = 0; ADC_(mem[0x00B2]); mem[0x00B2] = A;    /* a1a0-a1a3 */
            A = Y; ADC_(mem[0x00B5]);                    /* a1a5-a1a6 (TYA;ADC, carry threads) */
            LSRA_(); RORM_(0x00B2);                      /* a1a8-a1a9 */
            LSRA_(); RORM_(0x00B2);                      /* a1ab-a1ac */
            mem[0x00B5] = A;                             /* a1ae */
            A = mem[0x22D2 + X];                        /* a1b0 */
            if (A & 0x80) {                              /* a1b3 BMI -> a1c7 */
                A = 0x80; c = 1; SBC_(mem[0x00B2]); mem[0x2400 + X] = A;  /* a1c7-a1cc */
                A = 0x00; SBC_(mem[0x00B5]); mem[0x242D + X] = A;        /* a1cf-a1d3 */
            } else {
                A = 0x80; c = 0; ADC_(mem[0x00B2]); mem[0x2400 + X] = A;  /* a1b5-a1ba */
                A = mem[0x00B5]; ADC_(0x00); mem[0x242D + X] = A;        /* a1bd-a1c1 */
            }
        }
    }

    /* --- half 2: {$235B:$2388}/{$2300:$232E} -> {$245A:$2487} --- */
    Y = 0x00;                                            /* a202 */
    A = mem[0x2300 + X]; mem[0x00AE] = A;                /* a204-a207 */
    A = mem[0x232E + X]; mem[0x00AF] = A;                /* a209-a20c */
    A = mem[0x2388 + X];                                /* a20e */
    if (A & 0x80) {                                      /* a211 BMI */
        A = 0x00; c = 1; SBC_(mem[0x235B + X]); mem[0x00B0] = A;  /* a21d-a223 */
        A = 0x00; SBC_(mem[0x2388 + X]); mem[0x00B1] = A;        /* a225-a22a */
    } else {
        mem[0x00B1] = A;                                 /* a213 */
        A = mem[0x235B + X]; mem[0x00B0] = A;            /* a215-a218 */
    }
    /* L_a22c — normalize */
    int half2_divide = 0;
    for (;;) {
        A = mem[0x00B1];                                 /* a22c */
        if (A < mem[0x00AF]) { half2_divide = 1; break; }   /* a22e/a230 BCC -> a262 */
        if (A == mem[0x00AF]) {                          /* a232 BNE */
            A = mem[0x00B0];                             /* a234 */
            if (A < mem[0x00AE]) { half2_divide = 1; break; }  /* a236/a238 BCC -> a262 */
        }
        ASLM_(0x00AE); ROLM_(0x00AF);                    /* a23a-a23c */
        Y = (uint8_t)(Y + 1);                            /* a23e INY */
        if (Y == 0x08) break;                            /* a23f CPY #8; a241 BNE -> a243 */
    }
    if (!half2_divide) {
        /* a243 — overflow */
        A = mem[0x2388 + X];                            /* a243 */
        if (A & 0x80) { mem[0x2487 + X] = 0xC0; mem[0x245A + X] = 0x00; }  /* a255 */
        else          { mem[0x2487 + X] = 0x40; mem[0x245A + X] = 0x00; }  /* a248 */
    } else {
        /* a262 — divide (see half-1 note: only the quotient feeds the fold) */
        mem[0x009F] = Y;                                 /* a262 */
        mem[0x00B2] = divide_16x16_core(rd16(0x00B0), rd16(0x00AE)).quotient;  /* a264 */
        Y = mem[0x009F];                                 /* a267 */
        Y = (uint8_t)(Y - 1);                            /* a269 DEY */
        if (Y & 0x80) {                                  /* a26a BMI -> a2b4 */
            mem[0x2487 + X] = 0x00;                      /* a2b4-a2b6 */
            A = mem[0x2388 + X];                        /* a2b9 */
            if (A & 0x80) {                              /* a2bc BMI -> a2cd */
                A = mem[0x00B2]; LSRA_(); c = 1; ADC_(mem[0x00B2]);  /* a2cd-a2d1 */
                RORA_(); LSRA_(); A ^= 0xFF;             /* a2d3-a2d5 */
                c = 1; ADC_(0x80); mem[0x245A + X] = A;  /* a2d7-a2da */
            } else {
                A = mem[0x00B2]; LSRA_(); c = 1; ADC_(mem[0x00B2]);  /* a2be-a2c2 */
                RORA_(); LSRA_(); A |= 0x80; mem[0x245A + X] = A;    /* a2c4-a2c8 */
                /* a2cb BNE always taken -> a2dd */
            }
        } else {
            A = 0x00;                                    /* a26c */
            do { ASLM_(0x00B2); ROLA_(); Y = (uint8_t)(Y - 1); }     /* a26e-a270 */
            while (!(Y & 0x80));                          /* a271-a272 DEY; BPL */
            mem[0x00B5] = A;                             /* a274 */
            LSRA_(); Y = A;                              /* a276-a277 */
            A = mem[0x00B2]; RORA_();                    /* a278-a27a */
            c = 0; ADC_(mem[0x00B2]); mem[0x00B2] = A;    /* a27b-a27e */
            A = Y; ADC_(mem[0x00B5]);                    /* a280-a281 */
            LSRA_(); RORM_(0x00B2);                      /* a283-a284 */
            LSRA_(); RORM_(0x00B2);                      /* a286-a287 */
            mem[0x00B5] = A;                             /* a289 */
            A = mem[0x2388 + X];                        /* a28b */
            if (A & 0x80) {                              /* a28e BMI -> a2a2 */
                A = 0x80; c = 1; SBC_(mem[0x00B2]); mem[0x245A + X] = A;  /* a2a2-a2a7 */
                A = 0x00; SBC_(mem[0x00B5]); mem[0x2487 + X] = A;        /* a2aa-a2ae */
            } else {
                A = 0x80; c = 0; ADC_(mem[0x00B2]); mem[0x245A + X] = A;  /* a290-a295 */
                A = mem[0x00B5]; ADC_(0x00); mem[0x2487 + X] = A;        /* a298-a29c */
            }
        }
    }

    /* a2dd — tail: add the per-screen-band scroll offset (signed) into {$245A:$2487}[X].
       The offset source and its sign select the high-byte addend (0x00 vs 0xFF). */
    {
        uint8_t off;
        A = mem[0x242D + X];                            /* a2dd */
        if (A & 0x80) {                                  /* a2e0 BMI -> a310 */
            off = mem[0x270E];                          /* a310 */
        } else if (A != 0) {                             /* a2e2 BNE -> a317 */
            off = mem[0x272D];                          /* a317 */
        } else {
            A = mem[0x2400 + X];                        /* a2e4 */
            LSRA_(); LSRA_(); LSRA_();                    /* a2e7-a2e9 (>>3) */
            Y = A;                                       /* a2ea TAY */
            off = mem[0x270E + Y];                       /* a2eb */
        }
        A = off;
        c = 0; ADC_(mem[0x245A + X]); mem[0x245A + X] = A;       /* a2f0 / a300 */
        A = mem[0x2487 + X]; ADC_((off & 0x80) ? 0xFF : 0x00);   /* a2f7 #$00 / a307 #$FF */
        mem[0x2487 + X] = A;
        return;                                          /* a2ff / a30f */
    }
    /* a31e fall-through into terrain_draw_frame is unreachable */

    #undef ADC_
    #undef SBC_
    #undef ASLM_
    #undef ROLM_
    #undef RORM_
    #undef ROLA_
    #undef RORA_
    #undef LSRA_
}

/* terrain_collision @ $AE53 — terrain collision/silhouette fill (flight top #4).
 *
 * Input: cpu.X = starting column.  Iterates 42 columns (Y = X..X+41), and for each:
 *  - CASCADE: scan the 48 terrain rows ($1010, stride $60) top-to-bottom for the
 *    first non-empty cell; its index k (0..$2F, or $30 if all empty) is the
 *    collision row.
 *  - WATERFALL: paint $55 into rows k-1..1 (everything above the hit; row 0 is left
 *    alone — the original jumps into a fall-through chain of stores that does
 *    exactly this).  k=0/1 paint nothing.
 *  - RASTER ($B12F): walk the column's bitmap downward from base ptr {$0793:$073D}[k]
 *    via ($80),Y stepping $60, OR-ing in the voxel masks ($BF00[]) and chaining the
 *    fill mask $96 through the $BE00[] table until $BE00[X]&$96 == 0.
 *
 * The cascade/waterfall are written as plain loops (byte-equivalent to the 6502's
 * unrolled compare ladder + store fall-through, confirmed by the harness); the
 * raster loop is a faithful transliteration.  The $B141 loop only terminates on
 * real terrain tables, so it is validated against a flight RAM snapshot.
 * Contract: memory only (caller reloads regs).
 */
void terrain_collision(void) {
    uint8_t A, X, Y, c;
    Y = cpu.X;                                           /* ae53 TXA; ae54 TAY */
    mem[0x009F] = 0x2A;                                  /* ae55-ae57 (42 columns) */

    for (;;) {                                           /* L_ae59 — one column */
        int k = 0x30;                                    /* all-empty -> $30 */
        for (int i = 0; i < 0x30; i++)                   /* LDA #0; CMP $ROW,Y ladder */
            if (mem[(0x1010 + i * 0x60) + Y] != 0) { k = i; break; }
        for (int i = k - 1; i >= 1; i--)                 /* waterfall: $55 into rows k-1..1 */
            mem[(0x1010 + i * 0x60) + Y] = 0x55;

        X = (uint8_t)k;                                  /* b12f */
        mem[0x0080] = mem[0x073D + X];                   /* b12f-b132 */
        mem[0x0081] = mem[0x0793 + X];                   /* b134-b137 */
        mem[0x0095] = 0x00;                              /* b139-b13b */
        mem[0x0096] = 0x55;                              /* b13d-b13f */
        for (;;) {                                       /* L_b141 */
            uint16_t addr = (uint16_t)(mem[0x0080] | (mem[0x0081] << 8)) + Y;
            A = bus_read(addr);                          /* b141 LDA ($80),Y */
            X = A;                                        /* b143 TAX */
            A &= mem[0x0095];                            /* b144 AND $95 */
            A |= mem[0x0096];                            /* b146 ORA $96 */
            A |= mem[0xBF00 + X];                        /* b148 ORA $BF00,X */
            bus_write(addr, A);                          /* b14b STA ($80),Y */
            A = mem[0xBE00 + X];                         /* b14d LDA $BE00,X */
            A &= mem[0x0096];                            /* b150 AND $96 */
            if (A == 0) break;                            /* b152 BEQ b169 */
            mem[0x0096] = A;                             /* b154 STA $96 */
            X = A;                                        /* b156 TAX */
            mem[0x0095] = mem[0xBE00 + X];               /* b157-b15a */
            c = 0; A = mem[0x0080];                      /* b15c CLC; b15d LDA $80 */
            { uint16_t t = (uint16_t)A + 0x60 + c; c = (uint8_t)(t >> 8); A = (uint8_t)t; }  /* b15f ADC #$60 */
            mem[0x0080] = A;                             /* b161 */
            if (!c) continue;                             /* b163 BCC b141 */
            mem[0x0081] = (uint8_t)(mem[0x0081] + 1);    /* b165 INC $81 (C unchanged) */
            /* b167 BCS b141 — C still set here, so always loops */
        }
        Y = (uint8_t)(Y + 1);                            /* b169 INY */
        mem[0x009F] = (uint8_t)(mem[0x009F] - 1);        /* b16a DEC $9F */
        if (mem[0x009F] == 0) return;                     /* b16c BEQ b171 */
    }                                                    /* b16e goto ae59 */
}

/* terrain_draw_frame @ $A31E — main per-frame terrain driver (flight top #5, the last).
 *
 * Input: cpu.X = level base index (saved to $00A7).  Phases:
 *  1. INIT: fill the $BD00 column-id table (2 ids x 8 cells, Y=$20..$D0 step 8),
 *     and the $263A/$26CE and $264E/$266F/$2690/$26B1 work arrays.
 *  2. SETUP: stash $0028/$0029, compute_row_xspans, seed span/clip accumulators.
 *  3. OBJECT LOOP ($A3AB, Y over the $B67C draw order until $90): per active pair,
 *     project_terrain_points + terrain_plot_object, then terrain_subdivide_column (the
 *     fractal subdivision/column renderer).  All callees native.
 *  4. TAIL: altitude/pitch/scroll game-state math, check_target_in_window,
 *     obj_table_set_active, the $0A00 object bump, the random enemy-spawn path
 *     (terrain_jitter_column), reading POKEY RANDOM $D20A several times.
 *
 * Structured C, locals A/X/Y/c (6502 op order preserved so carry threads identically).
 * Native callees that consume a
 * register get cpu.X set first: project_terrain_points/terrain_plot_object take the
 * object index, terrain_subdivide_column takes 0, the transpiled ring_push_marked takes $14.
 * Reads $D20A (harness seeds it identically per case).  Contract: memory only.
 */
void terrain_draw_frame(void) {
    uint8_t A, X, Y, c = 0;
    #define ADC_(v)  do { uint16_t _t=(uint16_t)A+(uint8_t)(v)+c; c=(uint8_t)(_t>>8); A=(uint8_t)_t; } while(0)
    #define SBC_(v)  ADC_((uint8_t)~(uint8_t)(v))
    #define ASLA_()  do { c=A>>7; A=(uint8_t)(A<<1); } while(0)
    #define ROLA_()  do { uint8_t _n=A>>7; A=(uint8_t)((A<<1)|c); c=_n; } while(0)
    #define RORA_()  do { uint8_t _n=A&1; A=(uint8_t)((A>>1)|(c<<7)); c=_n; } while(0)
    #define LSRA_()  do { c=A&1; A=(uint8_t)(A>>1); } while(0)
    #define ASLM_(a) do { uint8_t _v=mem[a]; c=_v>>7; mem[a]=(uint8_t)(_v<<1); } while(0)
    #define ROLM_(a) do { uint8_t _v=mem[a],_n=_v>>7; mem[a]=(uint8_t)((_v<<1)|c); c=_n; } while(0)

    mem[0x00A7] = cpu.X;                                 /* a31e */
    X = cpu.X;
    Y = 0x20; c = 0;                                     /* a320-a322 */
    do {                                                 /* L_a323 — fill the $BD00 column-id table */
        A = X; mem[0xBD00+Y]=A; mem[0xBD01+Y]=A; mem[0xBD02+Y]=A; mem[0xBD03+Y]=A;  /* a323-a32d */
        X = (uint8_t)(X+1);                              /* a330 */
        A = X; mem[0xBD04+Y]=A; mem[0xBD05+Y]=A; mem[0xBD06+Y]=A; mem[0xBD07+Y]=A;  /* a331-a33b */
        X = (uint8_t)(X+1);                              /* a33e */
        A = Y; ADC_(0x08); Y = A;                        /* a33f-a342 (c=0 from CLC/CPY) */
        c = (Y >= 0xD4) ? 1 : 0;                         /* a343 CPY #$D4 */
    } while (Y < 0xD4);                                  /* a345 BCC */
    A = X; c = 0; ADC_(0x2E); mem[0x00B3] = A;           /* a347-a34b */
    Y = 0x14; A = 0x67;                                  /* a34d-a34f */
    do {                                                 /* L_a351 */
        Y = (uint8_t)(Y-1); mem[0x263A+Y]=A; mem[0x26CE + Y]=A;  /* a351-a355 */
    } while (Y != 0);                                    /* a358 BNE */
    Y = 0x21; A = 0x6B;                                  /* a35a-a35c */
    do {                                                 /* L_a35e */
        Y = (uint8_t)(Y-1);                              /* a35e */
        mem[0x264E + Y]=A; mem[0x266F+Y]=A; mem[0x2690+Y]=A; mem[0x26B1+Y]=A;  /* a35f-a368 */
    } while (Y != 0);                                    /* a36b BNE */

    mem[0x2907] = mem[0x0028];                           /* a36d-a36f */
    mem[0x2908] = mem[0x0029];                           /* a372-a374 */
    compute_row_xspans();                                /* a377 */
    A = 0x80;                                            /* a37a */
    mem[0x28E7]=A; mem[0x28E8]=A; mem[0x2912]=A; mem[0x2913]=A; mem[0x2914]=A; mem[0x2915]=A;  /* a37c-a38b */
    Y = mem[0x00A7];                                     /* a38e */
    if (Y == 0) { mem[0x28EA]=A; mem[0x0079]=A; }        /* a390 BNE a397; else a392-a395 */
    A = 0x00;                                            /* a397 */
    mem[0x28ED]=A; mem[0x28FB]=A; mem[0x28FC]=A;         /* a399-a39f */
    A = mem[0x006A];                                     /* a3a2 */
    if (!(A & 0x80)) mem[0x28FC] = (uint8_t)(mem[0x28FC]+1);  /* a3a4 BMI a3a9; else a3a6 INC */

    Y = 0x00;                                            /* a3a9 */
    for (;;) {                                           /* L_a3ab — object draw-order loop */
        A = mem[0xB67C+Y]; Y = (uint8_t)(Y+1); X = A; mem[0x28DB] = X;  /* a3ab-a3b0 */
        A = mem[0x24B4+X];                               /* a3b3 */
        if ((A & 0x80) || (A & 0x20)) {                  /* a3b6 BMI / a3b8 AND#$20 -> L_a42b */
            Y = (uint8_t)(Y+1);                          /* a42b INY */
        } else {
            A = mem[0xB67C+Y]; Y = (uint8_t)(Y+1); mem[0x272E] = Y; X = A;  /* a3bc-a3c3 */
            A = mem[0x24B4+X];                           /* a3c4 */
            if (!((A & 0x80) || (A & 0x40))) {           /* a3c7 BMI / a3c9 AND#$40 -> L_a42c */
                if (!(mem[0x24B4+X] & 0x10)) {           /* a3cd BNE L_a3da -> else project */
                    cpu.X = X; project_terrain_points(); cpu.X = X; terrain_plot_object();  /* a3d4-a3d7 */
                }
                /* a3da */
                mem[0x25B4]=mem[0x2400+X]; mem[0x25D2]=mem[0x242D+X]; mem[0x25F0]=mem[0x245A+X];  /* a3da-a3ec */
                mem[0x24E2]=mem[0x2487+X]; mem[0x23E2]=mem[0x23B5+X];  /* a3ef-a3f5 */
                X = mem[0x28DB];                         /* a3f8 */
                if (!(mem[0x24B4+X] & 0x10)) {           /* a3fb BNE L_a408 -> else project */
                    cpu.X = X; project_terrain_points(); cpu.X = X; terrain_plot_object();  /* a402-a405 */
                }
                /* a408 */
                mem[0x0082]=mem[0x2400+X]; mem[0x0083]=mem[0x242D+X]; mem[0x0084]=mem[0x245A+X];  /* a408-a415 */
                mem[0x0085]=mem[0x2487+X]; mem[0x0086]=mem[0x23B5+X];  /* a417-a41f */
                cpu.X = 0x00; terrain_subdivide_column();             /* a421-a423 */
                Y = mem[0x272E];                         /* a426 */
                if (Y == 0) Y = (uint8_t)(Y+1);          /* a429 BNE L_a42c; else a42b INY */
            }
        }
        /* a42c */
        if (Y == 0x90) break;                            /* CPY#$90; BEQ a433 */
    }

    /* a433 */
    mem[0x28D9] = mem[0x28E7];                           /* a433-a436 */
    mem[0x28DA] = mem[0x28E8];                           /* a439-a43c */
    A = mem[0x0079];                                     /* a43f */
    if (!(A & 0x80)) {                                   /* a441 BPL a44b */
        LSRA_(); LSRA_(); c = 0; ADC_(0x01);             /* a44b-a44e */
        if (A >= 0x0A) A = 0x09;                         /* a450 CMP#$0A; a452 BCC a456; a454 */
        mem[0x0642] = A;                                 /* a456 */
        c = (A >= mem[0x283F]) ? 1 : 0;                  /* a459 CMP $283F */
        if (A != mem[0x283F]) {                          /* a45c BEQ a46c -> else */
            mem[0x283F] = A;                             /* a45e */
            if (!c && mem[0x003D] == 0) {                /* a461 BCS a46c / a463 BNE a46c */
                cpu.X = 0x14; ring_push_marked();        /* a467-a469 */
            }
        }
    } else {
        mem[0x0642] = 0x00;                              /* a443-a445 */
    }
    /* a46c */
    A = mem[0x2912]; c = (A >= 0x80) ? 1 : 0; RORA_(); c = 0; ADC_(0x7D); mem[0x2847] = A;  /* a46c-a475 */
    A = mem[0x2913]; c = 0; ADC_(0x15); mem[0x2845] = A;  /* a478-a47e */
    mem[0x2916] = mem[0x2914];                           /* a481-a484 */
    mem[0x2839] = mem[0x2910];                           /* a487-a48a */
    mem[0x283A] = mem[0x2911];                           /* a48d-a490 */
    if (mem[0x0004] != 0) check_target_in_window();      /* a493 BEQ a49a; a497 */
    A = (mem[0x28FC] != 0) ? 0x74 : mem[0x28FC];         /* a49a-a49f (LDA $28FC; BEQ; LDA #$74) */
    mem[0x2840] = A;                                     /* a4a1 */
    mem[0x28FD]=mem[0x2270]; mem[0x28FE]=mem[0x2271]; mem[0x28FF]=mem[0x2272];  /* a4a4-a4b3 */
    mem[0x2900]=mem[0x2273]; mem[0x2901]=mem[0x2274]; mem[0x2902]=mem[0x2275];  /* a4b6-a4c5 */
    mem[0x2903]=mem[0x2809]; mem[0x2904]=mem[0x280A]; mem[0x2905]=mem[0x280B]; mem[0x2906]=mem[0x280C];  /* a4c8-a4dd */
    mem[0x2909]=mem[0x2907]; mem[0x290A]=mem[0x2908];    /* a4e0-a4e9 */
    Y = 0x1F;                                            /* a4ec */
    do {                                                 /* L_a4ee */
        c = 1; A = mem[0x271E]; SBC_(mem[0x270E + Y]);     /* a4ee-a4f2 */
        c = (A >= 0x80) ? 1 : 0; RORA_(); ADC_(0x00);    /* a4f5-a4f8 (sign-ext shift + round) */
        mem[0x28B6+Y] = A;                               /* a4fa */
        Y = (uint8_t)(Y-1);                              /* a4fd DEY */
    } while (!(Y & 0x80));                               /* a4fe BPL */

    int a562_keep_carry = 0;                             /* arriving via a509 keeps c=1 into a562 */
    if (mem[0x28FB] == 0) {                              /* a500 LDA $28FB; a503 BNE a50c */
        c = 1; A = mem[0x061C];                          /* a505-a506 SEC; LDA $061C */
        a562_keep_carry = 1;                             /* a509 goto a562 */
    } else if (mem[0x003D] != 0) {
        A = 0x00;                                        /* a50c-a512 goto a561 */
    } else {
        A = mem[0x0025]; ASLA_(); mem[0x00C3] = A;       /* a515-a518 */
        A = mem[0x0026]; ROLA_(); ASLM_(0x00C3); ROLA_(); ASLM_(0x00C3); ROLA_(); mem[0x00C3] = A;  /* a51a-a523 */
        A = mem[0x2919]; ASLA_(); mem[0x00C4] = A;       /* a525-a529 */
        A = mem[0x291A]; ROLA_(); ASLM_(0x00C4); ROLA_(); ASLM_(0x00C4); ROLA_();  /* a52b-a534 */
        c = 1; SBC_(mem[0x00C3]);                        /* a535-a536 SEC; SBC $C3 */
        if (A & 0x80) { c = 0; A ^= 0xFF; ADC_(0x01); }  /* a538 BPL a53f; a53a-a53d negate */
        mem[0x00C3] = A;                                 /* a53f */
        A = mem[0x0027]; c = (A>=0x80)?1:0; RORA_(); c = (A>=0x80)?1:0; RORA_(); mem[0x00C4] = A;  /* a541-a549 */
        A = mem[0x291B]; c = (A>=0x80)?1:0; RORA_(); c = (A>=0x80)?1:0; RORA_();  /* a54b-a553 */
        c = 1; SBC_(mem[0x00C4]);                        /* a554-a555 SEC; SBC $C4 */
        if (A & 0x80) { c = 0; A ^= 0xFF; ADC_(0x01); }  /* a557 BPL a55e; negate */
        c = 0; ADC_(mem[0x00C3]);                        /* a55e CLC; ADC $C3 */
    }
    /* a561 */
    if (!a562_keep_carry) c = 0;                         /* a561 CLC (skipped when arriving via a562) */
    /* a562 */
    ADC_(mem[0x004D]);                                   /* a562 ADC $4D */
    if (A & 0x80) A = 0x7F;                              /* a564 BPL a568; a566 */
    c = 1; SBC_(mem[0x061C]);                            /* a568-a569 SEC; SBC $061C */
    if (!c) A = 0x00;                                    /* a56c BCS a570; a56e */
    mem[0x004D] = A;                                     /* a570 */
    Y = mem[0x007E];                                     /* a572 */
    c = (A >= 0x20) ? 1 : 0;                             /* a574 CMP #$20 */
    if (c) {                                             /* a576 BCC a580 -> else branch */
        if (!(Y & 0x80)) Y = 0x80;                       /* a578 TYA; a579 BMI a57d; a57b LDY#$80 */
    } else {                                             /* a580 */
        if (Y == 0x80) Y = 0x00;                         /* a580 CPY#$80; a582 BNE a586; a584 */
    }
    mem[0x007E] = Y;                                     /* a586 */
    if (mem[0x0041] != 0) {                              /* a588 BEQ a59b */
        c = 1; A = mem[0x006D]; SBC_(0x06);              /* a58c-a58f SEC; LDA $6D; SBC #6 */
        if (c) {                                         /* a591 BCC a59b */
            c = (A >= bus_read(0xD20A)) ? 1 : 0;         /* a593 CMP $D20A */
            if (c) obj_table_set_active();               /* a596 BCC a59b; a598 */
        }
    }
    /* a59b */
    if (mem[0x2843] != 0) {                              /* a59b BEQ a5b5 */
        X = 0x00; mem[0x2843] = X; A = 0xF9;             /* a5a0-a5a5 */
        do {                                             /* L_a5a7 */
            if (A < mem[0x0A00+X]) {                     /* a5a7 CMP; a5aa BCS a5b2 -> else */
                mem[0x0A00+X] = (uint8_t)(mem[0x0A00+X]+1);  /* a5ac INC $0A00,X */
                mem[0x2843] = (uint8_t)(mem[0x2843]+1);  /* a5af INC $2843 */
            }
            X = (uint8_t)(X+1);                          /* a5b2 INX */
        } while (X != 0);                                /* a5b3 BNE */
    }
    /* a5b5 — random enemy-spawn path; any failed guard returns (a612) */
    X = mem[0x0063];                                     /* a5b5 */
    if (!(X & 0x80)) return;                             /* a5b7 BPL a612 */
    if (mem[0x003D] != 0) return;                        /* a5b9-a5bb BNE a612 */
    A = mem[0x0621];                                     /* a5bd */
    if (A == 0) return;                                  /* a5c0 BEQ a612 */
    mem[0x0622] = (uint8_t)(mem[0x0622]-1);              /* a5c2 DEC $0622 */
    if (mem[0x0622] != 0) return;                        /* a5c5 BNE a612 */
    mem[0x0622] = A;                                     /* a5c7 STA $0622 (A=$0621) */
    c = 0; A = bus_read(0xD20A);                         /* a5ca CLC; LDA $D20A */
    if (A & 0x80) return;                                /* a5ce BMI a612 */
    ADC_(0x40); X = A;                                   /* a5d0-a5d2 ADC #$40 (c=0); TAX */
    A = bus_read(0xD20A); A &= 0x1F; ADC_(0x6E);         /* a5d3-a5d8 (ADC #$6E carry from a5d0) */
    c = (A >= mem[0x260E + X]) ? 1 : 0;                    /* a5da CMP $260E,X */
    if (!c) return;                                      /* a5dd BCC a612 */
    if (A == mem[0x260E + X]) return;                     /* a5df BEQ a612 */
    mem[0x0066] = A;                                     /* a5e1 */
    A = 0x80; SBC_(mem[0x0066]);                         /* a5e3-a5e5 (c=1 from a5da CMP) */
    ASLA_(); c = 0; ADC_(0x42); mem[0x0066] = A;         /* a5e7-a5eb */
    A = X; c = 1; SBC_(0x10); mem[0x0064] = A;           /* a5ed-a5f1 TXA; SEC; SBC #$10 */
    A = 0x00; mem[0x0065]=A; mem[0x0067]=A; mem[0x0068]=A; mem[0x0069]=A;  /* a5f3-a5fb */
    terrain_jitter_column();                                  /* a5fd */
    A = 0x7F; mem[0x006A]=A; mem[0x0063]=A; mem[0x2845]=A;  /* a600-a606 */
    mem[0x0035] = 0x01;                                  /* a609-a60b */
    mem[0x282D] = mem[0x0034];                           /* a60d-a60f */
    return;                                              /* a612 */

    #undef ADC_
    #undef SBC_
    #undef ASLA_
    #undef ROLA_
    #undef RORA_
    #undef LSRA_
    #undef ASLM_
    #undef ROLM_
}

/* ===========================================================================
 * flight_control_integrate subtree — ISR de-transpile (the last transpiled code
 * on the flight VBI path).  Leaves first.  See [[flight-must-be-native]].
 * ======================================================================== */

/* reset_flags_ff @ $4E98 — LDA #$FF then STA $006A/$0063/$2826.
 * All four callers overwrite A or branch immediately after → mem-only contract. */
void reset_flags_ff(void) {
    mem[0x006A] = 0xFF;
    mem[0x0063] = 0xFF;
    mem[0x2826] = 0xFF;
}

/* load_velocity_from_param_block @ $94BF — seed the object velocity accumulators
 * $2854-$2863 from the param block $28FD-$290A: zero the low fraction bytes, copy
 * the mid bytes, clamp $285B = min($2902+8, $FF), sign-extend-and-x4 the two 16-bit
 * components {$2903,$2904}->$285C-$285E and {$2905,$2906}->$285F-$2861, build
 * $2862/$2863 from {$2909,$290A} (arithmetic >>1 via CMP#$80;ROR, then -1), then
 * tail-push X=$11 to the event ring.  Outputs are mem[]; the ring push goes through
 * the same native ring_push_marked the oracle calls, so it's equivalence-safe. */
void load_velocity_from_param_block(void) {
    uint8_t A, Y, c;
    #define ADC_(v)  do { uint16_t _t=(uint16_t)A+(uint8_t)(v)+c; c=(uint8_t)(_t>>8); A=(uint8_t)_t; } while(0)
    #define SBC_(v)  ADC_((uint8_t)~(uint8_t)(v))
    #define ROLA_()  do { uint8_t _n=A>>7; A=(uint8_t)((A<<1)|c); c=_n; } while(0)
    #define RORA_()  do { uint8_t _n=A&1; A=(uint8_t)((A>>1)|(c<<7)); c=_n; } while(0)
    #define ASLM_(a) do { uint8_t _v=mem[a]; c=_v>>7; mem[a]=(uint8_t)(_v<<1); } while(0)
    #define ROLM_(a) do { uint8_t _v=mem[a],_n=_v>>7; mem[a]=(uint8_t)((_v<<1)|c); c=_n; } while(0)
    #define RORM_(a) do { uint8_t _v=mem[a],_n=_v&1; mem[a]=(uint8_t)((_v>>1)|(c<<7)); c=_n; } while(0)

    mem[0x2854] = 0x00;                       /* 94bf-94c1 */
    mem[0x2857] = 0x00;                       /* 94c4 */
    mem[0x2855] = mem[0x28FD];                /* 94c7-94ca */
    mem[0x2856] = mem[0x28FE];                /* 94cd-94d0 */
    mem[0x2858] = mem[0x28FF];                /* 94d3-94d6 */
    mem[0x2859] = mem[0x2900];                /* 94d9-94dc */
    mem[0x285A] = mem[0x2901];                /* 94df-94e2 */
    c = 0; A = mem[0x2902]; ADC_(0x08);       /* 94e5 CLC; 94e6 LDA $2902; 94e9 ADC #$08 */
    if (c) A = 0xFF;                          /* 94eb BCC; 94ed LDA #$FF */
    mem[0x285B] = A;                          /* 94ef */

    Y = 0x00;                                 /* 94f2 */
    A = mem[0x2903]; mem[0x285C] = A;         /* 94f4-94f7 */
    A = mem[0x2904]; mem[0x285D] = A;         /* 94fa-94fd */
    if (A & 0x80) Y--;                        /* 9500 BPL; 9502 DEY (N from LDA $2904) */
    A = Y;                                    /* 9503 TYA */
    ASLM_(0x285C); ROLM_(0x285D); ROLA_();    /* 9504/9507/950a */
    ASLM_(0x285C); ROLM_(0x285D); ROLA_();    /* 950b/950e/9511 */
    mem[0x285E] = A;                          /* 9512 */

    Y = 0x00;                                 /* 9515 */
    A = mem[0x2905]; mem[0x285F] = A;         /* 9517-951a */
    A = mem[0x2906]; mem[0x2860] = A;         /* 951d-9520 */
    if (A & 0x80) Y--;                        /* 9523 BPL; 9525 DEY */
    A = Y;                                    /* 9526 TYA */
    ASLM_(0x285F); ROLM_(0x2860); ROLA_();    /* 9527/952a/952d */
    ASLM_(0x285F); ROLM_(0x2860); ROLA_();    /* 952e/9531/9534 */
    mem[0x2861] = A;                          /* 9535 */

    mem[0x2862] = mem[0x2909];                /* 9538-953b */
    A = mem[0x290A];                          /* 953e */
    c = (A >= 0x80) ? 1 : 0;                  /* 9541 CMP #$80 -> carry = A>=$80 */
    RORA_();                                  /* 9543 ROR A (arithmetic >>1) */
    RORM_(0x2862);                            /* 9544 ROR $2862 */
    c = 1; SBC_(0x01);                        /* 9547 SEC; 9548 SBC #$01 */
    mem[0x2863] = A;                          /* 954a */

    cpu.X = 0x11;                             /* 954d LDX #$11 */
    ring_push_marked();                       /* 954f (native; reads cpu.X) */

    #undef ADC_
    #undef SBC_
    #undef ROLA_
    #undef RORA_
    #undef ASLM_
    #undef ROLM_
    #undef RORM_
}

/* bcd_inc_counter_0641 @ $7B88 — CLC; SED; ADC #1; CLD.  The cpu core ignores
 * decimal mode ("decimal ignored in game"), so the oracle does a plain BINARY +1;
 * match it (the whole transpiled game runs binary here). mem-only contract. */
void bcd_inc_counter_0641(void) {
    mem[0x0641] = (uint8_t)(mem[0x0641] + 1);
}

/* set_place_params_inc_count @ $7B80 — $0045=0, $0046=1, then bump the counter. */
void set_place_params_inc_count(void) {
    mem[0x0045] = 0x00;
    mem[0x0046] = 0x01;
    bcd_inc_counter_0641();                   /* 7b8d tail (native) */
}

/* trigger_object_explosion @ $96D9 — INC $0041, seed the explosion sprite/anim
 * pointers ($00DA-$00DD, $28EE), then push X=$0F to the event ring. */
void trigger_object_explosion(void) {
    mem[0x0041] = (uint8_t)(mem[0x0041] + 1);  /* 96d9 INC $0041 */
    mem[0x00DB] = 0x7E;                        /* 96db-96dd */
    mem[0x00DD] = 0x7C;                        /* 96df-96e1 */
    mem[0x00DC] = 0x76;                        /* 96e3-96e5 */
    mem[0x00DA] = 0x78;                        /* 96e7-96e9 */
    mem[0x28EE] = 0x02;                        /* 96eb-96ed */
    cpu.X = 0x0F;                              /* 96f0 LDX #$0F */
    ring_push_marked();                        /* 96f2 (native; reads cpu.X) */
}

/* reset_object_slot @ $9677 — mark object slot $0036=$80, push X=$0E to the ring. */
void reset_object_slot(void) {
    mem[0x0036] = 0x80;                        /* 9677-9679 */
    cpu.X = 0x0E;                              /* 967b LDX #$0E */
    ring_push_marked();                        /* 967d (native; reads cpu.X) */
}

/* enqueue_indicator_event @ $B756 — write indicator HUD params ($0673 from $0035,
 * $0665=$A0, $0681 = ($0014 & 5) + 2), then refresh display field Y=$08. */
void enqueue_indicator_event(void) {
    mem[0x0673] = mem[0x0035];                 /* b756-b758 */
    mem[0x0665] = 0xA0;                        /* b75b-b75d */
    mem[0x0681] = (uint8_t)((mem[0x0014] & 0x05) + 0x02);  /* b760-b767 (AND#5 max 5, +2 no carry) */
    cpu.Y = 0x08;                              /* b76a LDY #$08 */
    game_sub_55FC();                           /* b76c (native; reads cpu.Y) */
}

/* object_integrate_position @ $930E — integrate an object's 24-bit world position.
 * Four add/subtract blocks update {$2827,$0064,$0065} and {$2828,$0066,$0067}: subtract
 * the velocity $2850/$2851 and decel $2829/$0068; add velocity $2852/$2853 and $282C/$0069.
 * Each block sign-extends its operand's high byte into the 3rd byte via a carry-conditional
 * INC/DEC.  Tail: if both high bytes settled to 0, derive the screen blip $2821 from a
 * heading table $93F3[$0063] (arith >>1 gated by $282D) + $0066, and $2824 from $0064;
 * else $2824 = 0.  mem-only contract (starts SEC; X/Y loaded from mem; no RANDOM). */
void object_integrate_position(void) {
    uint8_t A, X, Y, c;
    #define ADC_(v) do { uint16_t _t=(uint16_t)A+(uint8_t)(v)+c; c=(uint8_t)(_t>>8); A=(uint8_t)_t; } while(0)
    #define SBC_(v) ADC_((uint8_t)~(uint8_t)(v))
    #define RORA_() do { uint8_t _n=A&1; A=(uint8_t)((A>>1)|(c<<7)); c=_n; } while(0)

    /* block 1: subtract velocity $2850/$2851 (BPL form) */
    c = 1; A = mem[0x2827]; SBC_(mem[0x2850]); mem[0x2827] = A;   /* 930e-9315 */
    A = mem[0x0064]; SBC_(mem[0x2851]); mem[0x0064] = A;          /* 9318-931d */
    A = mem[0x2851];                                             /* 931f */
    if (A & 0x80) { if (c)  mem[0x0065]++; }                     /* 9322 BPL: N set -> if c INC */
    else          { if (!c) mem[0x0065]--; }                     /*          N clear -> if !c DEC */

    /* block 2: add velocity $2852/$2853 (BMI form) */
    c = 0; A = mem[0x2828]; ADC_(mem[0x2852]); mem[0x2828] = A;   /* 932f-9336 */
    A = mem[0x0066]; ADC_(mem[0x2853]); mem[0x0066] = A;          /* 9339-933e */
    A = mem[0x2853];                                             /* 9340 */
    if (A & 0x80) { if (!c) mem[0x0067]--; }                     /* 9343 BMI: N set -> if !c DEC */
    else          { if (c)  mem[0x0067]++; }                     /*          N clear -> if c INC */

    /* block 3: subtract decel $2829/$0068 (BPL form) */
    c = 1; A = mem[0x2827]; SBC_(mem[0x2829]); mem[0x2827] = A;   /* 9350-9357 */
    A = mem[0x0064]; SBC_(mem[0x0068]); mem[0x0064] = A;          /* 935a-935e */
    A = mem[0x0068];                                             /* 9360 */
    if (A & 0x80) { if (c)  mem[0x0065]++; }                     /* 9362 BPL */
    else          { if (!c) mem[0x0065]--; }

    /* block 4: add $282C/$0069 (BMI form) */
    c = 0; A = mem[0x2828]; ADC_(mem[0x282C]); mem[0x2828] = A;   /* 936f-9376 */
    A = mem[0x0066]; ADC_(mem[0x0069]); mem[0x0066] = A;          /* 9379-937d */
    A = mem[0x0069];                                             /* 937f */
    if (A & 0x80) { if (!c) mem[0x0067]--; }                     /* 9381 BMI */
    else          { if (c)  mem[0x0067]++; }

    /* tail: settled? */
    if (mem[0x0065] != 0 || mem[0x0067] != 0) {                  /* 938e/9392 BNE L_93b7 */
        mem[0x2824] = 0x00;                                      /* 93b7-93b9 */
        goto done;
    }
    X = mem[0x0063];                                            /* 9396 */
    A = mem[0x93F3 + X];                                        /* 9398 heading table */
    Y = mem[0x282D];                                           /* 939b */
    if (Y >= 0x30) A = 0x00;                                    /* 939e CPY#$30; BCC; LDA#0 */
    if (Y >= 0x20) { c = (A >= 0x80) ? 1 : 0; RORA_(); }        /* 93a4 CPY#$20; CMP#$80; ROR A */
    c = 0; ADC_(mem[0x0066]); mem[0x2821] = A;                  /* 93ab CLC; ADC $0066; STA $2821 */
    mem[0x2824] = mem[0x0064];                                  /* 93b1-93b4 */
done:
    #undef ADC_
    #undef SBC_
    #undef RORA_
    return;
}

/* jitter_roll_pitch @ $AA95 — per-frame random walk of the pitch ($0029) and roll
 * ($0026) accumulators, plus a decay of $002E.  $0029 drifts toward $F4 (down up to
 * 2, never past); $0026 walks toward $FB (down) when RANDOM is negative or toward $05
 * (up) when positive, up to 2 and never past; $002E -= 8 with a floor of 0.
 * Reads POKEY RANDOM $D20A once (harness seeds it identically).  mem-only contract. */
void jitter_roll_pitch(void) {
    uint8_t A, Y;
    Y = mem[0x0029];                                   /* aa95 */
    if (Y != 0xF4) { Y--; if (Y != 0xF4) Y--; }        /* aa97-aaa0 */
    mem[0x0029] = Y;                                   /* aaa1 */
    Y = mem[0x0026];                                   /* aaa3 */
    A = bus_read(0xD20A);                              /* aaa5 LDA $D20A */
    if (A & 0x80) { if (Y != 0xFB) { Y--; if (Y != 0xFB) Y--; } }  /* aaa8 BPL -> neg: toward $FB */
    else          { if (Y != 0x05) { Y++; if (Y != 0x05) Y++; } }  /*           pos: toward $05 */
    mem[0x0026] = Y;                                   /* aac1 */
    A = mem[0x002E];                                   /* aac4 SEC; SBC #$08; clamp 0 */
    mem[0x002E] = (A >= 0x08) ? (uint8_t)(A - 0x08) : 0x00;
}

/* mul_u8 @ $9821 — shift-add multiply driven by the multiplicand bits in $28D6: while
 * $006B != 0, shift $28D6 left and, for each 1 bit shifted out, add (the LSR'd) $006B
 * into the accumulator.  Both $006B and $28D6 are consumed (shifted to 0/out).  RESULT
 * IS RETURNED IN cpu.A — the caller reads it (e.g. BIT right after) — so the contract
 * is mem[$006B]/mem[$28D6] AND cpu.A. */
void mul_u8(void) {
    uint8_t A = 0x00, c = 0;                    /* 9821 LDA #0 */
    goto check;                                 /* 9823 BEQ (A==0) */
add:                                            /* 9825 */
    c = mem[0x006B] & 1; mem[0x006B] >>= 1;     /* LSR $006B */
    { uint16_t t = (uint16_t)A + mem[0x006B] + c; c = (uint8_t)(t >> 8); A = (uint8_t)t; }  /* ADC $006B */
check:                                          /* 9829 */
    { uint8_t v = mem[0x28D6]; c = (uint8_t)(v >> 7); mem[0x28D6] = (uint8_t)(v << 1); }    /* ASL $28D6 */
    if (c) goto add;                            /* 982c BCS */
    c = mem[0x006B] & 1; mem[0x006B] >>= 1;     /* 982e LSR $006B */
    if (mem[0x006B] != 0) goto check;           /* 9830 BNE (Z from the shifted result) */
    cpu.A = A;                                  /* 9832 RTS — result in A */
}

/* compute_target_blip_position @ $9713 — derive the target-blip screen coords $0021/$0027
 * from the nearest-point range latch $27F7/$27F8, the depth $0034, sensor masks
 * $1027/$1057, and the parallax samples $2912/$2913.  Several early returns.  mem-only
 * (starts SEC; all inputs from mem; no entry regs).  NOTE the $976b SBC reads a
 * PATH-DEPENDENT carry (0 when reached via the $975f BCC, 1 via the $9763 BMI). */
void compute_target_blip_position(void) {
    uint8_t A, X, c;
    #define ADC_(v)  do { uint16_t _t=(uint16_t)A+(uint8_t)(v)+c; c=(uint8_t)(_t>>8); A=(uint8_t)_t; } while(0)
    #define SBC_(v)  ADC_((uint8_t)~(uint8_t)(v))
    #define ASLA_()  do { c=(uint8_t)(A>>7); A=(uint8_t)(A<<1); } while(0)
    #define RORA_()  do { uint8_t _n=A&1; A=(uint8_t)((A>>1)|(c<<7)); c=_n; } while(0)
    #define LSRA_()  do { c=A&1; A=(uint8_t)(A>>1); } while(0)

    c = 1; A = mem[0x27F7]; SBC_(mem[0x27F8]); mem[0x0021] = A;   /* 9713-971a */
    c = 0; A = mem[0x27F7]; ADC_(mem[0x27F8]); LSRA_();           /* 971c-9723 */
    c = (A >= mem[0x0034]) ? 1 : 0;                              /* 9724 CMP $0034 */
    if (!c) {                                                    /* 9726 BCS L_9738 */
        A = mem[0x0021];                                        /* 9728 */
        if (A & 0x80) { A ^= 0xFF; ADC_(0x01); }                /* 972a BPL; 972c EOR; 972e ADC#1 (c=0) -> abs */
        c = (A >= 0x10) ? 1 : 0;                                /* 9730 CMP #$10 */
        if (!c) mem[0x0021] = 0x00;                             /* 9732 BCS; 9734-9736 */
    }
    /* L_9738 */
    if (mem[0x005D] == 0) {                                     /* 9738 LDA $005D; 973a BNE L_9741 */
        mem[0x0027] = 0xC8;                                     /* 973c-973e */
        goto out;
    }
    /* L_9741 */
    X = 0x38;                                                  /* 9741 */
    if ((mem[0x1027] & 0xAA) == 0 &&                            /* 9743-9748 */
        (mem[0x1057] & 0xAA) == 0 &&                            /* 974a-974f */
        mem[0x0070] >= 0x2A)                                   /* 9751-9755 BCC (C set => >=) */
        X = 0x00;                                              /* 9757 */
    mem[0x0027] = X;                                           /* 9759 STX $0027 */

    A = mem[0x0034];                                          /* 975b */
    c = (A >= 0x2A) ? 1 : 0;                                  /* 975d CMP #$2A */
    int useBlk2;
    if (!c) useBlk2 = 1;                                      /* 975f BCC L_9769 (c=0) */
    else {
        A = mem[0x0029];                                     /* 9761 */
        if (A & 0x80) useBlk2 = 1;                           /* 9763 BMI L_9769 (c stays 1) */
        else { A = 0xC8; useBlk2 = 0; }                      /* 9765 LDA #$C8; 9767 BNE L_9774 */
    }
    if (useBlk2) {                                            /* L_9769 */
        A = 0x20; SBC_(mem[0x0034]);                          /* 9769-976b (path-dependent c) */
        c = (A >= 0x80) ? 1 : 0;                              /* 976d CMP #$80 */
        RORA_();                                             /* 976f ROR A */
        if (!(A & 0x80)) A = 0x00;                           /* 9770 BMI L_9774; 9772 LDA #0 */
    }
    /* L_9774 */
    c = 0; ADC_(mem[0x0027]); mem[0x0027] = A;                /* 9774-9777 */

    A = mem[0x2912];                                         /* 9779 */
    if (A >= 0x30 && A < 0xD1) goto out;                     /* 977c-9784 in-band -> RTS */
    ASLA_();                                                 /* 9785 */
    c = 0; ADC_(mem[0x0021]); mem[0x0021] = A;                /* 9786-9789 */

    A = mem[0x2913];                                         /* 978b */
    if (A >= 0x20 && A < 0xE1) goto out;                     /* 978e-9796 in-band -> RTS */
    ASLA_();                                                 /* 9797 */
    A ^= 0xFF;                                               /* 9798 EOR #$FF */
    c = 0; ADC_(mem[0x0027]); mem[0x0027] = A;                /* 979a-979d */
out:
    #undef ADC_
    #undef SBC_
    #undef ASLA_
    #undef RORA_
    #undef LSRA_
    return;
}

/* obj_table_scan_replace @ $4E1C — place the entry value (cpu.A) into a free object
 * slot.  Retries up to cpu.Y times ($288C): each pass picks a RANDOM start index, then
 * scans with stride $43 (coprime to 256 -> visits all indices, returns to start) for a
 * slot with $0900,X < $30 and $0A00,X == 0 whose index isn't already in the $2276[0..$2C]
 * table; the first match gets the entry value written to $0A00,X.  Reads entry A/Y and
 * POKEY RANDOM $D20A (harness seeds it); mem-only effects ($281D/$288B/$288C/$0A00,X). */
void obj_table_scan_replace(void) {
    uint8_t X;
    mem[0x281D] = cpu.A;                          /* 4e1c entry value */
    mem[0x288C] = cpu.Y;                           /* 4e1f retry count */
    do {                                           /* L_4e22 */
        X = bus_read(0xD20A);                      /* 4e22 LDX $D20A */
        mem[0x288B] = X;                           /* 4e25 */
        for (;;) {                                 /* L_4e28 scan with stride $43 */
            if (mem[0x0900 + X] < 0x30 && mem[0x0A00 + X] == 0) {  /* 4e28-4e32 */
                uint8_t inTable = 0;
                for (int y = 0x2C; y >= 0; y--)    /* L_4e37: X already in $2276[]? */
                    if (X == mem[0x2276 + y]) { inTable = 1; break; }
                if (!inTable) { mem[0x0A00 + X] = mem[0x281D]; break; }  /* 4e3f-4e45 write + done */
            }
            X = (uint8_t)(X + 0x43);               /* 4e48 X += $43 */
            if (X == mem[0x288B]) break;           /* 4e4d-4e50 full cycle -> stop */
        }
        mem[0x288C] = (uint8_t)(mem[0x288C] - 1);  /* 4e52 DEC $288C */
    } while (mem[0x288C] != 0);                    /* 4e55 BNE L_4e22 */
}

/* obj_table_scan_a_c8 @ $4E1A — preset A=$C8, then place it via obj_table_scan_replace
 * (caller supplies Y = retry count). */
void obj_table_scan_a_c8(void) {
    cpu.A = 0xC8;                /* 4e1a */
    obj_table_scan_replace();    /* native; reads cpu.A/Y */
}

/* obj_table_scan_y1_c8 @ $4E18 — preset Y=1 (single pass), tail to obj_table_scan_a_c8. */
void obj_table_scan_y1_c8(void) {
    cpu.Y = 0x01;                /* 4e18 */
    obj_table_scan_a_c8();       /* native */
}

/* HUD display-field refresh chain ($4EA2-$4EB7).  Each link stores the entry value
 * and/or sets the field index Y, then tail-chains; game_sub_55FC (native) pushes the
 * indexed field to the display.  store_676_init/set_hud_fields read entry cpu.A. */
void refresh_hud_fields_0d_0e(void) {            /* $4EB2 (Y set by caller) */
    game_sub_55FC();                             /* 4eb2 */
    cpu.Y++;                                     /* 4eb5 INY */
    game_sub_55FC();                             /* 4eb6 */
}
void refresh_hud_field_0d_entry(void) {          /* $4EB0 */
    cpu.Y = 0x0D;                                /* 4eb0 */
    refresh_hud_fields_0d_0e();
}
void refresh_hud_field_0b(void) {                /* $4EAB */
    cpu.Y = 0x0B;                                /* 4eab */
    game_sub_55FC();                             /* 4ead */
    refresh_hud_field_0d_entry();
}
void set_hud_fields_678_679(void) {              /* $4EA5 */
    mem[0x0678] = cpu.A;                          /* 4ea5 */
    mem[0x0679] = cpu.A;                          /* 4ea8 */
    refresh_hud_field_0b();
}
void store_676_init(void) {                      /* $4EA2 */
    mem[0x0676] = cpu.A;                          /* 4ea2 */
    set_hud_fields_678_679();
}

/* step_object_along_axes @ $9473 — step the object's screen pos $0023/$0024 by ±$14
 * along the sign of $0024 (decrement clamps to 0), then either decrement depth
 * $0033/$0034 by $30 (when $0070 != 0) or, on first settle ($003D==0 && $0678==0),
 * bump $003D and (if $0063 >= 0) reset the gameplay flags.  mem-only contract. */
void step_object_along_axes(void) {
    uint8_t A, Y, c;
    #define ADC_(v) do { uint16_t _t=(uint16_t)A+(uint8_t)(v)+c; c=(uint8_t)(_t>>8); A=(uint8_t)_t; } while(0)
    #define SBC_(v) ADC_((uint8_t)~(uint8_t)(v))
    if (mem[0x0024] & 0x80) {                              /* 9473-9475 BPL: neg -> increment */
        c = 0; A = mem[0x0023]; ADC_(0x14); mem[0x0023] = A;  /* 9477-947c */
        if (c) mem[0x0024]++;                              /* 947e-9480 */
    } else {                                               /* L_9485 decrement, clamp 0 */
        c = 1; A = mem[0x0023]; SBC_(0x14); Y = A;         /* 9485-948a SEC;SBC;TAY */
        A = mem[0x0024]; SBC_(0x00);                       /* 948b-948d */
        if (!c) { A = 0x00; Y = 0x00; }                    /* 948f BCS; 9491-9493 */
        mem[0x0024] = A; mem[0x0023] = Y;                  /* 9494-9496 */
    }
    if (mem[0x0070] != 0) {                                /* 9498-949a BEQ L_94ac */
        c = 1; A = mem[0x0033]; SBC_(0x30); mem[0x0033] = A;  /* 949c-94a1 */
        A = mem[0x0034]; SBC_(0x00); mem[0x0034] = A;      /* 94a3-94a7 */
    } else if (mem[0x003D] == 0 && mem[0x0678] == 0) {     /* 94ac-94b3 */
        mem[0x003D]++;                                     /* 94b5 INC $003D */
        if (!(mem[0x0063] & 0x80)) reset_flags_ff();       /* 94b7-94bb BMI; tail (native) */
    }
    #undef ADC_
    #undef SBC_
}

/* draw_object_column @ $43E8 — draw a vertical PMG dial-bar column.  Loops the counter
 * $00BD down to $00BE; for each value X it loads a column pointer from $4581[2*X] into
 * $BB/$BC, writes a body byte ($B4/$B7 with bit7 set when X<$00BF, the lit threshold)
 * via ($BB),Y=0, and when $00C0!=0 a second byte ($B8/$38) via ($BB),Y=1.  Entry A is
 * the initial column index.  Tail draw_bar_loop_end ($442D) is a bare RTS (absorbed). */
void draw_object_column(void) {
    uint8_t A = cpu.A, X, Y;
    for (;;) {
        X = A;                                   /* 43e8 TAX */
        Y = (uint8_t)(A << 1);                   /* 43e9 ASL A; 43ea TAY */
        mem[0x00BB] = mem[0x4581 + Y];           /* 43eb-43ee */
        mem[0x00BC] = mem[0x4582 + Y];           /* 43f0-43f3 */
        Y = 0x00;                                /* 43f5 */
        if (X & 1) { A = 0xB7; mem[0x00C0] = A; }        /* 43f9 BCS L_4402: odd column */
        else       { mem[0x00C0] = 0x00; A = 0xB4; }     /* 43fb even column ($00C0=0) */
        if (X >= mem[0x00BF]) A &= 0x7F;          /* 4406-440c X>=thresh -> clear bit7 */
        else                  A |= 0x80;          /* 440f X<thresh -> lit (bit7) */
        bus_write((uint16_t)(mem[0x00BB] | (mem[0x00BC] << 8)) + Y, A);   /* 4411 ($BB),Y=0 */
        if (mem[0x00C0] != 0) {                   /* 4413-4415 */
            Y = 0x01;                             /* 4417 INY */
            A = (X >= mem[0x00BF]) ? 0x38 : 0xB8; /* 4418-4421 */
            bus_write((uint16_t)(mem[0x00BB] | (mem[0x00BC] << 8)) + Y, A);  /* 4423 ($BB),Y=1 */
        }
        mem[0x00BD] = (uint8_t)(mem[0x00BD] - 1); /* 4425 DEC $00BD */
        A = mem[0x00BD];                          /* 4427 */
        if (A == mem[0x00BE]) break;              /* 4429-442b loop while != $00BE */
    }
}

/* setup_dial_bar_draw @ $444A — set the dial-bar params ($BF=limit from A, $BE=7, $BD=$0F)
 * then draw the column from index $0F. */
void setup_dial_bar_draw(void) {
    mem[0x00BF] = cpu.A;          /* 444a (limit) */
    mem[0x00BE] = 0x07;           /* 444c-444e */
    mem[0x00BD] = 0x0F;           /* 4450-4452 */
    cpu.A = 0x0F;                 /* draw_object_column's entry index */
    draw_object_column();         /* 4454 (native) */
}

/* game_sub_4447 @ $4447 — A += 8 (the lit threshold), then draw the dial bar. */
void game_sub_4447(void) {
    cpu.A = (uint8_t)(cpu.A + 0x08);   /* 4447 CLC; 4448 ADC #$08 */
    setup_dial_bar_draw();             /* 444a (native) */
}

/* reset_indicator_event @ $B786 — clear $0035, then enqueue the indicator event. */
void reset_indicator_event(void) {
    mem[0x0035] = 0x00;            /* b786-b788 */
    enqueue_indicator_event();     /* b78a (native) */
}

/* compute_obj_rel_angle_scale @ $97A0 — build a 10-bit relative angle from $2885/$2886,
 * derive sign/quadrant ($2882, $28D7/$28D8 indices), look up scale factors in $4EB9, and
 * multiply by $002E (two mul_u8 passes) to produce the scaled coords $002B and $2881.
 * NOTE: the first ROL A rotates in the ENTRY CARRY -> contract includes entry C. */
void compute_obj_rel_angle_scale(void) {
    uint8_t A, X, c, n, v;
    #define ASLA_()  do { c=(uint8_t)(A>>7); A=(uint8_t)(A<<1); } while(0)
    #define ROLA_()  do { n=(uint8_t)(A>>7); A=(uint8_t)((A<<1)|c); c=n; } while(0)
    #define ROLM_(a) do { v=mem[a]; n=(uint8_t)(v>>7); mem[a]=(uint8_t)((v<<1)|c); c=n; } while(0)
    #define SBC_(x)  do { uint16_t _t=(uint16_t)A+(uint8_t)~(uint8_t)(x)+c; c=(uint8_t)(_t>>8); A=(uint8_t)_t; } while(0)

    c = cpu.C;                              /* ENTRY CARRY -> first ROL A */
    mem[0x006B] = mem[0x2886];              /* 97a0-97a3 */
    A = mem[0x2885];                        /* 97a5 */
    ROLA_(); ROLM_(0x006B); ROLA_(); ROLM_(0x006B);   /* 97a8-97ad build 10-bit */
    X = 0x00; mem[0x002C] = 0x00;           /* 97ae-97b0 */
    A = mem[0x006B];                        /* 97b2 */
    ASLA_(); if (c) mem[0x002C] = (uint8_t)(mem[0x002C] - 1);   /* 97b4-97b7 */
    ASLA_(); if (c) X--;                    /* 97b9-97bc */
    A = (uint8_t)(X ^ mem[0x002C]); mem[0x2882] = A;           /* 97bd-97c0 TXA;EOR;STA */
    A = 0x00; ROLA_(); X = A;               /* 97c3-97c6 LDA#0;ROL A;TAX (X = last carry) */
    A = (uint8_t)(mem[0x006B] & 0x3F);      /* 97c7-97c9 */
    mem[0x28D7] = A; mem[0x28D8] = A;       /* 97cb-97ce */
    A ^= 0x3F; mem[0x28D7 + X] = A;         /* 97d1-97d3 EOR #$3F; STA $28D7,X */
    mem[0x28D7 + X] = (uint8_t)(mem[0x28D7 + X] + 1);          /* 97d6 INC $28D7,X */
    X = mem[0x28D8];                        /* 97d9 */
    mem[0x006B] = mem[0x4EB9 + X];          /* 97dc-97df scale factor */
    mem[0x28D6] = mem[0x002E];              /* 97e1-97e3 multiplier */
    mul_u8(); A = cpu.A;                    /* 97e6 (native; product in cpu.A) */
    mem[0x002B] = A;                        /* 97e9 */
    if (A == 0) mem[0x002C] = A;            /* 97eb-97ef */
    A = mem[0x002C];                        /* 97f1 */
    if (A != 0) { c = 0; SBC_(mem[0x002B]); mem[0x002B] = A; }  /* 97f3 BEQ; 97f5 CLC;SBC;STA */
    X = mem[0x28D7];                        /* 97fa */
    mem[0x006B] = mem[0x4EB9 + X];          /* 97fd-9800 */
    mem[0x28D6] = mem[0x002E];              /* 9802-9804 */
    mul_u8(); A = cpu.A;                    /* 9807 */
    mem[0x2881] = A;                        /* 980a */
    if (A == 0) mem[0x2882] = A;            /* 980d-9811 */
    A = mem[0x2882];                        /* 9814 */
    if (A != 0) { c = 0; SBC_(mem[0x2881]); mem[0x2881] = A; }  /* 9817-981d */
    #undef ASLA_
    #undef ROLA_
    #undef ROLM_
    #undef SBC_
}
