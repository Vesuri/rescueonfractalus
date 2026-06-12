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

/* Like test_mem_contract but with random entry A/X/Y/C — for routines that read
 * an entry register as input (a table index, a value to store, an entry carry). */
static int test_mem_contract_regs(const char *name, void (*native)(void), void (*t6502)(void)) {
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

/* terrain_draw_frame @ $A31E: snapshot-driven (its object loop drives terrain_subdivide_column /
 * terrain_column_rasterize, which only terminate on real terrain arrays).  Entry X
 * is the level base index — the two real call-site values are $00 and $30, which
 * also exercise both sides of the $A7-zero check; alternate them.  RANDOM ($D20A)
 * is read several times and seeded identically per case by diff_run. */
static int test_terrain_draw_frame(void) {
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

int main(void) {
    platform_test_init_headless();   /* enable seedable RANDOM ($D20A) for both runs */

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
    fails += test_mem_contract("compute_target_blip_position", compute_target_blip_position, compute_target_blip_position__t6502);
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

    printf("\n%s\n", fails == 0
        ? "PASS — all native reimplementations are memory-equivalent to their 6502 oracles."
        : "FAIL — a native version diverges from its 6502 oracle (see diffs above).");
    return fails == 0 ? 0 : 1;
}
