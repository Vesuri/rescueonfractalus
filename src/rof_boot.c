/* rof_boot.c — the boot INITAD chain: staged XEX load + the two boot scenes.
 *
 * On the Atari, rof.xex is loaded by the OS in stages: the loader places segments until
 * one writes INITAD ($02E2), JSRs through it, then carries on with the next segment.
 * rof.xex uses that four times:
 *
 *   segments  1..7   INITAD $5000   the Lucasfilm Games logo      (scene 1)
 *   segments  8..11  INITAD $1A97   page swap + station cinematic (scene 2)
 *   segments 12..14  INITAD $B800   display init (CHBAS/DL/colours/DMACTL)
 *   segments 15..20  INITAD $3CDE   game_entry — the real program entry
 *
 * The staging is not cosmetic: LATER SEGMENTS OVERWRITE EARLIER ONES, so there is no
 * instant at which a full image holds either boot scene's data.  Segment 16 ($3CDE-$B7FF)
 * buries the logo code at $5000 AND the station image that segment 10 stages at
 * $4000-$44FF for screen_page_swap to move down to $0600.  Load everything at once and
 * the station's page swap moves main-blob code into its screen RAM instead of the image.
 *
 * Nothing is saved or restored anywhere: each phase simply sees the memory the Atari sees.
 * $0600-$0AFF even comes out CLEAN by construction — station_exit's second
 * screen_page_swap swaps the (zero) $4000-$44FF back into it, and segment 16 then
 * overwrites $4000 itself.  Full derivation: docs/logo-station-plan.md §3.
 *
 * Scene 2 needs no new code — station_init/display_list_build/station_audio/
 * station_star_fade_in/vbi_handler_station are all transliterated in rof_gen.c and
 * initad_1A97() is a single faithful entry point for the whole cinematic.  Scene 1 does:
 * see rof_logo.c.
 */
#include "cpu/cpu.h"        /* mem[] + the integer types */
#include "gen/rof_decl.h"   /* initad_1A97 / init_B800 (transliterated) */
#include "rof_boot.h"

/* Which boot scene (if any) is on screen, and "mem[] is mid-load" — see rof_boot.h. */
volatile unsigned char g_bootScene    = ROF_BOOTSCENE_NONE;
volatile unsigned char g_bootLoadBusy = 0;

/* Place one stage with the real VBI held inert (mem[] is inconsistent while it runs). */
static uint32_t boot_stage(uint32_t from)
{
    uint32_t next;
    g_bootLoadBusy = 1;
    next = rof_load_stage(from);
    g_bootLoadBusy = 0;
    return next;
}

void rof_boot_chain(int firstScene)
{
    uint32_t off = 0;

    /* Power-on RAM.  On the Amiga this REDOES the zero + OS-ROM overlay that
       PlatformAmiga::run() already did with a full load — deliberately.  That full load
       exists so RescueOnFractalus::initialize() (which decodes cockpit/title/door bitmaps
       out of mem[]) sees exactly the image it has always seen, keeping the SKIPBOOT build
       byte-for-byte identical to the pre-boot-scenes port.  Redoing it costs ~120 KB of
       stores once, invisible against a multi-second boot, and buys a single readable
       staged walk instead of a second load path spliced through initialize(). */
    g_bootLoadBusy = 1;
    rof_load_stage_reset();
    g_bootLoadBusy = 0;

    /* ---- stage 1: segments 1..7 -> INITAD $5000, the logo -------------------------- */
    off = boot_stage(off);
    if (firstScene <= ROF_BOOT_LOGO) {
        g_bootScene = ROF_BOOTSCENE_LOGO;
        rof_logo_run();
        g_bootScene = ROF_BOOTSCENE_NONE;
    }

    /* ---- stage 2: segments 8..11 -> INITAD $1A97, the station cinematic ------------ */
    off = boot_stage(off);
    if (firstScene <= ROF_BOOT_STATION) {
        /* initad_1A97: screen_page_swap (the $4000 image -> $0600 screen RAM), silence
           AUDF3/AUDF4, then station_init — which installs VVBLKI=$1B30, builds the
           scrolling display list at $B800, fades the starfield in and runs the cinematic
           until START, any key, or the ~17 s RTCLOK_MID>=4 timeout.  Its exit path swaps
           the pages back and kills PMG/playfield DMA. */
        g_bootScene = ROF_BOOTSCENE_STATION;
        initad_1A97();
        g_bootScene = ROF_BOOTSCENE_NONE;
    }

    /* ---- stage 3: segments 12..14 -> INITAD $B800, display init -------------------- */
    off = boot_stage(off);
    if (firstScene <= ROF_BOOT_STATION) {
        /* init_B800 ($B800, transliterated): CHBAS=$04, SDLSTL/H=$B832, COLOR0/1/2/4,
           SDMCTL=$22, then waits ~32 frames on RTCLOK before re-enabling DMACTL.  Only
           run on a path that actually played a boot scene: it is skipped entirely in the
           port's historical (and SKIPBOOT) entry, where game_entry does its own display
           setup, and the RTCLOK it spins on is advanced by whichever VBI body is live. */
        init_B800();
    }

    /* ---- stage 4: segments 15..20 -> INITAD $3CDE (game_entry, run by the caller) --- */
    for (;;) {
        uint32_t next = boot_stage(off);
        if (next == off) break;         /* no progress: malformed tail, stop */
        off = next;
        if (mem[0x02E2] == 0xDE && mem[0x02E3] == 0x3C) break;   /* INITAD = $3CDE */
    }
}
