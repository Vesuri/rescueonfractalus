/* Register the Atari address → C function dispatch table for VBI handlers.
   Called once from main.cpp before game_entry().
   Add new handlers here as they are identified in Phase 4/5 analysis. */
#include "../platform/platform_c.h"
#include "rof_decl.h"
#include "rof_manual.h"

void rof_register_vbi_handlers(void) {
    /* VBI handlers */
    platform_register_vbi(0x1B30, vbi_handler_attract);
    platform_register_vbi(0x53CC, vbi_handler_1);
    platform_register_vbi(0x4FF5, vbi_handler_2);
    platform_register_vbi(0x52D7, vbi_handler_game);

    /* DLI top-level handlers (called by renderer on DL entries with bit 7) */
    platform_register_vbi(0x49EE, dli_handler_game);
    platform_register_vbi(0x6CC2, dli_handler_game2);
    platform_register_vbi(0x6CAD, dli_handler_cockpit);  /* cockpit layout 2 */

    /* DLI colour-setting subroutines (called via platform_indirect_jmp) */
    platform_register_vbi(0x4A0C, dli_sub_4a0c);
    platform_register_vbi(0x4A11, dli_sub_4a11);
    platform_register_vbi(0x4A1F, dli_sub_4a1f);
    platform_register_vbi(0x4A40, dli_sub_4a40);
    platform_register_vbi(0x4A78, dli_sub_4a78);
    platform_register_vbi(0x4ACD, dli_sub_4acd);
    platform_register_vbi(0x6CD7, dli_sub_6cd7);
    platform_register_vbi(0x6CF1, dli_sub_6cf1);
    platform_register_vbi(0x6D0E, dli_sub_6d0e);
    platform_register_vbi(0x6D28, dli_sub_6d28);
    platform_register_vbi(0x6D42, dli_sub_6d42);
    platform_register_vbi(0x6D4F, dli_sub_6d4f);
    platform_register_vbi(0x6D67, dli_sub_6d67);
    platform_register_vbi(0x6D7C, dli_sub_6d7c);
    platform_register_vbi(0x6D99, dli_sub_6d99);
    platform_register_vbi(0x6DA1, dli_sub_6da1);
}
