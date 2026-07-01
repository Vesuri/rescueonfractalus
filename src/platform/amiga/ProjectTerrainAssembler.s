; ProjectTerrainAssembler.s — hand-written m68k twin of project_terrain_points_core
; (asm-migration-plan Phase 3).  Plain C linkage (our own function, no SAS/C wrapper):
;   void project_terrain_points_core(uint8_t X)
; GCC m68k passes the single int-promoted byte ON THE STACK; before any push its byte
; is at 7(sp) (the low byte of the 4-byte int at 4(sp); return address at 0(sp)).
;
; Byte-identical to the C oracle (project_terrain_points_core_c in rof_native.c) —
; verified by the in-process differential (make VERIFY=1 PROBES=1 + project_verify.gdb):
; each call runs this twin + the C oracle back-to-back on the same inputs and compares
; the cells it writes ($2400/$242D/$245A/$2487/$24B4 + X, plus $009F/$00B5).
;
; Per object X: mark it projected, run two perspective divides sharing the depth
; divisor {$232E:$2300}[X] — screen-X = {$22D2:$22A4}[X]/depth -> {$242D:$2400}[X],
; screen-Y = {$2388:$235B}[X]/depth -> {$2487:$245A}[X] — then add a per-band scroll
; offset (chosen from the screen-X result) into the screen-Y coordinate.
;
; The perspective divide (project_axis): normalize the divisor up (<<count, count<8)
; until it strictly exceeds the |numerator|; if it never fits -> off-screen edge
; ($40/$C0 hi, 0 lo).  Otherwise $9F=count, q = (|numer|<<8)/divisor (a single DIVU,
; quotient<256), then a 3/2 perspective factor /4 (count==0: 7-bit magnitude folded
; into the low byte, hi=0; count!=0: undo the <<count scaling into a 16-bit screen X
; around $80, $B5 = hi-fold scratch).
;
; Registers (callee-saved d2-d7 saved at entry; a0/a1/d0/d1 are scratch/caller-saved):
;   d7 = X (word, zero-extended)   a0 = mem + X   a1 = scratch address base
;   d6 = neg (per-axis)  d5 = count (per-axis)  d0/d1/d2 = scratch

	xdef	project_terrain_points_core_asm
	ifnd	ROF_PROJECT_VERIFY
	xdef	project_terrain_points_core		; ships as the core symbol directly
	endif
	xref	mem

	section	code

; --- one perspective divide.  \1=num_lo disp  \2=num_hi disp  \3=out_lo disp  \4=out_hi disp
PAXIS	macro
	; numer word = (mem[num_hi]<<8)|mem[num_lo]; neg detect; absn
	moveq	#0,d1
	move.b	(\2,a0),d1
	lsl.w	#8,d1
	move.b	(\1,a0),d1		; d1 = numer (16-bit, upper word 0)
	moveq	#0,d6			; neg = 0
	tst.w	d1
	bpl	.abs\@
	neg.w	d1			; absn = -numer
	moveq	#1,d6			; neg = 1
.abs\@:
	; divisor word = (mem[$232E]<<8)|mem[$2300]
	moveq	#0,d2
	move.b	($232E,a0),d2
	lsl.w	#8,d2
	move.b	($2300,a0),d2		; d2 = divisor (16-bit)
	moveq	#0,d5			; count = 0
.scale\@:
	cmp.w	d2,d1			; absn - divisor (unsigned)
	bcs	.fits\@			; absn < divisor -> fits
	lsl.w	#1,d2			; divisor <<= 1 (uint16 wrap)
	addq.w	#1,d5
	cmp.w	#8,d5
	bne	.scale\@
	; count == 8: never fit -> off-screen edge
	tst.w	d6
	bne	.edgeN\@
	move.b	#$40,(\4,a0)
	bra	.edgeL\@
.edgeN\@:
	move.b	#$C0,(\4,a0)
.edgeL\@:
	clr.b	(\3,a0)
	bra	.done\@
.fits\@:
	move.b	d5,mem+$9F		; draw_row_bottom = count
	move.l	d1,d0			; absn (upper word 0)
	lsl.l	#8,d0			; numerator = absn << 8
	divu.w	d2,d0			; d0 low word = quotient (<256), high = remainder
	and.l	#$FFFF,d0		; d0 = q (drop remainder)
	tst.w	d5
	beq	.c0\@
	; count != 0 : scaled = q<<count ; mag = (scaled + scaled>>1)>>2 ; screen = $80 +/- mag
	lsl.w	d5,d0			; d0 = scaled (uint16)
	move.w	d0,d1
	lsr.w	#1,d1
	add.w	d0,d1			; scaled + scaled>>1
	lsr.w	#2,d1			; d1 = mag (uint16)
	move.w	d1,d0
	lsr.w	#8,d0
	move.b	d0,mem+$B5		; $B5 = mag>>8
	tst.w	d6
	bne	.cN\@
	add.w	#$80,d1			; screen = $80 + mag
	bra	.cS\@
.cN\@:
	move.w	#$80,d0
	sub.w	d1,d0			; screen = $80 - mag (16-bit)
	move.w	d0,d1
.cS\@:
	move.b	d1,(\3,a0)		; out_lo = screen low
	lsr.w	#8,d1
	move.b	d1,(\4,a0)		; out_hi = screen high
	bra	.done\@
.c0\@:
	; count == 0 : mag = (((q>>1)+q+1)>>2) & $7F ; out_lo = $80 +/- mag ; out_hi = 0
	move.w	d0,d1
	lsr.w	#1,d1
	add.w	d0,d1			; (q>>1)+q
	addq.w	#1,d1
	lsr.w	#2,d1
	and.w	#$7F,d1			; d1 = mag (7-bit)
	tst.w	d6
	bne	.c0N\@
	add.w	#$80,d1
	bra	.c0S\@
.c0N\@:
	move.w	#$80,d0
	sub.w	d1,d0
	move.w	d0,d1
.c0S\@:
	move.b	d1,(\3,a0)		; out_lo
	clr.b	(\4,a0)			; out_hi = 0
.done\@:
	endm

project_terrain_points_core:
project_terrain_points_core_asm:
	movem.l	d2-d7,-(sp)		; 6 longs = 24 bytes; arg X now at 7+24 = 31(sp)
	moveq	#0,d7
	move.b	31(sp),d7		; X (int-promoted byte)
	lea	mem,a0
	adda.l	d7,a0			; a0 = mem + X (all per-object cells are (disp16,a0))

	or.b	#$10,($24B4,a0)		; mark object projected this frame

	PAXIS	$22A4,$22D2,$2400,$242D	; screen X
	PAXIS	$235B,$2388,$245A,$2487	; screen Y

	; --- band scroll offset -> add (signed) into screen Y ---
	move.b	($242D,a0),d0		; x_hi
	btst	#7,d0
	bne	.offL
	tst.b	d0
	bne	.offR
	; on-screen band: off = mem[$270E + (mem[$2400+X] >> 3)]
	moveq	#0,d1
	move.b	($2400,a0),d1
	lsr.w	#3,d1			; band index (0..31)
	lea	mem+$270E,a1
	move.b	(a1,d1.w),d1		; off
	bra	.haveOff
.offL:
	move.b	mem+$270E,d1		; X off left edge
	bra	.haveOff
.offR:
	move.b	mem+$272D,d1		; X off right edge
.haveOff:
	and.w	#$FF,d1			; off in 0..255 (clear stray upper bits)
	moveq	#0,d0
	move.b	($245A,a0),d0
	add.w	d1,d0			; y_lo = mem[$245A+X] + off  (bit8 = carry)
	move.b	d0,($245A,a0)		; store low
	lsr.w	#8,d0			; d0 = carry (0/1)
	move.b	($2487,a0),d2		; current y_hi
	add.b	d0,d2			; += carry
	btst	#7,d1			; off & 0x80 ?
	beq	.noSext
	subq.b	#1,d2			; += 0xFF  (sign-extend of off)
.noSext:
	move.b	d2,($2487,a0)

	movem.l	(sp)+,d2-d7
	rts
