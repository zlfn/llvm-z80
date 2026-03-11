	.area _CODE
	.globl ___umodhi3

___umodhi3:
	call	___udivhi3	; BC = quotient, HL = remainder
	ld	c, l		; BC = remainder
	ld	b, h
	ret

;===------------------------------------------------------------------------===;
; ___modhi3 - 16-bit signed modulo
;
; Input:  DE = dividend, BC = divisor
; Output: BC = remainder (same sign as dividend, C99)
;===------------------------------------------------------------------------===;
