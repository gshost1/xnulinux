// SPDX-License-Identifier: APSL-2.0
/*
 * Copyright (c) 2026 xnulinux contributors. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * This file contains Original Code and/or Modifications of Original Code as defined in and that are subject to the Apple
 * Public Source License Version 2.0 (the 'License'). You may not use this file except in compliance with the License.
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * The LINUX64 machine layer: XNU hosted inside a Linux kernel by xnu.ko. Everything the rest of XNU uses of it.
 * Copied with linux64_abi.h into EXTERNAL_HEADERS/linux64 (on every component's include path) when the patches are
 * applied (scripts/vm-xnu-rebuild.sh).
 *
 * Hosted or not is decided at run time: linux64_hook is 0 when this kernel boots on hardware, and every hosted path
 * is behind `if (linux64_hook)`.
 */
#ifndef _LINUX64_LINUX64_H_
#define _LINUX64_LINUX64_H_

#include <linux64/linux64_abi.h>

/* Bring-up diagnostics (thread dumps from the idle loop, per-exception traces): off unless built with -DLINUX64_DEBUG=1 */
#ifndef LINUX64_DEBUG
#define LINUX64_DEBUG 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * xnu.ko's entry point (0: not hosted) and the slide it applied. TPIDR_EL1 is Linux's per-CPU base: XNU's kernel keeps
 * its thread pointer in TPIDRRO_EL0 (per CPU), and user space gets its TSD there on each return to EL0.
 */
extern unsigned long linux64_hook;
extern unsigned long linux64_slide;
/* This CPU's slot in the per-CPU arrays (LINUX64_CPU_SLOT of its MPIDR): valid before XNU knows its cpu number */
static inline unsigned
linux64_cpu_slot(void)
{
	unsigned long mpidr;

	__asm__ volatile ("mrs %0, MPIDR_EL1" : "=r"(mpidr));
	return LINUX64_CPU_SLOT(mpidr);
}

/*
 * Each CPU's state, one 128-byte line per CPU (no false sharing between CPUs: Apple's line, and the pair of 64-byte
 * lines a prefetcher fetches together), linux64_pcpu[linux64_pcpu_index[slot]]: line 0 is the boot CPU's (and that of
 * every slot no CPU has), cpu_start gives each other CPU its line before the host starts it (linux64_pcpu_assign).
 * A small table and few lines: the kernel's __DATA has no room for 256 lines.
 *   vint        XNU's interrupt-enable state: hardware interrupts stay masked while XNU runs; this flag is the virtual one
 *   ipi_pending another CPU's inter-processor interrupt (set by the sender, taken where XNU's interrupts are enabled
 *               again); the host's IDLE call watches this word (its a2)
 *   pv_kicked   a ticket-lock waiter's kick (arm64_hypercall.c), the IDLE word of that wait
 *   polling     cpu_idle polls ipi_pending: a sender needs no host IPI (machine_routines_common.c, arm64/cpu.c)
 *   last_kyield, relax_start, last_relax: counter ticks of the kernel's interrupt windows and spin-waits
 *   dbg         this CPU's share of the hot linux64_dbg counters (below 8), added to them every 64
 */
struct linux64_pcpu {
	unsigned int vint;
	unsigned int ipi_pending;
	unsigned int pv_kicked;
	unsigned int polling;
	unsigned long long last_kyield;
	unsigned long long relax_start;
	unsigned long long last_relax;
	unsigned long dbg[8];
} __attribute__((aligned(128)));
#define LINUX64_PCPU_LINES 32
extern unsigned char linux64_pcpu_index[LINUX64_CPU_SLOTS];
extern struct linux64_pcpu linux64_pcpu[LINUX64_PCPU_LINES];
static inline struct linux64_pcpu *
linux64_pcpu_of_slot(unsigned slot)
{
	return &linux64_pcpu[linux64_pcpu_index[slot & (LINUX64_CPU_SLOTS - 1)]];
}
static inline struct linux64_pcpu *
linux64_pcpu_self(void)
{
	return linux64_pcpu_of_slot(linux64_cpu_slot());
}
#define LINUX64_VINT (linux64_pcpu_self()->vint)
#define LINUX64_IPI_PENDING(slot) (linux64_pcpu_of_slot(slot)->ipi_pending)
/* osfmk/arm/arm_init.c: a line for the CPU of this MPIDR (cpu_start, before the host starts it) */
void linux64_pcpu_assign(unsigned long mpidr);
/* enum linux64_counter */
extern unsigned long linux64_dbg[LINUX64_NCOUNTERS];

/*
 * The timebase's scale (droidvm-timebase-hz) for division-free conversion: osfmk/arm64/machine_routines.c, and the EL0
 * time traps cswitch.s answers (which read it at these offsets). scaled: 0 none, 1 by reciprocal, 2 by division.
 */
struct linux64_tb_scale {
	unsigned long long num;		/* 0 */
	unsigned long long den;		/* 8 */
	unsigned long long magic;	/* 16 */
	unsigned long long shift;	/* 24 */
	unsigned int scaled;		/* 32 */
};
extern struct linux64_tb_scale linux64_tb;

/* A call into the host (enum linux64_call). The host's answer: 0 to go on. */
static inline unsigned long
linux64_call2(enum linux64_call call, unsigned long a1, unsigned long a2)
{
	register unsigned long x0 __asm__("x0") = (unsigned long)call;
	register unsigned long x1 __asm__("x1") = a1;
	register unsigned long x2 __asm__("x2") = a2;
	if (linux64_hook) {
		/* a plain blr: the hook is host code, not an XNU function pointer (no pointer authentication) */
		__asm__ volatile ("blr %3"
		    : "+r"(x0), "+r"(x1), "+r"(x2)
		    : "r"(linux64_hook)
		    : "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17",
		    "x30", "cc", "memory");
	}
	return x0;
}

static inline unsigned long
linux64_call(enum linux64_call call, const void *a1)
{
	return linux64_call2(call, (unsigned long)a1, 0);
}

/* Bytes from the host's cryptographic generator (Linux's get_random_bytes) */
static inline void
linux64_random(void *buf, unsigned long len)
{
	linux64_call2(LINUX64_CALL_RANDOM, (unsigned long)buf, len);
}

/* A uniform value in [0, bound) (0 when bound is 0), by rejection: no modulo bias */
static inline __UINT64_TYPE__
linux64_random_uniform(__UINT64_TYPE__ bound)
{
	__UINT64_TYPE__ v, limit;

	if (bound == 0) {
		return 0;
	}
	limit = -bound % bound;	/* 2^64 mod bound: values below it would bias the result */
	do {
		linux64_random(&v, sizeof(v));
	} while (v < limit);
	return v % bound;
}

/*
 * A monitor counter. The hot ones (below 8: idle, switches, rtclock) count per CPU and reach linux64_dbg every 64, so
 * CPUs do not take turns at one cache line on every context switch. True when this call added to linux64_dbg.
 */
static inline int
linux64_count(enum linux64_counter c)
{
	if ((unsigned)c < 8) {
		struct linux64_pcpu *pc = linux64_pcpu_self();
		if (__builtin_expect(++pc->dbg[c] >= 64, 0)) {
			__atomic_fetch_add(&linux64_dbg[c], pc->dbg[c], __ATOMIC_RELAXED);
			pc->dbg[c] = 0;
			return 1;
		}
		return 0;
	}
	linux64_dbg[c]++;
	return 1;
}

/* osfmk/arm/arm_init.c */
void linux64_assert_report(const char *file, int line, const char *expr);
/* osfmk/arm/machine_routines_common.c: IPIs. With XNU's interrupts off: take this CPU's pending IPI, if any */
void linux64_ipi_take(void);
/* signal a CPU (kexts/XnuCPU's IOCPU::signalCPU): its logical number */
void linux64_cpu_signal(unsigned int cpu);
/* bsd/kern/kern_trustcache.c, libkern/amfi/amfi.c */
void linux64_trust_cache_late_init(void);
int linux64_amfi_off(void);
/* osfmk/arm64/cswitch.s, sleh.c: user space while hosted */
void linux64_el0_run(void *saved_state, unsigned long arg) __attribute__((noreturn));
/* osfmk/arm/machine_routines_common.c: in a spin-wait, let the host take this CPU now and then (see arm/locks.h) */
void linux64_spin_relax(void);
/* osfmk/kern/sched_prim.c: every thread's state and saved frames to the host log (on the host's request) */
void linux64_dump_threads(void);

#ifdef __cplusplus
}
#endif

#endif /* _LINUX64_LINUX64_H_ */
