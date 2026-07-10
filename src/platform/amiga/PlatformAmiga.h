#pragma once
/* PlatformAmiga — the Amiga concrete implementation of the abstract Platform.
 *
 * Owns everything Amiga-hardware-specific below the RescueOnFractalus scene: the
 * display takeover (LoadView/DMACON/display window), the real INTB_VERTB VBI server,
 * the CIA-B Timer A music tick, the CIA-A serial-port keyboard (RETURN -> Atari START),
 * the POKEY->Paula audio backend, and the launch-cinematic frame pump.
 *
 * It implements the Platform interface the C-compiled 6502 transliteration reaches
 * through the shared platform_cbridge.cpp (hwRead/hwWrite/renderFrame/tickVBI/...).
 */
#include "Platform.h"           // the abstract base (src/platform, on the build -I path)
#include "platform_c.h"         // the extern "C" bridge decls (platform_hw_write etc.)
#include "framework/Util.h"  // uint8_t, uint16_t, uint32_t

// PETSCIIRobots-SDL convention: main.cpp instantiates PlatformClass(image) without
// knowing the concrete type; the build define selects which header is included.
#define PlatformClass PlatformAmiga

class RescueOnFractalus;

class PlatformAmiga : public Platform {
public:
    explicit PlatformAmiga(const char* imagePath);   // imagePath ignored (image is embedded)
    virtual ~PlatformAmiga();

    // run(): the whole Amiga lifecycle.  Takes over the display, installs the VBI
    // server + CIA-B music tick + CIA-A keyboard, loads the boot image, runs the
    // RescueOnFractalus scene (the genuine transpiled/native boot chain under the
    // launch frame pump), then restores the system.  Returns when the user quits.
    virtual void run() override;

    // Platform bus + frame interface (reached via platform_cbridge.cpp).
    virtual uint8_t hwRead(uint16_t addr)            override;
    virtual void    hwWrite(uint16_t addr, uint8_t val) override;
    virtual void    renderFrame()                    override;   // render + wait for next VBI
    virtual void    pollEvents()                     override;   // poll quit (left mouse)
    virtual void    tickVBI()                        override;   // no-op (ISR owns RTCLOK)
    virtual void    tunnelRingsDrawn()               override;   // flag the $1000 ring field dirty
    virtual void    titleChanged()                   override;   // flag the $32B7 title region dirty
    virtual void    compassChanged()                 override;   // flag the $32E3 compass cells dirty
    virtual void    lockonChanged()                  override;   // flag the $3491 lock-on cells dirty
    virtual void    cockpitDirty(uint16_t addr, uint8_t nCells) override;  // register a changed cockpit cell span
    virtual void    titleScreenDirty(uint16_t addr, uint8_t nCells) override;  // grow the Title Screen value-cell dirty range
    virtual uint8_t flightIrqKey()                   override;   // consume a pending in-flight command key
    virtual int     loadImage(const char* path)      override;   // image is embedded -> no-op
    virtual void    setInterrupt(void (*fn)(void))   override;   // Amiga uses the real VBI -> no-op
    virtual int     framesPerSecond()                override;   // 50 (PAL)

    // POKEY->Paula audio backend (static — no instance state; the 6502-converted
    // station_audio writes POKEY registers via hwWrite, these route them to Paula DMA).
    //   audioInit/audioShutdown : bracket Paula audio DMA (called by RescueOnFractalus).
    //   pokeyRandom             : advance + return one POKEY LFSR byte (attract/noise/stars).
    //   noiseTick               : per-VBI refresh of the continuous poly17 noise sample.
    static void audioInit();
    static void audioShutdown();
    static uint8_t pokeyRandom();
    static void noiseTick();
    static void flightShotTick();   // build the player laser sprite from the VBI (50Hz), via s_scene
};

