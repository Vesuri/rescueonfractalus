#ifndef PLATFORM_H
#define PLATFORM_H
#include <stdint.h>

/* Platform abstraction API.
   Maps every hardware-register access identified in docs/hw-access.md
   to a backend-neutral function.  The SDL (macOS) and Amiga backends
   each provide a concrete implementation.

   Register groups (see docs/atari-hardware.md):
     GTIA  $D000–$D01F — sprites, colors, collision, console buttons
     POKEY $D200–$D21F — audio, RANDOM, keyboard, timers
     PIA   $D300–$D31F — joystick ports, XL/XE bank switching
     ANTIC $D400–$D41F — display list, DMA control, WSYNC, NMI
*/

/* ------------------------------------------------------------------ */
/* Raw hardware bus (called by bus.h for the $D000-$D7FF range)       */
/* ------------------------------------------------------------------ */
uint8_t platform_hw_read (uint16_t addr);
void    platform_hw_write(uint16_t addr, uint8_t val);

/* Called by bus_write for OS page-2 shadow register writes so the
   platform can react (display-list changes, vector changes, etc.).
   Backends may ignore writes they don't care about.               */
void    platform_shadow_write(uint16_t addr, uint8_t val);

/* ------------------------------------------------------------------ */
/* Frame / interrupt callbacks (called by the platform run loop)      */
/* ------------------------------------------------------------------ */

/* Tick one video frame: runs the NMI VBI handler, then renders.
   Returns 0 to continue, non-zero to quit.                        */
int platform_frame(void);

/* ------------------------------------------------------------------ */
/* Specific hardware reads (convenience wrappers used by the game)    */
/* ------------------------------------------------------------------ */

/* POKEY RANDOM ($D20A): must reproduce the 17-bit LFSR bit-exactly   */
uint8_t platform_random(void);

/* Joystick: PORTA ($D300) — bit 0=up 1=down 2=left 3=right (active low) */
uint8_t platform_porta(void);

/* Console buttons: CONSOL ($D01F) — bits: START=0x01 SELECT=0x02 OPTION=0x04 (active low from 0x07) */
uint8_t platform_consol(void);

/* Keyboard: CH ($02FC shadow) — $FF = no key                         */
uint8_t platform_ch(void);

/* ANTIC VCOUNT ($D40B) — current vertical scanline / 2               */
uint8_t platform_vcount(void);

/* GTIA player-3 graphic / collision ($D010)                          */
uint8_t platform_grafp3(void);

/* ------------------------------------------------------------------ */
/* Audio (POKEY channels 1-4)                                         */
/* ------------------------------------------------------------------ */
/* All audio state is written via platform_hw_write; these helpers    */
/* provide typed access for platform implementations.                 */
void platform_audf(int ch, uint8_t freq);   /* channel 1-4 */
void platform_audc(int ch, uint8_t ctrl);
void platform_audctl(uint8_t ctl);

/* ------------------------------------------------------------------ */
/* Initialisation / shutdown                                          */
/* ------------------------------------------------------------------ */
int  platform_init(void);
void platform_shutdown(void);

/* Load the post-load 64K ROM image into mem[] at startup.
   Returns 0 on success.                                              */
int  platform_load_image(const char *path);

#endif /* PLATFORM_H */
