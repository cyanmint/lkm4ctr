// SPDX-License-Identifier: GPL-2.0
/*
 * shadow_hijack - shared hook engine linked into the unified lkm4ctr.ko.
 *
 * Background
 * ----------
 * The five entry points exported here (shadow_hook_resolve,
 * shadow_hook_install, shadow_hook_remove, shadow_hook_install_all,
 * shadow_hook_remove_all) used to be `static inline` helpers in
 * common/shadow_hook.h, duplicated into every module TU that needed them so
 * that no shared .ko was required. Now that more than one subsystem hooks
 * syscalls (vendor_kernel and shadow_cgdevices), that duplication is
 * wasteful and, more importantly, made
 * the recursion guard fragile (see below). The logic now lives here as a
 * single shared implementation; common/shadow_hook.h is a purely declarative
 * ABI header that both this module and its callers agree on.
 *
 * This subsystem has no runtime state of its own: its init/exit are no-ops
 * called from the unified lkm4ctr.ko entry/exit sequencer. Callers describe
 * each hooked symbol with a `struct shadow_hook` and hand it to
 * shadow_hook_install()/remove().
 *
 * Backend selection
 * -----------------
 * The hooking backend is chosen at *compile time* from what the target
 * kernel's own Kconfig enables:
 *   - the ftrace_ops/IPMODIFY backend when CONFIG_FUNCTION_TRACER and
 *     CONFIG_DYNAMIC_FTRACE are available; otherwise
 *   - a kprobe pre_handler backend that redirects control flow by rewriting
 *     the trapped pt_regs program counter and returning 1.
 * Several "certified"/production Android GKI boot images ship with
 * CONFIG_FUNCTION_TRACER compiled out entirely, so the kprobe backend is what
 * actually runs on stock GKI; CONFIG_KPROBES is a hard requirement of the
 * wider KernelSU/SUSFS ecosystem and is effectively always present. Both
 * backends expose the identical shadow_hook_install/remove API, so callers do
 * not know or care which one is active.
 *
 * The owner-based recursion guard
 * -------------------------------
 * A hook sits at the very first instruction of the target function, so it also
 * fires when *our own* replacement (hook->function) calls through
 * hook->original -- which resolves to the same hooked address -- to invoke
 * genuine kernel behaviour. Without a guard, that pass-through call would be
 * redirected straight back into hook->function, recursing until the kernel
 * stack overflows. The guard detects the pass-through by checking whether the
 * caller's return address lies within the module that owns the hook.
 *
 * When this logic was `static inline` and compiled into each caller,
 * THIS_MODULE naturally referred to that caller. In the merged build every
 * caller and the hook engine all live inside the same lkm4ctr.ko, so
 * hook->owner simply resolves to that unified module. The guard therefore
 * checks within_module(caller_pc, hook->owner), which is enough to recognise
 * the pass-through call. This is applied identically in both backends (the
 * ftrace thunk and the kprobe pre_handler).
 *
 * rmmod safety
 * ------------
 * Neither backend's "unregister" call waits for calls already redirected
 * into hook->function to finish executing, so an rmmod running concurrently
 * with such a call could previously free the module's memory out from under
 * it -- typically surfacing as a kernel panic some time (not necessarily
 * immediately) after rmmod, once the freed pages are reused or something
 * else runs into them. Every hook now also carries a kretprobe placed on
 * hook->function itself; the redirect points (shadow_hook_thunk(),
 * shadow_hook_pre_handler()) take a module reference with
 * try_module_get(hook->owner) immediately before redirecting, and the
 * kretprobe's return handler drops it once hook->function actually
 * returns. This keeps module_refcount() non-zero for exactly as long as any
 * redirected call is in flight anywhere in the system, so the kernel's own
 * sys_delete_module() refuses rmmod (-EBUSY, "Module ... is in use")
 * instead of racing with it -- the same protection struct
 * file_operations::owner gives an ordinary char/misc device while one of
 * its files is open. See the struct shadow_hook comment in
 * common/shadow_hook.h for the full rationale.
 */

#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/ftrace.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/atomic.h>

#include "shadow_hook.h"
#include "lkm4ctr_log.h"

#define SHADOW_HIJACK_VERSION	"1.0"

/*
 * shadow_hook_registry - a global, tag-keyed record of every hook group a
 * subsystem has handed to shadow_hook_install_all(), kept purely so the
 * lkm4ctr diagfs (lkm4ctr_diagfs.c) can render "what hooks are currently
 * loaded, in which submodule" without each subsystem needing its own
 * bespoke introspection API, and so the diagfs's per-module "status" files
 * can force a submodule's hooks to load/unload at runtime
 * (shadow_hook_registry_set_active()) without needing a dedicated
 * force-load/unload entry point in every subsystem.
 *
 * Unlike the registry's first revision, nodes are never freed once created:
 * a subsystem's hook array is a static file-scope array for the entire
 * life of the module, so the pointer (and the node describing it) remains
 * a valid, reusable handle across repeated shadow_hook_install_all()/
 * shadow_hook_remove_all() cycles -- exactly what runtime force-load/unload
 * needs. @active tracks whether the group's hooks are currently installed.
 */
struct shadow_hook_registry_group {
	const char		*tag;
	struct shadow_hook	**hooks;
	bool			active;
	struct list_head	list;
};

static LIST_HEAD(shadow_hook_registry_groups);
static DEFINE_MUTEX(shadow_hook_registry_lock);

/*
 * shadow_hook_inflight - count of currently in-flight redirected calls,
 * incremented alongside every successful try_module_get(hook->owner) at
 * each redirect point (shadow_hook_thunk() x2, shadow_hook_pre_handler())
 * and decremented in shadow_hook_retprobe_ret() alongside the matching
 * module_put(). Exists purely for diagnostics -- see the comment on
 * shadow_hook_inflight_count() in common/shadow_hook.h.
 */
static atomic_t shadow_hook_inflight = ATOMIC_INIT(0);

int shadow_hook_inflight_count(void)
{
	return atomic_read(&shadow_hook_inflight);
}
EXPORT_SYMBOL_GPL(shadow_hook_inflight_count);

/* Caller must hold shadow_hook_registry_lock. */
static struct shadow_hook_registry_group *
shadow_hook_registry_find_locked(struct shadow_hook **hooks)
{
	struct shadow_hook_registry_group *g;

	list_for_each_entry(g, &shadow_hook_registry_groups, list) {
		if (g->hooks == hooks)
			return g;
	}
	return NULL;
}

static void shadow_hook_registry_add(const char *tag, struct shadow_hook **hooks)
{
	struct shadow_hook_registry_group *g;

	mutex_lock(&shadow_hook_registry_lock);
	g = shadow_hook_registry_find_locked(hooks);
	if (g) {
		g->active = true;
		mutex_unlock(&shadow_hook_registry_lock);
		return;
	}

	mutex_unlock(&shadow_hook_registry_lock);
	g = kzalloc(sizeof(*g), GFP_KERNEL);
	if (!g)
		return;
	g->tag = tag;
	g->hooks = hooks;
	g->active = true;

	mutex_lock(&shadow_hook_registry_lock);
	/* Re-check: another caller may have raced us in while unlocked. */
	if (shadow_hook_registry_find_locked(hooks)) {
		kfree(g);
	} else {
		list_add_tail(&g->list, &shadow_hook_registry_groups);
	}
	mutex_unlock(&shadow_hook_registry_lock);
}

static void shadow_hook_registry_remove(struct shadow_hook **hooks)
{
	struct shadow_hook_registry_group *g;

	mutex_lock(&shadow_hook_registry_lock);
	g = shadow_hook_registry_find_locked(hooks);
	if (g)
		g->active = false;
	mutex_unlock(&shadow_hook_registry_lock);
}

/*
 * shadow_hook_registry_tag_matches() - whether @group_tag (e.g.
 * "vendor_kernel_procfs") belongs to the @filter subsystem (e.g.
 * "vendor_kernel"): either an exact match, or @filter followed by '_' as a
 * prefix. vendor_kernel registers several hook groups under
 * related-but-distinct tags (its own "vendor_kernel" core plus
 * "vendor_kernel_procfs"/"_syscalls"/"_ipc"), so the diagfs's single
 * per-module "hooks" file needs every one of them when asked for
 * "vendor_kernel", not just an exact-string match.
 */
static bool shadow_hook_registry_tag_matches(const char *filter, const char *group_tag)
{
	size_t len = strlen(filter);

	if (strncmp(filter, group_tag, len))
		return false;
	return group_tag[len] == '\0' || group_tag[len] == '_';
}

/*
 * shadow_hook_registry_snprintf() - render every hook belonging to @tag (or
 * every hook in every group, if @tag is NULL) as one "resolved_name
 * installed=yes/no address=0x...\n" line per hook into @buf (size @buflen).
 * Returns the number of bytes that would have been written (snprintf()
 * semantics), for lkm4ctr_diagfs.c's per-module "hooks" files.
 */
size_t shadow_hook_registry_snprintf(const char *tag, char *buf, size_t buflen)
{
	struct shadow_hook_registry_group *g;
	size_t pos = 0;

	mutex_lock(&shadow_hook_registry_lock);
	list_for_each_entry(g, &shadow_hook_registry_groups, list) {
		int i;

		if (tag && !shadow_hook_registry_tag_matches(tag, g->tag))
			continue;

		for (i = 0; g->hooks[i]; i++) {
			struct shadow_hook *h = g->hooks[i];
			const char *name = h->resolved_name ? h->resolved_name :
				(h->names && h->names[0] ? h->names[0] : "?");

			pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
					  "%s: %-32s installed=%s address=0x%lx\n",
					  g->tag, name, h->installed ? "yes" : "no",
					  h->address);
		}
	}
	mutex_unlock(&shadow_hook_registry_lock);

	return pos;
}
EXPORT_SYMBOL_GPL(shadow_hook_registry_snprintf);

/*
 * shadow_hook_registry_tag_active() - whether a subsystem tagged @tag has a
 * currently-registered hook group, i.e. whether that submodule successfully
 * made it through shadow_hook_install_all() and has not since called
 * shadow_hook_remove_all() (nor been force-unloaded via
 * shadow_hook_registry_set_active()). Used by lkm4ctr_diagfs.c's per-module
 * "status" files as the "activated" flag requested for the diagnostics
 * tree.
 */
bool shadow_hook_registry_tag_active(const char *tag)
{
	struct shadow_hook_registry_group *g;
	bool found = false;

	mutex_lock(&shadow_hook_registry_lock);
	list_for_each_entry(g, &shadow_hook_registry_groups, list) {
		if (!strcmp(g->tag, tag)) {
			found = g->active;
			break;
		}
	}
	mutex_unlock(&shadow_hook_registry_lock);

	return found;
}
EXPORT_SYMBOL_GPL(shadow_hook_registry_tag_active);

/*
 * shadow_hook_registry_set_active() - force every hook group whose tag
 * matches @tag (see shadow_hook_registry_tag_matches()) to be installed
 * (@enable true) or removed (@enable false), if it is not already in that
 * state. This is what the lkm4ctr diagfs's per-module "status" files use
 * for `echo load`/`echo unload`.
 *
 * Matching groups are first snapshotted (tag + hooks pointer) under
 * shadow_hook_registry_lock, then acted on with the lock released: both
 * shadow_hook_install_all() and shadow_hook_remove_all() call back into
 * shadow_hook_registry_add()/_remove() above, which take the same mutex
 * themselves, so calling them while already holding it would deadlock.
 * A fixed-size on-stack snapshot array is enough here -- no subsystem
 * currently registers more than a handful of hook groups.
 *
 * Every step is logged verbosely via LKM4CTR_LOG (tagged with @tag) so
 * that a "why didn't this take effect?" question is always answerable
 * from ./mnt/<subsystem>/log alone: which groups matched, which were
 * already in the requested state, and -- on failure -- exactly which
 * group and underlying shadow_hook_install_all() error blocked it, plus a
 * concrete next step.
 *
 * Returns 0 on success (including the case where @tag matches nothing, or
 * every matching group is already in the requested state -- both are
 * treated as a no-op, matching a status file for a submodule with no
 * runtime-hookable syscalls on this kernel), or the first non-ENOENT
 * error shadow_hook_install_all() reports for any matching group.
 */
#define SHADOW_HOOK_REGISTRY_SNAPSHOT_MAX 16

static int shadow_hook_group_count(struct shadow_hook **hooks)
{
	int n = 0;

	while (hooks[n])
		n++;
	return n;
}

int shadow_hook_registry_set_active(const char *tag, bool enable)
{
	struct shadow_hook_registry_group *g;
	struct shadow_hook_registry_group *snapshot[SHADOW_HOOK_REGISTRY_SNAPSHOT_MAX];
	int i, n = 0, ret = 0;
	bool any_group_for_tag = false;

	mutex_lock(&shadow_hook_registry_lock);
	list_for_each_entry(g, &shadow_hook_registry_groups, list) {
		if (!shadow_hook_registry_tag_matches(tag, g->tag))
			continue;
		any_group_for_tag = true;
		if (g->active == enable) {
			LKM4CTR_INFO(tag, "hook group \"%s\" is already %s, nothing to do",
				     g->tag, enable ? "active" : "inactive");
			continue;
		}
		if (n < SHADOW_HOOK_REGISTRY_SNAPSHOT_MAX)
			snapshot[n++] = g;
	}
	mutex_unlock(&shadow_hook_registry_lock);

	if (!any_group_for_tag) {
		LKM4CTR_WARN(tag,
			     "no hook group is registered under \"%s\"; this submodule either was never initialized on this kernel (e.g. it detected genuine native kernel support and never needed to hook anything) or the tag is unknown -- %s request is a no-op",
			     tag, enable ? "load" : "unload");
		return 0;
	}

	for (i = 0; i < n; i++) {
		int count = shadow_hook_group_count(snapshot[i]->hooks);

		if (enable) {
			int installed;

			LKM4CTR_INFO(tag, "loading hook group \"%s\" (%d hook(s) to attempt)",
				     snapshot[i]->tag, count);
			installed = shadow_hook_install_all(snapshot[i]->hooks, snapshot[i]->tag);
			if (installed < 0) {
				LKM4CTR_ERR(tag, "failed to load hook group \"%s\": %d",
					    snapshot[i]->tag, installed);
				LKM4CTR_ERR(tag,
					    "cause: see the \"failed to install hook[N]\" line just above this one in this same log for the exact hook and the underlying error");
				LKM4CTR_ERR(tag,
					    "resolution: common causes are the resolved symbol no longer matching the expected prototype on this kernel build, or the ftrace/kprobe backend rejecting an already-hooked address (only one shadow_hook may own a given symbol at a time)");
				if (!ret)
					ret = installed;
				continue;
			}
			LKM4CTR_INFO(tag, "hook group \"%s\" loaded: %d/%d hook(s) installed (the rest, if any, had no matching symbol on this kernel and were skipped -- see \"symbol for hook[N] not found\" lines above)",
				     snapshot[i]->tag, installed, count);
		} else {
			LKM4CTR_INFO(tag, "unloading hook group \"%s\" (%d hook(s))",
				     snapshot[i]->tag, count);
			shadow_hook_remove_all(snapshot[i]->hooks);
			LKM4CTR_INFO(tag, "hook group \"%s\" unloaded", snapshot[i]->tag);
		}
	}

	return ret;
}
EXPORT_SYMBOL_GPL(shadow_hook_registry_set_active);

/*
 * shadow_hook_resolve - find the runtime address of a kernel symbol.
 *
 * Uses the register_kprobe()/unregister_kprobe() trick so we don't depend on
 * the exported-ness of kallsyms_lookup_name() (largely unexported since Linux
 * 5.7). register_kprobe() internally resolves kp.addr from kp.symbol_name
 * using the kernel's own symbol table walker; we immediately unregister the
 * (never armed for our purposes) kprobe and reuse the resolved address.
 * Returns 0 if not found.
 *
 * Note: this only works if register_kprobe()/unregister_kprobe() themselves
 * are available -- i.e. CONFIG_KPROBES=y in the target kernel. Since these
 * are the very primitives this resolver is built on, an "Unknown symbol
 * register_kprobe"/"unregister_kprobe" error at insmod cannot be worked
 * around by resolving them the same way (a bootstrapping/chicken-and-egg
 * problem). CONFIG_KPROBES is effectively mandatory for any real Android
 * GKI kernel (ftrace/perfetto tracing infrastructure depends on it), so
 * this indicates the target kernel's own config lacks CONFIG_KPROBES
 * (e.g. a minimal test kernel), not a normal GKI KMI symbol-list-trimming
 * issue -- there is no supported fix for that case short of enabling
 * CONFIG_KPROBES in the target kernel build.
 */
unsigned long shadow_hook_resolve(const char *name)
{
	struct kprobe kp;
	unsigned long addr;
	int ret;

	memset(&kp, 0, sizeof(kp));
	kp.symbol_name = name;

	ret = register_kprobe(&kp);
	if (ret < 0)
		return 0;

	addr = (unsigned long)kp.addr;
	unregister_kprobe(&kp);
	return addr;
}
EXPORT_SYMBOL_GPL(shadow_hook_resolve);

#if defined(CONFIG_ARM64)
static void shadow_hook_redirect(struct pt_regs *regs, void *function)
{
	regs->pc = (unsigned long)function;
}

/*
 * shadow_hook_caller_pc - return address of whoever called into the hooked
 * function, as seen at the very first instruction of that function (i.e.
 * before its prologue has run). On arm64 the AAPCS64 calling convention
 * passes this in the link register (x30/regs[30]).
 */
static unsigned long shadow_hook_caller_pc(const struct pt_regs *regs)
{
	return regs->regs[30];
}
#elif defined(CONFIG_X86_64)
static void shadow_hook_redirect(struct pt_regs *regs, void *function)
{
	regs->ip = (unsigned long)function;
}

/*
 * shadow_hook_caller_pc - on x86-64, CALL pushes the return address onto
 * the stack; at the hooked function's very first instruction regs->sp still
 * points directly at it.
 */
static unsigned long shadow_hook_caller_pc(const struct pt_regs *regs)
{
	return *(unsigned long *)regs->sp;
}
#else
#error "shadow_hook: unsupported architecture"
#endif

/*
 * --- rmmod safety: kretprobe-based module refcounting -------------------
 *
 * See the "rmmod safety" note on struct shadow_hook (common/shadow_hook.h)
 * for the full rationale. In short: a kretprobe placed on hook->function
 * itself (not the hooked symbol) gives us a portable, prototype-agnostic
 * way to know exactly when a redirected call finishes, regardless of
 * hook->function's real signature (pt_regs-based syscall wrappers,
 * chrdev_open()'s (inode, file) pair, ...). Its return handler drops the
 * module reference that the forward hook's redirect point acquires just
 * before redirecting, so module_refcount() stays non-zero for exactly as
 * long as a redirected call is in flight anywhere in the system.
 *
 * shadow_hook_ri_to_kretprobe() maps a struct kretprobe_instance back to
 * its owning struct kretprobe. Upstream replaced the plain `struct kretprobe
 * *rp` field of struct kretprobe_instance with the struct kretprobe_holder
 * indirection (and introduced the get_kretprobe() accessor) in the 5.15
 * cycle; every supported Android GKI branch tracks that upstream cutoff
 * exactly (android12-5.10 and android13-5.10 still have the plain `rp`
 * field and no get_kretprobe() at all, while android13-5.15 and newer both
 * have the holder indirection and get_kretprobe()), so gating on
 * LINUX_VERSION_CODE >= 5.15 is reliable here.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)
static struct kretprobe *shadow_hook_ri_to_kretprobe(struct kretprobe_instance *ri)
{
	return get_kretprobe(ri);
}
#else
static struct kretprobe *shadow_hook_ri_to_kretprobe(struct kretprobe_instance *ri)
{
	return ri->rp;
}
#endif

static int shadow_hook_retprobe_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct kretprobe *rp = shadow_hook_ri_to_kretprobe(ri);
	struct shadow_hook *hook;

	if (!rp)
		return 0;

	hook = container_of(rp, struct shadow_hook, retprobe);
	atomic_dec(&shadow_hook_inflight);
	module_put(hook->owner);
	return 0;
}

/*
 * shadow_hook_retprobe_install() - best-effort; a failure here only means
 * this particular hook loses rmmod protection (shadow_hook_thunk()/
 * shadow_hook_pre_handler() check hook->retprobe_installed before taking a
 * reference, so we never acquire a module reference we could fail to
 * release). CONFIG_KRETPROBES is implied by CONFIG_KPROBES on every arch
 * this module targets, so this is not expected to fail in practice.
 *
 * Idempotent by design, same as shadow_hook_install(): a hook that is
 * force-unloaded and reloaded at runtime via
 * shadow_hook_registry_set_active() never has its retprobe torn down (see
 * shadow_hook_remove()'s comment below for why), so a reinstall must not
 * blindly re-register -- doing so would double-register an already-live
 * kretprobe (and the memset() below would corrupt it mid-flight).
 */
static void shadow_hook_retprobe_install(struct shadow_hook *hook)
{
	int err;

	if (hook->retprobe_installed)
		return;

	memset(&hook->retprobe, 0, sizeof(hook->retprobe));
	hook->retprobe.kp.addr = (kprobe_opcode_t *)hook->function;
	hook->retprobe.handler = shadow_hook_retprobe_ret;

	err = register_kretprobe(&hook->retprobe);
	if (err) {
		LKM4CTR_WARN("shadow_hijack",
			     "register_kretprobe() failed for %s: %d; rmmod will not wait for in-flight calls to this hook",
			     hook->resolved_name, err);
		return;
	}
	hook->retprobe_installed = true;
}

/*
 * shadow_hook_retprobe_remove() - only ever safe to call once the kernel
 * itself has already guaranteed no call can still be executing inside
 * hook->function, i.e. from shadow_hook_teardown_all_retprobes() at the
 * very end of lkm4ctr_exit() (module_exit() is only reached after the
 * kernel's own delete_module() path has confirmed module_refcount() is
 * zero). See that function's comment for the full rationale on why this
 * must never be called from a runtime shadow_hook_remove() (deactivate).
 */
static void shadow_hook_retprobe_remove(struct shadow_hook *hook)
{
	if (!hook->retprobe_installed)
		return;
	unregister_kretprobe(&hook->retprobe);
	hook->retprobe_installed = false;
}

/*
 * shadow_hook_quiescing - module-wide "stop redirecting new calls" flag.
 *
 * Set by the safe-unload orchestration (lkm4ctr_safe_unload.c) before it
 * starts waiting for module_refcount() to drain to zero. Once set, both
 * redirect points (shadow_hook_thunk(), shadow_hook_pre_handler()) fall
 * through to genuine kernel behaviour instead of redirecting into
 * hook->function, so no *new* redirected call can start (and therefore no
 * new module reference can be acquired) while a safe-unload is in
 * progress. This is deliberately a plain bool rather than something
 * requiring hook-specific state: it is checked once per redirect attempt,
 * ahead of the (per-hook) try_module_get() call, and reset back to false if
 * a safe-unload attempt aborts (e.g. on timeout) so hooks keep working
 * normally.
 */
static bool shadow_hook_quiescing;

void shadow_hook_quiesce(bool quiesce)
{
	/* Ordinary store: this is a best-effort, eventually-consistent
	 * request flag, not a synchronisation primitive in its own right --
	 * module_refcount() is what safe-unload actually waits on.
	 */
	WRITE_ONCE(shadow_hook_quiescing, quiesce);
}
EXPORT_SYMBOL_GPL(shadow_hook_quiesce);

bool shadow_hook_is_quiescing(void)
{
	return READ_ONCE(shadow_hook_quiescing);
}
EXPORT_SYMBOL_GPL(shadow_hook_is_quiescing);

#if defined(CONFIG_FUNCTION_TRACER) && defined(CONFIG_DYNAMIC_FTRACE)

/*
 * ftrace_set_filter_ip()/register_ftrace_function()/
 * unregister_ftrace_function() are EXPORT_SYMBOL_GPL()'d, but -- like
 * path_put()/vfs_mkdir()/anon_inode_getfd_secure() elsewhere in this
 * module -- some "certified"/production Android GKI boot images build
 * with CONFIG_TRIM_UNUSED_KSYMS, which strips their ksymtab entries
 * whenever no *other* module the vendor ships happens to reference them,
 * causing a hard "Unknown symbol" failure at insmod time even though
 * CONFIG_FUNCTION_TRACER/CONFIG_DYNAMIC_FTRACE are both enabled and this
 * backend is otherwise fully applicable. Resolve them the same way as
 * every other potentially-trimmed symbol instead of linking against them
 * directly.
 */
typedef int (*shadow_ftrace_set_filter_ip_t)(struct ftrace_ops *, unsigned long,
					      int, int);
typedef int (*shadow_register_ftrace_function_t)(struct ftrace_ops *);
typedef int (*shadow_unregister_ftrace_function_t)(struct ftrace_ops *);

static shadow_ftrace_set_filter_ip_t shadow_ftrace_set_filter_ip_fn;
static shadow_register_ftrace_function_t shadow_register_ftrace_function_fn;
static shadow_unregister_ftrace_function_t shadow_unregister_ftrace_function_fn;

/*
 * shadow_hook_ftrace_api_ready() - resolve the ftrace registration API on
 * first use and cache the results; idempotent. Only ever called from
 * shadow_hook_install()/shadow_hook_remove(), both of which run from the
 * single-threaded module init/exit sequencer (never concurrently), so no
 * extra locking is needed here -- same assumption already relied upon by
 * every other shadow_hook_resolve()-based lazy resolution in this codebase
 * (e.g. vendor_kernel_ipc_mount.c's vns_mqueue_dev_ensure()).
 */
static bool shadow_hook_ftrace_api_ready(void)
{
	if (!shadow_ftrace_set_filter_ip_fn) {
		shadow_ftrace_set_filter_ip_fn = (shadow_ftrace_set_filter_ip_t)
			shadow_hook_resolve("ftrace_set_filter_ip");
		if (!shadow_ftrace_set_filter_ip_fn)
			pr_debug("shadow_hook: could not resolve ftrace_set_filter_ip\n");
	}
	if (!shadow_register_ftrace_function_fn) {
		shadow_register_ftrace_function_fn = (shadow_register_ftrace_function_t)
			shadow_hook_resolve("register_ftrace_function");
		if (!shadow_register_ftrace_function_fn)
			pr_debug("shadow_hook: could not resolve register_ftrace_function\n");
	}
	if (!shadow_unregister_ftrace_function_fn) {
		shadow_unregister_ftrace_function_fn = (shadow_unregister_ftrace_function_t)
			shadow_hook_resolve("unregister_ftrace_function");
		if (!shadow_unregister_ftrace_function_fn)
			pr_debug("shadow_hook: could not resolve unregister_ftrace_function\n");
	}

	return shadow_ftrace_set_filter_ip_fn &&
	       shadow_register_ftrace_function_fn &&
	       shadow_unregister_ftrace_function_fn;
}

/*
 * --- ftrace_ops/IPMODIFY backend --------------------------------------
 *
 * The ftrace callback signature changed with
 * CONFIG_DYNAMIC_FTRACE_WITH_ARGS (introduced upstream for arm64/x86-64
 * around v5.19/v6.0): the fourth argument became an opaque `struct
 * ftrace_regs *` instead of `struct pt_regs *`, accessed via the
 * ftrace_get_regs() accessor. Older kernels in our supported range
 * (5.10/5.15/6.1) still use the plain pt_regs form. Both are handled here so
 * the exact same source builds unmodified against every Android GKI branch
 * we target (5.10 through 6.12).
 *
 * The recursion guard checks within_module(parent_ip, hook->owner): parent_ip
 * is the return address of whoever called the hooked function, and hook->owner
 * is the owning lkm4ctr.ko module performing the pass-through call. See
 * the file header for why.
 *
 * try_module_get(hook->owner) immediately before redirecting acquires the
 * module reference that hook->retprobe's return handler releases once
 * hook->function actually finishes -- see the "rmmod safety" note on
 * struct shadow_hook. If it fails (module already on its way out, or this
 * particular hook's retprobe never installed), fall through to genuine
 * kernel behaviour instead of redirecting.
 */
#ifdef CONFIG_DYNAMIC_FTRACE_WITH_ARGS
static void notrace shadow_hook_thunk(unsigned long ip, unsigned long parent_ip,
				       struct ftrace_ops *ops, struct ftrace_regs *fregs)
{
	struct pt_regs *regs = ftrace_get_regs(fregs);
	struct shadow_hook *hook = container_of(ops, struct shadow_hook, ops);

	if (!regs)
		return;
	if (within_module(parent_ip, hook->owner))
		return;
	if (shadow_hook_is_quiescing())
		return;
	if (hook->retprobe_installed) {
		if (!try_module_get(hook->owner))
			return;
		atomic_inc(&shadow_hook_inflight);
	}
	shadow_hook_redirect(regs, hook->function);
}
#else
static void notrace shadow_hook_thunk(unsigned long ip, unsigned long parent_ip,
				       struct ftrace_ops *ops, struct pt_regs *regs)
{
	struct shadow_hook *hook = container_of(ops, struct shadow_hook, ops);

	if (within_module(parent_ip, hook->owner))
		return;
	if (shadow_hook_is_quiescing())
		return;
	if (hook->retprobe_installed) {
		if (!try_module_get(hook->owner))
			return;
		atomic_inc(&shadow_hook_inflight);
	}
	shadow_hook_redirect(regs, hook->function);
}
#endif

/*
 * shadow_hook_install - resolve @hook->names and start redirecting calls.
 *
 * On success, *(void **)hook->original holds the genuine function's address
 * (call through it with the same prototype to invoke real kernel code), and
 * hook->installed is true.
 *
 * Returns 0 on success, negative errno otherwise. It is not an error for the
 * symbol to be missing entirely (returns -ENOENT) so callers can simply skip
 * shadowing a syscall that a particular kernel build already implements
 * natively (CONFIG_SYSVIPC=y, etc.) or does not expose at all.
 *
 * __nocfi: the only CFI-unsafe indirect calls in this whole file live here
 * and in the ftrace-backend shadow_hook_remove() below (through
 * shadow_ftrace_set_filter_ip_fn()/shadow_register_ftrace_function_fn(),
 * resolved by name via shadow_hook_resolve() since they can be trimmed from
 * a GKI KMI's export table). Everything else in this file -- most
 * importantly shadow_hook_pre_handler()/shadow_hook_thunk()/
 * shadow_hook_retprobe_ret(), which the real, CFI-instrumented kernel calls
 * back into indirectly via struct kprobe/kretprobe/ftrace_ops -- must keep
 * ordinary CFI instrumentation so those callback functions retain a valid
 * KCFI type-hash prefix; see lkm4ctr/Makefile's VNS_CFI_UNSAFE_OBJS comment
 * for why shadow_hijack.o itself is deliberately *not* CFI-disabled
 * wholesale (doing so previously caused "CFI failure at
 * kprobe_breakpoint_handler+... target: shadow_hook_pre_handler+..." on
 * insmod).
 */
int __nocfi shadow_hook_install(struct shadow_hook *hook)
{
	const char * const *name;
	int err;

	/*
	 * Idempotent by design: shadow_hook_registry_set_active()'s runtime
	 * force-load path (diagfs "status" writes) may call this on a hook
	 * that is already installed (e.g. re-issuing "load" after it already
	 * took effect), and must be able to do so safely without re-arming
	 * the ftrace_ops/kprobe or double-counting install state.
	 */
	if (hook->installed)
		return 0;

	if (!shadow_hook_ftrace_api_ready())
		return -ENOENT;

	for (name = hook->names; *name; name++) {
		hook->address = shadow_hook_resolve(*name);
		if (hook->address) {
			hook->resolved_name = *name;
			pr_debug("shadow_hook: resolved candidate \"%s\" -> %px\n",
				 *name, (void *)hook->address);
			break;
		}
		pr_debug("shadow_hook: candidate \"%s\" not found, trying next\n", *name);
	}
	if (!hook->address)
		return -ENOENT;

	*((unsigned long *)hook->original) = hook->address;

	hook->ops.func = shadow_hook_thunk;
	hook->ops.flags = FTRACE_OPS_FL_SAVE_REGS
			 | FTRACE_OPS_FL_IPMODIFY
			 | FTRACE_OPS_FL_RECURSION;

	err = shadow_ftrace_set_filter_ip_fn(&hook->ops, hook->address, 0, 0);
	if (err) {
		pr_debug("shadow_hook: ftrace_set_filter_ip(%s) failed: %d\n",
			 hook->resolved_name, err);
		return err;
	}

	err = shadow_register_ftrace_function_fn(&hook->ops);
	if (err) {
		pr_debug("shadow_hook: register_ftrace_function(%s) failed: %d\n",
			 hook->resolved_name, err);
		shadow_ftrace_set_filter_ip_fn(&hook->ops, hook->address, 1, 0);
		return err;
	}

	hook->installed = true;
	shadow_hook_retprobe_install(hook);
	return 0;
}
EXPORT_SYMBOL_GPL(shadow_hook_install);

/*
 * shadow_hook_remove() - stop redirecting *new* calls into hook->function.
 *
 * Deliberately does NOT tear down hook->retprobe: that would call
 * unregister_kretprobe(), which (kernel/kprobes.c
 * unregister_kretprobes()) unconditionally sets every already in-flight
 * kretprobe_instance's rp back-pointer (ri->rph->rp, via
 * get_kretprobe()) to NULL before returning -- by design, so a caller
 * that immediately frees/reuses the kretprobe struct after unregistering
 * cannot be used-after-freed by a late-firing instance. That means any
 * call that was already redirected into hook->function *before* this
 * shadow_hook_remove() runs, but has not returned yet, would silently
 * lose its shadow_hook_retprobe_ret() callback the moment
 * unregister_kretprobe() executes: shadow_hook_ri_to_kretprobe()
 * (get_kretprobe()/ri->rp) now returns NULL for it, so the handler
 * returns immediately without its atomic_dec(&shadow_hook_inflight) or
 * module_put(hook->owner) -- permanently orphaning that one reference
 * and hanging module_refcount() above zero forever (observed via the
 * diagfs "references" file reporting a huge, permanently-stuck
 * in-flight count with every submodule already reporting "unloaded").
 *
 * Since hook->retprobe is embedded in struct shadow_hook (this module's
 * own static/allocated memory, not a separately freed object), there is
 * no such use-after-free risk here: it is always safe to simply leave
 * the retprobe registered for the rest of the hook's lifetime once
 * installed, so already in-flight calls keep draining correctly no
 * matter how many times this hook is force-unloaded/reloaded at runtime
 * via shadow_hook_registry_set_active(). It is only ever torn down by
 * shadow_hook_teardown_all_retprobes() at the very end of lkm4ctr_exit(),
 * once the kernel itself has already guaranteed module_refcount() is
 * zero (module_exit() is only reached after that).
 *
 * __nocfi: see shadow_hook_install()'s comment above -- the resolved-pointer
 * calls below are the same class of CFI-unsafe indirect call.
 */
void __nocfi shadow_hook_remove(struct shadow_hook *hook)
{
	if (!hook->installed)
		return;

	shadow_unregister_ftrace_function_fn(&hook->ops);
	shadow_ftrace_set_filter_ip_fn(&hook->ops, hook->address, 1, 0);
	hook->installed = false;
}
EXPORT_SYMBOL_GPL(shadow_hook_remove);

#else /* !(CONFIG_FUNCTION_TRACER && CONFIG_DYNAMIC_FTRACE) */

/*
 * --- kprobe pre_handler backend -----------------------------------------
 *
 * Used whenever the target kernel does not have CONFIG_FUNCTION_TRACER (and
 * therefore no CONFIG_DYNAMIC_FTRACE / register_ftrace_function()) compiled
 * in, e.g. several "certified"/production GKI boot images.
 *
 * A kprobe is placed at the very first instruction of the target function.
 * When it fires, the CPU has already trapped into the kernel and @regs holds
 * the exact register state the target function would have seen. Rewriting
 * the saved program counter (arm64 pc / x86-64 ip) to our replacement
 * function and returning 1 from the pre_handler tells the kprobes core that
 * the handler has fully taken over: it must NOT single-step the original
 * (now bypassed) instruction, it should just resume the CPU with the
 * (modified) register state as-is. This is the standard technique used by
 * numerous out-of-tree hooking modules on kernels without ftrace, and is
 * fully described by the kprobes documentation's "jump/int3 based
 * probing" and "pre_handler return value" semantics.
 */
static int shadow_hook_pre_handler(struct kprobe *p, struct pt_regs *regs)
{
	struct shadow_hook *hook = container_of(p, struct shadow_hook, kp);

	/*
	 * The kprobe sits at the very first instruction of the hooked
	 * function, so it also fires again when *our own* replacement
	 * (hook->function) calls through hook->original -- which is simply
	 * the same hooked address -- to invoke genuine kernel behaviour.
	 * Without this check, that pass-through call would be redirected
	 * straight back into hook->function, recursing until the kernel
	 * stack overflows.
	 *
	 * Mirror the ftrace backend's within_module(parent_ip, hook->owner)
	 * guard (see shadow_hook_thunk() above) using the caller's return
	 * address, which is available at function entry (in the link
	 * register on arm64, or on the stack on x86-64) before any prologue
	 * instructions have executed. hook->owner is the merged lkm4ctr.ko
	 * module that owns the replacement function making the pass-through
	 * call.
	 * If the call came from that module, return 0 to tell the kprobes
	 * core the pre_handler has *not* taken over: it will single-step the
	 * original (untouched) instruction and resume normal execution, i.e.
	 * genuinely fall through into the target function's real body.
	 * Otherwise (a fresh, external call) redirect: return 1, which tells
	 * kprobes we have fully handled the trap ourselves (regs->pc/ip
	 * already points at hook->function) and the replaced instruction must
	 * not be single-stepped.
	 *
	 * try_module_get(hook->owner) immediately before redirecting acquires
	 * the module reference that hook->retprobe's return handler releases
	 * once hook->function actually finishes -- see the "rmmod safety"
	 * note on struct shadow_hook. If it fails (module already on its way
	 * out, or this particular hook's retprobe never installed), fall
	 * through to genuine kernel behaviour (return 0) instead of
	 * redirecting.
	 */
	if (within_module(shadow_hook_caller_pc(regs), hook->owner))
		return 0;

	if (shadow_hook_is_quiescing())
		return 0;

	if (hook->retprobe_installed) {
		if (!try_module_get(hook->owner))
			return 0;
		atomic_inc(&shadow_hook_inflight);
	}

	shadow_hook_redirect(regs, hook->function);
	return 1;
}

int shadow_hook_install(struct shadow_hook *hook)
{
	const char * const *name;
	int err;

	/* Idempotent by design -- see the ftrace-backend variant's comment above. */
	if (hook->installed)
		return 0;

	for (name = hook->names; *name; name++) {
		hook->address = shadow_hook_resolve(*name);
		if (hook->address) {
			hook->resolved_name = *name;
			pr_debug("shadow_hook: resolved candidate \"%s\" -> %px\n",
				 *name, (void *)hook->address);
			break;
		}
		pr_debug("shadow_hook: candidate \"%s\" not found, trying next\n", *name);
	}
	if (!hook->address)
		return -ENOENT;

	*((unsigned long *)hook->original) = hook->address;

	memset(&hook->kp, 0, sizeof(hook->kp));
	hook->kp.addr = (kprobe_opcode_t *)hook->address;
	hook->kp.pre_handler = shadow_hook_pre_handler;

	err = register_kprobe(&hook->kp);
	if (err) {
		pr_debug("shadow_hook: register_kprobe(%s) failed: %d\n",
			 hook->resolved_name, err);
		return err;
	}

	hook->installed = true;
	shadow_hook_retprobe_install(hook);
	return 0;
}
EXPORT_SYMBOL_GPL(shadow_hook_install);

/*
 * shadow_hook_remove() - stop redirecting *new* calls into hook->function.
 * See the ftrace-backend variant's comment above for why hook->retprobe
 * is deliberately left registered here (only torn down at true module
 * exit, via shadow_hook_teardown_all_retprobes()).
 */
void shadow_hook_remove(struct shadow_hook *hook)
{
	if (!hook->installed)
		return;

	unregister_kprobe(&hook->kp);
	hook->installed = false;
}
EXPORT_SYMBOL_GPL(shadow_hook_remove);

#endif /* CONFIG_FUNCTION_TRACER && CONFIG_DYNAMIC_FTRACE */

/*
 * shadow_hook_install_all()/shadow_hook_remove_all() - convenience helpers
 * for a NULL-terminated array of `struct shadow_hook *`. A resolution
 * failure (-ENOENT) for an individual hook is logged and skipped rather than
 * aborting the whole batch, since a given kernel build may simply not have
 * that particular syscall compiled in under any name (nothing to shadow) or
 * may already provide it natively (nothing to fall back for).
 */
int shadow_hook_install_all(struct shadow_hook **hooks, const char *tag)
{
	int i, err, installed = 0;

	shadow_hook_registry_add(tag, hooks);

	for (i = 0; hooks[i]; i++) {
		pr_debug("%s: attempting to install hook[%d]\n", tag, i);
		err = shadow_hook_install(hooks[i]);
		if (err == -ENOENT) {
			LKM4CTR_INFO(tag, "symbol for hook[%d] not found, skipping", i);
			continue;
		}
		if (err) {
			LKM4CTR_ERR(tag, "failed to install hook[%d]: %d", i, err);
			return err;
		}
		LKM4CTR_INFO(tag, "hooked %s at %px", hooks[i]->resolved_name,
			     (void *)hooks[i]->address);
		installed++;
	}

	pr_debug("%s: hook install pass complete, %d installed\n", tag, installed);
	return installed;
}
EXPORT_SYMBOL_GPL(shadow_hook_install_all);

void shadow_hook_remove_all(struct shadow_hook **hooks)
{
	struct shadow_hook_registry_group *g;
	const char *tag = "shadow_hook";
	int i, removed = 0;

	mutex_lock(&shadow_hook_registry_lock);
	g = shadow_hook_registry_find_locked(hooks);
	if (g)
		tag = g->tag;
	mutex_unlock(&shadow_hook_registry_lock);

	for (i = 0; hooks[i]; i++) {
		if (!hooks[i]->installed)
			continue;
		LKM4CTR_INFO(tag, "removing hook %s (was installed at 0x%lx)",
			     hooks[i]->resolved_name ? hooks[i]->resolved_name : "?",
			     hooks[i]->address);
		shadow_hook_remove(hooks[i]);
		removed++;
	}
	LKM4CTR_INFO(tag, "hook remove pass complete, %d hook(s) removed", removed);

	shadow_hook_registry_remove(hooks);
}
EXPORT_SYMBOL_GPL(shadow_hook_remove_all);

/*
 * shadow_hook_teardown_all_retprobes() - unregister every hook's retprobe,
 * across every group ever registered via shadow_hook_install_all(),
 * whether or not that group's forward hooks are currently installed.
 *
 * Only safe to call once the kernel itself has already guaranteed
 * module_refcount() is zero, i.e. from shadow_hijack_exit() -- the very
 * last submodule _exit() called from lkm4ctr_exit() -- since module_exit()
 * is only ever reached after the kernel's own delete_module() path has
 * confirmed no reference (including every try_module_get(hook->owner)
 * this file's redirect points take) remains outstanding. At that point
 * there cannot be any call still executing inside any hook->function, so
 * unregister_kretprobe()'s in-flight-instance-orphaning behaviour (see
 * shadow_hook_remove()'s comment) is a non-issue here.
 */
static void shadow_hook_teardown_all_retprobes(void)
{
	struct shadow_hook_registry_group *g;

	mutex_lock(&shadow_hook_registry_lock);
	list_for_each_entry(g, &shadow_hook_registry_groups, list) {
		int i;

		for (i = 0; g->hooks[i]; i++)
			shadow_hook_retprobe_remove(g->hooks[i]);
	}
	mutex_unlock(&shadow_hook_registry_lock);
}

int shadow_hijack_init(void)
{
	/*
	 * No global state to set up: this module exists solely to host the
	 * exported shadow_hook_* implementation. Loading it makes those
	 * symbols available to the hooking modules that depend on it.
	 */
	LKM4CTR_INFO("shadow_hijack", "loaded (shared shadow_hook implementation)");
	return 0;
}

void shadow_hijack_exit(void)
{
	shadow_hook_teardown_all_retprobes();
	LKM4CTR_INFO("shadow_hijack", "unloaded");
}
