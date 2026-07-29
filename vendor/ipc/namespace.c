// SPDX-License-Identifier: GPL-2.0
/*
 * Vendored from kernel-common ipc/namespace.c (kernel version 6.1.124,
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
/*
 * linux/ipc/namespace.c
 * Copyright (C) 2006 Pavel Emelyanov <xemul@openvz.org> OpenVZ, SWsoft Inc.
 */

#include <linux/ipc.h>
#include <linux/msg.h>
#include <linux/ipc_namespace.h>
#include <linux/rcupdate.h>
#include <linux/nsproxy.h>
#include <linux/slab.h>
#include <linux/cred.h>
#include <linux/fs.h>
#include <linux/mount.h>
#include <linux/user_namespace.h>
#include <linux/proc_ns.h>
#include <linux/sched/task.h>

#include "util.h"
#include "../vendor_kernel.h"

static struct ucounts *inc_ipc_namespaces(struct user_namespace *ns)
{
	return inc_ucount(ns, current_euid(), UCOUNT_IPC_NAMESPACES);
}

static void dec_ipc_namespaces(struct ucounts *ucounts)
{
	dec_ucount(ucounts, UCOUNT_IPC_NAMESPACES);
}

static struct ipc_namespace *create_ipc_ns(struct user_namespace *user_ns,
					   struct ipc_namespace *old_ns)
{
	struct ipc_namespace *ns;
	struct ucounts *ucounts;
	int err;

	err = -ENOSPC;
	ucounts = inc_ipc_namespaces(user_ns);
	if (!ucounts)
		goto fail;

	err = -ENOMEM;
	ns = kzalloc(sizeof(struct ipc_namespace), GFP_KERNEL_ACCOUNT);
	if (ns == NULL)
		goto fail_dec;

	/* [BUILD-COMPAT] proc_alloc_inum is resolved lazily in vendor_kernel. */
	err = vns_alloc_inum(&ns->ns);
	if (err)
		goto fail_free;
	/* [RENAME] vendored proc-ns ops are prefixed. */
	ns->ns.ops = &vns_ipcns_operations;

	vns_ipc_init_ref(ns);
	ns->user_ns = vns_get_user_ns(user_ns); /* [BUILD-COMPAT] */
	ns->ucounts = ucounts;

	err = mq_init_ns(ns);
	if (err)
		goto fail_put;

	err = -ENOMEM;
	if (!setup_mq_sysctls(ns))
		goto fail_put;

	if (!setup_ipc_sysctls(ns))
		goto fail_mq;

	err = msg_init_ns(ns);
	if (err)
		goto fail_ipc;

	sem_init_ns(ns);
	shm_init_ns(ns);

	return ns;

fail_ipc:
	retire_ipc_sysctls(ns);
fail_mq:
	retire_mq_sysctls(ns);

fail_put:
	vns_put_user_ns(ns->user_ns); /* [BUILD-COMPAT] */
	vns_free_inum(&ns->ns); /* [BUILD-COMPAT] */
fail_free:
	kfree(ns);
fail_dec:
	dec_ipc_namespaces(ucounts);
fail:
	return ERR_PTR(err);
}

struct ipc_namespace *vns_copy_ipcs( /* [RENAME] */
unsigned long flags,
	struct user_namespace *user_ns, struct ipc_namespace *ns)
{
	/*
	 * [BUILD-COMPAT] @ns is task->nsproxy->ipc_ns of the task this is
	 * copying from (create_new_namespaces()'s tsk->nsproxy->ipc_ns,
	 * kernel/nsproxy.c). On upstream this is never NULL: a real
	 * CONFIG_IPC_NS=y kernel's init_nsproxy.ipc_ns is always
	 * &init_ipc_ns. On vendor_kernel's actual target
	 * (CONFIG_SYSVIPC=n && CONFIG_POSIX_MQUEUE=n), the real kernel's own
	 * init_nsproxy.ipc_ns is compiled out entirely and stays NULL
	 * (kernel/nsproxy.c's init_nsproxy initializer), so any task that
	 * inherited that real nsproxy verbatim -- i.e. never yet had its
	 * task->nsproxy replaced by vendor_kernel's own
	 * create_new_namespaces()/switch_task_namespaces() path -- still has
	 * a literal NULL ipc_ns here. Passing that straight to get_ipc_ns()
	 * below (CLONE_NEWIPC not requested) would silently propagate NULL
	 * into the freshly built nsproxy, which every real-kernel-invoked
	 * proc_ns_operations callback below (ipcns_install() in particular)
	 * unconditionally dereferences -- CI observed a live NULL-pointer
	 * Oops in vns_put_ipc_ns() called from ipcns_install() on `docker
	 * exec` (runc's nsexec setns(2)ing into the container's ipc
	 * namespace from a task whose own nsproxy->ipc_ns was still this
	 * NULL). Substitute vendor_kernel's own module-owned default ipc
	 * namespace (vns_ipc_active_default(), glue/vendor_kernel_ipc_syscalls.c)
	 * instead, exactly like vns_task_ipc_ns() already falls back to for
	 * syscall lookups, so every nsproxy built from here on always
	 * carries a valid, non-NULL ipc_ns.
	 */
	if (!ns)
		ns = vns_ipc_active_default();
	if (!(flags & CLONE_NEWIPC))
		return get_ipc_ns(ns);
	return create_ipc_ns(user_ns, ns);
}

/*
 * free_ipcs - free all ipcs of one type
 * @ns:   the namespace to remove the ipcs from
 * @ids:  the table of ipcs to free
 * @free: the function called to free each individual ipc
 *
 * Called for each kind of ipc when an ipc_namespace exits.
 */
void vns_free_ipcs(struct ipc_namespace *ns, struct ipc_ids *ids, /* [RENAME] */
	       void (*free)(struct ipc_namespace *, struct kern_ipc_perm *))
{
	struct kern_ipc_perm *perm;
	int next_id;
	int total, in_use;

	down_write(&ids->rwsem);

	in_use = ids->in_use;

	for (total = 0, next_id = 0; total < in_use; next_id++) {
		perm = idr_find(&ids->ipcs_idr, next_id);
		if (perm == NULL)
			continue;
		rcu_read_lock();
		ipc_lock_object(perm);
		free(ns, perm);
		total++;
	}
	up_write(&ids->rwsem);
}

static void vns_free_ipc_ns(struct ipc_namespace *ns) /* [RENAME] */
{
	/* mq_put_mnt() waits for a grace period as kern_unmount()
	 * uses synchronize_rcu().
	 */
	mq_put_mnt(ns);
	/*
	 * sem_exit_ns()/msg_exit_ns()/shm_exit_ns() are not exported by the
	 * real kernel, but vendor_kernel vendors its own copies of them
	 * (ipc/sem.c, ipc/msg.c, ipc/shm.c, renamed vns_sem_exit_ns/
	 * vns_msg_exit_ns/vns_shm_exit_ns via vendor_kernel.h's #define and
	 * linked into the same lkm4ctr.ko), so they can and must be called
	 * directly here exactly like the real free_ipc_ns() does. Skipping
	 * them (as a previous version of this function did) leaks every
	 * queue/array/segment still registered in @ns, and -- critically --
	 * never calls percpu_counter_destroy() on msg_exit_ns()'s own
	 * ns->percpu_msg_bytes/percpu_msg_hdrs (see ipc/msg.c's
	 * vns_msg_accounting_destroy()), so kfree(ns) below frees memory
	 * that is still linked into the kernel-wide percpu_counters list,
	 * corrupting it for any *unrelated* later percpu_counter_init() call
	 * (observed as "list_add corruption ... kernel BUG at
	 * lib/list_debug.c:29" from an entirely unrelated real cgroup mkdir
	 * -> mem_cgroup_css_alloc -> wb_domain_init -> fprop_global_init ->
	 * __percpu_counter_init call site).
	 */
	sem_exit_ns(ns);
	msg_exit_ns(ns);
	shm_exit_ns(ns);

	retire_mq_sysctls(ns);
	retire_ipc_sysctls(ns);

	dec_ipc_namespaces(ns->ucounts);
	vns_put_user_ns(ns->user_ns); /* [BUILD-COMPAT] */
	vns_free_inum(&ns->ns); /* [BUILD-COMPAT] */
	kfree(ns);
}

static LLIST_HEAD(free_ipc_list);
static void free_ipc(struct work_struct *unused)
{
	struct llist_node *node = llist_del_all(&free_ipc_list);
	struct ipc_namespace *n, *t;

	llist_for_each_entry_safe(n, t, node, mnt_llist)
		vns_free_ipc_ns(n);
}

/*
 * The work queue is used to avoid the cost of synchronize_rcu in kern_unmount.
 */
static DECLARE_WORK(free_ipc_work, free_ipc);

/*
 * put_ipc_ns - drop a reference to an ipc namespace.
 * @ns: the namespace to put
 *
 * If this is the last task in the namespace exiting, and
 * it is dropping the refcount to 0, then it can race with
 * a task in another ipc namespace but in a mounts namespace
 * which has this ipcns's mqueuefs mounted, doing some action
 * with one of the mqueuefs files.  That can raise the refcount.
 * So dropping the refcount, and raising the refcount when
 * accessing it through the VFS, are protected with mq_lock.
 *
 * (Clearly, a task raising the refcount on its own ipc_ns
 * needn't take mq_lock since it can't race with the last task
 * in the ipcns exiting).
 */
void vns_put_ipc_ns(struct ipc_namespace *ns) /* [RENAME] */
{
	if (vns_ipc_put_ref_lock(ns, &mq_lock)) {
		mq_clear_sbinfo(ns);
		spin_unlock(&mq_lock);

		if (llist_add(&ns->mnt_llist, &free_ipc_list))
			schedule_work(&free_ipc_work);
	}
}

static inline struct ipc_namespace *to_ipc_ns(struct ns_common *ns)
{
	return container_of(ns, struct ipc_namespace, ns);
}

static struct ns_common *ipcns_get(struct task_struct *task)
{
	struct ipc_namespace *ns = NULL;
	struct nsproxy *nsproxy;

	task_lock(task);
	nsproxy = task->nsproxy;
	if (nsproxy)
		ns = get_ipc_ns(nsproxy->ipc_ns);
	task_unlock(task);

	return ns ? &ns->ns : NULL;
}

static void ipcns_put(struct ns_common *ns)
{
	return vns_put_ipc_ns(to_ipc_ns(ns));
}

static int ipcns_install(struct nsset *nsset, struct ns_common *new)
{
	struct nsproxy *nsproxy = nsset->nsproxy;
	struct ipc_namespace *ns = to_ipc_ns(new);
	if (!ns_capable(ns->user_ns, CAP_SYS_ADMIN) ||
	    !ns_capable(nsset->cred->user_ns, CAP_SYS_ADMIN))
		return -EPERM;

	vns_put_ipc_ns(nsproxy->ipc_ns);
	nsproxy->ipc_ns = get_ipc_ns(ns);
	return 0;
}

static struct user_namespace *ipcns_owner(struct ns_common *ns)
{
	return to_ipc_ns(ns)->user_ns;
}

const struct proc_ns_operations vns_ipcns_operations = { /* [RENAME] */
	.name		= "ipc",
	.type		= CLONE_NEWIPC,
	.get		= ipcns_get,
	.put		= ipcns_put,
	.install	= ipcns_install,
	.owner		= ipcns_owner,
};
