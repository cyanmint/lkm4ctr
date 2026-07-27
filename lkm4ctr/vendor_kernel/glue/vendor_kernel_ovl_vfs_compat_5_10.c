// SPDX-License-Identifier: GPL-2.0-only
/*
 * vendor_kernel_ovl_vfs_compat_5_10.c - resolves the VFS-internal helpers
 * declared in vendor_kernel_ovl_vfs_compat_5_10.h at module load time. This is
 * NEW code (not vendored from kernel-common). See
 * vendor_kernel_ovl_vfs_compat_5_10.h for the full rationale.
 */
#include <linux/version.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0) && LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 0)

#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/mount.h>
#include <linux/xattr.h>
#include <linux/security.h>
#include <linux/cred.h>
#include <linux/posix_acl.h>
#include <linux/posix_acl_xattr.h>
#include <linux/exportfs.h>
#include <linux/splice.h>
#include <linux/dcache.h>
#include <linux/uio.h>
#include <linux/errseq.h>
#include <linux/rwsem.h>

#include "../../../common/shadow_hook.h"
#include "../../../common/lkm4ctr_log.h"

/* Real pointer definitions matching the extern typeof() declarations in
 * vendor_kernel_ovl_vfs_compat_5_10.h (must be built from this same file,
 * before that header's #define pass has any effect - so include it only once,
 * after these).
 */
#undef mount_nodev
#undef prepare_creds
#undef errseq_sample
#undef mntput
#undef vfs_statfs
#undef clone_private_mount
#undef lock_rename
#undef unlock_rename
#undef take_dentry_name_snapshot
#undef vfs_rename
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
#undef is_subdir
#undef vfs_getxattr
#undef __vfs_getxattr
#undef lookup_one_len_unlocked
#undef __d_drop
#undef vfs_getattr
#undef vfs_listxattr
#undef get_acl
#undef inode_insert5
#undef vfs_get_link
#undef vfs_unlink
#undef vfs_rmdir
#undef vfs_link
#undef vfs_mknod
#undef vfs_mkdir
#undef vfs_create
#undef vfs_symlink
#undef vfs_tmpfile
#undef security_dentry_create_files_as
#undef posix_acl_create
#undef security_inode_copy_up_xattr
#undef exportfs_encode_fh
#undef security_inode_copy_up
#undef do_clone_file_range
#undef do_splice_direct
#undef d_find_any_alias
#undef d_alloc_anon
#undef d_instantiate_anon
#undef vfs_iocb_iter_read
#undef vfs_iter_read
#undef vfs_iocb_iter_write
#undef vfs_iter_write
#undef iter_file_splice_write
#undef vfs_fsync_range
#undef vfs_fallocate
#undef dentry_open
#undef open_with_fake_path
#undef vfs_copy_file_range
#undef vfs_dedupe_file_range_one
#undef vfs_clone_file_range

#undef d_invalidate
#undef errseq_check
#undef iterate_dir
#undef lookup_positive_unlocked
#undef override_creds
#undef revert_creds
#undef security_file_ioctl
#undef vfs_fadvise
#undef vfs_ioctl
#undef vfs_setpos

#undef down_write_killable

#define VNS_OVL_VFS_COMPAT_5_10_LIST(X) \
	X(mount_nodev) \
	X(prepare_creds) \
	X(errseq_sample) \
	X(mntput) \
	X(vfs_statfs) \
	X(clone_private_mount) \
	X(lock_rename) \
	X(unlock_rename) \
	X(take_dentry_name_snapshot) \
	X(vfs_rename) \
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
	X(is_subdir) \
	X(vfs_getxattr) \
	X(__vfs_getxattr) \
	X(lookup_one_len_unlocked) \
	X(__d_drop) \
	X(vfs_getattr) \
	X(vfs_listxattr) \
	X(get_acl) \
	X(inode_insert5) \
	X(vfs_get_link) \
	X(vfs_unlink) \
	X(vfs_rmdir) \
	X(vfs_link) \
	X(vfs_mknod) \
	X(vfs_mkdir) \
	X(vfs_create) \
	X(vfs_symlink) \
	X(vfs_tmpfile) \
	X(security_dentry_create_files_as) \
	X(posix_acl_create) \
	X(security_inode_copy_up_xattr) \
	X(exportfs_encode_fh) \
	X(security_inode_copy_up) \
	X(do_clone_file_range) \
	X(do_splice_direct) \
	X(d_find_any_alias) \
	X(d_alloc_anon) \
	X(d_instantiate_anon) \
	X(vfs_iocb_iter_read) \
	X(vfs_iter_read) \
	X(vfs_iocb_iter_write) \
	X(vfs_iter_write) \
	X(iter_file_splice_write) \
	X(vfs_fsync_range) \
	X(vfs_fallocate) \
	X(dentry_open) \
	X(open_with_fake_path) \
	X(vfs_copy_file_range) \
	X(vfs_dedupe_file_range_one) \
	X(vfs_clone_file_range) \
	X(d_invalidate) \
	X(errseq_check) \
	X(iterate_dir) \
	X(lookup_positive_unlocked) \
	X(override_creds) \
	X(revert_creds) \
	X(security_file_ioctl) \
	X(vfs_fadvise) \
	X(vfs_ioctl) \
	X(vfs_setpos) \
	X(down_write_killable)

#define VNS_OVL_VFSC_5_10_DEFINE(name) typeof(name) *vns_ovl_vfsc_5_10_##name;
VNS_OVL_VFS_COMPAT_5_10_LIST(VNS_OVL_VFSC_5_10_DEFINE)
#undef VNS_OVL_VFSC_5_10_DEFINE

int vns_ovl_vfs_compat_5_10_resolve(void)
{
#define VNS_OVL_VFSC_5_10_RESOLVE(name) \
	do { \
		vns_ovl_vfsc_5_10_##name = (typeof(vns_ovl_vfsc_5_10_##name)) \
			shadow_hook_resolve(#name); \
		if (!vns_ovl_vfsc_5_10_##name) { \
			LKM4CTR_ERR("vendor_kernel", \
				    "overlayfs_5_10: could not resolve VFS symbol '%s'; vendored overlayfs unavailable", \
				    #name); \
			return -ENOENT; \
		} \
	} while (0);
	VNS_OVL_VFS_COMPAT_5_10_LIST(VNS_OVL_VFSC_5_10_RESOLVE)
#undef VNS_OVL_VFSC_5_10_RESOLVE
	return 0;
}

#endif /* LINUX_VERSION_CODE in [5.10, 5.15) */
