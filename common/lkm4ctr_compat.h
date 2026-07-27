/* SPDX-License-Identifier: GPL-2.0 */
/*
 * lkm4ctr_compat - small cross-kernel-version compatibility shims shared
 * by the unified lkm4ctr.ko subsystems.
 *
 * Historically these shims lived in lkm4ctr_internal.h, back when every
 * subsystem was linked into a single combined lkm4ctr.ko. They now live
 * here and are pulled in (via -I../common) by whichever unified-module
 * subsystem actually uses them.
 *
 * fd_file()/fd_empty() were introduced by the "struct fd" API rework
 * (upstream commit "file: convert to struct fd") that landed in v6.8; on the
 * older GKI branches (e.g. 6.1) these modules still target, "struct fd" is a
 * plain aggregate with a directly accessible ->file member, so provide
 * compatible shims when the helpers aren't present. Used by vendor_kernel's
 * procfs and mqueue glue.
 */

#ifndef _LKM4CTR_COMPAT_H
#define _LKM4CTR_COMPAT_H

#include <linux/file.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/namei.h>
#include <linux/version.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
#include <linux/mnt_idmapping.h>
#endif

#ifndef fd_file
#define fd_file(f) ((f).file)
#endif
#ifndef fd_empty
#define fd_empty(f) (!fd_file(f))
#endif

/*
 * lkm4ctr_inode_init_ts() - set a freshly allocated inode's
 * atime/mtime/ctime to "now", across the API rework simple_inode_init_ts()
 * introduced upstream in v6.6 (older kernels this module targets, down to
 * 5.10, still expose the plain i_atime/i_mtime/i_ctime struct timespec64
 * fields directly). Used by lkm4ctr_diagfs.c's inode allocation helper.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
#define lkm4ctr_inode_init_ts(inode) simple_inode_init_ts(inode)
#define lkm4ctr_inode_update_ts(inode)					\
	do {								\
		struct timespec64 _ts = current_time(inode);		\
		inode_set_atime_to_ts((inode), _ts);			\
		inode_set_mtime_to_ts((inode), _ts);			\
		inode_set_ctime_to_ts((inode), _ts);			\
	} while (0)
#else
#define lkm4ctr_inode_init_ts(inode) \
	((inode)->i_atime = (inode)->i_mtime = (inode)->i_ctime = current_time(inode))
#define lkm4ctr_inode_update_ts(inode) lkm4ctr_inode_init_ts(inode)
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 17, 0)
static inline int lkm4ctr_inode_permission(struct inode *inode, int mask)
{
	return inode_permission(&nop_mnt_idmap, inode, mask);
}

static inline int lkm4ctr_vfs_unlink(struct inode *dir, struct dentry *dentry,
				     struct inode **delegated_inode)
{
	return vfs_unlink(&nop_mnt_idmap, dir, dentry, delegated_inode);
}

static inline struct dentry *lkm4ctr_lookup_one_len(const char *name,
						    struct dentry *base, int len)
{
	struct qstr qstr = QSTR_INIT(name, len);

	return lookup_one(&nop_mnt_idmap, &qstr, base);
}
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
static inline int lkm4ctr_inode_permission(struct inode *inode, int mask)
{
	return inode_permission(&nop_mnt_idmap, inode, mask);
}

static inline int lkm4ctr_vfs_unlink(struct inode *dir, struct dentry *dentry,
				     struct inode **delegated_inode)
{
	return vfs_unlink(&nop_mnt_idmap, dir, dentry, delegated_inode);
}

static inline struct dentry *lkm4ctr_lookup_one_len(const char *name,
						    struct dentry *base, int len)
{
	return lookup_one(&nop_mnt_idmap, name, base, len);
}
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 12, 0)
static inline int lkm4ctr_inode_permission(struct inode *inode, int mask)
{
	return inode_permission(&init_user_ns, inode, mask);
}

static inline int lkm4ctr_vfs_unlink(struct inode *dir, struct dentry *dentry,
				     struct inode **delegated_inode)
{
	return vfs_unlink(&init_user_ns, dir, dentry, delegated_inode);
}

static inline struct dentry *lkm4ctr_lookup_one_len(const char *name,
						    struct dentry *base, int len)
{
	return lookup_one_len(name, base, len);
}
#else
static inline int lkm4ctr_inode_permission(struct inode *inode, int mask)
{
	return inode_permission(inode, mask);
}

static inline int lkm4ctr_vfs_unlink(struct inode *dir, struct dentry *dentry,
				     struct inode **delegated_inode)
{
	return vfs_unlink(dir, dentry, delegated_inode);
}

static inline struct dentry *lkm4ctr_lookup_one_len(const char *name,
						    struct dentry *base, int len)
{
	return lookup_one_len(name, base, len);
}
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 17, 0)
static inline int lkm4ctr_vfs_mmap(struct file *file, struct vm_area_struct *vma)
{
	return vfs_mmap(file, vma);
}
#else
static inline int lkm4ctr_vfs_mmap(struct file *file, struct vm_area_struct *vma)
{
	return call_mmap(file, vma);
}
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
static inline unsigned long lkm4ctr_do_mmap(struct file *file, unsigned long addr,
					    unsigned long len, unsigned long prot,
					    unsigned long flags, unsigned long pgoff,
					    unsigned long *populate,
					    struct list_head *uf)
{
	return do_mmap(file, addr, len, prot, flags, 0, pgoff, populate, uf);
}
#else
static inline unsigned long lkm4ctr_do_mmap(struct file *file, unsigned long addr,
					    unsigned long len, unsigned long prot,
					    unsigned long flags, unsigned long pgoff,
					    unsigned long *populate,
					    struct list_head *uf)
{
	return do_mmap(file, addr, len, prot, flags, pgoff, populate, uf);
}
#endif

#endif /* _LKM4CTR_COMPAT_H */
