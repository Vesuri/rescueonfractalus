#pragma once
#include "../framework/CopperList.h"
#include "../framework/Bitmap.h"
#include "../framework/Palette.h"
#include "../framework/Sprite.h"

// 2-bitplane attract screen: one BPLCON0 mode for the whole frame, Copper
// switches the 4-colour palette (and bitmap pointer) at each region boundary,
// mirroring the Atari DLI schedule exactly.
//
// Regions:
//   Title   (lines 0..kTerrainLine-1) : title.raw  + kTitlePalette
//   Terrain (lines kTerrainLine..kCockpitLine-1) : terrain.raw + kTerrainPalette
//   Cockpit (lines kCockpitLine..end) : cockpit.raw + kCockpitPalette (+ blink)
class AttractScene {
public:
    void initialize();
    void update(uint16_t frame);
    void render();
    void shutdown();
private:
    void buildCopperList(CopperList* cl, uint16_t frame);
    void fillSpriteData(Sprite* s, bool isRight);

    CopperList* copperLists[2] = { nullptr, nullptr };
    Bitmap*     titleBitmap    = nullptr;
    Bitmap*     terrainBitmap  = nullptr;
    Bitmap*     cockpitBitmap  = nullptr;
    Palette*    palette        = nullptr;  // drives title region + global fade
    Sprite*     leftPost       = nullptr;
    Sprite*     rightPost      = nullptr;
    Sprite*     nullSprite     = nullptr;
    uint8_t     active         = 0;
    uint16_t    blinkFrame     = 0;
};
