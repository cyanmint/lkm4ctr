// SPDX-License-Identifier: GPL-2.0-only
/*
 * Vendored from kernel-common kernel/nsproxy.c (kernel version 6.1.124,
 * android14-6.1 branch). CHANGES FROM UPSTREAM:
 *   - [RENAME] All non-static global symbols prefixed with vns_ to avoid
 *     collision with the built-in kernel implementation.
 *   - [BUILD-COMPAT] namespace struct allocated via kmem_cache_alloc/_zalloc
 *     against the real kernel's private cache, resolved at init via
 *     shadow_hook_resolve(), so the real kernel's own exit path can
 *     kmem_cache_free() it safely.
 *   - [BUILD-COMPAT] ns_alloc_inum/ns_free_inum -> vns_alloc_inum/vns_free_inum
 *     (proc_alloc_inum not exported; resolved at init via shadow_hook_resolve).
 *   - [BUILD-COMPAT] __init/__exit removed from non-module-init functions.
 *   - [DIAGFS] Statistics incremented via vendor_kernel_registry for diagfs exposure.
 *   Any line NOT marked RENAME/BUILD-COMPAT/DIAGFS is unchanged from upstream.
 */
/*
 *  Copyright (C) 2006 IBM Corporation
 *
 *  Author: Serge Hallyn <serue@us.ibm.com>
 *
 *  Jun 2006 - namespaces support
 *             OpenVZ, SWsoft Inc.
 *             Pavel Emelianov <xemul@openvz.org>
 */

/*
 * [BUILD-COMPAT] Must be included before any other header: several headers
 * pulled in below (<linux/nsproxy.h>, <linux/cred.h> et al) have static
 * inline helpers (e.g. the real current_user_ns() on a CONFIG_USER_NS=n
 * target) that reference the bare init_user_ns name directly -- see
 * ../../glue/vendor_kernel_data_syms.h for the full rationale. Missing this
 * previously left vns_unshare_nsproxy_namespaces()/vns_sys_setns()'s
 * current_user_ns() calls referencing the real, unexported init_user_ns
 * symbol ("Unknown symbol init_user_ns" observed at insmod on
 * android12-5.10); those call sites now use vns_current_user_ns()
 * (vendor_kernel.h) instead, which never touches current_user_ns() at all,
 * but this header is kept first regardless as defensive practice.
 */
#include "../../glue/vendor_kernel_data_syms.h"
#include <linux/slab.h>
#include <linux/export.h>
#include <linux/module.h>
#include <linux/nsproxy.h>
#include <linux/init_task.h>
#include <linux/mnt_namespace.h>
#include <linux/utsname.h>
#include <linux/pid_namespace.h>
#include <net/net_namespace.h>
#include <linux/ipc_namespace.h>
#include <linux/time_namespace.h>
#include <linux/fs_struct.h>
#include <linux/proc_fs.h>
#include <linux/proc_ns.h>
#include <linux/file.h>
#include <linux/syscalls.h>
#include <linux/cgroup.h>
#include <linux/perf_event.h>
#include <linux/hashtable.h>
#include <linux/atomic.h>
#include <linux/spinlock.h>
#include <linux/smp.h>
#include <linux/sched/task.h>
#include <linux/llist.h>
#include <linux/version.h>
#include <linux/workqueue.h>
#include "../vendor_kernel.h"
#include "../include/uapi/vendor_kernel.h"
#include "../../glue/shadow_hook.h"
#include "../../glue/lkm4ctr_log.h"

/* [BUILD-COMPAT] Forward declaration for vendored time_namespace init object. */
extern struct time_namespace vns_init_time_ns;

/*
 * [BUILD-COMPAT] vns_nsproxy_set tracks every live module-owned struct
 * nsproxy, from allocation in create_nsproxy() through final destruction in
 * vns_free_nsproxy(). It provides pointer-identity classification for
 * vns_task_exit_cleanup() and the free_nsproxy fallback, plus an exact live
 * object count for safe controlled teardown. Every tracked object holds a
 * module reference. The set distinguishes vendored objects allocated from
 * vns_nsproxy_cachep from the real kernel's init_nsproxy/
 * &vns_init_nsproxy singleton, without virt_to_cache() or task lookups.
 */
static DEFINE_HASHTABLE(vns_nsproxy_set, 6);
static DEFINE_SPINLOCK(vns_nsproxy_set_lock);
static atomic_t vns_nsproxy_live = ATOMIC_INIT(0);
static bool vns_namespace_stop_gate;
static atomic_t vns_free_nsproxy_intercepts = ATOMIC_INIT(0);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0) && \
	LINUX_VERSION_CODE < KERNEL_VERSION(5, 16, 0)
static atomic_t vns_free_nsproxy_exit_hits = ATOMIC_INIT(0);
static atomic_t vns_exit_fs_intercepts = ATOMIC_INIT(0);
#endif
static atomic_t vns_exit_task_namespaces_intercepts = ATOMIC_INIT(0);

struct vns_nsproxy_set_entry {
	struct nsproxy *ns;
	struct hlist_node node;
};

static bool vns_nsproxy_set_add(struct nsproxy *ns)
{
	struct vns_nsproxy_set_entry *entry;
	unsigned long flags;

	entry = kmalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry) {
		LKM4CTR_ERR(VENDOR_KERNEL_TAG,
			    "vns_nsproxy_set_add: tracking allocation failed for %p; refusing untracked namespace proxy",
			    ns);
		return false;
	}
	entry->ns = ns;

	spin_lock_irqsave(&vns_nsproxy_set_lock, flags);
	/* Close the check-then-register race with vns_nsproxy_stop_begin(). */
	if (vns_namespace_stop_gate) {
		spin_unlock_irqrestore(&vns_nsproxy_set_lock, flags);
		kfree(entry);
		return false;
	}
	hash_add(vns_nsproxy_set, &entry->node, (unsigned long)ns);
	atomic_inc(&vns_nsproxy_live);
	spin_unlock_irqrestore(&vns_nsproxy_set_lock, flags);
	return true;
}

static bool vns_nsproxy_set_remove(struct nsproxy *ns)
{
	struct vns_nsproxy_set_entry *entry;
	unsigned long flags;
	bool found = false;

	spin_lock_irqsave(&vns_nsproxy_set_lock, flags);
	hash_for_each_possible(vns_nsproxy_set, entry, node, (unsigned long)ns) {
		if (entry->ns == ns) {
			hash_del(&entry->node);
			atomic_dec(&vns_nsproxy_live);
			found = true;
			break;
		}
	}
	spin_unlock_irqrestore(&vns_nsproxy_set_lock, flags);

	if (found)
		kfree(entry);
	return found;
}

int vns_nsproxy_live_count(void)
{
	return atomic_read(&vns_nsproxy_live);
}

void vns_nsproxy_stop_begin(void)
{
	unsigned long flags;

	spin_lock_irqsave(&vns_nsproxy_set_lock, flags);
	smp_store_release(&vns_namespace_stop_gate, true);
	spin_unlock_irqrestore(&vns_nsproxy_set_lock, flags);
}

void vns_nsproxy_stop_abort(void)
{
	unsigned long flags;

	spin_lock_irqsave(&vns_nsproxy_set_lock, flags);
	vns_namespace_stop_gate = false;
	spin_unlock_irqrestore(&vns_nsproxy_set_lock, flags);
}

bool vns_namespace_creation_allowed(void)
{
	if (!smp_load_acquire(&vns_namespace_stop_gate))
		return true;
	return vns_semundo_clone_namespace_admitted();
}

static bool vns_nsproxy_set_contains(struct nsproxy *ns)
{
	struct vns_nsproxy_set_entry *entry;
	unsigned long flags;
	bool found = false;

	spin_lock_irqsave(&vns_nsproxy_set_lock, flags);
	hash_for_each_possible(vns_nsproxy_set, entry, node, (unsigned long)ns) {
		if (entry->ns == ns) {
			found = true;
			break;
		}
	}
	spin_unlock_irqrestore(&vns_nsproxy_set_lock, flags);

	return found;
}


struct nsproxy vns_init_nsproxy = { /* [RENAME] */
	.count			= ATOMIC_INIT(1),
	.uts_ns			= &init_uts_ns,
#if defined(CONFIG_POSIX_MQUEUE) || defined(CONFIG_SYSVIPC)
	/* [BUILD-COMPAT] init_ipc_ns is not exported; set at runtime in vendor_kernel_init(). */
	.ipc_ns			= NULL,
#endif
	.mnt_ns			= NULL,
	.pid_ns_for_children	= &init_pid_ns,
#ifdef CONFIG_NET
	.net_ns			= &init_net,
#endif
#ifdef CONFIG_CGROUPS
	/* [BUILD-COMPAT] init_cgroup_ns is not exported; set at runtime via
	 * shadow_hook_resolve("init_cgroup_ns") in vendor_kernel_init(). */
	.cgroup_ns		= NULL,
#endif
#ifdef CONFIG_TIME_NS
	.time_ns		= &vns_init_time_ns, /* [RENAME] init_time_ns → vns_init_time_ns */
	.time_ns_for_children	= &vns_init_time_ns, /* [RENAME] */
#endif
};

static inline struct nsproxy *create_nsproxy(void)
{
	struct nsproxy *nsproxy;

	if (smp_load_acquire(&vns_namespace_stop_gate))
		return NULL;
	if (!try_module_get(THIS_MODULE))
		return NULL;

	/* [BUILD-COMPAT] allocate from vendor_kernel's own module-owned
	 * nsproxy cache (created in vns_nsproxy_cache_init()) instead of the
	 * real kernel's private nsproxy_cachep. See vns_task_exit_cleanup()
	 * below for how the real kernel's own exit path is kept from ever
	 * touching this module-owned object. */
	nsproxy = kmem_cache_alloc(vns_nsproxy_cachep, GFP_KERNEL);
	if (nsproxy) {
		vns_init_count(&nsproxy->count, 1); /* [BUILD-COMPAT] */
		if (!vns_nsproxy_set_add(nsproxy)) {
			kmem_cache_free(vns_nsproxy_cachep, nsproxy);
			nsproxy = NULL;
		}
	}
	if (!nsproxy)
		module_put(THIS_MODULE);
	return nsproxy;
}

/*
 * Create new nsproxy and all of its the associated namespaces.
 * Return the newly created nsproxy.  Do not attach this to the task,
 * leave it to the caller to do proper locking and attach it to task.
 */
/*
 * [BUILD-COMPAT] __nocfi: this function's vns_copy_mnt_ns_fn() call below is
 * a genuine CFI-unsafe indirect call, through a pointer resolved at runtime
 * via shadow_hook_resolve("copy_mnt_ns") (vendor_kernel_module.c), not known
 * to the compiler at this call site. On CONFIG_CFI_CLANG=y GKI kernels
 * (5.15+) this previously panicked with "CFI failure ... (target:
 * copy_mnt_ns+...)" from inside create_new_namespaces(). Marking just this
 * function __nocfi (rather than disabling CFI for the whole nsproxy.o via
 * CFLAGS_REMOVE_<obj>.o in lkm4ctr/Makefile) keeps the rest of this file --
 * including vns_exit_task_namespaces_hook(), the shadow-hook replacement
 * reached from a real-kernel-invoked kprobe/ftrace callback --
 * CFI-instrumented and a valid indirect-call target for the real kernel,
 * matching the pattern used elsewhere in this module (see e.g.
 * vendor_kernel_procfs_userns.c's vns_idmap_create_fd()).
 */
static __nocfi struct nsproxy *create_new_namespaces(unsigned long flags,
	struct task_struct *tsk, struct user_namespace *user_ns,
	struct fs_struct *new_fs)
{
	struct nsproxy *new_nsp;
	int err;

	new_nsp = create_nsproxy();
	if (!new_nsp)
		return ERR_PTR(vns_namespace_creation_allowed() ? -ENOMEM : -EBUSY);

	/* [BUILD-COMPAT] copy_mnt_ns is resolved lazily for vendor_kernel. */
	if (vns_copy_mnt_ns_fn) {
		new_nsp->mnt_ns = vns_copy_mnt_ns_fn(flags, tsk->nsproxy->mnt_ns, user_ns, new_fs);
		if (IS_ERR(new_nsp->mnt_ns)) {
			err = PTR_ERR(new_nsp->mnt_ns);
			goto out_ns;
		}
	} else {
		new_nsp->mnt_ns = NULL;
	}

	new_nsp->uts_ns = vns_copy_utsname(flags, user_ns, tsk->nsproxy->uts_ns); /* [RENAME] */
	if (IS_ERR(new_nsp->uts_ns)) {
		err = PTR_ERR(new_nsp->uts_ns);
		goto out_uts;
	}

	new_nsp->ipc_ns = vns_copy_ipcs(flags, user_ns, tsk->nsproxy->ipc_ns); /* [RENAME] */
	if (IS_ERR(new_nsp->ipc_ns)) {
		err = PTR_ERR(new_nsp->ipc_ns);
		goto out_ipc;
	}

	new_nsp->pid_ns_for_children =
		vns_copy_pid_ns(flags, user_ns, tsk->nsproxy->pid_ns_for_children); /* [RENAME] */
	if (IS_ERR(new_nsp->pid_ns_for_children)) {
		err = PTR_ERR(new_nsp->pid_ns_for_children);
		goto out_pid;
	}

	new_nsp->cgroup_ns = vns_copy_cgroup_ns(flags, user_ns, /* [RENAME] */
					    tsk->nsproxy->cgroup_ns);
	if (IS_ERR(new_nsp->cgroup_ns)) {
		err = PTR_ERR(new_nsp->cgroup_ns);
		goto out_cgroup;
	}

	/*
	 * [BUILD-COMPAT] copy_net_ns is resolved lazily for vendor_kernel, and
	 * CLONE_NEWNET is deliberately masked out of the flags passed to it:
	 * the real kernel's copy_net_ns()/setup_net() path (net_alloc() ->
	 * ops_init() -> every registered pernet_operations, including
	 * xfrm4_net_init()'s __percpu_counter_init()) has been observed to
	 * corrupt the kernel-wide percpu_counters list ("list_add corruption
	 * ... kernel BUG at lib/list_debug.c:29", Call trace through
	 * xfrm4_net_init -> __percpu_counter_init -> ops_init -> setup_net ->
	 * copy_net_ns -> create_new_namespaces [lkm4ctr]) the first time it is
	 * asked to actually build a brand-new struct net from this call site,
	 * even though every other vendored namespace type built alongside it
	 * here (UTS/IPC/PID/CGROUP/TIME) is unaffected. Until that is fully
	 * root-caused, always take copy_net_ns()'s own safe "just grab another
	 * reference" fast path (the same one already exercised, without
	 * incident, by every unshare() call that does *not* request
	 * CLONE_NEWNET) instead of risking this crash -- i.e. CLONE_NEWNET is
	 * bookkeeping-only here, matching the checker's STUB result for it and
	 * the same safety-over-completeness posture already documented for
	 * MNT_NS in vendor/README.md.
	 */
	if (vns_copy_net_ns_fn) {
		new_nsp->net_ns = vns_copy_net_ns_fn(flags & ~CLONE_NEWNET, user_ns,
						      tsk->nsproxy->net_ns);
		if (IS_ERR(new_nsp->net_ns)) {
			err = PTR_ERR(new_nsp->net_ns);
			goto out_net;
		}
	} else {
		/*
		 * [BUILD-COMPAT] copy_net_ns unresolved (e.g. target kernel
		 * built with CONFIG_NET_NS=n, so it has no standalone exported
		 * symbol): net_ns must still never be NULL on a CONFIG_NET=y
		 * kernel (real kernel code unconditionally dereferences
		 * nsproxy->net_ns), so just keep sharing the task's existing
		 * net_ns exactly like the real kernel's own inline fallback
		 * would.
		 */
		new_nsp->net_ns = get_net(tsk->nsproxy->net_ns);
	}

	new_nsp->time_ns_for_children = vns_copy_time_ns(flags, user_ns, /* [RENAME] */
					tsk->nsproxy->time_ns_for_children);
	if (IS_ERR(new_nsp->time_ns_for_children)) {
		err = PTR_ERR(new_nsp->time_ns_for_children);
		goto out_time;
	}
	new_nsp->time_ns = get_time_ns(tsk->nsproxy->time_ns);

	return new_nsp;

out_time:
	if (new_nsp->net_ns)
		put_net(new_nsp->net_ns); /* [BUILD-COMPAT] real inline, not resolved by name */
out_net:
	vns_put_cgroup_ns(new_nsp->cgroup_ns); /* [RENAME] */
out_cgroup:
	if (new_nsp->pid_ns_for_children)
		vns_put_pid_ns(new_nsp->pid_ns_for_children); /* [RENAME] */
out_pid:
	if (new_nsp->ipc_ns)
		vns_put_ipc_ns(new_nsp->ipc_ns); /* [RENAME] */
out_ipc:
	if (new_nsp->uts_ns)
		vns_put_uts_ns(new_nsp->uts_ns); /* [BUILD-COMPAT] */
out_uts:
	if (new_nsp->mnt_ns)
		if (vns_put_mnt_ns_fn)
			vns_put_mnt_ns_fn(new_nsp->mnt_ns); /* [BUILD-COMPAT] */
out_ns:
	/* create_nsproxy() registered this unpublished object and took a module
	 * reference. Construction failures reach this label directly instead of
	 * going through vns_free_nsproxy(), so retire both explicitly here.
	 */
	vns_nsproxy_set_remove(new_nsp);
	kmem_cache_free(vns_nsproxy_cachep, new_nsp); /* [BUILD-COMPAT] */
	module_put(THIS_MODULE);
	return ERR_PTR(err);
}

/*
 * called from clone.  This now handles copy for nsproxy and all
 * namespaces therein.
 */
struct nsproxy *vns_copy_namespaces(unsigned long flags, struct task_struct *tsk) /* [RENAME] */
{
	struct nsproxy *old_ns = tsk->nsproxy;
	struct user_namespace *user_ns = task_cred_xxx(tsk, user_ns);
	struct nsproxy *new_ns;

	if (likely(!(flags & (CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWIPC |
			      CLONE_NEWPID | CLONE_NEWNET |
			      CLONE_NEWCGROUP | CLONE_NEWTIME)))) {
		if (likely(old_ns->time_ns_for_children == old_ns->time_ns)) {
			get_nsproxy(old_ns);
			return old_ns;
		}
	} else if (!ns_capable(user_ns, CAP_SYS_ADMIN))
		return ERR_PTR(-EPERM);

	/*
	 * CLONE_NEWIPC must detach from the undolist: after switching
	 * to a new ipc namespace, the semaphore arrays from the old
	 * namespace are unreachable.  In clone parlance, CLONE_SYSVSEM
	 * means share undolist with parent, so we must forbid using
	 * it along with CLONE_NEWIPC.
	 */
	if ((flags & (CLONE_NEWIPC | CLONE_SYSVSEM)) ==
		(CLONE_NEWIPC | CLONE_SYSVSEM))
		return ERR_PTR(-EINVAL);

	new_ns = create_new_namespaces(flags, tsk, user_ns, tsk->fs);
	if (IS_ERR(new_ns))
		return new_ns;

	vns_timens_on_fork(new_ns, tsk); /* [RENAME] */

	return new_ns;
}

/*
 * [BUILD-COMPAT] __nocfi: like create_new_namespaces() above, this
 * function's vns_put_mnt_ns_fn() call below is a genuine CFI-unsafe
 * indirect call through a pointer resolved at runtime via
 * shadow_hook_resolve("put_mnt_ns") (vendor_kernel_module.c), not known to
 * the compiler at this call site. On CONFIG_CFI_CLANG=y GKI kernels
 * (5.15+) this panicked with "CFI failure ... (target: put_mnt_ns+...)"
 * from inside vns_free_nsproxy() itself (called from
 * vns_nsproxy_deferred_put_fn()'s workqueue context). Marking just this
 * function __nocfi keeps the rest of this file -- including
 * vns_exit_kprobe_pre_handler(), a real-kernel-invoked kprobe pre_handler
 * callback -- CFI-instrumented and a valid indirect-call target for the
 * real kernel.
 */
void __nocfi vns_free_nsproxy(struct nsproxy *ns) /* [RENAME] */
{
	bool module_pinned;

	if (ns->mnt_ns)
		if (vns_put_mnt_ns_fn)
			vns_put_mnt_ns_fn(ns->mnt_ns); /* [BUILD-COMPAT] */
	if (ns->uts_ns)
		vns_put_uts_ns(ns->uts_ns); /* [BUILD-COMPAT] */
	if (ns->ipc_ns)
		vns_put_ipc_ns(ns->ipc_ns); /* [RENAME] */
	if (ns->pid_ns_for_children)
		vns_put_pid_ns(ns->pid_ns_for_children); /* [RENAME] */
	if (ns->time_ns)
		vns_put_time_ns(ns->time_ns); /* [RENAME] */
	if (ns->time_ns_for_children)
		vns_put_time_ns(ns->time_ns_for_children); /* [RENAME] */
	vns_put_cgroup_ns(ns->cgroup_ns); /* [RENAME] */
	if (ns->net_ns)
		put_net(ns->net_ns); /* [BUILD-COMPAT] real inline, not resolved by name */
	module_pinned = vns_nsproxy_set_remove(ns); /* [BUILD-COMPAT] */
	kmem_cache_free(vns_nsproxy_cachep, ns); /* [BUILD-COMPAT] */
	if (module_pinned)
		module_put(THIS_MODULE);
	else
		LKM4CTR_ERR(VENDOR_KERNEL_TAG,
			    "vns_free_nsproxy: untracked vendored nsproxy %p; module lifetime pin missing",
			    ns);
}

/*
 * [BUILD-COMPAT] Drop a reference to a *foreign* struct nsproxy * -- i.e. one
 * vendor_kernel did not itself allocate from vns_nsproxy_cachep (tracked via
 * vns_nsproxy_set_add()/vns_nsproxy_set_contains()). The very first time a
 * task calls unshare()/setns() through vendor_kernel's hooks, its existing
 * tsk->nsproxy is still whatever the real kernel installed (its own
 * init_nsproxy, or a real nsproxy previously built by the real kernel's own,
 * unhooked create_new_namespaces()); vns_switch_task_namespaces() below must
 * still release that task's one reference to it, but MUST NOT run it through
 * vendor_kernel's own vns_free_nsproxy() if the refcount reaches zero: that
 * function unconditionally ends with
 * kmem_cache_free(vns_nsproxy_cachep, ns), which for a real, kernel-allocated
 * nsproxy is a free into the *wrong* kmem_cache and corrupts the slab
 * allocator (observed as unrelated-looking "list_del corruption"/kernel BUG
 * crashes much later, e.g. in cleanup_net()'s xfrm4_net_exit ->
 * percpu_counter_destroy()). Route the real free through the real kernel's
 * own (resolved-by-name) free_nsproxy() instead; if that could not be
 * resolved, leak the reference rather than risk corrupting memory.
 */
static void vns_put_foreign_nsproxy(struct nsproxy *ns)
{
	if (!vns_put_count(&ns->count))
		return;
	if (vns_real_free_nsproxy_fn) {
		vns_real_free_nsproxy_fn(ns);
		return;
	}
	LKM4CTR_WARN(VENDOR_KERNEL_TAG,
		"vns_put_foreign_nsproxy: free_nsproxy unresolved, leaking foreign nsproxy %p",
		ns);
}


/*
 * Called from unshare. Unshare all the namespaces part of nsproxy.
 * On success, returns the new nsproxy.
 */
int vns_unshare_nsproxy_namespaces(unsigned long unshare_flags, /* [RENAME] */
	struct nsproxy **new_nsp, struct cred *new_cred, struct fs_struct *new_fs)
{
	struct user_namespace *user_ns;
	int err = 0;

	if (!(unshare_flags & (CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWIPC |
			       CLONE_NEWNET | CLONE_NEWPID | CLONE_NEWCGROUP |
			       CLONE_NEWTIME)))
		return 0;

	user_ns = new_cred ? new_cred->user_ns : vns_current_user_ns(); /* [BUILD-COMPAT] */
	if (!ns_capable(user_ns, CAP_SYS_ADMIN))
		return -EPERM;

	*new_nsp = create_new_namespaces(unshare_flags, current, user_ns,
					 new_fs ? new_fs : current->fs);
	if (IS_ERR(*new_nsp)) {
		err = PTR_ERR(*new_nsp);
		goto out;
	}

out:
	return err;
}

void vns_switch_task_namespaces(struct task_struct *p, struct nsproxy *new) /* [RENAME] */
{
	struct nsproxy *ns;

	might_sleep();

	task_lock(p);
	ns = p->nsproxy;
	p->nsproxy = new;
	task_unlock(p);

	if (!ns)
		return;
	/*
	 * [BUILD-COMPAT] ns may be a module-owned object from a previous
	 * vendor_kernel unshare()/setns()/clone(), or -- on the very first
	 * call for this task -- still the real kernel's own nsproxy (its
	 * shared init_nsproxy, or one the real, unhooked kernel created
	 * earlier). Route the release accordingly; see
	 * vns_put_foreign_nsproxy()'s comment above for why this distinction
	 * is safety-critical.
	 */
	if (vns_nsproxy_set_contains(ns))
		vns_put_nsproxy(ns); /* [RENAME] */
	else
		vns_put_foreign_nsproxy(ns);
}

void vns_exit_task_namespaces(struct task_struct *p) /* [RENAME] */
{
	vns_switch_task_namespaces(p, NULL);
}

static int check_setns_flags(unsigned long flags)
{
	if (!flags || (flags & ~(CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWIPC |
				 CLONE_NEWNET | CLONE_NEWTIME | CLONE_NEWUSER |
				 CLONE_NEWPID | CLONE_NEWCGROUP)))
		return -EINVAL;

#ifndef CONFIG_USER_NS
	if (flags & CLONE_NEWUSER)
		return -EINVAL;
#endif
#ifndef CONFIG_PID_NS
	if (flags & CLONE_NEWPID)
		return -EINVAL;
#endif
#ifndef CONFIG_UTS_NS
	if (flags & CLONE_NEWUTS)
		return -EINVAL;
#endif
#ifndef CONFIG_IPC_NS
	if (flags & CLONE_NEWIPC)
		return -EINVAL;
#endif
#ifndef CONFIG_CGROUPS
	if (flags & CLONE_NEWCGROUP)
		return -EINVAL;
#endif
#ifndef CONFIG_NET_NS
	if (flags & CLONE_NEWNET)
		return -EINVAL;
#endif
#ifndef CONFIG_TIME_NS
	if (flags & CLONE_NEWTIME)
		return -EINVAL;
#endif

	return 0;
}

static void put_nsset(struct nsset *nsset)
{
	unsigned flags = nsset->flags;

	if (flags & CLONE_NEWUSER)
		put_cred(nsset_cred(nsset));
	/*
	 * We only created a temporary copy if we attached to more than just
	 * the mount namespace.
	 */
	if (nsset->fs && (flags & CLONE_NEWNS) && (flags & ~CLONE_NEWNS))
		free_fs_struct(nsset->fs);
	if (nsset->nsproxy)
		vns_free_nsproxy(nsset->nsproxy); /* [RENAME] */
}

static int prepare_nsset(unsigned flags, struct nsset *nsset)
{
	struct task_struct *me = current;

	nsset->nsproxy = create_new_namespaces(0, me, vns_current_user_ns(), me->fs); /* [BUILD-COMPAT] */
	if (IS_ERR(nsset->nsproxy))
		return PTR_ERR(nsset->nsproxy);

	if (flags & CLONE_NEWUSER)
		nsset->cred = prepare_creds();
	else
		nsset->cred = current_cred();
	if (!nsset->cred)
		goto out;

	/* Only create a temporary copy of fs_struct if we really need to. */
	if (flags == CLONE_NEWNS) {
		nsset->fs = me->fs;
	} else if (flags & CLONE_NEWNS) {
		nsset->fs = copy_fs_struct(me->fs);
		if (!nsset->fs)
			goto out;
	}

	nsset->flags = flags;
	return 0;

out:
	put_nsset(nsset);
	return -ENOMEM;
}

static inline int validate_ns(struct nsset *nsset, struct ns_common *ns)
{
	return ns->ops->install(nsset, ns);
}

/*
 * This is the inverse operation to unshare().
 * Ordering is equivalent to the standard ordering used everywhere else
 * during unshare and process creation. The switch to the new set of
 * namespaces occurs at the point of no return after installation of
 * all requested namespaces was successful in commit_nsset().
 */
static int validate_nsset(struct nsset *nsset, struct pid *pid)
{
	int ret = 0;
	unsigned flags = nsset->flags;
	struct user_namespace *user_ns = NULL;
	struct pid_namespace *pid_ns = NULL;
	struct nsproxy *nsp;
	struct task_struct *tsk;

	/* Take a "snapshot" of the target task's namespaces. */
	rcu_read_lock();
	tsk = pid_task(pid, PIDTYPE_PID);
	if (!tsk) {
		rcu_read_unlock();
		return -ESRCH;
	}

	if (!ptrace_may_access(tsk, PTRACE_MODE_READ_REALCREDS)) {
		rcu_read_unlock();
		return -EPERM;
	}

	task_lock(tsk);
	nsp = tsk->nsproxy;
	if (nsp)
		get_nsproxy(nsp);
	task_unlock(tsk);
	if (!nsp) {
		rcu_read_unlock();
		return -ESRCH;
	}

	if (flags & CLONE_NEWPID) {
		pid_ns = task_active_pid_ns(tsk);
		if (unlikely(!pid_ns)) {
			rcu_read_unlock();
			ret = -ESRCH;
			goto out;
		}
		vns_get_pid_ns(pid_ns); /* [BUILD-COMPAT] */
	}

	if (flags & CLONE_NEWUSER)
		user_ns = vns_get_user_ns(__task_cred(tsk)->user_ns); /* [BUILD-COMPAT] */
	rcu_read_unlock();

	/*
	 * Install requested namespaces. The caller will have
	 * verified earlier that the requested namespaces are
	 * supported on this kernel. We don't report errors here
	 * if a namespace is requested that isn't supported.
	 */
	if (flags & CLONE_NEWUSER) {
		ret = validate_ns(nsset, &user_ns->ns);
		if (ret)
			goto out;
	}

	if (flags & CLONE_NEWNS) {
		ret = validate_ns(nsset, from_mnt_ns(nsp->mnt_ns));
		if (ret)
			goto out;
	}

	/*
	 * [BUILD-COMPAT] Unlike upstream, this is not gated on CONFIG_UTS_NS:
	 * vendor_kernel always vendors its own UTS namespace support,
	 * independent of whether the target kernel's own CONFIG_UTS_NS is y
	 * or n (see vendor/kernel/utsname.c).
	 */
	if (flags & CLONE_NEWUTS) {
		ret = validate_ns(nsset, &nsp->uts_ns->ns);
		if (ret)
			goto out;
	}

	/*
	 * [BUILD-COMPAT] Unlike upstream, this is not gated on CONFIG_IPC_NS:
	 * vendor_kernel always vendors its own IPC namespace support,
	 * independent of whether the target kernel's own CONFIG_IPC_NS is y
	 * or n (see vendor/ipc/namespace.c). Leaving this behind
	 * `#ifdef CONFIG_IPC_NS` compiled the whole CLONE_NEWIPC install out
	 * on the CONFIG_NAMESPACES=n target kernels this module exists for.
	 */
	if (flags & CLONE_NEWIPC) {
		ret = validate_ns(nsset, &nsp->ipc_ns->ns);
		if (ret)
			goto out;
	}

	/*
	 * [BUILD-COMPAT] Unlike upstream, this is not gated on CONFIG_PID_NS:
	 * vendor_kernel always vendors its own pid namespace support (see
	 * vns_get_pid_ns()/vns_put_pid_ns() above), independent of whether
	 * the target kernel's own CONFIG_PID_NS is y or n.
	 */
	if (flags & CLONE_NEWPID) {
		ret = validate_ns(nsset, &pid_ns->ns);
		if (ret)
			goto out;
	}

#ifdef CONFIG_CGROUPS
	if (flags & CLONE_NEWCGROUP) {
		ret = validate_ns(nsset, &nsp->cgroup_ns->ns);
		if (ret)
			goto out;
	}
#endif

#ifdef CONFIG_NET_NS
	if (flags & CLONE_NEWNET) {
		ret = validate_ns(nsset, &nsp->net_ns->ns);
		if (ret)
			goto out;
	}
#endif

#ifdef CONFIG_TIME_NS
	if (flags & CLONE_NEWTIME) {
		ret = validate_ns(nsset, &nsp->time_ns->ns);
		if (ret)
			goto out;
	}
#endif

out:
	if (pid_ns)
		vns_put_pid_ns(pid_ns); /* [BUILD-COMPAT] */
	if (nsp)
		vns_put_nsproxy(nsp); /* [RENAME] */
	vns_put_user_ns(user_ns); /* [BUILD-COMPAT] */

	return ret;
}


/*
 * This is the point of no return. There are just a few namespaces
 * that do some actual work here and it's sufficiently minimal that
 * a separate ns_common operation seems unnecessary for now.
 * Unshare is doing the same thing. If we'll end up needing to do
 * more in a given namespace or a helper here is ultimately not
 * exported anymore a simple commit handler for each namespace
 * should be added to ns_common.
 */
static void commit_nsset(struct nsset *nsset)
{
	unsigned flags = nsset->flags;
	struct task_struct *me = current;

	/*
	 * [BUILD-COMPAT] Unlike upstream, this is not gated on CONFIG_USER_NS:
	 * vendor_kernel always vendors its own user namespace support,
	 * independent of the target kernel's own CONFIG_USER_NS setting.
	 */
	if (flags & CLONE_NEWUSER) {
		/* transfer ownership */
		commit_creds(nsset_cred(nsset));
		nsset->cred = NULL;
	}

	/* We only need to commit if we have used a temporary fs_struct. */
	if ((flags & CLONE_NEWNS) && (flags & ~CLONE_NEWNS)) {
		set_fs_root(me->fs, &nsset->fs->root);
		set_fs_pwd(me->fs, &nsset->fs->pwd);
	}

	/*
	 * [BUILD-COMPAT] Unlike upstream, this is not gated on CONFIG_IPC_NS:
	 * vendor_kernel always vendors its own IPC namespace support (see
	 * vendor/ipc/namespace.c), independent of whether the target
	 * kernel's own CONFIG_IPC_NS is y or n. `exit_sem` is always
	 * redirected to `vns_exit_sem` (vendor_kernel.h) regardless of
	 * config, but leaving the *call site* behind `#ifdef CONFIG_IPC_NS`
	 * compiled it out entirely on the CONFIG_NAMESPACES=n target
	 * kernels this module exists for. That silently skipped detaching
	 * the task's SysV semaphore undo list from the *old* ipc_ns before
	 * vns_switch_task_namespaces() below installs the new one, leaving
	 * stale, namespace-relative `semid`-keyed sem_undo entries (from the
	 * old ipc_ns's semaphore IDR) attached to the task. A later semop()
	 * in the new ipc_ns can then find_alloc_undo() a stale entry whose
	 * semadj[] array size doesn't match the new (unrelated) semaphore
	 * array reusing that same numeric semid, corrupting adjacent heap
	 * memory -- surfacing later as an unrelated-looking, delayed slab/
	 * heap-corruption panic, e.g. during `docker exec` into a container.
	 */
	if (flags & CLONE_NEWIPC)
		exit_sem(me);

#ifdef CONFIG_TIME_NS
	if (flags & CLONE_NEWTIME)
		vns_timens_commit(me, nsset->nsproxy->time_ns); /* [RENAME] */
#endif

	/* transfer ownership */
	vns_switch_task_namespaces(me, nsset->nsproxy); /* [RENAME] */
	nsset->nsproxy = NULL;
}

long vns_sys_setns(int fd, int flags) /* [RENAME] */
{
	struct file *file;
	struct ns_common *ns = NULL;
	struct nsset nsset = {};
	int err = 0;

	file = fget(fd);
	if (!file)
		return -EBADF;

	if (proc_ns_file(file)) {
		ns = get_proc_ns(file_inode(file));
		if (flags && (vns_get_ns_type(ns) != flags))
			err = -EINVAL;
		flags = vns_get_ns_type(ns);
	} else if (!IS_ERR(pidfd_pid(file))) {
		err = check_setns_flags(flags);
	} else {
		err = -EINVAL;
	}
	if (err)
		goto out;

	err = prepare_nsset(flags, &nsset);
	if (err)
		goto out;

	if (proc_ns_file(file))
		err = validate_ns(&nsset, ns);
	else
		err = validate_nsset(&nsset, file->private_data);
	if (!err) {
		commit_nsset(&nsset);
		vns_registry_set_nsproxy(task_tgid_nr(current), current->nsproxy); /* [DIAGFS] */
		perf_event_namespaces(current);
	}
	put_nsset(&nsset);
out:
	fput(file);
	return err;
}



void vns_put_nsproxy(struct nsproxy *ns) /* [RENAME] */
{
	if (ns && vns_put_count(&ns->count))
		vns_free_nsproxy(ns);
}

void vns_nsproxy_cache_init(void) /* [BUILD-COMPAT] */
{
	/* [BUILD-COMPAT] module-owned cache: see vendor/README.md's
	 * "Slab-cache consistency with the real kernel" for why this no
	 * longer resolves the real kernel's private nsproxy_cachep. */
	if (!vns_nsproxy_cachep)
		vns_nsproxy_cachep = kmem_cache_create("vns_nsproxy",
			sizeof(struct nsproxy), 0,
			SLAB_HWCACHE_ALIGN | SLAB_ACCOUNT, NULL);

	/*
	 * Pin vns_init_nsproxy's refcount to a large sentinel value so it can
	 * never legitimately reach zero and be mistaken for a freeable
	 * object -- it is a static singleton, never slab-allocated, and is
	 * only ever used as the safe fallback nsproxy in
	 * vns_task_exit_cleanup() below.
	 */
	vns_init_count(&vns_init_nsproxy.count, 0x40000000);
}

/*
 * vns_task_exit_cleanup() runs from the exit_task_namespaces() replacement
 * below, or from the exit_fs() fallback on Linux 5.15, in the exiting task's
 * normal process context. vns_put_nsproxy()/vns_free_nsproxy() can reach
 * put_mnt_ns() -> namespace_unlock() -> synchronize_rcu_expedited(); keep
 * that potentially long mount/RCU teardown off the task-exit path by
 * deferring it to a workqueue. Only the task_lock()-protected pointer swap
 * and plain refcounting happen inline before the real kernel teardown.
 */
struct vns_nsproxy_deferred_put {
	struct llist_node node;
	struct nsproxy *ns;
};

static LLIST_HEAD(vns_nsproxy_deferred_list);

static void vns_nsproxy_deferred_put_fn(struct work_struct *work)
{
	struct llist_node *node = llist_del_all(&vns_nsproxy_deferred_list);
	struct vns_nsproxy_deferred_put *entry, *tmp;

	llist_for_each_entry_safe(entry, tmp, node, node) {
		vns_put_nsproxy(entry->ns); /* [RENAME] */
		kfree(entry);
	}
}
static DECLARE_WORK(vns_nsproxy_deferred_put_work, vns_nsproxy_deferred_put_fn);

void vns_nsproxy_deferred_flush(void)
{
	flush_work(&vns_nsproxy_deferred_put_work);
}

static void vns_nsproxy_put_deferred(struct nsproxy *ns)
{
	struct vns_nsproxy_deferred_put *entry;

	entry = kmalloc(sizeof(*entry), GFP_ATOMIC);
	if (!entry) {
		/*
		 * Atomic allocation failure for a tiny fixed-size object is
		 * exceedingly unlikely; there is no safe way to free ns
		 * synchronously here (see the atomic-context comment above),
		 * so log and leak the reference rather than risk a second
		 * "scheduling while atomic" crash.
		 */
		LKM4CTR_WARN(VENDOR_KERNEL_TAG,
			"vns_nsproxy_put_deferred: kmalloc failed, leaking nsproxy %p", ns);
		return;
	}
	entry->ns = ns;
	llist_add(&entry->node, &vns_nsproxy_deferred_list);
	schedule_work(&vns_nsproxy_deferred_put_work);
}

/*
 * Hand off a vendored PID namespace's child reaper before native
 * exit_notify()->find_child_reaper() reaches the target kernel's CONFIG_PID_NS=n
 * zap_pid_ns_processes() BUG stub. The exit_task_namespaces() shadow hook is
 * the primary per-task entry point; the free_nsproxy() hook remains a fallback
 * when that call site is inlined or otherwise not intercepted.
 */
static void vns_task_exit_pidns_cleanup(struct task_struct *tsk)
{
	struct pid_namespace *pid_ns;
	bool handed_off = false;

	if (vns_pidns_runtime_supported)
		return;

	pid_ns = task_active_pid_ns(tsk);
	if (!pid_ns || pid_ns == &init_pid_ns ||
	    READ_ONCE(pid_ns->child_reaper) != tsk)
		return;

	/* Only the non-blocking cascade is safe on this task-exit path. */
	vns_zap_pid_ns_processes(pid_ns);

	/* Serialize with find_child_reaper(), which reads this under tasklist_lock. */
	write_lock_irq(&tasklist_lock);
	if (pid_ns->child_reaper == tsk) {
		pid_ns->child_reaper = READ_ONCE(init_pid_ns.child_reaper);
		handed_off = true;
	}
	write_unlock_irq(&tasklist_lock);

	if (handed_off)
		LKM4CTR_INFO(VENDOR_KERNEL_TAG,
			     "[DEBUG-KMI-EXIT] handed off PID namespace reaper pid=%d comm=%s pid_ns=%p",
			     task_pid_nr(tsk), tsk->comm, pid_ns);
}

/*
 * vns_task_exit_cleanup() - see the declaration comment in vendor_kernel.h.
 * Runs from the exit_task_namespaces() shadow hook, and from the earlier
 * exit_fs() hook on Linux 5.15 where that callback is not reached. The
 * free_nsproxy() hook invokes only the PID-reaper cleanup above as a final
 * fallback when neither task-exit call site is intercepted.
 */
void vns_task_exit_cleanup(struct task_struct *tsk) /* [BUILD-COMPAT] */
{
	struct nsproxy *ns;

	vns_task_exit_pidns_cleanup(tsk);

	vns_prepare_exit_sem(tsk);
	vns_prepare_exit_shm(tsk);

	task_lock(tsk);
	ns = tsk->nsproxy;
	if (!ns || !vns_nsproxy_set_contains(ns)) {
		task_unlock(tsk);
		return;
	}
	get_nsproxy(&vns_init_nsproxy);
	tsk->nsproxy = &vns_init_nsproxy;
	task_unlock(tsk);

	/*
	 * From here on, the native switch_task_namespaces() call will only
	 * see &vns_init_nsproxy (pinned, never slab-allocated) on this task,
	 * so native free_nsproxy() will not receive a module-owned object and
	 * call kmem_cache_free() against the wrong cache. Tear the vendored
	 * object down through
	 * vendor_kernel's own self-contained free path, deferred to process
	 * context (see the atomic-context comment above this function).
	 */
	vns_nsproxy_put_deferred(ns);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0) && \
	LINUX_VERSION_CODE < KERNEL_VERSION(5, 16, 0)
/*
 * On the Android 13/14 5.15 stock Images, exit_task_namespaces() resolves
 * and installs but its shadow callback is not reached by the do_exit path.
 * exit_fs() is a distinct, returning call immediately before it in do_exit;
 * run the normal-context cleanup here so the native exit_notify() cannot
 * reach CONFIG_PID_NS=n's zap_pid_ns_processes() BUG with a vendored reaper.
 */
static void (*vns_real_exit_fs)(struct task_struct *);

static const char * const vns_exit_fs_names[] = {
	"exit_fs", NULL,
};

static void __nocfi vns_call_real_exit_fs(struct task_struct *tsk)
{
	vns_real_exit_fs(tsk);
}

static void vns_exit_fs_hook(struct task_struct *tsk)
{
	if (tsk->flags & PF_EXITING) {
		int hit = atomic_inc_return(&vns_exit_fs_intercepts);

		if (hit <= 8) {
			struct pid_namespace *pid_ns = task_active_pid_ns(tsk);

			LKM4CTR_INFO(VENDOR_KERNEL_TAG,
				     "[DEBUG-KMI-EXIT] exit_fs hook #%d pid=%d comm=%s tracked_nsproxy=%d child_reaper=%d",
				     hit, task_pid_nr(tsk), tsk->comm,
				     vns_nsproxy_set_contains(READ_ONCE(tsk->nsproxy)),
				     pid_ns && READ_ONCE(pid_ns->child_reaper) == tsk);
		}

		vns_task_exit_cleanup(tsk);
	}

	vns_call_real_exit_fs(tsk);
}

static struct shadow_hook vns_exit_fs_shadow_hook =
	SHADOW_HOOK(vns_exit_fs_names, vns_exit_fs_hook, &vns_real_exit_fs);
#endif

/*
 * As a final ownership boundary, route native free_nsproxy() calls for
 * tracked vendored objects through the vendored destructor. The earlier
 * exit_task_namespaces() hook normally swaps these objects out first, but
 * stock GKI exit paths can still reach native free_nsproxy() with a
 * module-owned nsproxy; the native destructor would free its child objects
 * through the wrong slab caches.
 */
static const char * const vns_free_nsproxy_names[] = {
	"free_nsproxy", NULL,
};

static void vns_free_nsproxy_hook(struct nsproxy *ns)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0) && \
	LINUX_VERSION_CODE < KERNEL_VERSION(5, 16, 0)
	if (current->flags & PF_EXITING) {
		struct pid_namespace *pid_ns = task_active_pid_ns(current);
		bool is_child_reaper = pid_ns &&
			READ_ONCE(pid_ns->child_reaper) == current;
		int hit = atomic_inc_return(&vns_free_nsproxy_exit_hits);

		if (hit <= 8 || is_child_reaper)
			LKM4CTR_INFO(VENDOR_KERNEL_TAG,
				     "[DEBUG-KMI-EXIT] free_nsproxy exit #%d pid=%d comm=%s nsproxy=%p tracked=%d pid_ns=%p child_reaper=%d",
				     hit, task_pid_nr(current), current->comm, ns,
				     vns_nsproxy_set_contains(ns), pid_ns,
				     is_child_reaper);
	}
#endif
	if (current->flags & PF_EXITING)
		vns_task_exit_pidns_cleanup(current);

	if (vns_nsproxy_set_contains(ns)) {
		if (atomic_inc_return(&vns_free_nsproxy_intercepts) == 1)
			LKM4CTR_INFO(VENDOR_KERNEL_TAG,
				     "intercepted native free_nsproxy for vendored object %p",
				     ns);
		vns_free_nsproxy(ns);
		return;
	}

	if (vns_real_free_nsproxy_fn)
		vns_real_free_nsproxy_fn(ns);
}

static struct shadow_hook vns_free_nsproxy_shadow_hook =
	SHADOW_HOOK(vns_free_nsproxy_names,
		    vns_free_nsproxy_hook,
		    &vns_real_free_nsproxy_fn);

/*
 * [BUILD-COMPAT] Interpose on exit_task_namespaces(), not do_exit(). The
 * latter is __noreturn and was being probed directly with a pre_handler that
 * ran cleanup inside kprobe context; on an Android 6.1.138 stock image this
 * path panicked with an arm64 BRK #0x6 at do_exit+4 during task exit. The
 * namespace hook is a returning function, so the normal shadow-hook
 * kretprobe can release its rmmod-safety module reference after the wrapper
 * calls the original function. This also keeps cleanup immediately before
 * the kernel's native nsproxy teardown and child-reaper processing.
 */
static void (*vns_real_exit_task_namespaces)(struct task_struct *);

static const char * const vns_exit_task_namespaces_names[] = {
	"exit_task_namespaces", NULL,
};

/*
 * KMI kernels can assign a different CFI type hash to the same private
 * exit_task_namespaces() prototype. Call the target through a narrow __nocfi
 * wrapper while keeping the replacement hook itself CFI-instrumented.
 */
static void __nocfi vns_call_real_exit_task_namespaces(struct task_struct *tsk)
{
	vns_real_exit_task_namespaces(tsk);
}

static void vns_exit_task_namespaces_hook(struct task_struct *tsk)
{
	int hit = atomic_inc_return(&vns_exit_task_namespaces_intercepts);

	if (hit <= 8) {
		struct nsproxy *ns = READ_ONCE(tsk->nsproxy);
		struct pid_namespace *pid_ns = task_active_pid_ns(tsk);

		LKM4CTR_INFO(VENDOR_KERNEL_TAG,
			     "[DEBUG-KMI-EXIT] exit hook pre #%d pid=%d comm=%s tracked_nsproxy=%d pid_ns=%s child_reaper=%d",
			     hit, task_pid_nr(tsk), tsk->comm,
			     vns_nsproxy_set_contains(ns),
			     !pid_ns ? "NULL" :
			     pid_ns == &init_pid_ns ? "init" : "non-init",
			     pid_ns && pid_ns->child_reaper == tsk);
	}

	vns_task_exit_cleanup(tsk);

	if (hit <= 8) {
		struct nsproxy *ns = READ_ONCE(tsk->nsproxy);
		struct pid_namespace *pid_ns = task_active_pid_ns(tsk);

		LKM4CTR_INFO(VENDOR_KERNEL_TAG,
			     "[DEBUG-KMI-EXIT] exit hook post #%d pid=%d comm=%s tracked_nsproxy=%d pid_ns=%s child_reaper=%d",
			     hit, task_pid_nr(tsk), tsk->comm,
			     vns_nsproxy_set_contains(ns),
			     !pid_ns ? "NULL" :
			     pid_ns == &init_pid_ns ? "init" : "non-init",
			     pid_ns && pid_ns->child_reaper == tsk);
	}

	vns_call_real_exit_task_namespaces(tsk);
}

static struct shadow_hook vns_exit_task_namespaces_shadow_hook =
	SHADOW_HOOK(vns_exit_task_namespaces_names,
		    vns_exit_task_namespaces_hook,
		    &vns_real_exit_task_namespaces);

int vns_exit_hook_init(void) /* [BUILD-COMPAT] */
{
	int ret;

	/* Safe-unload quiesce must still admit task-exit cleanup so live
	 * vendored nsproxy references can drain before the module is removed.
	 */
	vns_free_nsproxy_shadow_hook.quiesce_exempt = true;
	vns_exit_task_namespaces_shadow_hook.quiesce_exempt = true;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0) && \
	LINUX_VERSION_CODE < KERNEL_VERSION(5, 16, 0)
	vns_exit_fs_shadow_hook.quiesce_exempt = true;
#endif

	ret = shadow_hook_install(&vns_free_nsproxy_shadow_hook);
	if (ret) {
		LKM4CTR_ERR(VENDOR_KERNEL_TAG,
			    "[DEBUG-KMI-EXIT] free_nsproxy hook install failed: %d",
			    ret);
		return ret;
	}

	ret = shadow_hook_install(&vns_exit_task_namespaces_shadow_hook);
	if (ret) {
		LKM4CTR_ERR(VENDOR_KERNEL_TAG,
			    "[DEBUG-KMI-EXIT] exit_task_namespaces hook install failed: %d",
			    ret);
		shadow_hook_remove(&vns_free_nsproxy_shadow_hook);
		return ret;
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0) && \
	LINUX_VERSION_CODE < KERNEL_VERSION(5, 16, 0)
	ret = shadow_hook_install(&vns_exit_fs_shadow_hook);
	if (ret) {
		LKM4CTR_ERR(VENDOR_KERNEL_TAG,
			    "[DEBUG-KMI-EXIT] exit_fs fallback hook install failed: %d",
			    ret);
		shadow_hook_remove(&vns_exit_task_namespaces_shadow_hook);
		shadow_hook_remove(&vns_free_nsproxy_shadow_hook);
		return ret;
	}
	LKM4CTR_INFO(VENDOR_KERNEL_TAG,
		     "[DEBUG-KMI-EXIT] free_nsproxy, exit_task_namespaces and 5.15 exit_fs hooks installed");
#else
	LKM4CTR_INFO(VENDOR_KERNEL_TAG,
		     "[DEBUG-KMI-EXIT] free_nsproxy and exit_task_namespaces hooks installed");
#endif

	return ret;
}

void vns_exit_hook_exit(void) /* [BUILD-COMPAT] */
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0) && \
	LINUX_VERSION_CODE < KERNEL_VERSION(5, 16, 0)
	shadow_hook_remove(&vns_exit_fs_shadow_hook);
#endif
	shadow_hook_remove(&vns_exit_task_namespaces_shadow_hook);
	shadow_hook_remove(&vns_free_nsproxy_shadow_hook);
}
