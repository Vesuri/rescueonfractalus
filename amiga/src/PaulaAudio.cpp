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
// Memory snapshot: disasm/rof_mem.bin (64 KB flat) is embedded in .rodata via
// incbin.s and copied verbatim into mem[] on init.  It is the post-loader state
// captured at game_entry() — all attract tables are already correctly initialised.

#define ECS_SPECIFIC
#include <hardware/dmabits.h>
#include "../framework/AmigaHardware.h"
#include "PaulaAudio.h"

// mem[] and cpu are defined in src/cpu/cpu.c (compiled for m68k as audio/cpu.o)
extern "C" uint8_t mem[65536];

// 64 KB flat memory snapshot embedded in incbin.s
extern "C" uint8_t rof_mem_bin[];
extern "C" uint8_t rof_mem_bin_end[];

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
    bool poly4     = !(audc & POKEY_PURETONE) && (audc & POKEY_NOTPOLY5) && (audc & POKEY_POLY4); // $C0
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

// ---- XEX loader --------------------------------------------------------------
static void load_mem_snapshot(void)
{
    // Flat 64 KB copy — rof_mem.bin is the post-loader state, ready to run.
    const uint8_t* src = rof_mem_bin;
    for (uint32_t i = 0; i < 65536u; i++) {
        mem[i] = src[i];
    }
}

// Forward declaration — defined in SfxPlayer.cpp
extern "C" void sfx_seq_step_native(void);

// ---- public interface --------------------------------------------------------
void paula_audio_init(void)
{
    load_mem_snapshot();

    // Clear POKEY shadow and LFSR
    for (int i = 0; i < 16; i++) pokey[i] = 0;
    lfsr_state = 0x1FFFFu;

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

uint8_t platform_hw_read(uint16_t addr)
{
    if (addr == 0xD20Au) return pokey_random_step();  // POKEY RANDOM register
    if (addr >= 0xD200u && addr < 0xD210u) return pokey[addr - 0xD200u];
    // PIA PORTA ($D300): Atari joysticks are ACTIVE-LOW (1 = open/neutral).  With
    // no Amiga joystick wired in yet, report neutral ($FF) so flight_control_integrate
    // reads "stick centred, no fire" — the ship flies straight instead of jamming.
    if (addr == 0xD300u) return 0xFFu;
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
void platform_render_frame(void) {}
void platform_poll_events(void) {}

// When set (by flight_init_native), advance RTCLOK ($0014) inside transpiled
// frame-wait spin loops so they resolve in compute time instead of waiting on the
// real Amiga VBI.  flight init's init_gameplay_state calls wait_frames_60 six times
// (~5s of launch-sequence pacing we don't want on the dev F-skip).  Steady-state
// flight leaves this 0, so $0014 advances at the real one-per-frame VBI rate.
volatile uint8_t g_fastForwardFrames = 0;
void platform_tick_vbi(void) {
    if (g_fastForwardFrames) {
        mem[0x0014]++;                      // RTCLOK_LOW (mirrors vbiHandler)
        if (!mem[0x0014]) mem[0x0013]++;    // RTCLOK_MID carry
    }
}
int  platform_load_image(const char* /*path*/) { return 0; }

} // extern "C"
