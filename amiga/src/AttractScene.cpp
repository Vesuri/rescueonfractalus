#define ECS_SPECIFIC
#include <hardware/dmabits.h>
#include "AttractScene.h"
#include "../framework/AmigaHardware.h"
#include "../framework/CopperList.h"

// M0 copper list: blue background, no bitplanes.
// Must be in chip RAM (Paula/copper DMA can only reach chip RAM).
static __chip uint32_t copperData[] = {
    copperMove(color00, 0x00f),  // blue background
    0xfffffffe                   // copper end
};

void AttractScene::initialize()
{
    // Point the copper at our list and restart it.
    // Main has already disabled old DMA and set the display window;
    // we just install the list here.
    *cop1lcPointer = (uint32_t*)copperData;
    *copjmp1Pointer = 0;
}

void AttractScene::update(uint16_t /*frame*/)
{
    // M0: nothing to animate.
}

void AttractScene::render()
{
    // M0: static copper list, nothing to update.
}

void AttractScene::shutdown()
{
    // Nothing allocated.
}
