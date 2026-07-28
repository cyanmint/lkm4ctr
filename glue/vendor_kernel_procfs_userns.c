// SPDX-License-Identifier: GPL-2.0
/*
 * vendor_kernel_procfs_userns.c - real file_operations callbacks
 * (open/read/write/release) for the fabricated
 * /proc/<pid>/{uid_map,gid_map,projid_map,setgroups} descriptors, split out
 * of glue/vendor_kernel_procfs.c into their own translation unit.
 *
 * Background: glue/vendor_kernel_procfs.o is compiled with Clang CFI
 * (CONFIG_CFI_CLANG) disabled (see lkm4ctr/Makefile's VNS_CFI_UNSAFE_OBJS),
 * because vendor_kernel_hook_newfstatat()/_stat()/_lstat() in that file
 * make unsafe indirect calls through real_sys_newfstatat/real_sys_stat/
 * real_sys_lstat, resolved by name via shadow_hook_resolve() rather than
 * declared with their real prototypes, which Clang's KCFI cannot verify at
 * the call site.
 *
 * The callbacks below are the opposite case: real file_operations
 * callbacks the kernel's own VFS (vfs_write()/vfs_read()/do_dentry_open(),
 * via the per-kind fops wired up in vns_idmap_create_fd()) calls back into
 * indirectly once the fabricated fd is opened. Those calls happen through a
 * KCFI-checked indirect branch, so the callees must keep ordinary CFI
 * instrumentation to remain valid indirect-call targets. Leaving them in
 * vendor_kernel_procfs.o (whole object CFI-disabled for the newfstatat/
 * stat/lstat hooks above) strips their own KCFI type-hash prefix too,
 * producing a live "CFI failure at vfs_write+... (target:
 * vns_proc_setgroups_write+...)" panic the first time containerd/runc
 * writes "deny" to a container's /proc/<pid>/setgroups (this file
 * supersedes the older, disconnected vendor_kernel_procfs_setgroups.c,
 * which had exactly this split for the same reason but only ever latched a
 * private allow/deny flag instead of the real per-task user_namespace's
 * ns->flags). Splitting these callbacks into their own translation unit
 * (kept out of lkm4ctr/Makefile's VNS_CFI_UNSAFE_OBJS) lets them keep
 * normal CFI instrumentation; only vns_idmap_create_fd()'s own outgoing
 * resolved-pointer call (anon_inode_getfd_secure(), via
 * shadow_hook_resolve()) is marked __nocfi individually, mirroring
 * vendor_kernel_ipc_callbacks.c/vns_kill_litter_super().
 */
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/uaccess.h>
#include <linux/file.h>
#include <linux/fcntl.h>
#include <linux/sched.h>
#include <linux/pid.h>
#include <linux/cred.h>
#include <linux/seq_file.h>

#include "../vendor/vendor_kernel.h"
#include "shadow_hook.h"
#include "lkm4ctr_compat.h"

/*
 * The fabricated inode has no connection to the target task's real
 * /proc/<pid> directory (it is a fresh, unique anon_inode_getfd_secure()
 * inode), so the (pid, kind) it was created for is tagged onto
 * inode->i_private as a single packed integer: the low 2 bits carry
 * @kind (see enum vns_idmap_kind), the rest carry @pid. This lets a later
 * magic-link reopen (e.g. via "/proc/thread-self/fd/<n>", which every
 * modern runc/containerd performs on freshly-opened procfs fds as a
 * defensive "unsafe procfs" check) re-derive both without any separate
 * heap-allocated state to track or free.
 */
#define VNS_IDMAP_KIND_BITS	2
#define VNS_IDMAP_KIND_MASK	((1UL << VNS_IDMAP_KIND_BITS) - 1)

static inline void *vns_idmap_pack(pid_t pid, enum vns_idmap_kind kind)
{
	return (void *)(((unsigned long)(unsigned int)pid << VNS_IDMAP_KIND_BITS) |
			(unsigned long)kind);
}

static inline pid_t vns_idmap_unpack_pid(void *tag)
{
	return (pid_t)((unsigned long)tag >> VNS_IDMAP_KIND_BITS);
}

static inline enum vns_idmap_kind vns_idmap_unpack_kind(void *tag)
{
	return (enum vns_idmap_kind)((unsigned long)tag & VNS_IDMAP_KIND_MASK);
}

/*
 * vns_idmap_get_task_userns() - resolve @pid to its current real
 * user_namespace, taking a reference the caller must vns_put_user_ns().
 */
static struct user_namespace *vns_idmap_get_task_userns(pid_t pid)
{
	struct pid *kpid;
	struct task_struct *task;
	struct user_namespace *ns = NULL;

	kpid = find_get_pid(pid);
	if (!kpid)
		return NULL;

	task = get_pid_task(kpid, PIDTYPE_PID);
	put_pid(kpid);
	if (!task)
		return NULL;

	rcu_read_lock();
	ns = vns_get_user_ns(__task_cred(task)->user_ns);
	rcu_read_unlock();

	put_task_struct(task);
	return ns;
}

/*
 * Common .open path, used both directly at fd-creation time (on the
 * freshly-allocated struct file*, since alloc_file_pseudo() does not itself
 * invoke f_op->open()) and from the real .open callbacks below on a later
 * magic-link reopen.
 */
static int vns_idmap_do_open(struct inode *inode, struct file *file)
{
	void *tag = inode->i_private;
	pid_t pid = vns_idmap_unpack_pid(tag);
	enum vns_idmap_kind kind = vns_idmap_unpack_kind(tag);
	struct user_namespace *ns;
	struct seq_file *seq;
	int ret;

	ns = vns_idmap_get_task_userns(pid);
	if (!ns)
		return -ENOENT;

	if (kind == VNS_IDMAP_SETGROUPS)
		ret = single_open(file, vns_proc_setgroups_show, ns);
	else
		ret = seq_open(file, kind == VNS_IDMAP_UID ? &vns_proc_uid_seq_operations :
				kind == VNS_IDMAP_GID ? &vns_proc_gid_seq_operations :
							 &vns_proc_projid_seq_operations);
	if (ret) {
		vns_put_user_ns(ns);
		return ret;
	}

	seq = file->private_data;
	seq->private = ns;
	return 0;
}

static int vns_idmap_release(struct inode *inode, struct file *file)
{
	struct seq_file *seq = file->private_data;
	enum vns_idmap_kind kind = vns_idmap_unpack_kind(inode->i_private);

	/* NULL here only if vns_idmap_do_open() itself failed (see below). */
	if (!seq)
		return 0;

	vns_put_user_ns(seq->private);
	return kind == VNS_IDMAP_SETGROUPS ? single_release(inode, file) :
					      seq_release(inode, file);
}

/*
 * vns_idmap_do_open() (called both at fd-creation time, before the fd is
 * ever handed back to userspace, and from the real .open callbacks below on
 * a later magic-link reopen) is the only place that can fail with
 * file->private_data left unset -- anon_inode_getfd_secure() installs the
 * fd atomically with allocating the underlying struct file, so unlike a
 * real VFS open() there is no chance to undo the fd on failure here. Every
 * read/write/llseek callback below therefore defensively checks for that
 * (kzalloc-failure-only, effectively unreachable in practice) case rather
 * than assume seq_open()/single_open() always succeeded.
 */
static ssize_t vns_idmap_read(struct file *file, char __user *ubuf,
			       size_t count, loff_t *ppos)
{
	if (!file->private_data)
		return -EIO;
	return seq_read(file, ubuf, count, ppos);
}

static loff_t vns_idmap_llseek(struct file *file, loff_t offset, int whence)
{
	if (!file->private_data)
		return -EIO;
	return seq_lseek(file, offset, whence);
}

static ssize_t vns_idmap_uid_write(struct file *file, const char __user *buf,
				    size_t size, loff_t *ppos)
{
	if (!file->private_data)
		return -EIO;
	return vns_proc_uid_map_write(file, buf, size, ppos);
}

static ssize_t vns_idmap_gid_write(struct file *file, const char __user *buf,
				    size_t size, loff_t *ppos)
{
	if (!file->private_data)
		return -EIO;
	return vns_proc_gid_map_write(file, buf, size, ppos);
}

static ssize_t vns_idmap_projid_write(struct file *file, const char __user *buf,
				       size_t size, loff_t *ppos)
{
	if (!file->private_data)
		return -EIO;
	return vns_proc_projid_map_write(file, buf, size, ppos);
}

static ssize_t vns_idmap_setgroups_write(struct file *file, const char __user *buf,
					  size_t count, loff_t *ppos)
{
	if (!file->private_data)
		return -EIO;
	return vns_proc_setgroups_write(file, buf, count, ppos);
}

static int vns_idmap_uid_open(struct inode *inode, struct file *file)
{
	return vns_idmap_do_open(inode, file);
}

static int vns_idmap_gid_open(struct inode *inode, struct file *file)
{
	return vns_idmap_do_open(inode, file);
}

static int vns_idmap_projid_open(struct inode *inode, struct file *file)
{
	return vns_idmap_do_open(inode, file);
}

static int vns_idmap_setgroups_open(struct inode *inode, struct file *file)
{
	return vns_idmap_do_open(inode, file);
}

static const struct file_operations vns_idmap_fops[] = {
	[VNS_IDMAP_UID] = {
		.owner		= THIS_MODULE,
		.open		= vns_idmap_uid_open,
		.read		= vns_idmap_read,
		.write		= vns_idmap_uid_write,
		.llseek		= vns_idmap_llseek,
		.release	= vns_idmap_release,
	},
	[VNS_IDMAP_GID] = {
		.owner		= THIS_MODULE,
		.open		= vns_idmap_gid_open,
		.read		= vns_idmap_read,
		.write		= vns_idmap_gid_write,
		.llseek		= vns_idmap_llseek,
		.release	= vns_idmap_release,
	},
	[VNS_IDMAP_PROJID] = {
		.owner		= THIS_MODULE,
		.open		= vns_idmap_projid_open,
		.read		= vns_idmap_read,
		.write		= vns_idmap_projid_write,
		.llseek		= vns_idmap_llseek,
		.release	= vns_idmap_release,
	},
	[VNS_IDMAP_SETGROUPS] = {
		.owner		= THIS_MODULE,
		.open		= vns_idmap_setgroups_open,
		.read		= vns_idmap_read,
		.write		= vns_idmap_setgroups_write,
		.llseek		= vns_idmap_llseek,
		.release	= vns_idmap_release,
	},
};

typedef int (*vns_anon_inode_getfd_secure_fn)(const char *,
					       const struct file_operations *,
					       void *, int,
					       const struct inode *);

/*
 * __nocfi: this is the only CFI-unsafe indirect call in this file (through
 * anon_inode_getfd_secure_fn, resolved by name at runtime since it can be
 * trimmed from a GKI KMI's export table). vns_idmap_*_open()/_write()/
 * vns_idmap_release() above are real file_operations callbacks the
 * kernel's own VFS calls back into indirectly once wired up via
 * vns_idmap_fops[], and must keep ordinary CFI instrumentation to remain
 * valid indirect-call targets; see this file's own header comment and
 * lkm4ctr/Makefile's VNS_CFI_UNSAFE_OBJS comment for why this whole object
 * is therefore deliberately *not* CFI-disabled.
 */
long __nocfi vns_idmap_create_fd(pid_t pid, enum vns_idmap_kind kind)
{
	vns_anon_inode_getfd_secure_fn anon_inode_getfd_secure_fn;
	const struct file_operations *fops = &vns_idmap_fops[kind];
	static const char * const names[] = {
		[VNS_IDMAP_UID]		= "[vns_uid_map]",
		[VNS_IDMAP_GID]		= "[vns_gid_map]",
		[VNS_IDMAP_PROJID]	= "[vns_projid_map]",
		[VNS_IDMAP_SETGROUPS]	= "[vns_setgroups]",
	};
	struct file *file;
	struct inode *inode;
	int fd;

	if ((unsigned int)kind >= ARRAY_SIZE(vns_idmap_fops))
		return -EINVAL;

	/*
	 * anon_inode_getfd_secure() (not the plain, shared-singleton-inode
	 * anon_inode_getfd()) is required here so both the initial open
	 * below and any later magic-link reopen get their own private
	 * inode to tag with (pid, kind), and the symbol still needs to be
	 * resolved via shadow_hook_resolve() rather than called directly.
	 */
	anon_inode_getfd_secure_fn = (vns_anon_inode_getfd_secure_fn)
		shadow_hook_resolve("anon_inode_getfd_secure");
	if (!anon_inode_getfd_secure_fn)
		return -ENOENT;

	fd = anon_inode_getfd_secure_fn(names[kind], fops, NULL,
					 O_RDWR | O_CLOEXEC, NULL);
	if (fd < 0)
		return fd;

	/*
	 * fget() on an fd this task just installed a moment ago in its own
	 * fd table cannot fail in practice; if it somehow does (or the
	 * seq_open()/single_open() setup below fails, e.g. transient
	 * -ENOMEM), still return @fd unchanged rather than trying to
	 * unwind it -- every read/write/release callback above defensively
	 * checks for exactly this file->private_data-still-NULL case and
	 * fails safe with -EIO instead of dereferencing it, so the
	 * unusable fd is simply inert (and reclaimed normally on close())
	 * rather than a use-after-free/NULL-deref hazard.
	 */
	file = fget(fd);
	if (!file)
		return fd;

	inode = file_inode(file);
	inode->i_fop = fops;
	inode->i_private = vns_idmap_pack(pid, kind);

	/*
	 * alloc_file_pseudo() (inside anon_inode_getfd_secure()) does not
	 * itself call fops->open(), so perform the equivalent seq_open()/
	 * single_open() setup directly on this already-live file now.
	 */
	vns_idmap_do_open(inode, file);
	fput(file);
	return fd;
}
