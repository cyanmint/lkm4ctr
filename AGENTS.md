# AGENTS.md

Guidance for AI coding agents working in this repository.

## What this repository is

`lkm4ctr` builds a single out-of-tree Linux kernel module, **`lkm4ctr.ko`**,
that lets a stock, unpatched `containerd`/`runc`/`dockerd` run on Android
GKI kernels built without the usual container prerequisites (namespace
support, `CONFIG_SYSVIPC`, `CONFIG_POSIX_MQUEUE`, cgroup-v1 device control,
…). It targets **arm64 Android GKI** kernels across multiple KMIs (Kernel
Module Interface versions, e.g. `android14-6.1`), not a generic desktop
Linux kernel.

Start with `README.md` for the full module map and subsystem
classifications ("real" vs. "bookkeeping-only" vs. "stub"); each subsystem
directory also has its own `README.md` with implementation-specific detail
— read the relevant one before changing that subsystem.

## Repository layout

- `lkm4ctr/` — the unified kernel module source and kbuild `Makefile`.
  - `lkm4ctr_main.c`, `lkm4ctr_log.c`, `lkm4ctr_diagfs.c`,
    `lkm4ctr_hotreload.c` — module entry/exit, logging, and the `lkm4ctr`
    diagfs (`mount -t lkm4ctr diag <mnt>`) used for runtime
    diagnostics/log inspection.
  - `shadow_hijack/` — shared ftrace/kprobe hook engine used by the other
    subsystems.
  - `vendor_kernel/` — vendored namespace/IPC/mqueue/overlayfs
    implementation, duplicated per KMI/VFS-API era under
    `fs/overlayfs`, `fs/overlayfs_5_10`, `fs/overlayfs_5_15`,
    `fs/overlayfs_6_6`, `fs/overlayfs_6_12` (see "Vendored overlayfs
    variants" below).
  - `shadow_cgdevices/` — cgroup-device compatibility hook shim.
- `lkm4ctr_checker/` — a plain **userspace** diagnostic binary (not a
  kernel module). No build-time or load-time dependency on `lkm4ctr.ko`.
- `common/` — shared, header-only helpers (`shadow_hook.h`,
  `lkm4ctr_compat.h`, `lkm4ctr_log.h`) included by multiple subsystems.
- `qemu_test.sh` — a single merged script with three roles, dispatched on
  PID/argv[0]/flags (see its own header comment): host-side QEMU launcher,
  ramdisk stage-1/stage-2 init, and a `-t` "checker mode" that runs
  `lkm4ctr_checker` before/after `insmod`.
- `remove_syscalls.py` — maintenance helper (not run in CI).
- `.github/workflows/build-lkm4ctr.yml` — CI: builds `lkm4ctr.ko` across
  a KMI matrix inside SukiSU-Ultra's DDK container images and boot-tests
  every KMI under QEMU.

## Vendored overlayfs variants — a recurring pitfall

`vendor_kernel/fs/overlayfs*` contains **five near-duplicate copies** of
overlayfs, one per VFS-API era:

| Directory                          | Kernel version range | KMIs |
|-------------------------------------|-----------------------|------|
| `fs/overlayfs_5_10/`                | [5.10, 5.15)          | android12-5.10, android13-5.10 |
| `fs/overlayfs_5_15/`                | [5.15, 6.1)           | android13-5.15, android14-5.15 |
| `fs/overlayfs/`                     | [6.1, 6.3)            | android14-6.1 |
| `fs/overlayfs_6_6/`                 | [6.6, 6.7)            | android15-6.6 |
| `fs/overlayfs_6_12/`                | [6.12, 6.13)          | android16-6.12 |

A fix or feature change to overlayfs logic almost always needs to be
applied to **all five** copies (they intentionally diverge only where the
underlying kernel VFS API itself diverges across these ranges) — check
`vendor_kernel/glue/vendor_kernel_overlay.c`'s header comment for the
authoritative range table before editing just one. A `__init`/`__exit`
annotation must match across all variants for functions with the same
name/role (e.g. `ovl_aio_request_cache_init`); a mismatch (marking it
`__init` in one copy but calling it from non-`__init` code) causes a
modpost "section mismatch" build failure, not a runtime bug — reconcile by
following whichever variants got it right, don't just silence the warning.

## Building

- **Kernel module** (`lkm4ctr.ko`): must be built against a real GKI
  `vmlinux`/`Module.symvers`/matching clang toolchain for the target KMI —
  a bare `gki_defconfig` + `modules_prepare` tree only has stub/empty
  symbol CRC data and produces modules that panic on `insmod`. CI builds
  inside `ghcr.io/ylarod/ddk-min:<kmi>-<release>` DDK container images
  (see SukiSU-Ultra's `build-lkm.yml`) for exactly this reason:
  ```sh
  make -C /opt/ddk/kdir/<kmi> M="$PWD/lkm4ctr" modules
  ```
  To compile-test locally against a plain kernel source tree instead
  (only useful as a rough syntax/API check, **not** equivalent to a real
  GKI DDK build — a sandbox host's own kernel headers are usually a
  different major version and will produce spurious VFS API-mismatch
  errors unrelated to real bugs):
  ```sh
  make ARCH=x86_64 x86_64_defconfig && ./scripts/config --enable MODULES ... \
    && make ARCH=x86_64 modules_prepare   # inside a checked-out kernel-common tree
  make -C <kernel-common> M=<module-dir> modules
  ```
- **`lkm4ctr_checker`** (userspace, cross-compiled statically for arm64,
  no kernel headers/KDIR involved):
  ```sh
  make -C lkm4ctr_checker CC=aarch64-linux-gnu-gcc
  ```

## CI / testing

`.github/workflows/build-lkm4ctr.yml`:
- `build` job: matrix-builds `lkm4ctr.ko` for every KMI in `DEFAULT_KMIS`
  (or a custom `kmis` `workflow_dispatch` input) inside the matching DDK
  container image.
- `build-checker` job: builds `lkm4ctr_checker` once (arm64, static),
  independent of the KMI matrix.
- `test` job: a matrix over every KMI (the build matrix plus
  `TEST_ONLY_KMIS`, i.e. KMIs with no DDK image/build yet, tested boot-only
  without a module). Boots every kernel Image bundled for that KMI in
  `testsuitekernels.zip` (`Image.<short-kmi>.stock` /
  `Image.<short-kmi>.patched`) under QEMU via `qemu_test.sh`, against the
  shared `image1.ext4`/busybox from `testsuite.zip`, with the KMI's
  `lkm4ctr.ko` (if built) injected and `insmod`'d.
- `vendor-ns-diff` job: diffs `vendor_kernel` against a shallow clone of
  upstream `kernel/common` (android14-6.1) and posts the diff to the job
  summary — informational, not a pass/fail gate.
- `release` job: only on a manual `workflow_dispatch` with
  `create_release` checked; publishes every built `lkm4ctr-<kmi>-arm64.ko`
  plus the checker binary as GitHub release assets.

There is no separate lint step; the closest to "linting" this module gets
is a clean `modpost` (no section-mismatch/unresolved-symbol warnings) in
the `build` job.

## Conventions

- Header-only shared helpers go in `common/`, not duplicated per subsystem.
- Vendored subsystem code is intentionally kept close to its upstream
  kernel source layout/paths under `vendor_kernel/` for diffability against
  `kernel/common` (see `vendor_kernel/vendor_kernel_diff.sh` and the
  `vendor-ns-diff` CI job) — avoid gratuitous reformatting of vendored
  files.
- `lkm4ctr_checker` intentionally has zero build/runtime dependency on
  `lkm4ctr.ko` or any kernel headers; keep it that way when adding checks.
- Every subsystem README documents its own "real vs. bookkeeping vs. stub"
  classification — update the relevant README (and the summary table in
  the top-level `README.md`) whenever a change moves a feature between
  these categories.
