/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * vendor_kernel_ovl_vfs_compat.h - lkm4ctr [BUILD-COMPAT]: the single, unified
 * VFS/fs_context compatibility layer for the single vendored overlayfs source
 * tree (vendor_kernel/fs/overlayfs/). This is NEW code (not vendored).
 *
 * *** GENERATED-STYLE HEADER ***
 * The per-tier VNS_OVL_VFS_COMPAT_LIST()/redirect blocks below are mechanical
 * (one entry per resolved helper). If you add/remove a resolved helper, keep
 * pass 1 (typeof declare) and pass 2 (redirect) in sync for that tier.
 *
 * vendor_kernel/fs/overlayfs/ used to be five near-duplicate copies of upstream
 * overlayfs, one per VFS-API era. They were collapsed to a SINGLE copy taken
 * from android16-6.12 (kernel 6.12.x). This header lets that one 6.12-shaped
 * source build and run across every KMI in .github/workflows/build-lkm4ctr.yml:
 *
 *   android12-5.10, android13-5.10   kernel 5.10   -> tier OLD [5.10, 5.12)
 *   android13-5.15, android14-5.15   kernel 5.15   -> tier MID [5.12, 6.3)
 *   android14-6.1                    kernel 6.1    -> tier MID [5.12, 6.3)
 *   android15-6.6                    kernel 6.6    -> tier NEW [6.3, 6.19)
 *   android16-6.12                   kernel 6.12   -> tier NEW (native)
 *   android17-6.18                   kernel 6.18   -> tier NEW [6.3, 6.19)
 *
 * Three compatibility tiers
 * -------------------------
 *   tier NEW [6.3, 6.19)  Kernel threads inode-op / VFS helpers with
 *                         `struct mnt_idmap *` already; the 6.12 source is
 *                         essentially native. Bridging = (a) resolving helpers
 *                         trimmed by CONFIG_TRIM_UNUSED_KSYMS / protected-KMI,
 *                         (b) a manual backing_file rw fallback on [6.3, 6.9)
 *                         where backing_file_read_iter()/... do not exist yet
 *                         (android15-6.6 falls here for read/write/splice, but
 *                         already has backing_file_open()).
 *
 *   tier MID [5.12, 6.3)  Idmapped mounts exist but helpers take
 *                         `struct user_namespace *`. We alias the type
 *                         (`#define mnt_idmap user_namespace`), translate the
 *                         handful of 6.6+/6.9+/6.12+-only helpers back to their
 *                         [6.1, 6.3) equivalents (version-gated bodies inside
 *                         the vendored sources), and use the manual
 *                         open/read/write/splice fallback in file.c.
 *
 *   tier OLD [5.10, 5.12) No idmapped mounts: helpers take NO idmap argument.
 *                         In addition to the MID translations, the idmap the
 *                         6.12 source threads is DROPPED at each real VFS call
 *                         via the variadic redirect macros in
 *                         vendor_kernel_ovl_vfs_compat_old_idmap.h, and each
 *                         inode/file-op callback the kernel invokes is reached
 *                         through a thin wrapper thunk (see the [TIER-OLD]
 *                         blocks in dir.c / inode.c / readdir.c / file.c).
 *
 * Symbol-resolution mechanism
 * ---------------------------
 * Every name in a VNS_OVL_VFS_COMPAT_LIST(X) is a real kernel helper that may
 * be trimmed from the module symbol table on production GKI. Pass 1 declares
 * `extern typeof(name) *vns_ovl_vfsc_<name>;` while `name` still refers to the
 * real declaration; pass 2 `#define`s the bare name to `(*vns_ovl_vfsc_<name>)`;
 * the .c resolves each pointer at load via shadow_hook_resolve(). typeof() needs
 * the real declaration, so any helper absent on a tier is translated to an
 * existing equivalent (in version-gated source bodies) BEFORE that tier's list
 * is expanded -- never listed here.
 */
#ifndef _VNS_OVL_VFS_COMPAT_H
#define _VNS_OVL_VFS_COMPAT_H

#include <linux/version.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0) && LINUX_VERSION_CODE < KERNEL_VERSION(6, 19, 0)

#define VNS_OVL_TIER_NEW (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0))
#define VNS_OVL_TIER_MID (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 12, 0) && \
			  LINUX_VERSION_CODE <  KERNEL_VERSION(6, 3, 0))
#define VNS_OVL_TIER_OLD (LINUX_VERSION_CODE <  KERNEL_VERSION(5, 12, 0))

/* backing_file_open() exists since 6.6; backing_file_read_iter()/... since 6.9. */
#define VNS_OVL_HAVE_BACKING_FILE_OPEN (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0))
#define VNS_OVL_HAVE_BACKING_FILE_RW   (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0))
#define VNS_OVL_NEED_BACKING_FILE_FALLBACK (!VNS_OVL_HAVE_BACKING_FILE_RW)

/* ------------------------------------------------------------------ */
/* Idmap type / accessor bridging for tiers MID and OLD (pre-6.3).     */
/* ------------------------------------------------------------------ */
#if VNS_OVL_TIER_MID || VNS_OVL_TIER_OLD
/*
 * [BUILD-COMPAT] `struct mnt_idmap` does not exist before 6.3. Alias it to
 * `struct user_namespace` so the 6.12 source's `struct mnt_idmap *idmap`
 * parameters have the *identical* type the running kernel's inode_operations
 * expect. Object-like on purpose (also rewrites declarations). The bare
 * accessor `mnt_idmap(mnt)` was renamed to `ovl_mnt_idmap(mnt)` throughout the
 * vendored source so this alias never clobbers it.
 */
#define mnt_idmap user_namespace
#define nop_mnt_idmap init_user_ns
#endif /* MID || OLD */

#if VNS_OVL_TIER_NEW
#define ovl_mnt_idmap(mnt) mnt_idmap(mnt)
#elif VNS_OVL_TIER_MID
#define ovl_mnt_idmap(mnt) mnt_user_ns(mnt)
#else /* OLD */
#define ovl_mnt_idmap(mnt) (&nop_mnt_idmap)
#endif

/* ------------------------------------------------------------------ */
/* Split i_atime/i_mtime accessors.                                    */
/* ------------------------------------------------------------------ */
/*
 * [BUILD-COMPAT] The 6.12 source uses inode_get_atime()/inode_set_atime_to_ts()
 * and the mtime equivalents. Upstream added these split accessors together with
 * the ctime ones, but the Android GKI branches backported them piecemeal:
 * android12-5.10, android13-5.15 and android14-6.1 ship the *ctime* accessors
 * (inode_get_ctime()/inode_set_ctime_to_ts(), which the 6.12 source also uses)
 * but NOT the atime/mtime ones, while android15-6.6 (and 6.12/6.18) ship all of
 * them. So we only provide atime/mtime fallbacks below, gated < 6.6, matching
 * the actual header contents of every targeted KMI (verified against
 * android.googlesource.com include/linux/fs.h for each branch). i_atime/i_mtime
 * are plain struct timespec64 fields on those kernels.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
static inline struct timespec64 inode_get_atime(const struct inode *inode)
{
	return inode->i_atime;
}
static inline struct timespec64 inode_set_atime_to_ts(struct inode *inode,
						      struct timespec64 ts)
{
	inode->i_atime = ts;
	return ts;
}
static inline struct timespec64 inode_get_mtime(const struct inode *inode)
{
	return inode->i_mtime;
}
static inline struct timespec64 inode_set_mtime_to_ts(struct inode *inode,
						      struct timespec64 ts)
{
	inode->i_mtime = ts;
	return ts;
}
#endif /* < 6.6 */


#if VNS_OVL_TIER_NEW
#define VNS_OVL_VFS_COMPAT_LIST(X) \
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
	X(vfs_llseek)

#if !VNS_OVL_NEED_BACKING_FILE_FALLBACK
#define VNS_OVL_VFS_COMPAT_LIST_BF(X) \
	X(backing_file_read_iter) \
	X(backing_file_write_iter) \
	X(backing_file_splice_read) \
	X(backing_file_splice_write) \
	X(backing_file_mmap)
#else
#define VNS_OVL_VFS_COMPAT_LIST_BF(X) \
	X(vfs_iter_read) \
	X(vfs_iter_write) \
	X(vfs_iocb_iter_read) \
	X(vfs_iocb_iter_write) \
	X(iter_file_splice_write)
#endif
#endif /* NEW */

#if VNS_OVL_TIER_MID
#define VNS_OVL_VFS_COMPAT_LIST(X) \
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
	X(is_subdir) \
	X(vfs_getxattr) \
	X(lookup_one_positive_unlocked) \
	X(lookup_one_unlocked) \
	X(__d_drop) \
	X(vfs_getattr) \
	X(generic_fill_statx_attr) \
	X(vfs_listxattr) \
	X(get_cached_acl_rcu) \
	X(get_acl) \
	X(posix_acl_clone) \
	X(vfs_set_acl_prepare) \
	X(set_posix_acl) \
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
	X(exportfs_encode_fh) \
	X(security_inode_copy_up) \
	X(do_splice_direct) \
	X(d_find_any_alias) \
	X(vfs_fallocate) \
	X(vfs_tmpfile_open) \
	X(vfs_copy_file_range) \
	X(vfs_dedupe_file_range_one) \
	X(vfs_clone_file_range) \
	X(vfs_llseek) \
	X(inode_permission) \
	X(open_with_fake_path)

#define VNS_OVL_VFS_COMPAT_LIST_BF(X) \
	X(vfs_iter_read) \
	X(vfs_iter_write) \
	X(vfs_iocb_iter_read) \
	X(vfs_iocb_iter_write) \
	X(iter_file_splice_write)
#endif /* MID */

#if VNS_OVL_TIER_OLD
#define VNS_OVL_VFS_COMPAT_LIST(X) \
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
	X(set_posix_acl) \
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
	X(vfs_llseek) \
	X(inode_permission) \
	X(security_file_ioctl) \
	X(vfs_fadvise) \
	X(vfs_ioctl) \
	X(vfs_setpos) \
	X(down_write_killable) \
	X(generic_fillattr)

#define VNS_OVL_VFS_COMPAT_LIST_BF(X)
#endif /* OLD */

/*
 * Pass 1: declare the resolved function pointers while every name still refers
 * to the real kernel declaration (must precede the redirect #defines below).
 */
#define VNS_OVL_VFSC_DECLARE(name) extern typeof(name) *vns_ovl_vfsc_##name;
VNS_OVL_VFS_COMPAT_LIST(VNS_OVL_VFSC_DECLARE)
VNS_OVL_VFS_COMPAT_LIST_BF(VNS_OVL_VFSC_DECLARE)
#undef VNS_OVL_VFSC_DECLARE

/* Pass 2: redirect bare uses through the resolved pointers.
 *
 * Skipped when VNS_OVL_VFS_COMPAT_IMPL is defined (the resolver .c defines it so
 * that typeof(name) below still sees the real kernel declarations, not the
 * redirected pointers).
 */
#ifndef VNS_OVL_VFS_COMPAT_IMPL
#if VNS_OVL_TIER_NEW
#define prepare_creds (*vns_ovl_vfsc_prepare_creds)
#define errseq_sample (*vns_ovl_vfsc_errseq_sample)
#define mntput (*vns_ovl_vfsc_mntput)
#define __mnt_is_readonly (*vns_ovl_vfsc___mnt_is_readonly)
#define vfs_statfs (*vns_ovl_vfsc_vfs_statfs)
#define clone_private_mount (*vns_ovl_vfsc_clone_private_mount)
#define lock_rename (*vns_ovl_vfsc_lock_rename)
#define unlock_rename (*vns_ovl_vfsc_unlock_rename)
#define take_dentry_name_snapshot (*vns_ovl_vfsc_take_dentry_name_snapshot)
#define vfs_rename (*vns_ovl_vfsc_vfs_rename)
#define lookup_one (*vns_ovl_vfsc_lookup_one)
#define release_dentry_name_snapshot (*vns_ovl_vfsc_release_dentry_name_snapshot)
#define vfs_setxattr (*vns_ovl_vfsc_vfs_setxattr)
#define vfs_removexattr (*vns_ovl_vfsc_vfs_removexattr)
#define get_anon_bdev (*vns_ovl_vfsc_get_anon_bdev)
#define kern_unmount_array (*vns_ovl_vfsc_kern_unmount_array)
#define free_anon_bdev (*vns_ovl_vfsc_free_anon_bdev)
#define kern_path (*vns_ovl_vfsc_kern_path)
#define dget_parent (*vns_ovl_vfsc_dget_parent)
#define inode_owner_or_capable (*vns_ovl_vfsc_inode_owner_or_capable)
#define exportfs_decode_fh (*vns_ovl_vfsc_exportfs_decode_fh)
#define exportfs_encode_inode_fh (*vns_ovl_vfsc_exportfs_encode_inode_fh)
#define is_subdir (*vns_ovl_vfsc_is_subdir)
#define vfs_getxattr (*vns_ovl_vfsc_vfs_getxattr)
#define lookup_one_positive_unlocked (*vns_ovl_vfsc_lookup_one_positive_unlocked)
#define lookup_one_unlocked (*vns_ovl_vfsc_lookup_one_unlocked)
#define __d_drop (*vns_ovl_vfsc___d_drop)
#define vfs_getattr (*vns_ovl_vfsc_vfs_getattr)
#define generic_fill_statx_attr (*vns_ovl_vfsc_generic_fill_statx_attr)
#define vfs_listxattr (*vns_ovl_vfsc_vfs_listxattr)
#define get_cached_acl_rcu (*vns_ovl_vfsc_get_cached_acl_rcu)
#define vfs_get_acl (*vns_ovl_vfsc_vfs_get_acl)
#define posix_acl_clone (*vns_ovl_vfsc_posix_acl_clone)
#define vfs_set_acl (*vns_ovl_vfsc_vfs_set_acl)
#define vfs_remove_acl (*vns_ovl_vfsc_vfs_remove_acl)
#define vfs_fileattr_set (*vns_ovl_vfsc_vfs_fileattr_set)
#define vfs_fileattr_get (*vns_ovl_vfsc_vfs_fileattr_get)
#define inode_insert5 (*vns_ovl_vfsc_inode_insert5)
#define vfs_get_link (*vns_ovl_vfsc_vfs_get_link)
#define vfs_rmdir (*vns_ovl_vfsc_vfs_rmdir)
#define vfs_link (*vns_ovl_vfsc_vfs_link)
#define vfs_mknod (*vns_ovl_vfsc_vfs_mknod)
#define vfs_mkdir (*vns_ovl_vfsc_vfs_mkdir)
#define vfs_create (*vns_ovl_vfsc_vfs_create)
#define vfs_symlink (*vns_ovl_vfsc_vfs_symlink)
#define security_dentry_create_files_as (*vns_ovl_vfsc_security_dentry_create_files_as)
#define posix_acl_create (*vns_ovl_vfsc_posix_acl_create)
#define security_inode_copy_up_xattr (*vns_ovl_vfsc_security_inode_copy_up_xattr)
#define security_inode_copy_up (*vns_ovl_vfsc_security_inode_copy_up)
#define do_splice_direct (*vns_ovl_vfsc_do_splice_direct)
#define d_find_any_alias (*vns_ovl_vfsc_d_find_any_alias)
#define vfs_fallocate (*vns_ovl_vfsc_vfs_fallocate)
#define kernel_tmpfile_open (*vns_ovl_vfsc_kernel_tmpfile_open)
#define vfs_copy_file_range (*vns_ovl_vfsc_vfs_copy_file_range)
#define vfs_dedupe_file_range_one (*vns_ovl_vfsc_vfs_dedupe_file_range_one)
#define vfs_clone_file_range (*vns_ovl_vfsc_vfs_clone_file_range)
#define vfs_llseek (*vns_ovl_vfsc_vfs_llseek)
#if !VNS_OVL_NEED_BACKING_FILE_FALLBACK
#define backing_file_read_iter (*vns_ovl_vfsc_backing_file_read_iter)
#define backing_file_write_iter (*vns_ovl_vfsc_backing_file_write_iter)
#define backing_file_splice_read (*vns_ovl_vfsc_backing_file_splice_read)
#define backing_file_splice_write (*vns_ovl_vfsc_backing_file_splice_write)
#define backing_file_mmap (*vns_ovl_vfsc_backing_file_mmap)
#else
#define vfs_iter_read (*vns_ovl_vfsc_vfs_iter_read)
#define vfs_iter_write (*vns_ovl_vfsc_vfs_iter_write)
#define vfs_iocb_iter_read (*vns_ovl_vfsc_vfs_iocb_iter_read)
#define vfs_iocb_iter_write (*vns_ovl_vfsc_vfs_iocb_iter_write)
#define iter_file_splice_write (*vns_ovl_vfsc_iter_file_splice_write)
#endif
#endif /* NEW */

#if VNS_OVL_TIER_MID
#define prepare_creds (*vns_ovl_vfsc_prepare_creds)
#define errseq_sample (*vns_ovl_vfsc_errseq_sample)
#define mntput (*vns_ovl_vfsc_mntput)
#define __mnt_is_readonly (*vns_ovl_vfsc___mnt_is_readonly)
#define vfs_statfs (*vns_ovl_vfsc_vfs_statfs)
#define clone_private_mount (*vns_ovl_vfsc_clone_private_mount)
#define lock_rename (*vns_ovl_vfsc_lock_rename)
#define unlock_rename (*vns_ovl_vfsc_unlock_rename)
#define take_dentry_name_snapshot (*vns_ovl_vfsc_take_dentry_name_snapshot)
#define vfs_rename (*vns_ovl_vfsc_vfs_rename)
#define lookup_one (*vns_ovl_vfsc_lookup_one)
#define release_dentry_name_snapshot (*vns_ovl_vfsc_release_dentry_name_snapshot)
#define vfs_setxattr (*vns_ovl_vfsc_vfs_setxattr)
#define vfs_removexattr (*vns_ovl_vfsc_vfs_removexattr)
#define get_anon_bdev (*vns_ovl_vfsc_get_anon_bdev)
#define kern_unmount_array (*vns_ovl_vfsc_kern_unmount_array)
#define free_anon_bdev (*vns_ovl_vfsc_free_anon_bdev)
#define kern_path (*vns_ovl_vfsc_kern_path)
#define dget_parent (*vns_ovl_vfsc_dget_parent)
#define inode_owner_or_capable (*vns_ovl_vfsc_inode_owner_or_capable)
#define exportfs_decode_fh (*vns_ovl_vfsc_exportfs_decode_fh)
#define is_subdir (*vns_ovl_vfsc_is_subdir)
#define vfs_getxattr (*vns_ovl_vfsc_vfs_getxattr)
#define lookup_one_positive_unlocked (*vns_ovl_vfsc_lookup_one_positive_unlocked)
#define lookup_one_unlocked (*vns_ovl_vfsc_lookup_one_unlocked)
#define __d_drop (*vns_ovl_vfsc___d_drop)
#define vfs_getattr (*vns_ovl_vfsc_vfs_getattr)
#define generic_fill_statx_attr (*vns_ovl_vfsc_generic_fill_statx_attr)
#define vfs_listxattr (*vns_ovl_vfsc_vfs_listxattr)
#define get_cached_acl_rcu (*vns_ovl_vfsc_get_cached_acl_rcu)
/* get_acl: not redirected (see header comment). */
#define posix_acl_clone (*vns_ovl_vfsc_posix_acl_clone)
#define vfs_set_acl_prepare (*vns_ovl_vfsc_vfs_set_acl_prepare)
#define set_posix_acl (*vns_ovl_vfsc_set_posix_acl)
#define vfs_fileattr_set (*vns_ovl_vfsc_vfs_fileattr_set)
#define vfs_fileattr_get (*vns_ovl_vfsc_vfs_fileattr_get)
#define inode_insert5 (*vns_ovl_vfsc_inode_insert5)
#define vfs_get_link (*vns_ovl_vfsc_vfs_get_link)
#define vfs_rmdir (*vns_ovl_vfsc_vfs_rmdir)
#define vfs_link (*vns_ovl_vfsc_vfs_link)
#define vfs_mknod (*vns_ovl_vfsc_vfs_mknod)
#define vfs_mkdir (*vns_ovl_vfsc_vfs_mkdir)
#define vfs_create (*vns_ovl_vfsc_vfs_create)
#define vfs_symlink (*vns_ovl_vfsc_vfs_symlink)
#define security_dentry_create_files_as (*vns_ovl_vfsc_security_dentry_create_files_as)
#define posix_acl_create (*vns_ovl_vfsc_posix_acl_create)
#define security_inode_copy_up_xattr (*vns_ovl_vfsc_security_inode_copy_up_xattr)
#define exportfs_encode_fh (*vns_ovl_vfsc_exportfs_encode_fh)
#define security_inode_copy_up (*vns_ovl_vfsc_security_inode_copy_up)
#define do_splice_direct (*vns_ovl_vfsc_do_splice_direct)
#define d_find_any_alias (*vns_ovl_vfsc_d_find_any_alias)
#define vfs_fallocate (*vns_ovl_vfsc_vfs_fallocate)
#define vfs_tmpfile_open (*vns_ovl_vfsc_vfs_tmpfile_open)
#define vfs_copy_file_range (*vns_ovl_vfsc_vfs_copy_file_range)
#define vfs_dedupe_file_range_one (*vns_ovl_vfsc_vfs_dedupe_file_range_one)
#define vfs_clone_file_range (*vns_ovl_vfsc_vfs_clone_file_range)
#define vfs_llseek (*vns_ovl_vfsc_vfs_llseek)
#define inode_permission (*vns_ovl_vfsc_inode_permission)
#define open_with_fake_path (*vns_ovl_vfsc_open_with_fake_path)
#define vfs_iter_read (*vns_ovl_vfsc_vfs_iter_read)
#define vfs_iter_write (*vns_ovl_vfsc_vfs_iter_write)
#define vfs_iocb_iter_read (*vns_ovl_vfsc_vfs_iocb_iter_read)
#define vfs_iocb_iter_write (*vns_ovl_vfsc_vfs_iocb_iter_write)
#define iter_file_splice_write (*vns_ovl_vfsc_iter_file_splice_write)
#endif /* MID */

#if VNS_OVL_TIER_OLD
#define mount_nodev (*vns_ovl_vfsc_mount_nodev)
#define prepare_creds (*vns_ovl_vfsc_prepare_creds)
#define errseq_sample (*vns_ovl_vfsc_errseq_sample)
#define mntput (*vns_ovl_vfsc_mntput)
#define vfs_statfs (*vns_ovl_vfsc_vfs_statfs)
#define clone_private_mount (*vns_ovl_vfsc_clone_private_mount)
#define lock_rename (*vns_ovl_vfsc_lock_rename)
#define unlock_rename (*vns_ovl_vfsc_unlock_rename)
#define take_dentry_name_snapshot (*vns_ovl_vfsc_take_dentry_name_snapshot)
#define vfs_rename (*vns_ovl_vfsc_vfs_rename)
#define release_dentry_name_snapshot (*vns_ovl_vfsc_release_dentry_name_snapshot)
#define vfs_setxattr (*vns_ovl_vfsc_vfs_setxattr)
#define vfs_removexattr (*vns_ovl_vfsc_vfs_removexattr)
#define get_anon_bdev (*vns_ovl_vfsc_get_anon_bdev)
#define kern_unmount_array (*vns_ovl_vfsc_kern_unmount_array)
#define free_anon_bdev (*vns_ovl_vfsc_free_anon_bdev)
#define kern_path (*vns_ovl_vfsc_kern_path)
#define dget_parent (*vns_ovl_vfsc_dget_parent)
#define inode_owner_or_capable (*vns_ovl_vfsc_inode_owner_or_capable)
#define exportfs_decode_fh (*vns_ovl_vfsc_exportfs_decode_fh)
#define is_subdir (*vns_ovl_vfsc_is_subdir)
#define vfs_getxattr (*vns_ovl_vfsc_vfs_getxattr)
#define __vfs_getxattr (*vns_ovl_vfsc___vfs_getxattr)
#define lookup_one_len_unlocked (*vns_ovl_vfsc_lookup_one_len_unlocked)
#define __d_drop (*vns_ovl_vfsc___d_drop)
#define vfs_getattr (*vns_ovl_vfsc_vfs_getattr)
#define vfs_listxattr (*vns_ovl_vfsc_vfs_listxattr)
/* get_acl: not redirected (see header comment). */
#define set_posix_acl (*vns_ovl_vfsc_set_posix_acl)
#define inode_insert5 (*vns_ovl_vfsc_inode_insert5)
#define vfs_get_link (*vns_ovl_vfsc_vfs_get_link)
#define vfs_unlink (*vns_ovl_vfsc_vfs_unlink)
#define vfs_rmdir (*vns_ovl_vfsc_vfs_rmdir)
#define vfs_link (*vns_ovl_vfsc_vfs_link)
#define vfs_mknod (*vns_ovl_vfsc_vfs_mknod)
#define vfs_mkdir (*vns_ovl_vfsc_vfs_mkdir)
#define vfs_create (*vns_ovl_vfsc_vfs_create)
#define vfs_symlink (*vns_ovl_vfsc_vfs_symlink)
#define vfs_tmpfile (*vns_ovl_vfsc_vfs_tmpfile)
#define security_dentry_create_files_as (*vns_ovl_vfsc_security_dentry_create_files_as)
#define posix_acl_create (*vns_ovl_vfsc_posix_acl_create)
#define security_inode_copy_up_xattr (*vns_ovl_vfsc_security_inode_copy_up_xattr)
#define exportfs_encode_fh (*vns_ovl_vfsc_exportfs_encode_fh)
#define security_inode_copy_up (*vns_ovl_vfsc_security_inode_copy_up)
#define do_clone_file_range (*vns_ovl_vfsc_do_clone_file_range)
#define do_splice_direct (*vns_ovl_vfsc_do_splice_direct)
#define d_find_any_alias (*vns_ovl_vfsc_d_find_any_alias)
#define d_alloc_anon (*vns_ovl_vfsc_d_alloc_anon)
#define d_instantiate_anon (*vns_ovl_vfsc_d_instantiate_anon)
#define vfs_iocb_iter_read (*vns_ovl_vfsc_vfs_iocb_iter_read)
#define vfs_iter_read (*vns_ovl_vfsc_vfs_iter_read)
#define vfs_iocb_iter_write (*vns_ovl_vfsc_vfs_iocb_iter_write)
#define vfs_iter_write (*vns_ovl_vfsc_vfs_iter_write)
#define iter_file_splice_write (*vns_ovl_vfsc_iter_file_splice_write)
#define vfs_fsync_range (*vns_ovl_vfsc_vfs_fsync_range)
#define vfs_fallocate (*vns_ovl_vfsc_vfs_fallocate)
#define dentry_open (*vns_ovl_vfsc_dentry_open)
#define open_with_fake_path (*vns_ovl_vfsc_open_with_fake_path)
#define vfs_copy_file_range (*vns_ovl_vfsc_vfs_copy_file_range)
#define vfs_dedupe_file_range_one (*vns_ovl_vfsc_vfs_dedupe_file_range_one)
#define vfs_clone_file_range (*vns_ovl_vfsc_vfs_clone_file_range)
#define d_invalidate (*vns_ovl_vfsc_d_invalidate)
#define errseq_check (*vns_ovl_vfsc_errseq_check)
#define iterate_dir (*vns_ovl_vfsc_iterate_dir)
#define lookup_positive_unlocked (*vns_ovl_vfsc_lookup_positive_unlocked)
#define override_creds (*vns_ovl_vfsc_override_creds)
#define revert_creds (*vns_ovl_vfsc_revert_creds)
#define vfs_llseek (*vns_ovl_vfsc_vfs_llseek)
#define inode_permission (*vns_ovl_vfsc_inode_permission)
#define security_file_ioctl (*vns_ovl_vfsc_security_file_ioctl)
#define vfs_fadvise (*vns_ovl_vfsc_vfs_fadvise)
#define vfs_ioctl (*vns_ovl_vfsc_vfs_ioctl)
#define vfs_setpos (*vns_ovl_vfsc_vfs_setpos)
#define down_write_killable (*vns_ovl_vfsc_down_write_killable)
#define generic_fillattr (*vns_ovl_vfsc_generic_fillattr)
#endif /* OLD */

/* ================================================================== */
/* Backport shims: let the single 6.12-shaped source spell VFS helpers */
/* that only exist on newer kernels, mapping them to the equivalent    */
/* older-kernel helper. Placed AFTER the pass-2 redirects so any name   */
/* they reference that is itself resolved (e.g. vfs_tmpfile_open) still */
/* goes through its function pointer. Each is version-gated to exactly  */
/* the kernels that lack the 6.12 spelling. Semantics were taken from   */
/* the pre-unification per-era overlayfs copies (the DDK oracle).       */
/* ================================================================== */

/* AT_GETATTR_NOSEC (6.6+): the "skip security_inode_getattr()" fast path.
 * On older kernels the flag does not exist; define it to 0 so
 * ovl_do_getattr() always takes the *secure* vfs_getattr() path. Degraded
 * but strictly safe (more security checks, never fewer) and matches how
 * pre-6.6 overlayfs always called vfs_getattr(). */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
#ifndef AT_GETATTR_NOSEC
#define AT_GETATTR_NOSEC 0
#endif
#endif

/* mnt_get_write_access()/mnt_put_write_access() are the 6.8 rename of the
 * vfsmount write-access counter helpers. Pre-6.8 overlayfs used
 * mnt_want_write()/mnt_drop_write() at these exact call sites (see the
 * pre-unification 6.1 util.c). */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 8, 0)
#define mnt_get_write_access mnt_want_write
#define mnt_put_write_access mnt_drop_write
#endif

/* super_set_uuid() (6.7+): copies a UUID into sb->s_uuid. */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 7, 0)
static inline void super_set_uuid(struct super_block *sb, const u8 *uuid,
				  unsigned int len)
{
	if (WARN_ON(len > sizeof(sb->s_uuid)))
		len = sizeof(sb->s_uuid);
	memcpy(&sb->s_uuid, uuid, len);
}
#endif

/* exportfs_can_decode_fh() (6.6+): true iff the fs can decode file handles. */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
static inline bool exportfs_can_decode_fh(const struct export_operations *nop)
{
	return nop && nop->fh_to_dentry;
}
#endif

/* generic_encode_ino32_fh() (6.9+) is the default 32-bit-ino fh encoder;
 * ovl compares sb->s_export_op->encode_fh against it to detect the default
 * encoding. Pre-6.9 the default encoder is signalled by a NULL ->encode_fh
 * (exportfs falls back to the built-in 32-bit encoding), so mapping the
 * symbol to NULL keeps the comparison (and any assignment) semantically
 * identical. */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 9, 0)
#define generic_encode_ino32_fh NULL
#endif

/* kernel_file_open() (6.6+): open a struct file from a path. Pre-6.6
 * overlayfs used dentry_open() with the same (path, flags, cred) shape. */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
#define kernel_file_open(path, flags, cred) dentry_open((path), (flags), (cred))
#endif

/* backing_file_open()/backing_tmpfile_open() (6.6+): open a real file while
 * presenting the overlay's own (fake) path to the VFS/LSM layer instead of
 * the real (underlying) one. Pre-6.6 overlayfs achieved the same effect for
 * regular opens via open_with_fake_path() (see the pre-unification 6.1
 * file.c's ovl_open_realfile()); it had no O_TMPFILE-via-backing-file
 * support at all (O_TMPFILE creation through a dedicated realfile helper is
 * itself a >=6.6 addition), so there is no faithful pre-6.6 fake-path
 * equivalent for the tmpfile case. Degraded but safe: fall back to
 * vfs_tmpfile_open(), which opens the tmpfile against its *real* path
 * instead of the overlay's; the resulting file->f_path points at the
 * upper/real dentry rather than the overlay dentry on these kernels. */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
static inline struct file *backing_file_open(const struct path *user_path, int flags,
					     const struct path *real_path,
					     const struct cred *cred)
{
	return open_with_fake_path(user_path, flags, d_inode(real_path->dentry), cred);
}
static inline struct file *backing_tmpfile_open(const struct path *user_path, int flags,
						const struct path *real_parentpath,
						umode_t mode, const struct cred *cred)
{
	return vfs_tmpfile_open(ovl_mnt_idmap(real_parentpath->mnt), real_parentpath,
				mode, flags, cred);
}
#endif

/* d_mark_tmpfile() (6.6+) is the 6.12 source's spelling of the older
 * d_tmpfile(); both take the same (struct file *, struct inode *) pair. */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
#define d_mark_tmpfile d_tmpfile
#endif

/* WRAP_DIR_ITER()/wrap_directory_iterator() (6.5+): generates a
 * shared_<fn>() wrapper that forces exclusive inode access around an
 * ->iterate_shared callback that (like overlayfs's) is not actually safe to
 * call concurrently. Pre-6.5 overlayfs registered its iterate function as
 * ->iterate_shared directly, with no such wrapper -- because the underlying
 * VFS locking change wrap_directory_iterator() compensates for is itself
 * part of the same 6.5 series. So the faithful pre-6.5 equivalent is simply
 * to call the wrapped function straight through, unwrapped. */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 5, 0)
#define WRAP_DIR_ITER(x) \
	static int shared_##x(struct file *file, struct dir_context *ctx) \
	{ return x(file, ctx); }
#endif

/* fsparam_string_empty() (6.6+): a string mount option that also accepts the
 * empty value ("opt="). Byte-identical to the upstream 6.6 definition, built
 * from the __fsparam()/fs_param_can_be_empty primitives present since 5.x. */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
#include <linux/fs_parser.h>
#ifndef fsparam_string_empty
#define fsparam_string_empty(NAME, OPT) \
	__fsparam(fs_param_is_string, NAME, OPT, fs_param_can_be_empty, NULL)
#endif
#endif

/* super_block::s_iflags bits used by the 6.12 source that were added later:
 *   SB_I_NOUMASK              (6.7) - fs applies POSIX ACL default mode itself
 *   SB_I_EVM_HMAC_UNSUPPORTED (6.9) - EVM should not compute an HMAC here
 * On older kernels these flags simply do not exist; define them to 0 so the
 * "sb->s_iflags |= ..." statements are no-ops. Degraded but safe: on <6.7 the
 * VFS applied umask the classic way (matching pre-6.7 overlayfs), and the EVM
 * hint is advisory. */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 7, 0)
#ifndef SB_I_NOUMASK
#define SB_I_NOUMASK 0
#endif
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 9, 0)
#ifndef SB_I_EVM_HMAC_UNSUPPORTED
#define SB_I_EVM_HMAC_UNSUPPORTED 0
#endif
#endif

/* fsverity_get_digest() gained a size-returning 4-argument form (u8 *alg
 * holding an FS_VERITY_HASH_ALG_* constant, plus struct fsverity_digest **out)
 * around 6.9. Older kernels only have int fsverity_get_digest(inode, digest,
 * enum hash_algo *) that returns 0/-errno and reports a *crypto* hash_algo,
 * not the FS_VERITY_HASH_ALG_* constant the 6.12 call sites compare against,
 * and no size. A faithful translation is impossible, so on <6.9 we report
 * "no digest" (return 0). Callers treat that as absent: OVL_VERITY_REQUIRE
 * fails closed (-EIO) and other modes skip -- never a false accept. This
 * matches pre-6.1 overlayfs, which had no verity-metacopy support at all. */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 9, 0)
static inline int vns_ovl_fsverity_get_digest(struct inode *inode, u8 *digest,
					      u8 *alg, void *out)
{
	if (alg)
		*alg = 0;
	return 0;
}
#define fsverity_get_digest(inode, digest, alg, out) \
	vns_ovl_fsverity_get_digest((inode), (digest), (alg), (out))
#endif

#if VNS_OVL_TIER_MID || VNS_OVL_TIER_OLD
/* struct renamedata field rename (6.3): old_mnt_idmap/new_mnt_idmap were
 * old_mnt_userns/new_mnt_userns on [5.12, 6.3). Plain identifier tokens used
 * only in renamedata initialisers, safe to rewrite. (On OLD/5.10 renamedata
 * does not exist at all -- ovl_do_rename is handled separately there.) */
#if VNS_OVL_TIER_MID
#define old_mnt_idmap old_mnt_userns
#define new_mnt_idmap new_mnt_userns
#endif

/* kernel_tmpfile_open() (6.6+) -> vfs_tmpfile_open() ([6.1, 6.6)). Same
 * (idmap, parentpath, mode, open_flag, cred) shape; on MID `idmap` is the
 * aliased struct user_namespace *. */
#if VNS_OVL_TIER_MID
static inline struct file *kernel_tmpfile_open(struct user_namespace *idmap,
					       const struct path *parentpath,
					       umode_t mode, int open_flag,
					       const struct cred *cred)
{
	return vfs_tmpfile_open(idmap, parentpath, mode, open_flag, cred);
}
#endif
#endif /* MID || OLD */

/* vfs_set_acl()/vfs_remove_acl() (name-based, 6.2+) do not exist on older
 * kernels. Map them to set_posix_acl(), which pre-unification overlayfs used
 * for the same effect. Degraded: skips the LSM security_inode_set_acl() hook
 * that 6.2+ vfs_set_acl() runs -- acceptable for overlay-internal ACL copy-up,
 * mirroring how overlay xattr copy-up already bypasses LSM set hooks for its
 * own book-keeping xattrs. On MID set_posix_acl() takes the (aliased) idmap;
 * on OLD (5.10) it predates idmapped mounts and takes none. */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 2, 0)
#include <linux/posix_acl.h>
#include <linux/posix_acl_xattr.h>
#include <uapi/linux/xattr.h>
static inline int vns_ovl_acl_type_by_name(const char *acl_name)
{
	if (!strcmp(acl_name, XATTR_NAME_POSIX_ACL_ACCESS))
		return ACL_TYPE_ACCESS;
	if (!strcmp(acl_name, XATTR_NAME_POSIX_ACL_DEFAULT))
		return ACL_TYPE_DEFAULT;
	return -1;
}
static inline int vfs_set_acl(struct mnt_idmap *idmap, struct dentry *dentry,
			      const char *acl_name, struct posix_acl *acl)
{
	int type = vns_ovl_acl_type_by_name(acl_name);

	if (type < 0)
		return -EOPNOTSUPP;
#if VNS_OVL_TIER_OLD
	return set_posix_acl(d_inode(dentry), type, acl);
#else
	return set_posix_acl(idmap, d_inode(dentry), type, acl);
#endif
}
static inline int vfs_remove_acl(struct mnt_idmap *idmap, struct dentry *dentry,
				 const char *acl_name)
{
	int type = vns_ovl_acl_type_by_name(acl_name);

	if (type < 0)
		return -EOPNOTSUPP;
#if VNS_OVL_TIER_OLD
	return set_posix_acl(d_inode(dentry), type, NULL);
#else
	return set_posix_acl(idmap, d_inode(dentry), type, NULL);
#endif
}

/* Same 6.2 boundary on the get side: get_acl() (inode-op-cache helper) was
 * renamed get_inode_acl() and gained a name-based vfs_get_acl() wrapper, and
 * posix_acl_type()/posix_acl_xattr_name() (type<->xattr-name conversion) were
 * introduced alongside. Provide all four in terms of the pre-6.2 get_acl().
 * Degraded: skips whatever LSM/security hook 6.2+ vfs_get_acl() may run --
 * same trade-off already accepted for vfs_set_acl()/vfs_remove_acl() above. */
static inline int posix_acl_type(const char *acl_name)
{
	int type = vns_ovl_acl_type_by_name(acl_name);

	return type < 0 ? ACL_TYPE_ACCESS : type;
}
static inline const char *posix_acl_xattr_name(int type)
{
	return (type == ACL_TYPE_DEFAULT) ? XATTR_NAME_POSIX_ACL_DEFAULT
					   : XATTR_NAME_POSIX_ACL_ACCESS;
}
static inline struct posix_acl *get_inode_acl(struct inode *inode, int type)
{
	return get_acl(inode, type);
}
static inline struct posix_acl *vfs_get_acl(struct mnt_idmap *idmap,
					    struct dentry *dentry,
					    const char *acl_name)
{
	int type = vns_ovl_acl_type_by_name(acl_name);

	if (type < 0)
		return ERR_PTR(-EOPNOTSUPP);
	return get_acl(d_inode(dentry), type);
}
/* is_posix_acl_xattr() (6.2+) checks whether a xattr name is one of the two
 * POSIX ACL names. Trivial, ABI-stable comparison; reimplement directly
 * rather than resolve a kernel symbol (there isn't one to resolve pre-6.2). */
static inline bool is_posix_acl_xattr(const char *name)
{
	return !strcmp(name, XATTR_NAME_POSIX_ACL_ACCESS) ||
	       !strcmp(name, XATTR_NAME_POSIX_ACL_DEFAULT);
}
#endif /* < 6.2 */

/* security_inode_copy_up_xattr() gained a leading `struct dentry *src`
 * parameter in 6.12 (the source dentry of the copy-up, used by newer LSM
 * hooks). Pre-6.12 kernels only take the xattr `name`. Wrap the resolved
 * (pre-6.12-shaped) function pointer in a 2-arg shim that drops `src`;
 * degraded only for LSMs that would have used `src` (none of the in-tree
 * hooks on these kernels do -- the parameter is purely additive in 6.12). */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 12, 0)
static inline int vns_ovl_security_inode_copy_up_xattr(struct dentry *src,
							const char *name)
{
	return security_inode_copy_up_xattr(name);
}
#undef security_inode_copy_up_xattr
#define security_inode_copy_up_xattr(src, name) \
	vns_ovl_security_inode_copy_up_xattr((src), (name))
#endif

#if VNS_OVL_TIER_MID || VNS_OVL_TIER_OLD
/* exportfs_encode_inode_fh() gained a trailing `int flags` parameter in 6.6
 * (EXPORT_FH_* bits); MID/OLD kernels only resolve the older dentry-based
 * exportfs_encode_fh(dentry, fid, max_len, connectable) (see
 * VNS_OVL_VFS_COMPAT_LIST above). Bridge the inode-based 6.12 call by
 * looking up any dentry alias for the inode. `flags` is dropped: every
 * ovl_encode_real_fh() call site in this source passes flags=0 (plain,
 * non-FID-only encoding) with parent either NULL or set, i.e. exactly what
 * `connectable = !!parent` already reproduces via exportfs_encode_fh(). */
static inline int vns_ovl_exportfs_encode_inode_fh(struct inode *inode, struct fid *fid,
						   int *max_len, struct inode *parent,
						   int flags)
{
	struct dentry *dentry = d_find_any_alias(inode);
	int err;

	if (!dentry)
		return FILEID_INVALID;
	err = exportfs_encode_fh(dentry, fid, max_len, !!parent);
	dput(dentry);
	return err;
}
#define exportfs_encode_inode_fh(inode, fid, max_len, parent, flags) \
	vns_ovl_exportfs_encode_inode_fh((inode), (fid), (max_len), (parent), (flags))
#endif /* MID || OLD */

#endif /* !VNS_OVL_VFS_COMPAT_IMPL */

int vns_ovl_vfs_compat_resolve(void);

#endif /* LINUX_VERSION_CODE in [5.10, 6.19) */

#endif /* _VNS_OVL_VFS_COMPAT_H */
