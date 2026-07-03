; FillTerrainAssembler.s — hand-written m68k twin of fill_terrain_columns_core
; (asm-migration-plan Phase 3).  Plain C linkage (our own function, no args):
;   void fill_terrain_columns_core(void)
;
; One-shot stars/planet field build: fill all 89 columns (index $59..$01, right-to-left;
; column 0 left untouched) of the four parallel terrain-height buffers ($0C32/$0D32/$0E32/
; $0F32) with sparse random heights.  Each sample is flat (0) 31/32 of the time; 1/32 (low
; 5 random bits all zero) it takes a second RANDOM read and picks one of four preset heights
; from the $6B5F table.  The 17-bit POKEY LFSR (rof_lfsr_state, x^17+x^5+1) is held in d0 for
; the whole run (one load at entry, one store at exit).
;
; Byte-identical to the C oracle (fill_terrain_columns_core_c in rof_native.c) — verified by
; the in-process differential (make VERIFY=1 PROBES=1 + fillterr_verify.gdb): the twin runs on
; the real state from the current LFSR seed, its 4 buffers + resulting LFSR are captured, and
; the C oracle is run from the same seed and compared.
;
; Win over GCC's C: byte-width LFSR ANDs (andi.b #1 / bclr #17 vs GCC's long andi.l #imm =
; 16cyc each) and displacement-addressed stores (move.b d3,(disp,a1) vs GCC's per-store
; move.l/addi.l/indexed) — the two things GCC won't emit here.
;
; Registers (d2-d5/a2-a3 callee-saved; d0/d1/a1 caller-saved scratch):
;   d0 = LFSR state s        d1 = col ($59..1)     a1 = mem + $0C32 + col (this col's layer 0)
;   d2 = random byte / index d3 = height / table   d4/d5 = LFSR scratch
;   a2 = table base (mem + $6B5F)   a3 = buffer base (mem + $0C32)
; The indexed modes ($6B5F/$0C32 as base) go in address regs because (d,An,Xn) allows only an
; 8-bit displacement; the four stores use (d16,a1) (no index) which does take a 16-bit $0300.

	xdef	fill_terrain_columns_core_asm
	ifnd	ROF_FILLTERR_VERIFY
	xdef	fill_terrain_columns_core		; ships as the core symbol directly
	endif
	xref	mem
	xref	rof_lfsr_state

	section	code

; RAND: advance the 17-bit LFSR in d0 (tap = bit16 ^ bit4), leave new low byte in d2.
RAND	macro
	move.l	d0,d4
	clr.w	d4
	swap	d4			; d4 = s>>16 (0 or 1 — state is 17-bit)
	move.b	d0,d5
	lsr.b	#4,d5			; d5 bit0 = bit4 of s
	eor.b	d5,d4			; d4 bit0 = bit16 ^ bit4
	andi.b	#1,d4			; isolate the tap
	add.l	d0,d0			; s <<= 1
	or.b	d4,d0			; insert tap into bit0
	bclr	#17,d0			; clear the lone carry bit -> keep state 17-bit (== &$1FFFF)
	move.b	d0,d2			; r = (uint8_t)s
	endm

; SAMPLE \1 = store displacement off a1 (0/$100/$200/$300).  One height sample + store.
SAMPLE	macro
	RAND
	moveq	#0,d3			; h = 0 (flat ground, 31/32)
	andi.b	#$1F,d2
	bne.b	.st\@			; (r & $1F) != 0 -> flat, keep h=0
	RAND				; 1/32: second read -> table index
	andi.w	#3,d2
	move.b	(a2,d2.w),d3		; h = TERRAIN_HEIGHT_TABLE[r2 & 3]
.st\@:
	move.b	d3,(\1,a1)
	endm

	ifnd	ROF_FILLTERR_VERIFY
fill_terrain_columns_core:
	endif
fill_terrain_columns_core_asm:
	movem.l	d2-d5/a2-a3,-(sp)
	lea	mem,a3
	lea	($6B5F,a3),a2		; a2 = table base (mem + $6B5F)  (d16,An)
	lea	($0C32,a3),a3		; a3 = buffer base (mem + $0C32)
	move.l	rof_lfsr_state,d0	; LFSR in a register for the whole run
	moveq	#$59,d1			; col = $59, count down to 1
.col:
	lea	(a3,d1.w),a1		; a1 = mem + $0C32 + col
	SAMPLE	0			; layer 0 -> $0C32 + col
	SAMPLE	$0100			; layer 1 -> $0D32 + col
	SAMPLE	$0200			; layer 2 -> $0E32 + col
	SAMPLE	$0300			; layer 3 -> $0F32 + col
	subq.w	#1,d1
	bne.w	.col			; while col != 0
	move.l	d0,rof_lfsr_state	; commit the advanced LFSR back once
	movem.l	(sp)+,d2-d5/a2-a3
	rts
