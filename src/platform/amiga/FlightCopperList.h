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
                     const Sprite& ahLeft, const Sprite& ahRight,   // AH ground-fill (ch0/1 reuse)
                     const Sprite& scopeP3);                      // Targeting-Scope P3 object (ch3 reuse)

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
    void setBandPalette(uint16_t grey);   // wing-band: color04 = frame grey (plane3); color00-03 inherit from terrain
    // Cockpit dashboard palette (color00-07) + dashboard background (color00 on the blue band).
    // Baked constant at fade 16 in buildLayout; poked live only during the death cinematic so the
    // whole cockpit tints gray→salmon with the frame (the Atari cockpit DLI's $00CF-$00D6 source).
    void setCockpitPalette(uint16_t c0, uint16_t c1, uint16_t c2, uint16_t c3,
                           uint16_t c4, uint16_t c5, uint16_t c6, uint16_t c7);
    void setDashBg(uint16_t c);           // dashboard blue band color00 (INDEX_DASH_BLUE)
    // Crosshair (#10) plane3 palette (color04-07).  Visible = all four = the reticle salmon ($26);
    // hidden = the four terrain pens (= color00-03) so the plane3 "+" reads identical to the terrain
    // beneath it.  Gated on the Atari missile HPOS (mem[$2840]==0 ⇒ reticle pushed off-screen).
    void setCrosshairPalette(uint16_t c04, uint16_t c05, uint16_t c06, uint16_t c07);
    void setHudSprite(int slot, const Sprite& s);                  // slot 0..7 sprite pointer
    void setShotColor(uint16_t c);                                 // COLOR27 (player laser shot, ch4 pen11)
    void setScopeP3Color(uint16_t c);                            // COLOR23 (Targeting-Scope P3 object, ch3 pen11, from $00D9)
    void setViewportP3Color(uint16_t c);                         // COLOR31 (Main-Window P3 object, ch7 pen11, from $00D9)
    // Dashboard sprite re-point (the region-boundary SPRxPT rewrite that reuses a viewport channel
    // for a dashboard element — AH ch0/1, scope ch3, altimeter-ship ch7).  Data-driven: the channel
    // is looked up in the kDashRepoints table (FlightCopperList.cpp) which owns the copper index +
    // boundary line.  See docs/sprite-multiplex-plan.md §3 for the full channel×region map, and the
    // arming rule (the channel's viewport sprite must run to VSTOP 180 or the re-point never arms).
    void setDashboardSprite(int ch, const Sprite& s);
    void setTerrainBitplanes(const Bitmap& terrain);               // re-point viewport bpl ptrs (double-buffer flip)
};
