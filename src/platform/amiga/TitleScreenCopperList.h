#pragma once
#include "framework/CopperList.h"

class Bitmap;
class Sprite;

// TitleScreenCopperList — the fixed copper list for the Title Screen (scene "Title Screen",
// formerly "Scoreboard 3b"): the attract/level-select/results card showing the big
// "RESCUE ON FRACTALUS!" title (ANTIC mode 7) plus the copyright / STARTING LEVEL /
// RANKING LEVEL / LAST SCORE / HIGH SCORE lines (ANTIC mode 6), on a black background.
//
// On the Atari the text is a charset ($0400) screen at $365B; each character's top 2 bits
// select one of COLPF0-3 as its colour (the rest is COLBK=black).  We pre-decode the whole
// text into a single 3-bitplane bitmap where pen = colour-select + 1 (COLPF0->pen1 ..
// COLPF3->pen4), then this list shows it full-screen with color00=COLBK and color01-04 =
// COLPF0-3.  Those four pens are poked per frame (updateTitleScreenCopper) so the Atari's
// periodic palette cycle is reproduced.  Pure colour-poke, single-buffered (geometry fixed).
class TitleScreenCopperList : public CopperList {
public:
    TitleScreenCopperList();

    // One-time layout: 3bp full-screen playfield, the text bitmap pointers, black COLBK +
    // seeded text pens, null sprites.  setTextPalette() refreshes the cycling pens.
    void buildLayout(const Bitmap& text, const Sprite& nullSprite);

    // Per-frame: color01-04 = COLPF0-3 (the cycling text palette).  color00 (COLBK) is
    // black and constant, so it is not exposed here.
    void setTextPalette(uint16_t pf0, uint16_t pf1, uint16_t pf2, uint16_t pf3);
};
