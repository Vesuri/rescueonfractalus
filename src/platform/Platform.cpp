#include "Platform.h"
#include <cstdio>
#include <cstring>

extern uint8_t mem[65536];

Platform* platform = nullptr;

Platform::Platform() : quit(false), rngState(0x1FFFF) {}
Platform::~Platform() {}

uint8_t Platform::hwRead(uint16_t)        { return 0x00; }
void    Platform::hwWrite(uint16_t, uint8_t) {}
void    Platform::shadowWrite(uint16_t, uint8_t) {}

/* POKEY 17-bit Fibonacci LFSR.  Polynomial x^17 + x^5 + 1.
   Taps at bit 16 (MSB) and bit 4; shift left, feedback to bit 0.
   The real POKEY LFSR runs free at ~64 kHz; we step once per read. */
uint8_t Platform::pokeyRandomStep() {
    uint32_t bit = ((rngState >> 16) ^ (rngState >> 4)) & 1;
    rngState = ((rngState << 1) | bit) & 0x1FFFF;
    return (uint8_t)rngState;
}
