/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
/*
 * Foreign kernel interface (FKI), version 1 (every version 0 call kept as it was).
 *
 * The only interface between Linux and a foreign-kernel module. It is
 * semantic and shaped like a hypervisor interface: plain integer types and
 * opaque handles, no Linux structure crosses it. A foreign module includes
 * this header only.
 */
#ifndef _LINUX_FKI_H
#define _LINUX_FKI_H

#define FKI_VERSION 1

#ifndef __KERNEL__
#include <stdint.h>
typedef uint64_t fki_u64;
#else
#include <linux/types.h>
typedef u64 fki_u64;
#endif

/* Physically contiguous memory, mapped contiguously and executable. */
struct fki_region {
	void *va;
	fki_u64 pa;
	fki_u64 size;
	void *handle;
};

/* Pointer authentication keys (each 128 bits: lo, hi). */
struct fki_pac_keys {
	fki_u64 ia[2], ib[2], da[2], db[2], ga[2];
	fki_u64 sctlr_en;	/* SCTLR_EL1 EnIA|EnIB|EnDA|EnDB bits */
};

#ifdef __cplusplus
extern "C" {
#endif
int fki_version(void);
void fki_log(const char *s, unsigned long len);
void fki_log_alert(const char *s, unsigned long len);	/* KERN_ALERT: on the console even when it is quiet */
int fki_region_alloc(unsigned long size, struct fki_region *r);
void fki_region_free(struct fki_region *r);
/* A named blob from the firmware search path. */
int fki_blob_get(const char *name, const void **data, unsigned long *size, void **handle);
void fki_blob_put(void *handle);
/* Enter and leave a foreign context on this CPU: no preemption, no interrupts, foreign PAC keys; the FP/SIMD registers are
 * the foreign kernel's to use (the current task's state saved first). */
unsigned long fki_enter(const struct fki_pac_keys *foreign, struct fki_pac_keys *saved);
void fki_leave(unsigned long cookie, const struct fki_pac_keys *saved);
/* Swap PAC keys only (caller already in a foreign context): save current into *save, load *load. */
void fki_pac_swap(const struct fki_pac_keys *load, struct fki_pac_keys *save);
fki_u64 fki_counter_frequency(void);
fki_u64 fki_counter(void);
void fki_random(void *buf, unsigned long len);
/*
 * Physically contiguous memory with real physical addresses (for a foreign kernel whose own page tables
 * the hardware walks). va is a kernel linear-map address (RW, not executable). size rounds up to a power of two.
 */
int fki_contig_alloc(unsigned long size, struct fki_region *r);
void fki_contig_free(struct fki_region *r);
/*
 * The host's translation granule: 12 (4 KB) or 14 (16 KB). A table holds 1 << (shift - 3) descriptors; an L2 entry maps
 * 1 << (2 * shift - 3) bytes (2 MiB, 32 MiB), an L1 entry 1 << (3 * shift - 6) (1 GiB, 64 GiB).
 */
unsigned fki_page_shift(void);
/*
 * The top foreign window: the L1 table (Linux's pud table) that the kernel half's top-level entry for va points to, with
 * 4 KB and 16 KB granules alike. A foreign kernel may own L1 entries Linux leaves empty in that table; fki_window_set
 * installs one (then a full TLB flush).
 */
int fki_window_l1(unsigned long va, unsigned long long **l1_va, fki_u64 *l1_pa);
void fki_window_set(unsigned long long *slot, fki_u64 desc);
/*
 * An L1 entry Linux shares with the foreign window (16 KB granule: one L1 entry spans 64 GiB, and Linux's fixmap and PCI I/O
 * sit in the same one as the foreign kernel's top): the L2 table it points to (-ENOENT when the entry is not a table).
 */
int fki_window_l2(unsigned long va, unsigned long long **l2_va, fki_u64 *l2_pa);
/*
 * One L2 entry of that table for va (the foreign kernel's bootstrap blocks), under Linux's kernel page-table lock, then a full
 * TLB flush. Refuses (-EBUSY) an entry Linux has in use; desc 0 clears an entry this call installed.
 */
int fki_window_l2_set(unsigned long va, fki_u64 desc);
/*
 * Hand the shared L1 entry for va to the foreign kernel's own L2 table (new_l2_va, mapped by the caller; new_desc the L1
 * descriptor pointing at it): its entries [lo, hi) are the foreign kernel's, every other entry Linux has in its table is
 * copied into the new table first (which must have nothing there). Refuses when Linux has anything in [lo, hi) but the
 * bootstrap entries fki_window_l2_set installed; those are cleared from Linux's table once the new one is live. *old_desc:
 * Linux's descriptor, for fki_window_l2_restore. Callable with interrupts off.
 */
int fki_window_l2_adopt(unsigned long va, unsigned long long *new_l2_va, fki_u64 new_desc, unsigned lo, unsigned hi,
			fki_u64 *old_desc);
/* Back to Linux's own L2 table: its entries outside [lo, hi) as they are now in cur_l2_va, [lo, hi) empty, then old_desc. */
void fki_window_l2_restore(unsigned long va, unsigned long long *cur_l2_va, fki_u64 old_desc, unsigned lo, unsigned hi);
/*
 * Called from inside fki_enter() (preemption and IRQs off): let Linux run (IRQs and preemption on)
 * until the counter reaches deadline or max_ns pass, or fki_kick_cpu() is called for this CPU, then return with them off
 * again. Not at all if *wake (when given) is already nonzero: the kicker sets it before kicking. wake is read while
 * Linux's address space is live: an address Linux maps (a foreign kernel's word through its Linux alias).
 */
void fki_idle_until(fki_u64 counter_deadline, fki_u64 max_ns, const volatile unsigned int *wake);
/*
 * Called from inside fki_enter() while the hosted kernel is busy: let Linux take its pending interrupts and
 * reschedule if it must (RCU, other tasks on this CPU), then return with IRQs and preemption off again.
 */
void fki_yield(void);
/* The host's wall clock (UTC): seconds and nanoseconds since 1970. */
void fki_realtime(fki_u64 *sec, fki_u64 *nsec);
/*
 * A virtual Ethernet link to Linux (the netdev "xnu0"): send one frame (header included, no FCS); receive the next
 * frame Linux sent (its length, 0 when none waits); how many frames wait.
 */
long fki_net_send(const void *frame, unsigned long len);
long fki_net_recv(void *buf, unsigned long len);
long fki_net_pending(void);
/* Keep the calling task on one CPU (the hosted kernel sees that CPU's MPIDR). */
int fki_bind_cpu(int cpu);
/* CPUs: how many are online, and a CPU's MPIDR (from ordinary Linux context) */
int fki_num_cpus(void);
int fki_cpu_mpidr(int cpu, fki_u64 *mpidr);
/* Make the foreign kernel on a CPU look at its pending work: wake its idle call, or interrupt it (interrupts may be off) */
void fki_kick_cpu(int cpu);
/*
 * A helper kernel thread bound to one CPU: fn(arg) runs until it returns or fki_thread_stop() is called;
 * fn should loop on !fki_thread_should_stop() and sleep with fki_msleep().
 */
void *fki_thread_start(int (*fn)(void *), void *arg, int cpu, const char *name);
void fki_thread_stop(void *thread);
int fki_thread_should_stop(void);
/* In a helper thread: sleep until *flag is non-zero or the thread is told to stop; fki_thread_wake() wakes it (callable
 * with interrupts off, after setting the flag) */
void fki_thread_wait(const volatile unsigned long *flag);
void fki_thread_wake(void *thread);
void fki_msleep(unsigned int ms);
/*
 * Foreign kernel faults: a kernel-mode data or instruction abort whose address or faulting pc is in [lo, hi) is offered
 * to handler (far, esr, the faulting registers) before Linux's own handling; handler returns 0 when the exception should
 * return (the faulting instruction runs again, or wherever fki_regs_set_pc points it), nonzero to let Linux handle it as
 * usual. One registration at a time.
 */
unsigned long fki_regs_pc(void *regs);
unsigned long fki_regs_lr(void *regs);
unsigned long fki_regs_pstate(void *regs);	/* the interrupted PSTATE (SPSR) */
void fki_regs_set_pc(void *regs, unsigned long pc);
void fki_regs_set_reg(void *regs, unsigned int n, unsigned long v);	/* x0..x30 */
unsigned long fki_regs_reg(void *regs, unsigned int n);			/* x0..x30 */
int fki_fault_register(unsigned long lo, unsigned long hi, int (*handler)(unsigned long far, unsigned long esr, void *regs));
void fki_fault_unregister(void);
/* Read 8 bytes of RAM by physical address (debugging: walking a foreign page table). */
fki_u64 fki_phys_peek(fki_u64 pa);
/*
 * A host file as backing store (a foreign block device's image). fki_file_open is called from ordinary Linux context
 * and returns an opaque handle (NULL on failure) and the file's size. fki_file_rw is called from inside fki_enter():
 * it lets Linux run (IRQs and preemption on) for the I/O, copies through a Linux bounce buffer to or from buf
 * (with IRQs off again), and returns the bytes moved or a negative errno.
 */
void *fki_file_open(const char *path, int writable, fki_u64 *size);
long fki_file_rw(void *file, int write, void *buf, unsigned long len, fki_u64 off);
long fki_file_rw_direct(void *file, int write, void *kva, unsigned long len, fki_u64 off);
void fki_file_close(void *file);
/* Write to a host file from ordinary Linux context (a host thread, not inside fki_enter()); bytes written or -errno. */
long fki_file_pwrite(void *file, const void *buf, unsigned long len, fki_u64 off);
/*
 * A host call (from inside fki_enter()): len bytes at req to the Linux service reading /dev/xnu-hostcall, its reply into
 * reply (at most max bytes). Returns the reply's length or a negative errno (-ETIMEDOUT: no service answered).
 */
long fki_hostcall(const void *req, unsigned long len, void *reply, unsigned long max);

/* ---- version 1: several foreign kernels at once, each in an address space of its own ---- */
/*
 * The foreign kernel's address space on this CPU. fki_enter_mm is fki_enter plus the user half (ttbr0, tcr) and, on a CPU
 * attached to a private kernel-half root (fki_cpu_attach), TTBR1. Linux itself only ever runs on its own swapper_pg_dir:
 * every call that lets Linux run (idle, yield, file I/O, a network send, a host call) switches to Linux's TTBR0, TCR and
 * TTBR1 and back, and never dereferences a foreign address while Linux's are live. TPIDR_EL0 and TPIDRRO_EL0 are the
 * foreign kernel's while it runs (Linux's context switch rewrites both). The caller is bound to one CPU.
 */
unsigned long fki_enter_mm(const struct fki_pac_keys *foreign, struct fki_pac_keys *saved, fki_u64 ttbr0, fki_u64 tcr);
void fki_leave_mm(unsigned long cookie, const struct fki_pac_keys *saved);
/* An empty top-level table (the foreign kernel's first TTBR0 until it switches to its own user tables) */
fki_u64 fki_empty_ttbr0(void);
/*
 * A private kernel-half root: a copy of Linux's top levels in which [lo, hi) is the foreign kernel's (Linux has nothing
 * there; -EBUSY otherwise) and everything else is Linux's own lower tables, shared. swapper_pg_dir is never written. lo is
 * aligned to an L1 entry, hi to an L2 entry; an L1 entry [lo, hi) only partly covers (16 KB granule: the 64 GiB entry the
 * foreign kernel's top 48 GiB share with Linux's PCI I/O and fixmap) points at an L2 table of the root's own, Linux's
 * entries copied in above hi. Entries Linux adds later are copied in when a CPU on the root faults on them.
 */
struct fki_ttbr1;
struct fki_ttbr1 *fki_ttbr1_create(unsigned long lo, unsigned long hi, int *err);
void fki_ttbr1_destroy(struct fki_ttbr1 *t);	/* no CPU attached any more; before the memory its tables point into goes */
/* The root's L1 table for va (its own copy), and the partial entry's bootstrap L2 table (-ENOENT/-EBUSY otherwise) */
int fki_ttbr1_window_l1(struct fki_ttbr1 *t, unsigned long va, unsigned long long **l1_va, fki_u64 *l1_pa);
int fki_ttbr1_window_l2(struct fki_ttbr1 *t, unsigned long va, unsigned long long **l2_va, fki_u64 *l2_pa);
/* One entry of the foreign kernel's in the root's own tables (an L1 entry in [lo, hi), or a bootstrap L2 entry) */
int fki_ttbr1_set(struct fki_ttbr1 *t, unsigned long long *slot, fki_u64 desc);
/*
 * Hand the partial L1 entry to the foreign kernel's own L2 table (l2_va mapped by the caller, l2_pa its address): it has
 * nothing above hi, Linux's entries there are copied in, then the entry is switched break-before-make. Only the foreign
 * kernel's boot CPU may run on the root meanwhile (interrupts off is fine).
 */
int fki_ttbr1_adopt_l2(struct fki_ttbr1 *t, unsigned long long *l2_va, fki_u64 l2_pa);
/* Compare the root with Linux's tables: the number of differences (0: clean); missing entries are copied in */
long fki_ttbr1_audit(struct fki_ttbr1 *t);
unsigned long fki_ttbr1_syncs(struct fki_ttbr1 *t);	/* entries copied in on a fault so far */
/*
 * Per-CPU dispatch: the root a CPU runs the foreign kernel on and the handler for kernel faults whose address or pc is in
 * [fault_lo, fault_hi) (as fki_fault_register, with ctx first). Not together with fki_fault_register (-EBUSY). Each
 * attached CPU gets its own bounce buffers; it cannot go offline while attached.
 */
struct fki_cpu_cfg {
	struct fki_ttbr1 *ttbr1;
	int (*fault)(void *ctx, unsigned long far, unsigned long esr, void *regs);
	void *fault_ctx;
	unsigned long fault_lo, fault_hi;
};
int fki_cpu_attach(int cpu, const struct fki_cpu_cfg *cfg);
void fki_cpu_detach(int cpu);
/* A host file's size without I/O (callable from inside fki_enter()) */
fki_u64 fki_file_size(void *file);
/*
 * Links: fki makes the netdevs xnu0 .. xnu<nets-1> at its load (they stay across runs, so host addresses, DHCP and firewall
 * rules stay too); fki_net_create claims one by name (making it when it does not exist) for one foreign kernel, whose
 * CPU wake_cpu is woken for a frame Linux sends; fki_net_release gives it back (fki_net_destroy also removes one made by
 * fki_net_create). Frames Linux sends on an unclaimed link are dropped (tx_carrier_errors). fki_net_send/recv/pending
 * (version 0) are xnu0's.
 */
struct fki_net;
struct fki_net *fki_net_create(const char *name, int wake_cpu);
void fki_net_release(struct fki_net *net);
void fki_net_destroy(struct fki_net *net);
long fki_net_send2(struct fki_net *net, const void *frame, unsigned long len);
long fki_net_recv2(struct fki_net *net, void *buf, unsigned long len);
long fki_net_pending2(struct fki_net *net);
/*
 * Host call channels: index 0 is /dev/xnu-hostcall (version 0's fki_hostcall), index n /dev/xnu-hostcall<n>. Create before
 * the foreign kernel runs (its buffer is allocated then); the device stays until fki unloads.
 */
void *fki_hostcall_create(int index, int *err);
void fki_hostcall_release(void *hc);
long fki_hostcall2(void *hc, const void *req, unsigned long len, void *reply, unsigned long max);
/*
 * A control directory /sys/kernel/<dir>/ with ctl (write-only: write_cb gets what was written) and status (read-only:
 * read_cb fills at most len bytes, returns how many). One at a time; unregister waits for callbacks in progress.
 */
int fki_ctl_register(const char *dir, long (*write_cb)(const char *buf, unsigned long len),
		     long (*read_cb)(char *buf, unsigned long len));
void fki_ctl_unregister(void);
#ifdef __cplusplus
}
#endif
#endif
