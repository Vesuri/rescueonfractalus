#ifndef PLATFORM_C_H
#define PLATFORM_C_H
/* C-compatible bridge header — included by src/cpu/bus.h so the
   C-compiled 6502 transliteration can reach hardware emulation.
   Implemented in platform_cbridge.cpp which calls the C++ Platform singleton. */
#include <stdint.h>

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

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_C_H */
