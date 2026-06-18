// Standby screen (scene 3) — 2 bitplanes, Copper colour splits at region boundaries.
//
// Terrain colours are read dynamically each frame from mem[]:
//   - Atari GTIA mode-10 nibble 8 (dominant closed-door fill) → colHW[8] = COLBK
//   - DLI dli_sub_6cf1 sets COLBK = mem[$0071] during terrain rows.
//   → Amiga terrain col3 = atariToOCS(mem[$0071]).
//
// Copper list structure:
//   [preamble]  setPlayfield (2bp, 320x216)
//               showBitmap(titleBitmap)  + title palette (4 colours)
//               sprite colours (COLOR16/17) + sprite pointers (8 sprites)
//   WAIT(kTerrainLine-1, 0xE0)   ← end of previous line, in overscan
//               bpl1/2 ptr → terrainBitmap, terrain palette (dynamic from mem[])
//   WAIT(kCockpitLine-1, 0xE0)
//               bpl1/2 ptr → cockpitBitmap (modeD $350D + mode4 $332D from mem[])
//
// Colour-register writes are IMMEDIATE on OCS.  The correct split technique is
// a single WAIT at the end of the line before the boundary (in the H-blank /
// overscan area).  Bitplane pointers go first (timing-critical); colours follow.

#define ECS_SPECIFIC
#include <hardware/dmabits.h>
#include <hardware/custom.h>
#include <graphics/display.h>
#include "framework/AmigaHardware.h"
#include "framework/CopperList.h"
#include "framework/Bitmap.h"
#include "framework/Palette.h"
#include "framework/Sprite.h"
#include "RescueOnFractalus.h"
#include "PlatformAmiga.h"
#include "../../gen/AtariZp.h"      // zp:: named Atari memory offsets
#include "FlightProf.h"   // per-frame VBI-count profiler (g_flightProf / flight_vbi_tick)

// Native handler functions — see NativeHandlers.cpp and SfxPlayer.cpp.
extern "C" void vbi_attract_timer_native(void);                  // $52D7: timer cascade
extern "C" void update_indicator_blink_native(void);           // $4131: cockpit blink
extern "C" void update_cockpit_digits_native(void);                      // $3FFA: cockpit digit update
extern "C" void lock_on_indicator_tick_native(void);               // $4229: cockpit counter animation
extern "C" void sound_event_dispatch_native(void);              // $5367: ring ($0088) vs door scroll ($008A)
extern "C" volatile uint8_t g_tunnelFieldDirty;                // set when advance_message_column draws into $2000
extern "C" volatile uint8_t g_tunRowLo, g_tunRowHi;            // row extent of the expanding black clear
extern "C" volatile uint8_t g_activeVbi;                       // 0=none 1=standby($52D7) 2=flight($4FF5); read by game_vbi_isr

// The genuine transpiled launch cinematic ($5F1D, src/gen/rof_gen.c): display_setup()'s
// frame-wait spin loops call platform_render_frame, which renders then waits for a real VBI.
extern "C" void display_setup(void);

// Black-until-ready reveal gate, latched on at display_setup entry (rof_native.c); renderFrame
// holds the EmptyCopperList on screen until it sets, then switches to the real lists.
extern "C" volatile unsigned char g_standbyRevealReady;
// Door-field-ready gate, latched on in display_setup once the doors/dots/LEVEL field has been
// drawn into $2000 but BEFORE delay_loop_c2_to_c9 ramps the green colour $0071 (rof_native.c).
// render() decodes $2000 -> terrainBitmap once when this rises, so the door pixels exist before
// the fade and the per-frame color03 ramp shows the dark->bright green build on them.
extern "C" volatile unsigned char g_doorFieldReady;
// Screen-RAM dirty flags: render() scans the title ($32B7) + cockpit ($332D mode4 / $350D
// modeD) regions only when these are set, instead of re-scanning all ~580 cells every
// frame.  During the static doors/standby phases nothing changes, so the scan was pure
// overhead (~7 ms/frame, the dominant door-cinematic cost).  g_titleDirty is set by the
// genuine $782A title writer (copy_altitude_graphic_to_screen) via the platform_title_
// changed() hook; g_cockpitDirty by update_cockpit_digits / lock_on_indicator_tick at their
// store sites.  Both are force-set at phase transitions in deriveRenderSignals() so the
// initial build (by the transpiled display_setup, not those writers) + flight updates are
// never missed.
extern "C" volatile unsigned char g_titleDirty   = 1;
extern "C" volatile unsigned char g_cockpitDirty = 1;
// The genuine boot chain (src/gen/rof_gen.c): station_init = attract ($195D, returns on
// START); game_entry = $3CDE -> game_main_loop (game-display setup -> display_setup
// cinematic -> flight loop, never returns).  g_quitJmp = the __builtin_setjmp buffer
// (defined in PlatformAmiga.cpp) the pump longjmps to on quit, unwinding the transpiled chain.
extern "C" void station_init(void);
extern "C" void game_entry(void);
extern "C" void* g_quitJmp[];   // definition (sized) lives in PlatformAmiga.cpp

extern "C" volatile uint8_t mem[65536];

// Lookup table: byte → 16-bit doubled glyph pattern (each bit → 2 pixels).
// Filled once in initialize(); used by title render for mode-6 1bpp doubling.
static uint16_t kDoubleGlyph[256];

// Precomputed decode tables (filled in initialize()) — convert one source byte
// straight to its output bitplane bytes, replacing the per-byte bit loops.
//   mode-D (2bpp, stars/planet viewport): byte = 4 pixels (2 bits) → 8 Amiga px.
//     kModeDP1[s] = plane1 (colour bit0 of each pixel), kModeDP2[s] = plane2 (bit1).
static uint8_t kModeDP1[256];
static uint8_t kModeDP2[256];
//   GTIA mode-10 (tunnel field at $2000): byte = 2 nibbles; nibble bit k → 4px.
static uint8_t kGtia10P1[256];   // nibble bit0
static uint8_t kGtia10P2[256];   // nibble bit1
static uint8_t kGtia10P3[256];   // nibble bit2
//   Standby door field at $2000: like GTIA-10 but each nibble is first mapped through
//   kNibbleColour (collapsing the 16 GTIA registers to pens 0/1/3) — so this is a
//   distinct table.  kDoorP1[s]=plane1 byte, kDoorP2[s]=plane2 byte (plane3 always 0).
static uint8_t kDoorP1[256];
static uint8_t kDoorP2[256];
// GTIA mode-10 nibble → Amiga pen for the Standby door field.  In this scene only three
// nibble values occur: 0 (road dots → COLPM0 → pen0), 7 ("LEVEL 04" text → COLPF3 → pen1)
// and 8 (background → COLBK → pen3).  Nibble 7 MUST differ from 8 or the level text (baked
// into the bitmap as COLPF3 pixels) vanishes into the green background.  Used only to build
// kDoorP1/kDoorP2 in initialize().
static const uint8_t kNibbleColour[16] = {
    0,                   // 0   → COLPM0 → pen0 (road dots / black)
    3, 3, 3,             // 1-3 → bg
    3, 3, 3,             // 4-6 → bg
    1,                   // 7   → COLPF3 → pen1 ("LEVEL 04" text)
    3,                   // 8   → COLBK  → pen3 (green background)
    3, 3, 3, 3, 3, 3, 3  // 9-15 → bg
};

#include "assets/terrain_pal.h"
#include "assets/atari_pal.h"
// cockpit_pal.h and cockpit_raw removed: cockpit palette is now fully dynamic
// from mem[] via atariToOCS(), cockpit bitmap decoded each frame in render().

static const uint16_t kW   = 320;
static const uint16_t kH   = 216;   // Atari attract = 216 visible scanlines
static const uint16_t kHT  = 86;    // terrain sprite/bitmap height (placeholder; M6a audit may revise)
static const uint8_t  kBP2 = 2;
static const uint8_t  kBP3 = 3;   // cockpit only — 3rd plane carries bit-7 chars (red)

// 160 sufficed for the single-band terrain; the flight/stars viewport now does
// per-scanline bitplane-modulo line-doubling (~2 MOVEs + 1 WAIT per displayed
// scanline over the 86-line region), so the list needs ~3x the headroom.
static const uint32_t kCopperLen = 420;

// Display geometry: anchored at the standard PAL display-window top (0x2c).
// All boundary lines are derived so changing the top can't desync them.
static const uint16_t kDisplayTop    = 0x2c;               // DIWSTRT.y (PAL standard)
static const uint16_t kTitleHeight   = 42;                 // title region display lines
static const uint16_t kTerrainHeight = kHT;                // terrain region (placeholder, see kHT)
static const uint16_t kTerrainLine   = kDisplayTop + kTitleHeight;   // = 0x56 (86)
static const uint16_t kCockpitLine   = kTerrainLine + kTerrainHeight; // = 172
// Cockpit height: 4 modeD DL entries × 2 scan lines + 10 mode4 DL entries × 8 scans
// (title 42 + terrain 86 + cockpit 88 = 216 = kH).
static const uint16_t kCockpitH     = 4 * 2 + 10 * 8;               // = 88
// centerY so that DIWSTRT.y = kDisplayTop: centerY = kDisplayTop + kH/2 = 0x2c + 108 = 0x98
static const uint16_t kCenterY       = kDisplayTop + kH / 2;

// BPLCON0: 2 bitplanes, lores.
static const uint16_t kBPLCON0_2P = (uint16_t)((2 << PLNCNTSHFT) | USE_BPLCON3);
// Terrain + cockpit run at 3 bitplanes: the cockpit needs the 3rd plane for its
// bit-7 red chars, and the terrain/doors run 3bp too (3rd plane zeroed) so the
// tunnel reveal slots into the door gap with no mid-screen plane-count switch.
static const uint16_t kBPLCON0_3P = (uint16_t)((3 << PLNCNTSHFT) | USE_BPLCON3);

// Sprite horizontal positions (see fillSpriteData for slant details).
static const uint16_t kSprXLeft  = 0x81 + 17;
static const uint16_t kSprXRight = 0x81 + 285;

// BLACK-UNTIL-READY: on the real Atari the boot→Standby build is near-instant, so the
// screen just appears.  On the Amiga the one-time setup (load_xex_image, scene.initialize,
// the game_entry mega-init) spans a couple of seconds during which the screen would
// otherwise show a piecemeal, janky build.  We hold an EmptyCopperList (black, no
// playfield/sprites) on screen until g_standbyRevealReady latches, then switch to the real
// lists — see renderFrame.  (Previously this was a palette fade-to-black; the fade is gone.)
//
// g_standbyRevealReady LATCHES on at display_setup entry (rof_native.c) — by then
// game_main_loop has drawn the cockpit/top bar and scene.initialize has set up the sprites.
// It latches (never clears) on purpose: the launch sequence re-runs display_setup and
// transiently clears the music gate $00E7, so gating on $00E7 would black the screen out
// again when START is pressed — the latch keeps it revealed through the cinematic and flight.

// ---- sprite data (staircase slant, see commit history for derivation) --------
void RescueOnFractalus::fillSpriteData(Sprite* s, bool isRight)
{
    uint16_t* d = s->data() + 2;
    for (int i = 0; i < kHT; i++) {
        int shift;
        if (i < 8) {
            shift = isRight ? 12 : 0;
        } else {
            int step = (i - 8) / 14 + 1;
            shift = isRight ? (12 - 2 * step) : (2 * step);
            if (shift < 0) shift = 0;
            if (shift > 12) shift = 12;
        }
        uint16_t sprA = (uint16_t)(0xF000u >> shift);
        d[i * 2]     = sprA;
        d[i * 2 + 1] = 0x0000;
    }
}

// ---- throttle gauge sprite ---------------------------------------------------
// Build the player-1 throttle bar from the vobj strip mem[$0D98..].  Each strip
// byte is one Atari player scanline ($F0 = leftmost 4px on); we map a filled row
// to the leftmost 4 px (colour 01) of an Amiga sprite line.
void RescueOnFractalus::buildGaugeSprite()
{
    // 57-row strip $0D98..$0DD0 (the original vobj player extent).  Each Atari
    // player bit is one colour clock = 2 Amiga lores px, so the 4-bit $F0 segment
    // is 8 px wide -> 0xFF00 (matches SIZEP1=0, normal width).
    uint16_t* d = gaugeSprite->data() + 2;   // skip the 2 control words
    for (int i = 0; i < 57; i++) {
        uint16_t on = (mem[0x0D98 + i] & 0xF0u) ? 0xFF00u : 0x0000u;
        d[i * 2]     = on;     // plane A (colour bit 0)
        d[i * 2 + 1] = 0x0000; // plane B
    }
}

// ---- starfield sprites -------------------------------------------------------
// During the stars phase display_setup positions players P0/P2/P3 as a sparse
// scrolling starfield (random_terrain_height $6B47: POKEY RANDOM, 1/32 chance of a dot
// from table $6B5F = [$80,$20,$04,$01]; scroll_terrain_columns $6AEE shifts each
// player up one scanline/frame and appends a new bottom byte).  The genuine
// transpiled scroll_terrain_columns already maintains those player buffers in
// mem[], so we just map the 89-byte visible strip ($..32..$..8A, player scanlines
// $32..$8A) of each into an Amiga sprite.  Each Atari player bit → 2 Amiga lores
// px (matches the gauge mapping); a single 1-bit star dot lands at one of 4 sub-x.
static const uint16_t kStarSrc[3]  = { 0x0C32, 0x0E32, 0x0F32 };  // P0, P2, P3
// First-guess Amiga X: the mode-D viewport's visible window is the central 40 of
// 48 bytes → Atari colour clock $32 maps to the left display edge (0x81), scale 2
// (320 px / 160 cc).  HPOSP0=$38, HPOSP2=$8E, HPOSP3=$B8 (display_setup $64F3-$6503).
static const uint16_t kStarX[3]    = { 0x81 + (0x38 - 0x32) * 2,    // P0 = 141
                                       0x81 + (0x8E - 0x32) * 2,    // P2 = 313
                                       0x81 + (0xB8 - 0x32) * 2 };  // P3 = 397
static const int       kStarRows   = 89;   // visible strip $..32..$..8A ($59 bytes)

void RescueOnFractalus::buildStarSprites()
{
    for (int c = 0; c < 3; c++) {
        uint16_t* d = starSprite[c]->data() + 2;   // skip the 2 control words
        const uint8_t* src = (const uint8_t*)&mem[kStarSrc[c]];
        const uint8_t* end = src + kStarRows;
        // Double each set player bit to 2 Amiga px: player bit b (b7 = leftmost) → sprite
        // word bits (2b+1, 2b).  All 4 star sub-positions ($80/$20/$04/$01 = bits 7/5/2/0)
        // land inside the 16 px sprite this way.  kDoubleGlyph is the precomputed byte→
        // doubled-word table (built in initialize()), so the per-row 8-iteration bit loop
        // becomes one lookup — that loop was ~22 ms of the stars/planet frame on the A500.
        // Post-increment pointers (the 68000 (An)+ mode), no per-row index multiplies.
        while (src < end) {
            *d++ = kDoubleGlyph[*src++];   // plane A (colour bit 0 = pen 01)
            *d++ = 0x0000;                 // plane B
        }
    }
}

// ---- copper list builder -----------------------------------------------------
void RescueOnFractalus::buildCopperList(CopperList* cl, uint16_t frame)
{
    (void)frame;
    uint32_t* d   = cl->data();

    // d[0] is already copperWait(16,0) from the CopperList constructor; start at 1.
    uint32_t  idx = 1;

    // ---- title region -------------------------------------------------------
    // Emits BPLCON0 (2bp) + BPL1MOD/BPL2MOD for the title band; the constant playfield
    // registers — including BPLCON2 = the GPRIOR=$14 priority (PF in front of sprite pair
    // 1+ / the gauge, behind pair 0 / the canopy posts) — are set once in initialize().
    idx = cl->setPlayfield(idx, kW, kH, kBP2, /*interleaved*/true,
                           /*hires*/false, /*interlace*/false,
                           /*dualPlayfield*/false, /*holdAndModify*/false,
                           kCenterY);
    // Title palette.
    // vbi_handler_standby ($52D7) sets every frame:
    //   COLPF0 ($D016) = mem[$00D8]  — title text colour (mode-6 col=1 chars)
    //   COLBK  ($D01A) = mem[$02C8]  — title background
    //   COLPF1 ($D017) = $78         — hardcoded blue (same role on real hw)
    // copy_altitude_graphic_to_screen ($782A) sets mem[$00D8]=$44 for the
    // copyright block so the text colour changes per alternation.
    // Mode-6 selects the per-char text colour from the byte's top 2 bits:
    //   hi2=0 → COLPF0 = mem[$00D8]   (copyright block, e.g. $44)
    //   hi2=1 → COLPF1 = $78 (blue)   (RESCUE ON FRACTALUS!, hardcoded by vbi $52F7)
    // render() routes hi2=0 chars to col1 (plane1) and hi2=1 chars to col2
    // (plane2); "off" pixels use col0 = COLBK.
    {
        uint16_t tbg   = atariToOCS(mem[0x02C8]);  // COLBK = background (grey)
        uint16_t tpf0  = atariToOCS(mem[zp::textColorPf0]);  // COLPF0 = hi2=0 text
        uint16_t tpf1  = atariToOCS(0x78);         // COLPF1 = hi2=1 text (blue) — score digits
        d[idx++] = copperMove(color00, tbg);
        d[idx++] = copperMove(color01, tpf0);
        d[idx++] = copperMove(color02, tpf1);
        d[idx++] = copperMove(color03, tbg);
    }
    cl->showBitmap(idx, *titleBitmap);
    idx += 2 * kBP2;

    // Sprite colour registers.  The canopy posts must match the top/bottom
    // background grey (COLBK = mem[$02C8] = atariToOCS($06) = 0x555), so read
    // the same source rather than a hardcoded constant.
    d[idx++] = copperMove(color16, 0x000);
    d[idx++] = copperMove(color17, atariToOCS(mem[0x02C8]));

    // Sprite pointers:
    cl->showSprite(idx, 0, *leftPost);  idx += 2;
    cl->showSprite(idx, 1, *rightPost); idx += 2;
    // Sprite 2 = throttle gauge bar (vobj player strip), only once the launch
    // begins (its data is built from $0D98 from then on); 3..7 stay null.
    cl->showSprite(idx, 2, rsGauge ? *gaugeSprite : *nullSprite); idx += 2;
    // Sprite 3 stays null (it shares the gauge's colour registers).  Sprites 4/5/6
    // carry the starfield (players P0/P2/P3) during the stars + planet phases.
    const bool stars = rsStars;
    // F0 flight perf probe: in flight, sprite 4 (idle starfield sprite) is reused
    // as a vertical bar whose length ∝ the flight render pass's scanline cost.
    const bool flightProbe = rsFlight;
    cl->showSprite(idx, 3, *nullSprite); idx += 2;
    cl->showSprite(idx, 4, (stars || flightProbe) ? *starSprite[0] : *nullSprite); idx += 2;
    cl->showSprite(idx, 5, stars ? *starSprite[1] : *nullSprite); idx += 2;
    cl->showSprite(idx, 6, stars ? *starSprite[2] : *nullSprite); idx += 2;
    cl->showSprite(idx, 7, *nullSprite); idx += 2;
    // Gauge bar colour (sprite pair 2/3, colour 01 = COLOR21 = $1AA).  The
    // original ramps player 1's colour through the $4DEA table as it fills,
    // ending at $D6 = #560; launch_gauge_step_native tracks that in $00DE.
    d[idx++] = copperMove(0x1AA, atariToOCS(mem[0x00DE]));
    // Star colour: COLPM (mem[$02C0]) faded in 0→$0C grey (display_setup $6555).
    // Sprites 4/5 share COLOR25 ($1B2); sprites 6/7 share COLOR29 ($1BA).
    if (flightProbe) {
        d[idx++] = copperMove(0x1B2, 0xF0F);     // COLOR25 = magenta probe bar (sprite 4)
    } else if (stars) {
        const uint16_t starCol = atariToOCS(mem[0x02C0]);
        d[idx++] = copperMove(0x1B2, starCol);   // COLOR25 (sprite pair 4/5 pen 01)
        d[idx++] = copperMove(0x1BA, starCol);   // COLOR29 (sprite pair 6/7 pen 01)
    }

    // ---- terrain region -------------------------------------------------------
    // Wait until the end of the previous line (in the right border / H-blank).
    // Colour writes are immediate on OCS, so everything written during H-blank is
    // live for the first pixel of kTerrainLine.  Bitplane pointers go first
    // (timing-critical); colours follow.
    // In Standby this is one full-height terrain band.  When the doors open it
    // splits into a sliding top half, the tunnel reveal, and a sliding bottom half.
    // Bitplane pointers go FIRST after every WAIT (timing-critical: they must land
    // in H-blank before display fetch); the 2bp->3bp switch + colours follow.  Zero-
    // height bands are skipped so we never emit two WAITs for the same scanline —
    // a second WAIT on a line already reached would run its pointer writes late,
    // into the visible area (the fully-open frame: top/bottom collapse to 0 rows).
    const uint32_t ta   = (uint32_t)terrainBitmap->data;
    const uint16_t half = kTerrainHeight / 2;                 // = 43
    // Door-open progress = steps the native scroll_terrain_dl has taken, read from
    // the $008A counter it decrements ($2B closed -> 0 fully open).  Half-gap g2 in
    // rows grows 0 -> half as the doors part; the split "arises from $008A".
    const uint16_t g2   = rsLaunched ? (uint16_t)(0x2Bu - mem[zp::terrainScrollCounter]) : 0;
    const bool     door = (g2 > 0);
    const uint16_t topH = (uint16_t)(half - g2);               // == bottom-band rows
    const bool tunnelFirst = door && (topH == 0);              // fully open: tunnel fills region
    const uint32_t tun  = (uint32_t)tunnelBitmap->data + (uint32_t)(half - g2) * 120u;

    auto emitBpl = [&](uint32_t base) {                        // 3bp interleaved = 120 B/row
        d[idx++] = copperMove(bpl1pth, (uint16_t)(base >> 16));
        d[idx++] = copperMove(bpl1ptl, (uint16_t)(base & 0xFFFF));
        d[idx++] = copperMove(bpl2pth, (uint16_t)((base + 40) >> 16));
        d[idx++] = copperMove(bpl2ptl, (uint16_t)((base + 40) & 0xFFFF));
        d[idx++] = copperMove(bpl3pth, (uint16_t)((base + 80) >> 16));
        d[idx++] = copperMove(bpl3ptl, (uint16_t)((base + 80) & 0xFFFF));
    };
    // Terrain palette (GTIA mode-10 nibbles 0/7/8): col0 black (also the tunnel's
    // pen 0), col1 LEVEL-04 text, col3 green background.  Ring feeds pens 1-6.
    const uint16_t terr0 = atariToOCS(mem[0x02C0]);
    const uint16_t terr1 = atariToOCS(mem[0x02C7]);
    const uint16_t terr2 = atariToOCS(mem[zp::colorRing]);
    const uint16_t terr3 = atariToOCS(mem[zp::displayFlags]);
    // Tunnel ring colours. The GTIA mode-10 pixel value -> ring-slot mapping is ROTATED +3,
    // per the Atari tunnel DLI ($6CD7/$6CF1): pixels 1-3 = COLPM1-3 <- $08D7/$08D8/$08D9
    // (ring[3..5]); pixels 4-6 = COLPF0-2 <- $08D4/$08D5/$08D6 (ring[0..2]).  The Amiga
    // bitmap pen == GTIA pixel value, so pen k <- colorRing[(k+2)%6].  This offset is
    // invisible during the rotating-ramp cycle, but it decides WHICH ring the clear's
    // $08D8=0 blackens — getting it wrong drops a black ring between still-lit rings.
    auto emitRing = [&]() {                                    // tunnel pens 1-3 (pixels 1-3)
        d[idx++] = copperMove(color01, atariToOCS(mem[zp::colorRing + 3]));
        d[idx++] = copperMove(color02, atariToOCS(mem[zp::colorRing + 4]));
        d[idx++] = copperMove(color03, atariToOCS(mem[zp::colorRing + 5]));
    };
    auto emitTerrCols = [&]() {                                // terrain pens 1-3
        d[idx++] = copperMove(color01, terr1);
        d[idx++] = copperMove(color02, terr2);
        d[idx++] = copperMove(color03, terr3);
    };

    // ---- region WAIT: pointers first, then the 2bp->3bp switch, then colours ----
    d[idx++] = copperWait(kTerrainLine - 1, 0xE0);
    if (rsViewport) {
        // Stars / planet: one full-height mode-D viewport band from terrainBitmap,
        // with the $6CC2 DLI viewport palette.  Upper zone (slot1 $6D0E): COLBK =
        // mem[$00DC] (black space), COLPF0=$24, COLPF1=$28, COLPF2=$2A — the star /
        // planet tones.  (The slot4 $6D67 lower-greys split near the horizon is a
        // follow-up; one palette renders the whole viewport for now.)
        emitBpl((uint32_t)terrainBitmap->data);
        d[idx++] = copperMove(bplcon0, kBPLCON0_3P);
        if (rsFlight) {
            // Flight terrain palette (the F-skip bypasses the planet-entry colour
            // setup + the dynamic atmosphere pipeline, so render a fixed authentic
            // Fractalus palette here).  Empirically the terrain bitmap encodes
            // pen0 = terrain body, pen1 = sky, pen2 = dots (NOT the conventional
            // 0=background).  Nearest authentic Atari palette entries (the real
            // hardware can only produce these 256 colours):
            d[idx++] = copperMove(color00, atariToOCS(0x14));  // pen0 = terrain body (brown #530)
            d[idx++] = copperMove(color01, atariToOCS(0x2A));  // pen1 = sky (salmon #c76)
            d[idx++] = copperMove(color02, atariToOCS(0x20));  // pen2 = dots (dark #300)
            d[idx++] = copperMove(color03, atariToOCS(0x18));  // pen3 = terrain highlight (#962)
        } else {
            d[idx++] = copperMove(color00, atariToOCS(mem[0x00DC]));  // COLBK
            d[idx++] = copperMove(color01, atariToOCS(0x24));         // COLPF0
            d[idx++] = copperMove(color02, atariToOCS(0x28));         // COLPF1
            d[idx++] = copperMove(color03, atariToOCS(0x2A));         // COLPF2
        }
        // Vertical line-doubling in the copper: the bitmap holds 43 mode-D rows
        // (single height — renderViewportModeD writes each row ONCE).  Each 120-byte
        // interleaved row is re-displayed on 2 scanlines by toggling the bitplane
        // modulo at end-of-line: -40 rewinds plane1's fetch back to the same row (so
        // the next scanline repeats it), +80 advances to the next row.  The modulo
        // set during scanline Sk's h-blank applies at Sk's end; V(Sk) = -40 (k even)
        // / +80 (k odd).  S0's -40 is set here; S1..S(kTerrainHeight-1) below.
        d[idx++] = copperMove(bpl1mod, (uint16_t)-40);
        d[idx++] = copperMove(bpl2mod, (uint16_t)-40);
        for (uint16_t k = 1; k < kTerrainHeight; k++) {
            d[idx++] = copperWait((uint16_t)(kTerrainLine + k - 1), 0xE0);  // S(k) h-blank
            const uint16_t v = (k & 1) ? (uint16_t)80 : (uint16_t)-40;       // odd: advance, even: rewind
            d[idx++] = copperMove(bpl1mod, v);
            d[idx++] = copperMove(bpl2mod, v);
        }
    } else {
    emitBpl(tunnelFirst ? tun : (ta + (uint32_t)g2 * 120u));   // top half slides up (g2 rows)
    d[idx++] = copperMove(bplcon0, kBPLCON0_3P);
    d[idx++] = copperMove(bpl1mod, 80);                        // BPL1MOD = odd planes 1&3
    d[idx++] = copperMove(bpl2mod, 80);                        // BPL2MOD = plane 2
    d[idx++] = copperMove(color00, terr0);                     // pen 0 = black (terrain & tunnel)
    if (tunnelFirst) emitRing(); else emitTerrCols();
    if (door) {                                                // ring pens 4-6 (pixels 4-6)
        d[idx++] = copperMove(color04, atariToOCS(mem[zp::colorRing + 0]));  // pen4 = COLPF0 <- $08D4
        d[idx++] = copperMove(color05, atariToOCS(mem[zp::colorRing + 1]));  // pen5 = COLPF1 <- $08D5
        d[idx++] = copperMove(color06, atariToOCS(mem[zp::colorRing + 2]));  // pen6 = COLPF2 <- $08D6
    }

    // ---- mid-screen bands (only when partially open; each on its own WAIT) ----
    if (door && topH > 0) {
        // Tunnel reveal, centred on its vanishing point (row 43): the gap shows the
        // deepest part first and widens outward.  The tunnel band has no end WAIT,
        // so if the bottom band is dropped below it simply extends to the cockpit.
        d[idx++] = copperWait((uint16_t)(kTerrainLine + topH - 1), 0xE0);
        emitBpl(tun);
        emitRing();
        // Bottom door half (slides down): terrain from row kTerrainHeight/2.  Emit
        // it only while its WAIT clears the cockpit WAIT by a margin — otherwise two
        // WAITs land on the same/adjacent line and the second one's pointer writes
        // run into the visible area (the near-fully-open frames).  When skipped, the
        // tunnel covers the last 1-2 rows of the gap, which is imperceptible.
        const uint16_t botWaitY = (uint16_t)(kTerrainLine + half + g2 - 1);
        if (botWaitY + 2 <= (uint16_t)(kCockpitLine - 1)) {
            d[idx++] = copperWait(botWaitY, 0xE0);
            emitBpl(ta + (uint32_t)half * 120u);
            emitTerrCols();
        }
    }
    }   // end !rsViewport

    // ---- cockpit region ------------------------------------------------------
    // The cockpit runs at 3 bitplanes (title/terrain stay 2bp): mode-4's bit-7
    // chars swap pixel-11 from COLPF2 (salmon) to COLPF3 (red), which can't be
    // expressed in 2bp since salmon and red share scanlines.  We give those chars
    // plane3 = 0xFF (render()), shifting their pixels from colours 0-3 to 4-7;
    // colours 4-6 mirror 0-2, and colour 7 = red, so a bit-7 char matches a normal
    // one except its pixel-11 is red.
    d[idx++] = copperWait(kCockpitLine - 1, 0xE0);
    uint32_t ca = (uint32_t)cockpitBitmap->data;
    d[idx++] = copperMove(bpl1pth, (uint16_t)(ca >> 16));
    d[idx++] = copperMove(bpl1ptl, (uint16_t)(ca & 0xFFFF));
    d[idx++] = copperMove(bpl2pth, (uint16_t)((ca + 40) >> 16));
    d[idx++] = copperMove(bpl2ptl, (uint16_t)((ca + 40) & 0xFFFF));
    d[idx++] = copperMove(bpl3pth, (uint16_t)((ca + 80) >> 16));
    d[idx++] = copperMove(bpl3ptl, (uint16_t)((ca + 80) & 0xFFFF));
    // Switch to 3 bitplanes + 3bp interleaved modulo (= (3-1)*40) for the cockpit.
    d[idx++] = copperMove(bplcon0, (uint16_t)((3 << PLNCNTSHFT) | USE_BPLCON3));
    d[idx++] = copperMove(bpl1mod, 80);
    d[idx++] = copperMove(bpl2mod, 80);
    // Cockpit palette — cockpit DLIs ($6D4F/$6D67/$6D7C) reload the registers for
    // these scanlines with hardcoded immediates (NOT the $02C4-$02C8 title/terrain
    // shadows), so we use the same constants:
    //   pixel 00 = COLBK  = $00 (black);   01 = COLPF0 = $04 (dark gray)
    //   pixel 10 = COLPF1 = $06 (gray);    11 = COLPF2 = $2C (salmon)
    //   bit-7 char pixel 11 = COLPF3 = $26 (red), via plane3 → colour 7.
    d[idx++] = copperMove(color00, atariToOCS(0x00));
    d[idx++] = copperMove(color01, atariToOCS(0x04));
    d[idx++] = copperMove(color02, atariToOCS(0x06));
    d[idx++] = copperMove(color03, atariToOCS(0x2C));
    d[idx++] = copperMove(color04, atariToOCS(0x00));  // = col0 (black)
    d[idx++] = copperMove(color05, atariToOCS(0x04));  // = col1 (dark gray)
    d[idx++] = copperMove(color06, atariToOCS(0x06));  // = col2 (gray)
    d[idx++] = copperMove(color07, atariToOCS(0x26));  // red (COLPF3)

    // Terminate the list right after the content.  The instruction count varies
    // per frame (the door split adds/removes bands), and the two double-buffered
    // lists are rebuilt at different gaps — without a terminator here the copper
    // runs into each buffer's stale trailing instructions (old band WAITs/pointer
    // writes), which differ between buffers and flicker the display every frame.
    d[idx++] = copperWait(255, 254);
}

// ---- public interface --------------------------------------------------------
void RescueOnFractalus::initialize()
{
    titleBitmap   = Bitmap::allocate(kW, kTitleHeight,   kBP2, true);
    terrainBitmap = Bitmap::allocate(kW, kTerrainHeight, kBP3, true);  // 3bp: tunnel reveal uses pens 4-7
    cockpitBitmap = Bitmap::allocate(kW, kCockpitH, kBP3, true);  // 3bp: bit-7 chars → red
    tunnelBitmap  = Bitmap::allocate(kW, kTerrainHeight, kBP3, true);  // door-gap reveal

    // (The tunnel rings are decoded into tunnelBitmap from the $1000 field by
    // decodeTunnelField, triggered by the platform_tunnel_rings_drawn() hook when the
    // genuine display_setup draws them — not at init; see decodeTunnelField.)

    leftPost   = Sprite::allocate(kHT);
    rightPost  = Sprite::allocate(kHT);
    nullSprite = Sprite::allocate(0);
    gaugeSprite = Sprite::allocate(57);    // throttle bar: 57 vobj-strip rows ($0D98..$0DD0)
    if (!leftPost || !rightPost || !nullSprite || !gaugeSprite) return;
    // Starfield sprites (P0/P2/P3): 89-row strips, all at the windscreen top
    // (player scanline $32 → Amiga Y = kTerrainLine, the +36 offset that maps the
    // gauge strip $0D98/scanline $98 to Amiga Y 0x2c+144).
    for (int c = 0; c < 3; c++) {
        starSprite[c] = Sprite::allocate(kStarRows);
        if (!starSprite[c]) return;
        starSprite[c]->setX(kStarX[c]);
        starSprite[c]->setY(kTerrainLine);
    }
    // Player 1 is the throttle gauge: original HPOSP1 = mem[$00B5] = $BE, single-
    // line PMG strip at $0D98 (P1+$98).  The Atari-HPOS / PM-scanline -> Amiga-pixel
    // transform isn't 1:1 (wide-playfield crop + DIWSTRT), so the on-screen XY here
    // is a starting estimate to calibrate visually.
    gaugeSprite->setX(0x81 + 203);
    gaugeSprite->setY(0x2c + 144);

    fillSpriteData(leftPost,  false);
    fillSpriteData(rightPost, true);

    leftPost->setX(kSprXLeft);
    leftPost->setY(kTerrainLine);
    rightPost->setX(kSprXRight);
    rightPost->setY(kTerrainLine);

    for (int i = 0; i < 2; i++) {
        copperLists[i] = CopperList::allocate(kCopperLen);
    }
    if (!copperLists[0] || !copperLists[1]) return;

    // One-time playfield setup: the constant display registers (FMODE, BPLCON3/2/1,
    // DIWSTRT/STOP/HIGH, DDFSTRT/STOP) never change, so set them ONCE here via the CPU
    // instead of re-running those MOVEs in every frame's copper list.  The copper lists
    // only emit the per-region-varying BPLCON0 + BPL1MOD/BPL2MOD (via CopperList::setPlayfield).
    // BPLCON2 is then overridden to the game's GPRIOR=$14 priority (PF1P=PF2P=1: playfield
    // in front of sprite pair 1+ / the throttle gauge, behind pair 0 / the canopy posts).
    AmigaHardware::setPlayfield(kW, kH, kBP2, /*interleaved*/true, /*hires*/false,
                                /*interlace*/false, /*dualPlayfield*/false,
                                /*holdAndModify*/false, kCenterY);
    *bplcon2Pointer = (uint16_t)((1u << 3) | 1u);

    deriveRenderSignals();   // seed the render signals from the initial mem[] (standby) state
    buildCopperList(copperLists[0], 0);
    buildCopperList(copperLists[1], 0);
    active = 0;

    // Show a blank black display until the boot/standby build is ready (g_standbyRevealReady).
    // The real lists point at bitmaps that are still being built during the multi-second boot;
    // holding the EmptyCopperList avoids showing that piecemeal build (replaces the fade-to-black).
    emptyCopper = new EmptyCopperList();
    if (emptyCopper && emptyCopper->data()) {
        emptyCopper->buildLayout(*nullSprite);
        AmigaHardware::setCopperList(*emptyCopper, true);
        emptyCopperInstalled = true;
    } else {
        AmigaHardware::setCopperList(*copperLists[active], true);
    }

    // Static-Standby fixed copper list: built once here (bitmaps + sprites now exist),
    // its dynamic colour/sprite slots refreshed each frame by updateStandbyCopper.
    // renderFrame installs it once the doors are decoded and the scene settles into
    // Standby; until then the double-buffered buildCopperList path drives the build.
    standbyCopper = new StandbyCopperList();
    if (standbyCopper && standbyCopper->data())
        standbyCopper->buildLayout(*titleBitmap, *terrainBitmap, *cockpitBitmap,
                                   *leftPost, *rightPost, *nullSprite);

    // Static stars/planet viewport fixed copper list (the line-doubled mode-D band),
    // same build-once + poke-in-place scheme; renderFrame installs it during rsStars.
    planetCopper = new PlanetCopperList();
    if (planetCopper && planetCopper->data())
        planetCopper->buildLayout(*titleBitmap, *terrainBitmap, *cockpitBitmap,
                                    *leftPost, *rightPost, *gaugeSprite, *nullSprite,
                                    *starSprite[0], *starSprite[1], *starSprite[2]);

    // Static flight fixed copper list (scene 7), same build-once + poke scheme;
    // renderFrame installs it during rsFlight.  HUD sprites are poked in later by the
    // ported flight VBI; buildLayout seeds posts + gauge + nulls.
    flightCopper = new FlightCopperList();
    if (flightCopper && flightCopper->data())
        flightCopper->buildLayout(*titleBitmap, *terrainBitmap, *cockpitBitmap,
                                  *leftPost, *rightPost, *gaugeSprite, *nullSprite);

    // Precompute glyph doubling table: each byte → 16-bit pattern (each bit → 2 bits).
    for (int i = 0; i < 256; i++) {
        uint16_t out = 0;
        for (int b = 7; b >= 0; b--)
            if (i & (1 << b)) out |= (uint16_t)3u << (b * 2);
        kDoubleGlyph[i] = out;
    }

    // Precompute the mode-D (2bpp) and GTIA-10 (nibble) byte→bitplane decode tables.
    for (int s = 0; s < 256; s++) {
        uint8_t pa = 0, pc = 0;                       // mode-D: 4 pixels × 2 bits
        for (int i = 0; i < 4; i++) {
            uint8_t px   = (uint8_t)((s >> (6 - i * 2)) & 3u);
            uint8_t mask = (uint8_t)(0xC0u >> (i * 2));   // 0xC0,0x30,0x0C,0x03
            if (px & 1u) pa |= mask;
            if (px & 2u) pc |= mask;
        }
        kModeDP1[s] = pa; kModeDP2[s] = pc;
        uint8_t ph = (uint8_t)((s >> 4) & 0xF), pl = (uint8_t)(s & 0xF);   // GTIA-10
        kGtia10P1[s] = (uint8_t)(((ph & 1) ? 0xF0u : 0u) | ((pl & 1) ? 0x0Fu : 0u));
        kGtia10P2[s] = (uint8_t)(((ph & 2) ? 0xF0u : 0u) | ((pl & 2) ? 0x0Fu : 0u));
        kGtia10P3[s] = (uint8_t)(((ph & 4) ? 0xF0u : 0u) | ((pl & 4) ? 0x0Fu : 0u));
        // Standby door field: map each nibble through kNibbleColour first, then split
        // the resulting pen into plane1 (bit0) / plane2 (bit1) bytes.
        uint8_t ch = kNibbleColour[ph], cl = kNibbleColour[pl];
        kDoorP1[s] = (uint8_t)(((ch & 1) ? 0xF0u : 0u) | ((cl & 1) ? 0x0Fu : 0u));
        kDoorP2[s] = (uint8_t)(((ch & 2) ? 0xF0u : 0u) | ((cl & 2) ? 0x0Fu : 0u));
    }

    PlatformAmiga::audioInit();   // init Paula audio DMA (mem[] already loaded by run())

    // Patch mem[] values that are mid-animation in the snapshot.
    // (Must happen before initial render() call below.)
    // screen3_mem.bin was captured mid-animation; game_entry sets these on startup.
    // mem[$0071]: COLBK source for terrain rows (DLI dli_sub_6cf1 reads it).
    //   Snapshot has $DB (mid-animation); attract init targets $C8 (green, $C8=hue12/luma4).
    //   SDL oracle (atari000.png) shows terrain as (82,140,22) = $C8.
    mem[zp::displayFlags] = 0xC8;   // COLBK source for terrain DLI → green
    mem[0x02C0] = 0x00;   // COLPM0 → nibble-0 terrain dots; $00=black matches SDL oracle
                           // (music_playing.a8s has $18=orange from mid-animation)

    // mem[$00D8] is the title TEXT colour (COLPF0); snapshot has $78 (blue). Leave as-is.
    // mem[$02C8] is the title BACKGROUND (COLBK); snapshot has $06 (grey). Leave as-is.

    // (Removed: $0044/$063E patches that used to placate vbi_handler_flight's FUN_47A3 /
    // $480B paths — neither handler is ported on the Amiga, so nothing reads those
    // bytes here; the writes were dead.)

    // Initial render: populate all three bitmaps from mem[] once so that
    // render() called from the main loop has nothing to do until data changes.
    // This captures the closed-door terrain image from $2000 into terrainBitmap.
    render();

    // The tunnel bitmap is NOT decoded here: the genuine display_setup draws the ring
    // pattern into the $1000 field via draw_frame_pattern_seq, and the platform_tunnel_
    // rings_drawn() hook flags it for decode then (advance_message_column streams the
    // per-frame ring-clear updates).  Decoding at init would capture an empty $1000.

    // Init complete — release the ISR's "scene ready" gate (g_activeVbi != 0).  The ISR
    // dispatches on the live VVBLKI vector ($0222/$0223), so seed it to $52D7 (standby)
    // for the brief window before run()->station_init installs the $1B30 attract vector;
    // otherwise the snapshot's stale VVBLKI could pick the wrong body for a few frames.
    mem[0x0222] = 0xD7u;   // VVBLKI lo
    mem[0x0223] = 0x52u;   // VVBLKI hi  -> $52D7 standby VBI
    g_activeVbi = 1;
}

// decodeTunnelField: decode rows [rowLo..rowHi] of the GTIA-10 tunnel-ring field at
// mem[$1000] into the tunnel bitmap, PER-BYTE shadow-gated so only the bytes that actually
// changed are re-decoded.  $1000 (NOT $2000, which holds the door field) is where the
// genuine display_setup renders the rings — draw_frame_pattern_seq plots through the
// $073D/$0793 row-address table built for base $1000, and advance_message_column streams
// its expanding black ring-clear frames into the same buffer.  The exit clear draws a thin
// black frame OUTLINE each step (horizontal edges full-width + left/right VERTICAL pieces
// down the inner rows), so even passing the full new row extent [botAfter..topAfter] here
// touches only the outline bytes — staying far under one PAL frame (GTIA-10 decode tables).
void RescueOnFractalus::decodeTunnelField(int rowLo, int rowHi)
{
    if (!tunnelBitmap) return;
    if (rowLo < 0) rowLo = 0;
    if (rowHi > (int)kTerrainHeight - 1) rowHi = (int)kTerrainHeight - 1;
    // Pointer-walk (no per-row 68000 multiplies — row*46/*120/*40 would be __mulsi3
    // soft-multiplies each row).  Compute the row-0 bases once, then += stride per row,
    // as render()/renderViewportModeD() do.
    const uint8_t* src   = (const uint8_t*)&mem[0x1000 + rowLo * 46 + 4];  // +4: wide-field crop
    uint8_t*       p1     = (uint8_t*)tunnelBitmap->data + rowLo * 120;
    uint8_t*       shadow = &tunnelShadow[rowLo * 40];
    for (int row = rowLo; row <= rowHi; row++) {
        uint8_t* pp2 = p1 + 40; uint8_t* pp3 = p1 + 80;
        for (int b = 0; b < 40; b++) {
            uint8_t s = src[b];
            if (s != shadow[b]) {                  // changed byte — re-decode 3 planes
                shadow[b] = s;
                p1[b] = kGtia10P1[s]; pp2[b] = kGtia10P2[s]; pp3[b] = kGtia10P3[s];
            }
        }
        src += 46; p1 += 120; shadow += 40;        // walk to next row
    }
}

// renderViewportModeD: decode the stars/planet viewport buffer mem[$1000] as an
// ANTIC mode-D field into terrainBitmap.  Layout (verified vs launch_5_planet.a8s
// row-addr table $073D/$0793): 43 mode-D rows, 48 bytes/row (WIDE playfield), the
// central 40 displayed (+4 crop, as terrain/cockpit); each mode-D row is 2 display
// scanlines, so 43*2 = 86 = kTerrainHeight.  mode-D is 2bpp: byte = 4 pixels (2
// bits each) -> Amiga colour 0-3 (plane1=bit0, plane2=bit1); plane3 unused (0).
// srcBase/stride/rows parameterise the source: stars/planet = ($1000, 48, 43);
// flight = ($1070, 96, 43) — flight's mode-D rows are stride 96 (two 48-byte
// double-buffer halves; offset 0 is the displayed half) LMS'd from $1070 (= the
// $1010 row-addr base + one off-screen scroll-margin row).  The +4 crop centres
// the displayed 40 of 48 either way.
void RescueOnFractalus::renderViewportModeD(uint16_t srcBase, int stride, int rows)
{
    if (!terrainBitmap) return;
    static const int kCrop   = 4;    // central 40 of 48 (centres content)

    // Write each mode-D row to ONE interleaved scanline; the copper line-doubles the
    // region vertically (buildCopperList's viewport band toggles the bitplane modulo
    // -40/+80 per scanline, re-displaying each row twice).  Layout per row: 40 plane1
    // bytes, plane2 at +40, plane3 (always 0) at +80, then +120 to the next scanline.
    //
    // Decode 4 source bytes at a time into one 32-bit store per plane (the 68000 (An)+
    // mode), as the Standby door decoder does: the plane bytes go to DMA-contended CHIP
    // RAM, so throughput is dominated by the store count and longs roughly halve it vs
    // byte writes.  But the planet zoom / star scroll leaves much of the field static
    // frame-to-frame, so guard each long with a long-granular shadow: skip the (q1,q2)
    // stores when the 4-byte source group is unchanged.  plane3 is always 0 in mode-D —
    // clear it only on a forceFull frame (entry / source-base change); nothing writes it
    // during the viewport, so it stays 0 thereafter.  vdest is chip-aligned and the
    // +40/+80/120 offsets keep every long aligned.
    const bool full = viewportForceFull || (srcBase != viewportLastBase);
    viewportForceFull = false;
    viewportLastBase  = srcBase;

    // Dirty-row band.  For the stars/planet source ($1000) the planet renderer
    // (draw_vline_pair, the only writer of this field — validated: every shadow-detected
    // change lay inside its reported extent) records the rows it touched in
    // g_planetRowLo/Hi.  Decode only that band instead of scanning all 43 rows: the field
    // lives in DMA-contended CHIP RAM, so the full per-frame scan cost ~17 ms even though
    // only ~3 rows change.  full frames (entry) still decode everything (and clear plane3).
    // The flight source ($1070) has a different writer, so it keeps the full scan + shadow.
    extern volatile unsigned long g_planetRowLo, g_planetRowHi;
    int rStart = 0, rEnd = rows - 1;
    if (srcBase == 0x1000u && !full) {
        rStart = (int)g_planetRowLo;
        rEnd   = (int)g_planetRowHi;
        g_planetRowLo = 9999; g_planetRowHi = 0;             // consume for next frame
        if (rEnd < rStart) return;                           // nothing drawn this frame
        if (rStart < 0) rStart = 0;
        if (rEnd >= rows) rEnd = rows - 1;
    }

    const uint8_t* src = (const uint8_t*)&mem[srcBase + kCrop] + (unsigned)rStart * stride;
    uint8_t* vdest    = (uint8_t*)terrainBitmap->data + (unsigned)rStart * 120;
    uint32_t* shadow  = viewportShadow + rStart * 10;
    for (int row = rStart; row <= rEnd; row++, src += stride) {
        const uint8_t* rs = src;
        uint32_t* q1 = (uint32_t*)vdest;
        uint32_t* q2 = (uint32_t*)(vdest + 40);
        uint32_t* q3 = (uint32_t*)(vdest + 80);
        for (int b = 0; b < 10; b++, q1++, q2++, q3++, shadow++) {   // 10 longs = 40 bytes
            uint8_t s0 = rs[0], s1 = rs[1], s2 = rs[2], s3 = rs[3]; rs += 4;
            uint32_t key = ((uint32_t)s0 << 24) | ((uint32_t)s1 << 16) |
                           ((uint32_t)s2 <<  8) |  (uint32_t)s3;
            if (!full && key == *shadow) continue;           // 4-byte group unchanged
            *shadow = key;
            *q1 = ((uint32_t)kModeDP1[s0] << 24) | ((uint32_t)kModeDP1[s1] << 16) |
                  ((uint32_t)kModeDP1[s2] <<  8) |  (uint32_t)kModeDP1[s3];
            *q2 = ((uint32_t)kModeDP2[s0] << 24) | ((uint32_t)kModeDP2[s1] << 16) |
                  ((uint32_t)kModeDP2[s2] <<  8) |  (uint32_t)kModeDP2[s3];
            if (full) *q3 = 0u;                              // plane3 unused; clear once on entry
        }
        vdest += 120;                                        // one interleaved scanline
    }
}

// run(): the whole game, driven by the genuine transpiled/native boot chain
// (game_entry -> game_main_loop -> display_setup -> flight).  That chain is
// straight-line 6502 control flow that busy-waits between phases while its VBI/DLI
// interrupts animate the screen; each original wait point is a SPINWAIT_HOOK that
// calls platform_render_frame -> renderFrame (render + wait for next VBI),
// and the VBI body follows the live VVBLKI vector the chain installs per scene
// (game_vbi_isr dispatches $1B30/$52D7/$4FF5 automatically).  Returns when the user
// quits (left mouse button), unwound here via __builtin_longjmp.
void RescueOnFractalus::run()
{
    // ---- the genuine transpiled boot chain drives EVERYTHING --------------------
    // mem[] is the pristine rof.xex image (load_xex_image in PlatformAmiga::run): every
    // segment at its load address, all runtime state at genuine power-on values
    // ($00E7 music gate = 0, etc.).  rof.xex boots via a chain of INITAD stubs run by
    // the OS loader: $5000 (Logo) -> $1A97 (Station cinematic) -> $B800 (display setup)
    // -> $3CDE (game_entry, the final INITAD = the real program entry).
    //
    // FOUNDATION SCOPE (parts 1+2): our port begins at Standby (the hangar), so we skip
    // the Logo + Station-cinematic INITADs ($5000/$1A97 — deferred: they need their own
    // screenmodes + native twins for the heavy transpiled animation) and enter directly
    // at game_entry ($3CDE).  game_entry's 737-byte mega-init establishes the genuine
    // game state itself — including calling $70E7 to start the Standby music (setting
    // $00E7) at the right moment — instead of inheriting it from a hand-crafted snapshot.
    //
    // game_main_loop's flight loop never returns; the user-quit path unwinds the whole
    // transpiled call stack back here via __builtin_longjmp (armed below).  Each frame-
    // wait spin loop is a SPINWAIT_HOOK driving a real Amiga frame through the pump
    // (platform_render_frame); the VBI body follows the live VVBLKI vector
    // game_entry installs (game_vbi_isr dispatches $52D7/$4FF5 automatically).
    if (__builtin_setjmp(g_quitJmp) != 0) return;   // quit: unwound here from renderFrame/pollEvents

    game_entry();     // $3CDE: mega-init -> game_main_loop (Standby -> cinematic -> flight); never returns
}

// renderFrame(): the per-frame repaint body, called from PlatformAmiga::renderFrame()
// at each transpiled frame-wait hook.  Does the non-phase per-frame work, repaints the
// bitmaps, rebuilds the back copper list and flips to it.  The VBI has not yet fired
// when this is entered; rendering happens first, then the caller waits for the VBI.
void RescueOnFractalus::renderFrame()
{
    // Black-until-ready: while the boot/standby build is still in progress, keep the blank
    // EmptyCopperList on screen and do no rendering — the bitmaps are mid-build and the real
    // lists would show garbage.  When g_standbyRevealReady latches, fall through and the copper
    // path below installs the real (standby / viewport / dynamic) list for this frame.
    if (emptyCopper && !g_standbyRevealReady) {
        if (!emptyCopperInstalled) {
            AmigaHardware::setCopperList(*emptyCopper, false);
            emptyCopperInstalled = true;
            standbyCopperInstalled = false; planetCopperInstalled = false;
        }
        return;
    }
    emptyCopperInstalled = false;

    frameCounter++;
    deriveRenderSignals();   // recompute the mem[]-derived render-gating signals for this frame
    // Tunnel reveal: the $52D7 VBI's advance_message_column draws the expanding black
    // clear into mem[$2000] and flags g_tunnelFieldDirty with its row extent; re-decode
    // those rows into tunnelBitmap here (was in run()'s tunnel loop, now that the
    // transpiled display_setup drives the cinematic).
    if (g_tunnelFieldDirty) {
        decodeTunnelField((int)g_tunRowLo, (int)g_tunRowHi); g_tunnelFieldDirty = 0;
    }
    perFrameWork();
    render();

    // Static Standby (incl. the gauge-fill sub-phase before the doors scroll): the
    // copper layout buildCopperList would emit is FIXED here (!rsViewport, doors not
    // parting), so drive the single fixed StandbyCopperList by poking only changed
    // colour/sprite slots — no per-frame full rebuild, no double-buffer flip.  Gated
    // on g_doorFieldReady (doors decoded, fade reveal done -> global fade is 16).
    const bool staticStandby = standbyCopper && rsStandby && g_doorFieldReady
                               && !rsViewport && !rsLaunched;
    if (staticStandby) {
        if (!standbyCopperInstalled) {
            updateStandbyCopper(true);
            AmigaHardware::setCopperList(*standbyCopper, false);
            standbyCopperInstalled = true;
        } else {
            updateStandbyCopper(false);
        }
        planetCopperInstalled = false;
        return;
    }

    // Static stars/planet viewport: the rsViewport (non-flight) copper layout is FIXED
    // (one line-doubled mode-D band — see PlanetCopperList).  render() has already
    // refreshed the bitmap content and buildStarSprites the sprite data, both at constant
    // pointers, so only a few colours change — poke them in place, no full rebuild/flip.
    const bool staticPlanet = planetCopper && rsStars;
    if (staticPlanet) {
        if (!planetCopperInstalled) {
            updatePlanetCopper(true);
            AmigaHardware::setCopperList(*planetCopper, false);
            planetCopperInstalled = true;
        } else {
            updatePlanetCopper(false);
        }
        standbyCopperInstalled = false;
        flightCopperInstalled = false;
        return;
    }

    // Static flight: the flight copper layout is FIXED too (same line-doubled mode-D band,
    // flight palette + HUD sprites — see FlightCopperList).  render() refreshes the terrain
    // bitmap content (constant pointer); the ported flight VBI pokes the per-frame colours/
    // sprites via updateFlightCopper.  No full rebuild/flip.
    const bool staticFlight = flightCopper && rsFlight;
    if (staticFlight) {
        if (!flightCopperInstalled) {
            updateFlightCopper(true);
            AmigaHardware::setCopperList(*flightCopper, false);
            flightCopperInstalled = true;
        } else {
            updateFlightCopper(false);
        }
        standbyCopperInstalled = false;
        planetCopperInstalled = false;
        return;
    }

    // Dynamic phases (door-open + tunnel cinematic): the layout varies per frame, so keep
    // the double-buffered full rebuild + flip for now (DoorsCopperList/TunnelCopperList to
    // come).  Flight and stars/planet are handled by their fixed lists above.
    uint8_t next = 1 - active;
    buildCopperList(copperLists[next], frameCounter);
    AmigaHardware::setCopperList(*copperLists[next], false);
    active = next;
    standbyCopperInstalled = false;   // left Standby — next static entry re-seeds + re-installs
    planetCopperInstalled = false;
    flightCopperInstalled = false;
}

// updateStandbyCopper(): refresh the StandbyCopperList's per-frame-varying colour and
// sprite slots from mem[].  Colours are written straight as atariToOCS(byte) — there is no
// fade (the screen is held black by the EmptyCopperList until g_standbyRevealReady, after
// which colours show at full intensity).  Each slot is poked only when its value changed
// since last frame (force = poke all, on install).
void RescueOnFractalus::updateStandbyCopper(bool force)
{
    const uint16_t titleBg  = atariToOCS(mem[0x02C8]);             // COLBK = title bg / canopy posts
    const uint16_t titlePf0 = atariToOCS(mem[zp::textColorPf0]);   // COLPF0 = title text ($00D8)
    const uint16_t gaugeCol = atariToOCS(mem[0x00DE]);             // gauge bar colour ramp
    const uint16_t terr0    = atariToOCS(mem[0x02C0]);             // terrain pen0 (road dots)
    const uint16_t terr1    = atariToOCS(mem[0x02C7]);             // terrain pen1 (LEVEL text)
    const uint16_t terr2    = atariToOCS(mem[zp::colorRing]);      // terrain pen2 ($08D4)
    const uint16_t terr3    = atariToOCS(mem[zp::displayFlags]);   // terrain pen3 (green bg, $0071)
    const int8_t   gauge    = (int8_t)(rsGauge ? 1 : 0);

    if (force || titleBg != sbTitleBg || titlePf0 != sbTitlePf0) {
        standbyCopper->setTitlePalette(titleBg, titlePf0, atariToOCS(0x78));  // pf1 = blue (const)
        standbyCopper->setSpritePostColor(titleBg);
        sbTitleBg = titleBg; sbTitlePf0 = titlePf0;
    }
    if (force || gaugeCol != sbGaugeCol) {
        standbyCopper->setGaugeColor(gaugeCol);
        sbGaugeCol = gaugeCol;
    }
    if (force || terr0 != sbTerr0 || terr1 != sbTerr1 || terr2 != sbTerr2 || terr3 != sbTerr3) {
        // Any terrain pen changed: rewrite all four (terr3 is the dark->bright green fade).
        standbyCopper->setTerrainPalette(terr0, terr1, terr2, terr3);
        sbTerr0 = terr0; sbTerr1 = terr1; sbTerr2 = terr2; sbTerr3 = terr3;
    }
    if (force || gauge != sbGauge) {
        standbyCopper->setSprite2(gauge ? *gaugeSprite : *nullSprite);
        sbGauge = gauge;
    }
}

// updatePlanetCopper(): refresh the PlanetCopperList's per-frame-varying colour
// slots from mem[].  Same fade-16-identity / poke-only-on-change scheme as
// updateStandbyCopper.  The structural slots (bitmap + sprite pointers, line-doubling
// band) are constant, set once in buildLayout — only these colours move per frame.
void RescueOnFractalus::updatePlanetCopper(bool force)
{
    const uint16_t titleBg  = atariToOCS(mem[0x02C8]);             // COLBK = title bg / canopy posts
    const uint16_t titlePf0 = atariToOCS(mem[zp::textColorPf0]);   // COLPF0 = title text ($00D8)
    const uint16_t gaugeCol = atariToOCS(mem[0x00DE]);             // gauge bar colour ramp
    const uint16_t starCol  = atariToOCS(mem[0x02C0]);             // starfield grey ($02C0)
    const uint16_t bgCol    = atariToOCS(mem[0x00DC]);             // viewport COLBK (space, $00DC)

    if (force || titleBg != plTitleBg || titlePf0 != plTitlePf0) {
        planetCopper->setTitlePalette(titleBg, titlePf0, atariToOCS(0x78));  // pf1 = blue (const)
        planetCopper->setSpritePostColor(titleBg);
        plTitleBg = titleBg; plTitlePf0 = titlePf0;
    }
    if (force || gaugeCol != plGaugeCol) {
        planetCopper->setGaugeColor(gaugeCol);
        plGaugeCol = gaugeCol;
    }
    if (force || starCol != plStarCol) {
        planetCopper->setStarColor(starCol);
        plStarCol = starCol;
    }
    if (force || bgCol != plBg) {
        planetCopper->setPlanetBgColor(bgCol);
        plBg = bgCol;
    }
}

// updateFlightCopper(): refresh the FlightCopperList's per-frame-varying slots from mem[].
// Same poke-only-on-change scheme.  The terrain pens are baked constant in buildLayout
// (the salmon→brown fade will poke setTerrainPen0 from the native atmosphere code); the
// HUD sprite pointers are poked by the ported flight VBI.  Here we keep the top-bar / gauge
// colours live, matching the flight branch of the legacy buildCopperList.
void RescueOnFractalus::updateFlightCopper(bool force)
{
    const uint16_t titleBg  = atariToOCS(mem[0x02C8]);             // COLBK = top-bar bg / canopy posts
    const uint16_t titlePf0 = atariToOCS(mem[zp::textColorPf0]);   // COLPF0 = top-bar text ($00D8)
    const uint16_t gaugeCol = atariToOCS(mem[0x00DE]);             // gauge bar colour

    if (force || titleBg != flTitleBg || titlePf0 != flTitlePf0) {
        flightCopper->setTitlePalette(titleBg, titlePf0, atariToOCS(0x78));  // pf1 = blue (const)
        flightCopper->setSpritePostColor(titleBg);
        flTitleBg = titleBg; flTitlePf0 = titlePf0;
    }
    if (force || gaugeCol != flGaugeCol) {
        flightCopper->setGaugeColor(gaugeCol);
        flGaugeCol = gaugeCol;
    }
}

// deriveRenderSignals(): recompute the renderer's phase-gating signals from mem[]
// hardware state, once per frame.  These replace the C++ launchPhase enum as the
// renderer's source of truth, so buildCopperList/render/perFrameWork keep working as
// the transpiled game_entry/game_main_loop/display_setup drive the program.
//
// Scene identity comes from the LIVE VVBLKI vector ($0222/$0223) the genuine flow
// installs per scene — NOT the raw DLI byte $0200.  game_main_loop loops over
// display_setup; while display_setup BUILDS the Standby display it sets $0200 to many
// DLI handlers in turn (including the $6CC2 mode-D one), so the old `$0200==$C2` test
// faked "stars" during Standby and drew an empty mode-D viewport (the black-middle bug).
//   $53CC  game_main_loop init / transitional (screen rebuilding)
//   $52D7  Standby + the launch cinematic (Doors/Tunnel/Planet)
//   $4FF5  in-flight
// Within the $52D7 scene, the launch sub-state distinguishes the phases.  $060B
// (cockpit_flag) takes several values: 0/1 during the Standby/attract churn (the idle
// loop INCs it; the $52D2 cockpit-setup writes $04), but the genuine LAUNCH sets it to
// $23 ($6364, the START-pressed path).  So $060B==$23 cleanly means "launch underway":
//   flight   : VVBLKI == $4FF5
//   stars    : VVBLKI == $52D7 && $060B == $23 (launch) && $0200 == $C2 (mode-D DLI
//              installed) — i.e. the genuine stars/planet viewport phase.  Gating on
//              $060B==$23 (not !=0) rejects the transient $6CC2 the Standby build sets
//              while $060B is 0/1 (the old $0200==$C2 test drew an empty viewport then —
//              the black-middle bug).
//   viewport : stars || flight                      — the mode-D viewport band is active
//   gauge    : $060B != 0                            — cinematic/cockpit active (gauge strip)
// NOTE: the $060B==$23 stars gate is verified to fix Standby (where $060B is 0/1/4);
// the stars/planet sub-phase value must be reconfirmed once a START launch is reached.
void RescueOnFractalus::deriveRenderSignals()
{
    const uint16_t vvblki = (uint16_t)(mem[0x0222] | (mem[0x0223] << 8));
    const bool standbyVbi = (vvblki == 0x52D7u);   // Standby + launch cinematic
    const bool flightVbi  = (vvblki == 0x4FF5u);   // in-flight

    rsStandby  = standbyVbi;
    rsFlight   = flightVbi;
    rsStars    = standbyVbi && (mem[0x060B] == 0x23u) && (mem[0x0200] == 0xC2u);
    rsViewport = rsStars || rsFlight;
    rsGauge    = (mem[0x060B] != 0);

    // launched = doors scroll armed / ring armed / viewport active.  Safe to derive
    // now that the transpiled display_setup drives: it arms the ring before the next
    // platform_render_frame, so no frame renders in the doors-fully-open gap where this
    // would briefly read false (the artifact that kept this as a C++ bool through C4).
    rsLaunched = (mem[zp::terrainScrollCounter] != 0) || (mem[zp::vbiFlags] != 0) || rsViewport;

    // Door-fully-open latch.  When the door scroll completes ($008A counts $2B->0) there
    // is a multi-frame gap before the tunnel-ring dispatcher arms ($0088 set) / the
    // viewport takes over.  In that gap all of $008A/$0088/rsViewport are 0, so rsLaunched
    // would read false and the terrain region collapses back to the full closed-door
    // bitmap (the doors visibly "snap shut" for a beat, until ring cycling starts).  Latch
    // it: once the door has fully opened, keep rsLaunched true (so g2 stays at the full gap
    // and the tunnel bitmap is shown) until the cinematic leaves launch or a viewport scene
    // begins.  (The old comment here wrongly assumed the ring arms before the next frame.)
    {
        const uint8_t scroll = mem[zp::terrainScrollCounter];
        if (mem[0x060B] != 0x23u || rsViewport) doorsOpenedLatch = false;  // left launch / viewport took over
        else if (prevScrollCtr != 0u && scroll == 0u) doorsOpenedLatch = true;  // door just reached fully open
        prevScrollCtr = scroll;
        if (doorsOpenedLatch) rsLaunched = true;
    }

    // Re-arm the one-time Standby door capture (render() does it once when the doors
    // are built, gated on $00E7!=0) whenever the scene is NOT a settled Standby —
    // music off (building / not yet there), launched, or a viewport scene.  So each
    // fresh entry into Standby re-decodes the doors exactly once and then idles.
    if (g_doorFieldReady == 0u || rsLaunched || rsViewport) terrainDirty = true;

    // Force a full title + cockpit rescan while the scene is transitional (boot/building)
    // or in flight, plus ONCE on entry to the stars/planet viewport — so the initial
    // cockpit build (written by the transpiled display_setup, not the native writers) and
    // the per-frame flight cockpit updates are never missed.  During the settled stars/
    // planet phase the cockpit is static ($004A clear → update_cockpit_digits doesn't run),
    // so re-scanning ~580 cells EVERY frame was pure cost (~10 ms/frame on the A500); after
    // the one-time entry scan we rely on the writers' dirty flags (title alternation flags
    // g_titleDirty via the $782A copy hook).  (The g_doorFieldReady==0 window covers boot.)
    if (g_doorFieldReady == 0u || rsFlight || (rsStars && !prevRsStars)) {
        g_titleDirty = 1; g_cockpitDirty = 1;
    }
    prevRsStars = rsStars;
}

// perFrameWork(): per-frame non-phase work (the tail of the old update()).  These
// ran every frame regardless of cinematic phase, driven by the standby/flight VBI
// body + the main loop on the Atari; here they run once per renderFrame.
void RescueOnFractalus::perFrameWork()
{
    update_indicator_blink_native();    // $4131: cockpit blink lights (flight-VBI routine)
    // sfx_voice_tick_native() is driven by CIA-B Timer A at 25 Hz (main.cpp).

    // $62E7 SFX-reinit gate: when $0090 is non-zero the loop reinits the SFX sequence.
    if (mem[zp::sfxReinitGate]) {
        mem[0x073Au] = 0u;    // immediate underflow → next CIA tick loads note[0]
        mem[0x073Cu] = 0xFFu; // sequence ptr before index 0
        mem[zp::sfxReinitGate] = 0u;    // clear flag (as $70E7 does via STX $0090)
    }

    // Title text ("RESCUE ON FRACTALUS!" / copyright): the genuine standby loop
    // ($62FB) drives it — copy_altitude_graphic_to_screen ($782A) copies the block
    // the SFX sequencer selects (via $0091) into screen RAM $32B7 every frame.  We
    // don't re-copy it here; render() picks up the change by shadow-comparing $32B7.

    if (mem[zp::joystickSaved] != 0)            // $004A set when the game starts
        update_cockpit_digits_native();          // $3FFA: cockpit digit update

    if (rsGauge) buildGaugeSprite();
    // Starfield players $0C32/$0E32/$0F32: scrolled+seeded during stars, static
    // through the planet zoom, so map them both phases.
    if (rsStars) buildStarSprites();
}

// ---- cockpit helpers ---------------------------------------------------------
// Decode one 2bpp byte (modeD raw or mode4 glyph) → Amiga 2bp byte pair.
// Each byte contains 4 × 2-bit Atari pixels; each pixel expands to 2 Amiga pixels.
// Amiga colour index = {p2_bit, p1_bit} — same layout as terrain kNibbleColour.
static void decode2bppByte(uint8_t src, uint8_t* p1out, uint8_t* p2out)
{
    uint8_t p1 = 0, p2 = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t pixel = (src >> (6 - i*2)) & 3u;
        uint8_t mask  = (uint8_t)(0xC0u >> (i*2));  // 0xC0, 0x30, 0x0C, 0x03
        if (pixel & 1u) p1 |= mask;   // plane1 = bit 0 of colour index
        if (pixel & 2u) p2 |= mask;   // plane2 = bit 1 of colour index
    }
    *p1out = p1;
    *p2out = p2;
}

void RescueOnFractalus::render()
{
    unsigned short profR0 = flight_vbi_tick();   // whole-render() timer (flight only)
    // ---- terrain / door view ------------------------------------------------
    // Only re-render when terrainDirty (set in initialize(); cleared here).
    // During static Standby the terrain is constant ($88 = closed door).
    // DL $3000: 86 Mode-F rows from $2000, stride 46 (40 data + 6 pad), GTIA mode 10.
    // Each byte = two 4-bit nibbles → two GTIA pixels, each 4 Amiga pixels wide.
    // Interleaved 3bp row = 40 bytes plane1 + 40 plane2 + 40 plane3 (plane3 = 0
    // for the doors; the tunnel reveal fills it for pens 4-7).

    // Wide-playfield crop: the terrain region runs in a GTIA-10 *wide* playfield
    // (48 bytes / 192 colour clocks — SDMCTL bit set, DMACTL rewritten per region
    // by the DLI at $4F78).  The road/door content is centred for the 48-byte
    // field (centre dash at byte 23 ≈ wide centre), so the TV-visible 320px window
    // is the central 40 bytes — skip the 4 left overscan bytes (pure green fill).
    // This matches the title region, which already crops 2 mode-6 chars (= 16cc).
    static const int kTerrainXByteOffset = 4;
    if (rsViewport) {
        // Stars/planet (mem[$1000], stride 48) or flight (mem[$1070], stride 96 —
        // displayed offset-0 half).  Content changes every frame, so re-decode each
        // render(); the per-byte shadow keeps it cheap.
        if (rsFlight) {
            unsigned short r0 = flight_vbi_tick();
            renderViewportModeD(0x1070, 96, 43);
            g_flightProf.render += (unsigned short)(flight_vbi_tick() - r0);
        }
        else                        renderViewportModeD(0x1000, 48, 43);
    } else if (terrainDirty && g_doorFieldReady != 0u && !rsLaunched) {
        // Standby doors: decode the GTIA mode-10 door field at $2000 to the bitplanes
        // ONCE, then leave it.  The genuine display_setup builds $2000 AFTER
        // initialize() ran (so a capture at init grabbed the empty pristine RAM — the
        // garbled-doors bug).  g_doorFieldReady (latched in display_setup right after
        // blit_message_block/blit_numeric_readout draw the doors into $2000, BEFORE the
        // green fade delay_loop_c2_to_c9) is that "$2000 is built" signal — decode once
        // here, BEFORE the fade, so the live color03 (= mem[$0071]) ramp animates the
        // dark->bright green build on the real door pixels (was gated on $00E7 = the
        // music/build-END gate, AFTER the fade, so the doors popped in already-green).
        // Clear terrainDirty and do no per-frame work on the static Standby.
        // deriveRenderSignals re-arms terrainDirty when the scene leaves Standby, so
        // re-entering it re-captures the doors once.
        terrainDirty = false;
        // GTIA mode-10 nibble field → 3bp interleaved bitplanes via the precomputed
        // kDoorP1/kDoorP2 tables (one lookup per byte, no per-byte nibble math).  Read the
        // source through a non-volatile pointer — display_setup has finished writing $2000 by
        // now ($00E7 is set), so the volatile per-byte reloads the old loop forced were pure
        // overhead.  plane3 is always 0 for the doors, so clear it once per row by longs.
        // The plane bytes go to CHIP RAM (DMA-contended), so throughput is dominated by the
        // number of stores, not the arithmetic.  Pack 4 source bytes into one 32-bit store
        // per plane (10 longs/plane/row instead of 40 byte writes) and use *p++ post-increment
        // (the 68000's (An)+ mode).  vdest is chip-aligned; +40/+80 keep each plane long-
        // aligned.  Big-endian packing so plane[4k+n] = kDoorPx[src[4k+n]].  plane3 = 0.
        const uint8_t* sbase = (const uint8_t*)mem + 0x2000 + kTerrainXByteOffset;
        uint8_t* vdest = (uint8_t*)terrainBitmap->data;
        for (int row = 0; row < (int)kTerrainHeight; row++) {
            const uint8_t* src = sbase + row * 46;
            uint32_t* p1 = (uint32_t*)vdest;
            uint32_t* p2 = (uint32_t*)(vdest + 40);
            uint32_t* p3 = (uint32_t*)(vdest + 80);
            for (int b = 0; b < 10; b++) {                 // 10 longs = 40 bytes
                uint8_t s0 = *src++, s1 = *src++, s2 = *src++, s3 = *src++;
                *p1++ = ((uint32_t)kDoorP1[s0] << 24) | ((uint32_t)kDoorP1[s1] << 16) |
                        ((uint32_t)kDoorP1[s2] <<  8) |  (uint32_t)kDoorP1[s3];
                *p2++ = ((uint32_t)kDoorP2[s0] << 24) | ((uint32_t)kDoorP2[s1] << 16) |
                        ((uint32_t)kDoorP2[s2] <<  8) |  (uint32_t)kDoorP2[s3];
                *p3++ = 0u;
            }
            vdest += 120;
        }
    }

    // ---- title region -------------------------------------------------------
    // Shadow-compare: re-render only chars whose byte changed since last frame.
    // titleShadow[] mirrors $32B7-$32CA; updated here on change.
    // Chars start at $32B7 (skip $32B5/$32B6 left-border), charset $3800 (NTSC).
    static const int      kTitleTextRow  = 21;
    // The flight top-bar mode-6 line (flight DL $3123) is at $32B5 but the screen
    // runs WIDE playfield (24 chars, $32B5..$32CC); the visible central 20 are
    // $32B7..$32CA — same window the Standby title uses.  The score's right-most
    // digit lands at $32CA = col 19, so this window already covers it.
    static const uint16_t kScreenRAM = 0x32B7;
    // CHBAS=$04 ($0400): vbi_handler_standby sets this each VBI. dli_sub_4a0c fires
    // at scanY=28 (after title scanlines 20-27) → title uses $0400 for all 8 scans.
    static const uint16_t kCharsetBase  = 0x0400;

    // Walk the screen RAM + shadow with pointers (no per-col indexing).  The unchanged-
    // cell hot path is just `*src++ == *shadow++` — no 68000 muls.  68000 muls (~70cy) are
    // kept out of the per-cell path entirely: the changed-cell decode uses a row pointer
    // pre-offset once (kTitleTextRow*80 computed before the loop) and `*8`/`*2` are shifts.
    // Skip the whole title scan unless the genuine $782A writer (copy_altitude_graphic_to_
    // screen) rewrote $32B7-$32CA — it flags g_titleDirty through the platform_title_changed()
    // hook on each copy — or a full repaint is forced.  Cleared after the scan.
    if (g_titleDirty || cockpitForceFull) {
    uint8_t* tbmp = (uint8_t*)titleBitmap->data;
    uint8_t* const titleBase = tbmp + kTitleTextRow * 80;     // first text scanline row (once)
    const uint8_t* tsrc    = (const uint8_t*)mem + kScreenRAM;  // non-volatile walk (RAM static this frame)
    uint8_t*       tshadow = titleShadow;
    for (int col = 0; col < 20; col++) {
        uint8_t charByte = *tsrc++;
        if (charByte == *tshadow++) continue;   // unchanged — skip (shadow already advanced)
        tshadow[-1] = charByte;

        // Re-render this char: mode-6 is 1bpp, but the byte's top 2 bits select
        // the text colour register.  We support the two cases that occur here:
        //   hi2=0 → COLPF0 → col1 (plane1)   — copyright block
        //   hi2=1 → COLPF1 → col2 (plane2)   — RESCUE ON FRACTALUS! (blue)
        // "off" pixels stay col0 (COLBK).  Each glyph bit is doubled to 2 px.
        const uint8_t* glyph = (const uint8_t*)mem + kCharsetBase + (charByte & 0x3Fu) * 8u;
        const bool usePF1 = ((charByte >> 6) & 3u) == 1u;     // hi2=1 → COLPF1/blue
        uint8_t* row = titleBase + col * 2;                   // col*2 = shift; titleBase const
        for (int scanline = 0; scanline < 8; scanline++, row += 80) {
            if (kTitleTextRow + scanline >= (int)kTitleHeight) break;
            uint16_t doubled = kDoubleGlyph[*glyph++];
            uint8_t hb = (uint8_t)(doubled >> 8);
            uint8_t lb = (uint8_t)(doubled & 0xFF);
            // plane1 carries COLPF0 chars, plane2 carries COLPF1 chars.
            row[0]  = usePF1 ? 0 : hb;
            row[1]  = usePF1 ? 0 : lb;
            row[40] = usePF1 ? hb : 0;
            row[41] = usePF1 ? lb : 0;
        }
    }
    g_titleDirty = 0;
    }

    // ---- cockpit region ------------------------------------------------------
    // Per-cell shadow compare (like the title region above): re-decode only the
    // cells whose Atari source byte changed since last frame.  A single Atari
    // write therefore costs 6 Amiga writes (modeD: 2 rows × 3 planes) or 24
    // (mode4: 8 scanlines × 3 planes) — never a full-region re-decode.
    const bool cockpitFull = cockpitForceFull;
    cockpitForceFull = false;
    // The cockpit shares the terrain's WIDE playfield (48 bytes/line).  Both the
    // modeD and mode4 blocks are sequential DL entries (LMS only on the first
    // line), so each successive row advances by ANTIC's fetch width = 48 bytes,
    // NOT 40.  As with the terrain we render the central 40 bytes of each line
    // (+4-byte crop) so the content centres.  Using stride 40 / offset 0 mis-reads
    // every row past the first → the garbled "modulo" shear.
    //
    // ModeD $350D: 4 DL entries × 2 identical scan lines = 8 rows (raw 2bpp).
    // Mode4 $332D: 10 DL entries × 8 scan lines = 80 rows ($332D..$350D = 10×48).
    //   40 chars/row, glyph per scanline from charset $3800.
    //   Glyph byte = 4 × 2-bit pixels (COLBK/COLPF0/COLPF1/COLPF2).
    static const int kCockpitStride    = 48;   // wide-playfield bytes per line
    static const int kCockpitXByteCrop = 4;    // skip 4 left overscan bytes (= terrain)
    uint8_t* cdest = (uint8_t*)cockpitBitmap->data;

    // 3bp interleaved row = plane1(40) + plane2(40) + plane3(40) = 120 bytes.
    static const int kRowBytes = 120;

    // Skip both cockpit scans unless a writer (update_cockpit_digits / lock_on_indicator)
    // dirtied $332D-$352C or a full repaint is forced.  Cleared after the scans.
    if (cockpitFull || g_cockpitDirty) {
    // ModeD rows 0-7 (raw bitmap, no bit-7 colour swap → plane3 = 0).  Each
    // source byte feeds the same column of 2 identical scan lines.
    // Walk source/shadow/dest with pointers: the per-cell hot path is `*src++ == *shadow++`
    // with NO muls (the old `entry*40+b` shadow index and `(entry*2+scan)*kRowBytes` dest
    // offset were a 68000 muls per cell).  src advances +1/cell then skips the wide-field
    // overscan (+kCockpitStride-40) per entry; dest advances 2 rows (240B) per entry.
    {
        const uint8_t* src    = (const uint8_t*)mem + 0x350D + kCockpitXByteCrop;
        uint8_t*       shadow = cockpitModeDShadow;
        uint8_t*       drow   = cdest;
        for (int entry = 0; entry < 4; entry++) {
            uint8_t* d0 = drow;               // scan line 0 of this entry
            uint8_t* d1 = drow + kRowBytes;   // scan line 1 (identical content)
            for (int b = 0; b < 40; b++) {
                uint8_t s = *src++;
                uint8_t old = *shadow++;            // always advance shadow (cockpitFull-safe)
                if (!cockpitFull && s == old) continue;
                shadow[-1] = s;
                uint8_t p1v, p2v;
                decode2bppByte(s, &p1v, &p2v);
                d0[b] = p1v; d0[40 + b] = p2v; d0[80 + b] = 0;
                d1[b] = p1v; d1[40 + b] = p2v; d1[80 + b] = 0;
            }
            src  += kCockpitStride - 40;
            drow += 2 * kRowBytes;
        }
    }

    // Mode4 rows 8-87.  Char bit-7 → plane3 = 0xFF for that cell, shifting its
    // pixels to colours 4-7 (col7 = red); bit-7 clear → plane3 = 0 (cols 0-3).
    // One changed char re-decodes its 8 scanlines × 3 planes = 24 writes.
    // Same pointer-walk as modeD: per-cell hot path is `*chars++ == *shadow++`, no muls
    // (the old `entry*40+col` shadow index and `(8+entry*8+scan)*kRowBytes` dest offset
    // were per-cell muls).  dest starts at row 8 and advances 8 rows (8*120B) per entry.
    {
        const uint8_t* chars  = (const uint8_t*)mem + 0x332D + kCockpitXByteCrop;
        uint8_t*       shadow = cockpitMode4Shadow;
        uint8_t*       drow   = cdest + 8 * kRowBytes;   // mode4 begins after the 8 modeD rows
        for (int entry = 0; entry < 10; entry++) {
            for (int col = 0; col < 40; col++) {
                uint8_t ch  = *chars++;
                uint8_t old = *shadow++;               // always advance shadow (cockpitFull-safe)
                if (!cockpitFull && ch == old) continue;
                shadow[-1] = ch;
                // Mode-4 glyph index is bits 0-6 (128 glyphs); bit 7 is the
                // COLPF2/PF3 colour flag (handled via plane3).  *8 is a shift.
                uint8_t plane3 = (ch & 0x80u) ? 0xFFu : 0x00u;
                const uint8_t* glyph = (const uint8_t*)mem + 0x3800u + (ch & 0x7Fu) * 8u;
                uint8_t* p = drow + col;                 // col = add; drow already row-offset
                for (int scan = 0; scan < 8; scan++, p += kRowBytes) {
                    uint8_t p1v, p2v;
                    decode2bppByte(*glyph++, &p1v, &p2v);
                    p[0] = p1v; p[40] = p2v; p[80] = plane3;
                }
            }
            chars += kCockpitStride - 40;
            drow  += 8 * kRowBytes;
        }
    }
    g_cockpitDirty = 0;
    }
    if (rsFlight) g_flightProf.renderTot += (unsigned short)(flight_vbi_tick() - profR0);
}

void RescueOnFractalus::shutdown()
{
    for (int i = 0; i < 2; i++) { delete copperLists[i]; copperLists[i] = nullptr; }
    delete standbyCopper; standbyCopper = nullptr;
    delete planetCopper; planetCopper = nullptr;
    delete flightCopper; flightCopper = nullptr;
    delete emptyCopper;   emptyCopper   = nullptr;
    PlatformAmiga::audioShutdown();
    delete titleBitmap;   titleBitmap   = nullptr;
    delete terrainBitmap; terrainBitmap = nullptr;
    delete cockpitBitmap; cockpitBitmap = nullptr;
    delete tunnelBitmap;  tunnelBitmap  = nullptr;
    delete leftPost;      leftPost      = nullptr;
    delete rightPost;     rightPost     = nullptr;
    delete nullSprite;    nullSprite    = nullptr;
    delete gaugeSprite;   gaugeSprite   = nullptr;
    for (int c = 0; c < 3; c++) { delete starSprite[c]; starSprite[c] = nullptr; }
}
