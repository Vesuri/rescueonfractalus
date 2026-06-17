#pragma once
// Minimal stdint.h for the -nostdlib m68k (Amiga) cross build.
// Only the fixed-width integer types used by the RoF CPU/audio core are defined.
// m68k is big-endian ILP32: char=8, short=16, int=32, long=32.
#ifndef _STDINT_H
#define _STDINT_H

typedef signed   char      int8_t;
typedef signed   short     int16_t;
typedef signed   long      int32_t;
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned long      uint32_t;

typedef int32_t            intptr_t;
typedef uint32_t           uintptr_t;

#define INT8_MIN   (-128)
#define INT8_MAX   127
#define INT16_MIN  (-32768)
#define INT16_MAX  32767
#define INT32_MIN  (-2147483648L)
#define INT32_MAX  2147483647L
#define UINT8_MAX  255u
#define UINT16_MAX 65535u
#define UINT32_MAX 4294967295uL

#endif /* _STDINT_H */
