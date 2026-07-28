// SPDX-License-Identifier: GPL-2.0
/*
 * Vendored from kernel-common fs/nsfs.c (kernel version 6.1.124,
 * android14-6.1 branch). CHANGES FROM UPSTREAM:
 *   - [RENAME] All non-static global symbols prefixed with vns_ to avoid
 *     collision with the built-in kernel implementation.
 *   - [BUILD-COMPAT] slab caches replaced with kzalloc/kfree (no kmem_cache_create
 *     in out-of-tree module init context).
 *   - [BUILD-COMPAT] ns_alloc_inum/ns_free_inum -> vns_alloc_inum/vns_free_inum
 *     (proc_alloc_inum not exported; resolved at init via shadow_hook_resolve).
 *   - [BUILD-COMPAT] __init/__exit removed from non-module-init functions.
 *   - [DIAGFS] Statistics incremented via vendor_kernel_registry for diagfs exposure.
 *   Any line NOT marked RENAME/BUILD-COMPAT/DIAGFS is unchanged from upstream.
 */
#include <linux/kernel.h>
#include <linux/mount.h>
#include <linux/proc_ns.h>

#include "../vendor_kernel.h"

void vns_nsfs_init(void) /* [RENAME] */
{
	/* [BUILD-COMPAT] vendor_kernel reuses the built-in nsfs on the running kernel. */
}
