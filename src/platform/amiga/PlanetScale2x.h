#pragma once

// Scale one indexed source pixel into a 2x2 block while preserving hard pixel-art edges.
// B/D/F/H are the four direct neighbours of E.  Unlike nearest-neighbour doubling, diagonal
// boundaries may occupy either physical pixel, giving the reconstructed image 1x1 precision.
static inline void rofPlanetScale2xPixel(uint8_t b, uint8_t d, uint8_t e, uint8_t f, uint8_t h,
                                         uint8_t* e0, uint8_t* e1,
                                         uint8_t* e2, uint8_t* e3)
{
    *e0 = *e1 = *e2 = *e3 = e;
    if (b == h || d == f) return;
    if (d == b) *e0 = d;
    if (b == f) *e1 = f;
    if (d == h) *e2 = d;
    if (h == f) *e3 = f;
}
