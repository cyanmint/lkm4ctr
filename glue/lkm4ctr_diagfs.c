// SPDX-License-Identifier: GPL-2.0
/*
 * lkm4ctr_diagfs - the "lkm4ctr" pseudo-filesystem: `mount -t lkm4ctr diag
 * ./mnt` exposes a read/write diagnostics tree replacing the earlier
 * /dev/lkm4ctr_safe_unload misc device entirely (see lkm4ctr_safe_unload.c's
 * removal), plus runtime control/introspection over every linked subsystem.
 *
 * Root layout (flattened)
 * ------------------------
 * The tree root is now exactly five entries plus one subtree:
 *
 *   ./mnt/status                   - read/write (0644). READ: the whole
 *                                    module's lifecycle state, one of
 *                                    "active" / "inactive" / "unloading"
 *                                    (plus the transient "loading"). WRITE:
 *                                    the merged control surface that the old
 *                                    split global/status + global/control
 *                                    pair used to provide --
 *                                      on           (alias: load, load_all)
 *                                                   -> (re)load every
 *                                                   diagfs-managed submodule
 *                                      off          (aliases: unload, 1,
 *                                                   remove, graceful)
 *                                                   -> graceful self-unload
 *                                      forceunload  (aliases: force,
 *                                                   force_unload)
 *                                                   -> force self-unload
 *                                      force2       -> start (or escalate an
 *                                                   in-progress unload to) the
 *                                                   aggressive force2 sequence
 *                                                   -- see
 *                                                   lkm4ctr_force2_override_refcount()
 *                                                   and lkm4ctr_run_rmmod().
 *
 *   ./mnt/log                      - read-only (0444), the flattened,
 *                                    unfiltered global log (replacing the
 *                                    old global/log): every log line from
 *                                    every subsystem, tag-unfiltered. The
 *                                    per-subsystem ./mnt/v/log and
 *                                    ./mnt/v/hotreload/log files remain for
 *                                    tag-filtered views of the same
 *                                    underlying ring buffer.
 *
 *   ./mnt/helper.sh                - read-only (0555), a POSIX-sh helper
 *                                    script (contents generated into
 *                                    helper.sh.c as a plain C string) meant
 *                                    to be sourced by a caller that has
 *                                    exported $lkm4ctr_diagfs to this mount's
 *                                    path: `. "$lkm4ctr_diagfs/helper.sh"`
 *                                    then defines an `lkm4ctr` dispatcher
 *                                    function (mount/umount/status/on/off/
 *                                    forceunload/force2/logcat/references/
 *                                    resources/v/hot-upgrade/help).
 *
 *   ./mnt/readme.txt               - read-only (0444), a full plain-text
 *                                    description of this whole diagfs tree
 *                                    and how to use it (contents generated
 *                                    into readme.txt.c as a plain C string,
 *                                    kept in sync with this comment block by
 *                                    hand). Self-contained: readable with
 *                                    "cat" alone, no source tree needed.
 *
 *   ./mnt/resources/               - a dynamic directory with one file per
 *                                    real reference that is currently
 *                                    preventing a safe `rmmod` -- the same
 *                                    breakdown the "references" renderer
 *                                    reports, but one dentry per holder:
 *                                      mount-<n>  each active lkm4ctr diagfs
 *                                                 mount (file_system_type->
 *                                                 owner reference)
 *                                      hook-<n>   each in-flight shadow_hook-
 *                                                 redirected call
 *                                      other-<n>  each still-unaccounted
 *                                                 external reference
 *                                    `cat`ing one describes that specific
 *                                    holder; deleting one forcibly clears it
 *                                    where possible (see the per-type notes
 *                                    in lkm4ctr_diag_res_delete(): mount and
 *                                    hook deletions are honestly *aggregate*
 *                                    -- they umount all lkm4ctr mounts /
 *                                    drain all in-flight hooks respectively,
 *                                    since individual ones are not targetable
 *                                    today; "other" holders cannot be cleared
 *                                    from here at all).
 *
 *                                    SAFE-RMMOD RULE: when ./mnt/resources/
 *                                    contains only the file(s) for the diagfs
 *                                    mount(s) actively keeping it busy (i.e.
 *                                    no hook-* or other-* remain, only the very
 *                                    mount you are reading through), it is
 *                                    safe to `umount` diagfs and then `rmmod
 *                                    lkm4ctr` -- exactly the self-referential
 *                                    caveat the "references"/auto-umount code
 *                                    already documents about the mount you
 *                                    read/write through itself holding a
 *                                    reference.
 *
 *   ./mnt/v/                       - vendor_kernel: the only runtime-loadable
 *                                    subsystem linked into lkm4ctr.ko (the
 *                                    shared ftrace/kprobe hook engine and the
 *                                    former shadow_cgdevices submodule are no
 *                                    longer separately loadable). A procfs-
 *                                    style live-resource tree:
 *       ./mnt/v/control            (0644) per-subsystem command help / load /
 *                                    unload / forceunload (force2 is a
 *                                    module-wide escalation on ./mnt/status
 *                                    only).
 *       ./mnt/v/status             (0444) vendor_kernel's own lifecycle state.
 *       ./mnt/v/enabled            (0644) read: "1"/"0" (vendor_kernel_enabled);
 *                                    write: "1"/"on" loads, "0"/"off" unloads
 *                                    vendor_kernel -- a meaningful debug toggle
 *                                    of that flag/subsystem, same parse style
 *                                    as the control files.
 *       ./mnt/v/hooks              (0444) vendor_kernel's installed hooks.
 *       ./mnt/v/log                (0444) vendor_kernel's own log lines.
 *       ./mnt/v/references         (0444) vendor_kernel's contribution to
 *                                    module_refcount(), broken down.
 *       ./mnt/v/namespaces         (0444) full registry dump (every tracked
 *                                    tgid->nsproxy) + overlay diag.
 *       ./mnt/v/resources          (0444) aggregate live-resource dump.
 *       ./mnt/v/ns/<type>/         one dynamic directory per namespace type
 *                                    (pid, uts, ipc, user, net, time, mnt,
 *                                    cgroup). Each lists one file per live
 *                                    instance vendor_kernel tracks, named by
 *                                    the owning tgid; `cat` shows detail,
 *                                    delete SIGKILLs that thread group and
 *                                    frees its vendored nsproxy (see
 *                                    vns_diag_res_delete()).
 *       ./mnt/v/ipc/shm|msg|sem/   one dynamic directory per SysV IPC class.
 *                                    Each lists one file per live object in
 *                                    the default vendored ipc namespace,
 *                                    named by ipc id; delete performs
 *                                    IPC_RMID.
 *       ./mnt/v/ipc/mqueue/messages
 *                                    (0444) POSIX mqueue listing (aggregate;
 *                                    per-object mqueue delete is not wired --
 *                                    see the note there).
 *       ./mnt/v/hotreload/{status,log,do-hot-reload}
 *                                    hot-reload state, log, and the
 *                                    write-only absolute-path trigger for an
 *                                    in-place .ko replacement (validate path
 *                                    -> quiesce hooks -> drain refcount ->
 *                                    unmount diagfs -> hand off to a detached
 *                                    "rmmod && insmod <path> hotreload=1"
 *                                    shell -> module_put_and_kthread_exit()).
 *                                    See lkm4ctr_hotreload.c.
 *
 * The earlier ./mnt/global and ./mnt/vendor_kernel trees and the
 * ./mnt/modules/<subsystem>/status tree are intentionally gone; the
 * root-level ./mnt/safe_unload misc-device-era file never existed in this
 * filesystem and stays gone, but a root-level ./mnt/log is back (see above)
 * as the flattened, unfiltered global log. control/status stay split: on
 * ./mnt/status the read side is a strict lifecycle-state readout and the
 * write side is the command surface; ./mnt/v/control keeps that same split
 * per-subsystem.
 *
 * Implementation
 * --------------
 * The fixed skeleton (./mnt/status, ./mnt/log, ./mnt/helper.sh, ./mnt/readme.txt, the
 * ./mnt/v control/status/listing files, and the always-present per-type
 * directories ./mnt/resources/, ./mnt/v/ns/<type>/, ./mnt/v/ipc/<class>/) is
 * still built once, up front, at mount time (mount_nodev() + fill_super()),
 * in the same in-memory ramfs/securityfs spirit as before; the contents of
 * every readable file are regenerated fresh on every open() via seq_file
 * single_open().
 *
 * What is new is that the *leaf* per-instance entries under those per-type
 * directories are genuinely dynamic: they are not enumerated at mount time
 * (the live vendored namespaces / SysV IPC objects / reference holders come
 * and go at runtime), so ./mnt/resources/, ./mnt/v/ns/<type>/ and
 * ./mnt/v/ipc/<class>/ are backed by a small dynamic
 * .lookup/.iterate_shared/.unlink implementation
 * (lkm4ctr_diagfs_dyn_*()): .iterate_shared snapshots the live registry and
 * emits a dentry name per instance, .lookup materialises a
 * matching-and-still-present instance file on demand, and .unlink resolves
 * the instance's key back to its provider and tears the underlying resource
 * down (kill/RMID/quiesce) before removing the dentry. The provider backend
 * for the ./mnt/v/ trees lives in glue/vendor_kernel_diag.c behind the
 * vns_diag_res_* vtable (see glue/vendor_kernel_diag.h); the ./mnt/resources/
 * provider is diagfs-local (lkm4ctr_diag_res_*() below) because its holders
 * are diagfs/hook bookkeeping, not vendor_kernel objects. Aggregate registry
 * dumps that are not naturally one-object-per-dentry (the namespace registry
 * dump, the POSIX mqueue listing) remain readable listing files rather than
 * dynamic directories.
 *
 * Directory traversal reuses the kernel's own simple_lookup() (fs/libfs.c)
 * -- a plain function symbol, resolved lazily via lkm4ctr_diagfs_resolve()
 * like mount_nodev()/generic_delete_inode() below -- wired up into a
 * locally-owned struct inode_operations rather than referencing the
 * kernel's own simple_dir_inode_operations struct directly: that struct is
 * plain data, which register_kprobe()'s resolution trick fundamentally
 * cannot recover (kprobes require a probe-able instruction/text address),
 * and unlike mount_nodev()/generic_delete_inode() it is also a GKI
 * "Protected symbol" on some KMIs (EACCES at insmod even when present),
 * blocking direct reference regardless of export/trim state. Likewise,
 * kill_litter_super() and d_alloc_name() -- both plain functions -- are
 * resolved lazily instead of called directly, for the same "Protected
 * symbol" reason.
 */


#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/mount.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include <linux/version.h>
#include <linux/kthread.h>
#include <linux/umh.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/atomic.h>
#include <linux/kprobes.h>

#include "shadow_hook.h"
#include "../vendor/vendor_kernel.h"
#include "vendor_kernel_diag.h"
#include "lkm4ctr_log.h"
#include "lkm4ctr_compat.h"

#define LKM4CTR_DIAGFS_MAGIC	0x4C4B4D34 /* "LKM4" */
#define LKM4CTR_DIAGFS_TAG	"diagfs"

/*
 * lkm4ctr_diagfs_resolve() - resolve a kernel symbol's runtime address (0 if
 * not found), via the same register_kprobe()/unregister_kprobe() trick used
 * by shadow_hijack.c's shadow_hook_resolve() -- but implemented as its own,
 * private copy rather than a call into shadow_hijack.c.
 *
 * diagfs is the controller filesystem: it must keep mounting, rendering
 * status/log files and driving the final rmmod regardless of whatever
 * runtime load/unload state vendor_kernel itself is in. shadow_hijack (the
 * shared hook engine) is no longer a separately loadable/unloadable
 * subsystem at all -- it is always active for the lifetime of lkm4ctr.ko,
 * started/stopped directly by lkm4ctr_main.c -- so diagfs has no lifecycle
 * dependency on it to worry about either way. A controller fs that could
 * only mount itself by calling into the hook engine's own exported resolver
 * would still make diagfs *functionally* depend on shadow_hijack, defeating
 * that independence. The handful of plain kernel symbols diagfs itself
 * needs (mount_nodev/generic_delete_inode/module_refcount/
 * call_usermodehelper) are therefore resolved right here instead.
 */
static unsigned long lkm4ctr_diagfs_resolve(const char *name)
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

/*
 * mount_nodev() and generic_delete_inode() are ordinary EXPORT_SYMBOL()
 * functions, plain code, so they can be recovered via
 * lkm4ctr_diagfs_resolve()'s register_kprobe() trick if
 * CONFIG_TRIM_UNUSED_KSYMS strips them (confirmed: "Unknown symbol
 * mount_nodev"/"Unknown symbol generic_delete_inode" on insmod against a
 * production GKI kernel), same class of issue as module_refcount()/
 * call_usermodehelper() below. Resolved lazily at lkm4ctr_diagfs_init()
 * time instead of calling them directly.
 */
typedef struct dentry *(*lkm4ctr_mount_nodev_t)(struct file_system_type *fs_type,
						 int flags, void *data,
						 int (*fill_super)(struct super_block *,
								   void *, int));
typedef int (*lkm4ctr_generic_delete_inode_t)(struct inode *inode);

static lkm4ctr_mount_nodev_t lkm4ctr_mount_nodev_fn;
static lkm4ctr_generic_delete_inode_t lkm4ctr_generic_delete_inode_fn;

/*
 * kill_litter_super()/d_alloc_name()/simple_lookup() are ordinary plain
 * function symbols (fs/super.c, fs/dcache.c, fs/libfs.c respectively), but
 * on some GKI KMIs they are enforced as "Protected symbols" (EACCES at
 * insmod, distinct from the ordinary ENOENT/CONFIG_TRIM_UNUSED_KSYMS case
 * mount_nodev()/generic_delete_inode() hit above) -- resolved lazily via
 * lkm4ctr_diagfs_resolve() the same way, which sidesteps both enforcement
 * mechanisms uniformly since the reference becomes a runtime indirect call
 * rather than a direct ELF-level one.
 *
 * generic_shutdown_super() (fs/super.c, our .kill_sb fallback when
 * kill_litter_super() itself fails to resolve) is likewise resolved
 * lazily here rather than called directly by name: although genuinely
 * EXPORT_SYMBOL'd on every KMI in our support matrix, CI observed a live
 * "Unknown symbol generic_shutdown_super" insmod failure on a production
 * GKI build that trimmed it from the module symbol table
 * (CONFIG_TRIM_UNUSED_KSYMS), the same reason mount_nodev()/
 * generic_delete_inode() above are resolved lazily instead of imported.
 */
typedef void (*lkm4ctr_kill_litter_super_t)(struct super_block *sb);
typedef void (*lkm4ctr_generic_shutdown_super_t)(struct super_block *sb);
typedef struct dentry *(*lkm4ctr_d_alloc_name_t)(struct dentry *parent,
						  const char *name);
typedef struct dentry *(*lkm4ctr_simple_lookup_t)(struct inode *dir,
						   struct dentry *dentry,
						   unsigned int flags);
/*
 * simple_unlink() (fs/libfs.c) is resolved lazily too, exactly like
 * simple_lookup() above and for the same "Protected symbol"/CONFIG_TRIM
 * reasons -- the dynamic ./mnt/resources/ and ./mnt/v/{ns,ipc}/ directories'
 * .unlink callback uses it to drop the freed instance's dentry once the
 * underlying resource has been torn down.
 */
typedef int (*lkm4ctr_simple_unlink_t)(struct inode *dir, struct dentry *dentry);

static lkm4ctr_kill_litter_super_t lkm4ctr_kill_litter_super_fn;
static lkm4ctr_generic_shutdown_super_t lkm4ctr_generic_shutdown_super_fn;
static lkm4ctr_d_alloc_name_t lkm4ctr_d_alloc_name_fn;
static lkm4ctr_simple_lookup_t lkm4ctr_simple_lookup_fn;
static lkm4ctr_simple_unlink_t lkm4ctr_simple_unlink_fn;


static struct dentry *__nocfi lkm4ctr_diagfs_dir_lookup(struct inode *dir,
						 struct dentry *dentry,
						 unsigned int flags)
{
	if (!lkm4ctr_simple_lookup_fn)
		return ERR_PTR(-ENOSYS);
	return lkm4ctr_simple_lookup_fn(dir, dentry, flags);
}

/*
 * Module-owned replacement for the kernel's own simple_dir_inode_operations
 * (see the file header comment above for why that struct cannot be
 * referenced directly). Only .lookup is populated upstream too (see
 * fs/libfs.c), so this is a complete, faithful replica.
 */
static const struct inode_operations lkm4ctr_diagfs_dir_inode_operations = {
	.lookup = lkm4ctr_diagfs_dir_lookup,
};

/*
 * module_refcount() and call_usermodehelper() are ordinary EXPORT_SYMBOL()
 * functions, not GPL-only, but that alone doesn't save them from
 * CONFIG_TRIM_UNUSED_KSYMS on production GKI kernels: like
 * ftrace_set_filter_ip()/vm_mmap()/anon_inode_getfd_secure() elsewhere in
 * this module, they get stripped whenever nothing built into vmlinux
 * itself calls them. Resolved lazily via lkm4ctr_diagfs_resolve() instead
 * of shadow_hijack.c's shadow_hook_resolve() -- self-unload/rmmod is core
 * diagfs functionality that must keep working independently of anything
 * else, same rationale as lkm4ctr_diagfs_resolve() itself above. Declared
 * this early (rather than down by lkm4ctr_safe_unload_resolve() where they
 * used to live) so the "references" renderer below can also read
 * module_refcount() directly.
 */
typedef int (*lkm4ctr_module_refcount_t)(struct module *mod);
typedef int (*lkm4ctr_call_usermodehelper_t)(const char *path, char **argv,
					      char **envp, int wait);

static lkm4ctr_module_refcount_t lkm4ctr_module_refcount_fn;
static lkm4ctr_call_usermodehelper_t lkm4ctr_call_usermodehelper_fn;

extern int vendor_kernel_init(void);
extern void vendor_kernel_exit(void);
extern size_t vendor_kernel_diag_snprintf(char *buf, size_t buflen);

/* lkm4ctr_hotreload.c - see lkm4ctr_hotreload.h for full contracts. */
extern size_t lkm4ctr_hotreload_status_snprintf(char *buf, size_t buflen);
extern int lkm4ctr_hotreload_trigger(const char *path);

/* helper.sh.c - the diagfs helper.sh contents, generated from the canonical
 * shell script; see that file for what it does.
 */
extern const char lkm4ctr_helper_sh_data[];
extern const unsigned long lkm4ctr_helper_sh_size;

/* readme.txt.c - the diagfs readme.txt contents; see that file for the full
 * description of the diagfs tree and how to use it.
 */
extern const char lkm4ctr_readme_txt_data[];
extern const unsigned long lkm4ctr_readme_txt_size;

enum lkm4ctr_diagfs_kind {
	LKM4CTR_DIAG_CONTROL,
	LKM4CTR_DIAG_STATUS,
	LKM4CTR_DIAG_ENABLED,
	LKM4CTR_DIAG_HOOKS,
	LKM4CTR_DIAG_NAMESPACES,
	LKM4CTR_DIAG_LOG,
	LKM4CTR_DIAG_GLOBAL_RESOURCES,
	LKM4CTR_DIAG_MQUEUE_MSG,
	LKM4CTR_DIAG_SYSVIPC_RESOURCES,
	LKM4CTR_DIAG_REFERENCES,
	LKM4CTR_DIAG_HOTRELOAD_STATUS,
	LKM4CTR_DIAG_HOTRELOAD_TRIGGER,
	LKM4CTR_DIAG_HELPER_SCRIPT,
	LKM4CTR_DIAG_README,
	/* per-instance leaf under a dynamic ./mnt/resources or ./mnt/v/{ns,ipc} dir */
	LKM4CTR_DIAG_DYN_INSTANCE,
};

/*
 * Dynamic-directory "domains": which provider backs a dynamic dir's
 * .lookup/.iterate_shared/.unlink and its per-instance leaf files.
 *   VNS - vendor_kernel's live namespaces / SysV IPC objects, via the
 *         vns_diag_res_* vtable (glue/vendor_kernel_diag.c); dyn_provider is
 *         an enum vns_diag_res value, dyn_key is a tgid or ipc id.
 *   RES - the diagfs-local ./mnt/resources holders (mounts / in-flight hooks
 *         / other refs); dyn_provider is unused, dyn_key packs a holder type
 *         in its high 32 bits and an index in its low 32 bits.
 */
enum lkm4ctr_diagfs_dyn_domain {
	LKM4CTR_DYN_DOMAIN_NONE = 0,
	LKM4CTR_DYN_DOMAIN_VNS,
	LKM4CTR_DYN_DOMAIN_RES,
};

/* ./mnt/resources holder types packed into the high 32 bits of dyn_key. */
#define LKM4CTR_RES_MOUNT	1
#define LKM4CTR_RES_HOOK	2
#define LKM4CTR_RES_OTHER	3

enum lkm4ctr_diagfs_lifecycle_state {
	LKM4CTR_STATE_UNLOADED,
	LKM4CTR_STATE_LOADING,
	LKM4CTR_STATE_ACTIVE,
	LKM4CTR_STATE_GRACEFUL_UNLOADING,
	LKM4CTR_STATE_FORCE_UNLOADING,
};

struct lkm4ctr_diagfs_info {
	enum lkm4ctr_diagfs_kind	kind;
	char				tag[LKM4CTR_LOG_TAG_MAX];
	u32					ns_type;
	bool					has_ns_type;
	bool					is_global;
	int					dyn_domain;	/* enum lkm4ctr_diagfs_dyn_domain */
	int					dyn_provider;	/* enum vns_diag_res (VNS domain) */
	u64					dyn_key;	/* per-instance key (leaf files) */
};

struct lkm4ctr_diagfs_module {
	const char	*dirname;
	const char	*tag;
	bool		has_hooks;
	bool		has_namespaces;
	int		(*mod_init)(void);
	void		(*mod_exit)(void);
	enum lkm4ctr_diagfs_lifecycle_state state;
};

static struct lkm4ctr_diagfs_module lkm4ctr_diagfs_modules[] = {
	{ "v", 	"vendor_kernel", 	true,  true,  vendor_kernel_init,	vendor_kernel_exit,	LKM4CTR_STATE_ACTIVE },
};

static enum lkm4ctr_diagfs_lifecycle_state lkm4ctr_diagfs_global_state =
	LKM4CTR_STATE_ACTIVE;
static struct task_struct *lkm4ctr_unload_thread;
static DEFINE_MUTEX(lkm4ctr_unload_lock);
static bool lkm4ctr_unload_in_progress;
static bool lkm4ctr_unload_force;
/*
 * lkm4ctr_unload_force2 - "force2" escalation flag, checked by
 * lkm4ctr_safe_unload_fn()'s wait loop exactly like lkm4ctr_unload_force is
 * (see lkm4ctr_diagfs_control_write()): once set, the aggressive path
 * below skips all further waiting, forcibly drops any remaining
 * module_refcount() via repeated module_put(THIS_MODULE) calls (bypassing
 * the normal "wait for in-flight calls to finish naturally" protocol), and
 * finally requests rmmod with the force flag (`rmmod -f`, i.e.
 * delete_module(2)'s O_TRUNC), ignoring version/refcount safety checks the
 * same way an administrator running `rmmod -f` by hand would. This is
 * strictly more aggressive than plain "force"/"forceunload" and should
 * only ever be reached deliberately -- see the file header and
 * lkm4ctr_diagfs_parse_control_cmd() above.
 */
static bool lkm4ctr_unload_force2;
static atomic_t lkm4ctr_diagfs_mount_count = ATOMIC_INIT(0);

static struct lkm4ctr_diagfs_module *lkm4ctr_diagfs_find_module(const char *tag)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(lkm4ctr_diagfs_modules); i++) {
		if (!strcmp(lkm4ctr_diagfs_modules[i].tag, tag))
			return &lkm4ctr_diagfs_modules[i];
	}

	return NULL;
}

static const char *lkm4ctr_diagfs_state_name(enum lkm4ctr_diagfs_lifecycle_state state)
{
	switch (state) {
	case LKM4CTR_STATE_UNLOADED:
		return "unloaded";
	case LKM4CTR_STATE_LOADING:
		return "loading";
	case LKM4CTR_STATE_ACTIVE:
		return "active";
	case LKM4CTR_STATE_GRACEFUL_UNLOADING:
		return "graceful unloading";
	case LKM4CTR_STATE_FORCE_UNLOADING:
		return "force unloading";
	default:
		return "unloaded";
	}
}

static bool lkm4ctr_diagfs_state_busy(enum lkm4ctr_diagfs_lifecycle_state state)
{
	return state == LKM4CTR_STATE_LOADING ||
	       state == LKM4CTR_STATE_GRACEFUL_UNLOADING ||
	       state == LKM4CTR_STATE_FORCE_UNLOADING;
}

static enum lkm4ctr_diagfs_lifecycle_state
lkm4ctr_diagfs_module_stable_state(struct lkm4ctr_diagfs_module *mod)
{
	return shadow_hook_registry_tag_active(mod->tag) ?
		LKM4CTR_STATE_ACTIVE : LKM4CTR_STATE_UNLOADED;
}

static bool lkm4ctr_diagfs_any_module_transition_locked(void)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(lkm4ctr_diagfs_modules); i++) {
		if (lkm4ctr_diagfs_state_busy(lkm4ctr_diagfs_modules[i].state))
			return true;
	}
	return false;
}

/* ------------------------------------------------------------------- */
/* inode/dentry tree construction                                       */
/* ------------------------------------------------------------------- */

static const struct file_operations lkm4ctr_diagfs_ro_fops;
static const struct file_operations lkm4ctr_diagfs_control_fops;
static const struct file_operations lkm4ctr_diagfs_enabled_fops;
static const struct file_operations lkm4ctr_diagfs_hotreload_trigger_fops;
static const struct inode_operations lkm4ctr_diagfs_dyn_dir_inode_operations;
static struct file_operations lkm4ctr_diagfs_dyn_dir_fops;

static struct inode *lkm4ctr_diagfs_make_inode(struct super_block *sb, umode_t mode)
{
	struct inode *inode = new_inode(sb);

	if (!inode)
		return NULL;

	inode->i_ino = get_next_ino();
	inode->i_mode = mode;
	inode->i_uid = GLOBAL_ROOT_UID;
	inode->i_gid = GLOBAL_ROOT_GID;
	lkm4ctr_inode_init_ts(inode);

	return inode;
}

static struct dentry *__nocfi lkm4ctr_diagfs_mkdir(struct super_block *sb,
					    struct dentry *parent,
					    const char *name)
{
	struct inode *inode;
	struct dentry *dentry;

	inode = lkm4ctr_diagfs_make_inode(sb, S_IFDIR | 0555);
	if (!inode)
		return ERR_PTR(-ENOMEM);
	inode->i_op = &lkm4ctr_diagfs_dir_inode_operations;
	inode->i_fop = &simple_dir_operations;
	set_nlink(inode, 2);

	inode_lock(d_inode(parent));
	dentry = lkm4ctr_d_alloc_name_fn ? lkm4ctr_d_alloc_name_fn(parent, name) : NULL;
	if (!dentry) {
		inode_unlock(d_inode(parent));
		iput(inode);
		return ERR_PTR(-ENOMEM);
	}
	d_add(dentry, inode);
	inc_nlink(d_inode(parent));
	inode_unlock(d_inode(parent));

	return dentry;
}

static struct dentry *__nocfi lkm4ctr_diagfs_create_file(struct super_block *sb,
					  struct dentry *parent,
					  const char *name, umode_t mode,
					  enum lkm4ctr_diagfs_kind kind,
					  const char *tag,
					  bool is_global,
					  bool has_ns_type,
					  u32 ns_type)
{
	struct inode *inode;
	struct dentry *dentry;
	struct lkm4ctr_diagfs_info *info;

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info)
		return ERR_PTR(-ENOMEM);
	info->kind = kind;
	info->is_global = is_global;
	info->has_ns_type = has_ns_type;
	info->ns_type = ns_type;
	if (tag)
		strscpy(info->tag, tag, sizeof(info->tag));

	inode = lkm4ctr_diagfs_make_inode(sb, S_IFREG | mode);
	if (!inode) {
		kfree(info);
		return ERR_PTR(-ENOMEM);
	}
	inode->i_private = info;
	switch (kind) {
	case LKM4CTR_DIAG_CONTROL:
		inode->i_fop = &lkm4ctr_diagfs_control_fops;
		break;
	case LKM4CTR_DIAG_STATUS:
		/*
		 * The module-wide ./mnt/status is the merged status+control
		 * file: readable lifecycle state, writable command surface
		 * (on/off/forceunload/force2). Per-subsystem ./mnt/v/status
		 * stays read-only (its control lives in ./mnt/v/control).
		 */
		inode->i_fop = is_global ? &lkm4ctr_diagfs_control_fops :
					   &lkm4ctr_diagfs_ro_fops;
		break;
	case LKM4CTR_DIAG_ENABLED:
		inode->i_fop = &lkm4ctr_diagfs_enabled_fops;
		break;
	case LKM4CTR_DIAG_HOTRELOAD_TRIGGER:
		inode->i_fop = &lkm4ctr_diagfs_hotreload_trigger_fops;
		break;
	default:
		inode->i_fop = &lkm4ctr_diagfs_ro_fops;
		break;
	}

	inode_lock(d_inode(parent));
	dentry = lkm4ctr_d_alloc_name_fn ? lkm4ctr_d_alloc_name_fn(parent, name) : NULL;
	if (!dentry) {
		inode_unlock(d_inode(parent));
		/*
		 * iput() runs our sb->s_op->evict_inode callback
		 * (lkm4ctr_diagfs_evict_inode()), which already kfree()s
		 * inode->i_private (== info); do not free it again here.
		 * This is module-specific, not a general VFS guarantee.
		 */
		iput(inode);
		return ERR_PTR(-ENOMEM);
	}
	d_add(dentry, inode);
	inode_unlock(d_inode(parent));

	return dentry;
}

static int lkm4ctr_diagfs_create_checked(struct super_block *sb,
					 struct dentry *parent,
					 const char *name, umode_t mode,
					 enum lkm4ctr_diagfs_kind kind,
					 const char *tag,
					 bool is_global,
					 bool has_ns_type,
					 u32 ns_type)
{
	struct dentry *dentry;

	dentry = lkm4ctr_diagfs_create_file(sb, parent, name, mode, kind, tag,
					    is_global, has_ns_type, ns_type);
	return IS_ERR(dentry) ? PTR_ERR(dentry) : 0;
}

/* ------------------------------------------------------------------- */
/* content rendering                                                    */
/* ------------------------------------------------------------------- */

static size_t lkm4ctr_diagfs_control_snprintf(const struct lkm4ctr_diagfs_info *info,
					      char *buf, size_t buflen)
{
	size_t pos = 0;

	if (info->is_global) {
		pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
				 "commands:\n"
				 "  load         - load every submodule (alias: load_all)\n"
				 "  unload       - graceful self-unload (aliases: 1, remove, graceful)\n"
				 "  forceunload  - force self-unload (aliases: force, force_unload)\n");
		return pos;
	}

	pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
			 "commands:\n"
			 "  load         - load this subsystem\n"
			 "  unload       - graceful unload (aliases: remove, graceful)\n"
			 "  forceunload  - force unload (aliases: force, force_unload)\n");
	return pos;
}

static size_t lkm4ctr_diagfs_status_snprintf(const struct lkm4ctr_diagfs_info *info,
					     char *buf, size_t buflen)
{
	enum lkm4ctr_diagfs_lifecycle_state state;
	struct lkm4ctr_diagfs_module *mod;

	if (info->is_global) {
		const char *name;

		mutex_lock(&lkm4ctr_unload_lock);
		state = lkm4ctr_diagfs_global_state;
		mutex_unlock(&lkm4ctr_unload_lock);
		/*
		 * The module-wide ./mnt/status uses the spec's three-value
		 * vocabulary "active"/"inactive"/"unloading" (plus the
		 * transient "loading"), a simplification of the richer
		 * internal state names used by the per-subsystem ./mnt/v/status.
		 */
		switch (state) {
		case LKM4CTR_STATE_ACTIVE:
			name = "active";
			break;
		case LKM4CTR_STATE_LOADING:
			name = "loading";
			break;
		case LKM4CTR_STATE_GRACEFUL_UNLOADING:
		case LKM4CTR_STATE_FORCE_UNLOADING:
			name = "unloading";
			break;
		case LKM4CTR_STATE_UNLOADED:
		default:
			name = "inactive";
			break;
		}
		return scnprintf(buf, buflen, "%s\n", name);
	}

	mod = lkm4ctr_diagfs_find_module(info->tag);
	if (!mod)
		return scnprintf(buf, buflen, "unloaded\n");

	mutex_lock(&lkm4ctr_unload_lock);
	state = mod->state;
	mutex_unlock(&lkm4ctr_unload_lock);

	return scnprintf(buf, buflen, "%s\n", lkm4ctr_diagfs_state_name(state));
}

/*
 * ./mnt/v/enabled - read the vendor_kernel_enabled flag as "1"/"0".
 * The write side (a meaningful debug toggle: "1"/"on" loads vendor_kernel,
 * "0"/"off" unloads it) is handled by lkm4ctr_diagfs_enabled_write() below.
 */
static size_t lkm4ctr_diagfs_enabled_snprintf(const struct lkm4ctr_diagfs_info *info,
					      char *buf, size_t buflen)
{
	(void)info;
	return scnprintf(buf, buflen, "%d\n", vendor_kernel_enabled ? 1 : 0);
}

static size_t lkm4ctr_diagfs_hooks_snprintf(const struct lkm4ctr_diagfs_info *info,
					    char *buf, size_t buflen)
{
	return shadow_hook_registry_snprintf(info->tag[0] ? info->tag : NULL,
					     buf, buflen);
}

static size_t lkm4ctr_diagfs_namespaces_snprintf(const struct lkm4ctr_diagfs_info *info,
					 char *buf, size_t buflen)
{
	(void)info;
	return vendor_kernel_diag_snprintf(buf, buflen);
}

static size_t lkm4ctr_diagfs_log_snprintf(const struct lkm4ctr_diagfs_info *info,
					  char *buf, size_t buflen)
{
	return lkm4ctr_log_snprintf(info->tag[0] ? info->tag : NULL, buf, buflen);
}

static size_t lkm4ctr_diagfs_global_resources_snprintf(const struct lkm4ctr_diagfs_info *info,
				       char *buf, size_t buflen)
{
	size_t pos = 0;

	(void)info;
	pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
			 "hooks:\n");
	pos += shadow_hook_registry_snprintf(NULL,
				 buf + pos,
				 pos < buflen ? buflen - pos : 0);
	pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
			 "vendor_kernel:\n");
	pos += vendor_kernel_diag_snprintf(buf + pos,
				      pos < buflen ? buflen - pos : 0);
	return pos;
}

static size_t lkm4ctr_diagfs_mqueue_msg_snprintf(const struct lkm4ctr_diagfs_info *info,
					 char *buf, size_t buflen)
{
	(void)info;
	return vendor_kernel_diag_snprintf(buf, buflen);
}

static size_t lkm4ctr_diagfs_sysvipc_resources_snprintf(const struct lkm4ctr_diagfs_info *info,
							char *buf, size_t buflen)
{
	(void)info;
	return vendor_kernel_diag_snprintf(buf, buflen);
}

/* ------------------------------------------------------------------- */
/* ./mnt/resources/ provider (diagfs-local reference holders)           */
/* ------------------------------------------------------------------- */

/*
 * The ./mnt/resources/ directory breaks the same module_refcount() total the
 * "references" renderer explains into one dynamic file per real holder:
 *   mount-<n>  each active lkm4ctr diagfs mount (file_system_type->owner)
 *   hook-<n>   each in-flight shadow_hook-redirected call
 *   other-<n>  each still-unaccounted external reference
 * These counts are diagfs/hook bookkeeping, not vendor_kernel objects, so
 * they are served by this diagfs-local provider rather than the vns_diag_res_*
 * vtable used by ./mnt/v/{ns,ipc}/.
 */
static void lkm4ctr_diag_res_counts(int *mounts, int *hooks, int *other)
{
	int refcount = -1;

	*mounts = atomic_read(&lkm4ctr_diagfs_mount_count);
	*hooks = shadow_hook_inflight_count();
	if (lkm4ctr_module_refcount_fn)
		refcount = lkm4ctr_module_refcount_fn(THIS_MODULE);
	*other = (refcount < 0) ? 0 : max(0, (refcount - 1) - *mounts - *hooks);
}

static int lkm4ctr_diag_res_snapshot(u64 *keys, int max)
{
	int mounts, hooks, other, i, n = 0;

	lkm4ctr_diag_res_counts(&mounts, &hooks, &other);
	for (i = 0; i < mounts; i++, n++)
		if (n < max)
			keys[n] = ((u64)LKM4CTR_RES_MOUNT << 32) | (u32)i;
	for (i = 0; i < hooks; i++, n++)
		if (n < max)
			keys[n] = ((u64)LKM4CTR_RES_HOOK << 32) | (u32)i;
	for (i = 0; i < other; i++, n++)
		if (n < max)
			keys[n] = ((u64)LKM4CTR_RES_OTHER << 32) | (u32)i;
	return n;
}

static bool lkm4ctr_diag_res_present(u64 key)
{
	int mounts, hooks, other;
	int type = (int)(key >> 32);
	u32 idx = (u32)key;

	lkm4ctr_diag_res_counts(&mounts, &hooks, &other);
	switch (type) {
	case LKM4CTR_RES_MOUNT:
		return idx < (u32)mounts;
	case LKM4CTR_RES_HOOK:
		return idx < (u32)hooks;
	case LKM4CTR_RES_OTHER:
		return idx < (u32)other;
	default:
		return false;
	}
}

static size_t lkm4ctr_diag_res_render(u64 key, char *buf, size_t buflen)
{
	int type = (int)(key >> 32);
	u32 idx = (u32)key;
	size_t pos = 0;

	switch (type) {
	case LKM4CTR_RES_MOUNT:
		pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
				 "holder: lkm4ctr diagfs mount #%u\n"
				 "type: file_system_type->owner reference\n"
				 "detail: each active `mount -t lkm4ctr` pins module_refcount(), exactly like any other in-use filesystem module\n",
				 idx);
		pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
				 "delete: umounts ALL lkm4ctr diagfs mounts (aggregate; individual mounts are not targetable today -- honest limitation)\n");
		break;
	case LKM4CTR_RES_HOOK:
		pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
				 "holder: in-flight shadow_hook-redirected call #%u\n"
				 "type: shadow_hook_inflight_count() contributor\n"
				 "detail: a hooked syscall is currently executing in another task and holds a module reference until it returns\n",
				 idx);
		pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
				 "delete: quiesces new redirects and drains ALL in-flight hook calls (aggregate; a single call is not targetable, and a genuinely stuck call cannot be force-killed -- honest limitation)\n");
		break;
	case LKM4CTR_RES_OTHER:
		pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
				 "holder: unaccounted external reference #%u\n"
				 "type: other/unexplained module_refcount() contributor\n"
				 "detail: typically another module using an EXPORT_SYMBOL_GPL() of lkm4ctr.ko, or a reference this file does not yet break out\n",
				 idx);
		pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
				 "delete: not supported (an external holder cannot be cleared from here) -- use `echo force2 > ../status` for the aggressive override\n");
		break;
	default:
		pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
				 "unknown resource holder\n");
		break;
	}

	pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
			 "\n"
			 "SAFE-RMMOD RULE: when this directory contains only the mount-* file(s) for the diagfs mount(s) actively keeping it busy (i.e. no hook-*/other-* remain, only the very mount you are reading through), it is safe to `umount` diagfs and then `rmmod lkm4ctr`.\n");
	return pos;
}

/*
 * lkm4ctr_diag_res_delete() forward declaration: the actual teardown reuses
 * lkm4ctr_auto_umount_diagfs()/shadow_hook_quiesce(), which are defined
 * further down with the self-unload machinery, so the body lives there.
 */
static int lkm4ctr_diag_res_delete(u64 key);

/*
 * lkm4ctr_diagfs_dyn_instance_snprintf() - render one per-instance leaf file
 * under a dynamic directory, dispatching on its domain: vendor_kernel
 * namespaces/IPC via the vns_diag_res_* vtable, or a diagfs-local resource
 * holder.
 */
static size_t lkm4ctr_diagfs_dyn_instance_snprintf(const struct lkm4ctr_diagfs_info *info,
						   char *buf, size_t buflen)
{
	if (info->dyn_domain == LKM4CTR_DYN_DOMAIN_VNS)
		return vns_diag_res_render(info->dyn_provider, info->dyn_key,
					   buf, buflen);
	if (info->dyn_domain == LKM4CTR_DYN_DOMAIN_RES)
		return lkm4ctr_diag_res_render(info->dyn_key, buf, buflen);
	return scnprintf(buf, buflen, "unknown dynamic resource\n");
}

/*
 * lkm4ctr_diagfs_references_snprintf() - explain *why* module_refcount() is
 * whatever it currently is, i.e. who is actually holding a reference to
 * this module right now. This is what "try again"/-EBUSY failures on
 * rmmod are ultimately about: module_refcount() itself is just a number,
 * this file is meant to answer "a reference to *what*, exactly?" without
 * an operator having to already know this module's internals.
 *
 * Every summand below is a real, tracked contributor:
 *   - the diagfs mount count (each active `mount -t lkm4ctr` pins a
 *     reference via file_system_type->owner, exactly like any other
 *     in-use filesystem module -- see lkm4ctr_diagfs_mount_count).
 *   - in-flight shadow_hook-redirected calls (shadow_hook_inflight_count(),
 *     see glue/shadow_hook.h and shadow_hijack.c).
 * Anything left over after subtracting those (and the worker thread's own
 * transient reference while a self-unload is in progress) is an
 * *external* reference this module cannot itself explain -- typically
 * another kernel module that links against one of lkm4ctr.ko's
 * EXPORT_SYMBOL_GPL() entry points (shadow_hook_install() etc.), or a
 * kernel object type this file doesn't yet know to break out (userspace
 * itself never holds a struct module reference merely by having a
 * pathname open).
 */
static size_t lkm4ctr_diagfs_references_snprintf(const struct lkm4ctr_diagfs_info *info,
						  char *buf, size_t buflen)
{
	size_t pos = 0;
	int refcount = -1;
	int mounts = atomic_read(&lkm4ctr_diagfs_mount_count);
	int inflight = shadow_hook_inflight_count();
	int accounted, other;

	(void)info;

	if (lkm4ctr_module_refcount_fn)
		refcount = lkm4ctr_module_refcount_fn(THIS_MODULE);

	if (refcount < 0) {
		pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
				 "module_refcount() unavailable (module_refcount symbol not yet resolved; write to /status at least once to trigger resolution)\n");
	} else {
		accounted = mounts + inflight;
		other = max(0, (refcount - 1) - accounted);
		pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
				 "module_refcount()=%d\n", refcount);
		pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
				 "  1 base reference (module is loaded)\n");
		pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
				 "  %d active lkm4ctr diagfs mount(s) (file_system_type->owner)\n",
				 mounts);
		pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
				 "  %d in-flight shadow_hook-redirected call(s) across every submodule\n",
				 inflight);
		pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
				 "  %d other/unaccounted reference(s)%s\n",
				 other,
				 other ?
				 " (likely another module using an EXPORT_SYMBOL_GPL() of this module, or a reference this file does not yet break out)" :
				 "");
	}

	pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
			 "submodule states:\n");
	{
		unsigned int i;

		for (i = 0; i < ARRAY_SIZE(lkm4ctr_diagfs_modules); i++) {
			struct lkm4ctr_diagfs_module *mod = &lkm4ctr_diagfs_modules[i];

			pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
					 "  %-16s %s\n", mod->tag,
					 lkm4ctr_diagfs_state_name(
						 lkm4ctr_diagfs_module_stable_state(mod)));
		}
	}

	return pos;
}

/*
 * Hot reload's own status/log/trigger renderers live in
 * lkm4ctr_hotreload.c (a self-contained subsystem, not gated by the usual
 * per-submodule load/unload lifecycle above); these are thin adapters to
 * the shared lkm4ctr_diagfs_render_fn signature, same pattern as the
 * vendor_kernel-backed listing renderers above.
 */
static size_t lkm4ctr_diagfs_hotreload_status_snprintf(const struct lkm4ctr_diagfs_info *info,
							char *buf, size_t buflen)
{
	(void)info;
	return lkm4ctr_hotreload_status_snprintf(buf, buflen);
}

static size_t lkm4ctr_diagfs_hotreload_trigger_snprintf(const struct lkm4ctr_diagfs_info *info,
							 char *buf, size_t buflen)
{
	(void)info;
	return scnprintf(buf, buflen,
			 "write an absolute path to a replacement lkm4ctr.ko to trigger a hot reload, e.g.:\n"
			 "  echo /path/to/new/lkm4ctr.ko > do-hot-reload\n"
			 "see ./status and ./log in this same directory for progress.\n");
}

/*
 * helper.sh is served verbatim from the C string generated into
 * lkm4ctr/helper.sh.c; unlike every other diagfs file this is not a
 * "live" render of kernel state, so the wrapper below just copies the
 * embedded script text (it is plain text and NUL-terminated, so %s is
 * exact and safe).
 */
static size_t lkm4ctr_diagfs_helper_script_snprintf(const struct lkm4ctr_diagfs_info *info,
						     char *buf, size_t buflen)
{
	(void)info;
	return scnprintf(buf, buflen, "%s", lkm4ctr_helper_sh_data);
}

/*
 * readme.txt is served verbatim from the C string generated into
 * lkm4ctr/readme.txt.c, the same way helper.sh is served above.
 */
static size_t lkm4ctr_diagfs_readme_snprintf(const struct lkm4ctr_diagfs_info *info,
					      char *buf, size_t buflen)
{
	(void)info;
	return scnprintf(buf, buflen, "%s", lkm4ctr_readme_txt_data);
}

typedef size_t (*lkm4ctr_diagfs_render_fn)(const struct lkm4ctr_diagfs_info *info,
						   char *buf, size_t buflen);

static lkm4ctr_diagfs_render_fn lkm4ctr_diagfs_render_for(enum lkm4ctr_diagfs_kind kind)
{
	switch (kind) {
	case LKM4CTR_DIAG_CONTROL:
		return lkm4ctr_diagfs_control_snprintf;
	case LKM4CTR_DIAG_STATUS:
		return lkm4ctr_diagfs_status_snprintf;
	case LKM4CTR_DIAG_ENABLED:
		return lkm4ctr_diagfs_enabled_snprintf;
	case LKM4CTR_DIAG_HOOKS:
		return lkm4ctr_diagfs_hooks_snprintf;
	case LKM4CTR_DIAG_NAMESPACES:
		return lkm4ctr_diagfs_namespaces_snprintf;
	case LKM4CTR_DIAG_LOG:
		return lkm4ctr_diagfs_log_snprintf;
	case LKM4CTR_DIAG_GLOBAL_RESOURCES:
		return lkm4ctr_diagfs_global_resources_snprintf;
	case LKM4CTR_DIAG_MQUEUE_MSG:
		return lkm4ctr_diagfs_mqueue_msg_snprintf;
	case LKM4CTR_DIAG_SYSVIPC_RESOURCES:
		return lkm4ctr_diagfs_sysvipc_resources_snprintf;
	case LKM4CTR_DIAG_REFERENCES:
		return lkm4ctr_diagfs_references_snprintf;
	case LKM4CTR_DIAG_HOTRELOAD_STATUS:
		return lkm4ctr_diagfs_hotreload_status_snprintf;
	case LKM4CTR_DIAG_HOTRELOAD_TRIGGER:
		return lkm4ctr_diagfs_hotreload_trigger_snprintf;
	case LKM4CTR_DIAG_HELPER_SCRIPT:
		return lkm4ctr_diagfs_helper_script_snprintf;
	case LKM4CTR_DIAG_README:
		return lkm4ctr_diagfs_readme_snprintf;
	case LKM4CTR_DIAG_DYN_INSTANCE:
		return lkm4ctr_diagfs_dyn_instance_snprintf;
	default:
		return NULL;
	}
}

/*
 * lkm4ctr_diagfs_show()'s grow-and-retry loop
 * -------------------------------------------
 * Every render function below is built out of repeated
 * scnprintf(buf + pos, buflen - pos, ...) accumulation, same as the ring
 * log's own renderer used to be. scnprintf() clamps its return value to
 * "bytes actually written" (glibc snprintf() semantics do NOT apply): once
 * @buf saturates mid-scan, every later call in the same accumulation gets a
 * remaining size of 0 or 1 and contributes nothing more, so the final
 * reported "need" deterministically comes out to exactly cap - 1 --
 * regardless of how much real content was actually left over. Trusting
 * that value at face value (the previous "if (need < cap) break;" check)
 * therefore silently truncated any content that didn't fit in the first
 * 4096-byte guess, instead of ever growing the buffer.
 *
 * Fixed by treating that exact "need == cap - 1" signature as "didn't fit,
 * keep growing" (need + 1 < cap is the only way to conclude the content
 * genuinely was smaller than @cap), and by growing @cap exponentially
 * (doubling) instead of the previous cap = need + 1 (which, given the bug
 * above, only ever grew by a single byte per retry). Growth stops at
 * LKM4CTR_DIAGFS_SHOW_MAX_CAP as a sanity ceiling; lkm4ctr_log_snprintf()
 * itself separately guarantees the log's tail is always what a
 * still-too-small @buf ends up holding (see lkm4ctr_log.c), so hitting that
 * ceiling on the log file never loses the most recent entries.
 */
#define LKM4CTR_DIAGFS_SHOW_INITIAL_CAP	4096
#define LKM4CTR_DIAGFS_SHOW_MAX_CAP		(1 * 1024 * 1024)

static int lkm4ctr_diagfs_show(struct seq_file *m, void *v)
{
	struct lkm4ctr_diagfs_info *info = m->private;
	lkm4ctr_diagfs_render_fn fn = lkm4ctr_diagfs_render_for(info->kind);
	char *buf;
	size_t cap = LKM4CTR_DIAGFS_SHOW_INITIAL_CAP, need;

	if (!fn)
		return -EINVAL;

	for (;;) {
		buf = kmalloc(cap, GFP_KERNEL);
		if (!buf)
			return -ENOMEM;
		need = fn(info, buf, cap);
		if (need + 1 < cap || cap >= LKM4CTR_DIAGFS_SHOW_MAX_CAP)
			break;
		kfree(buf);
		cap *= 2;
		if (cap > LKM4CTR_DIAGFS_SHOW_MAX_CAP)
			cap = LKM4CTR_DIAGFS_SHOW_MAX_CAP;
	}

	/*
	 * @need is the *true* required size as reported by the render
	 * function (lkm4ctr_log_snprintf() in particular reports this even
	 * when it exceeds @cap, so a grow-and-retry caller can tell it's
	 * still truncated). If growth stopped at LKM4CTR_DIAGFS_SHOW_MAX_CAP
	 * with @need still >= @cap, @buf itself only ever had @cap bytes
	 * allocated and written into (scnprintf() never writes past
	 * cap - 1 chars + a NUL) -- writing @need bytes to seq_write() in
	 * that case would read past the end of @buf. Clamp to what is
	 * actually present.
	 */
	seq_write(m, buf, need < cap ? need : cap - 1);
	kfree(buf);
	return 0;
}

static int lkm4ctr_diagfs_open(struct inode *inode, struct file *file)
{
	return single_open(file, lkm4ctr_diagfs_show, inode->i_private);
}

static const struct file_operations lkm4ctr_diagfs_ro_fops = {
	.owner		= THIS_MODULE,
	.open		= lkm4ctr_diagfs_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

/* ------------------------------------------------------------------- */
/* control write handling                                               */
/* ------------------------------------------------------------------- */

enum lkm4ctr_diagfs_control_cmd {
	LKM4CTR_CONTROL_LOAD,
	LKM4CTR_CONTROL_UNLOAD,
	LKM4CTR_CONTROL_FORCE_UNLOAD,
	LKM4CTR_CONTROL_FORCE_UNLOAD2,
};

static bool lkm4ctr_diagfs_is_unload_cmd(const char *cmd, bool is_global)
{
	if (!strcmp(cmd, "unload") || !strcmp(cmd, "remove") || !strcmp(cmd, "graceful"))
		return true;
	/* "1" and "off" are the module-wide ./mnt/status "turn it off" aliases. */
	return is_global && (!strcmp(cmd, "1") || !strcmp(cmd, "off"));
}

static int lkm4ctr_diagfs_parse_control_cmd(const char *cmd, bool is_global,
					    enum lkm4ctr_diagfs_control_cmd *out)
{
	if (!strcmp(cmd, "load") ||
	    (is_global && (!strcmp(cmd, "load_all") || !strcmp(cmd, "on")))) {
		/* "on" is the module-wide ./mnt/status "turn it on" alias. */
		*out = LKM4CTR_CONTROL_LOAD;
	} else if (lkm4ctr_diagfs_is_unload_cmd(cmd, is_global)) {
		*out = LKM4CTR_CONTROL_UNLOAD;
	} else if (!strcmp(cmd, "force") || !strcmp(cmd, "force_unload") ||
		   !strcmp(cmd, "forceunload")) {
		*out = LKM4CTR_CONTROL_FORCE_UNLOAD;
	} else if (is_global && !strcmp(cmd, "force2")) {
		/*
		 * "force2" is a module-wide-only escalation, deliberately not
		 * accepted as a first command on a fresh (not-yet-unloading)
		 * ./mnt/status -- see the file header note above and
		 * lkm4ctr_diagfs_control_write() below: it either escalates
		 * an already-running force-unload to the aggressive path, or
		 * (same as writing "force" then immediately "force2") starts
		 * one directly in aggressive mode.
		 */
		*out = LKM4CTR_CONTROL_FORCE_UNLOAD2;
	} else {
		return -EINVAL;
	}
	return 0;
}

static int lkm4ctr_diagfs_module_load(struct lkm4ctr_diagfs_module *mod,
					      bool from_global)
{
	int ret;

	if (!mod->mod_init)
		return 0;

	mutex_lock(&lkm4ctr_unload_lock);
	if (!from_global &&
	    (lkm4ctr_diagfs_global_state != LKM4CTR_STATE_ACTIVE ||
	     lkm4ctr_unload_in_progress ||
	     lkm4ctr_diagfs_any_module_transition_locked())) {
		mutex_unlock(&lkm4ctr_unload_lock);
		return -EBUSY;
	}
	if (lkm4ctr_diagfs_module_stable_state(mod) == LKM4CTR_STATE_ACTIVE) {
		mutex_unlock(&lkm4ctr_unload_lock);
		LKM4CTR_INFO(mod->tag,
			     "load requested via diagfs control write, but already active; nothing to do");
		return 0;
	}
	mod->state = LKM4CTR_STATE_LOADING;
	mutex_unlock(&lkm4ctr_unload_lock);

	LKM4CTR_INFO(mod->tag, "load requested via diagfs control write");
	ret = mod->mod_init();

	mutex_lock(&lkm4ctr_unload_lock);
	mod->state = lkm4ctr_diagfs_module_stable_state(mod);
	mutex_unlock(&lkm4ctr_unload_lock);

	return ret;
}

static int lkm4ctr_diagfs_module_unload(struct lkm4ctr_diagfs_module *mod,
					bool force, bool from_global)
{
	if (!mod->mod_exit)
		return -EOPNOTSUPP;

	mutex_lock(&lkm4ctr_unload_lock);
	if (!from_global &&
	    (lkm4ctr_diagfs_global_state != LKM4CTR_STATE_ACTIVE ||
	     lkm4ctr_unload_in_progress ||
	     lkm4ctr_diagfs_any_module_transition_locked())) {
		mutex_unlock(&lkm4ctr_unload_lock);
		return -EBUSY;
	}
	if (!force && lkm4ctr_diagfs_module_stable_state(mod) == LKM4CTR_STATE_UNLOADED) {
		mutex_unlock(&lkm4ctr_unload_lock);
		LKM4CTR_INFO(mod->tag,
			     "unload requested via diagfs control write, but already inactive; nothing to do");
		return 0;
	}
	mod->state = force ? LKM4CTR_STATE_FORCE_UNLOADING :
		LKM4CTR_STATE_GRACEFUL_UNLOADING;
	mutex_unlock(&lkm4ctr_unload_lock);

	LKM4CTR_INFO(mod->tag, "%s unload requested via diagfs control write",
		     force ? "force" : "graceful");

	if (force && !from_global)
		shadow_hook_quiesce(true);
	mod->mod_exit();
	if (force && !from_global)
		shadow_hook_quiesce(false);

	mutex_lock(&lkm4ctr_unload_lock);
	mod->state = lkm4ctr_diagfs_module_stable_state(mod);
	mutex_unlock(&lkm4ctr_unload_lock);

	LKM4CTR_INFO(mod->tag, "%s unload complete", force ? "force" : "graceful");
	return 0;
}

static int lkm4ctr_diagfs_load_all(void)
{
	unsigned int i;
	int ret = 0;

	mutex_lock(&lkm4ctr_unload_lock);
	if (lkm4ctr_unload_in_progress ||
	    lkm4ctr_diagfs_global_state != LKM4CTR_STATE_ACTIVE ||
	    lkm4ctr_diagfs_any_module_transition_locked()) {
		mutex_unlock(&lkm4ctr_unload_lock);
		return -EBUSY;
	}
	lkm4ctr_diagfs_global_state = LKM4CTR_STATE_LOADING;
	mutex_unlock(&lkm4ctr_unload_lock);

	LKM4CTR_INFO(LKM4CTR_DIAGFS_TAG, "load-all requested via diagfs");
	for (i = 0; i < ARRAY_SIZE(lkm4ctr_diagfs_modules); i++) {
		struct lkm4ctr_diagfs_module *mod = &lkm4ctr_diagfs_modules[i];
		int err;

		if (!mod->mod_init)
			continue;

		err = lkm4ctr_diagfs_module_load(mod, true);
		if (err && !ret)
			ret = err;
	}

	mutex_lock(&lkm4ctr_unload_lock);
	lkm4ctr_diagfs_global_state = LKM4CTR_STATE_ACTIVE;
	mutex_unlock(&lkm4ctr_unload_lock);

	if (ret)
		LKM4CTR_INFO(LKM4CTR_DIAGFS_TAG,
			     "load-all complete with at least one failure; see the per-submodule log lines above");
	else
		LKM4CTR_INFO(LKM4CTR_DIAGFS_TAG, "load-all complete");

	return ret;
}

static void lkm4ctr_diagfs_unload_all_now(const char *label, bool force)
{
	unsigned int i;

	LKM4CTR_INFO(LKM4CTR_DIAGFS_TAG, "%s-unloading every submodule", label);
	for (i = 0; i < ARRAY_SIZE(lkm4ctr_diagfs_modules); i++) {
		struct lkm4ctr_diagfs_module *mod = &lkm4ctr_diagfs_modules[i];

		if (!mod->mod_exit)
			continue;
		lkm4ctr_diagfs_module_unload(mod, force, true);
	}
	LKM4CTR_INFO(LKM4CTR_DIAGFS_TAG,
		     "%s-unload of every submodule complete; all of their resources are now free",
		     label);
}

#define LKM4CTR_SAFE_UNLOAD_TIMEOUT_MS	30000
#define LKM4CTR_SAFE_UNLOAD_POLL_MS	50
#define LKM4CTR_SAFE_UNLOAD_TAG		"safe_unload"

static const char * const lkm4ctr_rmmod_candidates[] = {
	"/system/bin/rmmod",
	"/sbin/rmmod",
	"/usr/sbin/rmmod",
	"/usr/bin/rmmod",
	"/bin/rmmod",
	NULL,
};

static const char * const lkm4ctr_umount_candidates[] = {
	"/system/bin/umount",
	"/sbin/umount",
	"/usr/sbin/umount",
	"/usr/bin/umount",
	"/bin/umount",
	NULL,
};

/*
 * module_refcount_fn/call_usermodehelper_fn are declared earlier (near
 * lkm4ctr_diagfs_resolve()) so the "references" renderer can use them too;
 * only the lazy resolver itself lives here.
 */
#define LKM4CTR_RESOLVE_ONE(fn, name)						\
	do {									\
		if (!(fn)) {							\
			(fn) = (typeof(fn))lkm4ctr_diagfs_resolve(name);	\
			if (!(fn))						\
				LKM4CTR_WARN(LKM4CTR_SAFE_UNLOAD_TAG,		\
					     "could not resolve %s; global unload unavailable", \
					     name);				\
		}								\
	} while (0)

static bool lkm4ctr_safe_unload_resolve(void)
{
	LKM4CTR_RESOLVE_ONE(lkm4ctr_module_refcount_fn, "module_refcount");
	LKM4CTR_RESOLVE_ONE(lkm4ctr_call_usermodehelper_fn, "call_usermodehelper");

	return lkm4ctr_module_refcount_fn && lkm4ctr_call_usermodehelper_fn;
}

/* ------------------------------------------------------------------- */
/* global self-unload rationale (unchanged from the removed misc device) */
/* ------------------------------------------------------------------- */

/*
 * The self-unload hazard
 * -----------------------
 * A module can never *directly* free the memory its own currently
 * executing code lives in -- whatever function is doing the freeing would
 * have to keep running afterwards to return to its caller, straight into
 * pages that no longer exist. This is why every practical "self-unload"
 * design (including this one) is actually two cooperating pieces:
 *
 *   1. A worker kthread, spawned by the control-file write() below, that
 *      does the waiting (quiesce + poll module_refcount()) and then asks a
 *      real userspace process to do the actual `rmmod`/`modprobe -r` --
 *      module removal is always driven by an external process calling
 *      delete_module(2); no in-kernel API removes "the currently running
 *      module" from within itself.
 *   2. module_put_and_kthread_exit(), used instead of a normal return from
 *      the worker thread's body once its job is done. This is the one
 *      piece of core kernel code (kernel/module/main.c, *not* our module's
 *      .text) whose entire purpose is to drop the worker's own module
 *      reference and terminate the thread as a single atomic step from the
 *      kernel's point of view, so that no instruction belonging to
 *      lkm4ctr.ko executes after the reference that was keeping the module
 *      alive for the worker's own sake is gone. On kernels old enough to
 *      predate that helper (introduced upstream alongside kthread_exit()
 *      around v5.17), the equivalent classic module_put_and_exit()/
 *      do_exit() pairing is used instead.
 *
 * With both pieces in place, module_refcount() only ever reaches zero
 * after every hooked call *and* the worker thread itself has stopped
 * touching the module's code, so the external `rmmod` this file spawns is
 * operating on a module that is genuinely idle, not one that merely
 * *looks* idle from a racing kthread's perspective.
 */

static int lkm4ctr_auto_umount_diagfs(void)
{
	char *envp[] = { "HOME=/", "PATH=/sbin:/usr/sbin:/bin:/usr/bin:/system/bin", NULL };
	const char * const *path;
	int mounts = atomic_read(&lkm4ctr_diagfs_mount_count);
	int ret = -ENOENT;

	if (mounts <= 0) {
		LKM4CTR_INFO(LKM4CTR_SAFE_UNLOAD_TAG,
			     "no lkm4ctr diagfs mount currently active, nothing to auto-unmount");
		return 0;
	}

	LKM4CTR_INFO(LKM4CTR_SAFE_UNLOAD_TAG,
		     "auto-unmounting %d active lkm4ctr diagfs mount(s) before self-unload",
		     mounts);

	for (path = lkm4ctr_umount_candidates; *path; path++) {
		char *argv[] = { (char *)*path, "-l", "-a", "-t", "lkm4ctr", NULL };

		ret = lkm4ctr_call_usermodehelper_fn(*path, argv, envp, UMH_WAIT_PROC);
		if (ret == 0) {
			LKM4CTR_INFO(LKM4CTR_SAFE_UNLOAD_TAG,
				     "ran \"%s -l -a -t lkm4ctr\"", *path);
			break;
		}
		LKM4CTR_WARN(LKM4CTR_SAFE_UNLOAD_TAG,
			     "\"%s -l -a -t lkm4ctr\" failed or exited non-zero: %d",
			     *path, ret);
	}

	mounts = atomic_read(&lkm4ctr_diagfs_mount_count);
	if (mounts > 0) {
		LKM4CTR_WARN(LKM4CTR_SAFE_UNLOAD_TAG,
			     "%d lkm4ctr diagfs mount(s) still marked active after the auto-unmount attempt",
			     mounts);
		LKM4CTR_WARN(LKM4CTR_SAFE_UNLOAD_TAG,
			     "cause: no working umount binary was found on this system, or the lazy-detach superblock is still waiting on the last held reference to actually drop");
		LKM4CTR_WARN(LKM4CTR_SAFE_UNLOAD_TAG,
			     "resolution: self-unload will time out below unless these are cleared -- see the timeout guidance further down this log for exact resolution steps");
	} else {
		LKM4CTR_INFO(LKM4CTR_SAFE_UNLOAD_TAG,
			     "all lkm4ctr diagfs mounts successfully auto-unmounted");
	}

	return ret;
}

static int lkm4ctr_run_rmmod(bool aggressive)
{
	char *envp[] = { "HOME=/", "PATH=/sbin:/usr/sbin:/bin:/usr/bin:/system/bin", NULL };
	const char * const *path;
	int ret = -ENOENT;

	for (path = lkm4ctr_rmmod_candidates; *path; path++) {
		/*
		 * "-f" is rmmod(8)'s force flag (delete_module(2)'s O_TRUNC),
		 * requiring CONFIG_MODULE_FORCE_UNLOAD on the target kernel:
		 * it tells the kernel to ignore module_refcount() (and, on
		 * kernels new enough to still check it, the module version)
		 * entirely rather than returning -EBUSY/-EWOULDBLOCK/-EAGAIN.
		 * Only ever passed by the force2 path below, after this
		 * worker has already forcibly overridden module_refcount()
		 * itself (see lkm4ctr_force2_override_refcount()) -- "-f" is
		 * this file's one remaining line of defense if that somehow
		 * still wasn't enough.
		 */
		char *argv_plain[] = { (char *)*path, "lkm4ctr", NULL };
		char *argv_force[] = { (char *)*path, "-f", "lkm4ctr", NULL };
		char **argv = aggressive ? argv_force : argv_plain;

		ret = lkm4ctr_call_usermodehelper_fn(*path, argv, envp, UMH_WAIT_EXEC);
		if (ret == 0) {
			LKM4CTR_INFO(LKM4CTR_SAFE_UNLOAD_TAG, "launched \"%s%s lkm4ctr\"",
				     *path, aggressive ? " -f" : "");
			return 0;
		}
		LKM4CTR_WARN(LKM4CTR_SAFE_UNLOAD_TAG, "\"%s\" failed to exec: %d", *path, ret);
	}

	LKM4CTR_ERR(LKM4CTR_SAFE_UNLOAD_TAG,
		    "no rmmod candidate could be exec'd (last error %d)", ret);
	LKM4CTR_ERR(LKM4CTR_SAFE_UNLOAD_TAG,
		    "cause: module was successfully quiesced and drained, so the module itself is not the problem -- likely no rmmod (or busybox applet providing it) is installed/executable on this system");
	LKM4CTR_ERR(LKM4CTR_SAFE_UNLOAD_TAG,
		    "resolution: ensure a working rmmod exists and is executable on one of /system/bin, /sbin, /usr/sbin, /usr/bin, /bin, then retry -- or simply run `rmmod%s lkm4ctr` by hand right now, it will succeed immediately since hooks are already quiesced and the refcount is already drained; module left quiesced but loaded",
		    aggressive ? " -f" : "");
	return ret;
}

/*
 * lkm4ctr_force2_override_refcount() - the aggressive core of "force2".
 *
 * module_put(THIS_MODULE) is an ordinary, always-inline, always-linked
 * kernel helper (include/linux/module.h) -- unlike module_refcount()/
 * call_usermodehelper() elsewhere in this file, it needs no
 * lkm4ctr_diagfs_resolve() trick, so it is always available. Calling it
 * additional times beyond what this module's own bookkeeping actually
 * released is exactly the "hook related kernel functions and do the force
 * rmmod ignoring anything" escalation this command is documented (control
 * file help text, README.md) to be: it directly overrides module_refcount()
 * regardless of *why* it was still non-zero (an in-flight shadow_hook call,
 * an unexpectedly-still-mounted diagfs, or any other external reference),
 * rather than waiting for or explaining it. This is unsafe in the general
 * case -- if a reference genuinely reflected a CPU still executing inside
 * this module's .text, dropping it early does not stop that CPU, it only
 * hides the fact from module_refcount() -- but "force2" is explicitly an
 * administrator override of last resort for a module that refuses to
 * rmmod any other way, exactly like `rmmod -f` itself already is.
 */
#define LKM4CTR_FORCE2_MAX_ITER		4096

static void lkm4ctr_force2_override_refcount(void)
{
	int refcount = lkm4ctr_module_refcount_fn(THIS_MODULE);
	int dropped = 0;

	LKM4CTR_WARN(LKM4CTR_SAFE_UNLOAD_TAG,
		     "force2: aggressively overriding module_refcount()=%d by calling module_put() %d extra time(s), ignoring whether each reference has genuinely finished",
		     refcount, refcount > 1 ? refcount - 1 : 0);

	while (refcount > 1 && dropped < LKM4CTR_FORCE2_MAX_ITER) {
		module_put(THIS_MODULE);
		dropped++;
		refcount = lkm4ctr_module_refcount_fn(THIS_MODULE);
		LKM4CTR_WARN(LKM4CTR_SAFE_UNLOAD_TAG,
			     "force2: module_put() #%d done, module_refcount() now %d",
			     dropped, refcount);
	}

	if (refcount > 1)
		LKM4CTR_ERR(LKM4CTR_SAFE_UNLOAD_TAG,
			    "force2: module_refcount() still %d after %d overriding module_put() call(s) (hit the %d-iteration safety ceiling); proceeding to rmmod -f anyway",
			    refcount, dropped, LKM4CTR_FORCE2_MAX_ITER);
	else
		LKM4CTR_WARN(LKM4CTR_SAFE_UNLOAD_TAG,
			     "force2: module_refcount() forced down to %d after %d overriding module_put() call(s)",
			     refcount, dropped);
}

static int lkm4ctr_safe_unload_fn(void *unused)
{
	unsigned long waited_ms = 0;
	int ret;
	int refcount;
	bool force;
	bool force2;

	mutex_lock(&lkm4ctr_unload_lock);
	force = lkm4ctr_unload_force;
	force2 = lkm4ctr_unload_force2;
	mutex_unlock(&lkm4ctr_unload_lock);

	__module_get(THIS_MODULE);

	LKM4CTR_INFO(LKM4CTR_SAFE_UNLOAD_TAG,
		     "%s unload started: quiescing hooks (in-flight shadow_hook-redirected syscalls are allowed to finish, new entries are refused with -EAGAIN)",
		     force2 ? "force2" : force ? "force" : "safe");
	shadow_hook_quiesce(true);

	if (force)
		lkm4ctr_diagfs_unload_all_now(force2 ? "force2" : "force", true);

	lkm4ctr_auto_umount_diagfs();

	refcount = lkm4ctr_module_refcount_fn(THIS_MODULE);
	LKM4CTR_INFO(LKM4CTR_SAFE_UNLOAD_TAG,
		     "module_refcount()=%d after quiesce (%d diagfs mount(s), %d in-flight hook call(s) currently accounted for; must drop to 1, i.e. only this worker's own reference, before rmmod can succeed; timeout is %ums, after which force unload proceeds to rmmod anyway despite outstanding references)",
		     refcount, atomic_read(&lkm4ctr_diagfs_mount_count),
		     shadow_hook_inflight_count(), LKM4CTR_SAFE_UNLOAD_TIMEOUT_MS);

	while (!force2 && (refcount = lkm4ctr_module_refcount_fn(THIS_MODULE)) > 1) {
		bool escalated = false;
		bool escalated2 = false;

		mutex_lock(&lkm4ctr_unload_lock);
		if (!force && lkm4ctr_unload_force) {
			force = true;
			escalated = true;
		}
		if (!force2 && lkm4ctr_unload_force2) {
			force2 = true;
			escalated2 = true;
		}
		mutex_unlock(&lkm4ctr_unload_lock);

		if (escalated2) {
			LKM4CTR_WARN(LKM4CTR_SAFE_UNLOAD_TAG,
				     "unload escalated to the aggressive force2 path mid-wait via diagfs control write: force-unloading every submodule (if not already) and forcibly overriding the remaining reference(s) instead of continuing to wait");
			if (!escalated)
				lkm4ctr_diagfs_unload_all_now("force2", true);
			break;
		}
		if (escalated) {
			LKM4CTR_INFO(LKM4CTR_SAFE_UNLOAD_TAG,
				     "unload escalated to force mid-wait via diagfs control write: force-unloading every submodule immediately instead of continuing to wait for the graceful drain");
			lkm4ctr_diagfs_unload_all_now("force", true);
		}

		if (waited_ms >= LKM4CTR_SAFE_UNLOAD_TIMEOUT_MS) {
			int mounts = atomic_read(&lkm4ctr_diagfs_mount_count);

			LKM4CTR_ERR(LKM4CTR_SAFE_UNLOAD_TAG,
				    "timed out after %lums waiting for %d extra reference(s) to drain (module_refcount()=%d)",
				    waited_ms, refcount - 1, refcount);
			if (mounts > 0) {
				LKM4CTR_ERR(LKM4CTR_SAFE_UNLOAD_TAG,
					    "cause: this lkm4ctr diagfs is currently mounted %d time(s); every active mount pins module_refcount() via file_system_type->owner, exactly like rmmod refuses any other in-use filesystem module, and this alone will block self-unload forever",
					    mounts);
				LKM4CTR_ERR(LKM4CTR_SAFE_UNLOAD_TAG,
					    "resolution: `umount` every mountpoint of type \"lkm4ctr\" (check with `grep lkm4ctr /proc/mounts`) -- including the one you may be reading/writing /status through right now -- then write to /status again, or escalate to `echo force2 > /status` to override it forcibly");
			} else {
				if (force)
					LKM4CTR_ERR(LKM4CTR_SAFE_UNLOAD_TAG,
						    "cause: %d extra reference(s) remain with no lkm4ctr diagfs mounted, so a shadow_hook-redirected syscall is most likely still executing in another task (%d currently tracked in-flight)",
						    refcount - 1, shadow_hook_inflight_count());
				else
					LKM4CTR_ERR(LKM4CTR_SAFE_UNLOAD_TAG,
						    "cause: %d extra reference(s) remain with no lkm4ctr diagfs mounted, so a shadow_hook-redirected syscall is most likely still executing in another task (%d currently tracked in-flight), or a resource created via a hook (e.g. an anon-inode fd) is still held open",
						    refcount - 1, shadow_hook_inflight_count());
				LKM4CTR_ERR(LKM4CTR_SAFE_UNLOAD_TAG,
					    "resolution: this is a real in-flight kernel call still executing somewhere -- it cannot be forced to finish sooner without risking a crash, so simply wait and retry; if the count never drops on retry this may be a reference leak worth reporting, or escalate to `echo force2 > /status` to override it forcibly (unsafe, last resort)");
			}

			if (force) {
				/*
				 * A force unload must always ultimately reach rmmod:
				 * an administrator who wrote "forceunload" must
				 * always be able to unload this module manually, so
				 * our own internal wait/timeout bookkeeping is never
				 * allowed to keep blocking that indefinitely. Proceed
				 * to rmmod below instead of aborting -- the kernel's
				 * own module_refcount() check inside
				 * sys_delete_module() remains the real,
				 * un-bypassable safety net beyond this point: if
				 * still genuinely unsafe, rmmod will simply fail
				 * with its own -EBUSY rather than crashing.
				 */
				LKM4CTR_WARN(LKM4CTR_SAFE_UNLOAD_TAG,
					     "force unload: proceeding to rmmod despite %d outstanding reference(s) instead of aborting -- administrator override always takes priority; retry manually (or via forceunload again, or force2 for the aggressive override) if the kernel itself still refuses",
					     refcount - 1);
				break;
			}

			LKM4CTR_ERR(LKM4CTR_SAFE_UNLOAD_TAG, "aborting, module remains loaded");
			shadow_hook_quiesce(false);
			goto abort;
		}
		LKM4CTR_INFO(LKM4CTR_SAFE_UNLOAD_TAG,
			     "still waiting after %lums: module_refcount()=%d (%d extra reference(s) remaining: %d diagfs mount(s), %d in-flight hook call(s), %d other; %lums until timeout)",
			     waited_ms, refcount, refcount - 1,
			     atomic_read(&lkm4ctr_diagfs_mount_count),
			     shadow_hook_inflight_count(),
			     max(0, (refcount - 1) - atomic_read(&lkm4ctr_diagfs_mount_count) -
				 shadow_hook_inflight_count()),
			     LKM4CTR_SAFE_UNLOAD_TIMEOUT_MS - waited_ms);
		msleep(LKM4CTR_SAFE_UNLOAD_POLL_MS);
		waited_ms += LKM4CTR_SAFE_UNLOAD_POLL_MS;
	}

	if (!force && !force2) {
		LKM4CTR_INFO(LKM4CTR_SAFE_UNLOAD_TAG,
			     "drained after %lums (module_refcount()=%d), about to gracefully unload every submodule",
			     waited_ms, refcount);
		lkm4ctr_diagfs_unload_all_now("graceful", false);
	} else if (force2) {
		/*
		 * force2 always aggressively unloads every submodule first
		 * (idempotent if lkm4ctr_diagfs_unload_all_now() already ran
		 * above from the initial force branch or a mid-wait
		 * escalation), then forcibly drops any remaining
		 * module_refcount() instead of continuing to wait/poll for
		 * it, and finally requests the aggressive `rmmod -f`.
		 */
		lkm4ctr_diagfs_unload_all_now("force2", true);
		lkm4ctr_force2_override_refcount();
	}

	refcount = lkm4ctr_module_refcount_fn(THIS_MODULE);
	LKM4CTR_INFO(LKM4CTR_SAFE_UNLOAD_TAG,
		     "drained after %lums (module_refcount()=%d), launching rmmod%s",
		     waited_ms, refcount, force2 ? " -f (force2)" : "");
	ret = lkm4ctr_run_rmmod(force2);
	if (ret) {
		shadow_hook_quiesce(false);
		goto abort;
	}

	LKM4CTR_INFO(LKM4CTR_SAFE_UNLOAD_TAG, "rmmod launched successfully, module is unloading");

	mutex_lock(&lkm4ctr_unload_lock);
	lkm4ctr_unload_in_progress = false;
	lkm4ctr_unload_force = false;
	lkm4ctr_unload_force2 = false;
	mutex_unlock(&lkm4ctr_unload_lock);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 17, 0)
	module_put_and_kthread_exit(0);
#else
	lkm4ctr_module_put_and_exit(0);
#endif

abort:
	mutex_lock(&lkm4ctr_unload_lock);
	lkm4ctr_unload_in_progress = false;
	lkm4ctr_unload_force = false;
	lkm4ctr_unload_force2 = false;
	lkm4ctr_diagfs_global_state = LKM4CTR_STATE_ACTIVE;
	mutex_unlock(&lkm4ctr_unload_lock);
	module_put(THIS_MODULE);
	return 0;
}

static ssize_t lkm4ctr_diagfs_control_write(struct file *file,
					    const char __user *ubuf,
					    size_t count, loff_t *ppos)
{
	struct seq_file *m = file->private_data;
	struct lkm4ctr_diagfs_info *info = m->private;
	char cmd[16];
	enum lkm4ctr_diagfs_control_cmd action;
	struct lkm4ctr_diagfs_module *mod;

	(void)ppos;
	if (count == 0 || count >= sizeof(cmd))
		return -EINVAL;
	if (copy_from_user(cmd, ubuf, count))
		return -EFAULT;
	cmd[count] = '\0';
	strim(cmd);

	if (lkm4ctr_diagfs_parse_control_cmd(cmd, info->is_global, &action))
		return -EINVAL;

	if (info->is_global) {
		struct task_struct *thread;
		bool force;
		bool force2;

		if (action == LKM4CTR_CONTROL_LOAD) {
			int ret = lkm4ctr_diagfs_load_all();

			return ret ? ret : count;
		}

		force2 = (action == LKM4CTR_CONTROL_FORCE_UNLOAD2);
		force = force2 || (action == LKM4CTR_CONTROL_FORCE_UNLOAD);
		if (!lkm4ctr_safe_unload_resolve())
			return -EOPNOTSUPP;

		mutex_lock(&lkm4ctr_unload_lock);
		if (lkm4ctr_unload_in_progress) {
			/*
			 * An unload is already running (graceful, force, or
			 * force2). Never answer a repeated write here with
			 * -EBUSY: that would be exactly the kind of
			 * self-imposed block an administrator writing
			 * "forceunload"/"force2" must always be able to get
			 * past. If it's not already in that mode, escalate it
			 * in place -- lkm4ctr_safe_unload_fn()'s wait loop
			 * polls these same flags and switches path (force:
			 * skip further waiting, force-unload every submodule,
			 * then drive rmmod; force2: additionally skip straight
			 * to forcibly overriding module_refcount() and running
			 * `rmmod -f`) on its very next check. A repeated write
			 * in the same mode is simply a no-op success.
			 */
			if (force2 && !lkm4ctr_unload_force2) {
				lkm4ctr_unload_force = true;
				lkm4ctr_unload_force2 = true;
				lkm4ctr_diagfs_global_state = LKM4CTR_STATE_FORCE_UNLOADING;
				mutex_unlock(&lkm4ctr_unload_lock);
				LKM4CTR_WARN(LKM4CTR_DIAGFS_TAG,
					     "escalating already in-progress unload to the aggressive force2 path via diagfs control write: any outstanding reference will now be forcibly overridden instead of waited on");
				return count;
			}
			if (force && !lkm4ctr_unload_force) {
				lkm4ctr_unload_force = true;
				lkm4ctr_diagfs_global_state = LKM4CTR_STATE_FORCE_UNLOADING;
				mutex_unlock(&lkm4ctr_unload_lock);
				LKM4CTR_INFO(LKM4CTR_DIAGFS_TAG,
					     "escalating already in-progress unload to force via diagfs control write");
				return count;
			}
			mutex_unlock(&lkm4ctr_unload_lock);
			return count;
		}
		if (lkm4ctr_diagfs_global_state != LKM4CTR_STATE_ACTIVE ||
		    lkm4ctr_diagfs_any_module_transition_locked()) {
			mutex_unlock(&lkm4ctr_unload_lock);
			return -EBUSY;
		}
		lkm4ctr_unload_in_progress = true;
		lkm4ctr_unload_force = force;
		lkm4ctr_unload_force2 = force2;
		lkm4ctr_diagfs_global_state = force ?
			LKM4CTR_STATE_FORCE_UNLOADING :
			LKM4CTR_STATE_GRACEFUL_UNLOADING;
		mutex_unlock(&lkm4ctr_unload_lock);

		thread = kthread_run(lkm4ctr_safe_unload_fn, NULL, "lkm4ctr_unload");
		if (IS_ERR(thread)) {
			mutex_lock(&lkm4ctr_unload_lock);
			lkm4ctr_unload_in_progress = false;
			lkm4ctr_diagfs_global_state = LKM4CTR_STATE_ACTIVE;
			mutex_unlock(&lkm4ctr_unload_lock);
			return PTR_ERR(thread);
		}
		lkm4ctr_unload_thread = thread;
		return count;
	}

	mod = lkm4ctr_diagfs_find_module(info->tag);
	if (!mod)
		return -ENOSYS;

	switch (action) {
	case LKM4CTR_CONTROL_LOAD: {
		int ret = lkm4ctr_diagfs_module_load(mod, false);

		if (ret) {
			LKM4CTR_ERR(info->tag,
				    "load request failed (%d); see the messages just above in this same log for exactly which hook group and underlying error blocked it, and %s/log for the full log",
				    ret, info->tag);
			return ret;
		}
		break;
	}
	case LKM4CTR_CONTROL_UNLOAD: {
		int ret = lkm4ctr_diagfs_module_unload(mod, false, false);

		if (ret)
			return ret;
		break;
	}
	case LKM4CTR_CONTROL_FORCE_UNLOAD: {
		int ret = lkm4ctr_diagfs_module_unload(mod, true, false);

		if (ret)
			return ret;
		break;
	}
	default:
		/* force2 is a global-only escalation; no per-module meaning. */
		return -ENOSYS;
	}

	return count;
}

static const struct file_operations lkm4ctr_diagfs_control_fops = {
	.owner		= THIS_MODULE,
	.open		= lkm4ctr_diagfs_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
	.write		= lkm4ctr_diagfs_control_write,
};

/*
 * ./mnt/v/enabled write handler - a meaningful debug toggle of the
 * vendor_kernel_enabled flag/subsystem: "1"/"on" loads vendor_kernel,
 * "0"/"off" unloads it (routing through the same
 * lkm4ctr_diagfs_module_load()/unload() paths ./mnt/v/control uses, so the
 * lifecycle bookkeeping and locking stay identical). Read renders the flag.
 */
static ssize_t lkm4ctr_diagfs_enabled_write(struct file *file,
					    const char __user *ubuf,
					    size_t count, loff_t *ppos)
{
	char cmd[16];
	struct lkm4ctr_diagfs_module *mod;
	bool enable;
	int ret;

	(void)file;
	(void)ppos;
	if (count == 0 || count >= sizeof(cmd))
		return -EINVAL;
	if (copy_from_user(cmd, ubuf, count))
		return -EFAULT;
	cmd[count] = '\0';
	strim(cmd);

	if (!strcmp(cmd, "1") || !strcmp(cmd, "on"))
		enable = true;
	else if (!strcmp(cmd, "0") || !strcmp(cmd, "off"))
		enable = false;
	else
		return -EINVAL;

	mod = lkm4ctr_diagfs_find_module("vendor_kernel");
	if (!mod)
		return -ENOSYS;

	ret = enable ? lkm4ctr_diagfs_module_load(mod, false) :
		       lkm4ctr_diagfs_module_unload(mod, false, false);
	return ret ? ret : count;
}

static const struct file_operations lkm4ctr_diagfs_enabled_fops = {
	.owner		= THIS_MODULE,
	.open		= lkm4ctr_diagfs_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
	.write		= lkm4ctr_diagfs_enabled_write,
};

/*
 * do-hot-reload write handler. Deliberately tiny: all the real work
 * (validation, quiesce/drain, spawning the rmmod+insmod handoff) lives in
 * lkm4ctr_hotreload.c's lkm4ctr_hotreload_trigger(); this is purely the
 * diagfs glue, same division of labour as
 * lkm4ctr_diagfs_control_write()/lkm4ctr_safe_unload_fn() above.
 */
#define LKM4CTR_HOTRELOAD_PATH_MAX	256

static ssize_t lkm4ctr_diagfs_hotreload_trigger_write(struct file *file,
						      const char __user *ubuf,
						      size_t count, loff_t *ppos)
{
	char path[LKM4CTR_HOTRELOAD_PATH_MAX];
	int ret;

	(void)file;
	(void)ppos;
	if (count == 0 || count >= sizeof(path))
		return -EINVAL;
	if (copy_from_user(path, ubuf, count))
		return -EFAULT;
	path[count] = '\0';
	strim(path);
	if (!path[0])
		return -EINVAL;

	ret = lkm4ctr_hotreload_trigger(path);
	if (ret)
		return ret;
	return count;
}

static const struct file_operations lkm4ctr_diagfs_hotreload_trigger_fops = {
	.owner		= THIS_MODULE,
	.open		= lkm4ctr_diagfs_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
	.write		= lkm4ctr_diagfs_hotreload_trigger_write,
};

/* ------------------------------------------------------------------- */
/* dynamic directories: ./mnt/resources/ and ./mnt/v/{ns,ipc}/<type>/   */
/* ------------------------------------------------------------------- */

#define LKM4CTR_RES_HOOK_DRAIN_ITERS	200
#define LKM4CTR_RES_HOOK_DRAIN_POLL_MS	25

/*
 * lkm4ctr_diag_res_delete() (declared far above with the other renderers) -
 * tear down one ./mnt/resources holder. Defined here because it reuses the
 * self-unload machinery (lkm4ctr_auto_umount_diagfs(), shadow_hook_quiesce())
 * defined above.
 */
static int lkm4ctr_diag_res_delete(u64 key)
{
	int type = (int)(key >> 32);
	int i;

	switch (type) {
	case LKM4CTR_RES_MOUNT:
		if (!lkm4ctr_safe_unload_resolve())
			return -EOPNOTSUPP;
		/*
		 * Aggregate (honest limitation): there is no way to umount one
		 * specific lkm4ctr mount by index, so this lazily umounts every
		 * lkm4ctr diagfs mount, reusing the exact auto-umount path the
		 * self-unload sequence already uses. Because it is a lazy
		 * detach ("umount -l"), doing this through the very mount being
		 * unmounted does not deadlock on the dentry we hold.
		 */
		lkm4ctr_auto_umount_diagfs();
		return 0;
	case LKM4CTR_RES_HOOK:
		/*
		 * Aggregate (honest limitation): quiesce new redirects, drain
		 * ALL in-flight hook calls with the same bounded msleep-poll
		 * idiom as lkm4ctr_safe_unload_fn(), then re-enable. A single
		 * in-flight call is not individually targetable, and a
		 * genuinely stuck call cannot be force-killed here.
		 */
		shadow_hook_quiesce(true);
		for (i = 0; i < LKM4CTR_RES_HOOK_DRAIN_ITERS &&
			    shadow_hook_inflight_count() > 0; i++)
			msleep(LKM4CTR_RES_HOOK_DRAIN_POLL_MS);
		shadow_hook_quiesce(false);
		return 0;
	case LKM4CTR_RES_OTHER:
	default:
		/* An external holder cannot be cleared from here. */
		return -EOPNOTSUPP;
	}
}

/* Name <-> key codec for a dynamic directory, dispatched on its domain. */
static bool lkm4ctr_diagfs_dyn_name_to_key(const struct lkm4ctr_diagfs_info *di,
					   const char *name, u64 *key)
{
	if (di->dyn_domain == LKM4CTR_DYN_DOMAIN_VNS) {
		unsigned long long v;

		if (kstrtoull(name, 10, &v))
			return false;
		*key = v;
		return true;
	}
	if (di->dyn_domain == LKM4CTR_DYN_DOMAIN_RES) {
		unsigned long long v;
		const char *num;
		int type;

		if (!strncmp(name, "mount-", 6)) {
			type = LKM4CTR_RES_MOUNT;
			num = name + 6;
		} else if (!strncmp(name, "hook-", 5)) {
			type = LKM4CTR_RES_HOOK;
			num = name + 5;
		} else if (!strncmp(name, "other-", 6)) {
			type = LKM4CTR_RES_OTHER;
			num = name + 6;
		} else {
			return false;
		}
		if (kstrtoull(num, 10, &v) || v > U32_MAX)
			return false;
		*key = ((u64)type << 32) | (u32)v;
		return true;
	}
	return false;
}

static int lkm4ctr_diagfs_dyn_key_to_name(const struct lkm4ctr_diagfs_info *di,
					  u64 key, char *buf, size_t sz)
{
	if (di->dyn_domain == LKM4CTR_DYN_DOMAIN_VNS)
		return scnprintf(buf, sz, "%llu", key);
	if (di->dyn_domain == LKM4CTR_DYN_DOMAIN_RES) {
		int type = (int)(key >> 32);
		u32 idx = (u32)key;
		const char *p = type == LKM4CTR_RES_MOUNT ? "mount" :
				type == LKM4CTR_RES_HOOK ? "hook" : "other";

		return scnprintf(buf, sz, "%s-%u", p, idx);
	}
	return 0;
}

static bool lkm4ctr_diagfs_dyn_present(const struct lkm4ctr_diagfs_info *di, u64 key)
{
	if (di->dyn_domain == LKM4CTR_DYN_DOMAIN_VNS)
		return vns_diag_res_present(di->dyn_provider, key);
	if (di->dyn_domain == LKM4CTR_DYN_DOMAIN_RES)
		return lkm4ctr_diag_res_present(key);
	return false;
}

static int lkm4ctr_diagfs_dyn_snapshot(const struct lkm4ctr_diagfs_info *di,
				       u64 *keys, int max)
{
	if (di->dyn_domain == LKM4CTR_DYN_DOMAIN_VNS)
		return vns_diag_res_snapshot(di->dyn_provider, keys, max);
	if (di->dyn_domain == LKM4CTR_DYN_DOMAIN_RES)
		return lkm4ctr_diag_res_snapshot(keys, max);
	return 0;
}

static int lkm4ctr_diagfs_dyn_delete(const struct lkm4ctr_diagfs_info *fi)
{
	if (fi->dyn_domain == LKM4CTR_DYN_DOMAIN_VNS)
		return vns_diag_res_delete(fi->dyn_provider, fi->dyn_key);
	if (fi->dyn_domain == LKM4CTR_DYN_DOMAIN_RES)
		return lkm4ctr_diag_res_delete(fi->dyn_key);
	return -EINVAL;
}

/*
 * .lookup for a dynamic directory: parse the requested name back to a key,
 * and if that key is a live instance right now, materialise a read-only leaf
 * file for it (its contents render fresh on open via LKM4CTR_DIAG_DYN_INSTANCE,
 * and it can be unlinked to tear the underlying resource down). Otherwise the
 * dentry is left negative (ENOENT). Real-kernel-invoked VFS callback, so
 * __nocfi like every other inode_operations member in this file.
 */
static struct dentry *__nocfi lkm4ctr_diagfs_dyn_lookup(struct inode *dir,
							struct dentry *dentry,
							unsigned int flags)
{
	struct lkm4ctr_diagfs_info *di = dir->i_private;
	struct lkm4ctr_diagfs_info *info;
	struct inode *inode;
	u64 key;

	(void)flags;
	if (!di)
		return ERR_PTR(-ENOENT);
	if (dentry->d_name.len >= NAME_MAX)
		return ERR_PTR(-ENAMETOOLONG);
	if (!lkm4ctr_diagfs_dyn_name_to_key(di, dentry->d_name.name, &key) ||
	    !lkm4ctr_diagfs_dyn_present(di, key)) {
		d_add(dentry, NULL);
		return NULL;
	}

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info)
		return ERR_PTR(-ENOMEM);
	info->kind = LKM4CTR_DIAG_DYN_INSTANCE;
	info->dyn_domain = di->dyn_domain;
	info->dyn_provider = di->dyn_provider;
	info->dyn_key = key;

	inode = lkm4ctr_diagfs_make_inode(dir->i_sb, S_IFREG | 0444);
	if (!inode) {
		kfree(info);
		return ERR_PTR(-ENOMEM);
	}
	inode->i_private = info;
	inode->i_fop = &lkm4ctr_diagfs_ro_fops;
	d_add(dentry, inode);
	return NULL;
}

/*
 * .iterate_shared for a dynamic directory: snapshot the live registry once
 * and emit a name per instance. The snapshot is per-getdents-pass, so an
 * instance that appears/disappears mid-scan may be shown once or skipped --
 * acceptable "live view" semantics for a diagnostics fs, and never unsafe
 * (each name is re-validated by .lookup before a leaf file is created).
 */
#define LKM4CTR_DIAG_DYN_MAX	4096

static int __nocfi lkm4ctr_diagfs_dyn_iterate(struct file *file,
					      struct dir_context *ctx)
{
	struct inode *dir = file_inode(file);
	struct lkm4ctr_diagfs_info *di = dir->i_private;
	char name[40];
	u64 *keys;
	int total, i, len;

	if (!dir_emit_dots(file, ctx))
		return 0;
	if (!di)
		return 0;

	keys = kmalloc_array(LKM4CTR_DIAG_DYN_MAX, sizeof(*keys), GFP_KERNEL);
	if (!keys)
		return -ENOMEM;
	total = lkm4ctr_diagfs_dyn_snapshot(di, keys, LKM4CTR_DIAG_DYN_MAX);
	if (total > LKM4CTR_DIAG_DYN_MAX)
		total = LKM4CTR_DIAG_DYN_MAX;

	/* pos 0,1 are "."/".."; entry i is at pos 2 + i. */
	for (i = (int)ctx->pos - 2; i >= 0 && i < total; i++) {
		len = lkm4ctr_diagfs_dyn_key_to_name(di, keys[i], name, sizeof(name));
		if (len <= 0) {
			ctx->pos++;
			continue;
		}
		if (!dir_emit(ctx, name, len, (u64)(2 + i), DT_REG))
			break;
		ctx->pos++;
	}
	kfree(keys);
	return 0;
}

/*
 * .unlink for a dynamic directory: resolve the leaf's key back to its
 * provider and gracefully tear the underlying resource down (kill a thread
 * group + free its nsproxy / IPC_RMID / quiesce+drain), then drop the dentry.
 * If teardown fails, the error is propagated and the dentry is left in place.
 * Real-kernel-invoked VFS callback, so __nocfi.
 */
static int __nocfi lkm4ctr_diagfs_dyn_unlink(struct inode *dir,
					     struct dentry *dentry)
{
	struct inode *inode = d_inode(dentry);
	struct lkm4ctr_diagfs_info *fi = inode ? inode->i_private : NULL;
	int ret;

	if (!fi)
		return -EPERM;

	ret = lkm4ctr_diagfs_dyn_delete(fi);
	if (ret)
		return ret;

	if (lkm4ctr_simple_unlink_fn)
		return lkm4ctr_simple_unlink_fn(dir, dentry);
	/*
	 * Fallback if simple_unlink() could not be resolved: drop the link
	 * count so the now-freed resource's dentry becomes negative. The VFS
	 * still dput()s it after we return 0.
	 */
	drop_nlink(inode);
	return 0;
}

static const struct inode_operations lkm4ctr_diagfs_dyn_dir_inode_operations = {
	.lookup		= lkm4ctr_diagfs_dyn_lookup,
	.unlink		= lkm4ctr_diagfs_dyn_unlink,
};

/*
 * .read/.llseek are copied from simple_dir_operations at init (see
 * lkm4ctr_diagfs_init()); this file already references simple_dir_operations
 * directly for the static dirs, so borrowing its plain generic_read_dir/
 * generic_file_llseek members avoids importing those symbols by name (and the
 * attendant CONFIG_TRIM_UNUSED_KSYMS risk).
 */
static struct file_operations lkm4ctr_diagfs_dyn_dir_fops = {
	.owner		= THIS_MODULE,
	.iterate_shared	= lkm4ctr_diagfs_dyn_iterate,
};

static struct dentry *__nocfi lkm4ctr_diagfs_mkdir_dyn(struct super_block *sb,
						       struct dentry *parent,
						       const char *name,
						       int dyn_domain,
						       int dyn_provider)
{
	struct inode *inode;
	struct dentry *dentry;
	struct lkm4ctr_diagfs_info *info;

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info)
		return ERR_PTR(-ENOMEM);
	info->kind = LKM4CTR_DIAG_DYN_INSTANCE; /* unused for the dir itself */
	info->dyn_domain = dyn_domain;
	info->dyn_provider = dyn_provider;

	/*
	 * 0755 (not 0555 like the static dirs): the VFS checks unlink
	 * permission against the *parent directory*, so these dynamic dirs must
	 * be writable for `rm <leaf>` (routed to our .unlink) to be permitted
	 * by DAC without relying on CAP_DAC_OVERRIDE. There is still no
	 * .create/.mkdir op, so the write bit cannot add anything -- it only
	 * allows deleting the module-owned per-instance leaves.
	 */
	inode = lkm4ctr_diagfs_make_inode(sb, S_IFDIR | 0755);
	if (!inode) {
		kfree(info);
		return ERR_PTR(-ENOMEM);
	}
	inode->i_op = &lkm4ctr_diagfs_dyn_dir_inode_operations;
	inode->i_fop = &lkm4ctr_diagfs_dyn_dir_fops;
	inode->i_private = info;
	set_nlink(inode, 2);

	inode_lock(d_inode(parent));
	dentry = lkm4ctr_d_alloc_name_fn ? lkm4ctr_d_alloc_name_fn(parent, name) : NULL;
	if (!dentry) {
		inode_unlock(d_inode(parent));
		/*
		 * iput() runs our lkm4ctr_diagfs_evict_inode() (set as
		 * sb->s_op->evict_inode), which frees inode->i_private
		 * (== info); module-specific, not a general VFS guarantee.
		 */
		iput(inode);
		return ERR_PTR(-ENOMEM);
	}
	d_add(dentry, inode);
	inc_nlink(d_inode(parent));
	inode_unlock(d_inode(parent));

	return dentry;
}

static int lkm4ctr_diagfs_mkdir_dyn_checked(struct super_block *sb,
					    struct dentry *parent,
					    const char *name,
					    int dyn_domain, int dyn_provider)
{
	struct dentry *dentry;

	dentry = lkm4ctr_diagfs_mkdir_dyn(sb, parent, name, dyn_domain,
					  dyn_provider);
	return IS_ERR(dentry) ? PTR_ERR(dentry) : 0;
}

/* ------------------------------------------------------------------- */
/* superblock / filesystem_type registration                           */
/* ------------------------------------------------------------------- */

static void lkm4ctr_diagfs_evict_inode(struct inode *inode)
{
	truncate_inode_pages_final(&inode->i_data);
	clear_inode(inode);
	kfree(inode->i_private);
}

static int __nocfi lkm4ctr_diagfs_drop_inode(struct inode *inode)
{
	if (lkm4ctr_generic_delete_inode_fn)
		return lkm4ctr_generic_delete_inode_fn(inode);
	return 1;
}

static const struct super_operations lkm4ctr_diagfs_super_ops = {
	.statfs		= simple_statfs,
	.drop_inode	= lkm4ctr_diagfs_drop_inode,
	.evict_inode	= lkm4ctr_diagfs_evict_inode,
};

/*
 * ./mnt/v/ns/ - one dynamic directory per namespace type vendor_kernel
 * tracks. The provider (vns_diag_res_name()) supplies each directory's short
 * name ("pid", "uts", ...); each dir lists one file per live instance.
 */
static const int lkm4ctr_diagfs_ns_providers[] = {
	VNS_DIAG_NS_PID, VNS_DIAG_NS_UTS, VNS_DIAG_NS_IPC, VNS_DIAG_NS_USER,
	VNS_DIAG_NS_NET, VNS_DIAG_NS_TIME, VNS_DIAG_NS_MNT, VNS_DIAG_NS_CGROUP,
};

/* ./mnt/v/ipc/ - one dynamic directory per SysV IPC class. */
static const int lkm4ctr_diagfs_ipc_providers[] = {
	VNS_DIAG_IPC_SHM, VNS_DIAG_IPC_MSG, VNS_DIAG_IPC_SEM,
};

static int lkm4ctr_diagfs_fill_v_ns_dir(struct super_block *sb,
					struct dentry *v_dir)
{
	struct dentry *dir;
	unsigned int i;
	int ret;

	dir = lkm4ctr_diagfs_mkdir(sb, v_dir, "ns");
	if (IS_ERR(dir))
		return PTR_ERR(dir);

	for (i = 0; i < ARRAY_SIZE(lkm4ctr_diagfs_ns_providers); i++) {
		int prov = lkm4ctr_diagfs_ns_providers[i];
		const char *name = vns_diag_res_name(prov);

		if (!name)
			continue;
		ret = lkm4ctr_diagfs_mkdir_dyn_checked(sb, dir, name,
						       LKM4CTR_DYN_DOMAIN_VNS,
						       prov);
		if (ret)
			return ret;
	}
	return 0;
}

static int lkm4ctr_diagfs_fill_v_ipc_dir(struct super_block *sb,
					 struct dentry *v_dir)
{
	struct dentry *dir, *mq;
	unsigned int i;
	int ret;

	dir = lkm4ctr_diagfs_mkdir(sb, v_dir, "ipc");
	if (IS_ERR(dir))
		return PTR_ERR(dir);

	for (i = 0; i < ARRAY_SIZE(lkm4ctr_diagfs_ipc_providers); i++) {
		int prov = lkm4ctr_diagfs_ipc_providers[i];
		const char *name = vns_diag_res_name(prov);

		if (!name)
			continue;
		ret = lkm4ctr_diagfs_mkdir_dyn_checked(sb, dir, name,
						       LKM4CTR_DYN_DOMAIN_VNS,
						       prov);
		if (ret)
			return ret;
	}

	/*
	 * POSIX mqueue: per-object enumeration/delete is not feasible without
	 * a new vendored mqueuefs-walk accessor, so ./mnt/v/ipc/mqueue/ is a
	 * plain directory with a single read-only "messages" listing file
	 * rather than a dynamic per-object directory (honest limitation:
	 * individual mqueue objects cannot be deleted here today).
	 */
	mq = lkm4ctr_diagfs_mkdir(sb, dir, "mqueue");
	if (IS_ERR(mq))
		return PTR_ERR(mq);
	return lkm4ctr_diagfs_create_checked(sb, mq, "messages", 0444,
					     LKM4CTR_DIAG_MQUEUE_MSG,
					     "vendor_kernel", false, false, 0);
}

static int lkm4ctr_diagfs_fill_v_hotreload_dir(struct super_block *sb,
					       struct dentry *v_dir)
{
	struct dentry *dir;
	int ret;

	dir = lkm4ctr_diagfs_mkdir(sb, v_dir, "hotreload");
	if (IS_ERR(dir))
		return PTR_ERR(dir);

	ret = lkm4ctr_diagfs_create_checked(sb, dir, "status", 0444,
					    LKM4CTR_DIAG_HOTRELOAD_STATUS,
					    NULL, false, false, 0);
	if (ret)
		return ret;
	ret = lkm4ctr_diagfs_create_checked(sb, dir, "log", 0444,
					    LKM4CTR_DIAG_LOG,
					    "hotreload", false, false, 0);
	if (ret)
		return ret;
	return lkm4ctr_diagfs_create_checked(sb, dir, "do-hot-reload", 0200,
					    LKM4CTR_DIAG_HOTRELOAD_TRIGGER,
					    NULL, false, false, 0);
}

/*
 * ./mnt/v/ - vendor_kernel's procfs-style subtree: control/status/enabled,
 * the aggregate listing files, and the dynamic ns/ and ipc/ resource trees,
 * plus the hot-reload control directory.
 */
static int lkm4ctr_diagfs_fill_v_dir(struct super_block *sb,
				     struct dentry *root,
				     struct lkm4ctr_diagfs_module *mod)
{
	struct dentry *dir;
	int ret;

	dir = lkm4ctr_diagfs_mkdir(sb, root, mod->dirname);
	if (IS_ERR(dir))
		return PTR_ERR(dir);

	ret = lkm4ctr_diagfs_create_checked(sb, dir, "control", 0644,
					    LKM4CTR_DIAG_CONTROL,
					    mod->tag, false, false, 0);
	if (ret)
		return ret;
	ret = lkm4ctr_diagfs_create_checked(sb, dir, "status", 0444,
					    LKM4CTR_DIAG_STATUS,
					    mod->tag, false, false, 0);
	if (ret)
		return ret;
	ret = lkm4ctr_diagfs_create_checked(sb, dir, "enabled", 0644,
					    LKM4CTR_DIAG_ENABLED,
					    mod->tag, false, false, 0);
	if (ret)
		return ret;
	ret = lkm4ctr_diagfs_create_checked(sb, dir, "log", 0444,
					    LKM4CTR_DIAG_LOG,
					    mod->tag, false, false, 0);
	if (ret)
		return ret;
	ret = lkm4ctr_diagfs_create_checked(sb, dir, "references", 0444,
					    LKM4CTR_DIAG_REFERENCES,
					    mod->tag, false, false, 0);
	if (ret)
		return ret;

	if (mod->has_hooks) {
		ret = lkm4ctr_diagfs_create_checked(sb, dir, "hooks", 0444,
					    LKM4CTR_DIAG_HOOKS,
					    mod->tag, false, false, 0);
		if (ret)
			return ret;
	}

	ret = lkm4ctr_diagfs_create_checked(sb, dir, "namespaces", 0444,
					    LKM4CTR_DIAG_NAMESPACES,
					    mod->tag, false, false, 0);
	if (ret)
		return ret;
	ret = lkm4ctr_diagfs_create_checked(sb, dir, "resources", 0444,
					    LKM4CTR_DIAG_SYSVIPC_RESOURCES,
					    mod->tag, false, false, 0);
	if (ret)
		return ret;

	ret = lkm4ctr_diagfs_fill_v_ns_dir(sb, dir);
	if (ret)
		return ret;
	ret = lkm4ctr_diagfs_fill_v_ipc_dir(sb, dir);
	if (ret)
		return ret;
	return lkm4ctr_diagfs_fill_v_hotreload_dir(sb, dir);
}

static int lkm4ctr_diagfs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct inode *root_inode;
	unsigned int i;
	int ret;

	(void)data;
	(void)silent;
	atomic_inc(&lkm4ctr_diagfs_mount_count);

	sb->s_maxbytes = MAX_LFS_FILESIZE;
	sb->s_blocksize = PAGE_SIZE;
	sb->s_blocksize_bits = PAGE_SHIFT;
	sb->s_magic = LKM4CTR_DIAGFS_MAGIC;
	sb->s_op = &lkm4ctr_diagfs_super_ops;
	sb->s_time_gran = 1;

	root_inode = lkm4ctr_diagfs_make_inode(sb, S_IFDIR | 0555);
	if (!root_inode)
		return -ENOMEM;
	root_inode->i_op = &lkm4ctr_diagfs_dir_inode_operations;
	root_inode->i_fop = &simple_dir_operations;
	set_nlink(root_inode, 2);

	sb->s_root = d_make_root(root_inode);
	if (!sb->s_root)
		return -ENOMEM;

	/*
	 * ./mnt/status - the flattened, merged status+control file at the
	 * root (replacing the old split global/status + global/control):
	 * readable lifecycle state, writable on/off/forceunload/force2
	 * command surface. is_global => control_fops (see create_file).
	 */
	ret = lkm4ctr_diagfs_create_checked(sb, sb->s_root, "status", 0644,
					    LKM4CTR_DIAG_STATUS, NULL,
					    true, false, 0);
	if (ret)
		return ret;

	/*
	 * ./mnt/log - the flattened, unfiltered global log (replacing the
	 * old global/log): every log line from every subsystem, no tag
	 * filter (NULL tag => lkm4ctr_log_snprintf() returns everything).
	 * Read-only; the per-subsystem v/log and v/hotreload/log files
	 * still exist for tag-filtered views.
	 */
	ret = lkm4ctr_diagfs_create_checked(sb, sb->s_root, "log", 0444,
					    LKM4CTR_DIAG_LOG, NULL,
					    true, false, 0);
	if (ret)
		return ret;

	/*
	 * helper.sh lives at the diagfs mount root so that
	 * `. "$lkm4ctr_diagfs/helper.sh"` reads naturally. 0555 = read+execute.
	 */
	ret = lkm4ctr_diagfs_create_checked(sb, sb->s_root, "helper.sh", 0555,
					    LKM4CTR_DIAG_HELPER_SCRIPT, NULL,
					    true, false, 0);
	if (ret)
		return ret;

	/*
	 * readme.txt lives alongside helper.sh at the diagfs mount root: a
	 * full plain-text description of this whole tree. Read-only (0444).
	 */
	ret = lkm4ctr_diagfs_create_checked(sb, sb->s_root, "readme.txt", 0444,
					    LKM4CTR_DIAG_README, NULL,
					    true, false, 0);
	if (ret)
		return ret;

	/*
	 * ./mnt/resources/ - dynamic directory, one file per live reference
	 * holder currently blocking a safe rmmod (see the file header's
	 * SAFE-RMMOD RULE and lkm4ctr_diag_res_*()).
	 */
	ret = lkm4ctr_diagfs_mkdir_dyn_checked(sb, sb->s_root, "resources",
					       LKM4CTR_DYN_DOMAIN_RES, 0);
	if (ret)
		return ret;

	for (i = 0; i < ARRAY_SIZE(lkm4ctr_diagfs_modules); i++) {
		ret = lkm4ctr_diagfs_fill_v_dir(sb, sb->s_root,
						&lkm4ctr_diagfs_modules[i]);
		if (ret)
			return ret;
	}

	return 0;
}

static struct dentry *__nocfi lkm4ctr_diagfs_mount(struct file_system_type *fs_type,
					    int flags, const char *dev_name,
					    void *data)
{
	(void)dev_name;
	if (!lkm4ctr_mount_nodev_fn)
		return ERR_PTR(-ENOSYS);
	return lkm4ctr_mount_nodev_fn(fs_type, flags, data, lkm4ctr_diagfs_fill_super);
}

static void __nocfi lkm4ctr_diagfs_kill_sb(struct super_block *sb)
{
	atomic_dec(&lkm4ctr_diagfs_mount_count);
	if (lkm4ctr_kill_litter_super_fn) {
		lkm4ctr_kill_litter_super_fn(sb);
	} else if (lkm4ctr_generic_shutdown_super_fn) {
		lkm4ctr_generic_shutdown_super_fn(sb);
	} else {
		LKM4CTR_ERR(LKM4CTR_DIAGFS_TAG,
			    "kill_litter_super and generic_shutdown_super both unresolved; leaking superblock teardown");
	}
}

static struct file_system_type lkm4ctr_diagfs_type = {
	.owner		= THIS_MODULE,
	.name		= "lkm4ctr",
	.mount		= lkm4ctr_diagfs_mount,
	.kill_sb	= lkm4ctr_diagfs_kill_sb,
};

int lkm4ctr_diagfs_init(void)
{
	unsigned int i;
	int ret;

	lkm4ctr_mount_nodev_fn =
		(lkm4ctr_mount_nodev_t)lkm4ctr_diagfs_resolve("mount_nodev");
	lkm4ctr_generic_delete_inode_fn =
		(lkm4ctr_generic_delete_inode_t)lkm4ctr_diagfs_resolve("generic_delete_inode");
	lkm4ctr_kill_litter_super_fn =
		(lkm4ctr_kill_litter_super_t)lkm4ctr_diagfs_resolve("kill_litter_super");
	lkm4ctr_generic_shutdown_super_fn =
		(lkm4ctr_generic_shutdown_super_t)lkm4ctr_diagfs_resolve("generic_shutdown_super");
	lkm4ctr_d_alloc_name_fn =
		(lkm4ctr_d_alloc_name_t)lkm4ctr_diagfs_resolve("d_alloc_name");
	lkm4ctr_simple_lookup_fn =
		(lkm4ctr_simple_lookup_t)lkm4ctr_diagfs_resolve("simple_lookup");
	lkm4ctr_simple_unlink_fn =
		(lkm4ctr_simple_unlink_t)lkm4ctr_diagfs_resolve("simple_unlink");

	if (!lkm4ctr_mount_nodev_fn || !lkm4ctr_generic_delete_inode_fn ||
	    !lkm4ctr_d_alloc_name_fn || !lkm4ctr_simple_lookup_fn) {
		LKM4CTR_ERR(LKM4CTR_DIAGFS_TAG,
			    "could not resolve mount_nodev/generic_delete_inode/d_alloc_name/simple_lookup; diagfs unavailable");
		return -ENOSYS;
	}
	if (!lkm4ctr_simple_unlink_fn)
		LKM4CTR_WARN(LKM4CTR_DIAGFS_TAG,
			     "simple_unlink unresolved; dynamic resource delete falls back to drop_nlink() (resource is still torn down, only the dentry cleanup differs)");

	/*
	 * Borrow generic_read_dir()/generic_file_llseek() from the always-
	 * present simple_dir_operations for the dynamic directory fops rather
	 * than importing those symbols by name (avoids CONFIG_TRIM_UNUSED_KSYMS
	 * breakage). Only .iterate_shared is our own callback.
	 */
	lkm4ctr_diagfs_dyn_dir_fops.read = simple_dir_operations.read;
	lkm4ctr_diagfs_dyn_dir_fops.llseek = simple_dir_operations.llseek;

	if (!lkm4ctr_kill_litter_super_fn && !lkm4ctr_generic_shutdown_super_fn)
		LKM4CTR_WARN(LKM4CTR_DIAGFS_TAG,
			     "kill_litter_super and generic_shutdown_super both unresolved; diagfs unmount will leak the superblock");
	else if (!lkm4ctr_kill_litter_super_fn)
		LKM4CTR_WARN(LKM4CTR_DIAGFS_TAG,
			     "kill_litter_super unresolved; falling back to generic_shutdown_super() on unmount");

	mutex_lock(&lkm4ctr_unload_lock);
	lkm4ctr_diagfs_global_state = LKM4CTR_STATE_ACTIVE;
	lkm4ctr_unload_in_progress = false;
	lkm4ctr_unload_force = false;
	lkm4ctr_unload_force2 = false;
	for (i = 0; i < ARRAY_SIZE(lkm4ctr_diagfs_modules); i++)
		lkm4ctr_diagfs_modules[i].state =
			lkm4ctr_diagfs_module_stable_state(&lkm4ctr_diagfs_modules[i]);
	mutex_unlock(&lkm4ctr_unload_lock);

	ret = register_filesystem(&lkm4ctr_diagfs_type);
	if (ret)
		LKM4CTR_ERR(LKM4CTR_DIAGFS_TAG, "register_filesystem() failed: %d", ret);
	else
		LKM4CTR_INFO(LKM4CTR_DIAGFS_TAG,
			     "registered; mount -t lkm4ctr diag <mountpoint> for diagnostics");
	return ret;
}

void lkm4ctr_diagfs_exit(void)
{
	unregister_filesystem(&lkm4ctr_diagfs_type);
}
