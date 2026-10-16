// SPDX-License-Identifier: GPL-2.0
/*
 * vendor_kernel_module.c - vendor_kernel lifecycle: init/exit and hook installation.
 * This is NEW code (not vendored from kernel-common).
 */
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/hashtable.h>
#include <linux/atomic.h>

#include "../vendor/vendor_kernel.h"
#include "shadow_hook.h"
#include "lkm4ctr_log.h"

int (*vns_proc_alloc_inum_fn)(unsigned int *);
void (*vns_proc_free_inum_fn)(unsigned int);
struct mnt_namespace *(*vns_copy_mnt_ns_fn)(unsigned long, struct mnt_namespace *, struct user_namespace *, struct fs_struct *);
void (*vns_put_mnt_ns_fn)(struct mnt_namespace *);
struct net *(*vns_copy_net_ns_fn)(unsigned long, struct user_namespace *, struct net *);
void (*vns_real_free_nsproxy_fn)(struct nsproxy *);
struct pid_namespace *(*vns_real_copy_pid_ns_fn)(unsigned long flags,
	struct user_namespace *user_ns, struct pid_namespace *old_ns);
void (*vns_real_put_pid_ns_fn)(struct pid_namespace *ns);
bool vendor_kernel_enabled;
bool vns_pidns_runtime_supported;

struct vns_registry vendor_kernel_registry;
static atomic_t vns_inum_counter = ATOMIC_INIT(0x60000000);

int vns_alloc_inum(struct ns_common *ns)
{
	vns_zero_stashed(ns); /* [BUILD-COMPAT] */
	if (vns_proc_alloc_inum_fn)
		return vns_proc_alloc_inum_fn(&ns->inum);
	ns->inum = (unsigned int)atomic_inc_return(&vns_inum_counter);
	return 0;
}

void vns_free_inum(struct ns_common *ns)
{
	if (vns_proc_free_inum_fn && ns->inum)
		vns_proc_free_inum_fn(ns->inum);
}

static struct vns_task *__vns_task_find_locked(pid_t tgid)
{
	struct vns_task *t;
	hash_for_each_possible(vendor_kernel_registry.tasks, t, node, (unsigned long)tgid)
		if (t->tgid == tgid)
			return t;
	return NULL;
}

struct vns_task *vns_task_find(pid_t tgid)
{
	struct vns_task *t;
	unsigned long flags;

	spin_lock_irqsave(&vendor_kernel_registry.lock, flags);
	t = __vns_task_find_locked(tgid);
	spin_unlock_irqrestore(&vendor_kernel_registry.lock, flags);
	return t;
}

struct nsproxy *vns_current_nsproxy(void)
{
	struct vns_task *t = vns_task_find(task_tgid_nr(current));
	return t ? t->nsproxy : NULL;
}

struct ipc_namespace *vns_task_ipc_ns(struct task_struct *task)
{
	struct ipc_namespace *ns = task->nsproxy ? task->nsproxy->ipc_ns : NULL;

	if (vns_ipc_ns_is_vendored(ns))
		return ns;
	/*
	 * The task has no vendored ipc_namespace of its own: either its nsproxy
	 * carries no ipc_ns, or it carries any native (non-vendored) namespace.
	 * The pointer-identity set in vendor/ipc/namespace.c is authoritative;
	 * a native child namespace is never mistaken for a module object. Fall
	 * back to vendor_kernel's own fully-initialised, module-owned default so the
	 * SysV/mqueue handlers always act on vendored state exclusively.
	 */
	return vns_ipc_active_default();
}

int vns_registry_set_nsproxy(pid_t tgid, struct nsproxy *nsproxy)
{
	struct vns_task *t;
	unsigned long flags;

	spin_lock_irqsave(&vendor_kernel_registry.lock, flags);
	t = __vns_task_find_locked(tgid);
	if (!t) {
		t = kzalloc(sizeof(*t), GFP_ATOMIC);
		if (!t) {
			spin_unlock_irqrestore(&vendor_kernel_registry.lock, flags);
			return -ENOMEM;
		}
		t->tgid = tgid;
		hash_add(vendor_kernel_registry.tasks, &t->node, (unsigned long)tgid);
		vendor_kernel_registry.task_count++;
	}
	if (nsproxy)
		get_nsproxy(nsproxy);
	if (t->nsproxy)
		vns_put_nsproxy(t->nsproxy);
	t->nsproxy = nsproxy;
	spin_unlock_irqrestore(&vendor_kernel_registry.lock, flags);
	return 0;
}

static struct vns_task *__vns_registry_detach(pid_t tgid)
{
	struct vns_task *t;
	unsigned long flags;

	spin_lock_irqsave(&vendor_kernel_registry.lock, flags);
	t = __vns_task_find_locked(tgid);
	if (t) {
		hash_del(&t->node);
		if (vendor_kernel_registry.task_count)
			vendor_kernel_registry.task_count--;
	}
	spin_unlock_irqrestore(&vendor_kernel_registry.lock, flags);
	return t;
}

void vns_registry_remove(pid_t tgid)
{
	struct vns_task *t = __vns_registry_detach(tgid);

	if (!t)
		return;
	if (t->nsproxy)
		vns_put_nsproxy(t->nsproxy);
	kfree(t);
}

struct nsproxy *vns_registry_take_nsproxy(pid_t tgid)
{
	struct vns_task *t = __vns_registry_detach(tgid);
	struct nsproxy *nsproxy;

	if (!t)
		return NULL;
	nsproxy = t->nsproxy;
	kfree(t);
	return nsproxy;
}

void vns_registry_clone(pid_t parent_tgid, pid_t child_tgid)
{
	struct nsproxy *nsproxy = NULL;
	struct vns_task *t;
	unsigned long flags;

	spin_lock_irqsave(&vendor_kernel_registry.lock, flags);
	t = __vns_task_find_locked(parent_tgid);
	if (t && t->nsproxy) {
		get_nsproxy(t->nsproxy);
		nsproxy = t->nsproxy;
	}
	spin_unlock_irqrestore(&vendor_kernel_registry.lock, flags);
	if (!nsproxy)
		return;
	vns_registry_set_nsproxy(child_tgid, nsproxy);
	vns_put_nsproxy(nsproxy);
}

static void vns_registry_clear_all(void)
{
	struct vns_task *t;
	struct hlist_node *tmp;
	unsigned int bkt;

	hash_for_each_safe(vendor_kernel_registry.tasks, bkt, tmp, t, node) {
		hash_del(&t->node);
		if (t->nsproxy)
			vns_put_nsproxy(t->nsproxy);
		kfree(t);
	}
	vendor_kernel_registry.task_count = 0;
}

static void vns_resolve_symbols(void)
{
	vns_proc_alloc_inum_fn = (void *)shadow_hook_resolve("proc_alloc_inum");
	vns_proc_free_inum_fn = (void *)shadow_hook_resolve("proc_free_inum");
	vns_copy_mnt_ns_fn = (void *)shadow_hook_resolve("copy_mnt_ns");
	vns_put_mnt_ns_fn = (void *)shadow_hook_resolve("put_mnt_ns");
	vns_copy_net_ns_fn = (void *)shadow_hook_resolve("copy_net_ns");
	vns_real_free_nsproxy_fn = (void *)shadow_hook_resolve("free_nsproxy");
	vns_real_copy_pid_ns_fn = (void *)shadow_hook_resolve("copy_pid_ns");
	vns_real_put_pid_ns_fn = (void *)shadow_hook_resolve("put_pid_ns");
	/*
	 * [BUILD-COMPAT] Both must resolve for vendor_kernel to safely hand
	 * pid namespace creation/teardown off to the real kernel (see the
	 * declaration comment on vns_real_copy_pid_ns_fn in vendor_kernel.h);
	 * a kernel exposing only one of the two would be unexpected (both
	 * live in the same CONFIG_PID_NS-gated kernel/pid_namespace.c
	 * translation unit), but fail closed to the module-owned path rather
	 * than risk calling through a NULL/mismatched pointer.
	 */
	vns_pidns_runtime_supported = vns_real_copy_pid_ns_fn && vns_real_put_pid_ns_fn;
	/*
	 * [BUILD-COMPAT] put_net() is always a static inline in
	 * <net/net_namespace.h> (never a standalone kernel symbol), so it
	 * must NOT be resolved by name here: shadow_hook_resolve("put_net")
	 * either fails or, worse, silently binds to an unrelated symbol that
	 * happens to share the name in kallsyms, causing a CFI failure when
	 * called through this mismatched function pointer. Callers use the
	 * real put_net() inline directly instead (see kernel/nsproxy.c).
	 */
}

int vendor_kernel_init(void)
{
	int hooked;
	int total_hooked;
	int err;

	if (vendor_kernel_enabled)
		return 0;

	/*
	 * [BUILD-COMPAT] Capture the real init_user_ns before anything else
	 * runs: vns_current_user_ns() is a plain read of
	 * current_cred()->user_ns (no unresolved symbol involved -- unlike
	 * the real kernel's own current_user_ns(), see vendor_kernel.h's
	 * vns_current_user_ns() comment for why that one must never be
	 * called directly), and insmod always executes from a real
	 * top-level process context, so this is the same object the running
	 * kernel's own (data-symbol, kprobe-unresolvable, sometimes trimmed)
	 * init_user_ns points at. See vendor_kernel.h's init_user_ns macro
	 * for why every other reference to init_user_ns in this module is
	 * redirected to dereference this pointer instead of the real symbol.
	 */
	vns_real_init_user_ns = vns_current_user_ns();
	if (!vns_real_init_user_ns) {
		LKM4CTR_ERR("vendor_kernel", "failed to capture init_user_ns from current task");
		return -ENOENT;
	}

	hash_init(vendor_kernel_registry.tasks);
	spin_lock_init(&vendor_kernel_registry.lock);
	vendor_kernel_registry.task_count = 0;
	vendor_kernel_registry.stat_unshare = 0;
	vendor_kernel_registry.stat_setns = 0;
	vendor_kernel_registry.stat_clone = 0;

	vns_resolve_symbols();
	vns_compat_resolve(); /* [BUILD-COMPAT] resolve non-exported kernel symbols */
	vns_procfs_compat_resolve(); /* [BUILD-COMPAT] resolve native procfs symlink instantiation helpers */
	vns_ipc_compat_resolve(); /* [BUILD-COMPAT] resolve non-exported ipc/mm/security/audit symbols */
	vns_ipc_lookup_resolve(); /* [BUILD-COMPAT] resolve simple_lookup()/security_*_associate() (own CFI-safe translation unit) */
	if (!vns_compat_ready())
		return -ENOENT;
	/*
	 * [BUILD-COMPAT] Build vendor_kernel's own default cgroup_namespace
	 * and point vns_init_nsproxy.cgroup_ns at it unconditionally, never
	 * at the running kernel's real init_cgroup_ns (vns_init_cgroup_ns_ptr
	 * is resolved for cosmetic bookkeeping only -- see vendor_kernel.h).
	 * This mirrors the vns_default_ipc_ns/vns_ipc_active_default()
	 * pattern below, so cgroup namespace support no longer depends on
	 * whether the running kernel's init_cgroup_ns can be resolved at all.
	 */
	vns_cgroup_default_init();
	vns_init_nsproxy.cgroup_ns = &vns_default_cgroup_ns;
	vns_time_ns_default_init();
#if defined(CONFIG_POSIX_MQUEUE) || defined(CONFIG_SYSVIPC)
	/*
	 * [BUILD-COMPAT] vns_init_ipc_ns_ptr (the running kernel's real,
	 * non-exported init_ipc_ns) is resolved for cosmetic bookkeeping only
	 * and is deliberately NOT installed on vns_init_nsproxy.ipc_ns here:
	 * vendor_kernel's IPC subsystem must depend exclusively on the vendored
	 * vns_default_ipc_ns, never on the real kernel's ipc_namespace object.
	 * vns_init_nsproxy.ipc_ns is instead pointed at the vendored default
	 * below, once vns_ipc_default_init() has fully built it.
	 */
#endif
	/* [BUILD-COMPAT] vendored init helpers create vendor_kernel's own
	 * module-owned slab caches instead of resolving the real kernel's. */
	vns_uts_ns_init();
	vns_pid_ns_init();
	err = vns_user_ns_init();
	if (err)
		return err;
	vns_nsproxy_cache_init();
	vns_nsfs_init();

	if (!vns_uts_ns_cache || !vns_pid_ns_cachep || !vns_user_ns_cachep ||
	    !vns_nsproxy_cachep) {
		LKM4CTR_ERR("vendor_kernel", "failed to create module-owned namespace slab cache(s)");
		return -ENOMEM;
	}

	/*
	 * Build vendor_kernel's own default ipc_namespace (mqueuefs + SysV
	 * msg/sem/shm IDRs) before any IPC syscall hook is live, so that every
	 * task that never unshare(CLONE_NEWIPC)'d has a valid namespace to
	 * operate against via vns_current_ipc_ns(). This is built
	 * unconditionally from the vendored vns_* mqueue/sysvipc code, never
	 * from (nor gated on) the running kernel's own init_ipc_ns, so the
	 * shadowed handlers only ever touch vendored ipc state.
	 */
	hooked = vns_ipc_default_init();
	if (hooked) {
		LKM4CTR_ERR("vendor_kernel", "failed to init default ipc namespace (%d)", hooked);
		return hooked;
	}
	/*
	 * Point the pinned default nsproxy at the vendored default ipc_ns (never
	 * the real kernel's) so copy_ipcs()/setns() and the exit-safety sentinel
	 * see a valid, vendored source ns even on CONFIG_IPC_NS=n.
	 */
	vns_init_nsproxy.ipc_ns = vns_ipc_active_default();

	/*
	 * Initialize overlayfs before installing the namespace syscall hooks.
	 * This leaves no fallible initialization after module-owned namespace
	 * objects become reachable through procfs, so a failed module init can
	 * always remove the NSFS lifetime hooks without leaving cached callbacks.
	 */
	hooked = vns_overlay_init();
	if (hooked) {
		vns_ipc_default_exit();
		return hooked;
	}

	hooked = vns_nsfs_lifecycle_init();
	if (hooked) {
		vns_overlay_exit();
		vns_ipc_default_exit();
		return hooked;
	}

	/* [BUILD-COMPAT] must succeed before any hooks are live: without it,
	 * an exiting task's module-owned nsproxy could be freed by the real
	 * kernel's own exit path against the real, mismatched kmem_cache. */
	hooked = vns_exit_hook_init();
	if (hooked) {
		vns_nsfs_lifecycle_exit();
		vns_overlay_exit();
		vns_ipc_default_exit();
		return hooked;
	}

	hooked = shadow_hook_install_all(vendor_kernel_core_hooks, "vendor_kernel");
	if (hooked < 0) {
		shadow_hook_remove_all(vendor_kernel_core_hooks);
		vns_nsfs_lifecycle_exit();
		vns_exit_hook_exit();
		vns_overlay_exit();
		vns_ipc_default_exit();
		return hooked;
	}
	if (!vendor_kernel_required_lifecycle_hooks_ready()) {
		LKM4CTR_ERR("vendor_kernel",
			    "required clone, pre-wakeup namespace-copy, or user-credential lifetime hooks are unavailable");
		shadow_hook_remove_all(vendor_kernel_core_hooks);
		vns_nsfs_lifecycle_exit();
		vns_exit_hook_exit();
		vns_overlay_exit();
		vns_ipc_default_exit();
		return -ENOENT;
	}

	hooked = shadow_hook_install_all(vendor_kernel_ipc_hooks, "vendor_kernel_ipc");
	if (hooked < 0) {
		shadow_hook_remove_all(vendor_kernel_ipc_hooks);
		shadow_hook_remove_all(vendor_kernel_core_hooks);
		vns_nsfs_lifecycle_exit();
		vns_exit_hook_exit();
		vns_overlay_exit();
		vns_ipc_default_exit();
		return hooked;
	}
	total_hooked = hooked;

	/* Keep procfs PID identity capture and inode-eviction tracking live
	 * independently of the optional lookup-hook group, so namespace-directory
	 * entries created while compatibility is live are mapped and cleaned.
	 */
	hooked = vns_procfs_compat_lifecycle_init();
	if (hooked)
		LKM4CTR_WARN("vendor_kernel",
			     "procfs namespace lifecycle hooks unavailable (%d); missing native namespace entries remain fail-closed",
			     hooked);

	/* Best-effort: without the procfs lookup hook, absent native IPC/PID
	 * namespace components remain ENOENT; no syscall result is fabricated.
	 */
	hooked = shadow_hook_install_all(vendor_kernel_procfs_hooks, "vendor_kernel_procfs");
	if (hooked < 0)
		LKM4CTR_WARN("vendor_kernel",
			     "failed to install procfs compatibility hooks (%d); missing native namespace entries will remain ENOENT",
			     hooked);
	else
		total_hooked += hooked;
	if (!vns_procfs_compat_active())
		LKM4CTR_WARN("vendor_kernel",
			     "native /proc/self/ns/{ipc,pid} dentry compatibility is "
			     "unavailable; missing native namespace entries will "
			     "remain ENOENT");

	/*
	 * The translation hooks are needed only when the running kernel lacks
	 * CONFIG_USER_NS, where its uid/gid syscalls return raw global IDs. Do
	 * not infer this from the module's build-time config: a module built by a
	 * no-USER_NS DDK can also be loaded on a test/deployment kernel with the
	 * native core enabled, whose syscalls already translate IDs. Installing
	 * our hooks there would translate the already-mapped syscall result a
	 * second time (e.g. mapped uid 0 becomes overflowuid).
	 */
	if (vns_native_userns_core_available()) {
		LKM4CTR_INFO("vendor_kernel",
			     "native USER_NS core detected; using kernel uid/gid syscall translation");
	} else {
		hooked = shadow_hook_install_all(vendor_kernel_userns_hooks,
						 "vendor_kernel_userns");
		if (hooked < 0)
			LKM4CTR_WARN("vendor_kernel",
				     "failed to install uid/gid getter/setter syscall hooks (%d); unmapped global IDs may be exposed",
				     hooked);
		else
			total_hooked += hooked;
	}

	/*
	 * Best-effort: make sure /dev/mqueue is already a working mountpoint by
	 * the time this returns, instead of only reacting to a container's own
	 * mount(2) call that init.rc's boot-time attempt may already have
	 * failed before this (typically late-loaded) module was ever inserted.
	 * See glue/vendor_kernel_ipc_mount.c for the full rationale. Never
	 * allowed to fail vendor_kernel_init() itself.
	 *
	 * Deliberately called here, last, after every step above that can
	 * still return an error has already succeeded (vns_ipc_default_init()
	 * itself used to call this right before its own "return 0;", which is
	 * too early: any *subsequent* failure in this function -- e.g.
	 * vns_exit_hook_init(), either shadow_hook_install_all(), or
	 * vns_overlay_init() above -- would fail module initialization without
	 * running the normal module-exit path, leaving a mount whose callbacks
	 * point into code the loader is about to discard. This now only schedules
	 * delayed work; the worker waits until the module is LIVE, takes a module
	 * reference, and lets the public mqueue filesystem acquire its own owner
	 * reference. Scheduling is deliberately the last operation that can be
	 * reached after the other failure points, so failed initialization cannot
	 * leave the worker or mount behind. From here on,
	 * vendor_kernel_init() unconditionally succeeds.
	 */
	vns_nsproxy_stop_abort();
	vns_mqueue_dev_ensure();

	/* Open user-namespace creation only after every required hook is live. */
	vns_user_ns_stop_abort();
	vendor_kernel_enabled = true;
	LKM4CTR_INFO("vendor_kernel", "loaded (%d hook(s) installed)", total_hooked);
	if (!vns_pidns_runtime_supported)
		LKM4CTR_INFO("vendor_kernel",
			     "running kernel lacks real pid namespace core; CLONE_NEWPID/setns(pid) still perform real pid namespace isolation, with the CONFIG_PID_NS=n zap_pid_ns_processes() BUG defused at exit time (see vns_task_exit_cleanup)");
	return 0;
}

static int __vendor_kernel_exit(bool module_exit)
{
	unsigned int waited_ms = 0;
	bool was_quiescing;
	int err;

	if (!vendor_kernel_enabled)
		return 0;

	/*
	 * Keep lifecycle hooks live while existing vendored nsproxy objects
	 * drain. The gate prevents new namespace objects from being created,
	 * while the pre-quiesce count check ensures task-exit cleanup cannot be
	 * bypassed by the global hook quiesce below.
	 */
	vns_nsproxy_stop_begin();
	vns_user_ns_stop_begin();
	/* A task-exit callback may already have queued its final proxy put. */
	vns_nsproxy_deferred_flush();
	if (vns_nsproxy_live_count() > 0) {
		LKM4CTR_WARN("vendor_kernel",
			     "%s while %d vendored nsproxy object(s) remain live",
			     module_exit ? "module exit continuing" :
			     "refusing stop",
			     vns_nsproxy_live_count());
		if (!module_exit) {
			vns_nsproxy_stop_abort();
			vns_user_ns_stop_abort();
			return -EBUSY;
		}
	}
	vns_nsfs_lifecycle_begin_exit();
	while (!module_exit && shadow_hook_inflight_count() > 0 &&
	       waited_ms < 30000) {
		msleep(10);
		waited_ms += 10;
	}
	/* Include deferred puts queued by hooks that drained above. */
	vns_nsproxy_deferred_flush();
	err = vns_user_ns_prepare_exit();
	if (!err)
		err = vns_ipc_namespace_prepare_exit();
	if (shadow_hook_inflight_count() > 0 || vns_nsproxy_live_count() > 0 ||
	    err) {
		if (module_exit) {
			LKM4CTR_WARN("vendor_kernel",
				     "module exit continuing with %d hook call(s), %d vendored nsproxy object(s), %d user namespace object(s), %d IPC namespace object(s) reported",
				     shadow_hook_inflight_count(),
				     vns_nsproxy_live_count(),
				     vns_user_ns_live_count(),
				     vns_ipc_ns_live_count());
		} else {
			LKM4CTR_WARN("vendor_kernel",
				     "refusing stop before quiesce: %d hook call(s), %d vendored nsproxy object(s), %d user namespace object(s), %d IPC namespace object(s) remain or teardown work is pending",
				     shadow_hook_inflight_count(),
				     vns_nsproxy_live_count(),
				     vns_user_ns_live_count(),
				     vns_ipc_ns_live_count());
			vns_nsfs_lifecycle_abort_exit();
			vns_nsproxy_stop_abort();
			vns_user_ns_stop_abort();
			return err ? err : -EBUSY;
		}
	}

	was_quiescing = shadow_hook_is_quiescing();
	shadow_hook_quiesce(true);
	vns_mqueue_dev_cancel();
	while (!module_exit && shadow_hook_inflight_count() > 0 &&
	       waited_ms < 30000) {
		msleep(10);
		waited_ms += 10;
	}
	if (!module_exit && shadow_hook_inflight_count() > 0) {
		LKM4CTR_WARN("vendor_kernel",
			     "refusing stop with %d in-flight hook call(s)",
			     shadow_hook_inflight_count());
		vns_mqueue_dev_ensure();
		if (!was_quiescing)
			shadow_hook_quiesce(false);
		vns_nsfs_lifecycle_abort_exit();
		vns_nsproxy_stop_abort();
		vns_user_ns_stop_abort();
		return -EBUSY;
	}

	err = vns_procfs_compat_prepare_exit();
	if (err) {
		if (!module_exit) {
			vns_mqueue_dev_ensure();
			if (!was_quiescing)
				shadow_hook_quiesce(false);
			vns_nsfs_lifecycle_abort_exit();
			vns_nsproxy_stop_abort();
			vns_user_ns_stop_abort();
			return err;
		}
		LKM4CTR_WARN("vendor_kernel",
			     "module exit continuing despite procfs namespace inode pins (error %d)",
			     err);
	}

	err = vns_nsfs_lifecycle_prepare_exit();
	if (err) {
		if (!module_exit) {
			vns_mqueue_dev_ensure();
			if (!was_quiescing)
				shadow_hook_quiesce(false);
			vns_nsfs_lifecycle_abort_exit();
			vns_nsproxy_stop_abort();
			vns_user_ns_stop_abort();
			return err;
		}
		LKM4CTR_WARN("vendor_kernel",
			     "module exit continuing despite nsfs lifecycle prepare error %d",
			     err);
	}

	err = vns_sem_prepare_exit();
	if (!err)
		err = vns_shm_prepare_exit(vns_ipc_active_default());
	if (err) {
		LKM4CTR_WARN("vendor_kernel",
			     "%s while SysV semaphore undo state, shared-memory segments/mappings, or task-exit work remain (error %d)",
			     module_exit ? "module exit continuing" :
			     "refusing stop", err);
		if (!module_exit) {
			vns_mqueue_dev_ensure();
			if (!was_quiescing)
				shadow_hook_quiesce(false);
			vns_nsfs_lifecycle_abort_exit();
			vns_nsproxy_stop_abort();
			vns_user_ns_stop_abort();
			return err;
		}
	}

	err = vns_mqueue_fs_prepare_exit();
	if (err) {
		LKM4CTR_WARN("vendor_kernel",
			     "%s while mqueue filesystem contexts, mounts, or open files remain (error %d)",
			     module_exit ? "module exit continuing" :
			     "refusing stop", err);
		if (!module_exit) {
			vns_mqueue_dev_ensure();
			if (!was_quiescing)
				shadow_hook_quiesce(false);
			vns_nsfs_lifecycle_abort_exit();
			vns_nsproxy_stop_abort();
			vns_user_ns_stop_abort();
			return err;
		}
	}

	err = vns_procfs_compat_detach_init_pidns_ops();
	if (err) {
		if (!module_exit) {
			vns_mqueue_dev_ensure();
			if (!was_quiescing)
				shadow_hook_quiesce(false);
			vns_nsfs_lifecycle_abort_exit();
			vns_nsproxy_stop_abort();
			vns_user_ns_stop_abort();
			return err;
		}
		LKM4CTR_WARN("vendor_kernel",
			     "module exit continuing despite init pid namespace operations still being attached (error %d)",
			     err);
	}

	vendor_kernel_enabled = false;
	vns_overlay_exit();
	shadow_hook_remove_all(vendor_kernel_userns_hooks);
	shadow_hook_remove_all(vendor_kernel_procfs_hooks);
	vns_procfs_compat_lifecycle_exit();
	shadow_hook_remove_all(vendor_kernel_ipc_hooks);
	shadow_hook_remove_all(vendor_kernel_core_hooks);
	vns_exit_hook_exit();
	vns_nsfs_lifecycle_exit();
	if (module_exit) {
		/*
		 * module_exit cannot veto rmmod. The kernel has already accepted
		 * zero module references, so unregister every return hook rather
		 * than leave callbacks into unloaded module text if the diagnostic
		 * in-flight counter is stale.
		 */
		shadow_hook_teardown_all_retprobes();
	} else {
		/*
		 * Quiesce-exempt lifecycle hooks may have entered after the
		 * preflight snapshot. Hook removal closes admission; drain those
		 * already redirected before flushing deferred puts or clearing
		 * the TGID registry.
		 */
		while (shadow_hook_inflight_count() > 0)
			msleep(10);
	}
	vns_procfs_compat_clear_task_pid_map();
	if (module_exit && shadow_hook_inflight_count() > 0) {
		char hooks[512] = { 0 };

		shadow_hook_inflight_snprintf(hooks, sizeof(hooks));
		LKM4CTR_WARN("vendor_kernel",
			     "in-flight hook counter remains %d after final hook unregister: %s",
			     shadow_hook_inflight_count(), hooks);
	}
	vns_nsproxy_deferred_flush();
	vns_registry_clear_all();
	vns_nsproxy_deferred_flush();
	vns_user_ns_flush_work();
	vns_ipc_namespace_flush_work();
	vns_ipc_default_exit();
	LKM4CTR_INFO("vendor_kernel", "unloaded");
	if (!module_exit && !was_quiescing)
		shadow_hook_quiesce(false);
	return 0;
}

int vendor_kernel_exit(void)
{
	return __vendor_kernel_exit(false);
}

int vendor_kernel_module_exit(void)
{
	return __vendor_kernel_exit(true);
}
