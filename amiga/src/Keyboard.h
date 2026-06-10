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
// This handler IS the Atari console-switch hardware abstraction: it maps the
// RETURN key onto the START switch in CONSOL ($D01F / 53279), writing $06 while
// RETURN is held and $07 when idle.  The launch cinematic is triggered by
// polling that register with the native port of station_init's CONSOL read
// (station_poll_start_native), exactly as the original 6502 attract loop did.
class Keyboard {
public:
    bool initialize();        // steal CIA-A SP vector; false if ciaa.resource missing
    void shutdown();          // restore the previous (keyboard.device) vector
};
