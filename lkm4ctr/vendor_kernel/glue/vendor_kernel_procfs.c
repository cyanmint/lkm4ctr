// SPDX-License-Identifier: GPL-2.0
/*
 * vendor_kernel_procfs.c - fabricate /proc/<pid>/ns/{ipc,pid}'s readlink(2)
 * target and stat(2)/lstat(2)/newfstatat(2) success on kernels genuinely
 * missing CONFIG_IPC_NS/CONFIG_PID_NS.
 *
 * This is NEW code (not vendored from kernel-common).
 *
 * Why this exists
 * ----------------
 * vendor_kernel_hook_unshare()/vendor_kernel_hook_clone*()
 * (glue/vendor_kernel_syscalls.c) already do real work for
 * CLONE_NEWIPC/CLONE_NEWPID: they build a genuine vendored `struct
 * ipc_namespace`/`struct pid_namespace` (ipc/namespace.c's
 * create_ipc_ns()/kernel/pid_namespace.c's vns_copy_pid_ns(), both ported
 * near-verbatim from kernel-common) and install it on the calling task via
 * vns_switch_task_namespaces() (IPC) or the real copy_process()/alloc_pid()
 * path (PID, unconditional of CONFIG_PID_NS), exactly like the real
 * unshare(2)/clone(2) would. SysV IPC/POSIX mqueue syscalls and
 * getpid()/proc/kill(2)/etc. observe this correctly through
 * vns_current_ipc_ns() (glue/vendor_kernel_ipc_syscalls.c) and the real,
 * unconditional task_active_pid_ns() respectively.
 *
 * However, nothing about that makes /proc/<pid>/ns/ipc or /proc/<pid>/ns/pid
 * themselves resolvable. fs/proc/namespaces.c's `ns_entries[]` table only
 * includes `&ipcns_operations`/`&pidns_operations` `#ifdef
 * CONFIG_IPC_NS`/`#ifdef CONFIG_PID_NS` -- a compile-time decision baked
 * into vmlinux. On a kernel genuinely built with CONFIG_IPC_NS=n/
 * CONFIG_PID_NS=n (the whole reason vendor_kernel's IPC/PID namespaces
 * exist), that procfs directory entry does not exist at all, regardless of
 * what task->nsproxy->ipc_ns/task_active_pid_ns(task) actually point to:
 * readlink(2) on that path fails with plain -ENOENT.
 *
 * That breaks two independent things:
 *   - runc/containerd's own namespace-support probe (stat(2), not
 *     readlink(2) -- see the stat(2)-family fabrication further down this
 *     file), which checks /proc/<pid>/ns/{ipc,pid,user,uts,...} as part of a
 *     single combined check before issuing unshare()/clone3(), and again
 *     before `docker exec` joins a running container's namespaces. Without
 *     this fabrication, that probe fails outright and `docker exec`/`docker
 *     run` abort with `"OCI runtime exec failed: ... namespace NEWPID is
 *     not supported: unknown"` (or the analogous NEWIPC message).
 *   - any external tool (including lkm4ctr_checker) that verifies real
 *     namespace isolation by diffing /proc/self/ns/{ipc,pid}'s readlink(2)
 *     target before and after unshare(CLONE_NEWIPC)/unshare(CLONE_NEWPID):
 *     with no fabrication, the "after" readlink still fails with -ENOENT
 *     exactly like the "before" one, so the diff-based probe cannot observe
 *     vendor_kernel's real, already-working namespace isolation and reports
 *     a false STUB.
 *
 * This file closes both observability gaps: hook readlink(2)/readlinkat(2)
 * (for the first gap above) and stat(2)/lstat(2)/newfstatat(2) (for the
 * runc/containerd probe -- see the block comment further down for how that
 * one is implemented), let the real syscall run first, and only when it
 * fails with -ENOENT for a path unambiguously naming ".../ns/ipc" or
 * ".../ns/pid" under a procfs-rooted pid directory
 * (".../<pid|self|thread-self>/ns/{ipc,pid}", or a bare "ns/{ipc,pid}"
 * resolved relative to a dfd whose superblock is procfs) do we step in. For
 * readlink(2)/readlinkat(2) that means fabricating the "ipc:[<ino>]"/
 * "pid:[<ino>]" text real readlink(2) would have produced, mirroring
 * fs/nsfs.c's ns_get_name() format exactly. Any other -ENOENT (including
 * every other ns/ entry) passes through untouched.
 *
 * The fabricated inode number comes straight from vns_task_ipc_ns(task)'s /
 * task_active_pid_ns(task)'s `ns.inum` (allocated by vns_alloc_inum() -- see
 * ipc/namespace.c's create_ipc_ns()/kernel/pid_namespace.c's
 * vns_copy_pid_ns() and vns_ipc_default_init() below), so it is self
 * consistent with the real vendored namespace object the SysV/mqueue/pid
 * syscalls already operate against: a task that never
 * unshare(CLONE_NEWIPC)/unshare(CLONE_NEWPID) reads back vendor_kernel's own
 * default namespace's inum, and a task that did reads back a distinct one,
 * exactly matching real kernel semantics.
 *
 * Only IPC and PID get a fabricated entry here: UTS and USER namespaces are
 * likewise vendored unconditionally of their respective CONFIG_*_NS option,
 * but every device observed lacking CONFIG_IPC_NS/CONFIG_PID_NS still ships
 * CONFIG_UTS_NS=y/CONFIG_USER_NS=y, so no equivalent gap has been observed
 * for them; NET/MNT/CGROUP namespaces remain deliberately bookkeeping-only,
 * so fabricating their /proc/ns entries here would misreport nonexistent
 * isolation as real.
 */
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/uaccess.h>
#include <linux/ctype.h>
#include <linux/file.h>
#include <linux/fcntl.h>
#include <linux/magic.h>
#include <linux/pid.h>
#include <linux/pid_namespace.h>
#include <linux/sched.h>
#include <asm/ptrace.h>

#include "../vendor_kernel.h"
#include "../../../common/shadow_hook.h"
#include "../../../common/lkm4ctr_compat.h"

#define VNS_PROC_NS_IPC_NAME	"ipc"
#define VNS_PROC_NS_PID_NAME	"pid"

#if defined(CONFIG_ARM64)
static unsigned long vns_procfs_arg0(const struct pt_regs *regs) { return regs->regs[0]; }
static unsigned long vns_procfs_arg1(const struct pt_regs *regs) { return regs->regs[1]; }
static unsigned long vns_procfs_arg2(const struct pt_regs *regs) { return regs->regs[2]; }
static unsigned long vns_procfs_arg3(const struct pt_regs *regs) { return regs->regs[3]; }
#elif defined(CONFIG_X86_64)
static unsigned long vns_procfs_arg0(const struct pt_regs *regs) { return regs->di; }
static unsigned long vns_procfs_arg1(const struct pt_regs *regs) { return regs->si; }
static unsigned long vns_procfs_arg2(const struct pt_regs *regs) { return regs->dx; }
static unsigned long vns_procfs_arg3(const struct pt_regs *regs) { return regs->r10; }
#else
#error "vendor_kernel: unsupported architecture"
#endif

/* Bare "self"/"thread-self"/numeric-pid path component used for procfs pid
 * path parsing in the namespace-fallback helpers below. */
static bool vns_component_is_pid_dir(const char *s)
{
	if (!*s)
		return false;
	if (!strcmp(s, "self") || !strcmp(s, "thread-self"))
		return true;
	for (; *s; s++) {
		if (!isdigit((unsigned char)*s))
			return false;
	}
	return true;
}

/*
 * A bare "ns/ipc" path component with no directory prefix only makes sense
 * relative to a dfd that is itself rooted somewhere inside a procfs mount
 * (a detached fsopen("proc")+fsmount() dirfd, or an already-open
 * /proc/<pid> directory fd).
 */
static bool vns_dfd_is_procfs(int dfd)
{
	struct fd f;
	bool ret;

	if (dfd == AT_FDCWD)
		return false;

	f = fdget(dfd);
	if (fd_empty(f))
		return false;
	ret = fd_file(f)->f_path.dentry->d_sb->s_magic == PROC_SUPER_MAGIC;
	fdput(f);
	return ret;
}

/*
 * vns_resolve_ns_pid() - resolve the "self"/"thread-self"/numeric path
 * component immediately preceding "/ns/{ipc,pid}" to the real (host) pid it
 * names. Numeric components are taken to already be real pids: vendor_kernel's
 * vendored PID namespace installs the real task->nsproxy directly, so no
 * separate vpid<->rpid translation table is needed here.
 */
static pid_t vns_resolve_ns_pid(const char *comp)
{
	long val;

	if (!strcmp(comp, "self"))
		return task_tgid_nr(current);
	if (!strcmp(comp, "thread-self"))
		return task_pid_nr(current);
	if (kstrtol(comp, 10, &val) || val <= 0 || val > INT_MAX)
		return 0;
	return (pid_t)val;
}

/*
 * vns_path_is_ns_entry() - does @upath (relative to @dfd) name
 * ".../<piddir>/ns/<ns_name>"? If so, resolves the owning task's real pid
 * into *rpid and returns true. Returns false otherwise (including on any
 * parse failure) -- callers must fall back to the real syscall unchanged.
 *
 * @ns_name is one of VNS_PROC_NS_IPC_NAME/VNS_PROC_NS_PID_NAME (both
 * exactly 3 bytes, matching vns_swap_leaf_to_mnt()'s assumption below).
 *
 * @out_len, if non-NULL, receives the total length (excluding the NUL
 * terminator) of the user-supplied path string on a true return, so callers
 * that need to locate the trailing @ns_name component's user address (see
 * vns_ns_fstat_fallback() below) don't have to re-parse @upath a second
 * time.
 */
static bool vns_path_is_ns_entry(int dfd, const char __user *upath,
				  const char *ns_name, pid_t *rpid,
				  long *out_len)
{
	char buf[192];
	char want[16];
	char *base, *slash1, *piddir;
	long n;

	if (!upath)
		return false;

	n = strncpy_from_user(buf, upath, sizeof(buf));
	if (n <= 0 || n >= sizeof(buf))
		return false;

	if (buf[0] == '/') {
		if (strncmp(buf, "/proc/", 6))
			return false;
		base = buf + 6;
	} else {
		if (dfd == AT_FDCWD || !vns_dfd_is_procfs(dfd))
			return false;
		base = buf;
	}

	slash1 = strchr(base, '/');
	if (!slash1)
		return false;
	*slash1 = '\0';
	piddir = base;
	if (!vns_component_is_pid_dir(piddir))
		return false;

	slash1++;
	snprintf(want, sizeof(want), "ns/%s", ns_name);
	if (strcmp(slash1, want))
		return false;

	*rpid = vns_resolve_ns_pid(piddir);
	if (*rpid <= 0)
		return false;
	if (out_len)
		*out_len = n;
	return true;
}

/*
 * vns_ipc_ns_inum() - resolve @task's effective ipc_namespace inode number
 * into *inum. Returns false (leaving *inum untouched) if @task has none.
 */
static bool vns_ipc_ns_inum(struct task_struct *task, unsigned int *inum)
{
	struct ipc_namespace *ns;

	/*
	 * vns_task_ipc_ns() returns a borrowed pointer into
	 * task->nsproxy->ipc_ns with no reference of its own, and reads
	 * task->nsproxy without task_lock(). Both the pointer read and the
	 * ref-get must happen under task_lock(task), the same lock every
	 * nsproxy-swapping path (vns_task_exit_cleanup(), unshare(2), setns(2))
	 * takes before replacing/freeing task->nsproxy -- otherwise @task
	 * could swap/free its nsproxy concurrently between the lookup above
	 * and the vns_ipc_get_ref() below, leaving @ns dangling. The real
	 * kernel's get_ipc_ns()/put_ipc_ns() are also no-ops when
	 * CONFIG_IPC_NS=n (see <linux/ipc_namespace.h>) -- exactly the config
	 * this whole file exists for -- so vns_ipc_get_ref()/vns_put_ipc_ns()
	 * (the same refcount vendor_kernel's own put path already maintains
	 * unconditionally) must be used instead.
	 */
	task_lock(task);
	ns = vns_task_ipc_ns(task);
	if (ns)
		vns_ipc_get_ref(ns);
	task_unlock(task);
	if (!ns)
		return false;

	*inum = ns->ns.inum;
	vns_put_ipc_ns(ns);
	return true;
}

/*
 * vns_pid_ns_inum() - resolve @task's active pid_namespace inode number
 * into *inum. Returns false (leaving *inum untouched) if @task has none.
 *
 * task_active_pid_ns() (real kernel API, unconditional of CONFIG_PID_NS)
 * must be read under rcu_read_lock() -- the same protection
 * kernel/pid_namespace.c's own pidns_get() uses -- before taking the
 * vendored refcount via vns_get_pid_ns()/vns_put_pid_ns().
 */
static bool vns_pid_ns_inum(struct task_struct *task, unsigned int *inum)
{
	struct pid_namespace *ns;

	rcu_read_lock();
	ns = task_active_pid_ns(task);
	if (ns)
		vns_get_pid_ns(ns);
	rcu_read_unlock();
	if (!ns)
		return false;

	*inum = ns->ns.inum;
	vns_put_pid_ns(ns);
	return true;
}

/*
 * vns_ns_readlink() - fabricate the "<ns_name>:[<ino>]" symlink target text
 * real readlink(2) on .../ns/<ns_name> would return, for the task named by
 * @rpid, using @get_inum to resolve that task's namespace inode number.
 * Returns the string length copied (>= 0) on success, or a negative errno.
 */
static long vns_ns_readlink(pid_t rpid, const char *ns_name,
			     bool (*get_inum)(struct task_struct *, unsigned int *),
			     char __user *ubuf, int bufsiz)
{
	struct pid *pid;
	struct task_struct *task;
	unsigned int inum;
	char name[32];
	int n;

	if (bufsiz < 0)
		return -EINVAL;

	pid = find_get_pid(rpid);
	if (!pid)
		return -ENOENT;

	task = get_pid_task(pid, PIDTYPE_PID);
	put_pid(pid);
	if (!task)
		return -ENOENT;

	if (!get_inum(task, &inum)) {
		put_task_struct(task);
		return -ENOENT;
	}
	put_task_struct(task);

	n = snprintf(name, sizeof(name), "%s:[%u]", ns_name, inum);
	if (n < 0)
		return -ENOENT;

	if (bufsiz > n)
		bufsiz = n;
	if (copy_to_user(ubuf, name, bufsiz))
		return -EFAULT;
	return bufsiz;
}

/*
 * vns_ns_readlink_fallback() - shared -ENOENT fallback for
 * readlink(2)/readlinkat(2): only reached once the real syscall has already
 * failed to resolve the caller's original path. Tries ".../ns/ipc" then
 * ".../ns/pid"; returns @ret unchanged if neither matches.
 */
static long vns_ns_readlink_fallback(int dfd, const char __user *upath,
				      long ret, char __user *ubuf, int bufsiz)
{
	pid_t rpid;

	if (ret != -ENOENT)
		return ret;

	if (vns_path_is_ns_entry(dfd, upath, VNS_PROC_NS_IPC_NAME, &rpid, NULL))
		return vns_ns_readlink(rpid, VNS_PROC_NS_IPC_NAME,
					vns_ipc_ns_inum, ubuf, bufsiz);
	if (vns_path_is_ns_entry(dfd, upath, VNS_PROC_NS_PID_NAME, &rpid, NULL))
		return vns_ns_readlink(rpid, VNS_PROC_NS_PID_NAME,
					vns_pid_ns_inum, ubuf, bufsiz);

	return ret;
}

static long (*real_sys_readlinkat)(const struct pt_regs *regs);
static long (*real_sys_readlink)(const struct pt_regs *regs);

static long vendor_kernel_hook_readlinkat(const struct pt_regs *regs)
{
	int dfd = (int)vns_procfs_arg0(regs);
	const char __user *upath =
		(const char __user *)(uintptr_t)vns_procfs_arg1(regs);
	char __user *ubuf =
		(char __user *)(uintptr_t)vns_procfs_arg2(regs);
	int bufsiz = (int)vns_procfs_arg3(regs);
	long ret = real_sys_readlinkat(regs);

	return vns_ns_readlink_fallback(dfd, upath, ret, ubuf, bufsiz);
}

static long vendor_kernel_hook_readlink(const struct pt_regs *regs)
{
	const char __user *upath =
		(const char __user *)(uintptr_t)vns_procfs_arg0(regs);
	char __user *ubuf =
		(char __user *)(uintptr_t)vns_procfs_arg1(regs);
	int bufsiz = (int)vns_procfs_arg2(regs);
	long ret = real_sys_readlink(regs);

	return vns_ns_readlink_fallback(AT_FDCWD, upath, ret, ubuf, bufsiz);
}

/*
 * /proc/<pid>/setgroups fabrication on kernels genuinely missing
 * CONFIG_USER_NS: fs/proc/base.c only wires up the "setgroups" per-pid dentry
 * "#ifdef CONFIG_USER_NS", so modern runc/containerd's unconditional
 * open()/openat2() sanity-check of "self/setgroups" as part of its "is this
 * really an unrestricted procfs" probe fails with plain -ENOENT and aborts
 * container creation with "unsafe procfs detected", independent of whether
 * the container itself asked for a new user namespace.
 *
 * The fabricated descriptor stores a simple one-way "allow" -> "deny" latch
 * on its own private inode (via anon_inode_getfd_secure(), resolved through
 * shadow_hook_resolve() for the same CONFIG_TRIM_UNUSED_KSYMS reasons as the
 * rest of this file), rather than being wired to vendor_kernel's own real
 * per-task user_namespace
 * (kernel/user_namespace.c's vns_proc_setgroups_show()/_write(), reachable
 * via current_cred()->user_ns) -- reproducing the exact allow/deny/
 * gid-map-set interactions real setgroups(7) has with a specific
 * unshare(CLONE_NEWUSER)'d namespace is unnecessary complexity for what
 * every observed caller only ever treats as a one-shot defensive probe.
 *
 * The actual file_operations callbacks (open/read/write) and
 * vns_setgroups_create_fd() itself live in
 * glue/vendor_kernel_procfs_setgroups.c, a separate translation unit kept
 * out of lkm4ctr/Makefile's VNS_CFI_UNSAFE_OBJS: unlike the
 * newfstatat/stat/lstat hooks below, those callbacks are called back into
 * indirectly by the kernel's own (KCFI-checked) VFS and must keep ordinary
 * CFI instrumentation to remain valid indirect-call targets. See that
 * file's header comment for the full rationale.
 */
long vns_setgroups_create_fd(void);

static bool vns_path_wants_setgroups(int dfd, const char __user *upath)
{
	char buf[192];
	long n;
	char *slash, *base, *dir_last;

	if (!upath)
		return false;

	n = strncpy_from_user(buf, upath, sizeof(buf));
	if (n <= 0 || n >= sizeof(buf))
		return false;

	slash = strrchr(buf, '/');
	base = slash ? slash + 1 : buf;
	if (strcmp(base, "setgroups"))
		return false;

	if (!slash)
		return vns_dfd_is_procfs(dfd);

	*slash = '\0';
	dir_last = strrchr(buf, '/');
	dir_last = dir_last ? dir_last + 1 : buf;
	return vns_component_is_pid_dir(dir_last);
}

static long (*real_sys_openat2)(const struct pt_regs *regs);
static long (*real_sys_openat)(const struct pt_regs *regs);
static long (*real_sys_open)(const struct pt_regs *regs);

/*
 * vns_open_fallback() - shared -ENOENT fallback for openat2/openat/open:
 * only reached once the real syscall has already failed to open the path.
 * Fabricates the "setgroups" leaf when the path matches; returns @ret
 * unchanged otherwise.
 */
static long vns_open_fallback(int dfd, const char __user *upath, long ret)
{
	if (ret != -ENOENT)
		return ret;

	if (vns_path_wants_setgroups(dfd, upath))
		return vns_setgroups_create_fd();

	return ret;
}

static long vendor_kernel_hook_openat2(const struct pt_regs *regs)
{
	int dfd = (int)vns_procfs_arg0(regs);
	const char __user *upath =
		(const char __user *)(uintptr_t)vns_procfs_arg1(regs);
	long ret = real_sys_openat2(regs);

	return vns_open_fallback(dfd, upath, ret);
}

static long vendor_kernel_hook_openat(const struct pt_regs *regs)
{
	int dfd = (int)vns_procfs_arg0(regs);
	const char __user *upath =
		(const char __user *)(uintptr_t)vns_procfs_arg1(regs);
	long ret = real_sys_openat(regs);

	return vns_open_fallback(dfd, upath, ret);
}

static long vendor_kernel_hook_open(const struct pt_regs *regs)
{
	const char __user *upath =
		(const char __user *)(uintptr_t)vns_procfs_arg0(regs);
	long ret = real_sys_open(regs);

	return vns_open_fallback(AT_FDCWD, upath, ret);
}

static const char * const vendor_kernel_openat2_names[] = {
	"__arm64_sys_openat2", "__x64_sys_openat2", "sys_openat2", NULL,
};
static const char * const vendor_kernel_openat_names[] = {
	"__arm64_sys_openat", "__x64_sys_openat", "sys_openat", NULL,
};
static const char * const vendor_kernel_open_names[] = {
	"__arm64_sys_open", "__x64_sys_open", "sys_open", NULL,
};

static struct shadow_hook vendor_kernel_openat2_hook =
	SHADOW_HOOK(vendor_kernel_openat2_names, vendor_kernel_hook_openat2,
		    &real_sys_openat2);
static struct shadow_hook vendor_kernel_openat_hook =
	SHADOW_HOOK(vendor_kernel_openat_names, vendor_kernel_hook_openat,
		    &real_sys_openat);
static struct shadow_hook vendor_kernel_open_hook =
	SHADOW_HOOK(vendor_kernel_open_names, vendor_kernel_hook_open,
		    &real_sys_open);

static const char * const vendor_kernel_readlinkat_names[] = {
	"__arm64_sys_readlinkat", "__x64_sys_readlinkat", "sys_readlinkat",
	NULL,
};
static const char * const vendor_kernel_readlink_names[] = {
	"__arm64_sys_readlink", "__x64_sys_readlink", "sys_readlink", NULL,
};

static struct shadow_hook vendor_kernel_readlinkat_hook =
	SHADOW_HOOK(vendor_kernel_readlinkat_names, vendor_kernel_hook_readlinkat,
		    &real_sys_readlinkat);
static struct shadow_hook vendor_kernel_readlink_hook =
	SHADOW_HOOK(vendor_kernel_readlink_names, vendor_kernel_hook_readlink,
		    &real_sys_readlink);

/*
 * stat(2)/lstat(2)/newfstatat(2) fabrication for /proc/<pid>/ns/{ipc,pid}.
 *
 * readlink(2)/readlinkat(2) above only fixes tools that explicitly read the
 * symlink *target* text. runc/containerd's actual namespace-support probe
 * (libcontainer/configs.IsNamespaceSupported(), consulted both before
 * `docker run` and before `docker exec` joins a running container's
 * namespaces) never reads the target at all: it only calls stat(2) on the
 * path and checks whether the call itself succeeds ("os.Stat(...);
 * supported = err == nil"). Since fs/proc/namespaces.c's ns_entries[] table
 * omits &ipcns_operations/&pidns_operations entirely #ifndef
 * CONFIG_IPC_NS/CONFIG_PID_NS, that stat(2) call fails with plain -ENOENT
 * regardless of the readlink(2) fabrication above, and runc reports
 * "namespace NEWIPC/NEWPID is not supported" (surfacing as "OCI runtime
 * exec failed: ... namespace NEWPID is not supported" on `docker exec`).
 *
 * The stat(2) family has no equivalent "just fabricate the numbers" path
 * here: struct stat's on-wire layout is architecture-specific (arm64 uses
 * the asm-generic layout, x86_64 its own), and none of the kernel's own
 * construction code (fs/stat.c's cp_new_stat() et al) is exported for
 * reuse. Rather than hand-roll and risk getting either arch's field layout
 * wrong, this reuses the real kernel's own (guaranteed ABI-correct)
 * newfstatat(2)/stat(2)/lstat(2) implementation by transparently
 * substituting the ".../ns/ipc" or ".../ns/pid" leaf for ".../ns/mnt" --
 * exactly the same length in both cases, so the substitution can be done in
 * place on the caller's own path buffer with no reallocation -- before
 * calling through to the real syscall. The mount namespace entry is the one
 * /proc/<pid>/ns/ entry that is never Kconfig-gated (mount namespaces are
 * core kernel functionality, not an optional CONFIG_*_NS symbol), so it is
 * always present to redirect to. Every caller of this stat(2) family only
 * cares whether the call succeeds or fails (see above), not which
 * namespace's numbers come back, so borrowing the mnt namespace's stat(2)
 * result is harmless. The path bytes are restored to the original leaf
 * immediately afterwards so the caller's buffer is left exactly as it was.
 */
static bool vns_swap_leaf_to_mnt(const char __user *upath, long len)
{
	if (len < 3)
		return false;
	if (copy_to_user((char __user *)upath + len - 3, "mnt", 3))
		return false;
	return true;
}

static void vns_restore_mnt_leaf(const char __user *upath, long len,
				  const char *orig_leaf)
{
	/* Best-effort: nothing sane to do if this copy fails. */
	unsigned long unused = copy_to_user((char __user *)upath + len - 3,
					     orig_leaf, 3);
	(void)unused;
}

/*
 * vns_ns_fstat_fallback() - shared -ENOENT fallback for
 * newfstatat/stat/lstat: only reached once the real syscall already failed
 * to stat the caller's original path. Returns @ret unchanged unless the
 * path names ".../ns/ipc" or ".../ns/pid", in which case it retries the
 * real syscall against ".../ns/mnt" instead (see the block comment above).
 */
static long vns_ns_fstat_fallback(int dfd, const char __user *upath,
				   long ret,
				   long (*real_stat_fn)(const struct pt_regs *),
				   const struct pt_regs *regs)
{
	pid_t rpid;
	long len;
	const char *ns_name;

	if (ret != -ENOENT)
		return ret;

	if (vns_path_is_ns_entry(dfd, upath, VNS_PROC_NS_IPC_NAME, &rpid, &len))
		ns_name = VNS_PROC_NS_IPC_NAME;
	else if (vns_path_is_ns_entry(dfd, upath, VNS_PROC_NS_PID_NAME, &rpid,
				       &len))
		ns_name = VNS_PROC_NS_PID_NAME;
	else
		return ret;

	if (!vns_swap_leaf_to_mnt(upath, len))
		return ret;

	ret = real_stat_fn(regs);
	vns_restore_mnt_leaf(upath, len, ns_name);
	return ret;
}

static long (*real_sys_newfstatat)(const struct pt_regs *regs);
static long (*real_sys_stat)(const struct pt_regs *regs);
static long (*real_sys_lstat)(const struct pt_regs *regs);

static long vendor_kernel_hook_newfstatat(const struct pt_regs *regs)
{
	int dfd = (int)vns_procfs_arg0(regs);
	const char __user *upath =
		(const char __user *)(uintptr_t)vns_procfs_arg1(regs);
	long ret = real_sys_newfstatat(regs);

	return vns_ns_fstat_fallback(dfd, upath, ret, real_sys_newfstatat,
				      regs);
}

static long vendor_kernel_hook_stat(const struct pt_regs *regs)
{
	const char __user *upath =
		(const char __user *)(uintptr_t)vns_procfs_arg0(regs);
	long ret = real_sys_stat(regs);

	return vns_ns_fstat_fallback(AT_FDCWD, upath, ret, real_sys_stat,
				      regs);
}

static long vendor_kernel_hook_lstat(const struct pt_regs *regs)
{
	const char __user *upath =
		(const char __user *)(uintptr_t)vns_procfs_arg0(regs);
	long ret = real_sys_lstat(regs);

	return vns_ns_fstat_fallback(AT_FDCWD, upath, ret, real_sys_lstat,
				      regs);
}

static const char * const vendor_kernel_newfstatat_names[] = {
	"__arm64_sys_newfstatat", "__x64_sys_newfstatat", "sys_newfstatat",
	NULL,
};
static const char * const vendor_kernel_stat_names[] = {
	"__arm64_sys_stat", "__x64_sys_newstat", "__x64_sys_stat", "sys_stat",
	NULL,
};
static const char * const vendor_kernel_lstat_names[] = {
	"__arm64_sys_lstat", "__x64_sys_newlstat", "__x64_sys_lstat",
	"sys_lstat", NULL,
};

static struct shadow_hook vendor_kernel_newfstatat_hook =
	SHADOW_HOOK(vendor_kernel_newfstatat_names,
		    vendor_kernel_hook_newfstatat, &real_sys_newfstatat);
static struct shadow_hook vendor_kernel_stat_hook =
	SHADOW_HOOK(vendor_kernel_stat_names, vendor_kernel_hook_stat,
		    &real_sys_stat);
static struct shadow_hook vendor_kernel_lstat_hook =
	SHADOW_HOOK(vendor_kernel_lstat_names, vendor_kernel_hook_lstat,
		    &real_sys_lstat);

struct shadow_hook *vendor_kernel_procfs_hooks[] = {
	&vendor_kernel_openat2_hook,
	&vendor_kernel_openat_hook,
	&vendor_kernel_open_hook,
	&vendor_kernel_readlinkat_hook,
	&vendor_kernel_readlink_hook,
	&vendor_kernel_newfstatat_hook,
	&vendor_kernel_stat_hook,
	&vendor_kernel_lstat_hook,
	NULL,
};
