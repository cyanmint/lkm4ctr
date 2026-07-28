// SPDX-License-Identifier: GPL-2.0
/*
 * vendor_kernel_ipc_callbacks.c - real-name wrappers that are installed as
 * callbacks called back into by this module's OWN CFI-instrumented ipc
 * code (util.o/shm.o), split out of glue/vendor_kernel_ipc_compat.c into
 * their own translation unit.
 *
 * Background: glue/vendor_kernel_ipc_compat.o is compiled with Clang CFI
 * (CONFIG_CFI_CLANG) disabled (see lkm4ctr/Makefile's VNS_CFI_UNSAFE_OBJS),
 * because nearly every function it defines makes an unsafe indirect call
 * through a shadow_hook_resolve()-resolved raw function pointer, which can
 * never satisfy the kernel's compile-time KCFI type-hash check. That is fine
 * for ordinary wrapper functions, which are only ever called *from* this
 * module's own vendored ipc/mqueue.c sources by name (an outgoing call, no
 * CFI check involved at all). But two classes of function defined in that
 * file are instead reached through an indirect call from elsewhere:
 *
 *   - simple_lookup(): ipc/mqueue.c's mq_dir_inode_operations installs it as
 *     its .lookup callback, so the real, CFI-instrumented kernel calls
 *     *into* it indirectly via dir->i_op->lookup() (fs/namei.c's
 *     __lookup_slow()). Observed live:
 *       "CFI failure at __lookup_slow+... (target: simple_lookup+...)"
 *
 *   - security_msg_queue_associate()/security_sem_associate()/
 *     security_shm_associate(): ipc/msg.c's msg_ops.associate,
 *     ipc/sem.c's sem_ops.associate and ipc/shm.c's shm_ops.associate all
 *     point at these, and ipc/util.c's ipcget()/ipcget_public() (compiled
 *     WITH CFI, it is not in VNS_CFI_UNSAFE_OBJS) calls ops->associate(...)
 *     indirectly. Observed live:
 *       "CFI failure at ipcget+... (target: security_msg_queue_associate+...)"
 *     (msg_ops.getnew = newque and sem_ops.getnew = newary hit the exact
 *     same problem from the other direction -- see lkm4ctr/Makefile, which
 *     no longer disables CFI for ipc/msg.o / ipc/sem.o for this reason).
 *
 * Disabling CFI for the whole vendor_kernel_ipc_compat.o object also strips
 * the KCFI type-hash prefix from these functions, so the indirect call made
 * *into* them by this module's own (or the real kernel's) CFI-instrumented
 * caller fails its type check and panics (observed live in QEMU boot
 * testing on android14-6.1, CONFIG_CFI_CLANG=y).
 *
 * The fix mirrors the pattern already used for vns_kill_litter_super()
 * (glue/vendor_kernel_compat.c) and the mqueue_fs_type/proc_ns_operations
 * callback objects documented in lkm4ctr/Makefile's VNS_CFI_UNSAFE_OBJS
 * comment: keep *this* object's own CFI instrumentation enabled (so each of
 * these functions keeps a valid KCFI type-hash prefix and remains a valid
 * indirect-call target), and mark only the specific outgoing resolved-
 * pointer call each one makes with the kernel's own __nocfi attribute
 * (<linux/compiler_types.h>) to suppress the guaranteed-false-positive CFI
 * check on that one outgoing call.
 */
#include <linux/kernel.h>
#include <linux/compiler_types.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/dcache.h>
#include <linux/ipc.h>

#include "../../../common/shadow_hook.h"
#include "../../../common/lkm4ctr_log.h"

typedef struct dentry *(*simple_lookup_fn_t)(struct inode *, struct dentry *,
	unsigned int);
typedef int (*sec_msg_queue_associate_fn_t)(struct kern_ipc_perm *, int);
typedef int (*sec_sem_associate_fn_t)(struct kern_ipc_perm *, int);
typedef int (*sec_shm_associate_fn_t)(struct kern_ipc_perm *, int);

static simple_lookup_fn_t r_simple_lookup;
static sec_msg_queue_associate_fn_t r_sec_msg_queue_associate;
static sec_sem_associate_fn_t r_sec_sem_associate;
static sec_shm_associate_fn_t r_sec_shm_associate;

/*
 * Resolve these symbols at init time. Called from vendor_kernel_init()
 * alongside vns_ipc_compat_resolve() (glue/vendor_kernel_ipc_compat.c),
 * before any vendored mqueue directory lookup or msgget/semget/shmget
 * call can occur.
 */
void vns_ipc_lookup_resolve(void)
{
#define R(var, sym) \
	do { \
		(var) = (typeof(var))(uintptr_t)shadow_hook_resolve(#sym); \
		if (!(var)) \
			LKM4CTR_WARN("vendor_kernel", \
				"ipc compat: " #sym " not resolved (stub active)"); \
	} while (0)

	R(r_simple_lookup, simple_lookup);
	R(r_sec_msg_queue_associate, security_msg_queue_associate);
	R(r_sec_sem_associate, security_sem_associate);
	R(r_sec_shm_associate, security_shm_associate);

#undef R
}

/*
 * [BUILD-COMPAT] simple_lookup (fs/libfs.c, not exported). Installed as
 * mq_dir_inode_operations.lookup (ipc/mqueue.c) and therefore called back
 * by the real kernel's own, CFI-instrumented VFS lookup path -- see the
 * file header comment above for why only this function's own outgoing
 * r_simple_lookup() call is marked __nocfi, while the function itself
 * keeps its normal KCFI type-hash prefix.
 */
struct dentry *__nocfi simple_lookup(struct inode *dir, struct dentry *dentry,
				     unsigned int flags)
{
	if (r_simple_lookup)
		return r_simple_lookup(dir, dentry, flags);
	d_add(dentry, NULL);
	return NULL;
}

/*
 * [BUILD-COMPAT] security_msg_queue_associate/security_sem_associate/
 * security_shm_associate (kernel/security.c wrappers around the LSM
 * ipc_associate hooks, not exported). Installed as msg_ops.associate/
 * sem_ops.associate/shm_ops.associate (ipc/msg.c, ipc/sem.c, ipc/shm.c) and
 * called back indirectly from this module's own CFI-instrumented
 * ipc/util.c ipcget()/ipcget_public() -- see the file header comment above
 * for why only each function's own outgoing r_sec_*_associate() call is
 * marked __nocfi, while the functions themselves keep their normal KCFI
 * type-hash prefix.
 */
int __nocfi security_msg_queue_associate(struct kern_ipc_perm *msq, int msqflg)
{
	if (r_sec_msg_queue_associate)
		return r_sec_msg_queue_associate(msq, msqflg);
	return 0;
}

int __nocfi security_sem_associate(struct kern_ipc_perm *sma, int semflg)
{
	if (r_sec_sem_associate)
		return r_sec_sem_associate(sma, semflg);
	return 0;
}

int __nocfi security_shm_associate(struct kern_ipc_perm *shp, int shmflg)
{
	if (r_sec_shm_associate)
		return r_sec_shm_associate(shp, shmflg);
	return 0;
}
