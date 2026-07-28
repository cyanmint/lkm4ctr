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
 * inline helpers (e.g. current_user_ns() on a CONFIG_USER_NS=n target) that
 * reference the bare init_user_ns name directly -- see
 * ../../glue/vendor_kernel_data_syms.h for the full rationale. Missing this
 * left vns_unshare_nsproxy_namespaces()/vns_sys_setns()'s current_user_ns()
 * calls referencing the real, unexported init_user_ns symbol ("Unknown
 * symbol init_user_ns" observed at insmod on android12-5.10).
 */
#include "../../glue/vendor_kernel_data_syms.h"
#include <linux/slab.h>
#include <linux/export.h>
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
#include <linux/spinlock.h>
#include <linux/kprobes.h>
#include <linux/llist.h>
#include <linux/workqueue.h>
#include "../vendor_kernel.h"
#include "../include/uapi/vendor_kernel.h"
#include "../../glue/lkm4ctr_log.h"

/* [BUILD-COMPAT] Forward declaration for vendored time_namespace init object. */
extern struct time_namespace vns_init_time_ns;

/*
 * [BUILD-COMPAT] vns_nsproxy_set tracks every module-owned struct nsproxy *
 * currently installed on some task_struct->nsproxy. It exists purely so
 * that vns_task_exit_cleanup() (invoked from the do_exit() shadow_hook in
 * glue/vendor_kernel_syscalls.c) can tell, for an arbitrary exiting task,
 * whether that task's nsproxy is one of vendor_kernel's own vendored
 * objects (allocated from vns_nsproxy_cachep) as opposed to the real
 * kernel's init_nsproxy/&vns_init_nsproxy singleton -- without needing any
 * unexported mm-internal helper such as virt_to_cache(). Membership is a
 * pointer identity check only; the hash table is a small fixed-size
 * spinlock-protected chain, sized generously since a device is expected to
 * have at most a handful of concurrently-live vendored nsproxy objects.
 */
static DEFINE_HASHTABLE(vns_nsproxy_set, 6);
static DEFINE_SPINLOCK(vns_nsproxy_set_lock);

struct vns_nsproxy_set_entry {
	struct nsproxy *ns;
	struct hlist_node node;
};

static void vns_nsproxy_set_add(struct nsproxy *ns)
{
	struct vns_nsproxy_set_entry *entry;
	unsigned long flags;

	entry = kmalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry) {
		/*
		 * Allocation failure here just means vns_task_exit_cleanup()
		 * will fail to recognize this object later and the real
		 * kernel's exit path may attempt to free it against its own
		 * cache. This is exceedingly unlikely (a tiny fixed-size
		 * allocation) and there is no safe way to fail create_nsproxy()
		 * at this point without unwinding a fully-populated object, so
		 * we log and continue rather than leak the whole nsproxy.
		 */
		LKM4CTR_WARN(VENDOR_KERNEL_TAG, "vns_nsproxy_set_add: kmalloc failed, exit-safety tracking degraded for %p", ns);
		return;
	}
	entry->ns = ns;

	spin_lock_irqsave(&vns_nsproxy_set_lock, flags);
	hash_add(vns_nsproxy_set, &entry->node, (unsigned long)ns);
	spin_unlock_irqrestore(&vns_nsproxy_set_lock, flags);
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
			found = true;
			break;
		}
	}
	spin_unlock_irqrestore(&vns_nsproxy_set_lock, flags);

	if (found)
		kfree(entry);
	return found;
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

	/* [BUILD-COMPAT] allocate from vendor_kernel's own module-owned
	 * nsproxy cache (created in vns_nsproxy_cache_init()) instead of the
	 * real kernel's private nsproxy_cachep. See vns_task_exit_cleanup()
	 * below for how the real kernel's own exit path is kept from ever
	 * touching this module-owned object. */
	nsproxy = kmem_cache_alloc(vns_nsproxy_cachep, GFP_KERNEL);
	if (nsproxy) {
		vns_init_count(&nsproxy->count, 1); /* [BUILD-COMPAT] */
		vns_nsproxy_set_add(nsproxy); /* [BUILD-COMPAT] */
	}
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
 * including vns_exit_kprobe_pre_handler(), a real-kernel-invoked kprobe
 * pre_handler callback -- CFI-instrumented and a valid indirect-call target
 * for the real kernel, matching the pattern used elsewhere in this module
 * (see e.g. vendor_kernel_procfs_userns.c's vns_idmap_create_fd()).
 */
static __nocfi struct nsproxy *create_new_namespaces(unsigned long flags,
	struct task_struct *tsk, struct user_namespace *user_ns,
	struct fs_struct *new_fs)
{
	struct nsproxy *new_nsp;
	int err;

	new_nsp = create_nsproxy();
	if (!new_nsp)
		return ERR_PTR(-ENOMEM);

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
	kmem_cache_free(vns_nsproxy_cachep, new_nsp); /* [BUILD-COMPAT] */
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
	vns_nsproxy_set_remove(ns); /* [BUILD-COMPAT] */
	kmem_cache_free(vns_nsproxy_cachep, ns); /* [BUILD-COMPAT] */
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

	user_ns = new_cred ? new_cred->user_ns : current_user_ns();
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

	nsset->nsproxy = create_new_namespaces(0, me, current_user_ns(), me->fs);
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

#ifdef CONFIG_UTS_NS
	if (flags & CLONE_NEWUTS) {
		ret = validate_ns(nsset, &nsp->uts_ns->ns);
		if (ret)
			goto out;
	}
#endif

#ifdef CONFIG_IPC_NS
	if (flags & CLONE_NEWIPC) {
		ret = validate_ns(nsset, &nsp->ipc_ns->ns);
		if (ret)
			goto out;
	}
#endif

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

#ifdef CONFIG_IPC_NS
	if (flags & CLONE_NEWIPC)
		exit_sem(me);
#endif

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
		if (flags && (ns->ops->type != flags))
			err = -EINVAL;
		flags = ns->ops->type;
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
 * [BUILD-COMPAT] vns_task_exit_cleanup() runs from a plain pre_handler-only
 * kprobe on do_exit() (vns_exit_kprobe_pre_handler() below), and kprobe
 * handlers execute in atomic context (preemption disabled) regardless of
 * the underlying int3/brk trap mechanism -- see Documentation/trace/kprobes.rst:
 * "Probes are run with preemption disabled ... you must not do anything
 * that could cause a sleep". vns_put_nsproxy()/vns_free_nsproxy() can reach
 * put_mnt_ns() -> namespace_unlock() -> synchronize_rcu_expedited(), which
 * schedules -- calling that path directly from the kprobe pre_handler
 * triggers "BUG: scheduling while atomic". So the actual teardown of a
 * vendored nsproxy is deferred to process context via a workqueue; only the
 * task_lock()-protected pointer swap (fully atomic-safe: spinlock and plain
 * refcounting) happens inline in the kprobe handler itself.
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
 * vns_task_exit_cleanup() - see the declaration comment in vendor_kernel.h.
 * Called from the do_exit() shadow_hook (glue/vendor_kernel_syscalls.c)
 * for every exiting task, strictly before the real do_exit() body (and
 * therefore the real exit_task_namespaces()/free_nsproxy()) runs.
 */
void vns_task_exit_cleanup(struct task_struct *tsk) /* [BUILD-COMPAT] */
{
	struct nsproxy *ns;

	if (!vns_pidns_runtime_supported) {
		struct pid_namespace *pid_ns = task_active_pid_ns(tsk);

		/*
		 * [BUILD-COMPAT] task_active_pid_ns() is derived from
		 * tsk->thread_pid, fixed at fork time, and is untouched by
		 * the tsk->nsproxy swap further down. If tsk is the last
		 * live thread of one of vendor_kernel's own module-owned pid
		 * namespaces (kernel/pid_namespace.c:vns_copy_pid_ns()) and
		 * also that namespace's pid 1, the real kernel's own
		 * copy_process() (kernel/fork.c, unconditional of
		 * CONFIG_PID_NS) has already set pid_ns->child_reaper == tsk.
		 * Left untouched, the real
		 * do_exit()->forget_original_parent()->find_child_reaper()
		 * path (kernel/exit.c) is about to see that and call this
		 * target kernel's own zap_pid_ns_processes(), an
		 * unconditional BUG() stub on a CONFIG_PID_NS=n build
		 * (include/linux/pid_namespace.h). Run the safe half of that
		 * cascade ourselves right now and then hand the namespace off
		 * to the real init task as its child_reaper, so the real
		 * find_child_reaper() takes its "reaper != father" fast path
		 * moments later in this same do_exit() call instead of ever
		 * reaching the broken stub. See vns_zap_pid_ns_processes()
		 * (kernel/pid_namespace.c) for the reduced-scope cascade
		 * this runs.
		 */
		if (pid_ns && pid_ns != &init_pid_ns &&
		    pid_ns->child_reaper == tsk) {
			vns_zap_pid_ns_processes(pid_ns);
			pid_ns->child_reaper = init_pid_ns.child_reaper;
		}
	}

#if !defined(CONFIG_SYSVIPC)
	vns_prepare_exit_sem(tsk);
	vns_prepare_exit_shm(tsk);
#endif

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
	 * From here on, the real kernel's own exit_task_namespaces() will
	 * only ever see &vns_init_nsproxy (pinned, never slab-allocated) on
	 * this task, and will never call kmem_cache_free() against a
	 * module-owned object. Tear the vendored object down through
	 * vendor_kernel's own self-contained free path, deferred to process
	 * context (see the atomic-context comment above this function).
	 */
	vns_nsproxy_put_deferred(ns);
}

/*
 * [BUILD-COMPAT] Exit-safety kprobe: a plain pre_handler-only kprobe on
 * do_exit(), NOT one of the redirecting shadow_hook entries used elsewhere
 * in vendor_kernel. do_exit() is __noreturn, so hooking it via the
 * SHADOW_HOOK()/shadow_hijack redirect mechanism (which relies on a
 * kretprobe firing on the *replacement* function's return to release the
 * rmmod-safety module reference -- see glue/shadow_hook.h's "rmmod
 * safety" note) would never release that reference, permanently pinning
 * module_refcount() above zero after the very first process exit on the
 * whole system. A pre_handler-only kprobe has no such problem: it runs
 * vns_task_exit_cleanup() and returns normally, letting the original
 * do_exit() instruction execute completely untouched immediately
 * afterwards, and register_kprobe()/unregister_kprobe() are already
 * synchronously safe to install/remove (the same primitive
 * shadow_hook_resolve() itself relies on for one-shot symbol resolution).
 */
static int vns_exit_kprobe_pre_handler(struct kprobe *p, struct pt_regs *regs)
{
	vns_task_exit_cleanup(current);
	return 0;
}

static struct kprobe vns_exit_kprobe = {
	.symbol_name	= "do_exit",
	.pre_handler	= vns_exit_kprobe_pre_handler,
};
static bool vns_exit_kprobe_installed;

int vns_exit_hook_init(void) /* [BUILD-COMPAT] */
{
	int ret;

	if (vns_exit_kprobe_installed)
		return 0;

	ret = register_kprobe(&vns_exit_kprobe);
	if (ret) {
		LKM4CTR_WARN(VENDOR_KERNEL_TAG,
			"do_exit exit-safety kprobe registration failed (%d)", ret);
		return ret;
	}
	vns_exit_kprobe_installed = true;
	return 0;
}

void vns_exit_hook_exit(void) /* [BUILD-COMPAT] */
{
	if (vns_exit_kprobe_installed) {
		unregister_kprobe(&vns_exit_kprobe);
		vns_exit_kprobe_installed = false;
	}
}
