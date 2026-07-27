// SPDX-License-Identifier: GPL-2.0-only
/*
 * Vendored from kernel-common kernel/utsname.c (kernel version 6.1.124,
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
 *  Copyright (C) 2004 IBM Corporation
 *
 *  Author: Serge Hallyn <serue@us.ibm.com>
 */

#include <linux/export.h>
#include <linux/uts.h>
#include <linux/utsname.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/cred.h>
#include <linux/user_namespace.h>
#include <linux/proc_ns.h>
#include <linux/sched/task.h>
#include "../vendor_kernel.h"

/*
 * [BUILD-COMPAT] uts_sem is defined in kernel/sys.c (not exported to modules).
 * The vendor_kernel module maintains its own rwsemaphore protecting its separate
 * copy of UTS namespace data.
 */
DECLARE_RWSEM(uts_sem);


static struct ucounts *inc_uts_namespaces(struct user_namespace *ns)
{
	return inc_ucount(ns, current_euid(), UCOUNT_UTS_NAMESPACES);
}

static void dec_uts_namespaces(struct ucounts *ucounts)
{
	dec_ucount(ucounts, UCOUNT_UTS_NAMESPACES);
}

static struct uts_namespace *create_uts_ns(void)
{
	struct uts_namespace *uts_ns;

	/* [BUILD-COMPAT] allocate from vendor_kernel's own module-owned
	 * uts_ns_cache (created in vns_uts_ns_init()) instead of kzalloc,
	 * purely to mirror upstream's kmem_cache_alloc() semantics; see
	 * vendor_kernel.h's comment on vns_get_uts_ns()/vns_put_uts_ns() for
	 * why the real kernel's exit path never touches this object. */
	uts_ns = kmem_cache_alloc(vns_uts_ns_cache, GFP_KERNEL);
	if (uts_ns)
		vns_uts_init_ref(uts_ns);
	return uts_ns;
}

/*
 * Clone a new ns copying an original utsname, setting refcount to 1
 * @old_ns: namespace to clone
 * Return ERR_PTR(-ENOMEM) on error (failure to allocate), new ns otherwise
 */
static struct uts_namespace *clone_uts_ns(struct user_namespace *user_ns,
					  struct uts_namespace *old_ns)
{
	struct uts_namespace *ns;
	struct ucounts *ucounts;
	int err;

	err = -ENOSPC;
	ucounts = inc_uts_namespaces(user_ns);
	if (!ucounts)
		goto fail;

	err = -ENOMEM;
	ns = create_uts_ns();
	if (!ns)
		goto fail_dec;

	/* [BUILD-COMPAT] proc_alloc_inum is resolved indirectly for vendor_kernel. */
	err = vns_alloc_inum(&ns->ns);
	if (err)
		goto fail_free;

	ns->ucounts = ucounts;
	/* [RENAME] vendored proc-ns ops are namespaced to avoid kernel symbol collisions. */
	ns->ns.ops = &vns_utsns_operations;

	down_read(&uts_sem);
	memcpy(&ns->name, &old_ns->name, sizeof(ns->name));
	ns->user_ns = vns_get_user_ns(user_ns); /* [BUILD-COMPAT] */
	up_read(&uts_sem);
	return ns;

fail_free:
	kmem_cache_free(vns_uts_ns_cache, ns); /* [BUILD-COMPAT] */
fail_dec:
	dec_uts_namespaces(ucounts);
fail:
	return ERR_PTR(err);
}

/*
 * Copy task tsk's utsname namespace, or clone it if flags
 * specifies CLONE_NEWUTS.  In latter case, changes to the
 * utsname of this process won't be seen by parent, and vice
 * versa.
 */
struct uts_namespace *vns_copy_utsname( /* [RENAME] */
unsigned long flags,
	struct user_namespace *user_ns, struct uts_namespace *old_ns)
{
	struct uts_namespace *new_ns;

	BUG_ON(!old_ns);
	vns_get_uts_ns(old_ns); /* [BUILD-COMPAT] */

	if (!(flags & CLONE_NEWUTS))
		return old_ns;

	new_ns = clone_uts_ns(user_ns, old_ns);

	vns_put_uts_ns(old_ns); /* [BUILD-COMPAT] */
	return new_ns;
}

void vns_free_uts_ns(struct uts_namespace *ns) /* [RENAME] */
{
	dec_uts_namespaces(ns->ucounts);
	vns_put_user_ns(ns->user_ns); /* [BUILD-COMPAT] */
	vns_free_inum(&ns->ns); /* [BUILD-COMPAT] */
	kmem_cache_free(vns_uts_ns_cache, ns); /* [BUILD-COMPAT] */
}

/*
 * [BUILD-COMPAT] vns_get_uts_ns()/vns_put_uts_ns() -- see the declaration
 * comment in vendor_kernel.h ("Namespace refcounting is fully
 * self-contained").
 */
struct uts_namespace *vns_get_uts_ns(struct uts_namespace *ns) /* [BUILD-COMPAT] */
{
	vns_uts_get_ref(ns);
	return ns;
}

void vns_put_uts_ns(struct uts_namespace *ns) /* [BUILD-COMPAT] */
{
	if (vns_uts_put_ref(ns))
		vns_free_uts_ns(ns);
}

static inline struct uts_namespace *to_uts_ns(struct ns_common *ns)
{
	return container_of(ns, struct uts_namespace, ns);
}

static struct ns_common *utsns_get(struct task_struct *task)
{
	struct uts_namespace *ns = NULL;
	struct nsproxy *nsproxy;

	task_lock(task);
	nsproxy = task->nsproxy;
	if (nsproxy) {
		ns = nsproxy->uts_ns;
		vns_get_uts_ns(ns); /* [BUILD-COMPAT] */
	}
	task_unlock(task);

	return ns ? &ns->ns : NULL;
}

static void utsns_put(struct ns_common *ns)
{
	vns_put_uts_ns(to_uts_ns(ns)); /* [BUILD-COMPAT] */
}

static int utsns_install(struct nsset *nsset, struct ns_common *new)
{
	struct nsproxy *nsproxy = nsset->nsproxy;
	struct uts_namespace *ns = to_uts_ns(new);

	if (!ns_capable(ns->user_ns, CAP_SYS_ADMIN) ||
	    !ns_capable(nsset->cred->user_ns, CAP_SYS_ADMIN))
		return -EPERM;

	vns_get_uts_ns(ns); /* [BUILD-COMPAT] */
	vns_put_uts_ns(nsproxy->uts_ns); /* [BUILD-COMPAT] */
	nsproxy->uts_ns = ns;
	return 0;
}

static struct user_namespace *utsns_owner(struct ns_common *ns)
{
	return to_uts_ns(ns)->user_ns;
}

const struct proc_ns_operations vns_utsns_operations = { /* [RENAME] */
	.name		= "uts",
	.type		= CLONE_NEWUTS,
	.get		= utsns_get,
	.put		= utsns_put,
	.install	= utsns_install,
	.owner		= utsns_owner,
};

void vns_uts_ns_init(void) /* [RENAME] */
{
	/* [BUILD-COMPAT] module-owned cache: see vendor_kernel.h's comment on
	 * vns_get_uts_ns()/vns_put_uts_ns() and vendor_kernel/README.md's
	 * "Slab-cache consistency with the real kernel" for why this no
	 * longer resolves the real kernel's private uts_ns_cache. */
	if (!vns_uts_ns_cache)
		vns_uts_ns_cache = kmem_cache_create("vns_uts_namespace",
			sizeof(struct uts_namespace), 0, SLAB_ACCOUNT, NULL);
}
