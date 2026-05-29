#include <stdio.h>
#include <stdlib.h>
#include "cpu/cpu.h"
#include "platform/platform.h"
#include "gen/rof_decl.h"

int main(int argc, char *argv[]) {
    const char *image = (argc > 1) ? argv[1] : "disasm/rof_mem.bin";

    if (platform_init() != 0) {
        fprintf(stderr, "platform_init failed\n");
        return 1;
    }
    if (platform_load_image(image) != 0) {
        fprintf(stderr, "failed to load %s\n", image);
        return 1;
    }

    /* Seed CPU registers to power-on state */
    cpu.A = cpu.X = cpu.Y = 0;
    cpu.S = 0xFF;
    cpu.N = cpu.V = cpu.Z = cpu.C = cpu.I = cpu.D = 0;

    printf("[rof] calling game_entry...\n");
    game_entry();
    printf("[rof] game_entry returned\n");

    platform_shutdown();
    return 0;
}
