// SAS/C -> m68k-amiga-elf-gcc compatibility shims. Force-included into every
// .cpp by the GCC Makefile (-include SASCCompat.h); the SAS/C smakefile does NOT
// include it. The whole body is also guarded by #ifndef __SASC so it stays inert
// even if pulled in under SAS/C.
//
// The GCC build compiles with ASSEMBLER undefined (Util.h only #defines it for
// __SASC), so the #ifndef ASSEMBLER C++ implementations are used instead of the
// SAS/C register-convention *Assembler.s. These macros neutralise the remaining
// SAS/C storage/keyword extensions for GCC.
#pragma once
#ifndef __SASC

// Fixed-width types: the framework only typedefs these under
// #if __cplusplus < 201103L (SAS/C was pre-C++11). We build as gnu++17, so
// define them here (identical repeats are legal). m68k LP32: short=16, long=32.
typedef char           int8_t;
typedef short          int16_t;
typedef long           int32_t;
typedef unsigned char  uint8_t;
typedef unsigned short uint16_t;
typedef unsigned long  uint32_t;

// NOTE: __asm is deliberately NOT redefined. The NDK library calls are
// statement-expression macros that expand `__asm("d0")` register bindings at
// the *call site*, so neutralising __asm would break every OpenLibrary/AllocMem/
// LoadView/... The SAS/C `__asm` *function qualifiers* are instead confined to
// the headers' #ifdef ASSEMBLER branches, which GCC never compiles.
//
// SAS/C storage qualifiers / keywords:
//   __chip  -> chip RAM section (the linker maps .MEMF_CHIP to a chip hunk)
//   __far   -> 32-bit absolute (gcc already absolute) -> no-op
//   __saveds-> reload small-data base (no baserel here) -> no-op
//   __inline-> nothing (the framework declares accessors __inline in headers but
//              DEFINES them out-of-line in .cpp; gcc's `inline` would not emit
//              them, breaking cross-TU calls)
#define __chip   __attribute__((section(".MEMF_CHIP")))
#define __far
#define __saveds
#define __inline
#define __stdargs        // SAS/C: args on the stack (gcc default) -> no-op
#define __regargs        // SAS/C: args in registers -> no-op (signature only)
#define __aligned

// SAS/C register-parameter keywords. Only ever seen by GCC inside #ifndef
// ASSEMBLER prototypes that don't use them; defined empty for safety.
#define __d0
#define __d1
#define __d2
#define __d3
#define __d4
#define __d5
#define __d6
#define __d7
#define __a0
#define __a1
#define __a2
#define __a3
#define __a4
#define __a5
#define __a6

#endif // !__SASC
