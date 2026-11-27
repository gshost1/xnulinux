// SPDX-License-Identifier: GPL-2.0-only
/*
 * Host calls: a message channel from a foreign kernel to a Linux user process (a host service such as the DRM bridge,
 * gpu/xnu-drm/xnudrmd). The foreign kernel calls fki_hostcall2() with a request; the service reads it from its channel's
 * device (/dev/xnu-hostcall for channel 0, /dev/xnu-hostcall<n> for channel n), does the work with ordinary system calls
 * and writes the reply back. fki only moves bytes: the message format belongs to the two ends.
 *
 * One call at a time per channel. The calling foreign CPU is blocked in the call and deaf to the other foreign CPUs'
 * interrupts, so a call with no service to answer it fails at once (-ENODEV), not after HC_TIMEOUT: the other CPUs would
 * time out waiting on that one (an XNU panic). The service also asks fki to translate its own mappings into physical
 * addresses (XNU_HOSTCALL_XLATE), so the foreign kernel can map the same pages.
 *
 * Linux never touches the foreign kernel's memory with its own address space live: a call is five steps, the request
 * copied into the channel's buffer and the reply out of it while the foreign address space is live, the rest (the
 * channel's lock, publishing, waiting) in Linux.
 */
#include "fki.h"
#include "fki_internal.h"
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>

#define HC_MAX (8UL << 20)		/* largest request or reply */
#define HC_TIMEOUT (120 * HZ)		/* a service that does not answer: the call fails */
#define HC_CHANNELS 8

struct xnu_hostcall_xlate {
	__u64 va, len;			/* a range of the caller's address space, 4 KiB aligned */
	__u64 out;			/* user pointer: one __u64 physical address per 4 KiB page */
	__u64 max;			/* entries at out */
};
#define XNU_HOSTCALL_XLATE _IOWR('X', 1, struct xnu_hostcall_xlate)

struct fki_hc {
	struct miscdevice misc;
	char name[24];
	int index;
	int registered;
	wait_queue_head_t req_wait, done_wait;
	spinlock_t lock;
	struct mutex mutex;		/* one call at a time through the single slot (several CPUs may call) */
	void *buf;			/* request, then reply (allocated before the foreign kernel runs) */
	unsigned long len;
	u64 seq, next_seq;		/* the call in flight (0: none) */
	int state;			/* 0 idle, 1 request waiting, 2 read by the service, 3 replied */
	long result;
	int open;			/* the service's opens (0: no service, calls fail at once) */
};
static struct fki_hc *hc_chan[HC_CHANNELS];
static DEFINE_MUTEX(hc_chan_lock);

static struct fki_hc *hc_of(struct file *f)
{
	return container_of(f->private_data, struct fki_hc, misc);
}

struct hc_args { struct fki_hc *hc; unsigned long len, max; long r; };

/* step 1, in Linux: the channel's lock, and is there a service */
static void hc_step_lock(void *p)
{
	struct hc_args *a = p;
	struct fki_hc *hc = a->hc;

	local_irq_enable();
	preempt_enable();
	mutex_lock(&hc->mutex);
	if (!hc->buf)
		hc->buf = kvmalloc(HC_MAX, GFP_KERNEL);	/* version 0 callers: not allocated ahead */
	spin_lock(&hc->lock);
	a->r = !hc->buf ? -ENOMEM : !hc->open ? -ENODEV : 0;
	spin_unlock(&hc->lock);
	if (a->r)
		mutex_unlock(&hc->mutex);
	preempt_disable();
	local_irq_disable();
}

/* step 3, in Linux: publish the request and wait for the reply */
static void hc_step_call(void *p)
{
	struct hc_args *a = p;
	struct fki_hc *hc = a->hc;
	long r;

	local_irq_enable();
	preempt_enable();
	spin_lock(&hc->lock);
	if (!hc->open) {
		spin_unlock(&hc->lock);
		r = -ENODEV;
		goto out;
	}
	hc->len = a->len;
	hc->seq = ++hc->next_seq;
	hc->state = 1;
	spin_unlock(&hc->lock);
	wake_up_interruptible(&hc->req_wait);
	/* until the reply, or the service closing its device with the call unanswered */
	wait_event_timeout(hc->done_wait, READ_ONCE(hc->state) == 3 || !READ_ONCE(hc->open), HC_TIMEOUT);
	spin_lock(&hc->lock);
	if (hc->state != 3) {
		r = hc->open ? -ETIMEDOUT : -ENODEV;
		hc->state = 0;
		hc->seq = 0;
		spin_unlock(&hc->lock);
		goto out;
	}
	spin_unlock(&hc->lock);
	r = hc->result;
	a->len = hc->len;
out:
	a->r = r;
	preempt_disable();
	local_irq_disable();
}

/* step 5, in Linux: the slot idle again, the lock released */
static void hc_step_unlock(void *p)
{
	struct hc_args *a = p;
	struct fki_hc *hc = a->hc;

	local_irq_enable();
	preempt_enable();
	spin_lock(&hc->lock);
	hc->state = 0;
	hc->seq = 0;
	spin_unlock(&hc->lock);
	mutex_unlock(&hc->mutex);
	preempt_disable();
	local_irq_disable();
}

/*
 * From inside fki_enter(): send len bytes at req to the channel's service and wait for its reply (at most max bytes into
 * reply). Returns the reply's length, -EMSGSIZE when it does not fit, -ETIMEDOUT when no service answered in time.
 */
long fki_hostcall2(void *handle, const void *req, unsigned long len, void *reply, unsigned long max)
{
	struct fki_hc *hc = handle;
	struct hc_args a = { hc, len, max, -EIO };
	long r;

	if (!hc)
		return -ENODEV;
	if (len > HC_MAX)
		return -EMSGSIZE;
	fki_linux_runs_ext(hc_step_lock, &a, sizeof(a));		/* 1: Linux */
	if (a.r)
		return a.r;
	memcpy(hc->buf, req, len);					/* 2: the foreign address space */
	a.len = len;
	fki_linux_runs_ext(hc_step_call, &a, sizeof(a));		/* 3: Linux */
	r = a.r;
	if (r >= 0) {							/* 4: the foreign address space */
		unsigned long n = min_t(unsigned long, a.len, max);

		memcpy(reply, hc->buf, n);
		r = a.len > max ? -EMSGSIZE : (long)n;
	}
	fki_linux_runs_ext(hc_step_unlock, &a, sizeof(a));		/* 5: Linux */
	return r;
}
EXPORT_SYMBOL(fki_hostcall2);

long fki_hostcall(const void *req, unsigned long len, void *reply, unsigned long max)
{
	return fki_hostcall2(hc_chan[0], req, len, reply, max);
}
EXPORT_SYMBOL(fki_hostcall);

/* The service: read() the next request, prefixed with its 8-byte sequence number. */
static ssize_t hc_read(struct file *f, char __user *buf, size_t n, loff_t *off)
{
	struct fki_hc *hc = hc_of(f);
	u64 seq;
	int err;

	err = wait_event_interruptible(hc->req_wait, READ_ONCE(hc->state) == 1);
	if (err)
		return err;
	spin_lock(&hc->lock);
	if (hc->state != 1) {
		spin_unlock(&hc->lock);
		return -EAGAIN;
	}
	hc->state = 2;
	seq = hc->seq;
	spin_unlock(&hc->lock);
	if (n < 8 + hc->len)
		return -EMSGSIZE;
	if (copy_to_user(buf, &seq, 8) || copy_to_user(buf + 8, hc->buf, hc->len))
		return -EFAULT;
	return 8 + hc->len;
}

/* The service: write() the reply, prefixed with the request's sequence number. */
static ssize_t hc_write(struct file *f, const char __user *buf, size_t n, loff_t *off)
{
	struct fki_hc *hc = hc_of(f);
	u64 seq;

	if (n < 8 || n - 8 > HC_MAX)
		return -EINVAL;
	if (copy_from_user(&seq, buf, 8))
		return -EFAULT;
	spin_lock(&hc->lock);
	if (hc->state != 2 || seq != hc->seq) {
		spin_unlock(&hc->lock);
		return -ESTALE;		/* the call timed out meanwhile */
	}
	spin_unlock(&hc->lock);
	if (copy_from_user(hc->buf, buf + 8, n - 8))
		return -EFAULT;
	hc->len = n - 8;
	hc->result = 0;
	WRITE_ONCE(hc->state, 3);
	wake_up(&hc->done_wait);
	return n;
}

static long hc_xlate_one(struct mm_struct *mm, unsigned long va, u64 *pa)
{
	struct vm_area_struct *vma = vma_lookup(mm, va);
	struct page *pg;

	if (!vma)
		return -EFAULT;
	if (vma->vm_flags & (VM_IO | VM_PFNMAP)) {
		struct follow_pfnmap_args args = { .vma = vma, .address = va };
		int err = follow_pfnmap_start(&args);
		if (err)
			return err;
		*pa = (u64)args.pfn << PAGE_SHIFT;
		follow_pfnmap_end(&args);
		return 0;
	}
	if (get_user_pages_remote(mm, va, 1, FOLL_WRITE, &pg, NULL) != 1)
		return -EFAULT;
	*pa = page_to_phys(pg);
	put_page(pg);		/* the mapping's owner (a GEM object) keeps the page */
	return 0;
}

static long hc_xlate(struct xnu_hostcall_xlate __user *ua)
{
	struct xnu_hostcall_xlate a;
	struct mm_struct *mm = current->mm;
	unsigned long i, n;
	long err = 0;

	if (copy_from_user(&a, ua, sizeof(a)))
		return -EFAULT;
	if ((a.va | a.len) & (PAGE_SIZE - 1))
		return -EINVAL;
	n = a.len >> PAGE_SHIFT;
	if (n > a.max)
		return -E2BIG;
	mmap_read_lock(mm);
	for (i = 0; i < n && !err; i++) {
		u64 pa;
		err = hc_xlate_one(mm, a.va + (i << PAGE_SHIFT), &pa);
		if (!err && put_user(pa, (u64 __user *)a.out + i))
			err = -EFAULT;
	}
	mmap_read_unlock(mm);
	return err;
}

static long hc_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	if (cmd == XNU_HOSTCALL_XLATE)
		return hc_xlate((struct xnu_hostcall_xlate __user *)arg);
	return -ENOTTY;
}

static int hc_dev_open(struct inode *i, struct file *f)
{
	struct fki_hc *hc = hc_of(f);

	spin_lock(&hc->lock);
	hc->open++;
	spin_unlock(&hc->lock);
	return 0;
}

static int hc_dev_release(struct inode *i, struct file *f)
{
	struct fki_hc *hc = hc_of(f);

	spin_lock(&hc->lock);
	hc->open--;
	spin_unlock(&hc->lock);
	wake_up(&hc->done_wait);		/* a call waiting on this service fails now */
	return 0;
}

static const struct file_operations hc_fops = {
	.owner = THIS_MODULE,
	.open = hc_dev_open,
	.release = hc_dev_release,
	.read = hc_read,
	.write = hc_write,
	.unlocked_ioctl = hc_ioctl,
	.llseek = noop_llseek,
};

/* A channel and its device (made once; it stays until fki unloads, so a service can keep it open across runs) */
static struct fki_hc *hc_make(int index, int *err)
{
	struct fki_hc *hc;

	if (index < 0 || index >= HC_CHANNELS) {
		*err = -EINVAL;
		return NULL;
	}
	mutex_lock(&hc_chan_lock);
	hc = hc_chan[index];
	if (!hc) {
		hc = kzalloc(sizeof(*hc), GFP_KERNEL);
		if (!hc) {
			mutex_unlock(&hc_chan_lock);
			*err = -ENOMEM;
			return NULL;
		}
		hc->index = index;
		init_waitqueue_head(&hc->req_wait);
		init_waitqueue_head(&hc->done_wait);
		spin_lock_init(&hc->lock);
		mutex_init(&hc->mutex);
		if (index)
			snprintf(hc->name, sizeof(hc->name), "xnu-hostcall%d", index);
		else
			strscpy(hc->name, "xnu-hostcall", sizeof(hc->name));
		hc->misc.minor = MISC_DYNAMIC_MINOR;
		hc->misc.name = hc->name;
		hc->misc.fops = &hc_fops;
		hc->misc.mode = 0600;
		*err = misc_register(&hc->misc);
		if (*err) {
			kfree(hc);
			mutex_unlock(&hc_chan_lock);
			return NULL;
		}
		hc->registered = 1;
		hc_chan[index] = hc;
	}
	mutex_unlock(&hc_chan_lock);
	*err = 0;
	return hc;
}

void *fki_hostcall_create(int index, int *err)
{
	struct fki_hc *hc = hc_make(index, err);

	if (!hc)
		return NULL;
	mutex_lock(&hc->mutex);
	if (!hc->buf)
		hc->buf = kvmalloc(HC_MAX, GFP_KERNEL);	/* now: before the foreign kernel's private root is made */
	mutex_unlock(&hc->mutex);
	if (!hc->buf) {
		*err = -ENOMEM;
		return NULL;
	}
	return hc;
}
EXPORT_SYMBOL(fki_hostcall_create);

void fki_hostcall_release(void *handle)
{
	(void)handle;	/* the channel and its buffer stay for the next run */
}
EXPORT_SYMBOL(fki_hostcall_release);

int fki_hostcall_init(void)
{
	int err;

	return hc_make(0, &err) ? 0 : err;
}

void fki_hostcall_exit(void)
{
	int i;

	for (i = 0; i < HC_CHANNELS; i++) {
		struct fki_hc *hc = hc_chan[i];

		if (!hc)
			continue;
		if (hc->registered)
			misc_deregister(&hc->misc);
		kvfree(hc->buf);
		kfree(hc);
		hc_chan[i] = NULL;
	}
}
