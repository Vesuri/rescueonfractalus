/* ras_restructure_test.c — host-side equivalence proof for the phase-2 restructure of
 * terrain_column_rasterize_core (the flight terrain silhouette tracer, 34.5% of the flight
 * frame).  Two implementations of the SAME algorithm run on identical inputs and every
 * observable is diffed:
 *
 *   ORACLE  — a verbatim copy of terrain_column_rasterize_core_c's AMIGA path
 *             (rof_native.c): control points in the $95/$EA/$F4 byte stacks indexed by
 *             `depth`, the gap==$FE/$FF/far dispatch, ROF_FIELD_PLOT a no-op.
 *   NEW     — the restructure:
 *             (1) the control point's COLUMN is replaced by a tracked `span`
 *                 (= ctlCol - plotCol).  child span = span>>1, the parent's post-child
 *                 span = span - (span>>1), disp_up = (span>>1)>>1,
 *                 disp_down = ((span>>1)-1)>>1 — so no column is ever stored, loaded or
 *                 subtracted, and the midpoint column is never materialised.
 *             (2) the TOS control-point HEIGHT lives in a register (chgt); the pushed
 *                 midpoint height is simply left there, the parent's is spilled.
 *             (3) `r = hsum&1` folded into ceil(hsum/2) = (hsum+1)>>1.
 *             (4) the plotCol >= $D4 bound and `col = plotCol` move OUT of the loop head
 *                 into the leaf handlers (the only places plotCol changes).
 *             (5) spans 3 and 4 are straight-line blocks: no push, no pop, no dispatch
 *                 (47.7% of all far-bisects, 98.6% of ff leaves and 86% of fe leaves,
 *                 measured on a real flight).
 *
 * Build/run:  cc -O2 -o /tmp/rast tools/ras_restructure_test.c && /tmp/rast
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ---- the modelled machine state (everything the Amiga path can observe) --------------- */
#define NCOL 256
#define DOTROWS 48
#define DOTSTRIDE 120
#define DOTSZ (DOTROWS * DOTSTRIDE)

typedef struct {
    uint8_t colMax[NCOL];       /* $260E[]                      */
    uint8_t dot[DOTSZ];         /* g_flightDotPlane             */
    uint8_t wbCol, wbHgt, wbFrac; /* $82 / $84 / $86 writeback  */
    unsigned long draws, plots;  /* shape counters (not compared, just reported) */
} State;

static uint16_t kRow120[48];
static const uint8_t kColMask4[4] = { 0xC0, 0x30, 0x0C, 0x03 };
static void tables_init(void) { for (int r = 0; r < 48; r++) kRow120[r] = (uint16_t)(r * 120); }

#define PLOT_DOT(S, col, h) do { \
    int _ac = (int)(col) - 48; \
    int _sc = 150 - (int)(h); \
    if ((unsigned)_ac < 160u && (unsigned)_sc < 47u && _sc != 43) \
        (S)->dot[kRow120[_sc] + (_ac >> 2)] |= kColMask4[_ac & 3]; \
} while (0)

/* ====================================================================================== */
/* ORACLE — verbatim structure of terrain_column_rasterize_core_c (Amiga path)             */
/* ====================================================================================== */
static void ras_oracle(State *S, uint8_t inCol, uint8_t inHgt, uint8_t inFrac,
                       uint8_t ctl0Col, uint8_t ctl0Hgt, uint8_t ctl0Frac)
{
    uint8_t CTLC[64], CTLH[64], CTLF[64];
    uint8_t col = inCol, height = inHgt, frac = inFrac;
    int depth = 0;
    uint8_t plotCol = 0;
    CTLC[0] = ctl0Col; CTLH[0] = ctl0Hgt; CTLF[0] = ctl0Frac;

#define O_WB() do { S->wbCol = col; S->wbHgt = height; S->wbFrac = frac; } while (0)
#define O_DRAW(hh) do { uint8_t _h = (hh); S->draws++; \
    if (_h > S->colMax[plotCol]) { \
        uint8_t _oldMax = S->colMax[plotCol]; \
        S->colMax[plotCol] = _h; \
        if (_h >= 0x97) { S->colMax[plotCol] = 0xFF; _h = 0x97; } \
        S->plots++; \
        PLOT_DOT(S, plotCol, _oldMax); \
    } } while (0)

    const uint8_t endCol = CTLC[0];
    if (endCol < 0x2D) { O_WB(); return; }
    if (endCol < col)  { O_WB(); return; }
    if (endCol == col) { plotCol = endCol; O_DRAW(CTLH[0]); O_WB(); return; }

    /* phase 1 — left clip */
    depth = 0;
    for (;;) {
        if (col >= 0x2C) { plotCol = col; break; }
        const uint8_t mid   = (uint8_t)(((unsigned)col + CTLC[depth]) >> 1);
        const unsigned fsum = (unsigned)frac + CTLF[depth] + 1u;
        const unsigned havg = ((unsigned)height + CTLH[depth]) >> 1;
        if (mid <= 0x2C) {
            col  = mid;
            frac = (uint8_t)fsum;
            if (!(frac & 0x80)) height = (uint8_t)havg;
            else {
                const uint8_t disp = (uint8_t)((uint8_t)(CTLC[depth] - col) >> 1);
                if (fsum >= 0x100u) { unsigned t = havg + disp; height = (t > 0xFF) ? 0xFF : (uint8_t)t; }
                else height = (havg >= disp) ? (uint8_t)(havg - disp) : 0;
            }
        } else {
            CTLC[depth + 1]  = mid;
            CTLF[depth + 1] = (uint8_t)fsum;
            uint8_t mh;
            if (!((uint8_t)fsum & 0x80)) mh = (uint8_t)havg;
            else {
                const uint8_t disp = (uint8_t)((uint8_t)(mid - col) >> 1);
                if (fsum >= 0x100u) { unsigned t = havg + disp; mh = (t > 0xFF) ? 0xFF : (uint8_t)t; }
                else mh = (havg >= disp) ? (uint8_t)(havg - disp) : 0;
            }
            CTLH[depth + 1] = mh;
            depth++;
        }
    }

    /* phase 2 — trace */
    for (;;) {
        if (plotCol >= 0xD4) { O_WB(); return; }
        col = plotCol;
        const uint8_t gap = (uint8_t)(plotCol - CTLC[depth]);
        if (gap == 0xFE) {
            O_DRAW((uint8_t)(((unsigned)CTLH[depth] + height + 1u) >> 1));
            plotCol++;
            height = CTLH[depth];
            O_DRAW(height);
            plotCol++;
            if (depth-- == 0) { O_WB(); return; }
            frac = CTLF[depth + 1];
        } else if (gap == 0xFF) {
            height = CTLH[depth];
            O_DRAW(height);
            plotCol++;
            if (depth-- == 0) { O_WB(); return; }
            frac = CTLF[depth + 1];
        } else {
            const uint8_t mid   = (uint8_t)(((unsigned)plotCol + CTLC[depth]) >> 1);
            CTLC[depth + 1]  = mid;
            const unsigned fsum = (unsigned)frac + CTLF[depth] + 1u;
            CTLF[depth + 1] = (uint8_t)fsum;
            const unsigned hsum = (unsigned)height + CTLH[depth];
            const unsigned havg = hsum >> 1;
            uint8_t mh;
            if (!((uint8_t)fsum & 0x80)) mh = (uint8_t)havg;
            else if (fsum >= 0x100u) {
                const uint8_t disp = (uint8_t)((uint8_t)(mid - col) >> 1);
                unsigned t = havg + disp + (hsum & 1u);
                mh = (t > 0xFF) ? 0xFF : (uint8_t)t;
            } else {
                const uint8_t disp = (uint8_t)((uint8_t)(mid - col - 1u) >> 1);
                unsigned t = havg + (unsigned)(uint8_t)~disp + (hsum & 1u);
                mh = (t > 0xFF) ? (uint8_t)t : 0;
            }
            CTLH[depth + 1] = mh;
            depth++;
        }
    }
#undef O_WB
#undef O_DRAW
}

/* ====================================================================================== */
/* NEW — the restructure (this is the exact shape the m68k asm will mirror)                */
/* ====================================================================================== */
/* Stack slot per depth: postSpan (the span left for this node AFTER its child's subtree),
 * hgt, frac.  In the asm these are one interleaved 3-byte-stride buffer walked by a3 +/-3;
 * `span` / `chgt` are the register-resident top of stack. */
static void ras_new(State *S, uint8_t inCol, uint8_t inHgt, uint8_t inFrac,
                    uint8_t ctl0Col, uint8_t ctl0Hgt, uint8_t ctl0Frac)
{
    uint8_t stkSpan[64], stkHgt[64], stkFrac[64];
    uint8_t col = inCol, height = inHgt, frac = inFrac;
    int depth = 0;
    uint8_t plotCol = 0;
    uint8_t span = 0, chgt = ctl0Hgt;          /* register-resident TOS */
    stkHgt[0] = ctl0Hgt; stkFrac[0] = ctl0Frac;

#define N_WB() do { S->wbCol = col; S->wbHgt = height; S->wbFrac = frac; } while (0)
#define N_DRAW(hh) do { uint8_t _h = (hh); S->draws++; \
    if (_h > S->colMax[plotCol]) { \
        uint8_t _oldMax = S->colMax[plotCol]; \
        S->colMax[plotCol] = (_h >= 0x97) ? 0xFF : _h; \
        S->plots++; \
        PLOT_DOT(S, plotCol, _oldMax); \
    } } while (0)
/* pop to the parent: frac comes from the slot we are LEAVING, span/chgt from the parent. */
#define N_POP() do { frac = stkFrac[depth]; depth--; span = stkSpan[depth]; chgt = stkHgt[depth]; } while (0)

    const uint8_t endCol = ctl0Col;
    if (endCol < 0x2D) { N_WB(); return; }
    if (endCol < col)  { N_WB(); return; }
    if (endCol == col) { plotCol = endCol; N_DRAW(chgt); N_WB(); return; }

    /* ---- phase 1 — left clip, span-tracked.  curSpan = ctlCol[depth] - col. ---------- */
    span = (uint8_t)(endCol - col);            /* endCol > col here, so this is exact */
    for (;;) {
        if (col >= 0x2C) { plotCol = col; break; }
        const uint8_t child = (uint8_t)(span >> 1);       /* mid - col */
        const uint8_t mid   = (uint8_t)(col + child);
        const unsigned fsum = (unsigned)frac + stkFrac[depth] + 1u;
        const unsigned havg = ((unsigned)height + chgt) >> 1;
        if (mid <= 0x2C) {                               /* advance the cursor onto mid */
            const uint8_t rest = (uint8_t)(span - child);/* ctlCol - mid = ceil(span/2) */
            col  = mid;
            frac = (uint8_t)fsum;
            span = rest;
            if (!(frac & 0x80)) height = (uint8_t)havg;
            else {
                const uint8_t disp = (uint8_t)(rest >> 1);
                if (fsum >= 0x100u) { unsigned t = havg + disp; height = (t > 0xFF) ? 0xFF : (uint8_t)t; }
                else height = (havg >= disp) ? (uint8_t)(havg - disp) : 0;
            }
        } else {                                         /* push mid as the new TOS */
            uint8_t mh;
            if (!((uint8_t)fsum & 0x80)) mh = (uint8_t)havg;
            else {
                const uint8_t disp = (uint8_t)(child >> 1);
                if (fsum >= 0x100u) { unsigned t = havg + disp; mh = (t > 0xFF) ? 0xFF : (uint8_t)t; }
                else mh = (havg >= disp) ? (uint8_t)(havg - disp) : 0;
            }
            stkSpan[depth] = (uint8_t)(span - child);    /* this node's post-child span */
            stkHgt[depth]  = chgt;                       /* spill the parent's height   */
            depth++;
            stkFrac[depth] = (uint8_t)fsum;
            span = child;
            chgt = mh;
        }
    }

    /* ---- phase 2 — trace.  `span` = ctlCol - plotCol (>= 1 always). ------------------ */
    if (plotCol >= 0xD4) { N_WB(); return; }
    for (;;) {
        if (span > 4) {
            /* ---- generic far bisect: push the interpolated midpoint --------------- */
            const uint8_t child = (uint8_t)(span >> 1);
            const unsigned fsum = (unsigned)frac + stkFrac[depth] + 1u;
            const unsigned hsum = (unsigned)height + chgt;
            uint8_t mh;
            if (!((uint8_t)fsum & 0x80)) mh = (uint8_t)(hsum >> 1);
            else {
                const unsigned hc = (hsum + 1u) >> 1;    /* havg + (hsum&1) */
                if (fsum >= 0x100u) {
                    unsigned t = hc + (unsigned)(child >> 1);
                    mh = (t > 0xFF) ? 0xFF : (uint8_t)t;
                } else {
                    unsigned t = hc + (unsigned)(uint8_t)~(uint8_t)((child - 1u) >> 1);
                    mh = (t > 0xFF) ? (uint8_t)t : 0;
                }
            }
            stkSpan[depth] = (uint8_t)(span - child);
            stkHgt[depth]  = chgt;
            depth++;
            stkFrac[depth] = (uint8_t)fsum;
            span = child;
            chgt = mh;
            continue;
        }
        if (span == 4) {
            /* ---- span 4 = far(child 2) + fe(child) + fe(parent): 4 columns ---------
             * child span 2 -> disp_up = 2>>1 = 1, disp_down = (2-1)>>1 = 0. */
            const unsigned fsum = (unsigned)frac + stkFrac[depth] + 1u;
            const unsigned hsum = (unsigned)height + chgt;
            uint8_t mh;
            if (!((uint8_t)fsum & 0x80)) mh = (uint8_t)(hsum >> 1);
            else {
                const unsigned hc = (hsum + 1u) >> 1;
                if (fsum >= 0x100u) { unsigned t = hc + 1u; mh = (t > 0xFF) ? 0xFF : (uint8_t)t; }
                else { unsigned t = hc + 0xFFu; mh = (t > 0xFF) ? (uint8_t)t : 0; }
            }
            /* child (span 2) fe */
            col = plotCol;
            N_DRAW((uint8_t)(((unsigned)mh + height + 1u) >> 1));
            plotCol++;
            height = mh;
            N_DRAW(mh);
            plotCol++;
            frac = (uint8_t)fsum;
            if (plotCol >= 0xD4) { N_WB(); return; }
            /* parent (span 2) fe */
            col = plotCol;
            N_DRAW((uint8_t)(((unsigned)chgt + height + 1u) >> 1));
            plotCol++;
            height = chgt;
            N_DRAW(height);
            plotCol++;
            if (depth == 0) { N_WB(); return; }
            N_POP();
            if (plotCol >= 0xD4) { N_WB(); return; }
            continue;
        }
        if (span == 3) {
            /* ---- span 3 = far(child 1) + ff(child) + fe(parent): 3 columns ---------
             * child span 1 -> disp_up = 1>>1 = 0, disp_down = (1-1)>>1 = 0. */
            const unsigned fsum = (unsigned)frac + stkFrac[depth] + 1u;
            const unsigned hsum = (unsigned)height + chgt;
            uint8_t mh;
            if (!((uint8_t)fsum & 0x80)) mh = (uint8_t)(hsum >> 1);
            else {
                const unsigned hc = (hsum + 1u) >> 1;
                if (fsum >= 0x100u) mh = (uint8_t)hc;          /* hc <= 255, no clamp */
                else { unsigned t = hc + 0xFFu; mh = (t > 0xFF) ? (uint8_t)t : 0; }
            }
            /* child (span 1) ff */
            col = plotCol;
            height = mh;
            N_DRAW(mh);
            plotCol++;
            frac = (uint8_t)fsum;
            if (plotCol >= 0xD4) { N_WB(); return; }
            /* parent (span 2) fe */
            col = plotCol;
            N_DRAW((uint8_t)(((unsigned)chgt + height + 1u) >> 1));
            plotCol++;
            height = chgt;
            N_DRAW(height);
            plotCol++;
            if (depth == 0) { N_WB(); return; }
            N_POP();
            if (plotCol >= 0xD4) { N_WB(); return; }
            continue;
        }
        if (span == 2) {
            col = plotCol;
            N_DRAW((uint8_t)(((unsigned)chgt + height + 1u) >> 1));
            plotCol++;
            height = chgt;
            N_DRAW(height);
            plotCol++;
            if (depth == 0) { N_WB(); return; }
            N_POP();
            if (plotCol >= 0xD4) { N_WB(); return; }
            continue;
        }
        /* span == 1 */
        col = plotCol;
        height = chgt;
        N_DRAW(height);
        plotCol++;
        if (depth == 0) { N_WB(); return; }
        N_POP();
        if (plotCol >= 0xD4) { N_WB(); return; }
    }
#undef N_WB
#undef N_DRAW
#undef N_POP
}

/* ====================================================================================== */
/* driver                                                                                 */
/* ====================================================================================== */
static unsigned long rng_s = 12345;
static unsigned rnd(void) { rng_s = rng_s * 6364136223846793005ULL + 1442695040888963407ULL;
                            return (unsigned)(rng_s >> 33); }

static void seed_colmax(State *S, int mode)
{
    for (int c = 0; c < NCOL; c++) {
        uint8_t v;
        switch (mode) {
            case 0:  v = (c < 0x40 || c >= 0xC0) ? 0x67 : 0x6B; break;      /* fresh frame */
            case 1:  v = (uint8_t)(0x60 + (rnd() % 0x40)); break;           /* partly drawn */
            case 2:  v = (rnd() % 8) ? (uint8_t)(0x60 + (rnd() % 0x38)) : 0xFF; break;
            default: v = (uint8_t)rnd(); break;                              /* adversarial */
        }
        S->colMax[c] = v;
    }
    memset(S->dot, 0, sizeof S->dot);
    S->wbCol = S->wbHgt = S->wbFrac = 0;
    S->draws = S->plots = 0;
}

int main(void)
{
    tables_init();
    static State A, B;
    unsigned long cases = 0, bad = 0, drawTot = 0, plotTot = 0;

    for (int pass = 0; pass < 4; pass++) {
        /* pass 0: realistic (span 1..64, col in view, fresh/partial colMax)
           pass 1: phase-1 heavy (col < $2C)
           pass 2: right-edge heavy (endCol near/past $D4)
           pass 3: fully adversarial (any byte anywhere) */
        for (int i = 0; i < 400000; i++) {
            uint8_t col, endCol, h0, h1, f0, f1;
            switch (pass) {
                case 0:
                    col = (uint8_t)(0x2C + rnd() % 0x90);
                    endCol = (uint8_t)(col + 1 + rnd() % 64);
                    break;
                case 1:
                    col = (uint8_t)(rnd() % 0x2C);
                    endCol = (uint8_t)(0x2D + rnd() % 0xC0);
                    break;
                case 2:
                    col = (uint8_t)(0xB0 + rnd() % 0x20);
                    endCol = (uint8_t)(col + 1 + rnd() % 0x40);
                    break;
                default:
                    col = (uint8_t)rnd(); endCol = (uint8_t)rnd();
                    break;
            }
            h0 = (uint8_t)(rnd() % 0x100); h1 = (uint8_t)(rnd() % 0x100);
            f0 = (uint8_t)rnd(); f1 = (uint8_t)rnd();
            int cmMode = (pass == 3) ? 3 : (int)(rnd() % 3);

            /* Guard against the algorithm's own non-termination cases (span 0 in phase 2 /
               span 1 stuck in phase 1) which are unreachable from the real caller: the
               oracle would spin forever, so skip those inputs. */
            if (endCol > col && (uint8_t)(endCol - col) == 1 && col < 0x2C) continue;

            seed_colmax(&A, cmMode);
            memcpy(&B, &A, sizeof A);
            ras_oracle(&A, col, h0, f0, endCol, h1, f1);
            ras_new   (&B, col, h0, f0, endCol, h1, f1);
            cases++; drawTot += A.draws; plotTot += A.plots;

            int diff = 0;
            if (memcmp(A.colMax, B.colMax, NCOL) != 0) diff |= 1;
            if (memcmp(A.dot, B.dot, DOTSZ) != 0)       diff |= 2;
            if (A.wbCol != B.wbCol || A.wbHgt != B.wbHgt || A.wbFrac != B.wbFrac) diff |= 4;
            if (A.draws != B.draws || A.plots != B.plots) diff |= 8;
            if (diff) {
                if (bad < 8)
                    printf("MISMATCH pass=%d diff=%d col=%02x end=%02x h0=%02x h1=%02x f0=%02x f1=%02x cm=%d\n"
                           "   wb oracle=%02x/%02x/%02x new=%02x/%02x/%02x  draws %lu/%lu plots %lu/%lu\n",
                           pass, diff, col, endCol, h0, h1, f0, f1, cmMode,
                           A.wbCol, A.wbHgt, A.wbFrac, B.wbCol, B.wbHgt, B.wbFrac,
                           A.draws, B.draws, A.plots, B.plots);
                bad++;
            }
        }
        printf("pass %d done: cases=%lu bad=%lu\n", pass, cases, bad);
    }
    printf("\n%lu cases, %lu MISMATCHES  (draws=%lu plots=%lu, accept=%.1f%%)\n",
           cases, bad, drawTot, plotTot, 100.0 * (double)plotTot / (double)drawTot);
    return bad ? 1 : 0;
}
