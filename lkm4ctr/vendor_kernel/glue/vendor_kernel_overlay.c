// SPDX-License-Identifier: GPL-2.0
/*
 * vendor_kernel_overlay.c - lifecycle + registration glue for the vendored
 * fs/overlayfs (vendor_kernel/fs/overlayfs/). This is NEW code (not
 * vendored from kernel-common).
 *
 * fs/overlayfs is upstream's own out-of-tree-buildable filesystem module,
 * so no functional edits were needed there beyond: kmem_cache
 * init/teardown split out of module_init()/module_exit() into plain
 * vns_ovl_init()/vns_ovl_exit() functions (lkm4ctr.ko already has a single
 * module_init/module_exit pair in lkm4ctr_main.c), and never calling
 * register_filesystem()/unregister_filesystem() on vns_ovl_fs_type at all
 * (see fs/overlayfs/super.c).
 *
 * Why hook get_fs_type() instead of register_filesystem()
 * ---------------------------------------------------------
 * The running kernel's own "overlay" file_system_type (built-in, or a
 * separate overlay.ko) may already be registered in the global
 * file_systems list. Calling register_filesystem(&vns_ovl_fs_type) with
 * the same name "overlay" would fail outright with -EBUSY whenever a real
 * one is already present, and unregister_filesystem() at unload time would
 * then tear down whichever one actually "won" the race -- both undesirable.
 *
 * Instead this hooks get_fs_type(), the exported kernel function
 * mount(2)'s filesystem-type lookup path already calls to resolve a name
 * to a struct file_system_type *. For the name "overlay", this hook always
 * hands out vns_ovl_fs_type -- unconditionally, regardless of whether the
 * running kernel has its own working overlayfs -- without ever calling
 * through to the real get_fs_type() for that name (so no module reference
 * is ever taken against a real overlay.ko, and it is never even looked
 * up). "mount -t overlay ..." therefore always uses this vendored
 * implementation while this module is loaded. Every other filesystem name
 * passes through to the real get_fs_type() untouched.
 */
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/version.h>

#include "../vendor_kernel.h"
#include "../../../common/shadow_hook.h"
#include "../../../common/lkm4ctr_log.h"

/*
 * The vendored fs/overlayfs sources (vendor_kernel/fs/overlayfs, *.c files) are
 * only compiled for kernels in [6.1, 6.3) -- see the [BUILD-COMPAT] comment
 * at the top of each of those files. Outside that range, vns_ovl_init(),
 * vns_ovl_exit() and vns_ovl_fs_type do not even exist (those files compile
 * to nothing), so the get_fs_type() override must not be installed; the
 * running kernel's own overlay implementation is used instead. Missing
 * vendored overlayfs support is not a fatal condition for the rest of
 * vendor_kernel, so vns_overlay_init() still returns 0 in that case.
 */
/*
 * Lower bound 6.1: vfs_tmpfile_open()/alloc_inode_sb()/vfs_set_acl_prepare()
 * (all used unconditionally by the vendored sources) are not present before
 * 6.1. Upper bound 6.3 (exclusive): from 6.3 onward the VFS idmap argument
 * type changes from struct user_namespace * to struct mnt_idmap * (the
 * vendored sources are hard-coded to the former), and mnt_user_ns() is
 * removed in favor of mnt_idmap().
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0) && LINUX_VERSION_CODE < KERNEL_VERSION(6, 3, 0)

static struct file_system_type *(*real_get_fs_type)(const char *name);

static struct file_system_type *vendor_kernel_hook_get_fs_type(const char *name)
{
	if (!strcmp(name, "overlay")) {
		if (!try_module_get(vns_ovl_fs_type.owner))
			return NULL;
		return &vns_ovl_fs_type;
	}
	return real_get_fs_type(name);
}

static const char * const vendor_kernel_get_fs_type_names[] = {
	"get_fs_type",
	NULL,
};

static struct shadow_hook vendor_kernel_get_fs_type_hook =
	SHADOW_HOOK(vendor_kernel_get_fs_type_names, vendor_kernel_hook_get_fs_type,
		    &real_get_fs_type);

struct shadow_hook *vendor_kernel_overlay_hooks[] = {
	&vendor_kernel_get_fs_type_hook,
	NULL,
};

int vns_overlay_init(void)
{
	int err = vns_ovl_init();

	if (err) {
		LKM4CTR_ERR("vendor_kernel", "failed to init vendored overlayfs (%d)", err);
		return err;
	}
	err = shadow_hook_install_all(vendor_kernel_overlay_hooks, "vendor_kernel_overlay");
	if (err < 0) {
		LKM4CTR_ERR("vendor_kernel", "failed to install get_fs_type() overlay override hook (%d)", err);
		vns_ovl_exit();
		return err;
	}
	LKM4CTR_INFO("vendor_kernel", "vendored overlayfs now handles every get_fs_type(\"overlay\") lookup");
	return 0;
}

void vns_overlay_exit(void)
{
	shadow_hook_remove_all(vendor_kernel_overlay_hooks);
	vns_ovl_exit();
	LKM4CTR_INFO("vendor_kernel", "vendored overlayfs override removed");
}

size_t vns_overlay_diag_snprintf(char *buf, size_t buflen)
{
	return scnprintf(buf, buflen, "overlay_mounts: %d\n",
			 atomic_read(&vns_ovl_mount_count));
}

#else /* !(LINUX_VERSION_CODE in [6.1, 6.3)) */

int vns_overlay_init(void)
{
	LKM4CTR_INFO("vendor_kernel",
		     "vendored overlayfs is not supported on this kernel version; running kernel's own overlay implementation is used for \"mount -t overlay ...\"");
	return 0;
}

void vns_overlay_exit(void)
{
}

size_t vns_overlay_diag_snprintf(char *buf, size_t buflen)
{
	return scnprintf(buf, buflen, "overlay_mounts: unsupported\n");
}

#endif /* LINUX_VERSION_CODE in [6.1, 6.3) */
