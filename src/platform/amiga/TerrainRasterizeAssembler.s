; TerrainRasterizeAssembler.s — hand-written m68k twin of terrain_column_rasterize_core
; (asm-migration-plan Phase 2).  Plain C linkage (our own function, no SAS/C wrapper):
;   void terrain_column_rasterize_core(uint8_t entryDepth, uint8_t colBase)
; GCC m68k passes both args int-promoted ON THE STACK; at entry (before any push)
; entryDepth's byte is at 7(sp), colBase's at 11(sp), return address at 0(sp).
;
; This is the AMIGA path of the C oracle (terrain_column_rasterize_core_c in
; rof_native.c) — verified byte-identical to it by headless render-diff of the
; terrainBitmap silhouette ($260E) + the plane2 dot buffer over the deterministic
; auto-flight.  The Amiga path differs from SDL/validate in two ways the C #ifdef's:
;   - ROF_FIELD_PLOT is a NO-OP (nothing reads the mode-D field on Amiga), so the
;     $80/$81 bitmap-row scratch is never touched.
;   - $B5 ("b5=depth"/disp), the $95/$EA/$F4 control-point STACK residue for depth>0,
;     and $60 are all dead after the call (CLAUDE.md VBI ZP audit + the subdivide
;     caller only reads $82/$84/$86 + the sub-point stacks back).  So the control-
;     point stack lives in a PRIVATE register-walked scratch buffer (the win GCC
;     could not realise: one base reg `a3` walked by +/-3, cheap displacement
;     addressing, no scaled index) and only slot [0] is seeded from mem[$95/$EA/$F4].
;     The sole live writeback is the running cursor -> $82(col)/$84(height)/$86(frac).
;
; Register map (callee-saved d2-d7/a2-a6 saved at entry):
;   d2=col  d3=height  d4=frac  d5=plotCol         (running state, low byte significant)
;   d0/d1/d6/d7 = scratch        a0/a1 = scratch
;   a2 = mem+$260E  (COL_MAX per-column max-height base, indexed by plotCol)
;   a3 = current control-point slot ptr (interleaved [col,hgt,frac], walked +/-3)
;   a4 = control-point stack base (for the depth==0 underflow test)
;   a5 = g_flightDotPlane (plane2 dot buffer; null on the first flight frame -> skip)
;   a6 = kRow120 (row -> byte-offset table, word entries)

	xdef	terrain_column_rasterize_core_asm
	ifnd	ROF_RASTERIZE_VERIFY
	xdef	terrain_column_rasterize_core		; ships as the core symbol directly
	endif
	xref	mem
	xref	kRow120
	xref	g_flightDotPlane

CPBUF	equ	96		; 32 control-point slots * 3 bytes (depth stays < ~12)

	section	code

terrain_column_rasterize_core:
terrain_column_rasterize_core_asm:
	movem.l	d2-d7/a2-a6,-(sp)	; 11 longs = 44 bytes; args now shift +44
	moveq	#0,d0
	move.b	55(sp),d0		; colBase  (11 + 44)
	move.b	d0,mem+$60		; mem[$60] = colBase   (faithful; dead on Amiga)
	lea	-CPBUF(sp),sp		; allocate the private control-point stack
	movea.l	sp,a4			; a4 = cp base
	movea.l	a4,a3			; a3 = current slot (depth 0)
	lea	mem+$260E,a2		; COL_MAX base
	move.l	g_flightDotPlane,a5	; plane2 dot buffer (may be 0)
	lea	kRow120,a6
	; seed control-point slot [0] from mem[$95]/[$EA]/[$F4]
	move.b	mem+$95,(a4)		; cp[0].col
	move.b	mem+$EA,1(a4)		; cp[0].hgt
	move.b	mem+$F4,2(a4)		; cp[0].frac
	; load running cursor col/height/frac from $82/$84/$86 (zero-extended)
	moveq	#0,d2
	move.b	mem+$82,d2		; col
	moveq	#0,d3
	move.b	mem+$84,d3		; height
	moveq	#0,d4
	move.b	mem+$86,d4		; frac
	moveq	#0,d5			; plotCol (set later)

	; ---- setup: trivial-segment early-outs --------------------------------
	moveq	#0,d0
	move.b	(a4),d0			; endCol = cp[0].col
	cmp.b	#$2D,d0
	bcs	done			; endCol < $2D -> nothing on screen
	cmp.b	d2,d0			; endCol - col
	bcs	done			; endCol < col -> empty segment
	bne	ph1_loop		; endCol > col -> phase 1
	; endCol == col: one column wide -> plot it and done
	move.l	d0,d5			; plotCol = endCol
	moveq	#0,d0
	move.b	1(a4),d0		; cp[0].hgt
	bsr	draw
	bra	done

	; ---- phase 1: left-clip ----------------------------------------------
	; depth = 0 (a3 == a4).  Bisect cursor->endpoint; advance the cursor onto
	; off-screen midpoints, push the first in-view one, until col reaches $2C.
ph1_loop:
	cmp.b	#$2C,d2
	bcc	ph2_enter		; col >= $2C -> start filling
	moveq	#0,d0
	move.b	(a3),d0			; ccol
	moveq	#0,d6
	move.b	d2,d6
	add.w	d0,d6			; col + ccol
	lsr.w	#1,d6			; d6 = mid
	moveq	#0,d7
	move.b	d4,d7			; frac
	moveq	#0,d1
	move.b	2(a3),d1		; cfrac
	add.w	d1,d7
	addq.w	#1,d7			; d7 = fsum (bit8 = carry)
	moveq	#0,d1
	move.b	d3,d1			; height
	moveq	#0,d0
	move.b	1(a3),d0		; chgt
	add.w	d0,d1
	lsr.w	#1,d1			; d1 = havg = (height+chgt)>>1
	cmp.b	#$2C,d6
	bhi	ph1_push		; mid > $2C -> push midpoint as control point
	; --- advance: col = mid; frac = fsum&0xFF ---
	move.b	d6,d2			; col = mid
	move.b	d7,d4			; frac = (uint8_t)fsum
	btst	#7,d4
	bne	ph1_adv_disp
	move.b	d1,d3			; no roughness: height = havg
	bra	ph1_loop
ph1_adv_disp:
	moveq	#0,d0
	move.b	(a3),d0			; ccol
	sub.b	d2,d0			; (ccol - col) low byte  (col == mid now)
	and.w	#$FF,d0
	lsr.w	#1,d0			; d0 = disp = (uint8_t)(ccol-col) >> 1
	btst	#8,d7
	beq	ph1_adv_down
	add.w	d0,d1			; up: t = havg + disp
	cmp.w	#$FF,d1
	bls	ph1_adv_setH
	move.w	#$FF,d1			; saturate $FF
ph1_adv_setH:
	move.b	d1,d3
	bra	ph1_loop
ph1_adv_down:
	cmp.w	d0,d1			; havg - disp
	bcs	ph1_adv_zero		; havg < disp -> floor 0
	sub.w	d0,d1
	move.b	d1,d3
	bra	ph1_loop
ph1_adv_zero:
	moveq	#0,d3
	bra	ph1_loop
ph1_push:
	; push midpoint as control point depth+1 (col still old in d2)
	move.b	d6,3(a3)		; cp[d+1].col = mid
	move.b	d7,5(a3)		; cp[d+1].frac = (uint8_t)fsum
	btst	#7,d7
	bne	ph1_push_disp
	move.b	d1,4(a3)		; no roughness: hgt = havg
	bra	ph1_push_adv
ph1_push_disp:
	moveq	#0,d0
	move.b	d6,d0			; mid
	sub.b	d2,d0			; (mid - col) low byte
	and.w	#$FF,d0
	lsr.w	#1,d0			; disp = (uint8_t)(mid-col) >> 1
	btst	#8,d7
	beq	ph1_push_down
	add.w	d0,d1			; up: t = havg + disp
	cmp.w	#$FF,d1
	bls	ph1_push_setH
	move.w	#$FF,d1
ph1_push_setH:
	move.b	d1,4(a3)
	bra	ph1_push_adv
ph1_push_down:
	cmp.w	d0,d1
	bcs	ph1_push_zero
	sub.w	d0,d1
	move.b	d1,4(a3)
	bra	ph1_push_adv
ph1_push_zero:
	clr.b	4(a3)
ph1_push_adv:
	addq.l	#3,a3			; depth++
	bra	ph1_loop

	; ---- phase 2: trace ---------------------------------------------------
ph2_enter:
	move.l	d2,d5			; plotCol = col
ph2_loop:
	cmp.b	#$D4,d5
	bcc	done			; plotCol >= $D4 -> done
	move.b	d5,d2			; col = plotCol (the displacement base)
	moveq	#0,d0
	move.b	(a3),d0			; ccol = cp[depth].col
	move.b	d5,d1
	sub.b	d0,d1			; gap = (uint8_t)(plotCol - ccol)
	cmp.b	#$FE,d1
	beq	ph2_fe
	cmp.b	#$FF,d1
	beq	ph2_ff
	; --- far: bisect, push interpolated midpoint (d0 = ccol still) ---
	moveq	#0,d6
	move.b	d5,d6
	add.w	d0,d6			; plotCol + ccol
	lsr.w	#1,d6			; d6 = mid
	move.b	d6,3(a3)		; cp[d+1].col = mid
	moveq	#0,d7
	move.b	d4,d7			; frac
	moveq	#0,d1
	move.b	2(a3),d1		; cfrac
	add.w	d1,d7
	addq.w	#1,d7			; d7 = fsum
	move.b	d7,5(a3)		; cp[d+1].frac
	moveq	#0,d1
	move.b	d3,d1			; height
	moveq	#0,d0
	move.b	1(a3),d0		; chgt
	add.w	d0,d1			; d1 = hsum
	move.w	d1,d0			; d0 = hsum (saved for &1 rounding)
	lsr.w	#1,d1			; d1 = havg
	and.w	#1,d0			; d0 = hsum & 1
	btst	#7,d7
	bne	ph2_far_disp
	move.b	d1,4(a3)		; no roughness: hgt = havg
	bra	ph2_far_adv
ph2_far_disp:
	btst	#8,d7
	beq	ph2_far_down
	; up: disp = (uint8_t)(mid - col) >> 1 ; t = havg + disp + (hsum&1)
	sub.b	d2,d6			; mid - col   (col == plotCol == d2)
	and.w	#$FF,d6
	lsr.w	#1,d6			; d6 = disp
	add.w	d6,d1			; havg + disp
	add.w	d0,d1			; + (hsum&1)
	cmp.w	#$FF,d1
	bls	ph2_far_up_set
	move.w	#$FF,d1
ph2_far_up_set:
	move.b	d1,4(a3)
	bra	ph2_far_adv
ph2_far_down:
	; down: disp = (uint8_t)(mid - col - 1) >> 1
	;       t = havg + (uint8_t)~disp + (hsum&1) ; mh = (t>0xFF)? t&0xFF : 0
	sub.b	d2,d6			; mid - col
	subq.b	#1,d6			; mid - col - 1
	and.w	#$FF,d6
	lsr.w	#1,d6			; disp
	not.b	d6			; ~disp
	and.w	#$FF,d6			; (uint8_t)~disp
	add.w	d6,d1			; havg + ~disp
	add.w	d0,d1			; + (hsum&1)
	cmp.w	#$FF,d1
	bhi	ph2_far_down_store	; t > 0xFF -> mh = t&0xFF (low byte)
	moveq	#0,d1			; else 0
ph2_far_down_store:
	move.b	d1,4(a3)
ph2_far_adv:
	addq.l	#3,a3			; depth++
	bra	ph2_loop
ph2_fe:
	; two columns short: interpolated column, then endpoint, then pop
	moveq	#0,d0
	move.b	1(a3),d0		; chgt = cp[depth].hgt
	moveq	#0,d1
	move.b	d3,d1			; height
	add.w	d1,d0
	addq.w	#1,d0
	lsr.w	#1,d0			; (chgt + height + 1) >> 1
	bsr	draw			; DRAW(interpolated)
	addq.b	#1,d5			; plotCol++
	move.b	1(a3),d3		; height = cp[depth].hgt
	moveq	#0,d0
	move.b	d3,d0
	bsr	draw			; DRAW(endpoint)
	addq.b	#1,d5			; plotCol++
	cmpa.l	a4,a3
	beq	done			; depth was 0 -> done
	subq.l	#3,a3			; pop
	move.b	5(a3),d4		; frac = popped slot's fraction
	bra	ph2_loop
ph2_ff:
	; one column short: plot endpoint, pop
	move.b	1(a3),d3		; height = cp[depth].hgt
	moveq	#0,d0
	move.b	d3,d0
	bsr	draw
	addq.b	#1,d5			; plotCol++
	cmpa.l	a4,a3
	beq	done
	subq.l	#3,a3
	move.b	5(a3),d4
	bra	ph2_loop

	; ---- DRAW(h in d0) : keep topmost height per column, lag-plot the dot ----
	; Clobbers d0/d1/d6/d7/a0.  Reads d5=plotCol, a2=COL_MAX, a5=dotplane, a6=kRow120.
draw:
	and.l	#$FF,d0			; _h
	moveq	#0,d1
	move.b	d5,d1			; plotCol (index)
	moveq	#0,d6
	move.b	(a2,d1.w),d6		; oldMax = COL_MAX(plotCol)
	cmp.b	d6,d0			; _h - oldMax
	bls	draw_ret		; _h <= oldMax -> hidden, nothing
	move.b	d0,(a2,d1.w)		; COL_MAX(plotCol) = _h
	cmp.b	#$97,d0
	bcs	draw_dot		; _h < $97
	move.b	#$FF,(a2,d1.w)		; saturate: full column
draw_dot:
	; ROF_PLOT_DOT(plotCol, oldMax) — uses oldMax (the PREVIOUS top), not _h
	move.l	a5,d7
	beq	draw_ret		; g_flightDotPlane null -> skip
	move.w	d1,d7			; plotCol
	sub.w	#48,d7			; _ac = plotCol - 48
	cmp.w	#160,d7
	bcc	draw_ret		; (unsigned) _ac >= 160 -> off viewport
	moveq	#0,d0
	move.w	#150,d0
	sub.w	d6,d0			; _sc = 150 - oldMax
	cmp.w	#43,d0
	bcc	draw_ret		; (unsigned) _sc >= 43 -> below viewport rows
	add.w	d0,d0			; _sc * 2 (word index)
	move.w	(a6,d0.w),d0		; kRow120[_sc]
	move.w	d7,d1
	lsr.w	#2,d1			; _ac >> 2
	add.w	d1,d0			; byte offset = kRow120[_sc] + (_ac>>2)
	move.w	d7,d1
	and.w	#3,d1			; _ac & 3
	add.w	d1,d1			; * 2
	move.w	#$C0,d6
	lsr.w	d1,d6			; mask = $C0 >> (2*(_ac&3))  ( = kColMask4[_ac&3] )
	movea.l	a5,a0
	or.b	d6,(a0,d0.w)		; g_flightDotPlane[off] |= mask
draw_ret:
	rts

	; ---- exit: write back the live cursor, free scratch, restore regs -------
done:
	move.b	d2,mem+$82		; col
	move.b	d3,mem+$84		; height
	move.b	d4,mem+$86		; frac
	lea	CPBUF(sp),sp		; free the control-point stack
	movem.l	(sp)+,d2-d7/a2-a6
	rts
