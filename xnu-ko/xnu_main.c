// SPDX-License-Identifier: APSL-2.0
/*
 * xnu.ko: hosts XNU kernel images inside Linux, several at once (guests), each on CPUs of its own, in DRAM of its own and
 * in a kernel address space of its own (a private TTBR1 root: every guest links at the same addresses). Talks to Linux
 * only through FKI (fki.h); <linux/module.h> provides the module ABI.
 *
 * mode=1: load, slide and sign the image, call two XNU functions (a smoke test).
 * mode=2: a bare kernel image; mode=3: a kernel collection. Hand XNU boot args and an ADT and run arm_init in the XNU
 *        world; XNU calls into this module through linux64_hook (the interface: linux64_abi.h). insmod runs guest 0
 *        from the module parameters and returns when its run ends (as before guests existed).
 * mode=0: start nothing; guests start, stop and go through /sys/kernel/xnu/ctl (start, stop, dump, reap), their state
 *        in /sys/kernel/xnu/status. host_cpus= are never given to a guest.
 *
 * A guest's state is its struct xnu_guest; per CPU (by MPIDR slot, 0..255, unique across guests) there is the world save
 * area world.S finds, the guest the slot belongs to and the slot's counters. Host threads (monitor, framebuffer) and the
 * set-up code reach a guest's memory through its Linux alias (the DRAM slice as Linux maps it), never through XNU's own
 * addresses, which only the guest's private root maps.
 */
#include <linux/module.h>
#include "../fki-ko/fki.h"
#include "xrt.h"
#include "macho.h"
#include "adt.h"
#include "world.h"
#include "../xnu-patches/include/linux64_abi.h"

static char *image = "xnu/kernel.macho";
module_param(image, charp, 0444);
static int mode = 2;
module_param(mode, int, 0444);
static int ram_mb = 256;
module_param(ram_mb, int, 0444);
static int kmax;	/* end the run after this many XNU kprintf lines (0: no limit) */
module_param(kmax, int, 0444);
static int idle_max;	/* end the run after this many XNU idle rounds (0: never) */
module_param(idle_max, int, 0444);
static int quiet_console = 1;	/* drop the known high-volume console and kprintf lines (see quiet_line) */
module_param(quiet_console, int, 0444);
static int quiet_line(const char *l, unsigned n)
{
	static const char *const pfx[] = { "com.apple.xpc.launchd|", "kmutil: AppleImage4", "cryptexd: AppleImage4", "ACMTRM", "ACM:",
		"AppleCredentialManagerUserClient", "hfs: set hardlink bit", "Couldn't alloc class", "  dynamic: platformVersion" };
	for (unsigned i = 0; i < sizeof(pfx) / sizeof(pfx[0]); i++) {
		unsigned k = 0;
		while (pfx[i][k] && k < n && l[k] == pfx[i][k]) k++;
		if (!pfx[i][k]) return 1;
	}
	for (unsigned k = 0; k + 18 <= n; k++)	/* "... triggered unnest of range ..." */
		if (!x_strncmp(l + k, "triggered unnest", 16)) return 1;
	return 0;
}
/* guest 0 under insmod mode=1/2/3: XNU's CPUs are Linux CPUs 0 .. cpus-1 (XNU's CPU 0 is Linux CPU 0, where insmod runs) */
#define XNU_MAX_CPUS 16
#define XNU_MAX_GUESTS 4
static int cpus = 1;
module_param(cpus, int, 0444);
static inline unsigned this_slot(void) { unsigned long m; __asm__ volatile("mrs %0, mpidr_el1" : "=r"(m)); return LINUX64_CPU_SLOT(m); }
/* Writing 1 ends guest 0's insmod run at XNU's next call into the host; insmod returns and the module can be removed */
static int stop_run;
module_param(stop_run, int, 0644);
static int assert_fatal;	/* 1: a failed XNU assert() ends the run; 0: report it and let XNU continue */
module_param(assert_fatal, int, 0444);
/* Extra XNU boot arguments, appended to the built-in command line */
static char *bootargs;
module_param(bootargs, charp, 0444);
/* A root RAM disk (firmware blob): placed in XNU's DRAM after the ADT, /chosen/memory-map RAMDisk, boot-arg rd=md0. */
static char *ramdisk;
module_param(ramdisk, charp, 0444);
/* The static trust cache a bootloader passes (raw trust cache module, e.g. the payload of the OS's StaticTrustCache.img4) */
static char *trustcache;
module_param(trustcache, charp, 0444);
/*
 * The display's framebuffer (boot_args Video, 32 bpp): reserved in the boot data like the RAM disk, so XNU never hands its pages
 * out; XnuDisplay (kexts/XnuDisplay) copies each frame WindowServer swaps into it. fb_width=0 boots without one. A host thread
 * writes it to fb_dump (raw BGRA, fb_width x fb_height) every fb_dump_ms milliseconds when both are set.
 */
static int fb_width = 1280, fb_height = 800;
module_param(fb_width, int, 0444);
module_param(fb_height, int, 0444);
static char *fb_dump;
module_param(fb_dump, charp, 0444);
static int fb_dump_ms = 1000;
module_param(fb_dump_ms, int, 0444);
/* A root disk backed by a host file (an HFS+ image): XNU's memdev md0 in host mode (LINUX64_CALL_RAMDISK_SIZE, _RW). */
static char *hostdisk;
static int hostdisk_rw;
module_param(hostdisk, charp, 0444);
module_param(hostdisk_rw, int, 0444);
/* Console input for XNU's /dev/console: bytes appended to this host file are typed in (LINUX64_CALL_CONSOLE_READ) */
static char *console_in;
module_param(console_in, charp, 0444);
/* The guest's own log (its xnu: lines, kprintf, console output): appended to this existing host file by its monitor */
static char *console_log;
module_param(console_log, charp, 0444);
/*
 * Input events for XnuInput (kexts/XnuInput): a host file of 8-byte records {u16 type, u16 code, s32 value} as Linux's evdev
 * (tools/hosted/xnu-input.py appends them); LINUX64_CALL_INPUT_READ reads the whole records written since the load.
 */
static char *input_in;
module_param(input_in, charp, 0444);
static char *keystore;		/* the hosted keystore's file (XnuEAN's EAN blobs): read and written through LINUX64_CALL_KEYSTORE_RW */
module_param(keystore, charp, 0444);
/* The hosted system's block disk (kexts/XnuDisk: IOKit media, e.g. an APFS container), read-write (LINUX64_CALL_DISK_SIZE, _RW) */
static char *disk;
module_param(disk, charp, 0444);
/*
 * What iBoot tells an APFS boot in /chosen: the system volume's UUID (boot-uuid; boot-arg rd=uuid finds the root by it) and
 * the volume group's folder in Preboot (apfs-preboot-uuid: kern.apfsprebootuuid, from which dyld's libignition grafts the
 * boot cryptexes, the OS cryptex with the dyld shared cache, before launchd runs).
 */
static char *boot_uuid, *preboot_uuid;
module_param(boot_uuid, charp, 0444);
module_param(preboot_uuid, charp, 0444);
/*
 * root_uuid: the root volume itself, by its APFS volume UUID, as /chosen root-matching (IOFindBSDRoot takes it before
 * boot-uuid): BSD disk numbers follow probe order and change from boot to boot, and the boot-uuid route waits on APFS to
 * publish boot-uuid-media from its volume group tree, which a disk that never booted on a Mac does not have yet.
 */
static char *root_uuid;
module_param(root_uuid, charp, 0444);
/*
 * The machine's model (the device tree root's "model", hw.model): user space takes the product type from it
 * (MobileGestalt ProductType), and apsd aborts without one, which holds up login (loginwindow's MDM pre-login check-in
 * waits on apsd). It must have the form of a Mac model identifier.
 */
static char *model = "VirtualMac2,1";	/* Apple's own virtual Mac's model identifier */
module_param(model, charp, 0444);
/* vm16k=1 (the default): XNU's VM pages are 16 KB, as on a Mac, over the host's 4 KB translation granule (XNU's
 * PAGE_RATIO 4): a quarter of the page faults of 4 KB pages; vm16k=0 gives 4 KB VM pages */
static int vm16k = 1;
module_param(vm16k, int, 0444);
static int park_secondaries;	/* diagnostics: 1 keeps secondary CPUs out of XNU */
module_param(park_secondaries, int, 0444);
static int perf;
module_param(perf, int, 0644);
static int monitor_cpu = -2;	/* -2: the first Linux CPU XNU does not run on; -1: no monitor */
module_param(monitor_cpu, int, 0444);
/* writing 1 asks guest 0's XNU for a dump of its threads at its next idle (/sys/module/xnu/parameters/dump_threads) */
static int dump_threads;
module_param(dump_threads, int, 0644);
/* the guest's NIC: its MAC is 02:58:4e:55:00:(01 + mac_id) (boot-arg xnu-mac-id, XnuEthernet), its host link net */
static int mac_id;
module_param(mac_id, int, 0444);
static char *net = "xnu0";
module_param(net, charp, 0444);
/* Linux CPUs no guest may have (mode=0: the host's own, e.g. host_cpus=0,7) */
static char *host_cpus;
module_param(host_cpus, charp, 0444);

#define XNU_VA_LO 0xffffffe000000000ULL
#define XNU_VA_HI 0xfffffffc00000000ULL
#define XNU_FAKE_PHYS 0x800000000ULL	/* XNU's view of the region's first byte (mode 2) */
#define BOOT_LINE 1024
#define DESC_PA(d, sz) ((d) & 0x0000ffffffffffffULL & ~((sz) - 1))

/*
 * The host's translation geometry (fki_page_shift): 4 KB granule s = 12, 16 KB s = 14. A table has g_n = 1 << (s - 3) entries;
 * an L2 entry maps g_block = 1 << g_l2s bytes (2 MiB, 32 MiB), an L1 entry 1 << g_l1s (1 GiB, 64 GiB). Set once at load.
 */
static unsigned g_s, g_n, g_l2s, g_l1s;
static u64_t g_page, g_block;
/* XNU's kernel VA range in L1 entries: VM_MIN_KERNEL_ADDRESS 0xffffffe000000000 .. VM_MAX_KERNEL_ADDRESS 0xfffffffbffffffff
 * (4 KB: 384..495; 16 KB: 2046..2047) */
static unsigned xnu_l1_first, xnu_l1_last;
/*
 * A guest's private root covers [root_lo, XNU_VA_HI): 4 KB, L1 entries [384, 496) of the last L0 entry, the guest's whole;
 * 16 KB, [0xfffffff000000000, ...): one 64 GiB L1 entry it shares with Linux's PCI I/O and fixmap, whose L2 entries
 * [0, 1536) are the guest's (p_idx, the root's partial entry). w_first .. w_end: the L1 entries the guest has whole.
 */
static u64_t root_lo;
static unsigned w_first, w_end, p_idx;
static int p_valid;

struct boot_args_v2 {			/* pexpert/arm64/boot.h layout */
	unsigned short Revision, Version;
	u64_t virtBase, physBase, memSize, topOfKernelData;
	struct { u64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth; } Video;
	unsigned int machineType;
	void *deviceTreeP;
	unsigned int deviceTreeLength;
	char CommandLine[BOOT_LINE];
	u64_t bootFlags, memSizeActual;
};

/* the compressor's limit is in VM pages: 6 GB either way */
static const char *cmdline = "debug=0x8 -v -noprogress serial=0 startup_debug=1";
static const char *cmdline_4k = " vm_compression_limit=1572864";
static const char *cmdline_16k = " vm_compression_limit=393216 linux64_vm16k=1";

/*
 * Where the time goes: host-counter ticks spent in each kind of call into the host (the rest of a CPU's time is XNU
 * running, user or kernel). The monitor prints the share of each over its last interval (perf=1).
 */
#define ACCT_N 64
/*
 * Each CPU's own state, one 128-byte aligned block per CPU slot (written on every upcall: with plain per-slot arrays the
 * CPUs shared cache lines and took turns at them): in XNU or in the host, the upcall count and last call (the monitor's
 * silent-CPU check), the deferred kernel fault, the fault-repeat check, disk bytes and the time per call kind (perf=1).
 */
struct xnu_slot {
	volatile int in_xnu;
	volatile int in_call;
	unsigned long upcalls, last_call;
	struct { unsigned long far, esr, pc, pstate; } deferred;
	unsigned long last_far, repeats;
	unsigned long acct_disk;
	unsigned long acct_ticks[ACCT_N], acct_calls[ACCT_N];
} __attribute__((aligned(128)));

enum { XG_FREE, XG_STARTING, XG_RUNNING, XG_STOPPING, XG_STOPPED, XG_ZOMBIE };
static const char *const xg_state_name[] = { "free", "starting", "running", "stopping", "stopped", "zombie" };
#define CFG_STR 160
struct xg_cfg {			/* a guest's configuration: guest 0's module parameters, or a ctl start line */
	char image[CFG_STR], disk[CFG_STR], keystore[CFG_STR], console_in[CFG_STR], console_log[CFG_STR], input_in[CFG_STR];
	char fb_dump[CFG_STR], hostdisk[CFG_STR], ramdisk[CFG_STR], trustcache[CFG_STR];
	char model[64], net[16];
	char root_uuid[40], boot_uuid[40], preboot_uuid[40];
	char bootargs[BOOT_LINE];
	int ram_mb, mac_id, fb_width, fb_height, fb_dump_ms, hostdisk_rw, vm16k, kmax, idle_max, quiet_console, assert_fatal;
	int park_secondaries, monitor_cpu, perf, test_boot_mismatch;
	int ncpus, cpu[XNU_MAX_CPUS];
};
struct xnu_guest;
struct xg_cpu {
	struct xnu_world world;		/* world.S's save area for this CPU (xnu_worlds[slot]) */
	struct xnu_guest *g;
	int idx, linux_cpu;
	unsigned slot;
	unsigned long long mpidr;	/* Aff1:Aff0: what XNU compares */
	void *thread;			/* a secondary CPU's host thread */
	volatile int running;
	volatile unsigned long start_arg;
	unsigned long seen, still;	/* the monitor's silent-CPU check */
};
#define XG_RING (64 * 1024)
struct xnu_guest {
	int id;
	volatile int state;
	int legacy;			/* guest 0 of an insmod mode=1/2/3 run */
	int mode;
	volatile int stop;
	volatile int dump_req;
	int result;
	int holds_ref;
	struct xg_cfg cfg;
	int ncpus;
	struct xg_cpu cpu[XNU_MAX_CPUS];
	void *boot_thread, *monitor, *fb_thread;
	int monitor_cpu;
	char mon_name[16], fb_name[16], boot_name[16];
	struct xnu_image img;
	void *blob, *rd_blob, *tc_blob;
	const void *rd_data, *tc_data;
	unsigned long rd_size, tc_size;
	void *hd_file, *ks_file, *dk_file, *ci_file, *in_file, *log_file;
	fki_u64 hd_size, dk_size, ci_off, in_off, log_off;
	struct fki_net *net;
	void *hc;
	struct fki_ttbr1 *ttbr1;
	unsigned long long *root_l1;	/* the root's own L1 table for the window */
	int attached[XNU_MAX_CPUS];
	struct fki_region l2page;	/* 4 KB: the bootstrap L2 table */
	u64_t window_base;		/* the bootstrap window: L2-block aligned, at or below the image's vmlow (an XNU address) */
	u64_t linear_size;		/* portion of the slice currently walked as a linear alias */
	u64_t static_size;		/* static region up to topOfKernelData (before XNU's own page tables take over) */
	u64_t alias_off;		/* Linux alias = XNU address + alias_off, inside the window */
	unsigned boot_count;
	int adopted;
	u64_t xnu_l2_pa;
	u64_t fb_off;			/* the framebuffer's offset in the DRAM slice */
	u64_t ba_xnu;			/* the boot args, XNU's address */
	u64_t diag_va[6];
	u64_t a_cpu_entry;
	unsigned long (*kfault)(unsigned long far, unsigned long esr);
	unsigned long (*kfault_recover)(unsigned long pc, unsigned long far, unsigned long esr);
	unsigned long kfaults, kfault_moved, ubsan_traps;
	volatile const unsigned long *dbg;	/* XNU's linux64_dbg, through the Linux alias (host threads) */
	volatile unsigned long *dbg_rw;
	volatile const unsigned long *el0_stat;	/* XNU's linux64_el0_stat[cpu][count, ticks][class], ditto */
	unsigned long kcount, idles, unknown_told;
	int told_sp;
	u64_t t_start;
	long audit_last;
	unsigned long syncs_snap;	/* the root's lazy syncs, as of the monitor's last look (status reads this) */
	unsigned long audits, audit_total;
	char root_matching[256];
	unsigned char seed[256];
	char compat[96];
	char cname[XNU_MAX_CPUS][8];
	unsigned long long tte[2048];	/* XNU's kernel L1 table, copied at LINUX64_CALL_KERNEL_TABLES */
	struct { atomic_t lock; unsigned n; char line[1000]; char m[1024]; } con;
	atomic_t llock;
	char lbuf[1100];
	unsigned long ring_head, ring_tail;
	char ring[XG_RING];		/* the guest's log lines for console_log (its monitor writes them out) */
	char flushbuf[XG_RING];
	unsigned long acct_last_t, acct_last_ticks[ACCT_N], acct_last_calls[ACCT_N], acct_last_disk;
	unsigned long el0_last_n[LINUX64_EL0_CLASSES], el0_last_t[LINUX64_EL0_CLASSES];
	int mon_lines;
	unsigned long mon_ticks;
};
static struct xnu_guest xnu_guest_pool[XNU_MAX_GUESTS];
/* per CPU slot: world.S's save area, the guest, the Linux CPU, the slot's own counters */
struct xnu_world *xnu_worlds[LINUX64_CPU_SLOTS];
static struct xnu_guest *xnu_guest_of_slot[LINUX64_CPU_SLOTS];
static short xnu_linux_cpu_of_slot[LINUX64_CPU_SLOTS];
static struct xnu_slot xnu_slot_state[LINUX64_CPU_SLOTS];
/* per Linux CPU: the guest that has it; host CPUs no guest may have */
#define XNU_LINUX_CPUS 256
static struct xnu_guest *xnu_cpu_owner[XNU_LINUX_CPUS];
static unsigned char host_cpu[XNU_LINUX_CPUS];
static int host_cpu_list[XNU_LINUX_CPUS], host_cpu_n;
/* claims (CPUs, slots) and ctl commands: process context only, so a sleeping lock */
static atomic_t xg_lock_word = ATOMIC_INIT(0), xg_ctl_word = ATOMIC_INIT(0);
static void xg_lock(void) { while (atomic_xchg_acquire(&xg_lock_word, 1)) fki_msleep(1); }
static void xg_unlock(void) { atomic_set_release(&xg_lock_word, 0); }
static void xg_ctl_lock(void) { while (atomic_xchg_acquire(&xg_ctl_word, 1)) fki_msleep(1); }
static void xg_ctl_unlock(void) { atomic_set_release(&xg_ctl_word, 0); }

#define IN_XNU xnu_slot_state[this_slot()].in_xnu
#define xnu_deferred xnu_slot_state[this_slot()].deferred

/* ---- logging: "xnu:" for guest 0, "xnu[N]:" (and "darwin console[N]:") for guest N; also into the guest's ring ---- */
static void xg_ring_put(struct xnu_guest *g, const char *s, unsigned n)
{
	unsigned long h = g->ring_head;
	for (unsigned i = 0; i < n; i++)
		g->ring[(h + i) & (XG_RING - 1)] = s[i];
	smp_store_release(&g->ring_head, h + n);
}

static void glog_lvl(struct xnu_guest *g, const char *s, int alert)
{
	if (!g) {
		(alert ? fki_log_alert : fki_log)(s, x_strlen(s));
		return;
	}
	while (atomic_cmpxchg_acquire(&g->llock, 0, 1))
		cpu_relax();
	char *b = g->lbuf;
	unsigned j = 0, i = 0;
	if (g->id) {
		const char *pfx = 0;
		if (!x_strncmp(s, "xnu", 3) && (s[3] == ':' || s[3] == ']')) { pfx = "xnu"; i = 3; }
		else if (!x_strncmp(s, "darwin console:", 15)) { pfx = "darwin console"; i = 14; }
		else pfx = "xnu";
		while (*pfx) b[j++] = *pfx++;
		b[j++] = '[';
		b[j++] = '0' + g->id;
		b[j++] = ']';
		if (!i) { b[j++] = ':'; b[j++] = ' '; }
	}
	while (s[i] && j < sizeof(g->lbuf) - 2) b[j++] = s[i++];
	b[j] = 0;
	(alert ? fki_log_alert : fki_log)(b, j);
	b[j++] = '\n';
	xg_ring_put(g, b, j);
	atomic_set_release(&g->llock, 0);
}
static void glogs(struct xnu_guest *g, const char *s) { glog_lvl(g, s, 0); }
static void glogs_alert(struct xnu_guest *g, const char *s) { glog_lvl(g, s, 1); }
static void glogkv(struct xnu_guest *g, const char *k, u64_t v)
{
	char buf[120]; char *p = buf;
	while (*k && p < buf + 96) *p++ = *k++;
	*p++ = ' '; *p++ = '0'; *p++ = 'x';
	for (int i = 60; i >= 0; i -= 4) *p++ = "0123456789abcdef"[(v >> i) & 15];
	*p = 0;
	glogs(g, buf);
}
static void logs(const char *s) { fki_log(s, x_strlen(s)); }

static char *put_dec(char *p, unsigned long v)
{
	char t[24]; int n = 0;
	do { t[n++] = '0' + v % 10; v /= 10; } while (v);
	while (n) *p++ = t[--n];
	return p;
}
static void put_str(char **p, const char *s) { while (*s) *(*p)++ = *s++; }
static void put_hex(char **p, unsigned long v) { for (int i = 44; i >= 0; i -= 4) *(*p)++ = "0123456789abcdef"[(v >> i) & 15]; }

/* ---- a guest's memory as Linux maps it ---- */
static void *xg_pa_alias_n(struct xnu_guest *g, u64_t pa, u64_t len)
{
	if (!g->img.region.va || !len || len > g->img.region.size || pa < g->img.region.pa) return 0;
	if (pa - g->img.region.pa > g->img.region.size - len) return 0;
	return (char *)g->img.region.va + (pa - g->img.region.pa);
}
static void *xg_pa_alias(struct xnu_guest *g, u64_t pa) { return xg_pa_alias_n(g, pa, 1); }
/* an address in XNU's bootstrap window (limited to linear_size once XNU's tables are live) */
static void *xg_va_alias_n(struct xnu_guest *g, u64_t va, u64_t len)
{
	u64_t lim = g->linear_size ? g->linear_size : g->img.region.size;
	if (!g->ttbr1) return (void *)va;	/* mode 2: XNU at a Linux address */
	if (!len || len > lim || va < g->window_base || va - g->window_base > lim - len) return 0;
	return (void *)(va + g->alias_off);
}
static void *xg_va_alias(struct xnu_guest *g, u64_t va) { return xg_va_alias_n(g, va, 1); }
/* any XNU kernel address: through XNU's own tables (16 KB: 32 MiB L2 blocks, 16 KB pages; 4 KB: 2 MiB, 4 KB) */
static void *xg_kva_alias_n(struct xnu_guest *g, u64_t va, u64_t len)
{
	void *p = xg_va_alias_n(g, va, len);
	if (p || !g->ttbr1 || !g->root_l1 || !len || va < XNU_VA_LO || va >= XNU_VA_HI) return p;
	u64_t d = READ_ONCE(g->root_l1[(va >> g_l1s) & (g_n - 1)]);
	if (!(d & 1)) return 0;
	if ((d & 3) == 1) {
		u64_t span = 1ULL << g_l1s, off = va & (span - 1);
		if (len > span - off) return 0;
		return xg_pa_alias_n(g, DESC_PA(d, span) | off, len);
	}
	unsigned long long *t = xg_pa_alias_n(g, DESC_PA(d, g_page), g_page);
	if (!t) return 0;
	d = READ_ONCE(t[(va >> g_l2s) & (g_n - 1)]);
	if (!(d & 1)) return 0;
	if ((d & 3) == 1) {
		u64_t off = va & (g_block - 1);
		if (len > g_block - off) return 0;
		return xg_pa_alias_n(g, DESC_PA(d, g_block) | off, len);
	}
	t = xg_pa_alias_n(g, DESC_PA(d, g_page), g_page);
	if (!t) return 0;
	d = READ_ONCE(t[(va >> g_s) & (g_n - 1)]);
	if ((d & 3) != 3) return 0;
	{
		u64_t off = va & (g_page - 1);
		if (len > g_page - off) return 0;
		return xg_pa_alias_n(g, DESC_PA(d, g_page) | off, len);
	}
}
static int xg_copy_xstr(struct xnu_guest *g, u64_t va, char *dst, unsigned cap)
{
	unsigned i = 0;
	if (!va || !cap) return 0;
	if (!g->ttbr1) {
		const char *s = (const char *)va;
		while (s[i] && i < cap - 1) { dst[i] = s[i]; i++; }
		dst[i] = 0;
		return i;
	}
	while (i < cap - 1) {
		const char *c = xg_kva_alias_n(g, va + i, 1);
		if (!c || !*c) break;
		dst[i++] = *c;
	}
	dst[i] = 0;
	return i;
}

/* ---- M1 ---- */
typedef unsigned long (*strlen_fn)(const char *);
typedef unsigned int (*crc32_fn)(unsigned int, const void *, unsigned long);

static int run_m1(struct xnu_guest *g)
{
	struct fki_pac_keys saved; unsigned long c;
	struct fki_pac_keys *keys = &g->cpu[0].world.xnu_keys;
	c = fki_enter(keys, &saved);
	int n = macho_rebase(&g->img);
	fki_leave(c, &saved);
	glogkv(g, "xnu: threaded fixups applied", (u64_t)n);
	if (n < 0) return n;
	u64_t v = macho_sym(&g->img, "_version"), sl = macho_sym(&g->img, "_strlen"), cr = macho_sym(&g->img, "_crc32");
	if (!v || !sl || !cr) { glogs(g, "xnu: symbols missing"); return 0; }
	c = fki_enter(keys, &saved);
	unsigned long len = ((strlen_fn)sl)((const char *)v);
	unsigned int crc = ((crc32_fn)cr)(0, (const void *)v, len);
	fki_leave(c, &saved);
	glogs(g, "xnu: XNU code ran inside Linux; XNU's version string:");
	fki_log((const char *)v, len);
	glogkv(g, "xnu: strlen(version) computed by XNU", len);
	glogkv(g, "xnu: crc32(version) computed by XNU", crc);
	return 0;
}

/* Walk XNU's kernel tables (inside its contiguous DRAM) for va and log the descriptors (in an upcall: XNU's root live). */
static unsigned long long *tbl_va(struct xnu_guest *g, u64_t desc)
{
	return xg_pa_alias(g, DESC_PA(desc, g_page));
}

static void walk_log(struct xnu_guest *g, const char *what, u64_t va, const unsigned long long *l1, unsigned l1off)
{
	unsigned i1 = (va >> g_l1s) & (g_n - 1), i2 = (va >> g_l2s) & (g_n - 1), i3 = (va >> g_s) & (g_n - 1);
	if (i1 < l1off) return;
	u64_t d1 = l1[i1 - l1off], d2 = 0, d3 = 0;
	glogs(g, what);
	glogkv(g, "xnu:   va", va);
	glogkv(g, g_s == 14 ? "xnu:   16K L1 index" : "xnu:   L1 index", i1);
	glogkv(g, "xnu:   L1", d1);
	if ((d1 & 3) == 3) {
		unsigned long long *t2 = tbl_va(g, d1);
		if (!t2) { glogs(g, "xnu:   L2 table outside XNU DRAM"); return; }
		d2 = t2[i2];
		glogkv(g, "xnu:   L2 index", i2);
		glogkv(g, "xnu:   L2", d2);
		if ((d2 & 3) == 3) {
			unsigned long long *t3 = tbl_va(g, d2);
			if (!t3) { glogs(g, "xnu:   L3 table outside XNU DRAM"); return; }
			d3 = t3[i3];
			glogkv(g, "xnu:   L3 index", i3);
			glogkv(g, "xnu:   L3", d3);
		} else if ((d2 & 3) == 1) {
			glogs(g, "xnu:   (L2 block)");
		}
	}
}

/*
 * Kernel page faults on XNU addresses, taken in Linux's vectors while XNU runs (XNU's keys and stack live), go to
 * XNU's own linux64_kernel_fault (vm_fault on the kernel map). IN_XNU is 0 while host code runs (upcalls, monitor).
 *
 * A kernel fault XNU's vm_fault should resolve is not resolved here, inside Linux's exception handler (a kprobe: no
 * sleeping, no rescheduling, and a nested fault in it is a missed kprobe and an oops), because vm_fault may page in
 * from disk (a file I/O upcall that lets Linux schedule) or block the thread. The exception instead returns into
 * xnu_fault_tramp (world.S) with every register as it was; the trampoline calls xnu_fault_deferred in XNU's own
 * context, then returns (eret) to the faulting instruction, to the copyio recovery path, or to the abort.
 * Each CPU's: the fault handler and the trampoline it returns into run on the same CPU, with nothing between them.
 */
extern void xnu_fault_tramp(void);
extern void xnu_world_fault_abort(void);

/*
 * No shadow call stack here: vm_fault may block, and the thread may come back on another CPU, whose Linux task's shadow
 * stack x18 then points into (the return address pushed on the first CPU's would be popped from the second's). XNU never
 * saves x18 per thread, so the register follows the CPU: what this calls pushes and pops on the current CPU's.
 */
unsigned long xnu_fault_deferred(unsigned long *frame) __attribute__((no_sanitize("shadow-call-stack")));
unsigned long xnu_fault_deferred(unsigned long *frame)
{
	unsigned s0 = this_slot();
	struct xnu_guest *g = xnu_guest_of_slot[s0];
	unsigned long far = xnu_deferred.far, esr = xnu_deferred.esr, pc = xnu_deferred.pc;
	/* per CPU: with one global record, two CPUs faulting at once reset each other's count, and a CPU looping on one
	 * address never reached the every-64 yield below (it held its Linux CPU, and Linux with it, for good) */
	unsigned long repeats = (far == xnu_slot_state[s0].last_far) ? xnu_slot_state[s0].repeats + 1 : 0;
	xnu_slot_state[s0].repeats = repeats;
	xnu_slot_state[s0].last_far = far;

	frame[31] = xnu_deferred.pstate;	/* the SPSR the trampoline returns with */
	/*
	 * The same address faulting again is normal (libmalloc's MADV_FREE_REUSABLE clears a page's referenced and
	 * modified state, so each reuse takes one fault). Only report it, rarely, when it looks endless.
	 */
	unsigned long kr_raw = g->kfault(far, esr);
	int kr = (int)kr_raw;
	if (this_slot() != s0) {
		/*
		 * vm_fault blocked and the thread came back on another CPU. xnu_fault_tramp's eret restores every register
		 * of the faulting context from frame, x18 included: that would be the first CPU's shadow stack pointer, and
		 * from then on this CPU's upcalls push and pop on the first CPU's Linux task's shadow stack while that CPU
		 * uses it too (corrupted shadow stacks, FPAC in whatever Linux code runs next). x18 follows the CPU.
		 */
		unsigned long x18;
		__asm__ volatile("mov %0, x18" : "=r"(x18));
		frame[18] = x18;
		g->kfault_moved++;
	}
	if (kr == 0) {
		if (repeats && (repeats & (repeats - 1)) == 0 && repeats >= 1024) {
			glogkv(g, "xnu: one address keeps taking resolved kernel faults, repeats", repeats);
			glogkv(g, "xnu:   far", far);
			glogkv(g, "xnu:   esr", esr);
			glogkv(g, "xnu:   pc ", pc);
			glogkv(g, "xnu:   lr ", frame[30]);
		}
		/* a fault loop runs with interrupts masked: let Linux take its interrupts now and then (RCU, network) */
		if ((repeats & 63) == 63) {
			void (*up)(unsigned long, unsigned long, unsigned long) = (void *)xnu_world_upcall;
			up(LINUX64_CALL_PREEMPTED, 0, 0);
		}
		return pc;
	}
	/* inside copyin/copyout: the routine's recovery path returns EFAULT to its caller, as XNU's own abort handler does */
	if (g->kfault_recover) {
		unsigned long rpc = g->kfault_recover(pc, far, esr);
		if (rpc) {
			frame[0] = 14;	/* EFAULT */
			return rpc;
		}
	}
	IN_XNU = 0;
	glogs(g, "xnu: XNU took a fault vm_fault cannot resolve; ending the run");
	glogkv(g, "xnu:   pc ", pc);
	glogkv(g, "xnu:   lr ", frame[30]);
	glogkv(g, "xnu:   far", far);
	glogkv(g, "xnu:   esr", esr);
	return (unsigned long)xnu_world_fault_abort;
}

/* fki's per-CPU fault handler (ctx: the guest): only on this guest's CPUs */
static int xnu_fault(void *ctx, unsigned long far, unsigned long esr, void *regs)
{
	struct xnu_guest *g = ctx;
	if (!IN_XNU)
		return 1;
	unsigned long fpc = fki_regs_pc(regs);
	unsigned long ufar = (far && !(far & (1ULL << 55))) ? (far & 0x00ffffffffffffffULL) : far;
	if (ufar && ((ufar >= XNU_VA_LO && ufar < XNU_VA_HI) || ufar < 0x0001000000000000ULL) && g->kfault &&
	    fpc >= XNU_VA_LO && fpc < XNU_VA_HI) {
		g->kfaults++;
		xnu_deferred.far = ufar;
		xnu_deferred.esr = esr;
		xnu_deferred.pc = fpc;
		xnu_deferred.pstate = fki_regs_pstate(regs);
		fki_regs_set_pc(regs, (unsigned long)xnu_fault_tramp);
		return 0;
	}
	/*
	 * An XNU fault nothing resolves (a bug, or a NULL dereference): report it and end the run instead of letting Linux
	 * oops with XNU's keys and stack live (which takes the machine with it). The exception returns into
	 * xnu_world_fault_abort, which puts Linux's keys back and leaves through xnu_world_abort.
	 */
	unsigned long pc = fki_regs_pc(regs);
	if (pc < XNU_VA_LO || pc >= XNU_VA_HI)
		return 1;
	/*
	 * brk #0xff00: XNU's UBSan minimal soft trap (signed overflow). XNU's own exception handler notes it
	 * (ubsan_handle_brk_trap) and resumes after the brk; do the same.
	 */
	if ((esr >> 26) == 0x3c && (esr & 0xffff) == 0xff00) {
		if (g->ubsan_traps++ < 8)
			glogkv(g, "xnu: UBSan soft trap (signed overflow), resuming; pc", pc);
		fki_regs_set_pc(regs, pc + 4);
		return 0;
	}
	/* inside copyin/copyout: the routine's recovery path returns EFAULT to its caller, as XNU's own abort handler does */
	if (g->kfault_recover) {
		unsigned long rpc = g->kfault_recover(pc, ufar, esr);
		if (rpc) {
			fki_regs_set_reg(regs, 0, 14);	/* EFAULT */
			fki_regs_set_pc(regs, rpc);
			return 0;
		}
	}
	IN_XNU = 0;
	glogs(g, "xnu: XNU took a fault it cannot handle; ending the run");
	glogkv(g, "xnu:   pc ", pc);
	glogkv(g, "xnu:   lr ", fki_regs_lr(regs));
	glogkv(g, "xnu:   far", far);
	glogkv(g, "xnu:   esr", esr);
	glogkv(g, "xnu:   CPU slot", this_slot());
	glogkv(g, "xnu:   x0 ", fki_regs_reg(regs, 0));
	glogkv(g, "xnu:   x1 ", fki_regs_reg(regs, 1));
	{	/* XNU's frame-pointer chain via XNU's own tables so a corrupt stack pointer cannot oops inside the kprobe */
		u64_t fp = fki_regs_reg(regs, 29);
		for (int i = 0; i < 12 && !(fp & 15) && fp >= XNU_VA_LO && fp < XNU_VA_HI; i++) {
			unsigned long *fr = g->ttbr1 ? xg_kva_alias_n(g, fp, 16) : (unsigned long *)fp;
			if (!fr) break;
			glogkv(g, "xnu:   frame", fr[1] | 0xffffff8000000000ul);
			if (fr[0] <= fp) break;
			fp = fr[0];
		}
	}
	fki_regs_set_pc(regs, (unsigned long)xnu_world_fault_abort);
	return 0;
}

/* A secondary CPU's host thread: its own world, entered at linux64_cpu_entry(cpu_data); returns when the run ends */
static int cpu_fn(void *arg)
{
	struct xg_cpu *pc = arg;
	struct xnu_guest *g = pc->g;

	fki_thread_wait(&pc->start_arg);	/* until XNU starts this CPU (LINUX64_CALL_CPU_START) */
	if (!pc->start_arg)
		return 0;			/* the run ended first: told to stop */
	glogkv(g, "xnu: XNU CPU starts on Linux CPU", (u64_t)pc->linux_cpu);
	if (g->cfg.park_secondaries) {	/* diagnostics: the CPU is asked for but never enters XNU */
		glogkv(g, "xnu: secondary CPU parked (park_secondaries)", (u64_t)pc->linux_cpu);
		pc->running = 0;
		while (!fki_thread_should_stop())
			fki_msleep(10);
		return 0;
	}
	IN_XNU = 1;
	long r = xnu_world_call(g->a_cpu_entry, pc->start_arg);
	IN_XNU = 0;
	if (!r) glogkv(g, "xnu: linux64_cpu_entry returned (unexpected) on CPU", (u64_t)pc->linux_cpu);
	g->stop = 1;			/* one CPU's run ending (a fatal fault, a stop) ends all of them */
	pc->running = 0;
	while (!fki_thread_should_stop())	/* kthread_stop() from the teardown wants the thread alive */
		fki_msleep(10);
	return 0;
}

static inline unsigned acct_bucket(unsigned long call) { return call >= 800 && call < 800 + ACCT_N - 1 ? call - 800 : ACCT_N - 1; }
static inline void acct_add(unsigned s, unsigned long call, u64_t ticks)
{
	unsigned b = acct_bucket(call);	/* per CPU: no atomics (and none of the compiler's out-of-line ones) */
	xnu_slot_state[s].acct_ticks[b] += ticks;
	xnu_slot_state[s].acct_calls[b]++;
}

/* XNU calls into the host: enum linux64_call (linux64_abi.h); a non-zero return ends the run */
static long xnu_hook_body(struct xnu_guest *g, unsigned long call, unsigned long a1, unsigned long a2);
/* each CPU's host calls so far, the last one's number, and whether it is inside one: the monitor's silent-CPU check */
long xnu_hook(unsigned long call, unsigned long a1, unsigned long a2)
{
	unsigned in_s = this_slot();	/* the thread is bound to its CPU while in the host */
	struct xnu_slot *ss = &xnu_slot_state[in_s];
	struct xnu_guest *g = xnu_guest_of_slot[in_s];
	ss->upcalls++;
	ss->last_call = call;
	ss->in_xnu = 0;
	if (!g)
		return 1;
	{
		/* debugging: an XNU stack Linux's overflow check would reject (bit 14 of sp set) */
		unsigned long sp;
		__asm__ volatile("mov %0, sp" : "=r"(sp));
		if ((sp & 0x4000) && g->told_sp < 3) {
			g->told_sp++;
			glogkv(g, "xnu: upcall on an XNU stack with sp bit 14 set, sp", sp);
			glogkv(g, "xnu:   call", call);
		}
	}
	if (READ_ONCE(g->stop) || (g->legacy && READ_ONCE(stop_run))) {
		/* the host asked the run to end (ctl stop, scripts/xnulinux-boot.sh stop): XNU is abandoned here */
		glogs(g, "xnu: run stopped by the host (stop_run)");
		return 1;
	}
	int pf = unlikely(perf || g->cfg.perf);
	u64_t t0 = pf ? fki_counter() : 0;
	ss->in_call = 1;
	long r = xnu_hook_body(g, call, a1, a2);
	ss->in_call = 0;
	if (pf)
		acct_add(in_s, call, fki_counter() - t0);
	if (!r) ss->in_xnu = 1;
	return r;
}

static int xg_cpu_of_slot(struct xnu_guest *g, unsigned slot)
{
	if (xnu_guest_of_slot[slot & (LINUX64_CPU_SLOTS - 1)] != g) return -1;
	for (int c = 0; c < g->ncpus; c++)
		if (g->cpu[c].slot == slot) return c;
	return -1;
}

static long xnu_hook_body(struct xnu_guest *g, unsigned long call, unsigned long a1, unsigned long a2)
{
	if (call == LINUX64_CALL_CONSOLE_READ) {		/* console input: new bytes of console_in */
		struct linux64_buf_req *q = (struct linux64_buf_req *)a1;
		if (!q) return 0;
		q->result = 0;
		/* nothing new (the usual answer): no I/O, Linux does not even run */
		if (g->ci_file && fki_file_size(g->ci_file) > g->ci_off) {
			long n = fki_file_rw(g->ci_file, 0, q->buf, (unsigned long)q->len, g->ci_off);
			if (n > 0) { g->ci_off += n; q->result = n; }
		}
		return 0;
	}
	if (call == LINUX64_CALL_INPUT_READ) {		/* input events: whole 8-byte records of input_in, into q->buf */
		struct linux64_buf_req *q = (struct linux64_buf_req *)a1;
		if (!q) return 0;
		q->result = 0;
		if (g->in_file && q->len >= 8 && fki_file_size(g->in_file) >= g->in_off + 8) {
			long n = fki_file_rw(g->in_file, 0, q->buf, (unsigned long)(q->len & ~7ULL), g->in_off);
			if (n > 0) { n &= ~7L; g->in_off += n; q->result = n; }
		}
		return 0;
	}
	if (call == LINUX64_CALL_NET_SEND || call == LINUX64_CALL_NET_RECV) {	/* XNU's Ethernet link: one frame each way */
		struct linux64_buf_req *q = (struct linux64_buf_req *)a1;
		if (!q) return 0;
		q->result = call == LINUX64_CALL_NET_SEND ? fki_net_send2(g->net, q->buf, (unsigned long)q->len) :
							    fki_net_recv2(g->net, q->buf, (unsigned long)q->len);
		return 0;
	}
	if (call == LINUX64_CALL_WALLCLOCK) {		/* the host's wall clock for XNU's calendar: a1 -> u64[2] {seconds, nanoseconds} UTC */
		if (a1) fki_realtime(&((fki_u64 *)a1)[0], &((fki_u64 *)a1)[1]);
		return 0;
	}
	if (call == LINUX64_CALL_RANDOM) {	/* a1: a2 bytes from Linux's generator */
		if (a1) fki_random((void *)a1, a2);
		return 0;
	}
	if (call == LINUX64_CALL_HOSTCALL) {		/* host call: q->result = the reply's length or -errno */
		struct linux64_hostcall_req *q = (struct linux64_hostcall_req *)a1;
		if (q) q->result = fki_hostcall2(g->hc, q->req, (unsigned long)q->len, q->reply, (unsigned long)q->max);
		return 0;
	}
	if (call == LINUX64_CALL_DISK_SIZE) {		/* the block disk's size in bytes into *a1, 0 when there is none */
		if (a1) *(u64_t *)a1 = g->dk_file ? g->dk_size : 0;
		return 0;
	}
	if (call == LINUX64_CALL_DISK_RW) {		/* block disk I/O: q->result = bytes moved or -errno */
		struct linux64_rw_req *q = (struct linux64_rw_req *)a1;
		if (q) q->result = g->dk_file ? fki_file_rw(g->dk_file, (int)q->write, q->buf, (unsigned long)q->len, q->off) : -6;
		if (q && q->result > 0) xnu_slot_state[this_slot()].acct_disk += (unsigned long)q->result;
		return 0;
	}
	if (call == LINUX64_CALL_DISK_RW_PHYS) {	/* zero-copy block disk I/O at guest physical addr */
		struct linux64_rw_req *q = (struct linux64_rw_req *)a1;
		if (!q) return 0;
		if (!g->dk_file) { q->result = -6; return 0; }
		u64_t pa = (u64_t)(uintptr_t)q->buf;
		u64_t len = q->len;
		u64_t base_pa = (g->mode == 3) ? g->img.region.pa : XNU_FAKE_PHYS;
		if (pa < base_pa || len > g->img.region.size || pa - base_pa > g->img.region.size - len) {
			q->result = -22;
			return 0;
		}
		void *kva = (char *)g->img.region.va + (pa - base_pa);	/* the Linux alias: what Linux reads into */
		q->result = fki_file_rw_direct(g->dk_file, (int)q->write, kva, (unsigned long)len, q->off);
		if (q->result > 0) xnu_slot_state[this_slot()].acct_disk += (unsigned long)q->result;
		return 0;
	}
	if (call == LINUX64_CALL_KEYSTORE_RW) {		/* the keystore file: q->result = bytes moved or -errno */
		struct linux64_rw_req *q = (struct linux64_rw_req *)a1;
		if (!q) return 0;
		q->result = g->ks_file ? fki_file_rw(g->ks_file, (int)q->write, q->buf, (unsigned long)q->len, q->off) : -6;
		return 0;
	}
	if (call == LINUX64_CALL_RAMDISK_SIZE) {		/* host disk size in bytes into *a1, 0 when there is none */
		if (a1) *(u64_t *)a1 = g->hd_file ? g->hd_size : 0;
		return 0;
	}
	if (call == LINUX64_CALL_RAMDISK_RW) {		/* host disk I/O: q->result = bytes moved or -errno */
		struct linux64_rw_req *q = (struct linux64_rw_req *)a1;
		if (!q) return 0;
		if (!g->hd_file) q->result = -6;
		else if (q->write && !g->cfg.hostdisk_rw) q->result = -30;
		else q->result = fki_file_rw(g->hd_file, (int)q->write, q->buf, (unsigned long)q->len, q->off);
		return 0;
	}
	if (call == LINUX64_CALL_CONSOLE_WRITE) {		/* XNU console bytes (/dev/console): line-buffered into the host log */
		const char *b = (const char *)a1; unsigned long i;
		while (atomic_cmpxchg_acquire(&g->con.lock, 0, 1))
			cpu_relax();
		for (i = 0; b && i < a2; i++) {
			char c = b[i];
			if (c == '\r') continue;
			if (c != '\n' && g->con.n < sizeof(g->con.line) - 1) g->con.line[g->con.n++] = c;
			if (c == '\n' || g->con.n >= sizeof(g->con.line) - 1) {
				unsigned j = 0, k;
				const char *p = "darwin console: ";
				while (*p) g->con.m[j++] = *p++;
				for (k = 0; k < g->con.n; k++) g->con.m[j++] = g->con.line[k];
				g->con.m[j] = 0;
				/* quiet_console: launchd's and AppleImage4's chatter (thousands of lines a second) stays out */
				if (!(g->cfg.quiet_console && quiet_line(g->con.line, g->con.n)))
					glogs(g, g->con.m);
				g->con.n = 0;
			}
		}
		/* a prompt (a write that ends in "# ", "$ " or "> " without a newline): show it now */
		unsigned n = g->con.n;
		if (n >= 2 && g->con.line[n - 1] == ' ' && (g->con.line[n - 2] == '#' || g->con.line[n - 2] == '$' || g->con.line[n - 2] == '>')) {
			unsigned j = 0, k;
			const char *p = "darwin console: ";
			while (*p) g->con.m[j++] = *p++;
			for (k = 0; k < n; k++) g->con.m[j++] = g->con.line[k];
			g->con.m[j] = 0; g->con.n = 0;
			glogs(g, g->con.m);
		}
		atomic_set_release(&g->con.lock, 0);
		return 0;
	}
	if (call == LINUX64_CALL_KPRINTF) {		/* XNU kprintf */
		char m[260]; const char *f = (const char *)a1; unsigned i = 0, j = 0;
		m[j++] = 'x'; m[j++] = 'n'; m[j++] = 'u'; m[j++] = ']'; m[j++] = ' ';
		while (f && f[i] && j < sizeof(m) - 1) { if (f[i] != '\n') m[j++] = f[i]; i++; }
		m[j] = 0;
		/* the per-entry startup trace is thousands of lines: count it, do not print it */
		if (!(f && !x_strncmp(f, "kernel_startup_initialize_upto[", 31)) && !(g->cfg.quiet_console && quiet_line(m + 5, j - 5)))
			glogs(g, m);
		if (g->cfg.kmax && ++g->kcount >= (unsigned long)g->cfg.kmax) { glogs(g, "xnu: kprintf budget reached"); return 1; }
		return 0;
	}
	if (call == LINUX64_CALL_KERNEL_TABLES) {	/* install XNU's kernel L1 entries (the kernel VA range only) in its root */
		if (!g->ttbr1 || !a1)
			return 0;
		/*
		 * XNU's L1 table (cpu_tte) is read through the bootstrap window, and installing XNU's entry for the window's
		 * own slot replaces that window: XNU's tables need not map cpu_tte at this stage (L3 0 here), so the rest of
		 * the table is read from a copy taken first, never through the slot being replaced.
		 */
		x_memcpy(g->tte, (const void *)a1, g_n * sizeof(g->tte[0]));
		unsigned wi = (unsigned)((g->window_base >> g_l1s) & (g_n - 1)), off = 0;
		if (g_s == 14 && !(g->tte[wi] & 1) && (g->tte[7] & 3) == 3) {
			/* an XNU whose kernel root is 8 entries (T1SZ 25): its entry 7 is the window's L1 entry */
			int only_low = 1;
			for (unsigned i = 8; i < g_n; i++) if (g->tte[i] & 1) only_low = 0;
			if (only_low) { off = wi - 7; glogkv(g, "xnu: XNU's kernel L1 table has 8 entries; its entry 7 is L1 index", wi); }
		}
		walk_log(g, "xnu: XNU table walk for arm_init (text)", g->diag_va[0], g->tte, off);
		walk_log(g, "xnu: XNU table walk for the Mach-O header", g->diag_va[1], g->tte, off);
		walk_log(g, "xnu: XNU table walk for boot args", g->diag_va[2], g->tte, off);
		walk_log(g, "xnu: XNU table walk for the cpu_tte page", a1, g->tte, off);
		int n = 0;
		for (unsigned i = 0; i + off < g_n && (off == 0 || i < 8); i++) {
			unsigned li = i + off;
			if (!(g->tte[i] & 1)) continue;
			if (li < xnu_l1_first || li > xnu_l1_last || (li < w_first && !(p_valid && li == p_idx)) ||
			    (li >= w_end && !(p_valid && li == p_idx))) {
				glogkv(g, "xnu: XNU L1 entry outside its root's range, not installed:", li);
				continue;
			}
			if (p_valid && li == p_idx) {	/* the partial entry: XNU's L2 table, Linux's entries above 48 GiB copied in */
				u64_t d = g->tte[i], l2pa = DESC_PA(d, g_page);
				unsigned long long *nl2 = (d & 3) == 3 ? xg_pa_alias(g, l2pa) : 0;
				if (!nl2 || !xg_pa_alias(g, l2pa + g_page - 1)) {
					glogkv(g, "xnu: XNU has no L2 table in its DRAM for the shared L1 slot, refusing:", d);
					return 1;
				}
				for (int c = 1; c < g->ncpus; c++)
					if (g->cpu[c].running || g->cpu[c].start_arg) {
						glogs(g, "xnu: secondary CPU running before shared L1 adopt, refusing");
						return 1;
					}
				int e = fki_ttbr1_adopt_l2(g->ttbr1, nl2, l2pa);
				if (e) { glogkv(g, "xnu: the shared L1 slot could not be handed to XNU's L2 table, err", (u64_t)(long)e); return 1; }
				g->adopted = 1;
				g->xnu_l2_pa = l2pa;
				g->boot_count = 0;
				glogkv(g, "xnu: shared L1 slot adopted: XNU's L2 table at phys", l2pa);
				n++;
				continue;
			}
			int e = fki_ttbr1_set(g->ttbr1, &g->root_l1[li], g->tte[i]);
			if (e) { glogkv(g, "xnu: the root refused XNU's L1 entry", li); return 1; }
			n++;
		}
		if (g->static_size) g->linear_size = g->static_size;
		glogkv(g, "xnu: XNU L1 entries installed", n);
		/* Touch XNU memory through XNU's own tables now (XNU's root live: a fault here is a readable oops). */
		glogkv(g, "xnu: probe text word", *(volatile unsigned int *)g->diag_va[0]);
		glogkv(g, "xnu: probe Mach-O magic", *(volatile unsigned int *)g->diag_va[1]);
		glogkv(g, "xnu: probe boot args Version", ((volatile struct boot_args_v2 *)g->diag_va[2])->Version);
		glogkv(g, "xnu: probe ADT word", *(volatile unsigned int *)g->diag_va[3]);
		if (g->diag_va[4]) { volatile unsigned int *pc = (void *)g->diag_va[4]; unsigned v = *pc; *pc = v; glogkv(g, "xnu: probe port_crumb (rw)", v); }
		if (g->diag_va[5]) { unsigned long l = ((unsigned long (*)(const char *))g->diag_va[5])("xnulinux"); glogkv(g, "xnu: probe exec strlen through XNU tables", l); }
		glogs(g, "xnu: probes done");
		return 0;
	}
	if (call == LINUX64_CALL_PREEMPTED) {		/* XNU busy: a host IRQ preempted its user code; let Linux handle it */
		fki_yield();
		return 0;
	}
	if (call == LINUX64_CALL_IDLE) {		/* XNU idle: let Linux run until XNU's next timer deadline */
		g->idles++;
		if ((g->idles & (g->idles - 1)) == 0) glogkv(g, "xnu: XNU idle, count", g->idles);
		if (!a1)		/* a timer is due: only let Linux take its interrupts */
			fki_yield();
		else {			/* sleep until XNU's next timer, at most 20 ms (XNU's devices are polled from its timers) */
			u64_t ns = a1 < 20000000ULL ? a1 : 20000000ULL;
			/* the CPU's pending-IPI word is XNU's (its KC __DATA): Linux reads it through the alias */
			const volatile unsigned int *w = a2 ? xg_va_alias(g, a2) : 0;
			fki_idle_until(fki_counter() + ns * fki_counter_frequency() / 1000000000ULL, ns, w);
		}
		return g->cfg.idle_max && g->idles >= (unsigned long)g->cfg.idle_max;
	}
	if (call == LINUX64_CALL_ASSERT) {		/* XNU assert() failed */
		const struct linux64_assert_info *a = !a1 ? 0 : g->ttbr1 ? xg_kva_alias_n(g, a1, sizeof(*a)) : (const void *)a1;
		char m[240], fs[96], es[120]; unsigned j = 0, i;
		if (!a) { glogs(g, "xnu: XNU assert failed (invalid info)"); return g->cfg.assert_fatal; }
		fs[0] = 0; es[0] = 0;
		xg_copy_xstr(g, (u64_t)a->file, fs, sizeof(fs));
		xg_copy_xstr(g, (u64_t)a->expr, es, sizeof(es));
		{
			const char *parts[4] = { "xnu: XNU assert failed: ", fs, ": ", es };
			for (int p = 0; p < 4; p++)
				for (i = 0; parts[p][i] && j < sizeof(m) - 1; i++) m[j++] = parts[p][i];
		}
		m[j] = 0;
		glogs(g, m);
		glogkv(g, "xnu:   at line", a->line);
		return g->cfg.assert_fatal;
	}
	if (call == LINUX64_CALL_USER_ENTER) {		/* the first entry into user space (linux64_el0_run follows) */
		glogkv(g, "xnu: XNU enters user space (EL0); user pc", a1);
		return 0;
	}
	if (call == LINUX64_CALL_PANIC) {
		char m[500];
		m[0] = 0;
		xg_copy_xstr(g, a1, m, sizeof(m));
		glogs_alert(g, "xnu: XNU panic:");
		glogs_alert(g, m);
		glogkv(g, "xnu: panic caller", a2);
		{	/* walk XNU's frames safely through XNU's own tables so corrupt pointers cannot oops on Linux */
			u64_t fp = (u64_t)__builtin_frame_address(0);
			for (unsigned i = 0; i < 24 && !(fp & 15) && fp >= XNU_VA_LO && fp < XNU_VA_HI; i++) {
				unsigned long *fr = g->ttbr1 ? xg_kva_alias_n(g, fp, 16) : (unsigned long *)fp;
				if (!fr) break;
				glogkv(g, "xnu:   frame", fr[1] | 0xffffff8000000000ul);
				if (fr[0] <= fp) break;
				fp = fr[0];
			}
		}
		return 1;
	}
	if (call == LINUX64_CALL_CPU_START) {	/* start XNU's CPU of MPIDR a1 at linux64_cpu_entry(a2), asynchronously */
		int c = xg_cpu_of_slot(g, LINUX64_CPU_SLOT(a1));
		/* its thread was made before XNU ran (an upcall runs with preemption off: no thread creation here); a CPU of
		 * another guest, or none, is refused */
		if (c <= 0 || !g->cpu[c].thread || !a2 || g->cpu[c].start_arg) {
			glogkv(g, "xnu: XNU asked to start a CPU it was not given, MPIDR", a1);
			return 0;
		}
		g->cpu[c].running = 1;
		WRITE_ONCE(g->cpu[c].start_arg, a2);
		fki_thread_wake(g->cpu[c].thread);
		return 0;
	}
	if (call == LINUX64_CALL_IPI) {		/* XNU's CPU of MPIDR a1 has an IPI pending: make it look (this guest's only) */
		int c = xg_cpu_of_slot(g, LINUX64_CPU_SLOT(a1));
		if (c >= 0)
			fki_kick_cpu(g->cpu[c].linux_cpu);
		return 0;
	}
	if (call != g->unknown_told) {	/* a call this module does not know (an XNU built against a newer linux64_abi.h): say so once, go on */
		g->unknown_told = call;
		glogkv(g, "xnu: unknown call from XNU", call);
	}
	return 0;
}

/*
 * Boot handoff. The XNU "DRAM" region is described by (virtBase, physBase, size): XNU sees xnu_base as the kernel virtual
 * address of physBase. The image's vmlow sits at xnu_base + image_off. Everything is written through base, where Linux
 * maps the same memory; what XNU is handed is its own addresses.
 */
#define TC_SEG_SIZE(g) ((g)->tc_size ? (g)->tc_size + 8 : 0)	/* trust_cache_offsets_t { num_caches = 1, offsets[1] } + the module */
#define FB_SIZE(g) ((g)->cfg.fb_width > 0 && (g)->cfg.fb_height > 0 ? (u64_t)(g)->cfg.fb_width * (g)->cfg.fb_height * 4 : 0)
#define BOOT_EXTRA(g) ((((g)->rd_size + 0x3fff) & ~0x3fffULL) + ((TC_SEG_SIZE(g) + 0x3fff) & ~0x3fffULL) + ((FB_SIZE(g) + 0x3fff) & ~0x3fffULL))
static const unsigned char nvram_blank[0x10000];
static const unsigned char zero48[48];

static void cat(char *dst, unsigned long cap, const char *s)
{
	unsigned long n = x_strlen(dst), m = x_strlen(s);
	if (n + m + 1 > cap) return;
	x_memcpy(dst + n, s, m + 1);
}
static int has_word(const char *s, const char *w)
{
	unsigned long m = x_strlen(w);
	for (; *s; s++)
		if (!x_strncmp(s, w, m)) return 1;
	return 0;
}

static struct boot_args_v2 *build_boot(struct xnu_guest *g, unsigned char *base, u64_t xnu_base, u64_t phys, u64_t size,
				       u64_t image_off, u64_t image_size)
{
	struct xg_cfg *c = &g->cfg;
	u64_t off_args = (image_off + image_size + 0x3fff) & ~0x3fffULL;
	struct boot_args_v2 *ba = (void *)(base + off_args);
	u64_t off_dt = off_args + 0x4000;
	struct adt a;

	fki_random(g->seed, sizeof(g->seed));
	glogkv(g, "xnu: running on MPIDR Aff1:Aff0", g->cpu[0].mpidr);
	glogkv(g, "xnu: XNU CPUs", (u64_t)g->ncpus);

	adt_init(&a, base + off_dt, 0x40000);
	adt_node(&a, 3, 6);				/* / */
	adt_prop_str(&a, "name", "device-tree");
	{	/* the platform's own name, then the model: user space's product type (MobileGestalt) comes from this list */
		static const char own[] = "xnulinux,linux64";
		unsigned n = sizeof(own), m = (unsigned)x_strlen(c->model);
		x_memcpy(g->compat, own, n);
		if (m && n + m + 1 <= sizeof(g->compat) && x_strcmp(c->model, "xnulinux")) {
			x_memcpy(g->compat + n, c->model, m + 1);
			n += m + 1;
		}
		adt_prop(&a, "compatible", g->compat, n);
	}
	adt_prop_str(&a, "model", c->model);
	g->root_matching[0] = 0;
	if (x_strlen(c->root_uuid) == 36) {
		static const char pre[] = "<dict><key>IOProviderClass</key><string>IOMedia</string><key>IOPropertyMatch</key>"
		    "<dict><key>UUID</key><string>", post[] = "</string></dict></dict>";
		x_memcpy(g->root_matching, pre, sizeof(pre) - 1);
		x_memcpy(g->root_matching + sizeof(pre) - 1, c->root_uuid, 36);
		x_memcpy(g->root_matching + sizeof(pre) - 1 + 36, post, sizeof(post));
	}
	int has_boot = c->boot_uuid[0] != 0, has_pre = c->preboot_uuid[0] != 0;
	adt_node(&a, 22 + has_boot + has_pre + (g->root_matching[0] ? 1 : 0), 3);	/* /chosen */
	if (g->root_matching[0]) adt_prop_str(&a, "root-matching", g->root_matching);
	if (has_boot) adt_prop_str(&a, "boot-uuid", c->boot_uuid);
	if (has_pre) adt_prop_str(&a, "apfs-preboot-uuid", c->preboot_uuid);
	adt_prop_str(&a, "name", "chosen");
	adt_prop_u64(&a, "dram-base", phys);
	adt_prop_u64(&a, "dram-size", size);
	adt_prop(&a, "random-seed", g->seed, sizeof(g->seed));
	adt_prop_str(&a, "firmware-version", "xnulinux-m2");
	adt_prop_u32(&a, "debug-enabled", 1);
	/* the SHA-384 of the boot manifest a bootloader would pass; there is none, so zeros (27's Image4 needs the property) */
	adt_prop(&a, "boot-manifest-hash", zero48, sizeof(zero48));
	/* NVRAM as iBoot hands it over: the bank size and the image (blank: IODTNVRAM formats it as a CHRP store) */
	adt_prop_u32(&a, "nvram-bank-size", sizeof(nvram_blank));
	adt_prop_u32(&a, "nvram-bank-count", 1);
	adt_prop_u32(&a, "nvram-current-bank", 0);
	adt_prop(&a, "nvram-proxy-data", nvram_blank, sizeof(nvram_blank));
	/*
	 * The identity a bootloader states for the AP, which Image4 reads: Apple's VM platform (chip 0xFE00, board 0x20)
	 * in an insecure, non-production mode, as a development VM boots.
	 */
	adt_prop_str(&a, "crypto-hash-method", "sha2-384");
	adt_prop_u32(&a, "chip-id", 0xfe00);
	adt_prop_u32(&a, "board-id", 0x20);
	adt_prop_u64(&a, "unique-chip-id", 0x0000584e554c4e58ULL);
	adt_prop_u32(&a, "chip-epoch", 0);
	adt_prop_u32(&a, "security-domain", 1);
	adt_prop_u32(&a, "certificate-production-status", 0);
	adt_prop_u32(&a, "certificate-security-mode", 0);
	adt_prop_u32(&a, "effective-production-status-ap", 0);
	adt_prop_u32(&a, "effective-security-mode-ap", 0);
	adt_prop_u32(&a, "mix-n-match-prevention-status", 0);
	u64_t off_rd = (off_dt + 0x40000 + 0x3fff) & ~0x3fffULL;	/* the RAM disk, after the ADT's room */
	u64_t off_tc = (off_rd + g->rd_size + 0x3fff) & ~0x3fffULL;	/* the trust cache segment, after the RAM disk */
	u64_t off_fb = (off_tc + TC_SEG_SIZE(g) + 0x3fff) & ~0x3fffULL;	/* the framebuffer, after the trust cache */
	adt_node(&a, 1 + (g->rd_size ? 1 : 0) + (g->tc_size ? 1 : 0), 0);	/* /chosen/memory-map (no AuxKC) */
	adt_prop_str(&a, "name", "memory-map");
	if (g->rd_size) {
		u64_t rd[2] = { phys + off_rd, g->rd_size };
		adt_prop(&a, "RAMDisk", rd, sizeof(rd));
	}
	if (g->tc_size) {
		u64_t tc[2] = { phys + off_tc, TC_SEG_SIZE(g) };	/* DTTrustCacheRange: load_static_trust_cache() */
		adt_prop(&a, "TrustCache", tc, sizeof(tc));
	}
	adt_node(&a, 1, 0);				/* /chosen/manifest-properties: AppleImage4 reads it (empty: no manifest) */
	adt_prop_str(&a, "name", "manifest-properties");
	adt_node(&a, 3, 0);				/* /chosen/asmb: the SIP configuration a bootloader passes (csr_bootstrap) */
	adt_prop_str(&a, "name", "asmb");
	adt_prop_u64(&a, "lp-sip0", 0x2);		/* CSR_ALLOW_UNTRUSTED_KEXTS, as DroidVM boots */
	adt_prop_u32(&a, "lp-sip1", 1);			/* CSR_ALLOW_UNAUTHENTICATED_ROOT */
	adt_node(&a, 1, g->ncpus);			/* /cpus */
	adt_prop_str(&a, "name", "cpus");
	/*
	 * /cpus/cpu0, without device_type "cpu": that would match Apple's CPU driver (AppleARMCPU, IONameMatch "cpu"),
	 * which drives the AIC interrupts and IPIs of real hardware. While hosted, the CPU is Linux's.
	 */
	for (int k = 0; k < g->ncpus; k++) {	/* compatible: kexts/XnuCPU registers each with XNU and starts it */
		char *cn = g->cname[k];
		cn[0] = 'c'; cn[1] = 'p'; cn[2] = 'u';
		if (k < 10) { cn[3] = '0' + k; cn[4] = 0; } else { cn[3] = '1'; cn[4] = '0' + k - 10; cn[5] = 0; }
		adt_node(&a, 6, 0);
		adt_prop_str(&a, "name", cn);
		adt_prop_str(&a, "compatible", "xnulinux,cpu");
		adt_prop_str(&a, "state", k ? "waiting" : "running");
		adt_prop_u32(&a, "reg", (unsigned)g->cpu[k].mpidr);
		adt_prop_u64(&a, "timebase-frequency", fki_counter_frequency());
		adt_prop_u32(&a, "cluster-type", 'P');	/* the AMP scheduler wants E/M/P, not SMP */
	}
	adt_node(&a, 1, 0);				/* /defaults: XNU looks these up without checking */
	adt_prop_str(&a, "name", "defaults");
	adt_node(&a, 4, 0);				/* /product */
	adt_prop_str(&a, "name", "product");
	/*
	 * MobileGestalt's product type in sandboxed processes: their profiles let them read only these two /product
	 * properties (apsd's: "sub-product-type", "unique-model"), and apsd aborts without a product type
	 */
	adt_prop_str(&a, "sub-product-type", c->model);
	adt_prop_str(&a, "unique-model", c->model);
	adt_prop_str(&a, "product-name", "xnulinux");
	adt_node(&a, 1, 0);				/* /options: NVRAM's node (AppleImage4 keeps its nonce seeds there) */
	adt_prop_str(&a, "name", "options");
	adt_node(&a, 3, FB_SIZE(g) ? 1 : 0);		/* /arm-io: XNU needs a nonzero SoC base */
	adt_prop_str(&a, "name", "arm-io");
	adt_prop_str(&a, "device_type", "linux64-io");
	{ u64_t ranges[3] = { 0, 0x7000000000ULL, 0x10000000ULL }; adt_prop(&a, "ranges", ranges, sizeof(ranges)); }
	if (FB_SIZE(g)) {
		/*
		 * /arm-io/gpu, compatible "paravirtualizedgraphics,gpu" as on Apple's VM: IOMobileFramebuffer.framework then waits
		 * for the framebuffers of the AppleParavirtGPU services (XnuDisplay; it matches IOResources, not this node, which
		 * no driver claims: /arm-io's device_type keeps Apple's ARM IO driver off). Without the node WindowServer counted
		 * no framebuffer and ran headless.
		 */
		adt_node(&a, 2, 0);
		adt_prop_str(&a, "name", "gpu");
		adt_prop_str(&a, "compatible", "paravirtualizedgraphics,gpu");
	}
	if (a.err) { glogs(g, "xnu: ADT overflow"); return 0; }
	u64_t dt_len = adt_size(&a);

	ba->Revision = 2; ba->Version = 2;
	ba->virtBase = xnu_base;
	ba->physBase = phys;
	ba->memSize = size;
	ba->memSizeActual = size;
	ba->topOfKernelData = phys + ((off_dt + dt_len + 0x3fff) & ~0x3fffULL);
	if (g->rd_size) {
		x_memcpy(base + off_rd, g->rd_data, g->rd_size);
		ba->topOfKernelData = phys + ((off_rd + g->rd_size + 0x3fff) & ~0x3fffULL);
		glogkv(g, "xnu: root RAM disk bytes", g->rd_size);
	}
	if (g->tc_size) {
		unsigned int hdr[2] = { 1, 8 };
		x_memcpy(base + off_tc, hdr, sizeof(hdr));
		x_memcpy(base + off_tc + 8, g->tc_data, g->tc_size);
		ba->topOfKernelData = phys + ((off_tc + TC_SEG_SIZE(g) + 0x3fff) & ~0x3fffULL);
		glogkv(g, "xnu: static trust cache bytes", g->tc_size);
	}
	if (FB_SIZE(g)) {
		unsigned int *px = (unsigned int *)(base + off_fb);
		for (u64_t i = 0; i < FB_SIZE(g) / 4; i++)
			px[i] = 0xff000000;	/* opaque black until the first frame */
		ba->Video.v_baseAddr = phys + off_fb;
		ba->Video.v_display = 0;	/* no boot console drawn into it (the console is the serial line) */
		ba->Video.v_rowBytes = (u64_t)c->fb_width * 4;
		ba->Video.v_width = (u64_t)c->fb_width;
		ba->Video.v_height = (u64_t)c->fb_height;
		ba->Video.v_depth = 32;
		ba->topOfKernelData = phys + ((off_fb + FB_SIZE(g) + 0x3fff) & ~0x3fffULL);
		g->fb_off = off_fb;
		glogkv(g, "xnu: framebuffer bytes", FB_SIZE(g));
	}
	ba->deviceTreeP = (void *)(xnu_base + off_dt);
	ba->deviceTreeLength = (unsigned)dt_len;
	ba->CommandLine[0] = 0;
	cat(ba->CommandLine, sizeof(ba->CommandLine), cmdline);
	cat(ba->CommandLine, sizeof(ba->CommandLine), (c->vm16k || g_s == 14) ? cmdline_16k : cmdline_4k);	/* a 16 KB host: 16 KB pages, always */
	if (g->rd_size || g->hd_file)
		cat(ba->CommandLine, sizeof(ba->CommandLine), " rd=md0");
	if (c->bootargs[0] && x_strlen(ba->CommandLine) + 1 + x_strlen(c->bootargs) < sizeof(ba->CommandLine)) {
		cat(ba->CommandLine, sizeof(ba->CommandLine), " ");
		cat(ba->CommandLine, sizeof(ba->CommandLine), c->bootargs);
	}
	if (!has_word(ba->CommandLine, "xnu-mac-id=")) {	/* XnuEthernet's MAC: 02:58:4e:55:00:(01 + id) */
		char idb[32] = " xnu-mac-id="; char *p = idb + x_strlen(idb);
		p = put_dec(p, (unsigned long)c->mac_id); *p = 0;
		cat(ba->CommandLine, sizeof(ba->CommandLine), idb);
	}
	g->ba_xnu = xnu_base + off_args;
	glogkv(g, "xnu: boot args at", g->ba_xnu);
	glogkv(g, "xnu: ADT bytes", dt_len);
	glogkv(g, "xnu: topOfKernelData (phys)", ba->topOfKernelData);
	return ba;
}

/*
 * Monitor: XNU runs with interrupts masked on its CPU, so when it spins nothing reaches Linux. A helper thread on a host
 * CPU writes the guest's log out to console_log (every 250 ms), prints XNU's debug counters (linux64_dbg, see the XNU
 * patch) every 2 s for the first 3 minutes, then every 20 s, checks for silent CPUs and audits the guest's private
 * root every 10 s. It reads the guest's memory through the Linux alias.
 */
static void xg_flush_log(struct xnu_guest *g)
{
	unsigned long h = smp_load_acquire(&g->ring_head), t = g->ring_tail, skip = 0, n;
	if (h == t) return;
	if (!g->log_file) { g->ring_tail = h; return; }
	if (h - t > XG_RING) t = h - XG_RING;	/* lost: the writer lapped the ring */
	n = h - t;
	for (unsigned long i = 0; i < n; i++)
		g->flushbuf[i] = g->ring[(t + i) & (XG_RING - 1)];
	unsigned long h2 = smp_load_acquire(&g->ring_head);
	if (h2 - t > XG_RING) skip = h2 - XG_RING - t;	/* overwritten while being copied */
	if (skip < n) {
		long w = fki_file_pwrite(g->log_file, g->flushbuf + skip, n - skip, g->log_off);
		if (w > 0) g->log_off += w;
	}
	g->ring_tail = h;
}

/* perf=1: per 2 s, each call kind's share of the interval in permille of one CPU, its count, and disk bytes */
static void acct_report(struct xnu_guest *g)
{
	unsigned long now = fki_counter(), dt = now - g->acct_last_t, total = 0;
	char buf[800]; char *p = buf;
	if (!g->acct_last_t) { g->acct_last_t = now; return; }
	g->acct_last_t = now;
	put_str(&p, "xnu: perf ms="); p = put_dec(p, dt * 1000 / fki_counter_frequency());
	for (unsigned b = 0; b < ACCT_N && p < buf + sizeof(buf) - 48; b++) {
		unsigned long t = 0, c = 0, d, n;
		for (int k = 0; k < g->ncpus; k++) {
			unsigned s = g->cpu[k].slot;
			t += READ_ONCE(xnu_slot_state[s].acct_ticks[b]); c += READ_ONCE(xnu_slot_state[s].acct_calls[b]);
		}
		d = t - g->acct_last_ticks[b]; n = c - g->acct_last_calls[b];
		g->acct_last_ticks[b] = t; g->acct_last_calls[b] = c;
		if (!n) continue;
		total += d;
		put_str(&p, b == ACCT_N - 1 ? " other" : " c"); if (b != ACCT_N - 1) p = put_dec(p, 800 + b);
		put_str(&p, "="); p = put_dec(p, d * 1000 / dt);
		put_str(&p, "%o/"); p = put_dec(p, n);
	}
	put_str(&p, " host="); p = put_dec(p, total * 1000 / dt);
	if (g->el0_stat) {	/* XNU's user-space exceptions by class: count and permille of the interval until back in user space */
		static const char *cls[LINUX64_EL0_CLASSES] = { " svc=", " dabt=", " iabt=", " irq=", " other=" };
		for (unsigned c = 0; c < LINUX64_EL0_CLASSES; c++) {
			unsigned long n = 0, t = 0;
			for (unsigned s = 0; s < LINUX64_EL0_CPUS; s++) {
				n += READ_ONCE(g->el0_stat[(s * 2 + 0) * LINUX64_EL0_CLASSES + c]);
				t += READ_ONCE(g->el0_stat[(s * 2 + 1) * LINUX64_EL0_CLASSES + c]);
			}
			put_str(&p, cls[c]); p = put_dec(p, n - g->el0_last_n[c]); put_str(&p, "/"); p = put_dec(p, (t - g->el0_last_t[c]) * 1000 / dt); put_str(&p, "%o");
			g->el0_last_n[c] = n; g->el0_last_t[c] = t;
		}
	}
	unsigned long dsk = 0;
	for (int k = 0; k < g->ncpus; k++) dsk += READ_ONCE(xnu_slot_state[g->cpu[k].slot].acct_disk);
	put_str(&p, "%o diskKB="); p = put_dec(p, (dsk - g->acct_last_disk) >> 10);
	g->acct_last_disk = dsk;
	*p = 0;
	glogs(g, buf);
}

static void monitor_round(struct xnu_guest *g)
{
	static const struct { int i; const char *name; } dec[] = {
		{ LINUX64_CTR_IDLE_THREAD, " idle_thread=" }, { LINUX64_CTR_MACHINE_IDLE, " machine_idle=" },
		{ LINUX64_CTR_CPU_IDLE, " cpu_idle=" }, { LINUX64_CTR_HOST_IDLE, " host_idle=" }, { LINUX64_CTR_SWITCHES, " switches=" },
		{ LINUX64_CTR_LAST_CALL, " last_call=" }, { LINUX64_CTR_RTCLOCK, " rtclock=" }, { LINUX64_CTR_EL0_EXC, " el0exc=" },
	}, hex[] = {
		{ LINUX64_CTR_EL0_ESR, " esr=0x" }, { LINUX64_CTR_EL0_FAR, " far=0x" }, { LINUX64_CTR_EL0_PC, " pc=0x" },
		{ LINUX64_CTR_BAD_ESR, " bad_esr=0x" }, { LINUX64_CTR_THREAD, " thread=0x" },
	};
	if (g->dbg_rw && (g->dump_req || (g->legacy && *(volatile int *)&dump_threads))) {
		g->dbg_rw[LINUX64_CTR_DUMP_REQ] = 1;
		g->dump_req = 0;
		if (g->legacy) dump_threads = 0;
	}
	/* a CPU in XNU that has not called the host for 2 s holds its Linux CPU with interrupts masked */
	for (int c = 0; c < g->ncpus && g->dbg; c++) {
		struct xg_cpu *pc = &g->cpu[c];
		struct xnu_slot *ss = &xnu_slot_state[pc->slot];
		if ((c && !pc->running) || ss->upcalls != pc->seen) { pc->seen = ss->upcalls; pc->still = 0; continue; }
		if (++pc->still == 1) {
			struct xnu_world *w = &pc->world;
			unsigned long fp = w->xnu_regs[10];
			char ab[200]; char *q = ab;
			put_str(&q, "xnu: XNU CPU "); q = put_dec(q, (unsigned long)c);
			put_str(&q, " made no host call for 2 s (its Linux CPU held, interrupts masked); last call ");
			q = put_dec(q, ss->last_call); put_str(&q, ss->in_call ? " (inside it)" : " (back in XNU)");
			*q = 0;
			glogs_alert(g, ab);
			glogkv(g, "xnu: XNU CPU silent for 2 s (no host call), cpu", (u64_t)c);
			glogkv(g, "xnu:   its last host call", ss->last_call);
			glogkv(g, "xnu:   still inside it (1) or back in XNU (0)", ss->in_call);
			glogkv(g, "xnu:   lr at that call", w->xnu_regs[11]);
			for (int f = 0; f < 16 && fp >= XNU_VA_LO && fp < XNU_VA_HI && !(fp & 15); f++) {
				unsigned long *fr = g->ttbr1 ? xg_kva_alias_n(g, fp, 16) : (unsigned long *)fp;	/* XNU's stack, through its own tables */
				if (!fr) break;
				glogkv(g, "xnu:   frame", fr[1]);
				if (fr[0] <= fp) break;
				fp = fr[0];
			}
		}
	}
	if ((perf || g->cfg.perf) && g->ncpus > 1) {	/* each XNU CPU's host calls so far and the last one's number */
		char cb[400]; char *q = cb;
		put_str(&q, "xnu: calls");
		for (int c = 0; c < g->ncpus; c++) {
			struct xnu_slot *ss = &xnu_slot_state[g->cpu[c].slot];
			put_str(&q, " "); q = put_dec(q, ss->upcalls); put_str(&q, "/"); q = put_dec(q, ss->last_call);
		}
		*q = 0;
		glogs(g, cb);
	}
	if (perf || g->cfg.perf)
		acct_report(g);
	if (!g->dbg || (g->mon_lines >= 90 && (++g->mon_ticks % 10) != 0))
		return;
	char buf[480]; char *p = buf;
	put_str(&p, "xnu: monitor");
	for (unsigned i = 0; i < sizeof(dec) / sizeof(dec[0]); i++) { put_str(&p, dec[i].name); p = put_dec(p, g->dbg[dec[i].i]); }
	put_str(&p, " kfaults="); p = put_dec(p, g->kfaults);
	put_str(&p, " moved="); p = put_dec(p, g->kfault_moved);
	put_str(&p, " syncs="); p = put_dec(p, fki_ttbr1_syncs(g->ttbr1));
	for (unsigned i = 0; i < sizeof(hex) / sizeof(hex[0]); i++) { put_str(&p, hex[i].name); put_hex(&p, g->dbg[hex[i].i]); }
	*p = 0;
	glogs(g, buf);
	g->mon_lines++;
}

static int monitor_fn(void *arg)
{
	struct xnu_guest *g = arg;
	unsigned long tick = 0;
	while (!fki_thread_should_stop()) {
		fki_msleep(250);
		tick++;
		xg_flush_log(g);
		if (g->state != XG_RUNNING)
			continue;
		if (g->ttbr1) g->syncs_snap = fki_ttbr1_syncs(g->ttbr1);
		if ((tick % 8) == 0)
			monitor_round(g);
		if ((tick % 40) == 0 && g->ttbr1) {	/* every 10 s: the private root against Linux's tables */
			long r = fki_ttbr1_audit(g->ttbr1);
			g->audits++;
			g->audit_last = r;
			g->audit_total += r > 0 ? r : 0;
			if (r) glogkv(g, "xnu: private root audit found differences", (u64_t)r);
		}
	}
	xg_flush_log(g);
	return 0;
}

/* The framebuffer to fb_dump every fb_dump_ms (a host thread: plain Linux context, XNU keeps running on its CPU). */
static int fb_fn(void *arg)
{
	struct xnu_guest *g = arg;
	void *f = fki_file_open(g->cfg.fb_dump, 1, 0);
	if (!f) { glogs(g, "xnu: cannot open fb_dump (create the file first)"); }
	while (!fki_thread_should_stop()) {
		fki_msleep(g->cfg.fb_dump_ms > 0 ? g->cfg.fb_dump_ms : 1000);
		if (f && g->fb_off && g->img.region.va && g->state == XG_RUNNING)
			fki_file_pwrite(f, (unsigned char *)g->img.region.va + g->fb_off, FB_SIZE(g), 0);
	}
	if (f) fki_file_close(f);
	return 0;
}

static int enter_arm_init(struct xnu_guest *g, struct boot_args_v2 *ba)
{
	struct xnu_image *img = &g->img;
	if (img->filetype == MH_FILESET) {
		/* kernel collection at its link address: XNU applies the chained fixups itself; nothing slides */
		if (img->slide) { glogs(g, "xnu: a kernel collection must not slide"); return -22; }
		glogs(g, "xnu: kernel collection: XNU applies its chained fixups (kernel_collection_slide)");
	} else {
		struct fki_pac_keys saved; unsigned long c = fki_enter(&g->cpu[0].world.xnu_keys, &saved);
		int n = macho_rebase(img);
		fki_leave(c, &saved);
		glogkv(g, "xnu: threaded fixups applied", (u64_t)n);
		if (n < 0) return n;

		u64_t mh_unslid = 0;
		int segs = macho_slide_header(img, &mh_unslid);
		glogkv(g, "xnu: load commands slid (segments)", (u64_t)segs);
	}
	u64_t a_init = macho_sym(img, "_arm_init"), a_hook = macho_sym(img, "_linux64_hook"),
	      a_slide = macho_sym(img, "_linux64_slide");
	glogkv(g, "xnu: arm_init at", a_init);
	if (!a_init || !a_hook || !a_slide) { glogs(g, "xnu: arm_init or linux64_hook missing (kernel without the LINUX64 patch?)"); return -2; }
	u64_t *hook_p = xg_va_alias(g, a_hook), *slide_p = xg_va_alias(g, a_slide);
	if (!hook_p || !slide_p) { glogs(g, "xnu: linux64_hook outside the bootstrap window"); return -22; }
	*hook_p = (u64_t)xnu_world_upcall;
	*slide_p = img->slide;

	g->diag_va[0] = a_init; g->diag_va[1] = img->vmlow; g->diag_va[2] = g->ba_xnu; g->diag_va[3] = (u64_t)ba->deviceTreeP;
	g->diag_va[4] = macho_sym(img, "_port_crumb");
	g->diag_va[5] = macho_sym(img, "_strlen");
	u64_t dbg = macho_sym(img, "_linux64_dbg"), el0 = macho_sym(img, "_linux64_el0_stat");
	g->dbg_rw = dbg ? xg_va_alias(g, dbg) : 0;
	g->dbg = g->dbg_rw;
	g->el0_stat = el0 ? xg_va_alias(g, el0) : 0;
	g->kfault = (void *)macho_sym(img, "_linux64_kernel_fault");
	g->kfault_recover = (void *)macho_sym(img, "_linux64_kernel_fault_recover");
	if (g->kfault)
		glogs(g, "xnu: kernel faults on XNU addresses go to XNU's vm_fault");
	{
		/*
		 * XNU's user half starts as an empty table (fki_empty_ttbr0). Its TCR is Linux's with TCR.A1 off in XNU's world:
		 * Linux takes the ASID from TTBR1 (A1 = 1), XNU's pmaps put theirs in TTBR0. With Linux's A1 every XNU user pmap
		 * ran under Linux's one ASID, so XNU's per-ASID TLB flushes missed and a freed pmap's cached table walk (the
		 * exec'd child's stack L2) served the parent: a translation fault that repeats after vm_fault. fki drops the
		 * local TLB at each switch, so the two ASID spaces never mix.
		 */
		unsigned long long tcr;
		__asm__ volatile("mrs %0, tcr_el1" : "=r"(tcr));
		struct xnu_world *w0 = &g->cpu[0].world;
		w0->xnu_tcr = tcr & ~(1ULL << 22);
		if (g_s == 14)
			/*
			 * 16 KB granule: XNU's user pmaps are 3-level from a 2048-entry L1 root over 47 bits (T0SZ 17, TG0 16 KB, as
			 * Linux's TG0 already is), not Linux's 4-level T0SZ 16.
			 */
			w0->xnu_tcr = (tcr & ~((1ULL << 22) | 0x3fULL)) | 17ULL;
		w0->xnu_ttbr0 = fki_empty_ttbr0();
		glogkv(g, "xnu: Linux TCR_EL1", tcr);
		glogkv(g, "xnu: XNU TCR_EL1", w0->xnu_tcr);
		glogkv(g, "xnu:   T0SZ", w0->xnu_tcr & 0x3f);
		glogkv(g, "xnu:   TG0 (0: 4K, 2: 16K)", (w0->xnu_tcr >> 14) & 3);
		glogkv(g, "xnu: XNU first TTBR0", w0->xnu_ttbr0);
	}
	g->a_cpu_entry = macho_sym(img, "_linux64_cpu_entry");
	for (int c = 1; c < g->ncpus; c++) {	/* the secondary CPUs' save areas start as the boot CPU's: XNU's keys, user TTBR0/TCR */
		g->cpu[c].world.xnu_keys = g->cpu[0].world.xnu_keys;
		g->cpu[c].world.xnu_tcr = g->cpu[0].world.xnu_tcr;
		g->cpu[c].world.xnu_ttbr0 = g->cpu[0].world.xnu_ttbr0;
	}
	if (g->ncpus > 1 && !g->a_cpu_entry) { glogs(g, "xnu: kernel without linux64_cpu_entry: its other CPUs stay unused"); }
	/* the boot CPU really is the guest's CPU 0 (its save area and slot): XNU's MPIDR checks depend on it */
	if (this_slot() != g->cpu[0].slot || g->cfg.test_boot_mismatch) {
		glogkv(g, "xnu: the boot thread does not run on the guest's boot CPU (MPIDR slot), refusing; slot", this_slot());
		return -22;
	}
	glogs(g, "xnu: entering arm_init");
	g->state = XG_RUNNING;
	IN_XNU = 1;
	long r = xnu_world_call(a_init, (unsigned long)g->ba_xnu);
	IN_XNU = 0;
	glogkv(g, r ? "xnu: run ended by stage hook, r =" : "xnu: arm_init returned (unexpected), r =", (u64_t)r);
	return 0;
}

/* M2a: the image in a vmalloc region with synthetic physical addresses (no private root). */
static int run_m2a(struct xnu_guest *g)
{
	u64_t image_size = g->img.vmhigh - g->img.vmlow;
	struct boot_args_v2 *ba = build_boot(g, g->img.region.va, (u64_t)g->img.region.va, XNU_FAKE_PHYS, g->img.region.size, 0, image_size);
	if (!ba) return -22;
	return enter_arm_init(g, ba);
}

/*
 * M2b: XNU at its own link address in the guest's private root, on physically contiguous memory with real physical
 * addresses, so that the page tables XNU builds can be walked by the hardware. The order: everything that allocates
 * kernel memory (threads, blobs, files, the link, the host call channel, the DRAM slice) before the root is copied from
 * Linux's tables, so the root has their mappings from the start (and syncs stays 0); then the root, the CPUs onto it, the
 * bootstrap window, the image and boot data through the Linux alias, arm_init.
 */
static int xg_map_window(struct xnu_guest *g)
{
	u64_t image_size = g->img.vmhigh - g->img.vmlow, image_off = g->img.vmlow - g->window_base;
	unsigned long long *l1; fki_u64 l1pa;
	int err = fki_ttbr1_window_l1(g->ttbr1, g->window_base, &l1, &l1pa);
	if (err) { glogs(g, "xnu: no L1 table in the root for the top window"); return err; }
	g->root_l1 = l1;
	unsigned idx = (unsigned)((g->window_base >> g_l1s) & (g_n - 1));
	unsigned first = (unsigned)((g->window_base >> g_l2s) & (g_n - 1));
	u64_t slot_va = g->window_base & ~((1ULL << g_l1s) - 1);
	/* XNU's L2 entries in the window's L1 slot: up to VM_MAX_KERNEL_ADDRESS (16 KB: [0, 1536) of 0xfffffff000000000's) */
	unsigned hi = g_n;
	if (XNU_VA_HI - slot_va < ((u64_t)g_n << g_l2s)) hi = (unsigned)((XNU_VA_HI - slot_va) >> g_l2s);
	/*
	 * The bootstrap mapping: L2 blocks over what fits in the window's L1 slot. XNU only touches the image, the boot data and
	 * its first bootstrap allocations before it hands us its own kernel L1 entries (LINUX64_CALL_KERNEL_TABLES), which map
	 * all of its DRAM, so DRAM beyond the slot needs no bootstrap mapping.
	 */
	u64_t boot_map = g->img.region.size & ~(g_block - 1);
	if (first >= hi) { glogs(g, "xnu: the image is above XNU's kernel VA range"); return -22; }
	if (boot_map > ((u64_t)(hi - first) << g_l2s)) boot_map = (u64_t)(hi - first) << g_l2s;
	if (boot_map < image_off + image_size + 0x60000 + BOOT_EXTRA(g) + (64ULL << 20)) {
		glogs(g, "xnu: image and boot data do not fit in the bootstrap window"); return -22;
	}
	glogkv(g, "xnu: translation granule shift", g_s);
	glogkv(g, "xnu: bootstrap window base", g->window_base);
	glogkv(g, "xnu: bootstrap window bytes", boot_map);
	glogkv(g, "xnu: bootstrap L2 block bytes", g_block);
	glogkv(g, "xnu: private root's L1 table at phys", l1pa);
	glogkv(g, "xnu: L1 slot", idx);
	/* L2 block: normal WB (AttrIdx 0 in Linux's MAIR), inner shareable, AF, EL1 RWX, UXN */
#define BOOT_BLOCK(o) ((g->img.region.pa + (o)) | (1ULL << 54) | (1ULL << 10) | (3ULL << 8) | 1)
	if (p_valid && idx == p_idx) {
		/* the shared slot (16 KB granule): XNU's top 48 GiB share a 64 GiB slot with Linux's PCI I/O and fixmap; the root's
		 * own L2 table for it has Linux's entries above, the bootstrap blocks go below */
		unsigned long long *l2; fki_u64 l2pa;
		err = fki_ttbr1_window_l2(g->ttbr1, g->window_base, &l2, &l2pa);
		if (err) { glogkv(g, "xnu: no bootstrap L2 table in the root, err", (u64_t)(long)err); return err; }
		for (u64_t o = 0; o < boot_map; o += g_block) {
			err = fki_ttbr1_set(g->ttbr1, &l2[first + (o >> g_l2s)], BOOT_BLOCK(o));
			if (err) { glogkv(g, "xnu: bootstrap L2 entry refused, index", first + (o >> g_l2s)); return err; }
			g->boot_count++;
		}
		glogkv(g, "xnu: bootstrap window mapped in the root's shared-slot L2 table, blocks", g->boot_count);
		glogkv(g, "xnu:   from L2 index", first);
	} else if (idx >= w_first && idx < w_end && g->l2page.va) {
		/* a slot of the guest's own (4 KB granule: 1 GiB): a bootstrap L2 table of ours */
		unsigned long long *l2 = g->l2page.va;
		for (u64_t o = 0; o < boot_map; o += g_block)
			l2[first + (o >> g_l2s)] = BOOT_BLOCK(o);
		err = fki_ttbr1_set(g->ttbr1, &l1[idx], g->l2page.pa | 3);
		if (err) { glogs(g, "xnu: the root refused the bootstrap L2 table"); return err; }
		glogs(g, "xnu: bootstrap window mapped (own L2 table)");
	} else {
		glogkv(g, "xnu: the window's L1 slot is outside the guest's root, slot", idx);
		return -16;
	}
#undef BOOT_BLOCK
	return 0;
}

static void xg_teardown(struct xnu_guest *g);

static int xg_open(struct xnu_guest *g, const char *path, int rw, void **f, fki_u64 *sz, const char *what, int required)
{
	if (!path[0]) return 0;
	fki_u64 s = 0;
	*f = fki_file_open(path, rw, &s);
	if (!*f) {
		char m[120]; char *p = m;
		put_str(&p, "xnu: cannot open the "); put_str(&p, what); *p = 0;
		glogs(g, m);
		return required ? -2 : 0;
	}
	if (sz) *sz = s;
	return 0;
}

/* In the guest's boot thread (bound to its CPU 0): set up, run, tear down. */
static int xg_run(struct xnu_guest *g)
{
	struct xg_cfg *c = &g->cfg;
	const void *data; unsigned long size;
	int err = 0;

	g->t_start = fki_counter();
	/* 1. threads first: the secondary CPUs' (waiting for XNU to start them), the monitor's, the framebuffer's */
	for (int k = 1; k < g->ncpus && g->mode != 1; k++) {
		g->cpu[k].start_arg = 0;
		g->cpu[k].thread = fki_thread_start(cpu_fn, &g->cpu[k], g->cpu[k].linux_cpu, "xnu-cpu");
		if (!g->cpu[k].thread) { glogkv(g, "xnu: cannot make a thread for Linux CPU", (u64_t)g->cpu[k].linux_cpu); err = -12; goto out; }
	}
	if (g->monitor_cpu >= 0) {
		char *p = g->mon_name; put_str(&p, "xnu-monitor"); if (g->id) p = put_dec(p, g->id); *p = 0;
		g->monitor = fki_thread_start(monitor_fn, g, g->monitor_cpu, g->mon_name);
	}
	if (c->fb_dump[0] && FB_SIZE(g) && c->fb_dump_ms > 0) {
		char *p = g->fb_name; put_str(&p, "xnu-fb"); if (g->id) p = put_dec(p, g->id); *p = 0;
		g->fb_thread = fki_thread_start(fb_fn, g, g->monitor_cpu >= 0 ? g->monitor_cpu : g->cpu[0].linux_cpu, g->fb_name);
	}
	/* 2. blobs and files */
	if (c->ramdisk[0]) {
		err = fki_blob_get(c->ramdisk, &g->rd_data, &g->rd_size, &g->rd_blob);
		if (err) { glogkv(g, "xnu: cannot read the RAM disk, err", (u64_t)err); goto out; }
	}
	if (c->trustcache[0]) {
		err = fki_blob_get(c->trustcache, &g->tc_data, &g->tc_size, &g->tc_blob);
		if (err) { glogkv(g, "xnu: cannot read the trust cache, err", (u64_t)err); goto out; }
	}
	xg_open(g, c->console_in, 0, &g->ci_file, &g->ci_off, "console_in", 0);	/* only what is written after the start */
	xg_open(g, c->keystore, 1, &g->ks_file, 0, "keystore file", 0);
	if (!xg_open(g, c->input_in, 0, &g->in_file, &g->in_off, "input_in", 0)) g->in_off &= ~7ULL;
	if ((err = xg_open(g, c->disk, 1, &g->dk_file, &g->dk_size, "block disk", 1))) goto out;
	if (g->dk_file) glogkv(g, "xnu: block disk bytes", g->dk_size);
	if ((err = xg_open(g, c->hostdisk, c->hostdisk_rw, &g->hd_file, &g->hd_size, "host disk", 1))) goto out;
	if (g->hd_file) glogkv(g, "xnu: host disk bytes", g->hd_size);
	xg_open(g, c->console_log, 1, &g->log_file, &g->log_off, "console_log (create the file first)", 0);
	err = fki_blob_get(c->image, &data, &size, &g->blob);
	if (err) { glogkv(g, "xnu: cannot read image, err", (u64_t)err); g->blob = 0; goto out; }
	err = macho_load(&g->img, data, size, g->mode == 3 ? ~0UL : g->mode == 2 ? (unsigned long)c->ram_mb << 20 : 0);
	if (err) { glogkv(g, "xnu: load failed", (u64_t)err); g->img.region.va = 0; goto out; }
	glogkv(g, "xnu: image vmlow", g->img.vmlow);
	glogkv(g, "xnu: image size", g->img.vmhigh - g->img.vmlow);
	fki_random(&g->cpu[0].world.xnu_keys, sizeof(g->cpu[0].world.xnu_keys));
	g->cpu[0].world.xnu_keys.sctlr_en = ~0ULL;
	if (g->mode == 1) { err = run_m1(g); goto out; }
	/* 3. the link and the host call channel */
	if (c->net[0]) {
		g->net = fki_net_create(c->net, g->cpu[0].linux_cpu);
		if (!g->net) { glogs(g, "xnu: the guest's link is missing or in use by another guest"); err = -16; goto out; }
	}
	g->hc = fki_hostcall_create(g->id, &err);
	if (!g->hc) { glogkv(g, "xnu: no host call channel, err", (u64_t)(long)err); err = 0; }
	if (g->mode == 2) {
		glogkv(g, "xnu: region at", (u64_t)g->img.region.va);
		glogkv(g, "xnu: region size", g->img.region.size);
		for (int k = 0; k < g->ncpus; k++) {
			struct fki_cpu_cfg cc = { 0, xnu_fault, g, XNU_VA_LO, XNU_VA_HI };
			if ((err = fki_cpu_attach(g->cpu[k].linux_cpu, &cc))) { glogkv(g, "xnu: fki_cpu_attach failed, err", (u64_t)(long)err); goto out; }
			g->attached[k] = 1;
		}
		err = run_m2a(g);
		goto out;
	}
	/* 4. the DRAM slice */
	u64_t image_size = g->img.vmhigh - g->img.vmlow;
	if (g->img.vmlow < XNU_VA_LO || g->img.vmhigh > XNU_VA_HI) { glogs(g, "xnu: unexpected link address"); err = -22; goto out; }
	g->window_base = g->img.vmlow & ~(g_block - 1);
	u64_t image_off = g->img.vmlow - g->window_base;
	u64_t want = image_off + image_size + 0x60000 + BOOT_EXTRA(g) + ((u64_t)c->ram_mb << 20);
	err = fki_contig_alloc(want, &g->img.region);
	if (err) { g->img.region.va = 0; glogkv(g, "xnu: contiguous allocation failed, err", (u64_t)(long)err); goto out; }
	glogkv(g, "xnu: contiguous DRAM at phys", g->img.region.pa);
	glogkv(g, "xnu: contiguous DRAM size", g->img.region.size);
	if (g->img.region.pa & (g_block - 1)) { glogkv(g, "xnu: region not aligned to the L2 block size", g_block); err = -22; goto out; }
	g->linear_size = g->img.region.size;
	g->alias_off = (u64_t)g->img.region.va - g->window_base;
	{
		unsigned idx = (unsigned)((g->window_base >> g_l1s) & (g_n - 1));
		if (!(p_valid && idx == p_idx)) {	/* 4 KB: the bootstrap L2 table, a page of our own */
			err = fki_contig_alloc(g_page, &g->l2page);
			if (err) { g->l2page.va = 0; goto out; }
		}
	}
	/* 5. the guest's private root, then its CPUs onto it */
	g->ttbr1 = fki_ttbr1_create(root_lo, XNU_VA_HI, &err);
	if (!g->ttbr1) { glogkv(g, "xnu: no private root for the guest, err", (u64_t)(long)err); goto out; }
	for (int k = 0; k < g->ncpus; k++) {
		struct fki_cpu_cfg cc = { g->ttbr1, xnu_fault, g, XNU_VA_LO, XNU_VA_HI };
		err = fki_cpu_attach(g->cpu[k].linux_cpu, &cc);
		if (err) { glogkv(g, "xnu: fki_cpu_attach failed for Linux CPU", (u64_t)g->cpu[k].linux_cpu); goto out; }
		g->attached[k] = 1;
	}
	/* 6. the bootstrap window in the root */
	err = xg_map_window(g);
	if (err) goto out;
	/* 7. the image and the boot data, through the Linux alias */
	unsigned char *base = g->img.region.va;
	macho_copy(&g->img, base + image_off);
	g->img.slide = 0;
	{	/* the granule XNU was built for: its page tables are the hardware's, so it has to be the host's */
		u64_t gs = macho_sym(&g->img, "_linux64_granule_shift");
		volatile unsigned int *gp = gs ? xg_va_alias(g, gs) : 0;
		if (gp) {
			unsigned v = *gp;
			glogkv(g, "xnu: XNU built for granule shift", v);
			if (v != g_s) { glogs(g, "xnu: XNU was built for another translation granule than the host's: refusing"); err = -22; goto out; }
		} else if (g_s != 12) {
			glogs(g, "xnu: XNU without linux64_granule_shift (a 4 KB build) on a 16 KB host: refusing (IMAGE=xnu/kc16k.bin)");
			err = -22; goto out;
		}
	}
	struct boot_args_v2 *ba = build_boot(g, base, g->window_base, g->img.region.pa, g->img.region.size, image_off, image_size);
	if (!ba) { err = -22; goto out; }
	g->static_size = ba->topOfKernelData - g->img.region.pa;
	/* 8. hooks, keys, arm_init */
	err = enter_arm_init(g, ba);
out:
	g->result = err;
	xg_teardown(g);
	return err;
}

/*
 * Everything the start set up, in reverse. A secondary CPU that does not leave XNU within 5 s still runs on the guest's
 * root and memory: then nothing is freed (the guest is a zombie, the module stays loaded).
 */
static void xg_teardown(struct xnu_guest *g)
{
	int stuck = 0;
	g->state = XG_STOPPING;
	g->stop = 1;		/* the other CPUs end at their next call into the host (kicked out of idle) */
	for (int t = 0; t < 500; t++) {
		stuck = 0;
		for (int c = 1; c < g->ncpus; c++)
			if (g->cpu[c].running) { stuck = 1; fki_kick_cpu(g->cpu[c].linux_cpu); }
		if (!stuck) break;
		fki_msleep(10);
	}
	if (stuck) {
		for (int c = 1; c < g->ncpus; c++)
			if (g->cpu[c].running) glogkv(g, "xnu: CPU did not leave XNU in 5 s:", (u64_t)g->cpu[c].linux_cpu);
		glogs_alert(g, "xnu: guest left as a zombie: its CPUs, memory and root stay");
		fki_thread_stop(g->monitor); g->monitor = 0;
		fki_thread_stop(g->fb_thread); g->fb_thread = 0;
		if (!g->holds_ref) { __module_get(THIS_MODULE); g->holds_ref = 1; }
		g->state = XG_ZOMBIE;
		return;
	}
	for (int c = 1; c < g->ncpus; c++) {
		fki_thread_stop(g->cpu[c].thread);
		g->cpu[c].thread = 0;
	}
	for (int c = 0; c < g->ncpus; c++)
		if (g->attached[c]) { fki_cpu_detach(g->cpu[c].linux_cpu); g->attached[c] = 0; }
	xg_lock();
	for (int c = 0; c < g->ncpus; c++) {
		unsigned s = g->cpu[c].slot;
		if (xnu_guest_of_slot[s] == g) {
			xnu_worlds[s] = 0;
			xnu_guest_of_slot[s] = 0;
			xnu_linux_cpu_of_slot[s] = -1;
		}
	}
	xg_unlock();
	glogkv(g, "xnu: kernel faults forwarded to XNU", g->kfaults);
	if (g->ttbr1) { g->syncs_snap = fki_ttbr1_syncs(g->ttbr1); glogkv(g, "xnu: private root lazy syncs", g->syncs_snap); }
	glogs(g, "xnu: guest stopped");
	fki_thread_stop(g->monitor); g->monitor = 0;	/* its last round writes the log out */
	fki_thread_stop(g->fb_thread); g->fb_thread = 0;
	if (g->ttbr1) { struct fki_ttbr1 *t = g->ttbr1; g->ttbr1 = 0; fki_ttbr1_destroy(t); }	/* before the memory XNU's tables are in */
	g->root_l1 = 0;
	if (g->mode == 3) {
		if (g->img.region.va) fki_contig_free(&g->img.region);
		if (g->l2page.va) fki_contig_free(&g->l2page);
	} else if (g->img.region.va) fki_region_free(&g->img.region);
	g->img.region.va = 0;
	g->l2page.va = 0;
	g->dbg = 0; g->dbg_rw = 0; g->el0_stat = 0;
	if (g->net) { fki_net_release(g->net); g->net = 0; }
	if (g->hc) { fki_hostcall_release(g->hc); g->hc = 0; }
	if (g->hd_file) { fki_file_close(g->hd_file); g->hd_file = 0; }
	if (g->ks_file) { fki_file_close(g->ks_file); g->ks_file = 0; }
	if (g->dk_file) { fki_file_close(g->dk_file); g->dk_file = 0; }
	if (g->ci_file) { fki_file_close(g->ci_file); g->ci_file = 0; }
	if (g->in_file) { fki_file_close(g->in_file); g->in_file = 0; }
	if (g->log_file) { fki_file_close(g->log_file); g->log_file = 0; }
	if (g->blob) { fki_blob_put(g->blob); g->blob = 0; }
	if (g->rd_blob) { fki_blob_put(g->rd_blob); g->rd_blob = 0; g->rd_size = 0; }
	if (g->tc_blob) { fki_blob_put(g->tc_blob); g->tc_blob = 0; g->tc_size = 0; }
	xg_lock();
	for (int c = 0; c < g->ncpus; c++)
		if (g->cpu[c].linux_cpu >= 0 && g->cpu[c].linux_cpu < XNU_LINUX_CPUS && xnu_cpu_owner[g->cpu[c].linux_cpu] == g)
			xnu_cpu_owner[g->cpu[c].linux_cpu] = 0;
	xg_unlock();
	g->state = XG_STOPPED;
	if (g->holds_ref) { g->holds_ref = 0; module_put(THIS_MODULE); }
}

/*
 * The guest's CPUs: online, not the host's, not another guest's, each with an MPIDR slot no other CPU has. Taken (owner,
 * slot, save area) all at once or not at all. strict=0 (guest 0 of insmod): stop at the first CPU that fails, as before.
 */
static int xg_claim(struct xnu_guest *g, int strict)
{
	int n = 0, err = 0;
	xg_lock();
	for (int k = 0; k < g->cfg.ncpus && k < XNU_MAX_CPUS; k++) {
		int cpu = g->cfg.cpu[k];
		fki_u64 m = 0;
		unsigned s;
		if (cpu < 0 || cpu >= XNU_LINUX_CPUS) { err = -22; glogkv(g, "xnu: no such online Linux CPU:", (u64_t)cpu); }
		else if (host_cpu[cpu]) { err = -16; glogkv(g, "xnu: Linux CPU is one of host_cpus, refusing:", (u64_t)cpu); }
		else if (xnu_cpu_owner[cpu] && xnu_cpu_owner[cpu] != g) { err = -16; glogkv(g, "xnu: Linux CPU belongs to another guest, refusing:", (u64_t)cpu); }
		else if (fki_cpu_mpidr(cpu, &m)) { err = -22; glogkv(g, "xnu: no such online Linux CPU:", (u64_t)cpu); }
		else {
			s = LINUX64_CPU_SLOT(m);
			if (xnu_guest_of_slot[s] || xnu_worlds[s]) { err = -16; glogkv(g, "xnu: MPIDR slot busy, refusing Linux CPU", (u64_t)cpu); }
			for (int j = 0; j < n && !err; j++)
				if (g->cpu[j].slot == s || g->cpu[j].linux_cpu == cpu) { err = -16; glogkv(g, "xnu: two CPUs with one MPIDR slot, refusing Linux CPU", (u64_t)cpu); }
			if (!err) {
				struct xg_cpu *pc = &g->cpu[n];
				pc->g = g; pc->idx = n; pc->linux_cpu = cpu; pc->slot = s; pc->mpidr = m & 0xffff;	/* XNU compares Aff1:Aff0 */
				n++;
				continue;
			}
		}
		if (!strict && n > 0) { glogkv(g, "xnu: no distinct MPIDR slot for Linux CPU, using CPUs below it:", (u64_t)cpu); err = 0; }
		break;
	}
	if (!err && !n) err = -22;
	if (!err) {
		g->ncpus = n;
		for (int k = 0; k < n; k++) {
			struct xg_cpu *pc = &g->cpu[k];
			xnu_cpu_owner[pc->linux_cpu] = g;
			xnu_guest_of_slot[pc->slot] = g;
			xnu_linux_cpu_of_slot[pc->slot] = (short)pc->linux_cpu;
			xnu_slot_state[pc->slot] = (struct xnu_slot){ 0 };
			xnu_worlds[pc->slot] = &pc->world;
		}
	}
	xg_unlock();
	return err;
}

static void xg_unclaim(struct xnu_guest *g)
{
	xg_lock();
	for (int k = 0; k < g->ncpus; k++) {
		struct xg_cpu *pc = &g->cpu[k];
		if (xnu_guest_of_slot[pc->slot] == g) { xnu_worlds[pc->slot] = 0; xnu_guest_of_slot[pc->slot] = 0; xnu_linux_cpu_of_slot[pc->slot] = -1; }
		if (pc->linux_cpu >= 0 && pc->linux_cpu < XNU_LINUX_CPUS && xnu_cpu_owner[pc->linux_cpu] == g) xnu_cpu_owner[pc->linux_cpu] = 0;
	}
	xg_unlock();
}

/* the host CPU a guest's helper threads run on: the asked one, else the first host CPU, else the first CPU no guest has */
static int xg_pick_monitor(struct xnu_guest *g, int asked)
{
	if (asked >= 0 || asked == -1) return asked;
	for (int i = 0; i < host_cpu_n; i++) { fki_u64 m; if (!fki_cpu_mpidr(host_cpu_list[i], &m)) return host_cpu_list[i]; }
	for (int cpu = 0; cpu < fki_num_cpus() && cpu < XNU_LINUX_CPUS; cpu++) {
		int mine = 0;
		for (int k = 0; k < g->ncpus; k++) if (g->cpu[k].linux_cpu == cpu) mine = 1;
		if (!mine && !xnu_cpu_owner[cpu]) return cpu;
	}
	return -1;
}

static void xg_reset(struct xnu_guest *g, int id)
{
	unsigned char *p = (unsigned char *)g;
	for (unsigned long i = 0; i < sizeof(*g); i++) p[i] = 0;
	g->id = id;
	for (int k = 0; k < XNU_MAX_CPUS; k++) g->cpu[k].linux_cpu = -1;
}

static void cfg_str(char *dst, unsigned long cap, const char *s)
{
	unsigned long n = s ? x_strlen(s) : 0;
	if (n >= cap) n = cap - 1;
	if (n) x_memcpy(dst, s, n);
	dst[n] = 0;
}

static void xg_cfg_defaults(struct xg_cfg *c, int id)
{
	unsigned char *p = (unsigned char *)c;
	for (unsigned long i = 0; i < sizeof(*c); i++) p[i] = 0;
	cfg_str(c->image, sizeof(c->image), g_s == 14 ? "xnu/kc16k.bin" : "xnu/kc.bin");
	cfg_str(c->model, sizeof(c->model), model ? model : "VirtualMac2,1");
	char nb[16] = "xnu"; char *q = nb + 3; q = put_dec(q, (unsigned long)id); *q = 0;
	cfg_str(c->net, sizeof(c->net), nb);
	c->ram_mb = 7900; c->mac_id = id; c->fb_width = 0; c->fb_height = 0; c->fb_dump_ms = 1000; c->vm16k = 1;
	c->quiet_console = 1; c->monitor_cpu = -2;
}

/* ---- the control interface: /sys/kernel/xnu/{ctl,status} ---- */
static int parse_num(const char *s, long *v)
{
	long r = 0; int neg = 0, base = 10, any = 0;
	if (*s == '-') { neg = 1; s++; }
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
	for (; *s; s++) {
		int d = *s >= '0' && *s <= '9' ? *s - '0' : *s >= 'a' && *s <= 'f' ? *s - 'a' + 10 : *s >= 'A' && *s <= 'F' ? *s - 'A' + 10 : 99;
		if (d >= base) return -22;
		r = r * base + d; any = 1;
	}
	if (!any) return -22;
	*v = neg ? -r : r;
	return 0;
}

/* "1-3,5" */
static int parse_cpus(const char *s, int *list, int max, int *n)
{
	*n = 0;
	while (*s) {
		char a[12], b[12]; int i = 0, j = 0; long lo, hi;
		while (*s && *s != ',' && *s != '-' && i < 11) a[i++] = *s++;
		a[i] = 0;
		if (parse_num(a, &lo)) return -22;
		hi = lo;
		if (*s == '-') {
			s++;
			while (*s && *s != ',' && j < 11) b[j++] = *s++;
			b[j] = 0;
			if (parse_num(b, &hi)) return -22;
		}
		if (hi < lo || lo < 0) return -22;
		for (long c = lo; c <= hi; c++) { if (*n >= max) return -7; list[(*n)++] = (int)c; }
		if (*s == ',') s++;
		else if (*s) return -22;
	}
	return *n ? 0 : -22;
}

static char ctl_key[32], ctl_val[BOOT_LINE];
static struct xg_cfg ctl_cfg;

/* the next key[=value] token (value in double quotes may hold spaces); 0 at the end */
static int next_token(const char **sp)
{
	const char *s = *sp;
	unsigned i = 0;
	while (*s == ' ' || *s == '\t' || *s == '\n') s++;
	if (!*s) return 0;
	while (*s && *s != '=' && *s != ' ' && *s != '\t' && *s != '\n' && i < sizeof(ctl_key) - 1) ctl_key[i++] = *s++;
	ctl_key[i] = 0;
	i = 0;
	if (*s == '=') {
		s++;
		if (*s == '"') {
			s++;
			while (*s && *s != '"' && i < sizeof(ctl_val) - 1) ctl_val[i++] = *s++;
			if (*s == '"') s++;
		} else {
			while (*s && *s != ' ' && *s != '\t' && *s != '\n' && i < sizeof(ctl_val) - 1) ctl_val[i++] = *s++;
		}
	}
	ctl_val[i] = 0;
	*sp = s;
	return 1;
}

static int xg_boot_fn(void *arg)
{
	struct xnu_guest *g = arg;
	xg_run(g);
	while (!fki_thread_should_stop())	/* until reaped */
		fki_msleep(20);
	return 0;
}

static long ctl_start(const char *s)
{
	struct xg_cfg *c = &ctl_cfg;
	long id = -1, v;
	const char *t = s;
	/* the id first: the defaults depend on it */
	while (next_token(&t))
		if (!x_strcmp(ctl_key, "id")) { if (parse_num(ctl_val, &id)) return -22; }
	if (id < 0) {
		for (int i = 0; i < XNU_MAX_GUESTS; i++) if (xnu_guest_pool[i].state == XG_FREE) { id = i; break; }
		if (id < 0) return -28;
	}
	if (id >= XNU_MAX_GUESTS) return -22;
	xg_cfg_defaults(c, (int)id);
	t = s;
	while (next_token(&t)) {
		const char *k = ctl_key, *val = ctl_val;
		struct { const char *k; char *dst; unsigned long cap; } strs[] = {
			{ "image", c->image, sizeof(c->image) }, { "disk", c->disk, sizeof(c->disk) },
			{ "keystore", c->keystore, sizeof(c->keystore) }, { "console_in", c->console_in, sizeof(c->console_in) },
			{ "console_log", c->console_log, sizeof(c->console_log) }, { "input_in", c->input_in, sizeof(c->input_in) },
			{ "fb_dump", c->fb_dump, sizeof(c->fb_dump) }, { "hostdisk", c->hostdisk, sizeof(c->hostdisk) },
			{ "ramdisk", c->ramdisk, sizeof(c->ramdisk) }, { "trustcache", c->trustcache, sizeof(c->trustcache) },
			{ "model", c->model, sizeof(c->model) }, { "net", c->net, sizeof(c->net) },
			{ "root_uuid", c->root_uuid, sizeof(c->root_uuid) }, { "boot_uuid", c->boot_uuid, sizeof(c->boot_uuid) },
			{ "preboot_uuid", c->preboot_uuid, sizeof(c->preboot_uuid) }, { "bootargs", c->bootargs, sizeof(c->bootargs) },
		};
		struct { const char *k; int *dst; } ints[] = {
			{ "ram_mb", &c->ram_mb }, { "mac_id", &c->mac_id }, { "fb_width", &c->fb_width }, { "fb_height", &c->fb_height },
			{ "fb_dump_ms", &c->fb_dump_ms }, { "hostdisk_rw", &c->hostdisk_rw }, { "vm16k", &c->vm16k }, { "kmax", &c->kmax },
			{ "idle_max", &c->idle_max }, { "quiet_console", &c->quiet_console }, { "assert_fatal", &c->assert_fatal },
			{ "park_secondaries", &c->park_secondaries }, { "monitor_cpu", &c->monitor_cpu }, { "perf", &c->perf },
			{ "test_boot_mismatch", &c->test_boot_mismatch },
		};
		int done = 0;
		if (!x_strcmp(k, "id")) continue;
		if (!x_strcmp(k, "cpus")) { if (parse_cpus(val, c->cpu, XNU_MAX_CPUS, &c->ncpus)) return -22; continue; }
		for (unsigned i = 0; i < sizeof(strs) / sizeof(strs[0]) && !done; i++)
			if (!x_strcmp(k, strs[i].k)) { if (x_strlen(val) >= strs[i].cap) return -36; cfg_str(strs[i].dst, strs[i].cap, val); done = 1; }
		for (unsigned i = 0; i < sizeof(ints) / sizeof(ints[0]) && !done; i++)
			if (!x_strcmp(k, ints[i].k)) { if (parse_num(val, &v)) return -22; *ints[i].dst = (int)v; done = 1; }
		if (!done) {
			char m[96]; char *p = m; put_str(&p, "xnu: ctl start: unknown key "); put_str(&p, k); *p = 0; logs(m);
			return -22;
		}
	}
	if (!c->ncpus) { logs("xnu: ctl start: cpus= is required"); return -22; }
	if (c->mac_id < 0 || c->mac_id > 253) { logs("xnu: ctl start: mac_id is 0..253"); return -22; }
	if (c->ram_mb < 256) return -22;
	struct xnu_guest *g = &xnu_guest_pool[id];
	if (g->state != XG_FREE) return -16;
	if (!try_module_get(THIS_MODULE)) return -11;
	xg_reset(g, (int)id);
	g->cfg = *c;
	g->mode = 3;
	g->holds_ref = 1;
	g->state = XG_STARTING;
	long err = xg_claim(g, 1);
	if (err) { g->state = XG_FREE; g->holds_ref = 0; module_put(THIS_MODULE); return err; }
	g->monitor_cpu = xg_pick_monitor(g, c->monitor_cpu);
	if (g->monitor_cpu < 0) glogs(g, "xnu: no host CPU for the guest's monitor: its console_log stays empty");
	char *p = g->boot_name; put_str(&p, "xnu-boot"); p = put_dec(p, (unsigned long)id); *p = 0;
	g->boot_thread = fki_thread_start(xg_boot_fn, g, g->cpu[0].linux_cpu, g->boot_name);
	if (!g->boot_thread) { xg_unclaim(g); g->state = XG_FREE; g->holds_ref = 0; module_put(THIS_MODULE); return -12; }
	/* until it runs XNU or failed: a start that cannot run reports why here (and leaves the guest free) */
	while (g->state == XG_STARTING || g->state == XG_STOPPING) fki_msleep(10);
	if (g->state == XG_STOPPED && g->result) {
		err = g->result;
		fki_thread_stop(g->boot_thread);
		g->boot_thread = 0;
		g->state = XG_FREE;
		return err < 0 ? err : -5;
	}
	return 0;
}

static long ctl_id(const char *s, long *id)
{
	const char *t = s;
	*id = -1;
	while (next_token(&t)) {
		if (!x_strcmp(ctl_key, "id")) { if (parse_num(ctl_val, id)) return -22; }
		else if (!ctl_val[0] && !parse_num(ctl_key, id)) ;
		else return -22;
	}
	return *id >= 0 && *id < XNU_MAX_GUESTS ? 0 : -22;
}

static long ctl_write(const char *buf, unsigned long len)
{
	char cmd[16]; unsigned i = 0;
	const char *s = buf;
	long id, err;
	(void)len;
	while (*s == ' ' || *s == '\n') s++;
	while (*s && *s != ' ' && *s != '\n' && i < sizeof(cmd) - 1) cmd[i++] = *s++;
	cmd[i] = 0;
	xg_ctl_lock();
	if (!x_strcmp(cmd, "start")) {
		err = mode != 0 ? -16 : ctl_start(s);
	} else if (!x_strcmp(cmd, "stop") || !x_strcmp(cmd, "dump") || !x_strcmp(cmd, "reap")) {
		err = ctl_id(s, &id);
		struct xnu_guest *g = err ? 0 : &xnu_guest_pool[id];
		if (!g) ;
		else if (!x_strcmp(cmd, "stop")) {
			if (g->state == XG_RUNNING || g->state == XG_STARTING) {
				g->stop = 1;
				if (g->legacy) stop_run = 1;
				for (int c = 0; c < g->ncpus; c++) fki_kick_cpu(g->cpu[c].linux_cpu);
				glogs(g, "xnu: stop asked through ctl");
			} else if (g->state != XG_STOPPING) err = -3;
		} else if (!x_strcmp(cmd, "dump")) {
			if (g->state == XG_RUNNING) g->dump_req = 1; else err = -3;
		} else {
			if (g->state == XG_STOPPED) {
				if (g->boot_thread) { fki_thread_stop(g->boot_thread); g->boot_thread = 0; }
				g->state = XG_FREE;
				logs(g->id ? "xnu: guest reaped" : "xnu: guest 0 reaped");
			} else if (g->state != XG_FREE) err = -16;
		}
	} else {
		err = -22;
	}
	xg_ctl_unlock();
	return err;
}

static long ctl_read(char *buf, unsigned long len)
{
	char *p = buf, *end = buf + len - 400;
	put_str(&p, "xnu: granule="); p = put_dec(p, g_page >> 10); put_str(&p, "K host_cpus=");
	for (int i = 0; i < host_cpu_n; i++) { if (i) put_str(&p, ","); p = put_dec(p, host_cpu_list[i]); }
	if (!host_cpu_n) put_str(&p, "-");
	put_str(&p, " max_guests="); p = put_dec(p, XNU_MAX_GUESTS);
	put_str(&p, "\n");
	for (int i = 0; i < XNU_MAX_GUESTS && p < end; i++) {
		struct xnu_guest *g = &xnu_guest_pool[i];
		int st = g->state;
		if (st == XG_FREE) continue;
		put_str(&p, "guest "); p = put_dec(p, i);
		put_str(&p, ": state="); put_str(&p, xg_state_name[st]);
		put_str(&p, g->legacy ? " legacy=1" : " legacy=0");
		put_str(&p, " cpus=");
		for (int k = 0; k < g->ncpus; k++) { if (k) put_str(&p, ","); p = put_dec(p, g->cpu[k].linux_cpu); }
		put_str(&p, " slots=");
		for (int k = 0; k < g->ncpus; k++) { if (k) put_str(&p, ","); p = put_dec(p, g->cpu[k].slot); }
		put_str(&p, " monitor="); if (g->monitor_cpu >= 0) p = put_dec(p, g->monitor_cpu); else put_str(&p, "-");
		put_str(&p, " pa=0x"); { unsigned long v = g->img.region.va ? g->img.region.pa : 0; char h[20]; char *q = h; put_hex(&q, v); *q = 0; put_str(&p, h); }
		put_str(&p, " size=0x"); { unsigned long v = g->img.region.va ? g->img.region.size : 0; char h[20]; char *q = h; put_hex(&q, v); *q = 0; put_str(&p, h); }
		put_str(&p, " net="); put_str(&p, g->cfg.net[0] ? g->cfg.net : "-");
		put_str(&p, " mac=02:58:4e:55:00:");
		{ unsigned m = 1 + (unsigned)g->cfg.mac_id; *p++ = "0123456789abcdef"[(m >> 4) & 15]; *p++ = "0123456789abcdef"[m & 15]; }
		put_str(&p, " ram_mb="); p = put_dec(p, (unsigned long)g->cfg.ram_mb);
		put_str(&p, " syncs="); p = put_dec(p, g->syncs_snap);
		put_str(&p, " audits="); p = put_dec(p, g->audits);
		put_str(&p, " audit="); p = put_dec(p, (unsigned long)(g->audit_last > 0 ? g->audit_last : 0));
		put_str(&p, " audit_total="); p = put_dec(p, g->audit_total);
		put_str(&p, " adopted="); p = put_dec(p, (unsigned long)g->adopted);
		put_str(&p, " kfaults="); p = put_dec(p, g->kfaults);
		put_str(&p, " idles="); p = put_dec(p, g->idles);
		{ unsigned long up = 0; for (int k = 0; k < g->ncpus; k++) up += xnu_slot_state[g->cpu[k].slot].upcalls;
		  if (st == XG_STOPPED || st == XG_FREE) up = 0;
		  put_str(&p, " upcalls="); p = put_dec(p, up); }
		put_str(&p, " uptime_s="); p = put_dec(p, g->t_start ? (fki_counter() - g->t_start) / fki_counter_frequency() : 0);
		put_str(&p, " result="); if (g->result < 0) { put_str(&p, "-"); p = put_dec(p, (unsigned long)-g->result); } else p = put_dec(p, (unsigned long)g->result);
		put_str(&p, "\n");
	}
	return p - buf;
}

static int parse_host_cpus(void)
{
	static int list[XNU_LINUX_CPUS];
	int n = 0;
	if (!host_cpus || !host_cpus[0]) return 0;
	if (parse_cpus(host_cpus, list, XNU_LINUX_CPUS, &n)) { logs("xnu: bad host_cpus="); return -22; }
	for (int i = 0; i < n; i++)
		if (list[i] >= 0 && list[i] < XNU_LINUX_CPUS && !host_cpu[list[i]]) { host_cpu[list[i]] = 1; host_cpu_list[host_cpu_n++] = list[i]; }
	return 0;
}

static int ctl_registered;
static int xnu_init_body(void);
static int __init xnu_init(void)
{
	int err = xnu_init_body();
	if (err) {
		logs("xnu: load failed");
		if (ctl_registered) { fki_ctl_unregister(); ctl_registered = 0; }
	}
	return err;
}

static int xnu_init_body(void)
{
	int err;

	if (fki_version() != 1)
		return -22;
	g_s = fki_page_shift();
	if (g_s != 12 && g_s != 14) { fki_log("xnu: unsupported host translation granule shift", 47); return -22; }
	g_n = 1U << (g_s - 3);
	g_l2s = g_s + (g_s - 3);
	g_l1s = g_s + 2 * (g_s - 3);
	g_page = 1ULL << g_s;
	g_block = 1ULL << g_l2s;
	xnu_l1_first = (unsigned)((XNU_VA_LO >> g_l1s) & (g_n - 1));
	xnu_l1_last = (unsigned)(((XNU_VA_HI - 1) >> g_l1s) & (g_n - 1));
	root_lo = g_s == 14 ? 0xfffffff000000000ULL : XNU_VA_LO;
	w_first = (unsigned)((root_lo >> g_l1s) & (g_n - 1));
	p_valid = (XNU_VA_HI & ((1ULL << g_l1s) - 1)) != 0;
	p_idx = (unsigned)((XNU_VA_HI >> g_l1s) & (g_n - 1));
	w_end = p_valid ? p_idx : (unsigned)(((XNU_VA_HI - 1) >> g_l1s) & (g_n - 1)) + 1;
	for (int i = 0; i < LINUX64_CPU_SLOTS; i++) xnu_linux_cpu_of_slot[i] = -1;
	for (int i = 0; i < XNU_MAX_GUESTS; i++) xg_reset(&xnu_guest_pool[i], i);
	err = parse_host_cpus();
	if (err) return err;
	err = fki_ctl_register("xnu", ctl_write, ctl_read);
	if (err) logs("xnu: no /sys/kernel/xnu (ctl, status)");
	else ctl_registered = 1;
	if (mode == 0) {
		logs("xnu: loaded for guests started through /sys/kernel/xnu/ctl");
		return 0;
	}
	/* guest 0 from the module parameters, in insmod's own context (it returns when the run ends) */
	struct xnu_guest *g = &xnu_guest_pool[0];
	struct xg_cfg *c = &g->cfg;
	xg_cfg_defaults(c, 0);
	cfg_str(c->image, sizeof(c->image), image);
	cfg_str(c->disk, sizeof(c->disk), disk);
	cfg_str(c->keystore, sizeof(c->keystore), keystore);
	cfg_str(c->console_in, sizeof(c->console_in), console_in);
	cfg_str(c->console_log, sizeof(c->console_log), console_log);
	cfg_str(c->input_in, sizeof(c->input_in), input_in);
	cfg_str(c->fb_dump, sizeof(c->fb_dump), fb_dump);
	cfg_str(c->hostdisk, sizeof(c->hostdisk), hostdisk);
	cfg_str(c->ramdisk, sizeof(c->ramdisk), ramdisk);
	cfg_str(c->trustcache, sizeof(c->trustcache), trustcache);
	cfg_str(c->model, sizeof(c->model), model);
	cfg_str(c->net, sizeof(c->net), net);
	cfg_str(c->root_uuid, sizeof(c->root_uuid), root_uuid);
	cfg_str(c->boot_uuid, sizeof(c->boot_uuid), boot_uuid);
	cfg_str(c->preboot_uuid, sizeof(c->preboot_uuid), preboot_uuid);
	cfg_str(c->bootargs, sizeof(c->bootargs), bootargs);
	c->ram_mb = ram_mb; c->mac_id = mac_id; c->fb_width = fb_width; c->fb_height = fb_height; c->fb_dump_ms = fb_dump_ms;
	c->hostdisk_rw = hostdisk_rw; c->vm16k = vm16k; c->kmax = kmax; c->idle_max = idle_max; c->quiet_console = quiet_console;
	c->assert_fatal = assert_fatal; c->park_secondaries = park_secondaries; c->monitor_cpu = monitor_cpu;
	if (c->mac_id < 0 || c->mac_id > 253) return -22;
	int n = cpus < 1 ? 1 : cpus > XNU_MAX_CPUS ? XNU_MAX_CPUS : cpus;
	if (n > fki_num_cpus()) n = fki_num_cpus();
	if (mode == 1) n = 1;
	c->ncpus = n;
	for (int k = 0; k < n; k++) c->cpu[k] = k;
	g->legacy = 1;
	g->mode = mode;
	g->state = XG_STARTING;
	/*
	 * XNU identifies its CPUs by MPIDR: its CPU 0 runs here, bound to Linux CPU 0; CPUs 1 .. cpus-1 are Linux's of the
	 * same numbers, started when XNU asks (LINUX64_CALL_CPU_START). Their slots in the per-CPU arrays must differ.
	 */
	err = fki_bind_cpu(0);
	if (err) { glogkv(g, "xnu: cannot bind to cpu0, err", (u64_t)err); g->state = XG_FREE; return err; }
	err = xg_claim(g, 0);
	if (err) { g->state = XG_FREE; return err; }
	{	/* the helpers run where XNU does not: a CPU running XNU lets Linux in only at XNU's calls into the host */
		int host_cpu = g->ncpus < fki_num_cpus() ? g->ncpus : fki_num_cpus() - 1;
		g->monitor_cpu = monitor_cpu == -2 ? host_cpu : monitor_cpu;
	}
	err = xg_run(g);
	if (err) glogkv(g, "xnu: load failed, cleaned up, err", (u64_t)(long)err);
	return err;
}

static void __exit xnu_exit(void)
{
	if (ctl_registered) { fki_ctl_unregister(); ctl_registered = 0; }
	for (int i = 0; i < XNU_MAX_GUESTS; i++) {
		struct xnu_guest *g = &xnu_guest_pool[i];
		if (g->boot_thread) { fki_thread_stop(g->boot_thread); g->boot_thread = 0; }
		g->state = XG_FREE;
	}
}

module_init(xnu_init);
module_exit(xnu_exit);
MODULE_LICENSE("Proprietary");	/* APSL-2.0 side: not GPL-compatible; taints the kernel like ZFS */
MODULE_DESCRIPTION("XNU personality (PoC), several guests");
