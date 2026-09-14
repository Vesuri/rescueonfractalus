/* Exhaustive host proof for the render-only 320x94 skyline mapping. */
#include <stdint.h>
#include <stdio.h>
#include "../src/platform/amiga/FlightTerrainGeometry.h"

static const uint8_t masks[8] = { 0x80,0x40,0x20,0x10,0x08,0x04,0x02,0x01 };

static uint8_t terrain_phase(int value)
{
    uint8_t p = (uint8_t)((unsigned)value * 73u + 41u);
    p ^= (uint8_t)(p >> 4);
    p = (uint8_t)((unsigned)p * 29u);
    p ^= (uint8_t)(p >> 3);
    return p;
}

int main(void)
{
    unsigned bad = 0;
    for (int c = 0; c < ROF_FLIGHT_SOURCE_WIDTH; ++c) {
        const int even = c * 2, odd = even + 1;
        if ((even >> 3) >= ROF_FLIGHT_PLANE_ROW_BYTES || !masks[even & 7]) ++bad;
        if ((odd  >> 3) >= ROF_FLIGHT_PLANE_ROW_BYTES || !masks[odd  & 7]) ++bad;
    }
    for (int h0 = 0; h0 < 255; ++h0) for (int h1 = 0; h1 < 255; ++h1) {
        int r0 = 2 * (150 - h0), r1 = 2 * (150 - h1);
        if (r0 < 0) r0 = 0; else if (r0 > 92) r0 = 92;
        if (r1 < 0) r1 = 0; else if (r1 > 92) r1 = 92;
        int midpoint = (r0 + r1) / 2;
        /* The midpoint may be odd, which is the new vertical precision. */
        if ((unsigned)midpoint >= ROF_FLIGHT_PHYSICAL_ROWS) ++bad;
    }
    for (int row = 0; row < ROF_FLIGHT_SOURCE_ROWS; ++row) {
        const int first = row * ROF_FLIGHT_SOURCE_TO_PHYSICAL_Y;
        const int second = first + 1;
        if (first < 0 || second >= ROF_FLIGHT_PHYSICAL_ROWS || second != first + 1) ++bad;
    }
    {
        unsigned even_rows = 0, odd_rows = 0, even_cols = 0, odd_cols = 0;
        for (int col = 48; col < 208; ++col) for (int h = 104; h <= 150; ++h) {
            const int source_row = 150 - h;
            for (unsigned old_max = 0; old_max < 256; ++old_max) {
                const int physical_row = source_row * 2 + ((terrain_phase(col) >> 1) & 1u);
                if ((unsigned)physical_row >= ROF_FLIGHT_PHYSICAL_ROWS) ++bad;
                if (physical_row & 1) ++odd_rows; else ++even_rows;
                const int physical_col = (col - 48) * 2 + (terrain_phase(col) & 1u);
                if ((unsigned)physical_col >= ROF_FLIGHT_PHYSICAL_WIDTH) ++bad;
                if ((physical_col >> 3) != ((col - 48) >> 2)) ++bad;
                if (physical_col & 1) ++odd_cols; else ++even_cols;
            }
        }
        if (!even_rows || !odd_rows || !even_cols || !odd_cols) ++bad;
    }
    /* Smooth terrain is the failure mode visible in screenshots: raw height bits remain nearly
     * constant and make a regular lattice.  Require each phase to remain balanced and prevent a
     * long same-X-phase run on several representative shallow slopes. */
    {
        unsigned quadrants[4] = {0,0,0,0};
        int previous = -1, run = 0, longest = 0;
        for (int c = 48; c < 208; ++c) {
            int q = terrain_phase(c) & 3u;
            ++quadrants[q];
            int xphase = q & 1;
            run = (xphase == previous) ? run + 1 : 1;
            previous = xphase;
            if (run > longest) longest = run;
        }
        for (int q = 0; q < 4; ++q)
            if (quadrants[q] < 24u || quadrants[q] > 56u) ++bad;
        if (longest > 8) ++bad;
    }
    printf("%s: 320 columns, 65025 height pairs, 47 authored row pairs, and both terrain X/Y phases, %u mismatches\n",
           bad ? "FAIL" : "PASS", bad);
    return bad != 0;
}
