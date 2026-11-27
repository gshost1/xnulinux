// SPDX-License-Identifier: APSL-2.0
/* World switch, see world.h. No PAC instructions here: keys change under our feet.
 * x18 is Linux's shadow call stack pointer (CONFIG_SHADOW_CALL_STACK), which belongs to the Linux task that entered the
 * world on this CPU. XNU does not use it or save it per thread, so it stays this CPU's while XNU runs (Linux code XNU
 * calls pushes and pops on it, balanced); the way out puts back the value the task entered with.
 * TPIDR_EL1 stays Linux's: the LINUX64 XNU keeps its thread pointer in TPIDRRO_EL0, so a fault taken in XNU code
 * reaches Linux with a sane per-cpu base and becomes a normal oops. Each CPU has its own save area. */
/*
 * This CPU's save area: xnu_worlds[slot], slot = affinity 0 and 1 of MPIDR_EL1, four bits each (LINUX64_CPU_SLOT).
 * Clobbers x10.
 */
#define W(reg) mrs reg, mpidr_el1; and x10, reg, #0xf; ubfx reg, reg, #8, #4; orr reg, x10, reg, lsl #4; \
	adrp x10, xnu_worlds; add x10, x10, :lo12:xnu_worlds; ldr reg, [x10, reg, lsl #3]

/*
 * The user half (TTBR0, TCR.T0SZ) belongs to whichever world runs: XNU switches TTBR0 to its user pmaps (with its own
 * ASIDs and T0SZ), Linux to the current task's mm. Each switch saves one and loads the other, then drops the local TLB
 * (the two worlds' ASIDs are unrelated). x9 = &xnu_W; clobbers x10.
 */
.macro TO_XNU_MM
	mrs	x10, ttbr0_el1
	str	x10, [x9, #416]
	mrs	x10, tcr_el1
	str	x10, [x9, #424]
	ldr	x10, [x9, #440]
	msr	tcr_el1, x10
	ldr	x10, [x9, #432]
	msr	ttbr0_el1, x10
	isb
	tlbi	vmalle1
	dsb	nsh
	isb
.endm
.macro TO_LINUX_MM
	mrs	x10, ttbr0_el1
	str	x10, [x9, #432]
	mrs	x10, tcr_el1
	str	x10, [x9, #440]
	ldr	x10, [x9, #424]
	msr	tcr_el1, x10
	ldr	x10, [x9, #416]
	msr	ttbr0_el1, x10
	isb
	tlbi	vmalle1
	dsb	nsh
	isb
.endm

	.text
	.globl	xnu_world_call
	.type	xnu_world_call, %function
xnu_world_call:
	W(x9)
	str	x18, [x9, #464]
	stp	x19, x20, [x9, #0]
	stp	x21, x22, [x9, #16]
	stp	x23, x24, [x9, #32]
	stp	x25, x26, [x9, #48]
	stp	x27, x28, [x9, #64]
	stp	x29, x30, [x9, #80]
	mov	x10, sp
	str	x10, [x9, #96]
	mov	x19, x0
	mov	x20, x1
	add	x0, x9, #328		/* load XNU keys */
	add	x1, x9, #240		/* save Linux keys */
	bl	fki_enter		/* no preemption, no IRQs */
	W(x9)
	str	x0, [x9, #104]
	msr	daifset, #0xf		/* XNU's kernel runs with debug exceptions masked (PSTATE.D) */
	mrs	x10, tpidr_el0		/* XNU writes the user TLS registers of the thread it believes current */
	str	x10, [x9, #216]
	mrs	x10, tpidrro_el0
	str	x10, [x9, #224]
	TO_XNU_MM
	/* XNU runs on the Linux task stack: Linux's entry code treats any other stack as overflow. */
	mov	x0, x20
	blr	x19
	/* fn returned: XNU keys and thread pointer are live */
	W(x9)
	mov	x19, #0
	b	2f
xnu_world_abort:			/* from the upcall: Linux keys and TPIDR are live */
	W(x9)
	mov	x19, #1
2:	TO_LINUX_MM
	ldr	x18, [x9, #464]
	ldr	x10, [x9, #216]
	msr	tpidr_el0, x10
	ldr	x10, [x9, #224]
	msr	tpidrro_el0, x10
	ldr	x10, [x9, #96]
	mov	sp, x10
	ldr	x0, [x9, #104]
	add	x1, x9, #240
	bl	fki_leave		/* Linux keys, IRQs, preemption back */
	mov	x0, x19
	W(x9)
	ldp	x19, x20, [x9, #0]
	ldp	x21, x22, [x9, #16]
	ldp	x23, x24, [x9, #32]
	ldp	x25, x26, [x9, #48]
	ldp	x27, x28, [x9, #64]
	ldp	x29, x30, [x9, #80]
	ret
	.size	xnu_world_call, . - xnu_world_call

	.globl	xnu_world_fault_abort
	.type	xnu_world_fault_abort, %function
xnu_world_fault_abort:			/* exception return here: XNU keys live, XNU stack */
	W(x9)
	add	x0, x9, #240		/* load Linux keys */
	add	x1, x9, #328		/* save XNU keys */
	bl	fki_pac_swap
	b	xnu_world_abort
	.size	xnu_world_fault_abort, . - xnu_world_fault_abort

	.globl	xnu_world_upcall
	.type	xnu_world_upcall, %function
xnu_world_upcall:			/* x0 = stage, called by XNU with blr */
	hint	#34			/* bti c: module text may be guarded */
	W(x9)
	stp	x19, x20, [x9, #112]
	stp	x21, x22, [x9, #128]
	stp	x23, x24, [x9, #144]
	stp	x25, x26, [x9, #160]
	stp	x27, x28, [x9, #176]
	stp	x29, x30, [x9, #192]
	mov	x10, sp
	str	x10, [x9, #208]
	mov	x19, x0
	mov	x20, x1
	mov	x21, x2
	add	x11, x9, #472
	stp	d8,  d9,  [x11, #0]
	stp	d10, d11, [x11, #16]
	stp	d12, d13, [x11, #32]
	stp	d14, d15, [x11, #48]
	mrs	x10, fpcr
	str	x10, [x9, #536]
	/* Non-sleeping IRQ-disabled host calls never touch TTBR0 or schedule Linux tasks: skip TLBI mm switches. */
	cmp	w0, #841		/* LINUX64_CALL_IPI */
	b.eq	L_fast_upcall
	cmp	w0, #817		/* LINUX64_CALL_NET_RECV (no fki_linux_runs) */
	b.eq	L_fast_upcall
	cmp	w0, #812		/* LINUX64_CALL_WALLCLOCK */
	b.eq	L_fast_upcall
	cmp	w0, #822		/* LINUX64_CALL_RANDOM */
	b.eq	L_fast_upcall
	cmp	w0, #821		/* LINUX64_CALL_DISK_SIZE */
	b.eq	L_fast_upcall
	cmp	w0, #995		/* LINUX64_CALL_RAMDISK_SIZE */
	b.eq	L_fast_upcall
	cmp	w0, #996		/* LINUX64_CALL_CONSOLE_WRITE */
	b.eq	L_fast_upcall
	cmp	w0, #998		/* LINUX64_CALL_KPRINTF */
	b.eq	L_fast_upcall
	cmp	w0, #840		/* LINUX64_CALL_CPU_START */
	b.eq	L_fast_upcall
	/*
	 * The current XNU thread's user TLS registers: Linux may schedule during the upcall (idle, file I/O), and its
	 * context switch rewrites TPIDR_EL0 and zeroes TPIDRRO_EL0 (where Darwin user code keeps its TSD pointer).
	 */
	mrs	x10, tpidr_el0
	str	x10, [x9, #448]
	mrs	x10, tpidrro_el0
	str	x10, [x9, #456]
	TO_LINUX_MM
	add	x0, x9, #240		/* load Linux keys */
	add	x1, x9, #328		/* save XNU keys */
	bl	fki_pac_swap
	mov	x0, x19
	mov	x1, x20
	mov	x2, x21
	bl	xnu_hook
	cbnz	x0, xnu_world_abort
	W(x9)
	ldr	x10, [x9, #448]
	msr	tpidr_el0, x10
	ldr	x10, [x9, #456]
	msr	tpidrro_el0, x10
	TO_XNU_MM
	add	x0, x9, #328		/* load XNU keys */
	add	x1, x9, #240		/* save Linux keys */
	bl	fki_pac_swap
	b	L_upcall_out
L_fast_upcall:
	add	x0, x9, #240		/* load Linux keys */
	add	x1, x9, #328		/* save XNU keys */
	bl	fki_pac_swap
	mov	x0, x19
	mov	x1, x20
	mov	x2, x21
	bl	xnu_hook
	cbnz	x0, xnu_world_abort
	W(x9)
	add	x0, x9, #328		/* load XNU keys */
	add	x1, x9, #240		/* save Linux keys */
	bl	fki_pac_swap
L_upcall_out:
	W(x9)
	ldr	x10, [x9, #536]
	msr	fpcr, x10
	add	x11, x9, #472
	ldp	d8,  d9,  [x11, #0]
	ldp	d10, d11, [x11, #16]
	ldp	d12, d13, [x11, #32]
	ldp	d14, d15, [x11, #48]
	ldr	x10, [x9, #208]
	mov	sp, x10
	ldp	x19, x20, [x9, #112]
	ldp	x21, x22, [x9, #128]
	ldp	x23, x24, [x9, #144]
	ldp	x25, x26, [x9, #160]
	ldp	x27, x28, [x9, #176]
	ldp	x29, x30, [x9, #192]
	ret
	.size	xnu_world_upcall, . - xnu_world_upcall

/*
 * Deferred XNU kernel fault (see xnu_fault in xnu_main.c): Linux's exception returns here with the faulting context's
 * registers, stack and PSTATE. Save them, let xnu_fault_deferred resolve the fault in XNU's context (it returns where
 * to go and may set x0 for a copyio recovery; frame[31] is the SPSR), then eret there with every register restored.
 */
	.globl	xnu_fault_tramp
	.type	xnu_fault_tramp, %function
xnu_fault_tramp:
	sub	sp, sp, #800
	stp	x0, x1, [sp, #0]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x19, [sp, #144]
	stp	x20, x21, [sp, #160]
	stp	x22, x23, [sp, #176]
	stp	x24, x25, [sp, #192]
	stp	x26, x27, [sp, #208]
	stp	x28, x29, [sp, #224]
	str	x30, [sp, #240]
	stp	q0, q1, [sp, #256]
	stp	q2, q3, [sp, #288]
	stp	q4, q5, [sp, #320]
	stp	q6, q7, [sp, #352]
	stp	q8, q9, [sp, #384]
	stp	q10, q11, [sp, #416]
	stp	q12, q13, [sp, #448]
	stp	q14, q15, [sp, #480]
	stp	q16, q17, [sp, #512]
	stp	q18, q19, [sp, #544]
	stp	q20, q21, [sp, #576]
	stp	q22, q23, [sp, #608]
	stp	q24, q25, [sp, #640]
	stp	q26, q27, [sp, #672]
	stp	q28, q29, [sp, #704]
	stp	q30, q31, [sp, #736]
	mrs	x1, fpsr
	mrs	x2, fpcr
	str	x1, [sp, #768]
	str	x2, [sp, #776]
	/*
	 * PAN on, as a real exception entry would set it (copyio runs with PAN off; vm_fault may block, and XNU panics on
	 * a context switch with PAN off). The eret below restores the faulting context's PSTATE, PAN included.
	 */
	.inst	0xd500419f		/* msr pan, #1 */
	mov	x0, sp
	bl	xnu_fault_deferred
	msr	elr_el1, x0
	ldr	x1, [sp, #248]
	msr	spsr_el1, x1
	ldr	x1, [sp, #768]
	ldr	x2, [sp, #776]
	msr	fpsr, x1
	msr	fpcr, x2
	ldp	q0, q1, [sp, #256]
	ldp	q2, q3, [sp, #288]
	ldp	q4, q5, [sp, #320]
	ldp	q6, q7, [sp, #352]
	ldp	q8, q9, [sp, #384]
	ldp	q10, q11, [sp, #416]
	ldp	q12, q13, [sp, #448]
	ldp	q14, q15, [sp, #480]
	ldp	q16, q17, [sp, #512]
	ldp	q18, q19, [sp, #544]
	ldp	q20, q21, [sp, #576]
	ldp	q22, q23, [sp, #608]
	ldp	q24, q25, [sp, #640]
	ldp	q26, q27, [sp, #672]
	ldp	q28, q29, [sp, #704]
	ldp	q30, q31, [sp, #736]
	ldp	x0, x1, [sp, #0]
	ldp	x2, x3, [sp, #16]
	ldp	x4, x5, [sp, #32]
	ldp	x6, x7, [sp, #48]
	ldp	x8, x9, [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x19, [sp, #144]
	ldp	x20, x21, [sp, #160]
	ldp	x22, x23, [sp, #176]
	ldp	x24, x25, [sp, #192]
	ldp	x26, x27, [sp, #208]
	ldp	x28, x29, [sp, #224]
	ldr	x30, [sp, #240]
	add	sp, sp, #800
	eret
	.size	xnu_fault_tramp, . - xnu_fault_tramp
