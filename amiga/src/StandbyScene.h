#pragma once
#include "../framework/CopperList.h"
#include "../framework/Bitmap.h"
#include "../framework/Palette.h"
#include "../framework/Sprite.h"

// 2-bitplane attract screen: one BPLCON0 mode for the whole frame, Copper
// switches the 4-colour palette (and bitmap pointer) at each region boundary,
// mirroring the Atari DLI schedule exactly.
//
// Regions:
//   Title   (lines 0..kTerrainLine-1) : title.raw  + kTitlePalette
//   Terrain (lines kTerrainLine..kCockpitLine-1) : terrain.raw + kTerrainPalette
//   Cockpit (lines kCockpitLine..end) : cockpit.raw + kCockpitPalette (+ blink)
class StandbyScene {
public:
    void initialize();
    void update(uint16_t frame);
    void render();
    void shutdown();

    // Launch cinematic: start the doors-open transition (Atari START press).
    // The terrain image (green/dots/LEVEL 04 = the "closed doors") splits from
    // the middle, halves sliding apart, revealing the tunnel in the gap.
    void openDoors();
private:
    enum class Phase { Standby, DoorsOpening };
    Phase    phase   = Phase::Standby;
    uint16_t doorGap = 0;   // middle gap height in rows (0 = closed)

    // Tunnel reveal: a 3bp concentric-rectangle bitmap shown in the door gap.
    // Motion is palette cycling — the 6-entry ring (Atari $08D4-$08D9, feeding
    // COLOR01-06) rotates one slot per tick (Atari +$75/frame accumulator).
    Bitmap*  tunnelBitmap = nullptr;
    uint8_t  ring[6]      = { 0x30, 0x32, 0x34, 0x36, 0x38, 0x3A };  // seed = $6E68 ramp
    uint16_t ringAcc      = 0;   // fixed-point cycle-rate accumulator

    void buildCopperList(CopperList* cl, uint16_t frame);
    void fillSpriteData(Sprite* s, bool isRight);

    CopperList* copperLists[2] = { nullptr, nullptr };
    Bitmap*     titleBitmap    = nullptr;
    Bitmap*     terrainBitmap  = nullptr;
    Bitmap*     cockpitBitmap  = nullptr;
    Palette*    palette        = nullptr;  // drives title region + global fade
    Sprite*     leftPost       = nullptr;
    Sprite*     rightPost      = nullptr;
    Sprite*     nullSprite     = nullptr;
    uint8_t     active         = 0;

    // Dirty-flag bitmap caching: bitmaps are rendered once on initialize() and
    // only re-rendered when the underlying mem[] data changes.
    bool    terrainDirty = true;   // re-render terrain rows from $2000
    uint8_t titleShadow[20] = {};  // shadow of last-rendered $32B7-$32CA

    // Per-cell shadow caching for the cockpit (mirrors titleShadow): a single
    // changed Atari source byte re-decodes only that cell — modeD: 2 rows × 3
    // planes = 6 writes; mode4: 8 scanlines × 3 planes = 24 writes — never the
    // whole 88-row region.  cockpitForceFull makes the first render() populate
    // every cell regardless of shadow contents.
    bool    cockpitForceFull = true;
    uint8_t cockpitModeDShadow[4 * 40] = {};   // shadow of modeD source ($350D region)
    uint8_t cockpitMode4Shadow[10 * 40] = {};  // shadow of mode4 source ($332D region)
};
