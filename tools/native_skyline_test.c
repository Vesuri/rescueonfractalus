/* Equivalence proof for the fused 320-column skyline edge plot.
 *
 * The three-pass body below is a VERBATIM snapshot of edgePlotEnhanced as it stood before the
 * fusion: pass 1 wrote the even entries of a 320-byte skyline field, pass 2 filled the odd ones
 * by midpoint displacement, pass 3 walked the field and ORed one plane-1 bit per physical column.
 * The shipping body computes the same two rows per source column in ONE pass, keeps the four
 * height samples both stages need in a sliding window, and plots as it produces.  Both are run
 * over the same height rows and the whole plane-1 plane is compared byte for byte.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../src/platform/amiga/FlightTerrainGeometry.h"

#define ROWS   ROF_FLIGHT_PHYSICAL_ROWS
#define STRIDE ROF_FLIGHT_ROW_STRIDE
#define SRCW   ROF_FLIGHT_SOURCE_WIDTH
#define PHYSW  ROF_FLIGHT_PHYSICAL_WIDTH
#define PLANE  (ROWS * STRIDE)

static uint16_t kRow120[ROWS];
static uint8_t  kHeightPhysicalRow[256];
static const uint8_t kPixelMask8[8] = { 0x80u,0x40u,0x20u,0x10u,0x08u,0x04u,0x02u,0x01u };
static const uint8_t kColMask4[4]   = { 0xC0u,0x30u,0x0Cu,0x03u };

static void build_tables(void)
{
    for (int r = 0; r < ROWS; ++r) kRow120[r] = (uint16_t)(r * STRIDE);
    for (int h = 0; h < 256; ++h) {
        int scan = 150 - h;
        if (scan < 0) scan = 0; else if (scan > 46) scan = 46;
        kHeightPhysicalRow[h] = (uint8_t)(scan * ROF_FLIGHT_SOURCE_TO_PHYSICAL_Y);
    }
    kHeightPhysicalRow[0xFF] = 0xFF;
}

/* ---------------- the three-pass snapshot ---------------- */

static uint8_t s_nativeSkylineRow[PHYSW];

static int old_midpoint(int c, const uint8_t* y, int r0, int r1)
{
    const int cp = c ? c - 1 : c;
    const int cn = c + 1 < SRCW ? c + 1 : c;
    const int cnn = cn + 1 < SRCW ? cn + 1 : cn;
    const uint8_t hp = y[cp], hn = y[cnn];
    int row = (r0 + r1 + 1) >> 1;
    if (hp != 0xFFu && hn != 0xFFu) {
        int d0 = (int)y[c] - (int)hp;
        int d1 = (int)y[cn] - (int)hn;
        int dr = r0 - r1;
        if (d0 < 0) d0 = -d0;
        if (d1 < 0) d1 = -d1;
        if (dr < 0) dr = -dr;
        unsigned rough = (unsigned)(d0 + d1 + dr);
        if (rough >= 3u) {
            unsigned phase = (unsigned)(c + 48) * 0x9E37u;
            phase ^= (unsigned)y[c] * 0x45D9u;
            phase ^= (unsigned)y[cn] * 0x119Du;
            phase ^= phase >> 7;
            row += (phase & 1u) ? 1 : -1;
        }
    }
    if (row < 0) row = 0;
    if (row >= ROWS) row = ROWS - 1;
    return row;
}

static void edge_plot_old(uint8_t* bp, const uint8_t* y)
{
    for (int c = 0; c < SRCW; ++c) {
        const uint8_t h = y[c];
        const int x = c * 2;
        if (h == 0xFFu) s_nativeSkylineRow[x] = 0xFFu;
        else {
            unsigned row = kHeightPhysicalRow[h];
            const uint8_t hp = y[c ? c - 1 : c];
            const uint8_t hn = y[c + 1 < SRCW ? c + 1 : c];
            if (hp != 0xFFu && hn != 0xFFu && row + 1u < (unsigned)ROWS &&
                (((unsigned)hp + hn) & 1u)) ++row;
            s_nativeSkylineRow[x] = (uint8_t)row;
        }
    }
    for (int c = 0; c < SRCW; ++c) {
        const int x = c * 2;
        const int nx = (c + 1 < SRCW) ? x + 2 : x;
        const uint8_t r0 = s_nativeSkylineRow[x], r1 = s_nativeSkylineRow[nx];
        s_nativeSkylineRow[x + 1] = (r0 == 0xFFu || r1 == 0xFFu)
            ? 0xFFu : (uint8_t)old_midpoint(c, y, r0, r1);
    }
    for (int x = 0; x < PHYSW; ++x) {
        const uint8_t row = s_nativeSkylineRow[x];
        if (row != 0xFFu)
            bp[kRow120[row] + (x >> 3)] |= kPixelMask8[x & 7];
    }
}

/* ---------------- the fused body ---------------- */

static const uint8_t kSkyEvenMask4[4] = { 0x80u, 0x20u, 0x08u, 0x02u };
static const uint8_t kSkyOddMask4[4]  = { 0x40u, 0x10u, 0x04u, 0x01u };

static inline int sky_even_row(int hp, int h, int hn)
{
    if (h == 0xFF) return 0xFF;
    int row = kHeightPhysicalRow[h];
    if (hp != 0xFF && hn != 0xFF && row + 1 < ROWS && ((hp + hn) & 1)) ++row;
    return row;
}

static int sky_mid_row(int c, int hp, int h, int hn, int hnn, int r0, int r1)
{
    int row = (r0 + r1 + 1) >> 1;
    if (hp != 0xFF && hnn != 0xFF) {
        int d0 = h - hp, d1 = hn - hnn, dr = r0 - r1;
        if (d0 < 0) d0 = -d0;
        if (d1 < 0) d1 = -d1;
        if (dr < 0) dr = -dr;
        if (d0 + d1 + dr >= 3) {
            unsigned phase = (unsigned)(c + 48) * 0x9E37u;
            phase ^= (unsigned)h * 0x45D9u;
            phase ^= (unsigned)hn * 0x119Du;
            phase ^= phase >> 7;
            row += (phase & 1u) ? 1 : -1;
        }
    }
    if (row < 0) row = 0;
    else if (row >= ROWS) row = ROWS - 1;
    return row;
}

static void edge_plot_new(uint8_t* bp, const uint8_t* y)
{
    uint8_t* colp = bp;                  /* plane-1 byte column: one byte per 4 source columns */
    int hp = y[0], h = y[0], hn = y[1], hnn = y[2];
    int rEven = sky_even_row(hp, h, hn);
    for (int c = 0; c < SRCW; ++c) {
        /* The next column's even row, from this window; carried over as rEven next iteration. */
        const int rNext = (c + 1 < SRCW) ? sky_even_row(h, hn, hnn) : rEven;
        const int pix = c & 3;
        if (rEven != 0xFF) {
            const int rOdd = (rNext == 0xFF) ? 0xFF
                           : sky_mid_row(c, hp, h, hn, hnn, rEven, rNext);
            if (rOdd == rEven) colp[kRow120[rEven]] |= kColMask4[pix];
            else {
                colp[kRow120[rEven]] |= kSkyEvenMask4[pix];
                if (rOdd != 0xFF) colp[kRow120[rOdd]] |= kSkyOddMask4[pix];
            }
        }
        if (pix == 3) colp++;
        hp = h; h = hn; hn = hnn;
        hnn = y[c + 3 < SRCW ? c + 3 : SRCW - 1];
        rEven = rNext;
    }
}

/* ---------------- the harness ---------------- */

static uint32_t rs = 0x1234567u;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

static uint8_t fieldbuf[SRCW];
static uint8_t planeA[PLANE], planeB[PLANE];

static int run_case(const uint8_t* y)
{
    memset(planeA, 0, sizeof planeA);
    memset(planeB, 0, sizeof planeB);
    edge_plot_old(planeA, y);
    edge_plot_new(planeB, y);
    return memcmp(planeA, planeB, sizeof planeA) == 0;
}

int main(void)
{
    build_tables();
    unsigned cases = 0, bad = 0;

    /* Constant fields, including $FF (all body) and both height clamps. */
    for (int v = 0; v < 256; ++v) {
        for (int c = 0; c < SRCW; ++c) fieldbuf[c] = (uint8_t)v;
        ++cases; if (!run_case(fieldbuf)) ++bad;
    }
    /* Unit ramps of every phase and both directions: these drive the parity half-row bump. */
    for (int base = 100; base < 160; ++base) for (int dir = -1; dir <= 1; dir += 2) {
        for (int c = 0; c < SRCW; ++c) fieldbuf[c] = (uint8_t)(base + dir * (c / 2));
        ++cases; if (!run_case(fieldbuf)) ++bad;
    }
    /* Alternating pairs and isolated $FF columns: the interval-touches-body path. */
    for (int t = 0; t < 256; ++t) {
        for (int c = 0; c < SRCW; ++c)
            fieldbuf[c] = (uint8_t)((c % 7 == t % 7) ? 0xFF : (120 + ((c * t) & 31)));
        ++cases; if (!run_case(fieldbuf)) ++bad;
    }
    /* Random fields over the whole byte domain, and over the live height band with sparse $FF. */
    for (int i = 0; i < 4000; ++i) {
        for (int c = 0; c < SRCW; ++c) fieldbuf[c] = (uint8_t)rnd();
        ++cases; if (!run_case(fieldbuf)) ++bad;
    }
    for (int i = 0; i < 4000; ++i) {
        for (int c = 0; c < SRCW; ++c) {
            unsigned r = rnd();
            fieldbuf[c] = (r & 15) == 0 ? 0xFFu : (uint8_t)(104 + (r >> 8) % 47);
        }
        ++cases; if (!run_case(fieldbuf)) ++bad;
    }

    if (bad) { printf("FAIL: %u of %u cases differ\n", bad, cases); return 1; }
    printf("PASS: %u height rows, plane-1 byte-identical over all %d rows\n", cases, ROWS);
    return 0;
}
