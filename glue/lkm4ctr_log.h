/* SPDX-License-Identifier: GPL-2.0 */
/*
 * lkm4ctr_log - shared, always-on ring-buffer log for the unified lkm4ctr.ko,
 * plus the CONFIG_LKM4CTR_LOG_DMESG build switch that controls whether the
 * same messages are *also* mirrored to the kernel log (dmesg/printk).
 *
 * Every subsystem previously called pr_info()/pr_warn()/pr_err() directly,
 * which meant the module's entire diagnostic history only ever existed in
 * dmesg -- unavailable once the ring buffer wraps, and, on production
 * devices, sometimes throttled or invisible to userspace entirely. This
 * header replaces those call sites (in the module's own lifecycle/hook/
 * safe_unload code; deliberately not every pr_debug() sprinkled through
 * per-syscall fast paths, which remain compiled out by default exactly as
 * before) with LKM4CTR_LOG()/_INFO()/_WARN()/_ERR(), each of which:
 *
 *   1. Always appends the formatted message to a small fixed-capacity,
 *      spinlock-protected circular buffer (lkm4ctr_log.c), tagged with the
 *      calling subsystem's name (e.g. "vendor_kernel", "safe_unload"). This
 *      buffer is what the lkm4ctr diagfs (lkm4ctr_diagfs.c)'s per-module
 *      `log` files and the top-level `safe_unload` file render on read().
 *      Because logging can happen from a kprobe pre_handler (atomic
 *      context, preemption disabled), the buffer never sleeps or allocates:
 *      entries are fixed-size structs in a static array, guarded by
 *      spin_lock_irqsave().
 *   2. Additionally calls printk() at the matching level *only* if
 *      CONFIG_LKM4CTR_LOG_DMESG is defined (see lkm4ctr/Kconfig and the
 *      LKM4CTR_LOG_DMESG Makefile knob that feeds it for this standalone
 *      out-of-tree module, where no real kernel .config participates).
 *      When undefined, lkm4ctr.ko never calls printk at all -- it is
 *      completely silent in dmesg, with every message only reachable via
 *      the diagfs mount.
 */

#ifndef _LKM4CTR_LOG_H
#define _LKM4CTR_LOG_H

#include <linux/kernel.h>
#include <linux/types.h>

/* Longest single formatted line kept in the ring buffer (truncated beyond this). */
#define LKM4CTR_LOG_LINE_MAX	200
/* Longest subsystem tag ("vendor_kernel" + NUL fits comfortably). */
#define LKM4CTR_LOG_TAG_MAX	20
/* Number of most-recent entries retained; oldest are overwritten first. */
#define LKM4CTR_LOG_CAPACITY	512

enum lkm4ctr_log_level {
	LKM4CTR_LOG_INFO,
	LKM4CTR_LOG_WARN,
	LKM4CTR_LOG_ERR,
};

/*
 * lkm4ctr_log() - record one line tagged with @tag at @level, and mirror it
 * to dmesg too when CONFIG_LKM4CTR_LOG_DMESG is enabled. Safe to call from
 * any context, including atomic (kprobe pre_handler / ftrace thunk).
 */
__printf(3, 4)
void lkm4ctr_log(const char *tag, enum lkm4ctr_log_level level,
		  const char *fmt, ...);

/*
 * lkm4ctr_log_snprintf() - render every currently retained entry whose tag
 * matches @tag (or every entry, if @tag is NULL) as newline-terminated text
 * "[level] message\n" lines into @buf (size @buflen). If every matching
 * entry doesn't fit, the OLDEST matching entries are discarded first so the
 * most recent (tail) entries are always what gets rendered; whatever
 * survives is still oldest-first among itself. Returns the true total size
 * of every matching entry (glibc snprintf() semantics: the number of bytes
 * that would have been written given unlimited space), *not* merely the
 * number of bytes actually written into @buf -- callers such as
 * lkm4ctr_diagfs_show() rely on this to detect truncation and grow @buf
 * before retrying.
 */
size_t lkm4ctr_log_snprintf(const char *tag, char *buf, size_t buflen);

#define LKM4CTR_LOG(tag, fmt, ...) \
	lkm4ctr_log((tag), LKM4CTR_LOG_INFO, fmt, ##__VA_ARGS__)
#define LKM4CTR_INFO(tag, fmt, ...) \
	lkm4ctr_log((tag), LKM4CTR_LOG_INFO, fmt, ##__VA_ARGS__)
#define LKM4CTR_WARN(tag, fmt, ...) \
	lkm4ctr_log((tag), LKM4CTR_LOG_WARN, fmt, ##__VA_ARGS__)
#define LKM4CTR_ERR(tag, fmt, ...) \
	lkm4ctr_log((tag), LKM4CTR_LOG_ERR, fmt, ##__VA_ARGS__)

#endif /* _LKM4CTR_LOG_H */
