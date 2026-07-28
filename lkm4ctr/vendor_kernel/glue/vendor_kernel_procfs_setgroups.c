// SPDX-License-Identifier: GPL-2.0
/*
 * vendor_kernel_procfs_setgroups.c - real file_operations callbacks
 * (open/read/write) for the fabricated /proc/<pid>/setgroups descriptor,
 * split out of glue/vendor_kernel_procfs.c into their own translation unit.
 *
 * Background: glue/vendor_kernel_procfs.o is compiled with Clang CFI
 * (CONFIG_CFI_CLANG) disabled (see lkm4ctr/Makefile's VNS_CFI_UNSAFE_OBJS),
 * because vendor_kernel_hook_newfstatat()/_stat()/_lstat() in that file
 * make unsafe indirect calls through real_sys_newfstatat/real_sys_stat/
 * real_sys_lstat, resolved by name via shadow_hook_resolve() rather than
 * declared with their real prototypes, which Clang's KCFI cannot verify at
 * the call site.
 *
 * vns_setgroups_open()/_read()/_write() below are the opposite case: real
 * file_operations callbacks the kernel's own VFS (vfs_write()/vfs_read()/
 * do_dentry_open(), via vns_setgroups_fops wired up in
 * vns_setgroups_create_fd()) calls back into indirectly once the fabricated
 * fd is opened. Those calls happen through a KCFI-checked indirect branch,
 * so the callees must keep ordinary CFI instrumentation to remain valid
 * indirect-call targets. Leaving them in vendor_kernel_procfs.o (whole
 * object CFI-disabled for the newfstatat/stat/lstat hooks above) strips
 * their own KCFI type-hash prefix too, producing a live
 * "CFI failure at vfs_write+... (target: vns_setgroups_write+...)" panic
 * the first time containerd/runc writes "deny" to a container's
 * /proc/<pid>/setgroups. Splitting them into their own translation unit
 * (kept out of lkm4ctr/Makefile's VNS_CFI_UNSAFE_OBJS) lets them keep
 * normal CFI instrumentation; only vns_setgroups_create_fd()'s own
 * outgoing resolved-pointer call (anon_inode_getfd_secure(), via
 * shadow_hook_resolve()) is marked __nocfi individually, mirroring
 * vendor_kernel_ipc_callbacks.c/vns_kill_litter_super().
 */
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/uaccess.h>
#include <linux/file.h>
#include <linux/fcntl.h>

#include "../vendor_kernel.h"
#include "../../../common/shadow_hook.h"
#include "../../../common/lkm4ctr_compat.h"

/*
 * /proc/<pid>/setgroups fabrication on kernels genuinely missing
 * CONFIG_USER_NS: see glue/vendor_kernel_procfs.c's vns_path_wants_setgroups()
 * for the full rationale. The fabricated descriptor stores a simple
 * one-way "allow" -> "deny" latch on its own private inode.
 */
static ssize_t vns_setgroups_read(struct file *file, char __user *ubuf,
				   size_t count, loff_t *ppos)
{
	bool deny = !!file_inode(file)->i_private;
	const char *str = deny ? "deny\n" : "allow\n";

	return simple_read_from_buffer(ubuf, count, ppos, str, strlen(str));
}

static ssize_t vns_setgroups_write(struct file *file, const char __user *ubuf,
				    size_t count, loff_t *ppos)
{
	struct inode *inode = file_inode(file);
	char kbuf[8];
	size_t n = min(count, sizeof(kbuf) - 1);

	if (copy_from_user(kbuf, ubuf, n))
		return -EFAULT;
	kbuf[n] = '\0';
	if (n && kbuf[n - 1] == '\n')
		kbuf[n - 1] = '\0';

	/*
	 * Real setgroups(7): "allow" is only a no-op re-affirmation of the
	 * default, "deny" latches permanently (a later "allow" is rejected
	 * once denied). No other value is accepted.
	 */
	if (!strcmp(kbuf, "deny")) {
		inode->i_private = (void *)1UL;
	} else if (strcmp(kbuf, "allow") || inode->i_private) {
		return -EINVAL;
	}

	*ppos += count;
	return count;
}

/*
 * Only ever invoked when this inode is opened a *second* time, through the
 * "/proc/thread-self/fd/<n>" magic-link reopen every modern
 * runc/containerd performs on a freshly-opened procfs fd -- see
 * vns_setgroups_create_fd() below for why this callback needs to exist at
 * all.
 */
static int vns_setgroups_open(struct inode *inode, struct file *file)
{
	return 0;
}

static const struct file_operations vns_setgroups_fops = {
	.owner		= THIS_MODULE,
	.open		= vns_setgroups_open,
	.read		= vns_setgroups_read,
	.write		= vns_setgroups_write,
	.llseek		= default_llseek,
};

typedef int (*vns_anon_inode_getfd_secure_fn)(const char *,
					       const struct file_operations *,
					       void *, int,
					       const struct inode *);

/*
 * __nocfi: this is the only CFI-unsafe indirect call in this file (through
 * anon_inode_getfd_secure_fn, resolved by name at runtime since it can be
 * trimmed from a GKI KMI's export table). vns_setgroups_open()/_read()/
 * _write() above are real file_operations callbacks the kernel's own VFS
 * calls back into indirectly once wired up via vns_setgroups_fops, and must
 * keep ordinary CFI instrumentation to remain valid indirect-call targets;
 * see this file's own header comment and lkm4ctr/Makefile's
 * VNS_CFI_UNSAFE_OBJS comment for why this whole object is therefore
 * deliberately *not* CFI-disabled.
 */
long __nocfi vns_setgroups_create_fd(void)
{
	vns_anon_inode_getfd_secure_fn anon_inode_getfd_secure_fn;
	struct file *file;
	int fd;

	/*
	 * anon_inode_getfd_secure() (not the plain, shared-singleton-inode
	 * anon_inode_getfd()) is required here so the magic-link reopen
	 * above succeeds, and the symbol still needs to be resolved via
	 * shadow_hook_resolve() rather than called directly.
	 */
	anon_inode_getfd_secure_fn = (vns_anon_inode_getfd_secure_fn)
		shadow_hook_resolve("anon_inode_getfd_secure");
	if (!anon_inode_getfd_secure_fn)
		return -ENOENT;

	fd = anon_inode_getfd_secure_fn("[vns_setgroups]", &vns_setgroups_fops,
					 NULL, O_RDWR | O_CLOEXEC, NULL);
	if (fd < 0)
		return fd;

	file = fget(fd);
	if (file) {
		file_inode(file)->i_fop = &vns_setgroups_fops;
		fput(file);
	}

	return fd;
}
