/* ExternalHooks.h — the patch block an external launcher fills in before the game runs.
 *
 * The port has exactly one thing it cannot do for itself under every launch method:
 * persist the high-score block.  From a Shell or Workbench it is a dos.library file
 * (PlatformAmiga.cpp); under WHDLoad the right answer is resload_SaveFile, which only
 * the slave holds a pointer to.  Rather than teach the game about WHDLoad, the game
 * exports THIS block: two function pointers that are 0 in the shipped executable and
 * that whoever launched it may overwrite with its own routines.  A 0 pointer means
 * "no external storage" and the platform falls back to its own path, so the standalone
 * binary behaves exactly as before — the very same `RoF` file is used either way.
 *
 * FINDING THE BLOCK.  The patcher is not rebuilt when the game is, so it must not know
 * any offset.  The block starts with an 8-byte magic and is longword-aligned in the
 * data hunk; the patcher walks the seglist LoadSeg() returned, scans each hunk at even
 * addresses for magic0/magic1, checks `version` and that `size` is at least as large as
 * the layout it knows, and stores its pointers.  Nothing else in the image carries the
 * magic (it exists only in this block's initialiser), and version/size are the guard
 * against a future layout change: a patcher that does not recognise `version` must
 * patch nothing rather than guess.  The reference patcher is `_patch_hooks` in
 * whdload/RoFSlave.s.
 *
 * GROWING IT.  Append fields, bump ROF_HOOKS_VERSION, leave the existing ones where
 * they are.  `size` is written by the compiler, so an old patcher's `size >= its own
 * layout` check keeps working.
 *
 * ABI.  The hooks are called with the ordinary GCC m68k C convention — arguments on the
 * stack in declaration order, each occupying a longword, result in D0, D2-D7/A2-A6
 * preserved by the callee.  They may be called at any point during the run, including
 * mid-takeover with the display and the interrupt vectors hijacked, so a hook that
 * needs the operating system must be able to reach it from there (resload can; a
 * dos.library packet cannot, which is the whole reason the fallback path defers).
 */
#ifndef ROF_EXTERNAL_HOOKS_H
#define ROF_EXTERNAL_HOOKS_H

/* 'RoF!' 'HOOK' — the two longwords the patcher scans for. */
#define ROF_HOOKS_MAGIC0   0x526F4621UL
#define ROF_HOOKS_MAGIC1   0x484F4F4BUL
#define ROF_HOOKS_VERSION  1

#ifdef __cplusplus
extern "C" {
#endif

/* Both return non-zero on success.  len is always ROF_HISCORE_BLOCK_SIZE (256) today;
   it is passed so a patcher can refuse a block it was not written for. */
typedef int (*RofHiscoreSaveHook)(const unsigned char* blk, unsigned long len);
typedef int (*RofHiscoreLoadHook)(unsigned char* blk, unsigned long len);

struct RofExternalHooks {
    unsigned long  magic0;
    unsigned long  magic1;
    unsigned short version;      /* ROF_HOOKS_VERSION */
    unsigned short size;         /* sizeof(struct RofExternalHooks) */
    /* volatile: these are written by another program between LoadSeg and the first
       instruction of main(), which is not something the compiler can be told about. */
    RofHiscoreSaveHook volatile hiscoreSave;   /* 0 = the platform saves it itself */
    RofHiscoreLoadHook volatile hiscoreLoad;   /* 0 = the platform loads it itself */
};

extern struct RofExternalHooks g_rofExternalHooks;

#ifdef __cplusplus
}
#endif

#endif /* ROF_EXTERNAL_HOOKS_H */
