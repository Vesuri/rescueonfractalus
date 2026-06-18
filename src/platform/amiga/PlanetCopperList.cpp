#define ECS_SPECIFIC
#include <hardware/custom.h>
#include <graphics/display.h>
#include <proto/exec.h>
#include <exec/memory.h>
#include "PlanetCopperList.h"
#include "framework/AmigaHardware.h"
#include "framework/Bitmap.h"
#include "framework/Sprite.h"
#include "assets/atari_pal.h"   // atariToOCS() for the constant viewport/cockpit palettes

// ---- display geometry (MUST match RescueOnFractalus.cpp) ---------------------
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

// Sprite colour-register addresses (custom-chip offsets), as in buildCopperList.
static const uint16_t kColor21 = 0x1AA;   // sprite pair 2/3 pen 01 (gauge bar)
static const uint16_t kColor25 = 0x1B2;   // sprite pair 4/5 pen 01 (starfield)
static const uint16_t kColor29 = 0x1BA;   // sprite pair 6/7 pen 01 (starfield)

// ---- fixed list layout (indices into data_, in 32-bit MOVE/WAIT words) -------
// d[0] = copperWait(16,0) (CopperList ctor).  The line-doubling band is 85 rows ×
// (WAIT + BPL1MOD + BPL2MOD) = 255 words (rows 1..kTerrainHeight-1; row 0's modulo
// is INDEX_VP_MOD0).
#define INDEX_PLAYFIELD       1                            // setPlayfield: 3
#define INDEX_TITLE_PAL       (INDEX_PLAYFIELD + 3)        // 4:  color00..03 (4)
#define INDEX_TITLE_BPL       (INDEX_TITLE_PAL + 4)        // 8:  title 2bp ptrs (4)
#define INDEX_SPRITE_COL      (INDEX_TITLE_BPL + 4)        // 12: color16,color17 (2)
#define INDEX_SPRITES         (INDEX_SPRITE_COL + 2)       // 14: 8 sprite ptrs (16)
#define INDEX_GAUGE_COL       (INDEX_SPRITES + 16)         // 30: COLOR21 (1)
#define INDEX_STAR_COL        (INDEX_GAUGE_COL + 1)        // 31: COLOR25,COLOR29 (2)
#define INDEX_COMPASS_WAIT    (INDEX_STAR_COL + 2)         // 33: WAIT(compass scanline) (1)
#define INDEX_COMPASS_COL     (INDEX_COMPASS_WAIT + 1)     // 34: color01 = compass COLPF0 (1)
#define INDEX_VP_WAIT         (INDEX_COMPASS_COL + 1)      // 35: WAIT(kTerrainLine-1) (1)
#define INDEX_VP_BPL          (INDEX_VP_WAIT + 1)          // 34: viewport 3bp ptrs (6)
#define INDEX_VP_BPLCON0      (INDEX_VP_BPL + 6)           // 40: bplcon0 3P (1)
#define INDEX_VP_PAL          (INDEX_VP_BPLCON0 + 1)       // 41: color00..03 (4)
#define INDEX_VP_MOD0         (INDEX_VP_PAL + 4)           // 45: row-0 bpl1mod,bpl2mod (2)
#define INDEX_VP_LINEDOUBLE   (INDEX_VP_MOD0 + 2)          // 47: 85 × (WAIT+2 mod) (255)
#define INDEX_COCKPIT_WAIT    (INDEX_VP_LINEDOUBLE + 3 * (kTerrainHeight - 1))  // 302
#define INDEX_COCKPIT_BPL     (INDEX_COCKPIT_WAIT + 1)     // 303: cockpit 3bp ptrs (6)
#define INDEX_COCKPIT_BPLCON0 (INDEX_COCKPIT_BPL + 6)      // 309: bplcon0 3P (1)
#define INDEX_COCKPIT_MOD     (INDEX_COCKPIT_BPLCON0 + 1)  // 310: bpl1mod,bpl2mod (2)
#define INDEX_COCKPIT_PAL     (INDEX_COCKPIT_MOD + 2)      // 312: color00..07 (8)
#define INDEX_TERMINATOR      (INDEX_COCKPIT_PAL + 8)      // 320: copperWait(255,254)
#define LIST_LENGTH           (INDEX_TERMINATOR + 1)       // 321

PlanetCopperList::PlanetCopperList()
    : CopperList((uint32_t*)AllocMem(LIST_LENGTH << 2, MEMF_CHIP | MEMF_CLEAR), LIST_LENGTH, true)
{
}

void PlanetCopperList::buildLayout(const Bitmap& title, const Bitmap& terrain, const Bitmap& cockpit,
                                     const Sprite& leftPost, const Sprite& rightPost, const Sprite& gauge,
                                     const Sprite& nullSprite, const Sprite& star0, const Sprite& star1,
                                     const Sprite& star2)
{
    uint32_t* d = data_;

    // ---- title region: playfield (2bp interleaved) ----
    setPlayfield(INDEX_PLAYFIELD, kW, kH, kBP2, /*interleaved*/true,
                 /*hires*/false, /*interlace*/false, /*dualPlayfield*/false,
                 /*holdAndModify*/false, kCenterY);
    setTitlePalette(0, 0, 0);                  // seeded; caller refreshes
    showBitmap(INDEX_TITLE_BPL, title);        // 2bp interleaved = 4 ptr moves

    // Sprite colour regs + pointers.  COLOR16 const black, COLOR17 = canopy post (setter).
    // Sprites: 0/1 = canopy posts, 2 = throttle gauge, 3/7 = null, 4/5/6 = starfield.
    d[INDEX_SPRITE_COL] = copperMove(color16, 0x000);
    setSpritePostColor(0);
    showSprite(INDEX_SPRITES + 0,  0, leftPost);
    showSprite(INDEX_SPRITES + 2,  1, rightPost);
    showSprite(INDEX_SPRITES + 4,  2, gauge);
    showSprite(INDEX_SPRITES + 6,  3, nullSprite);
    showSprite(INDEX_SPRITES + 8,  4, star0);
    showSprite(INDEX_SPRITES + 10, 5, star1);
    showSprite(INDEX_SPRITES + 12, 6, star2);
    showSprite(INDEX_SPRITES + 14, 7, nullSprite);
    setGaugeColor(0);                          // COLOR21 (setter)
    setStarColor(0);                           // COLOR25/29 (setter)

    // ---- compass band: color01 = compass COLPF0 ($00CF) for the mode-4 compass line ----
    // (the compass glyph is decoded into the title bitmap rows ~33-40; without this band
    // its value-1 pixels show in the title text colour — the "yellow compass" bug.)
    d[INDEX_COMPASS_WAIT] = copperWait(kDisplayTop + 33 - 1, 0xE0);
    setCompassColor(0);                        // poked from $00CF

    // ---- viewport region: WAIT, pointers, 2bp->3bp, palette, line-doubling band ----
    d[INDEX_VP_WAIT] = copperWait(kTerrainLine - 1, 0xE0);
    showBitmap(INDEX_VP_BPL, terrain);         // 3bp interleaved = 6 ptr moves
    d[INDEX_VP_BPLCON0] = copperMove(bplcon0, kBPLCON0_3P);
    // Viewport palette: color00 = COLBK (space, setter); 01..03 = the $24/$28/$2A
    // star/planet tones (constant, baked at fade 16 — see $6CC2 DLI palette).
    d[INDEX_VP_PAL + 0] = copperMove(color00, 0);              // seeded; caller refreshes (COLBK)
    d[INDEX_VP_PAL + 1] = copperMove(color01, atariToOCS(0x24));
    d[INDEX_VP_PAL + 2] = copperMove(color02, atariToOCS(0x28));
    d[INDEX_VP_PAL + 3] = copperMove(color03, atariToOCS(0x2A));
    // Line doubling: each 120-byte interleaved row is shown on 2 scanlines by toggling
    // the bitplane modulo at end-of-line — -40 rewinds plane1 to repeat the row, +80
    // advances to the next.  Row 0's -40 is set here; rows 1..kTerrainHeight-1 alternate
    // +80 (odd) / -40 (even) at each line's H-blank.  Constant every frame.
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
void PlanetCopperList::setTitlePalette(uint16_t bg, uint16_t pf0, uint16_t pf1)
{
    data_[INDEX_TITLE_PAL + 0] = copperMove(color00, bg);
    data_[INDEX_TITLE_PAL + 1] = copperMove(color01, pf0);
    data_[INDEX_TITLE_PAL + 2] = copperMove(color02, pf1);
    data_[INDEX_TITLE_PAL + 3] = copperMove(color03, bg);
}

void PlanetCopperList::setSpritePostColor(uint16_t c)
{
    data_[INDEX_SPRITE_COL + 1] = copperMove(color17, c);
}

void PlanetCopperList::setGaugeColor(uint16_t c)
{
    data_[INDEX_GAUGE_COL] = copperMove(kColor21, c);
}

void PlanetCopperList::setStarColor(uint16_t c)
{
    data_[INDEX_STAR_COL + 0] = copperMove(kColor25, c);
    data_[INDEX_STAR_COL + 1] = copperMove(kColor29, c);
}

void PlanetCopperList::setCompassColor(uint16_t c)
{
    data_[INDEX_COMPASS_COL] = copperMove(color01, c);
}

void PlanetCopperList::setPlanetBgColor(uint16_t c)
{
    data_[INDEX_VP_PAL] = copperMove(color00, c);
}
