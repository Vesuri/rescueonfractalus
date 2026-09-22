#ifndef ROF_ENHANCED_SPRITE_ASSETS_H
#define ROF_ENHANCED_SPRITE_ASSETS_H

// Native-resolution Amiga artwork used only by Enhanced Graphics. These are authored 16-pixel
// masks, not enlarged Atari PMG rows, so their edges can land at odd pixel positions.
struct EnhancedSpriteAsset {
    const uint16_t* rows;
    uint8_t height;
};

// Full-size 14x15 logo sparkle. The original PMG rows occupy the same footprint after bit
// doubling; this redraw keeps those extents while making the vertical and diagonal rays one pixel
// thick. The centre row is supplied separately because the original animation narrows it in place.
static const uint16_t kEnhancedLogoSparkle[15] = {
    0x0100, 0x0100, 0x0100, 0x0100, 0x0100,
    0x0940, 0x0580, 0xFFFC, 0x0580, 0x0940,
    0x0100, 0x0100, 0x0100, 0x0100, 0x0100
};

// Native-resolution launch flare (frames 0-1), followed by the receding torpedo (frames 2-7).
// The travelling shapes retain the original directionality: a pointed leading/top edge and a
// comparatively blunt tail, rather than a vertically symmetric capsule.
static const uint16_t kEnhancedTorpedo0[] = {
    0x00C0, 0x01E0, 0x03F0, 0x0FFC, 0x3FFF
};
static const uint16_t kEnhancedTorpedo2[] = {
    0x00C0, 0x01E0, 0x03F0, 0x07F8, 0x0FFC, 0x0FFC, 0x0FFC,
    0x0FFC, 0x0FFC, 0x0FFC, 0x0FFC, 0x0FFC, 0x0FFC, 0x03F0
};
static const uint16_t kEnhancedTorpedo3[] = {
    0x00C0, 0x01E0, 0x03F0, 0x03F0, 0x03F0, 0x03F0
};
static const uint16_t kEnhancedTorpedo4[] = { 0x00C0, 0x01E0, 0x01E0, 0x01E0 };
static const uint16_t kEnhancedTorpedo5[] = { 0x0080, 0x00C0, 0x00C0 };
static const uint16_t kEnhancedTorpedo6[] = { 0x0080, 0x00C0 };
static const uint16_t kEnhancedTorpedo7[] = { 0x0080 };

static const EnhancedSpriteAsset kEnhancedTorpedo[8] = {
    { kEnhancedTorpedo0, 5 }, { kEnhancedTorpedo0, 5 },
    { kEnhancedTorpedo2, 14 }, { kEnhancedTorpedo3, 6 },
    { kEnhancedTorpedo4, 4 }, { kEnhancedTorpedo5, 3 },
    { kEnhancedTorpedo6, 2 }, { kEnhancedTorpedo7, 1 }
};

// Seven centered, filled ellipses in 1x1 Amiga pixels. Their bounding boxes are respectively
// 1x1, 2x2, 6x4, 8x6, 10x8, 12x10, and 16x14 pixels.
static const uint16_t kEnhancedExplosion1[]  = { 0x0080 };
static const uint16_t kEnhancedExplosion2[]  = { 0x0180, 0x0180 };
static const uint16_t kEnhancedExplosion4[]  = { 0x03C0, 0x07E0, 0x07E0, 0x03C0 };
static const uint16_t kEnhancedExplosion6[]  = {
    0x03C0, 0x07E0, 0x0FF0, 0x0FF0, 0x07E0, 0x03C0
};
static const uint16_t kEnhancedExplosion8[]  = {
    0x03C0, 0x0FF0, 0x1FF8, 0x1FF8, 0x1FF8, 0x1FF8, 0x0FF0, 0x03C0
};
static const uint16_t kEnhancedExplosion10[] = {
    0x07E0, 0x0FF0, 0x1FF8, 0x3FFC, 0x3FFC,
    0x3FFC, 0x3FFC, 0x1FF8, 0x0FF0, 0x07E0
};
static const uint16_t kEnhancedExplosion14[] = {
    0x07E0, 0x1FF8, 0x3FFC, 0x7FFE, 0x7FFE, 0xFFFF, 0xFFFF,
    0xFFFF, 0xFFFF, 0x7FFE, 0x7FFE, 0x3FFC, 0x1FF8, 0x07E0
};

static const EnhancedSpriteAsset kEnhancedExplosion[7] = {
    { kEnhancedExplosion1, 1 }, { kEnhancedExplosion2, 2 },
    { kEnhancedExplosion4, 4 }, { kEnhancedExplosion6, 6 },
    { kEnhancedExplosion8, 8 }, { kEnhancedExplosion10, 10 },
    { kEnhancedExplosion14, 14 }
};

#endif
