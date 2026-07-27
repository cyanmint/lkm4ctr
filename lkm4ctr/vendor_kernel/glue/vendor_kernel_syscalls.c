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
#include <uapi/linux/sched.h>
#include <asm/ptrace.h>

#include "../vendor_kernel.h"
#include "../../../common/shadow_hook.h"

static long (*real_sys_unshare)(const struct pt_regs *regs);
static long (*real_sys_setns)(const struct pt_regs *regs);
static long (*real_sys_clone)(const struct pt_regs *regs);
static long (*real_sys_clone3)(const struct pt_regs *regs);
static long (*real_sys_fork)(const struct pt_regs *regs);
static long (*real_sys_vfork)(const struct pt_regs *regs);

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

#if defined(CONFIG_ARM64)
static unsigned long vns_sys_arg0(const struct pt_regs *regs) { return regs->regs[0]; }
static unsigned long vns_sys_arg1(const struct pt_regs *regs) { return regs->regs[1]; }
static void vns_sys_set_arg0(struct pt_regs *regs, unsigned long value) { regs->regs[0] = value; }
#elif defined(CONFIG_X86_64)
static unsigned long vns_sys_arg0(const struct pt_regs *regs) { return regs->di; }
static unsigned long vns_sys_arg1(const struct pt_regs *regs) { return regs->si; }
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

	ret = vns_unshare_nsproxy_namespaces(vns_flags, &new_nsp, new_cred, NULL);
	if (ret) {
		if (new_cred)
			put_cred(new_cred);
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

	if (ret <= 0)
		return;
	if (vns_flags) {
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
		 */
		if (!vns_unshare_nsproxy_namespaces(vns_flags, &new_nsp, NULL, NULL)) {
			struct pid *child_pid = find_get_pid((pid_t)ret);

			if (child_pid) {
				struct task_struct *child = get_pid_task(child_pid, PIDTYPE_PID);

				if (child) {
#if !defined(CONFIG_SYSVIPC)
					copy_semundo(clone_flags, child);
#endif
					vns_switch_task_namespaces(child, new_nsp);
					new_nsp = NULL;
					put_task_struct(child);
				}
				put_pid(child_pid);
			}
			if (new_nsp)
				vns_put_nsproxy(new_nsp);
		}
	}
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

struct shadow_hook *vendor_kernel_core_hooks[] = {
	&vendor_kernel_unshare_hook,
	&vendor_kernel_setns_hook,
	&vendor_kernel_clone_hook,
	&vendor_kernel_clone3_hook,
	&vendor_kernel_fork_hook,
	&vendor_kernel_vfork_hook,
	NULL,
};
