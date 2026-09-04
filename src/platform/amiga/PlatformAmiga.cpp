// PlatformAmiga — the Amiga-specific platform layer (see PlatformAmiga.h).
//
// This translation unit holds, in order:
//   1. the POKEY->Paula audio backend (the bulk of the file),
//   2. the platform_c.h bridge the C-compiled 6502 transliteration calls,
//   3. the launch-cinematic frame pump + quit handling,
//   4. the CIA-A serial-port keyboard (F1 -> Atari START switch),
//   5. the real INTB_VERTB VBI server (runs the per-frame game VBI body),
//   6. PlatformAmiga::run() — display takeover, install (1)/(4)/(5), run the scene,
//      then restore the system.
//
// AUDIO — frequency mapping (mirrors PlatformSDL::updateChannelFreq):
//   POKEY clock = 1789773 Hz (NTSC), base divider = 28 (64 kHz) or 114 (15 kHz).
//   AUDCTL bits: 0x01=15kHz, 0x08=CH0+CH1 16-bit chain, 0x10=CH2+CH3 chain,
//                0x20=CH0 uses 1.79 MHz, 0x40=CH2 uses 1.79 MHz.
//   Paula period for 2-sample square wave: PAULA_CLOCK / (2 * freq_hz).
//   Volume mapping: AUDC[3:0] * 4 -> Paula AUDxVOL (0..64); 0 if bit 4 set.
//
// Memory image: mem[] is populated by load_xex_image() (XexImage.cpp) from the
// pristine rof.xex before the scene initialises; the audio code only drives Paula.

#define ECS_SPECIFIC
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/cia.h>
#include <proto/dos.h>   // the ONLY OS file I/O in the port: the high-score file (hiscoreLoad/Save)
#include <exec/interrupts.h>
#include <exec/execbase.h>
#include "../../cpu/m68k_math.h"
#include <exec/nodes.h>
#include <exec/memory.h>
#include <graphics/gfxbase.h>
#include <graphics/view.h>
#include <hardware/dmabits.h>
#include <hardware/intbits.h>
#include <resources/cia.h>
#include <hardware/cia.h>
#include "framework/AmigaHardware.h"
#include "framework/CopperList.h"   // wraps GfxBase->copinit for the exit-time COP1LC restore
#include "PlatformAmiga.h"
#include "RescueOnFractalus.h"
#include "ExternalHooks.h"       // the launcher-patchable hiscore save/load hooks
#include "../../rof_hiscore.h"   // rof_hiscore_flush — the deferred save, after the OS is back
extern "C" volatile uint16_t g_atariDlist;   // DLISTL/DLISTH latch (bus.h) — which screen is up

// mem[] and cpu are defined in src/cpu/cpu.c (compiled for m68k as audio/cpu.o)
extern "C" volatile uint8_t mem[65536];

// Title Screen visual-test override: when set (only by the ROF_FORCE_TITLE probe path),
// deriveRenderSignals pins rsTitle true.  Defined unconditionally (default 0) so the normal
// build references a real symbol with zero effect.
extern "C" { volatile unsigned char g_forceTitleScreen = 0; }

#if defined(ROF_TITLE_START) && defined(ROF_FLIGHT_PROBE)
// Title-Screen -> START -> launch scene-routing trace (make PROBES=1 TITLE_START=1).  Filled from
// the vblank ISR every 2nd frame once the Title appears; dumped by amiga/title_start.gdb.
extern "C" volatile unsigned short g_tsRingVbi[96] = {0}, g_tsRingVV[96] = {0}, g_tsRingN = 0;
extern "C" volatile unsigned char  g_tsRing3A[96] = {0}, g_tsRing8D[96] = {0}, g_tsRing8E[96] = {0},
                                   g_tsRing60B[96] = {0}, g_tsRingCop[96] = {0}, g_tsRingFlg[96] = {0};
// ...and the gauge state at each routing transition: fuel $062F (drives the bar's Y) and the
// sprite VSTART the renderer actually published.  Lets the FIRST-boot Standby (correct) and the
// post-Title Standby (reported wrong) be diffed instead of reasoned about.
extern "C" volatile unsigned char  g_tsRing62F[96] = {0};
extern "C" volatile unsigned short g_tsRingSprY[96] = {0};
// ⚠ NOT a BPLCON2 read-back column: a CPU read of $DFF104 returns the floating bus, not the stored
// value (measured — it came back ffff/7f81/6441 varying frame to frame, while gdb's read of the
// same address returns the real value).  Sample the register from gdb at the rof_b2_mark()
// breakpoint instead; see amiga/b2_probe.gdb.
#endif

// GfxBase is opened in main() (GCCRuntime.cpp defines it); set before run() is called.
extern struct GfxBase* GfxBase;

// load_xex_image (XexImage.cpp): populate mem[] with the pristine rof.xex boot image.
extern "C" void load_xex_image(void);
// game_vbi_isr (flight_native.cpp): the per-frame VBI body, run from the real VBI below.
extern "C" void game_vbi_isr(void);

// POKEY distortion waveforms in chip RAM (Paula DMA must reach chip RAM).
// POKEY runs each channel's clock through a polynomial counter selected by the
// AUDC distortion bits (NOTPOLY5 $80, POLY4 $40, PURETONE $20).  A plain square
// only reproduces the pure-tone voices (melody, AUDC $A0); the Standby "bass"
// uses poly4 ($C0) and poly5-gated tone ($20), which have a different waveform
// and fundamental — rendering them as a square plays them off-key.
//
// The poly waveform is generated per note: POKEY's poly counters free-run at the
// base clock while the channel samples them every Div_n_max=(AUDF+1)*baseDiv
// ticks (atari800 pokeysnd.c), so the stride through the poly table is
// AUDF-dependent.  A fixed-stride table mistunes notes (e.g. poly4 at AUDF=215
// strides P4 by 3 → true period 5 fires, not 15).  We replay pokeysnd's toggle
// rule with the correct stride into a per-channel scratch buffer.  One captured
// byte = one channel fire, played by the same pokey_period() as the pure-tone
// path; bipolar ±127, AUDC volume scales amplitude via AUDxVOL.
// ⚠ Filled by audioInit(), NOT by a static initialiser: `.MEMF_CHIP` is linked as a chip
// BSS hunk (amiga/memf_chip_bss.ld), so an initialiser here would be silently DROPPED and
// every channel's default waveform would be two zero bytes = silence.  The build fails if
// any __chip variable carries one — see the CHIP-BSS guard in amiga/Makefile.
static __chip uint8_t wave_pure[2];                       // pure tone (square) = { $7F, $81 }
// Precomputed POKEY poly distortion waveforms.  A poly waveform's SHAPE depends only
// on the stride residue through the poly counter (s4 = stride%15 for the poly4 buzz,
// s5 = stride%31 for the poly5-gated tone) and the distortion mode — NOT on the channel
// or absolute pitch (pitch is carried separately by AUD_PER).  So there are only 15
// distinct poly4 waveforms and 31 distinct poly5-tone waveforms; build_poly_tables()
// builds all of them once at init and update_paula_channel just re-points Paula at the
// matching immutable buffer per note.  ⚠ Do NOT generate them on the fly (a per-note
// ping-pong buffer): the engine writes AUDF before AUDC, so a note's first repoint can land on
// a half-built or wrong-mode buffer that Paula then latches at the next DMA loop wrap — an
// audible glitch at note onset.  An immutable table cannot have that hazard.
// poly4 = 30 bytes (15 words); poly5tone = 62 bytes (31 words).
static __chip uint8_t poly4_wave[15][30];
static __chip uint8_t poly5_wave[31][62];

// POKEY distortion $00/$80 (PURE clear, POLY4 clear) is NOT white noise — it is a 2-level
// ±square whose output FOLLOWS the poly9 or poly17 bit at each frequency-divider underflow,
// optionally gated by poly5 ($00 = gated, $80 = ungated).  When AUDCTL bit7 (POLY9) is set
// the source is the short 511-bit poly9, which is tonal enough to loop cleanly — so we render
// the real poly5-gated poly9 waveform here (matching atari800 pokeysnd.c) instead of the flat
// noise_buf, restoring the punchy, pitched "buzz bass" the noise stream washed out.  Unlike
// poly4/poly5, the shape depends on BOTH the poly5 AND poly9 phase strides, so it can't be a
// per-stride table — it's regenerated per channel when the stride/gate changes (a menu-rate
// event, off the flight hot path).  Buffer = 2 poly9 periods (1022 B = 511 words, even for
// Paula LEN); poly9 realigns every 511 samples so the DMA-loop seam is a sub-audible gate blip.
// poly17 (AUDCTL bit7 clear) has no short loop and stays on the noise_buf fallback below.
#define POLY9_SIZE     511
#define POLY_DIST_LEN  1022
// Distortion $40 = poly4 GATED BY POLY5 shares the same per-channel buffer.  Its state is
// (p4, p5, out) — 15*31*2 = 930 — so 930 bytes is both a guaranteed full period and an exact
// bound, and it fits inside POLY_DIST_LEN.  465 words: odd is fine, only the byte count must be
// even.  (This is why it cannot be a per-stride table like poly4/poly5tone: it depends on BOTH
// phases, so the table would need 15*31 = 465 shapes of 930 bytes = 432 KB.)
#define POLY4G_LEN     930
#define POLY4G_SKIP      6   // transient before the state machine reaches its cycle
// Render modes for build_poly_dist, and the third component of the per-channel cache key.
#define POLY_DIST_P9_GATED   1u   // $00: poly9, poly5-gated
#define POLY_DIST_P9_UNGATED 2u   // $80: poly9, ungated
#define POLY_DIST_P4_GATED   3u   // $40: poly4, poly5-gated
static uint8_t        kBit9[POLY9_SIZE];              // poly9 bit stream (filled at init)
static __chip uint8_t poly_dist_buf[4][POLY_DIST_LEN];
static uint32_t       poly_dist_divider[4] = { 0, 0, 0, 0 }; // cache key: last AUDF divider built
static uint16_t       poly_dist_bd[4]      = { 0, 0, 0, 0 };  // cache key: last base_div (1/28/114)
static uint8_t        poly_dist_mode[4]    = { 0, 0, 0, 0 };  // cache key: last render mode (0=none)

// POKEY "noise" distortion (AUDC with PURE clear AND POLY4 clear: $80 ungated,
// $00 poly5-gated) is pseudo-random noise, which Paula cannot synthesise.  We DMA a
// long pre-rendered white-noise sample instead: it plays at the same pokey_period()
// as a tone would, and the buffer is long enough that the DMA loop period is a
// sub-audible rumble (~3.5 Hz at the worst-case 28.6 kB/s playback rate) rather than
// a pitched tone — the engine drone (event 8: AUDC $00, AUDF $FF) and explosion tails
// read as noise.  8 KB chip RAM.
//
// Generation: a 32-bit xorshift PRNG written one LONGWORD at a time (4 sample bytes
// per step), so the fill is ~4x cheaper than a per-bit poly17 LFSR — both fewer steps
// and `move.l` stores.  POKEY's own noise is 2-level (±vol square), but full-range
// uniform bytes are a valid white-noise source; the trade is a smoother hiss vs POKEY's
// harsher buzz.  noise_buf is 4-byte aligned for the longword writes (68000 even-addr).
#define NOISE_LEN 8192
static __chip __attribute__((aligned(4))) uint8_t noise_buf[NOISE_LEN];
static uint32_t       noise_rng = 0x13579BDFu;   // PERSISTENT xorshift state — carries across
                                                 // refills so the stream keeps evolving
static bool           noiseOn[4] = { false, false, false, false };

// Fill `words` longwords (4 bytes each) at byte offset `off` with fresh white noise.
//
// ⭐ The triple is chosen for the 68000, not copied from the textbook.  Marsaglia's 13/17/5
// costs 104 cycles of shift+eor per longword because `x << 13` has no short form: GCC must
// load the count into a data register and pay `lsl.l dN` = 8+2*13 = 34 cycles.  **1/5/16 is
// also a full-period (2^32-1) xorshift32 and costs 72** — `x << 1` is `add.l d0,d0` (8),
// `x >> 5` is `lsr.l #5` (18), and `x << 16` is `swap`+`clr.w` (8).  Verified full period the
// rigorous way, not by assertion: the map is linear over GF(2), so its 32x32 matrix M must
// satisfy M^(2^32-1) = I and M^((2^32-1)/p) != I for every prime p | 2^32-1 = 3*5*17*257*65537
// (tools/xorshift_triple_test.c — it also ranks every triple by 68000 cycle cost, which is
// how 1/5/16 was found; `cc -O2 -o /tmp/xs tools/xorshift_triple_test.c && /tmp/xs`).
//
// No hand asm: at -O2 GCC already emits the optimal 12-instruction body for this form
// (98 cycles/longword vs the old 136).  The only thing left on the table is a `dbf` back-edge
// instead of `cmp.l/jne` — 6 cycles per longword, ~0.06 t/firing at the current slice size,
// which is below every instrument in the tree.
static void fill_noise_words(int off, int words)
{
    uint32_t* p = (uint32_t*)(noise_buf + off);
    uint32_t  x = noise_rng;
    for (int i = 0; i < words; i++) {
        x ^= x << 1;
        x ^= x >> 5;
        x ^= x << 16;         // xorshift32 (1/5/16) — full period, cheapest trio on 68000
        p[i] = x;             // 4 sample bytes per step
    }
    noise_rng = x;
}
// Refill the whole buffer (init / channel-start prime).
static void fill_noise_buf(void) { fill_noise_words(0, NOISE_LEN / 4); }

// Called once per RENDERED FRAME from the main loop (PlatformAmiga::renderFrame): while any
// channel is in noise mode, refill a small slice round-robin through the buffer so the noise
// texture keeps evolving instead of statically repeating.  The buffer LENGTH (not the refill) is
// what keeps the DMA loop sub-audible, so this is cheap insurance, not a per-frame full
// regeneration (a full regen in the VBI ISR overruns the 20 ms vblank budget and drops the launch
// cinematic to 25 Hz).  Overwriting bytes Paula is mid-DMA on is inaudible for noise — random
// over random — which is what makes the main loop as safe a home as the ISR.
//
// ⚠ Do NOT move it back into the 50 Hz ISR.  It has no beam-timing requirement, and the ISR fires
// 50x/s no matter how slow the frame is, so there the SAME work costs ~2.3x per rendered frame in
// flight — measured at 16 longwords/VBI it was 5.71 t/firing = 8.5% of the whole flight VBI and
// 1.8% of ALL wall clock, for a texture nobody can hear.
//
// Sizing: the refill only has to keep up with the rate Paula READS the buffer.  The engine drone
// (the only steady noise voice in flight) runs AUDF ~$65 => Paula period ~5666 => ~626 B/s, one
// pass through the 8 KB buffer every ~13 s.  Pace the refill by WALL CLOCK (g_vbiCount, bumped in
// the real 50 Hz vblank ISR), NOT by how often the throttled main loop happens to call us: on a
// fast CPU / Fast-RAM machine the render rate soars well past 50 Hz, and refilling per rendered
// frame then churns the buffer several times faster than Paula reads it, so the steady noise turns
// harsh/distorted (reported on 25 MHz+ accelerators — the mother-ship launch engine).  ~2 longwords
// per VBI (~400 B/s) matches the A500 flight rate that sounds correct, stays below Paula's read
// rate so the write pointer never laps it, and is now identical on every CPU and every scene.

// g_vbiCount: bumped once per REAL vertical-blank interrupt (vbiHandler); read here to pace the
// noise refill, and below so flush_paula's beep capture can timestamp frames.
static volatile uint16_t g_vbiCount = 0;

void PlatformAmiga::noiseTick()
{
    if (!(noiseOn[0] || noiseOn[1] || noiseOn[2] || noiseOn[3])) return;

    static uint16_t lastVbi = 0;
    uint16_t elapsed = (uint16_t)(g_vbiCount - lastVbi);   // 16-bit wrap-safe
    if (elapsed == 0) return;                              // already refilled for this VBI
    lastVbi = g_vbiCount;
    if (elapsed > 8) elapsed = 8;                          // cap catch-up after a slow/stalled frame

    static int off = 0;
    for (uint16_t k = 0; k < elapsed; k++) {
        fill_noise_words(off, 2);       // 2 longwords (8 bytes) per VBI  ->  ~400 B/s (see above)
        off += 8;
        if (off >= NOISE_LEN) off = 0;
    }
}

// POKEY poly patterns (1 bit/entry) and AUDC distortion bits (atari800 pokeysnd.c/pokey.h)
static const uint8_t kBit4[15] = { 1,1,1,1,0,0,0,1,0,0,1,1,0,1,0 };
static const uint8_t kBit5[31] = { 1,1,1,1,0,1,1,0,1,0,0,1,1,0,0,0,0,0,1,1,1,0,0,1,0,0,0,1,0,1,0 };
#define POKEY_NOTPOLY5  0x80u
#define POKEY_POLY4     0x40u
#define POKEY_PURETONE  0x20u

// Build one poly waveform of `lenBytes` bytes into dst.  poly4: flip output per kBit4
// vs the current output; poly5tone: poly5-gated pure-tone flip.  Phase starts at p4=p5=0.
// Captures 2x the poly period (30 or 62 bytes) — a clean, even-length loop regardless of
// stride.  These two shapes depend on ONE poly phase each, so a per-stride table covers them;
// $40 (poly5-gated poly4) depends on both and is rendered per channel by build_poly_dist.
static void build_poly_one(uint8_t* dst, bool poly4, uint8_t s4, uint8_t s5, uint16_t lenBytes)
{
    uint8_t out = 0, p4 = 0, p5 = 0;
    for (uint16_t i = 0; i < lenBytes; i++) {
        p4 = (uint8_t)(p4 + s4); if (p4 >= 15u) p4 = (uint8_t)(p4 - 15u);  // p4,s4<15 → wrap = subtract
        p5 = (uint8_t)(p5 + s5); if (p5 >= 31u) p5 = (uint8_t)(p5 - 31u);  // p5,s5<31 → wrap = subtract
        bool toggle = poly4 ? (kBit4[p4] == (out ^ 1u))  // flip per poly4 vs current output
                            : (kBit5[p5] != 0);          // poly5-gated pure tone: flip when gate passes
        if (toggle) out ^= 1u;
        dst[i] = out ? 0x7Fu : 0x81u;
    }
}

// Precompute every distinct poly waveform — one per stride residue — into the static
// tables.  Called once from audioInit; the tables are immutable thereafter.
static void build_poly_tables(void)
{
    for (uint8_t s4 = 0; s4 < 15u; s4++) build_poly_one(poly4_wave[s4], true,  s4, 0, 30u);
    for (uint8_t s5 = 0; s5 < 31u; s5++) build_poly_one(poly5_wave[s5], false, 0, s5, 62u);

    // poly9 bit stream, generated by POKEY's 9-bit LFSR (atari800 pokey.c: reg starts $1FF,
    // reg = ((((reg>>5)^reg)&1)<<8) + (reg>>1); the audible bit is reg&1).
    uint16_t reg = 0x1FFu;
    for (int i = 0; i < POLY9_SIZE; i++) {
        reg = (uint16_t)((((((reg >> 5) ^ reg) & 1u) << 8)) + (reg >> 1));
        kBit9[i] = (uint8_t)(reg & 1u);
    }
}

// Render one channel's two-phase distortion waveform for the given per-underflow poly stride.
// Mirrors atari800 pokeysnd.c: at each underflow the polys advance by `stride` (the divide-by-N
// count in master-clock ticks); the poly5 gate decides whether the output may change at all
// (`if ((audc & NOTPOLY5) || bit5[P5])`), and inside the gate the 2-level output flips TOWARD
// the current poly9 or poly4 bit.  Output bytes are the same bipolar ±127 as wave_pure so
// AUDxVOL scales them identically.  Returns the length in bytes (mode-dependent).
// High-score save-block outcome counters (amiga/name_entry.gdb).  ⚠ Deliberately OUTSIDE the
// ROF_FLIGHT_PROBE guard below — src/rof_hiscore.c is shared, non-probe code and references them
// unconditionally on the Amiga, so guarding them fails the plain shipping link.  Six words.
extern "C" { volatile unsigned short g_hsSioRead  = 0; }     // sectors READ into mem[]
extern "C" { volatile unsigned short g_hsSioWrite = 0; }     // sectors WRITTEN back by the game
extern "C" { volatile unsigned short g_hsSioErr   = 0; }     // refused sector / command
extern "C" { volatile unsigned short g_hsFromFile = 0; }     // 1 = RoF.hi supplied the block
extern "C" { volatile unsigned short g_hsWritten  = 0; }     // RoF.hi writes that succeeded
extern "C" { volatile unsigned short g_hsDirty    = 0; }     // block waiting for the exit flush

#ifdef ROF_FLIGHT_PROBE
extern "C" { volatile unsigned long g_polyDistCalls = 0; }
#endif
static uint16_t build_poly_dist(uint8_t ch, uint32_t divider, uint16_t bd, uint8_t mode)
{
#ifdef ROF_FLIGHT_PROBE
    g_polyDistCalls++;
#endif
    // s5 = (divider*bd) % 31, s9 = (divider*bd) % 511, s4 = (divider*bd) % 15 — via
    // (a*b)%m = ((a%m)*(b%m))%m so every op is a 16-bit hardware DIVU.W/MULU.W (divider≤65536 →
    // quotient fits 16 bits), no 32-bit software mul/div even for the chain case.  bd<511 so
    // bd%511==bd.
    uint16_t s5  = rof_modu16(rof_mulu16(rof_modu16(divider, 31u), rof_modu16(bd, 31u)), 31u);
    uint16_t p5 = 0;
    uint8_t  out = 0;
    uint8_t* dst = poly_dist_buf[ch];

    if (mode == POLY_DIST_P4_GATED) {
        uint16_t s4 = rof_modu16(rof_mulu16(rof_modu16(divider, 15u), rof_modu16(bd, 15u)), 15u);
        uint16_t p4 = 0;
        // ⚠ SKIP THE TRANSIENT FIRST, or the buffer does not loop cleanly.  Inside the gate the
        // output is FORCED to the poly4 bit, so the state map (p4,p5,out) is not invertible and
        // (0,0,0) need not lie on its own cycle: 240 of the 465 strides then have dst[0] != the
        // sample that truly follows dst[929], i.e. a click at every DMA wrap (~7.6/s at the
        // footsteps' pitch).  The transient is at most 6 samples over all strides, and every
        // reachable cycle length (1,3,5,15,31,93,155,465) divides 930 — so skipping 6 and then
        // emitting 930 is always a whole number of cycles.  Proven exhaustively:
        // tools/poly_dist_test.c.
        for (int i = 0; i < POLY4G_SKIP + POLY4G_LEN; i++) {
            p4 = (uint16_t)(p4 + s4); if (p4 >= 15u) p4 = (uint16_t)(p4 - 15u);
            p5 = (uint16_t)(p5 + s5); if (p5 >= 31u) p5 = (uint16_t)(p5 - 31u);
            if (kBit5[p5]) {
                if (kBit4[p4] == (out ^ 1u)) out ^= 1u;   // output follows the poly4 bit
            }
            if (i >= POLY4G_SKIP) dst[i - POLY4G_SKIP] = out ? 0x7Fu : 0x81u;
        }
        return POLY4G_LEN;
    }

    uint16_t s9 = rof_modu16(rof_mulu16(rof_modu16(divider, POLY9_SIZE), bd), POLY9_SIZE);
    uint16_t p9 = 0;
    const bool gateAlways = (mode == POLY_DIST_P9_UNGATED);
    /* p5,s5 < 31 so p5+s5 < 62 → the phase wrap is a compare-subtract, NOT a modulo; same
     * for p9,s9 < 511.  This kills 2 DIVU/byte (~2044 divides over the 1022-byte buffer,
     * ~40ms) that fired on every poly9-voice stride change (SFX freq sweeps / explosions) —
     * byte-identical output. */
    for (int i = 0; i < POLY_DIST_LEN; i++) {
        p5 = (uint16_t)(p5 + s5); if (p5 >= 31u)  p5 = (uint16_t)(p5 - 31u);
        p9 = (uint16_t)(p9 + s9); if (p9 >= POLY9_SIZE) p9 = (uint16_t)(p9 - POLY9_SIZE);
        if (gateAlways || kBit5[p5]) {
            if (kBit9[p9] == (out ^ 1u)) out ^= 1u;   // output follows the poly9 bit
        }
        dst[i] = out ? 0x7Fu : 0x81u;
    }
    return POLY_DIST_LEN;
}

// Shadow of POKEY registers $D200..$D20F (bus_write doesn't update mem[] for
// hardware-range writes, so we maintain our own copy here)
static uint8_t pokey[16];   // [0]=AUDF1 [1]=AUDC1 ... [8]=AUDCTL ...

// POKEY LFSR (17-bit, polynomial x^17+x^5+1; matches Platform::pokeyRandomStep).
// Exposed (extern "C") so a RANDOM-heavy native loop (fill_terrain_columns) can hold the
// state in a register for its whole run and step the LFSR inline, instead of paying a
// cross-TU rof_pokey_random() call per read.  It's our own RNG (not POKEY-cycle-accurate),
// so an ISR RANDOM read interleaving with such a loop only reshuffles cosmetic output.
extern "C" { uint32_t rof_lfsr_state = 0x1FFFFu; }

static uint8_t pokey_random_step(void)
{
    uint32_t bit = ((rof_lfsr_state >> 16) ^ (rof_lfsr_state >> 4)) & 1u;
    rof_lfsr_state = ((rof_lfsr_state << 1) | bit) & 0x1FFFFu;
    return (uint8_t)rof_lfsr_state;
}

// Direct POKEY RANDOM accessor for bus_read($D20A) — bypasses the platform_hw_read C bridge
// + virtual hwRead dispatch (see bus.h).  Same lfsr_state hwRead($D20A) steps, so the RANDOM
// sequence is identical to the generic path.
extern "C" uint8_t rof_pokey_random(void) { return pokey_random_step(); }

// ---- Paula register helpers --------------------------------------------------
// Layout: AUD0=$DFF0A0, AUD1=$DFF0B0, AUD2=$DFF0C0, AUD3=$DFF0D0
// Within each: +0 PTR(32), +4 LEN(16), +6 PER(16), +8 VOL(16)
static const uint32_t kAudioBase[4] = {
    0xDFF0A0u, 0xDFF0B0u, 0xDFF0C0u, 0xDFF0D0u
};
// Paula's shortest legal period (~28.6 kHz playback); pokey_period_compute clamps to it too.
// Also the period flush_paula loads before a DMA-restart off-window, because "2 sample periods"
// is measured in THIS period — see the restart block.
#define kPaulaMinPer 124u
#define AUD_PER(ch) (*(volatile uint16_t*)(kAudioBase[ch] + 6))
#define AUD_VOL(ch) (*(volatile uint16_t*)(kAudioBase[ch] + 8))
#define AUD_PTR(ch) (*(volatile uint32_t*)(kAudioBase[ch] + 0))
#define AUD_LEN(ch) (*(volatile uint16_t*)(kAudioBase[ch] + 4))

// Changing a channel's WAVEFORM (PTR/LEN) — a real note/instrument change — requires a DMA
// restart, because writing AUD_PER takes effect immediately but the new AUD_PTR is only
// latched by Paula at the next loop wrap.  Without a restart the OLD waveform plays at the
// NEW period for up to a full loop (~100 ms on a big bass pitch drop) — the audible
// "squelch" between notes.  A restart must hold the channel OFF for >2 sample periods or
// the hardware "stays on and continues from where it left off" (HW manual §5-2-7); FS-UAE
// models this, so an immediate off→on is a no-op.  The fix is the classic ProTracker dance:
// DMA off → wait ~7 rasterlines → set registers → DMA on.
//
// Per-channel updates are DEFERRED into a "want" table during the music tick and applied
// once by flush_paula() at the tick's end, so the ~7-line wait is paid ONCE for ALL the
// channels that changed waveform (not once per channel).  Same-waveform updates (the
// melody's per-tick volume envelope) take the no-wait live-write path and never restart,
// so they don't click.
static uint32_t cur_ptr[4] = { 0, 0, 0, 0 };  // waveform Paula is currently pointed at
static uint16_t cur_len[4] = { 0, 0, 0, 0 };
static uint16_t cur_per[4] = { 0, 0, 0, 0 };  // period currently loaded (= the one in effect
                                              // during the off window → sizes the reset wait)
static uint32_t want_ptr[4];
static uint16_t want_len[4], want_per[4];
static uint8_t  want_vol[4];
static uint8_t  want_valid = 0;               // bitmask of channels written this tick

#ifdef ROF_FLIGHT_PROBE
// Sizing probe for the "defer the Paula recompute to flush_paula" candidate.  Paula is only
// PROGRAMMED once per frame (flush_paula), so every update_paula_channel except the last one per
// channel per frame is pure waste — but how much waste is an empirical question (the recipe's
// estimate spanned 0.4%-3.2% of wall clock).  s_upcSeen = channels already recomputed since the
// last flush; g_upcRedund counts the recomputes a per-channel dirty bit would have collapsed,
// g_upcDistinct the ones it would still have to do.  Declared here (not with the other g_p*
// probes further down) because flush_paula, which resets the mask, is defined above them.
extern "C" { volatile unsigned long g_upcRedund = 0, g_upcDistinct = 0, g_upcFlushes = 0; }
static uint8_t s_upcSeen = 0;

// Sizing the restart busy-wait — the WHOLE audio bracket is that wait (§22.3).  Two facts settle
// what to do about it, and only one of them is visible from the C:
//   g_fpRestartFlushes / g_fpWaitSum / g_fpWaitMax — how often a restart happens and how long the
//     wait it asks for is.  wl/flush is what the fix moves; a constant 7 means the period floor
//     reached every restart.
//   g_fpLenHist[] — the DMA loop LENGTH, in words, of the waveform each channel is switching AWAY
//     from, bucketed by waveform: [0] wave_pure (1) [1] poly4 (15) [2] poly5 (31) [3] poly_dist
//     (511) [4] noise_buf (4096).  This says whether the restart is needed at all: its only job is
//     to make Paula latch PTR/LEN before the current loop wraps by itself, and a 15-word loop wraps
//     in len*per ticks — so a table full of [1]/[2] means the restarts are all on short loops.
extern "C" { volatile unsigned long g_fpRestartFlushes = 0, g_fpWaitSum = 0, g_fpWaitMax = 0; }
extern "C" { volatile unsigned long g_fpChRestarts = 0, g_fpLenHist[5] = { 0, 0, 0, 0, 0 }; }
extern "C" { volatile unsigned long g_fpPerSum = 0; }      // sum of the OLD periods (what sized the wait)
#endif

#ifdef ROF_AUDIO_TRACE
// Per-RESTART log: every flush_paula DMA off->on, with the OUTGOING waveform's loop length and
// period — the two numbers that decide whether the off-window was long enough for Paula to reach
// idle and latch the new AUDxLC/LEN (HW manual §5-2-7: the channel must miss a sample boundary,
// and the countdown still in flight runs at the OLD period).
extern "C" {
    volatile unsigned short g_arVbi[600];
    volatile unsigned short g_arPer[600];   // cur_per[ch] BEFORE the kPaulaMinPer floor
    volatile unsigned short g_arLen[600];   // cur_len[ch] in words (1 / 15 / 31 / 511 / 4096)
    volatile unsigned char  g_arCh[600];
    volatile unsigned char  g_arWl[600];    // rasterlines actually waited
    volatile unsigned short g_arIdx = 0;
}
static void aud_restart_log(uint8_t ch, uint16_t oldPer, uint16_t oldLen, uint8_t wl)
{
    unsigned short i = g_arIdx;
    if (i >= 600u) return;
    g_arVbi[i] = g_vbiCount; g_arPer[i] = oldPer; g_arLen[i] = oldLen;
    g_arCh[i] = ch; g_arWl[i] = wl;
    g_arIdx = (unsigned short)(i + 1u);
}

// Per-FRAME snapshot of the LIVE Paula state of all four channels.  This is the exact input an
// offline DAC model needs to reproduce what the hardware emits: period, volume and loop length
// (= waveform identity: 1 pure / 15 poly4 / 31 poly5 / 465-511 poly_dist / 4096 noise) per frame.
// AUDxVOL is write-only, so the volume is shadowed at the two sites that write it.
#define AUD_TRACE_FRAMES 600u
extern "C" {
    volatile unsigned short g_asVbi[AUD_TRACE_FRAMES];
    volatile unsigned short g_asPer[AUD_TRACE_FRAMES][4];
    volatile unsigned short g_asLen[AUD_TRACE_FRAMES][4];
    volatile unsigned char  g_asVol[AUD_TRACE_FRAMES][4];
    volatile unsigned short g_asIdx = 0;
}
static uint8_t trace_vol[4] = { 0, 0, 0, 0 };
static void aud_state_snapshot(void)
{
    unsigned short i = g_asIdx;
    if (i >= AUD_TRACE_FRAMES) return;
    g_asVbi[i] = g_vbiCount;
    for (uint8_t ch = 0; ch < 4; ch++) {
        g_asPer[i][ch] = cur_per[ch];
        g_asLen[i][ch] = cur_len[ch];
        g_asVol[i][ch] = trace_vol[ch];
    }
    g_asIdx = (unsigned short)(i + 1u);
}
#endif

// Record a channel's desired Paula state; applied by flush_paula().
static void want_set(uint8_t ch, uint32_t ptr, uint16_t len, uint16_t per, uint8_t vol)
{
    want_ptr[ch] = ptr; want_len[ch] = len; want_per[ch] = per; want_vol[ch] = vol;
    want_valid |= (uint8_t)(1u << ch);
}

// Busy-wait `lines` rasterline transitions by watching the VHPOSR ($DFF006) vertical beam
// byte (V7..V0).  PAL has 312 lines, so that byte wraps 255→0; counting *transitions*
// (now != prev) handles the wrap with no special case — the bug that hung the old
// wait-on-exact-value loop.  A hard iteration cap guarantees we can never spin forever
// even if the beam were somehow stuck.
static void wait_rasterlines(uint8_t lines)
{
    uint8_t  prev  = (uint8_t)(*vhposrPointer >> 8);
    uint32_t guard = 0;
    while (lines) {
        uint8_t now = (uint8_t)(*vhposrPointer >> 8);
        if (now != prev) { prev = now; lines--; }
        if (++guard > 200000u) break;   // ~hundreds of lines worth — escape hatch, never hang
    }
}

#ifdef ROF_FPSCOUNT
// Painted terrain frames, bumped once in renderFlightDirect (`make FPSCOUNT=1`).  The whole
// point of this build is that it carries NO other instrumentation, so the framerate it
// reports is the shipping build's.  FPS = 50 * g_fpsFrames / g_vbiCount.
extern "C" { volatile unsigned long g_fpsFrames = 0; }
#endif


// Apply all channels recorded since the last flush.  Waveform changes are batched through a
// single DMA off → wait → on so the rasterline wait is paid once.  Called once per frame from
// game_vbi_isr, after both audio engines have recorded their POKEY writes for the frame: the
// VBI SFX-theme tick (sfx_voice_tick) and the in-game SFX engine (sfx_voice_envelope_tick).
extern "C" void flush_paula(void)
{
    uint8_t valid = want_valid;
    want_valid = 0;
    uint8_t restart = 0;
#ifdef ROF_FLIGHT_PROBE
    s_upcSeen = 0; g_upcFlushes++;   // sizing probe: a new frame's worth of recomputes starts here
#endif

    if (valid) {
        // Split into "restart" (waveform changed) and "live" (same waveform → just VOL/PER).
        for (uint8_t ch = 0; ch < 4; ch++) {
            if (!(valid & (1u << ch))) continue;
            if (want_ptr[ch] != cur_ptr[ch] || want_len[ch] != cur_len[ch]) {
                restart |= (uint8_t)(1u << ch);
#ifdef ROF_FLIGHT_PROBE
                // Sized here, where cur_len/cur_per are still the OLD (outgoing) waveform's.
                g_fpChRestarts++;
                g_fpPerSum += cur_per[ch];
                g_fpLenHist[cur_len[ch] >= 4096u ? 4 : cur_len[ch] >= 511u ? 3
                          : cur_len[ch] >= 31u   ? 2 : cur_len[ch] >= 15u  ? 1 : 0]++;
#endif
            }
            else { AUD_PER(ch) = want_per[ch]; AUD_VOL(ch) = want_vol[ch];     // live, no click
                   cur_per[ch] = want_per[ch];
#ifdef ROF_AUDIO_TRACE
                   trace_vol[ch] = want_vol[ch];
#endif
            }
        }

        if (restart) {
            // The channel must stay OFF for >2 sample periods of the period STILL LOADED (the old
            // note's) or it "stays on and continues" (§5-2-7).  A PAL rasterline is ~227 Paula
            // ticks, so size the wait from the slowest old period among the restarting channels:
            // 2*per/227 lines, plus margin.  Fast old notes wait the ~7-line floor; a slow old bass
            // note (per~6011) needs ~53 lines (~3.4 ms) — fine on the title screen.
            //
            // ⭐ …except THE PERIOD STILL LOADED IS OURS TO CHOOSE, and that is the whole game in
            // flight (docs/flight-perf-log.md §22/§25).  A laser/explosion sweeps AUDF every 50 Hz
            // firing, and the poly4/poly5 waveform POINTER is a function of the stride residue
            // (update_paula_channel: poly4_wave[stride%15]), so a sweep changes the waveform —
            // hence restarts — on EVERY frame for the length of the sound.  Sizing the wait from
            // the old note's period makes that the single most expensive thing in the flight VBI:
            // 37.06 t/firing, 32% of the whole ISR, for the ~2-3 s after a shot.
            //
            // ⚠ …but flooring the period is NOT sufficient on its own.  A write to AUDxPER
            // changes only the RELOAD value: the countdown ALREADY IN FLIGHT keeps its old
            // duration, and the channel reaches idle only after that countdown expires plus one
            // more sample at the new period.  So the honest window is `outgoing_per +
            // kPaulaMinPer` ticks, which the 7-line floor (1589) covers only while the outgoing
            // period is under ~1465 — i.e. above ~1200 Hz.  Every drone, bass and explosion tail
            // is below that.
            //
            // Under-waiting is not silent failure: Paula still latches the new AUDxLC/LEN at the
            // OUTGOING loop's own wrap, cur_len samples later.  That length decides whether it is
            // audible, and therefore which channels have to be waited for:
            //   • short loop (wave_pure 1, poly4 15, poly5 31 words) — the wrap is ≤31 samples
            //     away: a fraction of a frame of stale timbre on a sound that is sweeping anyway.
            //     This is the flight sweep path, and it keeps the floor.
            //   • long loop (poly_dist 465, noise_buf 4096) — the wrap is a tenth of a second to
            //     over a second away, and for all of it the channel plays the OLD waveform at the
            //     NEW volume.  A note onset arriving as the sound before it is exactly this: out
            //     of boot every channel sits on wave_pure (2 samples) and nothing is audible,
            //     but after a flight they are parked on those long loops.
            // So size the wait from the outgoing period only for the long-loop channels.
            // Derivation, the proof and the measured cost: docs/sfx-events.md §The Paula
            // DMA-restart wait.
            uint16_t max_per = 0;
#ifdef ROF_AUDIO_TRACE
            uint16_t logPer[4] = { 0, 0, 0, 0 }, logLen[4] = { 0, 0, 0, 0 };
            for (uint8_t ch = 0; ch < 4; ch++)
                if (restart & (1u << ch)) { logPer[ch] = cur_per[ch]; logLen[ch] = cur_len[ch]; }
#endif
            for (uint8_t ch = 0; ch < 4; ch++) {
                if (!(restart & (1u << ch))) continue;
#ifdef ROF_FLUSHWAIT_OLD
                if (cur_per[ch] > max_per) max_per = cur_per[ch];   // A/B control: size off the old note
#else
                // Only a long outgoing loop needs the honest window (see above); a short one
                // wraps by itself before the stale timbre is audible.
                if (cur_len[ch] > 31u && cur_per[ch] > max_per) max_per = cur_per[ch];
                AUD_PER(ch) = kPaulaMinPer;    // …so the post-reset sample period is ~0.5 lines
                cur_per[ch] = kPaulaMinPer;    // keep the shadow honest for the rest of this flush
#endif
            }
            // Ticks waited = 227*wl, and the requirement is max_per + kPaulaMinPer, so wl =
            // max_per/227 + 3 clears it with ~2.5 lines of margin.  max_per is 0 unless a
            // long-loop channel is restarting, so the floor is what almost every flush gets.
            // The 160-line ceiling bounds the ISR damage (3.2 ms); it is honest up to per 35000,
            // i.e. every period the game actually produces (the drone's worst is ~14300).
            uint16_t mp = max_per > 32000u ? 32000u : max_per;
            uint16_t wl = rof_divu16(mp, 227u) + 3u;
            if (wl < 7u)  wl = 7u;
            if (wl > 160u) wl = 160u;
#ifdef ROF_AUDIO_TRACE
            for (uint8_t ch = 0; ch < 4; ch++)
                if (restart & (1u << ch)) aud_restart_log(ch, logPer[ch], logLen[ch], (uint8_t)wl);
#endif
#ifdef ROF_FLIGHT_PROBE
            g_fpRestartFlushes++;
            g_fpWaitSum += wl;
            if ((unsigned long)wl > g_fpWaitMax) g_fpWaitMax = wl;
#endif

            *dmaconPointer = (uint16_t)restart;            // AUDxEN off for all changed channels
            wait_rasterlines((uint8_t)wl);                 // hold off past the in-flight countdown
            for (uint8_t ch = 0; ch < 4; ch++) {
                if (!(restart & (1u << ch))) continue;
                AUD_PTR(ch) = want_ptr[ch];
                AUD_LEN(ch) = want_len[ch];
                AUD_PER(ch) = want_per[ch];
                AUD_VOL(ch) = want_vol[ch];
                cur_ptr[ch] = want_ptr[ch];
                cur_len[ch] = want_len[ch];
                cur_per[ch] = want_per[ch];
#ifdef ROF_AUDIO_TRACE
                trace_vol[ch] = want_vol[ch];
#endif
            }
            *dmaconPointer = (uint16_t)(0x8000u | restart); // AUDxEN on — all at once, one wait paid
        }
    }

}

// ---- POKEY→Paula frequency conversion ----------------------------------------
// Pure POKEY-AUDF → Paula-period math.  Uses 32-bit divides (POKEY/PAULA clocks are >16 bits →
// GCC emits the slow __udivsi3 software routine, the 68000 has no 32-bit divide).  This is called
// only to PRE-BUILD the tables below at init, NOT per-frame.
//   POKEY's output flip-flop toggles once per counter underflow → the square wave is HALF the
//   counted-clock rate: f = clock/(2*divider).  Omitting the ÷2 makes every voice an octave high.
static uint16_t pokey_period_compute(uint32_t divider, bool use_179, uint32_t base_div)
{
    // freq = clock / (2*base_div*divider); via the exact integer identity ⌊n/(a·b)⌋ = ⌊⌊n/a⌋/b⌋
    // pre-fold the constant a = 2·base_div so the runtime divide is 16-bit (DIVU.W), no __udivsi3.
    //   num = ⌊1789773/(2·base_div)⌋ : base_div 28 → 31960, 114 → 7849 ; use_179 (÷2 only) → 894886.
    if (divider == 0u || divider > 0xFFFFu) return 0u;     // chain wrap → freq≈0 → silence
    uint32_t num = use_179 ? 894886u : (base_div == 114u ? 7849u : 31960u);
    if (use_179 && divider < 32u) return 0u;               // freq > 28000 → silence (keeps quotient ≤16b)
    uint16_t freq = rof_divu16(num, (uint16_t)divider);    // num≤894886 (32b), quotient ≤16b here
    if (freq < 20u || freq > 28000u) return 0u;            // out of range → silence
    if (freq < 28u) return 0xFFFFu;                        // per = 3546895/(2·freq) > 65535 → saturate
    uint16_t per = rof_divu16(3546895u, (uint16_t)(2u * freq));   // 2·freq ≤ 56000; quotient ≤ 63337
    return per < 124u ? 124u : per;                        // Paula minimum period
}

// Precomputed period tables for the common single-byte-AUDF (non-chain) case — the period is a
// pure function of (audf, clock config), so it's a lookup, not a per-frame divide.  3 configs:
//   [0] use_179 (1.79 MHz direct)   [1] base_div 28 (÷64k default)   [2] base_div 114 (15 kHz).
// Filled once at audioInit; this removes the ~18 32-bit software divide/mul calls that
// update_paula_channel→pokey_period otherwise ran on every AUDF/AUDC change (the 50 Hz VBI path).
static uint16_t s_perTable[3][256];
static void build_period_table(void)
{
    for (int a = 0; a < 256; a++) {
        // Divided-clock channels count AUDF+1; a channel clocked straight off 1.79 MHz counts
        // AUDF+4 (the extra pipeline delay POKEY only exposes at full rate — Atari HW ref,
        // atari800 pokeysnd.c Update_pokey_sound_rf).
        s_perTable[0][a] = pokey_period_compute((uint32_t)a + 4u, true,  28u);  // base_div unused
        s_perTable[1][a] = pokey_period_compute((uint32_t)a + 1u, false, 28u);
        s_perTable[2][a] = pokey_period_compute((uint32_t)a + 1u, false, 114u);
    }
}

// AUDCTL bit→channel (atari800 pokey.h).  The four audio channels pair up as (ch0,ch1) and
// (ch2,ch3); each pair's clock select and 16-bit-join bit belong to the pair, not the channel:
//   CH1_179 $40 = 1.79 MHz for the ch0+ch1 pair    CH3_179 $20 = 1.79 MHz for the ch2+ch3 pair
//   CH1_CH2 $10 = join ch0(lo)+ch1(hi)             CH3_CH4 $08 = join ch2(lo)+ch3(hi)
#define POKEY_PAIR_179(ch)    ((ch) & 2u ? 0x20u : 0x40u)
#define POKEY_PAIR_CHAIN(ch)  ((ch) & 2u ? 0x08u : 0x10u)

// Resolve one channel's frequency divider, honouring the 16-bit chain.  Returns the counter
// reload value in base-clock counts and, via the out-params, the master-clock ticks per base
// count (`*out_bd`, 1 when the channel runs straight off 1.79 MHz) and that same 1.79 flag.
// The single source of truth for chaining: period, poly-distortion stride and poly4/poly5
// stride all go through it, so they cannot disagree.  `out_179` may be NULL.
//
// ⚠ The joined 16-bit counter lives on the HIGH channel of the pair (ch1 / ch3), NOT the low
// one — AUDF_lo is only the low byte of its reload value (atari800 mzpokeysnd.c
// Update_c1divstart / Update_c3divstart).  The LOW channel keeps sounding, but its counter no
// longer reloads from AUDF: it free-runs the full 256-count byte wrap, an unpitched buzz
// (Update_c0divstart: c0divstart = 256 * mdivk).  Software conventionally sets the low half's
// AUDC volume to 0.  (Real POKEY reloads the low half with AUDF_lo on the one cycle the pair
// itself reloads; that once-per-16-bit-period jitter is below what a wavetable can carry.)
static uint32_t pokey_divider(uint8_t ch, uint8_t audf, uint8_t audctl,
                              uint16_t* out_bd, bool* out_179)
{
    const uint8_t lo_ch  = (uint8_t)(ch & 2u);            // 0 for the ch0+ch1 pair, 2 for ch2+ch3
    const bool    hi_half = (ch & 1u) != 0u;
    const bool    chained = (audctl & POKEY_PAIR_CHAIN(ch)) != 0u;
    bool     use_179;
    uint32_t divider;

    if (chained) {
        use_179 = (audctl & POKEY_PAIR_179(ch)) != 0u;    // the pair's clock is the low half's
        // 16-bit reload = AUDF_lo + 256*AUDF_hi, +7 at 1.79 MHz / +1 on a divided clock.
        divider = hi_half ? ((uint32_t)pokey[lo_ch * 2] + 256u * (uint32_t)audf + (use_179 ? 7u : 1u))
                          : 256u;                          // low half: free-running byte wrap
    } else {
        use_179 = !hi_half && (audctl & POKEY_PAIR_179(ch)) != 0u;   // only ch0/ch2 take 1.79 MHz
        divider = (uint32_t)audf + (use_179 ? 4u : 1u);
    }
    if (out_179) *out_179 = use_179;
    *out_bd  = use_179 ? 1u : ((audctl & 0x01u) ? 114u : 28u);
    return divider;
}

// (divider * base_div) % m — the poly stride residue — without overflowing MULU.W.  Unchained
// the product is at most 259*114 = 29526 and this is the one MULU.W + one DIVU.W it always was;
// only a 16-bit chain (divider up to 65543) needs the reduce-each-factor-first identity
// (a*b)%m = ((a%m)*(b%m))%m, which costs two more DIVU.W on that rare path.
static inline uint16_t poly_stride_mod(uint32_t divider, uint16_t bd, uint16_t m)
{
    if (divider <= 574u)                        // 574*114 = 65436, still 16-bit
        return rof_modu16(rof_mulu16((uint16_t)divider, bd), m);
    return rof_modu16(rof_mulu16(rof_modu16(divider, m), rof_modu16(bd, m)), m);
}

static uint16_t pokey_period(uint8_t ch, uint8_t audf, uint8_t audctl)
{
    if (!(audctl & POKEY_PAIR_CHAIN(ch))) {   // common case: the period is a pure function of AUDF
        bool use_179 = !(ch & 1u) && (audctl & POKEY_PAIR_179(ch));
        return s_perTable[use_179 ? 0 : ((audctl & 0x01u) ? 2 : 1)][audf];
    }
    uint16_t bd; bool use_179;                // rare 16-bit chain — compute directly (not tabled)
    uint32_t divider = pokey_divider(ch, audf, audctl, &bd, &use_179);
    return pokey_period_compute(divider, use_179, bd);
}

// Point channel `ch` at its per-channel two-phase distortion waveform, rebuilding it only when
// the stride or the mode changes (volume changes don't reshape it).  Cache key = (divider,
// base_div, mode) rather than the stride product, so no 32-bit multiply is needed even for a
// 16-bit chain — build_poly_dist reduces the factors itself.
static void want_poly_dist(uint8_t ch, uint32_t divider, uint16_t bd, uint8_t mode,
                           uint16_t per, uint8_t vol)
{
    static uint16_t poly_dist_len[4] = { 0, 0, 0, 0 };
    if (poly_dist_divider[ch] != divider || poly_dist_bd[ch] != bd || poly_dist_mode[ch] != mode) {
        poly_dist_len[ch] = build_poly_dist(ch, divider, bd, mode);
        poly_dist_divider[ch] = divider; poly_dist_bd[ch] = bd; poly_dist_mode[ch] = mode;
    }
    noiseOn[ch] = false;   // a deterministic loop, not the evolving noise_buf
    want_set(ch, (uint32_t)poly_dist_buf[ch], (uint16_t)(poly_dist_len[ch] / 2), per, vol);
}

static void update_paula_channel(uint8_t ch)
{
    uint8_t audf   = pokey[ch * 2];
    uint8_t audc   = pokey[ch * 2 + 1];
    uint8_t audctl = pokey[8];

    uint8_t vol;
    if (audc & 0x10u) {
        vol = 0;  // VOL_ONLY = DC = treat as silence
    } else {
        // POKEY 0..15 → Paula 0..64, and the top of the range MUST land exactly on 64.
        // Below 64 Paula abandons the plain sample-and-hold and approximates the level with a
        // PWM-like cycle on a FIXED 64-tick raster (3546895/64 = 55420 Hz), which re-quantises
        // every waveform edge to that grid.  A channel period that is not a multiple of 64 then
        // gets its edges snapped alternately, and the periodic edge jitter lands as sidebands
        // INSIDE the audio band — audible on a pure tone, and impossible to filter out because
        // they are in the passband, not ultrasonic images.  Volume 64 disables the raster.
        // (Henryk Richter, "Amiga Paula vs. System Theory" §2.4; docs/sfx-events.md.)
        // A table, not v*64/15: no divide, and the top entry is exact.
        static const uint8_t kPokeyToPaulaVol[16] = {
             0,  4,  9, 13, 17, 21, 26, 30,
            34, 38, 43, 47, 51, 55, 60, 64,
        };
        vol = kPokeyToPaulaVol[audc & 0x0Fu];
    }

    uint16_t per = pokey_period(ch, audf, audctl);

    // A silent channel's waveform is inaudible, so never re-point PTR/LEN while
    // vol==0.  This matters because POKEY's reset/idle AUDC value is $00, which
    // decodes as the NOISE distortion (PURE clear + POLY4 clear).  The SFX/music
    // engine writes a voice's AUDF (frequency) BEFORE its AUDC (distortion|volume)
    // — e.g. sfx_seq_step writes $D200/$D202/$D204, then sfx_voice_tick writes
    // $D201/$D203/$D205 — so on the first note each AUDF write recomputes the
    // channel against the stale, zeroed AUDC and would point it at the long
    // noise_buf.  When the real AUDC (pure, with volume) then arrives, AUD_VOL
    // rises immediately but Paula only latches the new PTR/LEN at the next DMA
    // loop wrap (~280 ms for the 8 KB noise_buf), so the note's onset plays as
    // noise instead of a square — intermittently, depending on the DMA phase.
    // Leaving PTR/LEN alone while silent keeps the channel on wave_pure until a
    // single AUDC-with-volume write sets distortion + period + volume together.
    if (vol == 0u) {
        noiseOn[ch] = false;
        // Silence with NO waveform change (want_ptr = current) so flush just drops VOL to 0
        // and never restarts.  Routing through want_set (rather than a live AUD_VOL write)
        // ensures this overrides any earlier want recorded for this channel this tick.
        want_set(ch, cur_ptr[ch], cur_len[ch], per ? per : 124u, 0u);
        return;
    }

    // POKEY noise distortion (PURE clear AND POLY4 clear → poly9/poly17 source: $00 / $80).
    bool is_noise = !(audc & POKEY_PURETONE) && !(audc & POKEY_POLY4) && !(audc & 0x10u);
    if (is_noise) {
        if (per == 0u) per = 124u;        // out-of-range → fastest Paula rate
        // poly9 source (AUDCTL bit7 set): render the real poly5-gated poly9 ±square — a
        // pitched, punchy buzz — instead of the flat white noise.  Regenerate only when the
        // poly stride or gate mode changes (volume changes don't alter the shape).
        if (audctl & 0x80u) {
            uint16_t bd;
            uint32_t divider = pokey_divider(ch, audf, audctl, &bd, nullptr);
            want_poly_dist(ch, divider, bd,
                           (audc & POKEY_NOTPOLY5) ? POLY_DIST_P9_UNGATED : POLY_DIST_P9_GATED,
                           per, vol);
            return;
        }
        // poly17 source: no short clean loop — fall back to the long white-noise sample.
        // Not pitched, so an out-of-range frequency must hiss (per already clamped above),
        // not silence — this carries the engine drone / explosion tails.
        noiseOn[ch] = (vol != 0u);
        want_set(ch, (uint32_t)noise_buf, (uint16_t)(NOISE_LEN / 2), per, vol);
        return;
    }

    noiseOn[ch] = false;      // pitched (non-noise) voice → no noise_buf refills
    if (per == 0u) vol = 0u;  // out-of-range frequency → silence

    // Point the channel at the precomputed waveform for its distortion mode + stride
    // residue (shapes built once at init; Paula latches PTR/LEN at the next DMA loop
    // wrap, so the change takes effect promptly).
    // POKEY's poly5 gate wraps ALL THREE distortion families, not just the noise one
    // (atari800 pokeysnd.c: `if ((audc & NOTPOLY5) || bit5[P5])` encloses the pure/poly4/poly9
    // cases alike).  So $C0 is ungated poly4 but $40 is poly4 GATED BY POLY5 — a different,
    // rougher waveform that depends on both poly phases and therefore cannot come from the
    // per-stride table.  Rendering $40 as ungated poly4 turned the airlock footsteps
    // (SFX event $1A, AUDC $46) into a clean square bleep: at their AUDF $08 the ungated poly4
    // orbit is only 5 samples long, i.e. a pure tone, where the gated one runs the full 930.
    bool poly5tone = (audc & POKEY_PURETONE) && !(audc & POKEY_NOTPOLY5);   // $20, $60
    bool poly4     = !(audc & POKEY_PURETONE) && (audc & POKEY_POLY4);      // $C0, $40
    if (!poly5tone && !poly4) {
        want_set(ch, (uint32_t)wave_pure, 1u, per ? per : 124u, vol);       // $A0/$E0 pure tone
        return;
    }
    // The shape depends only on the poly stride = divider*base_div (master-clock ticks per
    // underflow) taken mod 15 / mod 31 — so it must see the chain and the 1.79 MHz clock,
    // which is why the divider comes from the same resolver as the period.
    uint16_t bd;
    uint32_t divider = pokey_divider(ch, audf, audctl, &bd, nullptr);
    if (poly4 && !(audc & POKEY_NOTPOLY5)) {                                // $40
        want_poly_dist(ch, divider, bd, POLY_DIST_P4_GATED, per ? per : 124u, vol);
        return;
    }
    uint32_t sel_ptr; uint16_t sel_len;
    if (poly4) {
        sel_ptr = (uint32_t)poly4_wave[poly_stride_mod(divider, bd, 15u)]; sel_len = 15u;  // 30 B
    } else {
        sel_ptr = (uint32_t)poly5_wave[poly_stride_mod(divider, bd, 31u)]; sel_len = 31u;  // 62 B
    }
    want_set(ch, sel_ptr, sel_len, per ? per : 124u, vol);
}

// ---- public interface --------------------------------------------------------
// mem[] has already been populated (load_xex_image, called from main before the scene
// initialises); this only sets up the Paula side.
// CIA-A PRA bit 1 (Paula low-pass filter / power LED) as we found it -- audioInit turns the
// filter off, audioShutdown puts this back.  Machine state, same class as DMACON/INTENA.
static uint8_t s_savedCiaaPra = 0x02u;

void PlatformAmiga::audioInit()
{
    // Clear POKEY shadow and LFSR
    for (int i = 0; i < 16; i++) pokey[i] = 0;
    rof_lfsr_state = 0x1FFFFu;
    // The pure-tone square every channel defaults to.  Filled here rather than statically:
    // `.MEMF_CHIP` is a chip BSS hunk, so a static initialiser would be dropped (see the
    // declaration of wave_pure).  Must precede the AUD_PTR(ch) = wave_pure loop below.
    wave_pure[0] = 0x7Fu; wave_pure[1] = 0x81u;
    fill_noise_buf();   // pre-render the poly17 noise sample for noise-distortion voices
    build_poly_tables(); // pre-render every distinct poly distortion waveform (immutable)
    build_period_table(); // pre-render AUDF->Paula-period (kills the per-frame 32-bit soft divide)

    // SFX is initialised by the mem[$0090] gate in RescueOnFractalus::update():
    // the snapshot has $0090=1, so the first update() call resets $073C/$073A
    // to start the sequence from note 0 — no replay needed here.

    // Take the four channels down BEFORE programming them (the takeover in run() deliberately
    // leaves exec's audio DMA alone), so whatever the OS had running cannot keep fetching out of
    // its own buffers while we re-point them.  Same reason the dA JoRMaS runner clears DMAF_ALL.
    *dmaconPointer = 0x000Fu;                                  // clear AUD0..3 (no SETCLR)
    for (int ch = 0; ch < 4; ch++) AUD_VOL(ch) = 0u;

    // Point all Paula channels at the pure-tone wave, start silent.
    // update_paula_channel() re-points each channel per its AUDC distortion.
    for (int ch = 0; ch < 4; ch++) {
        AUD_PTR(ch) = (uint32_t)wave_pure;
        AUD_LEN(ch) = 1u;   // 1 word = 2 bytes
        AUD_PER(ch) = 256u; // default period, updated by POKEY writes
        AUD_VOL(ch) = 0u;   // silent until AUDC writes activate channels
    }

    // Disable the Paula "LED" audio low-pass filter (one-time HW write): CIA-A PRA bit 1
    // ($BFE001, 0x02) HIGH = LED dim + filter OFF; LOW = LED bright + filter ON (~5-6 kHz RC).
    // Kickstart leaves it ON after boot, which rolls off the highs.  The Atari POKEY has no such
    // filter, so switching it off is the faithful choice (and keeps the SFX bright).
    // Saved first: it is machine state like DMACON/INTENA, and audioShutdown puts it back --
    // otherwise the power LED stays dim and the OS's audio stays unfiltered after we exit.
    s_savedCiaaPra = (uint8_t)(*ciaapraPointer & 0x02u);
    *ciaapraPointer |= 0x02u;

    // Enable audio DMA for all 4 channels (DMAF_AUD0..3 = bits 0..3)
    *dmaconPointer = (uint16_t)(DMAF_SETCLR | 0x000Fu);
}

// Paula channel hand-back: the exact counterpart of audioInit.  Volumes first (Paula applies
// AUDxVOL at the next sample boundary, so the channel is silent before it stops feeding), then
// audio DMA off, then the one piece of MACHINE state audioInit changed -- CIA-A PRA bit 1, the
// low-pass filter / power LED -- back as we found it.  Nothing else: repointing AUDxLC at an
// in-image "silent" buffer buys nothing, because DOS unloads the hunks right after we return and
// that chip RAM goes back to exec like any other.  Same reason the latched AUDx interrupt
// requests are left alone: with DMA off, a channel that has been started sits in Paula's
// programmed-transfer mode re-raising AUDxIRQ to ask the CPU for the next word, so the bits
// cannot be handed back down (measured: the AUDx bits read clear right after this function and
// are latched again by the time run() returns, amiga/memaudit.gdb).  Harmless -- the restored INTENA has no AUDx bit, and
// audio.device clears INTREQ itself when it opens a channel.
void PlatformAmiga::audioShutdown()
{
    for (int ch = 0; ch < 4; ch++) AUD_VOL(ch) = 0u;
    *dmaconPointer = 0x000Fu;  // clear AUD0..3 (no SETCLR = clear)
    *ciaapraPointer = (uint8_t)((*ciaapraPointer & (uint8_t)~0x02u) | s_savedCiaaPra);
}

// ============================================================================
//  Platform interface — the bus/frame methods the C-compiled 6502 transliteration
//  reaches through the shared platform_cbridge.cpp (platform->hwRead etc.).
// ============================================================================

// Real Amiga vertical beam line (VPOSR/VHPOSR), defined in flight_native.cpp; used to
// derive a faithful ANTIC VCOUNT ($D40B) below.
extern "C" unsigned short rof_beam_line(void);

// Held flight inputs (level, NOT one-shot like the keyboard commands): the joystick
// directions read off PIA PORTA ($D300) and the fire button off TRIG0 ($D010), both
// active-LOW (bit clear = pressed).  The CIA-A keyboard ISR (keyboardHandler, below)
// sets/clears the matching bit on each key down/up edge; hwRead returns these instead of
// the old hardwired neutral.  PORTA stick-0 bits: 0=up 1=down 2=left 3=right (verified vs
// flight_control_integrate $8e74-$8eac); bits 4-7 (stick 1) stay high.  Declared here so
// hwRead — defined above the keyboard section — can see them.
static volatile uint8_t s_portaState = 0xFFu;   // joystick directions, active-low (neutral)
static volatile uint8_t s_trig0State = 0x01u;   // fire button, active-low ($00 = pressed)
// ...and the same two levels as sampled from the REAL Amiga joystick (port 1) by pollJoystick(),
// kept SEPARATE from the keyboard's copies above.  hwRead returns the AND of the two: both are
// active-low, so ANDing means "either the key or the stick is pressed", and a centred stick /
// unplugged port ($FF / $01) leaves the keyboard behaviour exactly as it was.  The keyboard
// mappings are NOT a fallback to be removed — they stay supported alongside the stick.
static volatile uint8_t s_joyPorta   = 0xFFu;
static volatile uint8_t s_joyTrig0   = 0x01u;
// The keyboard's two levels are IN-FLIGHT ONLY; the real stick's are live in every scene.  The
// arrow keys and Left Shift stand in for a stick that isn't plugged in, and a stick is a flight
// control — but PORTA/TRIG0 are read outside flight too (Standby's read_console_trig_delta $5A78
// launches the game on the trigger, and the level-selector card cycles the starting level on
// up/down), so an ungated keyboard put Left Shift on "launch" and the arrows on "change level" in
// the selector.  Gate on the flight VBI vector being live rather than on the key edges, so a key
// still held across a scene boundary cannot strand an active-low bit in the new scene.
static inline bool keyboardStickLive()
{
    return (uint16_t)(mem[0x0222] | (mem[0x0223] << 8)) == 0x4FF5u;   // VVBLKI = the flight VBI
}
// SHIFT, as POKEY SKSTAT ($D20F) bit3 sees it — active-LOW, like every other Atari key register.
// The ONLY reader in the binary is standby_level_select_loop $5978 at $59c5: joystick-down OR shift
// held picks the level-DECREMENT branch, anything else increments.  Both Amiga shift keys feed it,
// one bit each, so releasing one while the other is held stays "held".  Ungated (unlike the stick
// emulation above) because SHIFT is a real key the Atari can read in any scene, and no other code
// reads SKSTAT — in flight Left Shift doubles as the trigger, which nothing conflicts with.
static volatile uint8_t s_shiftMask = 0x00u;    // bit0 = left shift, bit1 = right shift
#ifdef ROF_FORCE_SELECT
// Synthetic stick for the headless SELECT probe, ANDed into PORTA *ungated*: the probe drives
// joystick-up on the selector CARD, which is exactly what the gate above shuts out.  Every other
// FORCE_* harness injects on the real-stick side (s_joyPorta/s_joyTrig0) and so is unaffected,
// but pollJoystick() overwrites those every vblank AFTER the probe blocks run, hence a third var.
static volatile uint8_t s_probePorta = 0xFFu;
#endif
// CONSOL ($D01F) console keys (START/SELECT/OPTION), active-low; idle $07.  Kept in a DEDICATED
// state var (like s_portaState/s_trig0State) — NOT read from mem[$D01F] — so a stray/aliased RAM
// write to the $D000 hardware-mirror page can't corrupt this hardware INPUT register.  (It used to
// read mem[$D01F], which was being clobbered to $06 sub-frame during flight → read_console_trig_delta
// $5A78 saw START "held" → spuriously re-entered boot_standby_launch_driver's launch block L_634f → event $01
// poly4 over the range beep, and sound_retrigger_random between beeps.  Matches the Atari, where the
// CONSOL read is a hardware register isolated from RAM.)
static volatile uint8_t s_consolState = 0x07u;

uint8_t PlatformAmiga::hwRead(uint16_t addr)
{
    if (addr == 0xD20Au) return pokey_random_step();  // POKEY RANDOM register
    // SKSTAT ($D20F read) is a STATUS register, distinct from the SKCTL we WRITE to the
    // same address.  The standby level-select (standby_level_select_loop $5978, reached via the
    // Title Screen) reads bit3 = the SHIFT key, ACTIVE-LOW (1 = NOT pressed): shift idle →
    // the level-INCREMENT path (joystick-up / SELECT raise the starting level), shift held →
    // decrement.  Returning the POKEY write-shadow (pokey[$F], bit3=0) reads as "shift held",
    // so the code always decremented → joystick-up appeared to do nothing / wrong direction.
    // Emit a proper idle SKSTAT (nothing pressed) so up=raise, down=lower, matching the Atari —
    // with bit3 driven by the live Amiga shift keys, which is what makes Shift+SELECT lower the
    // level the way SHIFT+SELECT does on the Atari.  Every other bit stays idle: nothing in the
    // binary reads them (this is the only $D20F read, at $59c5).
    if (addr == 0xD20Fu) return s_shiftMask ? (uint8_t)(0xFFu & ~0x08u) : 0xFFu;   // bit3 = SHIFT
    if (addr >= 0xD200u && addr < 0xD210u) return pokey[addr - 0xD200u];
    // PIA PORTA ($D300): Atari joysticks are ACTIVE-LOW (1 = open/neutral).  Driven by the real
    // port-1 stick in every scene, and by the Amiga arrow keys (stick-0 bits 0-3) in FLIGHT only;
    // neutral $FF = stick centred.
    if (addr == 0xD300u) {
        uint8_t p = (uint8_t)((keyboardStickLive() ? s_portaState : 0xFFu) & s_joyPorta);
#ifdef ROF_FORCE_SELECT
        p &= s_probePorta;
#endif
        return p;
    }
    // CONSOL ($D01F): console keys (START/SELECT/OPTION), ACTIVE-LOW (bit clear =
    // pressed).  The Amiga keyboard handler maintains it in mem[$D01F] (idle $07;
    // F1 clears bit0 for START).  Reflect that — falling through to 0 reads as
    // "all console keys held", so the genuine attract idle loop sees START/launch
    // pressed and auto-starts the DEMO DROID demo within seconds.  ($D01F writes go
    // through platform_hw_write, which drops non-POKEY addresses, so the keyboard's
    // mem[$D01F] is never clobbered by genuine code.)
    if (addr == 0xD01Fu) {
        // QoL divergence (user decision): the joystick FIRE button also advances the
        // two boot cinematics, alongside START.  The Station's attract loop ($1a01) exits on an
        // RTCLOK timeout, any keyboard key ($02FC != $FF), or `CONSOL == $06` = START — it never
        // reads TRIG0.  That loop is TRANSLITERATED code we must not hand-edit, so report fire AS
        // START while the Station's own VBI vector ($1B30, installed by station_init at $198d) is
        // the live VVBLKI.  Scoping it to that vector is what keeps the fold invisible everywhere
        // else: the Logo does its own platform-neutral TRIG0 check (rof_logo.c) and Standby already
        // responds to TRIG0 through read_console_trig_delta $5A78.
        uint8_t c = s_consolState;
        if (s_joyTrig0 == 0x00u && (uint16_t)(mem[0x0222] | (mem[0x0223] << 8)) == 0x1B30u)
            c &= (uint8_t)~0x01u;                // clear bit0 -> $07 becomes $06, which $1a11 wants
        return c;                                // dedicated input state, isolated from mem[] corruption
    }
    // TRIG0-3 ($D010-$D013): joystick fire buttons, ACTIVE-LOW ($01 = released,
    // $00 = pressed).  TRIG0 = the real port-1 button in every scene, plus Left Shift in FLIGHT
    // only; the others stay released.  (Released-by-default matters outside flight:
    // read_console_trig_delta $5A78 computes (CONSOL&$01) - TRIG0, so a trigger that reads pressed
    // at Standby launches the game — which is exactly why the keyboard's copy is gated.)
    if (addr == 0xD010u) return (uint8_t)((keyboardStickLive() ? s_trig0State : 0x01u) & s_joyTrig0);
    if (addr >= 0xD011u && addr <= 0xD013u) return 0x01u;
    // ANTIC VCOUNT ($D40B): the transpiled init code busy-waits on the beam position
    // (wait_vcount_eq $3C75, wait_vcount_ge_7a $3C7B) — spin until VCOUNT == a target or
    // >= $7A.  VCOUNT reflects the vertical scan counter at TWO-LINE resolution (bits
    // 1-8; bit 0 unconnected), counting 0..$9B on PAL.  Derive it faithfully from the
    // real Amiga vertical beam (VPOSR/VHPOSR via rof_beam_line): VCOUNT = beam_line >> 1.
    // Because the hardware beam advances on its own, these spins terminate naturally —
    // including the un-hooked wait_vcount_ge_7a — and any other VCOUNT reader gets a
    // physically meaningful value, not a per-read fiction.
    if (addr == 0xD40Bu) return (uint8_t)(rof_beam_line() >> 1);
    return 0u;
}

// Death-cinematic teardown signal (energy-out/crash).  The Atari's $4F76
// intro_teardown_fade_loop writes DMACTL ($D400)=0 to blank the ANTIC playfield before it
// fades COLBK ($00D4) to black.  On the Amiga the copper owns the display, so that write is
// otherwise dropped — latch it here so renderFrame stops the terrain convert and shows a
// solid full-screen colour faded from mem[$00D4] (mirroring "DMA off, only COLBK shows").
// Set on a 0 write during flight, cleared on any nonzero DMACTL write (and by renderFrame
// when the scene leaves flight).  See the death-cinematic memory.
extern "C" { volatile unsigned char g_flightBlank = 0; }

#ifdef ROF_FLIGHT_PROBE
// FORCE_PAUSE probe: how many ESC presses the harness had to inject before the freeze took
// ($0043 != 0), and the g_vbiCount at which it did.  g_fpFreezeVbi == 0 = the pause never engaged,
// so any "the strobe didn't reach X" reading from that run is vacuous.
extern "C" { volatile unsigned short g_fpTries     = 0; }
extern "C" { volatile unsigned short g_fpFreezeVbi = 0; }
// BREAK/Restart probe: bumped by the g_restartJmp handler in run() each time a restart is taken.
// Lets the headless FORCE_BREAK / FORCE_BREAK_EARLY runs confirm the longjmp actually fired.
extern "C" { volatile unsigned char g_restartCount = 0; }
// Standby door-field decode probe: bumped each time the $2000->viewportBitmap door decode runs;
// g_doorDecodeVbi stamps the last one.  Lets the restart runs confirm the doors were (re)decoded.
extern "C" { volatile unsigned char  g_doorDecodeCount = 0; }
extern "C" { volatile unsigned short g_doorDecodeVbi   = 0; }
// In-place SELECT level-cycle dispatch probes (post-mother-ship standby; read via FORCE_RETURN).
extern "C" { volatile unsigned short g_ipDispatch   = 0; }   // L_6324 idle dispatch reached
extern "C" { volatile unsigned short g_ipInPlace    = 0; }   // L_6332 in-place branch taken
extern "C" { volatile unsigned short g_ipDoorScroll = 0; }   // level<max → door-scroll rebuild
extern "C" { volatile unsigned short g_ipIntroWrap  = 0; }   // level>=max → intro_screen_build_seq wrap
// Level-complete LIFT probes (`make PROBES=1 FORCE_RETURN=1 LEVEL_DONE=1`, amiga/level_done.gdb).
// The lift is the elevator that scrolls the LEVEL doors up after a completed level; it is entered
// only when the pilot quota was met, which no other harness arranges.
extern "C" { volatile unsigned short g_liftClear  = 0; }     // L_6219 quota-met branch taken
extern "C" { volatile unsigned short g_liftScroll = 0; }     // L_622d a lift scroll started
extern "C" { volatile unsigned short g_liftPass   = 0; }     // L_6268 one level step of the lift
extern "C" { volatile unsigned short g_liftSpinB  = 0; }     // L_628f entered the "$008B == $0F" spin
// HIGH-SCORE INITIALS-entry probes (`make PROBES=1 NAME_ENTRY=1`, amiga/name_entry.gdb).  Bumped
// from PRE_INSN_HOOKS in the transpiled name_entry_loop / render_text_cell.
extern "C" { volatile unsigned short g_neEnter = 0; }        // $5B6C name_entry_loop called
extern "C" { volatile unsigned short g_nePass  = 0; }        // $5B81 past validate_save_state
extern "C" { volatile unsigned short g_neWait  = 0; }        // $5C6E in the input wait
extern "C" { volatile unsigned short g_neGlyph = 0; }        // $5CB2 rendering a glyph cell
#ifdef ROF_COMBAT_LOAD
// COMBAT-LOAD benchmark counters (`make COMBAT=1 PROBES=1`, read via amiga/combat_probe.gdb).
// They exist to PROVE the combat load is real before any timing is quoted from it — a run
// with g_clExplode==0 is the "firing into the void" mistake all over again.
extern "C" { volatile unsigned short g_clExplode   = 0; }  // trigger_object_explosion calls
extern "C" { volatile unsigned short g_clShotHit   = 0; }  // our shot destroyed a map occupant
extern "C" { volatile unsigned short g_clEnemyFire = 0; }  // emplacement queued a bolt at us
extern "C" { volatile unsigned short g_clImpact    = 0; }  // bolt HIT us (near-camera wedge)
extern "C" { volatile unsigned short g_clSaucer    = 0; }  // flying saucers spawned
extern "C" { volatile unsigned short g_clObjDraw   = 0; }  // ground objects rastered
extern "C" { volatile unsigned short g_clObjNear   = 0; }  // ...of those, depth < 4 (biggest)
extern "C" { volatile unsigned short g_clReseed    = 0; }  // VBI top-ups of aged-out emplacements
extern "C" { volatile unsigned long  g_clObjEnter  = 0; }  // cells with an occupant = ALL object work
                                                       // (g_clObjDraw only counts the ones that
                                                       //  reached raster_scaled_object, depth<$0D)
extern "C" { volatile unsigned short g_clObjDist[13] = {0}; }  // draws by $0051 depth (0 = closest)
extern "C" { volatile unsigned char  g_clLevel = 0, g_cl0621 = 0, g_cl0623 = 0, g_cl0624 = 0; }
// COMBAT-STATE FRAMERATE SPLIT — the only honest way this harness can price combat.
// Cross-BUILD end-to-end is invalid (a combat build and a quiet build fly different
// trajectories, so their bucket shares are not comparable), but splitting ONE run's frames by
// the combat state they were painted in is immune to that: same binary, same trajectory, same
// terrain.  vbiHandler classifies every flight vblank and attributes the painted frames since
// the previous vblank to it, so  FPS(state) = 50 * g_clFrm[state] / g_clVbi[state].
//   0 = EXPLOSION  ($0041 game_state != 0 — an explosion or bolt impact is animating)
//   1 = SAUCER     (a flying-saucer object slot is live: $006A bit7 clear)
//   2 = SHOT       (our own laser is in flight: $0036 object slot != $80 = reset_object_slot's
//                   idle marker) — separates the PLAYER's weapon from the enemies'
//   3 = QUIET      (none of the above; note emplacements are still drawn in this state)
extern "C" { volatile unsigned long g_clFrames = 0; }          // painted terrain frames (all states)
extern "C" { volatile unsigned long g_clVbi[4] = {0,0,0,0}; }  // flight vblanks per state
extern "C" { volatile unsigned long g_clFrm[4] = {0,0,0,0}; }  // painted frames per state
// Same idea, but split by HOW MANY ground objects (gun emplacements / bases / pilots) the last
// painted frame actually rastered: 0 / 1-2 / 3-5 / 6+.  The explosion split above only priced
// the animation; this prices the PERSISTENT load — objects on screen every frame — which is
// where most of the combat cost turned out to live.  Again one binary, one trajectory.
extern "C" { volatile unsigned short g_clObjFrame  = 0; }      // objects drawn since the last frame
extern "C" { volatile unsigned char  g_clObjBucket = 0; }      // that count, bucketed
extern "C" { volatile unsigned long  g_clVbiObj[4] = {0,0,0,0}; }
extern "C" { volatile unsigned long  g_clFrmObj[4] = {0,0,0,0}; }
// PER-PHASE FRAME DECOMPOSITION, split by combat state (see the CL_PH block in rof_native.c).
// [phase][state], state 0 = an explosion/bolt is live this iteration, 1 = not.
// Phases: 0 SETUP, 1 CLEAR, 2 DRAW (terrain+objects), 3 BOLT (game_state_update),
//         4 ENEMY (enemy_check), 5 FRAME (ds_frame = renderFlightDirect+sprites+copper+audio).
// Both terrain passes are bracketed now — they were not before, which is why every historical
// "terrain is N% of the frame" figure was taken over half the work.
extern "C" { volatile unsigned long g_clPh[6][2]  = {}; }
extern "C" { volatile unsigned long g_clPhIter[2] = {0,0}; }
extern "C" { volatile unsigned char g_clPhState   = 1; }
// RAW iteration wall time (no ISR subtraction) so the budget can be closed:
//     wall = ISR (g_isrBeamLines) + sum(phases) + unbracketed
extern "C" { volatile unsigned long g_clIterWall[2] = {0,0}; }
extern "C" { volatile unsigned long g_clIterPrev    = 0; }
// Sticky "an explosion/bolt was live at some point during this iteration", set by the VBI and
// consumed by CL_PH_ITER.  A flight iteration spans many vblanks at these frame rates, so an
// instantaneous sample of $0041 at the top of the iteration is a near-useless classifier.
extern "C" { volatile unsigned char g_clExplSeen = 0; }
// REAL flight-VBI handler ticks + firings, split by combat state (accumulated in
// rof_native_amiga.cpp's flight_vbi_native from dHandler, which excludes the ZP-audit probe).
extern "C" { volatile unsigned long g_clIsr[2]  = {0,0}; }
extern "C" { volatile unsigned long g_clIsrN[2] = {0,0}; }
// DRAW ticks + iterations bucketed by $28DA scanner_target_range (>>5) — a proxy for how disturbed
// the attitude is, NOT altitude.  Lets the combat and quiet runs be compared at a MATCHED bucket —
// a per-call comparison, immune to the cross-build objection.
extern "C" { volatile unsigned long g_clAltDraw[8] = {}; }
extern "C" { volatile unsigned long g_clAltIter[8] = {}; }
extern "C" { volatile unsigned char g_clAltBucket  = 0; }
// DRAW SUB-SPLIT (see the CL_SUB block in rof_native.c).  Localises combat's +474 t/it inside the
// DRAW phase without any cross-build reasoning:
//   [0] head = per-frame table fills + compute_row_xspans   [1] obj = terrain_draw_objects
//   [2] age  = the $0A00 near-max cell aging scan (256 volatile reads, only impacts arm it)
// tail = DRAW_total - head - obj, computed in combat_probe.gdb so the four early returns in the
// function's tail need no bracket.  g_clDrawSubN = bracketed calls, for the bracket-floor subtraction.
extern "C" { volatile unsigned long g_clDrawSub[3] = {}; }
extern "C" { volatile unsigned long g_clAgeScans   = 0; }
extern "C" { volatile unsigned long g_clDrawSubN   = 0; }
// Terrain tree entries (one subdivide call per visible pair).  A count, so no bracket floor.
extern "C" { volatile unsigned long g_clSubCalls   = 0; }
// CALIBRATION LOAD (`make CALIBRATE=1`, see the CL_CAL block in rof_native.c).  Two fixed-trip,
// data-independent loops run once per flight iteration, so their cost prices the MACHINE rather
// than the work: if they inflate in combat, so does every phase, and that part of the combat
// delta is DMA contention rather than computation.
extern "C" { volatile unsigned long g_clCalMem  = 0; }   // 1024 scattered volatile mem[] byte reads
extern "C" { volatile unsigned long g_clCalCpu  = 0; }   // 1024-step 16-bit LFSR, registers only
extern "C" { volatile unsigned long g_clCalN    = 0; }   // iterations calibrated
extern "C" { volatile unsigned long g_clCalSink = 0; }   // keeps the loops alive against DCE
extern "C" { volatile unsigned long g_clCalNoDma = 0; }  // the MEM loop again, AUD+SPRITE DMA masked
// A 128-read window counted only when NO ISR fired inside it — the one calibration that needs
// no ISR subtraction, so it tells a slower machine apart from a subtraction artifact.
extern "C" { volatile unsigned long g_clCalIsrFree  = 0; }
extern "C" { volatile unsigned long g_clCalIsrFreeN = 0; }
// Same window again but BEAM-LOCKED to scanline 200, so both builds measure the same slice of
// the same frame — bitplane DMA only steals CPU slots inside the display window.
extern "C" { volatile unsigned long g_clCalLocked  = 0; }
extern "C" { volatile unsigned long g_clCalLockedN = 0; }
// The 1024-read load again, measured as 8 short ISR-free sub-windows: the row that tells a
// slower machine apart from the long bracket's ISR-subtraction bias.
extern "C" { volatile unsigned long g_clCalSplit  = 0; }
extern "C" { volatile unsigned long g_clCalSplitN = 0; }
#ifdef ROF_OBJ_SHAPE
// OBJECT-PLOTTER SHAPE PROBE (`make COMBAT=1 PROBES=1 OBJ_SHAPE=1` + amiga/obj_shape.gdb).
// Measures the object plotter instead of inferring it from a visit count: two brackets (the whole plotter chain at its call sites, and the scaled
// blit nested inside it) plus the shape of every early-out.  The per-cell counts accumulate in
// LOCALS inside raster_scaled_object and are flushed once per call, so the hot loops carry no
// volatile traffic.
extern "C" { volatile unsigned long g_opTicks = 0; }   // ISR-corrected ticks in project+plot_object
extern "C" { volatile unsigned long g_opCalls = 0; }   // plotter-chain calls (incl. the empty bail)
extern "C" { volatile unsigned long g_opEmpty = 0; }   // $0A00 occupant == 0 -> immediate return
extern "C" { volatile unsigned long g_opStep  = 0; }   // $232E==0 && $2300<$22 -> return
extern "C" { volatile unsigned long g_opPathA = 0; }   // $0900 peak (emplacement) -> A822 plotter
extern "C" { volatile unsigned long g_opPathB = 0; }   // -> A90A plotter
extern "C" { volatile unsigned long g_opaBusy  = 0; }  // A822: $2487/$242D busy -> return
extern "C" { volatile unsigned long g_opaMask  = 0; }  // A822: occupant >= $FA (dying) -> step/4
extern "C" { volatile unsigned long g_opaBelow = 0; }  // A822: below the column clip -> return
extern "C" { volatile unsigned long g_opaDist  = 0; }  // A822: ran point_distance + clip_row_top
extern "C" { volatile unsigned long g_opaFire  = 0; }  // A822: reached the RANDOM fire-queue gate
extern "C" { volatile unsigned long g_opaDepth = 0; }  // A822: step_hi >= $0D -> no scaled blit
extern "C" { volatile unsigned long g_opbCross = 0; }  // A90A: the trailing 4-pixel cross ran
extern "C" { volatile unsigned long g_rsCalls  = 0; }  // raster_scaled_object calls
extern "C" { volatile unsigned long g_rsTicks  = 0; }  // ...ticks inside it (NESTED in g_opTicks)
extern "C" { volatile unsigned long g_rsRows   = 0; }  // outer-loop rows walked
extern "C" { volatile unsigned long g_rsCells  = 0; }  // inner-loop cells visited
extern "C" { volatile unsigned long g_rsPlots  = 0; }  // ...of those, shape bit set -> clip_row_top
extern "C" { volatile unsigned long g_rsCellMax = 0; } // worst single call (cap is 12 rows x 32 = 384)
extern "C" { volatile unsigned long g_rsCellsByStep[14] = {}; }  // cells, by entry depth $0051 (13=other)
extern "C" { volatile unsigned short g_rsCallsByStep[14] = {}; } // calls, same buckets
// ...and by CALL SIZE (<16 / 16-63 / 64-127 / 128-255 / 256+ cells).  The blit's mean call is 15
// cells and its worst is the 384-cell cap, so a mean is useless for judging a change aimed at the
// near/exploding-object calls.  Comparing the SAME size bucket across two builds keeps this a
// per-call metric rather than an end-to-end one.
extern "C" { volatile unsigned long g_rsBktCalls[5] = {}; }
extern "C" { volatile unsigned long g_rsBktCells[5] = {}; }
extern "C" { volatile unsigned long g_rsBktTicks[5] = {}; }
#endif
#endif
// Door-scroll liveness: total dl_lms_scroll_step calls via the $008B branch (level-select elevator scroll).
extern "C" { volatile unsigned short g_dlScrollCount = 0; }
// Door-scroll render-side probe: BPLxPT repoints + the row range the ISR scrolled through.
extern "C" { volatile unsigned short g_dsRepoints = 0; }
extern "C" { volatile unsigned short g_dsMaxRow   = 0; }
extern "C" { volatile unsigned short g_dsMinRow   = 0xFFFF; }
extern "C" { volatile unsigned short g_dsDecodes  = 0; }   // full-field decodes (should be few: dirty-gated)
// Door-scroll copper-rewrite TIMING probe (the "one misaligned-cockpit frame between levels" bug):
// the raster line the ISR has reached when it rewrites the live standby copper's terrain runs.  The
// cockpit region's WAIT is kCockpitLine-1 = 171; a rewrite that lands at/after that line writes the
// cockpit BPLxPT moves behind the beam -> the already-satisfied WAIT fires mid-line and the cockpit
// bitplanes shift horizontally for that frame.
extern "C" { volatile unsigned short g_dsRunLine[24]  = {}; }   // ring: beam line AT the setTerrainRuns call
extern "C" { volatile unsigned char  g_dsRunDec[24]   = {}; }   // ring: did the full-field decode run first?
extern "C" { volatile unsigned char  g_dsRunN[24]     = {}; }   // ring: run count written
extern "C" { volatile unsigned char  g_dsRunIdx   = 0; }
extern "C" { volatile unsigned short g_dsRunWrites = 0; }  // total setTerrainRuns calls from the ISR
extern "C" { volatile unsigned short g_dsRunLate   = 0; }  // ...of which landed at/after line 171
extern "C" { volatile unsigned short g_dsRunMaxLn  = 0; }  // worst beam line seen at a rewrite
// ...and the cost of the thing that makes it late: the full-field decode, in raster lines
// (PAL frame = 312, so >312 means the decode alone outlasts a whole frame).
extern "C" { volatile unsigned short g_dsDecLines    = 0; }  // last decode's cost in raster lines
extern "C" { volatile unsigned short g_dsDecLinesMax = 0; }
extern "C" { volatile unsigned short g_dsDecEntryLn  = 0; }  // beam line when the decode started (last)
extern "C" { volatile unsigned short g_dsDecRows     = 0; }  // field rows the last decode covered
extern "C" { volatile unsigned char  g_dsDecRing[24] = {}; } // ...and the last 24 decodes' row counts
extern "C" { volatile unsigned char  g_dsDecRingIdx  = 0; }
// why each decode ran: 1 = dirty flag with no marked range, 2 = dirty flag with a marked range,
// 3 = renderFrame's terrainDirty full re-decode.
extern "C" { volatile unsigned char  g_dsDecWhy = 0, g_dsDecWhyRing[24] = {}; }
// Whole-VBI-ISR cost while the standby body ($52D7) is live, in raster lines (PAL frame = 312).
extern "C" { volatile unsigned short g_isrEntryLine = 0, g_isrStbyLines = 0, g_isrStbyMax = 0; }
extern "C" { volatile unsigned short g_isrStbyOver = 0; }    // ISRs that outlasted ~a whole frame
extern "C" { volatile unsigned long  g_isrStbyCnt  = 0; }
extern "C" { volatile unsigned long  g_blackHoldFrames = 0; }  // frames the EmptyCopperList (black) was held
// Bug-3 probe: whether the top door band was black at the earliest (smallest-g2) doors frame.
extern "C" { volatile unsigned char  g_doorTopBlack = 0; }
extern "C" { volatile unsigned char  g_doorTopBlackVp = 0; }   // same sample taken from viewportBitmap (the pre-fix door source)
extern "C" { volatile unsigned char  g_doorTopG2    = 0xFF; }
extern "C" { volatile unsigned char  g_doorTopSeen  = 0; }
// Restart-flash ring: per-vblank VVBLKI + hold flag, armed at the trampoline/restart.
extern "C" { volatile unsigned short g_vvRing[32] = {}; }
extern "C" { volatile unsigned char  g_vvHold[32] = {}; }
extern "C" { volatile unsigned char  g_vvIdx  = 0; }
extern "C" { volatile unsigned char  g_vvArmed = 0; }
extern "C" { volatile unsigned char  g_blankForRestartCount = 0; }
extern "C" { volatile unsigned char  g_l3d0cFired = 0; }   // $3D23 level_or_state!=0 clear branch fired
#endif

// rof_pokey_write: the direct, non-virtual POKEY write fast-path (bus.h routes $D200-$D20F
// here, skipping the C-bridge + virtual hwWrite dispatch).  Change-detect first: the 50Hz SFX
// envelope engine rewrites AUDF/AUDC every tick, often with the same value; recomputing the
// Paula channel (period divide + waveform select) for an unchanged register is pure waste.
#ifdef ROF_FLIGHT_PROBE
extern "C" unsigned short rof_beam_line(void);
extern "C" { volatile unsigned long g_pUPC = 0, g_upcCalls = 0, g_pokeyWrites = 0, g_pokeyChanged = 0; }
static inline void upc_timed(uint8_t ch) {
    if (s_upcSeen & (1u << ch)) g_upcRedund++;                  // a dirty bit would collapse this one
    else { g_upcDistinct++; s_upcSeen |= (uint8_t)(1u << ch); }  // ...this one it would still do
    unsigned short a = rof_beam_line(); update_paula_channel(ch);
    unsigned short b = rof_beam_line();
    g_pUPC += (b >= a) ? (unsigned short)(b - a) : (unsigned short)(b + 313 - a);
    g_upcCalls++;
}
#else
static inline void upc_timed(uint8_t ch) { update_paula_channel(ch); }
#endif

#ifdef ROF_AUDIO_TRACE
// `make PROBES=1 AUDIO_TRACE=1` — marks where each launch began, so the restart rows logged by
// aud_restart_log() can be read against "the first launch" vs "the relaunch".  Armed from the
// auto-START press (window 0) and from FORCE_RELAUNCH's second press (window 1).
extern "C" {
    volatile unsigned short g_atStart[2] = { 0, 0 };
}
extern "C" void rof_audio_trace_arm(unsigned char w)
{
    if (w > 1u || g_atStart[w]) return;                 // arm once
    g_atStart[w] = g_vbiCount ? g_vbiCount : 1u;
}
#endif

extern "C" void rof_pokey_write(uint8_t reg, uint8_t val)
{
    uint16_t addr = (uint16_t)(0xD200u + reg);
#ifdef ROF_FLIGHT_PROBE
    g_pokeyWrites++;
#endif
    if (pokey[reg] == val) { mem[addr] = val; return; }
#ifdef ROF_FLIGHT_PROBE
    g_pokeyChanged++;
#endif
    pokey[reg] = val;
    mem[addr]  = val;   // keep Atari-RAM mirror in sync (matches transpile bus_write)

    if (reg <= 7u) {
        uint8_t ch = reg >> 1u;             // AUDF or AUDC write — update the affected channel
        upc_timed(ch);
        // A 16-bit pair's joined divider lives on the HIGH channel and includes the LOW half's
        // AUDF, so an AUDF1/AUDF3 write must re-derive ch1/ch3 too (atari800's chan_mask).
        if ((reg == 0u && (pokey[8] & 0x10u)) || (reg == 4u && (pokey[8] & 0x08u)))
            upc_timed((uint8_t)(ch + 1u));
    } else if (reg == 8u) {
        for (uint8_t ch = 0; ch < 4; ch++) upc_timed(ch);   // AUDCTL — all channels
    }
}

#if defined(ROF_SFXMIX_VERIFY) || defined(ROF_SFXMIX_FUZZ)
// The sfxmix in-process differential runs the asm twin and the C twin back-to-back on the same
// state.  rof_pokey_write's change-detect (pokey[reg] == val -> skip update_paula_channel) would
// otherwise make the SECOND run free — it re-writes the values the first run just latched — so
// the harness would report the asm as ~1.6x SLOWER purely from running first.  These let it
// rewind the shadow so both sides do identical Paula work.  Only pokey[] needs rewinding: the
// compared observable is mem[$D200-$D20F], which rof_pokey_write writes on BOTH paths.
extern "C" void rof_pokey_shadow_save(uint8_t* dst) { for (int i = 0; i < 16; i++) dst[i] = pokey[i]; }
extern "C" void rof_pokey_shadow_load(const uint8_t* src) { for (int i = 0; i < 16; i++) pokey[i] = src[i]; }
// ...and for the boot-time fuzz: discard the garbage "want" its random POKEY writes
// recorded, so the first real flush_paula does not program Paula from fuzz state.
extern "C" void rof_pokey_want_reset(void) { want_valid = 0; }
#endif

void PlatformAmiga::hwWrite(uint16_t addr, uint8_t val)
{
    if (addr == 0xD400u) { g_flightBlank = (val == 0u) ? 1u : 0u; return; }  // DMACTL: 0 = playfield blanked
    if (addr < 0xD200u || addr >= 0xD210u) return;  // only POKEY range
    rof_pokey_write((uint8_t)(addr - 0xD200u), val);
}

// shadowWrite / registerVBI / indirectJmp / setInterrupt are no-ops on Amiga (the 6502
// display/event code does nothing here, and the VBI is the real INTB_VERTB server) —
// shadowWrite/registerVBI/indirectJmp inherit the Platform base defaults.
void PlatformAmiga::setInterrupt(void (*/*fn*/)(void)) {}
int  PlatformAmiga::framesPerSecond() { return 50; }   // PAL
int  PlatformAmiga::loadImage(const char* /*path*/) { return 0; }  // image is embedded (incbin)

// --- frame rendering + quit --------------------------------------------------
// g_pumpQuit: the "user wants out" flag — set by renderFrame and pollEvents on
// left-mouse.  g_vbiCount: bumped once per REAL vertical-blank interrupt (vbiHandler
// below); renderFrame spins on it.  All file-local.
static volatile uint8_t  g_pumpQuit = 0;
// g_vbiCount defined earlier (ahead of flush_paula's beep capture).
static RescueOnFractalus* s_scene   = 0;   // running scene; set by run()

// flightShotTick: rebuild the player laser sprite from mem[] in the flight VBI (called from
// game_vbi_isr's $4FF5 branch, right after vbi_handler_flight fills mem[$0E32..]).  This is the
// faithful home for it — on the Atari the shot's HPOSP2/COLPM2/GRAFP2 are written by the VBI
// ($8c58, 50Hz), NOT the slow main-loop terrain render — so the bolt/explosion animate at full
// rate.  Runs at vblank START (beam off-screen), so the sprite-data write is race-free.
void PlatformAmiga::flightShotTick()
{
    if (s_scene) s_scene->buildShotSprite();
}

// flightScannerTick: the whole Long Range Scanner (#13) mirror, from the flight VBI (50Hz) — like
// flightShotTick this is the faithful home for both halves, and for the dot it is also the only
// race-free one:
//   * the GUIDE DOT (Atari missile M2) — buildScannerDotSprite consumes the range+bearing sighting the
//     flight loop PUSHED from the terrain display pass, and draws + BLINKS the dot from it.  Doing it
//     here (50Hz), not from the terrain render, is what makes the blink CPU-independent: the render
//     rate scales with the CPU, this ISR does not.  ⚠ It must not read mem[$28DA]/mem[$00CE] itself —
//     those are per-pass samples that alias against the free-running loop (see buildScannerDotSprite).
//   * the CLOSE-RANGE BLINK cells $33DF/$33E0 — startup_init() ($3FFA, likewise run in the handler
//     above) toggles their bit7 at 50Hz (a two-speed proximity blink), so decoding here makes the
//     blink run at full rate instead of the ~5-6fps main-loop render() cadence (which made it far
//     too slow and range 1 vs 2 indistinguishable).  s_scene decodes only on a bit7 flip (2 cells).
// Both write sprite/bitmap data the copper only reaches at line 179+ (the dashboard band), well
// past where this ISR ends, so the writes are safe unbuffered.
void PlatformAmiga::flightScannerTick()
{
    if (s_scene) {
        s_scene->buildScannerDotSprite();
        s_scene->decodeScannerBlinkCells();
    }
}

// ROF_TUNNEL_GROUP bridge: draw_symmetric_span_loop (rof_native.c) emits one whole ring group —
// the `count` nested outlines it just drew, in one colour — and we paint their union straight into
// the tunnel bitmap.  Deliberately a plain extern "C" call rather than a Platform virtual through
// platform_cbridge — this fires from inside the 50 Hz VBI ISR, where the indirection is not free
// and the work is Amiga-only rendering with no host counterpart.
extern "C" void platform_tunnel_group(uint16_t rowBase, uint8_t rowTop, uint8_t rowBot,
                                      uint8_t xL, uint8_t xR, uint8_t count, uint8_t colour)
{
    if (s_scene) s_scene->drawTunnelGroup(rowBase, rowTop, rowBot, xL, xR, count, colour);
}

// ROF_TUNNEL_COLS bridge: the pre-draw's three full-height guide columns, which are plotted
// outside the span loop and so never reach platform_tunnel_rect.
extern "C" void platform_tunnel_columns(uint16_t rowBase, uint8_t colL, uint8_t colR,
                                        uint8_t colR1, uint8_t colour)
{
    if (s_scene) s_scene->drawTunnelColumns(rowBase, colL, colR, colR1, colour);
}

// ROF_TUNNEL_SPANRUN bridge: one whole plot_terrain_span run (the pre-draw erase + the reveal's
// per-row coloured spans), which bypasses the group hook.
extern "C" void platform_tunnel_span_run(uint16_t rowBase, uint8_t r0, uint8_t r1, uint8_t xL,
                                         uint8_t xR, uint8_t count, uint8_t colour)
{
    if (s_scene) s_scene->drawTunnelSpanRun(rowBase, r0, r1, xL, xR, count, colour);
}

// Ring-rotation bridge: the ring VBI calls this on every tick that is about to rotate the colour
// ring, just BEFORE it draws the new group, so the LIVE copper list can take the rotation on the
// same frame the pixels appear.  See RescueOnFractalus::pokeTunnelRingAdvance().
extern "C" void platform_tunnel_ring_advance(void)
{
    if (s_scene) s_scene->pokeTunnelRingAdvance();
}


// Flight/init timing probes (enable with `make PROBES=1` → -DROF_FLIGHT_PROBE).  Sub-frame
// clock rof_subclock() = g_vbiCount*313 + beam_line, plus the accumulators that rof_native.c's
// FP_* macros and the renderFrame/atmosphere probes below write into.  All read from the gdb
// stub (amiga/diag_timing.gdb).  Compiled out by default so the SDL/release builds don't carry
// (or need to link) any of these symbols.
#ifdef ROF_FLIGHT_PROBE
// ⚠ RACE-FREE, and it MUST be.  A plain `g_vbiCount * 313 + rof_beam_line()` reads the two
// halves of the clock non-atomically: the VERTB ISR bumps g_vbiCount at beam line 0, so an ISR
// landing BETWEEN the two reads pairs the OLD frame number with a beam line that has already
// wrapped to ~0 and the clock jumps ~313 ticks BACKWARD.  Every FP_TIME bracket then computes
// `rof_subclock() - start` on unsigned longs, underflows to ~4.29e9, and one unlucky sample
// poisons a whole accumulator.  A vbi-count read on either side, retried until they agree, proves
// no VBI fired between the two halves.  Cost: two volatile short reads, nothing next to the CHIP
// register read it already does.
extern "C" unsigned long rof_subclock(void) {
    for (;;) {
        const unsigned short v0 = g_vbiCount;
        const unsigned short ln = rof_beam_line();
        const unsigned short v1 = g_vbiCount;
        if (v0 == v1) return (unsigned long)v0 * 313u + (unsigned long)ln;
    }
}
extern "C" { volatile unsigned long g_renderFrameCount = 0; }
extern "C" { volatile unsigned long g_probeDispSetup = 0, g_probeGameInit = 0,
    g_probeIntro = 0, g_probeRowAddr = 0, g_probeInitTotal = 0; }
extern "C" { volatile unsigned short g_probeFlightVbi = 0; }  // g_vbiCount at flight VBI install
// Flight-entry -> terrain-fade-start window decomposition (VBI frames vs game-loop iters).
extern "C" { volatile unsigned short g_fadeLoopVbi = 0; }   // vbi when flight loop (iterCount>=1) starts
extern "C" { volatile unsigned short g_fadeEntryVbi = 0, g_fadeStartVbi = 0; }
extern "C" { volatile unsigned short g_fadeEntryIter = 0, g_fadeStartIter = 0; }
extern "C" { volatile unsigned short g_fadeEntryFd = 0, g_fadeStartFd = 0; }
extern "C" { volatile unsigned char g_fadeEntryDC = 0, g_fadeDone = 0; }
extern "C" { volatile unsigned char g_fadeEntryState = 0, g_fadeStartState = 0; }   // mem[0x41]
extern "C" { volatile unsigned char g_fadeEntry66C = 0, g_fadeStart66C = 0; }       // mem[0x66C]
extern "C" { volatile unsigned char g_fadeEntryAlt = 0, g_fadeStartAlt = 0; }       // mem[0x34]
// renderFrame() no-yield-gap probe:
extern "C" { volatile unsigned short g_maxRenderGap = 0, g_maxGapAtVbi = 0, g_maxGapVvblki = 0; }
extern "C" { volatile unsigned char g_maxGap060B = 0, g_maxGap004A = 0; }
// Cinematic-only render-gap probe (launch VBI $52D7 active): isolates a tunnel->stars freeze.
extern "C" { volatile unsigned short g_maxCineGap = 0, g_maxCineGapAtVbi = 0; }
extern "C" { volatile unsigned char g_maxCineGap060B = 0; }
extern "C" { volatile unsigned short g_csGap = 0, g_csGapAtVbi = 0; }   // tunnel->stars window only
// standby->doors window render-gap probe (no vbi>360 gate, so it catches the early launch burst)
extern "C" { volatile unsigned short g_doorGap = 0, g_doorGapAtVbi = 0; }
extern "C" { volatile unsigned char g_doorGap060B = 0; }
// stretch-A per-function one-shot subclock deltas (ticks): find the standby->doors freeze.
extern "C" { volatile unsigned long g_saTicks[16] = {0}; }
// door-frame draw pixel-volume counters (span calls + total bytes/edges written).
extern "C" { volatile unsigned long g_dfVCalls = 0, g_dfVRows = 0, g_dfHCalls = 0, g_dfHCols = 0; }
// TUNNEL-GROUP cost probe (the "the outermost ring's drawing is clearly visible" report).  One
// draw_symmetric_span_loop call draws ONE ring group = $6E0F[i] nested rectangles in ONE colour,
// and it runs in the 50 Hz VBI ISR, so a group that costs more than 313 ticks drops a frame.  Per
// call: the site tag, the rectangle count, the vbi, and the subclock ticks (1 tick = 1 raster
// line = 63.56us).  Sites: 1/4 = the static pre-draws, 2 = the forward descent, 3 = the boost
// reverse ring.  The ring keeps the FIRST TG_N calls of sites 2+3 (the two the report is about);
// every site also gets a running aggregate, so the pre-draws stay accounted for.
// PLOT vs PAINT: `plotT` is the time inside the faithful 6502 mem[$1000] plot
// (fill_horizontal_span_core + fill_vertical_span_core), which is the floor no rendering change
// can move; total - plot is the Amiga paint, which is the part the batching attacks.
#define TG_N 40
extern "C" { volatile unsigned long  g_tgN = 0; }
extern "C" { volatile unsigned char  g_tgSite[TG_N] = {0}, g_tgRects[TG_N] = {0}; }
extern "C" { volatile unsigned short g_tgVbi[TG_N] = {0}; }
extern "C" { volatile unsigned long  g_tgTot[TG_N] = {0}, g_tgPlot[TG_N] = {0},
                                     g_tgPaint[TG_N] = {0}; }
extern "C" { volatile unsigned long  g_tgSiteCalls[5] = {0}, g_tgSiteRects[5] = {0},
                                     g_tgSiteTot[5] = {0}, g_tgSiteMax[5] = {0},
                                     g_tgSitePlot[5] = {0}, g_tgSitePaint[5] = {0}; }
// The reveal's OTHER span source: plot_terrain_span (emit_dl_coord_pairs tail-calls it once per
// revealed row), which paints through the same fillColor and is invisible to the group counters.
extern "C" { volatile unsigned long  g_tpCalls = 0, g_tpTot = 0, g_tpMax = 0, g_tpPlot = 0,
                                     g_tpPaint = 0; }

extern "C" void rof_tunnel_group(unsigned char site, unsigned char rects, unsigned long ticks,
                                 unsigned long plotTicks, unsigned long paintTicks)
{
    const unsigned s = (site < 5u) ? site : 0u;
    g_tgSiteCalls[s]++;
    g_tgSiteRects[s] += rects;
    g_tgSiteTot[s] += ticks;
    g_tgSitePlot[s] += plotTicks;
    g_tgSitePaint[s] += paintTicks;
    if (ticks > g_tgSiteMax[s]) g_tgSiteMax[s] = ticks;
    if (site != 2u && site != 3u) return;
    const unsigned long n = g_tgN;
    if (n < TG_N) {
        g_tgSite[n] = site; g_tgRects[n] = rects;
        g_tgVbi[n] = g_vbiCount;
        g_tgTot[n] = ticks; g_tgPlot[n] = plotTicks; g_tgPaint[n] = paintTicks;
    }
    g_tgN = n + 1;
}

extern "C" void rof_tunnel_span_cost(unsigned long ticks, unsigned long plotTicks,
                                     unsigned long paintTicks)
{
    g_tpCalls++;
    g_tpTot += ticks;
    g_tpPlot += plotTicks;
    g_tpPaint += paintTicks;
    if (ticks > g_tpMax) g_tpMax = ticks;
}
// decodeCockpitFull one-shot timing (chip-vs-fast-RAM experiment).
extern "C" { volatile unsigned long g_ckFullTicks = 0, g_ckFullCount = 0; }
// fill_terrain_columns one-shot timing (tunnel->stars setup gap).
extern "C" { volatile unsigned long g_fillTerrTicks = 0, g_fillTerrIsr = 0; }
// boot_standby_launch_driver launch-tail milestone stamps: rof_ds_mile(i) records g_vbiCount at milestone i,
// so a big jump between consecutive stamps localises the ~580ms cinematic freeze to one stretch.
extern "C" { volatile unsigned short g_dsMile[16] = {0}; }
extern "C" { volatile unsigned long g_burstClrTicks = 0, g_burstClrIsr = 0; }   // L_650b field-clear cost
extern "C" { volatile unsigned long g_burstMidTicks = 0, g_burstMidIsr = 0; }
extern "C" { volatile unsigned long g_sbATicks = 0, g_sbAIsr = 0, g_sbCTicks = 0, g_sbCIsr = 0; }
extern "C" void rof_ds_mile(int i) { if (i >= 0 && i < 16) g_dsMile[i] = g_vbiCount; }
// tunnel-prebuild probe: does the standby-construction ring draw (7262) run, and does its
// mem[$1000] ring field survive to the launch-time redraw (7601)?  (checksums + run flags)
// planet-approach spike probe (L_6578 loop): max advance_object_positions cost + max
// renderViewportModeD dirty-band decode (rows + ticks), each with the vbi it peaked at.
extern "C" { volatile unsigned long g_aopMax = 0, g_aopMaxVbi = 0; }
extern "C" { volatile unsigned long g_vpDecMax = 0, g_vpDecMaxVbi = 0, g_vpDecMaxRows = 0; }
// standby->doors gap localizer: stretch-A code sets g_saPhase as it progresses; renderFrame
// snapshots it (g_doorGapPhase) at the worst door-window gap so we know which phase preceded it.
extern "C" { volatile unsigned char g_saPhase = 0, g_doorGapPhase = 0; }
extern "C" { volatile unsigned long g_rbMax = 0; extern "C" volatile unsigned short g_rbMaxVbi = 0; }
// RTCLOK ownership-race probe: catch frames where RTCLOK ($0014) is advanced by BOTH the VBI
// body AND renderFrame (double-count -> equality spin-waits overshoot -> ~256-frame wrap), and
// frames where renderFrame read a "torn"/unexpected VVBLKI vector during the $52D7<->$4FF5 swap.
extern "C" { volatile unsigned short g_rtDoubleCount = 0, g_rtDoubleAtVbi = 0; }
extern "C" { volatile unsigned short g_rtZeroCount = 0, g_rtZeroAtVbi = 0; }
extern "C" { volatile unsigned short g_rtTornCount = 0, g_rtLastTornVec = 0, g_rtTornAtVbi = 0; }
// RTCLOK SKIP probe: while $4FF5 is active, renderFrame does NOT advance RTCLOK — the ISR does,
// once per REAL VBI.  If a single renderFrame spans >1 real VBI (slow render), RTCLOK jumps by
// >1 across one equality-spin iteration ($3CB8 push_a_thunk), which can step OVER the target and
// wrap 256 ticks.  Track the max single-iteration RTCLOK delta + how many iterations jumped >1.
extern "C" { volatile unsigned short g_rtJumpMax = 0, g_rtJumpGt1Count = 0, g_rtJumpAtVbi = 0; }
// VCOUNT busy-wait span probe (see pollEvents): longest run of frames a non-frame-pacing
// spin (wait_vcount_eq etc.) holds without a renderFrame — a big value = the equality miss.
extern "C" { volatile unsigned short g_maxPollSpinFrames = 0, g_maxPollSpinAtVbi = 0, g_pollSpinStartVbi = 0; }
static bool g_pollAfterRender = false;
// game_main_loop per-iteration + flight phase split (written by rof_native.c FP_* macros):
extern "C" { volatile unsigned long g_iterMax = 0, g_iterLast = 0, g_iterPostDs = 0; }
extern "C" { volatile unsigned short g_iterCount = 0, g_iterMaxAt = 0; }
#ifdef ROF_FORCE_ALIEN_ABOARD
// ALIEN_ABOARD ascent sampler (vbiHandler): the boosters climb with $0633 set.
extern "C" {
volatile unsigned short g_abVbi = 0, g_abIter0 = 0, g_abIterN = 0, g_abNSamp = 0;
volatile unsigned char  g_ab34First = 0, g_ab34Last = 0, g_ab34Max = 0;
volatile unsigned char  g_ab29Last = 0, g_ab2ALast = 0, g_ab26Last = 0, g_ab2ELast = 0;
volatile unsigned char  g_ab4ALast = 0, g_ab04Last = 0, g_abKeyPend = 0;
volatile unsigned char  g_abSamp34[32], g_abSamp29[32], g_abSamp2E[32];
volatile unsigned short g_abKeyWin = 0, g_abBreakVbi = 0, g_ab52B4 = 0;
}
#endif
extern "C" { volatile unsigned long g_fSetup=0,g_fClear=0,g_fDraw=0,g_fColl=0,g_fState=0,g_fEnemy=0; }
// ds_frame() total (both displayed halves per iteration) = renderFlightDirect + sprite builds +
// copper update + the audio flush.  Never bracketed earlier, so the phase sum used to
// miss the entire render side of the frame as well as all of terrain pass 2.
extern "C" { volatile unsigned long g_clFrameTicks = 0; }
// Stage-0 convert-pass cost (flight renderViewportModeD), beam-based, ISR-decontaminated.
extern "C" { volatile unsigned long g_fConvert=0; }
// atmosphere terrain-pen range during flight ($00DC/$00DD salmon→brown fade):
extern "C" { volatile unsigned char g_dcMin=0xFF, g_dcMax=0, g_ddMin=0xFF, g_ddMax=0; }
// Cockpit-decode probe (beam sub-frame ticks): g_fCockpit accumulates the time spent in the
// render() cockpit scan/decode block; g_fCockpitScans counts the frames it actually ran.
extern "C" { volatile unsigned long g_fCockpit=0, g_fCockpitScans=0; }
// OS interrupt-dispatch overhead probe ("the 8% unresolved/ROM bucket").  The flight PC
// profiler puts ~8% of its samples at Kickstart $F811F8 — the level-3 autovector entry
// (`movem.l d0-d1/a0-a1/a5-a6,-(sp)`, which then btsts INTREQR for BLIT/VERTB/COPER and
// jumps through ExecBase->IntVects[]).  That is exec's interrupt wrapper, not our code.
// VERTB is raised at the start of line 0, so the beam position when OUR AddIntServer
// handler finally runs measures EVERYTHING in between: the interrupted instruction
// finishing, the 68000 exception stacking, exec's wrapper, and any higher-priority VERTB
// server ahead of us — i.e. exactly the cost a raw autovector takeover could remove.
// Recorded as separate vpos/hpos sums so the target never needs a 32-bit multiply;
// irq_probe.gdb folds them into colour-clocks (1 line = 227 cc = 63.56us).
extern "C" { volatile unsigned long g_irqLatVsum=0, g_irqLatHsum=0, g_irqLatCnt=0; }
extern "C" { volatile unsigned short g_irqLatVmax=0, g_irqLatHmax=0; }
// INTENAR/INTREQR snapshot at VBI entry: which level-3 sources (bit4 COPER, bit5 VERTB,
// bit6 BLIT) are actually enabled decides the level-3 interrupt RATE — 50/s if only VERTB.
extern "C" { volatile unsigned short g_irqIntena=0, g_irqIntreq=0; }
// ---------------------------------------------------------------------------------------
// OS interrupt-chain taps — how OFTEN does an interrupt we do NOT own fire, and how long
// does its whole exec server chain run?  Each tap is a PAIR of servers on one chain: a
// pri-127 head (stamps the beam) and a pri-(-128) tail (accumulates the delta), both
// returning 0 so the walk continues untouched.  headCnt == tailCnt means nobody in between
// claimed the interrupt (a server returning non-zero ENDS the walk, so the tail is skipped).
//   [0] EXTER (level 6, CIA-B: timer A/B, TOD, FLG, SP — timer.device's MICROHZ unit etc.)
//   [1] PORTS (level 2, CIA-A: our keyboard handler hangs here via ciaa.resource)
//   [2] BLIT  (level 3, blit-done: the framework's blitter* helpers re-enable INTF_BLIT
//              right after starting a blit, so EVERY blit we issue raises one)
// Beam stamps are kept as separate v/h sums (as g_irqLat* does) so the target needs no
// 32-bit multiply; int_probe.gdb folds them into colour clocks (1 line = 227 cc = 63.56us).
// Chain duration only — the exception + Kickstart stub + walker ahead of the head tap is
// NOT included (that part is what the VERTB g_irqLat* number sizes).
// Slots: [0] EXTER whole chain, [1] PORTS whole chain, [2] PORTS up to pri 50 (= time in
// ciaa.resource), [3] PORTS up to pri 10 (= ciaa.resource + FS-UAE's "UAE fs" server).  Slots
// 2/3 are cumulative-from-the-head splits of slot 1, so subtracting them attributes the chain.
extern "C" { volatile unsigned long g_tapHeadCnt[4] = {0,0,0,0}, g_tapTailCnt[4] = {0,0,0,0}; }
extern "C" { volatile unsigned long g_tapVsum[4]    = {0,0,0,0}; }
extern "C" { volatile long          g_tapHsum[4]    = {0,0,0,0}; }
static   volatile unsigned short  s_tapV0[4]      = {0,0,0,0}, s_tapH0[4] = {0,0,0,0};
// Firings of OUR OWN level-2 handler in the PORTS-takeover build.  In that build the tap slots
// above read 0 — the taps are exec chain servers and exec's walker is no longer in the vector —
// so this is the PORTS rate, and a storm check on it (see portsHandler).
extern "C" { volatile unsigned long g_portsIrqCnt = 0; }
// VBI-body sub-profiling (beam-line deltas inside the flight VBI; normalize by isrCalls).
// integ/proj/sfx wrap individual native twins (rof_native.c).  The whole handler is timed by
// flight_vbi_native (g_flightProf.isrLines).  (The old top/atmo/hud/score/tail PRE_INSN_HOOK
// partition was retired when vbi_handler_flight went native — it had done its diagnostic job.)
extern "C" { volatile unsigned long g_pProj=0, g_pInteg=0, g_pSfx=0; }
extern "C" { volatile unsigned long g_pSfxEng=0, g_pSfxLoop=0, g_pSfxRing=0; }
extern "C" { volatile unsigned long g_sfxRingIters=0; }   // ring entries drained; /isrCalls = per-firing
// VBI handler section partition (the chunks NOT covered by integ/proj/sfx; see rof_native.c
// vbi_handler_flight).  Per-call = acc/isrCalls; sum(all sections)+integ+proj ≈ isrLines.
extern "C" { volatile unsigned long g_pDrawBr=0, g_pSimHead=0, g_pAtmo=0, g_pHud=0, g_pScore=0, g_pTail=0; }
// Stage-1 verifier: chip addr of terrainBitmap->data, so the gdb harness can dump the
// flight bitplanes and decode/diff them headlessly (no display needed).
extern "C" { volatile uint32_t g_terrainBmpAddr=0; }
// Stage-1 direct renderer's parallel bitmap (for pixel-diff vs the convert).
extern "C" { volatile uint32_t g_flightDirectAddr=0; }
// Stage-1 direct-render beam cost (same units as g_fConvert) for the head-to-head.
extern "C" { volatile unsigned long g_fDirect=0; }
// renderFlightDirect internal breakdown (beam ticks, accumulated over g_fdCalls):
// clear+wait / edge plot / fill+wait / plane2 scan / band convert / scanned-row count.
extern "C" { volatile unsigned long g_fdClear=0, g_fdEdge=0, g_fdFill=0, g_fdScan=0,
                                   g_fdBand=0, g_fdCalls=0, g_fdScanRows=0; }
// Render/glue-gap probe: attribute the per-iteration ds_frame() (= platform_tick_vbi +
// platform_render_frame) cost, which the phase buckets above do NOT cover.  ds_frame runs
// once per game-loop iteration, then ~300ms of two-pass terrain compute runs with the display
// frozen.  g_rRenderWall = raw beam ticks in scene->renderFrame(); g_rRenderCompute = the same
// ISR-subtracted (comparable to g_fDraw etc.); g_rIdleWall = raw ticks in the "wait next real
// VBI" spin (idle + ISR firings); g_rCalls = renderFrame invocations.  The compute sub-split
// (ISR-subtracted, written in RescueOnFractalus::renderFrame) is g_rPerFrame (perFrameWork: HUD
// sprites) / g_rRenderFn (render(): renderFlightDirect=g_fDirect + cockpit + title/compass) /
// g_rCopper (updateFlightCopper poke).
extern "C" volatile unsigned long g_isrBeamLines;  // defined in rof_native_amiga.cpp
// Noise-refill cost, accumulated in renderFrame now that noiseTick is main-loop work (still
// normalised by VBI firings in isr_full.gdb — see the note at the call site).
extern "C" volatile unsigned long g_vbiNoiseLines;  // defined in rof_native_amiga.cpp
extern "C" { volatile unsigned long g_rRenderCompute=0, g_rRenderWall=0, g_rIdleWall=0, g_rCalls=0; }
// INTEGER frame counters for the two flight pacing waits — immune to the beam-read race that
// poisons the tick accumulators above (g_fDraw et al. can run BACKWARDS).  g_idleFrames = VBI
// frames burned in renderFrame's "wait for the next real VBI" spin; g_flipWaitFrames = frames
// burned draining a deferred buffer flip at the top of renderFlightDirect.  Frames-per-iteration
// MINUS these is the compute, so an A/B can tell "this build does more work" from "this build
// lost a frame to vblank quantisation" — which a throughput number alone cannot.
extern "C" { volatile unsigned long g_idleFrames=0, g_flipWaitFrames=0, g_flipWaitCalls=0; }
extern "C" { volatile unsigned long g_rPerFrame=0, g_rRenderFn=0, g_rCopper=0; }
// Knock-gated ($0632) split of platform_render_frame: g_alTRScene = scene->renderFrame() (the
// dirty-rect composite + the renderFlightDirect while(flightSwapPending) flip wait); g_alTRIdle =
// the PlatformAmiga while(g_vbiCount==last) frame-sync wait after it.  Pinpoints the ~204ms.
extern "C" { volatile unsigned long g_alTRScene=0, g_alTRIdle=0; }
// Of g_alTRScene, the time specifically in renderFlightDirect's while(flightSwapPending) flip wait.
extern "C" { volatile unsigned long g_alTFlipWait=0; }
// flightVblankSwap during the knock: g_alVSwapRun = ISR firings that ran it; g_alVSwapCleared = of
// those, how many found flightSwapPending set (i.e. actually did the flip).  If Run >> Cleared, the
// ISR fires often but a flip is rarely pending -> the flip wait isn't ISR-starved; if Run ~ steps,
// the ISR itself is being throttled during the knock.
extern "C" { volatile unsigned long g_alVSwapRun=0, g_alVSwapCleared=0; }
// Altimeter sprite chip addresses (set in initialize) so the gdb harness can read their VSTART/
// VSTOP control words and confirm the bar Y vs mem[$281A]/$281B.
extern "C" { volatile uint32_t g_altimSprAddr=0, g_altimShipSprAddr=0, g_energySprAddr=0; }
extern "C" { volatile uint32_t g_viewportP3SprAddr=0, g_scopeP3SprAddr=0, g_flightCopperAddr=0; }
// Wide-object extension chain heads (segments 1-3 on ch5/ch6/ch1).  Each buffer holds TWO chained
// sprites, so amiga/wide_probe.gdb can read the extension's VSTART/VSTOP and then the chained
// element's control words right behind its data — the check that the energy bar / altimeter /
// left band triangle still arm now that they hang off the extension rather than SPRxPT directly.
extern "C" { volatile uint32_t g_wideExtAddr[3] = { 0, 0, 0 }; }
// Cockpit bitmap base (set in initialize) — lets amiga/b2_probe.gdb read the dashboard PEN under
// the energy-gauge column, which decides whether sprite priority can hide the bar's overflow at
// all (pen 0 = COLOR00 background, which every sprite beats regardless of BPLCON2).
extern "C" { volatile uint32_t g_cockpitBmpAddr=0; }
// Wide-object histograms: how often each SIZEPn scale was actually rendered (index 0/1/2 = 1×/2×/4×)
// and the widest burst seen.  A headless COMBAT run must show non-zero 2×/4× buckets, or the
// widening path never ran and a visual check would prove nothing.
extern "C" { volatile unsigned long g_wideShotScale[3] = { 0, 0, 0 }; }
extern "C" { volatile unsigned long g_wideP3Scale[3]   = { 0, 0, 0 }; }
extern "C" { volatile unsigned long g_wideMaxRows = 0, g_wideDenied = 0; }
// Burst releases that found pixels still live in the DISPLAYED extension chain — i.e. the 2×→1×
// (or 4×→1×) step, where the wide segment 0 is still on screen because it is double buffered.
// Each is one frame the old immediate both-chain blank showed with its right half missing.
extern "C" { volatile unsigned long g_wideLateBlank = 0; }
#endif

// g_quitJmp: the __builtin_setjmp buffer armed by RescueOnFractalus::run() so we can
// unwind the never-returning transpiled chain on quit (5 words per the GCC builtin;
// initializer forces the definition).  extern "C" — RescueOnFractalus.cpp references it.
extern "C" { void* g_quitJmp[5] = { 0, 0, 0, 0, 0 }; }

// g_restartJmp: the __builtin_setjmp buffer for the BREAK/Restart path (game_loop_reset).  The
// Atari trampoline ($52BE) restarts via a 6502 RTS stack trick that C control flow can't
// reproduce (and it runs in the VBI ISR, where longjmp is unsafe) — but it DOES leave its
// observable side-effect in mem[]: VVBLKI ($0222/3) = $52B4.  renderFrame/pollEvents (main-loop
// context) detect that and longjmp here; run() then re-enters game_main_loop with the faithful
// $3D1F init (which preserves the high score $0605-0608, unlike a full game_entry re-run).
extern "C" { void* g_restartJmp[5] = { 0, 0, 0, 0, 0 }; }
// The trampoline's persistent Amiga side-effect — VVBLKI left at this value = "restart requested".
static const uint16_t kRestartVvblki = 0x52B4u;

// Pending in-flight command keycode set by the keyboard ISR (keyboardHandler, below), consumed by
// the flight VBI through flightIrqKey().  $FF = none.  Volatile: written in the SP interrupt, read on
// the main thread (mirrors the Atari's X-register handoff out of the IRQ).  Declared here (ahead of
// flightIrqKey and the keyboard section) so rof_check_restart can see it for the out-of-flight BREAK.
static volatile uint8_t s_pendingFlightKey = 0xFF;

// rof_check_restart: the single pump-exit gate — quit / BREAK-restart.  Called from renderFrame +
// pollEvents AND from the flight render path (renderFlightDirect), because the flight loop busy-waits
// in renderFlightDirect and never reaches renderFrame — so a BREAK pressed mid-flight (VVBLKI left at
// $52B4 by the trampoline) would otherwise never be seen and the viewport would stay stuck (the
// black+brown state).  Runs in the MAIN loop (never the ISR), so the __builtin_longjmp back to run()'s
// setjmp buffers is safe.
extern "C" void rof_check_restart(void)
{
#ifdef ROF_FORCE_QUIT
    // `make FORCE_QUIT=<vbl>` + amiga/exit_restore.gdb: press the quit button from C at the given
    // vertical blank.  The system-restore tail of PlatformAmiga::run() is otherwise unreachable
    // headlessly — the only trigger is a physical mouse click, and the gdb stub drops memory
    // writes, so poking g_pumpQuit from a script silently does nothing (docs/headless-fsuae.md).
    // Goes through g_pumpQuit exactly as the mouse does, so nothing about the shutdown differs.
    if (g_vbiCount >= (uint16_t)ROF_FORCE_QUIT) g_pumpQuit = 1;
#endif
    if (AmigaHardware::isLeftMouseButtonPressed()) g_pumpQuit = 1;
    if (g_pumpQuit) __builtin_longjmp(g_quitJmp, 1);
    // BREAK (Help = Atari BREAK $80) OUTSIDE flight: only the flight VBI's $519c CLI window
    // consumes s_pendingFlightKey and runs game_loop_reset (the trampoline that leaves VVBLKI=$52B4).
    // In Standby / the doors-tunnel-stars launch cinematic nothing consumes it, so BREAK would do
    // nothing there — yet the Atari processes BREAK from its keyboard IRQ in ANY scene.  Model that
    // here: seeing a pending BREAK while NOT in the flight VBI ($4FF5), leave the same $52B4 marker
    // the trampoline would, so the restart longjmp below fires from any scene.  During flight leave it
    // for the dispatcher (the already-working trampoline path).
    if (s_pendingFlightKey == 0x80u && (uint16_t)(mem[0x0222] | (mem[0x0223] << 8)) != 0x4FF5u) {
        s_pendingFlightKey = 0xFF;
        mem[0x0222] = (uint8_t)(kRestartVvblki & 0xFFu);
        mem[0x0223] = (uint8_t)(kRestartVvblki >> 8);
    }
    if ((uint16_t)(mem[0x0222] | (mem[0x0223] << 8)) == kRestartVvblki) __builtin_longjmp(g_restartJmp, 1);
}

// renderFrame: called from the transpiled frame-wait hooks (platform_render_frame).
// Render first so the display reflects the state the spin-wait just advanced, then
// wait for the next real VBI (or return immediately if one already fired during
// rendering).  After the wait, advance RTCLOK by exactly one — owned here rather
// than in the ISR so the equality spin (wait_frames_set_count $3CB2) always gets one advance
// per iteration regardless of how long rendering takes.
// Exception: ATTRACT VBI ($1B30) bumps RTCLOK in its own transpiled body; skip here.
void PlatformAmiga::renderFrame() {
#ifdef ROF_FLIGHT_PROBE
    // Probe: track the largest gap (in real VBI frames) between successive renderFrame
    // calls — a long gap = a no-yield compute stretch where the display (and blink lights)
    // freeze.  Snapshot phase state at the max.
    {
        static uint16_t s_lastEntryVbi = 0;
        uint16_t nowVbi = g_vbiCount;
        uint16_t gap = (uint16_t)(nowVbi - s_lastEntryVbi);
        uint16_t vvblki = (uint16_t)(mem[0x0222] | (mem[0x0223] << 8));
        if (nowVbi > 360 && gap > g_maxRenderGap) {
            g_maxRenderGap   = gap;
            g_maxGapAtVbi    = nowVbi;
            g_maxGapVvblki   = vvblki;
            g_maxGap060B     = mem[0x060B];
            g_maxGap004A     = mem[0x004A];
        }
        // Cinematic-only gap: worst render freeze while the launch VBI ($52D7) is active
        // (i.e. NOT the flight VBI $4FF5) — pins a tunnel->stars-transition freeze that the
        // (larger) first-flight gap would otherwise overwrite in g_maxRenderGap.
        if (nowVbi > 360 && vvblki != 0x4FF5u && gap > g_maxCineGap) {
            g_maxCineGap    = gap;
            g_maxCineGapAtVbi = nowVbi;
            g_maxCineGap060B  = mem[0x060B];
        }
        // Tunnel->stars transition window only (the entry burst lands ~vbi 850-950): isolates
        // that specific freeze from later planet-scene gaps that now dominate g_maxCineGap.
        if (nowVbi > 820 && nowVbi < 1000 && vvblki != 0x4FF5u && gap > g_csGap) {
            g_csGap = gap; g_csGapAtVbi = nowVbi;
        }
        // standby->doors: whole launch-cinematic window BEFORE the tunnel->stars burst, NO
        // vbi>360 gate (the door sweep + stretch A land ~vbi 100-500).
        if (nowVbi > 150 && nowVbi < 300 && vvblki != 0x4FF5u && gap > g_doorGap) {
            extern volatile unsigned char g_saPhase, g_doorGapPhase;
            g_doorGap = gap; g_doorGapAtVbi = nowVbi; g_doorGap060B = mem[0x060B];
            g_doorGapPhase = g_saPhase;
        }
        s_lastEntryVbi = nowVbi;
    }
#endif
    uint16_t last = g_vbiCount;
#ifdef ROF_FLIGHT_PROBE
    uint8_t rtBefore = mem[0x0014];
    // Flight-only gate (matches the flight-gated sub-phases in RescueOnFractalus::renderFrame):
    // VVBLKI == $4FF5 is the flight VBI vector.
    const bool _rFlight = ((mem[0x0222] | (mem[0x0223] << 8)) == 0x4FF5u);
    unsigned long _rr0 = rof_subclock(), _rri = g_isrBeamLines;
#endif
    if (s_scene) s_scene->renderFrame();
#ifdef ROF_FLIGHT_PROBE
    { extern volatile unsigned long g_rbMax; extern volatile unsigned short g_rbMaxVbi;
      unsigned long _rbd = rof_subclock() - _rr0; uint16_t nv = g_vbiCount;
      if (nv > 150 && nv < 300 && _rbd > g_rbMax) { g_rbMax = _rbd; g_rbMaxVbi = nv; } }
    if (_rFlight) {
        unsigned long _w = rof_subclock() - _rr0;
        g_rRenderWall    += _w;
        g_rRenderCompute += _w - (g_isrBeamLines - _rri);
        g_rCalls++;
        if (mem[0x0632]) g_alTRScene += _w;   // knock: scene render (composite + flip wait)
    }
    unsigned long _ri0 = rof_subclock();
#endif
    // Dot side-buffer: when the flight normal path DEFERRED its flip (returned without busy-waiting for
    // the swap), SKIP this frame-sync vblank wait — that is the whole point of the scheme: the next
    // terrain compute must run DURING the pending flip's vblank, not after an idle wait for it.  The
    // deferred flip is drained at the top of the next renderFlightDirect (~0 by then).  Every other case
    // (non-flight scenes, the rescue-figure pause's immediate-flip path, the no-fresh-terrain early
    // return) leaves the flag clear and still paces to the display here.
    const bool _deferredFlip = s_scene && s_scene->consumeDeferredFlip();
#ifdef ROF_FLIGHT_PROBE
    const uint16_t _vSpin0 = g_vbiCount;
#endif
    if (!_deferredFlip) while (g_vbiCount == last) { /* wait for next real VBI */ }
#ifdef ROF_FLIGHT_PROBE
    if (_rFlight) g_idleFrames += (unsigned long)(uint16_t)(g_vbiCount - _vSpin0);
    if (_rFlight) { unsigned long _iw = rof_subclock() - _ri0; g_rIdleWall += _iw;
                    if (mem[0x0632]) g_alTRIdle += _iw; }   // knock: frame-sync wait
#endif
    // Flight double-buffer: the flip in render() has now latched (the just-painted buffer is on
    // screen).  Kick the blitter clear of the OTHER (now off-screen) buffer so it overlaps the
    // upcoming terrain draw instead of running serially inside the next convert.  No-op off flight.
    if (s_scene) s_scene->flightKickBackClear();
    // Noise-sample refill — MAIN-LOOP work (see PlatformAmiga::noiseTick).  Deliberately placed
    // straight after the blitter kick above so the 4-longword fill runs while the blitter is
    // clearing the back buffer, and deliberately NOT in the 50 Hz VBI ISR, where the same work was
    // charged to the vblank budget ~2.3x per rendered frame.
#ifdef ROF_FLIGHT_PROBE
    const unsigned short _n0 = rof_beam_line();
#endif
    noiseTick();
#ifdef ROF_FLIGHT_PROBE
    // Still accumulated into g_vbiNoiseLines and still divided by VBI FIRINGS by isr_full.gdb, so
    // the row stays directly comparable to the old in-ISR t/firing figure — the move shows up as
    // the drop it is, not as a units change.
    if (_rFlight) {
        const unsigned short _n1 = rof_beam_line();
        g_vbiNoiseLines += (_n1 >= _n0) ? (unsigned short)(_n1 - _n0)
                                        : (unsigned short)(_n1 + 313 - _n0); }
#endif
    uint16_t vbiVec = (uint16_t)(mem[0x0222] | (mem[0x0223] << 8));
#ifdef ROF_FLIGHT_PROBE
    // Did a VBI body advance RTCLOK during the wait?  (flight/attract bodies do; $52D7 doesn't)
    bool vbiAdvanced  = (mem[0x0014] != rtBefore);
    bool rfWillAdvance = (vbiVec != 0x1B30u && vbiVec != 0x4FF5u);
    if (vbiAdvanced && rfWillAdvance && g_vbiCount > 360) {     // DOUBLE: overshoots equality waits
        g_rtDoubleCount++; g_rtDoubleAtVbi = g_vbiCount; }
    if (!vbiAdvanced && !rfWillAdvance && g_vbiCount > 360) {   // ZERO: nobody advanced RTCLOK
        g_rtZeroCount++; g_rtZeroAtVbi = g_vbiCount; }
    if (g_vbiCount > 360 && vbiVec != 0x52D7u && vbiVec != 0x4FF5u &&
        vbiVec != 0x1B30u && vbiVec != 0x53CCu) {               // torn / unexpected vector read
        g_rtTornCount++; g_rtLastTornVec = vbiVec; g_rtTornAtVbi = g_vbiCount; }
    // SKIP detector: under $4FF5, renderFrame leaves RTCLOK to the ISR; measure how far it
    // moved across THIS spin iteration.  A delta >1 means the equality wait at $3CB8 can step
    // over its target and wrap 256 ticks (~5s) — the run-by-run variance.
    if (vbiVec == 0x4FF5u && g_vbiCount > 360) {
        uint8_t d = (uint8_t)(mem[0x0014] - rtBefore);
        if (d > g_rtJumpMax) { g_rtJumpMax = d; g_rtJumpAtVbi = g_vbiCount; }
        if (d > 1) g_rtJumpGt1Count++;
    }
#endif
    // RTCLOK ownership: the ATTRACT ($1B30), full flight ($4FF5) and boot-logo ($51EF) VBIs
    // advance RTCLOK ($0014) in their own bodies, so skip here to avoid double-counting.  The
    // standby/cinematic ($52D7) body does not, so renderFrame owns it there.
    // ⚠ $51EF is not optional: the logo's sparkle paces on wait_frames_2 ($3CCA), which zeroes
    // RTCLOK and waits for it to reach 2 — so double-counting ran the whole 32-frame fade at 2x
    // (measured: g_vbiCount advanced ONE per two-frame wait, amiga/logo_sparkle.gdb).
    if (vbiVec != 0x1B30u && vbiVec != 0x4FF5u && vbiVec != 0x51EFu) {
        mem[0x0014]++;
        if (!mem[0x0014]) mem[0x0013]++;
    }
    rof_check_restart();   // quit / BREAK-restart / SYSTEM-RESET (may __builtin_longjmp out)
#ifdef ROF_FLIGHT_PROBE
    g_pollAfterRender = true;   // next pollEvents starts a fresh VCOUNT-spin span measurement
#endif
}

// pollEvents: called from the non-frame spin-wait hooks (VCOUNT/CONSOL polls that don't
// pace a frame).  Poll the quit control so the player can always abort.
void PlatformAmiga::pollEvents() {
#ifdef ROF_FLIGHT_PROBE
    // VCOUNT/CONSOL busy-waits (wait_vcount_eq $3C75 etc.) spin here without pacing a frame.
    // Track how many REAL VBI frames a single such spin spans: an equality VCOUNT wait that
    // misses its exact target keeps spinning across frames (the "expected 1, got ~255" bug).
    if (g_pollAfterRender) { g_pollSpinStartVbi = g_vbiCount; g_pollAfterRender = false; }
    uint16_t span = (uint16_t)(g_vbiCount - g_pollSpinStartVbi);
    if (span > g_maxPollSpinFrames && g_vbiCount > 360) { g_maxPollSpinFrames = span; g_maxPollSpinAtVbi = g_vbiCount; }
#endif
    rof_check_restart();   // quit / BREAK-restart / SYSTEM-RESET (may __builtin_longjmp out)
}

// tickVBI: no-op — RTCLOK is advanced by renderFrame() after each VBI wait.
void PlatformAmiga::tickVBI() {}

// boot_standby_launch_driver is about to run draw_frame_pattern_seq for the FORWARD launch
// tunnel.  Claim tunnelBitmap for the forward direction and prime it to the field's background
// pen; the 43 rectangles + 3 guide columns then paint themselves through platform_tunnel_rect /
// platform_tunnel_columns as the draw emits them.  Nothing is decoded back out of $1000.
void PlatformAmiga::tunnelRingsBegin() {
    if (s_scene) s_scene->tunnelPaintBegin();
}

extern "C" void rof_cockpit_dial_dirty(unsigned short addr);
void PlatformAmiga::cockpitDirty(uint16_t addr, uint8_t nCells) {
    // The shared-code cockpit writer is the dial bars (draw_object_column); grow the dial
    // bounding box so render() re-decodes just that footprint (writer-driven, no full scan).
    for (uint8_t i = 0; i < nCells; i++) rof_cockpit_dial_dirty((uint16_t)(addr + i));
}

extern "C" void rof_title_screen_dirty(unsigned short addr, unsigned char nCells);
void PlatformAmiga::titleScreenDirty(uint16_t addr, uint8_t nCells) {
    // A span of Title Screen digit cells ($365B region) was written — the STARTING LEVEL
    // digit (level select) or LAST/HIGH SCORE digits (game-over).  Grow the value-cell dirty
    // range so the next renderFrame redraws just those cells (no full-screen flash).
    rof_title_screen_dirty(addr, nCells);
}

extern "C" volatile unsigned char g_compassDirty;
void PlatformAmiga::compassChanged() {
    // The compass heading cells $32E3-$32E6 were rewritten (housing or heading glyph).
    // Flag them so the next renderFrame re-decodes just those 4 mode-4 cells.
    g_compassDirty = 1;
}

extern "C" void rof_cockpit_lockon_dirty(unsigned char cellIdx);
void PlatformAmiga::lockonChanged(uint8_t cellIdx) {
    // ONE enemy lock-on indicator cell ($3491 + cellIdx) was rewritten by the native lock-on
    // animation twins (lock_on_indicator_tick / _step / _write_cell / lock_on_indicator_fill_cells in
    // rof_native.c, driven by both the standby and flight VBIs).  Flag just that cell so the next
    // renderFrame re-decodes it — this keeps the indicator randomly blinking through the planet
    // descent and into flight.  (The blink rewrites a single cell ~9x/second and the old
    // strip-wide flag re-decoded all 7 = ~1.2% of the whole flight frame; measured.)
    rof_cockpit_lockon_dirty(cellIdx);
}

extern "C" volatile int g_titleToRender;
void PlatformAmiga::titleChanged() {
    // copy_title_text_block_to_screen ($782A) just rewrote the banner text in $32B7-$32CA
    // (the SFX sequencer alternates the block via $0091).  Ask the next renderFrame to repaint
    // all 20 title cells straight from screen RAM (whichever of "RESCUE ON FRACTALUS!" / the
    // copyright line is now there).
    g_titleToRender = 20;
}


// rof_attract_poll_key: the keycode handoff for the OTHER CLI window — $539A, inside the
// $5398 console/attract poll that both standby-family VBI bodies run every frame (the standby
// cockpit + launch cinematic $52D7 at $533C, and the title/results card $53CC at $5400).
// Same one-shot semantics as flightIrqKey(), with ONE difference: a pending BREAK ($80) is
// LEFT pending.  On the Atari $5398 answers BREAK with `JMP $52BE` (the restart trampoline);
// here the restart has to be driven from MAIN-loop context by rof_check_restart (longjmp is
// unsafe in the ISR), and that gate reads s_pendingFlightKey itself — so consuming $80 here
// would swallow the restart.  Every other keycode IS consumed, which is the whole point:
// see vbi_attract_poll in rof_native_amiga.cpp.
extern "C" uint8_t rof_attract_poll_key(void) {
    uint8_t k = s_pendingFlightKey;
    if (k != 0x80u) s_pendingFlightKey = 0xFF;
    return k;
}

uint8_t PlatformAmiga::flightIrqKey() {
#ifdef ROF_FORCE_ALIEN_ABOARD
    { extern volatile unsigned short g_abKeyWin; g_abKeyWin++; }
#endif
    // Consume the keycode the keyboard ISR stashed (if any) and reset to "none".  The flight
    // VBI's CLI window ($519c) calls this once per frame; returning the code here is exactly
    // the Atari IRQ leaving KBCODE&$3F (or $80=BREAK) in X for event_sequence_dispatcher.
    uint8_t k = s_pendingFlightKey;
    s_pendingFlightKey = 0xFF;
    return k;
}

// ============================================================================
//  CIA-A serial-port keyboard — F1 -> Atari START switch (CONSOL $D01F)
// ============================================================================
// The Amiga keyboard shifts each keycode into CIA-A's serial data register, raising
// the CIA-A SP interrupt (CIAICRB_SP, via INTB_PORTS).  We hang a handler on that
// vector through ciaa.resource (the AddICRVector mechanism).  keyboard.device
// normally owns the vector, so we steal it (saving the
// previous) and restore it on shutdown, leaving the OS keyboard working afterwards.
//
// This handler IS the Atari console-switch hardware abstraction: it maps the Amiga
// function keys onto the CONSOL switches ($D01F / 53279), which read active-low in
// bits 0-2 (START/SELECT/OPTION); idle = $07, a switch down clears its bit.
//   F1 -> START  (bit0)  — what station_init's $1A0E exit poll tests ($D01F == $06)
//   F2 -> SELECT (bit1)  — Standby starting-level select
//   F3 -> OPTION (bit2)  — attract DEMO DROID
// SELECT/OPTION are read by the Standby driver's idle loop (boot_standby_launch_driver $5F1D:
// $D01F&$02 / &$04) and the level selector (standby_level_select_loop $5978).  Measured
// behaviour: from the INITIAL cockpit Standby, SELECT (or joystick-up — faithful)
// opens the separate level-selector card ($53CC); inside it joystick up/down cycles the starting
// level.  In the POST-mother-ship Standby ($003A==$FF), SELECT cycles the level in place (cockpit
// door-scroll).  All faithful to the transpiled binary.
// (BREAK/Restart = Help is handled below, not via CONSOL.  SYSTEM RESET is a hardware reset,
// not an application key — deliberately not mapped.)
static const uint16_t kConsol      = 0xD01F;
static const uint8_t  kConsolIdle  = 0x07;
static const uint8_t  kRawF1       = 0x50;   // F1 -> CONSOL START  (bit0)
static const uint8_t  kRawF2       = 0x51;   // F2 -> CONSOL SELECT (bit1)
static const uint8_t  kRawF3       = 0x52;   // F3 -> CONSOL OPTION (bit2)

// In-flight keyboard commands.  On the Atari these arrive as a POKEY keyboard/BREAK
// IRQ (IRQEN=$C0) whose handler (irq_handler $462A) leaves the event id — KBCODE&$3F,
// or $80 for BREAK — in X; the flight VBI's CLI window ($519c) then runs
// event_sequence_dispatcher ($4644), which matches X against tbl $4816 and dispatches
// by mode $0072.  We model that here: map the Amiga rawkey of each command key to its
// Atari KBCODE and stash it (one-shot) for the flight VBI to consume via flightIrqKey().
// The dispatcher itself filters unknown ids, so only the 8 table keys do anything.
//   Atari KBCODE -> command (event_sequence_dispatcher index Y), from the game manual:
//     $00 L (Y0) Land     $3f A (Y1) Air Lock    $15 B (Y2) Boosters    $3e S (Y3) Systems
//     $07 * (Y4) Increase Thrust      $06 + (Y5) Decrease Thrust
//     $80 BREAK ($519e -> game_loop_reset = "Restart")    $1c ESC (Y7) Freeze/pause
//   ($07/$06 are the Atari cursor-right/left keys, masked to base code by the IRQ's AND #$3F.
//    Thrust goes on the two keys immediately RIGHT OF '0' — Amiga '-' ($0b) and '=' ($0c) —
//    because on an Atari 800 keyboard those same two positions carry the '< -' / '> =' legends
//    the game uses for thrust down/up.  Raw keycodes, so '=' fires unshifted as well as as '+'.
//    The Amiga arrow keys stay free for the joystick directions.)
// ⚠ NOT only flight.  The SAME one-shot feeds the out-of-flight CLI window $539A
// (vbi_attract_poll, rof_native_amiga.cpp), which drops the code into $0049 — and $0049 is
// what the HIGH-SCORE INITIALS entry types from: render_text_cell $5CA7 waits on it and
// translates the KBCODE through the game's own table $5E50 into the screen cell.  So the map
// below is the whole Atari keyboard, not just the eight command keys; the flight dispatcher
// filters everything it does not know, and out of flight an unrecognised key is just an
// attract-timeout reset.  (It also feeds match_code_sequence $5B45's AUTHOR easter egg.)
struct AtariKeyMap { uint8_t rawkey; uint8_t kbcode; };
static const AtariKeyMap kAtariKeys[] = {
    // -- the command keys (all of them ordinary letters; the dispatcher matches the KBCODE) --
    { 0x28, 0x00 },   // Amiga 'L'   -> Atari L   $00  Land
    { 0x20, 0x3F },   // Amiga 'A'   -> Atari A   $3f  Air Lock
    { 0x35, 0x15 },   // Amiga 'B'   -> Atari B   $15  Boosters
    { 0x21, 0x3E },   // Amiga 'S'   -> Atari S   $3e  Systems
    { 0x0C, 0x07 },   // Amiga '='/'+' -> Atari * $07  Increase Thrust
    { 0x0B, 0x06 },   // Amiga '-'   -> Atari +   $06  Decrease Thrust
    { 0x45, 0x1C },   // Amiga 'Esc' -> Atari ESC $1c  Freeze/pause
    { 0x5F, 0x80 },   // Amiga 'Help' -> Atari BREAK $80 Restart
    // -- the rest of the alphabet, so initials can be typed --
    { 0x33, 0x12 },   // C
    { 0x22, 0x3A },   // D
    { 0x12, 0x2A },   // E
    { 0x23, 0x38 },   // F
    { 0x24, 0x3D },   // G
    { 0x25, 0x39 },   // H
    { 0x17, 0x0D },   // I
    { 0x26, 0x01 },   // J
    { 0x27, 0x05 },   // K
    { 0x37, 0x25 },   // M
    { 0x36, 0x23 },   // N
    { 0x18, 0x08 },   // O
    { 0x19, 0x0A },   // P
    { 0x10, 0x2F },   // Q
    { 0x13, 0x28 },   // R
    { 0x14, 0x2D },   // T
    { 0x16, 0x0B },   // U
    { 0x34, 0x10 },   // V
    { 0x11, 0x2E },   // W
    { 0x32, 0x16 },   // X
    { 0x15, 0x2B },   // Y
    { 0x31, 0x17 },   // Z
    // -- digits (the Atari's are scattered, hence the table) --
    { 0x0A, 0x32 },   // 0
    { 0x01, 0x1F },   // 1
    { 0x02, 0x1E },   // 2
    { 0x03, 0x1A },   // 3
    { 0x04, 0x18 },   // 4
    { 0x05, 0x1D },   // 5
    { 0x06, 0x1B },   // 6
    { 0x07, 0x33 },   // 7
    { 0x08, 0x35 },   // 8
    { 0x09, 0x30 },   // 9
    // -- editing + punctuation the entry accepts.  RETURN ends the name, BACKSPACE rubs one
    //    out ($5CC0 / $5CCA); the rest simply type.  Atari '-' ($0e) and '=' ($0f) are NOT
    //    here on purpose: those two Amiga keys are the thrust pair above (docs/controls.md).
    { 0x44, 0x0C },   // Return    -> Atari RETURN $0c  (end of name)
    { 0x43, 0x0C },   // keypad Enter, same
    { 0x41, 0x34 },   // Backspace -> Atari BACK S $34  (rub out)
    { 0x40, 0x21 },   // Space
    { 0x38, 0x20 },   // ,
    { 0x39, 0x22 },   // .
    { 0x3A, 0x26 },   // /
    { 0x29, 0x02 },   // ;
};

// Amiga rawkeys for the held joystick/fire inputs (driven into s_portaState/s_trig0State, which
// hwRead honours in FLIGHT only — see keyboardStickLive).
static const uint8_t kRawUp        = 0x4C;
static const uint8_t kRawDown      = 0x4D;
static const uint8_t kRawRight     = 0x4E;
static const uint8_t kRawLeft      = 0x4F;
static const uint8_t kRawFire      = 0x60;   // Left Shift = fire button (TRIG0)
// Both shift keys feed SKSTAT bit3 (see s_shiftMask).  kRawLShift aliases kRawFire on purpose:
// in flight that key is the trigger, at the level-selector card it is SHIFT, and the two readers
// never overlap.
static const uint8_t kRawLShift    = 0x60;
static const uint8_t kRawRShift    = 0x61;

// ---- Real Amiga joystick, port 1 ------------------------------------------------------------
// The manual's "plug your joystick into the second port" lands exactly right here: on the Amiga
// port 0 is the mouse and port 1 (the second connector) is the joystick, and port 1 is also what
// the 6502 binary polls — PORTA $D300 bits 0-3 + TRIG0 $D010.  ($D011/TRIG1 and PORTA bits 4-7 are
// never read anywhere in the game, so there is no second-stick path to feed.)
//
// Sampled once per vblank from vbiHandler: that is the rate the Atari's own VBI polled the stick
// at, and an edge-triggered button needs a fixed sampling rate (the main loop runs well under
// 50 Hz in flight and would drop short presses).  The cost is four register reads.
//
// Manual mapping, all of it faithful to what the binary already does with these bits:
//   directions -> the SAME PORTA bits the arrow keys drive (forward/back = dive/climb,
//                 left/right = bank), so stick and keyboard are indistinguishable downstream;
//   button 1   -> TRIG0 = Launch AMB Torpedo, and ALSO "start the game" before the game begins,
//                 free of charge: read_console_trig_delta $5A78 computes (CONSOL & $01) - TRIG0;
//   button 2   -> "the second fire button will land or launch the ship" = the L command key.
static volatile bool s_joyBtn2Prev = false;

static void pollJoystick()
{
    // JOY1DAT is a pair of QUADRATURE COUNTERS, not four direction bits: each axis' two switches
    // land on a (bit, bit^bit) pair (Hardware Reference Manual, "Reading the joystick").  Note the
    // crosswise layout — the Y counter carries forward+left, the X counter back+right.
    const uint16_t j = *joy1datPointer;
    const uint16_t y1 = (j >> 9) & 1u, y0 = (j >> 8) & 1u;
    const uint16_t x1 = (j >> 1) & 1u, x0 =  j       & 1u;
    uint8_t porta = 0xFFu;                          // active-low; $FF = centred
    if (y0 ^ y1) porta &= (uint8_t)~0x01u;          // forward -> bit 0 (what arrow-UP drives)
    if (x0 ^ x1) porta &= (uint8_t)~0x02u;          // back    -> bit 1 (arrow-DOWN)
    if (y1)      porta &= (uint8_t)~0x04u;          // left    -> bit 2 (arrow-LEFT)
    if (x1)      porta &= (uint8_t)~0x08u;          // right   -> bit 3 (arrow-RIGHT)
    s_joyPorta = porta;
    // Button 1: CIA-A PRA bit 7 = port 1 fire, active-low (bit 6 is port 0, which
    // AmigaHardware::isLeftMouseButtonPressed already uses for the mouse).
    s_joyTrig0 = (*ciaapraPointer & CIAF_GAMEPORT1) ? 0x01u : 0x00u;
    // Button 2: POTINP bit 14 (DATRY) = port 1 pin 9, active-low — the same technique
    // isRightMouseButtonPressed uses on port 0's bit 10, so no POTGO setup is introduced here.
    // ⚠ PRESS EDGE, not level: event_sequence_dispatcher takes a ONE-SHOT event id, so a held
    // button would re-issue Land every single frame.  Delivered through the same
    // s_pendingFlightKey path the L key uses (the $519c CLI window consumes it in flight; out of
    // flight the $5398 window consumes it harmlessly as an attract-timeout reset).
    const bool b2   = !(*potinpPointer & (1u << 14));
    const bool edge = b2 && !s_joyBtn2Prev;
    if (edge) s_pendingFlightKey = 0x00u;                   // Atari KBCODE L -> dispatcher Y0 = Land
    s_joyBtn2Prev = b2;
#ifdef ROF_FORCE_BOOT_FIRE
    // `make PROBES=1 SKIPBOOT=0 FORCE_BOOT_FIRE=1` + amiga/boot_fire.gdb: hold the fire button over
    // vbi 100..160 — mid-Logo (it runs ~280 frames) and on into the Station — so both boot-cinematic
    // skips are exercised with no physical joystick, then RELEASED so Standby is reached cleanly
    // rather than launched straight through.  Overrides the sampled level, so it reaches both
    // readers: the Logo's own TRIG0 check and the Station's fire-as-START fold in hwRead($D01F).
    // ⚠ Injected from C, not gdb.  A `set var` on this from the gdb script did NOT stick — the
    // FS-UAE stub serves memory reads but dropped the write (measured), which is why
    // every other FORCE_* harness in this file drives its input from C too.
    if (g_vbiCount >= 100u && g_vbiCount <= 160u) s_joyTrig0 = 0x00u;
#endif
#ifdef ROF_FLIGHT_PROBE
    // Stamp the two boot-cinematic transitions off the live VVBLKI, so the skips are MEASURED
    // rather than inferred from where a sample happened to land: the Station's vector appearing
    // means the Logo is done, and anything after it means the Station is done.
    { extern volatile unsigned short g_bfLogoEnd, g_bfStationEnd;
      const uint16_t vv = (uint16_t)(mem[0x0222] | (mem[0x0223] << 8));
      if (vv == 0x1B30u) { if (!g_bfLogoEnd) g_bfLogoEnd = (unsigned short)g_vbiCount; }
      else if (g_bfLogoEnd && !g_bfStationEnd && vv != 0x51EFu)
          g_bfStationEnd = (unsigned short)g_vbiCount; }
    // With NOTHING plugged into port 1 this must stay perfectly quiet, or it breaks keyboard play:
    // g_joyPortaStuck accumulates every direction bit ever seen low, g_joyB2Edges counts Land
    // injections, g_joyTrigLow counts frames with fire held.  All three should read 0 on an
    // untouched headless run.  g_joyRaw keeps the last JOY1DAT/POTINP words for eyeballing.
    { extern volatile unsigned char g_joyPortaStuck, g_joyPortaLast;
      extern volatile unsigned long g_joyB2Edges, g_joyTrigLow, g_joyPolls;
      extern volatile unsigned short g_joyRawJoy, g_joyRawPot;
      g_joyPolls++;
      g_joyPortaLast = porta;
      g_joyPortaStuck |= (unsigned char)(~porta & 0x0Fu);
      if (s_joyTrig0 == 0x00u) g_joyTrigLow++;
      if (edge) g_joyB2Edges++;
      g_joyRawJoy = j; g_joyRawPot = *potinpPointer; }
#endif
}

static struct Library*   s_ciaaBase    = 0;
static struct Interrupt  s_kbInterrupt;
static struct Interrupt* s_savedVector = 0;   // keyboard.device's vector, restored on exit

// CIA-A SP interrupt: a full keycode has shifted into the serial register.  ciaa.resource
// has already read+cleared the ICR before dispatching us, so we only touch the serial data
// register (read the code) and CRA (handshake).
#ifdef ROF_FLIGHT_PROBE
// Who is generating PORTS (level 2) interrupts?  The int_probe tap counted ~11/s during a
// headless flight with NOTHING pressed, at ~250us each — suspiciously close to this handler's
// own handshake spin.  g_kbCalls counts entries here; g_kbRing keeps the last 16 raw codes
// (bit7 = key-up) so the source is identifiable.  If g_kbCalls << the PORTS firing count, the
// interrupts are some other CIA-A source (timer A/B, TOD, FLG) and never reach us.
extern "C" { volatile unsigned long  g_kbCalls = 0; }
extern "C" { volatile unsigned char  g_kbRing[16] = {0}; }
extern "C" { volatile unsigned char  g_kbRingIdx = 0; }
#endif

static uint32_t keyboardHandler()
{
    uint8_t sdr = *ciaasdrPointer;
#ifdef ROF_FLIGHT_PROBE
    g_kbCalls++;
    g_kbRing[g_kbRingIdx & 15u] = sdr;
    g_kbRingIdx++;
#endif

    // Acknowledge: pulse SP to output mode (drives KDAT low) then back to input, so the
    // keyboard releases the next keycode.  HRM Appendix G (node G-2): "Software MUST pulse
    // the line low for 85 microseconds"; resync timeout is 143 ms.
    // ⚠ This busy-wait runs INSIDE the CIA-A keyboard ISR, so it steals the CPU from the main
    // thread every key event.  It was 1500 iters (~2 ms — ~23x the required 85 us): harmless for a
    // single keypress, but when a key is HELD (repeat) the ISR fires often enough that ~2 ms/event
    // preempts the main-loop for whole frames at a time.  That starved the post-mother-ship Standby
    // level-select spin (boot_standby_launch_driver `while ($008B != 0x3E)`), which relies on the
    // faithful Atari invariant that the main loop re-checks $008B every frame: the VBI kept
    // decrementing $008B past the 0x3E target into 0 (where the VBI stops touching it) → deadlock
    // (measured held/repeated F2 jammed the scroll).  ~200 iters (~270 us) keeps a
    // comfortable ~3x margin over the 85 us minimum while cutting the per-event ISR cost ~7x.
    *ciaacraPointer |= CIACRAF_SPMODE;
    for (volatile uint16_t d = 0; d < 200; d++) { /* >=85us KDAT-low handshake pulse */ }
    *ciaacraPointer &= (uint8_t)~CIACRAF_SPMODE;

    // Wire protocol (HRM Appendix G): the keycode is sent ROL'd one bit and KDAT is
    // active-low, so SDR holds ~(keycode ROL 1).  Recover by inverting then ROR 1.  Bit 7
    // of the result = key-up flag (0 = down).
    uint8_t code = (uint8_t)~sdr;
    code = (uint8_t)((code >> 1) | (code << 7));   // ROR 1
    uint8_t raw  = (uint8_t)(code & 0x7Fu);
    bool    down = (code & 0x80u) == 0u;


    // Drive the CONSOL console switches from their keys' down/up edges, so the register
    // continuously reflects each key's level — just like the real GTIA switches.  CONSOL is
    // active-low (bit clear = pressed).  Bitwise (not full assignment) so simultaneous presses
    // compose; mem[$D01F] mirror kept in sync for the Station-scene reader.
    switch (raw) {
        case kRawF1:      // START (bit0)
            if (down) s_consolState &= (uint8_t)~0x01u; else s_consolState |= 0x01u;
            mem[kConsol] = s_consolState;
            return 0;
        case kRawF2:      // SELECT (bit1)
            if (down) s_consolState &= (uint8_t)~0x02u; else s_consolState |= 0x02u;
            mem[kConsol] = s_consolState;
            return 0;
        case kRawF3:      // OPTION (bit2)
            if (down) s_consolState &= (uint8_t)~0x04u; else s_consolState |= 0x04u;
            mem[kConsol] = s_consolState;
            return 0;
        default:
            break;
    }

    // SHIFT level for SKSTAT bit3 — tracked before the joystick switch below and deliberately
    // NOT returning, because Left Shift is also the trigger and must reach that case too.
    if (raw == kRawLShift) { if (down) s_shiftMask |= 0x01u; else s_shiftMask &= (uint8_t)~0x01u; }
    if (raw == kRawRShift) { if (down) s_shiftMask |= 0x02u; else s_shiftMask &= (uint8_t)~0x02u; }

    // Held joystick/fire inputs — track the active-low PORTA/TRIG0 level across down/up
    // edges (pressed = clear the bit).  Arrows = stick-0 directions; Left Shift = fire.  Tracked
    // in every scene but only READ during flight (keyboardStickLive), so that a key held across a
    // scene boundary is released here rather than stranded active-low on the far side.
    switch (raw) {
        case kRawUp:    if (down) s_portaState &= (uint8_t)~0x01u; else s_portaState |= 0x01u; return 0;
        case kRawDown:  if (down) s_portaState &= (uint8_t)~0x02u; else s_portaState |= 0x02u; return 0;
        case kRawLeft:  if (down) s_portaState &= (uint8_t)~0x04u; else s_portaState |= 0x04u; return 0;
        case kRawRight: if (down) s_portaState &= (uint8_t)~0x08u; else s_portaState |= 0x08u; return 0;
        case kRawFire:  s_trig0State = down ? 0x00u : 0x01u; return 0;
        default: break;
    }

    // In-flight command keys: on the key-DOWN edge, stash the Atari KBCODE for the flight
    // VBI to pick up (mirrors the POKEY keyboard IRQ leaving the id in X).  One-shot — the
    // dispatcher consumes the last code, exactly as X holds the most recent KBCODE.
    if (down) {
        for (unsigned i = 0; i < sizeof(kAtariKeys) / sizeof(kAtariKeys[0]); i++)
            if (kAtariKeys[i].rawkey == raw) {
                s_pendingFlightKey = kAtariKeys[i].kbcode;
#ifdef ROF_FORCE_MOTHERSHIP
                // Debug: make the mother ship "present" the instant BOOSTERS (B, KBCODE $15) is
                // pressed, so you can test the return cinematic + post-mother-ship Standby without
                // rescuing the pilot quota.  The BOOSTERS handler (show_ace_or_message $493D) only
                // takes the return branch when mission flag $003A bit7 is set, and the return
                // cinematic gates on $003A==$FF (rsBoostReturn) — so force $003A=$FF (+ light the
                // mother-ship HUD indicator $0676) right here, one key edge, before the flight VBI's
                // $519c window dispatches this keycode.  Untouched otherwise, so normal flight is
                // unaffected until you actually press B.  (make FORCE_MOTHERSHIP=1)
                // ...and knock energy $062F down to half at the same edge: pressing B early
                // leaves it FULL, and a full bar exactly fills its dial, so nothing hangs below
                // the gauge and every gauge-vs-cockpit priority bug stays invisible.
                // $DC = full (bar-top index ($DC-$062F)>>2 = 0); $6E = index 27.
                if (kAtariKeys[i].kbcode == 0x15u) { mem[0x003Au] = 0xFFu; mem[0x0676u] = 0x01u;
                                                      mem[0x062Fu] = 0x6Eu; }
#endif
                break;
            }
    }
    return 0;
}

// ============================================================================
//  INTB_PORTS (level 2) vector takeover — OPT-IN, and measured NOT worth it
// ============================================================================
// `make PORTS_TAKEOVER=1`.  OFF by default.  Kept because the experiment is worth preserving:
//
// The idea was the VERTB takeover one level down.  Reaching the keyboard through
// ciaa.resource's ICR dispatcher means every level-2 interrupt walks exec's PORTS server chain,
// and that chain measures (amiga/int_probe.gdb, 60 s of flight) ~11 firings/s at ~250 us each =
// ~0.3% of all wall clock — with OUR handler running ZERO times.  All of it is ciaa.resource
// (~220 us) plus FS-UAE's "UAE fs" server (~90 us).
//
// Two measured reasons it is off:
//  1. "UAE fs" is the UAE Boot ROM's host-filesystem/clipboard trap server — an EMULATOR
//     artifact.  A bare A500 (floppy / WHDLoad) has no such server and no ~11 Hz CIA-A source,
//     so on the real target this reclaims ~nothing.
//  2. Taking the vector starves that trap server, and FS-UAE responds by RESETTING the machine:
//     with the takeover on the session reboots before flight every time (2/2 runs), while the
//     same tree with it off runs clean — which also breaks the headless harness, since
//     diag_run.sh boots from a host directory.
//
// If it is ever re-enabled: the handler does what ciaa.resource did for us — read CIA-A's ICR
// (that is what releases the CIA's IRQ line, so it MUST come before clearing Paula's INTREQ, or
// the still-asserted line re-latches the request at once), then dispatch the serial-port bit.
// Other CIA-A sources (timer A/B, TOD alarm, FLG) are swallowed for the duration.  keyboardInit()
// must still run first: its AddICRVector is what arms SP in the CIA's write-only ICR mask.
#ifdef ROF_PORTS_TAKEOVER
static struct Interrupt portsServer;
static struct IntVector s_savedPorts;
static bool             s_portsTaken = false;

static uint32_t portsHandler(void)
{
    const uint8_t icr = *ciaaicrPointer;                    // read = clear + release CIA-A IRQ
    *(volatile unsigned short*)0xDFF09Cu = (unsigned short)INTF_PORTS;   // then ack Paula
#ifdef ROF_FLIGHT_PROBE
    g_portsIrqCnt++;
#endif
    if (icr & CIAICRF_SP)
        keyboardHandler();
    return 0;
}

static void portsTakeover(void)
{
    portsServer.is_Node.ln_Type = NT_INTERRUPT;
    portsServer.is_Node.ln_Pri  = 127;
    portsServer.is_Node.ln_Name = (char*)"RoF PORTS";
    portsServer.is_Data = 0;
    portsServer.is_Code = (void(*)())portsHandler;
    struct IntVector* iv = &SysBase->IntVects[INTB_PORTS];
    Disable();
    s_savedPorts = *iv;
    iv->iv_Data  = 0;
    iv->iv_Code  = (void(*)())portsHandler;
    iv->iv_Node  = &portsServer.is_Node;
    Enable();
    s_portsTaken = true;
}

static void portsRestore(void)
{
    if (!s_portsTaken) return;
    Disable();
    SysBase->IntVects[INTB_PORTS] = s_savedPorts;
    Enable();
    s_portsTaken = false;
}
#endif  // ROF_PORTS_TAKEOVER

static bool keyboardInit()
{
    s_ciaaBase = (struct Library*)OpenResource((UBYTE*)CIAANAME);
    if (!s_ciaaBase) return false;

    s_consolState = kConsolIdle;  // power-on CONSOL state: no switch down (START up)
    mem[kConsol]  = kConsolIdle;

    s_kbInterrupt.is_Node.ln_Type = NT_INTERRUPT;
    s_kbInterrupt.is_Node.ln_Pri  = 0;
    s_kbInterrupt.is_Node.ln_Name = (char*)"RoF KB";
    s_kbInterrupt.is_Data = 0;
    s_kbInterrupt.is_Code = (void(*)())keyboardHandler;

    // AddICRVector returns NULL on success, or the already-installed (keyboard.device)
    // vector on conflict.  On conflict, steal it: remove theirs, install ours, remember
    // theirs to restore in shutdown.
    s_savedVector = AddICRVector(s_ciaaBase, CIAICRB_SP, &s_kbInterrupt);
    if (s_savedVector) {
        RemICRVector(s_ciaaBase, CIAICRB_SP, s_savedVector);
        AddICRVector(s_ciaaBase, CIAICRB_SP, &s_kbInterrupt);
    }
    return true;
}

static void keyboardShutdown()
{
    if (!s_ciaaBase) return;
    RemICRVector(s_ciaaBase, CIAICRB_SP, &s_kbInterrupt);
    if (s_savedVector) {
        AddICRVector(s_ciaaBase, CIAICRB_SP, s_savedVector);
        s_savedVector = 0;
    }
    s_ciaaBase = 0;
}

// ============================================================================
//  Real INTB_VERTB VBI handler — the per-frame VBI body + RTCLOK clock
// ============================================================================
// We do not hang this off exec's VERTB server CHAIN (AddIntServer) — we replace the
// whole VERTB IntVector, so ours is the only VERTB code that runs.  Why: measured with
// the g_irqLat* probe, the OS servers ahead of us in the chain (graphics.library pri 10,
// gameport.device, timer.device) delayed our handler to beam line ~12.7 on average
// (max line 20) = ~780 us of every 20 ms frame = ~3.9% of ALL wall clock, spent on
// bookkeeping this game does not use: graphics.library refreshes a view we replaced
// (LoadView(NULL) + our own copper), gameport.device generates mouse events we never
// read (the left-mouse quit polls CIA-A PRA directly), timer.device services the VBLANK
// timer unit nothing in the run window requests.  Taking the vector gets those cycles
// back AND puts our beam-critical work (flightVblankSwap's bitplane-pointer rewrite
// before the copper's line-85 fetch, starVblankUpdate's sprite control words before the
// sprite DMA fetch at ~line 25) at the very top of the vblank instead of in its tail.
//
// Safe because the takeover window (install → scene.run() → restore) contains no OS
// service that needs VERTB: the xex image is incbin-embedded so there is no disk I/O,
// and both WaitTOF() pairs (which DO need graphics.library's VERTB server) are outside
// it.  The original IntVector is saved verbatim and put back on the way out.
// `make VERTB_SERVER=1` falls back to the old AddIntServer chain for A/B testing.
static struct Interrupt vbiServer;
// DMACONR/INTENAR as the OS had them at takeover; both are restored verbatim on the way out.
// Read BEFORE the first write to either (see the takeover block in run()), so the handover gives
// the OS back the set it actually had rather than a hardcoded guess at it.
static uint16_t         s_savedDmacon = 0;
static uint16_t         s_savedIntena = 0;
#ifndef ROF_VERTB_SERVER
static struct IntVector s_savedVertb;      // exec's original VERTB IntVector, restored on exit
static bool             s_vertbTaken = false;
// exec puts IntVects[] at ExecBase+84, so VERTB (bit 5) is ExecBase+144 — which is exactly
// the offset Kickstart's level-3 autovector stub dispatches through (`movem.l 144(a6),a1/a5`).
static_assert(__builtin_offsetof(struct ExecBase, IntVects) == 84,
              "ExecBase::IntVects moved — re-check the VERTB vector takeover");
#endif

// Real vertical-blank frame counter (50 Hz PAL), exposed to the scene so time-based
// animations (e.g. the flight terrain colour fade) run at wall-clock rate regardless of
// how fast the main render loop iterates.
extern "C" unsigned short platform_frame_count(void) { return g_vbiCount; }

static uint32_t vbiHandler()
{
    // Clearing the interrupt request is the HANDLER's job — exec's server-chain walker
    // used to do it for us, and we replaced it (see the vbiServer comment above).  Miss
    // this and level 3 re-triggers forever.  No SETCLR bit = clear.  Harmless in the
    // VERTB_SERVER fallback build, where exec clears it as well.
    *(volatile unsigned short*)0xDFF09Cu = (unsigned short)INTF_VERTB;

    // The copper re-read COP1LC at the top of this vertical blank, so the buffer last handed to
    // setCopperList is now the one it is EXECUTING.  Latch that here — showTunnelCopper() needs it
    // to pick a back buffer, and it cannot use its own "last published" index: render() runs more
    // than once per displayed frame (each spin-wait hook drives one), and the second publish would
    // otherwise rewrite the live list's WAITs and bitplane pointers under the beam.
    { extern volatile unsigned char g_tunLiveIdx, g_tunPubIdx; g_tunLiveIdx = g_tunPubIdx; }

#ifdef ROF_FLIGHT_PROBE
    // FIRST statement in the server: how far has the beam travelled since VERTB was
    // raised at line 0?  That delta IS exec's interrupt-dispatch overhead (see the
    // g_irqLat* comment above).  Read VHPOSR once; V8 is ignored (the latency is only
    // ever a handful of lines, so the low 8 bits of vpos never wrap).
    {
        extern volatile unsigned long g_irqLatVsum, g_irqLatHsum, g_irqLatCnt;
        extern volatile unsigned short g_irqLatVmax, g_irqLatHmax, g_irqIntena, g_irqIntreq;
        const volatile unsigned short* _cst = (const volatile unsigned short*)0xDFF000u;
        unsigned short _vh = _cst[0x006u / 2];          // VHPOSR: hi = V7..V0, lo = HPOS
        unsigned short _vp = (unsigned short)(_vh >> 8), _hp = (unsigned short)(_vh & 0xFFu);
        g_irqLatVsum += _vp; g_irqLatHsum += _hp; g_irqLatCnt++;
        if (_vp > g_irqLatVmax || (_vp == g_irqLatVmax && _hp > g_irqLatHmax))
            { g_irqLatVmax = _vp; g_irqLatHmax = _hp; }
        g_irqIntena = _cst[0x01Cu / 2];                 // INTENAR
        g_irqIntreq = _cst[0x01Eu / 2];                 // INTREQR
    }
    // ...and stamp the entry beam line for the WHOLE-ISR cost probe (paired at the return below).
    // An ISR longer than a PAL frame (312 lines) means the next VERTB is already pending when we
    // return, so one displayed frame is lost — which is what made the level-select elevator scroll
    // stall for a frame and then jump two pixels.
    {
        extern volatile unsigned short g_isrEntryLine;
        unsigned short _vp2 = *(const volatile unsigned short*)0xDFF004u;
        unsigned short _vh2 = *(const volatile unsigned short*)0xDFF006u;
        g_isrEntryLine = (unsigned short)(((_vp2 & 1u) << 8) | (_vh2 >> 8));
    }
#endif
    // RTCLOK is owned by renderFrame() in the main thread (advanced exactly once per
    // spin-wait iteration, immune to ISR timing races with the equality spin).
    // Exception: ATTRACT VBI ($1B30) bumps RTCLOK in its own transpiled body.
    // (Do NOT touch $0080 — sync_flag, reused as the $80/$81 zp pointer.)
    g_vbiCount++;

#ifdef ROF_AUDIO_TRACE
    // One row per frame of the live Paula state, from the launch onward (window 0 = 1st launch).
    if (g_atStart[0]) aud_state_snapshot();
#endif

    // BREAK/Restart flash blank — do it here, at vblank (beam parked at top), so a COPJMP to the black
    // EmptyCopperList is safe (its sprite MOVEs at the top of the list run before the beam reaches the
    // sprites; a mid-frame COPJMP would smear them).  It blanks on ONE condition: VVBLKI==$52B4, i.e.
    // the trampoline has ARMED the restart and the main loop hasn't reached the rof_check_restart
    // longjmp yet — which persists for several vblanks (a flight compute iteration spans ~4 frames),
    // and across them the old FlightCopperList stays live and keeps swapping buffers: the stale /
    // mid-swap "brown rectangle".
    //
    // ⚠ It must NOT blank merely because a BREAK ($80) is pending in flight.  A pending key is not an
    // armed restart: the ONLY thing that consumes it in flight is the flight VBI's $519c CLI window,
    // and that window is reached only from the RESET/BLINK/KEYWIN arms of the $4FF5 dispatch — the
    // JOIN arm skips it.  During the BOOSTERS ascent ($0072 = 2) the dispatch takes JOIN every single
    // frame (measured: 0 window calls in 868 frames), so the key stays pending for the whole climb.
    // Blanking on it turned that into a permanently black screen with the simulation running
    // underneath.  The Atari behaves the same way — its $462A keyboard IRQ only leaves the code in X,
    // so a BREAK pressed there is likewise inert until the launch cinematic's own $539A window picks
    // it up — it just doesn't blank anything meanwhile.  Cost of waiting for $52B4: one frame of live
    // flight between the keypress and the trampoline arming the restart.
    {
        const uint16_t _vv = (uint16_t)(mem[0x0222] | (mem[0x0223] << 8));
        if (s_scene && _vv == 0x52B4u) s_scene->blankForRestart();
    }

    // Flight terrain double-buffer swap — do this FIRST, while the beam is still in vertical
    // blank (well above the viewport WAIT at scanline 85).  If renderFlightDirect has published a
    // freshly-painted buffer, rewrite the copper's viewport bitplane pointers now so the copper
    // fetches the new buffer this frame with no torn pointer (the brown-flash cause).  No-op unless
    // a swap is pending; clears the flag so the main thread's busy-wait can proceed.
    // Zero-copy starfield scroll — advance the ring window, re-point the copper SPRxPT operands,
    // write the fresh star-sprite control words and convert the new rows, all at the very TOP of
    // the vblank ISR: the operand has to be final before the copper reaches the list's sprite-
    // pointer MOVEs at scanline 16, and the control words before the sprite's control DMA fetch at
    // ~line 25 — otherwise a stale read corrupts VSTOP and drops the stars AND the channel-2
    // gauge.  ⚠ Must stay ahead of game_vbi_isr below (it bumps g_starScrollGen).  No-op unless
    // we're in the stars/planet phase.  (flightVblankSwap is a no-op during stars and vice-versa,
    // so ordering between them is free.)
    if (s_scene) s_scene->starVblankUpdate();

    // Boot scene 2 (the station cinematic): move the tall GTIA-9 field's four bitplane pointers to
    // the display list's current window row — the whole scroll.  Same reason it lives at the top of
    // the vblank ISR as the two above: the copper reads BPLxPT for the first display line, so a
    // pointer written from the main loop can be torn and garbage the entire frame.  No-op unless
    // the station copper is live.
    if (s_scene) s_scene->stationVblankUpdate();

    // Boot scene 1 (the Lucasfilm logo): its one PMG element, the sparkle.  Nothing to scroll —
    // the logo's display list never moves — but the shape is eaten away and the colour faded every
    // two frames, and animating PMG must be mirrored from the VBI like the station's.  No-op
    // unless the logo copper is live.
    if (s_scene) s_scene->logoVblankUpdate();

    if (s_scene) s_scene->flightVblankSwap();

    // Level-select "elevator" door scroll (post-mother-ship SELECT): repoint the standby terrain
    // BPLxPT from dl_src_index ($008B) each vblank while the scroll spins.  No-op unless active.
    if (s_scene) s_scene->doorScrollVblankUpdate();

#ifdef ROF_FLIGHT_PROBE
    // Probe: track the range of the atmosphere terrain pens ($00DC/$00DD) during flight to
    // confirm they ramp (salmon→brown fade) vs stay frozen.
    {
        extern volatile unsigned char g_dcMin, g_dcMax, g_ddMin, g_ddMax;
        if ((mem[0x0222] | (mem[0x0223] << 8)) == 0x4FF5u) {
            uint8_t dc = mem[0x00DC], dd = mem[0x00DD];
            if (dc < g_dcMin) g_dcMin = dc;
            if (dc > g_dcMax) g_dcMax = dc;
            if (dd < g_ddMin) g_ddMin = dd;
            if (dd > g_ddMax) g_ddMax = dd;
        }
    }

#endif  // ROF_FLIGHT_PROBE — probe-only taps above; the auto-fire + auto-launch below are also
        // wanted by the near-clean ROF_FPSCOUNT build, which has none of those probe globals.

#ifdef ROF_AUTO_FIRE
    // Auto-fire: once the flight VBI ($4FF5) is live, HOLD the trigger ($D010=0, active-low) so
    // the player laser fires continuously.  A held trigger auto-repeats (the $5178 fire path
    // re-arms whenever $0036 returns to 0), so snapshotting mem[] at varying delays catches every
    // phase of the shot animation (travel scale/pos + impact).
    // ⚠ This used to live inside the ROF_FLIGHT_PROBE block above, which silently compiled it out
    // of an FPSCOUNT build — i.e. `make FPSCOUNT=1 AUTO_FIRE=1` did not actually fire.  It is
    // outside now so the honest-framerate build can measure a COMBAT=1 run.
    if ((mem[0x0222] | (mem[0x0223] << 8)) == 0x4FF5u) s_trig0State = 0x00u;
#endif


#if defined(ROF_FLIGHT_PROBE) || defined(ROF_FPSCOUNT) || defined(ROF_NAME_ENTRY)
    // Auto-launch: replicate an F1/START press once Standby's idle loop is actually
    // polling CONSOL.
    //
    // ROF_NAME_ENTRY is in the list because that build exists to REACH THE HIGH-SCORE SCREEN and
    // nothing else: it already arranges the death and the qualifying score, so leaving the launch
    // to a manual F1 would be the one hand-operated step in an otherwise hands-off path.  It is
    // the only one of the three that is not a measurement build, and it needs the auto-launch
    // WITHOUT the probe machinery (which types the initials for you and then quits).  A fixed vbi==350 fired before boot_standby_launch_driver's standby poll was live
    // (g_standbyRevealReady latches at boot_standby_launch_driver entry, when the idle loop starts), so the
    // press was never seen.  Gate on the reveal latch + a settle delay, and HOLD START for a
    // wide window so the once-per-frame poll catches it.
    {
        extern volatile unsigned char g_standbyRevealReady;
        static uint16_t s_revealVbi = 0;
        if (g_standbyRevealReady && s_revealVbi == 0) s_revealVbi = g_vbiCount;
        if (s_revealVbi) {
            uint16_t d = (uint16_t)(g_vbiCount - s_revealVbi);
#ifdef ROF_FORCE_TITLE
            // Visual test path: instead of launching, inject the Title Screen's screen RAM
            // ($365B, 6 rows × 20 chars, captured from title.a8s) + its COLPF0-3 palette
            // ($02C4-7) and pin rsTitle (g_forceTitleScreen).  The charset ($0400) is already
            // present at runtime (decodeCompass reads it).  Re-injected each frame so the
            // still-running standby loop can't clobber it.  Lets us see the render + tune the
            // vertical layout before the real attract-mode entry is wired.
            extern volatile unsigned char g_forceTitleScreen;
            if (d >= 60) {
                static const unsigned char kTitleRAM[120] = {
                    0x72,0x65,0x73,0x63,0x75,0x65,0x40,0x6f,0x6e,0x40,0x66,0x72,0x61,0x63,0x74,0x61,0x6c,0x75,0x73,0x41,
                    0x20,0x11,0x19,0x18,0x15,0x00,0x2c,0x35,0x23,0x21,0x33,0x26,0x29,0x2c,0x2d,0x00,0x2c,0x34,0x24,0x0e,
                    0x80,0xb3,0xb4,0xa1,0xb2,0xb4,0xa9,0xae,0xa7,0x80,0xac,0xa5,0xb6,0xa5,0xac,0x9a,0x00,0x00,0x54,0x00,
                    0x80,0x80,0xb2,0xa1,0xae,0xab,0xa9,0xae,0xa7,0x80,0xac,0xa5,0xb6,0xa5,0xac,0x9a,0x00,0x00,0x00,0x00,
                    0xc0,0xec,0xe1,0xf3,0xf4,0xc0,0xf3,0xe3,0xef,0xf2,0xe5,0xda,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x50,
                    0xc0,0xe8,0xe9,0xe7,0xe8,0xc0,0xf3,0xe3,0xef,0xf2,0xe5,0xda,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x50,
                };
                for (int i = 0; i < 120; i++) mem[0x365Bu + i] = kTitleRAM[i];
                mem[0x02C4u] = 0x38; mem[0x02C5u] = 0x2a; mem[0x02C6u] = 0x46; mem[0x02C7u] = 0x56;
                g_forceTitleScreen = 1;
            }
#else
            // Hold START (bit0 clear) until the launch cinematic actually engages
            // ($060B==$23), then release.  A fixed [60,120) window was timing-flaky under
            // the slow headless gdb-stub emulation: the standby idle loop polls CONSOL only
            // once per main-loop iteration, and with heavy per-frame render work few polls
            // land inside a 60-frame window, so the press was often missed.  Holding until
            // 060B latches makes the catch race-free regardless of emulation speed.
#ifndef ROF_NO_AUTOLAUNCH
            if (d >= 60) {
                s_consolState = (mem[0x060Bu] != 0x23u) ? 0x06u : 0x07u;  // hwRead source
                if (mem[0x060Bu] != 0x23u) mem[0xD01Fu] = 0x06;   // START held until launched
                else                       mem[0xD01Fu] = 0x07;   // launched → release
            }
#ifdef ROF_AUDIO_TRACE
            if (d >= 60 && mem[0x060Bu] != 0x23u) rof_audio_trace_arm(0);   // window 0 = 1st launch
#endif
#else
            (void)d;   // manual-launch measurement: the player presses START themselves
#endif
#endif
        }
    }

#ifdef ROF_FORCE_SELECT
    // Headless SELECT verification: in the INITIAL Standby (VVBLKI $52D7, no mother ship
    // $003A==0), replicate a SELECT press (F2) (CONSOL bit1 clear) once the idle loop is
    // polling, to confirm the wired SELECT drives the transition to the SEPARATE level-
    // selector card (VVBLKI $53CC, $365B=$72).  Build: make PROBES=1 FORCE_SELECT=1 NO_AUTOLAUNCH=1.
    {
        extern volatile unsigned char g_standbyRevealReady;
        static uint16_t s_selRevealVbi = 0;
        if (g_standbyRevealReady && s_selRevealVbi == 0) s_selRevealVbi = g_vbiCount;
        if (s_selRevealVbi) {
            uint16_t d2 = (uint16_t)(g_vbiCount - s_selRevealVbi);
            const uint16_t vv = (uint16_t)(mem[0x0222u] | (mem[0x0223u] << 8));
            // Hold SELECT (bit1 clear) while still in the cockpit standby; release once the
            // selector card is up so its own up/down poll isn't jammed.
            if (d2 >= 60 && vv == 0x52D7u) s_consolState &= (uint8_t)~0x02u;
            else                           s_consolState |= 0x02u;
            // Once the selector card is up ($53CC), pulse joystick-UP (PORTA bit0) every
            // ~40 frames to confirm the in-selector up/down toggle re-renders the STARTING
            // LEVEL digit (level_stage $006D + $3694/5).
            // Drives s_probePorta, NOT the keyboard's s_portaState: that one is gated to the
            // flight VBI (keyboardStickLive), and the card this probe tests is not flight.
            if (vv == 0x53CCu) {
                uint16_t ph = (uint16_t)(d2 % 40u);
                if (ph < 8u) s_probePorta &= (uint8_t)~0x01u;   // up pressed
                else         s_probePorta |= 0x01u;             // released
            } else {
                s_probePorta |= 0x01u;
            }
        }
    }
#endif
#endif  // ROF_FLIGHT_PROBE || ROF_FPSCOUNT (auto-launch)

#ifdef ROF_ATTRACT_NOW
    // `make ATTRACT_NOW=1|2` — force the Standby attract timeout early so the Title Screen
    // (scene 3b) is reachable in seconds instead of ~2.5 minutes of idling.
    //
    // The genuine path: the standby VBI ($52D7) INCs the sub-counter $062D every frame and bumps
    // attract_timer $00E2 on each 256-wrap; boot_standby_launch_driver's idle loop seeds $00E2=$64
    // at L_62ee and tests bit7 at L_6309, so the card needs 28 wraps = 7168 frames.  Setting bit7
    // ourselves takes exactly the same branch (L_6309 -> L_6311 -> game_main_loop), so what the
    // Title sees is the real timeout, just sooner.
    //
    // ONE-SHOT on purpose: $00E2 is re-seeded to $64 only at idle-loop ENTRY, so a repeating poke
    // would send the Standby that FOLLOWS the Title straight back to the Title.
    {
        const uint16_t vv = (uint16_t)(mem[0x0222u] | (mem[0x0223u] << 8));
        extern volatile unsigned char g_doorFieldReady;
        static uint16_t s_anVbi = 0; static uint8_t s_anDone = 0;
        const bool inStandby = (vv == 0x52D7u) && g_doorFieldReady
#if ROF_ATTRACT_NOW >= 2
                               && (mem[0x003Au] == 0xFFu)   // POST-MOTHER-SHIP standby only
#endif
                               ;
        if (!s_anDone) {
            if (!inStandby) s_anVbi = 0;                       // wait for a settled standby
            else if (s_anVbi == 0) s_anVbi = g_vbiCount;
            else if ((uint16_t)(g_vbiCount - s_anVbi) >= 150) { mem[0x00E2u] |= 0x80u; s_anDone = 1; }
        }
    }
#endif

#ifdef ROF_TITLE_START
    // `make TITLE_START=1` — press START on the Title Screen, headlessly.  Once the card has been
    // up ~2.4 s, hold CONSOL bit0: read_console_trig_delta ($5A78, (CONSOL&1)-TRIG0) then returns
    // non-zero at the title loop's L_5a09 poll, which exits standby_scoreboard_render back into
    // game_main_loop, and again at the following Standby's L_634a poll, which launches.  Released
    // on reaching flight so nothing in-flight sees a stuck console switch.
    {
        const uint16_t vv = (uint16_t)(mem[0x0222u] | (mem[0x0223u] << 8));
        const bool titleUp = (vv == 0x53CCu) && (mem[0x365Bu] == 0x72u) && (mem[0x022Fu] != 0);
        static uint16_t s_tsVbi = 0; static uint8_t s_tsHold = 0;
        if (titleUp && s_tsVbi == 0) s_tsVbi = g_vbiCount;
        if (s_tsVbi && !s_tsHold && (uint16_t)(g_vbiCount - s_tsVbi) >= 120) s_tsHold = 1;
        if (s_tsHold) {
            if (vv == 0x4FF5u) s_consolState |= 0x01u;                 // in flight: release START
            else               s_consolState &= (uint8_t)~0x01u;       // otherwise hold it down
            mem[0xD01Fu] = s_consolState;
        }
#ifdef ROF_FLIGHT_PROBE
        // Scene-routing trace for the title->launch transition (the mothership-title-launch bug).
        // RECORD-ON-CHANGE from the frame the Title appears: append a row only when the routing
        // state (VVBLKI / live copper / the gate flags / whether $008D,$008E are non-zero) differs
        // from the last one recorded.  A per-frame ring cannot span this — the Title hold plus the
        // Standby rebuild plus the whole launch cinematic is ~1000 frames — and every frame in
        // between is a repeat.  Dumped by amiga/title_start.gdb.
        {
            extern volatile unsigned short g_tsRingVbi[96], g_tsRingVV[96], g_tsRingN;
            extern volatile unsigned char g_tsRing3A[96], g_tsRing8D[96], g_tsRing8E[96],
                                          g_tsRing60B[96], g_tsRingCop[96], g_tsRingFlg[96],
                                          g_tsRing62F[96];
            extern volatile unsigned short g_tsRingSprY[96], g_energySprY;
            extern volatile unsigned char g_boostRet, g_boostVp, g_liveCopper,
                                          g_standbyRevealReady, g_doorFieldReady;
            // Record from the FIRST Standby reveal, not from the Title: the first-boot Standby is
            // the known-good reference this trace exists to diff the post-Title one against.
            if (g_standbyRevealReady && g_tsRingN < 96) {
                const unsigned char flg = (unsigned char)((g_doorFieldReady ? 1 : 0)
                                        | (g_standbyRevealReady ? 2 : 0)
                                        | (g_boostVp ? 4 : 0) | (g_boostRet ? 8 : 0)
                                        | (mem[0x008Du] ? 0x10 : 0) | (mem[0x008Eu] ? 0x20 : 0));
                const unsigned n = g_tsRingN;
                if (n == 0 || g_tsRingVV[n - 1] != vv || g_tsRingCop[n - 1] != g_liveCopper
                           || g_tsRingFlg[n - 1] != flg || g_tsRing60B[n - 1] != mem[0x060Bu]) {
                    // (62F/sprY are SAMPLED, never a record trigger — the standby's gauge fill
                    // ramps them every frame and would eat the whole ring.)
                    g_tsRingVbi[n] = g_vbiCount;      g_tsRingVV[n]  = vv;
                    g_tsRing3A[n]  = mem[0x003Au];    g_tsRing8D[n]  = mem[0x008Du];
                    g_tsRing8E[n]  = mem[0x008Eu];    g_tsRing60B[n] = mem[0x060Bu];
                    g_tsRingCop[n] = g_liveCopper;    g_tsRingFlg[n] = flg;
                    g_tsRing62F[n] = mem[0x062Fu];    g_tsRingSprY[n] = g_energySprY;
                    g_tsRingN = (unsigned short)(n + 1);
                }
            }
        }
#endif
    }
#endif

#ifdef ROF_FORCE_DEMO
    // Headless DEMO DROID + BREAK verification: in the initial Standby, hold OPTION (CONSOL bit2)
    // to launch the self-playing demo, then once the demo flight ($4FF5) has run a moment inject
    // BREAK ($80) and confirm the restart does NOT re-run the demo (VVBLKI should settle on the
    // card/standby, level_or_state $0004 cleared).  Build: PROBES=1 FORCE_DEMO=1.
    {
        extern volatile unsigned char g_standbyRevealReady;
        static uint16_t s_dRevealVbi = 0, s_dFlightVbi = 0; static uint8_t s_dPhase = 0;
        if (g_standbyRevealReady && s_dRevealVbi == 0) s_dRevealVbi = g_vbiCount;
        const uint16_t vv = (uint16_t)(mem[0x0222u] | (mem[0x0223u] << 8));
        if (s_dRevealVbi && s_dPhase == 0) {
            uint16_t d2 = (uint16_t)(g_vbiCount - s_dRevealVbi);
            if (d2 >= 60 && vv == 0x52D7u) s_consolState &= (uint8_t)~0x04u;   // OPTION down → DEMO DROID
            else { s_consolState |= 0x04u; if (vv == 0x4FF5u) s_dPhase = 1; }  // released once demo flight starts
            mem[0xD01Fu] = s_consolState;
        }
        if (s_dPhase == 1) {
            if (s_dFlightVbi == 0) s_dFlightVbi = g_vbiCount;
            if ((uint16_t)(g_vbiCount - s_dFlightVbi) >= 120) { s_pendingFlightKey = 0x80; s_dPhase = 2; }  // BREAK
        }
    }
#endif

#ifdef ROF_FORCE_DEATH
    // Headless death-cinematic verification: once the flight VBI ($4FF5) has been live for a
    // while, arm the energy-out cinematic exactly once (event_trigger $063D) so diag_run can
    // observe the flash→salmon→black→title sequence + g_flightBlank.  (make PROBES=1 FORCE_DEATH=1)
    {
        const uint16_t vv = (uint16_t)(mem[0x0222u] | (mem[0x0223u] << 8));
        static uint16_t s_flightVbi = 0; static uint8_t s_forcedDeath = 0;
        if (vv == 0x4FF5u && s_flightVbi == 0) s_flightVbi = g_vbiCount;
        if (s_flightVbi && !s_forcedDeath && (uint16_t)(g_vbiCount - s_flightVbi) >= 120) {
            mem[0x063Du] = 1; s_forcedDeath = 1;
        }
    }
#endif

#ifdef ROF_NAME_ENTRY
    // `make NAME_ENTRY=1` — reach the HIGH-SCORE INITIALS entry (`name_entry_loop $5B6C`), which
    // nothing else does: it is reachable only by DYING, and only after the game-over tune has run
    // out.  ~4 s after flight starts, arm the energy-out death cinematic (event_trigger $063D —
    // the same byte an empty energy bar writes) and seed a current score, so the results card has
    // digits to show and the (zero) high score is beaten.  From there the faithful binary does the
    // rest: the death teardown sets $00E5=5, standby_scoreboard_render copies the score over the
    // high score, spins at L_596d until the tune ends, then calls name_entry_loop.
    // Interactive (no PROBES needed): joystick/keys pick the letters, exactly as on the Atari.
    // Headless readout: amiga/name_entry.gdb.
    {
        const uint16_t vv = (uint16_t)(mem[0x0222u] | (mem[0x0223u] << 8));
        static uint16_t s_neFlightVbi = 0; static uint8_t s_neArmed = 0;
        if (vv == 0x4FF5u && s_neFlightVbi == 0) s_neFlightVbi = g_vbiCount;
        if (s_neFlightVbi && !s_neArmed && (uint16_t)(g_vbiCount - s_neFlightVbi) >= 240) {
            mem[0x0600u] = 0x00; mem[0x0601u] = 0x12;   // current score, BCD, MSB first
            mem[0x0602u] = 0x34; mem[0x0603u] = 0x50;
            // ⚠ NOTHING ELSE IS FAKED HERE, deliberately.  This used to also write the
            // save-state block ($3700=$28, $3714=$EE, the $7BDA signature at $37C7) by hand,
            // because both SIO reads that filled it are NOPs in rof.xex and validate_save_state
            // $5D0D therefore always failed.  The block is now supplied for real by
            // src/rof_hiscore.c through the restored $5D86/$5D9D calls, so seeding it here would
            // hide a broken loader behind a passing gate — the whole point of the probe split
            // (g_neEnter "called" vs g_nePass "past the gate") is to catch exactly that.
            mem[0x063Du] = 1;                            // energy-out death cinematic
            s_neArmed = 1;
        }
    }
#ifdef ROF_FLIGHT_PROBE
    // Headless only: TYPE THE INITIALS.  Without this the probe run can only ever show the entry
    // waiting for a key, which leaves the second half of the feature — the insert, the game's own
    // save call ($5C54) and the deferred write — completely unmeasured.  Injected through
    // s_pendingFlightKey, the same one-shot a real keypress uses, so the whole delivery chain
    // (the $539A CLI window -> vbi_attract_poll -> $0049 -> render_text_cell's $5E50 lookup) is
    // exercised rather than bypassed.  The plain interactive `make NAME_ENTRY=1` build has no
    // PROBES and still lets the letters be picked by hand, on real hardware included.
    {
        static const uint8_t kType[] = { 0x3Fu, 0x12u, 0x2Au, 0x0Cu };   // A, C, E, RETURN
        static uint8_t  s_typeIdx = 0;
        static uint16_t s_typeLast = 0;
        extern volatile unsigned short g_nePass;
        if (g_nePass && s_typeIdx < sizeof kType && g_atariDlist == 0x5E2Eu
            && (uint16_t)(g_vbiCount - s_typeLast) >= 12u && s_pendingFlightKey == 0xFFu) {
            s_pendingFlightKey = kType[s_typeIdx++];
            s_typeLast = g_vbiCount;
        }
        // ...then QUIT cleanly, a few seconds after the game has written the block.  This is the
        // only way to reach the half of persistence a killed emulator can never show: the write
        // itself declines mid-run by design, so the file is only created by rof_hiscore_flush()
        // after run() has Permit()ed and restored the view.  Quitting through g_pumpQuit is the
        // same path the left mouse button takes, so nothing about the shutdown is special-cased.
        extern volatile unsigned short g_hsSioWrite;
        static uint16_t s_quitAt = 0;
        if (g_hsSioWrite >= 2u && s_quitAt == 0u) s_quitAt = (uint16_t)(g_vbiCount + 150u);
        if (s_quitAt && (int16_t)(g_vbiCount - s_quitAt) >= 0) g_pumpQuit = 1;
    }
#endif
#endif

#ifdef ROF_INVULNERABLE
    // Debug / benchmark toggle (`make INVULNERABLE=1`): never die in flight.
    //
    // The energy gauge is life_counter $062F, a 0..$DC bar position.  vobj_advance ($4184)
    // decrements it as damage lands and, the moment it reaches 0, writes event_trigger $063D,
    // which sends enemy_check into intro_cinematic_loop ($4F3F) = the energy-out death
    // cinematic.  Topping the bar up before it can reach 0, and clearing $063D outright,
    // removes both the trigger and its consequence.
    //
    // This exists for COMBAT=1: at level 40 the gun emplacements shoot back hard enough to
    // kill the no-input auto-pilot in ~1500 vbi (measured: 12 hits, dead at vbi 3400), which
    // is far too short a window to profile — and any sampling window that straddles the death
    // cinematic silently deflates every rate inside it (the same trap fps_seg.gdb warns about).
    // The top-up is a compare + occasional byte store per vblank, i.e. free.
    //
    // Deliberately leaves the bar draining between top-ups instead of pinning it at full, so
    // the gauge sprite keeps being rebuilt and that work stays in the profile.
    if ((mem[0x0222u] | (mem[0x0223u] << 8)) == 0x4FF5u) {
        if (mem[0x062Fu] < 0x60u) mem[0x062Fu] = 0xDCu;   // refill before it can hit 0
        mem[0x063Du] = 0;                                 // disarm the death cinematic
    }
#endif

// ⚠ The g_cl* counters are defined under ROF_FLIGHT_PROBE *and* ROF_COMBAT_LOAD, so this use
// site needs BOTH guards — every other g_cl* use site already has them.  With a bare
// `#ifdef ROF_COMBAT_LOAD` the documented `make COMBAT=1 FPSCOUNT=1 FIXED_RNG=1` recipe (the
// only valid way to quote a combat framerate) did not compile at all.
#if defined(ROF_COMBAT_LOAD) && defined(ROF_FLIGHT_PROBE)
    // Combat-state framerate split (see the g_clVbi/g_clFrm definitions above).  Classify this
    // flight vblank and hand it, plus every terrain frame painted since the previous vblank, to
    // that state's pair of counters.  EXPLOSION wins over SAUCER when both are true.
    if ((mem[0x0222u] | (mem[0x0223u] << 8)) == 0x4FF5u) {
        static unsigned long s_clPrevFrames = 0;
        const unsigned long now = g_clFrames;
        const int st = (mem[0x0041u] != 0)        ? 0    // an explosion / impact is animating
                     : !(mem[0x006Au] & 0x80u)    ? 1    // a saucer object slot is live
                     : (mem[0x0036u] != 0x80u)    ? 2    // our own laser is in flight
                                                  : 3;   // quiet
        if (st == 0) g_clExplSeen = 1;   // sticky for the phase decomposition's classifier
        g_clVbi[st] += 1;
        g_clFrm[st] += now - s_clPrevFrames;
        g_clVbiObj[g_clObjBucket] += 1;
        g_clFrmObj[g_clObjBucket] += now - s_clPrevFrames;
        s_clPrevFrames = now;
    }
#endif

    // Per-frame VBI body — run in the REAL vertical-blank interrupt, where the Atari ran
    // its VBI.  game_vbi_isr() dispatches by the live VVBLKI vector to the standby ($52D7)
    // or flight ($4FF5) body — as the Atari swaps VVBLKI — bracketing the work in a
    // save/restore of the shared 6502 register file (the main loop may be mid-instruction
    // using `cpu` when this interrupt preempts it).
#ifdef ROF_FORCE_ALIEN_ABOARD
    // Headless "alien aboard" repro (make PROBES=1 FORCE_RETURN=1 ALIEN_ABOARD=1).
    // Reproduces the state a boarded alien leaves behind — alien_trigger $0633 set — which
    // otherwise needs a landed, systems-off, airlock-open rescue.  $0633 is a plain
    // sticky flag (set at the reveal in pilot_render, cleared only by the launch cinematic), so
    // setting it in flight IS the state under test: enemy_check $3FCD then runs alien_attack_tick
    // every main-loop iteration, exactly as it does after a real boarding.
    // Sampled here too: how the ascent physics move once BOOSTERS has set $0072 = 2.
    {
        const uint16_t vv = (uint16_t)(mem[0x0222u] | (mem[0x0223u] << 8));
        static uint16_t s_abVbi = 0;
        if (vv == 0x4FF5u && s_abVbi == 0) s_abVbi = g_vbiCount;
        if (s_abVbi && (uint16_t)(g_vbiCount - s_abVbi) >= 120) mem[0x0633u] = 1;
        if (vv == 0x52B4u) g_ab52B4++;
        // Ascent sampler: everything the climb depends on, per VBI, while $0072 == 2.
        if (vv == 0x4FF5u && mem[0x0072u] == 0x02u) {
            if (g_abVbi == 0) { g_abIter0 = g_iterCount; g_ab34First = mem[0x0034u]; }
            g_abVbi += 1;
            g_abIterN = g_iterCount;
            const unsigned char d34 = mem[0x0034u];
            g_ab34Last = d34;
            if (d34 > g_ab34Max) g_ab34Max = d34;
            g_ab29Last = mem[0x0029u]; g_ab2ALast = mem[0x002Au];
            g_ab26Last = mem[0x0026u]; g_ab2ELast = mem[0x002Eu];
            g_ab4ALast = mem[0x004Au]; g_ab04Last = mem[0x0004u];
            g_abKeyPend = s_pendingFlightKey;
            if (g_abVbi == 200u && g_abBreakVbi == 0) {   // soft-reset mid-ascent, as the user did
                s_pendingFlightKey = 0x80u;                // Atari BREAK (Help)
                g_abBreakVbi = g_vbiCount;
            }
            if ((g_abVbi % 25u) == 1u && g_abNSamp < 32u) {
                g_abSamp34[g_abNSamp] = d34;
                g_abSamp29[g_abNSamp] = mem[0x0029u];
                g_abSamp2E[g_abNSamp] = mem[0x002Eu];
                g_abNSamp += 1;
            }
        }
    }
#endif

#ifdef ROF_FORCE_RETURN
    // Headless return-to-mother-ship verification — drive the REAL gameplay path, not a $0072
    // poke (which skipped the arrival setup and gave an unfaithful repro).  Two phases:
    //   Phase 1 (flight+250 VBI): arm the mother-ship ARRIVAL the way gameplay does — set the
    //     level_cleared_flag countdown $2849=1, which the flight VBI's per-frame check ($5223)
    //     decrements to 0 and then calls setup_level_clear_state $7BC6 → $003A=$FF + shows the
    //     "MOTHER SHIP!" message and lights the indicator.  (Natural arrival, faithful.)
    //   Phase 2 (flight+340 VBI, after the message shows): inject a real B keypress via the
    //     flight keyboard path (s_pendingFlightKey → the $519c CLI window → event_sequence_
    //     dispatcher $4644 → boosters handler $493D), which does the full faithful boosters
    //     setup ($0676 light, clear rescue state, $0072=2, "FIRE BOOSTERS" msg) itself.
    //   (make PROBES=1 FORCE_RETURN=1)
    {
        const uint16_t vv = (uint16_t)(mem[0x0222u] | (mem[0x0223u] << 8));
        static uint16_t s_flightVbi = 0; static uint8_t s_retPhase = 0; static uint16_t s_retSelVbi = 0;
        if (vv == 0x4FF5u && s_flightVbi == 0) s_flightVbi = g_vbiCount;
        const uint16_t dt = s_flightVbi ? (uint16_t)(g_vbiCount - s_flightVbi) : 0;
        if (s_flightVbi && s_retPhase == 0 && dt >= 250) {
            mem[0x003Au] = 0xFF;          // level-clear / mother-ship-arrived gate (what $7BC6 sets)
            mem[0x0676u] = 1;             // mother-ship HUD light on
            mem[0x2849u] = 1;             // level_cleared_flag → $5223 chain also fires $7BC6 (MOTHER SHIP! msg)
#ifdef ROF_FORCE_LEVEL_DONE
            // LEVEL_DONE: make this a COMPLETED level.  $0629 is the rescued-pilot count (INC'd per
            // rescue by $7AF4) and $062A the level's quota; boot_standby_launch_driver's L_615b
            // subtracts them, and quota met is what runs the BONUS POINTS tally, the LEVEL COMPLETE
            // message, the $0655 jingle and the lift.  Pinning it here — at the arrival, where real
            // play would already have the pilots aboard — leaves the whole sequence to the faithful
            // binary.  ($0629 >= $061F also makes $7AF4 call game_sub_7B54 for the rest of the
            // flight, exactly as a real quota-met flight does.)
            mem[0x0629u] = mem[0x062Au];
#endif
            s_retPhase = 1;
        }
        if (s_retPhase == 1 && dt >= 340) {
            s_pendingFlightKey = 0x15;     // Atari KBCODE 'B' (boosters) → $519c CLI window
            // HALF ENERGY at the boost.  The forced return fires before the ship has taken any
            // damage, so energy $062F is still FULL — and a full bar is exactly 56 rows tall,
            // filling its dial with nothing hanging below it.  Every gauge-vs-cockpit priority
            // bug is invisible in that state: the overflow only exists when
            // the bar top has moved down.  $DC = full (bar top index ($DC-$062F)>>2 = 0), so
            // $6E parks the top mid-dial (index 27) and leaves 27 rows hanging below it.
            mem[0x062Fu] = 0x6E;
            s_retPhase = 2;
        }
        // Phase 3: once the return cinematic has landed back in the POST-mother-ship Standby
        // cockpit ($52D7 + $003A==$FF), pulse Del/SELECT (CONSOL bit1) to confirm SELECT drives
        // the IN-PLACE level cycle (door-scroll level_stage++ / fade-rebuild) — NOT the separate
        // selector card.  Pulsed (down ~8f / up) so each press is a distinct edge.
        // ⚠ Suppressed under ATTRACT_NOW: that harness wants the post-mother-ship Standby to sit
        // IDLE until the attract timeout fires, and a SELECT press refreshes the idle state.
        // ⚠ Also suppressed under LEVEL_DONE: SELECT is the lift's early-out (L_628f reads CONSOL
        // and jumps to L_62b4), so a pulse would cut the lift short instead of letting it run its
        // level+1..min(level+4,$63) range — which is the sequence under test there.
#if !defined(ROF_ATTRACT_NOW) && !defined(ROF_FORCE_LEVEL_DONE)
        if (s_retPhase >= 2 && vv == 0x52D7u && mem[0x003Au] == 0xFFu) {
            if (s_retSelVbi == 0) s_retSelVbi = g_vbiCount;
            uint16_t ds = (uint16_t)(g_vbiCount - s_retSelVbi);
            if (ds >= 60) { uint16_t ph = (uint16_t)((ds - 60) % 60u);
                            if (ph < 8u) s_consolState &= (uint8_t)~0x02u; else s_consolState |= 0x02u; }
        }
#else
        (void)s_retSelVbi;
#endif
#ifdef ROF_FORCE_RELAUNCH
        // Phase 4: relaunch.  Once the return cinematic has landed back in the post-mother-ship
        // Standby ($52D7 + $003A==$FF) and it has settled, HOLD START again — the headless second
        // F1 press.  Released once flight ($4FF5) is reached, so the condition itself times out.
        // Runs AFTER the auto-launch block above, which by now sees $060B==$23 and releases START,
        // so this assignment is the one that survives the frame.
        static uint16_t s_relaunchVbi = 0;
        if (s_retPhase >= 2 && vv == 0x52D7u && mem[0x003Au] == 0xFFu) {
            if (s_relaunchVbi == 0) s_relaunchVbi = g_vbiCount;
            const uint16_t dr = (uint16_t)(g_vbiCount - s_relaunchVbi);
            if (dr >= 150) {
                s_consolState &= (uint8_t)~0x01u;      // START pressed (active low)
                mem[0xD01Fu] &= (uint8_t)~0x01u;
#ifdef ROF_AUDIO_TRACE
                rof_audio_trace_arm(1);                // window 1 = the 2nd launch
#endif
            }
        }
#endif
    }
#endif

#ifdef ROF_FORCE_BREAK
    // Headless BREAK/Restart verification: once flight has been live a moment, inject BREAK ($80)
    // via the flight keyboard path and verify the restart recovers (VVBLKI leaves $52B4 and
    // returns to a real scene instead of the black/brown stuck state).  Build: PROBES=1 FORCE_BREAK=1.
    {
        const uint16_t vv = (uint16_t)(mem[0x0222u] | (mem[0x0223u] << 8));
        static uint16_t s_fbVbi = 0; static uint8_t s_fbPhase = 0;
        if (vv == 0x4FF5u && s_fbVbi == 0) s_fbVbi = g_vbiCount;
        uint16_t dt = s_fbVbi ? (uint16_t)(g_vbiCount - s_fbVbi) : 0;
        if (s_fbVbi && s_fbPhase == 0 && dt >= 150) { s_pendingFlightKey = 0x80; s_fbPhase = 1; }  // BREAK
    }
#endif

#ifdef ROF_FORCE_BREAK_EARLY
    // Headless BREAK-outside-flight verification: inject BREAK ($80) while still in the
    // Standby / launch cinematic ($52D7), i.e. BEFORE flight, and confirm the restart fires from a
    // non-flight scene (nothing consumes s_pendingFlightKey there — rof_check_restart must).  Fires
    // once, before the vbi==350 auto-START.  After it we should reach the $53CC selector card.
    {
        static uint8_t s_fbeDone = 0;
        if (!s_fbeDone && g_vbiCount >= 240) { s_pendingFlightKey = 0x80; s_fbeDone = 1; }  // BREAK in Standby
        // After the restart lands on the rebuilt Standby ($52D7), hold START (CONSOL bit0) to launch
        // the doors so the doors-top-half-missing-post-restart case is reproducible headlessly.  Gate
        // on the restart having happened + the door field rebuilt; hold until launched ($060B==$23).
        extern volatile unsigned char g_restartCount, g_doorFieldReady;
        const uint16_t vv2 = (uint16_t)(mem[0x0222u] | (mem[0x0223u] << 8));
        if (g_restartCount && vv2 == 0x52D7u && g_doorFieldReady) {
            s_consolState = (mem[0x060Bu] != 0x23u) ? 0x06u : 0x07u;
            mem[0xD01Fu]  = (mem[0x060Bu] != 0x23u) ? 0x06u : 0x07u;  // START held until launched
        }
    }
#endif

#ifdef ROF_FORCE_BREAK_CARD
    // Headless bug-3 repro via the $53CC results/level-select CARD path (the scene the user tests):
    // break during the STARS cinematic ($060B==$23 under the standby VBI $52D7, NOT flight), so the
    // restart preserves cockpit_flag($060B)!=0 and game_main_loop shows the card.  Auto-launch (NOT
    // NOAUTO) then holds START from the card -> standby rebuild -> doors, exercising the card->doors
    // transition where the top half was reported missing.  Fires once.
    {
        static uint8_t s_fbcDone = 0;
        const uint16_t vvc = (uint16_t)(mem[0x0222u] | (mem[0x0223u] << 8));
        if (!s_fbcDone && vvc == 0x52D7u && mem[0x060Bu] == 0x23u && g_vbiCount > 400) {
            s_pendingFlightKey = 0x80; s_fbcDone = 1;   // BREAK during stars
        }
        // Once the restart lands on the $53CC card, hold START (CONSOL bit0) to launch from it ->
        // game_main_loop outer loop -> boot_standby_launch_driver rebuild -> doors (the bug-3 path).
        extern volatile unsigned char g_restartCount;
        if (g_restartCount && vvc == 0x53CCu && mem[0x365Bu] == 0x72u) {
            s_consolState &= (uint8_t)~0x01u; mem[0xD01Fu] = s_consolState;   // START down
        } else if (g_restartCount) {
            s_consolState |= 0x01u; mem[0xD01Fu] = s_consolState;             // START up once off the card
        }
    }
#endif

#ifdef ROF_FORCE_ESC_STANDBY
    // Headless repro of "ESC in Standby soft-hangs on the way into flight": inject ESC
    // (KBCODE $1c) once while the STANDBY VBI ($52D7) is live, i.e. BEFORE the auto-START.
    // Only the flight VBI's $519c CLI window consumes s_pendingFlightKey, so an unfixed build
    // carries the keycode across the whole launch cinematic and dispatches it as the freeze
    // takeover ($0043) on the first flight VBI firing.  Build: PROBES=1 FORCE_ESC_STANDBY=1.
    {
        static uint8_t s_fesDone = 0;
        const uint16_t vvE = (uint16_t)(mem[0x0222u] | (mem[0x0223u] << 8));
        if (!s_fesDone && vvE == 0x52D7u && g_vbiCount > 200) { s_pendingFlightKey = 0x1C; s_fesDone = 1; }
    }
#endif

#if defined(ROF_PAUSE_NOW) || defined(ROF_FORCE_PAUSE)
    // ESC-pause colour-cycle harness (the "screensaver" strobe).  Every nudge here leaves the strobe
    // itself entirely genuine — the real flight-VBI block at $5039 (rof_native.c, `mem[$07E9+y] ^
    // RTCLOK & $F6 -> $00CF+y`) is what writes the pens; we only cut the wait:
    //   * PAUSE_NOW (interactive) skips the WARMUP.  The strobe needs event_pending_flag $063B to
    //     have climbed 1 -> $80, and $063B only ticks on each 256-frame wrap of the jiffy low byte
    //     $0014 — ~127*256 frames ~= 11 MINUTES (see the pause-mechanism memory).  Seed $063B=$80 to
    //     land at the end of that climb, then force the jiffy wrap every 8 frames instead of every
    //     256 so the cycle steps ~6x/second.  RTCLOK $0012 still advances one step per fire, so the
    //     pens walk the same sequence — just clocked faster.  ESC stays the user's to press.
    //   * FORCE_PAUSE (headless) additionally injects ESC (KBCODE $1c) so the freeze takeover engages
    //     the way a keypress does ($0043=1 via event_sequence_dispatcher $4644).
    //   (make PAUSE_NOW=1 + ./run.sh, or make PROBES=1 FORCE_PAUSE=1 +
    //    GDBSCRIPT=pause_cycle.gdb ./diag_run.sh 45)
    // ⚠ FORCE_PAUSE must RETRY the ESC until the freeze actually engages.  A single shot silently
    // does nothing: the flight VBI reaches its KEYWIN window ($519A) only while joystick_saved $004A
    // != 0, and the headless auto-flight (no stick input) crashes into terrain after a couple of
    // hundred frames, which zeroes $004A — after that the keycode just sits in s_pendingFlightKey
    // forever, unconsumed, and the run reads as "the strobe never reached anything".
    {
        const uint16_t vvP = (uint16_t)(mem[0x0222u] | (mem[0x0223u] << 8));
        static uint16_t s_fpVbi = 0; static uint8_t s_fpPhase = 0;
        if (vvP == 0x4FF5u && s_fpVbi == 0) s_fpVbi = g_vbiCount;
        const uint16_t dtP = s_fpVbi ? (uint16_t)(g_vbiCount - s_fpVbi) : 0;
        // ⚠ Seed $063B ONLY once the freeze has actually latched ($0043 != 0).  Doing it up front
        // wedges the game: bit7 of event_pending_flag gates live gameplay (e.g. the atmosphere colour
        // ramp, rof_native.c `game_state == 0 && !(event_pending_flag & 0x80)`), so a build that set it
        // at flight+40 hung on the way into flight — altimeter up, wing-clearance bars never arriving,
        // and ESC dead because the flight loop never reached its KEYWIN window.  Gating on $0043 is
        // also the faithful order: $503D only starts climbing $063B once $0043|$003E is nonzero, i.e.
        // the warmup runs DURING the pause, never before it.
        if (s_fpVbi && s_fpPhase == 0 && dtP >= 40) {
            if (mem[0x0043u] != 0) {              // frozen — now skip the warmup
#ifdef ROF_FLIGHT_PROBE
                g_fpFreezeVbi = g_vbiCount;
#endif
                mem[0x063Bu] = 0x80;              // end of the ~11-minute event_pending_flag warmup
                s_fpPhase = 1;
            }
#ifdef ROF_FORCE_PAUSE
            else if ((dtP & 15u) == 0) {          // headless only: press ESC (a 2nd press unfreezes)
                s_pendingFlightKey = 0x1C; g_fpTries++;
            }
#endif
        }
        // Clock the jiffy wrap only WHILE frozen, so an un-pause hands $0014 straight back to the game.
        if (s_fpPhase == 1 && mem[0x0043u] != 0 && (g_vbiCount & 7u) == 7u) mem[0x0014u] = 0xFF;
    }
#endif


    // Sample the real joystick (port 1) immediately before the game body reads PORTA/TRIG0, which
    // is where the Atari's own VBI polled it.  Four register reads; no beam deadline of its own.
    pollJoystick();

    game_vbi_isr();
#ifdef ROF_FLIGHT_PROBE
    // Restart-flash diagnosis: ring-record VVBLKI + the hold flag every vblank once a restart is in
    // flight, so the post-run dump shows the exact per-frame VVBLKI timeline around the BREAK (and
    // whether any viewport vector is live while the hold is off = the flash frame).
    {
        extern volatile unsigned char g_restartCount, g_restartHoldBlack;
        extern volatile unsigned short g_vvRing[32]; extern volatile unsigned char g_vvHold[32];
        extern volatile unsigned char g_vvIdx; extern volatile unsigned char g_vvArmed;
        const uint16_t _vv = (uint16_t)(mem[0x0222] | (mem[0x0223] << 8));
        if (_vv == 0x52B4u || g_restartCount) g_vvArmed = 1;   // arm at the trampoline or the restart
        if (g_vvArmed && g_vvIdx < 32) {
            g_vvRing[g_vvIdx] = (unsigned short)(mem[0x0222] | (mem[0x0223] << 8));
            g_vvHold[g_vvIdx] = g_restartHoldBlack;
            g_vvIdx++;
        }
    }
    // Whole-ISR cost, in raster lines, while the STANDBY VBI ($52D7) is the live body — the
    // level-select elevator scroll runs entirely in here.  >312 = the ISR outlasted the frame,
    // so the next VERTB was already pending on return and one displayed scroll step was lost.
    {
        extern volatile unsigned short g_isrEntryLine, g_isrStbyLines, g_isrStbyMax, g_isrStbyOver;
        extern volatile unsigned long  g_isrStbyCnt;
        if ((uint16_t)(mem[0x0222] | (mem[0x0223] << 8)) == 0x52D7u) {
            unsigned short _vp3 = *(const volatile unsigned short*)0xDFF004u;
            unsigned short _vh3 = *(const volatile unsigned short*)0xDFF006u;
            unsigned short ln = (unsigned short)(((_vp3 & 1u) << 8) | (_vh3 >> 8));
            unsigned short d  = (unsigned short)((ln >= g_isrEntryLine) ? (ln - g_isrEntryLine)
                                                                       : (ln + 312u - g_isrEntryLine));
            g_isrStbyLines = d; g_isrStbyCnt++;
            if (d > g_isrStbyMax) g_isrStbyMax = d;
            if (d >= 290u) g_isrStbyOver++;   // ~a whole frame: the next VERTB is already pending
        }
    }
#endif
    return 0;
}

#ifdef ROF_FLIGHT_PROBE
// ============================================================================
//  Interrupt-chain taps (probe only) — see the g_tap* declarations above
// ============================================================================
// One head/tail server pair per chain, both returning 0 so the real chain runs untouched.
// The beam is read from VHPOSR only (low 8 bits of vpos): a chain runs for microseconds, so
// the one-per-frame vpos-255->0 wrap is the only ambiguity and the &0xFF wrap absorbs it.
#define ROF_INT_TAP(name, idx)                                                            \
    static uint32_t tapHead_##name(void) {                                                \
        const volatile unsigned short* _c = (const volatile unsigned short*)0xDFF000u;     \
        unsigned short _vh = _c[0x006u / 2];                                              \
        s_tapV0[idx] = (unsigned short)(_vh >> 8);                                        \
        s_tapH0[idx] = (unsigned short)(_vh & 0xFFu);                                     \
        g_tapHeadCnt[idx]++;                                                              \
        return 0;                                                                          \
    }                                                                                      \
    static uint32_t tapTail_##name(void) {                                                \
        const volatile unsigned short* _c = (const volatile unsigned short*)0xDFF000u;     \
        unsigned short _vh = _c[0x006u / 2];                                              \
        g_tapVsum[idx] += (unsigned long)(((_vh >> 8) - s_tapV0[idx]) & 0xFFu);            \
        g_tapHsum[idx] += (long)(short)((_vh & 0xFFu) - s_tapH0[idx]);                     \
        g_tapTailCnt[idx]++;                                                              \
        return 0;                                                                          \
    }
ROF_INT_TAP(exter, 0)
ROF_INT_TAP(ports, 1)

// A follower tap: no stamp of its own, it accumulates the elapsed time since the pri-127 HEAD
// of chain `src` into slot `dst`.  Slotted between the OS servers by priority, a set of these
// gives a cumulative timeline through one chain (see the slot map above).
#define ROF_INT_TAP_FOLLOW(name, src, dst)                                                \
    static uint32_t tapFollow_##name(void) {                                              \
        const volatile unsigned short* _c = (const volatile unsigned short*)0xDFF000u;      \
        unsigned short _vh = _c[0x006u / 2];                                              \
        g_tapVsum[dst] += (unsigned long)(((_vh >> 8) - s_tapV0[src]) & 0xFFu);             \
        g_tapHsum[dst] += (long)(short)((_vh & 0xFFu) - s_tapH0[src]);                      \
        g_tapTailCnt[dst]++;                                                              \
        g_tapHeadCnt[dst]++;                                                              \
        return 0;                                                                          \
    }
ROF_INT_TAP_FOLLOW(portsAfterCiaa, 1, 2)   // pri 50: after ciaa.resource (120), before UAE fs (20)
ROF_INT_TAP_FOLLOW(portsAfterUaefs, 1, 3)  // pri 10: after FS-UAE's "UAE fs" server

// EXTER + PORTS only.  There is deliberately NO BLIT tap: only PORTS/COPER/VERTB/EXTER/NMI are
// exec SERVER CHAINS (iv_Data = a List); BLIT is a single-handler vector, so AddIntServer would
// Enqueue into whatever its owner put in iv_Data.  Blit-interrupt traffic is measured instead by
// intvec_dump.gdb (who owns IntVects[6]) + the end-to-end INTF_BLIT A/B.
static struct Interrupt s_tapInt[4];       // [i*2] = head (pri 127), [i*2+1] = tail (pri -128)
static struct Interrupt s_tapMid[2];       // PORTS followers at pri 50 / pri 10
static const unsigned char kTapChain[2] = { INTB_EXTER, INTB_PORTS };

// BLIT-interrupt RATE probe.  IntVects[6] belongs to graphics.library ($F901C0 — its QBlit
// queue handler), so it cannot be tapped with AddIntServer; instead swap the whole vector for
// a bare counter (restored verbatim on exit, as the VERTB takeover does).  This measures how
// many level-3 interrupts our blits raise — the framework's blitter* helpers re-enable
// INTF_BLIT right after starting each blit, even though nothing in this port needs it
// (blitterWait polls DMACONR BLTBUSY and blitterDrain spin-drains the queue itself).
extern "C" { volatile unsigned long g_blitIrqCnt = 0; }
static struct IntVector s_savedBlitVec;
static struct Interrupt s_blitCntInt;
static bool             s_blitVecTaken = false;

static uint32_t blitCountHandler(void)
{
    *(volatile unsigned short*)0xDFF09Cu = (unsigned short)INTF_BLIT;   // ack (no SETCLR = clear)
    g_blitIrqCnt++;
    return 0;
}

static void intTapsInstall(void)
{
    static uint32_t (*const kHeads[2])(void) = { tapHead_exter, tapHead_ports };
    static uint32_t (*const kTails[2])(void) = { tapTail_exter, tapTail_ports };
    static const char* const kNames[4] = { "RoF tapH0", "RoF tapT0",
                                           "RoF tapH1", "RoF tapT1" };
    for (unsigned i = 0; i < 2; i++) {
        for (unsigned half = 0; half < 2; half++) {
            struct Interrupt* it = &s_tapInt[i * 2 + half];
            it->is_Node.ln_Type = NT_INTERRUPT;
            it->is_Node.ln_Pri  = half ? -128 : 127;
            it->is_Node.ln_Name = (char*)kNames[i * 2 + half];
            it->is_Data = 0;
            it->is_Code = (void(*)())(half ? kTails[i] : kHeads[i]);
            AddIntServer(kTapChain[i], it);
        }
    }

    {   // PORTS chain splitters: pri 50 lands between ciaa.resource (120) and "UAE fs" (20),
        // pri 10 after both — so slot2 = ciaa.resource's own time and slot3-slot2 = UAE fs's.
        static uint32_t (*const kMids[2])(void) = { tapFollow_portsAfterCiaa,
                                                   tapFollow_portsAfterUaefs };
        static const signed char kMidPri[2]     = { 50, 10 };
        static const char* const kMidName[2]    = { "RoF tapM2", "RoF tapM3" };
        for (unsigned i = 0; i < 2; i++) {
            s_tapMid[i].is_Node.ln_Type = NT_INTERRUPT;
            s_tapMid[i].is_Node.ln_Pri  = kMidPri[i];
            s_tapMid[i].is_Node.ln_Name = (char*)kMidName[i];
            s_tapMid[i].is_Data = 0;
            s_tapMid[i].is_Code = (void(*)())kMids[i];
            AddIntServer(INTB_PORTS, &s_tapMid[i]);
        }
    }

    s_blitCntInt.is_Node.ln_Type = NT_INTERRUPT;
    s_blitCntInt.is_Node.ln_Pri  = 0;
    s_blitCntInt.is_Node.ln_Name = (char*)"RoF BLITcnt";
    s_blitCntInt.is_Data = 0;
    s_blitCntInt.is_Code = (void(*)())blitCountHandler;
    {
        struct IntVector* iv = &SysBase->IntVects[INTB_BLIT];
        Disable();
        s_savedBlitVec = *iv;
        iv->iv_Data    = 0;
        iv->iv_Code    = (void(*)())blitCountHandler;
        iv->iv_Node    = &s_blitCntInt.is_Node;
        Enable();
        s_blitVecTaken = true;
    }
}

static void intTapsRemove(void)
{
    for (unsigned i = 0; i < 2; i++)
        for (unsigned half = 0; half < 2; half++)
            RemIntServer(kTapChain[i], &s_tapInt[i * 2 + half]);
    for (unsigned i = 0; i < 2; i++)
        RemIntServer(INTB_PORTS, &s_tapMid[i]);

    if (s_blitVecTaken) {
        Disable();
        SysBase->IntVects[INTB_BLIT] = s_savedBlitVec;
        Enable();
        s_blitVecTaken = false;
    }
}
#endif  // ROF_FLIGHT_PROBE

// ============================================================================
//  High-score persistence — the ONE file this port touches
// ============================================================================
// The block the game saves (name_entry_loop $5C54 -> rof_sio_block) is written MID-RUN, inside
// run()'s Forbid() with the OS display gone and the VERTB/PORTS vectors hijacked.  A dos.library
// call Wait()s on a filesystem packet, which is exactly what must not happen there: it breaks the
// Forbid, and a missing/unwritable volume can pop a requester onto a screen the OS no longer owns.
//
// So the file I/O is moved to the two moments the OS is fully ours to use:
//   read   in the constructor, before run() takes anything over;
//   write  after run() has Permit()ed and given the display back (s_hiscoreWritable).
// In between, hiscoreSave() DECLINES (returns false) and rof_hiscore.c keeps the block and its
// dirty flag in RAM for the exit flush.  The cost is that a hard reset or power-off loses the
// session's scores; the benefit is that nothing about the takeover has to change.
//
// That is the FALLBACK path, and it is what a Shell or Workbench launch uses.  A launcher that
// can persist a buffer at any moment says so instead, by filling in the ExternalHooks block
// (ExternalHooks.h) before the game's first instruction: hiscoreSave() then hands the block
// straight to the hook the moment the game writes it, and nothing is deferred.  The WHDLoad
// slave does exactly that, with resload_SaveFile — which is safe mid-takeover, unlike a dos
// packet — so under WHDLoad a score survives a hard reset or the F10 quit key too.  The game
// itself knows nothing about WHDLoad or resload; it only knows a pointer was, or was not, set.
//
// The fallback file name is relative on purpose.  "Next to the executable" cannot be spelled
// PROGDIR: under WHDLoad (the Shell/Workbench sets PROGDIR: up, and a LoadSeg does not), so a
// plain relative name is the one spelling that resolves sensibly under every launch method.
// (Under WHDLoad the hook takes over anyway, and resload puts the file in the data directory
// beside the slave.)
static const char kHiScoreFile[] = "RoF.hi";

static uint8_t s_hiscoreFile[256];
static bool    s_hiscoreFileValid = false;   // the ctor read a full 256-byte block
static bool    s_hiscoreWritable  = false;   // the OS is ours again — writes may go out

// Read the saved block, before the display/interrupt takeover.  A missing or short file is not
// an error: rof_hiscore_init keeps the factory table in that case.
static void hiscoreFileRead()
{
    // An external launcher that supplied a load hook owns the storage entirely — do not also
    // go looking for a file of our own next to whatever directory it happened to start us in.
    if (g_rofExternalHooks.hiscoreLoad) {
        s_hiscoreFileValid = (g_rofExternalHooks.hiscoreLoad(s_hiscoreFile, 256) != 0);
        return;
    }
    if (!DOSBase) return;
    BPTR fh = Open((STRPTR)kHiScoreFile, MODE_OLDFILE);
    if (!fh) return;
    LONG n = Read(fh, s_hiscoreFile, 256);
    Close(fh);
    s_hiscoreFileValid = (n == 256);
}

bool PlatformAmiga::hiscoreLoad(uint8_t* blk)
{
    extern volatile unsigned short g_hsFromFile;
    if (!s_hiscoreFileValid) return false;
    for (int i = 0; i < 256; i++) blk[i] = s_hiscoreFile[i];
    g_hsFromFile = 1;
    return true;
}

bool PlatformAmiga::hiscoreSave(const uint8_t* blk)
{
    // Always keep the newest block, whether or not it can go out yet — this is the RAM shadow
    // the exit flush writes.
    for (int i = 0; i < 256; i++) s_hiscoreFile[i] = blk[i];
    s_hiscoreFileValid = true;

    // A hooked launcher can write from here, mid-run, and reports whether it did.
    if (g_rofExternalHooks.hiscoreSave) {
        if (g_rofExternalHooks.hiscoreSave(s_hiscoreFile, 256) == 0) return false;
        extern volatile unsigned short g_hsWritten; g_hsWritten++;
        return true;
    }

    if (!s_hiscoreWritable || !DOSBase) return false;   // mid-run: decline, stay dirty
    BPTR fh = Open((STRPTR)kHiScoreFile, MODE_NEWFILE);
    if (!fh) return false;                              // read-only medium: keep it in RAM
    LONG n = Write(fh, (APTR)s_hiscoreFile, 256);
    Close(fh);
    if (n == 256) { extern volatile unsigned short g_hsWritten; g_hsWritten++; return true; }
    return false;
}

// ============================================================================
//  PlatformAmiga construction + run — takeover, install interrupts, run scene, restore
// ============================================================================
PlatformAmiga::PlatformAmiga(const char* /*imagePath*/)
{
    // Bring up the platform (mirrors PlatformSDL's ctor doing SDL_Init): open
    // graphics.library so run()'s display takeover can reach GfxBase.  On failure set
    // quit so main() bails (it checks plt.quit) instead of dereferencing a null GfxBase.
    // The Amiga image is embedded (incbin) and loaded in run() via load_xex_image(), so
    // the path argument is ignored.
    GfxBase = (struct GfxBase*)OpenLibrary((UBYTE*)"graphics.library", 33);
    quit = (GfxBase == 0);

    // dos.library, for the high-score file only — opened HERE so the read happens while the OS
    // still owns the machine, and so the open itself (which can Wait()) is outside run()'s
    // Forbid().  Its absence is not fatal: the game then runs on the factory table and saves
    // nothing.  V33 (1.2) is enough for Open/Read/Write/Close.
    DOSBase = (struct DosLibrary*)OpenLibrary((UBYTE*)"dos.library", 33);
    hiscoreFileRead();

    // Publish the global Platform* the C bridge (platform_cbridge.cpp) dispatches through.
    platform = this;
}

PlatformAmiga::~PlatformAmiga()
{
    if (DOSBase)  { CloseLibrary((struct Library*)DOSBase);  DOSBase  = 0; }
    if (GfxBase)  { CloseLibrary((struct Library*)GfxBase);  GfxBase  = 0; }
}

// RAM budget readout (always built — 6 longs and 6 AvailMem calls at startup).  The load image
// is fixed and readable offline (m68k-amiga-elf-objdump -h out/RoF.elf, the non-debug sections),
// but the RUNTIME allocations are not: the framework AllocMem()s every bitmap, copper list,
// sprite and audio buffer in CHIP.  These snapshot exec's free pools before and after the
// scene's constructor + display takeover, so "how much RAM does the game need" is a measurement
// rather than a sum of guesses.  Read with amiga/memreport.gdb.
extern "C" { volatile unsigned long g_memChipBefore = 0, g_memFastBefore = 0, g_memAnyBefore = 0; }
extern "C" { volatile unsigned long g_memChipAfter  = 0, g_memFastAfter  = 0, g_memAnyAfter  = 0; }
extern "C" { volatile unsigned long g_memChipLargest = 0; }

void PlatformAmiga::run()
{
    g_memChipBefore = AvailMem(MEMF_CHIP);
    g_memFastBefore = AvailMem(MEMF_FAST);
    g_memAnyBefore  = AvailMem(MEMF_ANY);

    // The scene holds several KB of shadow buffers; keep it in BSS (static), NOT on the
    // stack (which the PlatformAmiga instance lives on in main), to avoid stack overflow.
    static RescueOnFractalus scene;

    // --- takeover: save system state, disable OS display ---------------------
    // Four pieces of system state have to come back on the way out, and all four are captured
    // HERE, before the first write to any of them; the restore at the end of run() undoes them in
    // reverse order.
    //
    //   savedView     the OS View — reinstalled with LoadView().
    //   osCopperList  GfxBase->copinit, the OS's copper START-UP list.  This, not the View's own
    //                 list, is what COP1LC holds while the OS owns the display: it reinitialises
    //                 the display registers (FMODE, DIWSTRT/STOP, DDFSTRT/STOP, BPLCON0/2 and all
    //                 eight sprite pointers) at the top of every frame, then chains via a COPJMP2
    //                 strobe to the View's list in COP2LC.  graphics.library's VERTB server
    //                 rewrites COP2LC every frame, so COP1LC is the ONE copper register a takeover
    //                 has to put back — and LoadView() does NOT do it: LoadView only publishes the
    //                 View's lists for COP2LC.  Leave COP1LC pointing at our list and the OS runs
    //                 with our display setup forever — the machine is up, but the copper list is
    //                 wrong.  Same save/restore as the dA JoRMaS ProductionRunner.
    //   s_savedDmacon DMACONR — read before we clear a single channel.
    //   s_savedIntena INTENAR — likewise.
    struct View* savedView = GfxBase->ActiView;
    const CopperList osCopperList((uint32_t*)GfxBase->copinit);   // non-owning: never freed
    s_savedDmacon = AmigaHardware::enabledDMAChannels();
    s_savedIntena = AmigaHardware::enabledInterrupts();

    LoadView(NULL);
    WaitTOF();
    WaitTOF();

    // Disable raster (bitplane) and sprite DMA so old state doesn't leak through.  Keep
    // exec's disk/blitter/audio DMA as-is; copper DMA gets re-enabled below.
    AmigaHardware::setDMAChannels(DMAF_RASTER | DMAF_SPRITE | DMAF_COPPER, false);

    // --- interrupt sources: keep only what we actually service --------------------
    // Measured in flight (amiga/int_probe.gdb) with everything the OS left enabled: EXTER
    // (CIA-B) fires 0 times in 60 s, PORTS ~11/s (all of it ciaa.resource + FS-UAE's "UAE fs"
    // server — our own keyboard handler ran 0 times), and BLIT ~6 times per flight iteration,
    // every one of them a pointless level-3 dispatch (nothing here consumes blit-done; see
    // blitIrqArm in AmigaHardware.cpp).  So mask INTF_BLIT for the whole takeover window —
    // belt-and-braces alongside blitIrqArm, since something may have armed it before we ran —
    // and clear any request already latched so re-enabling it on the way out can't fire a
    // stale one into graphics.library.  The INTENAR saved above is restored verbatim at the end
    // (the OS needs its blit interrupt back for QBlit once we hand the machine over).
#ifndef ROF_BLIT_IRQ
    AmigaHardware::setInterrupts(INTF_BLIT, false);      // no SETCLR = disable
    AmigaHardware::clearInterruptRequests(INTF_BLIT);    // drop any latched blit-done request
#endif

    // Display window — standard PAL lores 320x200 visible area.  No bitplanes (bplcon0=0):
    // the whole area shows COLOR00 (copper-set background).
    *diwstrtPointer = 0x2c81;   // VSTRT=44, HSTRT=0x81
    *diwstopPointer = 0xf4c1;   // VSTOP=244, HSTOP=0xc1 (+256 implicit)
    *ddfstrtPointer = 0x0038;
    *ddfstopPointer = 0x00d0;
    *bplcon0Pointer = 0x0000;   // 0 bitplanes
    *bplcon1Pointer = 0x0000;
    *bplcon2Pointer = 0x0000;

    // --- VBI interrupt handler: take over the whole VERTB vector --------------
    vbiServer.is_Node.ln_Type = NT_INTERRUPT;
    // ln_Pri only matters in the VERTB_SERVER fallback (the takeover has no chain): 127 puts
    // us at the HEAD, ahead of graphics.library (pri 10) / gameport.device / timer.device.
    // Measured, pinned level, vbi-gated 60 s window: head-of-chain is +9.4% flight throughput
    // over the original pri 0 (0.0409 -> 0.0447 iterations/frame).
    vbiServer.is_Node.ln_Pri  = 127;
    vbiServer.is_Node.ln_Name = (char*)"RoF VBI";
    vbiServer.is_Data = 0;
    vbiServer.is_Code = (void(*)())vbiHandler;
#ifdef ROF_VERTB_SERVER
    AddIntServer(INTB_VERTB, &vbiServer);       // A/B fallback: share exec's server chain
#else
    // Replace exec's VERTB IntVector wholesale (see the long note at vbiServer): its
    // iv_Code is the server-chain walker, so overwriting it drops graphics.library /
    // gameport.device / timer.device off the vblank entirely.  iv_Node is cosmetic —
    // it is what OS debug tools report as the vector's owner.
    {
        struct IntVector* iv = &SysBase->IntVects[INTB_VERTB];
        Disable();
        s_savedVertb  = *iv;
        iv->iv_Data   = 0;
        iv->iv_Code   = (void(*)())vbiHandler;
        iv->iv_Node   = &vbiServer.is_Node;
        Enable();
        s_vertbTaken = true;
    }
#endif

    // (The attract/standby-theme SFX tick sfx_voice_tick ($70F9) now runs in the
    // INTB_VERTB VBI body — standby_vbi_native, gated $00E7 & BIT $062D = 25 Hz —
    // exactly as the Atari's deferred VBI $534D did, keeping it frame-synchronized
    // with the main loop.  It used to run off a dedicated CIA-B Timer A interrupt.)

    // --- bring up the scene --------------------------------------------------
    // Load the faithful boot memory image (pristine rof.xex) into mem[] before anything
    // reads it — the genuine power-on RAM.  Display DMA (copper/raster/sprite) stays OFF
    // here: COP1LC still points at the OS LoadView(NULL) copper, so enabling copper DMA now
    // would let the OS copper run all through initialize() and intermittently reset our
    // one-time custom-register setup (BPLCON3 border-blanking was getting clobbered when an
    // OS-copper frame happened to land after setPlayfield's write).  scene.initialize()
    // writes the constant registers and installs our first copper list (setCopperList →
    // COP1LC = ours) with the copper halted, so there is no race.
    load_xex_image();

    s_scene = &scene;
    scene.initialize();   // builds bitmaps/copper (COP1LC = ours); calls PlatformAmiga::audioInit

    // Our copper list is now installed and the constant registers are set — safe to start
    // display DMA.  The copper restarts from COP1LC (our list) at the next vblank; the OS
    // copper never runs again, so BPLCON3 (and the rest of setPlayfield) persists.
    *dmaconPointer = (uint16_t)(DMAF_SETCLR | DMAF_MASTER | DMAF_COPPER | DMAF_RASTER | DMAF_SPRITE);

    keyboardInit();       // F1 = START for the launch cinematic (also arms SP in CIA-A's ICR mask)
#ifdef ROF_PORTS_TAKEOVER
    portsTakeover();      // opt-in, measured NOT worth it — see the portsHandler comment
#endif

#ifdef ROF_FLIGHT_PROBE
    intTapsInstall();     // measure the EXTER/PORTS/BLIT chains we do NOT own (int_probe.gdb)
#endif

    // --- multitasking off for the duration ------------------------------------
    // Nothing here needs exec's scheduler and we never Wait(), so hold task switching off for
    // the whole window: no other task can steal the CPU or dirty the caches/bus mid-frame, and
    // no OS task can be woken by an interrupt we don't own.  Measured cost of NOT doing this
    // (amiga/int_probe.gdb, 60 s of flight): ExecBase->DispCount +0, IdleCount +0 — i.e. zero
    // task dispatches already, because the VERTB vector takeover removed exec's only scheduling
    // heartbeat (its VBlank server did the quantum accounting).  So this is insurance against
    // regressions, not a measured win.  ⚠ Everything between Forbid() and Permit() must be
    // Wait()-free: the WaitTOF() pairs and every library open/close are deliberately outside.
    Forbid();

    // Everything the game allocates (scene ctor: bitmaps, copper lists, sprites, audio
    // buffers) has happened by here, so this pair is the true runtime footprint.
    g_memChipAfter   = AvailMem(MEMF_CHIP);
    g_memFastAfter   = AvailMem(MEMF_FAST);
    g_memAnyAfter    = AvailMem(MEMF_ANY);
    g_memChipLargest = AvailMem(MEMF_CHIP | MEMF_LARGEST);

    // --- run -----------------------------------------------------------------
    // The whole game runs inside scene.run(): the genuine transpiled/native boot chain,
    // whose frame-wait spin loops each call platform_render_frame (render + wait on
    // g_vbiCount).  Returns when the user quits (left mouse button).
    scene.run();

    // --- restore system ------------------------------------------------------
    // The order below is the dA JoRMaS ProductionRunner teardown order, and every step of it is
    // load-bearing: stop OUR display, hand the copper back to the OS, and only THEN free the
    // memory the display was reading.  Freeing first leaves the copper executing a list that exec
    // is simultaneously relinking into its free pool, with the bitplane pointers aimed at bitmaps
    // going the same way.  Free-list bookkeeping assembles into copper instructions, and a
    // CDANG-less copper can still write everything from $080 up — COP1LC ($080) and DMACON ($096)
    // included.  That is how the machine ends up running but with the copper list not set
    // properly, instead of back on the Workbench.
#ifdef ROF_FLIGHT_PROBE
    intTapsRemove();
#endif
#ifdef ROF_PORTS_TAKEOVER
    portsRestore();       // hand level 2 back to ciaa.resource before the OS needs it again
#endif
    keyboardShutdown();

    // 0. Audio off BEFORE anything hands the machine back.  Nothing after the game loop makes a
    //    sound, and steps 2-3 below give the DISPLAY back to the OS — so silencing any later than
    //    this leaves Paula running while the Workbench is already on screen, for however long the
    //    rest of the teardown takes (the scene's ~30 FreeMem calls, LoadView, two WaitTOFs).  That
    //    is audible as the engine drone continuing after the game has visibly exited.
    audioShutdown();

    // 1. Our VBI off first: its scene bodies write COP1LC (setCopperList) and the live copper
    //    list's bitplane/sprite pointers, so every step below would otherwise be racing it.
    AmigaHardware::setInterrupts(INTF_VERTB, false);
    AmigaHardware::clearInterruptRequests(INTF_VERTB);

    // 2. Our display off: no copper, no bitplane fetch, no sprite fetch.
    AmigaHardware::setDMAChannels(DMAF_COPPER | DMAF_RASTER | DMAF_SPRITE, false);

    // 3. Copper back onto the OS's start-up list and RUN it — COP1LC = copinit plus an immediate
    //    COPJMP1 strobe, then copper DMA back on.  EXECUTING copinit is what undoes our display
    //    setup: FMODE, DIWSTRT/STOP (which also drops the ECS DIWHIGH extension setPlayfield
    //    wrote), DDFSTRT/STOP, BPLCON0/2, and all eight sprite pointers back to the OS's null
    //    sprite.  BPLCON3 is the one display register copinit does not carry, so undo
    //    setPlayfield's border blanking by hand — 0x0C00 is graphics.library's own
    //    OCS-compatible value, and without it the Workbench inherits our blanked borders on an
    //    ECS/AGA Denise.  Raster and sprite DMA stay off until step 6: the OS's View list has not
    //    been republished yet.
    AmigaHardware::setCopperList(osCopperList, true);
    *bplcon3Pointer = 0x0c00;
    AmigaHardware::setDMAChannels(DMAF_COPPER, true);

    // 4. Nothing reads our memory any more — the copper is on the OS's list, and the blitter
    //    queue is drained rather than left mid-blit into a bitmap about to go back to exec.  Only
    //    now is it safe to free it all.
    AmigaHardware::blitterDrain();
    scene.shutdown();     // frees every copper list / bitmap / sprite; calls audioShutdown

    // 5. VERTB vector back to exec's server-chain walker: before step 6 re-enables the interrupt,
    //    and before the WaitTOF()s below — those are signalled by graphics.library's VERTB server,
    //    which only runs again once the walker is back in the vector.
#ifdef ROF_VERTB_SERVER
    RemIntServer(INTB_VERTB, &vbiServer);
#else
    if (s_vertbTaken) {
        Disable();
        SysBase->IntVects[INTB_VERTB] = s_savedVertb;
        Enable();
        s_vertbTaken = false;
    }
#endif

    // 6. DMA channels and interrupt enables exactly as they were at takeover.  SETCLR alone is a
    //    complete restore here: every bit we turned on (MASTER, COPPER, RASTER, SPRITE, and the
    //    audio channels audioShutdown has already cleared) is one the OS had on too.  Mask to the
    //    writable DMACON bits — DMACONR's 13/14 are the read-only BZERO/BBUSY status flags, not
    //    channels.  Drop a latched blit request first so re-enabling INTF_BLIT cannot fire a
    //    stale one straight into graphics.library's QBlit handler.
    AmigaHardware::clearInterruptRequests(INTF_BLIT);
    AmigaHardware::setDMAChannels((uint16_t)(s_savedDmacon & (DMAF_ALL | DMAF_MASTER | DMAF_BLITHOG)), true);
    AmigaHardware::setInterrupts((uint16_t)((s_savedIntena & (uint16_t)~INTF_SETCLR) | INTF_INTEN), true);

    // 7. Multitasking and the OS View back.  Permit() belongs here, not at the top of the
    //    restore: everything above is Wait()-free, and the WaitTOF() pair below is not — a Wait()
    //    inside Forbid() breaks the forbidden state, and until step 5 there was no VERTB server
    //    to signal it anyway.  LoadView() republishes the View's copper lists for COP2LC, which
    //    copinit (running again from COP1LC) chains to; the two WaitTOF()s let that reach the
    //    screen before we return to DOS.
    Permit();
    LoadView(savedView);
    WaitTOF();
    WaitTOF();

    // The OS owns the machine again, so the deferred high-score write may finally go out.  Only
    // does anything if the player actually beat a score this session (rof_hiscore.c's dirty flag).
    s_hiscoreWritable = true;
    rof_hiscore_flush();
}
