/* Native-reimplementation validation harness.
 *
 * For each function listed in transpile.py's VALIDATE_FUNCS, two versions exist:
 *   <name>()         — the native reimplementation        (src/gen/rof_native.c)
 *   <name>__t6502()  — the faithful 6502 transliteration  (src/gen/rof_gen.c)
 *
 * This harness runs both on the SAME randomized pre-state and diffs the full
 * machine state (all 64 KB of mem[] + the CPU registers/flags). Memory diffs
 * are failures (the contract); CPU-register diffs are reported separately as
 * "incidental" — for these leaf routines the callers reload the registers
 * before reading them (verified at each call site).
 *
 * Build:  make validate     Run:  ./build/validate_native
 *
 * Deterministic: uses a fixed-seed xorshift PRNG (no wall-clock / rand()), so a
 * green run is reproducible and a regression is bisectable.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../src/cpu/cpu.h"
#include "../src/gen/rof_decl.h"

static uint32_t rng = 0x9D6F1234u;
static uint32_t xs(void) { uint32_t x = rng; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return rng = x; }

/* A neutral, deterministic starting CPU state for both runs. */
static Cpu6502 zero_cpu(void) {
    Cpu6502 c; memset(&c, 0, sizeof c); c.S = 0xFF; return c;
}

/* Run native vs __t6502 on the SAME pre-state; diff full mem[] + cpu.
 * Returns the number of cases with a memory-contract mismatch. */
static int diff_run(const char *name, const uint8_t *pre, Cpu6502 pre_cpu,
                    void (*native)(void), void (*t6502)(void),
                    int t, int *printed, int *cpu_diff_cases) {
    static uint8_t ref_mem[65536];

    memcpy((void *)mem, pre, 65536); cpu = pre_cpu;
    t6502();
    memcpy(ref_mem, (void *)mem, sizeof ref_mem);
    Cpu6502 ref_cpu = cpu;

    memcpy((void *)mem, pre, 65536); cpu = pre_cpu;
    native();

    int memdiffs = 0;
    for (int i = 0; i < 65536; i++) {
        if (mem[i] != ref_mem[i]) {
            memdiffs++;
            if (*printed < 12) {
                printf("[MEM DIFF] %s case %d  $%04X  ref=$%02X native=$%02X\n",
                       name, t, i, ref_mem[i], mem[i]);
                (*printed)++;
            }
        }
    }
    if (cpu.A != ref_cpu.A || cpu.X != ref_cpu.X || cpu.Y != ref_cpu.Y ||
        cpu.N != ref_cpu.N || cpu.V != ref_cpu.V ||
        cpu.Z != ref_cpu.Z || cpu.C != ref_cpu.C)
        (*cpu_diff_cases)++;
    return memdiffs ? 1 : 0;
}

/* --- divide_16x16 @ $9D6F: random 16-bit divides over the valid domain. --- */
static int test_divide_16x16(void) {
    enum { N = 200000 };
    static uint8_t pre[65536];
    int mem_fail = 0, cpu_diff = 0, printed = 0;

    for (int t = 0; t < N; t++) {
        uint16_t divisor  = (uint16_t)(xs() % 0x7FFF) + 1;   /* [1, 0x7FFF] */
        uint16_t dividend = (uint16_t)(xs() & 0xFFFF);
        uint8_t  q0       = (uint8_t)(xs() & 0xFF);

        memset(pre, 0, sizeof pre);
        pre[0x00B0] = dividend & 0xFF; pre[0x00B1] = dividend >> 8;
        pre[0x00AE] = divisor  & 0xFF; pre[0x00AF] = divisor  >> 8;
        pre[0x00B2] = q0;

        mem_fail += diff_run("divide_16x16", pre, zero_cpu(),
                             divide_16x16, divide_16x16__t6502, t, &printed, &cpu_diff);
    }

    printf("divide_16x16 : %d cases, %d mem mismatch (must be 0), %d cpu diffs "
           "(incidental — callers save Y via $009F, overwrite A/flags)\n",
           N, mem_fail, cpu_diff);
    return mem_fail;
}

/* --- terrain_gen_3 @ $AD5F: clear-column over a FULLY randomized mem[]. ---
 * Randomizing all of mem[] means any stray write (wrong cell, wrong span) shows
 * up as a diff.  X spans 0..255 to exercise the 6502 byte-index wrap too. */
static int test_terrain_gen_3(void) {
    enum { N = 100000 };
    static uint8_t pre[65536];
    int mem_fail = 0, cpu_diff = 0, printed = 0;

    for (int t = 0; t < N; t++) {
        for (int i = 0; i < 65536; i++) pre[i] = (uint8_t)(xs() & 0xFF);
        Cpu6502 c = zero_cpu();
        c.X = (uint8_t)(xs() & 0xFF);

        mem_fail += diff_run("terrain_gen_3", pre, c,
                             terrain_gen_3, terrain_gen_3__t6502, t, &printed, &cpu_diff);
    }

    printf("terrain_gen_3: %d cases, %d mem mismatch (must be 0), %d cpu diffs "
           "(incidental — callers reload X/A after the call)\n",
           N, mem_fail, cpu_diff);
    return mem_fail;
}

/* --- signed_mul_8x16 @ $9C97: random fixed-point multiplies. ---
 * Full random mem[] (catches stray writes); random multiplier A, 16-bit signed
 * multiplicand $AA/$AB, and entry carry (which threads into the final $AC). */
static int test_signed_mul_8x16(void) {
    enum { N = 100000 };
    static uint8_t pre[65536];
    int mem_fail = 0, cpu_diff = 0, printed = 0;

    for (int t = 0; t < N; t++) {
        for (int i = 0; i < 65536; i++) pre[i] = (uint8_t)(xs() & 0xFF);
        pre[0x00AA] = (uint8_t)(xs() & 0xFF);   /* multiplicand lo */
        pre[0x00AB] = (uint8_t)(xs() & 0xFF);   /* multiplicand hi (sign) */
        Cpu6502 c = zero_cpu();
        c.A = (uint8_t)(xs() & 0xFF);           /* multiplier */
        c.C = (uint8_t)(xs() & 1);              /* entry carry -> first ROR $AC */

        mem_fail += diff_run("signed_mul_8x16", pre, c,
                             signed_mul_8x16, signed_mul_8x16__t6502, t, &printed, &cpu_diff);
    }

    printf("signed_mul_8x16: %d cases, %d mem mismatch (must be 0), %d cpu diffs "
           "(incidental — callers reload A/flags after the call)\n",
           N, mem_fail, cpu_diff);
    return mem_fail;
}

int main(void) {
    int fails = 0;
    fails += test_divide_16x16();
    fails += test_terrain_gen_3();
    fails += test_signed_mul_8x16();

    printf("\n%s\n", fails == 0
        ? "PASS — all native reimplementations are memory-equivalent to their 6502 oracles."
        : "FAIL — a native version diverges from its 6502 oracle (see diffs above).");
    return fails == 0 ? 0 : 1;
}
