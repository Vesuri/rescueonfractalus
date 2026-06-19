#pragma once
#include "framework/CopperList.h"

class Bitmap;
class Sprite;

// TunnelCopperList — the fixed-layout copper list for the full tunnel descent
// (scene 5): the launch cinematic frames where the hangar doors have fully parted
// (g2 == kTerrainHeight/2) and a single tunnel band fills the whole terrain region:
// one 3bp tunnel band from tunnelBitmap, with the cycling ring palette (pens 1-6 fed by
// the rotating $08D4-$08D9 colour ring).
//
// Built ONCE in buildLayout() (geometry, bitmap/sprite pointers, the constant cockpit
// palette), then the per-frame-varying colours are poked in place via the setters — no
// per-frame full rebuild, no double buffer.  Mirrors StandbyCopperList; the only
// structural difference is the terrain region (a tunnel band with pens 4-6) and that
// the canopy posts + throttle gauge are always present during the cinematic.
class TunnelCopperList : public CopperList {
public:
    TunnelCopperList();

    // One-time layout: emit the whole fixed list (playfield, bitmap pointers, sprite
    // pointers, geometry waits/bplcon/modulo, constant cockpit palette, terminator).
    // The dynamic colour slots are seeded; the caller refreshes them each frame.
    void buildLayout(const Bitmap& title, const Bitmap& tunnel, const Bitmap& cockpit,
                     const Sprite& leftPost, const Sprite& rightPost,
                     const Sprite& gauge, const Sprite& nullSprite);

    // ---- per-frame setters (each pokes one MOVE, or one sprite pointer pair) ----
    void setTitlePalette(uint16_t bg, uint16_t pf0, uint16_t pf1);  // color00..03 (col0=col3=bg)
    void setSpritePostColor(uint16_t c);                           // color17 (canopy posts)
    void setEnergyIndicatorColor(uint16_t c);                                // COLOR21 ($1AA) gauge bar
    void setCompassColor(uint16_t c);                              // color01 over the compass band (COLPF0 $00CF)
    // Tunnel band colours: pen0 = black ($02C0), pens 1-3 = ring[3..5] ($08D7-$08D9),
    // pens 4-6 = ring[0..2] ($08D4-$08D6) — the GTIA mode-10 pixel→ring +3 rotation
    // the Atari tunnel DLI ($6CD7/$6CF1) applies.
    void setTunnelColors(uint16_t pen0, uint16_t pen1, uint16_t pen2, uint16_t pen3,
                         uint16_t pen4, uint16_t pen5, uint16_t pen6);
};
