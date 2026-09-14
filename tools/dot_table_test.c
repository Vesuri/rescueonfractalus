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
#include "../src/platform/amiga/FlightTerrainGeometry.h"
static const uint8_t kTerrainDotMask4[4] = { 0x80u, 0x20u, 0x08u, 0x02u };
static const uint8_t kColMask4[4] = { 0xC0u, 0x30u, 0x0Cu, 0x03u };
static uint16_t kRow120[ROF_FLIGHT_PHYSICAL_ROWS];
static uint16_t kDrawDotRowOff[256];
static uint8_t  kDotColMask[256], kDotColOff[256];
static uint8_t phase8(int value) { uint8_t p=(uint8_t)((unsigned)value*73u+41u); p^=(uint8_t)(p>>4); p=(uint8_t)((unsigned)p*29u); p^=(uint8_t)(p>>3); return p; }
int main(void) {
    for (int r = 0; r < ROF_FLIGHT_PHYSICAL_ROWS; r++) kRow120[r] = (uint16_t)(r * 120);
    long bad = 0, plots = 0;
    for (int enhanced = 0; enhanced <= 1; ++enhanced) {
        const int yScale = enhanced ? 2 : 1;
        for (int m = 0; m < 256; m++) { int sc = 150 - m;
            kDrawDotRowOff[m] = ((unsigned)sc < 47u && sc != 43)
                              ? kRow120[sc * yScale] : 0xFFFF; }
        for (int c = 0; c < 256; c++) { int ac = c - 48;
            if ((unsigned)ac < 160u) {
                uint8_t p = enhanced ? phase8(c) : 0;
                kDotColMask[c] = enhanced
                    ? (uint8_t)(kTerrainDotMask4[ac & 3] >> (p & 1u))
                    : kColMask4[ac & 3];
                kDotColOff[c] = (uint8_t)((ac >> 2) | ((p & 2u) ? 0x80u : 0u));
            } else { kDotColMask[c] = 0; kDotColOff[c] = 0; }
        }
        for (int col = 0; col < 256; col++) for (int oldMax = 0; oldMax < 256; oldMax++) {
            int oi = -1; uint8_t om = 0;
            { int ac = col - 48, sc = 150 - oldMax;
              if ((unsigned)ac < 160u && (unsigned)sc < 47u && sc != 43) {
                  uint8_t p = enhanced ? phase8(col) : 0;
                  oi = kRow120[sc * yScale + ((p >> 1) & 1u)] + (ac >> 2);
                  om = enhanced ? (uint8_t)(kTerrainDotMask4[ac & 3] >> (p & 1u))
                                : kColMask4[ac & 3]; } }
            int ai = -1; uint8_t am = 0;
            { uint16_t ro = kDrawDotRowOff[oldMax];
              if (!(ro & 0x8000u)) {
                  uint8_t mask = kDotColMask[col];
                  if (mask) { uint8_t co=kDotColOff[col]; ai=ro+(co&0x7f)+((co&0x80)?120:0); am=mask; } } }
            if (ai != oi || am != om) { if (++bad < 5)
                printf("MISMATCH mode=%d col=%d oldMax=%d oracle=(%d,%02x) asm=(%d,%02x)\n",
                       enhanced, col, oldMax, oi, om, ai, am); }
            if (oi >= 0) plots++;
        }
        for (int c = 48; c < 208; c++) if ((kDotColOff[c]&0x7f) > 39) {
            printf("FAIL off range mode=%d col=%d\n", enhanced, c); return 1; }
    }
    printf("%s: 131072 mode/(col,oldMax) cases, %ld plotting, %ld mismatches\n",
           bad ? "FAIL" : "PASS", plots, bad);
    if (kRow120[ROF_FLIGHT_PHYSICAL_ROWS - 1] + 39 >=
        ROF_FLIGHT_PHYSICAL_ROWS * ROF_FLIGHT_ROW_STRIDE) {
        printf("FAIL physical row offset range\n"); return 1;
    }
    return bad != 0;
}
