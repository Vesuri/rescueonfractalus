// Rescue on Fractalus — Amiga native attract screen
// PETSCII-Robots-style skeleton: own main() + exec AddIntServer VBI + while(!quit) loop.
// Takeover approach: LoadView(NULL) + own copper list; exec interrupt system untouched.
// See amiga/ARCH.md for architecture notes.

#define ECS_SPECIFIC
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/cia.h>
#include <exec/interrupts.h>
#include <exec/nodes.h>
#include <exec/memory.h>
#include <graphics/gfxbase.h>
#include <graphics/view.h>
#include <hardware/dmabits.h>
#include <hardware/intbits.h>
#include <resources/cia.h>
#include <hardware/cia.h>

#include "../framework/AmigaHardware.h"
#include "StandbyScene.h"
#include "Keyboard.h"

// GfxBase defined in GCCRuntime.cpp; set after OpenLibrary.
extern struct GfxBase* GfxBase;

// mem[] lives in audio/rof_gen.c; also written by the VBI handler.
extern "C" volatile uint8_t mem[65536];

// sfx_voice_tick_native: driven by CIA-B Timer A (see below), not the main loop.
extern "C" void sfx_voice_tick_native(void);

// ---- VBI interrupt server ---------------------------------------------------
// Mirrors the Atari RTCLOK increment from vbi_handler_1 ($53CC).
// DLIST/colour writes are handled by the Copper on the Amiga side.
static volatile uint16_t vbiCount = 0;
static struct Interrupt vbiServer;

static uint32_t vbiHandler()
{
    mem[0x0080]++;               // RTCLOK[2] — primary frame tick
    mem[0x0014]++;               // RTCLOK[1] — secondary tick
    if (!mem[0x0014]) mem[0x0013]++;  // RTCLOK[0] — carry
    vbiCount++;
    return 0;
}

// ---- CIA-B Timer A interrupt — SFX music tick --------------------------------
// The Atari SFX sequencer ticks every other VBI (the BIT $062D gate, $00E7=1) =
// 25 Hz.  On the Amiga we drive it from CIA-B Timer A at that exact rate, off a
// dedicated hardware interrupt rather than the VBI/render loop, so the music
// tempo is independent of frame timing.  CIA-B uses the Amiga E-clock
// (≈ 709379 Hz on PAL); period = 709379/25 = 28375 (see below).
// The interrupt fires through INTB_EXTER (level 6) via the ciab.resource.
static struct Library    *CIABBase;
static struct Interrupt   sfxTimer;

static uint32_t sfxTimerHandler()
{
    if (mem[0x00E7]) sfx_voice_tick_native();
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

    // --- CIA-B Timer A — SFX music at 25 Hz ----------------------------------
    // Use ciab.resource so the CIA ICR is demultiplexed for us.
    CIABBase = (struct Library*)OpenResource((UBYTE*)CIABNAME);
    if (CIABBase) {
        sfxTimer.is_Node.ln_Type = NT_INTERRUPT;
        sfxTimer.is_Node.ln_Pri  = 0;
        sfxTimer.is_Node.ln_Name = (char*)"RoF SFX";
        sfxTimer.is_Data = NULL;
        sfxTimer.is_Code = (void(*)())sfxTimerHandler;
        if (!AddICRVector(CIABBase, CIAICRB_TA, &sfxTimer)) {
            Disable();
            // Stop timer, load period.
            // Atari fires sfx_voice_tick every other VBI (BIT $062D gate, $00E7=1) = 25 Hz.
            // 25 Hz on PAL E-clock (709379 Hz): period = 709379/25 = 28375 = 0x6EC7.
            *((volatile uint8_t*)(ciab + ciacra)) &= (uint8_t)~CIACRAF_START;
            *((volatile uint8_t*)(ciab + ciatalo)) = (uint8_t)(28375 & 0xFF);
            *((volatile uint8_t*)(ciab + ciatahi)) = (uint8_t)(28375 >> 8);
            // Continuous mode (RUNMODE=0); START=1 auto-loads latch into counter
            *((volatile uint8_t*)(ciab + ciacra)) =
                (uint8_t)((*((volatile uint8_t*)(ciab + ciacra))
                           & ~(CIACRAF_RUNMODE | CIACRAF_PBON | CIACRAF_OUTMODE
                               | CIACRAF_SPMODE | CIACRAF_TODIN))
                          | CIACRAF_START);
            Enable();
        }
    }

    // --- attract scene -------------------------------------------------------
    // Enable copper + raster + sprite DMA, then let StandbyScene install its list.
    *dmaconPointer = (uint16_t)(DMAF_SETCLR | DMAF_MASTER | DMAF_COPPER | DMAF_RASTER | DMAF_SPRITE);

    StandbyScene scene;
    scene.initialize();

    // RETURN = START button for the launch cinematic (CIA-A SP keyboard).
    Keyboard keyboard;
    keyboard.initialize();

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

        // RETURN (START) opens the doors → launch cinematic.
        if (keyboard.returnPressed())
            scene.openDoors();

        scene.update(frame);
        scene.render();
    }

    keyboard.shutdown();
    scene.shutdown();

    // --- restore system ------------------------------------------------------
    // Stop CIA-B Timer A and release ICR vector before removing VBI server.
    if (CIABBase) {
        Disable();
        *((volatile uint8_t*)(ciab + ciacra)) &= (uint8_t)~CIACRAF_START;
        Enable();
        RemICRVector(CIABBase, CIAICRB_TA, &sfxTimer);
    }

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
