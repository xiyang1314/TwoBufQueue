	.section	__TEXT,__text,regular,pure_instructions
	.build_version macos, 14, 4	sdk_version 14, 4
	.globl	_ordinary16                     ; -- Begin function ordinary16
	.p2align	2
_ordinary16:                            ; @ordinary16
	.cfi_startproc
; %bb.0:
	ldr	x8, [x0, #64]
	add	x9, x8, #1
	ldr	x10, [x0]
	cmp	x9, x10
	csinc	x9, xzr, x8, eq
	ldr	x10, [x0, #128]
	cmp	x9, x10
	b.ne	LBB0_3
; %bb.1:
	ldapur	x10, [x0, #192]
	str	x10, [x0, #128]
	cmp	x9, x10
	b.ne	LBB0_3
; %bb.2:
	mov	w0, #0                          ; =0x0
	ret
LBB0_3:
	ldr	x10, [x0, #8]
	ldr	q0, [x1]
	add	x8, x10, x8, lsl #4
	str	q0, [x8, #64]
	stlur	x9, [x0, #64]
	mov	w0, #1                          ; =0x1
	ret
	.cfi_endproc
                                        ; -- End function
	.globl	_hinted16                       ; -- Begin function hinted16
	.p2align	2
_hinted16:                              ; @hinted16
	.cfi_startproc
; %bb.0:
	ldr	x8, [x0, #64]
	add	x9, x8, #1
	ldr	x10, [x0]
	cmp	x9, x10
	csinc	x9, xzr, x8, eq
	ldr	x10, [x0, #128]
	cmp	x9, x10
	b.ne	LBB1_3
; %bb.1:
	ldapur	x10, [x0, #192]
	str	x10, [x0, #128]
	cmp	x9, x10
	b.ne	LBB1_3
; %bb.2:
	mov	w0, #0                          ; =0x0
	ret
LBB1_3:
	ldr	x10, [x0, #8]
	add	x8, x10, x8, lsl #4
	prfm	pstl1keep, [x8, #64]
	ldr	q0, [x1]
	str	q0, [x8, #64]
	stlur	x9, [x0, #64]
	mov	w0, #1                          ; =0x1
	ret
	.cfi_endproc
                                        ; -- End function
.subsections_via_symbols
