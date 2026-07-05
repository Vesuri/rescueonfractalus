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
; ($00A0:$00A1 / $00A2:$00A3), via four signed 8x16 products:
;   $22A3:$22D1 =  smul(ts, mcA) - smul(vp, mcB)          (lo:hi)
;   $22FF:$232D = -(smul(ts, mcB) + smul(vp, mcA))        (lo:hi)
; where ts = terrain_state $89, vp = vbi_phase $87 (unsigned 8-bit multipliers),
;       mcA = (int16)($A1<<8 | $A0), mcB = (int16)($A3<<8 | $A2) (signed multiplicands).
;
; signed_mul_8x16 ($9C97) looks bit-serial on the 6502, but its product core is a PLAIN
; UNSIGNED 8x16 multiply P = m * |mc| (m = 0..255, |mc| = 0..$8000 both fit a word) with the
; multiplicand's sign re-applied to the 16-bit result P>>8.  So each of the four products is
; exactly ONE `mulu.w` on the magnitudes — no loop, no lookup, no per-byte shift-add (contrast
; mul_u8 $9821, a round-half-up multiply needing g_mulTable).  smul(m, mc) = trunc(m*mc / 256)
; (round toward zero): q = (m * |mc|) >> 8, negated when mc < 0.
;
; The two multiplicands' magnitudes + signs are formed ONCE (the only byte-wise work — the
; 6502 stores the words little-endian, so a big-endian word read would byte-swap them), then
; held in registers across all four products:
;   d0 = |mcA|   d1 = |mcB| (then reused for the mul4 product)   d2 = ts (then frac)
;   d3 = vp      d4 = signA (0/1)   d5 = signB (0/1)   d6/d7 = product / carry scratch
;
; signed_mul_8x16 memory side effects reproduced for the LAST (mul4 = vp*mcA) call only
; (earlier calls' $00A8-$00AD are overwritten): $00AA/$00AB = |mcA|, $00A8/$00A9 = product,
; $00AC = frac = ((P4 & $7F)<<1) | ac_bit0, $00AD = mul4 sign byte (= scroll_accum_b0 $A1).
; ac_bit0 is 0 when mcA is negative, else the 6502 "no-borrow" carry of the step-2 subtract
; (captured with `scc`), matching the 6502 carry the last two muls see.

	xdef	build_view_transform_matrix_asm
	ifnd	ROF_BUILDVIEW_VERIFY
	xdef	build_view_transform_matrix		; ships as the core symbol directly
	endif
	xref	mem

	section	code

; --- one signed 8x16 product -> prod16 (trunc(m*mc/256)) in \4.
;   \1 = multiplier reg (0..255)   \2 = |multiplicand| reg   \3 = sign reg (0/1)   \4 = dest
PROD	macro
	move.w	\1,\4
	mulu.w	\2,\4			; \4 = m * |mc|  (24-bit product P)
	lsr.l	#8,\4			; P >> 8  (fits a word, non-negative)
	tst.b	\3
	beq	.pp\@
	neg.w	\4			; re-apply multiplicand sign
.pp\@:
	endm

build_view_transform_matrix:
build_view_transform_matrix_asm:
	movem.l	d2-d7,-(sp)
	lea	mem,a0

	; --- |mcA| + signA   (mcA = int16 of $A1:$A0, little-endian in mem) ---
	moveq	#0,d0
	move.b	($A1,a0),d0
	lsl.w	#8,d0
	move.b	($A0,a0),d0		; d0.w = mcA
	moveq	#0,d4			; signA = 0
	tst.w	d0
	bpl	.aA
	neg.w	d0			; d0 = |mcA|
	moveq	#1,d4			; signA = 1
.aA:
	; --- |mcB| + signB   (mcB = int16 of $A3:$A2) ---
	moveq	#0,d1
	move.b	($A3,a0),d1
	lsl.w	#8,d1
	move.b	($A2,a0),d1		; d1.w = mcB
	moveq	#0,d5			; signB = 0
	tst.w	d1
	bpl	.aB
	neg.w	d1			; d1 = |mcB|
	moveq	#1,d5			; signB = 1
.aB:
	; --- multipliers ---
	moveq	#0,d2
	move.b	($89,a0),d2		; ts = terrain_state
	moveq	#0,d3
	move.b	($87,a0),d3		; vp = vbi_phase

	; --- $22A3:$22D1 = (ts*mcA) - (vp*mcB) ; capture no-borrow carry for mul4 frac ---
	PROD	d2,d0,d4,d6		; d6 = prod1 = ts*mcA
	PROD	d3,d1,d5,d7		; d7 = prod2 = vp*mcB
	sub.w	d7,d6			; d6 = prod1 - prod2 ; C = borrow
	scc	d7
	and.w	#1,d7			; d7 = 6502 no-borrow carry (ac_bit0 base for mul4)
	move.b	d6,($22A3,a0)		; lo
	lsr.w	#8,d6
	move.b	d6,($22D1,a0)		; hi

	; --- prod3 = ts*mcB ---
	PROD	d2,d1,d5,d6		; d6 = prod3

	; --- mul4 = vp*mcA : keep the full product for frac + ZP side effects ---
	move.w	d3,d1
	mulu.w	d0,d1			; d1 = P4 = vp * |mcA|  (24-bit)
	; frac = ((P4 & $7F) << 1) | ac_bit0   (ac_bit0 = signA ? 0 : step-2 carry)
	move.w	d1,d2
	and.w	#$7F,d2
	add.w	d2,d2
	tst.b	d4			; signA
	bne	.nobit
	or.w	d7,d2
.nobit:
	; prod4 = P4>>8, sign signA
	lsr.l	#8,d1
	tst.b	d4
	beq	.p4
	neg.w	d1
.p4:
	; --- $22FF:$232D = -(prod3 + prod4) ---
	add.w	d1,d6			; d6 = prod3 + prod4   (d1 = prod4 preserved)
	neg.w	d6
	move.b	d6,($22FF,a0)		; lo
	lsr.w	#8,d6
	move.b	d6,($232D,a0)		; hi

	; --- mul4 signed_mul_8x16 ZP side effects ---
	move.b	d1,($A8,a0)		; product lo
	move.w	d1,d6
	lsr.w	#8,d6
	move.b	d6,($A9,a0)		; product hi
	move.b	d0,($AA,a0)		; |mcA| lo
	move.w	d0,d6
	lsr.w	#8,d6
	move.b	d6,($AB,a0)		; |mcA| hi
	move.b	d2,($AC,a0)		; frac
	move.b	($A1,a0),($AD,a0)	; sign byte = scroll_accum_b0 (original mcA hi)

	movem.l	(sp)+,d2-d7
	rts
