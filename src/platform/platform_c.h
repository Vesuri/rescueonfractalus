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

/* Called by display_setup right after draw_frame_pattern_seq() has rendered the
   launch tunnel rings into the $1000 GTIA field, so a platform that mirrors mem[]
   into its own framebuffer (the Amiga copper/bitplane backend) can convert the
   freshly-drawn rings to bitplanes.  No-op on the host/SDL build, which renders
   mem[] directly. */
void platform_tunnel_rings_drawn(void);

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

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_C_H */
