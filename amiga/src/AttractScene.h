#pragma once
#include "../framework/CopperList.h"
#include "../framework/Bitmap.h"
#include "../framework/Palette.h"
#include "../framework/Sprite.h"

// M4: canopy frame posts as OCS hardware sprites (two 4-px green vertical bars).
//     Sprites 0+1 active during terrain region (beam 110-196), null elsewhere.
//     Sprites 2-7 silenced via null sprite pointer to prevent garbage display.
class AttractScene {
public:
    void initialize();
    void update(uint16_t frame);
    void render();
    void shutdown();
private:
    void buildCopperList(CopperList* cl, uint16_t frame);
    void fillSpriteData(Sprite* s, uint16_t sprA);

    CopperList* copperLists[2] = { nullptr, nullptr };
    Bitmap*     bitmap         = nullptr;
    Bitmap*     terrainBitmap  = nullptr;
    Palette*    palette        = nullptr;
    Sprite*     leftPost       = nullptr;  // sprite 0: left canopy post
    Sprite*     rightPost      = nullptr;  // sprite 1: right canopy post
    Sprite*     nullSprite     = nullptr;  // sprites 2-7: silence
    uint8_t     active         = 0;
};
