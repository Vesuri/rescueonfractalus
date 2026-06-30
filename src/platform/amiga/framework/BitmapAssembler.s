	xdef	_clear__6BitmapFUsUsUsUs
	xdef	_copy__6BitmapFRC6BitmapUsUsUsUsUsUsUs
	xdef	_copyWithMask__6BitmapFRC6BitmapRC6BitmapUsUsUsUsUsUsUsUsUc
	xdef	_line__6BitmapFUsUsUsUsUsUc
	xdef	_fill__6BitmapFUsUsUsUs
	xref	_blitterClear__13AmigaHardwareFPUsUsUss
	xref	_blitterCopy__13AmigaHardwareFPUsPUsUsUssssUsUsUs
	xref	_blitterCopyWithMask__13AmigaHardwareFPUsPUsPUsUsUssssssUsUsUc
	xref	_blitterLine__13AmigaHardwareFPUsUsUsUsUsUsUc
	xref	_blitterFill__13AmigaHardwareFPUsUsUss

; Offsets to various C++ structures. These MUST match the C++ structures!
; -----------------------------------------------------------------------
data_		equ	0
width_		equ	4
height_		equ	6
bitplanes_	equ	8
dataWidth_	equ	10
widthInBytes_	equ	12
rowSizeInBytes_	equ	14
bitplaneSizeInBytes_	equ	16
interleaved	equ	18
owner		equ	19
blittable	equ	20

	section	code

_clear__6BitmapFUsUsUsUs:
	movem.l	d2-d7/a2,-(sp)

	tst.w	d2			; if (width == 0) { width = width_; }
	bne.s	cl_widthOk
	move.w	width_(a0),d2
cl_widthOk:
	tst.w	d3			; if (height == 0) { height = height_; }
	bne.s	cl_heightOk
	move.w	height_(a0),d3
cl_heightOk:

	; Calculate blit width in words
	move.w	d0,d4			; uint16_t destFirstWord = x >> 4;
	lsr.w	#4,d4
	move.w	d0,d5			; uint16_t destLastWord = (x + width - 1) >> 4;
	add.w	d2,d5
	subq	#1,d5
	lsr.w	#4,d5
	sub.w	d4,d5			; uint16_t widthWords = destLastWord - destFirstWord + 1;
	addq	#1,d5
	move.w	widthInBytes_(a0),a2	; uint16_t destWidthWords = this->widthInWords();
	sub.w	d5,a2			; int16_t destRowModulo = destWidthWords - widthWords;
	sub.w	d5,a2
	move.l	data_(a0),a1		; uint16_t* destData = (uint16_t*)data_ + y * this->rowSizeInWords() + destFirstWord;
	move.w	rowSizeInBytes_(a0),d7
	mulu.w	d1,d7
	add.w	d4,d7
	add.w	d4,d7
	add.w	d7,a1

	tst.b	blittable(a0)		; if (blittable) {
	beq.s	cl_cpu
	tst.b	interleaved(a0)		; if (interleaved) {
	beq.s	cl_chipNonInterleaved
	move.w	d5,d0			; AmigaHardware::blitterClear(destData, widthWords, bitplanes_ * height, destRowModulo << 1);
	move.w	d3,d1
	mulu.w	bitplanes_(a0),d1
	move.l	a1,d2
	move.w	a2,d3
	bsr	_blitterClear__13AmigaHardwareFPUsUsUss
	bra	cl_done

cl_chipNonInterleaved:
	move.w	bitplaneSizeInBytes_(a0),d6
	move.w	d3,d4
	move.w	bitplanes_(a0),d7	; for (uint16_t i = 0; i < bitplanes_; i++) {
	subq	#1,d7
cl_chipNonInterleavedLoop:
	move.w	d5,d0			; AmigaHardware::blitterClear(destData, widthWords, height, destRowModulo << 1);
	move.w	d4,d1
	move.l	a1,d2
	move.w	a2,d3
	bsr	_blitterClear__13AmigaHardwareFPUsUsUss
	add.w	d6,a1			; destData += this->bitplaneSizeInWords();
	dbra	d7,cl_chipNonInterleavedLoop
	bra	cl_done

cl_cpu:
	moveq	#0,d7			; int16_t destBitplaneModulo = interleaved ? 0 : ((height_ - height) * destWidthWords);
	tst.b	interleaved(a0)
	bne.s	cl_cpuInterleavedOk
	move.w	height_(a0),d7
	sub.w	d1,d7
	mulu.w	d6,d7
cl_cpuInterleavedOk:
	move.w	bitplanes_(a0),d0	; for (uint16_t i = 0; i < bitplanes_; i++) {
	subq	#1,d0
cl_cpuBitplanesLoop:
	move.w	d3,d1			; for (uint16_t j = 0; j < height; j++) {
	subq	#1,d1
cl_cpuHeightLoop:
	move.w	d5,d2			; for (uint16_t k = 0; k < widthWords; k++) {
	subq	#1,d2
cl_cpuWidthLoop:
	clr.w	(a1)+			; *destData++ = 0;
	dbra	d2,cl_cpuWidthLoop
	add.w	a2,a1			; destData += destRowModulo;
	dbra	d1,cl_cpuHeightLoop
	add.w	d7,a1			; destData += destBitplaneModulo;
	dbra	d0,cl_cpuBitplanesLoop

cl_done:
	movem.l	(sp)+,d2-d7/a2
	rts

_copy__6BitmapFRC6BitmapUsUsUsUsUsUsUs:
	movem.l	d2-d7/a2-a6,-(sp)

	tst.w	d4			; if (width == 0) { width = source_width(); }
	bne.s	c_widthOk
	move.w	width_(a1),d4
c_widthOk:
	tst.w	d5			; if (height == 0) { height = source_height(); }
	bne.s	c_heightOk
	move.w	height_(a1),d5
c_heightOk:

	; Pack variables into registers
	swap	d1
	move.w	d0,d1
	swap	d3
	move.w	d2,d3
	swap	d5
	move.w	d4,d5
	swap	d7
	move.w	d3,d7
	; d1 low: destX high: destY
	; d3 low: sourceX high: sourceY
	; d5 low: width high: height
	; d7 low: sourceX high: mask
	; Free: d0, d2, d4, d6

	move.w	d3,d0			; uint16_t sourceFirstWord = sourceX >> 4;
	lsr.w	#4,d0
	move.w	d3,d2			; uint16_t sourceLastWord = (sourceX + width - 1) >> 4;
	add.w	d5,d2
	subq	#1,d2
	lsr.w	#4,d2
	move.w	d2,a3			; uint16_t sourceWords = sourceLastWord - sourceFirstWord;
	sub.w	d0,a3
	; d0 low: sourceFirstWord
	; d2 low: sourceLastWord
	; a3 low: sourceWords

	move.w	d1,d4			; uint16_t destFirstWord = destX >> 4;
	lsr.w	#4,d4
	move.w	d1,d6			; uint16_t destLastWord = (destX + width - 1) >> 4;
	add.w	d5,d6
	subq	#1,d6
	lsr.w	#4,d6
	move.w	d6,a4			; uint16_t destWords = destLastWord - destFirstWord;
	sub.w	d4,a4
 	; d4 low: destFirstWord
 	; d6 low: destLastWord
 	; a4 low: destWords

	move.w	a4,a2			; uint16_t widthWords = destWords;
	cmp.w	a3,a4			; if (sourceWords > destWords)
	bcc.s	c_widthWordsOk
	move.w	a3,a2			; widthWords = sourceWords;
	move.w	d0,d2			; sourceLastWord = (uint16_t)(sourceFirstWord + widthWords);
	add.w	a2,d2
	move.w	d4,d6			; destLastWord = (uint16_t)(destFirstWord + widthWords);
	add.w	a2,d6
c_widthWordsOk:
	add.w	#1,a2			; widthWords++;

	; Pack variables into registers
	swap	d2
	move.w	d0,d2
	swap	d6
	move.w	d4,d6
	move.l	d2,a3
	move.l	d6,a4
	; a2 low: widthWords
	; a3 low: sourceFirstWord high: sourceLastWord
	; a4 low: destFirstWord high: destLastWord
	; Free: d0, d2, d4, d6

	move.w	widthInBytes_(a0),d4	; uint16_t destWidthWords = this->widthInWords();
	sub.w	a2,d4			; int16_t destRowModulo = destWidthWords - widthWords;
	sub.w	a2,d4
	swap	d4
	move.w	widthInBytes_(a1),d4	; uint16_t sourceWidthWords = source_widthInWords();
	sub.w	a2,d4			; int16_t sourceRowModulo = sourceWidthWords - widthWords;
	sub.w	a2,d4
	; d4 low: sourceRowModulo high: destRowModulo

	and.w	#15,d3			; uint16_t sourceLeftShift = sourceX & 15;
	and.w	#15,d1			; uint16_t destRightShift = destX & 15;
	sub.w	d3,d1			; int16_t shift = destRightShift - sourceLeftShift;
	moveq	#-1,d2			; uint16_t firstWordMask = 0xffff; uint16_t lastWordMask = 0xffff;
	move.l	data_(a1),a5		; uint16_t* sourceData = (uint16_t*)source_data();
	move.l	data_(a0),a6		; uint16_t* destData = (uint16_t*)data_;

	; d0 free
	; d1 low: shift high: destY
	; d2 low: firstWordMask high: lastWordMask
	; d3 low: sourceLeftShift high: sourceY
	; d4 low: sourceRowModulo high: destRowModulo
	; d5 low: width high: height
	; d6 free
	; d7 low: sourceX high: mask
	; a0 this
	; a1 source
	; a2 low: widthWords
	; a3 low: sourceFirstWord high: sourceLastWord
	; a4 low: destFirstWord high: destLastWord
	; a5 sourceData
	; a6 destData

	tst.w	d1			; if (shift >= 0) {
	bmi.s	c_shiftNegative

	move.l	d3,d0			; sourceData += sourceY * source_rowSizeInWords() + sourceFirstWord;
	swap	d0
	mulu.w	rowSizeInBytes_(a1),d0
	add.l	d0,a5
	add.w	a3,a5
	add.w	a3,a5

	move.l	d1,d0			; destData += destY * this->rowSizeInWords() + destFirstWord;
	swap	d0
	mulu.w	rowSizeInBytes_(a0),d0
	add.l	d0,a6
	add.w	a4,a6
	add.w	a4,a6

	move.l	a3,d0			; lastWordMask <<= ((sourceLastWord << 4) + 16 - sourceX - width);
	swap	d0
	lsl.w	#4,d0
	add.w	#16,d0
	sub.w	d7,d0
	sub.w	d5,d0
	lsl.w	d0,d2
	swap	d2
	lsr.w	d3,d2			; firstWordMask >>= sourceLeftShift;
	bra	c_shiftOk

c_shiftNegative:
	swap	d5
	; d5 low: height

	move.l	d3,d0			; sourceData += (sourceY + height) * source_rowSizeInWords() - sourceWidthWords + sourceLastWord;
	swap	d0
	add.w	d5,d0
	mulu.w	rowSizeInBytes_(a1),d0
	add.l	d0,a5
	sub.w	widthInBytes_(a1),a5
	move.l	a3,d6
	swap	d6
	add.w	d6,a5
	add.w	d6,a5

	move.l	d1,d0			; destData += (destY + height) * this->rowSizeInWords() - destWidthWords + destLastWord;
	swap	d0
	add.w	d5,d0
	mulu.w	rowSizeInBytes_(a0),d0
	add.l	d0,a6
	sub.w	widthInBytes_(a0),a6
	move.l	a4,d6
	swap	d6
	add.w	d6,a6
	add.w	d6,a6

	swap	d5
	; d5 low: width

	move.l	a3,d0			; firstWordMask <<= ((sourceLastWord << 4) + 16 - sourceX - width);
	swap	d0
	lsl.w	#4,d0
	add.w	#16,d0
	sub.w	d7,d0
	sub.w	d5,d0
	lsl.w	d0,d2
	swap	d2
	lsr.w	d3,d2			; lastWordMask >>= sourceLeftShift
	swap	d2

c_shiftOk:
	tst.b	blittable(a0)		; if (blittable && source_blittable) {
	beq	c_cpu
	tst.b	blittable(a1)
	beq	c_cpu

	tst.b	interleaved(a0)		; if (interleaved) {
	beq.s	c_nonInterleaved

	move.w	a2,d0			; AmigaHardware::blitterCopy(sourceData, destData, widthWords, bitplanes_ * height, sourceRowModulo << 1, destRowModulo << 1, shift, firstWordMask, lastWordMask, mask);
	move.w	d4,a1
	swap	d4
	move.w	d4,a2
	move.w	d1,d4
	move.w	bitplanes_(a0),d1
	swap	d5
	mulu.w	d5,d1
	move.w	d2,d5
	swap	d2
	move.w	d2,d6
	swap	d7
	move.l	a5,d2
	move.l	a6,d3
	bsr	_blitterCopy__13AmigaHardwareFPUsPUsUsUssssUsUsUs
	bra.s	c_done

c_nonInterleaved:
	; TODO
	bra	c_done

c_cpu:
	swap	d5
	; d5 low: height

	moveq	#0,d0			; int16_t sourceBitplaneModulo = source_isInterleaved() ? 0 : ((source_height() - height) * sourceWidthWords);
	tst.b	interleaved(a1)
	bne.s	c_cpuSourceBitplaneModuloOk
	move.w	height_(a1),d0
	sub.w	d5,d0
	mulu	widthInBytes_(a1),d0
c_cpuSourceBitplaneModuloOk:

	moveq	#0,d2			; int16_t destBitplaneModulo = interleaved ? 0 : ((height_ - height) * destWidthWords);
	tst.b	interleaved(a0)
	bne.s	c_cpuDestBitplaneModuloOk
	move.w	height_(a0),d2
	sub.w	d5,d2
	mulu	widthInBytes_(a0),d2
c_cpuDestBitplaneModuloOk:

	move.w	bitplanes_(a0),d6	; uint16_t bitplanes = bitplanes_ < source_bitplanes() ? bitplanes_ : source_bitplanes();
	cmp.w	bitplanes_(a1),d6
	bcs.s	c_cpuBitplanesOk
	move.w	bitplanes_(a1),d6
c_cpuBitplanesOk:

	swap	d7
	; d7 low: mask

	move.w	d5,a3
	subq	#1,a3
	move.w	#12-1,a3
	move.w	d4,a4
	swap	d4
	subq	#1,a2
	subq	#1,d6
; a2 widthWords
; a3 height
; a4 sourceRowModulo
; d0 sourceBitplaneModulo
; d1 k
; d2 destBitplaneModulo
; d3 j
; d4 sourceDestModulo
; d5 free
; d6 i
; d7 mask
c_cpuBitplanesLoop:			; for (uint16_t i = 0; i < bitplanes; i++) {
	move.w	a3,d3
c_cpuYLoop:				; for (uint16_t j = 0; j < height; j++) {
	move.w	a2,d1
c_cpuXLoop:				; for (uint16_t k = 0; k < widthWords; k++) {
	move.w	(a5)+,d5		; *destData++ = (uint16_t)(*sourceData++ & mask);
	and.w	d7,d5
	move.w	d5,(a6)+
	dbra	d1,c_cpuXLoop		; }
	add.w	a4,a5			; sourceData += sourceRowModulo;
	add.w	d4,a6			; destData += destRowModulo;
	dbra	d3,c_cpuYLoop		; }
	add.l	d0,a5			; sourceData += sourceBitplaneModulo;
	add.l	d2,a6			; destData += destBitplaneModulo;
	dbra	d6,c_cpuBitplanesLoop	; }

c_done:	movem.l	(sp)+,d2-d7/a2-a6
	rts

_copyWithMask__6BitmapFRC6BitmapRC6BitmapUsUsUsUsUsUsUsUsUc:
	movem.l	d2-d7/a2-a6,-(sp)
	move.w	a3,-(sp)

	tst.w	d6			; if (width == 0) { width = source_width(); }
	bne.s	cWM_widthOk
	move.w	width_(a1),d6
cWM_widthOk:
	tst.w	d7			; if (height == 0) { height = source_height(); }
	bne.s	cWM_heightOk
	move.w	height_(a1),d7
cWM_heightOk:

	; Pack variables into registers
	swap	d1
	move.w	d0,d1
	swap	d3
	move.w	d2,d3
	swap	d5
	move.w	d4,d5
	swap	d7
	move.w	d6,d7
	; d1 low: destX high: destY
	; d3 low: sourceX high: sourceY
	; d5 low: maskX high: maskY
	; d7 low: width high: height
	; Free: d0, d2, d4, d6

	move.w	d3,d0			; uint16_t sourceFirstWord = sourceX >> 4;
	lsr.w	#4,d0
	move.w	d3,d2			; uint16_t sourceLastWord = (sourceX + width - 1) >> 4;
	add.w	d7,d2
	subq	#1,d2
	lsr.w	#4,d2
	move.w	d2,a4			; uint16_t sourceWords = sourceLastWord - sourceFirstWord;
	sub.w	d0,a4
	; d0 low: sourceFirstWord
	; d2 low: sourceLastWord
	; a4 low: sourceWords

	move.w	d1,d4			; uint16_t destFirstWord = destX >> 4;
	lsr.w	#4,d4
	move.w	d1,d6			; uint16_t destLastWord = (destX + width - 1) >> 4;
	add.w	d7,d6
	subq	#1,d6
	lsr.w	#4,d6
	move.w	d6,a5			; uint16_t destWords = destLastWord - destFirstWord;
	sub.w	d4,a5
 	; d4 low: destFirstWord
 	; d6 low: destLastWord
 	; a5 low: destWords

	move.w	a5,a3			; uint16_t widthWords = destWords;
	cmp.w	a4,a5			; if (sourceWords > destWords)
	bcc.s	cWM_widthWordsOk
	move.w	a4,a3			; widthWords = sourceWords;
	move.w	d0,d2			; sourceLastWord = (uint16_t)(sourceFirstWord + widthWords);
	add.w	a3,d2
	move.w	d4,d6			; destLastWord = (uint16_t)(destFirstWord + widthWords);
	add.w	a3,d6
cWM_widthWordsOk:
	; Pack variables into registers
	swap	d2
	move.w	d0,d2
	swap	d6
	move.w	d4,d6
	; d2 low: sourceFirstWord high: sourceLastWord
	; d6 low: destFirstWord high: destLastWord
	; a3 low: widthWords
	; Free: d0, d4

	move.w	d5,d0			; uint16_t maskFirstWord = maskX >> 4;
	lsr.w	#4,d0
	move.w	d0,d4			; uint16_t maskLastWord = (uint16_t)(maskFirstWord + widthWords);
	add.w	a3,d4
	add.w	#1,a3			; widthWords++;

	; Pack variables into registers
	swap	d4
	move.w	d0,d4
	; d4 low: maskFirstWord high: maskLastWord
	; Free: d0

	move.l	d2,a4
	move.l	d4,a5
	move.l	d6,a6
	; a4 low: sourceFirstWord high: sourceLastWord
	; a5 low: maskFirstWord high: maskLastWord
	; a6 low: destFirstWord high: destLastWord
	; Free: d0, d2, d4, d6

	move.w	widthInBytes_(a2),d4	; uint16_t maskWidthWords = mask_widthInWords();
	sub.w	a3,d4			; int16_t maskRowModulo = maskWidthWords - widthWords;
	sub.w	a3,d4
	swap	d4
	move.w	widthInBytes_(a1),d4	; uint16_t sourceWidthWords = source_widthInWords();
	sub.w	a3,d4			; int16_t sourceRowModulo = sourceWidthWords - widthWords;
	sub.w	a3,d4
	; d4 low: sourceRowModulo high: maskRowModulo

	move.w	widthInBytes_(a0),d6	; uint16_t destWidthWords = this->widthInWords();
	sub.w	a3,d6			; int16_t destRowModulo = destWidthWords - widthWords;
	sub.w	a3,d6
	swap	d6
	move.w	a3,d6
	; d6 low: widthWords high: destRowModulo
	; Free: d0,d2

	move.w	d3,d0
	and.w	#15,d3			; uint16_t sourceLeftShift = sourceX & 15;
	and.w	#15,d5			; uint16_t maskLeftShift = maskX & 15;
	and.w	#15,d1			; uint16_t destRightShift = destX & 15;
	neg.w	d3			; int16_t sourceShift = destRightShift - sourceLeftShift;
	add.w	d1,d3
	sub.w	d5,d1			; int16_t maskShift = destRightShift - maskLeftShift;
	move.w	d0,d5
	moveq	#-1,d2			; uint16_t firstWordMask = 0xffff; uint16_t lastWordMask = 0xffff;

	; d0 free
	; d1 low: maskShift high: destY
	; d2 low: firstWordMask high: lastWordMask
	; d3 low: sourceShift high: sourceY
	; d4 low: sourceRowModulo high: maskRowModulo
	; d5 low: sourceX high: maskY
	; d6 low: widthWords high: destRowModulo
	; d7 low: width high: height
	; a0 this
	; a1 source
	; a2 mask
	; a3 free
	; a4 low: sourceFirstWord high: sourceLastWord
	; a5 low: maskFirstWord high: maskLastWord
	; a6 low: destFirstWord high: destLastWord

	sub.w	#12,sp
	tst.w	d3			; if (sourceShift >= 0) {
	bmi.s	cWM_sourceShiftNegative

	move.l	d3,d0			; sourceData += sourceY * source_rowSizeInWords() + sourceFirstWord;
	swap	d0
	mulu.w	rowSizeInBytes_(a1),d0
	move.l	d0,a3
	add.w	a4,a3
	add.w	a4,a3
	add.l	data_(a1),a3
	move.l	a3,(sp)

	move.l	d5,d0			; maskData += maskY * mask_rowSizeInWords() + maskFirstWord;
	swap	d0
	mulu.w	rowSizeInBytes_(a2),d0
	move.l	d0,a3
	add.w	a5,a3
	add.w	a5,a3
	add.l	data_(a2),a3
	move.l	a3,8(sp)

	move.l	d1,d0			; destData += destY * this->rowSizeInWords() + destFirstWord;
	swap	d0
	mulu.w	rowSizeInBytes_(a0),d0
	move.l	d0,a3
	add.w	a6,a3
	add.w	a6,a3
	add.l	data_(a0),a3
	move.l	a3,4(sp)

	move.l	a4,d0			; lastWordMask <<= ((sourceLastWord << 4) + 16 - sourceX - width);
	swap	d0
	lsl.w	#4,d0
	add.w	#16,d0
	sub.w	d5,d0
	sub.w	d7,d0
	lsl.w	d0,d2
	swap	d2
	and.w	#15,d5
	lsr.w	d5,d2			; firstWordMask >>= sourceLeftShift;

	tst.w	d1			; if (maskShift < 0) {
	bpl	cWM_sourceShiftOk
	add.w	#16,d1			; maskShift += 16;
	bra	cWM_sourceShiftOk

cWM_sourceShiftNegative:
	swap	d7
	; d7 low: height

	move.l	d3,d0			; sourceData += (sourceY + height) * source_rowSizeInWords() - sourceWidthWords + sourceLastWord;
	swap	d0
	add.w	d7,d0
	mulu.w	rowSizeInBytes_(a1),d0
	move.l	d0,a3
	sub.w	widthInBytes_(a1),a3
	move.l	a4,d0
	swap	d0
	add.w	d0,a3
	add.w	d0,a3
	add.l	data_(a1),a3
	move.l	a3,(sp)

	move.l	d5,d0			; maskData += (maskY + height) * mask_rowSizeInWords() - maskWidthWords + maskLastWord;
	swap	d0
	add.w	d7,d0
	mulu.w	rowSizeInBytes_(a2),d0
	move.l	d0,a3
	sub.w	widthInBytes_(a2),a3
	move.l	a5,d0
	swap	d0
	add.w	d0,a3
	add.w	d0,a3
	add.l	data_(a2),a3
	move.l	a3,8(sp)

	move.l	d1,d0			; destData += (destY + height) * this->rowSizeInWords() - destWidthWords + destLastWord;
	swap	d0
	add.w	d7,d0
	mulu.w	rowSizeInBytes_(a0),d0
	move.l	d0,a3
	sub.w	widthInBytes_(a0),a3
	move.l	a6,d0
	swap	d0
	add.w	d0,a3
	add.w	d0,a3
	add.l	data_(a0),a3
	move.l	a3,4(sp)

	swap	d7
	; d7 low: width

	move.l	a4,d0			; firstWordMask <<= ((sourceLastWord << 4) + 16 - sourceX - width);
	swap	d0
	lsl.w	#4,d0
	add.w	#16,d0
	sub.w	d5,d0
	sub.w	d7,d0
	lsl.w	d0,d2
	swap	d2
	and.w	#15,d5
	lsr.w	d5,d2			; lastWordMask >>= sourceLeftShift
	swap	d2

	tst.w	d1			; if (maskShift > 0) {
	ble.s	cWM_sourceShiftOk
	sub.w	#16,d1			; maskShift -= 16;
	sub.l	#2,8(sp)		; maskData--;
cWM_sourceShiftOk:

	tst.b	interleaved(a0)		; if (interleaved) {
	beq	cWM_nonInterleaved

	cmp.w	#1,bitplanes_(a2)	; if (mask_bitplanes() == 1) {
	bne	cWM_interleavedMultiBitplaneMask

	move.w	bitplanes_(a0),a4	; uint16_t bitplanes = this->bitplanes < source_bitplanes ? this->bitplanes : source_bitplanes;
	cmp.w	bitplanes_(a1),a4
	bcs.s	cWM_interleavedBitplanesOk
	move.w	bitplanes_(a1),a4
cWM_interleavedBitplanesOk:
	add.w	rowSizeInBytes_(a1),d4	; sourceRowModulo += source_rowSizeInWords() - sourceWidthWords;
	sub.w	widthInBytes_(a1),d4
	swap	d6			; destRowModulo += this->rowSizeInWords() - destWidthWords;
	add.w	rowSizeInBytes_(a0),d6
	sub.w	widthInBytes_(a0),d6
	swap	d6

	movem.l	d0-d7/a0-a3/a5-a6,-(sp)
cWM_interleavedSingleBitplaneMaskLoop:	; for (uint16_t i = 0; i < bitplanes; i++) {
	; AmigaHardware::blitterCopyWithMask(sourceData, destData, maskData, widthWords, height, sourceRowModulo << 1, destRowModulo << 1, maskRowModulo << 1, sourceShift, maskShift, firstWordMask, lastWordMask, clearMasked);
	move.w	d2,a5	; firstWordMask
	swap	d2
	move.w	d2,a6	; lastWordMask
	move.w	d3,d5	; sourceShift
	move.w	d4,a1	; sourceModulo
	swap	d4
	move.w	d4,a3	; maskModulo
	move.w	d6,d0	; widthWords
	swap	d6
	move.w	d6,a2	; destRowModulo
	move.w	d1,d6	; maskShift
	swap	d7	; height
	move.w	d7,d1
	move.l	56(sp),d2	; sourceData
	move.l	60(sp),d3	; maskData
	move.l	64(sp),d4	; destData
	move.w	68(sp),d7	; clearMasked
	bsr	_blitterCopyWithMask__13AmigaHardwareFPUsPUsPUsUsUssssssUsUsUc
	movem.l	(sp)+,d0-d7/a0-a3/a5-a6

	subq	#1,a4
	cmp.w	#0,a4
	beq.s	cWM_interleavedSingleBitplaneMaskDone

	move.w	widthInBytes_(a1),d0	; sourceData += sourceShift >= 0 ? sourceWidthWords : -sourceWidthWords;
	tst.w	d3
	bpl.s	cWM_sourceAddOk
	neg.w	d0
cWM_sourceAddOk:
	ext.l	d0
	add.l	d0,(sp)

	move.w	widthInBytes_(a0),d0	; destData += sourceShift >= 0 ? destWidthWords : -destWidthWords;
	tst.w	d3
	bpl.s	cWM_destAddOk
	neg.w	d0
cWM_destAddOk:
	ext.l	d0
	add.l	d0,4(sp)

	lea	-56(sp),sp
	bra.s	cWM_interleavedSingleBitplaneMaskLoop

cWM_interleavedSingleBitplaneMaskDone:
	lea	14(sp),sp
	bra.s	cWM_done

cWM_interleavedMultiBitplaneMask:
	; AmigaHardware::blitterCopyWithMask(sourceData, destData, maskData, widthWords, bitplanes_ * height, sourceRowModulo << 1, destRowModulo << 1, maskRowModulo << 1, sourceShift, maskShift, firstWordMask, lastWordMask, clearMasked);
	move.w	d2,a5	; firstWordMask
	swap	d2
	move.w	d2,a6	; lastWordMask
	move.w	d3,d5	; sourceShift
	move.w	d4,a1	; sourceModulo
	swap	d4
	move.w	d4,a3	; maskModulo
	move.w	d6,d0	; widthWords
	swap	d6
	move.w	d6,a2	; destRowModulo
	move.w	d1,d6	; maskShift
	swap	d7	; height
	move.w	d7,d1
	mulu.w	bitplanes_(a0),d1
	movem.l	(sp)+,d2/d3/d4
	move.w	(sp)+,d7
	bsr	_blitterCopyWithMask__13AmigaHardwareFPUsPUsPUsUsUssssssUsUsUc
	bra.s	cWM_done

cWM_nonInterleaved:

	; TODO
	add.w	#14,sp

cWM_done:
	movem.l	(sp)+,d2-d7/a2-a6
	rts

_line__6BitmapFUsUsUsUsUsUc:
	movem.l	d2-d7/a2,-(sp)

	move.l	data_(a0),a1		; uint16_t* destData = (uint16_t*)data;
	move.w	widthInBytes_(a0),d7	; uint16_t destWidthWords = this->widthInWords();
	tst.b	interleaved(a0)		; int16_t destBitplaneModulo = interleaved ? destWidthWords : (height * destWidthWords);
	bne.s	l_destBitplaneModuloOk
	mulu.w	height_(a0),d7
l_destBitplaneModuloOk:
	sub.l	a2,a2
	move.w	d7,a2

	move.w	bitplanes_(a0),d7	; for (uint16_t i = 0; i < bitplanes && color != 0; i++, color >>= 1, destData += destBitplaneModulo) {
	subq	#1,d7
l_loop:	btst	#0,d4			; if (color & 1) {
	beq.s	l_colorDone
	movem.l	d0/d1/d4/a1,-(sp)	; AmigaHardware::blitterLine(destData, x1, y1, x2, y2, rowSizeInBytes, fillMode);
	move.l	a1,d4
	move.w	rowSizeInBytes_(a0),d5
	bsr	_blitterLine__13AmigaHardwareFPUsUsUsUsUsUsUc
	movem.l	(sp)+,d0/d1/d4/a1
l_colorDone:
	lsr.w	#1,d4
	beq.s	l_done
	add.l	a2,a1
	dbra	d7,l_loop

l_done:	movem.l	(sp)+,d2-d7/a2
	rts

_fill__6BitmapFUsUsUsUs:
	movem.l	d2-d7/a2,-(sp)

	tst.w	d2			; if (fillWidth == 0) { fillWidth = width; }
	bne.s	f_widthOk
	move.w	width_(a0),d2
f_widthOk:
	tst.w	d3			; if (fillHeight == 0) { fillHeight = height; }
	bne.s	f_heightOk
	move.w	height_(a0),d3
f_heightOk:

	; Calculate blit width in words
	move.w	d0,d4			; uint16_t destFirstWord = x >> 4;
	lsr.w	#4,d4
	move.w	d0,d5			; uint16_t destLastWord = (x + fillWidth - 1) >> 4;
	add.w	d2,d5
	subq	#1,d5
	lsr.w	#4,d5
	sub.w	d5,d4			; uint16_t widthWords = destLastWord - destFirstWord + 1;
	subq	#1,d4
	neg.w	d4
	move.w	widthInBytes_(a0),a2	; uint16_t destWidthWords = this->widthInWords();
	sub.w	d4,a2			; int16_t destRowModulo = destWidthWords - widthWords;
	sub.w	d4,a2
	move.l	data_(a0),a1		; uint16_t* destData = (uint16_t*)data_ + (y + fillHeight - 1) * this->rowSizeInWords() + destLastWord;
	move.w	d1,d6
	add.w	d3,d6
	subq	#1,d6
	move.w	rowSizeInBytes_(a0),d7
	mulu.w	d6,d7
	add.w	d5,d7
	add.w	d5,d7
	add.w	d7,a1

	tst.b	blittable(a0)		; if (blittable) {
	beq.s	f_done
	tst.b	interleaved(a0)		; if (interleaved) {
	beq.s	f_chipNonInterleaved
	move.w	d4,d0			; AmigaHardware::blitterFill(destData, widthWords, bitplanes_ * fillHeight, destRowModulo << 1);
	move.w	d3,d1
	mulu.w	bitplanes_(a0),d1
	move.l	a1,d2
	move.w	a2,d3
	bsr	_blitterFill__13AmigaHardwareFPUsUsUss
	bra	f_done

f_chipNonInterleaved:
	move.w	bitplaneSizeInBytes_(a0),d6
	move.w	d3,d5
	move.w	bitplanes_(a0),d7	; for (uint16_t i = 0; i < bitplanes_; i++) {
	subq	#1,d7
f_chipNonInterleavedLoop:
	move.w	d4,d0			; AmigaHardware::blitterFill(destData, widthWords, fillHeight, destRowModulo << 1);
	move.w	d5,d1
	move.l	a1,d2
	move.w	a2,d3
	bsr	_blitterFill__13AmigaHardwareFPUsUsUss
	sub.w	d6,a1			; destData -= this->bitplaneSizeInWords();
	dbra	d7,f_chipNonInterleavedLoop

f_done:	movem.l	(sp)+,d2-d7/a2
	rts

	end
