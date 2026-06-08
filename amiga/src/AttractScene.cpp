// M3: terrain viewport as a 2-bitplane region.
// The copper switches bitplane modes mid-frame via WAIT+MOVE instructions:
//   rows  0-41 : 4-bitplane from attract_raw (title + sky area)
//   rows 42-127: 2-bitplane from terrain_raw (fractal terrain viewport)
//   rows128-199: 4-bitplane from attract_raw cockpit section
// Per-frame animation: palette fade-in + sky hue sweep (from M2).

#define ECS_SPECIFIC
#include <hardware/dmabits.h>
#include <hardware/custom.h>    // USE_BPLCON3 = 1
#include <graphics/display.h>   // PLNCNTSHFT = 12
#include "../framework/AmigaHardware.h"
#include "../framework/CopperList.h"
#include "../framework/Bitmap.h"
#include "../framework/Palette.h"
#include "AttractScene.h"

#include "../assets/attract_pal.h"   // kAttractPalette[16]
#include "../assets/terrain_pal.h"   // kTerrainPalette[4]

extern "C" uint8_t attract_raw[];
extern "C" uint8_t terrain_raw[];

// Bitmap geometry
static const uint16_t kW    = 320;
static const uint16_t kH    = 200;
static const uint16_t kHT   = 86;   // terrain height in rows (rows 42..127)
static const uint8_t  kBP4  = 4;
static const uint8_t  kBP2  = 2;

// Copper list size: full list + terrain switch + cockpit restore + margin
static const uint32_t kCopperLen = 128;

// PAL beam lines for region boundaries.
// Display: centerY=168 → starts at beam line 168-100=68.
// terrain start row 42 → beam 68+42=110
// cockpit start row 128 → beam 68+128=196
static const uint8_t kTerrainLine = 110;
static const uint8_t kCockpitLine = 196;

// BPLCON0 values (PLNCNTSHFT=12, USE_BPLCON3=1)
static const uint16_t kBPLCON0_4P = (uint16_t)((4 << PLNCNTSHFT) | USE_BPLCON3);
static const uint16_t kBPLCON0_2P = (uint16_t)((2 << PLNCNTSHFT) | USE_BPLCON3);

// Animation (from M2): fade-in over 64 frames then sky hue sweep
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

// ---- copper list builder -----------------------------------------------------
void AttractScene::buildCopperList(CopperList* cl, uint16_t frame)
{
    animatePalette(palette, frame);

    uint32_t* d = cl->data();
    uint32_t  idx = 1;  // slot 0 = ctor's initial copperWait(16,0)

    // ---- title+sky region: 4-bitplane full attract bitmap ------------------
    idx = cl->setPlayfield(idx, kW, kH, kBP4, /*interleaved*/true);
    cl->showBitmap(idx, *bitmap);   // sets BPL1PT..4PT + BPL1MOD/2MOD for 4-plane
    idx += 2 * kBP4;                // showBitmap wrote 2 words × 4 planes = 8
    idx = cl->setPalette(idx, *palette);   // 16 × COLOR moves

    // ---- terrain region: switch to 2-bitplane at kTerrainLine --------------
    d[idx++] = copperWait(kTerrainLine, 0);
    d[idx++] = copperMove(bplcon0, kBPLCON0_2P);

    uint32_t ta = (uint32_t)terrain_raw;
    d[idx++] = copperMove(bpl1pth, (uint16_t)(ta >> 16));
    d[idx++] = copperMove(bpl1ptl, (uint16_t)(ta & 0xFFFF));
    d[idx++] = copperMove(bpl2pth, (uint16_t)((ta + 40) >> 16));
    d[idx++] = copperMove(bpl2ptl, (uint16_t)((ta + 40) & 0xFFFF));
    // bpl1mod/bpl2mod for 2-plane interleaved: (2-1)*40 = 40
    d[idx++] = copperMove(bpl1mod, (uint16_t)((kBP2 - 1) * (kW >> 3)));
    d[idx++] = copperMove(bpl2mod, (uint16_t)((kBP2 - 1) * (kW >> 3)));
    // Terrain palette: 4 entries (only need COLOR00..02; COLOR03 unused)
    d[idx++] = copperMove(color00, kTerrainPalette[0]);
    d[idx++] = copperMove(color01, kTerrainPalette[1]);
    d[idx++] = copperMove(color02, kTerrainPalette[2]);
    d[idx++] = copperMove(color03, kTerrainPalette[3]);

    // ---- cockpit region: restore 4-bitplane from attract_raw row 128 -------
    d[idx++] = copperWait(kCockpitLine, 0);
    d[idx++] = copperMove(bplcon0, kBPLCON0_4P);

    // Cockpit starts at row 128 in the 4-plane bitmap: offset = 128 * (4*40)
    uint32_t ca = (uint32_t)attract_raw + (uint32_t)128 * (kBP4 * (kW >> 3));
    d[idx++] = copperMove(bpl1pth, (uint16_t)(ca >> 16));
    d[idx++] = copperMove(bpl1ptl, (uint16_t)(ca & 0xFFFF));
    d[idx++] = copperMove(bpl2pth, (uint16_t)((ca + 40) >> 16));
    d[idx++] = copperMove(bpl2ptl, (uint16_t)((ca + 40) & 0xFFFF));
    d[idx++] = copperMove(bpl3pth, (uint16_t)((ca + 80) >> 16));
    d[idx++] = copperMove(bpl3ptl, (uint16_t)((ca + 80) & 0xFFFF));
    d[idx++] = copperMove(bpl4pth, (uint16_t)((ca + 120) >> 16));
    d[idx++] = copperMove(bpl4ptl, (uint16_t)((ca + 120) & 0xFFFF));
    // bpl1mod/bpl2mod for 4-plane interleaved: (4-1)*40 = 120
    d[idx++] = copperMove(bpl1mod, (uint16_t)((kBP4 - 1) * (kW >> 3)));
    d[idx++] = copperMove(bpl2mod, (uint16_t)((kBP4 - 1) * (kW >> 3)));
    // Restore full palette
    cl->setPalette(idx, *palette);
    // (end-of-list copperWait is at d[kCopperLen-1], set by CopperList ctor)
}

// ---- public interface --------------------------------------------------------
void AttractScene::initialize()
{
    Palette::initialize();
    palette       = new Palette(kAttractPalette, 16, /*fade*/0);
    bitmap        = new Bitmap(attract_raw, kW, kH, kBP4, /*interleaved*/true);
    terrainBitmap = new Bitmap(terrain_raw, kW, kHT, kBP2, /*interleaved*/true);

    for (int i = 0; i < 2; i++) {
        copperLists[i] = CopperList::allocate(kCopperLen);
    }

    if (!palette || !bitmap || !terrainBitmap || !copperLists[0] || !copperLists[1]) return;

    buildCopperList(copperLists[0], 0);
    buildCopperList(copperLists[1], 0);
    active = 0;
    AmigaHardware::setCopperList(*copperLists[active], true);
}

void AttractScene::update(uint16_t frame)
{
    uint8_t next = 1 - active;
    buildCopperList(copperLists[next], frame);
    AmigaHardware::setCopperList(*copperLists[next], false);
    active = next;
}

void AttractScene::render()
{
}

void AttractScene::shutdown()
{
    for (int i = 0; i < 2; i++) { delete copperLists[i]; copperLists[i] = nullptr; }
    delete bitmap;        bitmap        = nullptr;
    delete terrainBitmap; terrainBitmap = nullptr;
    delete palette;       palette       = nullptr;
}
