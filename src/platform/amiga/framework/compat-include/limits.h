#pragma once
// Minimal limits.h for the -nostdlib m68k build (LP32: int/long = 32-bit).
#define CHAR_BIT   8
#define SCHAR_MAX  127
#define SCHAR_MIN  (-128)
#define UCHAR_MAX  255
#define SHRT_MAX   32767
#define SHRT_MIN   (-32768)
#define USHRT_MAX  65535
#define INT_MAX    2147483647
#define INT_MIN    (-INT_MAX - 1)
#define UINT_MAX   4294967295U
#define LONG_MAX   2147483647L
#define LONG_MIN   (-LONG_MAX - 1L)
#define ULONG_MAX  4294967295UL
