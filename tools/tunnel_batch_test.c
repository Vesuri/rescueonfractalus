/* tunnel_batch_test.c — host-side equivalence proof for BATCHING the Amiga tunnel-ring painter.
 *
 * The launch and return-to-mother-ship tunnels paint straight into the bitplanes (no mem[] field
 * decode — docs/boost-tunnel-direct-handoff.md).  Until now the painter took ONE ITEM at a time:
 * one concentric rectangle OUTLINE per ROF_TUNNEL_RECT, and one vertical span PAIR per
 * ROF_TUNNEL_VSPAN.  Measured on an A500+, the outermost ring group (11 outlines up to 86 rows
 * tall) cost ~4 PAL frames inside the 50 Hz VBI ISR and the last reveal run 2.25 frames, which is
 * the "the drawing is clearly visible / noticeably jerky" report.
 *
 * The batching rests on two shape identities:
 *
 *   GROUP  — draw_symmetric_span_loop draws `count` outlines nested exactly one unit apart on all
 *            four sides, ALL IN ONE COLOUR, so their union is a solid rectangular ANNULUS (outer
 *            edge = the last outline, hole = the interior of the first).  Four boxes, not 4*count
 *            edges.
 *   RUN    — plot_terrain_span draws `count` vertical span pairs at a CONSTANT row window and
 *            colour, stepping only xL--/xR++, so the left nibbles are contiguous and so are the
 *            right ones.  Two boxes, not 2*count 4-px columns.
 *
 * Neither is provable by `make validate`: the mem[] plots are untouched (the 6502 side still runs
 * outline by outline), and Bitmap::fillColor is Amiga-only, so there is no 6502 oracle to diff
 * against.  This is that proof instead — OLD and NEW painters compiled side by side and run on
 * identical bitmaps:
 *
 *   OLD  — verbatim snapshots of Bitmap::fillColor + drawTunnelRect/paintVSpan/drawTunnelVSpan.
 *   NEW  — verbatim snapshots of drawTunnelGroup/paintTunnelBox/drawTunnelSpanRun.
 *
 * Coverage: both cinematics' REAL sequences (the $6E0F table, init_row_coords_9c's 46/48/43/42
 * seed, the forward descent's index 19->6 walk, the boost reverse's 19->0 walk, the pre-draw's
 * 20 groups with its colour cycle, and the two 20-call plot_terrain_span sequences) plus a
 * randomized sweep over the whole shape domain the loops can produce.
 *
 * DOMAIN, stated because the annulus identity needs it: rowTop >= rowBot, and the outermost
 * outline's four coordinates do not wrap uint8.  Both hold for every shape either cinematic
 * produces (43 nesting steps from 46/48/43/42 land exactly on 4/90/85/0); the shipping painter
 * still checks and falls back to the per-outline path, and the random sweep exercises that too.
 *
 * Build/run:  cc -O2 -o /tmp/tb tools/tunnel_batch_test.c && /tmp/tb
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* ---- the tunnel bitmap, as RescueOnFractalus allocates it ----------------------------------
 * 320x86, 3 bitplanes, INTERLEAVED: a row is 3*20 = 60 words, plane stride 20 words.  */
#define KW            320
#define KTERRAINHEIGHT 86
#define PLANES          3
#define ROWWORDS       (PLANES * (KW / 16))
#define BUFWORDS       (ROWWORDS * KTERRAINHEIGHT)

static uint16_t bufOld[BUFWORDS], bufNew[BUFWORDS];
static uint16_t* g_data;                 /* which buffer the painter under test writes */

/* ============================================================================================
 * VERBATIM SNAPSHOT — Bitmap::fillColor (src/platform/amiga/framework/Bitmap.cpp), specialised
 * to this bitmap's geometry (interleaved, 3 planes, 320x86).  Both painters share it: it is the
 * primitive, not the thing under test.
 * ==========================================================================================*/
static void fillColor(uint16_t x, uint16_t y, uint16_t fillWidth, uint16_t fillHeight,
                      uint16_t color)
{
    if (fillWidth == 0)  fillWidth = KW;
    if (fillHeight == 0) fillHeight = KTERRAINHEIGHT;

    uint16_t firstWord = x >> 4;
    uint16_t lastWord = (uint16_t)((x + fillWidth - 1) >> 4);
    uint16_t widthWords = (uint16_t)(lastWord - firstWord + 1);
    uint16_t rowWords = ROWWORDS;
    const uint16_t planeStride = KW / 16;

    uint16_t firstMask = (uint16_t)(0xffff >> (x & 15));
    uint16_t lastMask = (uint16_t)(0xffff << (15 - ((x + fillWidth - 1) & 15)));
    if (widthWords == 1) firstMask &= lastMask;
    const uint16_t keepFirst = (uint16_t)~firstMask;
    const uint16_t keepLast = (uint16_t)~lastMask;

    uint16_t planeVal[8];
    const uint16_t planeCount = PLANES;
    for (uint16_t k = 0; k < planeCount; k++)
        planeVal[k] = ((color >> k) & 1) ? (uint16_t)0xffff : (uint16_t)0x0000;

    uint16_t* rowStart = g_data + (unsigned)y * rowWords + firstWord;

    if (widthWords == 1 && planeCount == 3 && fillHeight > 1) {
        const uint16_t a0 = planeVal[0] ? (uint16_t)0xffff : keepFirst;
        const uint16_t a1 = planeVal[1] ? (uint16_t)0xffff : keepFirst;
        const uint16_t a2 = planeVal[2] ? (uint16_t)0xffff : keepFirst;
        const uint16_t o0 = (uint16_t)(planeVal[0] & firstMask);
        const uint16_t o1 = (uint16_t)(planeVal[1] & firstMask);
        const uint16_t o2 = (uint16_t)(planeVal[2] & firstMask);
        uint16_t* p0 = rowStart;
        uint16_t* p1 = p0 + planeStride;
        uint16_t* p2 = p1 + planeStride;
        for (uint16_t row = fillHeight; row != 0; row--) {
            *p0 = (uint16_t)((*p0 & a0) | o0);  p0 += rowWords;
            *p1 = (uint16_t)((*p1 & a1) | o1);  p1 += rowWords;
            *p2 = (uint16_t)((*p2 & a2) | o2);  p2 += rowWords;
        }
        return;
    }

    for (uint16_t row = 0; row < fillHeight; row++, rowStart += rowWords) {
        uint16_t* planePtr = rowStart;
        for (uint16_t k = 0; k < planeCount; k++, planePtr += planeStride) {
            const uint16_t v = planeVal[k];
            uint16_t* p = planePtr;
            if (widthWords == 1) {
                if (v) *p |= firstMask; else *p &= keepFirst;
                continue;
            }
            if (v) *p |= firstMask; else *p &= keepFirst;
            p++;
            for (uint16_t w = (uint16_t)(widthWords - 2); w != 0; w--) *p++ = v;
            if (v) *p |= lastMask; else *p &= keepLast;
        }
    }
}

/* ============================================================================================
 * OLD — the per-item painter (RescueOnFractalus.cpp, before the batching).
 * ==========================================================================================*/
static void oldPaintVSpan(uint8_t rowBot, uint8_t rowTop, uint8_t xL, uint8_t xR, uint16_t pen)
{
    if (rowBot > rowTop) return;
    const int kRows = KTERRAINHEIGHT;
    int vy = (int)rowBot, vh = (int)rowTop - (int)rowBot + 1;
    if (vy >= kRows) return;
    if (vy + vh > kRows) vh = kRows - vy;
    const int nib = (xL & 1) ? 4 : 0;             /* the quirk: xL's parity picks BOTH nibbles */
    const int cL = (int)(xL >> 1), cR = (int)(xR >> 1);
    if (cL >= 4 && cL <= 43)
        fillColor((uint16_t)((cL - 4) * 8 + nib), (uint16_t)vy, 4, (uint16_t)vh, pen);
    if (cR >= 4 && cR <= 43)
        fillColor((uint16_t)((cR - 4) * 8 + nib), (uint16_t)vy, 4, (uint16_t)vh, pen);
}

static void oldDrawTunnelRect(uint8_t rowTop, uint8_t rowBot, uint8_t xL, uint8_t xR,
                              uint8_t byteLo, uint8_t byteHi, uint16_t pen)
{
    const int kRows = KTERRAINHEIGHT;
    int bLo = (int)byteLo, bHi = (int)byteHi;
    if (bLo < 4) bLo = 4;
    if (bHi > 43) bHi = 43;
    if (bLo <= bHi) {
        const uint16_t hx = (uint16_t)((bLo - 4) * 8);
        const uint16_t hw = (uint16_t)((bHi - bLo + 1) * 8);
        if ((int)rowTop < kRows) fillColor(hx, rowTop, hw, 1, pen);
        if ((int)rowBot < kRows) fillColor(hx, rowBot, hw, 1, pen);
    }
    oldPaintVSpan(rowBot, rowTop, xL, xR, pen);
}

/* The whole group, the way draw_symmetric_span_loop used to emit it: one hook call per outline,
 * with the loop's own byteLo/byteHi nudge. */
static void oldGroup(uint8_t rowTop, uint8_t rowBot, uint8_t xL, uint8_t xR, uint8_t count,
                     uint16_t pen)
{
    uint8_t top = rowTop, bot = rowBot, l = xL, r = xR, n = count;
    for (;;) {
        uint8_t lo = (uint8_t)(l >> 1);
        uint8_t hi = (uint8_t)(r >> 1);
        if (r & 1) lo = (uint8_t)(lo + 1);
        else       hi = (uint8_t)(hi - 1);
        oldDrawTunnelRect(top, bot, l, r, lo, hi, pen);
        l = (uint8_t)(l - 1); r = (uint8_t)(r + 1);
        top = (uint8_t)(top + 1); bot = (uint8_t)(bot - 1);
        if ((uint8_t)(--n) == 0) break;
    }
}

/* The whole run, the way plot_terrain_span used to emit it: one hook call per pair. */
static void oldRun(uint8_t r0, uint8_t r1, uint8_t xL, uint8_t xR, uint8_t count, uint16_t pen)
{
    uint8_t l = xL, r = xR, n = count;
    do {
        oldPaintVSpan(r0, r1, l, r, pen);
        l = (uint8_t)(l - 1); r = (uint8_t)(r + 1);
    } while (--n != 0);
}

/* ============================================================================================
 * NEW — the batched painter (verbatim from RescueOnFractalus.cpp).
 * ==========================================================================================*/
static void newPaintTunnelBox(int x0, int x1, int y0, int y1, uint16_t pen)
{
    if (y0 < 0) y0 = 0;
    if (y1 > (int)KTERRAINHEIGHT - 1) y1 = (int)KTERRAINHEIGHT - 1;
    if (x0 < 0) x0 = 0;
    if (x1 > (int)KW - 1) x1 = (int)KW - 1;
    if (y1 < y0 || x1 < x0) return;
    fillColor((uint16_t)x0, (uint16_t)y0, (uint16_t)(x1 - x0 + 1), (uint16_t)(y1 - y0 + 1), pen);
}

static void newPaintTunnelOutline(uint8_t rowTop, uint8_t rowBot, uint8_t xL, uint8_t xR,
                                  uint16_t pen)
{
    int bLo = (int)(xL >> 1), bHi = (int)(xR >> 1);
    if (xR & 1) bLo++; else bHi--;
    if (bLo < 4) bLo = 4;
    if (bHi > 43) bHi = 43;
    if (bLo <= bHi) {
        const uint16_t hx = (uint16_t)((bLo - 4) * 8);
        const uint16_t hw = (uint16_t)((bHi - bLo + 1) * 8);
        if ((int)rowTop < (int)KTERRAINHEIGHT) fillColor(hx, rowTop, hw, 1, pen);
        if ((int)rowBot < (int)KTERRAINHEIGHT) fillColor(hx, rowBot, hw, 1, pen);
    }
    oldPaintVSpan(rowBot, rowTop, xL, xR, pen);   /* paintVSpan, shared verbatim */
}

static unsigned long g_newFallbackGroups, g_newFallbackRuns;

static void newDrawTunnelGroup(uint8_t rowTop, uint8_t rowBot, uint8_t xL, uint8_t xR,
                               uint8_t count, uint16_t pen)
{
    if (count == 0 || rowTop < rowBot) return;
    const int n = (int)count - 1;
    /* One outline is cheaper drawn as an outline (see the .cpp) — same pixels either way, which is
     * what the annulus/outline cases below prove against each other. */
    if (count == 1) { newPaintTunnelOutline(rowTop, rowBot, xL, xR, pen); return; }
    if ((int)xL - n < 0 || (int)xR + n > 255 || (int)rowBot - n < 0 || (int)rowTop + n > 255) {
        uint8_t oTop = rowTop, oBot = rowBot, oL = xL, oR = xR;
        g_newFallbackGroups++;
        for (int k = 0; k <= n; k++) {
            newPaintTunnelOutline(oTop, oBot, oL, oR, pen);
            oL--; oR++; oTop++; oBot--;
        }
        return;
    }
    const int rTopIn = (int)rowTop,        rTopOut = (int)rowTop + n;
    const int rBotIn = (int)rowBot,        rBotOut = (int)rowBot - n;
    const int xInL   = 4 * (int)xL - 32,   xInR    = 4 * (int)xR - 32 + 3;
    const int xOutL  = xInL - 4 * n,       xOutR   = xInR + 4 * n;

    newPaintTunnelBox(xOutL, xOutR, rTopIn,  rTopOut, pen);
    newPaintTunnelBox(xOutL, xOutR, rBotOut, rBotIn,  pen);
    if (rTopIn - 1 >= rBotIn + 1) {
        newPaintTunnelBox(xOutL,    xInL + 3, rBotIn + 1, rTopIn - 1, pen);
        newPaintTunnelBox(xInR - 3, xOutR,    rBotIn + 1, rTopIn - 1, pen);
    }
}

static void newDrawTunnelSpanRun(uint8_t r0, uint8_t r1, uint8_t xL, uint8_t xR, uint8_t count,
                                 uint16_t pen)
{
    if (count == 0 || r0 > r1) return;
    const int n = (int)count - 1;
    if (((xL ^ xR) & 1u) != 0u || (int)xL - n < 0 || (int)xR + n > 255) {
        uint8_t l = xL, r = xR;
        g_newFallbackRuns++;
        for (int k = 0; k <= n; k++) { oldPaintVSpan(r0, r1, l, r, pen); l--; r++; }
        return;
    }
    newPaintTunnelBox(4 * ((int)xL - n) - 32, 4 * (int)xL - 32 + 3,       r0, r1, pen);
    newPaintTunnelBox(4 * (int)xR - 32,       4 * ((int)xR + n) - 32 + 3, r0, r1, pen);
}

/* ============================================================================================
 * Harness
 * ==========================================================================================*/
/* frame_span_thickness_tbl $6E0F, entries 0..19 (dumped from rof.xex; sum = 43, which is what
 * makes the group counts self-checking). */
static const uint8_t kThick[20] = { 11,6,4,3,2,2,2,1,1,1,1,1,1,1,1,1,1,1,1,1 };

static unsigned long g_cases, g_bad;
static uint32_t g_rng = 0x13572468u;
static uint32_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }

/* One group, both painters, onto a common starting image; diff the whole bitmap. */
static void caseGroup(const char* what, uint8_t seed, uint8_t rowTop, uint8_t rowBot,
                      uint8_t xL, uint8_t xR, uint8_t count, uint16_t pen)
{
    memset(bufOld, seed, sizeof bufOld);
    memset(bufNew, seed, sizeof bufNew);
    g_data = bufOld; oldGroup(rowTop, rowBot, xL, xR, count, pen);
    g_data = bufNew; newDrawTunnelGroup(rowTop, rowBot, xL, xR, count, pen);
    g_cases++;
    if (memcmp(bufOld, bufNew, sizeof bufOld) != 0) {
        if (g_bad < 8) {
            unsigned w = 0; while (w < BUFWORDS && bufOld[w] == bufNew[w]) w++;
            printf("MISMATCH %s: top=%u bot=%u xL=%u xR=%u n=%u pen=%u seed=%02x"
                   "  first word %u row %u: old=%04x new=%04x\n",
                   what, rowTop, rowBot, xL, xR, count, pen, seed,
                   w, w / ROWWORDS, bufOld[w], bufNew[w]);
        }
        g_bad++;
    }
}

static void caseRun(const char* what, uint8_t seed, uint8_t r0, uint8_t r1, uint8_t xL,
                    uint8_t xR, uint8_t count, uint16_t pen)
{
    memset(bufOld, seed, sizeof bufOld);
    memset(bufNew, seed, sizeof bufNew);
    g_data = bufOld; oldRun(r0, r1, xL, xR, count, pen);
    g_data = bufNew; newDrawTunnelSpanRun(r0, r1, xL, xR, count, pen);
    g_cases++;
    if (memcmp(bufOld, bufNew, sizeof bufOld) != 0) {
        if (g_bad < 8) {
            unsigned w = 0; while (w < BUFWORDS && bufOld[w] == bufNew[w]) w++;
            printf("MISMATCH %s: r0=%u r1=%u xL=%u xR=%u n=%u pen=%u seed=%02x"
                   "  first word %u row %u: old=%04x new=%04x\n",
                   what, r0, r1, xL, xR, count, pen, seed,
                   w, w / ROWWORDS, bufOld[w], bufNew[w]);
        }
        g_bad++;
    }
}

int main(void)
{
    const uint8_t seeds[3] = { 0x00, 0xff, 0x5a };

    /* ---- 1. the STATIC PRE-DRAW: draw_frame_pattern_seq walks $6E0F index $13 -> 0 from the
     * init_row_coords_9c seed (xL=46 xR=48 top=43 bot=42), cycling the colour 1..6.  43 outlines
     * in 20 groups.  Run it as a sequence too (same buffer, all 20 groups) so a group's
     * interaction with its neighbours is covered, not just each group in isolation. */
    for (int si = 0; si < 3; si++) {
        uint8_t top = 43, bot = 42, l = 46, r = 48, colour = 1;
        memset(bufOld, seeds[si], sizeof bufOld);
        memset(bufNew, seeds[si], sizeof bufNew);
        for (int ring = 19; ring >= 0; ring--) {
            const uint8_t n = kThick[ring];
            const uint16_t pen = (uint16_t)(colour & 7);
            g_data = bufOld; oldGroup(top, bot, l, r, n, pen);
            g_data = bufNew; newDrawTunnelGroup(top, bot, l, r, n, pen);
            /* the loop's own coordinate step, n outlines' worth */
            top = (uint8_t)(top + n); bot = (uint8_t)(bot - n);
            l = (uint8_t)(l - n);     r = (uint8_t)(r + n);
            colour = (colour == 6) ? 1 : (uint8_t)(colour + 1);
        }
        g_cases++;
        if (memcmp(bufOld, bufNew, sizeof bufOld) != 0) {
            printf("MISMATCH pre-draw sequence, seed %02x\n", seeds[si]);
            g_bad++;
        }
    }

    /* ---- 2. the same 20 groups INDIVIDUALLY, so a failure names its group ---- */
    {
        uint8_t top = 43, bot = 42, l = 46, r = 48, colour = 1;
        for (int ring = 19; ring >= 0; ring--) {
            const uint8_t n = kThick[ring];
            for (int si = 0; si < 3; si++)
                caseGroup("pre-draw group", seeds[si], top, bot, l, r, n, (uint16_t)(colour & 7));
            top = (uint8_t)(top + n); bot = (uint8_t)(bot - n);
            l = (uint8_t)(l - n);     r = (uint8_t)(r + n);
            colour = (colour == 6) ? 1 : (uint8_t)(colour + 1);
        }
    }

    /* ---- 3. the FORWARD DESCENT: draw_ring_frame_step, $00A0 = 19 down to 6, thickness
     * $6E0F[$00A0], colour 0 (black = space).  15 outlines in 14 groups. ---- */
    {
        uint8_t top = 43, bot = 42, l = 46, r = 48;
        for (int a0 = 19; a0 >= 6; a0--) {
            const uint8_t n = kThick[a0];
            for (int si = 0; si < 3; si++)
                caseGroup("forward descent", seeds[si], top, bot, l, r, n, 0);
            top = (uint8_t)(top + n); bot = (uint8_t)(bot - n);
            l = (uint8_t)(l - n);     r = (uint8_t)(r + n);
        }
    }

    /* ---- 4. the BOOST REVERSE RING: step_accum_sub_7e, accumulator top byte 19 down to 0,
     * thickness $6E0F[a].  All 43 outlines, and the last group is the 11-outline outermost
     * one this whole change is about. ---- */
    {
        uint8_t top = 43, bot = 42, l = 46, r = 48;
        for (int a = 19; a >= 0; a--) {
            const uint8_t n = kThick[a];
            for (int si = 0; si < 3; si++)
                caseGroup("boost reverse", seeds[si], top, bot, l, r, n, 8);
            top = (uint8_t)(top + n); bot = (uint8_t)(bot - n);
            l = (uint8_t)(l - n);     r = (uint8_t)(r + n);
        }
    }

    /* ---- 5. plot_terrain_span, both of the boost's 20-call sequences: Y = $13 -> 0, count =
     * $6E0F[Y] (+2 when Y == 0), the row window shrinking by count each call, from the
     * init_row_coords_9c seed.  The erase pass runs at colour 8, the reveal cycles 1..6. ---- */
    for (int pass = 0; pass < 2; pass++) {
        uint8_t top = 43, bot = 42, l = 46, r = 48, colour = (pass == 0) ? 8 : 1;
        for (int y = 19; y >= 0; y--) {
            uint8_t n = kThick[y];
            if (y == 0) n = (uint8_t)(n + 2);
            for (int si = 0; si < 3; si++)
                caseRun(pass ? "reveal run" : "erase run", seeds[si], bot, top, l, r, n,
                        (uint16_t)(colour & 7));
            l = (uint8_t)(l - n); r = (uint8_t)(r + n);
            top = (uint8_t)(top + kThick[y]); bot = (uint8_t)(bot - kThick[y]);
            if (pass) colour = (colour == 6) ? 1 : (uint8_t)(colour + 1);
        }
    }

    /* The batching only pays if the REAL shapes take the batched path.  Every group and run
     * either cinematic produces must be inside the domain — if a coordinate wrapped, the painter
     * would silently drop to the per-outline path and this whole change would be a no-op on
     * target while still passing the equivalence check above. */
    if (g_newFallbackGroups != 0 || g_newFallbackRuns != 0) {
        printf("FAIL: a REAL cinematic shape took the fallback path "
               "(%lu groups, %lu runs) — the batching would not fire on target\n",
               g_newFallbackGroups, g_newFallbackRuns);
        g_bad++;
    }
    printf("real sequences: %lu cases, all on the batched path\n", g_cases);

    /* ---- 6. randomized sweep over the shape domain the loops can produce, PLUS the
     * out-of-domain shapes that must take the fallback (wrapping coordinates, mismatched
     * parity).  This is what proves the guards, not just the happy path. ---- */
    for (int i = 0; i < 40000; i++) {
        const uint8_t bot = (uint8_t)(rnd() % 90);
        const uint8_t top = (uint8_t)(bot + rnd() % 90);
        const uint8_t l = (uint8_t)(rnd() % 100);
        const uint8_t r = (uint8_t)(l + 2 + 2 * (rnd() % 24));   /* same parity, to the right */
        const uint8_t n = (uint8_t)(1 + rnd() % 14);
        const uint16_t pen = (uint16_t)(rnd() & 7);
        caseGroup("random group", (uint8_t)(rnd() & 0xff), top, bot, l, r, n, pen);
        caseRun("random run", (uint8_t)(rnd() & 0xff), bot, top, l, r, n, pen);
        /* odd-parity xR: the faithful nibble quirk makes the right run non-contiguous, so the
         * span-run painter must take its fallback and still match. */
        caseRun("random run odd", (uint8_t)(rnd() & 0xff), bot, top, l, (uint8_t)(r + 1), n, pen);
    }
    /* Deliberately wrapping shapes: xL/rowBot small enough that the outermost outline underflows. */
    for (int i = 0; i < 4000; i++) {
        const uint8_t bot = (uint8_t)(rnd() % 4);
        const uint8_t top = (uint8_t)(bot + rnd() % 90);
        const uint8_t l = (uint8_t)(rnd() % 6);
        const uint8_t r = (uint8_t)(l + 2 + 2 * (rnd() % 24));
        const uint8_t n = (uint8_t)(1 + rnd() % 14);
        const uint16_t pen = (uint16_t)(rnd() & 7);
        caseGroup("wrap group", (uint8_t)(rnd() & 0xff), top, bot, l, r, n, pen);
        caseRun("wrap run", (uint8_t)(rnd() & 0xff), bot, top, l, r, n, pen);
    }

    printf("%lu cases, %lu MISMATCHES  (fallbacks taken: %lu groups, %lu runs)\n",
           g_cases, g_bad, g_newFallbackGroups, g_newFallbackRuns);
    return g_bad ? 1 : 0;
}
