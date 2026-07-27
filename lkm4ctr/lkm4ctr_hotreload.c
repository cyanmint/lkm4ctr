// SPDX-License-Identifier: GPL-2.0
/*
 * lkm4ctr_hotreload - hot-reload this module in place from a new .ko on
 * disk, orchestrated entirely from inside the kernel via the diagfs
 * ("echo /path/to/new/lkm4ctr.ko > global/hotreload/do-hot-reload").
 *
 * The self-unload hazard already documented at length in lkm4ctr_diagfs.c
 * (a module can never free the memory its own currently executing code
 * lives in) applies here too, with one extra wrinkle: after this instance
 * is really gone, *something* still has to run `insmod` on the new path,
 * and that something cannot be this module's own code either. The design
 * below therefore hands off to a single detached userspace shell command
 * that does both steps in the right order once this module has quiesced
 * and drained -- never a second in-kernel step run by this module after
 * rmmod.
 *
 * Sequence
 * --------
 *  1. lkm4ctr_hotreload_trigger() validates @path (absolute, ends in
 *     ".ko", plain filesystem characters only -- no shell metacharacters,
 *     since @path ends up inside a generated shell command line) and a
 *     quick filp_open() existence/regular-file check, then records it and
 *     spawns lkm4ctr_hotreload_worker().
 *  2. The worker installs a best-effort, purely observational
 *     finit_module(2) hook (hotreload_hooks[] below) so that any insmod of
 *     the target path anywhere on the system is logged -- this is the
 *     "hook insmod syscalls" requirement: it never denies or redirects the
 *     call, it only lets the operator see (via global/hotreload/log) the
 *     moment the kernel's own module loader actually starts reading the
 *     new .ko, which is the closest observable proxy for "the kernel is
 *     now reloading code of this module from path" a hook operating at
 *     the syscall boundary can offer (the actual ELF relocation/symbol
 *     resolution work happens deep inside kernel/module/main.c, far below
 *     any hookable boundary).
 *  3. shadow_hook_quiesce(true), then poll module_refcount() same as
 *     lkm4ctr_safe_unload_fn(), best-effort (a stuck drain here still logs
 *     and proceeds -- this is opt-in operator action, not a safety gate).
 *  4. Best-effort `umount -l -a -t lkm4ctr` (any diagfs mount, including
 *     the one the operator is reading/writing this very sequence through,
 *     pins module_refcount() exactly like lkm4ctr_diagfs.c's own
 *     lkm4ctr_auto_umount_diagfs()).
 *  5. Launch a single detached `/bin/sh -c '...'` that retries
 *     `rmmod lkm4ctr` for a bounded number of attempts (this worker's own
 *     module_put_and_kthread_exit() below may not have run yet by the
 *     time the shell's first attempt fires) and only then runs
 *     `insmod <path> hotreload=1` -- seq_file the *new* instance's
 *     lkm4ctr_hotreload_init() reads (see the "hotreload" module
 *     parameter below) to tell a genuine first load apart from this one.
 *  6. module_put_and_kthread_exit(0), identical to
 *     lkm4ctr_safe_unload_fn()'s own final step.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/namei.h>
#include <linux/dcache.h>
#include <linux/limits.h>
#include <linux/fcntl.h>
#include <linux/umh.h>
#include <linux/version.h>
#include <linux/ctype.h>
#include <asm/ptrace.h>

#include "shadow_hook.h"
#include "lkm4ctr_log.h"
#include "lkm4ctr_compat.h"
#include "lkm4ctr_hotreload.h"

#define LKM4CTR_HOTRELOAD_TAG		"hotreload"
#define LKM4CTR_HOTRELOAD_PATH_MAX	256
#define LKM4CTR_HOTRELOAD_DRAIN_TIMEOUT_MS	30000
#define LKM4CTR_HOTRELOAD_DRAIN_POLL_MS		50

/*
 * "hotreload" module parameter - the handoff shell command below passes
 * `hotreload=1` on the new instance's own insmod command line
 * ("insmod <path> hotreload=1"); this is how lkm4ctr_hotreload_init() (see
 * bottom of this file, called from lkm4ctr_init()) tells a hot-reloaded
 * instance apart from a genuine first `insmod lkm4ctr.ko`. Read-only
 * (0444) purely for introspection (e.g. `cat /sys/module/lkm4ctr/parameters/hotreload`);
 * it is never written to at runtime, only ever supplied on the insmod
 * command line itself.
 */
static bool lkm4ctr_hotreload_is_reloaded_boot;
module_param_named(hotreload, lkm4ctr_hotreload_is_reloaded_boot, bool, 0444);
MODULE_PARM_DESC(hotreload,
		 "set by the hot-reload handoff shell command (insmod ... hotreload=1); never set by hand for a genuine first load");

enum lkm4ctr_hotreload_state {
	LKM4CTR_HOTRELOAD_IDLE,
	LKM4CTR_HOTRELOAD_RUNNING,
	LKM4CTR_HOTRELOAD_FAILED,
};

static DEFINE_MUTEX(lkm4ctr_hotreload_lock);
static enum lkm4ctr_hotreload_state lkm4ctr_hotreload_state = LKM4CTR_HOTRELOAD_IDLE;
static char lkm4ctr_hotreload_target[LKM4CTR_HOTRELOAD_PATH_MAX];
static char lkm4ctr_hotreload_fail_reason[160];
static struct task_struct *lkm4ctr_hotreload_thread;

/* ------------------------------------------------------------------- */
/* observational finit_module(2) hook                                   */
/* ------------------------------------------------------------------- */

static long (*real_finit_module)(const struct pt_regs *regs);
static long hotreload_hook_finit_module(const struct pt_regs *regs);

static const char * const finit_module_names[] = {
	"__arm64_sys_finit_module",
	"sys_finit_module",
	NULL,
};

static struct shadow_hook finit_module_hook =
	SHADOW_HOOK(finit_module_names, hotreload_hook_finit_module, &real_finit_module);
static struct shadow_hook *hotreload_hooks[] = {
	&finit_module_hook,
	NULL,
};

static bool lkm4ctr_hotreload_hooks_installed;

/*
 * hotreload_hook_finit_module() - purely observational: never denies or
 * redirects the call, only logs when the fd argument's path matches the
 * currently-in-progress hot reload's target (basename compare, since the
 * caller may pass a different-but-equivalent path to the same file). See
 * the file header's step 2.
 */
static long hotreload_hook_finit_module(const struct pt_regs *regs)
{
	int fd = (int)regs_get_kernel_argument((struct pt_regs *)regs, 0);
	struct fd f;
	char *pathbuf;
	const char *path;
	const char *target_base, *path_base;

	f = fdget(fd);
	if (fd_empty(f))
		goto out_call;

	pathbuf = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!pathbuf)
		goto out_fdput;

	path = d_path(&fd_file(f)->f_path, pathbuf, PATH_MAX);
	if (IS_ERR(path))
		goto out_free;

	mutex_lock(&lkm4ctr_hotreload_lock);
	if (lkm4ctr_hotreload_state == LKM4CTR_HOTRELOAD_RUNNING &&
	    lkm4ctr_hotreload_target[0]) {
		target_base = kbasename(lkm4ctr_hotreload_target);
		path_base = kbasename(path);
		if (!strcmp(target_base, path_base))
			LKM4CTR_INFO(LKM4CTR_HOTRELOAD_TAG,
				     "observed finit_module(2) loading \"%s\" (matches the in-progress hot reload's target basename \"%s\"): the kernel is now reloading this module's code from disk",
				     path, target_base);
		else
			LKM4CTR_LOG(LKM4CTR_HOTRELOAD_TAG,
				    "observed finit_module(2) loading \"%s\" (unrelated to the in-progress hot reload's target \"%s\")",
				    path, lkm4ctr_hotreload_target);
	}
	mutex_unlock(&lkm4ctr_hotreload_lock);

out_free:
	kfree(pathbuf);
out_fdput:
	fdput(f);
out_call:
	return real_finit_module(regs);
}

static void lkm4ctr_hotreload_hooks_ensure_installed(void)
{
	int hooked;

	if (lkm4ctr_hotreload_hooks_installed)
		return;

	hooked = shadow_hook_install_all(hotreload_hooks, LKM4CTR_HOTRELOAD_TAG);
	if (hooked < 0) {
		LKM4CTR_WARN(LKM4CTR_HOTRELOAD_TAG,
			     "could not install the observational finit_module(2) hook (%d); hot reload will still work, just without that one log line",
			     hooked);
		return;
	}
	lkm4ctr_hotreload_hooks_installed = true;
	LKM4CTR_INFO(LKM4CTR_HOTRELOAD_TAG,
		     "installed observational finit_module(2) hook (%d candidate matched)", hooked);
}

/* ------------------------------------------------------------------- */
/* path validation                                                      */
/* ------------------------------------------------------------------- */

/*
 * lkm4ctr_hotreload_path_is_safe() - @path is spliced verbatim into a
 * generated `/bin/sh -c '...'` command line by
 * lkm4ctr_hotreload_launch_handoff() below, so it must not contain
 * anything a shell would treat specially. Restrict to the plain,
 * conservative charset any real .ko path needs: alnum, '/', '.', '_', '-'.
 * Rejecting everything else (spaces, quotes, `;&|$<>`(){}[]*?~!#, ...)
 * is deliberately stricter than a real filesystem path could be, in
 * exchange for never needing shell-quoting logic here at all.
 */
static bool lkm4ctr_hotreload_path_is_safe(const char *path)
{
	size_t len = strlen(path);
	size_t i;

	if (len == 0 || len >= LKM4CTR_HOTRELOAD_PATH_MAX)
		return false;
	if (path[0] != '/')
		return false;
	if (len < 4 || strcmp(path + len - 3, ".ko"))
		return false;

	for (i = 0; i < len; i++) {
		char c = path[i];

		if (isalnum((unsigned char)c))
			continue;
		if (c == '/' || c == '.' || c == '_' || c == '-')
			continue;
		return false;
	}
	return true;
}

typedef struct file *(*lkm4ctr_hotreload_filp_open_t)(const char *, int, umode_t);

static int lkm4ctr_hotreload_check_exists(const char *path)
{
	static lkm4ctr_hotreload_filp_open_t filp_open_fn;
	lkm4ctr_hotreload_filp_open_t fn = READ_ONCE(filp_open_fn);
	struct file *filp;
	int ret = 0;

	if (!fn) {
		mutex_lock(&lkm4ctr_hotreload_lock);
		fn = READ_ONCE(filp_open_fn);
		if (!fn) {
			fn = (lkm4ctr_hotreload_filp_open_t)
				shadow_hook_resolve("filp_open");
			if (fn)
				WRITE_ONCE(filp_open_fn, fn);
		}
		mutex_unlock(&lkm4ctr_hotreload_lock);
	}
	if (!fn) {
		LKM4CTR_WARN(LKM4CTR_HOTRELOAD_TAG,
			     "could not resolve filp_open; rejecting do-hot-reload path \"%s\"",
			     path);
		return -ENOSYS;
	}

	filp = fn(path, O_RDONLY, 0);
	if (IS_ERR(filp))
		return PTR_ERR(filp);
	if (!S_ISREG(file_inode(filp)->i_mode))
		ret = -EINVAL;
	filp_close(filp, NULL);
	return ret;
}

/* ------------------------------------------------------------------- */
/* worker thread                                                       */
/* ------------------------------------------------------------------- */

typedef int (*lkm4ctr_hotreload_call_usermodehelper_t)(const char *path, char **argv,
							char **envp, int wait);
static int (*lkm4ctr_hotreload_module_refcount_fn)(struct module *mod);
static lkm4ctr_hotreload_call_usermodehelper_t lkm4ctr_hotreload_call_usermodehelper_fn;

static bool lkm4ctr_hotreload_resolve_symbols(void)
{
	if (!lkm4ctr_hotreload_module_refcount_fn)
		lkm4ctr_hotreload_module_refcount_fn =
			(int (*)(struct module *))shadow_hook_resolve("module_refcount");
	if (!lkm4ctr_hotreload_call_usermodehelper_fn)
		lkm4ctr_hotreload_call_usermodehelper_fn =
			(lkm4ctr_hotreload_call_usermodehelper_t)
			shadow_hook_resolve("call_usermodehelper");

	return lkm4ctr_hotreload_module_refcount_fn &&
	       lkm4ctr_hotreload_call_usermodehelper_fn;
}

static void lkm4ctr_hotreload_umount_diagfs(void)
{
	char *envp[] = { "HOME=/", "PATH=/sbin:/usr/sbin:/bin:/usr/bin:/system/bin", NULL };
	static const char * const candidates[] = {
		"/system/bin/umount", "/sbin/umount", "/usr/sbin/umount",
		"/usr/bin/umount", "/bin/umount", NULL,
	};
	const char * const *path;

	for (path = candidates; *path; path++) {
		char *argv[] = { (char *)*path, "-l", "-a", "-t", "lkm4ctr", NULL };
		int ret = lkm4ctr_hotreload_call_usermodehelper_fn(*path, argv, envp,
								   UMH_WAIT_PROC);
		if (ret == 0) {
			LKM4CTR_INFO(LKM4CTR_HOTRELOAD_TAG,
				     "ran \"%s -l -a -t lkm4ctr\" before handoff", *path);
			return;
		}
	}
	LKM4CTR_WARN(LKM4CTR_HOTRELOAD_TAG,
		     "no working umount found; any active lkm4ctr diagfs mount may block the handoff's rmmod");
}

/*
 * lkm4ctr_hotreload_launch_handoff() - the one piece of userspace this
 * module ever hands final control to. Retries `rmmod lkm4ctr` in a bounded
 * loop (this worker's own module reference may still be dropping when the
 * shell's first attempt runs) and only runs `insmod <path> hotreload=1`
 * once that succeeds, so the two module instances are never both present
 * (which insmod would refuse: a module named "lkm4ctr" is already loaded).
 */
static int lkm4ctr_hotreload_launch_handoff(const char *path)
{
	char *envp[] = { "HOME=/", "PATH=/sbin:/usr/sbin:/bin:/usr/bin:/system/bin", NULL };
	static const char * const shells[] = {
		"/system/bin/sh", "/bin/sh", "/usr/bin/sh", NULL,
	};
	const char * const *shell;
	char *script;
	int ret = -ENOENT;

	script = kasprintf(GFP_KERNEL,
			   "i=0; while ! rmmod lkm4ctr 2>/dev/null; do "
			   "i=$((i+1)); if [ $i -ge 100 ]; then exit 1; fi; "
			   "sleep 0.1 2>/dev/null || sleep 1; done; "
			   "exec insmod %s hotreload=1",
			   path);
	if (!script)
		return -ENOMEM;

	for (shell = shells; *shell; shell++) {
		char *argv[] = { (char *)*shell, "-c", script, NULL };

		ret = lkm4ctr_hotreload_call_usermodehelper_fn(*shell, argv, envp,
							       UMH_WAIT_EXEC);
		if (ret == 0) {
			LKM4CTR_INFO(LKM4CTR_HOTRELOAD_TAG,
				     "launched detached handoff: \"%s -c '%s'\"",
				     *shell, script);
			break;
		}
	}

	kfree(script);
	return ret;
}

static int lkm4ctr_hotreload_worker(void *unused)
{
	unsigned long waited_ms = 0;
	char target[LKM4CTR_HOTRELOAD_PATH_MAX];
	int refcount;
	int ret;

	(void)unused;

	mutex_lock(&lkm4ctr_hotreload_lock);
	strscpy(target, lkm4ctr_hotreload_target, sizeof(target));
	mutex_unlock(&lkm4ctr_hotreload_lock);

	__module_get(THIS_MODULE);

	LKM4CTR_INFO(LKM4CTR_HOTRELOAD_TAG,
		     "hot reload started for \"%s\": quiescing hooks", target);

	lkm4ctr_hotreload_hooks_ensure_installed();
	shadow_hook_quiesce(true);

	if (!lkm4ctr_hotreload_resolve_symbols()) {
		LKM4CTR_ERR(LKM4CTR_HOTRELOAD_TAG,
			    "could not resolve module_refcount/call_usermodehelper; aborting hot reload");
		shadow_hook_quiesce(false);
		mutex_lock(&lkm4ctr_hotreload_lock);
		lkm4ctr_hotreload_state = LKM4CTR_HOTRELOAD_FAILED;
		strscpy(lkm4ctr_hotreload_fail_reason,
			"could not resolve module_refcount/call_usermodehelper",
			sizeof(lkm4ctr_hotreload_fail_reason));
		mutex_unlock(&lkm4ctr_hotreload_lock);
		module_put(THIS_MODULE);
		return 0;
	}

	refcount = lkm4ctr_hotreload_module_refcount_fn(THIS_MODULE);
	LKM4CTR_INFO(LKM4CTR_HOTRELOAD_TAG,
		     "module_refcount()=%d after quiesce (%d in-flight shadow_hook call(s) tracked); waiting up to %ums for it to drain to 1",
		     refcount, shadow_hook_inflight_count(),
		     LKM4CTR_HOTRELOAD_DRAIN_TIMEOUT_MS);

	while ((refcount = lkm4ctr_hotreload_module_refcount_fn(THIS_MODULE)) > 1 &&
	       waited_ms < LKM4CTR_HOTRELOAD_DRAIN_TIMEOUT_MS) {
		if (waited_ms && waited_ms % 5000 == 0)
			LKM4CTR_INFO(LKM4CTR_HOTRELOAD_TAG,
				     "still waiting after %lums: module_refcount()=%d",
				     waited_ms, refcount);
		msleep(LKM4CTR_HOTRELOAD_DRAIN_POLL_MS);
		waited_ms += LKM4CTR_HOTRELOAD_DRAIN_POLL_MS;
	}
	if (refcount > 1)
		LKM4CTR_WARN(LKM4CTR_HOTRELOAD_TAG,
			     "proceeding despite module_refcount()=%d after %lums (opt-in operator action, not a safety-gated unload): the handoff shell's rmmod retry loop will keep trying",
			     refcount, waited_ms);
	else
		LKM4CTR_INFO(LKM4CTR_HOTRELOAD_TAG,
			     "drained after %lums (module_refcount()=%d)",
			     waited_ms, refcount);

	lkm4ctr_hotreload_umount_diagfs();

	ret = lkm4ctr_hotreload_launch_handoff(target);
	if (ret) {
		LKM4CTR_ERR(LKM4CTR_HOTRELOAD_TAG,
			    "could not launch the handoff shell (%d); no shell (sh) found on this system. module left quiesced but loaded -- run `rmmod lkm4ctr && insmod %s hotreload=1` by hand",
			    ret, target);
		shadow_hook_quiesce(false);
		mutex_lock(&lkm4ctr_hotreload_lock);
		lkm4ctr_hotreload_state = LKM4CTR_HOTRELOAD_FAILED;
		scnprintf(lkm4ctr_hotreload_fail_reason,
			  sizeof(lkm4ctr_hotreload_fail_reason),
			  "could not launch handoff shell: %d", ret);
		mutex_unlock(&lkm4ctr_hotreload_lock);
		module_put(THIS_MODULE);
		return 0;
	}

	LKM4CTR_INFO(LKM4CTR_HOTRELOAD_TAG,
		     "handoff launched successfully; this instance is unloading, the new instance will report itself as hot-reloaded once it loads");

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 17, 0)
	module_put_and_kthread_exit(0);
#else
	lkm4ctr_module_put_and_exit(0);
#endif
	return 0;
}

int lkm4ctr_hotreload_trigger(const char *path)
{
	struct task_struct *thread;
	int ret;

	if (!lkm4ctr_hotreload_path_is_safe(path)) {
		LKM4CTR_WARN(LKM4CTR_HOTRELOAD_TAG,
			     "rejected do-hot-reload path (must be an absolute \"*.ko\" path using only [A-Za-z0-9/._-]): \"%s\"",
			     path);
		return -EINVAL;
	}

	ret = lkm4ctr_hotreload_check_exists(path);
	if (ret) {
		LKM4CTR_WARN(LKM4CTR_HOTRELOAD_TAG,
			     "rejected do-hot-reload path \"%s\": could not open as a regular file (%d)",
			     path, ret);
		return ret;
	}

	mutex_lock(&lkm4ctr_hotreload_lock);
	if (lkm4ctr_hotreload_state == LKM4CTR_HOTRELOAD_RUNNING) {
		mutex_unlock(&lkm4ctr_hotreload_lock);
		LKM4CTR_WARN(LKM4CTR_HOTRELOAD_TAG,
			     "a hot reload is already in progress; ignoring new request for \"%s\"",
			     path);
		return -EBUSY;
	}
	strscpy(lkm4ctr_hotreload_target, path, sizeof(lkm4ctr_hotreload_target));
	lkm4ctr_hotreload_state = LKM4CTR_HOTRELOAD_RUNNING;
	lkm4ctr_hotreload_fail_reason[0] = '\0';
	mutex_unlock(&lkm4ctr_hotreload_lock);

	LKM4CTR_INFO(LKM4CTR_HOTRELOAD_TAG,
		     "hot reload requested via diagfs for \"%s\"", path);

	thread = kthread_run(lkm4ctr_hotreload_worker, NULL, "lkm4ctr_hotreload");
	if (IS_ERR(thread)) {
		ret = PTR_ERR(thread);
		mutex_lock(&lkm4ctr_hotreload_lock);
		lkm4ctr_hotreload_state = LKM4CTR_HOTRELOAD_FAILED;
		scnprintf(lkm4ctr_hotreload_fail_reason,
			  sizeof(lkm4ctr_hotreload_fail_reason),
			  "kthread_run() failed: %d", ret);
		mutex_unlock(&lkm4ctr_hotreload_lock);
		LKM4CTR_ERR(LKM4CTR_HOTRELOAD_TAG, "kthread_run() failed: %d", ret);
		return ret;
	}
	lkm4ctr_hotreload_thread = thread;
	return 0;
}

size_t lkm4ctr_hotreload_status_snprintf(char *buf, size_t buflen)
{
	size_t pos = 0;
	enum lkm4ctr_hotreload_state state;
	char target[LKM4CTR_HOTRELOAD_PATH_MAX];
	char reason[sizeof(lkm4ctr_hotreload_fail_reason)];

	mutex_lock(&lkm4ctr_hotreload_lock);
	state = lkm4ctr_hotreload_state;
	strscpy(target, lkm4ctr_hotreload_target, sizeof(target));
	strscpy(reason, lkm4ctr_hotreload_fail_reason, sizeof(reason));
	mutex_unlock(&lkm4ctr_hotreload_lock);

	pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
			 "this instance: %s\n",
			 lkm4ctr_hotreload_is_reloaded_boot ?
			 "hot-reloaded from a previous instance" : "first load");

	switch (state) {
	case LKM4CTR_HOTRELOAD_IDLE:
		pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
				 "current operation: idle\n");
		break;
	case LKM4CTR_HOTRELOAD_RUNNING:
		pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
				 "current operation: reloading (target: %s)\n", target);
		break;
	case LKM4CTR_HOTRELOAD_FAILED:
		pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
				 "current operation: failed (target: %s, reason: %s)\n",
				 target, reason);
		break;
	}
	return pos;
}

int lkm4ctr_hotreload_init(void)
{
	LKM4CTR_INFO(LKM4CTR_HOTRELOAD_TAG,
		     "loaded: %s (write an absolute /path/to/new/lkm4ctr.ko to global/hotreload/do-hot-reload to hot reload)",
		     lkm4ctr_hotreload_is_reloaded_boot ?
		     "this is a hot-reloaded instance" : "this is a first load");
	return 0;
}

void lkm4ctr_hotreload_exit(void)
{
	if (lkm4ctr_hotreload_hooks_installed) {
		shadow_hook_remove_all(hotreload_hooks);
		lkm4ctr_hotreload_hooks_installed = false;
	}
}
