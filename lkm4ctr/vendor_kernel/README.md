# vendor_kernel

vendor_kernel is a parallel, vendored copy of the kernel namespace subsystem for `lkm4ctr`. It hooks namespace syscalls unconditionally and installs a real, vendored `struct nsproxy *` directly on `task_struct->nsproxy` (via `vns_switch_task_namespaces()`), so the rest of the kernel (hostname, `/proc`, ipc/netns lookups, future `fork()`s) transparently observes the new namespace instead of the caller's original one.

## Real isolation vs. bookkeeping

- `unshare(CLONE_NEWxxx)` builds the new namespaces and then calls `vns_switch_task_namespaces(current, new_nsp)` to install them on the calling task for real. Because this happens synchronously before the syscall returns, any subsequent `fork()`/`clone()` from that task sees the new namespace state immediately. `CLONE_NEWPID` performs real pid namespace isolation even when the *running* kernel lacks its own pid-namespace core (`copy_pid_ns()` absent, i.e. a stock `CONFIG_PID_NS=n` kernel): `alloc_pid()`/`task_active_pid_ns()` are unconditional of `CONFIG_PID_NS`, so a module-owned `pid_namespace` still gets real per-namespace pid virtualization. The only real danger on such a kernel is the target's own inline `zap_pid_ns_processes()` `BUG()` stub, which the real `do_exit()` would otherwise reach once the namespace's pid 1 exits; vendor_kernel defuses this in `vns_task_exit_cleanup()`'s `do_exit()` exit-safety kprobe (`kernel/nsproxy.c`), running its own reduced-scope zap cascade (`vns_zap_pid_ns_processes()`, `kernel/pid_namespace.c`) and handing the namespace's `child_reaper` off to the real init task so the real `find_child_reaper()` never reaches the broken stub.
- `clone(CLONE_NEWxxx, ...)` (namespaces requested directly at clone time, without a prior `unshare()`) has the vns_* flags masked off before the underlying `clone()`/`clone3()` syscall runs, then the new namespaces are built and installed on the just-created child task. This makes UTS/IPC/USER/NET/MNT/CGROUP isolation real for that pattern too. The one caveat: because the real `copy_process()` already allocated the child's own `struct pid` from the *parent's* pid namespace before this hook runs, the child's own pid is not renumbered by this path (only namespaces it creates for its own descendants are new) — fully remapping the child's own pid for direct `clone(CLONE_NEWPID, ...)` would require hooking `copy_process()`/`kernel_clone()` itself.
- `setns(2)` (`vns_sys_setns()`) already performed a real install via the same switch primitive and required no changes.
- The per-tgid registry (`vns_task_find()` / `struct vns_task`) is retained purely for diagfs statistics (`stat_unshare`/`stat_setns`/`stat_clone`); it is no longer the source of truth for which namespaces a task is in — `task_struct->nsproxy` is.

## Slab-cache consistency with the real kernel

Once vendored namespaces are installed on the real `task_struct->nsproxy`/`cred->user_ns`, the real kernel's own exit path (`do_exit` -> `exit_task_namespaces` -> `free_nsproxy` -> `free_uts_ns`/`put_pid_ns`/`__put_user_ns`) would eventually try to free them via `kmem_cache_free()` against the real kernel's private, non-exported `kmem_cache` instances (`uts_ns_cache`, `nsproxy_cachep`, `pid_ns_cachep`, `user_ns_cachep`). If those objects had been allocated with plain `kzalloc()` -- or with the *real* kernel's cache, which then gets freed against a *different*, module-owned cache, or vice versa -- SLUB's `cache_from_obj()` detects the mismatch ("Wrong slab cache") and the resulting corruption crashes the kernel (observed as a `kernel BUG at pid_namespace.h:76` panic on task exit).

**Resolving the 4 real cache pointers by name is fundamentally unreliable on real devices and was abandoned.** An earlier revision resolved them at init time via `shadow_hook_resolve()` (which is itself `register_kprobe()`-based symbol lookup through kallsyms). That only ever works for kallsyms *function* symbols; `uts_ns_cache`/`nsproxy_cachep`/`pid_ns_cachep`/`user_ns_cachep` are all `static struct kmem_cache *` **data** variables, which kallsyms only exposes when the target kernel was built with `CONFIG_KALLSYMS_ALL` -- essentially never the case on production/certified/GKI Android kernels. Worse, `utsname.o`/`user_namespace.o`/`pid_namespace.o` are themselves gated by `obj-$(CONFIG_UTS_NS)`/`obj-$(CONFIG_USER_NS)`/`obj-$(CONFIG_PID_NS)` in upstream's `kernel/Makefile`, so on a target with those configs `=n` (`vendor_kernel`'s entire reason for existing), `uts_ns_cache`/`pid_ns_cachep`/`user_ns_cachep` don't even exist in vmlinib to resolve. This combination made the 4-cache resolution fail unconditionally on real hardware, and `vns_compat_ready()` then failed the whole module load with `-ENOENT` ("uts_ns_cache/nsproxy_cachep/pid_ns_cachep/user_ns_cachep unresolved; vendor_kernel unavailable").

The fix: **all 4 caches are now entirely module-owned**, created via `kmem_cache_create()` in each subsystem's own init function (`vns_uts_ns_init()`, `vns_nsproxy_cache_init()`, `vns_pid_ns_init()`, `vns_user_ns_init()`) instead of ever being resolved from the running kernel. `create_uts_ns()`/`create_nsproxy()`/`create_pid_namespace()`/`alloc_user_ns()` in the corresponding vendored files still allocate/free through `kmem_cache_alloc()`/`kmem_cache_zalloc()`/`kmem_cache_free()` (matching upstream's alloc-vs-zalloc semantics), just against vendor_kernel's own caches -- no call-site changes were needed for this. `vns_compat_ready()` no longer treats any of the 4 caches as part of its readiness gate; `vendor_kernel_init()` fails closed with `-ENOMEM` only if `kmem_cache_create()` itself fails (extremely unlikely).

Making the caches module-owned only solves half the problem: the real kernel's own exit path must now be kept from ever calling `kmem_cache_free()` against these module-owned objects at all, since it would use the wrong (real) cache pointer. `uts_namespace`/`pid_namespace`/`user_namespace` are safe by construction here: when the target's `CONFIG_UTS_NS`/`CONFIG_PID_NS`/`CONFIG_USER_NS` is `n` (the scenario vendor_kernel targets), the real kernel's own `put_uts_ns()`/`put_pid_ns()`/`__put_user_ns()` compile to no-ops (see "Namespace refcounting is fully self-contained" below), so the real exit path never reaches a real `kmem_cache_free()` call for these three types regardless. `nsproxy` itself is the one unconditional hazard: `kernel/nsproxy.c` is always `obj-y`, so the real kernel's `free_nsproxy()` *always* unconditionally calls `kmem_cache_free(nsproxy_cachep, ns)` — a guaranteed "wrong slab cache" panic if `ns` is one of vendor_kernel's own objects.

To close this, `kernel/nsproxy.c` adds:

- `vns_nsproxy_set` — a small spinlock-protected hash table (`kernel/nsproxy.c`) of every live, module-owned `struct nsproxy *` (inserted in `create_nsproxy()`, removed in `vns_free_nsproxy()`). A pointer-identity check, not a tgid/task lookup, so it works correctly regardless of how many threads/tasks share the pointer via ordinary `fork()`.
- `vns_task_exit_cleanup(struct task_struct *tsk)` — if `tsk->nsproxy` is currently module-owned (per `vns_nsproxy_set`), swaps it onto the pinned `vns_init_nsproxy` singleton (refcount pinned to a large sentinel at `vns_nsproxy_cache_init()` time so it can never reach zero) and tears the old, module-owned object down completely through vendor_kernel's own self-contained free path (`vns_put_nsproxy()`/`vns_free_nsproxy()`).
- A **plain `pre_handler`-only `struct kprobe` on `do_exit()`** (`vns_exit_hook_init()`/`vns_exit_hook_exit()`, `kernel/nsproxy.c`), registered from `vendor_kernel_init()`/removed from `vendor_kernel_exit()`, calls `vns_task_exit_cleanup(current)` for every exiting task on the system, strictly before the real `do_exit()` body (and therefore `exit_task_namespaces()`/`free_nsproxy()`) runs. By the time the real path executes, `tsk->nsproxy` already points at the pinned, never-slab-allocated `vns_init_nsproxy`, and the real kernel never touches a module-owned object. This is *not* implemented as one of the redirecting `SHADOW_HOOK()` entries used for syscalls elsewhere in `vendor_kernel`: `do_exit()` is `__noreturn`, so redirecting it would mean the rmmod-safety kretprobe placed on the *replacement* function (see `common/shadow_hook.h`'s "rmmod safety" note) could never fire on return, permanently pinning `module_refcount()` above zero the moment the first process on the whole system exits. A plain pre-handler kprobe has no such problem: it runs and returns normally, and `register_kprobe()`/`unregister_kprobe()` are already the same primitive `shadow_hook_resolve()` itself relies on.

**Known accepted remaining gap:** `cred->user_ns` can be freed independently of nsproxy teardown, via a deferred RCU callback (`put_cred_rcu()`) at an arbitrary later time and context, decoupled from task exit. A similarly rigorous fix would require intercepting `__put_user_ns()` or the cred RCU-free path itself, which is materially harder to make safe and is out of scope here. This is a plain no-op leak (not a crash risk) whenever the target's `CONFIG_USER_NS=n` (vendor_kernel's primary use case); it is only a real hazard in the unusual combination where the target ships `CONFIG_USER_NS=y` while still lacking other namespace types vendor_kernel exists for.

`ipc_namespace`, `cgroup_namespace`, and `time_namespace` are unaffected — upstream itself allocates those with plain `kzalloc()`/`kmalloc()` + `kfree()`, so there is no cache mismatch to fix. The per-level `struct pid` slab cache (`ns->pid_cachep`, created via `create_pid_cachep()`) was already a real, self-consistent `kmem_cache_create()`-based cache and needed no change.

## Namespace refcounting is fully self-contained (UTS_NS / PID_NS / USER_NS)

`get_uts_ns()`/`put_uts_ns()` (`include/linux/utsname.h`),
`get_pid_ns()`/`put_pid_ns()` (`include/linux/pid_namespace.h`) and
`get_user_ns()`/`put_user_ns()`/`__put_user_ns()`
(`include/linux/user_namespace.h`) are declared differently depending on the
*target* kernel's own `CONFIG_UTS_NS`/`CONFIG_PID_NS`/`CONFIG_USER_NS`
setting: a real, exported function (or a real `refcount_inc()`/
`refcount_dec_and_test()` body) when the option is `y`, versus a no-op
`static inline` stub when it is `n`. When these options are `n`,
`put_uts_ns()`/`put_pid_ns()`/`__put_user_ns()` are not merely unexported --
they are entirely absent from vmlinux, so no runtime symbol resolution
(`shadow_hook_resolve()`) can ever find them.
Likewise, several `#ifdef CONFIG_PID_NS`/`#ifdef CONFIG_USER_NS` blocks in
upstream's `kernel/nsproxy.c` (`validate_nsset()`/`commit_nsset()`, used by
`setns(2)`) are compiled out entirely when the target lacks these options,
which would silently skip pid/user namespace installation during `setns(2)`
even though `vendor_kernel` fully implements both namespace types itself.

To make `vendor_kernel` install, refcount, and free its own `uts_namespace`/
`pid_namespace`/`user_namespace` objects correctly regardless of the target
kernel's `CONFIG_UTS_NS`/`CONFIG_PID_NS`/`CONFIG_USER_NS` setting, every
vendored call site uses local, unconditionally-compiled equivalents instead
of calling the real kernel functions or gating on these macros:

- `vns_get_uts_ns()` / `vns_put_uts_ns()` (`kernel/utsname.c`) replace
  `get_uts_ns()`/`put_uts_ns()`, including the two call sites in
  `kernel/nsproxy.c`.
- `vns_get_pid_ns()` / `vns_put_pid_ns()` (`kernel/pid_namespace.c`) replace
  `get_pid_ns()`/`put_pid_ns()`.
- `vns_get_user_ns()` / `vns_put_user_ns()` (`kernel/user_namespace.c`)
  replace `get_user_ns()`/`put_user_ns()`, and `vns_put_user_ns()` itself
  routes to `vns___put_user_ns()` (the already-vendored teardown function)
  on the final put.
- The `#ifdef CONFIG_PID_NS`/`#ifdef CONFIG_USER_NS` blocks in
  `vns_sys_setns()`'s `validate_nsset()`/`commit_nsset()` helpers
  (`kernel/nsproxy.c`) were removed (made unconditional), since
  `vendor_kernel` always vendors both namespace types itself.

These shims reuse the same `ns.count`/`kref.refcount` field that the real
kernel functions operate on (via the existing `vns_pid_get_ref`/
`vns_pid_put_ref`/`vns_user_get_ref`/`vns_user_put_ref` macros in
`vendor_kernel.h`, which are already version-gated for the pre-5.15
`kref`-based layout vs. the 5.15+ `ns_common.count` layout), so refcounting
stays correct and slab-cache-consistent (see "Slab-cache consistency with
the real kernel" above) no matter what the target kernel's own
`CONFIG_PID_NS`/`CONFIG_USER_NS` says.

**Do not instead force these `CONFIG_*` options to `=1` at compile time
(e.g. via `-DCONFIG_PID_NS=1`/`-DCONFIG_USER_NS=1` compiler flags) to work
around a target kernel that lacks them.** Forcing the macro to `1` at
compile time makes vendor_kernel's compiled code take the `extern` branch
of the *kernel's own* declarations (e.g. `from_kuid()`/`from_kgid()` in
`include/linux/uidgid.h`) and reference the real exported symbol
unconditionally; if the actual running kernel was truly built with that
option `=n`, the symbol is genuinely absent there (not just unexported),
and `insmod` fails with `Unknown symbol from_kuid`/`put_pid_ns`/
`__put_user_ns`/etc. (err -2). This was tried once and reverted after
reproducing exactly this failure -- the local-shim approach above is the
supported fix instead.

## Required kernel Kconfig options

`vendor_kernel` resolves a handful of the running kernel's namespace-adjacent
*function* symbols (e.g. `init_ipc_ns`) by name at module
load time via `shadow_hook_resolve()`; the namespace object slab caches
themselves (`uts_ns_cache`/`nsproxy_cachep`/`pid_ns_cachep`/`user_ns_cachep`)
are no longer resolved from the running kernel at all -- see "Slab-cache
consistency with the real kernel" above. For any namespace type whose
backing `CONFIG_*_NS` option is not built into the running kernel, the
corresponding resolved function does not exist, and `unshare(2)`/
`clone(2)`/`setns(2)` for that namespace type either falls back to
bookkeeping-only behaviour or fails with `-EINVAL` (this is the root cause
of `ns_net` unshare test failures seen on kernels that ship with
`CONFIG_NET_NS=n`, even though `vendor_kernel` itself
loads and activates successfully). `UTS_NS`, `PID_NS`, `USER_NS`, `IPC_NS`,
and `CGROUP_NS` are the exception: as documented above under "Namespace
refcounting is fully self-contained" and below under "Module-owned default
cgroup namespace, always vendored", vendor_kernel no longer depends on the
target's `CONFIG_UTS_NS`/`CONFIG_PID_NS`/`CONFIG_USER_NS`/`CONFIG_IPC_NS`/
`CONFIG_CGROUPS` at all for these to install and refcount correctly (real,
per-task `CLONE_NEWCGROUP` isolation itself remains bookkeeping-only --
see "Known remaining gaps" below -- but that has never depended on the
running kernel's `CONFIG_CGROUPS` setting either way). The running kernel
still must be built with the remaining options for full namespace coverage:

- `CONFIG_NAMESPACES=y`
- `CONFIG_NET_NS=y`
- `CONFIG_TIME_NS=y`


Of these, `CONFIG_IPC_NS=n` (and, on kernels that still gate it,
`CONFIG_PID_NS=n`) are the exceptions that no longer need a diagnostic
caveat: `unshare(CLONE_NEWIPC)`/SysV IPC/mqueue syscalls and
`unshare(CLONE_NEWPID)`/pid virtualization have always performed genuine
vendored isolation regardless of these options (see "Module-owned default
namespace, always vendored" above), but until now external tools that
verify isolation by diffing `readlink(2)` on `/proc/<pid>/ns/{ipc,pid}`
(including `lkm4ctr_checker`'s generic namespace test) could not observe
it, and `docker exec`/`docker run` could not even *probe* namespace support
at all: `fs/proc/namespaces.c`'s `ns_entries[]` table only registers those
procfs entries `#ifdef CONFIG_IPC_NS`/`#ifdef CONFIG_PID_NS`, a decision
baked into the running `vmlinux` that no syscall hook can undo.
`glue/vendor_kernel_procfs.c` closes this observability gap in two parts:
- Hooking `readlink(2)`/`readlinkat(2)`: the real syscall always runs
  first, and only on its `-ENOENT` for a path unambiguously naming
  `.../<pid|self|thread-self>/ns/ipc` or `.../<pid|self|thread-self>/ns/pid`
  is the `"ipc:[<inum>]"`/`"pid:[<inum>]"` text fabricated, using the same
  `vns_task_ipc_ns()`/`task_active_pid_ns()` namespace objects the real
  SysV/mqueue syscalls and pid virtualization already act on.
- Hooking `stat(2)`/`lstat(2)`/`newfstatat(2)`: runc/containerd's own
  namespace-support probe (`configs.IsNamespaceSupported()`) never reads the
  readlink(2) target at all, it only checks whether `stat(2)` on the path
  *succeeds*. Without this second hook, that probe still fails with plain
  `-ENOENT` even with the readlink(2) fabrication in place, and `docker
  exec`/`docker run` abort with `"OCI runtime exec failed: ... namespace
  NEWIPC is not supported: unknown"` (or the analogous `"namespace NEWPID
  is not supported"` message for pid). Since struct stat's on-wire layout is
  architecture-specific and the kernel's own conversion code isn't exported,
  this hook instead transparently substitutes the `.../ns/ipc` or
  `.../ns/pid` leaf for `.../ns/mnt` (identical length in both cases,
  patched in place on the caller's own path buffer and restored immediately
  after) before calling through to the real syscall: the mount namespace
  entry is the one `/proc/<pid>/ns/` entry that is never Kconfig-gated, so
  it is always present, and every caller of this stat(2) family only cares
  whether the call succeeds (see above), not which namespace's numbers come
  back.

Both hook groups are installed best-effort/non-fatal (a resolution failure
only logs a warning): they are purely an observability enhancement, so they
never block `vendor_kernel`'s core namespace functionality from loading.
Only IPC and PID get this treatment: `UTS`/`USER` are likewise vendored
unconditionally of their respective `CONFIG_*_NS` option, but no device has
been observed lacking `CONFIG_UTS_NS`/`CONFIG_USER_NS` while also lacking
`CONFIG_IPC_NS`/`CONFIG_PID_NS`, so no equivalent gap has been seen for
them. `NET`/`MNT`/`CGROUP` are deliberately **not** given the same
treatment: unlike IPC/PID, `vendor_kernel` has no real per-task namespace
object backing those on a kernel missing the corresponding `CONFIG_*_NS`,
so fabricating their `/proc/<pid>/ns/*` entries would misreport nonexistent
isolation as real.

`lkm4ctr/Kconfig`'s `LKM4CTR_VENDOR_KERNEL` option `select`s `CONFIG_UTS_NS`,
`CONFIG_PID_NS`, and `CONFIG_USER_NS` too (along with the rest), so any
future in-tree build sourcing that file still forces them on for
consistency and for any code elsewhere in the kernel that assumes them, but
`vendor_kernel` itself does not require any of the three any more for its
own uts/pid/user namespace support.

`glue/vendor_kernel_procfs.c` also fabricates `/proc/<pid>/setgroups` on a
kernel genuinely missing `CONFIG_USER_NS`: `fs/proc/base.c`
only wires up that per-pid dentry `#ifdef CONFIG_USER_NS`, so modern
`runc`/`containerd`'s unconditional `open()`/`openat2()` sanity-check of
`self/setgroups` (part of its "is this really an unrestricted procfs"
probe, run independent of whether the container itself asked for a new
user namespace) fails with plain `-ENOENT` and aborts container creation
with `"unsafe procfs detected ... setgroups: no such file or directory"`.
`open`/`openat`/`openat2` are hooked to let the real syscall run first and
only fabricate a descriptor once it has already failed with `-ENOENT` for a
path unambiguously naming a `"setgroups"` leaf under a procfs-rooted pid
directory. The fabricated descriptor is a simple one-way
`"allow"` -> `"deny"` latch on its own private inode (via
`anon_inode_getfd_secure()`, resolved through `shadow_hook_resolve()`)
rather than being wired to `vendor_kernel`'s own real per-task
`user_namespace` (`kernel/user_namespace.c`'s
`vns_proc_setgroups_show()`/`_write()`) — reproducing the exact
allow/deny/gid-map-set interactions real `setgroups(7)` has with a
specific `unshare(CLONE_NEWUSER)`'d namespace is unnecessary complexity for
what every observed caller only ever treats as a one-shot defensive probe.

## SysV IPC and POSIX mqueue (real isolation, not bookkeeping)

The vendored SysV IPC (`ipc/msg.c`, `ipc/sem.c`, `ipc/shm.c`, `ipc/util.c`, `ipc/msgutil.c`, `ipc/compat.c`) and POSIX message-queue (`ipc/mqueue.c`) implementations are compiled into `lkm4ctr.ko` and hooked to their syscalls by `glue/vendor_kernel_ipc_syscalls.c`, mirroring the `SHADOW_HOOK()` pattern used for `unshare`/`setns`/`clone` in `glue/vendor_kernel_syscalls.c`. All twelve SysV syscalls (`msgget`/`msgsnd`/`msgrcv`/`msgctl`, `semget`/`semop`/`semtimedop`/`semctl`, `shmget`/`shmat`/`shmdt`/`shmctl`) and all six mqueue syscalls (`mq_open`/`mq_unlink`/`mq_timedsend`/`mq_timedreceive`/`mq_notify`/`mq_getsetattr`) are redirected to their `vns_*` handlers.

- **Unconditional redirect.** On `vendor_kernel`'s primary target (`CONFIG_SYSVIPC=n && CONFIG_POSIX_MQUEUE=n`) the real syscalls are `sys_ni_syscall()` stubs returning `-ENOSYS`. Even so, `COND_SYSCALL(msgget)`/etc. in `kernel/sys_ni.c` still emit the `__arm64_sys_msgget`/`__x64_sys_msgget` symbols as weak aliases of the ni-stub, so `shadow_hook_resolve()` finds a real function entry to hook. Unlike the `unshare`/`clone` hooks (which pass native-flag requests through to the real syscall), the IPC hooks *always* call the vendored `vns_*` handler — there is nothing useful to pass through to.
- **Per-syscall-call namespace lookup gives real isolation.** Each vendored handler resolves the caller's effective `ipc_namespace` internally through `vns_current_ipc_ns()` → `vns_task_ipc_ns(current)` at call time, rather than any per-hook bookkeeping. A task that did `unshare(CLONE_NEWIPC)` (or `clone(CLONE_NEWIPC, …)`/`setns()`) has a real vendored `ipc_namespace` installed on `task_struct->nsproxy->ipc_ns` by `vns_copy_ipcs()`/`create_ipc_ns()` (`ipc/namespace.c`), so it operates against its own isolated message-queue / SysV IDRs; every other task shares the module-owned default. `CLONE_NEWIPC` is already part of `VNS_CLONE_FLAGS`, so this reuses the existing `vns_unshare_nsproxy_namespaces()` install path unchanged.
- **Module-owned default namespace, always vendored.** Tasks that never unshared need a fully-initialised namespace. `vns_ipc_default_init()` (in `glue/vendor_kernel_ipc_syscalls.c`, called from `vendor_kernel_init()` before the hooks go live) builds the mqueue half (`vns_mqueue_fs_init()`: registers the real `mqueue` filesystem type, installs the mq sysctls, runs `mq_init_ns()`; then `vns_mqueue_dev_ensure()` proactively creates and mounts a working `/dev/mqueue`, see "`/dev/mqueue` availability" below) and the SysV half (ipc sysctls + `msg_init_ns()`/`sem_init_ns()`/`shm_init_ns()`) on the vendored `vns_default_ipc_ns` singleton (upstream's `init_ipc_ns`, renamed via `#define init_ipc_ns vns_default_ipc_ns`) — replicating the piecemeal `device_initcall()`/`module_init()` setup that never runs for an out-of-tree module. This build is **unconditional**: it runs regardless of the running kernel's own `CONFIG_SYSVIPC`/`CONFIG_POSIX_MQUEUE`, and `vns_ipc_active_default()` **always** returns `&vns_default_ipc_ns`, never the real kernel's `init_ipc_ns`.
- **No dependency on the real kernel's ipc code, even when it ships it (self-contained like UTS/PID/USER_NS).** The IPC subsystem is deliberately as self-contained as the UTS/PID/USER namespaces are (see "Namespace refcounting is fully self-contained"): every namespace-setup call site in `create_ipc_ns()`/`vns_free_ipc_ns()` (`mq_init_ns`/`msg_init_ns`/`sem_init_ns`/`shm_init_ns`, `setup_mq_sysctls`/`setup_ipc_sysctls`/`retire_mq_sysctls`/`retire_ipc_sysctls`, `mq_put_mnt`/`mq_clear_sbinfo`) is unconditionally `#define`-aliased in `vendor_kernel.h` to its vendored `vns_*` implementation in `ipc/mqueue.c`/`ipc/msg.c`/`ipc/sem.c`/`ipc/shm.c` — none of them is gated behind `#ifdef CONFIG_SYSVIPC`/`#ifdef CONFIG_POSIX_MQUEUE`, so none can ever fall through to a real kernel symbol. `vns_init_ipc_ns_ptr` (the real, non-exported `init_ipc_ns`, resolved best-effort via `shadow_hook_resolve("init_ipc_ns")`) is kept purely for cosmetic bookkeeping and is **never** substituted for the vendored default: it is not written onto `vns_default_ipc_ns`, not returned by `vns_ipc_active_default()`, and `vns_init_nsproxy.ipc_ns` is pointed at the vendored default (`&vns_default_ipc_ns`) rather than at it. As a belt-and-braces guard, `vns_task_ipc_ns()` explicitly rejects `vns_init_ipc_ns_ptr` (via `vns_ipc_ns_is_vendored()`) and falls back to the vendored default — this catches the one inheritance path where a task that unshared a *non*-IPC namespace on a `CONFIG_SYSVIPC=y` kernel would otherwise carry the real `init_ipc_ns` by reference (`vns_copy_ipcs()` returns `get_ipc_ns(old)` when `CLONE_NEWIPC` is absent). The net effect: real-kernel-owned message-queue / SysV state can never enter the shadowed syscall paths.
- **`vns_free_ipc_ns()` fully tears down a per-task IPC namespace, mirroring upstream `free_ipc_ns()`.** `sem_exit_ns()`/`msg_exit_ns()`/`shm_exit_ns()` (`ipc/sem.c`/`ipc/msg.c`/`ipc/shm.c`, `#define`-aliased to `vns_sem_exit_ns`/`vns_msg_exit_ns`/`vns_shm_exit_ns`) are called from `ipc/namespace.c`'s `vns_free_ipc_ns()` exactly where upstream calls them, right after `mq_put_mnt(ns)`. A previous version of this function skipped all three, reasoning they were "not exported to out-of-tree modules" — but they don't need to be: they are vendored, non-static functions linked into the very same `lkm4ctr.ko`, resolved at link time like every other cross-file call in this module. Skipping them leaked every SysV queue/array/segment still registered in the namespace and, critically, never called `percpu_counter_destroy()` on `msg_exit_ns()`'s own `ns->percpu_msg_bytes`/`percpu_msg_hdrs` (`vns_msg_accounting_destroy()`), so the immediately-following `kfree(ns)` freed memory that was still linked into the kernel-wide `percpu_counters` list. That corrupted the list for the next *unrelated* `percpu_counter_init()` call anywhere in the kernel (observed via a real `cgroup_mkdir` -> `mem_cgroup_css_alloc` -> `wb_domain_init` -> `fprop_global_init` -> `__percpu_counter_init` call site, minutes after the container that triggered the leaking `unshare(CLONE_NEWIPC)` had already exited): `kernel BUG at lib/list_debug.c:29` ("list_add corruption"). This may also be the true root cause (or a contributing one) behind the similarly-shaped `percpu_counters` corruption documented below for `CLONE_NEWNET`/`xfrm4_net_init`, since both share the same global list.
- **Non-exported symbol resolution.** The vendored `ipc/*.c` pull in ~40 non-exported kernel helpers (mm, `wake_q`, ucounts, vfs, netlink, audit and the `security_*` LSM ipc/msg/sem/shm hooks) plus a handful of data symbols (`ipc_mni`, `ipc_mni_shift`, `ipc_min_cycle`, `sysctl_overcommit_memory`). `glue/vendor_kernel_ipc_compat.c` resolves the functions by name via `shadow_hook_resolve()` (reliable for kallsyms *function* symbols) with safe fallback stubs, and defines the data symbols directly from their upstream constant values; the POSIX-mqueue exact-name wrappers there also cover trimmed VFS/mount helpers such as `getname`/`putname`, `dentry_open`, `fs_context_for_mount`, `fc_mount`, `get_tree_{nodev,keyed}` and `simple_lookup`, so production GKI `CONFIG_TRIM_UNUSED_KSYMS` no longer leaves `mq_open()`/`mq_unlink()` unresolved at insmod time. This mirrors the `glue/vendor_kernel_compat.c` strategy used for the namespace core.
- **Non-target caveat.** On a kernel that genuinely ships `CONFIG_SYSVIPC=y`/`CONFIG_POSIX_MQUEUE=y` (e.g. the host used for local `make` type-checking), loading these hooks *shadows* the already-working syscalls and routes them through the vendored code operating on the vendored `vns_default_ipc_ns` (or a per-task vendored `ipc_namespace`) — **not** the running kernel's real `init_ipc_ns`, per the self-containment guarantee above. That is still not the intended deployment — `vendor_kernel` targets kernels where these configs are `n` — but it is harmless in practice since the vendored implementation is a faithful copy of the same kernel version's code, and it will never corrupt or read the host kernel's own message queues / SysV IDRs.
- **`/dev/mqueue` availability, without requiring `--ipc host`.** `mqueue_fs_type.name` (`ipc/mqueue.c`) is registered under the real name `"mqueue"`, not a module-private alias, so an unmodified `runc`/`containerd`/`dockerd`'s own `mount("mqueue", "/dev/mqueue", "mqueue", MS_NOSUID|MS_NODEV|MS_NOEXEC, ...)` during container init finds it through the ordinary `get_fs_type("mqueue")` lookup and just works — no `--ipc host` workaround needed. `register_filesystem()` tolerates losing that name to a real `CONFIG_POSIX_MQUEUE=y` kernel's own builtin mqueue filesystem (`-EBUSY`): `mqueue_fs_type_registered` tracks whether registration actually succeeded, so `vns_mqueue_fs_exit()`/the error path never call `unregister_filesystem()` on a struct that was never linked in, and `mq_create_mount()`'s own `fs_context_for_mount(&mqueue_fs_type, SB_KERNMOUNT)` (used for the module's own internal ipc_namespace bookkeeping) never depends on the name lookup succeeding either way, since it references the local struct pointer directly. On top of that, `glue/vendor_kernel_ipc_mount.c`'s `vns_mqueue_dev_ensure()` (called from `vns_ipc_default_init()`) proactively creates `/dev/mqueue` and mounts it at module load time, since `lkm4ctr.ko` is typically insmod'd late (e.g. a KernelSU/Magisk post-fs-data module), well after init.rc's own one-shot `mount mqueue mqueue /dev/mqueue ...` line already ran and silently failed with `-ENODEV` (init never retries a failed boot-time mount). It proactively mounts the real, vendored `mqueue` filesystem type first, falling back to `tmpfs` only if that unexpectedly fails. All of the VFS helpers this needs (`path_mount`, `vfs_mkdir`, `kern_path`/`kern_path_create`/`done_path_create`, `path_put`) are resolved at runtime via `shadow_hook_resolve()`, same as the rest of vendor_kernel's non-exported-symbol handling; this step is best-effort and never fails module init.
- **`sysvsem`/`sysvshm` exit-time state is self-contained too.** Upstream keeps the per-task semaphore-undo list (`task_struct->sysvsem`) and the per-task orphaned-shm-segment list (`task_struct->sysvshm`) as real `task_struct` members, but on a `CONFIG_SYSVIPC=n` target kernel those members don't exist in `struct task_struct` at all. `ipc/sem.c`/`ipc/shm.c` therefore keep this state in vendor_kernel's own per-task side table (the same `struct vns_task` registry `glue/vendor_kernel_module.c` already uses for the per-task `nsproxy` pointer) instead of the real `task_struct` fields: `vns_copy_semundo()`/`vns_prepare_exit_sem()`/`vns_exit_sem()` and `vns_prepare_exit_shm()`/`vns_exit_shm()` are called from the clone/exit paths (`glue/vendor_kernel_syscalls.c`'s `vendor_kernel_clone_track()`, `kernel/nsproxy.c`'s `vns_task_exit_cleanup()`) instead of the upstream `copy_semundo()`/`exit_sem()`/`exit_shm()` call sites baked into the real `fork()`/`do_exit()`. On a target that genuinely ships `CONFIG_SYSVIPC=y` (so `task_struct` does carry real `sysvsem`/`sysvshm`), the real fields are used directly instead, guarded by `#if defined(CONFIG_SYSVIPC)`.

## Module-owned default cgroup namespace, always vendored

`vns_default_cgroup_ns` (`kernel/cgroup/namespace.c`) is a module-owned default
`cgroup_namespace` singleton, analogous to `vns_default_ipc_ns`/`vns_init_time_ns`.
`vendor_kernel_init()` calls `vns_cgroup_default_init()` (pinning its refcount to
a large sentinel, same pattern as `vns_init_nsproxy.count`) and unconditionally
points `vns_init_nsproxy.cgroup_ns` at it -- **never** at the running kernel's
real `init_cgroup_ns`, regardless of whether `shadow_hook_resolve("init_cgroup_ns")`
succeeds. `vns_init_cgroup_ns_ptr` is still resolved for cosmetic bookkeeping only
(mirroring `vns_init_ipc_ns_ptr`) and is never installed anywhere.

This closes the one remaining case where `vendor_kernel`'s pinned default
nsproxy depended on the target kernel's own resolved namespace object instead
of a vendored one, bringing `CGROUP_NS` bookkeeping in line with
`UTS_NS`/`PID_NS`/`USER_NS`/`IPC_NS`/overlayfs. It does **not** change the
scope of cgroup namespace *isolation* itself: `vns_copy_cgroup_ns()` still
unconditionally returns the caller's existing `cgroup_ns` (`get_cgroup_ns(old_ns)`)
rather than allocating a new one on `unshare(CLONE_NEWCGROUP)`, so
`vns_default_cgroup_ns.root_cset` is deliberately left `NULL` and never
dereferenced -- building a real, isolated per-namespace `root_cset` would
require duplicating the running kernel's non-exported cgroup core (`css_set`
table, `cgroup_mutex`, `task_css_set()`), which is out of scope for this
module (see "Known remaining gaps" below).

## Known remaining gaps

- `SHM_HUGETLB` shared-memory segments are a best-effort gap: `ipc/shm.c` references the running kernel's hugetlb `hstates[]`/`default_hstate_idx`/`size_to_hstate()`, which are not exported and are absent entirely on `CONFIG_HUGETLB_PAGE=n`. `glue/vendor_kernel_ipc_compat.c` defines these as zeroed/NULL-returning stubs, so `shmget(..., SHM_HUGETLB)` fails cleanly with `-EINVAL` (`shm.c` null-checks `hstate_sizelog()`) rather than doing anything unsafe; ordinary (non-hugetlb) `shmget()` is fully functional.
- `NET_NS` and `MNT_NS` are still resolved via optional function pointers (`vns_copy_net_ns_fn`/`vns_copy_mnt_ns_fn` in `glue/vendor_kernel_module.c`) rather than being fully vendored, so they still silently fall back to bookkeeping-only/no-op behaviour on a target kernel with `CONFIG_NET_NS=n`/`CONFIG_NAMESPACES` MNT support missing.
- `CGROUP_NS` bookkeeping (`vns_init_nsproxy.cgroup_ns`, `/proc/<pid>/ns/cgroup` refcounting) is fully vendored and independent of the target's `CONFIG_CGROUPS` setting (see "Module-owned default cgroup namespace, always vendored" above), but `unshare(CLONE_NEWCGROUP)` itself remains bookkeeping-only: `vns_copy_cgroup_ns()` always returns the caller's existing `cgroup_namespace` rather than allocating an isolated one, since a real one requires a live `root_cset` from the running kernel's own (non-exported) cgroup hierarchy. Same posture as `NET_NS`/`MNT_NS` above.
- `CLONE_NEWNET` is *always* bookkeeping-only (`create_new_namespaces()` in `kernel/nsproxy.c` masks `CLONE_NEWNET` out of the flags it passes to the real, resolved `copy_net_ns()`), even on a target kernel that genuinely has `CONFIG_NET_NS=y` and where `copy_net_ns()` resolves successfully. Letting `copy_net_ns()` actually build a brand-new `struct net` from this call site was observed to corrupt the kernel-wide `percpu_counters` list the first time it ran (`kernel BUG at lib/list_debug.c:29`, call trace `xfrm4_net_init` -> `__percpu_counter_init` -> `ops_init` -> `setup_net` -> `copy_net_ns` -> `create_new_namespaces` [lkm4ctr]), even though every other namespace type built alongside it in the same call is unaffected. The exact mechanism was not fully root-caused (no live KASAN/SLUB-debug reproduction was available), so real `NET_NS` isolation is disabled here rather than risk that crash; `unshare(CLONE_NEWNET)`/`ns_net` reports STUB (no real isolation) instead of PASS.
- `kernel/cgroup/namespace.c` and `kernel/time/namespace.c` store `user_ns` without taking a reference on it (`(void)user_ns;` in their copy functions), unlike `kernel/pid_namespace.c`/`kernel/user_namespace.c`/`kernel/utsname.c`/`ipc/namespace.c` which now use `vns_get_user_ns()`/`vns_put_user_ns()`; this is a pre-existing gap unrelated to `CONFIG_PID_NS`/`CONFIG_USER_NS` and was not changed here.
- `cred->user_ns` can be freed independently of nsproxy teardown via a deferred RCU callback (`put_cred_rcu()`), decoupled from task exit -- see "Slab-cache consistency with the real kernel" above for why this is an accepted, no-op-leak-only gap.
- overlayfs upper/work directory cloning failures are an in-tree overlayfs behavior on the stock vendor kernel binary; `vendor_kernel` (an out-of-tree LKM) cannot patch code that is already compiled into the running kernel, so this is out of scope for this module.

## Vendoring rules

- upstream sources for `kernel/`, `ipc/`, and `fs/nsfs.c` were copied from `kernel-common` `android14-6.1` (kernel `6.1.124`); the vendored overlayfs sources cover 5 separate KMI eras, each copied verbatim from its own real upstream branch (see "Vendored overlayfs" below)
- all non-static global symbols are renamed with a `vns_` prefix
- slab-cache users for `ipc_namespace`/`cgroup_namespace`/`time_namespace` are `kzalloc`/`kfree` (matches upstream, which also uses plain kzalloc/kmalloc for these); `uts_namespace`/`nsproxy`/`pid_namespace`/`user_namespace` allocate/free through vendor_kernel's own module-owned `kmem_cache_create()` caches (see "Slab-cache consistency with the real kernel" above), never the real kernel's private caches
- `get_uts_ns()`/`put_uts_ns()`/`get_pid_ns()`/`put_pid_ns()`/`get_user_ns()`/`put_user_ns()` call sites are replaced with `vns_get_uts_ns()`/`vns_put_uts_ns()`/`vns_get_pid_ns()`/`vns_put_pid_ns()`/`vns_get_user_ns()`/`vns_put_user_ns()` (see "Namespace refcounting is fully self-contained" above), and the `#ifdef CONFIG_PID_NS`/`#ifdef CONFIG_USER_NS` blocks gating pid/user namespace installation in `vns_sys_setns()`'s helpers are made unconditional, for the same reason
- inode-number allocation uses `vns_alloc_inum()` / `vns_free_inum()`
- diagfs statistics are emitted through `vendor_kernel_diag_snprintf()`

## Vendored files

- `kernel/utsname.c`
- `kernel/nsproxy.c`
- `kernel/pid_namespace.c`
- `kernel/user_namespace.c`
- `ipc/namespace.c`
- `ipc/util.c`
- `ipc/msgutil.c`
- `ipc/msg.c`
- `ipc/sem.c`
- `ipc/shm.c`
- `ipc/mqueue.c`
- `ipc/compat.c`
- `fs/nsfs.c`
- `kernel/cgroup/namespace.c`
- `kernel/time/namespace.c`
- `fs/overlayfs/super.c` (and `namei.c`, `util.c`, `inode.c`, `dir.c`, `readdir.c`, `copy_up.c`, `export.c`, `file.c`, `overlayfs.h`, `ovl_entry.h`) - `[6.1, 6.3)` era (`android14-6.1`)
- `fs/overlayfs_5_10/*` - same 11 files as above, `[5.10, 5.15)` era (`android12-5.10`, `android13-5.10`)
- `fs/overlayfs_5_15/*` - same 11 files as above, `[5.15, 6.1)` era (`android13-5.15`, `android14-5.15`)
- `fs/overlayfs_6_6/*` - same 11 files plus `params.c`/`params.h`, `[6.6, 6.7)` era (`android15-6.6`)
- `fs/overlayfs_6_12/*` - same 13 files plus `xattrs.c`, `[6.12, 6.13)` era (`android16-6.12`)

### Vendored overlayfs

Each of the 5 directories above (`fs/overlayfs/`, `fs/overlayfs_5_10/`,
`fs/overlayfs_5_15/`, `fs/overlayfs_6_6/`, `fs/overlayfs_6_12/`) is vendored
verbatim from kernel-common at the exact upstream branch noted above,
upstream's own out-of-tree-buildable overlay filesystem, with the same
minimal edits applied independently in each era's `super.c` (and, for the
`6.6`/`6.12` eras, `ovl_entry.h`, since `ovl_fs_type` is already declared
`extern` there rather than `static`):

- `module_init(ovl_init)`/`module_exit(ovl_exit)` were replaced with plain
  `vns_ovl_init()`/`vns_ovl_exit()` functions (renamed from `ovl_init`/
  `ovl_exit`, and no longer `static`), since lkm4ctr.ko already has a
  single `module_init`/`module_exit` pair in `lkm4ctr_main.c`. These are
  chained in from `vendor_kernel_init()`/`vendor_kernel_exit()` via
  `glue/vendor_kernel_overlay.c`.
- The `file_system_type` (`vns_ovl_fs_type`, renamed from the static
  `ovl_fs_type` so `glue/vendor_kernel_overlay.c` can reference it) keeps
  upstream's name, `"overlay"`, but is **never** passed to
  `register_filesystem()`/`unregister_filesystem()`. Instead,
  `glue/vendor_kernel_overlay.c` hooks the exported `get_fs_type()` kernel
  function (the same lookup `mount(2)` itself uses to resolve a filesystem
  name) and, for the name `"overlay"`, always hands out `vns_ovl_fs_type`
  -- unconditionally, regardless of whether the running kernel also ships
  its own built-in overlayfs or a separate `overlay.ko`. This means
  `mount -t overlay ...` always uses this vendored implementation while
  `lkm4ctr.ko` is loaded, with zero risk of a `register_filesystem()`
  `-EBUSY` collision against a real one (since it is never registered into
  the global `file_systems` list at all).
- `vns_ovl_mount_count` (new, not upstream) tracks live vendored-overlay
  mounts (incremented in `ovl_fill_super()` on success, decremented in the
  new `vns_ovl_kill_sb()` wrapper around `kill_anon_super()`) and is
  surfaced through vendor_kernel's diagfs status
  (`glue/vendor_kernel_diag.c`, via `vns_overlay_diag_snprintf()`).

**KMI support range**: every KMI in `.github/workflows/build-lkm4ctr.yml`
is now covered by exactly one of 5 individually verified vendored overlayfs
eras, each copied verbatim from the real upstream branch noted below and
compile-tested (`make ... modules`) against that branch's own
`kernel-common` checkout:

| Era directory                | `LINUX_VERSION_CODE` range | Source branch(es)                          | KMI(s) covered                  |
|-------------------------------|----------------------------|---------------------------------------------|----------------------------------|
| `fs/overlayfs_5_10/`          | `[5.10, 5.15)`              | `android12-5.10` (byte-identical to `android13-5.10`) | `android12-5.10`, `android13-5.10` |
| `fs/overlayfs_5_15/`          | `[5.15, 6.1)`               | `android14-5.15` (near-identical to `android13-5.15`) | `android13-5.15`, `android14-5.15` |
| `fs/overlayfs/`               | `[6.1, 6.3)`                | `android14-6.1`                              | `android14-6.1`                 |
| `fs/overlayfs_6_6/`           | `[6.6, 6.7)`                | `android15-6.6`                              | `android15-6.6`                 |
| `fs/overlayfs_6_12/`          | `[6.12, 6.13)`              | `android16-6.12`                             | `android16-6.12`                |

Every file in every era wraps its whole body in its own
`#if LINUX_VERSION_CODE >= KERNEL_VERSION(...) && LINUX_VERSION_CODE <
KERNEL_VERSION(...)` guard (see the `[BUILD-COMPAT]` comment at the top of
each file), compiling to an empty translation unit outside its range
instead of failing the build against a mismatched VFS/fs_context API.
`glue/vendor_kernel_overlay.c` gates its `get_fs_type()` override with the
union of all 5 ranges above; outside every covered range,
`vns_overlay_init()` skips installing the override and returns 0 (so the
rest of `lkm4ctr.ko` still loads normally), and `mount -t overlay ...`
falls back to the running kernel's own overlay implementation.

The `6.6` and `6.12` eras use the newer `fs_context`-based mount API
(`.init_fs_context` instead of `.mount`) and gained `params.c`/`params.h`
(mount-option parsing split out of `super.c`) and, in `6.12`, a separate
`xattrs.c`; both are otherwise vendored and transformed the same way as the
other 3 eras. `6.12`'s `namei.c` also replaces its private, non-exported
`#include "../internal.h"` (used only for the `vfs_path_lookup()`
prototype, itself `EXPORT_SYMBOL_NS(vfs_path_lookup,
ANDROID_GKI_VFS_EXPORT_ONLY)`-exported and thus safely linkable) with a
direct prototype declaration, since out-of-tree modules cannot see private
kernel headers.

Two ranges are deliberately left unsupported since no KMI in the build
matrix uses them: `[6.3, 6.6)` and `[6.7, 6.12)`. These are real VFS API
transitions, but only exact, verified upstream sources are vendored here --
speculative cross-version porting of unverified intermediate APIs is
avoided.

## Helper files

- `vendor_kernel.h` - shared internal declarations
- `glue/vendor_kernel_module.c` - lifecycle, symbol resolution, registry
- `glue/vendor_kernel_syscalls.c` - syscall hooks; installs real namespaces via `vns_switch_task_namespaces()`
- `glue/vendor_kernel_ipc_syscalls.c` - SysV IPC + POSIX mqueue syscall hooks; module-owned default `ipc_namespace` init/exit
- `glue/vendor_kernel_ipc_compat.c` - resolves/stubs the non-exported mm/vfs/netlink/audit/security/ucounts symbols the vendored `ipc/*.c` pull in
- `glue/vendor_kernel_overlay.c` - vendored overlayfs lifecycle + `get_fs_type("overlay")` hook (see "Vendored overlayfs" above)
- `glue/vendor_kernel_diag.c` - diagfs renderer
- `include/uapi/vendor_kernel.h` - minimal UAPI marker header

## Diffing against upstream

Run:

```sh
./vendor_kernel_diff.sh [path-to-kernel-common]
```

With no argument it defaults to `$RUNNER_TEMP/kernel-common`.
