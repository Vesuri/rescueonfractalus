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
; 2026-08-11 re-measure (332 iterations, quiet best-case arm): 68.1 calls/iteration, 1.23
; inner iterations, 0.397 midpoints and 0.61 rasterize calls a call — the same shape.  Two
; things came out of what was left: the three `bsr` helpers became the SUBMID / PUSHMID /
; LOADSPAN macros below, and the cascade's far.hgt classification moved onto the HIGH byte
; (see sd_inner).  Together ~4.3k cycles an iteration.
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
;   $82/$83 — dl_lms_push_bottom_core (inlined into dl_doors_open_split_step) read-modify-writes them
;           as the door-scroll cinematic's persistent bottom-LMS pointer, +$2E a step.
;   $84   — alien_field0_fill seeds its bit-packing accumulator from it and its caller does not
;           write it; that is the jump-scare creature overlay, also IN FLIGHT.
;   $8F   — scroll_field_columns_core does `sfx_toggle_8F >>= 1`, a persistent frame toggle.
;   $90/$91 — read by boot_standby_launch_driver / standby_level_select_loop.
;   $9F   — read by draw_ring_frame_step / draw_symmetric_span_loop as a span coordinate their
;           callers do not set.
; Only $86 survived the survey (both readers — dl_write_lms_window and alien_field0_fill — are written
; by their callers immediately before the call), and one byte store is 16 cycles = 0.16% of
; wall, not worth an asymmetric residue contract.
; ⭐ Two lessons.  (1) A C-only grep is NOT the survey: three of the consumers are in hand-asm
; twins or in a helper GCC inlined into its caller, so grep the .s files and the linked
; disassembly too.  (2) "6502 scratch nothing reads" is a hypothesis about a HEAVILY REUSED
; address, and these cells are reused as persistent pointers/toggles by other scenes.
;
; ⭐ DEFER (ROF_SUBDIV_DEFER_RESIDUE, 2026-08-09) — what DEADZP could not do, done the other way.
; DEADZP is about DROPPING the residue and stays closed.  But the nine live consumers it found
; are all OUTSIDE the terrain draw loop, so the residue does not have to be published on every
; call: sd_out and the object entry's guard bail park the span (and, when one ran, the midpoint)
; in g_sdResidue with a single MOVEM.W, and terrain_draw_objects writes it through to mem[] once
; the loop is done.  28 cycles against 100 for the span and 28 against 132 for the midpoint, on
; ~68 calls an iteration.
; The licensing survey (linked shipping image, objdump; the call graph followed jsr/bsr/jmp/Bcc/
; lea plus fall-through between labels, so it over-approximates):
;   * closure of terrain_draw_objects = 104 functions (the rasterizer span entry and all its
;     ras_* blocks, project_terrain_points_core, terrain_plot_object, rof_pokey_random):
;     ZERO readers of $82-$86, and zero of $8D-$91.
;   * closure of vbi_handler_flight = 74 functions: ZERO readers of either range.  (The whole
;     Amiga VBI closure has two — dl_doors_open_split_step reads $82/$83 and dl_write_lms_window reads
;     $84/$86 — but both are reached only via standby_vbi_native -> launch_anim_dispatch_native,
;     the door-scroll cinematic, which cannot run while the flight VBI is installed.)
;   * no indexed, (d16,An) or wide access anywhere in the image can reach either range outside
;     this file: every mem[] access to them is absolute (the generated C has no mem[base+i] with
;     a base below $88), the only (d16,An) displacements of 130-134 / 141-145 are C++ `this` and
;     ExecBase offsets, and nothing reads a word/long at mem+$7E..$81 or mem+$89..$8C spanning in.
;   * terrain_subdivide_column_obj is the ONLY live subdivide entry in the shipping image (one
;     `lea` of it, inside terrain_draw_objects); _core and the 6502 shim have no caller at all.
;   * the loop has no non-local exit that could skip the publish — the only platform call in its
;     whole closure is platform_hw_read (POKEY RANDOM); no spin-wait hook, no rof_check_restart.
; Because the midpoint half is still conditional, terrain_draw_objects must SEED g_sdResidue
; from mem[] before its loop — a call that computes no midpoint relies on the scratch already
; holding what mem[$8D-$91] holds, exactly as the old code relied on mem[] itself.
; ⚠ Deferring would normally blind subdiv_verify on those ten cells, which is the precise trap
; DEADZP fell into.  It does not here: the differential runs the same seed/publish pair around
; each call (see subv_snapshot / subv_capture_and_restore), so all sixteen ZP bytes are still
; compared.  `make SUBDIV_EAGER=1` restores the per-call flush for an A/B.
;
; ⭐ 2026-08-12 — the object entry's GUARD, 38 cycles a call (log §24.3).  Two things, both found
; while pricing the caller's five-byte SubPt-slot-0 seed (see SEED below):
;   (a) under ROF_SUBDIV_OBJ1ARG `depth` is the literal 0, so a1 IS mem — yet the guard addressed
;       mem+SDCOL_HI / mem+$B5 / mem+SDCOL_LO ABSOLUTELY.  abs.l is 16 cycles against (d16,An)'s
;       12; four operands = 16 cycles a call, three lines.  ⭐ The §23 "GCC addresses mem[]
;       absolutely" tax has a hand-asm cousin, and nobody looks for it inside a .s.
;   (b) the guard PROVES things three later blocks re-derive: phase 2 re-tests span.col's sign,
;       phase 3 range-tests it again, and sd_inner RELOADS the two bytes of far0.col the guard
;       left in d0.  sd_obj_go now goes straight to sd_inner_hgt.  ⭐ The two range tests collapse
;       into ONE because the UNSIGNED `span.col < $D8` already implies non-negative (bit 15 set =>
;       >= $8000 > $D8), so the common case is a single CMP/BCS and the out-of-range arms split on
;       the sign at sd_obj_slow.  46 cycles against 80 on the ~80% of calls that skip phase 2;
;       worst case over any mix is still −10 a call, so it cannot be a regression.
;   Joins: sd_inner_hgt (enter with far.col live in d0) and sd_p2body (enter having proved
;   span.col < 0).  subdiv_verify: 0 mismatch / 5147 calls, and 0 / 5136 on a pinned re-run.
;
; ⭐ SEED — the caller's five-byte SubPt slot 0 write, now the callee's job
; (ROF_SUBDIV_OBJ_SEED0, 2026-08-12, log §24).  terrain_draw_objects used to copy obj1's projected
; vector into slot 0 on EVERY visible pair — 5 memory-to-memory MOVE.B at 20 cycles, 68.1 pairs an
; iteration = 1.2% of wall — purely for this file to read back.
; ⚠ It looks like the §10.1 span handoff one level up.  It is NOT, and that distinction is the
; whole design: $82-$86 was a register round trip, but slot 0 is a stack the recursion INDEXES BY
; DEPTH (sd_inner, SUBMID and sd_doras all go back to memory for it).  So a callee that merely
; "loads the seed itself" still has to STORE it and the transfer nets −8 cycles a call.
; ⭐ When a "move it into the callee" candidate targets memory the callee INDEXES rather than
; memory it READS ONCE, price the STORE, not the load.
; What actually wins, and what is implemented here:
;   * obj1 becomes a second argument, and a0 = mem + obj1 for the whole call.
;   * a DUPLICATE depth-0 arm (sd0_*, past sd_dosub) reads the far endpoint out of the object
;     arrays instead of slot 0.  Same instruction costs — (d16,a0) and (d16,a1) are both 12.
;   * slot 0 is materialised LAZILY, by SEED0, on the two edges that leave depth 0 (~0.21 pushes
;     a call instead of 1.0), and terrain_draw_objects publishes it once per pass for the last
;     visible pair.  See THE INVARIANT at sd0_phase2.
; Net −51.5 cycles a call ≈ −0.62% of wall (caller −96, callee +44).  `make SUBDIV_EAGER0=1`
; restores the per-pair seed for an A/B.
; The reader survey that licenses it: across the WHOLE linked image only this file and that seed
; touch $25B4-$25C3 / $25D2-$25E1 / $25F0-$25FF / $24E2-$24F1 / $23E2-$23F1; no `lea` targets
; within $100 below any base, so no indexed base can reach them; and nothing reads a word spanning
; in from $25B3/$25D1/$25EF/$24E1/$23E1.
; ⚠ Three instruments, because no single one covers it:
;   * subdiv_verify — 0 mismatch / 5105 calls.  The harness seeds slot 0 around BOTH arms
;     (SD_SEED0 in rof_native.c) so the oracle keeps its slot-0 view and all 16 entries of all 5
;     stacks stay compared: §17.3's rule, relocate the ORACLE'S VIEW rather than narrow the compare.
;   * make validate FN=terrain_draw_frame — 0 mem mismatch / 2000 cases.  ⚠ The HOST keeps the
;     eager per-pair seed (its C oracle subdivide reads slot 0), so this proves the #ifdef split,
;     not the new path.
;   * make SEED0_VERIFY=1 + amiga/seed0_verify.gdb — 150 passes, 0 mismatch.  The ONLY instrument
;     that can see a wrong once-per-pass publish: nothing reads those cells, so no differential
;     can, and two builds of differing render speed fly different ground within frames (§19) so a
;     cross-build dump cannot either.  It shadows obj1's vector AT THE TIME OF EACH PAIR and
;     compares that to what the publish left — re-reading it at the end would be vacuous.

	xdef	terrain_subdivide_column_core_asm
	ifnd	ROF_SUBDIV_VERIFY
	xdef	terrain_subdivide_column_core		; ships as the core symbol directly
	endif
	xref	mem
	ifd	ROF_SUBDIV_DEFER_RESIDUE
	xref	g_sdResidue		; rof_native.c — the deferred exit residue, see DEFER above
	endc
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

; The per-object projected-vector arrays (indexed by object id).  Slot 0 of each SubPt stack is
; a COPY of obj1's entry in these — see SEED below.  Under ROF_SUBDIV_OBJ_SEED0 the depth-0 arm
; reads the far endpoint from here (base a0 = mem + obj1) and the copy is materialised lazily.
OCOL_LO		equ	$2400
OCOL_HI		equ	$242D
OHGT_LO		equ	$245A
OHGT_HI		equ	$2487
OFRAC		equ	$23B5

; ---------------------------------------------------------------------------
; The three helpers below used to be `bsr`-called subroutines.  Every one of them is short,
; leaf, and called from at most two places, so the `bsr`+`rts` pair (18 + 16 = 34 cycles) was a
; third of what some of them did.  They are MACROS now, expanded at their call sites — one
; source copy each, so there is still exactly one place to change the arithmetic.
; Shape (amiga/ras_shape.gdb, 2026-08-11, quiet best-case arm, 332 iterations): 27.0 SUBMID and
; 14.4 PUSHMID expansions an iteration (0.397 midpoints/call over 68.1 calls/it, and only 53% of
; midpoints are pushed — the rest are adopted as the near endpoint in phase 2), plus 8.1 for the
; pop's span load ⇒ 49.5 x 34 = ~1.7k cycles an iteration of pure call overhead removed.
; ⚠ Local labels inside a macro MUST carry `\@` (vasm's per-expansion counter) or the second
; expansion redefines the first's.

; SUBMID <base>,<colLo>,<colHi>,<hgtLo>,<hgtHi>,<frac> — mid = subdiv_midpoint(span, far).
; Reads span (d2/d3/d4), loads the far endpoint from (base) at the given displacements, writes
; mid (d5/d6/d7) and (on roughness) $B5/$B6.  Clobbers d0/d1.
; midCol = signed-avg = asr.w of (span.col+far.col+1); likewise midHgt; fracSum 9-bit.
; ⚠ The base is a PARAMETER because the depth-0 arm reads far out of obj1's object arrays
; (a0 + OCOL_*) while every deeper level reads slot `depth` of the stacks (a1 + SD*).  Same
; instruction costs either way — (d16,An) is 12 cycles for both — so this is pure plumbing.
SUBMID	macro
	moveq	#0,d0
	move.b	(\3,\1),d0
	lsl.w	#8,d0
	move.b	(\2,\1),d0		; d0 = far.col
	move.w	d2,d5
	add.w	d0,d5
	addq.w	#1,d5
	asr.w	#1,d5			; d5 = mid.col
	moveq	#0,d0
	move.b	(\5,\1),d0
	lsl.w	#8,d0
	move.b	(\4,\1),d0		; d0 = far.hgt
	move.w	d3,d6
	add.w	d0,d6
	addq.w	#1,d6
	asr.w	#1,d6			; d6 = mid.hgt (pre-roughness)
	moveq	#0,d0
	move.b	(\6,\1),d0		; far.frac
	move.w	d4,d1
	and.w	#$FF,d1			; span.frac byte
	add.w	d0,d1
	addq.w	#1,d1			; d1 = fracSum (0..511)
	move.b	d1,d7			; mid.frac = low byte
	tst.b	d7			; bit 7 of the byte == N after tst.b
	bpl.s	.smdone\@
	; roughness: disp = (uint16)(mid.col - span.col) >> 1
	move.w	d5,d0
	sub.w	d2,d0
	lsr.w	#1,d0			; d0 = disp
	btst	#8,d1			; fracSum >= $100 ?
	beq.s	.smdown\@
	add.w	d0,d6			; midHgt += disp
	bra.s	.smwrb\@
.smdown\@:
	sub.w	d0,d6			; midHgt -= disp
.smwrb\@:
	move.b	d0,mem+$B5
	lsr.w	#8,d0
	move.b	d0,mem+$B6
.smdone\@:
	endm

; PUSHMID — subpt_store(depth+1, mid): write mid (d5/d6/d7) to slot depth+1 (a1+1).
; Clobbers d0.
PUSHMID	macro
	move.b	d5,(SDCOL_LO+1,a1)
	move.l	d5,d0
	lsr.w	#8,d0
	move.b	d0,(SDCOL_HI+1,a1)
	move.b	d6,(SDHGT_LO+1,a1)
	move.l	d6,d0
	lsr.w	#8,d0
	move.b	d0,(SDHGT_HI+1,a1)
	move.b	d7,(SDFRAC+1,a1)
	endm

	ifd	ROF_SUBDIV_OBJ_SEED0
; SEED0 — materialise SubPt slot 0 from obj1's projected vector (a0 = mem + obj1, a1 = mem
; because this only ever runs at depth 0).  These are the five memory-to-memory MOVE.B the
; CALLER used to run on every visible pair; here they run only when the recursion is about to
; leave depth 0, which the shape says is ~0.21 pushes a call rather than 1.0.  See SEED above.
SEED0	macro
	move.b	(OCOL_LO,a0),(SDCOL_LO,a1)
	move.b	(OCOL_HI,a0),(SDCOL_HI,a1)
	move.b	(OHGT_LO,a0),(SDHGT_LO,a1)
	move.b	(OHGT_HI,a0),(SDHGT_HI,a1)
	move.b	(OFRAC,a0),(SDFRAC,a1)
	endm
	endc

; LOADSPAN — span (d2/d3/d4) = subpt_load(depth) from slot depth (a1).  One call site (the pop).
LOADSPAN	macro
	moveq	#0,d2
	move.b	(SDCOL_HI,a1),d2
	lsl.w	#8,d2
	move.b	(SDCOL_LO,a1),d2
	moveq	#0,d3
	move.b	(SDHGT_HI,a1),d3
	lsl.w	#8,d3
	move.b	(SDHGT_LO,a1),d3
	move.b	(SDFRAC,a1),d4
	endm

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
ARG_OBJ0	equ	7+FRM		; obj0 (the first argument)
	else
ARG_OBJ0	equ	15+FRM		; obj0 (the object-indexed entry only)
	endc
; ROF_SUBDIV_OBJ_SEED0 appends obj1.  Read as a WORD from the int-promoted slot's low half:
; the arg is 4 big-endian bytes 00 00 00 obj1, so a `movea.w` of the last two lands obj1 in the
; register already zero-extended (and obj1 <= $8E, so the sign extension is a no-op).
; obj1's BYTE sits at ARG_OBJ0+4 (the next int-promoted slot's last byte); the word that holds
; it in its low half therefore starts one lower, at ARG_OBJ0+3 — which is EVEN whenever
; ARG_OBJ0 is 7+FRM, as it must be for a word read not to take an address error.
ARG_OBJ1	equ	ARG_OBJ0+3	; low word of the next int-promoted slot

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
sd_p2body:				; entry for a caller that has ALREADY proved span.col < 0
	subq.l	#1,a3			; budget--
	cmpa.w	#-1,a3
	beq	sd_out			; budget was 0 -> exhausted
	SUBMID	a1,SDCOL_LO,SDCOL_HI,SDHGT_LO,SDHGT_HI,SDFRAC  ; mid = midpoint(span, far@depth)
	cmp.w	#$28,d5			; (int16)mid.col < $28 ?
	bge	sd_p2push
	move.w	d5,d2			; adopt mid as span (near midpoint)
	move.w	d6,d3
	move.b	d7,d4
	bra	sd_phase2
sd_p2push:
	PUSHMID				; store mid at depth+1
	addq.l	#1,a2			; depth++
	addq.l	#1,a1
	cmpa.w	#$0F,a2
	bcc	sd_out			; depth >= $0F -> stack full
	bra	sd_phase2

	; sd_dosub itself now lives past sd_ret, because inlining SUBMID + PUSHMID into it made it
	; ~140 bytes and it used to sit between the width test and sd_doras — far enough to push
	; sd_doras out of `.s` reach of every branch that targets it.  Only sd_inner's far.col
	; escape wants it CLOSE, and that one is 12.1 of 83.6 inner iterations, so it goes through
	; this trampoline: the 71.5 that fall through keep their 8-cycle short branch, and the
	; escape pays 10 for the extra `bra` instead of all 71.5 paying 4 for a word branch.
sd_dosubT:
	bra	sd_dosub

	; ---- cold: far.hgt's high byte is non-zero (8.3% of leaves) ----
	; Out of line, and placed BEFORE sd_inner rather than after: dropping it into the middle
	; of the leaf code pushed sd_doras out of every width test's `.s` reach (5 assembler
	; errors' worth), and the entry branch reaches back here perfectly well.
	; It keeps N from sd_inner's `move.b` — a Bcc does not touch the CCR — so the sign test
	; costs one branch and no reload.
sd_fhWide:
	bmi.s	sd_fhNeg
	; far.hgt > $FF, positive.  ZERO of 23261 measured leaves reach here — it exists so the
	; twin stays byte-identical to the oracle's `far.hgt > $FF` arm.
	tst.w	d3			; span.hgt & $8000 ?
	bmi	sd_wtSpanH		; spanLOW -> width test (span height)
	cmp.w	#$6C,d3
	bcs	sd_wtSpanH		; spanLOW -> width test (span height)
	bra	sd_doras		; spanHIGH -> rasterize
sd_fhNeg:
	; far.hgt < 0.  spanLOW skips (the 90% case, and it never needs the value); spanHIGH runs
	; the width test on the far height, which is the ONE consumer of the assembled 16-bit
	; far.hgt — so this is the only path that still pays the `lsl.w #8`.
	tst.w	d3			; span.hgt & $8000 ?
	bmi	sd_pop			; spanLOW -> skip
	cmp.w	#$6C,d3
	bcs	sd_pop			; spanLOW -> skip
	lsl.w	#8,d1
	move.b	(SDHGT_LO,a1),d1	; d1 = far.hgt, assembled at last
	bra	sd_wtFarH		; -> width test (far height)

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
	; byte.  (Shape: the escape fires on 4023 of 27754 inner iterations = 14.5%.)  Also
	; kills the bsr/rts pair, 34 cycles an iteration on its own.
	moveq	#0,d0
	move.b	(SDCOL_HI,a1),d0	; far.col hi
	bne.s	sd_dosubT		; far.col > $FF -> subdivide (SUBMID reloads it there)
	move.b	(SDCOL_LO,a1),d0	; d0 = far.col (high byte known 0)
sd_inner_hgt:				; entry with d0.w = far.col ALREADY loaded, high byte 0
	; far.hgt gets the SAME high-byte-first treatment, and here it pays three times over: all
	; THREE tests the cascade makes on far.hgt are answered by the high byte alone —
	; negative <=> hi & $80,  > $FF <=> hi != 0 (and not negative),  < $6C <=> hi == 0 &&
	; lo < $6C.  So on the common path the 22-cycle `lsl.w #8` disappears AND the sign and
	; > $FF tests disappear with it, leaving one compare.  The assembled 16-bit value is
	; needed by exactly one consumer, sd_wtFarH's `far.hgt - q`, reached only when far.hgt is
	; negative — so that assembly moves into the cold block below.
	; Shape (amiga/ras_shape.gdb, 2026-08-11, quiet best-case arm, 23261 leaf cascades):
	; far.hgt has hi == 0 on 21328 = 91.7%, is negative on 1933 = 8.3%, and is > $FF positive
	; on ZERO — that arm survives for byte-identity, not for the profile.  Of the 8.3%, 90%
	; (spanLOW) need only the SIGN and never assemble the value at all.
	moveq	#0,d1
	move.b	(SDHGT_HI,a1),d1	; far.hgt hi.  Z = (hi == 0), N = (far.hgt < 0)
	bne.s	sd_fhWide		; > $FF or negative -> the cold block
	move.b	(SDHGT_LO,a1),d1	; d1 = far.hgt, known 0..$FF (hi is 0, so no shift)
	; ---- CASCADE, far.hgt in [0,$FF] ----
	; Neither `far.hgt & $8000` nor `far.hgt > $FF` can fire, so both collapse out and the
	; whole cascade is the $6C compare against the chosen height.
	tst.w	d3			; span.hgt & $8000 ?
	bmi.s	sd_fhLoSpanLow
	cmp.w	#$6C,d3			; span.hgt < $6C ?
	bcs.s	sd_fhLoSpanLow
	; spanHIGH: default rasterize
	cmp.w	#$6C,d1
	bcs.s	sd_wtFarH		; far.hgt < $6C -> width test (far height)
	bra.s	sd_doras		; else rasterize
sd_fhLoSpanLow:
	; spanLOW: default skip
	cmp.w	#$6C,d1
	bcs	sd_pop			; far.hgt < $6C -> skip
	; else: fall through to the width test (span height).  The oracle's `bra sd_wtSpanH`
	; here branched to the very next instruction — 10 cycles for nothing.

	; width/steepness: width = (far.col - span.col) low byte; narrow -> rasterize, else
	; rasterize if the chosen height is shallower than width/4, else subdivide (steep).
	; The two variants were separate blocks that differed in ONE instruction — which height
	; lands in d1 — so useSpanHeight is now just an extra entry point above the shared body.
	; Hoisting `move.w d3,d1` above the width test is invisible: nothing between the old
	; position and here touches d1 or d3, and on this entry far.hgt (d1's old value) is dead
	; either way (the early `width < $14` exit goes to sd_doras, which does not read d1).
sd_wtSpanH:				; useSpanHeight = 1
	move.w	d3,d1			; hgt = span.hgt
sd_wtFarH:				; useSpanHeight = 0 (hgt = far.hgt, already in d1)
	sub.w	d2,d0			; far.col - span.col (d0 was far.col)
	and.w	#$FF,d0			; width (byte)
	cmp.w	#$14,d0
	bcs.s	sd_doras		; width < $14 -> rasterize
	lsr.w	#2,d0			; q = width>>2
	move.b	d0,mem+$B5
	sub.w	d0,d1			; hgt - q
	tst.w	d1
	bpl.s	sd_doras		; shallow -> rasterize
	bra	sd_dosub		; steep -> subdivide


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
	ifd	ROF_SUBDIV_OBJ_SEED0
	bra.s	sd_lh_clamp		; the depth-0 copy of the four loads sits between us and it
	endc
	ifd	ROF_SUBDIV_OBJ_SEED0
	; ---- sd_doras for depth 0: the same four control-point loads, out of obj1's arrays.
	; It sits HERE, immediately above the shared tail, so the COMMON (depth-0) path falls
	; through and the rarer generic one pays the 10-cycle `bra.s sd_lh_clamp` above.
sd0_doras:
	cmp.w	#$FF,d3			; clamp span.hgt to a byte if >$FF
	bls.s	sd0_r_noclamp
	tst.w	d3
	bmi.s	sd0_r_neg
	or.w	#$00FF,d3
	bra.s	sd0_r_noclamp
sd0_r_neg:
	and.w	#$FF00,d3
sd0_r_noclamp:
	move.b	(OCOL_LO,a0),mem+$95
	move.b	(OFRAC,a0),mem+$F4
	moveq	#0,d0
	move.b	(OHGT_HI,a0),d0
	lsl.w	#8,d0
	move.b	(OHGT_LO,a0),d0		; d0 = leaf.hgt  -> falls into the shared tail
	endc
sd_lh_clamp:				; SHARED tail: clamp leaf.hgt into $EA, then rasterize
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
	LOADSPAN			; span = subpt_load(depth)
	subq.l	#1,a2			; depth--
	subq.l	#1,a1
	bra	sd_phase3

	; ================= exit =================
sd_out:
	ifd	ROF_SUBDIV_DEFER_RESIDUE
	; Park the span in g_sdResidue — see the DEFER note at the top of this file.  MOVEM.W
	; register->memory stores the LOW WORD of each register in the 68000's native order, so
	; the two 22-cycle `ror.w #8` byte swaps disappear along with two of the three stores:
	; 16+4*3 = 28 cycles against 100, for a value nothing reads until the pass ends.
	movem.w	d2-d4,g_sdResidue	; [0] span.col  [1] span.hgt  [2] span.frac (hi byte dirty)
	else
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
	endc
	; Flush mid ($8D-$91) only if one was actually computed.  The budget is the flag for
	; free: it is set to $14 after the entry guard and decremented ONCE immediately before
	; each of the two SUBMID sites, so `budget != $14` <=> at least one midpoint ran.
	; (The exhaustion exit needs 21 decrements, so it implies ~20 midpoints — still dirty.)
	; When it is clean, d5/d6/d7 hold the CALLER's registers, not a midpoint — and the
	; residue (mem[], or g_sdResidue under DEFER) already holds what the oracle would write
	; back, so skipping the flush is what makes dropping the entry load correct.
	cmpa.w	#$14,a3
	beq.s	sd_out_nomid
	ifd	ROF_SUBDIV_DEFER_RESIDUE
	movem.w	d5-d7,g_sdResidue+6	; [3] mid.col  [4] mid.hgt  [5] mid.frac — 28 cyc vs 132
	else
	move.b	d5,mem+$8D		; flush mid
	move.l	d5,d0
	lsr.w	#8,d0
	move.b	d0,mem+$8E
	move.b	d6,mem+$8F
	move.l	d6,d0
	lsr.w	#8,d0
	move.b	d0,mem+$90
	move.b	d7,mem+$91
	endc
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

	; ================= descend one level (the inner loop's subdivide arm) =================
	; Past the exit on purpose: with SUBMID + PUSHMID expanded in line this block is ~140
	; bytes, and everything it used to separate (the width test and sd_doras) wants to reach
	; across it with a short branch.  Nothing falls through to it — the width test's steep
	; exit and sd_dosubT both branch here — and its own three exits are backward word
	; branches to sd_out / sd_inner, which cost the same as forward ones.
sd_dosub:
	subq.l	#1,a3			; budget--
	cmpa.w	#-1,a3
	beq	sd_out
	SUBMID	a1,SDCOL_LO,SDCOL_HI,SDHGT_LO,SDHGT_HI,SDFRAC  ; mid = midpoint(span, far@depth)
	PUSHMID				; store mid at depth+1
	addq.l	#1,a2			; depth++
	addq.l	#1,a1
	cmpa.w	#$0F,a2
	bcc	sd_out			; depth >= $0F
	bra	sd_inner		; continue inner loop

	ifd	ROF_SUBDIV_OBJ_SEED0
; ===========================================================================================
; ============ THE DEPTH-0 ARM (ROF_SUBDIV_OBJ_SEED0) =======================================
; A second copy of phase 2 + the leaf pass whose ONLY difference is where the far endpoint
; comes from: obj1's object arrays via a0 (= mem + obj1) instead of slot 0 of the stacks via
; a1.  See SEED in the file header for why this exists — it lets terrain_draw_objects stop
; copying those five bytes into slot 0 on every visible pair (5 x 20 cyc, 68.1 pairs an
; iteration).  Per-instruction cost is identical: (d16,a0) and (d16,a1) are both 12 cycles.
;
; ⚠ THE INVARIANT: slot 0 is STALE (it holds the previous visible pair's obj1) for as long as
; control is inside this arm.  Every edge that LEAVES depth 0 therefore runs SEED0 first, and
; from then on the generic a1-based code is correct — including the pop back to depth 0, which
; finds slot 0 already materialised.  There are exactly two such edges, sd0_p2push and
; sd0_dosub, and both are just `SEED0` + a branch into their generic counterpart.
;
; ⚠ a0 must survive the whole arm.  It does without touching the movem, because a0 is
; caller-saved in the m68k C ABI and the only call in here is the rasterize at the tail of
; sd0_doras — after which depth is still 0, so sd_pop exits immediately and nothing reads a0.
; SUBMID/PUSHMID clobber only d0/d1.
;
; ⚠ It lives past sd_dosub, not next to the generic cascade: dropping 340 bytes in front of
; sd_doras put it out of `.s` reach of all three of the generic width test's exits (vasm's
; "branch destination out of range" is the normal feedback for a code-size change under
; -no-opt).  The price is that this copy's own three branches to sd0_doras — which has to sit
; immediately above the shared tail so the COMMON depth-0 path falls through — are word-sized,
; ~2 cycles a call.
; Nothing here may fall through into the generic blocks: every exit is explicit.
; ===========================================================================================
sd0_phase2:
	tst.w	d2			; span.col & $8000 ?
	bpl	sd0_phase3		; non-negative -> start leaf pass
sd0_p2body:				; entry for a caller that has ALREADY proved span.col < 0
	subq.l	#1,a3			; budget--
	cmpa.w	#-1,a3
	beq	sd_out			; budget was 0 -> exhausted (slot 0 never needed)
	SUBMID	a0,OCOL_LO,OCOL_HI,OHGT_LO,OHGT_HI,OFRAC
	cmp.w	#$28,d5			; (int16)mid.col < $28 ?
	bge	sd0_p2push
	move.w	d5,d2			; adopt mid as span (near midpoint) — still depth 0
	move.w	d6,d3
	move.b	d7,d4
	bra	sd0_phase2
sd0_p2push:
	SEED0				; leaving depth 0 -> materialise slot 0, then go generic
	bra	sd_p2push
sd0_dosubT:
	bra	sd0_dosub

	; ---- cold: far.hgt's high byte is non-zero.  Placed before sd0_inner for the same
	; `.s`-reach reason the generic sd_fhWide is (see there). ----
sd0_fhWide:
	bmi.s	sd0_fhNeg
	tst.w	d3			; span.hgt & $8000 ?
	bmi	sd0_wtSpanH		; spanLOW -> width test (span height)
	cmp.w	#$6C,d3
	bcs	sd0_wtSpanH		; spanLOW -> width test (span height)
	bra	sd0_doras		; spanHIGH -> rasterize
sd0_fhNeg:
	tst.w	d3			; span.hgt & $8000 ?
	bmi	sd_pop			; spanLOW -> skip (depth 0 => sd_pop exits)
	cmp.w	#$6C,d3
	bcs	sd_pop			; spanLOW -> skip
	lsl.w	#8,d1
	move.b	(OHGT_LO,a0),d1		; d1 = far.hgt, assembled at last
	bra	sd0_wtFarH		; -> width test (far height)

sd0_phase3:
	cmp.w	#$D8,d2			; span.col >= $D8 ? (unsigned — subsumes the sign test)
	bcc	sd_out
sd0_inner:
	moveq	#0,d0
	move.b	(OCOL_HI,a0),d0		; far.col hi
	bne.s	sd0_dosubT		; far.col > $FF -> subdivide (SUBMID reloads it there)
	move.b	(OCOL_LO,a0),d0		; d0 = far.col (high byte known 0)
sd0_inner_hgt:				; entry with d0.w = far.col ALREADY loaded, high byte 0
	moveq	#0,d1
	move.b	(OHGT_HI,a0),d1		; far.hgt hi.  Z = (hi == 0), N = (far.hgt < 0)
	bne.s	sd0_fhWide		; > $FF or negative -> the cold block
	move.b	(OHGT_LO,a0),d1		; d1 = far.hgt, known 0..$FF (hi is 0, so no shift)
	tst.w	d3			; span.hgt & $8000 ?
	bmi.s	sd0_fhLoSpanLow
	cmp.w	#$6C,d3			; span.hgt < $6C ?
	bcs.s	sd0_fhLoSpanLow
	cmp.w	#$6C,d1			; spanHIGH: default rasterize
	bcs.s	sd0_wtFarH		; far.hgt < $6C -> width test (far height)
	bra	sd0_doras		; else rasterize (word: sd0_doras sits up by the shared tail)
sd0_fhLoSpanLow:
	cmp.w	#$6C,d1			; spanLOW: default skip
	bcs	sd_pop			; far.hgt < $6C -> skip
sd0_wtSpanH:				; useSpanHeight = 1
	move.w	d3,d1			; hgt = span.hgt
sd0_wtFarH:				; useSpanHeight = 0 (hgt = far.hgt, already in d1)
	sub.w	d2,d0			; far.col - span.col (d0 was far.col)
	and.w	#$FF,d0			; width (byte)
	cmp.w	#$14,d0
	bcs	sd0_doras		; width < $14 -> rasterize
	lsr.w	#2,d0			; q = width>>2
	move.b	d0,mem+$B5
	sub.w	d0,d1			; hgt - q
	tst.w	d1
	bpl	sd0_doras		; shallow -> rasterize
sd0_dosub:
	SEED0				; leaving depth 0 -> materialise slot 0, then go generic
	bra	sd_dosub
	endc

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
;
; ⭐ ROF_SUBDIV_OBJ_SEED0 (2026-08-12, log §24.2) adds a SECOND argument, obj1 — the companion
; endpoint, i.e. whose projected vector the caller used to copy into SubPt slot 0.  a0 is
; repointed at `mem + obj1` once the span is loaded and stays there for the whole call, which is
; what lets the depth-0 arm read the far endpoint without slot 0 existing yet.  See SEED in the
; file header, and THE INVARIANT at sd0_phase2.
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

	ifd	ROF_SUBDIV_OBJ_SEED0
	; obj0's span is in registers now, so a0 is free: repoint it at obj1's arrays, where the
	; depth-0 far endpoint lives.  20 cycles, and it replaces the caller's 12-cycle `lea` for
	; the same pointer as well as its five 20-cycle copies.
	movea.w	(ARG_OBJ1,sp),a0	; obj1, zero-extended by the word read
	adda.l	a1,a0			; a0 = mem + obj1   (a1 == mem: depth is 0 here)

	; --- entry guard: $B5 = (far0.col>>8)^$80; bail if span.col >= far0.col
	; far0.col comes out of obj1's arrays, not slot 0.  The core entry's `move.w` trick is not
	; available here — OCOL_HI is odd, so `mem + $242D + obj1` is only even for odd obj1 and a
	; word read would fault on half the objects — but the branch on `hi == 0` is CHEAPER than
	; the trick anyway on the 85% where it holds (sd_inner measures far.col hi != 0 at 14.5%):
	; 52 cycles against 56, and $B5 is then the constant $80.
	moveq	#0,d0
	move.b	(OCOL_HI,a0),d0		; far0.col hi.  Z = (hi == 0)
	bne.s	sd_obj_hiNZ		; (out of line past sd_obj_slow, so the 85% falls through)
	move.b	(OCOL_LO,a0),d0		; d0.w = far0.col (hi is 0)
	move.b	#$80,($B5,a1)		; $B5 = hi ^ $80 = $80
sd_obj_guarded:
	else
	; --- entry guard: $B5 = (far0.col>>8)^$80; bail if span.col >= far0.col
	; (word read for the high byte instead of `lsl.w #8` — see the core entry's guard)
	ifd	ROF_SUBDIV_OBJ1ARG
	; Under the 1-arg ABI `depth` is the literal 0, so a1 IS mem and every operand here folds
	; from absolute-long (16 cycles) to (d16,a1) (12) — four of them, 16 cycles a call.  The
	; core entry above cannot do this: there a1 = mem + startDepth.
	move.b	(SDCOL_HI,a1),d1	; far0.col hi (slot 0)
	eori.b	#$80,d1
	move.b	d1,($B5,a1)
	move.w	(SDCOL_HI,a1),d0	; d0 bits 8-15 = far0.col hi
	move.b	(SDCOL_LO,a1),d0	; d0.w = far0.col
	else
	move.b	mem+SDCOL_HI,d1		; far0.col hi (slot 0)
	eori.b	#$80,d1
	move.b	d1,mem+$B5
	move.w	mem+SDCOL_HI,d0		; d0 bits 8-15 = far0.col hi
	move.b	mem+SDCOL_LO,d0		; d0.w = far0.col
	endc
	endc
	cmp.w	d0,d2			; span.col - far0.col (signed)
	blt.s	sd_obj_go		; span.col < far0.col -> subdivide
	; bail: publish the span, i.e. exactly the 5 bytes the caller used to write
	ifd	ROF_SUBDIV_DEFER_RESIDUE
	movem.w	d2-d4,g_sdResidue	; deferred like sd_out's; the mid slot stays as it was
	else
	move.w	d2,d0			; (byte-swapped word stores — see sd_out)
	ror.w	#8,d0
	move.w	d0,mem+$82
	move.w	d3,d0
	ror.w	#8,d0
	move.w	d0,mem+$84
	move.b	d4,mem+$86
	endc
	bra	sd_ret
sd_obj_go:
	movea.w	#$14,a3			; budget = $14
	; ⭐ Straight into the leaf pass, with far0.col carried over in d0.
	; The old `bra sd_phase2` walked three blocks that between them re-derive what the guard
	; just proved: phase 2 re-tests span.col's sign, phase 3 range-tests it again, and
	; sd_inner RELOADS the very two bytes of far0.col the guard has sitting in d0.
	; The two range tests collapse into one: `span.col < $D8` UNSIGNED already implies
	; non-negative (anything with bit 15 set is >= $8000 > $D8), so the common case —
	; span.col in [0,$D7], i.e. exactly what phase 2 forwards to phase 3 and phase 3 accepts —
	; is a single CMP/BCS pair, and the two out-of-range arms split on the sign afterwards.
	; sd_inner's far.col escape (`far.col hi != 0`) is `far.col > $FF` unsigned on the value
	; already in d0.  46 cycles against 80, on the ~80% of calls that never enter phase 2.
	; ⚠ d0's upper WORD is dirty (the guard's `move.w`); every consumer past sd_inner_hgt
	; reads it as .w or .b only — same precondition the guard already documents.
	; ⚠ Under ROF_SUBDIV_OBJ_SEED0 all three targets are the DEPTH-0 arm's: slot 0 does not
	; hold obj1 yet, so the generic blocks would read the previous pair's far endpoint.
	cmp.w	#$D8,d2			; span.col < $D8 UNSIGNED ?
	bcc.s	sd_obj_slow		; no -> negative, or phase 3's own exit
	cmp.w	#$FF,d0			; far.col > $FF ?  (== sd_inner's `far.col hi != 0`)
	ifd	ROF_SUBDIV_OBJ_SEED0
	bhi	sd0_dosub		; yes -> subdivide (SEED0 there, then generic)
	bra	sd0_inner_hgt		; d0.w = far.col, high byte known 0
	else
	bhi	sd_dosub		; yes -> subdivide (SUBMID reloads far there)
	bra	sd_inner_hgt		; d0.w = far.col, high byte known 0
	endc
sd_obj_slow:
	tst.w	d2			; span.col & $8000 ?
	ifd	ROF_SUBDIV_OBJ_SEED0
	bmi	sd0_p2body		; negative -> the depth-0 descend loop (past its own re-test)
	else
	bmi	sd_p2body		; negative -> phase 2's descend loop (past its own re-test)
	endc
	bra	sd_out			; >= $D8 and positive -> phase 3's exit

	ifd	ROF_SUBDIV_OBJ_SEED0
	; far0.col's high byte is non-zero — 14.5% by sd_inner's count.  Out of line so the common
	; path falls straight through the guard instead of paying a 10-cycle `bra` over this.
sd_obj_hiNZ:
	move.b	d0,d1
	eori.b	#$80,d1
	move.b	d1,($B5,a1)
	lsl.w	#8,d0
	move.b	(OCOL_LO,a0),d0		; d0.w = far0.col
	bra	sd_obj_guarded
	endc

; (load_far, submid, push_mid and load_span are all gone as CALLABLE routines: each was inlined
; at its call sites — load_far directly into sd_inner, the other three as the SUBMID / PUSHMID /
; LOADSPAN macros at the top of this file — killing 34 cycles of bsr+rts a piece.)
