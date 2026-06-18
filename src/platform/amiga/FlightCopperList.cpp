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

// Sprite colour-register address (custom-chip offset), as in PlanetCopperList.
static const uint16_t kColor21 = 0x1AA;   // sprite pair 2/3 pen 01 (gauge bar)

// ---- fixed list layout (indices into data_, in 32-bit MOVE/WAIT words) -------
// d[0] = copperWait(16,0) (CopperList ctor).  The line-doubling band is 85 rows ×
// (WAIT + BPL1MOD + BPL2MOD) = 255 words (rows 1..kTerrainHeight-1).
#define INDEX_PLAYFIELD       1                            // setPlayfield: 3
#define INDEX_TITLE_PAL       (INDEX_PLAYFIELD + 3)        // 4:  color00..03 (4)
#define INDEX_TITLE_BPL       (INDEX_TITLE_PAL + 4)        // 8:  title 2bp ptrs (4)
#define INDEX_SPRITE_COL      (INDEX_TITLE_BPL + 4)        // 12: color16,color17 (2)
#define INDEX_SPRITES         (INDEX_SPRITE_COL + 2)       // 14: 8 sprite ptrs (16)
#define INDEX_GAUGE_COL       (INDEX_SPRITES + 16)         // 30: COLOR21 (1)
#define INDEX_VP_WAIT         (INDEX_GAUGE_COL + 1)        // 31: WAIT(kTerrainLine-1) (1)
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
#define INDEX_TERMINATOR      (INDEX_COCKPIT_PAL + 8)      // 318: copperWait(255,254)
#define LIST_LENGTH           (INDEX_TERMINATOR + 1)       // 319

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
    setGaugeColor(0);                          // COLOR21 (setter)

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

void FlightCopperList::setGaugeColor(uint16_t c)
{
    data_[INDEX_GAUGE_COL] = copperMove(kColor21, c);
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
