/* terr_blend_table_test — host proof that terr_blend's 8-iteration bit-serial loop is a
 * SEPARABLE sum, so it can be replaced by two table lookups and one add.
 *
 * The loop is
 *     A = 0
 *     for i in 0..7:  bit = f>>7; f <<= 1
 *                     if bit: L>>=1; c = H&1; H>>=1; A += H + c
 *                     else:   H>>=1; c = L&1; L>>=1; A += L + c
 * and the key structural fact is that BOTH branches shift BOTH operands exactly once.  So at
 * the start of iteration i, L is L0>>i and H is H0>>i regardless of the path taken, and the
 * term added is
 *     T(X,i) = (X >> (i+1)) + ((X >> i) & 1)          with X = H if bit i is set, else L
 * i.e. round-half-up of (X>>i)/2.  A is a plain 8-bit accumulator, so the sum is mod 256 and
 * therefore order-independent:
 *
 *     A = ( SUM over i where f's bit (7-i) is SET   of T(H,i)
 *         + SUM over i where f's bit (7-i) is CLEAR of T(L,i) ) mod 256
 *
 * The second sum is the first one applied to ~f, because ~f's set bits are exactly f's clear
 * bits.  So with ONE 256x256 table
 *
 *     B[X][f] = SUM over i where f's bit (7-i) is set of T(X,i)   (mod 256)
 *
 * the whole loop collapses to
 *
 *     A = (uint8_t)(B[H][f] + B[L][(uint8_t)~f])
 *
 * This program checks that identity over ALL 2^24 (f, L, H) triples, and also checks the
 * 8 KB nibble-split form (two 256x16 tables) that reaches the same result without spending
 * another 64 KB of an A500's RAM:
 *
 *     A = (uint8_t)(Bhi[H][f>>4] + Blo[H][f&15] + Bhi[L][15-(f>>4)] + Blo[L][15-(f&15)])
 *
 *   cc -O2 -o /tmp/tbtt tools/terr_blend_table_test.c && /tmp/tbtt
 */
#include <stdint.h>
#include <stdio.h>

/* --- the shipping register version, verbatim from rof_native.c (mem[] stores dropped: this
 *     test is about the RETURN value; the mem[] contract is proven by terr_blend_test.c) --- */
static uint8_t blend_ref(uint8_t f, uint8_t L, uint8_t H) {
    uint8_t A = 0;
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
    return A;
}

/* T(X,i) = round-half-up of (X>>i)/2 — the term iteration i contributes. */
static uint8_t term(uint8_t X, int i) {
    return (uint8_t)((X >> (i + 1)) + ((X >> i) & 1));
}

static uint8_t B64[256u * 256u];          /* B64[(X<<8)|f] — the 64 KB single-table form */
static uint8_t Bhi[256u * 16u];           /* nibble split: bits 7..4 of f */
static uint8_t Blo[256u * 16u];           /*               bits 3..0 of f */

static void build(void) {
    for (unsigned X = 0; X < 256; X++) {
        for (unsigned f = 0; f < 256; f++) {
            uint8_t s = 0;
            for (int i = 0; i < 8; i++)
                if ((f >> (7 - i)) & 1) s = (uint8_t)(s + term((uint8_t)X, i));
            B64[(X << 8) | f] = s;
        }
        for (unsigned n = 0; n < 16; n++) {
            uint8_t sh = 0, sl = 0;
            /* high nibble of f = bits 7..4 -> iterations i = 0..3 */
            for (int i = 0; i < 4; i++) if ((n >> (3 - i)) & 1) sh = (uint8_t)(sh + term((uint8_t)X, i));
            /* low nibble of f = bits 3..0 -> iterations i = 4..7 */
            for (int i = 4; i < 8; i++) if ((n >> (7 - i)) & 1) sl = (uint8_t)(sl + term((uint8_t)X, i));
            Bhi[(X << 4) | n] = sh;
            Blo[(X << 4) | n] = sl;
        }
    }
}

int main(void) {
    build();
    unsigned long bad64 = 0, bad8k = 0, n = 0;
    for (unsigned f = 0; f < 256; f++) {
        for (unsigned L = 0; L < 256; L++) {
            for (unsigned H = 0; H < 256; H++) {
                uint8_t want = blend_ref((uint8_t)f, (uint8_t)L, (uint8_t)H);

                uint8_t got64 = (uint8_t)(B64[(H << 8) | f] + B64[(L << 8) | ((~f) & 0xFF)]);

                unsigned fh = f >> 4, fl = f & 15;
                uint8_t got8k = (uint8_t)(Bhi[(H << 4) | fh] + Blo[(H << 4) | fl]
                                        + Bhi[(L << 4) | (15u - fh)] + Blo[(L << 4) | (15u - fl)]);

                if (got64 != want) {
                    if (bad64 < 5) printf("  64K MISMATCH f=%02x L=%02x H=%02x want=%02x got=%02x\n",
                                          f, L, H, want, got64);
                    bad64++;
                }
                if (got8k != want) {
                    if (bad8k < 5) printf("  8K  MISMATCH f=%02x L=%02x H=%02x want=%02x got=%02x\n",
                                          f, L, H, want, got8k);
                    bad8k++;
                }
                n++;
            }
        }
    }
    printf("terr_blend table identity: %lu triples tested\n", n);
    printf("  64 KB single table  B[H][f] + B[L][~f]                : %lu mismatches\n", bad64);
    printf("  8 KB nibble split   Bhi/Blo x (H,f) + Bhi/Blo x (L,~f): %lu mismatches\n", bad8k);
    return (bad64 || bad8k) ? 1 : 0;
}
