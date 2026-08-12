/* rof_boot.h — the boot INITAD chain (Logo -> Station -> display init -> game).
 *
 * rof.xex is a 20-segment Atari load file whose LATER SEGMENTS OVERWRITE THE EARLIER
 * ONES, and it carries four INITAD ($02E2) vectors: the OS loader places segments until
 * one sets INITAD, calls through it, and carries on.  So the two boot scenes only ever
 * exist in RAM *between* stages — a single full load buries both (segment 16, $3CDE-$B7FF,
 * covers the logo code at $5000 and the station image staged at $4000).  rof_boot_chain()
 * reproduces that staged walk; see docs/logo-station-plan.md §3.
 *
 * Shared by both backends: PlatformSDL::run() and RescueOnFractalus::run() call it in
 * place of jumping straight to game_entry().
 */
#ifndef ROF_BOOT_H
#define ROF_BOOT_H

#if !defined(ROF_PLATFORM_AMIGA)
#include <stdint.h>   /* Amiga: the C core gets the types from cpu.h and the C++ TUs from
                         the force-included framework/SASCCompat.h, whose compat
                         <stdint.h> would clash — same rule as platform_c.h. */
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Which boot scene to START at.  Everything from that point on plays; earlier scenes are
 * skipped but their SEGMENTS are still placed, so memory is identical either way. */
enum {
    ROF_BOOT_LOGO    = 0,   /* Lucasfilm logo, then the station, then the game */
    ROF_BOOT_STATION = 1,   /* station cinematic only                          */
    ROF_BOOT_GAME    = 2    /* neither — place everything and return           */
};

/* Run the staged load + the boot scenes, leaving mem[] exactly as the Atari's loader
 * leaves it just before it calls INITAD $3CDE.  The CALLER then calls game_entry().
 * Blocks for the duration of whichever scenes play (~4-5 s Logo, ~4 s + idle Station);
 * each of their frame waits drives one real platform frame, as everywhere else. */
void rof_boot_chain(int firstScene);

/* The Lucasfilm Games logo (INITAD $5000), hand-written in rof_logo.c.  NOT a
 * transliteration: segment 5 is overwritten before the image Ghidra disassembles, so
 * there is no listing.txt for it and never will be from the single-listing pipeline. */
void rof_logo_run(void);

/* Supplied by the platform TU (XexImage.cpp / PlatformSDL.cpp): power-on RAM (zero mem[]
 * + overlay the OS ROM), and "place the next stage's segments, return the resume offset". */
void     rof_load_stage_reset(void);
uint32_t rof_load_stage(uint32_t from);

/* Set while a boot scene is on screen, so the renderer can pick its copper list without
 * having to infer the scene from mem[] alone (the Station's own VVBLKI $1B30 survives
 * station_exit — os_setvbv is a no-op stub here — and the Logo's $51EF is ours).
 * 0 = no boot scene.  Read by RescueOnFractalus::deriveRenderSignals/renderFrame. */
extern volatile unsigned char g_bootScene;      /* 0 | 1 = Logo | 2 = Station */
#define ROF_BOOTSCENE_NONE    0
#define ROF_BOOTSCENE_LOGO    1
#define ROF_BOOTSCENE_STATION 2

/* Raised while a stage's segments are being placed — i.e. while mem[] is INCONSISTENT and
 * the VVBLKI vector in it is stale or zero.  The Amiga's real INTB_VERTB handler
 * (game_vbi_isr) must stay inert then: it dispatches on mem[$0222/$0223] and falls back to
 * the standby body for an unknown vector, which would run a whole scene VBI over half-loaded
 * RAM.  Inert on SDL, whose VBI only fires from the main thread's tickVBI(). */
extern volatile unsigned char g_bootLoadBusy;

#ifdef __cplusplus
}
#endif

#endif /* ROF_BOOT_H */
