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

// RETURN rawkey code (cf. DanceDiverse3 Input.cpp: RETURN=$44, ESC=$45).
static const uint8_t kRawReturn = 0x44;

static struct Library*   s_ciaaBase    = nullptr;
static struct Interrupt  s_kbInterrupt;
static struct Interrupt* s_savedVector = nullptr;   // keyboard.device's vector, restored on exit
static volatile uint8_t  s_returnLatch = 0;         // RETURN went down since last poll

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

    if (raw == kRawReturn && down) s_returnLatch = 1u;
    return 0;
}

bool Keyboard::initialize()
{
    s_ciaaBase = (struct Library*)OpenResource((UBYTE*)CIAANAME);
    if (!s_ciaaBase) return false;

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

bool Keyboard::returnPressed()
{
    if (s_returnLatch) { s_returnLatch = 0u; return true; }
    return false;
}
