	xdef	_getVBR__13AmigaHardwareFv
	xdef	_isBlitterBusy__13AmigaHardwareFv
	xdef	_blitterWait__13AmigaHardwareFv
	xdef	_blitterClear__13AmigaHardwareFPUsUsUss
	xdef	_blitterCopy__13AmigaHardwareFPUsPUsUsUssssUsUsUs
	xdef	_blitterCopyWithMask__13AmigaHardwareFPUsPUsPUsUsUssssssUsUsUc
	xdef	_blitterLine__13AmigaHardwareFPUsUsUsUsUsUsUc
	xdef	_blitterFill__13AmigaHardwareFPUsUsUss
	xdef	_processBlitterQueue__13AmigaHardwareFv
	xref	_blitterQueueToBeBlitted__13AmigaHardware
	xref	_blitterQueueAddPosition__13AmigaHardware
	xref	_blitterQueueBuffer__13AmigaHardware
	xref	_blitterQueueBufferEnd__13AmigaHardware
	xref	_hasQueuedBlits__13AmigaHardware
	xref	_octants__13AmigaHardware

	section	code

_getVBR__13AmigaHardwareFv:
	movem.l	a5-a6,-(sp)
	move.l	4.w,a6
	lea	GetVBRUserFunc(pc),a5
	jsr	-$1e(a6)		; _LVOSuperVisor
	movem.l	(sp)+,a5-a6
	rts

GetVBRUserFunc:
	ori.w	#$0700,sr
	movec	vbr,d0
	rte

_isBlitterBusy__13AmigaHardwareFv:
	tst.w	$dff002
	btst	#14,$dff002
	sne	d0
	rts

_blitterWait__13AmigaHardwareFv:
	tst.w	$dff002
bW_waitUntilBlitterNotBusy:
	btst	#14,$dff002
	bne.s	bW_waitUntilBlitterNotBusy
	rts

_blitterClear__13AmigaHardwareFPUsUsUss:
	move.w	#$0040,$dff09a					; AmigaHardware::setInterrupts(INTF_BLIT, false);
	tst.w	$dff002
	btst	#14,$dff002					; if (!isBlitterBusy() && !hasQueuedBlits()) {
	bne	bCl_queueClear
	tst.b	_hasQueuedBlits__13AmigaHardware
	bne	bCl_queueClear

	move.l	#-1,$dff044					; *bltawmPointer = 0xffffffff;
	move.w	#0,$dff042					; *bltcon1Pointer = 0;
	move.w	#$100,$dff040					; *bltcon0Pointer = BC0F_DEST;
	move.w	d3,$dff066					; *bltdmodPointer = modulo;
	move.l	d2,$dff054					; *bltdptPointer = data;
	lsl.w	#6,d1
	or.w	d0,d1
	move.w	d1,$dff058					; *bltsizePointer = (uint16_t)((height << 6) | width);
	move.w	#$8040,$dff09a					; AmigaHardware::setInterrupts(INTF_BLIT, true);
	rts

bCl_queueClear:
	move.l	a4,-(sp)

	move.l	_blitterQueueAddPosition__13AmigaHardware,a4
	move.w	#8,(a4)+				; *blitterQueueAddPosition++ = 8;
	lea	8*4(a4),a4
	cmp.l	_blitterQueueBufferEnd__13AmigaHardware,a4
	bcs.s	bCl_queuePositionOk
	lea	_blitterQueueBuffer__13AmigaHardware+8*4,a4
bCl_queuePositionOk:
	lea	-8*4(a4),a4
	move.w	#$044,(a4)+				; *bltafwmPointer = 0xffff;
	move.w	#-1,(a4)+
	move.w	#$046,(a4)+				; *bltalwmPointer = 0xffff;
	move.w	#-1,(a4)+
	move.w	#$042,(a4)+				; *bltcon1Pointer = 0;
	clr.w	(a4)+
	move.w	#$040,(a4)+				; *bltcon0Pointer = BC0F_DEST;
	move.w	#$100,(a4)+
	move.w	#$066,(a4)+				; *bltdmodPointer = modulo;
	move.w	d3,(a4)+
	move.w	#$056,(a4)+				; *bltdptPointer = destination;
	move.w	d2,(a4)+
	swap	d2
	move.w	#$054,(a4)+
	move.w	d2,(a4)+
	lsl.w	#6,d1
	or.w	d0,d1
	move.w	#$058,(a4)+				; *bltsizePointer = (uint16_t)((height << 6) | width);
	move.w	d1,(a4)+
	move.l	a4,_blitterQueueAddPosition__13AmigaHardware
	move.b	#1,_hasQueuedBlits__13AmigaHardware	; hasQueuedBlits = true;
	move.l	(sp)+,a4

	bsr	_processBlitterQueue__13AmigaHardwareFv

	move.w	#$8040,$dff09a				; AmigaHardware::setInterrupts(INTF_BLIT, true);
	rts

_blitterCopy__13AmigaHardwareFPUsPUsUsUssssUsUsUs:
	move.w	#$0040,$dff09a					; AmigaHardware::setInterrupts(INTF_BLIT, false);
	tst.w	$dff002
	btst	#14,$dff002					; if (!isBlitterBusy() && !hasQueuedBlits()) {
	bne	bC_queueCopy
	tst.b	_hasQueuedBlits__13AmigaHardware
	bne	bC_queueCopy

	move.w	d5,$dff044				; *bltafwmPointer = firstWordMask;
	move.w	d6,$dff046				; *bltalwmPointer = lastWordMask;
	moveq	#0,d6
	tst.w	d4
	bpl.s	bC_shiftOk1
	moveq	#2,d6					; (shift < 0 ? BLITREVERSE : 0)
	neg.w	d4					; (shift < 0 ? -shift : shift)
bC_shiftOk1:
	move.w	d6,$dff042				; *bltcon1Pointer = (uint16_t);
	ror.w	#4,d4					; << 12
	or.w	#$9c0,d4				; BC0F_SRCA | BC0F_DEST | ABC | ABNC
	move.w	d4,$dff040				; *bltcon0Pointer =
	move.w	a1,$dff064				; *bltamodPointer = sourceModulo;
	move.w	a2,$dff066				; *bltdmodPointer = destinationModulo;
	move.l	d2,$dff050				; *bltaptPointer = source;
	move.w	d7,$dff072				; *bltbdatPointer = mask;
	move.l	d3,$dff054				; *bltdptPointer = destination;
	lsl.w	#6,d1
	or.w	d0,d1
	move.w	d1,$dff058				; *bltsizePointer = (uint16_t)((height << 6) | width);
	move.w	#$8040,$dff09a				; AmigaHardware::setInterrupts(INTF_BLIT, true);
	rts

bC_queueCopy:
	move.l	a4,-(sp)

	move.l	_blitterQueueAddPosition__13AmigaHardware,a4
	move.w	#12,(a4)+				; *blitterQueueAddPosition++ = 12;
	lea	12*4(a4),a4
	cmp.l	_blitterQueueBufferEnd__13AmigaHardware,a4
	bcs.s	bC_queuePositionOk
	lea	_blitterQueueBuffer__13AmigaHardware+12*4,a4
bC_queuePositionOk:
	lea	-12*4(a4),a4
	move.w	#$044,(a4)+				; *bltafwmPointer = firstWordMask;
	move.w	d5,(a4)+
	move.w	#$046,(a4)+				; *bltalwmPointer = lastWordMask;
	move.w	d6,(a4)+
	moveq	#0,d6
	tst.w	d4
	bpl.s	bC_shiftOk2
	moveq	#2,d6					; (shift < 0 ? BLITREVERSE : 0)
	neg.w	d4					; (shift < 0 ? -shift : shift)
bC_shiftOk2:
	move.w	#$042,(a4)+				; *bltcon1Pointer = 
	move.w	d6,(a4)+
	ror.w	#4,d4					; << 12
	or.w	#$9c0,d4				; BC0F_SRCA | BC0F_DEST | ABC | ABNC
	move.w	#$040,(a4)+				; *bltcon0Pointer =
	move.w	d4,(a4)+
	move.w	#$064,(a4)+				; *bltamodPointer = sourceModulo;
	move.w	a1,(a4)+
	move.w	#$066,(a4)+				; *bltdmodPointer = destinationModulo;
	move.w	a2,(a4)+
	move.w	#$052,(a4)+				; *bltaptPointer = source;
	move.w	d2,(a4)+
	swap	d2
	move.w	#$050,(a4)+
	move.w	d2,(a4)+
	move.w	#$072,(a4)+				; *bltbptPointer = mask;
	move.w	d7,(a4)+
	move.w	#$056,(a4)+				; *bltdptPointer = destination;
	move.w	d3,(a4)+
	swap	d3
	move.w	#$054,(a4)+
	move.w	d3,(a4)+
	lsl.w	#6,d1
	or.w	d0,d1
	move.w	#$058,(a4)+				; *bltsizePointer = (uint16_t)((height << 6) | width);
	move.w	d1,(a4)+
	move.l	a4,_blitterQueueAddPosition__13AmigaHardware
	move.b	#1,_hasQueuedBlits__13AmigaHardware	; hasQueuedBlits = true;
	move.l	(sp)+,a4

	bsr	_processBlitterQueue__13AmigaHardwareFv

	move.w	#$8040,$dff09a				; AmigaHardware::setInterrupts(INTF_BLIT, true);
	rts

_blitterCopyWithMask__13AmigaHardwareFPUsPUsPUsUsUssssssUsUsUc:
	move.w	#$0040,$dff09a				; AmigaHardware::setInterrupts(INTF_BLIT, false);
	tst.w	$dff002
	btst	#14,$dff002				; if (!isBlitterBusy() && !hasQueuedBlits()) {
	bne	bCWM_queueCopyWithMask
	tst.b	_hasQueuedBlits__13AmigaHardware
	bne	bCWM_queueCopyWithMask

	move.w	a5,$dff044				; *bltafwmPointer = firstWordMask;
	move.w	a6,$dff046				; *bltalwmPointer = lastWordMask;
	tst.w	d6					; (maskShift < 0 ? -maskShift : maskShift)
	bpl.s	bCWM_maskShiftOk1
	neg.w	d6
bCWM_maskShiftOk1:
	ror.w	#4,d6					; << 12
	tst.w	d5					; (sourceShift < 0 ? BLITREVERSE : 0)
	bpl.s	bCWM_sourceShiftOk1
	addq	#2,d6
	neg.w	d5					; (sourceShift < 0 ? -sourceShift : sourceShift)
bCWM_sourceShiftOk1:
	move.w	d6,$dff042				; *bltcon1Pointer = 
	ror.w	#4,d5					; << 12
	or.w	#$fc0,d5				; BC0F_SRCA | BC0F_SRCB | BC0F_SRCC | BC0F_DEST | ABC | ABNC
	tst.b	d7					; (clearMasked ? 0 : (NANBC | ANBC))
	bne.s	bCWM_clearMaskedOk1
	or.w	#$22,d5
bCWM_clearMaskedOk1:
	move.w	d5,$dff040				; *bltcon0Pointer =
	move.w	a1,$dff064				; *bltamodPointer = sourceModulo;
	move.w	a3,$dff062				; *bltbmodPointer = maskModulo;
	move.w	a2,$dff060				; *bltcmodPointer = destinationModulo;
	move.w	a2,$dff066				; *bltdmodPointer = destinationModulo;
	move.l	d2,$dff050				; *bltaptPointer = source;
	move.l	d4,$dff04c				; *bltbptPointer = mask;
	move.l	d3,$dff048				; *bltcptPointer = destination;
	move.l	d3,$dff054				; *bltdptPointer = destination;
	lsl.w	#6,d1
	or.w	d0,d1
	move.w	d1,$dff058				; *bltsizePointer = (uint16_t)((height << 6) | width);
	move.w	#$8040,$dff09a				; AmigaHardware::setInterrupts(INTF_BLIT, true);
	rts

bCWM_queueCopyWithMask:
	move.l	a4,-(sp)

	move.l	_blitterQueueAddPosition__13AmigaHardware,a4
	move.w	#17,(a4)+				; *blitterQueueAddPosition++ = 17;
	lea	17*4(a4),a4
	cmp.l	_blitterQueueBufferEnd__13AmigaHardware,a4
	bcs.s	bCWM_queuePositionOk
	lea	_blitterQueueBuffer__13AmigaHardware+17*4,a4
bCWM_queuePositionOk:
	lea	-17*4(a4),a4
	move.w	#$044,(a4)+				; *bltafwmPointer = firstWordMask;
	move.w	a5,(a4)+
	move.w	#$046,(a4)+				; *bltalwmPointer = lastWordMask;
	move.w	a6,(a4)+
	tst.w	d6					; (maskShift < 0 ? -maskShift : maskShift)
	bpl.s	bCWM_maskShiftOk2
	neg.w	d6
bCWM_maskShiftOk2:
	ror.w	#4,d6					; << 12
	tst.w	d5					; (sourceShift < 0 ? BLITREVERSE : 0)
	bpl.s	bCWM_sourceShiftOk2
	addq	#2,d6
	neg.w	d5					; (sourceShift < 0 ? -sourceShift : sourceShift)
bCWM_sourceShiftOk2:
	move.w	#$042,(a4)+				; *bltcon1Pointer = 
	move.w	d6,(a4)+
	ror.w	#4,d5					; << 12
	or.w	#$fc0,d5				; BC0F_SRCA | BC0F_SRCB | BC0F_SRCC | BC0F_DEST | ABC | ABNC
	tst.b	d7					; (clearMasked ? 0 : (NANBC | ANBC))
	bne.s	bCWM_clearMaskedOk2
	or.w	#$22,d5
bCWM_clearMaskedOk2:
	move.w	#$040,(a4)+				; *bltcon0Pointer =
	move.w	d5,(a4)+
	move.w	#$064,(a4)+				; *bltamodPointer = sourceModulo;
	move.w	a1,(a4)+
	move.w	#$062,(a4)+				; *bltbmodPointer = maskModulo;
	move.w	a3,(a4)+
	move.w	#$060,(a4)+				; *bltcmodPointer = destinationModulo;
	move.w	a2,(a4)+
	move.w	#$066,(a4)+				; *bltdmodPointer = destinationModulo;
	move.w	a2,(a4)+
	move.w	#$052,(a4)+				; *bltaptPointer = source;
	move.w	d2,(a4)+
	swap	d2
	move.w	#$050,(a4)+
	move.w	d2,(a4)+
	move.w	#$04e,(a4)+				; *bltbptPointer = mask;
	move.w	d4,(a4)+
	swap	d4
	move.w	#$04c,(a4)+
	move.w	d4,(a4)+
	move.w	#$04a,(a4)+				; *bltcptPointer = destination;
	move.w	d3,(a4)+
	move.w	#$056,(a4)+				; *bltdptPointer = destination;
	move.w	d3,(a4)+
	swap	d3
	move.w	#$048,(a4)+
	move.w	d3,(a4)+
	move.w	#$054,(a4)+
	move.w	d3,(a4)+
	lsl.w	#6,d1
	or.w	d0,d1
	move.w	#$058,(a4)+				; *bltsizePointer = (uint16_t)((height << 6) | width);
	move.w	d1,(a4)+
	move.l	a4,_blitterQueueAddPosition__13AmigaHardware
	move.b	#1,_hasQueuedBlits__13AmigaHardware	; hasQueuedBlits = true;
	move.l	(sp)+,a4

	bsr	_processBlitterQueue__13AmigaHardwareFv

	move.w	#$8040,$dff09a				; AmigaHardware::setInterrupts(INTF_BLIT, true);
	rts

_blitterLine__13AmigaHardwareFPUsUsUsUsUsUsUc:
	tst.b	d6					; if (singleBitPerRow && y1 == y2) { return; }
	beq.s	bL_yDeltaOk
	cmp.w	d1,d3
	bne.s	bL_yDeltaOk
	rts
bL_yDeltaOk:

	movem.l	d2-d7,-(sp)
	cmp.w	d1,d3					; if (y2 < y1) {
	bge.s	bL_lineIsFromTopToBottom
	exg	d0,d2					; uint16_t temp = x2; x2 = x1; x1 = temp;
	exg	d1,d3					; temp = y2; y2 = y1; y1 = temp;
bL_lineIsFromTopToBottom:

	moveq	#0,d7					; uint16_t octant = 0;
	sub.w	d0,d2					; int16_t dx = x2 - x1;
	sub.w	d1,d3					; int16_t dy = y2 - y1;
	bne.s	bL_deltaOk				; if (dx == 0 && dy == 0) { return; }
	tst.w	d2
	bne.s	bL_deltaOk
	movem.l	(sp)+,d2-d7
	rts
bL_deltaOk:

	tst.w	d2					; if (dx < 0) {
	bpl.s	bL_dxOctantOk
	addq	#4,d7					; octant += 2;
	neg.w	d2					; dx = (int16_t)-dx;
bL_dxOctantOk:

	move.w	d3,a1					; if (dx >= (dy + dy)) {
	add.w	d3,a1
	cmp.w	a1,d2
	bcs.s	bL_dyOk
	subq	#1,d3					; dy--;
bL_dyOk:

	mulu	d5,d1					; uint16_t* firstWord = data + y1 * (bytesPerRow >> 1) + (x1 >> 4);
	add.l	d1,d4
	move.w	d0,d1
	lsr.w	#3,d1
	and.l	#$fffe,d1
	add.l	d1,d4
	cmp.w	d2,d3					; if (dy < dx) {
	bge.s	bL_dxdyOk
	exg	d2,d3					; int16_t signedTemp = dy; dy = dx; dx = signedTemp;
	addq	#2,d7					; octant++;
bL_dxdyOk:

	ror.w	#4,d0					; uint16_t bltcon0Value = ((x1 & 15) << 12) | BC0F_SRCA | BC0F_SRCC | BC0F_DEST;
	and.w	#$f000,d0
	or.w	#$b00,d0
	lea	_octants__13AmigaHardware,a1		; uint16_t bltcon1Value = octants[octant];
	move.w	(a1,d7.w),d7
	tst.b	d6					; if (singleBitPerRow) {
	bne.s	bL_singleBitPerRow
	or.w	#$fa,d0					; bltcon0Value |= A_OR_C;
	bra.s	bL_bplconValuesOk
bL_singleBitPerRow:
	or.w	#$5a,d0					; bltcon0Value |= A_XOR_C;
	addq	#2,d7					; bltcon1Value |= ONEDOT;
bL_bplconValuesOk:

	add.w	d2,d2					; int16_t v = dx + dx;

	move.w	#$0040,$dff09a				; AmigaHardware::setInterrupts(INTF_BLIT, false);
	tst.w	$dff002
	btst	#14,$dff002				; if (!isBlitterBusy() && !hasQueuedBlits()) {
	bne	bL_queueLine
	tst.b	_hasQueuedBlits__13AmigaHardware
	bne	bL_queueLine

	move.l	#-1,$dff044				; *bltawmPointer = 0xffffffff;
	move.w	#$8000,$dff074				; *bltadatPointer = 0x8000;
	move.w	#$ffff,$dff072				; *bltbdatPointer = 0xffff;
	move.w	d5,$dff060				; *bltcmodPointer = (int16_t)bytesPerRow;
	move.w	d5,$dff066				; *bltdmodPointer = (int16_t)bytesPerRow;
	move.w	d2,$dff062				; *bltbmodPointer = v;
	sub.w	d3,d2					; v -= dy;
	bpl.s	bL_vPositive1				; if (v < 0) {
	or.w	#$40,d7					; bltcon1Value |= SIGNFLAG;
bL_vPositive1:
	move.w	d2,$dff052				; *bltaptlPointer = v;
	sub.w	d3,d2					; v -= dy;
	move.w	d2,$dff064				; *bltamodPointer = v;
	move.w	d0,$dff040				; *bltcon0Pointer = bltcon0Value;
	move.w	d7,$dff042				; *bltcon1Pointer = bltcon1Value;
	move.l	d4,$dff048				; *bltcptPointer = firstWord;
	move.l	d4,$dff054				; *bltdptPointer = firstWord;
	lsl.w	#6,d3					; *bltsizePointer = (uint16_t)((dy << 6) | 2);
	or.w	#2,d3
	move.w	d3,$dff058
	move.w	#$8040,$dff09a				; AmigaHardware::setInterrupts(INTF_BLIT, true);
	movem.l	(sp)+,d2-d7
	rts

bL_queueLine:
	move.l	a4,-(sp)

	move.l	_blitterQueueAddPosition__13AmigaHardware,a4
	move.w	#16,(a4)+				; *blitterQueueAddPosition++ = 16;
	lea	16*4(a4),a4
	cmp.l	_blitterQueueBufferEnd__13AmigaHardware,a4
	bcs.s	bL_queuePositionOk
	lea	_blitterQueueBuffer__13AmigaHardware+16*4,a4
bL_queuePositionOk:
	lea	-16*4(a4),a4
	move.w	#$044,(a4)+				; *bltafwmPointer = 0xffff;
	move.w	#-1,(a4)+
	move.w	#$046,(a4)+				; *bltalwmPointer = 0xffff;
	move.w	#-1,(a4)+
	move.w	#$074,(a4)+				; *bltadatPointer = 0x8000;
	move.w	#$8000,(a4)+
	move.w	#$072,(a4)+				; *bltbdatPointer = 0xffff;
	move.w	#$ffff,(a4)+
	move.w	#$060,(a4)+				; *bltcmodPointer = (int16_t)bytesPerRow;
	move.w	d5,(a4)+
	move.w	#$066,(a4)+				; *bltdmodPointer = (int16_t)bytesPerRow;
	move.w	d5,(a4)+
	move.w	#$062,(a4)+				; *bltbmodPointer = v;
	move.w	d2,(a4)+
	sub.w	d3,d2					; v -= dy;
	bpl.s	bL_vPositive2				; if (v < 0) {
	or.w	#$40,d7					; bltcon1Value |= SIGNFLAG;
bL_vPositive2:
	move.w	#$052,(a4)+				; *bltaptlPointer = v;
	move.w	d2,(a4)+
	sub.w	d3,d2					; v -= dy;
	move.w	#$064,(a4)+				; *bltamodPointer = v;
	move.w	d2,(a4)+
	move.w	#$040,(a4)+				; *bltcon0Pointer = bltcon0Value;
	move.w	d0,(a4)+
	move.w	#$042,(a4)+				; *bltcon1Pointer = bltcon1Value;
	move.w	d7,(a4)+
	move.w	#$04a,(a4)+				; *bltcptPointer = firstWord;
	move.w	d4,(a4)+
	swap	d4
	move.w	#$048,(a4)+
	move.w	d4,(a4)+
	swap	d4
	move.w	#$056,(a4)+				; *bltdptPointer = firstWord;
	move.w	d4,(a4)+
	swap	d4
	move.w	#$054,(a4)+
	move.w	d4,(a4)+
	lsl.w	#6,d3					; *bltsizePointer = (uint16_t)((dy << 6) | 2);
	or.w	#2,d3
	move.w	#$058,(a4)+
	move.w	d3,(a4)+
	move.l	a4,_blitterQueueAddPosition__13AmigaHardware
	move.b	#1,_hasQueuedBlits__13AmigaHardware	; hasQueuedBlits = true;
	move.l	(sp)+,a4

	bsr	_processBlitterQueue__13AmigaHardwareFv

	move.w	#$8040,$dff09a				; AmigaHardware::setInterrupts(INTF_BLIT, true);
	movem.l	(sp)+,d2-d7
	rts

_blitterFill__13AmigaHardwareFPUsUsUss:
	move.w	#$0040,$dff09a					; AmigaHardware::setInterrupts(INTF_BLIT, false);
	tst.w	$dff002
	btst	#14,$dff002					; if (!isBlitterBusy() && !hasQueuedBlits()) {
	bne	bF_queueFill
	tst.b	_hasQueuedBlits__13AmigaHardware
	bne	bF_queueFill

	move.l	#-1,$dff044					; *bltawmPointer = 0xffffffff;
	move.w	#$9f0,$dff040					; *bltcon0Pointer = BC0F_SRCA | BC0F_DEST | A_TO_D;
	move.w	#$a,$dff042					; *bltcon1Pointer = BLITREVERSE | FILL_OR;
	move.w	d3,$dff064					; *bltamodPointer = modulo;
	move.w	d3,$dff066					; *bltdmodPointer = modulo;
	move.l	d2,$dff050					; *bltaptPointer = data;
	move.l	d2,$dff054					; *bltdptPointer = data;
	lsl.w	#6,d1
	or.w	d0,d1
	move.w	d1,$dff058					; *bltsizePointer = (uint16_t)((height << 6) | width);
	move.w	#$8040,$dff09a					; AmigaHardware::setInterrupts(INTF_BLIT, true);
	rts

bF_queueFill:
	move.l	a4,-(sp)

	move.l	_blitterQueueAddPosition__13AmigaHardware,a4
	move.w	#11,(a4)+				; *blitterQueueAddPosition++ = 11;
	lea	11*4(a4),a4
	cmp.l	_blitterQueueBufferEnd__13AmigaHardware,a4
	bcs.s	bF_queuePositionOk
	lea	_blitterQueueBuffer__13AmigaHardware+11*4,a4
bF_queuePositionOk:
	lea	-11*4(a4),a4
	move.w	#$044,(a4)+				; *bltafwmPointer = 0xffff;
	move.w	#-1,(a4)+
	move.w	#$046,(a4)+				; *bltalwmPointer = 0xffff;
	move.w	#-1,(a4)+
	move.w	#$040,(a4)+				; *bltcon0Pointer = BC0F_SRCA | BC0F_DEST | A_TO_D;
	move.w	#$9f0,(a4)+
	move.w	#$042,(a4)+				; *bltcon1Pointer = BLITREVERSE | FILL_OR;
	move.w	#$a,(a4)+
	move.w	#$064,(a4)+				; *bltamodPointer = modulo;
	move.w	d3,(a4)+
	move.w	#$066,(a4)+				; *bltdmodPointer = modulo;
	move.w	d3,(a4)+
	move.w	#$052,(a4)+				; *bltaptPointer = destination;
	move.w	d2,(a4)+
	swap	d2
	move.w	#$050,(a4)+
	move.w	d2,(a4)+
	swap	d2
	move.w	#$056,(a4)+				; *bltdptPointer = destination;
	move.w	d2,(a4)+
	swap	d2
	move.w	#$054,(a4)+
	move.w	d2,(a4)+
	lsl.w	#6,d1
	or.w	d0,d1
	move.w	#$058,(a4)+				; *bltsizePointer = (uint16_t)((height << 6) | width);
	move.w	d1,(a4)+
	move.l	a4,_blitterQueueAddPosition__13AmigaHardware
	move.b	#1,_hasQueuedBlits__13AmigaHardware	; hasQueuedBlits = true;
	move.l	(sp)+,a4

	bsr	_processBlitterQueue__13AmigaHardwareFv

	move.w	#$8040,$dff09a				; AmigaHardware::setInterrupts(INTF_BLIT, true);
	rts

_processBlitterQueue__13AmigaHardwareFv:
	tst.w	$dff002
	btst	#14,$dff002					; if (isBlitterBusy()) return;
	bne	pBQ_done
	tst.b	_hasQueuedBlits__13AmigaHardware		; if (!hasQueuedBlits) return;
	beq	pBQ_done

	move.l	_blitterQueueToBeBlitted__13AmigaHardware,a0
	move.w	(a0)+,d0					; uint16_t registerCount = *blitterQueueToBeBlitted++;
	move.l	a0,a1
	move.w	d0,d1
	add.w	d1,d1
	add.w	d1,d1
	add.w	d1,a1
	cmp.l	_blitterQueueBufferEnd__13AmigaHardware,a1	; if (blitterQueueToBeBlitted + registerCount + registerCount >= blitterQueueBufferEnd) {
	bcs.s	pBQ_queuePositionOk
	lea	_blitterQueueBuffer__13AmigaHardware,a0		; blitterQueueToBeBlitted = blitterQueueBuffer;
pBQ_queuePositionOk:

	lea	$dff000,a1
	subq	#1,d0						; for (uint16_t i = 0; i < registerCount; i++)
pBQ_copyRegistersLoop:
	move.w	(a0)+,d1					; uint32_t destination = 0xdff000 + *blitterQueueToBeBlitted++;
	move.w	(a0)+,(a1,d1.w)					; *((uint16_t*)destination) = *blitterQueueToBeBlitted++;
	dbra	d0,pBQ_copyRegistersLoop

	cmp.l	_blitterQueueAddPosition__13AmigaHardware,a0	; hasQueuedBlits = blitterQueueToBeBlitted != blitterQueueAddPosition;
	sne	_hasQueuedBlits__13AmigaHardware

	move.l	a0,_blitterQueueToBeBlitted__13AmigaHardware
pBQ_done:
	rts

	end
