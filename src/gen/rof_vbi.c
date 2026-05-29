/* Register the Atari address → C function dispatch table for VBI handlers.
   Called once from main.cpp before game_entry().
   Add new handlers here as they are identified in Phase 4/5 analysis. */
#include "../platform/platform_c.h"
#include "rof_decl.h"

void rof_register_vbi_handlers(void) {
    platform_register_vbi(0x1B30, vbi_handler_attract);
    platform_register_vbi(0x53CC, vbi_handler_1);
    platform_register_vbi(0x4FF5, vbi_handler_2);
    platform_register_vbi(0x52D7, vbi_handler_game);
    /* Additional handlers discovered during Phase 4 analysis go here. */
}
