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
static const uint16_t kColor27 = 0x1B6;   // pair 4/5 pen 11 (player laser shot — independent of energy's pen01=COLOR25)
static const uint16_t kColor30 = 0x1BC;   // pair 6/7 pen 10 (altimeter ship-height bar, M3)
static const uint16_t kColor23 = 0x1AE;   // pair 2/3 pen 11 (Targeting-Scope P3 object — unused by the frame's pen01/pen10)
static const uint16_t kColor31 = 0x1BE;   // pair 6/7 pen 11 (Main-Window P3 object — unused by the altimeter's pen01/pen10)

// ---- fixed list layout (indices into data_, in 32-bit MOVE/WAIT words) -------
// d[0] = copperWait(16,0) (CopperList ctor).  The line-doubling band is 85 rows ×
// (WAIT + BPL1MOD + BPL2MOD) = 255 words (rows 1..kTerrainHeight-1).
#define INDEX_PLAYFIELD       1                            // setPlayfield: 3
// BPLCON2 sprite-vs-playfield priority.  The windscreen frame spans TWO sprite pairs:
// 0/1 (left post+triangle) and 2/3 (right post+triangle).  The global init value (PFxP=1)
// puts only sprites 0/1 in FRONT of the playfield, leaving 2-7 BEHIND — so the right frame
// (pair 2/3) gets occluded by the band bitmap while the left doesn't.  Flight needs sprites
// 0-3 in front, so set PFxP=2 here.  The HUD on 5/6/7 stays behind (as the gauge always was).
#define INDEX_BPLCON2         (INDEX_PLAYFIELD + 3)        // 4: BPLCON2 PFxP=2 (1)
#define INDEX_TITLE_PAL       (INDEX_BPLCON2 + 1)          // 5:  color00..03 (4)
#define INDEX_TITLE_BPL       (INDEX_TITLE_PAL + 4)        // 8:  title 2bp ptrs (4)
#define INDEX_SPRITE_COL      (INDEX_TITLE_BPL + 4)        // 12: color16,color17,COLOR21,COLOR18,COLOR22 (5)
#define INDEX_SPRITES         (INDEX_SPRITE_COL + 5)       // 17: 8 sprite ptrs (16)
#define INDEX_ENERGY_COL       (INDEX_SPRITES + 16)         // 31: COLOR25 (energy bar) (1)
#define INDEX_ALTIM_COL       (INDEX_ENERGY_COL + 1)        // 32: COLOR29 (altimeter terrain-height bar P0) (1)
#define INDEX_SHIP_COL        (INDEX_ALTIM_COL + 1)        // 33: COLOR30 (altimeter ship-height bar M3) (1)
// Player laser shot (Atari player P2, on the otherwise-idle sprite ch4): COLOR27 = pair 4/5
// pen 11, set once near the top and holding all frame (the shot is viewport-only + the only
// pen-11 user of pair 4/5, so it never clashes with the energy bar on ch5 pen01=COLOR25).
#define INDEX_SHOT_COL        (INDEX_SHIP_COL + 1)         // COLOR27 (laser shot colour) (1)
// Targeting-Scope P3 object (Atari player 3, reused onto sprite ch3 below the frame — see
// INDEX_SCOPE_SPR).  COLOR23 = pair 2/3 pen 11 (unused by the frame's post/triangle pens),
// set near the top and holding all frame; poked per frame from mem[$00D9] (COLPM3, cyan).
#define INDEX_SCOPE_COL       (INDEX_SHOT_COL + 1)         // COLOR23 (Targeting-Scope P3 colour) (1)
// Main-Window P3 object: the gun-emplacement / flying-saucer target rendered as a sprite
// IN FRONT of the terrain (viewport BPLCON2 PFxP=4), on ch7 (altimeter ship — free in the
// viewport) multiplexed via SPR7PT (see INDEX_ALTIM_SHIP_SPR).  COLOR31 = pair 6/7 pen 11
// (unused by the altimeter's COLOR29/30), poked per frame from mem[$00D9] (COLPM3, cyan).
#define INDEX_VP_P3_COL       (INDEX_SCOPE_COL + 1)        // COLOR31 (Main-Window P3 colour) (1)
// Compass band: between the title text and the viewport, re-point color01 to the compass
// COLPF0 ($00CF, dark grey) for the mode-4 compass line — the $49EE slot-0 DLI's colour.
#define INDEX_COMPASS_WAIT    (INDEX_VP_P3_COL + 1)        // WAIT(compass scanline) (1)
#define INDEX_COMPASS_COL     (INDEX_COMPASS_WAIT + 1)     // 32: color01 = compass COLPF0 (housing) (1)
#define INDEX_COMPASS_COL3    (INDEX_COMPASS_COL + 1)      // 33: color03 = compass COLPF2 (needle salmon) (1)
#define INDEX_VP_WAIT         (INDEX_COMPASS_COL3 + 1)     // 34: WAIT(kTerrainLine-1) (1)
#define INDEX_VP_BPL          (INDEX_VP_WAIT + 1)          // 32: viewport 3bp ptrs (6)
#define INDEX_VP_BPLCON0      (INDEX_VP_BPL + 6)           // 38: bplcon0 3P (1)
#define INDEX_VP_PAL          (INDEX_VP_BPLCON0 + 1)       // 39: color00..03 (4)
#define INDEX_VP_MOD0         (INDEX_VP_PAL + 4)           // 43: row-0 bpl1mod,bpl2mod (2)
#define INDEX_VP_LINEDOUBLE   (INDEX_VP_MOD0 + 2)          // 45: 93 × (WAIT+2 mod), +1 band block
// The line-doubling loop runs k=1..kViewportHeight-1.  At k==kTerrainHeight (scanline 172) we
// enter the wing-clearance band.  The terrain is rendered full-height across the whole band, so
// the band's planes 1&2 hold terrain; the grey windscreen frame is overlaid on plane3.  Where
// plane3=1 the pen is 4/5/6/7 (plane3 + whatever terrain is in planes 1&2), so the band block
// sets ALL of color04-07 = the frame grey — the frame then reads solid grey over any terrain.
// color00-03 stay the terrain palette (VP_PAL): the salmon clearance bars are plane3 holes that
// overwrite planes 1&2 to color01, so they fade salmon->brown WITH the terrain.  BPLCON2 stays at
// its init value 0x09 (sprites behind the playfield) throughout — no per-band flip needed.
#define BAND_BLOCK_WORDS      4
#define INDEX_BAND_BLOCK      (INDEX_VP_LINEDOUBLE + 3 * (kTerrainHeight - 1) + 1)  // band color04-07 (4)
// TWO WAITs at the viewport→dashboard boundary, BOTH on line 179 but at different hpos — required
// because the sprite re-points and the bitmap pointers must land in DIFFERENT time windows and a
// single sequential run of 14 moves can't hit both (measured: one WAIT early → bitmaps land before
// the band's DDFSTOP = garbage; one WAIT late → the 14 moves overrun line 180's DDFSTRT = left-edge
// glitch).  The two windows:
//  (1) INDEX_SPR_WAIT @ hpos 0xB0 — the 8 dashboard sprite re-points (AH ch0/1, scope ch3, ship ch7).
//      Safe mid-band: SPRxPT pokes don't touch bitplanes, and each reused channel's line-179 DATA was
//      already fetched at the line start, so re-pointing now only affects the next (line-180) fetch =
//      the post-VSTOP CONTROL re-fetch that arms the dashboard sprite.  That fetch MUST read the new
//      pointer (a re-point landing after it never arms — the altimeter-ship/scope disappearing bug
//      when they sat at line 181).
//  (2) INDEX_COCKPIT_WAIT @ hpos 0xE0 — RESYNCS the clock so the 6 cockpit bitmap-ptr moves land in
//      their own late window: after the band's bitplane DMA has ended (so re-pointing BPLxPT can't
//      corrupt line 179's right edge) yet before line 180's DDFSTRT (so no left-edge glitch).
#define INDEX_SPR_WAIT        (INDEX_VP_LINEDOUBLE + 3 * (kViewportHeight - 1) + BAND_BLOCK_WORDS)
#define INDEX_AH_SPR          (INDEX_SPR_WAIT + 1)          // SPR0PT/SPR1PT -> ahLeft/ahRight (4)
#define INDEX_SCOPE_SPR       (INDEX_AH_SPR + 4)            // SPR3PT -> Targeting-Scope image (2)
#define INDEX_ALTIM_SHIP_SPR  (INDEX_SCOPE_SPR + 2)         // SPR7PT -> altimeter ship / ch7 viewport half (2)
#define INDEX_COCKPIT_WAIT    (INDEX_ALTIM_SHIP_SPR + 2)    // WAIT(179, 0xE0) resync for the bitmap ptrs (1)
#define INDEX_COCKPIT_BPL     (INDEX_COCKPIT_WAIT + 1)      // cockpit 3bp ptrs, yOffset 8 (6)
#define INDEX_COCKPIT_BPLCON0 (INDEX_COCKPIT_BPL + 6)      // bplcon0 3P (1)
#define INDEX_COCKPIT_MOD     (INDEX_COCKPIT_BPLCON0 + 1)  // bpl1mod,bpl2mod (2)
#define INDEX_COCKPIT_PAL     (INDEX_COCKPIT_MOD + 2)      // color00..07 (8)
// AH colour + priority (line 180, AFTER the frame's last line 179 → safe; COLOR17 is unused on
// the band rows 172-179, which use COLOR18).  COLOR17 = $26 brown (AH pen01); BPLCON2 PFxP=0
// puts sprite pair 0 (ch0/1 = AH) BEHIND the playfield so the bitmap dial frame shows in front
// and the brown shows only through the dial's value-0 centre (mirrors the Atari PRIOR=$04).
#define INDEX_AH_COL          (INDEX_COCKPIT_PAL + 8)      // COLOR17 = $26 brown (1)
#define INDEX_AH_BPLCON2      (INDEX_AH_COL + 1)           // BPLCON2 PFxP=0 (sprites behind playfield) (1)
// Cockpit bitmap starts at kCockpitLine=180 (yOffset 8 skips the $350D band).  COLBK splits
// match the launch cockpit: baked color00=$00 covers the black divider strip (180-188); then
// dark-blue $90 dashboard instrument backgrounds (182-251); then black floor (252+).
#define INDEX_DASH_BLUE_WAIT  (INDEX_AH_BPLCON2 + 1)       // WAIT(kCockpitLine+2-1 = 181) (1)
#define INDEX_DASH_BLUE       (INDEX_DASH_BLUE_WAIT + 1)   // color00 = $90 dark blue (dashboard) (1)
#define INDEX_FLOOR_WAIT_BASE (INDEX_DASH_BLUE + 1)
// The gauge sprites (altimeter pair 6/7, energy pair 4/5) are fixed 56-row SOLID sprites whose Y
// tracks the bar value (setY), so a short/high bar overflows below the dial into the black floor.
// On the one line color00 switches to black (the floor), also switch the gauge bar colours
// (COLOR25 energy, COLOR29/30 altimeter) to black, so the overflow vanishes into the floor.
#define INDEX_FLOOR_WAIT      (INDEX_FLOOR_WAIT_BASE)      // WAIT(kCockpitLine+72-1 = 251) (1)
#define INDEX_FLOOR           (INDEX_FLOOR_WAIT + 1)       // color00 = black (floor) (1)
#define INDEX_FLOOR_ALTIM     (INDEX_FLOOR + 1)            // COLOR29 = black (altimeter terrain overflow) (1)
#define INDEX_FLOOR_SHIP      (INDEX_FLOOR_ALTIM + 1)      // COLOR30 = black (altimeter ship overflow) (1)
#define INDEX_FLOOR_ENERGY    (INDEX_FLOOR_SHIP + 1)       // COLOR25 = black (energy bar overflow) (1)
#define INDEX_TERMINATOR      (INDEX_FLOOR_ENERGY + 1)     // copperWait(255,254)
#define LIST_LENGTH           (INDEX_TERMINATOR + 1)

// ---- Sprite channel × region plan (the single source of truth; see docs/sprite-multiplex-plan.md) ----
// Each Amiga sprite channel mirrors one Atari PMG object and is re-pointed at region boundaries to
// follow that object's own vertical multiplex.  Three regions: VIEWPORT 86-171 / BAND 172-179 /
// DASHBOARD 180-267.  The ARMING RULE: a reused channel must present exactly ONE active sprite whose
// VSTOP == the boundary (180) with no earlier VSTOP, so the post-VSTOP control fetch at 180/181 reads
// the re-pointed dashboard sprite (a re-point alone does NOT re-arm a disarmed channel).
//
//   ch | viewport (86-171)      | band (172-179)     | dashboard (180-267)   | dashboard re-point
//   ---+------------------------+--------------------+-----------------------+-------------------
//    0 | left A-pillar (P0)     | left-tri inner     | AH fill left (P2)     | INDEX_AH_SPR+0
//    1 | idle                   | left-tri outer     | AH fill right (P2)    | INDEX_AH_SPR+2
//    2 | right A-pillar (P1)    | right-tri inner    | idle                  | —
//    3 | idle                   | right-tri outer    | scope-P3 dome (P3)    | INDEX_SCOPE_SPR
//    4 | laser shot (P2)        | (laser)            | idle                  | —
//    5 | free (crosshair TODO)  | —                  | energy (P1)           | (non-mux)
//    6 | free (crosshair TODO)  | —                  | altimeter terrain (P0)| (non-mux)
//    7 | viewport-P3 (P3, tall) | (P3 tall)          | altimeter ship (M3)   | INDEX_ALTIM_SHIP_SPR
// ALL re-points sit in the early-line-179 group (INDEX_SPR_WAIT @ 0xB0, before the bitmap ptrs'
// own resync WAIT) — a re-point at line 181 fires after the channel's post-VSTOP re-fetch and never arms.
//
// The dashboard re-points (channels reused viewport→dashboard) are table-driven — setDashboardSprite()
// looks the channel up here, so adding an element (e.g. the crosshair on ch5/6) is a one-row change.
struct DashRepoint { uint8_t ch; uint16_t copperIndex; };
static const DashRepoint kDashRepoints[] = {
    { 0, INDEX_AH_SPR + 0 },       // AH ground-fill left
    { 1, INDEX_AH_SPR + 2 },       // AH ground-fill right
    { 3, INDEX_SCOPE_SPR },        // Targeting-Scope P3 dome
    { 7, INDEX_ALTIM_SHIP_SPR },   // altimeter ship (ch7 viewport half = the Main-Window P3 object)
};
static const int kNumDashRepoints = (int)(sizeof(kDashRepoints) / sizeof(kDashRepoints[0]));

FlightCopperList::FlightCopperList()
    : CopperList((uint32_t*)AllocMem(LIST_LENGTH << 2, MEMF_CHIP | MEMF_CLEAR), LIST_LENGTH, true)
{
}

void FlightCopperList::buildLayout(const Bitmap& title, const Bitmap& terrain, const Bitmap& cockpit,
                                   const Sprite& leftPost, const Sprite& leftTri,
                                   const Sprite& rightPost, const Sprite& rightTri,
                                   const Sprite& nullSprite,
                                   const Sprite& ahLeft, const Sprite& ahRight,
                                   const Sprite& scopeP3)
{
    uint32_t* d = data_;

    // ---- title region: playfield (2bp interleaved) ----
    setPlayfield(INDEX_PLAYFIELD, kW, kH, kBP2, /*interleaved*/true,
                 /*hires*/false, /*interlace*/false, /*dualPlayfield*/false,
                 /*holdAndModify*/false, kCenterY);
    // PFxP=4: ALL sprites in front of the playfield in the viewport — the windscreen frame
    // (pairs 0/1 = ch0-3) AND the player laser shot (pair 2 = ch4) sit over the terrain.  (The
    // gauges on ch5/6/7 live in the dashboard, where INDEX_AH_BPLCON2 flips PFxP=0 = all sprites
    // BEHIND the playfield, so the dial-frame bitmaps show in front of the bars.)  Was PFxP=2,
    // which left pair 2 (the laser) behind the terrain.
    d[INDEX_BPLCON2] = copperMove(bplcon2, (uint16_t)((4u << 3) | 4u));
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
    d[INDEX_SHOT_COL] = copperMove(kColor27, 0);         // player laser shot colour (poked per frame)
    d[INDEX_SCOPE_COL] = copperMove(kColor23, 0);        // Targeting-Scope P3 colour (poked per frame from $00D9)
    d[INDEX_VP_P3_COL] = copperMove(kColor31, 0);        // Main-Window P3 object colour (poked per frame from $00D9)
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
            // Crossing into the wing-clearance band (scanline kBandLine = 172): the grey windscreen
            // frame is on plane3, over terrain in planes 1&2 -> set ALL of color04-07 = frame grey
            // (seeded 0, poked by setBandPalette) so the frame reads solid grey over any terrain.
            // color00-03 stay the terrain pens (VP_PAL) — the salmon bars fade with the terrain.
            // plane3 is 0 across the terrain rows above, so color04-07 are only selected in the band.
            d[idx++] = copperMove(color04, 0);
            d[idx++] = copperMove(color05, 0);
            d[idx++] = copperMove(color06, 0);
            d[idx++] = copperMove(color07, 0);
        }
        const uint16_t v = (k & 1) ? (uint16_t)80 : (uint16_t)-40;
        d[idx++] = copperMove(bpl1mod, v);
        d[idx++] = copperMove(bpl2mod, v);
    }

    // ---- cockpit region: two WAITs on line 179 (see the INDEX_SPR_WAIT/INDEX_COCKPIT_WAIT comment) ----
    // (1) early WAIT (hpos 0xB0): the 8 dashboard sprite re-points, first.
    d[INDEX_SPR_WAIT] = copperWait(kCockpitLine - 1, 0xB0);
    setDashboardSprite(0, ahLeft);              // SPR0PT -> ahLeft  (left 16px of the 32px dial)
    setDashboardSprite(1, ahRight);             // SPR1PT -> ahRight (right 16px)
    setDashboardSprite(3, scopeP3);             // SPR3PT -> Targeting-Scope P3 dome (ch3)
    setDashboardSprite(7, nullSprite);          // SPR7PT -> altimeter ship (real ptr set on force)
    // (2) resync WAIT (hpos 0xE0): the cockpit bitmap ptrs land in their own late window.
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

    // AH ground-fill colour + priority (line 180, after the frame): COLOR17 = $26 red-brown
    // (the AH sprites use pen 01), and BPLCON2 PFxP=0 so sprite pair 0 (ch0/1 = AH) sits BEHIND
    // the playfield — the bitmap dial frame stays in front, brown shows through the value-0 centre.
    d[INDEX_AH_COL]     = copperMove(color17, atariToOCS(0x26));
    d[INDEX_AH_BPLCON2] = copperMove(bplcon2, (uint16_t)((0u << 3) | 0u));

    // Dashboard instrument backgrounds = dark blue COLBK $90 (Amiga 182-251); floor black (252+).
    // Only COLBK (color00) changes; baked color00=$00 above covers the divider strip (180-188).
    d[INDEX_DASH_BLUE_WAIT] = copperWait(kCockpitLine + 2 - 1, 0xE0);
    d[INDEX_DASH_BLUE]      = copperMove(color00, atariToOCS(0x90));
    // (ch3 scope + ch7 altimeter-ship SPR re-points now live in the line-180 boundary group above,
    // with the AH re-points — deferring them to line 181 here disarmed the channels.  See INDEX_AH_SPR.)
    d[INDEX_FLOOR_WAIT]  = copperWait(kCockpitLine + 72 - 1, 0xE0);
    d[INDEX_FLOOR]        = copperMove(color00, atariToOCS(0x00));  // floor background → black
    d[INDEX_FLOOR_ALTIM]  = copperMove(kColor29, 0x000);   // altimeter terrain pen01 → black (hide overflow)
    d[INDEX_FLOOR_SHIP]   = copperMove(kColor30, 0x000);   // altimeter ship   pen10 → black (hide overflow)
    d[INDEX_FLOOR_ENERGY] = copperMove(kColor25, 0x000);   // energy bar       pen01 → black (hide overflow)

    d[INDEX_TERMINATOR] = copperWait(255, 254);
}

// Wing-clearance band (scanlines 172-179): the grey windscreen frame ($00D4=$06) is drawn on
// plane3, over terrain in planes 1&2, so a frame pixel selects one of color04-07 — all four are
// set to the frame grey here so the frame reads solid over any terrain.  The bars/marker are
// plane3 holes taking color00-03 from the terrain palette (VP_PAL), which fade salmon→brown WITH
// the terrain.
void FlightCopperList::setBandPalette(uint16_t grey)
{
    data_[INDEX_BAND_BLOCK + 0] = copperMove(color04, grey);
    data_[INDEX_BAND_BLOCK + 1] = copperMove(color05, grey);
    data_[INDEX_BAND_BLOCK + 2] = copperMove(color06, grey);
    data_[INDEX_BAND_BLOCK + 3] = copperMove(color07, grey);
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

void FlightCopperList::setShotColor(uint16_t c)
{
    data_[INDEX_SHOT_COL] = copperMove(kColor27, c);    // COLOR27 = sprite pair 4/5 pen 11 (laser shot)
}

void FlightCopperList::setScopeP3Color(uint16_t c)
{
    data_[INDEX_SCOPE_COL] = copperMove(kColor23, c);   // COLOR23 = sprite pair 2/3 pen 11 (Targeting-Scope P3 object, cyan)
}

void FlightCopperList::setViewportP3Color(uint16_t c)
{
    data_[INDEX_VP_P3_COL] = copperMove(kColor31, c);   // COLOR31 = sprite pair 6/7 pen 11 (Main-Window P3 object, cyan)
}

// Re-point one dashboard-reused channel's SPRxPT at the region boundary (table-driven — the copper
// index per channel lives in kDashRepoints, mirroring docs/sprite-multiplex-plan.md §3).  Callers
// name the CHANNEL, not a copper offset, so the multiplex map is edited in one place.
void FlightCopperList::setDashboardSprite(int ch, const Sprite& s)
{
    for (int i = 0; i < kNumDashRepoints; i++)
        if (kDashRepoints[i].ch == ch) { showSprite(kDashRepoints[i].copperIndex, (uint16_t)ch, s); return; }
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

// Re-point the viewport (terrain) bitplane pointers to a different bitmap — the flight
// terrain double-buffer flip.  renderFlightDirect paints the off-screen buffer, then calls
// this to swap it in; the copper picks up the new pointers at the next vblank restart, so the
// live displayed buffer is never repainted mid-frame (was the plane1 flicker).  Same 3bp
// interleaved layout as buildLayout's showBitmap(INDEX_VP_BPL, terrain) — only the 6 ptrs change.
void FlightCopperList::setTerrainBitplanes(const Bitmap& terrain)
{
    showBitmap(INDEX_VP_BPL, terrain);
}
