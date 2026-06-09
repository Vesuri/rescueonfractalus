// Native 68000 translations of Atari VBI handler fragments used by StandbyScene.
//
// The full vbi_handler_game ($52D7) and vbi_handler_2 ($4FF5) do many things:
// hardware register writes (DMACTL, CHBASE, COLPF0-3, HPOSP, PRIOR), display-list
// management, sound dispatch, and object animation.  On the Amiga:
//   • Copper handles all hardware register writes.
//   • main.cpp VBI interrupt server handles RTCLOK ($0014/$0013/$0080).
//   • SfxPlayer handles the audio (sfx_voice_tick_native).
// Only the pure mem[]-state fragments below remain.

#include "PaulaAudio.h"   // for mem[] + uint types
#include "AtariZp.h"      // zp:: named Atari memory offsets
extern "C" volatile uint8_t mem[65536];

// vbi_attract_timer_native: fragment of vbi_handler_game @ $52D7 relevant to
// Standby.  The full handler also writes DMACTL/CHBASE/colour/HPOS registers
// (handled by Copper) and increments $0014 (handled by main.cpp VBI server).
// This fragment is lines $5335-$533A:
//   INC_M($062D)           — attract timer sub-counter
//   if Z: INC_M($00E2)     — cascade to attract_timer on 256-wrap
extern "C" void vbi_attract_timer_native(void)
{
    mem[zp::attractTimerSub]++;
    if (mem[zp::attractTimerSub] == 0) mem[zp::attractTimer]++;
}

// copy_text_block_to_screen_native: direct translation of $782A.
// Fires when mem[$0091] >= $C0; the SFX sequencer (sfx_seq_step_native) writes
// negative sequence bytes — $C0/$C4 = Block1 ("rescue on fractalus"), $E2 =
// Block2 ("©1985...") — to mem[$0091] as a side effect of voice-param processing.
// Gate at $0091==$C0 exactly: only fires when mem[$00E2] is non-negative (< $80).
// Copies 20 chars from $5A9F (Block1) or $5AB3 (Block2) into $32B7-$32CA.
// Also writes mem[$00D8]=$44 (title COLBK) for the copyright path.
extern "C" void copy_text_block_to_screen_native(void)
{
    uint8_t alt = mem[zp::altitudeThreshold];
    if (alt < 0xC0u) return;                                        // CMP $C0, BCC
    if (alt == 0xC0u && (int8_t)mem[zp::attractTimer] < 0) return;           // attract-timer gate
    mem[zp::altitudeThreshold] = 0;                                                // reset (Y=0 in original)
    uint16_t src;
    if (alt >= 0xE0u) {
        src = 0x5AB3u;          // Block2: copyright string ($5A9F+$13, per $7845)
        mem[zp::textColorPf0] = 0x44u;    // title text colour for copyright path
    } else {
        src = 0x5A9Fu;          // Block1: rescue string ($5A9F+$00, per $7849)
    }
    for (int i = 0; i < 20; i++)
        mem[0x32B7u + (uint16_t)i] = mem[src + (uint16_t)i];
}

// update_cockpit_digits_native: direct translation of startup_init @ $3FFA.
// Updates three cockpit digit displays based on mem[$0642], mem[$0641], mem[$0628].
// Each digit is a 2×2 block of mode-4 chars from the $4AE3 glyph table (10 entries,
// 4 bytes each: top-left, top-right, bottom-left, bottom-right).  Writes go to
// (dest+0), (dest+1), (dest+$30), (dest+$31) — the +$30 skips one mode-4 row
// (40 bytes) plus 8 column positions.  Only called when mem[$004A] != 0 (active game).
//
// Digit 1: mem[$0642] lower nibble   → dest = $33B4
// Digit 2: mem[$0641] upper nibble   → dest = $3413 (tens)
//           mem[$0641] lower nibble  → dest = $3445 (units)
// Digit 3: mem[$0628] upper nibble   → dest = $3472 (tens)
//           mem[$0628] lower nibble  → dest = $34A4 (units)
//           mem[$0628] is OR'd with $80 when mem[$062B]!=0 and !(0x0C&mem[$004B])
//
// Additionally writes mem[$33DF]/$33E0 (display-list stride control) as $9E/$9D
// when mem[$0642] is 1 or 2 and mem[$004B] passes the BIT test; otherwise $1E/$1D.
extern "C" void update_cockpit_digits_native(void)
{
    // helper: write a 2×2 digit block from table $4AE3[idx*4] to dest, OR'ing flag
    auto writeDigit = [](uint16_t dest, uint8_t idx, uint8_t flag) {
        uint16_t t = (uint16_t)(0x4AE3u + (uint16_t)(idx << 2u));
        mem[dest + 0u]     = mem[t + 0u] | flag;
        mem[dest + 1u]     = mem[t + 1u] | flag;
        mem[dest + 0x30u]  = mem[t + 2u] | flag;
        mem[dest + 0x31u]  = mem[t + 3u] | flag;
    };

    mem[zp::barColThreshold] = 0u;
    uint8_t y = 0x1Eu;
    uint8_t a = mem[zp::gamePhaseFlag];

    if (a >= 1u && a < 3u && (a & mem[zp::collisionFlags]) == 0u) {
        if (mem[zp::playerLives] != 0u) {
            // game_sub_5815(X=$14): push (a|$80) into altitude ring buffer at $0719
            uint8_t ptr = mem[zp::altRingHead];
            if (ptr >= 0x20u) ptr = 0x1Fu;
            mem[0x0719u + ptr] = a | 0x80u;
            mem[zp::altRingHead] = (ptr == 0u) ? 0x1Fu : (uint8_t)(ptr - 1u);
        }
        y = 0x9Eu;
    }
    mem[0x33DFu] = y;
    mem[0x33E0u] = (uint8_t)(y - 1u);

    // Digit 1: lower nibble of mem[$0642], change-detected against mem[$0647]
    if (a != mem[zp::digitCache647]) {
        mem[zp::digitCache647] = a;
        writeDigit(0x33B4u, (uint8_t)(a & 0x0Fu), 0u);
    }

    // Digit 2: BCD byte mem[$0641], upper nibble → $3413, lower nibble → $3445
    a = mem[zp::placedItemCountBcd];
    if (a != mem[zp::shieldOrDamage]) {
        mem[zp::shieldOrDamage] = a;
        uint8_t hi = (uint8_t)(a >> 4u);  // upper nibble (BCD tens)
        uint8_t lo = (uint8_t)(a & 0x0Fu);                   // lower nibble (BCD units)
        writeDigit(0x3413u, hi, 0u);
        writeDigit(0x3445u, lo, 0u);
    }

    // Digit 3: BCD byte mem[$0628] with optional $80 flag
    uint8_t bf = 0u;
    if (mem[0x062Bu] != 0u && (0x0Cu & mem[zp::collisionFlags]) == 0u)
        bf = 0x80u;
    mem[zp::barColThreshold] = bf;
    a = mem[zp::scoreBcd];
    if ((a | bf) != mem[zp::digitCache646]) {
        mem[zp::digitCache646] = a | bf;
        uint8_t hi = (uint8_t)(((a >> 2u) & 0x3Cu) >> 2u);
        uint8_t lo = (uint8_t)(a & 0x0Fu);
        writeDigit(0x3472u, hi, bf);
        writeDigit(0x34A4u, lo, bf);
    }
}

// saucer_anim_tick_native: direct translation of saucer_anim_tick @ $4229.
// (Previously mislabelled update_gauge_digits — that is a different routine at
// $548D; the canonical name for $4229 in disasm/symbols.csv is saucer_anim_tick.)
// Called from vbi_handler_game ($52D7) every other frame (LSR $0643 gate).
// Drives the cockpit score/counter animation at $3491-$3497 (mode-4 chars).
// State machine in mem[$007E]:
//   0       — init: fill $3492-$3497 with $A9 (coloured glyph), advance to 1
//   1-6     — each call (after timer $00E6 expires): blank $3491+state with $29,
//             advance state; state 6→7 on final step
//   7       — done: set mem[$0048]=1 / mem[$28EE]=1
//   $80     — random blink: toggle colour of one $3492-$3497 char each tick
//   $81+    — reverse fill: restore $A9 at $3491+state, decrement state
// mem[$0618] = per-step timer reload (0 = advance every call).
// game_sub_5815 ring-buffer push is inlined (ring at $0719, ptr at $0073).
extern "C" void saucer_anim_tick_native(void)
{
    auto pushRingBuf = [](uint8_t val) {
        uint8_t ptr = mem[zp::altRingHead];
        if (ptr >= 0x20u) ptr = 0x1Fu;
        mem[0x0719u + ptr] = val | 0x80u;
        mem[zp::altRingHead] = (ptr == 0u) ? 0x1Fu : (uint8_t)(ptr - 1u);
    };

    uint8_t s = mem[zp::scoreOrRescued];

    if ((int8_t)s < 0) {        // s >= $80
        if (s >= 0x81u) {
            // Reverse-fill path: restore $A9 glyphs one by one.
            // LSR $0631 / BCS skip: rate-limit alternate calls.
            if (mem[zp::saucerPhaseFlag] & 1u) { mem[zp::saucerPhaseFlag] >>= 1u; return; }
            mem[zp::saucerPhaseFlag] = (uint8_t)((mem[zp::saucerPhaseFlag] >> 1u) + 1u);
            uint8_t n = (uint8_t)(s & 0x0Fu);
            uint8_t newS = (n == 7u) ? (uint8_t)(s - 2u) : (uint8_t)(s - 1u);
            mem[zp::scoreOrRescued] = newS;
            mem[0x3491u + newS] = 0xA9u;
            pushRingBuf(0xA9u);
        } else {    // s == $80: random blink
            if (mem[zp::animStepTimer] > 0u) { mem[zp::animStepTimer]--; return; }
            // Atari: LDA $D20A (RANDOM); Amiga: use RTCLOK low byte as proxy.
            uint8_t r = mem[zp::rtclokLow] & 7u;
            mem[zp::animStepTimer] = (r >= 6u) ? (uint8_t)(r >> 1u) : r;   // mirror $36→3, $37→3
            uint8_t y = (r >= 6u) ? (uint8_t)(r >> 1u) : r;
            mem[0x3492u + y] ^= 0x80u;   // toggle colour bit
        }
        return;
    }

    if (s != 0u) {              // s = 1-7
        if (mem[zp::animStepTimer] > 0u) { mem[zp::animStepTimer]--; return; }
        mem[zp::animStepTimer] = mem[zp::gaugeStepReload];
        if (s == 7u) {
            if (mem[zp::saucerActiveFlag] == 0u) { mem[zp::saucerActiveFlag] = 1u; mem[0x28EEu] = 1u; }
            return;
        }
        mem[zp::scoreOrRescued]++;
        uint8_t newS = mem[zp::scoreOrRescued];
        mem[0x3491u + newS] = 0x29u;
        pushRingBuf(0x29u);     // pushes $A9 = $29|$80
    } else {                    // s == 0: initialise
        mem[zp::saucerActiveFlag] = 0u;
        mem[zp::scoreOrRescued] = 1u;
        mem[zp::animStepTimer] = mem[zp::gaugeStepReload];
        for (int i = 5; i >= 0; i--) mem[0x3492u + (uint16_t)i] = 0xA9u;
    }
}

// update_indicator_blink_native: direct translation of update_blink_timer_006e
// @ $4131, called via vbi_handler_2 ($4FF5) during Standby.
// Counts down mem[$006E]; on expiry reloads to $0F and sets mem[$00DE]=$4E (ON);
// when counter drops below $0A sets mem[$00DE]=$46 (OFF).
// Tail calls to vobj_* (in-game object animation) are skipped for Standby.
extern "C" void update_indicator_blink_native(void)
{
    if (mem[zp::blinkTimer] < 1) return;   // CMP #1 / BCC: skip if already 0
    mem[zp::blinkTimer]--;
    if (mem[zp::blinkTimer] == 0) {
        mem[zp::blinkTimer] = 0x0F;        // reload timer
        mem[zp::indicatorLightState] = 0x4E;        // lights ON
    } else if (mem[zp::blinkTimer] < 0x0A) {
        mem[zp::indicatorLightState] = 0x46;        // lights OFF (last 9 ticks of cycle)
    }
}

// --- Tunnel-ring cycle: faithful ports of the $5367 dispatcher's $0088 branch ---
//
// The original per-frame driver is sound_event_dispatch ($5367), a strict
// priority dispatcher that runs exactly ONE action per frame:
//     if   $008D != 0  -> step_accum_sub_7e   (ring reverse — NOT ported)
//     elif $0088 != 0  -> step_accum_add_75   (tunnel ring cycle — ported below)
//     elif $0089 != 0  -> scroll_terrain_columns
//     elif $008B != 0  -> dl_index_dec
//     else (via $008F toggle) $008A != 0 -> scroll_terrain_dl  (door scroll)
// Only the $0088 ring branch is ported here; the lower-priority branches drive
// the ANTIC display-list machinery and are deferred to the (not-yet-done) DL pass.

// add_multibyte_a1 @ $6AB5: A (carry clear) += $00A1, carry chaining up through
// $00A2,$00A3,$00A4; returns the top byte (the caller stores it to $00A4).
// NOTE: the $A3 step reuses the freshly-written $A2 as its operand (no LDA #0
// between them) — a quirk of the original that is reproduced verbatim.
static uint8_t add_multibyte_a1(uint8_t a)
{
    uint16_t r = (uint16_t)a + mem[zp::scrollAccum0];                 // CLC; ADC $A1
    mem[zp::scrollAccum0] = (uint8_t)r;
    r = (uint16_t)mem[zp::scrollAccum1] + (r >> 8);                   // LDA #0; ADC $A2
    mem[zp::scrollAccum1] = (uint8_t)r;
    r = (uint16_t)mem[zp::scrollAccum1] + mem[zp::scrollAccum2] + (r >> 8);     // ADC $A3 (A = new $A2!)
    mem[zp::scrollAccum2] = (uint8_t)r;
    r = (uint16_t)mem[zp::scrollAccum3] + (r >> 8);                   // LDA #0; ADC $A4
    return (uint8_t)r;                                      // top byte
}

// advance_history_6a4d @ $6A4D: rotate the 6-byte colour ring $08D4-$08D9 up one
// slot (old $08D9 wraps back into $08D4 — feeds COLOR01-06); if $008D is negative
// copy $08D8 -> $0071; bump $0679[$0C] by $06CC, saturating to $FF on wrap-to-0.
// The tail-call to reorder_sprite_slot ($5629, PMG slot reordering) is irrelevant
// to the Standby tunnel and is omitted (same convention as the handlers above).
static void advance_history_6a4d(void)
{
    uint8_t top = mem[zp::colorRing + 5];
    mem[zp::colorRing + 5] = mem[zp::colorRing + 4];
    mem[zp::colorRing + 4] = mem[zp::colorRing + 3];
    mem[zp::colorRing + 3] = mem[zp::colorRing + 2];
    mem[zp::colorRing + 2] = mem[zp::colorRing + 1];
    mem[zp::colorRing + 1] = mem[zp::colorRing];
    mem[zp::colorRing] = top;
    if ((int8_t)mem[zp::stepModeFlag] < 0) mem[zp::displayFlags] = mem[zp::colorRing + 4];     // $008D < 0
    uint8_t s = (uint8_t)(mem[0x0679 + 0x0C] + mem[zp::historyRingStep]);    // CLC; ADC $06CC
    mem[0x0679 + 0x0C] = s ? s : 0xFF;                          // BNE keep; else LDA #$FF
}

// step_accum_add_75 @ $6A38: add $75 into the accumulator; if the resulting top
// byte ($A4) is unchanged, do nothing; otherwise store it and rotate the ring.
// The advance_message_column call (top byte >= $90) drives scrolling message
// text not present in the attract tunnel — unreached at this step rate, omitted.
static void step_accum_add_75(void)
{
    uint8_t a = add_multibyte_a1(0x75);
    mem[zp::scrollAccum3] = a;
    if (a == mem[zp::scrollAccumPrev]) return;     // CMP $A5; BEQ -> top byte unchanged
    mem[zp::scrollAccumPrev] = a;
    // CMP #$90; BCS advance_message_column  (unreached in attract; omitted)
    advance_history_6a4d();
}

// tunnel_ring_tick_native: the $0088 branch of sound_event_dispatch ($5367) —
// the per-frame tunnel-ring driver.  Forward cinematic only ($008D stays 0).
extern "C" void tunnel_ring_tick_native(void)
{
    if (mem[zp::stepModeFlag]) return;          // $008D!=0 -> step_accum_sub_7e (reverse, not ported)
    if (mem[zp::vbiFlags]) step_accum_add_75();
}
