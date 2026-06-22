#define ECS_SPECIFIC
#include <hardware/custom.h>
#include <graphics/display.h>
#include <proto/exec.h>
#include <exec/memory.h>
#include "FlightCopperList.h"
#include "framework/AmigaHardware.h"
#include "framework/Bitmap.h"
#include "framework/Sprite.h"
#include "assets/atari_pal.h"   // atariToOCS() for the constant cockpit palette

// ---- display geometry (MUST match RescueOnFractalus.cpp / PlanetCopperList.cpp) -----
static const uint16_t kW            = 320;
static const uint16_t kH            = 216;
static const uint8_t  kBP2          = 2;
static const uint16_t kDisplayTop   = 0x2c;
static const uint16_t kTitleHeight  = 42;
static const uint16_t kTerrainHeight = 86;   // 43 mode-D terrain rows, each shown on 2 scanlines
// The $316B flight DL has 47 mode-D rows, not 43: the bottom 4 ($2090-$21B0) are the
// wing-clearance band (windscreen-bottom frame + the salmon clearance bars + terrain
// triangles), shown on scanlines 172-179.  They use a DIFFERENT palette than the terrain
// (set by the band DLI), so the band is a distinct palette zone between terrain and cockpit.
static const uint16_t kBandHeight    = 8;    // 4 band rows × 2 scanlines
static const uint16_t kViewportHeight = kTerrainHeight + kBandHeight;  // = 94
static const uint16_t kTerrainLine  = kDisplayTop + kTitleHeight;     // = 0x56
static const uint16_t kBandLine     = kTerrainLine + kTerrainHeight;  // = 172 (wing band start)
static const uint16_t kCockpitLine  = kTerrainLine + kViewportHeight; // = 180 (dashboard start)
static const uint16_t kCenterY      = kDisplayTop + kH / 2;           // = 0x98
static const uint16_t kBPLCON0_3P   = (uint16_t)((3 << PLNCNTSHFT) | USE_BPLCON3);

// Sprite colour-register addresses (custom-chip offsets).  Flight sprite layout:
//   pair 0/1 (ch0 leftPost, ch1 leftTriangle)   -> COLOR17 pen01 (windscreen-frame grey)
//   pair 2/3 (ch2 rightPost, ch3 rightTriangle) -> COLOR21 pen01 (windscreen-frame grey)
//   pair 4/5 (ch5 energy indicator)             -> COLOR25 pen01
//   pair 6/7 (ch6 altimeter terrain / ch7 ship) -> COLOR29 pen01 / COLOR30 pen10 (two pens,
//            two colours from one pair: terrain draws plane0, ship draws plane1)
static const uint16_t kColor18 = 0x1A4;   // pair 0/1 pen 10 (left band TRIANGLE — darker grey, $00CF)
static const uint16_t kColor21 = 0x1AA;   // pair 2/3 pen 01 (windscreen A-pillar grey, right post)
static const uint16_t kColor22 = 0x1AC;   // pair 2/3 pen 10 (right band TRIANGLE — darker grey, $00CF)
static const uint16_t kColor25 = 0x1B2;   // pair 4/5 pen 01 (energy-indicator bar)
static const uint16_t kColor29 = 0x1BA;   // pair 6/7 pen 01 (altimeter terrain-height bar, P0)
static const uint16_t kColor30 = 0x1BC;   // pair 6/7 pen 10 (altimeter ship-height bar, M3)

// ---- fixed list layout (indices into data_, in 32-bit MOVE/WAIT words) -------
// d[0] = copperWait(16,0) (CopperList ctor).  The line-doubling band is 85 rows ×
// (WAIT + BPL1MOD + BPL2MOD) = 255 words (rows 1..kTerrainHeight-1).
#define INDEX_PLAYFIELD       1                            // setPlayfield: 3
#define INDEX_TITLE_PAL       (INDEX_PLAYFIELD + 3)        // 4:  color00..03 (4)
#define INDEX_TITLE_BPL       (INDEX_TITLE_PAL + 4)        // 8:  title 2bp ptrs (4)
#define INDEX_SPRITE_COL      (INDEX_TITLE_BPL + 4)        // 12: color16,color17,COLOR21,COLOR18,COLOR22 (5)
#define INDEX_SPRITES         (INDEX_SPRITE_COL + 5)       // 17: 8 sprite ptrs (16)
#define INDEX_ENERGY_COL       (INDEX_SPRITES + 16)         // 31: COLOR25 (energy bar) (1)
#define INDEX_ALTIM_COL       (INDEX_ENERGY_COL + 1)        // 32: COLOR29 (altimeter terrain-height bar P0) (1)
#define INDEX_SHIP_COL        (INDEX_ALTIM_COL + 1)        // 33: COLOR30 (altimeter ship-height bar M3) (1)
// Compass band: between the title text and the viewport, re-point color01 to the compass
// COLPF0 ($00CF, dark grey) for the mode-4 compass line — the $49EE slot-0 DLI's colour.
#define INDEX_COMPASS_WAIT    (INDEX_SHIP_COL + 1)         // 33: WAIT(compass scanline) (1)
#define INDEX_COMPASS_COL     (INDEX_COMPASS_WAIT + 1)     // 32: color01 = compass COLPF0 (housing) (1)
#define INDEX_COMPASS_COL3    (INDEX_COMPASS_COL + 1)      // 33: color03 = compass COLPF2 (needle salmon) (1)
#define INDEX_VP_WAIT         (INDEX_COMPASS_COL3 + 1)     // 34: WAIT(kTerrainLine-1) (1)
#define INDEX_VP_BPL          (INDEX_VP_WAIT + 1)          // 32: viewport 3bp ptrs (6)
#define INDEX_VP_BPLCON0      (INDEX_VP_BPL + 6)           // 38: bplcon0 3P (1)
#define INDEX_VP_PAL          (INDEX_VP_BPLCON0 + 1)       // 39: color00..03 (4)
#define INDEX_VP_MOD0         (INDEX_VP_PAL + 4)           // 43: row-0 bpl1mod,bpl2mod (2)
#define INDEX_VP_LINEDOUBLE   (INDEX_VP_MOD0 + 2)          // 45: 93 × (WAIT+2 mod), +1 band block
// The line-doubling loop runs k=1..kViewportHeight-1.  At k==kTerrainHeight (scanline 172) the
// flight band DLI changes ONLY COLPF2 (terrain sky $B8 -> band frame grey $06) — COLBK/COLPF0/
// COLPF1 stay the terrain values (measured live).  So, faithfully, the band block emits ONLY
// color03 (= COLPF2) and inherits color00/01/02 from the terrain palette (VP_PAL) — which means
// the band bg/pens fade salmon->brown WITH the terrain automatically.  BPLCON2 stays at its
// init value 0x09 (sprites behind the playfield) throughout — no per-band flip needed.
#define BAND_BLOCK_WORDS      1
#define INDEX_BAND_BLOCK      (INDEX_VP_LINEDOUBLE + 3 * (kTerrainHeight - 1) + 1)  // band color03 (1)
#define INDEX_COCKPIT_WAIT    (INDEX_VP_LINEDOUBLE + 3 * (kViewportHeight - 1) + BAND_BLOCK_WORDS)
#define INDEX_COCKPIT_BPL     (INDEX_COCKPIT_WAIT + 1)     // cockpit 3bp ptrs, yOffset 8 (6)
#define INDEX_COCKPIT_BPLCON0 (INDEX_COCKPIT_BPL + 6)      // bplcon0 3P (1)
#define INDEX_COCKPIT_MOD     (INDEX_COCKPIT_BPLCON0 + 1)  // bpl1mod,bpl2mod (2)
#define INDEX_COCKPIT_PAL     (INDEX_COCKPIT_MOD + 2)      // color00..07 (8)
#define INDEX_TERMINATOR      (INDEX_COCKPIT_PAL + 8)      // copperWait(255,254)
#define LIST_LENGTH           (INDEX_TERMINATOR + 1)

FlightCopperList::FlightCopperList()
    : CopperList((uint32_t*)AllocMem(LIST_LENGTH << 2, MEMF_CHIP | MEMF_CLEAR), LIST_LENGTH, true)
{
}

void FlightCopperList::buildLayout(const Bitmap& title, const Bitmap& terrain, const Bitmap& cockpit,
                                   const Sprite& leftPost, const Sprite& leftTri,
                                   const Sprite& rightPost, const Sprite& rightTri,
                                   const Sprite& nullSprite)
{
    uint32_t* d = data_;

    // ---- title region: playfield (2bp interleaved) ----
    setPlayfield(INDEX_PLAYFIELD, kW, kH, kBP2, /*interleaved*/true,
                 /*hires*/false, /*interlace*/false, /*dualPlayfield*/false,
                 /*holdAndModify*/false, kCenterY);
    setTitlePalette(0, 0, 0);                  // seeded; caller refreshes
    showBitmap(INDEX_TITLE_BPL, title);        // 2bp interleaved = 4 ptr moves

    // Sprite colour regs + pointers.  COLOR16 const black; COLOR17 (pair 0/1) + COLOR21
    // (pair 2/3) = the windscreen-frame grey (setSpritePostColor sets both).  Sprites:
    //   0 = leftPost (left A-pillar + inner-16px of the left band triangle)
    //   1 = leftTriangle (outer-16px of the left band triangle)
    //   2 = rightPost (right A-pillar + inner-16px of the right band triangle)
    //   3 = rightTriangle (outer-16px of the right band triangle)
    //   4 = null; 5 = energy bar; 6 = altimeter terrain; 7 = altimeter ship (poked later).
    d[INDEX_SPRITE_COL] = copperMove(color16, 0x000);
    setSpritePostColor(0);                               // color17 + COLOR21 (A-pillars, pen01 grey)
    setTriangleColor(0);                                 // COLOR18 + COLOR22 (band triangles, pen10 darker grey)
    showSprite(INDEX_SPRITES + 0,  0, leftPost);
    showSprite(INDEX_SPRITES + 2,  1, leftTri);
    showSprite(INDEX_SPRITES + 4,  2, rightPost);
    showSprite(INDEX_SPRITES + 6,  3, rightTri);
    showSprite(INDEX_SPRITES + 8,  4, nullSprite);
    showSprite(INDEX_SPRITES + 10, 5, nullSprite);
    showSprite(INDEX_SPRITES + 12, 6, nullSprite);
    showSprite(INDEX_SPRITES + 14, 7, nullSprite);
    setEnergyIndicatorColor(0);                 // COLOR25 (setter) — energy bar (sprite pair 4/5)
    setAltimeterColor(0);                       // COLOR29 (setter) — altimeter terrain bar (pair 6/7 pen01)
    setAltimeterShipColor(0);                   // COLOR30 (setter) — altimeter ship bar (pair 6/7 pen10)

    // ---- compass band: re-point color01 to the compass COLPF0 for the mode-4 compass
    // line (title-bitmap row 33 = scanline kDisplayTop+33).  Title text above it keeps the
    // frame-top color01 (titlePf0); the viewport WAIT below resets the palette. ----
    d[INDEX_COMPASS_WAIT] = copperWait(kDisplayTop + 33 - 1, 0xE0);
    setCompassColor(0);                        // color01 = housing (poked from $00CF)
    setCompassNeedleColor(0);                  // color03 = needle/heading-letter (salmon)

    // ---- viewport region: WAIT, pointers, 2bp->3bp, palette, line-doubling band ----
    d[INDEX_VP_WAIT] = copperWait(kTerrainLine - 1, 0xE0);
    showBitmap(INDEX_VP_BPL, terrain);         // 3bp interleaved = 6 ptr moves
    d[INDEX_VP_BPLCON0] = copperMove(bplcon0, kBPLCON0_3P);
    // Flight terrain palette.  Empirically the terrain bitmap encodes pen0 = terrain
    // body, pen1 = sky, pen2 = dots, pen3 = highlight (NOT 0=background).  pen0 is the
    // salmon→brown fade target; seed the fully-faded values, the native code pokes them.
    setTerrainPalette(atariToOCS(0x14), atariToOCS(0x2A), atariToOCS(0x20), atariToOCS(0x18));
    // Line doubling: each 120-byte interleaved row shown on 2 scanlines via bplmod toggle
    // (-40 rewind to repeat, +80 advance).  Row 0's -40 here; rows 1.. alternate.
    d[INDEX_VP_MOD0 + 0] = copperMove(bpl1mod, (uint16_t)-40);
    d[INDEX_VP_MOD0 + 1] = copperMove(bpl2mod, (uint16_t)-40);
    uint32_t idx = INDEX_VP_LINEDOUBLE;
    for (uint16_t k = 1; k < kViewportHeight; k++) {
        d[idx++] = copperWait((uint16_t)(kTerrainLine + k - 1), 0xE0);
        if (k == kTerrainHeight) {
            // Crossing into the wing-clearance band (scanline kBandLine = 172): the band DLI
            // changes ONLY COLPF2 to the frame grey, so emit just color03 (seeded 0, poked by
            // setBandPalette).  color00/01/02 stay the terrain pens (VP_PAL) — they fade with it.
            d[idx++] = copperMove(color03, 0);
        }
        const uint16_t v = (k & 1) ? (uint16_t)80 : (uint16_t)-40;
        d[idx++] = copperMove(bpl1mod, v);
        d[idx++] = copperMove(bpl2mod, v);
    }

    // ---- cockpit region: WAIT, pointers (skip the 8 modeD frame scanlines now drawn by
    // the wing band above via yOffset=8), 3bp, modulo, constant palette ----
    d[INDEX_COCKPIT_WAIT] = copperWait(kCockpitLine - 1, 0xE0);
    showBitmap(INDEX_COCKPIT_BPL, cockpit, 1, 1, 0, 8);   // yOffset 8 scanlines: skip the $350D band rows
    d[INDEX_COCKPIT_BPLCON0] = copperMove(bplcon0, kBPLCON0_3P);
    d[INDEX_COCKPIT_MOD]     = copperMove(bpl1mod, 80);
    d[INDEX_COCKPIT_MOD + 1] = copperMove(bpl2mod, 80);
    // Cockpit palette: cockpit DLIs reload hardcoded immediates; constant at fade 16.
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

// Wing-clearance band (scanlines 172-179): the band DLI changes ONLY COLPF2 (mode-D value-3
// = the grey windscreen frame, $00D4=$06).  COLBK/COLPF0/COLPF1 (value-0/1/2 = terrain bg /
// clearance bars / dots) are NOT touched by the DLI — they stay the terrain pens (VP_PAL) and
// so fade salmon→brown WITH the terrain.  So only color03 is poked here.
void FlightCopperList::setBandPalette(uint16_t pen3)
{
    // The band DLI changes only COLPF2 (the windscreen-frame grey); color00/01/02 are
    // inherited from the terrain palette (which fades), so only color03 is poked here.
    data_[INDEX_BAND_BLOCK] = copperMove(color03, pen3);
}

// ---- per-frame setters -------------------------------------------------------
void FlightCopperList::setTitlePalette(uint16_t bg, uint16_t pf0, uint16_t pf1)
{
    data_[INDEX_TITLE_PAL + 0] = copperMove(color00, bg);
    data_[INDEX_TITLE_PAL + 1] = copperMove(color01, pf0);
    data_[INDEX_TITLE_PAL + 2] = copperMove(color02, pf1);
    data_[INDEX_TITLE_PAL + 3] = copperMove(color03, bg);
}

void FlightCopperList::setSpritePostColor(uint16_t c)
{
    // The windscreen frame uses TWO sprite pairs (0/1 = left post+triangle, 2/3 = right),
    // both the same grey, so set both pens: COLOR17 (pair 0/1) and COLOR21 (pair 2/3).
    data_[INDEX_SPRITE_COL + 1] = copperMove(color17, c);
    data_[INDEX_SPRITE_COL + 2] = copperMove(kColor21, c);
}

// The band windscreen-corner TRIANGLES are drawn on the sprites' SECOND bitplane (pen 10),
// so they take a separate, darker grey than the A-pillars (pen 01) — COLOR18 (pair 0/1) +
// COLOR22 (pair 2/3).  Faithful source = the band DLI $4A40's COLPM0/1 = mem[$00CF] ($04).
void FlightCopperList::setTriangleColor(uint16_t c)
{
    data_[INDEX_SPRITE_COL + 3] = copperMove(kColor18, c);
    data_[INDEX_SPRITE_COL + 4] = copperMove(kColor22, c);
}

void FlightCopperList::setEnergyIndicatorColor(uint16_t c)
{
    data_[INDEX_ENERGY_COL] = copperMove(kColor25, c);  // COLOR25 = sprite pair 4/5 pen 01
}

void FlightCopperList::setAltimeterColor(uint16_t c)
{
    data_[INDEX_ALTIM_COL] = copperMove(kColor29, c);   // COLOR29 = sprite pair 6/7 pen 01 (terrain bar)
}

void FlightCopperList::setAltimeterShipColor(uint16_t c)
{
    data_[INDEX_SHIP_COL] = copperMove(kColor30, c);    // COLOR30 = sprite pair 6/7 pen 10 (ship bar)
}

void FlightCopperList::setCompassColor(uint16_t c)
{
    data_[INDEX_COMPASS_COL] = copperMove(color01, c);
}

void FlightCopperList::setCompassNeedleColor(uint16_t c)
{
    data_[INDEX_COMPASS_COL3] = copperMove(color03, c);
}

void FlightCopperList::setTerrainPalette(uint16_t pen0, uint16_t pen1, uint16_t pen2, uint16_t pen3)
{
    data_[INDEX_VP_PAL + 0] = copperMove(color00, pen0);
    data_[INDEX_VP_PAL + 1] = copperMove(color01, pen1);
    data_[INDEX_VP_PAL + 2] = copperMove(color02, pen2);
    data_[INDEX_VP_PAL + 3] = copperMove(color03, pen3);
}

void FlightCopperList::setTerrainPen0(uint16_t pen0)
{
    data_[INDEX_VP_PAL + 0] = copperMove(color00, pen0);
}

void FlightCopperList::setHudSprite(int slot, const Sprite& s)
{
    showSprite(INDEX_SPRITES + slot * 2, slot, s);
}
