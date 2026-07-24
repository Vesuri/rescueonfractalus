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
; The row base addresses are read from the $073D/$0793 table only for the FIRST two rows;
; thereafter the base is stepped by the constant stride = base[r0+1]-base[r0].  This is
; byte-identical because build_row_addr_table always builds the table arithmetically
; (entry[i+1]=entry[i]+stride, never modified), so base[row]==base[r0]+(row-r0)*stride
; exactly — it just drops the two per-row volatile table reads.
;
; Every effective address is computed as (base + off) & $FFFF and indexed through a
; zero-extended .l displacement, so writes always land inside mem[0..$FFFF] — faithful to
; the 6502 ($80),Y 64K wrap and to the C oracle's mem[(uint16_t)(base+off)].  (The earlier
; `.w`-indexed row-pointer march SIGN-EXTENDED base, so a base >= $8000 — or a marched
; pointer — wrote BELOW mem[] into .text/.rodata: the game-over wild-write.  Legit door/ring
; bases $1000-$2000 never wrap, so this stays byte-identical under framedraw_verify.)
;
; Registers (d2-d7/a2-a3 callee-saved at entry):
;   a0 = mem base   a2 = mem + eff (per-column pointer)
;   d7 = base (word VALUE, stepped by stride; upper word kept 0)
;   d6 = cnt (byte loop counter)   d5 = orMask   d4 = andMask
;   d3 = offL (colL>>1, word)   d2 = offR (colR>>1, word)   d0 = stride (word)   d1 = scratch

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

	; --- cnt = r1 - r0 (byte) ---
	moveq	#0,d7
	move.b	39(sp),d7		; r0 (row)
	moveq	#0,d6
	move.b	43(sp),d6		; r1
	sub.b	d7,d6			; d6.b = cnt = r1 - r0

	; --- base0 from table[r0], stride from table[r0+1]-table[r0] (arithmetic table) ---
	lea	($073D,a0),a2
	adda.w	d7,a2			; a2 = &row_base_lo[r0]
	moveq	#0,d7
	move.b	(a2),d7
	moveq	#0,d1
	move.b	($56,a2),d1		; row_base_hi[r0]   ($0793-$073D=$56)
	lsl.w	#8,d1
	or.w	d1,d7			; d7 = base0
	moveq	#0,d0
	move.b	(1,a2),d0		; row_base_lo[r0+1]
	moveq	#0,d1
	move.b	($57,a2),d1		; row_base_hi[r0+1]
	lsl.w	#8,d1
	or.w	d1,d0			; d0 = base1
	sub.w	d7,d0			; d0 = stride = base1 - base0 (d7 still = base0, upper word 0)

.loop:
	; left column: eff = (base + offL) & $FFFF
	moveq	#0,d1
	move.w	d7,d1
	add.w	d3,d1			; (base + offL) mod $10000 (carry discarded; upper word stays 0)
	lea	(a0,d1.l),a2		; a2 = mem + eff  (0..$FFFF -> always in the 64K window)
	move.b	(a2),d1
	or.b	d5,d1
	and.b	d4,d1
	move.b	d1,(a2)
	; right column: eff = (base + offR) & $FFFF
	moveq	#0,d1
	move.w	d7,d1
	add.w	d2,d1			; (base + offR) mod $10000
	lea	(a0,d1.l),a2
	move.b	(a2),d1
	or.b	d5,d1
	and.b	d4,d1
	move.b	d1,(a2)

	add.w	d0,d7			; base = (base + stride) mod $10000 (low word; upper stays 0)
	subq.b	#1,d6			; cnt--  (N set when (cnt-1) bit7 set)
	bpl	.loop			; loop while result >= 0 (bit7 clear)

	movem.l	(sp)+,d2-d7/a2-a3
	rts

; --- fill_horizontal_span_core(uint16_t base1, uint16_t base2, uint8_t y_hi, uint8_t cnt,
;                               uint8_t pat) --------------------------------------------------
; Fill the uniform pattern byte `pat` across the horizontal run on two scanlines: base1+y and
; base2+y for y = y_hi down to y_hi-cnt (cnt+1 bytes, exactly the C loop's descending walk).
; Each byte address is computed as (base + y) & $FFFF with y a wrapping uint8 (y = y_hi
; down), matching the C's mem[(uint16_t)(base+y)] with y = (uint8_t)(y-1) exactly — so every
; write lands inside mem[0..$FFFF] (no pre-decrement pointer that could drift below mem[] or
; a `.w`-indexed base >= $8000 sign-extending outside it).  Byte-identical to
; fill_horizontal_span_core_c (same subq.b/bpl loop control, so every cnt value — incl. the
; pathological >=$81 -> 1 byte — matches; the y-underflow-past-0 case now matches the C too).
;
; Args after saving d2-d4 (12 bytes): base1 word 18(sp), base2 word 22(sp), y_hi 27(sp),
; cnt 31(sp), pat 35(sp).  a0=mem  a1=byte pointer  d0=pat  d1=count  d2=eff scratch
;   d3=y (uint8, descends)  d4=base (word value)

	xdef	fill_horizontal_span_core_asm
	ifnd	ROF_FRAMEDRAW_VERIFY
	xdef	fill_horizontal_span_core
	endif

fill_horizontal_span_core:
fill_horizontal_span_core_asm:
	movem.l	d2-d4,-(sp)		; 12 bytes; args at +12
	lea	mem,a0
	moveq	#0,d0
	move.b	35(sp),d0		; pat

	; run 1: base1 + y for y = y_hi down, cnt+1 bytes (each addr wrapped to 16 bits)
	move.w	18(sp),d4		; base1
	moveq	#0,d3
	move.b	27(sp),d3		; y = y_hi
	moveq	#0,d1
	move.b	31(sp),d1		; working count = cnt
.h1:
	moveq	#0,d2
	move.b	d3,d2			; y (uint8)
	add.w	d4,d2			; (base1 + y) mod $10000 (carry discarded; upper word 0)
	lea	(a0,d2.l),a1		; a1 = mem + eff  (0..$FFFF -> in window)
	move.b	d0,(a1)
	subq.b	#1,d3			; y = (y-1) & $FF
	subq.b	#1,d1			; count--
	bpl	.h1

	; run 2: base2 + y (same y range)
	move.w	22(sp),d4		; base2
	moveq	#0,d3
	move.b	27(sp),d3		; y = y_hi
	moveq	#0,d1
	move.b	31(sp),d1		; working count = cnt
.h2:
	moveq	#0,d2
	move.b	d3,d2
	add.w	d4,d2			; (base2 + y) mod $10000
	lea	(a0,d2.l),a1
	move.b	d0,(a1)
	subq.b	#1,d3
	subq.b	#1,d1
	bpl	.h2

	movem.l	(sp)+,d2-d4
	rts
