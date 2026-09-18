;*---------------------------------------------------------------------------
;  :Modul.	RoFSlave.s
;  :Contents.	WHDLoad slave for the Amiga port of "Rescue on Fractalus!"
;  :Author.	Vesuri
;  :History.	14.08.26 started
;  :Requires.	WHDLoad 17+, whdload/kick13.s, an installed Kickstart 1.3 image
;  :Copyright.	Public Domain
;  :Language.	68000 Assembler
;  :Translator.	BASM 2.16
;---------------------------------------------------------------------------*
;
; WHY THIS SLAVE IS A KICKEMU AND NOT A PLAIN LOADER
;
; A plain WHDLoad slave runs the installed program with *no* operating system at
; all -- WHDLoad sets execbase ($4) to $f0000001 so that any OS access takes an
; address error.  That works for a trackloader game, but RoF is an ordinary
; AmigaDOS executable whose entire display takeover IS operating system calls:
;
;   OpenLibrary("graphics.library",33) / CloseLibrary
;   AllocMem / FreeMem              -- every bitmap, copper list, sprite, audio buffer
;   LoadView / WaitTOF              -- save + restore the OS view
;   SysBase->IntVects[INTB_VERTB]   -- the VBI vector takeover, plus Disable/Enable
;   OpenResource("ciaa.resource") + AddICRVector -- the CIA-A SP keyboard handler
;   Forbid / Permit / AvailMem
;
; So instead of emulating a dozen entry points, the slave boots a real Kickstart
; into WHDLoad's memory (WHDLoad's own kick13.s does all of that) and runs the game
; inside it as a normal CLI program.  The installed file is byte for byte the same
; `RoF` that runs from Workbench or a Shell; the loader only patches its explicit
; magic-tagged hook/configuration blocks in memory before entry.
;
; 1.3 rather than 3.1 for two reasons: the game is 1.3-clean (it asks for
; graphics.library v33 and touches nothing newer), and the 1.3 ROM image is 256 KB
; against 3.1's 512 KB, which comes straight off the memory requirement.
;
; HOW THE HIGH SCORES ARE SAVED
;
; The game persists its 256-byte high-score block itself, with dos.library, when
; it is started from a Shell or Workbench -- but only on the way out, because the
; write happens mid-takeover (Forbid, no OS display) where a filesystem packet must
; not be waited on.  Here we can do better: resload_SaveFile is safe to call at any
; moment, so a score is on disk the instant the player types the initials, and it
; survives a hard reset or the F10 quit key.
;
; The game does not know what WHDLoad or resload are.  It exports a small block of
; function pointers (src/platform/amiga/ExternalHooks.h) which are 0 in the shipped
; executable, and _patch_hooks below fills them with the trampolines at the end of
; this file (including the option-controlled logo callback).  The block is located by
; SCANNING the loaded hunks for its magic,
; so the game can be rebuilt -- moving every offset -- without touching this slave.
;
;---------------------------------------------------------------------------*

	INCDIR	Include:
	INCLUDE	whdload.i
	INCLUDE	whdmacros.i
;	INCLUDE	lvo/dos_lib.i

;============================================================================
; kick13.s configuration
;============================================================================
; kick13.s derives the WHDLoad memory request from these two:
;   ws_BaseMemSize = CHIPMEMSIZE                     (chip, mapped from address 0)
;   ws_ExpMem      = $40000 + FASTMEMSIZE            ($40000 = the 1.3 ROM image)
;
; What has to fit in each, and where the numbers come from:
;
;   CHIP  212 KB  the game AllocMem(MEMF_CHIP)s at runtime -- bitmaps, copper lists,
;                 sprites, audio buffers.  MEASURED, not estimated: 217,408 bytes,
;                 amiga/memreport.gdb (it snapshots exec's free pools either side of
;                 the scene constructor + display takeover).
;          15 KB  the .MEMF_CHIP hunk (14,654 bytes, objdump -h out/RoF.elf)
;           ? KB  the 1.3 boot's own chip use -- CLI screen bitmap, copper, fonts
;
;   FAST 487 KB   the non-chip hunks: .text 222,352 + .rodata 20,524
;                 + .init_array 4 + .data 2,924 + .bss 252,244 = 498,048 bytes
;           1 KB  operator new (measured: 1,272 bytes)
;          16 KB  the CLI stack (STACKSIZE below)
;           ? KB  exec/dos/filesystem-handler structures
;
; The two "?" lines are why these are set generously rather than tightly.  To tune
; them properly, build the TUNE slave (`make RoFTune.slave`), play through, and read
; the low-water marks -- see docs/whdload-slave.md.

CHIPMEMSIZE	= $4c000	;384 KB
FASTMEMSIZE	= $90000	;576 KB
NUMDRIVES	= 1		;NOT 0: only 3.1 survives a driveless boot, 1.2/1.3 crash
WPDRIVES	= %0000		;all emulated drives write protected

BLACKSCREEN			;1.3's boot colours all black -- no CLI flash before the game
BOOTDOS				;_bootdos below runs as a real CLI process
CACHECHIP			;instruction cache on (chip write-through) for accelerated machines
HDINIT				;mount slv_CurrentDir as DH0: -- BOOTDOS requires it
SEGTRACKER			;so a WHDLoad crash report names our hunk + offset
STACKSIZE	= 4096		;V33's initial CLI default is 4000 bytes

	IFD TUNE
DEBUG				;extra internal checks in the OS emulation
MEMFREE		= $200		;record the low-water mark of free chip/fast memory
	ENDC

slv_Version	= 17		;ws_config/Custom1: optional ECS/AGA border blanking
slv_Flags	= WHDLF_NoError	;kick13.s ORs in EmulPriv (needed by exec.Supervisor)
				;and, because HDINIT is set, Examine
slv_keyexit	= $59		;F10.  Note WHDLoad can only read this via the moved VBR,
				;i.e. on a 68010 or better; on a plain 68000 A500 the way
				;out is the game's own quit (left mouse button), which
				;returns here and aborts cleanly.

	INCLUDE	whdload/kick13.s

;============================================================================

slv_CurrentDir	dc.b	"data",0
slv_name	dc.b	"Rescue on Fractalus!",0
slv_copy	dc.b	"1985 Lucasfilm Games",0
		;-1 = line feed plus a half-font vertical skip, so it ends a section
slv_info	dc.b	"Amiga port by Vesuri",10
		dc.b	"version 1.0 (13.09.2026)",-1
		dc.b	"An unofficial, non-commercial fan project,",10
		dc.b	"not affiliated with or endorsed by Lucasfilm.",-1
		dc.b	"Left mouse button quits.",10
		dc.b	"F10 also quits, on a 68010 or better.",0
; Enhanced Graphics is opt-in at build time (basm -dROF_ENHANCED_GRAPHICS=1, via
; `make ENHANCED_GRAPHICS=1`).  A default slave omits the enhanced logo/games
; artwork, its logo hook and the Custom4 patch, so it must not advertise Custom4.
slv_config	dc.b	"C1:B:Border Blanking (ECS/AGA only);"
		dc.b	"C2:B:Enhanced Terrain Rendering;"
		dc.b	"C3:B:Enhanced Palette;"
	IFD ROF_ENHANCED_GRAPHICS
		dc.b	"C4:B:Enhanced Graphics;"
	ENDC
		dc.b	0
		dc.b	"$VER: RoF.slave 1.0 (13.09.2026)",0
	EVEN

;============================================================================
; The game.
;
; dos.library is fully up by now and we are a real CLI process (kick13.s reaches
; _bootdos through a synthesised startup-sequence), so starting the game is just
; LoadSeg + call -- exactly what a Shell would do.
;
; It returns when the player quits, and at that point there is nothing left to run:
; drop back to the 1.3 Shell and the user would face a prompt they cannot see
; (BLACKSCREEN) and, on a 68000, could not exit from (no QuitKey without a VBR).
; So quit WHDLoad instead.

_program	dc.b	"RoF",0
_romfile	dc.b	"rof.rom",0
_args		dc.b	10		;empty argument line -- must be LF terminated
_args_end
	EVEN

_bootdos	move.l	(_resload,pc),a2	;A2 = resload

	;read the version-17 Custom1..4 options before loading/patching the game
		lea	(_rof_tags,pc),a0
		jsr	(resload_Control,a2)

	;open dos.library.  OldOpenLibrary, not TaggedOpenLibrary: V33 has no
	;TaggedOpenLibrary.  _dosname comes from kickfs.s (present because HDINIT).
		lea	(_dosname,pc),a1
		move.l	(4),a6
		jsr	(_LVOOldOpenLibrary,a6)
		move.l	d0,a6			;A6 = dosbase
		tst.l	d0
		beq	.dos_err

	;load the game.  SEGTRACKER has patched LoadSeg, so the hunks are registered
	;with WHDLoad here and a later crash gets reported against them by name.
		lea	(_program,pc),a0
		move.l	a0,d1
		jsr	(_LVOLoadSeg,a6)
		move.l	d0,d7			;D7 = segment list (BPTR)
		beq	.program_err

	;Fill the executable's original-data BSS from the user-supplied cartridge.
	;Only three audited ranges are read; the complete ROM remains installed so its
	;provenance is explicit and no derived game-data file is distributed.
	bsr	_load_game_data

	;give the game somewhere to save its high scores (see _patch_hooks)
		bsr	_patch_hooks

	;optionally patch the executable-side BPLCON3 configuration word
	bsr	_patch_bplcon3

	;optionally select the executable's enhanced flight terrain renderer
	bsr	_patch_terrain_renderer

	;optionally select enhanced colour resolution for identified palette fades
	bsr	_patch_enhanced_palette

	IFD ROF_ENHANCED_GRAPHICS
	;Custom4 selects every enhanced-graphics feature, including the cockpit path
	bsr	_patch_enhanced_graphics
	ENDC

	;call it.  D0/A0 = argument line, as dos would pass them; the game's CRT
	;ignores both (its main() takes no arguments).
		move.l	d7,a1
		add.l	a1,a1
		add.l	a1,a1			;BPTR -> APTR
		moveq	#_args_end-_args,d0
		lea	(_args,pc),a0
		jsr	(4,a1)			;first hunk + 4 = the code

	;quit
		pea	TDREASON_OK
		move.l	(_resload,pc),a2
		jmp	(resload_Abort,a2)

	;LoadSeg failed -- report the file and dos.IoErr()
.program_err	jsr	(_LVOIoErr,a6)
		pea	(_program,pc)
		move.l	d0,-(a7)
		pea	TDREASON_DOSREAD
		jmp	(resload_Abort,a2)

	;no dos.library.  Cannot happen short of a broken OS emulation, so say that.
.dos_err	clr.l	-(a7)
		clr.l	-(a7)
		pea	TDREASON_OSEMUFAIL
		jmp	(resload_Abort,a2)

;============================================================================
; Locate the retained RoF!DATA descriptor in the LoadSeg hunks, load the three
; cartridge ranges into its BSS package, then publish the ready marker.

data_MAGIC0	= $526f4621		;'RoF!'
data_MAGIC1	= $44415441		;'DATA'
data_VERSION	= 1
data_READY	= $52444621		;'RDF!'
data_DESC_SIZE	= 24
	INCLUDE	rof_data_layout.i

_load_game_data
		movem.l	d2-d6/a3-a5,-(a7)
		lea	(_romfile,pc),a0
		jsr	(resload_GetFileSize,a2)
		cmp.l	#$10000,d0
		bne	.bad

		move.l	d7,d0			;current hunk BPTR
.seg		tst.l	d0
		beq	.bad
		add.l	d0,d0
		add.l	d0,d0
		move.l	d0,a3			;hunk header
		move.l	(-4,a3),d1		;allocated size incl header
		move.l	(a3)+,d0		;next BPTR; A3 = first data byte
		sub.l	#8+data_DESC_SIZE,d1
		bmi.s	.seg
		move.l	a3,a4
		add.l	d1,a4
.scan		cmpa.l	a4,a3
		bhi.s	.seg
		cmp.l	#data_MAGIC0,(a3)
		bne.s	.next
		cmp.l	#data_MAGIC1,4(a3)
		bne.s	.next
		cmp.w	#data_VERSION,8(a3)
		bne	.bad
		cmp.l	#data_SIZE,20(a3)
		bne	.bad
		move.l	16(a3),a5		;package BSS destination

		lea	(_romfile,pc),a0
		move.l	a5,a1
		move.l	#data_ROM0_SIZE,d0
		move.l	#data_ROM0_OFFSET,d1
		jsr	(resload_LoadFileOffset,a2)

		lea	(_romfile,pc),a0
		lea	data_PACK1_OFFSET(a5),a1
		move.l	#data_ROM1_SIZE,d0
		move.l	#data_ROM1_OFFSET,d1
		jsr	(resload_LoadFileOffset,a2)

		lea	(_romfile,pc),a0
		lea	data_PACK2_OFFSET(a5),a1
		move.l	#data_ROM2_SIZE,d0
		move.l	#data_ROM2_OFFSET,d1
		jsr	(resload_LoadFileOffset,a2)

	;Reject a different 64 KB cartridge before the executable is entered.  CRC32 uses
	;a 16-entry nibble table: small, and much faster than eight bit steps per byte.
		move.l	a5,a0
		move.l	#data_SIZE,d0
		moveq	#-1,d1
.crcbyte	moveq	#0,d2
		move.b	(a0)+,d2
		eor.l	d2,d1
		move.l	d1,d2
		and.w	#$f,d2
		lsl.w	#2,d2
		lsr.l	#4,d1
		eor.l	(_crc_table,pc,d2.w),d1
		move.l	d1,d2
		and.w	#$f,d2
		lsl.w	#2,d2
		lsr.l	#4,d1
		eor.l	(_crc_table,pc,d2.w),d1
		subq.l	#1,d0
		bne.s	.crcbyte
		not.l	d1
		cmp.l	#data_CRC32,d1
		bne	.bad

		move.l	#data_READY,12(a3)
		movem.l	(a7)+,d2-d6/a3-a5
		rts
.next		addq.l	#4,a3
		bra.s	.scan
.bad		pea	TDREASON_WRONGVER
		jmp	(resload_Abort,a2)

_crc_table	dc.l	$00000000,$1db71064,$3b6e20c8,$26d930ac
		dc.l	$76dc4190,$6b6b51f4,$4db26158,$5005713c
		dc.l	$edb88320,$f00f9344,$d6d6a3e8,$cb61b38c
		dc.l	$9b64c2b0,$86d3d2d4,$a00ae278,$bdbdf21c

;============================================================================
; The external-hook block.
;
; Layout (src/platform/amiga/ExternalHooks.h) -- the fields up to and including
; hook_Load are frozen; a later game may only APPEND, and must bump hook_VERSION
; when it does, so the version check below fails safe rather than writing pointers
; into a layout we do not know.  hook_SIZEOF is the minimum size we accept, not an
; exact match, so a grown block still gets patched.

hook_MAGIC0	= $526f4621		;'RoF!'
hook_MAGIC1	= $484f4f4b		;'HOOK'
hook_VERSION	= 3
hook_Save	= 12			;APTR  int (*)(const UBYTE *blk, ULONG len)
hook_Load	= 16			;APTR  int (*)(UBYTE *blk, ULONG len)
hook_Logo	= 20			;APTR  ULONG (*)(struct RofLogoOverrideContext *)
hook_SIZEOF	= 24
logo_USE_BITMAP = 1
logo_USE_PALETTE = 2
logo_USE_GEOMETRY = 4

; Walk the segment list and patch the block wherever it turns out to be.
;
; Every hunk is scanned, on longword boundaries: the block is declared 4-aligned and a
; hunk is allocated 4-aligned, so its address is a multiple of 4 from the hunk start.
; The eight magic bytes exist nowhere else in the image -- they are built by the block's
; initialiser and by nothing else -- and there is exactly one block, so the first match
; ends the search.  Finding no block is NOT an error: an older game simply saves the
; scores its own way.
;
; This reads the whole ~290 KB load image once, which is around half a second of 68000,
; so it lives here in the loader (behind the WHDLoad splash) and the inner loop is kept
; to the bounds check and one long compare.
;
;  in:      D7 = segment list (BPTR), as returned by LoadSeg
;  trashes: D0-D1/A0-A1
;  keeps:   A2 (resload), A6 (dosbase), D7

_patch_hooks	movem.l	d2-d3,-(a7)
		move.l	#hook_MAGIC0,d2
		move.l	#hook_MAGIC1,d3
		move.l	d7,d0			;D0 = current segment (BPTR)

.seg		tst.l	d0
		beq.s	.done			;end of the list, no block: fine
		add.l	d0,d0
		add.l	d0,d0			;BPTR -> APTR, the segment header
		move.l	d0,a0
		move.l	(-4,a0),d1		;allocated size, incl. the 8-byte header
		move.l	(a0)+,d0		;next segment; A0 = first data byte
		sub.l	#8+hook_SIZEOF,d1	;last offset a whole block can start at
		bmi.s	.seg			;hunk too small to hold one
		move.l	a0,a1
		add.l	d1,a1			;A1 = that address

.scan		cmpa.l	a1,a0
		bhi.s	.seg			;hunk exhausted
		cmp.l	(a0)+,d2		;magic0.  A0 now points at magic1, so
		bne.s	.scan			;the field offsets below are all -4
		cmp.l	(a0),d3			;magic1
		bne.s	.scan
		cmp.w	#hook_VERSION,(4,a0)	;a layout we know how to write?
		bne.s	.scan
		cmp.w	#hook_SIZEOF,(6,a0)
		blo.s	.scan

		lea	(_hook_save,pc),a1
		move.l	a1,(hook_Save-4,a0)
		lea	(_hook_load,pc),a1
		move.l	a1,(hook_Load-4,a0)
	IFD ROF_ENHANCED_GRAPHICS
		lea	(_hook_logo,pc),a1
		move.l	a1,(hook_Logo-4,a0)
	ENDC
.done		movem.l	(a7)+,d2-d3
		rts


;============================================================================
; Optional ECS/AGA border blanking.
;
; The executable contains a retained writable block:
;   dc.l 'RoF!','BPL3'
;   dc.w $0c10,0
; Its word is consumed by every game-side BPLCON3 setup.  Scan the LoadSeg hunks
; rather than baking in an offset, and only patch the expected default value.

bpl3_MAGIC0	= $526f4621		;'RoF!'
bpl3_MAGIC1	= $42504c33		;'BPL3'
bpl3_SIZEOF	= 12
bpl3_DEFAULT	= $0c10		;$0c00 | BPLCON3_BRDNTRAN
bpl3_BLANKED	= $0c30		;$0c00 | BPLCON3_BRDNBLNK | BPLCON3_BRDNTRAN

_patch_bplcon3
		lea	(_rof_custom1,pc),a0	;tst has no PC-relative mode
		tst.l	(a0)
		beq.s	.done			;default: visible COLOR00 border
		movem.l	d2-d5,-(a7)
		move.l	#bpl3_MAGIC0,d2
		move.l	#bpl3_MAGIC1,d3
		move.w	#bpl3_DEFAULT,d4
		move.w	#bpl3_BLANKED,d5
		bsr.s	_patch_config_word
		movem.l	(a7)+,d2-d5
.done		rts

terr_MAGIC0	= $526f4621		;'RoF!'
terr_MAGIC1	= $54455252		;'TERR'
terr_DEFAULT	= 0
terr_ENHANCED	= 1

_patch_terrain_renderer
		lea	(_rof_custom2,pc),a0
		tst.l	(a0)
		beq.s	.done			;default: original 2x2 renderer
		movem.l	d2-d5,-(a7)
		move.l	#terr_MAGIC0,d2
		move.l	#terr_MAGIC1,d3
		move.w	#terr_DEFAULT,d4
		move.w	#terr_ENHANCED,d5
		bsr.s	_patch_config_word
		movem.l	(a7)+,d2-d5
.done		rts

epal_MAGIC0	= $526f4621		;'RoF!'
epal_MAGIC1	= $4550414c		;'EPAL'
epal_DEFAULT	= 0
epal_ENHANCED	= 1

_patch_enhanced_palette
		lea	(_rof_custom3,pc),a0
		tst.l	(a0)
		beq.s	.done			;default: faithful Atari colour resolution
		movem.l	d2-d5,-(a7)
		move.l	#epal_MAGIC0,d2
		move.l	#epal_MAGIC1,d3
		move.w	#epal_DEFAULT,d4
		move.w	#epal_ENHANCED,d5
		bsr.s	_patch_config_word
		movem.l	(a7)+,d2-d5
.done		rts

	IFD ROF_ENHANCED_GRAPHICS
egrf_MAGIC0	= $526f4621		;'RoF!'
egrf_MAGIC1	= $45475246		;'EGRF'
egrf_DEFAULT	= 0
egrf_ENHANCED	= 1

_patch_enhanced_graphics
		lea	(_rof_custom4,pc),a0
		tst.l	(a0)
		beq.s	.done			;default: faithful cockpit graphics
		movem.l	d2-d5,-(a7)
		move.l	#egrf_MAGIC0,d2
		move.l	#egrf_MAGIC1,d3
		move.w	#egrf_DEFAULT,d4
		move.w	#egrf_ENHANCED,d5
		bsr.s	_patch_config_word
		movem.l	(a7)+,d2-d5
.done		rts
	ENDC

; D2/D3 = block magic, D4 = expected word, D5 = replacement word.
_patch_config_word
		move.l	d7,d0			;D0 = current segment (BPTR)

.seg		tst.l	d0
		beq.s	.not_found
		add.l	d0,d0
		add.l	d0,d0			;BPTR -> segment header APTR
		move.l	d0,a0
		move.l	(-4,a0),d1		;allocated size, incl. the 8-byte header
		move.l	(a0)+,d0		;next segment; A0 = first data byte
		sub.l	#8+bpl3_SIZEOF,d1	;both retained blocks are 12 bytes
		bmi.s	.seg
		move.l	a0,a1
		add.l	d1,a1			;last address a whole block can start

.scan		cmpa.l	a1,a0
		bhi.s	.seg
		cmp.l	(a0)+,d2		;A0 now points at magic1
		bne.s	.scan
		cmp.l	(a0),d3
		bne.s	.scan
		cmp.w	(4,a0),d4
		bne.s	.scan
		move.w	d5,(4,a0)
.not_found	rts


;============================================================================
; The trampolines.
;
; Called by the game with the ordinary GCC m68k C convention: arguments pushed
; right to left, each a longword, result in D0, everything but D0-D1/A0-A1
; preserved by the callee.  resload destroys exactly those same four registers,
; so only the register we pick up the resload base in has to be saved.
;
; Both may be called mid-takeover, with the game's own copper list and interrupt
; vectors installed.  That is what resload is for; WHDLoad switches to the OS and
; back around the file access itself.
;
; Note WHDLF_NoError: resload_SaveFile and resload_LoadFile only RETURN on success,
; so the FALSE arms below are unreachable in practice -- a write to a full or
; write-protected data directory ends in WHDLoad's own error requester instead of
; being reported back to the game.  resload_GetFileSize is the exception, and is
; the reason the load path asks the size first: a first run has no file yet, and
; that must be an ordinary "nothing saved", not a failure.

_hifile		dc.b	"RoF.hi",0
	EVEN

_rof_tags	dc.l	WHDLTAG_CUSTOM1_GET
_rof_custom1	dc.l	0
		dc.l	WHDLTAG_CUSTOM2_GET
_rof_custom2	dc.l	0
		dc.l	WHDLTAG_CUSTOM3_GET
_rof_custom3	dc.l	0
	IFD ROF_ENHANCED_GRAPHICS
		dc.l	WHDLTAG_CUSTOM4_GET
_rof_custom4	dc.l	0
	ENDC
		dc.l	TAG_DONE

; int _hook_save(const UBYTE *blk, ULONG len)      4(sp)=blk  8(sp)=len
_hook_save	move.l	a2,-(a7)
		move.l	(12,a7),d0		;size
		move.l	(8,a7),a1		;source
		lea	(_hifile,pc),a0
		move.l	(_resload,pc),a2
		jsr	(resload_SaveFile,a2)	;D0 = TRUE on success
		move.l	(a7)+,a2
		rts

; int _hook_load(UBYTE *blk, ULONG len)            4(sp)=blk  8(sp)=len
_hook_load	movem.l	d2/a2,-(a7)
		move.l	(16,a7),d2		;D2 = the size the game expects
		move.l	(_resload,pc),a2
		lea	(_hifile,pc),a0
		jsr	(resload_GetFileSize,a2)
		cmp.l	d2,d0
		bne.s	.fail			;absent, or not a block of ours
		lea	(_hifile,pc),a0
		move.l	(12,a7),a1		;destination
		jsr	(resload_LoadFile,a2)
		cmp.l	d2,d0
		bne.s	.fail
		moveq	#1,d0
		bra.s	.out
.fail		moveq	#0,d0
.out		movem.l	(a7)+,d2/a2
		rts

; ULONG _hook_logo(struct RofLogoOverrideContext *ctx)   4(sp)=ctx
;
; Custom4=Enhanced Graphics supplies the bundled 288x100 Lucasfilm image.  The
; initial phase copies its GAMES-free 320x100 planar field and palette; the GAMES
; phase copies the extracted centre overlay into the already displayed field.
;
; Context v2 is 40 bytes (the first 36 bytes are the old v1 layout):
;   +0.w version=2       +2.w size=40          +4.l bitmap
;   +8.l bitmapBytes     +12.w width=320       +14.w height=340
;   +16.w bitplanes=4    +18.w planeRowBytes=40
;   +20.w rowBytes=160   +22.w visibleRows=62  +24.w topBlankLines=64
;   +26.w format=1       +28.l palette         +32.w paletteEntries
;   +34.w reserved       +36.w phase           +38.w phaseReserved
;
; phase=1 is INITIAL: the bitmap is not displayed yet, palette points at 16 native
; Amiga $0RGB words, and returning bit 0/1/2 claims bitmap/palette/geometry.
; phase=2 is GAMES, exactly at the original 86-frame cue: bitmap is the live planar
; field and palette is null.  Overlay it in place and return logo_USE_BITMAP to keep
; the game from applying its original GAMES update.  Returning zero at either phase
; selects that phase's original path.
;
; Built only for ENHANCED_GRAPHICS: without it neither the trampoline nor the artwork
; below is assembled, and _patch_hooks leaves the game's logo hook pointer at 0.
	IFD ROF_ENHANCED_GRAPHICS
_hook_logo	lea	(_rof_custom4,pc),a0
		tst.l	(a0)
		beq.s	.decline
		move.l	(4,a7),a0		;context
		cmp.w	#2,(a0)		;context version
		bne.s	.decline
		cmp.w	#40,(2,a0)		;minimum context size
		blo.s	.decline
		cmp.w	#1,(26,a0)		;4-plane interleaved format
		bne.s	.decline
		cmp.w	#320,(12,a0)
		bne.s	.decline
		cmp.w	#340,(14,a0)
		bne.s	.decline
		cmp.w	#4,(16,a0)
		bne.s	.decline
		cmp.w	#40,(18,a0)
		bne.s	.decline
		cmp.w	#160,(20,a0)
		bne.s	.decline

		cmp.w	#1,(36,a0)		;INITIAL?
		beq.s	.initial
		cmp.w	#2,(36,a0)		;GAMES?
		bne.s	.decline

		;30 rows x 4 planes x 11 bytes.  The source rectangle is image
		;x=104..191, centred at bitmap x=120; row 65 begins at byte $28a0.
		move.l	(4,a0),a1
		lea	($28af,a1),a1
		lea	(_enhanced_games,pc),a0
		move.w	#119,d0		;120 plane spans
.games_copy	move.l	(a0)+,(a1)+
		move.l	(a0)+,(a1)+
		move.w	(a0)+,(a1)+
		move.b	(a0)+,(a1)+
		lea	(29,a1),a1		;next 40-byte plane row
		dbf	d0,.games_copy
		moveq	#logo_USE_BITMAP,d0
		rts

.initial	cmp.l	#16000,(8,a0)		;100 complete 160-byte rows
		blo.s	.decline
		move.l	a0,d1			;keep context in caller-saved D1
		move.l	(4,a0),a1
		lea	(_enhanced_logo,pc),a0
		move.w	#3999,d0		;16,000 bytes / 4
.logo_copy	move.l	(a0)+,(a1)+
		dbf	d0,.logo_copy
		move.l	d1,a0
		move.l	(28,a0),a1		;palette destination
		tst.l	a1
		beq.s	.decline
		cmp.w	#16,(32,a0)
		blo.s	.decline
		lea	(_enhanced_palette,pc),a0
		moveq	#15,d0
.pal_copy	move.w	(a0)+,(a1)+
		dbf	d0,.pal_copy
		move.l	d1,a0
		move.w	#100,(22,a0)		;native image height
		move.w	#58,(24,a0)		;vertically centred in 216 lines
		moveq	#logo_USE_BITMAP|logo_USE_PALETTE|logo_USE_GEOMETRY,d0
		rts

.decline	moveq	#0,d0
		rts

	EVEN
_enhanced_logo
	INCBIN	"assets/enhanced_logo.bin"
_enhanced_games
	INCBIN	"assets/enhanced_games.bin"
_enhanced_palette
	INCBIN	"assets/enhanced_logo.pal"
	ENDC

;============================================================================

	END
