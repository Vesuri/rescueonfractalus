// Native 68000 reimplementation of sfx_voice_tick + sfx_seq_step.
// Replaces the 6502-transpiled versions (rof_gen.c $70F9/$7148) which emulate
// 6502 registers/flags on every "instruction" — ~60x too slow at 7 MHz.
//
// State in mem[] (identical to Atari):
//   $073A  — duration countdown (decrements each call; sfx_seq_step on underflow)
//   $073B  — gate/mute (upper nibble of last note byte; 0 = silence)
//   $073C  — sequence pointer (index into $71DB table)
//   $0091  — last voice-param command byte (scratch)
//
// Tables in mem[] (game ROM, loaded from screen3_mem.bin):
//   $71DB  — sequence byte stream (0=loop to 0, 0x01-0x7F=note, 0x80-0xFF=voice cmd)
//   $71D2  — duration table indexed by note & 0x1F
//   $71AB/$719E/$7191/$71B8  — AUDF1-4 per voice index
//   $71C5  — AUDC4 per voice index

#include "PaulaAudio.h"

extern "C" volatile uint8_t mem[65536];
// platform_hw_write declared in PaulaAudio.h (extern "C")

// ---- sfx_seq_step_native ($7148) — also called from paula_audio_init() ------
// Advances the sequence pointer and loads the next note.
//   Negative bytes (0x80-0xFF): voice-param command — write AUDF1-4+AUDC4,
//     continue if AUDC4 != 0, else fall through with note=0.
//   Zero: reset pointer to 0 and re-read (sequence loop).
//   Positive (0x01-0x7F): note byte — decode duration+gate and stop.
extern "C" void sfx_seq_step_native(void)
{
    uint8_t x = mem[0x073C];
    uint8_t note;

    for (;;) {
        x++;                                        // INX
        uint8_t cmd = mem[0x71DB + x];
        if (cmd == 0) { x = 0; cmd = mem[0x71DB]; } // TAX / re-read table[0]

        if ((int8_t)cmd >= 0) {                     // positive: note command
            note = cmd;
            break;
        }

        // Negative: voice-parameter command
        mem[0x0091] = cmd;
        uint8_t v = cmd & 0x1Fu;                    // voice index = lower 5 bits
        platform_hw_write(0xD200, mem[0x71AB + v]); // AUDF1
        platform_hw_write(0xD202, mem[0x719E + v]); // AUDF2
        platform_hw_write(0xD204, mem[0x7191 + v]); // AUDF3
        platform_hw_write(0xD206, mem[0x71B8 + v]); // AUDF4
        uint8_t audc4 = mem[0x71C5 + v];
        platform_hw_write(0xD207, audc4);
        if (audc4 != 0) continue;                   // keep processing commands
        note = 0;                                   // audc4==0 → treat as rest
        break;
    }

    // L_717b: save pointer, decode duration and gate
    mem[0x073C] = x;
    mem[0x073A] = mem[0x71D2 + (note & 0x1Fu)];    // duration from table
    mem[0x073B] = note >> 4;                        // gate = upper nibble
}

// ---- sfx_voice_tick_native ($70F9) ------------------------------------------
// Driven by CIA-B Timer A (main.cpp) when mem[$00E7] != 0.  On the Atari this
// runs from vbi_deferred_dispatch ($534D) every *other* VBI — gated by
// BIT $062D bit 0 with $00E7=1 — i.e. 25 Hz on PAL, which the CIA timer matches.
// Decrements the duration counter; on underflow calls sfx_seq_step_native.
// Computes an AUDC amplitude value and writes it to POKEY channels 1-3.
//
// AUDC computation (matches 6502 exactly):
//   half = mem[$073A] >> 1
//   if half < 3:  audc = half + 0xA0  (CMP carry = 0)
//   if half >= 3: audc = 0xA3         (CMP carry = 1 → LDA #2 + ADC #0xA0 + C)
extern "C" void sfx_voice_tick_native(void)
{
    mem[0x073A]--;
    if ((int8_t)mem[0x073A] < 0) sfx_seq_step_native();

    uint8_t half = mem[0x073A] >> 1;
    uint8_t audc = (half < 3) ? (uint8_t)(half + 0xA0u) : 0xA3u;

    uint8_t gate = mem[0x073B];
    if (gate == 0) {
        platform_hw_write(0xD201, 0);
        platform_hw_write(0xD203, 0);
        platform_hw_write(0xD205, 0);
    } else {
        platform_hw_write(0xD201, audc);
        platform_hw_write(0xD203, audc);
        platform_hw_write(0xD205, audc);
        // Original: STA $D1FF,Y (Y=gate) — writes audc+2 to a POKEY register.
        // Route through platform_hw_write so Paula + mem[] both see it.
        platform_hw_write((uint16_t)(0xD1FFu + gate), (uint8_t)(audc + 2u));
    }
}
