/* dot_table_test.c — host-side equivalence proof for the rasterizer DRAWDOT column tables
 * (RescueOnFractalus.cpp kDotColMask/kDotColOff, consumed by TerrainRasterizeAssembler.s).
 *
 * DRAWDOT used to derive the plane2 write from plotCol at run time: reject plotCol >= 208,
 * `_ac = plotCol - 48` (its borrow being the low-edge reject), `_ac >> 2` for the byte
 * offset and `$C0 >> (2*(_ac&3))` for the pixel mask — 13 instructions, one of them a
 * variable-count shift.  All three are pure functions of plotCol, so they fold into two
 * byte tables indexed by the RAW column, with mask==0 (impossible for a real 2-bit mask)
 * doubling as the off-viewport gate.
 *
 * This walks the ENTIRE input domain — every (plotCol, oldMax) byte pair, 65536 cases —
 * and diffs the resulting (byte offset, mask) against the C oracle macro ROF_PLOT_DOT in
 * rof_native.c, including which cases plot at all.  Build + run:
 *     cc -O2 -o /tmp/dottab tools/dot_table_test.c && /tmp/dottab
 */
#include <stdio.h>
#include <stdint.h>
static const uint8_t kColMask4[4] = { 0xC0u, 0x30u, 0x0Cu, 0x03u };
static uint16_t kRow120[47];
static uint16_t kDrawDotRowOff[256];
static uint8_t  kDotColMask[256], kDotColOff[256];
int main(void) {
    for (int r = 0; r < 47; r++) kRow120[r] = (uint16_t)(r * 120);
    for (int m = 0; m < 256; m++) { int sc = 150 - m;
        kDrawDotRowOff[m] = ((unsigned)sc < 47u && sc != 43) ? kRow120[sc] : 0xFFFF; }
    for (int c = 0; c < 256; c++) { int ac = c - 48;
        if ((unsigned)ac < 160u) { kDotColMask[c] = kColMask4[ac & 3]; kDotColOff[c] = (uint8_t)(ac >> 2); }
        else                     { kDotColMask[c] = 0;                 kDotColOff[c] = 0; } }
    long bad = 0, plots = 0;
    for (int col = 0; col < 256; col++) for (int oldMax = 0; oldMax < 256; oldMax++) {
        /* --- oracle: the C macro ROF_PLOT_DOT(col, oldMax) --- */
        int oi = -1; uint8_t om = 0;
        { int _ac = col - 48, _sc = 150 - oldMax;
          if ((unsigned)_ac < 160u && (unsigned)_sc < 47u && _sc != 43) {
              oi = kRow120[_sc] + (_ac >> 2); om = kColMask4[_ac & 3]; } }
        /* --- asm: kDrawDotRowOff sentinel, then the two column tables --- */
        int ai = -1; uint8_t am = 0;
        { uint16_t ro = kDrawDotRowOff[oldMax];
          if (!(ro & 0x8000u)) {                       /* bmi sentinel */
              uint8_t mask = kDotColMask[col];
              if (mask) { ai = ro + kDotColOff[col]; am = mask; } } }
        if (ai != oi || am != om) { if (++bad < 5)
            printf("MISMATCH col=%d oldMax=%d oracle=(%d,%02x) asm=(%d,%02x)\n", col, oldMax, oi, om, ai, am); }
        if (oi >= 0) plots++;
    }
    printf("%s: 65536 (col,oldMax) pairs, %ld plotting, %ld mismatches\n", bad ? "FAIL" : "PASS", plots, bad);
    /* also assert every offset stays inside one plane row (0..39) */
    for (int c = 48; c < 208; c++) if (kDotColOff[c] > 39) { printf("FAIL off range col=%d\n", c); return 1; }
    return bad != 0;
}
