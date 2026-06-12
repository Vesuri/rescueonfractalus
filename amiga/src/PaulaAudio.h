#pragma once
/* Paula audio backend for RoF Amiga.
 * Routes POKEY register writes (from the 6502-converted station_audio)
 * to Amiga Paula hardware via DMACON-driven square-wave DMA.
 * Also implements the platform_c.h bridge (all stubs except the POKEY ones).
 */
#include "../framework/Util.h"  // provides uint8_t, uint16_t, uint32_t
#ifdef __cplusplus
extern "C" {
#endif

void paula_audio_init(void);      /* load XEX + init Paula DMA; call once */
void paula_audio_shutdown(void);  /* disable Paula DMA; call on exit */
uint8_t paula_pokey_random(void); /* advance + return one POKEY LFSR byte */
void    paula_noise_tick(void);   /* per-VBI: refresh the poly17 noise sample (continuous noise) */

/* Platform bridge — C linkage so bus.h inlines (compiled from C rof_gen.c)
 * can reach them.  Declared in platform_c.h; defined here. */
uint8_t platform_hw_read (uint16_t addr);
void    platform_hw_write(uint16_t addr, uint8_t val);
void    platform_shadow_write(uint16_t addr, uint8_t val);
void    platform_register_vbi(uint16_t addr, void (*fn)(void));
void    platform_indirect_jmp(uint16_t addr);
void    platform_render_frame(void);
void    platform_poll_events(void);
void    platform_tick_vbi(void);
int     platform_load_image(const char* path);

#ifdef __cplusplus
}
#endif
