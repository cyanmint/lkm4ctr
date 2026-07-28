// SPDX-License-Identifier: GPL-2.0
/*
 * vendor_kernel_syscalls_userns.c - uid/gid getter/setter syscall hooks for
 * vendor_kernel. This is NEW code (not vendored from kernel-common).
 *
 * Background: on a kernel genuinely missing CONFIG_USER_NS, <linux/
 * uidgid.h>'s make_kuid()/from_kuid()/from_kuid_munged() (and the kgid
 * equivalents) collapse to trivial static-inline identity functions that
 * ignore the struct user_namespace * argument entirely and just hand back
 * the raw id unchanged. So even though glue/vendor_kernel_procfs_userns.c
 * (and kernel/user_namespace.c's vns_create_user_ns()/vns_unshare_userns())
 * install a real, fully-populated per-task struct user_namespace after
 * unshare(CLONE_NEWUSER) + a uid_map/gid_map write, the real, unhooked
 * getuid()/setuid()/etc. syscalls in vmlinux never actually consult it --
 * every mapped id is silently unmapped, making the whole namespace
 * unobservable through ordinary credential syscalls.
 *
 * These hooks close that gap directly at the syscall boundary instead of
 * trying to patch every make_kuid()/from_kuid_munged() call site compiled
 * into vmlinux itself:
 *   - getters (getuid/geteuid/getgid/getegid/getresuid/getresgid): call the
 *     real syscall first (whose returned/written value is always the raw,
 *     global id, since the real from_kuid_munged()/from_kgid_munged() are
 *     no-ops here), then translate that raw id down into the calling
 *     task's own user_namespace via vns_from_kuid_munged()/
 *     vns_from_kgid_munged() -- exactly the translation the real syscall
 *     would have performed had CONFIG_USER_NS been enabled.
 *   - setters (setuid/setgid/setreuid/setregid/setresuid/setresgid):
 *     translate each supplied (non -1) id up into a raw global id via
 *     vns_make_kuid()/vns_make_kgid() *before* calling the real syscall
 *     unchanged, so every one of the real syscall's own privilege/
 *     capability checks (security_task_fix_setuid(), CAP_SETUID/CAP_SETGID
 *     against current_user_ns(), etc. -- none of which are gated by
 *     CONFIG_USER_NS) still runs, just against the already-translated
 *     value; the real (no-op) make_kuid()/make_kgid() it then applies
 *     internally is a further identity pass-through that changes nothing.
 *
 * Both directions are skipped whenever the caller is still running in the
 * real, un-unshare()'d init_user_ns (vns_real_init_user_ns, populated once
 * at vendor_kernel_init() time -- see glue/vendor_kernel_module.c and
 * glue/vendor_kernel_data_syms.h), so ordinary (non-namespaced) processes
 * see no behavior change at all.
 */
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/uaccess.h>
#include <linux/cred.h>
#include <linux/uidgid.h>
#include <asm/ptrace.h>

#include "../vendor/vendor_kernel.h"
#include "shadow_hook.h"

static long (*real_sys_getuid)(const struct pt_regs *regs);
static long (*real_sys_geteuid)(const struct pt_regs *regs);
static long (*real_sys_getgid)(const struct pt_regs *regs);
static long (*real_sys_getegid)(const struct pt_regs *regs);
static long (*real_sys_getresuid)(const struct pt_regs *regs);
static long (*real_sys_getresgid)(const struct pt_regs *regs);
static long (*real_sys_setuid)(const struct pt_regs *regs);
static long (*real_sys_setgid)(const struct pt_regs *regs);
static long (*real_sys_setreuid)(const struct pt_regs *regs);
static long (*real_sys_setregid)(const struct pt_regs *regs);
static long (*real_sys_setresuid)(const struct pt_regs *regs);
static long (*real_sys_setresgid)(const struct pt_regs *regs);

#define VNS_USERNS_SYSCALL_NAMES(sym) \
	{ "__arm64_sys_" sym, "__x64_sys_" sym, "sys_" sym, NULL, }

static const char * const vendor_kernel_getuid_names[] = VNS_USERNS_SYSCALL_NAMES("getuid");
static const char * const vendor_kernel_geteuid_names[] = VNS_USERNS_SYSCALL_NAMES("geteuid");
static const char * const vendor_kernel_getgid_names[] = VNS_USERNS_SYSCALL_NAMES("getgid");
static const char * const vendor_kernel_getegid_names[] = VNS_USERNS_SYSCALL_NAMES("getegid");
static const char * const vendor_kernel_getresuid_names[] = VNS_USERNS_SYSCALL_NAMES("getresuid");
static const char * const vendor_kernel_getresgid_names[] = VNS_USERNS_SYSCALL_NAMES("getresgid");
static const char * const vendor_kernel_setuid_names[] = VNS_USERNS_SYSCALL_NAMES("setuid");
static const char * const vendor_kernel_setgid_names[] = VNS_USERNS_SYSCALL_NAMES("setgid");
static const char * const vendor_kernel_setreuid_names[] = VNS_USERNS_SYSCALL_NAMES("setreuid");
static const char * const vendor_kernel_setregid_names[] = VNS_USERNS_SYSCALL_NAMES("setregid");
static const char * const vendor_kernel_setresuid_names[] = VNS_USERNS_SYSCALL_NAMES("setresuid");
static const char * const vendor_kernel_setresgid_names[] = VNS_USERNS_SYSCALL_NAMES("setresgid");

#if defined(CONFIG_ARM64)
static unsigned long vns_userns_arg0(const struct pt_regs *regs) { return regs->regs[0]; }
static unsigned long vns_userns_arg1(const struct pt_regs *regs) { return regs->regs[1]; }
static unsigned long vns_userns_arg2(const struct pt_regs *regs) { return regs->regs[2]; }
static void vns_userns_set_arg0(struct pt_regs *regs, unsigned long v) { regs->regs[0] = v; }
static void vns_userns_set_arg1(struct pt_regs *regs, unsigned long v) { regs->regs[1] = v; }
static void vns_userns_set_arg2(struct pt_regs *regs, unsigned long v) { regs->regs[2] = v; }
#elif defined(CONFIG_X86_64)
static unsigned long vns_userns_arg0(const struct pt_regs *regs) { return regs->di; }
static unsigned long vns_userns_arg1(const struct pt_regs *regs) { return regs->si; }
static unsigned long vns_userns_arg2(const struct pt_regs *regs) { return regs->dx; }
static void vns_userns_set_arg0(struct pt_regs *regs, unsigned long v) { regs->di = v; }
static void vns_userns_set_arg1(struct pt_regs *regs, unsigned long v) { regs->si = v; }
static void vns_userns_set_arg2(struct pt_regs *regs, unsigned long v) { regs->dx = v; }
#else
#error "vendor_kernel: unsupported architecture"
#endif

/* ---- getters ---- */

static long vendor_kernel_hook_getuid(const struct pt_regs *regs)
{
	long ret = real_sys_getuid(regs);
	struct user_namespace *ns = current_user_ns();

	if (ns == vns_real_init_user_ns)
		return ret;
	return vns_from_kuid_munged(ns, KUIDT_INIT((uid_t)ret));
}

static long vendor_kernel_hook_geteuid(const struct pt_regs *regs)
{
	long ret = real_sys_geteuid(regs);
	struct user_namespace *ns = current_user_ns();

	if (ns == vns_real_init_user_ns)
		return ret;
	return vns_from_kuid_munged(ns, KUIDT_INIT((uid_t)ret));
}

static long vendor_kernel_hook_getgid(const struct pt_regs *regs)
{
	long ret = real_sys_getgid(regs);
	struct user_namespace *ns = current_user_ns();

	if (ns == vns_real_init_user_ns)
		return ret;
	return vns_from_kgid_munged(ns, KGIDT_INIT((gid_t)ret));
}

static long vendor_kernel_hook_getegid(const struct pt_regs *regs)
{
	long ret = real_sys_getegid(regs);
	struct user_namespace *ns = current_user_ns();

	if (ns == vns_real_init_user_ns)
		return ret;
	return vns_from_kgid_munged(ns, KGIDT_INIT((gid_t)ret));
}

static long vendor_kernel_hook_getresuid(const struct pt_regs *regs)
{
	long ret = real_sys_getresuid(regs);
	struct user_namespace *ns;
	uid_t __user *ptrs[3];
	int i;

	if (ret)
		return ret;
	ns = current_user_ns();
	if (ns == vns_real_init_user_ns)
		return ret;

	ptrs[0] = (uid_t __user *)vns_userns_arg0(regs);
	ptrs[1] = (uid_t __user *)vns_userns_arg1(regs);
	ptrs[2] = (uid_t __user *)vns_userns_arg2(regs);

	for (i = 0; i < 3; i++) {
		uid_t raw, mapped;

		if (get_user(raw, ptrs[i]))
			return -EFAULT;
		mapped = vns_from_kuid_munged(ns, KUIDT_INIT(raw));
		if (put_user(mapped, ptrs[i]))
			return -EFAULT;
	}
	return ret;
}

static long vendor_kernel_hook_getresgid(const struct pt_regs *regs)
{
	long ret = real_sys_getresgid(regs);
	struct user_namespace *ns;
	gid_t __user *ptrs[3];
	int i;

	if (ret)
		return ret;
	ns = current_user_ns();
	if (ns == vns_real_init_user_ns)
		return ret;

	ptrs[0] = (gid_t __user *)vns_userns_arg0(regs);
	ptrs[1] = (gid_t __user *)vns_userns_arg1(regs);
	ptrs[2] = (gid_t __user *)vns_userns_arg2(regs);

	for (i = 0; i < 3; i++) {
		gid_t raw, mapped;

		if (get_user(raw, ptrs[i]))
			return -EFAULT;
		mapped = vns_from_kgid_munged(ns, KGIDT_INIT(raw));
		if (put_user(mapped, ptrs[i]))
			return -EFAULT;
	}
	return ret;
}

/* ---- setters ---- */

static long vendor_kernel_hook_setuid(const struct pt_regs *regs)
{
	struct pt_regs regs_copy = *regs;
	struct user_namespace *ns = current_user_ns();
	kuid_t kuid;

	if (ns == vns_real_init_user_ns)
		return real_sys_setuid(regs);

	kuid = vns_make_kuid(ns, (uid_t)vns_userns_arg0(regs));
	if (!uid_valid(kuid))
		return -EINVAL;
	vns_userns_set_arg0(&regs_copy, (unsigned long)(uid_t)__kuid_val(kuid));
	return real_sys_setuid(&regs_copy);
}

static long vendor_kernel_hook_setgid(const struct pt_regs *regs)
{
	struct pt_regs regs_copy = *regs;
	struct user_namespace *ns = current_user_ns();
	kgid_t kgid;

	if (ns == vns_real_init_user_ns)
		return real_sys_setgid(regs);

	kgid = vns_make_kgid(ns, (gid_t)vns_userns_arg0(regs));
	if (!gid_valid(kgid))
		return -EINVAL;
	vns_userns_set_arg0(&regs_copy, (unsigned long)(gid_t)__kgid_val(kgid));
	return real_sys_setgid(&regs_copy);
}

static long vendor_kernel_hook_setreuid(const struct pt_regs *regs)
{
	struct pt_regs regs_copy = *regs;
	struct user_namespace *ns = current_user_ns();
	uid_t ruid = (uid_t)vns_userns_arg0(regs);
	uid_t euid = (uid_t)vns_userns_arg1(regs);
	kuid_t k;

	if (ns == vns_real_init_user_ns)
		return real_sys_setreuid(regs);

	if (ruid != (uid_t)-1) {
		k = vns_make_kuid(ns, ruid);
		if (!uid_valid(k))
			return -EINVAL;
		vns_userns_set_arg0(&regs_copy, (unsigned long)(uid_t)__kuid_val(k));
	}
	if (euid != (uid_t)-1) {
		k = vns_make_kuid(ns, euid);
		if (!uid_valid(k))
			return -EINVAL;
		vns_userns_set_arg1(&regs_copy, (unsigned long)(uid_t)__kuid_val(k));
	}
	return real_sys_setreuid(&regs_copy);
}

static long vendor_kernel_hook_setregid(const struct pt_regs *regs)
{
	struct pt_regs regs_copy = *regs;
	struct user_namespace *ns = current_user_ns();
	gid_t rgid = (gid_t)vns_userns_arg0(regs);
	gid_t egid = (gid_t)vns_userns_arg1(regs);
	kgid_t k;

	if (ns == vns_real_init_user_ns)
		return real_sys_setregid(regs);

	if (rgid != (gid_t)-1) {
		k = vns_make_kgid(ns, rgid);
		if (!gid_valid(k))
			return -EINVAL;
		vns_userns_set_arg0(&regs_copy, (unsigned long)(gid_t)__kgid_val(k));
	}
	if (egid != (gid_t)-1) {
		k = vns_make_kgid(ns, egid);
		if (!gid_valid(k))
			return -EINVAL;
		vns_userns_set_arg1(&regs_copy, (unsigned long)(gid_t)__kgid_val(k));
	}
	return real_sys_setregid(&regs_copy);
}

static long vendor_kernel_hook_setresuid(const struct pt_regs *regs)
{
	struct pt_regs regs_copy = *regs;
	struct user_namespace *ns = current_user_ns();
	uid_t ruid = (uid_t)vns_userns_arg0(regs);
	uid_t euid = (uid_t)vns_userns_arg1(regs);
	uid_t suid = (uid_t)vns_userns_arg2(regs);
	kuid_t k;

	if (ns == vns_real_init_user_ns)
		return real_sys_setresuid(regs);

	if (ruid != (uid_t)-1) {
		k = vns_make_kuid(ns, ruid);
		if (!uid_valid(k))
			return -EINVAL;
		vns_userns_set_arg0(&regs_copy, (unsigned long)(uid_t)__kuid_val(k));
	}
	if (euid != (uid_t)-1) {
		k = vns_make_kuid(ns, euid);
		if (!uid_valid(k))
			return -EINVAL;
		vns_userns_set_arg1(&regs_copy, (unsigned long)(uid_t)__kuid_val(k));
	}
	if (suid != (uid_t)-1) {
		k = vns_make_kuid(ns, suid);
		if (!uid_valid(k))
			return -EINVAL;
		vns_userns_set_arg2(&regs_copy, (unsigned long)(uid_t)__kuid_val(k));
	}
	return real_sys_setresuid(&regs_copy);
}

static long vendor_kernel_hook_setresgid(const struct pt_regs *regs)
{
	struct pt_regs regs_copy = *regs;
	struct user_namespace *ns = current_user_ns();
	gid_t rgid = (gid_t)vns_userns_arg0(regs);
	gid_t egid = (gid_t)vns_userns_arg1(regs);
	gid_t sgid = (gid_t)vns_userns_arg2(regs);
	kgid_t k;

	if (ns == vns_real_init_user_ns)
		return real_sys_setresgid(regs);

	if (rgid != (gid_t)-1) {
		k = vns_make_kgid(ns, rgid);
		if (!gid_valid(k))
			return -EINVAL;
		vns_userns_set_arg0(&regs_copy, (unsigned long)(gid_t)__kgid_val(k));
	}
	if (egid != (gid_t)-1) {
		k = vns_make_kgid(ns, egid);
		if (!gid_valid(k))
			return -EINVAL;
		vns_userns_set_arg1(&regs_copy, (unsigned long)(gid_t)__kgid_val(k));
	}
	if (sgid != (gid_t)-1) {
		k = vns_make_kgid(ns, sgid);
		if (!gid_valid(k))
			return -EINVAL;
		vns_userns_set_arg2(&regs_copy, (unsigned long)(gid_t)__kgid_val(k));
	}
	return real_sys_setresgid(&regs_copy);
}

static struct shadow_hook vendor_kernel_getuid_hook =
	SHADOW_HOOK(vendor_kernel_getuid_names, vendor_kernel_hook_getuid, &real_sys_getuid);
static struct shadow_hook vendor_kernel_geteuid_hook =
	SHADOW_HOOK(vendor_kernel_geteuid_names, vendor_kernel_hook_geteuid, &real_sys_geteuid);
static struct shadow_hook vendor_kernel_getgid_hook =
	SHADOW_HOOK(vendor_kernel_getgid_names, vendor_kernel_hook_getgid, &real_sys_getgid);
static struct shadow_hook vendor_kernel_getegid_hook =
	SHADOW_HOOK(vendor_kernel_getegid_names, vendor_kernel_hook_getegid, &real_sys_getegid);
static struct shadow_hook vendor_kernel_getresuid_hook =
	SHADOW_HOOK(vendor_kernel_getresuid_names, vendor_kernel_hook_getresuid, &real_sys_getresuid);
static struct shadow_hook vendor_kernel_getresgid_hook =
	SHADOW_HOOK(vendor_kernel_getresgid_names, vendor_kernel_hook_getresgid, &real_sys_getresgid);
static struct shadow_hook vendor_kernel_setuid_hook =
	SHADOW_HOOK(vendor_kernel_setuid_names, vendor_kernel_hook_setuid, &real_sys_setuid);
static struct shadow_hook vendor_kernel_setgid_hook =
	SHADOW_HOOK(vendor_kernel_setgid_names, vendor_kernel_hook_setgid, &real_sys_setgid);
static struct shadow_hook vendor_kernel_setreuid_hook =
	SHADOW_HOOK(vendor_kernel_setreuid_names, vendor_kernel_hook_setreuid, &real_sys_setreuid);
static struct shadow_hook vendor_kernel_setregid_hook =
	SHADOW_HOOK(vendor_kernel_setregid_names, vendor_kernel_hook_setregid, &real_sys_setregid);
static struct shadow_hook vendor_kernel_setresuid_hook =
	SHADOW_HOOK(vendor_kernel_setresuid_names, vendor_kernel_hook_setresuid, &real_sys_setresuid);
static struct shadow_hook vendor_kernel_setresgid_hook =
	SHADOW_HOOK(vendor_kernel_setresgid_names, vendor_kernel_hook_setresgid, &real_sys_setresgid);

struct shadow_hook *vendor_kernel_userns_hooks[] = {
	&vendor_kernel_getuid_hook,
	&vendor_kernel_geteuid_hook,
	&vendor_kernel_getgid_hook,
	&vendor_kernel_getegid_hook,
	&vendor_kernel_getresuid_hook,
	&vendor_kernel_getresgid_hook,
	&vendor_kernel_setuid_hook,
	&vendor_kernel_setgid_hook,
	&vendor_kernel_setreuid_hook,
	&vendor_kernel_setregid_hook,
	&vendor_kernel_setresuid_hook,
	&vendor_kernel_setresgid_hook,
	NULL,
};
