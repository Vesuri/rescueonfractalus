#pragma once
#include "framework/CopperList.h"

class Bitmap;
class Sprite;

// FlightCopperList — the fixed-layout copper list for the in-flight gameplay scene
// (scene 7, rsFlight): title/top-bar + a line-doubled mode-D terrain viewport band +
// cockpit.  Structurally identical to PlanetCopperList (same 85-WAIT line-doubling
// band, same title and cockpit regions); it differs only in the viewport palette (the
// terrain pens, incl. the salmon→brown terrain-body fade) and in the sprite set (the
// flight HUD — gauges/horizon/compass/wing-clearance — rather than the starfield).
//
// Built ONCE in buildLayout() (geometry, bitmap pointers, sprite pointers, the constant
// line-doubling band, the constant title/cockpit palettes and the fixed viewport pens),
// then the per-frame-varying colours/sprites are poked in place via the setters — no
// per-frame full rebuild, no double buffer.  The native flight code (the ported $4FF5
// VBI + atmosphere fade) is the source of truth and drives the setters via callbacks,
// exactly as the title-text writer drives the title repaint.  Mirrors PlanetCopperList /
// StandbyCopperList.
class FlightCopperList : public CopperList {
public:
    FlightCopperList();

    // One-time layout: emit the whole fixed list.  HUD sprite pointers are poked later
    // (setHudSprite) as the flight VBI port maps the Atari players/missiles to sprites;
    // buildLayout seeds the canopy posts + throttle gauge and nulls the rest.
    void buildLayout(const Bitmap& title, const Bitmap& terrain, const Bitmap& cockpit,
                     const Sprite& leftPost, const Sprite& leftTri,
                     const Sprite& rightPost, const Sprite& rightTri,
                     const Sprite& nullSprite,
                     const Sprite& ahLeft, const Sprite& ahRight);  // AH ground-fill (ch0/1 reuse)

    // ---- per-frame setters (each pokes one MOVE, or one sprite pointer pair) ----
    void setTitlePalette(uint16_t bg, uint16_t pf0, uint16_t pf1);  // color00..03 (col0=col3=bg)
    void setSpritePostColor(uint16_t c);                           // color17 + COLOR21 (A-pillars, pen01)
    void setTriangleColor(uint16_t c);                             // COLOR18 + COLOR22 (band triangles, pen10)
    void setEnergyIndicatorColor(uint16_t c);                                // COLOR21 ($1AA) energy-indicator bar
    void setAltimeterColor(uint16_t c);                            // COLOR25 ($1B2) altimeter terrain-height bar (P0)
    void setAltimeterShipColor(uint16_t c);                        // COLOR29 ($1BA) altimeter ship-height bar (M3)
    void setCompassColor(uint16_t c);                              // color01 over the compass band (COLPF0 $00CF, housing)
    void setCompassNeedleColor(uint16_t c);                        // color03 over the compass band (COLPF2 salmon, needle/letters)
    // Viewport (terrain) pens.  pen0 = terrain body (the salmon→brown fade target),
    // pen1 = sky, pen2 = dots, pen3 = highlight — driven per-frame by the native code.
    void setTerrainPalette(uint16_t pen0, uint16_t pen1, uint16_t pen2, uint16_t pen3);
    void setTerrainPen0(uint16_t pen0);                            // the fade-only fast path
    void setBandPalette(uint16_t pen3);   // wing-band: only COLPF2 (frame grey); color00/01/02 inherit from terrain
    void setHudSprite(int slot, const Sprite& s);                  // slot 0..7 sprite pointer
    void setTerrainBitplanes(const Bitmap& terrain);               // re-point viewport bpl ptrs (double-buffer flip)
};
