#define ECS_SPECIFIC
#include <hardware/custom.h>
#include <graphics/display.h>
#include <proto/exec.h>
#include <exec/memory.h>
#include "TitleScreenCopperList.h"
#include "framework/AmigaHardware.h"
#include "framework/Bitmap.h"
#include "framework/Sprite.h"

// ---- display geometry (MUST match RescueOnFractalus.cpp) ---------------------
static const uint16_t kW          = 320;
static const uint16_t kH          = 216;
static const uint8_t  kBP3        = 3;                       // pens 0..4 (black + COLPF0-3)
static const uint16_t kDisplayTop = 0x2c;
static const uint16_t kCenterY    = kDisplayTop + kH / 2;    // = 0x98

// ---- fixed list layout (indices into data_, 32-bit MOVE/WAIT words) ----------
// d[0] = copperWait(16,0) (CopperList ctor).  setPlayfield emits 3 words (BPLCON0 +
// BPL1MOD/BPL2MOD); the constant playfield registers are set once by
// AmigaHardware::setPlayfield in RescueOnFractalus::initialize.
#define INDEX_PLAYFIELD   1
#define INDEX_PAL         (INDEX_PLAYFIELD + 3)   // 4: color00..07 (8)
#define INDEX_BPL         (INDEX_PAL + 8)         // 12: text bitmap ptrs (3bp interleaved = 6)
#define INDEX_SPRITES     (INDEX_BPL + 6)         // 18: 8 sprite ptrs (16)
#define INDEX_TERMINATOR  (INDEX_SPRITES + 16)    // 34: copperWait(255,254)
#define LIST_LENGTH       (INDEX_TERMINATOR + 1)

TitleScreenCopperList::TitleScreenCopperList()
    : CopperList((uint32_t*)AllocMem(LIST_LENGTH << 2, MEMF_CHIP | MEMF_CLEAR), LIST_LENGTH, true)
{
}

void TitleScreenCopperList::buildLayout(const Bitmap& text, const Sprite& nullSprite)
{
    uint32_t* d = data_;

    // Full-screen 3bp interleaved playfield.
    setPlayfield(INDEX_PLAYFIELD, kW, kH, kBP3, /*interleaved*/true,
                 /*hires*/false, /*interlace*/false, /*dualPlayfield*/false,
                 /*holdAndModify*/false, kCenterY);

    // Palette: color00 = COLBK (black, constant); color01..04 = COLPF0-3 (the four text
    // pens, poked per frame for the palette cycle); color05..07 unused.
    d[INDEX_PAL + 0] = copperMove(color00, 0x000);
    setTextPalette(0, 0, 0, 0);                  // seeded; caller refreshes
    d[INDEX_PAL + 5] = copperMove(color05, 0x000);
    d[INDEX_PAL + 6] = copperMove(color06, 0x000);
    d[INDEX_PAL + 7] = copperMove(color07, 0x000);

    showBitmap(INDEX_BPL, text);                 // 3bp interleaved = 6 ptr moves

    for (int s = 0; s < 8; s++)
        showSprite(INDEX_SPRITES + s * 2, (uint16_t)s, nullSprite);

    d[INDEX_TERMINATOR] = copperWait(255, 254);
}

void TitleScreenCopperList::setTextPalette(uint16_t pf0, uint16_t pf1, uint16_t pf2, uint16_t pf3)
{
    data_[INDEX_PAL + 1] = copperMove(color01, pf0);   // COLPF0 -> pen1
    data_[INDEX_PAL + 2] = copperMove(color02, pf1);   // COLPF1 -> pen2
    data_[INDEX_PAL + 3] = copperMove(color03, pf2);   // COLPF2 -> pen3
    data_[INDEX_PAL + 4] = copperMove(color04, pf3);   // COLPF3 -> pen4
}
