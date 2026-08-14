#ifndef PLATFORM_H
#define PLATFORM_H

#if !defined(ROF_PLATFORM_AMIGA)
#include <cstdint>   /* SDL/host build.  The Amiga build has no libstdc++ and gets the
                        integer types from the force-included framework/SASCCompat.h. */
#endif

/* Abstract platform base.  Each supported target (SDL, Amiga) provides a concrete subclass.
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
       the interval elapses.                                             */
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

    /* Notification that boot_standby_launch_driver is ABOUT TO draw the launch tunnel rings
       into the $1000 GTIA field.  A platform that mirrors mem[] into its own framebuffer
       (the Amiga copper/bitplane backend) claims + primes its ring bitmap here, then paints
       each span as the draw emits it.  No-op on platforms that render mem[] directly (SDL). */
    virtual void tunnelRingsBegin() {}

    /* Notification that the genuine $782A title writer (copy_altitude_graphic_to_
       screen) just rewrote the Standby banner text in screen RAM $32B7-$32CA (the
       SFX sequencer alternates "RESCUE ON FRACTALUS!" / copyright via $0091).  A
       platform mirroring mem[] into bitplanes flags the title region for re-scan
       here.  No-op on platforms that render mem[] directly (SDL).          */
    virtual void titleChanged() {}

    /* Notification that the compass heading cells ($32E3-$32E6, the mode-4 line
       below the title) were rewritten — the housing ($01) by game_sub_4606 or the
       heading glyph by the compass updater ($3FDE).  A bitplane-mirroring platform
       re-decodes just those 4 cells.  No-op on direct-mem[] platforms (SDL).     */
    virtual void compassChanged() {}

    /* Notification that ONE enemy lock-on indicator cell (#11, $3491 + cellIdx, cellIdx
       0..6) was rewritten by the lock-on animation.  A bitplane-mirroring platform flags
       just that cell for re-decode; no-op on direct-mem[] platforms (SDL / headless).
       The random blink rewrites a SINGLE cell ~9x/second, so decoding the whole 7-cell
       strip on each write was ~6x the necessary work (measured).            */
    virtual void lockonChanged(uint8_t /*cellIdx*/) {}

    /* Notification that an instrument writer changed a span of nCells cockpit cells
       starting at Atari screen-RAM address `addr`.  A bitplane-mirroring platform records
       the span for a targeted decode; no-op on direct-mem[] platforms (SDL / headless).  */
    virtual void cockpitDirty(uint16_t /*addr*/, uint8_t /*nCells*/) {}

    /* Notification that a span of nCells Title Screen (scene 3b) digit cells was written
       starting at Atari screen-RAM address `addr` — the STARTING LEVEL digit (level select)
       or a LAST/HIGH SCORE digit (game-over build).  A bitplane-mirroring platform grows
       its value-cell dirty range so render() redraws only those cells (no full-screen
       repaint / flash).  No-op on direct-mem[] platforms (SDL / headless).            */
    virtual void titleScreenDirty(uint16_t /*addr*/, uint8_t /*nCells*/) {}

    /* Poll+consume a pending in-flight keyboard-command keycode (Atari KBCODE&$3F,
       or $80 for BREAK); $FF = none pending.  The flight VBI's CLI window ($519c)
       calls this to feed event_sequence_dispatcher ($4644), replacing the POKEY
       keyboard IRQ that left the id in X on the Atari.  Default: no in-flight key
       delivery (SDL / headless) — always $FF, so the dispatch stays skipped.    */
    virtual uint8_t flightIrqKey() { return 0xFF; }

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

protected:
    /* POKEY 17-bit LFSR state — concrete classes call pokeyRandomStep().  */
    uint32_t rngState;
    uint8_t  pokeyRandomStep();
};

extern Platform* platform;

#endif /* PLATFORM_H */
