/* test_xex_sparse.c — prove the C sparse loader == the C segment loader, on the host.
 *
 *   cc -O2 -o /tmp/test_xex_sparse tools/test_xex_sparse.c
 *   /tmp/test_xex_sparse <zeroed.xex> <sparse.bin>
 *
 * The Amiga cannot answer this by A/B: the sparse asset is 15 KB smaller, so the binary is a
 * different size, and a different-sized binary does a different number of main-loop
 * iterations per vblank and diverges the read-clocked $D20A LFSR (docs/asset-extraction.md
 * §3.1).  So instead of comparing two RUNS, compare the two LOADERS as pure functions over
 * the same target image — which is deterministic and takes a millisecond.
 *
 * Checks that xex_sparse_stage reproduces xex_parse_stage's mem[] byte-for-byte:
 *   - at every one of the four INITAD stage boundaries (the boot scenes read those states)
 *   - after the full non-staged load (xex_sparse_all vs xex_parse)
 *   - that both report the same number of stages
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/xex_load.h"

static uint8_t img_seg[65536];
static uint8_t img_spr[65536];
static uint8_t* cur;

static void wr(uint16_t start, const uint8_t* src, uint32_t count)
{
    for (uint32_t k = 0; k < count; k++) cur[(uint16_t)(start + k)] = src[k];
}

static uint8_t* slurp(const char* path, uint32_t* len)
{
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* b = malloc((size_t)n);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "short read\n"); exit(2); }
    fclose(f);
    *len = (uint32_t)n;
    return b;
}

static int cmp_img(const char* what)
{
    if (!memcmp(img_seg, img_spr, 65536)) { printf("  %-28s OK\n", what); return 0; }
    int first = -1, n = 0;
    for (int k = 0; k < 65536; k++)
        if (img_seg[k] != img_spr[k]) { if (first < 0) first = k; n++; }
    printf("  %-28s MISMATCH: %d bytes, first $%04X (seg=%02x sparse=%02x)\n",
           what, n, first, img_seg[first], img_spr[first]);
    return 1;
}

int main(int argc, char** argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s <zeroed.xex> <sparse.bin>\n", argv[0]); return 2; }
    uint32_t xlen, slen;
    uint8_t* xex = slurp(argv[1], &xlen);
    uint8_t* spr = slurp(argv[2], &slen);
    int bad = 0;

    printf("segment asset %u B, sparse asset %u B (%.1f%% smaller)\n",
           xlen, slen, 100.0 * (double)(xlen - slen) / (double)xlen);

    /* --- staged: compare mem[] at each INITAD boundary, as rof_boot.c sees it --- */
    memset(img_seg, 0, sizeof img_seg);
    memset(img_spr, 0, sizeof img_spr);
    uint32_t xo = 0, so = 0, stage = 0;
    while (xo < xlen || so < slen) {
        uint32_t xn = xo, sn = so;
        if (xo < xlen) { cur = img_seg; xn = xex_parse_stage(xex, xlen, xo, wr); }
        if (so < slen) { cur = img_spr; sn = xex_sparse_stage(spr, slen, so, wr); }
        stage++;
        char lbl[48];
        snprintf(lbl, sizeof lbl, "stage %u boundary", stage);
        bad |= cmp_img(lbl);
        if (xn <= xo && sn <= so) break;
        xo = xn; so = sn;
        if (stage > 16) { printf("  runaway stage count\n"); bad = 1; break; }
    }
    printf("  stages walked: %u (expected 4)\n", stage);
    if (stage != 4) bad = 1;

    /* --- non-staged: the one-shot full load --- */
    memset(img_seg, 0, sizeof img_seg);
    memset(img_spr, 0, sizeof img_spr);
    cur = img_seg; xex_parse(xex, xlen, wr);
    cur = img_spr; xex_sparse_all(spr, slen, wr);
    bad |= cmp_img("full one-shot load");

    printf(bad ? "FAIL\n" : "PASS: the sparse loader reproduces the segment loader exactly\n");
    return bad;
}
