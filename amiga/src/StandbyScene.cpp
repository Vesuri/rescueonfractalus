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

// Native handler functions — see NativeHandlers.cpp and SfxPlayer.cpp.
extern "C" void vbi_handler_game_native(void);                  // $52D7: timer cascade
extern "C" void update_blink_timer_006e_native(void);           // $4131: cockpit blink
extern "C" void copy_altitude_graphic_to_screen_native(void);   // $782A: title text
extern "C" void startup_init_native(void);                      // $3FFA: cockpit digit update
extern "C" void update_gauge_digits_native(void);               // $4229: cockpit counter animation
extern "C" void tunnel_ring_tick_native(void);                  // $6A38/$6A4D: tunnel ring cycle ($0088 gate)

extern "C" volatile uint8_t mem[65536];

// tunnel.raw — 86 rows x 40 bytes of GTIA mode-10 nibbles (pens 1-6); see incbin.s.
extern "C" const uint8_t tunnel_raw[];

// Lookup table: byte → 16-bit doubled glyph pattern (each bit → 2 pixels).
// Filled once in initialize(); used by title render for mode-6 1bpp doubling.
static uint16_t kDoubleGlyph[256];

#include "../assets/title_pal.h"
#include "../assets/terrain_pal.h"
#include "../assets/atari_pal.h"
// cockpit_pal.h and cockpit_raw removed: cockpit palette is now fully dynamic
// from mem[] via atariToOCS(), cockpit bitmap decoded each frame in render().

static const uint16_t kW   = 320;
static const uint16_t kH   = 216;   // Atari attract = 216 visible scanlines
static const uint16_t kHT  = 86;    // terrain sprite/bitmap height (placeholder; M6a audit may revise)
static const uint8_t  kBP2 = 2;
static const uint8_t  kBP3 = 3;   // cockpit only — 3rd plane carries bit-7 chars (red)

static const uint32_t kCopperLen = 160;

// Display geometry: anchored at the standard PAL display-window top (0x2c).
// All boundary lines are derived so changing the top can't desync them.
static const uint16_t kDisplayTop    = 0x2c;               // DIWSTRT.y (PAL standard)
static const uint16_t kTitleHeight   = 42;                 // title region display lines
static const uint16_t kTerrainHeight = kHT;                // terrain region (placeholder, see kHT)
static const uint16_t kTerrainLine   = kDisplayTop + kTitleHeight;   // = 0x56 (86)
static const uint16_t kCockpitLine   = kTerrainLine + kTerrainHeight; // = 172
// Cockpit height: 4 modeD DL entries × 2 scan lines + 10 mode4 DL entries × 8 scans
// (title 42 + terrain 86 + cockpit 88 = 216 = kH).
static const uint16_t kCockpitH     = 4 * 2 + 10 * 8;               // = 88
// centerY so that DIWSTRT.y = kDisplayTop: centerY = kDisplayTop + kH/2 = 0x2c + 108 = 0x98
static const uint16_t kCenterY       = kDisplayTop + kH / 2;

// BPLCON0: 2 bitplanes, lores.
static const uint16_t kBPLCON0_2P = (uint16_t)((2 << PLNCNTSHFT) | USE_BPLCON3);
// Terrain + cockpit run at 3 bitplanes: the cockpit needs the 3rd plane for its
// bit-7 red chars, and the terrain/doors run 3bp too (3rd plane zeroed) so the
// tunnel reveal slots into the door gap with no mid-screen plane-count switch.
static const uint16_t kBPLCON0_3P = (uint16_t)((3 << PLNCNTSHFT) | USE_BPLCON3);

// Sprite horizontal positions (see fillSpriteData for slant details).
static const uint16_t kSprXLeft  = 0x81 + 17;
static const uint16_t kSprXRight = 0x81 + 285;

// ---- OCS colour helpers ------------------------------------------------------
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
    // Title palette.
    // vbi_handler_game ($52D7) sets every frame:
    //   COLPF0 ($D016) = mem[$00D8]  — title text colour (mode-6 col=1 chars)
    //   COLBK  ($D01A) = mem[$02C8]  — title background
    //   COLPF1 ($D017) = $78         — hardcoded blue (same role on real hw)
    // copy_altitude_graphic_to_screen_native sets mem[$00D8]=$44 for the
    // copyright block so the text colour changes per alternation.
    // Mode-6 selects the per-char text colour from the byte's top 2 bits:
    //   hi2=0 → COLPF0 = mem[$00D8]   (copyright block, e.g. $44)
    //   hi2=1 → COLPF1 = $78 (blue)   (RESCUE ON FRACTALUS!, hardcoded by vbi $52F7)
    // render() routes hi2=0 chars to col1 (plane1) and hi2=1 chars to col2
    // (plane2); "off" pixels use col0 = COLBK.
    {
        uint16_t tbg   = atariToOCS(mem[0x02C8]);  // COLBK = background
        uint16_t tpf0  = atariToOCS(mem[0x00D8]);  // COLPF0 = hi2=0 text
        uint16_t tpf1  = atariToOCS(0x78);         // COLPF1 = hi2=1 text (blue)
        d[idx++] = copperMove(color00, fadeColor(tbg,  f));
        d[idx++] = copperMove(color01, fadeColor(tpf0, f));
        d[idx++] = copperMove(color02, fadeColor(tpf1, f));
        d[idx++] = copperMove(color03, fadeColor(tbg,  f));
    }
    cl->showBitmap(idx, *titleBitmap);
    idx += 2 * kBP2;

    // Sprite colour registers.  The canopy posts must match the top/bottom
    // background grey (COLBK = mem[$02C8] = atariToOCS($06) = 0x555), so read
    // the same source rather than a hardcoded constant.
    d[idx++] = copperMove(color16, 0x000);
    d[idx++] = copperMove(color17, fadeColor(atariToOCS(mem[0x02C8]), f));

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
    // In Standby this is one full-height terrain band.  When the doors open it
    // splits into a sliding top half, the tunnel reveal, and a sliding bottom half.
    // Bitplane pointers go FIRST after every WAIT (timing-critical: they must land
    // in H-blank before display fetch); the 2bp->3bp switch + colours follow.  Zero-
    // height bands are skipped so we never emit two WAITs for the same scanline —
    // a second WAIT on a line already reached would run its pointer writes late,
    // into the visible area (the fully-open frame: top/bottom collapse to 0 rows).
    const uint32_t ta   = (uint32_t)terrainBitmap->data;
    const uint16_t half = kTerrainHeight / 2;                 // = 43
    const bool     door = (phase == Phase::DoorsOpening && doorGap > 0);
    const uint16_t g2   = door ? (uint16_t)(doorGap >> 1) : 0;  // half-gap in rows
    const uint16_t topH = (uint16_t)(half - g2);               // == bottom-band rows
    const bool tunnelFirst = door && (topH == 0);              // fully open: tunnel fills region
    const uint32_t tun  = (uint32_t)tunnelBitmap->data + (uint32_t)(half - g2) * 120u;

    auto emitBpl = [&](uint32_t base) {                        // 3bp interleaved = 120 B/row
        d[idx++] = copperMove(bpl1pth, (uint16_t)(base >> 16));
        d[idx++] = copperMove(bpl1ptl, (uint16_t)(base & 0xFFFF));
        d[idx++] = copperMove(bpl2pth, (uint16_t)((base + 40) >> 16));
        d[idx++] = copperMove(bpl2ptl, (uint16_t)((base + 40) & 0xFFFF));
        d[idx++] = copperMove(bpl3pth, (uint16_t)((base + 80) >> 16));
        d[idx++] = copperMove(bpl3ptl, (uint16_t)((base + 80) & 0xFFFF));
    };
    // Terrain palette (GTIA mode-10 nibbles 0/7/8): col0 black (also the tunnel's
    // pen 0), col1 LEVEL-04 text, col3 green background.  Ring feeds pens 1-6.
    const uint16_t terr0 = fadeColor(atariToOCS(mem[0x02C0]), f);
    const uint16_t terr1 = fadeColor(atariToOCS(mem[0x02C7]), f);
    const uint16_t terr2 = fadeColor(atariToOCS(mem[0x08D4]), f);
    const uint16_t terr3 = fadeColor(atariToOCS(mem[0x0071]), f);
    auto emitRing = [&]() {                                    // tunnel pens 1-3
        d[idx++] = copperMove(color01, fadeColor(atariToOCS(mem[0x08D4]), f));
        d[idx++] = copperMove(color02, fadeColor(atariToOCS(mem[0x08D5]), f));
        d[idx++] = copperMove(color03, fadeColor(atariToOCS(mem[0x08D6]), f));
    };
    auto emitTerrCols = [&]() {                                // terrain pens 1-3
        d[idx++] = copperMove(color01, terr1);
        d[idx++] = copperMove(color02, terr2);
        d[idx++] = copperMove(color03, terr3);
    };

    // ---- region WAIT: pointers first, then the 2bp->3bp switch, then colours ----
    d[idx++] = copperWait(kTerrainLine - 1, 0xE0);
    emitBpl(tunnelFirst ? tun : (ta + (uint32_t)g2 * 120u));   // top half slides up (g2 rows)
    d[idx++] = copperMove(bplcon0, kBPLCON0_3P);
    d[idx++] = copperMove(bpl1mod, 80);                        // BPL1MOD = odd planes 1&3
    d[idx++] = copperMove(bpl2mod, 80);                        // BPL2MOD = plane 2
    d[idx++] = copperMove(color00, terr0);                     // pen 0 = black (terrain & tunnel)
    if (tunnelFirst) emitRing(); else emitTerrCols();
    if (door) {                                                // ring upper half (pens 4-6),
        d[idx++] = copperMove(color04, fadeColor(atariToOCS(mem[0x08D7]), f));  // unused by terrain
        d[idx++] = copperMove(color05, fadeColor(atariToOCS(mem[0x08D8]), f));
        d[idx++] = copperMove(color06, fadeColor(atariToOCS(mem[0x08D9]), f));
    }

    // ---- mid-screen bands (only when partially open; each on its own WAIT) ----
    if (door && topH > 0) {
        // Tunnel reveal, centred on its vanishing point (row 43): the gap shows the
        // deepest part first and widens outward.  The tunnel band has no end WAIT,
        // so if the bottom band is dropped below it simply extends to the cockpit.
        d[idx++] = copperWait((uint16_t)(kTerrainLine + topH - 1), 0xE0);
        emitBpl(tun);
        emitRing();
        // Bottom door half (slides down): terrain from row kTerrainHeight/2.  Emit
        // it only while its WAIT clears the cockpit WAIT by a margin — otherwise two
        // WAITs land on the same/adjacent line and the second one's pointer writes
        // run into the visible area (the near-fully-open frames).  When skipped, the
        // tunnel covers the last 1-2 rows of the gap, which is imperceptible.
        const uint16_t botWaitY = (uint16_t)(kTerrainLine + half + g2 - 1);
        if (botWaitY + 2 <= (uint16_t)(kCockpitLine - 1)) {
            d[idx++] = copperWait(botWaitY, 0xE0);
            emitBpl(ta + (uint32_t)half * 120u);
            emitTerrCols();
        }
    }

    // ---- cockpit region ------------------------------------------------------
    // The cockpit runs at 3 bitplanes (title/terrain stay 2bp): mode-4's bit-7
    // chars swap pixel-11 from COLPF2 (salmon) to COLPF3 (red), which can't be
    // expressed in 2bp since salmon and red share scanlines.  We give those chars
    // plane3 = 0xFF (render()), shifting their pixels from colours 0-3 to 4-7;
    // colours 4-6 mirror 0-2, and colour 7 = red, so a bit-7 char matches a normal
    // one except its pixel-11 is red.
    d[idx++] = copperWait(kCockpitLine - 1, 0xE0);
    uint32_t ca = (uint32_t)cockpitBitmap->data;
    d[idx++] = copperMove(bpl1pth, (uint16_t)(ca >> 16));
    d[idx++] = copperMove(bpl1ptl, (uint16_t)(ca & 0xFFFF));
    d[idx++] = copperMove(bpl2pth, (uint16_t)((ca + 40) >> 16));
    d[idx++] = copperMove(bpl2ptl, (uint16_t)((ca + 40) & 0xFFFF));
    d[idx++] = copperMove(bpl3pth, (uint16_t)((ca + 80) >> 16));
    d[idx++] = copperMove(bpl3ptl, (uint16_t)((ca + 80) & 0xFFFF));
    // Switch to 3 bitplanes + 3bp interleaved modulo (= (3-1)*40) for the cockpit.
    d[idx++] = copperMove(bplcon0, (uint16_t)((3 << PLNCNTSHFT) | USE_BPLCON3));
    d[idx++] = copperMove(bpl1mod, 80);
    d[idx++] = copperMove(bpl2mod, 80);
    // Cockpit palette — cockpit DLIs ($6D4F/$6D67/$6D7C) reload the registers for
    // these scanlines with hardcoded immediates (NOT the $02C4-$02C8 title/terrain
    // shadows), so we use the same constants:
    //   pixel 00 = COLBK  = $00 (black);   01 = COLPF0 = $04 (dark gray)
    //   pixel 10 = COLPF1 = $06 (gray);    11 = COLPF2 = $2C (salmon)
    //   bit-7 char pixel 11 = COLPF3 = $26 (red), via plane3 → colour 7.
    d[idx++] = copperMove(color00, fadeColor(atariToOCS(0x00), f));
    d[idx++] = copperMove(color01, fadeColor(atariToOCS(0x04), f));
    d[idx++] = copperMove(color02, fadeColor(atariToOCS(0x06), f));
    d[idx++] = copperMove(color03, fadeColor(atariToOCS(0x2C), f));
    d[idx++] = copperMove(color04, fadeColor(atariToOCS(0x00), f));  // = col0 (black)
    d[idx++] = copperMove(color05, fadeColor(atariToOCS(0x04), f));  // = col1 (dark gray)
    d[idx++] = copperMove(color06, fadeColor(atariToOCS(0x06), f));  // = col2 (gray)
    d[idx++] = copperMove(color07, fadeColor(atariToOCS(0x26), f));  // red (COLPF3)

    // Terminate the list right after the content.  The instruction count varies
    // per frame (the door split adds/removes bands), and the two double-buffered
    // lists are rebuilt at different gaps — without a terminator here the copper
    // runs into each buffer's stale trailing instructions (old band WAITs/pointer
    // writes), which differ between buffers and flicker the display every frame.
    d[idx++] = copperWait(255, 254);
}

// ---- public interface --------------------------------------------------------
void StandbyScene::initialize()
{
    Palette::initialize();
    palette       = new Palette(kTitlePalette, 4, /*fade*/0);
    titleBitmap   = Bitmap::allocate(kW, kTitleHeight,   kBP2, true);
    terrainBitmap = Bitmap::allocate(kW, kTerrainHeight, kBP3, true);  // 3bp: tunnel reveal uses pens 4-7
    cockpitBitmap = Bitmap::allocate(kW, kCockpitH, kBP3, true);  // 3bp: bit-7 chars → red
    tunnelBitmap  = Bitmap::allocate(kW, kTerrainHeight, kBP3, true);  // door-gap reveal

    // Decode the static tunnel image (tunnel.raw, GTIA-10 nibbles = pens 1-6) into
    // the 3bp interleaved bitmap once.  Each byte = 2 GTIA pixels (4 Amiga px each);
    // pen bit b -> plane(b+1).  Motion comes from cycling COLOR01-06, not the bitmap.
    if (tunnelBitmap) {
        uint8_t* vdest = (uint8_t*)tunnelBitmap->data;
        for (int row = 0; row < (int)kTerrainHeight; row++) {
            const uint8_t* src = &tunnel_raw[row * 40];
            uint8_t* p1 = vdest; uint8_t* p2 = vdest + 40; uint8_t* p3 = vdest + 80;
            for (int b = 0; b < 40; b++) {
                uint8_t ph = (uint8_t)((src[b] >> 4) & 0xF), pl = (uint8_t)(src[b] & 0xF);
                p1[b] = (uint8_t)(((ph & 1) ? 0xF0u : 0u) | ((pl & 1) ? 0x0Fu : 0u));
                p2[b] = (uint8_t)(((ph & 2) ? 0xF0u : 0u) | ((pl & 2) ? 0x0Fu : 0u));
                p3[b] = (uint8_t)(((ph & 4) ? 0xF0u : 0u) | ((pl & 4) ? 0x0Fu : 0u));
            }
            vdest += 120;
        }
    }

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

    // Precompute glyph doubling table: each byte → 16-bit pattern (each bit → 2 bits).
    for (int i = 0; i < 256; i++) {
        uint16_t out = 0;
        for (int b = 7; b >= 0; b--)
            if (i & (1 << b)) out |= (uint16_t)3u << (b * 2);
        kDoubleGlyph[i] = out;
    }

    paula_audio_init();      // loads screen3_mem.bin into mem[] (Standby scene snapshot)

    // Patch mem[] values that are mid-animation in the snapshot.
    // (Must happen before initial render() call below.)
    // screen3_mem.bin was captured mid-animation; game_entry sets these on startup.
    // mem[$0071]: COLBK source for terrain rows (DLI dli_sub_6cf1 reads it).
    //   Snapshot has $DB (mid-animation); attract init targets $C8 (green, $C8=hue12/luma4).
    //   SDL oracle (atari000.png) shows terrain as (82,140,22) = $C8.
    mem[0x0071] = 0xC8;   // COLBK source for terrain DLI → green
    mem[0x02C0] = 0x00;   // COLPM0 → nibble-0 terrain dots; $00=black matches SDL oracle
                           // (music_playing.a8s has $18=orange from mid-animation)

    // mem[$00D8] is the title TEXT colour (COLPF0); snapshot has $78 (blue). Leave as-is.
    // mem[$02C8] is the title BACKGROUND (COLBK); snapshot has $06 (grey). Leave as-is.

    // vbi_handler_2 attract path: steady-state has $0044=0 (FUN_47A3 already ran)
    // and $063E=$FF (BMI path taken, no further timer expiry).  The snapshot captured
    // these mid-animation; patch to steady-state to avoid spurious $480B/FUN_47A3 firing.
    mem[0x0044] = 0x00;   // prevent FUN_47A3 one-shot (would set $00D8=$CA = yellow)
    mem[0x063E] = 0xFF;   // prevent $480B title-clear on first frame

    // Seed $0091=$C0 so copy_altitude_graphic_to_screen_native fires on the first
    // update() call and writes Block1 ("rescue on fractalus") to $32B7.
    // On the real Atari, $0091 is set by the SFX sequencer; we prime it once here
    // so the title is correct before the first SFX tick produces a $C0 byte.
    mem[0x0091] = 0xC0;

    // Initial render: populate all three bitmaps from mem[] once so that
    // render() called from the main loop has nothing to do until data changes.
    render();
}

void StandbyScene::openDoors()
{
    if (phase != Phase::Standby) return;
    phase = Phase::DoorsOpening;
    doorGap = 0;

    // Arm the tunnel-ring cycle the way the 6502 game does: the ring colours live
    // in mem[$08D4-$08D9] (feeding COLOR01-06) and are rotated by step_accum_add_75
    // ($6A38) when the $5367 dispatcher sees gate $0088 != 0.  The Standby snapshot
    // has all of this zeroed (the accumulator is normally seeded from live flight
    // state, absent here), so we seed: the visible blue ring ramp, a zero $A1-$A5
    // accumulator (literal-$75 / zero-seed fidelity choice), and the $0088 gate.
    static const uint8_t kRingRamp[6] = { 0x30, 0x32, 0x34, 0x36, 0x38, 0x3A };
    for (int i = 0; i < 6; i++) mem[0x08D4u + (uint16_t)i] = kRingRamp[i];
    mem[0x00A1] = mem[0x00A2] = mem[0x00A3] = mem[0x00A4] = mem[0x00A5] = 0u;
    mem[0x008D] = 0u;     // forward cycle (step_accum_add_75, not _sub_7e)
    mem[0x0088] = 1u;     // gate: dispatcher routes to the tunnel-ring branch
}

void StandbyScene::update(uint16_t frame)
{
    // Advance the doors-open transition: gap grows 2 rows/frame (~43 frames to
    // fully open, matching the Atari $008A=$2B counter), then holds open.
    if (phase == Phase::DoorsOpening && doorGap < kTerrainHeight) {
        doorGap = (uint16_t)(doorGap + 2);
        if (doorGap > kTerrainHeight) doorGap = kTerrainHeight;
    }
    // Tunnel palette cycle: the genuine $5367-dispatcher $0088 branch — add $75
    // into the $A1-$A4 accumulator and, when its top byte changes, rotate the
    // 6-colour ring in mem[$08D4-$08D9].  Seeded zero in openDoors(), so the
    // rotation cadence is whatever the verbatim 6502 algorithm produces.
    tunnel_ring_tick_native();

    vbi_handler_game_native();           // $52D7: attract timer cascade
    update_blink_timer_006e_native();    // $4131: cockpit blink lights
    // sfx_voice_tick_native() is now driven by CIA-B Timer A at ~100 Hz (main.cpp).

    // Mirror $62E7 SFX-reinit gate: when mem[$0090] is non-zero the attract loop
    // calls JSR $70E7 (sfx init) which resets the sequence to index 0.
    // mem[$0090] = 1 in the snapshot; cleared here after first reinit.
    if (mem[0x0090u]) {
        mem[0x073Au] = 0u;    // immediate underflow → next CIA tick loads note[0]
        mem[0x073Cu] = 0xFFu; // sequence ptr before index 0
        mem[0x0090u] = 0u;    // clear flag (as $70E7 does via STX $0090)
    }

    if (mem[0x060B] == 0)               // $62FB: title text (gated by $060B)
        copy_altitude_graphic_to_screen_native();    // $782A: $0091→title string

    // FUN_4229 ($4229): gauge/counter animation AND — when mem[$007E]==$80 — the
    // random blink of the centre-bottom indicator lights ($3492-$3497).  The
    // original vbi_handler_game calls it EVERY OTHER FRAME and is NOT gated by
    // $004A: LSR $0643 / BCS skip / JSR $4229 / INC $0643 ($5342).  (Our earlier
    // $004A gate suppressed the Standby blink entirely.)
    {
        uint8_t g = mem[0x0643u];
        mem[0x0643u] = (uint8_t)(g >> 1);          // LSR $0643
        if (!(g & 1u)) {                           // carry clear → run, then INC
            update_gauge_digits_native();
            mem[0x0643u]++;
        }
        // Any cockpit RAM changes (e.g. the centre-bottom indicator-light blink
        // at $3491-$3498) are picked up by render()'s per-cell shadow compare.
    }

    if (mem[0x004A] != 0) {             // $004A set when game starts (door sequence)
        startup_init_native();          // $3FFA: cockpit digit update
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



// GTIA mode-10 nibble → Amiga 2bp colour index.
// Zones: 0→bg, 1-2→COLPF0-1 (col1), 3-4→COLPF2-3 (col2), 5-8+→COLPM0-3 (col3).
// Each Atari byte = 2 GTIA pixels (4 Amiga pixels each); 40 bytes → 80 GTIA px → 320 Amiga px.
// In the 2bp interleaved layout (40 bytes plane1, 40 bytes plane2 per row):
//   byte b: high nibble covers Amiga pixels 8b..8b+3 → plane bits [7:4] of byte b
//            low nibble covers Amiga pixels 8b+4..8b+7 → plane bits [3:0] of byte b
// In this scene only three nibble values occur: 0 (road dots → COLPM0),
// 7 ("LEVEL 04" text → COLPF3) and 8 (background → COLBK).  GTIA mode-10 maps
// nibble→register as 0-3:COLPM0-3, 4-7:COLPF0-3, 8:COLBK.  We collapse to our
// 4 Amiga colours: col0=COLPM0 (road), col1=COLPF3 (text), col3=COLBK (bg);
// col2 is unused here.  Nibble 7 MUST be distinct from 8 or the level text
// (baked into the bitmap as COLPF3 pixels) vanishes into the green background.
static const uint8_t kNibbleColour[16] = {
    0,           // 0   → COLPM0 → col0 (road dots / black)
    3, 3, 3,     // 1-3 → (unused) → bg
    3, 3, 3,     // 4-6 → (unused) → bg
    1,           // 7   → COLPF3 → col1 ("LEVEL 04" text)
    3,           // 8   → COLBK  → col3 (green background)
    3, 3, 3, 3, 3, 3, 3  // 9-15 → bg
};

void StandbyScene::render()
{
    // ---- terrain / door view ------------------------------------------------
    // Only re-render when terrainDirty (set in initialize(); cleared here).
    // During static Standby the terrain is constant ($88 = closed door).
    // DL $3000: 86 Mode-F rows from $2000, stride 46 (40 data + 6 pad), GTIA mode 10.
    // Each byte = two 4-bit nibbles → two GTIA pixels, each 4 Amiga pixels wide.
    // Interleaved 3bp row = 40 bytes plane1 + 40 plane2 + 40 plane3 (plane3 = 0
    // for the doors; the tunnel reveal fills it for pens 4-7).

    // Wide-playfield crop: the terrain region runs in a GTIA-10 *wide* playfield
    // (48 bytes / 192 colour clocks — SDMCTL bit set, DMACTL rewritten per region
    // by the DLI at $4F78).  The road/door content is centred for the 48-byte
    // field (centre dash at byte 23 ≈ wide centre), so the TV-visible 320px window
    // is the central 40 bytes — skip the 4 left overscan bytes (pure green fill).
    // This matches the title region, which already crops 2 mode-6 chars (= 16cc).
    static const int kTerrainXByteOffset = 4;
    if (terrainDirty) {
        terrainDirty = false;
        uint8_t* vdest = (uint8_t*)terrainBitmap->data;
        for (int row = 0; row < (int)kTerrainHeight; row++) {
            const uint8_t* src = (const uint8_t*)&mem[0x2000 + row * 46 + kTerrainXByteOffset];
            uint8_t* plane1 = vdest;
            uint8_t* plane2 = vdest + 40;
            uint8_t* plane3 = vdest + 80;
            for (int b = 0; b < 40; b++) {
                uint8_t hi = kNibbleColour[(src[b] >> 4) & 0xF];
                uint8_t lo = kNibbleColour[src[b] & 0xF];
                plane1[b] = (uint8_t)(((hi & 1) ? 0xF0u : 0u) | ((lo & 1) ? 0x0Fu : 0u));
                plane2[b] = (uint8_t)(((hi & 2) ? 0xF0u : 0u) | ((lo & 2) ? 0x0Fu : 0u));
                plane3[b] = 0;
            }
            vdest += 120;
        }
    }

    // ---- title region -------------------------------------------------------
    // Shadow-compare: re-render only chars whose byte changed since last frame.
    // titleShadow[] mirrors $32B7-$32CA; updated here on change.
    // Chars start at $32B7 (skip $32B5/$32B6 left-border), charset $3800 (NTSC).
    static const int      kTitleTextRow  = 21;
    static const uint16_t kScreenRAM    = 0x32B7;
    // CHBAS=$04 ($0400): vbi_handler_game sets this each VBI. dli_sub_4a0c fires
    // at scanY=28 (after title scanlines 20-27) → title uses $0400 for all 8 scans.
    static const uint16_t kCharsetBase  = 0x0400;

    uint8_t* tbmp = (uint8_t*)titleBitmap->data;
    for (int col = 0; col < 20; col++) {
        uint8_t charByte = mem[kScreenRAM + (uint16_t)col];
        if (charByte == titleShadow[col]) continue;   // unchanged — skip
        titleShadow[col] = charByte;

        // Re-render this char: mode-6 is 1bpp, but the byte's top 2 bits select
        // the text colour register.  We support the two cases that occur here:
        //   hi2=0 → COLPF0 → col1 (plane1)   — copyright block
        //   hi2=1 → COLPF1 → col2 (plane2)   — RESCUE ON FRACTALUS! (blue)
        // "off" pixels stay col0 (COLBK).  Each glyph bit is doubled to 2 px.
        uint8_t charIdx    = charByte & 0x3Fu;
        bool    usePF1     = ((charByte >> 6) & 3u) == 1u;   // hi2=1 → COLPF1/blue
        uint16_t glyphBase = kCharsetBase + charIdx * 8u;
        for (int scanline = 0; scanline < 8; scanline++) {
            int destRow = kTitleTextRow + scanline;
            if (destRow >= (int)kTitleHeight) break;
            uint8_t glyph = mem[glyphBase + (uint16_t)scanline];
            uint16_t doubled = kDoubleGlyph[glyph];
            uint8_t hb = (uint8_t)(doubled >> 8);
            uint8_t lb = (uint8_t)(doubled & 0xFF);
            uint8_t* row = tbmp + destRow * 80;
            // plane1 carries COLPF0 chars, plane2 carries COLPF1 chars.
            row[col*2]       = usePF1 ? 0 : hb;
            row[col*2+1]     = usePF1 ? 0 : lb;
            row[40+col*2]    = usePF1 ? hb : 0;
            row[40+col*2+1]  = usePF1 ? lb : 0;
        }
    }

    // ---- cockpit region ------------------------------------------------------
    // Per-cell shadow compare (like the title region above): re-decode only the
    // cells whose Atari source byte changed since last frame.  A single Atari
    // write therefore costs 6 Amiga writes (modeD: 2 rows × 3 planes) or 24
    // (mode4: 8 scanlines × 3 planes) — never a full-region re-decode.
    const bool cockpitFull = cockpitForceFull;
    cockpitForceFull = false;
    // The cockpit shares the terrain's WIDE playfield (48 bytes/line).  Both the
    // modeD and mode4 blocks are sequential DL entries (LMS only on the first
    // line), so each successive row advances by ANTIC's fetch width = 48 bytes,
    // NOT 40.  As with the terrain we render the central 40 bytes of each line
    // (+4-byte crop) so the content centres.  Using stride 40 / offset 0 mis-reads
    // every row past the first → the garbled "modulo" shear.
    //
    // ModeD $350D: 4 DL entries × 2 identical scan lines = 8 rows (raw 2bpp).
    // Mode4 $332D: 10 DL entries × 8 scan lines = 80 rows ($332D..$350D = 10×48).
    //   40 chars/row, glyph per scanline from charset $3800.
    //   Glyph byte = 4 × 2-bit pixels (COLBK/COLPF0/COLPF1/COLPF2).
    static const int kCockpitStride    = 48;   // wide-playfield bytes per line
    static const int kCockpitXByteCrop = 4;    // skip 4 left overscan bytes (= terrain)
    uint8_t* cdest = (uint8_t*)cockpitBitmap->data;

    // 3bp interleaved row = plane1(40) + plane2(40) + plane3(40) = 120 bytes.
    static const int kRowBytes = 120;

    // ModeD rows 0-7 (raw bitmap, no bit-7 colour swap → plane3 = 0).  Each
    // source byte feeds the same column of 2 identical scan lines.
    for (int entry = 0; entry < 4; entry++) {
        const uint8_t* src = (const uint8_t*)&mem[0x350D + (uint16_t)(entry * kCockpitStride + kCockpitXByteCrop)];
        for (int b = 0; b < 40; b++) {
            uint8_t s = src[b];
            int sh = entry * 40 + b;
            if (!cockpitFull && s == cockpitModeDShadow[sh]) continue;
            cockpitModeDShadow[sh] = s;
            uint8_t p1v, p2v;
            decode2bppByte(s, &p1v, &p2v);
            for (int scan = 0; scan < 2; scan++) {
                uint8_t* p1 = cdest + (entry * 2 + scan) * kRowBytes;
                p1[b] = p1v; p1[40 + b] = p2v; p1[80 + b] = 0;
            }
        }
    }

    // Mode4 rows 8-87.  Char bit-7 → plane3 = 0xFF for that cell, shifting its
    // pixels to colours 4-7 (col7 = red); bit-7 clear → plane3 = 0 (cols 0-3).
    // One changed char re-decodes its 8 scanlines × 3 planes = 24 writes.
    for (int entry = 0; entry < 10; entry++) {
        const uint8_t* chars = (const uint8_t*)&mem[0x332D + (uint16_t)(entry * kCockpitStride + kCockpitXByteCrop)];
        for (int col = 0; col < 40; col++) {
            uint8_t ch = chars[col];
            int sh = entry * 40 + col;
            if (!cockpitFull && ch == cockpitMode4Shadow[sh]) continue;
            cockpitMode4Shadow[sh] = ch;
            // Mode-4 glyph index is bits 0-6 (128 glyphs); bit 7 is the
            // COLPF2/PF3 colour flag (handled via plane3).
            uint8_t plane3 = (ch & 0x80u) ? 0xFFu : 0x00u;
            for (int scan = 0; scan < 8; scan++) {
                uint8_t* p1 = cdest + (8 + entry * 8 + scan) * kRowBytes;
                uint8_t glyphData = mem[0x3800u + (ch & 0x7Fu) * 8u + (uint16_t)scan];
                uint8_t p1v, p2v;
                decode2bppByte(glyphData, &p1v, &p2v);
                p1[col] = p1v; p1[40 + col] = p2v; p1[80 + col] = plane3;
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
    delete tunnelBitmap;  tunnelBitmap  = nullptr;
    delete palette;       palette       = nullptr;
    delete leftPost;      leftPost      = nullptr;
    delete rightPost;     rightPost     = nullptr;
    delete nullSprite;    nullSprite    = nullptr;
}
