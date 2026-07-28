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
classifications ("real" vs. "bookkeeping-only" vs. "stub"); `vendor/
README.md` and `glue/README.md` have implementation-specific
detail — read the relevant one before changing that area.

## Repository layout

The project is a flat, unified tree: only `glue/` and
`lkm4ctr_checker/` are code this project writes; everything under
`vendor/` is vendored code.

- `vendor/` — the unified kernel module build directory and kbuild
  `Makefile`/`Kconfig`, at the project root.
  - `kernel/`, `ipc/`, `fs/` — vendored namespace/IPC/mqueue/overlayfs
    implementation. Overlayfs lives in a single `fs/overlayfs/` tree
    (android16-6.12 baseline) with `../glue/vendor_kernel_ovl_vfs_compat.{h,c}`
    providing the cross-KMI VFS/fs_context compatibility tiers described
    below.
- `glue/` — the project root sibling of `vendor/`; all hand-written code:
    - `lkm4ctr_main.c`, `lkm4ctr_log.c`, `lkm4ctr_diagfs.c`,
      `lkm4ctr_hotreload.c` — module entry/exit, logging, and the `lkm4ctr`
      diagfs (`mount -t lkm4ctr diag <mnt>`) used for runtime
      diagnostics/log inspection. The diagfs is now a single, flat tree (no
      more per-submodule `hijack`/`cgroupdevices` subdirectories) since
      `vendor_kernel` is the only runtime-loadable subsystem.
    - `shadow_hijack.c`, `shadow_hook.h` — the shared ftrace/kprobe hook
      engine used by `vendor_kernel`. No longer a separately loadable
      subsystem: it is started/stopped directly by `lkm4ctr_main.c` and has
      no diagfs control surface of its own.
    - `lkm4ctr_compat.h`, `lkm4ctr_log.h` — shared, header-only helpers
      formerly under a top-level `common/` directory.
    - `vendor_kernel_*.c`/`.h` — the vendor_kernel/VFS/overlayfs glue and
      compat layers.
- `lkm4ctr_checker/` — a plain **userspace** diagnostic binary (not a
  kernel module). No build-time or load-time dependency on `lkm4ctr.ko`.
- `qemu_test.sh` — a single merged script with three roles, dispatched on
  PID/argv[0]/flags (see its own header comment): host-side QEMU launcher,
  ramdisk stage-1/stage-2 init, and a `-t` "checker mode" that runs
  `lkm4ctr_checker` before/after `insmod`.
- `remove_syscalls.py` — maintenance helper (not run in CI).
- `.github/workflows/build-lkm4ctr.yml` — CI: builds `lkm4ctr.ko` across
  a KMI matrix inside SukiSU-Ultra's DDK container images and boot-tests
  every KMI under QEMU.

The `shadow_cgdevices` submodule (cgroup-device compatibility hook shim)
has been removed entirely; it is not vendored or replaced elsewhere in this
tree.

## Vendored overlayfs compatibility tiers

`vendor/fs/overlayfs/` is now a **single** vendored overlayfs tree
taken from the android16-6.12 upstream snapshot. Cross-KMI support comes
from `glue/vendor_kernel_ovl_vfs_compat.{h,c}`, which adapts
that 6.12-shaped source across three `LINUX_VERSION_CODE` tiers:

| Tier | Kernel version range | KMIs |
|------|-----------------------|------|
| `OLD` | `[5.10, 5.12)` | android12-5.10, android13-5.10 |
| `MID` | `[5.12, 6.3)` | android13-5.15, android14-5.15, android14-6.1 |
| `NEW` | `[6.3, 6.19)` | android15-6.6, android16-6.12, android17-6.18 |

When changing overlayfs logic, edit `fs/overlayfs/` once, then verify
whether the change also needs tier-specific bridging in
`vendor_kernel_ovl_vfs_compat.{h,c}` or the existing version-gated
compat blocks inside the vendored sources. The compat header's top comment
is the authoritative description of what each tier translates (OLD drops
idmap arguments, MID maps `mnt_idmap` onto `user_namespace`-style helpers,
NEW is mostly native with backing-file fallbacks on older 6.x kernels).

If you add or remove a resolved VFS helper, keep the compat header's
per-tier declare/redirect lists and `vendor_kernel_ovl_vfs_compat.c`'s
resolver storage in sync. A bad `__init`/`__exit` annotation can still
trigger a modpost section-mismatch build failure; reconcile it with the
surrounding unified source and active tier paths rather than papering it
over.

## Building

- **Kernel module** (`lkm4ctr.ko`): must be built against a real GKI
  `vmlinux`/`Module.symvers`/matching clang toolchain for the target KMI —
  a bare `gki_defconfig` + `modules_prepare` tree only has stub/empty
  symbol CRC data and produces modules that panic on `insmod`. CI builds
  inside `ghcr.io/ylarod/ddk-min:<kmi>-<release>` DDK container images
  (see SukiSU-Ultra's `build-lkm.yml`) for exactly this reason:
  ```sh
  make -C /opt/ddk/kdir/<kmi> M="$PWD/vendor" modules
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

- Header-only shared helpers go in `glue/`, not duplicated
  per subsystem.
- Vendored subsystem code is intentionally kept close to its upstream
  kernel source layout/paths under `vendor/` for diffability against
  `kernel/common` (see `vendor/vendor_kernel_diff.sh` and the
  `vendor-ns-diff` CI job) — avoid gratuitous reformatting of vendored
  files.
- `lkm4ctr_checker` intentionally has zero build/runtime dependency on
  `lkm4ctr.ko` or any kernel headers; keep it that way when adding checks.
- Every README documents its own "real vs. bookkeeping vs. stub"
  classification — update the relevant README (and the summary table in
  the top-level `README.md`) whenever a change moves a feature between
  these categories.
