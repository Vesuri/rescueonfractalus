; TerrainFrameSetupAssembler.s — hand-written m68k twin of terrain_frame_setup_core
; (asm-migration-plan Phase 3).  Plain C linkage:  void terrain_frame_setup_core(void)
; (the two projection calls stay in the C terrain_frame_setup wrapper; this is just the
; two per-frame transform loops).  Byte-identical to terrain_frame_setup_core_c
; (rof_native.c, the SDL/validate oracle), verified by the in-process differential
; (make VERIFY=1 PROBES=1 + tfsetup_verify.gdb).
;
; The win over GCC: loop 1 touches ~15 Y-indexed arrays spanning ~530 bytes ($2276..$24E7).
; GCC can only reach them with 8-bit displacements, so it juggles several base registers and
; emits `lea disp8(base),aN` + `move (aN)` per access.  Here a0 = mem+Y and every array is a
; single `move.b (d16,a0)` (signed 16-bit displacement covers the whole cluster).  All loop
; INPUTS ($92/$8A/$88/$A0-$A3/$8B/$8C/$22A3.. vectors/$0900 heights) are VBI-stable.
;
; ⭐ The per-cell column vectors are CARRIED IN REGISTERS across cells (2026-08-05).  The output
; arrays ARE the input arrays shifted by one — $22A4+Y == $22A3+(Y+1) and $232E+Y == $232D+(Y+1) —
; so cell Y+1's in_u/in_v is exactly the u/v cell Y just stored, and the per-cell reassembly of it
; from memory (4 indexed byte loads + 2 `lsl.w #8` at 22 cycles each + 2 moveq ~= 100 cycles) was
; pure redundancy: representation, not work.  Proven twice before it was written: statically, only
; 1 of the 180 pattern-table cells is non-storing ($B622[0]) and at runtime the recurrence held on
; 18072/18072 claimable cells (make TFSETUP_C=1 TFS_SHAPE=1, amiga/shape_probe.gdb, g_tfsRecurBad).
; The one non-storing cell re-syncs from memory at tf_resync, so this is exact for ANY table.
; Same class of find as the rasterizer's phase-2 restructure — see docs/asm-migration-plan.md.
;
; Loop-1 registers:
;   a0 = mem + Y (walk +1)   a1 = mem + srcptr + Y (pattern table, walk +1)
;   a2 = v   (the CARRIED depth vector; adda/suba.w, low word only)
;   a3 = rot_a   a4 = rot_b   (16-bit; used as add.w/sub.w aN,dN)
;   a5 = mem + $0900 (height base, indexed by X)   a6 = mem + $2D (loop-1 end sentinel)
;   d1 = u   (the CARRIED screen-X vector)
;   d7 = X   d6 = b6   d5 = SUB   d4 = K   d2 = b5 (running)   d0/d3 = the ONLY scratch
;
; d5/d4 are the HOISTED forms of the screen-Y numerator's two alt terms (both loop-invariant —
; they derive only from alt_b $8B / alt_c $8C, which neither loop writes):
;   SUB = alt_c + (alt_b != 0)             so  diff = h - SUB   is one word subtract
;   K   = ((uint8)(0 - alt_b)) >> 4        so  b5 = (y_hi << 4) | K
; and the 16-bit pair shift `pair = ((y_hi<<8)|y_lo) >> 4` collapses to two BYTE shifts:
; b5 = (uint8)pair = (y_hi<<4)|(y_lo>>4) and r_hi = pair>>8 = y_hi>>4.  That drops three
; `lsl/lsr.w #8` (22 cycles each on a 68000) per cell.  Note `sub.b`-derived borrows: the
; classify's 16-bit negate is likewise byte-wise (neg.b + negx.b) instead of building a word.
;
; Branch sizing: the amiga Makefile assembles with `-no-opt` (load-bearing), so a bare Bcc is
; always `.w` = 12 cycles when NOT taken vs 8 for `.s`.  Every branch below that is in range is
; hand-marked `.s` (taken cost is 10 either way, so this is free).  See the vasm-no-opt note in
; docs/asm-migration-plan.md / the rasterizer twin.

	xdef	terrain_frame_setup_core_asm
	ifnd	ROF_TFSETUP_VERIFY
	xdef	terrain_frame_setup_core		; ships as the core symbol directly
	endif
	xref	mem

	section	code

terrain_frame_setup_core:
terrain_frame_setup_core_asm:
	movem.l	d2-d7/a2-a6,-(sp)	; 11 longs = 44 bytes

	; --- src table + col-0 seed from draw_row ($92) ---
	moveq	#0,d0
	move.b	mem+$92,d0		; dr
	btst	#7,d0
	bne.s	tf_drhi
	btst	#6,d0
	bne.s	tf_dr01
	; dr==00: seed col-0 outputs, src = $B622
	move.b	mem+$22A3,mem+$22A4
	move.b	mem+$22D1,mem+$22D2
	move.b	mem+$22FF,mem+$2300
	move.b	mem+$232D,mem+$232E
	lea	mem+$B622,a1
	move.b	#$22,mem+$80
	move.b	#$B6,mem+$81
	bra.s	tf_srcdone
tf_dr01:
	lea	mem+$B64F,a1
	move.b	#$4F,mem+$80
	move.b	#$B6,mem+$81
	bra.s	tf_srcdone
tf_drhi:
	btst	#6,d0
	bne.s	tf_dr11
	lea	mem+$B5F5,a1		; dr==10
	move.b	#$F5,mem+$80
	move.b	#$B5,mem+$81
	bra.s	tf_srcdone
tf_dr11:
	lea	mem+$B5C8,a1
	move.b	#$C8,mem+$80
	move.b	#$B5,mem+$81
tf_srcdone:

	; b6 = (mem[$8A] & $0F) << 4
	moveq	#0,d6
	move.b	mem+$8A,d6
	and.b	#$0F,d6
	lsl.b	#4,d6
	; X = mem[$88] & $0F
	moveq	#0,d7
	move.b	mem+$88,d7
	and.b	#$0F,d7
	; mem[$B4] = X | mem[$B5]  (reads OLD $B5, before the loop overwrites it)
	move.b	mem+$B5,d0
	or.b	d7,d0
	move.b	d0,mem+$B4
	; rot_a = (mem[$A1]<<8)|mem[$A0] -> a3 ; rot_b = (mem[$A3]<<8)|mem[$A2] -> a4
	moveq	#0,d0
	move.b	mem+$A1,d0
	lsl.w	#8,d0
	move.b	mem+$A0,d0
	movea.w	d0,a3
	moveq	#0,d0
	move.b	mem+$A3,d0
	lsl.w	#8,d0
	move.b	mem+$A2,d0
	movea.w	d0,a4
	; hoisted numerator constants: d5 = SUB = alt_c + (alt_b != 0) ; d4 = K = (uint8)(-alt_b) >> 4
	moveq	#0,d5
	move.b	mem+$8C,d5		; alt_c
	moveq	#0,d4
	move.b	mem+$8B,d4		; alt_b (sets Z)
	beq.s	tf_noborrow
	addq.w	#1,d5			; the 6502's `- 1` borrow when alt_b != 0
tf_noborrow:
	neg.b	d4			; y_lo = (uint8)(0 - alt_b)
	lsr.b	#4,d4			; K = y_lo >> 4
	; bases + running pointers
	lea	mem+$0900,a5
	lea	mem+$2D,a6
	lea	mem,a0			; a0 = mem + Y (Y=0)

	; ---- seed the CARRIED column vectors from cell 0's inputs (see the recurrence note) ----
	moveq	#0,d1
	move.b	(0+$22D1,a0),d1
	lsl.w	#8,d1
	move.b	(0+$22A3,a0),d1		; u = in_u[0]  -> carried in d1
	moveq	#0,d0
	move.b	(0+$232D,a0),d0
	lsl.w	#8,d0
	move.b	(0+$22FF,a0),d0
	movea.w	d0,a2			; v = in_v[0]  -> carried in a2

	; ================= LOOP 1 (45 cells) =================
tf_l1:
	move.b	(a1),d0			; pat
	; ---- pattern decode.  u is carried in d1 and v in a2 from the PREVIOUS cell (the
	; recurrence: this cell's in_u/in_v IS the vector the previous cell stored), so there is
	; nothing to load or byte-assemble here.  v lives in an address register because that is
	; the only register left, and adda/suba.w serve it exactly as add/sub.w serve d1; only the
	; low word is ever read back, so their sign-extension to 32 bits is harmless. ----
	;
	; ⭐ JUMP TABLE, not the oracle's nested btst chain (2026-08-07).  The decode is a pure
	; function of pat's TOP NIBBLE — the low nibble is never tested on any path, and every
	; branch outcome collapses to two independent signs:
	;       u += sA*rot_a + sB*rot_b        v += -sA*rot_b + sB*rot_a
	;       b6 += (sA>0) ? $F0 : (sA<0) ? $10 : 0            X += sB
	; with (sA,sB) read straight off the nibble (n=0 is the empty pattern -> resync):
	;   n: 0 1  2  3  4  5  6  7  8 9 A  B  C  D  E  F
	;  sA: - 0  0  0 -1 -1 -1 -1 +1 +1 +1 +1 +1 +1 +1 +1
	;  sB: - +1 -1 -1  0 +1 -1 -1  0  0 +1 +1 -1 -1 -1 -1
	; (Derived from the oracle branch-for-branch — note n=9 and n=3 ignore bit4/bit5 exactly as
	; the chain does, because the chain stops testing once its branch is decided.)
	;
	; So `pat & $F0` IS the table index already, and one `and.w` + one `jmp` replaces 2-4
	; btst/beq pairs at 18-20 cycles each.  Blocks are 32 bytes (hence the `add.w d0,d0`) so the
	; longest body — a primary AND a secondary step, 14 bytes — fits with a uniform `bra.w`
	; exit; the `dcb.b` after each block pads it out and would fail to assemble if one grew too
	; big.  The table must sit here rather than out of line: `jmp (d8,PC,Dn.W)` has only an
	; 8-bit displacement, so tf_jt has to be within 127 bytes of the jmp (the INDEX is added
	; afterwards and is unbounded).
	;
	; Cost, weighted by the real pattern tables (all four are pure top-nibble, low nibble
	; always 0; the dr==00 table $B622 is used on 408/420 frames and its 45 cells are
	; n=8 x12, n=1 x12, n=2 x8, n=4 x8, n=6 x2, n=A x2, n=0 x1):
	;   chain 4008 cycles/pass -> table 2484 = -1524, i.e. 89 -> 55 cycles/cell.
	; x45 cells x2 calls/iteration = ~3.0k cycles/it = ~6.8 beam ticks of the SETUP phase's
	; 168 t/it (~4% of the function, ~0.2% of flight wall clock).
	and.w	#$F0,d0			; index = top nibble * 16 (low nibble is a don't-care)
	add.w	d0,d0			; * 2 -> 32-byte blocks
	jmp	tf_jt(pc,d0.w)

tf_jt:
tf_n0:	; ---- $0x: no bit set — empty pattern, leave the column unchanged ----
	bra.w	tf_resync
	dcb.b	tf_jt+32*1-*,0
tf_n1:	; ---- $1x: sB=+1 ----
	addq.b	#1,d7			; X++
	add.w	a4,d1			; u += rot_b
	adda.w	a3,a2			; v += rot_a
	bra.w	tf_fired
	dcb.b	tf_jt+32*2-*,0
tf_n2:	; ---- $2x: sB=-1 ----
	subq.b	#1,d7			; X--
	sub.w	a4,d1			; u -= rot_b
	suba.w	a3,a2			; v -= rot_a
	bra.w	tf_fired
	dcb.b	tf_jt+32*3-*,0
tf_n3:	; ---- $3x: sB=-1 (bit4 never tested once bit5 decided it) ----
	subq.b	#1,d7
	sub.w	a4,d1
	suba.w	a3,a2
	bra.w	tf_fired
	dcb.b	tf_jt+32*4-*,0
tf_n4:	; ---- $4x: sA=-1 ----
	add.b	#$10,d6			; b6 += $10
	sub.w	a3,d1			; u -= rot_a
	adda.w	a4,a2			; v += rot_b
	bra.w	tf_fired
	dcb.b	tf_jt+32*5-*,0
tf_n5:	; ---- $5x: sA=-1, sB=+1 ----
	add.b	#$10,d6
	sub.w	a3,d1
	adda.w	a4,a2
	addq.b	#1,d7
	add.w	a4,d1
	adda.w	a3,a2
	bra.w	tf_fired
	dcb.b	tf_jt+32*6-*,0
tf_n6:	; ---- $6x: sA=-1, sB=-1 ----
	add.b	#$10,d6
	sub.w	a3,d1
	adda.w	a4,a2
	subq.b	#1,d7
	sub.w	a4,d1
	suba.w	a3,a2
	bra.w	tf_fired
	dcb.b	tf_jt+32*7-*,0
tf_n7:	; ---- $7x: sA=-1, sB=-1 ----
	add.b	#$10,d6
	sub.w	a3,d1
	adda.w	a4,a2
	subq.b	#1,d7
	sub.w	a4,d1
	suba.w	a3,a2
	bra.w	tf_fired
	dcb.b	tf_jt+32*8-*,0
tf_n8:	; ---- $8x: sA=+1 ----
	add.b	#$F0,d6			; b6 += $F0
	add.w	a3,d1			; u += rot_a
	suba.w	a4,a2			; v -= rot_b
	bra.w	tf_fired
	dcb.b	tf_jt+32*9-*,0
tf_n9:	; ---- $9x: sA=+1 (the bit7 branch never tests bit4) ----
	add.b	#$F0,d6
	add.w	a3,d1
	suba.w	a4,a2
	bra.w	tf_fired
	dcb.b	tf_jt+32*10-*,0
tf_nA:	; ---- $Ax: sA=+1, sB=+1 ----
	add.b	#$F0,d6
	add.w	a3,d1
	suba.w	a4,a2
	addq.b	#1,d7
	add.w	a4,d1
	adda.w	a3,a2
	bra.w	tf_fired
	dcb.b	tf_jt+32*11-*,0
tf_nB:	; ---- $Bx: sA=+1, sB=+1 ----
	add.b	#$F0,d6
	add.w	a3,d1
	suba.w	a4,a2
	addq.b	#1,d7
	add.w	a4,d1
	adda.w	a3,a2
	bra.w	tf_fired
	dcb.b	tf_jt+32*12-*,0
tf_nC:	; ---- $Cx: sA=+1, sB=-1 ----
	add.b	#$F0,d6
	add.w	a3,d1
	suba.w	a4,a2
	subq.b	#1,d7
	sub.w	a4,d1
	suba.w	a3,a2
	bra.w	tf_fired
	dcb.b	tf_jt+32*13-*,0
tf_nD:	; ---- $Dx: sA=+1, sB=-1 ----
	add.b	#$F0,d6
	add.w	a3,d1
	suba.w	a4,a2
	subq.b	#1,d7
	sub.w	a4,d1
	suba.w	a3,a2
	bra.w	tf_fired
	dcb.b	tf_jt+32*14-*,0
tf_nE:	; ---- $Ex: sA=+1, sB=-1 ----
	add.b	#$F0,d6
	add.w	a3,d1
	suba.w	a4,a2
	subq.b	#1,d7
	sub.w	a4,d1
	suba.w	a3,a2
	bra.w	tf_fired
	dcb.b	tf_jt+32*15-*,0
tf_nF:	; ---- $Fx: sA=+1, sB=-1 ----
	add.b	#$F0,d6
	add.w	a3,d1
	suba.w	a4,a2
	subq.b	#1,d7
	sub.w	a4,d1
	suba.w	a3,a2
	bra.w	tf_fired
	dcb.b	tf_jt+32*16-*,0
tf_fired:
	; Store u/v, keeping BOTH carried registers intact for the next cell (so the hi byte goes
	; out through the d0 scratch instead of rotating d1/a2 in place: +4 cycles a cell against
	; the ~100 the removed per-cell reassembly cost).
	move.b	d1,(0+$22A4,a0)		; u lo
	move.w	d1,d0
	lsr.w	#8,d0
	move.b	d0,(0+$22D2,a0)		; u hi
	move.w	a2,d0
	move.b	d0,(0+$2300,a0)		; v lo
	lsr.w	#8,d0
	move.b	d0,(0+$232E,a0)		; v hi
	bra.s	tf_afterstore
tf_resync:
	; Empty pattern: the 6502 leaves this column's output slot UNTOUCHED, so the next cell's
	; input is whatever memory holds there (a seed or last frame's value) — NOT our carried
	; register.  Re-sync from that slot so the recurrence stays exact for any pattern table.
	; Measured: exactly 1 of the 180 table cells is empty ($B622[0], and the dr==00 branch
	; pre-seeds column 0 precisely because of it), so this runs ~once a frame.
	moveq	#0,d1
	move.b	(0+$22D2,a0),d1
	lsl.w	#8,d1
	move.b	(0+$22A4,a0),d1		; u = mem[{$22D2:$22A4}+Y]
	moveq	#0,d0
	move.b	(0+$232E,a0),d0
	lsl.w	#8,d0
	move.b	(0+$2300,a0),d0
	movea.w	d0,a2			; v = mem[{$232E:$2300}+Y]
tf_afterstore:
	; ---- screen-Y numerator ----  (d1/a2 are OFF LIMITS below: they carry u/v.  The diff
	; scratch moved from d1 to d0, and the classify's u_hi/lo scratch from d1 to d0/d3.)
	and.b	#$0F,d7			; X = (X & $0F) | b6   (d7's upper word stays 0)
	or.b	d6,d7
	move.b	(a5,d7.w),d3		; h = mem[$0900 + X]
	move.b	d3,(0+$23B5,a0)		; mem[$23B5+Y] = h
	; diff = h - SUB  (SUB = alt_c + borrow, hoisted)   -> d0, signed word
	moveq	#0,d0
	move.b	d3,d0			; h
	sub.w	d5,d0
	; b5 = (y_hi << 4) | K     (== (uint8)pair; y_hi = diff's low byte, K = y_lo>>4)
	move.b	d0,d2
	lsl.b	#4,d2
	or.b	d4,d2			; b5 (kept in d2)
	move.b	d2,(0+$235B,a0)		; mem[$235B+Y] = b5
	; r_hi = y_hi >> 4         (== pair >> 8), high nibble flipped on borrow
	move.b	d0,d3
	lsr.b	#4,d3
	btst	#15,d0			; diff < 0 ?
	beq.s	tf_nosign
	eori.b	#$F0,d3			; r_hi ^= $F0
tf_nosign:
	move.b	d3,(0+$2388,a0)		; mem[$2388+Y] = r_hi
	move.b	d7,(0+$2276,a0)		; mem[$2276+Y] = X
	; ---- visibility classify -> $24B4+Y ----
	move.b	(0+$232E,a0),d3		; v_hi (move.b sets N/Z -> no separate btst/tst needed)
	bmi.s	tf_cls80		; v_hi & $80
	bne.s	tf_vismain		; v_hi != 0
	move.b	(0+$2300,a0),d0		; v_hi==0: check v_lo < $20
	cmp.b	#$20,d0
	bcs.s	tf_cls80
tf_vismain:
	move.b	(0+$22D2,a0),d0		; u_hi
	bmi.s	tf_visneg
	; positive u
	cmp.b	d3,d0			; u_hi - v_hi
	bcs.s	tf_cls00
	bne.s	tf_cls20
	move.b	(0+$22A4,a0),d0		; u_hi==v_hi: u_lo < v_lo ?
	move.b	(0+$2300,a0),d3
	cmp.b	d3,d0
	bcs.s	tf_cls00
	bra.s	tf_cls20
tf_visneg:
	; nu = (uint16)(0 - u) byte-wise: neg the lo half, negx the hi half (d0 already = u_hi).
	; u_lo is loaded straight into d2 (the b5 slot — the oracle leaves nu_lo in $B5), which
	; also drops the original's separate `move.b d0,d2` copy.  Nothing between neg and negx
	; touches X.
	move.b	(0+$22A4,a0),d2		; u_lo
	neg.b	d2			; nu_lo  (X = borrow) ; b5 = (uint8)nu
	negx.b	d0			; nu_hi
	cmp.b	d3,d0			; nu_hi - v_hi
	bcs.s	tf_cls00
	bne.s	tf_cls40
	move.b	(0+$2300,a0),d3		; v_lo
	cmp.b	d3,d2			; nu_lo - v_lo
	bcs.s	tf_cls00
	bra.s	tf_cls40
tf_cls80:
	move.b	#$80,(0+$24B4,a0)
	bra.s	tf_iterend
tf_cls00:
	clr.b	(0+$24B4,a0)
	bra.s	tf_iterend
tf_cls20:
	move.b	#$20,(0+$24B4,a0)
	bra.s	tf_iterend
tf_cls40:
	move.b	#$40,(0+$24B4,a0)
tf_iterend:
	addq.l	#1,a0
	addq.l	#1,a1
	cmpa.l	a6,a0
	bne	tf_l1

	; loop-1 residue: $B5 = last b5 (d2), $B6 = b6 (d6)
	move.b	d2,mem+$B5
	move.b	d6,mem+$B6

	; ================= LOOP 2 (object collapse) =================
	moveq	#0,d0			; y2 = 0
	lea	mem+$B67C,a2		; draw-order base
	lea	mem+$24B4,a3		; class base
	lea	mem+$232E,a4		; v_hi base
	lea	mem+$2300,a5		; v_lo base
tf_l2:
	moveq	#0,d1
	move.b	(a2,d0.w),d1		; obj = mem[$B67C + y2]
	move.b	d1,mem+$28DB		; mem[$28DB] = obj
	addq.b	#1,d0			; y2++
	move.b	(a3,d1.w),d2		; cls = mem[$24B4 + obj]  (sets N/Z)
	bmi.s	tf_l2_chk		; cls & $80 -> check_following
	bne.s	tf_l2_extra		; cls != 0 -> extra_step
	; fully visible
	moveq	#0,d3
	move.b	(a2,d0.w),d3		; nxt
	addq.b	#1,d0			; y2++
	move.b	(a3,d3.w),d1		; mem[$24B4 + nxt]
	bpl.s	tf_l2_tail		; next not off-screen -> done
	; hide next
	move.b	d0,mem+$B5		; $B5 = y2
	clr.b	(a4,d3.w)		; $232E+nxt = 0
	move.b	#$20,(a5,d3.w)		; $2300+nxt = $20
	clr.b	(a3,d3.w)		; $24B4+nxt = 0
	move.b	mem+$B5,d0		; y2 = mem[$B5]  (sets Z)
	bne.s	tf_l2_tail
	; y2==0 -> check_following
tf_l2_chk:
	moveq	#0,d3
	move.b	(a2,d0.w),d3		; nxt
	addq.b	#1,d0			; y2++
	move.b	(a3,d3.w),d1		; mem[$24B4 + nxt]  (sets Z)
	bne.s	tf_l2_tail		; next not visible -> done
	; hide current
	move.b	d0,mem+$B5		; $B5 = y2
	moveq	#0,d3
	move.b	mem+$28DB,d3		; prev
	clr.b	(a4,d3.w)		; $232E+prev = 0
	move.b	#$20,(a5,d3.w)		; $2300+prev = $20
	clr.b	(a3,d3.w)		; $24B4+prev = 0
	move.b	mem+$B5,d0		; y2 = mem[$B5]  (sets Z)
	bne.s	tf_l2_tail
	; y2==0 -> extra_step
tf_l2_extra:
	addq.b	#1,d0			; y2++
tf_l2_tail:
	cmp.b	#$0C,d0
	bcs.s	tf_l2			; y2 < $0C -> loop
	movem.l	(sp)+,d2-d7/a2-a6
	rts
