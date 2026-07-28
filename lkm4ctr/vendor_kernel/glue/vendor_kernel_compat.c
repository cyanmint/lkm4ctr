// SPDX-License-Identifier: GPL-2.0
/*
 * vendor_kernel_compat.c - local stubs/resolvers for kernel-internal symbols
 * used by the vendored namespace files.
 *
 * This is NEW code (not vendored from kernel-common); it lives in glue/
 * alongside the other vendor_kernel-specific support files.
 *
 * Problem: the vendored kernel files (utsname.c, user_namespace.c, etc.)
 * call several kernel-internal symbols that are NOT exported via
 * EXPORT_SYMBOL/EXPORT_SYMBOL_GPL and are therefore invisible to out-of-
 * tree modules.  Rather than changing every call site in the vendored
 * files (which would inflate the diff against the upstream source), we
 * provide local definitions here that shadow the kernel versions for any
 * call within this module:
 *
 *   inc_ucount / dec_ucount       kernel/ucount.c  — not exported
 *   setup_userns_sysctls           kernel/user_namespace.c — not exported
 *   retire_userns_sysctls          kernel/user_namespace.c — not exported
 *   security_create_user_ns        security/security.c — not exported
 *   perf_event_namespaces          kernel/events/core.c — not exported
 *   setup_mq_sysctls               ipc/mqueue.c — not exported
 *   mq_clear_sbinfo                ipc/mqueue.c — not exported
 *   mq_put_mnt                     ipc/mqueue.c — not exported
 *   mq_lock (variable)             ipc/mqueue.c — not exported
 *   msg_init_ns                    ipc/msg.c — not exported
 *   from_mnt_ns                    fs/namespace.c — not exported
 *   pidfd_pid                      kernel/pid.c — not exported
 *
 * At vendor_kernel_init() time, each of these is resolved via
 * shadow_hook_resolve() so that, on the real Android GKI kernel where the
 * symbols ARE present (just not exported), the real kernel functions are
 * called through the function pointers stored here.  On the host build-
 * check kernel (where modpost would otherwise reject the undefined
 * references), the local definitions below satisfy the linker.
 *
 * Every stub is safe to call even when the resolved pointer is NULL:
 *   - ucounts stubs: return a static placeholder that passes null-checks;
 *     actual resource limit enforcement is skipped (acceptable for the
 *     parallel vendor namespace subsystem).
 *   - sysctl/perf/security stubs: no-ops or "allow-all" returns.
 *   - mqueue stubs: no-ops (mqueue VFS state lives in the real kernel).
 *   - from_mnt_ns: returns NULL (mount-ns setns is not implemented yet).
 *   - pidfd_pid: returns ERR_PTR(-EBADF) (fallback to proc_ns_file path).
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/user_namespace.h>
#include <linux/pid.h>
#include <linux/pid_namespace.h>
#include <linux/security.h>
#include <linux/perf_event.h>
#include <linux/ipc_namespace.h>
#include <linux/mnt_namespace.h>
#include <linux/ns_common.h>
#include <linux/sched.h>
#include <linux/err.h>
#include <linux/file.h>
#include <linux/fs_struct.h>
#include <linux/ptrace.h>
#include <linux/key.h>
#include <linux/time_namespace.h>
#include <linux/cgroup.h>
#include <linux/proc_fs.h>
#include <linux/sem.h>
#include <linux/cred.h>
#include <linux/capability.h>

#define VNS_COMPAT_IMPL
#include "../vendor_kernel.h"
#include "../include/uapi/vendor_kernel.h"
#include "../../../common/shadow_hook.h"
#include "../../../common/lkm4ctr_log.h"

/* ---- resolved function pointer types ---------------------------------- */

typedef struct ucounts *(*inc_ucount_fn_t)(struct user_namespace *, kuid_t,
					   enum ucount_type);
typedef void (*dec_ucount_fn_t)(struct ucounts *, enum ucount_type);
typedef bool (*setup_userns_sysctls_fn_t)(struct user_namespace *);
typedef void (*retire_userns_sysctls_fn_t)(struct user_namespace *);
typedef int  (*security_create_user_ns_fn_t)(const struct cred *);
typedef void (*perf_event_namespaces_fn_t)(struct task_struct *);
typedef bool (*setup_mq_sysctls_fn_t)(struct ipc_namespace *);
typedef struct ns_common *(*from_mnt_ns_fn_t)(struct mnt_namespace *);
typedef struct pid *(*pidfd_pid_fn_t)(const struct file *);
typedef void (*set_fs_root_fn_t)(struct fs_struct *, const struct path *);
typedef struct fs_struct *(*copy_fs_struct_fn_t)(struct fs_struct *);
typedef void (*set_fs_pwd_fn_t)(struct fs_struct *, const struct path *);
typedef void (*free_fs_struct_fn_t)(struct fs_struct *);
typedef bool (*ptrace_may_access_fn_t)(struct task_struct *, unsigned int);
typedef bool (*current_chrooted_fn_t)(void);
typedef void (*disable_pid_allocation_fn_t)(struct pid_namespace *);
typedef bool (*proc_ns_file_fn_t)(const struct file *);
typedef void (*retire_ipc_sysctls_fn_t)(struct ipc_namespace *);
typedef void (*retire_mq_sysctls_fn_t)(struct ipc_namespace *);
#ifdef CONFIG_KEYS
typedef void (*key_free_user_ns_fn_t)(struct user_namespace *);
#endif
typedef bool (*setup_ipc_sysctls_fn_t)(struct ipc_namespace *);
typedef int  (*set_cred_ucounts_fn_t)(struct cred *);
typedef struct cred *(*prepare_creds_fn_t)(void);
typedef int  (*commit_creds_fn_t)(struct cred *);
typedef bool (*file_ns_capable_fn_t)(const struct file *,
				     struct user_namespace *, int);
typedef void (*do_exit_fn_t)(long);
typedef pid_t (*pid_nr_ns_fn_t)(struct pid *, struct pid_namespace *);
#ifdef CONFIG_KEYS
typedef void (*key_put_fn_t)(struct key *);
#endif
typedef void (*kill_litter_super_fn_t)(struct super_block *);

static inc_ucount_fn_t            vns_inc_ucount_real;
static dec_ucount_fn_t            vns_dec_ucount_real;
static setup_userns_sysctls_fn_t  vns_setup_userns_sysctls_real;
static retire_userns_sysctls_fn_t vns_retire_userns_sysctls_real;
static security_create_user_ns_fn_t vns_security_create_user_ns_real;
static perf_event_namespaces_fn_t vns_perf_event_namespaces_real;
static setup_mq_sysctls_fn_t      vns_setup_mq_sysctls_real;
static from_mnt_ns_fn_t           vns_from_mnt_ns_real;
static pidfd_pid_fn_t             vns_pidfd_pid_real;
static set_fs_root_fn_t           vns_set_fs_root_real;
static copy_fs_struct_fn_t        vns_copy_fs_struct_real;
static set_fs_pwd_fn_t            vns_set_fs_pwd_real;
static free_fs_struct_fn_t        vns_free_fs_struct_real;
static ptrace_may_access_fn_t     vns_ptrace_may_access_real;
static current_chrooted_fn_t      vns_current_chrooted_real;
static disable_pid_allocation_fn_t vns_disable_pid_allocation_real;
static proc_ns_file_fn_t          vns_proc_ns_file_real;
static retire_ipc_sysctls_fn_t    vns_retire_ipc_sysctls_real;
static retire_mq_sysctls_fn_t     vns_retire_mq_sysctls_real;
#ifdef CONFIG_KEYS
static key_free_user_ns_fn_t      vns_key_free_user_ns_real;
#endif
static setup_ipc_sysctls_fn_t     vns_setup_ipc_sysctls_real;
static set_cred_ucounts_fn_t      vns_set_cred_ucounts_real;
static prepare_creds_fn_t         vns_prepare_creds_real;
static commit_creds_fn_t          vns_commit_creds_real;
static file_ns_capable_fn_t       vns_file_ns_capable_real;
static do_exit_fn_t               vns_do_exit_real;
static pid_nr_ns_fn_t              vns_pid_nr_ns_real;
#ifdef CONFIG_KEYS
static key_put_fn_t                vns_key_put_real;
#endif
static kill_litter_super_fn_t      vns_kill_litter_super_real;

/*
 * [BUILD-COMPAT] tasklist_lock (kernel/fork.c, not exported).
 * Protects the task list when pid_namespace.c walks processes during teardown.
 * This is a module-local lock; it does not protect the kernel's task list,
 * but satisfies the linker reference and provides mutual exclusion within
 * vendor_kernel's own teardown paths.
 */
DEFINE_RWLOCK(tasklist_lock);

/* Resolved pointer to the kernel's init_cgroup_ns data object, kept for
 * cosmetic bookkeeping only: it is never installed onto
 * vns_init_nsproxy.cgroup_ns, which always points at the module-owned
 * vns_default_cgroup_ns singleton (kernel/cgroup/namespace.c) instead --
 * mirroring vns_init_ipc_ns_ptr below. */
#ifdef CONFIG_CGROUPS
struct cgroup_namespace *vns_init_cgroup_ns_ptr;
#endif /* CONFIG_CGROUPS */

/* Resolved pointer to the *real* kernel's init_ipc_ns data object (best-effort;
 * NULL on vendor_kernel's primary CONFIG_SYSVIPC=n && CONFIG_POSIX_MQUEUE=n
 * target, where the vendor-owned vns_default_ipc_ns singleton is used instead).
 * Declared unconditionally so mqueue/sysvipc support is always compiled. */
struct ipc_namespace *vns_init_ipc_ns_ptr;

/* [BUILD-COMPAT] The real kernel's init_user_ns, captured (never resolved
 * via kallsyms/kprobe -- it is a data symbol, see vendor_kernel.h's
 * init_user_ns macro comment) from current_user_ns() at the very start of
 * vendor_kernel_init(). NULL until then. overflowgid has no real-kernel
 * dependency at all: it is just the standard kernel.overflowgid default. */
struct user_namespace *vns_real_init_user_ns;
const int vns_local_overflowgid = 65534;

/*
 * [BUILD-COMPAT] Module-owned kmem_cache pointers for the four namespace-
 * related structs (uts_namespace, nsproxy, pid_namespace, user_namespace).
 *
 * These used to be resolved from the *real* kernel's own private,
 * non-exported kmem_cache instances (uts_ns_cache, nsproxy_cachep,
 * pid_ns_cachep, user_ns_cachep) via shadow_hook_resolve(), on the theory
 * that vendor_kernel installs its vendored namespaces directly onto the
 * real task_struct->nsproxy (see vns_switch_task_namespaces()), so the
 * *real* kernel's own exit path (do_exit -> exit_task_namespaces ->
 * free_nsproxy -> free_uts_ns/__put_user_ns/put_pid_ns) would eventually
 * kmem_cache_free() these objects using the real kernel's own cache
 * pointers, and SLUB's cache_from_obj() would detect a kzalloc()-vs-real-
 * cache mismatch ("Wrong slab cache") and corrupt state otherwise.
 *
 * That resolution was fundamentally unreliable: these are non-exported
 * `struct kmem_cache *` *data* symbols, and shadow_hook_resolve() finds
 * symbols through register_kprobe(), which relies on kallsyms -- kallsyms
 * only carries function symbols unless CONFIG_KALLSYMS_ALL is set (almost
 * never true on production/GKI kernels). uts_ns_cache/pid_ns_cachep/
 * user_ns_cachep additionally do not exist in vmlinux at all whenever
 * CONFIG_UTS_NS/CONFIG_PID_NS/CONFIG_USER_NS is `n` (exactly the scenario
 * vendor_kernel targets). This made vendor_kernel fail to load with -ENOENT
 * on essentially every real device.
 *
 * Instead, these caches are now module-owned: created via
 * kmem_cache_create() in vns_uts_ns_init()/vns_nsproxy_cache_init()/
 * vns_pid_ns_init()/vns_user_ns_init(), matching the exact object layout
 * (so kmem_cache_alloc()/kmem_cache_zalloc() call sites are unchanged).
 * The real kernel's own exit path is prevented from ever touching a
 * module-owned object via vns_task_exit_cleanup() (kernel/nsproxy.c),
 * hooked onto the real do_exit() (glue/vendor_kernel_syscalls.c): before
 * the real do_exit() body runs, any exiting task whose task_struct->nsproxy
 * is a module-owned object (tracked in vns_nsproxy_set, kernel/nsproxy.c)
 * is swapped back onto the pinned vns_init_nsproxy singleton and the real
 * vendored object is torn down entirely by vendor_kernel itself
 * (vns_put_nsproxy()/vns_free_nsproxy(), which recurse into
 * vns_put_uts_ns()/vns_put_pid_ns()/vns_put_user_ns() -- all self-
 * contained, module-owned frees). See vendor_kernel/README.md, "Slab-cache
 * consistency with the real kernel".
 */
struct kmem_cache *vns_uts_ns_cache;
struct kmem_cache *vns_nsproxy_cachep;
struct kmem_cache *vns_pid_ns_cachep;
struct kmem_cache *vns_user_ns_cachep;

/*
 * Resolve all non-exported symbols at init time.  Called from
 * vendor_kernel_init() before any vendored namespace code runs.
 */
void vns_compat_resolve(void)
{
#define RESOLVE(var, sym) \
	do { \
		(var) = (typeof(var))(uintptr_t)shadow_hook_resolve(#sym); \
		if (!(var)) \
			LKM4CTR_WARN(VENDOR_KERNEL_TAG, \
				"compat: " #sym " not resolved (stub active)"); \
	} while (0)

	RESOLVE(vns_inc_ucount_real,            inc_ucount);
	RESOLVE(vns_dec_ucount_real,            dec_ucount);
	RESOLVE(vns_setup_userns_sysctls_real,  setup_userns_sysctls);
	RESOLVE(vns_retire_userns_sysctls_real, retire_userns_sysctls);
	RESOLVE(vns_security_create_user_ns_real, security_create_user_ns);
	RESOLVE(vns_perf_event_namespaces_real, perf_event_namespaces);
	RESOLVE(vns_setup_mq_sysctls_real,      setup_mq_sysctls);
	RESOLVE(vns_from_mnt_ns_real,           from_mnt_ns);
	RESOLVE(vns_pidfd_pid_real,             pidfd_pid);
	RESOLVE(vns_set_fs_root_real,           set_fs_root);
	RESOLVE(vns_copy_fs_struct_real,        copy_fs_struct);
	RESOLVE(vns_set_fs_pwd_real,            set_fs_pwd);
	RESOLVE(vns_free_fs_struct_real,        free_fs_struct);
	RESOLVE(vns_ptrace_may_access_real,     ptrace_may_access);
	RESOLVE(vns_current_chrooted_real,      current_chrooted);
	RESOLVE(vns_disable_pid_allocation_real, disable_pid_allocation);
	RESOLVE(vns_proc_ns_file_real,          proc_ns_file);
	RESOLVE(vns_retire_ipc_sysctls_real,    retire_ipc_sysctls);
	RESOLVE(vns_retire_mq_sysctls_real,     retire_mq_sysctls);
#ifdef CONFIG_KEYS
	RESOLVE(vns_key_free_user_ns_real,      key_free_user_ns);
#endif
	RESOLVE(vns_setup_ipc_sysctls_real,     setup_ipc_sysctls);
	RESOLVE(vns_set_cred_ucounts_real,      set_cred_ucounts);
	RESOLVE(vns_prepare_creds_real,         prepare_creds);
	RESOLVE(vns_commit_creds_real,          commit_creds);
	RESOLVE(vns_file_ns_capable_real,       file_ns_capable);
	RESOLVE(vns_do_exit_real,               do_exit);
	RESOLVE(vns_pid_nr_ns_real,             pid_nr_ns);
#ifdef CONFIG_KEYS
	RESOLVE(vns_key_put_real,               key_put);
#endif
	RESOLVE(vns_kill_litter_super_real,     kill_litter_super);
#ifdef CONFIG_CGROUPS
	/*
	 * Best-effort resolve of the *real* kernel's init_cgroup_ns. This is
	 * used for bookkeeping ONLY (see vns_init_cgroup_ns_ptr's declaration
	 * above); a failure to resolve it no longer disables cgroup namespace
	 * bookkeeping, since vns_init_nsproxy.cgroup_ns is always pointed at
	 * the vendored vns_default_cgroup_ns singleton instead (see
	 * vendor_kernel_init(), glue/vendor_kernel_module.c).
	 */
	vns_init_cgroup_ns_ptr = (struct cgroup_namespace *)(uintptr_t)
		shadow_hook_resolve("init_cgroup_ns");
	if (!vns_init_cgroup_ns_ptr)
		LKM4CTR_WARN(VENDOR_KERNEL_TAG,
			"compat: init_cgroup_ns not resolved (bookkeeping only; cgroup ns unaffected)");
#endif
	/*
	 * Best-effort resolve of the *real* kernel's init_ipc_ns. This is now
	 * used for bookkeeping ONLY -- specifically so vns_task_ipc_ns() can
	 * recognise and reject it (vns_ipc_ns_is_vendored()), guaranteeing the
	 * shadow SysV/mqueue handlers never operate on the running kernel's own
	 * ipc state. It is NEVER substituted for the vendored default: the
	 * module always uses vns_default_ipc_ns (see vendor_kernel_init()),
	 * regardless of whether this resolve succeeds. Succeeds only on a kernel
	 * that ships sysvipc/mqueue and exposes the symbol; NULL (the common
	 * case, and vendor_kernel's primary target) is perfectly fine.
	 */
	vns_init_ipc_ns_ptr = (struct ipc_namespace *)(uintptr_t)
		shadow_hook_resolve("init_ipc_ns");
	if (!vns_init_ipc_ns_ptr)
		LKM4CTR_INFO(VENDOR_KERNEL_TAG,
			"compat: real init_ipc_ns not resolved (expected on CONFIG_SYSVIPC=n/CONFIG_POSIX_MQUEUE=n); vendor-owned vns_default_ipc_ns is authoritative either way");
	/*
	 * [BUILD-COMPAT] uts_ns_cache/nsproxy_cachep/pid_ns_cachep/
	 * user_ns_cachep are NOT resolved here anymore. They are private
	 * `struct kmem_cache *` *data* symbols: shadow_hook_resolve() finds
	 * them through register_kprobe(), which in turn relies on kallsyms,
	 * and kallsyms only carries function symbols unless the running
	 * kernel was built with CONFIG_KALLSYMS_ALL (essentially never true
	 * on production/GKI kernels) -- so this resolution was guaranteed to
	 * fail on every real device, making vns_compat_ready() fail closed
	 * unconditionally. uts_ns_cache/pid_ns_cachep/user_ns_cachep also
	 * simply do not exist in vmlinux at all whenever the corresponding
	 * CONFIG_UTS_NS/CONFIG_PID_NS/CONFIG_USER_NS is `n` (kernel/Makefile
	 * gates utsname.o/pid_namespace.o/user_namespace.o on those
	 * options), which is exactly the scenario vendor_kernel targets.
	 * vns_uts_ns_cache/vns_nsproxy_cachep/vns_pid_ns_cachep/
	 * vns_user_ns_cachep are now module-owned kmem_cache_create() caches
	 * instead (created in vns_uts_ns_init()/vns_nsproxy_cache_init()/
	 * vns_pid_ns_init()/vns_user_ns_init()); see vendor_kernel/README.md,
	 * "Slab-cache consistency with the real kernel".
	 */

#undef RESOLVE
}

bool vns_compat_ready(void)
{
	bool ready = true;

	if (!vns_prepare_creds_real) {
		LKM4CTR_ERR(VENDOR_KERNEL_TAG,
			    "compat: prepare_creds unresolved; vendor_kernel unavailable");
		ready = false;
	}
	if (!vns_commit_creds_real) {
		LKM4CTR_ERR(VENDOR_KERNEL_TAG,
			    "compat: commit_creds unresolved; vendor_kernel unavailable");
		ready = false;
	}
	if (!vns_file_ns_capable_real) {
		LKM4CTR_ERR(VENDOR_KERNEL_TAG,
			    "compat: file_ns_capable unresolved; vendor_kernel unavailable");
		ready = false;
	}
	if (!vns_do_exit_real) {
		LKM4CTR_ERR(VENDOR_KERNEL_TAG,
			    "compat: do_exit unresolved; vendor_kernel unavailable");
		ready = false;
	}
	/*
	 * uts_ns_cache/nsproxy_cachep/pid_ns_cachep/user_ns_cachep are no
	 * longer resolved from the running kernel at all (see the comment in
	 * vns_compat_resolve() above); vns_uts_ns_cache/vns_nsproxy_cachep/
	 * vns_pid_ns_cachep/vns_user_ns_cachep are module-owned caches
	 * created by each subsystem's own _init() function during
	 * vendor_kernel_init(), so they are not part of this readiness gate.
	 */

	return ready;
}

/* ---- placeholder ucounts object --------------------------------------- */
/*
 * A static placeholder returned by inc_ucount() when the real function is
 * not available.  All dec_ucount() callers must test for this sentinel and
 * skip the real dec call.  Declared in vendor_kernel.h so vendored files can
 * reference it if needed (they currently do not).
 */
struct ucounts vns_ucounts_stub;

/* ---- local definitions of kernel-internal symbols -------------------- */

/*
 * [BUILD-COMPAT] inc_ucount / dec_ucount (kernel/ucount.c, not exported).
 * The real functions enforce per-user-namespace resource limits.  The stubs
 * below skip enforcement (always allow) so vendor_kernel namespaces can be
 * created even on kernels that restrict ucounts.  On kernels where the real
 * functions resolve, they are called instead.
 */
struct ucounts *vns_inc_ucount(struct user_namespace *ns, kuid_t uid,
			       enum ucount_type type)
{
	if (vns_inc_ucount_real)
		return vns_inc_ucount_real(ns, uid, type);
	/* stub: return non-NULL sentinel — no limit enforced */
	return &vns_ucounts_stub;
}

void vns_dec_ucount(struct ucounts *ucounts, enum ucount_type type)
{
	if (!ucounts || ucounts == &vns_ucounts_stub)
		return;
	if (vns_dec_ucount_real)
		vns_dec_ucount_real(ucounts, type);
}

/*
 * [BUILD-COMPAT] setup_userns_sysctls / retire_userns_sysctls
 * (kernel/user_namespace.c, not exported).
 * These register/unregister per-user-ns sysctl entries.  Skipping them
 * means /proc/sys/user/ entries for vendor namespaces are absent, which
 * is acceptable for a parallel namespace subsystem.
 */
bool vns_setup_userns_sysctls(struct user_namespace *ns)
{
	if (vns_setup_userns_sysctls_real)
		return vns_setup_userns_sysctls_real(ns);
	return true; /* stub: pretend success */
}

void vns_retire_userns_sysctls(struct user_namespace *ns)
{
	if (vns_retire_userns_sysctls_real)
		vns_retire_userns_sysctls_real(ns);
	/* stub: no-op */
}

/*
 * [BUILD-COMPAT] security_create_user_ns (security/security.c, not exported).
 * LSM hook that decides whether creating a new user namespace is allowed.
 * When the real hook is not available, we allow all creation (returns 0).
 * <linux/security.h> declares this as extern when CONFIG_SECURITY=y;
 * our definition satisfies in-module references and avoids modpost errors.
 */
int vns_security_create_user_ns(const struct cred *cred)
{
	if (vns_security_create_user_ns_real)
		return vns_security_create_user_ns_real(cred);
	return 0; /* stub: allow all */
}

/*
 * [BUILD-COMPAT] perf_event_namespaces (kernel/events/core.c, not exported).
 * Notification to the perf subsystem after setns().  Safe to skip.
 * <linux/perf_event.h> declares this as extern when CONFIG_PERF_EVENTS=y.
 */
#ifdef CONFIG_PERF_EVENTS
void vns_perf_event_namespaces(struct task_struct *tsk)
{
	if (vns_perf_event_namespaces_real)
		vns_perf_event_namespaces_real(tsk);
	/* stub: no-op */
}
#endif

/*
 * [BUILD-COMPAT] setup_mq_sysctls (ipc/mqueue.c, not exported).
 * Sets up per-ipc-ns mqueue sysctl table.  Skipping means mqueue sysctl
 * entries are absent for vendor IPC namespaces.
 * <linux/ipc_namespace.h> declares this as extern when CONFIG_POSIX_MQUEUE=y.
 */
bool vns_setup_mq_sysctls(struct ipc_namespace *ns)
{
	if (vns_setup_mq_sysctls_real)
		return vns_setup_mq_sysctls_real(ns);
	return true; /* stub */
}

/*
 * [BUILD-COMPAT] from_mnt_ns (fs/namespace.c, not exported).
 * Returns the ns_common embedded inside a mnt_namespace.  Used in
 * nsproxy.c's validate_nsset() when checking CLONE_NEWNS during setns.
 * Mount namespace support is not yet vendored, so setns(CLONE_NEWNS) is
 * intentionally rejected via this returning NULL.
 */
struct ns_common *vns_from_mnt_ns(struct mnt_namespace *mnt_ns)
{
	if (vns_from_mnt_ns_real)
		return vns_from_mnt_ns_real(mnt_ns);
	return NULL; /* stub: mount ns not vendored */
}

/*
 * [BUILD-COMPAT] pidfd_pid (kernel/pid.c, not exported on all kernels).
 * Returns the struct pid for a pidfd file, used in vns_sys_setns() to
 * identify the target namespace set from a process pidfd.
 * Falls back to ERR_PTR(-EBADF) if unresolved, causing setns to reject
 * pidfds (it will still work with /proc/<pid>/ns/<type> paths).
 */
struct pid *vns_pidfd_pid(const struct file *file)
{
	if (vns_pidfd_pid_real)
		return vns_pidfd_pid_real(file);
	return ERR_PTR(-EBADF); /* stub */
}

/*
 * [BUILD-COMPAT] free_time_ns (kernel/time/namespace.c, not exported).
 * put_time_ns() is a static inline in <linux/time_namespace.h> that calls
 * free_time_ns() when the refcount reaches zero.  Our vendored copy of
 * free_time_ns lives as vns_free_time_ns(); this shim forwards the call so
 * the inline can resolve.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 0)
void free_time_ns(struct kref *kref)
{
	vns_free_time_ns(container_of(kref, struct time_namespace, kref));
}
#else
void free_time_ns(struct time_namespace *ns)
{
	vns_free_time_ns(ns);
}
#endif

/*
 * [BUILD-COMPAT] set_fs_root (fs/fs_struct.c, not exported).
 * Updates the root path stored in a task's fs_struct.  Used by
 * vns_install_nsproxy() when switching namespaces.
 */
void vns_set_fs_root(struct fs_struct *fs, const struct path *path)
{
	if (vns_set_fs_root_real)
		vns_set_fs_root_real(fs, path);
	/* stub: root path unchanged — acceptable for the parallel ns subsystem */
}

/*
 * [BUILD-COMPAT] copy_fs_struct (fs/fs_struct.c, not exported).
 * Allocates a copy of the caller's fs_struct.  Used in copy_namespaces()
 * when the new namespace set needs an independent filesystem root.
 */
struct fs_struct *vns_copy_fs_struct(struct fs_struct *old)
{
	if (vns_copy_fs_struct_real)
		return vns_copy_fs_struct_real(old);
	return NULL; /* stub: no fs_struct copy — fs root stays shared */
}

/*
 * [BUILD-COMPAT] ptrace_may_access (kernel/ptrace.c, not exported on GKI).
 * Access-mode check used in vns_sys_setns() to gate cross-process ns changes.
 * Fall back to denying access if the real function is not resolved.
 */
bool vns_ptrace_may_access(struct task_struct *task, unsigned int mode)
{
	if (vns_ptrace_may_access_real)
		return vns_ptrace_may_access_real(task, mode);
	return false; /* stub: deny — safer than allow */
}

/*
 * [BUILD-COMPAT] disable_pid_allocation (kernel/pid.c, not exported).
 * Clears the PIDNS_ADDING flag so the pid namespace stops accepting new pids.
 * Called in vns_zap_pid_ns_processes() during pid namespace teardown.
 */
void vns_disable_pid_allocation(struct pid_namespace *ns)
{
	if (vns_disable_pid_allocation_real)
		vns_disable_pid_allocation_real(ns);
	/* stub: no-op — pids drain normally on process exit */
}

#ifdef CONFIG_KEYS
/*
 * [BUILD-COMPAT] key_free_user_ns (security/keys/user_defined.c, not exported).
 * Releases keyrings tied to a user_namespace.
 * <linux/key.h> already provides a no-op macro when !CONFIG_KEYS.
 */
void key_free_user_ns(struct user_namespace *ns)
{
	if (vns_key_free_user_ns_real)
		vns_key_free_user_ns_real(ns);
	/* stub: keyrings not freed — acceptable as they are ref-counted */
}
#endif /* CONFIG_KEYS */

/*
 * [BUILD-COMPAT] free_uts_ns (kernel/utsname.c, not exported).
 * put_uts_ns() is a static inline in <linux/utsname.h> that calls
 * free_uts_ns() when the refcount reaches zero.  Delegate to
 * vns_free_uts_ns() defined in our vendored utsname.c.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 0)
void free_uts_ns(struct kref *kref)
{
	vns_free_uts_ns(container_of(kref, struct uts_namespace, kref));
}
#else
void free_uts_ns(struct uts_namespace *ns)
{
	vns_free_uts_ns(ns);
}
#endif

/*
 * [BUILD-COMPAT] set_fs_pwd (fs/fs_struct.c, not exported).
 * Updates the current working directory in a task's fs_struct.
 */
void vns_set_fs_pwd(struct fs_struct *fs, const struct path *path)
{
	if (vns_set_fs_pwd_real)
		vns_set_fs_pwd_real(fs, path);
	/* stub: pwd unchanged */
}

/*
 * [BUILD-COMPAT] free_fs_struct (fs/fs_struct.c, not exported).
 * Releases an fs_struct allocated by copy_fs_struct().
 */
void vns_free_fs_struct(struct fs_struct *fs)
{
	if (vns_free_fs_struct_real)
		vns_free_fs_struct_real(fs);
	/* stub: no-op (minor leak acceptable for ns teardown) */
}

/*
 * [BUILD-COMPAT] current_chrooted (fs/fs_struct.c, not exported).
 * Returns true if the current task is in a chroot jail.
 * Used in user_namespace.c to gate unshare(CLONE_NEWUSER).
 */
bool vns_current_chrooted(void)
{
	if (vns_current_chrooted_real)
		return vns_current_chrooted_real();
	return false; /* stub: not chrooted — allow user namespace creation */
}

/*
 * [BUILD-COMPAT] proc_ns_file (fs/nsfs.c, not exported on GKI).
 * Returns true if the given file is a /proc/<pid>/ns/<ns> magic-link file.
 * Used in vns_sys_setns() to check whether the fd refers to a namespace.
 */
bool vns_proc_ns_file(const struct file *file)
{
	if (vns_proc_ns_file_real)
		return vns_proc_ns_file_real(file);
	return false; /* stub: treat as non-proc-ns file */
}

/*
 * [BUILD-COMPAT] retire_ipc_sysctls (ipc/sysctls.c, not exported).
 * Unregisters per-ipc-ns sysctl entries.
 * <linux/ipc_namespace.h> declares this when CONFIG_SYSCTL=y.
 */
void vns_retire_ipc_sysctls(struct ipc_namespace *ns)
{
	if (vns_retire_ipc_sysctls_real)
		vns_retire_ipc_sysctls_real(ns);
	/* stub: sysctl entries remain (harmless for a parallel ns subsystem) */
}

/*
 * [BUILD-COMPAT] retire_mq_sysctls (ipc/mqueue.c, not exported).
 * Unregisters per-ipc-ns mqueue sysctl entries.
 */
void vns_retire_mq_sysctls(struct ipc_namespace *ns)
{
	if (vns_retire_mq_sysctls_real)
		vns_retire_mq_sysctls_real(ns);
	/* stub: no-op */
}

/*
 * [BUILD-COMPAT] setup_ipc_sysctls (ipc/sysctls.c, not exported).
 * Registers per-ipc-ns sysctl table entries.
 * <linux/ipc_namespace.h> declares this as extern when CONFIG_SYSCTL=y.
 */
bool vns_setup_ipc_sysctls(struct ipc_namespace *ns)
{
	if (vns_setup_ipc_sysctls_real)
		return vns_setup_ipc_sysctls_real(ns);
	return true; /* stub: pretend success */
}

/*
 * [BUILD-COMPAT] set_cred_ucounts (kernel/cred.c, not exported).
 * Associates ucounts with a credentials struct during user_namespace creation.
 * Returns 0 on success.  Stub returns 0 (allow) when not resolved.
 */
int vns_set_cred_ucounts(struct cred *new)
{
	if (vns_set_cred_ucounts_real)
		return vns_set_cred_ucounts_real(new);
	return 0; /* stub: no ucounts tracking */
}

/*
 * [BUILD-COMPAT] prepare_creds / commit_creds / file_ns_capable / do_exit are
 * present in the kernel but may be unavailable to out-of-tree modules on GKI
 * because of symbol trimming. Resolve them via shadow_hook_resolve() at init
 * time and keep local wrappers here so the module never imports them directly.
 */
struct cred *vns_prepare_creds(void)
{
	if (vns_prepare_creds_real)
		return vns_prepare_creds_real();
	return NULL;
}

int vns_commit_creds(struct cred *new)
{
	if (vns_commit_creds_real)
		return vns_commit_creds_real(new);
	return -ENOENT;
}

bool vns_file_ns_capable(const struct file *file, struct user_namespace *ns,
			 int cap)
{
	if (vns_file_ns_capable_real)
		return vns_file_ns_capable_real(file, ns, cap);
	return false;
}

void __noreturn vns_do_exit(long error_code)
{
	if (vns_do_exit_real)
		vns_do_exit_real(error_code);
	LKM4CTR_ERR(VENDOR_KERNEL_TAG,
		    "compat: do_exit unresolved during runtime; aborting");
	BUG();
}

/*
 * [BUILD-COMPAT] pid_nr_ns (kernel/pid.c, not exported on some KMIs).
 * Translates a struct pid into the pid_t value seen from a given
 * pid_namespace. Called directly from our vendored ipc/shm.c and ipc/msg.c.
 */
pid_t vns_pid_nr_ns(struct pid *pid, struct pid_namespace *ns)
{
	if (vns_pid_nr_ns_real)
		return vns_pid_nr_ns_real(pid, ns);
	return 0; /* stub: report "no pid" rather than a bogus value */
}

#ifdef CONFIG_KEYS
/*
 * [BUILD-COMPAT] key_put (security/keys/key.c, not exported on some KMIs).
 * Drops a reference on a struct key. Called directly from our vendored
 * kernel/user_namespace.c.
 */
void vns_key_put(struct key *key)
{
	if (vns_key_put_real)
		vns_key_put_real(key);
	/* stub: leak the key rather than risk a bad refcount/UAF */
}
#endif /* CONFIG_KEYS */

/*
 * [BUILD-COMPAT] kill_litter_super (fs/super.c, "Protected symbol" -- EACCES
 * at insmod -- on some KMIs even though present/exported). Used as the
 * .kill_sb of our vendored ipc/mqueue.c's pseudo-filesystem. Falls back to
 * generic_shutdown_super() (always available) if unresolved: this skips
 * kill_litter_super()'s own d_genocide()/kill_anon_super() bookkeeping
 * (forced dentry eviction plus device-number release), but
 * generic_shutdown_super() alone already forcibly evicts the dcache for
 * this anon superblock, so the mount still tears down cleanly at the cost
 * of a harmless bdev-number leak in the rare case this fallback is hit.
 */
void vns_kill_litter_super(struct super_block *sb)
{
	if (vns_kill_litter_super_real) {
		vns_kill_litter_super_real(sb);
		return;
	}
	LKM4CTR_WARN(VENDOR_KERNEL_TAG,
		     "compat: kill_litter_super unresolved; using generic_shutdown_super() instead");
	generic_shutdown_super(sb);
}

#ifdef CONFIG_CGROUPS
/*
 * [BUILD-COMPAT] free_cgroup_ns (kernel/cgroup/namespace.c, not exported).
 * This symbol is referenced from inline helpers in <linux/cgroup.h> that are
 * parsed before vendor_kernel.h can remap the name. vendor_kernel never allocates its
 * own cgroup namespaces, so hitting this fallback would only mean a real
 * kernel cgroup namespace refcount unexpectedly dropped to zero through this
 * module; leak it rather than recurse back into put_cgroup_ns().
 */
void free_cgroup_ns(struct cgroup_namespace *ns)
{
	LKM4CTR_WARN(VENDOR_KERNEL_TAG,
		     "compat: unexpected free_cgroup_ns(%px); leaking cgroup namespace",
		     ns);
}
#endif

#ifdef CONFIG_NET_NS
/*
 * [BUILD-COMPAT] __put_net (net/core/net_namespace.c, not exported on some
 * KMIs). put_net()'s body is a static inline in <net/net_namespace.h> that
 * is parsed (and its call to __put_net() already resolved to whatever
 * symbol table entry the kernel provides) well before vendor_kernel.h's own
 * macro-remap definitions could take effect, so the usual
 * vns_xxx()+#define redirect trick cannot intercept this call. Providing
 * our own externally-linked __put_net() here satisfies put_net()'s
 * reference directly out of this module's own object files instead of
 * requiring it to be resolved from vmlinux. vendor_kernel never allocates
 * its own net namespaces (see vendor_kernel/kernel/nsproxy.c, which only
 * ever shares/gets a reference on a real net_ns), so hitting this fallback
 * would only mean a real kernel net namespace refcount unexpectedly
 * dropped to zero through this module; leak it rather than attempt to
 * replicate net namespace teardown ourselves.
 */
void __put_net(struct net *net)
{
	LKM4CTR_WARN(VENDOR_KERNEL_TAG,
		     "compat: unexpected __put_net(%px); leaking net namespace",
		     net);
}
#endif /* CONFIG_NET_NS */
