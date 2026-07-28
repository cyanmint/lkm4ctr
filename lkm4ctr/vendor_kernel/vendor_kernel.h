/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vendor_kernel.h - internal header for the vendor_kernel submodule.
 * vendor_kernel is NEW code (not vendored from kernel).
 */
#ifndef _VENDOR_KERNEL_H
#define _VENDOR_KERNEL_H

/*
 * Must be included before any other header: it #defines init_user_ns/
 * overflowgid/overflowuid, and several real kernel headers included below
 * (or transitively by this module's own .c files) have static inline
 * helpers that reference those bare names directly. See
 * vendor_kernel_data_syms.h for the full rationale.
 */
#include "glue/vendor_kernel_data_syms.h"

#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/version.h>
#include <linux/kref.h>
#include <linux/hashtable.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/refcount.h>
#include <linux/nsproxy.h>
#include <linux/proc_ns.h>
#include <linux/utsname.h>
#include <linux/pid_namespace.h>
#include <linux/user_namespace.h>
#include <linux/ipc_namespace.h>
#include <linux/time_namespace.h>
#include <linux/cgroup.h>
#include <linux/sched.h>
#include <linux/cred.h>
#include <linux/rcupdate.h>
#include <linux/spinlock.h>
#include <linux/fs.h>
#include <linux/fs_struct.h>
#include <linux/file.h>
#include <linux/ipc.h>
#include <linux/msg.h>
#include <linux/sem.h>
#include <linux/signal.h>
#include <net/net_namespace.h>
#include <uapi/linux/mqueue.h>
#include <uapi/linux/shm.h>
#include <uapi/linux/time_types.h>

struct vns_task;
struct shadow_hook;

/*
 * [BUILD-COMPAT] bsearch() (kernel/user_namespace.c's uid/gid map lookups)
 * is EXPORT_SYMBOL'd in every KMI we target, but may still be trimmed from
 * the running kernel's module symbol table (CONFIG_TRIM_UNUSED_KSYMS,
 * protected-KMI allow-lists), causing "Unknown symbol bsearch" at insmod.
 * <linux/bsearch.h> already ships a self-contained, always-inline
 * equivalent (__inline_bsearch()) purely to let callers avoid the external
 * call; redirect the bare name to it here instead of resolving the real
 * kernel symbol via shadow_hook_resolve(), since it needs no kernel-internal
 * state at all. Requires <linux/bsearch.h> to already be included at each
 * use site (kernel/user_namespace.c includes it before this header).
 */
#define bsearch(key, base, num, size, cmp) \
	__inline_bsearch((key), (base), (num), (size), (cmp))

/*
 * init_user_ns/overflowgid/overflowuid are redirected to module-owned
 * stand-ins by vendor_kernel_data_syms.h, included at the very top of this
 * file (see that header for the full rationale). NOTE: because of this,
 * init_user_ns must never be used in a static/global initializer (a
 * runtime pointer dereference isn't a compile-time constant) -- assign
 * such fields at runtime instead, once vns_real_init_user_ns has been set.
 * See ipc/msgutil.c's init_ipc_ns, kernel/time/namespace.c's
 * vns_init_time_ns and kernel/cgroup/namespace.c's vns_default_cgroup_ns,
 * whose .user_ns fields are populated in vendor_kernel_init() for exactly
 * this reason.
 */

#define VNS_TASK_HASH_BITS 10
#define VNS_CLONE_FLAGS ((unsigned long)(CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWNS | \
				CLONE_NEWPID | CLONE_NEWNET | CLONE_NEWUSER | \
				CLONE_NEWCGROUP | CLONE_NEWTIME))

struct vns_task {
	pid_t tgid;
	struct nsproxy *nsproxy;
	struct hlist_node node;
};

struct vns_registry {
	DECLARE_HASHTABLE(tasks, VNS_TASK_HASH_BITS);
	spinlock_t lock;
	unsigned long task_count;
	unsigned long stat_unshare;
	unsigned long stat_setns;
	unsigned long stat_clone;
};

extern struct vns_registry vendor_kernel_registry;

/*
 * vendored fs/overlayfs (vendor_kernel/fs/overlayfs/). vns_ovl_fs_type
 * uses the same name as upstream, "overlay", but is never
 * register_filesystem()'d directly: glue/vendor_kernel_overlay.c hooks
 * get_fs_type() and always hands this struct out for the name "overlay"
 * -- regardless of whether the running kernel has its own working
 * overlayfs -- so "mount -t overlay ..." always uses this vendored
 * implementation while lkm4ctr.ko is loaded. See fs/overlayfs/super.c.
 * Lifecycle is chained from vendor_kernel_init()/vendor_kernel_exit();
 * vns_ovl_mount_count feeds vendor_kernel's diagfs status
 * (glue/vendor_kernel_diag.c).
 */
extern atomic_t vns_ovl_mount_count;
extern struct file_system_type vns_ovl_fs_type;
int vns_ovl_init(void);
void vns_ovl_exit(void);
int vns_overlay_init(void);
void vns_overlay_exit(void);
size_t vns_overlay_diag_snprintf(char *buf, size_t buflen);

extern int (*vns_proc_alloc_inum_fn)(unsigned int *);
extern void (*vns_proc_free_inum_fn)(unsigned int);
extern struct mnt_namespace *(*vns_copy_mnt_ns_fn)(unsigned long, struct mnt_namespace *, struct user_namespace *, struct fs_struct *);
extern void (*vns_put_mnt_ns_fn)(struct mnt_namespace *);
extern struct net *(*vns_copy_net_ns_fn)(unsigned long, struct user_namespace *, struct net *);
/*
 * [BUILD-COMPAT] Real kernel's own (non-exported, non-static) free_nsproxy(),
 * resolved by name so a *foreign* (non-module-owned) struct nsproxy * whose
 * refcount we drop to zero (see vns_switch_task_namespaces() in
 * kernel/nsproxy.c) can be torn down through the real kernel's own path
 * instead of vendor_kernel's vns_free_nsproxy(), which assumes the object was
 * allocated from vns_nsproxy_cachep and would otherwise kmem_cache_free() a
 * real nsproxy_cachep object into the wrong cache (heap corruption).
 */
extern void (*vns_real_free_nsproxy_fn)(struct nsproxy *);
extern bool vendor_kernel_enabled;
extern bool vns_pidns_runtime_supported;
/*
 * [BUILD-COMPAT] Real kernel's own (non-static, non-exported) copy_pid_ns()
 * and (EXPORT_SYMBOL_GPL) put_pid_ns(), resolved by name so vns_copy_pid_ns()/
 * vns_put_pid_ns() (kernel/pid_namespace.c) can hand pid namespace creation
 * and teardown off to the real kernel's own implementation whenever the
 * running kernel already has real pid-namespace support (CONFIG_PID_NS=y).
 * Both resolve together (vns_pidns_runtime_supported tracks their combined
 * availability): on such a kernel, a module-owned pid_namespace (allocated
 * from vns_pid_ns_cachep) installed as some task's
 * nsproxy->pid_ns_for_children is still reached by the real, unconditional
 * alloc_pid()/free_pid() (kernel/pid.c), whose get_pid_ns()/put_pid_ns()
 * calls are real refcount ops (not no-ops) in that configuration -- so the
 * real put_pid_ns() eventually calls the real kernel's own
 * destroy_pid_namespace(), which kmem_cache_free()s the object against the
 * real, private pid_ns_cachep instead of vns_pid_ns_cachep, triggering
 * SLUB's "Wrong slab cache" warning (cache_from_obj() self-corrects the
 * free, so this is not a memory-corruption risk, but it is a real, avoidable
 * defect). Delegating entirely to the real copy_pid_ns()/put_pid_ns() in
 * this case (see vns_copy_pid_ns()) avoids the mismatch altogether.
 */
extern struct pid_namespace *(*vns_real_copy_pid_ns_fn)(unsigned long flags,
	struct user_namespace *user_ns, struct pid_namespace *old_ns);
extern void (*vns_real_put_pid_ns_fn)(struct pid_namespace *ns);

static inline void vns_count_set(void *count, int value, bool is_refcount)
{
	if (is_refcount)
		refcount_set((refcount_t *)count, value);
	else
		atomic_set((atomic_t *)count, value);
}

static inline bool vns_count_dec_and_test(void *count, bool is_refcount)
{
	if (is_refcount)
		return refcount_dec_and_test((refcount_t *)count);
	return atomic_dec_and_test((atomic_t *)count);
}

static inline void vns_count_inc(void *count, bool is_refcount)
{
	if (is_refcount)
		refcount_inc((refcount_t *)count);
	else
		atomic_inc((atomic_t *)count);
}

#define VNS_COUNT_TYPE_IS_REFCOUNT(ptr) \
	__builtin_types_compatible_p(typeof(*(ptr)), refcount_t)

#define vns_init_count(ptr, value) \
	vns_count_set((void *)(ptr), (value), VNS_COUNT_TYPE_IS_REFCOUNT(ptr))

#define vns_put_count(ptr) \
	vns_count_dec_and_test((void *)(ptr), VNS_COUNT_TYPE_IS_REFCOUNT(ptr))

#define vns_get_count(ptr) \
	vns_count_inc((void *)(ptr), VNS_COUNT_TYPE_IS_REFCOUNT(ptr))

/*
 * [BUILD-COMPAT] refcount_dec_and_lock() itself is not always exported /
 * present in a given GKI KMI's trimmed symbol table ("Unknown symbol
 * refcount_dec_and_lock (err -2)" observed at insmod on some KMIs), but its
 * upstream implementation (lib/refcount.c) is trivially reproducible from
 * ordinary always-available inline primitives (refcount_dec_and_test(),
 * spin_lock()/spin_unlock()) without needing to resolve the real symbol at
 * all. This is a faithful reimplementation of refcount_dec_and_lock(),
 * intentionally not calling the real kernel symbol by name.
 */
static inline bool vns_refcount_dec_and_lock(refcount_t *r, spinlock_t *lock)
{
	spin_lock(lock);
	if (!refcount_dec_and_test(r)) {
		spin_unlock(lock);
		return false;
	}
	return true;
}

static inline void vns_zero_stashed(struct ns_common *ns)
{
	memset(&ns->stashed, 0, sizeof(ns->stashed));
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 0)
#define vns_uts_init_ref(obj) vns_init_count(&(obj)->kref.refcount, 1)
#define vns_uts_get_ref(obj) vns_get_count(&(obj)->kref.refcount)
#define vns_uts_put_ref(obj) vns_put_count(&(obj)->kref.refcount)
#define vns_pid_init_ref(obj) vns_init_count(&(obj)->kref.refcount, 1)
#define vns_pid_get_ref(obj) vns_get_count(&(obj)->kref.refcount)
#define vns_pid_put_ref(obj) vns_put_count(&(obj)->kref.refcount)
#define vns_user_init_ref(obj) vns_init_count(&(obj)->count, 1)
#define vns_user_get_ref(obj) vns_get_count(&(obj)->count)
#define vns_user_put_ref(obj) vns_put_count(&(obj)->count)
#define vns_ipc_init_ref(obj) vns_init_count(&(obj)->count, 1)
#define vns_ipc_get_ref(obj) vns_get_count(&(obj)->count)
#define vns_ipc_put_ref_lock(obj, lock) vns_refcount_dec_and_lock(&(obj)->count, (lock))
#define vns_cgroupns_init_ref(obj, value) vns_init_count(&(obj)->count, (value))
#define VNS_TIME_REF_INIT .kref = KREF_INIT(1),
#else
#define vns_uts_init_ref(obj) vns_init_count(&(obj)->ns.count, 1)
#define vns_uts_get_ref(obj) vns_get_count(&(obj)->ns.count)
#define vns_uts_put_ref(obj) vns_put_count(&(obj)->ns.count)
#define vns_pid_init_ref(obj) vns_init_count(&(obj)->ns.count, 1)
#define vns_pid_get_ref(obj) vns_get_count(&(obj)->ns.count)
#define vns_pid_put_ref(obj) vns_put_count(&(obj)->ns.count)
#define vns_user_init_ref(obj) vns_init_count(&(obj)->ns.count, 1)
#define vns_user_get_ref(obj) vns_get_count(&(obj)->ns.count)
#define vns_user_put_ref(obj) vns_put_count(&(obj)->ns.count)
#define vns_ipc_init_ref(obj) vns_init_count(&(obj)->ns.count, 1)
#define vns_ipc_get_ref(obj) vns_get_count(&(obj)->ns.count)
#define vns_ipc_put_ref_lock(obj, lock) vns_refcount_dec_and_lock(&(obj)->ns.count, (lock))
#define vns_cgroupns_init_ref(obj, value) vns_init_count(&(obj)->ns.count, (value))
#define VNS_TIME_REF_INIT .ns.count = REFCOUNT_INIT(1),
#endif

int vns_alloc_inum(struct ns_common *ns);
void vns_free_inum(struct ns_common *ns);

struct uts_namespace *vns_copy_utsname(unsigned long flags, struct user_namespace *user_ns, struct uts_namespace *old_ns);
void vns_free_uts_ns(struct uts_namespace *ns);
/*
 * [BUILD-COMPAT] vns_get_uts_ns()/vns_put_uts_ns() are self-contained
 * replacements for the real kernel's get_uts_ns()/put_uts_ns(), which are
 * declared in <linux/utsname.h> as a real refcount_inc()/
 * refcount_dec_and_test()+free_uts_ns() pair when CONFIG_UTS_NS=y, or a
 * pair of plain no-ops when CONFIG_UTS_NS=n (same pattern as
 * get_pid_ns()/put_pid_ns() and get_user_ns()/put_user_ns() -- see
 * vendor_kernel/README.md, "Namespace refcounting is fully
 * self-contained"). Since vendor_kernel always vendors and installs its
 * own uts_namespace objects regardless of the target's CONFIG_UTS_NS, every
 * vendored call site uses these local equivalents instead.
 */
struct uts_namespace *vns_get_uts_ns(struct uts_namespace *ns);
void vns_put_uts_ns(struct uts_namespace *ns);
extern const struct proc_ns_operations vns_utsns_operations;
void vns_uts_ns_init(void);

extern struct nsproxy vns_init_nsproxy;
struct nsproxy *vns_copy_namespaces(unsigned long flags, struct task_struct *tsk);
void vns_free_nsproxy(struct nsproxy *ns);
void vns_put_nsproxy(struct nsproxy *ns);
int vns_unshare_nsproxy_namespaces(unsigned long unshare_flags, struct nsproxy **new_nsproxy, struct cred *new_cred, struct fs_struct *new_fs);
void vns_switch_task_namespaces(struct task_struct *p, struct nsproxy *new);
void vns_exit_task_namespaces(struct task_struct *p);
long vns_sys_setns(int fd, int flags);
void vns_nsproxy_cache_init(void);
/*
 * vns_task_exit_cleanup() - called from the do_exit() shadow_hook
 * (glue/vendor_kernel_syscalls.c) for every exiting task, before the real
 * do_exit() body runs. If @tsk->nsproxy is currently one of vendor_kernel's
 * own module-owned objects (tracked in vns_nsproxy_set, kernel/nsproxy.c),
 * swaps it back onto the pinned vns_init_nsproxy singleton and tears the
 * real vendored object down entirely through vendor_kernel's own
 * self-contained free path (vns_put_nsproxy()/vns_free_nsproxy()), so the
 * real kernel's own exit_task_namespaces()/free_nsproxy() never sees a
 * module-owned object and can never attempt to kmem_cache_free() it
 * against a real, mismatched kmem_cache. A no-op for any task that never
 * had a vendor_kernel namespace installed.
 */
void vns_task_exit_cleanup(struct task_struct *tsk);
int vns_exit_hook_init(void);
void vns_exit_hook_exit(void);
/*
 * vns_nsproxy_deferred_flush() - waits for every nsproxy teardown deferred
 * by vns_task_exit_cleanup() (kernel/nsproxy.c) to finish. Must be called
 * from vendor_kernel_exit() strictly after vns_exit_hook_exit() has
 * unregistered the do_exit() kprobe (so no further work can be queued),
 * and before the module image can be unloaded, or a still-pending
 * workqueue callback would execute code that has already been unmapped.
 */
void vns_nsproxy_deferred_flush(void);

struct ipc_namespace *vns_copy_ipcs(unsigned long flags, struct user_namespace *user_ns, struct ipc_namespace *old_ns);
void vns_put_ipc_ns(struct ipc_namespace *ns);
extern const struct proc_ns_operations vns_ipcns_operations;

struct cgroup_namespace *vns_copy_cgroup_ns(unsigned long flags, struct user_namespace *user_ns, struct cgroup_namespace *old_cgroup_ns);
void vns_put_cgroup_ns(struct cgroup_namespace *ns);
extern const struct proc_ns_operations vns_cgroupns_operations;
/*
 * vns_default_cgroup_ns / vns_cgroup_default_init() (kernel/cgroup/namespace.c)
 * -- module-owned default cgroup_namespace, always used as
 * vns_init_nsproxy.cgroup_ns regardless of whether the running kernel's own
 * init_cgroup_ns can be resolved, mirroring vns_default_ipc_ns/vns_init_time_ns.
 */
extern struct cgroup_namespace vns_default_cgroup_ns;
void vns_cgroup_default_init(void);

extern struct time_namespace vns_init_time_ns;
void vns_time_ns_default_init(void);
struct time_namespace *vns_copy_time_ns(unsigned long flags, struct user_namespace *user_ns, struct time_namespace *old_ns);
void vns_put_time_ns(struct time_namespace *ns);
void vns_free_time_ns(struct time_namespace *ns);
void vns_timens_commit(struct task_struct *tsk, struct time_namespace *ns);
void vns_timens_on_fork(struct nsproxy *nsproxy, struct task_struct *tsk);
void vns_proc_timens_show_offsets(struct task_struct *p, struct seq_file *m);
int vns_proc_timens_set_offset(struct file *file, struct task_struct *p, struct proc_timens_offset *offsets, int noffsets);
extern const struct proc_ns_operations vns_timens_operations;
extern const struct proc_ns_operations vns_timens_for_children_operations;

struct pid_namespace *vns_copy_pid_ns(unsigned long flags, struct user_namespace *user_ns, struct pid_namespace *old_ns);
/*
 * [BUILD-COMPAT] vns_get_pid_ns()/vns_put_pid_ns() are self-contained
 * replacements for the real kernel's get_pid_ns()/put_pid_ns(). Unlike the
 * real ones, they never depend on CONFIG_PID_NS: get_pid_ns() is always a
 * static inline in kernel headers (a real refcount_inc() when
 * CONFIG_PID_NS=y, a no-op when =n), and put_pid_ns() is an exported
 * extern function only when CONFIG_PID_NS=y (absent from vmlinux entirely,
 * not just unexported, when =n). Because vendor_kernel installs its own
 * struct pid_namespace objects and must refcount/free them correctly
 * regardless of the target kernel's CONFIG_PID_NS setting, every vendored
 * call site uses these local equivalents instead of get_pid_ns()/
 * put_pid_ns() directly (see vendor_kernel/README.md, "Namespace
 * refcounting is fully self-contained").
 */
struct pid_namespace *vns_get_pid_ns(struct pid_namespace *ns);
void vns_put_pid_ns(struct pid_namespace *ns);
void vns_zap_pid_ns_processes(struct pid_namespace *pid_ns);
int vns_reboot_pid_ns(struct pid_namespace *pid_ns, int cmd);
extern const struct proc_ns_operations vns_pidns_operations;
extern const struct proc_ns_operations vns_pidns_for_children_operations;
void vns_pid_ns_init(void);

int vns_create_user_ns(struct cred *new);
int vns_unshare_userns(unsigned long unshare_flags, struct cred **new_cred);
/*
 * [BUILD-COMPAT] vns_get_user_ns()/vns_put_user_ns() are self-contained
 * replacements for the real kernel's get_user_ns()/put_user_ns(). Both are
 * always static inline in kernel headers, but their bodies differ (real
 * refcounting + __put_user_ns() teardown when CONFIG_USER_NS=y, plain
 * no-ops returning init_user_ns when =n), and __put_user_ns() itself is an
 * exported extern function that is entirely absent from vmlinux when
 * CONFIG_USER_NS=n. vendor_kernel creates its own struct user_namespace
 * objects and must refcount/free them correctly regardless of the target
 * kernel's CONFIG_USER_NS setting, so every vendored call site uses these
 * local equivalents (which route to vns___put_user_ns() below) instead of
 * get_user_ns()/put_user_ns() directly.
 */
struct user_namespace *vns_get_user_ns(struct user_namespace *ns);
void vns_put_user_ns(struct user_namespace *ns);
void vns___put_user_ns(struct user_namespace *ns);
kuid_t vns_make_kuid(struct user_namespace *ns, uid_t uid);
uid_t vns_from_kuid(struct user_namespace *targ, kuid_t kuid);
uid_t vns_from_kuid_munged(struct user_namespace *targ, kuid_t kuid);
kgid_t vns_make_kgid(struct user_namespace *ns, gid_t gid);
gid_t vns_from_kgid(struct user_namespace *targ, kgid_t kgid);
gid_t vns_from_kgid_munged(struct user_namespace *targ, kgid_t kgid);
kprojid_t vns_make_kprojid(struct user_namespace *ns, projid_t projid);
projid_t vns_from_kprojid(struct user_namespace *targ, kprojid_t kprojid);
projid_t vns_from_kprojid_munged(struct user_namespace *targ, kprojid_t kprojid);
ssize_t vns_proc_uid_map_write(struct file *file, const char __user *buf, size_t size, loff_t *ppos);
ssize_t vns_proc_gid_map_write(struct file *file, const char __user *buf, size_t size, loff_t *ppos);
ssize_t vns_proc_projid_map_write(struct file *file, const char __user *buf, size_t size, loff_t *ppos);
int vns_proc_setgroups_show(struct seq_file *seq, void *v);
ssize_t vns_proc_setgroups_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos);
bool vns_userns_may_setgroups(const struct user_namespace *ns);
bool vns_in_userns(const struct user_namespace *ancestor, const struct user_namespace *child);
bool vns_current_in_userns(const struct user_namespace *target_ns);
struct ns_common *vns_ns_get_owner(struct ns_common *ns);
extern const struct proc_ns_operations vns_userns_operations;
void vns_user_ns_init(void);

void vns_nsfs_init(void);

struct vns_task *vns_task_find(pid_t tgid);
struct nsproxy *vns_current_nsproxy(void);
struct ipc_namespace *vns_task_ipc_ns(struct task_struct *task);
int vns_registry_set_nsproxy(pid_t tgid, struct nsproxy *nsproxy);
void vns_registry_remove(pid_t tgid);
void vns_registry_clone(pid_t parent_tgid, pid_t child_tgid);

static inline struct ipc_namespace *vns_current_ipc_ns(void)
{
	return vns_task_ipc_ns(current);
}

extern struct shadow_hook *vendor_kernel_core_hooks[];
extern struct shadow_hook *vendor_kernel_ipc_hooks[];
extern struct shadow_hook *vendor_kernel_procfs_hooks[];

/* compat layer (glue/vendor_kernel_compat.c) */
extern struct ucounts vns_ucounts_stub;
/*
 * vns_init_cgroup_ns_ptr is the best-effort resolved pointer to the *real*
 * kernel's init_cgroup_ns data object (resolved via shadow_hook_resolve() in
 * glue/vendor_kernel_compat.c), kept for cosmetic bookkeeping only -- mirroring
 * vns_init_ipc_ns_ptr below. It is never installed onto vns_init_nsproxy.cgroup_ns:
 * that field always points at the module-owned vns_default_cgroup_ns singleton
 * (kernel/cgroup/namespace.c), regardless of whether the running kernel's own
 * init_cgroup_ns resolves successfully.
 */
#ifdef CONFIG_CGROUPS
extern struct cgroup_namespace *vns_init_cgroup_ns_ptr;
#endif
/*
 * vns_init_ipc_ns_ptr is the best-effort resolved pointer to the *real*
 * kernel's init_ipc_ns data object (resolved via shadow_hook_resolve() in
 * glue/vendor_kernel_compat.c). It is only non-NULL on a target kernel that
 * genuinely ships sysvipc/mqueue (CONFIG_SYSVIPC=y or CONFIG_POSIX_MQUEUE=y)
 * AND exposes it through kallsyms. On vendor_kernel's primary target
 * (CONFIG_SYSVIPC=n && CONFIG_POSIX_MQUEUE=n) it is NULL and the vendor-owned
 * vns_default_ipc_ns singleton is used instead (see glue/vendor_kernel_module.c
 * and ipc/namespace.c). Declared unconditionally so mqueue/sysvipc support is
 * always compiled regardless of the target kernel's CONFIG_SYSVIPC/
 * CONFIG_POSIX_MQUEUE (mirroring the UTS_NS/PID_NS/USER_NS self-containment).
 */
extern struct ipc_namespace *vns_init_ipc_ns_ptr;
/*
 * vns_default_ipc_ns is vendor_kernel's own module-owned default ipc
 * namespace (defined in ipc/msgutil.c as the vendored init_ipc_ns object,
 * renamed via the #define below). It is fully initialized and its refcount
 * pinned at vendor_kernel_init() time (see vns_ipc_default_init()) so it can
 * serve as the fall-through ipc namespace for every task that never called
 * unshare(CLONE_NEWIPC), even on a kernel whose own init_nsproxy.ipc_ns is
 * NULL (CONFIG_IPC_NS=n).
 */
extern struct ipc_namespace vns_default_ipc_ns;
int vns_ipc_default_init(void);
void vns_ipc_default_exit(void);
struct ipc_namespace *vns_ipc_active_default(void);
/*
 * Module-owned kmem_cache pointers (created via kmem_cache_create() in
 * vns_uts_ns_init()/vns_nsproxy_cache_init()/vns_pid_ns_init()/
 * vns_user_ns_init(), NOT resolved from the running kernel). Vendored
 * namespace allocators use kmem_cache_alloc()/kmem_cache_zalloc() against
 * these instead of kzalloc()/kfree() purely to mirror upstream's
 * alloc-vs-zalloc semantics; the real kernel's own exit path is prevented
 * from ever touching a module-owned object at all (see
 * vns_task_exit_cleanup(), kernel/nsproxy.c).
 */
extern struct kmem_cache *vns_uts_ns_cache;
extern struct kmem_cache *vns_nsproxy_cachep;
extern struct kmem_cache *vns_pid_ns_cachep;
extern struct kmem_cache *vns_user_ns_cachep;
struct ucounts *vns_inc_ucount(struct user_namespace *ns, kuid_t uid,
			       enum ucount_type type);
void vns_dec_ucount(struct ucounts *ucounts, enum ucount_type type);
bool vns_setup_userns_sysctls(struct user_namespace *ns);
void vns_retire_userns_sysctls(struct user_namespace *ns);
void vns_compat_resolve(void);
void vns_ipc_compat_resolve(void);
/* glue/vendor_kernel_ipc_callbacks.c: resolves simple_lookup() and the
 * security_{msg_queue,sem,shm}_associate wrappers separately, in their own
 * CFI-instrumented translation unit -- see that file's header comment. */
void vns_ipc_lookup_resolve(void);
bool vns_compat_ready(void);
int vns_security_create_user_ns(const struct cred *cred);
void vns_perf_event_namespaces(struct task_struct *tsk);
bool vns_setup_mq_sysctls(struct ipc_namespace *ns);
void vns_mq_clear_sbinfo(struct ipc_namespace *ns);
void vns_mq_put_mnt(struct ipc_namespace *ns);
int vns_msg_init_ns(struct ipc_namespace *ns);
void vns_free_ipcs(struct ipc_namespace *ns, struct ipc_ids *ids,
		 void (*free)(struct ipc_namespace *, struct kern_ipc_perm *));
struct ns_common *vns_from_mnt_ns(struct mnt_namespace *mnt_ns);
struct pid *vns_pidfd_pid(const struct file *file);
void vns_set_fs_root(struct fs_struct *fs, const struct path *path);
struct fs_struct *vns_copy_fs_struct(struct fs_struct *old);
void vns_set_fs_pwd(struct fs_struct *fs, const struct path *path);
void vns_free_fs_struct(struct fs_struct *fs);
bool vns_ptrace_may_access(struct task_struct *task, unsigned int mode);
bool vns_current_chrooted(void);
void vns_disable_pid_allocation(struct pid_namespace *ns);
bool vns_proc_ns_file(const struct file *file);
void vns_retire_mq_sysctls(struct ipc_namespace *ns);
bool vns_setup_ipc_sysctls(struct ipc_namespace *ns);
void vns_retire_ipc_sysctls(struct ipc_namespace *ns);
int vns_set_cred_ucounts(struct cred *new);
struct cred *vns_prepare_creds(void);
int vns_commit_creds(struct cred *new);
bool vns_file_ns_capable(const struct file *file, struct user_namespace *ns,
			 int cap);
void __noreturn vns_do_exit(long error_code);
pid_t vns_pid_nr_ns(struct pid *pid, struct pid_namespace *ns);
#ifdef CONFIG_KEYS
void vns_key_put(struct key *key);
#endif
void vns_kill_litter_super(struct super_block *sb);
void vns_sem_init_ns(struct ipc_namespace *ns);
void vns_sem_exit_ns(struct ipc_namespace *ns);
void vns_shm_init_ns(struct ipc_namespace *ns);
void vns_shm_exit_ns(struct ipc_namespace *ns);
void vns_exit_sem(struct task_struct *tsk);
void vns_prepare_exit_sem(struct task_struct *tsk);
int vns_mq_init_ns(struct ipc_namespace *ns);
int vns_mqueue_fs_init(void);
void vns_mqueue_fs_exit(bool cache_teardown_unsafe);
void vns_mqueue_dev_ensure(void);
bool vns_mqueue_dev_teardown(void);
long vns_mq_open(const char __user *u_name, int oflag, umode_t mode,
		struct mq_attr __user *u_attr);
long vns_mq_unlink(const char __user *u_name);
long vns_mq_timedsend(mqd_t mqdes, const char __user *u_msg_ptr,
		     size_t msg_len, unsigned int msg_prio,
		     const struct __kernel_timespec __user *u_abs_timeout);
long vns_mq_timedreceive(mqd_t mqdes, char __user *u_msg_ptr, size_t msg_len,
			unsigned int __user *u_msg_prio,
			const struct __kernel_timespec __user *u_abs_timeout);
long vns_mq_notify(mqd_t mqdes, const struct sigevent __user *u_notification);
long vns_mq_getsetattr(mqd_t mqdes, const struct mq_attr __user *u_mqstat,
		      struct mq_attr __user *u_omqstat);
long vns_ksys_msgget(key_t key, int msgflg);
long vns_msgctl(int msqid, int cmd, struct msqid_ds __user *buf);
long vns_ksys_msgsnd(int msqid, struct msgbuf __user *msgp, size_t msgsz,
		    int msgflg);
long vns_ksys_msgrcv(int msqid, struct msgbuf __user *msgp, size_t msgsz,
		    long msgtyp, int msgflg);
int vns_copy_semundo(unsigned long clone_flags, struct task_struct *tsk);
void vns_msg_exit_ns(struct ipc_namespace *ns);
long vns_ksys_semget(key_t key, int nsems, int semflg);
long vns_semctl(int semid, int semnum, int cmd, unsigned long arg);
long vns_ksys_semtimedop(int semid, struct sembuf __user *tsops,
			unsigned int nsops,
			const struct __kernel_timespec __user *timeout);
long vns_ksys_shmget(key_t key, size_t size, int shmflg);
void vns_shm_destroy_orphaned(struct ipc_namespace *ns);
long vns_shmctl(int shmid, int cmd, struct shmid_ds __user *buf);
long vns_shmat(int shmid, char __user *shmaddr, int shmflg);
long vns_ksys_shmdt(char __user *shmaddr);
void vns_exit_shm(struct task_struct *task);
void vns_prepare_exit_shm(struct task_struct *task);
bool vns_is_file_shm_hugepages(struct file *file);

#ifndef VNS_COMPAT_IMPL
#define inc_ucount vns_inc_ucount
#define dec_ucount vns_dec_ucount
#define setup_userns_sysctls vns_setup_userns_sysctls
#define retire_userns_sysctls vns_retire_userns_sysctls
#define security_create_user_ns vns_security_create_user_ns
#define perf_event_namespaces vns_perf_event_namespaces
#define setup_mq_sysctls vns_setup_mq_sysctls
#define mq_clear_sbinfo vns_mq_clear_sbinfo
#define mq_put_mnt vns_mq_put_mnt
#define msg_init_ns vns_msg_init_ns
#define init_ipc_ns vns_default_ipc_ns
#define from_mnt_ns vns_from_mnt_ns
#define pidfd_pid vns_pidfd_pid
#define set_fs_root vns_set_fs_root
#define copy_fs_struct vns_copy_fs_struct
#define set_fs_pwd vns_set_fs_pwd
#define free_fs_struct vns_free_fs_struct
#define ptrace_may_access vns_ptrace_may_access
#define current_chrooted vns_current_chrooted
#define disable_pid_allocation vns_disable_pid_allocation
#define proc_ns_file vns_proc_ns_file
#define retire_mq_sysctls vns_retire_mq_sysctls
#define sem_init_ns vns_sem_init_ns
#define sem_exit_ns vns_sem_exit_ns
#define shm_init_ns vns_shm_init_ns
#define shm_exit_ns vns_shm_exit_ns
#define exit_sem vns_exit_sem
#define copy_semundo vns_copy_semundo
#define setup_ipc_sysctls vns_setup_ipc_sysctls
#define retire_ipc_sysctls vns_retire_ipc_sysctls
#define set_cred_ucounts vns_set_cred_ucounts
#define prepare_creds vns_prepare_creds
#define commit_creds vns_commit_creds
#define file_ns_capable vns_file_ns_capable
#define do_exit vns_do_exit
#define pid_nr_ns vns_pid_nr_ns
#ifdef CONFIG_KEYS
#define key_put vns_key_put
#endif
#define kill_litter_super vns_kill_litter_super
#define msg_exit_ns vns_msg_exit_ns
#define shm_destroy_orphaned vns_shm_destroy_orphaned
#define exit_shm vns_exit_shm
#define is_file_shm_hugepages vns_is_file_shm_hugepages
#ifdef CONFIG_USER_NS
#define in_userns vns_in_userns
#endif
#define mq_init_ns vns_mq_init_ns
#ifdef CONFIG_CGROUPS
#define free_cgroup_ns vns_free_cgroup_ns
#endif
#endif

int vendor_kernel_init(void);
void vendor_kernel_exit(void);
size_t vendor_kernel_diag_snprintf(char *buf, size_t buflen);

#endif /* _VENDOR_KERNEL_H */
