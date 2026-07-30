// SPDX-License-Identifier: GPL-2.0
/*
 * vendor_kernel_diag.c - diagnostics renderer and live-resource "provider"
 * backend for vendor_kernel. This is NEW code (not vendored from kernel-common).
 *
 * Two roles:
 *   1. vendor_kernel_diag_snprintf() - the aggregate registry/overlay dump
 *      shown by the /v/namespaces and /v/resources diagfs files.
 *   2. the vns_diag_res_* provider vtable (see glue/vendor_kernel_diag.h) that
 *      backs the procfs-style /v/ns/<type>/ and /v/ipc/<class>/ trees: it
 *      enumerates, renders and (crucially) *deletes* the live per-task
 *      vendored namespaces and SysV IPC objects vendor_kernel owns, reusing
 *      the same registry lock and IPC teardown paths the rest of the module
 *      already uses -- no new lock orderings are introduced here.
 */
#include <linux/kernel.h>
#include <linux/hashtable.h>
#include <linux/spinlock.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/pid.h>
#include <linux/cred.h>
#include <linux/uidgid.h>
#include <linux/rwsem.h>
#include <linux/idr.h>
#include <linux/ipc.h>
#include <linux/ipc_namespace.h>

#include "../vendor/vendor_kernel.h"
#include "vendor_kernel_diag.h"

size_t vendor_kernel_diag_snprintf(char *buf, size_t buflen)
{
	size_t pos = 0;
	struct vns_task *t;
	unsigned int bkt;
	unsigned long flags;

	spin_lock_irqsave(&vendor_kernel_registry.lock, flags);
	pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
			 "enabled: %s\n"
			 "tasks: %lu\n"
			 "stat_unshare: %lu\n"
			 "stat_setns: %lu\n"
			 "stat_clone: %lu\n"
			 "stat_reboot: %lu\n",
			 vendor_kernel_enabled ? "yes" : "no",
			 vendor_kernel_registry.task_count,
			 vendor_kernel_registry.stat_unshare,
			 vendor_kernel_registry.stat_setns,
			 vendor_kernel_registry.stat_clone,
			 vendor_kernel_registry.stat_reboot);
	hash_for_each(vendor_kernel_registry.tasks, bkt, t, node)
		pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
				 "tgid=%d nsproxy=%px\n", t->tgid, t->nsproxy);
	spin_unlock_irqrestore(&vendor_kernel_registry.lock, flags);
	pos += vns_overlay_diag_snprintf(buf + pos, pos < buflen ? buflen - pos : 0);
	return pos;
}

/* ------------------------------------------------------------------- */
/* provider vtable: shared helpers                                      */
/* ------------------------------------------------------------------- */

static bool vns_diag_is_ns(int prov)
{
	return prov >= VNS_DIAG_NS_PID && prov <= VNS_DIAG_NS_CGROUP;
}

static bool vns_diag_is_ipc(int prov)
{
	return prov >= VNS_DIAG_IPC_SHM && prov <= VNS_DIAG_IPC_SEM;
}

const char *vns_diag_res_name(int prov)
{
	switch (prov) {
	case VNS_DIAG_NS_PID:	return "pid";
	case VNS_DIAG_NS_UTS:	return "uts";
	case VNS_DIAG_NS_IPC:	return "ipc";
	case VNS_DIAG_NS_USER:	return "user";
	case VNS_DIAG_NS_NET:	return "net";
	case VNS_DIAG_NS_TIME:	return "time";
	case VNS_DIAG_NS_MNT:	return "mnt";
	case VNS_DIAG_NS_CGROUP:	return "cgroup";
	case VNS_DIAG_IPC_SHM:	return "shm";
	case VNS_DIAG_IPC_MSG:	return "msg";
	case VNS_DIAG_IPC_SEM:	return "sem";
	default:		return NULL;
	}
}

/* ------------------------------------------------------------------- */
/* namespace provider (keyed by owning tgid)                            */
/* ------------------------------------------------------------------- */

/*
 * vns_diag_ns_obj_locked() - resolve the per-type namespace object and its
 * proc inum for one tracked task's nsproxy. MUST be called with
 * vendor_kernel_registry.lock held (the nsproxy stays pinned by the registry
 * entry's own reference for as long as that entry, hence this lock, is held).
 *
 * Some namespace types (mnt) are opaque incomplete structs in public kernel
 * headers, so only their pointer -- never a dereferenced inum -- can be read
 * here; *inum stays 0 in that case. USER is not reachable from nsproxy at all
 * (it lives on the task's cred) and is resolved outside the lock in the
 * render path.
 */
static void vns_diag_ns_obj_locked(struct nsproxy *nsp, int prov,
				   void **obj, unsigned int *inum)
{
	*obj = NULL;
	*inum = 0;
	if (!nsp)
		return;

	switch (prov) {
	case VNS_DIAG_NS_PID:
		if (nsp->pid_ns_for_children) {
			*obj = nsp->pid_ns_for_children;
			*inum = nsp->pid_ns_for_children->ns.inum;
		}
		break;
	case VNS_DIAG_NS_UTS:
		if (nsp->uts_ns) {
			*obj = nsp->uts_ns;
			*inum = nsp->uts_ns->ns.inum;
		}
		break;
	case VNS_DIAG_NS_IPC:
		if (nsp->ipc_ns) {
			*obj = nsp->ipc_ns;
			*inum = nsp->ipc_ns->ns.inum;
		}
		break;
	case VNS_DIAG_NS_NET:
		if (nsp->net_ns) {
			*obj = nsp->net_ns;
			*inum = nsp->net_ns->ns.inum;
		}
		break;
	case VNS_DIAG_NS_TIME:
		if (nsp->time_ns) {
			*obj = nsp->time_ns;
			*inum = nsp->time_ns->ns.inum;
		}
		break;
	case VNS_DIAG_NS_CGROUP:
		if (nsp->cgroup_ns) {
			*obj = nsp->cgroup_ns;
			*inum = nsp->cgroup_ns->ns.inum;
		}
		break;
	case VNS_DIAG_NS_MNT:
		/* struct mnt_namespace is opaque here: pointer only, no inum. */
		*obj = nsp->mnt_ns;
		break;
	case VNS_DIAG_NS_USER:
	default:
		/* user_ns lives on the task cred, resolved in the render path. */
		break;
	}
}

static bool vns_diag_ns_type_present(struct nsproxy *nsp, int prov)
{
	void *obj;
	unsigned int inum;

	if (prov == VNS_DIAG_NS_USER)
		return nsp != NULL;
	vns_diag_ns_obj_locked(nsp, prov, &obj, &inum);
	return obj != NULL;
}

static int vns_diag_ns_snapshot(int prov, u64 *keys, int max)
{
	struct vns_task *t;
	unsigned int bkt;
	unsigned long flags;
	int n = 0;

	spin_lock_irqsave(&vendor_kernel_registry.lock, flags);
	hash_for_each(vendor_kernel_registry.tasks, bkt, t, node) {
		if (!vns_diag_ns_type_present(t->nsproxy, prov))
			continue;
		if (n < max)
			keys[n] = (u64)(u32)t->tgid;
		n++;
	}
	spin_unlock_irqrestore(&vendor_kernel_registry.lock, flags);
	return n;
}

static bool vns_diag_ns_present(int prov, u64 key)
{
	pid_t tgid = (pid_t)key;
	struct vns_task *t;
	unsigned long flags;
	bool present = false;

	spin_lock_irqsave(&vendor_kernel_registry.lock, flags);
	hash_for_each_possible(vendor_kernel_registry.tasks, t, node,
			       (unsigned long)tgid) {
		if (t->tgid == tgid) {
			present = vns_diag_ns_type_present(t->nsproxy, prov);
			break;
		}
	}
	spin_unlock_irqrestore(&vendor_kernel_registry.lock, flags);
	return present;
}

static size_t vns_diag_ns_render(int prov, u64 key, char *buf, size_t buflen)
{
	pid_t tgid = (pid_t)key;
	struct vns_task *t;
	unsigned long flags;
	bool present = false;
	void *obj = NULL;
	unsigned int inum = 0;
	struct nsproxy *nsp = NULL;
	size_t pos = 0;
	struct pid *pid;
	struct task_struct *task;
	char comm[TASK_COMM_LEN] = "";

	spin_lock_irqsave(&vendor_kernel_registry.lock, flags);
	hash_for_each_possible(vendor_kernel_registry.tasks, t, node,
			       (unsigned long)tgid) {
		if (t->tgid == tgid) {
			present = true;
			nsp = t->nsproxy;
			if (prov != VNS_DIAG_NS_USER)
				vns_diag_ns_obj_locked(nsp, prov, &obj, &inum);
			break;
		}
	}
	spin_unlock_irqrestore(&vendor_kernel_registry.lock, flags);

	if (!present)
		return scnprintf(buf, buflen,
				 "no such vendored namespace instance (tgid %d is not tracked by vendor_kernel)\n",
				 tgid);

	/* Best-effort live-task lookup for comm and, for USER, the user_ns. */
	pid = find_get_pid(tgid);
	if (pid) {
		task = get_pid_task(pid, PIDTYPE_PID);
		if (task) {
			get_task_comm(comm, task);
			if (prov == VNS_DIAG_NS_USER) {
				const struct cred *cred = get_task_cred(task);

				if (cred) {
					obj = cred->user_ns;
					if (cred->user_ns)
						inum = cred->user_ns->ns.inum;
					put_cred(cred);
				}
			}
			put_task_struct(task);
		}
		put_pid(pid);
	}

	pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
			 "tgid: %d\n", tgid);
	pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
			 "comm: %s\n", comm[0] ? comm : "(exited or unreachable)");
	pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
			 "type: %s namespace\n", vns_diag_res_name(prov));
	pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
			 "nsproxy: %px\n", nsp);
	pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
			 "object: %px\n", obj);
	if (inum)
		pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
				 "inum: %u\n", inum);
	else
		pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
				 "inum: (opaque/unavailable for this namespace type)\n");
	pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
			 "note: deleting this file SIGKILLs thread group %d and frees its whole vendored nsproxy (shared across every /v/ns/<type>/%d)\n",
			 tgid, tgid);
	return pos;
}

#define VNS_DIAG_NS_DRAIN_ITERS	200
#define VNS_DIAG_NS_DRAIN_POLL_MS	25

static int vns_diag_ns_delete(u64 key)
{
	pid_t tgid = (pid_t)key;
	struct pid *pid;
	struct task_struct *task;
	int i;

	if (!vns_task_find(tgid))
		return -ENOENT;

	/*
	 * Make every thread still pinned to this vendored nsproxy leave it
	 * cleanly: a group-wide SIGKILL routes each one through the do_exit()
	 * cleanup hook (vns_task_exit_cleanup(), kernel/nsproxy.c), which
	 * swaps its nsproxy back onto the pinned init singleton and defers the
	 * vendored object's teardown. Then wait, bounded, for the task to
	 * actually die -- the same msleep-poll drain idiom as
	 * lkm4ctr_safe_unload_fn()/lkm4ctr_hotreload_worker(), never a busy
	 * spin.
	 */
	pid = find_get_pid(tgid);
	if (pid) {
		task = get_pid_task(pid, PIDTYPE_PID);
		if (task) {
			kill_pid(pid, SIGKILL, 1);
			put_task_struct(task);
			for (i = 0; i < VNS_DIAG_NS_DRAIN_ITERS; i++) {
				struct task_struct *t = get_pid_task(pid, PIDTYPE_PID);

				if (!t)
					break;
				/*
				 * A zombie (exit_state set) has already run the
				 * do_exit() body, so its vendored nsproxy is
				 * already swapped away; that is "left it
				 * cleanly" for our purposes.
				 */
				if (t->exit_state) {
					put_task_struct(t);
					break;
				}
				put_task_struct(t);
				msleep(VNS_DIAG_NS_DRAIN_POLL_MS);
			}
		}
		put_pid(pid);
	}

	/*
	 * Drop vendor_kernel's own pinned registry reference last. If the task
	 * had already exited (or never had a live task, i.e. a stale entry),
	 * this alone frees the tracked nsproxy; if it is somehow still running,
	 * this only releases our reference and the object stays alive under the
	 * task's own until it exits -- never a use-after-free either way.
	 */
	vns_registry_remove(tgid);
	return 0;
}

/* ------------------------------------------------------------------- */
/* SysV IPC provider (keyed by ipc id, in the default vendored ns)      */
/* ------------------------------------------------------------------- */

/*
 * ipc_namespace::ids[] is indexed by the private IPC_{SEM,MSG,SHM}_IDS
 * constants from vendor/ipc/util.h, which is not a public header we can
 * include from glue. These values are part of the on-disk-stable SysV IPC ABI
 * layout and identical across every supported KMI, so mirror them locally.
 */
#define VNS_DIAG_IPC_SEM_IDS	0
#define VNS_DIAG_IPC_MSG_IDS	1
#define VNS_DIAG_IPC_SHM_IDS	2

static int vns_diag_ipc_ids_index(int prov)
{
	switch (prov) {
	case VNS_DIAG_IPC_SHM:	return VNS_DIAG_IPC_SHM_IDS;
	case VNS_DIAG_IPC_MSG:	return VNS_DIAG_IPC_MSG_IDS;
	case VNS_DIAG_IPC_SEM:	return VNS_DIAG_IPC_SEM_IDS;
	default:		return -1;
	}
}

static struct ipc_ids *vns_diag_ipc_ids(int prov)
{
	struct ipc_namespace *ns = vns_ipc_active_default();
	int idx = vns_diag_ipc_ids_index(prov);

	if (!ns || idx < 0)
		return NULL;
	return &ns->ids[idx];
}

static int vns_diag_ipc_snapshot(int prov, u64 *keys, int max)
{
	struct ipc_ids *ids = vns_diag_ipc_ids(prov);
	struct kern_ipc_perm *perm;
	int pos = 0;
	int n = 0;

	if (!ids)
		return 0;

	down_read(&ids->rwsem);
	while ((perm = idr_get_next(&ids->ipcs_idr, &pos)) != NULL) {
		if (n < max)
			keys[n] = (u64)(u32)perm->id;
		n++;
		pos++;
	}
	up_read(&ids->rwsem);
	return n;
}

static struct kern_ipc_perm *vns_diag_ipc_find_locked(struct ipc_ids *ids, int id)
{
	struct kern_ipc_perm *perm;
	int pos = 0;

	while ((perm = idr_get_next(&ids->ipcs_idr, &pos)) != NULL) {
		if (perm->id == id)
			return perm;
		pos++;
	}
	return NULL;
}

static bool vns_diag_ipc_present(int prov, u64 key)
{
	struct ipc_ids *ids = vns_diag_ipc_ids(prov);
	bool present;

	if (!ids)
		return false;
	down_read(&ids->rwsem);
	present = vns_diag_ipc_find_locked(ids, (int)key) != NULL;
	up_read(&ids->rwsem);
	return present;
}

static size_t vns_diag_ipc_render(int prov, u64 key, char *buf, size_t buflen)
{
	struct ipc_ids *ids = vns_diag_ipc_ids(prov);
	struct kern_ipc_perm *perm;
	size_t pos = 0;
	int id = (int)key;

	if (!ids)
		return scnprintf(buf, buflen,
				 "vendor_kernel default ipc namespace unavailable\n");

	down_read(&ids->rwsem);
	perm = vns_diag_ipc_find_locked(ids, id);
	if (perm) {
		pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
				 "type: %s\n", vns_diag_res_name(prov));
		pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
				 "id: %d\n", perm->id);
		pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
				 "key: %d\n", perm->key);
		pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
				 "uid: %u\n", __kuid_val(perm->uid));
		pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
				 "gid: %u\n", __kgid_val(perm->gid));
		pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
				 "cuid: %u\n", __kuid_val(perm->cuid));
		pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
				 "cgid: %u\n", __kgid_val(perm->cgid));
		pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
				 "mode: %#o\n", perm->mode);
		pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
				 "note: deleting this file performs IPC_RMID (wakes any blocked waiter with -EIDRM, then frees the object)\n");
	} else {
		pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
				 "no such %s object (id %d) in the default vendored ipc namespace\n",
				 vns_diag_res_name(prov), id);
	}
	up_read(&ids->rwsem);
	return pos;
}

/*
 * vns_diag_ipc_delete() - IPC_RMID one object. Reuses the vendored SysV
 * ctl() syscall handlers verbatim (correct rwsem/rcu/ipc_lock ordering,
 * graceful -EIDRM wakeup of blocked waiters). These operate on the caller's
 * *current* ipc namespace: for an ordinary (never unshare(CLONE_NEWIPC))
 * writer that is exactly vns_ipc_active_default(), the same namespace the
 * enumeration above walks. A writer inside a container's own unshared ipc
 * namespace would instead act on that namespace (or get -EINVAL if the id
 * isn't present there) -- a deliberate, documented context dependence, not a
 * cross-namespace teardown of state this module merely observes.
 */
static int vns_diag_ipc_delete(int prov, u64 key)
{
	int id = (int)key;
	long ret;

	switch (prov) {
	case VNS_DIAG_IPC_SHM:
		ret = vns_shmctl(id, IPC_RMID, NULL);
		break;
	case VNS_DIAG_IPC_MSG:
		ret = vns_msgctl(id, IPC_RMID, NULL);
		break;
	case VNS_DIAG_IPC_SEM:
		ret = vns_semctl(id, 0, IPC_RMID, 0);
		break;
	default:
		return -EINVAL;
	}
	return ret < 0 ? (int)ret : 0;
}

/* ------------------------------------------------------------------- */
/* provider dispatch                                                    */
/* ------------------------------------------------------------------- */

int vns_diag_res_snapshot(int prov, u64 *keys, int max)
{
	if (vns_diag_is_ns(prov))
		return vns_diag_ns_snapshot(prov, keys, max);
	if (vns_diag_is_ipc(prov))
		return vns_diag_ipc_snapshot(prov, keys, max);
	return 0;
}

bool vns_diag_res_present(int prov, u64 key)
{
	if (vns_diag_is_ns(prov))
		return vns_diag_ns_present(prov, key);
	if (vns_diag_is_ipc(prov))
		return vns_diag_ipc_present(prov, key);
	return false;
}

size_t vns_diag_res_render(int prov, u64 key, char *buf, size_t buflen)
{
	if (vns_diag_is_ns(prov))
		return vns_diag_ns_render(prov, key, buf, buflen);
	if (vns_diag_is_ipc(prov))
		return vns_diag_ipc_render(prov, key, buf, buflen);
	return scnprintf(buf, buflen, "unknown resource provider\n");
}

int vns_diag_res_delete(int prov, u64 key)
{
	if (vns_diag_is_ns(prov))
		return vns_diag_ns_delete(key);
	if (vns_diag_is_ipc(prov))
		return vns_diag_ipc_delete(prov, key);
	return -EINVAL;
}
