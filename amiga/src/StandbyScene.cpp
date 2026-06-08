// Standby screen (scene 3) — 2 bitplanes, Copper colour splits at region boundaries.
//
// Terrain colours are read dynamically each frame from mem[]:
//   - Atari GTIA mode-10 nibble 8 (dominant closed-door fill) → colHW[8] = COLBK
//   - DLI dli_sub_6cf1 sets COLBK = mem[$0071] during terrain rows.
//   → Amiga terrain col3 = atariToOCS(mem[$0071]).
//
// Copper list structure:
//   [preamble]  setPlayfield (2bp, 320x216)
//               showBitmap(titleBitmap)  + title palette (4 colours)
//               sprite colours (COLOR16/17) + sprite pointers (8 sprites)
//   WAIT(kTerrainLine-1, 0xE0)   ← end of previous line, in overscan
//               bpl1/2 ptr → terrainBitmap, terrain palette (dynamic from mem[])
//   WAIT(kCockpitLine-1, 0xE0)
//               bpl1/2 ptr → cockpitBitmap (modeD $350D + mode4 $332D from mem[])
//
// Colour-register writes are IMMEDIATE on OCS.  The correct split technique is
// a single WAIT at the end of the line before the boundary (in the H-blank /
// overscan area).  Bitplane pointers go first (timing-critical); colours follow.

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

// sfx_voice_tick_native: native 68000 replacement for the transpiled sfx_voice_tick.
// See SfxPlayer.cpp for the implementation.
extern "C" void sfx_voice_tick_native(void);

extern "C" volatile uint8_t mem[65536];

#include "../assets/title_pal.h"
#include "../assets/terrain_pal.h"
#include "../assets/atari_pal.h"
// cockpit_pal.h and cockpit_raw removed: cockpit palette is now fully dynamic
// from mem[] via atariToOCS(), cockpit bitmap decoded each frame in render().

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
// Cockpit height: 4 modeD DL entries × 2 scan lines + 9 mode4 DL entries × 8 scans
static const uint16_t kCockpitH     = 4 * 2 + 9 * 8;                // = 80
// centerY so that DIWSTRT.y = kDisplayTop: centerY = kDisplayTop + kH/2 = 0x2c + 108 = 0x98
static const uint16_t kCenterY       = kDisplayTop + kH / 2;

// BPLCON0: 2 bitplanes, lores.
static const uint16_t kBPLCON0_2P = (uint16_t)((2 << PLNCNTSHFT) | USE_BPLCON3);

// Sprite horizontal positions (see fillSpriteData for slant details).
static const uint16_t kSprXLeft  = 0x81 + 17;
static const uint16_t kSprXRight = 0x81 + 285;

// Canopy post colour: must match attract panel background (kAttractPalette[2] = 0x444).
static const uint16_t kPostColor = 0x444;

// ---- OCS colour helpers ------------------------------------------------------
static uint16_t blendOCS(uint16_t a, uint16_t b)
{
    uint16_t r  = (((a >> 8) & 0xFu) + ((b >> 8) & 0xFu)) >> 1;
    uint16_t g  = (((a >> 4) & 0xFu) + ((b >> 4) & 0xFu)) >> 1;
    uint16_t bv = ((a & 0xFu) + (b & 0xFu)) >> 1;
    return (uint16_t)((r << 8) | (g << 4) | bv);
}

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
    // Title palette: dli_sub_4a0c fires at start of mode-6 row and sets colours
    // from mem[$00Dx] shadow RAM.  COLBK=mem[$00D8]=bg, COLPF1=mem[$00D5]=fg.
    // col2 = NTSC blend (avg of bg+fg), col3 = COLPF2=mem[$00D6] (colour-select).
    {
        uint16_t tbg   = atariToOCS(mem[0x00D8]);
        uint16_t tfg   = atariToOCS(mem[0x00D5]);
        uint16_t tbl   = blendOCS(tbg, tfg);
        uint16_t tsel  = atariToOCS(mem[0x00D6]);
        d[idx++] = copperMove(color00, fadeColor(tbg,  f));
        d[idx++] = copperMove(color01, fadeColor(tfg,  f));
        d[idx++] = copperMove(color02, fadeColor(tbl,  f));
        d[idx++] = copperMove(color03, fadeColor(tsel, f));
    }
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
    // Terrain palette derived from Atari colour registers (via kNibbleColour zones):
    //   col0 = nibble 0 → colHW[0] = COLPM0 = mem[$02C0]
    //   col1 = nibbles 1-2 → COLPM1/2 zone = mem[$08D7] (DLI override)
    //   col2 = nibbles 3-6 → COLPF0-3 zone = mem[$08D4] (DLI override)
    //   col3 = nibbles 5-8 → COLBK zone    = mem[$0071] (DLI dli_sub_6cf1)
    d[idx++] = copperMove(color00, fadeColor(atariToOCS(mem[0x02C0]), f));
    d[idx++] = copperMove(color01, fadeColor(atariToOCS(mem[0x08D7]), f));
    d[idx++] = copperMove(color02, fadeColor(atariToOCS(mem[0x08D4]), f));
    d[idx++] = copperMove(color03, fadeColor(atariToOCS(mem[0x0071]), f));

    // ---- cockpit region — same pattern --------------------------------------
    d[idx++] = copperWait(kCockpitLine - 1, 0xE0);
    uint32_t ca = (uint32_t)cockpitBitmap->data;
    d[idx++] = copperMove(bpl1pth, (uint16_t)(ca >> 16));
    d[idx++] = copperMove(bpl1ptl, (uint16_t)(ca & 0xFFFF));
    d[idx++] = copperMove(bpl2pth, (uint16_t)((ca + 40) >> 16));
    d[idx++] = copperMove(bpl2ptl, (uint16_t)((ca + 40) & 0xFFFF));
    // Cockpit palette from Atari shadow RAM:
    //   col0 = COLBK (panel background) = mem[$02C8]
    //   col1 = COLPF1 (panel body)      = mem[$02C5]
    //   col2 = COLPF2 (mid grey)        = mem[$02C6]
    //   col3 = cockpit light (blinks)   = mem[$00DE] drives COLPM3 on Atari
    d[idx++] = copperMove(color00, fadeColor(atariToOCS(mem[0x02C8]), f));
    d[idx++] = copperMove(color01, fadeColor(atariToOCS(mem[0x02C5]), f));
    d[idx++] = copperMove(color02, fadeColor(atariToOCS(mem[0x02C6]), f));
    uint16_t litColor = (mem[0x00DE] >= 0x4E) ? fadeColor(atariToOCS(mem[0x00D8]), f) : 0x000;
    d[idx++] = copperMove(color03, litColor);
}

// ---- public interface --------------------------------------------------------
void StandbyScene::initialize()
{
    Palette::initialize();
    palette       = new Palette(kTitlePalette, 4, /*fade*/0);
    titleBitmap   = Bitmap::allocate(kW, kTitleHeight,   kBP2, true);
    terrainBitmap = Bitmap::allocate(kW, kTerrainHeight, kBP2, true);
    cockpitBitmap = Bitmap::allocate(kW, kCockpitH, kBP2, true);

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

    paula_audio_init();      // loads screen3_mem.bin into mem[] (Standby scene snapshot)

    // R4 parity: patch key attract-mode values that differ from the SDL oracle.
    // (Must happen before initial render() call below.)
    // screen3_mem.bin was captured mid-animation; game_entry sets these on startup.
    // mem[$0071]: COLBK source for terrain rows (DLI dli_sub_6cf1 reads it).
    //   Snapshot has $DB (mid-animation); attract init targets $C8 (green, $C8=hue12/luma4).
    //   SDL oracle (atari000.png) shows terrain as (82,140,22) = $C8.
    mem[0x0071] = 0xC8;

    // Initial render: populate all three bitmaps from mem[] once so that
    // render() called from the main loop has nothing to do until data changes.
    render();
}

void StandbyScene::update(uint16_t frame)
{
    // ---- Music: SFX sequencer tick (R3 — native) --------------------------------
    // Equivalent to Atari VTIMR2 IRQ at $54C0. Calls sfx_voice_tick_native from
    // SfxPlayer.cpp — replaces transpiled 6502 version (~60x faster on 68000).
    if (mem[0x00E7] != 0) sfx_voice_tick_native();

    // ---- VBI animation (mimics vbi_handler_game + update_blink_timer_006e) ---
    // NOTE: main.cpp's VBI interrupt server already increments mem[$0014] and
    // mem[$0013]/$0080 each VBI — do NOT increment $0014 here again.
    mem[0x062D]++;
    if (mem[0x062D] == 0) mem[0x00E2]++;

    // Blink timer (update_blink_timer_006e @ $4131):
    //   mem[$006E] counts down from $0F; at 0 it reloads and turns lights ON ($4E);
    //   when below $0A, lights go OFF ($46).  mem[$00DE] drives cockpit light colour.
    if (mem[0x006E] == 0) {
        mem[0x006E] = 0x0F;
        mem[0x00DE] = 0x4E;   // lights ON
    } else {
        mem[0x006E]--;
        if (mem[0x006E] < 0x0A)
            mem[0x00DE] = 0x46;   // lights OFF
    }

    uint8_t next = 1 - active;
    buildCopperList(copperLists[next], frame);
    AmigaHardware::setCopperList(*copperLists[next], false);
    active = next;
}

// ---- cockpit helpers ---------------------------------------------------------
// Decode one 2bpp byte (modeD raw or mode4 glyph) → Amiga 2bp byte pair.
// Each byte contains 4 × 2-bit Atari pixels; each pixel expands to 2 Amiga pixels.
// Amiga colour index = {p2_bit, p1_bit} — same layout as terrain kNibbleColour.
static void decode2bppByte(uint8_t src, uint8_t* p1out, uint8_t* p2out)
{
    uint8_t p1 = 0, p2 = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t pixel = (src >> (6 - i*2)) & 3u;
        uint8_t mask  = (uint8_t)(0xC0u >> (i*2));  // 0xC0, 0x30, 0x0C, 0x03
        if (pixel & 1u) p1 |= mask;   // plane1 = bit 0 of colour index
        if (pixel & 2u) p2 |= mask;   // plane2 = bit 1 of colour index
    }
    *p1out = p1;
    *p2out = p2;
}

// NTSC-blended mode-6 glyph render (2bp, 3-colour output).
// On real Atari NTSC, adjacent differing bits ($55/$AA patterns) colour-blend
// into a third hue via carrier phase cancellation.  We approximate this by
// assigning adjacent same-bits → col0/col1, adjacent differing-bits → col2.
//
// Layout: glyph byte = 8 bits → 4 adjacent pairs (b7b6, b5b4, b3b2, b1b0).
// Each pair → 4 Amiga pixels (doubled: 2px per bit).
// In interleaved 2bp row (40 bytes plane1 + 40 bytes plane2):
//   pair 0 → high nibble of plane bytes at col*2
//   pair 1 → low  nibble of plane bytes at col*2
//   pair 2 → high nibble of plane bytes at col*2+1
//   pair 3 → low  nibble of plane bytes at col*2+1
// Amiga 2bp: col0 = p1=0,p2=0 | col1 = p1=1,p2=0 | col2 = p1=0,p2=1
static void renderNTSCGlyph(uint8_t g, uint8_t* p1, uint8_t* p2)
{
    p1[0] = p1[1] = p2[0] = p2[1] = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t bit_hi = (g >> (7 - i*2)) & 1u;
        uint8_t bit_lo = (g >> (6 - i*2)) & 1u;
        uint8_t mask   = (i & 1) ? 0x0Fu : 0xF0u;   // even pairs → high nibble
        uint8_t* b1    = (i < 2) ? &p1[0] : &p1[1];
        uint8_t* b2    = (i < 2) ? &p2[0] : &p2[1];
        if (bit_hi == bit_lo) {
            if (bit_hi) *b1 |= mask;   // col1: plane1 set
        } else {
            *b2 |= mask;               // col2: plane2 set (NTSC blend)
        }
    }
}

// GTIA mode-10 nibble → Amiga 2bp colour index.
// Zones: 0→bg, 1-2→COLPF0-1 (col1), 3-4→COLPF2-3 (col2), 5-8+→COLPM0-3 (col3).
// Each Atari byte = 2 GTIA pixels (4 Amiga pixels each); 40 bytes → 80 GTIA px → 320 Amiga px.
// In the 2bp interleaved layout (40 bytes plane1, 40 bytes plane2 per row):
//   byte b: high nibble covers Amiga pixels 8b..8b+3 → plane bits [7:4] of byte b
//            low nibble covers Amiga pixels 8b+4..8b+7 → plane bits [3:0] of byte b
static const uint8_t kNibbleColour[16] = {
    0,           // 0  → col0 (COLBK / sky)
    1, 1,        // 1-2 → col1 (COLPF0-1)
    2, 2,        // 3-4 → col2 (COLPF2-3)
    3, 3, 3, 3,  // 5-8 → col3 (COLPM0-3)
    3, 3, 3, 3, 3, 3, 3  // 9-15 → col3
};

void StandbyScene::render()
{
    // ---- terrain / door view ------------------------------------------------
    // Only re-render when terrainDirty (set in initialize(); cleared here).
    // During static Standby the terrain is constant ($88 = closed door).
    // DL $3000: 86 Mode-F rows from $2000, stride 46 (40 data + 6 pad), GTIA mode 10.
    // Each byte = two 4-bit nibbles → two GTIA pixels, each 4 Amiga pixels wide.
    // Interleaved 2bp row = 40 bytes plane1 + 40 bytes plane2.

    if (terrainDirty) {
        terrainDirty = false;
        uint8_t* vdest = (uint8_t*)terrainBitmap->data;
        for (int row = 0; row < (int)kTerrainHeight; row++) {
            const uint8_t* src = (const uint8_t*)&mem[0x2000 + row * 46];
            uint8_t* plane1 = vdest;
            uint8_t* plane2 = vdest + 40;
            for (int b = 0; b < 40; b++) {
                uint8_t hi = kNibbleColour[(src[b] >> 4) & 0xF];
                uint8_t lo = kNibbleColour[src[b] & 0xF];
                plane1[b] = (uint8_t)(((hi & 1) ? 0xF0u : 0u) | ((lo & 1) ? 0x0Fu : 0u));
                plane2[b] = (uint8_t)(((hi & 2) ? 0xF0u : 0u) | ((lo & 2) ? 0x0Fu : 0u));
            }
            vdest += 80;
        }
    }

    // ---- title region -------------------------------------------------------
    // Shadow-compare: re-render only chars whose byte changed since last frame.
    // titleShadow[] mirrors $32B7-$32CA; updated here on change.
    // Chars start at $32B7 (skip $32B5/$32B6 left-border), charset $3800 (NTSC).
    static const int      kTitleTextRow  = 21;
    static const uint16_t kScreenRAM    = 0x32B7;
    static const uint16_t kCharsetBase  = 0x3800;

    uint8_t* tbmp = (uint8_t*)titleBitmap->data;
    for (int col = 0; col < 20; col++) {
        uint8_t charByte = mem[kScreenRAM + (uint16_t)col];
        if (charByte == titleShadow[col]) continue;   // unchanged — skip
        titleShadow[col] = charByte;

        // Clear this character's 8 scanlines in both planes
        for (int scanline = 0; scanline < 8; scanline++) {
            int destRow = kTitleTextRow + scanline;
            if (destRow >= (int)kTitleHeight) break;
            uint8_t* row = tbmp + destRow * 80;
            row[col*2] = 0; row[col*2+1] = 0;
            row[40+col*2] = 0; row[40+col*2+1] = 0;
        }

        // Re-render this char with NTSC blending
        uint8_t charIdx    = charByte & 0x3Fu;
        uint16_t glyphBase = kCharsetBase + charIdx * 8u;
        for (int scanline = 0; scanline < 8; scanline++) {
            int destRow = kTitleTextRow + scanline;
            if (destRow >= (int)kTitleHeight) break;
            uint8_t glyph = mem[glyphBase + (uint16_t)scanline];
            uint8_t* row  = tbmp + destRow * 80;
            renderNTSCGlyph(glyph, &row[col*2], &row[40+col*2]);
        }
    }

    // ---- cockpit region ------------------------------------------------------
    // Only re-render when cockpitDirty (set in initialize(); cleared here).
    // During static Standby the cockpit data is constant.
    if (!cockpitDirty) return;
    cockpitDirty = false;
    // ModeD $350D: 4 DL entries × 2 identical scan lines = 8 rows.
    //   Each entry reads 40 bytes of raw 2bpp bitmap; same data for both scans.
    // Mode4 $332D: 9 DL entries × 8 scan lines = 72 rows.
    //   40 chars/row, glyph per scanline from charset $3800 (set by dli_sub_4a0c).
    //   Glyph byte = 4 × 2-bit pixels (COLBK/COLPF0/COLPF1/COLPF2).
    uint8_t* cdest = (uint8_t*)cockpitBitmap->data;

    // ModeD rows 0-7
    for (int entry = 0; entry < 4; entry++) {
        const uint8_t* src = (const uint8_t*)&mem[0x350D + (uint16_t)(entry * 40)];
        for (int scan = 0; scan < 2; scan++) {
            int row = entry * 2 + scan;
            uint8_t* p1 = cdest + row * 80;
            uint8_t* p2 = p1 + 40;
            for (int b = 0; b < 40; b++) decode2bppByte(src[b], &p1[b], &p2[b]);
        }
    }

    // Mode4 rows 8-79
    for (int entry = 0; entry < 9; entry++) {
        const uint8_t* chars = (const uint8_t*)&mem[0x332D + (uint16_t)(entry * 40)];
        for (int scan = 0; scan < 8; scan++) {
            int row = 8 + entry * 8 + scan;
            uint8_t* p1 = cdest + row * 80;
            uint8_t* p2 = p1 + 40;
            for (int col = 0; col < 40; col++) {
                uint8_t glyphIdx  = chars[col] & 0x3Fu;
                uint8_t glyphData = mem[0x3800u + glyphIdx * 8u + (uint16_t)scan];
                decode2bppByte(glyphData, &p1[col], &p2[col]);
            }
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
