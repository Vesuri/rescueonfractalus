#pragma once
#include "framework/Util.h"  // uint16_t

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
constexpr uint16_t animStepTimer    = 0x00E6;  // per-step delay for the lock-on-indicator/gauge animation
constexpr uint16_t lockOnIndicatorTickParity = 0x0643;  // every-other-frame gate for lock_on_indicator_tick

// --- dispatcher gates (read by sound_event_dispatch $5367) ---
constexpr uint16_t vbiFlags         = 0x0088;  // gate: routes the dispatcher to the tunnel-ring step
constexpr uint16_t stepModeFlag     = 0x008D;  // <0 selects the reverse ring step (step_accum_sub_7e)

// --- terrain / display-list scroll (door split) ---
constexpr uint16_t terrainScrollCounter = 0x008A;  // door/terrain scroll step counter (launch: $2B-> 0)
constexpr uint16_t terrainScrollReload  = 0x008C;  // reload value (8) when the scroll counter expires
constexpr uint16_t terrainScrollPhase   = 0x008F;  // every-other-frame toggle gating scroll_terrain_dl
constexpr uint16_t scrollColumnsGate    = 0x0089;  // dispatcher gate: scroll_terrain_columns (unused in Standby)
constexpr uint16_t dlIndexGate          = 0x008B;  // dispatcher gate: dl_index_dec (unused in Standby)

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

// --- cockpit digit / gauge / lock-on indicator state ---
constexpr uint16_t playerLives    = 0x0072;  // player life count (gates the altitude ring push)
constexpr uint16_t altRingHead    = 0x0073;  // head index of the altitude ring buffer at $0719
constexpr uint16_t lockOnIndicatorState = 0x007E;  // lock-on-indicator/gauge state machine selector
constexpr uint16_t lockOnIndicatorActive = 0x0048; // set when the lock-on indicator animation completes
constexpr uint16_t joystickSaved  = 0x004A;  // saved joystick / active-game flag
constexpr uint16_t collisionFlags = 0x004B;  // collision / mode flags (BIT-tested in cockpit digits)
constexpr uint16_t barColThreshold = 0x00BF; // cockpit bar-column threshold scratch
constexpr uint16_t gaugeStepReload = 0x0618; // per-step timer reload for the gauge counter
constexpr uint16_t scoreBcd        = 0x0628; // packed-BCD score counter
constexpr uint16_t lockOnIndicatorPhase = 0x0631; // lock-on indicator reverse-fill phase rate gate
constexpr uint16_t placedItemCountBcd = 0x0641; // BCD count of placed items
constexpr uint16_t gamePhaseFlag   = 0x0642; // game phase (0=intro 1/2=active 3=transition)
constexpr uint16_t shieldOrDamage  = 0x0645; // BCD digit-cache (digit 2 change detect)
constexpr uint16_t digitCache646   = 0x0646; // digit-cache slot (digit 3 change detect)
constexpr uint16_t digitCache647   = 0x0647; // digit-cache slot (digit 1 change detect)

// --- flight: world state + per-frame loop (flight_native.cpp / flight VBI $4FF5) ---
constexpr uint16_t headingLo      = 0x2885;  // ship heading low byte (16-bit); drives compute_heading_sincos
constexpr uint16_t headingHi      = 0x2886;  // ship heading high byte
constexpr uint16_t worldXLo       = 0x2887;  // ground-plane world X low; {worldXHi:worldXLo}>>4 -> view vector
constexpr uint16_t worldXHi       = 0x2888;  // ground-plane world X high
constexpr uint16_t worldZLo       = 0x2889;  // ground-plane world Z low
constexpr uint16_t worldZHi       = 0x288A;  // ground-plane world Z high
constexpr uint16_t pilotState     = 0x288F;  // pilot rescue state; flight loop stores game_state ($0041) here
constexpr uint16_t gameState      = 0x0041;  // global game-state flag
constexpr uint16_t gamePhase      = 0x0042;  // game phase (0=init 1=active 2=post-level); flight pass selector
constexpr uint16_t timerOrCounter = 0x0044;  // game-pacing countdown ($54/$0E)
constexpr uint16_t levelOrState   = 0x0004;  // level number / fresh-start flag ($00=fresh start)
constexpr uint16_t freshStartFlag = 0x0627;  // 0=first time through (run intro), nonzero=continue/skip
constexpr uint16_t screenState    = 0x060C;  // cleared at flight init ($3E3A)
constexpr uint16_t frameCounter   = 0x00B7;  // frame counter (ticked by VBI)
constexpr uint16_t gpriorShadow   = 0x026F;  // GPRIOR/PRIOR shadow (=$11 in flight)
constexpr uint16_t rowTableStride = 0x00C1;  // per-scanline base-addr table stride (=$60=96 in flight)
constexpr uint16_t rowTableBaseLo = 0x00C3;  // row base-addr table base low (=$10 -> base $1010)
constexpr uint16_t rowTableBaseHi = 0x00C4;  // row base-addr table base high (=$10)
constexpr uint16_t scoreDisplay   = 0x0601;  // displayed score, 3-byte packed BCD $0601-$0603

} // namespace zp
