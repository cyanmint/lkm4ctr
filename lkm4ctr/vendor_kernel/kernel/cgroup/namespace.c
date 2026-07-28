// SPDX-License-Identifier: GPL-2.0
/*
 * Vendored from kernel-common kernel/cgroup/namespace.c (kernel version 6.1.124,
 * android14-6.1 branch). CHANGES FROM UPSTREAM:
 *   - [RENAME] All non-static global symbols prefixed with vns_ to avoid
 *     collision with the built-in kernel implementation.
 *   - [BUILD-COMPAT] slab caches replaced with kzalloc/kfree (no kmem_cache_create
 *     in out-of-tree module init context).
 *   - [BUILD-COMPAT] ns_alloc_inum/ns_free_inum -> vns_alloc_inum/vns_free_inum
 *     (proc_alloc_inum not exported; resolved at init via shadow_hook_resolve).
 *   - [BUILD-COMPAT] __init/__exit removed from non-module-init functions.
 *   - [DIAGFS] Statistics incremented via vendor_kernel_registry for diagfs exposure.
 *   - [BUILD-COMPAT] vns_default_cgroup_ns / vns_cgroup_default_init() added
 *     (new, not upstream): a module-owned default cgroup_namespace singleton
 *     used unconditionally as vns_init_nsproxy.cgroup_ns instead of resolving
 *     and pointing at the running kernel's own init_cgroup_ns, mirroring the
 *     module-owned default already used for ipc_namespace
 *     (vns_default_ipc_ns, ipc/msgutil.c) and time_namespace
 *     (vns_init_time_ns, kernel/time/namespace.c).
 *   Any other line NOT marked RENAME/BUILD-COMPAT/DIAGFS is unchanged from upstream.
 */
#include <linux/cgroup.h>
#include <linux/nsproxy.h>
#include <linux/proc_ns.h>
#include <linux/sched/task.h>

#include "../../vendor_kernel.h"

struct cgroup_namespace *vns_copy_cgroup_ns(unsigned long flags,
					struct user_namespace *user_ns,
					struct cgroup_namespace *old_ns) /* [RENAME] */
{
	(void)flags;
	(void)user_ns;
	get_cgroup_ns(old_ns); /* [BUILD-COMPAT] */
	return old_ns;
}

void vns_put_cgroup_ns(struct cgroup_namespace *ns) /* [RENAME] */
{
	if (ns)
		put_cgroup_ns(ns);
}

void vns_free_cgroup_ns(struct cgroup_namespace *ns) /* [RENAME] */
{
	vns_put_cgroup_ns(ns);
}

static struct cgroup_namespace *to_cg_ns(struct ns_common *ns)
{
	return container_of(ns, struct cgroup_namespace, ns);
}

static int vns_cgroupns_install(struct nsset *nsset, struct ns_common *ns)
{
	struct nsproxy *nsproxy = nsset->nsproxy;
	struct cgroup_namespace *cgroup_ns = to_cg_ns(ns);

	if (!ns_capable(nsset->cred->user_ns, CAP_SYS_ADMIN) ||
	    !ns_capable(cgroup_ns->user_ns, CAP_SYS_ADMIN))
		return -EPERM;
	get_cgroup_ns(cgroup_ns);
	vns_put_cgroup_ns(nsproxy->cgroup_ns);
	nsproxy->cgroup_ns = cgroup_ns;
	return 0;
}

static struct ns_common *vns_cgroupns_get(struct task_struct *task)
{
	struct cgroup_namespace *ns = NULL;
	struct nsproxy *nsproxy;

	task_lock(task);
	nsproxy = task->nsproxy;
	if (nsproxy) {
		ns = nsproxy->cgroup_ns;
		get_cgroup_ns(ns);
	}
	task_unlock(task);
	return ns ? &ns->ns : NULL;
}

static void vns_cgroupns_put(struct ns_common *ns)
{
	vns_put_cgroup_ns(to_cg_ns(ns));
}

static struct user_namespace *vns_cgroupns_owner(struct ns_common *ns)
{
	return to_cg_ns(ns)->user_ns;
}

const struct proc_ns_operations vns_cgroupns_operations = { /* [RENAME] */
	.name		= "cgroup",
	.type		= CLONE_NEWCGROUP,
	.get		= vns_cgroupns_get,
	.put		= vns_cgroupns_put,
	.install	= vns_cgroupns_install,
	.owner		= vns_cgroupns_owner,
};

/*
 * [BUILD-COMPAT] vns_default_cgroup_ns (new, not upstream) is vendor_kernel's
 * own module-owned default cgroup_namespace, used unconditionally as
 * vns_init_nsproxy.cgroup_ns (see glue/vendor_kernel_module.c) instead of
 * resolving and pointing at the running kernel's real, non-exported
 * init_cgroup_ns -- mirroring the module-owned default already used for
 * ipc_namespace (vns_default_ipc_ns) and time_namespace (vns_init_time_ns).
 * root_cset is deliberately left NULL: vns_copy_cgroup_ns() never actually
 * builds a brand-new cgroup_namespace (CLONE_NEWCGROUP is bookkeeping-only,
 * same as CLONE_NEWNET/CLONE_NEWNS -- see vendor_kernel/README.md, "Known
 * remaining gaps"), so root_cset is never dereferenced by any vendored code
 * path; a real per-namespace root_cset would require duplicating the
 * running kernel's non-exported cgroup core (css_set table, cgroup_mutex),
 * which is out of scope here.
 */
struct cgroup_namespace vns_default_cgroup_ns = {
	.ns.ops		= &vns_cgroupns_operations,
	/* [BUILD-COMPAT] .user_ns set at runtime in vns_cgroup_default_init():
	 * init_user_ns is now a runtime pointer dereference (vendor_kernel.h),
	 * not a compile-time constant usable in a static initializer. */
};

void vns_cgroup_default_init(void) /* [BUILD-COMPAT] */
{
	vns_default_cgroup_ns.user_ns = vns_real_init_user_ns;

	/*
	 * Pin vns_default_cgroup_ns's refcount to a large sentinel value so
	 * it can never legitimately reach zero and be mistaken for a
	 * freeable object -- it is a static singleton, never slab-allocated,
	 * shared by every task that never unshare(CLONE_NEWCGROUP)'d (same
	 * pattern as vns_init_nsproxy.count, kernel/nsproxy.c).
	 */
	vns_cgroupns_init_ref(&vns_default_cgroup_ns, 0x40000000);
}
