/* SPDX-License-Identifier: GPL-2.0 */
/*
 * lkm4ctr_hotreload - "hot reload" this module in place from a new .ko
 * built on disk, without an operator having to run `rmmod`/`insmod` by
 * hand in the right order.
 *
 * See lkm4ctr_hotreload.c for the full design. In short:
 *
 *   echo /path/to/new/lkm4ctr.ko > <mnt>/global/hotreload/do-hot-reload
 *
 * quiesces every hook, waits (best-effort) for module_refcount() to drain,
 * unmounts any lkm4ctr diagfs mount, then hands off to a detached userspace
 * shell that retries `rmmod lkm4ctr` until it succeeds and then runs
 * `insmod <path> hotreload=1` -- the `hotreload=1` module parameter is how
 * the *new* instance's lkm4ctr_hotreload_init() (called from
 * lkm4ctr_init()) tells a genuine first `insmod` apart from a hot-reloaded
 * one.
 *
 * <mnt>/global/hotreload/status and .../log (tag "hotreload") report
 * progress; see lkm4ctr_diagfs.c for how these three files are wired into
 * the diagfs tree.
 */

#ifndef _LKM4CTR_HOTRELOAD_H
#define _LKM4CTR_HOTRELOAD_H

#include <linux/types.h>

/*
 * lkm4ctr_hotreload_init() - called once from lkm4ctr_init(). Always
 * succeeds (best-effort observational hooks only); logs whether this
 * instance is a first load or a hot-reloaded one (see the "hotreload"
 * module parameter in lkm4ctr_hotreload.c).
 */
int lkm4ctr_hotreload_init(void);
void lkm4ctr_hotreload_exit(void);

/*
 * lkm4ctr_hotreload_trigger() - validate @path and kick off the hot-reload
 * sequence in a dedicated kthread. Returns 0 once the worker has been
 * started (not once the reload has completed -- poll
 * lkm4ctr_hotreload_status_snprintf()/./status for that), or a negative
 * errno if @path is invalid or a hot reload is already in progress.
 */
int lkm4ctr_hotreload_trigger(const char *path);

/*
 * lkm4ctr_hotreload_status_snprintf() - render current hot-reload state
 * (this instance's first-load/hot-reloaded origin, plus the current
 * operation's state/target/last error if any) into @buf.
 */
size_t lkm4ctr_hotreload_status_snprintf(char *buf, size_t buflen);

#endif /* _LKM4CTR_HOTRELOAD_H */
