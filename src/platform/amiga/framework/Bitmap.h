#ifndef _BITMAP_H
#define _BITMAP_H

#include "Util.h"

class Bitmap {
public:
    Bitmap(void* data, uint16_t width, uint16_t height, uint16_t bitplanes, bool interleaved, bool takeOwnership = false, uint16_t dataWidth = 0);
    ~Bitmap();

    __inline uint16_t widthInWords() const;
    __inline uint16_t rowSizeInWords() const;
    __inline uint16_t bitplaneSizeInWords() const;
    __inline uint32_t dataSize() const;

#if defined(ASSEMBLER) && defined(__SASC)
    __asm void clear(register __d0 uint16_t x = 0, register __d1 uint16_t y = 0, register __d2 uint16_t width = 0, register __d3 uint16_t height = 0);
    __asm void copy(register __a1 const Bitmap& source, register __d0 uint16_t destX = 0, register __d1 uint16_t destY = 0, register __d2 uint16_t sourceX = 0, register __d3 uint16_t sourceY = 0, register __d4 uint16_t width = 0, register __d5 uint16_t height = 0, register __d7 uint16_t mask = 0xffff);
    __asm void copyWithMask(register __a1 const Bitmap& source, register __a2 const Bitmap& mask, register __d0 uint16_t destX = 0, register __d1 uint16_t destY = 0, register __d2 uint16_t sourceX = 0, register __d3 uint16_t sourceY = 0, register __d4 uint16_t maskX = 0, register __d5 uint16_t maskY = 0, register __d6 uint16_t width = 0, register __d7 uint16_t height = 0, register __a3 bool clearMasked = false);
    __asm void line(register __d0 uint16_t x1, register __d1 uint16_t y1, register __d2 uint16_t x2, register __d3 uint16_t y2, register __d4 uint16_t color = 1, register __d6 bool fillMode = false);
    __asm void fill(register __d0 uint16_t x = 0, register __d1 uint16_t y = 0, register __d2 uint16_t width = 0, register __d3 uint16_t height = 0);
#else
    void clear(uint16_t x = 0, uint16_t y = 0, uint16_t width = 0, uint16_t height = 0);
    void copy(const Bitmap& source, uint16_t destX = 0, uint16_t destY = 0, uint16_t sourceX = 0, uint16_t sourceY = 0, uint16_t width = 0, uint16_t height = 0, uint16_t mask = 0xffff);
    void copyWithMask(const Bitmap& source, const Bitmap& mask, uint16_t destX = 0, uint16_t destY = 0, uint16_t sourceX = 0, uint16_t sourceY = 0, uint16_t maskX = 0, uint16_t maskY = 0, uint16_t width = 0, uint16_t height = 0, bool clearMasked = false);
    void line(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color = 1, bool fillMode = false);
    void fill(uint16_t x = 0, uint16_t y = 0, uint16_t width = 0, uint16_t height = 0);
#endif

    // Higher-level drawing (imported from DanceDiverse3). Portable C++ on both
    // compilers — they build on clear/copy/line/fill and the masked blitter ops,
    // so they need no hand-written asm counterpart and are always available.
    void polygon(const Polygon& polygon, uint16_t color = 1, bool fillMode = false);
    void combineWithMask(const Bitmap& background, const Bitmap& source, const Bitmap& mask, uint16_t destX = 0, uint16_t destY = 0, uint16_t backgroundX = 0, uint16_t backgroundY = 0, uint16_t sourceX = 0, uint16_t sourceY = 0, uint16_t maskX = 0, uint16_t maskY = 0, uint16_t width = 0, uint16_t height = 0, bool clearMasked = false);
    void patternWithMask(const Bitmap& mask, uint16_t destX = 0, uint16_t destY = 0, uint16_t maskX = 0, uint16_t maskY = 0, uint16_t width = 0, uint16_t height = 0, uint16_t pattern = 0xffff);

    static Bitmap* allocate(uint16_t width, uint16_t height, uint16_t bitplanes, bool interleaved, uint16_t dataWidth = 0);
    static Bitmap* generateMask(const Bitmap& source, void* data = 0, bool singleBitplane = true, bool takeOwnership = false);

    void* data;
    uint16_t width;
    uint16_t height;
    uint16_t bitplanes;
    uint16_t dataWidth;
    uint16_t widthInBytes;
    uint16_t rowSizeInBytes;
    uint16_t bitplaneSizeInBytes;
    bool interleaved;
    bool owner;
    bool blittable;
};

#endif
