#pragma once
#include "framework/CopperList.h"

class Sprite;

// EmptyCopperList — a black, blank display: 0 bitplanes (BPLCON0 = 0, so no playfield
// fetch), COLOR00 = black, and all 8 sprites pointed at a null sprite.  Shown while the
// boot/standby build is still in progress (before g_standbyRevealReady latches), so the
// piecemeal bitmap build is never visible — replacing the old palette-fade-to-black
// reveal.  renderFrame switches to the real lists once the standby screen is ready.
class EmptyCopperList : public CopperList {
public:
    EmptyCopperList();
    void buildLayout(const Sprite& nullSprite);   // one-time: COLOR00=0, BPLCON0=0, null sprites
    // Poke the full-screen background colour (COLOR00).  With 0 bitplanes the whole display
    // is COLOR00, so this drives the death-cinematic salmon-hold → black fade (color00 ←
    // mem[$00D4], mirroring the Atari's DMA-off COLBK fade).  Colour-only poke = safe on the
    // live list.  See death-cinematic memory + RescueOnFractalus::renderFrame.
    void setColor00(uint16_t c);
};
