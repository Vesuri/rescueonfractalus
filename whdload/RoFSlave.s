;*---------------------------------------------------------------------------
;  :Modul.	RoFSlave.s
;  :Contents.	WHDLoad slave for the Amiga port of "Rescue on Fractalus!"
;  :Author.	Vesuri
;  :History.	14.08.26 started
;  :Requires.	WHDLoad 16+, whdload/kick13.s, an installed Kickstart 1.3 image
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
; inside it as a normal CLI program.  The game binary is used completely unmodified
; -- byte for byte the same `RoF` that runs from Workbench or a Shell.
;
; 1.3 rather than 3.1 for two reasons: the game is 1.3-clean (it asks for
; graphics.library v33 and touches nothing newer), and the 1.3 ROM image is 256 KB
; against 3.1's 512 KB, which comes straight off the memory requirement.
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
;   FAST 426 KB   the non-chip hunks: .text 200,386 + code 16,666 + .rodata 33,048
;                 + .init_array 4 + .data 2,777 + .bss 183,440 = 436,321 bytes
;           1 KB  operator new (measured: 1,272 bytes)
;          16 KB  the CLI stack (STACKSIZE below)
;           ? KB  exec/dos/filesystem-handler structures
;
; The two "?" lines are why these are set generously rather than tightly.  To tune
; them properly, build the TUNE slave (`make RoFTune.slave`), play through, and read
; the low-water marks -- see docs/whdload-slave.md.

CHIPMEMSIZE	= $4c000	;384 KB
FASTMEMSIZE	= $78000	;480 KB
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

slv_Version	= 16		;16, not 17, ON PURPOSE: ws_config exists only from 17 on
				;and MUST then be initialised, which puts a gadget in the
				;splash window.  There is nothing here to configure -- see
				;slv_config below.  WHDLoad's own kick13.asm example slave
				;is 16 for the same reason.
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
		dc.b	"version 0.9 (14.08.2026)",-1
		dc.b	"An unofficial, non-commercial fan project,",10
		dc.b	"not affiliated with or endorsed by Lucasfilm.",-1
		dc.b	"Left mouse button quits.",10
		dc.b	"F10 also quits, on a 68010 or better.",0
	IFGE slv_Version-17
		;NOT ASSEMBLED at slv_Version 16 -- kept only so a real option can be
		;added later by raising the version.  Every ws_config item is a gadget in
		;the splash window, and each one has to MEAN something: the options are
		;ButtonWait and Custom1-5, and this slave implements none of them.  It
		;used to declare "BW;" -- a ButtonWait checkbox the slave never reads (
		;WHDLoad leaves ButtonWait to the slave, see WHDLTAG_BUTTONWAIT_GET and
		;PL_IFBW), so the box did nothing when ticked.  An empty string is not
		;the fix either: ws_config's grammar wants at least one option.
slv_config	dc.b	"C1:B:Example",0
	ENDC
		dc.b	"$VER: RoF.slave 0.9 (14.08.2026)",0
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
_args		dc.b	10		;empty argument line -- must be LF terminated
_args_end
	EVEN

_bootdos	move.l	(_resload,pc),a2	;A2 = resload

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

	END
