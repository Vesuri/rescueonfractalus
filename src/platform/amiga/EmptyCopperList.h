#pragma once
#include "framework/CopperList.h"

class Sprite;

// EmptyCopperList — a uniform single-colour display: ONE bitplane whose two pens (color00 +
// color01) are set to the same colour, so the whole display window reads that one colour
// regardless of the (irrelevant) bitplane content, while BPLCON3 BRDNBLNK blanks the border.
// (0 bitplanes + BRDNBLNK blanks the ENTIRE raster to black — so we keep one active bitplane,
// re-reading a tiny throwaway line via bpl1mod = -linebytes.)  Shown black while the boot/standby
// build is still in progress (before g_standbyRevealReady), and reused for the death-cinematic
// salmon-hold → black fade (setColor00 poked from mem[$00D4]).  renderFrame switches to the real
// lists once ready.
class EmptyCopperList : public CopperList {
public:
    EmptyCopperList();
    void buildLayout(const Sprite& nullSprite);   // one-time: 1 plane (both pens equal), BRDNBLNK, null sprites
    // Poke the full-screen colour — sets BOTH pens (color00 AND color01) so the single-bitplane
    // display window reads one uniform colour.  Drives the death-cinematic salmon-hold → black
    // fade (colour ← mem[$00D4], mirroring the Atari's DMA-off COLBK fade).  Colour-only poke =
    // safe on the live list.  See death-cinematic memory + RescueOnFractalus::renderFrame.
    void setColor00(uint16_t c);
private:
    uint16_t* filler_ = 0;   // tiny chip buffer the dummy bitplane re-reads each scanline
};
