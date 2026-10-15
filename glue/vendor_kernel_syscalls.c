// SPDX-License-Identifier: GPL-2.0
/*
 * vendor_kernel_syscalls.c - syscall hook handlers for vendor_kernel.
 * This is NEW code (not vendored from kernel-common).
 */
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/uaccess.h>
#include <linux/capability.h>
#include <linux/sched/task.h>
#include <linux/sched/signal.h>
#include <linux/pid.h>
#include <linux/pid_namespace.h>
#include <linux/reboot.h>
#include <uapi/linux/sched.h>
#include <asm/ptrace.h>

#include "../vendor/vendor_kernel.h"
#include "lkm4ctr_log.h"
#include "shadow_hook.h"

/*
 * [BUILD-COMPAT] fs_struct's own internal spinlock member (guarding
 * fs->users/root/pwd) moved across kernel versions: pre-6.17 kernels
 * expose it directly as `fs_struct->lock`; the 6.17 "fold fs_struct->
 * {lock,seq} into a seqlock" change replaced both with a single
 * `fs_struct->seq` seqlock_t whose own embedded spinlock is
 * `fs_struct->seq.lock`. Used by vendor_kernel_hook_unshare() below to
 * safely decrement an old, shared fs_struct's `users` count without
 * assuming one fixed member name.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 17, 0)
#define VNS_FS_STRUCT_LOCK(fs) (&(fs)->lock)
#else
#define VNS_FS_STRUCT_LOCK(fs) (&(fs)->seq.lock)
#endif

static long (*real_sys_unshare)(const struct pt_regs *regs);
static long (*real_sys_setns)(const struct pt_regs *regs);
static long (*real_sys_clone)(const struct pt_regs *regs);
static long (*real_sys_clone3)(const struct pt_regs *regs);
static long (*real_sys_fork)(const struct pt_regs *regs);
static long (*real_sys_vfork)(const struct pt_regs *regs);
static long (*real_sys_reboot)(const struct pt_regs *regs);
static void (*real_wake_up_new_task)(struct task_struct *task);
static struct cred *(*real_prepare_creds)(void);
static struct cred *(*real_prepare_kernel_cred)(struct task_struct *daemon);
static void (*real_put_cred_rcu)(struct rcu_head *rcu);
static void (*real___put_user_ns)(struct user_namespace *ns);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
static int (*real_copy_creds)(struct task_struct *task, u64 clone_flags);
#else
static int (*real_copy_creds)(struct task_struct *task,
			      unsigned long clone_flags);
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
static int (*real_copy_namespaces)(u64 flags, struct task_struct *task);
#else
static int (*real_copy_namespaces)(unsigned long flags,
				   struct task_struct *task);
#endif

static const char * const vendor_kernel_unshare_names[] = {
	"__arm64_sys_unshare", "__x64_sys_unshare", "sys_unshare", NULL,
};
static const char * const vendor_kernel_setns_names[] = {
	"__arm64_sys_setns", "__x64_sys_setns", "sys_setns", NULL,
};
static const char * const vendor_kernel_clone_names[] = {
	"__arm64_sys_clone", "__x64_sys_clone", "sys_clone", NULL,
};
static const char * const vendor_kernel_clone3_names[] = {
	"__arm64_sys_clone3", "__x64_sys_clone3", "sys_clone3", NULL,
};
static const char * const vendor_kernel_fork_names[] = {
	"__arm64_sys_fork", "__x64_sys_fork", "sys_fork", NULL,
};
static const char * const vendor_kernel_vfork_names[] = {
	"__arm64_sys_vfork", "__x64_sys_vfork", "sys_vfork", NULL,
};
static const char * const vendor_kernel_reboot_names[] = {
	"__arm64_sys_reboot", "__x64_sys_reboot", "sys_reboot", NULL,
};
static const char * const vendor_kernel_wake_up_new_task_names[] = {
	"wake_up_new_task", NULL,
};
static const char * const vendor_kernel_prepare_creds_names[] = {
	"prepare_creds", NULL,
};
static const char * const vendor_kernel_prepare_kernel_cred_names[] = {
	"prepare_kernel_cred", NULL,
};
static const char * const vendor_kernel_put_cred_rcu_names[] = {
	"put_cred_rcu", NULL,
};
static const char * const vendor_kernel_put_user_ns_names[] = {
	"__put_user_ns", NULL,
};
static const char * const vendor_kernel_copy_namespaces_names[] = {
	"copy_namespaces", NULL,
};
static const char * const vendor_kernel_copy_creds_names[] = {
	"copy_creds", NULL,
};

#if defined(CONFIG_ARM64)
static unsigned long vns_sys_arg0(const struct pt_regs *regs) { return regs->regs[0]; }
static unsigned long vns_sys_arg1(const struct pt_regs *regs) { return regs->regs[1]; }
static unsigned long vns_sys_arg2(const struct pt_regs *regs) { return regs->regs[2]; }
static void vns_sys_set_arg0(struct pt_regs *regs, unsigned long value) { regs->regs[0] = value; }
#elif defined(CONFIG_X86_64)
static unsigned long vns_sys_arg0(const struct pt_regs *regs) { return regs->di; }
static unsigned long vns_sys_arg1(const struct pt_regs *regs) { return regs->si; }
static unsigned long vns_sys_arg2(const struct pt_regs *regs) { return regs->dx; }
static void vns_sys_set_arg0(struct pt_regs *regs, unsigned long value) { regs->di = value; }
#else
#error "vendor_kernel: unsupported architecture"
#endif

static long vendor_kernel_hook_unshare(const struct pt_regs *regs)
{
	struct pt_regs regs_copy = *regs;
	unsigned long flags = vns_sys_arg0(regs);
	unsigned long vns_flags = flags & VNS_CLONE_FLAGS;
	unsigned long native_flags = flags & ~VNS_CLONE_FLAGS;
	struct cred *new_cred = NULL;
	struct nsproxy *new_nsp = NULL;
	struct fs_struct *new_fs = NULL;
	long ret = 0;

	if (!vns_flags)
		return real_sys_unshare(regs);

	if (native_flags) {
		vns_sys_set_arg0(&regs_copy, native_flags);
		ret = real_sys_unshare(&regs_copy);
		if (ret)
			return ret;
	}

	if (vns_flags & CLONE_NEWUSER) {
		ret = vns_unshare_userns(vns_flags, &new_cred);
		if (ret)
			return ret;
	}

	/*
	 * Real unshare(CLONE_NEWNS) always implies CLONE_FS (ksys_unshare()
	 * unconditionally ORs it in before calling unshare_fs()): the real,
	 * resolved copy_mnt_ns() (vns_copy_mnt_ns_fn) repoints its fs_struct
	 * argument's root/pwd in place onto the freshly copied mount tree, so
	 * that argument must be a private fs_struct, not one still shared
	 * with any other thread/task, or every other user of the shared
	 * fs_struct (anything that did a plain clone() with CLONE_FS, e.g. a
	 * pthread) has its root/pwd silently repointed into this task's
	 * brand-new, otherwise-invisible mount namespace too. Passing
	 * current->fs unconditionally here (as this hook used to) skipped
	 * that split, so current->fs->users could still be >1 by the time a
	 * later setns(fd, CLONE_NEWNS) ran -- the real kernel's own
	 * mntns_install() (fs/namespace.c) hard-requires `fs->users == 1` and
	 * fails the setns() with -EINVAL otherwise, observed as Android
	 * init's "Cannot switch back to bootstrap mount namespace: Invalid
	 * argument" / "SetupMountNamespaces failed" abort. The clone path now
	 * runs the vendored copy_namespaces hook before child wakeup, after the
	 * child's private fs_struct has been constructed by the native fork core.
	 */
	if (vns_flags & CLONE_NEWNS) {
		struct fs_struct *fs = current->fs;

		if (fs && fs->users > 1) {
			new_fs = copy_fs_struct(fs);
			if (!new_fs) {
				if (new_cred)
					put_cred(new_cred);
				return -ENOMEM;
			}
		}
	}

	ret = vns_unshare_nsproxy_namespaces(vns_flags, &new_nsp, new_cred, new_fs);
	if (ret) {
		if (new_cred)
			put_cred(new_cred);
		if (new_fs)
			free_fs_struct(new_fs);
		return ret;
	}

	if (new_cred)
		commit_creds(new_cred);
	/*
	 * Actually install the newly created namespaces on the calling task,
	 * exactly like the real unshare(2) does via switch_task_namespaces().
	 * Ownership of the new_nsp reference is transferred here; without this
	 * step the rest of the kernel (hostname, /proc, ipc, netns lookups,
	 * future fork()s, ...) keeps using the task's original nsproxy and
	 * unshare() degenerates into bookkeeping only.
	 */
	if (new_nsp)
		vns_switch_task_namespaces(current, new_nsp);

	/*
	 * Install the private fs_struct copy allocated above and drop this
	 * task's reference on the old, still-shared one, mirroring upstream
	 * ksys_unshare()'s post-switch fs_struct swap. `fs_struct->users` is
	 * only ever modified under its own internal spinlock, whose exact
	 * field layout has changed across kernel versions -- pre-6.17
	 * kernels expose it directly as `fs_struct->lock`; 6.17 folded
	 * `fs_struct->{lock,seq}` into a single `fs_struct->seq` seqlock_t,
	 * whose own embedded spinlock is `fs_struct->seq.lock` -- hence the
	 * version gate below (see VNS_FS_STRUCT_LOCK()) instead of assuming
	 * one fixed member name.
	 */
	if (new_fs) {
		struct fs_struct *old_fs;

		task_lock(current);
		old_fs = current->fs;
		spin_lock(VNS_FS_STRUCT_LOCK(old_fs));
		current->fs = new_fs;
		old_fs->users--;
		spin_unlock(VNS_FS_STRUCT_LOCK(old_fs));
		task_unlock(current);
	}

	vendor_kernel_registry.stat_unshare++;
	return 0;
}

static long vendor_kernel_hook_setns(const struct pt_regs *regs)
{
	int fd = (int)vns_sys_arg0(regs);
	int flags = (int)vns_sys_arg1(regs);
	long ret = vns_sys_setns(fd, flags);

	if (!ret)
		vendor_kernel_registry.stat_setns++;
	return ret;
}

static void vendor_kernel_clone_track(long ret)
{
	if (ret > 0)
		vendor_kernel_registry.stat_clone++;
}

static long vendor_kernel_hook_clone(const struct pt_regs *regs)
{
	struct pt_regs regs_copy = *regs;
	unsigned long flags = vns_sys_arg0(regs);
	unsigned long vns_flags = flags & VNS_CLONE_FLAGS;
	long ret;

	if (!vns_namespace_creation_allowed())
		return -EBUSY;
	if ((flags & (CLONE_NEWIPC | CLONE_SYSVSEM)) ==
	    (CLONE_NEWIPC | CLONE_SYSVSEM))
		return -EINVAL;
	if ((flags & (CLONE_NEWUSER | CLONE_THREAD)) ==
	    (CLONE_NEWUSER | CLONE_THREAD))
		return -EINVAL;
	if ((flags & (CLONE_NEWUSER | CLONE_FS)) ==
	    (CLONE_NEWUSER | CLONE_FS))
		return -EINVAL;
	if ((flags & (CLONE_NEWNS | CLONE_FS)) ==
	    (CLONE_NEWNS | CLONE_FS))
		return -EINVAL;
	if ((flags & (CLONE_NEWPID | CLONE_THREAD)) ==
	    (CLONE_NEWPID | CLONE_THREAD))
		return -EINVAL;
	ret = vns_semundo_clone_begin(flags);
	if (ret)
		return ret;
	if (vns_flags)
		vns_sys_set_arg0(&regs_copy, flags & ~VNS_CLONE_FLAGS);
	ret = real_sys_clone(vns_flags ? &regs_copy : regs);
	vns_semundo_clone_abort();
	vendor_kernel_clone_track(ret);
	return ret;
}

static long vendor_kernel_hook_clone3(const struct pt_regs *regs)
{
	void __user *uargs = (void __user *)(uintptr_t)vns_sys_arg0(regs);
	u64 orig_flags;
	u64 native_flags;
	unsigned long vns_flags;
	long ret;
	bool patched = false;

	if (!vns_namespace_creation_allowed())
		return -EBUSY;
	if (!uargs || copy_from_user(&orig_flags, uargs, sizeof(orig_flags)))
		return real_sys_clone3(regs);

	vns_flags = (unsigned long)(orig_flags & VNS_CLONE_FLAGS);
	if ((orig_flags & (CLONE_NEWIPC | CLONE_SYSVSEM)) ==
	    (CLONE_NEWIPC | CLONE_SYSVSEM))
		return -EINVAL;
	if ((orig_flags & (CLONE_NEWUSER | CLONE_THREAD)) ==
	    (CLONE_NEWUSER | CLONE_THREAD))
		return -EINVAL;
	if ((orig_flags & (CLONE_NEWUSER | CLONE_FS)) ==
	    (CLONE_NEWUSER | CLONE_FS))
		return -EINVAL;
	if ((orig_flags & (CLONE_NEWNS | CLONE_FS)) ==
	    (CLONE_NEWNS | CLONE_FS))
		return -EINVAL;
	if ((orig_flags & (CLONE_NEWPID | CLONE_THREAD)) ==
	    (CLONE_NEWPID | CLONE_THREAD))
		return -EINVAL;
	native_flags = orig_flags & ~((u64)VNS_CLONE_FLAGS);
	if (vns_flags) {
		if (copy_to_user(uargs, &native_flags, sizeof(native_flags)))
			return -EFAULT;
		patched = true;
	}
	ret = vns_semundo_clone_begin((unsigned long)orig_flags);
	if (ret) {
		if (patched && copy_to_user(uargs, &orig_flags, sizeof(orig_flags)))
			return -EFAULT;
		return ret;
	}

	ret = real_sys_clone3(regs);
	vns_semundo_clone_abort();
	if (patched && copy_to_user(uargs, &orig_flags, sizeof(orig_flags))) {
		if (ret < 0)
			return -EFAULT;
		LKM4CTR_WARN("vendor_kernel",
			     "clone3 succeeded but could not restore the caller's flags field");
	}
	vendor_kernel_clone_track(ret);
	return ret;
}

static long vendor_kernel_hook_fork(const struct pt_regs *regs)
{
	long ret;

	if (!vns_namespace_creation_allowed())
		return -EBUSY;
	ret = real_sys_fork(regs);
	vendor_kernel_clone_track(ret);
	return ret;
}

static long vendor_kernel_hook_vfork(const struct pt_regs *regs)
{
	long ret;

	if (!vns_namespace_creation_allowed())
		return -EBUSY;
	ret = real_sys_vfork(regs);
	vendor_kernel_clone_track(ret);
	return ret;
}

static int vendor_kernel_hook_copy_namespaces(
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
	u64 flags,
#else
	unsigned long flags,
#endif
	struct task_struct *task)
{
	unsigned long clone_flags = vns_semundo_clone_flags();
	unsigned long vns_flags = clone_flags & VNS_CLONE_FLAGS;
	struct nsproxy *new_nsproxy;
	int err;

	err = vns_user_ns_cred_get(task->cred);
	if (err)
		return err;
	err = real_copy_namespaces(flags, task);
	if (err || !vns_flags)
		return err;

	if (vns_flags & CLONE_NEWUSER) {
		err = vns_user_ns_cred_get(task->cred);
		if (err)
			return err;
		err = vns_create_user_ns((struct cred *)task->cred);
		if (err)
			return err;
	}

	new_nsproxy = vns_copy_namespaces(vns_flags, task);
	if (IS_ERR(new_nsproxy))
		return PTR_ERR(new_nsproxy);
	vns_switch_task_namespaces(task, new_nsproxy);
	return 0;
}

static int vendor_kernel_hook_copy_creds(
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
	struct task_struct *task, u64 clone_flags
#else
	struct task_struct *task, unsigned long clone_flags
#endif
)
{
	int err;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
	err = real_copy_creds(task, clone_flags);
#else
	err = real_copy_creds(task, clone_flags);
#endif
	if (err)
		return err;
	return vns_user_ns_cred_get(task->cred);
}

/*
 * reboot(2): on the stock CONFIG_PID_NS=n kernels this module targets,
 * reboot_pid_ns() (include/linux/pid_namespace.h) is a static-inline stub
 * that unconditionally returns 0, so the real sys_reboot() falls straight
 * through to kernel_restart()/kernel_power_off()/... regardless of which
 * pid namespace the caller is actually in. That means reboot(2) from inside
 * a vendor_kernel container (e.g. `runc`/`dockerd` handling a restart
 * policy, or a plain `reboot` inside `docker exec`) reboots the whole host.
 *
 * Mirror the real kernel/reboot.c SYSCALL_DEFINE4(reboot, ...) checks here
 * (capability + magic numbers) and route non-init pid namespaces to
 * vns_reboot_pid_ns() -- the real reboot_pid_ns() logic, vendored verbatim
 * -- instead of ever reaching the real syscall.
 */
static long vendor_kernel_hook_reboot(const struct pt_regs *regs)
{
	struct pid_namespace *pid_ns = task_active_pid_ns(current);
	int magic1, magic2;
	unsigned int cmd;

	if (pid_ns == &init_pid_ns)
		return real_sys_reboot(regs);

	if (!ns_capable(pid_ns->user_ns, CAP_SYS_BOOT))
		return -EPERM;

	/*
	 * Safe truncation: reboot(2)'s real prototype is
	 * SYSCALL_DEFINE4(reboot, int magic1, int magic2, unsigned int cmd,
	 * void __user *arg) -- the calling convention already delivers these
	 * arguments as 32-bit values, matching what real_sys_reboot()/the
	 * upstream syscall wrapper itself would extract from the same regs.
	 */
	magic1 = (int)vns_sys_arg0(regs);
	magic2 = (int)vns_sys_arg1(regs);
	cmd = (unsigned int)vns_sys_arg2(regs);

	if (magic1 != LINUX_REBOOT_MAGIC1 ||
	    (magic2 != LINUX_REBOOT_MAGIC2 &&
	     magic2 != LINUX_REBOOT_MAGIC2A &&
	     magic2 != LINUX_REBOOT_MAGIC2B &&
	     magic2 != LINUX_REBOOT_MAGIC2C))
		return -EINVAL;

	vendor_kernel_registry.stat_reboot++;
	/*
	 * For RESTART/RESTART2/POWER_OFF/HALT, vns_reboot_pid_ns() calls
	 * do_exit() and never returns; only CAD_ON/CAD_OFF/KEXEC/SW_SUSPEND
	 * (unsupported inside a pid namespace, matching upstream) fall
	 * through to its -EINVAL return here.
	 */
	return vns_reboot_pid_ns(pid_ns, (int)cmd);
}

static void vendor_kernel_hook_wake_up_new_task(struct task_struct *task)
{
	if (vns_user_ns_cred_get(task->cred))
		LKM4CTR_ERR("vendor_kernel",
			    "could not account vendored user namespace on child credentials %p; retaining a fail-safe reference",
			    task->cred);
	vns_semundo_clone_wake(task);
	real_wake_up_new_task(task);
}

static struct cred *vendor_kernel_hook_prepare_creds(void)
{
	struct cred *cred = real_prepare_creds();

	if (cred && vns_user_ns_cred_get(cred))
		LKM4CTR_ERR("vendor_kernel",
			    "could not account vendored user namespace on prepared credentials %p; retaining a fail-safe reference",
			    cred);
	return cred;
}

static struct cred *vendor_kernel_hook_prepare_kernel_cred(
	struct task_struct *daemon)
{
	struct cred *cred = real_prepare_kernel_cred(daemon);

	if (cred && vns_user_ns_cred_get(cred))
		LKM4CTR_ERR("vendor_kernel",
			    "could not account vendored user namespace on kernel credentials %p; retaining a fail-safe reference",
			    cred);
	return cred;
}

static void vendor_kernel_hook_put_cred_rcu(struct rcu_head *rcu)
{
	struct cred *cred = container_of(rcu, struct cred, rcu);
	struct user_namespace *user_ns = vns_user_ns_cred_detach(cred);

	real_put_cred_rcu(rcu);
	if (user_ns)
		vns_put_user_ns(user_ns);
}

static void vendor_kernel_hook_put_user_ns(struct user_namespace *ns)
{
	if (vns_user_ns_is_vendored(ns))
		vns___put_user_ns(ns);
	else
		real___put_user_ns(ns);
}

void vns_native___put_user_ns(struct user_namespace *ns)
{
	if (real___put_user_ns)
		real___put_user_ns(ns);
}

static struct shadow_hook vendor_kernel_unshare_hook =
	SHADOW_HOOK(vendor_kernel_unshare_names, vendor_kernel_hook_unshare, &real_sys_unshare);
static struct shadow_hook vendor_kernel_setns_hook =
	SHADOW_HOOK(vendor_kernel_setns_names, vendor_kernel_hook_setns, &real_sys_setns);
static struct shadow_hook vendor_kernel_clone_hook =
	SHADOW_HOOK(vendor_kernel_clone_names, vendor_kernel_hook_clone, &real_sys_clone);
static struct shadow_hook vendor_kernel_clone3_hook =
	SHADOW_HOOK(vendor_kernel_clone3_names, vendor_kernel_hook_clone3, &real_sys_clone3);
static struct shadow_hook vendor_kernel_fork_hook =
	SHADOW_HOOK(vendor_kernel_fork_names, vendor_kernel_hook_fork, &real_sys_fork);
static struct shadow_hook vendor_kernel_vfork_hook =
	SHADOW_HOOK(vendor_kernel_vfork_names, vendor_kernel_hook_vfork, &real_sys_vfork);
static struct shadow_hook vendor_kernel_reboot_hook =
	SHADOW_HOOK(vendor_kernel_reboot_names, vendor_kernel_hook_reboot, &real_sys_reboot);
static struct shadow_hook vendor_kernel_wake_up_new_task_hook = {
	.names = vendor_kernel_wake_up_new_task_names,
	.function = vendor_kernel_hook_wake_up_new_task,
	.original = &real_wake_up_new_task,
	.owner = THIS_MODULE,
	.quiesce_exempt = true,
};
static struct shadow_hook vendor_kernel_prepare_creds_hook = {
	.names = vendor_kernel_prepare_creds_names,
	.function = vendor_kernel_hook_prepare_creds,
	.original = &real_prepare_creds,
	.owner = THIS_MODULE,
	.quiesce_exempt = true,
};
static struct shadow_hook vendor_kernel_prepare_kernel_cred_hook = {
	.names = vendor_kernel_prepare_kernel_cred_names,
	.function = vendor_kernel_hook_prepare_kernel_cred,
	.original = &real_prepare_kernel_cred,
	.owner = THIS_MODULE,
	.quiesce_exempt = true,
};
static struct shadow_hook vendor_kernel_put_cred_rcu_hook = {
	.names = vendor_kernel_put_cred_rcu_names,
	.function = vendor_kernel_hook_put_cred_rcu,
	.original = &real_put_cred_rcu,
	.owner = THIS_MODULE,
	.quiesce_exempt = true,
};
static struct shadow_hook vendor_kernel_put_user_ns_hook = {
	.names = vendor_kernel_put_user_ns_names,
	.function = vendor_kernel_hook_put_user_ns,
	.original = &real___put_user_ns,
	.owner = THIS_MODULE,
	.quiesce_exempt = true,
};
static struct shadow_hook vendor_kernel_copy_namespaces_hook = {
	.names = vendor_kernel_copy_namespaces_names,
	.function = vendor_kernel_hook_copy_namespaces,
	.original = &real_copy_namespaces,
	.owner = THIS_MODULE,
	.quiesce_exempt = true,
};
static struct shadow_hook vendor_kernel_copy_creds_hook =
{
	.names = vendor_kernel_copy_creds_names,
	.function = vendor_kernel_hook_copy_creds,
	.original = &real_copy_creds,
	.owner = THIS_MODULE,
	.quiesce_exempt = true,
};

struct shadow_hook *vendor_kernel_core_hooks[] = {
	&vendor_kernel_unshare_hook,
	&vendor_kernel_setns_hook,
	&vendor_kernel_clone_hook,
	&vendor_kernel_clone3_hook,
	&vendor_kernel_fork_hook,
	&vendor_kernel_vfork_hook,
	&vendor_kernel_reboot_hook,
	&vendor_kernel_wake_up_new_task_hook,
	&vendor_kernel_prepare_creds_hook,
	&vendor_kernel_prepare_kernel_cred_hook,
	&vendor_kernel_put_cred_rcu_hook,
	&vendor_kernel_put_user_ns_hook,
	&vendor_kernel_copy_namespaces_hook,
	&vendor_kernel_copy_creds_hook,
	NULL,
};

bool vendor_kernel_required_lifecycle_hooks_ready(void)
{
	bool native_userns = vns_native_userns_core_available();
	bool clone_hooks = vendor_kernel_clone_hook.installed &&
	       vendor_kernel_clone_hook.retprobe_installed &&
	       vendor_kernel_clone3_hook.installed &&
	       vendor_kernel_clone3_hook.retprobe_installed &&
	       vendor_kernel_fork_hook.installed &&
	       vendor_kernel_fork_hook.retprobe_installed &&
	       vendor_kernel_vfork_hook.installed &&
	       vendor_kernel_vfork_hook.retprobe_installed &&
	       vendor_kernel_wake_up_new_task_hook.installed &&
	       vendor_kernel_wake_up_new_task_hook.retprobe_installed &&
	       vendor_kernel_copy_namespaces_hook.installed &&
	       vendor_kernel_copy_namespaces_hook.retprobe_installed;

	if (!clone_hooks)
		return false;
	if (native_userns)
		return vendor_kernel_put_user_ns_hook.installed &&
		       vendor_kernel_put_user_ns_hook.retprobe_installed;
	return vendor_kernel_prepare_creds_hook.installed &&
	       vendor_kernel_prepare_creds_hook.retprobe_installed &&
	       vendor_kernel_prepare_kernel_cred_hook.installed &&
	       vendor_kernel_prepare_kernel_cred_hook.retprobe_installed &&
	       vendor_kernel_copy_creds_hook.installed &&
	       vendor_kernel_copy_creds_hook.retprobe_installed &&
	       vendor_kernel_put_cred_rcu_hook.installed &&
	       vendor_kernel_put_cred_rcu_hook.retprobe_installed;
}
