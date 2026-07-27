/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * vendor_kernel_ovl_vfs_compat.h - lkm4ctr [BUILD-COMPAT]: resolve VFS-internal helpers
 * this vendored overlayfs needs that are not (or not reliably) present in
 * the running kernel's module symbol table.
 *
 * This is NEW code (not vendored from kernel-common).
 *
 * Why this exists: every one of the symbols listed in VNS_OVL_VFS_COMPAT_LIST()
 * below is a real, ordinary VFS/security/exportfs function -- declared as a
 * normal extern prototype in the running kernel's own public headers
 * (<linux/fs.h>, <linux/namei.h>, <linux/xattr.h>, <linux/exportfs.h>,
 * <linux/dcache.h>, <linux/posix_acl.h>, <linux/security.h>, ...) and
 * present in the running kernel's binary (visible in /proc/kallsyms) -- but
 * NOT necessarily EXPORT_SYMBOL()'d for out-of-tree module use. Many
 * production/GKI-certified Android kernels build with
 * CONFIG_TRIM_UNUSED_KSYMS enabled and a protected KMI export allow-list
 * that only covers the (small) set of symbols vendor DLKMs are declared to
 * need; upstream in-tree overlayfs itself never needs these to be
 * EXPORT_SYMBOL()'d (it is built directly into vmlinux, not as a module),
 * so they are routinely trimmed away even though the functions themselves
 * are compiled in and perfectly callable. Linking directly against them (as
 * upstream overlayfs.ko itself does, since it never has to survive
 * out-of-tree loading) makes modpost/the module loader's
 * simplify_symbols() fail insmod outright with "Unknown symbol %s (err -2)"
 * for every one of them, on any kernel where they were trimmed -- this is
 * exactly the failure mode this header exists to avoid.
 *
 * Fix: resolve each of these by name at module load time via
 * shadow_hook_resolve() (see common/shadow_hook.h; the same
 * register_kprobe()-based kallsyms lookup already used elsewhere in
 * vendor_kernel, e.g. glue/vendor_kernel_module.c's vns_proc_alloc_inum_fn,
 * glue/vendor_kernel_ipc_mount.c's kern_path_fn, ...), which finds a
 * function by name directly through kallsyms and does not care whether it
 * is exported, GPL-only, or namespaced. Almost every call site in this
 * vendored overlayfs is left completely unmodified: VNS_OVL_VFS_COMPAT_LIST()
 * below is expanded twice -- once (while each name still refers to the real
 * kernel declaration) to declare a same-signature `extern typeof(name) *`
 * function pointer via typeof(), and again afterwards to #define the bare
 * name to `(*vns_ovl_vfsc_<name>)` -- so every existing call to e.g.
 * mount_nodev(...) transparently goes through the resolved pointer instead,
 * without touching that call site in super.c/dir.c/inode.c/... . Using
 * typeof() instead of hand-typed prototypes also means the pointer's
 * signature is always exactly whatever this specific kernel's own headers
 * say it is, with zero risk of a transcription mismatch.
 *
 * Three symbols are handled differently and are NOT blanket-#define'd here:
 *   - generic_delete_inode() and noop_direct_IO() are both single-line,
 *     ABI-stable helpers (`return 1;` and `return -EINVAL;` respectively,
 *     see fs/inode.c and fs/libfs.c) that are only ever used as
 *     `struct inode_operations`/`struct file_operations` initializer values
 *     (e.g. `.drop_inode = generic_delete_inode,`), which must be
 *     compile-time constants -- a runtime-resolved function pointer can't
 *     be used there. super.c/inode.c instead define tiny local equivalents
 *     (ovl_generic_delete_inode()/ovl_noop_direct_IO()) and reference those.
 *   - get_acl() is resolved like everything else (its logic is not
 *     trivial), but is deliberately NOT #define'd to a bare name here: this
 *     kernel's own `struct inode_operations.get_acl` field is named
 *     identically, and `#define get_acl (*vns_ovl_vfsc_get_acl)` would also
 *     rewrite every `.get_acl = ovl_get_acl,` initializer into invalid
 *     syntax. Its one real call site (inode.c) calls vns_ovl_vfsc_get_acl()
 *     directly instead.
 *
 * vns_ovl_vfs_compat_resolve() (vendor_kernel_ovl_vfs_compat.c) is called once from
 * vns_ovl_init() (super.c) before the vendored overlayfs is ever used; if
 * any symbol fails to resolve, vns_ovl_init() fails cleanly (propagated up
 * through vendor_kernel_overlay.c/vendor_kernel_init()) instead of leaving
 * a NULL function pointer that would crash on first use.
 */
#ifndef _VNS_OVL_VFS_COMPAT_H
#define _VNS_OVL_VFS_COMPAT_H

#define VNS_OVL_VFS_COMPAT_LIST(X) \
	X(mount_nodev) \
	X(prepare_creds) \
	X(errseq_sample) \
	X(mntput) \
	X(__mnt_is_readonly) \
	X(vfs_statfs) \
	X(clone_private_mount) \
	X(lock_rename) \
	X(unlock_rename) \
	X(vfs_tmpfile_open) \
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
	X(vfs_set_acl_prepare) \
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
	X(do_clone_file_range) \
	X(do_splice_direct) \
	X(d_find_any_alias) \
	X(d_alloc_anon) \
	X(d_instantiate_anon) \
	X(vfs_iocb_iter_read) \
	X(vfs_iter_read) \
	X(vfs_iocb_iter_write) \
	X(vfs_iter_write) \
	X(vfs_fallocate) \
	X(open_with_fake_path) \
	X(vfs_copy_file_range) \
	X(vfs_dedupe_file_range_one) \
	X(vfs_clone_file_range)

/*
 * Pass 1: declare the resolved function pointers while every name in
 * VNS_OVL_VFS_COMPAT_LIST() still refers to the real kernel declaration (must
 * come strictly before the #define pass below).
 */
#define VNS_OVL_VFSC_DECLARE(name) extern typeof(name) *vns_ovl_vfsc_##name;
VNS_OVL_VFS_COMPAT_LIST(VNS_OVL_VFSC_DECLARE)
#undef VNS_OVL_VFSC_DECLARE

/*
 * Pass 2: redirect every bare use of each name (call expression or
 * function-pointer value, e.g. `.direct_IO = noop_direct_IO`) to go through
 * the resolved pointer instead.
 */
#define mount_nodev (*vns_ovl_vfsc_mount_nodev)
#define prepare_creds (*vns_ovl_vfsc_prepare_creds)
#define errseq_sample (*vns_ovl_vfsc_errseq_sample)
#define mntput (*vns_ovl_vfsc_mntput)
#define __mnt_is_readonly (*vns_ovl_vfsc___mnt_is_readonly)
#define vfs_statfs (*vns_ovl_vfsc_vfs_statfs)
#define clone_private_mount (*vns_ovl_vfsc_clone_private_mount)
#define lock_rename (*vns_ovl_vfsc_lock_rename)
#define unlock_rename (*vns_ovl_vfsc_unlock_rename)
#define vfs_tmpfile_open (*vns_ovl_vfsc_vfs_tmpfile_open)
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
#define vfs_set_acl_prepare (*vns_ovl_vfsc_vfs_set_acl_prepare)
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
#define posix_acl_clone (*vns_ovl_vfsc_posix_acl_clone)
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
#define do_clone_file_range (*vns_ovl_vfsc_do_clone_file_range)
#define do_splice_direct (*vns_ovl_vfsc_do_splice_direct)
#define d_find_any_alias (*vns_ovl_vfsc_d_find_any_alias)
#define d_alloc_anon (*vns_ovl_vfsc_d_alloc_anon)
#define d_instantiate_anon (*vns_ovl_vfsc_d_instantiate_anon)
#define vfs_iocb_iter_read (*vns_ovl_vfsc_vfs_iocb_iter_read)
#define vfs_iter_read (*vns_ovl_vfsc_vfs_iter_read)
#define vfs_iocb_iter_write (*vns_ovl_vfsc_vfs_iocb_iter_write)
#define vfs_iter_write (*vns_ovl_vfsc_vfs_iter_write)
#define vfs_fallocate (*vns_ovl_vfsc_vfs_fallocate)
#define open_with_fake_path (*vns_ovl_vfsc_open_with_fake_path)
#define vfs_copy_file_range (*vns_ovl_vfsc_vfs_copy_file_range)
#define vfs_dedupe_file_range_one (*vns_ovl_vfsc_vfs_dedupe_file_range_one)
#define vfs_clone_file_range (*vns_ovl_vfsc_vfs_clone_file_range)

/*
 * vns_ovl_vfs_compat_resolve() - resolve every symbol in VNS_OVL_VFS_COMPAT_LIST()
 * via shadow_hook_resolve(). Returns 0 on success, -ENOENT if any symbol
 * could not be found (logging which one via LKM4CTR_ERR).
 */
int vns_ovl_vfs_compat_resolve(void);

#endif /* _VNS_OVL_VFS_COMPAT_H */
