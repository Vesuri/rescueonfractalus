/* terr_blend_test — host proof that the register-hoisted terr_blend is byte-identical
 * to the transliterated mem[]-in-place version, over ALL 2^24 (fraction, lo, hi) inputs.
 *
 * The transliterated blend keeps its three operands in mem[] scratch and shifts them in
 * place, costing 8 volatile byte accesses per iteration.  The hoisted version keeps them
 * in registers.  That is only legal if the FINAL mem[] state still matches, so this also
 * checks the claim the rewrite rests on: after 8 iterations the fraction has been shifted
 * left 8 times and each operand right 8 times, so all three cells end at 0 regardless of
 * their inputs.
 *
 *   cc -O2 -o /tmp/tbt tools/terr_blend_test.c && /tmp/tbt
 */
#include <stdint.h>
#include <stdio.h>

static uint8_t mem[0x10000];

/* --- the current in-place version, verbatim from rof_native.c --- */
static uint8_t blend_mem(uint16_t fa, uint16_t lo, uint16_t hi) {
    uint8_t A = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t f = mem[fa];
        uint8_t bit = (uint8_t)(f >> 7);
        mem[fa] = (uint8_t)(f << 1);
        if (bit) {
            mem[lo] = (uint8_t)(mem[lo] >> 1);
            uint8_t h = mem[hi], c = (uint8_t)(h & 1);
            mem[hi] = (uint8_t)(h >> 1);
            A = (uint8_t)(A + mem[hi] + c);
        } else {
            mem[hi] = (uint8_t)(mem[hi] >> 1);
            uint8_t l = mem[lo], c = (uint8_t)(l & 1);
            mem[lo] = (uint8_t)(l >> 1);
            A = (uint8_t)(A + mem[lo] + c);
        }
    }
    return A;
}

/* --- the proposed register-hoisted version --- */
static uint8_t blend_reg(uint16_t fa, uint16_t lo, uint16_t hi) {
    uint8_t f = mem[fa], L = mem[lo], H = mem[hi], A = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t bit = (uint8_t)(f >> 7);
        f = (uint8_t)(f << 1);
        if (bit) {
            L = (uint8_t)(L >> 1);
            uint8_t c = (uint8_t)(H & 1);
            H = (uint8_t)(H >> 1);
            A = (uint8_t)(A + H + c);
        } else {
            H = (uint8_t)(H >> 1);
            uint8_t c = (uint8_t)(L & 1);
            L = (uint8_t)(L >> 1);
            A = (uint8_t)(A + L + c);
        }
    }
    mem[fa] = 0; mem[lo] = 0; mem[hi] = 0;   /* all three shift out to 0 after 8 steps */
    return A;
}

int main(void) {
    const uint16_t FA = 0x27FA, LO = 0x27F0, HI = 0x27F1;
    long bad_ret = 0, bad_mem = 0, n = 0;

    for (int f = 0; f < 256; f++)
        for (int l = 0; l < 256; l++)
            for (int h = 0; h < 256; h++) {
                mem[FA] = (uint8_t)f; mem[LO] = (uint8_t)l; mem[HI] = (uint8_t)h;
                uint8_t ra = blend_mem(FA, LO, HI);
                uint8_t ma = mem[FA], ml = mem[LO], mh = mem[HI];

                mem[FA] = (uint8_t)f; mem[LO] = (uint8_t)l; mem[HI] = (uint8_t)h;
                uint8_t rb = blend_reg(FA, LO, HI);

                if (ra != rb) {
                    if (!bad_ret++)
                        printf("RET  f=%02x lo=%02x hi=%02x: mem=%02x reg=%02x\n", f, l, h, ra, rb);
                }
                if (ma != mem[FA] || ml != mem[LO] || mh != mem[HI]) {
                    if (!bad_mem++)
                        printf("MEM  f=%02x lo=%02x hi=%02x: mem=%02x/%02x/%02x reg=%02x/%02x/%02x\n",
                               f, l, h, ma, ml, mh, mem[FA], mem[LO], mem[HI]);
                }
                if (ma | ml | mh) {
                    printf("!! in-place version did NOT end at 0: f=%02x lo=%02x hi=%02x -> %02x/%02x/%02x\n",
                           f, l, h, ma, ml, mh);
                    return 1;
                }
                n++;
            }

    printf("%ld cases: %ld return mismatches, %ld mem mismatches\n", n, bad_ret, bad_mem);
    printf("in-place scratch ended at 0 in all %ld cases (the rewrite's precondition)\n", n);
    return (bad_ret || bad_mem) ? 1 : 0;
}
