// Native launch-cinematic ports.
//
// The post-START launch cinematic ("STAND BY" + score, throttle gauge, doors,
// tunnel) runs LINEARLY inside the Atari display_setup ($5F1D), which draws each
// phase then busy-waits (wait_frames_60) before the next.  The Amiga is frame-
// driven (RescueOnFractalus::update per VBI), so we reproduce display_setup as a set
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
void draw_cockpit_dial_bar(void);             // $4447: $00BF=A+8 -> setup_dial_bar_draw -> draw_object_column

// Stars/planet phase (display_setup $64C8-$6594):
void clear_scroll_accum(void);                  // $6B71: zero $02C0-3 + scroll accum $A1-A5
void copy_192_to_1800(void);                    // $75A5: seed the $1810 lower viewport bank
void build_line_addr_table_1000_stride(void);   // $65D2: $073D/$0793 row-addr table, $1000 stride $C1
void init_object_positions(void);               // $6B85: seed planet object table $08A4..
void fill_terrain_columns(void);                // $6AE5: fill height buffers $0C32-$0F32 (POKEY RANDOM)
void scroll_terrain_columns(void);              // $6AEE: one column scroll step (A = $0089 on entry)
void advance_object_positions(void);            // $6BA8: one planet zoom step -> draw_vline_pair to $1000
void draw_player3_object(void);                 // $42A7: the planet as a scaled Player-3 object

// Door-opening sound ($6DF4): sweeps AUDF2 ($D202) by $08DB-=$08DC, $004C times,
// with a 1-frame wait (clear_colors) between — the genuine per-frame pitch sweep.
void audf2_sweep_clear_colors(void);
void input_init(void);                // $581C: load a voice (sound id in cpu.X) from the SFX param tables
void reorder_sprite_slot(void);       // $5614: SFX priority mixer — seat voice (cpu.Y) onto a POKEY channel
}

// Launch-cinematic blocking frame pump toggle (PaulaAudio.cpp): while on, the
// transpiled frame-waits drive a real one-VBI repaint (see main.cpp launchFramePump).
extern "C" void rof_launch_blocking(uint8_t on);

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
// Setting $060B=$23 makes RescueOnFractalus::update() stop calling the attract title
// writer (copy_text_block_to_screen_native is gated on $060B==0), so the
// RESCUE/(C)1985 toggle ceases and "STAND BY..." persists.
// audio_stop_native: port of audio_timer_setup ($712D) — the Atari "stop all
// sound" routine the launch path runs (display_setup $6337/$634F) the instant
// START is pressed ($0004 set → the attract loop branches to $634F).  It
// silences BOTH audio engines and all four POKEY voices:
//   712d  LDA #0 / STA $00E7   stop the SFX-sequencer tick (= the Standby music;
//                              the CIA-B Timer A ISR gates on mem[$00E7])
//         STA $0655            stop music_player_tick
//         STA $00E5
//         STA $D201/3/5/7      AUDC1-4 = 0  (mute all four voices)
//   7142  LDY #$60 / STY $D208 AUDCTL = $60
// On the Amiga, clearing mem[$00E7] makes the CIA-B ISR (sfxTimerHandler) stop
// ticking the sequencer; the AUDC writes must go through platform_hw_write so
// update_paula_channel drops each Paula channel's volume to 0 — otherwise the
// last waveform would keep sounding even though the sequencer is no longer
// updating it.  Clear the $00E7 gate FIRST so the ISR can't fire a fresh voice
// between here and the mutes.
extern "C" void audio_stop_native(void)
{
    mem[0x00E7] = 0x00;   // SFX-sequencer gate (Standby music) — clear first
    mem[0x0655] = 0x00;   // music_player_tick gate
    mem[0x00E5] = 0x00;
    platform_hw_write(0xD201, 0x00);   // AUDC1
    platform_hw_write(0xD203, 0x00);   // AUDC2
    platform_hw_write(0xD205, 0x00);   // AUDC3
    platform_hw_write(0xD207, 0x00);   // AUDC4
    platform_hw_write(0xD208, 0x60);   // AUDCTL
}

// launch_doors_sound_native: faithful port of the display_setup doors-opening
// sound ($6235-$62E4).  Runs the GENUINE transpiled audf2_sweep_clear_colors
// ($6DF4) for each door row, paced by real frame-waits — rof_launch_blocking(1)
// turns the transpiled wait hooks into a one-VBI repaint pump (main.cpp), so the
// AUDF2 pitch sweep on voice 2 (AUDC2=$44 poly4 buzz) plays at the original
// cadence while the native scroll_terrain_dl animates the doors in the VBI.  The
// interleaved gauge/throttle readout ($6268-$62B2) is handled natively and omitted.
//   $6237  AUDC2 ($D203) = $44                     poly4 buzz, vol 4
//   $623A  $08DB = $52 (start pitch)   $623F  $08DC = $01 (down-step)
//   $6244  phase 1: B7 = 0..$12, $004C = $6595[B7]; audf2_sweep_clear_colors
//   $6295  $00B9 = $0F                 $62B4  $08DC = $FF (SBC #$FF = +1, up-step)
//   $62B9  phase 2: B9 = $0F..1,       $004C = $6598[B9]; audf2_sweep_clear_colors
//   $62E4  AUDC2 = 0
extern "C" void launch_doors_sound_native(void)
{
    rof_launch_blocking(1);

    platform_hw_write(0xD203, 0x44);            // $6237 AUDC2 = poly4 buzz, vol 4
    mem[0x08DB] = 0x52;                         // $623A start pitch
    mem[0x08DC] = 0x01;                         // $623F down-step

    for (uint8_t b7 = 0; b7 < 0x13; b7++) {     // $6244 phase 1 (19 door rows)
        mem[0x004C] = mem[0x6595 + b7];
        audf2_sweep_clear_colors();
    }

    mem[0x00B9] = 0x0F;                         // $6295
    mem[0x08DC] = 0xFF;                         // $62B4 up-step (SBC #$FF = +1)
    for (uint8_t b9 = 0x0F; b9 != 0; b9--) {    // $62B9 phase 2
        mem[0x004C] = mem[0x6598 + b9];
        audf2_sweep_clear_colors();
    }

    platform_hw_write(0xD203, 0x00);            // $62E4 silence voice 2

    rof_launch_blocking(0);
}

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

// The Atari display_setup seeds two launch SFX with direct input_init calls (the
// standby port had deferred them).  Both play through the standby/launch VBI's
// update_gauge_digits voice engine (gated on $060B = $23, set by launch_show_standby).
// Ground-truthed against an atari800 -pokeyrec capture + the host build voice engine.
//
// The Atari seeds BOTH the instant START is pressed ($63A7, $63D0), but the door
// voice can't grab a POKEY channel until the doors actually open (~67 frames later) —
// the engine/other voices hold all four channels until then.  The Amiga, freshly
// reset, has no such competition, so seeding the door at START makes it play AT ONCE
// (bending up then holding forever) and mask the arpeggio.  So we split them: the
// arpeggio at START (openDoors), the door swoosh when the doors begin scrolling
// (startDoors), and we silence the door when the doors finish (it has dur $06A3=0, so
// it never self-terminates — on the Atari the tunnel SFX supersede it).

// launch_start_arpeggio_native: the START bleep — sound id $1D, which self-chains
// $1D -> $1E -> $12 (rising 3-blip arpeggio an octave apart, distort $A0, freq
// $3F/$1F/$0F, ~0.2s) via update_gauge_digits' $06F7 loop.  ($63A7)  Must run after
// sfx_engine_reset_native (openDoors order) or the reset would wipe the seed.
extern "C" void launch_start_arpeggio_native(void)
{
    cpu.X = 0x1D; input_init();          // $63A7: START arpeggio
}

// launch_door_swoosh_native: the door-open swoosh — sound id $01, a poly-buzz on a
// voice (AUDC distort $40, vol 4) whose pitch sweeps $3A->$1F then holds (rate
// $06DB=1, target $06CD=$1F).  ($63D0)  Seeded into voice slot 5 ($56D4[0]).
extern "C" void launch_door_swoosh_native(void)
{
    cpu.X = 0x01; input_init();          // $63D0: door-open swoosh
}

// launch_door_swoosh_stop_native: silence the door voice (slot 5) once the doors are
// fully open.  $01 has dur=0 so update_gauge_digits would replay it forever; clear its
// envelope arrays and zero its assigned POKEY channel's AUDC so it actually goes quiet.
extern "C" void launch_door_swoosh_stop_native(void)
{
    uint8_t reg = mem[0x0705 + 5];                 // POKEY reg index the mixer gave slot 5
    if (reg) platform_hw_write((uint16_t)(0xD1FF + reg), 0x00);  // AUDCn = 0 (silence)
    mem[0x065D + 5] = 0x00;   // distort
    mem[0x066B + 5] = 0x00;   // priority -> mixer drops the voice
    mem[0x06A3 + 5] = 0x00;   // duration
    mem[0x06DB + 5] = 0x00;   // rate
    mem[0x0705 + 5] = 0x00;   // release the channel
}

// ---- launch engine-ramp (display_setup $3DD3-$3DF9 cold-init + $6460-$64E8) -----
// Faithful port of the Atari engine "spin-up": the engine voices' distortion is set
// once (cold-init), then as the doors finish and the tunnel plays, the engine BODY
// voice (slot 12 = $0C, noise) is seeded LOUD (priority $0F) and ramped down to its
// steady priority ($04) while its companion (slot 11 = $0B) is seeded — so the engine
// audibly spins up across the launch.  update_gauge_digits ($548D, running in the
// standby/launch VBI gated on $060B=$23) drains these onto POKEY/Paula.  In flight,
// flight_control_integrate ($8E5B @ $91DC, Y=$0C) rewrites slot 12's priority + freq
// every frame from the ship pitch, so the drone then tracks the throttle.
//
// Faithful to the engine-VOICE operations of $6460-$64E8; the gauge-readout spins
// ($645B/$646C/$6478, on slot freqs $0684/$0686) and HUD digit writes interleaved
// there belong to the throttle-gauge phase, which the Amiga sequences natively, so
// they are not reproduced here (the doors/lights/tunnel order is preserved).

// $3DD3-$3DF9 (voice subset): the engine voices' distortion + the two persistent
// cold-seed sounds.  Run once at launch start, AFTER sfx_engine_reset (which would
// otherwise wipe these), so the voices have a waveform before the ramp seeds them.
extern "C" void launch_engine_voice_init(void)
{
    mem[0x0668] = 0x80;   // $3DDF slot 11 distort = NOISE
    mem[0x0669] = 0x80;   // $3DE2 slot 12 distort = NOISE (the engine body)
    mem[0x0663] = 0xA0;   // $3DEA slot  6 distort = pure
    mem[0x066A] = 0xA0;   // $3DED slot 13 distort = pure
    mem[0x066B] = 0xA0;   // $3DF0 slot 14 distort = pure
    cpu.X = 0x1F; input_init();   // $3DF5 seed persistent voice id $1F
    cpu.X = 0x20; input_init();   // $3DF9 seed persistent voice id $20
}

// $6487-$6493: seed the engine body voice (slot 12) at a loud onset priority and
// seat it on a POKEY channel.  Called once the doors are fully open + lights lit.
extern "C" void launch_engine_seed_start(void)
{
    mem[0x066B + 0x0C] = 0x0F;   // $6487 slot 12 priority = $0F (loud onset)
    mem[0x0679 + 0x0C] = 0xB4;   // $6490 slot 12 frequency
    cpu.Y = 0x0C; reorder_sprite_slot();   // $6493 seat slot 12 on a POKEY channel
}

// $64B0-$64C6 body (one tunnel-ring step): ramp the engine body voice's priority
// $0F -> $08 as the tunnel plays — the engine settling from its loud onset.
extern "C" void launch_engine_ramp_step(void)
{
    if (mem[0x066B + 0x0C] != 0x08) mem[0x066B + 0x0C]--;   // $64B3-$64BD
}

// $64C8-$64E6: engine steady-state once the tunnel clears.  Engine body (slot 12)
// settles to priority $04 / freq $65; companion (slot 11) to priority $01 / freq $0E;
// each re-seated on a POKEY channel.  ($64CD draw_cockpit_dial_bar(A=4) redraws the dial bar.)
extern "C" void launch_engine_steady(void)
{
    mem[0x066B + 0x0C] = 0x04;             // $64C8 slot 12 priority = 4
    cpu.A = 0x04; draw_cockpit_dial_bar();         // $64CD redraw the dial bar at level 4
    mem[0x0679 + 0x0C] = 0x65;             // $64D2 slot 12 frequency
    cpu.Y = 0x0C; reorder_sprite_slot();   // $64D7
    mem[0x066B + 0x0B] = 0x01;             // $64DA-$64DE slot 11 priority = 1
    mem[0x0679 + 0x0B] = 0x0E;             // $64E1-$64E3 slot 11 frequency
    cpu.Y = 0x0B; reorder_sprite_slot();   // $64E6
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

// ---- effects 3 & 4: left indicator-light column -----------------------------
// The left lights are the lower half (rows 8..15) of the cockpit dial-bar drawn
// by draw_cockpit_dial_bar -> setup_dial_bar_draw -> draw_object_column ($43E8): for each
// row X (=$00BD, $0F..$08) it writes cell $4581[X] with bit7 = lit/unlit set by
// X >= $00BF (lit) vs X < $00BF (unlit).  $00BF = A+8.  Lit = $34/$37/$38 (COLPF2),
// unlit = $B4/$B7/$B8 (COLPF3).  The existing render() draws these cockpit cells.

// Effect 3: when the doors start, light the bottom-left indicator only.
//   display_setup $63FD: LDA #$07 / draw_cockpit_dial_bar  ($00BF=$0F -> only row 15 = $34E5 lit)
extern "C" void launch_light_doorstart_native(void)
{
    cpu.A = 0x07;
    draw_cockpit_dial_bar();
}

// Effect 4: before the tunnel animates, light the whole left column.
//   display_setup $6482: LDA #$00 / draw_cockpit_dial_bar  ($00BF=$08 -> rows 8..15 all lit)
extern "C" void launch_light_all_native(void)
{
    cpu.A = 0x00;
    draw_cockpit_dial_bar();
}

// ---- stars / space phase (display_setup $64C8-$6552 setup) ------------------
// Once the tunnel ring auto-clears $0088 (advance_message_column, NativeHandlers),
// the cinematic switches the viewport to ANTIC mode-D from $1000 (VDSLST $6CC2),
// pre-fills the fractal terrain-column buffers, and scrolls them while the planet
// builds.  We port the mem[]-state subset of $64C8-$6552 (HW register writes —
// SIZEM/HPOSP/DLIST — are the Amiga's copper/sprite job) and call the genuine
// transpiled setup routines.  build_line_addr_table_1000 points the draw row-addr
// table $073D/$0793 at $1000 stride $30 (=48, wide playfield) so the planet's
// draw_vline_pair writes land in the displayed buffer.
extern "C" void launch_stars_init_native(void)
{
    clear_scroll_accum();                       // $6506 ($6B71)
    mem[0x00B5] = 0x62;                          // $64F8

    // Clear the mode-D viewport buffer $1000..$1810 (43 rows * 48 bytes).
    for (uint16_t i = 0; i < 43u * 48u; i++) mem[0x1000 + i] = 0u;   // $650B loop equiv
    copy_192_to_1800();                          // $6521 ($75A5): seed lower bank $1810

    mem[0x00DC] = 0x00;                          // $6524: viewport COLBK source = black
    mem[0x0071] = 0x00;                          // $6528
    mem[0x3157] = 0x10; mem[0x3158] = 0x18;      // $652A: DL LMS bytes (-> $1810; copper ignores)
    mem[0x0200] = 0xC2; mem[0x0201] = 0x6C;      // $6537: VDSLST = $6CC2 (stars/planet DLI)

    init_object_positions();                     // $654B ($6B85): seed planet object table
    mem[0x0089] = 0x7F;                          // $654E: arm scroll_terrain_columns
    fill_terrain_columns();                      // $6552 ($6AE5): fill height buffers

    cpu.A = 0x30;                                // $6569: stride $30 (48, wide)
    build_line_addr_table_1000_stride();         // $65D2 -> $073D/$0793 point at $1000
}

// One stars-scroll step (the $6557-$656E warm-up + spin, frame-driven): the VBI
// dispatcher's $0089 branch is JMP $6AEE with A = $0089, so set A then call it.
// scroll_terrain_columns sets $0089 = 2 when the scroll accumulator reaches its
// mark; display_setup leaves the stars phase once $0089 < 4 (i.e. == 2).  Returns
// 1 while still scrolling, 0 once $0089 has dropped below 4 (advance to planet).
extern "C" uint8_t launch_stars_step_native(void)
{
    cpu.A = mem[0x0089];
    scroll_terrain_columns();                    // $6AEE
    return (mem[0x0089] >= 4u) ? 1u : 0u;        // $656E: CMP #4 / BPL spin
}

// ---- planet phase (display_setup $6574-$6594) -------------------------------
// The planet "zooms up" as advance_object_positions ($6BA8) steps its scaled
// object table and draw_vline_pair writes the growing sphere into $1000, plus
// draw_player3_object ($42A7) renders it as a scaled Player-3 sprite.  The loop
// runs every other frame ($6578: $0014 >= 2) until $1002 == $FF, then $0089 = 0
// and display_setup returns to flight.  Returns 1 while zooming, 0 when done.
extern "C" uint8_t launch_planet_step_native(void)
{
    advance_object_positions();                  // $6BA8 -> update_object_distance -> draw_vline_pair
    // draw_player3_object ($42A7) renders the planet as Player-3 — a heavy TRANSPILED
    // 6502 routine.  We don't render P3 as an Amiga sprite (sprite 3 stays null), so
    // its output is invisible; skipping it removes the dominant per-step cost.  The
    // visible sphere comes entirely from advance_object_positions -> draw_vline_pair
    // into $1000, and the $1002==$FF "risen" mark is set there, not by P3.
    if (mem[0x1002] == 0xFFu) {                   // $6585: planet fully risen
        mem[0x0089] = 0x00;                       // $658C
        return 0u;
    }
    return 1u;
}

// During the planet zoom the Atari VBI keeps dispatching scroll_terrain_columns
// via the $0089 branch ($5367), so the starfield keeps drifting up — but with
// $0089 == 2 (< 4) the routine takes the scroll-only path (shift up by one
// scanline, NO new dots appended).  launch_planet_step_native runs only every
// other frame; this scroll must run EVERY frame to match the VBI cadence.
extern "C" void launch_planet_scroll_native(void)
{
    cpu.A = mem[0x0089];                          // == 2: A < 4 -> $6AEE scroll-only branch
    scroll_terrain_columns();                     // $6AEE
}
