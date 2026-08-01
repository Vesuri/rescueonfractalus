// PlatformAmiga — the Amiga-specific platform layer (see PlatformAmiga.h).
//
// This translation unit holds, in order:
//   1. the POKEY->Paula audio backend (the bulk of the file),
//   2. the platform_c.h bridge the C-compiled 6502 transliteration calls,
//   3. the launch-cinematic frame pump + quit handling,
//   4. the CIA-A serial-port keyboard (RETURN -> Atari START switch),
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

// g_vbiCount: bumped once per REAL vertical-blank interrupt (vbiHandler).  Defined here (ahead
// of its main use below) so flush_paula's beep capture can timestamp frames.
static volatile uint16_t g_vbiCount = 0;

#ifdef ROF_BEEP_CAP
// ---- pilot-proximity-beep capture (make PROBES=1 BEEP_CAP=1) ------------------------
// Records, each flush_paula, the POKEY shadow + per-channel Paula waveform class / period /
// volume + the restart bitmask into a ring, so amiga/beep_cap.gdb can read how ch3 (the beep
// channel) behaves under the fast every-2-frame range-1 re-push forced in vbiHandler().  This
// isolates whether the distortion is the flush restart dance, a stuck voice, or a ring/order race.
#define BC_N 640
extern "C" volatile unsigned short g_bcVbi[BC_N] = {};
extern "C" volatile unsigned char  g_bcPokey[BC_N][9] = {};   // AUDF1,AUDC1,..,AUDF4,AUDC4,AUDCTL
extern "C" volatile unsigned char  g_bcKind[BC_N][4] = {};    // 0 silent/other 1 pure 2 poly4 3 poly5 4 noise 5 polydist
extern "C" volatile unsigned short g_bcPer[BC_N][4] = {};
extern "C" volatile unsigned char  g_bcVol[BC_N][4] = {};
extern "C" volatile unsigned char  g_bcRestart[BC_N] = {};    // channels that restarted this flush
// aux[]: slot-5 lifecycle per frame (see the flush_paula population for the exact bytes) —
//  slot5 dist/vol/freq/chan + mixer top-prio/top-voice + slot5 freq-env phase + range digit.
extern "C" volatile unsigned char  g_bcAux[BC_N][8] = {};
extern "C" volatile unsigned short g_bcIdx = 0;          // write cursor (mod BC_N)
extern "C" volatile unsigned char  g_bcOn  = 0;          // armed once flight VBI is live
// break-hook: gdb `break rof_bc_done` fires when the ring is full.  Must have a UNIQUE,
// non-empty body or identical-code-folding merges it into another empty fn (e.g. a blitter
// helper) → the breakpoint fires at boot.  The volatile bump defeats ICF.
extern "C" volatile unsigned long g_bcDoneTick = 0;
extern "C" void rof_bc_done(void) { g_bcDoneTick++; }
// SFX-event-push log: rof_bc_push() records every ring_push_0719 (event id | $80) with the
// frame it fired on, so we can see WHICH events drive the range-1 "wrong sound" and when.
#define BCP_N 256
extern "C" volatile unsigned char  g_bcPushId[BCP_N] = {};   // FULL pushed byte (bit7 = marked/event)
extern "C" volatile unsigned short g_bcPushVbi[BCP_N] = {};  // g_vbiCount at push
extern "C" volatile unsigned short g_bcPushIdx = 0;          // WRAP cursor (keeps last BCP_N)
extern "C" volatile unsigned short g_bcPushN   = 0;          // total pushes ever
extern "C" volatile unsigned short g_bcPush01N = 0;          // pushes whose low-7-bits == 1 (event $01)
extern "C" void rof_bc_push(unsigned char v) {
    // Log EVERY push (not gated on g_bcOn) into a wrap ring so the range-1 window is always
    // present; store the FULL byte so a marked event $01 ($81) is distinguishable from a raw $01.
    unsigned i = g_bcPushIdx;
    g_bcPushId[i] = v; g_bcPushVbi[i] = g_vbiCount;
    g_bcPushIdx = (unsigned short)((i + 1u) % BCP_N);
    g_bcPushN = (unsigned short)(g_bcPushN + 1u);
    if ((v & 0x7Fu) == 1u) g_bcPush01N = (unsigned short)(g_bcPush01N + 1u);   // caught the $81 pusher
}
// slot-5 lifecycle logs (range-1 poly4 bug): does slot-5 volume ever get cleared on the Amiga?
// On the Atari sfx_engine_reset ($5433) zeroes slot-5 vol (+ envelopes), leaving a SILENT poly4/$1f
// leftover; the bug is the Amiga leaving slot-5 vol nonzero → audible poly4 warble.  These two logs
// (UNGATED — they must catch the level-start reset/push that precedes flight arming) record the
// g_vbiCount of every sfx_engine_reset call and every event-$01 load so we can see the ORDER.
#define BCE_N 256
extern "C" volatile unsigned short g_bcResetVbi[BCE_N] = {};
extern "C" volatile unsigned short g_bcResetN = 0;
extern "C" volatile unsigned short g_bc01Vbi[BCE_N] = {};
extern "C" volatile unsigned short g_bc01N = 0;
// event $01 lives in boot_standby_launch_driver's standby_level_select_loop block L_634f, reachable by THREE
// paths: (1) $6150 when $006c(sound_active_flag)==0 && $0644(sound_event_flag)==0; (2) $62f4 when
// $0004(level_or_state)!=0; (3) the $5a78 (CONSOL/TRIG) fall-through.  g_l634fPath is set to 1/2/3
// at each entry (rof_native.c, under ROF_BEEP_CAP) so we see WHICH fires during flight.  Ctx per
// load: [0]=$0004 [1]=$006d(level_stage) [2]=$006c [3]=$0644 [4]=$0642 range [5]=mem[$D01F]
// [6]=$0627(fresh_start_flag) [7]=g_l634fPath.  Plus count boot_standby_launch_driver ($5f1d) entries to see if
// it's being re-invoked every game_main_loop iteration during flight.
extern "C" volatile unsigned char  g_bc01Ctx[BCE_N][8] = {};
extern "C" volatile unsigned char  g_l634fPath = 0;      // set at each L_634f entry (1/2/3)
// $5a78 evaluation at the last PATH-3 (fall-through) entry: a=result A ($01-TRIG0), d01f/d010 =
// what bus_read actually returned for CONSOL/TRIG0.  If d010==0 while s_trig0State is live=$01,
// the trigger read is wrong (the poly4 root cause); if d010==1 the flag logic is.
extern "C" volatile unsigned char  g_p3_a = 0, g_p3_d01f = 0, g_p3_d010 = 0, g_p3_memd01f = 0;
extern "C" void rof_bc_p3(unsigned char a, unsigned char d01f, unsigned char d010, unsigned char memd01f) {
    g_p3_a = a; g_p3_d01f = d01f; g_p3_d010 = d010; g_p3_memd01f = memd01f;
}
extern "C" volatile unsigned short g_dsEntryN = 0;       // boot_standby_launch_driver ($5f1d) invocation count
extern "C" volatile unsigned short g_dsEntryVbi = 0;     // g_vbiCount of the most recent entry
extern "C" void rof_bc_ds_entry(void) { g_dsEntryN++; g_dsEntryVbi = g_vbiCount; }  // hooked at $5f1d
extern "C" void rof_bc_reset_log(void) {   // hooked at sfx_engine_reset $5433
    unsigned i = g_bcResetN; if (i < BCE_N) { g_bcResetVbi[i] = g_vbiCount; g_bcResetN = (unsigned short)(i + 1u); }
}
// setup_level_clear_state ($7BC6) is the SOLE writer of player_lives($0072)=2 = the ONLY cause
// of a game_main_loop_body flight-loop break -> outer-loop -> boot_standby_launch_driver re-invocation ->
// event $01 reload.  If this fires during a range-1 pilot PASS (not a genuine level advance),
// that's the range-1 bug: a spurious level-clear.  Ctx: [0]=$0004(level_or_state) [1]=$0642(range)
// [2]=$008F?level_cleared_flag  [3]=$0072 before  [4]=$003A.  g_lclN counts, ring holds vbis.
extern "C" volatile unsigned short g_lclN = 0;
extern "C" volatile unsigned short g_lclVbi[BCE_N] = {};
extern "C" volatile unsigned char  g_lclCtx[BCE_N][5] = {};
extern "C" void rof_bc_lcl_log(void) {     // hooked at setup_level_clear_state call ($5223x/9175)
    unsigned i = g_lclN; if (i < BCE_N) {
        g_lclVbi[i] = g_vbiCount;
        g_lclCtx[i][0] = mem[0x0004]; g_lclCtx[i][1] = mem[0x0642];
        g_lclCtx[i][2] = mem[0x2849]; g_lclCtx[i][3] = mem[0x0072];  // $2849 = level_cleared_flag
        g_lclCtx[i][4] = mem[0x003A];
        g_lclN = (unsigned short)(i + 1u);
    }
}
// Envelope-expiry RE-QUEUE log (sfx_voice_envelope_tick line ~8568): whenever a voice slot's
// envelope expires it re-queues that slot's FOLLOW-ON event id ($06F7+y) via ring_push_marked.
// This is the self-sustaining chain that reloads event $01 (poly4, slot 5) during flight at
// range 1 — and it is NOT captured by the existing ring_push_0719 push-log.  We log every
// re-queue: [slot y, follow-on id $06F7+y, vbi] + slot-5 envelope state at the moment, so we
// see WHICH slot re-arms event $01 and why it keeps expiring.  Ring wraps (last BCE_N).
extern "C" volatile unsigned short g_rqN = 0;              // total re-queues seen (may exceed ring)
extern "C" volatile unsigned short g_rqIdx = 0;            // write cursor into the wrap ring
extern "C" volatile unsigned short g_rqVbi[BCE_N] = {};
extern "C" volatile unsigned char  g_rqSlot[BCE_N] = {};
extern "C" volatile unsigned char  g_rqId[BCE_N] = {};
// count re-queues whose follow-on id == 1 (event $01) specifically, by originating slot
extern "C" volatile unsigned short g_rq01BySlot[16] = {};
extern "C" void rof_bc_requeue_log(unsigned char y, unsigned char id) {  // hooked at line ~8568
    unsigned i = g_rqIdx;
    g_rqVbi[i] = g_vbiCount; g_rqSlot[i] = y; g_rqId[i] = id;
    g_rqIdx = (unsigned short)((i + 1u) % BCE_N);
    g_rqN = (unsigned short)(g_rqN + 1u);
    if (id == 1u && y < 16u) g_rq01BySlot[y] = (unsigned short)(g_rq01BySlot[y] + 1u);
}
// Ring-drain EVENT log: every bit7-set entry the drain feeds to sfx_event_load, with the ring index
// it came from.  Catches (a) event $01 ($81), (b) OUT-OF-RANGE event ids (>33=$21) that make
// sfx_event_load read $56D4+i out of bounds -> a garbage slot y (possibly bit7-set) -> game_sub_55FC
// pushes $8x -> the $81 cascade.  g_drainOOR counts out-of-range events; g_drain81 counts $81.
extern "C" volatile unsigned short g_drainN = 0, g_drainOOR = 0, g_drain81 = 0, g_drainIdx = 0;
extern "C" volatile unsigned char  g_drainEvt[BCE_N] = {};   // full entry byte (wrap ring)
extern "C" volatile unsigned char  g_drainTail[BCE_N] = {};  // ring index it was read from
extern "C" volatile unsigned short g_drainVbi[BCE_N] = {};
// FIRST $81 snapshot: the ring index + absolute address (for a HW watchpoint) + head/tail geometry
// + a full 32-byte ring copy, so ONE full-speed flight pins where the stale $81 sits and what
// surrounds it (nothing pushes $81, so it must be written OOB by a non-ring writer).
extern "C" volatile unsigned char  g_drain81Tail = 0xFF, g_drain81Head = 0;
extern "C" volatile unsigned short g_drain81Addr = 0;              // $0719 + tail (watch this)
extern "C" volatile unsigned char  g_drain81Ring[32] = {};
extern "C" void rof_bc_drain_evt(unsigned char entry, unsigned char tail) {
    unsigned i = g_drainIdx;
    g_drainEvt[i] = entry; g_drainTail[i] = tail; g_drainVbi[i] = g_vbiCount;
    g_drainIdx = (unsigned short)((i + 1u) % BCE_N);
    g_drainN = (unsigned short)(g_drainN + 1u);
    if ((entry & 0x7Fu) > 0x21u) g_drainOOR = (unsigned short)(g_drainOOR + 1u);  // out-of-range id
    if (entry == 0x81u) {
        if (g_drain81 == 0) {   // snapshot the FIRST occurrence
            g_drain81Tail = tail; g_drain81Head = mem[0x0073];
            g_drain81Addr = (unsigned short)(0x0719u + tail);
            for (int k = 0; k < 32; k++) g_drain81Ring[k] = mem[0x0719u + k];
        }
        g_drain81 = (unsigned short)(g_drain81 + 1u);  // event $01
    }
}
// $81 (event $01) push capture: the exact caller chain at the moment a $81 is written to the
// ring, recorded in CODE (no gdb breakpoint => full-speed flight => keyboard key-ups still
// register).  g_push81Ra0 = ring_push_0719's caller (ring_push_marked vs game_sub_55FC =
// distinguishes an X=1 event push from a Y=$81 slot push); Ra1/Ra2 = up the chain to the real
// culprit.  Resolve with `info symbol` in gdb after a normal flight to a range-1 pilot.
extern "C" volatile unsigned short g_push81N = 0;
extern "C" volatile void *g_push81Ra0 = 0;
extern "C" volatile unsigned short g_push81Vbi = 0;
extern "C" void rof_bc_push81(void *ra0) {
    if (g_push81N == 0) { g_push81Ra0 = ra0; g_push81Vbi = g_vbiCount; }
    g_push81N++;   // keep the FIRST caller (later ones may be re-entrant cascades)
}
extern "C" void rof_bc_ev01_log(void) {    // hooked in sfx_event_load/sfx_event_load when event id==1
    unsigned i = g_bc01N; if (i < BCE_N) {
        g_bc01Vbi[i] = g_vbiCount;
        g_bc01Ctx[i][0] = mem[0x0004]; g_bc01Ctx[i][1] = mem[0x006D];
        g_bc01Ctx[i][2] = mem[0x006C]; g_bc01Ctx[i][3] = mem[0x0644];
        g_bc01Ctx[i][4] = mem[0x0642]; g_bc01Ctx[i][5] = g_p3_d01f;    // [5]=CONSOL $5a78 read
        g_bc01Ctx[i][6] = g_p3_d010; g_bc01Ctx[i][7] = g_l634fPath;    // [6]=TRIG0 $5a78 read
        g_bc01N = (unsigned short)(i + 1u);
    }
}
// Keyboard event log (from boot): every decoded (raw,down) the CIA-A keyboard ISR sees, to catch
// the PHANTOM Left Shift ($60)-down that stalls s_trig0State at 0 without the user firing.  The
// ring wraps (holds the last BCK_N events); g_bcKeyFireN counts ALL $60 events ever (down or up)
// so a boot-time phantom is caught even if the ring later wraps past it.
#define BCK_N 512
extern "C" volatile unsigned short g_bcKeyVbi[BCK_N] = {};
extern "C" volatile unsigned char  g_bcKeyRaw[BCK_N] = {};
extern "C" volatile unsigned char  g_bcKeyDown[BCK_N] = {};
extern "C" volatile unsigned short g_bcKeyIdx = 0;       // wraps mod BCK_N
extern "C" volatile unsigned short g_bcKeyFireN = 0;     // count of raw==$60 events (any edge)
extern "C" volatile unsigned char  g_bcKeyFireLastDown = 0xFF;  // last $60 edge seen (1=down 0=up)
extern "C" void rof_bc_key(unsigned char raw, unsigned char down) {   // hooked in keyboardHandler
    unsigned k = g_bcKeyIdx % BCK_N; g_bcKeyIdx = (unsigned short)(g_bcKeyIdx + 1u);
    g_bcKeyVbi[k] = g_vbiCount; g_bcKeyRaw[k] = raw; g_bcKeyDown[k] = down;
    if (raw == 0x60u) { g_bcKeyFireN++; g_bcKeyFireLastDown = down; }
}
// EDGE-LOG of mem[$D01F] (CONSOL): store (vbi,value) only when it CHANGES, from boot.  This shows
// exactly when/how CONSOL becomes $06 (START held) — the value $5a78 reads as launch.  Non-wrapping.
#define BCD_N 128
extern "C" volatile unsigned short g_bcD01FVbi[BCD_N] = {};
extern "C" volatile unsigned char  g_bcD01FVal[BCD_N] = {};
extern "C" volatile unsigned short g_bcD01FN = 0;
extern "C" void rof_bc_d01f(void) {   // called every vbi
    static unsigned char last = 0xAA;
    unsigned char v = mem[0xD01Fu];
    if (v != last) { last = v; unsigned k = g_bcD01FN; if (k < BCD_N) { g_bcD01FVbi[k] = g_vbiCount; g_bcD01FVal[k] = v; g_bcD01FN = (unsigned short)(k + 1u); } }
}
static unsigned char cur_vol_cap[4] = { 0, 0, 0, 0 };    // last VOL applied per channel
static unsigned char bc_classify(uint32_t p) {
    if (p == (uint32_t)wave_pure) return 1;
    if (p >= (uint32_t)noise_buf     && p < (uint32_t)noise_buf     + sizeof(noise_buf))     return 4;
    if (p >= (uint32_t)poly4_wave    && p < (uint32_t)poly4_wave    + sizeof(poly4_wave))    return 2;
    if (p >= (uint32_t)poly5_wave    && p < (uint32_t)poly5_wave    + sizeof(poly5_wave))    return 3;
    if (p >= (uint32_t)poly_dist_buf && p < (uint32_t)poly_dist_buf + sizeof(poly_dist_buf)) return 5;
    return 0;
}
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

    if (valid) {
        // Split into "restart" (waveform changed) and "live" (same waveform → just VOL/PER).
        for (uint8_t ch = 0; ch < 4; ch++) {
            if (!(valid & (1u << ch))) continue;
            if (want_ptr[ch] != cur_ptr[ch] || want_len[ch] != cur_len[ch])
                restart |= (uint8_t)(1u << ch);
            else { AUD_PER(ch) = want_per[ch]; AUD_VOL(ch) = want_vol[ch];     // live, no click
                   cur_per[ch] = want_per[ch];
#ifdef ROF_BEEP_CAP
                   cur_vol_cap[ch] = want_vol[ch];
#endif
            }
        }

        if (restart) {
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
#ifdef ROF_BEEP_CAP
                cur_vol_cap[ch] = want_vol[ch];
#endif
            }
            *dmaconPointer = (uint16_t)(0x8000u | restart); // AUDxEN on — all at once, one wait paid
        }
    }

#ifdef ROF_BEEP_CAP
    if (g_bcOn) {
        unsigned s = g_bcIdx; unsigned ni = (s + 1u) % BC_N; g_bcIdx = (unsigned short)ni;
        g_bcVbi[s]     = g_vbiCount;
        g_bcRestart[s] = restart;
        for (int i = 0; i < 9; i++) g_bcPokey[s][i] = pokey[i];
        // slot-5 lifecycle (the range-1 poly4 = a stale event-$01 voice in slot 5):
        //  [0]=$0662 slot5 distortion  [1]=$0670 slot5 vol(&0f)  [2]=$067e slot5 freq
        //  [3]=$070a slot5 POKEY-channel idx (0=unassigned)  [4]=$0714 mixer top-prio-val
        //  [5]=$0715 mixer top-voice-idx  [6]=$06e0 slot5 freq-env phase  [7]=$0642 range digit
        // Atari-correct in flight: dist=$40 freq=$1f but vol=0, chan=0 (silent leftover).
        g_bcAux[s][0] = mem[0x0662]; g_bcAux[s][1] = (unsigned char)(mem[0x0670] & 0x0F);
        g_bcAux[s][2] = mem[0x067E]; g_bcAux[s][3] = mem[0x070A];
        g_bcAux[s][4] = mem[0x0714]; g_bcAux[s][5] = mem[0x0715];
        g_bcAux[s][6] = mem[0x06E0]; g_bcAux[s][7] = mem[0x0642];
        for (int ch = 0; ch < 4; ch++) {
            g_bcKind[s][ch] = bc_classify(cur_ptr[ch]);   // waveform Paula is playing after this flush
            g_bcPer[s][ch]  = cur_per[ch];
            g_bcVol[s][ch]  = cur_vol_cap[ch];
        }
        if (ni == 0u) rof_bc_done();   // full ring: keep WRAPPING (hold the last 320 frames so
                                       // SIGINT catches the stuck poly4), just tick the break-hook
    }
#endif
}

// ---- POKEY→Paula frequency conversion ----------------------------------------
static uint16_t pokey_period(uint8_t ch, uint8_t audf, uint8_t audctl)
{
    static const uint32_t POKEY_CLOCK = 1789773u;
    static const uint32_t PAULA_CLOCK = 3546895u;

    uint32_t base_div = (audctl & 0x01u) ? 114u : 28u;
    // AUDCTL bit→channel (atari800 pokey.h): CH1_179 $40 = 1.79MHz for Ch1 (0-indexed ch0),
    // CH3_179 $20 = Ch3 (ch2); CH1_CH2 $10 joins Ch1+Ch2 (lo=ch0), CH3_CH4 $08 joins Ch3+Ch4
    // (lo=ch2).  (These were previously transposed here.)
    bool use_179 = ((ch == 0) && (audctl & 0x40u)) ||
                   ((ch == 2) && (audctl & 0x20u));
    bool chain_lo = (ch == 0 && (audctl & 0x10u)) ||
                    (ch == 2 && (audctl & 0x08u));

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
            bool     use_179   = ((ch == 0) && (audctl & 0x40u)) ||   // CH1_179 $40 → ch0
                                 ((ch == 2) && (audctl & 0x20u));     // CH3_179 $20 → ch2
            bool     chain_lo  = (ch == 0 && (audctl & 0x10u)) ||     // CH1_CH2 $10 → ch0 lo
                                 (ch == 2 && (audctl & 0x08u));       // CH3_CH4 $08 → ch2 lo
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

    // Disable the Paula "LED" audio low-pass filter (one-time HW write): CIA-A PRA bit 1
    // ($BFE001, 0x02) HIGH = LED dim + filter OFF; LOW = LED bright + filter ON (~5-6 kHz RC).
    // Kickstart leaves it ON after boot, which rolls off the highs.  The Atari POKEY has no such
    // filter, so switching it off is the faithful choice (and keeps the SFX bright).
    *ciaapraPointer |= 0x02u;

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
    if (addr == 0xD01Fu) return s_consolState;   // dedicated input state, isolated from mem[] corruption
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

// Death-cinematic teardown signal (energy-out/crash).  The Atari's $4F76
// intro_teardown_fade_loop writes DMACTL ($D400)=0 to blank the ANTIC playfield before it
// fades COLBK ($00D4) to black.  On the Amiga the copper owns the display, so that write is
// otherwise dropped — latch it here so renderFrame stops the terrain convert and shows a
// solid full-screen colour faded from mem[$00D4] (mirroring "DMA off, only COLBK shows").
// Set on a 0 write during flight, cleared on any nonzero DMACTL write (and by renderFrame
// when the scene leaves flight).  See the death-cinematic memory.
extern "C" volatile unsigned char g_flightBlank = 0;

void PlatformAmiga::hwWrite(uint16_t addr, uint8_t val)
{
    if (addr == 0xD400u) { g_flightBlank = (val == 0u) ? 1u : 0u; return; }  // DMACTL: 0 = playfield blanked
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

// flightScannerTick: decode the Long Range Scanner (#13) close-range blink cells $33DF/$33E0
// straight into the cockpit bitmap, from the flight VBI (50Hz) — like flightShotTick, this is
// the faithful home for it: startup_init() ($3FFA, run in vbi_handler_flight just above) toggles
// their bit7 at 50Hz (a two-speed proximity blink), so decoding here at 50Hz makes the blink run
// at full rate instead of the ~5-6fps main-loop render() cadence (which made it far too slow and
// range 1 vs 2 indistinguishable).  s_scene decodes only on a bit7 flip (cheap: 2 cells).
void PlatformAmiga::flightScannerTick()
{
    if (s_scene) s_scene->decodeScannerBlinkCells();
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
extern "C" volatile unsigned long g_renderFrameCount = 0;
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
// boot_standby_launch_driver launch-tail milestone stamps: rof_ds_mile(i) records g_vbiCount at milestone i,
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
// Knock-gated ($0632) split of platform_render_frame: g_alTRScene = scene->renderFrame() (the
// dirty-rect composite + the renderFlightDirect while(flightSwapPending) flip wait); g_alTRIdle =
// the PlatformAmiga while(g_vbiCount==last) frame-sync wait after it.  Pinpoints the ~204ms.
extern "C" volatile unsigned long g_alTRScene=0, g_alTRIdle=0;
// Of g_alTRScene, the time specifically in renderFlightDirect's while(flightSwapPending) flip wait.
extern "C" volatile unsigned long g_alTFlipWait=0;
// flightVblankSwap during the knock: g_alVSwapRun = ISR firings that ran it; g_alVSwapCleared = of
// those, how many found flightSwapPending set (i.e. actually did the flip).  If Run >> Cleared, the
// ISR fires often but a flip is rarely pending -> the flip wait isn't ISR-starved; if Run ~ steps,
// the ISR itself is being throttled during the knock.
extern "C" volatile unsigned long g_alVSwapRun=0, g_alVSwapCleared=0;
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
    if (!_deferredFlip) while (g_vbiCount == last) { /* wait for next real VBI */ }
#ifdef ROF_FLIGHT_PROBE
    if (_rFlight) { unsigned long _iw = rof_subclock() - _ri0; g_rIdleWall += _iw;
                    if (mem[0x0632]) g_alTRIdle += _iw; }   // knock: frame-sync wait
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
    // boot_standby_launch_driver's draw_frame_pattern_seq just rendered the full ring pattern into the
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
// vector through ciaa.resource (the AddICRVector mechanism).  keyboard.device
// normally owns the vector, so we steal it (saving the
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

#ifdef ROF_BEEP_CAP
    { extern void rof_bc_key(unsigned char, unsigned char); rof_bc_key(raw, down ? 1u : 0u); }
#endif

    // Drive the CONSOL START switch (bit0) from RETURN's down/up edges, so the register
    // continuously reflects the key's level — just like the real GTIA switch.
    if (raw == kRawReturn) {
        s_consolState = down ? kConsolStart : kConsolIdle;   // the read source (hwRead $D01F)
        mem[kConsol]  = s_consolState;                        // keep RAM mirror in sync (Station-scene reader)
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
#ifdef ROF_BEEP_CAP
    { extern void rof_bc_d01f(void); rof_bc_d01f(); }   // edge-log CONSOL ($D01F) transitions
#endif

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
    // polling CONSOL.  A fixed vbi==350 fired before boot_standby_launch_driver's standby poll was live
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
#else
            (void)d;   // manual-launch measurement: the player presses START themselves
#endif
#endif
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

    // Per-frame VBI body — run in the REAL vertical-blank interrupt, where the Atari ran
    // its VBI.  game_vbi_isr() dispatches by the live VVBLKI vector to the standby ($52D7)
    // or flight ($4FF5) body — as the Atari swaps VVBLKI — bracketing the work in a
    // save/restore of the shared 6502 register file (the main loop may be mid-instruction
    // using `cpu` when this interrupt preempts it).
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
        static uint16_t s_flightVbi = 0; static uint8_t s_retPhase = 0;
        if (vv == 0x4FF5u && s_flightVbi == 0) s_flightVbi = g_vbiCount;
        const uint16_t dt = s_flightVbi ? (uint16_t)(g_vbiCount - s_flightVbi) : 0;
        if (s_flightVbi && s_retPhase == 0 && dt >= 250) {
            mem[0x003Au] = 0xFF;          // level-clear / mother-ship-arrived gate (what $7BC6 sets)
            mem[0x0676u] = 1;             // mother-ship HUD light on
            mem[0x2849u] = 1;             // level_cleared_flag → $5223 chain also fires $7BC6 (MOTHER SHIP! msg)
            s_retPhase = 1;
        }
        if (s_retPhase == 1 && dt >= 340) {
            s_pendingFlightKey = 0x15;     // Atari KBCODE 'B' (boosters) → $519c CLI window
            s_retPhase = 2;
        }
    }
#endif

#ifdef ROF_BEEP_CAP
    // Force the pilot-proximity beep (SFX event $14) at the range-1 rate so the fast-beep
    // distortion reproduces headlessly.  Once the flight VBI ($4FF5) has been live a moment,
    // arm the flush_paula capture and, every 2 frames (= range-1 blink rate), replicate what
    // startup_init $3FFA does at $4016: ring_push_marked(X=$14) into the SFX event ring $0719
    // (head $0073, ring_push_0719 $55FF) — the in-flight SFX engine (sfx_voice_envelope_tick,
    // inside game_vbi_isr just below) then plays it.  Nothing else pushes $14 (no pilot in the
    // headless run), so ch3 sees exactly the fast beep.
    {
        // FLIGHT range-1 pilot-beep capture (interactive): arm the ring once the flight VBI
        // ($4FF5) is live AND the range-to-pilot digit $0642 has ticked down to 1 or 2 (the
        // user flies toward a downed pilot in the FS-UAE window).  The ring records 320 frames
        // of all 4 Paula channels + POKEY shadow, then freezes (g_bcOn=0), catching the range
        // 2→1→0 beep transition.  On the Atari (lrscanner.a8s) that beep is a PURE tone on ch3
        // pulsing faster as range drops; if the Amiga shows NOISE distortion there, that's the bug.
        // Arm the per-frame slot-5 ring at the range-1/2 pilot approach (needs interactive
        // flying to a downed pilot; the headless auto-launch never gets there, and a headless
        // no-pilot run already CONFIRMED slot 5 is correctly vol=0 through standby + flight).
        // The reset/ev01 logs above are ungated so they catch the level-start reset+push
        // regardless.  On the Atari (lrscanner) slot 5 stays vol=0 at range 1..2; if the Amiga
        // shows slot-5 vol nonzero (audible poly4) here, that's the bug — and whether event $01
        // is re-loaded (g_bc01N ticks up) or slot-5 vol is bumped in place tells us the cause.
        static unsigned char s_bcArmed = 0;
        if (!s_bcArmed && g_probeFlightVbi && mem[0x0642u] >= 1u && mem[0x0642u] <= 2u) {
            g_bcOn = 1; s_bcArmed = 1;   // one-shot; freeze in flush_paula sticks
        }
    }
#endif

    game_vbi_isr();
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

    keyboardInit();       // RETURN = START for the launch cinematic

    // --- run -----------------------------------------------------------------
    // The whole game runs inside scene.run(): the genuine transpiled/native boot chain,
    // whose frame-wait spin loops each call platform_render_frame (render + wait on
    // g_vbiCount).  Returns when the user quits (left mouse button).
    scene.run();

    // --- restore system ------------------------------------------------------
    keyboardShutdown();
    scene.shutdown();     // calls PlatformAmiga::audioShutdown

    RemIntServer(INTB_VERTB, &vbiServer);

    // Disable our display DMA before handing back.
    *dmaconPointer = (uint16_t)(DMAF_COPPER | DMAF_RASTER | DMAF_SPRITE);

    LoadView(savedView);
    WaitTOF();
    WaitTOF();
}
