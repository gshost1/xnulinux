// SPDX-License-Identifier: APSL-2.0
/* World switch between Linux and the hosted XNU (see world.S for the layout). */
#pragma once
#include "../fki-ko/fki.h"
struct xnu_world {
	unsigned long long linux_regs[14];	/* 0: x19..x28, x29, x30, sp, irq cookie */
	unsigned long long xnu_regs[13];	/* 112: x19..x28, x29, x30, sp */
	unsigned long long linux_tpidr0;		/* 216: unused (fki_enter_mm keeps Linux's TPIDR_EL0) */
	unsigned long long linux_tpidrro0;	/* 224: unused (and TPIDRRO_EL0) */
	unsigned long long xnu_stack_top;	/* 232 */
	struct fki_pac_keys linux_keys;		/* 240 */
	struct fki_pac_keys xnu_keys;		/* 328 */
	unsigned long long linux_ttbr0, linux_tcr;	/* 416, 424: unused (fki keeps Linux's address space) */
	unsigned long long xnu_ttbr0, xnu_tcr;		/* 432, 440: XNU's first TTBR0 and its TCR, for fki_enter_mm */
	unsigned long long xnu_tpidr, xnu_tpidrro;	/* 448, 456: unused (fki keeps XNU's across calls that let Linux run) */
	unsigned long long linux_scs;		/* 464: x18, the shadow call stack of the Linux task that entered */
	unsigned long long xnu_d[8];		/* 472: XNU callee-saved d8..d15 (AAPCS-64) across upcalls */
	unsigned long long xnu_fpcr;		/* 536: XNU FPCR across upcalls */
};
_Static_assert(__builtin_offsetof(struct xnu_world, linux_scs) == 464, "xnu_world layout");
_Static_assert(__builtin_offsetof(struct xnu_world, xnu_d) == 472, "xnu_world layout");
_Static_assert(__builtin_offsetof(struct xnu_world, xnu_fpcr) == 536, "xnu_world layout");
_Static_assert(__builtin_offsetof(struct xnu_world, xnu_tpidrro) == 456, "xnu_world layout");
_Static_assert(__builtin_offsetof(struct xnu_world, linux_ttbr0) == 416, "xnu_world layout");
_Static_assert(sizeof(struct fki_pac_keys) == 88, "fki_pac_keys layout");
_Static_assert(__builtin_offsetof(struct xnu_world, xnu_keys) == 328, "xnu_world layout");
_Static_assert(__builtin_offsetof(struct xnu_world, xnu_ttbr0) == 432, "xnu_world layout");
_Static_assert(__builtin_offsetof(struct xnu_world, xnu_tcr) == 440, "xnu_world layout");
_Static_assert(sizeof(struct xnu_world) == 544, "xnu_world layout");
extern struct xnu_world *xnu_worlds[];	/* each CPU's, by slot (LINUX64_CPU_SLOT), whichever guest it belongs to */
/* Run fn(arg) in the XNU world. 0: fn returned; 1: the hook ended the run. */
long xnu_world_call(unsigned long fn, unsigned long arg);
/* The address XNU calls (linux64_hook); it calls xnu_hook(stage) in the Linux world. */
void xnu_world_upcall(void);
/* Where an XNU fault that cannot be handled returns to (still with XNU's keys): back to Linux's, then xnu_world_abort. */
void xnu_world_fault_abort(void);
/* Linux-world handler: return 0 to resume XNU, nonzero to end the run. */
long xnu_hook(unsigned long stage, unsigned long a1, unsigned long a2);
