/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * vendor_kernel_ovl_vfs_compat_6_12.h - lkm4ctr [BUILD-COMPAT]: resolve the
 * VFS/security/exportfs helpers the android16-6.12 vendored overlayfs pulls in
 * directly but that may be missing from the running kernel's module symbol
 * table on production GKI builds (CONFIG_TRIM_UNUSED_KSYMS, protected KMI
 * allow-lists, etc.).
 *
 * This is NEW code (not vendored from kernel-common).
 *
 * Fix: resolve each symbol below by name at module-load time via
 * shadow_hook_resolve(), then #define the bare call-site name to dereference a
 * same-signature function pointer (`typeof(name) *`) whose underlying storage
 * uses a version-specific `vns_ovl_vfsc_6_12_` prefix. The bare-name #defines
 * keep almost every call site in the vendored overlayfs untouched while
 * avoiding direct module linkage against possibly-trimmed kernel helpers.
 *
 * generic_delete_inode() and noop_direct_IO() are not redirected here: they
 * are only needed as compile-time-constant initializer values, so super.c and
 * inode.c provide tiny local equivalents instead. vfs_whiteout() is likewise
 * not listed because it is a static inline in <linux/fs.h>; overlayfs.h
 * reimplements its one-line body through the resolved vfs_mknod().
 */
#ifndef _VNS_OVL_VFS_COMPAT_6_12_H
#define _VNS_OVL_VFS_COMPAT_6_12_H

#include <linux/version.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0) && LINUX_VERSION_CODE < KERNEL_VERSION(6, 13, 0)

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
	X(vfs_clone_file_range) \
	X(backing_file_mmap) \
	X(backing_file_read_iter) \
	X(backing_file_write_iter) \
	X(backing_file_splice_read) \
	X(backing_file_splice_write)

/*
 * Pass 1: declare the resolved function pointers while every name in
 * VNS_OVL_VFS_COMPAT_6_12_LIST() still refers to the real kernel declaration.
 */
#define VNS_OVL_VFSC_6_12_DECLARE(name) extern typeof(name) *vns_ovl_vfsc_6_12_##name;
VNS_OVL_VFS_COMPAT_6_12_LIST(VNS_OVL_VFSC_6_12_DECLARE)
#undef VNS_OVL_VFSC_6_12_DECLARE
#define backing_file_mmap (*vns_ovl_vfsc_6_12_backing_file_mmap)
#define backing_file_read_iter (*vns_ovl_vfsc_6_12_backing_file_read_iter)
#define backing_file_write_iter (*vns_ovl_vfsc_6_12_backing_file_write_iter)
#define backing_file_splice_read (*vns_ovl_vfsc_6_12_backing_file_splice_read)
#define backing_file_splice_write (*vns_ovl_vfsc_6_12_backing_file_splice_write)

/*
 * Pass 2: redirect every bare use of each name (call expression or
 * function-pointer value) to go through the resolved pointer instead.
 */
#define prepare_creds (*vns_ovl_vfsc_6_12_prepare_creds)
#define errseq_sample (*vns_ovl_vfsc_6_12_errseq_sample)
#define mntput (*vns_ovl_vfsc_6_12_mntput)
#define __mnt_is_readonly (*vns_ovl_vfsc_6_12___mnt_is_readonly)
#define vfs_statfs (*vns_ovl_vfsc_6_12_vfs_statfs)
#define clone_private_mount (*vns_ovl_vfsc_6_12_clone_private_mount)
#define lock_rename (*vns_ovl_vfsc_6_12_lock_rename)
#define unlock_rename (*vns_ovl_vfsc_6_12_unlock_rename)
#define take_dentry_name_snapshot (*vns_ovl_vfsc_6_12_take_dentry_name_snapshot)
#define vfs_rename (*vns_ovl_vfsc_6_12_vfs_rename)
#define lookup_one (*vns_ovl_vfsc_6_12_lookup_one)
#define release_dentry_name_snapshot (*vns_ovl_vfsc_6_12_release_dentry_name_snapshot)
#define vfs_setxattr (*vns_ovl_vfsc_6_12_vfs_setxattr)
#define vfs_removexattr (*vns_ovl_vfsc_6_12_vfs_removexattr)
#define get_anon_bdev (*vns_ovl_vfsc_6_12_get_anon_bdev)
#define kern_unmount_array (*vns_ovl_vfsc_6_12_kern_unmount_array)
#define free_anon_bdev (*vns_ovl_vfsc_6_12_free_anon_bdev)
#define kern_path (*vns_ovl_vfsc_6_12_kern_path)
#define dget_parent (*vns_ovl_vfsc_6_12_dget_parent)
#define inode_owner_or_capable (*vns_ovl_vfsc_6_12_inode_owner_or_capable)
#define exportfs_decode_fh (*vns_ovl_vfsc_6_12_exportfs_decode_fh)
#define exportfs_encode_inode_fh (*vns_ovl_vfsc_6_12_exportfs_encode_inode_fh)
#define is_subdir (*vns_ovl_vfsc_6_12_is_subdir)
#define vfs_getxattr (*vns_ovl_vfsc_6_12_vfs_getxattr)
#define lookup_one_positive_unlocked (*vns_ovl_vfsc_6_12_lookup_one_positive_unlocked)
#define lookup_one_unlocked (*vns_ovl_vfsc_6_12_lookup_one_unlocked)
#define __d_drop (*vns_ovl_vfsc_6_12___d_drop)
#define vfs_getattr (*vns_ovl_vfsc_6_12_vfs_getattr)
#define vfs_getattr_nosec (*vns_ovl_vfsc_6_12_vfs_getattr_nosec)
#define generic_fill_statx_attr (*vns_ovl_vfsc_6_12_generic_fill_statx_attr)
#define vfs_listxattr (*vns_ovl_vfsc_6_12_vfs_listxattr)
#define get_cached_acl_rcu (*vns_ovl_vfsc_6_12_get_cached_acl_rcu)
#define vfs_get_acl (*vns_ovl_vfsc_6_12_vfs_get_acl)
#define posix_acl_clone (*vns_ovl_vfsc_6_12_posix_acl_clone)
#define vfs_set_acl (*vns_ovl_vfsc_6_12_vfs_set_acl)
#define vfs_remove_acl (*vns_ovl_vfsc_6_12_vfs_remove_acl)
#define vfs_fileattr_set (*vns_ovl_vfsc_6_12_vfs_fileattr_set)
#define vfs_fileattr_get (*vns_ovl_vfsc_6_12_vfs_fileattr_get)
#define inode_insert5 (*vns_ovl_vfsc_6_12_inode_insert5)
#define vfs_get_link (*vns_ovl_vfsc_6_12_vfs_get_link)
#define vfs_rmdir (*vns_ovl_vfsc_6_12_vfs_rmdir)
#define vfs_link (*vns_ovl_vfsc_6_12_vfs_link)
#define vfs_mknod (*vns_ovl_vfsc_6_12_vfs_mknod)
#define vfs_mkdir (*vns_ovl_vfsc_6_12_vfs_mkdir)
#define vfs_create (*vns_ovl_vfsc_6_12_vfs_create)
#define vfs_symlink (*vns_ovl_vfsc_6_12_vfs_symlink)
#define security_dentry_create_files_as (*vns_ovl_vfsc_6_12_security_dentry_create_files_as)
#define posix_acl_create (*vns_ovl_vfsc_6_12_posix_acl_create)
#define security_inode_copy_up_xattr (*vns_ovl_vfsc_6_12_security_inode_copy_up_xattr)
#define security_inode_copy_up (*vns_ovl_vfsc_6_12_security_inode_copy_up)
#define do_splice_direct (*vns_ovl_vfsc_6_12_do_splice_direct)
#define d_find_any_alias (*vns_ovl_vfsc_6_12_d_find_any_alias)
#define vfs_fallocate (*vns_ovl_vfsc_6_12_vfs_fallocate)
#define kernel_tmpfile_open (*vns_ovl_vfsc_6_12_kernel_tmpfile_open)
#define vfs_copy_file_range (*vns_ovl_vfsc_6_12_vfs_copy_file_range)
#define vfs_dedupe_file_range_one (*vns_ovl_vfsc_6_12_vfs_dedupe_file_range_one)
#define vfs_clone_file_range (*vns_ovl_vfsc_6_12_vfs_clone_file_range)

int vns_ovl_vfs_compat_6_12_resolve(void);

#endif /* LINUX_VERSION_CODE in [6.12, 6.13) */

#endif /* _VNS_OVL_VFS_COMPAT_6_12_H */
