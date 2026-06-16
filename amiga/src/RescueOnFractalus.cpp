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
#include "../framework/AmigaHardware.h"
#include "../framework/CopperList.h"
#include "../framework/Bitmap.h"
#include "../framework/Palette.h"
#include "../framework/Sprite.h"
#include "RescueOnFractalus.h"
#include "PaulaAudio.h"
#include "AtariZp.h"      // zp:: named Atari memory offsets
#include "FlightProf.h"   // per-frame VBI-count profiler (g_flightProf / flight_vbi_tick)

// Native handler functions — see NativeHandlers.cpp and SfxPlayer.cpp.
extern "C" void vbi_attract_timer_native(void);                  // $52D7: timer cascade
extern "C" void update_indicator_blink_native(void);           // $4131: cockpit blink
extern "C" void copy_text_block_to_screen_native(void);   // $782A: title text
extern "C" void update_cockpit_digits_native(void);                      // $3FFA: cockpit digit update
extern "C" void lock_on_indicator_tick_native(void);               // $4229: cockpit counter animation
extern "C" void sound_event_dispatch_native(void);              // $5367: ring ($0088) vs door scroll ($008A)
extern "C" void draw_tunnel_rings_native(void);                 // $65FB: draw concentric tunnel rings into $2000
extern "C" void audio_stop_native(void);                        // $712D: stop music + mute POKEY (START press)
extern "C" void sfx_engine_reset_native(void);                  // $5433: clean SFX voice engine for the launch
extern "C" void launch_show_standby_native(void);               // display_setup $635F: "STAND BY..." + score
extern "C" void launch_start_arpeggio_native(void);             // display_setup $63A7: START 3-blip arpeggio (id $1D)
extern "C" void launch_door_swoosh_native(void);                // display_setup $63D0: door-open swoosh (id $01)
extern "C" void launch_door_swoosh_stop_native(void);           // silence the door voice when the doors finish
extern "C" void launch_engine_voice_init(void);                 // $3DD3: engine-voice distorts + $1F/$20 cold-seeds
extern "C" void launch_engine_seed_start(void);                 // $6487: seed engine body voice (slot 12) loud
extern "C" void launch_engine_ramp_step(void);                  // $64B0: ramp engine body priority $0F->$08
extern "C" void launch_engine_steady(void);                     // $64C8: engine steady-state (slots 12/11)
extern "C" void launch_gauge_init_native(void);                 // vobj strip init ($062F/$0D98)
extern "C" uint8_t launch_gauge_step_native(void);              // one vobj fill step; 0 when full
extern "C" void launch_light_doorstart_native(void);           // $63FD: bottom-left light on
extern "C" void launch_doors_sound_native(void);               // $6235-$62E4: door-open AUDF2 sweep (blocking port)
extern "C" void launch_light_all_native(void);                 // $6482: all left lights on
extern "C" void tunnel_ring_arm_native(void);                  // $647D: reseed message-column coords
extern "C" volatile uint8_t g_tunnelFieldDirty;                // set when advance_message_column draws into $2000
extern "C" volatile uint8_t g_tunRowLo, g_tunRowHi;            // row extent of the expanding black clear
extern "C" void launch_stars_init_native(void);                // $64C8: stars setup (mode-D $1000 viewport)
extern "C" uint8_t launch_stars_step_native(void);             // $6557: one scroll step; 0 when -> planet
extern "C" uint8_t launch_planet_step_native(void);            // $6574: one planet-zoom step; 0 when done
extern "C" void    launch_planet_scroll_native(void);          // $6AEE scroll-only: keep starfield drifting
extern "C" void    flight_init_native(void);                   // game_entry $3E12-$3EB8 flight init
extern "C" void    flight_reset_parity_native(void);           // reset double-buffer pass parity
extern "C" volatile uint8_t g_activeVbi;                       // 0=none 1=standby($52D7) 2=flight($4FF5); read by game_vbi_isr
extern "C" uint8_t flight_frame_native(void);                  // one flight heavy pass; returns $0072 (==2 done)

// Real-VBI frame clock + input, used by the straight-line driver (run/frameStep).
extern "C" volatile uint16_t g_vbiCount;                       // bumped by the real INTB_VERTB ISR (main.cpp)
extern "C" bool    station_poll_start_native(void);            // station_init CONSOL ($D01F) START poll
extern "C" volatile uint8_t g_skipToFlight;                    // F-key edge: dev skip-to-flight

// The genuine transpiled launch cinematic ($5F1D, src/gen/rof_gen.c) and the blocking-
// pump gate (PaulaAudio.cpp): while rof_launch_blocking(1) is set, display_setup()'s
// frame-wait spin loops drive real frames through platform_render_frame and RTCLOK is
// owned by platform_tick_vbi (the ISR's bump is gated off).
extern "C" void display_setup(void);
extern "C" void rof_launch_blocking(uint8_t on);

// Black-until-ready reveal gate, latched on at display_setup entry (rof_native.c); read by
// animatePalette to hold the screen black until the cockpit + sprites are set up.
extern "C" volatile unsigned char g_standbyRevealReady;
// Door-field-ready gate, latched on in display_setup once the doors/dots/LEVEL field has been
// drawn into $2000 but BEFORE delay_loop_c2_to_c9 ramps the green colour $0071 (rof_native.c).
// render() decodes $2000 -> terrainBitmap once when this rises, so the door pixels exist before
// the fade and the per-frame color03 ramp shows the dark->bright green build on them.
extern "C" volatile unsigned char g_doorFieldReady;
// The genuine boot chain (src/gen/rof_gen.c): station_init = attract ($195D, returns on
// START); game_entry = $3CDE -> game_main_loop (game-display setup -> display_setup
// cinematic -> flight loop, never returns).  g_quitJmp = the __builtin_setjmp buffer
// (defined in main.cpp) the pump longjmps to on quit, unwinding the transpiled chain.
extern "C" void station_init(void);
extern "C" void game_entry(void);
extern "C" void* g_quitJmp[];   // definition (sized) lives in main.cpp

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

#include "../assets/title_pal.h"
#include "../assets/terrain_pal.h"
#include "../assets/atari_pal.h"
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

// ---- OCS colour helpers ------------------------------------------------------
static uint16_t fadeColor(uint16_t color, uint16_t fade)
{
    uint16_t r = color >> 8;
    uint16_t g = (color >> 4) & 0xFu;
    uint16_t b = color & 0xFu;
    return (uint16_t)(((fade * r >> 4) << 8) | ((fade * g >> 4) << 4) | (fade * b >> 4));
}

// ---- palette animation -------------------------------------------------------
static void animatePalette(Palette* palette, uint16_t frame)
{
    // BLACK-UNTIL-READY: on the real Atari the boot→Standby build is near-instant, so the
    // screen just appears.  On the Amiga the one-time setup (load_xex_image, scene.initialize,
    // the game_entry mega-init) spans a couple of seconds during which the screen would
    // otherwise show a piecemeal, janky build.  Hold the whole display black (fade 0 →
    // fadeColor(c,0)=$000 for every copper colour) until the cockpit + top bar are drawn and
    // the sprites are set up, then reveal at full intensity — the cockpit pops in and the
    // window build (green fill / LEVEL / fade / title) animates visibly, as on the Atari.
    //
    // Ready signal: g_standbyRevealReady, LATCHED on at display_setup entry (rof_native.c) —
    // by then game_main_loop has drawn the cockpit/top bar and scene.initialize has set up the
    // sprites.  It LATCHES (never clears) on purpose: the launch sequence re-runs display_setup
    // and transiently clears the music gate $00E7 (audio_timer_setup $712D), so gating on $00E7
    // would black the screen out again when START is pressed — the latch keeps it revealed
    // through the cinematic and flight.  (FAITHFUL: this only gates the one-time initial reveal;
    // the live mem[] colour bytes still drive every in-scene ramp at full fade.)
    (void)frame;
    palette->setFade(g_standbyRevealReady ? 16 : 0);
}

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
        for (int i = 0; i < kStarRows; i++) {
            uint8_t v = src[i];
            // Double each set player bit to 2 Amiga px: player bit b (b7 = leftmost)
            // → sprite word bits (2b+1, 2b).  All 4 star sub-positions ($80/$20/$04/
            // $01 = bits 7/5/2/0) land inside the 16 px sprite this way.
            uint16_t w = 0;
            for (int b = 0; b < 8; b++)
                if (v & (1u << b)) w |= (uint16_t)(3u << (2 * b));
            d[i * 2]     = w;        // plane A (colour bit 0 = pen 01)
            d[i * 2 + 1] = 0x0000;   // plane B
        }
    }
}

// ---- copper list builder -----------------------------------------------------
void RescueOnFractalus::buildCopperList(CopperList* cl, uint16_t frame)
{
    animatePalette(palette, frame);
    uint16_t f = palette->fade();

    uint32_t* d   = cl->data();

    // d[0] is already copperWait(16,0) from the CopperList constructor; start at 1.
    uint32_t  idx = 1;

    // ---- title region -------------------------------------------------------
    idx = cl->setPlayfield(idx, kW, kH, kBP2, /*interleaved*/true,
                           /*hires*/false, /*interlace*/false,
                           /*dualPlayfield*/false, /*holdAndModify*/false,
                           kCenterY);
    // Sprite/playfield priority: the original runs GPRIOR=$14 (players behind the
    // foreground PF2/PF3).  setPlayfield already emitted bplcon2 (=0x0024 = PF
    // behind all sprites) 10 moves before the index it returned; patch that entry
    // in place to PF priority slot 1, so the playfield sits in front of sprite
    // pair 1+ (the throttle gauge on sprite 2) but behind pair 0 (the canopy
    // posts on sprites 0/1) — the gauge ends up behind the cockpit colours.
    d[idx - 10] = copperMove(bplcon2, (uint16_t)((1u << 3) | 1u));   // PF1P=PF2P=1
    // Title palette.
    // vbi_handler_standby ($52D7) sets every frame:
    //   COLPF0 ($D016) = mem[$00D8]  — title text colour (mode-6 col=1 chars)
    //   COLBK  ($D01A) = mem[$02C8]  — title background
    //   COLPF1 ($D017) = $78         — hardcoded blue (same role on real hw)
    // copy_text_block_to_screen_native sets mem[$00D8]=$44 for the
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
        d[idx++] = copperMove(color00, fadeColor(tbg,  f));
        d[idx++] = copperMove(color01, fadeColor(tpf0, f));
        d[idx++] = copperMove(color02, fadeColor(tpf1, f));
        d[idx++] = copperMove(color03, fadeColor(tbg,  f));
    }
    cl->showBitmap(idx, *titleBitmap);
    idx += 2 * kBP2;

    // Sprite colour registers.  The canopy posts must match the top/bottom
    // background grey (COLBK = mem[$02C8] = atariToOCS($06) = 0x555), so read
    // the same source rather than a hardcoded constant.
    d[idx++] = copperMove(color16, 0x000);
    d[idx++] = copperMove(color17, fadeColor(atariToOCS(mem[0x02C8]), f));

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
    // as a vertical bar whose length ∝ flight_frame_native()'s scanline cost.
    const bool flightProbe = rsFlight;
    cl->showSprite(idx, 3, *nullSprite); idx += 2;
    cl->showSprite(idx, 4, (stars || flightProbe) ? *starSprite[0] : *nullSprite); idx += 2;
    cl->showSprite(idx, 5, stars ? *starSprite[1] : *nullSprite); idx += 2;
    cl->showSprite(idx, 6, stars ? *starSprite[2] : *nullSprite); idx += 2;
    cl->showSprite(idx, 7, *nullSprite); idx += 2;
    // Gauge bar colour (sprite pair 2/3, colour 01 = COLOR21 = $1AA).  The
    // original ramps player 1's colour through the $4DEA table as it fills,
    // ending at $D6 = #560; launch_gauge_step_native tracks that in $00DE.
    d[idx++] = copperMove(0x1AA, fadeColor(atariToOCS(mem[0x00DE]), f));
    // Star colour: COLPM (mem[$02C0]) faded in 0→$0C grey (display_setup $6555).
    // Sprites 4/5 share COLOR25 ($1B2); sprites 6/7 share COLOR29 ($1BA).
    if (flightProbe) {
        d[idx++] = copperMove(0x1B2, 0xF0F);     // COLOR25 = magenta probe bar (sprite 4)
    } else if (stars) {
        const uint16_t starCol = fadeColor(atariToOCS(mem[0x02C0]), f);
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
    const uint16_t terr0 = fadeColor(atariToOCS(mem[0x02C0]), f);
    const uint16_t terr1 = fadeColor(atariToOCS(mem[0x02C7]), f);
    const uint16_t terr2 = fadeColor(atariToOCS(mem[zp::colorRing]), f);
    const uint16_t terr3 = fadeColor(atariToOCS(mem[zp::displayFlags]), f);
    auto emitRing = [&]() {                                    // tunnel pens 1-3
        d[idx++] = copperMove(color01, fadeColor(atariToOCS(mem[zp::colorRing]), f));
        d[idx++] = copperMove(color02, fadeColor(atariToOCS(mem[zp::colorRing + 1]), f));
        d[idx++] = copperMove(color03, fadeColor(atariToOCS(mem[zp::colorRing + 2]), f));
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
            d[idx++] = copperMove(color00, fadeColor(atariToOCS(0x14), f));  // pen0 = terrain body (brown #530)
            d[idx++] = copperMove(color01, fadeColor(atariToOCS(0x2A), f));  // pen1 = sky (salmon #c76)
            d[idx++] = copperMove(color02, fadeColor(atariToOCS(0x20), f));  // pen2 = dots (dark #300)
            d[idx++] = copperMove(color03, fadeColor(atariToOCS(0x18), f));  // pen3 = terrain highlight (#962)
        } else {
            d[idx++] = copperMove(color00, fadeColor(atariToOCS(mem[0x00DC]), f));  // COLBK
            d[idx++] = copperMove(color01, fadeColor(atariToOCS(0x24), f));         // COLPF0
            d[idx++] = copperMove(color02, fadeColor(atariToOCS(0x28), f));         // COLPF1
            d[idx++] = copperMove(color03, fadeColor(atariToOCS(0x2A), f));         // COLPF2
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
    if (door) {                                                // ring upper half (pens 4-6),
        d[idx++] = copperMove(color04, fadeColor(atariToOCS(mem[zp::colorRing + 3]), f));  // unused by terrain
        d[idx++] = copperMove(color05, fadeColor(atariToOCS(mem[zp::colorRing + 4]), f));
        d[idx++] = copperMove(color06, fadeColor(atariToOCS(mem[zp::colorRing + 5]), f));
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
    d[idx++] = copperMove(color00, fadeColor(atariToOCS(0x00), f));
    d[idx++] = copperMove(color01, fadeColor(atariToOCS(0x04), f));
    d[idx++] = copperMove(color02, fadeColor(atariToOCS(0x06), f));
    d[idx++] = copperMove(color03, fadeColor(atariToOCS(0x2C), f));
    d[idx++] = copperMove(color04, fadeColor(atariToOCS(0x00), f));  // = col0 (black)
    d[idx++] = copperMove(color05, fadeColor(atariToOCS(0x04), f));  // = col1 (dark gray)
    d[idx++] = copperMove(color06, fadeColor(atariToOCS(0x06), f));  // = col2 (gray)
    d[idx++] = copperMove(color07, fadeColor(atariToOCS(0x26), f));  // red (COLPF3)

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
    Palette::initialize();
    palette       = new Palette(kTitlePalette, 4, /*fade*/0);
    titleBitmap   = Bitmap::allocate(kW, kTitleHeight,   kBP2, true);
    terrainBitmap = Bitmap::allocate(kW, kTerrainHeight, kBP3, true);  // 3bp: tunnel reveal uses pens 4-7
    cockpitBitmap = Bitmap::allocate(kW, kCockpitH, kBP3, true);  // 3bp: bit-7 chars → red
    tunnelBitmap  = Bitmap::allocate(kW, kTerrainHeight, kBP3, true);  // door-gap reveal

    // (Tunnel rings are decoded in decodeTunnelRings(), called after the first
    // render() below captures the closed-door terrain — see initialize() tail.)

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

    deriveRenderSignals();   // seed the render signals from the initial mem[] (standby) state
    buildCopperList(copperLists[0], 0);
    buildCopperList(copperLists[1], 0);
    active = 0;
    AmigaHardware::setCopperList(*copperLists[active], true);

    // Static-Standby fixed copper list: built once here (bitmaps + sprites now exist),
    // its dynamic colour/sprite slots refreshed each frame by updateStandbyCopper.
    // pumpFrame installs it once the doors are decoded and the scene settles into
    // Standby; until then the double-buffered buildCopperList path drives the build.
    standbyCopper = new StandbyCopperList();
    if (standbyCopper && standbyCopper->data())
        standbyCopper->buildLayout(*titleBitmap, *terrainBitmap, *cockpitBitmap,
                                   *leftPost, *rightPost, *nullSprite);

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

    paula_audio_init();      // loads screen3_mem.bin into mem[] (Standby scene snapshot)

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

    // Seed $0091=$C0 so copy_text_block_to_screen_native fires on the first
    // update() call and writes Block1 ("rescue on fractalus") to $32B7.
    // On the real Atari, $0091 is set by the SFX sequencer; we prime it once here
    // so the title is correct before the first SFX tick produces a $C0 byte.
    mem[zp::altitudeThreshold] = 0xC0;

    // Initial render: populate all three bitmaps from mem[] once so that
    // render() called from the main loop has nothing to do until data changes.
    // This captures the closed-door terrain image from $2000 into terrainBitmap.
    render();

    // Now draw the tunnel rings procedurally (native port of $65FB) INTO $2000 —
    // this overwrites the door image, which terrainBitmap has already captured —
    // and decode that GTIA-10 field into the 3bp tunnel bitmap (pens 1-6).  Motion
    // later comes from cycling COLOR01-06; the ring pattern itself is static.
    decodeTunnelRings();

    // Init complete — release the ISR's "scene ready" gate (g_activeVbi != 0).  The ISR
    // dispatches on the live VVBLKI vector ($0222/$0223), so seed it to $52D7 (standby)
    // for the brief window before run()->station_init installs the $1B30 attract vector;
    // otherwise the snapshot's stale VVBLKI could pick the wrong body for a few frames.
    mem[0x0222] = 0xD7u;   // VVBLKI lo
    mem[0x0223] = 0x52u;   // VVBLKI hi  -> $52D7 standby VBI
    g_activeVbi = 1;
}

void RescueOnFractalus::decodeTunnelRings()
{
    if (!tunnelBitmap) return;
    draw_tunnel_rings_native();   // $65FB: render concentric frames into mem[$2000]
    decodeTunnelField(0, (int)kTerrainHeight - 1);
}

// decodeTunnelField: decode rows [rowLo..rowHi] of the GTIA-10 field at mem[$2000]
// into the tunnel bitmap, PER-BYTE shadow-gated so only the bytes that actually
// changed are re-decoded.  The exit clear draws a thin black frame OUTLINE each step
// (horizontal edges full-width + left/right VERTICAL pieces down the inner rows), so
// even passing the full new row extent [botAfter..topAfter] here touches only the
// outline bytes — covering the vertical pieces a horizontal-only band would miss,
// while staying far under one PAL frame (GTIA-10 byte→plane decode via tables).
void RescueOnFractalus::decodeTunnelField(int rowLo, int rowHi)
{
    if (!tunnelBitmap) return;
    if (rowLo < 0) rowLo = 0;
    if (rowHi > (int)kTerrainHeight - 1) rowHi = (int)kTerrainHeight - 1;
    uint8_t* bm = (uint8_t*)tunnelBitmap->data;
    for (int row = rowLo; row <= rowHi; row++) {
        const uint8_t* src = (const uint8_t*)&mem[0x2000 + row * 46 + 4];  // +4: wide-field crop
        uint8_t* p1 = bm + row * 120; uint8_t* p2 = p1 + 40; uint8_t* p3 = p1 + 80;
        uint8_t* shadow = &tunnelShadow[row * 40];
        for (int b = 0; b < 40; b++) {
            uint8_t s = src[b];
            if (s == shadow[b]) continue;          // unchanged byte — skip
            shadow[b] = s;
            p1[b] = kGtia10P1[s]; p2[b] = kGtia10P2[s]; p3[b] = kGtia10P3[s];
        }
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
// the displayed 40 of 48 either way; the per-byte viewportShadow is shared (the
// stride/base change between phases re-fills it via viewportForceFull).
void RescueOnFractalus::renderViewportModeD(uint16_t srcBase, int stride, int rows)
{
    if (!terrainBitmap) return;
    static const int kCrop   = 4;    // central 40 of 48 (centres content)
    const bool full = viewportForceFull;
    viewportForceFull = false;
    uint8_t* base = (uint8_t*)terrainBitmap->data;

    // Write each mode-D row to ONE interleaved scanline; the copper line-doubles the
    // region vertically (buildCopperList's viewport band toggles the bitplane modulo
    // -40/+80 per scanline, re-displaying each row twice).  This halves the per-byte
    // store work here vs the old CPU doubling (which wrote both scanlines).  p1 tracks
    // plane1; plane2/plane3 at +40/+80; per row p1 walks 40 then += 80 (= one 120-byte
    // interleaved scanline).  ALL pointers advance in the for-increment — even on the
    // shadow-skip `continue` — so they stay column-aligned.  Per-byte shadow: the
    // planet sphere / scrolling terrain only changes some bytes, so re-decode just those.
    const uint8_t* src = (const uint8_t*)&mem[srcBase + kCrop];
    uint8_t* p1  = base;
    uint8_t* shp = viewportShadow;                            // walking shadow pointer
    for (int row = 0; row < rows; row++, src += stride) {
        const uint8_t* rowSrc = src;
        for (int b = 0; b < 40; b++, rowSrc++, shp++, p1++) {
            uint8_t s = *rowSrc;
            if (!full && s == *shp) continue;
            *shp = s;
            p1[0] = kModeDP1[s]; p1[40] = kModeDP2[s]; p1[80] = 0;   // one scanline; copper repeats it
        }
        p1 += 80;                                             // 40 walked -> 120 = one interleaved scanline
    }
}

void RescueOnFractalus::openDoors()
{
    if (launchPhase != kLaunchNone) return;   // launch already begun

    // The instant START is pressed the Atari launch path calls audio_timer_setup
    // ($712D) to STOP the attract music (display_setup $6337/$634F): the Standby
    // SFX-sequencer melody must not carry into the launch cinematic or flight.
    // (Faithful fix for the "music keeps playing in flight" bug.)
    audio_stop_native();
    // Launch begins: reset the SFX voice engine to a clean state (Atari display_setup
    // $6118 -> $5433) so the standby VBI's update_gauge_digits can play the START/
    // doors/tunnel launch effects from a known-good baseline.
    sfx_engine_reset_native();

    // NOTE: `launched` is set in startDoors, NOT here — during the gauge phase the
    // viewport must still show the FULLY-CLOSED doors.  buildCopperList derives the
    // door gap g2 from `launched` (= $2B - $008A); leaving it false keeps g2 = 0
    // (closed) until the door scroll actually starts.

    // Cinematic effect 1: switch the message line from the attract title scroll
    // to "STAND BY..." + score (display_setup $635F, genuine 6502 routines).
    launch_show_standby_native();

    // Launch SFX: seed the START 3-blip arpeggio ($63A7, id $1D) the instant START is
    // pressed.  Plays via the standby/launch VBI's update_gauge_digits voice engine;
    // must follow sfx_engine_reset_native + launch_show_standby_native ($060B=$23).
    // The door swoosh is seeded later (startDoors) so it doesn't mask the arpeggio.
    launch_start_arpeggio_native();

    // Cinematic effect 2: BEFORE the doors open, fill the throttle gauge (the
    // vobj player-1 strip).  Enter the gauge phase; startDoors() runs only once
    // it completes — matching display_setup's order ($63FF gauge, then $641E doors).
    launchPhase = kLaunchGauge;
    launch_gauge_init_native();
}

// skipToFlight: dev shortcut (F key) — jump straight to the in-game flight stage
// from anywhere in the standby/cinematic.  startFlight() runs the genuine flight
// init (game_entry $3E12-$3EB8), which re-initialises the game state, so no
// cinematic state is required first.  No-op once already flying.
void RescueOnFractalus::skipToFlight()
{
    if (launchPhase == kFlight) return;
    launched = true;          // mark the cinematic as begun (stops the attract title toggle)
    audio_stop_native();      // F-skip bypasses openDoors() — still stop the attract music
    startFlight();
}

// startDoors: the door-scroll launch state (display_setup $63DC-$63FB), run once
// the gauge has filled.  Drives the existing native scroll_terrain_dl dispatcher.
void RescueOnFractalus::startDoors()
{
    launchPhase = kLaunchDoors;
    launched    = true;   // doors now scrolling: buildCopperList tracks the gap via $008A

    // Cinematic effect 3: as the doors start, light the bottom-left indicator
    // (display_setup $63FD: draw_cockpit_dial_bar with A=7 -> dial-bar threshold $0F).
    launch_light_doorstart_native();

    // Door-open swoosh ($63D0, id $01): seed it now (as the doors begin scrolling)
    // rather than at START, so it doesn't mask the START arpeggio.  Silenced when the
    // doors finish (kLaunchDoors -> kLaunchTunnel below), since id $01 never expires.
    launch_door_swoosh_native();

    // Set the launch state the way display_setup ($5F1D) does, then let the $5367
    // dispatcher drive it.  The doors open FIRST (ring gate $0088 = 0, so the tunnel
    // is static): scroll_terrain_dl decrements $008A from $2B to 0 over its run.
    // Only when the doors are fully open does update() arm $0088 so the ring starts
    // cycling — matching the hangar→launch sequence.
    mem[zp::terrainScrollCounter] = 0x2Bu;   // $008A: 43 door-scroll steps
    mem[zp::terrainScrollReload]  = 0x00u;   // $008C
    mem[zp::terrainScrollPhase]   = 0x00u;   // $008F (every-other-frame toggle)
    mem[zp::vbiFlags]             = 0x00u;   // $0088: ring OFF until doors finish
    mem[zp::stepModeFlag]         = 0x00u;   // $008D: forward ring
    mem[zp::scrollColumnsGate]    = 0x00u;   // $0089
    mem[zp::dlIndexGate]          = 0x00u;   // $008B

    // DL push pointers + edge indices, per display_setup ($63E1-$63F7).
    mem[0x0080] = 0x8Cu; mem[0x0081] = 0x17u;  // top push pointer    = $178C
    mem[0x0082] = 0xBAu; mem[0x0083] = 0x17u;  // bottom push pointer = $17BA
    mem[0x0097] = 0x7Fu;                       // top LMS edge index
    mem[0x0098] = 0x02u;                       // bottom LMS edge index

    // Tunnel ring colours (mem[$08D4-$08D9], feeding COLOR01-06): seed the blue
    // ramp once, statically — they sit still until $0088 is armed, then rotate.
    // Accumulator $A1-$A5 zeroed (literal-$75 / zero-seed fidelity choice).
    static const uint8_t kRingRamp[6] = { 0x30, 0x32, 0x34, 0x36, 0x38, 0x3A };
    for (int i = 0; i < 6; i++) mem[zp::colorRing + (uint16_t)i] = kRingRamp[i];
    mem[zp::scrollAccum0] = mem[zp::scrollAccum1] = mem[zp::scrollAccum2] =
        mem[zp::scrollAccum3] = mem[zp::scrollAccumPrev] = 0u;

    // NOTE: the door/launch sound is NOT the audf2_sweep_clear_colors ($622D) colour
    // sweep — that was the wrong component.  The real launch audio is a multi-engine,
    // event-driven sound (SFX sequencer sfx_seq_step $7148 + noise voice sfx_engine_step
    // $5553 + engine_sound_update $5978), triggered off the launch lights/throttle.
    // launch_doors_sound_native() is left in place but NOT called pending that rework.
}

// startStars: the stars/space setup (display_setup $64C8-$6552), run once the
// tunnel ring auto-clears $0088.  Switches the viewport to ANTIC mode-D from
// $1000 (rendered by renderViewportModeD); the per-frame scroll is driven by
// launch_stars_step_native from update().
void RescueOnFractalus::startStars()
{
    launchPhase   = kLaunchStars;
    launch_stars_init_native();   // $64C8-$6552 setup + $1000 row-addr table (clears $1000)
    for (int i = 0; i < 4; i++) mem[0x02C0 + (uint16_t)i] = 0u;  // $6555: COLPM fade starts at 0
    viewportActive    = true;     // viewport now decodes mem[$1000] as mode-D 2bpp
    viewportForceFull = true;     // first decode must clear the stale door image (the
                                  // shadow would otherwise match the freshly-cleared $1000)
    // Decode the (now-cleared) $1000 into terrainBitmap NOW, before update() rebuilds
    // the copper to point the viewport at it.  Otherwise the copper would display the
    // stale closed-door image (terrainBitmap is untouched all through doors/tunnel,
    // which show tunnelBitmap) for a frame in the stars palette.  The live display is
    // still showing tunnelBitmap here, so this rewrite of terrainBitmap is invisible
    // until the switch takes effect next vblank — no mid-screen tearing.
    renderViewportModeD(0x1000, 48, 43);
}

// startPlanet: the planet setup (display_setup $6555-$6574).  The stars setup
// already seeded the object table and the $1000 row-addr table; here we clear the
// player-colour shadows ($6557 loop) and reset the frame gate, then the planet
// loop (launch_planet_step_native) zooms the sphere into $1000 each step.
void RescueOnFractalus::startPlanet()
{
    launchPhase = kLaunchPlanet;
    // (The $6555-$6567 COLPM ramp belongs to the stars setup — it fades the star
    // players in to $0C, not a planet-start clear; we leave $02C0-3 at $0C here so
    // the starfield stays lit behind the zooming planet, matching the Atari.)
    mem[0x0014] = 0u;                                              // $6574 frame gate
}

// startFlight: hand off from the launch cinematic to the in-game flight loop.
// On the Atari, display_setup ($5F1D) RTSes into game_entry, which runs the
// flight init ($3E12-$3EB8) then loops at $3EBA.  flight_init_native ports the
// mem[]-state subset (and repoints the terrain row-addr table from the stars'
// $1000/stride-48 to flight's $1010/stride-96).  The viewport stays active and
// is force-redecoded once (stride/base changed → the per-byte shadow is stale).
void RescueOnFractalus::startFlight()
{
    flight_reset_parity_native();
    flight_prof_reset();          // zero the per-frame profiler counters for this flight
    flight_init_native();         // game_entry $3E12-$3EB8 (mem[] subset; HW writes skipped)
    launchPhase       = kFlight;
    viewportActive    = true;
    viewportForceFull = true;     // re-decode the whole viewport (now $1070 stride 96)
    // Flight top bar: clear the stale Standby banner from the whole wide (24-char)
    // mode-6 line $32B5..$32CC and force a full title re-render; the per-frame
    // render_bcd_counter then draws the score on the right (visible at $32CA).
    for (uint16_t i = 0x32B5; i <= 0x32CC; i++) mem[i] = 0x00;
    for (int i = 0; i < 20; i++) titleShadow[i] = 0xFF;
    // Swap the active VBI body to flight LAST — only now (init complete) may the real
    // INTB_VERTB ISR (game_vbi_isr) start running the flight VBI ($4FF5) instead of
    // the standby/launch VBI ($52D7).  Mirrors the Atari swapping VVBLKI at $3E50.
    g_activeVbi = 2;
}

// run(): the whole game as a faithful straight-line transcription of the Atari
// control flow.  The Atari ran station_init -> display_setup -> game_entry as
// straight-line code that busy-waits between phases while its VBI/DLI interrupts
// animate the screen; this mirrors that exactly — each original wait point becomes
// a real frame-wait (cinematicFrame/frameStep) backed by the INTB_VERTB VBI ISR.
// No per-frame launchPhase dispatch: the call FLOW drives progression, the phase
// field is only a render-mode tag set by the startX() helpers below.
void RescueOnFractalus::run()
{
#ifdef ROF_AUTOFLIGHT
    // Dev/profiling build (-DROF_AUTOFLIGHT): jump straight to flight, as the old
    // loop's startup did, then run game_entry's flight loop.
    skipToFlight();
    flightLoop();
    return;
#endif

    // ---- the genuine transpiled boot chain drives EVERYTHING --------------------
    // mem[] is the pristine rof.xex image (load_xex_image in paula_audio_init): every
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
    // (platform_render_frame -> pumpFrame); the VBI body follows the live VVBLKI vector
    // game_entry installs (game_vbi_isr dispatches $52D7/$4FF5 automatically).
    if (__builtin_setjmp(g_quitJmp) != 0) return;   // quit: unwound here from the pump/poll hook

    rof_launch_blocking(1);
    game_entry();     // $3CDE: mega-init -> game_main_loop (Standby -> cinematic -> flight); never returns
}

// flightLoop(): game_entry's per-frame flight loop ($3EBA).  The flight motion VBI
// ($4FF5) runs in the real INTB_VERTB ISR (frameStep's wait lets one fire each
// frame); flight_frame_native is the heavy main-loop pass (the Atari runs two passes
// per iteration for double-buffering — one Amiga frame = one pass).
void RescueOnFractalus::flightLoop()
{
    for (;;) {
        flight_frame_native();
        if (frameStep() == kFrameQuit) return;
    }
}

// cinematicFrame(): one cinematic frame-wait.  Returns true if the cinematic should
// abort — the user quit, or the F-key skip fired (handed off to flightLoop already).
bool RescueOnFractalus::cinematicFrame()
{
    switch (frameStep()) {
        case kFrameQuit: return true;
        case kFrameSkip: flightLoop(); return true;
        default:         return false;
    }
}

// frameStep(): the REAL per-frame busy-wait that backs every wait point in the
// original straight-line code.  Spin until the next INTB_VERTB VBI (the ISR ran the
// Atari per-frame VBI body for this phase + bumped RTCLOK $0014 once), then do the
// per-frame non-phase work and repaint on the main thread.  This is the matching
// Amiga construct for the Atari frame-wait loops (wait_frames_*): the real vertical-
// blank interrupt is the frame clock, exactly as it was on the Atari.
RescueOnFractalus::FrameResult RescueOnFractalus::frameStep()
{
    unsigned short profU0 = flight_vbi_tick();   // whole-frame timer (flight only)

    uint16_t last = g_vbiCount;
    while (g_vbiCount == last) { /* wait one real VBI */ }

    if (AmigaHardware::isLeftMouseButtonPressed()) return kFrameQuit;

    // F-key dev skip (keyboard ISR edge): hand off to flight from anywhere in the
    // standby/cinematic.  Only meaningful before flight; in flight it's ignored.
    if (g_skipToFlight && launchPhase != kFlight) {
        g_skipToFlight = 0;
        skipToFlight();
        return kFrameSkip;
    }

    pumpFrame();   // the shared repaint body (also used by the transpiled frame pump)
    if (rsFlight) g_flightProf.updateTot += (unsigned short)(flight_vbi_tick() - profU0);
    return kFrameContinue;
}

// pumpFrame(): the per-frame repaint body shared by frameStep() and the transpiled
// frame pump (launchFramePump in main.cpp).  The caller has already waited one real
// VBI; this does the non-phase per-frame work, repaints the bitmaps, rebuilds the back
// copper list and flips to it — one implementation so both drivers match exactly.
void RescueOnFractalus::pumpFrame()
{
    frameCounter++;
    deriveRenderSignals();   // recompute the mem[]-derived render-gating signals for this frame
    // Tunnel reveal: the $52D7 VBI's advance_message_column draws the expanding black
    // clear into mem[$2000] and flags g_tunnelFieldDirty with its row extent; re-decode
    // those rows into tunnelBitmap here (was in run()'s tunnel loop, now that the
    // transpiled display_setup drives the cinematic).
    if (g_tunnelFieldDirty) { decodeTunnelField((int)g_tunRowLo, (int)g_tunRowHi); g_tunnelFieldDirty = 0; }
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
            updateStandbyCopper(true);   // seed every dynamic slot from current mem[]
            AmigaHardware::setCopperList(*standbyCopper, false);
            standbyCopperInstalled = true;
        } else {
            updateStandbyCopper(false);  // poke only the slots whose value changed
        }
        return;
    }

    // Dynamic phases (door-open cinematic, stars, planet, flight): the layout varies
    // per frame, so keep the double-buffered full rebuild + flip.
    uint8_t next = 1 - active;
    unsigned short c0 = flight_vbi_tick();
    buildCopperList(copperLists[next], frameCounter);
    if (rsFlight) g_flightProf.copper += (unsigned short)(flight_vbi_tick() - c0);
    AmigaHardware::setCopperList(*copperLists[next], false);
    active = next;
    standbyCopperInstalled = false;   // left Standby — next static entry re-seeds + re-installs
}

// updateStandbyCopper(): refresh the StandbyCopperList's per-frame-varying colour and
// sprite slots from mem[].  The global fade is 16 throughout this list's life (it's
// only used once g_doorFieldReady is latched, which is AFTER g_standbyRevealReady), so
// fadeColor is the identity and the OCS colour is just atariToOCS(byte).  Each slot is
// poked only when its value changed since last frame (force = poke all, on install).
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

    // Re-arm the one-time Standby door capture (render() does it once when the doors
    // are built, gated on $00E7!=0) whenever the scene is NOT a settled Standby —
    // music off (building / not yet there), launched, or a viewport scene.  So each
    // fresh entry into Standby re-decodes the doors exactly once and then idles.
    if (g_doorFieldReady == 0u || rsLaunched || rsViewport) terrainDirty = true;
}

// perFrameWork(): per-frame non-phase work (the tail of the old update()).  These
// ran every frame regardless of cinematic phase, driven by the standby/flight VBI
// body + the main loop on the Atari; here they run once per frameStep.
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

    // Title text ("RESCUE ON FRACTALUS!" / copyright) — the Standby attract banner;
    // don't draw it over the flight cockpit/viewport ($62FB, gated by $060B).
    if (!rsFlight && mem[0x060B] == 0)
        copy_text_block_to_screen_native();    // $782A: $0091 → title string

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

    uint8_t* tbmp = (uint8_t*)titleBitmap->data;
    for (int col = 0; col < 20; col++) {
        uint8_t charByte = mem[kScreenRAM + (uint16_t)col];
        if (charByte == titleShadow[col]) continue;   // unchanged — skip
        titleShadow[col] = charByte;

        // Re-render this char: mode-6 is 1bpp, but the byte's top 2 bits select
        // the text colour register.  We support the two cases that occur here:
        //   hi2=0 → COLPF0 → col1 (plane1)   — copyright block
        //   hi2=1 → COLPF1 → col2 (plane2)   — RESCUE ON FRACTALUS! (blue)
        // "off" pixels stay col0 (COLBK).  Each glyph bit is doubled to 2 px.
        uint8_t charIdx    = charByte & 0x3Fu;
        bool    usePF1     = ((charByte >> 6) & 3u) == 1u;   // hi2=1 → COLPF1/blue
        uint16_t glyphBase = kCharsetBase + charIdx * 8u;
        for (int scanline = 0; scanline < 8; scanline++) {
            int destRow = kTitleTextRow + scanline;
            if (destRow >= (int)kTitleHeight) break;
            uint8_t glyph = mem[glyphBase + (uint16_t)scanline];
            uint16_t doubled = kDoubleGlyph[glyph];
            uint8_t hb = (uint8_t)(doubled >> 8);
            uint8_t lb = (uint8_t)(doubled & 0xFF);
            uint8_t* row = tbmp + destRow * 80;
            // plane1 carries COLPF0 chars, plane2 carries COLPF1 chars.
            row[col*2]       = usePF1 ? 0 : hb;
            row[col*2+1]     = usePF1 ? 0 : lb;
            row[40+col*2]    = usePF1 ? hb : 0;
            row[40+col*2+1]  = usePF1 ? lb : 0;
        }
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

    // ModeD rows 0-7 (raw bitmap, no bit-7 colour swap → plane3 = 0).  Each
    // source byte feeds the same column of 2 identical scan lines.
    for (int entry = 0; entry < 4; entry++) {
        const uint8_t* src = (const uint8_t*)&mem[0x350D + (uint16_t)(entry * kCockpitStride + kCockpitXByteCrop)];
        for (int b = 0; b < 40; b++) {
            uint8_t s = src[b];
            int sh = entry * 40 + b;
            if (!cockpitFull && s == cockpitModeDShadow[sh]) continue;
            cockpitModeDShadow[sh] = s;
            uint8_t p1v, p2v;
            decode2bppByte(s, &p1v, &p2v);
            for (int scan = 0; scan < 2; scan++) {
                uint8_t* p1 = cdest + (entry * 2 + scan) * kRowBytes;
                p1[b] = p1v; p1[40 + b] = p2v; p1[80 + b] = 0;
            }
        }
    }

    // Mode4 rows 8-87.  Char bit-7 → plane3 = 0xFF for that cell, shifting its
    // pixels to colours 4-7 (col7 = red); bit-7 clear → plane3 = 0 (cols 0-3).
    // One changed char re-decodes its 8 scanlines × 3 planes = 24 writes.
    for (int entry = 0; entry < 10; entry++) {
        const uint8_t* chars = (const uint8_t*)&mem[0x332D + (uint16_t)(entry * kCockpitStride + kCockpitXByteCrop)];
        for (int col = 0; col < 40; col++) {
            uint8_t ch = chars[col];
            int sh = entry * 40 + col;
            if (!cockpitFull && ch == cockpitMode4Shadow[sh]) continue;
            cockpitMode4Shadow[sh] = ch;
            // Mode-4 glyph index is bits 0-6 (128 glyphs); bit 7 is the
            // COLPF2/PF3 colour flag (handled via plane3).
            uint8_t plane3 = (ch & 0x80u) ? 0xFFu : 0x00u;
            for (int scan = 0; scan < 8; scan++) {
                uint8_t* p1 = cdest + (8 + entry * 8 + scan) * kRowBytes;
                uint8_t glyphData = mem[0x3800u + (ch & 0x7Fu) * 8u + (uint16_t)scan];
                uint8_t p1v, p2v;
                decode2bppByte(glyphData, &p1v, &p2v);
                p1[col] = p1v; p1[40 + col] = p2v; p1[80 + col] = plane3;
            }
        }
    }
    if (rsFlight) g_flightProf.renderTot += (unsigned short)(flight_vbi_tick() - profR0);
}

void RescueOnFractalus::shutdown()
{
    for (int i = 0; i < 2; i++) { delete copperLists[i]; copperLists[i] = nullptr; }
    delete standbyCopper; standbyCopper = nullptr;
    paula_audio_shutdown();
    delete titleBitmap;   titleBitmap   = nullptr;
    delete terrainBitmap; terrainBitmap = nullptr;
    delete cockpitBitmap; cockpitBitmap = nullptr;
    delete tunnelBitmap;  tunnelBitmap  = nullptr;
    delete palette;       palette       = nullptr;
    delete leftPost;      leftPost      = nullptr;
    delete rightPost;     rightPost     = nullptr;
    delete nullSprite;    nullSprite    = nullptr;
    delete gaugeSprite;   gaugeSprite   = nullptr;
    for (int c = 0; c < 3; c++) { delete starSprite[c]; starSprite[c] = nullptr; }
}
