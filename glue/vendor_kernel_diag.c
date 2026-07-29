// SPDX-License-Identifier: GPL-2.0
/*
 * vendor_kernel_diag.c - diagnostics renderer for vendor_kernel.
 * This is NEW code (not vendored from kernel-common).
 */
#include <linux/kernel.h>
#include <linux/hashtable.h>

#include "../vendor/vendor_kernel.h"

size_t vendor_kernel_diag_snprintf(char *buf, size_t buflen)
{
	size_t pos = 0;
	struct vns_task *t;
	unsigned int bkt;
	unsigned long flags;

	spin_lock_irqsave(&vendor_kernel_registry.lock, flags);
	pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
			 "enabled: %s\n"
			 "tasks: %lu\n"
			 "stat_unshare: %lu\n"
			 "stat_setns: %lu\n"
			 "stat_clone: %lu\n"
			 "stat_reboot: %lu\n",
			 vendor_kernel_enabled ? "yes" : "no",
			 vendor_kernel_registry.task_count,
			 vendor_kernel_registry.stat_unshare,
			 vendor_kernel_registry.stat_setns,
			 vendor_kernel_registry.stat_clone,
			 vendor_kernel_registry.stat_reboot);
	hash_for_each(vendor_kernel_registry.tasks, bkt, t, node)
		pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
				 "tgid=%d nsproxy=%px\n", t->tgid, t->nsproxy);
	spin_unlock_irqrestore(&vendor_kernel_registry.lock, flags);
	pos += vns_overlay_diag_snprintf(buf + pos, pos < buflen ? buflen - pos : 0);
	return pos;
}
