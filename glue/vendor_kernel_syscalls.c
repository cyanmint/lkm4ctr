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
#include "shadow_hook.h"

static long (*real_sys_unshare)(const struct pt_regs *regs);
static long (*real_sys_setns)(const struct pt_regs *regs);
static long (*real_sys_clone)(const struct pt_regs *regs);
static long (*real_sys_clone3)(const struct pt_regs *regs);
static long (*real_sys_fork)(const struct pt_regs *regs);
static long (*real_sys_vfork)(const struct pt_regs *regs);
static long (*real_sys_reboot)(const struct pt_regs *regs);

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
	 * argument" / "SetupMountNamespaces failed" abort. Mirrors the
	 * clone(CLONE_NEWNS, ...) path's identical fs_struct-isolation fix in
	 * vendor_kernel_clone_track() above (matching upstream's
	 * unshare_fs()).
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
	 * Install the private fs_struct copy allocated above, mirroring
	 * upstream ksys_unshare()'s post-switch fs_struct swap. The old,
	 * still-shared fs_struct's own `users` refcount (protected by an
	 * internal lock/seqlock whose exact field layout has changed across
	 * kernel versions, e.g. the 6.17 `fs_struct->{lock,seq}` ->
	 * `fs_struct->seq` seqlock fold-in) is intentionally left untouched
	 * here rather than decremented through a version-fragile, unexported
	 * field: this only leaves a harmless, bounded refcount overcount of 1
	 * on that fs_struct (it is never freed a moment later than it
	 * otherwise would have been), not a crash or correctness risk, since
	 * current->fs itself is still correctly swapped to the new, private,
	 * exclusively-owned copy that copy_mnt_ns() already repointed at the
	 * new mount tree.
	 */
	if (new_fs) {
		task_lock(current);
		current->fs = new_fs;
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

static void vendor_kernel_clone_track(long ret, unsigned long clone_flags,
				      unsigned long vns_flags)
{
	struct nsproxy *new_nsp = NULL;
	struct pid *child_pid;
	struct task_struct *child;

	if (ret <= 0)
		return;
	if (!vns_flags) {
		vendor_kernel_registry.stat_clone++;
		return;
	}

	/*
	 * clone(CLONE_NEWxxx, ...) requested new namespaces directly
	 * (rather than unshare()+fork()). The real clone() syscall was
	 * already invoked with the vns_* flags masked off, so the
	 * child was created sharing the parent's nsproxy. Build the
	 * new namespaces now and install them for real on the child
	 * task (same effect create_new_namespaces()+switch would have
	 * had, minus pid_ns_for_children applying to the child's own
	 * struct pid, which is unavoidable without hooking
	 * copy_process() itself since the child's pid was already
	 * allocated from the parent's pid namespace by the time this
	 * hook runs).
	 *
	 * The child task must be looked up *before* calling
	 * vns_unshare_nsproxy_namespaces() and its own fs_struct (child->fs)
	 * must be passed as new_fs. This hook runs in the parent's context
	 * (right after real_sys_clone()/real_sys_clone3() returns there), so
	 * a NULL new_fs would make vns_unshare_nsproxy_namespaces() default
	 * to current->fs -- the *parent's* fs_struct, not the child's own,
	 * separately-copied one. For CLONE_NEWNS specifically that would
	 * make the real, resolved copy_mnt_ns() retarget the parent's own
	 * root/pwd onto the freshly copied mount tree while leaving the
	 * child's fs_struct pointing at the old tree, completely
	 * disconnected from the new mnt_namespace about to be installed on
	 * it below. Every subsequent mount(2) the child performs on an
	 * absolute path then resolves through that stale root/pwd, whose
	 * vfsmount belongs to a different mnt_namespace than
	 * current->nsproxy->mnt_ns, and the kernel's check_mnt() rejects it
	 * with -EINVAL -- observed as e.g. systemd's generator sandbox
	 * (clone(CLONE_NEWNS, ...) + a plain "mount tmpfs /tmp") failing
	 * with "Failed to overmount /tmp/: Invalid argument".
	 */
	child_pid = find_get_pid((pid_t)ret);
	if (!child_pid) {
		vendor_kernel_registry.stat_clone++;
		return;
	}
	child = get_pid_task(child_pid, PIDTYPE_PID);
	put_pid(child_pid);
	if (!child) {
		vendor_kernel_registry.stat_clone++;
		return;
	}

	if (!vns_unshare_nsproxy_namespaces(vns_flags, &new_nsp, NULL, child->fs)) {
#if !defined(CONFIG_SYSVIPC)
		copy_semundo(clone_flags, child);
#endif
		/*
		 * new_nsp can legitimately still be NULL here: vns_flags ==
		 * CLONE_NEWUSER alone builds nothing in nsproxy (matching
		 * upstream unshare_nsproxy_namespaces()), so only switch when
		 * a new nsproxy was actually produced.
		 */
		if (new_nsp)
			vns_switch_task_namespaces(child, new_nsp);
		new_nsp = NULL;
	} else {
		/*
		 * vns_unshare_nsproxy_namespaces() failed: *new_nsp may have
		 * been left untouched (still NULL) or, if
		 * create_new_namespaces() itself failed after the ns_capable()
		 * check passed, set to an ERR_PTR() encoding the failure --
		 * never a real nsproxy. Passing that ERR_PTR to
		 * vns_put_nsproxy() below would treat a bogus, near-top
		 * address (e.g. -EPERM/-ENOMEM as a pointer) as a live
		 * nsproxy and corrupt/crash on the resulting bad refcount
		 * dereference, so make sure the pending release below never
		 * fires for a failed call.
		 */
		new_nsp = NULL;
	}
	if (new_nsp)
		vns_put_nsproxy(new_nsp);
	put_task_struct(child);
	vendor_kernel_registry.stat_clone++;
}

static long vendor_kernel_hook_clone(const struct pt_regs *regs)
{
	struct pt_regs regs_copy = *regs;
	unsigned long flags = vns_sys_arg0(regs);
	unsigned long vns_flags = flags & VNS_CLONE_FLAGS;
	long ret;

	if (vns_flags)
		vns_sys_set_arg0(&regs_copy, flags & ~VNS_CLONE_FLAGS);
	ret = real_sys_clone(vns_flags ? &regs_copy : regs);
	vendor_kernel_clone_track(ret, flags, vns_flags);
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

	if (!uargs || copy_from_user(&orig_flags, uargs, sizeof(orig_flags)))
		return real_sys_clone3(regs);

	vns_flags = (unsigned long)(orig_flags & VNS_CLONE_FLAGS);
	native_flags = orig_flags & ~((u64)VNS_CLONE_FLAGS);
	if (vns_flags) {
		if (copy_to_user(uargs, &native_flags, sizeof(native_flags)))
			return -EFAULT;
		patched = true;
	}

	ret = real_sys_clone3(regs);
	if (patched && copy_to_user(uargs, &orig_flags, sizeof(orig_flags)))
		return -EFAULT;
	vendor_kernel_clone_track(ret, (unsigned long)orig_flags, vns_flags);
	return ret;
}

static long vendor_kernel_hook_fork(const struct pt_regs *regs)
{
	long ret = real_sys_fork(regs);
	vendor_kernel_clone_track(ret, 0, 0);
	return ret;
}

static long vendor_kernel_hook_vfork(const struct pt_regs *regs)
{
	long ret = real_sys_vfork(regs);
	vendor_kernel_clone_track(ret, 0, 0);
	return ret;
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

struct shadow_hook *vendor_kernel_core_hooks[] = {
	&vendor_kernel_unshare_hook,
	&vendor_kernel_setns_hook,
	&vendor_kernel_clone_hook,
	&vendor_kernel_clone3_hook,
	&vendor_kernel_fork_hook,
	&vendor_kernel_vfork_hook,
	&vendor_kernel_reboot_hook,
	NULL,
};
