
work/burst_instruction_20261012/business_warm_owner:	file format mach-o arm64

Disassembly of section __TEXT,__text:

00000001000014ec <__ZN11TwoBufQueue11performance8hot_path8Business4stepEv>:
1000014ec: f9401008    	ldr	x8, [x0, #0x20]
1000014f0: 3940b009    	ldrb	w9, [x0, #0x2c]
1000014f4: 34000209    	cbz	w9, 0x100001534 <__ZN11TwoBufQueue11performance8hot_path8Business4stepEv+0x48>
1000014f8: b9402809    	ldr	w9, [x0, #0x28]
1000014fc: 34000169    	cbz	w9, 0x100001528 <__ZN11TwoBufQueue11performance8hot_path8Business4stepEv+0x3c>
100001500: d28f82aa    	mov	x10, #0x7c15            ; =31765
100001504: f2afe94a    	movk	x10, #0x7f4a, lsl #16
100001508: f2cf372a    	movk	x10, #0x79b9, lsl #32
10000150c: f2f3c6ea    	movk	x10, #0x9e37, lsl #48
100001510: ca083508    	eor	x8, x8, x8, lsl #13
100001514: ca481d08    	eor	x8, x8, x8, lsr #7
100001518: ca084508    	eor	x8, x8, x8, lsl #17
10000151c: 8b0a0108    	add	x8, x8, x10
100001520: 71000529    	subs	w9, w9, #0x1
100001524: 54ffff61    	b.ne	0x100001510 <__ZN11TwoBufQueue11performance8hot_path8Business4stepEv+0x24>
100001528: f9001008    	str	x8, [x0, #0x20]
10000152c: aa0803e0    	mov	x0, x8
100001530: d65f03c0    	ret
100001534: b940280a    	ldr	w10, [x0, #0x28]
100001538: b9401809    	ldr	w9, [x0, #0x18]
10000153c: 340001ea    	cbz	w10, 0x100001578 <__ZN11TwoBufQueue11performance8hot_path8Business4stepEv+0x8c>
100001540: d28f82ab    	mov	x11, #0x7c15            ; =31765
100001544: f2afe94b    	movk	x11, #0x7f4a, lsl #16
100001548: f2cf372b    	movk	x11, #0x79b9, lsl #32
10000154c: f2f3c6eb    	movk	x11, #0x9e37, lsl #48
100001550: f940000c    	ldr	x12, [x0]
100001554: 8b091989    	add	x9, x12, x9, lsl #6
100001558: f940052d    	ldr	x13, [x9, #0x8]
10000155c: ca0801a8    	eor	x8, x13, x8
100001560: 93c8e508    	ror	x8, x8, #0x39
100001564: 8b0b0108    	add	x8, x8, x11
100001568: f9000528    	str	x8, [x9, #0x8]
10000156c: b9400129    	ldr	w9, [x9]
100001570: 7100054a    	subs	w10, w10, #0x1
100001574: 54ffff01    	b.ne	0x100001554 <__ZN11TwoBufQueue11performance8hot_path8Business4stepEv+0x68>
100001578: b9001809    	str	w9, [x0, #0x18]
10000157c: f9001008    	str	x8, [x0, #0x20]
100001580: aa0803e0    	mov	x0, x8
100001584: d65f03c0    	ret
