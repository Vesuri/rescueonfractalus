/* Hand-written implementations for self-modifying / special-case routines.
   These are NOT auto-generated; edit this file freely. */
#include "../cpu/cpu.h"
#include "../cpu/bus.h"
#include "rof_decl.h"
#include <string.h>

/* screen_page_swap ($1A62): swaps 5 x 256-byte pages between $40xx and $06xx.
   The original code is self-modifying: it patches the high bytes of its own
   LDA/STA instructions to cycle through pages $40–$44 and $06–$0A.
   The semantics are straightforward so we translate the intent directly. */
void screen_page_swap(void) {
    int page;
    for (page = 0; page < 5; page++) {
        uint8_t *a = (uint8_t*)mem + ((0x40 + page) << 8);
        uint8_t *b = (uint8_t*)mem + ((0x06 + page) << 8);
        uint8_t tmp[256];
        memcpy(tmp, a,   256);
        memcpy(a,   b,   256);
        memcpy(b,   tmp, 256);
    }
}
