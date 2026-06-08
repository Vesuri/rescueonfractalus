#pragma once
#include "../framework/CopperList.h"
#include "../framework/Bitmap.h"
#include "../framework/Palette.h"

// M3: 2-bitplane terrain viewport blended with 4-bitplane title/cockpit bitmap.
// The copper switches modes mid-frame:
//   rows  0-41  : 4-plane attract_raw (title)
//   rows 42-127 : 2-plane terrain_raw (terrain, copper-overridden pointers)
//   rows128-199 : 4-plane attract_raw cockpit section (copper-restored pointers)
class AttractScene {
public:
    void initialize();
    void update(uint16_t frame);
    void render();
    void shutdown();
private:
    void buildCopperList(CopperList* cl, uint16_t frame);

    CopperList* copperLists[2] = { nullptr, nullptr };
    Bitmap*     bitmap         = nullptr;  // full 4-plane attract_raw
    Bitmap*     terrainBitmap  = nullptr;  // 2-plane terrain_raw
    Palette*    palette        = nullptr;  // full 16-colour attract palette
    uint8_t     active         = 0;
};
