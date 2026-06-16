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
#include "../framework/AmigaHardware.h"
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
void PlatformAmiga::noiseTick()
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
void PlatformAmiga::audioInit()
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

void PlatformAmiga::audioShutdown()
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

// g_pumpQuit: the shared "user wants out" flag — set by the frame pump and the poll
// hook on left-mouse.  g_quitJmp: the __builtin_setjmp buffer armed by
// RescueOnFractalus::run() so we can unwind the never-returning transpiled chain on quit
// (5 words per the GCC builtin; initializer forces the definition).  g_vbiCount: bumped
// once per REAL vertical-blank interrupt (vbiHandler below); the frame pump spins on it
// as the matching Amiga construct for the Atari frame-wait busy-loops.
volatile uint8_t  g_pumpQuit   = 0;
void*             g_quitJmp[5]  = { 0, 0, 0, 0, 0 };
volatile uint16_t g_vbiCount   = 0;

// g_launchBlocking: gate for the launch-cinematic frame pump.  While on, vbiHandler must
// NOT bump RTCLOK ($0014) — platform_tick_vbi advances it synchronously, once per
// transpiled spin iteration, to stay in lockstep with the wait loop (an async ISR bump
// racing the loop's RTCLOK reset would desync and hang ~256 frames).
volatile uint8_t g_launchBlocking = 0;

// s_scene: the running scene, set by PlatformAmiga::run; the frame pump repaints through it.
static RescueOnFractalus* s_scene = 0;

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
// the original cadence: render() waits for the next real VBI (so the ISR advances
// RTCLOK and the native door/tunnel visuals animate), repaints the screen via the
// scene, and polls quit — escaping the never-returning chain on left-mouse.
static void launchFramePump(void) {
    uint16_t last = g_vbiCount;
    while (g_vbiCount == last) { /* wait for next real VBI */ }
    if (s_scene) s_scene->pumpFrame();   // full repaint body
    if (AmigaHardware::isLeftMouseButtonPressed()) g_pumpQuit = 1;
    if (g_pumpQuit) __builtin_longjmp(g_quitJmp, 1);   // escape the never-returning chain
}

void rof_launch_blocking(uint8_t on) { g_launchBlocking = on; }

void platform_render_frame(void) {
    if (g_launchBlocking) launchFramePump();
}

void platform_tick_vbi(void) {
    // In LAUNCH-BLOCKING mode the ISR's RTCLOK bump is gated OFF (see g_launchBlocking in
    // vbiHandler below); we advance RTCLOK here instead — exactly once per transpiled
    // spin iteration — so it stays in LOCKSTEP with the wait loop while platform_render_frame()
    // (launchFramePump) still waits one real VBI for real-time pacing.  This is essential: the
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
    // RTCLOK ($0014 low, carry $0013) ownership depends on the phase:
    //  * LAUNCH-BLOCKING (g_launchBlocking=1): the transpiled frame-waits use an EXACT-
    //    equality spin (wait_setcount/wait_frames_N $3CB2 waits for RTCLOK_LOW to *equal*
    //    a target).  RTCLOK must advance by EXACTLY ONE per spin iteration (platform_tick_vbi
    //    does that); if the free ISR ALSO bumped it, a slow pumpFrame (>1 frame) would
    //    overshoot the target and hang a full 256-tick wrap.  So the ISR must NOT bump
    //    RTCLOK here while launch-blocking (lockstep, exactly as the SDL gated tickVBI).
    //  * FLIGHT / steady state (g_launchBlocking=0): the ISR owns RTCLOK, bumped once per
    //    real VBI as the Atari OS / in-game VBI did.
    //  * ATTRACT VBI ($1B30): bumps RTCLOK itself in its own transpiled body, so skip here
    //    (else double).  (Do NOT touch $0080 — sync_flag, reused as the $80/$81 zp pointer.)
    uint16_t vbiVec = (uint16_t)(mem[0x0222] | (mem[0x0223] << 8));
    if (vbiVec != 0x1B30u && !g_launchBlocking) {
        mem[0x0014]++;                    // RTCLOK_LOW
        if (!mem[0x0014]) mem[0x0013]++;  // RTCLOK_MID carry
    }
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
//  PlatformAmiga::run — display takeover, install interrupts, run scene, restore
// ============================================================================
void PlatformAmiga::run(RescueOnFractalus& scene)
{
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
    // whose frame-wait spin loops each drive one real Amiga frame through launchFramePump
    // (the pump spins on g_vbiCount).  Returns when the user quits (left mouse button).
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
