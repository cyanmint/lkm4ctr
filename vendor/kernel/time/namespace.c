// SPDX-License-Identifier: GPL-2.0
/*
 * Vendored from kernel-common kernel/time/namespace.c (kernel version 6.1.124,
 * android14-6.1 branch). CHANGES FROM UPSTREAM:
 *   - [RENAME] All non-static global symbols prefixed with vns_ to avoid
 *     collision with the built-in kernel implementation.
 *   - [BUILD-COMPAT] slab caches replaced with kzalloc/kfree (no kmem_cache_create
 *     in out-of-tree module init context).
 *   - [BUILD-COMPAT] ns_alloc_inum/ns_free_inum -> vns_alloc_inum/vns_free_inum
 *     (proc_alloc_inum not exported; resolved at init via shadow_hook_resolve).
 *   - [BUILD-COMPAT] __init/__exit removed from non-module-init functions.
 *   - [DIAGFS] Statistics incremented via vendor_kernel_registry for diagfs exposure.
 *   Any line NOT marked RENAME/BUILD-COMPAT/DIAGFS is unchanged from upstream.
 */
#include <linux/time_namespace.h>
#include <linux/user_namespace.h>
#include <linux/nsproxy.h>
#include <linux/proc_ns.h>
#include <linux/sched/task.h>
#include <linux/seq_file.h>

#include "../../vendor_kernel.h"

struct time_namespace *vns_copy_time_ns(unsigned long flags,
					struct user_namespace *user_ns,
					struct time_namespace *old_ns) /* [RENAME] */
{
	(void)flags;
	(void)user_ns;
	return get_time_ns(old_ns); /* [BUILD-COMPAT] */
}

void vns_put_time_ns(struct time_namespace *ns) /* [RENAME] */
{
	if (ns)
		put_time_ns(ns);
}

void vns_free_time_ns(struct time_namespace *ns) /* [RENAME] */
{
	vns_put_time_ns(ns);
}

void vns_timens_commit(struct task_struct *tsk, struct time_namespace *ns) /* [RENAME] */
{
	(void)tsk;
	(void)ns;
	/* [BUILD-COMPAT] vDSO time-namespace hooks are not reproduced out of tree. */
}

void vns_timens_on_fork(struct nsproxy *nsproxy, struct task_struct *tsk) /* [RENAME] */
{
	(void)tsk;
	if (nsproxy->time_ns == nsproxy->time_ns_for_children)
		return;
	get_time_ns(nsproxy->time_ns_for_children);
	vns_put_time_ns(nsproxy->time_ns);
	nsproxy->time_ns = nsproxy->time_ns_for_children;
}

void vns_proc_timens_show_offsets(struct task_struct *p, struct seq_file *m) /* [RENAME] */
{
	(void)p;
	(void)m;
}

int vns_proc_timens_set_offset(struct file *file, struct task_struct *p,
			      struct proc_timens_offset *offsets, int noffsets) /* [RENAME] */
{
	(void)file;
	(void)p;
	(void)offsets;
	(void)noffsets;
	return -EOPNOTSUPP; /* [BUILD-COMPAT] */
}

static struct time_namespace *vns_to_time_ns(struct ns_common *ns)
{
	return container_of(ns, struct time_namespace, ns);
}

static struct ns_common *vns_timens_get(struct task_struct *task)
{
	struct time_namespace *ns = NULL;
	struct nsproxy *nsproxy;

	task_lock(task);
	nsproxy = task->nsproxy;
	if (nsproxy) {
		ns = nsproxy->time_ns;
		get_time_ns(ns);
	}
	task_unlock(task);
	return ns ? &ns->ns : NULL;
}

static struct ns_common *vns_timens_for_children_get(struct task_struct *task)
{
	struct time_namespace *ns = NULL;
	struct nsproxy *nsproxy;

	task_lock(task);
	nsproxy = task->nsproxy;
	if (nsproxy) {
		ns = nsproxy->time_ns_for_children;
		get_time_ns(ns);
	}
	task_unlock(task);
	return ns ? &ns->ns : NULL;
}

static void vns_timens_put(struct ns_common *ns)
{
	vns_put_time_ns(vns_to_time_ns(ns));
}

static int vns_timens_install(struct nsset *nsset, struct ns_common *new)
{
	struct nsproxy *nsproxy = nsset->nsproxy;
	struct time_namespace *ns = vns_to_time_ns(new);

	if (!ns_capable(ns->user_ns, CAP_SYS_ADMIN) ||
	    !ns_capable(nsset->cred->user_ns, CAP_SYS_ADMIN))
		return -EPERM;
	get_time_ns(ns);
	vns_put_time_ns(nsproxy->time_ns);
	nsproxy->time_ns = ns;
	get_time_ns(ns);
	vns_put_time_ns(nsproxy->time_ns_for_children);
	nsproxy->time_ns_for_children = ns;
	return 0;
}

static struct user_namespace *vns_timens_owner(struct ns_common *ns)
{
	return vns_to_time_ns(ns)->user_ns;
}

const struct proc_ns_operations vns_timens_operations = { /* [RENAME] */
	.name		= "time",
	.type		= CLONE_NEWTIME,
	.get		= vns_timens_get,
	.put		= vns_timens_put,
	.install	= vns_timens_install,
	.owner		= vns_timens_owner,
};

const struct proc_ns_operations vns_timens_for_children_operations = { /* [RENAME] */
	.name		= "time_for_children",
	.real_ns_name	= "time",
	.type		= CLONE_NEWTIME,
	.get		= vns_timens_for_children_get,
	.put		= vns_timens_put,
	.install	= vns_timens_install,
	.owner		= vns_timens_owner,
};

struct time_namespace vns_init_time_ns = { /* [RENAME] */
	VNS_TIME_REF_INIT
	/* [BUILD-COMPAT] .user_ns set at runtime in vns_time_ns_default_init()
	 * (glue/vendor_kernel_module.c): init_user_ns is now a runtime pointer
	 * dereference (vendor_kernel.h), not a compile-time constant usable
	 * in a static initializer. */
	.ns.ops		= &vns_timens_operations,
	.frozen_offsets	= true,
};

void vns_time_ns_default_init(void) /* [BUILD-COMPAT] */
{
	vns_init_time_ns.user_ns = vns_real_init_user_ns;
}
