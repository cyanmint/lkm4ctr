/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * vendor_kernel_ovl_vfs_compat_5_10.h - lkm4ctr [BUILD-COMPAT]: resolve the
 * VFS-internal helpers the [5.10, 5.15) vendored overlayfs snapshot needs but
 * that are not (or not reliably) present in the running kernel's module symbol
 * table.
 *
 * This is NEW code (not vendored from kernel-common).
 *
 * Why this exists: every symbol in VNS_OVL_VFS_COMPAT_5_10_LIST() below is a
 * real VFS/security/exportfs helper declared through the running kernel's own
 * public headers (<linux/fs.h>, <linux/namei.h>, <linux/xattr.h>,
 * <linux/exportfs.h>, <linux/posix_acl.h>, <linux/security.h>, ...), but on
 * Android GKI kernels they may still be trimmed from the module symbol table by
 * CONFIG_TRIM_UNUSED_KSYMS / protected-KMI export allow-lists. Linking this
 * out-of-tree vendored overlayfs directly against them therefore makes insmod
 * fail with "Unknown symbol %s (err -2)" even though the functions themselves
 * are compiled into vmlinux and visible in kallsyms.
 *
 * Fix: resolve each helper by exact name at module-load time via
 * shadow_hook_resolve() (the same register_kprobe()-based kallsyms lookup used
 * elsewhere in vendor_kernel), then transparently redirect existing call sites
 * through same-signature function pointers declared with typeof(). Most of the
 * vendored sources stay untouched: pass 1 declares
 * `extern typeof(name) *vns_ovl_vfsc_5_10_<name>;` while `name` still refers to
 * the real kernel declaration, and pass 2 #define's the bare name to
 * `(*vns_ovl_vfsc_5_10_<name>)` so ordinary call expressions and
 * function-pointer values keep compiling unchanged.
 *
 * A few helpers are intentionally handled differently:
 *   - generic_delete_inode() and noop_direct_IO() are tiny, ABI-stable helpers
 *     used only as constant initializer values. super.c/inode.c provide local
 *     one-line equivalents instead of trying to use runtime-resolved pointers
 *     in those initializer slots.
 *   - get_acl() is resolved here but deliberately not blanket-#define'd, since
 *     this kernel's struct inode_operations also has a field named get_acl and
 *     a macro alias would corrupt `.get_acl = ...` initializers. inode.c calls
 *     vns_ovl_vfsc_5_10_get_acl() directly at its one real call site.
 *   - vfs_whiteout() is not resolved here: on this 5.10 API era it is an
 *     inline helper whose body would still call the real vfs_mknod() symbol
 *     before these macro redirects take effect. overlayfs.h therefore
 *     reimplements that one-line body directly through the already-resolved
 *     vfs_mknod().
 */
#ifndef _VNS_OVL_VFS_COMPAT_5_10_H
#define _VNS_OVL_VFS_COMPAT_5_10_H

#include <linux/version.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0) && LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 0)

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

/*
 * Pass 1: declare the resolved function pointers while every name in
 * VNS_OVL_VFS_COMPAT_5_10_LIST() still refers to the real kernel declaration
 * (must come strictly before the #define pass below).
 */
#define VNS_OVL_VFSC_5_10_DECLARE(name) extern typeof(name) *vns_ovl_vfsc_5_10_##name;
VNS_OVL_VFS_COMPAT_5_10_LIST(VNS_OVL_VFSC_5_10_DECLARE)
#undef VNS_OVL_VFSC_5_10_DECLARE
#define down_write_killable (*vns_ovl_vfsc_5_10_down_write_killable)
#define d_invalidate (*vns_ovl_vfsc_5_10_d_invalidate)
#define errseq_check (*vns_ovl_vfsc_5_10_errseq_check)
#define iterate_dir (*vns_ovl_vfsc_5_10_iterate_dir)
#define lookup_positive_unlocked (*vns_ovl_vfsc_5_10_lookup_positive_unlocked)
#define override_creds (*vns_ovl_vfsc_5_10_override_creds)
#define revert_creds (*vns_ovl_vfsc_5_10_revert_creds)
#define security_file_ioctl (*vns_ovl_vfsc_5_10_security_file_ioctl)
#define vfs_fadvise (*vns_ovl_vfsc_5_10_vfs_fadvise)
#define vfs_ioctl (*vns_ovl_vfsc_5_10_vfs_ioctl)
#define vfs_setpos (*vns_ovl_vfsc_5_10_vfs_setpos)

/*
 * Pass 2: redirect every bare use of each name (call expression or
 * function-pointer value) to go through the resolved pointer instead.
 */
#define mount_nodev (*vns_ovl_vfsc_5_10_mount_nodev)
#define prepare_creds (*vns_ovl_vfsc_5_10_prepare_creds)
#define errseq_sample (*vns_ovl_vfsc_5_10_errseq_sample)
#define mntput (*vns_ovl_vfsc_5_10_mntput)
#define vfs_statfs (*vns_ovl_vfsc_5_10_vfs_statfs)
#define clone_private_mount (*vns_ovl_vfsc_5_10_clone_private_mount)
#define lock_rename (*vns_ovl_vfsc_5_10_lock_rename)
#define unlock_rename (*vns_ovl_vfsc_5_10_unlock_rename)
#define take_dentry_name_snapshot (*vns_ovl_vfsc_5_10_take_dentry_name_snapshot)
#define vfs_rename (*vns_ovl_vfsc_5_10_vfs_rename)
#define release_dentry_name_snapshot (*vns_ovl_vfsc_5_10_release_dentry_name_snapshot)
#define vfs_setxattr (*vns_ovl_vfsc_5_10_vfs_setxattr)
#define vfs_removexattr (*vns_ovl_vfsc_5_10_vfs_removexattr)
#define get_anon_bdev (*vns_ovl_vfsc_5_10_get_anon_bdev)
#define kern_unmount_array (*vns_ovl_vfsc_5_10_kern_unmount_array)
#define free_anon_bdev (*vns_ovl_vfsc_5_10_free_anon_bdev)
#define kern_path (*vns_ovl_vfsc_5_10_kern_path)
#define dget_parent (*vns_ovl_vfsc_5_10_dget_parent)
#define inode_owner_or_capable (*vns_ovl_vfsc_5_10_inode_owner_or_capable)
#define exportfs_decode_fh (*vns_ovl_vfsc_5_10_exportfs_decode_fh)
#define is_subdir (*vns_ovl_vfsc_5_10_is_subdir)
#define vfs_getxattr (*vns_ovl_vfsc_5_10_vfs_getxattr)
#define __vfs_getxattr (*vns_ovl_vfsc_5_10___vfs_getxattr)
#define lookup_one_len_unlocked (*vns_ovl_vfsc_5_10_lookup_one_len_unlocked)
#define __d_drop (*vns_ovl_vfsc_5_10___d_drop)
#define vfs_getattr (*vns_ovl_vfsc_5_10_vfs_getattr)
#define vfs_listxattr (*vns_ovl_vfsc_5_10_vfs_listxattr)
#define inode_insert5 (*vns_ovl_vfsc_5_10_inode_insert5)
#define vfs_get_link (*vns_ovl_vfsc_5_10_vfs_get_link)
#define vfs_unlink (*vns_ovl_vfsc_5_10_vfs_unlink)
#define vfs_rmdir (*vns_ovl_vfsc_5_10_vfs_rmdir)
#define vfs_link (*vns_ovl_vfsc_5_10_vfs_link)
#define vfs_mknod (*vns_ovl_vfsc_5_10_vfs_mknod)
#define vfs_mkdir (*vns_ovl_vfsc_5_10_vfs_mkdir)
#define vfs_create (*vns_ovl_vfsc_5_10_vfs_create)
#define vfs_symlink (*vns_ovl_vfsc_5_10_vfs_symlink)
#define vfs_tmpfile (*vns_ovl_vfsc_5_10_vfs_tmpfile)
#define security_dentry_create_files_as (*vns_ovl_vfsc_5_10_security_dentry_create_files_as)
#define posix_acl_create (*vns_ovl_vfsc_5_10_posix_acl_create)
#define security_inode_copy_up_xattr (*vns_ovl_vfsc_5_10_security_inode_copy_up_xattr)
#define exportfs_encode_fh (*vns_ovl_vfsc_5_10_exportfs_encode_fh)
#define security_inode_copy_up (*vns_ovl_vfsc_5_10_security_inode_copy_up)
#define do_clone_file_range (*vns_ovl_vfsc_5_10_do_clone_file_range)
#define do_splice_direct (*vns_ovl_vfsc_5_10_do_splice_direct)
#define d_find_any_alias (*vns_ovl_vfsc_5_10_d_find_any_alias)
#define d_alloc_anon (*vns_ovl_vfsc_5_10_d_alloc_anon)
#define d_instantiate_anon (*vns_ovl_vfsc_5_10_d_instantiate_anon)
#define vfs_iocb_iter_read (*vns_ovl_vfsc_5_10_vfs_iocb_iter_read)
#define vfs_iter_read (*vns_ovl_vfsc_5_10_vfs_iter_read)
#define vfs_iocb_iter_write (*vns_ovl_vfsc_5_10_vfs_iocb_iter_write)
#define vfs_iter_write (*vns_ovl_vfsc_5_10_vfs_iter_write)
#define iter_file_splice_write (*vns_ovl_vfsc_5_10_iter_file_splice_write)
#define vfs_fsync_range (*vns_ovl_vfsc_5_10_vfs_fsync_range)
#define vfs_fallocate (*vns_ovl_vfsc_5_10_vfs_fallocate)
#define dentry_open (*vns_ovl_vfsc_5_10_dentry_open)
#define open_with_fake_path (*vns_ovl_vfsc_5_10_open_with_fake_path)
#define vfs_copy_file_range (*vns_ovl_vfsc_5_10_vfs_copy_file_range)
#define vfs_dedupe_file_range_one (*vns_ovl_vfsc_5_10_vfs_dedupe_file_range_one)
#define vfs_clone_file_range (*vns_ovl_vfsc_5_10_vfs_clone_file_range)

/*
 * vns_ovl_vfs_compat_5_10_resolve() - resolve every symbol in
 * VNS_OVL_VFS_COMPAT_5_10_LIST() via shadow_hook_resolve(). Returns 0 on
 * success, -ENOENT if any symbol could not be found (logging which one via
 * LKM4CTR_ERR).
 */
int vns_ovl_vfs_compat_5_10_resolve(void);

#endif /* LINUX_VERSION_CODE in [5.10, 5.15) */

#endif /* _VNS_OVL_VFS_COMPAT_5_10_H */
