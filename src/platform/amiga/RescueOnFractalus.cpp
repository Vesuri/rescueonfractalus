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
#include "../../cpu/m68k_math.h"
#include "framework/CopperList.h"
#include "framework/Bitmap.h"
#include "framework/Palette.h"
#include "framework/Sprite.h"
#include "RescueOnFractalus.h"
#include "PlatformAmiga.h"
#include "../../gen/mem.h"           // MEM_<name> named Atari memory offsets
#include "FlightProf.h"   // per-frame VBI-count profiler (g_flightProf / flight_vbi_tick)

// Native handler functions — see NativeHandlers.cpp and SfxPlayer.cpp.
extern "C" void vbi_attract_timer_native(void);                  // $52D7: timer cascade
extern "C" void update_indicator_blink_native(void);           // $4131: cockpit blink
extern "C" void startup_init_native(void);                      // $3FFA: cockpit digit update
extern "C" void launch_anim_dispatch_native(void);              // $5367: ring ($0088) vs door scroll ($008A)
extern "C" volatile uint8_t g_tunnelFieldDirty;                // set when draw_ring_frame_step draws into $1000
extern "C" volatile uint8_t g_tunRowLo, g_tunRowHi;            // outer row extent of the expanding black clear
extern "C" volatile uint8_t g_tunInRowLo, g_tunInRowHi;        // inner (previous-outer) row extent
extern "C" volatile uint8_t g_tunColLpx, g_tunColRpx;          // outer left/right PIXEL cols
extern "C" volatile uint8_t g_tunInColLpx, g_tunInColRpx;      // inner left/right PIXEL cols
extern "C" volatile uint8_t g_tunBandMode;                     // 1 = band decode, 0 = full extent (reveal)
extern "C" volatile uint8_t g_boostStarsDirty;                 // set by fill_region_2000; boost stars decode-on-change gate
extern "C" volatile unsigned short g_starScrollGen;            // rof_native.c: bumped per scrolled star row
extern "C" volatile uint8_t g_activeVbi;                       // 0=none 1=standby($52D7) 2=flight($4FF5); read by game_vbi_isr

// The genuine transpiled launch cinematic ($5F1D, src/gen/rof_gen.c): boot_standby_launch_driver()'s
// frame-wait spin loops call platform_render_frame, which renders then waits for a real VBI.
extern "C" void boot_standby_launch_driver(void);

// Black-until-ready reveal gate, latched on at boot_standby_launch_driver entry (rof_native.c); renderFrame
// holds the EmptyCopperList on screen until it sets, then switches to the real lists.
extern "C" volatile unsigned char g_standbyRevealReady;
extern "C" volatile unsigned char g_doorScrollFieldDirty;   // rof_native.c: LEVEL digit rewritten (door scroll)
// BREAK/Restart black hold: set by the g_restartJmp handler in run() the instant a Backspace/BREAK
// restart is taken.  Until the level-selector card ($53CC) is genuinely rebuilt (VVBLKI=$53CC, title
// text in $365B, DMA shadow $022F back on), renderFrame keeps the EmptyCopperList (black) on screen —
// otherwise the STALE flight/launch copper + bitplanes flash for a beat (garbage from the launch
// sequence, black+brown from flight), and rsLaunched (stale terrain-scroll/vbi flags left in mem[])
// can even re-install the doors/tunnel copper over old data before the card exists.
extern "C" volatile unsigned char g_restartHoldBlack = 0;
// Door-field-ready gate, latched on in boot_standby_launch_driver once the doors/dots/LEVEL field has been
// drawn into $2000 but BEFORE delay_loop_c2_to_c9 ramps the green colour $0071 (rof_native.c).
// render() decodes $2000 -> viewportBitmap once when this rises, so the door pixels exist before
// the fade and the per-frame color03 ramp shows the dark->bright green build on them.
extern "C" volatile unsigned char g_doorFieldReady;
// Screen-RAM dirty flags: render() scans the title ($32B7) + cockpit ($332D mode4 / $350D
// modeD) regions only when these are set, instead of re-scanning all ~580 cells every
// frame.  During the static doors/standby phases nothing changes, so the scan was pure
// overhead (~7 ms/frame, the dominant door-cinematic cost).  The title region is driven by
// g_titleToRender (below); g_cockpitDirty by update_cockpit_digits / lock_on_indicator_tick
// at their store sites.  These are force-set at phase transitions in deriveRenderSignals() so
// the initial build (by the transpiled boot_standby_launch_driver, not those writers) + flight updates are
// never missed.

// Top-bar title text: no shadow / per-cell compare.  Two counts drive the re-decode:
//   g_titleToRender = how many of the 20 title cells to (re)paint from screen RAM this frame;
//                     -1 means "nothing to do" (idle).  Set by the genuine $782A title writer
//                     (copy_title_text_block_to_screen) via the platform_title_changed() hook.
//   titleRendered   = how many cells were painted last time (member), so shrinking the count
//                     blanks the now-unwanted trailing (titleRendered - g_titleToRender) cells.
extern "C" volatile int g_titleToRender = 20;   // >=0 → paint that many; -1 → idle

// Title Screen (scene 3b) value-cell dirty range: while the Title Screen is up the only
// cells that change are the STARTING LEVEL digit (level select) and the LAST/HIGH SCORE
// digits (game-over build).  Their writers report each changed cell here; render() redraws
// exactly [g_titleCellLo..g_titleCellHi] so the screen never fully repaints (no flash).
// Empty range = lo>hi.  Cell index = Atari screen addr - $365B (0..119).  Single ints
// (atomic on the 68000); the writers run on the main thread, render() clears the range.
extern "C" volatile int g_titleCellLo = 120, g_titleCellHi = -1;

// The BCD digit writer (plot_char_bounded, rof_native.c) and the level-digit renderer
// (setup_initials_ptr $5A63) report the Title-Screen-region cell span they write here via
// PlatformAmiga::titleScreenDirty → this.  Off-screen / cockpit digit dests (e.g. $32C5,
// $37F5) fall outside the window and are clamped away.
extern "C" void rof_title_screen_dirty(unsigned short addr, unsigned char nCells)
{
    int lo = (int)addr - 0x365B;
    int hi = lo + (int)nCells - 1;
    if (lo < 0) lo = 0;
    if (hi > 119) hi = 119;
    if (lo > hi) return;                       // span entirely outside the $365B window
    if (lo < g_titleCellLo) g_titleCellLo = lo;
    if (hi > g_titleCellHi) g_titleCellHi = hi;
}

// ---- cockpit per-instrument dirty flags -------------------------------------
// The cockpit ($332D mode4 / $350D modeD) is decoded WRITER-DRIVEN by instrument: the only
// instruments that change in flight are the digits, the lock-on indicator and the two dial bars,
// so each writer raises ONE boolean and render() decodes just that instrument's cells — no array,
// no scan/walk.  Idle frames (the common case) cost three boolean reads.  Flags are single bytes
// (atomic on the 68000) so writers on the main thread (digits) and the VBI ISR (lock-on, dials)
// need no Disable() (an early Disable()/Enable()-in-ISR version wedged interrupt delivery — incl.
// the keyboard ISR that starts the game).  render() clears-then-decodes; a write racing the clear
// is caught next frame.  Other instruments (status lights, scope, scanner) are static after the
// scene-entry full paint until a writer is hooked — see docs/cockpit-render-plan.md "TODO".
extern "C" volatile unsigned char g_ckDigits = 0;   // score/kills/quota digits + DL-stride (startup_init)
extern "C" volatile unsigned char g_ckLockon = 0;   // lock-on indicator $3491-$3497
extern "C" volatile unsigned char g_ckDial   = 0;   // thrust/danger-alt dial bars (draw_object_column)
// The two dial bars are the one instrument without a fixed cell span — their cells come from the
// $4581 column table — so the dial alone needs per-cell precision (a fixed-box decode re-paints
// dozens of static cells every time one bar cell moves, which measured ~4x worse).  Per-cell
// dirty flags over the mode4 region, walked ONLY when g_ckDial is set (so idle frames still cost
// nothing).  Single-byte stores → lock-free vs the VBI writer; clear-then-decode, race re-set
// caught next frame.
static const int CK_DIAL_N = 0x350D - 0x332D;   // 480 mode4 cells ($332D..$350C)
static volatile unsigned char g_ckDialFlag[CK_DIAL_N] __attribute__((aligned(4))) = {};

// The shared dial writer (draw_object_column, rof_native.c) reports each changed bar cell here via
// PlatformAmiga::cockpitDirty → this.  Mode4 only (the bars live in $332D..$350C).
extern "C" void rof_cockpit_dial_dirty(unsigned short addr)
{
    if (addr < 0x332Du || addr >= 0x350Du) return;
    g_ckDialFlag[addr - 0x332Du] = 1u;
    g_ckDial = 1u;
}
#ifdef ROF_FLIGHT_PROBE
extern "C" unsigned long rof_subclock(void);
extern "C" volatile unsigned long g_fConvert, g_isrBeamLines;  // Stage-0 convert-pass probe
extern "C" volatile unsigned long g_fCockpit, g_fCockpitScans;
extern "C" volatile unsigned long g_ckFullTicks, g_ckFullCount;  // decodeCockpitFull one-shot timing
extern "C" volatile unsigned short g_ckFullVbi[4] = {0,0,0,0};       // g_vbiCount at each ckFull call
// Boost-return probe: last-installed copper id (1=title 2=standby 3=planet 4=flight 5=tunnel
// 6=doors 7=empty 8=boost-handoff-hold) + the live boost signals, sampled per render() to
// confirm phase routing.  g_boostHandoffHoldFrames counts the T6 handoff-hold frames (proves
// the reverse-tunnel->standby window exists and the guard catches it).
extern "C" volatile unsigned char g_boostRet = 0, g_boostVp = 0, g_liveCopper = 0;
extern "C" volatile unsigned long g_boostHandoffHoldFrames = 0;
// Boost-viewport decode-cost probe (item 2, decode-consume): count decode events, to confirm the
// decode-on-change gating runs and quantify the win vs the old always-full decode EVERY frame.
// g_bStarDec = $2000 stars decodes (dirty-gated, ~2/cinematic); g_bTunDec = reverse-tunnel full
// decodes (per-frame — the field is a VBI-written multi-writer reveal, unsafe to gate).
extern "C" volatile unsigned long g_bStarDec = 0, g_bTunDec = 0;
extern "C" volatile unsigned short g_starEntryVbi = 0;              // vbi at first rsStars viewport decode
extern "C" volatile unsigned long  g_starEntryTicks = 0, g_starEntryIsr = 0; // its cost
extern "C" volatile unsigned short g_starSprVbi = 0;
extern "C" volatile unsigned long  g_starSprTicks = 0;             // first buildStarSprites cost
extern "C" volatile unsigned long  g_starGroups = 0;              // non-skipped groups on the entry decode
extern "C" volatile unsigned long  g_starClrTicks = 0;
// starVblankUpdate exit-beam probe: the max scanline at which the control-word write finished
// (must beat the sprite's ~line-25 control fetch or channel-2's star VSTOP reads stale → gauge drop).
extern "C" volatile unsigned short g_starVbiExitLine = 0, g_starVbiExitLineAtVbi = 0;
extern "C" volatile unsigned short g_starVbiLateCount = 0, g_starVbiCalls = 0;
// Gauge-drop detector: ISR firings where the copper operand (starWindow) points at a slot whose
// control words were NOT (re)written because starVbiRows was reset to 0 by a 2nd perFrameWork call.
extern "C" volatile unsigned short g_starDropRisk = 0, g_starDropRiskAtVbi = 0;
extern "C" volatile unsigned short g_pfwStarCalls = 0, g_pfwStarZeroAfterAdv = 0;
extern "C" unsigned short platform_frame_count(void);
#endif
// Compass (#2): the heading cells $32E3-$32E6 (mode-4 line below the title) — flagged by
// platform_compass_changed() from the housing init (game_sub_4606) / heading updater ($3FDE).
extern "C" volatile unsigned char g_compassDirty = 1;
// The genuine boot chain (src/gen/rof_gen.c): station_init = attract ($195D, returns on
// START); game_entry = $3CDE -> game_main_loop (game-display setup -> boot_standby_launch_driver
// cinematic -> flight loop, never returns).  g_quitJmp = the __builtin_setjmp buffer
// (defined in PlatformAmiga.cpp) the pump longjmps to on quit, unwinding the transpiled chain.
extern "C" void station_init(void);
extern "C" void game_entry(void);
extern "C" void game_main_loop(void);     // $3D48: DL/sound/PMG init -> scoreboard/standby
#ifdef ROF_FIXED_RNG
extern "C" uint32_t rof_lfsr_state;       // PlatformAmiga.cpp — re-pinned by FIXED_RNG builds
#endif
extern "C" void audio_timer_setup(void);  // $712D: clear POKEY AUDF, AUDCTL=$60
extern "C" void sfx_engine_reset(void);   // $5433: zero SFX voice slots
extern "C" void rof_check_restart(void);  // pump-exit gate: quit / BREAK-restart (PlatformAmiga.cpp)
extern "C" void* g_quitJmp[];   // definition (sized) lives in PlatformAmiga.cpp
extern "C" void* g_restartJmp[];   // BREAK/Restart re-entry buffer (defined in PlatformAmiga.cpp)

extern "C" volatile uint8_t mem[65536];
// Which terrain field half renderFlightDirect displays (defined in rof_native.c, set by
// game_main_loop before each ds_frame): 0 = display half (offset 0), 1 = back half (offset $30).
extern "C" volatile unsigned char g_flightRenderHalf;
// Death-cinematic teardown flag (defined in PlatformAmiga.cpp): set when the game blanks ANTIC
// DMA ($4F76 DMACTL $D400=0) so renderFrame shows the solid COLBK fade.  See death-cinematic memory.
extern "C" volatile unsigned char g_flightBlank;

// Lookup table: byte → 16-bit doubled glyph pattern (each bit → 2 pixels).
// Filled once in initialize(); used by title render for mode-6 1bpp doubling.
static uint16_t kDoubleGlyph[256];

// Starfield glyph tables: byte → 16-bit sprite word, rendering each star sub-position as a
// 4-lores-px dot at its FAITHFUL offset (matches the Atari's SIZEP=$03 quad players, whose set
// bits are 4 colour clocks = 4 Amiga px wide — measured against atari033.png).  The star
// pattern only ever sets bits 7/5/2/0 ($80/$20/$04/$01), which on a 32-cc quad player sit at
// offsets 0/8/20/28 cc.  A 16 px hardware sprite can't span that, so each Atari player is drawn
// as TWO adjacent sprites (low = px 0-15, high = px 16-31, giving the full 32 cc span at 1:1):
//   kStarGlyphLo — bits 7/5 → low  sprite px 0-3 / 8-11
//   kStarGlyphHi — bits 2/0 → high sprite px 4-7 / 12-15  (offsets 20/28 cc minus the 16 px base)
// bit15 = leftmost sprite pixel.  Filled once in initialize().
static uint16_t kStarGlyphLo[256];
static uint16_t kStarGlyphHi[256];

// Precomputed decode tables (filled in initialize()) — convert one source byte
// straight to its output bitplane bytes, replacing the per-byte bit loops.
//   mode-D (2bpp, stars/planet viewport): byte = 4 pixels (2 bits) → 8 Amiga px.
//     kModeDP1[s] = plane1 (colour bit0 of each pixel), kModeDP2[s] = plane2 (bit1).
// Non-static + extern "C" so the alien-creature mirror (ROF_PLOT_ALIEN in rof_native.c) can decode
// the creature's mode-D field bytes into plane1/plane2 with the identical mapping this file uses.
extern "C" {
uint8_t kModeDP1[256];
uint8_t kModeDP2[256];
}
// Windscreen-bottom band (flight rows 43-46) decode.  The band mode-D field (mem[$1074+43*96],
// written per frame by game_sub_451d) holds: value 3 = grey windscreen frame (the dominant
// "middle"), value 0 = the L/R edge regions (terrain body colour, behind the corner-triangle
// sprites), value 1 = the salmon wing-clearance bars, value 2 = the centre marker.  Instead of
// the terrain palette carrying the grey (a color03 poke), only the grey frame (value 3) is put on
// the UNUSED third bitplane -> color04 (cockpit grey).  Values 0/1/2 are plane3 HOLES taking the
// terrain palette color00-03 (so the L/R edges show color00 = terrain body and the salmon bars
// show color01 = sky, both fading salmon->brown WITH the terrain).  kBandP1/2/3[s] = the
// plane1/2/3 byte for source byte s.
static uint8_t kBandP1[256];
static uint8_t kBandP2[256];
static uint8_t kBandP3[256];
static uint8_t kBandOW[256];   // = kBandP1[s] | kBandP2[s]: the terrain-overwriting (bar|marker)
                               // pixels.  ow==0 for pure grey-frame / L-R-edge bytes (the band
                               // majority) -> the plane1/2 RMW is a no-op there and can be skipped.
// Row -> byte offset within the flight bitmap (120 bytes/scanline = plane1 40 + plane2 40 +
// plane3 40).  The 68000 has no fast multiply, so the per-column horizon plotter (and the
// direct-to-plane2 terrain rasterizer in rof_native.c) index this instead of computing scan*120.
// Covers terrain rows 0-42 + the windscreen band rows 43-47.  extern "C" so rof_native.c can use it.
// Row-offset (row × width) lookup tables — the 68000 has no cheap multiply, so a `row * stride`
// with a NON-sequential row (e.g. the flight plot macros, where the row is computed from x/y) is
// a __mulsi3 soft-multiply; these replace it with an index.  (For a SEQUENTIAL loop, walk a
// pointer by the stride instead — cheaper still than a table read.)  kRow120 is the interleaved-
// terrain scanline stride (3bp × 40 B); kRow40 / kRow80 are one / two mode-D planes (the rescue-
// figure overlay: mask stride 40, interleaved figure planes stride 80).  Used by the flight
// terrain plot macros (rof_native.c) and a few viewport/composite/HUD paths here.  [0..47].
extern "C" const uint16_t kRow120[48] = {
       0,  120,  240,  360,  480,  600,  720,  840,  960, 1080, 1200, 1320,
    1440, 1560, 1680, 1800, 1920, 2040, 2160, 2280, 2400, 2520, 2640, 2760,
    2880, 3000, 3120, 3240, 3360, 3480, 3600, 3720, 3840, 3960, 4080, 4200,
    4320, 4440, 4560, 4680, 4800, 4920, 5040, 5160, 5280, 5400, 5520, 5640,
};
extern "C" const uint16_t kRow40[48] = {
       0,   40,   80,  120,  160,  200,  240,  280,  320,  360,  400,  440,
     480,  520,  560,  600,  640,  680,  720,  760,  800,  840,  880,  920,
     960, 1000, 1040, 1080, 1120, 1160, 1200, 1240, 1280, 1320, 1360, 1400,
    1440, 1480, 1520, 1560, 1600, 1640, 1680, 1720, 1760, 1800, 1840, 1880,
};
extern "C" const uint16_t kRow80[48] = {
       0,   80,  160,  240,  320,  400,  480,  560,  640,  720,  800,  880,
     960, 1040, 1120, 1200, 1280, 1360, 1440, 1520, 1600, 1680, 1760, 1840,
    1920, 2000, 2080, 2160, 2240, 2320, 2400, 2480, 2560, 2640, 2720, 2800,
    2880, 2960, 3040, 3120, 3200, 3280, 3360, 3440, 3520, 3600, 3680, 3760,
};
// 2-bit intra-byte column mask (4 columns/byte): used for both the plane1 skyline edge and the
// plane2 dot write.  A value-2/3 mode-D pixel decodes (kModeDP2) to exactly these bits.
extern "C" const uint8_t kColMask4[4] = { 0xC0u, 0x30u, 0x0Cu, 0x03u };
// Plane2 base of the off-screen buffer the terrain rasterizer should OR its dots into this frame
// (= back->data + 40).  Set by flightKickBackClear once the buffer + its clear are committed; null
// on the first flight frame (rasterizer then skips the direct write).  See renderFlightDirect.
extern "C" uint8_t* g_flightDotPlane = nullptr;
// "Terrain was freshly drawn since the last flight render" flag.  terrain_draw_frame_core
// (rof_native.c) sets it each time it draws; renderFlightDirect checks + clears it and, when
// it is clear, SKIPS the clear+repaint and leaves the last terrain frame on screen.  This is
// what keeps the plane2 dots (and silhouette) alive during a rescue PAUSE (systems off): the
// main loop is parked in pilot_render's hold loop so no terrain_draw runs, and re-clearing the
// buffer each yielded frame would drop the dots (renderFlightDirect only ever refills plane2
// from the rasterizer's live draw).  Init 1 so the first flight frame paints.  See renderFlightDirect.
extern "C" volatile int g_flightTerrainFresh = 1;
// Object plane1 overlay (post-fill).  Ground objects (gun emplacement / downed pilot / enemy
// fire) are drawn value-3 (COLPF2) = plane1+plane2 for their highlight pixels (terrain_plot_object
// variant A whole-body + variant B's 2x2 cross; variant B bodies stay value-2 = plane2 only).  We
// can't set plane1 during the draw: the sky-fill blit (blitterFillUp) would treat a stray plane1
// bit in the terrain body as a fill seed and paint a vertical sky-coloured streak above it.  So
// terrain_plot_pixel records the value-3 LOW bit here (a separate plane1-shaped scratch); after the
// sky fill, renderFlightDirect ORs it into the back buffer's plane1 over the dirty scanline range,
// clearing as it applies.  Sized like one plane (47 rows x 120 stride) so the plot reuses kRow120.
static uint8_t s_flightObjP1[47 * 120];
extern "C" uint8_t* g_flightObjP1 = nullptr;      // = s_flightObjP1 during flight; null otherwise
extern "C" int g_objRowLo = 47, g_objRowHi = -1;  // dirty scanline range in s_flightObjP1 (empty)
// Windscreen-band composite cache (see the band overlay at the end of renderFlightDirect).  The
// band is re-composited every frame because the whole 47-row buffer is cleared and the terrain
// repainted under it — but the SOURCE it decodes barely moves: measured (BAND_SHAPE probe,
// 2026-08-05, 420 frames) only ~0.9 of 160 field bytes change per frame, all of them in row 45
// (the wing-clearance bar); rows 43/44/46 changed exactly twice = the flight-entry transient.
// So cache the DECODED bytes and refresh only what changed:
//   * change detection = 40 long compares of the field against a per-half shadow (writer-agnostic,
//     so no assumption about who writes the field and no coupling into the 6502 twins).
//   * plane3 (the static grey frame, ~130 of 160 bytes) becomes a straight long COPY, no per-byte
//     table lookups at all.  A byte-for-byte copy is endianness-neutral, unlike a value alias.
//   * planes 1&2 are RMW'd only over each row's cached ow!=0 range (measured 34 bytes: row 44
//     [19,20], row 45 [5,34], row 46 [19,20]) instead of testing all 160.
// The field is written by game_sub_451d from the flight VBI ISR (update_terrain_scanline_proj), so
// the reads stay volatile.  A long read torn by the ISR is harmless and self-correcting: the shadow
// holds exactly the bytes read, so the next frame's compare sees the settled value and re-decodes
// (the pre-cache code had the same one-frame exposure, re-reading the field every frame).
static uint32_t s_bandShadow[2][10 * 4];                  // field bytes last seen, per half
static uint32_t s_bandP3c[2][10 * 4];                     // decoded plane3 (long-copied out)
static uint8_t  s_bandP1c[2][40 * 4], s_bandP2c[2][40 * 4], s_bandOWc[2][40 * 4];
static signed char s_bandOwLo[2][4] = {{40,40,40,40},{40,40,40,40}};
static signed char s_bandOwHi[2][4] = {{-1,-1,-1,-1},{-1,-1,-1,-1}};
// ...and the plane3 long copy itself is skipped when the destination already holds the right bytes.
// Same invariant the crosshair one-shot relies on (see s_p3Clean): nothing else in the frame writes
// plane3 rows 43-46, so once a display buffer holds a given decoded band it keeps holding it.  Per
// FIELD HALF, version each of the 4 rows' decoded plane3 (bumped whenever the decode cache changes);
// per DISPLAY BUFFER, remember which half and which versions are actually painted in it.  Measured
// shape (BAND_SHAPE, 1cab6f4): rows 43/44/46 changed exactly twice in 420 frames and row 45 carries
// the wing-clearance bar — so this normally skips 30 of the 40 long copies, and all 40 when the bar
// is steady.  s_bandP3SeenHalf = -1 means "unknown, repaint": the initial state, and what the
// one-shot plane3 clear re-arms (that clear wipes rows 43-46 along with the rest of the plane).
static uint16_t    s_bandP3Ver[2][4]   = {{0,0,0,0},{0,0,0,0}};   // [half]   content version per row
static uint16_t    s_bandP3Seen[2][4]  = {{0,0,0,0},{0,0,0,0}};   // [buffer] version painted per row
static signed char s_bandP3SeenHalf[2] = { -1, -1 };              // [buffer] which half that was
#ifdef ROF_BAND_VERIFY
extern "C" volatile unsigned long g_bandCalls = 0, g_bandMismatch = 0, g_bandFirstBad = 0,
                                  g_objLeak = 0;
#endif
// ...and the dirty BYTE-COLUMN range (0..39) of the same scratch.  Ground objects / enemy fire are
// narrow, so the row range alone left the apply loop scanning all 40 bytes of each dirty row to find
// them: measured 577 nonzero of 31840 bytes scanned = 1.8% (BAND_SHAPE probe, 2026-08-05).  Tracked
// as a bounding box by the same three writers that set the bytes (ROF_PLOT_DOT_P1, laser_dot_column,
// laser_dot_run), so every nonzero byte is inside it and the narrowed apply still clears them all.
extern "C" int g_objColLo = 40, g_objColHi = -1;

// Rescue-figure scratch overlay (43 mode-D rows × 40 plane bytes): the ONLY figure pixels
// plot_clipped_pixel actually drew (mirrored via ROF_PLOT_FIG in rof_native.c) — plane1, plane2,
// and the opaque-pixel mask.  Decoding the raw mode-D field instead would splatter the stale
// non-figure data in that shed region (it corrupted the viewport).  g_figRowLo/Hi = dirty rows.
// The overlay + clean-terrain snapshot are CHIP-RAM Bitmaps so the per-frame composite (erase +
// cookie-cut draw) runs on the BLITTER via Bitmap::combineWithMask instead of a CPU 32-bit loop
// (was ~136 ms/step, chip-RAM/ISR-bound).  Layout:
//  - s_figBmp: 2-plane INTERLEAVED (plane1 @ +0, plane2 @ +40, 80-byte rows) = the figure source;
//    g_figP1/P2 point at its two planes and rof_native.c's plot macros write them at row stride 80.
//  - s_figMaskBmp: 1-plane opaque mask (40-byte rows); g_figM points at it.
//  - s_cleanBmp: 3-plane interleaved, identical layout to terrainBitmap = the composite background.
// Allocated in initialize(); the g_fig* pointers stay null until then (the plot macros no-op on null).
static Bitmap* s_figBmp     = nullptr;   // 320x43, 2bp interleaved (figure planes)
static Bitmap* s_figMaskBmp = nullptr;   // 320x43, 1bp (opaque mask)
static Bitmap* s_cleanBmp   = nullptr;   // 320x94, 3bp interleaved (clean-terrain snapshot)
extern "C" uint8_t* g_figP1 = nullptr;   // -> s_figBmp plane1 (offset 0)
extern "C" uint8_t* g_figP2 = nullptr;   // -> s_figBmp plane2 (offset 40)
extern "C" uint8_t* g_figM  = nullptr;   // -> s_figMaskBmp
extern "C" int g_figRowLo = 99, g_figRowHi = -1;   // empty
extern "C" int g_figColLo = 40, g_figColHi = -1;   // dirty byte-column extent (0..39) — narrow-rect composite
// Rescue-pause dirty-rect state.  The terrain is FROZEN during the walk-to-airlock pause, so
// instead of re-rendering the whole viewport each frame we snapshot the clean frozen terrain once
// (all 3 planes of the 47 interleaved rows) and, per frame, only ERASE the previous figure's row
// box (restore plane1+plane2 from the snapshot) and draw the new figure.  Because the viewport is
// double-buffered we track per-buffer state: whether the buffer has been seeded with clean terrain
// yet, and the row box its current figure occupies (to erase next time that buffer comes round).
// The clean snapshot itself is s_cleanBmp (declared above, chip) — combineWithMask reads it as the
// composite background, so it must be chip and layout-identical to the terrain buffers.
static bool    s_cleanValid = false;
static bool    s_bufSeeded[2] = { false, false };
static int     s_boxLo[2] = { 99, 99 }, s_boxHi[2] = { -1, -1 };
// Per-buffer WORD-column extent of the figure last composited into each buffer (for the
// narrow-rect erase, alongside the row box above).  Empty = lo>hi.
static int     s_boxColLo[2] = { 20, 20 }, s_boxColHi[2] = { -1, -1 };
// While set, flightKickBackClear does NOT wipe the off-screen buffer (the dirty-rect needs it to
// retain the frozen terrain) but still re-arms the dot/object plane pointers.  Set/cleared by
// renderFlightDirect.
static bool    s_flightRescuePause = false;
// Resume-frame terrain-dot recovery, keyed on the $3E (clear_colors_done) latch — NOT on
// rescueFigure.  $3E is set ONCE when the rescue starts and cleared ONCE when it ends (measured
// clean 01->00 single edge, diag_rescue.gdb 2026-07-21), whereas rescueFigure ($3E && $3D>=3) also
// goes false on the mid-zoom frames where pilot_render drops $3D to 2 ($7a37) — firing a
// rescueFigure-keyed recovery DURING the zoom, which corrupts it (user-observed).  So we detect the
// true rescue END as the $3E nonzero->zero edge and latch a one-shot restore that runs on the first
// terrain-rendering frame at/after it (survives a possible !g_flightTerrainFresh hold on the exact
// edge frame).  s_prevRescueActive tracks "$3E was nonzero last frame".
static bool    s_prevRescueActive   = false;
static bool    s_resumeRestorePend  = false;
// One-shot latched at the SAME $3E nonzero->zero (systems-back-on) edge: on resume, restore clean
// terrain into BOTH terrain buffers AND clear the figure overlay, so a subsequent rescue pause can't
// display a stale figure (esp. the alien creature) for a frame before its own figure composites.
// Keyed on the true edge (not every non-rescueFigure frame) so the pilot approach's mid-zoom $3D
// dips — where rescueActive stays set — never trigger it.  Fires for ANY rescue (alien or pilot).
static bool    s_resumeClearPend    = false;
// Per terrain buffer (0 = terrainBitmap, 1 = terrainBitmapBack): is its plane3 already the content
// renderFlightDirect wants?  plane3 has exactly two writers in the terrain region and NEITHER needs
// a cleared canvas: rows 0-42 hold only the targeting crosshair, whose geometry is a compile-time
// constant (visibility is a copper palette swap, not a draw skip), and rows 43-46 are overwritten
// wholesale by the band composite's plane3 long copy.  So the per-frame plane3 clear was pure cost
// (11 beam ticks/painted frame of CPU stall) and BOTH it and the crosshair draw are now one-shot per
// buffer, armed on the flight rising edge in deriveRenderSignals — the one place a foreign scene's
// bits can reach plane3, since terrainBitmap is shared with the doors/tunnel/planet viewport decode.
// (The rescue paths' 3-plane copies from s_cleanBmp cannot dirty it: that snapshot is itself a live
// flight buffer, so its plane3 is already crosshair + band and nothing else.)
static bool    s_p3Clean[2]         = { false, false };
// Called by the terrain draw (rof_native.c) before its first dot write, to ensure the kicked
// off-screen-buffer clear has finished (the dots OR into freshly-zeroed plane2).
#ifdef ROF_BLIT_SHAPE
// ---- blitter-STALL attribution probe (`make PROBES=1 BLIT_SHAPE=1`, amiga/blit_shape.gdb) ----
// The PC profile puts ~5% of the flight frame inside bW_waitUntilBlitterNotBusy /
// processBlitterQueue / blitterDrain, but a PC sample cannot say WHICH call site is stalling —
// and that is the only thing that matters, because the fix for a stall is to give the CPU work
// to do during it (or to split the blit), which is a per-site decision.  So: bracket each wait
// in the flight path and tally its beam-ticks separately.  ISR beam-lines are subtracted (a
// blitter wait very often spans a VBI firing, which would otherwise be counted as stall).
extern "C" volatile unsigned long g_bwDotClear = 0, g_bwClearCopy = 0, g_bwSkyFill = 0,
                                  g_bwPendClear = 0, g_bwFlip = 0, g_bwCalls = 0,
                                  g_bwP3Clear = 0;
#define BW_AT(acc, stmt) do { unsigned long _t = rof_subclock(), _i = g_isrBeamLines; \
    stmt; unsigned long _d = rof_subclock() - _t, _di = g_isrBeamLines - _i; \
    (acc) += (_d > _di) ? (_d - _di) : 0; } while (0)
#else
#define BW_AT(acc, stmt) do { stmt; } while (0)
#endif

// Waits for the dot-side-buffer clear kicked at the end of the previous renderFlightDirect.
// Called from terrain_draw_frame_core BEFORE the first rasterizer dot lands.
extern "C" void rof_flight_wait_dotclear(void) {
#ifdef ROF_BLIT_SHAPE
    BW_AT(g_bwDotClear, AmigaHardware::blitterWait());
    g_bwCalls++;
#else
    AmigaHardware::blitterWait();
#endif
}
// Edge-plot height->plane1-row-byte-offset table: kHeightRowOff[h] = kRow120[clamp(150-h,0,46)].
// Folds the per-column "scanline = 150-h, clamp to the terrain rows" arithmetic out of the
// skyline plot loop (a pure table index), so the loop has no per-column clamp branches — used by
// both the C reference edgePlotCore and the hand-asm flight_edge_plot_asm.  extern "C" so the asm
// can xref it; built once (it depends only on kRow120, not on per-frame state).  The clamp is row
// 46 (not 42) so a low horizon lets the terrain silhouette extend into the windscreen band rows
// 43-46 — the band's L/R edges then show real terrain (the sky fill writes rows 0-45, seed 46).
extern "C" uint16_t kHeightRowOff[256];
uint16_t kHeightRowOff[256];
static bool kHeightRowOffBuilt = false;
static void buildHeightRowOff() {
    for (int h = 0; h < 256; h++) {
        int scan = 150 - h;
        if (scan < 0) scan = 0; else if (scan > 46) scan = 46;
        kHeightRowOff[h] = kRow120[scan];
    }
    kHeightRowOffBuilt = true;
}
// Dot-plot row-offset table for the rasterizer's inner DRAW/draw_dot (TerrainRasterizeAssembler.s):
// kDrawDotRowOff[oldMax] folds the ROF_PLOT_DOT height gate for the *previous* top (oldMax) —
// _sc = 150-oldMax, the off-display reject ((unsigned)_sc >= 47) and the $6b reset-floor skip
// (_sc == 43) — plus the kRow120[_sc] lookup into ONE table read indexed by oldMax.  Entries that
// fail the gate hold the sentinel $FFFF (bit15 set) so the asm rejects with a single `bmi`.  Byte
// offsets kRow120[0..46] are 0..5520 (positive), so they never collide with the sentinel.  Built
// once at initialize() (depends only on kRow120) — the rasterizer runs before renderFlightDirect,
// so this must exist before the first flight frame (unlike kHeightRowOff, built lazily there).
extern "C" uint16_t kDrawDotRowOff[256];
uint16_t kDrawDotRowOff[256];
static void buildDrawDotRowOff() {
    for (int m = 0; m < 256; m++) {          // m = oldMax (the previous column top)
        int sc = 150 - m;
        if ((unsigned)sc < 47u && sc != 43)  // matches ROF_PLOT_DOT's _sc gate exactly
            kDrawDotRowOff[m] = kRow120[sc];
        else
            kDrawDotRowOff[m] = 0xFFFF;       // off-display / reset-floor -> skip
    }
}
// Dot-plot COLUMN tables for the rasterizer's inlined DRAWDOT (TerrainRasterizeAssembler.s).
// ROF_PLOT_DOT's per-plot column work is: _ac = col-48, gate (unsigned)_ac < 160, plane byte
// offset _ac>>2, pixel mask kColMask4[_ac&3].  All three are pure functions of the column, so
// they fold into two tables indexed by the RAW column (48 is a multiple of 4, so no shifting is
// needed to keep _ac&3 == col&3).  kDotColMask doubles as the range gate: it is 0 outside
// [48,208), a value no real 2-bit mask can take, so the asm rejects an off-viewport column with
// the same `move.b` that fetches the mask.  That replaces the per-plot
// `cmp #208 / sub #48 / and #3 / add / lsr #2 / move #$C0 / lsr d0` chain (13 instructions,
// ~104 cycles, one of them a variable-count shift) with two table reads and an add (~62).
// Built once at initialize() next to kDrawDotRowOff — the rasterizer runs before the first
// renderFlightDirect, so both must exist by then.
extern "C" uint8_t kDotColMask[256];
extern "C" uint8_t kDotColOff[256];
uint8_t kDotColMask[256];
uint8_t kDotColOff[256];
static void buildDotColTables() {
    for (int c = 0; c < 256; c++) {
        const int ac = c - 48;
        if ((unsigned)ac < 160u) {                  // matches ROF_PLOT_DOT's _ac gate exactly
            kDotColMask[c] = kColMask4[ac & 3];
            kDotColOff[c]  = (uint8_t)(ac >> 2);    // 0..39
        } else {
            kDotColMask[c] = 0;                     // off-viewport -> the asm skips the plot
            kDotColOff[c]  = 0;
        }
    }
}
// C reference / non-asm fallback for the plane1 skyline edge plot (see renderFlightDirect).
// One bit per column at its skyline scanline; h==$FF (off-top, all body) plots nothing.
static void edgePlotCore(uint8_t* bp) {
    const uint8_t* y = (const uint8_t*)mem + 0x260E + 48;    // col 0 -> $260E[48]
    uint8_t* colp = bp;
    for (int c = 0; c < 160; c++) {
        uint8_t h = *y++;
        if (h != 0xFFu) colp[kHeightRowOff[h]] |= kColMask4[c & 3];
        if ((c & 3) == 3) colp++;                            // next 4-column plane1 byte
    }
}
extern "C" void flight_edge_plot_asm(uint8_t* bp);           // TerrainRasterizeAssembler.s
#if defined(ROF_RASTERIZE_ASM) && defined(ROF_RASTERIZE_VERIFY)
extern "C" volatile unsigned long g_edgeCalls = 0, g_edgeMismatch = 0, g_edgeAsmTicks = 0, g_edgeCTicks = 0;
// rof_subclock / g_isrBeamLines come from the ROF_FLIGHT_PROBE block above (VERIFY pairs with PROBES).
#endif
#ifdef ROF_BAND_SHAPE
// ---- Band-block STRUCTURAL shape probe (make BAND_SHAPE=1 + amiga/shape_probe.gdb) ----------
// The g_fdBand bucket is the BIGGEST CPU part of renderFlightDirect (59 ticks/call = 1.97% of
// flight, measured 2026-08-05) but it is THREE loops, not one: the object plane-1 overlay, the
// crosshair, and the windscreen-band composite.  Split them, and measure the one thing that
// decides whether the band composite can be replaced by a pre-built masked blit: how much of the
// mode-D band field actually CHANGES from frame to frame (shadowed per double-buffer half, since
// the two halves alternate).  Off by default — the shadow compare costs more than the loop.
extern "C" volatile unsigned long
    g_bsPre = 0, g_bsObj = 0, g_bsCross = 0, g_bsBand = 0,
    g_bsObjFrames = 0, g_bsObjRows = 0, g_bsObjBytes = 0, g_bsObjBox = 0,
    g_bsBandFrames = 0, g_bsBandChanged = 0, g_bsBandClean = 0, g_bsBandMaxChg = 0,
    g_bsBandOwNz = 0, g_bsChgLate = 0;
extern "C" volatile unsigned short g_bsChgPos[160] = {0}, g_bsOwPos[160] = {0};
static uint8_t s_bsShadow[2][4 * 40];
// Lap timer.  rof_beam_line() races the ISR's g_vbiCount++ between its VPOSR and VHPOSR reads, so
// a single bad sample can make the ISR-corrected delta negative and poison an unsigned accumulator
// for the whole run (the known g_fDraw/g_fDirect failure).  Compute signed and drop absurd laps.
static unsigned long s_bsT = 0, s_bsI = 0;
#define BS_RESET()  do { s_bsT = rof_subclock(); s_bsI = g_isrBeamLines; } while (0)
#define BS_LAP(acc) do { unsigned long _n = rof_subclock(), _ni = g_isrBeamLines; \
        long _d = (long)(_n - s_bsT) - (long)(_ni - s_bsI); \
        if (_d >= 0 && _d < 1000) (acc) += (unsigned long)_d; \
        s_bsT = _n; s_bsI = _ni; } while (0)
#endif
#ifdef ROF_EDGE_SHAPE
// ---- Edge-plot STRUCTURAL shape probe (make EDGE_SHAPE=1 + amiga/shape_probe.gdb) -----------
// The edge plot is a 160-column scatter-OR: per column, one table lookup (kHeightRowOff[h]) and
// one indexed byte-OR of a 2-bit mask into the plane-1 row.  Whether that can be restructured
// depends entirely on the SHAPE of the skyline it is fed (mem[$260E+48..]), which the PC profile
// cannot see:
//   * consecutive columns landing on the SAME row can share ONE lookup, and — when they also fall
//     in the same 4-column plane-1 byte — merge their masks into ONE byte-OR.  So the achievable
//     access count is "distinct rows per group", not 4 per group; g_epORs sums exactly that.
//   * a whole group at one row is a single `or.b #$FF`; two adjacent such groups at the same row
//     would be a single `or.w #$FFFF` (g_epWordSame) — only if the runs are that long AND aligned.
// Off by default: the scan itself costs more than the loop it measures.
extern "C" volatile unsigned long
    g_epFrames = 0, g_epFF = 0, g_epSameH = 0, g_epSameRow = 0, g_epLookups = 0, g_epORs = 0,
    g_epRow0 = 0, g_epRow46 = 0, g_epGroupAllSame = 0, g_epWordSame = 0,
    g_epGroupDistinct[5] = {0,0,0,0,0}, g_epRunHist[9] = {0,0,0,0,0,0,0,0,0};
static void edgeShapeProbe() {
    const uint8_t* y = (const uint8_t*)mem + 0x260E + 48;
    g_epFrames++;
    int  prevH = -1;             // previous column's raw height (-1 = none/$FF)
    long prevRow = -1;           // previous column's row offset (-1 = none/$FF)
    int  run = 0;                // current run length of equal row offsets
    long prevGroupRow = -2;      // the row a fully-uniform previous group sat at (-2 = not uniform)
    for (int g = 0; g < 40; g++) {
        long gr[4]; int distinct = 0; long seen[4];
        for (int k = 0; k < 4; k++) {
            const uint8_t h = y[g * 4 + k];
            if (h == 0xFFu) { gr[k] = -1; g_epFF++; prevH = -1; prevRow = -1;
                              if (run) { g_epRunHist[run > 8 ? 8 : run]++; run = 0; } continue; }
            const long row = kHeightRowOff[h];
            gr[k] = row;
            if (row == kRow120[0])  g_epRow0++;
            if (row == kRow120[46]) g_epRow46++;
            if ((int)h == prevH) g_epSameH++; else g_epLookups++;   // lookups needed if h is cached
            if (row == prevRow) { g_epSameRow++; run++; }
            else { if (run) g_epRunHist[run > 8 ? 8 : run]++; run = 1; }
            prevH = h; prevRow = row;
            int dup = 0;
            for (int j = 0; j < distinct; j++) if (seen[j] == row) { dup = 1; break; }
            if (!dup) seen[distinct++] = row;
        }
        g_epGroupDistinct[distinct]++;
        g_epORs += distinct;                                  // byte-ORs a merged loop would issue
        const int uniform = (distinct == 1 && gr[0] >= 0 && gr[1] >= 0 && gr[2] >= 0 && gr[3] >= 0);
        if (uniform) {
            g_epGroupAllSame++;
            if ((g & 1) && prevGroupRow == gr[0]) g_epWordSame++;   // even+odd pair, same row -> or.w
        }
        prevGroupRow = uniform ? gr[0] : -2;
    }
    if (run) g_epRunHist[run > 8 ? 8 : run]++;
}
#endif
//   GTIA mode-10 (tunnel field at $2000): byte = 2 nibbles; nibble bit k → 4px.
static uint8_t kGtia10P1[256];   // nibble bit0
static uint8_t kGtia10P2[256];   // nibble bit1
static uint8_t kGtia10P3[256];   // nibble bit2
// BOOST reverse-tunnel variant of the GTIA-10 LUT (decodeBoostViewport only): value-2 (outermost
// ring) -> pen0/color00, value-8 (background) -> pen2/color02.  See the constructor build + the
// updateTunnelCopper boost palette branch.  Kept separate so the FORWARD tunnel LUT is untouched.
static uint8_t kGtia10BoostP1[256];
static uint8_t kGtia10BoostP2[256];
static uint8_t kGtia10BoostP3[256];
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
// g_standbyRevealReady LATCHES on at boot_standby_launch_driver entry (rof_native.c) — by then
// game_main_loop has drawn the cockpit/top bar and scene.initialize has set up the sprites.
// It latches (never clears) on purpose: the launch sequence re-runs boot_standby_launch_driver and
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

// Each GRAFP2 nibble (4 bits) expands to 16 px — each bit → 4 Amiga px.  Precomputed so the
// per-row decode is two table lookups instead of expandNibble16's 8 conditional branches.
static const uint16_t kAHExpand[16] = {
    0x0000, 0x000F, 0x00F0, 0x00FF, 0x0F00, 0x0F0F, 0x0FF0, 0x0FFF,
    0xF000, 0xF00F, 0xF0F0, 0xF0FF, 0xFF00, 0xFF0F, 0xFFF0, 0xFFFF,
};

void RescueOnFractalus::buildAHSprite()
{
    // Change-detect: the ground-fill source ($0E92..) is rewritten IN FLIGHT only by
    // draw_ah_ground_fill_p2 ($40B0), which is itself gated on the pitch index $291C/$291D (the
    // other writers of this range — fill_four_bufs_ff, init_gameplay_state — are gameplay-init
    // only).  So skip the 33-row rebuild when pitch is unchanged; cockpitForceFull (scene entry,
    // where the init writers ran) forces a rebuild so the post-init state is captured.
    uint8_t idx = mem[0x291C], sub = mem[0x291D];
    if (!cockpitForceFull && idx == ahLastIdx && sub == ahLastSub) return;
    ahLastIdx = idx; ahLastSub = sub;
    uint16_t* l = ahLeft->data()  + 2;         // skip the 2 control words
    uint16_t* r = ahRight->data() + 2;
    for (int i = 0; i < kAHRows; i++) {
        uint8_t b = mem[kAHBufBase + i];                 // GRAFP2 ($00 sky / $FF ground)
        l[i * 2] = kAHExpand[b >> 4];   l[i * 2 + 1] = 0x0000;   // bits 7-4 → left 16px (plane A = pen01)
        r[i * 2] = kAHExpand[b & 0x0F]; r[i * 2 + 1] = 0x0000;   // bits 3-0 → right 16px
    }
}

// ---- player laser shot sprite (instrument-free gameplay PMG) ------------------
// The player's laser is Atari player P2, rendered by draw_player_shot ($8c58) into the P2
// buffer's UPPER region (mem[$0E32+], separate from the AH ground fill at $0E92+).  The
// transpiled $8c58 already fills mem[] on the Amiga every flight frame (only its $D0xx writes
// are ignored), so we mirror it into a sprite exactly like buildAHSprite:
//   active  = mem[$0036] != 0   (0 = no shot; 1..~$11 = travel; $81..$8a = impact burst)
//   pixels  = mem[$0E00 + O]    (8 GRAFP2 bits per row; O in $34..$91)
//   HPOS    = mem[$00CB]        (true HPOSP2 shadow → viewport X = 0x81 + (hpos-$32)*2)
//   colour  = mem[$0037]        (COLPM2 → COLOR27; blue $78 travel → white/orange impact)
//   size    = mem[$00CD]        (0=1×/1=2×/3=4×; milestone renders 1×, each bit → 2 Amiga px)
// The sprite lives on the otherwise-idle ch4 (energy is ch5, altimeter ch6/7).  Fixed height;
// its VSTART (setY) moves to the current run each frame, unused rows stay blank (transparent).
static const int kShotRows = 32;          // covers the 1× bolt + 1× impact burst

// User-tweakable alignment nudge for the player laser sprite (viewport space).  The shot read
// ~4px too far LEFT and ~2px too far DOWN vs the crosshair; kShotXOff moves it right (+),
// kShotYOff moves it down (+).  Tune these two numbers to land it exactly.
static const int kShotXOff = 3;    // + = right (Amiga sprite X units)
static const int kShotYOff = 0;    // + = down  (Amiga scan lines)

// Expand an 8-bit Atari player row (bit7 = leftmost) to 16 Amiga sprite px, each bit → 2 px
// (the 1× player→viewport scale, 2 Amiga lores px per Atari colour clock).  The per-call bit
// loop cost 8 variable-shift iterations (no 68000 barrel shifter); a 256-entry LUT filled once
// by buildShotExpandLut() replaces it for the per-frame shot / scope-P3 / viewport-P3 mirrors.
static uint16_t s_shotExpand[256];
static bool     s_shotExpandReady = false;
static void buildShotExpandLut()
{
    for (int b = 0; b < 256; b++) {
        uint16_t v = 0;
        for (int k = 0; k < 8; k++)
            if (b & (uint8_t)(0x80u >> k)) v |= (uint16_t)(3u << (14 - 2 * k));
        s_shotExpand[b] = v;
    }
    s_shotExpandReady = true;
}
static inline uint16_t expandShotRow(uint8_t b) { return s_shotExpand[b]; }

void RescueOnFractalus::buildShotSprite()
{
    if (mem[0x0036] == 0) {                            // no shot active
        if (shotWasActive) {                           // shot just ended: blank BOTH buffers once so
            uint16_t* a = shotSprite->data()     + 2;  // whichever is on-screen shows nothing, then
            uint16_t* b = shotSpriteBack->data() + 2;  // leave ch4 pointing at a blank buffer (no more
            for (int i = 0; i < kShotRows * 2; i++) { a[i] = 0; b[i] = 0; }   // per-frame work while idle)
            if (flightCopper) flightCopper->setHudSprite(4, *shotSprite);
            shotWasActive = false;
        }
        return;
    }
    shotWasActive = true;
    // Build into the OFF-screen buffer (the one NOT latched for display this frame): the copper
    // fetched SPR4PT at the top of the frame, before this VBI-time build, so re-pointing it now
    // takes effect NEXT frame — the displayed buffer is always a fully-built one, never mid-write.
    Sprite* s = shotBuildIdx ? shotSpriteBack : shotSprite;
    uint16_t* d = s->data() + 2;                       // skip the 2 control words
    // Find the shot's non-zero run in the P2 UPPER region ($34..$91; $92+ is the AH ground fill).
    int top = -1, bot = -1;
    for (int o = 0x34; o <= 0x91; o++)
        if (mem[0x0E00 + o]) { if (top < 0) top = o; bot = o; }
    for (int i = 0; i < kShotRows * 2; i++) d[i] = 0;  // clear, then decode the run below
    if (top >= 0) {
        int rows = bot - top + 1;
        if (rows > kShotRows) rows = kShotRows;
        for (int i = 0; i < rows; i++) {
            uint16_t m = expandShotRow(mem[0x0E00 + top + i]);
            d[i * 2] = m; d[i * 2 + 1] = m;            // both planes → pen 11 → COLOR27
        }
        s->setY((uint16_t)(kTerrainLine + (top - 0x32) + kShotYOff));            // buffer row → Amiga line
        s->setX((uint16_t)(0x81 + ((int)mem[0x00CB] - 0x32) * 2 + kShotXOff));   // HPOSP2 → viewport X
    }
    if (flightCopper) {
        flightCopper->setHudSprite(4, *s);                            // display this buffer next frame
        flightCopper->setShotColor(atariToOCS(mem[0x0037]));          // COLPM2 → COLOR27
    }
    shotBuildIdx ^= 1;                                                // next frame builds the other buffer
}

// ---- Targeting Scope (#8) P3 object -------------------------------------------------
// The object shown in the Targeting Scope (#8) is a generic Atari player-3 object (the
// gun emplacement renders as a half-ball $38 7C FE FE FE; the saucer as a diamond) that draw_player3_object $42A7 plots into the P3 buffer $0F00 in the
// scope band, coloured by COLPM3 = mem[$00D9] (cyan, cycled each frame by the flight VBI at
// $50ab).  P3 is otherwise unrendered on the Amiga; reuse sprite ch3 (right window-frame
// triangle, whose VSTOP<=180 leaves it idle in the dashboard) via the copper SPR3PT re-point
// (INDEX_SCOPE_SPR).  Same buffer→sprite mirror as the laser shot, single-buffered like the
// AH ground fill (the object moves at terrain-render rate, so a per-render rebuild suffices).
// Scope-band window in the P3 buffer: offset $98..$B8 → Amiga lines 188..220 (the dashboard
// scope box).  (The viewport saucer copy lives lower in the buffer and is a later increment.)
static const int kScopeP3Rows = 24;
void RescueOnFractalus::buildScopeP3Sprite()
{
    // Per-frame mirror of the P3 Targeting-Scope copy.  Runs every frame (position/shape track the
    // object, whose coordinates are integrated per-frame elsewhere), with only the CLEAR made
    // incremental — clear last frame's rows (p3ScopePrevRows), not the whole sprite.
    uint16_t* d = scopeP3Sprite->data() + 2;         // skip the 2 control words
    int top = -1, bot = -1;
    for (int o = 0x98; o <= 0xB8; o++) {
        if (mem[0x0F00 + o]) { if (top < 0) top = o; bot = o; }
        else if (top >= 0) break;   // contiguous P3 object: run ended → stop the volatile scan
    }
    for (int i = 0; i < p3ScopePrevRows; i++) { d[i * 2] = 0; d[i * 2 + 1] = 0; }  // clear last frame's rows
    if (top >= 0) {
        int rows = bot - top + 1;
        if (rows > kScopeP3Rows) rows = kScopeP3Rows;
        for (int i = 0; i < rows; i++) {
            uint16_t m = expandShotRow(mem[0x0F00 + top + i]);
            d[i * 2] = m; d[i * 2 + 1] = m;        // both planes → pen 11 → COLOR23 (cyan)
        }
        scopeP3Sprite->setY((uint16_t)(kTerrainLine + (top - 0x32) + 7));  // buffer row → Amiga line (+7 user-calibrated)
        // Scope-P3 X = mem[$00CC], NOT the viewport HPOSP3 ($2870): the flight dashboard DLI $4A78
        // reloads HPOSP3←$CC before the scope band, and draw_player3_object $43C4 sets $CC = the
        // viewport X clamped into the scope-box window.  (For a saucer $CC==$2870; they diverge for
        // other target positions.)  Same cc→hw-X transform as the viewport copy.
        scopeP3Sprite->setX((uint16_t)(0x81 + ((int)mem[0x00CC] - 0x32) * 2));
        p3ScopePrevRows = rows;
    } else {
        p3ScopePrevRows = 0;                         // inactive: nothing to clear next frame
    }
    if (flightCopper) flightCopper->setScopeP3Color(atariToOCS(mem[0x00D9]));  // COLPM3 → COLOR23
}

// ---- Main Window (#9) P3 object -------------------------------------------------
// The SECOND P3 copy: the target's body in the main viewport (gun emplacement / flying saucer),
// IN FRONT of the terrain, tracking the target's screen position (X = HPOSP3 mem[$2870], Y = the
// P3 buffer offset).  Same buffer->sprite mirror as the Targeting-Scope copy, but the Main-Window window
// ($0F00+$32..$85, i.e. the same offset range as the canopy posts / AH span) on ch7 (altimeter
// ship, free in the viewport) via the copper SPR7PT multiplex.  Present only when the target is
// close enough to have a Main-Window body; far targets show only the Targeting-Scope copy.  Colour cyan
// from mem[$00D9] (COLPM3) → COLOR31.
// ch7 is MULTIPLEXED: this P3 object in the viewport (top sprite), the altimeter-ship gauge in the
// dashboard (re-pointed by SPR7PT at line 181, INDEX_ALTIM_SHIP_SPR).  Amiga sprite reuse only
// re-arms a channel on the control-word re-fetch that follows a VSTOP — so the outgoing top sprite
// MUST run continuously to the re-point line (180), exactly like the ch0-3 frame posts (which the
// AH ch0/1 + scope ch3 re-points depend on).  An 8-row object parked at VSTART 86 hits VSTOP ~94,
// the DMA fetches its trailing 0,0 terminator and DISARMS ch7 for the frame → the 181 re-point is
// too late and the altimeter ship never arms (the regression).  So keep viewportP3 a FIXED-VSTART
// (kTerrainLine) FULL-VIEWPORT-HEIGHT (94 rows → VSTOP 180) sprite with the object drawn at its
// internal row and every other row transparent — then ch7 re-arms the altimeter at 181 like ch3.
static const int kViewportP3Rows = kViewportFullHeight;   // 94: VSTART 86 → VSTOP 180 (matches the frame posts / star sprites)
void RescueOnFractalus::buildViewportP3Sprite()
{
    // Per-frame mirror of the P3 Main-Window copy.  Position ($2870 HPOSP3 shadow) and the shape's
    // vertical offset both advance whenever the object moves (its inputs — player3_ytop $2824 — are
    // integrated every frame by object_integrate_position, a DIFFERENT routine), so this must run
    // every frame (a shape-dirty gate would freeze the position — the earlier regression).  The one
    // avoidable cost is the tall CLEAR: instead of zeroing all 94 rows, clear only the rows written
    // last frame (p3ViewportPrev*), which is just the object's on-screen height.  ("Sprite data
    // modified only when needed" without losing per-frame position.)
    uint16_t* d = viewportP3Sprite->data() + 2;      // skip the 2 control words
    int top = -1, bot = -1;
    for (int o = 0x32; o <= 0x85; o++) {
        if (mem[0x0F00 + o]) { if (top < 0) top = o; bot = o; }
        else if (top >= 0) break;   // contiguous P3 object: run ended → stop the volatile scan
    }
    for (int i = 0; i < p3ViewportPrevRows; i++) {   // clear only last frame's rows (not all 94)
        d[(p3ViewportPrevBase + i) * 2] = 0; d[(p3ViewportPrevBase + i) * 2 + 1] = 0;
    }
    viewportP3Sprite->setY(kTerrainLine);            // FIXED VSTART: keep ch7 busy through the viewport
    if (top >= 0) {
        int base = top - 0x32;                       // object's row within the fixed-VSTART sprite
        int rows = bot - top + 1;
        if (base + rows > kViewportP3Rows) rows = kViewportP3Rows - base;
        for (int i = 0; i < rows; i++) {
            uint16_t m = expandShotRow(mem[0x0F00 + top + i]);
            d[(base + i) * 2] = m; d[(base + i) * 2 + 1] = m;  // both planes → pen 11 → COLOR31 (cyan)
        }
        viewportP3Sprite->setX((uint16_t)(0x81 + ((int)mem[0x2870] - 0x32) * 2));  // HPOSP3 shadow → X
        p3ViewportPrevBase = base; p3ViewportPrevRows = rows;
    } else {
        p3ViewportPrevRows = 0;                      // inactive: nothing to clear next frame
    }
    if (flightCopper) flightCopper->setViewportP3Color(atariToOCS(mem[0x00D9]));  // COLPM3 → COLOR31
}

// ---- Long Range Scanner (#13) guide dot ------------------------------------------
// The single flashing-red dot that points the player toward a downed pilot is Atari MISSILE M2 (a
// ~3-scanline blob), NOT a bitmap cell.  Its VERTICAL position (range) lives as the M2 bits (5:4)
// of the missile DMA buffer $0B00 in the dashboard band (written by $44E0: it clears 3 M2 rows at
// the old row and sets 3 at the new row mem[$B9]); its HORIZONTAL position (bearing) is mem[$00CE]
// (the flight dashboard DLI $4AC7 loads it into HPOSM2); its colour is COLPM2 $26 (red-brown).
// The native flight VBI writes all of that into mem[], so this is a READ-ONLY mirror (like the
// scope/AH copies) onto sprite ch2 (right A-pillar, idle+armed in the dashboard) via SPR2PT.
// Blink: when the game clears the M2 bits, the scan finds no run → the sprite goes transparent.
static const int kScannerDotRows = 8;   // the M2 blob is ~3 rows; a few spare for safety
void RescueOnFractalus::buildScannerDotSprite()
{
    // Scan the DASHBOARD band of the missile buffer for M2 (bits 5:4).  The crosshair M2 lives
    // higher ($0B4D-71, viewport) so start at $0B88; M1/M3 (other bits) don't trip the M2 mask.
    // Per-frame position + incremental clear: the dot moves (bearing X $00CE, range Y) and blinks
    // (M2 bits cleared) every frame, so run every frame; only clear the few rows written last frame
    // (p3ScopePrev-style scannerPrevRows) rather than the whole sprite.
    int top = -1, bot = -1;
    for (int o = 0x88; o <= 0xB8; o++)
        if ((mem[0x0B00 + o] >> 4) & 3) { if (top < 0) top = o; bot = o; }
    uint16_t* d = scannerDotSprite->data() + 2;       // skip the 2 control words
    for (int i = 0; i < scannerPrevRows; i++) { d[i * 2] = 0; d[i * 2 + 1] = 0; }   // clear last frame's rows
    if (top >= 0) {
        int rows = bot - top + 1;
        if (rows > kScannerDotRows) rows = kScannerDotRows;
        for (int i = 0; i < rows; i++) {
            uint8_t m2 = (uint8_t)((mem[0x0B00 + top + i] >> 4) & 3);   // the 2 M2 pixels
            uint16_t w = (uint16_t)((m2 & 2 ? 0x8000u : 0u) | (m2 & 1 ? 0x4000u : 0u));
            d[i * 2] = 0; d[i * 2 + 1] = w;          // plane B only → pen 10 → COLOR22 (red)
        }
        // buffer row → Amiga line (same PMG single-line mapping as the AH/scope copies; +0 =
        // no extra offset — user-calibrated for the scanner disc on FS-UAE, higher than the scope's +7).
        scannerDotSprite->setY((uint16_t)(kTerrainLine + (top - 0x32)));
        // bearing X = mem[$00CE] (the HPOSM2 source the dashboard DLI $4AC7 loads); same Atari-HPOS
        // → Amiga hardware-X transform as the scope/viewport-P3 copies (+4 user-calibrated on FS-UAE).
        scannerDotSprite->setX((uint16_t)(0x85 + ((int)mem[0x00CE] - 0x32) * 2));
        scannerPrevRows = rows;
    } else {
        scannerPrevRows = 0;                          // inactive: nothing to clear next frame ("off" blink)
    }
}

// ---- throttle gauge sprite ---------------------------------------------------
// Build the player-1 throttle bar from the vobj strip mem[$0D98..].  Each strip
// byte is one Atari player scanline ($F0 = leftmost 4px on); we map a filled row
// to the leftmost 4 px (colour 01) of an Amiga sprite line.
static const int kEnergyRows = 56;   // full bar = 56 px, same as the altimeter (NOT 57 — that 1px diff was a bug)
void RescueOnFractalus::buildEnergyIndicatorSprite()
{
    // The energy/fuel bar (Atari P1 strip $0D98, the "right gauge") is a solid 8px bottom-anchored
    // bar — filled with $F0 from a fuel-derived top down to the bottom, exactly the altimeter shape
    // (verified live: strip is a contiguous $F0 run, empty when fuel $062F = 0).  So use the same
    // trick: build the solid sprite ONCE and move its Y, instead of decoding 57 rows every frame.
    // Bar top index = (($DC - fuel) & 0xFF) >> 2, +1 to match the drawn fill (vobj_pos_to_pmstrip_index
    // $41DA).  fuel 0 (empty / out of fuel) parks the bar below the floor (line 252) where the
    // COLOR25 black-out hides it.  Gauge is 8px wide → plane A = 0xFF00 (left half of the 16px sprite).
    static const uint16_t kBase = 0x2c + 144;            // buffer offset 0 → line 188 (same base as the altimeter)
    if (!energySolidBuilt) {
        uint16_t* d = energyIndicatorSprite->data() + 2; // skip the 2 control words
        for (int i = 0; i < kEnergyRows; i++) { d[i * 2] = 0xFF00u; d[i * 2 + 1] = 0x0000u; }
        energySolidBuilt = true;
    }
    uint8_t  fuel = mem[0x062F];
    uint16_t top;
    if (fuel == 0) top = 64u;                            // empty → park below floor = blacked out
    else {
        top = (uint16_t)((uint8_t)(0xDC - fuel) >> 2);   // bar-top index (vobj_pos_to_pmstrip_index $41DA)
        if (top > (uint16_t)kEnergyRows) top = 0u;       // clamp garbage → full bar
    }
    energyIndicatorSprite->setY((uint16_t)(kBase + top));
}

static const int      kAltimRows    = 56;             // 8×56 rectangle ($0C98..$0CCF / $0B98..$0BCF)
static const uint16_t kAltimTopLine = 0x2c + 144;     // buffer offset 0 → Amiga line 188 (matches setY below)

// ---- altimeter bars (flight) -------------------------------------------------
// The terrain-height (P0 $0C98) and ship-height (M3 $0B98) bars are fixed 8×56 solid rectangles
// whose TOP edge tracks the value (bar offsets $281A / $281B).  On the Atari, draw_altimeter_bars
// ($40E5) redraws the GRAFP fill because players have no per-line start register; the Amiga sprite
// does (VSTART), so we keep a SOLID 56-row sprite and just move its Y each frame.  The 56-row bar
// overflows past the dial bottom, but sprite<playfield priority hides the overflow behind the
// cockpit dashboard, so the visible bar = top..dial-bottom — identical to the Atari, at the cost
// of one setY (a few header bytes) instead of 56 row-decodes.  The solid fill is done once,
// lazily on the first flight frame, so pre-flight the (zeroed) sprites stay invisible.
void RescueOnFractalus::buildAltimeterSprite()
{
    if (!altimSolidBuilt) {
        uint16_t* at = altimeterSprite->data() + 2;       // skip the 2 control words
        uint16_t* sh = altimeterShipSprite->data() + 2;
        for (int i = 0; i < kAltimRows; i++) {
            at[i * 2] = 0xFFFFu; at[i * 2 + 1] = 0x0000u; // terrain bar: plane A (pen 01 / COLOR29)
            sh[i * 2] = 0x0000u; sh[i * 2 + 1] = 0xFFFFu; // ship bar:    plane B (pen 10 / COLOR30)
        }
        altimSolidBuilt = true;
    }
    // top = bar-top offset (0 = full bar at the dial top, kAltimRows = empty).  Pre-flight (and the
    // first flight frame before the VBI computes it) $281A holds garbage (e.g. $88); an out-of-range
    // value added to kAltimTopLine would push VSTART past 255 and wrap into the windscreen (Sprite::setY
    // has no SV8), so clamp it to 0 = full (the topmost valid position).
    uint16_t top = mem[0x281A];
    if (top > (uint16_t)kAltimRows) top = 0;
    altimeterSprite->setY((uint16_t)(kAltimTopLine + top));
}

void RescueOnFractalus::buildAltimeterShipSprite()
{
    uint16_t top = mem[0x281B];
    if (top > (uint16_t)kAltimRows) top = 0;       // clamp pre-flight garbage to full (see buildAltimeterSprite)
    altimeterShipSprite->setY((uint16_t)(kAltimTopLine + top));
}

// ---- starfield sprites -------------------------------------------------------
// During the stars phase boot_standby_launch_driver positions players P0/P2/P3 as a sparse
// scrolling starfield (random_terrain_height $6B47: POKEY RANDOM, 1/32 chance of a dot
// from table $6B5F = [$80,$20,$04,$01]; scroll_field_columns $6AEE shifts each
// player up one scanline/frame and appends a new bottom byte).  The genuine
// transpiled scroll_field_columns already maintains those player buffers in
// mem[], so we just map the 89-byte visible strip ($..32..$..8A, player scanlines
// $32..$8A) of each into a PAIR of Amiga sprites (the faithful SIZEP=$03 quad player spans
// 32 colour clocks = 32 lores px at 1:1, wider than one 16 px hardware sprite).
static const uint16_t kStarSrc[3]  = { 0x0C32, 0x0E32, 0x0F32 };  // P0, P2, P3
// Amiga X of each player's LOW sprite: Atari colour clock $32 maps to the left display edge
// (0x81), scale 2 (320 px / 160 cc) for the on-screen COLUMN placement (matches the terrain
// viewport scale, so the columns spread across the screen as on the Atari).  Each player's
// two-sprite pair then spans 32 px internally (the HIGH sprite sits +16 px right), rendering
// the quad at 1:1 so a 4-cc dot is 4 px wide.  HPOSP0=$38, HPOSP2=$8E, HPOSP3=$B8
// (boot_standby_launch_driver $64F3-$6503).
static const uint16_t kStarX[3]    = { (uint16_t)(0x81 + (0x38 - 0x32) * 2),    // P0 lo = 141
                                       (uint16_t)(0x81 + (0x8E - 0x32) * 2),    // P2 lo = 313
                                       (uint16_t)(0x81 + (0xB8 - 0x32) * 2) };  // P3 lo = 397
static const int       kStarRows   = 89;   // visible strip $..32..$..8A ($59 bytes)
// Zero-copy scroll ring sizing.  The starfield scrolls up a fixed maximum over the whole
// cinematic — measured 595 rows (g_starScrollGen plateaus there, then freezes for the rest of
// the scene; a faithful port so it won't grow).  Size the ring for that plus headroom; a clamp
// in starVblankUpdate freezes rather than overruns if the window ever hits the end.  Each ring
// "slot" = one sprite row (2 words: plane A glyph + plane B 0).  Window advances 0..kStarMaxScroll.
static const int       kStarMaxScroll = 640;                  // 595 measured + margin
// display window = kViewportFullHeight rows; +2 = control slot + terminator fetch past the window.
static const int       kStarRingSlots = kStarMaxScroll + kViewportFullHeight + 2;

// Full (re)build of all 6 star rings at window 0 — run once on stars entry (and on re-entry).
// Clears each ring (so a previous pass's rows above the window / stale terminator are gone),
// re-seeds the window-0 control slot, then converts the 89 visible rows into slots [1..89].
// Each set star bit becomes a 4-px dot at its faithful 0/8/20/28-cc offset via kStarGlyphLo/Hi.
void RescueOnFractalus::buildStarSprites()
{
    for (int i = 0; i < 6; i++) {
        uint16_t* ring = starRing[i];
        if (!ring) continue;
        for (int w = 0; w < kStarRingSlots * 2; w++) ring[w] = 0;   // clear whole ring (rare — entry only)
        ring[0] = starCtl[i][0];  ring[1] = starCtl[i][1];          // window-0 control slot
        const uint8_t* src = (const uint8_t*)&mem[kStarSrc[i >> 1]];
        const uint16_t* tbl = (i & 1) ? kStarGlyphHi : kStarGlyphLo;
        uint16_t* dst = ring + 2;                                   // slot 1 (skip control slot 0)
        for (int r = 0; r < kStarRows; r++) { *dst++ = tbl[src[r]]; *dst++ = 0x0000; }
    }
    starWindow  = 0;
    starLastGen = g_starScrollGen;
}

// starVblankUpdate: the per-vblank zero-copy star scroll, called from the real VBI ISR
// (PlatformAmiga::vbiHandler) so the copper SPRxPT + control-word writes are tear-free.
// The field scrolls up by N rows since last frame (g_starScrollGen delta, 0 or 1 normally).
// We advance the window by N (re-pointing the copper), convert ONLY the N new bottom rows into
// the ring, and write the (constant) control words at the new window slot.  Everything else —
// the already-converted rows, the 5 blank padding rows and the terminator — is untouched (the
// padding/terminator are the still-zero slots below the star region).  See RescueOnFractalus.h.
void RescueOnFractalus::starVblankUpdate()
{
    // Pixel half of the zero-copy star scroll (pointer half is in perFrameWork).  Runs at vblank —
    // after the previous frame's display (so overwriting the now-scrolled-off control slot doesn't
    // tear it) and before this frame's sprite control fetch (~line 14-24, so the fresh control
    // words are in place when fetched).  perFrameWork already advanced starWindow + set the copper
    // pointer operand for this frame and handed us the new-row count in starVbiRows.
    if (!starPhaseActive) return;
    int N = starVbiRows;
    starVbiRows = 0;
    const int nw = starWindow;                                  // window perFrameWork advanced to
    // Write the CONTROL WORDS for all 6 sprites at the current window head — UNCONDITIONALLY, even
    // when N<=0.  perFrameWork runs more than once per vblank when the planet bitplane conversion is
    // slow, and a 2nd zero-scroll call resets starVbiRows to 0.  Gating this write on N>0 therefore
    // left the (already-advanced) window-head slot holding stale star pixel data → the sprite read
    // it as its control words → corrupt VSTOP → the channel-2 throttle-gauge re-arm dropped for a
    // frame (measured: ~24 drops/rise via g_starDropRisk).  The write is cheap (12 words) and
    // idempotent when the window is static, so always do it — the operand always finds fresh control
    // words at nw.  (The old "top-of-frame race" theory was disproven: exitLineMax=21, lateCount=0.)
    for (int i = 0; i < 6; i++) {
        uint16_t* ring = starRing[i];
        ring[2 * nw]     = starCtl[i][0];
        ring[2 * nw + 1] = starCtl[i][1];
    }
#ifdef ROF_FLIGHT_PROBE
    {
        // Post-fix verifier: nw always has fresh control words now, so dropRisk must stay 0.
        extern volatile unsigned short g_starDropRisk, g_starDropRiskAtVbi;
        static int s_lastCtlWindow = -1;
        if (s_lastCtlWindow >= 0 && nw != s_lastCtlWindow && N <= 0) {   // skip first-firing sentinel
            g_starDropRisk++; g_starDropRiskAtVbi = platform_frame_count();
        }
        s_lastCtlWindow = nw;
    }
#endif
    if (N <= 0) return;                                          // control refreshed above; no new rows
    if (N > kStarRows) N = kStarRows;                            // safety clamp (never underflow the loop)
    // Convert the N new bottom rows (mem visible rows [kStarRows-N .. kStarRows-1]) into the
    // ring.  Rows above are reused in place; the 5 padding rows + terminator stay zero (ambient,
    // never-written slots — the data-side end marker is always present without being rewritten).
    for (int i = 0; i < 6; i++) {
        uint16_t* ring = starRing[i];
        const uint8_t* src = (const uint8_t*)&mem[kStarSrc[i >> 1]];
        const uint16_t* tbl = (i & 1) ? kStarGlyphHi : kStarGlyphLo;
        for (int r = kStarRows - N; r < kStarRows; r++) {
            int slot = nw + 1 + r;                              // display row r of the advanced window
            ring[2 * slot]     = tbl[src[r]];
            ring[2 * slot + 1] = 0x0000;
        }
    }
#ifdef ROF_FLIGHT_PROBE
    {
        extern volatile unsigned short g_starVbiExitLine, g_starVbiExitLineAtVbi;
        extern volatile unsigned short g_starVbiLateCount, g_starVbiCalls;
        uint16_t vpos  = *(volatile uint16_t*)0xDFF004u;   // VPOSR: bit0 = V8
        uint16_t vhpos = *(volatile uint16_t*)0xDFF006u;   // VHPOSR: high byte = V7-V0
        uint16_t line  = (uint16_t)(((vpos & 1) << 8) | (vhpos >> 8));
        g_starVbiCalls++;
        if (line > g_starVbiExitLine) { g_starVbiExitLine = line; g_starVbiExitLineAtVbi = platform_frame_count(); }
        if (line > 25) g_starVbiLateCount++;   // past the sprite control fetch → stale-read risk
    }
#endif
}

// decodeDoorScrollField(): decode the whole $2000 GTIA mode-10 door field (85 rows, stride 46)
// into the TALL doorScrollBitmap, then pad the rows below the field with the green closed-door
// row so the elevator scroll always reads valid pixels.  Same packing as the render() door decode
// (kDoorP1/kDoorP2 LUTs, 4-byte overscan crop, 10 longs/plane/row, plane3=0) — see that comment.
void RescueOnFractalus::decodeDoorScrollField()
{
    if (!doorScrollBitmap) return;
#ifdef ROF_FLIGHT_PROBE
    { extern volatile unsigned short g_dsDecodes; g_dsDecodes++; }
#endif
    const uint8_t* sbase = (const uint8_t*)mem + 0x2000 + 4;   // +4 = wide-playfield overscan crop
    uint8_t* vdest = (uint8_t*)doorScrollBitmap->data;
    // Rows 0..84: the live field.  Rows 85..171: green closed-door pad (field byte $88, which the
    // field itself uses for the blank door area above/below the LEVEL text).
    const uint32_t g1 = (uint32_t)kDoorP1[0x88] * 0x01010101u;
    const uint32_t g2 = (uint32_t)kDoorP2[0x88] * 0x01010101u;
    for (int row = 0; row < 172; row++) {
        uint32_t* p1 = (uint32_t*)vdest;
        uint32_t* p2 = (uint32_t*)(vdest + 40);
        uint32_t* p3 = (uint32_t*)(vdest + 80);
        if (row < 85) {
            const uint8_t* src = sbase + row * 46;
            for (int b = 0; b < 10; b++) {
                uint8_t s0 = *src++, s1 = *src++, s2 = *src++, s3 = *src++;
                *p1++ = ((uint32_t)kDoorP1[s0] << 24) | ((uint32_t)kDoorP1[s1] << 16) |
                        ((uint32_t)kDoorP1[s2] <<  8) |  (uint32_t)kDoorP1[s3];
                *p2++ = ((uint32_t)kDoorP2[s0] << 24) | ((uint32_t)kDoorP2[s1] << 16) |
                        ((uint32_t)kDoorP2[s2] <<  8) |  (uint32_t)kDoorP2[s3];
                *p3++ = 0u;
            }
        } else {
            for (int b = 0; b < 10; b++) { *p1++ = g1; *p2++ = g2; *p3++ = 0u; }
        }
        vdest += 120;
    }
}

// doorScrollVblankUpdate(): the level-select "elevator" door scroll, driven from the INTB_VERTB
// ISR at vblank start (the main thread is spinning in boot_standby_launch_driver's L_626a/L_628f
// level-select loops, so renderFrame never runs during the scroll — only this ISR does).  The
// Atari scrolls by DECrementing dl_src_index ($008B) per frame (dl_index_dec), which rebuilds the
// per-scanline DL LMS window so the viewport top shows field row $008B (measured 2026-08-04).
// Reproduce it with a single BPLxPT offset: point the standby terrain region at doorScrollBitmap
// offset by $008B rows.  Repoint at vblank ONLY (a torn BPLxPT garbles the whole viewport).
void RescueOnFractalus::doorScrollVblankUpdate()
{
    // Runs only when the settled standby cockpit is the live display (its terrain region is the door
    // field; the forward-launch doors use a different copper).  standbyCopperInstalled stays true
    // across the SELECT scroll spin (renderFrame doesn't run then), and g_standbyRevealReady gates
    // out the initial black-held boot build.
    if (!(standbyCopperInstalled && g_standbyRevealReady && standbyCopper)) return;

    // MIRROR the live door DL ($300A) into the copper EVERY standby frame — scroll, settle, AND idle
    // rest — so the copper always equals the DL.  There is no freeze/stale point, which is what made
    // repeated scrolls start 1px off: the earlier gates stopped updating at rest and froze the copper
    // at whatever frame the DL happened to be on, 1px from where the next scroll's first render put
    // it.  Guard: entry 0 must be a $2000-field address (else $300A isn't the door DL — a scene
    // transition — so leave the copper alone).
    unsigned a0 = mem[0x300A] | (mem[0x300B] << 8);
    if (a0 < 0x2000u || a0 >= 0x3000u) return;

    // Re-decode the tall door bitmap only when blit_numeric_readout marked the digit dirty (a couple
    // of times per scroll).  The pointers below are unchanged by a decode, so the fresh pixels show
    // automatically.  The digit is rewritten while its rows are off-screen, so the decode is tear-free.
    if (g_doorScrollFieldDirty) { decodeDoorScrollField(); g_doorScrollFieldDirty = 0; }

    // Lock-on indicator blink (cells $3491-$3497).  The faithful standby VBI keeps running
    // lock_on_indicator_tick ($4229) throughout the scroll, so it toggles the cell bytes in mem[]
    // exactly as the Atari does — but on the Amiga those cells only reach the display via the
    // cockpit decode in renderFrame, and renderFrame is stalled while boot_standby_launch_driver
    // busy-spins the scroll.  So complete the ISR-side display bridge here: whenever the tick has
    // flagged the strip dirty, re-decode those 7 cockpit cells straight into cockpitBitmap.  The
    // cockpit sits at Amiga lines 172+, decoded here at vblank start before the beam reaches it →
    // tear-free (same discipline as decodeScannerBlinkCells).  Clearing the flag also means the
    // main-loop renderFrame won't redundantly re-decode when it is running (idle standby).
    if (g_ckLockon) { g_ckLockon = 0u; decodeCockpitSpan(0x3491u, 7u); }

    // Parse the DL (86 mode-F LMS entries, stride 3) into runs of consecutive field rows.  Consecutive
    // rows are +46 in the LMS address, so work in ADDRESSES and divide (→ field row) only per run.
    static const int kMaxDoorRuns = 20;          // == StandbyCopperList MAX_TERRAIN_RUNS
    uint8_t  runScan[kMaxDoorRuns];
    uint16_t runRow[kMaxDoorRuns];
    int nRuns = 0;
    unsigned prevA = 0xFFFFu;                     // sentinel: forces a run break at k=0
    for (int k = 0; k < 86; k++) {
        unsigned a = mem[0x300A + 3*k] | (mem[0x300A + 3*k + 1] << 8);
        if (a != prevA + 46u && nRuns < kMaxDoorRuns) {   // discontinuity -> new run
            uint16_t off = (a >= 0x2000u) ? (uint16_t)(a - 0x2000u) : 0u;
            uint16_t fr  = rof_divu16(off, 46u);
            if (fr > 171u) fr = 171u;             // clamp into the 172-row door bitmap (green pad)
            runScan[nRuns] = (uint8_t)k;
            runRow[nRuns]  = fr;
            nRuns++;
        }
        prevA = a;
    }
    // Skip the copper rewrite when the run structure is unchanged since last frame (the common idle
    // case) — the bitmap-content refresh above is enough then.  Keeps idle standby free of redundant
    // per-frame copper writes while still tracking every DL change during the scroll.
    static uint8_t  sScan[kMaxDoorRuns]; static uint16_t sRow[kMaxDoorRuns]; static int sN = -1;
    bool same = (nRuns == sN);
    for (int i = 0; same && i < nRuns; i++) if (sScan[i] != runScan[i] || sRow[i] != runRow[i]) same = false;
    if (same) return;
    for (int i = 0; i < nRuns; i++) { sScan[i] = runScan[i]; sRow[i] = runRow[i]; }
    sN = nRuns;
    standbyCopper->setTerrainRuns(*doorScrollBitmap, runScan, runRow, nRuns);
}

// ---- public interface --------------------------------------------------------
// 2bpp→Amiga plane-pair decode LUT (filled by buildDecode2bppLut below; used by the
// cockpit/title/compass decoders).
static uint8_t s_dec2bppP1[256], s_dec2bppP2[256];
static bool s_dec2bppReady = false;
static void buildDecode2bppLut();
#ifdef ROF_FLIGHT_PROBE
// Rescue-figure/clean/mask Bitmap chip addresses (want <0x200000 => the combineWithMask blitter
// path is taken, not the slow CPU fallback).  Defined before initialize() so it can set them.
extern "C" volatile uint32_t g_figBmpAddr = 0, g_cleanBmpAddr = 0, g_maskBmpAddr = 0;
#endif
void RescueOnFractalus::initialize()
{
    titleBitmap   = Bitmap::allocate(kW, kTitleHeight,   kBP2, true);
    terrainBitmap = Bitmap::allocate(kW, kViewportFullHeight, kBP3, true);  // FLIGHT-ONLY (double-buffered); 47 rows incl. wing band
    // Second flight terrain buffer for double-buffering renderFlightDirect (see header).
    terrainBitmapBack = Bitmap::allocate(kW, kViewportFullHeight, kBP3, true);
    // Dot side-buffer (see header): off-display scratch the rasterizer ORs plane2 dots into.
    // Same 3bp interleaved layout as the terrain buffers so kRow120 geometry matches; MEMF_CLEAR'd.
    terrainDotBuffer = Bitmap::allocate(kW, kViewportFullHeight, kBP3, true);
    // Arm the rasterizer's plane2 dot target ONCE, here — terrainDotBuffer lives for the whole run
    // and the rasterizer only runs in flight, so g_flightDotPlane is now never null when it plots.
    // That lets draw_dot drop its per-plot null test (TerrainRasterizeAssembler.s).  flightKickBackClear
    // re-affirms the same pointer each flight frame (harmless).
    g_flightDotPlane = (uint8_t*)terrainDotBuffer->data + 40;
    buildDrawDotRowOff();   // rasterizer dot row-offset table (before the first flight frame)
    buildDotColTables();    // ...and its column mask/offset pair (same deadline)
    // Shared single-buffered pre-flight viewport bitmap for Standby / Doors (door halves) / Planet /
    // Stars — the scenes that never composite together in one frame (unlike the tunnel reveal, which
    // coexists with the door halves during Doors, so it keeps its own tunnelBitmap).  Kept separate
    // from the flight terrainBitmap so flight-side rendering can never clobber a still-displayed
    // pre-flight frame at a scene handoff (was the one-frame planet→flight black-band glitch).
    viewportBitmap  = Bitmap::allocate(kW, kViewportFullHeight, kBP3, true);
    // Level-select elevator door scroll: tall door bitmap = 85 field rows + a full viewport (86)
    // of green-door pad below, so BPLxPT + dl_src_index*rowstride (max $55=85) always stays in
    // bounds with the 86-row viewport reading rows [D..D+85].
    doorScrollBitmap = Bitmap::allocate(kW, 172, kBP3, true);
#ifdef ROF_FLIGHT_PROBE
    extern volatile uint32_t g_terrainBmpAddr;   // chip addr of terrainBitmap->data (Stage 1 verifier dump)
    g_terrainBmpAddr = (uint32_t)terrainBitmap->data;
#endif
    if (!s_dec2bppReady) buildDecode2bppLut();   // 2bpp→Amiga plane-pair LUT (cockpit/title decode)
    if (!s_shotExpandReady) buildShotExpandLut(); // 8-bit player row → 16px sprite LUT (shot/P3 mirrors)
    // Rescue-figure overlay + clean-terrain snapshot (chip Bitmaps for the blitter composite).
    // s_cleanBmp mirrors terrainBitmap's layout exactly so combineWithMask's per-row modulos line up.
    s_cleanBmp   = Bitmap::allocate(kW, kViewportFullHeight, kBP3, true);   // 3bp interleaved, == terrain
    s_figBmp     = Bitmap::allocate(kW, 43, 2, true);                       // 2bp interleaved (figure planes)
    s_figMaskBmp = Bitmap::allocate(kW, 43, 1, true);                       // 1bp opaque mask
    g_figP1 = (uint8_t*)s_figBmp->data;          // plane1 base (row stride 80)
    g_figP2 = (uint8_t*)s_figBmp->data + 40;     // plane2 base (offset 40, row stride 80)
    g_figM  = (uint8_t*)s_figMaskBmp->data;      // mask (row stride 40)
#ifdef ROF_FLIGHT_PROBE
    g_figBmpAddr = (uint32_t)s_figBmp->data; g_cleanBmpAddr = (uint32_t)s_cleanBmp->data;
    g_maskBmpAddr = (uint32_t)s_figMaskBmp->data;
#endif
    cockpitBitmap = Bitmap::allocate(kW, kCockpitH, kBP3, true);  // 3bp: bit-7 chars → red
    tunnelBitmap  = Bitmap::allocate(kW, kTerrainHeight, kBP3, true);  // door-gap reveal
    titleScreenBitmap = Bitmap::allocate(kW, kH, kBP3, true);  // 3bp: black + COLPF0-3 text pens

    // (The tunnel rings are decoded into tunnelBitmap from the $1000 field by
    // decodeTunnelRect/decodeTunnelBand, triggered by the platform_tunnel_rings_drawn() hook
    // when the genuine boot_standby_launch_driver draws them — not at init; see decodeTunnelRect.)

    leftPost   = Sprite::allocate(kHT);
    rightPost  = Sprite::allocate(kHT);
    nullSprite = Sprite::allocate(0);
    energyIndicatorSprite = Sprite::allocate(kEnergyRows);    // energy bar: 56 px high when full (same as the altimeter)
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
    // Player laser shot (Atari P2 $0E32) on the idle sprite ch4.  Two fixed-height buffers,
    // double-buffered by buildShotSprite (VBI); Y moves to the shot's current row each frame.
    shotSprite     = Sprite::allocate(kShotRows);
    shotSpriteBack = Sprite::allocate(kShotRows);
    // Targeting-Scope P3 object on the idle sprite ch3 (reused below the frame via SPR3PT re-point).
    scopeP3Sprite = Sprite::allocate(kScopeP3Rows);
    // Main-Window P3 object on ch7 (altimeter ship, free in the viewport) via SPR7PT multiplex.
    viewportP3Sprite = Sprite::allocate(kViewportP3Rows);
    // Long Range Scanner (#13) guide dot (Atari missile M2) on ch2 via SPR2PT re-point.
    scannerDotSprite = Sprite::allocate(kScannerDotRows);
    if (!leftPost || !rightPost || !nullSprite || !energyIndicatorSprite || !altimeterSprite
        || !altimeterShipSprite || !flLeftPost || !flRightPost || !flLeftTri || !flRightTri
        || !ahLeft || !ahRight || !shotSprite || !shotSpriteBack || !scopeP3Sprite
        || !viewportP3Sprite || !scannerDotSprite) return;
    // Starfield sprites: each Atari player P0/P2/P3 is a 32-cc quad, drawn as a pair of strips —
    // starSprite[2c] (low, at kStarX[c]) + starSprite[2c+1] (high, +16 px) — both at the windscreen
    // top (player scanline $32 → Amiga Y = kTerrainLine).  Height = kViewportFullHeight so VSTOP
    // lands on PlanetCopperList's cockpit line (180): the copper re-points channel 2 to the throttle
    // gauge THERE, and the re-arm requires the outgoing (P0-low) sprite's post-VSTOP control-word
    // fetch to coincide with the re-point (only kStarRows=89 rows carry star data; the rest blank).
    // Each star sprite is backed by an oversized ring buffer (kStarRingSlots rows) holding the
    // whole scene's scroll laid out linearly; the Sprite object wraps the ring base (window 0)
    // for the copper's initial pointer.  Sprite::allocate(h) gives (h+2) cleared chip slots.
    for (int i = 0; i < 6; i++) {
        starSprite[i] = Sprite::allocate(kStarRingSlots - 2);
        if (!starSprite[i]) return;
        starRing[i] = starSprite[i]->data();
        // Precompute the constant POS/CTL control words for this sprite (X per sprite, Y =
        // kTerrainLine, VSTOP = +kViewportFullHeight = line 180 so the channel-2 gauge re-point
        // still coincides).  Written big-endian as the hardware expects (see Sprite::setX/setY).
        uint16_t x = (i & 1) ? (uint16_t)(kStarX[i >> 1] + 16) : kStarX[i >> 1];
        uint16_t vstop = (uint16_t)(kTerrainLine + kViewportFullHeight);
        starCtl[i][0] = (uint16_t)((kTerrainLine << 8) | (x >> 1));   // SV7-0, SH8-1
        starCtl[i][1] = (uint16_t)((vstop << 8) | (x & 1));           // EV7-0 | SH0 (ATT/SV8/EV8 = 0)
        starRing[i][0] = starCtl[i][0];                              // seed the window-0 control slot
        starRing[i][1] = starCtl[i][1];
    }
    starWindow = 0; starSpritesValid = false;
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
#ifdef ROF_FLIGHT_PROBE
    { extern volatile uint32_t g_altimSprAddr, g_altimShipSprAddr, g_energySprAddr, g_viewportP3SprAddr, g_scopeP3SprAddr;
      g_altimSprAddr     = (uint32_t)altimeterSprite->data();
      g_altimShipSprAddr = (uint32_t)altimeterShipSprite->data();
      g_energySprAddr    = (uint32_t)energyIndicatorSprite->data();
      g_viewportP3SprAddr = (uint32_t)viewportP3Sprite->data();
      g_scopeP3SprAddr    = (uint32_t)scopeP3Sprite->data(); }
#endif

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
        // Terrain (door) region points at the TALL doorScrollBitmap (row 0 == the resting doors),
        // so the level-select "elevator" scroll is a pure BPLxPT row-offset with no buffer handoff:
        // the standby door decode writes doorScrollBitmap and the scroll just moves the pointer.
        standbyCopper->buildLayout(*titleBitmap, *doorScrollBitmap, *cockpitBitmap,
                                   *leftPost, *rightPost, *nullSprite);

    // Static stars/planet viewport fixed copper list (the line-doubled mode-D band),
    // same build-once + poke-in-place scheme; renderFrame installs it during rsStars.
    planetCopper = new PlanetCopperList();
    if (planetCopper && planetCopper->data())
        planetCopper->buildLayout(*titleBitmap, *viewportBitmap, *cockpitBitmap,
                                    *leftPost, *rightPost, *energyIndicatorSprite, starSprite);

    // Static flight fixed copper list (scene 7), same build-once + poke scheme;
    // renderFrame installs it during rsFlight.  HUD sprites are poked in later by the
    // ported flight VBI; buildLayout seeds posts + gauge + nulls.
    flightCopper = new FlightCopperList();
    if (flightCopper && flightCopper->data())
        flightCopper->buildLayout(*titleBitmap, *terrainBitmap, *cockpitBitmap,
                                  *flLeftPost, *flLeftTri, *flRightPost, *flRightTri, *nullSprite,
                                  *ahLeft, *ahRight, *scopeP3Sprite);
#ifdef ROF_FLIGHT_PROBE
    { extern volatile uint32_t g_flightCopperAddr;
      if (flightCopper) g_flightCopperAddr = (uint32_t)flightCopper->data(); }
#endif

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

    // Precompute the starfield glyph tables (see kStarGlyphLo/Hi decl): each star sub-position
    // → a 4-px dot at its faithful 0/8/20/28-cc offset, split across the low (px 0-15) and high
    // (px 16-31) sprites of the player's two-sprite quad.  bit15 = leftmost sprite pixel.
    for (int i = 0; i < 256; i++) {
        uint16_t lo = 0, hi = 0;
        if (i & 0x80) lo |= 0xF000u;   // $80 (bit7): cc  0 → low  px 0-3
        if (i & 0x20) lo |= 0x00F0u;   // $20 (bit5): cc  8 → low  px 8-11
        if (i & 0x04) hi |= 0x0F00u;   // $04 (bit2): cc 20 → high px 4-7  (20-16)
        if (i & 0x01) hi |= 0x000Fu;   // $01 (bit0): cc 28 → high px 12-15 (28-16)
        kStarGlyphLo[i] = lo;
        kStarGlyphHi[i] = hi;
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
        // Windscreen-band split (see kBandP1/2/3): only the grey frame (value 3) -> plane3
        // (color04).  value 1 -> plane1 (color01 salmon bar), value 2 -> plane2 (color02 centre
        // marker), value 0 -> all planes 0 -> color00 = terrain body (the L/R edge regions, which
        // the Atari band draws in COLBK=$DC = the terrain pen0; the grey corner-triangle sprites
        // overlay on top).  So values 0/1/2 are plane3 HOLES taking the fading terrain palette.
        uint8_t bp1 = 0, bp2 = 0, bp3 = 0;
        for (int i = 0; i < 4; i++) {
            uint8_t px   = (uint8_t)((s >> (6 - i * 2)) & 3u);
            uint8_t mask = (uint8_t)(0xC0u >> (i * 2));
            if      (px == 3) bp3 |= mask;                    // grey windscreen frame -> plane3 -> color04
            else if (px == 1) bp1 |= mask;                    // clearance bar -> hole -> color01
            else if (px == 2) bp2 |= mask;                    // centre marker -> hole -> color02
            // px == 0: L/R edge -> all planes 0 -> color00 (terrain body); sprite triangle on top
        }
        kBandP1[s] = bp1; kBandP2[s] = bp2; kBandP3[s] = bp3; kBandOW[s] = (uint8_t)(bp1 | bp2);
        uint8_t ph = (uint8_t)((s >> 4) & 0xF), pl = (uint8_t)(s & 0xF);   // GTIA-10
        // FORWARD tunnel (kGtia10P*, decodeTunnelRect) — the long-working mapping, UNCHANGED:
        // value-0 (exit-clear) -> spare pen7 (color07=black); value-8 (background/corner) -> pen0
        // (color00 = the tunnel corner $08D8, carried into the band); others straight through.
        // Use gh/gl so kDoorP below still sees the ORIGINAL ph/pl.
        uint8_t gh = ph ? ph : 7, gl = pl ? pl : 7;
        kGtia10P1[s] = (uint8_t)(((gh & 1) ? 0xF0u : 0u) | ((gl & 1) ? 0x0Fu : 0u));
        kGtia10P2[s] = (uint8_t)(((gh & 2) ? 0xF0u : 0u) | ((gl & 2) ? 0x0Fu : 0u));
        kGtia10P3[s] = (uint8_t)(((gh & 4) ? 0xF0u : 0u) | ((gl & 4) ? 0x0Fu : 0u));
        // BOOST reverse tunnel (kGtia10BoostP*, decodeBoostViewport only) — additionally move value-2
        // (the outermost ring, COLPM2) -> pen0 (color00) so the ring is DRAWN in the same register as
        // the windscreen-band corner triangle (mode-D value-0 -> color00), and value-8 (COLBK
        // background: star field + unrevealed rows) -> the freed pen2 (color02) so the background keeps
        // its own register and does not inherit the outermost-ring colour (which caused teal edges).
        uint8_t bh = (ph == 0) ? 7 : (ph == 2) ? 0 : (ph == 8) ? 2 : ph;
        uint8_t bl = (pl == 0) ? 7 : (pl == 2) ? 0 : (pl == 8) ? 2 : pl;
        kGtia10BoostP1[s] = (uint8_t)(((bh & 1) ? 0xF0u : 0u) | ((bl & 1) ? 0x0Fu : 0u));
        kGtia10BoostP2[s] = (uint8_t)(((bh & 2) ? 0xF0u : 0u) | ((bl & 2) ? 0x0Fu : 0u));
        kGtia10BoostP3[s] = (uint8_t)(((bh & 4) ? 0xF0u : 0u) | ((bl & 4) ? 0x0Fu : 0u));
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
    mem[MEM_display_flags] = 0xC8;   // COLBK source for terrain DLI → green
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

    // The tunnel bitmap is NOT decoded here: the genuine boot_standby_launch_driver draws the ring
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

// decodeTunnelRect: decode the sub-rectangle rows [rowLo..rowHi] × displayed bytes
// [byteLo..byteHi] of the GTIA-10 tunnel-ring field at mem[$1000] into the tunnel bitmap.
// $1000 (NOT $2000, which holds the door field) is where the genuine boot_standby_launch_driver renders
// the rings — draw_frame_pattern_seq plots through the $073D/$0793 row-address table built
// for base $1000 (so table index r == row r == $1000 + r*46), and draw_ring_frame_step streams
// its expanding black ring-clear frames into the same buffer.  No shadow / compare: decoding a
// byte to LUT[$1000[byte]] is idempotent, so re-decoding a superset of the changed bytes is
// byte-identical to a per-byte-gated pass — the shadow only ever saved work, never correctness.
void RescueOnFractalus::decodeTunnelRect(int rowLo, int rowHi, int byteLo, int byteHi)
{
    if (!tunnelBitmap) return;
    if (rowLo < 0) rowLo = 0;
    if (rowHi > (int)kTerrainHeight - 1) rowHi = (int)kTerrainHeight - 1;
    if (byteLo < 0) byteLo = 0;
    if (byteHi > 39) byteHi = 39;
    if (rowLo > rowHi || byteLo > byteHi) return;   // degenerate band strip — nothing to do
    // Pointer-walk (no per-row 68000 multiplies — row*46/*120 would be __mulsi3 soft-multiplies
    // each row).  Compute the row-0 bases once, then += stride per row, as render() does.
    const uint8_t* src = (const uint8_t*)&mem[tunnelSrcBase + rowLo * 46 + 4];  // +4: wide-field crop
    uint8_t*       p1  = (uint8_t*)tunnelBitmap->data + rowLo * 120;
    for (int row = rowLo; row <= rowHi; row++) {
        uint8_t* pp2 = p1 + 40; uint8_t* pp3 = p1 + 80;
        for (int b = byteLo; b <= byteHi; b++) {
            uint8_t s = src[b];
            p1[b] = kGtia10P1[s]; pp2[b] = kGtia10P2[s]; pp3[b] = kGtia10P3[s];
        }
        src += 46; p1 += 120;                        // walk to next row
    }
}

// decodeBoostViewport: the boost reverse-tunnel row-by-row REVEAL.  Each viewport row's
// source is read from the live $3000 launch DL's per-row mode-F LMS word (maintained by the
// faithful emit_dl_coord_pairs, which the reverse-ring loop calls once per VBI-paced step to
// convert rows $2000→$1000 from the centre outward — verified on-Amiga: at draw_pattern_byte
// $09 exactly rows 33-42 + 57-66 held $1xxx, everything else still the DL leftover $2f74).
// So: a row whose LMS is in [$1000,$2000) has been converted → decode the rings straight from
// that LMS (this also gives the rev strand's mirrored bottom-half addresses for free); every
// other row is not yet revealed → decode stars from $2000+row*46 (NOT the raw $2f74 leftover,
// which points one row past the stars field).  During the stars sub-phase ($008D==0) the DL is
// all $2f74 so every row is stars — identical to the old whole-buffer $2000 decode.  This
// reproduces the Atari's row-by-row reveal and removes the whole-buffer $008D snap (the bowtie
// flash) at the stars→tunnel handoff.  Full-viewport decode/frame is fine for a brief cinematic.
void RescueOnFractalus::decodeBoostViewport()
{
    if (!tunnelBitmap) return;
    const bool tunnel = (mem[0x008D] != 0u);   // reverse-ring active (0 = stars sub-phase)
    // Reverse-tunnel reveal band.  The $1000 ring field is a FULL nested-rectangle tunnel; the
    // reveal shows a CONTIGUOUS band of rows centred on the vanishing point (rows 42/43), growing
    // symmetrically outward, decoded LINEARLY (row r -> $1000+r*46, as the forward tunnel).  A
    // symmetric band around the centre is a clean small tunnel; the earlier bugs came from decoding
    // an ASYMMETRIC set (the Amiga emit converts fwd rows 42->0 but rev rows 57->85, skipping the
    // centre 43-56) — a lopsided slice reads as a bowtie/staircase.  Measured on the Atari (rv_8.6,
    // 22 rows in: rows 32-53 revealed = symmetric around 42.5).  So derive the band from the FWD
    // strand alone: K = the topmost DL row (0..42) that emit has pointed at $1xxx; reveal [K, 85-K]
    // and black (stars) outside.  As the reverse ring runs, K falls 42->0 and the tunnel grows.
    int K = 43;
    if (tunnel) {
        for (int row = 0; row <= 42; row++) {
            uint16_t lms = (uint16_t)(mem[0x300Au + row * 3] | (mem[0x300Bu + row * 3] << 8));
            if (lms >= 0x1000u && lms < 0x2000u) { K = row; break; }
        }
    }
    uint8_t* p1 = (uint8_t*)tunnelBitmap->data;
    for (int row = 0; row < (int)kTerrainHeight; row++) {
        // Stars ($008D==0): whole viewport = $2000 starfield.  Tunnel: rings ($1000+row*46) inside
        // the symmetric reveal band [K, 85-K]; the black space surround OUTSIDE it.
        const bool rings = tunnel && row >= K && row <= 85 - K;
        const uint16_t base = (uint16_t)((rings ? 0x1000u : 0x2000u) + row * 46);
        const uint8_t* src = (const uint8_t*)&mem[base + 4];   // +4: wide-field crop
        uint8_t* pp2 = p1 + 40; uint8_t* pp3 = p1 + 80;
        for (int b = 0; b < 40; b++) {
            uint8_t s = src[b];
            p1[b] = kGtia10BoostP1[s]; pp2[b] = kGtia10BoostP2[s]; pp3[b] = kGtia10BoostP3[s];  // boost LUT: value-2->color00, value-8->color02
        }
        p1 += 120;
    }
}

// decodeTunnelBand: re-decode only the frame band draw_ring_frame_step just wrote — the
// picture-frame between the previous outer rectangle (inner, before this step's expansion)
// and the new outer rectangle.  The four g_tun* bound pairs come from draw_symmetric_span_loop's
// boundary regs snapshotted before/after the loop (rof_native_amiga.cpp).  Rows are $073D-table
// indices (== decode rows); columns are PIXEL cols → displayed byte = (px>>1) - 4.  We decode
// four thin strips whose union covers the whole band (idempotent overlap at the corners):
// full-width top+bottom bands + left/right column strips over the middle rows.
void RescueOnFractalus::decodeTunnelBand()
{
    const int R0 = g_tunRowLo,   R1 = g_tunRowHi;     // outer rows (R0 = top, small index)
    const int r0 = g_tunInRowLo, r1 = g_tunInRowHi;   // inner rows (previous outer)
    // Pixel col → displayed byte.  outL/outR = new outer edges, inL/inR = inner edges.
    const int outL = ((int)g_tunColLpx   >> 1) - 4;
    const int outR = ((int)g_tunColRpx   >> 1) - 4;
    const int inL  = ((int)g_tunInColLpx >> 1) - 4;
    const int inR  = ((int)g_tunInColRpx >> 1) - 4;
    // Row-spill guard: fill_horizontal_span writes bytes [.. $009D>>1] within a row; if that
    // exceeds the 46-byte row width it wraps into the NEXT row's low bytes, which the strips
    // would miss.  Rare (would need the frame to grow past the field edge), but decode the full
    // outer row extent full-width if it can happen — still shadow-free, just less selective.
    if (((int)g_tunColRpx >> 1) >= 46) { decodeTunnelRect(R0, R1, 0, 39); return; }
    decodeTunnelRect(R0, r0, 0, 39);      // top band, full width
    decodeTunnelRect(r1, R1, 0, 39);      // bottom band, full width
    decodeTunnelRect(r0, r1, outL, inL);  // left edge strip over the middle rows
    decodeTunnelRect(r0, r1, inR, outR);  // right edge strip over the middle rows
}

// renderViewportModeD: decode the stars/planet viewport buffer mem[$1000] as an
// ANTIC mode-D field into viewportBitmap (the DEDICATED planet buffer, NOT flight's
// shared terrainBitmap).  Layout (verified vs launch_5_planet.a8s
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
    if (!viewportBitmap) return;
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
    // stores when the 4-byte source group is unchanged.  plane3 is always 0 in mode-D and
    // is zeroed by the full-frame blitter clear below (nothing writes it during the viewport,
    // so it stays 0 thereafter).  vdest is chip-aligned and the +40/+80 offsets keep every
    // long aligned.
    const bool full = viewportForceFull || (srcBase != viewportLastBase);
    viewportForceFull = false;
    viewportLastBase  = srcBase;

    // On a full (re)decode the naive path re-stores all 470 groups into terrainBitmap — ~50ms,
    // CHIP-write-bound under the viewport's 3-bitplane + sprite DMA.  Instead, blitter-clear the
    // WHOLE bitmap (all 3 planes, incl. plane3 which the tunnel reveal dirtied with pens 4-7) to
    // pen 0 and zero the shadow, then fall through to the ordinary change-skip pass.  On stars/
    // planet ENTRY the $1000 field is mostly value 0 (black space, the planet still far), and
    // value 0 decodes to pen 0 = the freshly-cleared bitmap, so the skip drops those groups and
    // only the sparse non-zero stars/planet cells are stored.  Byte-identical output; the entry
    // cost is now proportional to content, not the full 47-row field.  The blitter clear must
    // finish before the CPU writes the non-zero groups, so wait right after kicking it.
    bool clearedFull = false;
    if (full) {
#ifdef ROF_FLIGHT_PROBE
        extern volatile unsigned long g_starClrTicks; unsigned long _c0 = rof_subclock();
#endif
        // The clear was normally kicked in perFrameWork (overlapping buildStarSprites); if not
        // (base-change mid-stream, not the stars entry), kick it here.  Either way the shadow-zero
        // loop below runs while the blit is in flight, then we wait for it before the CPU writes.
        if (!viewportClearKicked)
            AmigaHardware::blitterClear((uint16_t*)viewportBitmap->data, 60, (uint16_t)rows, 0);
        for (int i = 0; i < rows * 10; i++) viewportShadow[i] = 0u;   // FAST RAM, overlaps the blit
        AmigaHardware::blitterWait();
        viewportClearKicked = false;
        clearedFull = true;
#ifdef ROF_FLIGHT_PROBE
        if (g_starEntryVbi == 0 || g_starClrTicks == 0) g_starClrTicks = rof_subclock() - _c0;
#endif
    }

    // Dirty-row band.  For the stars/planet source ($1000) the planet renderer
    // (draw_vline_pair, the only writer of this field — validated: every shadow-detected
    // change lay inside its reported extent) records the rows it touched in
    // g_planetRowLo/Hi.  Decode only that band instead of scanning all 43 rows: mem[] is FAST
    // RAM but every 68000 access is slow + volatile, so the full per-frame scan cost ~17 ms
    // even though only ~3 rows change.  Entry frames (clearedFull) scan all rows but skip the
    // (now-cleared) zero groups.
    extern volatile unsigned long g_planetRowLo, g_planetRowHi;
    int rStart = 0, rEnd = rows - 1;
    if (srcBase == 0x1000u && !clearedFull) {
        rStart = (int)g_planetRowLo;
        rEnd   = (int)g_planetRowHi;
        g_planetRowLo = 9999; g_planetRowHi = 0;             // consume for next frame
        if (rEnd < rStart) return;                           // nothing drawn this frame
        if (rStart < 0) rStart = 0;
        if (rEnd >= rows) rEnd = rows - 1;
    }

    const uint8_t* src = (const uint8_t*)&mem[srcBase + kCrop] + rof_mulu16((uint16_t)rStart, (uint16_t)stride);
    uint8_t* vdest    = (uint8_t*)viewportBitmap->data + rof_mulu16((uint16_t)rStart, 120u);
    uint32_t* shadow  = viewportShadow + rof_mulu16((uint16_t)rStart, 10u);
#ifdef ROF_FLIGHT_PROBE
    extern volatile unsigned long g_vpDecMax, g_vpDecMaxVbi, g_vpDecMaxRows;
    unsigned long _vp0 = rof_subclock();
#endif
    for (int row = rStart; row <= rEnd; row++, src += stride) {
        const uint8_t* rs = src;
        uint32_t* q1 = (uint32_t*)vdest;
        uint32_t* q2 = (uint32_t*)(vdest + 40);
        for (int b = 0; b < 10; b++, q1++, q2++, shadow++) {   // 10 longs = 40 bytes
            // The Amiga 68000 is big-endian, so one aligned long read of the source == the
            // byte-packed key (s0<<24|s1<<16|s2<<8|s3) — halve the scan cost by reading a single
            // long for the (dominant) unchanged/cleared case instead of 4 separate byte reads;
            // only decompose into bytes for the sparse groups that actually decode.  (Amiga-only
            // file; rs is 4-aligned: srcBase $1000 + kCrop 4, stride 48, b*4 all keep alignment.)
            uint32_t key = *(const uint32_t*)rs; rs += 4;
            if (key == *shadow) continue;                    // 4-byte group unchanged (or still cleared-to-0)
            *shadow = key;
            uint8_t s0 = (uint8_t)(key >> 24), s1 = (uint8_t)(key >> 16),
                    s2 = (uint8_t)(key >>  8), s3 = (uint8_t)key;
            *q1 = ((uint32_t)kModeDP1[s0] << 24) | ((uint32_t)kModeDP1[s1] << 16) |
                  ((uint32_t)kModeDP1[s2] <<  8) |  (uint32_t)kModeDP1[s3];
            *q2 = ((uint32_t)kModeDP2[s0] << 24) | ((uint32_t)kModeDP2[s1] << 16) |
                  ((uint32_t)kModeDP2[s2] <<  8) |  (uint32_t)kModeDP2[s3];
#ifdef ROF_FLIGHT_PROBE
            extern volatile unsigned long g_starGroups; if (clearedFull) g_starGroups++;
#endif
        }
        vdest += 120;                                        // one interleaved scanline
    }
#ifdef ROF_FLIGHT_PROBE
    { unsigned long _d = rof_subclock() - _vp0;
      if (_d > g_vpDecMax) { g_vpDecMax = _d; g_vpDecMaxVbi = rof_subclock()/313u;
                             g_vpDecMaxRows = (unsigned long)(rEnd - rStart + 1); } }
#endif
}

#ifdef ROF_FLIGHT_PROBE
// ── Pilot-rescue resume-frame dot-dropout diagnostic ring buffer ───────────────
// One record per renderFlightDirect() entry, capturing exactly the state that decides which
// buffer is displayed vs painted vs cleared across the rescue pause + resume, so we can see how
// they desync (the doc's investigation-plan step 1).  A rescue can only be reached by interactive
// play, so we can't drive this headlessly — instead the ring auto-FREEZES a few frames after the
// pause ends (rescueFigure seen true then false), preserving the window for a later gdb dump
// (amiga/diag_rescue.gdb) without the history scrolling away.  All globals are read from the gdb
// stub.  Buffer ids: 0=terrainBitmap, 1=terrainBitmapBack, 2=null.  Path: 1=rescue-pause branch,
// 2=hold (!terrainFresh early return), 3=normal render, 0=early null return.
#define RF_RING_N 128
extern "C" volatile unsigned short g_rfN = RF_RING_N;
extern "C" volatile unsigned short g_rfIdx = 0;      // next write slot (wraps); #records = min(count,N)
extern "C" volatile unsigned long  g_rfCount = 0;    // total records ever (pre-freeze)
extern "C" volatile unsigned char  g_rfFrozen = 0;   // 1 once the post-resume window elapsed
extern "C" volatile unsigned char  g_rfSawRescue = 0;
extern "C" volatile unsigned short g_rfPostResume = 0;
// Per-slot columns (parallel arrays so the gdb `while` loop can dump each cleanly).
extern "C" volatile unsigned long  g_rfFrame[RF_RING_N] = {0};   // platform_frame_count (g_vbiCount) at entry
extern "C" volatile unsigned char  g_rfPath[RF_RING_N]  = {0};
extern "C" volatile unsigned char  g_rfResc[RF_RING_N]  = {0};   // rescueFigure
extern "C" volatile unsigned char  g_rf3D[RF_RING_N]    = {0};   // mem[0x3D]
extern "C" volatile unsigned char  g_rf3E[RF_RING_N]    = {0};   // mem[0x3E]
extern "C" volatile unsigned char  g_rfDisp[RF_RING_N]  = {0};   // flightDisplayed id
extern "C" volatile unsigned char  g_rfBack[RF_RING_N]  = {0};   // back id (buffer to paint)
extern "C" volatile unsigned char  g_rfClr[RF_RING_N]   = {0};   // flightClearPending id
extern "C" volatile unsigned long  g_alComp = 0;   // live creature composites during the knock ($0632)
extern "C" volatile unsigned long  g_alTComp = 0;    // ticks in the composite + blitterDrain (excl. flip wait), knock
extern "C" volatile unsigned long  g_alRFD = 0;      // renderFlightDirect entries during the knock
extern "C" volatile unsigned long  g_alRFDresc = 0;  // ...of those, rescueFigure true
extern "C" volatile unsigned long  g_alRFDclean = 0; // ...of those, s_cleanValid true
extern "C" volatile unsigned long  g_alRF = 0;       // renderFrame entries during the knock
extern "C" volatile unsigned short g_alVV = 0;       // VVBLKI during the knock (want $4FF5)
extern "C" volatile unsigned char  g_alRFfl = 0;     // rsFlight at renderFrame during the knock
extern "C" volatile unsigned char  g_alRFvw = 0;     // rsViewport at renderFrame during the knock
extern "C" volatile unsigned char  g_rfFresh[RF_RING_N] = {0};   // g_flightTerrainFresh
extern "C" volatile short          g_rfFigLo[RF_RING_N] = {0};   // g_figRowLo
extern "C" volatile short          g_rfFigHi[RF_RING_N] = {0};   // g_figRowHi
// Cheap byte-sum checksums of plane1 (dots' silhouette) and plane2 (terrain dots) of BOTH fixed
// buffers, so we can see which buffer holds dots and when a clear wiped them.
extern "C" volatile unsigned long  g_rfP1a[RF_RING_N] = {0};  // terrainBitmap  plane1 sum
extern "C" volatile unsigned long  g_rfP2a[RF_RING_N] = {0};  // terrainBitmap  plane2 sum
extern "C" volatile unsigned long  g_rfP1b[RF_RING_N] = {0};  // terrainBitmapBack plane1 sum
extern "C" volatile unsigned long  g_rfP2b[RF_RING_N] = {0};  // terrainBitmapBack plane2 sum
// s_clean snapshot checksums (to catch a re-snapshot contaminating it mid-rescue) + whether the
// resume-frame dot-recovery restore was armed (s_wasRescuePause) at entry = fires this frame.
extern "C" volatile unsigned long  g_rfScP1[RF_RING_N] = {0};  // s_clean plane1 sum
extern "C" volatile unsigned long  g_rfScP2[RF_RING_N] = {0};  // s_clean plane2 sum
extern "C" volatile unsigned char  g_rfWasR[RF_RING_N] = {0};  // s_wasRescuePause at entry
extern "C" unsigned short platform_frame_count(void);   // returns g_vbiCount (PlatformAmiga.cpp)

static unsigned long rfPlaneSum(const uint8_t* base, int planeOff)
{
    unsigned long s = 0;
    const uint8_t* p = base + planeOff;                  // walk the plane row-by-row (+120), no per-row multiply
    for (int r = 0; r < 47; r++, p += 120) {
        for (int b = 0; b < 40; b++) s += p[b];
    }
    return s;
}
#endif  // ROF_FLIGHT_PROBE

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
    // The flight loop renders here and busy-waits on flightSwapPending — it never reaches the
    // renderFrame/pollEvents pump, so honour quit / BREAK-restart / SYSTEM-RESET here too (else a
    // BREAK pressed mid-flight leaves the viewport stuck on the trampoline's VVBLKI=$52B4).
    rof_check_restart();   // may __builtin_longjmp out
    if (!terrainBitmap || !terrainBitmapBack || !flightCopper) return;

    // Rescue "figure walks to the airlock": during a systems-off rescue at landing phase >=3 the
    // game runs animate_zoom_sequence, which draws the approaching pilot/alien figure as a BITMAP
    // into the mode-D flight field via plot_clipped_pixel (verified from rescue_pilot.a8s — NOT PMG;
    // the viewport PMG holds only static frame elements).  The Amiga sheds the field for the terrain
    // body, so the figure would be dropped.  The terrain is FROZEN during the pause (no terrain_draw
    // runs), so rather than re-render the whole viewport every frame we do DIRTY-RECT: snapshot the
    // clean frozen terrain once, suppress the per-frame back-buffer clear so the buffers retain it,
    // and each frame only ERASE the previous figure's row box (restore plane1+plane2 from the
    // snapshot) and draw the new figure.  Reuses the exact normal VBI-synced flip.
    const bool rescueActive = (mem[0x003E] != 0);
    const bool rescueFigure = (rescueActive && mem[0x003D] >= 3);
#if defined(ROF_FLIGHT_PROBE)
    // Locate where the creature composite drops out during the knock ($0632): entered renderFlightDirect
    // at all / rescueFigure true / s_cleanValid true.  (g_alComp below counts the actual composite.)
    { extern volatile unsigned long g_alRFD, g_alRFDresc, g_alRFDclean;
      if (mem[0x0632]) { g_alRFD++; if (rescueFigure) g_alRFDresc++; if (s_cleanValid) g_alRFDclean++; } }
#endif
    // $3E nonzero->zero edge = the rescue truly ended (this is the resume frame).  Latch a one-shot
    // dot restore for the next rendering frame; never set during the pause or its mid-zoom $3D dips.
    // GATE on s_cleanValid: $3E (rescueActive) also goes active when systems are switched off with NO
    // pilot in range (no walk animation, $3D never reaches 3 -> rescueFigure never true -> s_clean is
    // never snapshotted this pause).  In that case the clear was NOT suppressed (s_flightRescuePause
    // stayed false), so the normal render path already produces correct fresh dots on resume — arming
    // the restore there would copy STALE s_clean (from a prior rescue, or empty) over them, which is
    // the "resume shows a stale/incorrect frame" bug.  Only restore when a real rescue-figure pause
    // actually captured s_clean.
    if (s_prevRescueActive && !rescueActive && s_cleanValid) { s_resumeRestorePend = true; s_resumeClearPend = true; }
    s_prevRescueActive = rescueActive;

    // Dot side-buffer flip DRAIN.  The normal path DEFERS its flip-wait: after requesting the flip it
    // returns immediately so the next terrain compute overlaps the flip's vblank (that compute writes
    // the dot side-buffer, NOT a display buffer, so it has no dependency on the flip).  Here — at the
    // TOP of the next entry, AFTER that compute has run (and long since passed the vblank) — we finally
    // block on the pending swap, so it is ~0.  Placed before every branch so flightDisplayed/`back` are
    // always current (the rescue-pause composite + buffer selection depend on it) and any deferred flip
    // is drained before the pause path takes over.  No-op when nothing is pending (first frame / pause).
#ifdef ROF_FLIGHT_PROBE
    {   // integer frame cost of this drain (see g_flipWaitFrames in PlatformAmiga.cpp)
        extern volatile unsigned long g_flipWaitFrames, g_flipWaitCalls;
        const unsigned short _fw0 = platform_frame_count();
        while (flightSwapPending) { }
        g_flipWaitFrames += (unsigned long)(unsigned short)(platform_frame_count() - _fw0);
        g_flipWaitCalls++;
    }
#else
    while (flightSwapPending) { }
#endif
#if defined(ROF_FLIGHT_PROBE) && !defined(ROF_PROFILE_NORING)
    // Record this entry into the rescue diagnostic ring (see the block above renderFlightDirect).
    if (!g_rfFrozen) {
        auto bid = [&](Bitmap* b) -> unsigned char {
            return (unsigned char)(b == terrainBitmap ? 0 : (b == terrainBitmapBack ? 1 : 2)); };
        Bitmap* const backPrev = (flightDisplayed == terrainBitmapBack) ? terrainBitmap : terrainBitmapBack;
        const unsigned char path = rescueFigure ? 1 : (!g_flightTerrainFresh ? 2 : 3);
        const unsigned i = g_rfIdx;
        g_rfFrame[i] = platform_frame_count(); g_rfPath[i] = path;
        g_rfResc[i]  = rescueFigure ? 1 : 0;
        g_rf3D[i]    = mem[0x003D];         g_rf3E[i]   = mem[0x003E];
        g_rfDisp[i]  = bid(flightDisplayed); g_rfBack[i] = bid(backPrev);
        g_rfClr[i]   = bid(flightClearPending);
        g_rfFresh[i] = (unsigned char)(g_flightTerrainFresh ? 1 : 0);
        g_rfFigLo[i] = (short)g_figRowLo;   g_rfFigHi[i] = (short)g_figRowHi;
        g_rfP1a[i]   = rfPlaneSum((const uint8_t*)terrainBitmap->data, 0);
        g_rfP2a[i]   = rfPlaneSum((const uint8_t*)terrainBitmap->data, 40);
        g_rfP1b[i]   = rfPlaneSum((const uint8_t*)terrainBitmapBack->data, 0);
        g_rfP2b[i]   = rfPlaneSum((const uint8_t*)terrainBitmapBack->data, 40);
        g_rfScP1[i]  = s_cleanBmp ? rfPlaneSum((const uint8_t*)s_cleanBmp->data, 0)  : 0;
        g_rfScP2[i]  = s_cleanBmp ? rfPlaneSum((const uint8_t*)s_cleanBmp->data, 40) : 0;
        g_rfWasR[i]  = (unsigned char)(s_resumeRestorePend ? 1 : 0);
        g_rfIdx = (unsigned short)((i + 1) % RF_RING_N);
        g_rfCount++;
        // Track the rescue->resume window and freeze ~24 frames after the pause ends so the
        // capture survives until a gdb break.  (rescueFigure can toggle mid-animation, so require
        // a sustained run of non-rescue frames after having seen a rescue.)
        if (rescueFigure) { g_rfSawRescue = 1; g_rfPostResume = 0; }
        else if (g_rfSawRescue) { if (++g_rfPostResume >= 24) g_rfFrozen = 1; }
    }
#endif
    if (rescueFigure) {
        // Entry: snapshot the clean frozen terrain (all 3 planes, 47 interleaved rows) from the
        // currently-displayed buffer, which still holds the last real frame here.  Mark that buffer
        // seeded (it IS the clean terrain); the other buffer gets seeded on its first use below.
        // Blitter copy (Bitmap::copy) — runs on the blitter, parallel to the CPU.
        if (!s_cleanValid && flightDisplayed) {
            s_cleanBmp->copy(*flightDisplayed, 0, 0, 0, 0, kW, 47);
            s_cleanValid = true;
            const int di = (flightDisplayed == terrainBitmapBack) ? 1 : 0;
            s_bufSeeded[di] = true;      s_boxLo[di] = 99;      s_boxHi[di] = -1;
            s_bufSeeded[di ^ 1] = false; s_boxLo[di ^ 1] = 99;  s_boxHi[di ^ 1] = -1;
            s_boxColHi[di] = -1;         s_boxColHi[di ^ 1] = -1;   // no prior figure to erase
        }
        s_flightRescuePause = true;   // tell flightKickBackClear to stop wiping the off-screen buffer
        if (s_cleanValid) {
            Bitmap* const back = (flightDisplayed == terrainBitmapBack) ? terrainBitmap : terrainBitmapBack;
            const int bi = (back == terrainBitmapBack) ? 1 : 0;
            // A clear may still be pending on this buffer from the frame we entered the pause on.
            if (flightClearPending == back) { BW_AT(g_bwPendClear, AmigaHardware::blitterWait()); flightClearPending = nullptr; }
            if (!s_bufSeeded[bi]) {
                // First use of this buffer in the pause — it was cleared blank; seed clean terrain.
                back->copy(*s_cleanBmp, 0, 0, 0, 0, kW, 47);
                s_bufSeeded[bi] = true; s_boxLo[bi] = 99; s_boxHi[bi] = -1; s_boxColHi[bi] = -1;
                // The seed brought in the SNAPSHOT's band plane3 (rows 43-46), which came from the
                // other buffer and so may be the other field half's — while this buffer's
                // s_bandP3Seen record still describes what it held before.  Drop the record.
                s_bandP3SeenHalf[bi] = -1;
            }
#if defined(ROF_FLIGHT_PROBE)
            // Alien-colour diagnosis: count live composites of a non-empty overlay during the
            // knock ($0632).  >0 confirms the creature IS composited+flipped live (so invisibility
            // is a palette issue, not a render/flip one).
            { extern volatile unsigned long g_alComp; if (mem[0x0632] && g_figRowHi >= g_figRowLo) g_alComp++; }
#endif
            // Composite = erase-old + cookie-cut-draw in ONE blitter pass (Bitmap::combineWithMask,
            // 4-channel A=mask B=figure C=clean D=dest).  Writes dest = (clean & ~mask) | (figure &
            // mask): clean where the mask is 0 (erasing the previous figure + filling the gaps), the
            // figure where the mask is 1.  ROF_CLEAR_FIG keeps the mask nonzero EXACTLY on the current
            // figure, so compositing any rect covering (this buffer's previous figure box ∪ the current
            // figure) restores clean over the old figure and paints the new one — with no ghosting
            // outside the current figure.  We composite that union as a NARROW word-aligned sub-rect
            // (rows AND columns) instead of full 320px, cutting the blitter work to the figure's actual
            // footprint.  Only planes 1+2 are touched (s_figBmp is 2-plane); plane3 (windscreen frame)
            // stays the clean seed.  Runs on the blitter, parallel to the CPU + the 50Hz ISR.
#ifdef ROF_FLIGHT_PROBE
            unsigned long _tc0 = rof_subclock();
#endif
            int lo = g_figRowLo, hi = g_figRowHi;                 // current figure row extent
            int wlo = (g_figColHi >= g_figColLo) ? (g_figColLo >> 1) : 20;  // -> word columns (0..19)
            int whi = (g_figColHi >= g_figColLo) ? (g_figColHi >> 1) : -1;
            if (s_boxHi[bi] >= s_boxLo[bi]) {           // union in the previous figure box (to erase it)
                if (s_boxLo[bi] < lo) lo = s_boxLo[bi];
                if (s_boxHi[bi] > hi) hi = s_boxHi[bi];
            }
            if (s_boxColHi[bi] >= 0) {                   // union in the previous word-column box
                if (s_boxColLo[bi] < wlo) wlo = s_boxColLo[bi];
                if (s_boxColHi[bi] > whi) whi = s_boxColHi[bi];
            }
            if (hi >= lo && whi >= wlo) {
                const uint16_t y = (uint16_t)lo, h = (uint16_t)(hi - lo + 1);
                const uint16_t x = (uint16_t)(wlo << 4), w = (uint16_t)((whi - wlo + 1) << 4);  // word-aligned px
                back->combineWithMask(*s_cleanBmp, *s_figBmp, *s_figMaskBmp,
                                      x, y,   // dest x,y
                                      x, y,   // background (clean) x,y
                                      x, y,   // source (figure) x,y
                                      x, y,   // mask x,y
                                      w, h);
            }
            if (g_figRowHi >= g_figRowLo) {
                s_boxLo[bi] = g_figRowLo; s_boxHi[bi] = g_figRowHi;
                s_boxColLo[bi] = g_figColLo >> 1; s_boxColHi[bi] = g_figColHi >> 1;
            } else {
                s_boxLo[bi] = 99; s_boxHi[bi] = -1; s_boxColHi[bi] = -1;
            }
            // FULLY drain the queue (not just blitterWait): combineWithMask enqueues one blit
            // per plane (+ a possible seed copy), and blitterWait() returns after only the FIRST
            // completes — flipping then would show a half-composited buffer (missing plane/rows).
            BW_AT(g_bwFlip, AmigaHardware::blitterDrain());  // composite (+ any seed copy) fully done before the flip
#ifdef ROF_FLIGHT_PROBE
            if (mem[0x0632]) g_alTComp += rof_subclock() - _tc0;   // composite+drain only (excl. flip wait)
#endif
            // Flip via the VBI (same torn-pointer-safe protocol as the normal render path).
            flightPendingFlip = back;
            flightSwapPending = true;
#if defined(ROF_FLIGHT_PROBE)
            { extern volatile unsigned long g_alTFlipWait;
              unsigned long _fw = rof_subclock();
              while (flightSwapPending) { }
              if (mem[0x0632]) g_alTFlipWait += rof_subclock() - _fw; }
#else
            while (flightSwapPending) { }
#endif
        }
        return;
    }
    s_flightRescuePause = false;   // not in the rescue pause — resume normal clears
    // Rescue pause just ended (systems back on): wipe every trace of the pause's figure so the NEXT
    // rescue can't flash a stale one for a frame.  Two parts, both needed:
    //  1. Restore clean frozen terrain into BOTH terrain buffers (s_cleanBmp still valid here, reset
    //     just below) — they hold the figure composited during the pause.  On the blitter; the
    //     displayed buffer is restored to the terrain it should show on resume (harmless seam).
    //  2. Clear the figure OVERLAY (s_figBmp/mask) + reset the extents and per-buffer erase boxes.
    //     After a knock the overlay still holds the alien silhouette (ROF_CLEAR_FIG only clears at the
    //     START of the next draw), and the next rescue's first composite can run BEFORE that draw
    //     populates it — compositing the stale alien for one frame.  This was the actual flash (part 1
    //     alone did NOT fix it — measured).  Keyed on the true systems-back-on edge (s_resumeClearPend)
    //     so the pilot approach's mid-zoom $3D dips never trigger it.
    if (s_resumeClearPend) {
        s_resumeClearPend = false;
        if (s_cleanValid) {
            terrainBitmap->copy(*s_cleanBmp, 0, 0, 0, 0, kW, 47);
            terrainBitmapBack->copy(*s_cleanBmp, 0, 0, 0, 0, kW, 47);
            // Both buffers' band plane3 (rows 43-46) now hold the snapshot's, not what their
            // s_bandP3Seen records claim — drop both records so the next paint re-copies.
            s_bandP3SeenHalf[0] = -1; s_bandP3SeenHalf[1] = -1;
        }
        if (s_figBmp) { s_figBmp->clear(); s_figMaskBmp->clear(); }
        BW_AT(g_bwFlip, AmigaHardware::blitterDrain());     // terrain restore + overlay clear both done before continuing
        g_figRowLo = 99; g_figRowHi = -1; g_figColLo = 40; g_figColHi = -1;
        s_boxLo[0] = 99; s_boxHi[0] = -1; s_boxLo[1] = 99; s_boxHi[1] = -1;
        s_boxColHi[0] = -1; s_boxColHi[1] = -1;
    }
    s_cleanValid = false;          // re-snapshot on the next rescue

    // Preserve the last terrain frame across rescue PAUSES (e.g. the knock phase).  When the main
    // loop is parked in pilot_render's hold loop it drives frames via the SPINWAIT yield but runs NO
    // terrain_draw, so g_flightTerrainFresh stays clear.  Repainting from an empty dot plane would
    // drop the plane2 dots, so we skip the whole clear/edge/fill/flip and leave the last good
    // terrain on screen.  Cleared here so the next real draw repaints exactly once.
    if (!g_flightTerrainFresh) return;
    g_flightTerrainFresh = 0;

    // Double-buffer: paint the OFF-screen buffer (the one the copper is NOT currently showing),
    // then re-point the copper to it.  The flip latches at the next vblank, so the live buffer
    // is never cleared/refilled mid-frame (that was the plane1 flicker).  First frame
    // (flightDisplayed==null) draws into terrainBitmapBack.
    Bitmap* const back = (flightDisplayed == terrainBitmapBack) ? terrainBitmap : terrainBitmapBack;
    uint8_t* const bp = (uint8_t*)back->data;

#ifdef ROF_FLIGHT_PROBE
    extern volatile unsigned long g_fdClear, g_fdEdge, g_fdFill, g_fdScan,
                                  g_fdBand, g_fdCalls, g_fdScanRows;
    unsigned long _fi = g_isrBeamLines, _ft = rof_subclock();
  #define FD_LAP(acc) do { unsigned long _n=rof_subclock(), _ni=g_isrBeamLines; \
        (acc) += (_n - _ft) - (_ni - _fi); _ft=_n; _fi=_ni; } while(0)
#else
  #define FD_LAP(acc) do {} while(0)
#endif

    // The blits below run on the Amiga blitter while the 68000 keeps working; we only
    // blitterWait() at the point where the CPU actually needs the blit's result.  The sky fill
    // (plane1 rows 0-41) is kicked and only waited on just before the flip.  The edge plot DOES
    // depend on the clear (it ORs into freshly-zeroed plane1), so it sits behind a blitterWait.

    // Dot side-buffer model: `back` is the freshly-freed off-screen buffer (the flip that freed it was
    // drained at the top of this function).  It needs 47 rows (0-46) — terrain viewport (0-42) +
    // windscreen band (43-46) — of fresh content, and the terrain dots (plane2) COPIED in from the dot
    // side-buffer (the rasterizer ORed this frame's dots into terrainDotBuffer's plane2, NOT a display
    // buffer, during the upstream compute, so they survived the flip).
    //
    // That used to be one whole-buffer 3-plane clear + the copy, both drained before continuing — 43
    // beam-ticks of pure CPU stall per painted frame (2.7% of flight, BLIT_SHAPE probe 835c942).  But
    // the only thing the CPU needs next is the edge plot, and that touches plane1 ALONE.  So the work
    // is split by plane and only plane1 is awaited:
    //   plane1 (+0)  clear — the edge plot ORs the skyline into it, so it must be zero first: awaited.
    //   plane2 (+40) copy  — a straight A->D copy covering all 20 words x 47 rows, so it needs no
    //                        clear at all (the old code cleared those words, then overwrote them).
    //                        Kicked here; runs UNDER the edge plot.
    //   plane3 (+80) clear — first touched by the CPU far below (crosshair/band), and needed at all
    //                        only on a buffer's first flight frame: moved past the edge plot and
    //                        made one-shot (s_p3Clean).
    // Dropping the redundant plane2 clear also cuts total blitter work here by ~20%.
    AmigaHardware::blitterClear((uint16_t*)bp, 20, 47, 80);   // plane1 only (mod = 120 stride - 40 row bytes)
    BW_AT(g_bwClearCopy, AmigaHardware::blitterDrain());      // the one blocking wait: plane1 must be clean for the edge plot
    // Blitter idle here, so blitterCopy pokes the registers directly and starts NOW (a queued blit
    // would not: nothing drains the queue asynchronously — INTF_BLIT is masked and
    // processBlitterQueue() only runs from a wait).  It overlaps the edge plot below.
    AmigaHardware::blitterCopy((uint16_t*)((uint8_t*)terrainDotBuffer->data + 40),  // src plane2
                               (uint16_t*)(bp + 40),                                // dst plane2
                               20 /*words*/, 47 /*rows*/,
                               80 /*srcMod bytes = 120-40*/, 80 /*dstMod bytes*/,
                               0 /*shift*/, 0xFFFF /*fwm*/, 0xFFFF /*lwm*/, 0xFFFF /*unused (minterm=A)*/);
    flightClearPending = nullptr;
    FD_LAP(g_fdClear);

    // Resume-frame terrain-dot recovery.  On the single normal frame that ends a rescue-figure
    // pause the off-screen buffer was NEVER pre-cleared (flightKickBackClear suppressed the clear
    // for the whole pause), so the terrain rasterizer ORed this frame's fresh dots into the
    // un-cleared buffer and the safety clear/copy just above replaced BOTH planes — measured: the painted
    // buffer's plane2 byte-sum collapses ~10209->1290 for exactly one displayed frame, then
    // self-corrects once flightKickBackClear re-arms the pre-clear.  The fresh dots are
    // unrecoverable (ORed on top of stale content, inseparable), but the ship is stationary across
    // a rescue, so the FROZEN dots captured in s_clean at pause entry are a byte-identical stand-in
    // for this one frame.  Restore plane2 (offset +40) for terrain body rows 0-42 only; the
    // windscreen band (43-46) is repainted from mem[$2098] by the band overlay below.  s_clean is
    // the pre-figure snapshot (no figure pixels) so this leaves no ghost.  Touches ONLY the normal
    // path — the dirty-rect clear/flip/seed state machine is untouched, and there is no flip-skip
    // (attempt #1's shear) nor pause-flow change (attempt #2's shear).  Keyed on the $3E-end latch
    // (s_resumeRestorePend), so it fires exactly once at the true resume and never during the zoom.
    if (s_resumeRestorePend) {
        s_resumeRestorePend = false;
        // The plane2 dot copy kicked above is still in flight and targets exactly these bytes, so
        // settle it before overwriting them by CPU (the copy must land FIRST, then be replaced — the
        // old whole-buffer drain gave that ordering for free).  One-shot per rescue; cost irrelevant.
        AmigaHardware::blitterDrain();
        const uint8_t* s2 = (const uint8_t*)s_cleanBmp->data + 40;   // plane2 base, walked +120/row
        uint8_t* d2 = bp + 40;                                       // plane2 in the back buffer
        for (int r = 0; r <= 42; r++, s2 += 120, d2 += 120) {
            for (int b = 0; b < 40; b++) d2[b] = s2[b];
        }
    }

    // Edge plot: ONE plane1 bit per column at its skyline scanline (160 byte-ORs).  Hand-asm twin
    // (flight_edge_plot_asm, TerrainRasterizeAssembler.s) — 4 columns unrolled with immediate masks,
    // the plane1 byte pointer walked +1 per 4 cols, the 150-h/clamp folded into kHeightRowOff[].
    // The crest row IS the silhouette top; the rasterizer lags its plane2 dots by one so it never
    // plots at COL_MAX, so plane1 sky safely covers down to and INCLUDING the crest with no overlap.
    if (!kHeightRowOffBuilt) buildHeightRowOff();
#ifdef ROF_EDGE_SHAPE
    edgeShapeProbe();          // structural shape of the skyline this loop is fed (off by default)
#endif
#if defined(ROF_RASTERIZE_ASM) && defined(ROF_RASTERIZE_VERIFY)
    // Differential verify (same run, deterministic): C reference and asm into fresh scratch planes
    // from the same $260E, byte-compare; perf timed back-to-back.  Live plane uses the proven C.
    edgePlotCore(bp);
    { static uint8_t eScrC[47*120], eScrA[47*120];
      for (int i = 0; i < 47*120; i++) { eScrC[i] = 0; eScrA[i] = 0; }
      unsigned long p, ib;
      p = rof_subclock(); ib = g_isrBeamLines; edgePlotCore(eScrC);        g_edgeCTicks   += (rof_subclock()-p) - (g_isrBeamLines-ib);
      p = rof_subclock(); ib = g_isrBeamLines; flight_edge_plot_asm(eScrA); g_edgeAsmTicks += (rof_subclock()-p) - (g_isrBeamLines-ib);
      g_edgeCalls++;
      for (int i = 0; i < 47*120; i++) if (eScrC[i] != eScrA[i]) { g_edgeMismatch++; break; }
    }
#elif defined(ROF_RASTERIZE_ASM)
    flight_edge_plot_asm(bp);
#else
    edgePlotCore(bp);
#endif
    // plane3: ONE-SHOT clear per buffer, not per frame (see s_p3Clean).  Both of plane3's writers
    // are self-sufficient — the crosshair below ORs the same fixed bytes every frame and the band
    // composite long-COPIES rows 43-46 — so once a buffer's plane3 is right it stays right, and the
    // clear is only needed where a foreign scene could have left bits in it (flight entry, armed in
    // deriveRenderSignals).  Steady state: no clear at all.
    const int p3i = (back == terrainBitmapBack) ? 1 : 0;
    const bool p3Fresh = !s_p3Clean[p3i];    // this buffer's plane3 is being rebuilt this frame
    if (p3Fresh) {
        AmigaHardware::blitterClear((uint16_t*)(bp + 80), 20, 47, 80);
        s_p3Clean[p3i] = true;
        s_bandP3SeenHalf[p3i] = -1;   // that clear wiped rows 43-46 too: force the band's plane3 copy
    }
    // Settle whatever is still in flight (the dot copy, and the clear just above on an entry frame)
    // before the sky fill.  blitterFillUp would drain in its own prologue anyway; spelling it out
    // lets the BLIT_SHAPE probe attribute the wait here instead of hiding it inside the fill.
    BW_AT(g_bwP3Clear, AmigaHardware::blitterDrain());

    // Sky fill: propagate each edge bit UP in ONE descending blit (writes rows 0-45, seed 46).
    // Full-height (47 rows) so the terrain silhouette continues into the windscreen band — the
    // band's L/R edges then show real terrain.  (Was 43 rows / seed 42; buildHeightRowOff clamps
    // the skyline to row 46 to match.)
    AmigaHardware::blitterFillUp((uint16_t*)bp, 20, 46, 80);
    FD_LAP(g_fdEdge);

    // plane2 = terrain dots/detail (mode-D value-2/3).  The rasterizer ORed them into the dot
    // side-buffer (g_flightDotPlane = terrainDotBuffer plane2) during the upstream compute; they were
    // copied into `back`'s plane2 by the blitterCopy above, so they are already here.
    FD_LAP(g_fdScan);                                        // (now ~0)
    BW_AT(g_bwSkyFill, AmigaHardware::blitterWait());                            // sky fill must finish before the band overlay + flip
    FD_LAP(g_fdFill);

    // Object plane1 overlay: OR the value-3 ground-object low bits (recorded by terrain_plot_pixel
    // during the draw) into plane1 NOW — AFTER the sky fill — so those objects show value-3 (COLPF2,
    // the distinct object colour) instead of value-2 (COLPF1, the terrain-dot colour).  Deferred to
    // here because a plane1 bit present during blitterFillUp would seed a spurious sky-coloured
    // vertical streak.  Walk only the dirty scanline range; clear each byte as it is applied so the
    // scratch is ready for the next frame.  (Objects are sparse, so this is a few rows x 40 bytes.)
#ifdef ROF_BAND_SHAPE
    BS_RESET();            // start of the g_fdBand window (object overlay + crosshair + band)
    if (g_objRowHi >= g_objRowLo) {
        g_bsObjFrames++;
        g_bsObjRows += (unsigned long)(g_objRowHi - g_objRowLo + 1);
        g_bsObjBox  += (unsigned long)(g_objRowHi - g_objRowLo + 1)
                     * (unsigned long)(g_objColHi - g_objColLo + 1);   // bytes the box walk visits
        const uint8_t* sp = s_flightObjP1 + kRow120[g_objRowLo];
        for (int sc = g_objRowLo; sc <= g_objRowHi; sc++, sp += 120)
            for (int b = 0; b < 40; b++) if (sp[b]) g_bsObjBytes++;   // nonzero = real work
    }
#endif
    // Walk only the dirty bounding BOX (rows AND byte-columns).  The row range alone left this
    // scanning all 40 bytes of each dirty row for the handful of object bytes actually in it —
    // 1.8% of bytes scanned were nonzero (BAND_SHAPE probe).  Every nonzero byte is inside the box
    // (the three writers maintain both ranges together), so the narrowed walk still clears them all.
    if (g_objRowHi >= g_objRowLo) {
        const int cl = g_objColLo, n = g_objColHi - cl + 1;
        uint8_t* d = bp            + kRow120[g_objRowLo] + cl;  // plane1, walked +120/scanline
        uint8_t* s = s_flightObjP1 + kRow120[g_objRowLo] + cl;  // scratch (same base offset)
        for (int sc = g_objRowLo; sc <= g_objRowHi; sc++, d += 120, s += 120) {
            for (int b = 0; b < n; b++) { if (s[b]) { d[b] |= s[b]; s[b] = 0; } }
        }
        g_objRowLo = 47; g_objRowHi = -1;                       // range consumed
        g_objColLo = 40; g_objColHi = -1;
    }
#ifdef ROF_BAND_SHAPE
    BS_LAP(g_bsObj);
#endif

    // Targeting crosshair (#10): the Atari's "+" reticle rendered into the otherwise-empty plane3
    // of the terrain body (plane3 is 0 across rows 0-42; only the band below uses it).  Drawn ONCE
    // per buffer, together with that buffer's one-shot plane3 clear (p3Fresh) — this is the SOLE
    // writer of plane3 in the terrain body, the geometry is a compile-time constant, and nothing
    // clears plane3 between frames any more, so the reticle simply stays where it was put.  Its
    // visibility is a copper palette swap (color04-07), never a redraw, so there is nothing
    // per-frame to do here at all.  ⚠ Any position- or state-dependent plane3 pixel added to the
    // terrain body would break BOTH halves of that: it would need its own erase, and this block
    // would have to go back to running every frame (with the per-frame clear restored).  The copper
    // sets color04-07 in the viewport = the reticle salmon ($26, #833c2d) so a plane3 pixel reads
    // that colour over any terrain in planes 1&2.  The "+" is missiles M2/M1/M3 (flight VBI
    // $505F-$5071: HPOSM3=$74, HPOSM2=$80, HPOSM1=$85, SIZEM=$CC → M1/M3 quad-width); measured
    // byte-identical across captures = a static frame element.  Geometry (column = HPOS-$30, one
    // colour clock = one terrain column = 2 Amiga px = kColMask4[col&3]; row = (missile offset-$32)/2
    // under the ×2 line-doubling):
    //   • VERTICAL (M2 @ $80 = column 80): buffer $0B4D-$5A / $0B64-$71 → rows 13-20 / 25-31, gap at
    //     the horizon (rows 21-24).
    //   • HORIZONTAL arms at the gap-centre line (buffer $0B5F → row 22): M3 @ $74 quad = columns
    //     68-75 (left), M1 @ $85 quad = columns 85-92 (right), leaving the centre gap around col 80.
    if (p3Fresh) {
        uint8_t* const p3 = bp + 80;                            // plane3 base (offset 80 per 120B scanline)
        uint8_t* vu = p3 + kRow120[13] + 20;                    // vertical stem, walked +120/row
        for (int r = 13; r <= 20; r++, vu += 120) *vu |= 0xC0u;        // upper (col 80)
        uint8_t* vl = p3 + kRow120[25] + 20;
        for (int r = 25; r <= 31; r++, vl += 120) *vl |= 0xC0u;        // lower
        uint8_t* const h = p3 + kRow120[22];                    // horizontal arms, row 22
        for (int c = 68; c <= 75; c++) h[c >> 2] |= kColMask4[c & 3];   // left arm (M3)
        for (int c = 85; c <= 92; c++) h[c >> 2] |= kColMask4[c & 3];   // right arm (M1)
    }
#ifdef ROF_BAND_SHAPE
    BS_LAP(g_bsCross);
#endif

    // Windscreen-bottom band overlay (rows 43-46 = scanlines 172-179): the cockpit frame + the
    // wing-clearance bars, punched OVER the now-rendered terrain.  Source = the mode-D band field
    // mem[$1074+43*96] (double-buffer half via g_flightRenderHalf), written per frame by
    // game_sub_451d.  Per pixel: the grey frame (value 3) sets plane3 -> color04-07 (all grey), so
    // it covers the terrain in planes 1&2 with a solid frame; the salmon bars (value 1) + centre
    // marker (value 2) OVERWRITE planes 1&2 (bar -> color01 salmon, marker -> color02) and clear
    // plane3, so they punch through the frame; value 0 (the L/R edge regions) touches nothing, so
    // the rendered terrain shows there.  (The bars/marker overwrite must clear the terrain bits
    // under them, hence the read-modify-write with the `ow` mask.)
    {
        const unsigned fieldHalf = g_flightRenderHalf ? 0x30u : 0x00u;
        const uint8_t* srow = (const uint8_t*)mem + 0x1074 + fieldHalf + 43 * 96;
        uint8_t* vrow = bp + 43 * 120;
#ifdef ROF_BAND_VERIFY
        // In-process differential for the cached band composite (make BAND_VERIFY=1 +
        // amiga/band_verify.gdb).  This is a RENDERING change, and rendering cannot be judged from
        // a headless run (the remote debugger greys the display), so prove byte-identity instead:
        // snapshot the band rows, run the cache path, stash its output, restore, run the ORIGINAL
        // per-byte composite (which stays LIVE, as the edge-plot verify keeps its C reference live),
        // then compare.  g_bandMismatch must be 0.
        static uint8_t bvSnap[4 * 120], bvNew[4 * 120];
        for (int i = 0; i < 4 * 120; i++) bvSnap[i] = vrow[i];
        // The field is written by game_sub_451d from the flight VBI ISR, so the two passes below
        // would otherwise read DIFFERENT source bytes whenever the ISR fires between them — a
        // harness artifact, not a logic difference (the same effect the tfsetup differential
        // documents for $2270-$2274).  Freeze one copy and point BOTH passes at it, so any
        // surviving mismatch is genuinely the cache's fault.
        static uint8_t bvField[4 * 96] __attribute__((aligned(4)));
        for (int r = 0; r < 4; r++)
            for (int b = 0; b < 40; b++) bvField[r * 96 + b] = srow[r * 96 + b];
        srow = bvField;
#endif
#ifdef ROF_BAND_SHAPE
        // Is the band field worth re-compositing every frame?  Compare it against a per-HALF
        // shadow (the two double-buffer halves alternate, so one shadow would read as "all
        // changed" every frame) and tally changed bytes + how many bytes take the rare
        // overwrite (ow != 0) path vs the plane3-only path.
        {
            const unsigned hi = g_flightRenderHalf ? 1u : 0u;
            unsigned long changed = 0, ow_nz = 0;
            for (int row = 0; row < 4; row++)
                for (int b = 0; b < 40; b++) {
                    const uint8_t v = srow[row * 96 + b];
                    const int p = row * 40 + b;
                    if (kBandOW[v]) { ow_nz++; g_bsOwPos[p]++; }   // where the bars/marker live
                    if (s_bsShadow[hi][p] != v) {                  // WHICH positions are dynamic
                        changed++; g_bsChgPos[p]++; s_bsShadow[hi][p] = v;
                        if (g_bsBandFrames > 8) g_bsChgLate++;     // ...after the entry transient
                    }
                }
            g_bsBandFrames++;
            g_bsBandChanged += changed;
            g_bsBandOwNz    += ow_nz;
            if (!changed) g_bsBandClean++;
            if (changed > g_bsBandMaxChg) g_bsBandMaxChg = changed;
        }
        BS_RESET();     // exclude the shadow-compare above from the band composite's own lap
#endif
        // Every per-half base is hoisted to a running pointer and every walk is an autoincrement:
        // the first cut of this indexed the caches as s_bandXc[hf][row*40+b], which put a 2D
        // address computation (and a row*40) inside a 40-iteration loop and measured almost no
        // better than the per-byte decode it replaced (39 -> 36 ticks).  See the "pointer-walk with
        // autoincrement, never multiply+index in a loop" rule in CLAUDE.md.
        const unsigned hf = g_flightRenderHalf ? 1u : 0u;
        uint32_t* shad = s_bandShadow[hf];                    // 10 longs/row
        uint32_t* p3c  = s_bandP3c[hf];                       // 10 longs/row
        uint8_t*  p1c  = s_bandP1c[hf];                       // 40 bytes/row
        uint8_t*  p2c  = s_bandP2c[hf];
        uint8_t*  owc  = s_bandOWc[hf];
        signed char* owLo = s_bandOwLo[hf];
        signed char* owHi = s_bandOwHi[hf];
        // 1. Refresh the decode cache for whatever the ISR changed since this half's last frame
        //    (typically nothing, or one long in row 45).  Compared as longs; decoded byte-wise.
        {
            const uint8_t* fs = srow;
            uint32_t* sh = shad; uint32_t* p3w = p3c;
            uint8_t* p1w = p1c; uint8_t* p2w = p2c; uint8_t* oww = owc;
            for (int row = 0; row < 4; row++, fs += 96, sh += 10, p3w += 10,
                                             p1w += 40, p2w += 40, oww += 40) {
                const volatile uint32_t* f4 = (const volatile uint32_t*)fs;
                bool rowChanged = false;
                for (int g = 0; g < 10; g++) {
                    const uint32_t fv = f4[g];
                    if (fv == sh[g]) continue;
                    sh[g] = fv;
                    rowChanged = true;
                    uint8_t* const p3b = (uint8_t*)p3w;
                    const int k0 = g * 4;
                    for (int k = k0; k < k0 + 4; k++) {       // re-decode just this long's 4 bytes
                        const uint8_t v = fs[k];
                        p3b[k] = kBandP3[v]; p1w[k] = kBandP1[v];
                        p2w[k] = kBandP2[v]; oww[k] = kBandOW[v];
                    }
                }
                if (rowChanged) {                             // re-derive this row's ow!=0 range
                    int lo = 40, hi = -1;
                    for (int b = 0; b < 40; b++) if (oww[b]) { if (b < lo) lo = b; hi = b; }
                    owLo[row] = (signed char)lo; owHi[row] = (signed char)hi;
                    s_bandP3Ver[hf][row]++;                   // this row's decoded plane3 moved on
                }
            }
        }
        // 2. Paint: plane3 = a straight long copy of the cached grey frame, but ONLY into a buffer
        //    that isn't already showing this half's current version of that row (see s_bandP3Ver —
        //    normally just row 45, the wing-clearance bar); planes 1&2 RMW every frame over each
        //    row's ow!=0 range (the bar / centre marker punching through the live terrain).
        const bool p3HalfChanged = (s_bandP3SeenHalf[p3i] != (signed char)hf);
        for (int row = 0; row < 4; row++, vrow += 120, p3c += 10, p1c += 40, p2c += 40, owc += 40) {
            if (p3HalfChanged || s_bandP3Seen[p3i][row] != s_bandP3Ver[hf][row]) {
                uint32_t* p3d = (uint32_t*)(vrow + 80);
                const uint32_t* p3s = p3c;
                p3d[0] = p3s[0]; p3d[1] = p3s[1]; p3d[2] = p3s[2]; p3d[3] = p3s[3]; p3d[4] = p3s[4];
                p3d[5] = p3s[5]; p3d[6] = p3s[6]; p3d[7] = p3s[7]; p3d[8] = p3s[8]; p3d[9] = p3s[9];
                s_bandP3Seen[p3i][row] = s_bandP3Ver[hf][row];
            }
            const int lo = owLo[row], hi = owHi[row];
            if (hi < lo) continue;                            // no bar bytes in this row (43)
            const uint8_t* ow = owc + lo;
            const uint8_t* p1s = p1c + lo;
            const uint8_t* p2s = p2c + lo;
            uint8_t* d1 = vrow + lo; uint8_t* d2 = vrow + 40 + lo;
            for (int n = hi - lo + 1; n--; d1++, d2++, ow++, p1s++, p2s++) {
                const uint8_t m = *ow;
                if (!m) continue;
                *d1 = (uint8_t)((*d1 & ~m) | *p1s);           // salmon bar; terrain kept elsewhere
                *d2 = (uint8_t)((*d2 & ~m) | *p2s);           // centre marker; terrain kept elsewhere
            }
        }
        s_bandP3SeenHalf[p3i] = (signed char)hf;   // this buffer's band plane3 now holds half `hf`
#ifdef ROF_BAND_VERIFY
        {   // stash the cache path's output, restore the pre-composite state, run the ORIGINAL
            // per-byte composite live, and compare (see the snapshot above).
            uint8_t* const v0 = bp + 43 * 120;
            for (int i = 0; i < 4 * 120; i++) { bvNew[i] = v0[i]; v0[i] = bvSnap[i]; }
            const uint8_t* s_ = srow;
            uint8_t* v_ = v0;
            for (int row = 0; row < 4; row++, s_ += 96, v_ += 120) {
                const uint8_t* s = s_;
                uint8_t* d1 = v_; uint8_t* d2 = v_ + 40; uint8_t* d3 = v_ + 80;
                for (int b = 0; b < 40; b++, s++, d1++, d2++, d3++) {
                    uint8_t v = *s;
                    uint8_t ow = kBandOW[v];
                    if (ow) {
                        *d1 = (uint8_t)((*d1 & ~ow) | kBandP1[v]);
                        *d2 = (uint8_t)((*d2 & ~ow) | kBandP2[v]);
                    }
                    *d3 = kBandP3[v];
                }
            }
            g_bandCalls++;
            for (int i = 0; i < 4 * 120; i++)
                if (bvNew[i] != v0[i]) { g_bandMismatch++; if (!g_bandFirstBad) g_bandFirstBad = (unsigned long)i + 1; break; }
        }
        // Object-overlay invariant: after the box-narrowed apply, NO nonzero byte may remain
        // anywhere in the scratch — that is exactly the claim that every nonzero byte was inside
        // the tracked bounding box.  A leak here would show as a stale object pixel next frame.
        for (int i = 0; i < 47 * 120; i++) if (s_flightObjP1[i]) { g_objLeak++; break; }
#endif
#ifdef ROF_BAND_SHAPE
        BS_LAP(g_bsBand);
#endif
    }
    FD_LAP(g_fdBand);
#ifdef ROF_FLIGHT_PROBE
    g_fdCalls++;
#endif
#if defined(ROF_COMBAT_LOAD) && defined(ROF_FLIGHT_PROBE)
    // Combat-load split (see PlatformAmiga.cpp): one increment per painted terrain frame, in
    // every combat PROBE build, so the VBI can attribute painted frames to the combat state they
    // were painted in.  That split is the only cross-state comparison this harness can make
    // honestly — one binary, one run, one trajectory, so it is immune to the cross-build trap.
    // ⚠ Needs the ROF_FLIGHT_PROBE guard as well: g_clFrames is only DEFINED in a probe build, so
    // under a bare `#ifdef ROF_COMBAT_LOAD` a `COMBAT=1 FPSCOUNT=1` build failed to link.
    { extern volatile unsigned long g_clFrames; g_clFrames++; }
#ifdef ROF_FLIGHT_PROBE
    // Bucket this frame by how many ground objects it rastered, so the VBI can price the
    // PERSISTENT object load (not just the explosion animation).  0 / 1-2 / 3-5 / 6+.
    {
        extern volatile unsigned short g_clObjFrame;
        extern volatile unsigned char  g_clObjBucket;
        const unsigned n = g_clObjFrame;
        g_clObjBucket = (unsigned char)(n == 0 ? 0 : n <= 2 ? 1 : n <= 5 ? 2 : 3);
        g_clObjFrame = 0;
    }
#endif
#endif
#ifdef ROF_FPSCOUNT
    // Near-clean framerate counter (`make FPSCOUNT=1`): ONE increment per painted terrain
    // frame.  Paired with g_vbiCount (bumped unconditionally by the real VERTB handler), so
    // FPS = 50 * g_fpsFrames / g_vbiCount.  Deliberately independent of ROF_FLIGHT_PROBE:
    // that flag's FP_TIME brackets read two CHIP custom registers and do a 16x16 multiply
    // several times per iteration, which is exactly the instrumentation cost this build
    // exists to measure.  See amiga/fps_seg.gdb.
    { extern volatile unsigned long g_fpsFrames; g_fpsFrames++; }
#endif
#undef FD_LAP

    // Flip — handed to the VBI so the copper's viewport bitplane-pointer words are rewritten at
    // the VERY START of the next vertical blank, i.e. BEFORE the beam reaches the WAIT(scanline
    // 85)/BPLxPT MOVEs the copper executes for the viewport.  Poking those 6 pointer words from
    // here (mid-frame, arbitrary beam position) could tear a pointer as the copper fetched it →
    // a brown/garbage viewport for one frame.  Protocol: publish the target buffer + raise the swap
    // flag.  DEFERRED (dot side-buffer): we do NOT busy-wait for the swap here — instead we return so
    // the next terrain compute (which writes only the dot side-buffer, never a display buffer) overlaps
    // this flip's vblank.  The wait moves to the DRAIN at the top of the next entry, by which point the
    // compute has run and the swap has long since latched (~0 wait).  flightFlipDeferred tells
    // PlatformAmiga::renderFrame to SKIP its own vblank wait too, so that overlap actually happens.
    flightPendingFlip = back;
    flightSwapPending = true;
    flightFlipDeferred = true;

    // Kick the dot side-buffer plane2 clear for the NEXT frame's rasterize (it ORs into it, so it must
    // start clean).  Kicked here and awaited later by the rasterizer's rof_flight_wait_dotclear —
    // exactly the old flightKickBackClear "kick a clear now, wait at the next draw" idiom, but on the
    // off-display scratch instead of a display buffer.  Runs concurrently with the game compute.
    AmigaHardware::blitterClear((uint16_t*)((uint8_t*)terrainDotBuffer->data + 40),
                                20 /*words*/, 47 /*rows*/, 80 /*mod bytes = 120-40*/);
}

// flightVblankSwap: run from the real INTB_VERTB ISR (PlatformAmiga vbiHandler) at the very start
// of vertical blank.  If a flip is pending, rewrite the flight copper's viewport bitplane pointers
// NOW — the beam is above the viewport, so the copper won't fetch them until scanline 85, long
// after this returns → no torn pointer.  Then publish the new displayed buffer and clear the flag
// so the main thread's busy-wait in renderFlightDirect can proceed.  Safe to call every vblank
// (no-op unless a swap is pending); the flag is only ever set during flight.
void RescueOnFractalus::flightVblankSwap()
{
#if defined(ROF_FLIGHT_PROBE)
    { extern volatile unsigned long g_alVSwapRun, g_alVSwapCleared;
      if (mem[0x0632]) { g_alVSwapRun++; if (flightSwapPending) g_alVSwapCleared++; } }
#endif
    if (!flightSwapPending) return;
    if (flightCopper && flightPendingFlip) {
        flightCopper->setTerrainBitplanes(*flightPendingFlip);
        flightDisplayed = flightPendingFlip;
    }
    flightSwapPending = false;
}

// blankForRestart(): called from the VBI ISR (vbiHandler) once a BREAK/Restart is armed but not yet
// taken — VVBLKI = the trampoline's $52B4 — which persists for several vblanks because a single
// flight terrain-compute iteration spans ~4 frames, so the main loop doesn't reach the rof_check_
// restart at the top of renderFlightDirect (→ the longjmp) for that long.  Across those frames the
// OLD FlightCopperList stays live and the flight loop keeps computing + swapping buffers, so the
// viewport shows stale / mid-swap flight (the "brown rectangle" flash).  We are in the VBI ISR here,
// i.e. the beam is parked at the top of the frame, so an immediate COPJMP1 to the black
// EmptyCopperList is safe (its sprite MOVEs at the top of the list execute before the beam reaches
// the sprites — the smear that made a MID-FRAME COPJMP unusable can't happen).  Also cancel any
// pending flight flip so flightVblankSwap doesn't re-point the (now hidden) flight copper.  Idempotent:
// re-jumping to the same list each armed vblank is harmless.
void RescueOnFractalus::blankForRestart()
{
#ifdef ROF_FLIGHT_PROBE
    { extern volatile unsigned char g_blankForRestartCount; g_blankForRestartCount++; }
#endif
    flightSwapPending = false;                       // don't chase a flip into the hidden flight copper
    if (!emptyCopper) return;
    emptyCopper->setColor00(atariToOCS(0));          // pure black
    AmigaHardware::setCopperList(*emptyCopper, true); // immediate COPJMP1 — safe: we're at vblank (beam top)
}

// flightKickBackClear: called by PlatformAmiga::renderFrame after each flight frame.  In the dot
// side-buffer model the terrain rasterizer targets the DEDICATED off-display scratch (terrainDotBuffer,
// constant pointer), never a display buffer — so there is no per-frame display-buffer clear to kick
// here any more.  That clear moved into renderFlightDirect (it clears the freed `back` after draining
// the flip, then clears the scratch's plane2 for the next rasterize).  This now only (re)arms the
// constant scratch pointers the rasterizer ORs into (cheap; safe to repeat every frame).
void RescueOnFractalus::flightKickBackClear()
{
    if (!rsFlight || !terrainDotBuffer) return;
    g_flightDotPlane = (uint8_t*)terrainDotBuffer->data + 40;   // rasterizer plane2 target (constant)
    g_flightObjP1    = s_flightObjP1;                           // object plane1 overlay scratch
}

// run(): the whole game, driven by the genuine transpiled/native boot chain
// (game_entry -> game_main_loop -> boot_standby_launch_driver -> flight).  That chain is
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

    // BREAK/Restart (game_loop_reset): the Atari trampoline ($52BE) does a 6502 RTS stack trick to
    // re-enter game_entry at $3D1F — which SKIPS the $3D0C clear of $0600-$060C, so the high score
    // ($0605-$0608) survives ("score lost, highs kept").  That stack trick can't run in C (and fires
    // from the VBI ISR), so the pump longjmps here when it sees the trampoline's VVBLKI=$52B4.  We
    // replicate the faithful $3D1F->$3D48 init (NOT a full game_entry re-run, which would clear the
    // highs) and fall into game_main_loop.  cockpit_flag/$00E4 keep the =4 the trampoline set.
    if (__builtin_setjmp(g_restartJmp) != 0) {
#ifdef ROF_FLIGHT_PROBE
        extern volatile unsigned char g_restartCount; g_restartCount++;
#endif
        // "Bug 2": keep the screen black across the WHOLE restart so no stale/mid-swap scene flashes.
        // The real flash was the flight case: the trampoline sets VVBLKI=$52B4 from inside the VBI
        // ISR, but the main loop doesn't reach the rof_check_restart at the top of renderFlightDirect
        // (→ this longjmp) for ~4 frames, because a flight terrain-compute iteration spans that long;
        // across those frames the old FlightCopperList stays live and the flight loop keeps computing
        // + swapping buffers, so the viewport shows stale/mid-swap flight (the "brown rectangle").
        // That window is blanked by blankForRestart(), called from the VBI ISR while VVBLKI==$52B4
        // (safe immediate COPJMP there — beam parked at top).  Here we just keep it black:
        //   - g_restartHoldBlack FIRST, so if any renderFrame pump runs before the resting scene is
        //     ready it holds black (never installs a viewport copper from stale rsLaunched flags).
        //   - install the black EmptyCopperList (deferred) as the resting COP1LC.
        //   - waitBeamLine for the latch so the NON-flight paths (whose $52B4 is consumed in the same
        //     rof_check_restart call, so the ISR never blanks them) are black before init mutates buffers.
        g_restartHoldBlack = true;
        g_flightBlank = 0;                                      // never carry a death-blank into the restart
        // Reset the standby door-field latch so the black hold's "standby ready" edge is REAL:
        // boot_standby_launch_driver re-sets it to 1 only AFTER rebuilding the $2000 door field, so
        // this makes the hold release exactly when the fresh standby cockpit is built (not on the
        // previous boot's stale latch).  g_standbyRevealReady is deliberately NOT reset — the card
        // ($53CC) landing path never sets it, and the top-of-renderFrame gate keys off it.
        g_doorFieldReady = 0;
        mem[0x0222] = 0xCC; mem[0x0223] = 0x53;                 // VVBLKI = $53CC (also clears the $52B4 marker)
        if (emptyCopper) {
            emptyCopper->setColor00(atariToOCS(0));             // pure black
            AmigaHardware::setCopperList(*emptyCopper, false);  // COP1LC = empty (latches next vblank)
            emptyCopperInstalled = true;
            standbyCopperInstalled = false; planetCopperInstalled = false;
            flightCopperInstalled = false; tunnelCopperInstalled = false;
            titleScreenCopperInstalled = false;
            AmigaHardware::waitBeamLine(250);                   // wait one full vblank crossing so the
            AmigaHardware::waitBeamLine(20);                    // black list is displaying before init runs
        }
        mem[0x0041] = 3;                                        // $3D21: game_state = 3
        // $3D23-25: if level_or_state ($0004) != 0, loop back through the $3D0C clear.  The trampoline
        // re-enters game_entry at $3D1F, and $3D23 branches on $0004: a NORMAL flight/standby break has
        // $0004==0 so this is skipped and the trampoline's cockpit_flag/game_var_E4 = 4 + the high score
        // survive (→ the results/level-select card).  A DEMO DROID break leaves $0004 != 0, so the real
        // game runs $3D0C — clearing $0600-$060C (incl. cockpit_flag + high score) and $0004/$37F4/$00E4/
        // level_stage — then re-checks with $0004==0 and falls through.  My handler previously omitted
        // this, so the demo state persisted and the demo re-ran; replicate it faithfully.
        if (mem[0x0004] != 0) {                                 // $3D23-25 (taken on a DEMO DROID break)
#ifdef ROF_FLIGHT_PROBE
            { extern volatile unsigned char g_l3d0cFired; g_l3d0cFired++; }
#endif
            for (uint16_t a = 0x0600; a <= 0x060C; a++) mem[a] = 0;  // $3D0C-12: clear $0600-$060C
            mem[0x0004] = 0;                                    // $3D14: level_or_state = 0
            mem[0x37F4] = 0;                                    // $3D16: game_var_37F4 = 0
            mem[0x00E4] = 0;                                    // $3D19: game_var_E4 = 0
            mem[0x006D] = 4;                                    // $3D1D: level_stage = 4
            mem[0x0041] = 3;                                    // $3D21 (loop): game_state = 3
        }
        mem[0x0216] = 0x2A; mem[0x0217] = 0x46;                 // $3D28-2F: IRQ vector $462A (inert on Amiga)
        audio_timer_setup();                                    // $3D32
        sfx_engine_reset();                                     // $3D35
        for (uint16_t a = 0x0626; a <= 0x062B; a++) mem[a] = 0; // $3D38-40: clear $0626-$062B
        mem[0x006C] = 0;                                        // $3D42: sound_active_flag
        mem[0x00E2] = 0x64;                                     // $3D44: attract_timer
        game_main_loop();                                       // $3D48: never returns
    }

    game_entry();     // $3CDE: mega-init -> game_main_loop (Standby -> cinematic -> flight); never returns
}

// renderFrame(): the per-frame repaint body, called from PlatformAmiga::renderFrame()
// at each transpiled frame-wait hook.  Does the non-phase per-frame work, repaints the
// bitmaps, rebuilds the back copper list and flips to it.  The VBI has not yet fired
// when this is entered; rendering happens first, then the caller waits for the VBI.
void RescueOnFractalus::renderFrame()
{
#ifdef ROF_FLIGHT_PROBE
    extern volatile unsigned long g_renderFrameCount; g_renderFrameCount++;
#endif
    // Black-until-ready: while the boot/standby build is still in progress, keep the blank
    // EmptyCopperList on screen and do no rendering — the bitmaps are mid-build and the real
    // lists would show garbage.  When g_standbyRevealReady latches, fall through and the copper
    // path below installs the real (standby / viewport / dynamic) list for this frame.
    //
    // EXCEPT the boost return-to-mother-ship reverse cinematic: it plays (stars, then reverse
    // tunnel rings) WHILE boot_standby_launch_driver is re-entered with g_standbyRevealReady still
    // 0 (it only re-latches at construction-done, rof_native.c $6118).  That cinematic is VISIBLE
    // content, not the piecemeal door BUILD the black hold exists to mask — so black-holding it
    // paints the whole "ship ascends to space → stars → reverse tunnel" sequence solid black.
    // Detect the boost VIEWPORT phase (identical to rsBoostViewport, but computed inline here since
    // deriveRenderSignals hasn't run for this frame yet: VVBLKI=$52D7, mother-ship flag $003A=$FF,
    // reverse ring active $008D!=0 OR pre-ring stars $008E==0) and skip the hold so the
    // rsBoostViewport branch below renders it.  The final next-level door BUILD ($008D==0 &&
    // $008E!=0) is NOT a viewport phase, so it still black-holds until reveal (masking the build).
    const uint16_t vvblkiRF = (uint16_t)(mem[0x0222] | (mem[0x0223] << 8));
    const bool boostViewportCine = (vvblkiRF == 0x52D7u) && (mem[0x003A] == 0xFFu)
                                   && (mem[0x008D] != 0u || mem[0x008E] == 0u);
    if (emptyCopper && !g_standbyRevealReady && !boostViewportCine) {
        // Track the render signals EVERY held frame so the g_doorFieldReady 0->1 edge that fires
        // mid-build (boot_standby_launch_driver clears it at entry, re-sets it at construction-done)
        // is observed HERE and arms the one-time door decode (terrainDirty, line ~3115) + full
        // cockpit repaint (cockpitForceFull rising-edge, line ~3136).  Those flags aren't consumed
        // until render() runs (skipped while held), so they survive to the reveal frame's decode.
        // Without this, a REBUILD (post-crash / post-BREAK / START-from-the-card) leaves the screen
        // black through the build but then reveals a STALE viewportBitmap (doors' top half black/
        // wrong until something else redraws it) — the reason bug 3 survived the earlier fixes.  On
        // first boot terrainDirty starts true so it worked by luck; this makes every build correct.
        deriveRenderSignals();
#ifdef ROF_FLIGHT_PROBE
        { extern volatile unsigned char g_liveCopper; g_liveCopper = 9;   // 9 = black EmptyCopperList
          extern volatile unsigned long g_blackHoldFrames; g_blackHoldFrames++; }
#endif
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

    // BREAK/Restart black hold (bugs 2 & 3): from the instant a restart is taken (g_restartJmp
    // handler in run()) keep the screen black until the fresh resting scene is genuinely rebuilt,
    // so the STALE flight/launch copper + bitplanes (garbage / black+brown) never flash, and
    // rsLaunched (stale terrain-scroll/vbi flags in mem[]) can't install the doors/tunnel copper
    // over old data in the transitional frames.  game_main_loop lands on ONE of two rest scenes,
    // by cockpit_flag ($060B): the $53CC results/level-select CARD (standby_scoreboard_render, when
    // a game was played) or the $52D7 standby COCKPIT (fresh start).  Release on whichever is ready:
    //   card    : VVBLKI=$53CC + title text in $365B ('R'=$72) + DMA shadow $022F back on
    //   standby : VVBLKI=$52D7 + door field rebuilt (g_doorFieldReady, reset to 0 on restart above,
    //             re-set by boot_standby_launch_driver only after the $2000 field is drawn)
    if (g_restartHoldBlack) {
        // Track the render signals EVERY hold frame (not just on release): boot_standby_launch_driver
        // drives g_doorFieldReady 0->1 while we're holding, and deriveRenderSignals turns that edge
        // into the one-time door-field decode (terrainDirty) + full cockpit repaint (cockpitForceFull,
        // rising-edge on prevDoorFieldReady).  If we returned BEFORE deriveRenderSignals during the
        // hold, prevDoorFieldReady would never see the 0, the edge would be missed, and the doors
        // would come up half-decoded on release (bug 3).  Those flags aren't consumed until render()
        // runs (skipped while holding), so they persist to the release frame's decode.
        deriveRenderSignals();
        const uint16_t vv = (uint16_t)(mem[0x0222] | (mem[0x0223] << 8));
        const bool cardReady    = (vv == 0x53CCu) && (mem[0x365B] == 0x72u) && (mem[0x022F] != 0);
        const bool standbyReady = (vv == 0x52D7u) && g_doorFieldReady;
        if (!cardReady && !standbyReady) {
            if (emptyCopper && !emptyCopperInstalled) {
                emptyCopper->setColor00(atariToOCS(0));   // pure black
                AmigaHardware::setCopperList(*emptyCopper, false);
                emptyCopperInstalled = true;
                standbyCopperInstalled = false; planetCopperInstalled = false;
                flightCopperInstalled = false; tunnelCopperInstalled = false;
                titleScreenCopperInstalled = false;
            }
            return;
        }
        g_restartHoldBlack = false;   // resting scene rebuilt — resume normal rendering (decode fires)
    }

    // Lighter knock render path: during the airlock-closed alien knock ($0632) the whole flight
    // scene is frozen except the animating creature overlay, yet the game is parked in a blocking
    // per-frame loop (alien_knock_setup_loop).  renderFlightDirect already does a dirty-rect figure composite
    // (+ VBI flip) for the rescue-figure pause, so go straight to it and skip the full per-frame
    // machinery that would only recompute frozen state: deriveRenderSignals() rebuilds SIX cockpit
    // sprites every frame (altimeter/AH/scope/viewport-P3/scanner), plus the tunnel-band decode
    // checks and the flight-copper install/refresh.  All of those are stable across the pause (set by
    // the frames before the knock), so recomputing them per knock frame is the dominant render cost.
    // Gate == renderFlightDirect's own rescueFigure branch ($3E!=0 && $3D>=3) AND the knock flag.
    if (mem[0x0632] && mem[0x003E] != 0 && mem[0x003D] >= 3) {
        renderFlightDirect();
        return;
    }

    deriveRenderSignals();   // recompute the mem[]-derived render-gating signals for this frame
#if defined(ROF_FLIGHT_PROBE)
    // Diagnose why renderFlightDirect isn't reached during the knock: is renderFrame even entered
    // ($0632), what is VVBLKI, and is rsFlight/rsViewport true at that moment?
    { extern volatile unsigned long g_alRF; extern volatile unsigned short g_alVV;
      extern volatile unsigned char g_alRFfl, g_alRFvw;
      if (mem[0x0632]) { g_alRF++; g_alVV = (unsigned short)(mem[0x0222] | (mem[0x0223] << 8));
                         g_alRFfl = rsFlight ? 1 : 0; g_alRFvw = rsViewport ? 1 : 0; } }
#endif
    // Tunnel: the reveal (platform_tunnel_rings_drawn) decodes the full field once
    // (g_tunBandMode==0); thereafter the $52D7 VBI's draw_ring_frame_step draws each expanding
    // black clear frame into mem[$1000] and publishes the exact band it touched (g_tunBandMode==1
    // + the g_tun* bounds).  Decode only that band into tunnelBitmap — no shadow scan.
    // ⚠ SKIP during the boost: decodeBoostViewport owns the tunnelBitmap there.  The boost's
    // draw_frame_pattern_seq plots its ring field into $1000 and sets g_tunnelFieldDirty during
    // the STARS sub-phase; if this forward-tunnel path decodes it, the $1000 rings (whose value-0
    // exit-clear pixels are pen7 black) flash over the starfield on any frame where the boost
    // branch's decodeBoostViewport doesn't re-overwrite it = the bowtie + black-stars-on-salmon.
    // ⚠ SKIP only while the BOOST owns tunnelBitmap.  The boost owns it during:
    //   (a) rsBoostViewport — decodeBoostViewport is painting the stars / reverse tunnel; and
    //   (b) the T6 handoff hold AND its g_doorFieldReady 0->1 EDGE frame, where the tunnel copper
    //       is still the live display (copper installs defer to vblank, so staticStandby's list only
    //       takes over next frame) — a decode there re-paints $1000 rings into the still-shown
    //       tunnelBitmap = the ring FLASH before the LEVEL-NN card (bug 1).
    // It does NOT own it during a FORWARD launch, INCLUDING the next level's STANDBY pre-build:
    // tunnel_prebuild_rings (rof_native.c ~8780) fills $1000 + flags the one-time full decode while
    // still on the standby idle, where rsLaunched is false — so gating on rsLaunched alone skipped
    // that pre-build decode and the 2nd launch's doors opened onto a stale tunnelBitmap (no rings).
    // The signal that separates the handoff EDGE from the standby pre-build is WHICH copper is live:
    // at the edge the boost's tunnel copper is still installed (held), at the pre-build the standby
    // copper is.  ($003A stays $FF into the next level, and g_doorFieldReady is 1 at both, so neither
    // rsBoostReturn nor g_doorFieldReady can tell them apart; tunnelCopperInstalled can.)  !rsLaunched
    // then still lets the 2nd launch's actual tunnel DESCENT (tunnelCopperInstalled true) decode.
    if (g_tunnelFieldDirty) {
        const bool boostOwnsTunnel = rsBoostViewport ||
                                     (rsBoostReturn && tunnelCopperInstalled && !rsLaunched);
        if (!boostOwnsTunnel) {
            tunnelSrcBase = 0x1000u;   // forward tunnel rings live in $1000 (boost may have left $2000)
            if (g_tunBandMode) decodeTunnelBand();
            else               decodeTunnelRect((int)g_tunRowLo, (int)g_tunRowHi, 0, 39);
            g_tunnelFieldDirty = 0;    // consume ONLY when actually decoded (see the DEFER note below)
        }
        // ⚠ Do NOT clear g_tunnelFieldDirty when skipped — DEFER the decode.  The next forward
        // launch's one-shot FULL pre-build (tunnel_prebuild_rings -> mode 0, rows 0-85) fires at
        // the boost handoff edge, where the boost's tunnel copper is still live (tunnelCopperInstalled)
        // so decoding it here would flash rings over the LEVEL-NN card (bug 1).  Keeping the dirty flag
        // set retries next frame, by which point staticStandby has taken over (tunnel copper no longer
        // installed, tunnelBitmap not displayed) and the reverse ring is finished (mode still 0), so the
        // full field decodes OFF-SCREEN — ready for when the player launches (fixes "doors open, no
        // tunnel").  Measured: pre-build skipped at the edge, then decoded the next frame.
    }
#ifdef ROF_FLIGHT_PROBE
    extern volatile unsigned long g_rPerFrame, g_rRenderFn;
    const bool _profR = rsFlight;
    unsigned long _p0 = _profR ? rof_subclock() : 0, _pi = _profR ? g_isrBeamLines : 0;
#endif
    perFrameWork();
#ifdef ROF_FLIGHT_PROBE
    if (_profR) { g_rPerFrame += (rof_subclock() - _p0) - (g_isrBeamLines - _pi);
                  _p0 = rof_subclock(); _pi = g_isrBeamLines; }
#endif
    render();
#ifdef ROF_FLIGHT_PROBE
    if (_profR) g_rRenderFn += (rof_subclock() - _p0) - (g_isrBeamLines - _pi);
#endif

    // Death-cinematic teardown ($4F76): once the game blanks ANTIC DMA (g_flightBlank, set by
    // the DMACTL $D400=0 write while VVBLKI is still the flight $4FF5), show a SOLID full-screen
    // colour — the EmptyCopperList (0 bitplanes → whole display = color00) with color00 poked
    // from mem[$00D4].  That reproduces the Atari's "DMA off, only COLBK shows" salmon-hold →
    // black fade (COLBK ramps $2F salmon → black in $4F76).  The flash + cockpit-salmon tint ran
    // on FlightCopperList before this; the JMP $3D38 that ends the cinematic flips VVBLKI away
    // from $4FF5, so rsFlight drops → g_flightBlank is cleared below and the Title (game-over)
    // takes over.  See the death-cinematic memory.
    static bool deathBlankInstalled = false;
    if (!rsFlight) g_flightBlank = 0;   // safety: never carry the blank out of flight
    if (rsFlight && g_flightBlank && emptyCopper) {
        emptyCopper->setColor00(atariToOCS(mem[0x00D4]));   // COLBK fade colour, poked every frame
        if (!deathBlankInstalled) {
            AmigaHardware::setCopperList(*emptyCopper, false);
            deathBlankInstalled    = true;
            flightCopperInstalled  = false; standbyCopperInstalled  = false;
            planetCopperInstalled  = false; tunnelCopperInstalled   = false;
            titleScreenCopperInstalled = false;
        }
        return;
    }
    deathBlankInstalled = false;   // left the blank phase → allow a fresh install next death

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
        // Atari game-over black: the Title screen comes up while ANTIC DMA is still OFF (the
        // death teardown $4F76 left SDMCTL/$D400 off; game_main_loop re-clears the $022F shadow
        // at entry).  standby_scoreboard_render ($587B) writes the LAST/HIGH SCORE + level digits into
        // $365B and STARTS the game-over music, and only THEN, at its $595a, does
        // display_list_init + SDMCTL($022F)=$22 turn DMA back on — so on the Atari the screen is
        // completely BLACK from game-over entry until the music begins, and the text + score
        // appear TOGETHER.  Mirror that: while the game's DMA shadow is off, keep the screen
        // black (the blank EmptyCopperList) instead of showing the decoded static text early.
        // A legitimately-visible Title (attract idle / level-select) always runs with DMA on
        // (the $5A82 Title DL can't display otherwise), so this only blacks the game-over
        // pre-music window; when $022F flips to $22 the full decode below paints text+score at once.
        static bool titleBlankInstalled = false;
        if (emptyCopper && mem[0x022F] == 0) {
            if (!titleBlankInstalled) {
                emptyCopper->setColor00(atariToOCS(0));   // pure black (COLBK off)
                AmigaHardware::setCopperList(*emptyCopper, false);
                titleBlankInstalled = true;
                titleScreenCopperInstalled = false;   // force a fresh full decode when DMA returns
                standbyCopperInstalled = false; planetCopperInstalled = false;
                flightCopperInstalled = false; tunnelCopperInstalled = false;
            }
            return;
        }
        titleBlankInstalled = false;
        if (!titleScreenCopperInstalled) {
            // Entry: decode the whole screen once, and drop any pending value-cell
            // dirty range (the full decode already captured everything).
            decodeTitleScreen();
            g_titleCellLo = 120; g_titleCellHi = -1;
            updateTitleScreenCopper(true);
            AmigaHardware::setCopperList(*titleScreenCopper, false);
            titleScreenCopperInstalled = true;
        } else {
            // Thereafter only the VALUES change, and we know exactly when: the STARTING
            // LEVEL digit as joystick up/down selects the level (setup_initials_ptr $5A63)
            // and the LAST/HIGH SCORE digits on the game-over build (standby_scoreboard_render $587B,
            // whose template copy trips rsTitle BEFORE the digits are plotted a few
            // instructions later).  Those writers mark a dirty cell range via the
            // rof_title_screen_dirty() hook; redraw only that range so the screen never fully
            // repaints (no flash).  No shadow-compare / checksum scan.
            if (g_titleCellHi >= g_titleCellLo) {
                decodeTitleCells(g_titleCellLo, g_titleCellHi);
                g_titleCellLo = 120; g_titleCellHi = -1;
            }
            updateTitleScreenCopper(false);
        }
        standbyCopperInstalled = false; planetCopperInstalled = false;
        flightCopperInstalled = false; tunnelCopperInstalled = false;
        return;
    }

    // Boost / return-to-mother-ship reverse cinematic — stars + reverse tunnel rings (scene
    // 2b/5 played backwards).  Reuse the TunnelCopperList (its viewport palette tracks the live
    // $08D4-$08D9 ring / $0071 fade / $02C0 black registers, so it follows the boost palette
    // automatically).  The reverse ring does NOT publish the g_tun* dirty band the forward ring
    // does, so decode the full field each frame (a brief cinematic, not the 50 FPS flight budget).
    //
    // ⚠ SOURCE BUFFER SWITCHES per sub-phase (the KEY faithful detail — the $3000 launch DL's
    // mode-F LMS is rewritten mid-cinematic, measured from boost_stars/boost_tunnel savestates):
    //   $008D==0 (stars): the DL displays the $2000 GTIA-10 field = the black starfield (value-8
    //     bg + sparse dots).  $1000 is empty/being-built here.
    //   $008D!=0 (reverse tunnel): emit_dl_coord_pairs has rewritten the DL LMS to the $1000 ring
    //     field (the concentric rings).
    // (The committed code always decoded $1000 → stars showed the empty/bowtie ring field instead
    // of the starfield.  See docs/boost-cinematic-plan.md §1b.)  Placed before staticStandby so it
    // wins over the (mispositioned) Standby door copper the forward gates would otherwise select.
    if (rsBoostViewport && tunnelCopper) {
        if (!tunnelCopperInstalled) {
            // Defer the install until the star pens are seeded.  boot_standby_launch_driver writes the star
            // pens $08D4-$08D9 (=color_ring) ONE frame after the boost viewport becomes active,
            // so on the very first boost frame they are still $00 (black) while the fade bg $0071
            // is salmon.  Installing the copper here (with black color01-07) and then decoding the
            // star pixels next frame paints BLACK stars over the salmon fade for a frame (the
            // copper's colour writes lag the bitmap by one frame).  So while the pens are unseeded
            // during the stars sub-phase, keep the previous (ascent) copper — which shows the same
            // salmon fade — and install only once the pens are valid, with correct colours.
            if (mem[0x008D] == 0u && mem[MEM_color_ring] == 0u)
                return;   // pens not ready yet — hold the ascent copper one more frame
            // First boost-viewport frame: the faithful boot_standby_launch_driver writes the $2000 starfield
            // ONE frame later, so the source field may still hold the stale standby door-field
            // ("LEVEL NN") content and would decode as a garbage flash.  Clear the bitmap instead
            // (→ pen0 = color00 = $0071, the salmon fade bg) and skip this frame's decode; the next
            // frame decodes the ready starfield.
            uint8_t* bd = (uint8_t*)tunnelBitmap->data;
            for (int i = 0; i < 120 * (int)kTerrainHeight; i++) bd[i] = 0;
            updateTunnelCopper(true);
            AmigaHardware::setCopperList(*tunnelCopper, false);
            tunnelCopperInstalled = true;
            g_boostStarsDirty = 1;          // force the first stars decode once the field is ready
        } else if (mem[0x008D] == 0u) {
            // STARS sub-phase: whole viewport = the $2000 starfield.  fill_region_2000 is the sole
            // $2000 writer and re-fills the field only twice per cinematic (the star fade in between
            // is palette-only, read live by the copper), so decode ONLY when g_boostStarsDirty says
            // the content changed — not the old ~56ms full decode EVERY frame.
            if (g_boostStarsDirty) {
                decodeBoostViewport();
                g_boostStarsDirty = 0;
#ifdef ROF_FLIGHT_PROBE
                { extern volatile unsigned long g_bStarDec; g_bStarDec++; }
#endif
            }
            updateTunnelCopper(false);
        } else {
            // REVERSE TUNNEL sub-phase: decode the full boost viewport every frame.  The $1000 ring
            // field is written by BOTH the main loop (draw_frame_pattern_seq's one-shot pre-draw)
            // AND the VBI (step_accum_sub_7e's ring draws), progressively REVEALED row by row as
            // emit_dl_coord_pairs rewrites the $3000 DL LMS $2000->$1000 (the symmetric band
            // decodeBoostViewport reads).  A decode-on-change gate here proved fragile: skipping a
            // frame HOLDS any mid-reveal / mid-clear state the next writer hasn't settled yet (a
            // stray teal reveal row; the final clear's outermost-ring colour), which per-frame
            // re-decoding heals automatically.  This sub-phase is brief, so pay the full decode.
            // (The big win — the ~150-frame STARS sub-phase — is safely dirty-gated above.)
            decodeBoostViewport();
#ifdef ROF_FLIGHT_PROBE
            { extern volatile unsigned long g_bTunDec; g_bTunDec++; }
#endif
            updateTunnelCopper(false);
        }
        standbyCopperInstalled = false; planetCopperInstalled = false;
        flightCopperInstalled = false; titleScreenCopperInstalled = false;
#ifdef ROF_FLIGHT_PROBE
        { extern volatile unsigned char g_liveCopper; g_liveCopper = 5; }
#endif
        return;
    }

    // Boost handoff hold (T6).  When the reverse tunnel ends ($008D clears) rsBoostViewport
    // goes false (its gate is $008D!=0 || $008E==0, and the VBI has bumped $008E by then), but
    // boot_standby_launch_driver then spends ~13 frames finishing the NEXT-level Standby door field: it
    // rebuilds $2000 via fill_region_2000 + blit_message_block/blit_numeric_readout at L_6118,
    // and only THEN latches g_doorFieldReady (rof_native.c ~8783).  In that window rsBoostViewport
    // is false (so we've left the boost branch above) but g_doorFieldReady is still 0 (so
    // staticStandby below can't fire yet) — so control would fall through to the forward-launch
    // doors/tunnel fallthrough (~line 2046) and paint the stale/partial $2000 field for those
    // frames (the "black-top + green-doors '04', then re-render as LEVEL NN" glitch, bug 6).
    // Hold instead: keep whatever copper is live (the last reverse-ring frame) and skip re-decode
    // until the door field is ready; staticStandby then takes over cleanly on the g_doorFieldReady
    // 0->1 edge with the finished LEVEL-NN field.
    // In-place level cycle (post-mother-ship SELECT): both the door scroll (level<max) and the
    // intro_screen_build_seq fade-rebuild WRAP (level>=max) run with g_doorFieldReady cleared (at
    // L_6332) while rsBoostReturn holds ($003A==$FF).  Unlike the boost reverse-tunnel handoff below
    // (which arrives on the tunnel copper), this comes from the live static standby
    // (standbyCopperInstalled && !tunnelCopperInstalled).  Keep the standby copper live and ANIMATE
    // it rather than freezing: poke the per-frame colours so the WRAP's $0071 dark-green fade shows
    // (was frozen by the boost-handoff-hold below), and re-decode the door bitmap when the LEVEL
    // digit changes (g_doorScrollFieldDirty) so the fade-up reveals the new level.  The door-scroll
    // case spins in the main loop (renderFrame isn't called mid-scroll; doorScrollVblankUpdate drives
    // the scroll from the ISR), so this only refreshes colours + the decode at the scroll boundaries.
    if (rsBoostReturn && !g_doorFieldReady && standbyCopperInstalled && !tunnelCopperInstalled) {
        if (g_doorScrollFieldDirty) { decodeDoorScrollField(); g_doorScrollFieldDirty = 0; }
        updateStandbyCopper(false);   // pokes terrain color00/03 = atariToOCS(mem[$0071]) fade ramp
#ifdef ROF_FLIGHT_PROBE
        { extern volatile unsigned char g_liveCopper; g_liveCopper = 10; }   // 10 = in-place wrap fade
#endif
        return;
    }

    if (rsBoostReturn && !g_doorFieldReady) {
#ifdef ROF_FLIGHT_PROBE
        { extern volatile unsigned char g_liveCopper; g_liveCopper = 8;
          extern volatile unsigned long g_boostHandoffHoldFrames; g_boostHandoffHoldFrames++; }
#endif
        // Row-by-row band-triangle recede (teal -> dark green) while the viewport stays frozen.
        // boot_standby_launch_driver fills the canopy-wedge buffer $0C88-$0C8F with $FF TOP-DOWN, one row per
        // frame (rof_native.c ~8763-8767) — on the Atari the green quad-player corner triangle
        // (COLPM0/1 = mem[$0071] = $C0 dark green) grows over the tunnel teal (the outermost-ring
        // corner mem[$08D8]).  Mirror it: band top = the dark-green door colour, then flip color00
        // to teal from the first STILL-EMPTY wedge row down (setBandReveal), so rows 0..k-1 read
        // green and k..7 read teal, with k = filled-row count growing 0->8.  (Analogous to the
        // FORWARD doors->tunnel green->purple reveal, just the opposite colour + fill direction.)
        // Measured: $0C88 fills 00->ff top-down over the 8 hold frames; $0071 = $C0 throughout.
        if (tunnelCopper && tunnelCopperInstalled) {
            uint16_t k = 8;
            for (uint16_t i = 0; i < 8; i++) if (mem[0x0C88 + i] == 0u) { k = i; break; }
            tunnelCopper->setBandTopColor00(true, atariToOCS(mem[0x0071]));   // band top = dark green
            tunnelCopper->setBandReveal(k, atariToOCS(mem[0x08D8]));          // teal from row k down
        }
        return;
    }

    const bool staticStandby = standbyCopper && rsStandby && g_doorFieldReady
                               && !rsViewport && !rsLaunched && !rsBoostViewport;
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
#ifdef ROF_FLIGHT_PROBE
        { extern volatile unsigned char g_liveCopper; g_liveCopper = 2; }
#endif
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
#ifdef ROF_FLIGHT_PROBE
        extern volatile unsigned long g_rCopper;
        unsigned long _c0 = rof_subclock(), _ci = g_isrBeamLines;
#endif
        if (!flightCopperInstalled) {
            updateFlightCopper(true);
            AmigaHardware::setCopperList(*flightCopper, false);
            flightCopperInstalled = true;
#ifdef ROF_FLIGHT_PROBE
            extern volatile unsigned short g_fadeEntryVbi, g_fadeEntryIter, g_fadeEntryFd, g_iterCount;
            extern volatile unsigned long g_fdCalls;
            extern volatile unsigned char g_fadeEntryDC, g_fadeEntryState, g_fadeEntry66C, g_fadeEntryAlt, g_fadeDone;
            g_fadeEntryVbi = (unsigned short)(rof_subclock() / 313u);
            g_fadeEntryIter = g_iterCount;
            g_fadeEntryFd = (unsigned short)g_fdCalls;
            g_fadeEntryDC = mem[0x00DC]; g_fadeEntryState = mem[0x0041];
            g_fadeEntry66C = mem[0x066C]; g_fadeEntryAlt = mem[0x0034];
            g_fadeDone = 0;
#endif
        } else {
            updateFlightCopper(false);
        }
#ifdef ROF_FLIGHT_PROBE
        g_rCopper += (rof_subclock() - _c0) - (g_isrBeamLines - _ci);
        {
            extern volatile unsigned short g_fadeStartVbi, g_fadeStartIter, g_fadeStartFd, g_iterCount;
            extern volatile unsigned long g_fdCalls;
            extern volatile unsigned char g_fadeEntryDC, g_fadeDone, g_fadeStartState, g_fadeStart66C, g_fadeStartAlt;
            extern volatile unsigned short g_fadeLoopVbi;
            if (!g_fadeDone && g_fadeLoopVbi == 0 && g_iterCount != 0)
                g_fadeLoopVbi = (unsigned short)(rof_subclock() / 313u);
            if (!g_fadeDone && mem[0x00DC] != g_fadeEntryDC) {
                g_fadeStartVbi = (unsigned short)(rof_subclock() / 313u);
                g_fadeStartIter = g_iterCount;
                g_fadeStartFd = (unsigned short)g_fdCalls;
                g_fadeStartState = mem[0x0041]; g_fadeStart66C = mem[0x066C]; g_fadeStartAlt = mem[0x0034];
                g_fadeDone = 1;
            }
        }
#endif
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
    const uint16_t g2   = rsLaunched ? (uint16_t)(0x2Bu - mem[MEM_terrain_scroll_counter]) : 0;
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
#ifdef ROF_FLIGHT_PROBE
        // Bug 3 probe: at the moment a doors frame is shown, is the TOP of viewportBitmap (the top
        // door band, rows g2..) blank?  Sample the first 6 rows across all 3 planes; if every byte is
        // 0 the top door is black.  Record the worst (earliest, smallest g2) case per run.
        { extern volatile unsigned char g_doorTopBlack, g_doorTopG2, g_doorTopSeen;
          const uint8_t* vp = (const uint8_t*)viewportBitmap->data + (uint32_t)g2 * 120u;
          int nz = 0; for (int i = 0; i < 6 * 120; i++) if (vp[i]) { nz = 1; break; }
          if (!g_doorTopSeen || (unsigned char)g2 <= g_doorTopG2) {   // earliest / smallest-g2 doors frame
              g_doorTopBlack = nz ? 0 : 1; g_doorTopG2 = (unsigned char)g2; g_doorTopSeen = 1; } }
#endif
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
    const uint16_t titlePf0 = atariToOCS(mem[MEM_text_color_pf0]);   // COLPF0 = title text ($00D8)
    const uint16_t energyCol = atariToOCS(mem[0x00DE]);             // gauge bar colour ramp
    const uint16_t terr0    = atariToOCS(mem[0x02C0]);             // terrain pen0 (road dots)
    const uint16_t terr1    = atariToOCS(mem[0x02C7]);             // terrain pen1 (LEVEL text)
    const uint16_t terr2    = atariToOCS(mem[MEM_color_ring]);      // terrain pen2 ($08D4)
    const uint16_t terr3    = atariToOCS(mem[MEM_display_flags]);   // terrain pen3 (green bg, $0071)
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
// shadows ($02C4-$02C7).  The Atari cycles these in the $53CC vbi_handler_1 body (the
// $53ED loop, X=4..0): pen = ($02C4+X EOR $0013) & $F6 when the attract fraction $0002 has
// gone negative (bit7 set), else the raw shadow — then STA $D016,X (COLPF0-3/COLBK).  That
// HW write is skipped on the Amiga (the copper owns the pens), so we reproduce the SAME
// modulation here on the source shadows before converting.  $0002/$0013 are maintained by
// vbi_handler_1_native (rof_native_amiga.cpp).  Poke-on-change.
void RescueOnFractalus::updateTitleScreenCopper(bool force)
{
    const bool cyc = (mem[MEM_rtclok_frac] & 0x80u) != 0u;   // $0002 negative -> cycle active
    const uint8_t mid = mem[MEM_RTCLOK_MID];                 // $0013 modulator
    auto penOf = [&](uint16_t sh) -> uint16_t {
        uint8_t v = mem[sh];
        if (cyc) v = (uint8_t)((v ^ mid) & 0xF6u);
        return atariToOCS(v);
    };
    const uint16_t pf0 = penOf(0x02C4);
    const uint16_t pf1 = penOf(0x02C5);
    const uint16_t pf2 = penOf(0x02C6);
    const uint16_t pf3 = penOf(0x02C7);
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
    const uint16_t titlePf0 = atariToOCS(mem[MEM_text_color_pf0]);   // COLPF0 = title text ($00D8)
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
    // Death cinematic: while it runs (event_trigger $063D != 0, DMA still on before $4F76) the
    // frame greys must fade to salmon with the cockpit.  On the Atari the top-bar background +
    // canopy pillars are COLBK = mem[$00D4] (the frame-grey display param $4FE0 ramps to hue-2);
    // normal flight uses the $02C8 shadow.  Drive titleBg from $00D4 during the cinematic so the
    // top bar + pillars (setSpritePostColor(titleBg)) fade too — not just the band.
    const bool cine = (mem[0x063D] != 0);
    const uint16_t titleBg  = atariToOCS(cine ? mem[0x00D4] : mem[0x02C8]);  // COLBK = top-bar bg / canopy posts
    const uint16_t titlePf0 = atariToOCS(mem[MEM_text_color_pf0]);   // COLPF0 = top-bar text ($00D8)
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
        // Player laser (P2) — ch4 is otherwise idle.  Point at the BACK buffer so the first
        // buildShotSprite (which writes shotSprite, idx 0) never touches the displayed buffer.
        flightCopper->setHudSprite(4, *shotSpriteBack);
        flightCopper->setHudSprite(5, *energyIndicatorSprite);
        flightCopper->setHudSprite(6, *altimeterSprite);
        // ch7 is multiplexed: the VIEWPORT half shows the Main-Window P3 object (ch7 top pointer,
        // SPR7PT via setHudSprite), the DASHBOARD half shows the altimeter-ship gauge (the
        // SPR7PT re-point at the cockpit WAIT, setDashboardSprite(7, ...)).
        flightCopper->setHudSprite(7, *viewportP3Sprite);
        flightCopper->setDashboardSprite(7, *altimeterShipSprite);
        // Long Range Scanner guide dot (Atari M2) on ch2 (idle in the dashboard) via SPR2PT.
        flightCopper->setDashboardSprite(2, *scannerDotSprite);
    }

    // HUD sprite colours sourced (on the Atari) from the cockpit DLI $4A78's display params —
    // altimeter terrain P0 ← $00D5 (COLPM0), ship M3 ← $00D6 (COLPM3), AH ground P2 ← $00D0
    // (COLPM2).  These are constant in normal flight, so poke-on-change costs nothing; but the
    // death cinematic's $4FE0 ramps $00CF-$00D6 to salmon, so driving them live makes the gauges
    // fade gray→salmon WITH the cockpit bitmap + canopy posts (matching the Atari — the DLI
    // reloads COLPM from these shadows every frame).  (Energy P1 ← $00DE is OUTSIDE the ramp, so
    // it correctly does not fade; the ship is empty at energy-out anyway.)
    const uint16_t altimCol  = atariToOCS(mem[0x00D5]);
    const uint16_t shipCol   = atariToOCS(mem[0x00D6]);
    const uint16_t ahGround  = atariToOCS(mem[0x00D0]);
    if (force || altimCol != flAltimCol)     { flightCopper->setAltimeterColor(altimCol);      flAltimCol = altimCol; }
    if (force || shipCol  != flAltimShipCol) { flightCopper->setAltimeterShipColor(shipCol);   flAltimShipCol = shipCol; }
    if (force || ahGround != flAHGround)     { flightCopper->setAHGroundColor(ahGround);       flAHGround = ahGround; }
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

    // Terrain salmon→brown fade (#2): the flight VBI atmosphere ramp ($51C8) computes ALL
    // FOUR terrain pens each frame from altitude ($0034 → tables $07F9/$0823/$084D/$0877),
    // matching the Atari viewport DLI $4A1F which loads them into the mode-D pens:
    //   pen0 = body      (value0 = COLBK)  ← $00DC
    //   pen1 = sky       (value1 = COLPF0) ← $00DD
    //   pen2 = dots      (value2 = COLPF1) ← $00DA
    //   pen3 = highlight (value3 = COLPF2) ← $00DB
    // Poke them all as they ramp (the "native computes, callback updates the copper" model).
    // The plane-2 dots ($00DA) start salmon at flight entry and fade in WITH the terrain,
    // just like the body/sky — so pen2 must track $00DA, not a baked constant.
    const uint16_t terr0 = atariToOCS(mem[0x00DC]);
    const uint16_t terr1 = atariToOCS(mem[0x00DD]);
    const uint16_t terr2 = atariToOCS(mem[0x00DA]);
    const uint16_t terr3 = atariToOCS(mem[0x00DB]);
    if (force || terr0 != flTerr0 || terr1 != flTerr1 || terr2 != flTerr2 || terr3 != flTerr3) {
        flightCopper->setTerrainPalette(terr0, terr1, terr2, terr3);
        flTerr0 = terr0; flTerr1 = terr1; flTerr2 = terr2; flTerr3 = terr3;
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

    // Targeting-crosshair (#10) visibility — faithful to the Atari missile HPOS gate.  The flight
    // VBI ($505F) pushes HPOSM3=mem[$2840] / M2=+$0C / M1=+$11 every frame, and $A49A sets
    // mem[$2840] = ($28FC==0) ? $00 : $74: at $74 the "+" sits at screen centre (visible), at $00 the
    // three missiles sit off the left edge (hidden).  The plane3 "+" is drawn unconditionally
    // (renderFlightDirect); we gate visibility purely by colour — visible → salmon $26; hidden →
    // the terrain pens (color00-03) so the plane3 pixels read identical to the terrain = invisible.
    if (mem[0x2840] != 0) {
        const uint16_t cross = atariToOCS(0x26);
        flightCopper->setCrosshairPalette(cross, cross, cross, cross);
    } else {
        flightCopper->setCrosshairPalette(terr0, terr1, terr2, terr3);
    }

    // Death-cinematic cockpit fade: the Atari cockpit DLI loads its pens from the display params
    // $00CF-$00D6, which intro_fill_display_params ($4FE0) ramps to salmon (hue 2) during the
    // cinematic.  Drive the FlightCopperList cockpit palette + dashboard-blue bg from them while
    // the cinematic runs, so the WHOLE dashboard (body, divider, gauge housings) tints gray→salmon
    // with the frame — not just the band.  Mapping mirrors the buildLayout bake (each baked value
    // equals a $00CF-$00D6 base): color00/04←$D3, 01/05←$CF, 02/06←$D4, 03←$D1, 07←$D0, dash←$D2.
    // Normal flight leaves the baked palette alone (those params hold their base values then); the
    // baked constants are restored once when the cinematic ends (or on the next flight-entry force).
    if (cine) {
        flightCopper->setCockpitPalette(
            atariToOCS(mem[0x00D3]), atariToOCS(mem[0x00CF]), atariToOCS(mem[0x00D4]), atariToOCS(mem[0x00D1]),
            atariToOCS(mem[0x00D3]), atariToOCS(mem[0x00CF]), atariToOCS(mem[0x00D4]), atariToOCS(mem[0x00D0]));
        flightCopper->setDashBg(atariToOCS(mem[0x00D2]));
        flCinePrev = true;
    } else if (flCinePrev || force) {
        flightCopper->setCockpitPalette(
            atariToOCS(0x00), atariToOCS(0x04), atariToOCS(0x06), atariToOCS(0x2C),
            atariToOCS(0x00), atariToOCS(0x04), atariToOCS(0x06), atariToOCS(0x26));
        flightCopper->setDashBg(atariToOCS(0x90));
        flCinePrev = false;
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
    const uint16_t titlePf0 = atariToOCS(mem[MEM_text_color_pf0]);  // COLPF0 = title text ($00D8)
    const uint16_t energyCol = atariToOCS(mem[0x00DE]);            // gauge bar colour ramp
    const uint16_t compassCol = atariToOCS(mem[0x00CF]);          // compass band COLPF0

    dc->setTitlePalette(titleBg, titlePf0, atariToOCS(0x78));     // pf1 = blue (const)
    dc->setSpritePostColor(titleBg);
    dc->setEnergyIndicatorColor(energyCol);
    dc->setCompassColor(compassCol);

    // Sliding-door geometry.  topBase = terrain row g2 (slides up); tunBase = tunnel row
    // (half - g2) (the reveal centred on the vanishing point); botBase = terrain row half.
    const uint16_t half = (uint16_t)(kTerrainHeight / 2);
    const uint16_t g2   = rsLaunched ? (uint16_t)(0x2Bu - mem[MEM_terrain_scroll_counter]) : 0;
    const uint32_t ta   = (uint32_t)viewportBitmap->data;   // door halves live in the shared pre-flight viewport bitmap
    // pen0 = COLBK green ($0071), pen3 = road-dot dark ($02C0): the door field decodes
    // COLBK (value 8)→pen0 and the dark dots (value 0)→pen3 (see kNibbleColour).  color00
    // green then flows unbroken through all three terrain bands AND the tunnel reveal into
    // the windscreen band (the Atari keeps COLBK green across the whole viewport+band — see
    // doors_mid, COLBK=$C8 y50-136), so the band corners inherit it: no setBandBgColor.
    dc->update(g2,
                        ta + (uint32_t)g2 * 120u,
                        (uint32_t)tunnelBitmap->data + (uint32_t)(half - g2) * 120u,
                        ta + (uint32_t)half * 120u,
                        atariToOCS(mem[MEM_display_flags]),       // pen0 = COLBK green ($0071)
                        atariToOCS(mem[0x02C7]),                 // terrain pen1
                        atariToOCS(mem[MEM_color_ring]),          // terrain pen2 ($08D4)
                        atariToOCS(mem[0x02C0]),                 // pen3 = road-dot dark ($02C0)
                        atariToOCS(mem[MEM_color_ring + 0]),      // ring pen4 ($08D4)
                        atariToOCS(mem[MEM_color_ring + 1]),      // ring pen5 ($08D5)
                        atariToOCS(mem[MEM_color_ring + 2]),      // ring pen6 ($08D6)
                        atariToOCS(mem[MEM_color_ring + 3]),      // tunnel pen1 ($08D7)
                        atariToOCS(mem[MEM_color_ring + 4]),      // tunnel pen2 ($08D8)
                        atariToOCS(mem[MEM_color_ring + 5]));     // tunnel pen3 ($08D9)
}

// updateTunnelCopper(): refresh the TunnelCopperList for the full tunnel descent (scene 5,
// doors fully open).  Constant title/gauge/compass via poke-on-change; the tunnel ring
// palette (pen0 black + pens 1-6 fed by the rotating $08D4-$08D9 ring, +3 rotated as the
// Atari tunnel DLI applies) is poked when any entry changed.
void RescueOnFractalus::updateTunnelCopper(bool force)
{
    const uint16_t titleBg  = atariToOCS(mem[0x02C8]);
    const uint16_t titlePf0 = atariToOCS(mem[MEM_text_color_pf0]);
    const uint16_t energyCol = atariToOCS(mem[0x00DE]);
    const uint16_t compassCol = atariToOCS(mem[0x00CF]);
    // Canopy-post/pillar grey.  The forward tunnel colours the posts from the title bg
    // ($02C8).  In the BOOSTERS reverse cinematic $02C8 is black (posts vanish) — the Atari
    // pillars are 5th-player missiles at the fixed frame grey COLPF3=$06 (measured T0,
    // boost_stars/boost_tunnel savestates), so use that in boost mode.
    const uint16_t postCol = rsBoostViewport ? atariToOCS(0x06) : titleBg;

    if (force || titleBg != tnTitleBg || titlePf0 != tnTitlePf0) {
        tunnelCopper->setTitlePalette(titleBg, titlePf0, atariToOCS(0x78));
        tnTitleBg = titleBg; tnTitlePf0 = titlePf0;
    }
    if (force || postCol != tnPostCol) { tunnelCopper->setSpritePostColor(postCol); tnPostCol = postCol; }
    if (force || energyCol != tnEnergyCol)   { tunnelCopper->setEnergyIndicatorColor(energyCol);   tnEnergyCol = energyCol; }
    if (force || compassCol != tnCompassCol) { tunnelCopper->setCompassColor(compassCol); tnCompassCol = compassCol; }
    // Windscreen-band corner + tunnel palette.  FORWARD and BOOST share the copper but use DIFFERENT
    // GTIA->pen mappings (kGtia10P vs kGtia10BoostP), so their palette wiring differs.  Shared bits:
    uint16_t ring[6];
    for (int i = 0; i < 6; i++) ring[i] = atariToOCS(mem[MEM_color_ring + i]);
    const uint16_t black = atariToOCS(mem[0x02C0]);          // color07 = value-0 (COLPM0), both paths

    if (rsBoostViewport) {
        // BOOST reverse cinematic (kGtia10BoostP): value-2 (outermost ring) -> color00, value-8
        // (background) -> color02.  So the band-corner triangle (mode-D value-0 -> color00) IS the
        // outermost ring, and the star / unrevealed background keeps its own COLBK register.
        //   color00 = the outermost ring $08D8 during the tunnel; the COLBK fade $0071 during stars.
        const uint16_t col00 = (mem[0x008D] == 0u) ? atariToOCS(mem[0x0071]) : atariToOCS(mem[0x08D8]);
        const uint16_t colBK = atariToOCS(mem[0x0071]);     // color02 = value-8 background (COLBK)
        // The band triangle must stay BLACK until the OUTER tunnel rows are actually drawn (early in
        // the expansion they're still the value-8 background = dark), then follow the ring.  Latch on
        // $008D going negative (= outermost ring rendered); reset at the stars phase.
        if (mem[0x008D] == 0u && mem[0x008E] == 0u) boostRingRevealed = false;
        if ((int8_t)mem[0x008D] < 0)                boostRingRevealed = true;
        // The band triangle needs its color00 set BLACK only during expansion; during stars (inherit
        // the $0071 fade) and once the outermost ring is drawn (inherit the ring $08D8) it must NOT be
        // re-touched — else the copper flip lands ~16px into the band's first line (teal stripe).  Do
        // the black flip EARLY (band-top slot, before the cockpit bitplane overrun) and NO-OP the late
        // slot; the no-touch phases no-op BOTH so color00 simply carries in from the viewport.
        const bool bandBlack = (mem[0x008D] != 0u) && !boostRingRevealed;   // expanding only
        tunnelCopper->setBandTopColor00(bandBlack, atariToOCS(0x00));
        tunnelCopper->disableBandReveal();                                  // late slot unused by boost

        bool changed = (col00 != tnCorner) || (black != tnPen0) || (colBK != tnColBK);
        for (int i = 0; i < 6; i++) if (ring[i] != tnRing[i]) changed = true;
        if (force || changed) {
            // pen0=color00 (value-2 outermost ring); pen1/3=ring[3]/ring[5] (COLPM1/3); pen2=color02
            // (value-8 COLBK); pen4/5/6=ring[0..2] (COLPF0/1/2); pen7=value-0 black.
            tunnelCopper->setTunnelColors(col00, ring[3], colBK, ring[5], ring[0], ring[1], ring[2], black);
            tnCorner = col00; tnPen0 = black; tnColBK = colBK;
            for (int i = 0; i < 6; i++) tnRing[i] = ring[i];
        }
    } else {
        // FORWARD tunnel (kGtia10P) — the long-working mapping, UNCHANGED.  The corner triangle is the
        // quad-width canopy-post player ($0C88-$0C8F), green (COLPM0/1 = mem[$0071]); the launch clears
        // it top-down so the tunnel ($08D8) shows through.  Rendered WITHOUT a per-band poke: color00
        // (value-8) carries the tunnel corner from the viewport into the band (value-0 -> spare pen7),
        // and a moving WAIT flips color00 to green from the first still-set player scanline down.
        tunnelCopper->setBandTopColor00(false, 0);               // no-op the boost band-top slot
        uint16_t greenLine = 8;                                   // first still-green band scanline
        for (uint16_t i = 0; i < 8; i++) { if (mem[0x0C88 + i]) { greenLine = i; break; } }
        tunnelCopper->setBandReveal(greenLine, atariToOCS(mem[0x0071]));

        const uint16_t corner = atariToOCS(mem[0x08D8]);    // color00 = value-8 = the tunnel corner
        bool ringChanged = (corner != tnCorner) || (black != tnPen0);
        for (int i = 0; i < 6; i++) if (ring[i] != tnRing[i]) ringChanged = true;
        if (force || ringChanged) {
            // pen0/color00 = corner ($08D8); pens 1-3 = ring[3..5]; pens 4-6 = ring[0..2]; pen7 = black.
            tunnelCopper->setTunnelColors(corner, ring[3], ring[4], ring[5], ring[0], ring[1], ring[2], black);
            tnCorner = corner; tnPen0 = black;
            for (int i = 0; i < 6; i++) tnRing[i] = ring[i];
        }
    }
}

// deriveRenderSignals(): recompute the renderer's phase-gating signals from mem[]
// hardware state, once per frame.  These replace the C++ launchPhase enum as the
// renderer's source of truth, so the copper-list selection/render/perFrameWork keep
// working as the transpiled game_entry/game_main_loop/boot_standby_launch_driver drive the program.
//
// Scene identity comes from the LIVE VVBLKI vector ($0222/$0223) the genuine flow
// installs per scene — NOT the raw DLI byte $0200.  game_main_loop loops over
// boot_standby_launch_driver; while boot_standby_launch_driver BUILDS the Standby display it sets $0200 to many
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
#ifdef ROF_FIXED_RNG
    // Benchmark determinism (make FIXED_RNG=1) — see rof_native.c intro_random_setup.  Re-pin
    // the POKEY LFSR on the flight RISING EDGE so the in-flight terrain sequence
    // (random_terrain_height / terrain_jitter_column) starts from the same place in every
    // build, not just the level maps.  Deliberately a different constant from the level seed.
    {
        static bool s_wasFlight = false;
        if (flightVbi && !s_wasFlight) rof_lfsr_state = 0x0ACE1u;
        s_wasFlight = flightVbi;
    }
#endif
    // Title Screen (attract/level-select/results): runs under the $53CC in-game VBI, which
    // ALSO covers early-boot transitional frames — so additionally require the title text to
    // be present in its screen RAM ($365B holds 'R' of "RESCUE" = internal $32 | COLPF1<<6 =
    // $72).  display_list_init ($5D29) builds it there; standby/flight don't use $365B.
    extern volatile unsigned char g_forceTitleScreen;   // ROF_FORCE_TITLE visual-test override
    rsTitle    = ((vvblki == 0x53CCu) && (mem[0x365B] == 0x72u)) || g_forceTitleScreen;
    rsStars    = standbyVbi && (mem[0x060B] == 0x23u) && (mem[0x0200] == 0xC2u);
    rsViewport = rsStars || rsFlight;
    rsEnergyIndicator    = (mem[0x060B] != 0);

    // Boost / return-to-mother-ship reverse cinematic (BOOSTERS, B key).  When the level quota
    // is met the player docks with the mother ship: the boosters handler ($493D) sets the
    // mother-ship-arrived flag level_clear_gate($003A) to $FF, then the ship ascends (flight VBI)
    // and the game plays a REVERSE launch cinematic — stars → reverse tunnel rings → next-level
    // Standby — all under the launch-cockpit VBI $52D7 + $6CAD mode-D DLI ($0200==$AD).  The
    // FORWARD-launch gates never match this path ($0200 is $AD not $C2; the door/ring flags
    // $008A/$0088 stay 0; the reverse ring uses $008D instead), so without a dedicated gate the
    // wrong Standby door copper is shown over a stale/mispositioned field (the confirmed bug).
    // GATE on mission_event_flag($003A)==$FF (set only when the mother ship arrives, held through
    // the whole cinematic) — NOT on flight_mode_state($0072)==2, which is the crash/landing/
    // level-clear MODE (NOT a lives count) that also matches the forward launch and mis-fires there.
    // Sub-phase by the reverse-ring flags (measured live, FORCE_RETURN):
    //   stars   : $008D==0 && $008E==0   reverse ring not started — starfield in the $1000 field
    //   tunnel  : $008D!=0               reverse ring active      — concentric rings in $1000
    //   standby : $008D==0 && $008E!=0   ring done                — green LEVEL-NN door field ($2000)
    // Stars+tunnel share the launch-cockpit mode-D VIEWPORT bitmap (decoded from $1000, per the
    // user's faithful-to-$6CAD decision — NOT the forward $6CC2 PMG starfield); the final standby
    // falls through to the normal rsStandby path.
    rsBoostReturn   = standbyVbi && (mem[0x003A] == 0xFFu);
    rsBoostViewport = rsBoostReturn && (mem[0x008D] != 0u || mem[0x008E] == 0u);
#ifdef ROF_FLIGHT_PROBE
    { extern volatile unsigned char g_boostRet, g_boostVp;
      g_boostRet = rsBoostReturn ? 1 : 0; g_boostVp = rsBoostViewport ? 1 : 0; }
#endif

    // launched = doors scroll armed / ring armed / viewport active.  Safe to derive
    // now that the transpiled boot_standby_launch_driver drives: it arms the ring before the next
    // platform_render_frame, so no frame renders in the doors-fully-open gap where this
    // would briefly read false (the artifact that kept this as a C++ bool through C4).
    rsLaunched = (mem[MEM_terrain_scroll_counter] != 0) || (mem[MEM_vbi_flags] != 0) || rsViewport;

    // Door-fully-open latch.  When the door scroll completes ($008A counts $2B->0) there
    // is a multi-frame gap before the tunnel-ring dispatcher arms ($0088 set) / the
    // viewport takes over.  In that gap all of $008A/$0088/rsViewport are 0, so rsLaunched
    // would read false and the terrain region collapses back to the full closed-door
    // bitmap (the doors visibly "snap shut" for a beat, until ring cycling starts).  Latch
    // it: once the door has fully opened, keep rsLaunched true (so g2 stays at the full gap
    // and the tunnel bitmap is shown) until the cinematic leaves launch or a viewport scene
    // begins.  (The old comment here wrongly assumed the ring arms before the next frame.)
    {
        const uint8_t scroll = mem[MEM_terrain_scroll_counter];
        if (mem[0x060B] != 0x23u || rsViewport) doorsOpenedLatch = false;  // left launch / viewport took over
        else if (prevScrollCtr != 0u && scroll == 0u) doorsOpenedLatch = true;  // door just reached fully open
        prevScrollCtr = scroll;
        if (doorsOpenedLatch) rsLaunched = true;
    }

    // Re-arm the one-time Standby door capture (render() does it once when the doors
    // are built, gated on $00E7!=0) whenever the scene is NOT a settled Standby —
    // music off (building / not yet there), launched, or a viewport scene.  So each
    // fresh entry into Standby re-decodes the doors exactly once and then idles.
    // (The level-select round-trip's re-decode is armed by resetting g_doorFieldReady
    // to 0 at boot_standby_launch_driver entry — see rof_native.c — so this ==0 clause fires and the
    // g_doorFieldReady 0→1 edge at L_6118, set right AFTER the door blits, decodes the
    // freshly-written $2000 with the new level.  A scene-based re-arm here fired too
    // early: rsTitle drops when the VBI vector flips, BEFORE L_6118 redraws $2000.)
    if (g_doorFieldReady == 0u || rsLaunched || rsViewport) terrainDirty = true;

    // Force a one-time full title + cockpit repaint when the transpiled boot_standby_launch_driver (NOT a
    // hooked writer) builds the cockpit: while the scene is transitional (boot/building), and
    // ONCE on entry to the stars/planet viewport or to flight.  The cockpit is otherwise
    // WRITER-DRIVEN (the g_ck* span registry) — in flight the instrument writers register the
    // exact cells they change, so re-scanning ~580 cells EVERY frame is gone (it was the #1
    // flight cost).  Title still uses g_titleToRender via the $782A copy hook.
    // Title just repaints its 20 cells, so forcing it every transitional frame is fine.
    if (g_doorFieldReady == 0u || (rsStars && !prevRsStars) || (rsFlight && !prevRsFlight))
        g_titleToRender = 20;
    // Flight ENTRY: invalidate both terrain buffers' plane3 so renderFlightDirect clears it once
    // per buffer (see s_p3Clean there — the per-frame plane3 clear is gone).  Needed because
    // terrainBitmap is SHARED with the doors/tunnel/planet viewport decode, so it arrives here
    // holding that scene's bits in plane3.  Runs before render() -> renderFlightDirect in the same
    // frame, so the very first painted flight frame is already covered.
    if (rsFlight && !prevRsFlight) { s_p3Clean[0] = false; s_p3Clean[1] = false; }
    // The cockpit full repaint (decodeCockpitFull = 560 cells) is EXPENSIVE (~300ms even after
    // the decode LUT) and must run only when the static dashboard is actually (re)built.  That
    // happens exactly ONCE: the transpiled boot_standby_launch_driver builds it during the standby
    // construction, latched by the g_doorFieldReady 0->nonzero edge below.  There is a SINGLE
    // persistent cockpitBitmap shared by every copper list (never cleared on a scene switch), and
    // standby_scoreboard_render ($587B) is an input handler that writes NO cockpit cells — so the decoded
    // dashboard survives Standby->Doors->Tunnel->Stars->Flight unchanged, with dial/digit deltas
    // caught incrementally by the g_ck* writer registry.  So the old stars-/flight-ENTRY repaints
    // were redundant; they cost a ~580ms tunnel->stars freeze and a ~700ms flight-entry freeze.
    // Repaint on the build edge only.
    if (g_doorFieldReady != 0u && prevDoorFieldReady == 0u)
        cockpitForceFull = true;
    prevRsStars         = rsStars;
    prevRsFlight        = rsFlight;
    prevDoorFieldReady  = g_doorFieldReady;
}

// perFrameWork(): per-frame non-phase work (the tail of the old update()).  These
// ran every frame regardless of cinematic phase, driven by the standby/flight VBI
// body + the main loop on the Atari; here they run once per renderFrame.
void RescueOnFractalus::perFrameWork()
{
    update_indicator_blink_native();    // $4131: cockpit blink lights (flight-VBI routine)
    // sfx_voice_tick() is driven by CIA-B Timer A at 25 Hz (main.cpp).

    // $62E7 SFX-reinit gate: when $0090 is non-zero the loop reinits the SFX sequence.
    if (mem[MEM_sfx_reinit_gate]) {
        mem[0x073Au] = 0u;    // immediate underflow → next CIA tick loads note[0]
        mem[0x073Cu] = 0xFFu; // sequence ptr before index 0
        mem[MEM_sfx_reinit_gate] = 0u;    // clear flag (as $70E7 does via STX $0090)
    }

    // Title text ("RESCUE ON FRACTALUS!" / copyright): the genuine standby loop
    // ($62FB) drives it — copy_title_text_block_to_screen ($782A) copies the block
    // the SFX sequencer selects (via $0091) into screen RAM $32B7 every frame.  We
    // don't re-copy it here; render() picks up the change by shadow-comparing $32B7.

    if (mem[MEM_joystick_saved] != 0)            // $004A set when the game starts
        startup_init_native();          // $3FFA: cockpit digit update

    if (rsEnergyIndicator) buildEnergyIndicatorSprite();
    // Canopy posts: constant graphic, decoded once from the real RLE source tables — shown
    // in every screen (independent of the live $0C32/$0D32 buffers, which only hold the
    // frame at gameplay init and are the starfield otherwise).
    if (!postsBuilt) { buildPostSprites(); buildFlightFrameSprites(); postsBuilt = true; }
    // Starfield players $0C32/$0E32/$0F32: scrolled+seeded during stars, static
    // through the planet zoom, so map them both phases.
    // On the stars ENTRY frame the planet bitmap needs a full clear (an earlier scene may have
    // left stale pens, incl. plane3).  Kick that clear on the BLITTER now, BEFORE buildStarSprites
    // (pure CPU, ~7ms) — the blit runs in parallel with the sprite build and the shadow-zero loop,
    // so renderViewportModeD only has to blitterWait() for it (≈free) instead of stalling the CPU
    // ~7ms on it.  viewportForceFull is still set here (renderViewportModeD consumes it later).
    if (rsStars && viewportForceFull && viewportBitmap && !viewportClearKicked) {
        AmigaHardware::blitterClear((uint16_t*)viewportBitmap->data, 60, 47, 0);
        viewportClearKicked = true;
    }
    // Starfield zero-copy scroll — the pointer half runs HERE (during the frame's render pass),
    // the pixel half in the VBI.  Advancing the ring window means re-pointing the copper SPRxPT
    // operand; done here it is read cleanly at the NEXT frame's top (no race with the sprite's
    // early control DMA fetch).  The VBI (starVblankUpdate) then writes the control words + converts
    // the new rows for that same window at vblank (after this frame's display, before the fetch) —
    // so pointer and pixels land in lockstep.  starVbiRows hands the new-row count to the VBI.
    if (rsStars) {
        extern volatile unsigned short g_starScrollGen;
        if (!starSpritesValid) {
            buildStarSprites();                         // full build at window 0 (one transient frame)
            starLastGen = g_starScrollGen;
            starVbiRows = 0;
            starSpritesValid = true;
        } else {
            unsigned short gen = g_starScrollGen;
            int N = (int)(unsigned short)(gen - starLastGen);   // rows scrolled since last frame
            if (N > 0) {
                starLastGen = gen;
                if (starWindow + N > kStarMaxScroll)            // clamp: freeze rather than overrun
                    N = (starWindow >= kStarMaxScroll) ? 0 : (kStarMaxScroll - starWindow);
                starWindow += N;
                starVbiRows += N;   // ACCUMULATE rows pending conversion (ISR zeroes on consume) — a
                                    // 2nd perFrameWork call before the VBI must not drop pending rows.
            }
            // N==0: leave starVbiRows untouched so a zero-scroll call can't clobber a pending count.
#ifdef ROF_FLIGHT_PROBE
            { extern volatile unsigned short g_pfwStarCalls, g_pfwStarZeroAfterAdv;
              g_pfwStarCalls++;
              if (N == 0) g_pfwStarZeroAfterAdv++; }   // zero-scroll calls (no longer clobber starVbiRows)
#endif
        }
        if (planetCopper) for (int i = 0; i < 6; i++) planetCopper->setStarOperand(i, starRing[i] + 2 * starWindow);
        starPhaseActive = true;
    } else {
        starPhaseActive = false;
        starSpritesValid = false;                       // force a full rebuild on the next stars entry
    }
    // Flight altimeter bars: mirror the live P0 $0C98 (terrain-height) + M3 $0B98
    // (ship-height) strips each frame.
    // The laser shot (buildShotSprite) is NOT built here — it runs in the flight VBI (50Hz) via
    // PlatformAmiga::flightShotTick, faithful to the Atari (the shot is a VBI op), so it animates
    // at full rate even while the terrain render is much slower.
    if (rsFlight) { buildAltimeterSprite(); buildAltimeterShipSprite(); buildAHSprite(); buildScopeP3Sprite(); buildViewportP3Sprite(); buildScannerDotSprite(); }
}

// ---- cockpit helpers ---------------------------------------------------------
// Decode one 2bpp byte (modeD raw or mode4 glyph) → Amiga 2bp byte pair.
// Each byte contains 4 × 2-bit Atari pixels; each pixel expands to 2 Amiga pixels.
// Amiga colour index = {p2_bit, p1_bit} — same layout as terrain kNibbleColour.
// The mapping is a fixed function of the source byte, so it's a 256-entry LUT: the
// per-call bit loop used variable shifts (1 bit/cycle on the 68000, no barrel shifter)
// and cost ~96 ms across the 3360 calls of a full cockpit repaint (decodeCockpitFull).
static void buildDecode2bppLut()
{
    for (int src = 0; src < 256; src++) {
        uint8_t p1 = 0, p2 = 0;
        for (int i = 0; i < 4; i++) {
            uint8_t pixel = (uint8_t)((src >> (6 - i*2)) & 3u);
            uint8_t mask  = (uint8_t)(0xC0u >> (i*2));  // 0xC0, 0x30, 0x0C, 0x03
            if (pixel & 1u) p1 |= mask;   // plane1 = bit 0 of colour index
            if (pixel & 2u) p2 |= mask;   // plane2 = bit 1 of colour index
        }
        s_dec2bppP1[src] = p1;
        s_dec2bppP2[src] = p2;
    }
    s_dec2bppReady = true;
}
static inline void decode2bppByte(uint8_t src, uint8_t* p1out, uint8_t* p2out)
{
    *p1out = s_dec2bppP1[src];
    *p2out = s_dec2bppP2[src];
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

    // All callers pass a span within ONE region (mode4 $332D / modeD $350D) and ONE 48-byte DL
    // row, so region + entry are constant across the span.  Compute the destination row base
    // ONCE (a single divide) and walk the columns — the old code recomputed entry=off/48 and
    // col=off%48 PER CELL as SIGNED int, which GCC lowered to __divsi3/__modsi3 subroutine
    // CALLS on every cell (2/cell x 560 cells in a full repaint).
    if (addr >= 0x350Du) {                          // modeD raster band (2 identical scan lines)
        unsigned off   = (unsigned)(addr - 0x350Du);
        unsigned entry = rof_divu16(off, (uint16_t)kStride);
        int      col   = (int)rof_modu16(off, (uint16_t)kStride) - kCrop;
        if (entry >= 4u) return;
        uint8_t* d0 = cdest + (entry * 2) * kRowBytes;
        uint8_t* d1 = d0 + kRowBytes;
        for (uint8_t i = 0; i < nCells; i++, col++) {
            if (col < 0 || col >= 40) continue;
            uint8_t p1v, p2v; decode2bppByte(mem[(uint16_t)(addr + i)], &p1v, &p2v);
            d0[col] = p1v; d0[40 + col] = p2v; d0[80 + col] = 0;
            d1[col] = p1v; d1[40 + col] = p2v; d1[80 + col] = 0;
        }
    } else {                                        // mode4 dashboard (8 scan lines, glyph)
        unsigned off   = (unsigned)(addr - 0x332Du);
        unsigned entry = rof_divu16(off, (uint16_t)kStride);
        int      col   = (int)rof_modu16(off, (uint16_t)kStride) - kCrop;
        if (entry >= 10u) return;
        uint8_t* base = cdest + (8 + entry * 8) * kRowBytes;
        for (uint8_t i = 0; i < nCells; i++, col++) {
            if (col < 0 || col >= 40) continue;
            uint8_t ch = mem[(uint16_t)(addr + i)];
            uint8_t plane3 = (ch & 0x80u) ? 0xFFu : 0x00u;
            const uint8_t* glyph = (const uint8_t*)mem + 0x3800u + (uint16_t)(ch & 0x7Fu) * 8u;
            uint8_t* p = base + col;
            for (int scan = 0; scan < 8; scan++, p += kRowBytes) {
                uint8_t p1v, p2v; decode2bppByte(*glyph++, &p1v, &p2v);
                p[0] = p1v; p[40] = p2v; p[80] = plane3;
            }
        }
    }
}

// LR Scanner (#13) close-range blink: $33DF/$33E0 are two mode-4 cells whose bit7 startup_init()
// ($3FFA, in the flight VBI) toggles $1E/$1D<->$9E/$9D at 50Hz when the pilot range ($0642) is 1
// or 2 (bit7 swaps the pen COLPF2 $2C <-> COLPF3 $26 — a two-speed proximity blink).  Called from
// the VBI ISR (PlatformAmiga::flightScannerTick) so the decode runs at 50Hz, NOT the throttled
// main-loop render() rate.  Decode only when the value flips (cheap: 2 cells x 8 scanlines into the
// static cockpit bitmap; disjoint bytes from any main-loop decode, so ISR-safe).  Reads mem[$33DF]
// after startup_init() has written this frame's value (flightScannerTick runs after the handler).
void RescueOnFractalus::decodeScannerBlinkCells()
{
    static uint8_t last = 0xFFu;
    uint8_t v = mem[0x33DFu];
    if (v == last) return;
    last = v;
    decodeCockpitSpan(0x33DFu, 2u);
}

// Decode the whole cockpit region once (scene-entry repaint / registry overflow): all 4
// modeD rows + 10 mode4 rows.  The transpiled boot_standby_launch_driver (not a hooked writer) builds
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
        uint8_t* row = tbmp + kRow80[kCompassRow];   // walked +80/scanline (80 = 40 plane1 + 40 plane2)
        for (int s = 0; s < 8; s++, row += 80) {
            uint8_t p1v, p2v;
            decode2bppByte(glyph[s], &p1v, &p2v);
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
// screen and pokes color01-04 from COLPF0-3 each frame (the palette cycle).  The whole
// screen is decoded once on entry; thereafter only the changed value cells are redrawn
// (writer-driven dirty range g_titleCellLo/Hi — see decodeTitleCells / rof_title_screen_dirty).

// Title Screen text geometry, shared by the full decode and the per-cell update.
namespace {
    constexpr uint16_t kTitleCharset   = 0x0400;
    constexpr int      kTitleStride    = 120;   // 3bp interleaved: 40 p1 + 40 p2 + 40 p3
    // Per-row (of the 6 x 20 screen) starting bitmap-y and vertical doubling.  A cell's
    // bitmap-y = kTitleRowY[row] and it occupies 8*vdup scanlines.  (Row 0 is the mode-7
    // double-height banner; rows 1-5 are single-height mode-6 lines.)
    constexpr short kTitleRowY[6]    = { 56, 96, 136, 146, 170, 180 };
    constexpr uint8_t kTitleRowVdup[6] = { 2, 1, 1, 1, 1, 1 };
}

// Decode Title Screen cells [cellLo..cellHi] (flat 0..119 = row*20+col) from screen RAM
// ($365B, charset $0400) into titleScreenBitmap.  Each cell clears its own 2-byte-wide
// column across its scanline range before OR-ing the glyph in, so a changed character
// fully replaces the old one.  The full decode passes (0,119) after a whole-bitmap clear;
// the writer-driven update (only the STARTING LEVEL digit / score cells ever change while
// the screen is up) passes a tight range so the screen never fully repaints (no flash).
// The row/col walk advances with a running column counter — no divide/modulo — and the
// glyph bit-doubling is a precomputed table (kDoubleGlyph).
void RescueOnFractalus::decodeTitleCells(int cellLo, int cellHi)
{
    if (!titleScreenBitmap) return;
    uint8_t* bmp = (uint8_t*)titleScreenBitmap->data;
    const uint8_t* src = (const uint8_t*)mem + 0x365B + cellLo;
    // Walk to the starting (row,col) with subtract-compares — no div/mod.
    int r = 0;
    while (cellLo >= 20) { cellLo -= 20; r++; }
    int c = cellLo;
    for (int cell = (r * 20 + c); cell <= cellHi; cell++) {
        const uint8_t byte = *src++;
        const uint8_t pen   = (uint8_t)((byte >> 6) + 1);            // COLPF0-3 -> pen1-4
        const uint8_t* glyph = (const uint8_t*)mem + kTitleCharset + (byte & 0x3Fu) * 8u;
        const int   bx   = c * 2;                                    // 16px char = 2 bytes/plane
        const int   y0   = kTitleRowY[r];
        const int   vdup = kTitleRowVdup[r];
        uint8_t* rowp = bmp + y0 * kTitleStride + bx;                // top-left of this cell's column
        for (int gr = 0; gr < 8; gr++) {
            const uint16_t dbl = kDoubleGlyph[glyph[gr]];            // bit-double table
            const uint8_t hi = (uint8_t)(dbl >> 8), lo = (uint8_t)(dbl & 0xFF);
            for (int vd = 0; vd < vdup; vd++) {
                // Clear this cell's 2-byte column in all 3 planes, then OR the glyph in.
                rowp[0]  = rowp[1]  = 0;
                rowp[40] = rowp[41] = 0;
                rowp[80] = rowp[81] = 0;
                if (pen & 1u) { rowp[0]  |= hi; rowp[1]  |= lo; }    // plane1
                if (pen & 2u) { rowp[40] |= hi; rowp[41] |= lo; }    // plane2
                if (pen & 4u) { rowp[80] |= hi; rowp[81] |= lo; }    // plane3
                rowp += kTitleStride;                                // next scanline
            }
        }
        if (++c == 20) { c = 0; r++; }                               // next cell, no div/mod
    }
}

void RescueOnFractalus::decodeTitleScreen()
{
    if (!titleScreenBitmap) return;
    uint8_t* bmp = (uint8_t*)titleScreenBitmap->data;
    // Blank = pen 0 (black): clear the whole bitmap once, then decode every cell.
    for (int i = 0; i < kTitleStride * (int)kH; i++) bmp[i] = 0;
    decodeTitleCells(0, 119);
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
            // Skip the terrain convert once the death cinematic has blanked ANTIC DMA
            // ($4F76 DMACTL=0 → g_flightBlank): renderFrame shows the solid COLBK fade
            // instead, so re-converting the frozen terrain field would only fight it.
            if (!g_flightBlank) renderFlightDirect();
#ifdef ROF_FLIGHT_PROBE
            g_fDirect += (rof_subclock() - _dv0) - (g_isrBeamLines - _dvi);
#endif
            g_flightProf.render += (unsigned short)(flight_vbi_tick() - r0);
        }
        else {
#ifdef ROF_FLIGHT_PROBE
            if (g_starEntryVbi == 0) {
                g_starEntryVbi = (unsigned short)(rof_subclock()/313u);
                unsigned long _s0 = rof_subclock(), _si = g_isrBeamLines;
                renderViewportModeD(0x1000, 48, 47);
                g_starEntryTicks = rof_subclock() - _s0;
                g_starEntryIsr   = g_isrBeamLines - _si;
            } else
#endif
            renderViewportModeD(0x1000, 48, 47);   // stars/planet: +4 band rows ($1810-$18A0)
        }
    } else if (terrainDirty && g_doorFieldReady != 0u && !rsLaunched) {
        // Standby doors: decode the GTIA mode-10 door field at $2000 to the bitplanes
        // ONCE, then leave it.  The genuine boot_standby_launch_driver builds $2000 AFTER
        // initialize() ran (so a capture at init grabbed the empty pristine RAM — the
        // garbled-doors bug).  g_doorFieldReady (latched in boot_standby_launch_driver right after
        // blit_message_block/blit_numeric_readout draw the doors into $2000, BEFORE the
        // green fade delay_loop_c2_to_c9) is that "$2000 is built" signal — decode once
        // here, BEFORE the fade, so the live color03 (= mem[$0071]) ramp animates the
        // dark->bright green build on the real door pixels (was gated on $00E7 = the
        // music/build-END gate, AFTER the fade, so the doors popped in already-green).
        // Clear terrainDirty and do no per-frame work on the static Standby.
        // deriveRenderSignals re-arms terrainDirty when the scene leaves Standby, so
        // re-entering it re-captures the doors once.
        terrainDirty = false;
#ifdef ROF_FLIGHT_PROBE
        { extern volatile unsigned char g_doorDecodeCount; extern volatile unsigned short g_doorDecodeVbi;
          g_doorDecodeCount++; g_doorDecodeVbi = (unsigned short)(rof_subclock() / 313u); }
#endif
        // GTIA mode-10 nibble field → 3bp interleaved bitplanes via the precomputed
        // kDoorP1/kDoorP2 tables (one lookup per byte, no per-byte nibble math).  Read the
        // source through a non-volatile pointer — boot_standby_launch_driver has finished writing $2000 by
        // now ($00E7 is set), so the volatile per-byte reloads the old loop forced were pure
        // overhead.  plane3 is always 0 for the doors, so clear it once per row by longs.
        // The plane bytes go to CHIP RAM (DMA-contended), so throughput is dominated by the
        // number of stores, not the arithmetic.  Pack 4 source bytes into one 32-bit store
        // per plane (10 longs/plane/row instead of 40 byte writes) and use *p++ post-increment
        // (the 68000's (An)+ mode).  vdest is chip-aligned; +40/+80 keep each plane long-
        // aligned.  Big-endian packing so plane[4k+n] = kDoorPx[src[4k+n]].  plane3 = 0.
        // Decode the whole $2000 GTIA mode-10 door field into the TALL doorScrollBitmap that
        // standbyCopper's terrain region points at (plus the green-door pad below, so the level-
        // select scroll offset never reads past the bitmap).  This is the authoritative decode for
        // the current field, so clear the scroll dirty flag it may have raised.
        decodeDoorScrollField();
        g_doorScrollFieldDirty = 0;
        // The stars/planet renderer decodes $1000 into viewportBitmap via renderViewportModeD; the
        // door field no longer touches viewportBitmap, but a re-launch after the BOOSTERS return
        // must still force a full planet clear (the door "LEVEL NN"/band remnants used to bleed
        // through the sparse starfield — bug 3).  Stamp viewportLastBase to $2000 so the NEXT
        // renderViewportModeD($1000) sees a base mismatch and blitter-clears first.
        viewportLastBase = 0x2000u;
    }

    // ---- title region -------------------------------------------------------
    // Count-driven: repaint g_titleToRender cells straight from screen RAM (no shadow / compare).
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

    // The title region is idle unless the genuine $782A writer (copy_title_text_block_to_
    // screen) rewrote $32B7-$32CA — it sets g_titleToRender through the platform_title_
    // changed() hook on each copy — or a full repaint is forced.  A forced full repaint paints
    // all 20 cells.  68000 muls (~70cy) are kept out of the per-cell path: the row pointer is
    // pre-offset once (kTitleTextRow*80 before the loop) and `*8`/`*2` are shifts.
    if (g_titleToRender >= 0 || cockpitForceFull) {
    const int want = cockpitForceFull ? 20 : g_titleToRender;
    uint8_t* tbmp = (uint8_t*)titleBitmap->data;
    uint8_t* const titleBase = tbmp + kTitleTextRow * 80;     // first text scanline row (once)
    const uint8_t* tsrc    = (const uint8_t*)mem + kScreenRAM;  // non-volatile walk (RAM static this frame)
    for (int col = 0; col < want; col++) {
        uint8_t charByte = tsrc[col];

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
    // Shrinking the count blanks the cells that were painted last time but are no longer wanted.
    for (int col = want; col < titleRendered; col++) {
        uint8_t* row = titleBase + col * 2;
        for (int scanline = 0; scanline < 8; scanline++, row += 80) {
            if (kTitleTextRow + scanline >= (int)kTitleHeight) break;
            row[0] = row[1] = row[40] = row[41] = 0;
        }
    }
    titleRendered = want;
    g_titleToRender = -1;
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
    // boot_standby_launch_driver built it — not a hooked writer); steady state is span-only, ~0 cost on
    // frames where nothing moved.  Layout note: the cockpit shares the terrain WIDE playfield
    // (48 bytes/DL row) — modeD $350D (4 entries × 2 scan lines) then mode4 $332D (10 × 8),
    // 40 visible cols of each 48-byte row (+4 crop) — all handled by decodeCockpitSpan().
#ifdef ROF_FLIGHT_PROBE
    unsigned long _ckp0 = rof_subclock();
#endif
    if (cockpitForceFull) {
        cockpitForceFull = false;
#ifdef ROF_FLIGHT_PROBE
        unsigned long _ckf0 = rof_subclock();
#endif
        decodeCockpitFull();
#ifdef ROF_FLIGHT_PROBE
        g_ckFullTicks = rof_subclock() - _ckf0;
        if (g_ckFullCount < 4) g_ckFullVbi[g_ckFullCount] = (unsigned short)(rof_subclock()/313u);
        g_ckFullCount++;
#endif
        // The full paint covers every cell — drop all instrument flags + the dial cell flags.
        g_ckDigits = g_ckLockon = g_ckDial = 0u;
        for (int i = 0; i < CK_DIAL_N; i++) g_ckDialFlag[i] = 0u;
#ifdef ROF_FLIGHT_PROBE
        if (rsFlight) g_fCockpitScans++;
#endif
    } else {
        bool any = false;
        // Digits (#17-19) + DL-stride: 5 two-tall 2×2 blocks + the $33DF/$33E0 stride pair.
        // Decoding all five whenever any digit changes is ~22 cells and digits change rarely.
        if (g_ckDigits) {
            g_ckDigits = 0u;
            static const uint16_t kDigit[5] = { 0x33B4u, 0x3413u, 0x3445u, 0x3472u, 0x34A4u };
            for (int i = 0; i < 5; i++) {
                decodeCockpitSpan(kDigit[i], 2u);                 // top row
                decodeCockpitSpan((uint16_t)(kDigit[i] + 0x30u), 2u);  // bottom row (one DL row down)
            }
            decodeCockpitSpan(0x33DFu, 2u);                       // DL-stride control bytes
            any = true;
        }
        // Lock-on indicator (#11): the 7 cells $3491-$3497.
        if (g_ckLockon) {
            g_ckLockon = 0u;
            decodeCockpitSpan(0x3491u, 7u);
            any = true;
        }
        // Thrust (#4) / Dangerous-Altitude (#5) dial bars: per-cell, walked only now (dial moved).
        // Long-batched skip so the all-clear runs between bar cells are cheap.
        if (g_ckDial) {
            g_ckDial = 0u;
            const unsigned long* fl = (const unsigned long*)(const void*)g_ckDialFlag;
            for (int i = 0; i < CK_DIAL_N / 4; i++) {
                if (fl[i] == 0u) continue;
                int base = i * 4;
                for (int b = 0; b < 4; b++) {
                    if (g_ckDialFlag[base + b]) {
                        g_ckDialFlag[base + b] = 0u;
                        decodeCockpitSpan((uint16_t)(0x332Du + base + b), 1u);
                    }
                }
            }
            any = true;
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
    delete terrainDotBuffer; terrainDotBuffer = nullptr;
    delete viewportBitmap;  viewportBitmap  = nullptr;
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
    for (int c = 0; c < 6; c++) { delete starSprite[c]; starSprite[c] = nullptr; }
}
