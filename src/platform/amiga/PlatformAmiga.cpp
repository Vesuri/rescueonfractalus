// PlatformAmiga — the Amiga-specific platform layer (see PlatformAmiga.h).
//
// This translation unit holds, in order:
//   1. the POKEY->Paula audio backend (the bulk of the file),
//   2. the platform_c.h bridge the C-compiled 6502 transliteration calls,
//   3. the launch-cinematic frame pump + quit handling,
//   4. the CIA-A serial-port keyboard (RETURN -> Atari START switch),
//   5. the real INTB_VERTB VBI server + CIA-B Timer A music tick,
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
#include <exec/interrupts.h>
#include <exec/nodes.h>
#include <exec/memory.h>
#include <graphics/gfxbase.h>
#include <graphics/view.h>
#include <hardware/dmabits.h>
#include <hardware/intbits.h>
#include <resources/cia.h>
#include <hardware/cia.h>
#include "framework/AmigaHardware.h"
#include "PlatformAmiga.h"
#include "RescueOnFractalus.h"

// mem[] and cpu are defined in src/cpu/cpu.c (compiled for m68k as audio/cpu.o)
extern "C" volatile uint8_t mem[65536];

// Title Screen visual-test override: when set (only by the ROF_FORCE_TITLE probe path),
// deriveRenderSignals pins rsTitle true.  Defined unconditionally (default 0) so the normal
// build references a real symbol with zero effect.
extern "C" volatile unsigned char g_forceTitleScreen = 0;

// GfxBase is opened in main() (GCCRuntime.cpp defines it); set before run() is called.
extern struct GfxBase* GfxBase;

// load_xex_image (XexImage.cpp): populate mem[] with the pristine rof.xex boot image.
extern "C" void load_xex_image(void);
// sfx_voice_tick_native (SfxPlayer.cpp): the SFX music tick, driven by CIA-B Timer A.
extern "C" void sfx_voice_tick_native(void);
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
static __chip uint8_t wave_pure[2]      = { 0x7F, 0x81 }; // pure tone (square)
// Precomputed POKEY poly distortion waveforms.  A poly waveform's SHAPE depends only
// on the stride residue through the poly counter (s4 = stride%15 for the poly4 buzz,
// s5 = stride%31 for the poly5-gated tone) and the distortion mode — NOT on the channel
// or absolute pitch (pitch is carried separately by AUD_PER).  So there are only 15
// distinct poly4 waveforms and 31 distinct poly5-tone waveforms; build_poly_tables()
// builds all of them once at init and update_paula_channel just re-points Paula at the
// matching immutable buffer per note.  Generating on the fly (the old per-note ping-pong)
// glitched at note onset: the engine writes AUDF before AUDC, so a note's first repoint
// could land on a half-built or wrong-mode buffer that Paula then latched at the next DMA
// loop wrap.  Selecting from a static table removes that hazard.
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
static uint8_t        kBit9[POLY9_SIZE];              // poly9 bit stream (filled at init)
static __chip uint8_t poly_dist_buf[4][POLY_DIST_LEN];
static uint32_t       poly_dist_stride[4] = { 0, 0, 0, 0 };  // cache key: last stride built
static uint8_t        poly_dist_gate[4]   = { 0, 0, 0, 0 };  // cache key: last gate mode (+1=valid)

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
static void fill_noise_words(int off, int words)
{
    uint32_t* p = (uint32_t*)(noise_buf + off);
    uint32_t  x = noise_rng;
    for (int i = 0; i < words; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;          // xorshift32 (Marsaglia 13/17/5) — full period, fast
        p[i] = x;             // 4 sample bytes per step
    }
    noise_rng = x;
}
// Refill the whole buffer (init / channel-start prime).
static void fill_noise_buf(void) { fill_noise_words(0, NOISE_LEN / 4); }

// Called once per VBI: while any channel is in noise mode, refill a small slice
// (128 bytes = 32 longwords, ~0.7 ms or less) round-robin through the buffer so the
// noise texture slowly evolves (full cycle ~1.3 s) without ever statically repeating.
// The buffer LENGTH (not the refill) is what keeps the DMA loop sub-audible, so this
// is cheap insurance, not a per-frame full regeneration — that full regen in the VBI
// ISR overran the 20 ms vblank budget and dropped the launch cinematic to 25 Hz.
// Overwriting bytes Paula is mid-DMA on is inaudible for noise (random over random).
void PlatformAmiga::noiseTick()
{
    if (!(noiseOn[0] || noiseOn[1] || noiseOn[2] || noiseOn[3])) return;
    static int off = 0;
    fill_noise_words(off, 32);          // 128 bytes / VBI
    off += 128;
    if (off >= NOISE_LEN) off = 0;
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
// stride.  ($40 poly5-gated poly4 is rendered as ungated poly4 — the full poly5×poly4
// period 465 won't fit — a close-enough raspy buzz for the launch-door swoosh.)
static void build_poly_one(uint8_t* dst, bool poly4, uint8_t s4, uint8_t s5, uint16_t lenBytes)
{
    uint8_t out = 0, p4 = 0, p5 = 0;
    for (uint16_t i = 0; i < lenBytes; i++) {
        p4 = (uint8_t)((p4 + s4) % 15u);   // advance polys by stride, then sample
        p5 = (uint8_t)((p5 + s5) % 31u);
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

// Render one channel's poly5-gated poly9 distortion waveform for the given per-underflow
// poly stride.  Mirrors atari800 pokeysnd.c: at each underflow the polys advance by `stride`
// (the divide-by-N count in master-clock ticks), and while the poly5 gate is open (always, if
// `gateAlways`) the 2-level output flips toward the current poly9 bit.  Output bytes are the
// same bipolar ±127 as wave_pure so AUDxVOL scales them identically.
static void build_poly_dist(uint8_t ch, uint32_t stride, bool gateAlways)
{
    uint16_t s5 = (uint16_t)(stride % 31u);
    uint16_t s9 = (uint16_t)(stride % (uint32_t)POLY9_SIZE);
    uint16_t p5 = 0, p9 = 0;
    uint8_t  out = 0;
    uint8_t* dst = poly_dist_buf[ch];
    for (int i = 0; i < POLY_DIST_LEN; i++) {
        p5 = (uint16_t)((p5 + s5) % 31u);
        p9 = (uint16_t)((p9 + s9) % (uint32_t)POLY9_SIZE);
        if (gateAlways || kBit5[p5]) {
            if (kBit9[p9] == (out ^ 1u)) out ^= 1u;   // output follows the poly9 bit
        }
        dst[i] = out ? 0x7Fu : 0x81u;
    }
}

// Shadow of POKEY registers $D200..$D20F (bus_write doesn't update mem[] for
// hardware-range writes, so we maintain our own copy here)
static uint8_t pokey[16];   // [0]=AUDF1 [1]=AUDC1 ... [8]=AUDCTL ...

// POKEY LFSR (17-bit, polynomial x^17+x^5+1; matches Platform::pokeyRandomStep).
// Exposed (extern "C") so a RANDOM-heavy native loop (fill_terrain_columns) can hold the
// state in a register for its whole run and step the LFSR inline, instead of paying a
// cross-TU rof_pokey_random() call per read.  It's our own RNG (not POKEY-cycle-accurate),
// so an ISR RANDOM read interleaving with such a loop only reshuffles cosmetic output.
extern "C" uint32_t rof_lfsr_state = 0x1FFFFu;

static uint8_t pokey_random_step(void)
{
    uint32_t bit = ((rof_lfsr_state >> 16) ^ (rof_lfsr_state >> 4)) & 1u;
    rof_lfsr_state = ((rof_lfsr_state << 1) | bit) & 0x1FFFFu;
    return (uint8_t)rof_lfsr_state;
}

uint8_t PlatformAmiga::pokeyRandom() { return pokey_random_step(); }

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

// Apply all channels recorded since the last flush.  Waveform changes are batched through a
// single DMA off → wait → on so the rasterline wait is paid once.  Called once per frame from
// game_vbi_isr, after both audio engines have recorded their POKEY writes for the frame: the
// CIA-B music tick (sfx_voice_tick_native) and the in-game SFX engine (sfx_voice_envelope_tick).
extern "C" void flush_paula(void)
{
    uint8_t valid = want_valid;
    want_valid = 0;
    if (!valid) return;

    // Split into "restart" (waveform changed) and "live" (same waveform → just VOL/PER).
    uint8_t restart = 0;
    for (uint8_t ch = 0; ch < 4; ch++) {
        if (!(valid & (1u << ch))) continue;
        if (want_ptr[ch] != cur_ptr[ch] || want_len[ch] != cur_len[ch])
            restart |= (uint8_t)(1u << ch);
        else { AUD_PER(ch) = want_per[ch]; AUD_VOL(ch) = want_vol[ch];     // live, no click
               cur_per[ch] = want_per[ch]; }
    }
    if (!restart) return;

    // The channel must stay OFF for >2 sample periods of the period STILL LOADED (the old
    // note's) or it "stays on and continues" (§5-2-7).  A PAL rasterline is ~227 Paula
    // ticks, so size the wait from the slowest old period among the restarting channels:
    // 2*per/227 lines, plus margin.  Fast old notes wait the ~7-line floor; a slow old bass
    // note (per~6011) needs ~53 lines (~3.4 ms) — fine on the title screen.
    uint16_t max_per = 0;
    for (uint8_t ch = 0; ch < 4; ch++)
        if ((restart & (1u << ch)) && cur_per[ch] > max_per) max_per = cur_per[ch];
    uint16_t wl = (uint16_t)((2u * (uint32_t)max_per) / 227u + 4u);
    if (wl < 7u)  wl = 7u;
    if (wl > 110u) wl = 110u;

    *dmaconPointer = (uint16_t)restart;            // AUDxEN off for all changed channels
    wait_rasterlines((uint8_t)wl);                 // hold off >2 OLD sample periods → resets
    for (uint8_t ch = 0; ch < 4; ch++) {
        if (!(restart & (1u << ch))) continue;
        AUD_PTR(ch) = want_ptr[ch];
        AUD_LEN(ch) = want_len[ch];
        AUD_PER(ch) = want_per[ch];
        AUD_VOL(ch) = want_vol[ch];
        cur_ptr[ch] = want_ptr[ch];
        cur_len[ch] = want_len[ch];
        cur_per[ch] = want_per[ch];
    }
    *dmaconPointer = (uint16_t)(0x8000u | restart); // AUDxEN on — all at once, one wait paid
}

// ---- POKEY→Paula frequency conversion ----------------------------------------
static uint16_t pokey_period(uint8_t ch, uint8_t audf, uint8_t audctl)
{
    static const uint32_t POKEY_CLOCK = 1789773u;
    static const uint32_t PAULA_CLOCK = 3546895u;

    uint32_t base_div = (audctl & 0x01u) ? 114u : 28u;
    bool use_179 = ((ch == 0) && (audctl & 0x20u)) ||
                   ((ch == 2) && (audctl & 0x40u));
    bool chain_lo = (ch == 0 && (audctl & 0x08u)) ||
                    (ch == 2 && (audctl & 0x10u));

    uint32_t divider;
    if (chain_lo) {
        // 16-bit chain: AUDF[lo] + 256*AUDF[hi] + 1
        uint8_t audf_hi = pokey[(ch + 1) * 2];  // next channel AUDF
        divider = (uint32_t)audf + 256u * audf_hi + 1u;
    } else {
        divider = (uint32_t)audf + 1u;
    }

    // POKEY's output flip-flop toggles once per counter underflow, so the audible
    // square wave is HALF the counted-clock rate: f = clock / (2 * divider).  Omitting
    // this ÷2 makes every voice an octave too high — most audible on the bass, whose
    // octave-up shift destroys its bass character (measured on the Standby tune: ch2
    // AUDF=$3B/15kHz reads 130.8 Hz on real POKEY but 261.6 Hz without the ÷2).  The
    // separate 2u in the Paula-period line below is the 2-sample wave_pure cycle, NOT
    // this toggle.
    uint32_t freq;
    if (use_179) {
        freq = POKEY_CLOCK / (2u * divider);
    } else {
        freq = POKEY_CLOCK / (2u * base_div * divider);
    }

    if (freq < 20u || freq > 28000u) return 0u;  // out of range → silence
                                                  // (floor 20 Hz: a deep 15kHz bass note
                                                  // is ~30 Hz after the ÷2 above)

    uint32_t per = PAULA_CLOCK / (2u * freq);
    if (per < 124u)   per = 124u;   // Paula minimum period
    if (per > 0xFFFFu) per = 0xFFFFu;
    return (uint16_t)per;
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
        vol = (audc & 0x0Fu) * 4u;  // map 0..15 → 0..60
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
            bool     use_179   = ((ch == 0) && (audctl & 0x20u)) ||
                                 ((ch == 2) && (audctl & 0x40u));
            bool     chain_lo  = (ch == 0 && (audctl & 0x08u)) ||
                                 (ch == 2 && (audctl & 0x10u));
            uint32_t divider   = chain_lo ? ((uint32_t)audf + 256u * pokey[(ch + 1) * 2] + 1u)
                                          : ((uint32_t)audf + 1u);
            uint32_t stride    = use_179 ? divider : divider * ((audctl & 0x01u) ? 114u : 28u);
            bool     gateAlways = (audc & POKEY_NOTPOLY5) != 0u;   // $80 = ungated poly9
            if (poly_dist_stride[ch] != stride ||
                poly_dist_gate[ch]   != (uint8_t)(gateAlways ? 2u : 1u)) {
                build_poly_dist(ch, stride, gateAlways);
                poly_dist_stride[ch] = stride;
                poly_dist_gate[ch]   = (uint8_t)(gateAlways ? 2u : 1u);
            }
            noiseOn[ch] = false;   // deterministic loop, not the evolving noise_buf
            want_set(ch, (uint32_t)poly_dist_buf[ch], (uint16_t)(POLY_DIST_LEN / 2), per, vol);
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
    bool poly5tone = (audc & POKEY_PURETONE) && !(audc & POKEY_NOTPOLY5);   // $20
    bool poly4     = !(audc & POKEY_PURETONE) && (audc & POKEY_POLY4);      // $C0, $40
    uint32_t sel_ptr; uint16_t sel_len;
    if (!poly5tone && !poly4) {
        sel_ptr = (uint32_t)wave_pure; sel_len = 1u;       // pure tone / unmodelled
    } else {
        uint16_t baseDiv = (audctl & 0x01u) ? 114u : 28u;
        uint32_t stride  = (uint32_t)(audf + 1u) * baseDiv;
        if (poly4) {
            sel_ptr = (uint32_t)poly4_wave[stride % 15u]; sel_len = 15u;   // 30 bytes
        } else {
            sel_ptr = (uint32_t)poly5_wave[stride % 31u]; sel_len = 31u;   // 62 bytes
        }
    }
    want_set(ch, sel_ptr, sel_len, per ? per : 124u, vol);
}

// Forward declaration — defined in SfxPlayer.cpp
extern "C" void sfx_seq_step_native(void);

// ---- public interface --------------------------------------------------------
// mem[] has already been populated (load_xex_image, called from main before the scene
// initialises); this only sets up the Paula side.
void PlatformAmiga::audioInit()
{
    // Clear POKEY shadow and LFSR
    for (int i = 0; i < 16; i++) pokey[i] = 0;
    rof_lfsr_state = 0x1FFFFu;
    fill_noise_buf();   // pre-render the poly17 noise sample for noise-distortion voices
    build_poly_tables(); // pre-render every distinct poly distortion waveform (immutable)

    // SFX is initialised by the mem[$0090] gate in RescueOnFractalus::update():
    // the snapshot has $0090=1, so the first update() call resets $073C/$073A
    // to start the sequence from note 0 — no replay needed here.

    // Point all Paula channels at the pure-tone wave, start silent.
    // update_paula_channel() re-points each channel per its AUDC distortion.
    for (int ch = 0; ch < 4; ch++) {
        AUD_PTR(ch) = (uint32_t)wave_pure;
        AUD_LEN(ch) = 1u;   // 1 word = 2 bytes
        AUD_PER(ch) = 256u; // default period, updated by POKEY writes
        AUD_VOL(ch) = 0u;   // silent until AUDC writes activate channels
    }

    // Enable audio DMA for all 4 channels (DMAF_AUD0..3 = bits 0..3)
    *dmaconPointer = (uint16_t)(DMAF_SETCLR | 0x000Fu);
}

void PlatformAmiga::audioShutdown()
{
    // Silence and disable audio DMA
    for (int ch = 0; ch < 4; ch++) AUD_VOL(ch) = 0u;
    *dmaconPointer = 0x000Fu;  // clear AUD0..3 (no SETCLR = clear)
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

uint8_t PlatformAmiga::hwRead(uint16_t addr)
{
    if (addr == 0xD20Au) return pokey_random_step();  // POKEY RANDOM register
    // SKSTAT ($D20F read) is a STATUS register, distinct from the SKCTL we WRITE to the
    // same address.  The standby level-select (engine_sound_update $5978, reached via the
    // Title Screen) reads bit3 = the SHIFT key, ACTIVE-LOW (1 = NOT pressed): shift idle →
    // the level-INCREMENT path (joystick-up / SELECT raise the starting level), shift held →
    // decrement.  Returning the POKEY write-shadow (pokey[$F], bit3=0) reads as "shift held",
    // so the code always decremented → joystick-up appeared to do nothing / wrong direction.
    // Emit a proper idle SKSTAT (nothing pressed) so up=raise, down=lower, matching the Atari.
    if (addr == 0xD20Fu) return 0xFFu;                // SKSTAT: idle (bit3=1 = shift not pressed)
    if (addr >= 0xD200u && addr < 0xD210u) return pokey[addr - 0xD200u];
    // PIA PORTA ($D300): Atari joysticks are ACTIVE-LOW (1 = open/neutral).  Driven by the
    // keyboard ISR from the Amiga arrow keys (stick-0 bits 0-3); neutral $FF = stick centred.
    if (addr == 0xD300u) return s_portaState;
    // CONSOL ($D01F): console keys (START/SELECT/OPTION), ACTIVE-LOW (bit clear =
    // pressed).  The Amiga keyboard handler maintains it in mem[$D01F] (idle $07;
    // RETURN clears a bit for START).  Reflect that — falling through to 0 reads as
    // "all console keys held", so the genuine attract idle loop sees START/launch
    // pressed and auto-starts the DEMO DROID demo within seconds.  ($D01F writes go
    // through platform_hw_write, which drops non-POKEY addresses, so the keyboard's
    // mem[$D01F] is never clobbered by genuine code.)
    if (addr == 0xD01Fu) return mem[0xD01Fu];
    // TRIG0-3 ($D010-$D013): joystick fire buttons, ACTIVE-LOW ($01 = released,
    // $00 = pressed).  TRIG0 is driven by the keyboard ISR from the Control key; the
    // others stay released.  (Default $01 also matters at Standby: read_console_trig_delta
    // $5A78 computes (CONSOL&$01) - TRIG0, so a stuck TRIG0=0 would auto-launch the game.)
    if (addr == 0xD010u) return s_trig0State;
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

void PlatformAmiga::hwWrite(uint16_t addr, uint8_t val)
{
    if (addr < 0xD200u || addr >= 0xD210u) return;  // only POKEY range
    uint8_t reg = (uint8_t)(addr - 0xD200u);
    // Change-detect: the 50Hz SFX envelope engine rewrites AUDF/AUDC every tick, often with
    // the same value; recomputing the Paula channel (period divide + waveform select) for an
    // unchanged register is pure waste.  Skip it when nothing changed.
    if (pokey[reg] == val) { mem[addr] = val; return; }
    pokey[reg] = val;
    mem[addr]  = val;   // keep Atari-RAM mirror in sync (matches transpile bus_write)

    if (reg <= 7u) {
        // AUDF or AUDC write — update the affected channel
        uint8_t ch = reg >> 1u;
        update_paula_channel(ch);
    } else if (reg == 8u) {
        // AUDCTL — recompute all channel periods
        for (uint8_t ch = 0; ch < 4; ch++) update_paula_channel(ch);
    }
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
static volatile uint16_t g_vbiCount = 0;
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

// Flight/init timing probes (enable with `make PROBES=1` → -DROF_FLIGHT_PROBE).  Sub-frame
// clock rof_subclock() = g_vbiCount*313 + beam_line, plus the accumulators that rof_native.c's
// FP_* macros and the renderFrame/atmosphere probes below write into.  All read from the gdb
// stub (amiga/diag_timing.gdb).  Compiled out by default so the SDL/release builds don't carry
// (or need to link) any of these symbols.
#ifdef ROF_FLIGHT_PROBE
extern "C" unsigned long rof_subclock(void) {
    return (unsigned long)g_vbiCount * 313u + (unsigned long)rof_beam_line();
}
extern "C" volatile unsigned long g_probeDispSetup = 0, g_probeGameInit = 0,
    g_probeIntro = 0, g_probeRowAddr = 0, g_probeInitTotal = 0;
extern "C" volatile unsigned short g_probeFlightVbi = 0;  // g_vbiCount at flight VBI install
// Flight-entry -> terrain-fade-start window decomposition (VBI frames vs game-loop iters).
extern "C" volatile unsigned short g_fadeLoopVbi = 0;   // vbi when flight loop (iterCount>=1) starts
extern "C" volatile unsigned short g_fadeEntryVbi = 0, g_fadeStartVbi = 0;
extern "C" volatile unsigned short g_fadeEntryIter = 0, g_fadeStartIter = 0;
extern "C" volatile unsigned short g_fadeEntryFd = 0, g_fadeStartFd = 0;
extern "C" volatile unsigned char g_fadeEntryDC = 0, g_fadeDone = 0;
extern "C" volatile unsigned char g_fadeEntryState = 0, g_fadeStartState = 0;   // mem[0x41]
extern "C" volatile unsigned char g_fadeEntry66C = 0, g_fadeStart66C = 0;       // mem[0x66C]
extern "C" volatile unsigned char g_fadeEntryAlt = 0, g_fadeStartAlt = 0;       // mem[0x34]
// renderFrame() no-yield-gap probe:
extern "C" volatile unsigned short g_maxRenderGap = 0, g_maxGapAtVbi = 0, g_maxGapVvblki = 0;
extern "C" volatile unsigned char g_maxGap060B = 0, g_maxGap004A = 0;
// Cinematic-only render-gap probe (launch VBI $52D7 active): isolates a tunnel->stars freeze.
extern "C" volatile unsigned short g_maxCineGap = 0, g_maxCineGapAtVbi = 0;
extern "C" volatile unsigned char g_maxCineGap060B = 0;
extern "C" volatile unsigned short g_csGap = 0, g_csGapAtVbi = 0;   // tunnel->stars window only
// standby->doors window render-gap probe (no vbi>360 gate, so it catches the early launch burst)
extern "C" volatile unsigned short g_doorGap = 0, g_doorGapAtVbi = 0;
extern "C" volatile unsigned char g_doorGap060B = 0;
// stretch-A per-function one-shot subclock deltas (ticks): find the standby->doors freeze.
extern "C" volatile unsigned long g_saTicks[16] = {0};
// door-frame draw pixel-volume counters (span calls + total bytes/edges written).
extern "C" volatile unsigned long g_dfVCalls = 0, g_dfVRows = 0, g_dfHCalls = 0, g_dfHCols = 0;
// decodeCockpitFull one-shot timing (chip-vs-fast-RAM experiment).
extern "C" volatile unsigned long g_ckFullTicks = 0, g_ckFullCount = 0;
// fill_terrain_columns one-shot timing (tunnel->stars setup gap).
extern "C" volatile unsigned long g_fillTerrTicks = 0, g_fillTerrIsr = 0;
// display_setup launch-tail milestone stamps: rof_ds_mile(i) records g_vbiCount at milestone i,
// so a big jump between consecutive stamps localises the ~580ms cinematic freeze to one stretch.
extern "C" volatile unsigned short g_dsMile[16] = {0};
extern "C" volatile unsigned long g_burstClrTicks = 0, g_burstClrIsr = 0;   // L_650b field-clear cost
extern "C" volatile unsigned long g_burstMidTicks = 0, g_burstMidIsr = 0;
extern "C" volatile unsigned long g_sbATicks = 0, g_sbAIsr = 0, g_sbCTicks = 0, g_sbCIsr = 0;
extern "C" void rof_ds_mile(int i) { if (i >= 0 && i < 16) g_dsMile[i] = g_vbiCount; }
// tunnel-prebuild probe: does the standby-construction ring draw (7262) run, and does its
// mem[$1000] ring field survive to the launch-time redraw (7601)?  (checksums + run flags)
// planet-approach spike probe (L_6578 loop): max advance_object_positions cost + max
// renderViewportModeD dirty-band decode (rows + ticks), each with the vbi it peaked at.
extern "C" volatile unsigned long g_aopMax = 0, g_aopMaxVbi = 0;
extern "C" volatile unsigned long g_vpDecMax = 0, g_vpDecMaxVbi = 0, g_vpDecMaxRows = 0;
// standby->doors gap localizer: stretch-A code sets g_saPhase as it progresses; renderFrame
// snapshots it (g_doorGapPhase) at the worst door-window gap so we know which phase preceded it.
extern "C" volatile unsigned char g_saPhase = 0, g_doorGapPhase = 0;
extern "C" volatile unsigned long g_rbMax = 0; extern "C" volatile unsigned short g_rbMaxVbi = 0;
// RTCLOK ownership-race probe: catch frames where RTCLOK ($0014) is advanced by BOTH the VBI
// body AND renderFrame (double-count -> equality spin-waits overshoot -> ~256-frame wrap), and
// frames where renderFrame read a "torn"/unexpected VVBLKI vector during the $52D7<->$4FF5 swap.
extern "C" volatile unsigned short g_rtDoubleCount = 0, g_rtDoubleAtVbi = 0;
extern "C" volatile unsigned short g_rtZeroCount = 0, g_rtZeroAtVbi = 0;
extern "C" volatile unsigned short g_rtTornCount = 0, g_rtLastTornVec = 0, g_rtTornAtVbi = 0;
// RTCLOK SKIP probe: while $4FF5 is active, renderFrame does NOT advance RTCLOK — the ISR does,
// once per REAL VBI.  If a single renderFrame spans >1 real VBI (slow render), RTCLOK jumps by
// >1 across one equality-spin iteration ($3CB8 push_a_thunk), which can step OVER the target and
// wrap 256 ticks.  Track the max single-iteration RTCLOK delta + how many iterations jumped >1.
extern "C" volatile unsigned short g_rtJumpMax = 0, g_rtJumpGt1Count = 0, g_rtJumpAtVbi = 0;
// VCOUNT busy-wait span probe (see pollEvents): longest run of frames a non-frame-pacing
// spin (wait_vcount_eq etc.) holds without a renderFrame — a big value = the equality miss.
extern "C" volatile unsigned short g_maxPollSpinFrames = 0, g_maxPollSpinAtVbi = 0, g_pollSpinStartVbi = 0;
static bool g_pollAfterRender = false;
// game_main_loop per-iteration + flight phase split (written by rof_native.c FP_* macros):
extern "C" volatile unsigned long g_iterMax = 0, g_iterLast = 0, g_iterPostDs = 0;
extern "C" volatile unsigned short g_iterCount = 0, g_iterMaxAt = 0;
extern "C" volatile unsigned long g_fSetup=0,g_fClear=0,g_fDraw=0,g_fColl=0,g_fState=0,g_fEnemy=0;
// Stage-0 convert-pass cost (flight renderViewportModeD), beam-based, ISR-decontaminated.
extern "C" volatile unsigned long g_fConvert=0;
// atmosphere terrain-pen range during flight ($00DC/$00DD salmon→brown fade):
extern "C" volatile unsigned char g_dcMin=0xFF, g_dcMax=0, g_ddMin=0xFF, g_ddMax=0;
// Cockpit-decode probe (beam sub-frame ticks): g_fCockpit accumulates the time spent in the
// render() cockpit scan/decode block; g_fCockpitScans counts the frames it actually ran.
extern "C" volatile unsigned long g_fCockpit=0, g_fCockpitScans=0;
// VBI-body sub-profiling (beam-line deltas inside the flight VBI; normalize by isrCalls).
// integ/proj/sfx wrap individual native twins (rof_native.c).  The whole handler is timed by
// flight_vbi_native (g_flightProf.isrLines).  (The old top/atmo/hud/score/tail PRE_INSN_HOOK
// partition was retired when vbi_handler_flight went native — it had done its diagnostic job.)
extern "C" volatile unsigned long g_pProj=0, g_pInteg=0, g_pSfx=0;
// VBI handler section partition (the chunks NOT covered by integ/proj/sfx; see rof_native.c
// vbi_handler_flight).  Per-call = acc/isrCalls; sum(all sections)+integ+proj ≈ isrLines.
extern "C" volatile unsigned long g_pDrawBr=0, g_pSimHead=0, g_pAtmo=0, g_pHud=0, g_pScore=0, g_pTail=0;
// Stage-1 verifier: chip addr of terrainBitmap->data, so the gdb harness can dump the
// flight bitplanes and decode/diff them headlessly (no display needed).
extern "C" volatile uint32_t g_terrainBmpAddr=0;
// Stage-1 direct renderer's parallel bitmap (for pixel-diff vs the convert).
extern "C" volatile uint32_t g_flightDirectAddr=0;
// Stage-1 direct-render beam cost (same units as g_fConvert) for the head-to-head.
extern "C" volatile unsigned long g_fDirect=0;
// renderFlightDirect internal breakdown (beam ticks, accumulated over g_fdCalls):
// clear+wait / edge plot / fill+wait / plane2 scan / band convert / scanned-row count.
extern "C" volatile unsigned long g_fdClear=0, g_fdEdge=0, g_fdFill=0, g_fdScan=0,
                                   g_fdBand=0, g_fdCalls=0, g_fdScanRows=0;
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
extern "C" volatile unsigned long g_rRenderCompute=0, g_rRenderWall=0, g_rIdleWall=0, g_rCalls=0;
extern "C" volatile unsigned long g_rPerFrame=0, g_rRenderFn=0, g_rCopper=0;
// Altimeter sprite chip addresses (set in initialize) so the gdb harness can read their VSTART/
// VSTOP control words and confirm the bar Y vs mem[$281A]/$281B.
extern "C" volatile uint32_t g_altimSprAddr=0, g_altimShipSprAddr=0, g_energySprAddr=0;
extern "C" volatile uint32_t g_viewportP3SprAddr=0, g_scopeP3SprAddr=0, g_flightCopperAddr=0;
#endif

// g_quitJmp: the __builtin_setjmp buffer armed by RescueOnFractalus::run() so we can
// unwind the never-returning transpiled chain on quit (5 words per the GCC builtin;
// initializer forces the definition).  extern "C" — RescueOnFractalus.cpp references it.
extern "C" void* g_quitJmp[5] = { 0, 0, 0, 0, 0 };

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
    }
    unsigned long _ri0 = rof_subclock();
#endif
    while (g_vbiCount == last) { /* wait for next real VBI */ }
#ifdef ROF_FLIGHT_PROBE
    if (_rFlight) g_rIdleWall += rof_subclock() - _ri0;
#endif
    // Flight double-buffer: the flip in render() has now latched (the just-painted buffer is on
    // screen).  Kick the blitter clear of the OTHER (now off-screen) buffer so it overlaps the
    // upcoming terrain draw instead of running serially inside the next convert.  No-op off flight.
    if (s_scene) s_scene->flightKickBackClear();
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
    // RTCLOK ownership: the ATTRACT ($1B30) and full flight ($4FF5) VBIs advance RTCLOK
    // ($0014) in their own transpiled bodies, so skip here to avoid double-counting.  The
    // standby/cinematic ($52D7) body does not, so renderFrame owns it there.
    if (vbiVec != 0x1B30u && vbiVec != 0x4FF5u) {
        mem[0x0014]++;
        if (!mem[0x0014]) mem[0x0013]++;
    }
    if (AmigaHardware::isLeftMouseButtonPressed()) g_pumpQuit = 1;
    if (g_pumpQuit) __builtin_longjmp(g_quitJmp, 1);
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
    if (AmigaHardware::isLeftMouseButtonPressed()) g_pumpQuit = 1;
    if (g_pumpQuit) __builtin_longjmp(g_quitJmp, 1);
}

// tickVBI: no-op — RTCLOK is advanced by renderFrame() after each VBI wait.
void PlatformAmiga::tickVBI() {}

// The tunnel-ring "dirty field" flags draw_ring_frame_step already uses to stream the
// ring-clear frames from the $1000 GTIA field into the tunnel bitmap (NativeHandlers.cpp).
extern "C" volatile uint8_t g_tunnelFieldDirty;
extern "C" volatile uint8_t g_tunRowLo, g_tunRowHi, g_tunBandMode;
void PlatformAmiga::tunnelRingsDrawn() {
    // display_setup's draw_frame_pattern_seq just rendered the full ring pattern into the
    // $1000 field.  Flag the whole field dirty (band mode 0 = full extent) so the next
    // renderFrame decodes it once into the tunnel bitmap (then draw_ring_frame_step publishes
    // the per-frame band updates in mode 1).
    g_tunRowLo = 0; g_tunRowHi = 85; g_tunBandMode = 0;
    g_tunnelFieldDirty = 1;
}

extern "C" void rof_cockpit_dial_dirty(unsigned short addr);
void PlatformAmiga::cockpitDirty(uint16_t addr, uint8_t nCells) {
    // The shared-code cockpit writer is the dial bars (draw_object_column); grow the dial
    // bounding box so render() re-decodes just that footprint (writer-driven, no full scan).
    for (uint8_t i = 0; i < nCells; i++) rof_cockpit_dial_dirty((uint16_t)(addr + i));
}

extern "C" volatile unsigned char g_compassDirty;
void PlatformAmiga::compassChanged() {
    // The compass heading cells $32E3-$32E6 were rewritten (housing or heading glyph).
    // Flag them so the next renderFrame re-decodes just those 4 mode-4 cells.
    g_compassDirty = 1;
}

extern "C" volatile unsigned char g_ckLockon;
void PlatformAmiga::lockonChanged() {
    // The enemy lock-on indicator cells $3491-$3497 were rewritten by the native lock-on
    // animation twins (lock_on_indicator_tick / _step / _write_cell / lock_on_indicator_fill_cells in
    // rof_native.c, driven by both the standby and flight VBIs).  Flag them so the next
    // renderFrame re-decodes those 7 cells — this keeps the indicator randomly blinking
    // through the planet descent and into flight.
    g_ckLockon = 1;
}

extern "C" volatile int g_titleToRender;
void PlatformAmiga::titleChanged() {
    // copy_title_text_block_to_screen ($782A) just rewrote the banner text in $32B7-$32CA
    // (the SFX sequencer alternates the block via $0091).  Ask the next renderFrame to repaint
    // all 20 title cells straight from screen RAM (whichever of "RESCUE ON FRACTALUS!" / the
    // copyright line is now there).
    g_titleToRender = 20;
}

// Pending in-flight command keycode set by the keyboard ISR (keyboardHandler, below),
// consumed by the flight VBI through flightIrqKey().  $FF = none.  Volatile: written in
// the SP interrupt, read on the main thread (mirrors the Atari's X-register handoff out
// of the IRQ).  Declared here so flightIrqKey() — defined before the keyboard section —
// can see it.
static volatile uint8_t s_pendingFlightKey = 0xFF;

uint8_t PlatformAmiga::flightIrqKey() {
    // Consume the keycode the keyboard ISR stashed (if any) and reset to "none".  The flight
    // VBI's CLI window ($519c) calls this once per frame; returning the code here is exactly
    // the Atari IRQ leaving KBCODE&$3F (or $80=BREAK) in X for event_sequence_dispatcher.
    uint8_t k = s_pendingFlightKey;
    s_pendingFlightKey = 0xFF;
    return k;
}

// ============================================================================
//  CIA-A serial-port keyboard — RETURN -> Atari START switch (CONSOL $D01F)
// ============================================================================
// The Amiga keyboard shifts each keycode into CIA-A's serial data register, raising
// the CIA-A SP interrupt (CIAICRB_SP, via INTB_PORTS).  We hang a handler on that
// vector through ciaa.resource (the same AddICRVector mechanism the CIA-B music tick
// uses).  keyboard.device normally owns the vector, so we steal it (saving the
// previous) and restore it on shutdown, leaving the OS keyboard working afterwards.
//
// This handler IS the Atari console-switch hardware abstraction: it maps RETURN onto
// the START switch in CONSOL ($D01F / 53279), writing $06 while RETURN is held and
// $07 when idle.  CONSOL reads active-low in bits 0-2 (START/SELECT/OPTION); idle = $07,
// START down clears bit0 -> $06, the value the genuine attract poll
// (station_poll_start_native) tests for.  We never touch SELECT/OPTION.
static const uint16_t kConsol      = 0xD01F;
static const uint8_t  kConsolIdle  = 0x07;
static const uint8_t  kConsolStart = 0x06;
static const uint8_t  kRawReturn   = 0x44;   // RETURN rawkey (cf. RETURN=$44, ESC=$45)

// In-flight keyboard commands.  On the Atari these arrive as a POKEY keyboard/BREAK
// IRQ (IRQEN=$C0) whose handler (irq_handler $462A) leaves the event id — KBCODE&$3F,
// or $80 for BREAK — in X; the flight VBI's CLI window ($519c) then runs
// event_sequence_dispatcher ($4644), which matches X against tbl $4816 and dispatches
// by mode $0072.  We model that here: map the Amiga rawkey of each command key to its
// Atari KBCODE and stash it (one-shot) for the flight VBI to consume via flightIrqKey().
// The dispatcher itself filters unknown ids, so only the 8 table keys do anything.
//   Atari KBCODE -> command (event_sequence_dispatcher index Y), from the game manual:
//     $00 L (Y0) Land     $3f A (Y1) Air Lock    $15 B (Y2) Boosters    $3e S (Y3) Systems
//     $07 * (Y4) Decrease Thrust (DEC $006F)      $06 + (Y5) Increase Thrust (INC $006F, cap 6)
//     $80 BREAK ($519e -> game_loop_reset = "Restart")    $1c ESC (Y7) Freeze/pause
//   ($07/$06 are the Atari cursor-right/left keys, masked to base code by the IRQ's AND #$3F.
//    We put thrust on '.'/',' so the Amiga arrow keys stay free for the joystick directions.)
struct FlightKeyMap { uint8_t rawkey; uint8_t kbcode; };
static const FlightKeyMap kFlightKeys[] = {
    { 0x28, 0x00 },   // Amiga 'L'   -> Atari L   $00  Land
    { 0x20, 0x3F },   // Amiga 'A'   -> Atari A   $3f  Air Lock
    { 0x35, 0x15 },   // Amiga 'B'   -> Atari B   $15  Boosters
    { 0x21, 0x3E },   // Amiga 'S'   -> Atari S   $3e  Systems
    { 0x39, 0x06 },   // Amiga '.'   -> Atari +   $06  Increase Thrust
    { 0x38, 0x07 },   // Amiga ','   -> Atari *   $07  Decrease Thrust
    { 0x45, 0x1C },   // Amiga 'Esc' -> Atari ESC $1c  Freeze/pause
    { 0x46, 0x80 },   // Amiga 'Del' -> Atari BREAK $80 Restart
};

// Amiga rawkeys for the held joystick/fire inputs (driven into s_portaState/s_trig0State).
static const uint8_t kRawUp        = 0x4C;
static const uint8_t kRawDown      = 0x4D;
static const uint8_t kRawRight     = 0x4E;
static const uint8_t kRawLeft      = 0x4F;
static const uint8_t kRawFire      = 0x60;   // Left Shift = fire button (TRIG0)

static struct Library*   s_ciaaBase    = 0;
static struct Interrupt  s_kbInterrupt;
static struct Interrupt* s_savedVector = 0;   // keyboard.device's vector, restored on exit

// CIA-A SP interrupt: a full keycode has shifted into the serial register.  ciaa.resource
// has already read+cleared the ICR before dispatching us, so we only touch the serial data
// register (read the code) and CRA (handshake).
static uint32_t keyboardHandler()
{
    uint8_t sdr = *ciaasdrPointer;

    // Acknowledge: pulse SP to output mode (drives KDAT low) then back to input, so the
    // keyboard releases the next keycode.  HRM Appendix G (node G-2): "Software MUST pulse
    // the line low for 85 microseconds"; resync timeout is 143 ms, so this ~2 ms busy-wait
    // is safe.
    *ciaacraPointer |= CIACRAF_SPMODE;
    for (volatile uint16_t d = 0; d < 1500; d++) { /* >=85us handshake */ }
    *ciaacraPointer &= (uint8_t)~CIACRAF_SPMODE;

    // Wire protocol (HRM Appendix G): the keycode is sent ROL'd one bit and KDAT is
    // active-low, so SDR holds ~(keycode ROL 1).  Recover by inverting then ROR 1.  Bit 7
    // of the result = key-up flag (0 = down).
    uint8_t code = (uint8_t)~sdr;
    code = (uint8_t)((code >> 1) | (code << 7));   // ROR 1
    uint8_t raw  = (uint8_t)(code & 0x7Fu);
    bool    down = (code & 0x80u) == 0u;

    // Drive the CONSOL START switch (bit0) from RETURN's down/up edges, so the register
    // continuously reflects the key's level — just like the real GTIA switch.
    if (raw == kRawReturn) {
        mem[kConsol] = down ? kConsolStart : kConsolIdle;
        return 0;
    }

    // Held joystick/fire inputs — track the active-low PORTA/TRIG0 level across down/up
    // edges (pressed = clear the bit).  Arrows = stick-0 directions; Control = fire.
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
        for (unsigned i = 0; i < sizeof(kFlightKeys) / sizeof(kFlightKeys[0]); i++)
            if (kFlightKeys[i].rawkey == raw) {
                s_pendingFlightKey = kFlightKeys[i].kbcode;
                break;
            }
    }
    return 0;
}

static bool keyboardInit()
{
    s_ciaaBase = (struct Library*)OpenResource((UBYTE*)CIAANAME);
    if (!s_ciaaBase) return false;

    mem[kConsol] = kConsolIdle;   // power-on CONSOL state: no switch down (START up)

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
//  Real INTB_VERTB VBI server — the per-frame VBI body + RTCLOK clock
// ============================================================================
static struct Interrupt vbiServer;

// Real vertical-blank frame counter (50 Hz PAL), exposed to the scene so time-based
// animations (e.g. the flight terrain colour fade) run at wall-clock rate regardless of
// how fast the main render loop iterates.
extern "C" unsigned short platform_frame_count(void) { return g_vbiCount; }

static uint32_t vbiHandler()
{
    // RTCLOK is owned by renderFrame() in the main thread (advanced exactly once per
    // spin-wait iteration, immune to ISR timing races with the equality spin).
    // Exception: ATTRACT VBI ($1B30) bumps RTCLOK in its own transpiled body.
    // (Do NOT touch $0080 — sync_flag, reused as the $80/$81 zp pointer.)
    g_vbiCount++;

    // Flight terrain double-buffer swap — do this FIRST, while the beam is still in vertical
    // blank (well above the viewport WAIT at scanline 85).  If renderFlightDirect has published a
    // freshly-painted buffer, rewrite the copper's viewport bitplane pointers now so the copper
    // fetches the new buffer this frame with no torn pointer (the brown-flash cause).  No-op unless
    // a swap is pending; clears the flag so the main thread's busy-wait can proceed.
    // Zero-copy starfield scroll — write the fresh star-sprite control words + convert the new
    // rows into the ring, at the very TOP of the vblank ISR so the control words are in place well
    // before the sprite's (early) control DMA fetch — otherwise a stale read corrupts VSTOP and
    // drops the channel-2 gauge.  No-op unless we're in the stars/planet phase.  (flightVblankSwap
    // is a no-op during stars and vice-versa, so ordering between them is free.)
    if (s_scene) s_scene->starVblankUpdate();

    if (s_scene) s_scene->flightVblankSwap();

#ifdef ROF_FLIGHT_PROBE
    // Probe: track the range of the atmosphere terrain pens ($00DC/$00DD) during flight to
    // confirm they ramp (salmon→brown fade) vs stay frozen.
    {
        extern volatile unsigned char g_dcMin, g_dcMax, g_ddMin, g_ddMax;
        if ((mem[0x0222] | (mem[0x0223] << 8)) == 0x4FF5u) {
            uint8_t dc = mem[0x00DC], dd = mem[0x00DD];
            if (dc < g_dcMin) g_dcMin = dc; if (dc > g_dcMax) g_dcMax = dc;
            if (dd < g_ddMin) g_ddMin = dd; if (dd > g_ddMax) g_ddMax = dd;
        }
    }

#ifdef ROF_AUTO_FIRE
    // Auto-fire probe (capture only): once the flight VBI ($4FF5) is live, HOLD the trigger
    // ($D010=0, active-low) so the player laser fires continuously.  A held trigger auto-repeats
    // (the $5178 fire path re-arms whenever $0036 returns to 0), so snapshotting mem[] at varying
    // delays catches every phase of the shot animation (travel scale/pos + impact).
    if ((mem[0x0222] | (mem[0x0223] << 8)) == 0x4FF5u) s_trig0State = 0x00u;
#endif

    // Auto-launch: replicate a RETURN/START press once Standby's idle loop is actually
    // polling CONSOL.  A fixed vbi==350 fired before display_setup's standby poll was live
    // (g_standbyRevealReady latches at display_setup entry, when the idle loop starts), so the
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
                if (mem[0x060Bu] != 0x23u) mem[0xD01Fu] = 0x06;   // START held until launched
                else                       mem[0xD01Fu] = 0x07;   // launched → release
            }
#else
            (void)d;   // manual-launch measurement: the player presses START themselves
#endif
#endif
        }
    }
#endif

    // Per-frame VBI body — run in the REAL vertical-blank interrupt, where the Atari ran
    // its VBI.  game_vbi_isr() dispatches by the live VVBLKI vector to the standby ($52D7)
    // or flight ($4FF5) body — as the Atari swaps VVBLKI — bracketing the work in a
    // save/restore of the shared 6502 register file (the main loop may be mid-instruction
    // using `cpu` when this interrupt preempts it).
    game_vbi_isr();
    return 0;
}

// ============================================================================
//  CIA-B Timer A interrupt — SFX music tick at 25 Hz
// ============================================================================
// The Atari SFX sequencer ticks every other VBI (the BIT $062D gate, $00E7=1) = 25 Hz.
// On the Amiga we drive it from CIA-B Timer A at that exact rate, off a dedicated
// hardware interrupt rather than the VBI/render loop, so the music tempo is independent
// of frame timing.  CIA-B uses the Amiga E-clock (~709379 Hz PAL); period = 709379/25 =
// 28375.  The interrupt fires through INTB_EXTER (level 6) via the ciab.resource.
static struct Library  *CIABBase;
static struct Interrupt sfxTimer;

static uint32_t sfxTimerHandler()
{
    if (mem[0x00E7]) sfx_voice_tick_native();
    return 0;
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

    // Publish the global Platform* the C bridge (platform_cbridge.cpp) dispatches through.
    platform = this;
}

PlatformAmiga::~PlatformAmiga()
{
    if (GfxBase) { CloseLibrary((struct Library*)GfxBase); GfxBase = 0; }
}

void PlatformAmiga::run()
{
    // The scene holds several KB of shadow buffers; keep it in BSS (static), NOT on the
    // stack (which the PlatformAmiga instance lives on in main), to avoid stack overflow.
    static RescueOnFractalus scene;

    // --- takeover: save system state, disable OS display ---------------------
    struct View* savedView = GfxBase->ActiView;
    LoadView(NULL);
    WaitTOF();
    WaitTOF();

    // Disable raster (bitplane) and sprite DMA so old state doesn't leak through.  Keep
    // exec's disk/blitter/audio DMA as-is; copper DMA gets re-enabled below.
    *dmaconPointer = (uint16_t)(DMAF_RASTER | DMAF_SPRITE | DMAF_COPPER);

    // Display window — standard PAL lores 320x200 visible area.  No bitplanes (bplcon0=0):
    // the whole area shows COLOR00 (copper-set background).
    *diwstrtPointer = 0x2c81;   // VSTRT=44, HSTRT=0x81
    *diwstopPointer = 0xf4c1;   // VSTOP=244, HSTOP=0xc1 (+256 implicit)
    *ddfstrtPointer = 0x0038;
    *ddfstopPointer = 0x00d0;
    *bplcon0Pointer = 0x0000;   // 0 bitplanes
    *bplcon1Pointer = 0x0000;
    *bplcon2Pointer = 0x0000;

    // --- VBI interrupt server ------------------------------------------------
    vbiServer.is_Node.ln_Type = NT_INTERRUPT;
    vbiServer.is_Node.ln_Pri  = 0;
    vbiServer.is_Node.ln_Name = (char*)"RoF VBI";
    vbiServer.is_Data = 0;
    vbiServer.is_Code = (void(*)())vbiHandler;
    AddIntServer(INTB_VERTB, &vbiServer);

    // --- CIA-B Timer A — SFX music at 25 Hz ----------------------------------
    // Use ciab.resource so the CIA ICR is demultiplexed for us.
    CIABBase = (struct Library*)OpenResource((UBYTE*)CIABNAME);
    if (CIABBase) {
        sfxTimer.is_Node.ln_Type = NT_INTERRUPT;
        sfxTimer.is_Node.ln_Pri  = 0;
        sfxTimer.is_Node.ln_Name = (char*)"RoF SFX";
        sfxTimer.is_Data = 0;
        sfxTimer.is_Code = (void(*)())sfxTimerHandler;
        if (!AddICRVector(CIABBase, CIAICRB_TA, &sfxTimer)) {
            Disable();
            // Stop timer, load period (28375 = 0x6EC7 = 709379/25), continuous mode.
            *((volatile uint8_t*)(ciab + ciacra)) &= (uint8_t)~CIACRAF_START;
            *((volatile uint8_t*)(ciab + ciatalo)) = (uint8_t)(28375 & 0xFF);
            *((volatile uint8_t*)(ciab + ciatahi)) = (uint8_t)(28375 >> 8);
            *((volatile uint8_t*)(ciab + ciacra)) =
                (uint8_t)((*((volatile uint8_t*)(ciab + ciacra))
                           & ~(CIACRAF_RUNMODE | CIACRAF_PBON | CIACRAF_OUTMODE
                               | CIACRAF_SPMODE | CIACRAF_TODIN))
                          | CIACRAF_START);
            Enable();
        }
    }

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

    keyboardInit();       // RETURN = START for the launch cinematic

    // --- run -----------------------------------------------------------------
    // The whole game runs inside scene.run(): the genuine transpiled/native boot chain,
    // whose frame-wait spin loops each call platform_render_frame (render + wait on
    // g_vbiCount).  Returns when the user quits (left mouse button).
    scene.run();

    // --- restore system ------------------------------------------------------
    keyboardShutdown();
    scene.shutdown();     // calls PlatformAmiga::audioShutdown

    if (CIABBase) {
        Disable();
        *((volatile uint8_t*)(ciab + ciacra)) &= (uint8_t)~CIACRAF_START;
        Enable();
        RemICRVector(CIABBase, CIAICRB_TA, &sfxTimer);
    }
    RemIntServer(INTB_VERTB, &vbiServer);

    // Disable our display DMA before handing back.
    *dmaconPointer = (uint16_t)(DMAF_COPPER | DMAF_RASTER | DMAF_SPRITE);

    LoadView(savedView);
    WaitTOF();
    WaitTOF();
}
