// Native 68000 translations of Atari VBI handler fragments used by RescueOnFractalus.
//
// The full vbi_handler_standby ($52D7) and vbi_handler_flight ($4FF5) do many things:
// hardware register writes (DMACTL, CHBASE, COLPF0-3, HPOSP, PRIOR), display-list
// management, sound dispatch, and object animation.  On the Amiga:
//   • Copper handles all hardware register writes.
//   • main.cpp VBI interrupt server handles RTCLOK ($0014/$0013/$0080).
//   • SfxPlayer handles the audio (sfx_voice_tick_native).
// Only the pure mem[]-state fragments below remain.

#include "PaulaAudio.h"   // for mem[] + uint types
#include "AtariZp.h"      // zp:: named Atari memory offsets
extern "C" volatile uint8_t mem[65536];

// vbi_attract_timer_native: fragment of vbi_handler_standby @ $52D7 relevant to
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
            // ring_push_marked(X=$14): push (a|$80) into altitude ring buffer at $0719
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

// lock_on_indicator_tick_native: direct translation of lock_on_indicator_tick @ $4229.
// (Previously mislabelled update_gauge_digits — that is a different routine at
// $548D; the canonical name for $4229 in disasm/symbols.csv is lock_on_indicator_tick.)
// Called from vbi_handler_standby ($52D7) every other frame (LSR $0643 gate).
// Drives the cockpit score/counter animation at $3491-$3497 (mode-4 chars).
// State machine in mem[$007E]:
//   0       — init: fill $3492-$3497 with $A9 (coloured glyph), advance to 1
//   1-6     — each call (after timer $00E6 expires): blank $3491+state with $29,
//             advance state; state 6→7 on final step
//   7       — done: set mem[$0048]=1 / mem[$28EE]=1
//   $80     — random blink: toggle colour of one $3492-$3497 char each tick
//   $81+    — reverse fill: restore $A9 at $3491+state, decrement state
// mem[$0618] = per-step timer reload (0 = advance every call).
// ring_push_marked ring-buffer push is inlined (ring at $0719, ptr at $0073).
extern "C" void lock_on_indicator_tick_native(void)
{
    auto pushRingBuf = [](uint8_t val) {
        uint8_t ptr = mem[zp::altRingHead];
        if (ptr >= 0x20u) ptr = 0x1Fu;
        mem[0x0719u + ptr] = val | 0x80u;
        mem[zp::altRingHead] = (ptr == 0u) ? 0x1Fu : (uint8_t)(ptr - 1u);
    };

    uint8_t s = mem[zp::lockOnIndicatorState];

    if ((int8_t)s < 0) {        // s >= $80
        if (s >= 0x81u) {
            // Reverse-fill path: restore $A9 glyphs one by one.
            // LSR $0631 / BCS skip: rate-limit alternate calls.
            if (mem[zp::lockOnIndicatorPhase] & 1u) { mem[zp::lockOnIndicatorPhase] >>= 1u; return; }
            mem[zp::lockOnIndicatorPhase] = (uint8_t)((mem[zp::lockOnIndicatorPhase] >> 1u) + 1u);
            uint8_t n = (uint8_t)(s & 0x0Fu);
            uint8_t newS = (n == 7u) ? (uint8_t)(s - 2u) : (uint8_t)(s - 1u);
            mem[zp::lockOnIndicatorState] = newS;
            mem[0x3491u + newS] = 0xA9u;
            pushRingBuf(0xA9u);
        } else {    // s == $80: random blink (faithful port of $4235-$4247)
            if (mem[zp::animStepTimer] > 0u) { mem[zp::animStepTimer]--; return; }
            // $4235 LDA $D20A / AND #7 — MUST be the POKEY RANDOM LFSR, not RTCLOK.
            // (RTCLOK is monotonic; once this runs in the ISR locked to the $0014++
            // tick, RTCLOK&7 at blink time aliases to one value -> only one light
            // blinks.  paula_pokey_random() is the real LFSR, as the Atari read.)
            uint8_t r = paula_pokey_random() & 7u;
            mem[zp::animStepTimer] = r;                       // $423A STA $E6 (full r)
            uint8_t y = (r >= 6u) ? (uint8_t)(r >> 1u) : r;   // $423C CMP #6 / BCC / LSR A
            mem[0x3492u + y] ^= 0x80u;                        // $4242-$4247 toggle colour bit
        }
        return;
    }

    if (s != 0u) {              // s = 1-7
        if (mem[zp::animStepTimer] > 0u) { mem[zp::animStepTimer]--; return; }
        mem[zp::animStepTimer] = mem[zp::gaugeStepReload];
        if (s == 7u) {
            if (mem[zp::lockOnIndicatorActive] == 0u) { mem[zp::lockOnIndicatorActive] = 1u; mem[0x28EEu] = 1u; }
            return;
        }
        mem[zp::lockOnIndicatorState]++;
        uint8_t newS = mem[zp::lockOnIndicatorState];
        mem[0x3491u + newS] = 0x29u;
        pushRingBuf(0x29u);     // pushes $A9 = $29|$80
    } else {                    // s == 0: initialise
        mem[zp::lockOnIndicatorActive] = 0u;
        mem[zp::lockOnIndicatorState] = 1u;
        mem[zp::animStepTimer] = mem[zp::gaugeStepReload];
        for (int i = 5; i >= 0; i--) mem[0x3492u + (uint16_t)i] = 0xA9u;
    }
}

// update_indicator_blink_native: direct translation of update_blink_timer_006e
// @ $4131, called via vbi_handler_flight ($4FF5) during Standby.
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

// advance_message_column @ $670D: the tunnel ring's "top byte >= $90" branch.
// It is the FAITHFUL tunnel→stars trigger: each invocation steps the column
// index $00A0 down by one and sets $0088 = $00A0 + 1, so once $00A0 wraps from
// $00 to $FF the gate $0088 becomes 0 and the ring stops cycling — display_setup
// then advances to the stars/space phase.  init_row_coords_9c seeds $00A0 = $13
// (=19), so the tunnel runs for 20 threshold crossings before it ends.
//
//   $670D  LDY $00A0
//   $670F  CPY #$06 / BMI $671E   ; $00A0 < 6 (signed) -> clear $08D8, else DRAW
//   $6713  LDA $6E0F,Y / STA $0096 / JSR draw_symmetric_span_loop ($6642)
//   $671E  (else) LDA #$00 / STA $08D8
//   $6723  DEC $00A0 / CLC / LDA $00A0 / ADC #$01 / STA $0088
//
// So the GEOMETRIC clear (expanding black frames from the centre) runs for the FIRST
// 14 crossings ($00A0 19->6); the $08D8 palette touch is only the last 6 ($00A0 5->0).
// The draw writes black (pen $0094=0) into the GTIA field at $2000, which the Amiga
// re-decodes (g_tunnelFieldDirty) so the rings visibly clear from the middle out.
static void draw_symmetric_span_loop(void);   // fwd decl (defined below)

// Set by advance_message_column when it draws into the GTIA field at $2000, with the
// touched row range, so RescueOnFractalus re-decodes ONLY those rows of the tunnel bitmap
// (a black ring band) — not the whole 86-row field, which costs > 1 PAL frame on the
// 68000 and freezes the ring cycle.  Cleared by RescueOnFractalus after it re-decodes.
// Report the row extent [$009F..$009E] the just-drawn black frame spans; RescueOnFractalus
// re-decodes that extent of the field, but PER-BYTE shadow-gated, so only the thin
// frame outline (horizontal edges + vertical side pieces) is actually re-decoded —
// fast enough to stay under one PAL frame (no tearing, stays synced to the palette).
extern "C" volatile uint8_t g_tunnelFieldDirty = 0;
extern "C" volatile uint8_t g_tunRowLo = 0, g_tunRowHi = 0;   // $009F .. $009E after the draw

static void advance_message_column(void)
{
    uint8_t a0 = mem[0x00A0];
    if ((int8_t)a0 >= 6) {                          // CPY #$06; BMI -> $00A0 >= 6 = DRAW
        mem[0x0096] = mem[0x6E0F + a0];             // LDA $6E0F,Y; STA $0096
        draw_symmetric_span_loop();                 // JSR $6642 (steps $009E++/$009F--)
        g_tunRowLo = mem[0x009F];                   // full new extent (bottom .. top)
        g_tunRowHi = mem[0x009E];
        g_tunnelFieldDirty = 1;
    } else {
        mem[0x08D8] = 0u;                           // $671E: LDA #$00; STA $08D8
    }
    mem[0x00A0]--;                                  // DEC $00A0
    mem[zp::vbiFlags] = (uint8_t)(mem[0x00A0] + 1u);   // CLC; LDA $00A0; ADC #$01; STA $0088
}

// step_accum_add_75 @ $6A38: add $75 into the accumulator; if the resulting top byte
// ($A4) is unchanged, do nothing; otherwise store it, and — when the top byte >= $90
// — step the message column (advance_message_column, the tunnel-exit clear + the
// stars trigger), then ALWAYS rotate the ring (advance_history_6a4d).  Per the real
// $6A38: `CMP #$90 / BCC $6A4D / JSR $670D` falls THROUGH into the rotation at $6A4D
// — the two are additive, NOT exclusive, so the palette keeps cycling while the
// tunnel clears.  (The earlier if/else port froze the cycle during the clear.)
static void step_accum_add_75(void)
{
    uint8_t a = add_multibyte_a1(0x75);
    mem[zp::scrollAccum3] = a;
    if (a == mem[zp::scrollAccumPrev]) return;     // CMP $A5; BEQ -> top byte unchanged
    mem[zp::scrollAccumPrev] = a;
    if (a >= 0x90u) advance_message_column();      // CMP #$90; BCS -> JSR $670D
    advance_history_6a4d();                        // $6A4D: ring rotation, ALWAYS runs
}

// ---- door display-list scroll: native ports of the $6953 family -------------
// The "doors" open by scrolling the ANTIC display list at $3000: each scanline's
// 3-byte LMS entry (mode byte + 16-bit source address) is shifted one slot, then a
// fresh leading row pointer is pushed at each edge.  These run on the live DL in
// mem[$3000+]; the door-open progress is the $008A counter they decrement.
// (Raw $00xx / $30xx literals kept here to mirror the 6502 DL layout 1:1.)

// dl_lms_scroll_up @ $69A9: shift top-half LMS entries from $300C,X up to $3009,X
// (3-byte stride) until X reaches the top index $0097.
static void dl_lms_scroll_up(void)
{
    uint8_t x = 1, top = mem[0x0097];
    while (x != top) {
        mem[0x3009 + x] = mem[0x300C + x]; x++;
        mem[0x3009 + x] = mem[0x300C + x]; x += 2;
    }
}

// dl_lms_scroll_down @ $69C3: shift bottom-half LMS entries from $3087,Y down to
// $308A,Y until Y reaches the bottom index $0098.
static void dl_lms_scroll_down(void)
{
    uint8_t y = 0x80, bot = mem[0x0098];
    while (y != bot) {
        mem[0x308A + y] = mem[0x3087 + y]; y--;
        mem[0x308A + y] = mem[0x3087 + y]; y -= 2;
    }
}

// dl_lms_push_top @ $6973: write the 16-bit top push pointer $0080/$0081 into the
// top LMS entry at $300A,X (net X -= 3), then advance the pointer up one row (-$2E).
static uint8_t dl_lms_push_top(uint8_t x)
{
    mem[0x300A + x] = mem[0x0081]; x--;
    mem[0x300A + x] = mem[0x0080];
    uint16_t p = (uint16_t)(mem[0x0080] | (mem[0x0081] << 8));
    p = (uint16_t)(p - 0x2E);
    mem[0x0080] = (uint8_t)p; mem[0x0081] = (uint8_t)(p >> 8);
    return (uint8_t)(x - 2);
}

// dl_lms_push_bottom @ $698E: write the 16-bit bottom push pointer $0082/$0083 into
// the bottom LMS entry at $3089,Y (net Y += 3), then advance the pointer down (+$2E).
static uint8_t dl_lms_push_bottom(uint8_t y)
{
    mem[0x3089 + y] = mem[0x0082]; y++;
    mem[0x3089 + y] = mem[0x0083];
    uint16_t p = (uint16_t)(mem[0x0082] | (mem[0x0083] << 8));
    p = (uint16_t)(p + 0x2E);
    mem[0x0082] = (uint8_t)p; mem[0x0083] = (uint8_t)(p >> 8);
    return (uint8_t)(y + 2);
}

// scroll_terrain_dl @ $6953: one door-open step.  Decrement $008A; while it is
// still non-zero scroll both DL halves apart; on the step that reaches 0 set the
// reload $008C=8 instead.  Then push the leading row into each half.
static void scroll_terrain_dl(void)
{
    if (--mem[zp::terrainScrollCounter] != 0) {     // DEC $008A; BNE
        dl_lms_scroll_down();
        dl_lms_scroll_up();
    } else {
        mem[zp::terrainScrollReload] = 8;            // $008C = 8
    }
    mem[0x0098] = dl_lms_push_bottom(mem[0x0098]);   // LDY $0098 / push / STY $0098
    mem[0x0097] = dl_lms_push_top(mem[0x0097]);      // LDX $0097 / push / STX $0097
}

// sound_event_dispatch @ $5367 (Standby subset): the per-frame priority dispatcher
// that runs exactly ONE action.  $0088 (ring) outranks $008A (door scroll), so the
// two are sequential — the doors scroll with a static tunnel while $0088==0, and
// the ring only animates once $0088 is armed (after the doors finish opening; the
// hand-off lives in RescueOnFractalus::update).  The $008C/clear_slot and the unused
// $0089/$008B branches are documented but inert in the Standby scene.
extern "C" void sound_event_dispatch_native(void)
{
    if (mem[zp::stepModeFlag]) return;                       // $008D: reverse ring (unused here)
    if (mem[zp::vbiFlags]) { step_accum_add_75(); return; }  // $0088: tunnel ring cycle
    if (mem[zp::scrollColumnsGate]) return;                  // $0089: scroll_terrain_columns (unused)
    if (mem[zp::dlIndexGate]) return;                        // $008B: dl_index_dec (unused)
    uint8_t ph = mem[zp::terrainScrollPhase];                // $008F every-other-frame toggle
    mem[zp::terrainScrollPhase] = (uint8_t)(ph >> 1);        // LSR $008F
    if (ph & 1u) return;                                     // carry set -> skip this frame
    mem[zp::terrainScrollPhase]++;                           // INC $008F
    if (mem[zp::terrainScrollCounter] == 0) return;          // $008A: doors already fully open
    scroll_terrain_dl();
}

// ---- procedural tunnel rings: native port of draw_frame_pattern_seq ($65FB) ---
// The concentric tunnel rings are DRAWN (not pre-baked): nested rectangle frames
// are filled into the GTIA-mode-10 screen at $2000 (per-row base address table
// $073D lo / $0793 hi, 46-byte stride), with the pen cycling 1..6.  Run once at
// scene init; the $2000 image is then decoded to the tunnel bitmap, the same path
// the terrain uses.  All scratch is in zero page ($0080-84/$0092/$0094/$0096/
// $009C-A0/$00B7-B9/$00DF) — re-seeded by openDoors() before the door scroll runs.

// plot_glyph_pixel_masked @ $66DE: OR then AND a pen into the screen byte at
// ($0080)+yByte using the mask tables $66E9 (set) / $66FB (clear), indexed by x.
static void plot_glyph_pixel_masked(uint8_t yByte, uint8_t x)
{
    uint16_t a = (uint16_t)((mem[0x0080] | (mem[0x0081] << 8)) + yByte);
    mem[a] = (uint8_t)((mem[a] | mem[0x66E9 + x]) & mem[0x66FB + x]);
}

// plot_pixel_masked @ $66D5: plot pen $0094 into pixel column `col` of the row
// pointer $0080/$0081 — high nibble for even columns, low nibble for odd.  (The
// 6502 LSR leaves carry = col bit0, so the odd-column "ADC #8" actually adds 9.)
static uint8_t plot_pixel_masked(uint8_t col)
{
    uint8_t x = (col & 1u) ? (uint8_t)(mem[0x0094] + 9u) : mem[0x0094];
    plot_glyph_pixel_masked((uint8_t)(col >> 1), x);
    return x;
}

// fill_horizontal_span @ $665D: solid-fill the pattern byte $00B9 across the byte
// range [$009C>>1 .. $009D>>1] in both the top ($009E) and bottom ($009F) rows.
static void fill_horizontal_span(void)
{
    uint8_t et = mem[0x009E], eb = mem[0x009F];
    uint16_t topp = (uint16_t)(mem[0x073D + et] | (mem[0x0793 + et] << 8));
    uint16_t botp = (uint16_t)(mem[0x073D + eb] | (mem[0x0793 + eb] << 8));
    uint8_t b0 = (uint8_t)(mem[0x009C] >> 1);            // $0082
    uint8_t b1 = (uint8_t)(mem[0x009D] >> 1);            // $0083
    if (mem[0x009D] & 1u) b0++; else b1--;               // trim one edge byte by parity
    uint8_t fill = mem[0x00B9];
    int cnt = (int)b1 - (int)b0;                         // $00DF
    uint8_t y = b1;
    do { mem[topp + y] = fill; mem[botp + y] = fill; y--; } while (--cnt >= 0);
}

// fill_vertical_span @ $669C: plot the left ($009C) and right ($009D) edge pixels
// down every row from $009F to $009E (the right pixel reuses the left's mask).
static void fill_vertical_span(void)
{
    mem[0x0084] = mem[0x009F];
    int cnt = (uint8_t)(mem[0x009E] - mem[0x009F]);      // $00DF
    do {
        uint8_t y = mem[0x0084];
        mem[0x0080] = mem[0x073D + y];
        mem[0x0081] = mem[0x0793 + y];
        uint8_t x = plot_pixel_masked(mem[0x009C]);                 // left edge
        plot_glyph_pixel_masked((uint8_t)(mem[0x009D] >> 1), x);    // right edge, same mask
        mem[0x0084]++;
    } while (--cnt >= 0);
}

// draw_symmetric_span_loop @ $6642: draw $0096 nested rectangle frames, each one
// step larger ($009C--/$009D++/$009E++/$009F--), in the current pen $0094.
static void draw_symmetric_span_loop(void)
{
    mem[0x00B9] = (uint8_t)(mem[0x0094] | mem[0x66E9 + mem[0x0094]]);  // both pixels = pen
    do {
        fill_horizontal_span();
        fill_vertical_span();
        mem[0x009C]--; mem[0x009D]++; mem[0x009E]++; mem[0x009F]--;
    } while (--mem[0x0096] != 0);
}

// draw_shape_rows_loop @ $6620: three full-height vertical lines at columns $009C,
// $009D and $009D+1 (the tunnel's vanishing-point verticals), in pen $0094.
static void draw_shape_rows_loop(void)
{
    mem[0x0092] = 0x55;
    mem[0x00A0] = (uint8_t)(mem[0x009D] + 1);
    do {
        uint8_t y = mem[0x0092];
        mem[0x0080] = mem[0x073D + y];
        mem[0x0081] = mem[0x0793 + y];
        plot_pixel_masked(mem[0x009C]);
        plot_pixel_masked(mem[0x009D]);
        plot_pixel_masked(mem[0x00A0]);
    } while (((int8_t)--mem[0x0092]) >= 0);
}

// init_row_coords_9c @ $6DDF: seed the symmetric-span coordinates (centre of the
// terrain region: rows 42/43, columns 46/48) and the 20-frame outer count.
static void init_row_coords_9c(void)
{
    mem[0x009C] = 0x2E; mem[0x009D] = 0x30;
    mem[0x009E] = 0x2B; mem[0x009F] = 0x2A;
    mem[0x00A0] = 0x13;
}

// tunnel_ring_arm_native: reseed the message-column / span coordinates the way the
// Atari tunnel setup does at $647D-$6480 (JSR init_row_coords_9c; $0094=0) just
// before it arms $0088=1 at $64A8.  The Amiga ran init_row_coords_9c earlier (in
// decodeTunnelRings, which left $00A0=$FF), so we re-seed here when arming the ring
// — otherwise advance_message_column would start from $FF and never clear $0088 in
// the expected 20 crossings.
extern "C" void tunnel_ring_arm_native(void)
{
    mem[0x009C] = 0x2E; mem[0x009D] = 0x30;
    mem[0x009E] = 0x2B; mem[0x009F] = 0x2A;
    mem[0x00A0] = 0x13;     // 19 → 20 message-column crossings before $0088 clears
    mem[0x0094] = 0x00;     // pen index reset ($6480)
}

// draw_frame_pattern_seq @ $65FB: draw 20 concentric frame groups (thickness from
// the $6E0F table, pen cycling 1..6), then the vanishing-point verticals.
// draw_tunnel_rings_native clears the $2000 GTIA field first so the rings land on
// a black background (pen 0); RescueOnFractalus decodes the result into the tunnel bitmap.
extern "C" void draw_tunnel_rings_native(void)
{
    for (uint16_t i = 0; i < 86u * 46u; i++) mem[0x2000 + i] = 0u;   // clear screen field
    init_row_coords_9c();
    mem[0x0094] = 1u;
    do {
        mem[0x0096] = mem[0x6E0F + mem[0x00A0]];
        draw_symmetric_span_loop();
        if (++mem[0x0094] == 7u) mem[0x0094] = 1u;
    } while (((int8_t)--mem[0x00A0]) >= 0);
    mem[0x0094]--;
    draw_shape_rows_loop();
}

// standby_vbi_native: the faithful per-frame body of vbi_handler_standby ($52D7),
// run from the real INTB_VERTB interrupt via game_vbi_isr() during the Standby
// screen + the launch cinematic (the Atari ran $52D7 in the VBI throughout).
// HW-display writes ($52D7-$5332: PMG positions, colours, GRAFP, HPOS) are SKIPPED —
// the Amiga copper owns the display.  Order mirrors $5333 onward:
//   $5333  INC $0014 (RTCLOK)            -> done by the ISR before this call
//   $5335  attract-timer cascade         -> vbi_attract_timer_native
//   $5398  attract input poll            -> SKIPPED: Atari $D01F/$D010/$D300 reads
//          (resets the attract timeout on input) return neutral on the Amiga, where
//          input is the keyboard ISR, so it would only ever no-op.
//   $5367  sound_event_dispatch          -> the door/tunnel/scroll cinematic driver
//          (self-gated on $0088/$0089/$008A/$008B/$008D — inert on the static screen)
//   $5342  lock_on_indicator_tick every other frame (LSR/INC $0643 gate)
//   $534D  SFX tick ($70F9)              -> runs on CIA-B Timer A instead (main.cpp)
//          update_gauge_digits ($548D) / music_player_tick ($7253) -> never ported.
extern "C" void update_gauge_digits(void);   // $548D: SFX voice engine + $0719 ring drain

extern "C" void standby_vbi_native(void)
{
    vbi_attract_timer_native();              // $5335
    sound_event_dispatch_native();           // $5367
    // $548D SFX voice engine — the Atari ran it in this VBI tail too.  Gate on
    // $060B (=$23 once the launch cinematic begins, 0 during pure attract) so the
    // attract music (CIA-B sequencer) is undisturbed but the START/doors/tunnel
    // launch effects get drained from the $0719 ring to POKEY -> Paula.
    if (mem[0x060B]) update_gauge_digits();
    uint8_t g = mem[zp::lockOnIndicatorTickParity];   // $5342: LSR $0643 / BCS skip / ... / INC
    mem[zp::lockOnIndicatorTickParity] = (uint8_t)(g >> 1);
    if (!(g & 1u)) {                         // carry clear -> run, then INC
        lock_on_indicator_tick_native();           // $4229
        mem[zp::lockOnIndicatorTickParity]++;
    }
}
