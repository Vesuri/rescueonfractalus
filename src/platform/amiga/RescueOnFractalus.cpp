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
#include "../../rof_boot.h"       // staged INITAD boot chain (Logo / Station) + g_bootScene
#include "../../gen/rof_manual.h" // g_stationDirty — the station image's dirty rectangles
#include "PlatformAmiga.h"
#include "../../gen/mem.h"           // MEM_<name> named Atari memory offsets
#include "FlightProf.h"   // per-frame VBI-count profiler (g_flightProf / flight_vbi_tick)

// Native handler functions — see NativeHandlers.cpp and SfxPlayer.cpp.
extern "C" void vbi_attract_timer_native(void);                  // $52D7: timer cascade
extern "C" void update_indicator_blink_native(void);           // $4131: cockpit blink
extern "C" void startup_init_native(void);                      // $3FFA: cockpit digit update
extern "C" void launch_anim_dispatch_native(void);              // $5367: ring ($0088) vs door scroll ($008A)
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
// ...and which $2000 field rows that rewrite touched (empty range = "unknown" -> full decode).
extern "C" volatile unsigned char g_doorDirtyRow0, g_doorDirtyRow1;
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
extern "C" volatile unsigned char g_ckLockon = 0;   // lock-on indicator $3491-$3497 (any cell)
extern "C" volatile unsigned char g_ckDial   = 0;   // thrust/danger-alt dial bars (draw_object_column)
// The lock-on indicator is the ONE cockpit instrument that changes continuously in flight: its
// random-blink state ($007E == $80) flips bit7 of a SINGLE cell about 9x/second, and the strip-wide
// flag then re-decoded all 7 ($3491-$3497) — measured 2026-08-09 at 24 of the cockpit scan's 39
// t/iteration, ~1.7% of the whole flight frame, for ~1.9 cells of real work.  So the strip keeps
// per-cell flags like the dial, but over 8 bytes (7 cells + pad) so the walk is two long tests.
// Byte stores are atomic on the 68000 and each cell owns its byte, so the VBI-ISR writer cannot
// lose an update to the main loop's clear (a bitmask's |= could) — worst case is a duplicate decode.
static volatile unsigned char g_ckLockFlag[8] __attribute__((aligned(4))) = {};

// The lock-on writer (lockon_write, rof_native.c) reports each rewritten cell here via
// PlatformAmiga::lockonChanged.  cellIdx = addr - $3491, 0..6.
extern "C" void rof_cockpit_lockon_dirty(unsigned char cellIdx)
{
    if (cellIdx > 6u) return;
    g_ckLockFlag[cellIdx] = 1u;
    g_ckLockon = 1u;
}

// The five digit 2×2 blocks + the DL-stride pair, one flag each — the writer (startup_init_native)
// knows exactly which block it rewrote, and measurement says exactly ONE changes per fire, so
// decoding all 22 cells was ~5x the work.  Main-thread writer (perFrameWork) and main-thread
// consumer (render), so no ISR race here; kept as bytes to match the lock-on/dial registries.
static const int CK_DIGIT_N = 6;                      // 0-4 = the 2x2 blocks, 5 = $33DF/$33E0
static volatile unsigned char g_ckDigitFlag[8] __attribute__((aligned(4))) = {};
extern "C" void rof_cockpit_digit_dirty(unsigned char slot)
{
    if (slot >= (unsigned char)CK_DIGIT_N) return;
    g_ckDigitFlag[slot] = 1u;
    g_ckDigits = 1u;
}
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
// Per-GROUP split of the cockpit scan (g_fCockpit lumps all three together, so "something decoded
// on ~0.9 of iterations" cannot tell the 22-cell digit block from a 1-cell dial cell).  Counts =
// how often each group fired; T = ticks inside it; g_ckDialCells = dial cells actually decoded.
extern "C" volatile unsigned long g_ckDigitFires = 0, g_ckLockFires = 0, g_ckDialFires = 0;
extern "C" volatile unsigned long g_ckDigitT = 0, g_ckLockT = 0, g_ckDialT = 0, g_ckDialCells = 0;
extern "C" volatile unsigned long g_ckLockCells = 0;   // lock-on cells actually decoded (was always 7)
extern "C" volatile unsigned long g_ckDigitBlocks = 0;  // digit blocks actually decoded (was always 6)
extern "C" volatile unsigned short g_ckFullVbi[4] = {0,0,0,0};       // g_vbiCount at each ckFull call
// Boost-return probe: last-installed copper id (1=title 2=standby 3=planet 4=flight
// 5=forward tunnel 6=doors 8=boost-handoff-hold 9=black EmptyCopperList 10=in-place wrap fade
// 11=boost REVERSE tunnel) + the live boost signals, sampled per render() to confirm phase
// routing.  g_boostHandoffHoldFrames counts the T6 handoff-hold frames (proves the
// reverse-tunnel->standby window exists and the guard catches it).
extern "C" volatile unsigned char g_boostRet = 0, g_boostVp = 0, g_liveCopper = 0;
// BPLCON2 audit (PROBES only): the value each BUILT copper list MOVEs to $104, or 0xFFFF if the
// list emits none — which for Standby/Doors/Title is BY DESIGN: their priority is a one-off CPU
// write at the scene transition (setSpritePriority).  g_cl2Cpu/g_cl2CpuN are that write's last
// value and its count, so the pair says who owns the register and what it currently holds.
extern "C" volatile unsigned short g_cl2Standby = 0, g_cl2Doors = 0, g_cl2Tunnel = 0,
                                   g_cl2Planet = 0, g_cl2Flight = 0, g_cl2Title = 0;
extern "C" volatile unsigned short g_cl2Cpu = 0xFFFF;
extern "C" volatile unsigned short g_energySprY = 0;   // live gauge-sprite VSTART line
extern "C" volatile unsigned long  g_cl2CpuN = 0;
extern "C" volatile unsigned long g_boostHandoffHoldFrames = 0;
// Boost-viewport decode-cost probe (item 2, decode-consume): count decode events, to confirm the
// decode-on-change gating runs and quantify the win vs the old always-full decode EVERY frame.
// g_bStarDec = $2000 starfield decodes into viewportBitmap, and what they cost in rof_subclock
// beam-lines with the ISR's own firings subtracted (as FP_TIME does), so it is comparable to the
// rest of the budget (1 tick = 1 scanline = 63.56us).  There is no reverse-tunnel decode counter
// any more: the rings are PAINTED (drawTunnelRect), never decoded.
extern "C" volatile unsigned long g_bStarDec = 0;
extern "C" volatile unsigned long g_bStarTicks = 0;
// Boost band-corner split histogram: how many frames of each sub-phase derived each greenLine
// (0 = whole band the door colour / wedge still full, 8 = whole band the carried ring colour).
extern "C" volatile unsigned long g_bwLine[9] = {0}, g_bwLineStars[9] = {0};
#ifdef ROF_FLIGHT_PROBE
// ROF_TUNNEL_RECT plumbing probe: g_trCalls = ring rectangles handed to the direct painter,
// g_trDoors = rectangles that belonged to the $2000 door field (skipped).  The first 8 ring
// rectangles are captured packed so their geometry can be eyeballed against the field.
extern "C" volatile unsigned long g_trCalls = 0, g_trDoors = 0,
    g_trRows[8] = {0}, g_trCols[8] = {0}, g_trPen[8] = {0};
// §0a of docs/boost-tunnel-direct-handoff.md: the previous session BUCKETED the rectangles by
// render phase and then LABELLED the biggest bucket "the forward launch's tunnel descent" without
// measuring it — the handoff calls that label the prime suspect.  So tag each rectangle with the
// call site that produced it (g_trSrc, set by rof_native.c at the draw_symmetric_span_loop call
// sites) and cross it with the phase, which is what actually decides whether the painter may
// paint.  src: 1 = draw_frame_pattern_seq @ L_6047 (43-rectangle static PRE-DRAW), 2 =
// draw_ring_frame_step (forward descent), 3 = step_accum_sub_7e (boost reverse), 4 =
// draw_frame_pattern_seq @ tunnel_prebuild_rings (the Amiga-only pre-draw).
// phase: 0 = neither flag, 1 = rsBoostReturn only, 2 = rsBoostViewport.
extern "C" volatile unsigned char g_trSrc = 0, g_trPreSite = 1;
// tunnelPaintBegin claim log: was tunnelBitmap on screen when a forward pre-draw claimed it?
extern "C" volatile unsigned char g_tpbN = 0, g_tpbTunInst[4] = {0}, g_tpbCopper[4] = {0};
extern "C" volatile unsigned short g_tpbVbi[4] = {0};
extern "C" volatile unsigned long g_trBySrc[5] = {0};       // rectangles per call site
extern "C" volatile unsigned long g_trPhase[15] = {0};      // [src*3 + phase]
// Per-RUN timeline: a run is a burst of rectangles from ONE site with no >8-vbi gap inside it.
// Totals alone cannot say WHERE on the timeline a pre-draw sits, and that placement (stars
// sub-phase vs reverse-tunnel sub-phase) is what decides whether painting it is safe.
#define TR_RUNS 16
extern "C" volatile unsigned long  g_trRunN = 0;            // runs seen (may exceed TR_RUNS)
extern "C" volatile unsigned char  g_trRunSrc[TR_RUNS] = {0}, g_trRunPhase[TR_RUNS] = {0};
// §2 fidelity differential (`make TUNDIFF=1`): decode the live $1000 field through the CURRENT
// OWNER's LUT (forward or boost) and diff it against what the direct painter actually left in
// tunnelBitmap, over the band the copper shows from it, [K, 85-K].  This
// is the check that makes the rewrite verifiable — it caught both wrong turns of the 2026-08-10
// session before either was visible on screen.  Race-aware by construction: the VBI writes the
// field while this reads it, so a small transient count is expected (~2% observed floor).
//   EXTRA   = the bitmap has ink where the field is pure background ($88) -> the painter drew
//             something the field does not have, or a stale pixel was never cleared.
//   MISSING = the field has content the painter never saw -> an unhooked field writer.
extern "C" volatile unsigned long g_dpBytes = 0, g_dpBad = 0, g_dpFirst = 0, g_dpGot = 0, g_dpWant = 0;
extern "C" volatile unsigned long g_dpExtra = 0, g_dpMissing = 0, g_dpCol[40] = {0}, g_dpFrames = 0;
extern "C" volatile unsigned long g_dpSnapDone = 0;
extern "C" volatile unsigned char g_dpSrc42[40] = {0}, g_dpB1[40] = {0};
extern "C" volatile unsigned char  g_trRun8D[TR_RUNS] = {0}, g_trRun8E[TR_RUNS] = {0};
extern "C" volatile unsigned short g_trRunVbi0[TR_RUNS] = {0}, g_trRunVbi1[TR_RUNS] = {0};
extern "C" volatile unsigned short g_trRunCnt[TR_RUNS] = {0};
// Ring paint vs the BEAM — the "multi-coloured rectangle edges on single reveal frames" report
// (2026-08-10).  A pen is three PLANES, so a pixel only carries the new colour once all three are
// written; while a paint is in flight the pixel shows a mix of the old and the new pen, i.e. a
// colour that is in NEITHER image.  That is the difference from the Atari, where a field byte is
// one store and a pixel can only ever be old-or-new.  This probe measures the two halves of the
// claim on the tall vertical edges (the worst case: one 4-px column up to 86 rows high), and only
// while the copper is actually displaying tunnelBitmap:
//   g_tbLines*  = how long one vertical-edge paint lasts, in raster lines
//   g_tbBeamIn  = paints during which the beam was inside the rows being painted (= visible tear)
extern "C" volatile unsigned long  g_tbCalls = 0, g_tbBeamIn = 0, g_tbLinesSum = 0;
extern "C" volatile unsigned short g_tbLinesMax = 0;
extern "C" volatile unsigned short g_tbEntryMin = 0xFFFF, g_tbEntryMax = 0;
#define TB_SAMP 12
extern "C" volatile unsigned short g_tbN = 0;
extern "C" volatile unsigned short g_tbIn[TB_SAMP] = {0}, g_tbOut[TB_SAMP] = {0};
extern "C" volatile unsigned short g_tbY0[TB_SAMP] = {0}, g_tbY1[TB_SAMP] = {0};
// Reverse-reveal K timeline (user report 2026-08-11: "the top and bottom of the viewport stayed
// black, the rectangles were only drawn to the vertically middle part").  K is the first viewport
// row the copper takes from tunnelBitmap; outside [K, 85-K] it shows the STARFIELD, which is black
// by this point in the cinematic.  So "black top and bottom" == the reveal never reached K = 0.
extern "C" volatile unsigned char  g_rkMin = 43;         // smallest K the reveal ever reached
extern "C" volatile unsigned long  g_rkHist[44] = {0};   // boost frames spent at each K
extern "C" volatile unsigned short g_rkFirstVbi = 0, g_rkLastVbi = 0;
// ...and the TIMELINE, which is what says whether the middle-only band is a brief pass or a stall:
// one entry per CHANGE of K, with the vbi it changed at and how many frames it then held.
#define RK_STEPS 48
extern "C" volatile unsigned short g_rkN = 0;
extern "C" volatile unsigned char  g_rkK[RK_STEPS] = {0};
extern "C" volatile unsigned short g_rkVbi[RK_STEPS] = {0}, g_rkHold[RK_STEPS] = {0};
// EmptyCopperList frames taken while RETURNING to the mother ship ($52D7 + $003A=$FF) — the black
// seam between the reverse tunnel ending and the standby appearing.  Must be 0: render()'s T6
// handoff hold is supposed to own that window, freezing the last reverse-ring frame on screen.
extern "C" volatile unsigned long  g_brBlackFrames = 0;
extern "C" volatile unsigned short g_brBlackFirstVbi = 0, g_brBlackLastVbi = 0;
extern "C" volatile unsigned char  g_brBlack8D = 0, g_brBlack8E = 0;   // the gating flags at entry
extern "C" volatile unsigned char  g_brBlackDoorRdy = 0, g_brBlackTunInst = 0;
#endif
extern "C" volatile unsigned short g_starEntryVbi = 0;              // vbi at first rsStars viewport decode
extern "C" volatile unsigned long  g_starEntryTicks = 0, g_starEntryIsr = 0; // its cost
extern "C" volatile unsigned short g_starSprVbi = 0;
extern "C" volatile unsigned long  g_starSprTicks = 0;             // first buildStarSprites cost
extern "C" volatile unsigned long  g_starGroups = 0;              // non-skipped groups on the entry decode
extern "C" volatile unsigned long  g_starClrTicks = 0;
// ── tunnel->stars transition: WHERE do the frozen frames go? ───────────────────
// The last forward-tunnel frame (outermost ring only) is held on screen for the whole of the
// FIRST rsStars renderFrame, because the planet copper is not installed until that frame's tail.
// Stamp every stage of that one frame (beam ticks; 313 = one 50 Hz frame) so the hold is
// attributed to a stage instead of guessed at.
extern "C" volatile unsigned short g_seEntryVbi = 0;    // g_vbiCount at renderFrame entry
extern "C" volatile unsigned long  g_seDrs     = 0;     // deriveRenderSignals
extern "C" volatile unsigned long  g_sePfw     = 0;     // perFrameWork (whole)
extern "C" volatile unsigned long  g_seSpr     = 0;     //   ...of which buildStarSprites
extern "C" volatile unsigned long  g_seRender  = 0;     // render() (whole; incl. the viewport decode)
extern "C" volatile unsigned long  g_seTail    = 0;     // the copper-install tail (to the planet install)
// The number the user actually sees: how many frames the LAST forward-tunnel image is displayed.
// The tunnel copper goes live one vblank after its install, the planet copper one vblank after
// ITS install, so the ring image is on screen for exactly (planet install vbi - last tunnel
// install vbi) frames.  1 = the Atari's behaviour.
extern "C" volatile unsigned short g_tunLastVbi = 0, g_planetInstVbi = 0;
extern "C" volatile unsigned long  g_seSprKick = 0, g_seSprConv = 0, g_seSprDrain = 0;  // buildStarSprites split
extern "C" volatile unsigned long  g_seWall    = 0;     // whole frame, entry -> planet copper installed
extern "C" volatile unsigned char  g_seArmed   = 0;     // 1 while that first stars frame is in flight
// starVblankUpdate beam-deadline probes.  The star update has three of them, all policed here
// (see the starVblankUpdate header comment for why each one exists):
//   entry / pub  — the copper executes the copper list's sprite-pointer MOVEs at scanline 16, so
//                  the SPRxPT operand publish must be finished before then;
//   exit         — the control words must beat the sprite's ~line-25 control DMA fetch, else
//                  channel-2's star VSTOP reads stale and the throttle gauge drops with it.
extern "C" volatile unsigned short g_starVbiEntryLineMax = 0;                    // ISR entry beam line
extern "C" volatile unsigned short g_starPubLineMax = 0;                         // after the SPRxPT publish
extern "C" volatile unsigned short g_starPubLate = 0, g_starPubLateAtVbi = 0;    // publish at/after line 16
extern "C" volatile unsigned short g_starVbiExitLine = 0, g_starVbiExitLineAtVbi = 0;
extern "C" volatile unsigned short g_starVbiLateCount = 0, g_starVbiCalls = 0;
extern "C" volatile unsigned short g_pfwStarCalls = 0;
extern "C" unsigned short platform_frame_count(void);
extern "C" unsigned short rof_beam_line(void);
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
// SIZEP3 latched out of bus_write (src/cpu/bus.h; defined in rof_native_amiga.cpp).  The player-3
// width has no shadow in the game's RAM the way SIZEP2 does in mem[$00CD], so this is how the
// Main-Window sprite mirror learns the object is 2×/4× wide.  Declared rather than #including
// bus.h: that header pulls in cpu.h, whose `mem` declaration has a different language linkage.
extern "C" volatile uint8_t g_sizep3_shadow;
// Defined with the wide-object machinery below; used earlier by buildFlightFrameSprites.
static void mirrorSprite(Sprite* dst, const Sprite* src, int rows);
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
// 4-aligned because the paint reads them a LONG at a time (see the band overlay); every row base is
// a multiple of 40 and every group offset a multiple of 4, so the whole walk stays aligned.
static uint8_t  s_bandP1c[2][40 * 4] __attribute__((aligned(4))),
                s_bandP2c[2][40 * 4] __attribute__((aligned(4))),
                s_bandOWc[2][40 * 4] __attribute__((aligned(4)));
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
// ...and, better still, the exact byte OFFSETS.  The box narrowed the walk but did not change its
// nature: it is still a search.  Measured (objp1_shape.gdb, COMBAT+FIXED_RNG, 358 painted frames)
// the box is 447 bytes/frame of which 2.2% -- about ten -- are nonzero, and the COMBAT PC profile
// put 20/300 samples on the inner test.  But every one of those bytes is written by a writer that
// already computed its offset, so the reader never needs to search at all: each writer appends the
// offset the first time it makes a byte nonzero (0->nonzero transition, so no duplicates), and the
// apply walks that list instead of the box.  Capacity is generous next to the ~10/frame measured;
// on overflow the flag makes the apply fall back to the box walk, which is still correct because
// the writers keep maintaining the box too.  g_objLeak (BAND_VERIFY) is the invariant check: after
// the apply NO nonzero byte may remain anywhere in the scratch.
#define ROF_OBJ_TOUCH_CAP 256
extern "C" uint16_t g_objTouch[ROF_OBJ_TOUCH_CAP];
extern "C" int g_objTouchN = 0;      // entries in use
extern "C" int g_objTouchOvf = 0;    // list overflowed -> apply must use the box walk
uint16_t g_objTouch[ROF_OBJ_TOUCH_CAP];

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
    // $FF (off-top: the column is all terrain body, so it plots NOTHING) gets the same negative
    // SENTINEL treatment as kDrawDotRowOff above, so the asm rejects it with the `bmi` that the
    // table read's own flags already set instead of a per-column `cmp.b #$FF / beq` — 16 cycles
    // on every one of the 160 columns, and `make EDGE_SHAPE=1` + amiga/edge_shape.gdb measured
    // $FF at only 3% of columns (5622/164160), so the trade is 8 cycles saved on 97% against 14
    // paid on 3%.  Real offsets are kRow120[0..46] = 0..5520, so they never collide with it.
    // ⚠ Both C readers (edgePlotCore, edgeShapeProbe) test h != $FF FIRST and so never index
    // this entry — which is what keeps edgePlotCore usable as the differential's oracle.
    kHeightRowOff[0xFF] = 0xFFFF;
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
// BOOST reverse-tunnel variant of the GTIA-10 LUT (the boost cinematic only): value-2 (outermost
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
//   GTIA mode-9 (the boot scenes: Logo $60A3 and Station $0600 — ANTIC mode F under
//   PRIOR bits 7:6 = 01): byte = 2 nibbles, each nibble a 4-bit LUMINANCE of the COLBK hue
//   covering 4 lores px.  Unlike the mode-10 tables above there is no colour remap at all —
//   nibble n IS pen n — so it needs four planes, and the palette carries the whole mapping
//   (StationCopperList's kGtia9Pal0).  One source byte → one byte in each plane.
static uint8_t kGtia9P[4][256];

// ---- boot scene 2 (the station cinematic) — the Atari addresses its decode walks -------------
// The display list display_list_build ($1C40) builds at $B800: 340 three-byte mode-F entries —
// 122 image rows (LMS $0600 + 40n) then 218 star/blank rows (LMS $2CB8 + 40k, or the shared
// blank row $2C90 — 1 in 8 by RANDOM, capped at 30 by encounter_count = $1E).
static const uint16_t kStationDL      = 0xB800u;
static const uint16_t kStationDLRows  = 340u;
static const uint16_t kStationImg     = 0x0600u;   // the 122-row image, stride 40
static const uint16_t kStationImgEnd  = (uint16_t)(kStationImg + 122u * 40u);   // $1930
// The star rows station_star_fade_in ($1E79) brightens: it seeds its pointer $90/$91 = $2CB8 and
// walks byte by byte until it reaches $3168 (its `CMP #$68` / `CMP #$31` exit).
static const uint16_t kStationStarLo  = 0x2CB8u;
static const uint16_t kStationStarHi  = 0x3168u;
// Screen geometry of each boot scene's display list: leading blank scanlines (its `$70` entries)
// and mode-F row count.  Station: one $70 + 192 displayed rows ($0240/3, the window the moving
// JVB holds).  Logo: eight $70 + all 62 of its rows.
static const uint16_t kStationTopLines = 8;
static const uint16_t kStationRows     = 192;
static const uint16_t kLogoTopLines    = 64;
static const uint16_t kLogoRows        = 62;

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
        // (flLeftTri is chained behind ch1's wide-object segment, which is double-buffered, so its
        // second copy is mirrored after this loop — it is fully static, hence once and never again.)

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
    mirrorSprite(wideLow[2][1], flLeftTri, 8);   // ch1's second chain (static: control + pixels, once)
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
// The player's laser is Atari player P2, drawn by build_player2_sprite ($8C58 — symbols.csv's
// name; earlier comments here called it "draw_player_shot", which is not a symbol) into the P2
// buffer's UPPER region (mem[$0E32+], separate from the AH ground fill at $0E87+).  $8C58 already
// fills mem[] on the Amiga every flight frame (only its $D0xx writes are ignored), and it also
// latches $00CB (HPOSP2) and the strip's extent, so we mirror it into a sprite like buildAHSprite:
//   active  = mem[$0036] != 0   (0 = no shot; 1..~$11 = travel; $81..$8a = impact burst)
//   pixels  = mem[$0E00 + O]    (8 GRAFP2 bits per row; O in $34..$91)
//   extent  = mem[$2865] (strip top, relative to $0E32) + mem[$2866] (row count) — see below
//   HPOS    = mem[$00CB]        (true HPOSP2 shadow → viewport X = 0x81 + (hpos-$32)*2)
//   colour  = mem[$0037]        (COLPM2 → COLOR27; blue $78 travel → white/orange impact)
//   size    = mem[$00CD]        (SIZEP2: 0=1×, 1=2×, 3=4× — the burst zooms as it peaks)
// The sprite lives on the otherwise-idle ch4, as SEGMENT 0 of a wide object: at 2×/4× the burst
// is 32/64 lores px and spills onto the shared extension channels (see RescueOnFractalus.h).
//
// ⚠ `size` is the real meaning of mem[$00CD], whose symbols.csv name `sizep2_shadow` is WRONG —
// $D00A is SIZEP2, not GRAFM (docs/rename.md, backlog 2026-08-10).  build_player2_sprite stores
// only $00/$01/$03 there, which is the SIZEP2 encoding, and pairs each with an HPOSP2 shift of
// 0/4/12 colour clocks ($286E) — exactly what re-centres a player 8/16/32 colour clocks wide.
//
// FIXED VSTART (kTerrainLine) with the strip at an internal row offset, like viewportP3: the old
// 32-row moving-VSTART sprite also CLIPPED the burst, which reaches ~52 rows at 4× (13 shape rows
// from $8DD0, each drawn 4× vertically).  94 rows covers the whole viewport, so nothing clips.
static const int kShotRows = kViewportFullHeight;     // 94: VSTART 86 → VSTOP 180

// Extension-segment sprite heights.  ch5/ch6 span the whole viewport (VSTOP 180, chained ahead of
// the energy bar / altimeter at VSTART 188+).  ch1 stops at VSTOP 171 so its chained band triangle
// keeps VSTART 172 — a chained VSTART must be strictly past the preceding VSTOP, so segment 3
// gives up the viewport's last scanline.  (Only reachable at 4×, where seg 3 is the rightmost 2
// source bits.)
static const int kWideExtRows    = kViewportFullHeight;      // 94: ch5, ch6
static const int kWideExtRowsCh1 = kTerrainHeight - 1;       // 85: ch1 (VSTOP 171, triangle at 172)

// User-tweakable alignment nudge for the player laser sprite (viewport space).  The shot read
// ~4px too far LEFT and ~2px too far DOWN vs the crosshair; kShotXOff moves it right (+),
// kShotYOff moves it down (+).  Tune these two numbers to land it exactly.
static const int kShotXOff = 3;    // + = right (Amiga sprite X units)
static const int kShotYOff = 0;    // + = down  (Amiga scan lines)

// Expand an 8-bit Atari player row (bit7 = leftmost) to 16 Amiga sprite px, each bit → 2 px
// (the 1× player→viewport scale, 2 Amiga lores px per Atari colour clock).  The per-call bit
// loop cost 8 variable-shift iterations (no 68000 barrel shifter); a 256-entry LUT filled once
// by buildShotExpandLut() replaces it for the per-frame shot / scope-P3 / viewport-P3 mirrors.
#ifdef ROF_SPRITE_SHAPE
// ---- Flight-VBI sprite bracket shape probe (make SPRITE_SHAPE=1 + amiga/sprite_shape.gdb) -----
// game_vbi_isr brackets buildShotSprite + decodeScannerBlinkCells TOGETHER as g_vbiSpriteLines,
// and that bracket measures 29 t/firing = ~9% of ALL wall clock (the ISR fires 50x/s regardless of
// frame rate) — with neither function ever profiled.  ~13000 cycles is far more than either looks
// like it should cost, so split it: WHICH of the two, which PATH inside buildShotSprite, and how
// the active path divides between the 94-byte no-early-exit run scan / the whole-sprite clear /
// the row decode / the copper + Sprite-header pokes.
// g_spNop is one EMPTY lap sampled per call = the lap's own floor; subtract one floor per lap or a
// cheap part reads as ~the bracket (the SFX_SHAPE lesson: use the split as SHARES, not absolutes).
extern "C" unsigned short rof_beam_line(void);
extern "C" volatile unsigned long
    g_spShotCalls = 0, g_spScanCalls = 0,                          // entries
    g_spIdle = 0, g_spBlank = 0, g_spActive = 0, g_spNoRun = 0,    // which buildShotSprite path
    g_spIdleT = 0, g_spRunT = 0, g_spClearT = 0, g_spDecT = 0, g_spCopT = 0,
    g_spScanT = 0, g_spScanDecodes = 0,
    g_spRows = 0, g_spRowsMax = 0, g_spTopSum = 0, g_spBotSum = 0,
    g_spNop = 0, g_spNopT = 0;
static unsigned short s_spB = 0;
#define SP_RESET()  (s_spB = rof_beam_line())
#define SP_LAP(acc) do { unsigned short _n = rof_beam_line(); \
    (acc) += (_n >= s_spB) ? (unsigned long)(_n - s_spB) : (unsigned long)(_n + 313 - s_spB); \
    s_spB = _n; } while (0)
#define SP_NOP()    do { ++g_spNop; SP_RESET(); SP_LAP(g_spNopT); } while (0)
#define SP_CNT(c)   (++(c))
#else
#define SP_RESET()  ((void)0)
#define SP_LAP(acc) ((void)0)
#define SP_NOP()    ((void)0)
#define SP_CNT(c)   ((void)0)
#endif

#ifdef ROF_SHOT_VERIFY
// ---- buildShotSprite extent differential (make SHOT_VERIFY=1 + amiga/shot_verify.gdb) ----------
// buildShotSprite now reads the P2 strip's extent from mem[$2865]/mem[$2866] (written by
// build_player2_sprite $8C58) instead of re-deriving it with a 94-byte scan.  That is an
// Amiga-only render mirror, so `make validate` cannot check it — this runs the old scan alongside
// and byte-compares the two.  g_svBad must stay 0 over a long flight.
extern "C" volatile unsigned long
    g_svCalls = 0, g_svBad = 0, g_svBenign = 0,
    g_svLastTop = 0, g_svLastRows = 0, g_svOraTop = 0, g_svOraRows = 0, g_svLastLit = 0,
    g_svLast36 = 0, g_svLast2865 = 0, g_svLast2866 = 0;
#endif

static uint16_t s_shotExpand[256];
static bool     s_shotExpandReady = false;
// 2× expansion: one NIBBLE of the source byte fills a whole 16px segment, each bit → 4 px.
static uint16_t s_wideExpand4[16];
// 4× expansion: one BIT PAIR fills a whole 16px segment, each bit → 8 px.  Small enough to be a
// constant — no build step, and the index (b >> 6/4/2/0) & 3 needs only immediate shifts.
static const uint16_t kWideExpand8[4] = { 0x0000u, 0x00FFu, 0xFF00u, 0xFFFFu };
static void buildShotExpandLut()
{
    for (int b = 0; b < 256; b++) {
        uint16_t v = 0;
        for (int k = 0; k < 8; k++)
            if (b & (uint8_t)(0x80u >> k)) v |= (uint16_t)(3u << (14 - 2 * k));
        s_shotExpand[b] = v;
    }
    for (int n = 0; n < 16; n++) {
        uint16_t v = 0;
        for (int k = 0; k < 4; k++)
            if (n & (uint8_t)(0x8u >> k)) v |= (uint16_t)(0xFu << (12 - 4 * k));
        s_wideExpand4[n] = v;
    }
    s_shotExpandReady = true;
}
static inline uint16_t expandShotRow(uint8_t b) { return s_shotExpand[b]; }

// GTIA SIZEPn ($D008-$D00B) → the Amiga horizontal scale.  Only bits 1-0 reach the hardware, and
// value 2 is a SECOND encoding of "normal" — so mask exactly as GTIA does rather than comparing
// the shadow byte to 0/1/3.  That also makes a stale shadow harmless: mem[$00CD] read $62 in
// a800dumps/saucerbigpause.a8s (no shot active, so nothing had written it since the last one).
static inline int sizepScale(uint8_t sizep)
{
    switch (sizep & 3) { case 1: return 2; case 3: return 4; default: return 1; }
}

// ---- shared wide-object (SIZEPn) segment machinery -------------------------------------------
// See the WIDE OBJECTS block in RescueOnFractalus.h for the channel map and why the extensions
// are chained rather than copper re-pointed.  Both users (the laser impact burst on ch4 and the
// Main-Window P3 object on ch7) call buildWideObject with their own segment-0 sprite.

// Copy a chained lower element's live state into its second copy.  Each extension channel has TWO
// chains (the double buffer), so the energy bar / altimeter / left band triangle behind them exists
// twice.  All three are build-once solids, so `rows` is passed only on the build that fills the
// pixels; every other frame this is the two control words (the setY that moves a gauge bar).
static void mirrorSprite(Sprite* dst, const Sprite* src, int rows)
{
    if (!dst || !src) return;
    const uint16_t* a = src->data();
    uint16_t* b = dst->data();
    b[0] = a[0]; b[1] = a[1];
    for (int i = 0; i < rows * 2; i++) b[2 + i] = a[2 + i];
}

bool RescueOnFractalus::wideExtAcquire(uint8_t owner)
{
    if (wideOwner == owner) return true;
    // The burst outranks the P3 object: it is the shorter, louder event, and the object that
    // explodes is deactivated by the hit anyway, so a real contest should never happen.
    if (wideOwner == kWideShot) return false;
    // FULL clear on a handover — the outgoing owner's incremental clear only ever tracked ITS
    // rows, so anything it left outside the incoming object's extent would linger.  Both buffers.
    for (int s = 0; s < 3; s++) {
        const int h = (s == 2) ? kWideExtRowsCh1 : kWideExtRows;
        for (int b = 0; b < 2; b++) {
            uint16_t* d = wideExt[s][b]->data() + 2;
            for (int i = 0; i < h * 2; i++) d[i] = 0;
            widePrevBase[s][b] = 0; widePrevRows[s][b] = 0;
        }
    }
    wideOwner = owner;
    return true;
}

// ⚠ PHASE.  The burst's segment 0 (shotSprite/shotSpriteBack) is DOUBLE BUFFERED, so the strip on
// screen right now is the one built LAST frame — still the wide one on the frame the burst narrows.
// Blanking the DISPLAYED extension chain here therefore takes the right-hand segments away one
// frame BEFORE segment 0 stops being wide, and that single frame is visible: the 2×→1× step near
// the end of the explosion showed the left half only (user-observed, 2026-08-11).  A release is a
// build of blank rows and has to be latched on the same boundary as one — so for the burst we
// clear the OFF-SCREEN chain and re-point SPR5/6/1PT, exactly as buildWideObject does.  The
// displayed chain keeps its wide segments for the one frame its segment 0 still needs them; the
// stale rows it is left holding never reach the screen again, because whoever writes that chain
// next has to come through wideExtAcquire, which full-clears BOTH chains.
// `now` forces the old immediate both-chain blank for the one caller that has no frame to defer to
// — buildShotSprite's shot-just-ended path, which likewise blanks segment 0 in BOTH its buffers.
void RescueOnFractalus::wideExtRelease(uint8_t owner, bool now)
{
    if (wideOwner != owner) return;
    // Only the burst is double buffered; the Main-Window P3 object writes the displayed chain at
    // render rate (single-buffered segment 0), so its release is immediate either way.
    if (owner == kWideShot && !now) {
        const int w = wideDispIdx ^ 1;
#ifdef ROF_FLIGHT_PROBE
        // Count the frames this deferral is FOR: a burst release with pixels still live in the
        // DISPLAYED chain.  Each one was a frame the old immediate both-chain blank corrupted
        // (right half gone while segment 0 was still wide).  Zero here = the run never reached the
        // 2x->1x step, so it proves nothing either way — check g_wideShotScale[1]/[2] first.
        { extern volatile unsigned long g_wideLateBlank;
          if (widePrevRows[0][wideDispIdx] | widePrevRows[1][wideDispIdx]
                                           | widePrevRows[2][wideDispIdx]) g_wideLateBlank++; }
#endif
        for (int s = 0; s < 3; s++) {
            uint16_t* d = wideExt[s][w]->data() + 2;
            for (int i = 0; i < widePrevRows[s][w]; i++) {
                d[(widePrevBase[s][w] + i) * 2] = 0; d[(widePrevBase[s][w] + i) * 2 + 1] = 0;
            }
            widePrevRows[s][w] = 0;
        }
        if (flightCopper) {
            flightCopper->setHudSprite(5, *wideExt[0][w]);
            flightCopper->setHudSprite(6, *wideExt[1][w]);
            flightCopper->setHudSprite(1, *wideExt[2][w]);
        }
        wideDispIdx = (uint8_t)w;
    } else {
        for (int s = 0; s < 3; s++)
            for (int b = 0; b < 2; b++) {
                uint16_t* d = wideExt[s][b]->data() + 2;
                for (int i = 0; i < widePrevRows[s][b]; i++) {
                    d[(widePrevBase[s][b] + i) * 2] = 0; d[(widePrevBase[s][b] + i) * 2 + 1] = 0;
                }
                widePrevRows[s][b] = 0;
            }
    }
    wideOwner = kWideNone;
}

void RescueOnFractalus::buildWideObject(uint16_t* dst0, const volatile uint8_t* src,
                                        int base, int rows, int scale, uint16_t x, uint8_t owner)
{
    int segs = (scale >= 4) ? 4 : ((scale >= 2) ? 2 : 1);
    const int wanted = segs;
    if (segs > 1 && !wideExtAcquire(owner)) segs = 1;   // lost the contest: render 1× wide
#ifdef ROF_FLIGHT_PROBE
    { extern volatile unsigned long g_wideShotScale[3], g_wideP3Scale[3], g_wideMaxRows, g_wideDenied;
      const int bucket = (scale >= 4) ? 2 : ((scale >= 2) ? 1 : 0);
      if (owner == kWideShot) g_wideShotScale[bucket]++; else g_wideP3Scale[bucket]++;
      if (wanted != segs) g_wideDenied++;
      if (segs > 1 && (unsigned long)rows > g_wideMaxRows) g_wideMaxRows = (unsigned long)rows; }
#endif
    if (segs == 1) {
        // Narrow (or beaten to the channels): give the extensions back so the other object can
        // have them, and blank whatever we last drew there — for the burst, in the OFF-SCREEN
        // chain, because the wide segment 0 it pairs with is still the one on screen.
        wideExtRelease(owner);
        for (int i = 0; i < rows; i++) {
            const uint16_t m = s_shotExpand[src[i]];
            dst0[(base + i) * 2] = m; dst0[(base + i) * 2 + 1] = m;   // both planes → pen 11
        }
        return;
    }

    // Which chain to write.  FLIP only for the burst: its segment 0 (shotSprite/shotSpriteBack) is
    // double-buffered and therefore reaches the screen a frame after it is built, so its segments
    // must be latched on that same boundary or they run a frame ahead of it.  The Main-Window P3
    // object's segment 0 (viewportP3Sprite) is single-buffered and written at render rate, so its
    // segments stay in the DISPLAYED chain and are likewise immediate — in phase either way.
    const bool flip = (owner == kWideShot);
    const int  w    = flip ? (wideDispIdx ^ 1) : wideDispIdx;

    // Incremental clear of this chain's extensions (the buildViewportP3Sprite pattern), then
    // rewrite.  Per-buffer, because the two alternate — same reason shotPrevRows is per-buffer.
    uint16_t* ed[3];
    for (int s = 0; s < 3; s++) {
        ed[s] = wideExt[s][w]->data() + 2;
        for (int i = 0; i < widePrevRows[s][w]; i++) {
            ed[s][(widePrevBase[s][w] + i) * 2] = 0; ed[s][(widePrevBase[s][w] + i) * 2 + 1] = 0;
        }
        widePrevRows[s][w] = 0;
    }
    // Per-segment row clip.  Segments 1 and 2 are as tall as segment 0, so only segment 3 (ch1,
    // 85 rows) can lose rows off the bottom.
    int segRows[4];
    segRows[0] = segRows[1] = segRows[2] = rows;
    segRows[3] = (base + rows > kWideExtRowsCh1) ? (kWideExtRowsCh1 - base) : rows;
    if (segRows[3] < 0) segRows[3] = 0;

    // One pass over the source: every segment of a row comes from the same byte, and each
    // segment's slice is an IMMEDIATE shift (no 68000 barrel shifter — a variable shift here
    // would cost more than the store).
    // ⚠ PEN per segment differs, because each channel had to borrow a pen its PAIR was not
    // already using in the viewport.  Sprite data word 0 is the LOW plane, word 1 the HIGH one:
    //   pen 01 = (data, 0) · pen 10 = (0, data) · pen 11 = (data, data)
    // seg 0 (ch4/ch7) and seg 3 (ch1) use pen 11; seg 1 (ch5) pen 10 = COLOR26; seg 2 (ch6)
    // pen 01 = COLOR29.  All four colour registers are poked to the same value each frame.
    if (segs == 2) {
        for (int i = 0; i < rows; i++) {
            const uint8_t b = src[i];
            const uint16_t w0 = s_wideExpand4[b >> 4];
            dst0[(base + i) * 2] = w0; dst0[(base + i) * 2 + 1] = w0;          // seg 0, pen 11
            const uint16_t w1 = s_wideExpand4[b & 0x0F];
            ed[0][(base + i) * 2] = 0;  ed[0][(base + i) * 2 + 1] = w1;        // seg 1 ch5, pen 10
        }
    } else {
        for (int i = 0; i < rows; i++) {
            const uint8_t b = src[i];
            const uint16_t w0 = kWideExpand8[(b >> 6) & 3];
            dst0[(base + i) * 2] = w0; dst0[(base + i) * 2 + 1] = w0;          // seg 0, pen 11
            const uint16_t w1 = kWideExpand8[(b >> 4) & 3];
            ed[0][(base + i) * 2] = 0;  ed[0][(base + i) * 2 + 1] = w1;        // seg 1 ch5, pen 10
            const uint16_t w2 = kWideExpand8[(b >> 2) & 3];
            ed[1][(base + i) * 2] = w2; ed[1][(base + i) * 2 + 1] = 0;         // seg 2 ch6, pen 01
        }
        for (int i = 0; i < segRows[3]; i++) {
            const uint16_t w3 = kWideExpand8[src[i] & 3];
            ed[2][(base + i) * 2] = w3; ed[2][(base + i) * 2 + 1] = w3;        // seg 3 ch1, pen 11
        }
        widePrevBase[1][w] = base; widePrevRows[1][w] = rows;
        widePrevBase[2][w] = base; widePrevRows[2][w] = segRows[3];
        wideExt[1][w]->setX((uint16_t)(x + 32));
        wideExt[2][w]->setX((uint16_t)(x + 48));
    }
    widePrevBase[0][w] = base; widePrevRows[0][w] = rows;
    wideExt[0][w]->setX((uint16_t)(x + 16));
    if (flip) {
        // Latch the whole segment set with segment 0: the copper picks these up at the top of the
        // next frame, the same frame shotSprite's own SPR4PT re-point takes effect.
        if (flightCopper) {
            flightCopper->setHudSprite(5, *wideExt[0][w]);
            flightCopper->setHudSprite(6, *wideExt[1][w]);
            flightCopper->setHudSprite(1, *wideExt[2][w]);
        }
        wideDispIdx = (uint8_t)w;
    }
}

void RescueOnFractalus::buildShotSprite()
{
    SP_CNT(g_spShotCalls); SP_NOP(); SP_RESET();
    if (mem[0x0036] == 0) {                            // no shot active
        SP_CNT(g_spIdle);
        if (shotWasActive) {                           // shot just ended: blank BOTH buffers once so
            SP_CNT(g_spBlank);
            uint16_t* a = shotSprite->data()     + 2;  // whichever is on-screen shows nothing, then
            uint16_t* b = shotSpriteBack->data() + 2;  // leave ch4 pointing at a blank buffer (no more
            for (int k = 0; k < 2; k++) {              // per-frame work while idle).  Base-aware:
                uint16_t* d = k ? b : a;               // the strip sits at an internal row offset now
                for (int i = 0; i < (int)shotPrevRows[k]; i++) {
                    const int r = (shotPrevBase[k] + i) * 2;
                    d[r] = 0; d[r + 1] = 0;
                }
                shotPrevRows[k] = 0; shotPrevBase[k] = 0;
            }
            // IMMEDIATE (now = true): this path blanks segment 0 in BOTH buffers, so the wide
            // strip leaves the screen THIS frame and the extensions must go with it — there is no
            // later frame to defer the blank to.  (In practice both chains are already empty: the
            // burst always shrinks back through 1× before $8CBA kills it.)
            wideExtRelease(kWideShot, true);           // hand the wide segments back to the P3 object
            if (flightCopper) flightCopper->setHudSprite(4, *shotSprite);
            shotWasActive = false;
        }
        SP_LAP(g_spIdleT);
        return;
    }
    SP_CNT(g_spActive);
    shotWasActive = true;
    // Build into the OFF-screen buffer (the one NOT latched for display this frame): the copper
    // fetched SPR4PT at the top of the frame, before this VBI-time build, so re-pointing it now
    // takes effect NEXT frame — the displayed buffer is always a fully-built one, never mid-write.
    Sprite* s = shotBuildIdx ? shotSpriteBack : shotSprite;
    uint16_t* d = s->data() + 2;                       // skip the 2 control words
#ifdef ROF_SHOT_SCAN
    // A/B baseline (make SHOT_SCAN=1): the ORIGINAL scan + unconditional clear, kept so the win can
    // be re-measured in one tree (`GDBSCRIPT=isr_ab.gdb` on this build vs the default).  Do not
    // combine with SHOT_VERIFY — the differential would then compare the scan against itself.
    int top = -1, bot = -1;
    for (int o = 0x34; o <= 0x91; o++)
        if (mem[0x0E00 + o]) { if (top < 0) top = o; bot = o; }
    int rows = (top < 0) ? 0 : bot - top + 1;
    if (top < 0) top = 0x34;
    const int shotBase = (top - 0x32) + kShotYOff;
    if (shotBase + rows > kShotRows) rows = kShotRows - shotBase;
    if (rows < 0) rows = 0;
    SP_LAP(g_spRunT);
    for (int i = 0; i < kShotRows * 2; i++) d[i] = 0;  // clear, then decode the run below
    SP_LAP(g_spClearT);
#else
    // The strip's extent is ALREADY in mem[]: build_player2_sprite ($8C58, which draws it) records
    // the start row in $2865 and the length in $2866 precisely so it can erase exactly that region
    // on its NEXT call, and every drawing path sets both ($8ce6 STX $2865, $8d00 $2866 = end-start).
    // Nothing else in the binary writes either byte.  So the 94-byte volatile scan that used to
    // re-derive them was pure waste — and expensive waste: GCC compiled it as a 32-bit loop counter
    // with a `.l`-indexed load and a 32-bit immediate compare, ~72 cycles/iteration x 94 with no
    // early exit.  Measured (SPRITE_SHAPE): 53% of this function, and this function is 88% of the
    // flight VBI's whole sprite bracket, which the ISR pays 50x/s regardless of frame rate.
    // The scan was also NOT strictly correct: draw_ah_ground_fill_p2 ($40B0) writes as low as
    // $0E87, inside the scanned window, so the artificial-horizon fill could extend the run it
    // found.  Reading the extent is immune to that.
    // $8C58 is skipped on some frames (the $00C8 parity gate / event_active_flag) — but then the
    // buffer and $2865/$2866 are BOTH unchanged, so they still describe it.
    // The $34 lower bound is the old scan's window: a strip starting at $32/$33 was clipped to $34
    // and setY placed accordingly, so clip identically here.
    int top = 0x32 + (int)mem[0x2865], rows = (int)mem[0x2866];
    if (top < 0x34) { rows -= 0x34 - top; top = 0x34; }
    // Clip to the sprite, which is now the FULL viewport height at a fixed VSTART, so a 4× burst
    // (up to ~52 rows) no longer loses its bottom the way the old 32-row moving-VSTART one did.
    const int shotBase = (top - 0x32) + kShotYOff;
    if (shotBase + rows > kShotRows) rows = kShotRows - shotBase;
    if (rows < 0) rows = 0;
#ifdef ROF_SHOT_VERIFY
    // In-process differential (make SHOT_VERIFY=1 + amiga/shot_verify.gdb).  make validate cannot
    // check this: it is an Amiga-only render mirror, not a mem[] contract.  So re-derive the extent
    // the OLD way (the 94-byte scan) every call and compare — but compare what actually reaches the
    // screen, the 32 decoded SPRITE ROWS plus (only when some row is lit) the VSTART.  An extent
    // difference over rows that are all ZERO produces a byte-identical, still-invisible sprite;
    // that is counted separately as g_svBenign, and g_svBad must stay 0.
    { int oTop = -1, oBot = -1;
      for (int o = 0x34; o <= 0x91; o++)
          if (mem[0x0E00 + o]) { if (oTop < 0) oTop = o; oBot = o; }
      int oRows = (oTop < 0) ? 0 : oBot - oTop + 1;
      if (oRows > kShotRows) oRows = kShotRows;
      if (oTop < 0) oTop = top;                        // no run: the old code left top untouched
      int differs = 0, lit = 0;
      for (int i = 0; i < kShotRows; i++) {
          uint16_t o = (i < oRows) ? expandShotRow(mem[0x0E00 + oTop + i]) : 0;
          uint16_t n = (i < rows)  ? expandShotRow(mem[0x0E00 + top  + i]) : 0;
          if (o != n) differs = 1;
          if (n | o) lit = 1;
      }
      g_svCalls++;
      if (differs || (lit && oTop != top)) g_svBad++;
      else if (oRows != rows || oTop != top) g_svBenign++;
      if (differs || (lit && oTop != top) || oRows != rows || oTop != top) {
          g_svLastTop = (unsigned long)top;  g_svLastRows = (unsigned long)rows;
          g_svOraTop = (unsigned long)oTop; g_svOraRows = (unsigned long)oRows;
          g_svLastLit = (unsigned long)lit;
          g_svLast36 = mem[0x0036];  g_svLast2865 = mem[0x2865]; g_svLast2866 = mem[0x2866];
      } }
#endif
    SP_LAP(g_spRunT);
    // Incremental clear (the buildScopeP3Sprite/buildViewportP3Sprite pattern): the decode below
    // overwrites rows 0..rows-1 anyway, so only rows this build does NOT reach need zeroing — and
    // only as far as THIS buffer's last build reached.  Mean run is 4.3 rows, so the old
    // unconditional 128-byte clear (which GCC turned into a `jsr memset` byte loop, 7.7 t/call =
    // 25% of the function) is ~0 work in steady state.  Per-buffer because the two alternate.
    for (int i = 0; i < (int)shotPrevRows[shotBuildIdx]; i++) {
        const int r = (shotPrevBase[shotBuildIdx] + i) * 2;
        if (r >= shotBase * 2 && r < (shotBase + rows) * 2) continue;   // about to be overwritten
        d[r] = 0; d[r + 1] = 0;
    }
    shotPrevRows[shotBuildIdx] = (uint8_t)rows;
    shotPrevBase[shotBuildIdx] = (uint8_t)shotBase;
    SP_LAP(g_spClearT);
#endif
    // Did we hold the extension channels ENTERING this frame?  On the frame the burst narrows, the
    // deferred wideExtRelease drops ownership but the displayed chain still shows our right-hand
    // segments for one more frame (it is in phase with the displayed segment 0) — so they still
    // want this frame's COLPM2, or the two halves of the same burst differ by a luminance step.
    const bool heldWideExt = (wideOwner == kWideShot);
    if (rows > 0) {
#ifdef ROF_SPRITE_SHAPE
        g_spRows += (unsigned long)rows; g_spTopSum += (unsigned long)top;
        g_spBotSum += (unsigned long)(top + rows - 1);
        if ((unsigned long)rows > g_spRowsMax) g_spRowsMax = (unsigned long)rows;
#endif
        // SIZEP2 (mem[$00CD]) widens the impact burst to 2×/4× as it peaks — 32/64 lores px, i.e.
        // 2 or 4 sprite segments.  The Atari has already shifted HPOSP2 left by $286E (4 or 12
        // colour clocks) so mem[$00CB] is the widened player's LEFT edge; segment n sits 16 px on.
        const uint16_t x = (uint16_t)(0x81 + ((int)mem[0x00CB] - 0x32) * 2 + kShotXOff);
        buildWideObject(d, mem + 0x0E00 + top, shotBase, rows,
                        sizepScale(mem[0x00CD]), x, kWideShot);
        s->setY(kTerrainLine);                         // FIXED VSTART; the strip sits at shotBase
        s->setX(x);
    } else {
        SP_CNT(g_spNoRun);
        wideExtRelease(kWideShot);                     // nothing to draw: let the P3 object have them
    }
    SP_LAP(g_spDecT);
    if (flightCopper) {
        flightCopper->setHudSprite(4, *s);                            // display this buffer next frame
        const uint16_t col = atariToOCS(mem[0x0037]);
        flightCopper->setShotColor(col);                              // COLPM2 → COLOR27 (segment 0)
        if (wideOwner == kWideShot || heldWideExt)
            flightCopper->setWideExtColor(col);                        // segments 1-3 match
    }
    shotBuildIdx ^= 1;                                                // next frame builds the other buffer
    SP_LAP(g_spCopT);
}

// ---- PMG run-scan helpers ------------------------------------------------------------
// Three per-frame sprite mirrors (scope P3, viewport P3, scanner dot) locate their object by
// scanning a window of the Atari PMG buffers in mem[] for the first/last byte with a bit set.
// Written the obvious way — `for (int o = lo; o <= hi; o++) if (mem[base + o] & m)` — GCC turns
// each byte test into `move.l d,d1 / addi.l #base,d1 / move.b (0,a2,d1.l),d1 / ... /
// cmpi.l #hi,d / bne`: 78-84 cycles PER BYTE TESTED, essentially all of it addressing.  Over the
// three windows (84 + 33 + 49 bytes) that is ~13.2k cycles a frame — measured 58 t/it, ~3.9% of
// ALL wall clock (phase_budget, 2026-08-09), for three loops that normally find nothing at all.
//
// These helpers walk a POINTER and test FOUR BYTES AT A TIME.  A test against a mask replicated
// into all four lanes is BYTE-ORDER INDEPENDENT (nonzero iff some byte matches, whichever end the
// bytes come from), so aliasing mem[] as a volatile uint32_t is safe here — unlike a value read,
// which would byte-swap between the big-endian Amiga and the little-endian validation host (see
// CLAUDE.md, "Endianness when aliasing mem[]").  No value ever leaves the long: once a long tests
// nonzero its four bytes are re-read individually through mem[] to find which one matched.
// Alignment is taken from the POINTER, not from the mem[] offset, so nothing depends on where the
// linker put mem[] (a misaligned long read is an address fault on the 68000).
//
// Same family as the fix in buildShotSprite above, one level along: there the scan was replaced by
// the extent its writer already publishes ($2865/$2866).  The P3/missile buffers have no such
// published extent, so these keep the scan and make it cheap instead — writer-agnostic, and
// equivalent by construction rather than by an argument about who writes what.

// Lowest offset in [lo,hi] whose mem[] byte has any bit of `mask` set; -1 if none.
static int pmgScanFirst(unsigned lo, unsigned hi, uint8_t mask)
{
    const volatile uint8_t* const M = mem;
    const volatile uint8_t* p = M + lo;
    const volatile uint8_t* const e = M + hi;               // inclusive
    while (p <= e && (((unsigned long)p) & 3u)) { if (*p & mask) return (int)(p - M); p++; }
    const uint32_t m4 = (uint32_t)mask * 0x01010101u;
    // Counted, not `for (; p + 3 <= e; p += 4)`: written that way GCC re-derives the limit inside
    // the loop (`move.l (a0),d0 / addq.l #4,a0 / lea 3(a0),a1 / cmpa.l d1,a1 / bls`) = 52 cyc per
    // long.  The trip count is a compile-time constant at both call sites, so a countdown lets it
    // hoist the end pointer: `addq.l #4,a0 / cmpa.l a0,a1 / beq.s / move.l (a0),d0 / beq.s` = 44,
    // i.e. 11 cycles per byte tested against the byte loop's 78.  (Verified in the disassembly —
    // GCC keeps the limit compare rather than taking the `(a0)+` the source suggests.)
    const volatile uint32_t* q = (const volatile uint32_t*)p;
    for (int n = (int)((e - p + 1) >> 2); n > 0; n--) {
        if (*q++ & m4) {
            p = (const volatile uint8_t*)(q - 1);
            if (p[0] & mask) return (int)(p - M);
            if (p[1] & mask) return (int)(p + 1 - M);
            if (p[2] & mask) return (int)(p + 2 - M);
            return (int)(p + 3 - M);
        }
    }
    for (p = (const volatile uint8_t*)q; p <= e; p++) if (*p & mask) return (int)(p - M);
    return -1;
}

// Lowest AND highest offset in [lo,hi] with any bit of `mask` set (gaps allowed, unlike
// pmgScanFirst's callers, which stop at the end of the first run).  One forward pass.
static void pmgScanBounds(unsigned lo, unsigned hi, uint8_t mask, int* firstOut, int* lastOut)
{
    const volatile uint8_t* const M = mem;
    const volatile uint8_t* p = M + lo;
    const volatile uint8_t* const e = M + hi;               // inclusive
    int first = -1, last = -1;
    while (p <= e && (((unsigned long)p) & 3u)) {
        if (*p & mask) { if (first < 0) first = (int)(p - M); last = (int)(p - M); }
        p++;
    }
    const uint32_t m4 = (uint32_t)mask * 0x01010101u;
    const volatile uint32_t* q = (const volatile uint32_t*)p;   // countdown + (a0)+, as above
    for (int n = (int)((e - p + 1) >> 2); n > 0; n--) {
        if (*q++ & m4) {
            const volatile uint8_t* b = (const volatile uint8_t*)(q - 1);
            for (int k = 0; k < 4; k++)
                if (b[k] & mask) { if (first < 0) first = (int)(b + k - M); last = (int)(b + k - M); }
        }
    }
    for (p = (const volatile uint8_t*)q; p <= e; p++)
        if (*p & mask) { if (first < 0) first = (int)(p - M); last = (int)(p - M); }
    *firstOut = first; *lastOut = last;
}

#ifdef ROF_SCAN_VERIFY
// In-process differential (make SCAN_VERIFY=1 + amiga/scan_verify.gdb).  `make validate` cannot
// check any of this: these are Amiga-only render mirrors, not a mem[] contract.  So every call
// re-derives the run the ORIGINAL byte-loop way and compares — the helper result is what goes
// live, so a surviving mismatch is the helper's fault.  g_scanBad must stay 0.
//
// ⚠ The oracle is run TWICE, once either side of the helper, and a call where the two oracle
// passes disagree is discarded as RACED rather than counted bad.  The P3/missile buffers are
// written by the flight VBI ISR (draw_player3_object clears and redraws the strip, $44E0 moves
// the scanner blob), so an ISR landing between the two passes makes them read different bytes —
// a harness artifact, not a logic difference, and the same one band_verify freezes its source to
// avoid.  Without this the first run reported 24 "mismatches" in 2244 calls, every one of them a
// run whose top or bottom had moved by a single row.  Sandwiching the oracle proves which it is
// instead of assuming: a genuine helper bug cannot depend on the buffer changing.
extern "C" volatile unsigned long g_scanCalls = 0, g_scanBad = 0, g_scanHit = 0, g_scanRaced = 0,
                                  g_scanLastBadTop = 0, g_scanLastBadBot = 0;
// The oracle: the original per-byte loop, verbatim.  `contiguous` = the two P3 mirrors' early
// break at the end of the first run; the scanner dot takes first/last over the whole window.
static void scanOracle(unsigned base, int lo, int hi, uint8_t mask, int contiguous,
                       int* topOut, int* botOut)
{
    int t = -1, b = -1;
    for (int o = lo; o <= hi; o++) {
        if (mem[base + o] & mask) { if (t < 0) t = o; b = o; }
        else if (contiguous && t >= 0) break;
    }
    *topOut = t; *botOut = b;
}
#define SCAN_PRE(base, lo, hi, mask, contiguous)                                                  \
    int _svPT, _svPB; scanOracle((base), (lo), (hi), (mask), (contiguous), &_svPT, &_svPB)
#define SCAN_CHK(top, bot, base, lo, hi, mask, contiguous) do {                                   \
        int _t, _b; scanOracle((base), (lo), (hi), (mask), (contiguous), &_t, &_b);               \
        g_scanCalls++;                                                                            \
        if (_t != _svPT || _b != _svPB) { g_scanRaced++; break; }   /* ISR moved the buffer */    \
        if (_t >= 0) g_scanHit++;                                                                 \
        if (_t != (top) || _b != (bot)) {                                                         \
            g_scanBad++;                                                                          \
            g_scanLastBadTop = (unsigned long)((((unsigned)_t & 0xFFFFu) << 16)                   \
                                             | ((unsigned)(top) & 0xFFFFu));                      \
            g_scanLastBadBot = (unsigned long)((((unsigned)_b & 0xFFFFu) << 16)                   \
                                             | ((unsigned)(bot) & 0xFFFFu));                      \
        }                                                                                         \
    } while (0)
#else
#define SCAN_PRE(base, lo, hi, mask, contiguous) ((void)0)
#define SCAN_CHK(top, bot, base, lo, hi, mask, contiguous) ((void)0)
#endif

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
    // Contiguous P3 object: find its first byte with the long-striding scan, then walk the run out
    // byte-wise (a run is a handful of rows, so only the SEARCH was worth widening).
    SCAN_PRE(0x0F00, 0x98, 0xB8, 0xFF, 1);
    int top = pmgScanFirst(0x0F98u, 0x0FB8u, 0xFFu), bot = -1;
    if (top >= 0) {
        bot = top;
        while (bot < 0x0FB8 && mem[bot + 1]) bot++;
        top -= 0x0F00; bot -= 0x0F00;                // back to the buffer-offset space used below
    }
    SCAN_CHK(top, bot, 0x0F00, 0x98, 0xB8, 0xFF, 1);
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
    // Contiguous P3 object — same shape as buildScopeP3Sprite: long-striding search, byte-wise run.
    SCAN_PRE(0x0F00, 0x32, 0x85, 0xFF, 1);
    int top = pmgScanFirst(0x0F32u, 0x0F85u, 0xFFu), bot = -1;
    if (top >= 0) {
        bot = top;
        while (bot < 0x0F85 && mem[bot + 1]) bot++;
        top -= 0x0F00; bot -= 0x0F00;                // back to the buffer-offset space used below
    }
    SCAN_CHK(top, bot, 0x0F00, 0x32, 0x85, 0xFF, 1);
    for (int i = 0; i < p3ViewportPrevRows; i++) {   // clear only last frame's rows (not all 94)
        d[(p3ViewportPrevBase + i) * 2] = 0; d[(p3ViewportPrevBase + i) * 2 + 1] = 0;
    }
    viewportP3Sprite->setY(kTerrainLine);            // FIXED VSTART: keep ch7 busy through the viewport
    if (top >= 0) {
        int base = top - 0x32;                       // object's row within the fixed-VSTART sprite
        int rows = bot - top + 1;
        if (base + rows > kViewportP3Rows) rows = kViewportP3Rows - base;
        // SIZEP3 widens the object exactly as SIZEP2 widens the laser burst: draw_player3_object
        // loads it from table $4566 = {$03,$01,$01} indexed by mem[$006A] (= $0063>>2, counting
        // down as the target closes), so the last three approach frames are 4×, 2×, 2× — 64/32
        // lores px.  Ground-truthed on a800dumps/saucerbigpause.a8s: $006A = 0 ⇒ SIZEP3 = $03 = 4×,
        // with the Main-Window strip spanning all 8 source bits over its 19 rows.  There is no
        // mem[] shadow for it, so the value comes from the bus_write latch (src/cpu/bus.h).
        const uint16_t x = (uint16_t)(0x81 + ((int)mem[0x2870] - 0x32) * 2);      // HPOSP3 shadow → X
        buildWideObject(d, mem + 0x0F00 + top, base, rows,
                        sizepScale(g_sizep3_shadow), x, kWideP3);
        viewportP3Sprite->setX(x);
        p3ViewportPrevBase = base; p3ViewportPrevRows = rows;
    } else {
        p3ViewportPrevRows = 0;                      // inactive: nothing to clear next frame
        wideExtRelease(kWideP3);                     // and give the wide segments back
    }
    if (flightCopper) {
        const uint16_t col = atariToOCS(mem[0x00D9]);
        flightCopper->setViewportP3Color(col);                            // COLPM3 → COLOR31 (segment 0)
        if (wideOwner == kWideP3) flightCopper->setWideExtColor(col);     // segments 1-3 match
    }
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
    // Unlike the two P3 mirrors this one takes the first AND last hit over the whole window (the
    // blob may in principle have gaps), so it cannot stop at the end of a run — which is why it was
    // the only one of the three scanning its window unconditionally, every frame.  One long-striding
    // pass now does both bounds.  Mask $30 = the M2 bit pair; replicated per lane it stays
    // byte-order independent.
    SCAN_PRE(0x0B00, 0x88, 0xB8, 0x30, 0);
    int top = -1, bot = -1;
    pmgScanBounds(0x0B88u, 0x0BB8u, 0x30u, &top, &bot);
    if (top >= 0) { top -= 0x0B00; bot -= 0x0B00; }   // back to the buffer-offset space used below
    SCAN_CHK(top, bot, 0x0B00, 0x88, 0xB8, 0x30, 0);
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
    bool filled = false;
    if (!energySolidBuilt) {
        uint16_t* d = energyIndicatorSprite->data() + 2; // skip the 2 control words
        for (int i = 0; i < kEnergyRows; i++) { d[i * 2] = 0xFF00u; d[i * 2 + 1] = 0x0000u; }
        energySolidBuilt = true; filled = true;
    }
    uint8_t  fuel = mem[0x062F];
    uint16_t top;
    if (fuel == 0) top = 64u;                            // empty → park below floor = blacked out
    else {
        top = (uint16_t)((uint8_t)(0xDC - fuel) >> 2);   // bar-top index (vobj_pos_to_pmstrip_index $41DA)
        if (top > (uint16_t)kEnergyRows) top = 0u;       // clamp garbage → full bar
    }
    energyIndicatorSprite->setY((uint16_t)(kBase + top));
#ifdef ROF_FLIGHT_PROBE
    { extern volatile unsigned short g_energySprY; g_energySprY = (unsigned short)(kBase + top); }
#endif
    // ch5 carries a wide-object segment ahead of this bar, and that segment is double-buffered, so
    // the bar is chained into BOTH chains — keep the spare copy in step (control words each frame,
    // pixels only on the one-time solid fill).
    mirrorSprite(wideLow[0][1], energyIndicatorSprite, filled ? kEnergyRows : 0);
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
    bool filled = false;
    if (!altimSolidBuilt) {
        uint16_t* at = altimeterSprite->data() + 2;       // skip the 2 control words
        uint16_t* sh = altimeterShipSprite->data() + 2;
        for (int i = 0; i < kAltimRows; i++) {
            at[i * 2] = 0xFFFFu; at[i * 2 + 1] = 0x0000u; // terrain bar: plane A (pen 01 / COLOR29)
            sh[i * 2] = 0x0000u; sh[i * 2 + 1] = 0xFFFFu; // ship bar:    plane B (pen 10 / COLOR30)
        }
        altimSolidBuilt = true; filled = true;
    }
    // top = bar-top offset (0 = full bar at the dial top, kAltimRows = empty).  Pre-flight (and the
    // first flight frame before the VBI computes it) $281A holds garbage (e.g. $88); an out-of-range
    // value added to kAltimTopLine would push VSTART past 255 and wrap into the windscreen (Sprite::setY
    // has no SV8), so clamp it to 0 = full (the topmost valid position).
    uint16_t top = mem[0x281A];
    if (top > (uint16_t)kAltimRows) top = 0;
    altimeterSprite->setY((uint16_t)(kAltimTopLine + top));
    mirrorSprite(wideLow[1][1], altimeterSprite, filled ? kAltimRows : 0);   // ch6's second chain
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
//
// The clear is BLITTED, and only over the TAIL.  Two disjoint regions:
//   head — slot 0 (control words) + slots 1..89 (the converted strip): fully overwritten below,
//          so pre-zeroing it would be wasted work;
//   tail — slots 90..kStarRingSlots-1: the padding rows and terminator the window scrolls into.
//          starVblankUpdate only ever writes the ONE new bottom row per advance, so those slots
//          must already read zero when the window reaches them — and on a re-entry they still
//          hold the previous pass's rows.
// The CPU loop that used to clear all 8832 words of the 6 rings cost ~1180 beam ticks ≈ 75 ms —
// on its own the whole of the tunnel->stars freeze (measured 185 ticks after this change).  Because head and tail are disjoint, the conversion below runs
// while the blit is in flight and nothing has to wait for it until the closing drain.
void RescueOnFractalus::buildStarSprites()
{
    static const int kHeadSlots = kStarRows + 1;                    // control slot + the 89 rows
    static const int kTailSlots = kStarRingSlots - kHeadSlots;
#ifdef ROF_FLIGHT_PROBE
    g_seSprKick = 0; g_seSprConv = 0;
#endif
    for (int i = 0; i < 6; i++) {
        uint16_t* ring = starRing[i];
        if (!ring) continue;
#ifdef ROF_FLIGHT_PROBE
        const unsigned long _k0 = rof_subclock();
#endif
        // 2 words per slot, no modulo → one contiguous kTailSlots*2-word wipe.
        AmigaHardware::blitterClear(ring + kHeadSlots * 2, 2, (uint16_t)kTailSlots, 0);
#ifdef ROF_FLIGHT_PROBE
        const unsigned long _k1 = rof_subclock(); g_seSprKick += _k1 - _k0;
#endif
        ring[0] = starCtl[i][0];  ring[1] = starCtl[i][1];          // window-0 control slot
        const uint8_t* src = (const uint8_t*)&mem[kStarSrc[i >> 1]];
        const uint16_t* tbl = (i & 1) ? kStarGlyphHi : kStarGlyphLo;
        uint16_t* dst = ring + 2;                                   // slot 1 (skip control slot 0)
        for (int r = 0; r < kStarRows; r++) { *dst++ = tbl[src[r]]; *dst++ = 0x0000; }
#ifdef ROF_FLIGHT_PROBE
        g_seSprConv += rof_subclock() - _k1;
#endif
    }
    // The blitter interrupt is disabled in this port, so a queued blit only runs when something
    // pumps the queue — drain here rather than leave the later rings' clears pending.
#ifdef ROF_FLIGHT_PROBE
    const unsigned long _d0 = rof_subclock();
#endif
    AmigaHardware::blitterDrain();
#ifdef ROF_FLIGHT_PROBE
    g_seSprDrain = rof_subclock() - _d0;
#endif
    starWindow  = 0;
    starLastGen = g_starScrollGen;
}

// starVblankUpdate: the WHOLE per-vblank zero-copy star scroll — window advance, copper SPRxPT
// re-point, sprite control words, and the new-row pixel conversion — called from the real VBI ISR
// (PlatformAmiga::vbiHandler), ahead of game_vbi_isr.  The field scrolls up by N rows since the
// last vblank (g_starScrollGen delta, 0 or 1 normally): advance the window by N, re-point the
// copper, write the (constant) control words at the new window slot, and convert ONLY the N new
// bottom rows into the ring.  Everything else — the already-converted rows, the 5 blank padding
// rows and the terminator — is untouched (the padding/terminator are the still-zero slots below
// the star region).  See RescueOnFractalus.h.
//
// ⚠ ALL FOUR STEPS MUST RUN HERE, in this order.  Each has a beam deadline, and doing any of them
// from the main loop races the beam on a fast CPU:
//   pointer  — the copper executes the list's sprite-pointer MOVEs at scanline 16 (CopperList's
//              d[0] = copperWait(16,0)), so the operand must be final by then;
//   control  — the sprite's control-word DMA fetch is at ~scanline 25;
//   pixels   — a row is fetched at VSTART+row, i.e. scanline 44 and below.
// The pointer half used to live in perFrameWork "a frame ahead", which held only because the A500
// render is slow enough to land it late in the frame (measured beam line 78..307).  On a faster
// CPU the main loop resumes from the frame-sync wait much earlier: A1200 measured 18..307, and a
// 68040 published 31 of 32 advances at line <= 16 — the copper then latched the NEW window in the
// SAME frame while the control words still sat at the OLD slot, so the sprite fetched star PIXELS
// as its control words (garbage VSTART/VSTOP).  That dropped the star sprites AND, because
// channel 2's mid-screen re-point to the throttle gauge depends on the star sprite's VSTOP
// landing at the cockpit line, the gauge with them = the reported A1200 star+instrument flicker.
// At vblank the beam is above every one of those deadlines, so the four steps are one atomic
// update.  (Bonus: the window now advances once per real vblank instead of once per render pass,
// so the field scrolls at a steady 50 Hz — what the Atari VBI does — instead of in render-rate
// bursts.)
void RescueOnFractalus::starVblankUpdate()
{
    if (!starPhaseActive) return;
#ifdef ROF_FLIGHT_PROBE
    { extern volatile unsigned short g_starVbiEntryLineMax;
      unsigned short _el = rof_beam_line();
      if (_el > g_starVbiEntryLineMax) g_starVbiEntryLineMax = _el; }
#endif
    // ---- 1. advance the ring window by the rows scrolled since the last vblank ----
    // g_starScrollGen is bumped by scroll_field_columns ($6AEE) inside game_vbi_isr, which runs
    // LATER in this same ISR — so the delta seen here is the previous frame's scroll, exactly the
    // one-frame relationship the old perFrameWork placement had.
    extern volatile unsigned short g_starScrollGen;
    const unsigned short gen = g_starScrollGen;
    int N = (int)(unsigned short)(gen - starLastGen);
    if (N > 0) {
        starLastGen = gen;
        if (starWindow + N > kStarMaxScroll)                     // clamp: freeze rather than overrun
            N = (starWindow >= kStarMaxScroll) ? 0 : (kStarMaxScroll - starWindow);
        starWindow += N;
    }
    const int nw = starWindow;
    // ---- 2. re-point the copper at the new window (tightest deadline: scanline 16) ----
    if (planetCopper)
        for (int i = 0; i < 6; i++) planetCopper->setStarOperand(i, starRing[i] + 2 * nw);
#ifdef ROF_FLIGHT_PROBE
    { extern volatile unsigned short g_starPubLineMax, g_starPubLate, g_starPubLateAtVbi;
      unsigned short _pl = rof_beam_line();
      if (_pl > g_starPubLineMax) g_starPubLineMax = _pl;
      if (_pl >= 16) { g_starPubLate++; g_starPubLateAtVbi = platform_frame_count(); } }
#endif
    // ---- 3. control words at the new window head (deadline: the ~line-25 control fetch) ----
    // Written UNCONDITIONALLY, even when N==0: cheap (12 words), idempotent when the window is
    // static, and it guarantees the operand published above always finds control words at nw.
    for (int i = 0; i < 6; i++) {
        uint16_t* ring = starRing[i];
        ring[2 * nw]     = starCtl[i][0];
        ring[2 * nw + 1] = starCtl[i][1];
    }
    // ---- 4. convert the N brand-new bottom rows (deadline: their own fetch, line 44+) ----
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

// decodeDoorScrollRows(): decode field rows r0..r1 (inclusive) of the $2000 GTIA mode-10 door
// field (85 rows, stride 46) into the TALL doorScrollBitmap.  Same packing as the render() door
// decode (kDoorP1/kDoorP2 LUTs, 4-byte overscan crop, 10 longs/plane/row, plane3=0) — see that
// comment.  Row-ranged because this runs in the vblank ISR: see decodeDoorScrollDirty below.
void RescueOnFractalus::decodeDoorScrollRows(unsigned r0, unsigned r1)
{
    if (!doorScrollBitmap) return;
    if (r1 > 84u) r1 = 84u;              // rows 85..171 are the constant pad (see below)
    if (r0 > r1) return;
#ifdef ROF_FLIGHT_PROBE
    { extern volatile unsigned short g_dsDecodes, g_dsDecRows;
      extern volatile unsigned char g_dsDecRing[24]; extern volatile unsigned char g_dsDecRingIdx;
      g_dsDecodes++; g_dsDecRows = (unsigned short)(r1 - r0 + 1u);
      extern volatile unsigned char g_dsDecWhy, g_dsDecWhyRing[24];
      g_dsDecRing[g_dsDecRingIdx] = (unsigned char)(r1 - r0 + 1u);
      g_dsDecWhyRing[g_dsDecRingIdx] = g_dsDecWhy;
      g_dsDecRingIdx = (unsigned char)((g_dsDecRingIdx + 1u >= 24u) ? 0u : g_dsDecRingIdx + 1u); }
#endif
    const uint8_t* src = (const uint8_t*)mem + 0x2000 + 4 + r0 * 46;   // +4 = overscan crop
    uint8_t* vdest = (uint8_t*)doorScrollBitmap->data + r0 * 120;
    for (unsigned row = r0; row <= r1; row++) {
        uint32_t* p1 = (uint32_t*)vdest;
        uint32_t* p2 = (uint32_t*)(vdest + 40);
        uint32_t* p3 = (uint32_t*)(vdest + 80);
        for (int b = 0; b < 10; b++) {
            uint8_t s0 = *src++, s1 = *src++, s2 = *src++, s3 = *src++;
            *p1++ = ((uint32_t)kDoorP1[s0] << 24) | ((uint32_t)kDoorP1[s1] << 16) |
                    ((uint32_t)kDoorP1[s2] <<  8) |  (uint32_t)kDoorP1[s3];
            *p2++ = ((uint32_t)kDoorP2[s0] << 24) | ((uint32_t)kDoorP2[s1] << 16) |
                    ((uint32_t)kDoorP2[s2] <<  8) |  (uint32_t)kDoorP2[s3];
            *p3++ = 0u;
        }
        src   += 46 - 40;                // 46-byte field stride, 40 bytes consumed
        vdest += 120;
    }
}

void RescueOnFractalus::decodeDoorScrollField()
{
    if (!doorScrollBitmap) return;
    // Rows 85..171 are the green closed-door pad (field byte $88, which the field itself uses for
    // the blank door area above/below the LEVEL text).  They are CONSTANT — nothing else writes
    // doorScrollBitmap — so paint them once rather than on every full decode.
    static bool s_padDone = false;
    if (!s_padDone) {
        const uint32_t g1 = (uint32_t)kDoorP1[0x88] * 0x01010101u;
        const uint32_t g2 = (uint32_t)kDoorP2[0x88] * 0x01010101u;
        uint8_t* vdest = (uint8_t*)doorScrollBitmap->data + 85 * 120;
        for (int row = 85; row < 172; row++) {
            uint32_t* p1 = (uint32_t*)vdest;
            uint32_t* p2 = (uint32_t*)(vdest + 40);
            uint32_t* p3 = (uint32_t*)(vdest + 80);
            for (int b = 0; b < 10; b++) { *p1++ = g1; *p2++ = g2; *p3++ = 0u; }
            vdest += 120;
        }
        s_padDone = true;
    }
    decodeDoorScrollRows(0, 84);
}

// decodeDoorScrollDirty(): consume g_doorScrollFieldDirty, decoding ONLY the field rows the
// writer marked (rof_native.c's g_doorDirtyRow0/1).  Called from the vblank ISR, where the full
// 85-row decode costs ~19.7 ms = a whole PAL frame: doing it there overran the frame on every
// mid-scroll LEVEL-digit rewrite, losing one displayed step of the elevator scroll (a one-frame
// stall then a 2-pixel jump) and sometimes pushing the copper rewrite below behind the beam.
// A digit rewrite marks 8 rows.  Clear the flag FIRST so a re-dirty raised by the preempted main
// thread mid-decode is not lost — it just re-arms for the next frame (with an empty range, which
// falls back to a full decode).
void RescueOnFractalus::decodeDoorScrollDirty()
{
    if (!g_doorScrollFieldDirty) return;
    g_doorScrollFieldDirty = 0;
    const unsigned r0 = g_doorDirtyRow0, r1 = g_doorDirtyRow1;
    g_doorDirtyRow0 = 0xFF; g_doorDirtyRow1 = 0x00;
#ifdef ROF_FLIGHT_PROBE
    { extern volatile unsigned char g_dsDecWhy; g_dsDecWhy = (r0 > r1) ? 1 : 2; }  // 1=empty range, 2=marked
#endif
    if (r0 > r1) decodeDoorScrollField();        // no range recorded → whole field
    else         decodeDoorScrollRows(r0, r1);
}

// doorScrollVblankUpdate(): the level-select "elevator" door scroll, driven from the INTB_VERTB
// ISR at vblank start (the main thread is spinning in boot_standby_launch_driver's L_626a/L_628f
// level-select loops, so renderFrame never runs during the scroll — only this ISR does).  The
// Atari scrolls by DECrementing dl_src_index ($008B) per frame (dl_lms_scroll_step), which rebuilds the
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
#ifdef ROF_FLIGHT_PROBE
    const bool didDecode = (g_doorScrollFieldDirty != 0);
    uint16_t decLn0 = 0;
    if (didDecode) {
        extern volatile unsigned short g_dsDecEntryLn;
        uint16_t vp = *(volatile uint16_t*)0xDFF004u, vh = *(volatile uint16_t*)0xDFF006u;
        decLn0 = (uint16_t)(((vp & 1) << 8) | (vh >> 8));
        g_dsDecEntryLn = decLn0;
    }
#endif
    decodeDoorScrollDirty();
#ifdef ROF_FLIGHT_PROBE
    if (didDecode) {
        extern volatile unsigned short g_dsDecLines, g_dsDecLinesMax;
        uint16_t vp = *(volatile uint16_t*)0xDFF004u, vh = *(volatile uint16_t*)0xDFF006u;
        uint16_t ln1 = (uint16_t)(((vp & 1) << 8) | (vh >> 8));
        uint16_t d = (uint16_t)((ln1 >= decLn0) ? (ln1 - decLn0) : (ln1 + 312u - decLn0));
        g_dsDecLines = d;
        if (d > g_dsDecLinesMax) g_dsDecLinesMax = d;
    }
#endif

    // Lock-on indicator blink (cells $3491-$3497).  The faithful standby VBI keeps running
    // lock_on_indicator_tick ($4229) throughout the scroll, so it toggles the cell bytes in mem[]
    // exactly as the Atari does — but on the Amiga those cells only reach the display via the
    // cockpit decode in renderFrame, and renderFrame is stalled while boot_standby_launch_driver
    // busy-spins the scroll.  So complete the ISR-side display bridge here: whenever the tick has
    // flagged the strip dirty, re-decode those 7 cockpit cells straight into cockpitBitmap.  The
    // cockpit sits at Amiga lines 172+, decoded here at vblank start before the beam reaches it →
    // tear-free (same discipline as decodeScannerBlinkCells).  Clearing the flag also means the
    // main-loop renderFrame won't redundantly re-decode when it is running (idle standby).
    if (g_ckLockon) { g_ckLockon = 0u; decodeLockonDirty(); }

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
#ifdef ROF_FLIGHT_PROBE
    // Where is the beam when we rewrite the LIVE copper list?  (see the g_dsRun* comment)
    {
        extern volatile unsigned short g_dsRunLine[24]; extern volatile unsigned char g_dsRunDec[24];
        extern volatile unsigned char g_dsRunN[24]; extern volatile unsigned char g_dsRunIdx;
        extern volatile unsigned short g_dsRunWrites, g_dsRunLate, g_dsRunMaxLn;
        uint16_t vp = *(volatile uint16_t*)0xDFF004u, vh = *(volatile uint16_t*)0xDFF006u;
        uint16_t ln = (uint16_t)(((vp & 1) << 8) | (vh >> 8));
        unsigned char i = g_dsRunIdx;
        g_dsRunLine[i] = ln; g_dsRunDec[i] = didDecode; g_dsRunN[i] = (unsigned char)nRuns;
        g_dsRunIdx = (unsigned char)((i + 1u >= 24u) ? 0u : i + 1u);   // no 32-bit modulo on 68000
        g_dsRunWrites++;
        if (ln >= kCockpitLine - 1) g_dsRunLate++;
        if (ln > g_dsRunMaxLn) g_dsRunMaxLn = ln;
    }
#endif
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

    // (Nothing to seed: the tunnel rings are PAINTED into tunnelBitmap as the 6502 draws them —
    // drawTunnelRect/Columns/VSpan — and each direction primes the bitmap at its own entry point
    // (tunnelPaintBegin for the forward launch, the copper install for the boost).  The $1000
    // GTIA field is never read back.)

    leftPost   = Sprite::allocate(kHT);
    rightPost  = Sprite::allocate(kHT);
    nullSprite = Sprite::allocate(0);
    altimeterShipSprite = Sprite::allocate(kAltimRows);   // M3 $0B98 ship-height bar (flight)
    // Flight windscreen frame: posts span the A-pillar (86 rows) + the 8 band scanlines
    // (= kHT+8 = 94); the triangle outer-half sprites cover only the 8 band scanlines.
    flLeftPost  = Sprite::allocate(kHT + 8);
    flRightPost = Sprite::allocate(kHT + 8);
    flRightTri  = Sprite::allocate(8);
    // Wide-object extension channels (see RescueOnFractalus.h): segments 1-3 of a SIZEPn-widened
    // player, on the three channels idle across the viewport.  Each is CHAINED in front of that
    // channel's lower-region element in one chip buffer, so the channel shows both with no copper
    // re-point — segment first, then energy (ch5) / altimeter terrain (ch6) / left band triangle
    // (ch1).  ch5/ch6 run the full viewport (VSTOP 180, then VSTART 188+); ch1's stops one line
    // early (VSTOP 171) so its triangle keeps line 172 — the chained VSTART must be strictly past
    // the preceding VSTOP.  These allocations replace the standalone energy/altimeter/leftTri ones.
    // TWO chains per channel — the double buffer (see RescueOnFractalus.h): the owner writes the
    // off-screen one and re-points SPRxPT so all four segments latch together with segment 0.
    // The chained element exists in both; wideLow[s][0] is the live Sprite the rest of the code
    // already uses, wideLow[s][1] its mirror (mirrorSprite keeps the control words in step).
    static const int kLowRows[3] = { kEnergyRows, kAltimRows, 8 };
    for (int s = 0; s < 3; s++) {
        const int extRows = (s == 2) ? kWideExtRowsCh1 : kWideExtRows;
        for (int b = 0; b < 2; b++) {
            wideChain[s][b] = Sprite::allocateChain((uint16_t)extRows, (uint16_t)kLowRows[s],
                                                    wideExt[s][b], wideLow[s][b]);
            if (!wideChain[s][b]) return;
            // FIXED VSTART: the extensions must be armed across the whole viewport whether or not
            // anything is wide, or the chained element below never gets its control re-fetch.
            wideExt[s][b]->setY(kTerrainLine);
            wideExt[s][b]->setX(0);
        }
    }
    energyIndicatorSprite = wideLow[0][0];
    altimeterSprite       = wideLow[1][0];
    flLeftTri             = wideLow[2][0];
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
    // Wide-object extension chain heads (amiga/wide_probe.gdb walks each chain's two control-word
    // pairs to check the extension's VSTOP and the chained gauge/triangle's VSTART behind it).
    { extern volatile uint32_t g_wideExtAddr[3];
      for (int s = 0; s < 3; s++) g_wideExtAddr[s] = (uint32_t)wideExt[s][0]->data(); }
    // Cockpit bitmap base — amiga/b2_probe.gdb dumps the dashboard PEN under the energy-gauge
    // column to tell "playfield hides the sprite" (non-zero pen, a priority question) apart from
    // "background shows it through" (pen 0 = COLOR00, which no BPLCON2 value can hide).
    { extern volatile uint32_t g_cockpitBmpAddr; g_cockpitBmpAddr = (uint32_t)cockpitBitmap->data; }
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
    mirrorSprite(wideLow[2][1], flLeftTri, 0);   // ch1's second chain: same X/Y (pixels come later)
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
    setSpritePriority(kSpritePriorityCockpit);   // the Standby priority (see setSpritePriority)

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
        // ch1's entry pointer is the wide-object extension, which is CHAINED in front of
        // flLeftTri in one chip buffer — the channel shows segment 3 across the viewport and
        // then the left band triangle at 172.  (ch3 still points straight at flRightTri.)
        flightCopper->buildLayout(*titleBitmap, *terrainBitmap, *cockpitBitmap,
                                  *flLeftPost, *wideExt[2][0], *flRightPost, *flRightTri, *nullSprite,
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
    for (int i = 0; i < 2; i++) {
        tunnelCopper[i] = new TunnelCopperList();
        if (tunnelCopper[i] && tunnelCopper[i]->data())
            tunnelCopper[i]->buildLayout(*titleBitmap, *tunnelBitmap, *cockpitBitmap,
                                         *leftPost, *rightPost, *energyIndicatorSprite, *nullSprite);
    }

    // Title Screen fixed copper list (attract/level-select/results); renderFrame installs it
    // during rsTitle.  Full-screen text bitmap, black COLBK, 4 cycling text pens.
    titleScreenCopper = new TitleScreenCopperList();
    if (titleScreenCopper && titleScreenCopper->data())
        titleScreenCopper->buildLayout(*titleScreenBitmap, *nullSprite);

#ifndef ROF_SKIP_BOOT_SCENES
    // Boot scenes 1 + 2 (Logo, Station cinematic): one tall 320x340 4bp GTIA-9 field bitmap
    // (54 KB chip) and one copper list, shared — the station decodes its whole 340-entry display
    // list into it and scrolls by moving the bitplane pointers; the logo uses rows 0..61.  The
    // layout is (re)built at each scene's entry in renderFrame, since the two differ in leading
    // blank lines, row count and palette hue.  Compiled out entirely by `make SKIPBOOT=1` (which
    // PROBES/FPSCOUNT imply), so the perf builds' chip footprint is unchanged.
    bootFieldBitmap = Bitmap::allocate(kW, kStationDLRows, 4, /*interleaved*/true);
    bootFieldCopper = new Gtia9CopperList();
    if (bootFieldCopper && !bootFieldCopper->data()) { delete bootFieldCopper; bootFieldCopper = nullptr; }
#endif

#ifdef ROF_FLIGHT_PROBE
    // BPLCON2 audit.  BPLCON2 (sprite-vs-playfield priority) is WRITE-ONLY hardware that persists
    // across copper lists, so a list that emits none inherits whatever ran before it — which is how
    // the throttle/energy gauge ended up in FRONT of the cockpit on every Standby entered after a
    // launch (TunnelCopperList leaves PFxP=4).  Standby/Doors/Title deliberately stay MOVE-free and
    // are driven by setSpritePriority's one-off CPU write instead, so this scan exists to keep that
    // split honest: it reads the list the copper really executes, not the constants in the source.
    // 0xFFFF = the list emits none (CPU-owned).  Read with amiga/title_start.gdb.
    {
        extern volatile unsigned short g_cl2Standby, g_cl2Doors, g_cl2Tunnel,
                                       g_cl2Planet, g_cl2Flight, g_cl2Title;
        struct { CopperList* cl; volatile unsigned short* out; } audit[] = {
            { standbyCopper,     &g_cl2Standby }, { doorsCopper[0],  &g_cl2Doors  },
            { tunnelCopper[0],   &g_cl2Tunnel  }, { planetCopper,    &g_cl2Planet },
            { flightCopper,      &g_cl2Flight  }, { titleScreenCopper, &g_cl2Title },
        };
        for (unsigned a = 0; a < sizeof(audit) / sizeof(audit[0]); a++) {
            *audit[a].out = 0xFFFFu;
            if (!audit[a].cl || !audit[a].cl->data()) continue;
            const uint32_t* d = audit[a].cl->data();
            for (uint32_t i = 0; i < 512u; i++) {
                if (d[i] == 0xFFFFFFFEu) break;                       // list terminator
                if ((d[i] >> 16) == 0x104u) { *audit[a].out = (unsigned short)(d[i] & 0xFFFFu); break; }
            }
        }
    }
#endif

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
        // FORWARD tunnel (kGtia10P*, the direct painter's default LUT) — the long-working mapping:
        // value-0 (exit-clear) -> spare pen7 (color07=black); value-8 (background/corner) -> pen0
        // (color00 = the tunnel corner $08D8, carried into the band); others straight through.
        // Use gh/gl so kDoorP below still sees the ORIGINAL ph/pl.
        uint8_t gh = ph ? ph : 7, gl = pl ? pl : 7;
        kGtia10P1[s] = (uint8_t)(((gh & 1) ? 0xF0u : 0u) | ((gl & 1) ? 0x0Fu : 0u));
        kGtia10P2[s] = (uint8_t)(((gh & 2) ? 0xF0u : 0u) | ((gl & 2) ? 0x0Fu : 0u));
        kGtia10P3[s] = (uint8_t)(((gh & 4) ? 0xF0u : 0u) | ((gl & 4) ? 0x0Fu : 0u));
        // BOOST reverse tunnel (kGtia10BoostP*, the boost cinematic only) — additionally move value-2
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
        // GTIA-9 (the boot scenes): nibble IS the pen, so plane k simply takes bit k of each
        // nibble, spread over that nibble's 4 lores px.  No remap — see kGtia9P.
        for (int p = 0; p < 4; p++)
            kGtia9P[p][s] = (uint8_t)(((ph >> p) & 1 ? 0xF0u : 0u) | ((pl >> p) & 1 ? 0x0Fu : 0u));
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

// GTIA nibble -> Amiga pen, read back out of the live owner's LUT rather than re-deriving the
// remap: the LUT is what the copper palette is wired to (plane k's byte carries pen bit k), so
// this cannot drift from it, and it follows the boost's value-2/value-8 moves automatically.
// The FORWARD and BOOST tunnels differ in NOTHING ELSE — same writers, same geometry, same
// prev-independent masks — so the whole direction split is this table choice.
uint16_t RescueOnFractalus::tunnelPen(uint8_t colour) const
{
    const uint8_t pat = (uint8_t)((colour << 4) | colour);
    const uint8_t* t1 = (tunnelOwner == kTunnelOwnerBoost) ? kGtia10BoostP1 : kGtia10P1;
    const uint8_t* t2 = (tunnelOwner == kTunnelOwnerBoost) ? kGtia10BoostP2 : kGtia10P2;
    const uint8_t* t3 = (tunnelOwner == kTunnelOwnerBoost) ? kGtia10BoostP3 : kGtia10P3;
    return (uint16_t)(((t1[pat] & 0x80u) ? 1u : 0u) |
                      ((t2[pat] & 0x80u) ? 2u : 0u) |
                      ((t3[pat] & 0x80u) ? 4u : 0u));
}

// tunnelPaintBegin: arm the direct painter for the FORWARD launch tunnel and prime the bitmap.
// Called (via platform_tunnel_rings_begin) immediately BEFORE tunnel_prebuild_rings' pre-draw —
// the counterpart of the boost's arm-at-copper-install, moved earlier because the forward
// pre-draw happens during standby construction, long before any tunnel copper exists.
//
// ⚠ Prime to the field's BACKGROUND pen, not to whatever is there.  The painter writes only the
// rectangles/columns the 6502 draws; the decode it replaces also wrote everything BETWEEN them,
// which is the untouched field value $88 = GTIA value 8.  For the forward LUT value 8 -> pen 0
// (kGtia10P*[0x88] == 0: the tunnel corner colour $08D8 lives in color00), so the prime is a
// plain blitter clear — but derive it from the LUT anyway so it cannot drift from the palette.
// The previous owner may have been the BOOST, whose ink and background pen are both different,
// so this must repaint the WHOLE bitmap, not just the rectangles.
void RescueOnFractalus::tunnelPaintBegin()
{
#ifdef ROF_FLIGHT_PROBE
    // The one thing that can go wrong here is repainting a bitmap that is ON SCREEN — the post-boost
    // pre-build fires ~40 frames after the reverse ring ends, and the T6 handoff hold displays
    // tunnelBitmap for part of that window.  Record whether the tunnel copper was still installed
    // at each claim; every row must read tunInst=0.  (Measured, not reasoned: this is the trap
    // commit 8339175 fixed for the decode and the paint cannot be deferred the way the decode was.)
    { extern volatile unsigned char g_tpbN, g_tpbTunInst[4], g_tpbCopper[4];
      extern volatile unsigned short g_tpbVbi[4];
      extern volatile unsigned char g_liveCopper;
      if (g_tpbN < 4) { g_tpbTunInst[g_tpbN] = tunnelCopperInstalled ? 1 : 0;
                        g_tpbCopper[g_tpbN] = g_liveCopper;
                        g_tpbVbi[g_tpbN] = platform_frame_count(); }
      g_tpbN++; }
#endif
#ifdef ROF_TUNNEL_DIFF
    // Check the finished pre-draw image on the next render.  The claim happens BEFORE the draw and
    // the whole pre-build is one straight-line block with no ds_frame, so by the next render the 43
    // rectangles + 3 guide columns are all in.  Both claims land on the standby idle, which is the
    // one place a ~40ms frame is harmless.  This is what covers the POST-BOOST pre-build — the
    // image the 2nd launch's doors open onto, and the exact thing commit 8339175 had to fix.
    tunnelDiffPending = true;
#endif
    tunnelOwner = kTunnelOwnerForward;
    if (!tunnelBitmap) return;
    const uint16_t bgPen = tunnelPen(8);
    if (bgPen == 0) {
        AmigaHardware::blitterClear((uint16_t*)tunnelBitmap->data, 60, kTerrainHeight, 0);
        AmigaHardware::blitterWait();
    } else {
        tunnelBitmap->fillColor(0, 0, kW, kTerrainHeight, bgPen);
    }
}

// drawTunnelColumns: the tail of draw_frame_pattern_seq (draw_frame_guide_columns) plots three
// FULL-HEIGHT columns through plot_pixel_masked_core — a field writer OUTSIDE the span loop, so
// the rectangle hook never sees it.  They sit at the vanishing point and are part of the static
// image, so the painter has to reproduce them or the centre of the tunnel comes out empty.
// Same 4-px nibble geometry as a rectangle's vertical edge, but each column picks its own parity
// (plot_pixel_masked_core is per-pixel; the xL-parity quirk is fill_vertical_span_core's alone).
void RescueOnFractalus::drawTunnelColumns(uint16_t rowBase, uint8_t colL, uint8_t colR,
                                          uint8_t colR1, uint8_t colour)
{
    if (tunnelOwner == kTunnelOwnerNone || !tunnelBitmap || rowBase >= 0x2000u) return;
    const uint16_t pen = tunnelPen(colour);
    const uint8_t cols[3] = { colL, colR, colR1 };
    for (int i = 0; i < 3; i++) {
        const int byteCol = (int)(cols[i] >> 1);
        if (byteCol < 4 || byteCol > 43) continue;
        const uint16_t x = (uint16_t)((byteCol - 4) * 8 + ((cols[i] & 1) ? 4 : 0));
        tunnelBitmap->fillColor(x, 0, 4, kTerrainHeight, pen);
    }
}

// ---- direct tunnel-ring painting (the ROF_TUNNEL_RECT hook) --------------------------------
// draw_symmetric_span_loop hands us each concentric rectangle it draws, so the rings are painted
// straight into tunnelBitmap instead of being decoded back out of the mem[] GTIA field afterwards.
// This is one hook for BOTH directions — the forward ring (draw_ring_frame_step) and the reverse
// one (step_accum_sub_7e) share that loop.
//
// A rectangle is four solid runs of ONE pen.  Nothing here needs the field's previous contents:
// the 6502's two mask tables ($66E9/$66FB, dumped 2026-08-10) are exactly "set this nibble to
// colour, preserve the other" — OR = colour<<4 / AND = (colour<<4)|$0F for an even column, OR =
// colour / AND = $F0|colour for an odd one — so every write is prev-independent.
//
// Geometry.  A field byte is 2 GTIA pixels and 8 Amiga pixels; the displayed window is field bytes
// 4..43 (the wide-field crop).  So field pixel column p -> Amiga x = ((p>>1) - 4) * 8 + (p&1)*4,
// and a field row is one bitmap row (the copper line-doubles the viewport).
//   horizontal edges: whole bytes [byteLo..byteHi], on rows rowTop AND rowBot
//   vertical edges:   one nibble (4 px) at columns xL and xR, rows rowBot..rowTop inclusive
// ⚠ rowBot ($009F) is the SMALL row index (screen top) and rowTop ($009E) the large one — the
// 6502's names are inverted with respect to the screen, and the span loop grows the rectangle by
// stepping rowTop up and rowBot down.
// ⚠ FAITHFUL QUIRK, do not "fix": fill_vertical_span_core picks its nibble mask from xL's parity
// and applies that SAME mask at both columns, so when xL and xR have different parity the right
// edge lands in the nibble xL selected, not its own.  Mirrored here.
void RescueOnFractalus::drawTunnelRect(uint16_t rowBase, uint8_t rowTop, uint8_t rowBot,
                                       uint8_t xL, uint8_t xR, uint8_t byteLo, uint8_t byteHi,
                                       uint8_t colour)
{
    // NOTE: the probes below run BEFORE the tunnelBitmap null check deliberately — a probe that
    // silently drops the very calls it is meant to account for is worse than no probe.
#ifdef ROF_FLIGHT_PROBE
    // Plumbing probe: does the hook fire, from which field, and with what geometry?
    { extern volatile unsigned long g_trCalls, g_trDoors, g_trRows[8], g_trCols[8], g_trPen[8];
      if (rowBase >= 0x2000u) g_trDoors++;
      else { if (g_trCalls < 8) {
                 g_trRows[g_trCalls] = ((unsigned long)rowTop << 8) | rowBot;
                 g_trCols[g_trCalls] = ((unsigned long)xL << 24) | ((unsigned long)xR << 16) |
                                       ((unsigned long)byteLo << 8) | byteHi;
                 g_trPen[g_trCalls]  = colour; }
             g_trCalls++; } }
    // §0a: call site x render phase, measured, plus the run timeline.  A "run" ends when the call
    // site changes or more than 8 vbi pass, so each pre-draw shows up as its own 43-rectangle burst
    // with the sub-phase state ($008D/$008E) it ran under.
    if (rowBase < 0x2000u) {
        extern volatile unsigned char g_trSrc;
        extern volatile unsigned long g_trBySrc[5], g_trPhase[15], g_trRunN;
        extern volatile unsigned char g_trRunSrc[TR_RUNS], g_trRunPhase[TR_RUNS],
                                      g_trRun8D[TR_RUNS], g_trRun8E[TR_RUNS];
        extern volatile unsigned short g_trRunVbi0[TR_RUNS], g_trRunVbi1[TR_RUNS], g_trRunCnt[TR_RUNS];
        const unsigned src = (g_trSrc < 5u) ? g_trSrc : 0u;
        const unsigned phase = rsBoostViewport ? 2u : (rsBoostReturn ? 1u : 0u);
        const unsigned short vbi = platform_frame_count();
        g_trBySrc[src]++;
        g_trPhase[src * 3u + phase]++;
        const unsigned long r = g_trRunN;              // index of the run in progress (r-1)
        const bool sameRun = r != 0 && r <= TR_RUNS &&
                             g_trRunSrc[r - 1] == (unsigned char)src &&
                             (unsigned short)(vbi - g_trRunVbi1[r - 1]) <= 8u;
        if (!sameRun) {
            g_trRunN = r + 1;
            if (r < TR_RUNS) {
                g_trRunSrc[r] = (unsigned char)src;
                g_trRunPhase[r] = (unsigned char)phase;
                g_trRun8D[r] = mem[0x008Du];
                g_trRun8E[r] = mem[0x008Eu];
                g_trRunVbi0[r] = vbi;
                g_trRunCnt[r] = 0;
            }
        }
        { const unsigned long i = g_trRunN - 1u;
          if (i < TR_RUNS) { g_trRunVbi1[i] = vbi; g_trRunCnt[i]++; } }
    }
#endif
    // Ring rectangles are emitted at four points in a session (measured 2026-08-10, the run
    // timeline in §0a of docs/boost-tunnel-direct-handoff.md) and BOTH directions are painted
    // now: the forward pre-build + descent under kTunnelOwnerForward, the boost's L_6047 pre-draw
    // + reverse groups under kTunnelOwnerBoost.  The owner is re-armed (and the bitmap re-primed)
    // at each direction's entry point, so a rectangle is only ever painted with the LUT of the
    // cinematic that is about to display it.
    if (tunnelOwner == kTunnelOwnerNone) return;
    if (!tunnelBitmap) return;
    if (rowBase >= 0x2000u) return;         // the $2000 door field keeps its own decode path

    const uint16_t pen = tunnelPen(colour);
    const int kRows = (int)kTerrainHeight;
    // Horizontal edges — whole bytes, clipped to the displayed window [4..43].
    int bLo = (int)byteLo, bHi = (int)byteHi;
    if (bLo < 4) bLo = 4;
    if (bHi > 43) bHi = 43;
    if (bLo <= bHi) {
        const uint16_t hx = (uint16_t)((bLo - 4) * 8);
        const uint16_t hw = (uint16_t)((bHi - bLo + 1) * 8);
        if ((int)rowTop < kRows) tunnelBitmap->fillColor(hx, rowTop, hw, 1, pen);
        if ((int)rowBot < kRows) tunnelBitmap->fillColor(hx, rowBot, hw, 1, pen);
    }
    paintVSpan(rowBot, rowTop, xL, xR, pen);   // vertical edges
}

// paintVSpan: the two 4-px vertical edges of a span pair, rows rowBot..rowTop inclusive.
// ⚠ FAITHFUL QUIRK, do not "fix": fill_vertical_span_core picks its nibble mask from xL's parity
// and applies that SAME mask at BOTH columns, so when xL and xR have different parity the right
// edge lands in the nibble xL selected, not its own.  Mirrored here.
void RescueOnFractalus::paintVSpan(uint8_t rowBot, uint8_t rowTop, uint8_t xL, uint8_t xR,
                                   uint16_t pen)
{
    if (rowBot > rowTop) return;
    const int kRows = (int)kTerrainHeight;
    int vy = (int)rowBot, vh = (int)rowTop - (int)rowBot + 1;
    if (vy >= kRows) return;
    if (vy + vh > kRows) vh = kRows - vy;
    const int nib = (xL & 1) ? 4 : 0;             // the quirk: xL's parity picks BOTH nibbles
    const int cL = (int)(xL >> 1), cR = (int)(xR >> 1);
#ifdef ROF_FLIGHT_PROBE
    // Beam race on the tall edges (see the g_tb* block up top).  Only meaningful while the copper
    // is showing tunnelBitmap — a paint into an off-screen bitmap cannot tear.
    const bool _tbOn = tunnelCopperInstalled;
    const unsigned short _tbIn = _tbOn ? rof_beam_line() : 0;
#endif
    if (cL >= 4 && cL <= 43)
        tunnelBitmap->fillColor((uint16_t)((cL - 4) * 8 + nib), (uint16_t)vy, 4, (uint16_t)vh, pen);
    if (cR >= 4 && cR <= 43)
        tunnelBitmap->fillColor((uint16_t)((cR - 4) * 8 + nib), (uint16_t)vy, 4, (uint16_t)vh, pen);
#ifdef ROF_FLIGHT_PROBE
    if (_tbOn) {
        const unsigned short out = rof_beam_line();
        const unsigned short d = (unsigned short)((out >= _tbIn) ? (out - _tbIn) : (out + 313u - _tbIn));
        // Painted rows vy..vy+vh-1 are Amiga scanlines kTerrainLine+vy .. kTerrainLine+vy+vh-1.
        const unsigned short y0 = (unsigned short)(kTerrainLine + vy);
        const unsigned short y1 = (unsigned short)(kTerrainLine + vy + vh - 1);
        g_tbCalls++;
        g_tbLinesSum += d;
        if (d > g_tbLinesMax) g_tbLinesMax = d;
        if (_tbIn < g_tbEntryMin) g_tbEntryMin = _tbIn;
        if (_tbIn > g_tbEntryMax) g_tbEntryMax = _tbIn;
        // Did the beam sweep through the painted rows while the paint was in flight?  O(1) on
        // purpose: this runs in the VBI ISR on the very path being measured, so a scan loop here
        // would be a probe that changes its own answer.  The beam covers lines [_tbIn, _tbIn+d]
        // mod 313; walking l up through the contiguous range [y0,y1] makes (l - _tbIn) mod 313
        // increase by one a step and wrap at most once, so the closest the beam gets is 0 when it
        // started inside the range and (y0 - _tbIn) mod 313 otherwise.
        // (subtract-or-add-313 rather than a modulo: the 68000 has no 32-bit divide)
        const unsigned short reach = (unsigned short)((y0 >= _tbIn) ? (y0 - _tbIn)
                                                                    : (y0 + 313u - _tbIn));
        const bool hit = (_tbIn >= y0 && _tbIn <= y1) || reach <= d;
        if (hit) {
            g_tbBeamIn++;
            if (g_tbN < TB_SAMP) {
                g_tbIn[g_tbN] = _tbIn; g_tbOut[g_tbN] = out;
                g_tbY0[g_tbN] = y0;    g_tbY1[g_tbN] = y1;  g_tbN++;
            }
        }
    }
#endif
}

// drawTunnelVSpan: the ROF_TUNNEL_VSPAN hook — one vertical span pair from plot_terrain_span, the
// third $1000 writer.  It both ERASES the static pre-draw (colour 8, straight after it) and draws
// one coloured pair per revealed row (emit_dl_coord_pairs tail-calls it during the reveal), so the
// painter is wrong in both directions without it.
void RescueOnFractalus::drawTunnelVSpan(uint16_t rowBase, uint8_t r0, uint8_t r1, uint8_t colL,
                                        uint8_t colR, uint8_t colour)
{
    if (tunnelOwner == kTunnelOwnerNone || !tunnelBitmap || rowBase >= 0x2000u) return;
    paintVSpan(r0, r1, colL, colR, tunnelPen(colour));
}

#ifdef ROF_TUNNEL_DIFF
// tunnelPaintDiff: the fidelity differential the whole direct-painter design is verified with
// (docs/boost-tunnel-direct-handoff.md §3).  Decode the live $1000 GTIA field through the CURRENT
// owner's LUT and diff it against what the painter actually left in tunnelBitmap, over the rows
// the copper is showing from that bitmap ([K, 85-K]; K = 0 for the forward descent's single
// full-height band).  Race-aware by construction — the VBI writes the field while this reads it,
// so a small transient count is expected; a systematic one is a bug.
//   EXTRA   = bitmap has ink where the field is pure background ($88) -> painted something the
//             field does not have, or stale ink that was never primed away.
//   MISSING = the field has content the painter never saw -> an unhooked field writer.
// Run this BEFORE looking at the screen: it caught every wrong turn in this work.
void RescueOnFractalus::tunnelPaintDiff(uint16_t K)
{
    if (!tunnelBitmap) return;
    const bool boost = (tunnelOwner == kTunnelOwnerBoost);
    const uint8_t* t1 = boost ? kGtia10BoostP1 : kGtia10P1;
    const uint8_t* t2 = boost ? kGtia10BoostP2 : kGtia10P2;
    const uint8_t* t3 = boost ? kGtia10BoostP3 : kGtia10P3;
    const uint8_t* bp = (const uint8_t*)tunnelBitmap->data + (uint32_t)K * 120u;
    g_dpFrames++;
    for (int row = K; row <= 85 - (int)K; row++, bp += 120) {
        const uint8_t* src = (const uint8_t*)(const void*)&mem[0x1000u + row * 46 + 4];
        for (int b = 0; b < 40; b++) {
            const uint8_t sb = src[b];
            g_dpBytes++;
            if (bp[b] != t1[sb] || bp[b + 40] != t2[sb] || bp[b + 80] != t3[sb]) {
                if (!g_dpBad) {
                    g_dpFirst = ((unsigned long)row << 8) | (unsigned)b;
                    g_dpGot  = ((unsigned long)bp[b] << 16) | ((unsigned long)bp[b + 40] << 8) | bp[b + 80];
                    g_dpWant = ((unsigned long)t1[sb] << 16) | ((unsigned long)t2[sb] << 8) | t3[sb];
                }
                if (sb == 0x88u) g_dpExtra++; else g_dpMissing++;
                g_dpCol[b]++;
                g_dpBad++;
            }
        }
        // Row-42 side-by-side snapshot — reading the bytes beats inferring from counters.
        if (row == 42 && !g_dpSnapDone && K <= 20) {
            for (int i = 0; i < 40; i++) { g_dpSrc42[i] = src[i]; g_dpB1[i] = bp[i]; }
            g_dpSnapDone = 1;
        }
    }
}
#endif

// decodeBoostStars: convert the $2000 GTIA-10 starfield into viewportBitmap for the boost
// cinematic.  This is the ONLY decode left in the reverse cinematic — the rings are painted
// straight into tunnelBitmap by drawTunnelRect, and the copper picks between the two bitmaps per
// band, so the reveal itself costs nothing and the $1000 field is never read back.
//
// fill_region_2000 is the sole $2000 writer and re-fills only twice per cinematic, so the caller
// gates this on g_boostStarsDirty.  That is a CONTENT-change gate on a single-writer field, not
// the per-frame decode-on-change gate that rendered a bowtie (docs/boost-cinematic-plan.md item 2):
// there is no multi-writer mid-update state here to freeze.
void RescueOnFractalus::decodeBoostStars()
{
    if (!viewportBitmap) return;
#ifdef ROF_FLIGHT_PROBE
    unsigned long _b0 = rof_subclock(), _bi = g_isrBeamLines;
#endif
    uint8_t* d = (uint8_t*)viewportBitmap->data;
    // +4: wide-field crop.  base+4 is EVEN but not always 4-aligned (stride 46), which is all a
    // 68000 long access needs — it faults on ODD only; 4-alignment is a 68020+ perf matter.
    const uint8_t* src = (const uint8_t*)(const void*)&mem[0x2000u + 4];
    for (int row = 0; row < (int)kTerrainHeight; row++, d += 120, src += 46) {
        for (int b = 0; b < 40; b++) {
            const uint8_t s = src[b];
            d[b] = kGtia10BoostP1[s]; d[b + 40] = kGtia10BoostP2[s]; d[b + 80] = kGtia10BoostP3[s];
        }
    }
    // The boost has just filled viewportBitmap with something renderViewportModeD did not put
    // there, so make the next stars/planet decode see a base mismatch and full-clear first.
    viewportLastBase = 0x2000u;
#ifdef ROF_FLIGHT_PROBE
    g_bStarTicks += (rof_subclock() - _b0) - (g_isrBeamLines - _bi);
#endif
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
// Knock palette liveness (OPEN #2): how many knock render frames actually MOVED a copper colour
// slot once the light path started refreshing them.  0 across a whole knock = the pens are static
// through the scare and the refresh is a correctness-only no-op; >0 = the frozen palette really was
// showing the creature in stale colours.  g_alPenPub = the pens as last PUBLISHED to the copper
// (Atari values), the counterpart to g_alPen[] which samples the live mem[] cells.
// g_alPenCalls makes the 0 READABLE: a refresh that never ran also reports 0 changes.  Only
// (calls == knock frames && chg == 0) means "the palette really is static through the scare".
extern "C" volatile unsigned long  g_alPenChg = 0;
extern "C" volatile unsigned long  g_alPenCalls = 0;     // knock frames that reached updateFlightCopper
extern "C" volatile unsigned char  g_alPenPub[6] = {0};  // [0]$DA [1]$DB [2]$DC [3]$DD [4]$D8 [5]$D4
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
    // Fill the clear's shadow with the deferred sprite work (altimeter pair + artificial horizon):
    // pure CPU on sprite buffers, no dependency on this blit, so the drain below drops toward 0.
    buildFlightSpritesEarly();
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
    //
    // The sky fill is now IN FLIGHT and nothing below needs it until the band overlay, so spend the
    // wait on the rest of the deferred sprite work (scope P3 / viewport P3 / scanner dot) instead of
    // stalling on it.  g_fdScan measures exactly this slot (it read ~0 before).
    buildFlightSpritesLate();
    // ---- windscreen band, STEP 1: refresh the decode cache (still in the sky fill's shadow) ----
    // Hoisted above the wait deliberately.  This half of the band composite reads the mode-D band
    // field in mem[] and the per-half decode caches and writes ONLY those caches — it never touches
    // the terrain bitmap the blit is filling, so it has no dependency on the fill.  Step 2 (the
    // paint) does, and stays below.
    // ⚠ Why now: the three PMG run-scans that used to fill this shadow got ~11x cheaper, and the
    // measurement showed 13 of those saved t/it handed straight back as blitter stall (g_fdFill
    // 2 -> 15 t/it).  Cheapening CPU work inside a blit's shadow only pays for the part that
    // exceeded the blit; the rest has to be repaid by moving other independent work in.
    // Sampling the field a few hundred cycles earlier in the frame adds no exposure: the field is
    // ISR-written and a torn read was already documented as harmless and self-correcting (the
    // shadow holds exactly the bytes read, so the next frame re-decodes).
    const unsigned fieldHalf = g_flightRenderHalf ? 0x30u : 0x00u;
    const uint8_t* srow = (const uint8_t*)mem + 0x1074 + fieldHalf + 43 * 96;
#ifdef ROF_BAND_VERIFY
    // Freeze the source so the cache path and the original per-byte composite below see identical
    // bytes even if the ISR fires between them (see the compare block at the end).
    static uint8_t bvField[4 * 96] __attribute__((aligned(4)));
    for (int r = 0; r < 4; r++)
        for (int b = 0; b < 40; b++) bvField[r * 96 + b] = srow[r * 96 + b];
    srow = bvField;
#endif
#ifdef ROF_BAND_SHAPE
    // Is the band field worth re-compositing every frame?  Compare it against a per-HALF shadow
    // (the two double-buffer halves alternate, so one shadow would read as "all changed" every
    // frame) and tally changed bytes + how many take the rare overwrite (ow != 0) path.
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
#endif
    // Every per-half base is hoisted to a running pointer and every walk is an autoincrement: the
    // first cut of this indexed the caches as s_bandXc[hf][row*40+b], which put a 2D address
    // computation (and a row*40) inside a 40-iteration loop and measured almost no better than the
    // per-byte decode it replaced (39 -> 36 ticks).  See the "pointer-walk with autoincrement,
    // never multiply+index in a loop" rule in CLAUDE.md.
    const unsigned hf = g_flightRenderHalf ? 1u : 0u;
    uint32_t* shad = s_bandShadow[hf];                    // 10 longs/row
    uint32_t* p3c  = s_bandP3c[hf];                       // 10 longs/row
    uint8_t*  p1c  = s_bandP1c[hf];                       // 40 bytes/row
    uint8_t*  p2c  = s_bandP2c[hf];
    uint8_t*  owc  = s_bandOWc[hf];
    signed char* owLo = s_bandOwLo[hf];
    signed char* owHi = s_bandOwHi[hf];
    // Refresh the decode cache for whatever the ISR changed since this half's last frame (typically
    // nothing, or one long in row 45).  Compared as longs; decoded byte-wise.
    {
        const uint8_t* fs = srow;
        uint32_t* sh = shad; uint32_t* p3w = p3c;
        uint8_t* p1w = p1c; uint8_t* p2w = p2c; uint8_t* oww = owc;
        for (int row = 0; row < 4; row++, fs += 96, sh += 10, p3w += 10,
                                         p1w += 40, p2w += 40, oww += 40) {
            const volatile uint32_t* f4 = (const volatile uint32_t*)fs;
            // The scan for "did anything change" is split off from the decode, because the two
            // want opposite things from the register allocator.  Fused, the loop juggled six
            // pointers, so GCC strength-reduced them to one index plus six bases and every access
            // became `(0,An,Dn.L)`: 90 cycles to compare one unchanged long, x40/frame, on a path
            // that in half of all frames finds nothing at all.  Split, the scan holds only two.
            {
                const volatile uint32_t* f = f4;
                const uint32_t* s = sh;
                const uint32_t* const sEnd = sh + 10;
                // Three things had to line up to get 46 cycles a long out of this, and each was
                // worth reading the disassembly for:
                //  * POST-increment BOTH pointers and make the exit test a pointer compare.
                //    Incrementing `s` in the while condition instead (the obvious spelling) kept
                //    GCC on `move.l (a0),d0` + two `addq.l #4` = 70.
                //  * the trip count is the constant 10, so `#pragma GCC unroll` removes the loop
                //    entirely; GCC then addresses with `(d16,An)` and there is no bookkeeping at
                //    all — `move.l 4(a2),d0` 16 + `cmp.l 4(a3),d0` 18 + a never-taken `bne.w` 12.
                //  * the scan needs no index, because the decode below rescans from g=0.  All
                //    this has to report is "something differs".
                #pragma GCC unroll 10
                do { if (*f++ != *s++) goto rowDirty; } while (s != sEnd);
                continue;                                 // row unchanged — the common case
            rowDirty: ;
            }
            for (int g = 0; g < 10; g++) {
                const uint32_t fv = f4[g];
                if (fv == sh[g]) continue;
                sh[g] = fv;
                uint8_t* const p3b = (uint8_t*)p3w;
                const int k0 = g * 4;
                for (int k = k0; k < k0 + 4; k++) {       // re-decode just this long's 4 bytes
                    const uint8_t v = fs[k];
                    p3b[k] = kBandP3[v]; p1w[k] = kBandP1[v];
                    p2w[k] = kBandP2[v]; oww[k] = kBandOW[v];
                }
            }
            {                                             // re-derive this row's ow!=0 range
                // Unconditional now: reaching here means the scan saw a difference.  It may have
                // healed by the time the decode re-reads (the field is ISR-written), in which case
                // this re-derives the same lo/hi and bumps a version nobody's output depends on —
                // idempotent, and cheaper than carrying a flag through the loop above.
                int lo = 40, hi = -1;
                for (int b = 0; b < 40; b++) if (oww[b]) { if (b < lo) lo = b; hi = b; }
                owLo[row] = (signed char)lo; owHi[row] = (signed char)hi;
                s_bandP3Ver[hf][row]++;                   // this row's decoded plane3 moved on
            }
        }
    }
    FD_LAP(g_fdScan);                                        // = the late sprite slot
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
        if (!g_objTouchOvf) {
            // Normal path: apply exactly the bytes the writers reported (~10/frame), no search.
            for (int i = 0; i < g_objTouchN; i++) {
                const unsigned off = g_objTouch[i];
                bp[off] |= s_flightObjP1[off];
                s_flightObjP1[off] = 0;
            }
        } else {
            // Overflow fallback: the writers kept the bounding box up to date, so walk that.
            const int cl = g_objColLo, n = g_objColHi - cl + 1;
            uint8_t* d = bp            + kRow120[g_objRowLo] + cl;  // plane1, walked +120/scanline
            uint8_t* s = s_flightObjP1 + kRow120[g_objRowLo] + cl;  // scratch (same base offset)
            for (int sc = g_objRowLo; sc <= g_objRowHi; sc++, d += 120, s += 120) {
                for (int b = 0; b < n; b++) { if (s[b]) { d[b] |= s[b]; s[b] = 0; } }
            }
        }
        g_objRowLo = 47; g_objRowHi = -1;                       // range consumed
        g_objColLo = 40; g_objColHi = -1;
        g_objTouchN = 0; g_objTouchOvf = 0;
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
        uint8_t* vrow = bp + 43 * 120;
#ifdef ROF_BAND_VERIFY
        // In-process differential for the cached band composite (make BAND_VERIFY=1 +
        // amiga/band_verify.gdb).  This is a RENDERING change, and rendering cannot be judged from
        // a headless run (the remote debugger greys the display), so prove byte-identity instead:
        // snapshot the band rows, run the cache path, stash its output, restore, run the ORIGINAL
        // per-byte composite (which stays LIVE, as the edge-plot verify keeps its C reference live),
        // then compare.  g_bandMismatch must be 0.  (The SOURCE freeze that keeps both passes on
        // identical bytes now happens up in step 1, where srow is set — see there.)
        static uint8_t bvSnap[4 * 120], bvNew[4 * 120];
        for (int i = 0; i < 4 * 120; i++) bvSnap[i] = vrow[i];
#endif
#ifdef ROF_BAND_SHAPE
        BS_RESET();     // exclude step 1 (hoisted above the fill wait) from the paint's own lap
#endif
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
            // RMW a LONG at a time over the ow range rounded out to whole 4-byte groups.  Bytes the
            // rounding pulls in are a genuine no-op, so this is byte-identical to the per-byte
            // version it replaces (which cost 146 cycles a byte — every access an indexed
            // `(0,An,Dn.L)`, see the disassembly): kBandOW = kBandP1|kBandP2, so ow==0 implies
            // p1==p2==0 and (d & ~0)|0 == d.  Every lane's AND/OR/NOT is independent of the others,
            // so the uint32_t alias is byte-order neutral — the safe case of the endianness rule in
            // CLAUDE.md, like the plane3 long copy above.  No per-long zero test: the measured ow
            // map (BAND_SHAPE, amiga/band_shape.gdb) has no all-zero group inside any row's range
            // — row 44 `...11...`, row 45 `.111 1111 ... 111.`, row 46 `...11...` — so the test
            // would be pure cost.  Alignment: vrow = bp (AllocMem, 8-aligned) + 120*row, the plane
            // stride is 40, the cache row bases are multiples of 40 and g0 of 4.
            const int g0 = lo & ~3, gn = ((hi | 3) - g0 + 1) >> 2;
            const uint32_t* ow4 = (const uint32_t*)(owc + g0);
            const uint32_t* q1  = (const uint32_t*)(p1c + g0);
            const uint32_t* q2  = (const uint32_t*)(p2c + g0);
            uint32_t* e1 = (uint32_t*)(vrow + g0);
            uint32_t* e2 = (uint32_t*)(vrow + 40 + g0);
            const uint32_t* const ow4End = ow4 + gn;
            do {
                const uint32_t m = *ow4++;
                *e1 = (*e1 & ~m) | *q1++; e1++;               // salmon bar; terrain kept elsewhere
                *e2 = (*e2 & ~m) | *q2++; e2++;               // centre marker; terrain kept elsewhere
            } while (ow4 != ow4End);
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
    // Both boot INITADs now RUN (rof_boot_chain, src/rof_boot.c): it redoes the XEX load one
    // INITAD stage at a time, because rof.xex's later segments overwrite the earlier ones and
    // neither boot scene's data exists in a fully-loaded image (segment 16 buries the logo
    // code at $5000 and the station image staged at $4000).  `make SKIPBOOT=1` skips both and
    // enters straight at game_entry — byte-for-byte the pre-boot-scenes behaviour, which every
    // wall-clock-calibrated probe in amiga/ depends on (PROBES/FPSCOUNT imply it).
    // game_entry's 737-byte mega-init then establishes the genuine game state itself —
    // including calling $70E7 to start the Standby music (setting $00E7) at the right moment —
    // instead of inheriting it from a hand-crafted snapshot.
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

#ifndef ROF_SKIP_BOOT_SCENES
    // The genuine staged INITAD chain: place segments 1..7 -> Logo ($5000), 8..11 -> Station
    // ($1A97), 12..14 -> display init ($B800), 15..20 -> and return, leaving mem[] exactly as
    // the Atari's loader leaves it the instant before it calls INITAD $3CDE.  Blocks for the
    // duration of both scenes; each of their frame waits is a SPINWAIT hook driving one real
    // Amiga frame, so the display and audio animate throughout.
    rof_boot_chain(ROF_BOOT_LOGO);
#endif
    game_entry();     // $3CDE: mega-init -> game_main_loop (Standby -> cinematic -> flight); never returns
}

// updateBoostCinematicLatch(): decide WHICH boot_standby_launch_driver construction is the
// BOOSTERS return-to-mother-ship reverse cinematic.  Called once at the top of renderFrame(),
// before anything reads rsBoostReturn / the inline boostReturnRF copy.
//
// The gate used to be mission_event_flag $003A == $FF alone.  That flag is set when the mother
// ship arrives and STAYS $FF into the next level (see the rsBoostReturn comment in
// deriveRenderSignals), and the reverse cinematic is not a scene of its own — it IS
// boot_standby_launch_driver's paced construction, played with the boost LUT and shown instead of
// black-held.  So every LATER construction under the same $003A was also rendered as a reverse
// cinematic: the Title Screen (attract timeout / SELECT) re-enters game_main_loop, which calls
// boot_standby_launch_driver afresh, and that rebuild came up through the boost branch — reverse
// ring palette, and the $2000 starfield decode reading the LEVEL-NN door field instead (measured
// 2026-08-11: 33 consecutive frames at copper id 11 with $008D==0 && $008E==0, i.e. rsBoostViewport
// held true by its pre-ring clause).  That is the "post-mother-ship Standby -> Title -> START =
// broken cockpit / wrong tunnel / white rectangle in the stars viewport" bug.
//
// The cinematic is exactly the construction that FOLLOWS the ascent, so latch that:
//   ARM    in flight ($4FF5) once the mother ship has arrived ($003A==$FF) — the ascent is still
//          flying at that point, and the very next VVBLKI is the $52D7 construction.
//   CLEAR  on VVBLKI $53CC — game_main_loop's re-init (Title Screen, results card, restart).  The
//          genuine cinematic never passes through it: the level-clear handoff goes straight from
//          the flight loop back to boot_standby_launch_driver, $4FF5 -> $52D7.
//   CLEAR  on the g_doorFieldReady 0->1 edge — construction complete, which is already the T6
//          handoff hold's release point, so the cinematic ends exactly where it did before.
void RescueOnFractalus::updateBoostCinematicLatch()
{
    const uint16_t vv = (uint16_t)(mem[0x0222] | (mem[0x0223] << 8));
    if (vv == 0x53CCu)                                boostCineLatch = false;
    else if (vv == 0x4FF5u && mem[0x003A] == 0xFFu)   boostCineLatch = true;
    else if (g_doorFieldReady && !latchPrevDoorRdy)   boostCineLatch = false;
    latchPrevDoorRdy = g_doorFieldReady;
}

// setSpritePriority(): one-off CPU write of BPLCON2 (sprite-vs-playfield priority) at a scene
// transition.  BPLCON2 is write-only hardware that PERSISTS across copper lists, and the Standby /
// Doors lists deliberately emit no MOVE for it — so whichever list ran last owned it.  After any
// launch that is TunnelCopperList's PFxP=4 (all sprites in front of the playfield), which put the
// throttle/energy gauge (sprite 2) ON TOP of the cockpit dashboard instead of behind it, on every
// Standby entered after a launch (user-reported 2026-08-11 on the post-mother-ship Standby → Title
// → START path).  It looked right only on a fresh boot, because initialize() had just written the
// Standby value by hand a moment earlier.
//
// It is called only at scene ENTRY (the copper-list install), so this is one write per transition,
// not per frame.  ⚠ Deliberately NOT cached against a "last value we wrote" shadow: the copper also
// writes this register, so any such shadow goes stale the moment a list moves it, and a stale
// shadow suppresses exactly the corrective write this exists to make.  One 16-bit store is far
// cheaper than reasoning about who wrote it last.
#ifdef ROF_FLIGHT_PROBE
// gdb breakpoint markers: BPLCON2 is write-only, and a CPU read of $DFF104 returns the FLOATING BUS
// (measured — ffff/7f81/6441, varying frame to frame), while gdb's read of the same address returns
// the real stored value (cross-checked against $DFF100 BPLCON0).  So the only way to see the live
// priority is to stop the emulator on a marker and let gdb read it.  TWO functions, not one with a
// tag argument: a breakpoint fires at function ENTRY, before the body can record which call it is,
// so a tag global reads one call stale.  See amiga/b2_probe.gdb.
extern "C" volatile unsigned long g_b2MarkPreN = 0, g_b2MarkPostN = 0;
extern "C" __attribute__((noinline)) void rof_b2_mark_pre(void)  { g_b2MarkPreN++;  }
extern "C" __attribute__((noinline)) void rof_b2_mark_post(void) { g_b2MarkPostN++; }
#endif

void RescueOnFractalus::setSpritePriority(uint16_t v)
{
    *bplcon2Pointer = v;
#ifdef ROF_FLIGHT_PROBE
    { extern volatile unsigned short g_cl2Cpu; extern volatile unsigned long g_cl2CpuN;
      g_cl2Cpu = v; g_cl2CpuN++; }
#endif
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
    updateBoostCinematicLatch();   // must precede every rsBoostReturn / boostReturnRF read below

    // ---- boot scenes 1 + 2 (Logo, Station cinematic) ------------------------------------------
    // FIRST, ahead of the black-until-ready hold below: these play while g_standbyRevealReady is
    // still 0 (it only latches when boot_standby_launch_driver has built the Standby), so the hold
    // would paint both of them solid black.  They are also completely self-contained — one GTIA-9
    // field bitmap and one copper list, no cockpit/title/viewport machinery — so they return
    // straight from here rather than falling through the whole per-frame body.
    if (g_bootScene != ROF_BOOTSCENE_NONE) { renderBootScene(); return; }
    // Past the boot scenes: this frame belongs to some other list (the black hold below installs
    // the EmptyCopperList, since the boot install cleared emptyCopperInstalled), so drop the boot
    // field's claim on the display — which is also stationVblankUpdate's guard.
    bootFieldCopperInstalled = false;
    bootFieldScene = ROF_BOOTSCENE_NONE;

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
    const bool boostReturnRF = (vvblkiRF == 0x52D7u) && (mem[0x003A] == 0xFFu) && boostCineLatch;
    const bool boostViewportCine = boostReturnRF && (mem[0x008D] != 0u || mem[0x008E] == 0u);
    // ...and NEITHER may the handoff window that follows the reverse tunnel ($008D==0 && $008E!=0,
    // the final next-level door build).  b61791e left that one black-holding on the grounds that it
    // IS the piecemeal door build the hold exists to mask — but render()'s T6 handoff hold (added
    // by d8d7c18 for exactly this transition) already covers it, and covers it better: it freezes
    // the last reverse-ring tunnel frame on screen, decodes nothing, and animates only the band
    // recede, until staticStandby takes over on the g_doorFieldReady 0->1 edge with the finished
    // LEVEL-NN field.  Black-holding here pre-empts it entirely — measured, the handoff hold ran
    // ZERO frames and 13 frames of EmptyCopperList (~0.26 s, vbi 2658-2670) flashed between the
    // reverse tunnel and the standby.  Holding the tunnel image is also the faithful choice: on the
    // Atari, ANTIC keeps displaying the ring field through this window.  Nothing is black there.
    // ⚠ Gated on the tunnel copper actually being LIVE.  With no reverse-tunnel frame to freeze
    // there is nothing seamless to show, and falling through would put a stale list on screen — so
    // a return that somehow reaches here without one still black-holds.
    const bool boostReturnHandoff = boostReturnRF && tunnelCopperInstalled && !g_doorFieldReady;
    if (emptyCopper && !g_standbyRevealReady && !boostViewportCine && !boostReturnHandoff) {
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
        // Black frames taken during the RETURN to the mother ship specifically — the seam the user
        // sees between the reverse tunnel ending and the standby appearing.  g_blackHoldFrames
        // alone cannot answer this: it also counts the initial boot build, which is legitimate.
        if (boostReturnRF) {
            if (!g_brBlackFrames) {
                g_brBlackFirstVbi = platform_frame_count();
                g_brBlack8D = mem[0x008D]; g_brBlack8E = mem[0x008E];
                g_brBlackDoorRdy = g_doorFieldReady;
                g_brBlackTunInst = tunnelCopperInstalled ? 1 : 0;
            }
            g_brBlackFrames++;
            g_brBlackLastVbi = platform_frame_count();
        }
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
    //
    // ...but the COLOURS are NOT stable across the pause and must keep tracking mem[].  The creature
    // is a mode-D BITMAP drawn into the viewport field, so on the Atari it is painted in the viewport
    // pens — and DLI $4A1F reloads those from $00DA/$00DB/$00DC/$00DD (COLPF1/COLPF2/COLBK/COLPF0)
    // on EVERY frame of the knock, exactly as in normal flight.  Skipping updateFlightCopper here
    // froze the whole flight palette at whatever the last pre-knock frame published, so any pen the
    // rescue/knock moves was simply not shown — the creature kept the pre-knock terrain palette.
    // (This is also where the "$0047 attack colour" actually lands: $0047 is colpf0_value, which
    // set_colpf0_from_flag $47A3 stores into $00D8 = the top-bar/message text colour, read below as
    // titlePf0.  It never reaches the viewport pens on the Atari — the DLI overwrites COLPF0 from
    // $00DD every frame — so there is nothing to wire from $0047 to the viewport.)
    // Cost is ~15 mem[] byte reads plus poke-on-change: nothing against the ~80 ms creature draw,
    // and unlike deriveRenderSignals() (six sprite rebuilds) it does no per-frame recomputation.
    if (mem[0x0632] && mem[0x003E] != 0 && mem[0x003D] >= 3) {
        if (flightCopper && flightCopperInstalled) {
#ifdef ROF_FLIGHT_PROBE
            const uint16_t _p0 = flTerr0, _p1 = flTerr1, _p2 = flTerr2, _p3 = flTerr3,
                           _pt = flTitlePf0, _pb = flBand3;
#endif
            updateFlightCopper(false);
#ifdef ROF_FLIGHT_PROBE
            { extern volatile unsigned long g_alPenChg, g_alPenCalls;
              extern volatile unsigned char g_alPenPub[6];
              g_alPenCalls++;
              if (_p0 != flTerr0 || _p1 != flTerr1 || _p2 != flTerr2 || _p3 != flTerr3
                  || _pt != flTitlePf0 || _pb != flBand3) g_alPenChg++;
              g_alPenPub[0] = mem[0x00DA]; g_alPenPub[1] = mem[0x00DB];
              g_alPenPub[2] = mem[0x00DC]; g_alPenPub[3] = mem[0x00DD];
              g_alPenPub[4] = mem[0x00D8]; g_alPenPub[5] = mem[0x00D4]; }
#endif
        }
        renderFlightDirect();
        return;
    }

#ifdef ROF_FLIGHT_PROBE
    // Tunnel->stars stage timeline (see the g_se* block).  t0/v0 are taken unconditionally; they
    // are only KEPT if deriveRenderSignals turns out to have raised rsStars for the first time.
    static bool s_seSeen = false;
    const unsigned long  _se0 = s_seSeen ? 0 : rof_subclock();
    const unsigned short _seV = s_seSeen ? 0 : platform_frame_count();
#endif
    deriveRenderSignals();   // recompute the mem[]-derived render-gating signals for this frame
#ifdef ROF_FLIGHT_PROBE
    if (rsStars && !s_seSeen) {
        s_seSeen = true; g_seArmed = 1; g_seEntryVbi = _seV; g_seDrs = rof_subclock() - _se0;
        g_seWall = _se0;                            // absolute; turned into a delta at the install
    }
#endif
#ifdef ROF_TUNNEL_DIFF
    if (tunnelDiffPending) { tunnelDiffPending = false; tunnelPaintDiff(0); }   // finished pre-draw
#endif
    // ---- stars ENTRY: switch the copper FIRST, then do the entry work ------------------------
    // The planet copper is normally installed by render()'s staticPlanet branch, i.e. at the TAIL
    // of this frame — so the previous scene stays on screen for the whole of the stars-entry work.
    // At the forward launch that previous scene is the tunnel's last frame (outermost ring only),
    // and the entry work is ~2 frames on a 68000, so the ring image visibly froze there (a 68020
    // is fast enough to hide it).
    //
    // NOW is the faithful instant to switch.  rsStars keys on the 6502 having written VDSLST=$C2 +
    // DLIST=$3120 in boot_standby_launch_driver's stretch-C burst, and on the Atari ANTIC starts
    // displaying the stars DL from the very next vblank — it waits for nothing.  The $1000 field is
    // legitimately BLACK at that instant (the same burst zero_run's it), so a cleared viewport is
    // the correct first stars frame, not a compromise for our sake.
    //
    // Nothing has to be finished for this: setCopperList only writes COP1LC, so the list goes live
    // at the NEXT vblank, and perFrameWork's star-sprite rebuild + render()'s viewport decode —
    // both below, both in this same frame — still get there first.  The one thing that must be
    // right BEFORE the list goes live is the star SPRxPT operands (the copper reads them at
    // scanline 16): starWindow still holds the previous pass's value here, so publish window 0,
    // which is exactly what buildStarSprites resets it to a few lines below.
    // The guard is exactly "no branch below can pre-empt render()'s staticPlanet this frame":
    // staticTitle needs VVBLKI $53CC (rsStars needs $52D7) and staticStandby needs !rsViewport
    // (rsStars implies it), so only the two boost branches can — the reverse-tunnel viewport and
    // the T6 handoff hold.  (Historically this could not be a plain !rsBoostReturn, because
    // mission_event_flag $003A stays $FF into the NEXT level and would switch the fix off for every
    // launch after a mother-ship return.  boostCineLatch now scopes rsBoostReturn to the one
    // cinematic, so the two forms agree — the explicit pair is kept as documentation of the intent.)
    const bool boostOwnsDisplay = rsBoostViewport || (rsBoostReturn && !g_doorFieldReady);
    if (rsStars && !boostOwnsDisplay && !planetCopperInstalled && planetCopper && viewportBitmap) {
        // Kick the viewport clear on the blitter now, and ONLY when renderViewportModeD is going
        // to take its full-redecode path — its `full` test, evaluated here.  If it were to take
        // the incremental path instead it would never consume viewportClearKicked, and the clear
        // would wipe content nothing repaints.
        if ((viewportForceFull || viewportLastBase != 0x1000u) && !viewportClearKicked) {
            AmigaHardware::blitterClear((uint16_t*)viewportBitmap->data, 60, 47, 0);
            viewportClearKicked = true;
        }
        for (int i = 0; i < 6; i++) planetCopper->setStarOperand(i, starRing[i]);
        updatePlanetCopper(true);
        AmigaHardware::setCopperList(*planetCopper, false);
        planetCopperInstalled = true;
        standbyCopperInstalled = false; flightCopperInstalled = false;
        tunnelCopperInstalled  = false; titleScreenCopperInstalled = false;

#ifdef ROF_FLIGHT_PROBE
        if (!g_planetInstVbi) g_planetInstVbi = platform_frame_count();
#endif
    }
#if defined(ROF_FLIGHT_PROBE)
    // Diagnose why renderFlightDirect isn't reached during the knock: is renderFrame even entered
    // ($0632), what is VVBLKI, and is rsFlight/rsViewport true at that moment?
    { extern volatile unsigned long g_alRF; extern volatile unsigned short g_alVV;
      extern volatile unsigned char g_alRFfl, g_alRFvw;
      if (mem[0x0632]) { g_alRF++; g_alVV = (unsigned short)(mem[0x0222] | (mem[0x0223] << 8));
                         g_alRFfl = rsFlight ? 1 : 0; g_alRFvw = rsViewport ? 1 : 0; } }
#endif
    // (No tunnel decode here any more.  BOTH tunnel directions are painted straight into
    // tunnelBitmap as the 6502 draws them — drawTunnelRect / drawTunnelColumns / drawTunnelVSpan —
    // so the $1000 GTIA ring field is never read back and there is no dirty flag to service.
    // What used to live here: a full-field decode at the standby reveal plus a four-strip band
    // decode per descent step, both gated by a boostOwnsTunnel test that existed only to keep the
    // forward decode off the bitmap the boost was displaying.  Ownership is now explicit
    // (tunnelOwner), set at each direction's entry point, so the gate went with the decode.)
#ifdef ROF_FLIGHT_PROBE
    extern volatile unsigned long g_rPerFrame, g_rRenderFn;
    const bool _profR = rsFlight;
    unsigned long _p0 = _profR ? rof_subclock() : 0, _pi = _profR ? g_isrBeamLines : 0;
    const unsigned long _sePfw0 = g_seArmed ? rof_subclock() : 0;
#endif
    perFrameWork();
#ifdef ROF_FLIGHT_PROBE
    if (g_seArmed) g_sePfw = rof_subclock() - _sePfw0;
    if (_profR) { g_rPerFrame += (rof_subclock() - _p0) - (g_isrBeamLines - _pi);
                  _p0 = rof_subclock(); _pi = g_isrBeamLines; }
    const unsigned long _seRen0 = g_seArmed ? rof_subclock() : 0;
#endif
    render();
#ifdef ROF_FLIGHT_PROBE
    if (g_seArmed) { g_seRender = rof_subclock() - _seRen0; g_seTail = rof_subclock(); }
#endif
    // Safety net for the deferred flight sprite builders: renderFlightDirect normally runs them
    // inside its two blitter shadows, but it has three early-return paths (no bitmaps / the
    // rescue-figure pause / a frame with no fresh terrain).  Run whatever it skipped, so the
    // sprites still update exactly once per frame on those frames too.
    buildFlightSpritesFlush();
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
#ifdef ROF_FLIGHT_PROBE
        { extern volatile unsigned char g_liveCopper; g_liveCopper = 1; }
#endif
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
    if (rsBoostViewport && tunnelCopper[0]) {
        // The rings and the stars live in SEPARATE bitmaps and the copper picks between them per
        // band (setRevealBands), so nothing is composited and the $1000 ring field is never
        // decoded: draw_symmetric_span_loop hands each ring rectangle straight to drawTunnelRect.
        //   tunnelBitmap   = the rings, painted directly, shown only inside the reveal band.
        //   viewportBitmap = the $2000 starfield, decoded twice per cinematic.  It is genuinely
        //     free here — renderViewportModeD is skipped by boostOwnsTunnel and the Standby door
        //     field decodes into doorScrollBitmap.
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
            // ("LEVEL NN") content and would decode as a garbage flash.  Clear the STAR bitmap
            // instead (→ pen0 = color00 = $0071, the salmon fade bg) and skip this frame's decode;
            // the next frame decodes the ready starfield.
            AmigaHardware::blitterClear((uint16_t*)viewportBitmap->data, 60, kTerrainHeight, 0);
            // ⚠ Prime the RING bitmap to the field's BACKGROUND pen, NOT pen 0.  The painter writes
            // only rectangles; the decode it replaces also wrote everything BETWEEN them, which is
            // the untouched field value $88 = GTIA value 8.  Priming to 0 left ~6% of the revealed
            // band wrong and read on screen as teal "gates opening" instead of a rectangle growing
            // from the centre (measured 2026-08-10).  Derive the pen from the LUT so it follows the
            // boost's value-8 -> color02 remap instead of hardcoding it.  (Take the owner first —
            // tunnelPen() reads it to pick the LUT.)
            tunnelOwner = kTunnelOwnerBoost;   // from here every ring rectangle takes the boost LUT
            const uint16_t bgPen = tunnelPen(8);
            AmigaHardware::blitterWait();
            tunnelBitmap->fillColor(0, 0, kW, kTerrainHeight, bgPen);
            tunnelCopperInstalled = true;
            g_boostStarsDirty = 1;          // force the first stars decode once the field is ready
        } else if (g_boostStarsDirty) {
            // fill_region_2000 is the sole $2000 writer and re-fills the starfield only twice per
            // cinematic (the fade in between is palette-only, read live by the copper), so decode
            // only when the content actually changed.
            decodeBoostStars();
            g_boostStarsDirty = 0;
#ifdef ROF_FLIGHT_PROBE
            { extern volatile unsigned long g_bStarDec; g_bStarDec++; }
#endif
        }
#ifdef ROF_TUNNEL_DIFF
        if (mem[0x008D] != 0u) tunnelPaintDiff(boostRevealK());   // §2: prove it numerically first
#endif
        showTunnelCopper();     // reveal band K + palette, into the back buffer, swapped at vblank
        standbyCopperInstalled = false; planetCopperInstalled = false;
        flightCopperInstalled = false; titleScreenCopperInstalled = false;
#ifdef ROF_FLIGHT_PROBE
        { extern volatile unsigned char g_liveCopper; g_liveCopper = 11; }   // 11 = boost REVERSE tunnel
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
    // In-place level cycle (post-mother-ship SELECT): both the "elevator" door scroll (level<max)
    // and the intro_screen_build_seq fade-rebuild WRAP (level>=max) rebuild the $2000 door field
    // WITHOUT re-entering boot_standby_launch_driver, so they run with g_doorFieldReady cleared (at
    // L_6332) while g_standbyRevealReady stays LATCHED.
    //
    // ⭐ That PAIR is what identifies this window, and it is the whole gate.  Every fresh
    // construction clears BOTH flags at boot_standby_launch_driver entry (rof_native.c ~$5F1D) and
    // re-latches both together at construction-done ($6118); L_6332 is the ONLY site that clears
    // the door flag alone.  So `revealed && !doorFieldReady` cannot be the boot build, a post-
    // BREAK/crash rebuild, or the boost reverse-tunnel handoff below (all of which have
    // g_standbyRevealReady == 0) — it is exactly the in-place cycle.
    //
    // The gate USED to be `rsBoostReturn && ... && standbyCopperInstalled && !tunnelCopperInstalled`,
    // borrowing the mother-ship flag $003A==$FF as a "we came back from a launch" proxy.  97a6b70
    // then correctly narrowed rsBoostReturn with boostCineLatch to the ONE construction after the
    // ascent — which silently switched this branch OFF, since the SELECT cycle runs long after the
    // latch releases.  Every in-place level change then fell through to the forward-launch DOORS
    // copper below, which clears standbyCopperInstalled — the gate on doorScrollVblankUpdate — so
    // the elevator scroll stopped happening at all (measured: 18 in-place cycles, ONE setTerrainRuns
    // rewrite, live copper id 6 = doors).  Derive the window from the state that DEFINES it, never
    // from a flag that merely co-occurs with it.
    //
    // Keep the standby copper live and ANIMATE it rather than freezing: poke the per-frame colours
    // so the WRAP's $0071 dark-green fade shows, and re-decode the door bitmap when the LEVEL digit
    // changes (g_doorScrollFieldDirty) so the fade-up reveals the new level.  The door-scroll case
    // spins in the main loop between its paced steps (doorScrollVblankUpdate drives the scroll
    // itself from the ISR), so this mostly refreshes colours + the decode at the step boundaries.
    // !rsLaunched keeps the branch out of a launch that escapes the cycle with the flag still clear
    // (L_62ee's `$0004 != 0` jump to L_634f skips the L_62f6 re-latch).
    const bool inPlaceLevelCycle = rsStandby && g_standbyRevealReady && !g_doorFieldReady
                                   && !rsLaunched && !rsBoostReturn;
    if (inPlaceLevelCycle && standbyCopper) {
        decodeDoorScrollDirty();
        // Normally already live (the cycle starts from the settled standby), so this is the poke
        // path.  Installing when it is NOT makes the branch self-healing instead of self-latching:
        // one frame that ever slips past it used to clear standbyCopperInstalled and kill the
        // scroll for the rest of the cycle.
        if (!standbyCopperInstalled) {
            setSpritePriority(kSpritePriorityCockpit);
            updateStandbyCopper(true);
            AmigaHardware::setCopperList(*standbyCopper, false);
            standbyCopperInstalled = true;
            planetCopperInstalled = false; flightCopperInstalled = false;
            tunnelCopperInstalled = false; titleScreenCopperInstalled = false;
        } else {
            updateStandbyCopper(false);   // pokes terrain color00/03 = atariToOCS(mem[$0071]) fade ramp
        }
#ifdef ROF_FLIGHT_PROBE
        { extern volatile unsigned char g_liveCopper; g_liveCopper = 10; }   // 10 = in-place level cycle
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
        if (tunnelCopper[tunnelActive] && tunnelCopperInstalled) {
            uint16_t k = 8;
            for (uint16_t i = 0; i < 8; i++) if (mem[0x0C88 + i] == 0u) { k = i; break; }
            // Poked in place on the LIVE list: these are COLOUR moves, which are safe mid-frame
            // (a torn colour is invisible for one frame).  Only POINTER moves need the swap.
            tunnelCopper[tunnelActive]->setBandTopColor00(true, atariToOCS(mem[0x0071]));  // band top = dark green
            tunnelCopper[tunnelActive]->setBandReveal(k, atariToOCS(mem[0x08D8]));         // teal from row k down
        }
        return;
    }

    const bool staticStandby = standbyCopper && rsStandby && g_doorFieldReady
                               && !rsViewport && !rsLaunched && !rsBoostViewport;
    if (staticStandby) {
        if (!standbyCopperInstalled) {
            // Scene transition: reclaim BPLCON2 from whatever list ran last (see setSpritePriority).
#ifdef ROF_FLIGHT_PROBE
            { extern void rof_b2_mark_pre(void); rof_b2_mark_pre(); }     // gdb reads $DFF104 here:
#endif                                                                    //   what the scene inherited
            setSpritePriority(kSpritePriorityCockpit);
#ifdef ROF_FLIGHT_PROBE
            { extern void rof_b2_mark_post(void); rof_b2_mark_post(); }   // ...and here: after the write
#endif
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
    #ifdef ROF_FLIGHT_PROBE
            if (!g_planetInstVbi) g_planetInstVbi = platform_frame_count();
#endif
        } else {
            updatePlanetCopper(false);
        }
        standbyCopperInstalled = false;
        flightCopperInstalled = false;
        tunnelCopperInstalled = false; titleScreenCopperInstalled = false;
#ifdef ROF_FLIGHT_PROBE
        { extern volatile unsigned char g_liveCopper; g_liveCopper = 3; }
        if (g_seArmed) { g_seTail = rof_subclock() - g_seTail;
                         g_seWall = rof_subclock() - g_seWall; g_seArmed = 0; }
#endif
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
#ifdef ROF_FLIGHT_PROBE
        { extern volatile unsigned char g_liveCopper; g_liveCopper = 4; }
#endif
        return;
    }

    // Launch cinematic (the only phase left for the fixed Doors/Tunnel lists): the hangar
    // doors part (scene 4) then the tunnel fills the screen (scene 5).  Door-scroll progress
    // g2 = 0x2B - $008A grows 0 -> kTerrainHeight/2 as the doors open; once it reaches the
    // half-height the doors are fully open and the single full tunnel band takes over.
    const uint16_t half = (uint16_t)(kTerrainHeight / 2);
    const uint16_t g2   = rsLaunched ? (uint16_t)(0x2Bu - mem[MEM_terrain_scroll_counter]) : 0;
    if (tunnelCopper[0] && g2 >= half) {
        // ---- scene 5: tunnel descent (doors fully open) ----
#ifdef ROF_TUNNEL_DIFF
        // §2 differential, forward LUT, the whole 86-row band — but ONLY while the ring is
        // actually stepping ($0088 = vbi_flags, the descent gate).  ⚠ It costs ~10k volatile
        // mem[] reads a frame, which pushes render past one vblank; left running through the
        // door-open sweep the main loop's `while (mem[$0684] != $64)` polls step OVER their target
        // and spin forever (measured 2026-08-10 — looked exactly like a broken descent, was the
        // probe).  Hence `make TUNDIFF=1`, off even under PROBES.
        if (mem[0x0088] != 0u) tunnelPaintDiff(0);
#endif
        showTunnelCopper();          // one full-height ring band (setRevealBands K = 0)
        tunnelCopperInstalled = true;
#ifdef ROF_FLIGHT_PROBE
        { extern volatile unsigned char g_liveCopper; g_liveCopper = 5; }
#endif
    } else if (doorsCopper[0]) {
        // ---- scene 4: hangar doors parting (also the closed g2==0 first frame) ----
        // Populate the BACK buffer fully (geometry + colours), then swap it in — the swap
        // latches at the next vblank, so the displayed buffer is never modified mid-frame
        // (avoids the ±1px tunnel-reveal jitter an in-place poke of the live list causes).
        const uint8_t back = (uint8_t)(1 - doorsActive);
        updateDoorsCopper(doorsCopper[back]);
#ifdef ROF_FLIGHT_PROBE
        // Bug 3 probe: at the moment a doors frame is shown, is the TOP of the door source (the top
        // door band, rows g2..) blank?  Sample the first 6 rows across all 3 planes; if every byte is
        // 0 the top door is black.  Record the worst (earliest, smallest g2) case per run.
        // Follows updateDoorsCopper's source bitmap — doorScrollBitmap; while this read viewportBitmap
        // and updateDoorsCopper had already been left behind pointing there too, the probe agreed with
        // the copper and so could never flag the missing door content.
        // Sample BOTH candidate sources so the fix is a measurement, not a code reading:
        // g_doorTopBlack is the bitmap the copper now reads, g_doorTopBlackVp the one it used to.
        { extern volatile unsigned char g_doorTopBlack, g_doorTopBlackVp, g_doorTopG2, g_doorTopSeen;
          const uint8_t* vp = (const uint8_t*)doorScrollBitmap->data + (uint32_t)g2 * 120u;
          const uint8_t* vo = (const uint8_t*)viewportBitmap->data + (uint32_t)g2 * 120u;
          int nz = 0; for (int i = 0; i < 6 * 120; i++) if (vp[i]) { nz = 1; break; }
          int nzo = 0; for (int i = 0; i < 6 * 120; i++) if (vo[i]) { nzo = 1; break; }
          if (!g_doorTopSeen || (unsigned char)g2 <= g_doorTopG2) {   // earliest / smallest-g2 doors frame
              g_doorTopBlack = nz ? 0 : 1; g_doorTopBlackVp = nzo ? 0 : 1;
              g_doorTopG2 = (unsigned char)g2; g_doorTopSeen = 1; } }
#endif
        // Same priority as the Standby the doors part from.  Normally already set by the
        // staticStandby transition above; written here too so a rebuild that reaches the doors
        // without a settled Standby frame cannot inherit the tunnel's PFxP=4 (gauge on top).
        setSpritePriority(kSpritePriorityCockpit);
        AmigaHardware::setCopperList(*doorsCopper[back], false);
        doorsActive = back;
        tunnelCopperInstalled = false;
#ifdef ROF_FLIGHT_PROBE
        { extern volatile unsigned char g_liveCopper; g_liveCopper = 6; }
#endif
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
    // Top-bar region (above the viewport): the flight VBI $4FF5 reloads its three GTIA colours from
    // display params on EVERY frame, before any DLI fires —
    //   $5004 COLBK  ($D01A) <- $00D4   = top-bar background
    //         COLPM0 ($D012) <- $00D4   = left canopy pillar   (players P0/P1, setSpritePostColor)
    //         COLPM1 ($D013) <- $00D4   = right canopy pillar
    //   $5014 COLPF0 ($D016) <- $00D8   = top-bar / message text
    //   $5019 COLPF1 ($D017) <- $00D7   = top-bar pf1 (blue)
    // so all three track mem[] live, not just during the death cinematic.  This used to read the
    // $02C8 COLOR4 shadow for the background and bake pf1 at $78, which is indistinguishable in
    // normal flight — the game's OWN base table $4DF1 (which intro_fill_display_params $4FE0 loads
    // into $00CF+y) is $04 $26 $2C $90 $00 $06 $44 $9A $78, so $00D4 == $06 == $02C8 and $00D7 == $78
    // — but it froze the top bar and the pillars during the ESC pause: the $5039 strobe rewrites ALL
    // of $00CF-$00DD (VBI $504F-$5058) and $02C8 is outside that range, so they never moved.  Sourcing
    // them the way the VBI does fixes the pause and the cinematic with one rule.  ($00DE, the energy
    // bar, sits one byte past the strobe's `LDY #$0E` top and correctly does NOT cycle.)
    const uint16_t titleBg  = atariToOCS(mem[MEM_display_param_5]);  // COLBK = top-bar bg / canopy posts
    const uint16_t titlePf0 = atariToOCS(mem[MEM_text_color_pf0]);   // COLPF0 = top-bar text ($00D8)
    const uint16_t titlePf1 = atariToOCS(mem[MEM_display_param_8]);  // COLPF1 = top-bar blue ($00D7)
    const uint16_t energyCol = atariToOCS(mem[0x00DE]);             // gauge bar colour

    if (force || titleBg != flTitleBg || titlePf0 != flTitlePf0 || titlePf1 != flTitlePf1) {
        flightCopper->setTitlePalette(titleBg, titlePf0, titlePf1);
        flightCopper->setSpritePostColor(titleBg);
        flTitleBg = titleBg; flTitlePf0 = titlePf0; flTitlePf1 = titlePf1;
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
        // ch5/ch6 point at the WIDE-OBJECT EXTENSION segments, not straight at the gauges: each
        // extension is chained in front of its gauge in one chip buffer, so the channel shows the
        // segment across the viewport and then the gauge below (see RescueOnFractalus.h).  The
        // gauges' own setX/setY still drive them — only the copper's entry pointer moves up.
        flightCopper->setHudSprite(5, *wideExt[0][wideDispIdx]);
        flightCopper->setHudSprite(6, *wideExt[1][wideDispIdx]);
        flightCopper->setHudSprite(1, *wideExt[2][wideDispIdx]);
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
    // $00D0 drives TWO dashboard slots, both COLPM2 via DLI $4A78 ($4A80): the AH ground fill (P2)
    // and the Long-Range-Scanner guide dot (missile M2 — the dashboard's PRIOR=$04 leaves the
    // fifth-player bit clear, so M2 takes its player's colour).  The scanner dot was baked at $26 in
    // buildLayout, which is exactly $00D0's $4DF1 base, so this is a no-op in normal flight and only
    // shows up under the pause strobe / the death ramp — where it must move with the AH fill.
    if (force || ahGround != flAHGround)     { flightCopper->setAHGroundColor(ahGround);
                                               flightCopper->setScannerDotColor(ahGround);
                                               flAHGround = ahGround; }
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
    // Compass needle / heading letters (mode-4 value-3 = COLPF2).  NOTHING writes COLPF2 before the
    // compass line: the VBI $4FF5 doesn't touch $D018 and DLI $4A11 sets only CHBASE + COLPF0, so the
    // compass shows whatever the PREVIOUS frame's dashboard DLI left there — $4A78's `STX COLPF2` at
    // $4A8E, i.e. **$00D1**.  The needle is therefore the same pen as the dashboard's value-3 pixels
    // and must track $00D1 with them.  This was baked at $2A while the cockpit bake used $2C for the
    // same register, which cannot both be right; $4DF1+2 = $2C settles it, so $2A was simply one luma
    // step dark.  Being a param, it also fades salmon in the death cinematic ($4FE0 ramps $00CF-$00D6,
    // and $00D1 is in range) and cycles under the ESC-pause strobe — neither of which it used to do.
    // (The Atari shows it one frame late, since the value is last frame's; not worth emulating.)
    const uint8_t needleD1 = mem[MEM_display_param_2];
    if (force || needleD1 != flNeedleD1) {
        flightCopper->setCompassNeedleColor(atariToOCS(needleD1));
        flNeedleD1 = needleD1;
    }

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
    //
    // ⚠ $26 is a CONSTANT here on purpose — do NOT "fix" it to track a display param.  Up here the
    // VBI's PRIOR = mem[$026F] = $11 has the fifth-player bit ($10) SET, so all four missiles take
    // **COLPF3**, not their players' colours.  And a raw scan of the whole main blob for `STA $D019`
    // finds only $4FEF, $52EE, $6D76, $6D93 — none of them in the flight VBI or any flight DLI.  So
    // COLPF3 holds the stale $26 the launch chain left ($6D76/$6D93 `LDA #$26 STA $D019`) for the
    // entire flight, and the ESC-pause strobe (which only rewrites RAM $00CF-$00DD) can never reach
    // it: the crosshair correctly does NOT colour-cycle, unlike everything else in the cockpit.
    // (The lone flight-adjacent writer $4FEF is the death ramp, so on the Atari the crosshair alone
    // fades on death without cycling on pause; it is gated hidden — $2840 = 0 unless the scope is
    // locked on — so that path is not worth the poke.)
    if (mem[0x2840] != 0) {
        const uint16_t cross = atariToOCS(0x26);
        flightCopper->setCrosshairPalette(cross, cross, cross, cross);
    } else {
        flightCopper->setCrosshairPalette(terr0, terr1, terr2, terr3);
    }

    // Cockpit body pens.  The Atari dashboard DLI $4A78 reloads them from the display params on EVERY
    // frame — COLPF0 <- $00CF ($4A88), COLPF1 <- $00D4 ($4A8B), COLPF2 <- $00D1 ($4A8E), COLBK <- $00D2
    // ($4A93); the bottom DLI $4ACD then puts $00D3 in COLBK ($4ACF) — so the whole dashboard follows
    // whatever writes those bytes, with no gating flag anywhere.  Mapping mirrors the buildLayout bake:
    // color00/04<-$D3, 01/05<-$CF, 02/06<-$D4, 03<-$D1, 07<-$D0 (COLPM2, the AH ground), dash<-$D2.
    //
    // This used to run ONLY during the death cinematic (event_trigger $063D != 0), which is one of the
    // two things that move these params — intro_fill_display_params $4FE0 ramps $00CF-$00D6 to salmon.
    // The other is the ESC-pause strobe ($5039: `LDA $07E9,Y EOR $0012 AND #$F6 STA $CF,Y`, Y=$0E..0),
    // which the gate excluded, so the cockpit body stayed frozen while the terrain cycled.  Driving it
    // live with no gate matches the DLI and covers both.  Each baked constant equals its param's base
    // value in the game's own $4DF1 table — $D3=$00, $CF=$04, $D4=$06, $D1=$2C, $D0=$26, $D2=$90 — so
    // normal flight is byte-identical to the bake and, the params being constant there, pokes nothing.
    const uint8_t ckD3 = mem[MEM_display_param_4], ckCF = mem[MEM_display_param_0];
    const uint8_t ckD4 = mem[MEM_display_param_5], ckD1 = mem[MEM_display_param_2];
    const uint8_t ckD0 = mem[MEM_display_param_1], ckD2 = mem[MEM_display_param_3];
    if (force || ckD3 != flCkD3 || ckCF != flCkCF || ckD4 != flCkD4
              || ckD1 != flCkD1 || ckD0 != flCkD0) {
        const uint16_t c0 = atariToOCS(ckD3), c1 = atariToOCS(ckCF), c2 = atariToOCS(ckD4);
        flightCopper->setCockpitPalette(c0, c1, c2, atariToOCS(ckD1),
                                        c0, c1, c2, atariToOCS(ckD0));
        flCkD3 = ckD3; flCkCF = ckCF; flCkD4 = ckD4; flCkD1 = ckD1; flCkD0 = ckD0;
    }
    if (force || ckD2 != flCkD2) {
        flightCopper->setDashBg(atariToOCS(ckD2));
        flCkD2 = ckD2;
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
    // Door halves come from the TALL doorScrollBitmap — the ONLY bitmap the door field is decoded
    // into (decodeDoorScrollField: rows 0..84 = the live $2000 field, 85..171 = green pad).  This
    // used to read viewportBitmap, from the era when the standby decoded the doors there; the
    // level-select elevator scroll moved that decode to doorScrollBitmap and render() now states
    // outright that "the door field no longer touches viewportBitmap".  Nothing repointed this, so
    // the moment the doors began to part the copper started reading a bitmap holding no door
    // content at all and both halves came out flat green — the LEVEL NN text and dots vanished on
    // the first opening frame (user-observed 2026-08-10; bisect put it before 39f4d8a, i.e. it
    // arrived with the scroll work, not with this session).  Same geometry either way (kW wide,
    // 3 interleaved planes, 120-byte rows, field row 0 = the resting doors), and the 172-row
    // height means the sliding g2 offset cannot run off the end the way an 86-row bitmap could.
    const uint32_t ta   = (uint32_t)doorScrollBitmap->data;
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

// boostRevealK(): how far the reverse-tunnel reveal has opened, as the first viewport row that
// shows rings — 43 means nothing yet, 0 means the rings fill the region.  Read from the live
// $3000 launch DL's per-row mode-F LMS words, which the faithful emit_dl_coord_pairs rewrites
// $2000 -> $1000 one row per reverse-ring step.
// ⚠ Derive it from the FORWARD strand (rows 0..42) alone and MIRROR it.  Reading the converted
// rows directly gives an asymmetric set (fwd rows 42->0 but rev rows 57->85) and renders as a
// bowtie; the Atari's reveal is symmetric about the vanishing point (measured rv_8.6, 22 rows in:
// rows 32-53 = symmetric around 42.5).
uint16_t RescueOnFractalus::boostRevealK() const
{
    for (int row = 0; row <= 42; row++) {
        const uint16_t lms = (uint16_t)(mem[0x300Au + row * 3] | (mem[0x300Bu + row * 3] << 8));
        if (lms >= 0x1000u && lms < 0x2000u) return (uint16_t)row;
    }
    return 43;
}

// showTunnelCopper(): populate the BACK tunnel buffer and swap it in.  setCopperList only writes
// COP1LC, which the copper re-reads at the next vblank, so the swap is atomic with respect to the
// beam — which is the whole point: the reveal moves bitplane POINTERS, and poking one on the live
// list can be read half-written (a torn pointer garbages the entire viewport for a frame).
void RescueOnFractalus::showTunnelCopper()
{
    const uint8_t back = (uint8_t)(1 - tunnelActive);
    if (!tunnelCopper[back]) return;
    updateTunnelCopper(tunnelCopper[back]);
    AmigaHardware::setCopperList(*tunnelCopper[back], false);
    tunnelActive = back;
#ifdef ROF_FLIGHT_PROBE
    g_tunLastVbi = platform_frame_count();
#endif
}

// updateTunnelCopper(): fully populate ONE TunnelCopperList buffer — the tunnel descent (scene 5,
// doors fully open) and the boost reverse cinematic both use it.  Every dynamic slot is written
// unconditionally: this always targets the BACK buffer, whose contents are two frames stale, so a
// poke-on-change cache would be comparing against the wrong list.
void RescueOnFractalus::updateTunnelCopper(TunnelCopperList* tunnelCopper)
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

    tunnelCopper->setTitlePalette(titleBg, titlePf0, atariToOCS(0x78));
    tunnelCopper->setSpritePostColor(postCol);
    tunnelCopper->setEnergyIndicatorColor(energyCol);
    tunnelCopper->setCompassColor(compassCol);
    // Terrain-region bitmap bands.  FORWARD descent: one full-height band from tunnelBitmap
    // (K = 0).  BOOST: the reverse-tunnel reveal — rings from tunnelBitmap in [K, 85-K], the
    // starfield from viewportBitmap outside it.  Nothing is composited; the copper picks.
    if (rsBoostViewport) {
        const uint16_t _k = boostRevealK();
#ifdef ROF_FLIGHT_PROBE
        if (_k <= 43) { g_rkHist[_k]++; if (_k < g_rkMin) g_rkMin = (unsigned char)_k; }
        if (!g_rkFirstVbi) g_rkFirstVbi = platform_frame_count();
        g_rkLastVbi = platform_frame_count();
        { const unsigned short n = g_rkN;
          if (n == 0 || (n <= RK_STEPS && g_rkK[n - 1] != (unsigned char)_k)) {
              if (n < RK_STEPS) { g_rkK[n] = (unsigned char)_k; g_rkVbi[n] = g_rkLastVbi; g_rkHold[n] = 1; }
              g_rkN = (unsigned short)(n + 1);
          } else if (n <= RK_STEPS) { g_rkHold[n - 1]++; } }
#endif
        tunnelCopper->setRevealBands(_k, (uint32_t)tunnelBitmap->data,
                                     (uint32_t)viewportBitmap->data);
    } else
        tunnelCopper->setRevealBands(0, (uint32_t)tunnelBitmap->data, (uint32_t)tunnelBitmap->data);
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
        // Band corner triangle: the FORWARD tunnel's mechanism, verbatim (see the else-branch below).
        // color00 already holds the colour the corner should show — in the boost LUT that is value-2,
        // the outermost ring — and it carries from the viewport into the band untouched; one moving
        // WAIT flips it to the green door colour from the topmost still-set canopy-wedge row down.
        // Wedge empty (the whole reverse tunnel) -> greenLine 8 -> the band shows the ring.
        //
        // ⚠ This REPLACES a phase latch (boostRingRevealed: absolute-poke color00 black at the band
        // top until $008D went negative = "the outermost ring has been drawn").  That latch was
        // sampled by the main loop from a byte the VBI animates, so whether it ever saw the negative
        // window depended on the render cadence — the corner came out black for the whole cinematic
        // on some runs and correct on others (user-observed 2026-08-10, and the odds moved when the
        // decode got 4.5x faster).  Nothing here is sampled: the split is re-derived from the live
        // wedge buffer every frame, which is why the forward path has never shown this class of bug.
        uint16_t bGreenLine = 8;                            // first still-green band scanline
        for (uint16_t i = 0; i < 8; i++) { if (mem[0x0C88 + i]) { bGreenLine = i; break; } }
        if (bGreenLine == 0) {
            // Whole band takes the door colour (the wedge is still up), so the moving WAIT has no
            // work to do — and at greenLine 0 it sits at kCockpitLine-1, the slot the cockpit
            // bitplane-pointer moves OVERRUN by ~16px into the band's first line, leaving the
            // carried ring colour showing there (user-observed teal stripe over black, 2026-08-10;
            // the same artifact commit 8481ec0 fixed for the old boost path).  Flip color00 EARLY
            // instead, before those moves — the pairing the T6 recede branch above already uses.
            tunnelCopper->setBandTopColor00(true, atariToOCS(mem[0x0071]));
            tunnelCopper->disableBandReveal();
        } else {
            tunnelCopper->setBandTopColor00(false, 0);      // no absolute band-top poke
            tunnelCopper->setBandReveal(bGreenLine, atariToOCS(mem[0x0071]));
        }
#ifdef ROF_FLIGHT_PROBE
        // Does the canopy wedge actually CLEAR during the boost?  In flight $0C88-$0C8F is all $FF
        // (the corner-triangle players), which would pin bGreenLine at 0 = the corner never shows
        // the ring.  Tally the derived split per sub-phase so that is a measurement, not a guess.
        { extern volatile unsigned long g_bwLine[9], g_bwLineStars[9];
          if (mem[0x008D] != 0u) g_bwLine[bGreenLine]++; else g_bwLineStars[bGreenLine]++; }
#endif

        // pen0=color00 (value-2 outermost ring); pen1/3=ring[3]/ring[5] (COLPM1/3); pen2=color02
        // (value-8 COLBK); pen4/5/6=ring[0..2] (COLPF0/1/2); pen7=value-0 black.
        tunnelCopper->setTunnelColors(col00, ring[3], colBK, ring[5], ring[0], ring[1], ring[2], black);
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
        // pen0/color00 = corner ($08D8); pens 1-3 = ring[3..5]; pens 4-6 = ring[0..2]; pen7 = black.
        tunnelCopper->setTunnelColors(corner, ring[3], ring[4], ring[5], ring[0], ring[1], ring[2], black);
    }
}

// ============================================================================
//  BOOT SCENES 1 + 2 — the GTIA-9 field decode (Logo / Station cinematic)
// ============================================================================
// Both scenes are ANTIC mode F under GTIA mode 9: a 40-byte row is 80 fat pixels, each a 4-bit
// LUMINANCE of the one COLBK hue.  On the Amiga that is 4 bitplanes; nibble n IS pen n, so the
// whole mapping lives in the palette (Gtia9CopperList) and the decode is four table lookups per
// source byte (kGtia9P).  One field bitmap and one copper list serve both scenes — the station
// is the larger, at 340 rows (it decodes its whole display list once and scrolls by moving the
// bitplane pointers); the logo uses rows 0..61.  See docs/logo-station-plan.md.

// gtia9Row(): decode `cols` GTIA-9 source bytes into ONE interleaved bitmap row, given a pointer
// to that row's plane-1 byte.  Pure pointer walk — one source read plus four d16(An) stores per
// byte, no index arithmetic and no per-byte multiply (the row-offset mul-tables are for
// NON-sequential row access; a sequential loop walks a pointer instead).  The four plane bytes of
// one interleaved row sit 40 apart, which is exactly d16(An)'s 12-cycle form.
static inline void gtia9Row(uint8_t* dst, const volatile uint8_t* src, unsigned cols)
{
    const uint8_t* t0 = kGtia9P[0];
    const uint8_t* t1 = kGtia9P[1];
    const uint8_t* t2 = kGtia9P[2];
    const uint8_t* t3 = kGtia9P[3];
    while (cols--) {
        const uint8_t b = *src++;
        dst[0]   = t0[b];
        dst[40]  = t1[b];
        dst[80]  = t2[b];
        dst[120] = t3[b];
        dst++;
    }
}

// decodeStationField(): decode ALL 340 display-list rows once, and note which of them are STAR
// rows (their LMS points into the range station_star_fade_in walks) so the fade-in's per-frame
// re-decode touches nothing else.  ~14k source bytes ≈ 70 ms — a one-off, and it runs during
// station_init's own one-frame sync spin ($19CD) with the screen still black.
// Both the display-list cursor (+3 per entry) and the destination row (+160) are walked.
void RescueOnFractalus::decodeStationField()
{
    if (!bootFieldBitmap) return;
    stationStarRows = 0;
    const volatile uint8_t* e = mem + kStationDL;              // display-list cursor
    uint8_t*              dst = (uint8_t*)bootFieldBitmap->data;
    for (unsigned r = 0; r < kStationDLRows; r++, e += 3, dst += 160) {
        if (e[0] != 0x4Fu) break;      // not an LMS mode-F entry: the JVB, or not built yet
        const uint16_t lms = (uint16_t)(e[1] | (e[2] << 8));
        gtia9Row(dst, mem + lms, 40);
        if (lms >= kStationStarLo && lms < kStationStarHi && stationStarRows < 40)
            stationStarRow[stationStarRows++] = (unsigned short)r;
    }
    g_stationDirtyCount = 0; g_stationDirtyFull = 0;   // the full decode captured everything
}

// decodeStationStars(): re-decode just the star rows.  station_star_fade_in brightens every
// non-zero nibble in $2CB8-$3167 once per frame for 14 frames, so those ~30 rows are the only
// thing moving before the scroll starts.  ~1200 bytes ≈ 6 ms, for 14 frames only.
// These rows are SCATTERED through the display list (1-in-8 by RANDOM), so each one does need
// its own row offset — via rof_mulu16 (mulu.w), never a 32-bit multiply the 68000 lacks.
void RescueOnFractalus::decodeStationStars()
{
    if (!bootFieldBitmap) return;
    uint8_t* const base = (uint8_t*)bootFieldBitmap->data;
    for (unsigned i = 0; i < stationStarRows; i++) {
        const uint16_t r = stationStarRow[i];
        const volatile uint8_t* e = mem + (uint16_t)(kStationDL + rof_mulu16(r, 3));
        gtia9Row(base + rof_mulu16(r, 160), mem + (uint16_t)(e[1] | (e[2] << 8)), 40);
    }
}

// decodeStationDirty(): consume the rectangles station_sub_1EB4 / station_chan_step recorded
// (rof_manual.h).  Under 200 bytes a frame, versus ~14k for a full re-decode — which is the
// whole reason those hooks exist.  A row of the 122-row image at $0600 is bitmap row (a-$0600)/40,
// since display_list_build gives image row n the LMS $0600 + 40n and puts it at DL entry n.
// A rectangle's rows ARE sequential, so one divide + one multiply per rectangle sets up the two
// cursors and the row loop then just walks them (+40 source, +160 destination).
void RescueOnFractalus::decodeStationDirty()
{
    if (g_stationDirtyFull) { decodeStationField(); return; }   // also clears the list
    if (!bootFieldBitmap) { g_stationDirtyCount = 0; return; }
    for (unsigned i = 0; i < g_stationDirtyCount; i++) {
        const uint16_t addr = g_stationDirty[i].addr;
        unsigned       cols = g_stationDirty[i].cols;
        unsigned       nrow = g_stationDirty[i].rows;
        if (addr < kStationImg || addr >= kStationImgEnd) continue;   // not in the image
        const uint16_t off = (uint16_t)(addr - kStationImg);
        const uint16_t row = rof_divu16(off, 40);                    // divu.w, never __udivsi3
        const uint16_t col = (uint16_t)(off - rof_mulu16(row, 40));
        if (col + cols > 40u) cols = 40u - col;                      // clip a rect at the row end
        if (row + nrow > 122u) nrow = 122u - row;                    // ...and at the image end
        const volatile uint8_t* src = mem + addr;
        uint8_t*                dst = (uint8_t*)bootFieldBitmap->data + rof_mulu16(row, 160) + col;
        while (nrow--) { gtia9Row(dst, src, cols); src += 40; dst += 160; }
    }
    g_stationDirtyCount = 0;
}

// renderBootScene(): the whole per-frame render for scenes 1 and 2, called from the top of
// renderFrame while g_bootScene is set.  Nothing else in the per-frame body applies — there is no
// cockpit, title bar, viewport or sprite HUD in either scene — so this is the entire pass.
//
// The scroll itself is NOT here: it is four bitplane-pointer writes that must happen at vblank
// (stationVblankUpdate, from the VBI ISR), because a pointer torn between the CPU write and the
// copper's read garbages the whole frame.  What is left is the field decode, and the field only
// changes in three ways: everything at scene entry, the ~30 star rows while station_star_fade_in
// is brightening them, and the animation's dirty rectangles thereafter.
void RescueOnFractalus::renderBootScene()
{
    if (!bootFieldCopper || !bootFieldBitmap) return;   // SKIPBOOT build, or an allocation failed

    // Keyed on WHICH scene the live layout was built for, not on a plain "is it installed" flag:
    // the Logo ends and the Station begins with the stage-2 segment load in between and NO
    // renderFrame call anywhere in that gap, so a bool would still read "installed" and the
    // station would inherit the logo's geometry and gold palette.
    if (bootFieldScene != g_bootScene) {
        // Scene entry.  Build the layout for THIS scene (the two differ in leading blank lines,
        // row count and palette hue), decode the field, publish the first window row, install.
        deriveRenderSignals();
        if (g_bootScene == ROF_BOOTSCENE_STATION) {
            bootFieldCopper->buildLayout(*bootFieldBitmap, kStationTopLines, kStationRows,
                                         kGtia9Pal0, *nullSprite);
            decodeStationField();
            stationWindowRow = 0xFFFF;   // force stationVblankUpdate to publish the real row
        } else {
            bootFieldCopper->buildLayout(*bootFieldBitmap, kLogoTopLines, kLogoRows,
                                         kGtia9Pal1, *nullSprite);
        }
        AmigaHardware::setCopperList(*bootFieldCopper, false);   // latches at the next vblank
        bootFieldCopperInstalled = true;
        bootFieldScene = g_bootScene;
        emptyCopperInstalled = false;   standbyCopperInstalled = false;
        planetCopperInstalled = false;  flightCopperInstalled = false;
        tunnelCopperInstalled = false;  titleScreenCopperInstalled = false;
        return;
    }

    if (g_bootScene != ROF_BOOTSCENE_STATION) return;   // the logo's field never changes

    // station_star_fade_in runs BEFORE the scroll loop, so the phase counter $008B is still 0 for
    // exactly the frames it is brightening the starfield.  ⚠ If the stars look black, this is the
    // routine (an earlier native list dropped it as "PMG only" — it is not, it is the fade-in).
    if (mem[0x008B] == 0) decodeStationStars();
    decodeStationDirty();
}

// stationVblankUpdate(): the scroll.  The display list's moving JMP operand ($1C39/$1C3A) walks
// from $B9BC down to $B800, 3 bytes (one entry) per step, so the window's first row is simply
// (ptr - $B800) / 3 and the whole scroll is four bitplane-pointer writes.
// ⚠ VBI ISR ONLY — a bitplane pointer torn between the CPU write and the copper's read garbages
// the entire frame (amiga-copper-lessons).  Called from PlatformAmiga::vbiHandler.
void RescueOnFractalus::stationVblankUpdate()
{
    if (!bootFieldCopperInstalled || !bootFieldCopper) return;
    if (g_bootScene != ROF_BOOTSCENE_STATION) return;
    const uint16_t ptr = (uint16_t)(mem[0x1C39] | (mem[0x1C3A] << 8));
    if (ptr < kStationDL) return;                                    // half-written / not started
    const unsigned row = rof_divu16((uint16_t)(ptr - kStationDL), 3); // divu.w
    if (row == stationWindowRow) return;
    stationWindowRow = (unsigned short)row;
    bootFieldCopper->setWindowRow((uint16_t)row);
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
    // Boot scene 2, the station cinematic.  Keyed on the boot chain's own g_bootScene rather than
    // on VVBLKI alone, because $1B30 SURVIVES station_exit: it hands the vector back with SETVBV,
    // whose os_setvbv is a no-op stub here — and that is deliberate, since the $1B30 body is what
    // keeps advancing the RTCLOK that init_B800's 32-frame wait spins on right afterwards.  The
    // vector is still required, so a half-written $0222/$0223 can never select this branch.
    rsStation  = (g_bootScene == ROF_BOOTSCENE_STATION) && (vvblki == 0x1B30u);
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
    // ...AND on boostCineLatch, because $003A alone is not enough: it stays $FF into the next level,
    // so every later boot_standby_launch_driver construction (notably the one the Title Screen's
    // START re-enters game_main_loop to run) was rendered as a second reverse cinematic.  The latch
    // picks out the ONE construction that follows the ascent — see updateBoostCinematicLatch().
    // Sub-phase by the reverse-ring flags (measured live, FORCE_RETURN):
    //   stars   : $008D==0 && $008E==0   reverse ring not started — starfield in the $1000 field
    //   tunnel  : $008D!=0               reverse ring active      — concentric rings in $1000
    //   standby : $008D==0 && $008E!=0   ring done                — green LEVEL-NN door field ($2000)
    // Stars+tunnel share the launch-cockpit mode-D VIEWPORT bitmap (decoded from $1000, per the
    // user's faithful-to-$6CAD decision — NOT the forward $6CC2 PMG starfield); the final standby
    // falls through to the normal rsStandby path.
    rsBoostReturn   = standbyVbi && (mem[0x003A] == 0xFFu) && boostCineLatch;
    rsBoostViewport = rsBoostReturn && (mem[0x008D] != 0u || mem[0x008E] == 0u);
    // Release the tunnel bitmap the moment the boost stops owning it.  Boost ownership is taken at
    // the boost copper install and must NOT survive past here — the very next pre-draw is
    // tunnel_prebuild_rings building the NEXT launch's FORWARD rings, which needs the forward LUT
    // and a forward re-prime (tunnelPaintBegin), not the boost's.  Drop to NONE rather than to
    // FORWARD: nothing may paint until that pre-build explicitly claims the bitmap, because the T6
    // handoff hold is still displaying the last reverse-ring frame at this point.
    if (!rsBoostViewport && tunnelOwner == kTunnelOwnerBoost) tunnelOwner = kTunnelOwnerNone;
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
    // ⚠ NOT during the boost: rsStars can be true while the reverse cinematic owns viewportBitmap
    // as its STARFIELD buffer (the copper reads it for the rows outside the reveal band), and this
    // clear would blank the starfield mid-cinematic.
    if (rsStars && !rsBoostViewport && viewportForceFull && viewportBitmap && !viewportClearKicked) {
        AmigaHardware::blitterClear((uint16_t*)viewportBitmap->data, 60, 47, 0);
        viewportClearKicked = true;
    }
    // Starfield zero-copy scroll — the main loop only does the ONE-TIME full build here; the whole
    // per-frame update (window advance, copper SPRxPT re-point, control words, new-row conversion)
    // belongs to the VBI ISR (starVblankUpdate), which is the only place the beam is guaranteed to
    // be above all three of its deadlines.  Publishing the copper operand from here instead used to
    // work only by the grace of the A500's slow render landing it late in the frame; see the beam
    // deadlines in the starVblankUpdate header comment.
    if (rsStars) {
        if (!starSpritesValid) {
            extern volatile unsigned short g_starScrollGen;
#ifdef ROF_FLIGHT_PROBE
            { const unsigned long _ss0 = rof_subclock();
              buildStarSprites();
              if (g_starSprVbi == 0) { g_starSprVbi = platform_frame_count();
                                       g_starSprTicks = rof_subclock() - _ss0; }
              if (g_seArmed) g_seSpr = rof_subclock() - _ss0; }
#else
            buildStarSprites();                         // full build at window 0 (one transient frame)
#endif
            starLastGen = g_starScrollGen;
            // Publish window 0 for the entry frame.  Safe to do from the main loop even though the
            // copper may latch it the same frame: buildStarSprites just wrote the control words at
            // slot 0, so the operand is self-consistent the instant it is written.  From the next
            // vblank on, starVblankUpdate owns it.
            if (planetCopper) for (int i = 0; i < 6; i++) planetCopper->setStarOperand(i, starRing[i]);
            starSpritesValid = true;
        }
#ifdef ROF_FLIGHT_PROBE
        { extern volatile unsigned short g_pfwStarCalls; g_pfwStarCalls++; }
#endif
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
    // The six flight sprite builders are DEFERRED, not run here: they are pure CPU on sprite
    // buffers with no dependency on any of the terrain blits, so renderFlightDirect runs them in
    // the shadow of the plane1-clear and sky-fill blits it would otherwise stall on.  Just mark
    // them owed; see buildFlightSpritesEarly/Late/Flush.
    if (rsFlight) flightSpritesOwed = 0x03;
}

// Slot A — runs between the plane1-clear KICK and the drain the edge plot needs.
void RescueOnFractalus::buildFlightSpritesEarly()
{
    if (!(flightSpritesOwed & 0x01)) return;
    flightSpritesOwed &= (uint8_t)~0x01u;
    buildAltimeterSprite();          // one-shot solid fill, then a setY
    buildAltimeterShipSprite();      // setY
    buildAHSprite();                 // artificial-horizon ground fill
    // Those three together are only ~2 ticks — not enough to cover the plane1 clear (~12), so the
    // scanner dot joins them.  It is the independent one of the three "late" builders (M2 scanner
    // dot; the two P3 builders share the target/saucer state), so it is the safe one to move.
    buildScannerDotSprite();
}

// Slot B — runs between the sky-fill KICK (blitterFillUp) and the wait before the band overlay.
void RescueOnFractalus::buildFlightSpritesLate()
{
    if (!(flightSpritesOwed & 0x02)) return;
    flightSpritesOwed &= (uint8_t)~0x02u;
    buildScopeP3Sprite();
    buildViewportP3Sprite();
}

// Safety net after render(): renderFlightDirect returns early on three paths (no bitmaps, the
// rescue-figure pause, a frame with no fresh terrain).  Without this the sprites would freeze on
// exactly those frames — the altimeter/AH/scope would stop tracking during a rescue.
void RescueOnFractalus::buildFlightSpritesFlush()
{
    if (!flightSpritesOwed) return;
    buildFlightSpritesEarly();
    buildFlightSpritesLate();
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

#ifdef ROF_CK_VERIFY
// Coverage check for the per-cell cockpit registries (see the CK_VERIFY block in amiga/Makefile).
// The risk of a targeted decode is not arithmetic — decodeCockpitSpan is unchanged — but a MISSED
// write leaving a cell stale, which no arithmetic differential can see.  So: a targeted decode is
// COMPLETE iff decoding the whole group afterwards changes nothing.
//
// The check brackets the WHOLE cockpit block (snap the source bytes before it, verify after it),
// for two reasons: a group's cells may legitimately be decoded by a LATER group's registry, and
// snapshotting the source before the decode is what makes the race filter sound — a write landing
// between the decode and the snapshot would otherwise read as a coverage hole (it did: 1 in 284).
// Any group whose source moved during the bracket, or whose two full passes disagree, is discarded
// as `raced` (the sandwich rule in flight-measurement-rules).
extern "C" volatile unsigned long g_ckVerCalls = 0, g_ckVerBad = 0, g_ckVerRaced = 0;
extern "C" volatile unsigned long g_ckVerCallsG[2] = {0,0}, g_ckVerBadG[2] = {0,0};

// Every cockpit group the registries are responsible for: the lock-on strip, the five digit blocks
// (two DL rows each) and the DL-stride pair.  group 0 = lock-on, 1 = digits.
struct CkVerGroup { uint16_t addr; uint8_t nCells; uint8_t group; };
static const CkVerGroup kCkVerGroups[] = {
    { 0x3491u, 7u, 0u },
    { 0x33B4u, 2u, 1u }, { 0x33E4u, 2u, 1u },
    { 0x3413u, 2u, 1u }, { 0x3443u, 2u, 1u },
    { 0x3445u, 2u, 1u }, { 0x3475u, 2u, 1u },
    { 0x3472u, 2u, 1u }, { 0x34A2u, 2u, 1u },
    { 0x34A4u, 2u, 1u }, { 0x34D4u, 2u, 1u },
    { 0x33DFu, 2u, 1u },
};
static const int kCkVerN = (int)(sizeof(kCkVerGroups) / sizeof(kCkVerGroups[0]));
static uint8_t s_ckVerSrc[kCkVerN][8];

void RescueOnFractalus::ckVerifySnap()
{
    for (int g = 0; g < kCkVerN; g++)
        for (int c = 0; c < kCkVerGroups[g].nCells; c++)
            s_ckVerSrc[g][c] = mem[(uint16_t)(kCkVerGroups[g].addr + c)];
}

void RescueOnFractalus::ckVerifyAll()
{
    static const int kRowBytes = 120;
    for (int g = 0; g < kCkVerN; g++) {
        const uint16_t addr   = kCkVerGroups[g].addr;
        const uint8_t  nCells = kCkVerGroups[g].nCells;
        const uint8_t  grp    = kCkVerGroups[g].group;
        unsigned off   = (unsigned)(addr - 0x332Du);
        unsigned entry = rof_divu16(off, 48u);
        int      col   = (int)rof_modu16(off, 48u) - 4;
        if (entry >= 10u || col < 0 || col + nCells > 40) continue;
        const uint8_t* base = (const uint8_t*)cockpitBitmap->data + (8 + entry * 8) * kRowBytes + col;

        uint8_t snap[3][8 * 8 * 3];               // [pass][((scan*nCells)+cell)*3 + plane]
        for (int pass = 0; pass < 3; pass++) {
            if (pass) decodeCockpitSpan(addr, nCells);
            int k = 0;
            for (int scan = 0; scan < 8; scan++)
                for (int c = 0; c < nCells; c++) {
                    const uint8_t* p = base + scan * kRowBytes + c;
                    snap[pass][k++] = p[0]; snap[pass][k++] = p[40]; snap[pass][k++] = p[80];
                }
        }
        g_ckVerCalls++; g_ckVerCallsG[grp]++;
        bool raced = false;
        for (int c = 0; c < nCells; c++)
            if (s_ckVerSrc[g][c] != mem[(uint16_t)(addr + c)]) raced = true;
        const int n = 8 * nCells * 3;
        for (int k = 0; k < n && !raced; k++)
            if (snap[1][k] != snap[2][k]) raced = true;
        if (raced) { g_ckVerRaced++; continue; }
        for (int k = 0; k < n; k++)
            if (snap[0][k] != snap[1][k]) { g_ckVerBad++; g_ckVerBadG[grp]++; break; }
    }
}
#endif

// Decode the lock-on indicator cells that a writer actually touched (see g_ckLockFlag).  Runs from
// BOTH consumers: the main-loop render() and the standby door-scroll vblank bridge.  Consecutive
// dirty cells are merged into one decodeCockpitSpan call, so the 6-cell fill sweep still costs one
// call while the far more frequent single-cell blink costs one cell.  Clear-then-decode: a writer
// racing the clear re-raises its own byte and is caught next frame (never lost).
void RescueOnFractalus::decodeLockonDirty()
{
    const unsigned long* fl = (const unsigned long*)(const void*)g_ckLockFlag;
    if ((fl[0] | fl[1]) == 0u) return;                 // flag set but cells already consumed
    for (int i = 0; i < 7; ) {
        if (!g_ckLockFlag[i]) { i++; continue; }
        int run = 0;
        while (i + run < 7 && g_ckLockFlag[i + run]) { g_ckLockFlag[i + run] = 0u; run++; }
        decodeCockpitSpan((uint16_t)(0x3491u + i), (uint8_t)run);
#ifdef ROF_FLIGHT_PROBE
        g_ckLockCells += (unsigned long)run;
#endif
        i += run;
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
    SP_CNT(g_spScanCalls); SP_RESET();
    static uint8_t last = 0xFFu;
    uint8_t v = mem[0x33DFu];
    if (v == last) { SP_LAP(g_spScanT); return; }
    last = v;
    SP_CNT(g_spScanDecodes);
    decodeCockpitSpan(0x33DFu, 2u);
    SP_LAP(g_spScanT);
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
    // ⚠ The BOOST cinematic owns BOTH viewport bitmaps — tunnelBitmap holds the painted rings and
    // viewportBitmap holds its starfield — so no terrain-view decode may run while it is up.
    // rsViewport is not enough on its own: there is a brief rsStars window DURING the boost (the
    // same one that made the perFrameWork entry clear blank the starfield), and taking the branch
    // below would decode the $1000 RING field as a mode-D field straight over the stars.
    if (!rsBoostViewport) {
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
#ifdef ROF_FLIGHT_PROBE
        { extern volatile unsigned char g_dsDecWhy; g_dsDecWhy = 3; }   // 3 = terrainDirty full decode
#endif
        decodeDoorScrollField();
        g_doorScrollFieldDirty = 0;
        // NOTE: deliberately do NOT reset g_doorDirtyRow0/1 here.  A pending "whole field" mark is
        // left by fill_region_2000/blit_message_block BEFORE the rebuild finishes, and it has to
        // survive until the blit_numeric_readout that ends the rebuild raises the flag.  Clearing
        // it here narrowed that decode to the 8 digit rows, so the wiped LEVEL text kept its stale
        // pixels across the level-04->01 wrap while only the digits changed.
        // The stars/planet renderer decodes $1000 into viewportBitmap via renderViewportModeD; the
        // door field no longer touches viewportBitmap, but a re-launch after the BOOSTERS return
        // must still force a full planet clear (the door "LEVEL NN"/band remnants used to bleed
        // through the sparse starfield — bug 3).  Stamp viewportLastBase to $2000 so the NEXT
        // renderViewportModeD($1000) sees a base mismatch and blitter-clears first.
        viewportLastBase = 0x2000u;
    }
    }   // !rsBoostViewport (the boost owns tunnelBitmap AND viewportBitmap)

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
#ifdef ROF_CK_VERIFY
    ckVerifySnap();          // source cells BEFORE any decode — makes the race filter sound
#endif
    bool any = false;
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
        // The full paint covers every cell — drop all instrument flags + the per-cell registries.
        g_ckDigits = g_ckLockon = g_ckDial = 0u;
        for (int i = 0; i < CK_DIAL_N; i++) g_ckDialFlag[i] = 0u;
        for (int i = 0; i < 8; i++) g_ckLockFlag[i] = g_ckDigitFlag[i] = 0u;
#ifdef ROF_FLIGHT_PROBE
        if (rsFlight) g_fCockpitScans++;
#endif
    } else {
        // Digits (#17-19) + DL-stride: 5 two-tall 2×2 blocks + the $33DF/$33E0 stride pair, one
        // registry slot each — exactly one block changes per fire (measured), so decode only it.
        if (g_ckDigits) {
            g_ckDigits = 0u;
#ifdef ROF_FLIGHT_PROBE
            unsigned long _ckd0 = rof_subclock();
#endif
            static const uint16_t kDigit[5] = { 0x33B4u, 0x3413u, 0x3445u, 0x3472u, 0x34A4u };
            for (int i = 0; i < 5; i++) {
                if (!g_ckDigitFlag[i]) continue;
                g_ckDigitFlag[i] = 0u;
                decodeCockpitSpan(kDigit[i], 2u);                 // top row
                decodeCockpitSpan((uint16_t)(kDigit[i] + 0x30u), 2u);  // bottom row (one DL row down)
#ifdef ROF_FLIGHT_PROBE
                g_ckDigitBlocks++;
#endif
            }
            if (g_ckDigitFlag[5]) {
                g_ckDigitFlag[5] = 0u;
                decodeCockpitSpan(0x33DFu, 2u);                   // DL-stride control bytes
#ifdef ROF_FLIGHT_PROBE
                g_ckDigitBlocks++;
#endif
            }
            any = true;
#ifdef ROF_FLIGHT_PROBE
            if (rsFlight) { g_ckDigitT += rof_subclock() - _ckd0; g_ckDigitFires++; }
#endif
        }
        // Lock-on indicator (#11): the 7 cells $3491-$3497.
        if (g_ckLockon) {
            g_ckLockon = 0u;
#ifdef ROF_FLIGHT_PROBE
            unsigned long _ckl0 = rof_subclock();
#endif
            decodeLockonDirty();
            any = true;
#ifdef ROF_FLIGHT_PROBE
            if (rsFlight) { g_ckLockT += rof_subclock() - _ckl0; g_ckLockFires++; }
#endif
        }
        // Thrust (#4) / Dangerous-Altitude (#5) dial bars: per-cell, walked only now (dial moved).
        // Long-batched skip so the all-clear runs between bar cells are cheap.
        if (g_ckDial) {
            g_ckDial = 0u;
#ifdef ROF_FLIGHT_PROBE
            unsigned long _cka0 = rof_subclock();
#endif
            const unsigned long* fl = (const unsigned long*)(const void*)g_ckDialFlag;
            for (int i = 0; i < CK_DIAL_N / 4; i++) {
                if (fl[i] == 0u) continue;
                int base = i * 4;
                for (int b = 0; b < 4; b++) {
                    if (g_ckDialFlag[base + b]) {
                        g_ckDialFlag[base + b] = 0u;
                        decodeCockpitSpan((uint16_t)(0x332Du + base + b), 1u);
#ifdef ROF_FLIGHT_PROBE
                        if (rsFlight) g_ckDialCells++;
#endif
                    }
                }
            }
            any = true;
#ifdef ROF_FLIGHT_PROBE
            if (rsFlight) { g_ckDialT += rof_subclock() - _cka0; g_ckDialFires++; }
#endif
        }
#ifdef ROF_FLIGHT_PROBE
        if (rsFlight && any) g_fCockpitScans++;
#endif
    }
#ifdef ROF_FLIGHT_PROBE
    if (rsFlight) g_fCockpit += rof_subclock() - _ckp0;   // flight-only: cockpitTicks/tdFrames is per-flight-frame
#endif
#ifdef ROF_CK_VERIFY
    // Only on frames that decoded something: checking EVERY frame costs ~36 extra span decodes and
    // slowed the launch cinematic so much the probe never reached flight.  A stale cell survives
    // until the next decode frame anyway, and those are ~44% of frames, so nothing hides for long.
    if (any) ckVerifyAll();
#else
    (void)any;
#endif
    if (rsFlight) g_flightProf.renderTot += (unsigned short)(flight_vbi_tick() - profR0);
}

void RescueOnFractalus::shutdown()
{
    delete standbyCopper; standbyCopper = nullptr;
    delete planetCopper; planetCopper = nullptr;
    delete flightCopper; flightCopper = nullptr;
    for (int i = 0; i < 2; i++) { delete doorsCopper[i]; doorsCopper[i] = nullptr; }
    for (int i = 0; i < 2; i++) { delete tunnelCopper[i]; tunnelCopper[i] = nullptr; }
    delete titleScreenCopper; titleScreenCopper = nullptr;
    delete bootFieldCopper; bootFieldCopper = nullptr;   // boot scenes 1+2 (null under SKIPBOOT)
    delete emptyCopper;   emptyCopper   = nullptr;
    PlatformAmiga::audioShutdown();
    delete bootFieldBitmap; bootFieldBitmap = nullptr;   // the shared 54 KB GTIA-9 field
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
    // ⚠ energy / altimeter-terrain / left band triangle are ALIASES of wideLow[0..2][0] — non-owning
    // views into the wide-object sprite chains.  Just drop the aliases; the wideLow loop below is
    // what deletes them (deleting here too would be a double free).
    energyIndicatorSprite = nullptr;
    altimeterSprite       = nullptr;
    flLeftTri             = nullptr;
    delete altimeterShipSprite; altimeterShipSprite = nullptr;
    delete flLeftPost;  flLeftPost  = nullptr;
    delete flRightPost; flRightPost = nullptr;
    delete flRightTri;  flRightTri  = nullptr;
    { static const int kLowRows[3] = { kEnergyRows, kAltimRows, 8 };
      for (int s = 0; s < 3; s++)
          for (int b = 0; b < 2; b++) {
              delete wideExt[s][b]; wideExt[s][b] = nullptr;
              delete wideLow[s][b]; wideLow[s][b] = nullptr;
              Sprite::freeChain(wideChain[s][b],
                                (uint16_t)((s == 2) ? kWideExtRowsCh1 : kWideExtRows),
                                (uint16_t)kLowRows[s]);
              wideChain[s][b] = nullptr;
          } }
    for (int c = 0; c < 6; c++) { delete starSprite[c]; starSprite[c] = nullptr; }
}
