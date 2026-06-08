#pragma once
#include "../framework/CopperList.h"
#include "../framework/Bitmap.h"
#include "../framework/Palette.h"

// M1: static 320×200 4-bitplane bitmap + flat palette.
// M2: double-buffered copper lists rebuilt per frame; palette fade-in over 64 frames,
//     then a gentle sky-colour hue sweep — demonstrating the per-frame copper idiom.
//     WAIT instructions at region boundaries prepare for M3 per-region palettes.
class AttractScene {
public:
    void initialize();
    void update(uint16_t frame);
    void render();
    void shutdown();
private:
    void buildCopperList(CopperList* cl, uint16_t frame);

    CopperList* copperLists[2] = { nullptr, nullptr };
    Bitmap*     bitmap         = nullptr;
    Palette*    palette        = nullptr;
    uint8_t     active         = 0;
};
