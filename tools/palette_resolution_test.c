#include <stdint.h>
#include <stdio.h>

#include "../amiga/assets/atari_pal.h"

static unsigned nibble(uint16_t c, unsigned shift)
{
    return (c >> shift) & 0x0Fu;
}

int main(void)
{
    for (unsigned hue = 0; hue < 16; hue++) {
        const unsigned base = hue << 4;
        for (unsigned lum = 0; lum < 16; lum += 2) {
            const unsigned i = base + lum;
            if (kAtariHalfLumaPalette[i] != kAtariPalette[i]) {
                fprintf(stderr, "$%02X: even entry %03X != faithful %03X\n",
                        i, kAtariHalfLumaPalette[i], kAtariPalette[i]);
                return 1;
            }
        }
        for (unsigned lum = 1; lum < 16; lum += 2) {
            const unsigned i = base + lum;
            const uint16_t lo = kAtariHalfLumaPalette[i - 1];
            const uint16_t mid = kAtariHalfLumaPalette[i];
            const uint16_t hi = lum == 15 ? 0x0FFFu : kAtariHalfLumaPalette[i + 1];
            for (unsigned shift = 0; shift <= 8; shift += 4) {
                if (nibble(mid, shift) < nibble(lo, shift) ||
                    nibble(mid, shift) > nibble(hi, shift)) {
                    fprintf(stderr, "$%02X: channel %u is outside its luminance interval\n",
                            i, shift >> 2);
                    return 1;
                }
            }
        }
    }
    puts("256 entries: faithful evens and bounded half-luminance odds");
    return 0;
}
