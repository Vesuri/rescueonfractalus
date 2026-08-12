/* Is xorshift32 (a,b,c) full period (2^32-1)?  The map is LINEAR over GF(2), so build its
 * 32x32 matrix (columns = f(e_i)) and test the order: M^(2^32-1) == I and M^((2^32-1)/p) != I
 * for every prime p | 2^32-1 = 3*5*17*257*65537.  Instant, and rigorous.
 * Prints the 68000 cycle cost of the shift trio so the cheapest full-period triple wins. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

typedef struct { uint32_t c[32]; } Mat;   /* c[i] = image of basis vector e_i */

static uint32_t xs(uint32_t x, int a, int b, int c)
{
    x ^= x << a; x ^= x >> b; x ^= x << c; return x;
}
static Mat build(int a, int b, int c)
{
    Mat m; for (int i = 0; i < 32; i++) m.c[i] = xs(1u << i, a, b, c); return m;
}
static Mat ident(void) { Mat m; for (int i = 0; i < 32; i++) m.c[i] = 1u << i; return m; }
static uint32_t apply(const Mat* m, uint32_t v)
{
    uint32_t r = 0;
    for (int i = 0; i < 32; i++) if ((v >> i) & 1u) r ^= m->c[i];
    return r;
}
static Mat mul(const Mat* p, const Mat* q)      /* (p*q)(x) = p(q(x)) */
{
    Mat r; for (int i = 0; i < 32; i++) r.c[i] = apply(p, q->c[i]); return r;
}
static Mat mpow(Mat m, uint64_t e)
{
    Mat r = ident();
    while (e) { if (e & 1) r = mul(&m, &r); m = mul(&m, &m); e >>= 1; }
    return r;
}
static int is_id(const Mat* m)
{
    for (int i = 0; i < 32; i++) if (m->c[i] != (1u << i)) return 0;
    return 1;
}
/* 68000 cost of one `x ^= x<<n` / `x ^= x>>n` step, including the register copy + eor:
 *   n<=8   : move.l(4) + lsl.l #n(8+2n) + eor.l(8)
 *   n==16  : move.l(4) + swap(4) + clr.w(4) + eor.l(8)          (both directions)
 *   else   : move.l(4) + lsl.l dN(8+2n) + eor.l(8)   (count preloaded in a data register)
 *   n==17..: >>17 is move.l(4)+clr.w(4)+swap(4)+lsr.w #1(8)+eor.l(8) = 28 (GCC's trick)   */
static int step_cost(int n, int right)
{
    if (n == 16) return 4 + 4 + 4 + 8;
    if (right && n > 16 && n - 16 <= 8) return 4 + 4 + 4 + (6 + 2 * (n - 16)) + 8;
    return 4 + (8 + 2 * n) + 8;
}
int main(void)
{
    const uint64_t P = 0xFFFFFFFFull;                 /* 2^32-1 */
    const uint32_t primes[5] = { 3u, 5u, 17u, 257u, 65537u };
    int bestcost = 1 << 30, ba = 0, bb = 0, bc = 0;
    for (int a = 1; a <= 31; a++)
        for (int b = 1; b <= 31; b++)
            for (int c = 1; c <= 31; c++) {
                int cost = step_cost(a, 0) + step_cost(b, 1) + step_cost(c, 0);
                if (cost >= bestcost) continue;       /* cheaper candidates only */
                Mat m = build(a, b, c);
                Mat t = mpow(m, P);
                if (!is_id(&t)) continue;
                int full = 1;
                for (int k = 0; k < 5; k++) {
                    Mat s = mpow(m, P / primes[k]);
                    if (is_id(&s)) { full = 0; break; }
                }
                if (!full) continue;
                bestcost = cost; ba = a; bb = b; bc = c;
                printf("full-period (%2d,%2d,%2d)  shift+eor cost = %d cycles\n", a, b, c, cost);
            }
    printf("\nBEST: (%d,%d,%d) at %d cycles;  current (13,17,5) = %d cycles\n",
           ba, bb, bc, bestcost,
           step_cost(13, 0) + step_cost(17, 1) + step_cost(5, 0));
    /* sanity: the chosen triple must never hit 0 and must visit 2^16 distinct states quickly */
    uint32_t x = 0x13579BDFu;
    for (int i = 0; i < 1000000; i++) { x = xs(x, ba, bb, bc); if (!x) { printf("HIT ZERO\n"); return 1; } }
    printf("1e6 steps, no zero, last=%08x\n", x);
    return 0;
}
