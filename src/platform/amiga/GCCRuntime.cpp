// Runtime shims for building RoF with m68k-amiga-elf-gcc (-nostdlib).
// Derived from the vendored framework's GCC runtime, modified:
//   - No demo-timeline dependency (VBI via exec AddIntServer instead).
//   - No level-3 autovector installation (exec handles it).

#include <proto/exec.h>
#include <exec/execbase.h>
#include <exec/memory.h>

struct ExecBase* SysBase = 0;
__attribute__((constructor)) static void initSysBase() { SysBase = *(struct ExecBase**)4UL; }

// Set by main() after OpenLibrary("graphics.library").
struct GfxBase* GfxBase = 0;

// Set by PlatformAmiga's ctor after OpenLibrary("dos.library"), and NULL if that failed.
// Used for exactly one thing: reading and writing the high-score file (PlatformAmiga's
// hiscoreLoad/hiscoreSave).  Everything else here is bare hardware.
struct DosLibrary* DOSBase = 0;

// ---- C++ heap via AllocMem --------------------------------------------------
void* operator new(unsigned long n)   { unsigned long* p = (unsigned long*)AllocMem(n + sizeof(unsigned long), MEMF_ANY | MEMF_CLEAR); if (!p) return 0; *p = n + sizeof(unsigned long); return p + 1; }
void* operator new[](unsigned long n) { return operator new(n); }
void  operator delete(void* p)             { if (!p) return; unsigned long* q = (unsigned long*)p - 1; FreeMem(q, *q); }
void  operator delete[](void* p)           { operator delete(p); }
void  operator delete(void* p, unsigned long)   { operator delete(p); }
void  operator delete[](void* p, unsigned long) { operator delete(p); }

extern "C" void __cxa_pure_virtual() { for (;;) ; }

// ---- minimal libc bits (-nostdlib) -----------------------------------------
extern "C" int  abs(int x)   { return x < 0 ? -x : x; }
extern "C" long labs(long x) { return x < 0 ? -x : x; }
extern "C" void qsort(void* base, unsigned long n, unsigned long size,
                      int (*cmp)(const void*, const void*)) {
    char* a = (char*)base;
    for (unsigned long i = 1; i < n; i++)
        for (unsigned long j = i; j > 0 && cmp(a + (j-1)*size, a+j*size) > 0; j--) {
            char* x = a+(j-1)*size; char* y = a+j*size;
            for (unsigned long k = 0; k < size; k++) { char t=x[k]; x[k]=y[k]; y[k]=t; }
        }
}
