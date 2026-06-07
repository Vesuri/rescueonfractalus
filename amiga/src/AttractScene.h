#pragma once
#include "../framework/CopperList.h"

// M0: blank Copper-driven background (recognisable blue = 0x00f).
// Subsequent milestones fill this out with bitmap, colour splits, sprites, audio.
class AttractScene {
public:
    void initialize();
    void update(uint16_t frame);
    void render();
    void shutdown();
private:
    CopperList* copperList = nullptr;
};
