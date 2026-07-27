// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2017 Red Hat, Inc.
 */
#include <linux/version.h>

/*
 * lkm4ctr [BUILD-COMPAT]: this vendored overlayfs source was taken from android16-6.12
 * (kernel 6.12.x): same fs_context-based file_system_type and
 * struct mnt_idmap * idmap arguments as the 6.6 era, plus
 * xattrs.c (extracted xattr handling) and further ACL/export-op
 * churn specific to 6.12.
 * On kernels outside this range the calls below do not match the
 * running kernel's VFS/fs_context prototypes at all. Rather than risk
 * incorrect behavior from an unverifiable cross-version port, this
 * file's content is excluded entirely outside its native range;
 * glue/vendor_kernel_overlay.c detects the same range and gracefully
 * skips installing the vendored overlayfs override on unsupported
 * kernels (falling back to the running kernel's own overlay
 * implementation) instead of failing the build.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0) && LINUX_VERSION_CODE < KERNEL_VERSION(6, 19, 0)


#include <linux/cred.h>
#include <linux/file.h>
#include <linux/mount.h>
#include <linux/xattr.h>
#include <linux/uio.h>
#include <linux/uaccess.h>
#include <linux/splice.h>
#include <linux/security.h>
#include <linux/mm.h>
#include <linux/fs.h>
/* [BUILD-COMPAT] backing-file.h only exists >=6.9 (backing_file_read_iter etc.);
 * backing_file_open() lives in <linux/fs.h> on 6.6. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
#include <linux/backing-file.h>
#endif
#include "overlayfs.h"

#if VNS_OVL_NEED_BACKING_FILE_FALLBACK
struct ovl_aio_req {
	struct kiocb iocb;
	refcount_t ref;
	struct kiocb *orig_iocb;
};

static rwf_t ovl_iocb_to_rwf(int ifl)
{
	rwf_t flags = 0;

	if (ifl & IOCB_NOWAIT)
		flags |= RWF_NOWAIT;
	if (ifl & IOCB_HIPRI)
		flags |= RWF_HIPRI;
	if (ifl & IOCB_DSYNC)
		flags |= RWF_DSYNC;
	if (ifl & IOCB_SYNC)
		flags |= RWF_SYNC;

	return flags;
}

static inline void ovl_aio_put(struct ovl_aio_req *aio_req)
{
	if (refcount_dec_and_test(&aio_req->ref)) {
		fput(aio_req->iocb.ki_filp);
		kfree(aio_req);
	}
}

static void ovl_aio_cleanup_handler(struct ovl_aio_req *aio_req)
{
	struct kiocb *iocb = &aio_req->iocb;
	struct kiocb *orig_iocb = aio_req->orig_iocb;

	if (iocb->ki_flags & IOCB_WRITE) {
		struct inode *inode = file_inode(orig_iocb->ki_filp);

		kiocb_end_write(iocb);
		ovl_copyattr(inode);
	}

	orig_iocb->ki_pos = iocb->ki_pos;
	ovl_aio_put(aio_req);
}

#if VNS_OVL_TIER_MID_NEW
static void ovl_aio_rw_complete(struct kiocb *iocb, long res)
{
	struct ovl_aio_req *aio_req = container_of(iocb,
						   struct ovl_aio_req, iocb);
	struct kiocb *orig_iocb = aio_req->orig_iocb;

	ovl_aio_cleanup_handler(aio_req);
	orig_iocb->ki_complete(orig_iocb, res);
}
#else
/*
 * [BUILD-COMPAT] struct kiocb's ->ki_complete took a third `long ret2`
 * argument on the 5.10/5.15-shaped kernels (verified against
 * android.googlesource.com include/linux/fs.h; dropped again by 6.1) --
 * match that arity here instead of the unified (6.12-shaped, 2-arg) one.
 */
static void ovl_aio_rw_complete(struct kiocb *iocb, long res, long res2)
{
	struct ovl_aio_req *aio_req = container_of(iocb,
						   struct ovl_aio_req, iocb);
	struct kiocb *orig_iocb = aio_req->orig_iocb;

	ovl_aio_cleanup_handler(aio_req);
	orig_iocb->ki_complete(orig_iocb, res, res2);
}
#endif
#endif /* VNS_OVL_NEED_BACKING_FILE_FALLBACK */

static char ovl_whatisit(struct inode *inode, struct inode *realinode)
{
	if (realinode != ovl_inode_upper(inode))
		return 'l';
	if (ovl_has_upperdata(inode))
		return 'u';
	else
		return 'm';
}

static struct file *ovl_open_realfile(const struct file *file,
				      const struct path *realpath)
{
	struct inode *realinode = d_inode(realpath->dentry);
	struct inode *inode = file_inode(file);
	struct mnt_idmap *real_idmap;
	struct file *realfile;
	const struct cred *old_cred;
	int flags = file->f_flags | OVL_OPEN_FLAGS;
	int acc_mode = ACC_MODE(flags);
	int err;

	if (flags & O_APPEND)
		acc_mode |= MAY_APPEND;

	old_cred = ovl_override_creds(inode->i_sb);
	real_idmap = ovl_mnt_idmap(realpath->mnt);
	err = inode_permission(real_idmap, realinode, MAY_OPEN | acc_mode);
	if (err) {
		realfile = ERR_PTR(err);
	} else {
		if (!inode_owner_or_capable(real_idmap, realinode))
			flags &= ~O_NOATIME;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
		realfile = backing_file_open(file_user_path((struct file *) file),
					     flags, realpath, current_cred());
#else
		realfile = backing_file_open(&file->f_path, flags, realpath,
					     current_cred());
#endif
	}
	revert_creds(old_cred);

	pr_debug("open(%p[%pD2/%c], 0%o) -> (%p, 0%o)\n",
		 file, file, ovl_whatisit(inode, realinode), file->f_flags,
		 realfile, IS_ERR(realfile) ? 0 : realfile->f_flags);

	return realfile;
}

#define OVL_SETFL_MASK (O_APPEND | O_NONBLOCK | O_NDELAY | O_DIRECT)

static int ovl_change_flags(struct file *file, unsigned int flags)
{
	struct inode *inode = file_inode(file);
	int err;

	flags &= OVL_SETFL_MASK;

	if (((flags ^ file->f_flags) & O_APPEND) && IS_APPEND(inode))
		return -EPERM;

	if ((flags & O_DIRECT) && !VNS_OVL_FMODE_CAN_ODIRECT(file))
		return -EINVAL;

	if (file->f_op->check_flags) {
		err = file->f_op->check_flags(flags);
		if (err)
			return err;
	}

	spin_lock(&file->f_lock);
	file->f_flags = (file->f_flags & ~OVL_SETFL_MASK) | flags;
#if VNS_OVL_TIER_MID_NEW
	file->f_iocb_flags = iocb_flags(file);
#endif
	spin_unlock(&file->f_lock);

	return 0;
}

static int ovl_real_fdget_meta(const struct file *file, struct fd *real,
			       bool allow_meta)
{
	struct dentry *dentry = file_dentry(file);
	struct file *realfile = file->private_data;
	struct path realpath;
	int err;

	*real = vns_ovl_borrowed_fd(realfile);

	if (allow_meta) {
		ovl_path_real(dentry, &realpath);
	} else {
		/* lazy lookup and verify of lowerdata */
		err = ovl_verify_lowerdata(dentry);
		if (err)
			return err;

		ovl_path_realdata(dentry, &realpath);
	}
	if (!realpath.dentry)
		return -EIO;

	/* Has it been copied up since we'd opened it? */
	if (unlikely(file_inode(realfile) != d_inode(realpath.dentry))) {
		struct file *f = ovl_open_realfile(file, &realpath);
		if (IS_ERR(f))
			return PTR_ERR(f);
		*real = vns_ovl_cloned_fd(f);
		return 0;
	}

	/* Did the flags change since open? */
	if (unlikely((file->f_flags ^ realfile->f_flags) & ~OVL_OPEN_FLAGS))
		return ovl_change_flags(realfile, file->f_flags);

	return 0;
}

static int ovl_real_fdget(const struct file *file, struct fd *real)
{
	if (d_is_dir(file_dentry(file))) {
		struct file *f = ovl_dir_real_file(file, false);
		if (IS_ERR(f))
			return PTR_ERR(f);
		*real = vns_ovl_borrowed_fd(f);
		return 0;
	}

	return ovl_real_fdget_meta(file, real, false);
}

static int ovl_open(struct inode *inode, struct file *file)
{
	struct dentry *dentry = file_dentry(file);
	struct file *realfile;
	struct path realpath;
	int err;

	/* lazy lookup and verify lowerdata */
	err = ovl_verify_lowerdata(dentry);
	if (err)
		return err;

	err = ovl_maybe_copy_up(dentry, file->f_flags);
	if (err)
		return err;

	/* No longer need these flags, so don't pass them on to underlying fs */
	file->f_flags &= ~(O_CREAT | O_EXCL | O_NOCTTY | O_TRUNC);

	ovl_path_realdata(dentry, &realpath);
	if (!realpath.dentry)
		return -EIO;

	realfile = ovl_open_realfile(file, &realpath);
	if (IS_ERR(realfile))
		return PTR_ERR(realfile);

	file->private_data = realfile;

	return 0;
}

static int ovl_release(struct inode *inode, struct file *file)
{
	fput(file->private_data);

	return 0;
}

static loff_t ovl_llseek(struct file *file, loff_t offset, int whence)
{
	struct inode *inode = file_inode(file);
	struct fd real;
	const struct cred *old_cred;
	loff_t ret;

	/*
	 * The two special cases below do not need to involve real fs,
	 * so we can optimizing concurrent callers.
	 */
	if (offset == 0) {
		if (whence == SEEK_CUR)
			return file->f_pos;

		if (whence == SEEK_SET)
			return vfs_setpos(file, 0, 0);
	}

	ret = ovl_real_fdget(file, &real);
	if (ret)
		return ret;

	/*
	 * Overlay file f_pos is the master copy that is preserved
	 * through copy up and modified on read/write, but only real
	 * fs knows how to SEEK_HOLE/SEEK_DATA and real fs may impose
	 * limitations that are more strict than ->s_maxbytes for specific
	 * files, so we use the real file to perform seeks.
	 */
	ovl_inode_lock(inode);
	fd_file(real)->f_pos = file->f_pos;

	old_cred = ovl_override_creds(inode->i_sb);
	ret = vfs_llseek(fd_file(real), offset, whence);
	revert_creds(old_cred);

	file->f_pos = fd_file(real)->f_pos;
	ovl_inode_unlock(inode);

	fdput(real);

	return ret;
}

static void ovl_file_modified(struct file *file)
{
	/* Update size/mtime */
	ovl_copyattr(file_inode(file));
}

#if VNS_OVL_HAVE_BACKING_FILE_RW
static void ovl_file_end_write(struct file *file, loff_t pos, ssize_t ret)
{
	ovl_file_modified(file);
}
#endif

static void ovl_file_accessed(struct file *file)
{
	struct inode *inode, *upperinode;
	struct timespec64 ctime, uctime;
	struct timespec64 mtime, umtime;

	if (file->f_flags & O_NOATIME)
		return;

	inode = file_inode(file);
	upperinode = ovl_inode_upper(inode);

	if (!upperinode)
		return;

	ctime = inode_get_ctime(inode);
	uctime = inode_get_ctime(upperinode);
	mtime = inode_get_mtime(inode);
	umtime = inode_get_mtime(upperinode);
	if ((!timespec64_equal(&mtime, &umtime)) ||
	     !timespec64_equal(&ctime, &uctime)) {
		inode_set_mtime_to_ts(inode, inode_get_mtime(upperinode));
		inode_set_ctime_to_ts(inode, uctime);
	}

	touch_atime(&file->f_path);
}

static ssize_t ovl_read_iter(struct kiocb *iocb, struct iov_iter *iter)
{
	struct file *file = iocb->ki_filp;
	struct fd real;
	ssize_t ret;
#if VNS_OVL_HAVE_BACKING_FILE_RW
	struct backing_file_ctx ctx = {
		.cred = ovl_creds(file_inode(file)->i_sb),
		.user_file = file,
		.accessed = ovl_file_accessed,
	};
#else
	const struct cred *old_cred;
#endif

	if (!iov_iter_count(iter))
		return 0;

	ret = ovl_real_fdget(file, &real);
	if (ret)
		return ret;

#if VNS_OVL_HAVE_BACKING_FILE_RW
	ret = backing_file_read_iter(fd_file(real), iter, iocb, iocb->ki_flags,
				     &ctx);
#else
	ret = -EINVAL;
	if (iocb->ki_flags & IOCB_DIRECT &&
	    !VNS_OVL_FMODE_CAN_ODIRECT(fd_file(real)))
		goto out_fdput;

	old_cred = ovl_override_creds(file_inode(file)->i_sb);
	if (is_sync_kiocb(iocb)) {
		ret = vfs_iter_read(fd_file(real), iter, &iocb->ki_pos,
				    ovl_iocb_to_rwf(iocb->ki_flags));
	} else {
		struct ovl_aio_req *aio_req;

		ret = -ENOMEM;
		aio_req = kzalloc(sizeof(*aio_req), GFP_KERNEL);
		if (!aio_req)
			goto out_revert;

		aio_req->orig_iocb = iocb;
		kiocb_clone(&aio_req->iocb, iocb, get_file(fd_file(real)));
		aio_req->iocb.ki_complete = ovl_aio_rw_complete;
		refcount_set(&aio_req->ref, 2);
		ret = vfs_iocb_iter_read(fd_file(real), &aio_req->iocb, iter);
		ovl_aio_put(aio_req);
		if (ret != -EIOCBQUEUED)
			ovl_aio_cleanup_handler(aio_req);
	}
out_revert:
	revert_creds(old_cred);
	ovl_file_accessed(file);
out_fdput:
#endif
	fdput(real);

	return ret;
}

static ssize_t ovl_write_iter(struct kiocb *iocb, struct iov_iter *iter)
{
	struct file *file = iocb->ki_filp;
	struct inode *inode = file_inode(file);
	struct fd real;
	ssize_t ret;
	int ifl = iocb->ki_flags;
#if VNS_OVL_HAVE_BACKING_FILE_RW
	struct backing_file_ctx ctx = {
		.cred = ovl_creds(inode->i_sb),
		.user_file = file,
		.end_write = ovl_file_end_write,
	};
#else
	const struct cred *old_cred;
#endif

	if (!iov_iter_count(iter))
		return 0;

	inode_lock(inode);
	/* Update mode */
	ovl_copyattr(inode);

	ret = ovl_real_fdget(file, &real);
	if (ret)
		goto out_unlock;

	if (!ovl_should_sync(OVL_FS(inode->i_sb)))
		ifl &= ~(IOCB_DSYNC | IOCB_SYNC);

	/*
	 * Overlayfs doesn't support deferred completions, don't copy
	 * this property in case it is set by the issuer.
	 */
#ifdef IOCB_DIO_CALLER_COMP
	ifl &= ~IOCB_DIO_CALLER_COMP;
#endif
#if VNS_OVL_HAVE_BACKING_FILE_RW
	ret = backing_file_write_iter(fd_file(real), iter, iocb, ifl, &ctx);
#else
	ret = -EINVAL;
	if (iocb->ki_flags & IOCB_DIRECT &&
	    !VNS_OVL_FMODE_CAN_ODIRECT(fd_file(real)))
		goto out_fdput;

	old_cred = ovl_override_creds(file_inode(file)->i_sb);
	if (is_sync_kiocb(iocb)) {
		file_start_write(fd_file(real));
		ret = vfs_iter_write(fd_file(real), iter, &iocb->ki_pos,
				     ovl_iocb_to_rwf(ifl));
		file_end_write(fd_file(real));
		ovl_copyattr(inode);
	} else {
		struct ovl_aio_req *aio_req;

		ret = -ENOMEM;
		aio_req = kzalloc(sizeof(*aio_req), GFP_KERNEL);
		if (!aio_req)
			goto out_revert;

		aio_req->orig_iocb = iocb;
		kiocb_clone(&aio_req->iocb, iocb, get_file(fd_file(real)));
		aio_req->iocb.ki_flags = ifl;
		aio_req->iocb.ki_complete = ovl_aio_rw_complete;
		refcount_set(&aio_req->ref, 2);
		kiocb_start_write(&aio_req->iocb);
		ret = vfs_iocb_iter_write(fd_file(real), &aio_req->iocb, iter);
		ovl_aio_put(aio_req);
		if (ret != -EIOCBQUEUED)
			ovl_aio_cleanup_handler(aio_req);
	}
out_revert:
	revert_creds(old_cred);
out_fdput:
#endif
	fdput(real);

out_unlock:
	inode_unlock(inode);

	return ret;
}

static ssize_t ovl_splice_read(struct file *in, loff_t *ppos,
			       struct pipe_inode_info *pipe, size_t len,
			       unsigned int flags)
{
	struct fd real;
	ssize_t ret;
#if VNS_OVL_HAVE_BACKING_FILE_RW
	struct backing_file_ctx ctx = {
		.cred = ovl_creds(file_inode(in)->i_sb),
		.user_file = in,
		.accessed = ovl_file_accessed,
	};
#else
	const struct cred *old_cred;
#endif

	ret = ovl_real_fdget(in, &real);
	if (ret)
		return ret;

#if VNS_OVL_HAVE_BACKING_FILE_RW
	ret = backing_file_splice_read(fd_file(real), ppos, pipe, len, flags, &ctx);
#else
	old_cred = ovl_override_creds(file_inode(in)->i_sb);
	ret = generic_file_splice_read(fd_file(real), ppos, pipe, len, flags);
	revert_creds(old_cred);
	ovl_file_accessed(in);
#endif
	fdput(real);

	return ret;
}

/*
 * Calling iter_file_splice_write() directly from overlay's f_op may deadlock
 * due to lock order inversion between pipe->mutex in iter_file_splice_write()
 * and file_start_write(fd_file(real)) in ovl_write_iter().
 *
 * So do everything ovl_write_iter() does and call iter_file_splice_write() on
 * the real file.
 */
static ssize_t ovl_splice_write(struct pipe_inode_info *pipe, struct file *out,
				loff_t *ppos, size_t len, unsigned int flags)
{
	struct fd real;
	struct inode *inode = file_inode(out);
	ssize_t ret;
#if VNS_OVL_HAVE_BACKING_FILE_RW
	struct backing_file_ctx ctx = {
		.cred = ovl_creds(inode->i_sb),
		.user_file = out,
		.end_write = ovl_file_end_write,
	};
#else
	const struct cred *old_cred;
#endif

	inode_lock(inode);
	/* Update mode */
	ovl_copyattr(inode);

	ret = ovl_real_fdget(out, &real);
	if (ret)
		goto out_unlock;

#if VNS_OVL_HAVE_BACKING_FILE_RW
	ret = backing_file_splice_write(pipe, fd_file(real), ppos, len, flags, &ctx);
#else
	old_cred = ovl_override_creds(inode->i_sb);
	file_start_write(fd_file(real));
	ret = iter_file_splice_write(pipe, fd_file(real), ppos, len, flags);
	file_end_write(fd_file(real));
	ovl_copyattr(inode);
	revert_creds(old_cred);
#endif
	fdput(real);

out_unlock:
	inode_unlock(inode);

	return ret;
}

static int ovl_fsync(struct file *file, loff_t start, loff_t end, int datasync)
{
	struct fd real;
	const struct cred *old_cred;
	int ret;

	ret = ovl_sync_status(OVL_FS(file_inode(file)->i_sb));
	if (ret <= 0)
		return ret;

	ret = ovl_real_fdget_meta(file, &real, !datasync);
	if (ret)
		return ret;

	/* Don't sync lower file for fear of receiving EROFS error */
	if (file_inode(fd_file(real)) == ovl_inode_upper(file_inode(file))) {
		old_cred = ovl_override_creds(file_inode(file)->i_sb);
		ret = vfs_fsync_range(fd_file(real), start, end, datasync);
		revert_creds(old_cred);
	}

	fdput(real);

	return ret;
}

static int ovl_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct file *realfile = file->private_data;
#if VNS_OVL_HAVE_BACKING_FILE_RW
	struct backing_file_ctx ctx = {
		.cred = ovl_creds(file_inode(file)->i_sb),
		.user_file = file,
		.accessed = ovl_file_accessed,
	};
#else
	const struct cred *old_cred;
	int ret;
#endif

#if VNS_OVL_HAVE_BACKING_FILE_RW
	return backing_file_mmap(realfile, vma, &ctx);
#else
	if (!realfile->f_op->mmap)
		return -ENODEV;

	if (WARN_ON(file != vma->vm_file))
		return -EIO;

	vma_set_file(vma, realfile);

	old_cred = ovl_override_creds(file_inode(file)->i_sb);
	ret = call_mmap(vma->vm_file, vma);
	revert_creds(old_cred);
	ovl_file_accessed(file);

	return ret;
#endif
}

static long ovl_fallocate(struct file *file, int mode, loff_t offset, loff_t len)
{
	struct inode *inode = file_inode(file);
	struct fd real;
	const struct cred *old_cred;
	int ret;

	inode_lock(inode);
	/* Update mode */
	ovl_copyattr(inode);
	ret = file_remove_privs(file);
	if (ret)
		goto out_unlock;

	ret = ovl_real_fdget(file, &real);
	if (ret)
		goto out_unlock;

	old_cred = ovl_override_creds(file_inode(file)->i_sb);
	ret = vfs_fallocate(fd_file(real), mode, offset, len);
	revert_creds(old_cred);

	/* Update size */
	ovl_file_modified(file);

	fdput(real);

out_unlock:
	inode_unlock(inode);

	return ret;
}

static int ovl_fadvise(struct file *file, loff_t offset, loff_t len, int advice)
{
	struct fd real;
	const struct cred *old_cred;
	int ret;

	ret = ovl_real_fdget(file, &real);
	if (ret)
		return ret;

	old_cred = ovl_override_creds(file_inode(file)->i_sb);
	ret = vfs_fadvise(fd_file(real), offset, len, advice);
	revert_creds(old_cred);

	fdput(real);

	return ret;
}

enum ovl_copyop {
	OVL_COPY,
	OVL_CLONE,
	OVL_DEDUPE,
};

static loff_t ovl_copyfile(struct file *file_in, loff_t pos_in,
			    struct file *file_out, loff_t pos_out,
			    loff_t len, unsigned int flags, enum ovl_copyop op)
{
	struct inode *inode_out = file_inode(file_out);
	struct fd real_in, real_out;
	const struct cred *old_cred;
	loff_t ret;

	inode_lock(inode_out);
	if (op != OVL_DEDUPE) {
		/* Update mode */
		ovl_copyattr(inode_out);
		ret = file_remove_privs(file_out);
		if (ret)
			goto out_unlock;
	}

	ret = ovl_real_fdget(file_out, &real_out);
	if (ret)
		goto out_unlock;

	ret = ovl_real_fdget(file_in, &real_in);
	if (ret) {
		fdput(real_out);
		goto out_unlock;
	}

	old_cred = ovl_override_creds(file_inode(file_out)->i_sb);
	switch (op) {
	case OVL_COPY:
		ret = vfs_copy_file_range(fd_file(real_in), pos_in,
					  fd_file(real_out), pos_out, len, flags);
		break;

	case OVL_CLONE:
		ret = vfs_clone_file_range(fd_file(real_in), pos_in,
					   fd_file(real_out), pos_out, len, flags);
		break;

	case OVL_DEDUPE:
		ret = vfs_dedupe_file_range_one(fd_file(real_in), pos_in,
						fd_file(real_out), pos_out, len,
						flags);
		break;
	}
	revert_creds(old_cred);

	/* Update size */
	ovl_file_modified(file_out);

	fdput(real_in);
	fdput(real_out);

out_unlock:
	inode_unlock(inode_out);

	return ret;
}

static ssize_t ovl_copy_file_range(struct file *file_in, loff_t pos_in,
				   struct file *file_out, loff_t pos_out,
				   size_t len, unsigned int flags)
{
	return ovl_copyfile(file_in, pos_in, file_out, pos_out, len, flags,
			    OVL_COPY);
}

static loff_t ovl_remap_file_range(struct file *file_in, loff_t pos_in,
				   struct file *file_out, loff_t pos_out,
				   loff_t len, unsigned int remap_flags)
{
	enum ovl_copyop op;

	if (remap_flags & ~(REMAP_FILE_DEDUP | REMAP_FILE_ADVISORY))
		return -EINVAL;

	if (remap_flags & REMAP_FILE_DEDUP)
		op = OVL_DEDUPE;
	else
		op = OVL_CLONE;

	/*
	 * Don't copy up because of a dedupe request, this wouldn't make sense
	 * most of the time (data would be duplicated instead of deduplicated).
	 */
	if (op == OVL_DEDUPE &&
	    (!ovl_inode_upper(file_inode(file_in)) ||
	     !ovl_inode_upper(file_inode(file_out))))
		return -EPERM;

	return ovl_copyfile(file_in, pos_in, file_out, pos_out, len,
			    remap_flags, op);
}

static int ovl_flush(struct file *file, fl_owner_t id)
{
	struct fd real;
	const struct cred *old_cred;
	int err;

	err = ovl_real_fdget(file, &real);
	if (err)
		return err;

	if (fd_file(real)->f_op->flush) {
		old_cred = ovl_override_creds(file_inode(file)->i_sb);
		err = fd_file(real)->f_op->flush(fd_file(real), id);
		revert_creds(old_cred);
	}
	fdput(real);

	return err;
}

const struct file_operations ovl_file_operations = {
	.open		= ovl_open,
	.release	= ovl_release,
	.llseek		= ovl_llseek,
	.read_iter	= ovl_read_iter,
	.write_iter	= ovl_write_iter,
	.fsync		= ovl_fsync,
	.mmap		= ovl_mmap,
	.fallocate	= ovl_fallocate,
	.fadvise	= ovl_fadvise,
	.flush		= ovl_flush,
	.splice_read    = ovl_splice_read,
	.splice_write   = ovl_splice_write,

	.copy_file_range	= ovl_copy_file_range,
	.remap_file_range	= ovl_remap_file_range,
};

#endif /* LINUX_VERSION_CODE in [KERNEL_VERSION(6, 12, 0), KERNEL_VERSION(6, 13, 0)) */
