#define ECS_SPECIFIC
#include <hardware/custom.h>
#include <proto/exec.h>
#include <exec/memory.h>
#include "EmptyCopperList.h"
#include "framework/AmigaHardware.h"   // color00 / bplcon0 register symbols
#include "framework/Sprite.h"

// d[0] = copperWait(16,0) (CopperList ctor).
#define INDEX_COLOR00     1                     // COLOR00 = black (1)
#define INDEX_BPLCON0     (INDEX_COLOR00 + 1)   // 2: BPLCON0 = 0 planes (1)
#define INDEX_SPRITES     (INDEX_BPLCON0 + 1)   // 3: 8 sprite ptrs (16)
#define INDEX_TERMINATOR  (INDEX_SPRITES + 16)  // 19: copperWait(255,254)
#define LIST_LENGTH       (INDEX_TERMINATOR + 1)

EmptyCopperList::EmptyCopperList()
    : CopperList((uint32_t*)AllocMem(LIST_LENGTH << 2, MEMF_CHIP | MEMF_CLEAR), LIST_LENGTH, true)
{
}

void EmptyCopperList::buildLayout(const Sprite& nullSprite)
{
    uint32_t* d = data_;
    d[INDEX_COLOR00] = copperMove(color00, 0x000);   // black background/border
    d[INDEX_BPLCON0] = copperMove(bplcon0, 0x0000);  // 0 bitplanes -> no playfield fetch
    for (int s = 0; s < 8; s++)
        showSprite(INDEX_SPRITES + s * 2, (uint16_t)s, nullSprite);
    d[INDEX_TERMINATOR] = copperWait(255, 254);
}

void EmptyCopperList::setColor00(uint16_t c)
{
    data_[INDEX_COLOR00] = copperMove(color00, c);
}
