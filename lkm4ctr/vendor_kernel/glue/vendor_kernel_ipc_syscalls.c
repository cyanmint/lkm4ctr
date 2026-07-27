// SPDX-License-Identifier: GPL-2.0
/*
 * vendor_kernel_ipc_syscalls.c - syscall hook handlers wiring the vendored
 * SysV IPC (ipc/msg.c, ipc/sem.c, ipc/shm.c) and POSIX message queue
 * (ipc/mqueue.c) implementations to their syscalls.
 *
 * This is NEW code (not vendored from kernel-common). It mirrors
 * glue/vendor_kernel_syscalls.c, but unlike the unshare/clone/setns hooks it
 * redirects unconditionally: on vendor_kernel's primary target
 * (CONFIG_SYSVIPC=n && CONFIG_POSIX_MQUEUE=n) the real syscalls are
 * sys_ni_syscall() stubs returning -ENOSYS, so there is no "real vs vendored"
 * branching to do - every call goes straight to the vendored vns_* handler.
 *
 * Each vendored handler internally resolves the caller's effective
 * ipc_namespace through vns_current_ipc_ns() (-> vns_task_ipc_ns(current)),
 * so a task that did unshare(CLONE_NEWIPC) operates against its own isolated
 * message-queue / sysvipc IDRs while every other task shares
 * vns_default_ipc_ns. The lookup happens per syscall-call, so no per-hook
 * namespace bookkeeping is needed here.
 */
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/ipc.h>
#include <linux/msg.h>
#include <linux/sem.h>
#include <linux/shm.h>
#include <linux/time_types.h>
#include <asm/ptrace.h>

#include "../vendor_kernel.h"
#include "../../../common/shadow_hook.h"
#include "../../../common/lkm4ctr_log.h"

/*
 * vns_ipc_active_default() - the ipc_namespace used by every task that never
 * called unshare(CLONE_NEWIPC).
 *
 * This ALWAYS returns vendor_kernel's own module-owned vns_default_ipc_ns
 * singleton, never the running kernel's real init_ipc_ns (vns_init_ipc_ns_ptr),
 * even when the target kernel genuinely ships CONFIG_SYSVIPC/CONFIG_POSIX_MQUEUE
 * and exposes its init_ipc_ns through kallsyms. The vendored SysV/mqueue
 * handlers must operate exclusively on vendored ipc_namespace state (mqueuefs +
 * msg/sem/shm IDRs set up by vns_ipc_default_init()), so that real-kernel-owned
 * message-queue / SysV data can never leak into the shadowed syscall paths. This
 * mirrors the fully self-contained UTS_NS/PID_NS/USER_NS handling documented in
 * the README ("Namespace refcounting is fully self-contained"): the vendored
 * default is authoritative and self-sufficient regardless of the target's own
 * ipc config. vns_init_ipc_ns_ptr is used only for cosmetic bookkeeping (see
 * vendor_kernel_module.c), never as a substitute for this default.
 */
struct ipc_namespace *vns_ipc_active_default(void)
{
	return &vns_default_ipc_ns;
}

/*
 * vns_ipc_default_init() - build vendor_kernel's own default ipc_namespace.
 *
 * Upstream initialises init_ipc_ns piecemeal from several device_initcall()s
 * (msg_init()/sem_init()/shm_init()) plus the mqueue module_init
 * (init_mqueue_fs()). None of those initcalls run for an out-of-tree module,
 * so replicate the parts that set up the namespace state here:
 *   - vns_mqueue_fs_init() registers mqueuefs, installs the mq sysctls and runs
 *     mq_init_ns(&vns_default_ipc_ns) (the mqueue half of the default ns);
 *   - the SysV half (ipc sysctls + msg/sem/shm IDRs) is set up directly, since
 *     the /proc/sysvipc registration in msg_init()/sem_init()/shm_init() is the
 *     only other thing those initcalls do and is optional for functionality.
 *
 * The vendored default is ALWAYS built here, unconditionally, regardless of
 * whether the running kernel exposes its own init_ipc_ns (vns_init_ipc_ns_ptr):
 * vendor_kernel's IPC syscall hooks operate exclusively on this vendored
 * namespace, so it must be fully live before they are installed even on a target
 * that ships CONFIG_SYSVIPC/CONFIG_POSIX_MQUEUE. We never re-use or re-initialise
 * the real kernel's init_ipc_ns.
 */
int vns_ipc_default_init(void)
{
	int err;

	err = vns_mqueue_fs_init();
	if (err)
		return err;

	if (!vns_setup_ipc_sysctls(&vns_default_ipc_ns)) {
		err = -ENOMEM;
		goto fail_mqueue;
	}

	err = vns_msg_init_ns(&vns_default_ipc_ns);
	if (err)
		goto fail_ipc_sysctls;

	vns_sem_init_ns(&vns_default_ipc_ns);
	vns_shm_init_ns(&vns_default_ipc_ns);

	/*
	 * Give the default namespace a real ns_common inode number too (same
	 * vns_alloc_inum() used by create_ipc_ns() for an unshare(2)'d one),
	 * so glue/vendor_kernel_procfs.c's fabricated /proc/<pid>/ns/ipc
	 * readlink(2) text has something non-zero and self-consistent to
	 * report for every task that never called unshare(CLONE_NEWIPC).
	 * Not fatal if it fails (proc_alloc_inum() unresolved): the
	 * fabricated text then reads "ipc:[0]" before any unshare(2), which
	 * is still a valid, stable baseline for a before/after diff.
	 */
	vns_alloc_inum(&vns_default_ipc_ns.ns);
	vns_default_ipc_ns.ns.ops = &vns_ipcns_operations;

	/*
	 * The proactive /dev/mqueue mount (vns_mqueue_dev_ensure(), see
	 * glue/vendor_kernel_ipc_mount.c) is deliberately NOT performed here
	 * anymore: it is now vendor_kernel_init()'s own responsibility, done
	 * only once every other step that could still fail has already
	 * succeeded. See the comment at that call site for why -- in short,
	 * mounting here and then having some *later* vendor_kernel_init()
	 * step fail used to leave a mount whose real superblock teardown
	 * (deferred to task_work by path_umount(), see
	 * vns_mqueue_dev_teardown()'s own comment) outlives this module's own
	 * memory once the failed load is synchronously unwound, causing a
	 * guaranteed use-after-free panic in deactivate_super() shortly after
	 * insmod returns.
	 */
	return 0;

fail_ipc_sysctls:
	vns_retire_ipc_sysctls(&vns_default_ipc_ns);
fail_mqueue:
	/*
	 * vns_mqueue_dev_ensure() (line above, on the success path) has not
	 * run yet on any path that reaches these labels, so there is no
	 * proactively-mounted /dev/mqueue whose teardown could still be
	 * pending -- always safe to destroy the cache here.
	 */
	vns_mqueue_fs_exit(false);
	return err;
}

void vns_ipc_default_exit(void)
{
	bool cache_teardown_unsafe;

	vns_free_inum(&vns_default_ipc_ns.ns);
	vns_retire_ipc_sysctls(&vns_default_ipc_ns);
	/*
	 * Must run before vns_mqueue_fs_exit()'s kmem_cache_destroy(), see
	 * vns_mqueue_dev_mounted's comment in vendor_kernel_ipc_mount.c. Its
	 * return value tells vns_mqueue_fs_exit() whether it actually had to
	 * detach a namespace-attached mount whose real teardown is deferred
	 * to task_work (and thus whether destroying the cache right now
	 * would be unsafe).
	 */
	cache_teardown_unsafe = vns_mqueue_dev_teardown();
	vns_mqueue_fs_exit(cache_teardown_unsafe);
}

/* --- argument accessors (extends the arg0/arg1 pair in
 * vendor_kernel_syscalls.c up to the 5 args the IPC syscalls need) --- */
#if defined(CONFIG_ARM64)
static unsigned long vns_ipc_arg0(const struct pt_regs *regs) { return regs->regs[0]; }
static unsigned long vns_ipc_arg1(const struct pt_regs *regs) { return regs->regs[1]; }
static unsigned long vns_ipc_arg2(const struct pt_regs *regs) { return regs->regs[2]; }
static unsigned long vns_ipc_arg3(const struct pt_regs *regs) { return regs->regs[3]; }
static unsigned long vns_ipc_arg4(const struct pt_regs *regs) { return regs->regs[4]; }
#elif defined(CONFIG_X86_64)
/*
 * x86_64 syscall ABI passes args in di, si, dx, r10, r8, r9 (note: r10, not
 * rcx, for the 4th arg - the `syscall` instruction clobbers rcx).
 */
static unsigned long vns_ipc_arg0(const struct pt_regs *regs) { return regs->di; }
static unsigned long vns_ipc_arg1(const struct pt_regs *regs) { return regs->si; }
static unsigned long vns_ipc_arg2(const struct pt_regs *regs) { return regs->dx; }
static unsigned long vns_ipc_arg3(const struct pt_regs *regs) { return regs->r10; }
static unsigned long vns_ipc_arg4(const struct pt_regs *regs) { return regs->r8; }
#else
#error "vendor_kernel: unsupported architecture"
#endif

/* ---------------------------- SysV IPC: msg ---------------------------- */

static long (*real_sys_msgget)(const struct pt_regs *regs);
static long (*real_sys_msgsnd)(const struct pt_regs *regs);
static long (*real_sys_msgrcv)(const struct pt_regs *regs);
static long (*real_sys_msgctl)(const struct pt_regs *regs);

static const char * const vk_msgget_names[] = {
	"__arm64_sys_msgget", "__x64_sys_msgget", "sys_msgget", NULL,
};
static const char * const vk_msgsnd_names[] = {
	"__arm64_sys_msgsnd", "__x64_sys_msgsnd", "sys_msgsnd", NULL,
};
static const char * const vk_msgrcv_names[] = {
	"__arm64_sys_msgrcv", "__x64_sys_msgrcv", "sys_msgrcv", NULL,
};
static const char * const vk_msgctl_names[] = {
	"__arm64_sys_msgctl", "__x64_sys_msgctl", "sys_msgctl", NULL,
};

static long vk_hook_msgget(const struct pt_regs *regs)
{
	key_t key = (key_t)vns_ipc_arg0(regs);
	int msgflg = (int)vns_ipc_arg1(regs);

	return vns_ksys_msgget(key, msgflg);
}

static long vk_hook_msgsnd(const struct pt_regs *regs)
{
	int msqid = (int)vns_ipc_arg0(regs);
	struct msgbuf __user *msgp = (struct msgbuf __user *)(uintptr_t)vns_ipc_arg1(regs);
	size_t msgsz = (size_t)vns_ipc_arg2(regs);
	int msgflg = (int)vns_ipc_arg3(regs);

	return vns_ksys_msgsnd(msqid, msgp, msgsz, msgflg);
}

static long vk_hook_msgrcv(const struct pt_regs *regs)
{
	int msqid = (int)vns_ipc_arg0(regs);
	struct msgbuf __user *msgp = (struct msgbuf __user *)(uintptr_t)vns_ipc_arg1(regs);
	size_t msgsz = (size_t)vns_ipc_arg2(regs);
	long msgtyp = (long)vns_ipc_arg3(regs);
	int msgflg = (int)vns_ipc_arg4(regs);

	return vns_ksys_msgrcv(msqid, msgp, msgsz, msgtyp, msgflg);
}

static long vk_hook_msgctl(const struct pt_regs *regs)
{
	int msqid = (int)vns_ipc_arg0(regs);
	int cmd = (int)vns_ipc_arg1(regs);
	struct msqid_ds __user *buf = (struct msqid_ds __user *)(uintptr_t)vns_ipc_arg2(regs);

	return vns_msgctl(msqid, cmd, buf);
}

/* ---------------------------- SysV IPC: sem ---------------------------- */

static long (*real_sys_semget)(const struct pt_regs *regs);
static long (*real_sys_semop)(const struct pt_regs *regs);
static long (*real_sys_semtimedop)(const struct pt_regs *regs);
static long (*real_sys_semctl)(const struct pt_regs *regs);

static const char * const vk_semget_names[] = {
	"__arm64_sys_semget", "__x64_sys_semget", "sys_semget", NULL,
};
static const char * const vk_semop_names[] = {
	"__arm64_sys_semop", "__x64_sys_semop", "sys_semop", NULL,
};
static const char * const vk_semtimedop_names[] = {
	"__arm64_sys_semtimedop", "__x64_sys_semtimedop", "sys_semtimedop", NULL,
};
static const char * const vk_semctl_names[] = {
	"__arm64_sys_semctl", "__x64_sys_semctl", "sys_semctl", NULL,
};

static long vk_hook_semget(const struct pt_regs *regs)
{
	key_t key = (key_t)vns_ipc_arg0(regs);
	int nsems = (int)vns_ipc_arg1(regs);
	int semflg = (int)vns_ipc_arg2(regs);

	return vns_ksys_semget(key, nsems, semflg);
}

static long vk_hook_semop(const struct pt_regs *regs)
{
	int semid = (int)vns_ipc_arg0(regs);
	struct sembuf __user *tsops = (struct sembuf __user *)(uintptr_t)vns_ipc_arg1(regs);
	unsigned int nsops = (unsigned int)vns_ipc_arg2(regs);

	/* Upstream sys_semop() forwards to ksys_semtimedop(..., NULL). */
	return vns_ksys_semtimedop(semid, tsops, nsops, NULL);
}

static long vk_hook_semtimedop(const struct pt_regs *regs)
{
	int semid = (int)vns_ipc_arg0(regs);
	struct sembuf __user *tsops = (struct sembuf __user *)(uintptr_t)vns_ipc_arg1(regs);
	unsigned int nsops = (unsigned int)vns_ipc_arg2(regs);
	const struct __kernel_timespec __user *timeout =
		(const struct __kernel_timespec __user *)(uintptr_t)vns_ipc_arg3(regs);

	return vns_ksys_semtimedop(semid, tsops, nsops, timeout);
}

static long vk_hook_semctl(const struct pt_regs *regs)
{
	int semid = (int)vns_ipc_arg0(regs);
	int semnum = (int)vns_ipc_arg1(regs);
	int cmd = (int)vns_ipc_arg2(regs);
	unsigned long arg = (unsigned long)vns_ipc_arg3(regs);

	return vns_semctl(semid, semnum, cmd, arg);
}

/* ---------------------------- SysV IPC: shm ---------------------------- */

static long (*real_sys_shmget)(const struct pt_regs *regs);
static long (*real_sys_shmat)(const struct pt_regs *regs);
static long (*real_sys_shmdt)(const struct pt_regs *regs);
static long (*real_sys_shmctl)(const struct pt_regs *regs);

static const char * const vk_shmget_names[] = {
	"__arm64_sys_shmget", "__x64_sys_shmget", "sys_shmget", NULL,
};
static const char * const vk_shmat_names[] = {
	"__arm64_sys_shmat", "__x64_sys_shmat", "sys_shmat", NULL,
};
static const char * const vk_shmdt_names[] = {
	"__arm64_sys_shmdt", "__x64_sys_shmdt", "sys_shmdt", NULL,
};
static const char * const vk_shmctl_names[] = {
	"__arm64_sys_shmctl", "__x64_sys_shmctl", "sys_shmctl", NULL,
};

static long vk_hook_shmget(const struct pt_regs *regs)
{
	key_t key = (key_t)vns_ipc_arg0(regs);
	size_t size = (size_t)vns_ipc_arg1(regs);
	int shmflg = (int)vns_ipc_arg2(regs);

	return vns_ksys_shmget(key, size, shmflg);
}

static long vk_hook_shmat(const struct pt_regs *regs)
{
	int shmid = (int)vns_ipc_arg0(regs);
	char __user *shmaddr = (char __user *)(uintptr_t)vns_ipc_arg1(regs);
	int shmflg = (int)vns_ipc_arg2(regs);

	return vns_shmat(shmid, shmaddr, shmflg);
}

static long vk_hook_shmdt(const struct pt_regs *regs)
{
	char __user *shmaddr = (char __user *)(uintptr_t)vns_ipc_arg0(regs);

	return vns_ksys_shmdt(shmaddr);
}

static long vk_hook_shmctl(const struct pt_regs *regs)
{
	int shmid = (int)vns_ipc_arg0(regs);
	int cmd = (int)vns_ipc_arg1(regs);
	struct shmid_ds __user *buf = (struct shmid_ds __user *)(uintptr_t)vns_ipc_arg2(regs);

	return vns_shmctl(shmid, cmd, buf);
}

/* -------------------------- POSIX message queues -------------------------- */

static long (*real_sys_mq_open)(const struct pt_regs *regs);
static long (*real_sys_mq_unlink)(const struct pt_regs *regs);
static long (*real_sys_mq_timedsend)(const struct pt_regs *regs);
static long (*real_sys_mq_timedreceive)(const struct pt_regs *regs);
static long (*real_sys_mq_notify)(const struct pt_regs *regs);
static long (*real_sys_mq_getsetattr)(const struct pt_regs *regs);

static const char * const vk_mq_open_names[] = {
	"__arm64_sys_mq_open", "__x64_sys_mq_open", "sys_mq_open", NULL,
};
static const char * const vk_mq_unlink_names[] = {
	"__arm64_sys_mq_unlink", "__x64_sys_mq_unlink", "sys_mq_unlink", NULL,
};
static const char * const vk_mq_timedsend_names[] = {
	"__arm64_sys_mq_timedsend", "__x64_sys_mq_timedsend", "sys_mq_timedsend", NULL,
};
static const char * const vk_mq_timedreceive_names[] = {
	"__arm64_sys_mq_timedreceive", "__x64_sys_mq_timedreceive", "sys_mq_timedreceive", NULL,
};
static const char * const vk_mq_notify_names[] = {
	"__arm64_sys_mq_notify", "__x64_sys_mq_notify", "sys_mq_notify", NULL,
};
static const char * const vk_mq_getsetattr_names[] = {
	"__arm64_sys_mq_getsetattr", "__x64_sys_mq_getsetattr", "sys_mq_getsetattr", NULL,
};

static long vk_hook_mq_open(const struct pt_regs *regs)
{
	const char __user *name = (const char __user *)(uintptr_t)vns_ipc_arg0(regs);
	int oflag = (int)vns_ipc_arg1(regs);
	umode_t mode = (umode_t)vns_ipc_arg2(regs);
	struct mq_attr __user *attr = (struct mq_attr __user *)(uintptr_t)vns_ipc_arg3(regs);

	return vns_mq_open(name, oflag, mode, attr);
}

static long vk_hook_mq_unlink(const struct pt_regs *regs)
{
	const char __user *name = (const char __user *)(uintptr_t)vns_ipc_arg0(regs);

	return vns_mq_unlink(name);
}

static long vk_hook_mq_timedsend(const struct pt_regs *regs)
{
	mqd_t mqdes = (mqd_t)vns_ipc_arg0(regs);
	const char __user *msg_ptr = (const char __user *)(uintptr_t)vns_ipc_arg1(regs);
	size_t msg_len = (size_t)vns_ipc_arg2(regs);
	unsigned int msg_prio = (unsigned int)vns_ipc_arg3(regs);
	const struct __kernel_timespec __user *abs_timeout =
		(const struct __kernel_timespec __user *)(uintptr_t)vns_ipc_arg4(regs);

	return vns_mq_timedsend(mqdes, msg_ptr, msg_len, msg_prio, abs_timeout);
}

static long vk_hook_mq_timedreceive(const struct pt_regs *regs)
{
	mqd_t mqdes = (mqd_t)vns_ipc_arg0(regs);
	char __user *msg_ptr = (char __user *)(uintptr_t)vns_ipc_arg1(regs);
	size_t msg_len = (size_t)vns_ipc_arg2(regs);
	unsigned int __user *msg_prio = (unsigned int __user *)(uintptr_t)vns_ipc_arg3(regs);
	const struct __kernel_timespec __user *abs_timeout =
		(const struct __kernel_timespec __user *)(uintptr_t)vns_ipc_arg4(regs);

	return vns_mq_timedreceive(mqdes, msg_ptr, msg_len, msg_prio, abs_timeout);
}

static long vk_hook_mq_notify(const struct pt_regs *regs)
{
	mqd_t mqdes = (mqd_t)vns_ipc_arg0(regs);
	const struct sigevent __user *notification =
		(const struct sigevent __user *)(uintptr_t)vns_ipc_arg1(regs);

	return vns_mq_notify(mqdes, notification);
}

static long vk_hook_mq_getsetattr(const struct pt_regs *regs)
{
	mqd_t mqdes = (mqd_t)vns_ipc_arg0(regs);
	const struct mq_attr __user *mqstat =
		(const struct mq_attr __user *)(uintptr_t)vns_ipc_arg1(regs);
	struct mq_attr __user *omqstat =
		(struct mq_attr __user *)(uintptr_t)vns_ipc_arg2(regs);

	return vns_mq_getsetattr(mqdes, mqstat, omqstat);
}

/* ------------------------------ hook table ------------------------------ */

static struct shadow_hook vk_msgget_hook =
	SHADOW_HOOK(vk_msgget_names, vk_hook_msgget, &real_sys_msgget);
static struct shadow_hook vk_msgsnd_hook =
	SHADOW_HOOK(vk_msgsnd_names, vk_hook_msgsnd, &real_sys_msgsnd);
static struct shadow_hook vk_msgrcv_hook =
	SHADOW_HOOK(vk_msgrcv_names, vk_hook_msgrcv, &real_sys_msgrcv);
static struct shadow_hook vk_msgctl_hook =
	SHADOW_HOOK(vk_msgctl_names, vk_hook_msgctl, &real_sys_msgctl);

static struct shadow_hook vk_semget_hook =
	SHADOW_HOOK(vk_semget_names, vk_hook_semget, &real_sys_semget);
static struct shadow_hook vk_semop_hook =
	SHADOW_HOOK(vk_semop_names, vk_hook_semop, &real_sys_semop);
static struct shadow_hook vk_semtimedop_hook =
	SHADOW_HOOK(vk_semtimedop_names, vk_hook_semtimedop, &real_sys_semtimedop);
static struct shadow_hook vk_semctl_hook =
	SHADOW_HOOK(vk_semctl_names, vk_hook_semctl, &real_sys_semctl);

static struct shadow_hook vk_shmget_hook =
	SHADOW_HOOK(vk_shmget_names, vk_hook_shmget, &real_sys_shmget);
static struct shadow_hook vk_shmat_hook =
	SHADOW_HOOK(vk_shmat_names, vk_hook_shmat, &real_sys_shmat);
static struct shadow_hook vk_shmdt_hook =
	SHADOW_HOOK(vk_shmdt_names, vk_hook_shmdt, &real_sys_shmdt);
static struct shadow_hook vk_shmctl_hook =
	SHADOW_HOOK(vk_shmctl_names, vk_hook_shmctl, &real_sys_shmctl);

static struct shadow_hook vk_mq_open_hook =
	SHADOW_HOOK(vk_mq_open_names, vk_hook_mq_open, &real_sys_mq_open);
static struct shadow_hook vk_mq_unlink_hook =
	SHADOW_HOOK(vk_mq_unlink_names, vk_hook_mq_unlink, &real_sys_mq_unlink);
static struct shadow_hook vk_mq_timedsend_hook =
	SHADOW_HOOK(vk_mq_timedsend_names, vk_hook_mq_timedsend, &real_sys_mq_timedsend);
static struct shadow_hook vk_mq_timedreceive_hook =
	SHADOW_HOOK(vk_mq_timedreceive_names, vk_hook_mq_timedreceive, &real_sys_mq_timedreceive);
static struct shadow_hook vk_mq_notify_hook =
	SHADOW_HOOK(vk_mq_notify_names, vk_hook_mq_notify, &real_sys_mq_notify);
static struct shadow_hook vk_mq_getsetattr_hook =
	SHADOW_HOOK(vk_mq_getsetattr_names, vk_hook_mq_getsetattr, &real_sys_mq_getsetattr);

struct shadow_hook *vendor_kernel_ipc_hooks[] = {
	&vk_msgget_hook,
	&vk_msgsnd_hook,
	&vk_msgrcv_hook,
	&vk_msgctl_hook,
	&vk_semget_hook,
	&vk_semop_hook,
	&vk_semtimedop_hook,
	&vk_semctl_hook,
	&vk_shmget_hook,
	&vk_shmat_hook,
	&vk_shmdt_hook,
	&vk_shmctl_hook,
	&vk_mq_open_hook,
	&vk_mq_unlink_hook,
	&vk_mq_timedsend_hook,
	&vk_mq_timedreceive_hook,
	&vk_mq_notify_hook,
	&vk_mq_getsetattr_hook,
	NULL,
};
