/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vendor_kernel_diag.h - the live-resource "provider" API that backs the
 * procfs-style /v/ns/<type>/ and /v/ipc/<class>/ trees exposed by the
 * lkm4ctr diagfs (glue/lkm4ctr_diagfs.c). This is NEW code (not vendored).
 *
 * lkm4ctr_diagfs.c owns all of the VFS plumbing (dynamic .lookup/.iterate/
 * .unlink dentries, seq_file rendering, mount lifecycle); it is deliberately
 * kept ignorant of vendor_kernel internals. Everything that has to reach into
 * vendor_kernel's own registries -- the tgid->nsproxy hash
 * (vendor_kernel_registry, glue/vendor_kernel_module.c) and the vendored
 * SysV IPC id-r's inside vns_ipc_active_default() (see vendor/ipc/) -- lives
 * behind this small provider vtable instead, defined in vendor_kernel_diag.c.
 *
 * A "resource" is one of:
 *   - a per-task vendored namespace, keyed by the owning thread-group id
 *     (tgid). Every registry entry carries exactly one struct nsproxy, so a
 *     single tgid appears under every /v/ns/<type>/ whose nsproxy pointer is
 *     non-NULL; deleting it under any one type tears that task's whole
 *     vendored nsproxy down (see vns_diag_res_delete()).
 *   - a live SysV IPC object (shm segment / message queue / semaphore set)
 *     in vendor_kernel's default ipc_namespace, keyed by its ipc id.
 *
 * Keys are u64 so both id spaces (tgid, ipc id) share one interface.
 */
#ifndef _VENDOR_KERNEL_DIAG_H
#define _VENDOR_KERNEL_DIAG_H

#include <linux/types.h>

/* Aggregate registry/overlay dump (the /v/namespaces and /v/resources files). */
size_t vendor_kernel_diag_snprintf(char *buf, size_t buflen);

enum vns_diag_res {
	/* per-task vendored namespaces, keyed by owning tgid */
	VNS_DIAG_NS_PID,
	VNS_DIAG_NS_UTS,
	VNS_DIAG_NS_IPC,
	VNS_DIAG_NS_USER,
	VNS_DIAG_NS_NET,
	VNS_DIAG_NS_TIME,
	VNS_DIAG_NS_MNT,
	VNS_DIAG_NS_CGROUP,
	/* live SysV IPC objects in the default vendored ipc_namespace, keyed by id */
	VNS_DIAG_IPC_SHM,
	VNS_DIAG_IPC_MSG,
	VNS_DIAG_IPC_SEM,
	VNS_DIAG_RES_MAX,
};

/*
 * vns_diag_res_name() - short lowercase directory name for @prov ("pid",
 * "uts", ..., "shm", "msg", "sem"), or NULL if @prov is out of range.
 */
const char *vns_diag_res_name(int prov);

/*
 * vns_diag_res_snapshot() - copy up to @max live instance keys for @prov into
 * @keys (an unsorted snapshot taken under the appropriate registry lock).
 * Returns the total number of live instances (which may exceed @max if the
 * caller's buffer was too small; only the first @max are stored).
 */
int vns_diag_res_snapshot(int prov, u64 *keys, int max);

/* vns_diag_res_present() - true iff @key is currently a live @prov instance. */
bool vns_diag_res_present(int prov, u64 key);

/*
 * vns_diag_res_render() - render human-readable detail about one instance
 * into @buf (seq_file "render on open" style, same scnprintf accumulation as
 * every other diagfs file). Returns bytes that would be written.
 */
size_t vns_diag_res_render(int prov, u64 key, char *buf, size_t buflen);

/*
 * vns_diag_res_delete() - gracefully tear one instance down and free it.
 *
 * Namespaces: SIGKILL the owning thread group (making every thread pinned to
 * that vendored nsproxy leave it cleanly via the do_exit() cleanup path,
 * vns_task_exit_cleanup()), wait a bounded time for the task to actually die,
 * then drop vendor_kernel's own pinned registry reference
 * (vns_registry_remove()). Only ever touches module-owned objects.
 *
 * SysV IPC: perform the exact IPC_RMID teardown the kernel itself uses
 * (vns_shmctl/vns_msgctl/vns_semctl with IPC_RMID), which removes the object
 * from the id-r and wakes any task blocked on it with -EIDRM. Operates in the
 * caller's own ipc namespace context, i.e. the default vendored namespace for
 * an ordinary (never-unshared) writer.
 *
 * Returns 0 on success or a negative errno.
 */
int vns_diag_res_delete(int prov, u64 key);

#endif /* _VENDOR_KERNEL_DIAG_H */
