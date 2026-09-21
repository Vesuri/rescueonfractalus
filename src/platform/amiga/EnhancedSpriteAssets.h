#ifndef ROF_ENHANCED_SPRITE_ASSETS_H
#define ROF_ENHANCED_SPRITE_ASSETS_H

// Native-resolution Amiga artwork used only by Enhanced Graphics. These are authored 16-pixel
// masks, not enlarged Atari PMG rows, so their edges can land at odd pixel positions.
struct EnhancedSpriteAsset {
    const uint16_t* rows;
    uint8_t height;
};

static const uint16_t kEnhancedTorpedo0[] = {
    0x00C0, 0x01E0, 0x03F0, 0x0FFC, 0x3FFF
};
static const uint16_t kEnhancedTorpedo2[] = {
    0x00C0, 0x01E0, 0x03F0, 0x07F8, 0x0FFC, 0x0FFC, 0x0FFC,
    0x0FFC, 0x0FFC, 0x0FFC, 0x0FFC, 0x07F8, 0x03F0, 0x01E0
};
static const uint16_t kEnhancedTorpedo3[] = {
    0x00C0, 0x01E0, 0x03F0, 0x03F0, 0x01E0, 0x00C0
};
static const uint16_t kEnhancedTorpedo4[] = { 0x00C0, 0x01E0, 0x01E0, 0x00C0 };
static const uint16_t kEnhancedTorpedo5[] = { 0x0080, 0x00C0, 0x0080 };
static const uint16_t kEnhancedTorpedo6[] = { 0x00C0, 0x0080 };
static const uint16_t kEnhancedTorpedo7[] = { 0x0080 };

static const EnhancedSpriteAsset kEnhancedTorpedo[8] = {
    { kEnhancedTorpedo0, 5 }, { kEnhancedTorpedo0, 5 },
    { kEnhancedTorpedo2, 14 }, { kEnhancedTorpedo3, 6 },
    { kEnhancedTorpedo4, 4 }, { kEnhancedTorpedo5, 3 },
    { kEnhancedTorpedo6, 2 }, { kEnhancedTorpedo7, 1 }
};

#endif
