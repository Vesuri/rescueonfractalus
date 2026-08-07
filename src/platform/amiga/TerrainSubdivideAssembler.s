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
	ifd	ROF_RASTER_SPAN_ABI
	xref	terrain_column_rasterize_span
	else
	xref	terrain_column_rasterize_core
	endc

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
	; mid ($8D-$91) is deliberately NOT loaded.  The oracle seeds it from ZP purely so that
	; its `out:` flush can write the same bytes back when no midpoint was ever computed —
	; every read of `mid` in the body is preceded by an assignment from subdiv_midpoint.  So
	; we skip the load (9 instructions, 2 of them 22-cycle `lsl.w #8`) and instead make the
	; flush CONDITIONAL at sd_out: if no midpoint ran, mem[$8D-$91] still holds exactly what
	; the oracle would write back, so leaving it alone is byte-identical.  See sd_out.

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
	; Flush span to $82/$84/$86 — the ONLY three the rasterizer reads (and rewrites).  The
	; oracle stores the high bytes $83/$85 here too, but nothing reads them before sd_out
	; rewrites them from d2/d3, so on the Amiga they are dead here: skipping them drops two
	; 22-cycle `lsr.w #8` (plus their move.l/move.b) per rasterized leaf.
	ifd	ROF_RASTER_SPAN_ABI
	; Hand the running cursor over in REGISTERS (terrain_column_rasterize_span, the private
	; entry in TerrainRasterizeAssembler.s).  Both sides keep col/height/frac in registers, so
	; the old mem[$82/$84/$86] round trip — 3 stores here, 3 loads there, 3 stores back, 3
	; loads plus two high-byte merges here — was ~220 cycles of pure handoff on every call.
	; The span entry preserves d2/d3/d5-d7/a2-a6 and returns the three bytes in d0/d1/d4, so
	; our 16-bit span.col/span.hgt keep their high bytes and only the low byte is merged.
	; a2 already holds `depth`, which IS the colBase argument.  rasterEntryDepth (a4) is dead
	; in the rasterizer (the C assigns it to `depth` then overwrites with 0), so it is not
	; passed.  mem[$82/$84/$86] stay stale until sd_out flushes them from d2/d3/d4 — nothing
	; between here and there reads them (grep: the only other refs in either file are that
	; flush and the entry load).
	moveq	#0,d0
	move.b	d2,d0			; col    (zero-extended: the body does .w arithmetic)
	moveq	#0,d1
	move.b	d3,d1			; height
	and.w	#$FF,d4			; frac — OUR d4's high byte is dirty (every writer here
					; is a bare move.b, which is why submid masks it before
					; `add.w`), and the rasterizer adds d4 as a WORD.  It used
					; to zero-extend on its own `moveq #0,d4` reload; now that
					; the value is handed over live, the clean-up is ours.
					; Bits 16-31 stay dirty and are never read (both sides are
					; .b/.w only), and inside the rasterizer every later write
					; to d4 is a move.b, so it stays clean for the whole call.
	jsr	terrain_column_rasterize_span
	lea	mem,a1			; raster clobbers a1 -> recompute a1 = mem+depth
	adda.l	a2,a1
	and.w	#$FF00,d2
	or.w	d0,d2			; span.col low byte = the cursor the rasterizer left
	and.w	#$FF00,d3
	or.w	d1,d3			; span.hgt low byte
	;      d4 already holds the rasterizer's frac
	else
	; Fallback for the builds where the span entry does not exist or must not be used:
	; RASTER_C=1 (no rasterizer asm at all) and the raster differential (ROF_RASTERIZE_VERIFY,
	; where every call has to go through the C dispatcher so raster_verify.gdb still sees it).
	move.b	d2,mem+$82
	move.b	d3,mem+$84
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
	endc
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
	; Flush mid ($8D-$91) only if one was actually computed.  The budget is the flag for
	; free: it is set to $14 after the entry guard and decremented ONCE immediately before
	; each of the two `bsr submid` sites, so `budget != $14` <=> at least one midpoint ran.
	; (The exhaustion exit needs 21 decrements, so it implies ~20 midpoints — still dirty.)
	; When it is clean, d5/d6/d7 hold the CALLER's registers, not a midpoint — and mem[]
	; already holds what the oracle would write back, so skipping the flush is what makes
	; dropping the entry load correct.
	cmpa.w	#$14,a3
	beq.s	sd_out_nomid
	move.b	d5,mem+$8D		; flush mid
	move.l	d5,d0
	lsr.w	#8,d0
	move.b	d0,mem+$8E
	move.b	d6,mem+$8F
	move.l	d6,d0
	lsr.w	#8,d0
	move.b	d0,mem+$90
	move.b	d7,mem+$91
sd_out_nomid:
	move.l	a3,d0			; $9F = budget (low byte; $FF if exhausted)
	move.b	d0,mem+$9F
sd_ret:
	move.l	a2,d0			; return depth
	movem.l	(sp)+,d2-d7/a2-a4
	rts

; ---------------------------------------------------------------------------
; terrain_subdivide_column_obj — the OBJECT-INDEXED entry.
;   uint8_t terrain_subdivide_column_obj(uint8_t startDepth, uint8_t rasterEntryDepth,
;                                        uint8_t obj0)
;
; Same body as the core entry; only where the running span comes from differs.
; terrain_draw_objects used to copy the primary endpoint's projected vector into mem[$82-$86]
; (5 memory-to-memory MOVE.B, ~120 cycles) purely so the prologue above could load it straight
; back out — the same shape as the rasterizer span handoff (Phase 5), one level up.  Take the
; object id instead and load the span from $2400/$242D/$245A/$2487/$23B5 directly.  That also
; makes the five loads CHEAPER: (d16,a0) is 12 cycles against 16 for absolute-long.
;
; ⚠ The one observable difference is the ENTRY-GUARD BAIL.  sd_out flushes $82-$86 from
; d2/d3/d4 on every other exit, so the caller's seeded values are dead there — but sd_ret does
; NOT flush, so on that path those 5 writes WERE the visible residue.  So publish them here
; instead: the same bytes, since d2/d3/d4 were just loaded from the same object arrays (only the
; write ORDER vs $B5 changes, and nothing reads them in between).  Measured over 12630 real
; subdivide calls the bail fires on 137 = 1.1%, so it costs ~1 cycle/call amortised.
;
; a0 is free here (the m68k C ABI's scratch set is d0/d1/a0/a1) and is dead after the guard.
	ifd	ROF_SUBDIV_VERIFY
	xdef	terrain_subdivide_column_obj_asm
	else
	xdef	terrain_subdivide_column_obj
	endc
terrain_subdivide_column_obj:
terrain_subdivide_column_obj_asm:
	movem.l	d2-d7/a2-a4,-(sp)	; 9 longs = 36 bytes; args shift +36
	moveq	#0,d0
	move.b	43(sp),d0		; startDepth (7 + 36)
	movea.l	d0,a2			; a2 = depth = startDepth
	lea	mem,a1
	adda.l	d0,a1			; a1 = mem + depth
	moveq	#0,d0
	move.b	47(sp),d0		; rasterEntryDepth (11 + 36)
	movea.l	d0,a4
	moveq	#0,d0
	move.b	51(sp),d0		; obj0 (15 + 36)
	lea	mem,a0
	adda.l	d0,a0			; a0 = mem + obj0

	; span = {$2400:$242D col, $245A:$2487 hgt, $23B5 frac} of object obj0
	moveq	#0,d2
	move.b	($242D,a0),d2
	lsl.w	#8,d2
	move.b	($2400,a0),d2
	moveq	#0,d3
	move.b	($2487,a0),d3
	lsl.w	#8,d3
	move.b	($245A,a0),d3
	move.b	($23B5,a0),d4
	; mid ($8D-$91) deliberately NOT loaded — same reason as the core entry (see there).

	; --- entry guard: $B5 = (far0.col>>8)^$80; bail if span.col >= far0.col
	moveq	#0,d0
	move.b	mem+SDCOL_HI,d0		; far0.col hi (slot 0)
	move.b	d0,d1
	eori.b	#$80,d1
	move.b	d1,mem+$B5
	lsl.w	#8,d0
	move.b	mem+SDCOL_LO,d0		; d0 = far0.col
	cmp.w	d0,d2			; span.col - far0.col (signed)
	blt.s	sd_obj_go		; span.col < far0.col -> subdivide
	; bail: publish the span, i.e. exactly the 5 bytes the caller used to write
	move.b	d2,mem+$82
	move.l	d2,d0
	lsr.w	#8,d0
	move.b	d0,mem+$83
	move.b	d3,mem+$84
	move.l	d3,d0
	lsr.w	#8,d0
	move.b	d0,mem+$85
	move.b	d4,mem+$86
	bra	sd_ret
sd_obj_go:
	movea.w	#$14,a3			; budget = $14
	bra	sd_phase2

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
