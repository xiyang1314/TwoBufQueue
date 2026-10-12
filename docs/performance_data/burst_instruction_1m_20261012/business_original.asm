
work/burst_instruction_20261012/build-pmu/test_burst_cache:	file format mach-o arm64

Disassembly of section __TEXT,__text:

0000000100015e38 <__ZN11TwoBufQueue11performance8hot_path8Business4stepEv>:
100015e38: f9401008    	ldr	x8, [x0, #0x20]
100015e3c: 3940b009    	ldrb	w9, [x0, #0x2c]
100015e40: 34000209    	cbz	w9, 0x100015e80 <__ZN11TwoBufQueue11performance8hot_path8Business4stepEv+0x48>
100015e44: b9402809    	ldr	w9, [x0, #0x28]
100015e48: 34000169    	cbz	w9, 0x100015e74 <__ZN11TwoBufQueue11performance8hot_path8Business4stepEv+0x3c>
100015e4c: d28f82aa    	mov	x10, #0x7c15            ; =31765
100015e50: f2afe94a    	movk	x10, #0x7f4a, lsl #16
100015e54: f2cf372a    	movk	x10, #0x79b9, lsl #32
100015e58: f2f3c6ea    	movk	x10, #0x9e37, lsl #48
100015e5c: ca083508    	eor	x8, x8, x8, lsl #13
100015e60: ca481d08    	eor	x8, x8, x8, lsr #7
100015e64: ca084508    	eor	x8, x8, x8, lsl #17
100015e68: 8b0a0108    	add	x8, x8, x10
100015e6c: 71000529    	subs	w9, w9, #0x1
100015e70: 54ffff61    	b.ne	0x100015e5c <__ZN11TwoBufQueue11performance8hot_path8Business4stepEv+0x24>
100015e74: f9001008    	str	x8, [x0, #0x20]
100015e78: aa0803e0    	mov	x0, x8
100015e7c: d65f03c0    	ret
100015e80: b940280a    	ldr	w10, [x0, #0x28]
100015e84: b9401809    	ldr	w9, [x0, #0x18]
100015e88: 340001ea    	cbz	w10, 0x100015ec4 <__ZN11TwoBufQueue11performance8hot_path8Business4stepEv+0x8c>
100015e8c: d28f82ab    	mov	x11, #0x7c15            ; =31765
100015e90: f2afe94b    	movk	x11, #0x7f4a, lsl #16
100015e94: f2cf372b    	movk	x11, #0x79b9, lsl #32
100015e98: f2f3c6eb    	movk	x11, #0x9e37, lsl #48
100015e9c: f940000c    	ldr	x12, [x0]
100015ea0: 8b091989    	add	x9, x12, x9, lsl #6
100015ea4: f940052d    	ldr	x13, [x9, #0x8]
100015ea8: ca0801a8    	eor	x8, x13, x8
100015eac: 93c8e508    	ror	x8, x8, #0x39
100015eb0: 8b0b0108    	add	x8, x8, x11
100015eb4: f9000528    	str	x8, [x9, #0x8]
100015eb8: b9400129    	ldr	w9, [x9]
100015ebc: 7100054a    	subs	w10, w10, #0x1
100015ec0: 54ffff01    	b.ne	0x100015ea0 <__ZN11TwoBufQueue11performance8hot_path8Business4stepEv+0x68>
100015ec4: b9001809    	str	w9, [x0, #0x18]
100015ec8: f9001008    	str	x8, [x0, #0x20]
100015ecc: aa0803e0    	mov	x0, x8
100015ed0: d65f03c0    	ret
