#pragma once
#include "framework/CopperList.h"

class Bitmap;
class Sprite;

// PlanetCopperList — the fixed-layout copper list for the stars/planet launch
// viewport (scene 6, rsStars): title + a line-doubled mode-D viewport band +
// cockpit — the layout used on the rsViewport (non-flight) path.
//
// The viewport band is the reason this needs its own class rather than reusing
// StandbyCopperList: the 43 mode-D rows are each re-displayed on two scanlines by
// toggling BPL1MOD/BPL2MOD (-40 rewind / +80 advance) at every line's H-blank — an
// 85-WAIT line-doubling sub-list that is CONSTANT every frame.  Built ONCE in
// buildLayout() (geometry, bitmap pointers, sprite pointers incl. the three star
// sprites, the line-doubling band, the constant viewport + cockpit palettes), then
// the handful of per-frame-varying colours are poked in place via the setters — no
// per-frame full rebuild, no double buffer.  Mirrors StandbyCopperList.
//
// Used only after g_standbyRevealReady has latched (display_setup entry), so the
// global fade is always 16 (identity): the setters take already-OCS colours
// (= atariToOCS(byte)) with no fade, and the constant palette slots are baked.
class PlanetCopperList : public CopperList {
public:
    PlanetCopperList();

    // One-time layout: emit the whole fixed list.  Sprite pointers (canopy posts,
    // throttle gauge, the six starfield sprites) are all constant for this phase —
    // only their pixel data changes frame-to-frame, which is written to the sprite
    // buffers directly (buildStarSprites/buildEnergyIndicatorSprite), not via the copper.
    // star[6] = the three quad Atari players as low/high sprite pairs (P0 lo/hi → ch 2/3,
    // P2 lo/hi → ch 4/5, P3 lo/hi → ch 6/7).  The throttle gauge shares channel 2: it is
    // re-pointed (SPR2PT) in below the starfield, where the two never overlap vertically.
    void buildLayout(const Bitmap& title, const Bitmap& terrain, const Bitmap& cockpit,
                     const Sprite& leftPost, const Sprite& rightPost, const Sprite& gauge,
                     Sprite* const star[6]);

    // ---- per-frame setters (each pokes one MOVE) ----
    void setTitlePalette(uint16_t bg, uint16_t pf0, uint16_t pf1);  // color00..03 (col0=col3=bg)
    void setSpritePostColor(uint16_t c);                           // color17 (canopy posts)
    void setEnergyIndicatorColor(uint16_t c);                                // COLOR21 ($1AA) gauge bar
    void setStarColor(uint16_t c);                                 // COLOR21/25/29 starfield pens (pairs 2/3,4/5,6/7)
    void setCompassColor(uint16_t c);                              // color01 over the compass band (COLPF0 $00CF)
    void setPlanetBgColor(uint16_t c);                           // viewport color00 (COLBK, space)
    // (The windscreen-bottom band palette is the constant windscreen-FRAME palette set by the
    // launch band DLI $6D67 — black bg / two greys / COLPF2=$2A planet — baked in buildLayout.)
};
