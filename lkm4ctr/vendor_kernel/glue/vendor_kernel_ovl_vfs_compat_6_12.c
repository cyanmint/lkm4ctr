// SPDX-License-Identifier: GPL-2.0-only
/*
 * vendor_kernel_ovl_vfs_compat_6_12.c - resolves the VFS-internal helpers used
 * by the android16-6.12 vendored overlayfs at module load time. This is NEW
 * code (not vendored from kernel-common). See
 * vendor_kernel_ovl_vfs_compat_6_12.h for the full rationale.
 */
#include <linux/version.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0) && LINUX_VERSION_CODE < KERNEL_VERSION(6, 13, 0)

#include <linux/kernel.h>
#include <linux/cred.h>
#include <linux/dcache.h>
#include <linux/errseq.h>
#include <linux/exportfs.h>
#include <linux/fileattr.h>
#include <linux/fs.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/posix_acl.h>
#include <linux/posix_acl_xattr.h>
#include <linux/security.h>
#include <linux/splice.h>
#include <linux/uio.h>
#include <linux/xattr.h>

#include "../../../common/shadow_hook.h"
#include "../../../common/lkm4ctr_log.h"

/*
 * Real pointer definitions matching the extern typeof() declarations in
 * vendor_kernel_ovl_vfs_compat_6_12.h (must be built from this same file, before
 * that header's #define pass has any effect).
 */
#undef prepare_creds
#undef errseq_sample
#undef mntput
#undef __mnt_is_readonly
#undef vfs_statfs
#undef clone_private_mount
#undef lock_rename
#undef unlock_rename
#undef take_dentry_name_snapshot
#undef vfs_rename
#undef lookup_one
#undef release_dentry_name_snapshot
#undef vfs_setxattr
#undef vfs_removexattr
#undef get_anon_bdev
#undef kern_unmount_array
#undef free_anon_bdev
#undef kern_path
#undef dget_parent
#undef inode_owner_or_capable
#undef exportfs_decode_fh
#undef exportfs_encode_inode_fh
#undef is_subdir
#undef vfs_getxattr
#undef lookup_one_positive_unlocked
#undef lookup_one_unlocked
#undef __d_drop
#undef vfs_getattr
#undef vfs_getattr_nosec
#undef generic_fill_statx_attr
#undef vfs_listxattr
#undef get_cached_acl_rcu
#undef vfs_get_acl
#undef posix_acl_clone
#undef vfs_set_acl
#undef vfs_remove_acl
#undef vfs_fileattr_set
#undef vfs_fileattr_get
#undef inode_insert5
#undef vfs_get_link
#undef vfs_rmdir
#undef vfs_link
#undef vfs_mknod
#undef vfs_mkdir
#undef vfs_create
#undef vfs_symlink
#undef security_dentry_create_files_as
#undef posix_acl_create
#undef security_inode_copy_up_xattr
#undef security_inode_copy_up
#undef do_splice_direct
#undef d_find_any_alias
#undef vfs_fallocate
#undef kernel_tmpfile_open
#undef vfs_copy_file_range
#undef vfs_dedupe_file_range_one
#undef vfs_clone_file_range

#define VNS_OVL_VFS_COMPAT_6_12_LIST(X) \
	X(prepare_creds) \
	X(errseq_sample) \
	X(mntput) \
	X(__mnt_is_readonly) \
	X(vfs_statfs) \
	X(clone_private_mount) \
	X(lock_rename) \
	X(unlock_rename) \
	X(take_dentry_name_snapshot) \
	X(vfs_rename) \
	X(lookup_one) \
	X(release_dentry_name_snapshot) \
	X(vfs_setxattr) \
	X(vfs_removexattr) \
	X(get_anon_bdev) \
	X(kern_unmount_array) \
	X(free_anon_bdev) \
	X(kern_path) \
	X(dget_parent) \
	X(inode_owner_or_capable) \
	X(exportfs_decode_fh) \
	X(exportfs_encode_inode_fh) \
	X(is_subdir) \
	X(vfs_getxattr) \
	X(lookup_one_positive_unlocked) \
	X(lookup_one_unlocked) \
	X(__d_drop) \
	X(vfs_getattr) \
	X(vfs_getattr_nosec) \
	X(generic_fill_statx_attr) \
	X(vfs_listxattr) \
	X(get_cached_acl_rcu) \
	X(vfs_get_acl) \
	X(posix_acl_clone) \
	X(vfs_set_acl) \
	X(vfs_remove_acl) \
	X(vfs_fileattr_set) \
	X(vfs_fileattr_get) \
	X(inode_insert5) \
	X(vfs_get_link) \
	X(vfs_rmdir) \
	X(vfs_link) \
	X(vfs_mknod) \
	X(vfs_mkdir) \
	X(vfs_create) \
	X(vfs_symlink) \
	X(security_dentry_create_files_as) \
	X(posix_acl_create) \
	X(security_inode_copy_up_xattr) \
	X(security_inode_copy_up) \
	X(do_splice_direct) \
	X(d_find_any_alias) \
	X(vfs_fallocate) \
	X(kernel_tmpfile_open) \
	X(vfs_copy_file_range) \
	X(vfs_dedupe_file_range_one) \
	X(vfs_clone_file_range)

#define VNS_OVL_VFSC_6_12_DEFINE(name) typeof(name) *vns_ovl_vfsc_6_12_##name;
VNS_OVL_VFS_COMPAT_6_12_LIST(VNS_OVL_VFSC_6_12_DEFINE)
#undef VNS_OVL_VFSC_6_12_DEFINE

int vns_ovl_vfs_compat_6_12_resolve(void)
{
#define VNS_OVL_VFSC_6_12_RESOLVE(name) \
	do { \
		vns_ovl_vfsc_6_12_##name = (typeof(vns_ovl_vfsc_6_12_##name)) \
			shadow_hook_resolve(#name); \
		if (!vns_ovl_vfsc_6_12_##name) { \
			LKM4CTR_ERR("vendor_kernel", \
				    "overlayfs_6_12: could not resolve VFS symbol '%s'; vendored overlayfs unavailable", \
				    #name); \
			return -ENOENT; \
		} \
	} while (0);
	VNS_OVL_VFS_COMPAT_6_12_LIST(VNS_OVL_VFSC_6_12_RESOLVE)
#undef VNS_OVL_VFSC_6_12_RESOLVE
	return 0;
}

#endif /* LINUX_VERSION_CODE in [6.12, 6.13) */
