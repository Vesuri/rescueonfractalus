#pragma once
#include "../framework/CopperList.h"
#include "../framework/Bitmap.h"
#include "../framework/Palette.h"

// M1: static 320×200 4-bitplane attract bitmap with flat 16-colour OCS palette,
//     shown via CopperList setPlayfield/showBitmap/setPalette.
// M2 will add copper colour splits. M3 terrain region. M4 sprites. M5 audio.
class AttractScene {
public:
    void initialize();
    void update(uint16_t frame);
    void render();
    void shutdown();
private:
    CopperList* copperList = nullptr;
    Bitmap*     bitmap     = nullptr;
    Palette*    palette    = nullptr;
};
