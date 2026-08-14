/* ras_fused_midpoint_test.c — exhaustive host proof for the constants the rasterizer's
 * FUSED span-5..8 blocks (TerrainRasterizeAssembler.s ras_s5/s6/s7/s8) bake in.
 *
 * Those blocks inline a far-bisect whose child span c = S>>1 is a COMPILE-TIME constant
 * (2/3/3/4), so the roughness displacement stops being computed from the span and becomes
 * an immediate.  The claim being proved here, over every reachable input, is:
 *
 *     generic(hsum, fsum, span)  ==  fused_c(hsum, fsum)      for c = span>>1
 *
 * where `generic` is a verbatim transcription of the C oracle's phase-2 far branch
 * (rof_native.c terrain_column_rasterize_core_c) and `fused_c` is what the asm emits:
 *
 *     no roughness  : mh = hsum >> 1
 *     roughness up  : mh = min(ceil(hsum/2) + (c>>1), 0xFF)          [addq + $FF clamp]
 *     roughness down: mh = max(ceil(hsum/2) - ((c-1)>>1) - 1, 0)     [subq + bcc + moveq]
 *
 * The `down` identity is the interesting one: the oracle computes
 *     t = havg + (uint8_t)~disp + (hsum & 1);  mh = (t > 0xFF) ? (uint8_t)t : 0
 * i.e. an 8-bit borrow test spelled as a 9-bit add.  With ceil = havg + (hsum&1) that is
 * exactly `ceil - (disp+1)` floored at 0 — which is why the asm can use subq/bcc.
 *
 * Domain: height and chgt are bytes, so hsum = 0..510; fsum is the 9-bit fraction sum
 * (0..511) and only its bits 7 and 8 select a branch, so all 512 values are enumerated.
 * That is 511*512 = 261632 cases per child span, all of them.
 *
 *   cc -O2 -o /tmp/rasfused tools/ras_fused_midpoint_test.c && /tmp/rasfused
 */
#include <stdio.h>
#include <stdint.h>

/* --- the C oracle's far-branch midpoint, verbatim (span -> child -> disp) --------------- */
static uint8_t generic_mid(unsigned hsum, unsigned fsum, unsigned span)
{
    /* mid = plotCol + (span>>1); the oracle takes disp from (mid - col) resp.
     * (mid - col - 1), and mid - col IS the child span. */
    const unsigned child = span >> 1;
    const unsigned havg  = hsum >> 1;
    uint8_t mh;
    if (!((uint8_t)fsum & 0x80)) {
        mh = (uint8_t)havg;                                  /* no roughness */
    } else if (fsum >= 0x100u) {                             /* roughness up */
        const uint8_t disp = (uint8_t)((uint8_t)child >> 1);
        unsigned t = havg + disp + (hsum & 1u);
        mh = (t > 0xFF) ? 0xFF : (uint8_t)t;
    } else {                                                 /* roughness down */
        const uint8_t disp = (uint8_t)((uint8_t)(child - 1u) >> 1);
        unsigned t = havg + (unsigned)(uint8_t)~disp + (hsum & 1u);
        mh = (t > 0xFF) ? (uint8_t)t : 0;
    }
    return mh;
}

/* --- what ras_s5/s6/s7/s8 (and ras_sp3/ras_sp4) actually execute ------------------------ */
static uint8_t fused_mid(unsigned hsum, unsigned fsum, unsigned up, unsigned dn1)
{
    unsigned d6 = hsum;                                      /* add.w d3,d6 */
    if (!((uint8_t)fsum & 0x80)) return (uint8_t)(d6 >> 1);  /* lsr.w #1,d6 */
    d6 = (d6 + 1) >> 1;                                      /* addq.w #1 ; lsr.w #1 = ceil */
    if (fsum >= 0x100u) {                                    /* btst #8 -> up */
        d6 += up;                                            /* addq.w #up,d6 */
        if (d6 > 0xFF) d6 = 0xFF;                            /* cmp.w #$FF ; bls ; move.w */
        return (uint8_t)d6;
    }
    if (d6 < dn1) return 0;                                  /* subq.w #dn1 ; bcc ; moveq #0 */
    return (uint8_t)(d6 - dn1);
}

int main(void)
{
    /* span, child span c, and the two immediates the asm block for it uses.
     * ras_sp3's inner midpoint (c == 1) and ras_sp4's (c == 2) are included as controls:
     * they are ALREADY shipping, so a failure there would mean this model is wrong, not
     * the new blocks. */
    static const struct { const char *name; unsigned span, up, dn1; } K[] = {
        { "ras_sp3 inner (c=1, control)", 3, 0, 1 },
        { "ras_sp4 inner (c=2, control)", 4, 1, 1 },
        { "ras_s5   head (c=2)",          5, 1, 1 },
        { "ras_s6   head (c=3)",          6, 1, 2 },
        { "ras_s7   head (c=3)",          7, 1, 2 },
        { "ras_s8   head (c=4)",          8, 2, 2 },
        { "ras_s8   child (c=2)",         4, 1, 1 },
        { "ras_s6/7 child (c=1)",         3, 0, 1 },
    };
    unsigned long total = 0, bad = 0;
    for (unsigned k = 0; k < sizeof K / sizeof K[0]; k++) {
        unsigned long kbad = 0, kcases = 0;
        for (unsigned hsum = 0; hsum <= 510; hsum++) {
            for (unsigned fsum = 0; fsum <= 511; fsum++) {
                uint8_t a = generic_mid(hsum, fsum, K[k].span);
                uint8_t b = fused_mid(hsum, fsum, K[k].up, K[k].dn1);
                kcases++;
                if (a != b) {
                    if (kbad == 0)
                        printf("  MISMATCH %s: hsum=%u fsum=%u generic=%u fused=%u\n",
                               K[k].name, hsum, fsum, a, b);
                    kbad++;
                }
            }
        }
        printf("%-30s span=%u up=+%u dn=-%u : %lu cases, %lu mismatches\n",
               K[k].name, K[k].span, K[k].up, K[k].dn1, kcases, kbad);
        total += kcases; bad += kbad;
    }
    printf("\nTOTAL %lu cases, %lu mismatches -> %s\n", total, bad, bad ? "FAIL" : "PASS");
    return bad != 0;
}
