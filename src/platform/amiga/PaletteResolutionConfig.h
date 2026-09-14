#pragma once

// Retained startup configuration in StartupConfig.s.  Loaders may patch this word before
// entering the executable; the renderer snapshots it once during initialize().
extern "C" unsigned short rof_enhanced_palette_value;

static inline bool configuredEnhancedPalette()
{
    return rof_enhanced_palette_value != 0;
}
