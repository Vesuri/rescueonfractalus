#include <stdint.h>
#include <stdio.h>

#include "../src/platform/amiga/PaletteInterpolation.h"

static unsigned nibble(uint16_t c, unsigned shift)
{
    return (c >> shift) & 0x0Fu;
}

int main(void)
{
    if (atmosphereBlendStep(0, 255) != 0 ||
        atmosphereBlendStep(0, 191) != 1 ||
        atmosphereBlendStep(1, 0) != 4 ||
        atmosphereBlendStep(3, 1) != 15 ||
        atmosphereBlendStep(4, 0) != 0) {
        fputs("atmosphere phase/countdown mapping failed\n", stderr);
        return 1;
    }

    for (unsigned from = 0; from < 0x1000; from++) {
        for (unsigned to = 0; to < 0x1000; to += 0x111) {
            uint16_t prev = (uint16_t)from;
            if (lerpOCS((uint16_t)from, (uint16_t)to, 0) != from) return 1;
            for (unsigned step = 1; step < 16; step++) {
                const uint16_t col = lerpOCS((uint16_t)from, (uint16_t)to, (uint8_t)step);
                for (unsigned shift = 0; shift <= 8; shift += 4) {
                    const unsigned a = nibble((uint16_t)from, shift);
                    const unsigned b = nibble((uint16_t)to, shift);
                    const unsigned p = nibble(prev, shift);
                    const unsigned c = nibble(col, shift);
                    if ((a <= b && (c < p || c > b)) || (a > b && (c > p || c < b))) {
                        fprintf(stderr, "%03X -> %03X step %u is not monotonic\n", from, to, step);
                        return 1;
                    }
                }
                prev = col;
            }
        }
    }
    puts("phase mapping and OCS nibble interpolation are bounded and monotonic");
    return 0;
}
