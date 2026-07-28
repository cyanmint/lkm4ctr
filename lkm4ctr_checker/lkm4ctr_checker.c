// SPDX-License-Identifier: GPL-2.0
/*
 * lkm4ctr_checker - userspace diagnostic tool for the lkm4ctr module
 * family.
 *
 * This used to be a small kernel module that read back its own compile-time
 * IS_ENABLED(CONFIG_*) view of the target kernel. That could only ever prove
 * what the kernel was *built* to support, never what actually happens at
 * runtime once lkm4ctr's hooked subsystems (or their absence) are in the
 * loop - which is exactly what matters when
 * diagnosing a real container-start failure such as:
 *
 *   failed to create task for container: failed to create shim task: OCI
 *   runtime create failed: runc create failed: unable to start container
 *   process: can't get final child's PID from pipe: EOF; runc init error(s):
 *   nsexec-1[19437]: failed to unshare remaining namespaces: Invalid
 *   argument; nsexec-0[19436]: failed to sync with stage-1: next state (got
 *   0 of 4 bytes): unknown
 *
 * This tool is a plain userspace binary (no kernel module, no /dev node): it
 * directly performs the same system calls runc's nsexec does
 * (unshare/clone/fork + setns), observes their *actual effect* (not just
 * whether the syscall returned 0), and prints one PASS/FAIL/STUB line per
 * feature:
 *
 *   PASS  - the syscall succeeded AND a real, observable side effect proves
 *           genuine isolation (e.g. a child's hostname change after
 *           unshare(CLONE_NEWUTS) is NOT visible to the parent).
 *   STUB  - the syscall succeeded but no isolation was actually observed
 *           (bookkeeping-only fallback: bit accepted, nothing isolated).
 *   FAIL  - the syscall itself failed (e.g. EINVAL/ENOSYS/EPERM) - this is
 *           the exact failure mode runc's nsexec surfaces as "failed to
 *           unshare remaining namespaces: Invalid argument".
 *
 * Every syscall requiring CLONE_NEW* is attempted inside a throwaway
 * fork(2)'d child, so a FAIL/STUB result never disturbs this process (or the
 * calling shell)'s own namespaces.
 *
 * Usage:
 *   lkm4ctr_checker            # human-readable report to stdout
 *   lkm4ctr_checker -q         # same, but exit status reflects overall
 *                                 # result (0 = all PASS, 1 = any FAIL)
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/msg.h>
#include <sys/ptrace.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>

#if __has_include(<mqueue.h>)
#include <mqueue.h>
#define SHADOW_CHECKER_HAVE_MQUEUE 1
#endif

#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif

#ifndef HOST_NAME_MAX
#define HOST_NAME_MAX 64
#endif
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifndef CLONE_NEWNS
#define CLONE_NEWNS 0x00020000
#endif
#ifndef CLONE_NEWUTS
#define CLONE_NEWUTS 0x04000000
#endif
#ifndef CLONE_NEWIPC
#define CLONE_NEWIPC 0x08000000
#endif
#ifndef CLONE_NEWUSER
#define CLONE_NEWUSER 0x10000000
#endif
#ifndef CLONE_NEWPID
#define CLONE_NEWPID 0x20000000
#endif
#ifndef CLONE_NEWNET
#define CLONE_NEWNET 0x40000000
#endif
#ifndef CLONE_NEWCGROUP
#define CLONE_NEWCGROUP 0x02000000
#endif

#define LKM4CTR_CHECKER_VERSION "4.0"

enum shadow_checker_result {
	SHADOW_CHECKER_PASS = 0,
	SHADOW_CHECKER_STUB,
	SHADOW_CHECKER_FAIL,
	SHADOW_CHECKER_SKIP,
};

static const char *const shadow_checker_result_str[] = {
	[SHADOW_CHECKER_PASS] = "PASS",
	[SHADOW_CHECKER_STUB] = "STUB",
	[SHADOW_CHECKER_FAIL] = "FAIL",
	[SHADOW_CHECKER_SKIP] = "SKIP",
};

static int g_fail_count;
static int g_stub_count;
static bool g_quiet;

static void shadow_checker_report(const char *label,
				   enum shadow_checker_result result,
				   const char *fmt, ...)
{
	char detail[256] = "";

	if (fmt) {
		va_list args;

		va_start(args, fmt);
		vsnprintf(detail, sizeof(detail), fmt, args);
		va_end(args);
	}

	if (result == SHADOW_CHECKER_FAIL)
		g_fail_count++;
	else if (result == SHADOW_CHECKER_STUB)
		g_stub_count++;

	if (g_quiet)
		return;

	if (detail[0])
		printf("%-28s %-4s  %s\n", label,
		       shadow_checker_result_str[result], detail);
	else
		printf("%-28s %-4s\n", label,
		       shadow_checker_result_str[result]);
}

/*
 * Reads a single "\n"-terminated status line (either "OK <payload>" or
 * "ERR <errno>") back from a child process through a pipe. Returns true and
 * fills ok/payload on success, false on a broken pipe / malformed message.
 */
struct shadow_checker_msg {
	bool ok;
	int err;
	char payload[128];
};

static bool shadow_checker_read_msg(int fd, struct shadow_checker_msg *out)
{
	char buf[256];
	ssize_t n;

	memset(out, 0, sizeof(*out));
	n = read(fd, buf, sizeof(buf) - 1);
	if (n <= 0)
		return false;
	buf[n] = '\0';
	if (n > 0 && buf[n - 1] == '\n')
		buf[n - 1] = '\0';

	if (!strncmp(buf, "OK ", 3)) {
		out->ok = true;
		/* Explicit precision (in addition to the destination size
		 * snprintf() already respects) purely to silence a
		 * -Wformat-truncation false positive: snprintf() always
		 * truncates+NUL-terminates safely on its own.
		 */
		snprintf(out->payload, sizeof(out->payload), "%.*s",
			 (int)sizeof(out->payload) - 1, buf + 3);
	} else if (!strncmp(buf, "ERR ", 4)) {
		out->ok = false;
		out->err = atoi(buf + 4);
	} else {
		return false;
	}
	return true;
}

static void shadow_checker_send_ok(int fd, const char *fmt, ...)
{
	char buf[256];
	va_list args;
	int n;

	n = snprintf(buf, sizeof(buf), "OK ");
	va_start(args, fmt);
	n += vsnprintf(buf + n, sizeof(buf) - n, fmt, args);
	va_end(args);
	buf[n] = '\n';
	if (write(fd, buf, n + 1) < 0) {
		/* best effort; parent side treats a closed pipe as SKIP */
	}
}

static void shadow_checker_send_err(int fd, int err)
{
	char buf[64];
	int n = snprintf(buf, sizeof(buf), "ERR %d\n", err);

	if (write(fd, buf, n) < 0) {
		/* best effort; nothing to recover here */
	}
}

/*
 * Build a collision-resistant name of the form "<prefix>-<pid>-<nsec-hex>".
 * Uses CLOCK_MONOTONIC at nanosecond resolution (rather than time(NULL),
 * which only has 1-second resolution) so that two invocations of the same
 * test in quick succession -- e.g. a PID reused across rapid re-runs within
 * the same wall-clock second -- can never collide on the generated name.
 * This matters for tests like mqueue that use O_EXCL and would otherwise
 * intermittently fail with EEXIST.
 */
static void shadow_checker_unique_name(char *out, size_t out_len,
					const char *prefix)
{
	struct timespec ts = { 0, 0 };

	clock_gettime(CLOCK_MONOTONIC, &ts);
	snprintf(out, out_len, "%s-%d-%lx-%08lx", prefix, getpid(),
		 (unsigned long)ts.tv_sec, (unsigned long)ts.tv_nsec);
}

/*
 * ---------------------------------------------------------------------
 * UTS namespace: unshare(CLONE_NEWUTS), then set a unique hostname inside
 * the child. Real isolation means the parent's own gethostname() never
 * observes the child's change; bookkeeping-only (or no isolation at all)
 * means it does.
 * ---------------------------------------------------------------------
 */
static void shadow_checker_uts(void)
{
	char before[HOST_NAME_MAX + 1] = "";
	char after[HOST_NAME_MAX + 1] = "";
	char newname[64];
	int pipefd[2];
	pid_t pid;
	struct shadow_checker_msg msg;

	if (gethostname(before, sizeof(before) - 1))
		before[0] = '\0';

	if (pipe(pipefd)) {
		shadow_checker_report("ns_uts (UTS)", SHADOW_CHECKER_SKIP,
				       "pipe() failed: %s", strerror(errno));
		return;
	}

	pid = fork();
	if (pid < 0) {
		shadow_checker_report("ns_uts (UTS)", SHADOW_CHECKER_SKIP,
				       "fork() failed: %s", strerror(errno));
		close(pipefd[0]);
		close(pipefd[1]);
		return;
	}

	if (pid == 0) {
		close(pipefd[0]);
		if (unshare(CLONE_NEWUTS)) {
			shadow_checker_send_err(pipefd[1], errno);
			_exit(0);
		}
		shadow_checker_unique_name(newname, sizeof(newname), "shadowchk");
		if (sethostname(newname, strlen(newname))) {
			shadow_checker_send_err(pipefd[1], errno);
			_exit(0);
		}
		/* Signal parent that the child's hostname has been changed;
		 * parent samples its own hostname now, before we exit.
		 */
		shadow_checker_send_ok(pipefd[1], "%s", newname);
		_exit(0);
	}

	close(pipefd[1]);
	if (!shadow_checker_read_msg(pipefd[0], &msg)) {
		shadow_checker_report("ns_uts (UTS)", SHADOW_CHECKER_SKIP,
				       "child produced no result");
		close(pipefd[0]);
		waitpid(pid, NULL, 0);
		return;
	}
	close(pipefd[0]);
	waitpid(pid, NULL, 0);

	if (!msg.ok) {
		shadow_checker_report("ns_uts (UTS)", SHADOW_CHECKER_FAIL,
				      "unshare(CLONE_NEWUTS): %s",
				      strerror(msg.err));
		return;
	}

	if (gethostname(after, sizeof(after) - 1))
		after[0] = '\0';

	if (!strcmp(before, after))
		shadow_checker_report("ns_uts (UTS)", SHADOW_CHECKER_PASS,
				      "child set '%s', parent still '%s'",
				      msg.payload, after);
	else
		shadow_checker_report("ns_uts (UTS)", SHADOW_CHECKER_STUB,
				      "child's hostname change leaked to parent ('%s')",
				      after);
}

/*
 * ---------------------------------------------------------------------
 * PID namespace: unshare(CLONE_NEWPID) does NOT move the calling task; only
 * a subsequently forked child joins the new pid namespace. A genuinely new,
 * empty pid namespace always numbers its very first task as pid 1 (as
 * observed by that task's own getpid()). If the grandchild does not see
 * itself as pid 1, no real isolation happened.
 * ---------------------------------------------------------------------
 */
static void shadow_checker_pid(void)
{
	int pipefd[2];
	pid_t pid;
	pid_t main_pid = getpid();
	struct shadow_checker_msg msg;
	char vpidbuf[32];
	int proc_self_ok = -1, proc_hidden_ok = -1, proc_listing_ok = -1;
	int have_fresh_proc = 0;

	if (pipe(pipefd)) {
		shadow_checker_report("ns_pid (PID)", SHADOW_CHECKER_SKIP,
				       "pipe() failed: %s", strerror(errno));
		return;
	}

	pid = fork();
	if (pid < 0) {
		shadow_checker_report("ns_pid (PID)", SHADOW_CHECKER_SKIP,
				       "fork() failed: %s", strerror(errno));
		close(pipefd[0]);
		close(pipefd[1]);
		return;
	}

	if (pid == 0) {
		pid_t grandchild;

		close(pipefd[0]);
		if (unshare(CLONE_NEWPID)) {
			shadow_checker_send_err(pipefd[1], errno);
			_exit(0);
		}

		grandchild = fork();
		if (grandchild < 0) {
			shadow_checker_send_err(pipefd[1], errno);
			_exit(0);
		}
		if (grandchild == 0) {
			/*
			 * /proc isolation check (see
			 * glue/vendor_kernel_procfs.c): from inside the new
			 * PID namespace, /proc/<own vpid> must resolve, the
			 * outer checker process's real, still-running host
			 * pid must NOT be visible via /proc/<main_pid>, and
			 * readdir("/proc") must reflect the same hide/rename.
			 *
			 * On a genuine CONFIG_PID_NS kernel this requires a
			 * *fresh* procfs mount bound to the new namespace --
			 * exactly what runc/containerd do after
			 * unshare(CLONE_NEWPID) -- otherwise the ambient
			 * host /proc mount (inherited unchanged) still shows
			 * the host's own pid namespace regardless of the new
			 * pid namespace. vendor_kernel's procfs fallback
			 * hooks intercept /proc access independent of which
			 * mount instance is used, so this remount is
			 * harmless from their point of view too.
			 */
			pid_t vpid = getpid();
			char path[64];
			int fd, have_fresh_proc;
			DIR *d;

			have_fresh_proc =
				(unshare(CLONE_NEWNS) == 0 &&
				 mount(NULL, "/", NULL,
				       MS_REC | MS_PRIVATE, NULL) == 0 &&
				 mount("proc", "/proc", "proc", 0, NULL) == 0);

			snprintf(path, sizeof(path), "/proc/%d", (int)vpid);
			fd = open(path, O_RDONLY | O_DIRECTORY);
			proc_self_ok = fd >= 0;
			if (fd >= 0)
				close(fd);

			snprintf(path, sizeof(path), "/proc/%d",
				 (int)main_pid);
			fd = open(path, O_RDONLY | O_DIRECTORY);
			proc_hidden_ok = (fd < 0 && errno == ENOENT);
			if (fd >= 0)
				close(fd);

			d = opendir("/proc");
			if (d) {
				struct dirent *de;
				bool saw_self = false, saw_outer = false;
				char selfbuf[16];

				snprintf(selfbuf, sizeof(selfbuf), "%d",
					 (int)vpid);
				while ((de = readdir(d)) != NULL) {
					char *end;
					long v;

					if (!strcmp(de->d_name, selfbuf))
						saw_self = true;
					v = strtol(de->d_name, &end, 10);
					if (*de->d_name && !*end &&
					    v == (long)main_pid)
						saw_outer = true;
				}
				closedir(d);
				proc_listing_ok = saw_self && !saw_outer;
			}

			shadow_checker_send_ok(pipefd[1], "%d;%d;%d;%d;%d",
						vpid, proc_self_ok,
						proc_hidden_ok, proc_listing_ok,
						have_fresh_proc);
			_exit(0);
		}
		waitpid(grandchild, NULL, 0);
		_exit(0);
	}

	close(pipefd[1]);
	if (!shadow_checker_read_msg(pipefd[0], &msg)) {
		shadow_checker_report("ns_pid (PID)", SHADOW_CHECKER_SKIP,
				       "child produced no result");
		close(pipefd[0]);
		waitpid(pid, NULL, 0);
		return;
	}
	close(pipefd[0]);
	waitpid(pid, NULL, 0);

	if (!msg.ok) {
		shadow_checker_report("ns_pid (PID)", SHADOW_CHECKER_FAIL,
				      "unshare(CLONE_NEWPID): %s",
				      strerror(msg.err));
		return;
	}

	if (sscanf(msg.payload, "%31[^;];%d;%d;%d;%d", vpidbuf, &proc_self_ok,
		   &proc_hidden_ok, &proc_listing_ok, &have_fresh_proc) != 5) {
		/* Older/unexpected payload shape: keep pre-/proc-check
		 * behaviour for the PID result itself.
		 */
		strncpy(vpidbuf, msg.payload, sizeof(vpidbuf) - 1);
		vpidbuf[sizeof(vpidbuf) - 1] = '\0';
		proc_self_ok = proc_hidden_ok = proc_listing_ok = -1;
		have_fresh_proc = 0;
	}

	if (strcmp(vpidbuf, "1")) {
		shadow_checker_report("ns_pid (PID)", SHADOW_CHECKER_STUB,
				      "grandchild kept real pid %s (no vpid remap)",
				      vpidbuf);
		shadow_checker_report("ns_pid (/proc isolation)",
				      SHADOW_CHECKER_SKIP,
				      "PID namespace was not isolated; nothing meaningful to check");
		return;
	}

	shadow_checker_report("ns_pid (PID)", SHADOW_CHECKER_PASS,
			      "grandchild became pid 1 in new namespace");

	if (proc_self_ok < 0) {
		shadow_checker_report("ns_pid (/proc isolation)",
				      SHADOW_CHECKER_SKIP,
				      "child produced no /proc-isolation result");
	} else if (!have_fresh_proc) {
		shadow_checker_report("ns_pid (/proc isolation)",
				      SHADOW_CHECKER_SKIP,
				      "couldn't mount a fresh /proc to test (needs CAP_SYS_ADMIN); self=%d hidden=%d listing=%d observed against the ambient /proc mount",
				      proc_self_ok, proc_hidden_ok,
				      proc_listing_ok);
	} else if (!proc_self_ok || !proc_hidden_ok || !proc_listing_ok) {
		shadow_checker_report("ns_pid (/proc isolation)",
				      SHADOW_CHECKER_FAIL,
				      "self=%d hidden=%d listing=%d (want all 1: /proc/1 open, host pid %d hidden, readdir filtered)",
				      proc_self_ok, proc_hidden_ok,
				      proc_listing_ok, (int)main_pid);
	} else {
		shadow_checker_report("ns_pid (/proc isolation)",
				      SHADOW_CHECKER_PASS,
				      "/proc/1 resolves to self; host pid %d hidden from open() and readdir(\"/proc\")",
				      (int)main_pid);
	}
}

/*
 * ---------------------------------------------------------------------
 * shadow_checker_run_as_uid1() - re-run @fn as uid 1 instead of SKIPping
 * a USER-namespace test outright when the checker itself is running as
 * uid 0 (e.g. as PID 1 during early boot, or invoked directly by root).
 *
 * @fn is one of shadow_checker_user()/shadow_checker_user_idmap(): both
 * start by reading getuid() and SKIP immediately if it is 0, since a
 * remap-to-0 can't be told apart from "no remap, still 0" otherwise. A
 * plain in-process setuid(1) would work for the test but would
 * irreversibly drop this process's own root privileges (needed by the
 * mount(2)/CLONE_NEWNS-using tests that run later), so the drop happens
 * in a throwaway fork(2)'d child instead: it calls setuid(1) (real,
 * effective and saved uid all become 1, exactly as if the checker had
 * been started as a normal user) and then simply calls @fn() again,
 * which now observes getuid() == 1 and proceeds with its normal test
 * logic and reporting.
 *
 * @fn's shadow_checker_report() calls happen inside that child and print
 * directly to the (shared) stdout as usual, but increment only the
 * child's own copy of g_fail_count/g_stub_count; the PASS/STUB/FAIL/SKIP
 * outcome is round-tripped back to this (real) process via the child's
 * exit status so the final summary counts stay accurate.
 * ---------------------------------------------------------------------
 */
#define SHADOW_CHECKER_UID1_EXIT_FAIL 1
#define SHADOW_CHECKER_UID1_EXIT_STUB 2

static void shadow_checker_run_as_uid1(const char *label, void (*fn)(void))
{
	int fail_before = g_fail_count, stub_before = g_stub_count;
	pid_t pid;
	int status;

	fflush(stdout);

	pid = fork();
	if (pid < 0) {
		shadow_checker_report(label, SHADOW_CHECKER_SKIP,
				       "fork() failed: %s", strerror(errno));
		return;
	}

	if (pid == 0) {
		int code = 0;

		if (setuid(1)) {
			shadow_checker_report(label, SHADOW_CHECKER_SKIP,
					      "setuid(1) to drop uid 0 for test: %s",
					      strerror(errno));
			fflush(stdout);
			_exit(0);
		}

		fn();

		if (g_fail_count > fail_before)
			code |= SHADOW_CHECKER_UID1_EXIT_FAIL;
		if (g_stub_count > stub_before)
			code |= SHADOW_CHECKER_UID1_EXIT_STUB;
		fflush(stdout);
		_exit(code);
	}

	if (waitpid(pid, &status, 0) == pid && WIFEXITED(status)) {
		int code = WEXITSTATUS(status);

		if (code & SHADOW_CHECKER_UID1_EXIT_FAIL)
			g_fail_count++;
		if (code & SHADOW_CHECKER_UID1_EXIT_STUB)
			g_stub_count++;
	}
}

/*
 * ---------------------------------------------------------------------
 * USER namespace: unshare(CLONE_NEWUSER) always changes the calling task's
 * apparent uid/gid inside the new namespace *before* any uid_map/gid_map is
 * written - either to the overflow uid (genuine, unmapped kernel
 * namespace: typically 65534) or to 0 (the module's container-style
 * single-mapping remap of the creator to root). Both are a real, observable
 * change; only an unchanged uid indicates no isolation at all. This test is
 * only meaningful when run as a non-root user, so if the checker itself is
 * running as uid 0 it re-runs the test as uid 1 instead (see
 * shadow_checker_run_as_uid1()).
 * ---------------------------------------------------------------------
 */
static void shadow_checker_user(void)
{
	uid_t before = getuid();
	int pipefd[2];
	pid_t pid;
	struct shadow_checker_msg msg;

	if (before == 0) {
		shadow_checker_run_as_uid1("ns_user (USER)",
					    shadow_checker_user);
		return;
	}

	if (pipe(pipefd)) {
		shadow_checker_report("ns_user (USER)", SHADOW_CHECKER_SKIP,
				       "pipe() failed: %s", strerror(errno));
		return;
	}

	pid = fork();
	if (pid < 0) {
		shadow_checker_report("ns_user (USER)", SHADOW_CHECKER_SKIP,
				       "fork() failed: %s", strerror(errno));
		close(pipefd[0]);
		close(pipefd[1]);
		return;
	}

	if (pid == 0) {
		close(pipefd[0]);
		if (unshare(CLONE_NEWUSER)) {
			shadow_checker_send_err(pipefd[1], errno);
			_exit(0);
		}
		shadow_checker_send_ok(pipefd[1], "%u", geteuid());
		_exit(0);
	}

	close(pipefd[1]);
	if (!shadow_checker_read_msg(pipefd[0], &msg)) {
		shadow_checker_report("ns_user (USER)", SHADOW_CHECKER_SKIP,
				       "child produced no result");
		close(pipefd[0]);
		waitpid(pid, NULL, 0);
		return;
	}
	close(pipefd[0]);
	waitpid(pid, NULL, 0);

	if (!msg.ok) {
		shadow_checker_report("ns_user (USER)", SHADOW_CHECKER_FAIL,
				      "unshare(CLONE_NEWUSER): %s",
				      strerror(msg.err));
		return;
	}

	if (atoi(msg.payload) != (int)before)
		shadow_checker_report("ns_user (USER)", SHADOW_CHECKER_PASS,
				      "uid remapped %u -> %s inside namespace",
				      before, msg.payload);
	else
		shadow_checker_report("ns_user (USER)", SHADOW_CHECKER_STUB,
				      "uid unchanged (%s) - no remap performed",
				      msg.payload);
}

/*
 * ---------------------------------------------------------------------
 * IPC / NET / MNT / CGROUP namespaces: none of these has a cheap, safe,
 * universal in-process "did isolation really happen" signal the way UTS/
 * PID/USER do. For these four, the remaining caveats live in
 * vendor/README.md, and genuine kernel isolation for
 * them touches subsystems this tool must not perturb - e.g. mounting/
 * networking. So this tool only reports whether the unshare(2) syscall
 * itself succeeds; a successful-but-unverifiable-isolation namespace is
 * still reported STUB unless the child can observe a distinct
 * /proc/self/ns/<type> identity from the parent, which is a reliable
 * indicator that the kernel actually allocated a new namespace object
 * (rather than just accepting the flag and doing nothing).
 * ---------------------------------------------------------------------
 */
static bool shadow_checker_read_ns_id(pid_t pid, const char *type, char *out,
				       size_t outlen)
{
	char path[64];
	ssize_t n;

	if (pid)
		snprintf(path, sizeof(path), "/proc/%d/ns/%s", pid, type);
	else
		snprintf(path, sizeof(path), "/proc/self/ns/%s", type);

	n = readlink(path, out, outlen - 1);
	if (n < 0)
		return false;
	out[n] = '\0';
	return true;
}

static void shadow_checker_generic_ns(const char *label, const char *ns_file,
				       int clone_flag)
{
	char before[128], after[128];
	int pipefd[2];
	pid_t pid;
	struct shadow_checker_msg msg;
	bool have_before;

	have_before = shadow_checker_read_ns_id(0, ns_file, before,
						 sizeof(before));

	if (pipe(pipefd)) {
		shadow_checker_report(label, SHADOW_CHECKER_SKIP,
				       "pipe() failed: %s", strerror(errno));
		return;
	}

	pid = fork();
	if (pid < 0) {
		shadow_checker_report(label, SHADOW_CHECKER_SKIP,
				       "fork() failed: %s", strerror(errno));
		close(pipefd[0]);
		close(pipefd[1]);
		return;
	}

	if (pid == 0) {
		char id[64];

		close(pipefd[0]);
		if (unshare(clone_flag)) {
			shadow_checker_send_err(pipefd[1], errno);
			_exit(0);
		}
		if (!shadow_checker_read_ns_id(0, ns_file, id, sizeof(id)))
			id[0] = '\0';
		shadow_checker_send_ok(pipefd[1], "%s", id);
		_exit(0);
	}

	close(pipefd[1]);
	if (!shadow_checker_read_msg(pipefd[0], &msg)) {
		shadow_checker_report(label, SHADOW_CHECKER_SKIP,
				       "child produced no result");
		close(pipefd[0]);
		waitpid(pid, NULL, 0);
		return;
	}
	close(pipefd[0]);
	waitpid(pid, NULL, 0);

	if (!msg.ok) {
		shadow_checker_report(label, SHADOW_CHECKER_FAIL,
				      "unshare(): %s", strerror(msg.err));
		return;
	}

	snprintf(after, sizeof(after), "%s", msg.payload);

	/*
	 * A distinct post-unshare() namespace identity counts as real
	 * isolation whether or not a "before" baseline existed: on a kernel
	 * genuinely lacking native namespace support for this type (e.g.
	 * CONFIG_IPC_NS=n), /proc/self/ns/<type> may not exist at all before
	 * unshare() (have_before false), so a fabricated identity appearing
	 * afterwards is exactly as meaningful a change as a differing id
	 * would be when a "before" baseline does exist.
	 */
	if (after[0] && (!have_before || strcmp(before, after)))
		shadow_checker_report(label, SHADOW_CHECKER_PASS,
				      "namespace id changed (%s -> %s)",
				      have_before ? before : "(none)", after);
	else
		shadow_checker_report(label, SHADOW_CHECKER_STUB,
				      "unshare() succeeded but namespace id unchanged (bookkeeping only)");
}

/*
 * ---------------------------------------------------------------------
 * POSIX message queues: real functional test (mq_open + send + receive),
 * not just "does mq_open() succeed".
 * ---------------------------------------------------------------------
 */
static void shadow_checker_mqueue(void)
{
#ifdef SHADOW_CHECKER_HAVE_MQUEUE
	char name[64];
	mqd_t mq;
	char msgbuf[16] = "hi";
	char rcvbuf[16] = "";
	struct mq_attr attr = {
		.mq_maxmsg = 4,
		.mq_msgsize = sizeof(msgbuf),
	};

	shadow_checker_unique_name(name, sizeof(name), "/shadowchk-mq");

	/* Best-effort cleanup of a stale queue left behind by a previous run
	 * that crashed before mq_unlink() (e.g. name reused after a PID
	 * wraparound); ignore ENOENT since that is the expected case.
	 */
	mq_unlink(name);

	mq = mq_open(name, O_CREAT | O_RDWR | O_EXCL, 0600, &attr);
	if (mq == (mqd_t)-1) {
		shadow_checker_report("mqueue (POSIX)", SHADOW_CHECKER_FAIL,
				      "mq_open(): %s", strerror(errno));
		return;
	}

	if (mq_send(mq, msgbuf, strlen(msgbuf), 0)) {
		shadow_checker_report("mqueue (POSIX)", SHADOW_CHECKER_FAIL,
				      "mq_send(): %s", strerror(errno));
		mq_close(mq);
		mq_unlink(name);
		return;
	}

	if (mq_receive(mq, rcvbuf, sizeof(rcvbuf), NULL) < 0) {
		shadow_checker_report("mqueue (POSIX)", SHADOW_CHECKER_FAIL,
				      "mq_receive(): %s", strerror(errno));
		mq_close(mq);
		mq_unlink(name);
		return;
	}

	mq_close(mq);
	mq_unlink(name);

	if (!strcmp(rcvbuf, msgbuf))
		shadow_checker_report("mqueue (POSIX)", SHADOW_CHECKER_PASS,
				      "message round-tripped through the queue");
	else
		shadow_checker_report("mqueue (POSIX)", SHADOW_CHECKER_STUB,
				      "queue accepted send/receive but payload mismatched");
#else
	shadow_checker_report("mqueue (POSIX)", SHADOW_CHECKER_SKIP,
			      "<mqueue.h> unavailable in this libc");
#endif
}

/*
 * ---------------------------------------------------------------------
 * System V IPC: real functional test via a message queue (msgget/msgsnd/
 * msgrcv), matching vendor_kernel's hooked SysV IPC path.
 * ---------------------------------------------------------------------
 */
struct shadow_checker_sysv_msg {
	long mtype;
	char mtext[16];
};

static void shadow_checker_sysvipc(void)
{
	int id;
	struct shadow_checker_sysv_msg out = { .mtype = 1, .mtext = "hi" };
	struct shadow_checker_sysv_msg in = { 0 };

	id = msgget(IPC_PRIVATE, IPC_CREAT | 0600);
	if (id < 0) {
		shadow_checker_report("sysvipc (SysV msg)", SHADOW_CHECKER_FAIL,
				      "msgget(): %s", strerror(errno));
		return;
	}

	if (msgsnd(id, &out, strlen(out.mtext) + 1, 0)) {
		shadow_checker_report("sysvipc (SysV msg)", SHADOW_CHECKER_FAIL,
				      "msgsnd(): %s", strerror(errno));
		msgctl(id, IPC_RMID, NULL);
		return;
	}

	if (msgrcv(id, &in, sizeof(in.mtext), 1, 0) < 0) {
		shadow_checker_report("sysvipc (SysV msg)", SHADOW_CHECKER_FAIL,
				      "msgrcv(): %s", strerror(errno));
		msgctl(id, IPC_RMID, NULL);
		return;
	}

	msgctl(id, IPC_RMID, NULL);

	if (!strcmp(in.mtext, out.mtext))
		shadow_checker_report("sysvipc (SysV msg)", SHADOW_CHECKER_PASS,
				      "message round-tripped through msgget/msgsnd/msgrcv");
	else
		shadow_checker_report("sysvipc (SysV msg)", SHADOW_CHECKER_STUB,
				      "queue accepted send/receive but payload mismatched");
}

/*
 * ---------------------------------------------------------------------
 * overlay2: attempt a real mount(2) of an overlay filesystem in a private
 * mount namespace (so nothing leaks onto the host), using throwaway tmpfs
 * dirs for lower/upper/work.
 * ---------------------------------------------------------------------
 */
static void shadow_checker_overlay(void)
{
	char base[] = "/tmp/shadowchk-ovl-XXXXXX";
	char lower[PATH_MAX], upper[PATH_MAX], work[PATH_MAX], merged[PATH_MAX];
	char opts[PATH_MAX * 4];
	int pipefd[2];
	pid_t pid;
	struct shadow_checker_msg msg;

	if (pipe(pipefd)) {
		shadow_checker_report("overlay2", SHADOW_CHECKER_SKIP,
				       "pipe() failed: %s", strerror(errno));
		return;
	}

	pid = fork();
	if (pid < 0) {
		shadow_checker_report("overlay2", SHADOW_CHECKER_SKIP,
				       "fork() failed: %s", strerror(errno));
		close(pipefd[0]);
		close(pipefd[1]);
		return;
	}

	if (pid == 0) {
		close(pipefd[0]);

		/* Isolate into a private mount namespace so the test mount
		 * never leaks onto the host, regardless of pass/fail.
		 */
		if (unshare(CLONE_NEWNS)) {
			shadow_checker_send_err(pipefd[1], errno);
			_exit(0);
		}

		if (!mkdtemp(base)) {
			shadow_checker_send_err(pipefd[1], errno);
			_exit(0);
		}
		snprintf(lower, sizeof(lower), "%s/lower", base);
		snprintf(upper, sizeof(upper), "%s/upper", base);
		snprintf(work, sizeof(work), "%s/work", base);
		snprintf(merged, sizeof(merged), "%s/merged", base);
		if (mkdir(lower, 0700) || mkdir(upper, 0700) ||
		    mkdir(work, 0700) || mkdir(merged, 0700)) {
			shadow_checker_send_err(pipefd[1], errno);
			_exit(0);
		}

		snprintf(opts, sizeof(opts),
			 "lowerdir=%s,upperdir=%s,workdir=%s", lower, upper,
			 work);

		if (mount("overlay", merged, "overlay", 0, opts)) {
			shadow_checker_send_err(pipefd[1], errno);
			_exit(0);
		}

		if (umount2(merged, MNT_DETACH))
			fprintf(stderr,
				"lkm4ctr_checker: warning: umount2(%s) failed: %s\n",
				merged, strerror(errno));
		shadow_checker_send_ok(pipefd[1], "mounted");
		_exit(0);
	}

	close(pipefd[1]);
	if (!shadow_checker_read_msg(pipefd[0], &msg)) {
		shadow_checker_report("overlay2", SHADOW_CHECKER_SKIP,
				       "child produced no result");
		close(pipefd[0]);
		waitpid(pid, NULL, 0);
		return;
	}
	close(pipefd[0]);
	waitpid(pid, NULL, 0);

	if (msg.ok)
		shadow_checker_report("overlay2", SHADOW_CHECKER_PASS,
				      "mount(2) of an overlay filesystem succeeded");
	else if (msg.err == ENODEV || msg.err == ENOENT)
		shadow_checker_report("overlay2", SHADOW_CHECKER_FAIL,
				      "overlay filesystem type not registered: %s",
				      strerror(msg.err));
	else
		shadow_checker_report("overlay2", SHADOW_CHECKER_FAIL,
				      "mount(): %s", strerror(msg.err));
}

/*
 * ---------------------------------------------------------------------
 * USER namespace id mapping: getuid/geteuid/getgid/getegid/getresuid/
 * getresgid after an explicit
 * "0 <real> 1" uid_map/gid_map write - exactly what every container
 * runtime (runc, crun, ...) does before entering the mapped identity, and
 * a strictly stronger check than shadow_checker_user()'s "did the id
 * merely change" test above: this confirms the *entire* getresuid/
 * getresgid family, not just geteuid(), observes the mapped value. Like
 * shadow_checker_user(), re-runs as uid 1 (shadow_checker_run_as_uid1())
 * instead of SKIPping when the checker itself is running as uid 0.
 * ---------------------------------------------------------------------
 */
static void shadow_checker_user_idmap(void)
{
	uid_t before = getuid();
	gid_t gbefore = getgid();
	int pipefd[2];
	pid_t pid;
	struct shadow_checker_msg msg;

	if (before == 0) {
		shadow_checker_run_as_uid1("ns_user (id mapping)",
					    shadow_checker_user_idmap);
		return;
	}

	if (pipe(pipefd)) {
		shadow_checker_report("ns_user (id mapping)", SHADOW_CHECKER_SKIP,
				       "pipe() failed: %s", strerror(errno));
		return;
	}

	pid = fork();
	if (pid < 0) {
		shadow_checker_report("ns_user (id mapping)", SHADOW_CHECKER_SKIP,
				       "fork() failed: %s", strerror(errno));
		close(pipefd[0]);
		close(pipefd[1]);
		return;
	}

	if (pid == 0) {
		char buf[64];
		int fd;
		uid_t ruid, euid, suid;
		gid_t rgid, egid, sgid;

		close(pipefd[0]);
		if (unshare(CLONE_NEWUSER)) {
			shadow_checker_send_err(pipefd[1], errno);
			_exit(0);
		}

		/* Deny setgroups(2) before writing gid_map, as every
		 * runtime does since CVE-2014-8989; harmless no-op if the
		 * fallback ignores it.
		 */
		fd = open("/proc/self/setgroups", O_WRONLY);
		if (fd >= 0) {
			ssize_t ignored = write(fd, "deny", 4);

			(void)ignored;
			close(fd);
		}

		fd = open("/proc/self/uid_map", O_WRONLY);
		if (fd < 0) {
			shadow_checker_send_err(pipefd[1], errno);
			_exit(0);
		}
		snprintf(buf, sizeof(buf), "0 %d 1\n", (int)before);
		if (write(fd, buf, strlen(buf)) < 0) {
			shadow_checker_send_err(pipefd[1], errno);
			close(fd);
			_exit(0);
		}
		close(fd);

		fd = open("/proc/self/gid_map", O_WRONLY);
		if (fd < 0) {
			shadow_checker_send_err(pipefd[1], errno);
			_exit(0);
		}
		snprintf(buf, sizeof(buf), "0 %d 1\n", (int)gbefore);
		if (write(fd, buf, strlen(buf)) < 0) {
			shadow_checker_send_err(pipefd[1], errno);
			close(fd);
			_exit(0);
		}
		close(fd);

		if (getresuid(&ruid, &euid, &suid) ||
		    getresgid(&rgid, &egid, &sgid)) {
			shadow_checker_send_err(pipefd[1], errno);
			_exit(0);
		}
		shadow_checker_send_ok(pipefd[1], "%d;%d;%d;%d;%d;%d;%d;%d",
				       (int)getuid(), (int)ruid, (int)euid,
				       (int)suid, (int)getgid(), (int)rgid,
				       (int)egid, (int)sgid);
		_exit(0);
	}

	close(pipefd[1]);
	if (!shadow_checker_read_msg(pipefd[0], &msg)) {
		shadow_checker_report("ns_user (id mapping)", SHADOW_CHECKER_SKIP,
				       "child produced no result");
		close(pipefd[0]);
		waitpid(pid, NULL, 0);
		return;
	}
	close(pipefd[0]);
	waitpid(pid, NULL, 0);

	if (!msg.ok) {
		if (msg.err == ENOENT)
			shadow_checker_report("ns_user (id mapping)",
					      SHADOW_CHECKER_STUB,
					      "no /proc/self/uid_map or gid_map (no real user namespace object was created)");
		else
			shadow_checker_report("ns_user (id mapping)",
					      SHADOW_CHECKER_FAIL,
					      "uid_map/gid_map setup: %s",
					      strerror(msg.err));
		return;
	}

	{
		int uid, ruid, euid, suid, gid, rgid, egid, sgid;

		if (sscanf(msg.payload, "%d;%d;%d;%d;%d;%d;%d;%d", &uid,
			   &ruid, &euid, &suid, &gid, &rgid, &egid,
			   &sgid) != 8) {
			shadow_checker_report("ns_user (id mapping)",
					      SHADOW_CHECKER_SKIP,
					      "unparsable child result");
			return;
		}

		if (!uid && !ruid && !euid && !suid && !gid && !rgid &&
		    !egid && !sgid)
			shadow_checker_report("ns_user (id mapping)",
					      SHADOW_CHECKER_PASS,
					      "getuid/getgid/getresuid/getresgid all report mapped id 0 (real %u/%u)",
					      before, gbefore);
		else
			shadow_checker_report("ns_user (id mapping)",
					      SHADOW_CHECKER_STUB,
					      "uid_map/gid_map write succeeded but ids unmapped (uid=%d ruid=%d euid=%d suid=%d gid=%d rgid=%d egid=%d sgid=%d)",
					      uid, ruid, euid, suid, gid, rgid,
					      egid, sgid);
	}
}

static volatile sig_atomic_t g_checker_got_signal;

static void shadow_checker_sigusr1_handler(int sig)
{
	(void)sig;
	g_checker_got_signal = 1;
}

/*
 * ---------------------------------------------------------------------
 * PID namespace signal delivery: kill(2) through vendor_kernel's vendored
 * pid-translation path must still
 * deliver a real signal, not just return 0. A grandchild inside the new
 * pid namespace announces its own (namespace-local) pid and blocks for
 * the signal; the namespace's "init" targets that exact pid with kill(2).
 * ---------------------------------------------------------------------
 */
static void shadow_checker_pid_signal(void)
{
	int pipefd[2];
	pid_t pid;
	struct shadow_checker_msg msg;

	if (pipe(pipefd)) {
		shadow_checker_report("ns_pid (kill)", SHADOW_CHECKER_SKIP,
				       "pipe() failed: %s", strerror(errno));
		return;
	}

	pid = fork();
	if (pid < 0) {
		shadow_checker_report("ns_pid (kill)", SHADOW_CHECKER_SKIP,
				       "fork() failed: %s", strerror(errno));
		close(pipefd[0]);
		close(pipefd[1]);
		return;
	}

	if (pid == 0) {
		pid_t grandchild;
		int gpipe[2];

		close(pipefd[0]);
		if (unshare(CLONE_NEWPID)) {
			shadow_checker_send_err(pipefd[1], errno);
			_exit(0);
		}
		if (pipe(gpipe)) {
			shadow_checker_send_err(pipefd[1], errno);
			_exit(0);
		}

		grandchild = fork();
		if (grandchild < 0) {
			shadow_checker_send_err(pipefd[1], errno);
			_exit(0);
		}
		if (grandchild == 0) {
			struct sigaction sa;

			close(gpipe[0]);
			memset(&sa, 0, sizeof(sa));
			sa.sa_handler = shadow_checker_sigusr1_handler;
			sigemptyset(&sa.sa_mask);
			sigaction(SIGUSR1, &sa, NULL);
			shadow_checker_send_ok(gpipe[1], "%d", (int)getpid());
			while (!g_checker_got_signal)
				pause();
			shadow_checker_send_ok(gpipe[1], "received");
			_exit(0);
		}

		close(gpipe[1]);
		{
			struct shadow_checker_msg gmsg;

			if (!shadow_checker_read_msg(gpipe[0], &gmsg) ||
			    !gmsg.ok) {
				shadow_checker_send_err(pipefd[1], EIO);
				close(gpipe[0]);
				waitpid(grandchild, NULL, 0);
				_exit(0);
			}
			/* gmsg's payload is the grandchild's *own* getpid(),
			 * i.e. its namespace-local vpid (always 1, since it
			 * is pid 1 of the new namespace it just unshared
			 * into) -- meaningless as a kill(2) target from here.
			 * unshare(CLONE_NEWPID) only ever affects *future*
			 * children, never the calling task itself, so this
			 * "child" process (the one about to call kill())
			 * was never moved into that namespace and remains in
			 * the same (ambient) pid namespace it already shares
			 * with grandchild -- `grandchild`, fork()'s own
			 * return value above, is already the correct,
			 * real/ambient-namespace pid to target. Using the
			 * reported vpid=1 instead would send SIGUSR1 to
			 * whatever real process happens to be pid 1 in this
			 * task's own ambient namespace rather than to
			 * grandchild, leaving grandchild's pause() loop
			 * below waiting for a signal that never arrives --
			 * and, being pid 1 of its own new namespace, immune
			 * to being torn down by any subsequent unhandled
			 * signal, permanently hanging this test (and every
			 * waitpid() after it).
			 */
			if (kill(grandchild, SIGUSR1)) {
				shadow_checker_send_err(pipefd[1], errno);
				close(gpipe[0]);
				waitpid(grandchild, NULL, 0);
				_exit(0);
			}

			if (!shadow_checker_read_msg(gpipe[0], &gmsg) ||
			    !gmsg.ok || strcmp(gmsg.payload, "received"))
				shadow_checker_send_ok(pipefd[1],
						       "not-delivered");
			else
				shadow_checker_send_ok(pipefd[1], "delivered");
		}
		close(gpipe[0]);
		waitpid(grandchild, NULL, 0);
		_exit(0);
	}

	close(pipefd[1]);
	if (!shadow_checker_read_msg(pipefd[0], &msg)) {
		shadow_checker_report("ns_pid (kill)", SHADOW_CHECKER_SKIP,
				       "child produced no result");
		close(pipefd[0]);
		waitpid(pid, NULL, 0);
		return;
	}
	close(pipefd[0]);
	waitpid(pid, NULL, 0);

	if (!msg.ok) {
		shadow_checker_report("ns_pid (kill)", SHADOW_CHECKER_FAIL,
				      "unshare(CLONE_NEWPID)/kill(): %s",
				      strerror(msg.err));
		return;
	}

	if (!strcmp(msg.payload, "delivered"))
		shadow_checker_report("ns_pid (kill)", SHADOW_CHECKER_PASS,
				      "kill(2) targeting a namespace-local pid delivered SIGUSR1 correctly");
	else
		shadow_checker_report("ns_pid (kill)", SHADOW_CHECKER_FAIL,
				      "kill(2) accepted the pid but the signal was never delivered");
}

/*
 * ---------------------------------------------------------------------
 * PID namespace process-group/session hooks: setpgid/getpgid/getsid
 * exercised from the namespace's own pid-1 task, whose
 * own pid/pgid/sid should all be self-consistent (namespace-local 1)
 * exactly like shadow_checker_pid()'s getpid() check above.
 *
 * A freshly unshare(CLONE_NEWPID)'d task does NOT automatically become its
 * own process group/session leader: its pgid/sid are inherited from before
 * the new pid namespace existed, and since the real leader of that
 * group/session lives outside the new
 * namespace, getpgid(0)/getsid(0) legitimately report 0 there (no vnr
 * exists for a group/session leader outside the namespace) even with a
 * fully genuine vpid remap - this is correct real-kernel behaviour, not a
 * stub. So this test must call setsid(2) first (which makes the calling
 * task both the process group leader and the session leader of a brand
 * new group/session, deterministically getting namespace-local id 1 for
 * both when the vpid remap is real) before reading pgid/sid back, or a
 * genuine remap is misreported as STUB.
 * ---------------------------------------------------------------------
 */
static void shadow_checker_pid_pgrp(void)
{
	int pipefd[2];
	pid_t pid;
	struct shadow_checker_msg msg;

	if (pipe(pipefd)) {
		shadow_checker_report("ns_pid (pgid/session)", SHADOW_CHECKER_SKIP,
				       "pipe() failed: %s", strerror(errno));
		return;
	}

	pid = fork();
	if (pid < 0) {
		shadow_checker_report("ns_pid (pgid/session)", SHADOW_CHECKER_SKIP,
				       "fork() failed: %s", strerror(errno));
		close(pipefd[0]);
		close(pipefd[1]);
		return;
	}

	if (pid == 0) {
		pid_t leader;

		close(pipefd[0]);
		if (unshare(CLONE_NEWPID)) {
			shadow_checker_send_err(pipefd[1], errno);
			_exit(0);
		}

		leader = fork();
		if (leader < 0) {
			shadow_checker_send_err(pipefd[1], errno);
			_exit(0);
		}
		if (leader == 0) {
			pid_t self;
			pid_t pgid, sid;

			/* setsid(2) makes this task the leader of both a
			 * brand new process group and a brand new session -
			 * deterministically namespace-local id 1 for both
			 * when the vpid remap is genuine (see comment
			 * above). Do this before reading pgid/sid back;
			 * reading them beforehand would reflect the
			 * pre-namespace group/session, whose leader lives
			 * outside the new namespace and so has no vnr here.
			 */
			if (setsid() == (pid_t)-1) {
				shadow_checker_send_err(pipefd[1], errno);
				_exit(0);
			}
			self = getpid();
			pgid = getpgid(0);
			sid = getsid(0);
			shadow_checker_send_ok(pipefd[1], "%d;%d;%d",
					       (int)self, (int)pgid,
					       (int)sid);
			_exit(0);
		}
		waitpid(leader, NULL, 0);
		_exit(0);
	}

	close(pipefd[1]);
	if (!shadow_checker_read_msg(pipefd[0], &msg)) {
		shadow_checker_report("ns_pid (pgid/session)", SHADOW_CHECKER_SKIP,
				       "child produced no result");
		close(pipefd[0]);
		waitpid(pid, NULL, 0);
		return;
	}
	close(pipefd[0]);
	waitpid(pid, NULL, 0);

	if (!msg.ok) {
		shadow_checker_report("ns_pid (pgid/session)", SHADOW_CHECKER_FAIL,
				      "setsid/getpgid/getsid: %s",
				      strerror(msg.err));
		return;
	}

	{
		int self, pgid, sid;

		if (sscanf(msg.payload, "%d;%d;%d", &self, &pgid, &sid) != 3) {
			shadow_checker_report("ns_pid (pgid/session)",
					      SHADOW_CHECKER_SKIP,
					      "unparsable child result");
			return;
		}

		if (self == 1 && pgid == 1 && sid == 1)
			shadow_checker_report("ns_pid (pgid/session)",
					      SHADOW_CHECKER_PASS,
					      "pid/pgid/sid of the namespace's own init all report 1");
		else
			shadow_checker_report("ns_pid (pgid/session)",
					      SHADOW_CHECKER_STUB,
					      "no vpid remap (pid=%d pgid=%d sid=%d)",
					      self, pgid, sid);
	}
}

/*
 * ---------------------------------------------------------------------
 * PID namespace /proc content rewriting: vendor_kernel's read(2)/pread64(2)
 * procfs fallback hooks rewrite /proc/<pid>/stat's pid/ppid/pgrp/session
 * fields and /proc/<pid>/status's Pid:/PPid: lines from real to
 * namespace-local virtual values (needed for e.g. `ps` to work correctly
 * inside a container). shadow_checker_pid()'s "/proc isolation" test
 * above only checks whether entries exist/are hidden; this test reads
 * the actual file content of the namespace's own init and confirms every
 * one of those fields reports the virtualized values (pid=1, ppid=0,
 * pgrp=1, session=1), not the real host values.
 * ---------------------------------------------------------------------
 */
static void shadow_checker_pid_procfs_content(void)
{
	int pipefd[2];
	pid_t pid;
	struct shadow_checker_msg msg;

	if (pipe(pipefd)) {
		shadow_checker_report("ns_pid (/proc stat content)",
				      SHADOW_CHECKER_SKIP,
				       "pipe() failed: %s", strerror(errno));
		return;
	}

	pid = fork();
	if (pid < 0) {
		shadow_checker_report("ns_pid (/proc stat content)",
				      SHADOW_CHECKER_SKIP,
				       "fork() failed: %s", strerror(errno));
		close(pipefd[0]);
		close(pipefd[1]);
		return;
	}

	if (pid == 0) {
		pid_t grandchild;

		close(pipefd[0]);
		if (unshare(CLONE_NEWPID)) {
			shadow_checker_send_err(pipefd[1], errno);
			_exit(0);
		}

		grandchild = fork();
		if (grandchild < 0) {
			shadow_checker_send_err(pipefd[1], errno);
			_exit(0);
		}
		if (grandchild == 0) {
			char stat_buf[256] = "";
			char status_buf[1024] = "";
			int stat_fd, status_fd;
			ssize_t n;
			int have_fresh_proc;
			int pid_f = -1, ppid_f = -1, pgrp_f = -1, sess_f = -1;
			int status_pid = -1, status_ppid = -1;
			char *line, *saveptr;

			/* setsid(2) makes this task (the namespace's own
			 * init) the leader of a brand new process group and
			 * session, deterministically namespace-local id 1
			 * for both when the vpid remap is genuine. Without
			 * this, pgrp/session are inherited from before the
			 * new pid namespace existed and their leader lives
			 * outside it, so /proc would legitimately report 0
			 * for both fields even with a fully genuine remap -
			 * see shadow_checker_pid_pgrp()'s comment above for
			 * the same reasoning.
			 */
			if (setsid() == (pid_t)-1) {
				shadow_checker_send_err(pipefd[1], errno);
				_exit(0);
			}

			/* Same private-/proc-remount technique as
			 * shadow_checker_pid(): on a genuine kernel this is
			 * required for /proc to reflect the new pid
			 * namespace at all; vendor_kernel's read()/pread64()
			 * translation hooks work independent of which mount
			 * instance backs /proc, so this remount is a
			 * harmless no-op from their point of view.
			 */
			have_fresh_proc =
				(unshare(CLONE_NEWNS) == 0 &&
				 mount(NULL, "/", NULL,
				       MS_REC | MS_PRIVATE, NULL) == 0 &&
				 mount("proc", "/proc", "proc", 0, NULL) == 0);

			if (!have_fresh_proc) {
				shadow_checker_send_ok(pipefd[1], "nofresh");
				_exit(0);
			}

			stat_fd = open("/proc/self/stat", O_RDONLY);
			if (stat_fd >= 0) {
				n = read(stat_fd, stat_buf,
					 sizeof(stat_buf) - 1);
				if (n > 0)
					stat_buf[n] = '\0';
				close(stat_fd);
			}

			status_fd = open("/proc/self/status", O_RDONLY);
			if (status_fd >= 0) {
				n = read(status_fd, status_buf,
					 sizeof(status_buf) - 1);
				if (n > 0)
					status_buf[n] = '\0';
				close(status_fd);
			}

			/* /proc/<pid>/stat: "pid (comm) state ppid pgrp
			 * session ...". The comm field is parenthesized and
			 * may itself contain spaces, so start parsing after
			 * the last ')'.
			 */
			{
				char *rparen = strrchr(stat_buf, ')');

				if (rparen)
					sscanf(rparen + 1, " %*c %d %d %d",
					       &ppid_f, &pgrp_f, &sess_f);
				sscanf(stat_buf, "%d", &pid_f);
			}

			for (line = strtok_r(status_buf, "\n", &saveptr);
			     line; line = strtok_r(NULL, "\n", &saveptr)) {
				if (!strncmp(line, "Pid:", 4))
					sscanf(line + 4, "%d", &status_pid);
				else if (!strncmp(line, "PPid:", 5))
					sscanf(line + 5, "%d", &status_ppid);
			}

			shadow_checker_send_ok(pipefd[1],
					       "%d;%d;%d;%d;%d;%d",
					       pid_f, ppid_f, pgrp_f, sess_f,
					       status_pid, status_ppid);
			_exit(0);
		}
		waitpid(grandchild, NULL, 0);
		_exit(0);
	}

	close(pipefd[1]);
	if (!shadow_checker_read_msg(pipefd[0], &msg)) {
		shadow_checker_report("ns_pid (/proc stat content)",
				      SHADOW_CHECKER_SKIP,
				       "child produced no result");
		close(pipefd[0]);
		waitpid(pid, NULL, 0);
		return;
	}
	close(pipefd[0]);
	waitpid(pid, NULL, 0);

	if (!msg.ok) {
		shadow_checker_report("ns_pid (/proc stat content)",
				      SHADOW_CHECKER_FAIL,
				      "unshare(CLONE_NEWPID): %s",
				      strerror(msg.err));
		return;
	}

	if (!strcmp(msg.payload, "nofresh")) {
		shadow_checker_report("ns_pid (/proc stat content)",
				      SHADOW_CHECKER_SKIP,
				      "couldn't mount a fresh /proc to test (needs CAP_SYS_ADMIN)");
		return;
	}

	{
		int pid_f, ppid_f, pgrp_f, sess_f, status_pid, status_ppid;

		if (sscanf(msg.payload, "%d;%d;%d;%d;%d;%d", &pid_f, &ppid_f,
			   &pgrp_f, &sess_f, &status_pid,
			   &status_ppid) != 6) {
			shadow_checker_report("ns_pid (/proc stat content)",
					      SHADOW_CHECKER_SKIP,
					      "unparsable /proc content");
			return;
		}

		if (pid_f == 1 && ppid_f == 0 && pgrp_f == 1 && sess_f == 1 &&
		    status_pid == 1 && status_ppid == 0)
			shadow_checker_report("ns_pid (/proc stat content)",
					      SHADOW_CHECKER_PASS,
					      "/proc/self/stat and /proc/self/status both report virtualized pid=1 ppid=0 pgrp=1 session=1");
		else
			shadow_checker_report("ns_pid (/proc stat content)",
					      SHADOW_CHECKER_STUB,
					      "/proc content not rewritten to namespace-local values (stat: pid=%d ppid=%d pgrp=%d session=%d; status: Pid=%d PPid=%d)",
					      pid_f, ppid_f, pgrp_f, sess_f,
					      status_pid, status_ppid);
	}
}

/*
 * ---------------------------------------------------------------------
 * pidfd_open(2): create a pidfd for a real child and confirm it becomes
 * readable (POLLIN) when that child exits, exactly as a container
 * supervisor waiting on a pidfd instead of polling wait4() would expect.
 * ---------------------------------------------------------------------
 */
static void shadow_checker_pidfd(void)
{
	pid_t pid;
	int pidfd;
	struct pollfd pfd;
	int r;

	pid = fork();
	if (pid < 0) {
		shadow_checker_report("pidfd_open", SHADOW_CHECKER_SKIP,
				      "fork() failed: %s", strerror(errno));
		return;
	}
	if (pid == 0) {
		/* Give the parent a moment to pidfd_open() us before we
		 * exit, so the test targets a still-running process.
		 */
		usleep(50000);
		_exit(0);
	}

	pidfd = (int)syscall(SYS_pidfd_open, pid, 0);
	if (pidfd < 0) {
		shadow_checker_report("pidfd_open", SHADOW_CHECKER_FAIL,
				      "pidfd_open(): %s", strerror(errno));
		waitpid(pid, NULL, 0);
		return;
	}

	pfd.fd = pidfd;
	pfd.events = POLLIN;
	pfd.revents = 0;
	r = poll(&pfd, 1, 2000);
	close(pidfd);
	waitpid(pid, NULL, 0);

	if (r > 0 && (pfd.revents & POLLIN))
		shadow_checker_report("pidfd_open", SHADOW_CHECKER_PASS,
				      "pidfd became readable on child exit");
	else
		shadow_checker_report("pidfd_open", SHADOW_CHECKER_STUB,
				      "pidfd_open() succeeded but never signalled child exit (poll() = %d)",
				      r);
}

/*
 * ---------------------------------------------------------------------
 * ptrace(2): PTRACE_TRACEME + PTRACE_CONT round trip, matching the vendored
 * pid-target translation path before forwarding to the real syscall.
 * ---------------------------------------------------------------------
 */
static void shadow_checker_ptrace(void)
{
	pid_t pid;
	int status;

	pid = fork();
	if (pid < 0) {
		shadow_checker_report("ptrace", SHADOW_CHECKER_SKIP,
				      "fork() failed: %s", strerror(errno));
		return;
	}
	if (pid == 0) {
		if (ptrace(PTRACE_TRACEME, 0, NULL, NULL))
			_exit(1);
		raise(SIGSTOP);
		_exit(0);
	}

	if (waitpid(pid, &status, 0) < 0 || !WIFSTOPPED(status)) {
		shadow_checker_report("ptrace", SHADOW_CHECKER_FAIL,
				      "child never reached the expected PTRACE_TRACEME stop");
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
		return;
	}

	if (ptrace(PTRACE_CONT, pid, NULL, NULL)) {
		shadow_checker_report("ptrace", SHADOW_CHECKER_FAIL,
				      "PTRACE_CONT: %s", strerror(errno));
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
		return;
	}

	waitpid(pid, NULL, 0);
	shadow_checker_report("ptrace", SHADOW_CHECKER_PASS,
			      "PTRACE_TRACEME/PTRACE_CONT round-tripped successfully");
}

/*
 * ---------------------------------------------------------------------
 * wait4(2)/waitid(2): confirm both vendored reaping paths report the correct
 * pid and exit status, not just success/0.
 * ---------------------------------------------------------------------
 */
static void shadow_checker_wait(void)
{
	pid_t pid;
	int status = 0;
	siginfo_t info;

	pid = fork();
	if (pid < 0) {
		shadow_checker_report("wait4/waitid", SHADOW_CHECKER_SKIP,
				      "fork() failed: %s", strerror(errno));
		return;
	}
	if (pid == 0)
		_exit(42);

	if (wait4(pid, &status, 0, NULL) != pid) {
		shadow_checker_report("wait4/waitid", SHADOW_CHECKER_FAIL,
				      "wait4(): %s", strerror(errno));
		return;
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 42) {
		shadow_checker_report("wait4/waitid", SHADOW_CHECKER_STUB,
				      "wait4() reaped the pid but exit status was wrong");
		return;
	}

	pid = fork();
	if (pid < 0) {
		shadow_checker_report("wait4/waitid", SHADOW_CHECKER_SKIP,
				      "fork() failed: %s", strerror(errno));
		return;
	}
	if (pid == 0)
		_exit(43);

	memset(&info, 0, sizeof(info));
	if (waitid(P_PID, pid, &info, WEXITED)) {
		shadow_checker_report("wait4/waitid", SHADOW_CHECKER_FAIL,
				      "waitid(): %s", strerror(errno));
		return;
	}

	if (info.si_pid == pid && info.si_status == 43)
		shadow_checker_report("wait4/waitid", SHADOW_CHECKER_PASS,
				      "wait4() and waitid() both correctly reaped pid/exit status");
	else
		shadow_checker_report("wait4/waitid", SHADOW_CHECKER_STUB,
				      "waitid() returned but si_pid/si_status mismatched (si_pid=%d si_status=%d)",
				      (int)info.si_pid, info.si_status);
}

/*
 * ---------------------------------------------------------------------
 * System V semaphores: semget/semctl(SETVAL,GETVAL)/semop, matching
 * vendor_kernel's hooked SysV semaphore path.
 * ---------------------------------------------------------------------
 */
static void shadow_checker_sysv_sem(void)
{
	int id;
	struct sembuf op = { .sem_num = 0, .sem_op = -1, .sem_flg = 0 };
	int val;

	id = semget(IPC_PRIVATE, 1, IPC_CREAT | 0600);
	if (id < 0) {
		shadow_checker_report("sysvipc (SysV sem)", SHADOW_CHECKER_FAIL,
				      "semget(): %s", strerror(errno));
		return;
	}

	if (semctl(id, 0, SETVAL, 1) < 0) {
		shadow_checker_report("sysvipc (SysV sem)", SHADOW_CHECKER_FAIL,
				      "semctl(SETVAL): %s", strerror(errno));
		semctl(id, 0, IPC_RMID);
		return;
	}

	if (semop(id, &op, 1)) {
		shadow_checker_report("sysvipc (SysV sem)", SHADOW_CHECKER_FAIL,
				      "semop(): %s", strerror(errno));
		semctl(id, 0, IPC_RMID);
		return;
	}

	val = semctl(id, 0, GETVAL);
	semctl(id, 0, IPC_RMID);

	if (val == 0)
		shadow_checker_report("sysvipc (SysV sem)", SHADOW_CHECKER_PASS,
				      "semget/semctl(SETVAL,GETVAL)/semop round-tripped a decrement");
	else
		shadow_checker_report("sysvipc (SysV sem)", SHADOW_CHECKER_STUB,
				      "semop() accepted but value not updated (got %d, want 0)",
				      val);
}

/*
 * ---------------------------------------------------------------------
 * System V shared memory: shmget/shmat/shmdt, matching vendor_kernel's hooked
 * SysV shared-memory path.
 * ---------------------------------------------------------------------
 */
static void shadow_checker_sysv_shm(void)
{
	int id;
	void *addr;
	static const char payload[] = "shadowchk-shm";
	char buf[32] = "";

	id = shmget(IPC_PRIVATE, 4096, IPC_CREAT | 0600);
	if (id < 0) {
		shadow_checker_report("sysvipc (SysV shm)", SHADOW_CHECKER_FAIL,
				      "shmget(): %s", strerror(errno));
		return;
	}

	addr = shmat(id, NULL, 0);
	if (addr == (void *)-1) {
		shadow_checker_report("sysvipc (SysV shm)", SHADOW_CHECKER_FAIL,
				      "shmat(): %s", strerror(errno));
		shmctl(id, IPC_RMID, NULL);
		return;
	}

	memcpy(addr, payload, sizeof(payload));
	memcpy(buf, addr, sizeof(payload));

	if (shmdt(addr))
		fprintf(stderr,
			"lkm4ctr_checker: warning: shmdt() failed: %s\n",
			strerror(errno));
	shmctl(id, IPC_RMID, NULL);

	if (!strcmp(buf, payload))
		shadow_checker_report("sysvipc (SysV shm)", SHADOW_CHECKER_PASS,
				      "shmget/shmat/shmdt round-tripped a write through shared memory");
	else
		shadow_checker_report("sysvipc (SysV shm)", SHADOW_CHECKER_STUB,
				      "shmat() succeeded but readback mismatched");
}

/*
 * ---------------------------------------------------------------------
 * POSIX mqueue attribute get/set: mq_getattr/mq_setattr, matching
 * vendor_kernel's hooked mq_getsetattr path.
 * ---------------------------------------------------------------------
 */
static void shadow_checker_mqueue_attr(void)
{
#ifdef SHADOW_CHECKER_HAVE_MQUEUE
	char name[64];
	mqd_t mq;
	struct mq_attr attr = { .mq_maxmsg = 4, .mq_msgsize = 16 };
	struct mq_attr newattr;
	struct mq_attr oldattr;
	struct mq_attr got;
	int nonblock_ok;

	shadow_checker_unique_name(name, sizeof(name), "/shadowchk-mqattr");
	mq_unlink(name);

	mq = mq_open(name, O_CREAT | O_RDWR | O_EXCL, 0600, &attr);
	if (mq == (mqd_t)-1) {
		shadow_checker_report("mqueue (mq_getsetattr)",
				      SHADOW_CHECKER_FAIL, "mq_open(): %s",
				      strerror(errno));
		return;
	}

	memset(&got, 0, sizeof(got));
	if (mq_getattr(mq, &got)) {
		shadow_checker_report("mqueue (mq_getsetattr)",
				      SHADOW_CHECKER_FAIL, "mq_getattr(): %s",
				      strerror(errno));
		mq_close(mq);
		mq_unlink(name);
		return;
	}

	newattr = got;
	newattr.mq_flags = O_NONBLOCK;
	memset(&oldattr, 0, sizeof(oldattr));
	if (mq_setattr(mq, &newattr, &oldattr)) {
		shadow_checker_report("mqueue (mq_getsetattr)",
				      SHADOW_CHECKER_FAIL, "mq_setattr(): %s",
				      strerror(errno));
		mq_close(mq);
		mq_unlink(name);
		return;
	}

	memset(&got, 0, sizeof(got));
	if (mq_getattr(mq, &got)) {
		shadow_checker_report("mqueue (mq_getsetattr)",
				      SHADOW_CHECKER_FAIL,
				      "mq_getattr() after set: %s",
				      strerror(errno));
		mq_close(mq);
		mq_unlink(name);
		return;
	}

	nonblock_ok = (got.mq_flags & O_NONBLOCK) != 0;
	mq_close(mq);
	mq_unlink(name);

	if (got.mq_maxmsg == attr.mq_maxmsg && nonblock_ok)
		shadow_checker_report("mqueue (mq_getsetattr)",
				      SHADOW_CHECKER_PASS,
				      "mq_getattr/mq_setattr round-tripped maxmsg=%ld and O_NONBLOCK",
				      (long)got.mq_maxmsg);
	else
		shadow_checker_report("mqueue (mq_getsetattr)",
				      SHADOW_CHECKER_STUB,
				      "mq_setattr() accepted but attributes not reflected back (maxmsg=%ld nonblock=%d)",
				      (long)got.mq_maxmsg, nonblock_ok);
#else
	shadow_checker_report("mqueue (mq_getsetattr)", SHADOW_CHECKER_SKIP,
			      "<mqueue.h> unavailable in this libc");
#endif
}

static void shadow_checker_usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s [-q]\n"
		"  -q  quiet: no report, exit status only (0 = all PASS, 1 = any FAIL)\n",
		argv0);
}

int main(int argc, char **argv)
{
	int opt;

	while ((opt = getopt(argc, argv, "qh")) != -1) {
		switch (opt) {
		case 'q':
			g_quiet = true;
			break;
		default:
			shadow_checker_usage(argv[0]);
			return 2;
		}
	}

	if (!g_quiet) {
		printf("lkm4ctr_checker v%s - userspace container-isolation diagnostics\n",
		       LKM4CTR_CHECKER_VERSION);
		printf("PASS = real isolation observed, STUB = bookkeeping only (no real isolation), FAIL = syscall itself failed\n");
		printf("--------------------------------------------------------------------------------------------------------\n");
	}

	shadow_checker_uts();
	shadow_checker_pid();
	shadow_checker_pid_procfs_content();
	shadow_checker_pid_signal();
	shadow_checker_pid_pgrp();
	shadow_checker_pidfd();
	shadow_checker_ptrace();
	shadow_checker_wait();
	shadow_checker_user();
	shadow_checker_user_idmap();
	shadow_checker_generic_ns("ns_ipc (IPC)", "ipc", CLONE_NEWIPC);
	shadow_checker_generic_ns("ns_net (NET)", "net", CLONE_NEWNET);
	shadow_checker_generic_ns("ns_mnt (MNT)", "mnt", CLONE_NEWNS);
	shadow_checker_generic_ns("ns_cgroup (CGROUP)", "cgroup", CLONE_NEWCGROUP);
	shadow_checker_mqueue();
	shadow_checker_mqueue_attr();
	shadow_checker_sysvipc();
	shadow_checker_sysv_sem();
	shadow_checker_sysv_shm();
	shadow_checker_overlay();

	if (!g_quiet) {
		printf("--------------------------------------------------------------------------------------------------------\n");
		printf("summary: %d FAIL, %d STUB (bookkeeping-only)\n", g_fail_count,
		       g_stub_count);
	}

	return g_fail_count ? 1 : 0;
}
