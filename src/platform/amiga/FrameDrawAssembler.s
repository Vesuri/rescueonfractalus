; FrameDrawAssembler.s — hand-written m68k twin of fill_vertical_span_core
; (asm-migration-plan; standby->doors door-frame draw).  Plain C linkage:
;   void fill_vertical_span_core(uint8_t r0, uint8_t r1, uint8_t colL, uint8_t colR, uint8_t maskSel)
; GCC m68k passes the 5 int-promoted bytes ON THE STACK; each is the low byte of a
; 4-byte int, so before any push their bytes are at 7/11/15/19/23(sp).
;
; Byte-identical to the C oracle (fill_vertical_span_core_c in rof_native.c) — verified by
; the in-process differential (make VERIFY=1 PROBES=1 + framedraw_verify.gdb): each call runs
; this twin + the C oracle back-to-back on the same field state and compares the $1000 door
; field window.
;
; Draws the two vertical edges of one concentric door-frame rectangle: down rows r0..r1
; (cnt+1 = r1-r0+1 rows) it masked-plots the byte columns colL>>1 and colR>>1 into the
; $1000 screen field, RMW = (byte | orMask) & andMask.  orMask/andMask come from the
; $66E9/$66FB tables at index (colL&1 ? maskSel+9 : maskSel) — the SAME index for both
; columns (faithful to the C, which derives x from colL only).  Row base addresses come
; from the $073D(lo)/$0793(hi) per-scanline table (86 bytes apart), pointer-walked.
;
; Registers (d2-d7/a2-a3 callee-saved at entry):
;   a0 = mem base   a3 = &row_base_lo[row] (walked)   a2 = scratch RMW address
;   d6 = cnt (byte loop counter)   d5 = orMask   d4 = andMask
;   d3 = offL (colL>>1, word)   d2 = offR (colR>>1, word)   d0/d1 = scratch

	xdef	fill_vertical_span_core_asm
	ifnd	ROF_FRAMEDRAW_VERIFY
	xdef	fill_vertical_span_core		; ships as the core symbol directly
	endif
	xref	mem

	section	code

fill_vertical_span_core:
fill_vertical_span_core_asm:
	movem.l	d2-d7/a2-a3,-(sp)	; 8 longs = 32 bytes; args now at +32
	lea	mem,a0

	; --- mask index x = (colL & 1) ? maskSel+9 : maskSel ---
	moveq	#0,d1
	move.b	55(sp),d1		; maskSel
	moveq	#0,d3
	move.b	47(sp),d3		; colL
	btst	#0,d3
	beq	.evenL
	addi.b	#9,d1			; x = maskSel + 9
.evenL:
	; d1 = x (small, 0..15).  orMask = mem[$66E9+x], andMask = mem[$66FB+x]
	lea	(a0,d1.w),a2		; a2 = mem + x
	move.b	($66E9,a2),d5		; d5 = orMask
	move.b	($66FB,a2),d4		; d4 = andMask

	; --- offsets ---
	lsr.w	#1,d3			; d3 = offL = colL>>1
	moveq	#0,d2
	move.b	51(sp),d2		; colR
	lsr.w	#1,d2			; d2 = offR = colR>>1

	; --- cnt = r1 - r0 (byte); row pointer a3 = &row_base_lo[r0] ---
	moveq	#0,d7
	move.b	39(sp),d7		; r0 (row)
	moveq	#0,d6
	move.b	43(sp),d6		; r1
	sub.b	d7,d6			; d6.b = cnt = r1 - r0
	lea	($073D,a0),a3
	adda.w	d7,a3			; a3 = mem + $073D + r0

.loop:
	moveq	#0,d0
	move.b	(a3),d0			; base lo = row_base_lo[row]
	moveq	#0,d1
	move.b	($56,a3),d1		; base hi = row_base_hi[row]  ($0793-$073D=$56)
	lsl.w	#8,d1
	or.w	d1,d0			; d0 = base (16-bit)

	; left column: aL = base + offL
	move.w	d0,d1
	add.w	d3,d1
	lea	(a0,d1.w),a2
	move.b	(a2),d1
	or.b	d5,d1
	and.b	d4,d1
	move.b	d1,(a2)

	; right column: aR = base + offR
	move.w	d0,d1
	add.w	d2,d1
	lea	(a0,d1.w),a2
	move.b	(a2),d1
	or.b	d5,d1
	and.b	d4,d1
	move.b	d1,(a2)

	addq.l	#1,a3			; next row base entry
	subq.b	#1,d6			; cnt--  (N set when (cnt-1) bit7 set)
	bpl	.loop			; loop while result >= 0 (bit7 clear)

	movem.l	(sp)+,d2-d7/a2-a3
	rts
