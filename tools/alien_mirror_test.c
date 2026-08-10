/* alien_mirror_test — exhaustive host proof for the alien_shape_blit overlay-mirror restructure.
 *
 * The mirror maps each of the 17 consecutive field cells of one creature row to an (row, byteCol)
 * slot in the figure overlay.  The ORIGINAL does it per cell:
 *
 *     b = figB0 + y;  r = figR0;
 *     if (b >= 96) { b -= 96; r++; }              // at most one wrap
 *     if ((unsigned)r < 43 && (unsigned)b < 40) draw(r, b);
 *
 * The RESTRUCTURE claims the drawn cells always form ONE CONTIGUOUS y-window with a CONSTANT
 * row, so the row lookup, the two base offsets and the four extent updates can leave the loop:
 *
 *     figB0 >= 80 : row = figR0+1, off = figB0-96, window [96-figB0, 16]
 *     else        : row = figR0,   off = figB0,    window [max(0,-off), min(16, 39-off)]
 *     row out of [0,43) => empty window
 *     draw(row, y+off) for y in the window
 *
 * The claim rests on 40 <= figB0 <= 79 drawing NOTHING (b lands in [40,95], past the 40-byte
 * bitmap but short of the 96 wrap), which makes the two cases mutually exclusive.  Both figB0
 * and figR0 come from a SIGNED div/mod (rof_divs16/rof_mods16) of figBase = dstRow - 0x10A4, so
 * figB0 is negative for dstRow < 0x10A4 and the negative side has to be covered too.
 *
 * This walks EVERY dstRow (all 65536) x EVERY y (0..16) and compares the two, decision by
 * decision.  Build + run:  cc -O2 -o /tmp/amt tools/alien_mirror_test.c && /tmp/amt
 */
#include <stdio.h>
#include <stdint.h>

/* The Amiga build's 16-bit signed div/mod helpers (src/cpu/m68k_math.h) are plain C off-target:
   C99 truncating division, so the remainder takes the sign of the dividend. */
static int divs16(int a, int b) { return a / b; }
static int mods16(int a, int b) { return a % b; }

struct slot { int drawn, r, b; };

/* ---- ORIGINAL: per-cell wrap + two unsigned range tests ---- */
static struct slot orig(int figR0, int figB0, int y)
{
    struct slot s = { 0, 0, 0 };
    int b = figB0 + y, r = figR0;
    if (b >= 96) { b -= 96; r++; }
    if ((unsigned)r < 43u && (unsigned)b < 40u) { s.drawn = 1; s.r = r; s.b = b; }
    return s;
}

/* ---- RESTRUCTURED: one row + one contiguous window, computed once per row ---- */
static void window(int figR0, int figB0, int *row, int *off, int *lo, int *hi)
{
    if (figB0 >= 80) {                       /* the wrapped group */
        *row = figR0 + 1; *off = figB0 - 96;
        *lo  = 96 - figB0; *hi = 16;
    } else {                                 /* the unwrapped group (empty for figB0 in 40..79) */
        *row = figR0; *off = figB0;
        *lo  = (*off < 0) ? -*off : 0;
        *hi  = 39 - figB0;
        if (*hi > 16) *hi = 16;
    }
    if ((unsigned)*row >= 43u) { *lo = 1; *hi = 0; }   /* row off-bitmap => empty */
}

static struct slot fast(int figR0, int figB0, int y)
{
    struct slot s = { 0, 0, 0 };
    int row, off, lo, hi;
    window(figR0, figB0, &row, &off, &lo, &hi);
    if (y >= lo && y <= hi) { s.drawn = 1; s.r = row; s.b = y + off; }
    return s;
}

int main(void)
{
    long cases = 0, drawn = 0, bad = 0;
    int minB = 999, maxB = -999, minR = 9999, maxR = -9999;

    for (long dstRow = 0; dstRow <= 0xFFFF; dstRow++) {
        int figBase = (int)dstRow - 0x10A4;
        int figR0 = divs16(figBase, 96), figB0 = mods16(figBase, 96);

        /* the window is claimed constant across the row — compute it once and reuse */
        int row, off, lo, hi;
        window(figR0, figB0, &row, &off, &lo, &hi);

        for (int y = 0; y <= 16; y++) {
            struct slot a = orig(figR0, figB0, y);
            struct slot b = fast(figR0, figB0, y);
            cases++;
            if (a.drawn != b.drawn || (a.drawn && (a.r != b.r || a.b != b.b))) {
                if (bad < 10)
                    printf("MISMATCH dstRow=$%04lX figR0=%d figB0=%d y=%d : "
                           "orig(drawn=%d r=%d b=%d) fast(drawn=%d r=%d b=%d)\n",
                           dstRow, figR0, figB0, y,
                           a.drawn, a.r, a.b, b.drawn, b.r, b.b);
                bad++;
            }
            if (a.drawn) {
                drawn++;
                if (a.b < minB) minB = a.b;
                if (a.b > maxB) maxB = a.b;
                if (a.r < minR) minR = a.r;
                if (a.r > maxR) maxR = a.r;
                /* the window must also be a CONTIGUOUS run with a CONSTANT row */
                if (a.r != row || y < lo || y > hi) {
                    if (bad < 10)
                        printf("WINDOW BREAK dstRow=$%04lX y=%d r=%d (window row=%d [%d,%d])\n",
                               dstRow, y, a.r, row, lo, hi);
                    bad++;
                }
            }
        }
    }

    printf("cases=%ld  drawn=%ld  mismatches=%ld\n", cases, drawn, bad);
    printf("drawn ranges: r in [%d,%d]  b in [%d,%d]\n", minR, maxR, minB, maxB);
    return bad ? 1 : 0;
}
