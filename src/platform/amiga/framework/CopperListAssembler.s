	xdef	_showBitmap__10CopperListFUlRC6BitmapUsUsssUs
	xdef	_showSprite__10CopperListFUlUsRC6Sprite

; Offsets to various C++ structures. These MUST match the C++ structures!
; -----------------------------------------------------------------------
data_				equ	4
bitmapData			equ	0
bitmapBitplanes			equ	8
bitmapWidthInBytes		equ	12
bitmapRowSizeInBytes		equ	14
bitmapBitplaneSizeInBytes	equ	16
bitmapInterleaved		equ	18
spriteData			equ	0

	section	code

_showBitmap__10CopperListFUlRC6BitmapUsUsssUs:
	movem.l	d2-d5,-(sp)

	tst.w	d3				; if (xOffset) {
	beq.s	sB_xOffsetOk
	asr.w	#3,d3				; bitplane += xOffset >> 3;
sB_xOffsetOk:
	tst.w	d4				; if (yOffset) {
	beq.s	sB_yOffsetOk
	muls	bitmapRowSizeInBytes(a1),d4	; bitplane += yOffset * bitmap_rowSizeInBytes;
	add.w	d4,d3
sB_yOffsetOk:
	ext.l	d3			; uint32_t bitplane = (uint32_t)bitmap_data;
	add.l	bitmapData(a1),d3

	tst.w	d5				; if (bitplaneCount == 0) {
	bne.s	sB_bitplaneCountOk
	move.w	bitmapBitplanes(a1),d5		; bitplaneCount = bitmap_bitplanes;
sB_bitplaneCountOk:

	moveq	#0,d4
	tst.b	bitmapInterleaved(a1)
	beq.s	sB_bitplaneDeltaPlaneSize
	move.w	bitmapWidthInBytes(a1),d4
	bra.s	sB_bitplaneDeltaOk
sB_bitplaneDeltaPlaneSize:
	move.w	bitmapBitplaneSizeInBytes(a1),d4
sB_bitplaneDeltaOk:

	move.l	data_(a0),a1
	add.w	d0,d0
	add.w	d0,d0
	add.w	d0,a1
	add.w	d1,d1				; uint16_t bitplanePointerRegister = bpl1pth + ((firstBitplane - 1) << 2);
	add.w	d1,d1
	add.w	#$0dc,d1
	add.w	d2,d2
	add.w	d2,d2
	subq	#2,d2
	subq	#1,d5				; for (uint16_t i = 0; i < bitplaneCount; i++, bitplanePointerRegister += (bitplaneNumberDelta << 2)) {
sB_loop:
	move.w	d1,(a1)+			; data_[listIndex++] = copperMove(bitplanePointerRegister, (uint16_t)(bitplane >> 16));
	swap	d3
	move.w	d3,(a1)+

	addq	#2,d1				; data_[listIndex++] = copperMove(bitplanePointerRegister + 2, (uint16_t)bitplane);
	move.w	d1,(a1)+
	swap	d3
	move.w	d3,(a1)+

	add.l	d4,d3				; bitplane += bitmap_interleaved ? bitmap_widthInBytes : bitmap_bitplaneSizeInBytes;
	add.w	d2,d1
	dbra	d5,sB_loop

	movem.l	(sp)+,d2-d5
	rts

_showSprite__10CopperListFUlUsRC6Sprite:
	move.l	data_(a0),a0
	add.w	d0,d0
	add.w	d0,d0
	add.w	d0,a0
	move.l	spriteData(a1),d0
	add.w	d1,d1
	add.w	d1,d1
	add.w	#$122,d1
	move.w	d1,4(a0)
	move.w	d0,6(a0)
	swap	d0
	subq	#2,d1
	move.w	d1,(a0)
	move.w	d0,2(a0)
	rts

	end

