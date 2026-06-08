// Attract screen — always 2 bitplanes, Copper colour splits at region boundaries.
//
// Copper list structure:
//   [preamble]  setPlayfield (2bp, 320x216)
//               showBitmap(titleBitmap)  + title palette (4 colours)
//               sprite colours (COLOR16/17) + sprite pointers (8 sprites)
//   WAIT(kTerrainLine-1, 0xE0)   ← end of previous line, in overscan
//               bpl1/2 ptr → terrainBitmap, terrain palette (4 colours)
//   WAIT(kCockpitLine-1, 0xE0)
//               bpl1/2 ptr → cockpit_raw, cockpit palette (4 colours + blink)
//
// Colour-register writes are IMMEDIATE on OCS.  The correct split technique is
// a single WAIT at the end of the line before the boundary (in the H-blank /
// overscan area).  Bitplane pointers go first (timing-critical: a missed
// pointer corrupts the whole display); colours follow (a late colour is just a
// cosmetic edge artefact at worst).

#define ECS_SPECIFIC
#include <hardware/dmabits.h>
#include <hardware/custom.h>
#include <graphics/display.h>
#include "../framework/AmigaHardware.h"
#include "../framework/CopperList.h"
#include "../framework/Bitmap.h"
#include "../framework/Palette.h"
#include "../framework/Sprite.h"
#include "StandbyScene.h"
#include "PaulaAudio.h"

// station_audio: 6502-transpiled, kept as-is (complex + already working).
extern "C" void station_audio(void);
// Native 68000 replacements for the other attract functions (station_native.cpp).
extern "C" void station_setup(void);    // one-time init (mirrors station_init setup)
extern "C" void station_anim_frame_native(void);
extern "C" void station_sub_1EB4_native(void);
extern "C" void station_sub_1F48_native(void);
// pmg_update_station and pmg_colors_station dropped: only modify PMG/GTIA
// registers not used on the Amiga.
extern "C" void station_audio_ch1_init(void);
extern "C" volatile uint8_t mem[65536];

#include "../assets/title_pal.h"
#include "../assets/terrain_pal.h"
#include "../assets/cockpit_pal.h"

extern "C" uint8_t cockpit_raw[];

static const uint16_t kW   = 320;
static const uint16_t kH   = 216;   // Atari attract = 216 visible scanlines
static const uint16_t kHT  = 86;    // terrain sprite/bitmap height (placeholder; M6a audit may revise)
static const uint8_t  kBP2 = 2;

static const uint32_t kCopperLen = 128;

// Display geometry: anchored at the standard PAL display-window top (0x2c).
// All boundary lines are derived so changing the top can't desync them.
static const uint16_t kDisplayTop    = 0x2c;               // DIWSTRT.y (PAL standard)
static const uint16_t kTitleHeight   = 42;                 // title region display lines
static const uint16_t kTerrainHeight = kHT;                // terrain region (placeholder, see kHT)
static const uint16_t kTerrainLine   = kDisplayTop + kTitleHeight;   // = 0x56 (86)
static const uint16_t kCockpitLine   = kTerrainLine + kTerrainHeight; // = 172
// centerY so that DIWSTRT.y = kDisplayTop: centerY = kDisplayTop + kH/2 = 0x2c + 108 = 0x98
static const uint16_t kCenterY       = kDisplayTop + kH / 2;

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
void StandbyScene::fillSpriteData(Sprite* s, bool isRight)
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
void StandbyScene::buildCopperList(CopperList* cl, uint16_t frame)
{
    animatePalette(palette, frame);
    uint16_t f = palette->fade();

    uint32_t* d   = cl->data();

    // d[0] is already copperWait(16,0) from the CopperList constructor; start at 1.
    uint32_t  idx = 1;

    // ---- title region -------------------------------------------------------
    idx = cl->setPlayfield(idx, kW, kH, kBP2, /*interleaved*/true,
                           /*hires*/false, /*interlace*/false,
                           /*dualPlayfield*/false, /*holdAndModify*/false,
                           kCenterY);
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
    // Wait until the end of the previous line (in the right border / H-blank).
    // Colour writes are immediate on OCS, so everything written during H-blank is
    // live for the first pixel of kTerrainLine.  Bitplane pointers go first
    // (timing-critical); colours follow.
    d[idx++] = copperWait(kTerrainLine - 1, 0xE0);
    uint32_t ta = (uint32_t)terrainBitmap->data;
    d[idx++] = copperMove(bpl1pth, (uint16_t)(ta >> 16));
    d[idx++] = copperMove(bpl1ptl, (uint16_t)(ta & 0xFFFF));
    d[idx++] = copperMove(bpl2pth, (uint16_t)((ta + 40) >> 16));
    d[idx++] = copperMove(bpl2ptl, (uint16_t)((ta + 40) & 0xFFFF));
    d[idx++] = copperMove(color00, fadeColor(kTerrainPalette[0], f));
    d[idx++] = copperMove(color01, fadeColor(kTerrainPalette[1], f));
    d[idx++] = copperMove(color02, fadeColor(kTerrainPalette[2], f));
    d[idx++] = copperMove(color03, fadeColor(kTerrainPalette[3], f));

    // ---- cockpit region — same pattern --------------------------------------
    d[idx++] = copperWait(kCockpitLine - 1, 0xE0);
    uint32_t ca = (uint32_t)cockpit_raw;
    d[idx++] = copperMove(bpl1pth, (uint16_t)(ca >> 16));
    d[idx++] = copperMove(bpl1ptl, (uint16_t)(ca & 0xFFFF));
    d[idx++] = copperMove(bpl2pth, (uint16_t)((ca + 40) >> 16));
    d[idx++] = copperMove(bpl2ptl, (uint16_t)((ca + 40) & 0xFFFF));
    d[idx++] = copperMove(color00, fadeColor(kCockpitPalette[0], f));
    d[idx++] = copperMove(color01, fadeColor(kCockpitPalette[1], f));
    d[idx++] = copperMove(color02, fadeColor(kCockpitPalette[2], f));
    uint16_t litColor = ((blinkFrame / 25) & 1) ? 0x000 : fadeColor(kCockpitPalette[3], f);
    d[idx++] = copperMove(color03, litColor);
}

// ---- public interface --------------------------------------------------------
void StandbyScene::initialize()
{
    Palette::initialize();
    palette       = new Palette(kTitlePalette, 4, /*fade*/0);
    titleBitmap   = Bitmap::allocate(kW, kTitleHeight,   kBP2, true);
    terrainBitmap = Bitmap::allocate(kW, kTerrainHeight, kBP2, true);
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

    paula_audio_init();      // loads standby_mem.bin into mem[] (attract-mode snapshot)

    // The snapshot was captured with $0013=4 (attract exit condition).
    // Reset RTCLOK so station_audio starts from the beginning of its sequence.
    mem[0x0013] = 0;
    mem[0x0014] = 0;
    mem[0x0080] = 0;

    station_audio_ch1_init();
}

void StandbyScene::update(uint16_t frame)
{
    blinkFrame++;

    // Attract state machine — same order as the Atari attract loop.
    // Timers (mem[$0080/$0014/$0013]) are incremented by the VBI handler.
    // pmg_update_station / pmg_colors_station dropped (PMG/GTIA only).
    station_anim_frame_native();
    station_audio();
    station_sub_1EB4_native();
    station_sub_1F48_native();

    uint8_t next = 1 - active;
    buildCopperList(copperLists[next], frame);
    AmigaHardware::setCopperList(*copperLists[next], false);
    active = next;
}

// Helper: expand 8 Mode-6 glyph bits to 16 doubled-pixel output bits (each bit → 2 bits).
static uint16_t doubleGlyphBits(uint8_t g)
{
    uint16_t out = 0;
    for (int b = 7; b >= 0; b--) {
        if (g & (1u << b)) out |= (uint16_t)3u << (b * 2);
    }
    return out;
}

void StandbyScene::render()
{
    // ---- terrain / door view ------------------------------------------------
    // The attract DL at $3000 uses 86 Mode F rows starting at $2000, stride 46.
    // ANTIC reads 40 bytes/row from there (6 bytes padding per row are ignored).
    // Layout in interleaved 2bp: 40 bytes plane1, 40 bytes plane2, per row.
    // Plane2 stays zero — all pixels use only colour indices 0 (background) and 1.

    uint8_t* vdest = (uint8_t*)terrainBitmap->data;
    for (int row = 0; row < (int)kTerrainHeight; row++) {
        const uint8_t* src = (const uint8_t*)&mem[0x2000 + row * 46];
        uint8_t* plane1 = vdest;
        uint8_t* plane2 = vdest + 40;
        for (int b = 0; b < 40; b++) {
            plane1[b] = src[b];
            plane2[b] = 0;
        }
        vdest += 80;
    }

    // ---- title region -------------------------------------------------------
    // The DL's title section is Mode 6 (character mode, 20 double-wide chars,
    // 8 scanlines tall) from screen RAM $32B5 using the custom charset at $0200.
    // Clear the bitmap first then render Mode 6 at the right row offset.
    // The DL has 27 blank scanlines + Mode 6 starts at DL-line 27 → in our
    // 42-line title bitmap the Mode 6 text starts at line (27 - 6) = 21.
    // (6-line offset because the Atari display starts 6 lines before ours.)
    static const int kTitleTextRow = 21;
    static const uint16_t kScreenRAM  = 0x32B5;
    static const uint16_t kCharsetBase = 0x0200;

    uint8_t* tbmp = (uint8_t*)titleBitmap->data;
    for (int i = 0; i < (int)kTitleHeight * 80; i++) tbmp[i] = 0;

    for (int col = 0; col < 20; col++) {
        uint8_t charByte = mem[kScreenRAM + col];
        uint8_t charIdx  = charByte & 0x3Fu;           // bits 5-0 = glyph index
        uint16_t glyphBase = kCharsetBase + charIdx * 8u;

        for (int scanline = 0; scanline < 8; scanline++) {
            int destRow = kTitleTextRow + scanline;
            if (destRow >= (int)kTitleHeight) break;

            uint8_t  glyph   = mem[glyphBase + scanline];
            uint16_t doubled = doubleGlyphBits(glyph);

            // Each doubled character occupies 2 bytes at col*2 in plane1.
            uint8_t* rowPtr = tbmp + destRow * 80;
            rowPtr[col * 2]     = (uint8_t)(doubled >> 8);
            rowPtr[col * 2 + 1] = (uint8_t)(doubled & 0xFF);
        }
    }
}

void StandbyScene::shutdown()
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
