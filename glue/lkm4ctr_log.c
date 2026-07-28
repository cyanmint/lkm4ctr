// SPDX-License-Identifier: GPL-2.0
/*
 * lkm4ctr_log - implementation of the ring buffer declared in
 * glue/lkm4ctr_log.h. See that header for the full rationale.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/ktime.h>

#include "lkm4ctr_log.h"

struct lkm4ctr_log_entry {
	u64	timestamp_ns;
	u8	level;
	char	tag[LKM4CTR_LOG_TAG_MAX];
	char	msg[LKM4CTR_LOG_LINE_MAX];
};

static struct lkm4ctr_log_entry lkm4ctr_log_ring[LKM4CTR_LOG_CAPACITY];
/* Index the *next* write will land on; entries wrap once full. */
static unsigned int lkm4ctr_log_head;
/* Total entries ever written, capped at LKM4CTR_LOG_CAPACITY for iteration. */
static unsigned int lkm4ctr_log_count;
static DEFINE_SPINLOCK(lkm4ctr_log_lock);

static const char *lkm4ctr_log_level_str(u8 level)
{
	switch (level) {
	case LKM4CTR_LOG_WARN:
		return "warn";
	case LKM4CTR_LOG_ERR:
		return "error";
	default:
		return "info";
	}
}

void lkm4ctr_log(const char *tag, enum lkm4ctr_log_level level,
		  const char *fmt, ...)
{
	struct lkm4ctr_log_entry *e;
	unsigned long flags;
	va_list args;

	spin_lock_irqsave(&lkm4ctr_log_lock, flags);
	e = &lkm4ctr_log_ring[lkm4ctr_log_head];
	e->timestamp_ns = ktime_get_boottime_ns();
	e->level = (u8)level;
	strscpy(e->tag, tag ? tag : "lkm4ctr", sizeof(e->tag));

	va_start(args, fmt);
	vsnprintf(e->msg, sizeof(e->msg), fmt, args);
	va_end(args);

	lkm4ctr_log_head = (lkm4ctr_log_head + 1) % LKM4CTR_LOG_CAPACITY;
	if (lkm4ctr_log_count < LKM4CTR_LOG_CAPACITY)
		lkm4ctr_log_count++;
	spin_unlock_irqrestore(&lkm4ctr_log_lock, flags);

#ifdef CONFIG_LKM4CTR_LOG_DMESG
	switch (level) {
	case LKM4CTR_LOG_WARN:
		pr_warn("lkm4ctr: %s: %s\n", tag ? tag : "lkm4ctr", e->msg);
		break;
	case LKM4CTR_LOG_ERR:
		pr_err("lkm4ctr: %s: %s\n", tag ? tag : "lkm4ctr", e->msg);
		break;
	default:
		pr_info("lkm4ctr: %s: %s\n", tag ? tag : "lkm4ctr", e->msg);
		break;
	}
#endif
}
EXPORT_SYMBOL_GPL(lkm4ctr_log);

static size_t lkm4ctr_log_entry_len(const struct lkm4ctr_log_entry *e)
{
	return snprintf(NULL, 0, "[%llu.%06llu][%s][%s] %s\n",
			 e->timestamp_ns / NSEC_PER_SEC,
			 (e->timestamp_ns % NSEC_PER_SEC) / NSEC_PER_USEC,
			 e->tag, lkm4ctr_log_level_str(e->level), e->msg);
}

/*
 * lkm4ctr_log_snprintf() - see header. Takes a private snapshot copy of the
 * matching entries under the spinlock (fixed-size on-stack-sized array
 * would be too large; use a bounded local scan instead: walk the ring
 * directly under the lock, formatting straight into @buf). This holds the
 * lock across vscnprintf() into @buf, which is acceptable here: @buf is a
 * plain kernel buffer (no page faults), and the whole scan is at most a
 * handful of LKM4CTR_LOG_CAPACITY-bounded passes of fixed-size formatting --
 * bounded, non-blocking work, matching what other spinlock-protected
 * fixed-size kernel ring buffers (e.g. printk's own logbuf) already do.
 *
 * Whenever every matching entry doesn't fit in @buflen, the OLDEST matching
 * entries are discarded first (never the newest): pass 1 below walks
 * newest -> oldest, growing a "keep" tail subset for as long as it still
 * fits @buflen, so the most recently logged lines -- the ones an
 * administrator actually cares about when something just went wrong -- are
 * always what a bounded read() sees, instead of a stale head with the tail
 * silently cut off.
 */
size_t lkm4ctr_log_snprintf(const char *tag, char *buf, size_t buflen)
{
	unsigned long flags;
	unsigned int i, start, n;
	unsigned int keep = 0, matched = 0, skip;
	size_t total_needed = 0, kept_len = 0, pos = 0;

	spin_lock_irqsave(&lkm4ctr_log_lock, flags);
	n = lkm4ctr_log_count;
	start = (lkm4ctr_log_head + LKM4CTR_LOG_CAPACITY - n) % LKM4CTR_LOG_CAPACITY;

	/* Pass 1 (newest -> oldest): how many of the most recent matching
	 * entries fit within @buflen.
	 */
	for (i = 0; i < n; i++) {
		unsigned int idx = (start + (n - 1 - i)) % LKM4CTR_LOG_CAPACITY;
		struct lkm4ctr_log_entry *e = &lkm4ctr_log_ring[idx];
		size_t len;

		if (tag && strcmp(tag, e->tag))
			continue;

		len = lkm4ctr_log_entry_len(e);
		if (kept_len + len >= buflen)
			break;
		kept_len += len;
		keep++;
	}

	/* Pass 2 (oldest -> newest): the true total size of every matching
	 * entry, regardless of what fits in @buflen, so a caller that grows
	 * @buflen and retries (see lkm4ctr_diagfs_show()) can still recover
	 * the entire log.
	 */
	for (i = 0; i < n; i++) {
		unsigned int idx = (start + i) % LKM4CTR_LOG_CAPACITY;
		struct lkm4ctr_log_entry *e = &lkm4ctr_log_ring[idx];

		if (tag && strcmp(tag, e->tag))
			continue;
		total_needed += lkm4ctr_log_entry_len(e);
		matched++;
	}

	/* Pass 3: render just the "keep" newest matching entries (oldest
	 * first within that subset), skipping the older ones that didn't
	 * fit.
	 */
	skip = matched > keep ? matched - keep : 0;
	matched = 0;
	for (i = 0; i < n && matched < skip + keep; i++) {
		unsigned int idx = (start + i) % LKM4CTR_LOG_CAPACITY;
		struct lkm4ctr_log_entry *e = &lkm4ctr_log_ring[idx];

		if (tag && strcmp(tag, e->tag))
			continue;
		if (matched++ < skip)
			continue;

		pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
				  "[%llu.%06llu][%s][%s] %s\n",
				  e->timestamp_ns / NSEC_PER_SEC,
				  (e->timestamp_ns % NSEC_PER_SEC) / NSEC_PER_USEC,
				  e->tag, lkm4ctr_log_level_str(e->level), e->msg);
	}
	spin_unlock_irqrestore(&lkm4ctr_log_lock, flags);

	return total_needed;
}
EXPORT_SYMBOL_GPL(lkm4ctr_log_snprintf);
