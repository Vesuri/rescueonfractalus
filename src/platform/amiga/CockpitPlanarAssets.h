#pragma once
// Generated at startup from cartridge-sourced glyphs. Entries are scan-major Amiga plane
// bytes, ready to copy to an interleaved Bitmap; rendering performs no per-pixel expansion.
extern "C" unsigned char rof_cockpit_planar[];
extern "C" unsigned char rof_cockpit_planar_end[];

enum CockpitPlanarAssetOffset {
    kCockpitPlanarHeaderOffset   = 0,
    kCockpitPlanarModeDOffset    = 32,       // 256 x (P1,P2)
    kCockpitPlanarMode4Offset    = 544,      // 256 x 8 x (P1,P2,P3)
    kCockpitPlanarCompassOffset  = 6688,     // 128 x 8 x (P1,P2)
    kCockpitPlanarTextMaskOffset = 8736,     // 64 x 8 x (high byte, low byte)
    kCockpitPlanarNormalMode4Offset = 9760,  // 256 x 8 x (P1,P2,P3,P4), normal playfield
    kCockpitPlanarAssetSize      = 17952
};

static inline const unsigned char* cockpitModeDPlanar(unsigned char source)
{
    return rof_cockpit_planar + kCockpitPlanarModeDOffset + ((unsigned short)source << 1);
}

static inline const unsigned char* cockpitMode4Planar(unsigned char character)
{
    const unsigned short index = (unsigned short)character;
    return rof_cockpit_planar + kCockpitPlanarMode4Offset + (index << 4) + (index << 3);
}

static inline const unsigned char* cockpitCompassPlanar(unsigned char character)
{
    return rof_cockpit_planar + kCockpitPlanarCompassOffset + ((unsigned short)(character & 0x7Fu) << 4);
}

static inline const unsigned char* cockpitTextMaskPlanar(unsigned char character)
{
    return rof_cockpit_planar + kCockpitPlanarTextMaskOffset + ((unsigned short)(character & 0x3Fu) << 4);
}

static inline const unsigned char* cockpitMode4NormalPlanar(unsigned char character)
{
    const unsigned short index = (unsigned short)character;
    return rof_cockpit_planar + kCockpitPlanarNormalMode4Offset + (index << 5);
}
