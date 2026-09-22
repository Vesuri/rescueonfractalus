/* Exhaustive proof for the Planet renderer's indexed-pixel Scale2x primitive. */
#include <stdint.h>
#include <stdio.h>
#include "../src/platform/amiga/PlanetScale2x.h"

int main(void)
{
    unsigned bad = 0, sharpened = 0, horizontal = 0, vertical = 0;
    for (uint8_t b = 0; b < 4; ++b)
    for (uint8_t d = 0; d < 4; ++d)
    for (uint8_t e = 0; e < 4; ++e)
    for (uint8_t f = 0; f < 4; ++f)
    for (uint8_t h = 0; h < 4; ++h) {
        uint8_t e0, e1, e2, e3;
        rofPlanetScale2xPixel(b, d, e, f, h, &e0, &e1, &e2, &e3);
        if (e0 > 3 || e1 > 3 || e2 > 3 || e3 > 3) ++bad;
        if (b == h || d == f) {
            if (e0 != e || e1 != e || e2 != e || e3 != e) ++bad;
        } else {
            if (e0 != (d == b ? d : e) || e1 != (b == f ? f : e) ||
                e2 != (d == h ? d : e) || e3 != (h == f ? f : e)) ++bad;
        }
        if (e0 != e1 || e2 != e3) ++horizontal;
        if (e0 != e2 || e1 != e3) ++vertical;
        if (e0 != e || e1 != e || e2 != e || e3 != e) ++sharpened;
    }
    if (!sharpened || !horizontal || !vertical) ++bad;
    printf("%s: 1024 neighbourhoods, %u sharpened, %u horizontal and %u vertical subpixel edges, %u failures\n",
           bad ? "FAIL" : "PASS", sharpened, horizontal, vertical, bad);
    return bad != 0;
}
