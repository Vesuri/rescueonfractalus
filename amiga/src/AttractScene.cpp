// Attract screen — always 2 bitplanes, Copper colour splits at region boundaries.
//
// Copper list structure:
//   [preamble]  setPlayfield (2bp, 320x200)
//               showBitmap(title_raw)   + title palette (4 colours)
//               sprite colours (COLOR16/17) + sprite pointers (8 sprites)
//   WAIT(kTerrainLine)
//               bpl1/2 ptr → terrain_raw + terrain palette (4 colours)
//   WAIT(kCockpitLine)
//               bpl1/2 ptr → cockpit_raw + cockpit palette (4 colours)
//               COLOR03 override for blinking lights

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

extern "C" void audio_attract(void);
extern "C" void audio_ch1_init(void);
extern "C" volatile uint8_t mem[65536];

#include "../assets/title_pal.h"
#include "../assets/terrain_pal.h"
#include "../assets/cockpit_pal.h"

extern "C" uint8_t title_raw[];
extern "C" uint8_t terrain_raw[];
extern "C" uint8_t cockpit_raw[];

static const uint16_t kW   = 320;
static const uint16_t kH   = 200;
static const uint16_t kHT  = 86;
static const uint8_t  kBP2 = 2;

static const uint32_t kCopperLen = 128;

// PAL raster lines for region boundaries (absolute, not relative to DIWSTRT).
static const uint8_t kTerrainLine = 110;
static const uint8_t kCockpitLine = 196;

// BPLCON0: 2 bitplanes, lores.
static const uint16_t kBPLCON0_2P = (uint16_t)((2 << PLNCNTSHFT) | USE_BPLCON3);

// Sprite horizontal positions (see fillSpriteData for slant details).
static const uint16_t kSprXLeft  = 0x81 + 17;
static const uint16_t kSprXRight = 0x81 + 285;

// Canopy post colour: must match attract panel background (kAttractPalette[2] = 0x444).
static const uint16_t kPostColor = 0x444;

// ---- fade helper -------------------------------------------------------------
static uint16_t fadeColor(uint16_t color, uint16_t fade)
{
    uint16_t r = color >> 8;
    uint16_t g = (color >> 4) & 0xFu;
    uint16_t b = color & 0xFu;
    return (uint16_t)(((fade * r >> 4) << 8) | ((fade * g >> 4) << 4) | (fade * b >> 4));
}

// ---- palette animation -------------------------------------------------------
static void animatePalette(Palette* palette, uint16_t frame)
{
    if (frame < 64) {
        palette->setFade((frame * 17) >> 6);
    } else {
        if (palette->fade() < 16) palette->setFade(16);
    }
}

// ---- sprite data (staircase slant, see commit history for derivation) --------
void AttractScene::fillSpriteData(Sprite* s, bool isRight)
{
    uint16_t* d = s->data() + 2;
    for (int i = 0; i < kHT; i++) {
        int shift;
        if (i < 8) {
            shift = isRight ? 12 : 0;
        } else {
            int step = (i - 8) / 14 + 1;
            shift = isRight ? (12 - 2 * step) : (2 * step);
            if (shift < 0) shift = 0;
            if (shift > 12) shift = 12;
        }
        uint16_t sprA = (uint16_t)(0xF000u >> shift);
        d[i * 2]     = sprA;
        d[i * 2 + 1] = 0x0000;
    }
}

// ---- copper list builder -----------------------------------------------------
void AttractScene::buildCopperList(CopperList* cl, uint16_t frame)
{
    animatePalette(palette, frame);
    uint16_t f = palette->fade();

    uint32_t* d   = cl->data();

    // d[0]: safe preamble wait — chip RAM is not guaranteed zero; without this
    // the Copper may execute a garbage instruction before setPlayfield's entries.
    d[0] = copperWait(16, 0);
    uint32_t  idx = 1;

    // ---- title region -------------------------------------------------------
    // Colours FIRST, then bitmap pointers — this order is critical at every
    // region boundary: if pointers are written before colours the DMA starts
    // feeding new-bitmap pixels while the old palette is still active, producing
    // a one-line colour artefact (the "blue stripe" on terrain row 0).
    idx = cl->setPlayfield(idx, kW, kH, kBP2, /*interleaved*/true);
    idx = cl->setPalette(idx, *palette);   // colours before bitmap pointers
    cl->showBitmap(idx, *titleBitmap);
    idx += 2 * kBP2;

    // Sprite colour registers:
    d[idx++] = copperMove(color16, 0x000);
    d[idx++] = copperMove(color17, fadeColor(kPostColor, f));

    // Sprite pointers:
    cl->showSprite(idx, 0, *leftPost);  idx += 2;
    cl->showSprite(idx, 1, *rightPost); idx += 2;
    for (uint16_t s = 2; s <= 7; s++) {
        cl->showSprite(idx, s, *nullSprite); idx += 2;
    }

    // ---- terrain region -------------------------------------------------------
    // Write the terrain palette one line before switching the bitmap pointer.
    // The OCS colour pipeline has a one-scanline latency: colour register writes
    // at line N are not visible until line N+1.  Setting colours at kTerrainLine-1
    // ensures the terrain palette is live when terrain.raw DMA begins at kTerrainLine.
    d[idx++] = copperWait(kTerrainLine - 1, 0);
    d[idx++] = copperMove(color00, fadeColor(kTerrainPalette[0], f));
    d[idx++] = copperMove(color01, fadeColor(kTerrainPalette[1], f));
    d[idx++] = copperMove(color02, fadeColor(kTerrainPalette[2], f));
    d[idx++] = copperMove(color03, fadeColor(kTerrainPalette[3], f));

    d[idx++] = copperWait(kTerrainLine, 0);
    uint32_t ta = (uint32_t)terrain_raw;
    d[idx++] = copperMove(bpl1pth, (uint16_t)(ta >> 16));
    d[idx++] = copperMove(bpl1ptl, (uint16_t)(ta & 0xFFFF));
    d[idx++] = copperMove(bpl2pth, (uint16_t)((ta + 40) >> 16));
    d[idx++] = copperMove(bpl2ptl, (uint16_t)((ta + 40) & 0xFFFF));

    // ---- cockpit region — same one-line-early colour pattern ----------------
    d[idx++] = copperWait(kCockpitLine - 1, 0);
    d[idx++] = copperMove(color00, fadeColor(kCockpitPalette[0], f));
    d[idx++] = copperMove(color01, fadeColor(kCockpitPalette[1], f));
    d[idx++] = copperMove(color02, fadeColor(kCockpitPalette[2], f));

    d[idx++] = copperWait(kCockpitLine, 0);
    uint32_t ca = (uint32_t)cockpit_raw;
    d[idx++] = copperMove(bpl1pth, (uint16_t)(ca >> 16));
    d[idx++] = copperMove(bpl1ptl, (uint16_t)(ca & 0xFFFF));
    d[idx++] = copperMove(bpl2pth, (uint16_t)((ca + 40) >> 16));
    d[idx++] = copperMove(bpl2ptl, (uint16_t)((ca + 40) & 0xFFFF));

    // Blink: lights are palette entry 3 (0x832 orange-red).
    uint16_t litColor = ((blinkFrame / 25) & 1) ? 0x000 : fadeColor(kCockpitPalette[3], f);
    d[idx++] = copperMove(color03, litColor);
}

// ---- public interface --------------------------------------------------------
void AttractScene::initialize()
{
    Palette::initialize();
    palette       = new Palette(kTitlePalette, 4, /*fade*/0);
    titleBitmap   = new Bitmap(title_raw,   kW, 42,  kBP2, true);
    terrainBitmap = new Bitmap(terrain_raw, kW, kHT, kBP2, true);
    cockpitBitmap = new Bitmap(cockpit_raw, kW, 104, kBP2, true);

    leftPost   = Sprite::allocate(kHT);
    rightPost  = Sprite::allocate(kHT);
    nullSprite = Sprite::allocate(0);
    if (!leftPost || !rightPost || !nullSprite) return;

    fillSpriteData(leftPost,  false);
    fillSpriteData(rightPost, true);

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

    paula_audio_init();
    audio_ch1_init();
}

void AttractScene::update(uint16_t frame)
{
    blinkFrame++;

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
    delete titleBitmap;   titleBitmap   = nullptr;
    delete terrainBitmap; terrainBitmap = nullptr;
    delete cockpitBitmap; cockpitBitmap = nullptr;
    delete palette;       palette       = nullptr;
    delete leftPost;      leftPost      = nullptr;
    delete rightPost;     rightPost     = nullptr;
    delete nullSprite;    nullSprite    = nullptr;
}
