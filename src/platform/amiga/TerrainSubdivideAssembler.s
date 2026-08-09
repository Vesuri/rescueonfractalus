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
;   a2 = depth (scalar, kept in lockstep with a1)   a3 = budget
;   a4 = rasterEntryDepth — ONLY in the mem[]-handoff fallback; see FRM below
; The 5 SubPt stacks + all ZP live in mem[] (main-RAM scratch the flight VBI never writes),
; addressed absolutely (mem+$xx) or via a1; a signed 16-bit displacement covers each base.
;
; 2026-08-08: this twin's cost is ~50% per-call MARSHALLING (prologue, span load, entry
; guard, flush, epilogue) for a body that averages 1.21 inner iterations and 0.55
; rasterize calls (amiga/ras_shape.gdb, quiet baseline, 16342 calls / 239 iterations =
; 68.4 calls per flight iteration).  Five dead or over-priced things removed — see the
; comments at FRM, sd_phase3, sd_inner, sd_pop and sd_out.
;
; ⛔ DEADZP — CLOSED 2026-08-09, do NOT re-open without new evidence.  The exit residue
; (sd_out's $82-$86 span flush + $8D-$91 mid flush + $9F, and the $B5/$B6 writes at the entry
; guard, both width tests and submid's roughness tail) is ~200 of the ~1000 cycles a call,
; ~2% of the quiet frame, and it LOOKS like pure 6502 bookkeeping no Amiga code consumes.
; It is not.  A reader survey over every function still in the linked image (--gc-sections
; drops 51 of the 68 that reference these cells) plus the four sibling asm twins found a live
; read-before-write consumer for nine of the twelve cells:
;   $B5   — TerrainFrameSetupAssembler.s reads the OLD $B5 to build $B4 ("reads OLD $B5, before
;           the loop overwrites it").  terrain_frame_setup runs TWICE PER ITERATION, right after
;           a pass's subdivide calls, so subdivide is normally its last writer.  IN FLIGHT.
;   $82/$83 — dl_lms_push_bottom_core (inlined into scroll_terrain_dl) read-modify-writes them
;           as the door-scroll cinematic's persistent bottom-LMS pointer, +$2E a step.
;   $84   — alien_field0_fill seeds its bit-packing accumulator from it and its caller does not
;           write it; that is the jump-scare creature overlay, also IN FLIGHT.
;   $8F   — scroll_field_columns_core does `sfx_toggle_8F >>= 1`, a persistent frame toggle.
;   $90/$91 — read by boot_standby_launch_driver / standby_level_select_loop.
;   $9F   — read by draw_ring_frame_step / draw_symmetric_span_loop as a span coordinate their
;           callers do not set.
; Only $86 survived the survey (both readers — dl_lms_fill and alien_field0_fill — are written
; by their callers immediately before the call), and one byte store is 16 cycles = 0.16% of
; wall, not worth an asymmetric residue contract.
; ⭐ Two lessons.  (1) A C-only grep is NOT the survey: three of the consumers are in hand-asm
; twins or in a helper GCC inlined into its caller, so grep the .s files and the linked
; disassembly too.  (2) "6502 scratch nothing reads" is a hypothesis about a HEAVILY REUSED
; address, and these cells are reused as persistent pointers/toggles by other scenes.

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

; rasterEntryDepth is DEAD in the shipping build: it exists only to be forwarded as the
; rasterizer's first C argument, and the register-ABI entry does not take it (the rasterizer
; assigns it to `depth` and immediately overwrites it with 0).  So under ROF_RASTER_SPAN_ABI
; neither a4 nor its two-instruction load is emitted, and the movem drops a long — 36 cycles
; a call.  FRM is the frame the movem pushes, i.e. what the incoming args shift by.
	ifd	ROF_RASTER_SPAN_ABI
FRM		equ	32		; 8 longs: d2-d7/a2-a3
	else
FRM		equ	36		; 9 longs: d2-d7/a2-a4
	endc
ARG_START	equ	7+FRM		; startDepth
ARG_RASENT	equ	11+FRM		; rasterEntryDepth
; Under ROF_SUBDIV_OBJ1ARG the object entry takes obj0 ALONE (see its header): startDepth is
; the literal 0 at its one call site and rasterEntryDepth is dead, so obj0 is the first arg.
	ifd	ROF_SUBDIV_OBJ1ARG
ARG_OBJ0	equ	7+FRM		; obj0 (the only argument)
	else
ARG_OBJ0	equ	15+FRM		; obj0 (the object-indexed entry only)
	endc

	section	code

terrain_subdivide_column_core:
terrain_subdivide_column_core_asm:
	ifd	ROF_RASTER_SPAN_ABI
	movem.l	d2-d7/a2-a3,-(sp)	; 8 longs = FRM bytes; args shift +FRM
	else
	movem.l	d2-d7/a2-a4,-(sp)	; 9 longs
	endc
	moveq	#0,d0
	move.b	(ARG_START,sp),d0	; startDepth
	movea.l	d0,a2			; a2 = depth = startDepth
	lea	mem,a1
	adda.l	d0,a1			; a1 = mem + depth
	ifnd	ROF_RASTER_SPAN_ABI
	moveq	#0,d0
	move.b	(ARG_RASENT,sp),d0	; rasterEntryDepth
	movea.l	d0,a4
	endc

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
	; far0.col's high byte gets into bits 8-15 by a WORD read, not a 22-cycle `lsl.w #8`:
	; SDCOL_HI is an even address and the 68000 is big-endian, so `move.w mem+SDCOL_HI` puts
	; SLOT 0's byte in bits 8-15 (bits 0-7 take slot 1's, which the next move.b overwrites).
	; d0's upper WORD is left dirty by that — every later read of d0 here is .w, and the next
	; writer on each path (submid/sd_inner's `moveq #0,d0`, sd_out's `move.w d2,d0`) is full.
	move.b	mem+SDCOL_HI,d1		; far0.col hi (slot 0)
	eori.b	#$80,d1
	move.b	d1,mem+$B5
	move.w	mem+SDCOL_HI,d0		; d0 bits 8-15 = far0.col hi
	move.b	mem+SDCOL_LO,d0		; d0.w = far0.col
	cmp.w	d0,d2			; span.col - far0.col (signed)
	bge	sd_ret			; span.col >= far0.col -> return depth
	movea.w	#$14,a3			; budget = $14

	; ================= phase 2: descend =================
	; `btst #15,Dn` is 10 cycles and needs a following Bcc; `tst.w Dn` is 4 and sets N from
	; the same bit.  Used for every `span/far & $8000` test in this file.
sd_phase2:
	tst.w	d2			; span.col & $8000 ?
	bpl	sd_phase3		; non-negative -> start leaf pass
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
	; The oracle's two exits — `span.col > $FF` and `span.col >= $D8` — are ONE test.  Any
	; value with a non-zero high byte is >= $0100 > $D8, so the UNSIGNED `>= $D8` compare
	; already covers it (including a negative span.col off the pop path, which is >= $8000).
	; The `move.w`/`and.w #$FF00`/`bne` triple was 24 cycles per phase-3 entry, of which
	; there are 1.11 a call.
	cmp.w	#$D8,d2			; span.col >= $D8 ? (unsigned)
	bcc	sd_out
sd_inner:
	; load_far, INLINED at its ONE call site and split on the high byte first.  The
	; oracle's `far.col > $FF` is exactly `far.col hi != 0`, so the escape to sd_dosub is
	; decided before the low byte and the 22-cycle `lsl.w #8` are ever touched — and the
	; 85% that continue then have far.col's high byte known 0, i.e. far.col IS the low
	; byte.  (Shape: the escape fires on 2900 of 19794 inner iterations = 14.6%.)  Also
	; kills the bsr/rts pair, 34 cycles an iteration on its own.
	moveq	#0,d0
	move.b	(SDCOL_HI,a1),d0	; far.col hi
	bne.s	sd_dosub		; far.col > $FF -> subdivide (submid reloads it there)
	move.b	(SDCOL_LO,a1),d0	; d0 = far.col (high byte known 0)
	moveq	#0,d1
	move.b	(SDHGT_HI,a1),d1
	lsl.w	#8,d1
	move.b	(SDHGT_LO,a1),d1	; d1 = far.hgt
	; ---- CASCADE ----
	tst.w	d3			; span.hgt & $8000 ?
	bmi.s	sd_spanlow
	cmp.w	#$6C,d3			; span.hgt < $6C ?
	bcs.s	sd_spanlow
	; spanHIGH: default rasterize
	tst.w	d1			; far.hgt & $8000 ?
	bmi.s	sd_wtFarH		; -> width test (far height)
	cmp.w	#$FF,d1
	bhi.s	sd_doras		; far.hgt > $FF -> rasterize
	cmp.w	#$6C,d1
	bcs.s	sd_wtFarH		; far.hgt < $6C -> width test (far height)
	bra.s	sd_doras		; else rasterize
sd_spanlow:
	; spanLOW: default skip
	tst.w	d1			; far.hgt & $8000 ?
	bmi	sd_pop			; skip
	cmp.w	#$FF,d1
	bhi.s	sd_wtSpanH		; far.hgt > $FF -> width test (span height)
	cmp.w	#$6C,d1
	bcs	sd_pop			; far.hgt < $6C -> skip
	; else: fall through to the width test (span height).  The oracle's `bra sd_wtSpanH`
	; here branched to the very next instruction — 10 cycles for nothing.

	; width/steepness: width = (far.col - span.col) low byte; narrow -> rasterize, else
	; rasterize if the chosen height is shallower than width/4, else subdivide (steep).
sd_wtSpanH:				; useSpanHeight = 1
	sub.w	d2,d0			; far.col - span.col (d0 was far.col)
	and.w	#$FF,d0			; width (byte)
	cmp.w	#$14,d0
	bcs.s	sd_doras		; width < $14 -> rasterize
	lsr.w	#2,d0			; q = width>>2
	move.b	d0,mem+$B5
	move.w	d3,d1			; hgt = span.hgt
	sub.w	d0,d1			; hgt - q
	tst.w	d1
	bpl.s	sd_doras		; shallow -> rasterize
	bra.s	sd_dosub		; steep -> subdivide
sd_wtFarH:				; useSpanHeight = 0 (hgt = far.hgt, still in d1)
	sub.w	d2,d0			; far.col - span.col
	and.w	#$FF,d0			; width
	cmp.w	#$14,d0
	bcs.s	sd_doras
	lsr.w	#2,d0			; q
	move.b	d0,mem+$B5
	sub.w	d0,d1			; far.hgt - q
	tst.w	d1
	bpl.s	sd_doras		; shallow -> rasterize
	; steep: fall through to sd_dosub (the oracle branched to the next instruction)

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
	bls.s	sd_r_noclamp
	tst.w	d3
	bmi.s	sd_r_neg
	or.w	#$00FF,d3
	bra.s	sd_r_noclamp
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
	bls.s	sd_lh_store		; <= $FF -> use low byte
	tst.w	d0
	bmi.s	sd_lh_neg
	moveq	#-1,d1			; > $FF, positive -> $FF
	bra.s	sd_lh_store
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
	; `cmpa.w #0,An` is 10 cycles; a MOVE sets N/Z from its source, and d0 is dead here.
	move.l	a2,d0			; depth == 0 ?
	beq.s	sd_out
	bsr	load_span		; span = subpt_load(depth)
	subq.l	#1,a2			; depth--
	subq.l	#1,a1
	bra	sd_phase3

	; ================= exit =================
sd_out:
	; Flush span.  mem[] is LITTLE-endian (6502: mem[a] = lo) and the 68000 is big-endian,
	; so the {lo at $82, hi at $83} pair is one word store of the BYTE-SWAPPED d2 — 42
	; cycles against the 58 of move.b/move.l/lsr.w #8/move.b.  $82 and $84 are even and
	; mem is aligned(4) (cpu.c), so the word access cannot fault; the flight VBI writes
	; none of $82-$86 (CLAUDE.md ZP write-set), so the wider store races nothing.
	move.w	d2,d0			; flush span
	ror.w	#8,d0
	move.w	d0,mem+$82		; $82 = span.col lo, $83 = hi
	move.w	d3,d0
	ror.w	#8,d0
	move.w	d0,mem+$84		; $84 = span.hgt lo, $85 = hi
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
	ifd	ROF_RASTER_SPAN_ABI
	movem.l	(sp)+,d2-d7/a2-a3	; must mirror the prologue's list exactly (FRM)
	else
	movem.l	(sp)+,d2-d7/a2-a4
	endc
	rts

; ---------------------------------------------------------------------------
; terrain_subdivide_column_obj — the OBJECT-INDEXED entry.
;   uint8_t terrain_subdivide_column_obj(uint8_t obj0)                 [ROF_SUBDIV_OBJ1ARG]
;   uint8_t terrain_subdivide_column_obj(uint8_t startDepth, uint8_t rasterEntryDepth,
;                                        uint8_t obj0)                 [otherwise]
;
; ⚠ The 1-argument form is the SHIPPING ABI (2026-08-09).  Of the three arguments this entry
; used to take, two were never live: `rasterEntryDepth` is dead the moment the rasterizer's
; register ABI exists (terrain_column_rasterize_core_c assigns it to `depth` and then does
; `depth = 0` before any read — and 5172 differential calls agree), and `startDepth` is the
; literal 0x00 at the ONE call site (terrain_draw_objects).  The caller was paying 12 cycles
; for a `move.l` and 20 for a `clr.l -(sp)`, plus 8 more of stack clean-up, so the CALL is 32
; cycles cheaper and the prologue another 32.
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
	ifd	ROF_RASTER_SPAN_ABI
	movem.l	d2-d7/a2-a3,-(sp)	; 8 longs = FRM bytes; args shift +FRM
	else
	movem.l	d2-d7/a2-a4,-(sp)	; 9 longs
	endc
	ifd	ROF_SUBDIV_OBJ1ARG
	; startDepth is the literal 0 at the one call site, so `depth` is a constant here and
	; a1 = mem + depth IS mem — which also makes a0 one `movea.l` off a1 instead of a second
	; 12-cycle `lea mem`.  (The general startDepth still lives in the core entry above.)
	moveq	#0,d0
	movea.l	d0,a2			; a2 = depth = 0
	lea	mem,a1			; a1 = mem + depth
	move.b	(ARG_OBJ0,sp),d0	; obj0
	movea.l	a1,a0
	adda.l	d0,a0			; a0 = mem + obj0
	else
	moveq	#0,d0
	move.b	(ARG_START,sp),d0	; startDepth
	movea.l	d0,a2			; a2 = depth = startDepth
	lea	mem,a1
	adda.l	d0,a1			; a1 = mem + depth
	ifnd	ROF_RASTER_SPAN_ABI
	moveq	#0,d0
	move.b	(ARG_RASENT,sp),d0	; rasterEntryDepth
	movea.l	d0,a4
	endc
	moveq	#0,d0
	move.b	(ARG_OBJ0,sp),d0	; obj0
	lea	mem,a0
	adda.l	d0,a0			; a0 = mem + obj0
	endc

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
	; (word read for the high byte instead of `lsl.w #8` — see the core entry's guard)
	move.b	mem+SDCOL_HI,d1		; far0.col hi (slot 0)
	eori.b	#$80,d1
	move.b	d1,mem+$B5
	move.w	mem+SDCOL_HI,d0		; d0 bits 8-15 = far0.col hi
	move.b	mem+SDCOL_LO,d0		; d0.w = far0.col
	cmp.w	d0,d2			; span.col - far0.col (signed)
	blt.s	sd_obj_go		; span.col < far0.col -> subdivide
	; bail: publish the span, i.e. exactly the 5 bytes the caller used to write
	move.w	d2,d0			; (byte-swapped word stores — see sd_out)
	ror.w	#8,d0
	move.w	d0,mem+$82
	move.w	d3,d0
	ror.w	#8,d0
	move.w	d0,mem+$84
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
	tst.b	d7			; bit 7 of the byte == N after tst.b
	bpl.s	sm_done
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

; (load_far was inlined into sd_inner — its one call site — and split on the high byte.)

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
