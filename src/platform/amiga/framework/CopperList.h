#ifndef _COPPERLIST_H
#define _COPPERLIST_H

#include "Util.h"

#define copperWait(y, x) (((y) << 24) | ((x) << 16) | 0x0001fffe)
#define copperMove(register, data) (((register) << 16) | (uint16_t)(data))

class Sprite;
class Bitmap;
class Palette;

class CopperList {
public:
    CopperList();
    CopperList(uint32_t* data, uint32_t length = 0, bool takeOwnership = false);
    virtual ~CopperList();

    __inline uint32_t* data() const;

#if defined(ASSEMBLER) && defined(__SASC)
    __asm void showSprite(register __d0 uint32_t listIndex, register __d1 uint16_t spriteNumber, register __a1 const Sprite& sprite);
    __asm void showBitmap(register __d0 uint32_t listIndex, register __a1 const Bitmap& bitmap, register __d1 uint16_t firstBitplane = 1, register __d2 uint16_t bitplaneNumberDelta = 1, register __d3 int16_t xOffset = 0, register __d4 int16_t yOffset = 0, register __d5 uint16_t bitplaneCount = 0);
#else
    void showSprite(uint32_t listIndex, uint16_t spriteNumber, const Sprite& sprite);
    void showBitmap(uint32_t listIndex, const Bitmap& bitmap, uint16_t firstBitplane = 1, uint16_t bitplaneNumberDelta = 1, int16_t xOffset = 0, int16_t yOffset = 0, uint16_t bitplaneCount = 0);
#endif

    // Copper-driven palette/playfield (imported from DanceDiverse3, adapted to the
    // template's Palette and setPlayfield). Write color/mode changes into the copper
    // list so they take effect mid-frame (raster splits) rather than at vblank.
    // Pure C++ on both compilers. setPalette/setColor cover the 32 colour registers;
    // for AGA 256-colour banking add the 24-bit palette bundle. Each returns the
    // next free list index.
    uint32_t setPalette(uint32_t listIndex, const Palette& palette, uint16_t colorIndex = 0, int16_t fromIndex = 0, int16_t toIndex = -1);
    void setColor(uint32_t listIndex, uint16_t color, uint16_t count);
    uint32_t setPlayfield(uint32_t listIndex, uint16_t width, uint16_t height, uint8_t bitplaneCount, bool interleaved, bool hires = false, bool interlace = false, bool dualPlayfield = false, bool holdAndModify = false, uint16_t centerY = 0xa8);

    static CopperList* allocate(uint32_t length);

protected:
    uint32_t* data_;
    uint32_t length;
    bool owner;
};

#endif
