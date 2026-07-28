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
#include <linux/string.h>
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
#include <linux/mm.h>
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

/*
 * [BUILD-COMPAT] seq_escape() is deliberately NOT in VNS_OVL_VFS_COMPAT_LIST:
 * params.c's seq_show_option() (a `static inline` parsed from
 * <linux/fs_context.h>, always before this file's own VNS_OVL_VFS_COMPAT_LIST
 * redirects could apply) calls seq_escape() directly by name, so a macro
 * redirect here could never reach that already-expanded call site anyway.
 *
 * Starting with the 6.1 kernel, <linux/seq_file.h> made seq_escape() a
 * `static inline` wrapper over seq_escape_mem(), so no import is ever
 * needed there. Before 6.1 (confirmed live on android12-5.10,
 * android13/14-5.15) it is a genuine, separately EXPORT_SYMBOL'd function
 * -- and CI observed a live "Unknown symbol seq_escape" insmod failure on
 * android14-5.15, because (like every other name in VNS_OVL_VFS_COMPAT_LIST)
 * it can be trimmed from a production GKI build's module symbol table even
 * though EXPORT_SYMBOL'd in source.
 *
 * Rather than require the real seq_escape() as an import (which
 * shadow_hook_resolve() can't help with here, since it's never called
 * through a resolved pointer at its call site), provide our own
 * externally-linked definition, gated to versions below 6.1 where the
 * kernel doesn't already supply an inline: any translation unit that
 * references "seq_escape" as an undefined symbol links against this local
 * definition instead of requiring one from vmlinux. It is a self-contained
 * reimplementation of the real kernel's octal-escaping logic using only
 * seq_putc()/seq_puts() -- fundamental, always exported, never-trimmed
 * seq_file primitives -- so it has no dependency on
 * seq_escape_str()/seq_escape_mem()/string_escape_str(), whose availability
 * and exact signature vary across this file's KMI range.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 1, 0)
void seq_escape(struct seq_file *m, const char *s, const char *esc)
{
	char c;

	while ((c = *s++) != '\0') {
		if (!strchr(esc, c)) {
			seq_putc(m, c);
			continue;
		}
		switch (c) {
		case '\n':
			seq_puts(m, "\\n");
			break;
		case '\t':
			seq_puts(m, "\\t");
			break;
		case '\\':
			seq_puts(m, "\\\\");
			break;
		default:
			seq_putc(m, '\\');
			seq_putc(m, '0' + ((unsigned char)c >> 6));
			seq_putc(m, '0' + (((unsigned char)c >> 3) & 7));
			seq_putc(m, '0' + ((unsigned char)c & 7));
		}
	}
}
#endif /* LINUX_VERSION_CODE < 6.1.0 */

/*
 * [BUILD-COMPAT] logfc (fs/fs_context.c). Genuinely EXPORT_SYMBOL'd on
 * every KMI in our support matrix, but -- same as seq_escape() above -- it
 * can be trimmed from a production GKI build's module symbol table
 * (CONFIG_TRIM_UNUSED_KSYMS), and CI observed a live "Unknown symbol
 * logfc" insmod failure on one such KMI.
 *
 * Its real callers here are not our own code: the __logfc()/__plog()
 * macros in <linux/fs_context.h> (backing infof()/warnf()/errorf()/
 * invalfc() etc., used extensively by our vendored fs/overlayfs/params.c)
 * call logfc() by name directly from their macro expansion, which is
 * finalized at each overlayfs call site well before this file's own
 * shadow_hook_resolve()-based redirects could ever intercept it -- the
 * same "already-inlined system header" pattern documented on
 * free_cgroup_ns()/__put_net()/down_write_killable() in
 * vendor_kernel_compat.c. Providing our own externally-linked logfc()
 * here satisfies every such caller directly out of this module's own
 * object files instead of requiring the (possibly trimmed) vmlinux
 * export.
 *
 * logfc()'s only purpose is to append a message to the mount's private
 * fc_log ring buffer for later retrieval via fsopen()/FSCONFIG_CMD_*
 * error reporting; it never affects whether a mount/parse operation
 * itself succeeds or fails. Rather than replicate struct fc_log's ring
 * buffer bookkeeping (module refcounting, kfree'able-string tracking),
 * this stub simply surfaces the message to the kernel log instead: mount
 * failures are still reported correctly to userspace (via the real errno
 * from the failing overlayfs call), just without the detailed message
 * text normally readable back through the fscontext fd.
 */
void logfc(struct fc_log *log, const char *prefix, char level, const char *fmt, ...)
{
	va_list args;
	char msg[256];

	va_start(args, fmt);
	vsnprintf(msg, sizeof(msg), fmt, args);
	va_end(args);

	LKM4CTR_WARN("vendor_kernel", "overlayfs: fs_context: %s%s%s",
		     prefix ? prefix : "", prefix ? ": " : "", msg);
}

#endif /* LINUX_VERSION_CODE in [5.10, 6.19) */
