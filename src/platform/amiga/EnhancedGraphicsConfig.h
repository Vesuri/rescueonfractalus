#pragma once

// Retained startup configuration in StartupConfig.s. Loaders may patch this word before
// entering the executable; the renderer snapshots it once during initialize(). Custom4 uses
// the same setting for the enhanced Lucasfilm logo and the enhanced cockpit renderer.
extern "C" unsigned short rof_enhanced_graphics_value;

#ifdef ROF_ENHANCED_GRAPHICS
// Enhanced Graphics compiled in: the retained word (loader-patchable via Custom4) is snapshotted
// into this mutable global once during initialize().
extern "C" int g_enhancedGraphics;

static inline bool configuredEnhancedGraphics()
{
    return rof_enhanced_graphics_value != 0;
}
#else
// Enhanced Graphics NOT built: a compile-time constant 0 so -O2 dead-code-eliminates every
// enhanced arm and --gc-sections drops the enhanced-only routines, the planar cockpit atlas
// and its startup builder.  The retained word still exists (StartupConfig.s), but this binary
// carries no code that acts on it.
enum : int { g_enhancedGraphics = 0 };
#endif
