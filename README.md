# lkm4ctr — container-support kernel modules for GKI

`lkm4ctr` builds a single out-of-tree kernel module,
**`lkm4ctr.ko`**, that makes a stock, unpatched
`containerd`/`runc`/`dockerd` run on Android GKI kernels that were built
without the usual container prerequisites (`CONFIG_*_NS`, `CONFIG_SYSVIPC`,
`CONFIG_POSIX_MQUEUE`, cgroup-v1 device control, …).

The project is now a flat, unified tree: one Makefile, one module entry
point, one `.ko`. `glue/` at the repository root holds all hand-written
code, and `vendor/` holds every vendored source tree; only `glue/` and
`lkm4ctr_checker/` are code this project writes, everything under
`vendor/` is vendored.

## Module map

| Component             | Directory                         | Summary |
|-----------------------|-----------------------------------|---------|
| `lkm4ctr.ko`          | `vendor/`                         | Unified module build directory (kbuild `Makefile`/`Kconfig`) containing the shared hook engine plus the vendored namespace/IPC/mqueue/overlayfs compatibility subsystem. |
| `glue/`               | `glue/`                           | Hand-written code: the shared ftrace/kprobe hook engine (`shadow_hook.h`/`shadow_hijack.c`), module entry/exit, the `lkm4ctr` diagfs, hot reload, logging, and the vendor/overlayfs compat glue. |
| `vendor_kernel`       | `vendor/{kernel,ipc,fs}/`         | Vendored namespace/IPC/mqueue/overlayfs implementation: namespace syscalls, `/proc` namespace visibility, SysV IPC, POSIX mqueue support, and an always-used vendored overlayfs (`get_fs_type("overlay")` hook). |
| `lkm4ctr_checker`     | `lkm4ctr_checker/`                | **Userspace** diagnostic binary (not a kernel module): performs real syscalls and reports PASS/STUB/FAIL per feature. |

### Real vs. bookkeeping vs. stub — a quick reference

Because this module hooks/simulates functionality that would
normally be compiled into `vmlinux`, "supported" does not always mean the same
thing. This table is the single place that spells out, per namespace type
inside `vendor_kernel`, whether the simulation is **real**
(behaves like the native kernel feature, verified by observable side effects),
**bookkeeping-only** (state is tracked and syscalls succeed, but there is no
functional isolation/enforcement behind it), or a **stub** (a hook exists but
currently only preserves/passes through native behaviour, i.e. it does not yet
change anything). See `vendor/README.md` for the full rationale.

| Component                          | Classification | Why |
|-------------------------------------|-----------------|-----|
| `vendor_kernel` — UTS namespace      | **Real**        | `uname()`/`sethostname()` after `unshare(CLONE_NEWUTS)` observe a genuinely separate nodename/domainname per vendored namespace. |
| `vendor_kernel` — PID namespace      | **Real**        | vpid remapping plus `/proc` integration mean `getpid()` and procfs inside a vendored PID namespace show namespace-local PIDs distinct from the host ones; `reboot(2)` (`glue/vendor_kernel_syscalls.c`) is also hooked to route non-init pid namespaces through `vns_reboot_pid_ns()` (killing the namespace's `child_reaper` via `SIGKILL`/`SIGHUP`/`SIGINT`) instead of rebooting the host. |
| `vendor_kernel` — USER namespace     | **Real**        | A real per-task `user_namespace` is installed after `unshare(CLONE_NEWUSER)`, with genuinely wired `/proc/<pid>/{uid_map,gid_map,projid_map,setgroups}` (`glue/vendor_kernel_procfs_userns.c`) and `getuid`/`setuid`/etc. syscall hooks (`glue/vendor_kernel_syscalls_userns.c`) that remap through it, so credentials genuinely differ inside vs. outside the vendored namespace even on kernels lacking `CONFIG_USER_NS`. |
| `vendor_kernel` — IPC namespace      | **Real**        | A vendored `ipc_namespace` is installed on `task_struct->nsproxy`, and the hooked SysV IPC and POSIX mqueue syscalls operate on that namespace-scoped state instead of the host default. |
| `vendor_kernel` — NET namespace      | **Bookkeeping** | A separate namespace id/refcount is tracked, but no network-stack partitioning is provided. |
| `vendor_kernel` — CGROUP namespace   | **Kernel-provided or bookkeeping-only** | Uses the real kernel cgroup namespace support when present; on kernels lacking it, only bookkeeping remains. |
| `vendor_kernel` — MNT namespace      | **Kernel-provided** | Relies on the running kernel's mount-namespace implementation; no separate vendored mount-namespace core is provided. |
| `vendor_kernel` — overlayfs          | **Real**        | A vendored `fs/overlayfs` always answers `get_fs_type("overlay")` (hooked), so `mount -t overlay ...` genuinely mounts and operates through this module's own overlay implementation, regardless of the running kernel's own overlayfs support. |
| `lkm4ctr_checker`                  | n/a (diagnostics) | **Userspace binary**, not a kernel module: actually attempts the relevant syscalls and reports PASS/STUB/FAIL based on the observed effect, rather than reporting compile-time config alone. |

Shared, header-only helpers live in `glue/`:

| File                          | Purpose |
|-------------------------------|---------|
| `glue/shadow_hook.h`        | ftrace/kprobe syscall-hijack ABI used by `vendor_kernel`. |
| `glue/lkm4ctr_compat.h`  | `fd_file()`/`fd_empty()` compat shims for kernels < 6.8 (used by `vendor_kernel`). |
| `glue/shadow_hook.README.md`| Documentation for the hook helper. |

## Load order

There is now exactly one kernel module to load:

```sh
insmod vendor/lkm4ctr.ko

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

The merged kernel module lives in `vendor/` as a dual-purpose
kbuild module (works both out-of-tree via `make KDIR=...` and embedded in an
in-tree `obj-$(CONFIG_...)` build). Out-of-tree, against a prepared kernel
build tree:

```sh
make -C /path/to/kernel/build M="$PWD/vendor" modules
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
[`.github/workflows/build-lkm4ctr.yml`](.github/workflows/build-lkm4ctr.yml)
(merged-module build matrix, plus a QEMU boot/load test of the android14-6.1
build).

## Kernel compatibility

`glue/shadow_hook.h` picks its hooking backend at compile time: ftrace-based
when `CONFIG_FUNCTION_TRACER`/`CONFIG_DYNAMIC_FTRACE` are available, and a
kprobe-`pre_handler` fallback otherwise — the latter is what runs on stock
Android GKI kernels, which ship with `CONFIG_FUNCTION_TRACER` disabled.

`vendor_kernel`'s overlayfs support now comes from a single vendored
`vendor/fs/overlayfs/` tree (android16-6.12 baseline).
`glue/vendor_kernel_ovl_vfs_compat.{h,c}` adapts that
one source tree across the three supported VFS/API tiers: `OLD`
`[5.10, 5.12)`, `MID` `[5.12, 6.3)`, and `NEW` `[6.3, 6.19)`.

`vendor_kernel`'s procfs and mqueue glue uses `fd_file()`/`fd_empty()`, a
kernel API that only
exists from Linux v6.8 onward; `glue/lkm4ctr_compat.h` provides shims so
the same source builds unmodified against older GKI branches (e.g. 6.1).

See `vendor/README.md` for the honest scope/limitations. In
particular, `vendor_kernel` provides the real vendored UTS/PID/USER/IPC/mqueue
paths and documents the remaining NET/MNT/CGROUP caveats there; see that
README plus the "Real vs. bookkeeping vs. stub" table above for the full
picture across the current subsystem set.
