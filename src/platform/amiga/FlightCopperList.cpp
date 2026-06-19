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
static const uint16_t kTerrainHeight = 86;   // 43 mode-D rows, each shown on 2 scanlines
static const uint16_t kTerrainLine  = kDisplayTop + kTitleHeight;     // = 0x56
static const uint16_t kCockpitLine  = kTerrainLine + kTerrainHeight;  // = 172
static const uint16_t kCenterY      = kDisplayTop + kH / 2;           // = 0x98
static const uint16_t kBPLCON0_3P   = (uint16_t)((3 << PLNCNTSHFT) | USE_BPLCON3);

// Sprite colour-register addresses (custom-chip offsets), as in PlanetCopperList.
static const uint16_t kColor21 = 0x1AA;   // sprite pair 2/3 pen 01 (energy-indicator bar)
static const uint16_t kColor25 = 0x1B2;   // sprite pair 4/5 pen 01 (altimeter terrain-height bar, P0)
static const uint16_t kColor29 = 0x1BA;   // sprite pair 6/7 pen 01 (altimeter ship-height bar, M3)
static const uint16_t kColor26 = 0x1B4;   // sprite pair 4/5 pen 10 (wing-clearance centre symbol, sprite 5)

// ---- fixed list layout (indices into data_, in 32-bit MOVE/WAIT words) -------
// d[0] = copperWait(16,0) (CopperList ctor).  The line-doubling band is 85 rows ×
// (WAIT + BPL1MOD + BPL2MOD) = 255 words (rows 1..kTerrainHeight-1).
#define INDEX_PLAYFIELD       1                            // setPlayfield: 3
#define INDEX_TITLE_PAL       (INDEX_PLAYFIELD + 3)        // 4:  color00..03 (4)
#define INDEX_TITLE_BPL       (INDEX_TITLE_PAL + 4)        // 8:  title 2bp ptrs (4)
#define INDEX_SPRITE_COL      (INDEX_TITLE_BPL + 4)        // 12: color16,color17 (2)
#define INDEX_SPRITES         (INDEX_SPRITE_COL + 2)       // 14: 8 sprite ptrs (16)
#define INDEX_ENERGY_COL       (INDEX_SPRITES + 16)         // 30: COLOR21 (1)
#define INDEX_ALTIM_COL       (INDEX_ENERGY_COL + 1)        // 31: COLOR25 (altimeter terrain-height bar P0) (1)
#define INDEX_SHIP_COL        (INDEX_ALTIM_COL + 1)        // 32: COLOR29 (altimeter ship-height bar M3) (1)
#define INDEX_SYM_COL         (INDEX_SHIP_COL + 1)         // 33: COLOR26 (wing-clearance centre symbol, sprite 5 pen 10) (1)
// Compass band: between the title text and the viewport, re-point color01 to the compass
// COLPF0 ($00CF, dark grey) for the mode-4 compass line — the $49EE slot-0 DLI's colour.
#define INDEX_COMPASS_WAIT    (INDEX_SYM_COL + 1)          // 34: WAIT(compass scanline) (1)
#define INDEX_COMPASS_COL     (INDEX_COMPASS_WAIT + 1)     // 32: color01 = compass COLPF0 (housing) (1)
#define INDEX_COMPASS_COL3    (INDEX_COMPASS_COL + 1)      // 33: color03 = compass COLPF2 (needle salmon) (1)
#define INDEX_VP_WAIT         (INDEX_COMPASS_COL3 + 1)     // 34: WAIT(kTerrainLine-1) (1)
#define INDEX_VP_BPL          (INDEX_VP_WAIT + 1)          // 32: viewport 3bp ptrs (6)
#define INDEX_VP_BPLCON0      (INDEX_VP_BPL + 6)           // 38: bplcon0 3P (1)
#define INDEX_VP_PAL          (INDEX_VP_BPLCON0 + 1)       // 39: color00..03 (4)
#define INDEX_VP_MOD0         (INDEX_VP_PAL + 4)           // 43: row-0 bpl1mod,bpl2mod (2)
#define INDEX_VP_LINEDOUBLE   (INDEX_VP_MOD0 + 2)          // 45: 85 × (WAIT+2 mod) (255)
#define INDEX_COCKPIT_WAIT    (INDEX_VP_LINEDOUBLE + 3 * (kTerrainHeight - 1))  // 300
#define INDEX_COCKPIT_BPL     (INDEX_COCKPIT_WAIT + 1)     // 301: cockpit 3bp ptrs (6)
#define INDEX_COCKPIT_BPLCON0 (INDEX_COCKPIT_BPL + 6)      // 307: bplcon0 3P (1)
#define INDEX_COCKPIT_MOD     (INDEX_COCKPIT_BPLCON0 + 1)  // 308: bpl1mod,bpl2mod (2)
#define INDEX_COCKPIT_PAL     (INDEX_COCKPIT_MOD + 2)      // 310: color00..07 (8)
// Wing-clearance band priority: the HUD sprites sit BEHIND the cockpit playfield
// (BPLCON2 PF1P=PF2P=1, set once at init).  For the 8-scanline wing band the centre
// plane symbol (sprite 5, pair 4/5) must be IN FRONT, so raise the playfield-priority
// code to 3 (PF behind sprite pairs 0/1, 2/3, 4/5) for the band, then restore 1 below
// so the altimeter (sprite 4, same pair, at raster ~188) stays behind the cockpit.
#define INDEX_PRIO_HI         (INDEX_COCKPIT_PAL + 8)      // 318: BPLCON2 = 0x1B (sprites 4/5 in front) (1)
#define INDEX_PRIO_WAIT       (INDEX_PRIO_HI + 1)          // 319: WAIT(kCockpitLine+7) (1)
#define INDEX_PRIO_LO         (INDEX_PRIO_WAIT + 1)        // 320: BPLCON2 = 0x09 (restore) (1)
#define INDEX_TERMINATOR      (INDEX_PRIO_LO + 1)          // 321: copperWait(255,254)
#define LIST_LENGTH           (INDEX_TERMINATOR + 1)       // 322

FlightCopperList::FlightCopperList()
    : CopperList((uint32_t*)AllocMem(LIST_LENGTH << 2, MEMF_CHIP | MEMF_CLEAR), LIST_LENGTH, true)
{
}

void FlightCopperList::buildLayout(const Bitmap& title, const Bitmap& terrain, const Bitmap& cockpit,
                                   const Sprite& leftPost, const Sprite& rightPost, const Sprite& gauge,
                                   const Sprite& nullSprite)
{
    uint32_t* d = data_;

    // ---- title region: playfield (2bp interleaved) ----
    setPlayfield(INDEX_PLAYFIELD, kW, kH, kBP2, /*interleaved*/true,
                 /*hires*/false, /*interlace*/false, /*dualPlayfield*/false,
                 /*holdAndModify*/false, kCenterY);
    setTitlePalette(0, 0, 0);                  // seeded; caller refreshes
    showBitmap(INDEX_TITLE_BPL, title);        // 2bp interleaved = 4 ptr moves

    // Sprite colour regs + pointers.  COLOR16 const black, COLOR17 = canopy post (setter).
    // Sprites: 0/1 = canopy posts, 2 = throttle gauge, 3..7 = null (HUD sprites are poked
    // in later via setHudSprite as the flight VBI port maps the players/missiles).
    d[INDEX_SPRITE_COL] = copperMove(color16, 0x000);
    setSpritePostColor(0);
    showSprite(INDEX_SPRITES + 0,  0, leftPost);
    showSprite(INDEX_SPRITES + 2,  1, rightPost);
    showSprite(INDEX_SPRITES + 4,  2, gauge);
    showSprite(INDEX_SPRITES + 6,  3, nullSprite);
    showSprite(INDEX_SPRITES + 8,  4, nullSprite);
    showSprite(INDEX_SPRITES + 10, 5, nullSprite);
    showSprite(INDEX_SPRITES + 12, 6, nullSprite);
    showSprite(INDEX_SPRITES + 14, 7, nullSprite);
    setEnergyIndicatorColor(0);                          // COLOR21 (setter)
    setAltimeterColor(0);                       // COLOR25 (setter) — altimeter terrain bar (sprite pair 4/5)
    setAltimeterShipColor(0);                   // COLOR29 (setter) — altimeter ship bar M3 (sprite pair 6/7)
    setWingSymbolColor(0);                       // COLOR26 (setter) — wing-clearance centre symbol (sprite 5 pen 10)

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
    for (uint16_t k = 1; k < kTerrainHeight; k++) {
        d[idx++] = copperWait((uint16_t)(kTerrainLine + k - 1), 0xE0);
        const uint16_t v = (k & 1) ? (uint16_t)80 : (uint16_t)-40;
        d[idx++] = copperMove(bpl1mod, v);
        d[idx++] = copperMove(bpl2mod, v);
    }

    // ---- cockpit region: WAIT, pointers, 3bp, modulo, constant palette ----
    d[INDEX_COCKPIT_WAIT] = copperWait(kCockpitLine - 1, 0xE0);
    showBitmap(INDEX_COCKPIT_BPL, cockpit);    // 3bp interleaved = 6 ptr moves
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

    // Wing-clearance band: sprite pair 4/5 in front of the cockpit playfield (PF1P=PF2P=3)
    // for the 8 band scanlines, then restore PF1P=PF2P=1 so the altimeter/gauge below stay
    // behind the cockpit.  Constant — never poked.
    d[INDEX_PRIO_HI]   = copperMove(bplcon2, (uint16_t)((3u << 3) | 3u));   // 0x1B
    d[INDEX_PRIO_WAIT] = copperWait(kCockpitLine + 8 - 1, 0xE0);
    d[INDEX_PRIO_LO]   = copperMove(bplcon2, (uint16_t)((1u << 3) | 1u));   // 0x09

    d[INDEX_TERMINATOR] = copperWait(255, 254);
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
    data_[INDEX_SPRITE_COL + 1] = copperMove(color17, c);
}

void FlightCopperList::setEnergyIndicatorColor(uint16_t c)
{
    data_[INDEX_ENERGY_COL] = copperMove(kColor21, c);
}

void FlightCopperList::setAltimeterColor(uint16_t c)
{
    data_[INDEX_ALTIM_COL] = copperMove(kColor25, c);   // COLOR25 = sprite pair 4/5 pen 01
}

void FlightCopperList::setAltimeterShipColor(uint16_t c)
{
    data_[INDEX_SHIP_COL] = copperMove(kColor29, c);    // COLOR29 = sprite pair 6/7 pen 01
}

void FlightCopperList::setWingSymbolColor(uint16_t c)
{
    data_[INDEX_SYM_COL] = copperMove(kColor26, c);     // COLOR26 = sprite pair 4/5 pen 10
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
