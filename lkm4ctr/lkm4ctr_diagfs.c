// SPDX-License-Identifier: GPL-2.0
/*
 * lkm4ctr_diagfs - the "lkm4ctr" pseudo-filesystem: `mount -t lkm4ctr diag
 * ./mnt` exposes a read/write diagnostics tree replacing the earlier
 * /dev/lkm4ctr_safe_unload misc device entirely (see lkm4ctr_safe_unload.c's
 * removal), plus runtime control/introspection over every linked subsystem.
 *
 * Root layout
 * -----------
 *   ./mnt/global/control          - read: command help. write: "load"
 *                                    (alias "load_all") loads every
 *                                    diagfs-managed submodule; "unload"
 *                                    (aliases "1", "remove", "graceful")
 *                                    starts the graceful self-unload
 *                                    sequence; "forceunload" (aliases
 *                                    "force", "force_unload") starts the
 *                                    force self-unload sequence; "force2"
 *                                    starts (or escalates an already
 *                                    in-progress unload to) the aggressive
 *                                    force2 sequence -- see
 *                                    lkm4ctr_force2_override_refcount() and
 *                                    lkm4ctr_run_rmmod() below.
 *   ./mnt/global/status           - one of exactly: "active", "loading",
 *                                    "graceful unloading", "force unloading".
 *                                    "unloaded" is reserved for completeness
 *                                    but is not observable once this mounted
 *                                    filesystem is reachable, because the
 *                                    module itself would already be gone.
 *   ./mnt/global/log              - every log line, unfiltered.
 *   ./mnt/global/resources        - best-effort aggregate of live resources
 *                                    already tracked by the vendor_kernel and
 *                                    hook registries.
 *   ./mnt/global/references       - breaks module_refcount() down into diagfs
 *                                    mount count + in-flight shadow_hook
 *                                    calls + unaccounted "other" holders, to
 *                                    explain why rmmod is refusing to unload.
 *   ./mnt/global/hotreload/status - "first load" vs "hot-reloaded" boot kind,
 *                                    plus in-progress hot-reload state.
 *   ./mnt/global/hotreload/log    - hot-reload subsystem's own log.
 *   ./mnt/global/hotreload/do-hot-reload
 *                                  - write-only: an absolute path to a
 *                                    replacement lkm4ctr.ko triggers a hot
 *                                    reload (validate path -> quiesce hooks
 *                                    -> drain refcount -> unmount diagfs ->
 *                                    hand off to a detached
 *                                    "rmmod && insmod <path> hotreload=1"
 *                                    shell -> module_put_and_kthread_exit()).
 *                                    See lkm4ctr_hotreload.c.
 *
 *   ./mnt/helper.sh                - read-only (0555), a POSIX-sh helper
 *                                    script (contents generated into
 *                                    helper.sh.c as a plain C string) meant
 *                                    to be sourced by a caller that has
 *                                    exported $lkm4ctr_diagfs to this mount's
 *                                    path: `. "$lkm4ctr_diagfs/helper.sh"`
 *                                    then defines an `lkm4ctr` dispatcher
 *                                    function (mount/umount/status/control/
 *                                    load/unload/forceunload/force2/logcat/
 *                                    references/hot-upgrade/help).
 *
 *   ./mnt/readme.txt               - read-only (0444), a full plain-text
 *                                    description of this whole diagfs tree
 *                                    and how to use it (contents generated
 *                                    into readme.txt.c as a plain C string,
 *                                    kept in sync with this comment block by
 *                                    hand). Self-contained: readable with
 *                                    "cat" alone, no source tree needed.
 *
 *   ./mnt/hijack/{control,status,log,functions,references}
 *                                  - diagnostics for the shared hook engine.
 *                                    load/unload/forceunload behave like any
 *                                    other submodule, with one ordering rule:
 *                                    since every other submodule's hooks are
 *                                    installed through this shared engine, a
 *                                    graceful unload is refused (-EBUSY)
 *                                    while any other submodule is still
 *                                    active; forceunload instead gracefully
 *                                    unloads every other active submodule
 *                                    first, waits briefly, then
 *                                    force-unloads whatever remains active,
 *                                    before finally tearing shadow_hijack
 *                                    itself down. functions is a live
 *                                    listing of every currently registered
 *                                    hook across all submodules.
 *
 *   ./mnt/vendor_kernel/{control,status,hooks,log,namespaces,msg,resources,references}
 *                                  - vendor_kernel's lifecycle, hook list,
 *                                    namespace registry dump, POSIX mqueue
 *                                    listing and SysV IPC resource listing.
 *                                    vendor_kernel auto-loads at insmod time,
 *                                    but its control file can still unload or
 *                                    reload it later.
 *
 *   ./mnt/cgroupdevices/{control,status,hooks,log,references}
 *                                  - the other runtime-loadable subsystem.
 *                                    references breaks down that submodule's
 *                                    own contribution to module_refcount()
 *                                    the same way global/references does.
 *
 * The earlier ./mnt/modules/<subsystem>/status tree and the root-level
 * ./mnt/safe_unload / ./mnt/log files are intentionally gone. control/status
 * are split everywhere: control is the command surface, status is a strict
 * lifecycle-state readout.
 *
 * Implementation
 * --------------
 * This is a small, fully in-memory pseudo-filesystem in the same spirit as
 * ramfs/securityfs: every dentry/inode in the tree is created once, up
 * front, at mount time (mount_nodev() + fill_super()); nothing is created or
 * destroyed lazily afterwards. File contents are regenerated fresh on every
 * open() via seq_file single_open(). That static-tree design is preserved for
 * the new layout too, so categories whose underlying live-object ids are not
 * naturally knowable before mount time (e.g. currently queued mqueue messages
 * or live vendor_kernel objects that may come and go after mount) are
 * exposed as readable listing files rather than on-demand per-object dentries.
 * This keeps the filesystem simple and race-resistant while still surfacing
 * the underlying registries' current state.
 *
 * Directory traversal reuses the kernel's own
 * simple_dir_inode_operations/simple_dir_operations (fs/libfs.c) rather than
 * hand-rolling dcache walking: unlike function symbols such as
 * ftrace_set_filter_ip()/vm_mmap()/module_refcount() elsewhere in this
 * module, these two are plain data (struct) symbols, so they cannot be
 * recovered via shadow_hook_resolve()'s register_kprobe() trick if
 * CONFIG_TRIM_UNUSED_KSYMS ever stripped them -- but they are directly
 * referenced by security/inode.c (securityfs), which every Android GKI
 * kernel builds directly into vmlinux (CONFIG_SECURITYFS=y, a hard
 * requirement of the SELinux LSM every such kernel enables), giving them a
 * permanent non-modular in-tree caller that CONFIG_TRIM_UNUSED_KSYMS can
 * never trim away. This is the same "always-referenced-by-something-
 * essential" reasoning already relied on for misc_register()/
 * misc_deregister() (see the removed lkm4ctr_safe_unload.c's file header).
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
#include "vendor_kernel/vendor_kernel.h"
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
 * runtime load/unload state the shadow_hijack subsystem itself is in (see
 * lkm4ctr_diagfs_hijack_unload() below -- shadow_hijack is now an ordinary,
 * unloadable submodule like any other). A controller fs that could only
 * mount itself by calling into the hook engine's own exported resolver
 * would make diagfs *functionally* depend on shadow_hijack, defeating that
 * independence. The handful of plain kernel symbols diagfs itself needs
 * (mount_nodev/generic_delete_inode/module_refcount/call_usermodehelper)
 * are therefore resolved right here instead.
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
 * functions, not GPL-only, but unlike simple_dir_inode_operations/
 * simple_dir_operations above they are plain code, not data -- so they
 * *can* be recovered via lkm4ctr_diagfs_resolve()'s register_kprobe() trick
 * if CONFIG_TRIM_UNUSED_KSYMS strips them (confirmed: "Unknown symbol
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
 * module_refcount() and call_usermodehelper() are ordinary EXPORT_SYMBOL()
 * functions, not GPL-only, but that alone doesn't save them from
 * CONFIG_TRIM_UNUSED_KSYMS on production GKI kernels: like
 * ftrace_set_filter_ip()/vm_mmap()/anon_inode_getfd_secure() elsewhere in
 * this module, they get stripped whenever nothing built into vmlinux
 * itself calls them. Resolved lazily via lkm4ctr_diagfs_resolve() instead
 * of shadow_hijack.c's shadow_hook_resolve() -- self-unload/rmmod is core
 * diagfs functionality that must keep working independently of
 * shadow_hijack's own load/unload state, same rationale as
 * lkm4ctr_diagfs_resolve() itself above. Declared this early (rather than
 * down by lkm4ctr_safe_unload_resolve() where they used to live) so the
 * "references" renderer below can also read module_refcount() directly.
 */
typedef int (*lkm4ctr_module_refcount_t)(struct module *mod);
typedef int (*lkm4ctr_call_usermodehelper_t)(const char *path, char **argv,
					      char **envp, int wait);

static lkm4ctr_module_refcount_t lkm4ctr_module_refcount_fn;
static lkm4ctr_call_usermodehelper_t lkm4ctr_call_usermodehelper_fn;

extern int vendor_kernel_init(void);
extern void vendor_kernel_exit(void);
extern size_t vendor_kernel_diag_snprintf(char *buf, size_t buflen);
extern int shadow_cgdevices_init(void);
extern void shadow_cgdevices_exit(void);
extern int shadow_hijack_init(void);
extern void shadow_hijack_exit(void);

/* lkm4ctr_hotreload.c - see lkm4ctr/lkm4ctr_hotreload.h for full contracts. */
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
};

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
	{ "hijack", 		"shadow_hijack", 	false, false, shadow_hijack_init,	shadow_hijack_exit,	LKM4CTR_STATE_ACTIVE },
	{ "vendor_kernel", 	"vendor_kernel", 	true,  true,  vendor_kernel_init,	vendor_kernel_exit,	LKM4CTR_STATE_ACTIVE },
	{ "cgroupdevices", 	"shadow_cgdevices", 	true,  false, shadow_cgdevices_init,	shadow_cgdevices_exit,	LKM4CTR_STATE_UNLOADED },
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

/*
 * shadow_hijack is now an ordinary submodule like the others (see the
 * lkm4ctr_diagfs_modules[] entry above), but it has no shadow_hook_install()
 * hooks of its own to query via shadow_hook_registry_tag_active() -- it
 * *is* the shared hook engine every other submodule's hooks are tracked
 * through. Its own active/unloaded state is therefore tracked directly
 * here instead, updated by lkm4ctr_diagfs_hijack_unload() below and read
 * back by lkm4ctr_diagfs_module_stable_state(). Protected by
 * lkm4ctr_unload_lock, same as every other piece of lifecycle state above.
 */
static bool lkm4ctr_hijack_active = true;

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
	if (!strcmp(mod->tag, "shadow_hijack"))
		return lkm4ctr_hijack_active ? LKM4CTR_STATE_ACTIVE : LKM4CTR_STATE_UNLOADED;
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
static const struct file_operations lkm4ctr_diagfs_hotreload_trigger_fops;

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

static struct dentry *lkm4ctr_diagfs_mkdir(struct super_block *sb,
					    struct dentry *parent,
					    const char *name)
{
	struct inode *inode;
	struct dentry *dentry;

	inode = lkm4ctr_diagfs_make_inode(sb, S_IFDIR | 0555);
	if (!inode)
		return ERR_PTR(-ENOMEM);
	inode->i_op = &simple_dir_inode_operations;
	inode->i_fop = &simple_dir_operations;
	set_nlink(inode, 2);

	inode_lock(d_inode(parent));
	dentry = d_alloc_name(parent, name);
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

static struct dentry *lkm4ctr_diagfs_create_file(struct super_block *sb,
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
	case LKM4CTR_DIAG_HOTRELOAD_TRIGGER:
		inode->i_fop = &lkm4ctr_diagfs_hotreload_trigger_fops;
		break;
	default:
		inode->i_fop = &lkm4ctr_diagfs_ro_fops;
		break;
	}

	inode_lock(d_inode(parent));
	dentry = d_alloc_name(parent, name);
	if (!dentry) {
		inode_unlock(d_inode(parent));
		iput(inode);
		kfree(info);
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
	struct lkm4ctr_diagfs_module *mod = NULL;

	if (!info->is_global)
		mod = lkm4ctr_diagfs_find_module(info->tag);

	if (info->is_global) {
		pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
				 "commands:\n"
				 "  load         - load every submodule (alias: load_all)\n"
				 "  unload       - graceful self-unload (aliases: 1, remove, graceful)\n"
				 "  forceunload  - force self-unload (aliases: force, force_unload)\n");
		return pos;
	}

	if (mod && !strcmp(mod->tag, "shadow_hijack")) {
		pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
				 "commands:\n"
				 "  load         - load this subsystem\n"
				 "  unload       - graceful unload (aliases: remove, graceful)\n"
				 "  forceunload  - force unload (aliases: force, force_unload)\n"
				 "shadow_hijack is the shared hook engine backing every other subsystem:\n"
				 "graceful unload is refused (-EBUSY) while any other submodule is still\n"
				 "active; forceunload instead gracefully unloads every other active\n"
				 "submodule first, waits briefly, then force-unloads whatever remains\n"
				 "active, before finally tearing down shadow_hijack itself.\n");
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
		mutex_lock(&lkm4ctr_unload_lock);
		state = lkm4ctr_diagfs_global_state;
		mutex_unlock(&lkm4ctr_unload_lock);
		return scnprintf(buf, buflen, "%s\n",
				 lkm4ctr_diagfs_state_name(state));
	}

	mod = lkm4ctr_diagfs_find_module(info->tag);
	if (!mod)
		return scnprintf(buf, buflen, "unloaded\n");

	mutex_lock(&lkm4ctr_unload_lock);
	state = mod->state;
	mutex_unlock(&lkm4ctr_unload_lock);

	return scnprintf(buf, buflen, "%s\n", lkm4ctr_diagfs_state_name(state));
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
 *     see common/shadow_hook.h and shadow_hijack.c).
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
				 "module_refcount() unavailable (module_refcount symbol not yet resolved; write to global/control at least once to trigger resolution)\n");
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
	return is_global && !strcmp(cmd, "1");
}

static int lkm4ctr_diagfs_parse_control_cmd(const char *cmd, bool is_global,
					    enum lkm4ctr_diagfs_control_cmd *out)
{
	if (!strcmp(cmd, "load") || (is_global && !strcmp(cmd, "load_all"))) {
		*out = LKM4CTR_CONTROL_LOAD;
	} else if (lkm4ctr_diagfs_is_unload_cmd(cmd, is_global)) {
		*out = LKM4CTR_CONTROL_UNLOAD;
	} else if (!strcmp(cmd, "force") || !strcmp(cmd, "force_unload") ||
		   !strcmp(cmd, "forceunload")) {
		*out = LKM4CTR_CONTROL_FORCE_UNLOAD;
	} else if (is_global && !strcmp(cmd, "force2")) {
		/*
		 * "force2" is a global-only escalation, deliberately not
		 * accepted as a first command on a fresh (not-yet-unloading)
		 * global/control -- see the file header note above and
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
	if (!ret && !strcmp(mod->tag, "shadow_hijack"))
		lkm4ctr_hijack_active = true;
	mod->state = lkm4ctr_diagfs_module_stable_state(mod);
	mutex_unlock(&lkm4ctr_unload_lock);

	return ret;
}

static int lkm4ctr_diagfs_module_unload(struct lkm4ctr_diagfs_module *mod,
					bool force, bool from_global);

#define LKM4CTR_HIJACK_UNLOAD_WAIT_MS	3000
#define LKM4CTR_HIJACK_UNLOAD_POLL_MS	50

static bool lkm4ctr_diagfs_other_modules_active(void)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(lkm4ctr_diagfs_modules); i++) {
		struct lkm4ctr_diagfs_module *other = &lkm4ctr_diagfs_modules[i];

		if (!strcmp(other->tag, "shadow_hijack") || !other->mod_exit)
			continue;
		if (lkm4ctr_diagfs_module_stable_state(other) != LKM4CTR_STATE_UNLOADED)
			return true;
	}
	return false;
}

/*
 * lkm4ctr_diagfs_hijack_unload() - shadow_hijack is the shared hook engine
 * every other submodule's hooks are installed through, so it may only
 * become "unloaded" once every other submodule already is: unlike them,
 * unloading it while e.g. vendor_kernel still has live hooks installed would
 * pull the rug out from under code that is still redirecting real syscalls
 * into this module.
 *
 * A plain graceful request (!force) is therefore simply refused with
 * -EBUSY while any other submodule remains active. A force request instead
 * cascades: gracefully unload every other active submodule first, wait
 * briefly for that to settle, then force-unload whatever is still active
 * after that wait -- mirroring exactly the same graceful-then-wait-then-
 * force pattern the whole-module self-unload sequence
 * (lkm4ctr_safe_unload_fn()) already uses -- before finally tearing down
 * shadow_hijack itself.
 */
static int lkm4ctr_diagfs_hijack_unload(bool force, bool from_global)
{
	unsigned int i;
	unsigned long waited_ms = 0;

	if (lkm4ctr_diagfs_other_modules_active()) {
		if (!force) {
			LKM4CTR_WARN("shadow_hijack",
				     "cannot unload: at least one other submodule is still active; unload it (or use forceunload) first");
			return -EBUSY;
		}

		LKM4CTR_INFO("shadow_hijack",
			     "force unload requested with other submodule(s) still active: gracefully unloading every one of them first");
		if (!from_global)
			shadow_hook_quiesce(true);

		for (i = 0; i < ARRAY_SIZE(lkm4ctr_diagfs_modules); i++) {
			struct lkm4ctr_diagfs_module *other = &lkm4ctr_diagfs_modules[i];

			if (!strcmp(other->tag, "shadow_hijack") || !other->mod_exit)
				continue;
			if (lkm4ctr_diagfs_module_stable_state(other) != LKM4CTR_STATE_UNLOADED)
				lkm4ctr_diagfs_module_unload(other, false, true);
		}

		while (lkm4ctr_diagfs_other_modules_active() &&
		       waited_ms < LKM4CTR_HIJACK_UNLOAD_WAIT_MS) {
			msleep(LKM4CTR_HIJACK_UNLOAD_POLL_MS);
			waited_ms += LKM4CTR_HIJACK_UNLOAD_POLL_MS;
		}

		if (lkm4ctr_diagfs_other_modules_active()) {
			LKM4CTR_WARN("shadow_hijack",
				     "force-unloading any submodule(s) still active after %lums of graceful waiting",
				     waited_ms);
			for (i = 0; i < ARRAY_SIZE(lkm4ctr_diagfs_modules); i++) {
				struct lkm4ctr_diagfs_module *other = &lkm4ctr_diagfs_modules[i];

				if (!strcmp(other->tag, "shadow_hijack") || !other->mod_exit)
					continue;
				if (lkm4ctr_diagfs_module_stable_state(other) != LKM4CTR_STATE_UNLOADED)
					lkm4ctr_diagfs_module_unload(other, true, true);
			}
		}

		if (!from_global)
			shadow_hook_quiesce(false);
	}

	shadow_hijack_exit();

	mutex_lock(&lkm4ctr_unload_lock);
	lkm4ctr_hijack_active = false;
	mutex_unlock(&lkm4ctr_unload_lock);

	return 0;
}

static int lkm4ctr_diagfs_module_unload(struct lkm4ctr_diagfs_module *mod,
					bool force, bool from_global)
{
	bool is_hijack = !strcmp(mod->tag, "shadow_hijack");

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

	if (is_hijack) {
		int ret = lkm4ctr_diagfs_hijack_unload(force, from_global);

		if (ret) {
			mutex_lock(&lkm4ctr_unload_lock);
			mod->state = lkm4ctr_diagfs_module_stable_state(mod);
			mutex_unlock(&lkm4ctr_unload_lock);
			return ret;
		}
	} else {
		if (force && !from_global)
			shadow_hook_quiesce(true);
		mod->mod_exit();
		if (force && !from_global)
			shadow_hook_quiesce(false);
	}

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
	struct lkm4ctr_diagfs_module *hijack = NULL;

	LKM4CTR_INFO(LKM4CTR_DIAGFS_TAG, "%s-unloading every submodule", label);
	for (i = 0; i < ARRAY_SIZE(lkm4ctr_diagfs_modules); i++) {
		struct lkm4ctr_diagfs_module *mod = &lkm4ctr_diagfs_modules[i];

		if (!mod->mod_exit)
			continue;
		if (!strcmp(mod->tag, "shadow_hijack")) {
			/* Unload last: it may only settle to "unloaded"
			 * once every other submodule already has (see
			 * lkm4ctr_diagfs_hijack_unload()).
			 */
			hijack = mod;
			continue;
		}
		lkm4ctr_diagfs_module_unload(mod, force, true);
	}
	if (hijack)
		lkm4ctr_diagfs_module_unload(hijack, force, true);
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
					    "resolution: `umount` every mountpoint of type \"lkm4ctr\" (check with `grep lkm4ctr /proc/mounts`) -- including the one you may be reading/writing global/control through right now -- then write to global/control again, or escalate to `echo force2 > global/control` to override it forcibly");
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
					    "resolution: this is a real in-flight kernel call still executing somewhere -- it cannot be forced to finish sooner without risking a crash, so simply wait and retry; if the count never drops on retry this may be a reference leak worth reporting, or escalate to `echo force2 > global/control` to override it forcibly (unsafe, last resort)");
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
	module_put_and_exit(0);
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
/* superblock / filesystem_type registration                           */
/* ------------------------------------------------------------------- */

static void lkm4ctr_diagfs_evict_inode(struct inode *inode)
{
	truncate_inode_pages_final(&inode->i_data);
	clear_inode(inode);
	kfree(inode->i_private);
}

static int lkm4ctr_diagfs_drop_inode(struct inode *inode)
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

static int lkm4ctr_diagfs_fill_module_dir(struct super_block *sb,
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

	if (!strcmp(mod->tag, "shadow_hijack")) {
		return lkm4ctr_diagfs_create_checked(sb, dir, "functions", 0444,
					    LKM4CTR_DIAG_HOOKS,
					    NULL, false, false, 0);
	}

	if (mod->has_hooks) {
		ret = lkm4ctr_diagfs_create_checked(sb, dir, "hooks", 0444,
					    LKM4CTR_DIAG_HOOKS,
					    mod->tag, false, false, 0);
		if (ret)
			return ret;
	}

	if (!strcmp(mod->tag, "vendor_kernel")) {
		ret = lkm4ctr_diagfs_create_checked(sb, dir, "namespaces", 0444,
					    LKM4CTR_DIAG_NAMESPACES,
					    mod->tag, false, false, 0);
		if (ret)
			return ret;
		ret = lkm4ctr_diagfs_create_checked(sb, dir, "msg", 0444,
					    LKM4CTR_DIAG_MQUEUE_MSG,
					    mod->tag, false, false, 0);
		if (ret)
			return ret;
		return lkm4ctr_diagfs_create_checked(sb, dir, "resources", 0444,
					    LKM4CTR_DIAG_SYSVIPC_RESOURCES,
					    mod->tag, false, false, 0);
	}

	return 0;
}

static int lkm4ctr_diagfs_fill_hotreload_dir(struct super_block *sb,
					     struct dentry *global_dir)
{
	struct dentry *dir;
	int ret;

	dir = lkm4ctr_diagfs_mkdir(sb, global_dir, "hotreload");
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

static int lkm4ctr_diagfs_fill_global_dir(struct super_block *sb,
					  struct dentry *root)
{
	struct dentry *dir;
	int ret;

	dir = lkm4ctr_diagfs_mkdir(sb, root, "global");
	if (IS_ERR(dir))
		return PTR_ERR(dir);

	ret = lkm4ctr_diagfs_create_checked(sb, dir, "control", 0644,
					    LKM4CTR_DIAG_CONTROL,
					    NULL, true, false, 0);
	if (ret)
		return ret;
	ret = lkm4ctr_diagfs_create_checked(sb, dir, "status", 0444,
					    LKM4CTR_DIAG_STATUS,
					    NULL, true, false, 0);
	if (ret)
		return ret;
	ret = lkm4ctr_diagfs_create_checked(sb, dir, "log", 0444,
					    LKM4CTR_DIAG_LOG,
					    NULL, false, false, 0);
	if (ret)
		return ret;
	ret = lkm4ctr_diagfs_create_checked(sb, dir, "resources", 0444,
					    LKM4CTR_DIAG_GLOBAL_RESOURCES,
					    NULL, true, false, 0);
	if (ret)
		return ret;
	ret = lkm4ctr_diagfs_create_checked(sb, dir, "references", 0444,
					    LKM4CTR_DIAG_REFERENCES,
					    NULL, true, false, 0);
	if (ret)
		return ret;
	return lkm4ctr_diagfs_fill_hotreload_dir(sb, dir);
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
	root_inode->i_op = &simple_dir_inode_operations;
	root_inode->i_fop = &simple_dir_operations;
	set_nlink(root_inode, 2);

	sb->s_root = d_make_root(root_inode);
	if (!sb->s_root)
		return -ENOMEM;

	ret = lkm4ctr_diagfs_fill_global_dir(sb, sb->s_root);
	if (ret)
		return ret;

	/*
	 * helper.sh lives at the diagfs mount root (a sibling of global/ and
	 * every submodule directory), not under global/, so that
	 * `. "$lkm4ctr_diagfs/helper.sh"` reads naturally regardless of
	 * which submodules happen to be present. 0555 matches the
	 * user-requested "chmod 555 r-x" read+execute-only permission.
	 */
	ret = lkm4ctr_diagfs_create_checked(sb, sb->s_root, "helper.sh", 0555,
					    LKM4CTR_DIAG_HELPER_SCRIPT, NULL,
					    true, false, 0);
	if (ret)
		return ret;

	/*
	 * readme.txt lives alongside helper.sh at the diagfs mount root: a
	 * full plain-text description of this whole tree and how to use it,
	 * self-contained so it is useful even without this repository's
	 * source tree. Read-only, not executable (0444), unlike helper.sh's
	 * 0555.
	 */
	ret = lkm4ctr_diagfs_create_checked(sb, sb->s_root, "readme.txt", 0444,
					    LKM4CTR_DIAG_README, NULL,
					    true, false, 0);
	if (ret)
		return ret;

	for (i = 0; i < ARRAY_SIZE(lkm4ctr_diagfs_modules); i++) {
		ret = lkm4ctr_diagfs_fill_module_dir(sb, sb->s_root,
						   &lkm4ctr_diagfs_modules[i]);
		if (ret)
			return ret;
	}

	return 0;
}

static struct dentry *lkm4ctr_diagfs_mount(struct file_system_type *fs_type,
					    int flags, const char *dev_name,
					    void *data)
{
	(void)dev_name;
	if (!lkm4ctr_mount_nodev_fn)
		return ERR_PTR(-ENOSYS);
	return lkm4ctr_mount_nodev_fn(fs_type, flags, data, lkm4ctr_diagfs_fill_super);
}

static void lkm4ctr_diagfs_kill_sb(struct super_block *sb)
{
	atomic_dec(&lkm4ctr_diagfs_mount_count);
	kill_litter_super(sb);
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

	if (!lkm4ctr_mount_nodev_fn || !lkm4ctr_generic_delete_inode_fn) {
		LKM4CTR_ERR(LKM4CTR_DIAGFS_TAG,
			    "could not resolve mount_nodev/generic_delete_inode; diagfs unavailable");
		return -ENOSYS;
	}

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
