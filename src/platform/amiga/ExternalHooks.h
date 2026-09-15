/* ExternalHooks.h — the patch block an external launcher fills in before the game runs.
 *
 * The port uses launcher services for facilities that do not belong in the standalone
 * game: persistent high scores and an optional replacement boot logo.  From a Shell or
 * Workbench the high-score block is a dos.library file
 * (PlatformAmiga.cpp); under WHDLoad the right answer is resload_SaveFile, which only
 * the slave holds a pointer to.  Rather than teach the game about WHDLoad, the game
 * exports THIS block: function pointers that are 0 in the shipped executable and
 * that whoever launched it may overwrite with its own routines.  A 0 pointer selects
 * that facility's built-in behaviour, so the standalone binary behaves exactly as
 * before — the very same `RoF` file is used either way.
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
#define ROF_HOOKS_VERSION  3

/* Logo override API.  The hook receives the game's already-allocated shared boot
 * bitmap and a writable copy of the normal logo palette.  It returns a mask of the
 * parts it supplied; every unclaimed part follows the original game path. */
#define ROF_LOGO_OVERRIDE_VERSION       2
#define ROF_LOGO_OVERRIDE_BITMAP        0x00000001UL
#define ROF_LOGO_OVERRIDE_PALETTE       0x00000002UL
#define ROF_LOGO_OVERRIDE_GEOMETRY      0x00000004UL
#define ROF_LOGO_FORMAT_PLANAR4_INTERLEAVED 0x0001
#define ROF_LOGO_PHASE_INITIAL          1
#define ROF_LOGO_PHASE_GAMES            2

#ifdef __cplusplus
extern "C" {
#endif

/* Both return non-zero on success.  len is always ROF_HISCORE_BLOCK_SIZE (256) today;
   it is passed so a patcher can refuse a block it was not written for. */
typedef int (*RofHiscoreSaveHook)(const unsigned char* blk, unsigned long len);
typedef int (*RofHiscoreLoadHook)(unsigned char* blk, unsigned long len);

/* Version 2 layout (40 bytes on m68k; its first 36 bytes are the v1 layout).
 * bitmap is 320x340x4 and rows are
 * interleaved: 40 bytes of plane 0, then planes 1..3, then the next row.
 * Only the first visibleRows (62) are displayed by the logo copper layout.
 * palette contains paletteEntries (16) native Amiga 0x0RGB colour words and is
 * pre-filled with the original gold ramp, so a hook may change only selected pens.
 *
 * The hook runs synchronously at each phase and must not retain either pointer.
 * At INITIAL, the bitmap is not live yet and palette may be edited.  At GAMES,
 * the bitmap is already live: edit it in place; palette is null/zero because the
 * copper palette cannot safely be rebuilt mid-display. */
struct RofLogoOverrideContext {
    unsigned short version;             /* ROF_LOGO_OVERRIDE_VERSION */
    unsigned short size;                /* sizeof(struct RofLogoOverrideContext) */
    unsigned char* bitmap;              /* writable CHIP RAM destination */
    unsigned long  bitmapBytes;         /* 54,400 */
    unsigned short width;               /* 320 pixels */
    unsigned short height;              /* 340 allocated rows */
    unsigned short bitplanes;           /* 4 */
    unsigned short planeBytesPerRow;     /* 40 */
    unsigned short rowBytes;             /* 160, all four interleaved planes */
    unsigned short visibleRows;          /* in/out at INITIAL; default 62 */
    unsigned short topBlankLines;        /* in/out at INITIAL; default 64 */
    unsigned short format;               /* ROF_LOGO_FORMAT_* */
    unsigned short* palette;             /* writable 16-entry 0x0RGB table */
    unsigned short paletteEntries;       /* 16 */
    unsigned short reserved;             /* v1 tail; must be ignored */
    unsigned short phase;                /* ROF_LOGO_PHASE_* */
    unsigned short phaseReserved;        /* must be ignored */
};

typedef unsigned long (*RofLogoOverrideHook)(struct RofLogoOverrideContext* context);

struct RofExternalHooks {
    unsigned long  magic0;
    unsigned long  magic1;
    unsigned short version;      /* ROF_HOOKS_VERSION */
    unsigned short size;         /* sizeof(struct RofExternalHooks) */
    /* volatile: these are written by another program between LoadSeg and the first
       instruction of main(), which is not something the compiler can be told about. */
    RofHiscoreSaveHook volatile hiscoreSave;   /* 0 = the platform saves it itself */
    RofHiscoreLoadHook volatile hiscoreLoad;   /* 0 = the platform loads it itself */
    RofLogoOverrideHook volatile logoOverride; /* 0 = original; build/slave may supply hook */
};

extern struct RofExternalHooks g_rofExternalHooks;

#ifdef __cplusplus
}
#endif

#endif /* ROF_EXTERNAL_HOOKS_H */
