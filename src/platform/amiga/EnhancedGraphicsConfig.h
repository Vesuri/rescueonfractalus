#pragma once

// Retained startup configuration in StartupConfig.s. Loaders may patch this word before
// entering the executable; the renderer snapshots it once during initialize(). Custom4 uses
// the same setting for the enhanced Lucasfilm logo and the enhanced cockpit renderer.
extern "C" unsigned short rof_enhanced_graphics_value;
extern "C" int g_enhancedGraphics;

static inline bool configuredEnhancedGraphics()
{
    return rof_enhanced_graphics_value != 0;
}
