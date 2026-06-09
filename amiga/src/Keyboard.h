#pragma once

// Low-level keyboard reader for the takeover model (no Intuition / input.device).
//
// The Amiga keyboard shifts each keycode into CIA-A's serial data register,
// raising the CIA-A SP interrupt (CIAICRB_SP, routed via INTB_PORTS).  We hang a
// handler on that vector through ciaa.resource — the same AddICRVector mechanism
// main.cpp already uses for the CIA-B Timer A music tick.
//
// keyboard.device normally owns the CIA-A SP vector, so initialize() *steals* it
// (saving the previous vector) and shutdown() restores it, leaving the OS
// keyboard working after we hand control back.
//
// Usage mirrors DanceDiverse3's Input class: poll a latching "was it pressed"
// query once per frame.  RETURN is the START button for the launch cinematic.
class Keyboard {
public:
    bool initialize();        // steal CIA-A SP vector; false if ciaa.resource missing
    void shutdown();          // restore the previous (keyboard.device) vector

    // True once per RETURN key-down since the last call (latched, then cleared).
    bool returnPressed();
};
