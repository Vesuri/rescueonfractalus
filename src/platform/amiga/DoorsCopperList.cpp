#define ECS_SPECIFIC
#include <hardware/custom.h>
#include <graphics/display.h>
#include <proto/exec.h>
#include <exec/memory.h>
#include "DoorsCopperList.h"
#include "framework/AmigaHardware.h"
#include "framework/Bitmap.h"
#include "framework/Sprite.h"
#include "assets/atari_pal.h"   // atariToOCS() for the constant cockpit palette

// ---- display geometry --------------------------------------------------------
// MUST match the constants in RescueOnFractalus.cpp (same derivation).
static const uint16_t kW            = 320;
static const uint16_t kH            = 216;
static const uint8_t  kBP2          = 2;
static const uint16_t kDisplayTop   = 0x2c;
static const uint16_t kTitleHeight  = 42;
static const uint16_t kTerrainHeight = 86;
static const uint16_t kTerrainLine  = kDisplayTop + kTitleHeight;     // = 0x56
static const uint16_t kCockpitLine  = kTerrainLine + kTerrainHeight;  // = 172
static const uint16_t kCenterY      = kDisplayTop + kH / 2;           // = 0x98
static const uint16_t kBPLCON0_3P   = (uint16_t)((3 << PLNCNTSHFT) | USE_BPLCON3);

// A copper NOP: MOVE to register $1FE (unused) changes nothing.  Used to fill the
// slots of a collapsed terrain band so a fixed-length list can hold a variable band
// count.  Collapsed bands park their WAIT mid-region (see update()) so the NOPs run
// in a harmless H-blank, never crowding the cockpit pointer writes.
#define COPPER_NOP copperMove(0x1FE, 0)

// ---- fixed list layout (indices into data_, in 32-bit MOVE/WAIT words) -------
// d[0] = copperWait(16,0) (CopperList ctor).  Title region identical to
// StandbyCopperList; terrain region = three bands (top terrain / tunnel reveal /
// bottom terrain), the maximal door-open layout.
#define INDEX_PLAYFIELD       1
#define INDEX_TITLE_PAL       (INDEX_PLAYFIELD + 3)    // color00..03 (4)
#define INDEX_TITLE_BPL       (INDEX_TITLE_PAL + 4)    // title bitmap ptrs (2bp = 4)
#define INDEX_SPRITE_COL      (INDEX_TITLE_BPL + 4)    // color16,color17 (2)
#define INDEX_SPRITES         (INDEX_SPRITE_COL + 2)   // 8 sprite ptrs (16)
#define INDEX_GAUGE_COL       (INDEX_SPRITES + 16)     // COLOR21 ($1AA) (1)
#define INDEX_COMPASS_WAIT    (INDEX_GAUGE_COL + 1)    // WAIT(compass scanline) (1)
#define INDEX_COMPASS_COL     (INDEX_COMPASS_WAIT + 1) // color01 = compass COLPF0 (1)
// Band 0 (top terrain): WAIT, 6 bpl ptrs, bplcon0, 2 modulo, color00..06 (7).
#define INDEX_B0_WAIT         (INDEX_COMPASS_COL + 1)
#define INDEX_B0_BPL          (INDEX_B0_WAIT + 1)       // 6
#define INDEX_B0_BPLCON0      (INDEX_B0_BPL + 6)        // 1
#define INDEX_B0_MOD          (INDEX_B0_BPLCON0 + 1)    // 2
#define INDEX_B0_COL          (INDEX_B0_MOD + 2)        // color00..06 (7)
// Band 1 (tunnel reveal): WAIT, 6 bpl ptrs, color01..03 (3).
#define INDEX_B1_WAIT         (INDEX_B0_COL + 7)
#define INDEX_B1_BPL          (INDEX_B1_WAIT + 1)       // 6
#define INDEX_B1_COL          (INDEX_B1_BPL + 6)        // color01..03 (3)
// Band 2 (bottom terrain): WAIT, 6 bpl ptrs, color01..03 (3).
#define INDEX_B2_WAIT         (INDEX_B1_COL + 3)
#define INDEX_B2_BPL          (INDEX_B2_WAIT + 1)       // 6
#define INDEX_B2_COL          (INDEX_B2_BPL + 6)        // color01..03 (3)
// Cockpit region (constant).
#define INDEX_COCKPIT_WAIT    (INDEX_B2_COL + 3)
#define INDEX_COCKPIT_BPL     (INDEX_COCKPIT_WAIT + 1)  // 6
#define INDEX_COCKPIT_BPLCON0 (INDEX_COCKPIT_BPL + 6)   // 1
#define INDEX_COCKPIT_MOD     (INDEX_COCKPIT_BPLCON0 + 1) // 2
#define INDEX_COCKPIT_PAL     (INDEX_COCKPIT_MOD + 2)   // color00..07 (8)
#define INDEX_TERMINATOR      (INDEX_COCKPIT_PAL + 8)
#define LIST_LENGTH           (INDEX_TERMINATOR + 1)

// Park line for a collapsed band's WAIT (mid terrain region, on its own H-blank).
static const uint16_t kBand1ParkLine = kTerrainLine + 28;
static const uint16_t kBand2ParkLine = kTerrainLine + 57;

// emitBpl: 3bp interleaved bitplane pointers (120 B/row) at list index `at`.
static void emitBpl(uint32_t* d, uint32_t at, uint32_t base)
{
    d[at + 0] = copperMove(bpl1pth, (uint16_t)(base >> 16));
    d[at + 1] = copperMove(bpl1ptl, (uint16_t)(base & 0xFFFF));
    d[at + 2] = copperMove(bpl2pth, (uint16_t)((base + 40) >> 16));
    d[at + 3] = copperMove(bpl2ptl, (uint16_t)((base + 40) & 0xFFFF));
    d[at + 4] = copperMove(bpl3pth, (uint16_t)((base + 80) >> 16));
    d[at + 5] = copperMove(bpl3ptl, (uint16_t)((base + 80) & 0xFFFF));
}

DoorsCopperList::DoorsCopperList()
    : CopperList((uint32_t*)AllocMem(LIST_LENGTH << 2, MEMF_CHIP | MEMF_CLEAR), LIST_LENGTH, true)
{
}

void DoorsCopperList::buildLayout(const Bitmap& title, const Bitmap& cockpit,
                                  const Sprite& leftPost, const Sprite& rightPost,
                                  const Sprite& gauge, const Sprite& nullSprite)
{
    uint32_t* d = data_;

    // ---- title region: playfield (2bp interleaved) ----
    setPlayfield(INDEX_PLAYFIELD, kW, kH, kBP2, /*interleaved*/true,
                 /*hires*/false, /*interlace*/false, /*dualPlayfield*/false,
                 /*holdAndModify*/false, kCenterY);

    setTitlePalette(0, 0, 0);                  // seeded; caller refreshes
    showBitmap(INDEX_TITLE_BPL, title);        // 2bp interleaved = 4 ptr moves

    d[INDEX_SPRITE_COL] = copperMove(color16, 0x000);
    setSpritePostColor(0);
    showSprite(INDEX_SPRITES + 0,  0, leftPost);
    showSprite(INDEX_SPRITES + 2,  1, rightPost);
    showSprite(INDEX_SPRITES + 4,  2, gauge);  // throttle gauge always present during launch
    showSprite(INDEX_SPRITES + 6,  3, nullSprite);
    showSprite(INDEX_SPRITES + 8,  4, nullSprite);
    showSprite(INDEX_SPRITES + 10, 5, nullSprite);
    showSprite(INDEX_SPRITES + 12, 6, nullSprite);
    showSprite(INDEX_SPRITES + 14, 7, nullSprite);
    setGaugeColor(0);

    d[INDEX_COMPASS_WAIT] = copperWait(kDisplayTop + 33 - 1, 0xE0);
    setCompassColor(0);

    // ---- terrain skeleton: band0 sets bplcon0(3P) + modulo; bands 1/2 inherit ----
    d[INDEX_B0_WAIT]     = copperWait(kTerrainLine - 1, 0xE0);
    d[INDEX_B0_BPLCON0]  = copperMove(bplcon0, kBPLCON0_3P);
    d[INDEX_B0_MOD]      = copperMove(bpl1mod, 80);   // 3bp interleaved = (3-1)*40
    d[INDEX_B0_MOD + 1]  = copperMove(bpl2mod, 80);
    // (band geometry / colours / pointers are all poked by update())

    // ---- cockpit region: WAIT, pointers, 3bp, modulo, constant palette ----
    d[INDEX_COCKPIT_WAIT] = copperWait(kCockpitLine - 1, 0xE0);
    showBitmap(INDEX_COCKPIT_BPL, cockpit);    // 3bp interleaved = 6 ptr moves
    d[INDEX_COCKPIT_BPLCON0] = copperMove(bplcon0, kBPLCON0_3P);
    d[INDEX_COCKPIT_MOD]     = copperMove(bpl1mod, 80);
    d[INDEX_COCKPIT_MOD + 1] = copperMove(bpl2mod, 80);
    d[INDEX_COCKPIT_PAL + 0] = copperMove(color00, atariToOCS(0x00));
    d[INDEX_COCKPIT_PAL + 1] = copperMove(color01, atariToOCS(0x04));
    d[INDEX_COCKPIT_PAL + 2] = copperMove(color02, atariToOCS(0x06));
    d[INDEX_COCKPIT_PAL + 3] = copperMove(color03, atariToOCS(0x2C));
    d[INDEX_COCKPIT_PAL + 4] = copperMove(color04, atariToOCS(0x00));
    d[INDEX_COCKPIT_PAL + 5] = copperMove(color05, atariToOCS(0x04));
    d[INDEX_COCKPIT_PAL + 6] = copperMove(color06, atariToOCS(0x06));
    d[INDEX_COCKPIT_PAL + 7] = copperMove(color07, atariToOCS(0x26));

    d[INDEX_TERMINATOR] = copperWait(255, 254);
}

// ---- constant-region per-frame setters ---------------------------------------
void DoorsCopperList::setTitlePalette(uint16_t bg, uint16_t pf0, uint16_t pf1)
{
    data_[INDEX_TITLE_PAL + 0] = copperMove(color00, bg);
    data_[INDEX_TITLE_PAL + 1] = copperMove(color01, pf0);
    data_[INDEX_TITLE_PAL + 2] = copperMove(color02, pf1);
    data_[INDEX_TITLE_PAL + 3] = copperMove(color03, bg);
}

void DoorsCopperList::setSpritePostColor(uint16_t c)
{
    data_[INDEX_SPRITE_COL + 1] = copperMove(color17, c);
}

void DoorsCopperList::setGaugeColor(uint16_t c)
{
    data_[INDEX_GAUGE_COL] = copperMove(0x1AA, c);   // COLOR21 (sprite pair 2/3 pen 01)
}

void DoorsCopperList::setCompassColor(uint16_t c)
{
    data_[INDEX_COMPASS_COL] = copperMove(color01, c);
}

// ---- sliding-door geometry (poked every frame) -------------------------------
void DoorsCopperList::update(uint16_t g2,
                             uint32_t topBase, uint32_t tunBase, uint32_t botBase,
                             uint16_t terrPen0, uint16_t terr1, uint16_t terr2, uint16_t terr3,
                             uint16_t ring0, uint16_t ring1, uint16_t ring2,
                             uint16_t ring3, uint16_t ring4, uint16_t ring5)
{
    uint32_t* d = data_;
    const uint16_t half = kTerrainHeight / 2;                 // = 43
    const bool door = (g2 > 0);
    const uint16_t topH = (uint16_t)(half - g2);              // top/bottom band rows

    // ---- band 0: top terrain (slides up by g2 rows) ----
    emitBpl(d, INDEX_B0_BPL, topBase);
    d[INDEX_B0_COL + 0] = copperMove(color00, terrPen0);      // pen0 = black (terrain & tunnel)
    d[INDEX_B0_COL + 1] = copperMove(color01, terr1);
    d[INDEX_B0_COL + 2] = copperMove(color02, terr2);
    d[INDEX_B0_COL + 3] = copperMove(color03, terr3);
    if (door) {                                               // ring pens 4-6 (pixels 4-6)
        d[INDEX_B0_COL + 4] = copperMove(color04, ring0);     // $08D4
        d[INDEX_B0_COL + 5] = copperMove(color05, ring1);     // $08D5
        d[INDEX_B0_COL + 6] = copperMove(color06, ring2);     // $08D6
    } else {
        d[INDEX_B0_COL + 4] = COPPER_NOP;
        d[INDEX_B0_COL + 5] = COPPER_NOP;
        d[INDEX_B0_COL + 6] = COPPER_NOP;
    }

    // ---- band 1: tunnel reveal (the widening gap) ----
    if (door && topH > 0) {
        d[INDEX_B1_WAIT] = copperWait((uint16_t)(kTerrainLine + topH - 1), 0xE0);
        emitBpl(d, INDEX_B1_BPL, tunBase);
        d[INDEX_B1_COL + 0] = copperMove(color01, ring3);     // tunnel pens 1-3 ($08D7-$08D9)
        d[INDEX_B1_COL + 1] = copperMove(color02, ring4);
        d[INDEX_B1_COL + 2] = copperMove(color03, ring5);
    } else {
        d[INDEX_B1_WAIT] = copperWait(kBand1ParkLine, 0xE0);
        for (int i = 0; i < 6; i++) d[INDEX_B1_BPL + i] = COPPER_NOP;
        d[INDEX_B1_COL + 0] = COPPER_NOP;
        d[INDEX_B1_COL + 1] = COPPER_NOP;
        d[INDEX_B1_COL + 2] = COPPER_NOP;
    }

    // ---- band 2: bottom terrain (slides down) ----
    // Emit only while its WAIT clears the cockpit WAIT by a margin; otherwise the
    // tunnel band above simply extends over the last 1-2 rows (imperceptible).
    const uint16_t botWaitY = (uint16_t)(kTerrainLine + half + g2 - 1);
    if (door && topH > 0 && botWaitY + 2 <= (uint16_t)(kCockpitLine - 1)) {
        d[INDEX_B2_WAIT] = copperWait(botWaitY, 0xE0);
        emitBpl(d, INDEX_B2_BPL, botBase);
        d[INDEX_B2_COL + 0] = copperMove(color01, terr1);     // terrain pens 1-3
        d[INDEX_B2_COL + 1] = copperMove(color02, terr2);
        d[INDEX_B2_COL + 2] = copperMove(color03, terr3);
    } else {
        d[INDEX_B2_WAIT] = copperWait(kBand2ParkLine, 0xE0);
        for (int i = 0; i < 6; i++) d[INDEX_B2_BPL + i] = COPPER_NOP;
        d[INDEX_B2_COL + 0] = COPPER_NOP;
        d[INDEX_B2_COL + 1] = COPPER_NOP;
        d[INDEX_B2_COL + 2] = COPPER_NOP;
    }
}
