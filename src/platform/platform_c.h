#ifndef PLATFORM_C_H
#define PLATFORM_C_H
/* C-compatible bridge header — included by src/cpu/bus.h so the
   C-compiled 6502 transliteration can reach hardware emulation.
   Implemented in platform_cbridge.cpp which calls the C++ Platform singleton. */
#if !defined(ROF_PLATFORM_AMIGA)
#include <stdint.h>   /* Amiga C++ build gets the integer types from the force-included
                         framework/SASCCompat.h; its compat <stdint.h> would clash. */
#endif

#ifdef __cplusplus
extern "C" {
#endif

uint8_t platform_hw_read (uint16_t addr);
void    platform_hw_write(uint16_t addr, uint8_t val);
void    platform_shadow_write(uint16_t addr, uint8_t val);
int     platform_load_image(const char* path);

/* Called once at startup to register Atari address → C function
   mappings for VBI / DLI dispatch. */
void platform_register_vbi(uint16_t addr, void (*fn)(void));

/* Runtime indirect JMP dispatch — used for JMP (zp) DLI chain pattern.
   Looks up addr in the VBI/DLI table and calls the matching function.
   If addr is 0 or unknown, returns without calling anything.            */
void platform_indirect_jmp(uint16_t addr);

/* Render the current display-list state to the SDL window if a new VBI
   frame has been produced since the last call.  Safe to call from a
   spin-wait loop — exits immediately if no new frame is pending.       */
void platform_render_frame(void);

/* Pump the platform event loop without rendering.  Call from any spin-wait
   loop to keep macOS from marking the window as unresponsive.           */
void platform_poll_events(void);

/* Fire a VBI tick if the audio callback has accumulated enough samples.
   Call explicitly from spin-waits that own a full frame boundary
   (FUN_3cb1, L_656e).  Do NOT call from scanline-position waits
   (FUN_3c7b) — VBI resets vcountReg and would prevent them from exiting. */
void platform_tick_vbi(void);

/* Called by boot_standby_launch_driver immediately BEFORE draw_frame_pattern_seq() renders the
   launch tunnel rings into the $1000 GTIA field, so a platform that mirrors mem[] into its own
   framebuffer (the Amiga copper/bitplane backend) can claim + prime its ring bitmap.  The rings
   themselves then arrive span by span through platform_tunnel_rect/_columns/_vspan as the draw
   emits them — the field is never read back.  No-op on the host/SDL build, which renders mem[]
   directly. */
void platform_tunnel_rings_begin(void);

/* Called by copy_title_text_block_to_screen ($782A) right after it rewrites the
   Standby banner text in screen RAM $32B7-$32CA (the SFX sequencer alternates
   "RESCUE ON FRACTALUS!" / copyright via $0091), so a platform that mirrors mem[]
   into bitplanes (the Amiga backend) can flag the title region for re-scan.  No-op
   on the host/SDL build, which renders mem[] directly. */
void platform_title_changed(void);

/* Notification that the compass heading cells $32E3-$32E6 (the mode-4 line below the
   title) were rewritten — by game_sub_4606 (housing $01) or the compass updater $3FDE.
   The Amiga backend re-decodes just those 4 cells; no-op on the host/SDL build. */
void platform_compass_changed(void);

/* Notification that the enemy lock-on indicator cells $3491-$3497 (mode-4, #11) were
   rewritten by the lock-on animation (lock_on_indicator_tick / _step / _phase_advance).
   The in-flight VBI drives this every other frame through lock_on_indicator_dispatch ($4225);
   the transpiled writers don't raise a dirty flag on their own, so the flight path calls
   this to flag the written cell for re-decode (during the planet descent the indicator keeps
   randomly blinking — faithful to the Atari).  `cellIdx` is the cell's offset from $3491
   (0..6): the blink rewrites ONE cell, so flagging the whole strip decoded ~6x too much.
   No-op on the host/SDL build. */
void platform_lockon_changed(uint8_t cellIdx);

/* Notification that an instrument writer changed a span of nCells cockpit cells starting at
   Atari screen-RAM address `addr` ($332D mode4 / $350D modeD).  The Amiga backend records the
   span so render() decodes only those cells (writer-driven cockpit decode, no full scan).
   No-op on platforms that render mem[] directly (SDL / validate headless). */
void platform_cockpit_dirty(uint16_t addr, uint8_t nCells);

/* Notification that a span of nCells Title Screen (scene 3b) digit cells was written starting
   at Atari screen-RAM address `addr` — the STARTING LEVEL digit (level select) or a LAST/HIGH
   SCORE digit (game-over build).  The Amiga backend grows its value-cell dirty range so render()
   redraws only those cells (no full-screen repaint / flash).  No-op on platforms that render
   mem[] directly (SDL / validate headless). */
void platform_title_screen_dirty(uint16_t addr, uint8_t nCells);

/* High-score persistence (docs/high-score-restore-plan.md W2).  The 256-byte save block the
   disk game read over SIO is supplied by rof_hiscore.c; these two move it to and from
   whatever storage the platform has.

   platform_hiscore_load() overwrites blk[] with a previously saved block and returns 1; it
   returns 0 (leaving the factory contents alone) when there is nothing saved or no storage.

   platform_hiscore_save() is called at the moment the GAME writes the block, from inside
   name_entry_loop.  Return 1 only if the block is now safely stored.  Return 0 to decline —
   the plain Amiga build does, because that instant is mid-Forbid with the display and the
   interrupt vectors hijacked, where a dos.library packet Wait() does not belong; it keeps
   the block in RAM and flushes it (rof_hiscore_flush) once the OS is back.  A WHDLoad build
   saves here for real, via resload_SaveFile, which is safe in that context. */
int platform_hiscore_load(uint8_t blk[256]);
int platform_hiscore_save(const uint8_t blk[256]);

/* Poll for a pending in-flight keyboard-command keycode (an Atari KBCODE&$3F, or
   $80 for BREAK), consuming it.  Returns $FF when no command is pending.  Called
   from the flight VBI's CLI window ($519c) to replicate the POKEY keyboard IRQ
   leaving the event id in X for event_sequence_dispatcher ($4644).  Returns $FF on
   platforms that don't deliver in-flight keys (SDL / validate headless). */
uint8_t platform_flight_irq_key(void);

/* Alien jump-scare creature-blit capture: called from the PRE_INSN_HOOK at $80E9 (the
   `STA ($8D),Y` in alien_creature_animate_draw's $80C5 blit) with the target field address + stored byte.
   Records the write extent + row-table geometry (rof_native.c) so a real closed-airlock knock
   reveals the field->plane mapping.  No-op unless the Amiga probe build is active. */
void rof_alien_crwrite(unsigned int addr, unsigned char val);

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_C_H */
