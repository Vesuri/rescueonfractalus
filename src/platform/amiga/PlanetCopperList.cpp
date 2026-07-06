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
static const uint16_t kTerrainHeight = 86;   // 43 mode-D terrain rows, each shown on 2 scanlines
// The $316B mode-D DL has 47 rows: the bottom 4 ($1810-$18A0) are the windscreen-bottom
// band (slanted frame; its value-0 triangle corners show the missile fill — a later task).
// They use a distinct band palette, so the band is its own zone between terrain and cockpit.
static const uint16_t kBandHeight    = 8;    // 4 band rows × 2 scanlines
static const uint16_t kViewportHeight = kTerrainHeight + kBandHeight;  // = 94
static const uint16_t kTerrainLine  = kDisplayTop + kTitleHeight;     // = 0x56
static const uint16_t kCockpitLine  = kTerrainLine + kViewportHeight; // = 180 (dashboard start)
static const uint16_t kCenterY      = kDisplayTop + kH / 2;           // = 0x98
static const uint16_t kBPLCON0_3P   = (uint16_t)((3 << PLNCNTSHFT) | USE_BPLCON3);

// Sprite colour-register addresses (custom-chip offsets).
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
#define INDEX_STAR_COL        (INDEX_SPRITES + 16)         // 30: COLOR21,COLOR25,COLOR29 star pens (3)
#define INDEX_COMPASS_WAIT    (INDEX_STAR_COL + 3)         // 33: WAIT(compass scanline) (1)
#define INDEX_COMPASS_COL     (INDEX_COMPASS_WAIT + 1)     // 34: color01 = compass COLPF0 (1)
#define INDEX_VP_WAIT         (INDEX_COMPASS_COL + 1)      // 35: WAIT(kTerrainLine-1) (1)
#define INDEX_VP_BPL          (INDEX_VP_WAIT + 1)          // 34: viewport 3bp ptrs (6)
#define INDEX_VP_BPLCON0      (INDEX_VP_BPL + 6)           // 40: bplcon0 3P (1)
#define INDEX_VP_PAL          (INDEX_VP_BPLCON0 + 1)       // 41: color00..03 (4)
#define INDEX_VP_MOD0         (INDEX_VP_PAL + 4)           // 45: row-0 bpl1mod,bpl2mod (2)
#define INDEX_VP_LINEDOUBLE   (INDEX_VP_MOD0 + 2)          // 47: 93 × (WAIT+2 mod), +2 band palette
// The line-doubling loop runs k=1..kViewportHeight-1.  At k==kTerrainHeight (scanline 172) it
// mirrors the band DLI $6D67, which writes ONLY COLPF0/COLPF1 (the two greys) and leaves COLBK
// and COLPF2 untouched.  So we emit exactly those two MOVEs (color01/color02) and nothing else
// — color00 (COLBK) and color03 (COLPF2=$2A planet) stay the viewport's values, as on the Atari.
#define BAND_BLOCK_WORDS      2
#define INDEX_COCKPIT_WAIT    (INDEX_VP_LINEDOUBLE + 3 * (kViewportHeight - 1) + BAND_BLOCK_WORDS)
// Throttle-gauge re-point (channel 2).  The P0 starfield low sprite owns channel 2 across the
// viewport; its VSTOP is at the cockpit line (180), so its post-VSTOP control-word fetch happens
// at line 180's sprite DMA slot (~cycle 0x14).  These SPR2PT moves MUST be the FIRST moves after
// the cockpit WAIT — at line 180 cycles 0-8, before that fetch — so the channel re-reads the
// gauge sprite's control words instead of the P0-low terminator (which would idle it).  Mirrors
// the flight-scene AH multiplex (FlightCopperList INDEX_AH_SPR).  COLOR21 (pair 2/3 pen 01) then
// switches from the star pen to the gauge-bar colour.  SPR3 (P0 high) terminates on its own.
#define INDEX_GAUGE_PTR       (INDEX_COCKPIT_WAIT + 1)     // SPR2PTH,SPR2PTL -> gauge (2)
#define INDEX_GAUGE_COL       (INDEX_GAUGE_PTR + 2)        // COLOR21 = gauge bar (1)
#define INDEX_COCKPIT_BPL     (INDEX_GAUGE_COL + 1)        // cockpit 3bp ptrs, yOffset 8 (6)
#define INDEX_COCKPIT_BPLCON0 (INDEX_COCKPIT_BPL + 6)      // bplcon0 3P (1)
#define INDEX_COCKPIT_MOD     (INDEX_COCKPIT_BPLCON0 + 1)  // bpl1mod,bpl2mod (2)
#define INDEX_COCKPIT_PAL     (INDEX_COCKPIT_MOD + 2)      // color00..07 (8)
// Cockpit bitmap starts at kCockpitLine=180 (yOffset 8 skips the $350D band).  COLBK splits
// match the launch cockpit: baked color00=$00 covers the black divider strip (180-188); then
// dark-blue $90 dashboard instrument backgrounds (182-251); then black floor (252+).
#define INDEX_DASH_BLUE_WAIT  (INDEX_COCKPIT_PAL + 8)      // WAIT(kCockpitLine+2-1 = 181) (1)
#define INDEX_DASH_BLUE       (INDEX_DASH_BLUE_WAIT + 1)   // color00 = $90 dark blue (dashboard) (1)
#define INDEX_FLOOR_WAIT      (INDEX_DASH_BLUE + 1)        // WAIT(kCockpitLine+72-1 = 251) (1)
#define INDEX_FLOOR           (INDEX_FLOOR_WAIT + 1)       // color00 = black (floor) (1)
#define INDEX_TERMINATOR      (INDEX_FLOOR + 1)            // copperWait(255,254)
#define LIST_LENGTH           (INDEX_TERMINATOR + 1)

PlanetCopperList::PlanetCopperList()
    : CopperList((uint32_t*)AllocMem(LIST_LENGTH << 2, MEMF_CHIP | MEMF_CLEAR), LIST_LENGTH, true)
{
}

void PlanetCopperList::buildLayout(const Bitmap& title, const Bitmap& terrain, const Bitmap& cockpit,
                                     const Sprite& leftPost, const Sprite& rightPost, const Sprite& gauge,
                                     Sprite* const star[6])
{
    uint32_t* d = data_;

    // ---- title region: playfield (2bp interleaved) ----
    setPlayfield(INDEX_PLAYFIELD, kW, kH, kBP2, /*interleaved*/true,
                 /*hires*/false, /*interlace*/false, /*dualPlayfield*/false,
                 /*holdAndModify*/false, kCenterY);
    setTitlePalette(0, 0, 0);                  // seeded; caller refreshes
    showBitmap(INDEX_TITLE_BPL, title);        // 2bp interleaved = 4 ptr moves

    // Sprite colour regs + pointers.  COLOR16 const black, COLOR17 = canopy post (setter).
    // Sprites: 0/1 = canopy posts; 2/3,4/5,6/7 = the three quad starfield players (low/high
    // pairs) — P0 on 2/3, P2 on 4/5, P3 on 6/7.  Channel 2 is re-pointed to the throttle gauge
    // below the starfield (INDEX_GAUGE_* block).
    d[INDEX_SPRITE_COL] = copperMove(color16, 0x000);
    setSpritePostColor(0);
    showSprite(INDEX_SPRITES + 0,  0, leftPost);
    showSprite(INDEX_SPRITES + 2,  1, rightPost);
    showSprite(INDEX_SPRITES + 4,  2, *star[0]);   // P0 low
    showSprite(INDEX_SPRITES + 6,  3, *star[1]);   // P0 high
    showSprite(INDEX_SPRITES + 8,  4, *star[2]);   // P2 low
    showSprite(INDEX_SPRITES + 10, 5, *star[3]);   // P2 high
    showSprite(INDEX_SPRITES + 12, 6, *star[4]);   // P3 low
    showSprite(INDEX_SPRITES + 14, 7, *star[5]);   // P3 high
    setStarColor(0);                           // COLOR21/25/29 star pens (setter)

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
    for (uint16_t k = 1; k < kViewportHeight; k++) {
        d[idx++] = copperWait((uint16_t)(kTerrainLine + k - 1), 0xE0);
        if (k == kTerrainHeight) {
            // Crossing into the windscreen-bottom band (scanline 172): mirror the band DLI
            // $6D67, which writes ONLY COLPF0=#$04 / COLPF1=#$06 (the two cockpit-frame greys).
            // It leaves COLBK and COLPF2 untouched, so — exactly as on the Atari — we emit just
            // these two MOVEs: color00 (COLBK) keeps the viewport's black, and color03 keeps the
            // viewport's COLPF2=$2A (the planet's brightest tone, from $6D0E), which is the
            // salmon shown through the corner-triangle gaps.  The band bitmap is value-2-dominant
            // -> reads as the grey frame with value-3 edges = the salmon planet.  ($6D4F/$6DA1
            // set COLPF2=$2C, but those govern the dashboard below the band, not this band.)
            d[idx++] = copperMove(color01, atariToOCS(0x04));   // grey frame (COLPF0, $6D67)
            d[idx++] = copperMove(color02, atariToOCS(0x06));   // grey frame (COLPF1, $6D67)
        }
        const uint16_t v = (k & 1) ? (uint16_t)80 : (uint16_t)-40;
        d[idx++] = copperMove(bpl1mod, v);
        d[idx++] = copperMove(bpl2mod, v);
    }

    // ---- cockpit region: WAIT, then the channel-2 gauge re-point (MUST be first — see the
    // INDEX_GAUGE_PTR comment), then pointers (skip the 8 modeD band scanlines now drawn by the
    // band above via yOffset=8), 3bp, modulo, constant palette ----
    d[INDEX_COCKPIT_WAIT] = copperWait(kCockpitLine - 1, 0xE0);
    showSprite(INDEX_GAUGE_PTR, 2, gauge);   // re-point channel 2 (P0-low -> throttle gauge)
    setEnergyIndicatorColor(0);              // COLOR21 = gauge bar (setter, at INDEX_GAUGE_COL)
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

    // Dashboard instrument backgrounds = dark blue COLBK $90 (Amiga 182-251); floor black (252+).
    // Only COLBK (color00) changes; baked color00=$00 above covers the divider strip (180-188).
    d[INDEX_DASH_BLUE_WAIT] = copperWait(kCockpitLine + 2 - 1, 0xE0);
    d[INDEX_DASH_BLUE]      = copperMove(color00, atariToOCS(0x90));
    d[INDEX_FLOOR_WAIT] = copperWait(kCockpitLine + 72 - 1, 0xE0);
    d[INDEX_FLOOR]      = copperMove(color00, atariToOCS(0x00));

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

void PlanetCopperList::setEnergyIndicatorColor(uint16_t c)
{
    // COLOR21 (sprite pair 2/3 pen 01) — poked BELOW the starfield, where channel 2 is the gauge.
    data_[INDEX_GAUGE_COL] = copperMove(kColor21, c);
}

void PlanetCopperList::setStarColor(uint16_t c)
{
    // Star pen for all three quad players in the viewport: pair 2/3 (P0), 4/5 (P2), 6/7 (P3).
    // COLOR21 is switched to the gauge colour again below the starfield (setEnergyIndicatorColor).
    data_[INDEX_STAR_COL + 0] = copperMove(kColor21, c);
    data_[INDEX_STAR_COL + 1] = copperMove(kColor25, c);
    data_[INDEX_STAR_COL + 2] = copperMove(kColor29, c);
}

void PlanetCopperList::setCompassColor(uint16_t c)
{
    data_[INDEX_COMPASS_COL] = copperMove(color01, c);
}

void PlanetCopperList::setPlanetBgColor(uint16_t c)
{
    data_[INDEX_VP_PAL] = copperMove(color00, c);   // only the viewport COLBK; the band keeps
                                                    // its own black frame background (baked)
}

// Windscreen-bottom band palette (scanlines 172-179): the band DLI recolours the mode-D
// pens — pen0/COLBK ($00DC), pen1/COLPF0 ($00DD), pen2/COLPF1 ($00DA), pen3/COLPF2 ($00D4,
// grey frame).  Poked per-frame so the descent fade tracks.
