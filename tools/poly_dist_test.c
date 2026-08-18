/* poly_dist_test — host proof for PlatformAmiga.cpp's poly5-gated poly4 rendering (AUDC $40).
 *
 * WHAT IT PROVES, over all 15 x 31 = 465 poly stride residues:
 *   (1) FIDELITY.  Each rendered byte matches atari800's rule, transcribed independently from
 *       tmp/atari800/src/pokeysnd.c:797-807 — the poly5 gate encloses the poly4 case, and inside
 *       the gate the 2-level output flips toward the poly4 bit:
 *           if ((audc & NOTPOLY5) || bit5[P5]) { ... else if (audc & POLY4)
 *                   toggle = (bit4[P4] == !out); }
 *   (2) SEAMLESS LOOP.  Paula repeats the buffer forever, so the 930 emitted samples must be a
 *       whole number of cycles of the state machine.  They are NOT if you start emitting at
 *       (p4,p5,out) = (0,0,0): inside the gate the output is FORCED to the poly4 bit, so the map
 *       is not invertible and the start state need not lie on its own cycle.  Skipping the
 *       transient first fixes it.  The proof checks three separate things:
 *         a. the transient is at most POLY4G_SKIP samples for every stride;
 *         b. every reachable cycle length divides POLY4G_LEN;
 *         c. concretely, rendering 3x POLY4G_LEN continuously equals the buffer repeated 3x.
 *       It also reports how many strides FAIL (2c) without the skip, so a future reader can see
 *       the skip is load-bearing and not a superstition.
 *
 * !! SNAPSHOT !!  `subject_render` is a verbatim copy of build_poly_dist's POLY_DIST_P4_GATED
 * arm (with the two rof_* helpers inlined as plain C, which is what they compile to).  Green
 * proves the renderer is right, NOT that the shipping source still matches this copy.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

/* the two poly bit streams, verbatim from PlatformAmiga.cpp */
static const uint8_t kBit4[15] = { 1,1,1,1,0,0,0,1,0,0,1,1,0,1,0 };
static const uint8_t kBit5[31] = { 1,1,1,1,0,1,1,0,1,0,0,1,1,0,0,0,0,0,1,1,1,0,0,1,0,0,0,1,0,1,0 };

#define POLY4G_LEN   930
#define POLY4G_SKIP    6

/* ---- SNAPSHOT of build_poly_dist's POLY_DIST_P4_GATED arm ----------------------------- */
static void subject_render(uint8_t* dst, uint16_t s4, uint16_t s5, int skip)
{
    uint16_t p4 = 0, p5 = 0;
    uint8_t  out = 0;
    for (int i = 0; i < skip + POLY4G_LEN; i++) {
        p4 = (uint16_t)(p4 + s4); if (p4 >= 15u) p4 = (uint16_t)(p4 - 15u);
        p5 = (uint16_t)(p5 + s5); if (p5 >= 31u) p5 = (uint16_t)(p5 - 31u);
        if (kBit5[p5]) {
            if (kBit4[p4] == (out ^ 1u)) out ^= 1u;
        }
        if (i >= skip) dst[i - skip] = out ? 0x7Fu : 0x81u;
    }
}

/* ---- ORACLE: atari800 pokeysnd.c, transcribed ----------------------------------------- */
/* One free-running step of the channel's output for AUDC distortion $40 (POLY4 set, PURETONE
   clear, NOTPOLY5 clear).  Returns the new output level. */
static int oracle_step(int* p4, int* p5, int out, int s4, int s5)
{
    *p4 = (*p4 + s4) % 15;
    *p5 = (*p5 + s5) % 31;
    if (kBit5[*p5]) {                       /* (audc & NOTPOLY5) is false for $40 */
        if (kBit4[*p4] == !out) out = !out;  /* toggle = (bit4[P4] == !out) */
    }
    return out;
}

int main(void)
{
    static uint8_t buf[POLY4G_LEN];
    long long checked = 0;
    int  worst_transient = 0, worst_cycle = 0, unskipped_bad = 0;

    for (int s4 = 0; s4 < 15; s4++) {
        for (int s5 = 0; s5 < 31; s5++) {
            /* --- (2a)(2b) walk the state machine to find its transient and cycle length --- */
            static int seen[15][31][2];
            for (int a = 0; a < 15; a++) for (int b = 0; b < 31; b++)
                seen[a][b][0] = seen[a][b][1] = -1;
            int p4 = 0, p5 = 0, out = 0, i = 0;
            while (seen[p4][p5][out] < 0) {
                seen[p4][p5][out] = i;
                out = oracle_step(&p4, &p5, out, s4, s5);
                i++;
            }
            int transient = seen[p4][p5][out];
            int cycle     = i - transient;
            if (transient > worst_transient) worst_transient = transient;
            if (cycle     > worst_cycle)     worst_cycle     = cycle;
            if (transient > POLY4G_SKIP) {
                printf("TRANSIENT %d > POLY4G_SKIP %d at s4=%d s5=%d\n",
                       transient, POLY4G_SKIP, s4, s5);
                return 1;
            }
            if (POLY4G_LEN % cycle != 0) {
                printf("CYCLE %d does not divide POLY4G_LEN %d at s4=%d s5=%d\n",
                       cycle, POLY4G_LEN, s4, s5);
                return 1;
            }

            /* --- (1)(2c) render, then walk the oracle 3x the buffer continuously ---------- */
            subject_render(buf, (uint16_t)s4, (uint16_t)s5, POLY4G_SKIP);
            p4 = 0; p5 = 0; out = 0;
            for (int k = 0; k < POLY4G_SKIP; k++) out = oracle_step(&p4, &p5, out, s4, s5);
            for (int k = 0; k < 3 * POLY4G_LEN; k++) {
                out = oracle_step(&p4, &p5, out, s4, s5);   /* sample k is the level AFTER step k */
                uint8_t want = out ? 0x7Fu : 0x81u;
                if (buf[k % POLY4G_LEN] != want) {
                    printf("MISMATCH s4=%d s5=%d sample %d: buf=$%02X want=$%02X\n",
                           s4, s5, k, buf[k % POLY4G_LEN], want);
                    return 1;
                }
                checked++;
            }

            /* --- how many strides the skip actually rescues (reported, not asserted) ------ */
            uint8_t nb[POLY4G_LEN];
            subject_render(nb, (uint16_t)s4, (uint16_t)s5, 0);
            p4 = 0; p5 = 0; out = 0;
            for (int k = 0; k < 2 * POLY4G_LEN; k++) {
                out = oracle_step(&p4, &p5, out, s4, s5);
                if (nb[k % POLY4G_LEN] != (out ? 0x7Fu : 0x81u)) { unskipped_bad++; break; }
            }
        }
    }
    printf("%lld samples over 465 strides, 0 mismatches; max transient %d (skip %d), "
           "max cycle %d | %d/465 strides would not loop cleanly without the skip\n",
           checked, worst_transient, POLY4G_SKIP, worst_cycle, unskipped_bad);
    return 0;
}
