#pragma once
#include "framework/CopperList.h"

class Bitmap;
class Sprite;

// DoorsCopperList — the fixed-structure copper list for the hangar doors parting
// (scene 4): the launch cinematic frames before the doors fully open (door-scroll
// g2 = 0..kTerrainHeight/2 - 1).  The terrain region is three bands: a top terrain band
// that slides up, a tunnel reveal in the widening gap, and a bottom terrain band that
// slides down.
//
// Unlike the other scene lists, the doors' GEOMETRY moves every frame, so this list
// can't be pure colour-poke.  Instead the list is allocated ONCE for the maximal
// 3-band layout, and update() pokes each band's WAIT scanline, bitplane pointers and
// colours in place each frame — collapsed bands (e.g. the closed-door first frame has
// only the top band) are filled with copper NOPs (MOVE $1FE,0).  No re-emit of the
// whole list, no double buffer.  The title and cockpit regions are constant (built
// once, only their colours poked), exactly as in StandbyCopperList.
class DoorsCopperList : public CopperList {
public:
    DoorsCopperList();

    // One-time layout: emit the constant title + cockpit regions and the 3-band terrain
    // skeleton.  The per-frame colour/geometry slots are seeded; update() refreshes them.
    void buildLayout(const Bitmap& title, const Bitmap& cockpit,
                     const Sprite& leftPost, const Sprite& rightPost,
                     const Sprite& gauge, const Sprite& nullSprite);

    // ---- constant-region per-frame setters (each pokes one MOVE) ----
    void setTitlePalette(uint16_t bg, uint16_t pf0, uint16_t pf1);  // color00..03 (col0=col3=bg)
    void setSpritePostColor(uint16_t c);                           // color17 (canopy posts)
    void setEnergyIndicatorColor(uint16_t c);                                // COLOR21 ($1AA) gauge bar
    void setCompassColor(uint16_t c);                              // color01 over the compass band

    // ---- terrain geometry (the sliding doors) ----
    // Pokes the whole 3-band terrain region for the current door-scroll progress g2
    // (0 = closed, kTerrainHeight/2 - 1 = nearly open).  topBase/tunBase/botBase are the
    // CHIP-RAM byte addresses of the top terrain row, the tunnel reveal row and the
    // bottom terrain row.  bandBg = pen0 = COLBK green ($0071) — set on band0 and inherited
    // by the tunnel/bottom bands and the windscreen band below (the door field decodes
    // COLBK→pen0).  terr1/terr2 = terrain pens 1-2 (color01-02); terrDots = pen3 = road-dot
    // dark ($02C0).  ring0_2 = ring pens 4-6 (color04-06, $08D4-$08D6); ring3_5 = tunnel
    // pens 1-3 (color01-03, $08D7-$08D9).
    void update(uint16_t g2,
                uint32_t topBase, uint32_t tunBase, uint32_t botBase,
                uint16_t bandBg, uint16_t terr1, uint16_t terr2, uint16_t terrDots,
                uint16_t ring0, uint16_t ring1, uint16_t ring2,
                uint16_t ring3, uint16_t ring4, uint16_t ring5);
};
