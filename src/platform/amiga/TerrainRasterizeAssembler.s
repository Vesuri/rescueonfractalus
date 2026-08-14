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
;  7. THE LOOP-TOP DISPATCH IS A 256-ENTRY PC-RELATIVE JUMP TABLE (2026-08-08).  After 6. the
;     loop top was a chain of up to five compares whose WEIGHTED cost was 49.5 cycles (span >= 9
;     44% / 5-8 49.5% / 4 5.5% / 1 1%, i.e. essentially a binary choice with a long tail); the
;     table is a flat 36 (move/add/add/jmp/bra.w) for EVERY span.  ~99 cycles a call.
;  6. SPANS 5-8 ARE FUSED BLOCKS (ras_s5/s6/s7/s8, 2026-08-08).  Such a node bisects into a
;     child of span S>>1 (2/3/3/4) and a parent remainder of S-(S>>1) (3/3/4/4), so BOTH
;     halves are leaf blocks: the push spill, the pop, the child's underflow test and exit
;     sequence, and two loop-top dispatches are all removable, and the block simply falls
;     into ras_sp3 / ras_sp4 for its parent half.  Re-shape-probed on the QUIET baseline
;     (9102 calls): spans 5-8 are 27.5% of all far-bisects and 53% of the ones 5. does not
;     already absorb = 3.6 per call.  ~160 cycles a node, ~8% of this function.
;
;  8. plotCol IS BIASED BY -$D4 THROUGHOUT PHASE 2 (2026-08-08), so the right-edge bound
;     test is a bare `bpl` on the flags the column step already set.  `plotCol >= $D4` runs
;     ~14 times a call and was `cmp.b #$D4,d5` + `bcc` = 16 cycles not-taken, for a branch
;     that fires exactly once per call.  In [$2C,$D4) the biased column is -168..-1, which
;     does NOT fit a signed BYTE — so d5 is stepped with `addq.w #1,d5` and tested as a WORD,
;     and a1/a2/a4 are rebased by +$D4 (the .w index sign-extends, so the negative index
;     lands back on the real entry).  Nine of the sites take the branch straight off the
;     step's own N flag (-8 each); the other four sit after RASPOP, which clobbers the flags
;     for a `frac = CTL_FRAC(depth+1)` the oracle genuinely does BEFORE the bound test, so
;     they get `tst.w d5` (-4 each).  ~-110 cycles a call against +12 to un-bias `col` once.
;
;  9. SPANS 9..16 GET THEIR OWN ph2_far STUBS (2026-08-08, ras_f9..ras_f16 + the FARFUSE
;     macro).  Such a node's child span c = S>>1 and parent remainder p = S-c are constants,
;     so the span arithmetic and the roughness displacement become immediates and the stub
;     branches STRAIGHT into the child block instead of `bra ph2_loop` + a 36-cycle dispatch.
;     ~60 cycles on 1.94 nodes a call.  This is the part of a span-9..16 FUSION that needs no
;     duplicated code: the child's own tail still pops and dispatches generically, so the
;     parent half is reached exactly as before and every existing block is untouched.
;
; Register map (callee-saved d2-d7/a2-a6 saved at entry):
;   d0/d1/d7 = scratch (also the DRAW macro's scratch)
;   d2 = span (cp[depth].col - plotCol)   d3 = height (cursor)   d4 = frac (cursor)
;   d5 = plotCol - $D4 in phase 2 (see 8. — NEGATIVE while on-screen, and a WORD)
;   d6 = chgt (TOS control-point height)
;     (d2/d3/d4/d6 hold a BYTE with the upper bits kept clean, so .w arithmetic is safe)
;   a0 = col — the $82 writeback value; only the leaf handlers update it, and they store the
;        BIASED d5, so `done` adds $D4 back.  The paths that exit before phase 2 has biased
;        anything leave a plain column there and exit through `done_raw` instead.
;   a1 = kDotColMask + $D4 (plotCol -> pixel mask, 0 = off-viewport)
;   a2 = mem+$260E + $D4  (COL_MAX per-column max-height base, indexed by plotCol)
;   a3 = current control-point slot ptr; slot = [postSpan, hgt, frac], walked +/-3
;   a4 = kDotColOff + $D4  (plotCol -> plane byte offset within the row)
;   a5 = g_flightDotPlane (plane2 dot buffer; armed once at init -> never null in flight)
;   a6 = kDrawDotRowOff (oldMax -> plane2 row byte-offset, or $FFFF sentinel; word entries)
;   d5 doubles as phase-1 scratch (plotCol is not live until ph2_enter).
; The control-point stack base (for the depth==0 underflow test) used to sit in a4; it is just
; SP — nothing between the `lea -CPBUF(sp),sp` and `done` touches the stack (no bsr/jsr since
; DRAW was inlined) — so the test is `cmpa.l sp,a3` and a4 is free for the second dot table.

	xdef	terrain_column_rasterize_core_asm
	ifnd	ROF_RASTERIZE_VERIFY
	xdef	terrain_column_rasterize_core		; ships as the core symbol directly
	endif
	xdef	flight_edge_plot_asm
	xref	mem
	xref	kDrawDotRowOff
	xref	kDotColMask
	xref	kDotColOff
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
; no per-plot null test.
; The COLUMN half is folded the same way, into a1/a4 (RescueOnFractalus.cpp): the plot's
; `_ac = plotCol-48`, its (unsigned)_ac < 160 gate, the `_ac>>2` byte offset and the
; `kColMask4[_ac&3]` pixel mask are all pure functions of plotCol, so they are two table
; reads indexed by the RAW column.  kDotColMask is 0 outside [48,208) — a value no real
; 2-bit mask has — so the SAME `move.b` that fetches the mask is also the range gate.
; That is 6 instructions (~62 cycles) in place of 13 (~104, one a variable-count LSR).
;
; ⭐ oldMax is read into d7, NOT d1, so the head needs no `moveq #0` (2026-08-08).  The read
; has to land in a register whose bits 8-15 are already 0, because the accepted path uses it
; as a WORD index into kDrawDotRowOff — and d1 cannot be that register: DRAWDOT itself leaves
; the row byte-offset (up to 5640) in d1, so back-to-back DRAWs inside one leaf block would
; see a dirty high byte.  d7 can: every writer of d7 in this file is either a `move.b` (here)
; or phase 1's `move.l d1,d7`, whose value is a column <= $AA (col < $2C plus child <= $7F).
; That INVARIANT IS NOW LOAD-BEARING — the one path that reaches a DRAW before phase 1 has
; written d7 (the one-column early-out at ph1_init's `bne`) clears it explicitly.
; Worth -4 cycles on EVERY draw against +4 (the `move.w d7,d1` below) on the 36% that are
; accepted: 23.8 draws and 8.6 accepts a call = ~-61 cycles a call.
DRAWDOT	macro
	move.b	(a2,d5.w),d7		; oldMax = COL_MAX(plotCol)   (d7 bits 8-15 stay 0)
	cmp.b	d7,d0			; _h - oldMax
	bls.s	.dend\@			; _h <= oldMax -> hidden, nothing
	move.b	d0,(a2,d5.w)		; COL_MAX(plotCol) = _h
	cmp.b	#$97,d0
	bcs.s	.ddot\@			; _h < $97
	move.b	#$FF,(a2,d5.w)		; saturate: full column
.ddot\@:
	; ROF_PLOT_DOT(plotCol, oldMax) — uses oldMax (the PREVIOUS top), not _h.
	move.w	d7,d1			; the doubling goes through d1: an `add.w d7,d7` here
	add.w	d1,d1			; would put oldMax's bit 7 into d7's high byte and
					; break the invariant the head depends on
	move.w	(a6,d1.w),d1		; kDrawDotRowOff[oldMax], or $FFFF
	bmi.s	.dend\@			; sentinel -> off display / $6b reset-floor -> skip
	move.b	(a1,d5.w),d7		; kDotColMask[plotCol]  (0 <=> off viewport)
	beq.s	.dend\@
	moveq	#0,d0			; (d0/_h is dead from here; clear for the byte index)
	move.b	(a4,d5.w),d0		; kDotColOff[plotCol] = (plotCol-48)>>2
	add.w	d0,d1			; byte offset = rowoff + colOff
	or.b	d7,(a5,d1.w)		; g_flightDotPlane[off] |= mask
.dend\@:
	endm

; ---------------------------------------------------------------------------
; POP to the parent control point.  frac comes from the slot we are LEAVING (the oracle's
; `frac = CTL_FRAC(depth+1)` after `depth--`), span/chgt from the parent's slot.
RASPOP	macro
	move.b	2(a3),d4		; frac  = this leaf's fraction
	subq.l	#3,a3			; depth--
	moveq	#0,d2			; the loop-top jump table indexes on d2.w, so bits 8-15
					; must be 0 — a bare move.b would inherit them from
					; whatever the leaf block last left in d2 (a control height
					; or a midpoint, both <= $FF, so this is belt-and-braces at
					; 4 cycles on ~3.2 pops/call).  See ph2_loop.
	move.b	(a3),d2			; span  = the parent's post-child span
	move.b	1(a3),d6		; chgt  = the parent's control-point height
	endm

; ---------------------------------------------------------------------------
; FARFUSE p, childBlock, dispUp, dSub — ph2_far specialised to a KNOWN span (see 9. below).
; For a node of span S in 9..16 the child span c = S>>1 and the parent remainder p = S-c are
; compile-time constants, so this is ph2_far with:
;   - the child/parent span arithmetic (move.w/lsr/sub.b/move.l = 28 cycles) replaced by one
;     12-cycle immediate store of p into the slot RASPOP reads back;
;   - the roughness displacement (c>>1 up, ((c-1)>>1)+1 down) an IMMEDIATE, which also collapses
;     the down path's not.b/add.w/cmp.w 9-bit-add spelling into a subq + bcc, exactly as the
;     shipped ras_s5..s8 blocks already do (same identity: `t = ceil + ~disp; mh = (t > $FF) ?
;     t&$FF : 0` is `ceil - (disp+1)` floored at 0);
;   - `bra ph2_loop` + the child's 36-cycle loop-top dispatch replaced by a direct `bra` into
;     the child block.
; Everything else is byte-for-byte ph2_far: the same three spills in the same order, the same
; a3 advance, and the child's own tail still does the generic cmpa/RASPOP/dispatch, so the
; parent half is reached exactly as it is today.  d2 is left holding S — dead, because every
; leaf block writes d2 before reading it (only ph2_far reads a dispatched span; see the ⚠ at
; ph2_loop) and RASPOP reloads it from (a3).
FARFUSE	macro
	move.b	#\1,(a3)		; post-child span p (RASPOP reads it back)
	move.b	d6,1(a3)		; spill this node's control-point height
	moveq	#0,d1
	move.b	2(a3),d1		; cfrac
	add.w	d4,d1
	addq.w	#1,d1			; d1 = fsum1 (bit8 = carry)
	move.b	d1,5(a3)		; slot[depth+1].frac = the child's cfrac
	add.w	d3,d6			; d6 = hsum = chgt + height
	btst	#7,d1
	bne.s	.rough\@
	lsr.w	#1,d6			; no roughness: mh1 = hsum>>1
.go\@:
	addq.l	#3,a3			; depth++
	bra	\2			; straight into the child block — no dispatch
.rough\@:
	addq.w	#1,d6
	lsr.w	#1,d6			; ceil = (hsum+1)>>1
	btst	#8,d1
	beq.s	.down\@
	addq.w	#\3,d6			; up: disp = c>>1
	cmp.w	#$FF,d6
	bls.s	.go\@
	move.w	#$FF,d6			; saturate $FF
	bra.s	.go\@
.down\@:
	subq.w	#\4,d6			; down: ceil - (((c-1)>>1) + 1) ...
	bcc.s	.go\@
	moveq	#0,d6			; ... floored at 0
	bra.s	.go\@
	endm

; ---------------------------------------------------------------------------
; PUBLIC C-ABI entry:  void terrain_column_rasterize_core(uint8_t entryDepth, uint8_t colBase)
; Keeps the exact mem[$82/$84/$86] contract that the C oracle, `make validate` and the
; in-process differential (raster_verify.gdb) check — it is a thin shim that loads the running
; cursor out of mem[], runs the body, and stores it back.  entryDepth is dead (the C assigns it
; to `depth` then immediately overwrites with 0), so it is not forwarded.
; The HOT caller (the subdivide asm) does NOT come through here: it calls
; terrain_column_rasterize_span below, which takes and returns the cursor in registers.
terrain_column_rasterize_core:
terrain_column_rasterize_core_asm:
	movem.l	d4/a2,-(sp)		; d4/a2 are callee-saved in the C ABI; args shift +8
	moveq	#0,d0
	move.b	19(sp),d0		; colBase  (11 + 8)
	movea.l	d0,a2			; -> the span entry's colBase register
	moveq	#0,d0
	move.b	mem+$82,d0		; col
	moveq	#0,d1
	move.b	mem+$84,d1		; height
	moveq	#0,d4
	move.b	mem+$86,d4		; frac
	bsr	terrain_column_rasterize_span
	move.b	d0,mem+$82		; the live writeback the caller reads back
	move.b	d1,mem+$84
	move.b	d4,mem+$86
	movem.l	(sp)+,d4/a2
	rts

; ---------------------------------------------------------------------------
; PRIVATE register ABI — the entry the subdivide asm calls.
;
; Subdivide holds the running cursor in registers and so does this function, yet the two used
; to hand it over through mem[$82/$84/$86]: 3 stores in the caller, 3 loads here, 3 stores here
; at `done`, 3 loads (plus two high-byte merges) back in the caller.  That round trip was ~220
; cycles on EVERY call — measured at ~49 calls per flight iteration — for a value both sides
; already had in a register.  Passing it in registers removes all twelve accesses, the
; stack-argument push/pop, and one long from the movem.
;   in : d0.b = col ($82)   d1.b = height ($84)   d4.b = frac ($86)   a2.l = colBase
;        (all three must arrive ZERO-EXTENDED — the body does .w arithmetic on them)
;   out: d0.b = col         d1.b = height         d4.b = frac
;   clobbers d0/d1/d4 (the results) + a0/a1; d2/d3/d5-d7/a2-a6 are preserved, so the caller's
;   16-bit span.col/span.hgt keep their high bytes and only need an or.w of the low byte.
	xdef	terrain_column_rasterize_span
terrain_column_rasterize_span:
	movem.l	d2-d3/d5-d7/a2-a6,-(sp)	; 10 longs = 40 bytes (d4 is in/out -> not saved)
	move.l	a2,d2
	move.b	d2,mem+$60		; mem[$60] = colBase   (faithful; dead on Amiga)
	move.l	d1,d3			; height -> the body's cursor register
	move.l	d0,d1			; col    -> the body's d1 (a0 below)
	lea	-CPBUF(sp),sp		; allocate the private control-point stack
	movea.l	sp,a3			; a3 = current slot (depth 0); the base for the
					;      depth==0 test is SP itself (see the header)
	; The three plotCol-indexed bases carry the +$D4 that d5's bias took out (see 8. above);
	; a5/a6 are indexed by the height, not the column, so they are untouched.
	lea	mem+$260E+$D4,a2	; COL_MAX base
	move.l	g_flightDotPlane,a5	; plane2 dot buffer (armed at init -> non-null in flight)
	lea	kDrawDotRowOff,a6	; oldMax -> plane2 row byte-offset
	lea	kDotColMask+$D4,a1	; plotCol -> pixel mask (0 = off viewport)
	lea	kDotColOff+$D4,a4	; plotCol -> plane byte offset
	; seed control-point slot [0] from mem[$EA]/[$F4]; its column becomes the span below
	moveq	#0,d6
	move.b	mem+$EA,d6		; chgt = CTL_HEIGHT(0)
	move.b	d6,1(a3)		; slot[0].hgt
	move.b	mem+$F4,2(a3)		; slot[0].frac
	; running cursor height (d3) / frac (d4) came in from the caller — no mem[] load
	moveq	#0,d2			; span   (set below)
	moveq	#0,d5			; plotCol / phase-1 scratch

	; ---- setup: trivial-segment early-outs --------------------------------
	moveq	#0,d0
	move.b	mem+$95,d0		; endCol = CTL_COL(0)
	move.w	d1,a0			; a0 = col (arrived in d0, moved to d1 above)
	cmp.b	#$2D,d0
	bcs	done_raw		; endCol < $2D -> nothing on screen
	cmp.b	d1,d0			; endCol - col
	bcs	done_raw		; endCol < col -> empty segment
	bne.s	ph1_init		; endCol > col -> phase 1
	; endCol == col: one column wide -> plot it and done.  There is no bound test on this
	; path (DRAWDOT's own kDotColMask==0 gate covers it), but d5 still has to carry the
	; bias, because the three tables it indexes are rebased.
	move.w	d0,d5			; plotCol = endCol
	sub.w	#$D4,d5			; ...biased
	move.l	d6,d0			; _h = CTL_HEIGHT(0)
	moveq	#0,d7			; the ONLY DRAW that runs before phase 1's `move.l d1,d7`
					; has established DRAWDOT's d7 bits-8-15-clear invariant,
					; so this path has to establish it itself (see DRAWDOT)
	DRAWDOT
	bra	done_raw		; a0 is still the plain col from the setup above

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
	move.w	d7,d5			; plotCol = col ...
	move.w	d7,a0			; a0 = col (plain — only done_raw below can read this one)
	sub.w	#$D4,d5			; ...biased; N = "still on screen" from here on
	bpl	done_raw
	; Dispatch on span, through a 256-entry PC-relative jump table.  The mix POST-FUSION
	; (derived from the amiga/ras_shape.gdb far-bisect histogram, quiet baseline 2026-08-08:
	; the fused blocks absorb 5.86 of the 6.27 span-3/4 nodes a call) is span >= 9 44% /
	; spans 5-8 49.5% / span 4 5.5% / span 1 1% / spans 2-3 ~0, over ~7.3 dispatches a call.
	; That is essentially a binary choice with a long tail, and the compare chain it replaces
	; priced it badly: 34 cycles for span >= 9 but 50/62/82/94 for spans 5/6/7/8 (every branch
	; in it assembles as a WORD branch under -no-opt, so each not-taken test cost 12), i.e. a
	; weighted 49.5.  The table is a FLAT 36 for every span — move/add/add (12) + jmp (14) +
	; the entry's bra.w (10) — and needs no compare at all.  ~99 cycles a call.
	;
	; ⚠ The index is scaled in d0, NOT in place: `add.w d2,d2` twice is two cycles cheaper
	; but ph2_far READS the span (`move.w d2,d0` / `sub.b d0,d2`) after it is dispatched, so
	; scaling d2 destroys it — the first cut did exactly that and the game died on its first
	; flight frame.  "d2 is dead once dispatched" is true only of the LEAF blocks.  d0 is the
	; safe choice: every one of the nine targets writes d0 before reading it.
	; d2's bits 8-15 must be 0 for the `.w` index: every writer of d2 in this file either
	; clears them (moveq / a move.l of a value <= $FF) or is a move.b into a register that
	; already had them clear, and RASPOP now states that explicitly.
	; The table has to sit HERE, immediately after the jmp: `jmp (d8,PC,Dn.W)` has only an
	; 8-bit displacement (the INDEX is added afterwards and is unbounded), so ras_jt must be
	; within 127 bytes of the extension word.  ph2_far therefore follows the table.
ph2_loop:
	move.w	d2,d0			; span -> scratch (see the warning above)
	add.w	d0,d0
	add.w	d0,d0			; span * 4 = byte offset of its 4-byte table slot
	jmp	ras_jt(pc,d0.w)

	; Slot 0 is unreachable — span 0 would not terminate in the oracle either — and slots
	; 9..255 are all the generic bisect, so a hypothetical out-of-range span still lands on
	; real code rather than in the middle of an instruction.
ras_jt:
	bra.w	ph2_far			; span 0 (unreachable)
	bra.w	ph2_ff			; span 1
	bra.w	ph2_fe			; span 2
	bra.w	ras_sp3			; span 3
	bra.w	ras_sp4			; span 4
	bra.w	ras_s5			; span 5
	bra.w	ras_s6			; span 6
	bra.w	ras_s7			; span 7
	bra.w	ras_s8			; span 8
	bra.w	ras_f9			; span 9   \
	bra.w	ras_f10			; span 10   |
	bra.w	ras_f11			; span 11   |  ph2_far with c/p and the roughness
	bra.w	ras_f12			; span 12   >  displacement as CONSTANTS, branching
	bra.w	ras_f13			; span 13   |  straight into the child block (see 9.)
	bra.w	ras_f14			; span 14   |
	bra.w	ras_f15			; span 15   |
	bra.w	ras_f16			; span 16  /
	rept	239
	bra.w	ph2_far			; spans 17..255
	endr

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

	; --- spans 9..16: ph2_far with the child's DISPATCH removed ---------------
	; A node of span S in 9..16 bisects into a child of span c = S>>1 (= 4..8) and a parent
	; remainder p = S-c (= 5..8), and BOTH of those are straight-line blocks already.  Full
	; fusion (inlining the child chain so the parent is a fall-through as well) was priced at
	; ~144 cycles a node but needs ~950 lines — a duplicate child chain per S, because S=2c and
	; S=2c+1 share a child but not a parent — and the `bsr`/`rts` variant at ~110 for ~500 lines
	; with a return address under `done`'s `lea CPBUF(sp),sp`.  Neither clears 1% of wall: the
	; span-5..8 fusion removed the SAME scaffolding per node and measured -8.0% of this function
	; at 3.61 nodes a call, and spans 9-16 are only 1.94 a call (17507 of the far-bisect
	; histogram), so the whole family caps at ~1.1% of wall even fused to infinity.
	;
	; What IS free is the half that needs no duplication at all.  With c and p constant the
	; stub can branch straight to the child block instead of going `bra ph2_loop` -> 36-cycle
	; dispatch, and the span arithmetic and the displacement collapse into immediates:
	; **~60 cycles a node, ~1.4% of this function**, with every existing block untouched, a3
	; moving exactly as it does now, and no register or stack change.  The parent half still
	; costs a dispatch, because that would need the child's tail duplicated (= the other two
	; designs).  See FARFUSE above for the per-item accounting.
	;
	;		 p  child    disp_up  ceil-(disp_down+1)
ras_f9:		FARFUSE	5,ras_sp4,2,2	; c=4
ras_f10:	FARFUSE	5,ras_s5,2,3	; c=5
ras_f11:	FARFUSE	6,ras_s5,2,3	; c=5
ras_f12:	FARFUSE	6,ras_s6,3,3	; c=6
ras_f13:	FARFUSE	7,ras_s6,3,3	; c=6
ras_f14:	FARFUSE	7,ras_s7,3,4	; c=7
ras_f15:	FARFUSE	8,ras_s7,3,4	; c=7
ras_f16:	FARFUSE	8,ras_s8,4,4	; c=8

	; --- span 1 (the oracle's gap==$FF): plot the endpoint column, pop -------
ph2_ff:
	move.w	d5,a0			; col = plotCol
	move.l	d6,d3			; height = chgt
	move.l	d3,d0			; _h
	DRAWDOT
	addq.w	#1,d5			; plotCol++
	cmpa.l	sp,a3			; depth == 0 ? (cp base == SP; a4 is a dot table now)
	beq.s	rdn_ff			; depth was 0 -> done
	RASPOP				; ...clobbers the step's N, and the oracle really does pop
	tst.w	d5			;    (frac included) BEFORE testing the bound
	bpl.s	rdn_ff
	bra	ph2_loop

	; A local `bra done` trampoline so the block's exits above can be SHORT branches.  The
	; build passes vasm -no-opt, so every `Bcc done` assembled as a WORD branch — and a
	; NOT-taken word Bcc costs 12 cycles against a short one's 8, over ~18 bound/depth tests
	; a call (~14 of them within reach of a stub; the four mid-block sites that no stub can
	; reach stay word branches).  Exactly one exit is ever TAKEN per call, so the stub's
	; extra `bra` is paid once.  vasm ENFORCES the +/-127 range: if a block grows past it the
	; `.s` fails to assemble rather than going quietly wrong.
rdn_ff:	bra	done

	; --- span 2 (the oracle's gap==$FE): interpolated column, endpoint, pop --
ph2_fe:
	move.w	d5,a0			; col = plotCol
	move.w	d6,d0
	add.w	d3,d0
	addq.w	#1,d0
	lsr.w	#1,d0			; (chgt + height + 1) >> 1
	DRAWDOT
	addq.w	#1,d5
	move.l	d6,d3			; height = chgt
	move.l	d3,d0
	DRAWDOT
	addq.w	#1,d5
	cmpa.l	sp,a3			; depth == 0 ? (cp base == SP; a4 is a dot table now)
	beq.s	rdn_fe
	RASPOP				; clobbers the step's N (see ph2_ff)
	tst.w	d5
	bpl.s	rdn_fe
	bra	ph2_loop
rdn_fe:	bra	done

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
	addq.w	#1,d5
	bpl.s	rdn_fe
	; parent (span 2) fe
	move.w	d5,a0
	move.w	d6,d0
	add.w	d3,d0
	addq.w	#1,d0
	lsr.w	#1,d0			; (chgt + mh + 1) >> 1
	DRAWDOT
	addq.w	#1,d5
	move.l	d6,d3			; height = chgt
	move.l	d3,d0
	DRAWDOT
	addq.w	#1,d5
	cmpa.l	sp,a3			; depth == 0 ? (cp base == SP; a4 is a dot table now)
	beq.s	rdn_sp3
	RASPOP				; clobbers the step's N (see ph2_ff)
	tst.w	d5
	bpl.s	rdn_sp3
	bra	ph2_loop
rdn_sp3:	bra	done

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
	addq.w	#1,d5
	move.l	d2,d3			; height = mh
	move.l	d3,d0
	DRAWDOT
	addq.w	#1,d5
	bpl	done			; no stub reaches this mid-block site: still a word branch,
					; but 12 cycles instead of the cmp's 8 + 12
	; parent (span 2) fe
	move.w	d5,a0
	move.w	d6,d0
	add.w	d3,d0
	addq.w	#1,d0
	lsr.w	#1,d0
	DRAWDOT
	addq.w	#1,d5
	move.l	d6,d3			; height = chgt
	move.l	d3,d0
	DRAWDOT
	addq.w	#1,d5
	cmpa.l	sp,a3			; depth == 0 ? (cp base == SP; a4 is a dot table now)
	beq.s	rdn_sp4
	RASPOP				; clobbers the step's N (see ph2_ff)
	tst.w	d5
	bpl.s	rdn_sp4
	bra	ph2_loop
rdn_sp4:	bra	done

	; --- spans 5..8: the bisect FUSED with both of its leaf halves ------------
	; A node of span S in 5..8 bisects into a child of span c = S>>1 (= 2/3/3/4) and a
	; parent remainder p = S-c (= 3/3/4/4).  BOTH are straight-line leaf blocks, so the
	; generic path's push/pop round trip through the control-point slot, the child's
	; loop-top dispatch, its whole exit sequence (cmpa/beq/RASPOP/bra) and the parent's
	; dispatch are all dead weight: the entire 5-to-8-column group runs as one block
	; that falls into ras_sp3 / ras_sp4 for its parent half.  Shape (quiet baseline,
	; 2026-08-08, 9102 calls): spans 5/6/7/8 = 12404/9147/6494/4855 far-bisects = 27.5%
	; of all of them and 53% of those the sp3/sp4 blocks do not already absorb, i.e.
	; 3.6 per rasterize call.  ~160 cycles saved per node, ~8% of this function.
	;
	; State handling vs the generic push:
	;   - the child's control height (mh1) lives in d6 exactly as a pushed one would;
	;   - this node's OWN control height goes to d2 (span is dead once dispatched)
	;     instead of 1(a3), and comes back with a register move;
	;   - fsum1 still goes to 5(a3) (the slot a push would have used) for c = 3 and 4,
	;     because those child bodies overwrite d4 with their own fsum; for c = 2
	;     (ras_s5) the fe child never touches d4, so fsum1 simply stays in it;
	;   - a3 never moves, so the child's `cmpa.l sp,a3` underflow test — which can never
	;     fire at depth+1 — is gone, and 2(a3) is still this node's cfrac for the parent.
	; The roughness displacement is a CONSTANT here (disp_up = c>>1, disp_down =
	; (c-1)>>1), and the oracle's `t = ceil + ~disp; mh = (t > $FF) ? t&$FF : 0` is
	; exactly `ceil - (disp+1)` floored at 0.  So: c=2 -> +1 / -1, c=3 -> +1 / -2,
	; c=4 -> +2 / -2.  (c=2 and c=3's child-of-child span 1 give disp 0 both ways,
	; which is why ras_sp3's inner midpoint has no add at all.)

	; --- span 5 = bisect(child 2) + fe(child) + sp3(parent) -------------------
ras_s5:
	move.l	d6,d2			; stash this node's control height (span is dead)
	moveq	#0,d1
	move.b	2(a3),d1		; cfrac
	add.w	d4,d1
	addq.w	#1,d1			; d1 = fsum1 (bit8 = carry)
	move.b	d1,d4			; frac = fsum1 — the fe child never touches d4
	add.w	d3,d6			; d6 = hsum = chgt + height
	btst	#7,d1
	bne.s	ras_s5_rough
	lsr.w	#1,d6			; mh1 = hsum>>1
	bra.s	ras_s5_go
ras_s5_rough:
	addq.w	#1,d6
	lsr.w	#1,d6			; ceil = (hsum+1)>>1
	btst	#8,d1
	beq.s	ras_s5_down
	addq.w	#1,d6			; up: disp = 2>>1 = 1
	cmp.w	#$FF,d6
	bls.s	ras_s5_go
	move.w	#$FF,d6			; saturate $FF
	bra.s	ras_s5_go
ras_s5_down:
	subq.w	#1,d6			; down: disp = (2-1)>>1 = 0 -> ceil-1 ...
	bcc.s	ras_s5_go
	moveq	#0,d6			; ... floored at 0
ras_s5_go:
	; child (span 2) fe, control height = mh1 in d6
	move.w	d5,a0
	move.w	d6,d0
	add.w	d3,d0
	addq.w	#1,d0
	lsr.w	#1,d0			; (mh1 + height + 1) >> 1
	DRAWDOT
	addq.w	#1,d5
	move.l	d6,d3			; height = mh1
	move.l	d3,d0
	DRAWDOT
	addq.w	#1,d5
	bpl.s	rdn_s5			; out of bounds -> done (d6 is callee-saved, not an output)
	move.l	d2,d6			; chgt = this node's control height again
	bra	ras_sp3			; parent remainder = 3
rdn_s5:	bra	done

	; --- span 6 = bisect(child 3) + sp3(child) + sp3(parent) ------------------
ras_s6:
	move.l	d6,d2			; stash this node's control height
	moveq	#0,d1
	move.b	2(a3),d1		; cfrac
	add.w	d4,d1
	addq.w	#1,d1			; d1 = fsum1
	move.b	d1,5(a3)		; the sp3 child overwrites d4 -> park fsum1
	add.w	d3,d6			; hsum
	btst	#7,d1
	bne.s	ras_s6_rough
	lsr.w	#1,d6			; mh1 = hsum>>1
	bra.s	ras_s6_go
ras_s6_rough:
	addq.w	#1,d6
	lsr.w	#1,d6			; ceil
	btst	#8,d1
	beq.s	ras_s6_down
	addq.w	#1,d6			; up: disp = 3>>1 = 1
	cmp.w	#$FF,d6
	bls.s	ras_s6_go
	move.w	#$FF,d6
	bra.s	ras_s6_go
ras_s6_down:
	subq.w	#2,d6			; down: disp = (3-1)>>1 = 1 -> ceil-2 ...
	bcc.s	ras_s6_go
	moveq	#0,d6			; ... floored at 0
ras_s6_go:
	; child (span 3): its own midpoint (child span 1 -> disp 0 both ways), then ff+fe.
	; d1 still holds fsum1, so the child's cfrac needs no reload from 5(a3).
	and.w	#$FF,d1
	add.w	d4,d1
	addq.w	#1,d1			; d1 = fsum2
	move.w	d6,d0
	add.w	d3,d0			; d0 = hsum2
	btst	#7,d1
	bne.s	ras_s6_r2
	lsr.w	#1,d0			; mh2 = hsum2>>1
	bra.s	ras_s6_go2
ras_s6_r2:
	addq.w	#1,d0
	lsr.w	#1,d0			; ceil
	btst	#8,d1
	bne.s	ras_s6_go2		; up: disp 0 -> mh2 = ceil (no clamp possible)
	subq.w	#1,d0			; down: ceil - 1 ...
	bcc.s	ras_s6_go2
	moveq	#0,d0			; ... floored at 0
ras_s6_go2:
	move.w	d5,a0
	move.b	d1,d4			; frac = fsum2 (before DRAWDOT clobbers d1)
	move.l	d0,d3			; height = mh2
	DRAWDOT
	addq.w	#1,d5
	bpl	done			; mid-block: no stub in reach, so still a word branch
	move.w	d5,a0
	move.w	d6,d0
	add.w	d3,d0
	addq.w	#1,d0
	lsr.w	#1,d0			; (mh1 + mh2 + 1) >> 1
	DRAWDOT
	addq.w	#1,d5
	move.l	d6,d3			; height = mh1
	move.l	d3,d0
	DRAWDOT
	addq.w	#1,d5
	bpl.s	rdn_s6			; out of bounds -> the frac restore still has to happen
	move.l	d2,d6			; chgt = this node's control height
	move.b	5(a3),d4		; frac = fsum1
	bra	ras_sp3			; parent remainder = 3
rdn_s6:	move.b	5(a3),d4		; frac = fsum1 — the oracle pops (frac included) before it
	bra	done			;   tests the bound, so this is the exit value either way

	; --- span 7 = bisect(child 3) + sp3(child) + sp4(parent) ------------------
	; Identical to ras_s6 except the parent remainder is 4.
ras_s7:
	move.l	d6,d2
	moveq	#0,d1
	move.b	2(a3),d1		; cfrac
	add.w	d4,d1
	addq.w	#1,d1			; d1 = fsum1
	move.b	d1,5(a3)
	add.w	d3,d6			; hsum
	btst	#7,d1
	bne.s	ras_s7_rough
	lsr.w	#1,d6
	bra.s	ras_s7_go
ras_s7_rough:
	addq.w	#1,d6
	lsr.w	#1,d6			; ceil
	btst	#8,d1
	beq.s	ras_s7_down
	addq.w	#1,d6			; up: disp = 3>>1 = 1
	cmp.w	#$FF,d6
	bls.s	ras_s7_go
	move.w	#$FF,d6
	bra.s	ras_s7_go
ras_s7_down:
	subq.w	#2,d6			; down: disp = (3-1)>>1 = 1
	bcc.s	ras_s7_go
	moveq	#0,d6
ras_s7_go:
	and.w	#$FF,d1
	add.w	d4,d1
	addq.w	#1,d1			; d1 = fsum2
	move.w	d6,d0
	add.w	d3,d0			; hsum2
	btst	#7,d1
	bne.s	ras_s7_r2
	lsr.w	#1,d0
	bra.s	ras_s7_go2
ras_s7_r2:
	addq.w	#1,d0
	lsr.w	#1,d0			; ceil
	btst	#8,d1
	bne.s	ras_s7_go2		; up: disp 0
	subq.w	#1,d0			; down: ceil - 1
	bcc.s	ras_s7_go2
	moveq	#0,d0
ras_s7_go2:
	move.w	d5,a0
	move.b	d1,d4			; frac = fsum2
	move.l	d0,d3			; height = mh2
	DRAWDOT
	addq.w	#1,d5
	bpl	done			; mid-block: no stub in reach, so still a word branch
	move.w	d5,a0
	move.w	d6,d0
	add.w	d3,d0
	addq.w	#1,d0
	lsr.w	#1,d0
	DRAWDOT
	addq.w	#1,d5
	move.l	d6,d3			; height = mh1
	move.l	d3,d0
	DRAWDOT
	addq.w	#1,d5
	bpl.s	rdn_s7			; out of bounds -> the frac restore still has to happen
	move.l	d2,d6			; chgt = this node's control height
	move.b	5(a3),d4		; frac = fsum1
	bra	ras_sp4			; parent remainder = 4
rdn_s7:	move.b	5(a3),d4		; frac = fsum1 (see rdn_s6)
	bra	done

	; --- span 8 = bisect(child 4) + sp4(child) + sp4(parent) ------------------
	; The child is a span-4 group; unlike ras_sp4 it must not use d2 (this node's
	; control height lives there), so it parks its own midpoint in d1 and sets
	; height = mh2 BEFORE the first DRAW — DRAWDOT never reads d3.
ras_s8:
	move.l	d6,d2			; stash this node's control height
	moveq	#0,d1
	move.b	2(a3),d1		; cfrac
	add.w	d4,d1
	addq.w	#1,d1			; d1 = fsum1
	move.b	d1,5(a3)		; the sp4 child overwrites d4 -> park fsum1
	add.w	d3,d6			; hsum
	btst	#7,d1
	bne.s	ras_s8_rough
	lsr.w	#1,d6
	bra.s	ras_s8_go
ras_s8_rough:
	addq.w	#1,d6
	lsr.w	#1,d6			; ceil
	btst	#8,d1
	beq.s	ras_s8_down
	addq.w	#2,d6			; up: disp = 4>>1 = 2
	cmp.w	#$FF,d6
	bls.s	ras_s8_go
	move.w	#$FF,d6
	bra.s	ras_s8_go
ras_s8_down:
	subq.w	#2,d6			; down: disp = (4-1)>>1 = 1 -> ceil-2
	bcc.s	ras_s8_go
	moveq	#0,d6
ras_s8_go:
	; child (span 4): midpoint with ITS child span 2 -> disp +1 / -1, then fe + fe
	and.w	#$FF,d1
	add.w	d4,d1
	addq.w	#1,d1			; d1 = fsum2
	move.w	d6,d0
	add.w	d3,d0			; hsum2
	btst	#7,d1
	bne.s	ras_s8_r2
	lsr.w	#1,d0			; mh2 = hsum2>>1
	bra.s	ras_s8_go2
ras_s8_r2:
	addq.w	#1,d0
	lsr.w	#1,d0			; ceil
	btst	#8,d1
	beq.s	ras_s8_d2
	addq.w	#1,d0			; up: disp = 2>>1 = 1
	cmp.w	#$FF,d0
	bls.s	ras_s8_go2
	move.w	#$FF,d0
	bra.s	ras_s8_go2
ras_s8_d2:
	subq.w	#1,d0			; down: disp = (2-1)>>1 = 0 -> ceil-1
	bcc.s	ras_s8_go2
	moveq	#0,d0
ras_s8_go2:
	move.b	d1,d4			; frac = fsum2
	move.w	d5,a0
	move.w	d0,d1			; park mh2 (d2 holds this node's control height)
	add.w	d3,d0
	addq.w	#1,d0
	lsr.w	#1,d0			; (mh2 + height + 1) >> 1
	move.l	d1,d3			; height = mh2 (DRAWDOT does not read d3)
	DRAWDOT
	addq.w	#1,d5
	move.l	d3,d0			; _h = mh2
	DRAWDOT
	addq.w	#1,d5
	bpl	done			; mid-block: no stub in reach, so still a word branch
	; the child's parent half (span 2) fe, control height = mh1 in d6
	move.w	d5,a0
	move.w	d6,d0
	add.w	d3,d0
	addq.w	#1,d0
	lsr.w	#1,d0
	DRAWDOT
	addq.w	#1,d5
	move.l	d6,d3			; height = mh1
	move.l	d3,d0
	DRAWDOT
	addq.w	#1,d5
	bpl.s	rdn_s8			; out of bounds -> the frac restore still has to happen
	move.l	d2,d6			; chgt = this node's control height
	move.b	5(a3),d4		; frac = fsum1
	bra	ras_sp4			; parent remainder = 4
rdn_s8:	move.b	5(a3),d4		; frac = fsum1 (see rdn_s6) — falls through to done

	; ---- exit: hand the live cursor back in registers, free scratch, restore -
	; d4 already holds frac (it is in/out and never saved), so only col and height move.
	;
	; TWO tails.  Every leaf block stores the BIASED plotCol into a0 (see 8. in the header),
	; so this one adds $D4 back — and it must land in a CLEAN d0: the C-ABI shim only keeps
	; the low byte, but the subdivide fast path merges the result with `or.w d0,d2`, so a
	; sign-extension left in the high byte would corrupt the caller's span.col.  `move.w d5,a0`
	; is a MOVEA.W, which sign-extends, hence the moveq.
done:
	moveq	#0,d0
	move.w	a0,d0			; biased col
	add.w	#$D4,d0			; -> the real column, 0..255
	move.l	d3,d1			; height out (before the movem restores the caller's d3)
	lea	CPBUF(sp),sp		; free the control-point stack
	movem.l	(sp)+,d2-d3/d5-d7/a2-a6
	rts

	; ...and the tail for the exits that happen BEFORE phase 2 biases anything: the two
	; trivial-segment early-outs, the one-column case and ph2_enter's bound test all leave a
	; plain column in a0.
done_raw:
	move.l	a0,d0			; col out
	move.l	d3,d1			; height out
	lea	CPBUF(sp),sp
	movem.l	(sp)+,d2-d3/d5-d7/a2-a6
	rts

; ---------------------------------------------------------------------------
; flight_edge_plot_asm(uint8_t* bp) — RescueOnFractalus::renderFlightDirect's
; plane-1 skyline edge plot: one bit per column at its skyline scanline, 160
; columns = 40 plane-1 bytes (4 cols/byte).  Structured after the user's
; hypothetical-renderer asm: 4 columns unrolled with the column masks
; ($C0/$30/$0C/$03 = kColMask4), the plane-1 byte pointer (a2) walked +1 per 4
; columns (no c>>2), and the height->row-byte-offset folded through the
; kHeightRowOff[256] table = kRow120[clamp(150-h,0,46)] (no per-column 150-h /
; clamp branches).  Reads heights from mem[$260E+48..].  d0's high byte stays 0
; (only move.b writes it; the *2 index is taken in d1), so no per-column
; re-clear is needed.
;   a0 = height source (mem+$260E+48)   a1 = kHeightRowOff
;   a2 = plane-1 byte base (bp), advanced +1 per 4-column group
;   d3-d6 = the four column masks, held in registers (see 2. below)
;
; 2026-08-12 shave, −12 of 68 cycles per column (~−1920/call, two calls an
; iteration ≈ −0.6% of flight wall clock).  Both come from measurement, not
; inspection — `make EDGE_SHAPE=1 PROBES=1` + amiga/edge_shape.gdb over 1026
; frames (164160 columns) is what priced them:
;   1. THE OFF-TOP TEST IS A TABLE SENTINEL NOW.  h==$FF means the column is all
;      terrain body and must plot NOTHING; that was a per-column
;      `cmp.b #$FF,d0 / beq` = 16 cycles paid by EVERY column.  The header used
;      to say there was "no safe table sentinel without an extra buffer row" —
;      true of a sentinel OFFSET, false of a sentinel VALUE: kHeightRowOff[$FF]
;      is $FFFF, so the `move.w` that fetches the offset already sets N and one
;      not-taken `bmi` (8) replaces the pair.  Real offsets are kRow120[0..46] =
;      0..5520 and can never be negative, exactly as kDrawDotRowOff does it.
;      The trade is −8 cycles on a non-$FF column against +14 on an $FF one, and
;      the probe measured $FF at 3% of columns (5622/164160) — the break-even is
;      38%, so this is not close.
;   2. THE MASKS MOVED INTO REGISTERS.  `or.b #imm,(a2,d1.w)` is ORI to memory =
;      12+10 = 22 cycles; `or.b dN,(a2,d1.w)` is 8+10 = 18.  −4 a column for
;      four moveqs in the prologue.  (moveq sign-extends, so d3 holds $FFFFFFC0
;      — or.b only reads the low byte.)
; ⚠ The C oracle edgePlotCore keeps its explicit `h != $FF` test and therefore
; never indexes the sentinel entry, which is what keeps it a valid oracle for
; the `make VERIFY=1` differential that byte-compares the whole 47x120 plane.

;   3. THE LOOP IS GONE.  `addq.l #1,a2` + `dbra` was 18 cycles of pure
;      bookkeeping per 4-column group against 224 of work — but the plane-1 byte
;      index IS the group number 0..39, and (d8,An,Xn)'s displacement is an
;      8-bit SIGNED field, so all 40 groups are reachable from the SAME a2 at no
;      extra cycles and no extra bytes.  Fully unrolled through `rept`, the
;      pointer walk and the counter both disappear: −18 × 40 = −720 cycles a
;      call for ~3.4 KB of code.  d7 is now unused but stays in the movem list —
;      it costs 8 cycles once a call and keeps the frame size (and hence the
;      28(sp) argument offset) in one place.
;   4. THE PAIRWISE MERGE (2026-08-14).  Two columns of a pair share the plane-1
;      BYTE, so if they also share the ROW they need ONE lookup and ONE OR with
;      the two masks combined.  The test is the two RAW HEIGHTS: equal h means
;      equal row (kHeightRowOff is a function of h), and $FF==$FF merges too —
;      both then skip on the same sentinel.  Filed in flight-perf-log §23.3 as
;      "survives arithmetic at only -3.6 a column" on a PROXY hit rate (52%
;      same-row over ALL adjacent columns).  `amiga/edge_shape.gdb` now counts
;      the exact pairing: **55% (40421/72480 over 906 frames), of which 5pp is
;      $FF==$FF** -- against a break-even of 40%, so it is 15 points clear.
;        hit  78 cycles vs 112 for the two columns   -> -34   (50pp)
;        hit, both $FF          62 vs 80             -> -18   ( 5pp)
;        miss 134 vs 112 (the extra cmp + the bra)   -> +22   (45pp)
;      = -8.0 a pair, or -5.8 for the (2,3) pair whose merged mask has to be an
;      ORI immediate (22 cyc) because only ONE register was free for a merged
;      mask (d7, previously dead-but-saved).  **-3.45 cycles a column** = ~-1100
;      a call, two calls an iteration ~= 0.19% of flight wall clock, for ~2 KB.
;      ⚠ The MISS path must stay cheap: at a ~50/50 split, anything that moves
;      work out of the hit path and into the split path gives the whole win back
;      (measured variants: reading hB with `cmp.b (a0)+,d0` and reloading it as
;      `-1(a0)` on the miss is -6.6 a pair, WORSE, because it taxes the 45%).
;      ⚠ d2 needs its high byte cleared ONCE, exactly like d0 — `move.w d2,d1`
;      indexes with the full word.
EPPAIR	macro				; \1 = group (plane-1 byte index), \2/\3 = the two
					; column masks, \4 = the two ORed together
	move.b	(a0)+,d0		; even column
	move.b	(a0)+,d2		; odd column
	cmp.b	d0,d2
	beq.s	.m\@			; same height -> same row -> one lookup, one OR
	move.w	d0,d1			; --- split (45%): two rows, two ORs ---
	add.w	d1,d1
	move.w	(a1,d1.w),d1		; kHeightRowOff[h]; $FFFF = off-top ($FF) -> skip
	bmi.s	.b\@
	or.b	\2,(\1,a2,d1.w)
.b\@:
	move.w	d2,d1
	add.w	d1,d1
	move.w	(a1,d1.w),d1
	bmi.s	.d\@
	or.b	\3,(\1,a2,d1.w)
	bra.s	.d\@
.m\@:					; --- merged (55%) ---
	move.w	d0,d1
	add.w	d1,d1
	move.w	(a1,d1.w),d1
	bmi.s	.d\@			; both columns off-top -> plot nothing, as before
	or.b	\4,(\1,a2,d1.w)
.d\@:
	endm

EPGRP	macro				; \1 = group number 0..39 = the plane-1 byte index
	EPPAIR	\1,d3,d4,d7		; cols 4k, 4k+1   ($C0,$30 -> $F0 in d7)
	EPPAIR	\1,d5,d6,#$0F		; cols 4k+2, 4k+3 ($0C,$03 -> ORI #$0F)
	endm

flight_edge_plot_asm:
	movem.l	d3-d7/a2,-(sp)		; 6 callee-saved longs = 24 bytes; arg shifts +24
	movea.l	28(sp),a2		; bp  (4 + 24)
	lea	mem+$260E+48,a0		; per-column max-height ($260E[48..])
	lea	kHeightRowOff,a1
	moveq	#0,d0			; d0 high stays 0 for the whole run
	moveq	#0,d2			; ...and d2's, for the pair's odd column
	moveq	#$C0-256,d3		; the four column masks (kColMask4), low byte only
	moveq	#$30,d4
	moveq	#$0C,d5
	moveq	#$03,d6
	moveq	#$F0-256,d7		; d3|d4, for a merged (0,1) pair — moveq sign-extends,
					; or.b reads the low byte only.  d7 was already in the
					; movem list but unused; the (2,3) merge has no register
					; left and pays ORI's 4 extra cycles instead.
ep_loop:				; kept as a label so prof_flight.py still buckets this
EPG	set	0
	rept	40
	EPGRP	EPG
EPG	set	EPG+1
	endr
	movem.l	(sp)+,d3-d7/a2
	rts
