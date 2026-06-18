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
}

// Shadow of POKEY registers $D200..$D20F (bus_write doesn't update mem[] for
// hardware-range writes, so we maintain our own copy here)
static uint8_t pokey[16];   // [0]=AUDF1 [1]=AUDC1 ... [8]=AUDCTL ...

// POKEY LFSR (17-bit, polynomial x^17+x^5+1; matches Platform::pokeyRandomStep)
static uint32_t lfsr_state = 0x1FFFFu;

static uint8_t pokey_random_step(void)
{
    uint32_t bit = ((lfsr_state >> 16) ^ (lfsr_state >> 4)) & 1u;
    lfsr_state = ((lfsr_state << 1) | bit) & 0x1FFFFu;
    return (uint8_t)lfsr_state;
}

uint8_t PlatformAmiga::pokeyRandom() { return pokey_random_step(); }

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
// CIA-B music tick (sfx_voice_tick_native) and the in-game SFX engine (update_gauge_digits).
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

    uint32_t freq;
    if (use_179) {
        freq = POKEY_CLOCK / divider;
    } else {
        freq = POKEY_CLOCK / (base_div * divider);
    }

    if (freq < 50u || freq > 28000u) return 0u;  // out of range → silence

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

    // POKEY noise distortion (PURE clear AND POLY4 clear → poly17 source: $00 / $80):
    // play the long poly17 noise sample.  Noise is not a pitched tone, so an
    // "out-of-range" frequency (per == 0, e.g. AUDF 0) must NOT silence it — clamp
    // to Paula's fastest rate instead so the engine drone/explosions still hiss.
    bool is_noise = !(audc & POKEY_PURETONE) && !(audc & POKEY_POLY4) && !(audc & 0x10u);
    noiseOn[ch] = is_noise && (vol != 0u);
    if (is_noise) {
        if (per == 0u) per = 124u;        // out-of-range → fastest Paula rate (still noise)
        want_set(ch, (uint32_t)noise_buf, (uint16_t)(NOISE_LEN / 2), per, vol);
        return;
    }

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
    lfsr_state = 0x1FFFFu;
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

uint8_t PlatformAmiga::hwRead(uint16_t addr)
{
    if (addr == 0xD20Au) return pokey_random_step();  // POKEY RANDOM register
    if (addr >= 0xD200u && addr < 0xD210u) return pokey[addr - 0xD200u];
    // PIA PORTA ($D300): Atari joysticks are ACTIVE-LOW (1 = open/neutral).  With
    // no Amiga joystick wired in yet, report neutral ($FF) so flight_control_integrate
    // reads "stick centred, no fire" — the ship flies straight instead of jamming.
    if (addr == 0xD300u) return 0xFFu;
    // CONSOL ($D01F): console keys (START/SELECT/OPTION), ACTIVE-LOW (bit clear =
    // pressed).  The Amiga keyboard handler maintains it in mem[$D01F] (idle $07;
    // RETURN clears a bit for START).  Reflect that — falling through to 0 reads as
    // "all console keys held", so the genuine attract idle loop sees START/launch
    // pressed and auto-starts the DEMO DROID demo within seconds.  ($D01F writes go
    // through platform_hw_write, which drops non-POKEY addresses, so the keyboard's
    // mem[$D01F] is never clobbered by genuine code.)
    if (addr == 0xD01Fu) return mem[0xD01Fu];
    // TRIG0-3 ($D010-$D013): joystick fire buttons, ACTIVE-LOW ($01 = released,
    // $00 = pressed).  No Amiga fire wired in yet → report released ($01).  Falling
    // through to 0 reads as "fire held": read_console_trig_delta ($5A78) computes
    // (CONSOL&$01) - TRIG0, so TRIG0=0 makes the Standby idle loop think fire is down
    // and auto-launch the game.  (Wire real Amiga fire here when adding flight input.)
    if (addr >= 0xD010u && addr <= 0xD013u) return 0x01u;
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

// g_quitJmp: the __builtin_setjmp buffer armed by RescueOnFractalus::run() so we can
// unwind the never-returning transpiled chain on quit (5 words per the GCC builtin;
// initializer forces the definition).  extern "C" — RescueOnFractalus.cpp references it.
extern "C" void* g_quitJmp[5] = { 0, 0, 0, 0, 0 };

// renderFrame: called from the transpiled frame-wait hooks (platform_render_frame).
// Render first so the display reflects the state the spin-wait just advanced, then
// wait for the next real VBI (or return immediately if one already fired during
// rendering).  After the wait, advance RTCLOK by exactly one — owned here rather
// than in the ISR so the equality spin (wait_setcount $3CB2) always gets one advance
// per iteration regardless of how long rendering takes.
// Exception: ATTRACT VBI ($1B30) bumps RTCLOK in its own transpiled body; skip here.
void PlatformAmiga::renderFrame() {
    uint16_t last = g_vbiCount;
    if (s_scene) s_scene->renderFrame();
    while (g_vbiCount == last) { /* wait for next real VBI */ }
    uint16_t vbiVec = (uint16_t)(mem[0x0222] | (mem[0x0223] << 8));
    // RTCLOK ownership: the ATTRACT ($1B30) and full flight ($4FF5) VBIs advance RTCLOK
    // ($0014) in their own transpiled bodies, so skip here to avoid double-counting.  The
    // standby/cinematic ($52D7) body does not, so renderFrame owns it there.
    if (vbiVec != 0x1B30u && vbiVec != 0x4FF5u) {
        mem[0x0014]++;
        if (!mem[0x0014]) mem[0x0013]++;
    }
    if (AmigaHardware::isLeftMouseButtonPressed()) g_pumpQuit = 1;
    if (g_pumpQuit) __builtin_longjmp(g_quitJmp, 1);
}

// pollEvents: called from the non-frame spin-wait hooks (VCOUNT/CONSOL polls that don't
// pace a frame).  Poll the quit control so the player can always abort.
void PlatformAmiga::pollEvents() {
    if (AmigaHardware::isLeftMouseButtonPressed()) g_pumpQuit = 1;
    if (g_pumpQuit) __builtin_longjmp(g_quitJmp, 1);
}

// tickVBI: no-op — RTCLOK is advanced by renderFrame() after each VBI wait.
void PlatformAmiga::tickVBI() {}

// The tunnel-ring "dirty field" flags advance_message_column already uses to stream the
// ring-clear frames from the $1000 GTIA field into the tunnel bitmap (NativeHandlers.cpp).
extern "C" volatile uint8_t g_tunnelFieldDirty;
extern "C" volatile uint8_t g_tunRowLo, g_tunRowHi;
void PlatformAmiga::tunnelRingsDrawn() {
    // display_setup's draw_frame_pattern_seq just rendered the full ring pattern into the
    // $1000 field.  Flag the whole field dirty so the next renderFrame decodes it once into
    // the tunnel bitmap (then advance_message_column streams the per-frame clear updates).
    g_tunRowLo = 0; g_tunRowHi = 85;
    g_tunnelFieldDirty = 1;
}

extern "C" volatile unsigned char g_titleDirty;
void PlatformAmiga::titleChanged() {
    // copy_altitude_graphic_to_screen ($782A) just rewrote the banner text in $32B7-$32CA
    // (the SFX sequencer alternates the block via $0091).  Flag the title region so the next
    // renderFrame re-scans it; the per-cell shadow compare then re-decodes only the glyphs
    // that actually changed between "RESCUE ON FRACTALUS!" and the copyright line.
    g_titleDirty = 1;
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
    if (raw == kRawReturn)
        mem[kConsol] = down ? kConsolStart : kConsolIdle;
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

static uint32_t vbiHandler()
{
    // RTCLOK is owned by renderFrame() in the main thread (advanced exactly once per
    // spin-wait iteration, immune to ISR timing races with the equality spin).
    // Exception: ATTRACT VBI ($1B30) bumps RTCLOK in its own transpiled body.
    // (Do NOT touch $0080 — sync_flag, reused as the $80/$81 zp pointer.)
    g_vbiCount++;

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
    // Enable copper + raster + sprite DMA, then load the faithful boot memory image
    // (pristine rof.xex) into mem[] before anything reads it — the genuine power-on RAM.
    *dmaconPointer = (uint16_t)(DMAF_SETCLR | DMAF_MASTER | DMAF_COPPER | DMAF_RASTER | DMAF_SPRITE);
    load_xex_image();

    s_scene = &scene;
    scene.initialize();   // builds bitmaps/copper; calls PlatformAmiga::audioInit
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
