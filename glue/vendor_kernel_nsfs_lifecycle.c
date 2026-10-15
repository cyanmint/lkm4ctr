// SPDX-License-Identifier: GPL-2.0
/*
 * Pin lkm4ctr while a kernel NSFS inode retains module-owned proc_ns_ops.
 *
 * Stock GKI's nsfs file_operations have no module owner. Its inode keeps a
 * namespace pointer and calls ns->ops->put() from nsfs_evict(), so an nsfs
 * dentry can otherwise retain callbacks into lkm4ctr after module removal.
 * The path helper hook takes a module reference before the real helper can
 * create/use an inode, records that reference by inode identity, and the
 * eviction hook releases it only after the real nsfs_evict() has finished.
 *
 * These are private kernel symbols, not stable KMI exports. Installing both
 * hooks is mandatory: fail vendor_kernel initialization if either cannot be
 * resolved, rather than expose module-owned namespace ops without lifetime
 * protection. The version split follows the Android common nsfs sources:
 * __ns_get_path() through 6.6, path_from_stashed() from 6.12 onward.
 */
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/hashtable.h>
#include <linux/magic.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/version.h>

#include "../vendor/vendor_kernel.h"
#include "shadow_hook.h"
#include "lkm4ctr_log.h"

#define VNS_NSFS_PIN_HASH_BITS 6

struct vns_nsfs_inode_pin {
	struct hlist_node node;
	struct inode *inode;
};

static DEFINE_HASHTABLE(vns_nsfs_inode_pins, VNS_NSFS_PIN_HASH_BITS);
static DEFINE_SPINLOCK(vns_nsfs_inode_pins_lock);
static DEFINE_MUTEX(vns_nsfs_lifecycle_lock);
static atomic_t vns_nsfs_inode_pin_count = ATOMIC_INIT(0);
static atomic_t vns_nsfs_ipc_debug_count = ATOMIC_INIT(0);
static bool vns_nsfs_hooks_active;
static bool vns_nsfs_stopping;

static void (*real_nsfs_evict)(struct inode *);

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 12, 0)
static int (*real_nsfs_get_path)(struct path *, struct ns_common *);
#else
static int (*real_path_from_stashed)(struct dentry **, struct vfsmount *,
				     void *, struct path *);
#endif

static bool vns_nsfs_module_owns_ops(const struct ns_common *ns)
{
	return ns && ns->ops &&
	       within_module((unsigned long)ns->ops, THIS_MODULE);
}

static bool vns_nsfs_debug_ipc(const struct ns_common *ns)
{
	return ns && ns->ops == &vns_ipcns_operations &&
	       atomic_inc_return(&vns_nsfs_ipc_debug_count) <= 8;
}

/*
 * Called only while the path returned by the real helper holds an inode
 * reference. The caller already owns one tentative module reference.
 */
static void vns_nsfs_pin_inode(struct inode *inode,
			       struct vns_nsfs_inode_pin *candidate)
{
	struct vns_nsfs_inode_pin *entry;
	unsigned long flags;

	if (!inode) {
		/* Keep the tentative module reference permanently: fail closed. */
		atomic_inc(&vns_nsfs_inode_pin_count);
		kfree(candidate);
		LKM4CTR_ERR("vendor_kernel",
			    "NSFS path helper returned success without an inode; retaining a fail-safe module pin");
		return;
	}

	if (!candidate) {
		/*
		 * Allocation failure must not leave a live NSFS inode unpinned.
		 * Retain the already-acquired reference permanently; controlled
		 * teardown will continue to refuse while the count is nonzero.
		 */
		atomic_inc(&vns_nsfs_inode_pin_count);
		LKM4CTR_ERR("vendor_kernel",
			    "could not allocate NSFS inode pin record; retaining a fail-safe module pin");
		return;
	}

	candidate->inode = inode;
	spin_lock_irqsave(&vns_nsfs_inode_pins_lock, flags);
	hash_for_each_possible(vns_nsfs_inode_pins, entry, node,
			       (unsigned long)inode) {
		if (entry->inode == inode) {
			spin_unlock_irqrestore(&vns_nsfs_inode_pins_lock, flags);
			kfree(candidate);
			module_put(THIS_MODULE);
			return;
		}
	}
	hash_add(vns_nsfs_inode_pins, &candidate->node, (unsigned long)inode);
	atomic_inc(&vns_nsfs_inode_pin_count);
	spin_unlock_irqrestore(&vns_nsfs_inode_pins_lock, flags);
}

static void vns_nsfs_unpin_inode(struct inode *inode)
{
	struct vns_nsfs_inode_pin *entry;
	struct vns_nsfs_inode_pin *found = NULL;
	unsigned long flags;

	spin_lock_irqsave(&vns_nsfs_inode_pins_lock, flags);
	hash_for_each_possible(vns_nsfs_inode_pins, entry, node,
			       (unsigned long)inode) {
		if (entry->inode == inode) {
			hash_del(&entry->node);
			found = entry;
			break;
		}
	}
	spin_unlock_irqrestore(&vns_nsfs_inode_pins_lock, flags);

	if (!found)
		return;

	atomic_dec(&vns_nsfs_inode_pin_count);
	kfree(found);
	module_put(THIS_MODULE);
}

static void vendor_kernel_hook_nsfs_evict(struct inode *inode)
{
	/* Keep the pin through nsfs_evict(), which invokes ns->ops->put(). */
	real_nsfs_evict(inode);
	vns_nsfs_unpin_inode(inode);
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 12, 0)
static int vendor_kernel_hook_nsfs_get_path(struct path *path,
					    struct ns_common *ns)
{
	struct vns_nsfs_inode_pin *candidate = NULL;
	bool debug_ipc = vns_nsfs_debug_ipc(ns);
	int ret;

	if (debug_ipc)
		LKM4CTR_INFO("vendor_kernel_nsfs",
			     "debug IPC path helper entry ns=%p ops=%p owned=%d",
			     ns, ns->ops, vns_nsfs_module_owns_ops(ns));
	if (!vns_nsfs_module_owns_ops(ns))
		return real_nsfs_get_path(path, ns);
	mutex_lock(&vns_nsfs_lifecycle_lock);
	if (vns_nsfs_stopping) {
		mutex_unlock(&vns_nsfs_lifecycle_lock);
		return -ESHUTDOWN;
	}

	candidate = kmalloc(sizeof(*candidate), GFP_KERNEL);
	if (!try_module_get(THIS_MODULE)) {
		kfree(candidate);
		mutex_unlock(&vns_nsfs_lifecycle_lock);
		return -ESHUTDOWN;
	}

	ret = real_nsfs_get_path(path, ns);
	if (debug_ipc)
		LKM4CTR_INFO("vendor_kernel_nsfs",
			     "debug IPC path helper returned ret=%d dentry=%p",
			     ret, path ? path->dentry : NULL);
	if (ret) {
		kfree(candidate);
		module_put(THIS_MODULE);
		mutex_unlock(&vns_nsfs_lifecycle_lock);
		return ret;
	}

	vns_nsfs_pin_inode(path && path->dentry ? d_inode(path->dentry) : NULL,
			   candidate);
	if (debug_ipc)
		LKM4CTR_INFO("vendor_kernel_nsfs",
			     "debug IPC path helper pins=%d",
			     atomic_read(&vns_nsfs_inode_pin_count));
	mutex_unlock(&vns_nsfs_lifecycle_lock);
	return 0;
}
#else
static int vendor_kernel_hook_path_from_stashed(struct dentry **stashed,
						struct vfsmount *mnt,
						void *data,
						struct path *path)
{
	struct ns_common *ns = data;
	struct vns_nsfs_inode_pin *candidate = NULL;
	bool debug_ipc = vns_nsfs_debug_ipc(ns);
	int ret;

	if (debug_ipc)
		LKM4CTR_INFO("vendor_kernel_nsfs",
			     "debug IPC stashed helper entry ns=%p ops=%p owned=%d mnt=%p magic=%lx",
			     ns, ns->ops, vns_nsfs_module_owns_ops(ns), mnt,
			     mnt && mnt->mnt_sb ? mnt->mnt_sb->s_magic : 0UL);
	if (!mnt || !mnt->mnt_sb || mnt->mnt_sb->s_magic != NSFS_MAGIC ||
	    !vns_nsfs_module_owns_ops(ns))
		return real_path_from_stashed(stashed, mnt, data, path);
	mutex_lock(&vns_nsfs_lifecycle_lock);
	if (vns_nsfs_stopping) {
		mutex_unlock(&vns_nsfs_lifecycle_lock);
		return -ESHUTDOWN;
	}

	candidate = kmalloc(sizeof(*candidate), GFP_KERNEL);
	if (!try_module_get(THIS_MODULE)) {
		kfree(candidate);
		mutex_unlock(&vns_nsfs_lifecycle_lock);
		return -ESHUTDOWN;
	}

	ret = real_path_from_stashed(stashed, mnt, data, path);
	if (debug_ipc)
		LKM4CTR_INFO("vendor_kernel_nsfs",
			     "debug IPC stashed helper returned ret=%d dentry=%p",
			     ret, path ? path->dentry : NULL);
	if (ret) {
		kfree(candidate);
		module_put(THIS_MODULE);
		mutex_unlock(&vns_nsfs_lifecycle_lock);
		return ret;
	}

	vns_nsfs_pin_inode(path && path->dentry ? d_inode(path->dentry) : NULL,
			   candidate);
	if (debug_ipc)
		LKM4CTR_INFO("vendor_kernel_nsfs",
			     "debug IPC stashed helper pins=%d",
			     atomic_read(&vns_nsfs_inode_pin_count));
	mutex_unlock(&vns_nsfs_lifecycle_lock);
	return 0;
}
#endif

static const char * const nsfs_evict_names[] = {
	"nsfs_evict",
	NULL,
};
static struct shadow_hook nsfs_evict_hook =
	SHADOW_HOOK(nsfs_evict_names, vendor_kernel_hook_nsfs_evict,
		    &real_nsfs_evict);

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 12, 0)
static const char * const nsfs_path_names[] = {
	"__ns_get_path",
	NULL,
};
static struct shadow_hook nsfs_path_hook =
	SHADOW_HOOK(nsfs_path_names, vendor_kernel_hook_nsfs_get_path,
		    &real_nsfs_get_path);
#else
static const char * const nsfs_path_names[] = {
	"path_from_stashed",
	NULL,
};
static struct shadow_hook nsfs_path_hook =
	SHADOW_HOOK(nsfs_path_names, vendor_kernel_hook_path_from_stashed,
		    &real_path_from_stashed);
#endif

static struct shadow_hook *vns_nsfs_lifecycle_hooks[] = {
	&nsfs_evict_hook,
	&nsfs_path_hook,
	NULL,
};

int vns_nsfs_lifecycle_init(void)
{
	int installed;

	if (vns_nsfs_hooks_active)
		return 0;
	mutex_lock(&vns_nsfs_lifecycle_lock);
	vns_nsfs_stopping = false;
	mutex_unlock(&vns_nsfs_lifecycle_lock);
	nsfs_evict_hook.quiesce_exempt = true;
	nsfs_path_hook.quiesce_exempt = true;

	installed = shadow_hook_install_all(vns_nsfs_lifecycle_hooks,
					    "vendor_kernel_nsfs");
	if (installed < 0) {
		shadow_hook_remove_all(vns_nsfs_lifecycle_hooks);
		return installed;
	}
	if (installed != 2) {
		shadow_hook_remove_all(vns_nsfs_lifecycle_hooks);
		LKM4CTR_ERR("vendor_kernel",
			    "required NSFS lifetime hook(s) unavailable (%d of 2 installed)",
			    installed);
		return -ENOENT;
	}
	if (!nsfs_evict_hook.retprobe_installed ||
	    !nsfs_path_hook.retprobe_installed) {
		shadow_hook_remove_all(vns_nsfs_lifecycle_hooks);
		LKM4CTR_ERR("vendor_kernel",
			    "required NSFS lifetime hook lacks its unload-safety retprobe");
		return -EOPNOTSUPP;
	}

	vns_nsfs_hooks_active = true;
	LKM4CTR_INFO("vendor_kernel",
		     "NSFS inode lifetime hooks installed; path helper=%s, eviction=nsfs_evict",
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 12, 0)
		     "__ns_get_path"
#else
		     "path_from_stashed"
#endif
		     );
	return 0;
}

void vns_nsfs_lifecycle_begin_exit(void)
{
	mutex_lock(&vns_nsfs_lifecycle_lock);
	vns_nsfs_stopping = true;
	mutex_unlock(&vns_nsfs_lifecycle_lock);
}

void vns_nsfs_lifecycle_abort_exit(void)
{
	mutex_lock(&vns_nsfs_lifecycle_lock);
	vns_nsfs_stopping = false;
	mutex_unlock(&vns_nsfs_lifecycle_lock);
}

int vns_nsfs_lifecycle_prepare_exit(void)
{
	int pins;

	mutex_lock(&vns_nsfs_lifecycle_lock);
	if (!vns_nsfs_stopping) {
		mutex_unlock(&vns_nsfs_lifecycle_lock);
		return -EINVAL;
	}
	pins = atomic_read(&vns_nsfs_inode_pin_count);
	mutex_unlock(&vns_nsfs_lifecycle_lock);
	if (pins) {
		LKM4CTR_WARN("vendor_kernel",
			     "refusing stop with %d live or fail-safe-pinned NSFS inode(s)",
			     pins);
		return -EBUSY;
	}
	return 0;
}

void vns_nsfs_lifecycle_exit(void)
{
	if (!vns_nsfs_hooks_active)
		return;

	if (atomic_read(&vns_nsfs_inode_pin_count)) {
		LKM4CTR_ERR("vendor_kernel",
			    "NSFS lifecycle hooks left installed because inode pins remain");
		return;
	}

	shadow_hook_remove_all(vns_nsfs_lifecycle_hooks);
	vns_nsfs_hooks_active = false;
}

int vns_nsfs_lifecycle_pin_count(void)
{
	return atomic_read(&vns_nsfs_inode_pin_count);
}
