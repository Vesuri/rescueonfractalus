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
  #include <proto/exec.h>
  #include <dos/dosextens.h>          /* struct Process — pr_CLI, pr_MsgPort */
  #include <workbench/startup.h>      /* struct WBStartup */
#else
  #include "platform/sdl/PlatformSDL.h"
#endif

/* Native (rof_native.c) one-shot builder for the 64KB mul_u8 lookup table. */
extern "C" void rof_mul_table_init(void);
/* ...and for the 8KB terr_blend nibble tables (sample_terrain_height_bilerp). */
extern "C" void rof_blend_table_init(void);

#ifdef ROF_SFXMIX_FUZZ
/* One-shot on-target fuzz of the hand-asm SFX mixer twin (rof_native.c, `make FUZZ=1`). */
extern "C" void sfx_mixer_fuzz(unsigned long cases);
#endif

#if defined(ROF_PLATFORM_AMIGA)
/* --- Workbench launch protocol -------------------------------------------------------
   A program started from an icon is a NEW DOS process, and Workbench posts it a WBStartup
   message.  We have to take that message off our port, and — as the very last thing the
   program does — reply it: the reply is what tells Workbench the launch is over and that our
   Process may be terminated and its code unloaded.  A compiler's standard startup module
   would do both; this port's freestanding CRT (_start) does neither, so the two halves live
   here, wrapped around everything else main() does.

   Documented in the ROM Kernel Reference Manual: Libraries, chapter 14 — node 14-2-2
   (WBStartup Message), 14-5-1 (Workbench Startup) and the "few words of warning for those of
   you who do not use standard startup code" at the end of 14-5-2.  Commodore's own reference
   implementation is the RKM startup module's fromWorkbench / getWbMsg / repWbMsg. */

/* Take the startup message, or 0 when we were not launched from an icon.  pr_CLI is non-NULL
   for a Shell launch — and for the WHDLoad slave, which reaches the game through a synthesised
   startup-sequence as a real CLI process — and no message exists in that case.
   This MUST run before any dos.library call: a Process's message port belongs exclusively to
   DOS, so leaving a foreign message sitting on it corrupts DOS's own message traffic. */
static struct WBStartup* wbGetStartupMessage(void) {
    struct Process* me = (struct Process*)FindTask(0);
    if (me->pr_CLI != 0) return 0;
    WaitPort(&me->pr_MsgPort);
    return (struct WBStartup*)GetMsg(&me->pr_MsgPort);
}

/* Reply it.  Forbid() first, and never Permit(): the instant the message is replied Workbench
   is free to UnLoadSeg() the very code we are still returning through, and forbidding task
   switching holds it off until we have actually exited (the forbid nesting is released when
   the task goes away).  Nothing may follow this call — which is why runGame() below is a
   separate function, so the platform destructor has already run by the time we get here.
   We deliberately never touch the WBArg locks: they belong to Workbench, which unlocks them
   on this reply. */
static void wbReplyStartupMessage(struct WBStartup* msg) {
    if (!msg) return;
    Forbid();
    ReplyMsg(&msg->sm_Message);
}
#endif

/* Brings up the platform and runs the game.  A function of its own rather than main()'s body
   so that on the Amiga the PlatformClass destructor has already run before main() replies the
   Workbench startup message — nothing may happen after that reply. */
static int runGame(const char* image) {

    /* Constructing PlatformClass brings up the platform (window/DMA/audio, loads
       the memory image) and sets the global Platform* pointer the C bridge uses. */
    PlatformClass plt(image);
    if (plt.quit) return 1;

    /* Build the 64KB mul_u8 lookup table ONCE, up front — before any game code runs.
       Otherwise it is built lazily on the first flight VBI ISR firing, a ~3.6s (7MHz 68000)
       stall that freezes the display right at flight entry.  See rof_mul_table_init(). */
    rof_mul_table_init();

    /* Same idea, 8KB: the terr_blend nibble tables that replace sample_terrain_height_bilerp's
       three bit-serial blend loops.  Cheap to build, but not inside the flight VBI. */
    rof_blend_table_init();

#ifdef ROF_SFXMIX_FUZZ
    /* One-shot on-target fuzz of the hand-asm SFX mixer twin vs its C oracle over randomised
       mem[] state, covering the paths flight rarely reaches (sfxmix_verify only sees the ones it
       does, and `make validate` can only exercise the C twin on the host).  Restores all state
       it touches; read the tallies with amiga/sfxmix_fuzz.gdb.  `make FUZZ=1` only. */
    sfx_mixer_fuzz(20000uL);
#endif

    plt.run();   /* runs the game; returns when the user quits */
    return 0;
}

/* Default to the pristine rof.xex so every build boots the SAME initial state and
   game_entry code path.  Pass a path to boot a different image where the platform
   supports it (SDL: a flat 64 KB .bin; Amiga ignores it — the image is embedded).
   NOTE: the Amiga freestanding CRT (_start) calls main() with NO arguments, so the
   Amiga main takes none (a mismatched signature reads garbage off the stack). */
#if defined(ROF_PLATFORM_AMIGA)
int main(void) {
    /* Before runGame(), because the platform it constructs opens dos.library. */
    struct WBStartup* wbMsg = wbGetStartupMessage();
    int rc = runGame("rof.xex");
    wbReplyStartupMessage(wbMsg);   /* strictly last — Workbench may unload us right after */
    return rc;
}
#else
int main(int argc, char* argv[]) {
    return runGame((argc > 1) ? argv[1] : "rof.xex");
}
#endif
