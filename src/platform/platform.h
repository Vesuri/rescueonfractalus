#ifndef PLATFORM_H
#define PLATFORM_H

#if !defined(ROF_PLATFORM_AMIGA)
#include <cstdint>   /* SDL/host build.  The Amiga build has no libstdc++ and gets the
                        integer types from the force-included framework/SASCCompat.h. */
#endif

/* Abstract platform base — mirrors the pattern from PETSCIIRobots-SDL/Platform.h.
   Each supported target (SDL, Amiga) provides a concrete subclass.
   The global singleton pointer is used by the C bridge layer so that the
   C-compiled 6502 transliteration can reach hardware emulation. */

class Platform {
public:
    Platform();
    virtual ~Platform();

    /* ------------------------------------------------------------------ */
    /* Lifecycle                                                            */
    /* ------------------------------------------------------------------ */

    /* Run the game.  Each platform sets up whatever it needs (interrupts,
       scene, signal handlers) and drives the genuine boot chain (game_entry).
       main() constructs the concrete PlatformClass and calls this; it returns
       when the user quits.  SDL: game_entry() directly (PlatformSDL renders
       mem[] each VBI).  Amiga: game_entry() inside the RescueOnFractalus scene
       under the launch frame pump, unwound on quit via setjmp/longjmp.       */
    virtual void run() = 0;

    /* ------------------------------------------------------------------ */
    /* Frame / interrupt                                                    */
    /* ------------------------------------------------------------------ */

    /* Register the VBI handler to be fired at framesPerSecond() rate.
       The audio callback advances a sample counter and invokes fn() when
       the interval elapses — same mechanism as PETSCIIRobots-SDL.        */
    virtual void setInterrupt(void (*fn)(void)) = 0;

    virtual int  framesPerSecond() = 0;

    /* Blit the offscreen buffer to the window and present.                */
    virtual void renderFrame() = 0;

    /* Pump the OS event loop without rendering — keeps the window
       manager from marking the app as unresponsive during spin-waits.  */
    virtual void pollEvents() {}

    /* Fire one VBI tick if the audio callback has accumulated enough samples.
       Call only from frame-boundary spin-waits, not scanline-position waits. */
    virtual void tickVBI() {}

    /* ------------------------------------------------------------------ */
    /* Hardware bus — called by bus_read / bus_write in bus.h             */
    /* ------------------------------------------------------------------ */

    /* Read a hardware-range address ($D000–$D7FF).  Default: 0x00.       */
    virtual uint8_t hwRead(uint16_t addr);

    /* Write a hardware-range address.  Default: no-op.                   */
    virtual void    hwWrite(uint16_t addr, uint8_t val);

    /* Notification that the game wrote an OS page-2 shadow register.
       ($0200–$02FF).  Used to track VVBLKI, VDSLST, etc.                 */
    virtual void    shadowWrite(uint16_t addr, uint8_t val);

    /* Register an Atari address → C function mapping for VBI/DLI dispatch.
       Implemented by concrete classes that support interrupt-driven VBI. */
    virtual void registerVBI(uint16_t /*addr*/, void (* /*fn*/)(void)) {}

    /* Runtime indirect JMP dispatch (DLI chain pattern: JMP ($E0)).     */
    virtual void indirectJmp(uint16_t addr) { (void)addr; }

    /* Notification that display_setup just drew the launch tunnel rings into the
       $1000 GTIA field.  A platform that mirrors mem[] into its own framebuffer
       (the Amiga copper/bitplane backend) converts them to bitplanes here.
       No-op on platforms that render mem[] directly (SDL).                */
    virtual void tunnelRingsDrawn() {}

    /* Notification that the genuine $782A title writer (copy_altitude_graphic_to_
       screen) just rewrote the Standby banner text in screen RAM $32B7-$32CA (the
       SFX sequencer alternates "RESCUE ON FRACTALUS!" / copyright via $0091).  A
       platform mirroring mem[] into bitplanes flags the title region for re-scan
       here.  No-op on platforms that render mem[] directly (SDL).          */
    virtual void titleChanged() {}

    /* ------------------------------------------------------------------ */
    /* Image loading                                                        */
    /* ------------------------------------------------------------------ */

    /* Load the 64K post-load memory image into mem[].  Returns 0 on OK.  */
    virtual int loadImage(const char* path) = 0;

    /* ------------------------------------------------------------------ */
    /* Shared state                                                         */
    /* ------------------------------------------------------------------ */
    bool quit;

    /* POKEY LFSR seed/peek — used by the native-reimplementation validation
       harness to give both runs an identical RANDOM ($D20A) stream.  Not used
       by the game itself.  17-bit state; 0 is a lockup value, avoid seeding it. */
    void     rngSeed(uint32_t s) { rngState = s; }
    uint32_t rngGet() const      { return rngState; }

protected:
    /* POKEY 17-bit LFSR state — concrete classes call pokeyRandomStep().  */
    uint32_t rngState;
    uint8_t  pokeyRandomStep();
};

extern Platform* platform;

#endif /* PLATFORM_H */
