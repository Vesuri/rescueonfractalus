; TerrainRasterizeAssembler.s — hand-written m68k twin of terrain_column_rasterize_core
; (asm-migration-plan Phase 2 / the 2026-08-05 phase-2 RESTRUCTURE).  Plain C linkage:
;   void terrain_column_rasterize_core(uint8_t entryDepth, uint8_t colBase)
; GCC m68k passes both args int-promoted ON THE STACK; at entry (before any push)
; entryDepth's byte is at 7(sp), colBase's at 11(sp), return address at 0(sp).
;
; This is the AMIGA path of the C oracle (terrain_column_rasterize_core_c in rof_native.c).
; The Amiga path differs from SDL/validate in two ways the C #ifdef's:
;   - ROF_FIELD_PLOT is a NO-OP (nothing reads the mode-D field on Amiga), so the
;     $80/$81 bitmap-row scratch is never touched.
;   - $B5 ("b5=depth"/disp), the $95/$EA/$F4 control-point STACK residue for depth>0,
;     and $60 are all dead after the call (CLAUDE.md VBI ZP audit + the subdivide caller
;     only reads $82/$84/$86 + the sub-point stacks back).  So the control-point stack
;     lives in a PRIVATE register-walked scratch buffer and only slot [0] is seeded from
;     mem[$95/$EA/$F4].  The sole live writeback is the running cursor -> $82/$84/$86.
; entryDepth is dead (the C assigns it to `depth` then overwrites with 0).
;
; ---------------------------------------------------------------------------------------
; STRUCTURE (2026-08-05).  Five changes, together ~2x fewer cycles than the previous asm.
; Proven byte-identical to the oracle over 1.6M randomised host cases (including a fully
; adversarial pass) by tools/ras_restructure_test.c, then on-target by the in-process
; differential (make VERIFY=1 PROBES=1 + amiga/raster_verify.gdb).
;
;  1. NO CONTROL-POINT COLUMNS.  The old loop head re-read cp[depth].col from the stack and
;     derived `gap = plotCol - ccol` every iteration, and the far path materialised
;     `mid = (plotCol+ccol)>>1` and stored it.  All of that is replaced by a tracked
;     SPAN = cp[depth].col - plotCol, kept in d2:
;         mid          = plotCol + (span>>1)      (never materialised)
;         child span   = span>>1
;         parent span, after the child's whole subtree = span - (span>>1)
;         disp (up)    = (span>>1)>>1
;         disp (down)  = ((span>>1)-1)>>1
;         gap==$FF <=> span==1 ;  gap==$FE <=> span==2
;     Identities: floor((a+b)/2) = a + floor((b-a)/2), and a finished subtree always leaves
;     plotCol exactly AT its control point's column.  Phase 1 tracks the same span (its
;     advance leaves span = span - (span>>1)).  Span is >= 1 throughout phase 2 (a push
;     needs span >= 3, so the parent's remainder is >= 2 and a pop lands >= 1); span == 0
;     is unreachable and would not terminate in the C either.
;  2. TOS CONTROL-POINT HEIGHT IN A REGISTER (d6).  The pushed midpoint height is simply
;     LEFT in d6 (never stored), the parent's is spilled on the way down, and the leaf
;     handlers read it for free.  Kills a store + 1-2 loads per node.
;  3. `r = hsum&1` FOLDED into ceil: havg + (hsum&1) == (hsum+1)>>1.  One register and two
;     instructions fewer on the roughness path.
;  4. THE `plotCol >= $D4` BOUND AND `col = plotCol` MOVED OUT OF THE LOOP HEAD into the
;     leaf handlers — the only places plotCol changes (the far path leaves it alone, so its
;     loop-top re-test was always redundant), plus one test at phase-2 entry.
;  5. SPANS 3 AND 4 ARE STRAIGHT-LINE BLOCKS (ras_sp3 / ras_sp4): no push, no pop, no
;     dispatch, no stack traffic — the whole 3- or 4-column leaf group inline.  Measured on
;     a real deep flight (6013 calls, 84842 far-bisects): span 3 = 31.2% and span 4 = 16.5%
;     of all far-bisects, and they subsume 98.6% of the ff leaves and 86% of the fe leaves.
;     Loop-top dispatches drop 174820 -> 93986 for the same picture.
;
; Register map (callee-saved d2-d7/a2-a6 saved at entry):
;   d0/d1/d7 = scratch (also the DRAW macro's scratch)
;   d2 = span (cp[depth].col - plotCol)   d3 = height (cursor)   d4 = frac (cursor)
;   d5 = plotCol   d6 = chgt (TOS control-point height)
;     (all six hold a BYTE with the upper bits kept clean, so .w arithmetic is safe)
;   a0 = col — the $82 writeback value; only the leaf handlers update it
;   a1 = free
;   a2 = mem+$260E  (COL_MAX per-column max-height base, indexed by plotCol)
;   a3 = current control-point slot ptr; slot = [postSpan, hgt, frac], walked +/-3
;   a4 = control-point stack base (for the depth==0 underflow test)
;   a5 = g_flightDotPlane (plane2 dot buffer; armed once at init -> never null in flight)
;   a6 = kDrawDotRowOff (oldMax -> plane2 row byte-offset, or $FFFF sentinel; word entries)
;   d5 doubles as phase-1 scratch (plotCol is not live until ph2_enter).

	xdef	terrain_column_rasterize_core_asm
	ifnd	ROF_RASTERIZE_VERIFY
	xdef	terrain_column_rasterize_core		; ships as the core symbol directly
	endif
	xdef	flight_edge_plot_asm
	xref	mem
	xref	kDrawDotRowOff
	xref	kHeightRowOff
	xref	g_flightDotPlane

CPBUF	equ	96		; 32 control-point slots * 3 bytes (depth stays < ~16)

	section	code

; ---------------------------------------------------------------------------
; DRAW(_h in d0) — keep the topmost height per column, lag-plot the dot.
; Inlined at every call site (11 of them): the old `bsr draw`/`rts` pair cost 34 cycles
; per DRAW, and DRAW runs once per terrain column (582/frame measured).
; Reads  d0 = _h (zero-extended), d5 = plotCol (clean), a2/a5/a6.
; Clobbers d0/d1/d7.  Leaves d2/d3/d4/d5/d6 and a0-a6 untouched.
;
; a6 = kDrawDotRowOff folds the whole oldMax gate: bails on oldMax FIRST (before any _sc
; arithmetic) via the $FFFF sentinel, and yields kRow120[150-oldMax] directly for the
; accepted rows.  a5 (g_flightDotPlane) is armed once at init and never null in flight ->
; no per-plot null test.  Range-check plotCol: reject the HIGH edge (>=208) BEFORE the sub,
; so an off-viewport column never pays for it; the LOW edge (<48) then falls out FREE as the
; borrow from that same sub.  Same accept set as the C oracle's (unsigned)(plotCol-48) < 160.
DRAWDOT	macro
	moveq	#0,d1
	move.b	(a2,d5.w),d1		; oldMax = COL_MAX(plotCol)
	cmp.b	d1,d0			; _h - oldMax
	bls.s	.dend\@			; _h <= oldMax -> hidden, nothing
	move.b	d0,(a2,d5.w)		; COL_MAX(plotCol) = _h
	cmp.b	#$97,d0
	bcs.s	.ddot\@			; _h < $97
	move.b	#$FF,(a2,d5.w)		; saturate: full column
.ddot\@:
	; ROF_PLOT_DOT(plotCol, oldMax) — uses oldMax (the PREVIOUS top), not _h.
	add.w	d1,d1			; oldMax * 2 (word index)
	move.w	(a6,d1.w),d1		; kDrawDotRowOff[oldMax], or $FFFF
	bmi.s	.dend\@			; sentinel -> off display / $6b reset-floor -> skip
	move.w	d5,d7			; plotCol
	cmp.w	#208,d7
	bcc.s	.dend\@			; plotCol >= 208 -> off viewport (skip the sub)
	sub.w	#48,d7			; _ac = plotCol - 48
	bcs.s	.dend\@			; plotCol < 48 -> borrow -> off viewport (free)
	move.w	d7,d0			; _ac  (d0/_h is dead from here)
	and.w	#3,d0
	add.w	d0,d0			; shift count = 2*(_ac&3)
	lsr.w	#2,d7			; _ac >> 2
	add.w	d7,d1			; byte offset = rowoff + (_ac>>2)
	move.w	#$C0,d7
	lsr.w	d0,d7			; mask = $C0 >> (2*(_ac&3))  ( = kColMask4[_ac&3] )
	or.b	d7,(a5,d1.w)		; g_flightDotPlane[off] |= mask
.dend\@:
	endm

; ---------------------------------------------------------------------------
; POP to the parent control point.  frac comes from the slot we are LEAVING (the oracle's
; `frac = CTL_FRAC(depth+1)` after `depth--`), span/chgt from the parent's slot.
RASPOP	macro
	move.b	2(a3),d4		; frac  = this leaf's fraction
	subq.l	#3,a3			; depth--
	move.b	(a3),d2			; span  = the parent's post-child span
	move.b	1(a3),d6		; chgt  = the parent's control-point height
	endm

; ---------------------------------------------------------------------------
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
	move.l	g_flightDotPlane,a5	; plane2 dot buffer (armed at init -> non-null in flight)
	lea	kDrawDotRowOff,a6	; oldMax -> plane2 row byte-offset
	; seed control-point slot [0] from mem[$EA]/[$F4]; its column becomes the span below
	moveq	#0,d6
	move.b	mem+$EA,d6		; chgt = CTL_HEIGHT(0)
	move.b	d6,1(a4)		; slot[0].hgt
	move.b	mem+$F4,2(a4)		; slot[0].frac
	; load running cursor height/frac from $84/$86 (zero-extended)
	moveq	#0,d3
	move.b	mem+$84,d3		; height
	moveq	#0,d4
	move.b	mem+$86,d4		; frac
	moveq	#0,d2			; span   (set below)
	moveq	#0,d5			; plotCol / phase-1 scratch

	; ---- setup: trivial-segment early-outs --------------------------------
	moveq	#0,d0
	move.b	mem+$95,d0		; endCol = CTL_COL(0)
	moveq	#0,d1
	move.b	mem+$82,d1		; col (the cursor / segment left end)
	move.w	d1,a0			; a0 = col
	cmp.b	#$2D,d0
	bcs	done			; endCol < $2D -> nothing on screen
	cmp.b	d1,d0			; endCol - col
	bcs	done			; endCol < col -> empty segment
	bne.s	ph1_init		; endCol > col -> phase 1
	; endCol == col: one column wide -> plot it and done
	move.l	d0,d5			; plotCol = endCol
	move.l	d6,d0			; _h = CTL_HEIGHT(0)
	DRAWDOT
	bra	done

	; ---- phase 1: left-clip ----------------------------------------------
	; depth = 0.  Bisect cursor->endpoint; advance the cursor onto the off-screen
	; midpoints, push the first in-view one, until col reaches $2C.  d7 = col,
	; d2 = span (= cp[depth].col - col), d5/d0/d1 = scratch.
ph1_init:
	sub.b	d1,d0			; endCol - col
	move.l	d0,d2			; span
	move.l	d1,d7			; d7 = col (phase-1 working cursor)
ph1_loop:
	cmp.b	#$2C,d7
	bcc	ph2_enter		; col >= $2C -> start filling
	move.w	d2,d0
	lsr.w	#1,d0			; d0 = child = span>>1  ( = mid - col )
	move.w	d7,d1
	add.w	d0,d1			; d1 = mid
	cmp.b	#$2C,d1
	bhi.s	ph1_push		; mid > $2C -> push midpoint as control point
	; --- advance: col = mid; span = span - child; frac = fsum&$FF; height per roughness
	move.l	d1,d7			; col = mid
	sub.b	d0,d2			; span = span - child  ( = cp.col - mid )
	moveq	#0,d1
	move.b	2(a3),d1		; cfrac
	add.w	d4,d1
	addq.w	#1,d1			; d1 = fsum (bit8 = carry)
	move.b	d1,d4			; frac = (uint8_t)fsum
	move.w	d6,d0
	add.w	d3,d0
	lsr.w	#1,d0			; d0 = havg = (height+chgt)>>1
	btst	#7,d1
	beq.s	ph1_adv_set		; no roughness: height = havg
	btst	#8,d1
	beq.s	ph1_adv_down
	move.w	d2,d1
	lsr.w	#1,d1			; disp = span>>1  (span == cp.col - mid now)
	add.w	d1,d0			; up: t = havg + disp
	cmp.w	#$FF,d0
	bls.s	ph1_adv_set
	move.w	#$FF,d0			; saturate $FF
	bra.s	ph1_adv_set
ph1_adv_down:
	move.w	d2,d1
	lsr.w	#1,d1			; disp
	cmp.w	d1,d0			; havg - disp
	bcs.s	ph1_adv_zero		; havg < disp -> floor 0
	sub.w	d1,d0
	bra.s	ph1_adv_set
ph1_adv_zero:
	moveq	#0,d0
ph1_adv_set:
	move.l	d0,d3			; height
	bra	ph1_loop
ph1_push:
	; push mid as control point depth+1 (d0 = child = mid - col, still the old col)
	move.l	d0,d5			; keep child
	moveq	#0,d1
	move.b	2(a3),d1		; cfrac
	add.w	d4,d1
	addq.w	#1,d1			; d1 = fsum
	move.b	d1,5(a3)		; slot[depth+1].frac = (uint8_t)fsum
	move.w	d6,d0
	add.w	d3,d0
	lsr.w	#1,d0			; d0 = havg
	btst	#7,d1
	beq.s	ph1_push_set		; no roughness: hgt = havg
	btst	#8,d1
	beq.s	ph1_push_down
	move.w	d5,d1
	lsr.w	#1,d1			; disp = child>>1  ( = (mid-col)>>1 )
	add.w	d1,d0
	cmp.w	#$FF,d0
	bls.s	ph1_push_set
	move.w	#$FF,d0
	bra.s	ph1_push_set
ph1_push_down:
	move.w	d5,d1
	lsr.w	#1,d1			; disp
	cmp.w	d1,d0
	bcs.s	ph1_push_zero
	sub.w	d1,d0
	bra.s	ph1_push_set
ph1_push_zero:
	moveq	#0,d0
ph1_push_set:
	sub.b	d5,d2			; this node's post-child span = span - child
	move.b	d2,(a3)			; spill it
	move.b	d6,1(a3)		; spill the parent's control-point height
	move.l	d5,d2			; span = child
	move.l	d0,d6			; chgt = mh
	addq.l	#3,a3			; depth++
	bra	ph1_loop

	; ---- phase 2: trace ---------------------------------------------------
ph2_enter:
	move.l	d7,d5			; plotCol = col
	move.w	d7,a0			; a0 = col
	cmp.b	#$D4,d5
	bcc	done
	; Dispatch on span.  Measured mix at the loop top (per frame, deep flight):
	; span>=5 47% / span==3 28% / span==4 15% / span==2 9% / span==1 0.4%, so the
	; generic far bisect FALLS THROUGH (one not-taken .s branch) and span 3 is next.
ph2_loop:
	cmp.b	#4,d2
	bls.s	ph2_small		; span <= 4 -> the specialised blocks

	; --- far: push an interpolated midpoint, descend --------------------------
ph2_far:
	move.w	d2,d0
	lsr.w	#1,d0			; d0 = child span
	sub.b	d0,d2			; d2 = this node's post-child span
	move.b	d2,(a3)			; spill it
	move.b	d6,1(a3)		; spill the parent's control-point height
	move.l	d0,d2			; span = child   (d0 keeps child for disp)
	moveq	#0,d1
	move.b	2(a3),d1		; cfrac
	add.w	d4,d1
	addq.w	#1,d1			; d1 = fsum (bit8 = carry)
	move.b	d1,5(a3)		; slot[depth+1].frac
	add.w	d3,d6			; d6 = hsum = chgt + height
	btst	#7,d1
	bne.s	ph2_far_rough
	lsr.w	#1,d6			; no roughness: chgt = havg = hsum>>1
ph2_far_next:
	addq.l	#3,a3			; depth++
	bra	ph2_loop
ph2_far_rough:
	addq.w	#1,d6
	lsr.w	#1,d6			; d6 = ceil(hsum/2) = havg + (hsum&1)
	btst	#8,d1
	beq.s	ph2_far_down
	lsr.w	#1,d0			; disp = child>>1
	add.w	d0,d6			; up: t = ceil + disp
	cmp.w	#$FF,d6
	bls.s	ph2_far_next
	move.w	#$FF,d6			; saturate $FF
	bra.s	ph2_far_next
ph2_far_down:
	subq.b	#1,d0			; child - 1   (span>=5 -> child>=2, no borrow)
	lsr.w	#1,d0			; disp
	not.b	d0			; ~disp (upper byte already 0)
	add.w	d0,d6			; t = ceil + ~disp
	cmp.w	#$FF,d6
	bhi.s	ph2_far_dmask		; t > $FF -> mh = t & $FF
	moveq	#0,d6			; else 0
	bra.s	ph2_far_next
ph2_far_dmask:
	and.w	#$FF,d6
	bra.s	ph2_far_next

	; --- small-span dispatch (span is 1..4 here) ------------------------------
ph2_small:
	cmp.b	#3,d2
	beq	ras_sp3
	bhi	ras_sp4			; span == 4
	cmp.b	#2,d2
	beq	ph2_fe
	; span == 1 -> fall through to ph2_ff

	; --- span 1 (the oracle's gap==$FF): plot the endpoint column, pop -------
ph2_ff:
	move.w	d5,a0			; col = plotCol
	move.l	d6,d3			; height = chgt
	move.l	d3,d0			; _h
	DRAWDOT
	addq.b	#1,d5			; plotCol++
	cmpa.l	a4,a3
	beq	done			; depth was 0 -> done
	RASPOP
	cmp.b	#$D4,d5
	bcc	done
	bra	ph2_loop

	; --- span 2 (the oracle's gap==$FE): interpolated column, endpoint, pop --
ph2_fe:
	move.w	d5,a0			; col = plotCol
	move.w	d6,d0
	add.w	d3,d0
	addq.w	#1,d0
	lsr.w	#1,d0			; (chgt + height + 1) >> 1
	DRAWDOT
	addq.b	#1,d5
	move.l	d6,d3			; height = chgt
	move.l	d3,d0
	DRAWDOT
	addq.b	#1,d5
	cmpa.l	a4,a3
	beq	done
	RASPOP
	cmp.b	#$D4,d5
	bcc	done
	bra	ph2_loop

	; --- span 3 = far(child span 1) + ff(child) + fe(parent), inline ---------
	; child span 1 -> disp is 0 in BOTH directions, so the midpoint is just
	; ceil(hsum/2) (up) / ceil(hsum/2)-1 floored at 0 (down) / hsum>>1 (flat), and
	; the up case cannot overflow (ceil(hsum/2) <= 255).  3 columns, one real pop.
ras_sp3:
	moveq	#0,d1
	move.b	2(a3),d1		; cfrac
	add.w	d4,d1
	addq.w	#1,d1			; d1 = fsum
	move.w	d6,d0
	add.w	d3,d0			; d0 = hsum
	btst	#7,d1
	bne.s	ras_sp3_rough
	lsr.w	#1,d0			; mh = hsum>>1
	bra.s	ras_sp3_go
ras_sp3_rough:
	addq.w	#1,d0
	lsr.w	#1,d0			; ceil(hsum/2)
	btst	#8,d1
	bne.s	ras_sp3_go		; up: mh = ceil (no clamp possible)
	subq.w	#1,d0			; down: ceil - 1 ...
	bcc.s	ras_sp3_go
	moveq	#0,d0			; ... floored at 0
ras_sp3_go:
	; child (span 1) ff: col = plotCol; height = mh; DRAW(mh); plotCol++; frac = fsum
	move.w	d5,a0
	move.b	d1,d4			; frac = (uint8_t)fsum  (before DRAWDOT clobbers d1)
	move.l	d0,d3			; height = mh
	DRAWDOT
	addq.b	#1,d5
	cmp.b	#$D4,d5
	bcc	done
	; parent (span 2) fe
	move.w	d5,a0
	move.w	d6,d0
	add.w	d3,d0
	addq.w	#1,d0
	lsr.w	#1,d0			; (chgt + mh + 1) >> 1
	DRAWDOT
	addq.b	#1,d5
	move.l	d6,d3			; height = chgt
	move.l	d3,d0
	DRAWDOT
	addq.b	#1,d5
	cmpa.l	a4,a3
	beq	done
	RASPOP
	cmp.b	#$D4,d5
	bcc	done
	bra	ph2_loop

	; --- span 4 = far(child span 2) + fe(child) + fe(parent), inline ---------
	; child span 2 -> disp_up = 2>>1 = 1, disp_down = (2-1)>>1 = 0.  4 columns, one
	; real pop.  d2 (span) is dead inside this block, so it carries mh across DRAWDOT.
ras_sp4:
	moveq	#0,d1
	move.b	2(a3),d1		; cfrac
	add.w	d4,d1
	addq.w	#1,d1			; d1 = fsum
	move.w	d6,d0
	add.w	d3,d0			; d0 = hsum
	btst	#7,d1
	bne.s	ras_sp4_rough
	lsr.w	#1,d0			; mh = hsum>>1
	bra.s	ras_sp4_go
ras_sp4_rough:
	addq.w	#1,d0
	lsr.w	#1,d0			; ceil(hsum/2)
	btst	#8,d1
	beq.s	ras_sp4_down
	addq.w	#1,d0			; up: ceil + 1
	cmp.w	#$FF,d0
	bls.s	ras_sp4_go
	move.w	#$FF,d0			; saturate $FF
	bra.s	ras_sp4_go
ras_sp4_down:
	subq.w	#1,d0			; down: ceil - 1 ...
	bcc.s	ras_sp4_go
	moveq	#0,d0			; ... floored at 0
ras_sp4_go:
	move.b	d1,d4			; frac = (uint8_t)fsum  (before DRAWDOT clobbers d1)
	move.l	d0,d2			; keep mh (span is dead in this block)
	; child (span 2) fe: col = plotCol; DRAW((mh+height+1)>>1); ++; height = mh; DRAW(mh); ++
	move.w	d5,a0
	add.w	d3,d0			; mh + height
	addq.w	#1,d0
	lsr.w	#1,d0
	DRAWDOT
	addq.b	#1,d5
	move.l	d2,d3			; height = mh
	move.l	d3,d0
	DRAWDOT
	addq.b	#1,d5
	cmp.b	#$D4,d5
	bcc	done
	; parent (span 2) fe
	move.w	d5,a0
	move.w	d6,d0
	add.w	d3,d0
	addq.w	#1,d0
	lsr.w	#1,d0
	DRAWDOT
	addq.b	#1,d5
	move.l	d6,d3			; height = chgt
	move.l	d3,d0
	DRAWDOT
	addq.b	#1,d5
	cmpa.l	a4,a3
	beq	done
	RASPOP
	cmp.b	#$D4,d5
	bcc	done
	bra	ph2_loop

	; ---- exit: write back the live cursor, free scratch, restore regs -------
done:
	move.l	a0,d0
	move.b	d0,mem+$82		; col
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
; kHeightRowOff[256] table = kRow120[clamp(150-h,0,46)] (no per-column 150-h /
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
