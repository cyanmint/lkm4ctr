# lkm4ctr — container-support kernel modules for GKI

`lkm4ctr/` now builds a single out-of-tree kernel module,
**`lkm4ctr.ko`**, that makes a stock, unpatched
`containerd`/`runc`/`dockerd` run on Android GKI kernels that were built
without the usual container prerequisites (`CONFIG_*_NS`, `CONFIG_SYSVIPC`,
`CONFIG_POSIX_MQUEUE`, cgroup-v1 device control, …).

The source remains split by subsystem under `lkm4ctr/lkm4ctr/` for
maintainability, but build/load/deploy is unified again: one Makefile, one
module entry point, one `.ko`.

## Module map

| Component             | Directory                         | Summary |
|-----------------------|-----------------------------------|---------|
| `lkm4ctr.ko`          | `lkm4ctr/`                        | nified module containing the shared hook engine plus the namespace, SysV IPC, POSIX mqueue and cgroup-device compatibility subsystems. |
| `shadow_hijack`       | `lkm4ctr/shadow_hijack/`          | Internal shared ftrace/kprobe hook implementation used by the other subsystems inside `lkm4ctr.ko`. |
| `shadow_ns`           | `lkm4ctr/shadow_ns/`              | `unshare/setns/clone/clone3/fork/vfork` hooks. Real per-namespace isolation for UTS, PID, USER and (for the SysV IPC data path, via `shadow_sysvipc`) IPC; bookkeeping only for NET/CGROUP/MNT when genuinely absent. |
| `shadow_sysvipc`      | `lkm4ctr/shadow_sysvipc/`         | System V IPC (msg/sem/shm) hooks and shadow registry. |
| `shadow_mqueue`       | `lkm4ctr/shadow_mqueue/`          | POSIX mqueue hooks plus real shadow message transfer. |
| `shadow_cgdevices`    | `lkm4ctr/shadow_cgdevices/`       | Transparent device-open hook shim for the cgroup-device compatibility slot. |
| `lkm4ctr_checker`     | `lkm4ctr_checker/`                | **Userspace** diagnostic binary (not a kernel module): performs real syscalls and reports PASS/STUB/FAIL per feature. |

### Real vs. bookkeeping vs. stub — a quick reference

Because every subsystem in this family hooks/simulates functionality that would
normally be compiled into `vmlinux`, "supported" does not always mean the same
thing. This table is the single place that spells out, per subsystem (and per
namespace type inside `shadow_ns`), whether the simulation is **real**
(behaves like the native kernel feature, verified by observable side effects),
**bookkeeping-only** (state is tracked and syscalls succeed, but there is no
functional isolation/enforcement behind it), or a **stub** (a hook exists but
currently only preserves/passes through native behaviour, i.e. it does not yet
change anything). See each module's own README for the full rationale.

| Component                          | Classification | Why |
|-------------------------------------|-----------------|-----|
| `shadow_ns` — UTS namespace          | **Real**        | `uname()`/`sethostname()` after `unshare(CLONE_NEWUTS)` observe a genuinely separate nodename/domainname per simulated namespace. |
| `shadow_ns` — PID namespace          | **Real**        | vpid↔rpid remapping means `getpid()`/`/proc` inside a simulated PID namespace show virtual, namespace-local PIDs distinct from the real ones. |
| `shadow_ns` — USER namespace         | **Real**        | uid/gid 0 remapping gives genuinely different credential mapping inside vs. outside the simulated namespace. |
| `shadow_ns` — IPC namespace          | **Real** (SysV IPC)  | A separate namespace id is tracked on `unshare`/`setns`/`clone(CLONE_NEWIPC)`, and `shadow_sysvipc`'s transparent `msgget`/`semget`/`shmget`/... hooks route any task that is a member of a simulated IPC namespace through a namespace-scoped shadow registry instead of the real, un-partitioned `init_ipc_ns` — so SysV message queues/semaphores/shared memory are genuinely partitioned per simulated namespace. POSIX message queues are not covered by this and remain unpartitioned. |
| `shadow_ns` — NET namespace          | **Bookkeeping**  | A separate namespace id/refcount is tracked, but no network-stack partitioning is provided. |
| `shadow_ns` — CGROUP namespace       | **Bookkeeping** (only if `CONFIG_CGROUPS=n`) | Same generic id/refcount registry as IPC/NET, used only on the (rare — no GKI defconfig disables it) kernel builds without `CONFIG_CGROUPS`; otherwise always builtin/passthrough. |
| `shadow_ns` — MNT namespace          | bookkeeping (only if `CONFIG_NAMESPACES=n`, effectively never in practice)   | Mount namespaces have no dedicated per-type Kconfig gate anywhere in mainline Linux, so `shadow_ns` keys MNT's builtin status off `CONFIG_NAMESPACES` itself as a defensive fallback; the real, always-compiled-in mount-namespace code keeps running regardless, this only adds a parallel bookkeeping entry. |
| `shadow_sysvipc` (msg/sem/shm)        | **Real**        | Maintains an actual object registry (ids, keys, lifecycle) behind the hooked syscalls — not a stub that just returns success. |
| `shadow_mqueue` (POSIX mqueue)        | **Real**        | Real message transfer: priority-ordered queue, blocking send/receive with timeout semantics, real anon-inode-backed fds. |
| `shadow_cgdevices` (`chrdev_open`)     | **Stub**        | Hook installed but currently only preserves native behaviour; no rule enforcement yet. |
| `shadow_cgdevices` (`blkdev_open`)     | **Stub, best-effort** | Same as above, and only installed if the symbol exists with the expected prototype on that KMI. |
| `lkm4ctr_checker`                  | n/a (diagnostics) | **Userspace binary**, not a kernel module: actually attempts the relevant syscalls and reports PASS/STUB/FAIL based on the observed effect, rather than reporting compile-time config alone. |

Shared, header-only helpers live in `common/`:

| File                          | Purpose |
|-------------------------------|---------|
| `common/shadow_hook.h`        | ftrace/kprobe syscall-hijack helper used by every hooking subsystem. |
| `common/lkm4ctr_compat.h`  | `fd_file()`/`fd_empty()` compat shims for kernels < 6.8 (used by `shadow_mqueue`). |
| `common/shadow_hook.README.md`| Documentation for the hook helper. |

## Load order

There is now exactly one kernel module to load:

```sh
insmod lkm4ctr/lkm4ctr/lkm4ctr.ko

# diagnostics: a plain userspace binary, run any time, no insmod needed:
./lkm4ctr_checker/lkm4ctr_checker
```

`lkm4ctr_checker` is a **userspace** diagnostic program, not a kernel
module: it has no build-time or load-time dependency on `lkm4ctr.ko`, and
instead of reporting
compile-time `IS_ENABLED(CONFIG_*)` facts, it directly performs the relevant
syscalls (`unshare`/`fork`/`setns`, `mq_*`, `msg*`, `mount`) and reports
PASS/STUB/FAIL based on their actual observed effect. See
`lkm4ctr_checker/README.md` for the full methodology and why this
replaced the earlier kernel-module version.

## Building

The merged kernel module lives in `lkm4ctr/lkm4ctr/` as a dual-purpose
kbuild module (works both out-of-tree via `make KDIR=...` and embedded in an
in-tree `obj-$(CONFIG_...)` build). Out-of-tree, against a prepared kernel
build tree:

```sh
make -C /path/to/kernel/build M="$PWD/lkm4ctr/lkm4ctr" modules
```

`lkm4ctr_checker` is a plain userspace program and builds with a normal C
compiler — no `KDIR`/kernel build tree involved:

```sh
make -C lkm4ctr_checker                          # host toolchain
make -C lkm4ctr_checker CC="clang --target=aarch64-linux-gnu"  # cross build
```

Out-of-tree modules for GKI **must** be built inside the matching
`ghcr.io/ylarod/ddk-min:<kmi>-<release>` DDK image against its real
`vmlinux`/`Module.symvers`, not a bare `gki_defconfig` tree, or `insmod` will
panic on the real kernel. See
[`../.github/workflows/build-lkm4ctr.yml`](../.github/workflows/build-lkm4ctr.yml)
(merged-module build matrix, plus a QEMU boot/load test of the android14-6.1
build).

## Kernel compatibility

`common/shadow_hook.h` picks its hooking backend at compile time: ftrace-based
when `CONFIG_FUNCTION_TRACER`/`CONFIG_DYNAMIC_FTRACE` are available, and a
kprobe-`pre_handler` fallback otherwise — the latter is what runs on stock
Android GKI kernels, which ship with `CONFIG_FUNCTION_TRACER` disabled.

`shadow_mqueue`'s `fd_file()`/`fd_empty()` use targets a kernel API that only
exists from Linux v6.8 onward; `common/lkm4ctr_compat.h` provides shims so
the same source builds unmodified against older GKI branches (e.g. 6.1).

See each subsystem's own README for its honest scope/limitations. In
particular, `shadow_ns` only ever simulates a namespace type genuinely absent
from this
kernel build (`IS_ENABLED(CONFIG_*_NS)`, which collapses correctly even when
`CONFIG_NAMESPACES` is disabled entirely) — when it does simulate, UTS/PID/USER
get real functional isolation; IPC/NET simulation (when needed) remains
reference-counted bookkeeping only. See
[`lkm4ctr/shadow_ns/README.md`](lkm4ctr/shadow_ns/README.md) for the full
design rationale, and the "Real vs. bookkeeping vs. stub" table above for the
full picture across every subsystem in this family.
