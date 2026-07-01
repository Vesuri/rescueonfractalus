; TerrainSubdivideAssembler.s — hand-written m68k twin of terrain_subdivide_column_core
; (asm-migration-plan Phase 3).  Plain C linkage (our own function, no SAS/C wrapper):
;   uint8_t terrain_subdivide_column_core(uint8_t startDepth, uint8_t rasterEntryDepth)
; GCC m68k passes both args int-promoted ON THE STACK; before any push startDepth's byte is
; at 7(sp), rasterEntryDepth's at 11(sp), return address at 0(sp).  Returns the final
; recursion depth in d0 (uint8_t).
;
; The coarse fractal-LOD pass over one terrain segment (see terrain_subdivide_column_core_c
; in rof_native.c — the SDL/validate oracle).  Byte-identical to it, verified by the
; in-process differential (make VERIFY=1 PROBES=1 + subdiv_verify.gdb): each call runs this
; twin + the C oracle back-to-back on the same inputs and compares the 5 SubPt stacks +
; the ZP residue ($60/$80-$91/$9F/$B5/$B6) + the return value.
;
; GCC compiled the C to 1173 insns with 129 frame-slot spills because the recursion's
; state (span, mid, far, depth, budget) exceeds what it will pin.  Here we pin all of it:
;   d2 = span.col   d3 = span.hgt   d4 = span.frac       (running segment near-endpoint)
;   d5 = mid.col    d6 = mid.hgt    d7 = mid.frac        (last computed midpoint)
;   d0/d1 = scratch (far endpoint load + arithmetic)
;   a1 = mem + depth   (single (d16,a1) covers every SubPt stack — the 68000 has no wide
;                       indexed mode, so we fold `depth` into the base instead of indexing)
;   a2 = depth (scalar, kept in lockstep with a1)   a3 = budget   a4 = rasterEntryDepth
; The 5 SubPt stacks + all ZP live in mem[] (main-RAM scratch the flight VBI never writes),
; addressed absolutely (mem+$xx) or via a1; a signed 16-bit displacement covers each base.

	xdef	terrain_subdivide_column_core_asm
	ifnd	ROF_SUBDIV_VERIFY
	xdef	terrain_subdivide_column_core		; ships as the core symbol directly
	endif
	xref	mem
	xref	terrain_column_rasterize_core

; SubPt stacks (parallel byte arrays, indexed by depth; a1 = mem+depth -> (base,a1))
SDCOL_LO	equ	$25B4
SDCOL_HI	equ	$25D2
SDHGT_LO	equ	$25F0
SDHGT_HI	equ	$24E2
SDFRAC		equ	$23E2

	section	code

terrain_subdivide_column_core:
terrain_subdivide_column_core_asm:
	movem.l	d2-d7/a2-a4,-(sp)	; 9 longs = 36 bytes; args shift +36
	moveq	#0,d0
	move.b	43(sp),d0		; startDepth (7 + 36)
	movea.l	d0,a2			; a2 = depth = startDepth
	lea	mem,a1
	adda.l	d0,a1			; a1 = mem + depth
	moveq	#0,d0
	move.b	47(sp),d0		; rasterEntryDepth (11 + 36)
	movea.l	d0,a4

	; span = {$82:$83 col, $84:$85 hgt, $86 frac}
	moveq	#0,d2
	move.b	mem+$83,d2
	lsl.w	#8,d2
	move.b	mem+$82,d2
	moveq	#0,d3
	move.b	mem+$85,d3
	lsl.w	#8,d3
	move.b	mem+$84,d3
	move.b	mem+$86,d4
	; mid = {$8D:$8E col, $8F:$90 hgt, $91 frac}
	moveq	#0,d5
	move.b	mem+$8E,d5
	lsl.w	#8,d5
	move.b	mem+$8D,d5
	moveq	#0,d6
	move.b	mem+$90,d6
	lsl.w	#8,d6
	move.b	mem+$8F,d6
	move.b	mem+$91,d7

	; --- entry guard: far0 = subpt_load(0); $B5 = (far0.col>>8)^$80; bail if span.col>=far0.col
	moveq	#0,d0
	move.b	mem+SDCOL_HI,d0		; far0.col hi (slot 0)
	move.b	d0,d1
	eori.b	#$80,d1
	move.b	d1,mem+$B5
	lsl.w	#8,d0
	move.b	mem+SDCOL_LO,d0		; d0 = far0.col
	cmp.w	d0,d2			; span.col - far0.col (signed)
	bge	sd_ret			; span.col >= far0.col -> return depth
	movea.w	#$14,a3			; budget = $14

	; ================= phase 2: descend =================
sd_phase2:
	btst	#15,d2			; span.col & $8000 ?
	beq	sd_phase3		; non-negative -> start leaf pass
	subq.l	#1,a3			; budget--
	cmpa.w	#-1,a3
	beq	sd_out			; budget was 0 -> exhausted
	bsr	submid			; mid = midpoint(span, far@depth)
	cmp.w	#$28,d5			; (int16)mid.col < $28 ?
	bge	sd_p2push
	move.w	d5,d2			; adopt mid as span (near midpoint)
	move.w	d6,d3
	move.b	d7,d4
	bra	sd_phase2
sd_p2push:
	bsr	push_mid		; store mid at depth+1
	addq.l	#1,a2			; depth++
	addq.l	#1,a1
	cmpa.w	#$0F,a2
	bcc	sd_out			; depth >= $0F -> stack full
	bra	sd_phase2

	; ================= phase 3: leaf + unwind =================
sd_phase3:
	move.w	d2,d0			; span.col > $FF ?
	and.w	#$FF00,d0
	bne	sd_out
	cmp.w	#$D8,d2			; span.col >= $D8 ? (unsigned; span.col is $00xx here)
	bcc	sd_out
sd_inner:
	bsr	load_far		; d0 = far.col, d1 = far.hgt
	cmp.w	#$FF,d0			; far.col > $FF ?
	bhi	sd_dosub		; -> subdivide
	; ---- CASCADE ----
	btst	#15,d3			; span.hgt & $8000 ?
	bne	sd_spanlow
	cmp.w	#$6C,d3			; span.hgt < $6C ?
	bcs	sd_spanlow
	; spanHIGH: default rasterize
	btst	#15,d1			; far.hgt & $8000 ?
	bne	sd_wtFarH		; -> width test (far height)
	cmp.w	#$FF,d1
	bhi	sd_doras		; far.hgt > $FF -> rasterize
	cmp.w	#$6C,d1
	bcs	sd_wtFarH		; far.hgt < $6C -> width test (far height)
	bra	sd_doras		; else rasterize
sd_spanlow:
	; spanLOW: default skip
	btst	#15,d1			; far.hgt & $8000 ?
	bne	sd_pop			; skip
	cmp.w	#$FF,d1
	bhi	sd_wtSpanH		; far.hgt > $FF -> width test (span height)
	cmp.w	#$6C,d1
	bcs	sd_pop			; far.hgt < $6C -> skip
	bra	sd_wtSpanH		; else width test (span height)

	; width/steepness: width = (far.col - span.col) low byte; narrow -> rasterize, else
	; rasterize if the chosen height is shallower than width/4, else subdivide (steep).
sd_wtSpanH:				; useSpanHeight = 1
	sub.w	d2,d0			; far.col - span.col (d0 was far.col)
	and.w	#$FF,d0			; width (byte)
	cmp.w	#$14,d0
	bcs	sd_doras		; width < $14 -> rasterize
	lsr.w	#2,d0			; q = width>>2
	move.b	d0,mem+$B5
	move.w	d3,d1			; hgt = span.hgt
	sub.w	d0,d1			; hgt - q
	btst	#15,d1
	beq	sd_doras		; shallow -> rasterize
	bra	sd_dosub		; steep -> subdivide
sd_wtFarH:				; useSpanHeight = 0 (hgt = far.hgt, still in d1)
	sub.w	d2,d0			; far.col - span.col
	and.w	#$FF,d0			; width
	cmp.w	#$14,d0
	bcs	sd_doras
	lsr.w	#2,d0			; q
	move.b	d0,mem+$B5
	sub.w	d0,d1			; far.hgt - q
	btst	#15,d1
	beq	sd_doras		; shallow -> rasterize
	bra	sd_dosub		; steep -> subdivide

sd_dosub:
	subq.l	#1,a3			; budget--
	cmpa.w	#-1,a3
	beq	sd_out
	bsr	submid			; mid = midpoint(span, far@depth)
	bsr	push_mid		; store mid at depth+1
	addq.l	#1,a2			; depth++
	addq.l	#1,a1
	cmpa.w	#$0F,a2
	bcc	sd_out			; depth >= $0F
	bra	sd_inner		; continue inner loop

sd_doras:
	; clamp span.hgt to a byte if >$FF (keep hi byte; lo = $00 neg / $FF pos)
	cmp.w	#$FF,d3
	bls	sd_r_noclamp
	btst	#15,d3
	bne	sd_r_neg
	or.w	#$00FF,d3
	bra	sd_r_noclamp
sd_r_neg:
	and.w	#$FF00,d3
sd_r_noclamp:
	; control point [0] from leaf = subpt_load(depth): $95=col lo, $F4=frac, $EA=clamp(hgt)
	move.b	(SDCOL_LO,a1),mem+$95
	move.b	(SDFRAC,a1),mem+$F4
	moveq	#0,d0
	move.b	(SDHGT_HI,a1),d0
	lsl.w	#8,d0
	move.b	(SDHGT_LO,a1),d0	; d0 = leaf.hgt
	moveq	#0,d1
	move.w	d0,d1			; d1 = leaf.hgt (upper word 0)
	cmp.w	#$FF,d1
	bls	sd_lh_store		; <= $FF -> use low byte
	btst	#15,d0
	bne	sd_lh_neg
	moveq	#-1,d1			; > $FF, positive -> $FF
	bra	sd_lh_store
sd_lh_neg:
	moveq	#0,d1			; > $FF, negative -> $00
sd_lh_store:
	move.b	d1,mem+$EA
	; flush span to $82-$86 (rasterizer reads/rewrites $82/$84/$86)
	move.b	d2,mem+$82
	move.l	d2,d0
	lsr.w	#8,d0
	move.b	d0,mem+$83
	move.b	d3,mem+$84
	move.l	d3,d0
	lsr.w	#8,d0
	move.b	d0,mem+$85
	move.b	d4,mem+$86
	; terrain_column_rasterize_core(rasterEntryDepth, depth)  (args pushed right-to-left)
	move.l	a2,d0			; depth (2nd arg)
	move.l	d0,-(sp)
	move.l	a4,d0			; rasterEntryDepth (1st arg)
	move.l	d0,-(sp)
	jsr	terrain_column_rasterize_core
	addq.l	#8,sp
	lea	mem,a1			; raster clobbers a1 -> recompute a1 = mem+depth
	adda.l	a2,a1
	; span.col/hgt low bytes reloaded from what the rasterizer wrote; frac too
	and.w	#$FF00,d2
	moveq	#0,d0
	move.b	mem+$82,d0
	or.w	d0,d2
	and.w	#$FF00,d3
	move.b	mem+$84,d0
	or.w	d0,d3
	move.b	mem+$86,d4
	; fall through to pop

sd_pop:
	cmpa.w	#0,a2			; depth == 0 ?
	beq	sd_out
	bsr	load_span		; span = subpt_load(depth)
	subq.l	#1,a2			; depth--
	subq.l	#1,a1
	bra	sd_phase3

	; ================= exit =================
sd_out:
	move.b	d2,mem+$82		; flush span
	move.l	d2,d0
	lsr.w	#8,d0
	move.b	d0,mem+$83
	move.b	d3,mem+$84
	move.l	d3,d0
	lsr.w	#8,d0
	move.b	d0,mem+$85
	move.b	d4,mem+$86
	move.b	d5,mem+$8D		; flush mid
	move.l	d5,d0
	lsr.w	#8,d0
	move.b	d0,mem+$8E
	move.b	d6,mem+$8F
	move.l	d6,d0
	lsr.w	#8,d0
	move.b	d0,mem+$90
	move.b	d7,mem+$91
	move.l	a3,d0			; $9F = budget (low byte; $FF if exhausted)
	move.b	d0,mem+$9F
sd_ret:
	move.l	a2,d0			; return depth
	movem.l	(sp)+,d2-d7/a2-a4
	rts

; ---------------------------------------------------------------------------
; submid — mid = subdiv_midpoint(span, far@depth).  Reads span (d2/d3/d4), loads far from
; (a1)[depth], writes mid (d5/d6/d7) and (on roughness) $B5/$B6.  Clobbers d0/d1.
; midCol = signed-avg = asr.w of (span.col+far.col+1); likewise midHgt; fracSum 9-bit.
submid:
	moveq	#0,d0
	move.b	(SDCOL_HI,a1),d0
	lsl.w	#8,d0
	move.b	(SDCOL_LO,a1),d0	; d0 = far.col
	move.w	d2,d5
	add.w	d0,d5
	addq.w	#1,d5
	asr.w	#1,d5			; d5 = mid.col
	moveq	#0,d0
	move.b	(SDHGT_HI,a1),d0
	lsl.w	#8,d0
	move.b	(SDHGT_LO,a1),d0	; d0 = far.hgt
	move.w	d3,d6
	add.w	d0,d6
	addq.w	#1,d6
	asr.w	#1,d6			; d6 = mid.hgt (pre-roughness)
	moveq	#0,d0
	move.b	(SDFRAC,a1),d0		; far.frac
	move.w	d4,d1
	and.w	#$FF,d1			; span.frac byte
	add.w	d0,d1
	addq.w	#1,d1			; d1 = fracSum (0..511)
	move.b	d1,d7			; mid.frac = low byte
	btst	#7,d7
	beq	sm_done
	; roughness: disp = (uint16)(mid.col - span.col) >> 1
	move.w	d5,d0
	sub.w	d2,d0
	lsr.w	#1,d0			; d0 = disp
	btst	#8,d1			; fracSum >= $100 ?
	beq	sm_down
	add.w	d0,d6			; midHgt += disp
	bra	sm_wrb
sm_down:
	sub.w	d0,d6			; midHgt -= disp
sm_wrb:
	move.b	d0,mem+$B5
	lsr.w	#8,d0
	move.b	d0,mem+$B6
sm_done:
	rts

; push_mid — subpt_store(depth+1, mid): write mid (d5/d6/d7) to slot depth+1 (a1+1).
; Clobbers d0.
push_mid:
	move.b	d5,(SDCOL_LO+1,a1)
	move.l	d5,d0
	lsr.w	#8,d0
	move.b	d0,(SDCOL_HI+1,a1)
	move.b	d6,(SDHGT_LO+1,a1)
	move.l	d6,d0
	lsr.w	#8,d0
	move.b	d0,(SDHGT_HI+1,a1)
	move.b	d7,(SDFRAC+1,a1)
	rts

; load_far — d0 = far.col, d1 = far.hgt from slot depth (a1).  (frac not needed by callers.)
load_far:
	moveq	#0,d0
	move.b	(SDCOL_HI,a1),d0
	lsl.w	#8,d0
	move.b	(SDCOL_LO,a1),d0
	moveq	#0,d1
	move.b	(SDHGT_HI,a1),d1
	lsl.w	#8,d1
	move.b	(SDHGT_LO,a1),d1
	rts

; load_span — span (d2/d3/d4) = subpt_load(depth) from slot depth (a1).
load_span:
	moveq	#0,d2
	move.b	(SDCOL_HI,a1),d2
	lsl.w	#8,d2
	move.b	(SDCOL_LO,a1),d2
	moveq	#0,d3
	move.b	(SDHGT_HI,a1),d3
	lsl.w	#8,d3
	move.b	(SDHGT_LO,a1),d3
	move.b	(SDFRAC,a1),d4
	rts
