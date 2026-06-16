#pragma once
#include "../framework/CopperList.h"
#include "../framework/Bitmap.h"
#include "../framework/Palette.h"
#include "../framework/Sprite.h"
#include "StandbyCopperList.h"

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
    // pumpFrame(): the per-frame repaint body — per-frame non-phase work + render +
    // back copper-list rebuild + flip.  Assumes the caller has already waited one real
    // VBI.  Shared by frameStep() (run()-driven) and the transpiled frame pump
    // (platform_render_frame -> launchFramePump in main.cpp) so both produce identical
    // frames; public because the free-function pump calls it through g_scenePtr.
    void pumpFrame();
    void shutdown();

    // run(): the whole game as a faithful straight-line transcription of the
    // Atari control flow — station_init's attract loop (wait for START) ->
    // display_setup's launch cinematic (gauge/doors/tunnel/stars/planet, each
    // phase set up then blocked-on with REAL frame-waits) -> game_entry's flight
    // loop.  Replaces the old per-frame update() launchPhase state machine: the
    // original is straight-line code that busy-waits, so this is too.  Returns
    // when the user quits (left mouse button).
    void run();

    // Launch cinematic: start the doors-open transition (Atari START press).
    // The terrain image (green/dots/LEVEL 04 = the "closed doors") splits from
    // the middle, halves sliding apart, revealing the tunnel in the gap.
    void openDoors();

    // Dev shortcut (F key): skip the whole launch cinematic and jump straight to
    // the in-game flight stage.  flight_init_native re-initialises the game state,
    // so this is safe to call from the standby screen or mid-cinematic.
    void skipToFlight();
private:
    // frameStep(): the REAL per-frame busy-wait that backs every wait point in
    // the original straight-line code (wait_frames_2/5/10/60 etc.).  Spins until
    // the real INTB_VERTB VBI ISR signals the next frame (the ISR ran the Atari
    // VBI body + bumped RTCLOK $0014 once), then does the per-frame non-phase work
    // and repaints (render() + Copper rebuild) on the main thread — exactly where
    // the Atari's ANTIC would have regenerated the display for that frame.
    enum FrameResult : uint8_t { kFrameContinue, kFrameQuit, kFrameSkip };
    FrameResult frameStep();
    // cinematicFrame(): one cinematic frame-wait.  Returns true if the cinematic
    // should abort — either the user quit, or the F-key skip-to-flight fired (in
    // which case it has already handed off to flightLoop()).
    bool cinematicFrame();
    void perFrameWork();    // per-frame non-phase work (title/blink/digits/sprites)
    void flightLoop();      // game_entry $3EBA flight loop: frameStep + flight_frame_native

    uint16_t frameCounter = 0;   // frames elapsed (drives buildCopperList fade-in)
    // Launch phases, mirroring display_setup's linear walk: after START we fill
    // the throttle gauge, open the doors, run the tunnel (ring cycle) until it
    // auto-clears $0088, then the stars/space scroll and the planet zoom — each a
    // step of the cinematic body $5F1D..$6594 driven one-per-frame.
    enum LaunchPhase : uint8_t {
        kLaunchNone, kLaunchGauge, kLaunchDoors, kLaunchTunnel,
        kLaunchStars, kLaunchPlanet, kFlight
    };
    uint8_t  launchPhase  = kLaunchNone;

    // Render-gating signals derived from mem[] hardware state each frame (validated in
    // Commit 3 against the launchPhase enum, now the source of truth for rendering).
    // buildCopperList/render/perFrameWork key off these instead of the enum, so the
    // renderer keeps working once the transpiled display_setup drives the cinematic and
    // the enum is gone.  (`launched`, which gates only the door gap, stays the C++ bool
    // until Commit 6 — see the g2 comment in buildCopperList.)
    void deriveRenderSignals();
    bool rsStandby  = false;   // VVBLKI $52D7        — Standby + launch cinematic VBI
    bool rsGauge    = false;   // $060B != 0          — cinematic begun (gauge sprite on)
    bool rsStars    = false;   // VDSLST $0200==$C2    — stars/planet viewport (sprites/colours)
    bool rsFlight   = false;   // $004A != 0           — in-game flight (palette/probe/profiler)
    bool rsViewport = false;   // stars || flight      — mode-D viewport band active
    bool rsLaunched = false;   // doors armed || viewport — door-gap g2 (doors..flight)

    void startStars();                   // display_setup $64C8-$6552 stars setup
    void startPlanet();                  // display_setup $6555-$6574 planet setup
    void startFlight();                  // game_entry $3E12-$3EB8 flight init (after planet)
    Sprite*  gaugeSprite   = nullptr;    // player-strip throttle bar ($0D98)
    void startDoors();                   // door-scroll state (display_setup $63DC)
    void buildGaugeSprite();             // $0D98 strip -> gaugeSprite lines

    // Stars/space starfield: the 3 Atari players P0/P2/P3 ($0C32/$0E32/$0F32),
    // scrolled + sparsely seeded by the genuine scroll_terrain_columns ($6AEE),
    // rendered as Amiga hardware sprites 4/5/6.  (P1 = $0D32 is the throttle gauge
    // player and stays on sprite 2; sprite 3 would share the gauge's colour
    // registers, so the stars use sprites 4/5/6 for an independent grey COLPM.)
    Sprite*  starSprite[3] = { nullptr, nullptr, nullptr };
    void buildStarSprites();             // $0C32/$0E32/$0F32 player buffers -> star sprites

    // Launch state lives in mem[] now: the door-open progress is the $008A
    // scroll counter (decremented by the native scroll_terrain_dl via the $5367
    // dispatcher), and the ring animates once $0088 is armed.  `launched` only
    // distinguishes pre-launch Standby (doors shut) from the running cinematic.
    bool launched = false;

    // Tunnel reveal: a 3bp concentric-rectangle bitmap shown in the door gap.
    // Motion is palette cycling — the 6-entry ring lives in mem[$08D4-$08D9]
    // (feeds COLOR01-06) and is rotated by the native ports of step_accum_add_75
    // ($6A38) -> advance_history_6a4d ($6A4D), gated by $0088 (tunnel_ring_tick_native).
    Bitmap*  tunnelBitmap = nullptr;

    void buildCopperList(CopperList* cl, uint16_t frame);
    void fillSpriteData(Sprite* s, bool isRight);
    void decodeTunnelField(int rowLo, int rowHi);  // decode mem[$2000] rows [lo,hi] -> tunnelBitmap
    void renderViewportModeD(uint16_t srcBase, int stride, int rows); // decode CHANGED mode-D bytes -> terrainBitmap (stars: $1000/48/43; flight: $1070/96)

    // Stars/planet phase: when true, the viewport region renders mem[$1000] as an
    // ANTIC mode-D 2bpp field (43 rows x 48 bytes, central 40 shown) instead of the
    // GTIA-10 terrain/door image, and uses the $6CC2 DLI viewport palette.
    bool viewportActive = false;

    CopperList* copperLists[2] = { nullptr, nullptr };

    // Static-Standby fixed copper list (built once, poked in place — see
    // StandbyCopperList).  Used while in the settled Standby/gauge-fill state
    // (rsStandby && g_doorFieldReady && !rsViewport && !rsLaunched): no per-frame
    // full rebuild, no double-buffer flip.  pumpFrame switches back to the
    // double-buffered buildCopperList path for the dynamic phases.
    StandbyCopperList* standbyCopper = nullptr;
    bool standbyCopperInstalled = false;   // is standbyCopper the currently-installed list?
    void updateStandbyCopper(bool force);  // poke changed colour/sprite slots (force = all)
    // Last-poked values, so updateStandbyCopper only writes a MOVE when it changed.
    uint16_t sbTitleBg = 0xFFFF, sbTitlePf0 = 0xFFFF, sbGaugeCol = 0xFFFF;
    uint16_t sbTerr0 = 0xFFFF, sbTerr1 = 0xFFFF, sbTerr2 = 0xFFFF, sbTerr3 = 0xFFFF;
    int8_t   sbGauge = -1;   // sprite-2 = gauge(1)/null(0); -1 = unset

    Bitmap*     titleBitmap    = nullptr;
    Bitmap*     terrainBitmap  = nullptr;
    Bitmap*     cockpitBitmap  = nullptr;
    Palette*    palette        = nullptr;  // drives title region + global fade
    Sprite*     leftPost       = nullptr;
    Sprite*     rightPost      = nullptr;
    Sprite*     nullSprite     = nullptr;
    uint8_t     active         = 0;

    // Dirty-flag bitmap caching: bitmaps are rendered once on initialize() and
    // only re-rendered when the underlying mem[] data changes.
    bool    terrainDirty = true;   // re-render terrain rows from $2000
    uint8_t titleShadow[20] = {};  // shadow of last-rendered $32B7-$32CA

    // Per-byte shadow for the stars/planet mode-D viewport: renderViewportModeD
    // re-decodes only the $1000 source bytes that changed since last frame (the
    // planet sphere grows a few bytes/frame), not the whole 43×40 field.
    bool    viewportForceFull = true;          // first viewport render populates all cells
    uint8_t viewportShadow[43 * 40] = {};      // shadow of the central-40 $1000 bytes

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
