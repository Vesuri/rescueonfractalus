/* Rescue on Fractalus! — shared entry point.
 *
 * One main() for every target: it constructs the concrete PlatformClass (selected
 * by a build define), then hands control to Platform::run(), which owns all the
 * platform-specific setup and drives the genuine boot chain (game_entry).
 *
 *   ROF_PLATFORM_AMIGA  -> PlatformAmiga  (src/platform/amiga, m68k cross-build)
 *   default             -> PlatformSDL    (macOS dev build)
 */
#if defined(ROF_PLATFORM_AMIGA)
  #include "PlatformAmiga.h"          /* src/platform/amiga — on the cross-build's -I path */
#else
  #include "platform/sdl/PlatformSDL.h"
#endif

/* Native (rof_native.c) one-shot builder for the 64KB mul_u8 lookup table. */
extern "C" void rof_mul_table_init(void);

/* Default to the pristine rof.xex so every build boots the SAME initial state and
   game_entry code path.  Pass a path to boot a different image where the platform
   supports it (SDL: a flat 64 KB .bin; Amiga ignores it — the image is embedded).
   NOTE: the Amiga freestanding CRT (_start) calls main() with NO arguments, so the
   Amiga main takes none (a mismatched signature reads garbage off the stack). */
#if defined(ROF_PLATFORM_AMIGA)
int main(void) {
    const char* image = "rof.xex";
#else
int main(int argc, char* argv[]) {
    const char* image = (argc > 1) ? argv[1] : "rof.xex";
#endif

    /* Constructing PlatformClass brings up the platform (window/DMA/audio, loads
       the memory image) and sets the global Platform* pointer the C bridge uses. */
    PlatformClass plt(image);
    if (plt.quit) return 1;

    /* Build the 64KB mul_u8 lookup table ONCE, up front — before any game code runs.
       Otherwise it is built lazily on the first flight VBI ISR firing, a ~3.6s (7MHz 68000)
       stall that freezes the display right at flight entry.  See rof_mul_table_init(). */
    rof_mul_table_init();

    plt.run();   /* runs the game; returns when the user quits */
    return 0;
}
