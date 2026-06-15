/* Rescue on Fractalus! — SDL entry point.
   Follows the PETSCIIRobots-SDL pattern: instantiate PlatformClass, register
   platform-specific handlers, then hand control to the game. */
#include "platform/PlatformSDL.h"
#include <csignal>
#include <cstdlib>

extern "C" {
    void rof_register_vbi_handlers(void);
    void game_entry(void);
}

/* SDL may swallow SIGINT on macOS; reinstall a plain exit handler so
   Ctrl-C always works, even when the game is stuck in a spin-wait loop
   before the SDL event loop gets a chance to run.                     */
static void handleSigInt(int) { exit(0); }

int main(int argc, char* argv[]) {
    signal(SIGINT, handleSigInt);   /* ensure Ctrl-C works before SDL events run */

    /* Default to the pristine rof.xex so the SDL build boots the SAME initial state
       and game_entry code path as the Amiga (load_xex_image).  Pass a ".bin" path to
       boot a flat 64 KB RAM snapshot instead (e.g. disasm/rof_mem.bin). */
    const char* image = (argc > 1) ? argv[1] : "rof.xex";

    /* Creating PlatformSDL initialises SDL, loads the memory image,
       starts the audio device, and sets the global Platform* pointer. */
    PlatformClass plt(image);

    if (plt.quit) {
        return 1;
    }

    /* SDL_Init (in the PlatformClass ctor above) reinstalls its own SIGINT/SIGTERM
       handlers, clobbering the one set before main()'s SDL setup and turning Ctrl-C
       into an SDL_QUIT event that the spin-wait-driven boot never gets to act on.
       Re-arm our plain exit handler AFTER SDL is up so Ctrl-C always kills the app. */
    signal(SIGINT,  handleSigInt);
    signal(SIGTERM, handleSigInt);

    /* Populate the VBI address → C-function dispatch table so the audio
       callback can fire the right handler when the game installs it. */
    rof_register_vbi_handlers();

    /* Run the game — this loops forever (or until ESC / window close). */
    game_entry();

    return 0;
}
