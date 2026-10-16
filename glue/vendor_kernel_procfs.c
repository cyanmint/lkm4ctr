// SPDX-License-Identifier: GPL-2.0
/*
 * vendor_kernel_procfs.c - expose real vendored IPC/PID namespace symlinks
 * through procfs when the corresponding native CONFIG_*_NS entry is absent.
 *
 * procfs compiles its namespace directory entries into ns_entries[];
 * CONFIG_IPC_NS=n or CONFIG_PID_NS=n therefore makes the component lookup
 * return -ENOENT even though vendor_kernel has real namespace objects. The
 * proc_ns_dir_lookup hook retries only that missing component for the
 * current task's /proc/self/ns or /proc/thread-self/ns directory and calls
 * procfs's own proc_ns_instantiate(). This creates a normal procfs symlink;
 * subsequent VFS, LSM, ptrace, and nsfs handling is unchanged.
 * Since procfs inodes do not pin module-owned proc_ns_operations, each
 * fallback inode holds a module reference until proc_evict_inode() completes.
 * The created dentry is unhashed immediately so ordinary lookups do not
 * leave a cached inode pinning the module until unrelated dcache reclaim. If
 * either lifetime helper is unavailable, lookup remains fail-closed.
 *
 * No readlink/stat/open syscall result is fabricated for IPC/PID paths.
 * Those syscall fallbacks cannot distinguish a missing procfs entry from
 * permission or LSM denial. If a required procfs/VFS helper is unavailable,
 * the missing component remains absent (fail closed). Directory enumeration
 * remains controlled by the kernel's native ns_entries[] table.
 *
 * The open syscall hooks below are retained only for the separate vendored
 * user-namespace id-map procfs compatibility path.
 */
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/hashtable.h>
#include <linux/uaccess.h>
#include <linux/ctype.h>
#include <linux/fcntl.h>
#include <linux/magic.h>
#include <linux/module.h>
#include <linux/namei.h>
#include <linux/pid.h>
#include <linux/pid_namespace.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <asm/ptrace.h>

#include "../vendor/vendor_kernel.h"
#include "shadow_hook.h"
#include "lkm4ctr_compat.h"
#include "lkm4ctr_log.h"

#define VNS_PROC_NS_IPC_NAME	"ipc"
#define VNS_PROC_NS_PID_NAME	"pid"

typedef struct dentry *(*vns_proc_ns_instantiate_fn_t)(struct dentry *,
						       struct task_struct *,
						       const void *);
typedef int (*vns_proc_kern_path_fn_t)(const char *, unsigned int,
				       struct path *);
typedef void (*vns_proc_path_put_fn_t)(const struct path *);
typedef void (*vns_proc_evict_inode_fn_t)(struct inode *);
typedef void (*vns_proc_d_drop_fn_t)(struct dentry *);

#define VNS_PROCFS_INODE_PIN_HASH_BITS 6

struct vns_procfs_inode_pin {
	struct hlist_node node;
	struct inode *inode;
};

static vns_proc_ns_instantiate_fn_t vns_proc_ns_instantiate_real;
static vns_proc_kern_path_fn_t vns_proc_kern_path_real;
static vns_proc_path_put_fn_t vns_proc_path_put_real;
static vns_proc_evict_inode_fn_t vns_proc_evict_inode_real;
static vns_proc_d_drop_fn_t vns_proc_d_drop_real;
static bool vns_proc_native_ipc_ns;
static bool vns_proc_ns_entry_fallback_ready;
static DEFINE_HASHTABLE(vns_procfs_inode_pins,
		VNS_PROCFS_INODE_PIN_HASH_BITS);
static DEFINE_SPINLOCK(vns_procfs_inode_pins_lock);
static atomic_t vns_procfs_inode_pin_count = ATOMIC_INIT(0);
static DEFINE_SPINLOCK(vns_proc_init_pid_ns_ops_lock);
static struct pid_namespace *vns_proc_init_pid_ns_ops_target;

static struct dentry *(*vns_real_proc_ns_dir_lookup)(struct inode *dir,
						     struct dentry *dentry,
						     unsigned int flags);

static bool vns_proc_same_ns_dir(struct dentry *parent,
				struct path *candidate)
{
	struct inode *parent_inode;

	if (!parent || !candidate->dentry ||
	    parent->d_sb->s_magic != PROC_SUPER_MAGIC ||
	    candidate->dentry->d_sb->s_magic != PROC_SUPER_MAGIC ||
	    parent->d_sb != candidate->dentry->d_sb)
		return false;
	parent_inode = d_inode(parent);
	return parent_inode && parent_inode == d_inode(candidate->dentry);
}

static void vns_procfs_pin_inode(struct inode *inode,
				 struct vns_procfs_inode_pin *candidate)
{
	struct vns_procfs_inode_pin *entry;
	unsigned long flags;

	if (!inode) {
		/* The created proc inode may retain our ops; never drop this pin. */
		atomic_inc(&vns_procfs_inode_pin_count);
		kfree(candidate);
		LKM4CTR_ERR("vendor_kernel",
			    "proc namespace instantiate returned success without an inode; retaining a fail-safe module pin");
		return;
	}

	candidate->inode = inode;
	spin_lock_irqsave(&vns_procfs_inode_pins_lock, flags);
	hash_for_each_possible(vns_procfs_inode_pins, entry, node,
			       (unsigned long)inode) {
		if (entry->inode == inode) {
			spin_unlock_irqrestore(&vns_procfs_inode_pins_lock, flags);
			kfree(candidate);
			module_put(THIS_MODULE);
			return;
		}
	}
	hash_add(vns_procfs_inode_pins, &candidate->node,
		 (unsigned long)inode);
	atomic_inc(&vns_procfs_inode_pin_count);
	spin_unlock_irqrestore(&vns_procfs_inode_pins_lock, flags);
}

static void vns_procfs_unpin_inode(struct inode *inode)
{
	struct vns_procfs_inode_pin *entry;
	struct vns_procfs_inode_pin *found = NULL;
	unsigned long flags;

	spin_lock_irqsave(&vns_procfs_inode_pins_lock, flags);
	hash_for_each_possible(vns_procfs_inode_pins, entry, node,
			       (unsigned long)inode) {
		if (entry->inode == inode) {
			hash_del(&entry->node);
			found = entry;
			break;
		}
	}
	spin_unlock_irqrestore(&vns_procfs_inode_pins_lock, flags);

	if (!found)
		return;
	atomic_dec(&vns_procfs_inode_pin_count);
	kfree(found);
	module_put(THIS_MODULE);
}

static void vendor_kernel_hook_proc_evict_inode(struct inode *inode)
{
	/* proc_evict_inode() clears proc-specific state before releasing our pin. */
	vns_proc_evict_inode_real(inode);
	vns_procfs_unpin_inode(inode);
}

static const char * const vendor_kernel_proc_evict_inode_names[] = {
	"proc_evict_inode",
	NULL,
};

static struct shadow_hook vendor_kernel_proc_evict_inode_hook =
	SHADOW_HOOK(vendor_kernel_proc_evict_inode_names,
		    vendor_kernel_hook_proc_evict_inode,
		    &vns_proc_evict_inode_real);

static struct task_struct *vns_proc_ns_parent_task(struct dentry *parent)
{
	static const struct {
		const char *path;
		bool thread_self;
	} aliases[] = {
		{ "/proc/thread-self/ns", true },
		{ "/proc/self/ns", false },
	};
	struct task_struct *task;
	struct path path;
	unsigned int i;

	if (!vns_proc_kern_path_real || !vns_proc_path_put_real)
		return NULL;

	for (i = 0; i < ARRAY_SIZE(aliases); i++) {
		if (vns_proc_kern_path_real(aliases[i].path,
					    LOOKUP_FOLLOW | LOOKUP_DIRECTORY,
					    &path))
			continue;

		if (vns_proc_same_ns_dir(parent, &path)) {
			task = aliases[i].thread_self ? current : current->group_leader;
			get_task_struct(task);
			vns_proc_path_put_real(&path);
			if (!pid_alive(task)) {
				put_task_struct(task);
				return NULL;
			}
			return task;
		}
		vns_proc_path_put_real(&path);
	}

	return NULL;
}

/*
 * CONFIG_PID_NS=n kernels still expose task_active_pid_ns(), but their
 * initial pid_namespace may have no ns_common ops because native procfs does
 * not publish the pid namespace link. nsfs unconditionally dereferences
 * ns->ops when it tears down a namespace inode, so make the immortal level-0
 * object usable by our normal proc_ns_operations path before publishing the
 * fallback symlink. The fallback procfs inode and any resulting NSFS inode
 * hold the module references while callbacks can run; controlled stop clears
 * this pointer after both inode-pin sets have drained.
 */
static bool vns_procfs_compat_enable_init_pidns_ops(struct task_struct *task)
{
	struct pid_namespace *active;
	const struct proc_ns_operations *ops;
	unsigned long flags;
	bool enabled = false;

	rcu_read_lock();
	active = task_active_pid_ns(task);
	if (!active) {
		rcu_read_unlock();
		return false;
	}
	ops = READ_ONCE(active->ns.ops);
	if (ops == &vns_pidns_operations) {
		rcu_read_unlock();
		return true;
	}
	if (ops || READ_ONCE(active->level) != 0 ||
	    READ_ONCE(active->parent) || !active->ns.inum ||
	    !active->user_ns) {
		rcu_read_unlock();
		return false;
	}
	rcu_read_unlock();

	spin_lock_irqsave(&vns_proc_init_pid_ns_ops_lock, flags);
	ops = READ_ONCE(active->ns.ops);
	if (!ops && !vns_proc_init_pid_ns_ops_target) {
		WRITE_ONCE(active->ns.ops, &vns_pidns_operations);
		vns_proc_init_pid_ns_ops_target = active;
		enabled = true;
	} else if (ops == &vns_pidns_operations &&
		   vns_proc_init_pid_ns_ops_target == active) {
		enabled = true;
	}
	spin_unlock_irqrestore(&vns_proc_init_pid_ns_ops_lock, flags);
	return enabled;
}

void vns_procfs_compat_resolve(void)
{
	vendor_kernel_proc_evict_inode_hook.quiesce_exempt = true;
	vns_proc_native_ipc_ns = !!shadow_hook_resolve("copy_ipcs");
	vns_proc_ns_instantiate_real = (vns_proc_ns_instantiate_fn_t)
		shadow_hook_resolve("proc_ns_instantiate");
	vns_proc_kern_path_real = (vns_proc_kern_path_fn_t)
		shadow_hook_resolve("kern_path");
	vns_proc_path_put_real = (vns_proc_path_put_fn_t)
		shadow_hook_resolve("path_put");
	vns_proc_evict_inode_real = (vns_proc_evict_inode_fn_t)
		shadow_hook_resolve("proc_evict_inode");
	vns_proc_d_drop_real = (vns_proc_d_drop_fn_t)
		shadow_hook_resolve("d_drop");
	vns_proc_ns_entry_fallback_ready = vns_proc_ns_instantiate_real &&
		vns_proc_kern_path_real && vns_proc_path_put_real &&
		vns_proc_evict_inode_real && vns_proc_d_drop_real;
}

static struct dentry *vendor_kernel_hook_proc_ns_dir_lookup(
	struct inode *dir, struct dentry *dentry, unsigned int flags)
{
	const struct proc_ns_operations *ns_ops;
	struct task_struct *task;
	struct vns_procfs_inode_pin *candidate;
	struct inode *inode;
	struct dentry *res;
	bool native;

	if (!vns_real_proc_ns_dir_lookup)
		return ERR_PTR(-ENOENT);
	res = vns_real_proc_ns_dir_lookup(dir, dentry, flags);
	if (!IS_ERR(res) || PTR_ERR(res) != -ENOENT ||
	    !vns_proc_ns_entry_fallback_ready ||
	    !vendor_kernel_proc_evict_inode_hook.installed)
		return res;

	if (dentry->d_name.len == strlen(VNS_PROC_NS_IPC_NAME) &&
	    !memcmp(dentry->d_name.name, VNS_PROC_NS_IPC_NAME,
		    dentry->d_name.len)) {
		native = vns_proc_native_ipc_ns;
		ns_ops = &vns_ipcns_operations;
	} else if (dentry->d_name.len == strlen(VNS_PROC_NS_PID_NAME) &&
		   !memcmp(dentry->d_name.name, VNS_PROC_NS_PID_NAME,
			   dentry->d_name.len)) {
		native = READ_ONCE(vns_pidns_runtime_supported);
		ns_ops = &vns_pidns_operations;
	} else {
		return res;
	}
	if (native)
		return res;

	/* Match the actual parent dentry to the current task's procfs alias. */
	if (!dentry->d_parent || d_inode(dentry->d_parent) != dir)
		return res;
	task = vns_proc_ns_parent_task(dentry->d_parent);
	if (!task)
		return res;
	candidate = kmalloc(sizeof(*candidate), GFP_KERNEL);
	if (!candidate || !try_module_get(THIS_MODULE)) {
		kfree(candidate);
		put_task_struct(task);
		return res;
	}
	if (ns_ops == &vns_pidns_operations &&
	    !vns_procfs_compat_enable_init_pidns_ops(task)) {
		kfree(candidate);
		module_put(THIS_MODULE);
		put_task_struct(task);
		return res;
	}
	res = vns_proc_ns_instantiate_real(dentry, task, ns_ops);
	put_task_struct(task);
	if (IS_ERR(res)) {
		kfree(candidate);
		module_put(THIS_MODULE);
		return res;
	}
	inode = d_inode(res ? res : dentry);
	vns_procfs_pin_inode(inode, candidate);
	/* Avoid leaving a cached dentry whose operations point into this module. */
	vns_proc_d_drop_real(res ? res : dentry);
	return res;
}

static const char * const vendor_kernel_proc_ns_dir_lookup_names[] = {
	"proc_ns_dir_lookup",
	NULL,
};

static struct shadow_hook vendor_kernel_proc_ns_dir_lookup_hook =
	SHADOW_HOOK(vendor_kernel_proc_ns_dir_lookup_names,
		    vendor_kernel_hook_proc_ns_dir_lookup,
		    &vns_real_proc_ns_dir_lookup);

bool vns_procfs_compat_active(void)
{
	bool needs_fallback = !vns_proc_native_ipc_ns ||
		!READ_ONCE(vns_pidns_runtime_supported);

	return !needs_fallback ||
		(vns_proc_ns_entry_fallback_ready &&
		 vendor_kernel_proc_ns_dir_lookup_hook.installed &&
		 vendor_kernel_proc_evict_inode_hook.installed);
}

int vns_procfs_compat_prepare_exit(void)
{
	int pins = atomic_read(&vns_procfs_inode_pin_count);

	if (!pins)
		return 0;
	LKM4CTR_WARN("vendor_kernel",
		     "refusing stop while %d procfs namespace inode(s) retain module-owned namespace operations",
		     pins);
	return -EBUSY;
}

int vns_procfs_compat_detach_init_pidns_ops(void)
{
	struct pid_namespace *pid_ns;
	unsigned long flags;

	if (!shadow_hook_is_quiescing() || shadow_hook_inflight_count() ||
	    atomic_read(&vns_procfs_inode_pin_count) ||
	    vns_nsfs_lifecycle_pin_count())
		return -EBUSY;

	spin_lock_irqsave(&vns_proc_init_pid_ns_ops_lock, flags);
	pid_ns = vns_proc_init_pid_ns_ops_target;
	if (pid_ns) {
		if (READ_ONCE(pid_ns->ns.ops) != &vns_pidns_operations) {
			spin_unlock_irqrestore(&vns_proc_init_pid_ns_ops_lock,
					       flags);
			return -EBUSY;
		}
		WRITE_ONCE(pid_ns->ns.ops, NULL);
		vns_proc_init_pid_ns_ops_target = NULL;
	}
	spin_unlock_irqrestore(&vns_proc_init_pid_ns_ops_lock, flags);
	return 0;
}

#if defined(CONFIG_ARM64)
static unsigned long vns_procfs_arg0(const struct pt_regs *regs) { return regs->regs[0]; }
static unsigned long vns_procfs_arg1(const struct pt_regs *regs) { return regs->regs[1]; }
#elif defined(CONFIG_X86_64)
static unsigned long vns_procfs_arg0(const struct pt_regs *regs) { return regs->di; }
static unsigned long vns_procfs_arg1(const struct pt_regs *regs) { return regs->si; }
#else
#error "vendor_kernel: unsupported architecture"
#endif

/* Bare "self"/"thread-self"/numeric-pid component used by the id-map
 * compatibility path below. */
static bool vns_component_is_pid_dir(const char *s)
{
	if (!*s)
		return false;
	if (!strcmp(s, "self") || !strcmp(s, "thread-self"))
		return true;
	for (; *s; s++) {
		if (!isdigit((unsigned char)*s))
			return false;
	}
	return true;
}


/*
 * vns_resolve_ns_pid() - resolve the "self"/"thread-self"/numeric path
 * component naming the pid directory for a userns id-map file. Return the
 * number as seen from the *calling task's own* active pid namespace, not
 * the raw/global (init_pid_ns) number.
 *
 * find_get_pid(nr)/find_vpid(nr) always resolve @nr relative to
 * task_active_pid_ns(current) -- there is no "give me the real/global
 * pid" variant. "self"/"thread-self" must therefore resolve via
 * task_tgid_vnr()/task_pid_vnr() (virtual, i.e. relative to the caller's
 * own namespace), matching task_active_pid_ns(current) exactly, not the
 * raw task_tgid_nr()/task_pid_nr(). Using the raw number here silently
 * works while the caller is still running in init_pid_ns (raw == virtual
 * there), but breaks for any task that has already become PID 1 of its
 * own pid namespace via clone3(CLONE_NEWPID) -- exactly a container's own
 * init process -- since find_get_pid() then looks up a large raw/global
 * number inside a small-numbered idr and finds nothing, making the whole
 * resulting fd's later magic-link reopen (".../fd/<n>", which every
 * modern runc/containerd performs as a defensive "unsafe procfs" check)
 * fail with -ENOENT even though the original open succeeded.
 *
 * Numeric components are passed through unchanged, matching the same
 * "already namespace-relative to the resolving task" contract:
 * find_get_pid() is later called by that same task, so a caller naming a
 * pid directory by an explicit number is expected to already be using
 * whatever numbering its own task's active pid namespace observes (e.g. a
 * numeric /proc/<pid>/... path typed from inside a container names a pid
 * relative to that container's own pid namespace, exactly like the real
 * kernel's own procfs would resolve it there). This is a pre-existing
 * limitation carried over unchanged from before this fix, not something
 * newly introduced by switching "self"/"thread-self" to the vnr() forms
 * above.
 */
static pid_t vns_resolve_ns_pid(const char *comp)
{
	long val;

	if (!strcmp(comp, "self"))
		return task_tgid_vnr(current);
	if (!strcmp(comp, "thread-self"))
		return task_pid_vnr(current);
	if (kstrtol(comp, 10, &val) || val <= 0 || val > INT_MAX)
		return 0;
	return (pid_t)val;
}




/*
 * /proc/<pid>/{uid_map,gid_map,projid_map,setgroups} fabrication on kernels
 * genuinely missing CONFIG_USER_NS: fs/proc/base.c only wires up these
 * per-pid dentries "#ifdef CONFIG_USER_NS", so:
 *   - modern runc/containerd's unconditional open()/openat2() sanity-check
 *     of "self/setgroups" as part of its "is this really an unrestricted
 *     procfs" probe fails with plain -ENOENT and aborts container creation
 *     with "unsafe procfs detected", independent of whether the container
 *     itself asked for a new user namespace;
 *   - every container runtime's classic "0 <host-id> 1" uid_map/gid_map
 *     write (the actual mechanism establishing a container's uid/gid
 *     remap) fails outright with -ENOENT before it can even open the file.
 *
 * All four fabricated descriptors are wired to vendor_kernel's own real
 * per-task user_namespace (kernel/user_namespace.c's
 * vns_proc_{uid,gid,projid}_seq_operations / vns_proc_*_map_write() /
 * vns_proc_setgroups_show()/_write(), reachable via the target task's
 * cred->user_ns) rather than a cosmetic probe stub: unlike setgroups(7) in
 * isolation, uid_map/gid_map content is not merely probed, it is the
 * mechanism itself, and new_idmap_permitted()'s unprivileged single-mapping
 * rule depends on setgroups' real ns->flags (CVE-2014-8989) too, so
 * setgroups must be real here for uid_map/gid_map to behave correctly at
 * all when written by an unprivileged caller.
 *
 * The actual file_operations callbacks (open/read/write) and
 * vns_idmap_create_fd() itself live in glue/vendor_kernel_procfs_userns.c,
 * a separate translation unit kept out of lkm4ctr/Makefile's
 * VNS_CFI_UNSAFE_OBJS: those callbacks are called back into indirectly by
 * the kernel's own
 * (KCFI-checked) VFS and must keep ordinary CFI instrumentation to remain
 * valid indirect-call targets. See that file's header comment for the full
 * rationale.
 */
static bool vns_path_wants_idmap(int dfd, const char __user *upath,
				  enum vns_idmap_kind *out_kind, pid_t *out_pid)
{
	char buf[192];
	long n;
	char *slash, *base, *dir_last;

	if (!upath)
		return false;

	n = strncpy_from_user(buf, upath, sizeof(buf));
	if (n <= 0 || n >= sizeof(buf))
		return false;

	slash = strrchr(buf, '/');
	base = slash ? slash + 1 : buf;
	if (!strcmp(base, "uid_map"))
		*out_kind = VNS_IDMAP_UID;
	else if (!strcmp(base, "gid_map"))
		*out_kind = VNS_IDMAP_GID;
	else if (!strcmp(base, "projid_map"))
		*out_kind = VNS_IDMAP_PROJID;
	else if (!strcmp(base, "setgroups"))
		*out_kind = VNS_IDMAP_SETGROUPS;
	else
		return false;

	/*
	 * A bare leaf name (no directory prefix) would need to be resolved
	 * relative to whatever pid directory @dfd itself is already open on
	 * -- rare in practice (every observed caller uses a full
	 * "/proc/<pid>/..." path) and not worth the extra dentry-to-pid
	 * lookup complexity; fall through to the real -ENOENT for that case.
	 */
	if (!slash)
		return false;

	*slash = '\0';
	dir_last = strrchr(buf, '/');
	dir_last = dir_last ? dir_last + 1 : buf;
	if (!vns_component_is_pid_dir(dir_last))
		return false;

	*out_pid = vns_resolve_ns_pid(dir_last);
	return *out_pid > 0;
}

static long (*real_sys_openat2)(const struct pt_regs *regs);
static long (*real_sys_openat)(const struct pt_regs *regs);
static long (*real_sys_open)(const struct pt_regs *regs);

/* Open fallback for the separate user-namespace id-map procfs files. */
static long vns_open_fallback(int dfd, const char __user *upath, long ret)
{
	enum vns_idmap_kind kind;
	pid_t pid;

	if (ret != -ENOENT)
		return ret;

	if (vns_path_wants_idmap(dfd, upath, &kind, &pid))
		return vns_idmap_create_fd(pid, kind);

	return ret;
}

static long vendor_kernel_hook_openat2(const struct pt_regs *regs)
{
	int dfd = (int)vns_procfs_arg0(regs);
	const char __user *upath =
		(const char __user *)(uintptr_t)vns_procfs_arg1(regs);
	long ret = real_sys_openat2(regs);

	return vns_open_fallback(dfd, upath, ret);
}

static long vendor_kernel_hook_openat(const struct pt_regs *regs)
{
	int dfd = (int)vns_procfs_arg0(regs);
	const char __user *upath =
		(const char __user *)(uintptr_t)vns_procfs_arg1(regs);
	long ret = real_sys_openat(regs);

	return vns_open_fallback(dfd, upath, ret);
}

static long vendor_kernel_hook_open(const struct pt_regs *regs)
{
	const char __user *upath =
		(const char __user *)(uintptr_t)vns_procfs_arg0(regs);
	long ret = real_sys_open(regs);

	return vns_open_fallback(AT_FDCWD, upath, ret);
}

static const char * const vendor_kernel_openat2_names[] = {
	"__arm64_sys_openat2", "__x64_sys_openat2", "sys_openat2", NULL,
};
static const char * const vendor_kernel_openat_names[] = {
	"__arm64_sys_openat", "__x64_sys_openat", "sys_openat", NULL,
};
static const char * const vendor_kernel_open_names[] = {
	"__arm64_sys_open", "__x64_sys_open", "sys_open", NULL,
};

static struct shadow_hook vendor_kernel_openat2_hook =
	SHADOW_HOOK(vendor_kernel_openat2_names, vendor_kernel_hook_openat2,
		    &real_sys_openat2);
static struct shadow_hook vendor_kernel_openat_hook =
	SHADOW_HOOK(vendor_kernel_openat_names, vendor_kernel_hook_openat,
		    &real_sys_openat);
static struct shadow_hook vendor_kernel_open_hook =
	SHADOW_HOOK(vendor_kernel_open_names, vendor_kernel_hook_open,
		    &real_sys_open);

struct shadow_hook *vendor_kernel_procfs_hooks[] = {
	&vendor_kernel_proc_evict_inode_hook,
	&vendor_kernel_proc_ns_dir_lookup_hook,
	&vendor_kernel_openat2_hook,
	&vendor_kernel_openat_hook,
	&vendor_kernel_open_hook,
	NULL,
};
