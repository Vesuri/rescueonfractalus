#pragma once
#include "framework/CopperList.h"

class Bitmap;
class Sprite;

// StandbyCopperList — the fixed-layout copper list for the static Standby screen
// (scene 3): title + terrain/doors + cockpit — the layout used while !rsViewport &&
// !rsLaunched (the settled Standby/gauge-fill state).
//
// Built ONCE in buildLayout() (geometry, bitmap pointers, sprite pointers, the
// constant cockpit palette), then the few per-frame-varying colours/sprite are
// poked in place via the setters below — no per-frame full rebuild, no double
// buffer.  Mirrors the MenuCopperList pattern (DanceDiverse3): INDEX_* constants
// address each section, setters write a single 32-bit MOVE word that the copper
// reads at most once per frame (a one-frame torn colour is invisible).
//
// Used only while g_doorFieldReady is latched, which is AFTER g_standbyRevealReady
// (boot_standby_launch_driver entry) — so the global fade is always 16 (fadeColor identity)
// here: the setters take already-OCS colours (= atariToOCS(byte)) with no fade.
class StandbyCopperList : public CopperList {
public:
    StandbyCopperList();

    // One-time layout: emit the whole fixed list (playfield, bitmap pointers, sprite
    // pointers, geometry waits/bplcon/modulo, constant cockpit palette, terminator).
    // The dynamic colour/sprite slots are seeded; the caller refreshes them each frame.
    void buildLayout(const Bitmap& title, const Bitmap& terrain, const Bitmap& cockpit,
                     const Sprite& leftPost, const Sprite& rightPost, const Sprite& nullSprite);

    // ---- per-frame setters (each pokes one MOVE) ----
    void setTitlePalette(uint16_t bg, uint16_t pf0, uint16_t pf1);  // color00..03 (col0=col3=bg)
    void setSpritePostColor(uint16_t c);                           // color17 (canopy posts)
    void setSprite2(const Sprite& s);                              // sprite-2 ptr (gauge or null)
    void setEnergyIndicatorColor(uint16_t c);                                // COLOR21 ($1AA) gauge bar
    void setCompassColor(uint16_t c);                              // color01 over the compass band (COLPF0 $00CF)
    // terrain color00..03: color00 = COLBK green ($0071), color03 = road-dot dark ($02C0)
    // (door field decodes COLBK→pen0; see kNibbleColour).  color00 carries the green into
    // the windscreen band, so there is no separate band-bg setter.
    void setTerrainPalette(uint16_t p0, uint16_t p1, uint16_t p2, uint16_t p3);
    void setTerrainBgColor(uint16_t c);                            // terrain color03 only
    // Rewrite the terrain (door) region as up to MAX_TERRAIN_RUNS per-scanline-LMS "runs" — the
    // level-select "elevator" door scroll (post-mother-ship SELECT).  Reproduces the Atari launch
    // DL's per-scanline mode-F LMS: each run is a maximal block of consecutive field rows the DL
    // maps to consecutive scanlines (the interleaved modulo auto-advances within a run).  Then the
    // CONSTANT cockpit region is re-emitted immediately after the last run (NO no-op padding), so
    // the copper never churns into / delays the cockpit.  startScan[0] must be 0.  Vblank-only —
    // a torn BPLxPT garbles the whole viewport.
    void setTerrainRuns(const Bitmap& b, const uint8_t* startScan, const uint16_t* startRow, int count);

private:
    const Bitmap* cockpitBmp_ = nullptr;   // stored by buildLayout; re-emitted after the runs
    uint32_t emitCockpitRegion(uint32_t idx);  // write the constant cockpit region + terminator; returns next idx
};
