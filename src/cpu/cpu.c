#include "cpu.h"

Cpu6502 cpu;
/* volatile: shared between the main thread (game loop) and the audio
   callback thread (VBI handler).  Without volatile, -O2 proves that
   spin-wait conditions like "while (mem[0x8E] == 0)" never change in
   the loop body and folds them to infinite loops.                    */
volatile uint8_t mem[65536];
