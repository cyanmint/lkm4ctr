// Public Domain since creation (AI-generated; no copyright subsists; not CC0).
/*
 * Old Qualcomm Android 12 kernels can omit exports present in newer GKI DDKs.
 * Keep the real credential/inode implementations; never emulate their lifetime
 * or link-count accounting. Local definitions also catch calls from DDK inlines.
 */
#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/cred.h>
#include <linux/fs.h>
#include <linux/percpu-rwsem.h>
#include "shadow_hook.h"
#include "lkm4ctr_log.h"

int vns_legacy_symbols_init(void);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0) && \
    LINUX_VERSION_CODE < KERNEL_VERSION(5, 12, 0)
static void (*real_put_cred)(struct cred *);
static const struct cred *(*real_get_task_cred)(struct task_struct *);
static void (*real_clear_nlink)(struct inode *);
static void (*real_record_pcpu_rwsem_starttime)(struct task_struct *, unsigned long);

int vns_legacy_symbols_init(void)
{
#define RESOLVE_REQUIRED(ptr, symbol) do { \
	ptr = (typeof(ptr))shadow_hook_resolve(symbol); \
	if (!ptr) { \
		LKM4CTR_ERR("legacy_symbols", "required function unavailable: %s", symbol); \
		return -ENOENT; \
	} \
} while (0)
	RESOLVE_REQUIRED(real_put_cred, "__put_cred");
	RESOLVE_REQUIRED(real_get_task_cred, "get_task_cred");
	RESOLVE_REQUIRED(real_clear_nlink, "clear_nlink");
#undef RESOLVE_REQUIRED
	/* Upstream helper only emits a vendor trace hook; it does not lock or
	 * unlock the semaphore. Older targets have neither helper nor hook.
	 * Preserve the hook on kernels that do have it; otherwise omit only
	 * this optional timing notification, not any semaphore operation.
	 */
	real_record_pcpu_rwsem_starttime =
		(typeof(real_record_pcpu_rwsem_starttime))shadow_hook_resolve(
			"_trace_android_vh_record_pcpu_rwsem_starttime");
	if (!real_record_pcpu_rwsem_starttime)
		LKM4CTR_WARN("legacy_symbols", "optional percpu rwsem timing hook absent");
	return 0;
}

void __nocfi __put_cred(struct cred *cred)
{
	real_put_cred(cred);
}

const struct cred *__nocfi get_task_cred(struct task_struct *task)
{
	return real_get_task_cred(task);
}

void __nocfi clear_nlink(struct inode *inode)
{
	real_clear_nlink(inode);
}

void __nocfi _trace_android_vh_record_pcpu_rwsem_starttime(
	struct task_struct *task, unsigned long settime)
{
	if (real_record_pcpu_rwsem_starttime)
		real_record_pcpu_rwsem_starttime(task, settime);
}
#else
int vns_legacy_symbols_init(void)
{
	return 0;
}
#endif
