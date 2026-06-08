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
extern "C" volatile uint8_t mem[65536];

// vbi_handler_game_native: fragment of vbi_handler_game @ $52D7 relevant to
// Standby.  The full handler also writes DMACTL/CHBASE/colour/HPOS registers
// (handled by Copper) and increments $0014 (handled by main.cpp VBI server).
// This fragment is lines $5335-$533A:
//   INC_M($062D)           — attract timer sub-counter
//   if Z: INC_M($00E2)     — cascade to attract_timer on 256-wrap
extern "C" void vbi_handler_game_native(void)
{
    mem[0x062D]++;
    if (mem[0x062D] == 0) mem[0x00E2]++;
}

// copy_altitude_graphic_to_screen_native: direct translation of $782A.
// Fires when mem[$0091] >= $C0; the SFX sequencer (sfx_seq_step_native) writes
// negative sequence bytes — $C0/$C4 = Block1 ("rescue on fractalus"), $E2 =
// Block2 ("©1985...") — to mem[$0091] as a side effect of voice-param processing.
// Gate at $0091==$C0 exactly: only fires when mem[$00E2] is non-negative (< $80).
// Copies 20 chars from $5A9F (Block1) or $5AB3 (Block2) into $32B7-$32CA.
// Also writes mem[$00D8]=$44 (title COLBK) for the copyright path.
extern "C" void copy_altitude_graphic_to_screen_native(void)
{
    uint8_t alt = mem[0x0091];
    if (alt < 0xC0u) return;                                        // CMP $C0, BCC
    if (alt == 0xC0u && (int8_t)mem[0x00E2] < 0) return;           // attract-timer gate
    mem[0x0091] = 0;                                                // reset (Y=0 in original)
    uint16_t src;
    if (alt >= 0xE0u) {
        src = 0x5AB3u;          // Block2: copyright string
        mem[0x00D8] = 0x44u;    // title COLBK for copyright path
    } else {
        src = 0x5A9Fu;          // Block1: rescue string
    }
    for (int i = 0; i < 20; i++)
        mem[0x32B7u + (uint16_t)i] = mem[src + (uint16_t)i];
}

// update_blink_timer_006e_native: direct translation of update_blink_timer_006e
// @ $4131, called via vbi_handler_2 ($4FF5) during Standby.
// Counts down mem[$006E]; on expiry reloads to $0F and sets mem[$00DE]=$4E (ON);
// when counter drops below $0A sets mem[$00DE]=$46 (OFF).
// Tail calls to vobj_* (in-game object animation) are skipped for Standby.
extern "C" void update_blink_timer_006e_native(void)
{
    if (mem[0x006E] < 1) return;   // CMP #1 / BCC: skip if already 0
    mem[0x006E]--;
    if (mem[0x006E] == 0) {
        mem[0x006E] = 0x0F;        // reload timer
        mem[0x00DE] = 0x4E;        // lights ON
    } else if (mem[0x006E] < 0x0A) {
        mem[0x00DE] = 0x46;        // lights OFF (last 9 ticks of cycle)
    }
}
