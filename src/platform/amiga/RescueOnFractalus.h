#pragma once
#include "framework/CopperList.h"
#include "framework/Bitmap.h"
#include "framework/Palette.h"
#include "framework/Sprite.h"
#include "StandbyCopperList.h"
#include "PlanetCopperList.h"
#include "FlightCopperList.h"
#include "EmptyCopperList.h"

// 2-bitplane attract screen: one BPLCON0 mode for the whole frame, Copper
// switches the 4-colour palette (and bitmap pointer) at each region boundary,
// mirroring the Atari DLI schedule exactly.
//
// Regions:
//   Title   (lines 0..kTerrainLine-1) : title.raw  + kTitlePalette
//   Terrain (lines kTerrainLine..kCockpitLine-1) : terrain.raw + kTerrainPalette
//   Cockpit (lines kCockpitLine..end) : cockpit.raw + kCockpitPalette (+ blink)
class RescueOnFractalus {
public:
    void initialize();
    void render();
    // renderFrame(): the per-frame repaint body — per-frame non-phase work + render +
    // back copper-list rebuild + flip.  Called from PlatformAmiga::renderFrame() at each
    // transpiled frame-wait hook; public because the caller reaches it through s_scene.
    void renderFrame();
    void shutdown();

    // run(): the whole game as a faithful straight-line transcription of the
    // Atari control flow — station_init's attract loop (wait for START) ->
    // display_setup's launch cinematic (gauge/doors/tunnel/stars/planet, each
    // phase set up then blocked-on with REAL frame-waits) -> game_entry's flight
    // loop.  Replaces the old per-frame update() launchPhase state machine: the
    // original is straight-line code that busy-waits, so this is too.  Returns
    // when the user quits (left mouse button).
    void run();
private:
    void perFrameWork();    // per-frame non-phase work (title/blink/digits/sprites)

    uint16_t frameCounter = 0;   // frames elapsed (drives buildCopperList fade-in)

    // Render-gating signals derived from mem[] hardware state each frame: the genuine
    // transpiled/native game_entry -> game_main_loop -> display_setup drives the program
    // and swaps the live VVBLKI vector per scene; buildCopperList/render/perFrameWork key
    // off these mem[]-derived signals to pick the render mode for the current phase.
    void deriveRenderSignals();
    bool rsStandby  = false;   // VVBLKI $52D7        — Standby + launch cinematic VBI
    bool rsGauge    = false;   // $060B != 0          — cinematic begun (gauge sprite on)
    bool rsStars    = false;   // VDSLST $0200==$C2    — stars/planet viewport (sprites/colours)
    bool rsFlight   = false;   // $004A != 0           — in-game flight (palette/probe/profiler)
    bool rsViewport = false;   // stars || flight      — mode-D viewport band active
    bool rsLaunched = false;   // doors armed || viewport — door-gap g2 (doors..flight)
    bool doorsOpenedLatch = false;  // door scroll finished; hold the tunnel view through the
    uint8_t prevScrollCtr = 0;      // gap before the ring/viewport arms (see deriveRenderSignals)
    bool    prevRsStars = false;    // rising edge → one-time title/cockpit rescan on stars entry

    Sprite*  gaugeSprite   = nullptr;    // player-strip throttle bar ($0D98)
    void buildGaugeSprite();             // $0D98 strip -> gaugeSprite lines

    // Stars/space starfield: the 3 Atari players P0/P2/P3 ($0C32/$0E32/$0F32),
    // scrolled + sparsely seeded by the genuine scroll_terrain_columns ($6AEE),
    // rendered as Amiga hardware sprites 4/5/6.  (P1 = $0D32 is the throttle gauge
    // player and stays on sprite 2; sprite 3 would share the gauge's colour
    // registers, so the stars use sprites 4/5/6 for an independent grey COLPM.)
    Sprite*  starSprite[3] = { nullptr, nullptr, nullptr };
    void buildStarSprites();             // $0C32/$0E32/$0F32 player buffers -> star sprites

    // Tunnel reveal: a 3bp concentric-rectangle bitmap shown in the door gap.
    // Motion is palette cycling — the 6-entry ring lives in mem[$08D4-$08D9]
    // (feeds COLOR01-06) and is rotated by the native ports of step_accum_add_75
    // ($6A38) -> advance_history_6a4d ($6A4D), gated by $0088 (tunnel_ring_tick_native).
    Bitmap*  tunnelBitmap = nullptr;

    void buildCopperList(CopperList* cl, uint16_t frame);
    void buildPostSprites();   // decode RLE tables $4DFA/$4E09 -> leftPost/rightPost (once)
    bool postsBuilt = false;   // canopy posts are constant: decode them a single time
    void decodeTunnelField(int rowLo, int rowHi);  // decode mem[$2000] rows [lo,hi] -> tunnelBitmap
    void renderViewportModeD(uint16_t srcBase, int stride, int rows); // decode CHANGED mode-D bytes -> terrainBitmap (stars: $1000/48/43; flight: $1070/96)

    CopperList* copperLists[2] = { nullptr, nullptr };

    // Static-Standby fixed copper list (built once, poked in place — see
    // StandbyCopperList).  Used while in the settled Standby/gauge-fill state
    // (rsStandby && g_doorFieldReady && !rsViewport && !rsLaunched): no per-frame
    // full rebuild, no double-buffer flip.  renderFrame switches back to the
    // double-buffered buildCopperList path for the dynamic phases.
    StandbyCopperList* standbyCopper = nullptr;
    bool standbyCopperInstalled = false;   // is standbyCopper the currently-installed list?
    void updateStandbyCopper(bool force);  // poke changed colour/sprite slots (force = all)
    // Last-poked values, so updateStandbyCopper only writes a MOVE when it changed.
    uint16_t sbTitleBg = 0xFFFF, sbTitlePf0 = 0xFFFF, sbGaugeCol = 0xFFFF;
    uint16_t sbTerr0 = 0xFFFF, sbTerr1 = 0xFFFF, sbTerr2 = 0xFFFF, sbTerr3 = 0xFFFF;
    int8_t   sbGauge = -1;   // sprite-2 = gauge(1)/null(0); -1 = unset

    // Static stars/planet viewport fixed copper list (the rsViewport non-flight layout —
    // line-doubled mode-D band).  Same build-once + poke-in-place scheme as standbyCopper:
    // renderViewportModeD updates the bitmap content (constant pointer), buildStarSprites
    // updates the sprite data (constant pointers), so only a few colours change per frame.
    PlanetCopperList* planetCopper = nullptr;
    bool planetCopperInstalled = false;  // is planetCopper the currently-installed list?
    void updatePlanetCopper(bool force); // poke changed colour slots (force = all)
    // Last-poked values (vp* — separate from sb* so a phase switch always force-refreshes).
    uint16_t plTitleBg = 0xFFFF, plTitlePf0 = 0xFFFF, plGaugeCol = 0xFFFF,
             plStarCol = 0xFFFF, plBg = 0xFFFF;

    // Static flight fixed copper list (scene 7 — same line-doubled mode-D band, with the
    // flight terrain palette + HUD sprites).  Same build-once + poke-in-place scheme:
    // renderViewportModeD updates the terrain bitmap content, the ported flight VBI pokes
    // the per-frame colours/sprites via updateFlightCopper.  Replaces the old per-frame
    // buildCopperList full rebuild for flight.
    FlightCopperList* flightCopper = nullptr;
    bool flightCopperInstalled = false;  // is flightCopper the currently-installed list?
    void updateFlightCopper(bool force); // poke changed colour/sprite slots (force = all)
    // Last-poked values (fl* — separate from sb*/pl* so a phase switch force-refreshes).
    uint16_t flTitleBg = 0xFFFF, flTitlePf0 = 0xFFFF, flGaugeCol = 0xFFFF;
    uint16_t flTerr0 = 0xFFFF, flTerr1 = 0xFFFF;   // terrain pen0/pen1 (atmosphere ramp $00DC/$00DD)

    Bitmap*     titleBitmap    = nullptr;
    Bitmap*     terrainBitmap  = nullptr;
    Bitmap*     cockpitBitmap  = nullptr;
    // Blank black list shown until g_standbyRevealReady latches (boot/standby build in
    // progress) — switched to the real lists in renderFrame once ready.
    EmptyCopperList* emptyCopper = nullptr;
    bool emptyCopperInstalled = false;
    Sprite*     leftPost       = nullptr;
    Sprite*     rightPost      = nullptr;
    Sprite*     nullSprite     = nullptr;
    uint8_t     active         = 0;

    // Dirty-flag bitmap caching: bitmaps are rendered once on initialize() and
    // only re-rendered when the underlying mem[] data changes.
    bool    terrainDirty = true;   // re-render terrain rows from $2000
    uint8_t titleShadow[20] = {};  // shadow of last-rendered $32B7-$32CA

    // Long-granular shadow for the stars/planet/flight mode-D viewport.  Holds the last
    // decoded source as 430 longs (43 rows × 10 longs of 4 packed bytes); renderViewportModeD
    // re-decodes only the 4-byte groups that changed (the planet zoom / star scroll leaves
    // much of the field static frame-to-frame).  forceFull re-decodes everything (and clears
    // plane3) on the first frame and whenever the source base changes (stars↔flight).
    bool     viewportForceFull = true;
    uint16_t viewportLastBase  = 0;
    uint32_t viewportShadow[43 * 10] = {};

    // Per-byte shadow for the tunnel field at $2000: the exit clear draws a thin
    // black frame outline (horizontal edges + vertical side pieces) each step, so
    // decodeTunnelField re-decodes only the bytes that changed — covering the
    // vertical pieces a row-band would miss, while staying well under a frame.
    uint8_t tunnelShadow[86 * 40] = {};

    // Per-cell shadow caching for the cockpit (mirrors titleShadow): a single
    // changed Atari source byte re-decodes only that cell — modeD: 2 rows × 3
    // planes = 6 writes; mode4: 8 scanlines × 3 planes = 24 writes — never the
    // whole 88-row region.  cockpitForceFull makes the first render() populate
    // every cell regardless of shadow contents.
    bool    cockpitForceFull = true;
    uint8_t cockpitModeDShadow[4 * 40] = {};   // shadow of modeD source ($350D region)
    uint8_t cockpitMode4Shadow[10 * 40] = {};  // shadow of mode4 source ($332D region)
};
