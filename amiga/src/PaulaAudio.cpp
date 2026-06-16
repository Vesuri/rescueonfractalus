// Paula audio backend: POKEY register writes → Amiga Paula DMA.
//
// Frequency mapping (mirrors PlatformSDL::updateChannelFreq):
//   POKEY clock = 1789773 Hz (NTSC), base divider = 28 (64 kHz) or 114 (15 kHz).
//   AUDCTL bits: 0x01=15kHz, 0x08=CH0+CH1 16-bit chain, 0x10=CH2+CH3 chain,
//                0x20=CH0 uses 1.79 MHz, 0x40=CH2 uses 1.79 MHz.
//   Paula period for 2-sample square wave: PAULA_CLOCK / (2 * freq_hz).
//
// Volume mapping: AUDC[3:0] * 4 → Paula AUDxVOL (0..64); 0 if bit 4 set.
//
// Memory image: mem[] is populated by load_xex_image() (XexImage.cpp) from the
// pristine rof.xex before the scene initialises; this file only drives Paula.

#define ECS_SPECIFIC
#include <hardware/dmabits.h>
#include "../framework/AmigaHardware.h"
#include "PaulaAudio.h"

// mem[] and cpu are defined in src/cpu/cpu.c (compiled for m68k as audio/cpu.o)
extern "C" volatile uint8_t mem[65536];

// The boot memory image (pristine rof.xex) is loaded by load_xex_image() in
// XexImage.cpp, called from main() before the scene initialises.

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
static __chip uint8_t wave_buf[4][2][64];                // per-channel poly, ping-pong
static uint8_t        wave_idx[4]       = { 0, 0, 0, 0 }; // index Paula is pointed at

// POKEY "noise" distortion (AUDC with NOTPOLY5/POLY4/PURE all giving the poly17
// source — i.e. PURE clear AND POLY4 clear: $80 ungated, $00 poly5-gated) is true
// pseudo-random noise (the 17-bit poly), which Paula cannot synthesise.  We DMA a
// long pre-rendered poly17 noise sample instead: each byte is one POKEY channel-
// clock sample (so it plays at the same pokey_period() as a tone would), and the
// buffer is long enough that the DMA loop period is a sub-audible rumble rather
// than a pitched tone — the engine drone (event 8: AUDC $00, AUDF $FF) and the
// explosion tails read as noise instead of a square.  1 KB chip RAM.
#define NOISE_LEN 1024
static __chip uint8_t noise_buf[NOISE_LEN];
static uint32_t       noise_lfsr = 0x1FFFFu;   // PERSISTENT — carries across refills so the
                                               // stream never repeats (true continuous noise)
static bool           noiseOn[4] = { false, false, false, false };
// Refill the shared noise sample with the NEXT poly17 run (continues noise_lfsr).
static void fill_noise_buf(void)
{
    for (int i = 0; i < NOISE_LEN; i++) {
        uint32_t bit = ((noise_lfsr >> 16) ^ (noise_lfsr >> 4)) & 1u;
        noise_lfsr = ((noise_lfsr << 1) | bit) & 0x1FFFFu;
        noise_buf[i] = (noise_lfsr & 1u) ? 0x7Fu : 0x81u;   // bipolar ±127
    }
}
// Called once per VBI: while any channel is in noise mode, regenerate the sample
// with fresh poly17 so a low-rate noise voice (the engine drone) doesn't audibly
// loop the 1 KB buffer.  Overwriting the buffer Paula is mid-DMA on is inaudible
// for noise (random over random).  No active noise channel → skip (cheap).
extern "C" void paula_noise_tick(void)
{
    if (noiseOn[0] || noiseOn[1] || noiseOn[2] || noiseOn[3]) fill_noise_buf();
}

// POKEY poly patterns (1 bit/entry) and AUDC distortion bits (atari800 pokeysnd.c/pokey.h)
static const uint8_t kBit4[15] = { 1,1,1,1,0,0,0,1,0,0,1,1,0,1,0 };
static const uint8_t kBit5[31] = { 1,1,1,1,0,1,1,0,1,0,0,1,1,0,0,0,0,0,1,1,1,0,0,1,0,0,0,1,0,1,0 };
#define POKEY_NOTPOLY5  0x80u
#define POKEY_POLY4     0x40u
#define POKEY_PURETONE  0x20u

// Build channel ch's poly waveform and return LEN in words (0 = pure tone /
// unmodelled mode → caller uses wave_pure).  Writes into the ping-pong buffer
// Paula is NOT currently reading, then advances wave_idx[ch] to it, so the
// playing waveform is never overwritten mid-DMA (avoids a click at note change).
// Captures 2x the poly period (30 or 62 bytes) — a clean, even-length loop
// regardless of stride.
static uint16_t build_poly_wave(uint8_t ch, uint8_t audc, uint8_t audf, uint8_t audctl)
{
    bool poly5tone = (audc & POKEY_PURETONE) && !(audc & POKEY_NOTPOLY5);                       // $20
    // POLY4 set, PURE clear → poly4 buzz: $C0 (poly5 bypassed) and $40 (the launch door
    // swoosh, poly5-gated).  The full poly5×poly4 period (465) can't fit the 64-byte
    // buffer, so $40 is rendered as ungated poly4 — same raspy buzz, close enough that
    // the door reads as a buzz rather than a clean tone.
    bool poly4     = !(audc & POKEY_PURETONE) && (audc & POKEY_POLY4);                          // $C0, $40
    if (!poly5tone && !poly4) return 0u;

    uint16_t baseDiv  = (audctl & 0x01u) ? 114u : 28u;
    uint32_t stride   = (uint32_t)(audf + 1u) * baseDiv;
    uint8_t  s4       = (uint8_t)(stride % 15u);
    uint8_t  s5       = (uint8_t)(stride % 31u);
    uint16_t lenBytes = poly4 ? 30u : 62u;

    uint8_t  next = wave_idx[ch] ^ 1u;       // the buffer Paula is not playing
    uint8_t* dst  = wave_buf[ch][next];
    uint8_t  out = 0, p4 = 0, p5 = 0;
    for (uint16_t i = 0; i < lenBytes; i++) {
        p4 = (uint8_t)((p4 + s4) % 15u);   // advance polys by stride, then sample
        p5 = (uint8_t)((p5 + s5) % 31u);
        bool toggle = poly4 ? (kBit4[p4] == (out ^ 1u))  // flip per poly4 vs current output
                            : (kBit5[p5] != 0);          // poly5-gated pure tone: flip when gate passes
        if (toggle) out ^= 1u;
        dst[i] = out ? 0x7Fu : 0x81u;
    }
    wave_idx[ch] = next;
    return (uint16_t)(lenBytes >> 1);
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

uint8_t paula_pokey_random(void) { return pokey_random_step(); }

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

    // POKEY noise distortion (PURE clear AND POLY4 clear → poly17 source: $00 / $80):
    // play the long poly17 noise sample.  Noise is not a pitched tone, so an
    // "out-of-range" frequency (per == 0, e.g. AUDF 0) must NOT silence it — clamp
    // to Paula's fastest rate instead so the engine drone/explosions still hiss.
    bool is_noise = !(audc & POKEY_PURETONE) && !(audc & POKEY_POLY4) && !(audc & 0x10u);
    noiseOn[ch] = is_noise && (vol != 0u);
    if (is_noise) {
        if (per == 0u) per = 124u;        // out-of-range → fastest Paula rate (still noise)
        AUD_PTR(ch) = (uint32_t)noise_buf;
        AUD_LEN(ch) = (uint16_t)(NOISE_LEN / 2);   // words
        AUD_PER(ch) = per;
        AUD_VOL(ch) = vol;
        return;
    }

    if (per == 0u) vol = 0u;  // out-of-range frequency → silence

    // Point the channel at the waveform for its distortion mode.  Paula latches
    // PTR/LEN at the next DMA loop wrap, so the change takes effect promptly.
    uint16_t len_words = build_poly_wave(ch, audc, audf, audctl);
    if (len_words == 0u) {
        AUD_PTR(ch) = (uint32_t)wave_pure;   // pure tone / unmodelled
        AUD_LEN(ch) = 1u;
    } else {
        AUD_PTR(ch) = (uint32_t)wave_buf[ch][wave_idx[ch]];
        AUD_LEN(ch) = len_words;
    }

    if (per != 0u) AUD_PER(ch) = per;
    AUD_VOL(ch) = vol;
}

// Forward declaration — defined in SfxPlayer.cpp
extern "C" void sfx_seq_step_native(void);

// ---- public interface --------------------------------------------------------
// mem[] has already been populated (load_xex_image, called from main before the scene
// initialises); this only sets up the Paula side.
void paula_audio_init(void)
{
    // Clear POKEY shadow and LFSR
    for (int i = 0; i < 16; i++) pokey[i] = 0;
    lfsr_state = 0x1FFFFu;
    fill_noise_buf();   // pre-render the poly17 noise sample for noise-distortion voices

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

void paula_audio_shutdown(void)
{
    // Silence and disable audio DMA
    for (int ch = 0; ch < 4; ch++) AUD_VOL(ch) = 0u;
    *dmaconPointer = 0x000Fu;  // clear AUD0..3 (no SETCLR = clear)
}

// ---- platform bridge (C linkage, called from bus.h inlines in rof_gen.c) ------
extern "C" {

// Real Amiga vertical beam line (VPOSR/VHPOSR), defined in flight_native.cpp; used to
// derive a faithful ANTIC VCOUNT ($D40B) below.
unsigned short rof_beam_line(void);

uint8_t platform_hw_read(uint16_t addr)
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

void platform_hw_write(uint16_t addr, uint8_t val)
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

// These platform functions are needed to compile rof_gen.c cleanly.
// They're all no-ops on Amiga (the 6502 display/event code does nothing here).
void platform_shadow_write(uint16_t /*addr*/, uint8_t /*val*/) {}
void platform_register_vbi(uint16_t /*addr*/, void (*/*fn*/)(void)) {}
void platform_indirect_jmp(uint16_t /*addr*/) {}

// g_pumpQuit: the shared "user wants out" flag (defined in main.cpp, also set by the
// frame pump there on left-mouse).  g_quitJmp: the __builtin_setjmp buffer armed by
// RescueOnFractalus::run() so we can unwind the never-returning transpiled chain on quit.
extern "C" volatile uint8_t g_pumpQuit;
extern "C" void* g_quitJmp[];

// platform_poll_events: called from the transpiled spin-wait hooks (SPINWAIT_HOOKS in
// transpile.py) at the VCOUNT/CONSOL poll points that do NOT pace a frame.  Poll the
// quit control (left mouse) so the player can always abort even while the transpiled
// code spins in a tight non-frame wait, and unwind to run() if so.  Unlike
// platform_render_frame this must NOT wait for a VBI.
void platform_poll_events(void) {
    if (AmigaHardware::isLeftMouseButtonPressed()) g_pumpQuit = 1;
    if (g_pumpQuit) __builtin_longjmp(g_quitJmp, 1);   // escape the never-returning chain
}

// --- launch-cinematic blocking frame pump -----------------------------------
// The transpiled launch/audio code paces itself with blocking frame-waits
// (wait_frames_60 etc.), spinning on RTCLOK ($0014) and calling
// platform_tick_vbi()/platform_render_frame() each iteration.  When launch
// blocking mode is on, we turn those hooks into a REAL one-frame pump so the
// actual 6502 audio code (e.g. audf2_sweep_clear_colors for the doors) runs at
// the original cadence: tick advances RTCLOK once, render waits for the next
// real VBI and repaints the screen (so the native door/tunnel visuals keep
// animating while the transpiled code blocks).  main.cpp installs the pump.
static void (*s_framePump)(void) = 0;          // wait 1 VBI + render + poll quit
// Non-static so main.cpp's vbiHandler can read it: while launch-blocking, the ISR
// must NOT bump RTCLOK ($0014) — platform_tick_vbi advances it synchronously, once
// per transpiled spin iteration, to stay in lockstep with the wait loop (an async
// ISR bump racing the loop's RTCLOK reset would desync and hang ~256 frames).
volatile uint8_t g_launchBlocking = 0;

extern "C" void rof_set_frame_pump(void (*fn)(void)) { s_framePump = fn; }
extern "C" void rof_launch_blocking(uint8_t on)      { g_launchBlocking = on; }

void platform_render_frame(void) {
    if (g_launchBlocking && s_framePump) s_framePump();
}

void platform_tick_vbi(void) {
    // In LAUNCH-BLOCKING mode the ISR's RTCLOK bump is gated OFF (see g_launchBlocking in
    // main.cpp's vbiHandler); we advance RTCLOK here instead — exactly once per transpiled
    // spin iteration — so it stays in LOCKSTEP with the wait loop while platform_render_frame()
    // (s_framePump) still waits one real VBI for real-time pacing.  This is essential: the
    // wait_setcount/wait_frames_N spin ($3CB2) waits for RTCLOK_LOW to *equal* a target, so a
    // free-running ISR bump (racing a slow pumpFrame that spans >1 frame) would overshoot the
    // target and hang a full 256-tick wrap.  Mirrors the SDL platform's gated tickVBI exactly.
    if (g_launchBlocking) {
        mem[0x0014]++;                      // RTCLOK_LOW (mirrors vbiHandler)
        if (!mem[0x0014]) mem[0x0013]++;    // RTCLOK_MID carry
    }
}

// The tunnel-ring "dirty field" flags advance_message_column already uses to stream the
// ring-clear frames from the $1000 GTIA field into the tunnel bitmap (NativeHandlers.cpp).
extern volatile uint8_t g_tunnelFieldDirty;
extern volatile uint8_t g_tunRowLo, g_tunRowHi;
void platform_tunnel_rings_drawn(void) {
    // display_setup's draw_frame_pattern_seq just rendered the full ring pattern into the
    // $1000 field.  Flag the whole field dirty so the next pumpFrame decodes it once into
    // the tunnel bitmap (then advance_message_column streams the per-frame clear updates).
    g_tunRowLo = 0; g_tunRowHi = 85;
    g_tunnelFieldDirty = 1;
}
int  platform_load_image(const char* /*path*/) { return 0; }

} // extern "C"
