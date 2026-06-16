#define ECS_SPECIFIC
#include <hardware/custom.h>
#include <graphics/display.h>
#include <proto/exec.h>
#include <exec/memory.h>
#include "StandbyCopperList.h"
#include "../framework/AmigaHardware.h"
#include "../framework/Bitmap.h"
#include "../framework/Sprite.h"
#include "../assets/atari_pal.h"   // atariToOCS() for the constant cockpit palette

// ---- display geometry --------------------------------------------------------
// MUST match the constants in RescueOnFractalus.cpp (same derivation).  The
// Standby copper layout buildCopperList() emits is reproduced here byte-for-byte.
static const uint16_t kW            = 320;
static const uint16_t kH            = 216;
static const uint8_t  kBP2          = 2;
static const uint8_t  kBP3          = 3;
static const uint16_t kDisplayTop   = 0x2c;
static const uint16_t kTitleHeight  = 42;
static const uint16_t kTerrainHeight = 86;
static const uint16_t kTerrainLine  = kDisplayTop + kTitleHeight;     // = 0x56
static const uint16_t kCockpitLine  = kTerrainLine + kTerrainHeight;  // = 172
static const uint16_t kCenterY      = kDisplayTop + kH / 2;           // = 0x98
static const uint16_t kBPLCON0_3P   = (uint16_t)((3 << PLNCNTSHFT) | USE_BPLCON3);

// ---- fixed list layout (indices into data_, in 32-bit MOVE/WAIT words) -------
// d[0] = copperWait(16,0) (CopperList ctor).  setPlayfield now emits 3 words
// (BPLCON0 + BPL1MOD/BPL2MOD — the per-region-varying regs); the constant playfield
// registers are set once by AmigaHardware::setPlayfield (see RescueOnFractalus::initialize).
#define INDEX_PLAYFIELD       1
#define INDEX_TITLE_PAL       (INDEX_PLAYFIELD + 3)    // color00..03 (4)
#define INDEX_TITLE_BPL       (INDEX_TITLE_PAL + 4)    // 17: title bitmap ptrs (2bp = 4)
#define INDEX_SPRITE_COL      (INDEX_TITLE_BPL + 4)    // 21: color16,color17 (2)
#define INDEX_SPRITES         (INDEX_SPRITE_COL + 2)   // 23: 8 sprite ptrs (16)
#define INDEX_GAUGE_COL       (INDEX_SPRITES + 16)     // 39: COLOR21 ($1AA) (1)
#define INDEX_TERRAIN_WAIT    (INDEX_GAUGE_COL + 1)    // 40: WAIT(kTerrainLine-1) (1)
#define INDEX_TERRAIN_BPL     (INDEX_TERRAIN_WAIT + 1) // 41: terrain bitmap ptrs (3bp = 6)
#define INDEX_TERRAIN_BPLCON0 (INDEX_TERRAIN_BPL + 6)  // 47: bplcon0 3P (1)
#define INDEX_TERRAIN_MOD     (INDEX_TERRAIN_BPLCON0 + 1) // 48: bpl1mod,bpl2mod (2)
#define INDEX_TERRAIN_PAL     (INDEX_TERRAIN_MOD + 2)  // 50: color00..03 (4)
#define INDEX_COCKPIT_WAIT    (INDEX_TERRAIN_PAL + 4)  // 54: WAIT(kCockpitLine-1) (1)
#define INDEX_COCKPIT_BPL     (INDEX_COCKPIT_WAIT + 1) // 55: cockpit bitmap ptrs (3bp = 6)
#define INDEX_COCKPIT_BPLCON0 (INDEX_COCKPIT_BPL + 6)  // 61: bplcon0 3P (1)
#define INDEX_COCKPIT_MOD     (INDEX_COCKPIT_BPLCON0 + 1) // 62: bpl1mod,bpl2mod (2)
#define INDEX_COCKPIT_PAL     (INDEX_COCKPIT_MOD + 2)  // 64: color00..07 (8)
#define INDEX_TERMINATOR      (INDEX_COCKPIT_PAL + 8)  // 72: copperWait(255,254)
#define LIST_LENGTH           (INDEX_TERMINATOR + 1)   // 73

StandbyCopperList::StandbyCopperList()
    : CopperList((uint32_t*)AllocMem(LIST_LENGTH << 2, MEMF_CHIP | MEMF_CLEAR), LIST_LENGTH, true)
{
}

void StandbyCopperList::buildLayout(const Bitmap& title, const Bitmap& terrain, const Bitmap& cockpit,
                                    const Sprite& leftPost, const Sprite& rightPost, const Sprite& nullSprite)
{
    uint32_t* d = data_;

    // ---- title region: playfield (2bp interleaved) ----
    // Emits BPLCON0 (2bp) + BPL1MOD/BPL2MOD for the title band; the constant playfield
    // registers (incl. BPLCON2 = PF priority) are set once in RescueOnFractalus::initialize.
    setPlayfield(INDEX_PLAYFIELD, kW, kH, kBP2, /*interleaved*/true,
                 /*hires*/false, /*interlace*/false, /*dualPlayfield*/false,
                 /*holdAndModify*/false, kCenterY);

    // Title palette + bitmap pointers (palette refreshed each frame via setters).
    setTitlePalette(0, 0, 0);                  // seeded; caller refreshes
    showBitmap(INDEX_TITLE_BPL, title);        // 2bp interleaved = 4 ptr moves

    // Sprite colour regs: COLOR16 = black (const), COLOR17 = canopy-post grey (setter).
    d[INDEX_SPRITE_COL] = copperMove(color16, 0x000);
    setSpritePostColor(0);
    // Sprite pointers: 0=left post, 1=right post, 2=null (gauge via setSprite2),
    // 3..7=null.  All const except sprite 2.
    showSprite(INDEX_SPRITES + 0,  0, leftPost);
    showSprite(INDEX_SPRITES + 2,  1, rightPost);
    showSprite(INDEX_SPRITES + 4,  2, nullSprite);
    showSprite(INDEX_SPRITES + 6,  3, nullSprite);
    showSprite(INDEX_SPRITES + 8,  4, nullSprite);
    showSprite(INDEX_SPRITES + 10, 5, nullSprite);
    showSprite(INDEX_SPRITES + 12, 6, nullSprite);
    showSprite(INDEX_SPRITES + 14, 7, nullSprite);
    setGaugeColor(0);                          // COLOR21 gauge bar (setter)

    // ---- terrain region: WAIT (end of prev line), pointers, 2bp->3bp, modulo ----
    d[INDEX_TERRAIN_WAIT] = copperWait(kTerrainLine - 1, 0xE0);
    showBitmap(INDEX_TERRAIN_BPL, terrain);    // 3bp interleaved = 6 ptr moves
    d[INDEX_TERRAIN_BPLCON0] = copperMove(bplcon0, kBPLCON0_3P);
    d[INDEX_TERRAIN_MOD]     = copperMove(bpl1mod, 80);   // 3bp interleaved = (3-1)*40
    d[INDEX_TERRAIN_MOD + 1] = copperMove(bpl2mod, 80);
    setTerrainPalette(0, 0, 0, 0);             // seeded; caller refreshes

    // ---- cockpit region: WAIT, pointers, 3bp, modulo, constant palette ----
    d[INDEX_COCKPIT_WAIT] = copperWait(kCockpitLine - 1, 0xE0);
    showBitmap(INDEX_COCKPIT_BPL, cockpit);    // 3bp interleaved = 6 ptr moves
    d[INDEX_COCKPIT_BPLCON0] = copperMove(bplcon0, kBPLCON0_3P);
    d[INDEX_COCKPIT_MOD]     = copperMove(bpl1mod, 80);
    d[INDEX_COCKPIT_MOD + 1] = copperMove(bpl2mod, 80);
    // Cockpit palette: the cockpit DLIs ($6D4F/$6D67/$6D7C) reload these registers
    // with hardcoded immediates (NOT the title/terrain shadows) — constant, and the
    // fade is 16 throughout StandbyCopperList's life, so bake them once here.
    //   00=COLBK $00 black, 01=COLPF0 $04, 02=COLPF1 $06, 03=COLPF2 $2C salmon,
    //   04..06 mirror 00..02, 07=COLPF3 $26 red (bit-7 chars via plane3 -> colour 7).
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

// ---- per-frame setters -------------------------------------------------------
void StandbyCopperList::setTitlePalette(uint16_t bg, uint16_t pf0, uint16_t pf1)
{
    data_[INDEX_TITLE_PAL + 0] = copperMove(color00, bg);
    data_[INDEX_TITLE_PAL + 1] = copperMove(color01, pf0);
    data_[INDEX_TITLE_PAL + 2] = copperMove(color02, pf1);
    data_[INDEX_TITLE_PAL + 3] = copperMove(color03, bg);
}

void StandbyCopperList::setSpritePostColor(uint16_t c)
{
    data_[INDEX_SPRITE_COL + 1] = copperMove(color17, c);
}

void StandbyCopperList::setSprite2(const Sprite& s)
{
    showSprite(INDEX_SPRITES + 4, 2, s);
}

void StandbyCopperList::setGaugeColor(uint16_t c)
{
    data_[INDEX_GAUGE_COL] = copperMove(0x1AA, c);   // COLOR21 (sprite pair 2/3 pen 01)
}

void StandbyCopperList::setTerrainPalette(uint16_t p0, uint16_t p1, uint16_t p2, uint16_t p3)
{
    data_[INDEX_TERRAIN_PAL + 0] = copperMove(color00, p0);
    data_[INDEX_TERRAIN_PAL + 1] = copperMove(color01, p1);
    data_[INDEX_TERRAIN_PAL + 2] = copperMove(color02, p2);
    data_[INDEX_TERRAIN_PAL + 3] = copperMove(color03, p3);
}

void StandbyCopperList::setTerrainBgColor(uint16_t c)
{
    data_[INDEX_TERRAIN_PAL + 3] = copperMove(color03, c);
}
