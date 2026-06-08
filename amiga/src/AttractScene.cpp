// M5: 6502-converted POKEY audio player → Paula DMA.
// M4: cockpit chrome as hardware sprites.
// Two sprites (0 = left canopy post, 1 = right canopy post) placed at the edges
// of the terrain viewport. Sprites 2-7 silenced with the null sprite.
// Sprite colour: COLOR17 = 0x5A0 (bright green, distinct from terrain green 0x580).
//
// Copper list structure (M4 adds sprite pointers to the M3 list):
//   [0]       copperWait(16,0)
//   [1..12]   setPlayfield (4-plane)
//   [13..20]  showBitmap (4-plane attract_raw)
//   [21..36]  setPalette (16 colour moves)
//   [37..38]  COLOR16/17 (sprite transparency + canopy green)
//   [39..54]  showSprite ×8 (sprites 0+1 = posts; 2-7 = null)
//   [55]      WAIT(kTerrainLine) + terrain mode switch
//   ...       (same terrain/cockpit switch as M3)
//
// Sprite positions (OCS lores, PAL):
//   HSTART is measured in color clocks from raster start.
//   Our display DIWSTRT HSTART = 0x71 = 113 → screen pixel 0 = color clock 113.
//   Left post:  setX(113)       → screen pixel  0, 4-px bar from left
//   Right post: setX(429)       → screen pixel 316, 4-px bar to right edge (429=113+316)
//   Y: setY(kTerrainLine)       → active during terrain viewport only

#define ECS_SPECIFIC
#include <hardware/dmabits.h>
#include <hardware/custom.h>
#include <graphics/display.h>
#include "../framework/AmigaHardware.h"
#include "../framework/CopperList.h"
#include "../framework/Bitmap.h"
#include "../framework/Palette.h"
#include "../framework/Sprite.h"
#include "AttractScene.h"
#include "PaulaAudio.h"

// 6502-converted audio functions (from src/gen/rof_gen.c compiled for m68k)
extern "C" void audio_attract(void);
extern "C" void audio_ch1_init(void);
// mem[] (shared 6502 address space) — defined in audio/cpu.o
extern "C" volatile uint8_t mem[65536];

#include "../assets/attract_pal.h"
#include "../assets/terrain_pal.h"

extern "C" uint8_t attract_raw[];
extern "C" uint8_t terrain_raw[];

static const uint16_t kW    = 320;
static const uint16_t kH    = 200;
static const uint16_t kHT   = 86;
static const uint8_t  kBP4  = 4;
static const uint8_t  kBP2  = 2;

static const uint32_t kCopperLen = 128;

// PAL beam positions (same as M3)
static const uint8_t kTerrainLine = 110;
static const uint8_t kCockpitLine = 196;

// BPLCON0 for OCS lores (PLNCNTSHFT=12, USE_BPLCON3=1)
static const uint16_t kBPLCON0_4P = (uint16_t)((4 << PLNCNTSHFT) | USE_BPLCON3);
static const uint16_t kBPLCON0_2P = (uint16_t)((2 << PLNCNTSHFT) | USE_BPLCON3);

// Sprite horizontal positions in raster lores-pixel units (SPRPOS HSTART, 9-bit).
// Standard OCS PAL lores display: left edge ≈ 0x81 = 129 raster pixels from hsync.
// Right-post starts 4 pixels before right edge: 129 + 316 = 445 (< 511 max). OK.
static const uint16_t kSprXLeft  = 0x81;          // screen pixel 0
static const uint16_t kSprXRight = 0x81 + 316;    // screen pixel 316 → bar to 319

// Canopy post colour (sprite 0+1 shared: COLOR17)
static const uint16_t kPostColor = 0x5A0;  // bright green (Atari $2A analog)

// M2 animation
static const uint16_t kSkyHues[8] = {
    0x46b, 0x56b, 0x56c, 0x46c, 0x36b, 0x46a, 0x56b, 0x56a
};

static void animatePalette(Palette* palette, uint16_t frame)
{
    if (frame < 64) {
        palette->setFade((frame * 17) >> 6);
    } else {
        if (palette->fade() < 16) palette->setFade(16);
        uint16_t step = (uint16_t)((frame - 64) >> 2) & 7;
        uint16_t pal[16];
        for (int i = 0; i < 16; i++) pal[i] = kAttractPalette[i];
        pal[5] = kSkyHues[step];
        palette->setPalette(pal, 16);
        palette->setFade(16);
    }
}

// ---- sprite data helper ------------------------------------------------------
void AttractScene::fillSpriteData(Sprite* s, uint16_t sprA)
{
    uint16_t* d = s->data() + 2;  // skip 2-word position header
    for (int i = 0; i < kHT; i++) {
        d[i * 2]     = sprA;   // plane A: determines which pixels are coloured
        d[i * 2 + 1] = 0x0000; // plane B: 0 → colour 1 (COLOR17) where A is set
    }
}

// ---- copper list builder (M4) ------------------------------------------------
void AttractScene::buildCopperList(CopperList* cl, uint16_t frame)
{
    animatePalette(palette, frame);

    uint32_t* d = cl->data();
    uint32_t  idx = 1;

    // ---- title region: 4-bitplane full attract bitmap -----------------------
    idx = cl->setPlayfield(idx, kW, kH, kBP4, /*interleaved*/true);
    cl->showBitmap(idx, *bitmap);
    idx += 2 * kBP4;
    idx = cl->setPalette(idx, *palette);

    // Sprite colour registers (in effect for the whole frame):
    d[idx++] = copperMove(color16, 0x000);   // sprite transparency
    d[idx++] = copperMove(color17, kPostColor);  // canopy post green

    // Sprite pointers (set before display begins so DMA reads correct data):
    cl->showSprite(idx, 0, *leftPost);    idx += 2;
    cl->showSprite(idx, 1, *rightPost);   idx += 2;
    for (uint16_t s = 2; s <= 7; s++) {
        cl->showSprite(idx, s, *nullSprite); idx += 2;
    }

    // ---- terrain region switch (M3): 2-bitplane, terrain palette -----------
    d[idx++] = copperWait(kTerrainLine, 0);
    d[idx++] = copperMove(bplcon0, kBPLCON0_2P);

    uint32_t ta = (uint32_t)terrain_raw;
    d[idx++] = copperMove(bpl1pth, (uint16_t)(ta >> 16));
    d[idx++] = copperMove(bpl1ptl, (uint16_t)(ta & 0xFFFF));
    d[idx++] = copperMove(bpl2pth, (uint16_t)((ta + 40) >> 16));
    d[idx++] = copperMove(bpl2ptl, (uint16_t)((ta + 40) & 0xFFFF));
    d[idx++] = copperMove(bpl1mod, (uint16_t)((kBP2 - 1) * (kW >> 3)));
    d[idx++] = copperMove(bpl2mod, (uint16_t)((kBP2 - 1) * (kW >> 3)));
    d[idx++] = copperMove(color00, kTerrainPalette[0]);
    d[idx++] = copperMove(color01, kTerrainPalette[1]);
    d[idx++] = copperMove(color02, kTerrainPalette[2]);
    d[idx++] = copperMove(color03, kTerrainPalette[3]);

    // ---- cockpit region: restore 4-bitplane, full palette ------------------
    d[idx++] = copperWait(kCockpitLine, 0);
    d[idx++] = copperMove(bplcon0, kBPLCON0_4P);

    uint32_t ca = (uint32_t)attract_raw + (uint32_t)128 * (kBP4 * (kW >> 3));
    d[idx++] = copperMove(bpl1pth, (uint16_t)(ca >> 16));
    d[idx++] = copperMove(bpl1ptl, (uint16_t)(ca & 0xFFFF));
    d[idx++] = copperMove(bpl2pth, (uint16_t)((ca + 40) >> 16));
    d[idx++] = copperMove(bpl2ptl, (uint16_t)((ca + 40) & 0xFFFF));
    d[idx++] = copperMove(bpl3pth, (uint16_t)((ca + 80) >> 16));
    d[idx++] = copperMove(bpl3ptl, (uint16_t)((ca + 80) & 0xFFFF));
    d[idx++] = copperMove(bpl4pth, (uint16_t)((ca + 120) >> 16));
    d[idx++] = copperMove(bpl4ptl, (uint16_t)((ca + 120) & 0xFFFF));
    d[idx++] = copperMove(bpl1mod, (uint16_t)((kBP4 - 1) * (kW >> 3)));
    d[idx++] = copperMove(bpl2mod, (uint16_t)((kBP4 - 1) * (kW >> 3)));
    cl->setPalette(idx, *palette);
}

// ---- public interface --------------------------------------------------------
void AttractScene::initialize()
{
    Palette::initialize();
    palette       = new Palette(kAttractPalette, 16, /*fade*/0);
    bitmap        = new Bitmap(attract_raw, kW, kH, kBP4, true);
    terrainBitmap = new Bitmap(terrain_raw,  kW, kHT, kBP2, true);

    // Allocate sprites in chip RAM
    leftPost   = Sprite::allocate(kHT);
    rightPost  = Sprite::allocate(kHT);
    nullSprite = Sprite::allocate(0);   // end marker only

    if (!leftPost || !rightPost || !nullSprite) return;

    // 4-pixel wide solid bar: sprA bits 15..12 set (MSB = leftmost pixel)
    fillSpriteData(leftPost,  0xF000);
    fillSpriteData(rightPost, 0xF000);

    // Position: visible only during terrain region (beam 110..196)
    leftPost->setX(kSprXLeft);
    leftPost->setY(kTerrainLine);

    rightPost->setX(kSprXRight);
    rightPost->setY(kTerrainLine);

    for (int i = 0; i < 2; i++) {
        copperLists[i] = CopperList::allocate(kCopperLen);
    }
    if (!copperLists[0] || !copperLists[1]) return;

    buildCopperList(copperLists[0], 0);
    buildCopperList(copperLists[1], 0);
    active = 0;
    AmigaHardware::setCopperList(*copperLists[active], true);

    // M5: initialise Paula audio (loads XEX + sets up DMA) and set AUDF1/AUDF2
    paula_audio_init();
    audio_ch1_init();   // sets AUDF1=0xFF, AUDF2=0xFC (carry-tone frequencies)
}

void AttractScene::update(uint16_t frame)
{
    // M5: advance VBI counter (simulates vbi_handler_attract counter logic)
    // mem[$0014] increments each frame; when it wraps, mem[$0013] increments.
    // audio_attract reads these to drive its volume-envelope state machine.
    mem[0x0014]++;
    if (mem[0x0014] == 0) mem[0x0013]++;
    audio_attract();

    uint8_t next = 1 - active;
    buildCopperList(copperLists[next], frame);
    AmigaHardware::setCopperList(*copperLists[next], false);
    active = next;
}

void AttractScene::render() {}

void AttractScene::shutdown()
{
    for (int i = 0; i < 2; i++) { delete copperLists[i]; copperLists[i] = nullptr; }
    paula_audio_shutdown();
    delete bitmap;        bitmap        = nullptr;
    delete terrainBitmap; terrainBitmap = nullptr;
    delete palette;       palette       = nullptr;
    delete leftPost;      leftPost      = nullptr;
    delete rightPost;     rightPost     = nullptr;
    delete nullSprite;    nullSprite    = nullptr;
}
