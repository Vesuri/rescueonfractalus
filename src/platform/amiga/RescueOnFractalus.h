#pragma once
#include "framework/CopperList.h"
#include "framework/Bitmap.h"
#include "framework/Palette.h"
#include "framework/Sprite.h"
#include "StandbyCopperList.h"
#include "PlanetCopperList.h"
#include "FlightCopperList.h"
#include "DoorsCopperList.h"
#include "TunnelCopperList.h"
#include "EmptyCopperList.h"
#include "TitleScreenCopperList.h"
#include "Gtia9CopperList.h"

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
    void flightKickBackClear();  // post-vblank (called by PlatformAmiga::renderFrame): kick the
                                 // blitter clear of the next back buffer so it overlaps the upcoming
                                 // terrain draw; renderFlightDirect then just waits for it.
    void buildShotSprite();      // player laser (P2 $0E32) -> shotSprite; PUBLIC because it runs in
                                 // the flight VBI (via PlatformAmiga::flightShotTick) at 50Hz, not
                                 // at the slow render rate — faithful to the Atari (the shot is a VBI
                                 // op).  Reads mem[$0036/$0037/$00CB]; safe at vblank (beam off-screen).
    void flightVblankSwap();     // run from the INTB_VERTB ISR at vblank start: if a flight buffer
                                 // swap is pending, rewrite the copper's viewport bitplane pointers
                                 // (before the beam reaches them) and clear the flag.
    void blankForRestart();      // run from the INTB_VERTB ISR when a BREAK/Restart is armed (VVBLKI=
                                 // $52B4): jump the copper to the black EmptyCopperList NOW (safe at
                                 // vblank — beam at top, so its sprite MOVEs still execute), so the
                                 // ~4 flight-compute frames between the trampoline and the main-loop
                                 // longjmp show black instead of stale/mid-swap flight (the flash).
    // Dot side-buffer: read-and-clear the deferred-flip flag.  renderFrame() calls this once per
    // frame — true means the flight normal path deferred its flip-wait, so renderFrame skips its own
    // vblank wait and the next terrain compute overlaps the pending flip's vblank.
    bool consumeDeferredFlip() { bool d = flightFlipDeferred; flightFlipDeferred = false; return d; }
    void starVblankUpdate();     // run from the INTB_VERTB ISR at vblank: zero-copy starfield scroll
    void stationVblankUpdate();  // run from the INTB_VERTB ISR at vblank (scene 2, the station
                                 // cinematic): move the tall field bitmap's four bitplane pointers to
                                 // the display list's current window row, and refresh the PMG sprites.
                                 // ⚠ MUST be the VBI, not render(): a torn bitplane pointer garbages
                                 // the whole frame.  No-op unless the station copper is live.
    void logoVblankUpdate();     // run from the INTB_VERTB ISR at vblank (scene 1, the Lucasfilm
                                 // logo): mirror player 0's eroding "✦" into its Amiga sprite.
                                 // Nothing to scroll — the logo's display list never moves — but
                                 // animating PMG belongs in the VBI all the same.  No-op unless
                                 // the logo copper is live.
    void doorScrollVblankUpdate(); // run from the INTB_VERTB ISR at vblank: level-select "elevator"
                                 // door scroll (post-mother-ship SELECT).  While dl_src_index ($008B)
                                 // is non-zero (boot_standby_launch_driver's level-select scroll spins
                                 // on the ISR-driven decrement), repoint the standby terrain BPLxPT to
                                 // the tall door bitmap offset by dl_src_index rows — the uniform DL-LMS
                                 // window scroll.  No-op unless the settled standby is live.
    void drawTunnelRect(uint16_t rowBase, uint8_t rowTop, uint8_t rowBot, uint8_t xL, uint8_t xR,
                        uint8_t byteLo, uint8_t byteHi, uint8_t colour);  // ROF_TUNNEL_RECT hook: paint one ring rectangle straight into tunnelBitmap
    void drawTunnelColumns(uint16_t rowBase, uint8_t colL, uint8_t colR, uint8_t colR1,
                           uint8_t colour);   // ROF_TUNNEL_COLS hook: the pre-draw's three full-height guide columns
    void drawTunnelVSpan(uint16_t rowBase, uint8_t r0, uint8_t r1, uint8_t colL, uint8_t colR,
                         uint8_t colour);   // ROF_TUNNEL_VSPAN hook: one plot_terrain_span vertical pair
    void paintVSpan(uint8_t rowBot, uint8_t rowTop, uint8_t xL, uint8_t xR, uint16_t pen);
    uint16_t tunnelPen(uint8_t colour) const;  // GTIA nibble -> Amiga pen through the live owner's LUT
    void tunnelPaintBegin();     // arm the FORWARD ring painter + prime tunnelBitmap (before the pre-draw)
    void decodeScannerBlinkCells(); // LR-scanner (#13) close-range blink cells $33DF/$33E0 -> cockpit
                                 // bitmap; PUBLIC because it runs in the flight VBI (via PlatformAmiga::
                                 // flightScannerTick) at 50Hz so the blink animates at full rate, not
                                 // the ~5-6fps main-loop render() cadence.  Decodes only on a bit7 flip.
    void shutdown();

    // run(): the whole game as a faithful straight-line transcription of the
    // Atari control flow — station_init's attract loop (wait for START) ->
    // boot_standby_launch_driver's launch cinematic (gauge/doors/tunnel/stars/planet, each
    // phase set up then blocked-on with REAL frame-waits) -> game_entry's flight
    // loop.  Replaces the old per-frame update() launchPhase state machine: the
    // original is straight-line code that busy-waits, so this is too.  Returns
    // when the user quits (left mouse button).
    void run();
private:
    void perFrameWork();    // per-frame non-phase work (title/blink/digits/sprites)

    // Render-gating signals derived from mem[] hardware state each frame: the genuine
    // transpiled/native game_entry -> game_main_loop -> boot_standby_launch_driver drives the program
    // and swaps the live VVBLKI vector per scene; render/perFrameWork key
    // off these mem[]-derived signals to pick the render mode for the current phase.
    void deriveRenderSignals();
    bool rsStandby  = false;   // VVBLKI $52D7        — Standby + launch cinematic VBI
    bool rsEnergyIndicator    = false;   // $060B != 0          — cinematic begun (gauge sprite on)
    bool rsStars    = false;   // VDSLST $0200==$C2    — stars/planet viewport (sprites/colours)
    bool rsFlight   = false;   // $004A != 0           — in-game flight (palette/probe/profiler)
    bool rsViewport = false;   // stars || flight      — mode-D viewport band active
    bool rsTitle    = false;   // VVBLKI $53CC && title active — attract/level-select Title Screen
    bool rsStation  = false;   // boot scene 2 (VVBLKI $1B30 + g_bootScene) — station cinematic
    bool rsLaunched = false;   // doors armed || viewport — door-gap g2 (doors..flight)
    bool rsBoostReturn   = false;  // $52D7 && $003A==$FF && boostCineLatch — BOOSTERS return reverse cinematic
    bool rsBoostViewport = false;  // rsBoostReturn && (reverse-ring active || pre-ring) — boost stars/tunnel
    // mission_event_flag $003A stays $FF into the NEXT level, so it alone cannot say WHICH
    // boot_standby_launch_driver construction is the reverse cinematic.  This latch does: armed
    // in flight when the mother ship has arrived, released when that construction completes.
    // updateBoostCinematicLatch() maintains it; see the comment there.
    bool    boostCineLatch = false;
    uint8_t latchPrevDoorRdy = 1;   // previous g_doorFieldReady, for the latch's release edge
    void updateBoostCinematicLatch();

    // BPLCON2 (sprite-vs-playfield priority) is write-only hardware that persists across copper
    // lists.  The Standby and Doors lists emit no BPLCON2 MOVE, so their priority is a one-off CPU
    // write at the scene transition — setSpritePriority(), called at those two copper installs.
    // PF2P=PF1P=1: sprite pair 0 (canopy posts) in FRONT of the playfield, pairs 1+ (the
    // throttle/energy gauge on sprite 2) BEHIND it, which is what hides the 56-row bar's overflow
    // below its dial.  The value initialize() seeds.
    static const uint16_t kSpritePriorityCockpit = (uint16_t)((1u << 3) | 1u);
    void setSpritePriority(uint16_t v);
    bool doorsOpenedLatch = false;  // door scroll finished; hold the tunnel view through the
    uint8_t prevScrollCtr = 0;      // gap before the ring/viewport arms (see deriveRenderSignals)
    bool    prevRsStars = false;    // rising edge → one-time title/cockpit rescan on stars entry
    bool    prevRsFlight = false;   // rising edge → one-time full cockpit repaint on flight entry
    uint8_t prevDoorFieldReady = 0; // rising edge (standby built) → one-time full cockpit repaint

    Sprite*  energyIndicatorSprite   = nullptr;    // player-strip throttle bar ($0D98)
    bool     energySolidBuilt = false;             // solid 57-row sprite filled once (lazily), then setY only
    void buildEnergyIndicatorSprite();             // $0D98 strip -> energyIndicatorSprite lines
    Sprite*  altimeterSprite = nullptr;  // P0 $0C98 terrain-height bar (flight altimeter)
    Sprite*  altimeterShipSprite = nullptr;  // M3 $0B98 ship-height bar (flight altimeter)
    bool     altimSolidBuilt = false;    // altimeter bars are solid 56-row sprites filled once (lazily)
    // Player laser (Atari P2 $0E32) on the idle sprite ch4.  DOUBLE-BUFFERED: buildShotSprite
    // (run in the flight VBI) writes the off-screen buffer and re-points SPR4PT to it; the copper
    // latches SPR4PT at the top of the NEXT frame, so the buffer being displayed is always one
    // fully built in a prior VBI — never rewritten mid-scan (was single-buffered → tearing).
    Sprite*  shotSprite     = nullptr;   // buffer 0
    Sprite*  shotSpriteBack = nullptr;   // buffer 1
    uint8_t  shotBuildIdx   = 0;         // which buffer buildShotSprite writes this frame (toggles)
    bool     shotWasActive  = false;     // last frame's mem[$0036]!=0 (blank both buffers once on end)
    uint8_t  shotPrevRows[2] = {0, 0};   // rows each buffer's last build wrote = the incremental
    uint8_t  shotPrevBase[2] = {0, 0};   // clear's extent (the buffers alternate, so per-buffer)

    // ---- WIDE OBJECTS (SIZEP2 / SIZEP3) and the shared extension channels --------------------
    // An Atari player widened by SIZEPn covers 8/16/32 colour clocks at 1×/2×/4× — 16/32/64 Amiga
    // lores px, i.e. up to FOUR 16px sprites.  Two objects do this: the LASER IMPACT BURST (P2,
    // SIZEP2 = mem[$00CD], set 0/1/3 by build_player2_sprite $8C58 as the burst zooms) and the
    // MAIN-WINDOW P3 OBJECT (saucer / gun emplacement, SIZEP3 from table $4566 by mem[$006A], so
    // 4× on its very last approach frame).  In both cases the Atari also shifts HPOSn left (by
    // $286E = 0/4/12 colour clocks for P2) so the widened player stays centred — the shadow we
    // read is therefore already the LEFT edge, and segment n simply sits at baseX + 16*n.
    //
    // Segment 0 is the object's own channel (ch4 burst / ch7 P3); segments 1-3 are these shared
    // extensions on ch5/ch6/ch1 — the only channels idle across the viewport.  Each is the HEAD of
    // a chained chip buffer whose second sprite is that channel's lower-region element (energy,
    // altimeter terrain, left band triangle), so they need no copper re-point at all; see the
    // chaining note in FlightCopperList.cpp for why that mattered.
    //
    // The burst builds in the flight VBI (50Hz) and the P3 object at render rate, so ownership is
    // arbitrated: the burst wins.  In practice they never overlap — the object that explodes is
    // deactivated ($006A = $FF) by the hit, so draw_player3_object stops drawing it.
    // ⚠ DOUBLE-BUFFERED, and it has to be: segment 0 of the burst (shotSprite/shotSpriteBack)
    // already is, so single-buffered extensions would reach the screen ONE FRAME EARLIER than
    // segment 0 — during a burst that changes shape every VBI that reads as a torn, misshapen
    // explosion.  The same argument the shot's own double buffer rests on applies twice over: the
    // VBI sprite bracket measures ~29 beam-lines and runs AFTER the heavy vbi_handler_flight, so
    // the build routinely lands past scanline 86 and an in-place write races the sprite DMA.
    // So each channel gets TWO chains; the owner writes the off-screen one and re-points SPRxPT,
    // and the copper latches all four segments together at the next frame.  The chained lower
    // element (energy / altimeter / left triangle) therefore exists in BOTH chains — cheap,
    // because all three are build-once solids whose only per-frame change is setY (mirrorSprite).
    enum WideOwner { kWideNone = 0, kWideShot = 1, kWideP3 = 2 };
    Sprite*   wideExt[3][2]   = { { nullptr, nullptr }, { nullptr, nullptr }, { nullptr, nullptr } };
    Sprite*   wideLow[3][2]   = { { nullptr, nullptr }, { nullptr, nullptr }, { nullptr, nullptr } };
    uint16_t* wideChain[3][2] = { { nullptr, nullptr }, { nullptr, nullptr }, { nullptr, nullptr } };
    uint8_t   wideOwner       = kWideNone;
    uint8_t   wideDispIdx     = 0;              // which chain the copper currently enters
    int       widePrevBase[3][2] = { {0,0}, {0,0}, {0,0} };  // rows each extension buffer's last
    int       widePrevRows[3][2] = { {0,0}, {0,0}, {0,0} };  // build wrote = its clear extent
    // Claim the extensions for `owner` (full-clearing on a handover, since the outgoing owner's
    // incremental clear only tracked its own rows); false if the burst already holds them.
    bool wideExtAcquire(uint8_t owner);
    // Blank the extensions and drop ownership — call when the object goes narrow or inactive.
    // ⚠ For the burst this is DEFERRED like a build (off-screen chain + SPRxPT re-point), because
    // its segment 0 is double buffered and still shows the wide strip this frame; pass now = true
    // only from a caller that blanks segment 0 in both buffers at once.  See the definition.
    void wideExtRelease(uint8_t owner, bool now = false);
    // Render one widened player strip across 1/2/4 segments.  dst0 = the object's own sprite data
    // (past the 2 control words); src = &mem[<PMG page> + top], `rows` consecutive bytes; base =
    // the strip's first row within the FIXED-VSTART sprites; scale = 1/2/4; x = segment 0's Amiga
    // sprite X.  Falls back to segment 0 alone if a higher-priority owner holds the extensions.
    void buildWideObject(uint16_t* dst0, const volatile uint8_t* src, int base, int rows,
                         int scale, uint16_t x, uint8_t owner);

    // Stars/space starfield: the 3 Atari players P0/P2/P3 ($0C32/$0E32/$0F32),
    // scrolled + sparsely seeded by the genuine scroll_field_columns ($6AEE).  Each is a
    // SIZEP=$03 quad player spanning 32 colour clocks, drawn faithfully as a PAIR of Amiga
    // sprites (low + high 16 px) on channels 2/3 (P0), 4/5 (P2), 6/7 (P3).  Channel 2 is
    // shared with the throttle gauge (P1 = $0D32), which sits lower in the cockpit and never
    // overlaps the stars — PlanetCopperList re-points SPR2PT to the gauge at the band scanline.
    // Layout: starSprite[2c] = player c low, starSprite[2c+1] = player c high.
    Sprite*  starSprite[6] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };

    // Zero-copy scroll: instead of rebuilding the whole sprite from mem[] every frame, each
    // star sprite's pixel data lives in an oversized "ring" chip buffer holding the WHOLE
    // scene's worth of star rows laid out linearly (the field scrolls up a known, fixed
    // maximum over the cinematic — measured 595 rows; sized generously below).  Each frame the
    // field scrolls up by N rows (0 or 1 normally); we advance a window pointer by N (re-pointing
    // the copper SPRxPT), convert only the N brand-new rows into the ring, and leave every
    // already-converted row untouched.  Control words are written at the current window slot
    // (which was a star row that has since scrolled off the top); the 5 blank padding rows + the
    // terminator are simply the not-yet-written (zero, MEMF_CLEAR) slots below the star region.
    // The whole update runs at vblank (starVblankUpdate, from the VBI ISR) so the pointer +
    // control-word writes are tear-free — the flightVblankSwap pattern.  ⚠ Do NOT move any part
    // of it back into the render pass: each step has a beam deadline (scanline 16 / 25 / 44) and
    // a fast CPU lands the render pass above them.  starVblankUpdate's header has the detail.
    uint16_t* starRing[6] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
    uint16_t  starCtl[6][2] = {};          // precomputed POS/CTL words per sprite (constant)
    int       starWindow = 0;              // current window start slot in the ring (control slot)
    unsigned short starLastGen = 0;        // last consumed g_starScrollGen (delta = rows scrolled)
    bool      starSpritesValid = false;    // false → perFrameWork does the full (re)build
    volatile bool starPhaseActive = false; // set by perFrameWork (rsStars); read by the VBI ISR
    void buildStarSprites();               // full (re)build: convert all rows into the ring at window 0

    // Tunnel reveal: a 3bp concentric-rectangle bitmap shown in the door gap.
    // Motion is palette cycling — the 6-entry ring lives in mem[$08D4-$08D9]
    // (feeds COLOR01-06) and is rotated by the native ports of step_accum_add_75
    // ($6A38) -> advance_history_6a4d ($6A4D), gated by $0088 (tunnel_ring_tick_native).
    Bitmap*  tunnelBitmap = nullptr;

    void buildPostSprites();   // decode RLE tables $4DFA/$4E09 -> leftPost/rightPost (once)
    void buildAltimeterSprite();   // mirror the live P0 $0C98 terrain-height bar -> altimeterSprite (flight)
    void buildAltimeterShipSprite();   // mirror the live M3 $0B98 ship-height bar -> altimeterShipSprite (flight)
    bool postsBuilt = false;   // canopy posts are constant: decode them a single time
    void decodeCompass();      // decode the 4 compass cells $32E3-$32E6 -> title bitmap (16 longwords)
    void decodeCockpitSpan(uint16_t addr, uint8_t nCells);  // decode nCells cockpit cells from Atari screen addr
    void decodeCockpitFull();  // decode the whole cockpit region (modeD + mode4) once (scene-entry repaint)
    void decodeLockonDirty();  // decode only the lock-on cells ($3491+) a writer actually touched
#ifdef ROF_CK_VERIFY
    void ckVerifySnap();   // snapshot the verified groups' SOURCE cells before the cockpit block
    void ckVerifyAll();    // after it: prove every targeted decode left the cockpit fully up to date
#endif
    void decodeTitleScreen();  // decode the whole Title Screen text ($365B/charset $0400) -> titleScreenBitmap
    void decodeTitleCells(int cellLo, int cellHi);  // (re)decode Title Screen cells [lo..hi] (clears+ORs); targeted value updates
    void decodeBoostStars();                       // boost cinematic: the $2000 starfield -> viewportBitmap (the rings are painted, not decoded)
    // Who owns tunnelBitmap right now — nobody, the FORWARD launch tunnel, or the BOOST reverse
    // cinematic.  The two directions share every field writer (draw_symmetric_span_loop /
    // plot_terrain_span / draw_frame_guide_columns) and differ ONLY in the GTIA->pen LUT, so the
    // owner is what tunnelPen() and the prime pen select on.  NONE = the hooks don't paint.
    enum : uint8_t { kTunnelOwnerNone = 0, kTunnelOwnerForward = 1, kTunnelOwnerBoost = 2 };
    uint8_t tunnelOwner = kTunnelOwnerNone;
#ifdef ROF_TUNNEL_DIFF
    void tunnelPaintDiff(uint16_t K);   // painted bitmap vs a decode of the live $1000 field, rows [K, 85-K]
    bool tunnelDiffPending = false;     // check the finished forward pre-draw on the next render
#endif
    void renderViewportModeD(uint16_t srcBase, int stride, int rows); // decode CHANGED mode-D bytes -> viewportBitmap (stars/planet: $1000/48/47)
    void renderFlightDirect();   // flight terrain: plot sky straight to bitplanes from $260E (replaces the convert)

    // Static-Standby fixed copper list (built once, poked in place — see
    // StandbyCopperList).  Used while in the settled Standby/gauge-fill state
    // (rsStandby && g_doorFieldReady && !rsViewport && !rsLaunched): no per-frame
    // full rebuild.  renderFrame switches to the Doors/Tunnel lists for the launch
    // cinematic phases.
    StandbyCopperList* standbyCopper = nullptr;
    bool standbyCopperInstalled = false;   // is standbyCopper the currently-installed list?
    void updateStandbyCopper(bool force);  // poke changed colour/sprite slots (force = all)
    // Last-poked values, so updateStandbyCopper only writes a MOVE when it changed.
    uint16_t sbTitleBg = 0xFFFF, sbTitlePf0 = 0xFFFF, sbEnergyCol = 0xFFFF, sbCompassCol = 0xFFFF;
    uint16_t sbTerr0 = 0xFFFF, sbTerr1 = 0xFFFF, sbTerr2 = 0xFFFF, sbTerr3 = 0xFFFF;
    int8_t   sbEnergyIndicator = -1;   // sprite-2 = gauge(1)/null(0); -1 = unset

    // Static stars/planet viewport fixed copper list (the rsViewport non-flight layout —
    // line-doubled mode-D band).  Same build-once + poke-in-place scheme as standbyCopper:
    // renderViewportModeD updates the bitmap content (constant pointer), buildStarSprites
    // updates the sprite data (constant pointers), so only a few colours change per frame.
    PlanetCopperList* planetCopper = nullptr;
    bool planetCopperInstalled = false;  // is planetCopper the currently-installed list?
    void updatePlanetCopper(bool force); // poke changed colour slots (force = all)
    // Last-poked values (vp* — separate from sb* so a phase switch always force-refreshes).
    uint16_t plTitleBg = 0xFFFF, plTitlePf0 = 0xFFFF, plEnergyCol = 0xFFFF,
             plStarCol = 0xFFFF, plBg = 0xFFFF, plCompassCol = 0xFFFF;

    // Static flight fixed copper list (scene 7 — same line-doubled mode-D band, with the
    // flight terrain palette + HUD sprites).  Same build-once + poke-in-place scheme:
    // renderViewportModeD updates the terrain bitmap content, the ported flight VBI pokes
    // the per-frame colours/sprites via updateFlightCopper.
    FlightCopperList* flightCopper = nullptr;
    bool flightCopperInstalled = false;  // is flightCopper the currently-installed list?
    void updateFlightCopper(bool force); // poke changed colour/sprite slots (force = all)
    // Last-poked values (fl* — separate from sb*/pl* so a phase switch force-refreshes).
    uint16_t flTitleBg = 0xFFFF, flTitlePf0 = 0xFFFF, flTitlePf1 = 0xFFFF;
    uint16_t flEnergyCol = 0xFFFF, flCompassCol = 0xFFFF;
    uint16_t flTerr0 = 0xFFFF, flTerr1 = 0xFFFF;   // terrain pen0/pen1 (atmosphere ramp $00DC/$00DD)
    uint16_t flTerr2 = 0xFFFF, flTerr3 = 0xFFFF;   // terrain pen2/pen3 (atmosphere ramp $00DA/$00DB)
    uint16_t flBand3 = 0xFFFF;   // wing-band color03 = COLPF2 frame grey ($00D4); other pens inherit terrain
    // HUD sprite colours whose Atari DLI ($4A78) source is a $00CF-$00D6 display param that the death
    // cinematic ($4FE0) ramps to salmon — poked on change so they fade with the cockpit (constant in
    // normal flight → free): altimeter terrain P0 ← $00D5, ship M3 ← $00D6, AH ground P2 ← $00D0.
    uint16_t flAltimCol = 0xFFFF, flAltimShipCol = 0xFFFF, flAHGround = 0xFFFF;
    // Cockpit-region pens: the Atari dashboard DLI $4A78 reloads them from $00CF-$00D4 EVERY frame,
    // so they are driven live + poked on change (constant in normal flight → free).  Cached as the
    // six mem[] SOURCE bytes rather than the eight OCS words: the mapping duplicates $00D3/$00CF/
    // $00D4, so six byte compares settle it and no atariToOCS runs on an unchanged frame.
    uint8_t  flCkD3 = 0xFF, flCkCF = 0xFF, flCkD4 = 0xFF, flCkD1 = 0xFF, flCkD0 = 0xFF, flCkD2 = 0xFF;
    uint8_t  flNeedleD1 = 0xFF;  // compass needle/heading COLPF2 — also $00D1 (see updateFlightCopper)

    // Launch-cinematic fixed copper lists:
    //   DoorsCopperList (scene 4) — hangar doors parting, sliding 3-band geometry poked
    //     each frame from the door-scroll counter (g2 = 0x2B - $008A).
    //   TunnelCopperList (scene 5) — doors fully open (g2 == kTerrainHeight/2), one full
    //     tunnel band with the cycling $08D4-$08D9 ring palette.
    // Both share the constant title + cockpit regions; only their terrain region differs.
    //
    // DoorsCopperList is DOUBLE-BUFFERED: its band WAIT lines + bitplane pointers move
    // every frame, and poking those in place on the live (displayed) list races the
    // copper — the beam can read a half-updated band and the tunnel reveal jitters ±1px
    // (render() runs before the poke, so the beam is usually already past the top border).
    // So updateDoorsCopper() fully populates the BACK buffer and setCopperList swaps it at
    // the next vblank (atomic).
    //
    // The Tunnel list is DOUBLE-BUFFERED for the same reason.  Its FORWARD geometry is fixed
    // (one full-height band), but the boost reverse cinematic drives setRevealBands(), which
    // moves band WAIT lines AND bitplane pointers every frame — and a pointer poked on the live
    // list can be read half-written by the copper, garbaging the whole viewport for a frame.
    // Since every slot is rewritten into the back buffer anyway, the old tn* poke-on-change
    // cache is gone: it would have been comparing against the wrong buffer's state.
    DoorsCopperList*  doorsCopper[2] = { nullptr, nullptr };
    uint8_t doorsActive = 0;
    void updateDoorsCopper(DoorsCopperList* dc);  // fully populate one buffer (colours + geometry)
    TunnelCopperList* tunnelCopper[2] = { nullptr, nullptr };
    uint8_t tunnelActive = 0;
    bool tunnelCopperInstalled = false;
    void updateTunnelCopper(TunnelCopperList* tc);  // fully populate one buffer (colours + reveal bands)
    void showTunnelCopper();      // populate the back buffer and swap it in at the next vblank
    uint16_t boostRevealK() const;  // first viewport row showing rings (43 = nothing revealed yet)

    Bitmap*     titleBitmap    = nullptr;
    Bitmap*     terrainBitmap  = nullptr;
    // Shared single-buffered pre-flight viewport bitmap: Standby (closed doors), Doors (door halves),
    // Planet, Stars — the scenes that are mutually exclusive in time and never composite together in
    // one frame.  (The tunnel reveal is the exception: it's shown ALONGSIDE the door halves during the
    // Doors scene, so it keeps its own tunnelBitmap.)  DEDICATED — NOT shared with flight's
    // terrainBitmap double-buffer, so flight-side rendering can never clobber a still-displayed
    // pre-flight frame at a scene handoff (was the one-frame planet→flight black-band glitch: the
    // flight renderer cleared the shared buffer mid-frame while the outgoing planet copper still
    // displayed it).  This mirrors the Atari, where the pre-flight scenes are single-buffered (one
    // $1000/$2000 field) and only flight double-buffers ($1070, two halves).  Written by the Standby/
    // Doors door-field decode and by renderViewportModeD (planet/stars).
    Bitmap*     viewportBitmap   = nullptr;
    // Level-select "elevator" door scroll (post-mother-ship SELECT).  A TALL door bitmap (the 85
    // $2000 field rows + a viewport-height green-door pad below) that the door-scroll ISR points the
    // standby terrain BPLxPT into, offset by dl_src_index ($008B) rows, to reproduce the Atari's
    // dl_lms_scroll_step DL-LMS window scroll.  Decoded per active scroll frame in doorScrollVblankUpdate;
    // doorScrollActive tracks whether we've repointed away from viewportBitmap (to restore on rest).
    Bitmap*     doorScrollBitmap  = nullptr;
    bool        doorScrollActive  = false;
    void        decodeDoorScrollField();   // decode $2000 (85 rows) + green pad -> doorScrollBitmap
    void        decodeDoorScrollRows(unsigned r0, unsigned r1);  // ...just field rows r0..r1
    void        decodeDoorScrollDirty();   // consume g_doorScrollFieldDirty over its marked rows
    // Flight terrain double-buffer: renderFlightDirect rebuilds the whole terrain region
    // every frame (blitter clear + sky fill), so it must NOT paint the live displayed buffer
    // mid-frame (caused plane1 flicker).  It renders into the off-screen one of these two and
    // re-points the flight copper's viewport bitplane ptrs to it; the swap latches next vblank.
    Bitmap*     terrainBitmapBack = nullptr;   // 2nd flight terrain buffer (== terrainBitmap dims)
    // Dot side-buffer: the terrain rasterizer ORs its plane2 dots (the DOMINANT flight compute) into
    // this DEDICATED off-display scratch, NOT a display buffer — so the compute no longer depends on
    // the double-buffer flip and can run DURING the flip's vblank wait (reclaiming the idle spin).
    // renderFlightDirect copies this plane2 into the display back buffer each frame (the "re-added
    // plane2 copy"), then clears the scratch for the next frame.  Same dims/layout as terrainBitmap
    // so the rasterizer's kRow120 (stride-120) geometry lands identically; only its plane2 is used.
    Bitmap*     terrainDotBuffer  = nullptr;
    Bitmap*     flightDisplayed   = nullptr;   // which buffer the flight copper currently shows
    Bitmap*     flightPendingFlip = nullptr;   // buffer renderFlightDirect just painted, awaiting the
                                               // vblank-safe pointer swap (done by flightVblankSwap)
    volatile bool flightSwapPending = false;   // main thread raises it; the VBI does the swap + clears
    volatile bool flightFlipDeferred = false;  // normal path deferred its flip-wait (no busy-wait after
                                               // the flip); renderFrame then SKIPS its own vblank wait so
                                               // the next terrain compute overlaps this flip's vblank.
    Bitmap*     flightClearPending = nullptr;  // back buffer whose terrain rows a post-vblank blitter
                                               // clear is currently clearing (overlapping the draw)
    Bitmap*     cockpitBitmap  = nullptr;
    Bitmap*     titleScreenBitmap = nullptr;   // full-screen 3bp text bitmap for the Title Screen

    // Title Screen (attract/level-select/results) fixed copper list — full-screen text
    // bitmap, black COLBK, 4 cycling text pens.  Same build-once + poke scheme as standbyCopper.
    TitleScreenCopperList* titleScreenCopper = nullptr;
    bool titleScreenCopperInstalled = false;
    void updateTitleScreenCopper(bool force);  // poke color01-04 = COLPF0-3 (cycling)
    uint16_t tsPf0 = 0xFFFF, tsPf1 = 0xFFFF, tsPf2 = 0xFFFF, tsPf3 = 0xFFFF;  // last-poked
    // ---- the two BOOT scenes: Logo (INITAD $5000) and Station cinematic (INITAD $1A97) --------
    // Both are ANTIC mode F under GTIA mode 9 = 4 bitplanes of one hue's 16 luminances, so they
    // share one field bitmap, one copper-list class and one decoder (see Gtia9CopperList).  The
    // bitmap is sized for the STATION, the larger of the two: the Atari scrolls by walking its
    // display list's JMP operand down 340 mode-F entries, and we decode ALL of them ONCE into one
    // tall 320x340 bitmap (54 KB chip) and scroll by moving the four bitplane pointers, which
    // makes the scroll free.  The logo uses rows 0..61 of the same buffer.
    // Per frame only the animated bytes come back: the dirty rectangles station_sub_1EB4 /
    // station_chan_step record (< 200 bytes, rof_manual.h) and, while station_star_fade_in is
    // still running, the ~30 star rows it brightens.  Allocated in initialize() and freed in
    // shutdown() like every other buffer — but only when the boot scenes are actually built in,
    // so `make SKIPBOOT=1` (which PROBES/FPSCOUNT imply) keeps its chip footprint unchanged.
    Bitmap*          bootFieldBitmap = nullptr;
    Gtia9CopperList* bootFieldCopper = nullptr;
    bool             bootFieldCopperInstalled = false;   // is it the live list? (stationVblankUpdate's gate)
    unsigned char    bootFieldScene = 0;                 // which scene the live LAYOUT was built for
    void renderBootScene();                        // the whole per-frame render for scenes 1 and 2
    void decodeLogoField();                        // scene 1: its 62 rows (at entry, then at "GAMES")
    void decodeStationField();                     // all 340 display-list rows (once, at entry)
    void decodeStationDirty();                     // consume the recorded dirty rectangles
    void decodeStationStars();                     // re-decode the star rows (during the fade-in)
    // Which bitmap rows are STAR rows — i.e. whose display-list LMS lands in the range
    // station_star_fade_in walks ($2CB8..$3168; it seeds $90/$91 = $2CB8 and stops at $3168).
    // Collected during decodeStationField so the fade-in re-decode touches nothing else.
    // display_list_build caps them at 30 (encounter_count = $1E); sized with headroom.
    unsigned short stationStarRow[40] = {};
    unsigned char  stationStarRows = 0;
    unsigned short stationWindowRow = 0xFFFF;      // last row published to the copper

    // The station's PMG, mirrored to Amiga sprites (buildStationSprites, run in the VBI ISR
    // alongside the scroll — PMG that animates every frame must update from the VBI).
    // CHANNEL MAP.  OCS gives one colour per sprite PAIR (0/1 -> COLOR17, 2/3 -> COLOR21,
    // 4/5 -> COLOR25, 6/7 -> COLOR29) and the Atari elements have four different colours, so each
    // takes its own pair — except the two that genuinely share one:
    //   ch0  spacecraft pair 0   COLOR17 = COLPM0 $06   P0 ($3400) in the sprite's plane A and
    //                            COLOR18 = COLPM1 $0A   P1 ($3500) in plane B, so the pen DATB<<1|
    //                            COLOR19 = $06|$0A=$0E  DATA reproduces PRIOR $71's multi-colour
    //                                                   player: the overlap (the fuselage) is the
    //                                                   OR.  ⚠ NOT an attached pair — that would
    //                                                   be 4 planes and land on COLOR20/21.
    //   ch4  P2 dot     pair 2   COLPM2 = $1F40[i]   left converging dot  ($3600)
    //   ch5  P3 dot     pair 2   COLPM3 == COLPM2    right converging dot ($3700) — free pairing
    //   ch6  missiles   pair 3   COLPF3 = $34        the 5th player's dots ($3300)
    //   ch7  missiles   pair 3   ...a second chain, for the scanline that carries two dots
    // Channel order also gets the Atari's priority right: PRIOR $71 is priority mode 1, where the
    // players sit in front of PF2/PF3 and rank P0 > P1 > P2 > P3 with the 5th player (missiles as
    // COLPF3) behind them all — which is exactly SPR0 > ... > SPR7 with the missiles last.
    // Each element is read back out of its real PM buffer rather than recomputed, so the mirror
    // cannot drift from the 6502 (and the writers' clear-behind falls out for free).
    Sprite*  stationSpr[3] = { nullptr, nullptr, nullptr };   // spacecraft (P0+P1), P2 dot, P3 dot
    Sprite*  stationMsl[2] = { nullptr, nullptr };   // the two missile-dot CHAINS (ch6/ch7)
    void     buildStationSprites();
    uint16_t stationDotCol   = 0xFFFF;   // last COLPM2/3 published (poke only on change)

    // Scene 1's single PMG element: the sparkle.  $5000 builds a 15-scanline "✦" in player 0's
    // buffer ($0C00, PMBASE $0800) at the fixed HPOSP0 $C1, then fades COLPM0 $0F->$00 over 16
    // two-frame steps while eating the shape away from both ends.  One Amiga sprite on channel 0
    // (pair 0 -> COLOR17), read back out of the real PM page by the same pmRun() + kDoubleGlyph
    // mirror the station's players use.  Its COLOUR cannot come from mem[]: the Atari writes
    // COLPM0 ($D012), which bus.h does not shadow (only HPOSP0-3/HPOSM0-3 $D000-$D007 are) — so
    // rof_logo.c publishes it as g_logoSparkleCol.  Updated from the VBI ISR like every other
    // animating PMG mirror in the port (the feedback-vbi-driven-pmg-sprites memory).
    Sprite*  logoSparkle    = nullptr;
    void     buildLogoSparkle();
    uint16_t logoSparkleCol = 0xFFFF;    // last COLOR17 published (poke only on change)
    unsigned char logoFieldGen = 0;      // g_logoFieldGen value the decoded field is from

    // Blank black list shown until g_standbyRevealReady latches (boot/standby build in
    // progress) — switched to the real lists in renderFrame once ready.
    EmptyCopperList* emptyCopper = nullptr;
    bool emptyCopperInstalled = false;
    Sprite*     leftPost       = nullptr;
    Sprite*     rightPost      = nullptr;
    Sprite*     nullSprite     = nullptr;
    // Flight (scene 7) windscreen-frame sprites: the A-pillar + the band-bottom
    // windscreen-corner triangle, which on the Atari is a SEPARATE element from the
    // Planet bitmap (players P0/P1, repositioned + double-width at the band DLI $4A40).
    // Four sprites: each side's ~28px double-width triangle is two 16px Amiga sprites.
    // flLeftPost/flRightPost carry the A-pillar (rows 86-171) AND the inner 16px of the
    // triangle (band rows 172-179); flLeftTri/flRightTri carry the outer 16px (band only).
    // Dedicated to flight so the shared leftPost/rightPost stay 86-row pillars for the
    // other scenes (extending those would draw the flight wedge into Standby/Planet too).
    Sprite*     flLeftPost     = nullptr;
    Sprite*     flRightPost    = nullptr;
    Sprite*     flLeftTri      = nullptr;
    Sprite*     flRightTri     = nullptr;
    void buildFlightFrameSprites();  // decode A-pillars ($4DFA/$4E09) + band wedges ($4DD2/$4DDA)

    // Artificial Horizon (#6): the brown ground-fill is the Atari player P2 (COLPM2=$26),
    // multiplexed below the windscreen frame.  ch0/ch1 are reused (copper re-points SPR0PT/
    // SPR1PT in the gap below the frame's VSTOP) for two 16px sprites = the 32px-wide dial
    // fill.  Decoded each flight frame from the live P2 buffer (mem[$0E92..], = GRAFP2 per
    // scanline) so the horizon tracks pitch.  Shown BEHIND the playfield so the bitmap dial
    // frame (value-1/2 glyphs) stays in front and the brown shows through the value-0 centre.
    Sprite*     ahLeft         = nullptr;
    Sprite*     ahRight        = nullptr;
    uint8_t     ahLastIdx = 0xFF, ahLastSub = 0xFF;   // AH ground-fill change-detect (pitch index $291C/$291D)
    void buildAHSprite();  // mirror the live P2 fill ($0E92..) -> ahLeft/ahRight (flight AH ground)

    // The targeted object (gun emplacement / flying saucer) is a single generic Atari player-3
    // sprite (draw_player3_object $42A7): shape+size from data tables by class+distance, written
    // into TWO P3 buffer copies in one loop.  We mirror each copy to one Amiga sprite (shape-
    // agnostic — the enemy TYPE is just different table data, not different code):
    //   Targeting Scope (#8) copy = mem[$0F00+$98..$B8], behind the bitplanes, on ch3.
    Sprite*     scopeP3Sprite = nullptr;
    int         p3ScopePrevRows = 0;     // rows written last frame (clear only those, not the full sprite)
    void buildScopeP3Sprite();    // mirror the P3 Targeting-Scope copy ($0F00 $98..$B8) -> scopeP3Sprite

    //   Main Window (#9) copy = mem[$0F00+$32..$85], IN FRONT of terrain, tracking X (HPOSP3
    //   $2870) + Y (buffer offset).  Reuses ch7 (altimeter ship, free in the viewport) via the
    //   copper SPR7PT multiplex; colour COLPM3=$00D9 -> COLOR31.  Present only when the target is
    //   close enough to have a Main-Window body (far targets: Targeting-Scope copy only).
    Sprite*     viewportP3Sprite = nullptr;
    int         p3ViewportPrevBase = 0, p3ViewportPrevRows = 0;  // rows written last frame (incremental clear)
    void buildViewportP3Sprite(); // mirror the P3 Main-Window copy ($0F00 $32..$85) -> viewportP3Sprite

    //   Long Range Scanner (#13) guide dot = Atari missile M2 (a 3-scanline blob).  Vertical
    //   position (RANGE to the downed pilot) = the M2 bits in the missile DMA buffer $0B00
    //   (dashboard band); horizontal position (BEARING) = mem[$00CE] (the flight dashboard DLI
    //   $4AC7 loads it into HPOSM2); colour = COLPM2 $26 (red).  Reuses ch2 (right A-pillar, runs
    //   to VSTOP 180 → arms) via the copper SPR2PT re-point.  Read-only mirror: the native flight
    //   VBI already writes the buffer + $CE into mem[], so no writer port / dirty hook is needed.
    Sprite*     scannerDotSprite = nullptr;
    int         scannerPrevRows = 0;  // rows written last frame (clear only those, not the whole sprite)
    void buildScannerDotSprite(); // mirror the M2 scanner dot (missile buf $0B00 + $CE) -> scannerDotSprite

    // ---- the six flight sprite builders, DEFERRED into renderFlightDirect's blitter shadows ----
    // They are pure CPU on sprite buffers and touch neither the terrain bitmap nor the dot side
    // buffer, so they have no dependency on any of the terrain blits — yet they used to run in
    // perFrameWork(), i.e. BEFORE renderFlightDirect kicked those blits, leaving the CPU to stall
    // on them afterwards (measured, blit_shape.gdb: 12 ticks/painted frame on the plane1 clear +
    // 17 on the sky fill = ~1.2% of all flight wall clock).  Now perFrameWork only marks them
    // owed and renderFlightDirect runs them in the two blits' shadows.
    // ⚠ renderFlightDirect has three early-return paths (no bitmaps / the rescue-figure pause / a
    // frame with no fresh terrain), so buildFlightSpritesFlush() runs after render() as the safety
    // net — without it the sprites would freeze on exactly those frames.
    uint8_t flightSpritesOwed = 0;    // bit0 = early slot owed, bit1 = late slot owed
    void buildFlightSpritesEarly();   // in the plane1-clear blit's shadow
    void buildFlightSpritesLate();    // in the sky-fill blit's shadow
    void buildFlightSpritesFlush();   // whatever the render path skipped

    // Dirty-flag bitmap caching: bitmaps are rendered once on initialize() and
    // only re-rendered when the underlying mem[] data changes.
    bool    terrainDirty = true;   // re-render terrain rows from $2000
    int     titleRendered = 0;     // # of top-bar title cells painted last time (see g_titleToRender)

    // Long-granular shadow for the stars/planet/flight mode-D viewport.  Holds the last
    // decoded source as 430 longs (43 rows × 10 longs of 4 packed bytes); renderViewportModeD
    // re-decodes only the 4-byte groups that changed (the planet zoom / star scroll leaves
    // much of the field static frame-to-frame).  forceFull re-decodes everything (and clears
    // plane3) on the first frame and whenever the source base changes (stars↔flight).
    bool     viewportForceFull = true;
    bool     viewportClearKicked = false;    // entry-frame terrain-bitmap clear kicked in perFrameWork (overlaps buildStarSprites)
    uint16_t viewportLastBase  = 0;
    uint32_t viewportShadow[47 * 10] = {};   // 47 mode-D rows (43 terrain + 4 wing-clearance band)

    // Cockpit decode is writer-driven per instrument: each writer raises one g_ck* boolean
    // (digits / lock-on / dial) and render() decodes only that instrument's cells — no
    // per-frame full scan / shadow compare.  cockpitForceFull forces a one-time full repaint
    // of the whole region (scene entry, when the transpiled boot_standby_launch_driver — not a hooked
    // writer — built the cockpit).
    bool    cockpitForceFull = true;
};
