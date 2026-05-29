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

    const char* image = (argc > 1) ? argv[1] : "disasm/rof_mem.bin";

    /* Creating PlatformSDL initialises SDL, loads the memory image,
       starts the audio device, and sets the global Platform* pointer. */
    PlatformClass plt(image);

    if (plt.quit) {
        return 1;
    }

    /* Populate the VBI address → C-function dispatch table so the audio
       callback can fire the right handler when the game installs it. */
    rof_register_vbi_handlers();

    /* Run the game — this loops forever (or until ESC / window close). */
    game_entry();

    return 0;
}
