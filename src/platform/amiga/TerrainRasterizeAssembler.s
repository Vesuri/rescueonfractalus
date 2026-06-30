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
	xdef	flight_edge_plot_asm
	xref	mem
	xref	kRow120
	xref	kHeightRowOff
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
	move.l	d2,d6			; col (already zero-extended)
	add.w	d0,d6			; col + ccol
	lsr.w	#1,d6			; d6 = mid
	move.l	d4,d7			; frac (already zero-extended)
	moveq	#0,d1
	move.b	2(a3),d1		; cfrac
	add.w	d1,d7
	addq.w	#1,d7			; d7 = fsum (bit8 = carry)
	move.l	d3,d1			; height (already zero-extended)
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
	sub.b	d2,d0			; (ccol - col) low byte  (col == mid now; upper stays 0)
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
	sub.b	d2,d0			; (mid - col) low byte  (upper stays 0)
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
	move.l	d5,d6			; plotCol (already zero-extended)
	add.w	d0,d6			; plotCol + ccol
	lsr.w	#1,d6			; d6 = mid
	move.b	d6,3(a3)		; cp[d+1].col = mid
	move.l	d4,d7			; frac (already zero-extended)
	moveq	#0,d1
	move.b	2(a3),d1		; cfrac
	add.w	d1,d7
	addq.w	#1,d7			; d7 = fsum
	move.b	d7,5(a3)		; cp[d+1].frac
	move.l	d3,d1			; height (already zero-extended)
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
	sub.b	d2,d6			; mid - col   (col == plotCol == d2; upper stays 0)
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
	sub.b	d2,d6			; mid - col   (upper stays 0)
	subq.b	#1,d6			; mid - col - 1
	lsr.w	#1,d6			; disp
	not.b	d6			; ~disp = (uint8_t)~disp (upper already 0)
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
	; Clobbers d0/d1/d6/d7.  Reads d5=plotCol (clean, zero-extended), a2=COL_MAX,
	; a5=dotplane, a6=kRow120.  _h (d0) arrives zero-extended from every caller.
draw:
	moveq	#0,d6
	move.b	(a2,d5.w),d6		; oldMax = COL_MAX(plotCol)
	cmp.b	d6,d0			; _h - oldMax
	bls	draw_ret		; _h <= oldMax -> hidden, nothing
	move.b	d0,(a2,d5.w)		; COL_MAX(plotCol) = _h
	cmp.b	#$97,d0
	bcs	draw_dot		; _h < $97
	move.b	#$FF,(a2,d5.w)		; saturate: full column
draw_dot:
	; ROF_PLOT_DOT(plotCol, oldMax) — uses oldMax (the PREVIOUS top), not _h
	move.l	a5,d7
	beq	draw_ret		; g_flightDotPlane null -> skip (cheap An-zero test)
	move.w	d5,d7			; plotCol
	sub.w	#48,d7			; _ac = plotCol - 48
	cmp.w	#160,d7
	bcc	draw_ret		; (unsigned) _ac >= 160 -> off viewport
	move.w	#150,d0			; (upper word irrelevant — all .w below)
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
	or.b	d6,(a5,d0.w)		; g_flightDotPlane[off] |= mask
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

; ---------------------------------------------------------------------------
; flight_edge_plot_asm(uint8_t* bp) — RescueOnFractalus::renderFlightDirect's
; plane-1 skyline edge plot: one bit per column at its skyline scanline, 160
; columns = 40 plane-1 bytes (4 cols/byte).  Structured after the user's
; hypothetical-renderer asm: 4 columns unrolled with IMMEDIATE column masks
; ($C0/$30/$0C/$03 = kColMask4), the plane-1 byte pointer (a2) walked +1 per 4
; columns (no c>>2), and the height->row-byte-offset folded through the
; kHeightRowOff[256] table = kRow120[clamp(150-h,0,42)] (no per-column 150-h /
; clamp branches).  The one residual per-column branch is h==$FF (off-top: the
; column is all terrain body, so it must plot NOTHING — no safe table sentinel
; without an extra buffer row).  Reads heights from mem[$260E+48..].  d0's high
; byte stays 0 (only move.b writes it; the *2 index is taken in d1), so no
; per-column re-clear is needed.
;   a0 = height source (mem+$260E+48)   a1 = kHeightRowOff
;   a2 = plane-1 byte base (bp), advanced +1 per 4-column group
flight_edge_plot_asm:
	movem.l	d7/a2,-(sp)		; 2 callee-saved longs = 8 bytes; arg shifts +8
	movea.l	12(sp),a2		; bp  (4 + 8)
	lea	mem+$260E+48,a0		; per-column max-height ($260E[48..])
	lea	kHeightRowOff,a1
	moveq	#0,d0			; d0 high stays 0 for the whole loop
	move.w	#40-1,d7
ep_loop:
	move.b	(a0)+,d0		; col 4k
	cmp.b	#$FF,d0
	beq.s	ep_c1
	move.w	d0,d1
	add.w	d1,d1
	move.w	(a1,d1.w),d1		; kHeightRowOff[h] = scanline byte offset
	or.b	#$C0,(a2,d1.w)
ep_c1:
	move.b	(a0)+,d0		; col 4k+1
	cmp.b	#$FF,d0
	beq.s	ep_c2
	move.w	d0,d1
	add.w	d1,d1
	move.w	(a1,d1.w),d1
	or.b	#$30,(a2,d1.w)
ep_c2:
	move.b	(a0)+,d0		; col 4k+2
	cmp.b	#$FF,d0
	beq.s	ep_c3
	move.w	d0,d1
	add.w	d1,d1
	move.w	(a1,d1.w),d1
	or.b	#$0C,(a2,d1.w)
ep_c3:
	move.b	(a0)+,d0		; col 4k+3
	cmp.b	#$FF,d0
	beq.s	ep_next
	move.w	d0,d1
	add.w	d1,d1
	move.w	(a1,d1.w),d1
	or.b	#$03,(a2,d1.w)
ep_next:
	addq.l	#1,a2			; next 4-column plane-1 byte
	dbra	d7,ep_loop
	movem.l	(sp)+,d7/a2
	rts
