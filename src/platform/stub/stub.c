/* Stub platform — no-op implementation used during Phase 2 to verify
   the transliterated C compiles and links.  All hardware reads return
   safe defaults; writes are silently dropped.

   POKEY RANDOM returns a simple counter so non-infinite loops that
   spin on RANDOM still terminate.
*/
#include <stdio.h>
#include <string.h>
#include "../platform.h"
#include "../../cpu/cpu.h"

/* ------------------------------------------------------------------ */
/* POKEY RANDOM — 17-bit Galois LFSR matching Atari hardware.
   Taps at bits 5 and 0 (feedback = bit17 XOR bit5).
   Initialized to non-zero; produces identical sequence to real POKEY.
   NOTE: real POKEY runs the LFSR continuously at 64kHz; here we step
   it once per read, which is cycle-inaccurate but functionally correct
   for game logic that reads it in sequence.  Phase 4 refines this.   */
static uint32_t rng_state = 0x1FFFF;  /* all-ones start (POKEY power-on) */

static uint8_t pokey_random_read(void) {
    /* Atari POKEY 17-bit LFSR polynomial: x^17 + x^5 + 1 */
    uint32_t bit = ((rng_state >> 0) ^ (rng_state >> 5)) & 1;
    rng_state = (rng_state >> 1) | (bit << 16);
    return (uint8_t)(rng_state & 0xFF);
}

/* ------------------------------------------------------------------ */
/* Raw hardware bus                                                    */
/* ------------------------------------------------------------------ */
uint8_t platform_hw_read(uint16_t addr) {
    switch (addr) {
    case 0xD20A: return pokey_random_read();   /* POKEY RANDOM        */
    case 0xD300: return 0xFF;                  /* PORTA: no joystick  */
    case 0xD01F: return 0x07;                  /* CONSOL: no buttons  */
    case 0xD40B: return 0x00;                  /* VCOUNT = top of screen */
    case 0xD010: return 0x00;                  /* GRAFP3: no collision */
    case 0xD20F: return 0x00;                  /* SKSTAT              */
    default:     return 0x00;
    }
}

void platform_hw_write(uint16_t addr, uint8_t val) {
    (void)addr; (void)val;
    /* stub: drop all hardware writes */
}

void platform_shadow_write(uint16_t addr, uint8_t val) {
    (void)addr; (void)val;
    /* stub: ignore shadow register notifications */
}

/* ------------------------------------------------------------------ */
/* Convenience helpers                                                 */
/* ------------------------------------------------------------------ */
uint8_t platform_random(void)   { return pokey_random_read(); }
uint8_t platform_porta(void)    { return 0xFF; }
uint8_t platform_consol(void)   { return 0x07; }
uint8_t platform_ch(void)       { return 0xFF; }
uint8_t platform_vcount(void)   { return 0x00; }
uint8_t platform_grafp3(void)   { return 0x00; }

void platform_audf(int ch, uint8_t freq)  { (void)ch; (void)freq; }
void platform_audc(int ch, uint8_t ctrl)  { (void)ch; (void)ctrl; }
void platform_audctl(uint8_t ctl)         { (void)ctl; }

/* ------------------------------------------------------------------ */
/* Frame tick — stub just returns "keep running"                      */
/* ------------------------------------------------------------------ */
int platform_frame(void) {
    return 0;
}

/* ------------------------------------------------------------------ */
/* Init / shutdown                                                     */
/* ------------------------------------------------------------------ */
int platform_init(void) {
    printf("[stub] platform_init\n");
    return 0;
}

void platform_shutdown(void) {
    printf("[stub] platform_shutdown\n");
}

int platform_load_image(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return -1; }
    memset(mem, 0, sizeof(mem));
    size_t n = fread(mem, 1, 65536, f);
    fclose(f);
    printf("[stub] loaded %zu bytes from %s\n", n, path);
    return 0;
}
