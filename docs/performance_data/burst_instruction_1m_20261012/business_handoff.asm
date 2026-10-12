
work/burst_instruction_20261012/business_cpu_handoff:	file format mach-o arm64

Disassembly of section __TEXT,__text:

0000000100001514 <__ZN11TwoBufQueue11performance8hot_path8Business4stepEv>:
100001514: f9401008    	ldr	x8, [x0, #0x20]
100001518: 3940b009    	ldrb	w9, [x0, #0x2c]
10000151c: 34000209    	cbz	w9, 0x10000155c <__ZN11TwoBufQueue11performance8hot_path8Business4stepEv+0x48>
100001520: b9402809    	ldr	w9, [x0, #0x28]
100001524: 34000169    	cbz	w9, 0x100001550 <__ZN11TwoBufQueue11performance8hot_path8Business4stepEv+0x3c>
100001528: d28f82aa    	mov	x10, #0x7c15            ; =31765
10000152c: f2afe94a    	movk	x10, #0x7f4a, lsl #16
100001530: f2cf372a    	movk	x10, #0x79b9, lsl #32
100001534: f2f3c6ea    	movk	x10, #0x9e37, lsl #48
100001538: ca083508    	eor	x8, x8, x8, lsl #13
10000153c: ca481d08    	eor	x8, x8, x8, lsr #7
100001540: ca084508    	eor	x8, x8, x8, lsl #17
100001544: 8b0a0108    	add	x8, x8, x10
100001548: 71000529    	subs	w9, w9, #0x1
10000154c: 54ffff61    	b.ne	0x100001538 <__ZN11TwoBufQueue11performance8hot_path8Business4stepEv+0x24>
100001550: f9001008    	str	x8, [x0, #0x20]
100001554: aa0803e0    	mov	x0, x8
100001558: d65f03c0    	ret
10000155c: b940280a    	ldr	w10, [x0, #0x28]
100001560: b9401809    	ldr	w9, [x0, #0x18]
100001564: 340001ea    	cbz	w10, 0x1000015a0 <__ZN11TwoBufQueue11performance8hot_path8Business4stepEv+0x8c>
100001568: d28f82ab    	mov	x11, #0x7c15            ; =31765
10000156c: f2afe94b    	movk	x11, #0x7f4a, lsl #16
100001570: f2cf372b    	movk	x11, #0x79b9, lsl #32
100001574: f2f3c6eb    	movk	x11, #0x9e37, lsl #48
100001578: f940000c    	ldr	x12, [x0]
10000157c: 8b091989    	add	x9, x12, x9, lsl #6
100001580: f940052d    	ldr	x13, [x9, #0x8]
100001584: ca0801a8    	eor	x8, x13, x8
100001588: 93c8e508    	ror	x8, x8, #0x39
10000158c: 8b0b0108    	add	x8, x8, x11
100001590: f9000528    	str	x8, [x9, #0x8]
100001594: b9400129    	ldr	w9, [x9]
100001598: 7100054a    	subs	w10, w10, #0x1
10000159c: 54ffff01    	b.ne	0x10000157c <__ZN11TwoBufQueue11performance8hot_path8Business4stepEv+0x68>
1000015a0: b9001809    	str	w9, [x0, #0x18]
1000015a4: f9001008    	str	x8, [x0, #0x20]
1000015a8: aa0803e0    	mov	x0, x8
1000015ac: d65f03c0    	ret
