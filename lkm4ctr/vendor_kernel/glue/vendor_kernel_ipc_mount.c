// SPDX-License-Identifier: GPL-2.0
/*
 * vendor_kernel_ipc_mount.c - proactive "/dev/mqueue" mount at module load
 * time, mirroring shadow_mqueue's own "Proactive /dev/mqueue creation" (see
 * ../../shadow_mqueue/README.md and shadow_mqueue/shadow_mqueue_mount.c).
 *
 * This is NEW code (not vendored from kernel-common).
 *
 * Why this exists: vns_mqueue_fs_init() (ipc/mqueue.c) registers a real,
 * fully-working POSIX mqueue filesystem under the actual "mqueue" name (see
 * mqueue_fs_type in ipc/mqueue.c), so an unmodified runc/containerd/dockerd's
 * own mount("mqueue", "/dev/mqueue", "mqueue", MS_NOSUID|MS_NODEV|MS_NOEXEC,
 * ...) call succeeds by finding it through the normal get_fs_type("mqueue")
 * lookup - no mount(2) hook is needed for that path, unlike shadow_mqueue
 * (which never registers a filesystem type under the real "mqueue" name at
 * all, see shadow_mqueue/README.md's "Architecture" section, and so has to
 * hook mount(2) itself to retry as tmpfs).
 *
 * But that only helps a *container's* mount(2) call. lkm4ctr.ko is typically
 * insmod'd late (e.g. as a KernelSU/Magisk post-fs-data module), well after
 * init.rc's own one-shot "mount mqueue mqueue /dev/mqueue ..." line already
 * ran on the host and silently failed with -ENODEV (init never retries a
 * failed boot-time mount). That leaves /dev/mqueue nonexistent, or an empty,
 * never-mounted directory, for the rest of boot on the *host* mount
 * namespace - exactly the scenario shadow_mqueue's proactive mount was
 * written to fix. So vendor_kernel does the same thing here: create
 * /dev/mqueue (if missing) and mount it directly, without waiting for any
 * mount(2) call that may never come.
 *
 * None of the VFS helpers this needs (path_mount(), vfs_mkdir(),
 * kern_path()/kern_path_create()/done_path_create(), path_put()) are called
 * directly by name: several of them are unexported or EXPORT_SYMBOL_NS()'d
 * under an allow-list namespace on various GKI branches, so (like the rest of
 * vendor_kernel, see glue/vendor_kernel_ipc_compat.c) they are resolved at
 * runtime via shadow_hook_resolve(), which walks kallsyms directly and
 * doesn't care whether a symbol is exported, namespaced, or trimmed. This
 * step is best-effort: if any helper fails to resolve, or the mount fails,
 * this logs it and leaves userspace's own reactive mount(2) call (against the
 * real "mqueue" registration above) as the fallback.
 */
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/mount.h>
#include <uapi/linux/mount.h>
#include <linux/version.h>

#include "../vendor_kernel.h"
#include "../../../common/shadow_hook.h"
#include "lkm4ctr_log.h"

#define VNS_MQ_DEV_MQUEUE_PATH "/dev/mqueue"
#define VNS_MQ_DEV_MQUEUE_MODE 0755

/*
 * vfs_mkdir()'s signature has changed twice upstream: it gained a
 * struct user_namespace * first parameter in v5.12, replaced by a
 * struct mnt_idmap * in v6.3. Same version split used by shadow_mqueue's own
 * copy of this shim (shadow_mqueue/shadow_mqueue_mount.c).
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
typedef int (*vns_mq_vfs_mkdir_fn)(struct mnt_idmap *, struct inode *,
				    struct dentry *, umode_t);
#define VNS_MQ_VFS_MKDIR(fn, dir, dentry, mode) \
	(fn)(&nop_mnt_idmap, (dir), (dentry), (mode))
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 12, 0)
typedef int (*vns_mq_vfs_mkdir_fn)(struct user_namespace *, struct inode *,
				    struct dentry *, umode_t);
#define VNS_MQ_VFS_MKDIR(fn, dir, dentry, mode) \
	(fn)(&init_user_ns, (dir), (dentry), (mode))
#else
typedef int (*vns_mq_vfs_mkdir_fn)(struct inode *, struct dentry *, umode_t);
#define VNS_MQ_VFS_MKDIR(fn, dir, dentry, mode) \
	(fn)((dir), (dentry), (mode))
#endif

typedef int (*vns_mq_kern_path_fn)(const char *, unsigned int, struct path *);
typedef struct dentry *(*vns_mq_kern_path_create_fn)(int, const char *,
						      struct path *,
						      unsigned int);
typedef void (*vns_mq_done_path_create_fn)(struct path *, struct dentry *);
typedef int (*vns_mq_path_mount_fn)(const char *, struct path *,
				     const char *, unsigned long, void *);
typedef void (*vns_mq_path_put_fn)(const struct path *);

/*
 * vns_mqueue_dev_do_mount() - mount at an already-resolved @path.
 *
 * Tries our own real "mqueue" filesystem type first (registered by
 * ipc/mqueue.c's vns_mqueue_fs_init(), see mqueue_fs_type there), which is
 * what makes the mounted /dev/mqueue an actually working POSIX mqueue rather
 * than shadow_mqueue's tmpfs stand-in. Falls back to tmpfs, same as
 * shadow_mqueue, only in the unexpected case that mount fails (e.g. the
 * "mqueue" name was already taken by the real kernel's own builtin
 * CONFIG_POSIX_MQUEUE=y filesystem and *that* mount somehow still fails here)
 * so /dev/mqueue is left as a working mountpoint either way.
 */
static int vns_mqueue_dev_do_mount(vns_mq_path_mount_fn path_mount_fn,
				    struct path *path)
{
	int ret;

	ret = path_mount_fn("mqueue", path, "mqueue",
			     MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
	if (ret)
		ret = path_mount_fn("mqueue", path, "tmpfs",
				     MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
	return ret;
}

void vns_mqueue_dev_ensure(void)
{
	vns_mq_kern_path_fn kern_path_fn;
	vns_mq_kern_path_create_fn kern_path_create_fn;
	vns_mq_done_path_create_fn done_path_create_fn;
	vns_mq_vfs_mkdir_fn vfs_mkdir_fn;
	vns_mq_path_mount_fn path_mount_fn;
	vns_mq_path_put_fn path_put_fn;
	struct path path;
	struct path create_path;
	struct dentry *dentry;
	int ret;

	kern_path_fn = (vns_mq_kern_path_fn)shadow_hook_resolve("kern_path");
	kern_path_create_fn = (vns_mq_kern_path_create_fn)
		shadow_hook_resolve("kern_path_create");
	done_path_create_fn = (vns_mq_done_path_create_fn)
		shadow_hook_resolve("done_path_create");
	vfs_mkdir_fn = (vns_mq_vfs_mkdir_fn)shadow_hook_resolve("vfs_mkdir");
	path_mount_fn = (vns_mq_path_mount_fn)shadow_hook_resolve("path_mount");
	path_put_fn = (vns_mq_path_put_fn)shadow_hook_resolve("path_put");

	if (!kern_path_fn || !kern_path_create_fn || !done_path_create_fn ||
	    !vfs_mkdir_fn || !path_mount_fn || !path_put_fn) {
		LKM4CTR_INFO("vendor_kernel",
			     "mqueue: could not resolve VFS helpers for proactive %s mount; relying on userspace's own reactive mount(2) instead",
			     VNS_MQ_DEV_MQUEUE_PATH);
		return;
	}

	ret = kern_path_fn(VNS_MQ_DEV_MQUEUE_PATH, LOOKUP_DIRECTORY, &path);
	if (!ret) {
		/*
		 * Something is already mounted at this path (real mqueue, a
		 * previous mount from an earlier load, ...): leave it alone.
		 * A dentry returned by a successful kern_path() always has a
		 * valid ->d_sb, so this is safe to dereference without a
		 * NULL check.
		 */
		if (path.dentry == path.dentry->d_sb->s_root) {
			LKM4CTR_INFO("vendor_kernel",
				     "mqueue: %s is already a mountpoint; leaving it as-is",
				     VNS_MQ_DEV_MQUEUE_PATH);
			path_put_fn(&path);
			return;
		}

		ret = vns_mqueue_dev_do_mount(path_mount_fn, &path);
		path_put_fn(&path);
		if (ret)
			LKM4CTR_INFO("vendor_kernel",
				     "mqueue: proactive mount on existing %s failed: %d",
				     VNS_MQ_DEV_MQUEUE_PATH, ret);
		else
			LKM4CTR_INFO("vendor_kernel", "mqueue: mounted %s", VNS_MQ_DEV_MQUEUE_PATH);
		return;
	}

	if (ret != -ENOENT) {
		LKM4CTR_INFO("vendor_kernel", "mqueue: kern_path(%s) failed: %d",
			     VNS_MQ_DEV_MQUEUE_PATH, ret);
		return;
	}

	/* /dev/mqueue does not exist yet: create it, then mount on it. */
	dentry = kern_path_create_fn(AT_FDCWD, VNS_MQ_DEV_MQUEUE_PATH,
				      &create_path, LOOKUP_DIRECTORY);
	if (IS_ERR(dentry)) {
		LKM4CTR_INFO("vendor_kernel", "mqueue: kern_path_create(%s) failed: %ld",
			     VNS_MQ_DEV_MQUEUE_PATH, PTR_ERR(dentry));
		return;
	}

	ret = VNS_MQ_VFS_MKDIR(vfs_mkdir_fn, d_inode(create_path.dentry), dentry,
			       VNS_MQ_DEV_MQUEUE_MODE);
	done_path_create_fn(&create_path, dentry);
	if (ret) {
		LKM4CTR_INFO("vendor_kernel", "mqueue: mkdir(%s) failed: %d",
			     VNS_MQ_DEV_MQUEUE_PATH, ret);
		return;
	}

	ret = kern_path_fn(VNS_MQ_DEV_MQUEUE_PATH, LOOKUP_DIRECTORY, &path);
	if (ret) {
		LKM4CTR_INFO("vendor_kernel", "mqueue: kern_path(%s) failed after mkdir: %d",
			     VNS_MQ_DEV_MQUEUE_PATH, ret);
		return;
	}

	ret = vns_mqueue_dev_do_mount(path_mount_fn, &path);
	path_put_fn(&path);
	if (ret)
		LKM4CTR_INFO("vendor_kernel", "mqueue: created %s but mount failed: %d",
			     VNS_MQ_DEV_MQUEUE_PATH, ret);
	else
		LKM4CTR_INFO("vendor_kernel", "mqueue: created and mounted %s", VNS_MQ_DEV_MQUEUE_PATH);
}
