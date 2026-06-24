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

/* Test filter: `make validate FN="name ..."` (or run ./build/validate_native name ...)
 * runs only the tests whose name CONTAINS one of the given substrings — so FN=terrain
 * runs all terrain_*, FN=mul_u8 runs just that.  No args (g_nfilter==0) runs everything.
 * Each test gates itself via want(<its name>) and returns 0 (skipped) when filtered out.
 * Lets you validate only the function(s) you just changed instead of the full suite. */
static char **g_filter = 0;
static int    g_nfilter = 0;
static int want(const char *name) {
    if (g_nfilter == 0) return 1;
    for (int i = 0; i < g_nfilter; i++)
        if (strstr(name, g_filter[i])) return 1;
    return 0;
}

/* Per-test contract mask: addresses excluded from the mem[] diff.  Use ONLY for
 * cells PROVEN dead after the function returns (no reader before the next write),
 * so the native version is free not to reproduce them.  e.g. project_terrain_points
 * leaves divide_16x16's $AE/$AF/$B0/$B1/$B2 scratch behind — those cells are touched
 * by NO routine other than divide_16x16 + project_terrain_points (verified via
 * disasm/zeropage.csv), so their exit value is dead.  Scoped: set before a test,
 * clear (n=0) after, so it never relaxes another function's contract. */
static const uint16_t *g_ignore = 0;
static int             g_ignore_n = 0;
static void set_ignore(const uint16_t *addrs, int n) { g_ignore = addrs; g_ignore_n = n; }

/* Test-only platform bridge (platform_cbridge.cpp): a headless Platform whose
   hwRead routes POKEY RANDOM ($D20A) to a seedable LFSR.  Seeding both runs of
   a case identically gives them the same RANDOM stream so RANDOM-reading
   terrain routines diff deterministically. */
void     platform_test_init_headless(void);
void     platform_test_seed_rng(uint32_t s);
uint32_t platform_test_get_rng(void);

/* Fill a 64 KB buffer with random bytes, 32 bits at a time (4x fewer PRNG calls
   than a per-byte loop — the dominant cost of the full-random-mem tests). */
static void fill_random(uint8_t *buf) {
    uint32_t *w = (uint32_t *)buf;
    for (int i = 0; i < 65536 / 4; i++) w[i] = xs();
}

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

    /* Give both runs an identical POKEY RANDOM ($D20A) stream.  Harmless for
       routines that never read it.  17-bit nonzero (0 is an LFSR lockup). */
    uint32_t rseed = (xs() & 0x1FFFF); if (rseed == 0) rseed = 1;

    platform_test_seed_rng(rseed);
    memcpy((void *)mem, pre, 65536); cpu = pre_cpu;
    t6502();
    memcpy(ref_mem, (void *)mem, sizeof ref_mem);
    Cpu6502 ref_cpu = cpu;

    platform_test_seed_rng(rseed);
    memcpy((void *)mem, pre, 65536); cpu = pre_cpu;
    native();

    /* Neutralize the masked (proven-dead) cells in both buffers so they compare
       equal — excludes them from the contract for this test only. */
    for (int i = 0; i < g_ignore_n; i++) ref_mem[g_ignore[i]] = mem[g_ignore[i]];

    /* Fast path: memcmp (SIMD libc) over the whole 64 KB; only walk byte-by-byte
       to report offending cells when a difference actually exists. */
    int mem_failed = memcmp((const void *)mem, ref_mem, 65536) != 0;
    if (mem_failed && *printed < 12) {
        for (int i = 0; i < 65536 && *printed < 12; i++) {
            if (mem[i] != ref_mem[i]) {
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
    return mem_failed ? 1 : 0;
}

/* --- divide_16x16 @ $9D6F: random 16-bit divides over the valid domain. ---
 * Domain is divisor in [1, 0x7FFF] AND dividend < divisor: the only caller
 * (project_terrain_points) scales the divisor up until it exceeds the dividend
 * before calling, and that is also the domain in which the 6502's 8-bit quotient
 * does not overflow (dividend >= divisor would shift quotient bits off the top).
 * The native divide is exact across this whole domain. */
static int test_divide_16x16(void) {
    if (!want("divide_16x16")) return 0;
    enum { N = 200000 };
    static uint8_t pre[65536];
    int mem_fail = 0, cpu_diff = 0, printed = 0;

    for (int t = 0; t < N; t++) {
        uint16_t divisor  = (uint16_t)(xs() % 0x7FFF) + 1;   /* [1, 0x7FFF] */
        uint16_t dividend = (uint16_t)(xs() % divisor);      /* < divisor (caller's contract) */
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

/* --- clear_terrain_column @ $AD5F: clear-column over a FULLY randomized mem[]. ---
 * Randomizing all of mem[] means any stray write (wrong cell, wrong span) shows
 * up as a diff.  X spans 0..255 to exercise the 6502 byte-index wrap too. */
static int test_clear_terrain_column(void) {
    if (!want("clear_terrain_column")) return 0;
    enum { N = 20000 };
    static uint8_t pre[65536];
    int mem_fail = 0, cpu_diff = 0, printed = 0;

    for (int t = 0; t < N; t++) {
        fill_random(pre);
        Cpu6502 c = zero_cpu();
        c.X = (uint8_t)(xs() & 0xFF);

        mem_fail += diff_run("clear_terrain_column", pre, c,
                             clear_terrain_column, clear_terrain_column__t6502, t, &printed, &cpu_diff);
    }

    printf("clear_terrain_column: %d cases, %d mem mismatch (must be 0), %d cpu diffs "
           "(incidental — callers reload X/A after the call)\n",
           N, mem_fail, cpu_diff);
    return mem_fail;
}

/* --- signed_mul_8x16 @ $9C97: random fixed-point multiplies. ---
 * Full random mem[] (catches stray writes); random multiplier A, 16-bit signed
 * multiplicand $AA/$AB, and entry carry (which threads into the final $AC). */
static int test_signed_mul_8x16(void) {
    if (!want("signed_mul_8x16")) return 0;
    enum { N = 20000 };
    static uint8_t pre[65536];
    int mem_fail = 0, cpu_diff = 0, printed = 0;

    for (int t = 0; t < N; t++) {
        fill_random(pre);
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

/* --- render_bcd_counter @ $49A0: BCD score render over FULLY randomized mem[]. ---
 * Random mem exercises arbitrary BCD bytes ($0601-$0603), suppress flag ($0600),
 * and prior screen content at $32C5.. (suppressed leading zeros must leave it
 * untouched).  emit_bcd_byte_digits' PHA/PLA leaves a dead scribble at $01FF
 * (S=$FF here) that the nibble-extracting native doesn't make — excluded. */
static int test_render_bcd_counter(void) {
    if (!want("render_bcd_counter")) return 0;
    enum { N = 20000 };
    static uint8_t pre[65536];
    static const uint16_t ignore[] = { 0x01FF };
    int mem_fail = 0, cpu_diff = 0, printed = 0;
    set_ignore(ignore, 1);
    for (int t = 0; t < N; t++) {
        fill_random(pre);
        mem_fail += diff_run("render_bcd_counter", pre, zero_cpu(),
                             render_bcd_counter, render_bcd_counter__t6502, t, &printed, &cpu_diff);
    }
    set_ignore(0, 0);
    printf("render_bcd_counter: %d cases, %d mem mismatch (must be 0), %d cpu diffs\n",
           N, mem_fail, cpu_diff);
    return mem_fail;
}

/* --- stack-aware push helpers (ring_push_marked $5815, game_sub_55FC $55FC). ---
 * Like ring_push_0719, the contract includes cpu.A/X/S (the 6502 PHAs the index
 * and ring_push_0719's PLA;TAX hands it back).  Randomize entry A/X/Y/S + ring
 * head $0073.  Both runs use the real 6502 stack ops, so the $01xx stack byte
 * matches without masking. */
static int test_stack_push(const char *name, void (*nat)(void), void (*t6502)(void)) {
    if (!want(name)) return 0;
    enum { N = 20000 };
    static uint8_t pre[65536], ref[65536];
    int mem_fail = 0, cpu_fail = 0, printed = 0;
    for (int t = 0; t < N; t++) {
        fill_random(pre);
        Cpu6502 c = zero_cpu();
        c.A = (uint8_t)xs(); c.X = (uint8_t)xs(); c.Y = (uint8_t)xs(); c.S = (uint8_t)xs();
        pre[0x0073] = (uint8_t)xs();
        memcpy((void *)mem, pre, 65536); cpu = c; t6502();
        memcpy(ref, (void *)mem, sizeof ref); Cpu6502 rc = cpu;
        memcpy((void *)mem, pre, 65536); cpu = c; nat();
        if (memcmp((const void *)mem, ref, 65536) != 0) {
            mem_fail++;
            if (printed < 12)
                for (int i = 0; i < 65536 && printed < 12; i++)
                    if (mem[i] != ref[i]) {
                        printf("[MEM DIFF] %s case %d  $%04X  ref=$%02X native=$%02X\n",
                               name, t, i, ref[i], mem[i]); printed++;
                    }
        }
        if (cpu.A != rc.A || cpu.X != rc.X || cpu.S != rc.S) cpu_fail++;
    }
    printf("%s: %d cases, %d mem mismatch, %d cpu(A/X/S) mismatch (both must be 0)\n",
           name, N, mem_fail, cpu_fail);
    return mem_fail + cpu_fail;
}

/* --- generic memory-contract test over a FULLY randomized mem[] and a neutral
 * entry CPU.  Fits any leaf whose result is observed purely through mem[] and
 * that does not read entry registers (lookup tables, etc., read from the same
 * random mem[] in both runs).  Stray writes and logic divergence both show up. */
static int test_mem_contract(const char *name, void (*native)(void), void (*t6502)(void)) {
    if (!want(name)) return 0;
    enum { N = 20000 };
    static uint8_t pre[65536];
    int mem_fail = 0, cpu_diff = 0, printed = 0;

    for (int t = 0; t < N; t++) {
        fill_random(pre);
        mem_fail += diff_run(name, pre, zero_cpu(), native, t6502, t, &printed, &cpu_diff);
    }
    printf("%s: %d cases, %d mem mismatch (must be 0), %d cpu diffs "
           "(incidental — callers reload A/flags)\n", name, N, mem_fail, cpu_diff);
    return mem_fail;
}

/* --- ring_push_0719 @ $55FF: stack-aware leaf. ---
 * Its contract includes CPU state (the PLA;TAX hands the pulled X back to the
 * caller), so this test fails on cpu.A/X/S mismatch in addition to mem[].
 * Randomizes the ring head $0073, cpu.A, and cpu.S (so PLA reads varied stack). */
static int test_ring_push_0719(void) {
    if (!want("ring_push_0719")) return 0;
    enum { N = 20000 };
    static uint8_t pre[65536], ref_mem[65536];
    int mem_fail = 0, cpu_fail = 0, printed = 0;

    for (int t = 0; t < N; t++) {
        fill_random(pre);
        Cpu6502 c = zero_cpu();
        c.A = (uint8_t)(xs() & 0xFF);
        c.S = (uint8_t)(xs() & 0xFF);          /* random SP -> PLA reads varied stack */
        pre[0x0073] = (uint8_t)(xs() & 0xFF);  /* random head exercises clamp + wrap */

        memcpy((void *)mem, pre, 65536); cpu = c;
        ring_push_0719__t6502();
        memcpy(ref_mem, (void *)mem, sizeof ref_mem);
        Cpu6502 ref_cpu = cpu;

        memcpy((void *)mem, pre, 65536); cpu = c;
        ring_push_0719();

        if (memcmp((const void *)mem, ref_mem, 65536) != 0) {
            mem_fail++;
            if (printed < 12) {
                for (int i = 0; i < 65536 && printed < 12; i++)
                    if (mem[i] != ref_mem[i]) {
                        printf("[MEM DIFF] ring_push_0719 case %d  $%04X  ref=$%02X native=$%02X\n",
                               t, i, ref_mem[i], mem[i]); printed++;
                    }
            }
        }
        if (cpu.A != ref_cpu.A || cpu.X != ref_cpu.X || cpu.S != ref_cpu.S)
            cpu_fail++;
    }
    printf("ring_push_0719: %d cases, %d mem mismatch, %d cpu(A/X/S) mismatch (both must be 0)\n",
           N, mem_fail, cpu_fail);
    return mem_fail + cpu_fail;
}

/* --- mul_u8 @ $9821: returns its product in cpu.A (the caller reads A, e.g. via a
 * following BIT), so the contract is mem[] AND cpu.A.  $006B/$28D6 are consumed. --- */
static int test_mul_u8(void) {
    if (!want("mul_u8")) return 0;
    enum { N = 20000 };
    static uint8_t pre[65536], ref_mem[65536];
    int mem_fail = 0, cpu_fail = 0, printed = 0;

    for (int t = 0; t < N; t++) {
        fill_random(pre);
        Cpu6502 c = zero_cpu();

        memcpy((void *)mem, pre, 65536); cpu = c;
        mul_u8__t6502();
        memcpy(ref_mem, (void *)mem, sizeof ref_mem);
        Cpu6502 ref_cpu = cpu;

        memcpy((void *)mem, pre, 65536); cpu = c;
        mul_u8();

        if (memcmp((const void *)mem, ref_mem, 65536) != 0) {
            mem_fail++;
            if (printed < 12)
                for (int i = 0; i < 65536 && printed < 12; i++)
                    if (mem[i] != ref_mem[i]) {
                        printf("[MEM DIFF] mul_u8 case %d  $%04X  ref=$%02X native=$%02X\n",
                               t, i, ref_mem[i], mem[i]); printed++;
                    }
        }
        if (cpu.A != ref_cpu.A) cpu_fail++;
    }
    printf("mul_u8: %d cases, %d mem mismatch, %d cpu(A) mismatch (both must be 0)\n",
           N, mem_fail, cpu_fail);
    return mem_fail + cpu_fail;
}

/* --- bin_to_bcd @ $4E84: binary A -> packed BCD.  Contract is mem[] (the ones
 * digit lands in $00C1) AND cpu.A (the packed BCD, consumed directly by callers)
 * AND cpu.Y (the tens digit, used as an index by setup_initials_ptr).  Entry A is
 * fully randomized 0..255 to cover the >= 100 truncation path. --- */
static int test_bin_to_bcd(void) {
    if (!want("bin_to_bcd")) return 0;
    enum { N = 20000 };
    static uint8_t pre[65536], ref_mem[65536];
    int mem_fail = 0, cpu_fail = 0, printed = 0;

    for (int t = 0; t < N; t++) {
        fill_random(pre);
        Cpu6502 c = zero_cpu();
        c.A = (uint8_t)(xs() & 0xFF);

        memcpy((void *)mem, pre, 65536); cpu = c;
        bin_to_bcd__t6502();
        memcpy(ref_mem, (void *)mem, sizeof ref_mem);
        Cpu6502 ref_cpu = cpu;

        memcpy((void *)mem, pre, 65536); cpu = c;
        bin_to_bcd();

        if (memcmp((const void *)mem, ref_mem, 65536) != 0) {
            mem_fail++;
            if (printed < 12)
                for (int i = 0; i < 65536 && printed < 12; i++)
                    if (mem[i] != ref_mem[i]) {
                        printf("[MEM DIFF] bin_to_bcd case %d  $%04X  ref=$%02X native=$%02X\n",
                               t, i, ref_mem[i], mem[i]); printed++;
                    }
        }
        if (cpu.A != ref_cpu.A || cpu.Y != ref_cpu.Y) cpu_fail++;
    }
    printf("bin_to_bcd: %d cases, %d mem mismatch, %d cpu(A/Y) mismatch (both must be 0)\n",
           N, mem_fail, cpu_fail);
    return mem_fail + cpu_fail;
}

/* --- random_terrain_height @ $6B47: result is cpu.A only (no mem writes), driven
 * by 1-2 POKEY RANDOM reads.  Seed the RANDOM LFSR identically per case (as
 * diff_run does) so both runs see the same stream; assert cpu.A. --- */
static int test_random_terrain_height(void) {
    if (!want("random_terrain_height")) return 0;
    enum { N = 50000 };
    static uint8_t pre[65536], ref_mem[65536];
    int mem_fail = 0, cpu_fail = 0, printed = 0;

    for (int t = 0; t < N; t++) {
        fill_random(pre);
        uint32_t rseed = (xs() & 0x1FFFF); if (rseed == 0) rseed = 1;
        Cpu6502 c = zero_cpu();

        platform_test_seed_rng(rseed);
        memcpy((void *)mem, pre, 65536); cpu = c;
        random_terrain_height__t6502();
        memcpy(ref_mem, (void *)mem, sizeof ref_mem);
        Cpu6502 ref_cpu = cpu;

        platform_test_seed_rng(rseed);
        memcpy((void *)mem, pre, 65536); cpu = c;
        random_terrain_height();

        if (memcmp((const void *)mem, ref_mem, 65536) != 0) {
            mem_fail++;
            if (printed < 12)
                for (int i = 0; i < 65536 && printed < 12; i++)
                    if (mem[i] != ref_mem[i]) {
                        printf("[MEM DIFF] random_terrain_height case %d  $%04X  ref=$%02X native=$%02X\n",
                               t, i, ref_mem[i], mem[i]); printed++;
                    }
        }
        if (cpu.A != ref_cpu.A) cpu_fail++;
    }
    printf("random_terrain_height: %d cases, %d mem mismatch, %d cpu(A) mismatch (both must be 0)\n",
           N, mem_fail, cpu_fail);
    return mem_fail + cpu_fail;
}

/* --- generic A-returning leaf test: random mem[] + seeded RANDOM stream, assert
 * both mem[] (catches stray writes) AND cpu.A (the return value the caller reads).
 * Fits leaves whose observable result is cpu.A and that take no entry registers. --- */
static int test_ret_a(const char *name, void (*native)(void), void (*t6502)(void)) {
    if (!want(name)) return 0;
    enum { N = 50000 };
    static uint8_t pre[65536], ref_mem[65536];
    int mem_fail = 0, cpu_fail = 0, printed = 0;

    for (int t = 0; t < N; t++) {
        fill_random(pre);
        uint32_t rseed = (xs() & 0x1FFFF); if (rseed == 0) rseed = 1;
        Cpu6502 c = zero_cpu();
        c.A = (uint8_t)(xs() & 0xFF);   /* entry A matters for some (e.g. rng_signed_jitter); */
        c.X = (uint8_t)(xs() & 0xFF);   /* the RANDOM-only leaves ignore entry regs, so seeding */
        c.Y = (uint8_t)(xs() & 0xFF);   /* is harmless for them and diffed identically in both runs. */
        c.C = (uint8_t)(xs() & 1);

        platform_test_seed_rng(rseed);
        memcpy((void *)mem, pre, 65536); cpu = c;
        t6502();
        memcpy(ref_mem, (void *)mem, sizeof ref_mem);
        Cpu6502 ref_cpu = cpu;

        platform_test_seed_rng(rseed);
        memcpy((void *)mem, pre, 65536); cpu = c;
        native();

        if (memcmp((const void *)mem, ref_mem, 65536) != 0) {
            mem_fail++;
            if (printed < 12)
                for (int i = 0; i < 65536 && printed < 12; i++)
                    if (mem[i] != ref_mem[i]) {
                        printf("[MEM DIFF] %s case %d  $%04X  ref=$%02X native=$%02X\n",
                               name, t, i, ref_mem[i], mem[i]); printed++;
                    }
        }
        if (cpu.A != ref_cpu.A) cpu_fail++;
    }
    printf("%s: %d cases, %d mem mismatch, %d cpu(A) mismatch (both must be 0)\n",
           name, N, mem_fail, cpu_fail);
    return mem_fail + cpu_fail;
}

/* --- generic test for emit_bcd_byte_digits-chain wrappers (render_bcd_low_bytes,
 * set_zsupp_pos_clear_delta): they plot digits through plot_char_bounded via the dest
 * pointer $00C5/$00C6, so seed it into $2000; entry A/X/Y are randomized.  The PHA in
 * emit_bcd_byte_digits scribbles the stack, neutralized before compare.  Assert mem[] +
 * cpu.A/X/Y. --- */
static int test_emit_chain(const char *name, void (*native)(void), void (*t6502)(void)) {
    if (!want(name)) return 0;
    enum { N = 50000 };
    static uint8_t pre[65536], ref_mem[65536];
    int mem_fail = 0, cpu_fail = 0, printed = 0;
    for (int t = 0; t < N; t++) {
        fill_random(pre);
        pre[0x00C5] = 0x00; pre[0x00C6] = 0x20;        /* dest ptr -> $2000 */
        Cpu6502 c = zero_cpu();
        c.A = (uint8_t)(xs() & 0xFF);
        c.X = (uint8_t)(xs() & 1);
        c.Y = (uint8_t)(xs() & 0xFF);

        memcpy((void *)mem, pre, 65536); cpu = c;
        t6502();
        memcpy(ref_mem, (void *)mem, sizeof ref_mem);
        Cpu6502 ref_cpu = cpu;

        memcpy((void *)mem, pre, 65536); cpu = c;
        native();

        for (int i = 0x0100; i <= 0x01FF; i++) ref_mem[i] = mem[i];
        if (memcmp((const void *)mem, ref_mem, 65536) != 0) {
            mem_fail++;
            if (printed < 12)
                for (int i = 0; i < 65536 && printed < 12; i++)
                    if (mem[i] != ref_mem[i]) {
                        printf("[MEM DIFF] %s case %d  $%04X  ref=$%02X native=$%02X\n",
                               name, t, i, ref_mem[i], mem[i]); printed++;
                    }
        }
        if (cpu.A != ref_cpu.A || cpu.X != ref_cpu.X || cpu.Y != ref_cpu.Y) cpu_fail++;
    }
    printf("%s: %d cases, %d mem mismatch, %d cpu(A/X/Y) mismatch (both must be 0)\n",
           name, N, mem_fail, cpu_fail);
    return mem_fail + cpu_fail;
}

/* --- plot_char_bounded @ $49D9: zero-suppress digit plotter.  Writes one char through
 * the dest pointer $00C5/$00C6, so seed it into safe bitmap RAM ($2000); entry A (digit),
 * X (suppress flag), Y (column) and the threshold $0619 are randomized.  The observable
 * result is mem[] AND the returned cpu.X (suppress flag) / cpu.A / cpu.Y, so assert all. --- */
static int test_plot_char_bounded(void) {
    if (!want("plot_char_bounded")) return 0;
    enum { N = 50000 };
    static uint8_t pre[65536], ref_mem[65536];
    int mem_fail = 0, cpu_fail = 0, printed = 0;

    for (int t = 0; t < N; t++) {
        fill_random(pre);
        pre[0x00C5] = 0x00; pre[0x00C6] = 0x20;        /* dest ptr -> $2000 (safe RAM) */
        Cpu6502 c = zero_cpu();
        c.A = (uint8_t)(xs() % 0x0A);                  /* digit 0-9 */
        c.X = (uint8_t)(xs() & 1);                     /* suppress flag 0/1 */
        c.Y = (uint8_t)(xs() & 0xFF);                  /* column (any; $2000+Y stays in RAM) */

        memcpy((void *)mem, pre, 65536); cpu = c;
        plot_char_bounded__t6502();
        memcpy(ref_mem, (void *)mem, sizeof ref_mem);
        Cpu6502 ref_cpu = cpu;

        memcpy((void *)mem, pre, 65536); cpu = c;
        plot_char_bounded();

        if (memcmp((const void *)mem, ref_mem, 65536) != 0) {
            mem_fail++;
            if (printed < 12)
                for (int i = 0; i < 65536 && printed < 12; i++)
                    if (mem[i] != ref_mem[i]) {
                        printf("[MEM DIFF] plot_char_bounded case %d  $%04X  ref=$%02X native=$%02X\n",
                               t, i, ref_mem[i], mem[i]); printed++;
                    }
        }
        if (cpu.A != ref_cpu.A || cpu.X != ref_cpu.X || cpu.Y != ref_cpu.Y) cpu_fail++;
    }
    printf("plot_char_bounded: %d cases, %d mem mismatch, %d cpu(A/X/Y) mismatch (both must be 0)\n",
           N, mem_fail, cpu_fail);
    return mem_fail + cpu_fail;
}

/* --- plot_terrain_span @ $692A: run of vertical spans via fill_vertical_span (plots
 * through the row-addr table $073D/$0793).  Fixture: table -> $2000 + row*$28; a FIXED
 * row window $009F=$08..$009E=$28 (valid table indices, unchanged during the inner loop);
 * column starts $009C/$009D mid-range; the span count $6E0F[Y] bounded small (1..16) to
 * keep the column walk in-row and the test fast.  Both runs share it. --- */
static int test_plot_terrain_span(void) {
    if (!want("plot_terrain_span")) return 0;
    enum { N = 4000 };
    static uint8_t pre[65536];
    int mem_fail = 0, cpu_diff = 0, printed = 0;
    for (int t = 0; t < N; t++) {
        fill_random(pre);
        for (int i = 0; i <= 0x54; i++) {
            uint16_t a = (uint16_t)(0x2000 + i * 0x28);
            pre[0x073D + i] = (uint8_t)a; pre[0x0793 + i] = (uint8_t)(a >> 8);
        }
        pre[0x009F] = 0x08; pre[0x009E] = 0x28;       /* fixed row window (valid indices) */
        pre[0x009C] = 0x60; pre[0x009D] = 0x10;       /* column starts */
        Cpu6502 c = zero_cpu();
        c.Y = (uint8_t)(xs() % 0x21);                 /* entry Y 0..$20 (indexes $6E0F) */
        pre[0x6E0F + c.Y] = (uint8_t)(1 + (xs() % 16));/* span count 1..16 */
        mem_fail += diff_run("plot_terrain_span", pre, c,
                             plot_terrain_span, plot_terrain_span__t6502, t, &printed, &cpu_diff);
    }
    printf("plot_terrain_span: %d cases, %d mem mismatch (must be 0), %d cpu diffs\n", N, mem_fail, cpu_diff);
    return mem_fail;
}

/* --- game_sub_6811 @ $6811: scatter random dots.  Plots through set_row_ptr (row-addr
 * table $073D/$0793 -> $80/$81) + plot_pixel_masked, so seed the table into safe bitmap
 * RAM ($2000 + row*$28); the jittered row stays within 0..$54 by construction.  diff_run
 * seeds the RANDOM stream identically for both runs. --- */
static int test_game_sub_6811(void) {
    if (!want("game_sub_6811")) return 0;
    enum { N = 4000 };
    static uint8_t pre[65536];
    int mem_fail = 0, cpu_diff = 0, printed = 0;
    for (int t = 0; t < N; t++) {
        fill_random(pre);
        for (int i = 0; i <= 0x54; i++) {
            uint16_t a = (uint16_t)(0x2000 + i * 0x28);
            pre[0x073D + i] = (uint8_t)a; pre[0x0793 + i] = (uint8_t)(a >> 8);
        }
        mem_fail += diff_run("game_sub_6811", pre, zero_cpu(),
                             game_sub_6811, game_sub_6811__t6502, t, &printed, &cpu_diff);
    }
    printf("game_sub_6811: %d cases, %d mem mismatch (must be 0), %d cpu diffs\n", N, mem_fail, cpu_diff);
    return mem_fail;
}

/* --- pixel/span plotter leaves (plot_glyph_pixel_masked, plot_pixel_masked, plot_pixel_2bpp,
 * fill_vertical_span, fill_horizontal_span).  Their real contract is plotting into the GTIA
 * screen field — through the $80/$81 row pointer or the $073D/$0793 addr table, both pointing
 * into bitmap RAM.  A fully-random mem[] instead aims them at random addresses (the $D000
 * hardware range, and self-clobbering zero page), which the faithful per-pixel bus path
 * reproduces but the idiomatic native cores (direct mem[], hoisted row bases) cannot — and
 * which never happens in the real game.  So seed a safe fixture, exactly as test_plot_terrain_
 * span / test_draw_symmetric_span_loop do.  Only mem[] is contracted (cpu exit is incidental). */
static void seed_row_addr_table(uint8_t *pre) {
    for (int i = 0; i <= 0x54; i++) {
        uint16_t a = (uint16_t)(0x2000 + i * 0x28);      /* mode-F layout: $2000 + row*40 */
        pre[0x073D + i] = (uint8_t)a; pre[0x0793 + i] = (uint8_t)(a >> 8);
    }
}
static int test_fill_vertical_span(void) {
    if (!want("fill_vertical_span")) return 0;
    enum { N = 8000 };
    static uint8_t pre[65536];
    int mem_fail = 0, cpu_diff = 0, printed = 0;
    for (int t = 0; t < N; t++) {
        fill_random(pre);
        seed_row_addr_table(pre);
        pre[0x009F] = 0x08; pre[0x009E] = 0x28;          /* row window r0<=r1 (valid indices) */
        pre[0x009C] = (uint8_t)(xs() % 0x60);            /* left/right columns within the row */
        pre[0x009D] = (uint8_t)(xs() % 0x60);
        pre[0x0094] = (uint8_t)(xs() & 0x0F);            /* mask selector */
        mem_fail += diff_run("fill_vertical_span", pre, zero_cpu(),
                             fill_vertical_span, fill_vertical_span__t6502, t, &printed, &cpu_diff);
    }
    printf("fill_vertical_span: %d cases, %d mem mismatch (must be 0), %d cpu diffs\n", N, mem_fail, cpu_diff);
    return mem_fail;
}
static int test_fill_horizontal_span(void) {
    if (!want("fill_horizontal_span")) return 0;
    enum { N = 8000 };
    static uint8_t pre[65536];
    int mem_fail = 0, cpu_diff = 0, printed = 0;
    for (int t = 0; t < N; t++) {
        fill_random(pre);
        seed_row_addr_table(pre);
        pre[0x009E] = 0x28; pre[0x009F] = 0x08;          /* top/bottom row indices */
        pre[0x009C] = 0x10; pre[0x009D] = 0x60;          /* lo<=hi columns */
        pre[0x00B9] = (uint8_t)xs();                     /* fill pattern */
        mem_fail += diff_run("fill_horizontal_span", pre, zero_cpu(),
                             fill_horizontal_span, fill_horizontal_span__t6502, t, &printed, &cpu_diff);
    }
    printf("fill_horizontal_span: %d cases, %d mem mismatch (must be 0), %d cpu diffs\n", N, mem_fail, cpu_diff);
    return mem_fail;
}
static int test_plot_glyph_pixel_masked(void) {
    if (!want("plot_glyph_pixel_masked")) return 0;
    enum { N = 20000 };
    static uint8_t pre[65536];
    int mem_fail = 0, cpu_diff = 0, printed = 0;
    for (int t = 0; t < N; t++) {
        fill_random(pre);
        pre[0x0080] = 0x00; pre[0x0081] = 0x20;          /* row ptr -> $2000 (bitmap RAM) */
        Cpu6502 c = zero_cpu();
        c.Y = (uint8_t)(xs() & 0x3F);                    /* byte offset 0..$3F */
        c.X = (uint8_t)(xs() & 0x1F);                    /* mask index 0..$1F */
        mem_fail += diff_run("plot_glyph_pixel_masked", pre, c,
                             plot_glyph_pixel_masked, plot_glyph_pixel_masked__t6502, t, &printed, &cpu_diff);
    }
    printf("plot_glyph_pixel_masked: %d cases, %d mem mismatch (must be 0), %d cpu diffs\n", N, mem_fail, cpu_diff);
    return mem_fail;
}
static int test_plot_pixel_masked(void) {
    if (!want("plot_pixel_masked")) return 0;
    enum { N = 20000 };
    static uint8_t pre[65536];
    int mem_fail = 0, cpu_diff = 0, printed = 0;
    for (int t = 0; t < N; t++) {
        fill_random(pre);
        pre[0x0080] = 0x00; pre[0x0081] = 0x20;
        pre[0x0094] = (uint8_t)(xs() & 0x1F);            /* mask selector */
        Cpu6502 c = zero_cpu();
        c.A = (uint8_t)(xs() & 0x7F);                    /* column 0..$7F -> byte offset 0..$3F */
        mem_fail += diff_run("plot_pixel_masked", pre, c,
                             plot_pixel_masked, plot_pixel_masked__t6502, t, &printed, &cpu_diff);
    }
    printf("plot_pixel_masked: %d cases, %d mem mismatch (must be 0), %d cpu diffs\n", N, mem_fail, cpu_diff);
    return mem_fail;
}
static int test_plot_pixel_2bpp(void) {
    if (!want("plot_pixel_2bpp")) return 0;
    enum { N = 20000 };
    static uint8_t pre[65536];
    static uint16_t stack_pg[256];
    for (int i = 0; i < 256; i++) stack_pg[i] = (uint16_t)(0x0100 + i);
    int mem_fail = 0, cpu_diff = 0, printed = 0;
    set_ignore(stack_pg, 256);                           /* oracle PHA/PLAs X through the stack */
    for (int t = 0; t < N; t++) {
        fill_random(pre);
        pre[0x0080] = 0x00; pre[0x0081] = 0x20;          /* row ptr -> $2000 */
        Cpu6502 c = zero_cpu();
        c.Y = (uint8_t)(xs() & 0x3F);
        c.X = (uint8_t)xs();
        c.C = (uint8_t)(xs() & 1);
        mem_fail += diff_run("plot_pixel_2bpp", pre, c,
                             plot_pixel_2bpp, plot_pixel_2bpp__t6502, t, &printed, &cpu_diff);
    }
    set_ignore(0, 0);
    printf("plot_pixel_2bpp: %d cases, %d mem mismatch (must be 0), %d cpu diffs\n", N, mem_fail, cpu_diff);
    return mem_fail;
}

/* --- sfx_engine_reset @ $5433: clears the music/voice state tables and seeds a few
 * slots/timers.  The 6502 STA $D1FF,X (indexed) writes are rendered by the transpiler as
 * DIRECT mem[] stores, but the native twin routes POKEY writes through bus_write (-> Paula
 * on Amiga), so the four AUDF addresses $D201/$D203/$D205/$D207 are excluded from the
 * contract (same masking the sfx engine uses for its indexed POKEY writes). --- */
static int test_sfx_engine_reset(void) {
    if (!want("sfx_engine_reset")) return 0;
    enum { N = 20000 };
    static uint8_t pre[65536];
    static const uint16_t ignore[] = { 0xD201, 0xD203, 0xD205, 0xD207 };
    int mem_fail = 0, cpu_diff = 0, printed = 0;
    set_ignore(ignore, 4);
    for (int t = 0; t < N; t++) {
        fill_random(pre);
        mem_fail += diff_run("sfx_engine_reset", pre, zero_cpu(),
                             sfx_engine_reset, sfx_engine_reset__t6502, t, &printed, &cpu_diff);
    }
    set_ignore(0, 0);
    printf("sfx_engine_reset: %d cases, %d mem mismatch (must be 0), %d cpu diffs\n", N, mem_fail, cpu_diff);
    return mem_fail;
}

/* --- blit_glyph_8rows @ $678B: 8-row glyph blitter.  Needs a realistic fixture: the
 * row-addr table $073D/$0793 points into safe bitmap RAM ($2000 + row*$28); the row
 * index $0092 is mid-range so the up-walk ($2E/row) stays in RAM; the glyph source ptr
 * $0084/$0085 -> $3000 (disjoint from the write region); mask base $0095 and column base
 * $009C are bounded.  The PHA/PLA inner loop scribbles the stack page, so neutralize
 * $0100-$01FF before comparing.  Both runs share the fixture; result observed via mem[]. --- */
static int test_blit_glyph_8rows(void) {
    if (!want("blit_glyph_8rows")) return 0;
    enum { N = 8000 };
    static uint8_t pre[65536], ref_mem[65536];
    int mem_fail = 0, cpu_diff = 0, printed = 0;
    for (int t = 0; t < N; t++) {
        fill_random(pre);
        for (int i = 0; i <= 0x54; i++) {                 /* row addr table -> $2000 + row*$28 */
            uint16_t a = (uint16_t)(0x2000 + i * 0x28);
            pre[0x073D + i] = (uint8_t)a; pre[0x0793 + i] = (uint8_t)(a >> 8);
        }
        pre[0x0084] = 0x00; pre[0x0085] = 0x30;           /* glyph source -> $3000 */
        pre[0x0092] = (uint8_t)(0x20 + (xs() % 0x30));    /* row index $20..$4F */
        pre[0x0095] = (uint8_t)(xs() % 8);                /* mask base 0..7 */
        pre[0x009C] = (uint8_t)(xs() % 0x20);             /* column base 0..$1F */
        Cpu6502 c = zero_cpu();

        platform_test_seed_rng(1);
        memcpy((void *)mem, pre, 65536); cpu = c;
        blit_glyph_8rows__t6502();
        memcpy(ref_mem, (void *)mem, sizeof ref_mem);

        platform_test_seed_rng(1);
        memcpy((void *)mem, pre, 65536); cpu = c;
        blit_glyph_8rows();

        for (int i = 0x0100; i <= 0x01FF; i++) ref_mem[i] = mem[i];  /* neutralize stack page */
        if (memcmp((const void *)mem, ref_mem, 65536) != 0) {
            mem_fail++;
            if (printed < 12)
                for (int i = 0; i < 65536 && printed < 12; i++)
                    if (mem[i] != ref_mem[i]) {
                        printf("[MEM DIFF] blit_glyph_8rows case %d  $%04X  ref=$%02X native=$%02X\n",
                               t, i, ref_mem[i], mem[i]); printed++;
                    }
        }
        (void)cpu_diff;
    }
    printf("blit_glyph_8rows: %d cases, %d mem mismatch (must be 0)\n", N, mem_fail);
    return mem_fail;
}

/* --- generic test for blit_glyph_8rows-chain wrappers (set_coord_y_e0, glyph_ptr_from_index):
 * they set the glyph source ptr $0084/$0085 themselves then tail-call blit_glyph_8rows, which
 * plots through the row-addr table.  Seed the table -> $2000 + row*$28 and bound the blit's
 * inputs ($0092 row index, $0095 mask base, $009C column base); the glyph source they compute
 * points into $E0xx (random mem, read identically by both).  Neutralize the PHA stack scribble;
 * assert mem[]. --- */
static int test_blit_chain(const char *name, void (*native)(void), void (*t6502)(void)) {
    if (!want(name)) return 0;
    enum { N = 8000 };
    static uint8_t pre[65536], ref_mem[65536];
    int mem_fail = 0, printed = 0;
    for (int t = 0; t < N; t++) {
        fill_random(pre);
        for (int i = 0; i <= 0x54; i++) {
            uint16_t a = (uint16_t)(0x2000 + i * 0x28);
            pre[0x073D + i] = (uint8_t)a; pre[0x0793 + i] = (uint8_t)(a >> 8);
        }
        pre[0x0092] = (uint8_t)(0x20 + (xs() % 0x30));
        pre[0x0095] = (uint8_t)(xs() % 8);
        pre[0x009C] = (uint8_t)(xs() % 0x20);
        Cpu6502 c = zero_cpu();
        c.A = (uint8_t)(xs() & 0xFF);
        c.Y = (uint8_t)(xs() & 0xFF);

        memcpy((void *)mem, pre, 65536); cpu = c;
        t6502();
        memcpy(ref_mem, (void *)mem, sizeof ref_mem);

        memcpy((void *)mem, pre, 65536); cpu = c;
        native();

        for (int i = 0x0100; i <= 0x01FF; i++) ref_mem[i] = mem[i];
        if (memcmp((const void *)mem, ref_mem, 65536) != 0) {
            mem_fail++;
            if (printed < 12)
                for (int i = 0; i < 65536 && printed < 12; i++)
                    if (mem[i] != ref_mem[i]) {
                        printf("[MEM DIFF] %s case %d  $%04X  ref=$%02X native=$%02X\n",
                               name, t, i, ref_mem[i], mem[i]); printed++;
                    }
        }
    }
    printf("%s: %d cases, %d mem mismatch (must be 0)\n", name, N, mem_fail);
    return mem_fail;
}

/* --- draw_glyph_2rows @ $4099: 2x2 glyph draw through the dest pointer $00BB.  Seed it
 * into $2000 (writes land at $2000/$2001/$2030/$2031); entry A is the glyph index. --- */
static int test_draw_glyph_2rows(void) {
    if (!want("draw_glyph_2rows")) return 0;
    enum { N = 50000 };
    static uint8_t pre[65536];
    int mem_fail = 0, cpu_diff = 0, printed = 0;
    for (int t = 0; t < N; t++) {
        fill_random(pre);
        pre[0x00BB] = 0x00; pre[0x00BC] = 0x20;        /* dest ptr -> $2000 */
        Cpu6502 c = zero_cpu();
        c.A = (uint8_t)(xs() & 0xFF);
        mem_fail += diff_run("draw_glyph_2rows", pre, c,
                             draw_glyph_2rows, draw_glyph_2rows__t6502, t, &printed, &cpu_diff);
    }
    printf("draw_glyph_2rows: %d cases, %d mem mismatch (must be 0), %d cpu diffs\n", N, mem_fail, cpu_diff);
    return mem_fail;
}

/* --- draw_digit_low_nibble @ $4095: A=(A&$0F)<<2 then draw_glyph_2rows through the
 * dest pointer $00BB -> seed it to $2000; entry A random. --- */
static int test_draw_digit_low_nibble(void) {
    if (!want("draw_digit_low_nibble")) return 0;
    enum { N = 50000 };
    static uint8_t pre[65536];
    int mem_fail = 0, cpu_diff = 0, printed = 0;
    for (int t = 0; t < N; t++) {
        fill_random(pre);
        pre[0x00BB] = 0x00; pre[0x00BC] = 0x20;        /* dest ptr -> $2000 */
        Cpu6502 c = zero_cpu();
        c.A = (uint8_t)(xs() & 0xFF);
        mem_fail += diff_run("draw_digit_low_nibble", pre, c,
                             draw_digit_low_nibble, draw_digit_low_nibble__t6502, t, &printed, &cpu_diff);
    }
    printf("draw_digit_low_nibble: %d cases, %d mem mismatch (must be 0), %d cpu diffs\n", N, mem_fail, cpu_diff);
    return mem_fail;
}

/* --- draw_2digit_value @ $4084: draws the high nibble at dest $BB ($2000), repoints
 * $BB <- $BD/$BE, then draws the low nibble there -> seed $BD/$BE to $2100 so both 2x2
 * draws land in disjoint safe RAM.  The PHA/PLA that preserves the entry byte scribbles
 * $01FF (masked); entry A random. --- */
static int test_draw_2digit_value(void) {
    if (!want("draw_2digit_value")) return 0;
    enum { N = 50000 };
    static uint8_t pre[65536];
    static const uint16_t ignore[] = { 0x01FF };
    int mem_fail = 0, cpu_diff = 0, printed = 0;
    set_ignore(ignore, 1);
    for (int t = 0; t < N; t++) {
        fill_random(pre);
        pre[0x00BB] = 0x00; pre[0x00BC] = 0x20;        /* hi-nibble dest -> $2000 */
        pre[0x00BD] = 0x00; pre[0x00BE] = 0x21;        /* lo-nibble dest -> $2100 */
        Cpu6502 c = zero_cpu();
        c.A = (uint8_t)(xs() & 0xFF);
        mem_fail += diff_run("draw_2digit_value", pre, c,
                             draw_2digit_value, draw_2digit_value__t6502, t, &printed, &cpu_diff);
    }
    set_ignore(0, 0);
    printf("draw_2digit_value: %d cases, %d mem mismatch (must be 0), %d cpu diffs\n", N, mem_fail, cpu_diff);
    return mem_fail;
}

/* --- unpack_terrain_seed_cols @ $7558: runs native rle_expand_list twice from the fixed
 * source addrs $4DFA / $4E09 (it sets all ptrs itself).  Seed a short terminating
 * (count,value) list at each source so the shared native expander returns quickly; the
 * expansion itself is already covered by test_rle_expand_list.  Result via mem[]. --- */
static int test_unpack_terrain_seed_cols(void) {
    if (!want("unpack_terrain_seed_cols")) return 0;
    enum { N = 20000 };
    static uint8_t pre[65536];
    int mem_fail = 0, cpu_diff = 0, printed = 0;
    for (int t = 0; t < N; t++) {
        fill_random(pre);
        int p = 0x4DFA, n1 = 1 + (int)(xs() % 3);      /* <=6 bytes, ends before $4E09 */
        for (int k = 0; k < n1; k++) { pre[p++] = (uint8_t)(1 + (xs() % 16)); pre[p++] = (uint8_t)(xs() & 0xFF); }
        pre[p] = 0x00;
        p = 0x4E09; int n2 = 1 + (int)(xs() % 3);
        for (int k = 0; k < n2; k++) { pre[p++] = (uint8_t)(1 + (xs() % 16)); pre[p++] = (uint8_t)(xs() & 0xFF); }
        pre[p] = 0x00;
        mem_fail += diff_run("unpack_terrain_seed_cols", pre, zero_cpu(),
                             unpack_terrain_seed_cols, unpack_terrain_seed_cols__t6502, t, &printed, &cpu_diff);
    }
    printf("unpack_terrain_seed_cols: %d cases, %d mem mismatch (must be 0), %d cpu diffs\n", N, mem_fail, cpu_diff);
    return mem_fail;
}

/* --- game_init_7588 @ $7588: $AA fill of $32FD..$332C, then native rle_decompress from
 * $6E6E -> $332D.  Seed a short terminating literal/run stream at $6E6E (ending in a $C0
 * marker) to bound the shared native decompressor.  Result via mem[]. --- */
static int test_game_init_7588(void) {
    if (!want("game_init_7588")) return 0;
    enum { N = 20000 };
    static uint8_t pre[65536];
    int mem_fail = 0, cpu_diff = 0, printed = 0;
    for (int t = 0; t < N; t++) {
        fill_random(pre);
        int p = 0x6E6E, nitems = 1 + (int)(xs() % 4);
        for (int k = 0; k < nitems; k++) {
            if (xs() & 1) { pre[p++] = (uint8_t)(xs() % 0xC0); }
            else { pre[p++] = (uint8_t)(0xC0 | (1 + (xs() % 16))); pre[p++] = (uint8_t)(xs() & 0xFF); }
        }
        pre[p] = 0xC0;
        mem_fail += diff_run("game_init_7588", pre, zero_cpu(),
                             game_init_7588, game_init_7588__t6502, t, &printed, &cpu_diff);
    }
    printf("game_init_7588: %d cases, %d mem mismatch (must be 0), %d cpu diffs\n", N, mem_fail, cpu_diff);
    return mem_fail;
}

/* --- rle_run_fill @ $3C58: RLE run expansion through copy_bytes_to_dst.  Seed the
 * source pointer $00BB/$00BC -> $2000 and the dest pointer $00BD/$00BE -> $2400 (both
 * safe RAM, disjoint), a bounded run length (entry A 1..64) and small offset (entry Y),
 * so the up-to-64-byte fill stays in RAM.  Result is observed through mem[]. --- */
static int test_rle_run_fill(void) {
    if (!want("rle_run_fill")) return 0;
    enum { N = 30000 };
    static uint8_t pre[65536];
    int mem_fail = 0, cpu_diff = 0, printed = 0;
    for (int t = 0; t < N; t++) {
        fill_random(pre);
        pre[0x00BB] = 0x00; pre[0x00BC] = 0x20;        /* src -> $2000 */
        pre[0x00BD] = 0x00; pre[0x00BE] = 0x24;        /* dest -> $2400 */
        Cpu6502 c = zero_cpu();
        c.A = (uint8_t)(1 + (xs() % 64));              /* run length 1..64 */
        c.Y = (uint8_t)(xs() & 0x0F);                  /* offset 0..15 */
        mem_fail += diff_run("rle_run_fill", pre, c,
                             rle_run_fill, rle_run_fill__t6502, t, &printed, &cpu_diff);
    }
    printf("rle_run_fill: %d cases, %d mem mismatch (must be 0), %d cpu diffs\n", N, mem_fail, cpu_diff);
    return mem_fail;
}

/* --- emit_dl_coord_pairs @ $68CF: copies row-addr table entries into the $300A/$308B DL
 * regions then tail-calls plot_terrain_span.  Union fixture: row table -> $2000+row*$28;
 * bounded DL Y-indices ($00C3/$00C1) and table X-indices ($00C4/$00C2); small counts
 * ($6E0F[entry Y] and $6E0F[$00B9]); plot_terrain_span's fixed row window $08..$28 + column
 * starts.  PHA scribbles the stack (native uses none) -> neutralize $0100-$01FF.  Result via mem[]. --- */
static int test_emit_dl_coord_pairs(void) {
    if (!want("emit_dl_coord_pairs")) return 0;
    enum { N = 8000 };
    static uint8_t pre[65536], ref_mem[65536];
    int mem_fail = 0, printed = 0;
    for (int t = 0; t < N; t++) {
        fill_random(pre);
        for (int i = 0; i <= 0x54; i++) {
            uint16_t a = (uint16_t)(0x2000 + i * 0x28);
            pre[0x073D + i] = (uint8_t)a; pre[0x0793 + i] = (uint8_t)(a >> 8);
        }
        uint8_t ey = (uint8_t)(xs() % 0x10);         /* entry Y -> first count */
        uint8_t bv = (uint8_t)(xs() % 0x10);         /* $00B9 value -> step / span count */
        pre[0x6E0F + ey] = (uint8_t)(1 + (xs() % 6));
        pre[0x6E0F + bv] = (uint8_t)(1 + (xs() % 4));
        pre[0x00B9] = bv;
        pre[0x00C3] = 0x60; pre[0x00C4] = 0x40;      /* first loop Y(desc)/X(desc) */
        pre[0x00C1] = 0x08; pre[0x00C2] = 0x20;      /* second loop Y(asc)/X(asc) */
        pre[0x009F] = 0x08; pre[0x009E] = 0x28;      /* plot_terrain_span row window */
        pre[0x009C] = 0x60; pre[0x009D] = 0x10;
        Cpu6502 c = zero_cpu();
        c.Y = ey;

        memcpy((void *)mem, pre, 65536); cpu = c;
        emit_dl_coord_pairs__t6502();
        memcpy(ref_mem, (void *)mem, sizeof ref_mem);

        memcpy((void *)mem, pre, 65536); cpu = c;
        emit_dl_coord_pairs();

        for (int i = 0x0100; i <= 0x01FF; i++) ref_mem[i] = mem[i];
        if (memcmp((const void *)mem, ref_mem, 65536) != 0) {
            mem_fail++;
            if (printed < 12)
                for (int i = 0; i < 65536 && printed < 12; i++)
                    if (mem[i] != ref_mem[i]) {
                        printf("[MEM DIFF] emit_dl_coord_pairs case %d  $%04X  ref=$%02X native=$%02X\n",
                               t, i, ref_mem[i], mem[i]); printed++;
                    }
        }
    }
    printf("emit_dl_coord_pairs: %d cases, %d mem mismatch (must be 0)\n", N, mem_fail);
    return mem_fail;
}

/* --- compute_stage_display_geometry @ $75F5: derive the gauge param block from
 * $006D over fully randomized mem[] (so X = $006D spans 0..255, covering every
 * clamp branch, and any stray write shows as a diff).  Calls the native bin_to_bcd,
 * whose PHA/PLA in the 6502 oracle leaves a dead byte at $01FF (S=$FF) that the
 * native twin doesn't reproduce — masked. */
static int test_compute_stage_display_geometry(void) {
    if (!want("compute_stage_display_geometry")) return 0;
    enum { N = 20000 };
    static uint8_t pre[65536];
    static const uint16_t ignore[] = { 0x01FF };
    int mem_fail = 0, cpu_diff = 0, printed = 0;
    set_ignore(ignore, 1);
    for (int t = 0; t < N; t++) {
        fill_random(pre);
        mem_fail += diff_run("compute_stage_display_geometry", pre, zero_cpu(),
                             compute_stage_display_geometry,
                             compute_stage_display_geometry__t6502, t, &printed, &cpu_diff);
    }
    set_ignore(0, 0);
    printf("compute_stage_display_geometry: %d cases, %d mem mismatch (must be 0), %d cpu diffs\n",
           N, mem_fail, cpu_diff);
    return mem_fail;
}

/* --- dl_lms_fill @ $69F1: writes pairs into ($C5/$C6)+Y, so seed the dest ptr to safe
 * DL RAM ($3000) and bound the X span ($008B..$0086, small forward count) so Y/dest stay
 * within $3000+ and the loop is short.  $073D/$0793 source random; $008B is 0 in some
 * cases (-> the ret_stub tail) and nonzero in others (-> native shift_object_table_up). */
static int test_dl_lms_fill(void) {
    if (!want("dl_lms_fill")) return 0;
    enum { N = 20000 };
    static uint8_t pre[65536];
    int mem_fail = 0, cpu_diff = 0, printed = 0;
    for (int t = 0; t < N; t++) {
        fill_random(pre);
        pre[0x00C5] = 0x00; pre[0x00C6] = 0x30;             /* dest ptr -> $3000 */
        uint8_t start = (uint8_t)(xs() % 0x20);             /* $008B (0 some cases) */
        uint8_t count = (uint8_t)(1 + (xs() % 0x20));       /* 1..32 X-iterations */
        pre[0x008B] = start;
        pre[0x0086] = (uint8_t)(start + count);             /* forward end (no wrap) */
        mem_fail += diff_run("dl_lms_fill", pre, zero_cpu(),
                             dl_lms_fill, dl_lms_fill__t6502, t, &printed, &cpu_diff);
    }
    printf("dl_lms_fill: %d cases, %d mem mismatch (must be 0), %d cpu diffs\n", N, mem_fail, cpu_diff);
    return mem_fail;
}

/* --- draw_scaled_shape @ $7C9A: 2D scaled blitter with divide-by-subtraction + nested
 * accum loops whose bounds come from the step $0051:$0050 (zero step -> infinite loops) and
 * the $7DA9/$7DBB/$7DD3/$7DA5 tables + ($C3) mask pointer.  Seed from the flight snapshot for
 * the real tables, and force a nonzero step ($0051 in 1..4, $0050 random) so every loop
 * terminates quickly; plot_clipped_pixel clips OOB.  Both runs share the native plotter and
 * the same seed, so results match regardless of where ($C3) points. --- */
static int test_draw_scaled_shape(void) {
    if (!want("draw_scaled_shape")) return 0;
    static uint8_t snap[65536], pre[65536];
    const char *path = "a800dumps/flight_ram_0000_BFFF.bin";
    FILE *f = fopen(path, "rb");
    if (!f) { printf("draw_scaled_shape: SKIP (%s not found)\n", path); return 0; }
    memset(snap, 0, sizeof snap);
    size_t got = fread(snap, 1, 0xC000, f); fclose(f);
    if (got != 0xC000) { printf("draw_scaled_shape: SKIP (short read %zu)\n", got); return 0; }
    int mem_fail = 0, cpu_diff = 0, printed = 0;
    for (int t = 0; t < 8000; t++) {
        memcpy(pre, snap, sizeof pre);
        pre[0x0050] = (uint8_t)(xs() & 0xFF);
        pre[0x0051] = (uint8_t)(1 + (xs() % 4));        /* nonzero step -> bounded loops */
        mem_fail += diff_run("draw_scaled_shape", pre, zero_cpu(),
                             draw_scaled_shape, draw_scaled_shape__t6502, t, &printed, &cpu_diff);
    }
    printf("draw_scaled_shape: %d cases (flight snapshot), %d mem mismatch (must be 0), %d cpu diffs\n",
           8000, mem_fail, cpu_diff);
    return mem_fail;
}

/* --- validate_save_state @ $5D0D: a pure comparator (no mem writes) whose result is the Z
 * flag, so assert cpu.A/Z/C/N rather than mem.  Seed the header bytes ($3700=$28, $3714=$EE)
 * and copy the 38-byte reference into the candidate region in ~half the cases each, so both
 * the early-out and full-match (Z set) paths are exercised. --- */
static int test_validate_save_state(void) {
    if (!want("validate_save_state")) return 0;
    enum { N = 50000 };
    static uint8_t pre[65536], ref_mem[65536];
    int mem_fail = 0, cpu_fail = 0, printed = 0; (void)printed;
    for (int t = 0; t < N; t++) {
        fill_random(pre);
        if (xs() & 1) pre[0x3700] = 0x28;
        if (xs() & 1) pre[0x3714] = 0xEE;
        if (xs() & 1) for (int i = 1; i <= 0x26; i++) pre[0x37C7 + i] = pre[0x7BDA + i];
        Cpu6502 c = zero_cpu();
        memcpy((void *)mem, pre, 65536); cpu = c; validate_save_state__t6502();
        Cpu6502 ref = cpu; memcpy(ref_mem, (void *)mem, sizeof ref_mem);
        memcpy((void *)mem, pre, 65536); cpu = c; validate_save_state();
        if (memcmp((const void *)mem, ref_mem, 65536) != 0) mem_fail++;
        if (cpu.A != ref.A || cpu.Z != ref.Z || cpu.C != ref.C || cpu.N != ref.N) cpu_fail++;
    }
    printf("validate_save_state: %d cases, %d mem mismatch, %d cpu(A/Z/C/N) mismatch (both must be 0)\n",
           N, mem_fail, cpu_fail);
    return mem_fail + cpu_fail;
}

/* --- cockpit_dial_update @ $4430: derives $0022 then tail-calls draw_cockpit_dial_bar, whose
 * draw loop needs the real $4581 table + valid bounds -> flight snapshot + forced $00BD=$0F/
 * $00BE=$07; vary entry A (the dial value, masked $3F).  The PHA/PLA leaves a dead $01FF. --- */
static int test_cockpit_dial_update(void) {
    if (!want("cockpit_dial_update")) return 0;
    static uint8_t snap[65536], pre[65536];
    static const uint16_t ignore[] = { 0x01FF };
    const char *path = "a800dumps/flight_ram_0000_BFFF.bin";
    FILE *f = fopen(path, "rb");
    if (!f) { printf("cockpit_dial_update: SKIP (%s not found)\n", path); return 0; }
    memset(snap, 0, sizeof snap);
    size_t got = fread(snap, 1, 0xC000, f); fclose(f);
    if (got != 0xC000) { printf("cockpit_dial_update: SKIP (short read %zu)\n", got); return 0; }
    int mem_fail = 0, cpu_diff = 0, printed = 0;
    set_ignore(ignore, 1);
    for (int t = 0; t < 20000; t++) {
        memcpy(pre, snap, sizeof pre);
        pre[0x00BD] = 0x0F; pre[0x00BE] = 0x07;
        Cpu6502 c = zero_cpu();
        c.A = (uint8_t)(xs() & 0x3F);
        mem_fail += diff_run("cockpit_dial_update", pre, c,
                             cockpit_dial_update, cockpit_dial_update__t6502, t, &printed, &cpu_diff);
    }
    set_ignore(0, 0);
    printf("cockpit_dial_update: %d cases (dial snapshot), %d mem mismatch (must be 0), %d cpu diffs\n",
           20000, mem_fail, cpu_diff);
    return mem_fail;
}

/* --- dl_lms_build @ $69E5: sets the DL dest ptr/end ($C5/$C6=$300A, $0086=$56) itself,
 * then dl_lms_fill, so only the X start $008B needs bounding -> seed it just below $56 so
 * the fill is a short forward run into safe DL RAM (and nonzero -> the shift tail). --- */
static int test_dl_lms_build(void) {
    if (!want("dl_lms_build")) return 0;
    enum { N = 20000 };
    static uint8_t pre[65536];
    int mem_fail = 0, cpu_diff = 0, printed = 0;
    for (int t = 0; t < N; t++) {
        fill_random(pre);
        pre[0x008B] = (uint8_t)(0x56 - (1 + (xs() % 0x20)));   /* forward run, 1..32 iters */
        mem_fail += diff_run("dl_lms_build", pre, zero_cpu(),
                             dl_lms_build, dl_lms_build__t6502, t, &printed, &cpu_diff);
    }
    printf("dl_lms_build: %d cases, %d mem mismatch (must be 0), %d cpu diffs\n", N, mem_fail, cpu_diff);
    return mem_fail;
}

/* --- draw_dial_bar_column @ $43CB: gates on entry Y then tail-calls draw_object_column,
 * whose $4581 pointer table must be real (random ptrs -> undiffable hang) -> seed from the
 * flight snapshot.  Vary entry Y (the dial value / threshold); A is set internally. --- */
static int test_draw_dial_bar_column(void) {
    if (!want("draw_dial_bar_column")) return 0;
    static uint8_t snap[65536], pre[65536];
    const char *path = "a800dumps/flight_ram_0000_BFFF.bin";
    FILE *f = fopen(path, "rb");
    if (!f) { printf("draw_dial_bar_column: SKIP (%s not found)\n", path); return 0; }
    memset(snap, 0, sizeof snap);
    size_t got = fread(snap, 1, 0xC000, f); fclose(f);
    if (got != 0xC000) { printf("draw_dial_bar_column: SKIP (short read %zu)\n", got); return 0; }
    int mem_fail = 0, cpu_diff = 0, printed = 0;
    for (int t = 0; t < 20000; t++) {
        memcpy(pre, snap, sizeof pre);
        Cpu6502 c = zero_cpu();
        c.Y = (uint8_t)(xs() & 0xFF);                       /* dial value / column threshold */
        mem_fail += diff_run("draw_dial_bar_column", pre, c,
                             draw_dial_bar_column, draw_dial_bar_column__t6502, t, &printed, &cpu_diff);
    }
    printf("draw_dial_bar_column: %d cases (dial snapshot), %d mem mismatch (must be 0), %d cpu diffs\n",
           20000, mem_fail, cpu_diff);
    return mem_fail;
}

/* --- draw_player3_object @ $42A7: the lock-on sprite drawer.  Reads many fixed tables
 * ($4566/$4569/$456C/$457A/$4D11/$4D3E/$4D3F + the $BB mask data) so seed from the flight
 * snapshot; vary entry A (the object selector — covers the A>=3 fixed path and the A<3
 * table path, plus the $03..$1F draw range after PLA) and $2826 (0 -> the RANDOM-dithered
 * mask source, !=0 -> the fixed source).  RANDOM ($D20A) is seeded per case by diff_run, so
 * both runs read the same stream regardless of where $BB points.  HW writes ($D00B/$D003) go
 * through bus_write in both.  The entry PHA/PLA leaves a dead $01FF byte the native skips. */
static int test_draw_player3_object(void) {
    if (!want("draw_player3_object")) return 0;
    static uint8_t snap[65536], pre[65536];
    static const uint16_t ignore[] = { 0x01FF };
    const char *path = "a800dumps/flight_ram_0000_BFFF.bin";
    FILE *f = fopen(path, "rb");
    if (!f) { printf("draw_player3_object: SKIP (%s not found)\n", path); return 0; }
    memset(snap, 0, sizeof snap);
    size_t got = fread(snap, 1, 0xC000, f); fclose(f);
    if (got != 0xC000) { printf("draw_player3_object: SKIP (short read %zu)\n", got); return 0; }
    int mem_fail = 0, cpu_diff = 0, printed = 0;
    set_ignore(ignore, 1);
    for (int t = 0; t < 20000; t++) {
        memcpy(pre, snap, sizeof pre);
        if (xs() & 1) pre[0x2826] = 0x00;            /* exercise the RANDOM mask source */
        Cpu6502 c = zero_cpu();
        c.A = (uint8_t)(xs() & 0xFF);                /* object selector */
        mem_fail += diff_run("draw_player3_object", pre, c,
                             draw_player3_object, draw_player3_object__t6502, t, &printed, &cpu_diff);
    }
    set_ignore(0, 0);
    printf("draw_player3_object: %d cases (flight snapshot), %d mem mismatch (must be 0), %d cpu diffs\n",
           20000, mem_fail, cpu_diff);
    return mem_fail;
}

/* --- intro_random_setup @ $6FBF: RANDOM-driven DFS maze generator on the $0900 map (+$2500
 * stack).  Self-contained (no entry regs / fixture); diff_run seeds the RANDOM stream so both
 * runs trace the same maze.  Fewer cases (each is hundreds of RANDOM reads). --- */
static int test_intro_random_setup(void) {
    if (!want("intro_random_setup")) return 0;
    enum { N = 4000 };
    static uint8_t pre[65536];
    int mem_fail = 0, cpu_diff = 0, printed = 0;
    for (int t = 0; t < N; t++) {
        fill_random(pre);
        mem_fail += diff_run("intro_random_setup", pre, zero_cpu(),
                             intro_random_setup, intro_random_setup__t6502, t, &printed, &cpu_diff);
    }
    printf("intro_random_setup: %d cases, %d mem mismatch (must be 0), %d cpu diffs\n", N, mem_fail, cpu_diff);
    return mem_fail;
}

/* --- unpack_bitmap_4d3e @ $74D7: bit-reversal unpacker through pointers read from the $4D3E
 * word table.  With a random table the pointers would be garbage (corrupt ZP / HW), so seed
 * EVERY table word to $2000: all reads/writes then stay in $2000..$207F (in-place reversal,
 * deterministic for both runs).  Input-less; result observed via mem[]. --- */
static int test_unpack_bitmap_4d3e(void) {
    if (!want("unpack_bitmap_4d3e")) return 0;
    enum { N = 4000 };
    static uint8_t pre[65536];
    int mem_fail = 0, cpu_diff = 0, printed = 0;
    for (int t = 0; t < N; t++) {
        fill_random(pre);
        for (int i = 0; i < 0x100; i++) pre[0x4D3E + i] = (i & 1) ? 0x20 : 0x00;  /* words -> $2000 */
        mem_fail += diff_run("unpack_bitmap_4d3e", pre, zero_cpu(),
                             unpack_bitmap_4d3e, unpack_bitmap_4d3e__t6502, t, &printed, &cpu_diff);
    }
    printf("unpack_bitmap_4d3e: %d cases, %d mem mismatch (must be 0), %d cpu diffs\n", N, mem_fail, cpu_diff);
    return mem_fail;
}

/* --- plot_clipped_pixel @ $7D38: clipped masked pixel.  Plots through row pointers from the
 * row-addr table $073D/$0793, so seed it -> $2000 + row*$28; the coords $004E/$004F are kept
 * inside the clip window and $00B3 high so the plot path runs; mask tables $4F3B/$7DEB and the
 * source $0058 are random.  Both runs share it; result observed via mem[]. --- */
static int test_plot_clipped_pixel(void) {
    if (!want("plot_clipped_pixel")) return 0;
    enum { N = 50000 };
    static uint8_t pre[65536];
    int mem_fail = 0, cpu_diff = 0, printed = 0;
    for (int t = 0; t < N; t++) {
        fill_random(pre);
        for (int i = 0; i <= 0x54; i++) {
            uint16_t a = (uint16_t)(0x2000 + i * 0x28);
            pre[0x073D + i] = (uint8_t)a; pre[0x0793 + i] = (uint8_t)(a >> 8);
        }
        pre[0x004E] = (uint8_t)(0x6C + (xs() % 0x2B));   /* Y in [$6C,$97) */
        pre[0x004F] = (uint8_t)(0x28 + (xs() % 0xB0));   /* X in [$28,$D8) */
        pre[0x00B3] = 0x80;                              /* high limit so col passes */
        Cpu6502 c = zero_cpu();
        c.A = (uint8_t)(xs() & 0xFF);                    /* source value $0058 */
        mem_fail += diff_run("plot_clipped_pixel", pre, c,
                             plot_clipped_pixel, plot_clipped_pixel__t6502, t, &printed, &cpu_diff);
    }
    printf("plot_clipped_pixel: %d cases, %d mem mismatch (must be 0), %d cpu diffs\n", N, mem_fail, cpu_diff);
    return mem_fail;
}

/* --- loader_util @ $3C00: clears three fixed regions via memset_or_copy.  Input-less and
 * deterministic, so a small N fully exercises it (random pre[] outside the cleared regions is
 * preserved identically by both runs). --- */
static int test_loader_util(void) {
    if (!want("loader_util")) return 0;
    enum { N = 500 };
    static uint8_t pre[65536];
    int mem_fail = 0, cpu_diff = 0, printed = 0;
    for (int t = 0; t < N; t++) {
        fill_random(pre);
        mem_fail += diff_run("loader_util", pre, zero_cpu(),
                             loader_util, loader_util__t6502, t, &printed, &cpu_diff);
    }
    printf("loader_util: %d cases, %d mem mismatch (must be 0), %d cpu diffs\n", N, mem_fail, cpu_diff);
    return mem_fail;
}

/* --- game_init_77DF @ $77DF: builds the $BE00/$BF00 bit tables.  Input-less; the PHA/PLA
 * scribbles the stack page (native uses no stack), so neutralize $0100-$01FF before compare. --- */
static int test_game_init_77DF(void) {
    if (!want("game_init_77DF")) return 0;
    enum { N = 2000 };
    static uint8_t pre[65536], ref_mem[65536];
    int mem_fail = 0, printed = 0;
    for (int t = 0; t < N; t++) {
        fill_random(pre);
        memcpy((void *)mem, pre, 65536); cpu = zero_cpu();
        game_init_77DF__t6502();
        memcpy(ref_mem, (void *)mem, sizeof ref_mem);

        memcpy((void *)mem, pre, 65536); cpu = zero_cpu();
        game_init_77DF();

        for (int i = 0x0100; i <= 0x01FF; i++) ref_mem[i] = mem[i];
        if (memcmp((const void *)mem, ref_mem, 65536) != 0) {
            mem_fail++;
            if (printed < 12)
                for (int i = 0; i < 65536 && printed < 12; i++)
                    if (mem[i] != ref_mem[i]) {
                        printf("[MEM DIFF] game_init_77DF case %d  $%04X  ref=$%02X native=$%02X\n",
                               t, i, ref_mem[i], mem[i]); printed++;
                    }
        }
    }
    printf("game_init_77DF: %d cases, %d mem mismatch (must be 0)\n", N, mem_fail);
    return mem_fail;
}

/* --- rle_expand_list @ $757B: expand a (count,value) run list.  Build a small stream at
 * $2000 (counts 1..16) ending in a 0 terminator; src ptr $BB -> $2000, dest ptr $BD -> $2800.
 * Both runs share it; result observed via mem[]. --- */
static int test_rle_expand_list(void) {
    if (!want("rle_expand_list")) return 0;
    enum { N = 20000 };
    static uint8_t pre[65536];
    int mem_fail = 0, cpu_diff = 0, printed = 0;
    for (int t = 0; t < N; t++) {
        fill_random(pre);
        int p = 0x2000, npairs = 1 + (int)(xs() % 6);
        for (int k = 0; k < npairs; k++) {
            pre[p++] = (uint8_t)(1 + (xs() % 16));   /* count 1..16 */
            pre[p++] = (uint8_t)(xs() & 0xFF);       /* value */
        }
        pre[p] = 0x00;                               /* terminator */
        pre[0x00BB] = 0x00; pre[0x00BC] = 0x20;
        pre[0x00BD] = 0x00; pre[0x00BE] = 0x28;
        mem_fail += diff_run("rle_expand_list", pre, zero_cpu(),
                             rle_expand_list, rle_expand_list__t6502, t, &printed, &cpu_diff);
    }
    printf("rle_expand_list: %d cases, %d mem mismatch (must be 0), %d cpu diffs\n", N, mem_fail, cpu_diff);
    return mem_fail;
}

/* --- rle_decompress @ $3C3D: literal/run RLE decompressor.  Build a small stream at $2000
 * mixing literals (<$C0) and runs ($C0|len, value; len 1..16), ending in a $C0 terminator;
 * src ptr $BB -> $2000, dest ptr $BD -> $2800.  Result observed via mem[]. --- */
static int test_rle_decompress(void) {
    if (!want("rle_decompress")) return 0;
    enum { N = 20000 };
    static uint8_t pre[65536];
    int mem_fail = 0, cpu_diff = 0, printed = 0;
    for (int t = 0; t < N; t++) {
        fill_random(pre);
        int p = 0x2000, nitems = 1 + (int)(xs() % 6);
        for (int k = 0; k < nitems; k++) {
            if (xs() & 1) {
                pre[p++] = (uint8_t)(xs() % 0xC0);              /* literal < $C0 */
            } else {
                pre[p++] = (uint8_t)(0xC0 | (1 + (xs() % 16))); /* run marker */
                pre[p++] = (uint8_t)(xs() & 0xFF);             /* value */
            }
        }
        pre[p] = 0xC0;                                          /* terminator (len 0) */
        pre[0x00BB] = 0x00; pre[0x00BC] = 0x20;
        pre[0x00BD] = 0x00; pre[0x00BE] = 0x28;
        mem_fail += diff_run("rle_decompress", pre, zero_cpu(),
                             rle_decompress, rle_decompress__t6502, t, &printed, &cpu_diff);
    }
    printf("rle_decompress: %d cases, %d mem mismatch (must be 0), %d cpu diffs\n", N, mem_fail, cpu_diff);
    return mem_fail;
}

/* --- emit_bcd_byte_digits @ $49CE: plots two digits via plot_char_bounded.  Same
 * dest-ptr ($00C5/$00C6 -> $2000) fixture as plot_char_bounded; entry A is a packed-BCD
 * byte, X the suppress flag, Y the column.  The 6502 PHA leaves a dead scribble on the
 * stack page (the native twin has no stack), so neutralize $0100-$01FF before comparing.
 * Assert mem[] + cpu.A/X/Y (X/Y carry forward to the caller's next digit). --- */
static int test_emit_bcd_byte_digits(void) {
    if (!want("emit_bcd_byte_digits")) return 0;
    enum { N = 50000 };
    static uint8_t pre[65536], ref_mem[65536];
    int mem_fail = 0, cpu_fail = 0, printed = 0;

    for (int t = 0; t < N; t++) {
        fill_random(pre);
        pre[0x00C5] = 0x00; pre[0x00C6] = 0x20;        /* dest ptr -> $2000 (safe RAM) */
        Cpu6502 c = zero_cpu();
        c.A = (uint8_t)(xs() & 0xFF);                  /* packed-BCD byte */
        c.X = (uint8_t)(xs() & 1);                     /* suppress flag */
        c.Y = (uint8_t)(xs() & 0xFF);                  /* column */

        memcpy((void *)mem, pre, 65536); cpu = c;
        emit_bcd_byte_digits__t6502();
        memcpy(ref_mem, (void *)mem, sizeof ref_mem);
        Cpu6502 ref_cpu = cpu;

        memcpy((void *)mem, pre, 65536); cpu = c;
        emit_bcd_byte_digits();

        for (int i = 0x0100; i <= 0x01FF; i++) ref_mem[i] = mem[i];  /* neutralize stack page */
        if (memcmp((const void *)mem, ref_mem, 65536) != 0) {
            mem_fail++;
            if (printed < 12)
                for (int i = 0; i < 65536 && printed < 12; i++)
                    if (mem[i] != ref_mem[i]) {
                        printf("[MEM DIFF] emit_bcd_byte_digits case %d  $%04X  ref=$%02X native=$%02X\n",
                               t, i, ref_mem[i], mem[i]); printed++;
                    }
        }
        if (cpu.A != ref_cpu.A || cpu.X != ref_cpu.X || cpu.Y != ref_cpu.Y) cpu_fail++;
    }
    printf("emit_bcd_byte_digits: %d cases, %d mem mismatch, %d cpu(A/X/Y) mismatch (both must be 0)\n",
           N, mem_fail, cpu_fail);
    return mem_fail + cpu_fail;
}

/* --- draw_symmetric_span_loop @ $6642: nested span fill, $0096 outer iterations.
 * The inner span fills write via the per-scanline address table ($073D lo/$0793 hi).
 * With fully random mem those pointers are garbage and the writes land on the routine's
 * own zero-page loop counters ($00DF/$0096) — hanging BOTH runs.  So seed a realistic
 * fixture: the address table points into safe bitmap RAM ($2000 + row*$28, the real
 * mode-F layout), the outer count is small, and the edge coordinates are bounded to the
 * table's valid row range.  Both runs share it, so the per-pass logic is fully diffed. --- */
static int test_draw_symmetric_span_loop(void) {
    if (!want("draw_symmetric_span_loop")) return 0;
    enum { N = 4000 };
    static uint8_t pre[65536];
    int mem_fail = 0, cpu_diff = 0, printed = 0;
    for (int t = 0; t < N; t++) {
        fill_random(pre);
        for (int i = 0; i <= 0x54; i++) {                 /* row addr table -> $2000.. */
            uint16_t p = (uint16_t)(0x2000 + i * 0x28);
            pre[0x073D + i] = (uint8_t)p;
            pre[0x0793 + i] = (uint8_t)(p >> 8);
        }
        pre[0x0096] = (uint8_t)((xs() & 3) + 1);           /* small outer count */
        pre[0x009C] = (uint8_t)(0x20 + (xs() & 0x1F));     /* column endpoints */
        pre[0x009D] = (uint8_t)(0x20 + (xs() & 0x1F));
        pre[0x009E] = (uint8_t)(0x30 + (xs() & 0x0F));     /* row range (stays < $54) */
        pre[0x009F] = (uint8_t)(0x28 + (xs() & 0x0F));
        mem_fail += diff_run("draw_symmetric_span_loop", pre, zero_cpu(),
                             draw_symmetric_span_loop, draw_symmetric_span_loop__t6502,
                             t, &printed, &cpu_diff);
    }
    printf("draw_symmetric_span_loop: %d cases, %d mem mismatch (must be 0), %d cpu diffs\n",
           N, mem_fail, cpu_diff);
    return mem_fail;
}

/* --- draw_shape_rows_loop @ $6620: 86-row masked plot via the row-addr table.
 * Same hazard as draw_symmetric_span_loop — random row pointers make plot_pixel_masked
 * overwrite the $0092 loop counter and hang both runs.  Seed the addr table into safe
 * bitmap RAM ($2000 + row*$28) and bound the plotted columns ($009C/$009D). --- */
static int test_draw_shape_rows_loop(void) {
    if (!want("draw_shape_rows_loop")) return 0;
    enum { N = 8000 };
    static uint8_t pre[65536];
    int mem_fail = 0, cpu_diff = 0, printed = 0;
    for (int t = 0; t < N; t++) {
        fill_random(pre);
        for (int i = 0; i <= 0x55; i++) {
            uint16_t p = (uint16_t)(0x2000 + i * 0x28);
            pre[0x073D + i] = (uint8_t)p;
            pre[0x0793 + i] = (uint8_t)(p >> 8);
        }
        pre[0x009C] = (uint8_t)(0x20 + (xs() & 0x1F));     /* plotted columns */
        pre[0x009D] = (uint8_t)(0x20 + (xs() & 0x1F));
        mem_fail += diff_run("draw_shape_rows_loop", pre, zero_cpu(),
                             draw_shape_rows_loop, draw_shape_rows_loop__t6502,
                             t, &printed, &cpu_diff);
    }
    printf("draw_shape_rows_loop: %d cases, %d mem mismatch (must be 0), %d cpu diffs\n",
           N, mem_fail, cpu_diff);
    return mem_fail;
}

/* --- draw_frame_pattern_seq @ $65FB: the doors/tunnel drawer.  init_row_coords_9c
 * seeds the coords, so only the addr table and the $6E0F span-count pattern need a
 * fixture: point the table into bitmap RAM ($2000+) and bound the span counts to 1..2
 * so the inner draw_symmetric_span_loop keeps $009E within the 85-row table (a larger
 * count would run the row index off the table into garbage/ZP and hang both runs). --- */
static int test_draw_frame_pattern_seq(void) {
    if (!want("draw_frame_pattern_seq")) return 0;
    enum { N = 3000 };
    static uint8_t pre[65536];
    int mem_fail = 0, cpu_diff = 0, printed = 0;
    for (int t = 0; t < N; t++) {
        fill_random(pre);
        for (int i = 0; i <= 0x55; i++) {
            uint16_t p = (uint16_t)(0x2000 + i * 0x28);
            pre[0x073D + i] = (uint8_t)p;
            pre[0x0793 + i] = (uint8_t)(p >> 8);
        }
        for (int i = 0; i <= 0x14; i++) pre[0x6E0F + i] = (uint8_t)((xs() & 1) + 1);  /* span counts 1..2 */
        mem_fail += diff_run("draw_frame_pattern_seq", pre, zero_cpu(),
                             draw_frame_pattern_seq, draw_frame_pattern_seq__t6502,
                             t, &printed, &cpu_diff);
    }
    printf("draw_frame_pattern_seq: %d cases, %d mem mismatch (must be 0), %d cpu diffs\n",
           N, mem_fail, cpu_diff);
    return mem_fail;
}

/* --- draw_vline_pair @ $6C4D: plots a vertical line pair, walking the row counter
 * $0092 from entry A down to $00B8.  Needs (a) the addr table in safe bitmap RAM,
 * (b) entry A >= $00B8 with both small so the loop terminates without the negative-row
 * clamp spinning, and (c) the stack page masked (the routine PHA/PLAs the row value).
 * Entry X drives the plotted column; entry carry is set internally per call. --- */
static int test_draw_vline_pair(void) {
    if (!want("draw_vline_pair")) return 0;
    enum { N = 6000 };
    static uint8_t pre[65536];
    static uint16_t stack_pg[256];
    for (int i = 0; i < 256; i++) stack_pg[i] = (uint16_t)(0x0100 + i);
    int mem_fail = 0, cpu_diff = 0, printed = 0;
    set_ignore(stack_pg, 256);
    for (int t = 0; t < N; t++) {
        fill_random(pre);
        for (int i = 0; i <= 0x55; i++) {
            uint16_t p = (uint16_t)(0x2000 + i * 0x28);
            pre[0x073D + i] = (uint8_t)p;
            pre[0x0793 + i] = (uint8_t)(p >> 8);
        }
        uint8_t b8 = (uint8_t)(xs() & 0x1F);
        pre[0x00B8] = b8;
        Cpu6502 c = zero_cpu();
        c.A = (uint8_t)(b8 + (xs() & 0x1F));               /* start row >= end row */
        c.X = (uint8_t)(xs() & 0xFF);
        mem_fail += diff_run("draw_vline_pair", pre, c,
                             draw_vline_pair, draw_vline_pair__t6502, t, &printed, &cpu_diff);
    }
    set_ignore(0, 0);
    printf("draw_vline_pair: %d cases, %d mem mismatch (must be 0), %d cpu diffs\n",
           N, mem_fail, cpu_diff);
    return mem_fail;
}

/* --- update_object_distance @ $6BED: clamped distance + up to 3 draw_vline_pair draws.
 * draw_vline_pair self-bounds (its start row $00B7 is clamped <= $2E and its end row
 * $00B8 stays in 0..$7F), so only draw_vline_pair's pointer safety needs the addr-table
 * fixture; the stack page is masked (draw_vline_pair PHA/PLAs).  Entry X = object slot. --- */
static int test_update_object_distance(void) {
    if (!want("update_object_distance")) return 0;
    enum { N = 6000 };
    static uint8_t pre[65536];
    static uint16_t stack_pg[256];
    for (int i = 0; i < 256; i++) stack_pg[i] = (uint16_t)(0x0100 + i);
    int mem_fail = 0, cpu_diff = 0, printed = 0;
    set_ignore(stack_pg, 256);
    for (int t = 0; t < N; t++) {
        fill_random(pre);
        for (int i = 0; i <= 0x55; i++) {
            uint16_t p = (uint16_t)(0x2000 + i * 0x28);
            pre[0x073D + i] = (uint8_t)p;
            pre[0x0793 + i] = (uint8_t)(p >> 8);
        }
        Cpu6502 c = zero_cpu();
        c.X = (uint8_t)(xs() & 0xFF);
        mem_fail += diff_run("update_object_distance", pre, c,
                             update_object_distance, update_object_distance__t6502, t, &printed, &cpu_diff);
    }
    set_ignore(0, 0);
    printf("update_object_distance: %d cases, %d mem mismatch (must be 0), %d cpu diffs\n",
           N, mem_fail, cpu_diff);
    return mem_fail;
}

/* --- advance_object_positions @ $6BA8: scroll-counter bump + 22x update_object_distance.
 * No entry registers (X is set internally); needs the same addr-table fixture + stack
 * mask as update_object_distance (reached via its draw_vline_pair calls). --- */
static int test_advance_object_positions(void) {
    if (!want("advance_object_positions")) return 0;
    enum { N = 4000 };
    static uint8_t pre[65536];
    static uint16_t stack_pg[256];
    for (int i = 0; i < 256; i++) stack_pg[i] = (uint16_t)(0x0100 + i);
    int mem_fail = 0, cpu_diff = 0, printed = 0;
    set_ignore(stack_pg, 256);
    for (int t = 0; t < N; t++) {
        fill_random(pre);
        for (int i = 0; i <= 0x55; i++) {
            uint16_t p = (uint16_t)(0x2000 + i * 0x28);
            pre[0x073D + i] = (uint8_t)p;
            pre[0x0793 + i] = (uint8_t)(p >> 8);
        }
        mem_fail += diff_run("advance_object_positions", pre, zero_cpu(),
                             advance_object_positions, advance_object_positions__t6502,
                             t, &printed, &cpu_diff);
    }
    set_ignore(0, 0);
    printf("advance_object_positions: %d cases, %d mem mismatch (must be 0), %d cpu diffs\n",
           N, mem_fail, cpu_diff);
    return mem_fail;
}

/* --- vobj_pos_to_pmstrip_index @ $41DA: result is cpu.Y (and A), no mem writes. --- */
static int test_vobj_pos_to_pmstrip_index(void) {
    if (!want("vobj_pos_to_pmstrip_index")) return 0;
    enum { N = 20000 };
    static uint8_t pre[65536], ref_mem[65536];
    int mem_fail = 0, cpu_fail = 0, printed = 0;
    for (int t = 0; t < N; t++) {
        fill_random(pre);
        Cpu6502 c = zero_cpu();
        memcpy((void *)mem, pre, 65536); cpu = c;
        vobj_pos_to_pmstrip_index__t6502();
        memcpy(ref_mem, (void *)mem, sizeof ref_mem);
        Cpu6502 ref_cpu = cpu;
        memcpy((void *)mem, pre, 65536); cpu = c;
        vobj_pos_to_pmstrip_index();
        if (memcmp((const void *)mem, ref_mem, 65536) != 0) {
            mem_fail++;
            if (printed < 12)
                for (int i = 0; i < 65536 && printed < 12; i++)
                    if (mem[i] != ref_mem[i]) {
                        printf("[MEM DIFF] vobj_pos_to_pmstrip_index case %d  $%04X  ref=$%02X native=$%02X\n",
                               t, i, ref_mem[i], mem[i]); printed++;
                    }
        }
        if (cpu.A != ref_cpu.A || cpu.Y != ref_cpu.Y) cpu_fail++;
    }
    printf("vobj_pos_to_pmstrip_index: %d cases, %d mem mismatch, %d cpu(A/Y) mismatch (both must be 0)\n",
           N, mem_fail, cpu_fail);
    return mem_fail + cpu_fail;
}

/* --- memset_or_copy @ $3C93: pointer fill.  Seed dest $C1/$C2 -> $2000 and a small
 * 16-bit count ($C3 = 0..$3F, $C4 = 0) so the fill stays in safe bitmap RAM and the
 * loop is short; a random dest/count could fill 64 KB over its own ZP counters. --- */
static int test_memset_or_copy(void) {
    if (!want("memset_or_copy")) return 0;
    enum { N = 8000 };
    static uint8_t pre[65536];
    int mem_fail = 0, cpu_diff = 0, printed = 0;
    for (int t = 0; t < N; t++) {
        fill_random(pre);
        pre[0x00C1] = 0x00; pre[0x00C2] = 0x20;            /* dest = $2000 */
        pre[0x00C3] = (uint8_t)(xs() & 0x3F); pre[0x00C4] = 0x00;  /* small count */
        mem_fail += diff_run("memset_or_copy", pre, zero_cpu(),
                             memset_or_copy, memset_or_copy__t6502, t, &printed, &cpu_diff);
    }
    printf("memset_or_copy: %d cases, %d mem mismatch (must be 0), %d cpu diffs\n", N, mem_fail, cpu_diff);
    return mem_fail;
}

/* --- copy_bytes_to_dst @ $3C61: pointer fill, count = entry X (<=256).  Seed dest
 * $BD/$BE -> $2000; entry A (value), X (count), Y (offset) are randomized. --- */
static int test_copy_bytes_to_dst(void) {
    if (!want("copy_bytes_to_dst")) return 0;
    enum { N = 8000 };
    static uint8_t pre[65536];
    int mem_fail = 0, cpu_diff = 0, printed = 0;
    for (int t = 0; t < N; t++) {
        fill_random(pre);
        pre[0x00BD] = 0x00; pre[0x00BE] = 0x20;            /* dest = $2000 */
        Cpu6502 c = zero_cpu();
        c.A = (uint8_t)(xs() & 0xFF);
        c.X = (uint8_t)(xs() & 0xFF);
        c.Y = (uint8_t)(xs() & 0xFF);
        mem_fail += diff_run("copy_bytes_to_dst", pre, c,
                             copy_bytes_to_dst, copy_bytes_to_dst__t6502, t, &printed, &cpu_diff);
    }
    printf("copy_bytes_to_dst: %d cases, %d mem mismatch (must be 0), %d cpu diffs\n", N, mem_fail, cpu_diff);
    return mem_fail;
}

/* Like test_mem_contract but with random entry A/X/Y/C — for routines that read
 * an entry register as input (a table index, a value to store, an entry carry). */
static int test_mem_contract_regs(const char *name, void (*native)(void), void (*t6502)(void)) {
    if (!want(name)) return 0;
    enum { N = 20000 };
    static uint8_t pre[65536];
    int mem_fail = 0, cpu_diff = 0, printed = 0;

    for (int t = 0; t < N; t++) {
        fill_random(pre);
        Cpu6502 c = zero_cpu();
        c.A = (uint8_t)(xs() & 0xFF);
        c.X = (uint8_t)(xs() & 0xFF);
        c.Y = (uint8_t)(xs() & 0xFF);
        c.C = (uint8_t)(xs() & 1);
        mem_fail += diff_run(name, pre, c, native, t6502, t, &printed, &cpu_diff);
    }
    printf("%s: %d cases, %d mem mismatch (must be 0), %d cpu diffs\n", name, N, mem_fail, cpu_diff);
    return mem_fail;
}

/* raster_fill_region @ $AB9A: nested fill driven by the fixed-point step
 * {$0051:$0050}.  The game's step is a sub-pixel increment (high byte $0051 is
 * small — set_plot_mask_and_halve_step divides by 4); a fully random $0051 could
 * make the accumulator loops run pathologically long, so seed a realistic step
 * ($0051 in 0..$3F).  Both runs share it, so the logic is still fully diffed. */
static int test_raster_fill_region(void) {
    if (!want("raster_fill_region")) return 0;
    enum { N = 20000 };
    static uint8_t pre[65536];
    int mem_fail = 0, cpu_diff = 0, printed = 0;

    for (int t = 0; t < N; t++) {
        fill_random(pre);
        pre[0x0051] = (uint8_t)(xs() % 0x40);   /* realistic step high byte (0..$3F) */
        mem_fail += diff_run("raster_fill_region", pre, zero_cpu(),
                             raster_fill_region, raster_fill_region__t6502, t, &printed, &cpu_diff);
    }
    printf("raster_fill_region: %d cases, %d mem mismatch (must be 0), %d cpu diffs\n",
           N, mem_fail, cpu_diff);
    return mem_fail;
}

/* terrain_plot_object_a/A90A @ $A822/$A90A: plot one terrain object indexed by entry
 * X.  Force the two slot-guard cells ($2487/$242D[X]) to 0 so the body runs every
 * case (the nonzero early-out is a trivial empty return), and seed a realistic
 * step ($232E[X] in 0..$3F -> $0051) so the raster_fill_region loops terminate. */
static int test_terrain_sub_obj(const char *name, void (*nat)(void), void (*t6502)(void)) {
    if (!want(name)) return 0;
    enum { N = 20000 };
    static uint8_t pre[65536];
    int mem_fail = 0, cpu_diff = 0, printed = 0;

    for (int t = 0; t < N; t++) {
        fill_random(pre);
        uint8_t x = (uint8_t)(xs() & 0xFF);
        pre[0x2487 + x] = 0x00;                  /* pass both slot guards -> run the body */
        pre[0x242D + x] = 0x00;
        pre[0x232E + x] = (uint8_t)(xs() % 0x40); /* realistic step ($0051) */
        Cpu6502 c = zero_cpu(); c.X = x;
        mem_fail += diff_run(name, pre, c, nat, t6502, t, &printed, &cpu_diff);
    }
    printf("%s: %d cases, %d mem mismatch (must be 0), %d cpu diffs\n", name, N, mem_fail, cpu_diff);
    return mem_fail;
}

/* Snapshot-driven test: seed the pre-state from a real in-flight RAM capture
 * (a800dumps/flight_ram_0000_BFFF.bin) and vary entry X (masked to a realistic
 * range) and Y.  Needed for the terrain functions whose loops only terminate with
 * realistic terrain arrays (random mem[] would spin forever).  The Makefile runs
 * ./build/validate_native from the repo root, so the relative path resolves. */
static int test_from_snapshot(const char *name, void (*nat)(void), void (*t6502)(void),
                              int N, uint8_t xmask) {
    if (!want(name)) return 0;
    static uint8_t snap[65536], pre[65536];
    const char *path = "a800dumps/flight_ram_0000_BFFF.bin";

    FILE *f = fopen(path, "rb");
    if (!f) { printf("%s: SKIP (%s not found)\n", name, path); return 0; }
    memset(snap, 0, sizeof snap);
    size_t got = fread(snap, 1, 0xC000, f);
    fclose(f);
    if (got != 0xC000) { printf("%s: SKIP (short read %zu)\n", name, got); return 0; }

    int mem_fail = 0, cpu_diff = 0, printed = 0;
    for (int t = 0; t < N; t++) {
        memcpy(pre, snap, sizeof pre);
        Cpu6502 c = zero_cpu();
        c.X = (uint8_t)(xs() & xmask);
        c.Y = (uint8_t)(xs() & 0xFF);
        mem_fail += diff_run(name, pre, c, nat, t6502, t, &printed, &cpu_diff);
    }
    printf("%s: %d cases (real flight snapshot), %d mem mismatch (must be 0), %d cpu diffs\n",
           name, N, mem_fail, cpu_diff);
    return mem_fail;
}

/* Dial-bar trio (draw_cockpit_dial_bar / setup_dial_bar_draw / draw_object_column): the draw loop
 * reads column pointers from the $4581 table and writes via them, so it needs the REAL
 * table (random mem could point a write at the loop counter $00BD -> same infinite loop in
 * both runs, undiffable).  Seed from the flight snapshot, force the real loop bounds
 * $00BD=$0F/$00BE=$07 (keeps the $4581 index in the valid 16-entry range), and vary entry A
 * (the dial value / column index) within a safe mask. */
static int test_dial_bar(const char *name, void (*nat)(void), void (*t6502)(void), uint8_t amask) {
    if (!want(name)) return 0;
    static uint8_t snap[65536], pre[65536];
    const char *path = "a800dumps/flight_ram_0000_BFFF.bin";
    FILE *f = fopen(path, "rb");
    if (!f) { printf("%s: SKIP (%s not found)\n", name, path); return 0; }
    memset(snap, 0, sizeof snap);
    size_t got = fread(snap, 1, 0xC000, f); fclose(f);
    if (got != 0xC000) { printf("%s: SKIP (short read %zu)\n", name, got); return 0; }
    int mem_fail = 0, cpu_diff = 0, printed = 0;
    for (int t = 0; t < 20000; t++) {
        memcpy(pre, snap, sizeof pre);
        pre[0x00BD] = 0x0F; pre[0x00BE] = 0x07;          /* real dial loop bounds */
        Cpu6502 c = zero_cpu();
        c.A = (uint8_t)(xs() & amask);
        mem_fail += diff_run(name, pre, c, nat, t6502, t, &printed, &cpu_diff);
    }
    printf("%s: %d cases (dial snapshot), %d mem mismatch (must be 0), %d cpu diffs\n",
           name, 20000, mem_fail, cpu_diff);
    return mem_fail;
}

/* flight_control_integrate @ $8E5B (the VBI root): seed from the flight snapshot so its
 * tables/arrays are real — the $0072==2 path runs draw_object_column, whose $4581 pointer
 * table must be valid (a random pointer could aim a write at its loop counter -> undiffable
 * hang).  Randomize the zero-page control bytes ($00-$7F) each case for branch coverage,
 * keeping the big arrays/tables from the snapshot.  Reads RANDOM (seeded per case).
 *
 * Masks the 6502 stack page $0100-$01FF: it holds only TRANSIENT pushed values from the
 * native callees (e.g. object_step_and_collide's $9641 PHA / the ring helpers), which are
 * pulled back before return and never read as stale residue — so the leftover byte is dead
 * scratch, outside the functional contract.  With the test's entry S=0, that residue lands
 * at an S-relative byte ($01FF) that the oracle and native can leave differing without any
 * functional consequence.  Verified the mask is load-bearing AND minimal: with it OFF the
 * ONLY diffs are in this page (all observed at $01FF), nowhere in functional memory. */
static int test_flight_control_integrate(void) {
    if (!want("flight_control_integrate")) return 0;
    static uint8_t snap[65536], pre[65536];
    static uint16_t stack_page[256];
    for (int i = 0; i < 256; i++) stack_page[i] = (uint16_t)(0x0100 + i);
    const char *path = "a800dumps/flight_ram_0000_BFFF.bin";
    FILE *f = fopen(path, "rb");
    if (!f) { printf("flight_control_integrate: SKIP (%s not found)\n", path); return 0; }
    memset(snap, 0, sizeof snap);
    size_t got = fread(snap, 1, 0xC000, f); fclose(f);
    if (got != 0xC000) { printf("flight_control_integrate: SKIP (short read %zu)\n", got); return 0; }
    int mem_fail = 0, cpu_diff = 0, printed = 0;
    set_ignore(stack_page, 256);                 /* dead CPU-stack scratch (see header) */
    for (int t = 0; t < 8000; t++) {
        memcpy(pre, snap, sizeof pre);
        for (int a = 0x00; a <= 0x7F; a++) pre[a] = (uint8_t)(xs() & 0xFF);  /* control bytes */
        Cpu6502 c = zero_cpu();
        mem_fail += diff_run("flight_control_integrate", pre, c,
                             flight_control_integrate, flight_control_integrate__t6502,
                             t, &printed, &cpu_diff);
    }
    set_ignore(0, 0);
    printf("flight_control_integrate: %d cases (snapshot + random ZP), %d mem mismatch (must be 0), %d cpu diffs\n",
           8000, mem_fail, cpu_diff);
    return mem_fail;
}

/* terrain_draw_frame @ $A31E: snapshot-driven (its object loop drives terrain_subdivide_column /
 * terrain_column_rasterize, which only terminate on real terrain arrays).  Entry X
 * is the level base index — the two real call-site values are $00 and $30, which
 * also exercise both sides of the $A7-zero check; alternate them.  RANDOM ($D20A)
 * is read several times and seeded identically per case by diff_run. */
static int test_terrain_draw_frame(void) {
    if (!want("terrain_draw_frame")) return 0;
    static uint8_t snap[65536], pre[65536];
    const char *path = "a800dumps/flight_ram_0000_BFFF.bin";
    FILE *f = fopen(path, "rb");
    if (!f) { printf("terrain_draw_frame: SKIP (%s not found)\n", path); return 0; }
    memset(snap, 0, sizeof snap);
    size_t got = fread(snap, 1, 0xC000, f);
    fclose(f);
    if (got != 0xC000) { printf("terrain_draw_frame: SKIP (short read %zu)\n", got); return 0; }

    enum { N = 2000 };
    int mem_fail = 0, cpu_diff = 0, printed = 0;
    for (int t = 0; t < N; t++) {
        memcpy(pre, snap, sizeof pre);
        Cpu6502 c = zero_cpu();
        c.X = (t & 1) ? 0x30 : 0x00;
        mem_fail += diff_run("terrain_draw_frame", pre, c, terrain_draw_frame,
                             terrain_draw_frame__t6502, t, &printed, &cpu_diff);
    }
    printf("terrain_draw_frame: %d cases (real flight snapshot, X in {0,$30}), %d mem mismatch (must be 0), %d cpu diffs\n",
           N, mem_fail, cpu_diff);
    return mem_fail;
}

/* --- flight main-loop batch (game_state_update + enemy_check) --- */

/* The scanline plotters (plot_scanline_up/down/rand_dir) walk via terrain_plot_pixel,
 * which writes through bitmap pointers from the $28CA/$28FA row tables.  With fully
 * random mem[] a write could alias the inner counter $28FA and spin forever; seed the
 * real flight snapshot (sane row tables -> writes land in the $1010 bitmap, far from
 * the $28xx accumulators) so the loops terminate.  Entry regs are loaded from mem[] at
 * entry (irrelevant), and RANDOM is seeded identically per case by diff_run. */
static int test_scanline(const char *name, void (*nat)(void), void (*t6502)(void)) {
    if (!want(name)) return 0;
    static uint8_t snap[65536], pre[65536];
    const char *path = "a800dumps/flight_ram_0000_BFFF.bin";
    FILE *f = fopen(path, "rb");
    if (!f) { printf("%s: SKIP (%s not found)\n", name, path); return 0; }
    memset(snap, 0, sizeof snap);
    size_t got = fread(snap, 1, 0xC000, f); fclose(f);
    if (got != 0xC000) { printf("%s: SKIP (short read %zu)\n", name, got); return 0; }
    int mem_fail = 0, cpu_diff = 0, printed = 0;
    enum { N = 4000 };
    for (int t = 0; t < N; t++) {
        memcpy(pre, snap, sizeof pre);
        mem_fail += diff_run(name, pre, zero_cpu(), nat, t6502, t, &printed, &cpu_diff);
    }
    printf("%s: %d cases (flight snapshot), %d mem mismatch (must be 0), %d cpu diffs\n",
           name, N, mem_fail, cpu_diff);
    return mem_fail;
}

/* game_state_update @ $A99C: snapshot-driven (it drives the scanline plotters, which
 * need the real row tables).  Randomize the control bytes that gate its branches so
 * all paths are covered — half the cases force $28EE=1 (so the DEC reaches 0 and the
 * line-plot path runs with $28ED!=0), and $007E hits both the ==7 special branch and
 * the normal branch.  $28EB/$28EC (-> plot start X/Y) and the step seeds vary too. */
static int test_game_state_update(void) {
    if (!want("game_state_update")) return 0;
    static uint8_t snap[65536], pre[65536];
    const char *path = "a800dumps/flight_ram_0000_BFFF.bin";
    FILE *f = fopen(path, "rb");
    if (!f) { printf("game_state_update: SKIP (%s not found)\n", path); return 0; }
    memset(snap, 0, sizeof snap);
    size_t got = fread(snap, 1, 0xC000, f); fclose(f);
    if (got != 0xC000) { printf("game_state_update: SKIP (short read %zu)\n", got); return 0; }
    int mem_fail = 0, cpu_diff = 0, printed = 0;
    enum { N = 8000 };
    for (int t = 0; t < N; t++) {
        memcpy(pre, snap, sizeof pre);
        pre[0x28EE] = (xs() & 1) ? 0x01 : (uint8_t)(xs() & 0xFF);  /* 50% -> DEC hits 0 */
        pre[0x28ED] = (uint8_t)(xs() | 0x01);                      /* event queued (nonzero) */
        pre[0x007E] = (xs() & 1) ? 0x07 : (uint8_t)(xs() & 0xFF);  /* both special + normal */
        pre[0x004D] = (uint8_t)(xs() & 0xFF);
        pre[0x2826] = (uint8_t)(xs() & 0xFF);
        pre[0x003D] = (uint8_t)(xs() & 0xFF);
        pre[0x291A] = (uint8_t)(xs() & 0xFF);
        pre[0x0624] = (uint8_t)(xs() & 0xFF);
        pre[0x0041] = (uint8_t)(xs() & 0xFF);
        pre[0x28EB] = (uint8_t)(xs() & 0xFF);
        pre[0x28EC] = (uint8_t)(xs() & 0xFF);
        mem_fail += diff_run("game_state_update", pre, zero_cpu(),
                             game_state_update, game_state_update__t6502, t, &printed, &cpu_diff);
    }
    printf("game_state_update: %d cases (snapshot + control bytes), %d mem mismatch (must be 0), %d cpu diffs\n",
           N, mem_fail, cpu_diff);
    return mem_fail;
}

/* enemy_check @ $3FCD: force $063D=0 so it dispatches down the $0633->pmg_enemy_update
 * branch (the $063D!=0 branch tail-calls the still-transpiled intro_cinematic_loop, whose
 * closure spins on VCOUNT busy-waits the harness can't advance — and it is a trivial
 * identical tail-call to unchanged transpiled code, so it needs no diff).  Random mem
 * otherwise; pmg_enemy_update + its native callees have no unbounded loops. */
static int test_enemy_check(void) {
    if (!want("enemy_check")) return 0;
    enum { N = 20000 };
    static uint8_t pre[65536];
    int mem_fail = 0, cpu_diff = 0, printed = 0;
    for (int t = 0; t < N; t++) {
        fill_random(pre);
        pre[0x063D] = 0x00;                       /* avoid the transpiled intro_cinematic_loop branch */
        mem_fail += diff_run("enemy_check", pre, zero_cpu(),
                             enemy_check, enemy_check__t6502, t, &printed, &cpu_diff);
    }
    printf("enemy_check: %d cases ($063D=0 -> pmg branch), %d mem mismatch (must be 0), %d cpu diffs\n",
           N, mem_fail, cpu_diff);
    return mem_fail;
}

/* --- in-game SFX engine ($548D subtree) -----------------------------------
 * The voice writers emit POKEY AUDF/AUDC at $D1FE+X / $D1FF+X where X is the
 * per-voice register index mem[$0705+Y].  Seed those indices with realistic
 * POKEY pairs {0,2,4,6,8} (0 = inactive slot) so the writes land in $D200-$D207,
 * and MASK $D200-$D20F: the native uses bus_write (-> Paula on the Amiga) while
 * the __t6502 twin writes raw mem[] there, so the POKEY range is a hardware side
 * effect, not part of the mem[] contract.  (Static AUDF/AUDC/AUDCTL writes use
 * bus_write in BOTH twins, so those need no mask.) */
static void seed_voice_regs(uint8_t *pre) {
    static const uint8_t regidx[5] = { 0, 2, 4, 6, 8 };
    for (int s = 0; s <= 15; s++) pre[0x0705 + s] = regidx[xs() % 5];
}
/* Combined contract mask for the stack-aware / POKEY-writing SFX fns: the 6502
 * stack page $0100-$01FF (PHA/PLA transient — the native uses C locals, no stack)
 * plus the POKEY range $D200-$D20F (hardware side effect via bus_write). */
static int build_sfx_mask(uint16_t *buf) {
    int n = 0;
    for (int a = 0x0100; a <= 0x01FF; a++) buf[n++] = (uint16_t)a;
    for (int a = 0xD200; a <= 0xD20F; a++) buf[n++] = (uint16_t)a;
    return n;
}
/* Voice writers, the mixer, and input_init: random entry A/X/C, entry Y a real
 * voice slot 1..14.  Mask stack + POKEY. */
static int test_sfx_voice(const char *name, void (*nat)(void), void (*t6502)(void)) {
    if (!want(name)) return 0;
    enum { N = 20000 };
    static uint8_t pre[65536];
    static uint16_t mask[272];
    int mem_fail = 0, cpu_diff = 0, printed = 0;
    set_ignore(mask, build_sfx_mask(mask));
    for (int t = 0; t < N; t++) {
        fill_random(pre);
        seed_voice_regs(pre);
        for (int i = 0; i < 128; i++) pre[0x56D4 + i] = (uint8_t)(1 + (xs() % 14)); /* valid slots (input_init) */
        Cpu6502 c = zero_cpu();
        c.A = (uint8_t)(xs() & 0xFF);
        c.X = (uint8_t)(xs() & 0xFF);
        c.Y = (uint8_t)(1 + (xs() % 14));        /* voice slot 1..14 */
        c.C = (uint8_t)(xs() & 1);
        mem_fail += diff_run(name, pre, c, nat, t6502, t, &printed, &cpu_diff);
    }
    set_ignore(0, 0);
    printf("%s: %d cases, %d mem mismatch (must be 0), %d cpu diffs\n", name, N, mem_fail, cpu_diff);
    return mem_fail;
}
/* sfx_voice_envelope_tick @ $548D — the apex.  The ring drain ($0719, head $0073 /
 * tail $0074, both decremented & wrapped at $1F) only terminates when the SFX
 * event-table $56D4 holds VALID voice-slot indices (1..14, bit7 clear): a bit7-set
 * ring entry runs input_init, whose tail game_sub_55FC re-pushes mem[$56D4+i] — if
 * that were bit7-set garbage it would re-dispatch input_init forever (the __t6502
 * twin hangs identically, so timeout can't diff it).  Seed $56D4 valid, the voice
 * register indices, and head/tail in 0..$1F; mask the POKEY range. */
static int test_sfx_voice_envelope_tick(void) {
    if (!want("sfx_voice_envelope_tick")) return 0;
    enum { N = 5000 };
    static uint8_t pre[65536];
    static uint16_t mask[272];
    int mem_fail = 0, cpu_diff = 0, printed = 0;
    set_ignore(mask, build_sfx_mask(mask));   /* stack page + POKEY */
    for (int t = 0; t < N; t++) {
        fill_random(pre);
        seed_voice_regs(pre);
        for (int i = 0; i < 128; i++) pre[0x56D4 + i] = (uint8_t)(1 + (xs() % 14)); /* valid slots */
        pre[0x0073] = (uint8_t)(xs() % 0x20);    /* ring head 0..$1F */
        pre[0x0074] = (uint8_t)(xs() % 0x20);    /* ring tail 0..$1F */
        mem_fail += diff_run("sfx_voice_envelope_tick", pre, zero_cpu(),
                             sfx_voice_envelope_tick, sfx_voice_envelope_tick__t6502, t, &printed, &cpu_diff);
    }
    set_ignore(0, 0);
    printf("sfx_voice_envelope_tick: %d cases, %d mem mismatch (must be 0), %d cpu diffs\n",
           N, mem_fail, cpu_diff);
    return mem_fail;
}

int main(int argc, char **argv) {
    g_filter = argv + 1; g_nfilter = argc - 1;   /* optional name-substring filters */
    platform_test_init_headless();   /* enable seedable RANDOM ($D20A) for both runs */
    if (g_nfilter) { printf("filter:"); for (int i = 0; i < g_nfilter; i++) printf(" %s", g_filter[i]); printf("\n"); }

    int fails = 0;
    fails += test_divide_16x16();
    fails += test_render_bcd_counter();
    fails += test_mem_contract("init_proj_scratch_pointers", init_proj_scratch_pointers, init_proj_scratch_pointers__t6502);
    fails += test_stack_push("ring_push_marked", ring_push_marked, ring_push_marked__t6502);
    fails += test_stack_push("game_sub_55FC", game_sub_55FC, game_sub_55FC__t6502);
    fails += test_mem_contract("sample_terrain_height_bilerp", sample_terrain_height_bilerp, sample_terrain_height_bilerp__t6502);
    fails += test_mem_contract_regs("game_sub_451d", game_sub_451d, game_sub_451d__t6502);
    fails += test_mem_contract_regs("enter_terrain_special_state", enter_terrain_special_state, enter_terrain_special_state__t6502);
    fails += test_mem_contract_regs("exit_terrain_special_state", exit_terrain_special_state, exit_terrain_special_state__t6502);
    fails += test_mem_contract("update_terrain_horizon_lr", update_terrain_horizon_lr, update_terrain_horizon_lr__t6502);
    fails += test_mem_contract("update_terrain_scanline_proj", update_terrain_scanline_proj, update_terrain_scanline_proj__t6502);
    /* flight_control_integrate subtree (leaves first) */
    fails += test_mem_contract("reset_flags_ff", reset_flags_ff, reset_flags_ff__t6502);
    fails += test_mem_contract("load_velocity_from_param_block", load_velocity_from_param_block, load_velocity_from_param_block__t6502);
    fails += test_mem_contract("bcd_inc_counter_0641", bcd_inc_counter_0641, bcd_inc_counter_0641__t6502);
    fails += test_mem_contract("set_place_params_inc_count", set_place_params_inc_count, set_place_params_inc_count__t6502);
    fails += test_mem_contract("trigger_object_explosion", trigger_object_explosion, trigger_object_explosion__t6502);
    fails += test_mem_contract("reset_object_slot", reset_object_slot, reset_object_slot__t6502);
    fails += test_mem_contract("enqueue_indicator_event", enqueue_indicator_event, enqueue_indicator_event__t6502);
    fails += test_mem_contract("object_integrate_position", object_integrate_position, object_integrate_position__t6502);
    fails += test_mem_contract("jitter_roll_pitch", jitter_roll_pitch, jitter_roll_pitch__t6502);
    fails += test_mul_u8();
    fails += test_bin_to_bcd();
    fails += test_mem_contract_regs("copy_title_text_block_to_screen",
                                    copy_title_text_block_to_screen,
                                    copy_title_text_block_to_screen__t6502);
    fails += test_mem_contract("init_row_coords_9c", init_row_coords_9c, init_row_coords_9c__t6502);
    fails += test_mem_contract("clear_scroll_accum", clear_scroll_accum, clear_scroll_accum__t6502);
    fails += test_mem_contract("copy_192_to_1800", copy_192_to_1800, copy_192_to_1800__t6502);
    fails += test_mem_contract("build_row_addr_table", build_row_addr_table, build_row_addr_table__t6502);
    fails += test_mem_contract("build_line_addr_table_2000", build_line_addr_table_2000, build_line_addr_table_2000__t6502);
    fails += test_mem_contract_regs("build_line_addr_table_1000_stride",
                                    build_line_addr_table_1000_stride, build_line_addr_table_1000_stride__t6502);
    fails += test_mem_contract("build_line_addr_table_1000", build_line_addr_table_1000, build_line_addr_table_1000__t6502);
    fails += test_mem_contract("init_object_positions", init_object_positions, init_object_positions__t6502);
    fails += test_mem_contract("audio_timer_setup", audio_timer_setup, audio_timer_setup__t6502);
    fails += test_random_terrain_height();
    /* The pixel/span plotters contract on plotting into bitmap RAM via the row pointer /
       addr table, so they use seeded fixtures (not fully-random mem[], which would aim them
       at the $D000 HW range + self-clobbering ZP that the idiomatic native cores don't model). */
    fails += test_fill_horizontal_span();
    fails += test_plot_glyph_pixel_masked();
    fails += test_plot_pixel_masked();
    fails += test_mem_contract("set_row_ptr", set_row_ptr, set_row_ptr__t6502);
    fails += test_mem_contract("set_row_ptr_from_count", set_row_ptr_from_count, set_row_ptr_from_count__t6502);
    fails += test_fill_vertical_span();
    fails += test_plot_pixel_2bpp();
    fails += test_draw_symmetric_span_loop();
    fails += test_mem_contract_regs("gen_terrain_column", gen_terrain_column, gen_terrain_column__t6502);
    fails += test_mem_contract("fill_terrain_columns", fill_terrain_columns, fill_terrain_columns__t6502);
    fails += test_draw_shape_rows_loop();
    fails += test_draw_frame_pattern_seq();
    fails += test_draw_vline_pair();
    fails += test_update_object_distance();
    fails += test_advance_object_positions();
    fails += test_mem_contract("clear_var_0632", clear_var_0632, clear_var_0632__t6502);
    fails += test_mem_contract_regs("clear_pm_state", clear_pm_state, clear_pm_state__t6502);
    fails += test_mem_contract("clear_terrain_lo_buffers", clear_terrain_lo_buffers, clear_terrain_lo_buffers__t6502);
    fails += test_mem_contract("fill_four_bufs_ff", fill_four_bufs_ff, fill_four_bufs_ff__t6502);
    fails += test_mem_contract_regs("fill_buf_08d4", fill_buf_08d4, fill_buf_08d4__t6502);
    fails += test_mem_contract_regs("copy_4byte_table_to_02c4", copy_4byte_table_to_02c4, copy_4byte_table_to_02c4__t6502);
    fails += test_mem_contract("reset_audctl_flags", reset_audctl_flags, reset_audctl_flags__t6502);
    fails += test_mem_contract("game_init_first", game_init_first, game_init_first__t6502);
    fails += test_mem_contract("mark_grid_slot_active", mark_grid_slot_active, mark_grid_slot_active__t6502);
    fails += test_mem_contract("push_grid_cell", push_grid_cell, push_grid_cell__t6502);
    fails += test_vobj_pos_to_pmstrip_index();
    fails += test_mem_contract("copy_terrain_seed_rows", copy_terrain_seed_rows, copy_terrain_seed_rows__t6502);
    fails += test_mem_contract("copy_row_addr_subset", copy_row_addr_subset, copy_row_addr_subset__t6502);
    fails += test_memset_or_copy();
    fails += test_copy_bytes_to_dst();
    /* batch — display_setup-subtree mem-effect leaves */
    fails += test_mem_contract("draw_compass_heading", draw_compass_heading, draw_compass_heading__t6502);
    fails += test_mem_contract("fill_buffer2_region_ff", fill_buffer2_region_ff, fill_buffer2_region_ff__t6502);
    fails += test_mem_contract("game_sub_4606", game_sub_4606, game_sub_4606__t6502);
    fails += test_mem_contract_regs("fill_message_buffer", fill_message_buffer, fill_message_buffer__t6502);
    fails += test_mem_contract("intro_fill_display_params", intro_fill_display_params, intro_fill_display_params__t6502);
    fails += test_mem_contract_regs("match_code_sequence", match_code_sequence, match_code_sequence__t6502);
    fails += test_mem_contract("init_terrain_dl", init_terrain_dl, init_terrain_dl__t6502);
    fails += test_mem_contract_regs("music_init_state", music_init_state, music_init_state__t6502);
    fails += test_mem_contract("count_up_to_level", count_up_to_level, count_up_to_level__t6502);
    fails += test_mem_contract("hud_fill_field1", hud_fill_field1, hud_fill_field1__t6502);
    fails += test_mem_contract("hud_fill_field3_font", hud_fill_field3_font, hud_fill_field3_font__t6502);
    /* batch — tail-wrappers + A-returning RANDOM/compute leaves */
    fails += test_mem_contract("clear_message_buffer", clear_message_buffer, clear_message_buffer__t6502);
    fails += test_mem_contract("plot_pixel_col93", plot_pixel_col93, plot_pixel_col93__t6502);
    fails += test_ret_a("random_digit", random_digit, random_digit__t6502);
    fails += test_ret_a("random_alpha_index", random_alpha_index, random_alpha_index__t6502);
    fails += test_ret_a("test_marked_neighbor", test_marked_neighbor, test_marked_neighbor__t6502);
    /* batch — fill-region wrappers + RANDOM/compute leaves */
    fails += test_mem_contract("fill_region_2000", fill_region_2000, fill_region_2000__t6502);
    fails += test_mem_contract_regs("silence_audio_channels", silence_audio_channels, silence_audio_channels__t6502);
    fails += test_mem_contract("init_terrain_render_buffers", init_terrain_render_buffers, init_terrain_render_buffers__t6502);
    fails += test_mem_contract("game_init_7813", game_init_7813, game_init_7813__t6502);
    fails += test_mem_contract("game_sub_7B54", game_sub_7B54, game_sub_7B54__t6502);
    fails += test_ret_a("rng_signed_jitter", rng_signed_jitter, rng_signed_jitter__t6502);
    /* batch — cockpit/score init + zero-suppress char plotter */
    fails += test_mem_contract("init_cockpit_bar_cells", init_cockpit_bar_cells, init_cockpit_bar_cells__t6502);
    fails += test_mem_contract("add_and_show_bcd_counter", add_and_show_bcd_counter, add_and_show_bcd_counter__t6502);
    fails += test_plot_char_bounded();
    /* batch — BCD digit-pair, slot drivers, bare-RTS stubs */
    fails += test_emit_bcd_byte_digits();
    /* batch — grid-neighbor scan, ring-push drivers, RLE run fill */
    fails += test_mem_contract("scan_grid_neighbors", scan_grid_neighbors, scan_grid_neighbors__t6502);
    fails += test_mem_contract_regs("intro_reset_score_slots", intro_reset_score_slots, intro_reset_score_slots__t6502);
    fails += test_mem_contract_regs("init_event_state_5815_x16", init_event_state_5815_x16, init_event_state_5815_x16__t6502);
    fails += test_rle_run_fill();
    fails += test_blit_glyph_8rows();
    /* batch — RANDOM-driven intro object-map seeders */
    fails += test_mem_contract("intro_seed_object_map", intro_seed_object_map, intro_seed_object_map__t6502);
    fails += test_mem_contract("intro_unmark_random_cells", intro_unmark_random_cells, intro_unmark_random_cells__t6502);
    /* batch — font/voice init + cockpit message renderer */
    fails += test_sfx_engine_reset();
    fails += test_game_sub_6811();
    fails += test_plot_terrain_span();
    /* batch — event/score/message wrappers + object-table shift */
    fails += test_mem_contract("trigger_effect_4a", trigger_effect_4a, trigger_effect_4a__t6502);
    fails += test_mem_contract("terrain_plot_skip_return", terrain_plot_skip_return, terrain_plot_skip_return__t6502);
    fails += test_emit_chain("render_bcd_low_bytes", render_bcd_low_bytes, render_bcd_low_bytes__t6502);
    fails += test_emit_chain("set_zsupp_pos_clear_delta", set_zsupp_pos_clear_delta, set_zsupp_pos_clear_delta__t6502);
    fails += test_mem_contract_regs("save_color_clear_y_bit5", save_color_clear_y_bit5, save_color_clear_y_bit5__t6502);
    fails += test_mem_contract_regs("shift_object_table_up", shift_object_table_up, shift_object_table_up__t6502);
    /* batch — glyph blit wrappers + 2x2 glyph draw */
    fails += test_blit_chain("set_coord_y_e0", set_coord_y_e0, set_coord_y_e0__t6502);
    fails += test_blit_chain("glyph_ptr_from_index", glyph_ptr_from_index, glyph_ptr_from_index__t6502);
    fails += test_draw_glyph_2rows();
    fails += test_rle_expand_list();
    fails += test_rle_decompress();
    fails += test_loader_util();
    fails += test_game_init_77DF();
    fails += test_plot_clipped_pixel();
    fails += test_unpack_bitmap_4d3e();
    fails += test_intro_random_setup();
    fails += test_emit_dl_coord_pairs();
    fails += test_compute_stage_display_geometry();
    fails += test_blit_chain("blit_label_row", blit_label_row, blit_label_row__t6502);
    fails += test_blit_chain("blit_message_block", blit_message_block, blit_message_block__t6502);
    fails += test_draw_digit_low_nibble();
    fails += test_draw_2digit_value();
    fails += test_blit_chain("glyph_ptr_shift3", glyph_ptr_shift3, glyph_ptr_shift3__t6502);
    fails += test_emit_chain("render_bcd_top_byte", render_bcd_top_byte, render_bcd_top_byte__t6502);
    fails += test_unpack_terrain_seed_cols();
    fails += test_game_init_7588();
    fails += test_emit_chain("render_bcd_digits_supp_all", render_bcd_digits_supp_all, render_bcd_digits_supp_all__t6502);
    fails += test_blit_chain("blit_numeric_readout", blit_numeric_readout, blit_numeric_readout__t6502);
    fails += test_dl_lms_fill();
    fails += test_draw_dial_bar_column();
    fails += test_draw_player3_object();
    fails += test_dl_lms_build();
    fails += test_mem_contract("game_init_76CB", game_init_76CB, game_init_76CB__t6502);
    fails += test_emit_chain("setup_initials_ptr", setup_initials_ptr, setup_initials_ptr__t6502);
    fails += test_mem_contract("startup_init", startup_init, startup_init__t6502);
    fails += test_mem_contract("dl_index_dec", dl_index_dec, dl_index_dec__t6502);
    fails += test_mem_contract("dl_index_dec_or_reset", dl_index_dec_or_reset, dl_index_dec_or_reset__t6502);
    fails += test_draw_scaled_shape();
    fails += test_ret_a("pack_byte_to_5bit_cells", pack_byte_to_5bit_cells, pack_byte_to_5bit_cells__t6502);
    fails += test_ret_a("read_console_trig_delta", read_console_trig_delta, read_console_trig_delta__t6502);
    fails += test_validate_save_state();
    fails += test_cockpit_dial_update();
    fails += test_mem_contract("hud_fill_field0", hud_fill_field0, hud_fill_field0__t6502);
    fails += test_mem_contract("hud_fill_field2", hud_fill_field2, hud_fill_field2__t6502);
    fails += test_mem_contract("hud_build_text_row", hud_build_text_row, hud_build_text_row__t6502);
    fails += test_mem_contract_regs("show_cockpit_message", show_cockpit_message, show_cockpit_message__t6502);
    fails += test_mem_contract_regs("mark_slot_and_countdown_char", mark_slot_and_countdown_char, mark_slot_and_countdown_char__t6502);
    fails += test_mem_contract_regs("mark_slot_and_inc_count", mark_slot_and_inc_count, mark_slot_and_inc_count__t6502);
    fails += test_mem_contract("return_stub_40af", return_stub_40af, return_stub_40af__t6502);
    fails += test_mem_contract("terrain_obj_skip_return", terrain_obj_skip_return, terrain_obj_skip_return__t6502);
    fails += test_mem_contract("ret_stub_6a26", ret_stub_6a26, ret_stub_6a26__t6502);
    fails += test_mem_contract("draw_bar_loop_end", draw_bar_loop_end, draw_bar_loop_end__t6502);
    fails += test_mem_contract("terrain_plot_return", terrain_plot_return, terrain_plot_return__t6502);
    fails += test_mem_contract("terrain_distance_clamp_return", terrain_distance_clamp_return, terrain_distance_clamp_return__t6502);
    fails += test_mem_contract("plot_line_done", plot_line_done, plot_line_done__t6502);
    fails += test_mem_contract("compute_target_blip_position", compute_target_blip_position, compute_target_blip_position__t6502);
    fails += test_mem_contract_regs("obj_table_scan_replace", obj_table_scan_replace, obj_table_scan_replace__t6502);
    /* batch 2 — shallow drivers */
    fails += test_mem_contract_regs("obj_table_scan_y1_c8", obj_table_scan_y1_c8, obj_table_scan_y1_c8__t6502);
    fails += test_mem_contract_regs("obj_table_scan_a_c8", obj_table_scan_a_c8, obj_table_scan_a_c8__t6502);
    fails += test_mem_contract_regs("store_676_init", store_676_init, store_676_init__t6502);
    fails += test_mem_contract_regs("set_hud_fields_678_679", set_hud_fields_678_679, set_hud_fields_678_679__t6502);
    fails += test_mem_contract_regs("refresh_hud_field_0b", refresh_hud_field_0b, refresh_hud_field_0b__t6502);
    fails += test_mem_contract_regs("refresh_hud_field_0d_entry", refresh_hud_field_0d_entry, refresh_hud_field_0d_entry__t6502);
    fails += test_mem_contract_regs("refresh_hud_fields_0d_0e", refresh_hud_fields_0d_0e, refresh_hud_fields_0d_0e__t6502);
    fails += test_mem_contract("step_object_along_axes", step_object_along_axes, step_object_along_axes__t6502);
    fails += test_mem_contract("reset_indicator_event", reset_indicator_event, reset_indicator_event__t6502);
    fails += test_mem_contract_regs("compute_obj_rel_angle_scale", compute_obj_rel_angle_scale, compute_obj_rel_angle_scale__t6502);
    fails += test_dial_bar("draw_object_column", draw_object_column, draw_object_column__t6502, 0x0F);
    fails += test_dial_bar("setup_dial_bar_draw", setup_dial_bar_draw, setup_dial_bar_draw__t6502, 0x3F);
    fails += test_dial_bar("draw_cockpit_dial_bar", draw_cockpit_dial_bar, draw_cockpit_dial_bar__t6502, 0x3F);
    /* batch 3 — mid drivers */
    fails += test_mem_contract("countdown_show_char_0620", countdown_show_char_0620, countdown_show_char_0620__t6502);
    fails += test_mem_contract("check_object_in_target_box", check_object_in_target_box, check_object_in_target_box__t6502);
    fails += test_mem_contract_regs("check_player_proximity_hit", check_player_proximity_hit, check_player_proximity_hit__t6502);
    /* batch 4 — apex */
    fails += test_mem_contract("object_step_and_collide", object_step_and_collide, object_step_and_collide__t6502);
    fails += test_flight_control_integrate();
    /* flight main-loop batch (game_state_update + enemy_check) */
    fails += test_scanline("plot_scanline_down", plot_scanline_down, plot_scanline_down__t6502);
    fails += test_scanline("plot_scanline_up", plot_scanline_up, plot_scanline_up__t6502);
    fails += test_scanline("plot_scanline_rand_dir", plot_scanline_rand_dir, plot_scanline_rand_dir__t6502);
    fails += test_game_state_update();
    fails += test_mem_contract("pmg_enemy_update", pmg_enemy_update, pmg_enemy_update__t6502);
    fails += test_enemy_check();

    /* in-game SFX engine ($548D subtree, run each flight VBI) */
    fails += test_mem_contract_regs("sfx_pick_top_voice", sfx_pick_top_voice, sfx_pick_top_voice__t6502);
    fails += test_mem_contract_regs("sfx_pick_next_voice", sfx_pick_next_voice, sfx_pick_next_voice__t6502);
    fails += test_mem_contract_regs("sfx_engine_step", sfx_engine_step, sfx_engine_step__t6502);
    fails += test_sfx_voice("input_init", input_init, input_init__t6502);
    fails += test_sfx_voice("sfx_voice_write_freq", sfx_voice_write_freq, sfx_voice_write_freq__t6502);
    fails += test_sfx_voice("sfx_voice_write_freq_ctrl", sfx_voice_write_freq_ctrl, sfx_voice_write_freq_ctrl__t6502);
    fails += test_sfx_voice("reorder_sprite_slot", reorder_sprite_slot, reorder_sprite_slot__t6502);
    fails += test_sfx_voice_envelope_tick();

    fails += test_clear_terrain_column();
    fails += test_signed_mul_8x16();
    fails += test_mem_contract("sine_table_lookup", sine_table_lookup, sine_table_lookup__t6502);
    fails += test_mem_contract("trig_interp_lookup", trig_interp_lookup, trig_interp_lookup__t6502);
    fails += test_mem_contract("compute_row_xspans", compute_row_xspans, compute_row_xspans__t6502);
    fails += test_mem_contract("check_target_in_window", check_target_in_window, check_target_in_window__t6502);
    fails += test_mem_contract("obj_table_set_active", obj_table_set_active, obj_table_set_active__t6502);
    fails += test_ring_push_0719();
    fails += test_mem_contract("compute_heading_sincos", compute_heading_sincos, compute_heading_sincos__t6502);
    fails += test_mem_contract("build_view_transform_matrix", build_view_transform_matrix, build_view_transform_matrix__t6502);
    fails += test_mem_contract("setup_projection_params", setup_projection_params, setup_projection_params__t6502);
    fails += test_mem_contract_regs("set_plot_mask_and_halve_step", set_plot_mask_and_halve_step, set_plot_mask_and_halve_step__t6502);
    fails += test_mem_contract_regs("terrain_point_distance", terrain_point_distance, terrain_point_distance__t6502);
    fails += test_mem_contract_regs("terrain_midpoint_displace", terrain_midpoint_displace, terrain_midpoint_displace__t6502);
    fails += test_mem_contract_regs("terrain_plot_pixel", terrain_plot_pixel, terrain_plot_pixel__t6502);
    fails += test_mem_contract_regs("terrain_clip_row_top", terrain_clip_row_top, terrain_clip_row_top__t6502);
    fails += test_raster_fill_region();
    fails += test_terrain_sub_obj("terrain_plot_object_a", terrain_plot_object_a, terrain_plot_object_a__t6502);
    fails += test_terrain_sub_obj("terrain_plot_object_b", terrain_plot_object_b, terrain_plot_object_b__t6502);
    fails += test_terrain_sub_obj("terrain_plot_object", terrain_plot_object, terrain_plot_object__t6502);
    fails += test_from_snapshot("terrain_column_rasterize", terrain_column_rasterize,
                                terrain_column_rasterize__t6502, 4000, 0xFF);
    fails += test_from_snapshot("terrain_subdivide_column", terrain_subdivide_column,
                                terrain_subdivide_column__t6502, 2000, 0x0F);
    fails += test_mem_contract("terrain_jitter_column", terrain_jitter_column, terrain_jitter_column__t6502);
    fails += test_mem_contract_regs("terrain_frame_setup", terrain_frame_setup, terrain_frame_setup__t6502);
    /* project_terrain_points: snapshot-driven (random mem can make divide_16x16's
       normalize loop spin forever on a pathological divisor — same hang in both
       runs, so a timeout can't diff them).  X masked to the real column range.
       Excludes divide_16x16's scratch ($AE/$AF/$B0/$B1/$B2) from the contract:
       proven dead after return (no reader before the next overwrite — only
       divide_16x16 + project_terrain_points touch those cells), so the native
       version calls divide_16x16_core() and need not leave the 6502 remainder/
       shifted-divisor residue behind. */
    static const uint16_t ptp_dead_scratch[] = { 0x00AE, 0x00AF, 0x00B0, 0x00B1, 0x00B2 };
    set_ignore(ptp_dead_scratch, 5);
    fails += test_from_snapshot("project_terrain_points", project_terrain_points,
                                project_terrain_points__t6502, 4000, 0x3F);
    set_ignore(0, 0);
    /* terrain_collision_and_silhouette: the $B141 column-raster loop only terminates on real
       terrain tables (random mem can chain forever via $BE00).  Snapshot-driven;
       entry X (column start) masked 0..$3F — all verified hang-free. */
    fails += test_from_snapshot("terrain_collision_and_silhouette", terrain_collision_and_silhouette,
                                terrain_collision_and_silhouette__t6502, 2000, 0x3F);
    fails += test_terrain_draw_frame();

    printf("\n%s%s\n", fails == 0 ? "PASS — " : "FAIL — ",
        fails == 0
        ? (g_nfilter ? "selected native reimplementations are memory-equivalent to their 6502 oracles."
                     : "all native reimplementations are memory-equivalent to their 6502 oracles.")
        : "a native version diverges from its 6502 oracle (see diffs above).");
    return fails == 0 ? 0 : 1;
}
