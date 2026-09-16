/* rof_logo.c — scene 1, the Lucasfilm Games logo (boot INITAD $5000).
 *
 * NOT a transliteration.  The logo lives in XEX segment 5 ($5000-$536F), which segment 16
 * ($3CDE-$B7FF) overwrites before the image Ghidra disassembles — so it is absent from
 * disasm/listing.txt, absent from rof_gen.c, and cannot be reached by the single-listing
 * pipeline.  ⚠ symbols.csv's `stage_5000` names the GAME routine that ends up at that
 * address; it is NOT this scene.  (docs/logo-station-plan.md §0/§1.)
 *
 * So this file is hand-written from a disassembly of segment 5 taken straight out of
 * rof.xex.  Everything below carries the $5000-page address it came from.
 *
 * The Atari does not store the bitmap: it plots it from the run-length stroke data at $525F.
 * The port now replays that plotter at runtime too, so no derived Logo field is compiled into
 * the executable.  Everything else $5000 does — display list, timing, audio and sparkle — is
 * likewise ported faithfully below.
 */
#include "cpu/cpu.h"
#include "cpu/bus.h"
#include "gen/rof_decl.h"   /* rle_decompress ($3C3D), wait_vcount_ge_7a ($3C7B),
                               wait_frames_2 ($3CCA) — all in segments the logo survives with */
#include "platform/platform_c.h"
#include "rof_boot.h"

#define ROF_LOGO_FIELD_ADDR    0x60A3u
#define ROF_LOGO_FIELD_ROWS    62u
#define ROF_LOGO_FIELD_STRIDE  40u

/* See rof_boot.h for what the renderer does with these. */
volatile unsigned char g_logoFieldGen   = 0;
volatile unsigned char g_logoSparkleCol = 0;

/* Atari addresses this scene touches.  The three $52xx tables are read out of mem[] rather
 * than hard-coded: segment 5 IS resident while the logo runs (the staged loader has placed
 * segments 1..7 and nothing overwrites $5000-$536F until stage 4), so the seeds stay tied to
 * the binary instead of to a transcription of it. */
#define LOGO_ZP_SEEDS   0x5254u   /* -> ZP $90..$99: 56 7F 00 2A 01 22 02 00 03 87        */
#define LOGO_AUDF_STEP  0x5258u   /* the four AUDF per-frame increments, indexed by X=0,2,4,6 */
#define LOGO_DL_RLE     0x52E7u   /* the 11-byte RLE source for the display list            */
#define LOGO_DL         0x6000u   /* ...and where it unpacks to                             */
#define LOGO_PM_P0      0x0C00u   /* player 0's buffer (PMBASE $0800, one-line resolution)  */

/* ---------------------------------------------------------------------------------------
 *  $51EF — the VBI.  It replaces the OS vblank outright (it increments RTCLOK itself) and
 *  is the scene's clock: the two blocking waits in rof_logo_run() below are waiting on ZP
 *  $90 and $91, which only this routine moves.  Three phases:
 *    1. 86 frames ($90 counting down from $56): a four-channel POKEY sweep, each AUDF
 *       stepping by its own increment.  When $90 wraps past 0 to $FF it idles forever.
 *    2. 127 frames of silence ($91 counting down from $7F), ending in a one-shot
 *       AUDCTL/AUDF1/AUDF3 setup that arms...
 *    3. ...36 frames of the AUDC sweep alone ($92 from $24), after which $90 goes $FF.
 * ------------------------------------------------------------------------------------- */

/* $523C — the shared AUDC sweep: one value derived from $92 across all four channels, with
 * channel 3 offset by the CARRY out of the third LSR and channel 4 inverted in its top bits. */
static void logo_audc_sweep(void)
{
    const uint8_t v = mem[0x0092];
    uint8_t a = (uint8_t)((v >> 3) | 0xA0u);      /* $523C LDA $92 / LSR x3 / ORA #$A0 */
    bus_write(0xD201, a);                         /* $5243 AUDC1 */
    bus_write(0xD203, a);                         /* $5246 AUDC2 */
    a = (uint8_t)(a + ((v >> 2) & 1u));           /* $5249 ADC #$00 — ORA leaves C from LSR #3 */
    bus_write(0xD205, a);                         /* $524B AUDC3 */
    bus_write(0xD207, (uint8_t)(a ^ 0x60u));      /* $524E EOR #$60 / $5250 AUDC4 */
}

void rof_logo_vbi(void)
{
    mem[0x0014]++;                                /* $51EF INC $14 (RTCLOK_LOW) */

    if (mem[0x0090] != 0) {                       /* $51F1 LDY $90 / BNE $521F */
        int x;
        if (mem[0x0090] & 0x80u) return;          /* $521F BMI — past the end, stay silent */
        logo_audc_sweep();                        /* $5221 */
        mem[0x0092]++;                            /* $5224 */
        for (x = 6; x >= 0; x -= 2) {             /* $5226 X = 6,4,2,0 */
            const uint8_t f = mem[0x0093 + x];
            bus_write((uint16_t)(0xD200 + x), f); /* $522A AUDF1/2/3/4 */
            mem[0x0093 + x] = (uint8_t)(f + mem[LOGO_AUDF_STEP + x]);   /* $522E */
        }
        mem[0x0090]--;                            /* $5237 */
    } else if (mem[0x0091] & 0x80u) {             /* $51F5 LDA $91 / BMI $5213 */
        logo_audc_sweep();                        /* $5213 */
        if (--mem[0x0092] == 0) mem[0x0090]--;    /* $5216 — $90: 0 -> $FF, the scene's end */
    } else if (--mem[0x0091] == 0) {              /* $51F9 DEC $91 / BNE $5210 */
        bus_write(0xD208, 0x60);                  /* $51FD AUDCTL */
        bus_write(0xD200, 0xFA);                  /* $5202 AUDF1  */
        bus_write(0xD204, 0xFF);                  /* $5207 AUDF3  */
        mem[0x0092] = 0x24;                       /* $520C — the phase-3 countdown */
    }
    /* $5239 JMP $E462 (XITVBV) */
}

/* ---------------------------------------------------------------------------------------
 *  $5000 — the sequencer.
 * ------------------------------------------------------------------------------------- */

/* One frame of the scene's own time: the SPINWAIT_HOOK pair every blocking wait in the
 * transpiled tree uses.  On the Amiga the VBI body runs in the real INTB_VERTB ISR, so
 * platform_tick_vbi() is a no-op there and platform_render_frame() paints; on SDL the tick
 * fires rof_logo_vbi() through the registered $51EF vector.  Either way this is the opaque
 * call that keeps the mem[] spin conditions below from folding (src/cpu/cpu.h). */
static void logo_frame(void)
{
    platform_tick_vbi();
    platform_render_frame();
}

#ifdef ROF_LOGO_START_ABORT
/* Let START — or the joystick FIRE button — end the logo early, the way both already exit the
 * Station.  A DELIBERATE DIVERGENCE: the Atari's $5000 is unconditional and has no exit check
 * anywhere, and it was opt-in at first; the user's call was that the quality-of-life win
 * outweighs faithfulness here, so it is ON by default.  `make LOGO_START=0` restores
 * the faithful blocking logo, and deliberately disables BOTH exits, not just START.
 *
 * Fire is read as TRIG0 rather than folded into CONSOL so this stays platform-neutral: SDL's
 * hwRead serves $D010 from readTrigger(0) and the Amiga's from the real port-1 button.  (The
 * Station needs the opposite treatment — its exit check is transliterated code that never reads
 * TRIG0 at all, so PlatformAmiga::hwRead reports fire AS START there.  See that comment.) */
static int logo_aborted(void)
{
    if ((bus_read(0xD01F) & 0x01u) == 0u) return 1;   /* CONSOL bit0 = START, active-low */
    return bus_read(0xD010) == 0u;                    /* TRIG0 = fire, active-low ($00 = pressed) */
}
#else
static int logo_aborted(void) { return 0; }
#endif

/* The original $5111 stroke plotter.  This used to be represented by a 2,623-byte compiled
 * bitmap.  Replaying it from the cartridge-sourced stream at $525F removes that original-game
 * payload from the executable while preserving the pixels exactly. */
static void logo_plot_nibble(unsigned col, unsigned row, unsigned shade)
{
    uint16_t a = (uint16_t)(0x6053u + row * 40u + (col >> 1));
    uint8_t v = mem[a];
    mem[a] = (uint8_t)((col & 1u) ? ((v & 0xF0u) | shade)
                                    : ((v & 0x0Fu) | (shade << 4)));
}

static void logo_plot_pair(unsigned col, unsigned row, unsigned shade)
{
    logo_plot_nibble(col, row - 1u, shade);
    logo_plot_nibble(col, row, shade);
}

static void logo_plot_brush(unsigned col, unsigned row, unsigned shade, int fat)
{
    if (!fat) {
        logo_plot_pair(col, row, shade);
    } else {
        logo_plot_pair(col,     row - 2u, shade);
        logo_plot_pair(col + 1, row - 2u, shade);
        logo_plot_pair(col + 1, row,      shade);
        logo_plot_pair(col,     row,      shade);
    }
}

static unsigned logo_plot_cell(uint8_t pattern, unsigned col, unsigned row,
                               unsigned pass, int fat)
{
    unsigned shade;
    unsigned x = col + 8u;
    int bit;
    if (pass)
        shade = (pass == 2u) ? 2u : ((row >> 2) == 8u ? 7u : row >> 2);
    else
        shade = ((((row - 2u) & 0xFFu) >> 2) ^ 0x0Fu);
    for (bit = 0; bit < 8; bit++) {
        --x;
        if (pattern & 1u) logo_plot_brush(x, row, shade, fat);
        pattern >>= 1;
    }
    return row + 2u;
}

static unsigned logo_plot_strip(unsigned stream, unsigned col, unsigned row,
                                unsigned pass, int fat)
{
    uint8_t pattern = mem[0x525Fu + stream++];
    unsigned run = 1;
    for (;;) {
        unsigned n;
        for (n = 0; n < run; n++) row = logo_plot_cell(pattern, col, row, pass, fat);
        {
            uint8_t code = mem[0x525Fu + stream];
            unsigned high;
            if (code == 0) return stream + 1u;
            run = code & 0x1Fu;
            high = code >> 5;
            if (high == 0) {
                pattern = mem[0x525Fu + stream + 1u];
                stream += 2u;
            } else {
                pattern = mem[0x52DFu + high];
                stream++;
            }
        }
    }
}

static void logo_plot_word(unsigned count, unsigned stream, unsigned col, unsigned row,
                           unsigned pass, int fat)
{
    while (count--) {
        stream = logo_plot_strip(stream, col, row, pass, fat);
        col += 8u;
    }
}

static void logo_paste_lucasfilm(void)
{
    unsigned i;
    for (i = 0; i < ROF_LOGO_FIELD_ROWS * ROF_LOGO_FIELD_STRIDE; i++)
        mem[ROF_LOGO_FIELD_ADDR + i] = 0;
    logo_plot_word(9, 0x00, 5, 4, 2, 1);
    logo_plot_word(9, 0x00, 4, 3, 1, 1);
    logo_plot_word(9, 0x00, 4, 3, 0, 0);
    g_logoFieldGen++;
}

/* $50B4/$50BB — the two passes that add "GAMES" 86 frames after the reveal. */
static void logo_paste_games(void)
{
    logo_plot_word(4, 0x66, 20, 0x36, 2, 0);
    logo_plot_word(4, 0x66, 19, 0x35, 1, 0);
    g_logoFieldGen++;
}

/* COLPM0 ($D012).  Written faithfully for SDL (which renders GTIA straight out of mem[]) and
 * mirrored into g_logoSparkleCol for the Amiga, where $D012 is not a mem[] shadow. */
static void logo_colpm0(uint8_t v)
{
    bus_write(0xD012, v);
    g_logoSparkleCol = v;
}

void rof_logo_run(void)
{
    unsigned i;
    int      y;

    platform_register_vbi(0x51EF, rof_logo_vbi);   /* SDL's dispatch table; inert on Amiga,
                                                      whose ISR switches on VVBLKI directly */

    /* ---- $5000-$500D: silence, kill NMIs, park the VBI ---------------------------------- */
    mem[0x0090] = 0xFF;               /* $5000 — "$51EF: do nothing yet" (its BMI idle path)  */
    bus_write(0xD208, 0x00);          /* $5005 AUDCTL = 0                                     */
    bus_write(0xD40E, 0x00);          /* $5008 NMIEN  = 0                                     */
    mem[0x0078] = 0x00;               /* $500B — the plotter's thin/fat brush flag            */
    mem[0x00BD] = 0x00;               /* $500D — rle_decompress's destination lo              */

    /* ---- $500F-$501B: unpack the display list ------------------------------------------- */
    /* 11 bytes -> 8 x $70 (64 blank scanlines), `4F A3 60` (mode F, LMS $60A3), 61 x $0F,
       `41 00 60` (JVB $6000).  Only the DL: the bitmap is drawn, never unpacked. */
    mem[0x00BB] = (uint8_t)(LOGO_DL_RLE & 0xFFu);   /* $500F source lo */
    mem[0x00BC] = (uint8_t)(LOGO_DL_RLE >> 8);      /* $5013 source hi */
    mem[0x00BE] = (uint8_t)(LOGO_DL >> 8);          /* $5017 dest hi ($BD already 0) */
    rle_decompress();                               /* $501B JSR $3C3D */

    /* ---- $501E-$502A: point ANTIC at it, with the PLAYFIELD OFF -------------------------- */
    bus_write(0xD402, (uint8_t)(LOGO_DL & 0xFFu));  /* $5020 DLISTL */
    bus_write(0xD403, (uint8_t)(LOGO_DL >> 8));     /* $5025 DLISTH */
    bus_write(0xD400, 0x20);                        /* $502A DMACTL = display-list DMA ONLY —
                                                       the screen stays blank until the reveal */

    /* ---- $502D-$5034: VVBLKI = $51EF ----------------------------------------------------- */
    bus_write(0x0222, 0xEF);
    bus_write(0x0223, 0x51);

    /* ---- $5037-$5048: the stroke plotter's 64-entry row-pointer table -------------------- */
    /* ZP $7F/$BF = $6053 + 40n.  Unused here (nothing but $5111 reads it), but it is $5000's
       own work and it survives into the next stage — ZP is not in any segment the loader
       places after this one — so write it and keep mem[] identical to the Atari's.  The base
       is the screen minus two rows: deliberate slack, because the plotter's fat brush reaches
       up to three scanlines above the cell it is drawing. */
    {
        uint16_t p = 0x6053u;
        for (i = 0; i < 0x40u; i++, p += 40u) {
            mem[0x007F + i] = (uint8_t)(p & 0xFFu);
            mem[0x00BF + i] = (uint8_t)(p >> 8);
        }
    }

    /* ---- $504A-$5061: draw "LUCASFILM" --------------------------------------------------- */
    mem[0x007D] = 0x02;               /* $504C — pass counter, selects the shade formula      */
    mem[0x007B] = 0x04;               /* $5050 — first row of the shadow pass                 */
    logo_paste_lucasfilm();           /* $5053 + $505A + $5061: three JSR $5111 passes        */
    mem[0x007B] = 0x03;               /* $5056 DEC $7B — passes 2 and 3 sit one row higher    */
    mem[0x0078] = 0x01;               /* $505D INC $78 — pass 3 uses the thin brush           */
    mem[0x007D] = 0xFF;               /* the three DEC $7D at $5131: 2 -> 1 -> 0 -> $FF       */

    /* ---- $5064-$5085: THE REVEAL --------------------------------------------------------- */
    bus_write(0xD407, 0x08);          /* $5066 PMBASE = $0800 -> player 0 at $0C00            */
    bus_write(0xD01B, 0x41);          /* $506B PRIOR = GTIA mode 9 + priority 1               */
    wait_vcount_ge_7a();              /* $506E JSR $3C7B — scanline sync (no-op on Amiga)     */
    bus_write(0xD01A, 0x10);          /* $5073 COLBK  = hue 1 (gold), luminance 0             */
    bus_write(0xD018, 0x10);          /* $5076 COLPF2 = the same                              */
    bus_write(0xD400, 0x3A);          /* $507B DMACTL = playfield + player DMA: the picture
                                              appears all at once, already finished           */
    bus_write(0xD40E, 0x40);          /* $5080 NMIEN = VBI enabled -> $51EF starts running    */
    bus_write(0xD01D, 0x02);          /* $5085 GRACTL = players only                          */

    /* ---- $5088-$50A4: arm the VBI, and build the sparkle's shape ------------------------- */
    for (i = 0; i < 10u; i++)                                     /* $508A LDA $5254,X */
        mem[0x0090 + i] = mem[LOGO_ZP_SEEDS + i];                 /* $508D STA $90,X   */
    for (i = 0; i <= 0x0Eu; i++) mem[LOGO_PM_P0 + 0x40 + i] = 0x10;  /* $5096 — a 15-row bar */
    for (i = 0; i <= 0x04u; i++) mem[LOGO_PM_P0 + 0x45 + i] = 0x38;  /* $50A0 — widened in
                                                                        the middle: a "✦"   */

    /* ---- $50A6: hold "LUCASFILM" for 86 frames ------------------------------------------- */
    while (mem[0x0090] != 0) {        /* $50A6 LDA $90 / BNE $50A6 */
        logo_frame();
        if (logo_aborted()) goto done;
    }

    /* ---- $50AA-$50BB: add "GAMES" -------------------------------------------------------- */
    mem[0x007D] = 0x02;               /* $50AC */
    mem[0x007B] = 0x36;               /* $50B0 — 10 scanlines, well below "LUCASFILM"         */
    logo_paste_games();               /* $50B4 + $50BB: two JSR $5117 passes                  */
    mem[0x007B] = 0x35;               /* $50B7 DEC $7B                                        */
    mem[0x007D] = 0x00;               /* the two DEC $7D                                      */

    /* ---- $50BE: ...and hold the finished logo until the VBI's phase 3 ends ---------------- */
    while ((mem[0x0091] & 0x80u) == 0) {   /* $50BE LDA $91 / BPL $50BE */
        logo_frame();
        if (logo_aborted()) goto done;
    }

    /* ---- $50C2-$5110: the sparkle -------------------------------------------------------- */
    /* Player 0 fades $0F -> $00 over 16 steps of two frames while its shape is eaten away from
       both ends at once: one row off the bottom and one off the top per step, plus — for the
       first four steps only — the five widened middle rows narrowed by a mask closing in from
       both sides ($80 shifting right, $81 shifting left). */
    mem[0x0080] = 0xFF;                             /* $50C2 */
    mem[0x0081] = 0xFE;                             /* $50C7 ASL A / $50C8 */
    mem[LOGO_PM_P0 + 0x47] = 0xFE;                  /* $50CA — the star's full-width centre */
    bus_write(0xD000, 0xC1);                        /* $50CF HPOSP0 */
    logo_colpm0(0x08);                              /* $50D4 COLPM0 = $08 */
    wait_frames_2();                                /* $50D7 JSR $3CCA */
    mem[0x0082] = 0xFF;                             /* $50DC STX $82 — once, above the loop */
    for (y = 0x0F; y >= 0; y--) {                   /* $50DA LDY #$0F ... $5108 DEY / BPL */
        if (logo_aborted()) goto done;
        logo_colpm0((uint8_t)y);                    /* $50DE STY $D012 */
        wait_frames_2();                            /* $50E1 JSR $3CCA */
        mem[LOGO_PM_P0 + 0x40 + y] = 0x00;          /* $50E6 — erase from the bottom up */
        mem[LOGO_PM_P0 + 0x40 + mem[0x0082]] = 0;   /* $50EB — ...and from the top down.  The
                                                       first pass has $82 = $FF, so it writes
                                                       $0D3F — player 1's page, which is empty
                                                       and never enabled.  Faithful. */
        mem[0x0082]++;                              /* $50EE */
        if (mem[0x0082] <= 3u) {                    /* $50F2 CPX #$03 vs $82 / BCC */
            const uint8_t mask = (uint8_t)(mem[0x0080] & mem[0x0081]);
            int x;
            for (x = 4; x >= 0; x--)                /* $50F7 X = 4..0 */
                mem[LOGO_PM_P0 + 0x45 + x] &= mask;
            mem[0x0080] >>= 1;                      /* $5104 LSR $80 */
            mem[0x0081] <<= 1;                      /* $5106 ASL $81 */
        }
    }

done:
    bus_write(0xD000, 0x00);          /* $510D — park player 0 off screen; $5110 RTS */
    logo_colpm0(0x00);                /* Not $5000's — but a no-op on the faithful path, where
                                         the fade above already left COLPM0 at 0.  It exists so
                                         a LOGO_START abort mid-fade doesn't hand the Amiga
                                         sprite mirror a lit colour on its way out. */
}
