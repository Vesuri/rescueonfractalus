// Rescue on Fractalus — Amiga entry point.
//
// main() is deliberately thin: it opens graphics.library, constructs the scene, and
// hands control to PlatformAmiga::run(), which owns the entire Amiga platform —
// display takeover, the real INTB_VERTB VBI server, the CIA-B Timer A music tick, the
// CIA-A keyboard, the POKEY->Paula audio backend, and the launch-cinematic frame pump.
// See PlatformAmiga.cpp and amiga/ARCH.md.

#define ECS_SPECIFIC
#include <proto/exec.h>
#include <graphics/gfxbase.h>

#include "PlatformAmiga.h"
#include "RescueOnFractalus.h"

// GfxBase defined in GCCRuntime.cpp; PlatformAmiga::run reads it for the display takeover.
extern struct GfxBase* GfxBase;

int main()
{
    GfxBase = (struct GfxBase*)OpenLibrary((UBYTE*)"graphics.library", 33);
    if (!GfxBase) return 1;

    // static (BSS), NOT a stack local: the scene holds several KB of shadow buffers
    // (tunnel/viewport/cockpit per-byte caches), which would overflow the program stack.
    static RescueOnFractalus scene;

    PlatformAmiga::run(scene);

    CloseLibrary((struct Library*)GfxBase);
    GfxBase = 0;
    return 0;
}
