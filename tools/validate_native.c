/* Native-reimplementation validation harness.
 *
 * For each function listed in transpile.py's VALIDATE_FUNCS, two versions exist:
 *   <name>()         — the native reimplementation        (src/gen/rof_native.c)
 *   <name>__t6502()  — the faithful 6502 transliteration  (src/gen/rof_gen.c)
 *
 * This harness runs both on the SAME randomized pre-state and diffs the full
 * machine state (all 64 KB of mem[] + the CPU registers/flags). Memory diffs
 * are failures (the contract); CPU-register diffs are reported separately as
 * "incidental" because for divide_16x16 the callers don't read them.
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

int main(void) {
    enum { N = 200000 };
    static uint8_t pre[65536], ref_mem[65536];
    const int out_addrs[] = { 0x00B0, 0x00B1, 0x00B2, 0x00AE, 0x00AF };

    int mem_fail_cases = 0, cpu_diff_cases = 0, printed = 0;

    for (int t = 0; t < N; t++) {
        /* Domain: divisor in [1, 0x7FFF] so normalization terminates. */
        uint16_t divisor  = (uint16_t)(xs() % 0x7FFF) + 1;
        uint16_t dividend = (uint16_t)(xs() & 0xFFFF);
        uint8_t  q0       = (uint8_t)(xs() & 0xFF);

        memset(pre, 0, sizeof pre);
        pre[0x00B0] = dividend & 0xFF; pre[0x00B1] = dividend >> 8;
        pre[0x00AE] = divisor  & 0xFF; pre[0x00AF] = divisor  >> 8;
        pre[0x00B2] = q0;
        Cpu6502 pre_cpu = zero_cpu();

        /* ---- reference: transliterated 6502 ---- */
        memcpy((void *)mem, pre, sizeof pre);
        cpu = pre_cpu;
        divide_16x16__t6502();
        memcpy(ref_mem, (void *)mem, sizeof ref_mem);
        Cpu6502 ref_cpu = cpu;

        /* ---- candidate: native ---- */
        memcpy((void *)mem, pre, sizeof pre);
        cpu = pre_cpu;
        divide_16x16();

        /* full-memory diff (the contract) */
        int memdiffs = 0;
        for (int i = 0; i < 65536; i++) {
            if (mem[i] != ref_mem[i]) {
                memdiffs++;
                if (printed < 12) {
                    printf("[MEM DIFF] case %d  $%04X  ref=$%02X native=$%02X   "
                           "(divisor=%u dividend=%u q0=%u)\n",
                           t, i, ref_mem[i], mem[i], divisor, dividend, q0);
                    printed++;
                }
            }
        }
        if (memdiffs) mem_fail_cases++;

        /* CPU register/flag diff (incidental for this function) */
        if (cpu.A != ref_cpu.A || cpu.X != ref_cpu.X || cpu.Y != ref_cpu.Y ||
            cpu.N != ref_cpu.N || cpu.V != ref_cpu.V ||
            cpu.Z != ref_cpu.Z || cpu.C != ref_cpu.C)
            cpu_diff_cases++;
    }

    printf("\ndivide_16x16: ran %d random cases\n", N);
    printf("  memory-contract mismatches : %d  (must be 0)\n", mem_fail_cases);
    printf("  cpu-register diffs         : %d  (incidental — callers save Y via $009F,\n"
           "                                     overwrite A/flags immediately)\n", cpu_diff_cases);
    printf("  contract output cells diffed: ");
    for (size_t k = 0; k < sizeof out_addrs / sizeof out_addrs[0]; k++)
        printf("$%04X ", out_addrs[k]);
    printf("\n\n%s\n", mem_fail_cases == 0
        ? "PASS — native divide_16x16 is memory-equivalent to the transliteration."
        : "FAIL — native version diverges from the 6502 oracle (see diffs above).");

    return mem_fail_cases == 0 ? 0 : 1;
}
