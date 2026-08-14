; SfxMixerAssembler.s — hand-written m68k twin of the SFX voice-priority mixer chain
; (asm-migration-plan Phase 6).  Plain C linkage, 6502-ABI void/void like the C twin:
;   void sfx_reorder_voice_slot(void)      ; reads cpu.Y (the voice slot); cpu.X is NOT an input
;
; WHY THIS FUNCTION.  It is not a rendering routine — it is the biggest single item inside the
; 50 Hz flight VBI, which fires 50x/s regardless of frame rate and is therefore a flat tax on
; ALL wall clock (~31% of it).  Measured under `make COMBAT=1`: the ISR is
; ~96 t/firing, its sfx block 43 t, the event-ring drain 27.7 t, and sfx_reorder_voice_slot
; alone 22.4 t/firing = 3.32 calls x 6.70 t.  The drain calls it once per active duration
; envelope, and each call genuinely re-decides voice priority (the intermediate $0714/$0715
; feed the next pass), so the passes cannot be collapsed — only made cheaper.
;
; ⚠⚠ HOW MUCH IS ACTUALLY ON THE TABLE — read this before "improving" anything here.
; The leaf split (floor-corrected SX_SPAN; the bracket's own ~0.29 t subtracted) of the
; 6.70 t/call is:
;   2.95 t (44%)  the two rof_pokey_write calls inside voice_write_freq_ctrl  <- NOT ours:
;                 that is the change-detect + update_paula_channel recompute, in C++
;   1.05 t (16%)  voice_write_freq_ctrl's OWN mem[] loads + call plumbing
;   1.04 t (16%)  sfx_pick_next_voice   (12-slot scan)
;   0.90 t (13%)  sfx_pick_top_voice    (12-slot scan)
;   0.85 t (13%)  sfx_reorder_voice_slot's own compare/move logic
; So only 3.84 t/call (57%) is asm-addressable at all, and the FIRST attempt at this twin came
; out 5% SLOWER than the C.  The reason is worth remembering: GCC is NOT leaving much here.
; It fully inlines AND fully unrolls both 12-slot scans into straight-line absolute-addressed
; code — `move.b (mem+$0706).l,d1` (16 cyc) + `beq.s` (10) = 26 cycles for an inactive slot.
; A "clean" pointer-walked loop (`tst.b (a0)+` 8 + `beq.s` 10 + `addq.l #1,a1` 8 + `dbra` 10 =
; 36) is 10 cycles per slot WORSE, because on the 68000 the 18 cycles of loop bookkeeping cost
; more than the 8 cycles that (a0)+ saves over absolute addressing.  ⚠ This is the limit of the
; CLAUDE.md "pointer-walk with autoincrement" rule: that rule is about killing a mulu+index in
; a loop, NOT about beating an unrolled absolute scan.  So this twin UNROLLS too (below), and
; keeps only the parts where hand-asm genuinely wins:
;   - a 5-register movem instead of 10 (the v1 mistake: 180 cycles of prologue/epilogue against
;     GCC's 68 — that handicap alone was bigger than everything the twin saved),
;   - `tst.b (a0)+` for the per-slot active test, which is the ONE place autoincrement wins once
;     the loop bookkeeping is gone (18 cyc/slot vs GCC's 26),
;   - the four table bases pinned in address registers, so every mem[] and cpu access is a
;     12-cycle d(An) instead of a 16-cycle absolute,
;   - write_freq_ctrl inlined as a local bsr with its arguments already in registers, instead of
;     GCC's real `jsr sfx_voice_write_freq_ctrl` with its own prologue and cpu-struct stores.
; Realistic remaining headroom after this: ~0.5% of all wall clock.  Do not expect the 2-3%
; an earlier estimate assumed — that estimate credited the whole 21.8 t/firing to mixer logic,
; before the leaf split showed 44% of it is the POKEY/Paula write path.
;
; Byte-identical to the C twin (sfx_reorder_voice_slot_c in rof_native.c) — verified by the
; in-process differential (make VERIFY=1 PROBES=1 + amiga/sfxmix_verify.gdb): each call runs
; this twin + the C twin back-to-back on the same state and compares mem[$0705-$0717] (the
; voice reg-idx table + the four scan cells), the POKEY mirror mem[$D200-$D20F], and cpu.A/X/Y.
; The C twin in turn is byte-compared against the $5614 transliteration by `make validate
; FN=sfx` (4 sfx fixtures at 20000 cases each + the sfx_voice_envelope_tick apex).
;
; FAITHFULNESS NOTES (each is a place where a "cleaner" asm would silently diverge):
;  - bus_write's hardware range test is reproduced exactly.  The POKEY writes are
;    bus_write($D1FE+x) and bus_write($D1FF+x) with x = mem[$0705+y] an arbitrary BYTE, and
;    bus.h only routes $D200-$D20F to rof_pokey_write (everything else in $D000-$D7FF is
;    dropped WITHOUT touching mem[]).  So AUDF lands only for x in [2,17] and AUDC only for
;    x in [1,16] — hence the `cmp.w #16 / bcc` guard on each, not an unconditional call.
;  - cpu.A is computed even when the AUDC write is dropped (the C assigns cpu.A before
;    bus_write), and cpu.X is set by write_freq_ctrl BEFORE its x==0 early-out.
;  - ⚠ THE ENTRY cpu.X IS NOT AN INPUT.  `5619 TXA; 561a BNE 5641` tests X as the JSR at 5616
;    LEFT it — i.e. x = mem[$0705+y], the voice's POKEY register index — so the two-way branch
;    is "is this voice slot ACTIVE?", not "which mode did the caller ask for".  x == 0 (idle
;    slot) takes the promote/compact path at 561c; x != 0 takes the demote path at 5641.  The
;    C twin gets this right (it reads cpu.X after the call) but its doc comment used to call
;    cpu.X "a selector", and building this twin from that comment produced a real mismatch on
;    the very first call — caught by sfxmix_verify (badCell=$c0ff then $d203).
;  - pick_top's $0716 = $10 init and pick_next's $0716 = 0 init happen even when no slot wins,
;    while $0714/$0715/$0717 keep their previous values — do not fold the init into the store.
;  - pick_top's compare is strict (<) so the FIRST minimum wins; pick_next's is >= so the LAST
;    maximum wins.  Both exit with cpu.X = $0C (the 6502 loop's terminal X).
;  - cpu.Y is never written before the epilogue, so savedY does NOT need a register: the
;    epilogue just copies cpu.Y into cpu.A.  (The C's mid-function `cpu.Y = savedY` is a no-op.)
;
; Registers (d2/d5 and a2-a4 callee-saved at entry; d0/d1/a0/a1 are scratch AND are clobbered
; by the jsr rof_pokey_write, so nothing live may sit in them across a call):
;   a2 = mem+$0705 voice reg-idx table — also reaches all four scan cells with an 8-bit
;        displacement: $0714=$0F(a2) $0715=$10(a2) $0716=$11(a2) $0717=$12(a2)
;   a3 = cpu (A=0, X=1, Y=2)
;   a4 = mem+$066B prio/vol value table — also reaches the other two columns: freq = $0E(a4),
;        distortion = -$0E(a4)
;   a0 = the scans' autoincrement walk pointer (scratch, not preserved)
;   d5 = the y the leaves see (= cpu.Y, or $0717 on the 5650 path)
;   d2 = wfc's reg idx x, and (disjoint in time) pick_next's excluded index $0715
;   d0 = the scans' running min/max   d1 = scratch value

	xdef	sfx_reorder_voice_slot_asm
	ifnd	ROF_SFXMIX_VERIFY
	xdef	sfx_reorder_voice_slot		; ships as the plain symbol directly
	endif
	xref	mem
	xref	cpu
	xref	rof_pokey_write

	section	code

sfx_reorder_voice_slot:
sfx_reorder_voice_slot_asm:
	movem.l	d2/d5/a2-a4,-(sp)
	lea	cpu,a3
	lea	mem+$0705,a2
	lea	mem+$066B,a4
	moveq	#0,d5
	move.b	2(a3),d5		; the leaves' y = cpu.Y  (5614 TYA; 5615 PHA)
	bsr	wfc			; 5616: write_freq_ctrl(y) -> d2 = mem[$0705+y]

	tst.b	d2			; 5619 TXA; 561a BNE 5641 — X is what the JSR LEFT, not
	bne.s	L_5641			; ...the caller's X: slot ACTIVE -> demote path

	; ---- 561c: the slot is idle.  a = mem[$066B+y] & $0F ----
	move.b	(a4,d5.w),d0
	andi.b	#$0F,d0			; 561f AND #$0F
	cmp.b	$0F(a2),d0		; 5621 CMP $0714
	bcs.s	L_5664			; 5624 BCC 5664 — a < top: done, no pick_top
	bne.s	do_move			; 5626 BNE 562d
	cmp.b	$10(a2),d5		; 5628 CPY $0715
	bcs.s	L_5664			; 562b BCC 5664 — a == top and y < topidx: done
do_move:
	; 562d: steal the current top's ($0715) POKEY register into slot y, then re-emit voice y.
	moveq	#0,d1
	move.b	$10(a2),d1		; tx = $0715
	move.b	(0,a2,d1.w),(0,a2,d5.w)	; 5630/5633 mem[$0705+y] = mem[$0705+tx]
	clr.b	(0,a2,d1.w)		; 5636/5638 mem[$0705+tx] = 0
	bsr.s	wfc			; 563b
	bra.s	L_5661			; 563e JMP 5661

L_5641:
	cmp.w	#$0D,d5			; 5641 CPY #$0D
	bcc.s	L_5661			; 5643 BCS 5661 — slot out of range: just re-latch the top
	bsr	pick_next		; 5645 JSR 56AF
	moveq	#0,d1
	move.b	$10(a2),d1		; 5648 LDX $0715
	cmp.b	$12(a2),d1		; 564b CPX $0717
	beq.s	L_5661			; 564e BEQ 5661 — the next-best IS the top: nothing to move
	move.b	$12(a2),d5		; 5650 LDY $0717 (the leaves' y = the next slot)
	move.b	(0,a2,d1.w),(0,a2,d5.w)	; 5653/5656 mem[$0705+next] = mem[$0705+tx]
	clr.b	(0,a2,d1.w)		; 5659/565b mem[$0705+tx] = 0
	bsr.s	wfc			; 565e
L_5661:
	bsr	pick_top		; 5661 JSR 568A
L_5664:
	move.b	2(a3),(a3)		; 5664 PLA; 5665 TAY — cpu.Y is untouched, so this is just
					; ...the C twin's cpu.A = savedY
	movem.l	(sp)+,d2/d5/a2-a4
	rts

; ---------------------------------------------------------------------------------------------
; wfc — sfx_voice_write_freq_ctrl @ $5673, inlined.  For voice d5: write AUDF ($D1FE+x) = the
; frequency value mem[$0679+y], then AUDC ($D1FF+x) = (prio nibble & $0F) | distortion, where
; x = mem[$0705+y]; skip everything if x == 0.
; In: d5 = y.  Out: d2 = x, cpu.X = x, cpu.A = the AUDC byte (when x != 0).
; Clobbers d0/d1/a0/a1.  d2/d5 and a2-a4 survive the call (GCC saves d2-d7/a2-a5).
wfc:
	move.b	(a2,d5.w),d2		; 5673 LDX $0705,Y
	move.b	d2,1(a3)		; cpu.X = x  (set BEFORE the early-out, like the C)
	beq.s	wfc_ret			; 5676 BEQ 5689 — inactive slot
	; --- AUDF: bus_write($D1FE+x, mem[$0679+y]) ---
	moveq	#0,d0
	move.b	d2,d0
	subq.w	#2,d0			; POKEY reg = ($D1FE+x) - $D200 = x-2
	cmp.w	#16,d0
	bcc.s	wfc_audc		; outside $D200-$D20F: bus_write drops it silently
	moveq	#0,d1
	move.b	($0E,a4,d5.w),d1	; 5678 LDA $0679,Y
	move.l	d1,-(sp)		; ...GCC m68k passes both args on the stack
	move.l	d0,-(sp)
	jsr	rof_pokey_write		; 567b STA $D1FE,X
	addq.l	#8,sp
wfc_audc:
	move.b	(a4,d5.w),d1		; 567e LDA $066B,Y
	andi.b	#$0F,d1			; 5681 AND #$0F
	or.b	(-$0E,a4,d5.w),d1	; 5683 ORA $065D,Y
	move.b	d1,(a3)			; cpu.A = the AUDC byte (computed even if the write drops)
	moveq	#0,d0
	move.b	d2,d0
	subq.w	#1,d0			; POKEY reg = ($D1FF+x) - $D200 = x-1
	cmp.w	#16,d0
	bcc.s	wfc_ret
	andi.l	#$FF,d1
	move.l	d1,-(sp)
	move.l	d0,-(sp)
	jsr	rof_pokey_write		; 5686 STA $D1FF,X
	addq.l	#8,sp
wfc_ret:
	rts

; ---------------------------------------------------------------------------------------------
; pick_top — sfx_pick_top_voice @ $568A, inlined and UNROLLED over slots 1..12 (see the header
; note on why unrolling beats a loop here).  Among ACTIVE slots (mem[$0705+x] != 0) latch the
; SMALLEST priority nibble below $10 into $0716 (running min) / $0714 (value) / $0715 (index).
; a0 walks the reg-idx table so the per-slot active test is one `tst.b (a0)+`; the slot index is
; a compile-time constant, so the $0715 store needs no counter register.
; Exit cpu.X = $0C.  Clobbers d0/d1/a0.

PT_SLOT	macro				; \1 = slot index 1..12
	tst.b	(a0)+			; 5692 LDA $0705,X
	beq.s	.nx\@			; 5695 BEQ 56AA — inactive slot
	move.b	(\1,a4),d1		; 5697 LDA $066B,X
	andi.b	#$0F,d1			; 569A AND #$0F
	cmp.b	d0,d1			; 569C CMP $0716
	bcc.s	.nx\@			; 569F BCS 56AA — a >= min: not an improvement
	move.b	d1,d0			; running min = a
	move.b	d1,$11(a2)		; 56A1 STA $0716
	move.b	d1,$0F(a2)		; 56A4 STA $0714
	move.b	#\1,$10(a2)		; 56A7 STX $0715
.nx\@:
	endm

pick_top:
	moveq	#$10,d0			; running min (mirrors $0716)
	move.b	d0,$11(a2)		; 568A/568C $0716 = $10 — even if nothing wins
	lea	1(a2),a0		; walk from mem[$0705+1]
	PT_SLOT	1
	PT_SLOT	2
	PT_SLOT	3
	PT_SLOT	4
	PT_SLOT	5
	PT_SLOT	6
	PT_SLOT	7
	PT_SLOT	8
	PT_SLOT	9
	PT_SLOT	10
	PT_SLOT	11
	PT_SLOT	12
	moveq	#$0C,d0
	move.b	d0,1(a3)		; cpu.X = $0C
	rts

; ---------------------------------------------------------------------------------------------
; pick_next — sfx_pick_next_voice @ $56AF, inlined and unrolled the same way.  Among EMPTY slots
; and the excluded slot $0715, latch the LARGEST priority nibble into $0716 / index $0717.
; d2 holds $0715 for the whole scan (loop-invariant: nothing in the loop writes it), so the
; exclusion test is an 8-cycle `cmpi.b #slot,d2` rather than a 16-cycle absolute compare.
; Exit cpu.X = $0C.  Clobbers d0/d1/d2/a0.

PN_SLOT	macro				; \1 = slot index 1..12
	tst.b	(a0)+			; 56B5 LDA $0705,X
	beq.s	.take\@			; 56B8 BEQ 56BF — empty slot: consider it
	cmpi.b	#\1,d2			; 56BA CPX $0715
	bne.s	.nx\@			; 56BD BNE 56CF — active and not the top: skip
.take\@:
	move.b	(\1,a4),d1		; 56BF LDA $066B,X
	andi.b	#$0F,d1			; 56C2 AND #$0F
	cmp.b	d0,d1			; 56C4 CMP $0716
	bcs.s	.nx\@			; 56C7 BCC 56CF — a < max: skip
	move.b	d1,d0			; running max = a
	move.b	d1,$11(a2)		; 56C9 STA $0716
	move.b	#\1,$12(a2)		; 56CC STX $0717
.nx\@:
	endm

pick_next:
	clr.b	$11(a2)			; 56AF/56B1 $0716 = 0
	moveq	#0,d0			; running max (mirrors $0716)
	move.b	$10(a2),d2		; $0715 — invariant across the scan
	lea	1(a2),a0
	PN_SLOT	1
	PN_SLOT	2
	PN_SLOT	3
	PN_SLOT	4
	PN_SLOT	5
	PN_SLOT	6
	PN_SLOT	7
	PN_SLOT	8
	PN_SLOT	9
	PN_SLOT	10
	PN_SLOT	11
	PN_SLOT	12
	moveq	#$0C,d0
	move.b	d0,1(a3)		; cpu.X = $0C
	rts

	end
