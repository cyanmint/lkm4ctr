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
| `lkm4ctr.ko`          | `lkm4ctr/`                        | Unified module containing the shared hook engine plus the vendored namespace/IPC/mqueue and cgroup-device compatibility subsystems. |
| `shadow_hijack`       | `lkm4ctr/shadow_hijack/`          | Internal shared ftrace/kprobe hook implementation used by the other subsystems inside `lkm4ctr.ko`. |
| `vendor_kernel`       | `lkm4ctr/vendor_kernel/`          | Vendored namespace/IPC/mqueue/overlayfs implementation: namespace syscalls, `/proc` namespace visibility, SysV IPC, POSIX mqueue support, and an always-used vendored overlayfs (`get_fs_type("overlay")` hook). |
| `shadow_cgdevices`    | `lkm4ctr/shadow_cgdevices/`       | Transparent device-open hook shim for the cgroup-device compatibility slot. |
| `lkm4ctr_checker`     | `lkm4ctr_checker/`                | **Userspace** diagnostic binary (not a kernel module): performs real syscalls and reports PASS/STUB/FAIL per feature. |

### Real vs. bookkeeping vs. stub — a quick reference

Because every subsystem in this family hooks/simulates functionality that would
normally be compiled into `vmlinux`, "supported" does not always mean the same
thing. This table is the single place that spells out, per subsystem (and per
namespace type inside `vendor_kernel`), whether the simulation is **real**
(behaves like the native kernel feature, verified by observable side effects),
**bookkeeping-only** (state is tracked and syscalls succeed, but there is no
functional isolation/enforcement behind it), or a **stub** (a hook exists but
currently only preserves/passes through native behaviour, i.e. it does not yet
change anything). See each module's own README for the full rationale.

| Component                          | Classification | Why |
|-------------------------------------|-----------------|-----|
| `vendor_kernel` — UTS namespace      | **Real**        | `uname()`/`sethostname()` after `unshare(CLONE_NEWUTS)` observe a genuinely separate nodename/domainname per vendored namespace. |
| `vendor_kernel` — PID namespace      | **Real**        | vpid remapping plus `/proc` integration mean `getpid()` and procfs inside a vendored PID namespace show namespace-local PIDs distinct from the host ones. |
| `vendor_kernel` — USER namespace     | **Real**        | uid/gid remapping gives genuinely different credential mapping inside vs. outside the vendored namespace. |
| `vendor_kernel` — IPC namespace      | **Real**        | A vendored `ipc_namespace` is installed on `task_struct->nsproxy`, and the hooked SysV IPC and POSIX mqueue syscalls operate on that namespace-scoped state instead of the host default. |
| `vendor_kernel` — NET namespace      | **Bookkeeping** | A separate namespace id/refcount is tracked, but no network-stack partitioning is provided. |
| `vendor_kernel` — CGROUP namespace   | **Kernel-provided or bookkeeping-only** | Uses the real kernel cgroup namespace support when present; on kernels lacking it, only bookkeeping remains. |
| `vendor_kernel` — MNT namespace      | **Kernel-provided** | Relies on the running kernel's mount-namespace implementation; no separate vendored mount-namespace core is provided. |
| `vendor_kernel` — overlayfs          | **Real**        | A vendored `fs/overlayfs` always answers `get_fs_type("overlay")` (hooked), so `mount -t overlay ...` genuinely mounts and operates through this module's own overlay implementation, regardless of the running kernel's own overlayfs support. |
| `shadow_cgdevices` (`chrdev_open`)     | **Stub**        | Hook installed but currently only preserves native behaviour; no rule enforcement yet. |
| `shadow_cgdevices` (`blkdev_open`)     | **Stub, best-effort** | Same as above, and only installed if the symbol exists with the expected prototype on that KMI. |
| `lkm4ctr_checker`                  | n/a (diagnostics) | **Userspace binary**, not a kernel module: actually attempts the relevant syscalls and reports PASS/STUB/FAIL based on the observed effect, rather than reporting compile-time config alone. |

Shared, header-only helpers live in `common/`:

| File                          | Purpose |
|-------------------------------|---------|
| `common/shadow_hook.h`        | ftrace/kprobe syscall-hijack helper used by every hooking subsystem. |
| `common/lkm4ctr_compat.h`  | `fd_file()`/`fd_empty()` compat shims for kernels < 6.8 (used by `vendor_kernel`). |
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

`vendor_kernel`'s overlayfs support now comes from a single vendored
`lkm4ctr/vendor_kernel/fs/overlayfs/` tree (android16-6.12 baseline).
`lkm4ctr/vendor_kernel/glue/vendor_kernel_ovl_vfs_compat.{h,c}` adapts that
one source tree across the three supported VFS/API tiers: `OLD`
`[5.10, 5.12)`, `MID` `[5.12, 6.3)`, and `NEW` `[6.3, 6.19)`.

`vendor_kernel`'s procfs and mqueue glue uses `fd_file()`/`fd_empty()`, a
kernel API that only
exists from Linux v6.8 onward; `common/lkm4ctr_compat.h` provides shims so
the same source builds unmodified against older GKI branches (e.g. 6.1).

See each subsystem's own README for its honest scope/limitations. In
particular, `vendor_kernel` provides the real vendored UTS/PID/USER/IPC/mqueue
paths and documents the remaining NET/MNT/CGROUP caveats in
[`lkm4ctr/vendor_kernel/README.md`](lkm4ctr/vendor_kernel/README.md); see that
README plus the "Real vs. bookkeeping vs. stub" table above for the full
picture across the current subsystem set.
