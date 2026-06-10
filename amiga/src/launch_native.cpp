// Native launch-cinematic ports.
//
// The post-START launch cinematic ("STAND BY" + score, throttle gauge, doors,
// tunnel) runs LINEARLY inside the Atari display_setup ($5F1D), which draws each
// phase then busy-waits (wait_frames_60) before the next.  The Amiga is frame-
// driven (StandbyScene::update per VBI), so we reproduce display_setup as a set
// of one-shot phase steps invoked at the right moments rather than one blocking
// routine.
//
// Each phase's DRAWING is done by the genuine 6502 routines: the full transpile
// (src/gen/rof_gen.c) is already linked into the Amiga build (it backs the SFX),
// so we call those functions directly instead of re-implementing them — they are
// the 6502 code "converted to native C", with zero divergence risk.  Being one-
// shot, their CPU-emulation overhead is irrelevant.  The interrupt-time audio is
// fully native (SfxPlayer.cpp) and never touches the shared `cpu` register
// struct, so driving cpu-based transpiled code from the main loop is race-free.

#include "PaulaAudio.h"   // mem[] + int types (matches the other native modules)
extern "C" volatile uint8_t mem[65536];

// The transpile's 6502 register file (src/cpu/cpu.h: `Cpu6502 cpu`).  We mirror
// its POD layout here rather than #include cpu.h, which drags in the real
// <stdint.h> and clashes with the Amiga framework's compat typedefs.  Field
// order/types must match cpu.h exactly so this binds to the same `cpu` symbol.
extern "C" {
typedef struct { uint8_t A, X, Y, S, N, V, Z, C, I, D; } Cpu6502;
extern Cpu6502 cpu;

// Genuine transpiled launch routines (src/gen/rof_gen.c):
void save_color_clear_y_bit5(void);   // $47B2: $00D8=A, Y&=$DF, -> show_cockpit_message ($47B8)
void render_bcd_counter(void);        // $49A0: render 6-digit BCD score ($0600-3) to $32C5-$32CA
}

// launch_show_standby_native: port of display_setup $635F-$63AF (cinematic
// effect 1).  Replaces the attract title scroll with the static "STAND BY..."
// message and the score (0 -> "     0") on the cockpit message line.
//
//   $635F  LDA #$23 / STA $08A3 / STA $060B / STA $006C
//   $6369  LDY #$0B (="STAND BY...";  #$13 if $0004!=0)
//   $6371  LDA #$EA ; save_color_clear_y_bit5  (sets $00D8, writes the message)
//   $6376  LDA #$00 / STA $0629
//   $6391  clear score field $0600-4 and on-screen cells $32C5-$32CA
//   $63A1  STA $0627 (A=0)
//   $63AF  render_bcd_counter  (writes "     0")
//
// Setting $060B=$23 makes StandbyScene::update() stop calling the attract title
// writer (copy_text_block_to_screen_native is gated on $060B==0), so the
// RESCUE/(C)1985 toggle ceases and "STAND BY..." persists.
extern "C" void launch_show_standby_native(void)
{
    mem[0x08A3] = 0x23;
    mem[0x060B] = 0x23;   // stops the attract title toggle
    mem[0x006C] = 0x23;

    cpu.A = 0xEA;
    cpu.Y = (mem[0x0004] == 0) ? 0x0B : 0x13;
    save_color_clear_y_bit5();          // -> show_cockpit_message: "STAND BY..." into $32B7

    mem[0x0629] = 0x00;

    // The genuine code gates this clear (carry skips it on the first level
    // change), relying on an earlier full message-line clear that the Amiga's
    // shortcut never ran; the score cells still hold attract-title scroll bytes
    // here, so run the clear unconditionally to reach the same state.
    for (int i = 0; i <= 4; i++) mem[0x0600 + i] = 0x00;   // $6391 score BCD field
    for (int i = 0; i <= 5; i++) mem[0x32C5 + i] = 0x00;   // $639B on-screen cells

    mem[0x0627] = 0x00;

    // (display_setup also calls compute_gauge_geometry_from_006D / input_init /
    // vobj_draw_dispatch here; those drive the gauge PMG and belong to effect 2,
    // so they are added when the gauge fill is ported.)
    render_bcd_counter();               // "     0" -> $32C5-$32CA
}

// ---- effect 2: throttle gauge fill (vobj vertical object) -------------------
// The throttle gauge is the Atari "vertical object": a player-1 P/M strip at
// $0D98 that fills downward as the object steps to the bottom ($DC).  Each step
// (vobj_step_down $41E8 / vobj_erase_row $4207) advances the vert position $062F
// by +4 and writes $F0 (a 4px-wide bar segment) into the strip row indexed by
// ($DC - $062F) >> 2, with a per-step frame wait.  On the frame-driven Amiga we
// run ONE step per call (the 6502's wait becomes the caller's frame cadence) and
// report when the bar has reached the bottom.
//
// The fill writes $0D99+index (index 0..55, 56 rows) plus $0D98 on the wrap, so
// the strip spans $0D98..$0DD0 (the door savestate shows 57 contiguous $F0).
// $062F runs 0 -> $DC (step +4); index = ($DC - $062F) >> 2 goes 55 -> 0, i.e.
// the bar fills bottom (index 55) to top (index 0).  The colour for each segment
// is $4DEA[index>>3] (the "shape" table is actually a colour ramp), ending at
// $4DEA[0]=$D6 (#560) when full — see vobj_erase_row ($4207) / vobj_advance.

static const uint8_t kVobjBottom = 0xDC;   // $00DC bottom limit

extern "C" void launch_gauge_init_native(void)
{
    mem[0x062F] = 0x00;                      // vert position at the top of travel
    for (uint16_t i = 0; i < 57; i++)        // clear the strip ($0D98..$0DD0)
        mem[0x0D98 + i] = 0x00;
    mem[0x00DE] = mem[0x4DEA + 6];           // initial (bottom) segment colour
}

// One vobj_step_down ($41E8) + vobj_erase_row ($4207) iteration, minus the 6502
// 1-frame wait (the caller paces it).  Returns 1 while filling, 0 once full.
extern "C" uint8_t launch_gauge_step_native(void)
{
    uint16_t pos = (uint16_t)mem[0x062F] + 4u;   // $41ED: LDA #4 / ADC $062F
    uint8_t done = 0;
    if (pos >= kVobjBottom) {                    // $41F4: CMP $DC, reached bottom
        mem[0x0D98] = 0xF0;                      // $41FA
        mem[0x062F] = kVobjBottom;               // $4201
        done = 1;
    } else {
        mem[0x062F] = (uint8_t)pos;              // $4201
    }
    // vobj_erase_row: index Y = ($DC - $062F) >> 2 ; $0D99+Y = $F0 ; colour ramp
    uint8_t y = (uint8_t)((kVobjBottom - mem[0x062F]) >> 2);
    mem[0x0D99 + y] = 0xF0;
    mem[0x00DE] = mem[0x4DEA + (y >> 3)];        // $421C: $00DE = $4DEA[Y>>3]
    return done ? 0u : 1u;
}
