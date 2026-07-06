; MulTableAssembler.s — hand-written m68k twin of the g_mulTable build (rof_native.c
; mul_table_fill_c).  Plain C linkage, no args:  void mul_table_build_asm(void)
;
; Builds the 64KB byte-exact mul_u8 lookup table (g_mulTable[m*256+n] = mul_u8(m,n)).
; mul_u8 is a round-half-up-per-bit multiply with multiplicand-precision truncation — NOT a
; plain product (proven: no single mulu/muls reproduces it; differs from round(m*n/256) on
; ~1/3 of the domain), so each entry needs the bit-serial reference.  BUT mul_u8 is SYMMETRIC
; (mul_u8(m,n) == mul_u8(n,m), brute-force verified), so we compute only the lower triangle
; (n from m..255) and mirror each result into both [m][n] and [n][m] — ~32896 bit-serial
; passes instead of 65536 (~2x fewer).
;
; This runs ONCE at startup (rof_mul_table_init, called from main before the game loop) so the
; table is never built lazily inside the flight VBI ISR — that first-flight build froze the
; ISR (and the whole display) ~3.6s at flight entry.
;
; Byte-identical to mul_table_fill_c — verified by the one-shot differential (make VERIFY=1:
; rof_mul_table_init fills g_mulTableRef via the full C bit-serial, then this twin, then
; memcmps -> g_mulTableMismatch, read via amiga/multable_verify.gdb).
;
; bit-serial mul_u8(M,N) (matches mul_u8_bitserial exactly):
;   A=0; goto ck
;   ad: c=M&1; M>>=1; A = A + M + c          (round carry = pre-shift low bit of M)
;   ck: c=N>>7; N<<=1; if c goto ad
;       c=M&1; M>>=1; if M!=0 goto ck
;       return A
;
; Registers (all scratch restored by movem):
;   d0 = M scratch   d1 = N scratch   d2 = A / result
;   d4 = M counter (0..255)   d5 = N counter (m..255)   d6 = row offset (M*257)
;   a0 = g_mulTable base   a1 = row-M write ptr (+1 per n)   a2 = col-M write ptr (+256 per n)

	xdef	mul_table_build_asm
	xref	g_mulTable

	section	code

mul_table_build_asm:
	movem.l	d2/d4-d6/a2,-(sp)
	lea	g_mulTable,a0
	moveq	#0,d4			; M = 0
.mloop:
	move.l	d4,d6			; M
	lsl.l	#8,d6			; M<<8
	add.l	d4,d6			; d6 = M*256 + M = offset of [M][M]
	movea.l	a0,a1
	adda.l	d6,a1			; a1 = &g_mulTable[M][M]  (walks +1 along row M)
	movea.l	a0,a2
	adda.l	d6,a2			; a2 = &g_mulTable[M][M]  (walks +256 down col M)
	move.b	d4,d5			; N = M
.nloop:
	; --- mul_u8(M=d4.b, N=d5.b) -> d2.b ---
	move.b	d4,d0			; M scratch
	move.b	d5,d1			; N scratch
	moveq	#0,d2			; A = 0
	bra.s	.ck
.ad:
	lsr.b	#1,d0			; X = old M bit0, d0 = M>>1
	addx.b	d0,d2			; A = A + (M>>1) + old-bit0   (carry out dead)
.ck:
	add.b	d1,d1			; N <<= 1, X=C = old N bit7
	bcs.s	.ad			; if that bit set -> add step
	lsr.b	#1,d0			; M >>= 1
	bne.s	.ck			; loop while M != 0
	; --- store result to both mirror cells ---
	move.b	d2,(a1)+		; g_mulTable[M][N]
	move.b	d2,(a2)			; g_mulTable[N][M]
	adda.w	#256,a2			; next row down column M
	addq.b	#1,d5			; N++
	bne.s	.nloop			; until N wraps past 255
	addq.b	#1,d4			; M++
	bne.s	.mloop			; until M wraps past 255
	movem.l	(sp)+,d2/d4-d6/a2
	rts
