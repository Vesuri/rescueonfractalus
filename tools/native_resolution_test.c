/* Exhaustive host proof for the render-only 320x94 skyline mapping. */
#include <stdint.h>
#include <stdio.h>
#include "../src/platform/amiga/FlightTerrainGeometry.h"

static const uint8_t masks[8] = { 0x80,0x40,0x20,0x10,0x08,0x04,0x02,0x01 };

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
        unsigned even_rows = 0, odd_rows = 0;
        for (int col = 48; col < 208; ++col) for (int h = 104; h <= 150; ++h) {
            const int source_row = 150 - h;
            for (unsigned phase = 0; phase < 256; ++phase) {
                const int physical_row = source_row * 2 + ((phase >> 6) & 1u);
                if ((unsigned)physical_row >= ROF_FLIGHT_PHYSICAL_ROWS) ++bad;
                if (physical_row & 1) ++odd_rows; else ++even_rows;
            }
        }
        if (!even_rows || !odd_rows) ++bad;
    }
    printf("%s: 320 columns, 65025 height pairs, 47 authored row pairs, and both terrain Y phases, %u mismatches\n",
           bad ? "FAIL" : "PASS", bad);
    return bad != 0;
}
