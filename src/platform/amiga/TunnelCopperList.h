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
    // Moving green->purple corner reveal: greenLine (0..8) = first band scanline still showing
    // the green door (= topmost still-set canopy-post player scanline, mem[$0C88+]).  Above it
    // the corner keeps color00's carried-in colour (tunnel purple); color00 flips to greenColor
    // from greenLine down.  color00 carries from the viewport, so there is NO band-top poke.
    void setBandReveal(uint16_t greenLine, uint16_t greenColor);
    // Neutralise the (late) band color00 reveal slot — write color31 instead of color00, so the band
    // inherits color00 from the viewport.  The boost uses this because it sets its band color00 at the
    // band top (setBandTopColor00) instead; if the late slot ALSO wrote color00 the change would land
    // ~16px into the band's first line (a teal stripe).
    void disableBandReveal();
    // Boost band-corner triangle color00, set at the band top BEFORE the cockpit bitplane-pointer
    // moves (which overrun ~16px into the band's first line and made setBandReveal's late flip land
    // as a stripe).  active=true -> color00=color; active=false -> no-op (color00 inherits the
    // viewport register: the ring $08D8 once drawn, or the $0071 star fade).
    void setBandTopColor00(bool active, uint16_t color);
    // Boost reverse-tunnel reveal: split the terrain region between the RING bitmap (rows
    // [K, 85-K]) and the STAR bitmap (the rows outside it), so the reveal is two WAITs and 12
    // pointer moves instead of recompositing both fields into one bitmap every frame.  K is the
    // first ring row: >= 43 = nothing revealed (stars fill the region), 0 = rings fill it (which
    // is also how the FORWARD descent asks for its plain full-height band — pass K = 0).
    // ⚠ This moves band WAIT lines AND bitplane pointers, so it must only ever be written into
    // the BACK buffer of a double-buffered list (a torn pointer garbages the whole viewport).
    void setRevealBands(uint16_t K, uint32_t ringBase, uint32_t starBase);
    // Tunnel band colours: pen0 = color00 = the band corner (tunnel purple mem[$08D8], carried
    // into the band); pens 1-3 = ring[3..5] ($08D7-$08D9); pens 4-6 = ring[0..2] ($08D4-$08D6)
    // — the GTIA mode-10 pixel→ring +3 rotation the Atari tunnel DLI ($6CD7/$6CF1) applies;
    // pen7 = color07 = the field's exit-clear black ($02C0, value-0 remapped to pen7).
    void setTunnelColors(uint16_t pen0, uint16_t pen1, uint16_t pen2, uint16_t pen3,
                         uint16_t pen4, uint16_t pen5, uint16_t pen6, uint16_t pen7);
};
