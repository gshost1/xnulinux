// SPDX-License-Identifier: GPL-2.0-only
/*
 * Foreign kernel interface, version 1 (every version 0 call kept): see fki.h.
 *
 * Exports are plain EXPORT_SYMBOL on purpose: this file defines a narrow,
 * stable, semantic interface of its own, and nothing behind it is
 * re-exported.
 *
 * Invariant (T1): Linux only ever runs on its own kernel page tables (swapper_pg_dir). A foreign kernel runs on a private
 * root of its own (struct fki_ttbr1: Linux's top levels copied, its lower tables shared); every way back into Linux that
 * may enable interrupts or schedule (fki_linux_runs) switches TTBR0, TCR and TTBR1 to Linux's first and never touches a
 * foreign address until it has switched back.
 */
#include "fki.h"
#include "fki_internal.h"
#include <linux/firmware.h>
#include <linux/gfp.h>
#include <linux/irqflags.h>
#include <linux/mm.h>
#include <linux/io.h>
#include <linux/efi.h>
#include <linux/ioport.h>
#include <linux/module.h>
#include <linux/preempt.h>
#include <linux/printk.h>
#include <linux/random.h>
#include <linux/slab.h>
#include <linux/timekeeping.h>
#include <linux/sched.h>
#include <linux/cpumask.h>
#include <linux/vmalloc.h>
#include <asm/arch_timer.h>
#include <asm/neon.h>
#include <asm/sysreg.h>
#include <asm/tlbflush.h>
#include <asm/barrier.h>
#include <asm/cpufeature.h>
#include <asm/memory.h>
#include <linux/kprobes.h>
#include <linux/hrtimer.h>
#include <linux/kthread.h>
#include <linux/smp.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <asm/stacktrace.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/pgtable.h>
#include <linux/spinlock.h>
#include <linux/bitmap.h>
#include <linux/sizes.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/rcupdate.h>
#include <linux/cpuhotplug.h>
#include <linux/memory.h>
#include <linux/suspend.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/rtnetlink.h>

/* The private roots copy Linux's 4-level, 48-bit kernel half; no LPA/LPA2 descriptor formats */
#if CONFIG_ARM64_VA_BITS != 48 || CONFIG_PGTABLE_LEVELS != 4
#error "fki: the foreign window needs 48-bit VA with 4-level kernel page tables"
#endif
#if defined(CONFIG_ARM64_PA_BITS_52) || defined(CONFIG_ARM64_LPA2)
#error "fki: 52-bit physical addresses (LPA, LPA2) are not supported"
#endif

int fki_version(void)
{
	return FKI_VERSION;
}
EXPORT_SYMBOL(fki_version);

void fki_log(const char *s, unsigned long len)
{
	pr_info("fki: %.*s\n", (int)len, s);
}
EXPORT_SYMBOL(fki_log);
/* what an operator must see even with the console quiet (an XNU panic, a CPU gone silent): KERN_ALERT */
void fki_log_alert(const char *s, unsigned long len)
{
	pr_alert("fki: %.*s\n", (int)len, s);
}
EXPORT_SYMBOL(fki_log_alert);


/*
 * Out-of-tree form only: stock kernels do not export set_memory_x(). The in-tree FKI
 * patch calls it directly; here it is found once through kallsyms (via a kprobe), at load, and never changes after.
 */
typedef int (*set_memory_fn)(unsigned long addr, int numpages);
typedef struct page *(*alloc_contig_fn)(unsigned long nr_pages, gfp_t gfp, int nid, nodemask_t *nodemask);
typedef void (*free_contig_fn)(unsigned long pfn, unsigned long nr_pages);
static alloc_contig_fn p_alloc_contig;
static free_contig_fn p_free_contig;
struct cma;
static struct cma **p_cma_area;	/* the default CMA area (cma= on the command line), if any */
static struct page *(*p_cma_alloc)(struct cma *, unsigned long, unsigned int, bool);	/* not exported on every kernel */
static bool (*p_cma_release)(struct cma *, const struct page *, unsigned long);
static set_memory_fn p_set_memory_x, p_set_memory_nx;
static unsigned long p_stext, p_end, p_text;
static u64 *p_kimage_voffset;
static int (*p_efi_mem_type)(unsigned long);	/* the firmware's memory map: is the carve-out RAM at all */
static struct mm_struct *p_init_mm;	/* the kernel's page tables (init_mm is not exported): the foreign window */
static u64 *p_swapper;			/* swapper_pg_dir (init_mm.pgd) */
static phys_addr_t fki_swapper_pa;	/* its physical address: TTBR1's base whenever Linux runs */
static int fki_resolve_err = -EAGAIN;

static int fki_resolve(void)
{
	unsigned long (*lookup)(const char *);
	struct kprobe kp = { .symbol_name = "kallsyms_lookup_name" };

	if (fki_resolve_err != -EAGAIN)
		return fki_resolve_err;
	if (register_kprobe(&kp))
		return fki_resolve_err = -ENOENT;
	lookup = (void *)kp.addr;
	unregister_kprobe(&kp);
	p_set_memory_x = (set_memory_fn)lookup("set_memory_x");
	p_alloc_contig = (alloc_contig_fn)lookup("alloc_contig_pages_noprof");
	if (!p_alloc_contig)
		p_alloc_contig = (alloc_contig_fn)lookup("alloc_contig_pages");
	p_free_contig = (free_contig_fn)lookup("free_contig_range");
	p_cma_area = (struct cma **)lookup("dma_contiguous_default_area");
	p_cma_alloc = (void *)lookup("cma_alloc");
	p_cma_release = (void *)lookup("cma_release");
	p_set_memory_nx = (set_memory_fn)lookup("set_memory_nx");
	p_stext = lookup("_stext");
	p_end = lookup("_end");
	p_text = lookup("_text");
	p_kimage_voffset = (u64 *)lookup("kimage_voffset");
	p_efi_mem_type = (void *)lookup("efi_mem_type");
	p_init_mm = (struct mm_struct *)lookup("init_mm");
	p_swapper = (u64 *)lookup("swapper_pg_dir");
	if (!p_set_memory_x || !p_set_memory_nx || !p_init_mm || !p_init_mm->pgd)
		return fki_resolve_err = -ENOENT;
	if (p_swapper && (void *)p_swapper != (void *)p_init_mm->pgd) {
		pr_err("fki: init_mm.pgd %px is not swapper_pg_dir %px\n", p_init_mm->pgd, p_swapper);
		return fki_resolve_err = -EINVAL;
	}
	p_swapper = (u64 *)p_init_mm->pgd;
	fki_swapper_pa = __pa_symbol(p_swapper);
	return fki_resolve_err = 0;
}

int fki_region_alloc(unsigned long size, struct fki_region *r)
{
	unsigned long n = PAGE_ALIGN(size) >> PAGE_SHIFT;
	void *va;
	int err;

	/* vmap() memory is never executable on current kernels; vmalloc + set_memory_x is (out-of-tree form). */
	err = fki_resolve();
	if (err)
		return err;
	va = vzalloc(n << PAGE_SHIFT);
	if (!va)
		return -ENOMEM;
	err = p_set_memory_x((unsigned long)va, n);
	if (err) {
		vfree(va);
		return err;
	}
	r->va = va;
	r->pa = page_to_phys(vmalloc_to_page(va));
	r->size = n << PAGE_SHIFT;
	r->handle = NULL;
	return 0;
}
EXPORT_SYMBOL(fki_region_alloc);

void fki_region_free(struct fki_region *r)
{
	p_set_memory_nx((unsigned long)r->va, r->size >> PAGE_SHIFT);
	vfree(r->va);
}
EXPORT_SYMBOL(fki_region_free);

int fki_blob_get(const char *name, const void **data, unsigned long *size, void **handle)
{
	const struct firmware *fw;
	int err = request_firmware(&fw, name, NULL);

	if (err)
		return err;
	*data = fw->data;
	*size = fw->size;
	*handle = (void *)fw;
	return 0;
}
EXPORT_SYMBOL(fki_blob_get);

void fki_blob_put(void *handle)
{
	release_firmware(handle);
}
EXPORT_SYMBOL(fki_blob_put);

#define FKI_SCTLR_EN (SCTLR_ELx_ENIA | SCTLR_ELx_ENIB | SCTLR_ELx_ENDA | SCTLR_ELx_ENDB)


static __always_inline void keys_read(struct fki_pac_keys *k)
{
	k->ia[0] = read_sysreg_s(SYS_APIAKEYLO_EL1); k->ia[1] = read_sysreg_s(SYS_APIAKEYHI_EL1);
	k->ib[0] = read_sysreg_s(SYS_APIBKEYLO_EL1); k->ib[1] = read_sysreg_s(SYS_APIBKEYHI_EL1);
	k->da[0] = read_sysreg_s(SYS_APDAKEYLO_EL1); k->da[1] = read_sysreg_s(SYS_APDAKEYHI_EL1);
	k->db[0] = read_sysreg_s(SYS_APDBKEYLO_EL1); k->db[1] = read_sysreg_s(SYS_APDBKEYHI_EL1);
	k->ga[0] = read_sysreg_s(SYS_APGAKEYLO_EL1); k->ga[1] = read_sysreg_s(SYS_APGAKEYHI_EL1);
	k->sctlr_en = read_sysreg(sctlr_el1) & FKI_SCTLR_EN;
}

static __always_inline void keys_write(const struct fki_pac_keys *k)
{
	write_sysreg_s(k->ia[0], SYS_APIAKEYLO_EL1); write_sysreg_s(k->ia[1], SYS_APIAKEYHI_EL1);
	write_sysreg_s(k->ib[0], SYS_APIBKEYLO_EL1); write_sysreg_s(k->ib[1], SYS_APIBKEYHI_EL1);
	write_sysreg_s(k->da[0], SYS_APDAKEYLO_EL1); write_sysreg_s(k->da[1], SYS_APDAKEYHI_EL1);
	write_sysreg_s(k->db[0], SYS_APDBKEYLO_EL1); write_sysreg_s(k->db[1], SYS_APDBKEYHI_EL1);
	write_sysreg_s(k->ga[0], SYS_APGAKEYLO_EL1); write_sysreg_s(k->ga[1], SYS_APGAKEYHI_EL1);
	sysreg_clear_set(sctlr_el1, FKI_SCTLR_EN, k->sctlr_en & FKI_SCTLR_EN);
	isb();
}

/*
 * The foreign kernel uses the FP/SIMD registers (its user threads' state, its own SIMD code) without Linux knowing.
 * Linux skips reloading a task's FP/SIMD state when it believes this CPU's registers still hold it, so a task that ran
 * here before would come back with the foreign kernel's register contents (random user-space crashes on the hosted
 * kernel's CPU). Before the foreign kernel runs (on entry, and each time it resumes after Linux ran), save the current
 * task's state and make Linux forget whose state the registers hold: an empty kernel-mode NEON section does both.
 * The foreign kernel's world switch (world.S) preserves XNU kernel's FP/SIMD registers across calls into the host
 * (callee-saved d8..d15 + FPCR across upcalls; q0..q31 + FPSR + FPCR across deferred kernel faults), while XNU saves
 * its user threads' registers on each EL0 exception.
 */
static void fki_fpsimd_forget(void)
{
	struct user_fpsimd_state scratch;	/* where Linux would save a kernel-mode state over a context switch: none here */

	kernel_neon_begin(&scratch);
	kernel_neon_end(&scratch);
}

/*
 * Each CPU's foreign-context state, in the linear map (one cache line block per CPU): the root it runs the foreign kernel
 * on and its fault handler (fki_cpu_attach), its bounce buffers (file I/O, network send: the foreign kernel's memory is
 * copied while its address space is live, Linux does the I/O on these), and the two address spaces it switches between.
 */
#define FKI_SCRATCH 128
struct fki_cpu {
	struct fki_ttbr1 *ttbr1;
	int (*fault)(void *ctx, unsigned long far, unsigned long esr, void *regs);
	void *fault_ctx;
	unsigned long fault_lo, fault_hi;
	void *bounce;
	unsigned long bounce_size;
	unsigned int bounce_order;
	int attached;
	void *txbuf;
	int mm_active;			/* inside fki_enter_mm (the foreign address space is this CPU's while it runs) */
	int in_runs;
	u64 linux_ttbr0, linux_tcr, linux_ttbr1, linux_tpidr, linux_tpidrro;
	u64 f_ttbr0, f_tcr, f_tpidr, f_tpidrro;
	void (*run_fn)(void *);
	u64 scratch[FKI_SCRATCH / 8] __aligned(16);
} ____cacheline_aligned_in_smp;
static struct fki_cpu *fki_cpus;
static DEFINE_MUTEX(fki_attach_lock);

/* TTBR_ELx.BADDR (48-bit physical addresses; bit 0 is CnP) */
#define FKI_TTBR_BADDR GENMASK_ULL(47, 1)

struct fki_ttbr1 {
	struct list_head node;
	unsigned long lo, hi;
	u64 *l0;			/* the root: a copy of swapper_pg_dir */
	phys_addr_t l0_pa;
	u64 ttbr1;			/* TTBR1_EL1 for it: no ASID (the foreign TCR takes ASIDs from TTBR0), no CnP */
	unsigned int i0;		/* the L0 entry [lo, hi) is in: its L1 table is the root's own copy */
	phys_addr_t linux_l1_pa;	/* Linux's L1 table for i0 when the root was made */
	u64 *l1;
	phys_addr_t l1_pa;
	unsigned int own_first, own_end;	/* L1 entries [lo, hi) covers whole: the foreign kernel's */
	int partial;			/* the L1 entry p_idx covers [lo, hi) only below hi: L2 entries [p_from, n) are Linux's */
	unsigned int p_idx, p_from;
	u64 p_attr;			/* attribute bits of Linux's descriptor for it */
	phys_addr_t linux_l2_pa;	/* Linux's L2 table for p_idx when the root was made (0: none) */
	u64 *l2_boot;			/* the root's own L2 table for p_idx until fki_ttbr1_adopt_l2 */
	phys_addr_t l2_boot_pa;
	u64 *cur_l2;			/* the L2 table p_idx points at now (l2_boot or the foreign kernel's) */
	phys_addr_t cur_l2_pa;
	int adopted;
	atomic_t cpus;			/* CPUs attached to it */
	unsigned long syncs, audit_fixes, audit_conflicts;
	raw_spinlock_t lock;
};
static LIST_HEAD(fki_roots);
static DEFINE_MUTEX(fki_roots_lock);
static atomic_t fki_nroots = ATOMIC_INIT(0);
static int fki_roots_ok;		/* the host qualifies for private roots (checked at load) */

/*
 * Address-space switches (interrupts off, on a stack both spaces map: this task's or fki's alternate stack). Each drops
 * this CPU's TLB: the two worlds' ASIDs are unrelated, and the foreign kernel's global kernel mappings must not outlive
 * its root on this CPU. Linux's values are read again each time: a context switch while Linux ran may have given this
 * task a new ASID.
 */
static __always_inline void fki_mm_to_linux(struct fki_cpu *fc)
{
	fc->f_ttbr0 = read_sysreg(ttbr0_el1);
	fc->f_tcr = read_sysreg(tcr_el1);
	fc->f_tpidr = read_sysreg(tpidr_el0);
	fc->f_tpidrro = read_sysreg(tpidrro_el0);
	write_sysreg(fc->linux_tcr, tcr_el1);
	write_sysreg(fc->linux_ttbr1, ttbr1_el1);
	write_sysreg(fc->linux_ttbr0, ttbr0_el1);
	isb();
	local_flush_tlb_all();
	write_sysreg(fc->linux_tpidr, tpidr_el0);
	write_sysreg(fc->linux_tpidrro, tpidrro_el0);
}

static __always_inline void fki_mm_to_foreign(struct fki_cpu *fc)
{
	struct fki_ttbr1 *t = fc->ttbr1;

	fc->linux_ttbr0 = read_sysreg(ttbr0_el1);
	fc->linux_tcr = read_sysreg(tcr_el1);
	fc->linux_ttbr1 = read_sysreg(ttbr1_el1);
	fc->linux_tpidr = read_sysreg(tpidr_el0);
	fc->linux_tpidrro = read_sysreg(tpidrro_el0);
	write_sysreg(fc->f_tcr, tcr_el1);
	if (t)
		write_sysreg(t->ttbr1, ttbr1_el1);
	write_sysreg(fc->f_ttbr0, ttbr0_el1);
	isb();
	local_flush_tlb_all();
	write_sysreg(fc->f_tpidr, tpidr_el0);
	write_sysreg(fc->f_tpidrro, tpidrro_el0);
}

__attribute__((target("branch-protection=none"))) unsigned long fki_enter(const struct fki_pac_keys *foreign, struct fki_pac_keys *saved)
{
	unsigned long flags;

	fki_fpsimd_forget();
	preempt_disable();
	local_irq_save(flags);
	keys_read(saved);
	keys_write(foreign);
	return flags;
}
EXPORT_SYMBOL(fki_enter);

__attribute__((target("branch-protection=none"))) void fki_leave(unsigned long cookie, const struct fki_pac_keys *saved)
{
	keys_write(saved);
	local_irq_restore(cookie);
	preempt_enable();
}
EXPORT_SYMBOL(fki_leave);

__attribute__((target("branch-protection=none")))
unsigned long fki_enter_mm(const struct fki_pac_keys *foreign, struct fki_pac_keys *saved, fki_u64 ttbr0, fki_u64 tcr)
{
	struct fki_cpu *fc;
	unsigned long flags;

	WARN_ON_ONCE(current->nr_cpus_allowed != 1);
	fki_fpsimd_forget();
	preempt_disable();
	local_irq_save(flags);
	fc = &fki_cpus[smp_processor_id()];
	fc->f_ttbr0 = ttbr0;
	fc->f_tcr = tcr;
	fc->f_tpidr = read_sysreg(tpidr_el0);
	fc->f_tpidrro = read_sysreg(tpidrro_el0);
	fki_mm_to_foreign(fc);
	fc->mm_active = 1;
	keys_read(saved);
	keys_write(foreign);
	return flags;
}
EXPORT_SYMBOL(fki_enter_mm);

__attribute__((target("branch-protection=none"))) void fki_leave_mm(unsigned long cookie, const struct fki_pac_keys *saved)
{
	struct fki_cpu *fc = &fki_cpus[smp_processor_id()];

	keys_write(saved);
	if (fc->mm_active) {
		fc->mm_active = 0;
		write_sysreg(fc->linux_tcr, tcr_el1);
		write_sysreg(fc->linux_ttbr1, ttbr1_el1);
		write_sysreg(fc->linux_ttbr0, ttbr0_el1);
		isb();
		local_flush_tlb_all();
		write_sysreg(fc->linux_tpidr, tpidr_el0);
		write_sysreg(fc->linux_tpidrro, tpidrro_el0);
	}
	WARN_ON_ONCE((read_sysreg(ttbr1_el1) & FKI_TTBR_BADDR) != fki_swapper_pa);
	local_irq_restore(cookie);
	preempt_enable();
}
EXPORT_SYMBOL(fki_leave_mm);

__attribute__((target("branch-protection=none"))) void fki_pac_swap(const struct fki_pac_keys *load, struct fki_pac_keys *save)
{
	keys_read(save);
	keys_write(load);
}
EXPORT_SYMBOL(fki_pac_swap);

static phys_addr_t fki_empty_pa;	/* an empty table page: a foreign kernel's first TTBR0 */
fki_u64 fki_empty_ttbr0(void)
{
	return fki_empty_pa;
}
EXPORT_SYMBOL(fki_empty_ttbr0);

fki_u64 fki_counter_frequency(void)
{
	return arch_timer_get_cntfrq();
}
EXPORT_SYMBOL(fki_counter_frequency);

fki_u64 fki_counter(void)
{
	return __arch_counter_get_cntvct();
}
EXPORT_SYMBOL(fki_counter);

void fki_random(void *buf, unsigned long len)
{
	get_random_bytes(buf, len);
}
EXPORT_SYMBOL(fki_random);

int fki_bind_cpu(int cpu)
{
	if (cpu < 0 || cpu >= nr_cpu_ids || !cpu_online(cpu))
		return -EINVAL;
	return set_cpus_allowed_ptr(current, cpumask_of(cpu));
}
EXPORT_SYMBOL(fki_bind_cpu);

void *fki_thread_start(int (*fn)(void *), void *arg, int cpu, const char *name)
{
	struct task_struct *t;

	if (cpu < 0 || cpu >= nr_cpu_ids || !cpu_online(cpu))
		return NULL;
	t = kthread_create(fn, arg, "%s", name);
	if (IS_ERR(t))
		return NULL;
	kthread_bind(t, cpu);
	get_task_struct(t);
	wake_up_process(t);
	return t;
}
EXPORT_SYMBOL(fki_thread_start);

void fki_thread_stop(void *thread)
{
	if (thread) {
		kthread_stop(thread);
		put_task_struct(thread);
	}
}
EXPORT_SYMBOL(fki_thread_stop);

int fki_thread_should_stop(void)
{
	return kthread_should_stop();
}
EXPORT_SYMBOL(fki_thread_should_stop);

void fki_thread_wait(const volatile unsigned long *flag)
{
	for (;;) {
		set_current_state(TASK_INTERRUPTIBLE);
		if (READ_ONCE(*flag) || kthread_should_stop())
			break;
		schedule();
	}
	__set_current_state(TASK_RUNNING);
}
EXPORT_SYMBOL(fki_thread_wait);

void fki_thread_wake(void *thread)
{
	if (thread)
		wake_up_process(thread);
}
EXPORT_SYMBOL(fki_thread_wake);

void fki_msleep(unsigned int ms)
{
	msleep(ms);
}
EXPORT_SYMBOL(fki_msleep);

/*
 * Foreign kernel faults. Out of tree, the hook is a kprobe on __do_kernel_fault(far, esr, regs), where Linux's abort path
 * goes for a kernel address it has no mapping for (do_mem_abort and do_translation_fault cannot be probed). When
 * the handler resolves the fault, __do_kernel_fault is skipped (return to its caller), and the exception returns to retry
 * the access. The in-tree patch calls the hook from do_translation_fault/do_page_fault directly. The probes are
 * registered once, at load; the handler is per CPU (fki_cpu_attach) or, version 0, one for all (fki_fault_register).
 */
static unsigned long fki_fault_lo, fki_fault_hi;
static int (*fki_fault_handler)(unsigned long far, unsigned long esr, void *regs);
static int fki_fault_legacy;		/* fki_fault_register's (excludes per-CPU attach) */
static int fki_cpus_attached;
static int fki_ttbr1_sync(struct fki_ttbr1 *t, unsigned long far);

static int fki_fault_pre(struct kprobe *p, struct pt_regs *kregs)
{
	unsigned long far = kregs->regs[0], esr = kregs->regs[1];
	struct pt_regs *regs = (struct pt_regs *)kregs->regs[2];
	int (*hc)(void *, unsigned long, unsigned long, void *);
	int (*h)(unsigned long, unsigned long, void *);
	struct fki_cpu *fc;
	struct fki_ttbr1 *t;

	if (!regs || user_mode(regs) || !fki_cpus)
		return 0;
	fc = &fki_cpus[smp_processor_id()];
	/*
	 * Lazy sync first: a Linux address (outside the foreign range) on a CPU running on a private root, which Linux has
	 * mapped since the root was copied. Copy the missing entry in and retry.
	 */
	t = READ_ONCE(fc->ttbr1);
	if (t && far >= (~0UL << VA_BITS) && (far < t->lo || far >= t->hi) &&
	    (read_sysreg(ttbr1_el1) & FKI_TTBR_BADDR) == t->l0_pa && fki_ttbr1_sync(t, far)) {
		instruction_pointer_set(kregs, kregs->regs[30]);	/* __do_kernel_fault returns at once */
		return 1;
	}
	hc = smp_load_acquire(&fc->fault);
	if (hc) {
		if ((far < fc->fault_lo || far >= fc->fault_hi) &&
		    (instruction_pointer(regs) < fc->fault_lo || instruction_pointer(regs) >= fc->fault_hi))
			return 0;
		if (hc(fc->fault_ctx, far, esr, regs))
			return 0;
		instruction_pointer_set(kregs, kregs->regs[30]);
		return 1;
	}
	h = READ_ONCE(fki_fault_handler);
	if (!h)
		return 0;
	if ((far < fki_fault_lo || far >= fki_fault_hi) &&
	    (instruction_pointer(regs) < fki_fault_lo || instruction_pointer(regs) >= fki_fault_hi))
		return 0;
	if (h(far, esr, regs))
		return 0;
	instruction_pointer_set(kregs, kregs->regs[30]);	/* __do_kernel_fault returns at once */
	return 1;
}
NOKPROBE_SYMBOL(fki_fault_pre);

unsigned long fki_regs_pc(void *regs) { return instruction_pointer((struct pt_regs *)regs); }
EXPORT_SYMBOL(fki_regs_pc);
unsigned long fki_regs_lr(void *regs) { return ((struct pt_regs *)regs)->regs[30]; }
EXPORT_SYMBOL(fki_regs_lr);
unsigned long fki_regs_pstate(void *regs) { return ((struct pt_regs *)regs)->pstate; }
EXPORT_SYMBOL(fki_regs_pstate);
void fki_regs_set_pc(void *regs, unsigned long pc) { instruction_pointer_set((struct pt_regs *)regs, pc); }
EXPORT_SYMBOL(fki_regs_set_pc);
void fki_regs_set_reg(void *regs, unsigned int n, unsigned long v) { if (n < 31) ((struct pt_regs *)regs)->regs[n] = v; }
EXPORT_SYMBOL(fki_regs_set_reg);
unsigned long fki_regs_reg(void *regs, unsigned int n) { return n < 31 ? ((struct pt_regs *)regs)->regs[n] : 0; }
EXPORT_SYMBOL(fki_regs_reg);

static struct kprobe fki_fault_kp = { .symbol_name = "__do_kernel_fault", .pre_handler = fki_fault_pre };
static int fki_fault_registered;

/*
 * Other kernel-mode exceptions the foreign kernel takes (an FPAC authentication failure, an undefined instruction, a
 * BTI fault) end in die(str, regs, err): offered to the same handler with far 0 and the ESR, before Linux oopses.
 */
static int fki_die_pre(struct kprobe *p, struct pt_regs *kregs)
{
	struct pt_regs *regs = (struct pt_regs *)kregs->regs[1];
	unsigned long esr = kregs->regs[2];
	int (*hc)(void *, unsigned long, unsigned long, void *);
	int (*h)(unsigned long, unsigned long, void *);
	struct fki_cpu *fc = fki_cpus ? &fki_cpus[smp_processor_id()] : NULL;

	if (!regs || user_mode(regs) || !fc)
		goto linux_die;
	hc = smp_load_acquire(&fc->fault);
	if (hc) {
		if (instruction_pointer(regs) < fc->fault_lo || instruction_pointer(regs) >= fc->fault_hi)
			goto linux_die;
		if (hc(fc->fault_ctx, 0, esr, regs))
			goto linux_die;
		instruction_pointer_set(kregs, kregs->regs[30]);
		return 1;
	}
	h = READ_ONCE(fki_fault_handler);
	if (!h)
		goto linux_die;
	if (instruction_pointer(regs) < fki_fault_lo || instruction_pointer(regs) >= fki_fault_hi)
		goto linux_die;
	if (h(0, esr, regs))
		goto linux_die;
	instruction_pointer_set(kregs, kregs->regs[30]);	/* die returns at once */
	return 1;
linux_die:
	/* Leaving die() to oops with the foreign TTBR1/TCR/TPIDR live breaks T1 across subsequent Linux tasks. */
	if (fc && fc->mm_active) {
		fc->mm_active = 0;
		fki_mm_to_linux(fc);
	}
	return 0;
}
NOKPROBE_SYMBOL(fki_die_pre);
static struct kprobe fki_die_kp = { .symbol_name = "die", .pre_handler = fki_die_pre };
static int fki_die_registered;

int fki_fault_register(unsigned long lo, unsigned long hi, int (*handler)(unsigned long far, unsigned long esr, void *regs))
{
	int err = 0;

	mutex_lock(&fki_attach_lock);
	if (fki_fault_legacy || fki_cpus_attached)
		err = -EBUSY;	/* one registration at a time, and never with per-CPU handlers */
	else if (!fki_fault_registered)
		err = -ENOENT;
	if (!err) {
		fki_fault_lo = lo;
		fki_fault_hi = hi;
		WRITE_ONCE(fki_fault_handler, handler);
		fki_fault_legacy = 1;
		if (!fki_die_registered)
			pr_warn("fki: no die() probe: foreign non-abort exceptions oops\n");
	}
	mutex_unlock(&fki_attach_lock);
	return err;
}
EXPORT_SYMBOL(fki_fault_register);

void fki_fault_unregister(void)
{
	mutex_lock(&fki_attach_lock);
	if (fki_fault_legacy) {
		WRITE_ONCE(fki_fault_handler, NULL);
		synchronize_rcu();	/* a handler running now (kprobe handlers run with preemption off) has returned */
		fki_fault_legacy = 0;
	}
	mutex_unlock(&fki_attach_lock);
}
EXPORT_SYMBOL(fki_fault_unregister);

fki_u64 fki_phys_peek(fki_u64 pa)
{
	if (!pfn_valid(PHYS_PFN(pa)))
		return ~0ULL;
	return READ_ONCE(*(u64 *)phys_to_virt(pa & ~7ULL));
}
EXPORT_SYMBOL(fki_phys_peek);

/* ---- contiguous memory and the top foreign window ---- */

/*
 * The foreign kernels' DRAM, each one physically contiguous block of GBs, from the first that works of:
 *  - carve_base/carve_mb: RAM the host kernel does not manage at all (its command line limits it with mem=, the rest is
 *    left for us), checked and mapped once at load (memremap: its page tables exist before any private root is made)
 *    and handed out in slices, first fit, whole L2 blocks. Nothing of Linux's ever lands in it: the block is always there.
 *    A slice is only for a block of 64 MiB or more; a full carve-out is -ENOSPC unless carve_fallback=1.
 *  - reserve_mb: taken from the CMA area when fki loads early in the host's boot. A single page pinned in the area
 *    (a file system's buffer-head page of the block device, allocated movable during the boot) can already be there
 *    and split the area, so this is best effort.
 *  - a CMA or alloc_contig_pages allocation at the time of the call.
 */
static unsigned long carve_base, carve_mb, reserve_mb;
module_param(carve_base, ulong, 0444);
module_param(carve_mb, ulong, 0444);
module_param(reserve_mb, ulong, 0444);
static int carve_fallback;
module_param(carve_fallback, int, 0444);
static struct page *resv_pg;
static unsigned long resv_cnt;
static bool resv_used;
static void *carve_va;			/* the whole carve-out, mapped at load */
static unsigned long carve_len;		/* whole L2 blocks */
static int carve_ok, carve_err;
#define FKI_SLICES 8
static struct { unsigned long off, len; int used; } fki_slices[FKI_SLICES];
static DEFINE_SPINLOCK(fki_carve_lock);

static void fki_reserve(void)
{
	unsigned long cnt = ALIGN(reserve_mb << 20, PMD_SIZE) >> PAGE_SHIFT;	/* in L2 blocks: 2 MiB (4 KB granule), 32 MiB (16 KB) */

	if (!reserve_mb || fki_resolve() || !p_cma_area || !*p_cma_area || !p_cma_alloc)
		return;
	resv_pg = p_cma_alloc(*p_cma_area, cnt, get_order(PMD_SIZE), true);	/* aligned to an L2 block */
	if (resv_pg && (page_to_phys(resv_pg) & (PMD_SIZE - 1))) {
		p_cma_release(*p_cma_area, resv_pg, cnt);
		resv_pg = NULL;
	}
	if (resv_pg)
		resv_cnt = cnt;
	pr_info("fki: %lu MiB for the foreign kernel's DRAM %s\n", reserve_mb, resv_pg ? "reserved" : "NOT reserved (CMA)");
}

static int fki_carve_check(unsigned long base, unsigned long len)
{
	unsigned long end = base + len;

	if (end < base) {
		pr_err("fki: carve-out [%#lx, %#lx) overflows physical address space\n", base, end);
		return -EINVAL;
	}
	/* RAM in the firmware's map, every 2 MiB of it: a carve-out past the end of the VM's memory (carve_base from a
	 * bigger VM) would map addresses with nothing behind them */
	if (p_efi_mem_type && p_efi_mem_type(base) >= 0) {	/* < 0: no EFI memory map (not an EFI boot) */
		unsigned long a;

		for (a = base; a < end; a += SZ_2M) {
			int t = p_efi_mem_type(a);

			/* free RAM once Linux runs: loader and boot-services code and data, conventional memory */
			if (t != EFI_LOADER_CODE && t != EFI_LOADER_DATA && t != EFI_BOOT_SERVICES_CODE &&
			    t != EFI_BOOT_SERVICES_DATA && t != EFI_CONVENTIONAL_MEMORY) {
				pr_err("fki: carve-out [%#lx, %#lx) is not all RAM (EFI type %d at %#lx); refusing to map (check carve_base, carve_mb and the VM's memory size)\n",
				       base, end, t, a);
				return -EINVAL;
			}
		}
	}
	if (p_stext && p_end && p_kimage_voffset) {
		unsigned long k_start = p_stext - *p_kimage_voffset;
		unsigned long k_end = p_end - *p_kimage_voffset;

		if (base < k_end && k_start < end) {
			pr_err("fki: carve-out [%#lx, %#lx) overlaps Linux kernel image [%#lx, %#lx); refusing to map (boot with nokaslr or adjust carve_base)\n",
			       base, end, k_start, k_end);
			return -EBUSY;
		}
	}
	if (region_intersects(base, len, IORESOURCE_SYSTEM_RAM, IORES_DESC_NONE) != REGION_DISJOINT) {
		pr_err("fki: carve-out [%#lx, %#lx) overlaps IORESOURCE_SYSTEM_RAM; refusing to map (check mem= and carve_base)\n",
		       base, end);
		return -EBUSY;
	}
	if (region_intersects(base, len, IORESOURCE_MEM, IORES_DESC_NONE) != REGION_DISJOINT) {
		pr_err("fki: carve-out [%#lx, %#lx) overlaps busy/reserved IORESOURCE_MEM; refusing to map (check /proc/iomem and carve_base)\n",
		       base, end);
		return -EBUSY;
	}
	return 0;
}

/* At load: the whole carve-out checked and mapped once */
static void fki_carve_init(void)
{
	if (!carve_mb)
		return;
	carve_len = ALIGN_DOWN(carve_mb << 20, PMD_SIZE);
	if ((carve_base & (PMD_SIZE - 1)) || !carve_len) {
		pr_err("fki: carve-out at %#lx, %lu MiB is not whole L2 blocks (%lu MiB aligned)\n", carve_base, carve_mb,
		       PMD_SIZE >> 20);
		carve_err = -EINVAL;
		return;
	}
	carve_err = fki_carve_check(carve_base, carve_len);
	if (carve_err)
		return;
	carve_va = memremap(carve_base, carve_len, MEMREMAP_WB);
	if (!carve_va) {
		pr_warn("fki: carve-out at %#lx not mappable\n", carve_base);
		carve_err = -ENOMEM;
		return;
	}
	carve_ok = 1;
	pr_info("fki: %lu MiB at %#lx for foreign kernels' DRAM (outside the host's memory), slices of %lu MiB blocks\n",
		carve_len >> 20, carve_base, PMD_SIZE >> 20);
}

static int fki_carve_alloc(unsigned long size, struct fki_region *r)
{
	unsigned long len = ALIGN(size, PMD_SIZE), cand[FKI_SLICES + 1], off = ~0UL;
	int i, j, n = 0, s = -1;

	spin_lock(&fki_carve_lock);
	cand[n++] = 0;
	for (i = 0; i < FKI_SLICES; i++)
		if (fki_slices[i].used)
			cand[n++] = fki_slices[i].off + fki_slices[i].len;
		else if (s < 0)
			s = i;
	/* first fit: the lowest gap start that holds len */
	for (i = 0; i < n && s >= 0; i++) {
		unsigned long c = cand[i];
		int fits = c + len <= carve_len && c + len > c;

		for (j = 0; j < FKI_SLICES && fits; j++)
			if (fki_slices[j].used && c < fki_slices[j].off + fki_slices[j].len && fki_slices[j].off < c + len)
				fits = 0;
		if (fits && c < off)
			off = c;
	}
	if (s < 0 || off == ~0UL) {
		spin_unlock(&fki_carve_lock);
		return -ENOSPC;
	}
	fki_slices[s].off = off;
	fki_slices[s].len = len;
	fki_slices[s].used = 1;
	spin_unlock(&fki_carve_lock);
	memset((char *)carve_va + off, 0, len);	/* GBs: outside the lock */
	r->va = (char *)carve_va + off;
	r->pa = carve_base + off;
	r->size = len;
	r->handle = (void *)(((unsigned long)s << 3) | 4);	/* bit 2: a carve-out slice, its index above */
	pr_info("fki: carve-out slice %d: pa %#llx, %lu MiB\n", s, (unsigned long long)r->pa, len >> 20);
	return 0;
}

int fki_contig_alloc(unsigned long size, struct fki_region *r)
{
	unsigned long n = 1;
	struct page *pg;
	int err = fki_resolve();

	if (err)
		return err;
	/* whole L2 blocks (PMD_SIZE: 2 MiB with a 4 KB granule, 32 MiB with 16 KB): the foreign kernel maps its DRAM with them */
	if (carve_mb && size >= SZ_64M) {
		err = carve_ok ? fki_carve_alloc(size, r) : carve_err;
		if (!err || !carve_fallback)
			return err;
	}
	if (size >= SZ_64M && resv_pg && !resv_used && ALIGN(size, PMD_SIZE) >> PAGE_SHIFT <= resv_cnt) {
		resv_used = true;
		r->va = page_address(resv_pg);
		r->size = ALIGN(size, PMD_SIZE);
		memset(r->va, 0, r->size);
		r->pa = page_to_phys(resv_pg);
		r->handle = (void *)((unsigned long)resv_pg | 2);	/* bit 1: the reservation, kept when freed */
		return 0;
	}
	/*
	 * Large blocks (a hosted kernel's DRAM, GBs) come from the CMA area when there is one: a long-running host
	 * fragments its memory with unmovable pages and alloc_contig_pages then fails. CMA blocks are L2-block aligned
	 * and sized (PMD_SIZE); a CMA block's handle has bit 0 set.
	 */
	if (size >= (64UL << 20) && p_cma_area && *p_cma_area && p_cma_alloc && p_cma_release) {
		unsigned long cnt = ALIGN(size, PMD_SIZE) >> PAGE_SHIFT;
		pg = p_cma_alloc(*p_cma_area, cnt, get_order(PMD_SIZE), true);
		if (pg && (page_to_phys(pg) & (PMD_SIZE - 1))) {
			p_cma_release(*p_cma_area, pg, cnt);
			pg = NULL;
		}
		if (pg) {
			r->va = page_address(pg);
			memset(r->va, 0, cnt << PAGE_SHIFT);
			r->pa = page_to_phys(pg);
			r->size = cnt << PAGE_SHIFT;
			r->handle = (void *)((unsigned long)pg | 1);
			return 0;
		}
	}
	if (!p_alloc_contig)
		return -ENOENT;
	while ((n << PAGE_SHIFT) < size)
		n <<= 1;
	pg = p_alloc_contig(n, GFP_KERNEL | __GFP_NOWARN, numa_node_id(), NULL);	/* naturally aligned (power of two) */
	if (!pg)
		return -ENOMEM;
	r->va = page_address(pg);
	memset(r->va, 0, n << PAGE_SHIFT);
	r->pa = page_to_phys(pg);
	r->size = n << PAGE_SHIFT;
	r->handle = pg;
	return 0;
}
EXPORT_SYMBOL(fki_contig_alloc);

void fki_contig_free(struct fki_region *r)
{
	unsigned long h = (unsigned long)r->handle;

	if ((h & 7) == 4) {
		unsigned int s = h >> 3;

		if (s < FKI_SLICES) {
			spin_lock(&fki_carve_lock);
			fki_slices[s].used = 0;
			spin_unlock(&fki_carve_lock);
			pr_info("fki: carve-out slice %u freed\n", s);
		}
		return;
	}
	if (h & 2) {
		resv_used = false;	/* back to the reservation, for the next run */
		return;
	}
	if (h & 1) {
		p_cma_release(*p_cma_area, (struct page *)(h & ~1UL), r->size >> PAGE_SHIFT);
		return;
	}
	if (p_free_contig)
		p_free_contig(page_to_pfn((struct page *)r->handle), r->size >> PAGE_SHIFT);
}
EXPORT_SYMBOL(fki_contig_free);

unsigned fki_page_shift(void)
{
	return PAGE_SHIFT;
}
EXPORT_SYMBOL(fki_page_shift);

static inline void fki_tlb_flush_all(void)
{
	dsb(ishst);
	__tlbi(vmalle1is);
	dsb(ish);
	isb();
}

/* ---- version 0: the foreign window inside Linux's own tables (one foreign kernel; kept as it was) ---- */

/* The kernel half's L1 (pud) table for va: Linux's own walk, so 4 KB (L0: 512 entries) and 16 KB (L0: 2) granules alike */
int fki_window_l1(unsigned long va, unsigned long long **l1_va, fki_u64 *l1_pa)
{
	pgd_t *pgdp;
	p4d_t *p4dp;
	p4d_t p4d;

	if (fki_resolve())
		return -ENOENT;
	pgdp = pgd_offset_pgd(p_init_mm->pgd, va);
	if (pgd_none(READ_ONCE(*pgdp)))
		return -ENOENT;
	p4dp = p4d_offset(pgdp, va);
	p4d = READ_ONCE(*p4dp);
	if (p4d_none(p4d) || (p4d_val(p4d) & 3) != 3)
		return -ENOENT;
	*l1_pa = p4d_page_paddr(p4d);
	*l1_va = (unsigned long long *)phys_to_virt(*l1_pa);
	return 0;
}
EXPORT_SYMBOL(fki_window_l1);

void fki_window_set(unsigned long long *slot, fki_u64 desc)
{
	WRITE_ONCE(*slot, desc);
	fki_tlb_flush_all();
}
EXPORT_SYMBOL(fki_window_set);

/*
 * The shared L1 entry (16 KB granule): which entries of Linux's L2 table fki_window_l2_set installed (the foreign kernel's
 * bootstrap blocks), for one L2 table at a time, and the entry while the foreign kernel's own L2 table has it.
 */
static DECLARE_BITMAP(fki_l2_owned, PTRS_PER_PMD);
static phys_addr_t fki_l2_owned_table;		/* the Linux L2 table those bits are of */
static unsigned long fki_l2_adopted_va;		/* nonzero: the shared L1 entry for it points at the foreign kernel's L2 */
static u64 fki_l2_adopted_old, fki_l2_adopted_new;
static unsigned long long *fki_l2_adopted_cur;
static unsigned fki_l2_adopted_lo, fki_l2_adopted_hi;

static int fki_l1_entry(unsigned long va, u64 **slot)
{
	unsigned long long *l1;
	fki_u64 l1pa;
	int err = fki_window_l1(va, &l1, &l1pa);

	if (err)
		return err;
	*slot = (u64 *)&l1[pud_index(va)];
	return 0;
}

int fki_window_l2(unsigned long va, unsigned long long **l2_va, fki_u64 *l2_pa)
{
	u64 *l1e;
	u64 e;
	int err = fki_l1_entry(va, &l1e);

	if (err)
		return err;
	e = READ_ONCE(*l1e);
	if ((e & 3) != 3)	/* empty, or a block: not a table */
		return -ENOENT;
	*l2_pa = pud_page_paddr(__pud(e));
	if (!pfn_valid(PHYS_PFN(*l2_pa)))	/* the foreign kernel's own (adopted): not in the linear map */
		return -EBUSY;
	*l2_va = (unsigned long long *)phys_to_virt(*l2_pa);
	return 0;
}
EXPORT_SYMBOL(fki_window_l2);

int fki_window_l2_set(unsigned long va, fki_u64 desc)
{
	unsigned long long *l2;
	fki_u64 l2pa;
	unsigned i = pmd_index(va);
	int err = fki_resolve();

	if (err)
		return err;
	spin_lock(&p_init_mm->page_table_lock);
	err = fki_l2_adopted_va ? -EBUSY : fki_window_l2(va, &l2, &l2pa);
	if (err)
		goto out;
	if (!bitmap_empty(fki_l2_owned, PTRS_PER_PMD) && fki_l2_owned_table != l2pa) {
		err = -EBUSY;	/* bootstrap entries in another L2 table still installed */
		goto out;
	}
	if (desc) {
		if (READ_ONCE(l2[i])) {
			err = -EBUSY;	/* Linux's (or already ours) */
			goto out;
		}
		WRITE_ONCE(l2[i], desc);
		__set_bit(i, fki_l2_owned);
		fki_l2_owned_table = l2pa;
	} else {
		if (!test_bit(i, fki_l2_owned)) {
			err = READ_ONCE(l2[i]) ? -EPERM : 0;	/* never clear an entry of Linux's */
			goto out;
		}
		WRITE_ONCE(l2[i], 0);
		__clear_bit(i, fki_l2_owned);
	}
	fki_tlb_flush_all();
out:
	spin_unlock(&p_init_mm->page_table_lock);
	return err;
}
EXPORT_SYMBOL(fki_window_l2_set);

int fki_window_l2_adopt(unsigned long va, unsigned long long *new_l2_va, fki_u64 new_desc, unsigned lo, unsigned hi,
			fki_u64 *old_desc)
{
	unsigned long long *l2;
	fki_u64 l2pa;
	u64 *l1e;
	u64 old;
	unsigned i;
	int err;

	if (lo >= hi || hi > PTRS_PER_PMD || !new_l2_va || (new_desc & 3) != 3)
		return -EINVAL;
	if (fki_resolve())
		return -ENOENT;
	spin_lock(&p_init_mm->page_table_lock);
	err = fki_l2_adopted_va ? -EBUSY : fki_l1_entry(va, &l1e);
	if (err)
		goto out;
	old = READ_ONCE(*l1e);
	err = fki_window_l2(va, &l2, &l2pa);
	if (err)
		goto out;
	if (!bitmap_empty(fki_l2_owned, PTRS_PER_PMD) && fki_l2_owned_table != l2pa) {
		err = -EBUSY;
		goto out;
	}
	/* the foreign kernel's range: Linux has nothing there but the bootstrap blocks */
	for (i = lo; i < hi; i++) {
		if (READ_ONCE(l2[i]) && !test_bit(i, fki_l2_owned)) {
			pr_err("fki: L2 entry %u of the shared L1 entry for %#lx is Linux's (%#llx): not adopting\n", i, va,
			       (unsigned long long)READ_ONCE(l2[i]));
			err = -EBUSY;
			goto out;
		}
	}
	/* Linux's entries outside it go into the new table, which has nothing there */
	for (i = 0; i < PTRS_PER_PMD; i++) {
		u64 e;

		if (i >= lo && i < hi)
			continue;
		e = READ_ONCE(l2[i]);
		if (!e)
			continue;
		if (new_l2_va[i] && new_l2_va[i] != e) {
			pr_err("fki: the foreign L2 table has entry %u (%#llx) where Linux has %#llx: not adopting\n", i,
			       new_l2_va[i], (unsigned long long)e);
			err = -EBUSY;
			goto out;
		}
	}
	for (i = 0; i < PTRS_PER_PMD; i++) {
		u64 e;

		if (i >= lo && i < hi)
			continue;
		e = READ_ONCE(l2[i]);
		if (e) {
			WRITE_ONCE(new_l2_va[i], e);
			pr_info("fki: shared L1 entry for %#lx: Linux's L2 entry %u (%#llx) kept in the foreign table\n", va, i,
				(unsigned long long)e);
		}
	}
	dsb(ishst);
	/* one store: every walk sees Linux's table (bootstrap blocks) or the new one (the foreign kernel's own tables) */
	WRITE_ONCE(*l1e, new_desc);
	fki_tlb_flush_all();
	for (i = lo; i < hi; i++) {
		if (test_bit(i, fki_l2_owned)) {
			WRITE_ONCE(l2[i], 0);
			__clear_bit(i, fki_l2_owned);
		}
	}
	fki_tlb_flush_all();
	fki_l2_adopted_va = va;
	fki_l2_adopted_old = old;
	fki_l2_adopted_new = new_desc;
	fki_l2_adopted_cur = new_l2_va;
	fki_l2_adopted_lo = lo;
	fki_l2_adopted_hi = hi;
	*old_desc = old;
out:
	spin_unlock(&p_init_mm->page_table_lock);
	return err;
}
EXPORT_SYMBOL(fki_window_l2_adopt);

static void fki_window_l2_restore_locked(unsigned long va, unsigned long long *cur_l2_va, fki_u64 old_desc, unsigned lo,
					 unsigned hi)
{
	unsigned long long *l2;
	phys_addr_t l2pa = pud_page_paddr(__pud(old_desc));
	u64 *l1e;
	unsigned i;

	if (fki_l1_entry(va, &l1e) || (old_desc & 3) != 3 || !pfn_valid(PHYS_PFN(l2pa))) {
		pr_err("fki: cannot restore the shared L1 entry for %#lx (old %#llx)\n", va, (unsigned long long)old_desc);
		return;
	}
	if (READ_ONCE(*l1e) != fki_l2_adopted_new)
		pr_warn("fki: shared L1 entry for %#lx is %#llx, not the foreign table's %#llx: restoring Linux's anyway\n", va,
			(unsigned long long)READ_ONCE(*l1e), (unsigned long long)fki_l2_adopted_new);
	l2 = (unsigned long long *)phys_to_virt(l2pa);
	for (i = 0; i < PTRS_PER_PMD; i++) {
		if (i >= lo && i < hi)
			WRITE_ONCE(l2[i], 0);
		else if (cur_l2_va && READ_ONCE(cur_l2_va[i]) != READ_ONCE(l2[i]))
			WRITE_ONCE(l2[i], READ_ONCE(cur_l2_va[i]));	/* what Linux changed there meanwhile */
	}
	dsb(ishst);
	WRITE_ONCE(*l1e, old_desc);
	fki_tlb_flush_all();
	fki_l2_adopted_va = 0;
	fki_l2_adopted_cur = NULL;
	pr_info("fki: shared L1 entry for %#lx back to Linux's L2 table (%#llx)\n", va, (unsigned long long)old_desc);
}

void fki_window_l2_restore(unsigned long va, unsigned long long *cur_l2_va, fki_u64 old_desc, unsigned lo, unsigned hi)
{
	if (fki_resolve()) {
		pr_err("fki: cannot restore the shared L1 entry for %#lx\n", va);
		return;
	}
	spin_lock(&p_init_mm->page_table_lock);
	if (!fki_l2_adopted_va || fki_l2_adopted_va != va)
		pr_warn("fki: restoring the shared L1 entry for %#lx, which fki did not hand over\n", va);
	fki_window_l2_restore_locked(va, cur_l2_va, old_desc, lo, hi);
	spin_unlock(&p_init_mm->page_table_lock);
}
EXPORT_SYMBOL(fki_window_l2_restore);

/* ---- version 1: private kernel-half roots ---- */

#define FKI_DESC_PA(d) __pte_to_phys(__pte(d))
#define FKI_DESC_ATTR(d) ((d) ^ (u64)__phys_to_pte_val(FKI_DESC_PA(d)))
#define FKI_IS_TABLE(d) (((d) & 3) == 3)

/* A Linux table by physical address, through the linear map (NULL when it is not RAM Linux has) */
static u64 *fki_table_va(phys_addr_t pa)
{
	if (!pa || !pfn_valid(PHYS_PFN(pa)))
		return NULL;
	return (u64 *)phys_to_virt(pa);
}

/* Linux's L1 table for L0 entry i0, now */
static u64 *fki_linux_l1(unsigned int i0)
{
	u64 e = READ_ONCE(p_swapper[i0]);

	return FKI_IS_TABLE(e) ? fki_table_va(FKI_DESC_PA(e)) : NULL;
}

/* Linux's L2 table for the root's partial L1 entry, now */
static u64 *fki_linux_l2(struct fki_ttbr1 *t)
{
	u64 *ll1 = fki_linux_l1(t->i0), e;

	if (!ll1)
		return NULL;
	e = READ_ONCE(ll1[t->p_idx]);
	return FKI_IS_TABLE(e) ? fki_table_va(FKI_DESC_PA(e)) : NULL;
}

/*
 * Switch one live table entry break-before-make with nothing in between that could need the range it maps (the caller's
 * stack may be in it): no stack, no other memory. x0 = the entry, x1 = the new descriptor.
 */
asm(
"	.pushsection .text\n"
"	.type	fki_bbm_swap, %function\n"
"fki_bbm_swap:\n"
"	hint	#34\n"
"	str	xzr, [x0]\n"
"	dsb	ish\n"
"	tlbi	vmalle1is\n"
"	dsb	ish\n"
"	isb\n"
"	str	x1, [x0]\n"
"	dsb	ish\n"
"	isb\n"
"	ret\n"
"	.size	fki_bbm_swap, . - fki_bbm_swap\n"
"	.popsection\n");
void fki_bbm_swap(u64 *slot, u64 desc);

struct fki_ttbr1 *fki_ttbr1_create(unsigned long lo, unsigned long hi, int *errp)
{
	struct fki_ttbr1 *t;
	u64 e0, ep = 0, *ll1, *ll2 = NULL;
	unsigned int i;
	int err = 0;

	if (!fki_roots_ok) {
		err = -EBUSY;
		pr_err("fki: this host cannot give a foreign kernel a root of its own (see fki's load messages)\n");
		goto fail0;
	}
	if (lo >= hi || lo < (~0UL << VA_BITS) || (lo & (PUD_SIZE - 1)) || (hi & (PMD_SIZE - 1)) ||
	    pgd_index(lo) != pgd_index(hi - 1)) {
		err = -EINVAL;
		goto fail0;
	}
	if ((read_sysreg(ttbr1_el1) & FKI_TTBR_BADDR) != fki_swapper_pa) {
		pr_err("fki: TTBR1 is not swapper_pg_dir\n");
		err = -EBUSY;
		goto fail0;
	}
	t = kzalloc(sizeof(*t), GFP_KERNEL);
	if (!t) {
		err = -ENOMEM;
		goto fail0;
	}
	raw_spin_lock_init(&t->lock);
	atomic_set(&t->cpus, 0);
	t->lo = lo;
	t->hi = hi;
	t->i0 = pgd_index(lo);
	t->own_first = pud_index(lo);
	if (hi & (PUD_SIZE - 1)) {
		t->partial = 1;
		t->p_idx = pud_index(hi);
		t->p_from = pmd_index(hi);
		t->own_end = t->p_idx;
	} else {
		t->own_end = pud_index(hi - 1) + 1;
	}
	t->l0 = (u64 *)get_zeroed_page(GFP_KERNEL);
	t->l1 = (u64 *)get_zeroed_page(GFP_KERNEL);
	if (t->partial)
		t->l2_boot = (u64 *)get_zeroed_page(GFP_KERNEL);
	if (!t->l0 || !t->l1 || (t->partial && !t->l2_boot)) {
		err = -ENOMEM;
		goto fail;
	}
	t->l0_pa = virt_to_phys(t->l0);
	t->l1_pa = virt_to_phys(t->l1);
	if (t->partial)
		t->l2_boot_pa = virt_to_phys(t->l2_boot);

	spin_lock(&p_init_mm->page_table_lock);
	e0 = READ_ONCE(p_swapper[t->i0]);
	ll1 = FKI_IS_TABLE(e0) ? fki_table_va(FKI_DESC_PA(e0)) : NULL;
	if (!ll1) {
		err = -ENOENT;
		goto unlock;
	}
	/* [lo, hi) empty in Linux: whole L1 entries, and the partial entry's L2 entries below hi */
	for (i = t->own_first; i < t->own_end; i++)
		if (READ_ONCE(ll1[i])) {
			pr_err("fki: Linux maps L1 entry %u of [%#lx, %#lx) (%#llx): no private root\n", i, lo, hi,
			       (unsigned long long)READ_ONCE(ll1[i]));
			err = -EBUSY;
			goto unlock;
		}
	if (t->partial) {
		ep = READ_ONCE(ll1[t->p_idx]);
		if (ep && !FKI_IS_TABLE(ep)) {
			err = -EBUSY;
			goto unlock;
		}
		ll2 = ep ? fki_table_va(FKI_DESC_PA(ep)) : NULL;
		if (ep && !ll2) {
			err = -EBUSY;
			goto unlock;
		}
		for (i = 0; ll2 && i < t->p_from; i++)
			if (READ_ONCE(ll2[i])) {
				pr_err("fki: Linux maps L2 entry %u of L1 entry %u in [%#lx, %#lx) (%#llx): no private root\n", i,
				       t->p_idx, lo, hi, (unsigned long long)READ_ONCE(ll2[i]));
				err = -EBUSY;
				goto unlock;
			}
		for (i = t->p_from; ll2 && i < PTRS_PER_PMD; i++)
			t->l2_boot[i] = READ_ONCE(ll2[i]);
		t->linux_l2_pa = ep ? FKI_DESC_PA(ep) : 0;
		t->p_attr = ep ? FKI_DESC_ATTR(ep) : (PUD_TYPE_TABLE | PUD_TABLE_AF);
	}
	for (i = 0; i < PTRS_PER_PUD; i++)
		t->l1[i] = READ_ONCE(ll1[i]);
	for (i = 0; i < PTRS_PER_PGD; i++)
		t->l0[i] = READ_ONCE(p_swapper[i]);
	if (t->partial)
		t->l1[t->p_idx] = t->p_attr | __phys_to_pte_val(t->l2_boot_pa);
	t->linux_l1_pa = FKI_DESC_PA(e0);
	t->l0[t->i0] = FKI_DESC_ATTR(e0) | __phys_to_pte_val(t->l1_pa);
unlock:
	spin_unlock(&p_init_mm->page_table_lock);
	if (err)
		goto fail;
	t->cur_l2 = t->l2_boot;
	t->cur_l2_pa = t->l2_boot_pa;
	t->ttbr1 = phys_to_ttbr(t->l0_pa) & ~(TTBR_CNP_BIT | TTBR_ASID_MASK);
	dsb(ishst);
	mutex_lock(&fki_roots_lock);
	list_add(&t->node, &fki_roots);
	atomic_inc(&fki_nroots);
	mutex_unlock(&fki_roots_lock);
	pr_info("fki: private root %#llx for [%#lx, %#lx): L0[%u] -> own L1, foreign L1 [%u, %u)%s%s\n",
		(unsigned long long)t->l0_pa, lo, hi, t->i0, t->own_first, t->own_end, t->partial ? ", partial L1 " : "",
		t->partial ? "entry with own L2" : "");
	*errp = 0;
	return t;
fail:
	if (t->l2_boot)
		free_page((unsigned long)t->l2_boot);
	if (t->l1)
		free_page((unsigned long)t->l1);
	if (t->l0)
		free_page((unsigned long)t->l0);
	kfree(t);
fail0:
	*errp = err;
	return NULL;
}
EXPORT_SYMBOL(fki_ttbr1_create);

void fki_ttbr1_destroy(struct fki_ttbr1 *t)
{
	if (!t)
		return;
	if (WARN_ON(atomic_read(&t->cpus))) {
		pr_err("fki: private root %#llx still has CPUs attached: leaking it\n", (unsigned long long)t->l0_pa);
		return;
	}
	mutex_lock(&fki_roots_lock);
	list_del(&t->node);
	atomic_dec(&fki_nroots);
	mutex_unlock(&fki_roots_lock);
	flush_tlb_all();	/* the foreign kernel's global mappings and table walks, on every CPU that ran on the root */
	pr_info("fki: private root %#llx destroyed (lazy syncs %lu, audit fixes %lu, conflicts %lu)\n",
		(unsigned long long)t->l0_pa, t->syncs, t->audit_fixes, t->audit_conflicts);
	free_page((unsigned long)t->l0);
	free_page((unsigned long)t->l1);
	if (t->l2_boot)
		free_page((unsigned long)t->l2_boot);
	kfree(t);
}
EXPORT_SYMBOL(fki_ttbr1_destroy);

int fki_ttbr1_window_l1(struct fki_ttbr1 *t, unsigned long va, unsigned long long **l1_va, fki_u64 *l1_pa)
{
	if (!t || pgd_index(va) != t->i0 || va < (~0UL << VA_BITS))
		return -ENOENT;
	*l1_va = (unsigned long long *)t->l1;
	*l1_pa = t->l1_pa;
	return 0;
}
EXPORT_SYMBOL(fki_ttbr1_window_l1);

int fki_ttbr1_window_l2(struct fki_ttbr1 *t, unsigned long va, unsigned long long **l2_va, fki_u64 *l2_pa)
{
	if (!t || !t->partial || pgd_index(va) != t->i0 || pud_index(va) != t->p_idx)
		return -ENOENT;
	if (t->adopted)
		return -EBUSY;
	*l2_va = (unsigned long long *)t->l2_boot;
	*l2_pa = t->l2_boot_pa;
	return 0;
}
EXPORT_SYMBOL(fki_ttbr1_window_l2);

int fki_ttbr1_set(struct fki_ttbr1 *t, unsigned long long *slot, fki_u64 desc)
{
	unsigned long flags;
	u64 *s = (u64 *)slot, old;
	int ok;

	if (!t)
		return -EINVAL;
	ok = (s >= t->l1 + t->own_first && s < t->l1 + t->own_end) ||
	     (t->partial && !t->adopted && s >= t->l2_boot && s < t->l2_boot + t->p_from);
	if (!ok)
		return -EINVAL;
	raw_spin_lock_irqsave(&t->lock, flags);
	old = READ_ONCE(*s);
	if (old & 1) {
		fki_bbm_swap(s, desc);	/* a live entry replaced (the 4 KB bootstrap table by the foreign kernel's own) */
	} else {
		WRITE_ONCE(*s, desc);
		dsb(ishst);
		isb();
	}
	raw_spin_unlock_irqrestore(&t->lock, flags);
	return 0;
}
EXPORT_SYMBOL(fki_ttbr1_set);

int fki_ttbr1_adopt_l2(struct fki_ttbr1 *t, unsigned long long *l2_va, fki_u64 l2_pa)
{
	u64 *xl2 = (u64 *)l2_va, *ll2;
	unsigned long flags;
	unsigned int i;

	if (!t || !t->partial || !xl2 || (l2_pa & (PAGE_SIZE - 1)))
		return -EINVAL;
	if (t->adopted)
		return -EBUSY;
	raw_spin_lock_irqsave(&t->lock, flags);
	for (i = t->p_from; i < PTRS_PER_PMD; i++)
		if (READ_ONCE(xl2[i])) {
			raw_spin_unlock_irqrestore(&t->lock, flags);
			pr_err("fki: the foreign L2 table has entry %u (%#llx) above its range: not adopting\n", i,
			       (unsigned long long)READ_ONCE(xl2[i]));
			return -EBUSY;
		}
	ll2 = fki_linux_l2(t);
	for (i = t->p_from; i < PTRS_PER_PMD; i++) {
		u64 e = ll2 ? READ_ONCE(ll2[i]) : 0;

		if (!(e & 1))
			e = READ_ONCE(t->l2_boot[i]);	/* never lose an entry the root has */
		WRITE_ONCE(xl2[i], e);
	}
	dsb(ishst);
	fki_bbm_swap(&t->l1[t->p_idx], t->p_attr | __phys_to_pte_val(l2_pa));
	t->cur_l2 = xl2;
	t->cur_l2_pa = l2_pa;
	t->adopted = 1;
	raw_spin_unlock_irqrestore(&t->lock, flags);
	return 0;
}
EXPORT_SYMBOL(fki_ttbr1_adopt_l2);

unsigned long fki_ttbr1_syncs(struct fki_ttbr1 *t)
{
	return t ? READ_ONCE(t->syncs) : 0;
}
EXPORT_SYMBOL(fki_ttbr1_syncs);

/*
 * Lazy sync, from the fault probe on a CPU running on t (interrupts off): far is a Linux address the root has no entry
 * for. Copy what Linux has now along L0 -> L1 -> (the partial entry's L2 above hi), an empty entry of the root's only:
 * a valid entry is never overwritten.
 */
static int fki_ttbr1_sync(struct fki_ttbr1 *t, unsigned long far)
{
	unsigned int i0 = pgd_index(far), i1, j;
	unsigned long flags;
	u64 e, o, *ll1, *ll2;
	int fixed = 0;

	raw_spin_lock_irqsave(&t->lock, flags);
	e = READ_ONCE(p_swapper[i0]);
	if (i0 != t->i0) {
		o = READ_ONCE(t->l0[i0]);
		if (!(o & 1) && (e & 1)) {
			WRITE_ONCE(t->l0[i0], e);
			fixed = 1;
		}
		goto out;
	}
	ll1 = FKI_IS_TABLE(e) ? fki_table_va(FKI_DESC_PA(e)) : NULL;
	if (!ll1)
		goto out;
	i1 = pud_index(far);
	if (i1 >= t->own_first && i1 < t->own_end)
		goto out;
	if (t->partial && i1 == t->p_idx) {
		j = pmd_index(far);
		e = READ_ONCE(ll1[i1]);
		ll2 = FKI_IS_TABLE(e) ? fki_table_va(FKI_DESC_PA(e)) : NULL;
		if (j < t->p_from || !ll2)
			goto out;
		e = READ_ONCE(ll2[j]);
		o = READ_ONCE(t->cur_l2[j]);
		if (!(o & 1) && (e & 1)) {
			WRITE_ONCE(t->cur_l2[j], e);
			fixed = 1;
		}
		goto out;
	}
	e = READ_ONCE(ll1[i1]);
	o = READ_ONCE(t->l1[i1]);
	if (!(o & 1) && (e & 1)) {
		WRITE_ONCE(t->l1[i1], e);
		fixed = 1;
	}
out:
	if (fixed) {
		t->syncs++;
		dsb(ishst);
		isb();
	}
	raw_spin_unlock_irqrestore(&t->lock, flags);
	return fixed;
}
NOKPROBE_SYMBOL(fki_ttbr1_sync);

/* One entry of the root against Linux's: 0 same, 1 missing (copied in), 2 conflict */
static int fki_audit_one(struct fki_ttbr1 *t, u64 *ours, u64 linux_e, const char *lvl, unsigned int idx)
{
	u64 o = READ_ONCE(*ours);

	if (o == linux_e)
		return 0;
	if (!(o & 1) && (linux_e & 1)) {
		WRITE_ONCE(*ours, linux_e);
		t->audit_fixes++;
		pr_info("fki: root %#llx: %s entry %u missing, copied in (%#llx)\n", (unsigned long long)t->l0_pa, lvl, idx,
			(unsigned long long)linux_e);
		return 1;
	}
	t->audit_conflicts++;
	pr_alert("fki: root %#llx: %s entry %u is %#llx, Linux has %#llx\n", (unsigned long long)t->l0_pa, lvl, idx,
		 (unsigned long long)o, (unsigned long long)linux_e);
	return 2;
}

long fki_ttbr1_audit(struct fki_ttbr1 *t)
{
	unsigned long flags;
	u64 e0, *ll1, *ll2 = NULL;
	unsigned int i;
	long issues = 0;

	if (!t)
		return 0;
	raw_spin_lock_irqsave(&t->lock, flags);
	for (i = 0; i < PTRS_PER_PGD; i++)
		if (i != t->i0)
			issues += !!fki_audit_one(t, &t->l0[i], READ_ONCE(p_swapper[i]), "L0", i);
	e0 = READ_ONCE(p_swapper[t->i0]);
	if (!FKI_IS_TABLE(e0) || FKI_DESC_PA(e0) != t->linux_l1_pa) {
		pr_alert("fki: root %#llx: Linux's L0 entry %u changed (%#llx)\n", (unsigned long long)t->l0_pa, t->i0,
			 (unsigned long long)e0);
		t->audit_conflicts++;
		issues++;
		goto out;
	}
	ll1 = fki_table_va(t->linux_l1_pa);
	for (i = 0; ll1 && i < PTRS_PER_PUD; i++) {
		if (i >= t->own_first && i < t->own_end) {
			if (READ_ONCE(ll1[i])) {
				pr_alert("fki: root %#llx: Linux now maps L1 entry %u in the foreign range\n",
					 (unsigned long long)t->l0_pa, i);
				t->audit_conflicts++;
				issues++;
			}
			continue;
		}
		if (t->partial && i == t->p_idx)
			continue;
		issues += !!fki_audit_one(t, &t->l1[i], READ_ONCE(ll1[i]), "L1", i);
	}
	if (t->partial && ll1) {
		u64 ep = READ_ONCE(ll1[t->p_idx]);

		if ((ep ? FKI_DESC_PA(ep) : 0) != t->linux_l2_pa) {
			pr_alert("fki: root %#llx: Linux's L1 entry %u changed (%#llx)\n", (unsigned long long)t->l0_pa,
				 t->p_idx, (unsigned long long)ep);
			t->audit_conflicts++;
			issues++;
		} else {
			ll2 = fki_table_va(t->linux_l2_pa);
		}
		if (READ_ONCE(t->l1[t->p_idx]) != (t->p_attr | __phys_to_pte_val(t->cur_l2_pa))) {
			pr_alert("fki: root %#llx: its L1 entry %u is %#llx\n", (unsigned long long)t->l0_pa, t->p_idx,
				 (unsigned long long)READ_ONCE(t->l1[t->p_idx]));
			t->audit_conflicts++;
			issues++;
		}
		for (i = 0; ll2 && i < PTRS_PER_PMD; i++) {
			if (i < t->p_from) {
				if (READ_ONCE(ll2[i])) {
					pr_alert("fki: root %#llx: Linux now maps L2 entry %u in the foreign range\n",
						 (unsigned long long)t->l0_pa, i);
					t->audit_conflicts++;
					issues++;
				}
				continue;
			}
			issues += !!fki_audit_one(t, &t->cur_l2[i], READ_ONCE(ll2[i]), "L2", i);
		}
	}
out:
	dsb(ishst);
	raw_spin_unlock_irqrestore(&t->lock, flags);
	return issues;
}
EXPORT_SYMBOL(fki_ttbr1_audit);

/* While a private root exists, its copies of Linux's top levels must stay right: no memory hotplug, no hibernation */
static int fki_mem_notify(struct notifier_block *nb, unsigned long action, void *arg)
{
	if ((action == MEM_GOING_ONLINE || action == MEM_GOING_OFFLINE) && atomic_read(&fki_nroots)) {
		pr_warn("fki: memory hotplug refused while a foreign kernel has a private root\n");
		return NOTIFY_BAD;
	}
	return NOTIFY_OK;
}
static struct notifier_block fki_mem_nb = { .notifier_call = fki_mem_notify };
static int fki_mem_nb_ok;

static int fki_pm_notify(struct notifier_block *nb, unsigned long action, void *arg)
{
	if ((action == PM_HIBERNATION_PREPARE || action == PM_SUSPEND_PREPARE || action == PM_RESTORE_PREPARE) &&
	    atomic_read(&fki_nroots)) {
		pr_warn("fki: suspend/hibernation refused while a foreign kernel runs\n");
		return notifier_from_errno(-EBUSY);
	}
	return NOTIFY_OK;
}
static struct notifier_block fki_pm_nb = { .notifier_call = fki_pm_notify };
static int fki_pm_nb_ok;

/*
 * Letting Linux run (IRQs on, maybe a reschedule) from inside fki_enter() happens on whatever stack the foreign kernel
 * is on. Linux's exception entry takes a stack whose bit THREAD_SHIFT is set at sp for an overflow and panics, so
 * from a stack that is not this task's, the work runs on a stack of our own laid out like Linux's (the lower half of a
 * 2 * THREAD_SIZE aligned block, which the linear map gives an order-aligned page block). With a private root the
 * foreign stack is not even mapped once Linux's address space is back: the switch happens on this stack.
 */
static void *fki_alt_stack[NR_CPUS];	/* allocated at load: an order-3 atomic allocation can fail once memory is fragmented */
#define FKI_ALT_STACK_ORDER get_order(2 * THREAD_SIZE)

asm(
"	.pushsection .text\n"
"	.type	fki_run_on_stack, %function\n"
"fki_run_on_stack:\n"
"	hint	#34\n"
"	stp	x29, x30, [sp, #-16]!\n"
"	mov	x29, sp\n"
"	mov	x9, sp\n"
"	mov	sp, x2\n"
"	stp	x9, xzr, [sp, #-16]!\n"
"	mov	x9, x0\n"
"	mov	x0, x1\n"
"	blr	x9\n"
"	ldp	x9, xzr, [sp], #16\n"
"	mov	sp, x9\n"
"	ldp	x29, x30, [sp], #16\n"
"	ret\n"
"	.size	fki_run_on_stack, . - fki_run_on_stack\n"
"	.popsection\n");
void fki_run_on_stack(void (*fn)(void *), void *arg, void *top);	/* x0 = fn, x1 = arg, x2 = stack top */

static void fki_net_drain_freeq(void);

/*
 * Linux runs with SError (PSTATE.A) unmasked; the foreign kernel keeps D, A, I and F masked and asserts so in its
 * idle loop. Give it back exactly the exception mask it called with.
 */
struct fki_runs {
	void (*fn)(void *);
	void *arg;
	unsigned long size;
};

/*
 * On a Linux stack, interrupts off, the foreign address space still live: copy the arguments to this CPU's scratch area
 * (r and arg may be on the foreign stack), switch to Linux's address space, run fn (it turns interrupts on and off again),
 * then, before the foreign kernel resumes, fki_fpsimd_forget; switch back and copy the arguments back.
 */
static void fki_runs_body(void *p)
{
	struct fki_runs *r = p;
	struct fki_cpu *fc = &fki_cpus[smp_processor_id()];
	void *arg = r->arg, *a = arg;
	unsigned long size = r->size;
	int mm = fc->mm_active;

	WARN_ON_ONCE(fc->in_runs);
	fc->in_runs = 1;
	fc->run_fn = r->fn;
	if (size) {
		memcpy(fc->scratch, arg, size);
		a = fc->scratch;
	}
	if (mm)
		fki_mm_to_linux(fc);
	WARN_ON_ONCE((read_sysreg(ttbr1_el1) & FKI_TTBR_BADDR) != fki_swapper_pa);	/* T1, before interrupts go on */
	fc->run_fn(a);
	local_irq_enable();
	fki_net_drain_freeq();
	fki_fpsimd_forget();
	local_irq_disable();
	if (mm)
		fki_mm_to_foreign(fc);
	if (size)
		memcpy(arg, fc->scratch, size);
	fc->in_runs = 0;
}

static void fki_linux_runs_body(void (*fn)(void *), void *arg)
{
	unsigned long sp = current_stack_pointer;
	int cpu = smp_processor_id();

	if (on_task_stack(current, sp, 1)) {
		fn(arg);
		return;
	}
	fki_run_on_stack(fn, arg, (char *)fki_alt_stack[cpu] + THREAD_SIZE);
}

static void fki_linux_runs(void (*fn)(void *), void *arg, unsigned long size)
{
	unsigned long daif = read_sysreg(daif);
	struct fki_runs r = { fn, arg, size };

	if (WARN_ON_ONCE(size > FKI_SCRATCH))
		return;
	fki_linux_runs_body(fki_runs_body, &r);
	write_sysreg(daif, daif);
}
#define FKI_RUNS(fn, a) do { BUILD_BUG_ON(sizeof(*(a)) > FKI_SCRATCH); fki_linux_runs(fn, a, sizeof(*(a))); } while (0)

void fki_linux_runs_ext(void (*fn)(void *), void *arg, unsigned long size)
{
	fki_linux_runs(fn, arg, size);
}

struct fki_idle_args { fki_u64 ns; const volatile unsigned int *wake; };

static struct task_struct *fki_idle_task[NR_CPUS];	/* the task sleeping in fki_idle_until on each CPU, for wakeups */

static void fki_idle_body(void *p)
{
	struct fki_idle_args *a = p;
	ktime_t t = ns_to_ktime(a->ns);

	local_irq_enable();
	preempt_enable();
	set_current_state(TASK_UNINTERRUPTIBLE);
	WRITE_ONCE(fki_idle_task[smp_processor_id()], current);
	smp_mb();	/* pairs with fki_kick_cpu: it sees this task, or this sees the word it set first */
	if (!a->wake || !READ_ONCE(*a->wake))
		schedule_hrtimeout_range(&t, 50000, HRTIMER_MODE_REL);
	__set_current_state(TASK_RUNNING);
	WRITE_ONCE(fki_idle_task[smp_processor_id()], NULL);
	preempt_disable();
	local_irq_disable();
}

static void fki_yield_body(void *p)
{
	local_irq_enable();
	preempt_enable();
	cond_resched();
	preempt_disable();
	local_irq_disable();
}

void fki_idle_until(fki_u64 counter_deadline, fki_u64 max_ns, const volatile unsigned int *wake)
{
	u64 now = __arch_counter_get_cntvct(), freq = arch_timer_get_cntfrq(), ns;
	struct fki_idle_args a;

	if (counter_deadline <= now) {
		/* already due: still let Linux take its interrupts and reschedule (an idle loop that never sleeps starves it) */
		fki_linux_runs(fki_yield_body, NULL, 0);
		return;
	}
	ns = div64_u64((counter_deadline - now) * 1000000ULL, freq) * 1000ULL;
	if (ns > max_ns)
		ns = max_ns;
	a.ns = ns;
	a.wake = wake;	/* an address Linux maps: read once Linux's address space is back */
	FKI_RUNS(fki_idle_body, &a);
}
EXPORT_SYMBOL(fki_idle_until);

void fki_yield(void)
{
	fki_linux_runs(fki_yield_body, NULL, 0);
}
EXPORT_SYMBOL(fki_yield);

/*
 * Make the foreign kernel on a CPU look at what another CPU left for it: wake it if it sleeps in fki_idle_until,
 * otherwise interrupt that CPU (its user code runs with interrupts on; its kernel looks where it enables its own).
 * Callable with interrupts off.
 */
/*
 * One csd per target CPU, shared by every sender: smp_call_function_single_async's -EBUSY check is not atomic, so two
 * CPUs kicking the same one at once would both queue the same csd, and the doubly linked llist entry loops the target
 * forever in its IPI handler with interrupts off (three or more hosted CPUs: two can kick a third). A sender claims the
 * target's csd first; the target's handler releases it (an async csd is unlocked before its function runs).
 */
static atomic_t fki_kick_claimed[NR_CPUS];
static void fki_kick_fn(void *unused) { atomic_set_release(&fki_kick_claimed[smp_processor_id()], 0); }
static DEFINE_PER_CPU(call_single_data_t, fki_kick_csd);

void fki_kick_cpu(int cpu)
{
	struct task_struct *t;

	if (cpu < 0 || cpu >= nr_cpu_ids)
		return;
	smp_mb();	/* the caller's pending word before the idle task: pairs with fki_idle_body */
	t = READ_ONCE(fki_idle_task[cpu]);
	if (t)
		wake_up_process(t);
	else if (cpu != smp_processor_id() && atomic_cmpxchg_acquire(&fki_kick_claimed[cpu], 0, 1) == 0) {
		/* irq_work_queue_on is not exported; an IPI already on its way (claimed) is just as good */
		if (smp_call_function_single_async(cpu, per_cpu_ptr(&fki_kick_csd, cpu)))
			atomic_set_release(&fki_kick_claimed[cpu], 0);
	}
}
EXPORT_SYMBOL(fki_kick_cpu);

static fki_u64 *fki_mpidrs;
static u8 *fki_mpidr_ok;

static void fki_read_mpidr(void *p) { *(u64 *)p = read_cpuid_mpidr(); }

int fki_cpu_mpidr(int cpu, fki_u64 *mpidr)
{
	u64 v = 0;
	int err;

	if (cpu < 0 || cpu >= nr_cpu_ids || !cpu_online(cpu))
		return -EINVAL;
	if (fki_mpidr_ok && fki_mpidr_ok[cpu]) {
		*mpidr = fki_mpidrs[cpu];
		return 0;
	}
	err = smp_call_function_single(cpu, fki_read_mpidr, &v, 1);
	if (!err && fki_mpidr_ok) {
		fki_mpidrs[cpu] = v;
		fki_mpidr_ok[cpu] = 1;
	}
	*mpidr = v;
	return err;
}
EXPORT_SYMBOL(fki_cpu_mpidr);

int fki_num_cpus(void)
{
	return num_online_cpus();
}
EXPORT_SYMBOL(fki_num_cpus);

void fki_realtime(fki_u64 *sec, fki_u64 *nsec)
{
	struct timespec64 ts;

	ktime_get_real_ts64(&ts);
	*sec = ts.tv_sec;
	*nsec = ts.tv_nsec;
}
EXPORT_SYMBOL(fki_realtime);

/*
 * Virtual Ethernet links to foreign kernels: the Linux netdevs xnu0, xnu1, ... What Linux transmits on one is queued for
 * the foreign kernel that claimed it (fki_net_recv2); what that kernel sends (fki_net_send2) arrives on it as received
 * frames. All of nets=N are made at load, so their host configuration outlives each run.
 */
#define FKI_NET_QLEN 1024
#define FKI_MAX_NETS 8
#define FKI_TXBUF (ETH_FRAME_LEN + 64)
struct fki_net {
	struct net_device *dev;
	struct sk_buff_head rxq;	/* Linux -> foreign */
	int wake_cpu;			/* the foreign kernel's CPU that polls the link */
	void *owner;			/* non-NULL while a foreign kernel has the link */
	int index;
	bool runtime;			/* made by fki_net_create, not at load */
};
static int nets = 1;
module_param(nets, int, 0444);
static struct fki_net *fki_nets[FKI_MAX_NETS];
static DEFINE_MUTEX(fki_net_lock);
static struct sk_buff_head fki_net_freeq;	/* drained with IRQs enabled in fki_runs / TX / exit */

static void fki_net_drain_freeq(void)
{
	struct sk_buff *skb;

	while ((skb = skb_dequeue(&fki_net_freeq)) != NULL)
		dev_consume_skb_any(skb);
}

static netdev_tx_t fki_net_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct fki_net *net = netdev_priv(dev);
	struct task_struct *t;
	int cpu;

	if (!READ_ONCE(net->owner)) {		/* no foreign kernel on the link: nobody to deliver to */
		dev->stats.tx_carrier_errors++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}
	if (skb_queue_len(&net->rxq) >= FKI_NET_QLEN) {
		dev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}
	dev->stats.tx_packets++;
	dev->stats.tx_bytes += skb->len;
	skb_queue_tail(&net->rxq, skb);
	cpu = READ_ONCE(net->wake_cpu);
	t = cpu >= 0 && cpu < NR_CPUS ? READ_ONCE(fki_idle_task[cpu]) : NULL;
	if (t)
		wake_up_process(t);	/* the foreign kernel's polling CPU is idle: let it see the frame now */
	return NETDEV_TX_OK;
}

static int fki_net_up(struct net_device *dev) { netif_start_queue(dev); return 0; }
static int fki_net_down(struct net_device *dev) { netif_stop_queue(dev); return 0; }

static const struct net_device_ops fki_net_ops = {
	.ndo_open = fki_net_up,
	.ndo_stop = fki_net_down,
	.ndo_start_xmit = fki_net_xmit,
	.ndo_set_mac_address = eth_mac_addr,
	.ndo_validate_addr = eth_validate_addr,
};

static struct fki_net *fki_net_make(int index, const char *name, bool runtime)
{
	struct net_device *dev = alloc_etherdev(sizeof(struct fki_net));
	struct fki_net *net;
	int err;

	if (!dev)
		return NULL;
	net = netdev_priv(dev);
	net->dev = dev;
	net->index = index;
	net->runtime = runtime;
	net->wake_cpu = 0;
	skb_queue_head_init(&net->rxq);
	strscpy(dev->name, name, IFNAMSIZ);
	dev->netdev_ops = &fki_net_ops;
	eth_hw_addr_random(dev);
	dev->min_mtu = 68;
	dev->max_mtu = ETH_DATA_LEN;
	dev->features |= NETIF_F_SG;
	dev->hw_features |= NETIF_F_SG;
	err = register_netdev(dev);
	if (err) {
		free_netdev(dev);
		pr_warn("fki: no %s link (%d)\n", name, err);
		return NULL;
	}
	return net;
}

static void fki_net_free(struct fki_net *net)
{
	struct net_device *dev = net->dev;

	unregister_netdev(dev);
	skb_queue_purge(&net->rxq);
	free_netdev(dev);
}

struct fki_net *fki_net_create(const char *name, int wake_cpu)
{
	struct fki_net *net = NULL;
	int i, free_i = -1;

	if (!name || wake_cpu < 0 || wake_cpu >= nr_cpu_ids)
		return NULL;
	mutex_lock(&fki_net_lock);
	for (i = 0; i < FKI_MAX_NETS; i++) {
		if (fki_nets[i] && !strcmp(fki_nets[i]->dev->name, name)) {
			net = fki_nets[i];
			break;
		}
		if (!fki_nets[i] && free_i < 0)
			free_i = i;
	}
	if (!net && free_i >= 0) {
		rtnl_lock();
		i = __dev_get_by_name(&init_net, name) != NULL;
		rtnl_unlock();
		if (!i) {
			net = fki_net_make(free_i, name, true);
			fki_nets[free_i] = net;
		}
	}
	if (net && READ_ONCE(net->owner)) {
		pr_warn("fki: link %s is in use by another foreign kernel\n", name);
		net = NULL;
	} else if (net) {
		skb_queue_purge(&net->rxq);
		WRITE_ONCE(net->wake_cpu, wake_cpu);
		smp_store_release(&net->owner, net);
	}
	mutex_unlock(&fki_net_lock);
	return net;
}
EXPORT_SYMBOL(fki_net_create);

void fki_net_release(struct fki_net *net)
{
	if (!net)
		return;
	mutex_lock(&fki_net_lock);
	WRITE_ONCE(net->owner, NULL);
	synchronize_rcu();	/* a transmit or a receive in progress on it has finished */
	skb_queue_purge(&net->rxq);
	fki_net_drain_freeq();
	mutex_unlock(&fki_net_lock);
}
EXPORT_SYMBOL(fki_net_release);

void fki_net_destroy(struct fki_net *net)
{
	if (!net)
		return;
	fki_net_release(net);
	if (!net->runtime)
		return;		/* one of nets=N: stays until fki unloads */
	mutex_lock(&fki_net_lock);
	fki_nets[net->index] = NULL;
	mutex_unlock(&fki_net_lock);
	fki_net_free(net);
}
EXPORT_SYMBOL(fki_net_destroy);

struct fki_net_io { struct fki_net *net; const void *in; unsigned long len; long result; };

static void fki_net_send_body(void *p)
{
	struct fki_net_io *io = p;
	struct net_device *dev = io->net->dev;
	struct sk_buff *skb;

	io->result = -ENODEV;
	skb = netdev_alloc_skb(dev, io->len + NET_IP_ALIGN);
	if (!skb) {
		dev->stats.rx_dropped++;
		io->result = -ENOMEM;
		return;
	}
	skb_reserve(skb, NET_IP_ALIGN);
	skb_put_data(skb, io->in, io->len);	/* the CPU's transmit buffer (linear map), or v0: the frame itself */
	skb->protocol = eth_type_trans(skb, dev);
	dev->stats.rx_packets++;
	dev->stats.rx_bytes += io->len;
	local_irq_enable();	/* netif_rx re-enables bottom halves, which must not happen with interrupts off */
	fki_net_drain_freeq();
	netif_rx(skb);
	local_irq_disable();
	io->result = io->len;
}

/* The foreign kernel sends one Ethernet frame (header included, no FCS) out of its link: it arrives on the netdev. */
long fki_net_send2(struct fki_net *net, const void *frame, unsigned long len)
{
	struct fki_cpu *fc = &fki_cpus[smp_processor_id()];
	struct fki_net_io io = { .net = net, .in = frame, .len = len };

	if (!net || len < ETH_HLEN || len > ETH_FRAME_LEN + 4)
		return -ENODEV;
	if (fc->txbuf) {
		memcpy(fc->txbuf, frame, len);	/* the foreign address space is live: Linux sees only the copy */
		io.in = fc->txbuf;
	} else if (fc->ttbr1) {
		return -ENOMEM;
	}
	FKI_RUNS(fki_net_send_body, &io);
	return io.result;
}
EXPORT_SYMBOL(fki_net_send2);

/* The next frame Linux sent on the link into buf (at most len bytes): its length, 0 when none is waiting. */
long fki_net_recv2(struct fki_net *net, void *buf, unsigned long len)
{
	struct sk_buff *skb;
	long ret;

	if (!net || skb_queue_empty(&net->rxq))
		return 0;
	skb = skb_dequeue(&net->rxq);
	if (!skb)
		return 0;
	if (skb->len > len)
		ret = -EMSGSIZE;
	else if (skb_copy_bits(skb, 0, buf, skb->len) == 0)
		ret = skb->len;
	else
		ret = -EIO;
	skb_queue_tail(&fki_net_freeq, skb);
	return ret;
}
EXPORT_SYMBOL(fki_net_recv2);

/* Frames waiting for the foreign kernel. */
long fki_net_pending2(struct fki_net *net)
{
	return net ? skb_queue_len(&net->rxq) : 0;
}
EXPORT_SYMBOL(fki_net_pending2);

/* version 0: xnu0's, claimed by the first caller */
static struct fki_net *fki_net0(void)
{
	struct fki_net *net = fki_nets[0];

	if (net && !READ_ONCE(net->owner))
		cmpxchg(&net->owner, NULL, net);
	return net;
}

long fki_net_send(const void *frame, unsigned long len)
{
	return fki_net_send2(fki_net0(), frame, len);
}
EXPORT_SYMBOL(fki_net_send);

long fki_net_recv(void *buf, unsigned long len)
{
	return fki_net_recv2(fki_net0(), buf, len);
}
EXPORT_SYMBOL(fki_net_recv);

long fki_net_pending(void)
{
	return fki_net_pending2(fki_net0());
}
EXPORT_SYMBOL(fki_net_pending);

/* ---- the control directory (kobjects and sysfs are GPL-only: a foreign module asks fki) ---- */
static DEFINE_MUTEX(fki_ctl_lock);
static struct kobject *fki_ctl_kobj;
static long (*fki_ctl_wcb)(const char *buf, unsigned long len);
static long (*fki_ctl_rcb)(char *buf, unsigned long len);

static ssize_t fki_ctl_store(struct kobject *k, struct kobj_attribute *a, const char *buf, size_t n)
{
	long (*cb)(const char *, unsigned long) = READ_ONCE(fki_ctl_wcb);
	long r = cb ? cb(buf, n) : -ENODEV;

	return r < 0 ? r : (ssize_t)n;
}

static ssize_t fki_status_show(struct kobject *k, struct kobj_attribute *a, char *buf)
{
	long (*cb)(char *, unsigned long) = READ_ONCE(fki_ctl_rcb);
	long r = cb ? cb(buf, PAGE_SIZE) : 0;

	return r < 0 ? r : min_t(long, r, PAGE_SIZE);
}

static struct kobj_attribute fki_ctl_attr = __ATTR(ctl, 0200, NULL, fki_ctl_store);
static struct kobj_attribute fki_status_attr = __ATTR(status, 0444, fki_status_show, NULL);
static struct attribute *fki_ctl_attrs[] = { &fki_ctl_attr.attr, &fki_status_attr.attr, NULL };
static const struct attribute_group fki_ctl_group = { .attrs = fki_ctl_attrs };

int fki_ctl_register(const char *dir, long (*write_cb)(const char *buf, unsigned long len),
		     long (*read_cb)(char *buf, unsigned long len))
{
	struct kobject *k;
	int err;

	mutex_lock(&fki_ctl_lock);
	if (fki_ctl_kobj) {
		mutex_unlock(&fki_ctl_lock);
		return -EBUSY;
	}
	k = kobject_create_and_add(dir, kernel_kobj);
	if (!k) {
		mutex_unlock(&fki_ctl_lock);
		return -ENOMEM;
	}
	WRITE_ONCE(fki_ctl_wcb, write_cb);
	WRITE_ONCE(fki_ctl_rcb, read_cb);
	err = sysfs_create_group(k, &fki_ctl_group);
	if (err) {
		kobject_put(k);
		WRITE_ONCE(fki_ctl_wcb, NULL);
		WRITE_ONCE(fki_ctl_rcb, NULL);
	} else {
		fki_ctl_kobj = k;
	}
	mutex_unlock(&fki_ctl_lock);
	return err;
}
EXPORT_SYMBOL(fki_ctl_register);

void fki_ctl_unregister(void)
{
	mutex_lock(&fki_ctl_lock);
	if (fki_ctl_kobj) {
		sysfs_remove_group(fki_ctl_kobj, &fki_ctl_group);	/* waits for a ctl write or status read in progress */
		kobject_put(fki_ctl_kobj);
		fki_ctl_kobj = NULL;
	}
	WRITE_ONCE(fki_ctl_wcb, NULL);
	WRITE_ONCE(fki_ctl_rcb, NULL);
	mutex_unlock(&fki_ctl_lock);
}
EXPORT_SYMBOL(fki_ctl_unregister);

/* ---- per-CPU attach ---- */
static int fki_cpuhp_state = -1;

static int fki_cpu_down_prep(unsigned int cpu)
{
	if (fki_cpus && READ_ONCE(fki_cpus[cpu].attached)) {
		pr_warn("fki: CPU %u runs a foreign kernel: it stays online\n", cpu);
		return -EBUSY;
	}
	return 0;
}

int fki_cpu_attach(int cpu, const struct fki_cpu_cfg *cfg)
{
	struct fki_cpu *fc;
	struct page *pg;
	unsigned int order;
	int err = 0;

	if (!cfg || cpu < 0 || cpu >= nr_cpu_ids || !cpu_online(cpu))
		return -EINVAL;
	if (cfg->fault && !fki_fault_registered)
		return -ENOENT;
	mutex_lock(&fki_attach_lock);
	fc = &fki_cpus[cpu];
	if (fki_fault_legacy || fc->attached) {
		err = -EBUSY;
		goto out;
	}
	order = get_order(SZ_1M);
	pg = alloc_pages(GFP_KERNEL | __GFP_NOWARN, order);
	if (!pg) {
		order = get_order(SZ_256K);
		pg = alloc_pages(GFP_KERNEL, order);
	}
	fc->txbuf = kmalloc(FKI_TXBUF, GFP_KERNEL);
	if (!pg || !fc->txbuf) {
		if (pg)
			__free_pages(pg, order);
		kfree(fc->txbuf);
		fc->txbuf = NULL;
		err = -ENOMEM;
		goto out;
	}
	fc->bounce = page_address(pg);
	fc->bounce_order = order;
	fc->bounce_size = PAGE_SIZE << order;
	if (cfg->ttbr1)
		atomic_inc(&cfg->ttbr1->cpus);
	fc->fault_lo = cfg->fault_lo;
	fc->fault_hi = cfg->fault_hi;
	fc->fault_ctx = cfg->fault_ctx;
	WRITE_ONCE(fc->ttbr1, cfg->ttbr1);
	smp_store_release(&fc->fault, cfg->fault);
	WRITE_ONCE(fc->attached, 1);
	fki_cpus_attached++;
out:
	mutex_unlock(&fki_attach_lock);
	return err;
}
EXPORT_SYMBOL(fki_cpu_attach);

void fki_cpu_detach(int cpu)
{
	struct fki_cpu *fc;
	struct fki_ttbr1 *t;

	if (cpu < 0 || cpu >= nr_cpu_ids)
		return;
	mutex_lock(&fki_attach_lock);
	fc = &fki_cpus[cpu];
	if (!fc->attached) {
		mutex_unlock(&fki_attach_lock);
		return;
	}
	if (fc->mm_active) {
		WARN_ON(1);
		mutex_unlock(&fki_attach_lock);
		return;
	}
	t = fc->ttbr1;
	WRITE_ONCE(fc->fault, NULL);
	WRITE_ONCE(fc->ttbr1, NULL);
	synchronize_rcu();	/* a fault handler running now on that CPU has returned */
	free_pages((unsigned long)fc->bounce, fc->bounce_order);
	fc->bounce = NULL;
	fc->bounce_size = 0;
	kfree(fc->txbuf);
	fc->txbuf = NULL;
	if (t)
		atomic_dec(&t->cpus);
	WRITE_ONCE(fc->attached, 0);
	fki_cpus_attached--;
	mutex_unlock(&fki_attach_lock);
}
EXPORT_SYMBOL(fki_cpu_detach);

static void fki_free_alt_stacks(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		if (fki_alt_stack[cpu])
			free_pages((unsigned long)fki_alt_stack[cpu], FKI_ALT_STACK_ORDER);
		fki_alt_stack[cpu] = NULL;
	}
}

/* Whether this host can give foreign kernels private roots: checked once, at load */
static void fki_roots_check(void)
{
	u64 ttbr1 = read_sysreg(ttbr1_el1);

	fki_roots_ok = 1;
	if ((ttbr1 & FKI_TTBR_BADDR) != fki_swapper_pa) {
		pr_warn("fki: TTBR1 %#llx is not swapper_pg_dir (%#llx): no private roots\n", ttbr1,
			(unsigned long long)fki_swapper_pa);
		fki_roots_ok = 0;
	}
	if (!p_text || p_text != KIMAGE_VADDR) {
		pr_warn("fki: the kernel image is not at its link address (KASLR; boot with nokaslr): no private roots\n");
		fki_roots_ok = 0;
	}
	if (system_uses_ttbr0_pan()) {
		pr_warn("fki: software TTBR0 PAN is in use: no private roots\n");
		fki_roots_ok = 0;
	}
	if (arm64_kernel_unmapped_at_el0()) {
		pr_warn("fki: KPTI is in use: no private roots\n");
		fki_roots_ok = 0;
	}
	if (system_uses_irq_prio_masking()) {
		pr_warn("fki: GICv3 pseudo-NMI is in use: no private roots\n");
		fki_roots_ok = 0;
	}
}

static int __init fki_init(void)
{
	int err, cpu, i;

	err = fki_resolve();
	if (err) {
		pr_err("fki: kernel symbols missing (%d)\n", err);
		return err;
	}
	fki_roots_check();
	fki_mpidrs = kcalloc(nr_cpu_ids, sizeof(*fki_mpidrs), GFP_KERNEL);
	fki_mpidr_ok = kcalloc(nr_cpu_ids, sizeof(*fki_mpidr_ok), GFP_KERNEL);
	if (!fki_mpidrs || !fki_mpidr_ok) {
		kfree(fki_mpidrs); fki_mpidrs = NULL;
		kfree(fki_mpidr_ok); fki_mpidr_ok = NULL;
		return -ENOMEM;
	}
	for_each_online_cpu(cpu) {
		u64 v = 0;
		if (!smp_call_function_single(cpu, fki_read_mpidr, &v, 1)) {
			fki_mpidrs[cpu] = v;
			fki_mpidr_ok[cpu] = 1;
		}
	}
	fki_cpus = kcalloc(nr_cpu_ids, sizeof(*fki_cpus), GFP_KERNEL);
	if (!fki_cpus) {
		kfree(fki_mpidrs); fki_mpidrs = NULL;
		kfree(fki_mpidr_ok); fki_mpidr_ok = NULL;
		return -ENOMEM;
	}
	BUILD_BUG_ON(sizeof(((struct fki_cpu *)0)->scratch) > 128);
	for_each_possible_cpu(cpu) {
		struct page *pg = alloc_pages(GFP_KERNEL, FKI_ALT_STACK_ORDER);

		if (!pg) {
			fki_free_alt_stacks();
			kfree(fki_cpus);
			return -ENOMEM;
		}
		fki_alt_stack[cpu] = page_address(pg);
		INIT_CSD(per_cpu_ptr(&fki_kick_csd, cpu), fki_kick_fn, NULL);
	}
	fki_empty_pa = 0;
	{
		void *p = (void *)get_zeroed_page(GFP_KERNEL);

		if (!p) {
			fki_free_alt_stacks();
			kfree(fki_cpus);
			return -ENOMEM;
		}
		fki_empty_pa = virt_to_phys(p);
	}
	skb_queue_head_init(&fki_net_freeq);
	fki_carve_init();
	fki_reserve();
	err = fki_hostcall_init();
	if (err)
		pr_warn("fki: no /dev/xnu-hostcall (%d)\n", err);
	if (nets < 0)
		nets = 0;
	if (nets > FKI_MAX_NETS)
		nets = FKI_MAX_NETS;
	for (i = 0; i < nets; i++) {
		char name[IFNAMSIZ];

		snprintf(name, sizeof(name), "xnu%d", i);
		fki_nets[i] = fki_net_make(i, name, false);
	}
	fki_fault_registered = register_kprobe(&fki_fault_kp) == 0;
	if (!fki_fault_registered)
		pr_warn("fki: no __do_kernel_fault probe: foreign kernel faults oops\n");
	fki_die_registered = register_kprobe(&fki_die_kp) == 0;	/* best effort */
	err = cpuhp_setup_state_nocalls(CPUHP_AP_ONLINE_DYN, "fki/cpu:online", NULL, fki_cpu_down_prep);
	fki_cpuhp_state = err < 0 ? -1 : err;
	fki_mem_nb_ok = register_memory_notifier(&fki_mem_nb) == 0;
	fki_pm_nb_ok = register_pm_notifier(&fki_pm_nb) == 0;
	pr_info("fki: version %d, %d link%s, private roots %s, %lu KB pages\n", FKI_VERSION, nets, nets == 1 ? "" : "s",
		fki_roots_ok ? "available" : "NOT available", PAGE_SIZE >> 10);
	return 0;
}

static void __exit fki_exit(void)
{
	int i;

	fki_ctl_unregister();
	if (fki_l2_adopted_va) {	/* a foreign module left without handing the shared L1 entry back */
		spin_lock(&p_init_mm->page_table_lock);
		if (fki_l2_adopted_va)
			fki_window_l2_restore_locked(fki_l2_adopted_va, NULL, fki_l2_adopted_old, fki_l2_adopted_lo,
						     fki_l2_adopted_hi);
		spin_unlock(&p_init_mm->page_table_lock);
	}
	if (fki_pm_nb_ok)
		unregister_pm_notifier(&fki_pm_nb);
	if (fki_mem_nb_ok)
		unregister_memory_notifier(&fki_mem_nb);
	if (fki_cpuhp_state >= 0)
		cpuhp_remove_state_nocalls(fki_cpuhp_state);
	if (fki_die_registered)
		unregister_kprobe(&fki_die_kp);
	if (fki_fault_registered)
		unregister_kprobe(&fki_fault_kp);
	fki_hostcall_exit();
	if (resv_pg && !resv_used)
		p_cma_release(*p_cma_area, resv_pg, resv_cnt);
	if (carve_va)
		memunmap(carve_va);
	for (i = 0; i < FKI_MAX_NETS; i++)
		if (fki_nets[i]) {
			fki_net_free(fki_nets[i]);
			fki_nets[i] = NULL;
		}
	skb_queue_purge(&fki_net_freeq);
	if (fki_empty_pa)
		free_page((unsigned long)phys_to_virt(fki_empty_pa));
	fki_free_alt_stacks();
	kfree(fki_cpus);
	fki_cpus = NULL;
	kfree(fki_mpidrs);
	fki_mpidrs = NULL;
	kfree(fki_mpidr_ok);
	fki_mpidr_ok = NULL;
}
module_init(fki_init);
module_exit(fki_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Foreign kernel interface v1 (out-of-tree form for stock kernels)");

void *fki_file_open(const char *path, int writable, fki_u64 *size)
{
	struct file *f = filp_open(path, (writable ? O_RDWR : O_RDONLY) | O_LARGEFILE, 0);

	if (IS_ERR(f))
		return NULL;
	if (size)	/* the mapping's host: a block device's own size (a device node's inode says 0) */
		*size = i_size_read(f->f_mapping->host);
	return f;
}
EXPORT_SYMBOL(fki_file_open);

fki_u64 fki_file_size(void *file)
{
	struct file *f = file;

	return f ? i_size_read(f->f_mapping->host) : 0;
}
EXPORT_SYMBOL(fki_file_size);

#define FKI_FILE_CHUNK (256UL << 10)

struct fki_file_args { void *file; int write; void *buf; unsigned long len; fki_u64 off; long ret; };

/* In Linux: len bytes between the file and buf, a buffer Linux maps (a CPU's bounce buffer, a Linux alias) */
static void fki_file_io_body(void *p)
{
	struct fki_file_args *a = p;
	unsigned long done = 0;
	long ret = 0;
	loff_t pos = a->off;

	local_irq_enable();
	preempt_enable();
	while (done < a->len) {
		unsigned long n = min(a->len - done, FKI_FILE_CHUNK);
		ssize_t r = a->write ? kernel_write(a->file, (char *)a->buf + done, n, &pos)
		                     : kernel_read(a->file, (char *)a->buf + done, n, &pos);
		if (r < 0) {
			ret = r;
			break;
		}
		done += r;
		if ((unsigned long)r < n)
			break;
	}
	preempt_disable();
	local_irq_disable();
	a->ret = ret < 0 && !done ? ret : (long)done;
}

/* version 0's form, for a CPU without bounce buffers (no private root: the foreign memory stays mapped in Linux's) */
static void fki_file_rw_v0_body(void *p)
{
	struct fki_file_args *a = p;
	unsigned long done = 0;
	long ret = 0;
	void *bounce;

	local_irq_enable();
	preempt_enable();
	bounce = kvmalloc(min(a->len, FKI_FILE_CHUNK), GFP_KERNEL);
	if (!bounce)
		ret = -ENOMEM;
	while (bounce && done < a->len) {
		unsigned long n = min(a->len - done, FKI_FILE_CHUNK);
		loff_t pos = a->off + done;
		ssize_t r;

		if (a->write) {
			preempt_disable(); local_irq_disable();
			memcpy(bounce, (char *)a->buf + done, n);
			local_irq_enable(); preempt_enable();
			r = kernel_write(a->file, bounce, n, &pos);
		} else {
			r = kernel_read(a->file, bounce, n, &pos);
			if (r > 0) {
				preempt_disable(); local_irq_disable();
				memcpy((char *)a->buf + done, bounce, r);
				local_irq_enable(); preempt_enable();
			}
		}
		if (r < 0) {
			ret = r;
			break;
		}
		done += r;
		if ((unsigned long)r < n)
			break;
	}
	kvfree(bounce);
	preempt_disable();
	local_irq_disable();
	a->ret = ret < 0 && !done ? ret : (long)done;
}

/*
 * From inside fki_enter(): buf is the foreign kernel's. It is copied to or from this CPU's bounce buffer while the foreign
 * address space is live; Linux does the I/O on the bounce buffer, a chunk at a time.
 */
long fki_file_rw(void *file, int write, void *buf, unsigned long len, fki_u64 off)
{
	struct fki_cpu *fc = &fki_cpus[smp_processor_id()];
	unsigned long done = 0;
	long ret = 0;

	if (!file)
		return -EBADF;
	if (!fc->bounce) {
		struct fki_file_args a = { file, write, buf, len, off, -EIO };

		if (fc->ttbr1)
			return -ENOMEM;
		FKI_RUNS(fki_file_rw_v0_body, &a);
		return a.ret;
	}
	while (done < len) {
		unsigned long n = min(len - done, fc->bounce_size);
		struct fki_file_args a = { file, write, fc->bounce, n, off + done, -EIO };

		if (write)
			memcpy(fc->bounce, (char *)buf + done, n);
		FKI_RUNS(fki_file_io_body, &a);
		if (a.ret < 0) {
			ret = a.ret;
			break;
		}
		if (!write && a.ret > 0)
			memcpy((char *)buf + done, fc->bounce, a.ret);
		done += a.ret;
		if ((unsigned long)a.ret < n)
			break;
	}
	return ret < 0 && !done ? ret : (long)done;
}
EXPORT_SYMBOL(fki_file_rw);

/* kva: memory Linux maps (a foreign kernel's DRAM through its Linux alias), no copy */
long fki_file_rw_direct(void *file, int write, void *kva, unsigned long len, fki_u64 off)
{
	struct fki_file_args a = { file, write, kva, len, off, -EIO };

	if (!file)
		return -EBADF;
	FKI_RUNS(fki_file_io_body, &a);
	return a.ret;
}
EXPORT_SYMBOL(fki_file_rw_direct);

long fki_file_pwrite(void *file, const void *buf, unsigned long len, fki_u64 off)
{
	loff_t pos = off;

	if (!file)
		return -EBADF;
	return kernel_write(file, buf, len, &pos);
}
EXPORT_SYMBOL(fki_file_pwrite);

void fki_file_close(void *file)
{
	if (file)
		filp_close(file, NULL);
}
EXPORT_SYMBOL(fki_file_close);
