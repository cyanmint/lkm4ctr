// SPDX-License-Identifier: GPL-2.0-only
/*
 * vendor_kernel_ovl_vfs_compat.c - resolves, at module-load time, every
 * VFS/security/exportfs helper the single vendored overlayfs source
 * (vendor_kernel/fs/overlayfs/) reaches for but that production GKI builds may
 * have trimmed from the module symbol table (CONFIG_TRIM_UNUSED_KSYMS,
 * protected-KMI allow-lists). This is NEW code (not vendored from
 * kernel-common). See vendor_kernel_ovl_vfs_compat.h for the full rationale and
 * the per-tier symbol lists.
 *
 * VNS_OVL_VFS_COMPAT_IMPL is defined before including the header so that the
 * header skips its "pass 2" bare-name redirects: typeof(name) below therefore
 * still refers to the running kernel's real declaration of each helper, which
 * is exactly what we need to declare/define a same-signature function pointer.
 */
#include <linux/version.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0) && LINUX_VERSION_CODE < KERNEL_VERSION(6, 19, 0)

#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/fs_parser.h>
#include <linux/namei.h>
#include <linux/xattr.h>
#include <linux/security.h>
#include <linux/cred.h>
#include <linux/capability.h>
#include <linux/dcache.h>
#include <linux/mount.h>
#include <linux/posix_acl.h>
#include <linux/posix_acl_xattr.h>
#include <linux/exportfs.h>
#include <linux/splice.h>
#include <linux/uio.h>
#include <linux/errseq.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 13, 0)
#include <linux/fileattr.h>
#endif
#include <linux/seq_file.h>
#include <linux/parser.h>
#include <linux/fadvise.h>
#include <linux/uuid.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
#include <linux/backing-file.h>
#endif

#define VNS_OVL_VFS_COMPAT_IMPL
#include "vendor_kernel_ovl_vfs_compat.h"

#include "../../../common/shadow_hook.h"
#include "../../../common/lkm4ctr_log.h"

/* Define the resolved function-pointer storage (extern-declared in the header). */
#define VNS_OVL_VFSC_DEFINE(name) typeof(name) *vns_ovl_vfsc_##name;
VNS_OVL_VFS_COMPAT_LIST(VNS_OVL_VFSC_DEFINE)
VNS_OVL_VFS_COMPAT_LIST_BF(VNS_OVL_VFSC_DEFINE)
#undef VNS_OVL_VFSC_DEFINE

int vns_ovl_vfs_compat_resolve(void)
{
#define VNS_OVL_VFSC_RESOLVE(name) \
	do { \
		vns_ovl_vfsc_##name = (typeof(vns_ovl_vfsc_##name)) \
			shadow_hook_resolve(#name); \
		if (!vns_ovl_vfsc_##name) { \
			LKM4CTR_ERR("vendor_kernel", \
				    "overlayfs: could not resolve VFS symbol '%s'; vendored overlayfs unavailable", \
				    #name); \
			return -ENOENT; \
		} \
	} while (0);
	VNS_OVL_VFS_COMPAT_LIST(VNS_OVL_VFSC_RESOLVE)
	VNS_OVL_VFS_COMPAT_LIST_BF(VNS_OVL_VFSC_RESOLVE)
#undef VNS_OVL_VFSC_RESOLVE
	return 0;
}

#endif /* LINUX_VERSION_CODE in [5.10, 6.19) */
