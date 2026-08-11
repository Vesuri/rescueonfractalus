#define ECS_SPECIFIC
#include <hardware/custom.h>
#include <graphics/display.h>
#include <proto/exec.h>
#include <exec/memory.h>
#include "TunnelCopperList.h"
#include "framework/AmigaHardware.h"
#include "framework/Bitmap.h"
#include "framework/Sprite.h"
#include "assets/atari_pal.h"   // atariToOCS() for the constant cockpit palette

// ---- display geometry --------------------------------------------------------
// MUST match the constants in RescueOnFractalus.cpp (same derivation).
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
// First scanline BELOW the energy-gauge dial (see StandbyCopperList's kGaugeBottomLine).
static const uint16_t kGaugeBottomLine = 0x2c + 144 + 56;             // = 244
// Park lines for a collapsed reveal band — mid-region, so its NOPs run in a harmless H-blank
// and never crowd the cockpit pointer writes (same convention as DoorsCopperList).
static const uint16_t kBand1ParkLine = kTerrainLine + 28;
static const uint16_t kBand2ParkLine = kTerrainLine + 57;

// A copper NOP: MOVE to register $1FE (unused) changes nothing.
#define COPPER_NOP copperMove(0x1FE, 0)

// ---- fixed list layout (indices into data_, in 32-bit MOVE/WAIT words) -------
// d[0] = copperWait(16,0) (CopperList ctor).  Title region is identical to
// StandbyCopperList; the terrain region is one tunnel band with pens 4-6.
#define INDEX_PLAYFIELD       1
#define INDEX_BPLCON2         (INDEX_PLAYFIELD + 3)    // BPLCON2 PFxP=1 (game GPRIOR=$14) (1)
#define INDEX_TITLE_PAL       (INDEX_BPLCON2 + 1)      // color00..03 (4)
#define INDEX_TITLE_BPL       (INDEX_TITLE_PAL + 4)    // title bitmap ptrs (2bp = 4)
#define INDEX_SPRITE_COL      (INDEX_TITLE_BPL + 4)    // color16,color17 (2)
#define INDEX_SPRITES         (INDEX_SPRITE_COL + 2)   // 8 sprite ptrs (16)
#define INDEX_ENERGY_COL       (INDEX_SPRITES + 16)     // COLOR21 ($1AA) (1)
#define INDEX_COMPASS_WAIT    (INDEX_ENERGY_COL + 1)    // WAIT(compass scanline) (1)
#define INDEX_COMPASS_COL     (INDEX_COMPASS_WAIT + 1) // color01 = compass COLPF0 (1)
#define INDEX_TERRAIN_WAIT    (INDEX_COMPASS_COL + 1)  // WAIT(kTerrainLine-1) (1)
#define INDEX_TERRAIN_BPL     (INDEX_TERRAIN_WAIT + 1) // tunnel bitmap ptrs (3bp = 6)
#define INDEX_TERRAIN_BPLCON0 (INDEX_TERRAIN_BPL + 6)  // bplcon0 3P (1)
#define INDEX_TERRAIN_MOD     (INDEX_TERRAIN_BPLCON0 + 1) // bpl1mod,bpl2mod (2)
#define INDEX_TERRAIN_PAL     (INDEX_TERRAIN_MOD + 2)  // color00..07 (8: corner-carry pen0 + ring pens 1-6 + spare pen7=black)
// Boost reverse-tunnel REVEAL bands.  The reverse cinematic shows a centre band of ring rows
// [K, 85-K] growing outward over a starfield; the two live in SEPARATE bitmaps and the copper
// picks between them, so the reveal costs two WAITs + 12 pointer moves per frame instead of
// recompositing both into one bitmap every frame.  Geometry mirrors DoorsCopperList's sliding
// gap with g2 = 43-K, so both cinematics share one proven band layout.  All three bands inherit
// the region-top BPLCON0/modulo/palette, so a band needs only its WAIT + bitplane pointers.
// Parked as copper NOPs by default — the FORWARD descent (setRevealBands(0, ...)) is untouched.
#define INDEX_RB1_WAIT        (INDEX_TERRAIN_PAL + 8)  // WAIT(kTerrainLine+K-1) (1)
#define INDEX_RB1_BPL         (INDEX_RB1_WAIT + 1)     // ring bitmap ptrs at row K (6)
#define INDEX_RB2_WAIT        (INDEX_RB1_BPL + 6)      // WAIT(kTerrainLine+86-K-1) (1)
#define INDEX_RB2_BPL         (INDEX_RB2_WAIT + 1)     // star bitmap ptrs at row 86-K (6)
#define INDEX_COCKPIT_WAIT    (INDEX_RB2_BPL + 6)      // WAIT(kCockpitLine-1) (1)
#define INDEX_BANDTOP_COL00   (INDEX_COCKPIT_WAIT + 1) // boost band-top color00, BEFORE the bitplane ptrs (1)
#define INDEX_COCKPIT_BPL     (INDEX_BANDTOP_COL00 + 1)// cockpit bitmap ptrs (3bp = 6)
#define INDEX_COCKPIT_BPLCON0 (INDEX_COCKPIT_BPL + 6)  // bplcon0 3P (1)
#define INDEX_COCKPIT_MOD     (INDEX_COCKPIT_BPLCON0 + 1) // bpl1mod,bpl2mod (2)
#define INDEX_COCKPIT_PAL     (INDEX_COCKPIT_MOD + 2)  // color01..07 (7; color00 carries in from the viewport)
// Windscreen-bottom band: cockpit bitmap's top 8 scanlines (mode-D $350D frame).  The L/R
// corner triangles are value-0 = color00.  During the tunnel reveal the Atari clears the
// quad-width canopy-post player ($0C88-$0C8F) top-down, so the green corner recedes and the
// tunnel (purple) behind shows through.
//
// We render this with NO per-band poke: the viewport carries the corner colour in color00.
// The tunnel field's value-0 (the exit-clear black) was routed to the spare register color07
// (RescueOnFractalus.cpp kGtia10P remap), so color00 is unused by the ring field and holds the
// tunnel purple (mem[$08D8], set in the terrain palette).  It flows across the viewport/band
// boundary untouched; a single moving WAIT (setBandReveal) flips color00 to green (door $0071)
// from the reveal boundary down.  At the exit clear color00 = mem[$08D8] -> 0, so the corner
// goes black in step with the tunnel, while the field's black comes from color07 independently.
#define INDEX_BAND_GREEN_WAIT (INDEX_COCKPIT_PAL + 7)  // WAIT(boundary) (1)
#define INDEX_BAND_GREEN      (INDEX_BAND_GREEN_WAIT + 1) // color00 = green door (1)
#define INDEX_DASH_BG_WAIT    (INDEX_BAND_GREEN + 1)   // WAIT(kCockpitLine+8-1) (1)
#define INDEX_DASH_BG         (INDEX_DASH_BG_WAIT + 1) // color00 = black (divider strip 180-188) (1)
// Dashboard instrument backgrounds = dark blue COLBK $90 (Amiga 182-251); floor black (252+).
#define INDEX_DASH_BLUE_WAIT  (INDEX_DASH_BG + 1)      // WAIT(kCockpitLine+10-1 = 181) (1)
#define INDEX_DASH_BLUE       (INDEX_DASH_BLUE_WAIT + 1) // color00 = $90 dark blue (dashboard) (1)
// Energy bar (ch2 / COLOR21) → black below the gauge DIAL, not at the floor: the Amiga bar is one
// solid 56-row sprite whose VSTART tracks the fuel, so below full fuel its bottom hangs past the
// dial, where the Atari's per-row P1 strip just stops.  See the same block in StandbyCopperList.
#define INDEX_GAUGE_BOT_WAIT  (INDEX_DASH_BLUE + 1)     // WAIT(kGaugeBottomLine-1 = 243) (1)
#define INDEX_GAUGE_BOT       (INDEX_GAUGE_BOT_WAIT + 1)// COLOR21 = black (1)
#define INDEX_FLOOR_WAIT      (INDEX_GAUGE_BOT + 1)     // WAIT(kCockpitLine+80-1 = 251) (1)
#define INDEX_FLOOR           (INDEX_FLOOR_WAIT + 1)    // color00 = black (floor) (1)
#define INDEX_TERMINATOR      (INDEX_FLOOR + 1)         // copperWait(255,254)
#define LIST_LENGTH           (INDEX_TERMINATOR + 1)

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

// parkBand: collapse one reveal band — its WAIT moves mid-region and its 6 pointer moves
// become NOPs, so the band above simply extends through it.
static void parkBand(uint32_t* d, uint32_t waitAt, uint16_t parkLine)
{
    d[waitAt] = copperWait(parkLine, 0xE0);
    for (int i = 1; i <= 6; i++) d[waitAt + i] = COPPER_NOP;
}

TunnelCopperList::TunnelCopperList()
    : CopperList((uint32_t*)AllocMem(LIST_LENGTH << 2, MEMF_CHIP | MEMF_CLEAR), LIST_LENGTH, true)
{
}

void TunnelCopperList::buildLayout(const Bitmap& title, const Bitmap& tunnel, const Bitmap& cockpit,
                                   const Sprite& leftPost, const Sprite& rightPost,
                                   const Sprite& gauge, const Sprite& nullSprite)
{
    uint32_t* d = data_;

    // ---- title region: playfield (2bp interleaved) ----
    setPlayfield(INDEX_PLAYFIELD, kW, kH, kBP2, /*interleaved*/true,
                 /*hires*/false, /*interlace*/false, /*dualPlayfield*/false,
                 /*holdAndModify*/false, kCenterY);

    // BPLCON2 PFxP=1 = sprite pair 0 (the canopy posts, ch0/ch1) in FRONT of the playfield, pairs
    // 1+ (the throttle gauge on ch2) BEHIND it — the game's GPRIOR=$14 priority, same value as
    // PlanetCopperList and the Standby's CPU write.
    // MUST be set explicitly: the boost reverse cinematic reaches this list straight from the
    // FlightCopperList, whose dashboard band leaves BPLCON2 at PFxP=0 (ALL sprites behind the
    // playfield, for the HUD gauges) and never restores it — so without this the rings/starfield
    // would draw over the pillars.  It was PFxP=4 (all sprites in front) until 2026-08-11, which
    // bought the pillars nothing they don't already get at 1 and put the energy gauge on top of the
    // dashboard for the whole reverse cinematic (user-reported).  ⚠ This value is also what the
    // NEXT scene inherits — BPLCON2 persists across copper lists; see setSpritePriority().
    // (CopperList::setPlayfield sets only bitplane geometry, not BPLCON2.)
    d[INDEX_BPLCON2] = copperMove(bplcon2, (uint16_t)((1u << 3) | 1u));

    setTitlePalette(0, 0, 0);                  // seeded; caller refreshes
    showBitmap(INDEX_TITLE_BPL, title);        // 2bp interleaved = 4 ptr moves

    // Sprite colour regs: COLOR16 = black (const), COLOR17 = canopy-post grey (setter).
    d[INDEX_SPRITE_COL] = copperMove(color16, 0x000);
    setSpritePostColor(0);
    // Sprite pointers: 0=left post, 1=right post, 2=throttle gauge (cinematic active),
    // 3..7 = null.  All const here (the gauge is always present during the cinematic).
    showSprite(INDEX_SPRITES + 0,  0, leftPost);
    showSprite(INDEX_SPRITES + 2,  1, rightPost);
    showSprite(INDEX_SPRITES + 4,  2, gauge);
    showSprite(INDEX_SPRITES + 6,  3, nullSprite);
    showSprite(INDEX_SPRITES + 8,  4, nullSprite);
    showSprite(INDEX_SPRITES + 10, 5, nullSprite);
    showSprite(INDEX_SPRITES + 12, 6, nullSprite);
    showSprite(INDEX_SPRITES + 14, 7, nullSprite);
    setEnergyIndicatorColor(0);                          // COLOR21 gauge bar (setter)

    // ---- compass band: color01 = compass COLPF0 ($00CF) for the mode-4 compass line ----
    d[INDEX_COMPASS_WAIT] = copperWait(kDisplayTop + 33 - 1, 0xE0);
    setCompassColor(0);                        // poked from $00CF

    // ---- terrain region: one full-height tunnel band (3bp) ----
    d[INDEX_TERRAIN_WAIT] = copperWait(kTerrainLine - 1, 0xE0);
    showBitmap(INDEX_TERRAIN_BPL, tunnel);     // 3bp interleaved = 6 ptr moves
    d[INDEX_TERRAIN_BPLCON0] = copperMove(bplcon0, kBPLCON0_3P);
    d[INDEX_TERRAIN_MOD]     = copperMove(bpl1mod, 80);   // 3bp interleaved = (3-1)*40
    d[INDEX_TERRAIN_MOD + 1] = copperMove(bpl2mod, 80);
    setTunnelColors(0, 0, 0, 0, 0, 0, 0, 0);   // seeded; caller refreshes (ring cycles)
    // Reveal bands collapsed: the forward descent is one full-height band from `tunnel`.
    parkBand(d, INDEX_RB1_WAIT, kBand1ParkLine);
    parkBand(d, INDEX_RB2_WAIT, kBand2ParkLine);

    // ---- cockpit region: WAIT, pointers, 3bp, modulo, constant palette ----
    d[INDEX_COCKPIT_WAIT] = copperWait(kCockpitLine - 1, 0xE0);
    // Boost band-top color00, executed HERE (right after the WAIT, before the 6 bitplane-pointer
    // moves that overrun ~16px into the band's first line).  Default = no-op (color00 carries in);
    // setBandTopColor00() activates it for the boost expansion phase.
    d[INDEX_BANDTOP_COL00] = copperMove(color31, 0);
    showBitmap(INDEX_COCKPIT_BPL, cockpit);    // 3bp interleaved = 6 ptr moves
    d[INDEX_COCKPIT_BPLCON0] = copperMove(bplcon0, kBPLCON0_3P);
    d[INDEX_COCKPIT_MOD]     = copperMove(bpl1mod, 80);
    d[INDEX_COCKPIT_MOD + 1] = copperMove(bpl2mod, 80);
    // Cockpit palette (constant immediates, as in StandbyCopperList).  color00 is NOT set
    // here: it carries the windscreen-band corner colour (tunnel purple mem[$08D8]) forward
    // from the terrain palette across the viewport/band boundary (see the band note above).
    //   01=COLPF0 $04, 02=COLPF1 $06, 03=COLPF2 $2C salmon, 04..06 mirror 00..02,
    //   07=COLPF3 $26 red (bit-7 chars via plane3).  color07 carried the field's exit-clear
    //   black through the viewport; this line (executed at the band top) restores it to $26
    //   for the dashboard below (the band itself uses only values 0/1/2, never 7).
    d[INDEX_COCKPIT_PAL + 0] = copperMove(color01, atariToOCS(0x04));
    d[INDEX_COCKPIT_PAL + 1] = copperMove(color02, atariToOCS(0x06));
    d[INDEX_COCKPIT_PAL + 2] = copperMove(color03, atariToOCS(0x2C));
    d[INDEX_COCKPIT_PAL + 3] = copperMove(color04, atariToOCS(0x00));
    d[INDEX_COCKPIT_PAL + 4] = copperMove(color05, atariToOCS(0x04));
    d[INDEX_COCKPIT_PAL + 5] = copperMove(color06, atariToOCS(0x06));
    d[INDEX_COCKPIT_PAL + 6] = copperMove(color07, atariToOCS(0x26));

    // Band reveal split (seeded all-green = reveal not started; setBandReveal moves it).
    setBandReveal(0, atariToOCS(0xC8));

    // Below the 8-row band: black divider strip (Amiga 180-188), dark-blue $90 dashboard
    // instrument backgrounds (182-251), then black floor (252+).  Only COLBK (color00) changes.
    d[INDEX_DASH_BG_WAIT] = copperWait(kCockpitLine + 8 - 1, 0xE0);
    d[INDEX_DASH_BG]      = copperMove(color00, atariToOCS(0x00));
    d[INDEX_DASH_BLUE_WAIT] = copperWait(kCockpitLine + 10 - 1, 0xE0);
    d[INDEX_DASH_BLUE]      = copperMove(color00, atariToOCS(0x90));
    d[INDEX_GAUGE_BOT_WAIT] = copperWait(kGaugeBottomLine - 1, 0xE0);
    d[INDEX_GAUGE_BOT]      = copperMove(color21, 0x000);   // clip the bar at its dial
    d[INDEX_FLOOR_WAIT] = copperWait(kCockpitLine + 80 - 1, 0xE0);
    d[INDEX_FLOOR]      = copperMove(color00, atariToOCS(0x00));

    d[INDEX_TERMINATOR] = copperWait(255, 254);
}

// ---- per-frame setters -------------------------------------------------------
void TunnelCopperList::setTitlePalette(uint16_t bg, uint16_t pf0, uint16_t pf1)
{
    data_[INDEX_TITLE_PAL + 0] = copperMove(color00, bg);
    data_[INDEX_TITLE_PAL + 1] = copperMove(color01, pf0);
    data_[INDEX_TITLE_PAL + 2] = copperMove(color02, pf1);
    data_[INDEX_TITLE_PAL + 3] = copperMove(color03, bg);
}

void TunnelCopperList::setSpritePostColor(uint16_t c)
{
    data_[INDEX_SPRITE_COL + 1] = copperMove(color17, c);
}

void TunnelCopperList::setEnergyIndicatorColor(uint16_t c)
{
    data_[INDEX_ENERGY_COL] = copperMove(color21, c);   // sprite pair 1 pen 01 (the gauge bar)
}

void TunnelCopperList::setCompassColor(uint16_t c)
{
    data_[INDEX_COMPASS_COL] = copperMove(color01, c);
}

// setBandReveal(): the green->purple corner reveal.  greenLine = the first band scanline
// (0..8, relative to kCockpitLine) that still shows the green door — i.e. the topmost
// still-set canopy-post-player scanline.  Lines above it keep color00's carried-in colour
// (tunnel purple, from the terrain palette — no band-top poke); this moving WAIT flips color00
// to green from greenLine down.  greenLine 0 = whole band green (reveal not started); 8 = whole
// band purple.
void TunnelCopperList::setBandReveal(uint16_t greenLine, uint16_t greenColor)
{
    if (greenLine > 8) greenLine = 8;
    data_[INDEX_BAND_GREEN_WAIT] = copperWait((uint16_t)(kCockpitLine + greenLine - 1), 0xE0);
    data_[INDEX_BAND_GREEN]      = copperMove(color00, greenColor);
}

void TunnelCopperList::disableBandReveal()
{
    data_[INDEX_BAND_GREEN] = copperMove(color31, 0);   // no-op: leave color00 = the viewport's
}

// setRevealBands(): point each band of the terrain region at the bitmap that owns it.
//   K  = the first viewport row showing RINGS (the reverse-tunnel reveal band is [K, 85-K]);
//        K >= 43 means nothing is revealed yet, K == 0 means the rings fill the region.
//   ringBase / starBase = the two source bitmaps' row-0 addresses.
// Band 0 (the region-top pointers) takes whichever bitmap owns the top rows, so a caller that
// wants a plain full-height ring band — the forward descent — passes K = 0 and both bases equal.
void TunnelCopperList::setRevealBands(uint16_t K, uint32_t ringBase, uint32_t starBase)
{
    uint32_t* d = data_;
    if (K == 0) {                                   // rings fill the region (forward descent too)
        emitBpl(d, INDEX_TERRAIN_BPL, ringBase);
        parkBand(d, INDEX_RB1_WAIT, kBand1ParkLine);
        parkBand(d, INDEX_RB2_WAIT, kBand2ParkLine);
        return;
    }
    emitBpl(d, INDEX_TERRAIN_BPL, starBase);        // band 0 = the stars above the reveal
    if (K >= kTerrainHeight / 2) {                  // nothing revealed: stars fill the region
        parkBand(d, INDEX_RB1_WAIT, kBand1ParkLine);
        parkBand(d, INDEX_RB2_WAIT, kBand2ParkLine);
        return;
    }
    d[INDEX_RB1_WAIT] = copperWait((uint16_t)(kTerrainLine + K - 1), 0xE0);
    emitBpl(d, INDEX_RB1_BPL, ringBase + (uint32_t)K * 120u);
    // Band 2 restores the stars below the reveal.  At K < 2 its WAIT would sit within a line or
    // two of the cockpit WAIT, too tight for 6 pointer moves — park it and let the ring band run
    // to the bottom instead (one row wrong on the very last reveal step, imperceptible).
    if (K >= 2) {
        d[INDEX_RB2_WAIT] = copperWait((uint16_t)(kTerrainLine + kTerrainHeight - K - 1), 0xE0);
        emitBpl(d, INDEX_RB2_BPL, starBase + (uint32_t)(kTerrainHeight - K) * 120u);
    } else {
        parkBand(d, INDEX_RB2_WAIT, kBand2ParkLine);
    }
}

void TunnelCopperList::setBandTopColor00(bool active, uint16_t color)
{
    data_[INDEX_BANDTOP_COL00] = active ? copperMove(color00, color) : copperMove(color31, 0);
}

// pen0 = color00 = the windscreen-band corner colour (tunnel purple mem[$08D8]); it is unused
// by the ring field (value-0 was remapped to pen7) and carries into the band.  pen7 = color07 =
// the field's exit-clear black (mem[$02C0]); the cockpit palette restores color07=$26 at the
// band top for the dashboard.
void TunnelCopperList::setTunnelColors(uint16_t pen0, uint16_t pen1, uint16_t pen2, uint16_t pen3,
                                       uint16_t pen4, uint16_t pen5, uint16_t pen6, uint16_t pen7)
{
    data_[INDEX_TERRAIN_PAL + 0] = copperMove(color00, pen0);
    data_[INDEX_TERRAIN_PAL + 1] = copperMove(color01, pen1);
    data_[INDEX_TERRAIN_PAL + 2] = copperMove(color02, pen2);
    data_[INDEX_TERRAIN_PAL + 3] = copperMove(color03, pen3);
    data_[INDEX_TERRAIN_PAL + 4] = copperMove(color04, pen4);
    data_[INDEX_TERRAIN_PAL + 5] = copperMove(color05, pen5);
    data_[INDEX_TERRAIN_PAL + 6] = copperMove(color06, pen6);
    data_[INDEX_TERRAIN_PAL + 7] = copperMove(color07, pen7);
}
