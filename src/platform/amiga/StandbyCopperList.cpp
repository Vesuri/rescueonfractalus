#define ECS_SPECIFIC
#include <hardware/custom.h>
#include <graphics/display.h>
#include <proto/exec.h>
#include <exec/memory.h>
#include "StandbyCopperList.h"
#include "framework/AmigaHardware.h"
#include "framework/Bitmap.h"
#include "framework/Sprite.h"
#include "assets/atari_pal.h"   // atariToOCS() for the constant cockpit palette

// ---- display geometry --------------------------------------------------------
// MUST match the constants in RescueOnFractalus.cpp (same derivation).
static const uint16_t kW            = 320;
static const uint16_t kH            = 216;
static const uint8_t  kBP2          = 2;
static const uint8_t  kBP3          = 3;
static const uint16_t kDisplayTop   = 0x2c;
static const uint16_t kTitleHeight  = 42;
static const uint16_t kTerrainHeight = 86;
static const uint16_t kTerrainLine  = kDisplayTop + kTitleHeight;     // = 0x56
static const uint16_t kCockpitLine  = kTerrainLine + kTerrainHeight;  // = 172
static const uint16_t kCenterY      = kDisplayTop + kH / 2;           // = 0x98
static const uint16_t kBPLCON0_3P   = (uint16_t)((3 << PLNCNTSHFT) | USE_BPLCON3);
// First scanline BELOW the energy-gauge dial = the gauge sprite's base line + its row count
// (RescueOnFractalus.cpp: setY base 0x2c+144, kEnergyRows 56 — instrument #12, 8x56 at y=144).
// Measured in the cockpit bitmap: the dial slot (pen 0, x204-211) is open on rows 16..71 and
// the dashboard closes over it with pen 2 from row 72 = this line.
static const uint16_t kGaugeBottomLine = 0x2c + 144 + 56;             // = 244

// ---- fixed list layout (indices into data_, in 32-bit MOVE/WAIT words) -------
// d[0] = copperWait(16,0) (CopperList ctor).  setPlayfield now emits 3 words
// (BPLCON0 + BPL1MOD/BPL2MOD — the per-region-varying regs); the constant playfield
// registers are set once by AmigaHardware::setPlayfield (see RescueOnFractalus::initialize).
#define INDEX_PLAYFIELD       1
#define INDEX_TITLE_PAL       (INDEX_PLAYFIELD + 3)    // color00..03 (4)
#define INDEX_TITLE_BPL       (INDEX_TITLE_PAL + 4)    // 17: title bitmap ptrs (2bp = 4)
#define INDEX_SPRITE_COL      (INDEX_TITLE_BPL + 4)    // 21: color16,color17 (2)
#define INDEX_SPRITES         (INDEX_SPRITE_COL + 2)   // 23: 8 sprite ptrs (16)
#define INDEX_ENERGY_COL       (INDEX_SPRITES + 16)     // 39: COLOR21 ($1AA) (1)
#define INDEX_COMPASS_WAIT    (INDEX_ENERGY_COL + 1)    // 40: WAIT(compass scanline) (1)
#define INDEX_COMPASS_COL     (INDEX_COMPASS_WAIT + 1) // 41: color01 = compass COLPF0 (1)
// Terrain (door) region — per-scanline-LMS "runs" for the level-select "elevator" scroll.  The
// Atari launch DL gives each mode-F scanline its own LMS ($300A, stride 3); during the digit roll
// dl_lms_fill leaves stale entries so the viewport shows several RUNS of consecutive field rows
// (measured).  setTerrainRuns emits: a fixed prologue (WAIT + bplcon0 + modulo + palette) + run 0's
// BPLxPT, then runs 1.. (each WAIT + 6 BPLxPT), then the CONSTANT cockpit region IMMEDIATELY after
// the last run (emitCockpitRegion) — NO no-op padding, so the copper never churns into / delays the
// cockpit WAIT.  The interleaved modulo (80) auto-advances the pointers one row/scanline within a run.
#define MAX_TERRAIN_RUNS      20
#define INDEX_TERRAIN_WAIT    (INDEX_COMPASS_COL + 1)     // WAIT(kTerrainLine-1) — run-0 entry (1)
#define INDEX_TERRAIN_BPLCON0 (INDEX_TERRAIN_WAIT + 1)    // bplcon0 3P (1)
#define INDEX_TERRAIN_MOD     (INDEX_TERRAIN_BPLCON0 + 1) // bpl1mod,bpl2mod (2)
#define INDEX_TERRAIN_PAL     (INDEX_TERRAIN_MOD + 2)     // color00..03 (4) — poked by updateStandbyCopper
#define INDEX_TERRAIN_BPL0    (INDEX_TERRAIN_PAL + 4)     // run-0 bitmap ptrs (3bp = 6)
#define INDEX_TERRAIN_RUNS    (INDEX_TERRAIN_BPL0 + 6)    // FLOATING: runs 1.. (WAIT+6) then cockpit region
// Cockpit region (re-emitted after the last run): WAIT(1) + BPLxPT(6) + bplcon0(1) + mod(2) +
// color01..07(7) + 3×(WAIT+color00 band/dash/floor splits)(6) + WAIT+GAUGE_BOTTOM COLOR21(2) +
// terminator(1) = 26.
#define COCKPIT_REGION_LEN    26
#define LIST_LENGTH           (INDEX_TERRAIN_RUNS + (MAX_TERRAIN_RUNS - 1) * 7 + COCKPIT_REGION_LEN)

StandbyCopperList::StandbyCopperList()
    : CopperList((uint32_t*)AllocMem(LIST_LENGTH << 2, MEMF_CHIP | MEMF_CLEAR), LIST_LENGTH, true)
{
}

void StandbyCopperList::buildLayout(const Bitmap& title, const Bitmap& terrain, const Bitmap& cockpit,
                                    const Sprite& leftPost, const Sprite& rightPost, const Sprite& nullSprite)
{
    uint32_t* d = data_;

    // ---- title region: playfield (2bp interleaved) ----
    // Emits BPLCON0 (2bp) + BPL1MOD/BPL2MOD for the title band; the constant playfield
    // registers (incl. BPLCON2 = PF priority) are set once in RescueOnFractalus::initialize.
    setPlayfield(INDEX_PLAYFIELD, kW, kH, kBP2, /*interleaved*/true,
                 /*hires*/false, /*interlace*/false, /*dualPlayfield*/false,
                 /*holdAndModify*/false, kCenterY);

    // Title palette + bitmap pointers (palette refreshed each frame via setters).
    setTitlePalette(0, 0, 0);                  // seeded; caller refreshes
    showBitmap(INDEX_TITLE_BPL, title);        // 2bp interleaved = 4 ptr moves

    // Sprite colour regs: COLOR16 = black (const), COLOR17 = canopy-post grey (setter).
    d[INDEX_SPRITE_COL] = copperMove(color16, 0x000);
    setSpritePostColor(0);
    // Sprite pointers: 0=left post, 1=right post, 2=null (gauge via setSprite2),
    // 3..7=null.  All const except sprite 2.
    showSprite(INDEX_SPRITES + 0,  0, leftPost);
    showSprite(INDEX_SPRITES + 2,  1, rightPost);
    showSprite(INDEX_SPRITES + 4,  2, nullSprite);
    showSprite(INDEX_SPRITES + 6,  3, nullSprite);
    showSprite(INDEX_SPRITES + 8,  4, nullSprite);
    showSprite(INDEX_SPRITES + 10, 5, nullSprite);
    showSprite(INDEX_SPRITES + 12, 6, nullSprite);
    showSprite(INDEX_SPRITES + 14, 7, nullSprite);
    setEnergyIndicatorColor(0);                          // COLOR21 gauge bar (setter)

    // ---- compass band: color01 = compass COLPF0 ($00CF) for the mode-4 compass line ----
    d[INDEX_COMPASS_WAIT] = copperWait(kDisplayTop + 33 - 1, 0xE0);
    setCompassColor(0);                        // poked from $00CF

    // ---- terrain region: WAIT (end of prev line) + prologue, then run segments ----
    d[INDEX_TERRAIN_WAIT] = copperWait(kTerrainLine - 1, 0xE0);
    d[INDEX_TERRAIN_BPLCON0] = copperMove(bplcon0, kBPLCON0_3P);
    d[INDEX_TERRAIN_MOD]     = copperMove(bpl1mod, 80);   // 3bp interleaved = (3-1)*40
    d[INDEX_TERRAIN_MOD + 1] = copperMove(bpl2mod, 80);
    setTerrainPalette(0, 0, 0, 0);             // seeded; caller refreshes
    // Seed a single run (rows 0..85 at offset 0 = resting doors); setTerrainRuns also emits the
    // cockpit region + terminator right after it.  doorScrollVblankUpdate rewrites the runs during
    // the level-select scroll.  Store the cockpit bitmap so setTerrainRuns can re-emit its region.
    cockpitBmp_ = &cockpit;
    { uint8_t scan0 = 0; uint16_t row0 = 0; setTerrainRuns(terrain, &scan0, &row0, 1); }
}

// emitCockpitRegion(): write the CONSTANT cockpit region (+ terminator) at list index `idx`, right
// after the last terrain run — so no no-op padding sits between the runs and the cockpit WAIT (that
// churn used to delay/garble the cockpit).  The cockpit content never varies during Standby (the
// cockpit DLIs reload hardcoded immediates; updateStandbyCopper never touches it), so re-emitting it
// per frame is just ~25 constant writes.  Returns the next free index.
uint32_t StandbyCopperList::emitCockpitRegion(uint32_t idx)
{
    uint32_t* d = data_;
    d[idx++] = copperWait(kCockpitLine - 1, 0xE0);
    showBitmap(idx, *cockpitBmp_, 1, 1, 0, 0, 3);  idx += 6;   // 3bp interleaved = 6 ptr moves
    d[idx++] = copperMove(bplcon0, kBPLCON0_3P);
    d[idx++] = copperMove(bpl1mod, 80);
    d[idx++] = copperMove(bpl2mod, 80);
    // Cockpit palette (hardcoded immediates the cockpit DLIs reload; color00 inherited from terrain).
    d[idx++] = copperMove(color01, atariToOCS(0x04));
    d[idx++] = copperMove(color02, atariToOCS(0x06));
    d[idx++] = copperMove(color03, atariToOCS(0x2C));
    d[idx++] = copperMove(color04, atariToOCS(0x00));
    d[idx++] = copperMove(color05, atariToOCS(0x04));
    d[idx++] = copperMove(color06, atariToOCS(0x06));
    d[idx++] = copperMove(color07, atariToOCS(0x26));
    // Windscreen-band / dash / floor COLBK splits (see the atari-scanline map above).
    d[idx++] = copperWait(kCockpitLine + 8 - 1, 0xE0);   d[idx++] = copperMove(color00, atariToOCS(0x00));
    d[idx++] = copperWait(kCockpitLine + 10 - 1, 0xE0);  d[idx++] = copperMove(color00, atariToOCS(0x90));
    // Energy bar (sprite 2 / COLOR21) → black at the DIAL BOTTOM, not at the floor.  The Amiga bar
    // is one SOLID kEnergyRows sprite whose VSTART tracks the fuel (buildEnergyIndicatorSprite),
    // so at anything below full fuel its bottom hangs `top` rows past the dial — where the Atari's
    // per-row P1 strip simply stops.  Clipping the pen at the dial bottom reproduces the strip
    // exactly and does it independently of BPLCON2: below the dial the dashboard is playfield pen 2
    // for 8 rows and then COLOR00 for the floor, and NO sprite/playfield priority can hide a sprite
    // over COLOR00.  (Also covers the fuel==0 park at line 252.)  COLOR21 is pair 1 pen 01 = this
    // bar alone here — ch3 is the null sprite.
    d[idx++] = copperWait(kGaugeBottomLine - 1, 0xE0);   d[idx++] = copperMove(color21, 0x000);
    d[idx++] = copperWait(kCockpitLine + 80 - 1, 0xE0);  d[idx++] = copperMove(color00, atariToOCS(0x00));
    d[idx++] = copperWait(255, 254);       // terminator
    return idx;
}

// ---- per-frame setters -------------------------------------------------------
void StandbyCopperList::setTitlePalette(uint16_t bg, uint16_t pf0, uint16_t pf1)
{
    data_[INDEX_TITLE_PAL + 0] = copperMove(color00, bg);
    data_[INDEX_TITLE_PAL + 1] = copperMove(color01, pf0);
    data_[INDEX_TITLE_PAL + 2] = copperMove(color02, pf1);
    data_[INDEX_TITLE_PAL + 3] = copperMove(color03, bg);
}

void StandbyCopperList::setSpritePostColor(uint16_t c)
{
    data_[INDEX_SPRITE_COL + 1] = copperMove(color17, c);
}

void StandbyCopperList::setSprite2(const Sprite& s)
{
    showSprite(INDEX_SPRITES + 4, 2, s);
}

void StandbyCopperList::setEnergyIndicatorColor(uint16_t c)
{
    data_[INDEX_ENERGY_COL] = copperMove(color21, c);   // sprite pair 1 pen 01 (the gauge bar)
}

void StandbyCopperList::setCompassColor(uint16_t c)
{
    data_[INDEX_COMPASS_COL] = copperMove(color01, c);
}

void StandbyCopperList::setTerrainRuns(const Bitmap& b, const uint8_t* startScan,
                                       const uint16_t* startRow, int count)
{
    if (count < 1) count = 1;
    if (count > MAX_TERRAIN_RUNS) count = MAX_TERRAIN_RUNS;
    // Run 0 always begins at the region-entry WAIT (INDEX_TERRAIN_WAIT = kTerrainLine-1); only its
    // 6 BPLxPT words are per-run.  showBitmap(idx, b, .., yOffset=row, 3) writes b.data + row*120
    // (+{0,40,80}); the interleaved 80-modulo then advances one row/scanline within the run.
    showBitmap(INDEX_TERRAIN_BPL0, b, 1, 1, 0, (int16_t)startRow[0], 3);
    // Runs 1..count-1: each = WAIT(scanline) + 6 BPLxPT.
    uint32_t idx = INDEX_TERRAIN_RUNS;
    for (int i = 1; i < count; i++) {
        data_[idx++] = copperWait(kTerrainLine + startScan[i] - 1, 0xE0);
        showBitmap(idx, b, 1, 1, 0, (int16_t)startRow[i], 3);
        idx += 6;
    }
    // Cockpit region immediately after the last run — no padding, so zero churn into the cockpit.
    emitCockpitRegion(idx);
}

void StandbyCopperList::setTerrainPalette(uint16_t p0, uint16_t p1, uint16_t p2, uint16_t p3)
{
    data_[INDEX_TERRAIN_PAL + 0] = copperMove(color00, p0);
    data_[INDEX_TERRAIN_PAL + 1] = copperMove(color01, p1);
    data_[INDEX_TERRAIN_PAL + 2] = copperMove(color02, p2);
    data_[INDEX_TERRAIN_PAL + 3] = copperMove(color03, p3);
}

void StandbyCopperList::setTerrainBgColor(uint16_t c)
{
    data_[INDEX_TERRAIN_PAL + 3] = copperMove(color03, c);
}
