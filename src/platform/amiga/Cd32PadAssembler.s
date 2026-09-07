; Cd32PadAssembler.s — the CD32 gamepad's 7-button serial read, port 1.
;
;   unsigned short rof_cd32_read(void)      ; plain C linkage, no args, result in d0
;
; A CD32 pad's stick reads through JOY1DAT exactly like a plain joystick; only the BUTTONS use
; this protocol, a bit-banged shift register that takes over three pins:
;
;   pin 6 (FIR1) = CIA-A PRA bit 7 ($bfe001), direction in DDRA ($bfe201)  -> CLOCK, driven by us
;   pin 5 (POTX) = POTGO $dff034 bits 13/12 = OUTRX/DATRX                  -> LATCH, driven low
;   pin 9 (POTY) = POTGOR $dff016 bit 14 = DATRY                           -> serial DATA, in
;
; Drive the latch low to put the pad in shift mode, then clock ten bits out MSB first, each one
; active-LOW on DATRY.  Result (bit SET = pressed):
;
;   $0200 BLUE  $0100 RED  $0080 YELLOW  $0040 GREEN  $0020 FORWARD  $0010 REVERSE  $0008 PLAY
;   $0004..$0001  pad marker bits
;
; ⚠ THE DELAYS ARE THE PROTOCOL, AND THIS FILE IS ASM FOR THAT REASON ALONE.  Each `tst.b (a0)`
; is a CIA byte access, synchronised to the ~716 kHz E clock, so it buys ~1.4 us of settle time
; for the pad's shift register — eight per bit, six before the first.  Replacing them with a
; counted delay, or letting a compiler reorder them, breaks the read on some pads only.  Do not
; "clean this up": it is kept verbatim from the reference implementation (Attack of the PETSCII
; Robots' readCD32Pad), which is the only version known to work across real pads.
;
; DDRA bit 7 and the whole POTGO word are saved and restored, because CIA-A is the OS's chip too
; (the keyboard ISR lives on its SP interrupt) and POTGO's other bits belong to the mouse.
;
; Registers: d0 result, d1 scratch, d2 saved POTGOR, d3 clock bit, d4 data bit, d5 bit counter.
; d2-d5 are callee-saved under the GCC m68k ABI, hence the movem.

	xdef	rof_cd32_read

	section	code

rof_cd32_read:
	movem.l	d2-d5,-(sp)

	lea	$bfe001,a0		; CIA-A PRA  (+$200 = DDRA)
	lea	$dff000,a1		; custom chips
	move.w	$16(a1),d2		; POTGOR — saved, written back to POTGO at the end
	moveq	#7,d3			; CIAB_GAMEPORT1 = pin 6, the clock
	moveq	#14,d4			; DATRY = pin 9, the serial data
	bset	d3,$200(a0)		; clock pin -> OUTPUT
	bclr	d3,(a0)			; clock low
	move.w	d2,d1
	and.w	#$cfff,d1		; mask OUTRX/DATRX out
	or.w	#$2000,d1		; OUTRX=1, DATRX=0 -> pin 5 driven low = shift mode
	move.w	d1,$34(a1)		; POTGO

	moveq	#0,d0
	moveq	#10-1,d5		; ten bits, MSB (bit 9) first
	bra.s	.timingSkip		; the first bit gets six delays, the rest eight
.buttonLoop:
	tst.b	(a0)
	tst.b	(a0)
.timingSkip:
	tst.b	(a0)
	tst.b	(a0)
	tst.b	(a0)
	tst.b	(a0)
	tst.b	(a0)
	tst.b	(a0)
	move.w	$16(a1),d1		; POTGOR: DATRY holds this bit
	bset	d3,(a0)			; pulse the clock high...
	bclr	d3,(a0)			; ...and straight back low
	btst	d4,d1
	bne.s	.noButton		; DATRY high = not pressed
	bset	d5,d0
.noButton:
	dbra	d5,.buttonLoop

	bclr	d3,$200(a0)		; clock pin back to INPUT (it is fire again)
	move.w	d2,$34(a1)		; restore POTGO

	movem.l	(sp)+,d2-d5
	rts
