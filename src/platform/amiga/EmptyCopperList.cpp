#define ECS_SPECIFIC
#include <hardware/custom.h>
#include <graphics/display.h>          // USE_BPLCON3 (ECSENA bit — keeps BPLCON3 BRDNBLNK live)
#include <proto/exec.h>
#include <exec/memory.h>
#include "EmptyCopperList.h"
#include "framework/AmigaHardware.h"   // colour / bplcon / bitplane register symbols
#include "framework/Sprite.h"

// The display is 320px lores → 40 bytes fetched per scanline for one bitplane.  We set
// bpl1mod = -kLineBytes so BPL1PT returns to the same throwaway line every scanline (Agnus
// re-reads one small buffer; its content is irrelevant since color00 == color01).
static const int kLineBytes = 40;

// DIWSTRT/DIWSTOP/DDFSTRT/DDFSTOP are deliberately NOT set here — like every other copper
// list, this inherits the display window / data-fetch set once at startup by
// AmigaHardware::setPlayfield (they hold constant through the whole game).
// d[0] = copperWait(16,0) (CopperList ctor).
#define INDEX_COLOR00     1                     // color00 (1)
#define INDEX_COLOR01     (INDEX_COLOR00 + 1)   // 2: color01 — same colour as color00 (uniform)
#define INDEX_BPLCON0     (INDEX_COLOR01 + 1)   // 3: BPLCON0 = 1 plane + ECSENA (1)
#define INDEX_BPLCON3     (INDEX_BPLCON0 + 1)   // 4: BPLCON3 = border-blank (1)
#define INDEX_BPL1PT      (INDEX_BPLCON3 + 1)   // 5: BPL1PTH/BPL1PTL (2)
#define INDEX_BPL1MOD     (INDEX_BPL1PT + 2)    // 7: BPL1MOD = -kLineBytes (1)
#define INDEX_SPRITES     (INDEX_BPL1MOD + 1)   // 8: 8 sprite ptrs (16)
#define INDEX_TERMINATOR  (INDEX_SPRITES + 16)  // 24: copperWait(255,254)
#define LIST_LENGTH       (INDEX_TERMINATOR + 1)

EmptyCopperList::EmptyCopperList()
    : CopperList((uint32_t*)AllocMem(LIST_LENGTH << 2, MEMF_CHIP | MEMF_CLEAR), LIST_LENGTH, true)
{
    // Throwaway single-line bitplane buffer (a little padding past kLineBytes for the DMA
    // read-ahead).  Its content never shows — color00 == color01 — so it stays cleared.
    filler_ = (uint16_t*)AllocMem(64, MEMF_CHIP | MEMF_CLEAR);
}

void EmptyCopperList::buildLayout(const Sprite& nullSprite)
{
    uint32_t* d = data_;
    d[INDEX_COLOR00] = copperMove(color00, 0x000);   // both pens black initially
    d[INDEX_COLOR01] = copperMove(color01, 0x000);
    // ONE bitplane + ECSENA (USE_BPLCON3): an active playfield in the display window (so it shows
    // color00/01) with BPLCON3 BRDNBLNK blanking the border.  0 bitplanes + BRDNBLNK would blank
    // the WHOLE raster to black (user-observed); with color00==color01 the plane reads uniform.
    d[INDEX_BPLCON0] = copperMove(bplcon0, (uint16_t)((1u << PLNCNTSHFT) | USE_BPLCON3));
    d[INDEX_BPLCON3] = copperMove(bplcon3, 0x0c00 | BPLCON3_BRDNBLNK | BPLCON3_BRDNTRAN);  // blank both borders
    d[INDEX_BPL1PT + 0] = copperMove(bpl1pth, (uint16_t)(((uint32_t)filler_) >> 16));
    d[INDEX_BPL1PT + 1] = copperMove(bpl1ptl, (uint16_t)(((uint32_t)filler_) & 0xFFFF));
    d[INDEX_BPL1MOD]    = copperMove(bpl1mod, (uint16_t)(-kLineBytes));   // re-read the same line each scanline
    for (int s = 0; s < 8; s++)
        showSprite(INDEX_SPRITES + s * 2, (uint16_t)s, nullSprite);
    d[INDEX_TERMINATOR] = copperWait(255, 254);
}

void EmptyCopperList::setColor00(uint16_t c)
{
    // Set BOTH pens so the single-bitplane display window reads one uniform colour.
    data_[INDEX_COLOR00] = copperMove(color00, c);
    data_[INDEX_COLOR01] = copperMove(color01, c);
}
