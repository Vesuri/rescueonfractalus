// CIA-A serial-port (SP) keyboard handler — see Keyboard.h.

#define ECS_SPECIFIC
#include <proto/exec.h>
#include <proto/cia.h>
#include <exec/interrupts.h>
#include <exec/nodes.h>
#include <resources/cia.h>
#include <hardware/cia.h>
#include "../framework/AmigaHardware.h"   // ciaasdrPointer / ciaacraPointer + int types
#include "Keyboard.h"

// The Atari read CONSOL ($D01F / 53279) to see whether START was pressed; we
// reproduce that register here so the unmodified attract-loop poll (the native
// port of station_init's CONSOL read, station_poll_start_native) works exactly
// as on the 6502.  This handler IS the hardware abstraction: it maps the Amiga
// RETURN key onto the START console switch.
//
// CONSOL switches read active-low in bits 0-2 (START / SELECT / OPTION).  Idle
// (no switch down) reads $07; START down clears bit0 → $06, the value the loop
// tests for (CMP #$06 / BEQ).  We never touch SELECT/OPTION, so they stay high.
extern "C" volatile uint8_t mem[65536];
static const uint16_t kConsol      = 0xD01F;
static const uint8_t  kConsolIdle  = 0x07;
static const uint8_t  kConsolStart = 0x06;

// RETURN rawkey code (cf. DanceDiverse3 Input.cpp: RETURN=$44, ESC=$45).
static const uint8_t kRawReturn = 0x44;
// 'F' rawkey ($23) — dev shortcut: skip the launch cinematic and jump straight
// to the in-game flight stage (mirrors the SDL build booting from a flight
// snapshot).  Polled by main.cpp via g_skipToFlight.
static const uint8_t kRawSkipFlight = 0x23;
extern "C" volatile uint8_t g_skipToFlight = 0;

static struct Library*   s_ciaaBase    = nullptr;
static struct Interrupt  s_kbInterrupt;
static struct Interrupt* s_savedVector = nullptr;   // keyboard.device's vector, restored on exit

// CIA-A SP interrupt: a full keycode has shifted into the serial register.
// ciaa.resource has already read+cleared the ICR before dispatching us, so we
// only touch the serial data register (read the code) and CRA (handshake).
static uint32_t keyboardHandler()
{
    uint8_t sdr = *ciaasdrPointer;

    // Acknowledge: pulse SP to output mode (drives KDAT low) then back to input,
    // so the keyboard releases the next keycode.  HRM Appendix G (node G-2):
    // "Software MUST pulse the line low for 85 microseconds"; the resync timeout
    // is 143 ms, so anywhere in between is safe — this busy-wait is ~2 ms.
    *ciaacraPointer |= CIACRAF_SPMODE;
    for (volatile uint16_t d = 0; d < 1500; d++) { /* >=85us handshake */ }
    *ciaacraPointer &= (uint8_t)~CIACRAF_SPMODE;

    // Wire protocol (HRM Appendix G, node G-2): the keycode is sent ROL'd one bit
    // and KDAT is active-low, so SDR holds ~(keycode ROL 1).  Recover by inverting
    // then rotating right one bit.  Bit 7 of the result = key-up flag (0 = down).
    uint8_t code = (uint8_t)~sdr;
    code = (uint8_t)((code >> 1) | (code << 7));   // ROR 1
    uint8_t raw  = (uint8_t)(code & 0x7Fu);
    bool    down = (code & 0x80u) == 0u;

    // Drive the CONSOL START switch (bit0) from RETURN's down/up edges, so the
    // register continuously reflects the key's level — just like the real GTIA
    // switch the attract loop polls.
    if (raw == kRawReturn)
        mem[kConsol] = down ? kConsolStart : kConsolIdle;
    else if (raw == kRawSkipFlight && down)
        g_skipToFlight = 1;          // edge-triggered; main.cpp consumes + clears it
    return 0;
}

bool Keyboard::initialize()
{
    s_ciaaBase = (struct Library*)OpenResource((UBYTE*)CIAANAME);
    if (!s_ciaaBase) return false;

    // Power-on CONSOL state: no switch down (START up).
    mem[kConsol] = kConsolIdle;

    s_kbInterrupt.is_Node.ln_Type = NT_INTERRUPT;
    s_kbInterrupt.is_Node.ln_Pri  = 0;
    s_kbInterrupt.is_Node.ln_Name = (char*)"RoF KB";
    s_kbInterrupt.is_Data = nullptr;
    s_kbInterrupt.is_Code = (void(*)())keyboardHandler;

    // AddICRVector returns NULL on success, or the already-installed vector
    // (keyboard.device's) on conflict.  On conflict, steal it: remove theirs,
    // install ours, and remember theirs to restore in shutdown().
    s_savedVector = AddICRVector(s_ciaaBase, CIAICRB_SP, &s_kbInterrupt);
    if (s_savedVector) {
        RemICRVector(s_ciaaBase, CIAICRB_SP, s_savedVector);
        AddICRVector(s_ciaaBase, CIAICRB_SP, &s_kbInterrupt);
    }
    return true;
}

void Keyboard::shutdown()
{
    if (!s_ciaaBase) return;
    RemICRVector(s_ciaaBase, CIAICRB_SP, &s_kbInterrupt);
    if (s_savedVector) {
        AddICRVector(s_ciaaBase, CIAICRB_SP, s_savedVector);
        s_savedVector = nullptr;
    }
    s_ciaaBase = nullptr;
}
