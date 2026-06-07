// Rescue on Fractalus — Amiga native attract screen
// PETSCII-Robots-style skeleton: own main() + exec AddIntServer VBI + while(!quit) loop.
// Takeover approach: LoadView(NULL) + own copper list; exec interrupt system untouched.
// See amiga/ARCH.md for architecture notes.

#define ECS_SPECIFIC
#include <proto/exec.h>
#include <proto/graphics.h>
#include <exec/interrupts.h>
#include <exec/nodes.h>
#include <exec/memory.h>
#include <graphics/gfxbase.h>
#include <graphics/view.h>
#include <hardware/dmabits.h>
#include <hardware/intbits.h>

#include "../framework/AmigaHardware.h"
#include "AttractScene.h"

// GfxBase defined in GCCRuntime.cpp; set after OpenLibrary.
extern struct GfxBase* GfxBase;

// ---- VBI interrupt server ---------------------------------------------------
// Runs at interrupt level: only increments a counter. Returns 0 so exec
// continues the server chain (letting other VBI servers — CIA timer etc — run).
static volatile uint16_t vbiCount = 0;
static struct Interrupt vbiServer;

static uint32_t vbiHandler()
{
    vbiCount++;
    return 0;
}

// ---- main -------------------------------------------------------------------
int main()
{
    GfxBase = (struct GfxBase*)OpenLibrary((UBYTE*)"graphics.library", 33);
    if (!GfxBase) return 1;

    // --- takeover: save system state, disable OS display ---------------------
    struct View* savedView = GfxBase->ActiView;
    LoadView(NULL);
    WaitTOF();
    WaitTOF();

    // Disable raster (bitplane) and sprite DMA so old state doesn't leak through.
    // Keep exec's disk/blitter/audio DMA as-is; copper DMA gets re-enabled below.
    *dmaconPointer = (uint16_t)(DMAF_RASTER | DMAF_SPRITE | DMAF_COPPER);

    // Display window — standard PAL lores 320×200 visible area.
    // No bitplanes (bplcon0=0): entire area shows COLOR00 (copper-set background).
    *diwstrtPointer = 0x2c81;   // VSTRT=44, HSTRT=0x81
    *diwstopPointer = 0xf4c1;   // VSTOP=244, HSTOP=0xc1 (+256 implicit)
    *ddfstrtPointer = 0x0038;
    *ddfstopPointer = 0x00d0;
    *bplcon0Pointer = 0x0000;   // 0 bitplanes
    *bplcon1Pointer = 0x0000;
    *bplcon2Pointer = 0x0000;

    // --- VBI interrupt server ------------------------------------------------
    vbiServer.is_Node.ln_Type = NT_INTERRUPT;
    vbiServer.is_Node.ln_Pri  = 0;
    vbiServer.is_Node.ln_Name = (char*)"RoF VBI";
    vbiServer.is_Data = NULL;
    vbiServer.is_Code = (void(*)())vbiHandler;
    AddIntServer(INTB_VERTB, &vbiServer);

    // --- attract scene -------------------------------------------------------
    // Enable copper DMA, then let AttractScene install its list.
    *dmaconPointer = (uint16_t)(DMAF_SETCLR | DMAF_MASTER | DMAF_COPPER);

    AttractScene scene;
    scene.initialize();

    // --- main loop -----------------------------------------------------------
    uint16_t frame    = 0;
    uint16_t lastVBI  = vbiCount;
    bool     quit     = false;

    while (!quit) {
        // Wait for next VBI tick (volatile spin — -O2 safe).
        while (vbiCount == lastVBI) { /* wait */ }
        lastVBI = vbiCount;
        frame++;

        // Input: left mouse button quits.
        if (AmigaHardware::isLeftMouseButtonPressed())
            quit = true;

        scene.update(frame);
        scene.render();
    }

    scene.shutdown();

    // --- restore system ------------------------------------------------------
    RemIntServer(INTB_VERTB, &vbiServer);

    // Disable our display DMA before handing back.
    *dmaconPointer = (uint16_t)(DMAF_COPPER | DMAF_RASTER | DMAF_SPRITE);

    LoadView(savedView);
    WaitTOF();
    WaitTOF();

    CloseLibrary((struct Library*)GfxBase);
    GfxBase = 0;

    return 0;
}
