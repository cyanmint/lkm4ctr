// SPDX-License-Identifier: GPL-2.0-only
/*
 * Vendored from kernel-common kernel/pid_namespace.c (kernel version 6.1.124,
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
 * Pid namespaces
 *
 * Authors:
 *    (C) 2007 Pavel Emelyanov <xemul@openvz.org>, OpenVZ, SWsoft Inc.
 *    (C) 2007 Sukadev Bhattiprolu <sukadev@us.ibm.com>, IBM
 *     Many thanks to Oleg Nesterov for comments and help
 *
 */

#include <linux/pid.h>
#include <linux/pid_namespace.h>
#include <linux/user_namespace.h>
#include <linux/syscalls.h>
#include <linux/cred.h>
#include <linux/err.h>
#include <linux/acct.h>
#include <linux/slab.h>
#include <linux/proc_ns.h>
#include <linux/reboot.h>
#include <linux/export.h>
#include <linux/sched/task.h>
#include <linux/sched/signal.h>
#include <linux/idr.h>
#include "../vendor_kernel.h"

static DEFINE_MUTEX(pid_caches_mutex);
/* Write once array, filled from the beginning. */
static struct kmem_cache *pid_cache[MAX_PID_NS_LEVEL];

/*
 * creates the kmem cache to allocate pids from.
 * @level: pid namespace level
 */

static struct kmem_cache *create_pid_cachep(unsigned int level)
{
	/* Level 0 is init_pid_ns.pid_cachep */
	struct kmem_cache **pkc = &pid_cache[level - 1];
	struct kmem_cache *kc;
	char name[4 + 10 + 1];
	unsigned int len;

	kc = READ_ONCE(*pkc);
	if (kc)
		return kc;

	snprintf(name, sizeof(name), "pid_%u", level + 1);
	len = sizeof(struct pid) + level * sizeof(struct upid);
	mutex_lock(&pid_caches_mutex);
	/* Name collision forces to do allocation under mutex. */
	if (!*pkc)
		*pkc = kmem_cache_create(name, len, 0,
					 SLAB_HWCACHE_ALIGN | SLAB_ACCOUNT, NULL);
	mutex_unlock(&pid_caches_mutex);
	/* current can fail, but someone else can succeed. */
	return READ_ONCE(*pkc);
}

static struct ucounts *inc_pid_namespaces(struct user_namespace *ns)
{
	return inc_ucount(ns, current_euid(), UCOUNT_PID_NAMESPACES);
}

static void dec_pid_namespaces(struct ucounts *ucounts)
{
	dec_ucount(ucounts, UCOUNT_PID_NAMESPACES);
}

static struct pid_namespace *create_pid_namespace(struct user_namespace *user_ns,
	struct pid_namespace *parent_pid_ns)
{
	struct pid_namespace *ns;
	unsigned int level = parent_pid_ns->level + 1;
	struct ucounts *ucounts;
	int err;

	err = -EINVAL;
	if (!in_userns(parent_pid_ns->user_ns, user_ns))
		goto out;

	err = -ENOSPC;
	if (level > MAX_PID_NS_LEVEL)
		goto out;
	ucounts = inc_pid_namespaces(user_ns);
	if (!ucounts)
		goto out;

	err = -ENOMEM;
	/* [BUILD-COMPAT] allocate from the real pid_ns_cachep (resolved at
	 * init) instead of kzalloc, so the real kernel's destroy_pid_namespace()
	 * / delayed_free_pidns() can safely kmem_cache_free() this object once
	 * installed on the real task_struct->nsproxy. */
	ns = kmem_cache_zalloc(vns_pid_ns_cachep, GFP_KERNEL);
	if (ns == NULL)
		goto out_dec;

	idr_init(&ns->idr);

	ns->pid_cachep = create_pid_cachep(level);
	if (ns->pid_cachep == NULL)
		goto out_free_idr;

	/* [BUILD-COMPAT] proc_alloc_inum is resolved lazily in vendor_kernel. */
	err = vns_alloc_inum(&ns->ns);
	if (err)
		goto out_free_idr;
	/* [RENAME] vendored proc-ns ops are namespaced. */
	ns->ns.ops = &vns_pidns_operations;

	vns_pid_init_ref(ns);
	ns->level = level;
	ns->parent = vns_get_pid_ns(parent_pid_ns); /* [BUILD-COMPAT] */
	ns->user_ns = vns_get_user_ns(user_ns); /* [BUILD-COMPAT] */
	ns->ucounts = ucounts;
	ns->pid_allocated = PIDNS_ADDING;

	return ns;

out_free_idr:
	idr_destroy(&ns->idr);
	kmem_cache_free(vns_pid_ns_cachep, ns); /* [BUILD-COMPAT] */
out_dec:
	dec_pid_namespaces(ucounts);
out:
	return ERR_PTR(err);
}

/*
 * [BUILD-COMPAT] Deliberately leak the pid_namespace object itself instead
 * of freeing it (upstream frees it here via kmem_cache_free() after
 * call_rcu()).
 *
 * The real kernel's alloc_pid()/free_pid()/put_pid() (kernel/pid.c) are
 * unconditional of CONFIG_PID_NS and keep touching this object's fields
 * (->pid_cachep, ->idr, ->pid_allocated, ->child_reaper) for as long as any
 * struct pid allocated against it is alive -- which, via a lingering procfs
 * dentry/struct file, can outlive our own nsproxy-based refcounting by an
 * arbitrary amount of time. On a real CONFIG_PID_NS=y kernel this is safe
 * because alloc_pid() takes a get_pid_ns() reference on the namespace for
 * every struct pid it hands out, and put_pid()'s matching put_pid_ns() is
 * exactly what is expected to trigger this destroy path -- so the object
 * cannot be destroyed while a struct pid still references it. But on the
 * target GKI kernels this module exists for (CONFIG_PID_NS=n),
 * get_pid_ns()/put_pid_ns() are no-op static inlines, so that protection is
 * gone entirely: nothing pins the namespace alive for the lifetime of the
 * struct pid objects allocated from it, and destroying it here (as soon as
 * our own nsproxy attach/detach refcount hits zero) reopens a
 * use-after-free window that crashes later (e.g. put_pid()/proc_free_inode()
 * dereferencing a freed ns->pid_cachep/ns->child_reaper from an RCU
 * callback, observed as a NULL-pointer oops in put_pid()).
 *
 * There is no cheap, safe way to hook the real (always compiled-in,
 * unconditional-of-config) alloc_pid()/free_pid()/put_pid() to restore the
 * missing pinning without risking further instability, so -- matching this
 * codebase's established "leak rather than risk a UAF/crash" convention
 * (see vns_nsproxy_put_deferred(), vns_put_foreign_nsproxy()) -- we simply
 * never give the object's memory back to the slab allocator. The ucounts
 * and user_ns references are still released promptly below since neither
 * is ever touched by alloc_pid()/free_pid()/put_pid(), so keeping those
 * indefinitely would be a needless additional (and unbounded) leak.
 *
 * This trades a small, bounded (sizeof(struct pid_namespace) plus its
 * pid_cachep and proc-ns inum) per-namespace-creation memory/inum leak for
 * eliminating a real kernel panic; pid namespaces are created far less
 * frequently than individual tasks, so the leak is not expected to be
 * operationally significant.
 *
 * NOTE: despite the (upstream-matching, kept for diffability) name, this no
 * longer actually destroys/frees @ns -- it only releases the two references
 * (ucounts, user_ns) that are safe to release immediately. See above.
 */
static void destroy_pid_namespace(struct pid_namespace *ns)
{
	dec_pid_namespaces(ns->ucounts);
	vns_put_user_ns(ns->user_ns); /* [BUILD-COMPAT] */
}

struct pid_namespace *vns_copy_pid_ns( /* [RENAME] */
unsigned long flags,
	struct user_namespace *user_ns, struct pid_namespace *old_ns)
{
	/*
	 * The real fork()/copy_process() path always consumes
	 * current->nsproxy->pid_ns_for_children for future children, even on a
	 * kernel built with CONFIG_PID_NS=n. Installing one of vendor_kernel's
	 * module-owned pid_namespace objects there therefore lets the real
	 * kernel allocate a task whose task_active_pid_ns() is that fake
	 * namespace; when that task is the namespace-local pid 1 and exits, the
	 * real kernel's own copy_process() (kernel/fork.c, unconditional of
	 * CONFIG_PID_NS) has already set pid_ns->child_reaper = that task, so
	 * the real do_exit()->find_child_reaper() path (kernel/exit.c) is
	 * about to call the target's own zap_pid_ns_processes(), which on a
	 * CONFIG_PID_NS=n build is an unconditional BUG() stub
	 * (include/linux/pid_namespace.h). Rather than degrade CLONE_NEWPID
	 * to no-op bookkeeping here and lose real pid namespace isolation
	 * entirely, always create the module-owned pid_namespace below --
	 * alloc_pid()/task_active_pid_ns() themselves are unconditional of
	 * CONFIG_PID_NS, so per-namespace pid virtualization keeps working
	 * for real regardless. The BUG() is instead defused right before it
	 * would fire, in vns_task_exit_cleanup() (kernel/nsproxy.c), the same
	 * do_exit() exit-safety kprobe this module already installs -- see
	 * that function's comment, and vns_zap_pid_ns_processes() below, for
	 * the reduced-scope cascade used here (SIGKILL everyone else in the
	 * namespace, then stop admitting new members; orphan
	 * reparenting/reaping is left to the host's own genuine parent chain).
	 */
	if (!(flags & CLONE_NEWPID))
		return vns_get_pid_ns(old_ns); /* [BUILD-COMPAT] */
	if (task_active_pid_ns(current) != old_ns)
		return ERR_PTR(-EINVAL);
	return create_pid_namespace(user_ns, old_ns);
}

/*
 * [BUILD-COMPAT] Local equivalent of the real kernel's get_pid_ns(), which
 * is always a static inline in pid_namespace.h whose body differs based on
 * the *target* kernel's own CONFIG_PID_NS setting (real refcount_inc() vs.
 * no-op). Since vendor_kernel installs and refcounts its own struct
 * pid_namespace objects independently of the target's CONFIG_PID_NS, this
 * shim always performs the real refcount_inc() semantics. See
 * vendor_kernel.h for the full rationale.
 */
struct pid_namespace *vns_get_pid_ns(struct pid_namespace *ns) /* [BUILD-COMPAT] */
{
	if (ns != &init_pid_ns)
		vns_pid_get_ref(ns);
	return ns;
}

void vns_put_pid_ns(struct pid_namespace *ns) /* [RENAME] */
{
	struct pid_namespace *parent;

	while (ns != &init_pid_ns) {
		parent = ns->parent;
		if (!vns_pid_put_ref(ns))
			break;
		destroy_pid_namespace(ns);
		ns = parent;
	}
}

void vns_zap_pid_ns_processes(struct pid_namespace *pid_ns) /* [RENAME] */
{
	struct task_struct *task;
	struct pid *pid;
	int nr;

	/* Don't allow any more processes into the pid namespace. */
	disable_pid_allocation(pid_ns);

	/*
	 * [BUILD-COMPAT] The real zap_pid_ns_processes() (kernel/pid_namespace.c)
	 * also blocks in a kernel_wait4() loop to reap every zombie left
	 * behind, which requires sleeping and therefore cannot run from
	 * vns_task_exit_cleanup()'s do_exit() kprobe pre_handler (atomic
	 * context; see the comment there). Only the safe, non-blocking half
	 * of the real cascade is reproduced here -- SIGKILL every other task
	 * this namespace's idr still tracks, then stop admitting new members.
	 * Orphan reparenting/reaping is deliberately left to the host's own
	 * genuine parent chain.
	 */
	rcu_read_lock();
	read_lock(&tasklist_lock);
	nr = 2;
	idr_for_each_entry_continue(&pid_ns->idr, pid, nr) {
		task = pid_task(pid, PIDTYPE_PID);
		if (task && task != current && !__fatal_signal_pending(task))
			send_sig(SIGKILL, task, 1);
	}
	read_unlock(&tasklist_lock);
	rcu_read_unlock();
}

#ifdef CONFIG_CHECKPOINT_RESTORE
/* [BUILD-COMPAT] sysctl registration is omitted for the out-of-tree vendor copy. */
#if 0
static int pid_ns_ctl_handler(struct ctl_table *table, int write,
		void *buffer, size_t *lenp, loff_t *ppos)
{
	struct pid_namespace *pid_ns = task_active_pid_ns(current);
	struct ctl_table tmp = *table;
	int ret, next;

	if (write && !checkpoint_restore_ns_capable(pid_ns->user_ns))
		return -EPERM;

	/*
	 * Writing directly to ns' last_pid field is OK, since this field
	 * is volatile in a living namespace anyway and a code writing to
	 * it should synchronize its usage with external means.
	 */

	next = idr_get_cursor(&pid_ns->idr) - 1;

	tmp.data = &next;
	ret = proc_dointvec_minmax(&tmp, write, buffer, lenp, ppos);
	if (!ret && write)
		idr_set_cursor(&pid_ns->idr, next + 1);

	return ret;
}

extern int pid_max;
static struct ctl_table pid_ns_ctl_table[] = {
	{
		.procname = "ns_last_pid",
		.maxlen = sizeof(int),
		.mode = 0666, /* permissions are checked in the handler */
		.proc_handler = pid_ns_ctl_handler,
		.extra1 = SYSCTL_ZERO,
		.extra2 = &pid_max,
	},
	{ }
};
static struct ctl_path kern_path[] = { { .procname = "kernel", }, { } };
#endif
#endif	/* CONFIG_CHECKPOINT_RESTORE */

int vns_reboot_pid_ns(struct pid_namespace *pid_ns, int cmd) /* [RENAME] */
{
	if (pid_ns == &init_pid_ns)
		return 0;

	switch (cmd) {
	case LINUX_REBOOT_CMD_RESTART2:
	case LINUX_REBOOT_CMD_RESTART:
		pid_ns->reboot = SIGHUP;
		break;

	case LINUX_REBOOT_CMD_POWER_OFF:
	case LINUX_REBOOT_CMD_HALT:
		pid_ns->reboot = SIGINT;
		break;
	default:
		return -EINVAL;
	}

	read_lock(&tasklist_lock);
	send_sig(SIGKILL, pid_ns->child_reaper, 1);
	read_unlock(&tasklist_lock);

	do_exit(0);

	/* Not reached */
	return 0;
}

static inline struct pid_namespace *to_pid_ns(struct ns_common *ns)
{
	return container_of(ns, struct pid_namespace, ns);
}

static struct ns_common *pidns_get(struct task_struct *task)
{
	struct pid_namespace *ns;

	rcu_read_lock();
	ns = task_active_pid_ns(task);
	if (ns)
		vns_get_pid_ns(ns); /* [BUILD-COMPAT] */
	rcu_read_unlock();

	return ns ? &ns->ns : NULL;
}

static struct ns_common *pidns_for_children_get(struct task_struct *task)
{
	struct pid_namespace *ns = NULL;

	task_lock(task);
	if (task->nsproxy) {
		ns = task->nsproxy->pid_ns_for_children;
		vns_get_pid_ns(ns); /* [BUILD-COMPAT] */
	}
	task_unlock(task);

	if (ns) {
		read_lock(&tasklist_lock);
		if (!ns->child_reaper) {
			vns_put_pid_ns(ns);
			ns = NULL;
		}
		read_unlock(&tasklist_lock);
	}

	return ns ? &ns->ns : NULL;
}

static void pidns_put(struct ns_common *ns)
{
	vns_put_pid_ns(to_pid_ns(ns)); /* [BUILD-COMPAT] */
}

static int pidns_install(struct nsset *nsset, struct ns_common *ns)
{
	struct nsproxy *nsproxy = nsset->nsproxy;
	struct pid_namespace *active = task_active_pid_ns(current);
	struct pid_namespace *ancestor, *new = to_pid_ns(ns);

	if (!ns_capable(new->user_ns, CAP_SYS_ADMIN) ||
	    !ns_capable(nsset->cred->user_ns, CAP_SYS_ADMIN))
		return -EPERM;

	/*
	 * Only allow entering the current active pid namespace
	 * or a child of the current active pid namespace.
	 *
	 * This is required for fork to return a usable pid value and
	 * this maintains the property that processes and their
	 * children can not escape their current pid namespace.
	 */
	if (new->level < active->level)
		return -EINVAL;

	ancestor = new;
	while (ancestor->level > active->level)
		ancestor = ancestor->parent;
	if (ancestor != active)
		return -EINVAL;

	vns_put_pid_ns(nsproxy->pid_ns_for_children); /* [BUILD-COMPAT] */
	nsproxy->pid_ns_for_children = vns_get_pid_ns(new); /* [BUILD-COMPAT] */
	return 0;
}

static struct ns_common *pidns_get_parent(struct ns_common *ns)
{
	struct pid_namespace *active = task_active_pid_ns(current);
	struct pid_namespace *pid_ns, *p;

	/* See if the parent is in the current namespace */
	pid_ns = p = to_pid_ns(ns)->parent;
	for (;;) {
		if (!p)
			return ERR_PTR(-EPERM);
		if (p == active)
			break;
		p = p->parent;
	}

	return &vns_get_pid_ns(pid_ns)->ns; /* [BUILD-COMPAT] */
}

static struct user_namespace *pidns_owner(struct ns_common *ns)
{
	return to_pid_ns(ns)->user_ns;
}

const struct proc_ns_operations vns_pidns_operations = { /* [RENAME] */
	.name		= "pid",
	.type		= CLONE_NEWPID,
	.get		= pidns_get,
	.put		= pidns_put,
	.install	= pidns_install,
	.owner		= pidns_owner,
	.get_parent	= pidns_get_parent,
};

const struct proc_ns_operations vns_pidns_for_children_operations = { /* [RENAME] */
	.name		= "pid_for_children",
	.real_ns_name	= "pid",
	.type		= CLONE_NEWPID,
	.get		= pidns_for_children_get,
	.put		= pidns_put,
	.install	= pidns_install,
	.owner		= pidns_owner,
	.get_parent	= pidns_get_parent,
};

void vns_pid_ns_init(void) /* [RENAME] */
{
	/* [BUILD-COMPAT] module-owned cache: see vendor_kernel/README.md's
	 * "Slab-cache consistency with the real kernel" for why this no
	 * longer resolves the real kernel's private pid_ns_cachep. */
	if (!vns_pid_ns_cachep)
		vns_pid_ns_cachep = kmem_cache_create("vns_pid_namespace",
			sizeof(struct pid_namespace), 0,
			SLAB_HWCACHE_ALIGN | SLAB_ACCOUNT, NULL);
}
