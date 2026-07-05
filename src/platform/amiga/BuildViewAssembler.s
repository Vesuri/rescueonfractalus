; BuildViewAssembler.s — hand-written m68k twin of build_view_transform_matrix_core
; (asm-migration-plan Phase 3).  Plain C linkage (our own function, no SAS/C wrapper):
;   void build_view_transform_matrix(void)          [memory-only contract]
;
; Byte-identical to the C oracle (build_view_transform_matrix_core_c in rof_native.c) —
; verified by the in-process differential (make VERIFY=1 PROBES=1 + buildview_verify.gdb):
; each call runs this twin + the C oracle back-to-back on the same inputs and compares the
; 10 cells it writes ($00A8-$00AD + $22A3/$22D1/$22FF/$232D).
;
; Computes two rotated view-space components from sin/cos $0089/$0087 and the 16-bit vector
; ($00A0:$00A1 / $00A2:$00A3), via four signed 8x16 multiplies:
;   $22A3:$22D1 =  ([A1:A0]*$89) - ([A3:A2]*$87)          (lo:hi)
;   $22FF:$232D = -(([A3:A2]*$89) + ([A1:A0]*$87))        (lo:hi)
;
; THE KEY WIN (this whole task): signed_mul_8x16 ($9C97) is a bit-serial 8-step shift-add on
; the 6502, but its product core is a plain UNSIGNED 8x16 multiply P = m * |multiplicand|
; (m = 0..255, |mc| = 0..$8000, both fit a 16-bit word), with the multiplicand's sign
; re-applied to the 16-bit result P>>8.  So ONE `mulu.w` per call is byte-exact — no loop,
; no lookup table (contrast mul_u8 $9821, a round-half-up multiply needing g_mulTable).
;
; signed_mul_8x16 memory side effects reproduced for the LAST (mul4) call only (earlier calls'
; $00A8-$00AD are overwritten): $00AA/$00AB = |mc4|, $00A8/$00A9 = product, $00AC = frac byte,
; $00AD = mul4 sign byte (= scroll_accum_b0).  frac = ((P&$7F)<<1) | ac_bit0, where ac_bit0 is
; 0 when the multiplicand is negative, else the 6502 carry threaded in — for mul4 that carry is
; the "no-borrow" result of the step-2 subtract ($22A3:$22D1 -= mul2 product).  mul1-3 frac is
; dead (overwritten) so their ac_bit0 is passed as 0.
;
; ZP source vars: $87 vbi_phase, $89 terrain_state, $A0 draw_iter_count, $A1 scroll_accum_b0,
;                 $A2 scroll_accum_b1, $A3 scroll_accum_b2.

	xdef	build_view_transform_matrix_asm
	ifnd	ROF_BUILDVIEW_VERIFY
	xdef	build_view_transform_matrix		; ships as the core symbol directly
	endif
	xref	mem

	section	code

; --- one signed 8x16 multiply.  Subroutine (4 call sites).
;   IN:  d0.w = m (0..255)          d1.b = multiplicand lo   d2.b = multiplicand hi (sign byte)
;        d6.w = ac_bit0 (0/1) to fold into frac when the multiplicand is non-negative
;   OUT: d3.w = prod16 (signed 16-bit result = P>>8, sign re-applied)
;        d4.w = |multiplicand|      d5.b = frac byte         d2 (sign byte) preserved
;   clobbers d0 (holds P on exit), d6
smul16:
	moveq	#0,d4
	move.b	d2,d4
	lsl.w	#8,d4
	move.b	d1,d4			; d4 = raw16 = (hi<<8)|lo
	btst	#7,d2
	beq	.pos
	neg.w	d4			; d4 = |multiplicand|
	moveq	#0,d6			; negative -> ac_bit0 = 0
.pos:
	mulu.w	d4,d0			; d0 = m * |mc|  (24-bit product P)
	move.w	d0,d5
	and.w	#$7F,d5
	add.w	d5,d5			; (P & $7F) << 1
	or.w	d6,d5			; | ac_bit0   (d5 low byte = frac)
	move.l	d0,d3
	lsr.l	#8,d3			; prod16 = P >> 8
	btst	#7,d2
	beq	.pos2
	neg.w	d3			; re-apply multiplicand sign
.pos2:
	rts

build_view_transform_matrix:
build_view_transform_matrix_asm:
	movem.l	d2-d7/a2,-(sp)
	lea	mem,a0

	; --- mul1: m=$89 terrain_state, mc = $A0:$A1 (draw_iter_count : scroll_accum_b0) ---
	moveq	#0,d0
	move.b	($89,a0),d0
	move.b	($A0,a0),d1
	move.b	($A1,a0),d2
	moveq	#0,d6			; frac dead
	bsr	smul16
	move.w	d3,d7			; save mul1 product

	; --- mul2: m=$87 vbi_phase, mc = $A2:$A3 (scroll_accum_b1 : scroll_accum_b2) ---
	moveq	#0,d0
	move.b	($87,a0),d0
	move.b	($A2,a0),d1
	move.b	($A3,a0),d2
	moveq	#0,d6			; frac dead
	bsr	smul16			; d3 = mul2 product

	; --- step 2: $22A3:$22D1 = mul1 - mul2 ; capture the 6502 carry (no-borrow) for mul4 ---
	sub.w	d3,d7			; d7 = mul1 - mul2 ; C = borrow
	scc	d0			; d0.b = $FF if carry clear (no borrow -> 6502 C=1)
	and.w	#1,d0
	movea.w	d0,a2			; a2 = ac_bit0 for mul4
	move.b	d7,($22A3,a0)		; lo
	move.w	d7,d0
	lsr.w	#8,d0
	move.b	d0,($22D1,a0)		; hi

	; --- mul3: m=$89 terrain_state, mc = $A2:$A3 ---
	moveq	#0,d0
	move.b	($89,a0),d0
	move.b	($A2,a0),d1
	move.b	($A3,a0),d2
	moveq	#0,d6			; frac dead
	bsr	smul16
	move.w	d3,d7			; save mul3 product

	; --- mul4: m=$87 vbi_phase, mc = $A0:$A1 ; ac_bit0 = step-2 carry ---
	moveq	#0,d0
	move.b	($87,a0),d0
	move.b	($A0,a0),d1
	move.b	($A1,a0),d2
	move.w	a2,d6			; ac_bit0 for mul4 frac
	bsr	smul16			; d3=prod16, d4=|mc4|, d5=frac, d2=sign byte

	; --- step 4: $22FF:$232D = -(mul3 + mul4) ---
	add.w	d3,d7			; d7 = mul3 + mul4  (d3 preserved for $A8/$A9)
	neg.w	d7
	move.b	d7,($22FF,a0)		; lo
	move.w	d7,d0
	lsr.w	#8,d0
	move.b	d0,($232D,a0)		; hi

	; --- mul4 signed_mul_8x16 ZP side effects ---
	move.b	d4,($AA,a0)		; |mc4| lo
	move.w	d4,d0
	lsr.w	#8,d0
	move.b	d0,($AB,a0)		; |mc4| hi
	move.b	d3,($A8,a0)		; product lo
	move.w	d3,d0
	lsr.w	#8,d0
	move.b	d0,($A9,a0)		; product hi
	move.b	d5,($AC,a0)		; frac
	move.b	d2,($AD,a0)		; sign byte (= scroll_accum_b0)

	movem.l	(sp)+,d2-d7/a2
	rts
