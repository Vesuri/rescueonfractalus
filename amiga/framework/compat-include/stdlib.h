#pragma once
// Minimal stdlib.h for the -nostdlib m68k build. Only what the demo uses.
#ifndef NULL
#define NULL 0
#endif
typedef unsigned long size_t;
#ifdef __cplusplus
extern "C" {
#endif
void  qsort(void* base, size_t nmemb, size_t size, int (*compar)(const void*, const void*));
int   abs(int x);
long  labs(long x);
#ifdef __cplusplus
}
#endif
