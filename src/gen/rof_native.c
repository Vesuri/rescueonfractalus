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
#include "../platform/platform_c.h" /* platform_tick_vbi/render_frame/poll_events for the apex spin-waits */

/* Amiga black-until-ready reveal gate (read by animatePalette in RescueOnFractalus.cpp).
 * Set at display_setup entry — by then game_main_loop has drawn the cockpit + top bar and
 * scene.initialize has set up the sprites, so the window build is about to begin: the point
 * the user wants the screen to appear (cockpit pops in, then the window builds visibly).
 * Harmless on SDL (nothing reads it there). */
volatile unsigned char g_standbyRevealReady = 0;

/* Amiga door-field-ready gate (read by render() in RescueOnFractalus.cpp).  Latched on in
 * display_setup once the doors/dots/LEVEL field has been drawn into $2000 (after
 * blit_message_block/blit_numeric_readout) but BEFORE delay_loop_c2_to_c9 ramps the green
 * background colour $0071.  The Amiga decodes $2000 -> terrainBitmap once when this rises, so
 * the door shapes exist before the fade and the live color03 (= atariToOCS(mem[$0071])) ramp
 * shows the dark->bright green build on the real door pixels (was gated on $00E7 = build end,
 * AFTER the fade -> doors popped in already-green).  Latches like g_standbyRevealReady so the
 * launch re-run of display_setup doesn't transiently re-arm it.  Harmless on SDL. */
volatile unsigned char g_doorFieldReady = 0;

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

/* bin_to_bcd @ $4E84 — convert binary A (0-99) to packed BCD.
 *
 * The 6502 does this by repeated subtraction: Y counts how many times 10 fits
 * (= the tens digit), the leftover after one over-subtract is the ones digit
 * (stored to $00C1), and the result A = (tens << 4) | ones.  For A >= 100 the
 * tens digit exceeds 9 and the ASL x4 truncates to 8 bits — faithfully replicated
 * by the (uint8_t) cast on (tens << 4).  Callers consume the BCD byte in cpu.A
 * and the tens digit in cpu.Y; $00C1 holds the ones digit. */
uint8_t bin_to_bcd_core(uint8_t a, uint8_t *units, uint8_t *tens) {
    uint8_t t = (uint8_t)(a / 10);
    uint8_t u = (uint8_t)(a % 10);
    *units = u;
    *tens  = t;
    return (uint8_t)((uint8_t)(t << 4) | u);
}

/* 6502-ABI shim: entry A = value.  Exit: A = packed BCD, Y = tens, $00C1 = ones.
 * N/Z reflect the final A (the 6502's terminal ORA sets them from the result). */
void bin_to_bcd(void) {
    uint8_t units, tens;
    uint8_t bcd = bin_to_bcd_core(cpu.A, &units, &tens);
    mem[0x00C1] = units;
    cpu.Y = tens;
    LDA(bcd);
}

/* copy_altitude_graphic_to_screen @ $782A — Standby per-frame altitude banner.
 *
 * 6502: A=$0091; if A<$C0 return; if A==$C0 and $00E2 has bit7 set return;
 * $0091=Y(entry); X=($A>=$E0 ? $27 [also $00D8=$44] : $13); then copy 20 bytes
 * $5A9F+X -> $32B6+Y for Y=$14..$01 (X and Y decrementing together).  Exit
 * A/X/Y are dead at the Standby-loop call site (incidental in validation). */
void copy_altitude_graphic_to_screen_core(uint8_t entryY) {
    uint8_t a = mem[0x0091];
    if (a < 0xC0) return;
    if (a == 0xC0 && (mem[0x00E2] & 0x80)) return;
    mem[0x0091] = entryY;
    uint8_t x = (a >= 0xE0) ? 0x27 : 0x13;
    if (a >= 0xE0) mem[0x00D8] = 0x44;
    for (uint8_t y = 0x14; y != 0; y--, x--)
        mem[0x32B6 + y] = mem[0x5A9F + x];
    platform_title_changed();   /* hook: $32B7 banner rewritten -> flag the title region dirty */
}

/* 6502-ABI shim: entry cpu.Y is the new $0091 gate value. */
void copy_altitude_graphic_to_screen(void) {
    copy_altitude_graphic_to_screen_core(cpu.Y);
}

/* init_row_coords_9c @ $6DDF — load 5 row/coordinate constants into $009C-$00A0.
 * Pure leaf; exit regs (A=$13) dead at call sites (incidental in validation). */
void init_row_coords_9c(void) {
    mem[0x009C] = 0x2E;
    mem[0x009D] = 0x30;
    mem[0x009E] = 0x2B;
    mem[0x009F] = 0x2A;
    mem[0x00A0] = 0x13;
}

/* clear_scroll_accum @ $6B71 — zero the PCOLR0-3 shadow $02C0-$02C3 and the
 * 24-bit scroll accumulator $00A1-$00A4 (plus $00A5).  Pure leaf. */
void clear_scroll_accum(void) {
    for (int i = 0; i < 4; i++) {       /* 6502: X = 3..0 */
        mem[0x02C0 + i] = 0x00;
        mem[0x00A1 + i] = 0x00;
    }
    mem[0x00A5] = 0x00;
}

/* copy_192_to_1800 @ $75A5 — set the $00BB/$00BC dest pointer to $180F and copy
 * 192 bytes $350C+Y -> $180F+Y for Y=$C0..$01 (i.e. $1810..$18CF).  The 6502
 * writes via bus_write(ZP_IND_Y($BB)); the dest is RAM, so this is a plain mem[]
 * copy.  Pure leaf. */
void copy_192_to_1800(void) {
    mem[0x00BB] = 0x0F;
    mem[0x00BC] = 0x18;
    for (uint8_t y = 0xC0; y != 0x00; y--)
        mem[0x180F + y] = mem[0x350C + y];
}

/* build_row_addr_table @ $7460 — build the 85-entry ($55) per-scanline base-
 * address table at $073D (lo) / $0793 (hi) from base $00C4:$00C3 and 8-bit stride
 * $00C1: entry[0] = base, entry[i+1] = entry[i] + stride (16-bit accumulate).
 * Pure leaf (mem-only; exit regs dead). */
void build_row_addr_table(void) {
    mem[0x073D] = mem[0x00C3];
    mem[0x0793] = mem[0x00C4];
    uint8_t stride = mem[0x00C1];
    for (uint8_t y = 0x00; y != 0x55; y++) {
        uint16_t lo = (uint16_t)mem[0x073D + y] + stride;   /* CLC; ADC stride */
        mem[0x073E + y] = (uint8_t)lo;
        mem[0x0794 + y] = (uint8_t)(mem[0x0793 + y] + (lo >> 8));  /* ADC #0 + carry */
    }
}

/* build_line_addr_table_2000 @ $65DF — base $2000, stride $2E; tail build_row_addr_table. */
void build_line_addr_table_2000(void) {
    mem[0x00C1] = 0x2E;
    mem[0x00C3] = 0x00;
    mem[0x00C4] = 0x20;
    build_row_addr_table();
}

/* build_line_addr_table_1000_stride @ $65D2 — base $1000, stride = entry cpu.A;
 * tail build_row_addr_table. */
void build_line_addr_table_1000_stride(void) {
    mem[0x00C1] = cpu.A;
    mem[0x00C3] = 0x00;
    mem[0x00C4] = 0x10;
    build_row_addr_table();
}

/* build_line_addr_table_1000 @ $65D0 — A=$2E, tail build_line_addr_table_1000_stride. */
void build_line_addr_table_1000(void) {
    cpu.A = 0x2E;
    build_line_addr_table_1000_stride();
}

/* init_object_positions @ $6B85 — zero the scroll counters $08D1/$08D2/$08D3,
 * then build the 22-entry world-position word array $08A4(lo)/$08A5(hi) by adding
 * the 16-bit base $2EE0 to each word of the $6E2D source table (Y=$2A..$00 step 2).
 * Pure leaf. */
void init_object_positions(void) {
    mem[0x08D2] = 0x00;
    mem[0x08D3] = 0x00;
    mem[0x08D1] = 0x00;
    for (int y = 0x2A; y >= 0; y -= 2) {
        uint16_t lo = (uint16_t)mem[0x6E2D + y] + 0xE0;        /* CLC; ADC #$E0 */
        mem[0x08A4 + y] = (uint8_t)lo;
        mem[0x08A5 + y] = (uint8_t)(mem[0x6E2E + y] + 0x2E + (lo >> 8));  /* ADC #$2E + carry */
    }
}

/* audio_timer_setup @ $712D — silence the music gate ($00E7), $0655 and $00E5,
 * clear the POKEY audio timers ($D201/$D203/$D205/$D207) and set AUDCTL=$60.
 * The POKEY writes go through bus_write (Paula/ignored on Amiga; not in mem[], so
 * identical side effect in both runs).  Leaf. */
void audio_timer_setup(void) {
    mem[0x00E7] = 0x00;
    mem[0x0655] = 0x00;
    mem[0x00E5] = 0x00;
    bus_write(0xD201, 0x00);
    bus_write(0xD203, 0x00);
    bus_write(0xD205, 0x00);
    bus_write(0xD207, 0x00);
    bus_write(0xD208, 0x60);
}

/* random_terrain_height @ $6B47 — produce one sparse terrain-height value.
 * Reads POKEY RANDOM ($D20A): if (r & $1F) != 0 the height is 0; only when the
 * low 5 bits are all zero (1/32) does it take a second RANDOM read and index the
 * 4-entry table $6B5F[r2 & 3].  Result is returned in cpu.A (no mem writes); both
 * branches advance the RANDOM LFSR by exactly the reads the 6502 made. */
void random_terrain_height(void) {
    uint8_t r = (uint8_t)(bus_read(0xD20A) & 0x1F);
    if (r != 0) {                       /* CMP #1 -> BPL taken (r >= 1): height 0 */
        LDA(0x00);
        return;
    }
    uint8_t x = (uint8_t)(bus_read(0xD20A) & 0x03);
    LDA(mem[0x6B5F + x]);
}

/* fill_horizontal_span @ $665D — fill pattern $00B9 across a horizontal run on
 * two scanlines.  Row pointers $80/$81 = addr-table[$009E], $B7/$B8 = table[$009F].
 * The column endpoints come from $009C>>1 and $009D>>1 (one adjusted by +/-1 per
 * the shifted-out bit of $009D), giving the inclusive count $00DF = hi - lo and the
 * start index Y = hi.  Writes the pattern to ($80)+Y and ($B7)+Y down to lo.  Leaf. */
void fill_horizontal_span(void) {
    uint8_t y1 = mem[0x009E];
    mem[0x0080] = mem[0x073D + y1];
    mem[0x0081] = mem[0x0793 + y1];
    uint8_t y2 = mem[0x009F];
    mem[0x00B7] = mem[0x073D + y2];
    mem[0x00B8] = mem[0x0793 + y2];

    mem[0x0082] = (uint8_t)(mem[0x009C] >> 1);            /* lo  = $9C>>1 */
    uint8_t c2  = (uint8_t)(mem[0x009D] & 1);             /* shifted-out bit of $9D */
    mem[0x0083] = (uint8_t)(mem[0x009D] >> 1);            /* hi  = $9D>>1 */
    if (!c2) mem[0x0083] = (uint8_t)(mem[0x0083] - 1);    /* BCC: DEC hi */
    else     mem[0x0082] = (uint8_t)(mem[0x0082] + 1);    /*      INC lo */

    uint8_t y  = mem[0x0083];                             /* Y = hi (before SBC) */
    uint8_t lo = mem[0x0082];
    uint8_t cnt = (uint8_t)(mem[0x0083] - mem[0x0082]);   /* SEC SBC: count = hi - lo */
    mem[0x00DF] = cnt;
    uint8_t pat = mem[0x00B9];
    /* Both row bases are loop-invariant (set above from the addr table) and the screen
     * field is RAM, so hoist them and write mem[] directly — no per-byte bus dispatch /
     * ZP_IND_Y volatile re-read. */
    uint16_t base1 = (uint16_t)(mem[0x0080] | (mem[0x0081] << 8));
    uint16_t base2 = (uint16_t)(mem[0x00B7] | (mem[0x00B8] << 8));
    for (;;) {
        mem[(uint16_t)(base1 + y)] = pat;
        mem[(uint16_t)(base2 + y)] = pat;
        y = (uint8_t)(y - 1);
        if (((uint8_t)(cnt - 1)) & 0x80) break;           /* BPL: loop while N clear */
        cnt = (uint8_t)(cnt - 1);
    }
    mem[0x00DF] = 0xFF;                                    /* faithful exit: count ran to -1 */
    cpu.Y = lo;                                           /* last Y set at loop top (incidental) */
}

/* plot_glyph_pixel_masked @ $66DE — OR/AND a 2-bit pixel into the screen byte at
 * ($80)+Y using the OR mask $66E9[X] and AND mask $66FB[X].  Leaf (entry X/Y). */
void plot_glyph_pixel_masked(void) {
    uint16_t a = ZP_IND_Y(0x80);          /* screen field ($1000/$2000) is RAM: direct mem[] */
    uint8_t v = mem[a];                   /* (bus_read/write would HW-range-check every pixel) */
    v |= mem[0x66E9 + cpu.X];
    v &= mem[0x66FB + cpu.X];
    mem[a] = v;
}

/* plot_pixel_masked @ $66D5 — entry A = column.  Y = A>>1 (byte index); the mask
 * index X = $0094, plus 9 (ADC #$08 with the LSR's shifted-out carry) for odd
 * columns; tail-calls plot_glyph_pixel_masked. */
void plot_pixel_masked(void) {
    uint8_t col = cpu.A;
    cpu.Y = (uint8_t)(col >> 1);
    uint8_t x = mem[0x0094];
    if (col & 1) x = (uint8_t)(x + 0x09);   /* ADC #$08 with carry=1 from LSR */
    cpu.X = x;
    plot_glyph_pixel_masked();
}

/* set_row_ptr @ $66C8 — load the bitmap row pointer $0080/$0081 from the per-
 * scanline base-address table $073D(lo)/$0793(hi) indexed by entry cpu.Y. */
void set_row_ptr(void) {
    mem[0x0080] = mem[0x073D + cpu.Y];
    mem[0x0081] = mem[0x0793 + cpu.Y];
}

/* set_row_ptr_from_count @ $66C6 — Y = row counter $0092, tail set_row_ptr. */
void set_row_ptr_from_count(void) {
    cpu.Y = mem[0x0092];
    set_row_ptr();
}

/* fill_vertical_span @ $669C — plot a vertical run of pixels down rows $009F..$009E.
 * Per row: set the row pointer $80/$81 from the addr table, then masked-plot column
 * $009C (plot_pixel_masked, which also leaves the mask index in cpu.X) and the glyph
 * column $009D>>1 reusing that mask (plot_glyph_pixel_masked).  $00DF = $009E-$009F
 * is the inclusive row count; $0084 walks the row index.
 *
 * Typed core: within one call the two columns, the (shared) mask index and its OR/AND
 * mask bytes, and the byte offsets within a row are all loop-invariant — only the row
 * base changes.  Hoist them into locals so the per-row body is two addr-table reads +
 * two direct RAM read-modify-writes, instead of re-reading volatile zero page + the
 * mask tables on every pixel via plot_pixel_masked/plot_glyph_pixel_masked.  The screen
 * field ($1000/$2000) is RAM, so the plots go straight to mem[].  Domain: rows r0<=r1
 * indexing the $073D/$0793 table into bitmap RAM (the real caller's contract — see the
 * fixture in tools/validate_native.c).  This was the bulk of the pre-door ring-draw
 * freeze on the 68000. */
static void fill_vertical_span_core(uint8_t r0, uint8_t r1, uint8_t colL, uint8_t colR, uint8_t maskSel) {
    uint8_t x    = (colL & 1u) ? (uint8_t)(maskSel + 9u) : maskSel;  /* mask index (both edges) */
    uint8_t orm  = mem[0x66E9 + x];
    uint8_t am   = mem[0x66FB + x];
    uint8_t offL = (uint8_t)(colL >> 1);
    uint8_t offR = (uint8_t)(colR >> 1);
    uint8_t cnt  = (uint8_t)(r1 - r0);               /* $00DF; loop runs cnt+1 rows */
    uint8_t row  = r0;
    for (;;) {
        uint16_t base = (uint16_t)(mem[0x073D + row] | (mem[0x0793 + row] << 8));
        uint16_t aL = (uint16_t)(base + offL);
        uint16_t aR = (uint16_t)(base + offR);
        mem[aL] = (uint8_t)((mem[aL] | orm) & am);
        mem[aR] = (uint8_t)((mem[aR] | orm) & am);
        row++;
        if (((uint8_t)(cnt - 1)) & 0x80) break;      /* BPL: stop after cnt+1 rows */
        cnt = (uint8_t)(cnt - 1);
    }
}
void fill_vertical_span(void) {
    uint8_t r0 = mem[0x009F], r1 = mem[0x009E];
    uint8_t colL = mem[0x009C], colR = mem[0x009D], maskSel = mem[0x0094];
    fill_vertical_span_core(r0, r1, colL, colR, maskSel);
    /* Faithful exit state: $0084 = last row + 1; $80/$81 = addr table[last row];
     * $00DF = $FF; cpu.X = mask index, cpu.Y = colR>>1 (cpu state is incidental). */
    mem[0x0084] = (uint8_t)(r1 + 1);
    mem[0x0080] = mem[0x073D + r1];
    mem[0x0081] = mem[0x0793 + r1];
    mem[0x00DF] = 0xFF;
    cpu.X = (colL & 1u) ? (uint8_t)(maskSel + 9u) : maskSel;
    cpu.Y = (uint8_t)(colR >> 1);
}

/* plot_pixel_2bpp @ $6C92 — pack the screen byte at ($80)+Y into a 2-bits-per-pixel
 * cell: read it, then 4 rounds of {force both top bits if either is set; ROL twice}
 * through the carry chain (seeded by the entry carry), a final ROL, and write back.
 * Preserves cpu.X (the 6502 saves/restores it on the stack). */
void plot_pixel_2bpp(void) {
    uint8_t savedX = cpu.X;
    uint8_t c = (uint8_t)(cpu.C & 1);                     /* carry into the first ROL = entry C */
    uint16_t a = ZP_IND_Y(0x80);                          /* screen field is RAM: direct mem[] */
    uint8_t acc = mem[a];
    mem[0x0082] = 0xC0;                                   /* BIT mask, set once */
    for (int i = 0; i < 4; i++) {
        if ((acc & 0xC0) == 0) acc |= 0xC0;               /* BIT $0082; BNE skips -> ORA #$C0 only when top bits clear */
        uint8_t nc = (uint8_t)((acc >> 7) & 1); acc = (uint8_t)((acc << 1) | c); c = nc;  /* ROL */
        nc = (uint8_t)((acc >> 7) & 1);          acc = (uint8_t)((acc << 1) | c); c = nc;  /* ROL */
    }
    acc = (uint8_t)((acc << 1) | c);                      /* final ROL */
    mem[a] = acc;
    cpu.X = savedX;
}

/* draw_symmetric_span_loop @ $6642 — draw $0096 nested span pairs.  The fill
 * pattern $00B9 = $0094 | maskTbl[$0094].  Each iteration draws one horizontal and
 * one vertical span, then steps the four edge coordinates inward ($9C--, $9D++,
 * $9E++, $9F--).  Loops $0096 times (DEC; BNE). */
void draw_symmetric_span_loop(void) {
    uint8_t v94 = mem[0x0094];
    mem[0x00B9] = (uint8_t)(v94 | mem[0x66E9 + v94]);
    for (;;) {
        fill_horizontal_span();
        fill_vertical_span();
        mem[0x009C] = (uint8_t)(mem[0x009C] - 1);
        mem[0x009D] = (uint8_t)(mem[0x009D] + 1);
        mem[0x009E] = (uint8_t)(mem[0x009E] + 1);
        mem[0x009F] = (uint8_t)(mem[0x009F] - 1);
        uint8_t n = (uint8_t)(mem[0x0096] - 1);
        mem[0x0096] = n;
        if (n == 0) break;                                /* DEC $0096; BNE */
    }
}

/* gen_terrain_column @ $6B2E — fill column (entry cpu.Y) of all four parallel
 * terrain buffers $0C32/$0D32/$0E32/$0F32 with sparse random heights.  Each of the
 * four random_terrain_height calls advances the POKEY RANDOM LFSR; cpu.Y is
 * preserved across them (neither this routine nor random_terrain_height touch Y). */
void gen_terrain_column(void) {
    uint8_t y = cpu.Y;
    random_terrain_height(); mem[0x0C32 + y] = cpu.A;
    random_terrain_height(); mem[0x0D32 + y] = cpu.A;
    random_terrain_height(); mem[0x0E32 + y] = cpu.A;
    random_terrain_height(); mem[0x0F32 + y] = cpu.A;
}

/* fill_terrain_columns @ $6AE5 — fill all 89 columns (Y=$59..$01) of the four
 * parallel terrain buffers by calling gen_terrain_column per column. */
void fill_terrain_columns(void) {
    for (uint8_t y = 0x59; y != 0x00; y--) {
        cpu.Y = y;
        gen_terrain_column();
    }
}

/* draw_shape_rows_loop @ $6620 — for 86 rows ($0092=$55..$00) set the row pointer
 * from the row counter, then masked-plot three columns ($009C, $009D, and
 * $00A0=$009D+1) into that row.  Tail of draw_frame_pattern_seq. */
void draw_shape_rows_loop(void) {
    mem[0x0092] = 0x55;
    mem[0x00A0] = (uint8_t)(mem[0x009D] + 1);             /* CLC; ADC #1 */
    for (;;) {
        set_row_ptr_from_count();                         /* $80/$81 = table[$0092] */
        cpu.A = mem[0x009C]; plot_pixel_masked();
        cpu.A = mem[0x009D]; plot_pixel_masked();
        cpu.A = mem[0x00A0]; plot_pixel_masked();
        uint8_t n = (uint8_t)(mem[0x0092] - 1);
        mem[0x0092] = n;
        if (n & 0x80) break;                              /* DEC $0092; BPL */
    }
}

/* draw_frame_pattern_seq @ $65FB — the per-frame doors/tunnel frame drawer.  After
 * init_row_coords_9c seeds the edge coords, loop $00A0+1 (=20) times: read the next
 * span count from the $6E0F pattern table into $0096, draw one symmetric span pair,
 * and cycle the pattern selector $0094 through 1..6.  Finally DEC $0094 and tail
 * draw_shape_rows_loop.  (Entry A=$01 is positive so the $6602 BMI never fires.) */
void draw_frame_pattern_seq(void) {
    init_row_coords_9c();
    mem[0x0094] = 0x01;
    for (;;) {
        cpu.Y = mem[0x00A0];
        mem[0x0096] = mem[0x6E0F + cpu.Y];
        draw_symmetric_span_loop();
        uint8_t v = (uint8_t)(mem[0x0094] + 1);           /* INC $0094 */
        if (v == 0x07) v = 0x01;                          /* CMP #7; BEQ -> wrap to 1 */
        mem[0x0094] = v;
        uint8_t a0 = (uint8_t)(mem[0x00A0] - 1);          /* DEC $00A0 */
        mem[0x00A0] = a0;
        if (a0 & 0x80) break;                             /* BPL: loop while N clear */
    }
    mem[0x0094] = (uint8_t)(mem[0x0094] - 1);             /* DEC $0094 */
    draw_shape_rows_loop();                                /* tail */
}

/* draw_vline_pair @ $6C4D — plot a symmetric pair of vertical lines.  Entry A is the
 * start row, $00B8 the end row; the loop walks A down to $00B8 ($0092 = row counter).
 * Per row: set the row pointer; the plot column is col = (entryX>>1)+2 and its mirror
 * $2F-col.  For rows >= $2B the two columns are written via plot_pixel_2bpp (2bpp pack,
 * carry-sensitive — the entry carry is whatever CMP/SBC last set), otherwise the fill
 * byte $0084 is stored directly.  cpu.X (entryX) is preserved across the loop. */
void draw_vline_pair(void) {
    mem[0x0092] = cpu.A;
    if ((uint8_t)(cpu.A - mem[0x00B8]) & 0x80) return;     /* CMP $00B8; BMI -> return */
    uint8_t entryX = cpu.X;
    for (;;) {
        uint8_t a = mem[0x0092];
        if (a & 0x80) { a = 0x00; mem[0x0092] = 0x00; }    /* CMP #0; BPL skips; clamp negative to 0 */
        set_row_ptr_from_count();                          /* Y=$0092 -> $80/$81 (preserves X) */
        uint8_t col = (uint8_t)((entryX >> 1) + 2);        /* TXA; LSR; CLC; ADC #$02 */
        mem[0x0085] = col;
        uint8_t y2 = (uint8_t)(0x2F - col);                /* mirror column */
        if (a >= 0x2B) {                                   /* CMP #$2B; BCC -> else (bus_write) path */
            cpu.Y = col; cpu.C = 1;                        /* C set by the CMP (a >= $2B) */
            plot_pixel_2bpp();
            cpu.Y = y2;  cpu.C = (uint8_t)(0x2F >= col);   /* C from SEC; SBC $0085 */
            plot_pixel_2bpp();
        } else {
            cpu.Y = col; mem[ZP_IND_Y(0x80)] = mem[0x0084];   /* screen field is RAM: direct mem[] */
            cpu.Y = y2;  mem[ZP_IND_Y(0x80)] = mem[0x0084];
        }
        uint8_t n = (uint8_t)(mem[0x0092] - 1);            /* DEC $0092 */
        mem[0x0092] = n;
        if ((uint8_t)(n - mem[0x00B8]) & 0x80) break;      /* CMP $00B8; BPL loops; break when N set */
    }
}

/* update_object_distance @ $6BED — compute an object's clamped 16-bit screen distance
 * {$00B7:$00B8} = {$00B9:$00BA} - {$0084:$0085} (clamped to >= 0), store it to the
 * object position arrays $08A4/$08A5[X], then draw up to three vertical line pairs
 * (top edge at row <= $2E, then two more stepping the row/fill byte) via draw_vline_pair,
 * with early-outs when the row counter $00B7 goes negative.  Entry cpu.X selects the
 * object slot and is the column source for the draws (draw_vline_pair preserves it). */
void update_object_distance(void) {
    uint8_t x = cpu.X;

    uint16_t lo = (uint16_t)mem[0x00B9] - mem[0x0084];     /* SEC; SBC $0084 */
    mem[0x00B7] = (uint8_t)lo;
    uint8_t borrow = (lo & 0x100) ? 1 : 0;
    uint8_t hi = (uint8_t)(mem[0x00BA] - mem[0x0085] - borrow);  /* SBC $0085 */
    if (hi & 0x80) hi = 0x00;                              /* BPL skips; clamp negative to 0 */
    mem[0x00B8] = hi;

    mem[0x08A4 + x] = mem[0x00B7];
    mem[0x08A5 + x] = mem[0x00B8];
    mem[0x0084] = 0xFF;

    mem[0x00B7] = (mem[0x00BA] < 0x2F) ? mem[0x00BA] : 0x2E;  /* CMP #$2F; BCC skips; else $2E */
    cpu.A = mem[0x00B7]; cpu.X = x; draw_vline_pair();
    mem[0x0084] = 0xAA;

    {   /* DEC $00B7; BMI -> return */
        uint8_t d = (uint8_t)(mem[0x00B7] - 1);
        mem[0x00B7] = d;
        if (d & 0x80) return;
    }

    {   /* SEC; LDA $00B8; SBC $08D3; BPL skips; clamp negative */
        uint8_t s = (uint8_t)(mem[0x00B8] - mem[0x08D3]);
        if (s & 0x80) s = 0x00;
        mem[0x00B8] = s;
    }
    cpu.A = mem[0x00B7]; cpu.X = x; draw_vline_pair();
    mem[0x00B7] = mem[0x00B8];
    mem[0x0084] = 0x55;

    {   /* DEC $00B7; BMI -> return */
        uint8_t d = (uint8_t)(mem[0x00B7] - 1);
        mem[0x00B7] = d;
        if (d & 0x80) return;
    }

    {   /* DEC $00B8; BPL skips; clamp negative to 0 */
        uint8_t d = (uint8_t)(mem[0x00B8] - 1);
        mem[0x00B8] = (d & 0x80) ? 0x00 : d;
    }
    if ((uint8_t)(mem[0x00B7] - 0x2B) & 0x80) {            /* CMP #$2B; BMI -> draw + return */
        cpu.A = mem[0x00B7]; cpu.X = x; draw_vline_pair();
    }
}

/* advance_object_positions @ $6BA8 — advance the scroll counters ($08D1++, the
 * 16-bit $08D2/$08D3 += $18) then, for each of the 22 object slots (X=$2A..$00 step 2),
 * build {$0085:$0084} = ($08A5[X] << 2) + $08D3 (and $00B9/$00BA = the slot's raw
 * lo/hi), and call update_object_distance to recompute + redraw that object. */
void advance_object_positions(void) {
    mem[0x08D1] = (uint8_t)(mem[0x08D1] + 1);              /* INC $08D1 */
    uint16_t s = (uint16_t)mem[0x08D2] + 0x18;             /* CLC; ADC #$18 */
    mem[0x08D2] = (uint8_t)s;
    mem[0x08D3] = (uint8_t)(mem[0x08D3] + (s >> 8));       /* ADC #$00 + carry */

    for (int x = 0x2A; x >= 0; x -= 2) {
        uint8_t lo = mem[0x08A4 + x];
        mem[0x00B9] = lo;
        mem[0x0084] = lo;
        uint8_t hi = mem[0x08A5 + x];
        mem[0x00BA] = hi;
        mem[0x0085] = hi;
        /* {$85:$84} = hi << 2 (via ASL $84; ROL into $85, twice) */
        uint8_t c0 = (uint8_t)((hi >> 7) & 1);             /* ASL A (=hi); $84 = hi<<1 */
        mem[0x0084] = (uint8_t)(hi << 1);
        uint8_t a = c0;                                    /* LDA #0; ROL A -> c0 */
        uint8_t c1 = (uint8_t)((mem[0x0084] >> 7) & 1);    /* ASL $0084 */
        mem[0x0084] = (uint8_t)(mem[0x0084] << 1);
        mem[0x0085] = (uint8_t)((a << 1) | c1);            /* ROL A -> $85 */
        uint16_t t = (uint16_t)mem[0x0084] + mem[0x08D3];  /* CLC; ADC $08D3 */
        mem[0x0084] = (uint8_t)t;
        mem[0x0085] = (uint8_t)(mem[0x0085] + (t >> 8));   /* ADC #$00 + carry */
        cpu.X = (uint8_t)x;
        update_object_distance();
    }
}

/* clear_var_0632 @ $7F74 — $0632 = 0. */
void clear_var_0632(void) { mem[0x0632] = 0x00; }

/* clear_pm_state @ $3FBF — fill $00DA-$00DD, $02C0-$02C3 and $00D9 with entry cpu.A. */
void clear_pm_state(void) {
    uint8_t a = cpu.A;
    for (int y = 0; y < 4; y++) { mem[0x00DA + y] = a; mem[0x02C0 + y] = a; }
    mem[0x00D9] = a;
}

/* clear_terrain_lo_buffers @ $6B63 — zero the two low terrain buffers $0E32/$0F32 (96 bytes). */
void clear_terrain_lo_buffers(void) {
    for (int y = 0x5F; y >= 0; y--) { mem[0x0E32 + y] = 0x00; mem[0x0F32 + y] = 0x00; }
}

/* fill_four_bufs_ff @ $6899 — $FF into the four buffers $0C87/$0D87/$0E87/$0F87 at +8..+1. */
void fill_four_bufs_ff(void) {
    for (uint8_t y = 0x08; y != 0x00; y--) {
        mem[0x0C87 + y] = 0xFF; mem[0x0D87 + y] = 0xFF;
        mem[0x0E87 + y] = 0xFF; mem[0x0F87 + y] = 0xFF;
    }
}

/* fill_buf_08d4 @ $6890 — fill $08D4-$08D9 with entry cpu.A. */
void fill_buf_08d4(void) {
    uint8_t a = cpu.A;
    for (int y = 5; y >= 0; y--) mem[0x08D4 + y] = a;
}

/* copy_4byte_table_to_02c4 @ $5D3B — copy from $5D48+X downward into $02C4+Y.
 * The loop branch (BPL) tests the flag from DEX, so it is X-CONTROLLED, not a fixed
 * 4 iterations: it runs until X (decrementing from entry X) goes negative, with Y
 * decrementing and wrapping in parallel.  In-game entry X is 3 (the intended 4-byte
 * copy), but the faithful port reproduces the full X-bounded, Y-wrapping behaviour. */
void copy_4byte_table_to_02c4(void) {
    uint8_t x = cpu.X, y = 0x03;
    do {
        mem[0x02C4 + y] = mem[0x5D48 + x];
        y = (uint8_t)(y - 1);   /* DEY */
        x = (uint8_t)(x - 1);   /* DEX (sets the N tested by BPL) */
    } while (!(x & 0x80));      /* BPL: loop while X >= 0 */
}

/* reset_audctl_flags @ $70E7 — $00E7=1 (music gate on), AUDCTL($D208)=0, $073A=0,
 * $0090=0, $073C=$FF. */
void reset_audctl_flags(void) {
    mem[0x00E7] = 0x01;
    bus_write(0xD208, 0x00);
    mem[0x073A] = 0x00;
    mem[0x0090] = 0x00;
    mem[0x073C] = 0xFF;
}

/* game_init_first @ $5DDB — $0043 = 1. */
void game_init_first(void) { mem[0x0043] = 0x01; }

/* mark_grid_slot_active @ $7B74 — $0A00[$28E6] = 1. */
void mark_grid_slot_active(void) { mem[0x0A00 + mem[0x28E6]] = 0x01; }

/* push_grid_cell @ $70A9 — store $009C into $2500[$0098], then INC $0098. */
void push_grid_cell(void) {
    mem[0x2500 + mem[0x0098]] = mem[0x009C];
    mem[0x0098] = (uint8_t)(mem[0x0098] + 1);
}

/* vobj_pos_to_pmstrip_index @ $41DA — Y = ($DC - $062F) >> 2 (also left in A).
 * Maps a vertical object position to a PMG strip index; result consumed via cpu.Y. */
void vobj_pos_to_pmstrip_index(void) {
    uint8_t a = (uint8_t)(0xDC - mem[0x062F]);   /* SEC; SBC $062F */
    a = (uint8_t)(a >> 2);                       /* LSR; LSR */
    cpu.A = a;
    cpu.Y = a;                                   /* TAY */
}

/* copy_terrain_seed_rows @ $45EE — copy three 8-byte seed rows from $4DD2/$4DDA/$4DE2
 * into the terrain buffers $0C88/$0D88/$0B88. */
void copy_terrain_seed_rows(void) {
    for (int y = 7; y >= 0; y--) {
        mem[0x0C88 + y] = mem[0x4DD2 + y];
        mem[0x0D88 + y] = mem[0x4DDA + y];
        mem[0x0B88 + y] = mem[0x4DE2 + y];
    }
}

/* copy_row_addr_subset @ $7483 — copy 48 entries of the row-addr table $073D(lo)/$0793(hi)
 * (Y=$2F..$00) into $2932(lo)/$2962(hi) (X=$00..$2F, reversed). */
void copy_row_addr_subset(void) {
    uint8_t x = 0x00;
    for (int y = 0x2F; y >= 0; y--) {
        mem[0x2932 + x] = mem[0x073D + y];
        mem[0x2962 + x] = mem[0x0793 + y];
        x++;
    }
}

/* memset_or_copy @ $3C93 — fill the byte $00B7 across [dest .. dest+count], where dest
 * is the 16-bit pointer $00C1/$00C2 (post-incremented) and count is the 16-bit value
 * $00C4:$00C3 + 1 (each underflow of $00C3 borrows from $00C4; loop ends when both
 * reach $FF).  Writes through bus_write; $00B7 is re-read each pass (faithful). */
void memset_or_copy(void) {
    /* Fast path: the byte $00B7 is filled to [dest, dest+count) where dest=$C1/$C2 and
       count = ($C4:$C3)+1.  The per-byte bus_write below is pure overhead for plain RAM
       (its function call + two range checks + (zp),Y recompute per byte made loader_util's
       ~9.5 KB clear cost ~23 VBIs / ~0.46s of the boot path).  bus_write only differs from
       a direct mem[] store for the hardware range ($D000-$D7FF) and the page-2 OS shadows
       ($0200-$02FF, which notify the platform).  All real callers (loader_util,
       fill_region_2000, init_terrain_render_buffers, display_setup) target plain RAM, so
       fill mem[] directly there and fall back to the faithful loop only when the region
       overlaps those ranges (or wraps $FFFF).  Leaves $C1-$C4/Y exactly as the loop would. */
    uint16_t start = (uint16_t)(mem[0x00C1] | (mem[0x00C2] << 8));
    uint32_t count = (uint32_t)(((uint32_t)mem[0x00C4] << 8) | mem[0x00C3]) + 1u;
    uint32_t end   = (uint32_t)start + count;                 /* unwrapped, for overlap test */
    int hw     = (start < 0xD800u) && (end > 0xD000u);
    int shadow = (start < 0x0300u) && (end > 0x0200u);
    if (!hw && !shadow && end <= 0x10000u) {
        uint8_t  val = mem[0x00B7];
        uint16_t a   = start;
        for (uint32_t k = 0; k < count; k++) { mem[a] = val; a = (uint16_t)(a + 1); }
        mem[0x00C1] = (uint8_t)(end & 0xFF);
        mem[0x00C2] = (uint8_t)((end >> 8) & 0xFF);
        mem[0x00C3] = 0xFF;
        mem[0x00C4] = 0xFF;
        cpu.Y = 0x00;
        return;
    }
    for (;;) {
        cpu.Y = 0x00;
        bus_write(ZP_IND_Y(0x00C1), mem[0x00B7]);
        mem[0x00C1] = (uint8_t)(mem[0x00C1] + 1);             /* INC $C1 */
        if (mem[0x00C1] == 0x00) mem[0x00C2] = (uint8_t)(mem[0x00C2] + 1);  /* INC $C2 on carry */
        mem[0x00C3] = (uint8_t)(mem[0x00C3] - 1);             /* DEC $C3 */
        if (mem[0x00C3] != 0xFF) continue;                    /* loop while no underflow */
        mem[0x00C4] = (uint8_t)(mem[0x00C4] - 1);             /* DEC $C4 */
        if (mem[0x00C4] != 0xFF) continue;
        return;
    }
}

/* copy_bytes_to_dst @ $3C61 — write entry cpu.A to the dest pointer $00BD/$00BE
 * (post-incremented), X times (entry cpu.X, 0 => 256), at offset cpu.Y; then bump the
 * source pointer $00BB/$00BC once. */
void copy_bytes_to_dst(void) {
    uint8_t a = cpu.A;
    uint8_t x = cpu.X;
    do {
        bus_write(ZP_IND_Y(0x00BD), a);
        mem[0x00BD] = (uint8_t)(mem[0x00BD] + 1);             /* INC $BD */
        if (mem[0x00BD] == 0x00) mem[0x00BE] = (uint8_t)(mem[0x00BE] + 1);  /* INC $BE on carry */
        x = (uint8_t)(x - 1);                                 /* DEX */
    } while (x != 0);                                         /* BNE */
    mem[0x00BB] = (uint8_t)(mem[0x00BB] + 1);                 /* INC $BB */
    if (mem[0x00BB] == 0x00) mem[0x00BC] = (uint8_t)(mem[0x00BC] + 1);  /* INC $BC on carry */
}

/* --- display_setup-subtree leaves (batch 2026-06-15): pure mem-effect leaves. --- */

/* terrain_lookup @ $3FDE — copy 4 bytes from the table $4B0B (descending) into
 * $32E3[3..0].  The base index is ($281C + $3FF6[$2836]) & $FF; the 6502 $4B0B,Y
 * lookup uses an 8-bit Y, so the source addresses span $4B0B..$4C0A. */
void terrain_lookup(void) {
    uint8_t y = (uint8_t)(mem[0x281C] + mem[0x3FF6 + mem[0x2836]]);
    for (int x = 3; x >= 0; x--) {
        mem[0x32E3 + x] = mem[0x4B0B + y];
        y = (uint8_t)(y - 1);
    }
}

/* fill_buffer2_region_ff @ $45A1 — fill 8 runs of 32 bytes ($FF) starting at $2098
 * with stride $30 (so $2098.., $20C8.., ... covers the 256-byte field region). */
void fill_buffer2_region_ff(void) {
    mem[0x00C1] = 0x98; mem[0x00C2] = 0x20;
    for (int blk = 8; blk > 0; blk--) {
        for (int y = 0x1F; y >= 0; y--) {
            cpu.Y = (uint8_t)y;
            bus_write(ZP_IND_Y(0x00C1), 0xFF);
        }
        uint16_t p = (uint16_t)(mem[0x00C1] | (mem[0x00C2] << 8));
        p = (uint16_t)(p + 0x30);                     /* ADC #$30 / ADC #$00 (16-bit) */
        mem[0x00C1] = (uint8_t)p; mem[0x00C2] = (uint8_t)(p >> 8);
    }
}

/* game_sub_4606 @ $4606 — init the target-state cells: $32E3[0..3]=1, the trio
 * $3355/$3356/$3357 = $B4/$B5/$B6, $3388 = $B4, and $33DF/$33E0 = $1E/$1D. */
void game_sub_4606(void) {
    for (int y = 3; y >= 0; y--) mem[0x32E3 + y] = 0x01;
    mem[0x3388] = 0xB4;
    mem[0x3355] = 0xB4; mem[0x3356] = 0xB5; mem[0x3357] = 0xB6;
    mem[0x33DF] = 0x1E; mem[0x33E0] = 0x1D;
}

/* fill_message_buffer @ $480F — store entry cpu.A into the message buffer at
 * $32B6+X down to $32B6+1 (DEX;BNE — entry X=0 wraps to a full 256-byte fill). */
void fill_message_buffer(void) {
    uint8_t a = cpu.A, x = cpu.X;
    do {
        mem[0x32B6 + x] = a;
        x = (uint8_t)(x - 1);
    } while (x != 0);
    platform_title_changed();   /* hook: message buffer $32B7 cleared/filled -> flag title dirty */
}

/* intro_fill_display_params @ $4FE0 — build 8 display params $00CF..$00D6 from the
 * low nibble of $4DF1[Y] OR'd with the current colour phase $00C2; then poke the
 * last value to GTIA HITCLR $D019 (ignored on host/Amiga) and bump $00C2. */
void intro_fill_display_params(void) {
    uint8_t a = 0;
    for (int y = 7; y >= 0; y--) {
        a = (uint8_t)((mem[0x4DF1 + y] & 0x0F) | mem[0x00C2]);
        mem[0x00CF + y] = a;
    }
    bus_write(0xD019, a);
    mem[0x00C2] = (uint8_t)(mem[0x00C2] + 1);
}

/* match_code_sequence @ $5B45 — cheat/code matcher.  Entry cpu.A is the next typed
 * code; $063F is the match position.  On a match advance the position; when it
 * reaches 6 copy the 40-byte payload $5B17..$5B3E into $36AB..$36D2 and reset the
 * position.  Any mismatch resets the position.  Always sets $0049 = $FF. */
void match_code_sequence(void) {
    uint8_t x = mem[0x063F];
    if (cpu.A == mem[0x5B3F + x]) {
        x = (uint8_t)(x + 1);
        if (x == 6) {
            for (int y = 0x27; y >= 0; y--) mem[0x36AB + y] = mem[0x5B17 + y];
            x = 0;
        }
    } else {
        x = 0;
    }
    mem[0x063F] = x;
    mem[0x0049] = 0xFF;
}

/* init_terrain_dl @ $68AD — fill the terrain display-list region: $2F75..$2FA3 = $88
 * (47 bytes), then every 3rd entry of $300A.. / $308B.. (LMS address pairs) gets the
 * pointer $2F74 (lo=$74, hi=$2F) for Y = $7E,$7B,...,3,0. */
void init_terrain_dl(void) {
    for (int y = 0x2F; y >= 1; y--) mem[0x2F74 + y] = 0x88;
    for (int y = 0x7E; y >= 0; y -= 3) {
        mem[0x300A + y] = 0x74; mem[0x308B + y] = 0x74;
        mem[0x300B + y] = 0x2F; mem[0x308C + y] = 0x2F;
    }
}

/* music_init_state @ $7238 — copy 6 bytes $731E[Y..Y-5] (entry Y) into $0657[5..0],
 * clear $0651 + POKEY AUDCTL ($D208), and set $0653/$0655 = 1. */
void music_init_state(void) {
    uint8_t y = cpu.Y;
    for (int x = 5; x >= 0; x--) {
        mem[0x0657 + x] = mem[0x731E + y];
        y = (uint8_t)(y - 1);
    }
    mem[0x0651] = 0x00;
    bus_write(0xD208, 0x00);
    mem[0x0653] = 0x01;
    mem[0x0655] = 0x01;
}

/* count_up_to_level @ $75B8 — bump $0604 (and a parallel counter $00C3) until $0604
 * reaches the target $006D.  The 6502 wraps $C3 in SED/ADC but our ADC ignores the
 * decimal flag (matching the transliterated oracle), so $C3 counts in binary. */
void count_up_to_level(void) {
    mem[0x00C3] = 0x00;
    do {
        mem[0x0604] = (uint8_t)(mem[0x0604] + 1);
        mem[0x00C3] = (uint8_t)(mem[0x00C3] + 1);
    } while (mem[0x0604] != mem[0x006D]);
}

/* hud_fill_field1 @ $811F — HUD field-1 advance.  Cursor Y = $0081: if it has
 * reached/passed the limit $2928, just INC $0081 and return.  Otherwise copy 5
 * bytes from the source ($0087)+Y into $009B..$009F (cells $8F+$0C..$8F+$10) and
 * store the advanced cursor (Y+5) back to $0081. */
void hud_fill_field1(void) {
    uint8_t y = mem[0x0081];
    if (y >= mem[0x2928]) { mem[0x0081] = (uint8_t)(y + 1); return; }
    for (uint8_t x = 0x0C; x < 0x11; x++) {
        cpu.Y = y;
        mem[(uint8_t)(0x8F + x)] = bus_read(ZP_IND_Y(0x87));
        y = (uint8_t)(y + 1);
    }
    mem[0x0081] = y;
}

/* hud_fill_field3_font @ $8168 — HUD field-3 advance.  Cursor Y = $0083: if >= $A8,
 * INC $0083 and return.  Otherwise copy 7 font bytes from $35CD[Y] into $0094..$009A
 * (cells $8F+$05..$8F+$0B) and store the advanced cursor (Y+7) to $0083. */
void hud_fill_field3_font(void) {
    uint8_t y = mem[0x0083];
    if (y >= 0xA8) { mem[0x0083] = (uint8_t)(y + 1); return; }
    for (uint8_t x = 0x05; x < 0x0C; x++) {
        mem[(uint8_t)(0x8F + x)] = mem[0x35CD + y];
        y = (uint8_t)(y + 1);
    }
    mem[0x0083] = y;
}

/* clear_message_buffer @ $480B — clear the 14-byte message buffer: set X=$0E, A=$00
 * and tail-call the (native) fill_message_buffer, zeroing $32B7..$32C4. */
void clear_message_buffer(void) {
    cpu.X = 0x0E;
    cpu.A = 0x00;
    fill_message_buffer();
}

/* plot_pixel_col93 @ $66D3 — plot the pixel whose column index is $0093: load A=$0093
 * and tail-call the (native) plot_pixel_masked. */
void plot_pixel_col93(void) {
    cpu.A = mem[0x0093];
    plot_pixel_masked();
}

/* random_digit @ $5A59 — POKEY RANDOM rejection-sampled to a decimal digit 0-9
 * (re-roll the low nibble while it is >= $0A).  Result in cpu.A. */
void random_digit(void) {
    uint8_t a;
    do { a = (uint8_t)(bus_read(0xD20A) & 0x0F); } while (a >= 0x0A);
    cpu.A = a; CMP(0x0A);                 /* exit flags from the accepting CMP */
}

/* random_alpha_index @ $5A4D — POKEY RANDOM rejection-sampled to a letter index:
 * re-roll (RANDOM & $1F) while >= $1A (26 letters), then + $21 (carry clear from the
 * accepting CMP) giving a code in $21..$3A.  Result in cpu.A. */
void random_alpha_index(void) {
    uint8_t a;
    do { a = (uint8_t)(bus_read(0xD20A) & 0x1F); } while (a >= 0x1A);
    cpu.A = a; cpu.C = 0; ADC(0x21);
}

/* test_marked_neighbor @ $7047 — probe the $0900 marker map for a marked (negative)
 * cell at three offsets from the base index $009C+$009A: the base, base+$009A, and
 * base+$009B.  Returns the third cell's value in cpu.A iff all three are marked,
 * else 0.  Indices wrap 8-bit ($0900..$09FF). */
void test_marked_neighbor(void) {
    uint8_t fx = (uint8_t)(mem[0x009C] + mem[0x009A]);
    if (!(mem[0x0900 + fx] & 0x80)) { LDA(0x00); return; }
    uint8_t idx2 = (uint8_t)(fx + mem[0x009A]);
    if (!(mem[0x0900 + idx2] & 0x80)) { LDA(0x00); return; }
    uint8_t idx3 = (uint8_t)(fx + mem[0x009B]);
    uint8_t a3 = mem[0x0900 + idx3];
    if (a3 & 0x80) { LDA(a3); return; }
    LDA(0x00);
}

/* fill_region_2000 @ $3C83 — fill the byte $00B7 across $2000..$2F73 (count $0F73+1
 * = $0F74 bytes): seed the dest ptr $C1/$C2 = $2000 and the 16-bit count $C4:$C3 =
 * $0F73, then tail-call the (native) memset_or_copy. */
void fill_region_2000(void) {
    mem[0x00C1] = 0x00; mem[0x00C2] = 0x20;   /* dest $2000 */
    mem[0x00C3] = 0x73; mem[0x00C4] = 0x0F;   /* count $0F73 */
    memset_or_copy();
}

/* silence_audio_channels @ $7F60 — write entry A to the SFX gate $0634 and POKEY
 * AUDF1/2/3/4 ($D201/$D203/$D205/$D207), set AUDCTL ($D208) = $60, then tail-call
 * the (native) clear_var_0632.  POKEY writes go through bus_write (HW, no mem[]). */
void silence_audio_channels(void) {
    mem[0x0634] = cpu.A;
    bus_write(0xD201, cpu.A); bus_write(0xD203, cpu.A);
    bus_write(0xD205, cpu.A); bus_write(0xD207, cpu.A);
    cpu.A = 0x60;
    bus_write(0xD208, cpu.A);
    clear_var_0632();
}

/* init_terrain_render_buffers @ $753B — set the whole terrain boundary table
 * $260E..$270D = $FF (256 bytes), then fill $00B7 across $1070..$220F (count $11A0)
 * by seeding the dest ptr $C1/$C2 = $1070 + count $C4:$C3 = $119F and tail-calling
 * the (native) memset_or_copy. */
void init_terrain_render_buffers(void) {
    for (int i = 0; i < 256; i++) mem[0x260E + i] = 0xFF;
    mem[0x00C1] = 0x70; mem[0x00C2] = 0x10;   /* dest $1070 */
    mem[0x00C3] = 0x9F; mem[0x00C4] = 0x11;   /* count $119F */
    memset_or_copy();
}

/* game_init_7813 @ $7813 — fill $BC00[0..255] with a rotating bit pattern (seed $80,
 * each step >>2 then ROR if the 2nd shifted-out bit was set) and clear $BD00[0..255].
 * Pure (no input); the rotate carry chain is reproduced with the cpu shift macros. */
void game_init_7813(void) {
    cpu.A = 0x80;
    uint8_t y = 0x00;
    do {
        mem[0xBC00 + y] = cpu.A;
        LSR_A(); LSR_A();
        if (cpu.C) ROR_A();
        y = (uint8_t)(y + 1);
    } while (y != 0x00);
    cpu.A = y;                                 /* TYA — y wrapped to 0 */
    do {
        mem[0xBD00 + y] = cpu.A;
        y = (uint8_t)(y + 1);
    } while (y != 0x00);
}

/* game_sub_7B54 @ $7B54 — maybe seed the wind/drift cell $2849 with a random value.
 * Bails if $003A is negative or $2849 already nonzero.  Otherwise a = (RANDOM|$08)&$3F,
 * shifted right by 1 ($0004 nonzero), 3 ($0004==0 && $062B nonzero), or 0 (both zero). */
void game_sub_7B54(void) {
    if (mem[0x003A] & 0x80) return;
    if (mem[0x2849] != 0x00) return;
    uint8_t a = (uint8_t)((bus_read(0xD20A) | 0x08) & 0x3F);
    if (mem[0x0004] != 0x00) {
        a = (uint8_t)(a >> 1);
    } else if (mem[0x062B] != 0x00) {
        a = (uint8_t)(a >> 3);
    }
    mem[0x2849] = a;
}

/* rng_signed_jitter @ $687D — add or subtract the entry magnitude (cpu.A, saved to
 * $00B7) from the base $0085, choosing the sign from the high bit of a POKEY RANDOM
 * read: negative => $0085 - mag, else $0085 + mag.  Result in cpu.A. */
void rng_signed_jitter(void) {
    mem[0x00B7] = cpu.A;
    uint8_t base = mem[0x0085];
    uint8_t r = bus_read(0xD20A);
    cpu.Y = r;                                 /* LDY sets N from the RANDOM byte */
    cpu.A = base;
    if (r & 0x80) { cpu.C = 1; SBC(mem[0x00B7]); }
    else          { cpu.C = 0; ADC(mem[0x00B7]); }
}

/* init_cockpit_bar_cells @ $45C5 — seed the cockpit bar graphic cells: $BE into the
 * four corner pairs ($2107/8, $2137/8, $21C7/8, $21F7/8) and $AA into the two middle
 * pairs ($2167/8, $2197/8).  Pure (no input). */
void init_cockpit_bar_cells(void) {
    mem[0x2107] = 0xBE; mem[0x2108] = 0xBE; mem[0x2137] = 0xBE; mem[0x2138] = 0xBE;
    mem[0x21C7] = 0xBE; mem[0x21C8] = 0xBE; mem[0x21F7] = 0xBE; mem[0x21F8] = 0xBE;
    mem[0x2167] = 0xAA; mem[0x2168] = 0xAA; mem[0x2197] = 0xAA; mem[0x2198] = 0xAA;
}

/* add_and_show_bcd_counter @ $497D — add the 16-bit delta $0045/$0046 into the 4-byte
 * score $0600-$0603 (big-endian, $0603 = low) and re-render it.  The 6502 uses SED but
 * our ADC ignores the decimal flag (matching the transliterated oracle), so the add is
 * binary; tail-calls the (native) render_bcd_counter (which then clears $0045/$0046). */
void add_and_show_bcd_counter(void) {
    cpu.A = mem[0x0603]; cpu.C = 0; ADC(mem[0x0045]); mem[0x0603] = cpu.A;
    cpu.A = mem[0x0602];            ADC(mem[0x0046]); mem[0x0602] = cpu.A;
    cpu.A = mem[0x0601];            ADC(0x00);        mem[0x0601] = cpu.A;
    cpu.A = mem[0x0600];            ADC(0x00);        mem[0x0600] = cpu.A;
    render_bcd_counter();
}

/* plot_char_bounded @ $49D9 — plot one score/HUD digit at column Y through the dest
 * pointer $00C5/$00C6, with leading-zero suppression.  X is the running "seen a nonzero
 * digit" flag: plot unconditionally if X!=0; else if the digit A!=0 plot and set X;
 * else (a leading zero) plot only when the column Y has reached the threshold $0619
 * (forcing at least one "0").  The char code is digit + $50.  Y is always advanced. */
void plot_char_bounded(void) {
    uint8_t a = cpu.A, x = cpu.X, y = cpu.Y;
    int plot = 1;
    if (x == 0) {
        if (a != 0) {
            x = (uint8_t)(x + 1);                 /* INX — first nonzero digit */
        } else if (y < mem[0x0619]) {
            plot = 0;                             /* leading zero below threshold: blank */
        } else {
            x = (uint8_t)(x + 1);                 /* INX — forced zero at/after threshold */
        }
    }
    if (plot) {
        a = (uint8_t)(a + 0x50);                  /* CLC; ADC #$50 (char code) */
        cpu.Y = y;
        bus_write(ZP_IND_Y(0x00C5), a);
    }
    cpu.A = a;
    cpu.X = x;
    cpu.Y = (uint8_t)(y + 1);                     /* INY */
}

/* emit_bcd_byte_digits @ $49CE — plot both digits of a packed-BCD byte (entry A) via
 * plot_char_bounded: high nibble (A>>4) then low nibble (A&$0F), sharing the running
 * suppress flag X and column Y across the two calls. */
void emit_bcd_byte_digits(void) {
    uint8_t a = cpu.A;
    cpu.A = (uint8_t)(a >> 4);
    plot_char_bounded();
    cpu.A = (uint8_t)(a & 0x0F);
    plot_char_bounded();
}

/* mark_slot_and_countdown_char @ $7B39 — mark the active grid slot then show the
 * countdown glyph (both native). */
void mark_slot_and_countdown_char(void) {
    mark_grid_slot_active();
    countdown_show_char_0620();
}

/* mark_slot_and_inc_count @ $7B7D — mark the active grid slot then set place params +
 * bump the count (both native). */
void mark_slot_and_inc_count(void) {
    mark_grid_slot_active();
    set_place_params_inc_count();
}

/* Bare-RTS / no-op stubs (each a standalone RTS in the 6502; native = empty).  Porting
 * them removes them from the transpiled set and lets their tail-callers go native. */
void return_stub_40af(void) { }
void terrain_obj_skip_return(void) { }
void ret_stub_6a26(void) { }
void draw_bar_loop_end(void) { }
void terrain_plot_return(void) { }
void terrain_distance_clamp_return(void) { }
void plot_line_done(void) { }

/* scan_grid_neighbors @ $7069 — probe the four diagonal neighbours of the current grid
 * cell (offset pairs in $009A/$009B) via test_marked_neighbor; for each one that comes
 * back marked (negative), push the cell with push_grid_cell.  Both callees are native. */
void scan_grid_neighbors(void) {
    static const uint8_t off[4][2] = { {0x01,0x10}, {0xFF,0xF0}, {0x10,0xFF}, {0xF0,0x01} };
    for (int i = 0; i < 4; i++) {
        mem[0x009A] = off[i][0];
        mem[0x009B] = off[i][1];
        test_marked_neighbor();
        if (cpu.N) push_grid_cell();
    }
}

/* intro_reset_score_slots @ $4FCE — clear two score slots ($066A/$0686 = 0), set $0678
 * = $0C, and enqueue event Y=$0D via the (native) game_sub_55FC. */
void intro_reset_score_slots(void) {
    mem[0x066A] = 0x00; mem[0x0686] = 0x00; mem[0x0678] = 0x0C;
    cpu.Y = 0x0D;
    game_sub_55FC();
}

/* init_event_state_5815_x16 @ $7AA8 — seed event state ($0044 = entry A, $3388 = $B4,
 * $003C = 0) and enqueue marked event X=$16 via the (native) ring_push_marked. */
void init_event_state_5815_x16(void) {
    mem[0x0044] = cpu.A; mem[0x3388] = 0xB4; mem[0x003C] = 0x00;
    cpu.X = 0x16;
    ring_push_marked();
}

/* rle_run_fill @ $3C58 — RLE run expansion: entry A = run length (-> X), bump the source
 * pointer $00BB/$00BC, read the run byte at ($00BB)+Y, and replicate it A times into the
 * dest pointer $00BD/$00BE via the (native) copy_bytes_to_dst. */
void rle_run_fill(void) {
    cpu.X = cpu.A;                                  /* TAX — run length */
    mem[0x00BB] = (uint8_t)(mem[0x00BB] + 1);
    if (mem[0x00BB] == 0x00) mem[0x00BC] = (uint8_t)(mem[0x00BC] + 1);
    cpu.A = bus_read(ZP_IND_Y(0x00BB));             /* the run byte (uses entry cpu.Y) */
    copy_bytes_to_dst();
}

/* blit_glyph_8rows @ $678B — blit an 8-row glyph into the bitmap.  The initial row
 * pointer $0080/$0081 comes from the row-addr table (set_row_ptr_from_count via $0092),
 * then walks UP one screen row ($2E bytes) per glyph row.  For each of rows 7..0 the
 * glyph byte ($0084[Y]) is shifted left bit-by-bit; each set bit plots a pixel at the
 * running column $0093 (base $009C) via plot_pixel_col93 (mask index from $0094=$0095).
 * Finally the column base $009C advances by 8.  The 6502 PHA/PLA preserves the shifted
 * byte across the plot call — reproduced here with a local. */
void blit_glyph_8rows(void) {
    mem[0x0094] = mem[0x0095];
    set_row_ptr_from_count();                       /* $80/$81 = table[$0092] */
    mem[0x0097] = 0x07;
    do {
        mem[0x0093] = mem[0x009C];                  /* column base for this row */
        cpu.Y = mem[0x0097];
        uint8_t a = bus_read(ZP_IND_Y(0x84));       /* glyph byte for row $0097 */
        do {
            uint8_t carry = (uint8_t)(a >> 7);      /* ASL A: bit shifted out */
            a = (uint8_t)(a << 1);
            if (carry) plot_pixel_col93();          /* plot at column $0093 */
            mem[0x0093] = (uint8_t)(mem[0x0093] + 1);
        } while (a != 0x00);                        /* loop while bits remain */
        uint16_t p = (uint16_t)(mem[0x0080] | (mem[0x0081] << 8));
        p = (uint16_t)(p - 0x2E);                   /* row pointer up one screen row */
        mem[0x0080] = (uint8_t)p; mem[0x0081] = (uint8_t)(p >> 8);
        mem[0x0097] = (uint8_t)(mem[0x0097] - 1);
    } while (!(mem[0x0097] & 0x80));                /* BPL: loop while $0097 >= 0 */
    mem[0x009C] = (uint8_t)(mem[0x009C] + 0x08);
}

/* intro_seed_object_map @ $7498 — seed the intro object map.  Clear $0A00[0..255], place
 * a $C8 entry (slot Y = $10 if $006D==1 else $08) via obj_table_scan_a_c8, optionally place
 * a value-1 entry (slot Y=4) via obj_table_scan_replace when $006D >= 4, then sprinkle $64
 * markers across $0A00 at stride-$43 offsets where RANDOM < $0623 and $0900[X] is marked.
 * All scan callees are native and consume RANDOM identically on both sides. */
void intro_seed_object_map(void) {
    for (int i = 0; i < 256; i++) mem[0x0A00 + i] = 0x00;
    cpu.Y = (mem[0x006D] == 0x01) ? 0x10 : 0x08;
    obj_table_scan_a_c8();
    if (mem[0x006D] >= 0x04) {
        cpu.Y = 0x04; cpu.A = 0x01;
        obj_table_scan_replace();
    }
    uint8_t x = 0x00;
    do {
        if (bus_read(0xD20A) < mem[0x0623]) {        /* CMP $0623; BCS skip */
            if (mem[0x0900 + x] & 0x80)              /* BPL skip; act when marked */
                mem[0x0A00 + x] = 0x64;
        }
        x = (uint8_t)(x + 0x43);
    } while (x != 0x00);
}

/* intro_unmark_random_cells @ $70B3 — sometimes (always if $006D==1, else 1/8 on RANDOM&7==0)
 * sweep the $0900 marker map and clear the marked bit ($7F mask) of cells whose index has
 * bits 7 and 3 clear, gated on RANDOM < a per-pass threshold $00C3 = (RANDOM&$70)|$80.
 * RANDOM read counts are matched to the 6502 exactly (none on the $006D==1 early path). */
void intro_unmark_random_cells(void) {
    uint8_t y = (uint8_t)(mem[0x006D] - 1);          /* LDY $006D; DEY */
    if (y != 0x00) {                                 /* BEQ skips the RANDOM&7 gate */
        if ((bus_read(0xD20A) & 0x07) != 0x00) return;  /* BNE -> return */
    }
    mem[0x00C3] = (uint8_t)((bus_read(0xD20A) & 0x70) | 0x80);
    uint8_t i = 0x00;
    do {
        if (!(i & 0x80) && !(i & 0x08)) {
            if (bus_read(0xD20A) < mem[0x00C3])
                mem[0x0900 + i] = (uint8_t)(mem[0x0900 + i] & 0x7F);
        }
        i = (uint8_t)(i + 1);
    } while (i != 0x00);
}

/* font_display_init @ $5433 — clear the music/voice state tables ($066B..$06F7 in 11
 * parallel 14-entry columns + $0705 block + $0714) and the ring head $0073/$0074, then
 * seed the voice-priority slots ($0705[2..8] / POKEY AUDF via $D1FF+X) and the music
 * timer fields $0712/$0713 = 2/6, AUDCTL ($D208) = $60.  Pure init (POKEY via bus_write). */
void font_display_init(void) {
    mem[0x0073] = 0x00; mem[0x0074] = 0x00;
    static const uint16_t cols[11] = { 0x066B, 0x0705, 0x0687, 0x0695, 0x06A3, 0x06B1,
                                        0x06BF, 0x06CD, 0x06DB, 0x06E9, 0x06F7 };
    for (int y = 0x0E; y >= 1; y--)                 /* DEY;BNE: Y=$0E..1 */
        for (int c = 0; c < 11; c++) mem[cols[c] + y] = 0x00;
    mem[0x0714] = 0x00;
    mem[0x0715] = 0x02;
    for (int y = 4; y >= 1; y--) {                  /* DEY;BNE: Y=4..1 */
        uint8_t a = (uint8_t)(y << 1);              /* TYA; ASL */
        mem[0x0705 + y] = a;
        bus_write(0xD1FF + a, 0x00);                /* TAX; STA $D1FF,X (POKEY) */
    }
    mem[0x0706] = 0x00; mem[0x0708] = 0x00;
    mem[0x0712] = 0x02; mem[0x0713] = 0x06;
    bus_write(0xD208, 0x60);                        /* AUDCTL */
}

/* show_cockpit_message @ $47B8 — render a HUD message (entry Y = message id, bit7 set
 * suppresses the buffer clear, bit6 selects the alt colour $5A in $063E) into the message
 * buffer $32B7+ from the glyph tables $481E / $491A (indexed by $4927[id]+offset).  Y==9
 * also sets the colour $00D8=$38.  A glyph with bit7 set is the end marker: stored masked,
 * and if nonzero terminates the string. */
void show_cockpit_message(void) {
    uint8_t y = cpu.Y;
    if (y & 0x80) y = (uint8_t)(y & 0x7F);          /* bit7: skip clear */
    else clear_message_buffer();
    mem[0x0044] = 0x00;
    mem[0x063E] = 0xFF;
    if (y >= 0x40) {                                /* bit6: alt colour */
        y = (uint8_t)(y & 0x3F);
        mem[0x063E] = 0x5A;
    }
    if (y == 0x09) mem[0x00D8] = 0x38;
    uint8_t x = mem[0x4927 + y];
    mem[0x00BB] = y;
    uint8_t pos = 0x00;
    for (;;) {
        uint8_t a = (mem[0x00BB] >= 0x15) ? mem[0x491A + x] : mem[0x481E + x];
        if (a & 0x80) {                             /* end marker */
            a = (uint8_t)(a & 0x7F);
            mem[0x32B7 + pos] = a;
            if (a != 0) break;
        }
        mem[0x32B7 + pos] = a;
        pos = (uint8_t)(pos + 1);
        x = (uint8_t)(x + 1);
        if (pos == 0x0E) break;
    }
    platform_title_changed();   /* hook: HUD message rendered into $32B7 -> flag title dirty */
}

/* game_sub_6811 @ $6811 — scatter random dots (6 outer passes, growing mask $0082 and
 * threshold $0084).  Each dot: re-roll two coords ($0093/$0092 = RANDOM & $0082) until at
 * least one reaches the threshold $0084, then plot it at a jittered row/col — row via
 * rng_signed_jitter($2A)+set_row_ptr, col via rng_signed_jitter($2F)+plot_pixel_masked.
 * The mask index $0094 cycles 4..6 then 1..6.  All callees are native. */
void game_sub_6811(void) {
    mem[0x0094] = 0x04;
    mem[0x00DF] = 0x06;
    mem[0x0082] = 0x07;
    mem[0x0084] = 0x00;
    do {
        mem[0x0096] = (uint8_t)((mem[0x0082] | 0x07) >> 2);   /* ORA #7; LSR; LSR */
        do {
            for (;;) {                                        /* re-roll until accepted */
                mem[0x0093] = (uint8_t)(bus_read(0xD20A) & mem[0x0082]);
                uint8_t v92 = (uint8_t)(bus_read(0xD20A) & mem[0x0082]);
                mem[0x0092] = v92;
                if (v92 >= mem[0x0084]) break;                /* BCS accept */
                if (mem[0x0093] >= mem[0x0084]) break;        /* BCC retry inverted */
            }
            mem[0x0085] = 0x2A;
            cpu.A = mem[0x0092];
            rng_signed_jitter();                              /* jittered row in A */
            cpu.Y = cpu.A;
            set_row_ptr();
            mem[0x0085] = 0x2F;
            cpu.A = mem[0x0093];
            rng_signed_jitter();                              /* jittered col in A */
            plot_pixel_masked();
            mem[0x0096] = (uint8_t)(mem[0x0096] - 1);
        } while (mem[0x0096] != 0x00);
        mem[0x0082] = (uint8_t)(mem[0x0082] + 0x07);
        mem[0x0084] = (uint8_t)(mem[0x0084] + 0x06);
        mem[0x0094] = (uint8_t)(mem[0x0094] + 1);
        if (mem[0x0094] == 0x07) mem[0x0094] = 0x01;
        mem[0x00DF] = (uint8_t)(mem[0x00DF] - 1);
    } while (mem[0x00DF] != 0x00);
}

/* plot_terrain_span @ $692A — draw a run of vertical spans.  The span count $0096 comes
 * from $6E0F[Y] (entry Y; +2 when Y==0); each pass calls the (native) fill_vertical_span
 * and steps the column pair $009C--/$009D++.  Afterwards the row window is shifted by the
 * span value $0085 (=$6E0F[Y]): $009E += $0085, $009F -= $0085. */
void plot_terrain_span(void) {
    uint8_t a = mem[0x6E0F + cpu.Y];
    mem[0x0096] = a;
    if (cpu.Y == 0x00) {
        mem[0x0096] = (uint8_t)(mem[0x0096] + 1);
        mem[0x0096] = (uint8_t)(mem[0x0096] + 1);
    }
    mem[0x0085] = a;
    do {
        fill_vertical_span();
        mem[0x009C] = (uint8_t)(mem[0x009C] - 1);
        mem[0x009D] = (uint8_t)(mem[0x009D] + 1);
        mem[0x0096] = (uint8_t)(mem[0x0096] - 1);
    } while (mem[0x0096] != 0x00);
    mem[0x009E] = (uint8_t)(mem[0x009E] + mem[0x0085]);
    mem[0x009F] = (uint8_t)(mem[0x009F] - mem[0x0085]);
}

/* trigger_effect_4a @ $7AA6 — fire event effect $4A via init_event_state_5815_x16. */
void trigger_effect_4a(void) {
    cpu.A = 0x4A;
    init_event_state_5815_x16();
}

/* terrain_plot_skip_return @ $A6F8 — bare RTS (no-op). */
void terrain_plot_skip_return(void) { }

/* render_bcd_low_bytes @ $49AE — render the two low score bytes $0602/$0603 as digit
 * pairs via the (native) emit_bcd_byte_digits. */
void render_bcd_low_bytes(void) {
    cpu.A = mem[0x0602]; emit_bcd_byte_digits();
    cpu.A = mem[0x0603]; emit_bcd_byte_digits();
}

/* set_zsupp_pos_clear_delta @ $49C5 — set the zero-suppress threshold $0619 = entry Y,
 * clear the 16-bit delta $0045/$0046, then render a digit pair via emit_bcd_byte_digits
 * (entry A = the BCD byte). */
void set_zsupp_pos_clear_delta(void) {
    mem[0x0619] = cpu.Y;
    cpu.Y = 0x00;
    mem[0x0045] = cpu.Y;
    mem[0x0046] = cpu.Y;
    emit_bcd_byte_digits();
}

/* save_color_clear_y_bit5 @ $47B2 — stash colour $00D8 = entry A, clear bit5 of the
 * message id (entry Y), and render it via the (native) show_cockpit_message. */
void save_color_clear_y_bit5(void) {
    mem[0x00D8] = cpu.A;
    cpu.Y = (uint8_t)(cpu.Y & 0xDF);
    show_cockpit_message();
}

/* shift_object_table_up @ $6A0F — shift the display-list LMS address pairs up by 3 bytes
 * ($3007/$3008[Y] -> $300A/$300B[Y]) for entry-A iterations, stepping Y down by 3. */
void shift_object_table_up(void) {
    mem[0x0084] = cpu.A;
    uint8_t y = 0xFF;
    do {
        mem[0x300A + y] = mem[0x3007 + y];
        mem[0x300B + y] = mem[0x3008 + y];
        y = (uint8_t)(y - 3);
        mem[0x0084] = (uint8_t)(mem[0x0084] - 1);
    } while (mem[0x0084] != 0x00);
}

/* set_coord_y_e0 @ $6805 — set the glyph source pointer $0084/$0085 = $E0(entry A + $80)
 * and blit it via the (native) blit_glyph_8rows. */
void set_coord_y_e0(void) {
    cpu.C = 0; ADC(0x80);                 /* CLC; ADC #$80 (entry A) */
    mem[0x0084] = cpu.A;
    cpu.Y = 0xE0;
    mem[0x0085] = cpu.Y;
    blit_glyph_8rows();
}

/* glyph_ptr_from_index @ $6773 — compute the glyph source pointer for index (entry A):
 * $0084/$0085 = $E000 + (index << 3) (with the 6502's single-ROL carry capture), then blit
 * via the (native) blit_glyph_8rows.  Reproduced with the cpu shift/ADC macros. */
void glyph_ptr_from_index(void) {
    cpu.Y = 0x00;
    mem[0x0085] = cpu.Y;
    ASL_A(); ASL_A(); ASL_A();            /* index << 3 (carry from last shift) */
    mem[0x0084] = cpu.A;
    ROL_M(0x0085);                        /* capture the last carry */
    cpu.C = 0; cpu.A = mem[0x0084]; ADC(0x00); mem[0x0084] = cpu.A;
    cpu.A = mem[0x0085]; ADC(0xE0); mem[0x0085] = cpu.A;   /* + $E000 */
    blit_glyph_8rows();
}

/* draw_glyph_2rows @ $4099 — draw a 2x2-byte glyph (rows $30 apart) from the table
 * $4AE3[index..index+3] (entry A), OR'd with $00BF, through the dest pointer $00BB at
 * column offsets 0,1,$30,$31. */
void draw_glyph_2rows(void) {
    uint8_t x = cpu.A;
    uint8_t y = 0x00;
    for (;;) {
        cpu.Y = y;
        bus_write(ZP_IND_Y(0x00BB), (uint8_t)(mem[0x4AE3 + x] | mem[0x00BF]));
        x = (uint8_t)(x + 1);
        y = (uint8_t)(y + 1);
        if (y == 0x02) y = 0x30;          /* CPY #2; BNE; LDY #$30 */
        if (y == 0x32) break;             /* CPY #$32; BNE loop */
    }
}

/* rle_expand_list @ $757B — expand a list of RLE runs from the source pointer $00BB: each
 * (count,value) pair is expanded into the dest via the (native) rle_run_fill; a count of 0
 * terminates. */
void rle_expand_list(void) {
    cpu.Y = 0x00;
    for (;;) {
        cpu.A = bus_read(ZP_IND_Y(0x00BB));   /* ($BB)+0 = run length */
        if (cpu.A == 0x00) return;
        rle_run_fill();
    }
}

/* rle_decompress @ $3C3D — decompress an RLE stream from the source pointer $00BB into the
 * dest $00BD/$00BE.  A byte < $C0 is a literal (copied once via copy_bytes_to_dst); a byte
 * >= $C0 is a run marker whose low 6 bits are the length (length 0 = $C0 terminator), with
 * the following byte the repeated value (via rle_run_fill). */
void rle_decompress(void) {
    cpu.Y = 0x00;
    for (;;) {
        cpu.A = bus_read(ZP_IND_Y(0x00BB));
        if (cpu.A < 0xC0) {                   /* CMP #$C0; BCC literal */
            cpu.X = 0x01;
            copy_bytes_to_dst();
        } else {
            cpu.A = (uint8_t)(cpu.A & 0x3F);
            if (cpu.A == 0x00) return;        /* $C0 terminator */
            rle_run_fill();
        }
    }
}

/* loader_util @ $3C00 — clear three regions with byte $00B7=0 via the (native) memset_or_copy:
 * $32B5..$32FC ($48 bytes), $1000..$2FFF ($2000 bytes), and $0B00..$0FFF ($500 bytes). */
void loader_util(void) {
    mem[0x00B7] = 0x00;
    mem[0x00C1] = 0xB5; mem[0x00C2] = 0x32; mem[0x00C3] = 0x47; mem[0x00C4] = 0x00;
    memset_or_copy();
    mem[0x00C1] = 0x00; mem[0x00C2] = 0x10; mem[0x00C3] = 0xFF; mem[0x00C4] = 0x1F;
    memset_or_copy();
    mem[0x00C1] = 0x00; mem[0x00C2] = 0x0B; mem[0x00C3] = 0xFF; mem[0x00C4] = 0x04;
    memset_or_copy();
}

/* game_init_77DF @ $77DF — build the 256-entry PMG/strip bit tables $BE00 and $BF00: for
 * each X (0..255) a rolling value seeded from X feeds, over 4 inner passes, 2 bits at a time
 * (via $780B[]/$780F[] lookups and LSR/ROR) into $BE00[X] and $BF00[X].  Pure; the carry-chain
 * ROR-into-memory is reproduced with the cpu macros. */
void game_init_77DF(void) {
    uint8_t x = 0x00;
    do {
        cpu.A = x;                                  /* TXA */
        mem[0x00C1] = 0x04;
        do {
            uint8_t saved = cpu.A;                  /* PHA */
            cpu.A &= 0x03;
            uint8_t y = cpu.A;                      /* TAY */
            cpu.A = mem[0x780B + y];
            LSR_A(); ROR_M(0xBE00 + x);
            LSR_A(); ROR_M(0xBE00 + x);
            cpu.A = mem[0x780F + y];
            LSR_A(); ROR_M(0xBF00 + x);
            LSR_A(); ROR_M(0xBF00 + x);
            cpu.A = saved;                          /* PLA */
            LSR_A(); LSR_A();
            mem[0x00C1] = (uint8_t)(mem[0x00C1] - 1);
        } while (mem[0x00C1] != 0x00);
        x = (uint8_t)(x + 1);                       /* INX */
    } while (x != 0x00);
}

/* plot_clipped_pixel @ $7D38 — plot one clipped HUD/radar pixel.  Entry A = source value
 * ($0058; 0 means "read the existing screen byte").  The pixel at ($004F,$004E) is plotted
 * only inside the window Y∈[$6C,$97) and X∈[$28,$D8) and when its packed column index stays
 * below $00B3.  The screen row pointers $0080 / $00C1 come from the row-addr table indexed by
 * $97-$004E; the pixel is masked into ($00C1)+col via $4F3B[X&3]/$7DEB[X&3].  $004F is always
 * incremented. */
void plot_clipped_pixel(void) {
    mem[0x0058] = cpu.A;
    uint8_t x = mem[0x004F];
    uint8_t y = mem[0x004E];
    if (y >= 0x6C && y < 0x97 && x >= 0x28 && x < 0xD8) {
        uint8_t ry = (uint8_t)(0x97 - mem[0x004E]);
        cpu.C = 0;
        cpu.A = mem[0x073D + ry]; mem[0x0080] = cpu.A; ADC(0x30); mem[0x00C1] = cpu.A;
        cpu.A = mem[0x0793 + ry]; mem[0x0081] = cpu.A; ADC(0x00); mem[0x00C2] = cpu.A;
        cpu.A = x;
        LSR_A(); LSR_A();
        cpu.C = 0; ADC(0xF8);
        uint8_t col = cpu.A;
        CMP(mem[0x00B3]);
        if (!cpu.C) {                               /* BCS skip => plot when col < $00B3 */
            cpu.Y = col;
            uint8_t mx = (uint8_t)(x & 0x03);
            uint8_t a = mem[0x0058];
            if (a == 0x00) a = bus_read(ZP_IND_Y(0x0080));
            a &= mem[0x4F3B + mx];
            mem[0x0095] = a;
            uint8_t b = bus_read(ZP_IND_Y(0x00C1));
            b &= mem[0x7DEB + mx];
            b |= mem[0x0095];
            bus_write(ZP_IND_Y(0x00C1), b);
        }
    }
    mem[0x004F] = (uint8_t)(mem[0x004F] + 1);
}

/* unpack_bitmap_4d3e @ $74D7 — unpack a bitmap by bit-reversing bytes between buffers whose
 * pointers come from the $4D3E word table.  Outer 8 passes x middle 4 passes: src ptr $00C1 =
 * table[$0084], dst ptr $00C3 = table[$0080-$0081]; the byte loop reads ($C1)+Y, reverses its
 * bits (8x ASL/ROR into $0085), writes ($C3)+Y, stopping when a reversed byte is 0 or Y reaches
 * $80.  Indices step $0084/$0081 by 2 (middle) and $0080/$0084 by $12/$0A (outer). */
void unpack_bitmap_4d3e(void) {
    mem[0x0084] = 0x00;
    mem[0x0080] = 0x10;
    mem[0x00DF] = 0x08;
    do {
        mem[0x0081] = 0x00;
        mem[0x0082] = 0x04;
        do {
            uint8_t ys = mem[0x0084];
            mem[0x00C1] = mem[0x4D3E + ys]; mem[0x00C2] = mem[0x4D3F + ys];
            uint8_t yd = (uint8_t)(mem[0x0080] - mem[0x0081]);
            mem[0x00C3] = mem[0x4D3E + yd]; mem[0x00C4] = mem[0x4D3F + yd];
            uint8_t y = 0x00;
            for (;;) {
                cpu.Y = y;
                cpu.A = bus_read(ZP_IND_Y(0x00C1));
                for (int k = 0; k < 8; k++) { ASL_A(); ROR_M(0x0085); }  /* reverse bits */
                cpu.A = mem[0x0085];
                cpu.Y = y;
                bus_write(ZP_IND_Y(0x00C3), cpu.A);
                if (cpu.A == 0x00) break;            /* BEQ */
                y = (uint8_t)(y + 1);
                if (y & 0x80) break;                 /* BPL: loop while Y < $80 */
            }
            mem[0x0084] = (uint8_t)(mem[0x0084] + 2);
            mem[0x0081] = (uint8_t)(mem[0x0081] + 2);
            mem[0x0082] = (uint8_t)(mem[0x0082] - 1);
        } while (mem[0x0082] != 0x00);
        mem[0x0080] = (uint8_t)(mem[0x0080] + 0x12);
        mem[0x0084] = (uint8_t)(mem[0x0084] + 0x0A);
        mem[0x00DF] = (uint8_t)(mem[0x00DF] - 1);
    } while (mem[0x00DF] != 0x00);
}

/* intro_random_setup @ $6FBF — depth-first maze/grid generation on the $0900 16x16 cell map.
 * Fill all 256 cells with RANDOM&$3F|$80 (bit7 = unvisited), seed a start cell, then DFS: at
 * each step scan_grid_neighbors pushes marked neighbours onto the $2500 stack ($0098); pick a
 * random marked direction (BIT RANDOM -> one of 4 offset pairs matching scan_grid_neighbors,
 * retried until test_marked_neighbor confirms it), carve through the wall + next cell (bit7
 * cleared), advance; when a cell has no marked neighbours, pop the stack until it empties. */
void intro_random_setup(void) {
    uint8_t x = 0x00, y = 0x00;
    do {                                            /* fill $0900[0..255] = RANDOM&$3F|$80 */
        mem[0x0900 + x] = (uint8_t)((bus_read(0xD20A) & 0x3F) | 0x80);
        x = (uint8_t)(x + 1);
        y = (uint8_t)(y - 1);
    } while (y != 0x00);
    mem[0x0098] = 0x01;
    cpu.A = bus_read(0xD20A); mem[0x009C] = cpu.A;
    mem[0x0900 + cpu.A] = (uint8_t)(bus_read(0xD20A) & 0x3F);
    for (;;) {                                      /* L_6fe3 */
        mem[0x0099] = mem[0x0098];
        scan_grid_neighbors();
        if (mem[0x0098] == mem[0x0099]) {           /* no neighbours pushed -> backtrack */
            mem[0x0098] = (uint8_t)(mem[0x0098] - 1);
            if (mem[0x0098] == 0x00) return;
            mem[0x009C] = mem[0x2500 + mem[0x0098]];
            continue;
        }
        for (;;) {                                  /* L_6ff0: pick a marked direction */
            uint8_t r = bus_read(0xD20A);           /* BIT: N=bit7, V=bit6 */
            uint8_t a, b;
            int V = (r >> 6) & 1, Nf = (r >> 7) & 1;
            if (V) { if (Nf) { a = 0xF0; b = 0x01; } else { a = 0x10; b = 0xFF; } }
            else   { if (Nf) { a = 0xFF; b = 0xF0; } else { a = 0x01; b = 0x10; } }
            mem[0x009A] = a; mem[0x009B] = b;
            test_marked_neighbor();
            if (cpu.N) break;                       /* BPL retry inverted: proceed when marked */
        }
        uint8_t cx = (uint8_t)(mem[0x009C] + mem[0x009A]);
        mem[0x0900 + cx] = (uint8_t)(bus_read(0xD20A) & 0x3F);
        cx = (uint8_t)(cx + mem[0x009A]);
        mem[0x0900 + cx] = (uint8_t)(bus_read(0xD20A) & 0x3F);
        mem[0x009C] = cx;
    }
}

/* emit_dl_coord_pairs @ $68CF — emit display-list LMS coordinate pairs.  Copy $0096 (=$6E0F[Y])
 * row-addr table entries into the DL region $300A/$300B (Y descending from $00C3, X descending
 * from $00C4), then the same count into $308B/$308C (Y ascending from $00C1, X ascending from
 * $00C2).  Advance $00C3/$00C1 by +-3*$6E0F[$00B9], then tail-call the (native) plot_terrain_span. */
void emit_dl_coord_pairs(void) {
    cpu.A = mem[0x6E0F + cpu.Y];
    mem[0x0096] = cpu.A;
    uint8_t count = cpu.A;                          /* PHA */
    uint8_t xi = mem[0x00C4], yi = mem[0x00C3];
    do {
        mem[0x300A + yi] = mem[0x073D + xi];
        mem[0x300B + yi] = mem[0x0793 + xi];
        yi = (uint8_t)(yi - 3);
        xi = (uint8_t)(xi - 1);
        mem[0x0096] = (uint8_t)(mem[0x0096] - 1);
    } while (mem[0x0096] != 0x00);
    mem[0x00C4] = xi;
    mem[0x0096] = count;                            /* PLA */
    xi = mem[0x00C2]; yi = mem[0x00C1];
    do {
        mem[0x308B + yi] = mem[0x073D + xi];
        mem[0x308C + yi] = mem[0x0793 + xi];
        yi = (uint8_t)(yi + 3);
        xi = (uint8_t)(xi + 1);
        mem[0x0096] = (uint8_t)(mem[0x0096] - 1);
    } while (mem[0x0096] != 0x00);
    mem[0x00C2] = xi;
    cpu.Y = mem[0x00B9];
    cpu.C = 1; cpu.A = mem[0x00C3];
    SBC(mem[0x6E0F + cpu.Y]); SBC(mem[0x6E0F + cpu.Y]); SBC(mem[0x6E0F + cpu.Y]);
    mem[0x00C3] = cpu.A;
    cpu.C = 0; cpu.A = mem[0x00C1];
    ADC(mem[0x6E0F + cpu.Y]); ADC(mem[0x6E0F + cpu.Y]); ADC(mem[0x6E0F + cpu.Y]);
    mem[0x00C1] = cpu.A;
    plot_terrain_span();
}

/* compute_gauge_geometry_from_006D @ $75F5 — derive the throttle/speed gauge
 * parameter block from the current gauge value $006D (held in X throughout).
 * Pure-compute leaf: a chain of clamps/shifts of X writing the gauge geometry
 * cells $0617-$062A (+ $08A2, scratch $00C1), with the displayed value run
 * through the native bin_to_bcd for $0628.  No loops.  Contract: mem[] — exit
 * regs are dead at the display_setup call site; the PHA/PLA byte at $01FF that
 * the 6502 leaves behind (S=$FF in the harness) is masked in validate_native.c. */
void compute_gauge_geometry_from_006D(void) {
    uint8_t x = mem[0x006D];

    /* $75F5-$7600: P = min((X>>1)+2, $14) */
    uint8_t p = (uint8_t)((x >> 1) + 2);
    if (p >= 0x14) p = 0x14;

    /* $7602-$7609: $062A = P; $0628 = BCD(P) via native bin_to_bcd */
    mem[0x062A] = p;
    cpu.A = p;
    bin_to_bcd();                       /* sets $00C1=ones, cpu.Y=tens, cpu.A=BCD */
    mem[0x0628] = cpu.A;

    /* $760d-$7612: $061F = (P>>1)+1 */
    mem[0x061F] = (uint8_t)((p >> 1) + 1);

    /* $7615-$761d: $0620 = (P>=5) ? P-5 : 0 */
    mem[0x0620] = (p >= 5) ? (uint8_t)(p - 5) : 0;

    /* $7620-$7632: $0623 = (X==1) ? 0 : (min(X,$2B) << 2) */
    mem[0x0623] = (x == 1) ? 0 : (uint8_t)(((x >= 0x2B) ? 0x2B : x) << 2);

    /* $7635-$7650: $0621 = $0622 = (X<4) ? 0 : $58 - 2*min(X,$22) */
    {
        uint8_t v;
        if (x < 4) {
            v = 0;
        } else {
            uint8_t c = (x >= 0x23) ? 0x22 : x;
            mem[0x00C1] = (uint8_t)(c << 1);
            v = (uint8_t)(0x58 - mem[0x00C1]);
        }
        mem[0x0621] = v;
        mem[0x0622] = v;
    }

    /* $7653-$7662: $0624 = ($2C - min(X,$28)) >> 1 */
    {
        uint8_t c = (x >= 0x28) ? 0x28 : x;
        mem[0x00C1] = c;
        mem[0x0624] = (uint8_t)((0x2C - c) >> 1);
    }

    /* $7665: $00C1 = X (raw) — base for the next two derivations */
    mem[0x00C1] = x;

    /* $7667-$7675: r=$2A-X (8-bit); if r bit7 set -> 4, else max(r>>1, 4).
     * BMI/CMP test bit7 of the 8-bit SBC result, not the true sign. */
    {
        uint8_t r = (uint8_t)(0x2A - x);
        uint8_t v;
        if (r & 0x80) {
            v = 4;
        } else {
            v = (uint8_t)(r >> 1);
            if (v < 4) v = 4;
        }
        mem[0x0618] = v;
    }

    /* $7678-$7699: r=$1A-X (8-bit) -> $061A, then clamp/scale into $061B.
     * BMI tests bit7 of the 8-bit result (skips the LSR when set). */
    {
        uint8_t r = (uint8_t)(0x1A - x);
        uint8_t a;
        if (r & 0x80) {
            a = r;                          /* bit7 set, no LSR */
        } else {
            a = (uint8_t)(r >> 1);
        }
        mem[0x061A] = a;
        if ((a & 0x80) || a < 2) a = 2;     /* $7683-$768a: clamp min 2 */
        a = (a >= 8) ? 0xFF : (uint8_t)(a << 5);   /* $768b-$7698 */
        mem[0x061B] = a;
    }

    /* $769c-$76a2: $061C = (X>>2)+5 */
    mem[0x061C] = (uint8_t)((x >> 2) + 5);

    /* $76a5-$76b2: $00C1 = X>>2; $0625 = (X>>2 <= 8) ? 8-(X>>2) : 0 */
    {
        uint8_t v = (uint8_t)(x >> 2);
        mem[0x00C1] = v;
        mem[0x0625] = (v <= 8) ? (uint8_t)(8 - v) : 0;
    }

    /* $76b5-$76c7: X>=$10 -> $08A2 = min(X-$10,$17), $0617 = 0; else $0617 = X */
    if (x >= 0x10) {
        uint8_t d = (uint8_t)(x - 0x10);
        mem[0x08A2] = (d >= 0x18) ? 0x17 : d;
        mem[0x0617] = 0;
    } else {
        mem[0x0617] = x;
    }
}

/* blit_label_row @ $6750 — blit a 5-glyph label row.  Index base = (mem[$0004]!=0)
 * ? 5 : 0; for the 5 codes mem[$6E23+base..base+4] call native glyph_ptr_from_index
 * (-> $E000 charset glyph -> blit_glyph_8rows) at fixed row $0092=$2E, column $009C=$1B.
 * Contract: mem[] (blit chain writes the $073D/$0793 row-addr-table bitmap; the
 * blit_glyph_8rows PHA/PLA stack scribble is masked in validate_native.c). */
void blit_label_row(void) {
    mem[0x0092] = 0x2E;
    mem[0x00C5] = (mem[0x0004] != 0) ? 0x05 : 0x00;   /* LDX $0004; BEQ -> keep, else 5 */
    mem[0x009C] = 0x1B;
    mem[0x00C6] = 0x05;                                /* loop count */
    do {
        cpu.A = mem[0x6E23 + mem[0x00C5]];
        glyph_ptr_from_index();
        mem[0x00C5]++;
        mem[0x00C6]--;
    } while (mem[0x00C6] != 0x00);
}

/* blit_message_block @ $672D — draw the message frame: from row $0092=$54 stepping
 * up by 8 while non-negative, set the row ptr ($80/$81 = table[$0092]) and plot the
 * three frame pixels at columns $15/$2E/$47; then tail-call blit_label_row.  $0094=0
 * (the plot_pixel_masked column-page base).  Contract: mem[] (same row-table bitmap). */
void blit_message_block(void) {
    mem[0x0092] = 0x54;
    mem[0x0094] = 0x00;
    do {
        set_row_ptr_from_count();
        cpu.A = 0x15; plot_pixel_masked();
        cpu.A = 0x2E; plot_pixel_masked();
        cpu.A = 0x47; plot_pixel_masked();
        mem[0x0092] = (uint8_t)(mem[0x0092] - 0x08);
    } while (!(mem[0x0092] & 0x80));                  /* BPL: loop while bit7 clear */
    blit_label_row();
}

/* draw_digit_low_nibble @ $4095 — A = (A & $0F) << 2, then tail draw_glyph_2rows
 * (the 2x2 glyph for that index).  Macros keep the N/Z/C flags identical to the
 * 6502 at the draw_glyph_2rows entry. */
void draw_digit_low_nibble(void) {
    AND(0x0F); ASL_A(); ASL_A();
    draw_glyph_2rows();
}

/* draw_2digit_value @ $4084 — render a packed-BCD byte as two 2x2 glyphs: draw the
 * high nibble ((A>>2)&$3C) at dest $BB/$BC, advance the dest pointer ($BD/$BE ->
 * $BB/$BC), then tail draw_digit_low_nibble for the low nibble.  PHA/PLA preserves
 * the entry byte across the first draw (kept in a local; the $01FF scribble is masked). */
void draw_2digit_value(void) {
    uint8_t saved = cpu.A;                 /* PHA */
    LSR_A(); LSR_A(); AND(0x3C);
    draw_glyph_2rows();
    mem[0x00BB] = mem[0x00BD];
    mem[0x00BC] = mem[0x00BE];
    LDA(saved);                            /* PLA */
    draw_digit_low_nibble();
}

/* glyph_ptr_shift3 @ $6802 — A <<= 3, then tail set_coord_y_e0 (builds the $E0xx
 * glyph source pointer from A and blits via blit_glyph_8rows). */
void glyph_ptr_shift3(void) {
    ASL_A(); ASL_A(); ASL_A();
    set_coord_y_e0();
}

/* render_bcd_top_byte @ $49C0 — entry point of the score render chain for the top
 * (most-significant) BCD byte: Y=5 (zero-suppress threshold), X=$0600 (running
 * suppress flag), then tail set_zsupp_pos_clear_delta (-> emit_bcd_byte_digits emits
 * the two nibbles of the entry-A byte). */
void render_bcd_top_byte(void) {
    LDY(0x05);
    LDX(mem[0x0600]);
    set_zsupp_pos_clear_delta();
}

/* unpack_terrain_seed_cols @ $7558 — seed the two terrain seed columns by running the
 * native rle_expand_list twice: source $4DFA -> dest $0C32, then source $4E09 -> $0D32.
 * Pure pointer setup + two native calls. */
void unpack_terrain_seed_cols(void) {
    mem[0x00BB] = 0xFA; mem[0x00BC] = 0x4D;   /* src $4DFA */
    mem[0x00BD] = 0x32; mem[0x00BE] = 0x0C;   /* dst $0C32 */
    rle_expand_list();
    mem[0x00BB] = 0x09; mem[0x00BC] = 0x4E;   /* src $4E09 */
    mem[0x00BD] = 0x32; mem[0x00BE] = 0x0D;   /* dst $0D32 */
    rle_expand_list();
}

/* game_init_7588 @ $7588 — fill $32FD..$332C (Y=$2F..0) with $AA, then decompress the
 * shape stream at $6E6E into $332D via the native rle_decompress. */
void game_init_7588(void) {
    for (uint8_t y = 0x2F; ; y--) {
        mem[0x32FD + y] = 0xAA;
        if (y == 0) break;
    }
    mem[0x00BD] = 0x2D; mem[0x00BE] = 0x33;   /* dst $332D */
    mem[0x00BB] = 0x6E; mem[0x00BC] = 0x6E;   /* src $6E6E */
    rle_decompress();
}

/* render_bcd_digits_supp_all @ $49BA — Y=7 (zero-suppress threshold), X=0 (suppress
 * flag), then set_zsupp_pos_clear_delta.  The 6502 does LDX #0 then BEQ, so the branch
 * to set_zsupp_pos_clear_delta is unconditional (the render_bcd_top_byte tail is dead). */
void render_bcd_digits_supp_all(void) {
    LDY(0x07);
    LDX(0x00);
    set_zsupp_pos_clear_delta();
}

/* blit_numeric_readout @ $67C3 — render the cockpit numeric readout.  Row $0092=$38.
 * If $0004!=0: draw 4 glyphs (codes $060F/$060D/$0610/$060E) via glyph_ptr_from_index /
 * glyph_ptr_shift3 at column $009C=$1F.  Else: clamp $006D to $63, BCD it, and draw the
 * tens ((BCD>>1)&$78 via set_coord_y_e0) and units ((BCD&$0F)<<3 via glyph_ptr_shift3) at
 * column $009C=$27.  The PHA/PLA across the tens draw is kept in a local (the $01FF
 * scribble is masked in the test). */
void blit_numeric_readout(void) {
    mem[0x0092] = 0x38;
    if (mem[0x0004] != 0) {
        mem[0x009C] = 0x1F;
        cpu.A = mem[0x060F]; glyph_ptr_from_index();
        cpu.A = mem[0x060D]; glyph_ptr_shift3();
        cpu.A = mem[0x0610]; glyph_ptr_from_index();
        cpu.A = mem[0x060E]; glyph_ptr_shift3();
        return;
    }
    mem[0x009C] = 0x27;
    cpu.A = mem[0x006D];
    if (cpu.A >= 0x63) { cpu.A = 0x63; mem[0x006D] = 0x63; }
    bin_to_bcd();                       /* A = packed BCD */
    uint8_t bcd = cpu.A;                /* PHA */
    LSR_A(); AND(0x78);                 /* tens index */
    set_coord_y_e0();
    LDA(bcd);                           /* PLA */
    AND(0x0F);                          /* units index */
    glyph_ptr_shift3();
}

/* dl_lms_fill @ $69F1 — build display-list LMS coordinate words.  For X=$008B up to
 * (but excluding) $0086, copy the pair ($073D[X], $0793[X]) into ($C5/$C6)+Y with Y
 * advancing by 3 per X-step; only the first INY's wrap bumps the high byte $00C6 (the
 * faithful 6502 quirk — the trailing INY INY is unchecked).  Then if $008B!=0 shift the
 * object table up, else no-op.  Dest is RAM (DL region $30xx) so a plain mem[] store
 * matches the 6502's STA ($C5),Y (rendered bus_write, non-HW -> mem[]). */
void dl_lms_fill(void) {
    uint8_t x = mem[0x008B];
    uint8_t y = 0x00;
    do {
        uint16_t base = (uint16_t)(mem[0x00C5] | (mem[0x00C6] << 8));
        mem[(uint16_t)(base + y)] = mem[0x073D + x];
        y++;                                       /* INY */
        if (y == 0x00) mem[0x00C6]++;              /* BNE skip; INC $C6 on wrap */
        base = (uint16_t)(mem[0x00C5] | (mem[0x00C6] << 8));
        mem[(uint16_t)(base + y)] = mem[0x0793 + x];
        y = (uint8_t)(y + 2);                      /* INY INY (unchecked) */
        x++;                                       /* INX */
    } while (x != mem[0x0086]);
    if (mem[0x008B] == 0x00) return;               /* ret_stub_6a26 (no-op) */
    cpu.A = mem[0x008B];
    shift_object_table_up();
}

/* draw_dial_bar_column @ $43CB — update one cockpit dial-bar column to value Y.
 * No-op if (Y>=9 && $062E==8) or Y already == $062E; otherwise latch $062E=Y, set the
 * bar params ($00BF=Y threshold, $00BE=$FF loop end, $00BD=$07 start) and draw via the
 * native draw_object_column (entry A=$07). */
void draw_dial_bar_column(void) {
    uint8_t y = cpu.Y;
    if (y >= 0x09 && mem[0x062E] == 0x08) return;
    if (y == mem[0x062E]) return;
    mem[0x062E] = y;
    mem[0x00BF] = y;
    mem[0x00BE] = 0xFF;
    mem[0x00BD] = 0x07;
    cpu.A = 0x07;
    draw_object_column();
}

/* draw_player3_object @ $42A7 — render the player-3 "lock-on" indicator sprite.  Entry A
 * selects the object: A>=3 uses a fixed config (HPOSP3=0, size $0C, col $6C); A<3 looks up
 * HPOS/size from $4566/$4569 and derives the column/size index from $2824 via $456C/$457A.
 * Then (if $2835!=0) clears a strip of the PMG buffers $0F1E/$0F71, and finally — for the
 * original entry A in $03..$1F — computes a screen position and blits the sprite mask
 * (bytes via the $4D3E/$4D3F pointer; RANDOM-dithered when $2826==0) into $0F1E/$0F71,
 * writing HPOSP3 ($D003) and the size clamp $00CC.
 *
 * Faithful transliteration.  HW touched via bus_read/bus_write ($D00B HPOSP3 hi?, $D003,
 * $D20A RANDOM — seeded identically in both runs).  Indexed RAM stores ($0F1E/$0F71,Y/X)
 * are plain mem[].  GOTCHA reproduced: the SBC #$55 at $42D3 inherits its borrow from
 * which CMP #$8B branch was taken (the $8C clamp path carries C=1, the fall-through C=0).
 * Contract: mem[] (exit regs dead); the entry PHA/PLA scribble at $01FF is masked. */
void draw_player3_object(void) {
    uint8_t entryA = cpu.A;                 /* PHA */
    uint8_t a, x, y = 0;

    if (cpu.A >= 0x03) {                     /* CMP #3; BCC L_42bc */
        bus_write(0xD00B, 0x00);
        mem[0x2823] = 0x0C;
        x = 0xFF;
        a = 0x6C;                            /* BNE L_42ef (always) */
    } else {
        /* L_42bc: A in {0,1,2} */
        y = cpu.A;                           /* TAY */
        bus_write(0xD00B, mem[0x4566 + y]);
        mem[0x2823] = mem[0x4569 + y];
        /* SEC; LDA $2824; CMP #$8B; (>= -> A=$8C,C=1) (< -> A=$2824,C=0); SBC #$55 */
        uint8_t v = mem[0x2824], carry;
        if (v >= 0x8B) { a = 0x8C; carry = 1; }
        else           { a = v;    carry = 0; }
        a = (uint8_t)((int)a - 0x55 - (1 - carry));
        if (a & 0x80) a = 0x00;              /* BPL skip; else LDA #0 */
        a >>= 2;                             /* LSR; LSR */
        if (y == 0x00) {                     /* CPY #1; BMI L_42e9 (Y==0) */
            y = a; x = mem[0x456C + y]; a = 0x54;
        } else {
            a >>= 1;                         /* LSR */
            y = a; x = mem[0x457A + y]; a = 0x5E;
        }
    }
    /* L_42ef */
    mem[0x2825] = x;
    mem[0x00BF] = a;
    x = mem[0x2835];
    if (x != 0x00) {                         /* clear strip */
        y = mem[0x286F];
        if (y < 0x14) y = 0x14;
        do {
            mem[0x0F1E + y] = 0x00;
            if (y < 0x4F) mem[0x0F71 + y] = 0x00;
            y++;
            x--;
        } while (x != 0x00);
    }
    /* L_4312: PLA */
    a = entryA;
    if (a & 0x80) { mem[0x006A]--; return; } /* BPL L_4318; else DEC $006A */
    if (a >= 0x20) return;                    /* CMP #$20; BCS return */
    if (a >= 0x15) {
        y = 0x49;
    } else if (a >= 0x0C) {
        y = 0x48;
    } else {
        uint8_t chainval;
        if (a >= 9)      chainval = 7;
        else if (a >= 7) chainval = 6;
        else if (a >= 5) chainval = 5;
        else             chainval = a;       /* a in 0..4 */
        y = (uint8_t)(mem[0x2822] + 9 * chainval);
    }
    /* L_4352 */
    a = mem[0x2821];
    if (a < 0x07) a = 0x07;
    if (a >= 0x71) a = 0x71;
    if (y < 0x2D) a = (uint8_t)(a - mem[0x4D11 + y]);   /* CPY #$2D; BCS skip; SEC; SBC */
    mem[0x286F] = a;
    x = a;                                   /* TAX */
    y = (uint8_t)(y << 1);                   /* TYA; ASL; TAY */
    mem[0x00BB] = mem[0x4D3E + y];
    mem[0x00BC] = mem[0x4D3F + y];
    y = 0x00;
    for (;;) {                               /* L_437c */
        uint8_t aa = (mem[0x2826] != 0) ? mem[0x2826] : bus_read(0xD20A);
        uint16_t bb = (uint16_t)(mem[0x00BB] | (mem[0x00BC] << 8));
        aa &= bus_read((uint16_t)(bb + y));
        if (aa == 0x00) break;               /* BEQ L_43a5 */
        if (x >= 0x14) {
            if (x < 0x73) mem[0x0F1E + x] = aa;
            y++;                             /* INY (only when x>=$14) */
            if (x >= 0x2E && x < 0x4F) {
                aa &= mem[0x2825];
                mem[0x0F71 + x] = aa;
            }
        }
        x++;                                 /* INX */
        if (x & 0x80) break;                 /* BPL L_437c (loop while x<$80) */
    }
    /* L_43a5 */
    mem[0x2835] = y;
    {
        uint8_t s = (uint8_t)(mem[0x2823] + mem[0x2824]);   /* CLC; ADC */
        mem[0x2870] = s;
        bus_write(0xD003, s);
        if (s >= 0x8E) s = 0x8D;             /* CMP #$8E; BCC L_43be; LDA #$8D */
        else if (s < mem[0x00BF]) s = mem[0x00BF];   /* CMP $BF; BCS skip; LDA $BF */
        mem[0x00CC] = s;
    }
}

/* dl_lms_build @ $69E5 — set the DL-fill dest pointer ($C5/$C6=$300A) and end index
 * ($0086=$56), then tail dl_lms_fill. */
void dl_lms_build(void) {
    mem[0x00C5] = 0x0A; mem[0x00C6] = 0x30;
    mem[0x0086] = 0x56;
    dl_lms_fill();
}

/* game_init_76CB @ $76CB — build the flight-mode display list and LMS tables in $30xx-$32xx.
 * Copies header bytes from $77C3/$77C9/$77D2 into the DL mirrors, fills mode lines/blanks,
 * pokes the fixed LMS/scroll constants, then builds two row-address tables (stride $60 from
 * $1070 / $10A0) and emits them into the DL via dl_lms_fill.  All-native callees, all writes
 * land in safe RAM. */
void game_init_76CB(void) {
    uint8_t a, y;
    for (y = 0x08; ; y--) {                      /* L_76cd */
        a = mem[0x77C9 + y];
        mem[0x3000 + y] = a; mem[0x3120 + y] = a; mem[0x316B + y] = a; mem[0x3210 + y] = a;
        a = mem[0x77C3 + y];
        mem[0x310B + y] = a; mem[0x3156 + y] = a;
        if (y == 0) break;
    }
    for (y = 0x0E; ; y--) {                      /* L_76ea */
        a = mem[0x77D2 + y];
        mem[0x3111 + y] = a; mem[0x315C + y] = a; mem[0x3201 + y] = a; mem[0x32A6 + y] = a;
        if (y == 0) break;
    }
    y = 0x87;                                    /* L_7700 */
    for (;;) {
        mem[0x3174 + y] = 0x4D; mem[0x3219 + y] = 0x4D;
        y--;
        if (y == 0xFF) break;
    }
    mem[0x3129] = 0x4D;
    mem[0x312A] = 0x00;
    mem[0x312B] = 0x10;
    y = 0x7E;                                    /* L_771c (step -3) */
    for (;;) {
        mem[0x3009 + y] = 0x4F; mem[0x308A + y] = 0x4F;
        y = (uint8_t)(y - 3);
        if (y & 0x80) break;
    }
    for (y = 0x28; ; y--) {                      /* L_772b */
        mem[0x312C + y] = 0x0D;
        if (y == 0) break;
    }
    mem[0x306F] = 0xCF; mem[0x30ED] = 0xCF; mem[0x30F9] = 0xCF; mem[0x3105] = 0xCF;
    mem[0x3008] = 0x8D; mem[0x3128] = 0x8D; mem[0x313C] = 0x8D; mem[0x3151] = 0x8D; mem[0x3155] = 0x8D;
    mem[0x3007] = 0x84;
    mem[0x31F2] = 0xCD; mem[0x31FE] = 0xCD; mem[0x3297] = 0xCD; mem[0x32A3] = 0xCD;
    mem[0x311E] = 0x00; mem[0x311F] = 0x30;
    mem[0x3169] = 0x20; mem[0x316A] = 0x31;
    mem[0x320E] = 0x6B; mem[0x320F] = 0x31;
    mem[0x32B3] = 0x10; mem[0x32B4] = 0x32;
    mem[0x00C1] = 0x60;
    mem[0x00C3] = 0x70; mem[0x00C4] = 0x10;      /* base $1070 */
    build_row_addr_table();
    mem[0x00C5] = 0x75; mem[0x00C6] = 0x31;      /* dest $3175 */
    mem[0x008B] = 0x00; mem[0x0086] = 0x2F;
    dl_lms_fill();
    mem[0x00C3] = 0xA0; mem[0x00C4] = 0x10;      /* base $10A0 */
    build_row_addr_table();
    mem[0x00C5] = 0x1A; mem[0x00C6] = 0x32;      /* dest $321A */
    dl_lms_fill();
}

/* setup_initials_ptr @ $5A63 — point the digit dest at $3694, render $006D as zero-suppressed
 * BCD ($3694=0 clears the running suppress flag), via bin_to_bcd + render_bcd_digits_supp_all. */
void setup_initials_ptr(void) {
    mem[0x00C5] = 0x94; mem[0x00C6] = 0x36;
    cpu.A = mem[0x006D];
    bin_to_bcd();
    cpu.Y = 0x00;
    mem[0x3694] = 0x00;
    render_bcd_digits_supp_all();
}

/* startup_init @ $3FFA — refresh the level/score HUD digits when their source values
 * change.  When $0642 is 1 or 2 and ($0642 & $004B)==0, optionally push event $14 (if
 * $0072!=0) and select the $9E vs $1E indicator base.  Then: redraw the level digit
 * ($0647, into $33B4) if changed; redraw the 2-digit field ($0641, into $3413/$3445) if
 * changed; and redraw the score field ($0628 | $00BF, into $3472/$34A4) if changed —
 * $00BF gets bit7 when $062B!=0 and ($0C & $004B)==0.
 *
 * The PHA/ring_push_marked/PLA bracket is reproduced with the real op-macros + native
 * ring_push_marked so the CPU stack page matches the oracle exactly (ring_push's internal
 * PLA reads the byte this PHA pushed — eliminating the PHA would diverge mem[$01FF] and the
 * pulled index).  Digit dests are fixed safe screen RAM.  Contract: mem[]. */
void startup_init(void) {
    mem[0x00BF] = 0x00;
    cpu.Y = 0x1E;
    LDA(mem[0x0642]);
    uint8_t v = cpu.A;
    int skip = (v < 0x01) || (v >= 0x03) || ((v & mem[0x004B]) != 0);
    if (!skip) {
        PHA();
        if (mem[0x0072] != 0x00) {            /* LDA $0072; CMP #0; BEQ skips ring_push */
            cpu.X = 0x14;
            ring_push_marked();
        }
        PLA();
        cpu.Y = 0x9E;
    }
    /* L_401e */
    mem[0x33DF] = cpu.Y;
    cpu.Y = (uint8_t)(cpu.Y - 1);             /* DEY */
    mem[0x33E0] = cpu.Y;
    if (cpu.A != mem[0x0647]) {                /* CMP $0647; BEQ L_4038 */
        mem[0x00BB] = 0xB4; mem[0x00BC] = 0x33;
        mem[0x0647] = cpu.A;
        draw_digit_low_nibble();
    }
    /* L_4038 */
    LDA(mem[0x0641]);
    if (cpu.A != mem[0x0645]) {                /* CMP $0645; BEQ L_4056 */
        mem[0x0645] = cpu.A;
        mem[0x00BD] = 0x45; mem[0x00BE] = 0x34;
        mem[0x00BB] = 0x13; mem[0x00BC] = 0x34;
        draw_2digit_value();
    }
    /* L_4056 */
    if (mem[0x062B] != 0x00) {
        if ((0x0C & mem[0x004B]) == 0) mem[0x00BF] = 0x80;   /* LDA #$0C; BIT $004B; BNE skips */
    }
    /* L_4065 */
    LDA(mem[0x0628]);
    cpu.Y = cpu.A;                             /* TAY */
    ORA(mem[0x00BF]);
    if (cpu.A == mem[0x0646]) return;          /* CMP $0646; BEQ return_stub_40af */
    mem[0x0646] = cpu.A;
    cpu.A = cpu.Y;                             /* TYA (the raw $0628 value) */
    mem[0x00BD] = 0xA4; mem[0x00BE] = 0x34;
    mem[0x00BB] = 0x72; mem[0x00BC] = 0x34;
    draw_2digit_value();
}

/* dl_index_dec @ $69E3 — DEC $008B, then tail dl_lms_build (rebuild the DL from the
 * decremented index). */
void dl_index_dec(void) {
    mem[0x008B] = (uint8_t)(mem[0x008B] - 1);
    dl_lms_build();
}

/* dl_index_dec_or_reset @ $69DD — reset $008B=0, then dl_lms_build.  The 6502 does LDA #0
 * then BEQ, so the branch is unconditional (the dl_index_dec tail is dead). */
void dl_index_dec_or_reset(void) {
    mem[0x008B] = 0x00;
    dl_lms_build();
}

/* draw_scaled_shape @ $7C9A — scale and blit a 2-bit shape into the HUD via the clipped
 * plotter.  First a divide-by-repeated-subtraction: $C2:$C1=$0600, subtract the step
 * $0051:$0050 until it borrows, decrementing $004F each pass; the leftover $004F (->$28DE)
 * is the per-row inner count reload.  Then 14 blank plots, and the nested row(outer,$0055
 * accum to $12)/col(inner,$0053 accum to $0C) loop: each cell derives a mask-byte offset via
 * the $7DA9/$7DBB/$7DD3 tables + the ($C3) mask pointer, extracts a 2-bit field (X from
 * $7DD3 selects how many >>2 steps — DEX/BMI: 0 for X==0 or X>=$81, else min(X,3)), maps it
 * through $7DA5 and plots it.  plot_clipped_pixel is native (clips OOB).  HW-free except the
 * mask read through ($C3) routed via bus_read.  Step must be nonzero or the loops never end. */
void draw_scaled_shape(void) {
    mem[0x00C2] = 0x06;
    mem[0x00C1] = 0x00;
    {
        uint8_t carry = 1;                              /* SEC */
        do {
            int r = (int)mem[0x00C1] - mem[0x0050] - (1 - carry);
            mem[0x00C1] = (uint8_t)r; carry = (r >= 0);
            r = (int)mem[0x00C2] - mem[0x0051] - (1 - carry);
            mem[0x00C2] = (uint8_t)r; carry = (r >= 0);
            mem[0x004F] = (uint8_t)(mem[0x004F] - 1);   /* DEC (carry unaffected) */
        } while (carry);
    }
    mem[0x28DE] = mem[0x004F];

    mem[0x0054] = 0x00; mem[0x0055] = 0x00;
    mem[0x0053] = 0x0E;
    do {                                                /* L_7cc2: 14 blank plots */
        cpu.A = 0x00; plot_clipped_pixel();
        mem[0x0053] = (uint8_t)(mem[0x0053] - 1);
    } while (mem[0x0053] != 0x00);
    mem[0x004E] = (uint8_t)(mem[0x004E] - 1);

    do {                                                /* L_7ccd (rows) */
        mem[0x004F] = mem[0x28DE];
        mem[0x0052] = 0x00; mem[0x0053] = 0x00;
        cpu.A = 0x00; plot_clipped_pixel();
        do {                                            /* L_7cdb (cols) */
            uint8_t y = mem[0x0055];
            unsigned t = (unsigned)mem[0x0053] + mem[0x28DF];   /* CLC; ADC $28DF */
            uint8_t x = (uint8_t)t;
            unsigned t2 = (unsigned)mem[0x7DA9 + y] + mem[0x7DBB + x] + (t > 0xFF ? 1 : 0);
            y = (uint8_t)t2;
            x = mem[0x7DD3 + x];
            uint16_t c3 = (uint16_t)(mem[0x00C3] | (mem[0x00C4] << 8));
            uint8_t a = bus_read((uint16_t)(c3 + y));
            for (int i = 0; i < 3; i++) {               /* DEX; BMI; LSR;LSR */
                x = (uint8_t)(x - 1);
                if (x & 0x80) break;
                a = (uint8_t)(a >> 2);
            }
            a &= 0x03;
            cpu.A = mem[0x7DA5 + a];
            plot_clipped_pixel();
            unsigned s = (unsigned)mem[0x0052] + mem[0x0050];
            mem[0x0052] = (uint8_t)s;
            mem[0x0053] = (uint8_t)((unsigned)mem[0x0053] + mem[0x0051] + (s > 0xFF ? 1 : 0));
        } while (mem[0x0053] < 0x0C);
        cpu.A = 0x00; plot_clipped_pixel();
        cpu.A = 0x00; plot_clipped_pixel();
        mem[0x004E] = (uint8_t)(mem[0x004E] - 1);
        unsigned u = (unsigned)mem[0x0054] + mem[0x0050];
        mem[0x0054] = (uint8_t)u;
        mem[0x0055] = (uint8_t)((unsigned)mem[0x0055] + mem[0x0051] + (u > 0xFF ? 1 : 0));
    } while (mem[0x0055] < 0x12);
}

/* pack_byte_to_5bit_cells @ $8181 — interleave the bits of A with the running cell byte
 * $0084 via a ROL/ROR carry chain, returning the packed result in A.  Faithfully reproduced
 * with the op-macros (the carry threads through every rotate, so an idiomatic rewrite would
 * be no faster and far more error-prone). */
void pack_byte_to_5bit_cells(void) {
    ROL_A(); ROL_A();
    ROR_M(0x0084); ROR_A(); ROR_M(0x0084);
    ROL_A(); ROL_A(); ROL_A();
    ROR_M(0x0084); ROR_A(); ROR_M(0x0084);
    ROL_A(); ROL_A(); ROL_A();
    ROR_M(0x0084); ROR_A(); ROR_M(0x0084);
    ROL_A(); ROL_A(); ROL_A();
    ROR_M(0x0084); ROR_A();
    LDA(mem[0x0084]); ROR_A();
}

/* read_console_trig_delta @ $5A78 — A = (CONSOL & 1) - TRIG0, reading the two active-low HW
 * inputs through bus_read ($D01F START bit, $D010 trigger 0). */
void read_console_trig_delta(void) {
    LDA(bus_read(0xD01F));
    AND(0x01);
    SEC();
    SBC(bus_read(0xD010));
}

/* validate_save_state @ $5D0D — verify the saved-state header: $3700==$28, $3714==$EE, and
 * the 38 bytes $37C7+$26..+1 match the reference $7BDA+$26..+1.  Sets the Z flag (valid =>
 * Z set from the final DEY; any mismatch => Z clear from the failing CMP).  No memory writes. */
void validate_save_state(void) {
    LDA(mem[0x3700]); CMP(0x28);
    if (!cpu.Z) return;
    LDA(mem[0x3714]); CMP(0xEE);
    if (!cpu.Z) return;
    cpu.Y = 0x26;
    for (;;) {
        LDA(mem[0x7BDA + cpu.Y]); CMP(mem[0x37C7 + cpu.Y]);
        if (!cpu.Z) return;
        DEY();
        if (cpu.Z) return;          /* BNE: loop while Y!=0 */
    }
}

/* cockpit_dial_update @ $4430 — set the dial source $006F=A, derive the bar value $0022
 * (0 when A==8, else $4457[A + $0625]) and draw it via the native draw_cockpit_dial_bar.
 * The PHA/PLA preserving the entry A across the derivation is reproduced with the op-macros
 * (its $01FF byte is masked in the test). */
void cockpit_dial_update(void) {
    mem[0x006F] = cpu.A;
    PHA();
    CMP(0x08);
    if (cpu.Z) {
        LDA(0x00);
    } else {
        CLC(); ADC(mem[0x0625]); cpu.Y = cpu.A;
        LDA(mem[0x4457 + cpu.Y]);
    }
    mem[0x0022] = cpu.A;
    PLA();
    draw_cockpit_dial_bar();
}

/* hud_fill_field0 @ $8105 — if $0080 has reached $2927 just bump it; otherwise pack 5 source
 * bytes (($85)+Y) through pack_byte_to_5bit_cells into the cell bytes $93..$8F (X=4..0) and
 * advance $0080 by 5. */
void hud_fill_field0(void) {
    uint8_t y = mem[0x0080];
    if (y >= mem[0x2927]) { mem[0x0080] = (uint8_t)(y + 1); return; }
    for (uint8_t x = 0x04; ; x--) {
        cpu.Y = y;
        cpu.A = bus_read(ZP_IND_Y(0x85));
        pack_byte_to_5bit_cells();
        mem[(uint8_t)(0x8F + x)] = cpu.A;
        y = (uint8_t)(y + 1);
        if (x == 0x00) break;                 /* DEX; BPL */
    }
    mem[0x0080] = y;
}

/* hud_fill_field2 @ $8138 — if $0082 reached $2929 just bump it; else fill $8F-$9A from
 * ($89)+Y.  When $292D==0 do a plain ascending copy (X=5..$0B); otherwise pack each byte via
 * pack_byte_to_5bit_cells descending (X=$0B..5).  Advance $0082 by the 7 bytes consumed. */
void hud_fill_field2(void) {
    uint8_t y = mem[0x0082];
    if (y >= mem[0x2929]) { mem[0x0082] = (uint8_t)(y + 1); return; }
    if (mem[0x292D] == 0x00) {
        for (uint8_t x = 0x05; x < 0x0C; x++) {       /* INX; CPX #$0C; BCC */
            cpu.Y = y;
            mem[(uint8_t)(0x8F + x)] = bus_read(ZP_IND_Y(0x89));
            y = (uint8_t)(y + 1);
        }
    } else {
        for (uint8_t x = 0x0B; ; x--) {               /* DEX; CPX #5; BCS */
            cpu.Y = y;
            cpu.A = bus_read(ZP_IND_Y(0x89));
            pack_byte_to_5bit_cells();
            mem[(uint8_t)(0x8F + x)] = cpu.A;
            y = (uint8_t)(y + 1);
            if (x == 0x05) break;
        }
    }
    mem[0x0082] = y;
}

/* hud_build_text_row @ $80C5 — assemble one HUD text row.  Clear the 17 cell bytes
 * $008F-$009F, fill them from the four fields (hud_fill_field0..3), then for each cell map
 * it through the $BE00 PMG bit table, AND with the mask row (($8B)+Y) and OR the raw cell,
 * writing the result via the dest row (($8D)+Y).  Finally advance the mask pointer $8B/$8C
 * by $60 and set the dest pointer $8D/$8E = $8B/$8C + $30. */
void hud_build_text_row(void) {
    for (uint8_t x = 0x10; ; x--) {              /* clear $8F..$9F */
        mem[(uint8_t)(0x8F + x)] = 0x00;
        if (x == 0x00) break;
    }
    hud_fill_field0();
    hud_fill_field1();
    hud_fill_field2();
    hud_fill_field3_font();
    for (uint8_t y = 0x10; ; y--) {
        cpu.Y = y;
        uint8_t cell = mem[0x008F + y];
        mem[0x0084] = cell;
        cpu.A = mem[0xBE00 + cell];              /* TAX; LDA $BE00,X */
        cpu.A &= bus_read(ZP_IND_Y(0x8B));       /* AND ($8B)+Y */
        cpu.A |= mem[0x0084];                    /* ORA $0084 */
        bus_write(ZP_IND_Y(0x8D), cpu.A);        /* STA ($8D)+Y */
        if (y == 0x00) break;
    }
    /* advance $8B/$8C += $60; $8D/$8E = $8B/$8C + $30 */
    unsigned s = (unsigned)mem[0x008B] + 0x60;
    mem[0x008B] = (uint8_t)s;
    if (s > 0xFF) mem[0x008C] = (uint8_t)(mem[0x008C] + 1);
    unsigned s2 = (unsigned)mem[0x008B] + 0x30;
    mem[0x008D] = (uint8_t)s2;
    mem[0x008E] = (uint8_t)(mem[0x008C] + (s2 > 0xFF ? 1 : 0));
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
    platform_title_changed();   /* hook: score digits rendered into $32C5-$32CA -> flag title dirty */
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
    /* _ad is a terrain-silhouette bitmap address: {$28CA[ai]:$28FA[ai]} (a row-addr
       table pointing into the ~$1010 bitmap) + $BD00[X].  It is ALWAYS plain RAM —
       never the HW range ($D000-$D7FF) nor a page-2 shadow ($0200-$02FF) — so the
       bus_read/bus_write routing (4 dead range-check branches per voxel, in the
       hottest loop in the game) is replaced with a direct mem[] read-modify-write.
       Safe because terrain_column_rasterize is validated from the REAL flight
       snapshot (test_from_snapshot), where the row tables are real -> _ad in-bitmap;
       the __t6502 oracle's bus_*() reduce to the same mem[] access for _ad<$D000. */
    #define PLOT()   do { mem[0x00B5]=Y; uint8_t _ai=A; \
        mem[0x0080]=mem[0x28CA+_ai]; mem[0x0081]=mem[0x28FA+_ai]; \
        uint8_t _bo=mem[0xBD00+X]; \
        uint16_t _ad=(uint16_t)(mem[0x0080]|(mem[0x0081]<<8))+_bo; \
        mem[_ad]=(uint8_t)(mem[_ad]|mem[0xBC00+X]); \
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
/* Optional sub-phase probe (Amiga autoflight only; -DROF_TDRAW_PROF): split the
 * terrain_draw_frame object loop's cost into the fractal subdivision vs the
 * projection+object-plot, via the raster beam line.  Desktop builds compile these
 * to no-ops (no HW beam there).  Read g_tdSubdiv / g_tdProjPlot from the debugger. */
#ifdef ROF_TDRAW_PROF
extern unsigned short rof_beam_line(void);
extern unsigned long g_tdSubdiv, g_tdProjPlot;
#define PB(v) unsigned short v = rof_beam_line()
#define PE(v,acc) do { unsigned short _e = rof_beam_line(); \
    (acc) += (_e >= (v)) ? (unsigned long)(_e - (v)) : (unsigned long)(_e + 313 - (v)); } while (0)
#else
#define PB(v) ((void)0)
#define PE(v,acc) ((void)0)
#endif

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
                    PB(_pp1); cpu.X = X; project_terrain_points(); cpu.X = X; terrain_plot_object(); PE(_pp1, g_tdProjPlot);  /* a3d4-a3d7 */
                }
                /* a3da */
                mem[0x25B4]=mem[0x2400+X]; mem[0x25D2]=mem[0x242D+X]; mem[0x25F0]=mem[0x245A+X];  /* a3da-a3ec */
                mem[0x24E2]=mem[0x2487+X]; mem[0x23E2]=mem[0x23B5+X];  /* a3ef-a3f5 */
                X = mem[0x28DB];                         /* a3f8 */
                if (!(mem[0x24B4+X] & 0x10)) {           /* a3fb BNE L_a408 -> else project */
                    PB(_pp2); cpu.X = X; project_terrain_points(); cpu.X = X; terrain_plot_object(); PE(_pp2, g_tdProjPlot);  /* a402-a405 */
                }
                /* a408 */
                mem[0x0082]=mem[0x2400+X]; mem[0x0083]=mem[0x242D+X]; mem[0x0084]=mem[0x245A+X];  /* a408-a415 */
                mem[0x0085]=mem[0x2487+X]; mem[0x0086]=mem[0x23B5+X];  /* a417-a41f */
                PB(_sd); cpu.X = 0x00; terrain_subdivide_column(); PE(_sd, g_tdSubdiv);  /* a421-a423 */
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

/* draw_cockpit_dial_bar @ $4447 — A += 8 (the lit threshold), then draw the dial bar. */
void draw_cockpit_dial_bar(void) {
    cpu.A = (uint8_t)(cpu.A + 0x08);   /* 4447 CLC; 4448 ADC #$08 */
    setup_dial_bar_draw();             /* 444a (native) */
}

/* object_step_and_collide @ $9552 — advance an object's position accumulators by its
 * velocity ($2854-$285B += $285C-$2863, 3-byte chained adds with the 12-bit map coords
 * mirrored to $27FD-$2800 and the altitude $285B clamped on overflow), run the player
 * hit test, then resolve terrain/object collision at the derived cell index $2864:
 * sample the terrain height, and on a real collision explode the occupant and dispatch
 * the pickup type ($64/$80/other -> $0044/$004D/$007E + counters).  Straight-line (no
 * loops).  PHA/PLA at $9641/$964d use the real 6502 stack (cpu.S) because the nested
 * trigger_object_explosion -> ring_push consumes it; the same native chain runs in both
 * the native and oracle paths, so it stays equivalent.  mem-only contract. */
void object_step_and_collide(void) {
    uint8_t A, Y, c;
    #define ADC_(v) do { uint16_t _t=(uint16_t)A+(uint8_t)(v)+c; c=(uint8_t)(_t>>8); A=(uint8_t)_t; } while(0)
    #define ASLA_() do { c=(uint8_t)(A>>7); A=(uint8_t)(A<<1); } while(0)

    c = 0; A = mem[0x2854]; ADC_(mem[0x285C]); mem[0x2854] = A;             /* 9552-9559 */
    A = mem[0x2855]; ADC_(mem[0x285D]); mem[0x2855] = A; mem[0x27FD] = A;   /* 955c-9565 */
    A = mem[0x2856]; ADC_(mem[0x285E]); A &= 0x0F; mem[0x2856] = A; mem[0x27FE] = A; /* 9568-9573 */
    c = 0; A = mem[0x2857]; ADC_(mem[0x285F]); mem[0x2857] = A;             /* 9576-957d */
    A = mem[0x2858]; ADC_(mem[0x2860]); mem[0x2858] = A; mem[0x27FF] = A;   /* 9580-9589 */
    A = mem[0x2859]; ADC_(mem[0x2861]); A &= 0x0F; mem[0x2859] = A; mem[0x2800] = A; /* 958c-9597 */
    c = 0; A = mem[0x285A]; ADC_(mem[0x2862]); mem[0x285A] = A;             /* 959a-95a1 */
    A = mem[0x285B]; ADC_(mem[0x2863]);                                     /* 95a4-95a7 */
    if (c) { if (!(mem[0x2863] & 0x80)) A = 0xFF; }                         /* 95aa-95b1 BMI: $FF on +overflow */
    else   { if (mem[0x2863] & 0x80) A = 0x00; }                            /* 95b6-95bb */
    mem[0x285B] = A;                                                        /* 95bd */
    /* check_player_proximity_hit reads ENTRY CARRY; at $95c4 that carry is the
       overflow out of the $95a7 ADC $2863 (BIT/LDA in the clamp don't touch C). */
    cpu.C = c;
    if (!(mem[0x0063] & 0x80)) check_player_proximity_hit();                /* 95c0-95c4 */

    A = mem[0x2858]; ASLA_();                  /* 95c7-95ca */
    A = mem[0x2859]; ADC_(0x00);               /* 95cb-95ce */
    ASLA_(); ASLA_(); ASLA_(); ASLA_();        /* 95d0-95d3 (x16) */
    mem[0x00BB] = A;                           /* 95d4 */
    A = mem[0x2855]; ASLA_();                  /* 95d6-95d9 */
    A = mem[0x2856]; ADC_(0x00); A &= 0x0F;    /* 95da-95df */
    A |= mem[0x00BB]; mem[0x2864] = A;         /* 95e1-95e3 */
    Y = A;                                     /* 95e6 TAY */

    if (mem[0x0A00 + Y] != 0) {                 /* 95e7-95ea BEQ L_9614 */
        A = mem[0x0900 + Y]; c = 0; ADC_(0x10); if (c) A = 0xFF;   /* 95ec-95f4 */
        if (A >= mem[0x285B]) {                 /* 95f6-95f9 BCC L_9614 */
            A = mem[0x2855];
            if (!(A >= 0x30 && A < 0xD0)) {      /* 95fb-9604 not in X-band -> L_9606 */
                A = mem[0x2858];
                if (!(A >= 0x30 && A < 0xD0)) goto L_9635;   /* 9606-960f / L_9611 */
            }
        }
    }
    /* L_9614 */
    sample_terrain_height_bilerp();             /* 9614 (native) */
    if (mem[0x0062] < mem[0x285B]) return;      /* 9617-961e */
    A = mem[0x2855];
    if (A >= 0x30 && A < 0xD0) { reset_object_slot(); return; }   /* 961f-9628 */
    A = mem[0x2858];
    if (A >= 0x30 && A < 0xD0) { reset_object_slot(); return; }   /* 962a-9633 */
L_9635:
    Y = mem[0x2864];                            /* 9635 */
    A = mem[0x0A00 + Y];                        /* 9638 */
    if (A == 0) { reset_object_slot(); return; }        /* 963b BEQ */
    if (A >= 0xF8) { reset_object_slot(); return; }     /* 963d-963f CMP #$F8; BCS */
    mem[0x0100 | cpu.S] = A; cpu.S--;           /* 9641 PHA */
    mem[0x0A00 + Y] = 0xFC;                      /* 9642-9644 */
    mem[0x2843] = 0xFC;                          /* 9647 */
    trigger_object_explosion();                  /* 964a (native; touches the stack) */
    cpu.S++; A = mem[0x0100 | cpu.S];           /* 964d PLA */
    if (A < 0x64) { reset_object_slot(); return; }      /* 964e-9650 CMP #$64; BCC */
    if (A != 0x64) {                             /* 9652 BNE L_9664 */
        if (A == 0x80) { set_place_params_inc_count(); A = 0x40; }   /* 9664-966d */
        else           { countdown_show_char_0620(); A = 0x49; }     /* 9670-9673 */
        mem[0x0044] = A; reset_object_slot(); return;                /* 9675 */
    }
    set_place_params_inc_count();                /* 9654 (A == $64) */
    mem[0x004D] = 0x28;                          /* 9657-9659 */
    mem[0x007E] = (uint8_t)(mem[0x007E] | 0x80); /* 965b-965f */
    reset_object_slot();                         /* 9661 */
    #undef ADC_
    #undef ASLA_
}

/* reset_indicator_event @ $B786 — clear $0035, then enqueue the indicator event. */
void reset_indicator_event(void) {
    mem[0x0035] = 0x00;            /* b786-b788 */
    enqueue_indicator_event();     /* b78a (native) */
}

/* countdown_show_char_0620 @ $7B3C — when the countdown $0620 is running, place the
 * countdown glyph into a free object slot (value $C9 with 1 retry when $0620==1, else
 * the default $C8 via obj_table_scan_y1_c8), then decrement $0620.  mem-only. */
void countdown_show_char_0620(void) {
    uint8_t Y = mem[0x0620];                          /* 7b3c */
    if (Y == 0) return;                               /* 7b3f BEQ */
    if (Y == 1) { cpu.A = 0xC9; cpu.Y = Y; obj_table_scan_replace(); }  /* 7b41-7b47 */
    else        { obj_table_scan_y1_c8(); }           /* 7b4d (sets Y=1, A=$C8) */
    mem[0x0620] = (uint8_t)(mem[0x0620] - 1);         /* 7b50 DEC $0620 */
}

/* check_object_in_target_box @ $93BD — if the object has settled ($0065==0 && $0067==0)
 * and its screen pos is inside the target box ($0064 in [$34,$AC), $0066 in [$1A,$6A)),
 * trigger the in-box event: set $003B=$20, enqueue the indicator, jitter, and arm the
 * box markers $2892/$3355/$2891.  mem-only. */
void check_object_in_target_box(void) {
    if (mem[0x0065] != 0 || mem[0x0067] != 0) return;          /* 93bd-93c3 */
    uint8_t a = mem[0x0064];
    if (a < 0x34 || a >= 0xAC) return;                         /* 93c5-93cd */
    a = mem[0x0066];
    if (a < 0x1A || a >= 0x6A) return;                         /* 93cf-93d7 */
    mem[0x003B] = 0x20;                                        /* 93d9-93db */
    reset_indicator_event();                                  /* 93dd (native) */
    jitter_roll_pitch();                                      /* 93e0 (native) */
    mem[0x2892] = 0x00;                                       /* 93e3-93e5 */
    mem[0x3355] = 0x34;                                       /* 93e8-93ea */
    mem[0x2891] = 0x1E;                                       /* 93ed-93ef */
}

/* check_player_proximity_hit @ $9680 — test whether object slot $0036 is within hit
 * range of the player: |($006A+4)-$0036| < 4, then a Manhattan-ish distance from
 * ($0038,$0039) vs ($2824,$2821) (folded via >>1>>1) compared to the per-shape
 * threshold $96F5[$0036].  On a hit: clear $2826 and fire the pickup/explosion chain.
 * Reads ENTRY CARRY ($9682 ADC #$04).  mem-only contract. */
void check_player_proximity_hit(void) {
    uint8_t A, X, c;
    #define ADC_(v) do { uint16_t _t=(uint16_t)A+(uint8_t)(v)+c; c=(uint8_t)(_t>>8); A=(uint8_t)_t; } while(0)
    #define SBC_(v) ADC_((uint8_t)~(uint8_t)(v))
    #define RORA_() do { uint8_t _n=A&1; A=(uint8_t)((A>>1)|(c<<7)); c=_n; } while(0)
    #define LSRA_() do { c=A&1; A=(uint8_t)(A>>1); } while(0)

    c = cpu.C;                                  /* ENTRY CARRY */
    A = mem[0x006A]; ADC_(0x04);                /* 9680-9682 */
    c = 1; SBC_(mem[0x0036]);                   /* 9684-9685 SEC; SBC $0036 */
    if (!c) { A ^= 0xFF; ADC_(0x01); }          /* 9687 BCS; 9689-968b abs */
    if (A >= 0x04) return;                      /* 968d CMP #$04; 968f BCS L_96d8 */
    c = 1; A = mem[0x0038]; SBC_(0x0C); SBC_(mem[0x2824]);   /* 9691-9696 SEC;SBC #$0C;SBC $2824 */
    if (!c) { A ^= 0xFF; ADC_(0x01); }          /* 9699-969d abs */
    mem[0x00BB] = A;                            /* 969f */
    c = 0; A = mem[0x0039]; ADC_(0x42);         /* 96a1-96a4 CLC;LDA $0039;ADC #$42 */
    c = 1; SBC_(mem[0x2821]);                   /* 96a6-96a7 SEC; SBC $2821 */
    if (!c) { A ^= 0xFF; ADC_(0x01); }          /* 96aa-96ae abs */
    c = 0; ADC_(mem[0x00BB]);                   /* 96b0-96b1 CLC; ADC $00BB */
    RORA_(); LSRA_();                           /* 96b3 ROR A; 96b4 LSR A */
    X = mem[0x0036];                            /* 96b5 */
    if (A >= mem[0x96F5 + X]) return;           /* 96b7 CMP $96F5,X; 96ba BCS L_96d8 */
    /* HIT */
    mem[0x2826] = 0x00;                         /* 96bc-96be */
    reset_indicator_event();                    /* 96c1 (native) */
    reset_object_slot();                        /* 96c4 (native) */
    trigger_object_explosion();                 /* 96c7 (native) */
    mem[0x0045] = 0x50;                         /* 96ca-96cc */
    mem[0x0046] = 0x02;                         /* 96ce-96d0 */
    bcd_inc_counter_0641();                     /* 96d2 (native) */
    terrain_jitter_column();                    /* 96d5 (native) */
    #undef ADC_
    #undef SBC_
    #undef RORA_
    #undef LSRA_
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

/* flight_control_integrate @ $8E5B — THE flight VBI root (the last transpiled fn on the
 * per-frame VBI path).  Reads the joystick (PORTA $D300) + throttle, integrates the
 * ship's pitch/roll/heading and 24-bit world position, clamps the angles, drives the
 * HUD/audio refresh + the per-object scratch ring rotation, and steps the active object
 * (load_velocity_from_param_block / object_step_and_collide).  Bounded loops only; reads
 * RANDOM $D20A.  Ported goto-faithfully (huge maze).  mem-only contract (reads no entry
 * regs/carry at $8e5b).  Native-call cpu setup: game_sub_55FC needs cpu.Y; draw_cockpit_dial_bar
 * / store_676_init need cpu.A; compute_obj_rel_angle_scale reads ENTRY CARRY ($90f8). */
void flight_control_integrate(void) {
    uint8_t A, X, Y, c = 0, n, v;
    #define ADC_(x) do { uint16_t _t=(uint16_t)A+(uint8_t)(x)+c; c=(uint8_t)(_t>>8); A=(uint8_t)_t; } while(0)
    #define SBC_(x) ADC_((uint8_t)~(uint8_t)(x))
    #define ASLA_() do { c=(uint8_t)(A>>7); A=(uint8_t)(A<<1); } while(0)
    #define ROLA_() do { n=(uint8_t)(A>>7); A=(uint8_t)((A<<1)|c); c=n; } while(0)
    #define RORA_() do { n=A&1; A=(uint8_t)((A>>1)|(c<<7)); c=n; } while(0)
    #define LSRA_() do { c=A&1; A=(uint8_t)(A>>1); } while(0)
    #define ROLM_(a) do { v=mem[a]; n=(uint8_t)(v>>7); mem[a]=(uint8_t)((v<<1)|c); c=n; } while(0)
    #define RORM_(a) do { v=mem[a]; n=v&1; mem[a]=(uint8_t)((v>>1)|(c<<7)); c=n; } while(0)
    #define ASLM_(a) do { v=mem[a]; c=(uint8_t)(v>>7); mem[a]=(uint8_t)(v<<1); } while(0)

    if (mem[0x004A] != 0x02) goto L_8ec5;          /* 8e5b-8e5f */
    if (mem[0x003E] != 0)    goto L_8ec5;          /* 8e61-8e63 */
    A = mem[0x0022]; LSRA_(); LSRA_(); ADC_(0x20); mem[0x0021] = A;  /* 8e68-8e6e */
    X = bus_read(0xD300);                          /* 8e70-8e73 */
    if ((X & 0x04) == 0) { mem[0x0021] ^= 0xFF; }  /* 8e74-8e7c */
    else if (X & 0x08)   { mem[0x0021] = 0x00; }   /* 8e81-8e88 */
    /* L_8e8a */
    if (mem[0x005D] != 0) { A = 0xD0; }            /* 8e8a-8e90 */
    else if (X & 0x01) {                           /* 8e93-8e96 */
        A = 0xD0;                                  /* 8e98 */
        if (mem[0x0029] == 0xF4 && mem[0x0028] == 0) A = 0xFF;   /* 8e9a-8ea4 */
    } else if (X & 0x02) {                         /* 8ea9-8eac */
        A = 0x30;                                  /* 8eae */
        if (mem[0x0029] == 0x0B && mem[0x0028] == 0xFF) A = 0x01; /* 8eb0-8ebc */
    } else { A = 0x00; }                           /* 8ec1 */
    mem[0x0027] = A;                               /* 8ec3 */
L_8ec5:
    if (mem[0x0004] != 0) compute_target_blip_position();   /* 8ec5-8ec9 */
    if (mem[0x0072] == 0x02) {                     /* 8ecc-8ed0 */
        mem[0x0027] = 0x30;                        /* 8ed2-8ed4 */
        mem[0x0021] = mem[0x0026];                 /* 8ed6-8ed8 */
        A = mem[0x0025]; ASLA_(); ROLM_(0x0021); ASLA_(); ROLM_(0x0021);   /* 8eda-8ee0 */
        c = 1; A = 0x00; SBC_(mem[0x0021]); mem[0x0021] = A;   /* 8ee2-8ee7 */
        mem[0x0022] = 0xF0;                        /* 8ee9-8eeb */
        cpu.A = 0x00; draw_cockpit_dial_bar();             /* 8eed-8eef */
        goto L_8f49;                               /* 8ef2 */
    }
    /* L_8ef5 */
    if (mem[0x003D] != 0) goto L_8f2b;             /* 8ef5-8ef7 */
    if (mem[0x0072] == 0) { step_object_along_axes(); goto L_8f28; }   /* 8ef9-8f00 */
    if (mem[0x0676] != 0x01) {                     /* 8f03-8f08 */
        mem[0x066C] = 0x00; mem[0x066D] = 0x00;    /* 8f0a-8f0f */
        cpu.Y = 0x01; game_sub_55FC();             /* 8f12-8f14 */
        cpu.Y++;       game_sub_55FC();            /* 8f17-8f18 */
        cpu.A = 0x01; store_676_init();            /* 8f1b-8f1d */
    }
    /* L_8f20 */
    mem[0x0023] = mem[0x0025];                     /* 8f20-8f22 */
    mem[0x0024] = mem[0x0026];                     /* 8f24-8f26 */
L_8f28:
    goto L_8f49;
L_8f2b:
    if (mem[0x283C] != 0) goto L_8f49;             /* 8f2b-8f2e */
    if (mem[0x066C] == 0x01) goto L_8f49;          /* 8f30-8f35 */
    mem[0x066C] = 0x01; mem[0x066D] = 0x01;        /* 8f37-8f3a */
    cpu.Y = 0x01; game_sub_55FC();                 /* 8f3d */
    cpu.Y++;       game_sub_55FC();                /* 8f40-8f41 */
    mem[0x3355] = 0x34;                            /* 8f44-8f46 */
L_8f49:
    if (mem[0x003E] != 0) {                        /* 8f49-8f4b */
        if (mem[0x066C] != 0x00) {                 /* 8f4d-8f52 */
            mem[0x066C] = 0x00; mem[0x066D] = 0x00; /* 8f54-8f57 */
            cpu.Y = 0x01; game_sub_55FC();         /* 8f5a-8f5c */
            cpu.Y++;       game_sub_55FC();        /* 8f5f-8f60 */
            mem[0x3355] = 0xB4;                    /* 8f63-8f65 */
        }
        return;                                    /* 8f68 */
    }
    /* L_8f69: roll integration */
    if (mem[0x0021] != 0) goto L_8f9a;             /* 8f69-8f6b */
    mem[0x00BB] = mem[0x0026];                     /* 8f6d-8f6f */
    A = mem[0x0025]; Y = 0x05;                     /* 8f71-8f73 */
L_8f75:
    ASLA_(); ROLM_(0x00BB); if (--Y != 0) goto L_8f75;   /* 8f75-8f79 */
    if (!c) goto L_8f8a;                           /* 8f7b */
    A = mem[0x0025]; SBC_(mem[0x00BB]); mem[0x0025] = A;  /* 8f7d-8f81 */
    if (c) mem[0x0026]++;                           /* 8f83-8f85 */
    goto L_8f98;                                   /* 8f87 */
L_8f8a:
    A = mem[0x0025]; SBC_(mem[0x00BB]);            /* 8f8a-8f8c */
    if (!c) {                                      /* 8f8e BCS L_8f98 */
        mem[0x0026] = (uint8_t)(mem[0x0026] - 1);  /* 8f90 DEC */
        if (mem[0x0026] & 0x80) { A = 0x00; mem[0x0026] = 0x00; }   /* 8f92-8f96 */
    }
L_8f98:
    mem[0x0025] = A;                               /* 8f98 */
L_8f9a:
    /* pitch integration */
    if (mem[0x003D] != 0) goto L_8fcd;             /* 8f9a-8f9c */
    if (mem[0x0027] != 0) goto L_8fcd;             /* 8f9e-8fa0 */
    mem[0x00BB] = mem[0x0029];                     /* 8fa2-8fa4 */
    A = mem[0x0028]; ASLA_(); ROLM_(0x00BB); ASLA_(); ROLM_(0x00BB);   /* 8fa6-8fac */
    if (!c) goto L_8fbd;                           /* 8fae */
    A = mem[0x0028]; SBC_(mem[0x00BB]); mem[0x0028] = A;  /* 8fb0-8fb4 */
    if (c) mem[0x0029]++;                           /* 8fb6-8fb8 */
    goto L_8fcd;                                   /* 8fba */
L_8fbd:
    A = mem[0x0028]; SBC_(mem[0x00BB]);            /* 8fbd-8fbf */
    if (!c) {                                      /* 8fc1 BCS L_8fcb */
        mem[0x0029] = (uint8_t)(mem[0x0029] - 1);  /* 8fc3 DEC */
        if (mem[0x0029] & 0x80) { A = 0x00; mem[0x0029] = 0x00; }   /* 8fc5-8fc9 */
    }
    mem[0x0028] = A;                               /* 8fcb */
L_8fcd:
    Y = 0x01;                                      /* 8fcd LDY #$01 */
    if (mem[0x0022] == 0) { Y = 0x03; goto L_8ff7; }   /* 8fcf-8fd5 BNE L_8fd8: run block when $0022!=0 */
    c = 1; A = mem[0x0022]; SBC_(mem[0x0029]); SBC_(mem[0x0029]); mem[0x00BB] = A;   /* 8fd8-8fdd */
    c = 0; A = mem[0x0022]; LSRA_(); A |= 0x07; A &= bus_read(0xD20A); ADC_(mem[0x00BB]);   /* 8fdf-8fe8 */
    if (c) A = 0xFF;                               /* 8fea-8fec */
    c = 0; ADC_(mem[0x002D]); mem[0x002D] = A;     /* 8fee-8ff1 */
    if (c) mem[0x002E]++;                          /* 8ff3-8ff5 */
L_8ff7:
    if (mem[0x005D] == 0) goto L_902d;             /* 8ff7-8ff9 */
    c = 0; A = mem[0x002E]; ADC_(0x02); mem[0x00BB] = A;   /* 8ffb-9000 */
    A = 0x00; ROLA_(); mem[0x00BC] = A;            /* 9002-9005 */
    A = mem[0x002D];                               /* 9007 */
L_9009:
    ASLA_(); ROLM_(0x00BB); ROLM_(0x00BC); if (--Y != 0) goto L_9009;   /* 9009-900f */
    A = mem[0x002D]; SBC_(mem[0x00BB]); mem[0x002D] = A;   /* 9011-9015 */
    A = mem[0x002E]; SBC_(mem[0x00BC]); mem[0x002E] = A;   /* 9017-901b */
    if (c) goto L_902d;                            /* 901d BCS */
    A = 0x00; mem[0x002D] = 0x00; mem[0x002E] = 0x00;   /* 901f-9023 */
    if (A != mem[0x0676]) { cpu.A = A; store_676_init(); }   /* 9025-902a (A=0) */
L_902d:
    c = 0; A = mem[0x0025]; ADC_(mem[0x0021]); mem[0x0025] = A;   /* 902d-9032 */
    if (mem[0x0021] & 0x80) { if (!c) mem[0x0026]--; }   /* 9034-903a BIT;BPL;BCC;DEC */
    else                    { if (c)  mem[0x0026]++; }   /* 903f-9041 */
    /* L_9043: clamp roll $0026 to [$FB,$04] */
    A = mem[0x0026];                               /* 9043 */
    if (!(A & 0x80)) {                             /* 9045 BMI L_9054 */
        if (A >= 0x05) { mem[0x0025] = 0xFF; A = 0x04; }   /* 9047-904f */
    } else {
        if (A < 0xFB) { mem[0x0025] = 0x00; A = 0xFB; }    /* 9054-905c */
    }
    mem[0x0026] = A;                               /* 905e */
    c = 0; A = mem[0x0028]; ADC_(mem[0x0027]); mem[0x0028] = A;   /* 9060-9065 */
    if (mem[0x0027] & 0x80) { if (!c) mem[0x0029]--; }   /* 9067-906d */
    else                    { if (c)  mem[0x0029]++; }   /* 9072-9074 */
    /* L_9076: clamp pitch $0029 to [$F4,$0B] */
    A = mem[0x0029];                               /* 9076 */
    if (!(A & 0x80)) {                             /* 9078 BMI L_9087 */
        if (A >= 0x0C) { mem[0x0028] = 0xFF; A = 0x0B; }   /* 907a-9082 */
    } else {
        if (A < 0xF4) { mem[0x0028] = 0x00; A = 0xF4; }    /* 9087-908f */
    }
    mem[0x0029] = A; mem[0x0020] = A;              /* 9091-9093 */
    A = mem[0x0028]; ASLA_(); ROLM_(0x0020); ASLA_(); ROLM_(0x0020); ASLA_(); ROLM_(0x0020);  /* 9095-909e */
    A = mem[0x0020];                               /* 90a0 */
    if (c) { A ^= 0xFF; ADC_(0x00); }              /* 90a2-90a6 BCC L_90a8; EOR;ADC */
    mem[0x28D6] = A;                               /* 90a8 */
    mem[0x006B] = mem[0x002E];                     /* 90ab-90ad */
    mul_u8(); A = cpu.A;                           /* 90af */
    Y = 0x00;                                      /* 90b2 */
    if (mem[0x0020] & 0x80) {                      /* 90b4-90b6 BIT;BPL L_90c1 */
        Y--;                                       /* 90b8 DEY */
        A ^= 0xFF; c = 0; ADC_(0x01);              /* 90b9-90bc EOR;CLC;ADC #1 */
        if (c) Y++;                                /* 90be-90c0 */
    }
    mem[0x2884] = Y;                               /* 90c1 STY $2884 */
    ASLA_(); ROLM_(0x2884); ASLA_(); ROLM_(0x2884); ASLA_(); ROLM_(0x2884);   /* 90c4-90cd */
    mem[0x2883] = A;                               /* 90d0 */
    mem[0x00BB] = mem[0x0025];                     /* 90d3-90d5 */
    A = mem[0x0026]; Y = 0x04;                     /* 90d7-90d9 */
L_90db:
    c = (A >= 0x80) ? 1 : 0; RORA_(); RORM_(0x00BB); if (--Y != 0) goto L_90db;   /* 90db-90e1 */
    mem[0x00BC] = A;                               /* 90e3 */
    c = 0; A = mem[0x2885]; ADC_(mem[0x00BB]); mem[0x2885] = A;   /* 90e5-90eb */
    A = mem[0x2886]; ADC_(mem[0x00BC]); A &= 0x3F; mem[0x2886] = A;   /* 90ee-90f5 */
    cpu.C = c; compute_obj_rel_angle_scale();      /* 90f8 (reads ENTRY CARRY) */
    c = 0; A = mem[0x2887]; ADC_(mem[0x002B]); mem[0x2887] = A;   /* 90fb-9101 */
    A = mem[0x2888]; ADC_(mem[0x002C]); mem[0x2888] = A;   /* 9104-9109 */
    c = 0; A = mem[0x2889]; ADC_(mem[0x2881]); mem[0x2889] = A;   /* 910c-9115 (910c LDA $002C dead) */
    A = mem[0x288A]; ADC_(mem[0x2882]); mem[0x288A] = A;   /* 9118-911e */
    c = 0; A = mem[0x0033]; ADC_(mem[0x2883]); mem[0x0033] = A;   /* 9121-9127 */
    A = mem[0x0034]; ADC_(mem[0x2884]);            /* 9129-912b */
    if (A == 0xFF) A = 0x00;                        /* 912e-9132 */
    if (A >= 0x50) {                               /* 9134-9136 BCC L_914e */
        Y = 0xFF;                                  /* 9138 */
        if (mem[0x0072] == 0x02) {                 /* 913a-913e */
            if (A >= 0x60) mem[0x283B] = Y;        /* 9140-9144 BCC L_9147 */
        } else { mem[0x0033] = Y; A = 0x4F; }      /* 914a-914c */
    }
    mem[0x0034] = A;                               /* 914e */
    c = mem[0x0028] >> 7; A = mem[0x0029]; ADC_(0x0C); mem[0x2873] = A;   /* 9150-9157 */
    c = mem[0x0023] >> 7; A = mem[0x0024]; ADC_(0x05); mem[0x2871] = A;   /* 915a-9161 */
    c = 1; A = 0x3A;                               /* 9164-9165 */
    if (mem[0x283D] != 0) SBC_(mem[0x0014]);       /* 9167-916a BNE L_9172: SBC $0014 */
    else                  SBC_(mem[0x2886]);       /* 916c SBC $2886 */
    A &= 0x3F; Y = A;                              /* 9174-9176 TAY */
    mem[0x2836] = (uint8_t)(A & 0x03);             /* 9177-9179 */
    A = Y; LSRA_(); LSRA_(); mem[0x281C] = A;      /* 917c-917f */
    A = mem[0x002D]; ASLA_(); A = mem[0x002E]; ROLA_(); A ^= 0xFF; mem[0x0686] = A;   /* 9182-918a */
    c = 1; SBC_(0x04); mem[0x0687] = A;            /* 918d-9190 */
    refresh_hud_field_0d_entry();                  /* 9193 (sets own Y) */
    Y = 0x0C;                                      /* 9196 */
    A = mem[0x0022];                               /* 9198 */
    if (A == 0xF0) goto L_91bc;                    /* 919a-919c (A=$F0) */
    A = mem[0x062F];                               /* 919e */
    if (A != 0) { mem[0x2917] = 0xFF; A = 0xFF; goto L_91bc; }   /* 91a1 BNE L_91b7; 91b7-91b9 */
    mem[0x005D] = A;                               /* 91a3 (A=0) */
    X = mem[0x2917];                               /* 91a5 */
    if (X == 0) goto L_91dc;                       /* 91a8 (A=0) */
    mem[0x2917] = (uint8_t)(mem[0x2917] - 1);      /* 91aa DEC */
    if (X < bus_read(0xD20A)) goto L_91dc;         /* 91ad-91b0 CPX $D20A; BCC (A=0) */
    A = 0xFF;                                      /* 91b2 (fall to L_91bc) */
L_91bc:
    mem[0x005D] = A;                               /* 91bc */
    X = mem[0x0022];                               /* 91be */
    if (X == 0) { A = 0x00; goto L_91dc; }         /* 91c0 BNE L_91c6; 91c2 TXA (A=0) */
    if (X < 0xF0) { A = 0x04; goto L_91dc; }       /* 91c6-91cc CPX #$F0; BCS L_91cf */
    A = mem[0x0034]; LSRA_(); LSRA_(); LSRA_(); A ^= 0x0F;   /* 91cf-91d4 */
    if (A < 0x04) A = 0x04;                        /* 91d6-91da CMP #$04; BCS L_91dc; LDA #$04 */
L_91dc:
    mem[0x066B + Y] = A;                           /* 91dc STA $066B,Y */
    A = mem[0x002D]; ASLA_(); A = mem[0x002E]; ROLA_(); A ^= 0xFF;   /* 91df-91e5 */
    if (A < 0x0C) A = 0x0C;                        /* 91e7-91eb */
    mem[0x0679 + Y] = A;                           /* 91ed STA $0679,Y */
    cpu.Y = Y; game_sub_55FC();                    /* 91f0 */
    mem[0x2850] = mem[0x2919];                     /* 91f3-91f6 */
    A = mem[0x291A]; c = (A >= 0x80) ? 1 : 0; RORA_(); RORM_(0x2850); mem[0x2851] = A;   /* 91f9-9202 */
    Y = 0x00; A = mem[0x291B]; mem[0x2852] = A;    /* 9205-920a */
    if (A & 0x80) Y--;                             /* 920d-920f BPL L_9210; DEY */
    A = Y; Y = 0x03;                               /* 9210-9211 TYA; LDY #$03 */
L_9213:
    ASLM_(0x2852); ROLA_(); if (--Y != 0) goto L_9213;   /* 9213-9218 */
    mem[0x2853] = A;                               /* 921a */
    Y = 0x00; A = mem[0x291A]; c = (A >= 0x80) ? 1 : 0; RORA_();   /* 921d-9224 */
    if (A & 0x80) Y--;                             /* 9225-9227 BPL L_9228; DEY */
    /* L_9228 */
    c = 0; ADC_(mem[0x2829]); mem[0x2829] = A;     /* 9228-922c */
    A = Y; ADC_(mem[0x0068]); mem[0x0068] = A;     /* 922e-9232 TYA; ADC $0068 */
    Y = 0x00; A = mem[0x291B];                     /* 9234-9236 */
    c = (A >= 0x80) ? 1 : 0; RORA_(); c = (A >= 0x80) ? 1 : 0; RORA_(); c = (A >= 0x80) ? 1 : 0; RORA_();   /* 9239-9241 */
    if (A & 0x80) Y--;                             /* 9242-9244 BPL L_9245; DEY */
    /* L_9245 */
    c = 0; ADC_(mem[0x282C]); mem[0x282C] = A;     /* 9245-9249 */
    A = Y; ADC_(mem[0x0069]); mem[0x0069] = A;     /* 924c-924f TYA; ADC $0069 */
    /* 9251: step the active object */
    A = mem[0x0036];                               /* 9251 */
    if (A == 0) goto L_9289;                       /* 9253 BNE L_9258; 9255 goto L_9289 */
    if (!(A & 0x80)) {                             /* 9258 BMI L_9267 */
        if (A == 0x01) load_velocity_from_param_block();   /* 925a-925e */
        else           object_step_and_collide();          /* 9264 */
    }
    /* L_9267 */
    c = 1; A = mem[0x284E]; SBC_(mem[0x2850]); mem[0x284E] = A;   /* 9267-926e */
    A = mem[0x0038]; SBC_(mem[0x2851]); mem[0x0038] = A;   /* 9271-9276 */
    c = 0; A = mem[0x284F]; ADC_(mem[0x2852]); mem[0x284F] = A;   /* 9278-927f */
    A = mem[0x0039]; ADC_(mem[0x2853]); mem[0x0039] = A;   /* 9282-9287 */
L_9289:
    Y = mem[0x0063];                               /* 9289 */
    if (Y & 0x80) goto L_92c7;                      /* 928b BMI L_92c7 */
    Y = (uint8_t)(Y - 1); mem[0x0063] = Y;         /* 928d-928e DEY; STY $0063 */
    if (!(Y & 0x80)) goto L_92a6;                  /* 9290 BPL L_92a6 */
    if (mem[0x2826] != 0) { reset_flags_ff(); goto L_92a3; }   /* 9292-929a BNE L_929d */
    reset_flags_ff(); check_object_in_target_box();            /* 929d-92a0 */
L_92a3:
    goto L_92c7;
L_92a6:
    A = Y; LSRA_(); LSRA_(); mem[0x006A] = A;      /* 92a6-92a9 TYA;LSR;LSR */
    if (Y == 0x5A) goto L_92c1;                    /* 92ab-92ad CPY #$5A; BEQ */
    A = mem[0x006D];                               /* 92af */
    if (A < 0x1F) goto L_92c4;                      /* 92b1-92b3 CMP #$1F; BCC L_92c4 */
    if (Y == 0x3C) goto L_92c1;                     /* 92b5-92b7 CPY #$3C; BEQ */
    if (A < 0x3D) goto L_92c4;                      /* 92b9-92bb CMP #$3D; BCC L_92c4 */
    if (Y != 0x28) goto L_92c4;                     /* 92bd-92bf CPY #$28; BNE L_92c4 */
L_92c1:
    terrain_jitter_column();                        /* 92c1 */
L_92c4:
    object_integrate_position();                    /* 92c4 */
L_92c7:
    Y = mem[0x291E]; Y++; if (Y >= 0x07) Y = 0x00;  /* 92c7-92cf INY; CPY #$07; BCC; LDY #0 */
    mem[0x291E] = Y;                                /* 92d1 */
    mem[0x2919] = mem[0x2893 + Y]; mem[0x2893 + Y] = mem[0x0025];   /* 92d4-92dc */
    mem[0x291A] = mem[0x289A + Y]; mem[0x289A + Y] = mem[0x0026];   /* 92df-92e7 */
    mem[0x291B] = mem[0x28A1 + Y]; mem[0x28A1 + Y] = mem[0x0027];   /* 92ea-92f2 */
    mem[0x291C] = mem[0x28A8 + Y]; mem[0x28A8 + Y] = mem[0x2871];   /* 92f5-92fe */
    mem[0x291D] = mem[0x28AF + Y]; mem[0x28AF + Y] = mem[0x2873];   /* 9301-930a */
    #undef ADC_
    #undef SBC_
    #undef ASLA_
    #undef ROLA_
    #undef RORA_
    #undef LSRA_
    #undef ROLM_
    #undef RORM_
    #undef ASLM_
}

/* ===========================================================================
 * Flight main-loop de-transpile (2026-06-12): game_state_update + enemy_check
 * and their callees — the last transpiled code on the flight per-frame path
 * (flight_frame_native).  Leaves first.
 * ------------------------------------------------------------------------- */

/* plot_scanline_down @ $AAD4 — line-plot loop walking DOWN the screen.
 * Walks the {$28EF:$28F0} fixed-point X position by the step {$5B:$5C} (which is
 * first decremented by $40) and the sub-step {$28F3:$28F4}; for each scanline Y
 * (from $28F2 down to $6C) it draws $28FA pixels across, advancing X by 1 and
 * plotting via terrain_plot_pixel (native) when the column X is in [$2C,$D4).
 * Inputs: $28EF-$28F4 position/step, $5B/$5C step, $58 plot mask (all set by the
 * caller).  Contract: memory (the bitmap + the accumulators); exit cpu dead.
 * plot_line_done $AB26 is a bare RTS -> absorbed as `return`. */
void plot_scanline_down(void) {
    /* aad4 SEC; LDA $5B; SBC #$40; STA $5B; BCC -> DEC $5C */
    uint8_t step = mem[0x005B];
    int borrow = (step < 0x40);                  /* SBC #$40 with C=1 */
    mem[0x005B] = (uint8_t)(step - 0x40);
    if (borrow) mem[0x005C]--;                    /* BCC L_aadf else DEC $5C */
    cpu.X = mem[0x28F0];                          /* LDX $28F0 */
    cpu.Y = mem[0x28F2];                          /* LDY $28F2 */
    if (cpu.Y < 0x6C) return;                     /* CPY #$6C; BCC plot_line_done */
    for (;;) {                                    /* L_aae9 (per scanline) */
        mem[0x28FA] = mem[0x28F4];                /* LDA $28F4; STA $28FA */
        do {                                      /* L_aaef (across the row) */
            if (cpu.X >= 0x2C && cpu.X < 0xD4)    /* CPX #$2C BCC; CPX #$D4 BCS */
                terrain_plot_pixel();             /* aaf7 (preserves X/Y) */
            cpu.X++;                              /* INX */
            mem[0x28FA]--;                        /* DEC $28FA */
        } while (mem[0x28FA] != 0);               /* BNE L_aaef */
        /* ab00 CLC; LDA $28F3; ADC $28F9; STA $28F3; BCC; INC $28F4; CLC */
        { uint16_t t = (uint16_t)mem[0x28F3] + mem[0x28F9];
          mem[0x28F3] = (uint8_t)t; if (t & 0x100) mem[0x28F4]++; }
        /* ab10 LDA $28EF; ADC $5B (C=0); STA $28EF; LDA $28F0; ADC $5C; STA $28F0; TAX */
        { uint16_t t = (uint16_t)mem[0x28EF] + mem[0x005B];
          mem[0x28EF] = (uint8_t)t; int c = (t >> 8) & 1;
          uint16_t u = (uint16_t)mem[0x28F0] + mem[0x005C] + c;
          mem[0x28F0] = (uint8_t)u; cpu.X = (uint8_t)u; }   /* TAX */
        cpu.Y--;                                  /* DEY */
        if (cpu.Y < 0x6C) return;                 /* CPY #$6C; BCS L_aae9 else done */
    }
}

/* plot_scanline_up @ $AB27 — Bresenham-style point plotter walking UP the screen.
 * From {$28F1:$28F2} (Y) and X=$28F0, walks via the {$28F1:$28F3}/{$28F7:$28F9}
 * step accumulators, drifting X by ±1 per scanline by the sign of $5C; plots
 * $28FA pixels up each column via terrain_plot_pixel (native).  Bounds: X in
 * [$2C,$D4), Y >= $6C.  Contract: memory; exit cpu dead.  plot_line_done absorbed. */
void plot_scanline_up(void) {
    cpu.X = mem[0x28F0];                          /* LDX $28F0 */
    cpu.Y = mem[0x28F2];                          /* LDY $28F2 */
    for (;;) {                                    /* L_ab2d */
        if (cpu.X < 0x2C) return;                 /* CPX #$2C; BCC done */
        if (cpu.X >= 0xD4) return;                /* CPX #$D4; BCS done */
        if (cpu.Y < 0x6C) return;                 /* CPY #$6C; BCC done */
        mem[0x28FA] = mem[0x28F4];                /* LDA $28F4; STA $28FA */
        do {                                      /* L_ab3f (up the column) */
            terrain_plot_pixel();                 /* ab3f (preserves X/Y) */
            cpu.Y--;                              /* DEY */
            if (cpu.Y < 0x6C) break;              /* CPY #$6C; BCC L_ab4c */
            mem[0x28FA]--;                        /* DEC $28FA */
        } while (mem[0x28FA] != 0);               /* BNE L_ab3f */
        /* L_ab4c CLC; LDA $28F3; ADC $28F9; STA $28F3; BCC; INC $28F4; CLC */
        { uint16_t t = (uint16_t)mem[0x28F3] + mem[0x28F9];
          mem[0x28F3] = (uint8_t)t; if (t & 0x100) mem[0x28F4]++; }
        /* L_ab5c LDA $28F1; ADC $28F7 (C=0); STA $28F1; LDA $28F2; ADC $28F8; STA $28F2; TAY */
        { uint16_t t = (uint16_t)mem[0x28F1] + mem[0x28F7];
          mem[0x28F1] = (uint8_t)t; int c = (t >> 8) & 1;
          uint16_t u = (uint16_t)mem[0x28F2] + mem[0x28F8] + c;
          mem[0x28F2] = (uint8_t)u; cpu.Y = (uint8_t)u; }   /* TAY */
        /* ab6f LDA $5C; BPL -> INX else DEX */
        if (mem[0x005C] & 0x80) cpu.X--;          /* DEX (X drift) */
        else                    cpu.X++;          /* INX */
    }
}

/* plot_scanline_rand_dir @ $AACF — RANDOM picks the walk direction. */
void plot_scanline_rand_dir(void) {
    if (bus_read(0xD20A) & 0x80) { plot_scanline_up(); return; }  /* BMI */
    plot_scanline_down();
}

/* game_state_update @ $A99C — the flight state machine.  Counts down $28EE; on
 * timeout reseeds it ($0624 & RANDOM) and resets state.  When the counter hits 0
 * with a queued event ($28ED!=0) it sets up the target-blip line-plot ($28EF-$28F9
 * seeded from RANDOM + the target column $28EB/$28EC) and draws it via the scanline
 * plotters, then pushes ring events ($0041 explosion-frame counter).  $007E==7 is
 * the special (impact?) branch with its own geometry + jitter_roll_pitch.
 * Contract: memory; exit cpu dead.  All callees native. */
void game_state_update(void) {
    uint8_t a;
    /* a99c DEC $28EE; BPL L_a9c3 */
    mem[0x28EE]--;
    if (mem[0x28EE] & 0x80) {                     /* N set: counter went negative */
        /* a9a1 LDA $0624; AND $D20A; STA $28EE */
        mem[0x28EE] = mem[0x0624] & bus_read(0xD20A);
        if (mem[0x2826] == 0) reset_flags_ff();   /* a9aa LDA $2826; BNE skip */
        if (mem[0x003D] == 0) mem[0x3355] = 0xB4; /* a9b2 LDA $003D; BNE skip */
        mem[0x0041] = 0x00;                       /* a9bb */
        mem[0x28ED] = 0x00;
        return;
    }
    /* L_a9c3: BPL taken — proceed only when the counter is exactly 0 with an event */
    if (mem[0x28EE] != 0) return;                 /* a9c3 BNE L_a9c2 */
    if (mem[0x28ED] == 0) return;                 /* a9c5 LDA $28ED; BEQ L_a9c2 */
    /* a9ca seed the line-plot from the target column $28EB/$28EC */
    mem[0x28F0] = mem[0x28EB];
    mem[0x28F2] = mem[0x28EC];
    mem[0x28F4] = 0x01;
    mem[0x28EF] = 0x80;
    mem[0x28F1] = 0x80;
    mem[0x28F3] = 0x80;
    mem[0x0058] = 0xFF;                            /* plot mask */
    mem[0x005C] = 0x00;
    mem[0x28F8] = 0x00;
    /* a9f1 LDA $D20A; ASL A; STA $5B; BCC; DEC $5C */
    a = bus_read(0xD20A);
    { int c = (a >> 7) & 1; mem[0x005B] = (uint8_t)(a << 1); if (c) mem[0x005C]--; }
    /* a9fb LDA $291A; BPL L_aa0b */
    a = mem[0x291A];
    if (a & 0x80) {                               /* N set */
        if (a < 0xFF) mem[0x005C] = 0x00;         /* aa00 CMP #$FF; BCS skip */
    } else {                                      /* L_aa0b */
        if (a >= 0x02) mem[0x005C] = 0xFF;        /* aa0b CMP #$02; BCC skip */
    }
    /* L_aa13 LDA $D20A; ASL A; STA $28F7; DEC $28F8 */
    mem[0x28F7] = (uint8_t)(bus_read(0xD20A) << 1);
    mem[0x28F8]--;
    if (mem[0x007E] == 0x07) {                     /* aa1d CMP #$07; BEQ L_aa4b */
        /* L_aa4b — special-state geometry */
        mem[0x005C] = 0x00;                        /* aa4b */
        /* aa4f SEC; LDA #$67; SBC $28EB; ASL A; STA $5B; BCC; DEC $5C */
        a = (uint8_t)(0x67 - mem[0x28EB]);
        { int c = (a >> 7) & 1; mem[0x005B] = (uint8_t)(a << 1); if (c) mem[0x005C]--; }
        /* aa5c SEC; LDA #$6B; SBC $28EC; STA $28F9 */
        mem[0x28F9] = (uint8_t)(0x6B - mem[0x28EC]);
        plot_scanline_down();                      /* aa65 */
        mem[0x003B] = 0x10;                        /* aa68 */
        mem[0x0041]++;                             /* aa6c INC $0041 */
        mem[0x00DB] = 0xBE;                        /* aa6e */
        mem[0x00DD] = 0xBC;                        /* aa72 */
        mem[0x00DC] = 0xB6;                        /* aa76 */
        mem[0x00DA] = 0xB8;                        /* aa7a */
        mem[0x3355] = 0x34;                        /* aa7e */
        cpu.X = 0x03; ring_push_marked();          /* aa83 LDX #$03 */
        cpu.X++;      ring_push_marked();          /* aa88 INX (X=$04) */
        cpu.X = 0x0A; ring_push_marked();          /* aa8c LDX #$0A */
        cpu.X++;      ring_push_marked();          /* aa91 INX (X=$0B) */
        jitter_roll_pitch();                       /* aa94 */
        return;
    }
    /* aa23 LDA $004D; EOR #$FF; LSR;LSR;LSR; CLC; ADC #$0C; STA $28F9 */
    mem[0x28F9] = (uint8_t)(((uint8_t)(mem[0x004D] ^ 0xFF) >> 3) + 0x0C);
    plot_scanline_rand_dir();                      /* aa30 */
    mem[0x00DB] = 0xBE;                            /* aa33 */
    if (mem[0x0041] == 0) mem[0x00DD] = 0x28;      /* aa37 LDA $0041; BNE skip */
    mem[0x0041]++;                                 /* aa3f INC $0041 */
    cpu.X = 0x07; ring_push_marked();              /* aa41 LDX #$07 */
    cpu.X = 0x02; ring_push_marked();              /* aa46 LDX #$02 */
}

/* pmg_enemy_update @ $7AB8 — per-frame enemy PMG update.  When RANDOM is negative,
 * occasionally repositions the enemy ($0044/$0047 to $6D or $70, $283D phase) and
 * jitters roll/pitch; then advances $003B unless level-clear ($0072==2); finally
 * pushes ring events $1A/$1B.  Reads RANDOM once.  Contract: memory; exit cpu dead. */
void pmg_enemy_update(void) {
    uint8_t a = bus_read(0xD20A);                 /* 7ab8 LDA $D20A */
    if (a & 0x80) {                               /* 7abb BPL L_7af3 (return) */
        if ((a & 0x0F) != 0) {                    /* 7abd AND #$0F; BEQ L_7add */
            a &= 0x0F;
            if (!(mem[0x003A] & 0x80)) {          /* 7ac1 LDY $003A; BMI L_7acb */
                mem[0x0044] = 0x6D;               /* 7ac5 LDY #$6D; STY $44 */
                mem[0x0047] = 0x6D;               /* STY $47 */
            }
            if ((a & 0x03) != 0) {                /* 7acb AND #$03; BEQ L_7add */
                mem[0x283D] = (uint8_t)(a & 0x01);/* 7acf AND #$01; STA $283D */
                mem[0x0044] = 0x70;               /* 7ad4 LDY #$70; STY $44 */
                mem[0x0047] = 0x70;               /* STY $47 */
                jitter_roll_pitch();              /* 7ada */
            }
        }
        /* L_7add LDA $0072; CMP #$02; BEQ L_7aea */
        if (mem[0x0072] != 0x02) {
            mem[0x003B] = (uint8_t)(mem[0x003B] + 0x04);   /* 7ae3 CLC; LDA $3B; ADC #$04; STA $3B */
        }
        /* L_7aea LDX #$1A; ring; INX; ring */
        cpu.X = 0x1A; ring_push_marked();         /* 7aea */
        cpu.X++;      ring_push_marked();         /* 7aef INX (X=$1B) */
    }
    /* L_7af3: RANDOM was positive -> nothing */
}

/* enemy_check @ $3FCD — event dispatch.  $063D (event trigger) -> intro_cinematic_loop
 * (LEFT TRANSPILED: its closure reaches the whole-program init/teardown; it has
 * 0 callers in steady flight); else $0633 (alien trigger) -> pmg_enemy_update.
 * Contract: memory; exit cpu dead. */
void enemy_check(void) {
    if (mem[0x063D] != 0) { intro_cinematic_loop(); return; }   /* 3fcd LDA $063D; BNE */
    if (mem[0x0633] != 0) pmg_enemy_update();            /* 3fd5 LDA $0633; BEQ skip */
}

/* ===========================================================================
 * IN-GAME SFX ENGINE — the $548D voice/gauge engine and its subtree.
 *
 * Run once per flight VBI (the Atari VBI tail $534D -> $548D).  These drain the
 * $0719 event ring that the native flight code (enemy_check / pmg_enemy_update /
 * ring_push_marked / the apex below) fills, advance per-voice envelopes, and
 * write POKEY AUDF/AUDC — which on the Amiga route through bus_write ->
 * platform_hw_write -> Paula, so the effects are audible automatically.
 *
 * They use the shared `cpu` register file for entry/exit registers exactly as
 * the 6502 ABI / their __t6502 twins do, so $548D threads X/Y/A into its callees
 * faithfully.  This is RACE-SAFE in flight: game_vbi_isr() save/restores `cpu`
 * around the whole VBI body (mirroring the OS VBLANK), and the only other `cpu`
 * users (the main-loop natives) are bracketed by that save/restore.  POKEY
 * register writes ($D1FE+X / $D1FF+X) are masked in validation (hardware side
 * effect, not mem[] state); the static AUDF/AUDC/AUDCTL writes already go through
 * bus_write in the oracle too, so those need no mask.
 * ========================================================================= */

/* sfx_voice_write_freq @ $5667 — write AUDF for voice (cpu.Y) to POKEY $D1FE+X,
 * where X = the POKEY register index mem[$0705+Y]; skip if 0 (inactive slot). */
void sfx_voice_write_freq(void) {
    uint8_t y = cpu.Y;
    uint8_t x = mem[0x0705 + y];
    cpu.X = x;
    if (x == 0) return;
    cpu.A = mem[0x0679 + y];
    bus_write((uint16_t)(0xD1FE + x), cpu.A);            /* AUDFn */
}

/* sfx_voice_write_freq_ctrl @ $5673 — write AUDF ($D1FE+X) freq AND AUDC ($D1FF+X)
 * = (prio/vol nibble $066B+Y & $0F) | (distortion $065D+Y) for voice cpu.Y. */
void sfx_voice_write_freq_ctrl(void) {
    uint8_t y = cpu.Y;
    uint8_t x = mem[0x0705 + y];
    cpu.X = x;
    if (x == 0) return;
    bus_write((uint16_t)(0xD1FE + x), mem[0x0679 + y]);  /* AUDFn freq */
    cpu.A = (uint8_t)((mem[0x066B + y] & 0x0F) | mem[0x065D + y]);
    bus_write((uint16_t)(0xD1FF + x), cpu.A);            /* AUDCn ctrl */
}

/* sfx_pick_top_voice @ $568A — scan slots X=1..12; latch the active slot
 * ($0705+X != 0) with the smallest priority nibble below $10 into
 * $0716 (running min) / $0714 (value) / $0715 (index). */
void sfx_pick_top_voice(void) {
    mem[0x0716] = 0x10;
    uint8_t x = 0;
    do {
        x++;                                             /* INX */
        if (mem[0x0705 + x] != 0) {                      /* LDA $0705+X; BEQ skip */
            uint8_t a = (uint8_t)(mem[0x066B + x] & 0x0F);
            if (a < mem[0x0716]) {                       /* CMP $0716; BCS skip (a>=M) */
                mem[0x0716] = a;
                mem[0x0714] = a;
                mem[0x0715] = x;
            }
        }
    } while (x < 0x0C);                                  /* CPX #$0C; BCC loop */
    cpu.X = x;
}

/* sfx_pick_next_voice @ $56AF — scan slots X=1..12; among EMPTY slots and the
 * excluded slot $0715, latch the largest priority nibble into $0716 / index
 * $0717.  (Faithful to the code: BEQ considers empty slots, else only X==$0715.) */
void sfx_pick_next_voice(void) {
    mem[0x0716] = 0x00;
    uint8_t x = 0;
    do {
        x++;                                             /* INX */
        int consider = (mem[0x0705 + x] == 0)            /* BEQ -> consider */
                     || (x == mem[0x0715]);              /* else only the $0715 slot */
        if (consider) {
            uint8_t a = (uint8_t)(mem[0x066B + x] & 0x0F);
            if (a >= mem[0x0716]) {                      /* CMP $0716; BCC skip (a<M) */
                mem[0x0716] = a;
                mem[0x0717] = x;
            }
        }
    } while (x < 0x0C);                                  /* CPX #$0C; BCC loop */
    cpu.X = x;
}

/* sfx_engine_step @ $5553 — the explosion/noise voice.  Entry cpu.A = mem[$0634]
 * (the engine state, set by the $548D caller): A==1 -> the descending-pitch
 * branch ($5585); else the RANDOM-reseeded noise branch.  Reads POKEY RANDOM
 * ($D20A) twice on reseed; writes AUDF1/AUDF3/AUDC1/AUDC3/AUDCTL. */
void sfx_engine_step(void) {
    if (cpu.A != 0x01) {
        /* L_555a: noise branch */
        uint8_t v = (uint8_t)(mem[0x0636] - 1);          /* DEC $0636 */
        mem[0x0636] = v;
        if (v & 0x80) {                                  /* BPL skip; underflow -> reseed */
            mem[0x0636] = (uint8_t)((bus_read(0xD20A) & 0x03) + 1);   /* RANDOM&3 +1 */
            uint8_t r = bus_read(0xD20A);
            bus_write(0xD204, (uint8_t)(r | 0x70));      /* AUDC3 */
            bus_write(0xD200, (uint8_t)((r & 0x7F) | 0x17)); /* AUDF1 */
        }
        /* L_557b */
        bus_write(0xD208, 0x04);                         /* AUDCTL */
        cpu.A = (uint8_t)(0x03 | 0xA0);                  /* A=3 -> L_55d3 ORA #$A0 */
        bus_write(0xD201, cpu.A);                        /* AUDC1 */
        bus_write(0xD205, cpu.A);                        /* AUDC3 */
        return;                                          /* (Y/$0637 untouched on this path) */
    }
    /* L_5585: descending-pitch branch */
    uint8_t y = (uint8_t)(mem[0x0635] - 1);              /* DEY */
    if (y & 0x80) y = 0x07;                              /* BPL skip; LDY #7 */
    mem[0x0635] = y;
    uint8_t d = mem[0x55DC + y];
    uint8_t a1 = (uint8_t)(mem[0x0638] - d);             /* SEC; SBC $55DC,Y */
    mem[0x0638] = a1; bus_write(0xD200, a1);             /* AUDF1 */
    uint8_t a3 = (uint8_t)(mem[0x0639] - d);             /* SEC; SBC */
    mem[0x0639] = a3; bus_write(0xD204, a3);             /* AUDF3 */
    uint8_t yy = mem[0x0637];
    uint8_t g  = (uint8_t)(mem[0x063A] - 1);             /* DEC $063A */
    mem[0x063A] = g;
    if (g & 0x80) {                                      /* BPL fails -> underflow */
        yy = (uint8_t)(yy + 2);                          /* INY; INY */
        if (yy >= 0x0F) {                                /* CPY #$0F; BCC skip */
            yy = 0x0F;
            mem[0x063A] = 0x32;
        }
    } else if (g == 0) {                                 /* BNE L_55cf; (g==0) */
        yy = (uint8_t)(yy - 1);                          /* DEY */
        if (yy == 0x06) mem[0x0634]++;                   /* CPY #6; BNE skip; INC $0634 */
        mem[0x063A]++;                                   /* INC $063A */
    }
    /* L_55cf */
    mem[0x0637] = yy;
    cpu.A = (uint8_t)(yy | 0xA0);                        /* TYA; ORA #$A0 */
    bus_write(0xD201, cpu.A);                            /* AUDC1 */
    bus_write(0xD205, cpu.A);                            /* AUDC3 */
    cpu.Y = yy;
}

/* input_init @ $581C — load a new voice into slot mem[$56D4+(X-1)] from the event
 * tables, where the entry cpu.X is the event id (1-based; X==0 is a no-op).  Saves
 * and restores X/Y (PHA/PLA).  Tail-calls game_sub_55FC (push the slot to the ring). */
void input_init(void) {
    uint8_t savedX = cpu.X, savedY = cpu.Y;
    uint8_t i = (uint8_t)(cpu.X - 1);                    /* DEX */
    if (!(i & 0x80)) {                                   /* BMI L_5876: skip when (X-1) negative */
        uint8_t y = mem[0x56D4 + i];                     /* voice slot */
        uint8_t ctl = mem[0x56F5 + i];
        mem[0x065D + y] = (uint8_t)(ctl & 0xF0);         /* distortion */
        mem[0x066B + y] = (uint8_t)(ctl & 0x0F);         /* prio/vol */
        mem[0x0679 + y] = mem[0x5716 + i];               /* freq */
        mem[0x06A3 + y] = mem[0x5737 + i];               /* duration */
        mem[0x0687 + y] = mem[0x5758 + i];
        mem[0x0695 + y] = mem[0x5779 + i];
        mem[0x06B1 + y] = mem[0x579A + i];
        uint8_t e = mem[0x57BB + i];
        mem[0x06DB + y] = e;
        if (e != 0) {                                    /* BEQ L_586d (skip these 3) */
            mem[0x06BF + y] = mem[0x57DC + i];
            mem[0x06CD + y] = mem[0x57E4 + i];
            mem[0x06E9 + y] = mem[0x57EC + i];
        }
        mem[0x06F7 + y] = mem[0x57F4 + i];               /* L_586d */
        cpu.Y = y;
        game_sub_55FC();                                 /* push slot Y to the ring */
    }
    cpu.Y = savedY; cpu.X = savedX;                      /* PLA;TAY; PLA;TAX */
    cpu.A = savedX;
}

/* reorder_sprite_slot @ $5614 — the voice-priority mixer.  Entry cpu.Y = the voice
 * slot just touched, cpu.X = a selector (0 -> promote/compact an empty slot via
 * sfx_pick_next_voice; !=0 -> demote a lower-priority slot under the current top).
 * Writes the moved voice (sfx_voice_write_freq_ctrl) and re-latches the top voice
 * (sfx_pick_top_voice).  Y is saved/restored (PHA/PLA); X is clobbered. */
void reorder_sprite_slot(void) {
    uint8_t savedY = cpu.Y;                              /* TYA; PHA */
    sfx_voice_write_freq_ctrl();                         /* 5616 (writes voice cpu.Y) */
    cpu.Y = savedY;
    int do_pick_top = 1;
    if (cpu.X == 0) {                                    /* TXA; BNE L_5641 -> here X==0 */
        uint8_t a = (uint8_t)(mem[0x066B + cpu.Y] & 0x0F);
        if (a < mem[0x0714]) {                           /* CMP $0714; BCC L_5664 */
            do_pick_top = 0;
        } else {
            int move = (a != mem[0x0714])                /* BNE L_562d */
                     || (cpu.Y >= mem[0x0715]);          /* equal: CPY $0715; BCC L_5664 else move */
            if (move) {                                  /* L_562d */
                uint8_t tx = mem[0x0715];
                mem[0x0705 + cpu.Y] = mem[0x0705 + tx];
                mem[0x0705 + tx] = 0x00;
                sfx_voice_write_freq_ctrl();             /* 563b (re-uses cpu.Y) */
            } else {
                do_pick_top = 0;                         /* a==top && Y<topidx -> L_5664 */
            }
        }
    } else {                                             /* L_5641: X!=0 */
        if (cpu.Y < 0x0D) {                              /* CPY #$0D; BCS L_5661 */
            sfx_pick_next_voice();
            uint8_t tx = mem[0x0715];
            if (tx != mem[0x0717]) {                     /* LDX $0715; CPX $0717; BEQ L_5661 */
                cpu.Y = mem[0x0717];                     /* LDY $0717 */
                mem[0x0705 + cpu.Y] = mem[0x0705 + tx];
                mem[0x0705 + tx] = 0x00;
                sfx_voice_write_freq_ctrl();             /* 565e */
            }
        }
    }
    if (do_pick_top) sfx_pick_top_voice();               /* L_5661 */
    cpu.Y = savedY;                                      /* L_5664: PLA; TAY */
    cpu.A = savedY;
}

/* update_gauge_digits @ $548D — APEX: the per-frame voice/gauge envelope engine.
 * Runs sfx_engine_step (when $0634 armed), advances the 14 voice/gauge slots'
 * frequency/duration/priority envelopes (BCD-step wrap $2D, gated by table
 * $5406; emits AUDF via sfx_voice_write_freq; re-queues finished slots via
 * game_sub_55FC / ring_push_marked), then drains the $0719 event ring:
 * bit7-set entries -> input_init (a new voice), bit7-clear -> reorder_sprite_slot. */
void update_gauge_digits(void) {
    if (mem[0x0634] != 0) {                              /* 548d LDA $0634; BEQ skip */
        cpu.A = mem[0x0634];                             /* sfx_engine_step reads entry A */
        sfx_engine_step();                               /* 5492 */
    }
    /* envelope loop: Y = $0E down to 1 */
    cpu.Y = 0x0E;
    do {
        uint8_t y = cpu.Y;
        mem[0x0718] = 0x00;                              /* 5497 per-slot "expired" flag */
        /* --- frequency-step block ($549c) --- */
        if (mem[0x06DB + y] != 0) {                      /* LDA $06DB+Y; BEQ L_54d5 */
            uint16_t s = (uint16_t)mem[0x06DB + y] + mem[0x06E9 + y];  /* CLC; ADC $06E9+Y */
            uint8_t a = (uint8_t)s;
            if (a & 0x80) a = (uint8_t)(a + 0x0A + (s >> 8)); /* BPL skip; ADC #$0A (+ prior carry) */
            else if (a >= 0x2D) a = 0x2C;                /* CMP #$2D; BCC skip; LDA #$2C */
            mem[0x06E9 + y] = a;
            if (mem[0x5406 + a] != 0) {                  /* TAX; LDA $5406,X; BEQ L_54d5 */
                uint8_t f = (uint8_t)(mem[0x0679 + y] + mem[0x06BF + y]);  /* CLC; ADC $06BF+Y */
                mem[0x0679 + y] = f;
                if (f == mem[0x06CD + y]) {              /* CMP $06CD+Y; BNE skip */
                    mem[0x06DB + y] = 0x00;
                    mem[0x0718]++;                       /* INC $0718 */
                }
                cpu.Y = y;
                sfx_voice_write_freq();                  /* 54d2 */
            }
        }
        /* --- duration/priority-step block ($54d5) --- */
        if (mem[0x06A3 + y] != 0) {                      /* LDA $06A3+Y; BEQ L_5510 */
            uint16_t s = (uint16_t)mem[0x06A3 + y] + mem[0x06B1 + y];  /* CLC; ADC $06B1+Y */
            uint8_t a = (uint8_t)s;
            if (a & 0x80) a = (uint8_t)(a + 0x0A + (s >> 8)); /* BPL skip; ADC #$0A (+ prior carry) */
            else if (a >= 0x2D) a = 0x2C;                /* CMP #$2D; BCC skip; LDA #$2C */
            mem[0x06B1 + y] = a;
            if (mem[0x5406 + a] != 0) {                  /* TAX; LDA $5406,X; BEQ L_5510 */
                uint8_t p = (uint8_t)((mem[0x066B + y] + mem[0x0687 + y]) & 0x0F); /* ADC; AND #$0F */
                mem[0x066B + y] = p;
                if (p == mem[0x0695 + y]) {              /* CMP $0695+Y; BNE skip */
                    mem[0x06A3 + y] = 0x00;
                    mem[0x0718]++;                       /* INC $0718 */
                }
                cpu.Y = y;
                game_sub_55FC();                         /* 550d (push slot Y to ring) */
            }
        }
        /* --- L_5510: if a slot expired this pass, re-queue its event id --- */
        if (mem[0x0718] != 0) {                          /* LDA $0718; BEQ L_551b */
            cpu.X = mem[0x06F7 + y];                     /* LDX $06F7+Y */
            ring_push_marked();                          /* 5518 (push X|$80) */
        }
        cpu.Y = (uint8_t)(y - 1);                        /* DEY */
    } while (cpu.Y != 0);                                /* BNE L_5497 (stop after Y=1) */

    /* --- clamp the ring head/tail to <= $1F ($5521) --- */
    if (mem[0x0073] > 0x1F) mem[0x0073] = 0x1F;          /* LDA #$1F; CMP $0073; BCS keep; STA */
    if (mem[0x0074] > 0x1F) mem[0x0074] = 0x1F;
    /* --- drain loop ($552F): walk tail $0074 down to head $0073, wrapping at $1F --- */
    for (;;) {
        cpu.Y = mem[0x0074];                             /* LDY $0074 */
        if (cpu.Y == mem[0x0073]) break;                 /* CPY $0073; BEQ done */
        cpu.A = mem[0x0719 + cpu.Y];                     /* LDA $0719,Y */
        if (cpu.A & 0x80) {                              /* BPL -> reorder; here bit7 set -> input */
            cpu.X = (uint8_t)(cpu.A & 0x7F);             /* AND #$7F; TAX */
            input_init();                                /* 553d */
        } else {
            cpu.Y = cpu.A;                               /* TAY (ring value = slot index) */
            reorder_sprite_slot();                       /* 5544 (entry cpu.X = leftover) */
        }
        uint8_t t = (uint8_t)(mem[0x0074] - 1);          /* DEC $0074 */
        if (t & 0x80) t = 0x1F;                          /* BPL skip; LDA #$1F; STA $0074 */
        mem[0x0074] = t;
    }
}

/* display_setup @ $5F1D — the orchestration APEX: main game display setup + the
 * Standby/attract idle loop + the launch (doors/tunnel/stars/planet) cinematic
 * driver.  game_main_loop ($3D48) calls it at the top of every game pass; it RTSes
 * back into game_main_loop's flight setup once START (or the demo timeout) fires.
 *
 * NOT validated by `make validate`: the body busy-waits on VBI-updated state
 * ($0655/$0089/$0014/$067E/$0684/$0686), the attract timer and live HW input,
 * which would hang the equivalence harness.  Correctness is verified on FS-UAE by
 * behaviour, exactly like RescueOnFractalus::run().  This is a FAITHFUL
 * transcription of the $5F1D transliteration: cpu/mem state is preserved bit-for-
 * bit so the (still-transpiled) leaf callees and the wait_frames_NN / clear_colors /
 * push_a_thunk_3cb2 frame primitives behave identically.  Each original spin-wait
 * SPINWAIT-hook point becomes one real Amiga frame via ds_frame() (the same
 * platform_tick_vbi + platform_render_frame the transpiler injected); the PHA-based
 * frame primitives drive frames internally and are called as-is.
 *
 * The only deviation from a verbatim copy is fidelity: where the 6502 consumes the
 * exit A of init_row_coords_9c (a pure-mem native leaf that does not set cpu.A),
 * the faithful exit value A=$13 is loaded explicitly before it is stored to $00B9. */
static inline void ds_frame(void) { platform_tick_vbi(); platform_render_frame(); }

void display_setup(void) {
    /* 5f1d */
    g_standbyRevealReady = 1;   /* Amiga: cockpit/top-bar drawn + sprites up — reveal (latched) */
    LDA(0x06);
    bus_write(0x02C7, cpu.A);
    build_line_addr_table_2000();
    dl_index_dec_or_reset();
    LDA(0x00);
    mem[0x00B7] = cpu.A;
    mem[0x08A2] = cpu.A;
    mem[0x062C] = cpu.A;
    LDY(0x0A);
L_5f34:
    DEY();
    mem[(0x0088)+cpu.Y] = cpu.A;
    if (!cpu.Z) goto L_5f34;
    LDA(0x00);
    mem[0x00C1] = cpu.A;
    LDA(0x10);
    mem[0x00C2] = cpu.A;
    LDA(0x0F);
    mem[0x00C3] = cpu.A;
    LDA(0x08);
    mem[0x00C4] = cpu.A;
    memset_or_copy();
    wait_vcount_30();
    LDA(0xD7);
    bus_write(0x0222, cpu.A);
    LDA(0x52);
    bus_write(0x0223, cpu.A);
    wait_vcount_ge_7a();
    LDA(0xC2);
    bus_write(0x0200, cpu.A);
    LDA(0x6C);
    bus_write(0x0201, cpu.A);
    LDA(0x20);
    bus_write(0xD402, cpu.A);
    LDA(0x31);
    bus_write(0xD403, cpu.A);
    LDA(0x59);
    mem[0x00BB] = cpu.A;
    LDA(0x6E);
    mem[0x00BC] = cpu.A;
    LDA(0x31);
    mem[0x00BD] = cpu.A;
    LDA(0x0B);
    mem[0x00BE] = cpu.A;
    rle_expand_list();
    LDA(0x14);
    bus_write(0x026F, cpu.A);
    bus_write(0xD01B, cpu.A);
    LDY(0x56);
    LDA(0x00);
L_5f90:
    mem[(0x0C31)+cpu.Y] = cpu.A;
    mem[(0x0D31)+cpu.Y] = cpu.A;
    DEY();
    if (!cpu.N) goto L_5f90;
    bus_write(0xD00C, cpu.A);
    LDY(0x03);
    TYA();
L_5f9f:
    mem[(0xD008)+cpu.Y] = cpu.A;
    DEY();
    if (!cpu.N) goto L_5f9f;
    LDA(0x2D);
    bus_write(0xD000, cpu.A);
    bus_write(0xD002, cpu.A);
    LDA(0xBE);
    mem[0x00B5] = cpu.A;
    bus_write(0xD003, cpu.A);
    clear_colors();
    LDA(0xC0);
    bus_write(0xD40E, cpu.A);
    LDA(0x3F);
    bus_write(0x022F, cpu.A);
    LDA(0x03);
    bus_write(0xD01D, cpu.A);
    LDA(0x00);
    bus_write(0x02C0, cpu.A);
    LDA(0x06);
    bus_write(0x02C8, cpu.A);
    LDA(0x88);
    mem[0x00B7] = cpu.A;
    fill_region_2000();
    wait_vcount_ge_7a();
    LDA(0xAD);
    bus_write(0x0200, cpu.A);
    LDA(0x6C);
    bus_write(0x0201, cpu.A);
    LDA(0x00);
    bus_write(0xD402, cpu.A);
    LDA(0x30);
    bus_write(0xD403, cpu.A);
    fill_four_bufs_ff();
    LDA(mem[0x006C]);
    if (!cpu.Z) goto L_5ff8;
    goto L_6118;
L_5ff8:
    clear_message_buffer();
    LDY(0x04);
L_5ffd:
    LDA(mem[(0x6B80)+cpu.Y]);
    mem[(0x00A1)+cpu.Y] = cpu.A;
    DEY();
    if (!cpu.N) goto L_5ffd;
    LDA(mem[0x00DD]);
    fill_buf_08d4();
    game_sub_6811();
    LDX(mem[0x00DD]);
    if (cpu.Z) goto L_601c;
L_6012:
    mem[0x0071] = cpu.X;
    wait_frames_2();
    DEX();
    CPX(0x1F);
    if (!cpu.Z) goto L_6012;
L_601c:
    LDA(0x00);
    mem[0x0071] = cpu.A;
    LDA(mem[0x00DD]);
    AND(0x0F);
    if (!cpu.Z) goto L_6028;
    ORA(0x02);
L_6028:
    TAX();
L_6029:
    TXA();
    fill_buf_08d4();
    clear_colors();
    INX();
    CPX(0x0F);
    if (!cpu.Z) goto L_6029;
    build_line_addr_table_1000();
    LDA(mem[0x0633]);
    if (cpu.Z) goto L_6047;
    LDY(0x00);
    mem[0x0633] = cpu.Y;
    LDA(0xC8);
    save_color_clear_y_bit5();
L_6047:
    draw_frame_pattern_seq();
    platform_tunnel_rings_drawn();   /* hook: convert the freshly-drawn $1000 ring field to bitplanes */
    init_row_coords_9c();
    LDA(0x13);                 /* init_row_coords_9c exit A=$13 (faithful; native leaf leaves cpu.A untouched) */
    mem[0x00B9] = cpu.A;
    LDA(0x08);
    mem[0x0094] = cpu.A;
L_6053:
    LDY(mem[0x00B9]);
    plot_terrain_span();
    DEC_M(0x00B9);
    if (!cpu.N) goto L_6053;
    LDA(0x07);
    draw_cockpit_dial_bar();
    LDY(0x00);
L_6063:
    LDX(0x0E);
L_6065:
    TXA();
    mem[(0x08D4)+cpu.Y] = cpu.A;
    push_a_thunk_3cb2();
    DEX();
    if (!cpu.N) goto L_6065;
    INY();
    CPY(0x06);
    if (!cpu.Z) goto L_6063;
    fill_region_2000();
    LDA(0x03);
    mem[0x004C] = cpu.A;
    mem[0x27A3] = cpu.A;
    LDY(0x90);
L_6080:
    mem[0x08D9] = cpu.Y;
    push_a_thunk_3cb2();
    INY();
    CPY(0x9B);
    if (!cpu.Z) goto L_6080;
    init_terrain_dl();
    LDY(0x05);
L_6090:
    LDA(mem[(0x6E68)+cpu.Y]);
    mem[(0x08D4)+cpu.Y] = cpu.A;
    DEY();
    if (!cpu.N) goto L_6090;
    LDA(0x01);
    mem[0x008D] = cpu.A;
    mem[0x0094] = cpu.A;
    mem[0x06CC] = cpu.A;
    init_row_coords_9c();
    LDA(0x13);                 /* init_row_coords_9c exit A=$13 (faithful) */
    mem[0x00B9] = cpu.A;
    LDA(0x7E);
    mem[0x00C3] = cpu.A;
    LDA(0x2A);
    mem[0x00C4] = cpu.A;
    mem[0x00C2] = cpu.A;
    clear_message_buffer();
    mem[0x00C1] = cpu.A;
L_60b6:
    LDY(mem[0x00B9]);
    if (!cpu.Z) goto L_60c3;
    LDA(0xFF);
    mem[0x008D] = cpu.A;
    LDA(mem[0x08D8]);
    mem[0x0071] = cpu.A;
L_60c3:
    /* spin: wait for the standby/launch VBI to clear $008E (one Amiga frame/iter) */
    LDA(mem[0x008E]);
    if (cpu.Z) { ds_frame(); goto L_60c3; }
    LDA(0x00);
    mem[0x008E] = cpu.A;
    emit_dl_coord_pairs();
    INC_M(0x0094);
    LDA(mem[0x0094]);
    CMP(0x07);
    if (!cpu.Z) goto L_60da;
    LDA(0x01);
    mem[0x0094] = cpu.A;
L_60da:
    DEC_M(0x00B9);
    if (!cpu.N) goto L_60b6;
    LDA(0x01);
    mem[0x008D] = cpu.A;
    init_row_coords_9c();
    LDY(0x08);
    mem[0x0094] = cpu.Y;
    LDA(0x00);
L_60eb:
    mem[(0x0C87)+cpu.Y] = cpu.A;
    mem[(0x0D87)+cpu.Y] = cpu.A;
    DEY();
    if (!cpu.Z) goto L_60eb;
    LDA(0xC0);
    mem[0x0071] = cpu.A;
L_60f8:
    /* spin: wait for the VBI to clear $008D (one frame/iter) */
    LDA(mem[0x008D]);
    if (!cpu.Z) { ds_frame(); goto L_60f8; }
    draw_shape_rows_loop();
    LDY(0x00);
    LDA(0xFF);
L_6103:
    clear_colors();
    mem[(0x0C88)+cpu.Y] = cpu.A;
    mem[(0x0D88)+cpu.Y] = cpu.A;
    INY();
    CPY(0x08);
    if (!cpu.Z) goto L_6103;
    TYA();
    draw_cockpit_dial_bar();
    build_line_addr_table_2000();
L_6118:
    font_display_init();
    wait_frames_5();
    LDA(0x07);
    mem[0x0095] = cpu.A;
    LDA(0x88);
    mem[0x00B7] = cpu.A;
    fill_region_2000();
    blit_message_block();
    blit_numeric_readout();
    g_doorFieldReady = 1;   /* Amiga: doors/LEVEL drawn into $2000 — decode now, before the green fade */
    dl_index_dec_or_reset();
    delay_loop_c2_to_c9();
    LDA(0xFF);
    mem[0x00E3] = cpu.A;
    LDA(mem[0x060B]);
    if (!cpu.Z) goto L_6141;
    goto L_62eb;
L_6141:
    LDA(mem[0x006C]);
    if (!cpu.Z) goto L_6153;
    LDY(mem[0x0644]);
    if (cpu.Z) goto L_614d;
    goto L_6332;
L_614d:
    wait_frames_save_a();
    goto L_634f;
L_6153:
    LDA(mem[0x0629]);
    if (!cpu.Z) goto L_615b;
    goto L_61f8;
L_615b:
    SEC();
    SBC(mem[0x062A]);
    mem[0x00E3] = cpu.A;
    if (!cpu.N) goto L_6174;
    LDY(mem[0x062A]);
    SEC();
    TYA();
    SBC(mem[0x0629]);
    mem[0x062A] = cpu.A;
    LDA(mem[0x0629]);
    goto L_6178;
L_6174:
    LDA(mem[0x062A]);
    TAY();
L_6178:
    mem[0x0096] = cpu.A;
    TYA();
    set_0628_bcd_redisplay();
    LDY(0x0C);
    LDA(0x78);
    save_color_clear_y_bit5();
L_6185:
    LDA(0x00);
    mem[0x0045] = cpu.A;
    LDA(0x05);
    mem[0x0046] = cpu.A;
    decrement_bcd_0628_restart();
    DEC_M(0x0096);
    if (!cpu.Z) goto L_6185;
    LDA(mem[0x00E3]);
    if (cpu.N) goto L_61f2;
    if (cpu.Z) goto L_61b7;
    wait_frames_save_a();
    set_0628_bcd_redisplay();
    LDA(0x7C);
    mem[0x00D8] = cpu.A;
L_61a4:
    LDA(0x10);
    mem[0x0046] = cpu.A;
    LDX(0x10);
    input_init();
    wait_frames_5();
    decrement_bcd_0628_restart();
    DEC_M(0x00E3);
    if (!cpu.Z) goto L_61a4;
L_61b7:
    wait_frames_save_a();
    LDA(0x2A);
    LDY(0x83);
    save_color_clear_y_bit5();
    LDY(0x0B);
    music_init_state();
L_61c6:
    /* spin: wait for the music/VBI to clear $0655 (one frame/iter) */
    ds_frame();
    LDA(mem[0x0655]);
    if (!cpu.Z) goto L_61c6;
    LDA(mem[0x006D]);
    mem[0x00C1] = cpu.A;
    CMP(mem[0x060A]);
    if (!cpu.C) goto L_61d7;
    mem[0x060A] = cpu.A;
L_61d7:
    count_up_to_level();
    LDA(mem[0x0627]);
    if (!cpu.Z) goto L_61f5;
L_61df:
    LDA(0x02);
    mem[0x0096] = cpu.A;
L_61e3:
    LDA(mem[0x00C3]);
    mem[0x0046] = cpu.A;
    reinit_and_redraw_via_delay();
    DEC_M(0x0096);
    if (!cpu.Z) goto L_61e3;
    DEC_M(0x00C1);
    if (!cpu.Z) goto L_61df;
L_61f2:
    wait_frames_save_a();
L_61f5:
    clear_message_buffer();
L_61f8:
    LDA(mem[0x00E3]);
    if (!cpu.N) goto L_6219;
    LDA(mem[0x006D]);
    CMP(0x05);
    if (cpu.C) goto L_620e;
    LDA(0x01);
    mem[0x009B] = cpu.A;
    LDA(0x04);
    mem[0x0609] = cpu.A;
    goto L_6216;
L_620e:
    mem[0x0609] = cpu.A;
    SEC();
    SBC(0x03);
    mem[0x009B] = cpu.A;
L_6216:
    goto L_62e7;
L_6219:
    LDA(mem[0x006D]);
    mem[0x009B] = cpu.A;
    INC_M(0x009B);
    CLC();
    ADC(0x04);
    CMP(0x64);
    if (!cpu.C) goto L_6228;
    LDA(0x63);
L_6228:
    mem[0x0609] = cpu.A;
    mem[0x0090] = cpu.A;
L_622d:
    LDY(0x00);
    mem[0x00B7] = cpu.Y;
    LDA(0x4F);
    mem[0x00B9] = cpu.A;
    LDA(0x44);
    bus_write(0xD203, cpu.A);
    LDA(0x52);
    mem[0x08DB] = cpu.A;
    LDA(0x01);
    mem[0x08DC] = cpu.A;
L_6244:
    LDA(mem[(0x6595)+cpu.Y]);
    mem[0x004C] = cpu.A;
    audf2_sweep_clear_colors();
    LDA(mem[0x00B9]);
    mem[0x008B] = cpu.A;
    dl_index_dec();
    LDA(0x00);
    mem[0x008B] = cpu.A;
    DEC_M(0x00B9);
    INC_M(0x00B7);
    LDY(mem[0x00B7]);
    CPY(0x13);
    if (!cpu.Z) goto L_6244;
    push_a_thunk_3cb2();
    LDA(0x53);
    mem[0x008B] = cpu.A;
L_6268:
    LDX(0x05);
L_626a:
    LDA(mem[0x008B]);
    CMP(0x3E);
    if (!cpu.Z) goto L_6289;
    LDA(0x56);
    mem[0x008B] = cpu.A;
    CPX(0x03);
    if (!cpu.Z) goto L_6288;
    LDA(0x08);
    mem[0x0095] = cpu.A;
    blit_numeric_readout();
    DEC_M(0x0095);
    INC_M(0x006D);
    blit_numeric_readout();
    LDX(0x03);
L_6288:
    DEX();
L_6289:
    if (!cpu.Z) goto L_626a;
    LDA(0x56);
    mem[0x008B] = cpu.A;
L_628f:
    LDA(mem[0x008B]);
    CMP(0x0F);
    if (!cpu.Z) goto L_628f;
    mem[0x00B9] = cpu.A;
    LDA(0x00);
    mem[0x008B] = cpu.A;
    LDA(bus_read(0xD300));
    AND(0x01);
    if (cpu.Z) goto L_62a9;
    LDA(bus_read(0xD01F));
    AND(0x02);
    if (!cpu.Z) goto L_62b4;
L_62a9:
    LDA(0x56);
    mem[0x008B] = cpu.A;
    LDY(mem[0x006D]);
    CPY(mem[0x0609]);
    if (!cpu.C) goto L_6268;
L_62b4:
    LDA(0xFF);
    mem[0x08DC] = cpu.A;
L_62b9:
    LDY(mem[0x00B9]);
    LDA(mem[(0x6598)+cpu.Y]);
    mem[0x004C] = cpu.A;
    mem[0x008B] = cpu.Y;
    dl_index_dec();
    LDA(0x00);
    mem[0x008B] = cpu.A;
    audf2_sweep_clear_colors();
    DEC_M(0x00B9);
    if (!cpu.Z) goto L_62b9;
    INC_M(0x004C);
    push_a_thunk_3cb2();
    LDA(0x55);
    shift_object_table_up();
    ASL_M(0x004C);
    push_a_thunk_3cb2();
    dl_index_dec_or_reset();
    LDA(0x00);
    bus_write(0xD203, cpu.A);
L_62e7:
    LDA(mem[0x0090]);
    if (cpu.Z) goto L_62ee;
L_62eb:
    ds_frame();
    reset_audctl_flags();
L_62ee:
    LDA(0x64);
    mem[0x00E2] = cpu.A;
    LDA(mem[0x0004]);
    if (!cpu.Z) goto L_634f;
L_62f6:
    ds_frame();
    LDY(mem[0x060B]);
    if (!cpu.Z) goto L_6309;
    copy_altitude_graphic_to_screen();
    LDA(bus_read(0xD01F));
    AND(0x04);
    if (!cpu.Z) goto L_6309;
    INC_M(0x0004);
    if (!cpu.Z) goto L_6311;
L_6309:
    LDA(mem[0x00E2]);
    if (!cpu.N) goto L_6324;
    LDX(mem[0x00E7]);
    if (!cpu.Z) goto L_631e;
L_6311:
    LDY(mem[0x060B]);
    mem[0x006C] = cpu.Y;
    if (!cpu.Z) goto L_631b;
    INC_M(0x060B);
L_631b:
    game_main_loop(); return;
L_631e:
    LDX(mem[0x0091]);
    CPX(0xC0);
    if (cpu.Z) goto L_6311;
L_6324:
    LDA(bus_read(0xD300));
    AND(0x01);
    if (cpu.Z) goto L_6332;
    LDA(bus_read(0xD01F));
    AND(0x02);
    if (!cpu.Z) goto L_634a;
L_6332:
    LDY(mem[0x060B]);
    if (cpu.Z) goto L_6311;
    audio_timer_setup();
    LDA(mem[0x006D]);
    CMP(mem[0x0609]);
    if (!cpu.C) goto L_6347;
    intro_screen_build_seq();
    goto L_634a;
L_6347:
    goto L_622d;
L_634a:
    platform_poll_events();
    read_console_trig_delta();
    if (cpu.Z) goto L_62f6;
L_634f:
    audio_timer_setup();
    rle_unpack_to_07f9();
    LDA(mem[0x060B]);
    if (!cpu.Z) goto L_635f;
    LDX(0x16);
    fill_message_buffer();
L_635f:
    LDA(0x23);
    mem[0x08A3] = cpu.A;
    mem[0x060B] = cpu.A;
    mem[0x006C] = cpu.A;
    LDY(0x0B);
    LDA(mem[0x0004]);
    if (cpu.Z) goto L_6371;
    LDY(0x13);
L_6371:
    LDA(0xEA);
    save_color_clear_y_bit5();
    LDA(0x00);
    mem[0x0629] = cpu.A;
    LDY(mem[0x0004]);
    if (!cpu.Z) goto L_63a1;
    LDY(mem[0x006D]);
    CPY(mem[0x0626]);
    mem[0x0626] = cpu.Y;
    if (!cpu.Z) goto L_638f;
    INC_M(0x0627);
    goto L_63a7;
L_638f:
    if (cpu.C) goto L_63a1;
    LDY(0x04);
L_6393:
    mem[(0x0600)+cpu.Y] = cpu.A;
    DEY();
    if (!cpu.N) goto L_6393;
    LDY(0x05);
L_639b:
    mem[(0x32C5)+cpu.Y] = cpu.A;
    DEY();
    if (!cpu.N) goto L_639b;
L_63a1:
    mem[0x0627] = cpu.A;
    compute_gauge_geometry_from_006D();
L_63a7:
    LDX(0x1D);
    input_init();
    vobj_draw_dispatch();
    render_bcd_counter();
    clear_scroll_accum();
    LDY(0x05);
    LDA(0x3A);
L_63b9:
    mem[(0x08D4)+cpu.Y] = cpu.A;
    SEC();
    SBC(0x02);
    DEY();
    if (!cpu.N) goto L_63b9;
    LDA(0x00);
    mem[0x0641] = cpu.A;
    startup_init();
    build_line_addr_table_1000();
    draw_frame_pattern_seq();
    platform_tunnel_rings_drawn();   /* hook: convert the freshly-drawn $1000 ring field to bitplanes */
    LDX(0x01);
    input_init();
    LDA(0x1F);
L_63d7:
    /* spin: wait for the door VBI counter $067E to reach $1F */
    ds_frame();
    CMP(mem[0x067E]);
    if (!cpu.Z) goto L_63d7;
    LDA(0x00);
    mem[0x06E0] = cpu.A;
    LDA(0x8C);
    mem[0x0080] = cpu.A;
    LDA(0x17);
    mem[0x0081] = cpu.A;
    LDA(0xBA);
    mem[0x0082] = cpu.A;
    LDA(0x17);
    mem[0x0083] = cpu.A;
    LDA(0x02);
    mem[0x0098] = cpu.A;
    LDA(0x7F);
    mem[0x0097] = cpu.A;
    LDA(0x2B);
    mem[0x008A] = cpu.A;
    LDA(0x07);
    draw_cockpit_dial_bar();
    LDA(0x7F);
    mem[0x0684] = cpu.A;
    LDA(0x09);
    mem[0x0676] = cpu.A;
    LDA(0xFB);
    mem[0x0686] = cpu.A;
    LDA(0xFF);
    mem[0x0687] = cpu.A;
    LDA(0x01);
    set_hud_fields_678_679();
L_641b:
    wait_frames_10();
    INC_M(0x0678);
    INC_M(0x0679);
    LDA(mem[0x0678]);
    CMP(0x03);
    if (!cpu.Z) goto L_644c;
    LDA(0xFF);
    mem[0x06CA] = cpu.A;
    mem[0x06CC] = cpu.A;
    mem[0x06CD] = cpu.A;
    LDA(0x01);
    mem[0x06E6] = cpu.A;
    mem[0x06E8] = cpu.A;
    mem[0x06E9] = cpu.A;
    LDA(0x00);
    mem[0x06F4] = cpu.A;
    mem[0x06F6] = cpu.A;
    mem[0x06F7] = cpu.A;
L_644c:
    DEC_M(0x0676);
    refresh_hud_field_0b();
    LDA(mem[0x0676]);
    CMP(0x04);
    if (!cpu.Z) goto L_641b;
    LDA(0x64);
L_645b:
    /* spin: wait for the gauge-fill VBI counter $0684 to reach $64 */
    ds_frame();
    CMP(mem[0x0684]);
    if (!cpu.Z) goto L_645b;
    LDY(0x05);
    LDA(0x00);
    mem[(0x066B)+cpu.Y] = cpu.A;
    reorder_sprite_slot();
    LDA(0x1D);
L_646c:
    /* spin: wait for $0684 to reach $1D */
    ds_frame();
    CMP(mem[0x0684]);
    if (!cpu.Z) goto L_646c;
    LDA(0xFF);
    mem[0x06E6] = cpu.A;
    LDA(0x64);
L_6478:
    /* spin: wait for the door-swoosh VBI counter $0686 to reach $64 */
    ds_frame();
    CMP(mem[0x0686]);
    if (!cpu.Z) goto L_6478;
    init_row_coords_9c();
    LDA(0x00);
    mem[0x0094] = cpu.A;
    draw_cockpit_dial_bar();
    LDY(0x0C);
    LDA(0x0F);
    mem[(0x066B)+cpu.Y] = cpu.A;
    LDA(0xB4);
    mem[(0x0679)+cpu.Y] = cpu.A;
    reorder_sprite_slot();
    LDA(0x02);
    mem[0x0676] = cpu.A;
    LDA(0x00);
    mem[0x06E8] = cpu.A;
    mem[0x06E9] = cpu.A;
    LDA(0x01);
    set_hud_fields_678_679();
    LDA(0x01);
    mem[0x0088] = cpu.A;
    LDA(0x0F);
    mem[0x004C] = cpu.A;
L_64b0:
    push_a_thunk_3cb2();
    LDA(mem[0x0677]);
    CMP(0x08);
    if (cpu.Z) goto L_64c0;
    DEC_M(0x0677);
    goto L_64c4;
L_64c0:
    LDA(0x01);
    mem[0x004C] = cpu.A;
L_64c4:
    LDA(mem[0x0088]);
    if (!cpu.Z) goto L_64b0;
    LDA(0x04);
    mem[0x0677] = cpu.A;
    draw_cockpit_dial_bar();
    LDY(0x0C);
    LDA(0x65);
    mem[(0x0679)+cpu.Y] = cpu.A;
    reorder_sprite_slot();
    LDY(0x0B);
    LDA(0x01);
    mem[(0x066B)+cpu.Y] = cpu.A;
    LDA(0x0E);
    mem[(0x0679)+cpu.Y] = cpu.A;
    reorder_sprite_slot();
    LDA(0x01);
    LDY(0x03);
L_64ed:
    mem[(0xD008)+cpu.Y] = cpu.A;
    DEY();
    if (!cpu.N) goto L_64ed;
    LDA(0x38);
    bus_write(0xD000, cpu.A);
    LDA(0x62);
    mem[0x00B5] = cpu.A;
    LDA(0x8E);
    bus_write(0xD002, cpu.A);
    LDA(0xB8);
    bus_write(0xD003, cpu.A);
    clear_scroll_accum();
    LDX(0x2C);
L_650b:
    LDA(mem[(0x073D)+cpu.X]);
    mem[0x00C1] = cpu.A;
    LDA(mem[(0x0793)+cpu.X]);
    mem[0x00C2] = cpu.A;
    LDY(0x2D);
    LDA(0x00);
L_6519:
    bus_write(ZP_IND_Y(0xC1), cpu.A);
    DEY();
    if (!cpu.N) goto L_6519;
    DEX();
    if (!cpu.N) goto L_650b;
    copy_192_to_1800();
    LDA(0x00);
    mem[0x00DC] = cpu.A;
    mem[0x0071] = cpu.A;
    LDA(0x10);
    mem[0x3157] = cpu.A;
    LDA(0x18);
    mem[0x3158] = cpu.A;
    wait_vcount_ge_7a();
    LDA(0xC2);
    bus_write(0x0200, cpu.A);
    LDA(0x6C);
    bus_write(0x0201, cpu.A);
    LDA(0x20);
    bus_write(0xD402, cpu.A);
    LDA(0x31);
    bus_write(0xD403, cpu.A);
    init_object_positions();
    LDA(0x7F);
    mem[0x0089] = cpu.A;
    fill_terrain_columns();
    LDA(0x00);
L_6557:
    LDX(0x03);
L_6559:
    mem[(0x02C0)+cpu.X] = cpu.A;
    DEX();
    if (!cpu.N) goto L_6559;
    wait_frames_2();
    CLC();
    ADC(0x01);
    CMP(0x0D);
    if (!cpu.Z) goto L_6557;
    LDA(0x30);
    build_line_addr_table_1000_stride();
L_656e:
    /* spin: wait until the planet-approach VBI drops $0089 below $04 */
    ds_frame();
    LDA(mem[0x0089]);
    CMP(0x04);
    if (!cpu.N) goto L_656e;
    LDA(0x00);
    mem[0x0014] = cpu.A;
L_6578:
    /* spin: every other frame ($0014 reaching 2) advance the object positions */
    ds_frame();
    LDA(mem[0x0014]);
    CMP(0x02);
    if (!cpu.C) goto L_6590;
    LDA(0x00);
    mem[0x0014] = cpu.A;
    advance_object_positions();
    LDA(mem[0x1002]);
    CMP(0xFF);
    if (!cpu.Z) goto L_6590;
    LDA(0x00);
    mem[0x0089] = cpu.A;
L_6590:
    LDA(mem[0x0089]);
    if (!cpu.Z) goto L_6578;
    return;
}

/* game_main_loop @ $3D48 — the second half of the orchestration apex.  game_entry
 * (-> init_game_vars_attract_timer) chains here; it does the one-time game init
 * (display list, sound, PMG, player, cockpit gated on $060B), then loops:
 *   L_3e0f  display_setup()      (Standby/attract + launch cinematic; RTSes after launch)
 *           flight init ($3e12)  (clear PMG/colours, VVBLKI=$4FF5, terrain seed, gameplay state)
 *   L_3eba  the in-game flight loop (two terrain passes/iteration for double-buffering),
 *           level-clear handoff at L_3f59, then back to L_3eba or L_3e0f.
 * The flight loop never returns (the user-quit path longjmps out of the pump); the
 * L_631b display_setup->game_main_loop tail call and the L_3e0f display_setup() call
 * mirror the genuine mutual structure (both are TCO'd / return-normally, as in the
 * shipped transpiled build — no unbounded recursion).
 *
 * Same seam/contract as display_setup: NOT in `make validate` (it spin-waits on VBI
 * state, and never returns), verified on FS-UAE.  Faithful transcription preserving
 * cpu/mem so the (mostly-native) leaf callees behave identically; the two hooked
 * spin labels L_3eba/L_3f6d become one real Amiga frame via ds_frame(); the
 * wait_vcount_30/wait_frames_10/clear_colors/push_a_thunk_3cb2 frame primitives drive
 * frames internally and are called as-is.  No leaf exit-register is consumed here
 * (every post-call read reloads from mem), so no fidelity fix-ups are needed. */
void game_main_loop(void) {
    LDA(0x00);
    bus_write(0x022F, cpu.A);
    bus_write(0xD01D, cpu.A);
    LDY(0x07);
L_3d52:
    mem[(0xD00D)+cpu.Y] = cpu.A;
    mem[(0x36CA)+cpu.Y] = cpu.A;
    DEY();
    if (!cpu.N) goto L_3d52;
    bus_write(0x02C8, cpu.A);
    wait_vcount_30();
    LDA(0xCC);
    bus_write(0x0222, cpu.A);
    LDA(0x53);
    bus_write(0x0223, cpu.A);
    display_list_init();
    LDA(0x00);
    mem[0x0002] = cpu.A;
    mem[0x00DC] = cpu.A;
    mem[0x0042] = cpu.A;
    mem[0x00C7] = cpu.A;
    LDY(0x19);
L_3d7a:
    DEY();
    mem[(0x062C)+cpu.Y] = cpu.A;
    if (!cpu.Z) goto L_3d7a;
    mem[0x062F] = cpu.A;
    LDA(0x08);
    bus_write(0xD407, cpu.A);
    LDA(0x04);
    bus_write(0xD409, cpu.A);
    LDA(0x03);
    bus_write(0xD20F, cpu.A);
    loader_util();
    game_init_7813();
    game_init_77DF();
    game_init_7588();
    game_init_76CB();
    LDA(0xC0);
    bus_write(0xD20E, cpu.A);
    LDA(mem[0x060B]);
    if (cpu.Z) goto L_3dae;
    cockpit_display();
L_3dae:
    game_sub_4258();
    LDY(0x09);
    draw_dial_bar_column();
    game_sub_4606();
    LDA(0x08);
    draw_cockpit_dial_bar();
    LDA(0x2C);
    bus_write(0x02C6, cpu.A);
    LDA(0x26);
    bus_write(0x02C7, cpu.A);
    LDY(0x08);
L_3dca:
    LDA(mem[(0x4DF1)+cpu.Y]);
    mem[(0x00CF)+cpu.Y] = cpu.A;
    DEY();
    if (!cpu.N) goto L_3dca;
    LDA(0x80);
    LDY(0x03);
L_3dd7:
    DEY();
    mem[(0x0645)+cpu.Y] = cpu.A;
    if (!cpu.Z) goto L_3dd7;
    mem[0x007E] = cpu.A;
    mem[0x0668] = cpu.A;
    mem[0x0669] = cpu.A;
    startup_init();
    LDA(0xA0);
    mem[0x0663] = cpu.A;
    mem[0x066A] = cpu.A;
    mem[0x066B] = cpu.A;
    LDX(0x1F);
    input_init();
    INX();
    input_init();
    LDA(0x40);
    bus_write(0xD40E, cpu.A);
    LDA(0xC0);
    mem[0x0071] = cpu.A;
    LDA(0x0D);
    mem[0x3157] = cpu.A;
    LDA(0x35);
    mem[0x3158] = cpu.A;
L_3e0f:
    display_setup();
    LDA(0x2A);
    clear_pm_state();
    clear_colors();
    LDA(0x0D);
    mem[0x3157] = cpu.A;
    LDA(0x35);
    mem[0x3158] = cpu.A;
    LDA(0x00);
    mem[0x00B7] = cpu.A;
    mem[0x0005] = cpu.A;
    LDY(0x2C);
L_3e2c:
    DEY();
    mem[(0x0020)+cpu.Y] = cpu.A;
    if (!cpu.Z) goto L_3e2c;
    LDY(0xA6);
L_3e34:
    DEY();
    mem[(0x2830)+cpu.Y] = cpu.A;
    if (!cpu.Z) goto L_3e34;
    mem[0x060C] = cpu.A;
    init_terrain_render_buffers();
    fill_buffer2_region_ff();
    clear_terrain_lo_buffers();
    unpack_terrain_seed_cols();
    LDA(0x45);
    wait_vcount_eq();
    LDA(0xF5);
    bus_write(0x0222, cpu.A);
    LDA(0x4F);
    bus_write(0x0223, cpu.A);
    LDA(0x00);
    LDY(0x57);
L_3e5c:
    DEY();
    mem[(0x0B31)+cpu.Y] = cpu.A;
    if (!cpu.Z) goto L_3e5c;
    LDA(0x11);
    bus_write(0x026F, cpu.A);
    copy_terrain_seed_rows();
    wait_vcount_ge_7a();
    LDA(0xEE);
    bus_write(0x0200, cpu.A);
    LDA(0x49);
    bus_write(0x0201, cpu.A);
    LDA(0x6B);
    bus_write(0xD402, cpu.A);
    LDA(0x31);
    bus_write(0xD403, cpu.A);
    LDA(0x40);
    bus_write(0xD004, cpu.A);
    init_gameplay_state();
    LDA(mem[0x0627]);
    if (!cpu.Z) goto L_3e97;
    intro_random_setup();
    intro_unmark_random_cells();
    intro_seed_object_map();
L_3e97:
    LDA(0x60);
    mem[0x00C1] = cpu.A;
    LDA(0x10);
    mem[0x00C3] = cpu.A;
    LDA(0x10);
    mem[0x00C4] = cpu.A;
    build_row_addr_table();
    copy_row_addr_subset();
    LDA(mem[0x0004]);
    if (!cpu.Z) goto L_3eb6;
    LDA(0x54);
    mem[0x0044] = cpu.A;
    LDA(0x02);
    goto L_3eb8;
L_3eb6:
    LDA(0x01);
L_3eb8:
    mem[0x004A] = cpu.A;
L_3eba:
    ds_frame();
    terrain_frame_setup();
    LDX(0x33);
    clear_terrain_column();
    LDX(0x30);
    terrain_draw_frame();
    LDX(0x33);
    terrain_collision();
    LDA(mem[0x0041]);
    mem[0x288F] = cpu.A;
    game_state_update();
    LDA(0x02);
    mem[0x0042] = cpu.A;
    enemy_check();
    LDA(mem[0x062F]);
    CMP(0x0E);
    if (cpu.C) goto L_3ef5;
    LDA(mem[0x0004]);
    if (cpu.Z) goto L_3eec;
    game_sub_7B54();
    goto L_3ef5;
L_3eec:
    LDA(mem[0x003A]);
    CMP(0x01);
    if (!cpu.Z) goto L_3ef5;
    mem[0x2849] = cpu.A;
L_3ef5:
    terrain_frame_setup();
    LDX(0x03);
    clear_terrain_column();
    LDX(0x00);
    terrain_draw_frame();
    LDX(0x03);
    terrain_collision();
    LDA(mem[0x0041]);
    if (cpu.Z) goto L_3f0e;
    mem[0x288F] = cpu.A;
L_3f0e:
    game_state_update();
    enemy_check();
    LDA(mem[0x288E]);
    if (cpu.Z) goto L_3f21;
    LDA(mem[0x288D]);
    if (cpu.Z) goto L_3f21;
    pilot_render();
L_3f21:
    LDA(mem[0x288D]);
    mem[0x288E] = cpu.A;
    LDA(mem[0x288F]);
    if (!cpu.Z) goto L_3f31;
    LDA(mem[0x003E]);
    goto L_3f33;
L_3f31:
    LDA(0x00);
L_3f33:
    mem[0x288D] = cpu.A;
    LDA(0x01);
    mem[0x0042] = cpu.A;
    CMP(mem[0x003E]);
    if (cpu.Z) goto L_3f50;
    LDX(mem[0x0072]);
    CPX(0x02);
    if (cpu.Z) goto L_3f50;
    LDA(mem[0x003D]);
    if (cpu.Z) goto L_3f50;
    LDA(0x02);
    mem[0x003D] = cpu.A;
    LDY(0x0E);
    mem[0x0044] = cpu.Y;
L_3f50:
    LDX(mem[0x0072]);
    CPX(0x02);
    if (cpu.Z) goto L_3f59;
    goto L_3eba;
L_3f59:
    INX();
    mem[0x004A] = cpu.X;
    LDY(0x80);
    mem[0x28D9] = cpu.Y;
    mem[0x28DA] = cpu.Y;
    LDA(mem[0x0034]);
    CMP(0x40);
    if (cpu.C) goto L_3f6d;
    goto L_3eba;
L_3f6d:
    ds_frame();
    LDA(mem[0x283B]);
    if (!cpu.N) goto L_3f6d;
    mem[0x007E] = cpu.Y;
    LDA(0x00);
    mem[0x062B] = cpu.A;
    mem[0x0642] = cpu.A;
    wait_frames_10();
    mem[0x004A] = cpu.A;
    push_a_thunk_3cb2();
    LDY(0xA3);
L_3f86:
    mem[(0x0F1D)+cpu.Y] = cpu.A;
    DEY();
    if (!cpu.Z) goto L_3f86;
    LDY(0x1E);
L_3f8e:
    mem[(0x0E8F)+cpu.Y] = cpu.A;
    DEY();
    if (!cpu.N) goto L_3f8e;
    LDA(mem[0x00DE]);
    CMP(0x4E);
    if (!cpu.Z) goto L_3f9e;
    LDA(0x46);
    mem[0x00DE] = cpu.A;
L_3f9e:
    game_sub_4606();
    LDA(mem[0x00DD]);
    CMP(0x2B);
    if (!cpu.C) goto L_3fab;
    LDA(0x2A);
    mem[0x00DD] = cpu.A;
L_3fab:
    mem[0x0041] = cpu.A;
    mem[0x0071] = cpu.A;
    clear_pm_state();
    LDA(0x00);
    LDY(0x03);
    mem[(0x066B)+cpu.Y] = cpu.A;
    game_sub_55FC();
    goto L_3e0f;
}
