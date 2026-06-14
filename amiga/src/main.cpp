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
#include "RescueOnFractalus.h"
#include "Keyboard.h"

// GfxBase defined in GCCRuntime.cpp; set after OpenLibrary.
extern struct GfxBase* GfxBase;

// mem[] lives in audio/rof_gen.c; also written by the VBI handler.
extern "C" volatile uint8_t mem[65536];

// sfx_voice_tick_native: driven by CIA-B Timer A (see below), not the main loop.
extern "C" void sfx_voice_tick_native(void);

// Native port of station_init's CONSOL ($D01F) read: true while START is down.
extern "C" bool station_poll_start_native(void);

// game_vbi_isr: the per-frame VBI body, run from the real vertical-blank interrupt
// below.  Dispatches by phase (g_activeVbi) to the standby ($52D7) or flight ($4FF5)
// VBI body, exactly as the Atari swaps VVBLKI, and saves/restores the shared `cpu`.
extern "C" void game_vbi_isr(void);

// Set by the keyboard ISR on the 'F' key-down edge: skip to the flight stage.
extern "C" volatile uint8_t g_skipToFlight;

// Set while the launch cinematic runs a transpiled blocking frame-wait; gates the
// VBI ISR's RTCLOK bump off (platform_tick_vbi advances it in lockstep instead).
extern "C" volatile uint8_t g_launchBlocking;

// ---- VBI interrupt server ---------------------------------------------------
// Mirrors the Atari RTCLOK increment from vbi_handler_1 ($53CC).
// DLIST/colour writes are handled by the Copper on the Amiga side.
// g_vbiCount: bumped once per REAL vertical-blank interrupt (vbiHandler below).
// RescueOnFractalus::frameStep() spins on it as the matching Amiga construct for
// the Atari's frame-wait busy-loops (wait_frames_2/5/10/60) — the real VBI is the
// frame clock, exactly as it was on the Atari.  Non-static so the scene can read it.
extern "C" volatile uint16_t g_vbiCount = 0;
static struct Interrupt vbiServer;

// --- launch-cinematic blocking frame pump ------------------------------------
// Installed into PaulaAudio's blocking-wait hooks (rof_set_frame_pump): while
// rof_launch_blocking(1) is set, the transpiled launch/audio code's frame-waits
// call this once per spin iteration.  It waits one REAL VBI (so the ISR advances
// RTCLOK and the native door/tunnel visuals animate), repaints, and polls quit —
// letting the genuine 6502 audio code (e.g. the doors' audf2_sweep_clear_colors)
// run at the original frame cadence without freezing the screen.
extern "C" void rof_set_frame_pump(void (*fn)(void));
extern "C" void rof_launch_blocking(uint8_t on);
static class RescueOnFractalus* g_scenePtr = 0;
static volatile bool            g_pumpQuit = false;
static void launchFramePump(void)
{
    uint16_t last = g_vbiCount;
    while (g_vbiCount == last) { /* wait for next real VBI */ }
    if (g_scenePtr) g_scenePtr->render();
    if (AmigaHardware::isLeftMouseButtonPressed()) g_pumpQuit = true;
}

static uint32_t vbiHandler()
{
    // Mirror vbi_handler_1 ($53CC): bump RTCLOK ($0014 low, carry into $0013).
    // (Do NOT touch $0080 — that is sync_flag, which the 6502 drawing/scroll
    // routines reuse as the $80/$81 zero-page pointer pair; incrementing it here
    // each frame corrupted mid-draw pointer writes, e.g. dropped tunnel pixels.)
    // While the launch cinematic blocks on a transpiled frame-wait, RTCLOK is
    // advanced synchronously by platform_tick_vbi (in lockstep with the wait loop)
    // — bumping it here too would desync/hang the loop.  vbiCount still ticks so the
    // frame pump can pace one repaint per real VBI.
    if (!g_launchBlocking) {
        mem[0x0014]++;               // RTCLOK_LOW
        if (!mem[0x0014]) mem[0x0013]++;  // RTCLOK_MID carry
    }
    g_vbiCount++;

    // Per-frame VBI body — run in the REAL vertical-blank interrupt, where the Atari
    // ran its VBI (not from the main loop).  game_vbi_isr() dispatches by phase
    // (g_activeVbi) to the standby ($52D7) or flight ($4FF5) body — as the Atari swaps
    // VVBLKI — and brackets the work in a save/restore of the shared 6502 register
    // file, mirroring the Atari OS VBLANK's hardware register save/restore (the main
    // loop may be mid-instruction using `cpu` when this interrupt preempts it).
    game_vbi_isr();
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
    // Enable copper + raster + sprite DMA, then let RescueOnFractalus install its list.
    *dmaconPointer = (uint16_t)(DMAF_SETCLR | DMAF_MASTER | DMAF_COPPER | DMAF_RASTER | DMAF_SPRITE);

    // static (BSS), NOT a stack local: the scene holds several KB of shadow buffers
    // (tunnel/viewport/cockpit per-byte caches), which would overflow the program stack.
    static RescueOnFractalus scene;
    scene.initialize();

    // RETURN = START button for the launch cinematic (CIA-A SP keyboard).
    Keyboard keyboard;
    keyboard.initialize();

    // Install the launch-cinematic frame pump so the transpiled audio code's
    // blocking frame-waits drive a real one-VBI repaint (see launchFramePump).
    g_scenePtr = &scene;
    rof_set_frame_pump(&launchFramePump);

    // --- main loop -----------------------------------------------------------
    // The whole game is a faithful straight-line transcription of the Atari
    // control flow inside RescueOnFractalus::run(): station_init's attract loop
    // (wait for START), display_setup's launch cinematic, then game_entry's
    // flight loop — each original busy-wait backed by the REAL INTB_VERTB VBI
    // (frameStep spins on g_vbiCount).  No per-frame state-machine dispatch here.
    // run() returns when the user quits (left mouse button); START / F-key are
    // polled inside its frame-waits via station_poll_start_native / g_skipToFlight.
    scene.run();

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
