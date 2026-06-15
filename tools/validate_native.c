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
/* update_gauge_digits @ $548D — the apex.  The ring drain ($0719, head $0073 /
 * tail $0074, both decremented & wrapped at $1F) only terminates when the SFX
 * event-table $56D4 holds VALID voice-slot indices (1..14, bit7 clear): a bit7-set
 * ring entry runs input_init, whose tail game_sub_55FC re-pushes mem[$56D4+i] — if
 * that were bit7-set garbage it would re-dispatch input_init forever (the __t6502
 * twin hangs identically, so timeout can't diff it).  Seed $56D4 valid, the voice
 * register indices, and head/tail in 0..$1F; mask the POKEY range. */
static int test_update_gauge_digits(void) {
    if (!want("update_gauge_digits")) return 0;
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
        mem_fail += diff_run("update_gauge_digits", pre, zero_cpu(),
                             update_gauge_digits, update_gauge_digits__t6502, t, &printed, &cpu_diff);
    }
    set_ignore(0, 0);
    printf("update_gauge_digits: %d cases, %d mem mismatch (must be 0), %d cpu diffs\n",
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
    fails += test_mem_contract_regs("copy_altitude_graphic_to_screen",
                                    copy_altitude_graphic_to_screen,
                                    copy_altitude_graphic_to_screen__t6502);
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
    fails += test_mem_contract("fill_horizontal_span", fill_horizontal_span, fill_horizontal_span__t6502);
    fails += test_mem_contract_regs("plot_glyph_pixel_masked", plot_glyph_pixel_masked, plot_glyph_pixel_masked__t6502);
    fails += test_mem_contract_regs("plot_pixel_masked", plot_pixel_masked, plot_pixel_masked__t6502);
    fails += test_mem_contract_regs("set_row_ptr", set_row_ptr, set_row_ptr__t6502);
    fails += test_mem_contract("set_row_ptr_from_count", set_row_ptr_from_count, set_row_ptr_from_count__t6502);
    fails += test_mem_contract("fill_vertical_span", fill_vertical_span, fill_vertical_span__t6502);
    /* plot_pixel_2bpp: the 6502 PHA/PLAs X through the stack; the native version
       keeps it in a local, so the dead stack scratch ($0100-$01FF) is masked. */
    {
        static uint16_t stack_pg[256];
        for (int i = 0; i < 256; i++) stack_pg[i] = (uint16_t)(0x0100 + i);
        set_ignore(stack_pg, 256);
        fails += test_mem_contract_regs("plot_pixel_2bpp", plot_pixel_2bpp, plot_pixel_2bpp__t6502);
        set_ignore(0, 0);
    }
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
    fails += test_mem_contract("terrain_lookup", terrain_lookup, terrain_lookup__t6502);
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
    fails += test_update_gauge_digits();

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
    /* terrain_collision: the $B141 column-raster loop only terminates on real
       terrain tables (random mem can chain forever via $BE00).  Snapshot-driven;
       entry X (column start) masked 0..$3F — all verified hang-free. */
    fails += test_from_snapshot("terrain_collision", terrain_collision,
                                terrain_collision__t6502, 2000, 0x3F);
    fails += test_terrain_draw_frame();

    printf("\n%s%s\n", fails == 0 ? "PASS — " : "FAIL — ",
        fails == 0
        ? (g_nfilter ? "selected native reimplementations are memory-equivalent to their 6502 oracles."
                     : "all native reimplementations are memory-equivalent to their 6502 oracles.")
        : "a native version diverges from its 6502 oracle (see diffs above).");
    return fails == 0 ? 0 : 1;
}
