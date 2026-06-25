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
extern "C" void startup_init_native(void);                      // $3FFA: cockpit digit update
extern "C" void lock_on_indicator_tick_native(void);               // $4229: cockpit counter animation
extern "C" void launch_anim_dispatch_native(void);              // $5367: ring ($0088) vs door scroll ($008A)
extern "C" volatile uint8_t g_tunnelFieldDirty;                // set when draw_ring_frame_step draws into $2000
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
// genuine $782A title writer (copy_title_text_block_to_screen) via the platform_title_
// changed() hook; g_cockpitDirty by update_cockpit_digits / lock_on_indicator_tick at their
// store sites.  Both are force-set at phase transitions in deriveRenderSignals() so the
// initial build (by the transpiled display_setup, not those writers) + flight updates are
// never missed.
extern "C" volatile unsigned char g_titleDirty   = 1;

// ---- cockpit dirty-cell registry --------------------------------------------
// The cockpit ($332D mode4 / $350D modeD) is decoded WRITER-DRIVEN: each instrument writer
// (digits startup_init, lock-on, dials draw_object_column) calls rof_cockpit_dirty(addr,n)
// with the exact cell span it just changed, and render() decodes only those cells — no
// per-frame full scan / shadow compare.  Writers run on BOTH the main thread (digits, in
// perFrameWork) AND the VBI ISR (lock-on, dials), so the registry must be lock-free: it is a
// per-cell dirty-flag array indexed by (addr - $332D), spanning $332D..$355D (mode4 + modeD).
// Single-byte stores are atomic on the 68000, so producers just set their cells' flags with no
// Disable() (calling Disable()/Enable() from the VBI ISR wedged interrupt delivery — incl. the
// keyboard ISR that starts the game).  render() clears-then-decodes each set flag; a write that
// races the clear simply re-flags for the next frame — never a missed change, never a lost span.
static const int CK_FLAGS = 0x355D - 0x332D;   // 560 cells: mode4 $332D..$350C + modeD $350D..$355C
static volatile unsigned char g_ckFlag[CK_FLAGS] __attribute__((aligned(4))) = {};

extern "C" void rof_cockpit_dirty(unsigned short addr, unsigned char n)
{
    if (addr < 0x332Du) return;
    unsigned short off = (unsigned short)(addr - 0x332Du);
    for (unsigned char j = 0; j < n; j++) {
        unsigned short o = (unsigned short)(off + j);
        if (o < (unsigned short)CK_FLAGS) g_ckFlag[o] = 1u;
    }
}
#ifdef ROF_FLIGHT_PROBE
extern "C" unsigned long rof_subclock(void);
extern "C" volatile unsigned long g_fConvert, g_isrBeamLines;  // Stage-0 convert-pass probe
extern "C" volatile unsigned long g_fCockpit, g_fCockpitScans;
#endif
// Compass (#2): the heading cells $32E3-$32E6 (mode-4 line below the title) — flagged by
// platform_compass_changed() from the housing init (game_sub_4606) / heading updater ($3FDE).
extern "C" volatile unsigned char g_compassDirty = 1;
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
// GTIA mode-10 nibble → Amiga pen for the Standby/Doors door field.  In these scenes only
// three nibble values occur: 0 (road dots → COLPM0 → dark), 7 ("LEVEL 04" text → COLPF3) and
// 8 (background → COLBK → green).  COLBK (value 8) maps to pen0 (Amiga color00) so that the
// green continues unbroken from the viewport into the windscreen band and the dashboard
// splits below — mirroring the Atari's single COLBK register, which stays green from the
// viewport top straight through the band (measured: COLBK=$C8 y50-136, →$00 at y138).  The
// dark road dots (value 0) move to pen3 instead, and nibble 7 stays on pen1 (it MUST differ
// from the green or the level text baked as COLPF3 pixels vanishes).  Used only to build
// kDoorP1/kDoorP2 in initialize().
static const uint8_t kNibbleColour[16] = {
    3,                   // 0   → COLPM0 → pen3 (road dots / dark seams)
    0, 0, 0,             // 1-3 → bg (COLBK green → pen0)
    0, 0, 0,             // 4-6 → bg
    1,                   // 7   → COLPF3 → pen1 ("LEVEL 04" text)
    0,                   // 8   → COLBK  → pen0 (green background = color00)
    0, 0, 0, 0, 0, 0, 0  // 9-15 → bg
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
// Flight/planet ($316B mode-D DL) display 47 mode-D rows, not 43: the bottom 4 rows
// ($2090-$21B0 / $1810-$18A0) are the wing-clearance band (windscreen-bottom frame +
// the salmon clearance bars).  terrainBitmap must hold all 47 rows × 2 scanlines = 94.
// The other scenes (standby/doors/tunnel) display only the first 86 — the extra rows
// are allocated but unused there.
static const uint16_t kViewportFullHeight = 47 * 2;                 // = 94
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

// ---- canopy posts (cockpit window A-pillars) ---------------------------------
// The posts are the real Atari players P0 (left) / P1 (right).  The genuine game decodes
// them with unpack_terrain_seed_cols ($7558), RLE-expanding the static source tables
// $4DFA (left) / $4E09 (right) into the player buffers $0C32/$0D32 — but only at gameplay
// init, so the LIVE buffers don't hold the frame pre-flight (and are repurposed as the
// station/stars starfield).  The post graphic itself is constant, so we decode it ONCE
// straight from the same source tables here, independent of phase, and never rebuild —
// faithful (real game data) and replacing the old hand-approximated 0xF000>>shift slant.
//
// RLE format (rle_expand_list $757B): (count, value) byte pairs, 0x00 terminator.  Each
// Atari player byte → 2 Amiga lores px via the kDoubleGlyph LUT (as the starfield maps).
static void decodePostRLE(const uint8_t* tbl, uint16_t* dst)
{
    int row = 0;
    while (row < kHT) {
        uint8_t count = *tbl++;
        if (count == 0) break;                 // terminator
        uint16_t doubled = kDoubleGlyph[*tbl++];
        for (uint8_t k = 0; k < count && row < kHT; k++, row++) {
            dst[row * 2] = doubled; dst[row * 2 + 1] = 0x0000;
        }
    }
    for (; row < kHT; row++) { dst[row * 2] = 0x0000; dst[row * 2 + 1] = 0x0000; }
}

void RescueOnFractalus::buildPostSprites()
{
    decodePostRLE((const uint8_t*)&mem[0x4DFA], leftPost->data()  + 2);   // P0 left
    decodePostRLE((const uint8_t*)&mem[0x4E09], rightPost->data() + 2);   // P1 right
}

// Expand one Atari band-wedge player byte to a 32px field at DOUBLE width (band players
// are SIZEP=$01 -> each set bit = 4 Amiga lores px).  Player bit 7 = leftmost 4px (field
// MSBs), bit 0 = rightmost.  The field spans the two 16px sprites of one side: bits 31..16
// = the left sprite, bits 15..0 = the right sprite.
static inline uint32_t expandWedge32(uint8_t byte)
{
    uint32_t f = 0;
    for (int b = 0; b < 8; b++)
        if (byte & (1u << b)) f |= (uint32_t)0xFu << (4 * b);
    return f;
}

// buildFlightFrameSprites: the flight (scene 7) windscreen frame = A-pillars + the
// band-bottom windscreen-corner triangles.  On the Atari the band triangle is PMG
// (players P0/P1, repositioned to HPOSP0=$30/HPOSP1=$C0 and double-width by the band
// DLI $4A40) — a separate element from the Planet bitmap frame.  A-pillars (sprite rows
// 0-85) come from the RLE tables $4DFA/$4E09 (as the shared posts); the triangle's 8 band
// scanlines come from the static source rows $4DD2 (left) / $4DDA (right): 8 player bytes
// each, gray right-justified for the left wedge ($01,$07,$1f,$7f -> 4/12/20/28px down) and
// left-justified for the right ($80,$e0,$f8,$fe).  Double-width => ~28px max => two 16px
// sprites per side: the post sprite carries the inner 16px (toward screen centre), the
// triangle sprite the outer 16px.  Static frame, built once.
void RescueOnFractalus::buildFlightFrameSprites()
{
    // A-pillars into the post sprites (rows 0-85); band rows 86-93 left zero by allocate.
    decodePostRLE((const uint8_t*)&mem[0x4DFA], flLeftPost->data()  + 2);
    decodePostRLE((const uint8_t*)&mem[0x4E09], flRightPost->data() + 2);

    uint16_t* lp = flLeftPost->data()  + 2;   // ch0: left wedge inner 16px, band rows 86..93
    uint16_t* lt = flLeftTri->data()   + 2;   // ch1: left wedge outer 16px, rows 0..7
    uint16_t* rp = flRightPost->data() + 2;   // ch2: right wedge inner 16px, band rows 86..93
    uint16_t* rt = flRightTri->data()  + 2;   // ch3: right wedge outer 16px, rows 0..7
    for (int i = 0; i < 8; i++) {             // 8 band scanlines (172-179)
        // 32px field per side (two 16px sprites).  Measured vs the Atari, both triangles sat
        // 2px too far toward screen-centre, so nudge each 2px OUTWARD in the data (sprites
        // stay put — flLeftPost/flRightPost also carry the correctly-placed A-pillar, so their
        // X can't move): left field shifts left (<<2), right field shifts right (>>2).
        const uint32_t Lf = expandWedge32(mem[0x4DD2 + i]) << 2;   // left  wedge, 2px left
        const uint32_t Rf = expandWedge32(mem[0x4DDA + i]) >> 2;   // right wedge, 2px right
        const int lr = (int)kTerrainHeight + i;   // post-sprite band row = 86 + i
        // Triangles go on the sprites' SECOND bitplane (pen 10) -> darker grey COLOR18/COLOR22,
        // distinct from the A-pillars on plane 0 (pen 01).  Plane 0 stays 0 in the band rows.
        // Field layout: bits 31..16 = the side's LEFT sprite, bits 15..0 = the RIGHT sprite.
        lt[i  * 2] = 0; lt[i  * 2 + 1] = (uint16_t)(Lf >> 16);     // left  outer (left sprite, 0x82)
        lp[lr * 2] = 0; lp[lr * 2 + 1] = (uint16_t)(Lf & 0xFFFF);  // left  inner (right sprite, 0x92)
        rp[lr * 2] = 0; rp[lr * 2 + 1] = (uint16_t)(Rf >> 16);     // right inner (left sprite, 0x19E)
        rt[i  * 2] = 0; rt[i  * 2 + 1] = (uint16_t)(Rf & 0xFFFF);  // right outer (right sprite, 0x1AE)

        // Missiles M0 (left) / M1 (right) — the SECOND dark element of the band triangle,
        // measured live in atari800: on scanlines L138-143 (band rows i>=2) GRAFM=$06 turns on
        // M0 @ HPOSM0=$40 (just inside P0's right edge cc63) and M1 @ HPOSM1=$BE (just inside
        // P1), both taking COLPM0/1=$04 (dark) because PRIOR drops to $02 there.  They extend
        // each corner's dark inner edge by one colour clock (~2 Amiga px).  Fold into the inner
        // post sprites (same $04 pen10), at the 2px the <<2/>>2 nudge vacated next to the player:
        // left = rightmost 2px (toward centre), right = leftmost 2px (toward centre).
        if (i >= 2) {
            lp[lr * 2 + 1] |= 0x0003;   // M0: +2px on the left triangle's inner (right) edge
            rp[lr * 2 + 1] |= 0xC000;   // M1: +2px on the right triangle's inner (left) edge
        }
    }
}

// ---- artificial-horizon ground-fill sprites (instrument #6) ------------------
// The AH's brown ground is the Atari player P2 (COLPM2=$26), multiplexed below the
// windscreen frame onto sprite channels 0/1 (copper re-points SPR0PT/SPR1PT in the gap).
// Its GRAFP2 per scanline lives in the P2 player buffer: mem[$0E00 + O], where
// O = $32 + (Amiga_line - kTerrainLine).  The dial spans Amiga lines 182-214 (offsets
// $92-$B2) — $00 above the horizon (empty sky), $FF below (solid ground), the boundary
// moving with pitch.  Each Atari player byte is DOUBLE-WIDTH (8 bits -> 32 lores px), so it
// expands across BOTH 16px sprites: bits 7-4 -> ahLeft, bits 3-0 -> ahRight, each bit -> 4 px.
// Brown is pen 01 (plane A -> COLOR17, which the copper sets to $26 over the AH rows).
static const int      kAHRows    = 33;        // Amiga lines 182..214 (the dial extent)
static const uint16_t kAHBufBase = 0x0E92;    // P2 player buffer offset for Amiga line 182

static __inline uint16_t expandNibble16(uint8_t nib)
{
    uint16_t w = 0;                            // each of the 4 bits -> 4 Amiga px
    if (nib & 8) w |= 0xF000;
    if (nib & 4) w |= 0x0F00;
    if (nib & 2) w |= 0x00F0;
    if (nib & 1) w |= 0x000F;
    return w;
}

void RescueOnFractalus::buildAHSprite()
{
    uint16_t* l = ahLeft->data()  + 2;         // skip the 2 control words
    uint16_t* r = ahRight->data() + 2;
    for (int i = 0; i < kAHRows; i++) {
        uint8_t b = mem[kAHBufBase + i];                    // GRAFP2 ($00 sky / $FF ground)
        l[i * 2]     = expandNibble16((uint8_t)(b >> 4));   // bits 7-4 -> left 16px (plane A = pen01)
        l[i * 2 + 1] = 0x0000;                              // plane B unused
        r[i * 2]     = expandNibble16((uint8_t)(b & 0x0F)); // bits 3-0 -> right 16px
        r[i * 2 + 1] = 0x0000;
    }
}

// ---- throttle gauge sprite ---------------------------------------------------
// Build the player-1 throttle bar from the vobj strip mem[$0D98..].  Each strip
// byte is one Atari player scanline ($F0 = leftmost 4px on); we map a filled row
// to the leftmost 4 px (colour 01) of an Amiga sprite line.
void RescueOnFractalus::buildEnergyIndicatorSprite()
{
    // 57-row strip $0D98..$0DD0 (the original vobj player extent).  Each Atari
    // player bit is one colour clock = 2 Amiga lores px, so the 4-bit $F0 segment
    // is 8 px wide -> 0xFF00 (matches SIZEP1=0, normal width).
    uint16_t* d = energyIndicatorSprite->data() + 2;   // skip the 2 control words
    for (int i = 0; i < 57; i++) {
        uint16_t on = (mem[0x0D98 + i] & 0xF0u) ? 0xFF00u : 0x0000u;
        d[i * 2]     = on;     // plane A (colour bit 0)
        d[i * 2 + 1] = 0x0000; // plane B
    }
}

// ---- altimeter terrain-height bar (flight) -----------------------------------
// The altimeter's terrain-height indicator is the Atari player P0 strip $0C98: a solid
// 8px-wide bar that draw_altimeter_bars ($40E5) fills with $FF from offset $281A (the bar
// top, which tracks terrain height $0062) down to a fixed bottom ($0C98+$37).  Mirror the
// live buffer into the sprite each flight frame, exactly as buildEnergyIndicatorSprite mirrors the
// energy indicator's $0D98 — each Atari player bit → 2 Amiga lores px via kDoubleGlyph,
// so a solid $FF row becomes a full 16px sprite line.  (The P3 ship-height bar at $0F98 is
// parked until triggered; not built yet.)
static const int kAltimRows = 56;   // $0C98..$0CCF (bar top $281A varies, bottom fixed $37)
void RescueOnFractalus::buildAltimeterSprite()
{
    uint16_t* d = altimeterSprite->data() + 2;   // skip the 2 control words
    for (int i = 0; i < kAltimRows; i++) {
        d[i * 2]     = kDoubleGlyph[mem[0x0C98 + i]];   // plane A (pen 01)
        d[i * 2 + 1] = 0x0000;                          // plane B
    }
}

// ---- altimeter ship-height bar (flight) --------------------------------------
// The ship-height indicator is the Atari missile M3 (bits 6-7 of the shared missile
// buffer $0B00): draw_altimeter_bars ($40E5) steps its top edge at $0B98+$281B (which tracks
// ship altitude $0034) down to a fixed bottom ($0BCF).  SIZEM expands the 2px missile to
// 8px (quad width) = a full 16px Amiga sprite line, so a set M3 row → 0xFFFF.  Mask bits
// 6-7 to ignore the other missiles (M0-M2, e.g. the wing-clearance bars) sharing $0B00.
void RescueOnFractalus::buildAltimeterShipSprite()
{
    // Draw into the SECOND bitplane (pen 10) so this unattached sprite uses COLOR30, while
    // the altimeter terrain bar (sprite 6, same pair 6/7) uses plane 0 / pen 01 / COLOR29 —
    // two distinct colours from one pair without attaching (planes don't interact).
    uint16_t* d = altimeterShipSprite->data() + 2;   // skip the 2 control words
    for (int i = 0; i < kAltimRows; i++) {
        d[i * 2]     = 0x0000;                                         // plane 0 (unused here)
        d[i * 2 + 1] = (mem[0x0B98 + i] & 0xC0u) ? 0xFFFFu : 0x0000u;  // plane 1 (pen 10) — M3 bits 6-7
    }
}

// ---- starfield sprites -------------------------------------------------------
// During the stars phase display_setup positions players P0/P2/P3 as a sparse
// scrolling starfield (random_terrain_height $6B47: POKEY RANDOM, 1/32 chance of a dot
// from table $6B5F = [$80,$20,$04,$01]; scroll_field_columns $6AEE shifts each
// player up one scanline/frame and appends a new bottom byte).  The genuine
// transpiled scroll_field_columns already maintains those player buffers in
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

// ---- public interface --------------------------------------------------------
void RescueOnFractalus::initialize()
{
    titleBitmap   = Bitmap::allocate(kW, kTitleHeight,   kBP2, true);
    terrainBitmap = Bitmap::allocate(kW, kViewportFullHeight, kBP3, true);  // 3bp: tunnel reveal uses pens 4-7; 47 rows incl. wing band
    // Second flight terrain buffer for double-buffering renderFlightDirect (see header).
    terrainBitmapBack = Bitmap::allocate(kW, kViewportFullHeight, kBP3, true);
#ifdef ROF_FLIGHT_PROBE
    extern volatile uint32_t g_terrainBmpAddr;   // chip addr of terrainBitmap->data (Stage 1 verifier dump)
    g_terrainBmpAddr = (uint32_t)terrainBitmap->data;
#endif
    cockpitBitmap = Bitmap::allocate(kW, kCockpitH, kBP3, true);  // 3bp: bit-7 chars → red
    tunnelBitmap  = Bitmap::allocate(kW, kTerrainHeight, kBP3, true);  // door-gap reveal
    titleScreenBitmap = Bitmap::allocate(kW, kH, kBP3, true);  // 3bp: black + COLPF0-3 text pens

    // (The tunnel rings are decoded into tunnelBitmap from the $1000 field by
    // decodeTunnelField, triggered by the platform_tunnel_rings_drawn() hook when the
    // genuine display_setup draws them — not at init; see decodeTunnelField.)

    leftPost   = Sprite::allocate(kHT);
    rightPost  = Sprite::allocate(kHT);
    nullSprite = Sprite::allocate(0);
    energyIndicatorSprite = Sprite::allocate(57);    // throttle bar: 57 vobj-strip rows ($0D98..$0DD0)
    altimeterSprite = Sprite::allocate(kAltimRows);   // P0 $0C98 terrain-height bar (flight)
    altimeterShipSprite = Sprite::allocate(kAltimRows);   // M3 $0B98 ship-height bar (flight)
    // Flight windscreen frame: posts span the A-pillar (86 rows) + the 8 band scanlines
    // (= kHT+8 = 94); the triangle outer-half sprites cover only the 8 band scanlines.
    flLeftPost  = Sprite::allocate(kHT + 8);
    flRightPost = Sprite::allocate(kHT + 8);
    flLeftTri   = Sprite::allocate(8);
    flRightTri  = Sprite::allocate(8);
    // AH ground-fill: two 16px sprites (32px dial) reusing ch0/1 below the frame.
    ahLeft  = Sprite::allocate(kAHRows);
    ahRight = Sprite::allocate(kAHRows);
    if (!leftPost || !rightPost || !nullSprite || !energyIndicatorSprite || !altimeterSprite
        || !altimeterShipSprite || !flLeftPost || !flRightPost || !flLeftTri || !flRightTri
        || !ahLeft || !ahRight) return;
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
    energyIndicatorSprite->setX(0x81 + 203);
    energyIndicatorSprite->setY(0x2c + 144);
    // Altimeter terrain-height bar (P0 $0C98): same cockpit scanline band as the energy
    // indicator (both are player strips at buffer offset $98 → Amiga Y 0x2c+144), placed
    // left of it (CLAUDE.md instrument x≈108).  Starting estimate — calibrate visually.
    altimeterSprite->setX(0x81 + 107);
    altimeterSprite->setY(0x2c + 144);
    // Ship-height bar (M3 missile): the SAME 8px column as the terrain bar (they overlap —
    // the manual's "amount of light blue showing" is the ship bar visible above the purple
    // terrain bar).  The terrain bar is the higher-priority sprite (4 < 6), so it covers the
    // bottom (terrain height) and the light-blue ship bar shows above it (clearance).  Same
    // X and Y; bottoms align ($0B98..$0BCF ↔ $0C98..$0CCF, both at offset $98..$CF).
    altimeterShipSprite->setX(0x81 + 107);
    altimeterShipSprite->setY(0x2c + 144);

    // Post graphics are decoded once from the real RLE source tables (buildPostSprites,
    // triggered on the first perFrameWork frame); nothing to fill here.  Position/Y below.
    leftPost->setX(kSprXLeft);
    leftPost->setY(kTerrainLine);
    rightPost->setX(kSprXRight);
    rightPost->setY(kTerrainLine);

    // Flight windscreen-frame sprites (built once in buildFlightFrameSprites).  Posts span
    // the A-pillar + band (Y = kTerrainLine, 94 rows); the triangle outer-halves cover only
    // the 8 band scanlines (Y = band line = kTerrainLine + kTerrainHeight = 172).  X: each
    // side is two 16px sprites — post at the pillar X, triangle 16px outward (band players
    // sit HPOSP0=$30/HPOSP1=$C0, ~9 colour-clocks wider than the pillar's $39/$BF).
    const uint16_t kBandSprY = kTerrainLine + kTerrainHeight;   // 172
    flLeftPost->setX(kSprXLeft);        flLeftPost->setY(kTerrainLine);   // 0x92, inner (toward centre)
    flLeftTri->setX(kSprXLeft - 16);    flLeftTri->setY(kBandSprY);       // 0x82, outer (toward edge)
    flRightPost->setX(kSprXRight);      flRightPost->setY(kTerrainLine);  // 0x19E, inner (toward centre)
    flRightTri->setX(kSprXRight + 16);  flRightTri->setY(kBandSprY);      // 0x1AE, outer (toward edge)

    // AH ground-fill sprites: 32px (two 16px) at the dial position (CLAUDE x56, y138 →
    // Amiga line 182).  ch0/1's frame use ends at VSTOP=180; the copper re-points SPR0PT/
    // SPR1PT to these in the gap (FlightCopperList).  X/Y constant; the FILL is refreshed
    // each frame by buildAHSprite.
    ahLeft->setX(0x81 + 55);        ahLeft->setY(0x2c + 138);    // x55 (1px left of the dial bitmap); line 182
    ahRight->setX(0x81 + 55 + 16);  ahRight->setY(0x2c + 138);   // gap below frame VSTOP 180

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

    // Show a blank black display until the boot/standby build is ready (g_standbyRevealReady).
    // The real lists point at bitmaps that are still being built during the multi-second boot;
    // holding the EmptyCopperList avoids showing that piecemeal build (replaces the fade-to-black).
    emptyCopper = new EmptyCopperList();
    if (emptyCopper && emptyCopper->data()) {
        emptyCopper->buildLayout(*nullSprite);
        AmigaHardware::setCopperList(*emptyCopper, true);
        emptyCopperInstalled = true;
    }

    // Static-Standby fixed copper list: built once here (bitmaps + sprites now exist),
    // its dynamic colour/sprite slots refreshed each frame by updateStandbyCopper.
    // renderFrame installs it once the doors are decoded and the scene settles into
    // Standby; until then the EmptyCopperList holds the screen black.
    standbyCopper = new StandbyCopperList();
    if (standbyCopper && standbyCopper->data())
        standbyCopper->buildLayout(*titleBitmap, *terrainBitmap, *cockpitBitmap,
                                   *leftPost, *rightPost, *nullSprite);

    // Static stars/planet viewport fixed copper list (the line-doubled mode-D band),
    // same build-once + poke-in-place scheme; renderFrame installs it during rsStars.
    planetCopper = new PlanetCopperList();
    if (planetCopper && planetCopper->data())
        planetCopper->buildLayout(*titleBitmap, *terrainBitmap, *cockpitBitmap,
                                    *leftPost, *rightPost, *energyIndicatorSprite, *nullSprite,
                                    *starSprite[0], *starSprite[1], *starSprite[2]);

    // Static flight fixed copper list (scene 7), same build-once + poke scheme;
    // renderFrame installs it during rsFlight.  HUD sprites are poked in later by the
    // ported flight VBI; buildLayout seeds posts + gauge + nulls.
    flightCopper = new FlightCopperList();
    if (flightCopper && flightCopper->data())
        flightCopper->buildLayout(*titleBitmap, *terrainBitmap, *cockpitBitmap,
                                  *flLeftPost, *flLeftTri, *flRightPost, *flRightTri, *nullSprite,
                                  *ahLeft, *ahRight);

    // Launch-cinematic fixed copper lists (scene 4 doors / scene 5 tunnel), same
    // build-once scheme; renderFrame installs them during the launch cinematic.  Doors
    // points at the terrain + tunnel bitmaps (its sliding bands draw from both); Tunnel
    // points at the tunnel bitmap.  updateDoorsCopper/updateTunnelCopper poke the rest.
    for (int i = 0; i < 2; i++) {
        doorsCopper[i] = new DoorsCopperList();
        if (doorsCopper[i] && doorsCopper[i]->data())
            doorsCopper[i]->buildLayout(*titleBitmap, *cockpitBitmap,
                                        *leftPost, *rightPost, *energyIndicatorSprite, *nullSprite);
    }
    tunnelCopper = new TunnelCopperList();
    if (tunnelCopper && tunnelCopper->data())
        tunnelCopper->buildLayout(*titleBitmap, *tunnelBitmap, *cockpitBitmap,
                                  *leftPost, *rightPost, *energyIndicatorSprite, *nullSprite);

    // Title Screen fixed copper list (attract/level-select/results); renderFrame installs it
    // during rsTitle.  Full-screen text bitmap, black COLBK, 4 cycling text pens.
    titleScreenCopper = new TitleScreenCopperList();
    if (titleScreenCopper && titleScreenCopper->data())
        titleScreenCopper->buildLayout(*titleScreenBitmap, *nullSprite);

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
    // rings_drawn() hook flags it for decode then (draw_ring_frame_step streams the
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
// $073D/$0793 row-address table built for base $1000, and draw_ring_frame_step streams
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
    // region vertically (the Planet/Flight viewport band toggles the bitplane modulo
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

// ── Direct flight terrain renderer (terrain-draw-plan Stages 1-3) ──────────────
// Plot the terrain sky straight to bitplanes from $260E (yForX) — NO mem[$1070] round-trip,
// NO full-buffer LUT scan, NO shadow (the heavy parts of renderViewportModeD).  Mapping
// pinned empirically: Amiga logical column c (0..159) <- $260E[c+48]; skyline scanline =
// 150 - height (the $28CA/$28FA row table is linear); $FF = off-top (all body).  plane1 =
// sky (filled above the skyline via ONE descending blitter fill), plane2 = dots (TODO).
// Terrain rows 0-42; the windscreen-bottom band (rows 43-46, from mem[$2098]) is still
// converted (4 rows) so it isn't lost.  Verified: plane1 byte-exact vs the old convert
// (0/13760); ~2.8x cheaper per frame (fDirect 120 vs fConvert 339 beam ticks).
void RescueOnFractalus::renderFlightDirect()
{
    if (!terrainBitmap || !terrainBitmapBack || !flightCopper) return;

    // Double-buffer: paint the OFF-screen buffer (the one the copper is NOT currently showing),
    // then re-point the copper to it.  The flip latches at the next vblank, so the live buffer
    // is never cleared/refilled mid-frame (that was the plane1 flicker).  First frame
    // (flightDisplayed==null) draws into terrainBitmapBack.
    Bitmap* const back = (flightDisplayed == terrainBitmapBack) ? terrainBitmap : terrainBitmapBack;
    uint8_t* const bp = (uint8_t*)back->data;

    // Terrain rows 0-42 (43 scanlines): clear all 3 planes, then build plane1 sky.
    AmigaHardware::blitterClear((uint16_t*)bp, 60, 43, 0);
    AmigaHardware::blitterWait();

    // Edge plot: ONE plane1 bit per column at its skyline scanline (160 byte-ORs).
    const uint8_t* const yForX = (const uint8_t*)mem + 0x260E + 48;   // col 0 -> $260E[48]
    static const uint8_t kMask[4] = { 0xC0u, 0x30u, 0x0Cu, 0x03u };   // 2-bit pixel within byte
    for (int c = 0; c < 160; c++) {
        uint8_t h = yForX[c];
        if (h == 0xFFu) continue;                            // off-top: all body
        int scan = 150 - (int)h;                             // height -> skyline scanline
        if (scan < 0) scan = 0;
        if (scan > 42) scan = 42;                            // terrain region
        bp[scan * 120 + (c >> 2)] |= kMask[c & 3];           // plane1 skyline edge bit
    }
    // Sky fill: propagate each edge bit UP in ONE descending blit (writes rows 0-41, seed 42).
    AmigaHardware::blitterFillUp((uint16_t*)bp, 20, 42, 80);
    AmigaHardware::blitterWait();

    // plane2 = terrain dots/detail (mode-D value-2; value-3 highlight also sets it).  The
    // blitter builds plane1 (sky) from $260E but carries no dot info, so decode plane2 from
    // the mode-D field mem[$1070] (the upstream CPU pass still fills it).  Sparse, but every
    // byte must be scanned to find the dots: 43 rows × 40 bytes via the kModeDP2 LUT into the
    // freshly-cleared plane2 (offset +40 within each 120-byte interleaved scanline).
    // (Perf endgame is to have the terrain rasterizer set plane2 as it plots — flight item #1.)
    {
        const uint8_t* s2 = (const uint8_t*)mem + 0x1070 + 4;   // row 0, +4 crop (matches plane1 align)
        uint8_t* p2 = bp + 40;                                  // plane2 of scanline 0
        for (int row = 0; row < 43; row++, s2 += 96, p2 += 120) {
            for (int b = 0; b < 40; b++) {
                uint8_t d = kModeDP2[s2[b]];
                if (d) p2[b] = d;                               // cleared to 0; write only set dots
            }
        }
    }

    // Windscreen-bottom band (scanlines 43-46): still a 4-row mode-D convert from mem[$1070]
    // (= $2098+ wing band).  Cheap (4/47 of the old convert); keeps the frame element.
    const uint8_t* src = (const uint8_t*)mem + 0x1070 + 4 + 43 * 96;  // band source rows
    uint8_t* vd = bp + 43 * 120;
    for (int row = 0; row < 4; row++, src += 96, vd += 120) {
        for (int b = 0; b < 40; b++) {
            uint8_t s = src[b];
            vd[b] = kModeDP1[s]; vd[40 + b] = kModeDP2[s]; vd[80 + b] = 0;
        }
    }

    // Flip: re-point the flight copper's viewport bitplanes to the buffer we just painted.
    // The copper latches this at the next vblank (renderFrame waits for VBI after render()).
    flightCopper->setTerrainBitplanes(*back);
    flightDisplayed = back;
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
            flightCopperInstalled = false;
            tunnelCopperInstalled = false;
        }
        return;
    }
    emptyCopperInstalled = false;

    deriveRenderSignals();   // recompute the mem[]-derived render-gating signals for this frame
    // Tunnel reveal: the $52D7 VBI's draw_ring_frame_step draws the expanding black
    // clear into mem[$2000] and flags g_tunnelFieldDirty with its row extent; re-decode
    // those rows into tunnelBitmap here (was in run()'s tunnel loop, now that the
    // transpiled display_setup drives the cinematic).
    if (g_tunnelFieldDirty) {
        decodeTunnelField((int)g_tunRowLo, (int)g_tunRowHi); g_tunnelFieldDirty = 0;
    }
    perFrameWork();
    render();

    // Static Standby (incl. the gauge-fill sub-phase before the doors scroll): the
    // copper layout is FIXED here (!rsViewport, doors not
    // parting), so drive the single fixed StandbyCopperList by poking only changed
    // colour/sprite slots — no per-frame full rebuild, no double-buffer flip.  Gated
    // on g_doorFieldReady (doors decoded, fade reveal done -> global fade is 16).
    // Title Screen (attract / level-select / results): a fixed full-screen text bitmap on
    // black, with 4 text pens that cycle.  Decode the text once on entry (it is static while
    // displayed); poke the cycling pens each frame.  Runs under the $53CC VBI (see rsTitle).
    const bool staticTitle = titleScreenCopper && rsTitle;
    if (staticTitle) {
        if (!titleScreenCopperInstalled) {
            decodeTitleScreen();
            updateTitleScreenCopper(true);
            AmigaHardware::setCopperList(*titleScreenCopper, false);
            titleScreenCopperInstalled = true;
        } else {
            updateTitleScreenCopper(false);
        }
        standbyCopperInstalled = false; planetCopperInstalled = false;
        flightCopperInstalled = false; tunnelCopperInstalled = false;
        return;
    }

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
        planetCopperInstalled = false; flightCopperInstalled = false;
        tunnelCopperInstalled = false; titleScreenCopperInstalled = false;
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
        tunnelCopperInstalled = false; titleScreenCopperInstalled = false;
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
        tunnelCopperInstalled = false; titleScreenCopperInstalled = false;
        return;
    }

    // Launch cinematic (the only phase left for the fixed Doors/Tunnel lists): the hangar
    // doors part (scene 4) then the tunnel fills the screen (scene 5).  Door-scroll progress
    // g2 = 0x2B - $008A grows 0 -> kTerrainHeight/2 as the doors open; once it reaches the
    // half-height the doors are fully open and the single full tunnel band takes over.
    const uint16_t half = (uint16_t)(kTerrainHeight / 2);
    const uint16_t g2   = rsLaunched ? (uint16_t)(0x2Bu - mem[zp::terrainScrollCounter]) : 0;
    if (tunnelCopper && g2 >= half) {
        // ---- scene 5: tunnel descent (doors fully open) ----
        if (!tunnelCopperInstalled) {
            updateTunnelCopper(true);
            AmigaHardware::setCopperList(*tunnelCopper, false);
            tunnelCopperInstalled = true;
        } else {
            updateTunnelCopper(false);
        }
    } else if (doorsCopper[0]) {
        // ---- scene 4: hangar doors parting (also the closed g2==0 first frame) ----
        // Populate the BACK buffer fully (geometry + colours), then swap it in — the swap
        // latches at the next vblank, so the displayed buffer is never modified mid-frame
        // (avoids the ±1px tunnel-reveal jitter an in-place poke of the live list causes).
        const uint8_t back = (uint8_t)(1 - doorsActive);
        updateDoorsCopper(doorsCopper[back]);
        AmigaHardware::setCopperList(*doorsCopper[back], false);
        doorsActive = back;
        tunnelCopperInstalled = false;
    }
    standbyCopperInstalled = false;   // left Standby — next static entry re-seeds + re-installs
    planetCopperInstalled = false;
    flightCopperInstalled = false;
    titleScreenCopperInstalled = false;
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
    const uint16_t energyCol = atariToOCS(mem[0x00DE]);             // gauge bar colour ramp
    const uint16_t terr0    = atariToOCS(mem[0x02C0]);             // terrain pen0 (road dots)
    const uint16_t terr1    = atariToOCS(mem[0x02C7]);             // terrain pen1 (LEVEL text)
    const uint16_t terr2    = atariToOCS(mem[zp::colorRing]);      // terrain pen2 ($08D4)
    const uint16_t terr3    = atariToOCS(mem[zp::displayFlags]);   // terrain pen3 (green bg, $0071)
    const int8_t   gauge    = (int8_t)(rsEnergyIndicator ? 1 : 0);

    if (force || titleBg != sbTitleBg || titlePf0 != sbTitlePf0) {
        standbyCopper->setTitlePalette(titleBg, titlePf0, atariToOCS(0x78));  // pf1 = blue (const)
        standbyCopper->setSpritePostColor(titleBg);
        sbTitleBg = titleBg; sbTitlePf0 = titlePf0;
    }
    if (force || energyCol != sbEnergyCol) {
        standbyCopper->setEnergyIndicatorColor(energyCol);
        sbEnergyCol = energyCol;
    }
    const uint16_t compassCol = atariToOCS(mem[0x00CF]);   // compass band COLPF0 (dark grey)
    if (force || compassCol != sbCompassCol) {
        standbyCopper->setCompassColor(compassCol);
        sbCompassCol = compassCol;
    }
    if (force || terr0 != sbTerr0 || terr1 != sbTerr1 || terr2 != sbTerr2 || terr3 != sbTerr3) {
        // Any terrain pen changed: rewrite all four.  color00 = COLBK green ($0071 = terr3),
        // color03 = road-dot dark ($02C0 = terr0): the door field decodes COLBK (value 8)→pen0
        // and the dark dots (value 0)→pen3 (see kNibbleColour).  color00 then carries the green
        // unbroken into the windscreen band (no setBandBgColor needed — the band corners inherit
        // it), matching the Atari's continuous COLBK.  terr3 still ramps the dark->bright green.
        standbyCopper->setTerrainPalette(terr3, terr1, terr2, terr0);
        sbTerr0 = terr0; sbTerr1 = terr1; sbTerr2 = terr2; sbTerr3 = terr3;
    }
    if (force || gauge != sbEnergyIndicator) {
        standbyCopper->setSprite2(gauge ? *energyIndicatorSprite : *nullSprite);
        sbEnergyIndicator = gauge;
    }
}

// updateTitleScreenCopper(): poke the Title Screen's 4 text pens from the live COLPF0-3
// shadows ($02C4-$02C7).  The Atari cycles these periodically (the "palette changes every
// now and then"); reading the shadows each frame reproduces that.  Poke-on-change.
void RescueOnFractalus::updateTitleScreenCopper(bool force)
{
    const uint16_t pf0 = atariToOCS(mem[0x02C4]);
    const uint16_t pf1 = atariToOCS(mem[0x02C5]);
    const uint16_t pf2 = atariToOCS(mem[0x02C6]);
    const uint16_t pf3 = atariToOCS(mem[0x02C7]);
    if (force || pf0 != tsPf0 || pf1 != tsPf1 || pf2 != tsPf2 || pf3 != tsPf3) {
        titleScreenCopper->setTextPalette(pf0, pf1, pf2, pf3);
        tsPf0 = pf0; tsPf1 = pf1; tsPf2 = pf2; tsPf3 = pf3;
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
    const uint16_t energyCol = atariToOCS(mem[0x00DE]);             // gauge bar colour ramp
    const uint16_t starCol  = atariToOCS(mem[0x02C0]);             // starfield grey ($02C0)
    const uint16_t bgCol    = atariToOCS(mem[0x00DC]);             // viewport COLBK (space, $00DC)

    if (force || titleBg != plTitleBg || titlePf0 != plTitlePf0) {
        planetCopper->setTitlePalette(titleBg, titlePf0, atariToOCS(0x78));  // pf1 = blue (const)
        planetCopper->setSpritePostColor(titleBg);
        plTitleBg = titleBg; plTitlePf0 = titlePf0;
    }
    if (force || energyCol != plEnergyCol) {
        planetCopper->setEnergyIndicatorColor(energyCol);
        plEnergyCol = energyCol;
    }
    if (force || starCol != plStarCol) {
        planetCopper->setStarColor(starCol);
        plStarCol = starCol;
    }
    const uint16_t compassCol = atariToOCS(mem[0x00CF]);   // compass band COLPF0 (dark grey)
    if (force || compassCol != plCompassCol) {
        planetCopper->setCompassColor(compassCol);
        plCompassCol = compassCol;
    }
    if (force || bgCol != plBg) {
        planetCopper->setPlanetBgColor(bgCol);
        plBg = bgCol;
    }
    // Windscreen-bottom band (scanlines 172-179): the launch band DLI $6D67 switches the
    // bottom 4 mode-D viewport rows to the windscreen-FRAME palette — black bg, two greys
    // ($04/$06) for the cockpit frame, and COLPF2 left at $2A (the planet's brightest tone,
    // from viewport DLI $6D0E) for the planet in the corner gaps.  Baked in buildLayout
    // (PlanetCopperList).  (Earlier this poked $00DD/$00DA/$00D4, which read black/wrong.)
}

// updateFlightCopper(): refresh the FlightCopperList's per-frame-varying slots from mem[].
// Same poke-only-on-change scheme.  The terrain pens are baked constant in buildLayout
// (the salmon→brown fade will poke setTerrainPen0 from the native atmosphere code); the
// HUD sprite pointers are poked by the ported flight VBI.  Here we keep the top-bar / gauge
// colours live.
void RescueOnFractalus::updateFlightCopper(bool force)
{
    const uint16_t titleBg  = atariToOCS(mem[0x02C8]);             // COLBK = top-bar bg / canopy posts
    const uint16_t titlePf0 = atariToOCS(mem[zp::textColorPf0]);   // COLPF0 = top-bar text ($00D8)
    const uint16_t energyCol = atariToOCS(mem[0x00DE]);             // gauge bar colour

    if (force || titleBg != flTitleBg || titlePf0 != flTitlePf0) {
        flightCopper->setTitlePalette(titleBg, titlePf0, atariToOCS(0x78));  // pf1 = blue (const)
        flightCopper->setSpritePostColor(titleBg);
        flTitleBg = titleBg; flTitlePf0 = titlePf0;
    }
    if (force || energyCol != flEnergyCol) {
        flightCopper->setEnergyIndicatorColor(energyCol);
        flEnergyCol = energyCol;
    }

    // Altimeter terrain-height bar (P0 $0C98) on sprite 4: pointer + colour are constant
    // (the live buffer content is refreshed in buildAltimeterSprite), so install once on
    // force.  COLPM0 for the cockpit bar is cyan ($A9 — the DLI value, distinct from the
    // grey it uses for the left canopy pillar at the top of the same player).
    // Altimeter (overlapping 8px column): terrain bar = $00D5 (purple/red), ship bar =
    // $00D6 (steady light blue).  Both are constant cockpit colours, so set once on install.
    // ($00D9 — which I'd used for the ship — color-cycles for the enemy/lock-on flash, not
    // the altimeter.)
    if (force) {
        // HUD sprite channels (the frame sprites 0-3 are seeded in buildLayout):
        //   5 = energy bar (COLOR25), 6 = altimeter terrain (COLOR29 pen01),
        //   7 = altimeter ship (COLOR30 pen10).
        flightCopper->setHudSprite(5, *energyIndicatorSprite);
        flightCopper->setHudSprite(6, *altimeterSprite);
        flightCopper->setAltimeterColor(atariToOCS(mem[0x00D5]));
        flightCopper->setHudSprite(7, *altimeterShipSprite);
        flightCopper->setAltimeterShipColor(atariToOCS(mem[0x00D6]));
    }
    // (The wing-clearance centre plane symbol is part of the mode-D band bitmap — the value-2
    // $AA $AA centre marker decoded into the viewport rows — so it needs no separate sprite.)

    // Compass band colour: the $49EE slot-0 DLI sets COLPF0 = mem[$00CF] (dark grey) for the
    // mode-4 compass line — poke it into the band's color01 so the housing/heading show in
    // the compass's own colour rather than the title text colour.
    const uint16_t compassCol = atariToOCS(mem[0x00CF]);
    if (force || compassCol != flCompassCol) {
        flightCopper->setCompassColor(compassCol);
        // The band windscreen-corner triangles (sprite pen 10) take the same $00CF dark grey
        // the band DLI $4A40 gives the wedge players (COLPM0/1) — COLOR18 + COLOR22.
        flightCopper->setTriangleColor(compassCol);
        flCompassCol = compassCol;
    }
    // Compass needle / heading letters (value-3 = COLPF2) are salmon ($2A); set once.
    if (force) flightCopper->setCompassNeedleColor(atariToOCS(0x2A));

    // Terrain salmon→brown fade (#2): the flight VBI computes the atmosphere colours
    // each frame from altitude — pen0 = terrain body ($00DC), pen1 = sky ($00DD).  Poke
    // them into the copper as they ramp (the "native computes, callback updates the
    // copper" model).  pen2 (dots) / pen3 (highlight) stay the baked constants.  At flight
    // entry pen0 starts near the sky tone and ramps to brown as the ship descends, so the
    // view no longer snaps from the planet's salmon straight to brown.
    const uint16_t terr0 = atariToOCS(mem[0x00DC]);
    const uint16_t terr1 = atariToOCS(mem[0x00DD]);
    if (force || terr0 != flTerr0 || terr1 != flTerr1) {
        flightCopper->setTerrainPalette(terr0, terr1, atariToOCS(0x20), atariToOCS(0x18));
        flTerr0 = terr0; flTerr1 = terr1;
    }

    // Wing-clearance band (scanlines 172-179): the band DLI changes ONLY COLPF2 to the frame
    // grey ($00D4) — color00/01/02 (bg/bars/dots) inherit from the terrain palette above and
    // fade with it (the salmon→brown bug was poking the band only when $00DD/$00D4 changed, so
    // the inherited-bg approach also fixes the stuck-salmon fade).  Poke only color03.
    const uint16_t band3 = atariToOCS(mem[0x00D4]);
    if (force || band3 != flBand3) {
        flightCopper->setBandPalette(band3);
        flBand3 = band3;
    }
}

// updateDoorsCopper(): fully populate one DoorsCopperList buffer for the hangar-doors-
// parting phase (scene 4) — the title/gauge/compass colours and the sliding-door geometry
// (the 3 terrain bands).  The bands' WAIT lines + bitplane pointers move as the
// doors open.  Because the list is double-buffered (the caller swaps the back buffer in at
// vblank), every slot is written unconditionally — the back buffer is two frames stale, so
// there is no poke-on-change fast path.
void RescueOnFractalus::updateDoorsCopper(DoorsCopperList* dc)
{
    const uint16_t titleBg  = atariToOCS(mem[0x02C8]);            // COLBK = title bg / canopy posts
    const uint16_t titlePf0 = atariToOCS(mem[zp::textColorPf0]);  // COLPF0 = title text ($00D8)
    const uint16_t energyCol = atariToOCS(mem[0x00DE]);            // gauge bar colour ramp
    const uint16_t compassCol = atariToOCS(mem[0x00CF]);          // compass band COLPF0

    dc->setTitlePalette(titleBg, titlePf0, atariToOCS(0x78));     // pf1 = blue (const)
    dc->setSpritePostColor(titleBg);
    dc->setEnergyIndicatorColor(energyCol);
    dc->setCompassColor(compassCol);

    // Sliding-door geometry.  topBase = terrain row g2 (slides up); tunBase = tunnel row
    // (half - g2) (the reveal centred on the vanishing point); botBase = terrain row half.
    const uint16_t half = (uint16_t)(kTerrainHeight / 2);
    const uint16_t g2   = rsLaunched ? (uint16_t)(0x2Bu - mem[zp::terrainScrollCounter]) : 0;
    const uint32_t ta   = (uint32_t)terrainBitmap->data;
    // pen0 = COLBK green ($0071), pen3 = road-dot dark ($02C0): the door field decodes
    // COLBK (value 8)→pen0 and the dark dots (value 0)→pen3 (see kNibbleColour).  color00
    // green then flows unbroken through all three terrain bands AND the tunnel reveal into
    // the windscreen band (the Atari keeps COLBK green across the whole viewport+band — see
    // doors_mid, COLBK=$C8 y50-136), so the band corners inherit it: no setBandBgColor.
    dc->update(g2,
                        ta + (uint32_t)g2 * 120u,
                        (uint32_t)tunnelBitmap->data + (uint32_t)(half - g2) * 120u,
                        ta + (uint32_t)half * 120u,
                        atariToOCS(mem[zp::displayFlags]),       // pen0 = COLBK green ($0071)
                        atariToOCS(mem[0x02C7]),                 // terrain pen1
                        atariToOCS(mem[zp::colorRing]),          // terrain pen2 ($08D4)
                        atariToOCS(mem[0x02C0]),                 // pen3 = road-dot dark ($02C0)
                        atariToOCS(mem[zp::colorRing + 0]),      // ring pen4 ($08D4)
                        atariToOCS(mem[zp::colorRing + 1]),      // ring pen5 ($08D5)
                        atariToOCS(mem[zp::colorRing + 2]),      // ring pen6 ($08D6)
                        atariToOCS(mem[zp::colorRing + 3]),      // tunnel pen1 ($08D7)
                        atariToOCS(mem[zp::colorRing + 4]),      // tunnel pen2 ($08D8)
                        atariToOCS(mem[zp::colorRing + 5]));     // tunnel pen3 ($08D9)
}

// updateTunnelCopper(): refresh the TunnelCopperList for the full tunnel descent (scene 5,
// doors fully open).  Constant title/gauge/compass via poke-on-change; the tunnel ring
// palette (pen0 black + pens 1-6 fed by the rotating $08D4-$08D9 ring, +3 rotated as the
// Atari tunnel DLI applies) is poked when any entry changed.
void RescueOnFractalus::updateTunnelCopper(bool force)
{
    const uint16_t titleBg  = atariToOCS(mem[0x02C8]);
    const uint16_t titlePf0 = atariToOCS(mem[zp::textColorPf0]);
    const uint16_t energyCol = atariToOCS(mem[0x00DE]);
    const uint16_t compassCol = atariToOCS(mem[0x00CF]);

    if (force || titleBg != tnTitleBg || titlePf0 != tnTitlePf0) {
        tunnelCopper->setTitlePalette(titleBg, titlePf0, atariToOCS(0x78));
        tunnelCopper->setSpritePostColor(titleBg);
        tnTitleBg = titleBg; tnTitlePf0 = titlePf0;
    }
    if (force || energyCol != tnEnergyCol)   { tunnelCopper->setEnergyIndicatorColor(energyCol);   tnEnergyCol = energyCol; }
    if (force || compassCol != tnCompassCol) { tunnelCopper->setCompassColor(compassCol); tnCompassCol = compassCol; }
    // Windscreen-corner reveal.  The corner triangle is the quad-width canopy-post player
    // ($0C88-$0C8F left), green (COLPM0/1 = mem[$0071]); the launch clears it top-down so the
    // tunnel ($08D8 purple) shows through line-by-line.  We approximate with a single moving
    // color00 split: band top = purple; flip back to green at the boundary = the first
    // still-set player scanline (so it descends as the buffer clears).  doors_mid/descent
    // show this band only at g2>=half, i.e. the Tunnel list (Doors keeps the band green).
    uint16_t greenLine = 8;                                       // first still-green band scanline
    for (uint16_t i = 0; i < 8; i++) { if (mem[0x0C88 + i]) { greenLine = i; break; } }
    const uint16_t green = atariToOCS(mem[0x0071]);
    // greenLine 0 = reveal not started (whole band green): keep band top green, no purple split
    // (avoids a 1-line purple flash where the band-top WAIT and the boundary WAIT collide).
    const uint16_t bandTop = (greenLine == 0) ? green : atariToOCS(mem[0x08D8]);
    if (force || bandTop != tnBandBg) { tunnelCopper->setBandBgColor(bandTop); tnBandBg = bandTop; }
    tunnelCopper->setBandReveal(greenLine, green);

    const uint16_t pen0 = atariToOCS(mem[0x02C0]);               // tunnel pen0 = black
    uint16_t ring[6];
    bool ringChanged = (pen0 != tnPen0);
    for (int i = 0; i < 6; i++) {
        ring[i] = atariToOCS(mem[zp::colorRing + i]);
        if (ring[i] != tnRing[i]) ringChanged = true;
    }
    if (force || ringChanged) {
        // pens 1-3 = ring[3..5] ($08D7-$08D9); pens 4-6 = ring[0..2] ($08D4-$08D6).
        tunnelCopper->setTunnelColors(pen0, ring[3], ring[4], ring[5], ring[0], ring[1], ring[2]);
        tnPen0 = pen0;
        for (int i = 0; i < 6; i++) tnRing[i] = ring[i];
    }
}

// deriveRenderSignals(): recompute the renderer's phase-gating signals from mem[]
// hardware state, once per frame.  These replace the C++ launchPhase enum as the
// renderer's source of truth, so the copper-list selection/render/perFrameWork keep
// working as the transpiled game_entry/game_main_loop/display_setup drive the program.
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
    // Title Screen (attract/level-select/results): runs under the $53CC in-game VBI, which
    // ALSO covers early-boot transitional frames — so additionally require the title text to
    // be present in its screen RAM ($365B holds 'R' of "RESCUE" = internal $32 | COLPF1<<6 =
    // $72).  display_list_init ($5D29) builds it there; standby/flight don't use $365B.
    extern volatile unsigned char g_forceTitleScreen;   // ROF_FORCE_TITLE visual-test override
    rsTitle    = ((vvblki == 0x53CCu) && (mem[0x365B] == 0x72u)) || g_forceTitleScreen;
    rsStars    = standbyVbi && (mem[0x060B] == 0x23u) && (mem[0x0200] == 0xC2u);
    rsViewport = rsStars || rsFlight;
    rsEnergyIndicator    = (mem[0x060B] != 0);

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

    // Force a one-time full title + cockpit repaint when the transpiled display_setup (NOT a
    // hooked writer) builds the cockpit: while the scene is transitional (boot/building), and
    // ONCE on entry to the stars/planet viewport or to flight.  The cockpit is otherwise
    // WRITER-DRIVEN (the g_ck* span registry) — in flight the instrument writers register the
    // exact cells they change, so re-scanning ~580 cells EVERY frame is gone (it was the #1
    // flight cost).  Title still uses g_titleDirty via the $782A copy hook.
    if (g_doorFieldReady == 0u || (rsStars && !prevRsStars) || (rsFlight && !prevRsFlight)) {
        g_titleDirty = 1; cockpitForceFull = true;
    }
    prevRsStars  = rsStars;
    prevRsFlight = rsFlight;
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
    // ($62FB) drives it — copy_title_text_block_to_screen ($782A) copies the block
    // the SFX sequencer selects (via $0091) into screen RAM $32B7 every frame.  We
    // don't re-copy it here; render() picks up the change by shadow-comparing $32B7.

    if (mem[zp::joystickSaved] != 0)            // $004A set when the game starts
        startup_init_native();          // $3FFA: cockpit digit update

    if (rsEnergyIndicator) buildEnergyIndicatorSprite();
    // Canopy posts: constant graphic, decoded once from the real RLE source tables — shown
    // in every screen (independent of the live $0C32/$0D32 buffers, which only hold the
    // frame at gameplay init and are the starfield otherwise).
    if (!postsBuilt) { buildPostSprites(); buildFlightFrameSprites(); postsBuilt = true; }
    // Starfield players $0C32/$0E32/$0F32: scrolled+seeded during stars, static
    // through the planet zoom, so map them both phases.
    if (rsStars) buildStarSprites();
    // Flight altimeter bars: mirror the live P0 $0C98 (terrain-height) + M3 $0B98
    // (ship-height) strips each frame.
    if (rsFlight) { buildAltimeterSprite(); buildAltimeterShipSprite(); buildAHSprite(); }
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

// Decode a run of nCells cockpit cells starting at Atari screen-RAM address `addr` (cells in
// the same DL row) into cockpitBitmap.  Handles both the modeD raster band ($350D, 4 entries
// × 2 identical scan lines, raw 2bpp) and the mode4 dashboard ($332D, 10 entries × 8 scan
// lines, charset $3800, bit-7 → plane3).  Cells outside the visible 40-byte window (the
// 4-byte wide-field crop) are skipped.  Layout matches the old full scan exactly.
void RescueOnFractalus::decodeCockpitSpan(uint16_t addr, uint8_t nCells)
{
    static const int kStride   = 48;    // wide-playfield bytes per DL row
    static const int kCrop     = 4;     // skip 4 left overscan bytes
    static const int kRowBytes = 120;   // 3bp interleaved: p1(40)+p2(40)+p3(40)
    uint8_t* cdest = (uint8_t*)cockpitBitmap->data;

    for (uint8_t i = 0; i < nCells; i++) {
        uint16_t a = (uint16_t)(addr + i);
        if (a >= 0x350Du) {                     // modeD raster band
            int off = (int)(a - 0x350Du);
            int entry = off / kStride, col = (off % kStride) - kCrop;
            if (entry < 0 || entry >= 4 || col < 0 || col >= 40) continue;
            uint8_t p1v, p2v; decode2bppByte(mem[a], &p1v, &p2v);
            uint8_t* d0 = cdest + (entry * 2) * kRowBytes;
            uint8_t* d1 = d0 + kRowBytes;
            d0[col] = p1v; d0[40 + col] = p2v; d0[80 + col] = 0;
            d1[col] = p1v; d1[40 + col] = p2v; d1[80 + col] = 0;
        } else {                                // mode4 dashboard
            int off = (int)(a - 0x332Du);
            int entry = off / kStride, col = (off % kStride) - kCrop;
            if (entry < 0 || entry >= 10 || col < 0 || col >= 40) continue;
            uint8_t ch = mem[a];
            uint8_t plane3 = (ch & 0x80u) ? 0xFFu : 0x00u;
            const uint8_t* glyph = (const uint8_t*)mem + 0x3800u + (uint16_t)(ch & 0x7Fu) * 8u;
            uint8_t* p = cdest + (8 + entry * 8) * kRowBytes + col;
            for (int scan = 0; scan < 8; scan++, p += kRowBytes) {
                uint8_t p1v, p2v; decode2bppByte(*glyph++, &p1v, &p2v);
                p[0] = p1v; p[40] = p2v; p[80] = plane3;
            }
        }
    }
}

// Decode the whole cockpit region once (scene-entry repaint / registry overflow): all 4
// modeD rows + 10 mode4 rows.  The transpiled display_setup (not a hooked writer) builds
// the cockpit on entry, so the writer-driven registry alone would miss the initial paint.
void RescueOnFractalus::decodeCockpitFull()
{
    for (int e = 0; e < 4;  e++) decodeCockpitSpan((uint16_t)(0x350Du + e * 48 + 4), 40);
    for (int e = 0; e < 10; e++) decodeCockpitSpan((uint16_t)(0x332Du + e * 48 + 4), 40);
}

// Compass (#2): the heading indicator is 4 mode-4 cells $32E3-$32E6 on the mode-4 line at
// display y=32 (below the title text) — drawn by the compass updater ($3FDE, mislabelled
// "draw_compass_heading") from glyph table $4B0B, or the housing $01 by game_sub_4606.  Its
// background/pens are the frame-top colours, which are exactly the title-region palette
// (COLBK=$00D4=$02C8, COLPF0=$00D8, COLPF1=$00D7), so it renders into the title bitmap with
// no extra palette band; charset is $0400 (CHBAS, before the $3FDE/$49EE DLI switches it to
// $3800 for the viewport below).  The mode-4 cells decode to the 2 title bitplanes via
// decode2bppByte.  We touch only these 4 cells × 8 scanlines × 2 planes = 16 longwords.
void RescueOnFractalus::decodeCompass()
{
    static const uint16_t kCompassRAM     = 0x32E3;   // 4 mode-4 cells (heading / housing)
    static const uint16_t kCompassCharset = 0x3800;   // cockpit font (the $49EE DLI sets CHBASE=$38
                                                       // for this line; char $01 = 0x55 solid housing)
    static const int       kCompassRow    = 33;       // title-bitmap row for display y=32
    static const int       kCompassByteX  = 18;       // x=144 → byte 144/8 (plane stride 80)
    uint8_t* tbmp = (uint8_t*)titleBitmap->data;
    const uint8_t* src = (const uint8_t*)mem + kCompassRAM;
    for (int cell = 0; cell < 4; cell++) {
        const uint8_t* glyph = (const uint8_t*)mem + kCompassCharset + (src[cell] & 0x7Fu) * 8u;
        for (int s = 0; s < 8; s++) {
            uint8_t p1v, p2v;
            decode2bppByte(glyph[s], &p1v, &p2v);
            uint8_t* row = tbmp + (kCompassRow + s) * 80;   // 80 = 40 plane1 + 40 plane2 (interleaved)
            row[kCompassByteX + cell]      = p1v;
            row[40 + kCompassByteX + cell] = p2v;
        }
    }
}

// decodeTitleScreen(): decode the Title Screen (scene "Title Screen", attract/level-select/
// results) text into titleScreenBitmap.  Source: charset $0400, screen RAM $365B (6 rows ×
// 20 chars).  ANTIC mode 6/7: each char byte = (colour-select << 6) | charcode; charcode
// indexes the charset (8 bytes/glyph), colour-select (0-3) picks COLPF0-3.  We render the
// glyph foreground as pen = colour-select + 1 (COLPF0->pen1 .. COLPF3->pen4) on a pen-0
// (black, COLBK) background, into a 3-bitplane interleaved bitmap.  Mode 6/7 chars are
// double-WIDTH (20 chars across 320px = 16px/char, each glyph bit -> 2 px); the mode-7 title
// row is also double-HEIGHT (each glyph scanline -> 2).  The copper shows the bitmap full
// screen and pokes color01-04 from COLPF0-3 each frame (the palette cycle).  Decodes once
// per entry (dirty-gated), so the full clear + per-pixel build is fine.
void RescueOnFractalus::decodeTitleScreen()
{
    if (!titleScreenBitmap) return;
    static const uint16_t kCharset   = 0x0400;
    static const uint16_t kScreenRAM = 0x365B;
    static const int       kStride   = 120;       // 3bp interleaved: 40 p1 + 40 p2 + 40 p3
    // Per-row Amiga display-y (≈ Atari DL-relative scanline) and vertical doubling.
    struct Row { int y; int vdup; };
    static const Row rows[6] = {
        {  56, 2 },   // mode 7: "RESCUE ON FRACTALUS!"  (double height)
        {  96, 1 },   // mode 6: copyright
        { 136, 1 },   // mode 6: STARTING LEVEL
        { 146, 1 },   // mode 6: RANKING LEVEL
        { 170, 1 },   // mode 6: LAST SCORE
        { 180, 1 },   // mode 6: HIGH SCORE
    };
    uint8_t* bmp = (uint8_t*)titleScreenBitmap->data;
    // Blank = pen 0 (black): clear the whole bitmap once.
    for (int i = 0; i < kStride * (int)kH; i++) bmp[i] = 0;

    for (int r = 0; r < 6; r++) {
        const uint8_t* src = (const uint8_t*)mem + kScreenRAM + r * 20;
        for (int c = 0; c < 20; c++) {
            const uint8_t cell = src[c];
            const uint8_t pen  = (uint8_t)((cell >> 6) + 1);          // COLPF0-3 -> pen1-4
            const uint8_t* glyph = (const uint8_t*)mem + kCharset + (cell & 0x3Fu) * 8u;
            for (int gr = 0; gr < 8; gr++) {
                // Double each glyph bit to 2 px (mode 6/7 = 16px/char) via the precomputed table.
                const uint16_t dbl = kDoubleGlyph[glyph[gr]];
                const uint8_t hi = (uint8_t)(dbl >> 8), lo = (uint8_t)(dbl & 0xFF);
                for (int vd = 0; vd < rows[r].vdup; vd++) {
                    const int sy = rows[r].y + gr * rows[r].vdup + vd;
                    if (sy < 0 || sy >= (int)kH) continue;
                    uint8_t* row = bmp + sy * kStride;
                    const int bx = c * 2;                             // 16px char = 2 bytes/plane
                    if (pen & 1u) { row[bx]      |= hi; row[bx + 1]      |= lo; }   // plane1
                    if (pen & 2u) { row[40 + bx] |= hi; row[40 + bx + 1] |= lo; }   // plane2
                    if (pen & 4u) { row[80 + bx] |= hi; row[80 + bx + 1] |= lo; }   // plane3
                }
            }
        }
    }
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
            // Direct $260E->bitplane terrain render (replaces the renderViewportModeD convert):
            // no mem[$1070] round-trip / buffer scan / shadow.  ~2.8x cheaper, plane1 byte-exact.
#ifdef ROF_FLIGHT_PROBE
            extern volatile unsigned long g_fDirect;
            unsigned long _dv0 = rof_subclock(), _dvi = g_isrBeamLines;
#endif
            renderFlightDirect();
#ifdef ROF_FLIGHT_PROBE
            g_fDirect += (rof_subclock() - _dv0) - (g_isrBeamLines - _dvi);
#endif
            g_flightProf.render += (unsigned short)(flight_vbi_tick() - r0);
        }
        else                        renderViewportModeD(0x1000, 48, 47);   // stars/planet: +4 band rows ($1810-$18A0)
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

    // Compass (#2): re-decode the 4 heading cells when flagged (housing/heading rewritten)
    // or on a forced full repaint — targeted 16-longword decode into the title bitmap.
    if (g_compassDirty || cockpitForceFull) {
        decodeCompass();
        g_compassDirty = 0;
    }

    // ---- cockpit region ------------------------------------------------------
    // WRITER-DRIVEN decode (replaces the per-frame full shadow scan): instrument writers
    // register the exact cell spans they changed in the g_ck* registry; we decode only those.
    // On scene entry (cockpitForceFull) the whole region is painted once (the transpiled
    // display_setup built it — not a hooked writer); steady state is span-only, ~0 cost on
    // frames where nothing moved.  Layout note: the cockpit shares the terrain WIDE playfield
    // (48 bytes/DL row) — modeD $350D (4 entries × 2 scan lines) then mode4 $332D (10 × 8),
    // 40 visible cols of each 48-byte row (+4 crop) — all handled by decodeCockpitSpan().
#ifdef ROF_FLIGHT_PROBE
    unsigned long _ckp0 = rof_subclock();
#endif
    if (cockpitForceFull) {
        cockpitForceFull = false;
        decodeCockpitFull();
        // The full paint covers every cell — drop any flags set before it.
        for (int i = 0; i < CK_FLAGS; i++) g_ckFlag[i] = 0u;
#ifdef ROF_FLIGHT_PROBE
        if (rsFlight) g_fCockpitScans++;
#endif
    } else {
        // Walk the dirty-flag array; long-batched so all-clear runs (the common case) skip 4
        // cells at a time.  A set flag → clear it, then decode that single cell.  (Reading the
        // flags through a non-volatile long alias lets the compiler batch the skip; a flag set
        // by the ISR mid-walk that this long missed stays set and is caught next frame.)
        const unsigned long* fl = (const unsigned long*)(const void*)g_ckFlag;
        bool any = false;
        for (int i = 0; i < CK_FLAGS / 4; i++) {
            if (fl[i] == 0u) continue;
            int base = i * 4;
            for (int b = 0; b < 4; b++) {
                if (g_ckFlag[base + b]) {
                    g_ckFlag[base + b] = 0u;
                    decodeCockpitSpan((uint16_t)(0x332Du + base + b), 1u);
                    any = true;
                }
            }
        }
#ifdef ROF_FLIGHT_PROBE
        if (rsFlight && any) g_fCockpitScans++;
#endif
    }
#ifdef ROF_FLIGHT_PROBE
    if (rsFlight) g_fCockpit += rof_subclock() - _ckp0;   // flight-only: cockpitTicks/tdFrames is per-flight-frame
#endif
    if (rsFlight) g_flightProf.renderTot += (unsigned short)(flight_vbi_tick() - profR0);
}

void RescueOnFractalus::shutdown()
{
    delete standbyCopper; standbyCopper = nullptr;
    delete planetCopper; planetCopper = nullptr;
    delete flightCopper; flightCopper = nullptr;
    for (int i = 0; i < 2; i++) { delete doorsCopper[i]; doorsCopper[i] = nullptr; }
    delete tunnelCopper; tunnelCopper = nullptr;
    delete titleScreenCopper; titleScreenCopper = nullptr;
    delete emptyCopper;   emptyCopper   = nullptr;
    PlatformAmiga::audioShutdown();
    delete titleBitmap;   titleBitmap   = nullptr;
    delete terrainBitmap; terrainBitmap = nullptr;
    delete terrainBitmapBack; terrainBitmapBack = nullptr;
    delete cockpitBitmap; cockpitBitmap = nullptr;
    delete tunnelBitmap;  tunnelBitmap  = nullptr;
    delete titleScreenBitmap; titleScreenBitmap = nullptr;
    delete leftPost;      leftPost      = nullptr;
    delete rightPost;     rightPost     = nullptr;
    delete nullSprite;    nullSprite    = nullptr;
    delete energyIndicatorSprite;   energyIndicatorSprite   = nullptr;
    delete altimeterSprite; altimeterSprite = nullptr;
    delete altimeterShipSprite; altimeterShipSprite = nullptr;
    delete flLeftPost;  flLeftPost  = nullptr;
    delete flRightPost; flRightPost = nullptr;
    delete flLeftTri;   flLeftTri   = nullptr;
    delete flRightTri;  flRightTri  = nullptr;
    for (int c = 0; c < 3; c++) { delete starSprite[c]; starSprite[c] = nullptr; }
}
