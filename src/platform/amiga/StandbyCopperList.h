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
// (display_setup entry) — so the global fade is always 16 (fadeColor identity)
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
    void setTerrainPalette(uint16_t p0, uint16_t p1, uint16_t p2, uint16_t p3);  // terrain color00..03
    void setTerrainBgColor(uint16_t c);                            // terrain color03 only (green fade)
};
