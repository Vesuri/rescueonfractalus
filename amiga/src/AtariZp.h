#pragma once
#include "../framework/Util.h"  // uint16_t

// Named Atari zero-page / RAM offsets into the shared mem[65536] snapshot.
//
// These mirror the canonical reverse-engineered names in disasm/symbols.csv
// (camelCased here).  The Atari 6502 code addresses these by raw hex; the
// hand-written native ports in this directory use these names instead so the
// control logic reads as variables (mem[zp::vbiFlags]) rather than mem[0x0088].
//
// Scope: the zero-page control/state variables touched by the native handlers,
// plus the colour-ring base.  Screen-buffer, glyph-table and colour-shadow
// addresses ($32xx/$33xx/$34xx/$02Cx/$4AE3/$5A9F …) remain raw hex — they are
// layout constants, not state variables.
namespace zp {

// --- timers / frame cadence ---
constexpr uint16_t rtclokLow        = 0x0014;  // RTCLOK low byte (frame counter / RNG proxy)
constexpr uint16_t blinkTimer       = 0x006E;  // down-counter driving the indicator-light blink
constexpr uint16_t indicatorLightState = 0x00DE;  // cockpit indicator light: $4E on / $46 off
constexpr uint16_t attractTimer     = 0x00E2;  // attract-mode timer
constexpr uint16_t attractTimerSub  = 0x062D;  // attract-timer sub-counter (256-wrap -> attractTimer)
constexpr uint16_t animStepTimer    = 0x00E6;  // per-step delay for the saucer/gauge animation
constexpr uint16_t saucerTickParity = 0x0643;  // every-other-frame gate for saucer_anim_tick

// --- dispatcher gates (read by sound_event_dispatch $5367) ---
constexpr uint16_t vbiFlags         = 0x0088;  // gate: routes the dispatcher to the tunnel-ring step
constexpr uint16_t stepModeFlag     = 0x008D;  // <0 selects the reverse ring step (step_accum_sub_7e)

// --- terrain / display-list scroll (door split; DL port still deferred) ---
constexpr uint16_t terrainScrollReload = 0x008C;  // reload value (8) when the scroll counter expires

// --- tunnel ring colour cycle ---
constexpr uint16_t colorRing      = 0x08D4;  // base of the 6-byte colour ring $08D4-$08D9 (feeds COLOR01-06)
constexpr uint16_t scrollAccum0   = 0x00A1;  // multi-byte scroll accumulator, byte 0 (LSB)
constexpr uint16_t scrollAccum1   = 0x00A2;  // byte 1
constexpr uint16_t scrollAccum2   = 0x00A3;  // byte 2
constexpr uint16_t scrollAccum3   = 0x00A4;  // byte 3 (top); step_accum_add_75 result
constexpr uint16_t scrollAccumPrev = 0x00A5; // previous top byte, compared to detect a ring step
constexpr uint16_t historyRingStep = 0x06CC; // increment added to $0679[$0C] in advance_history_6a4d

// --- title / SFX attract state ---
constexpr uint16_t sfxReinitGate  = 0x0090;  // non-zero -> attract loop re-inits the SFX sequence ($70E7)
constexpr uint16_t altitudeThreshold = 0x0091;  // selects which 20-byte block copy_text_block_to_screen blits
constexpr uint16_t textColorPf0   = 0x00D8;  // title text colour shadow (COLPF0)
constexpr uint16_t displayFlags   = 0x0071;  // display-mode flags ($C0 init / $00 attract / $2A game)

// --- cockpit digit / gauge / saucer state ---
constexpr uint16_t playerLives    = 0x0072;  // player life count (gates the altitude ring push)
constexpr uint16_t altRingHead    = 0x0073;  // head index of the altitude ring buffer at $0719
constexpr uint16_t scoreOrRescued = 0x007E;  // saucer/gauge state machine selector
constexpr uint16_t saucerActiveFlag = 0x0048; // set when the saucer animation completes
constexpr uint16_t joystickSaved  = 0x004A;  // saved joystick / active-game flag
constexpr uint16_t collisionFlags = 0x004B;  // collision / mode flags (BIT-tested in cockpit digits)
constexpr uint16_t barColThreshold = 0x00BF; // cockpit bar-column threshold scratch
constexpr uint16_t gaugeStepReload = 0x0618; // per-step timer reload for the gauge counter
constexpr uint16_t scoreBcd        = 0x0628; // packed-BCD score counter
constexpr uint16_t saucerPhaseFlag = 0x0631; // saucer reverse-fill phase rate gate
constexpr uint16_t placedItemCountBcd = 0x0641; // BCD count of placed items
constexpr uint16_t gamePhaseFlag   = 0x0642; // game phase (0=intro 1/2=active 3=transition)
constexpr uint16_t shieldOrDamage  = 0x0645; // BCD digit-cache (digit 2 change detect)
constexpr uint16_t digitCache646   = 0x0646; // digit-cache slot (digit 3 change detect)
constexpr uint16_t digitCache647   = 0x0647; // digit-cache slot (digit 1 change detect)

} // namespace zp
