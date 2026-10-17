# lkm4ctr Release Notes

Release sections are ordered newest first. The 4.0.5 section is prepared from repository history through 2026-10-17; release notes for 4.0.1–4.0.4 are reused from the project's published GitHub Releases.

## 4.0.5 (4.0.4 → 4.0.5)

### Highlights

- Added a KernelSU module package containing the supported KMI builds and userspace checker. It detects the device KMI, enables boot auto-load by default, and provides an Action button and multilingual WebUI for module controls and checker output.
- Added KernelSU Manager update metadata (`update.json`) and automated publication of versioned module assets, the checker, and the KernelSU ZIP. Release tags are checked against the module version; release creation accepts an HTTPS URL for release notes.
- Hardened controlled unload and hot-reload around live namespace and IPC state. The module tracks vendored namespace proxies, namespace-filesystem inode pins, IPC objects, and deferred cleanup; unsafe teardown is refused until references and work drain. Diagnostic resource counters help identify remaining blockers.
- Added procfs lookup fallbacks for `/proc/<pid>/ns/ipc` and `/proc/<pid>/ns/pid` when native namespace entries are unavailable, including an Android 17 / 6.18 path using that KMI's procfs PID accessor.

### Compatibility and testing

- The QEMU workflow exercises the checker before and after module load, namespace/procfs behavior, container workloads, and post-use module unload using pinned official Google GKI images across eight KMIs: `android12-5.10`, `android13-5.10`, `android13-5.15`, `android14-5.15`, `android14-6.1`, `android15-6.6`, `android16-6.12`, and `android17-6.18`.
- KernelSU ZIP and release packaging require a complete supported-KMI build matrix.
- `NET_NS` remains bookkeeping-only; use host networking (`--network=host`). The module must match the device's exact KMI. On Android data filesystems with casefold support, configure Docker to use `fuse-overlayfs` as described in the user guide.

### Changes

**fix:**
- Improved namespace/IPC lifetime handling, task-exit cleanup, and Android 5.10 compatibility; fixed KernelSU install permissions and shell invocation of control scripts.

**docs and licensing:**
- Added consolidated `copying.txt` terms to the project and KernelSU package, with clarified attribution and provenance metadata.

## 4.0.4 (4.0.3 → 4.0.4)

### Highlights

- Updated overlayfs, IPC, and diagnostic-filesystem compatibility for Linux 6.18 API changes.
- Improved namespace teardown and PID-namespace reaper handling, including a Linux 5.15-specific exit-path fallback.
- Expanded QEMU boot testing to include Android 17 / Linux 6.18.

### Compatibility and testing

Release module builds remain available for the seven arm64 Android GKI KMIs listed in 4.0.3. Android 17 / Linux 6.18 is included in boot-only CI coverage; this release does not provide an Android 17 module asset.

The vendor-kernel documentation at this tag still marks remote QEMU verification of the new Linux 5.15 exit-path fallback as pending.

### Changes

**feature:**
- Add compatibility paths for Linux 6.18 namespace, IPC, overlayfs, and diagnostic-filesystem APIs.

**fix:**
- Improve PID-namespace reaper handoff and vendored namespace teardown; preserve overlay credential references on Linux 6.18.

**docs:**
- Update the vendor-kernel compatibility notes for the Linux 5.15 exit path and newer-kernel API changes.

**chore:**
- Test against official Google GKI images across the KMI matrix and stream QEMU kernel logs to CI output. Android 17 / Linux 6.18 remains boot-only, without a module build.

## 4.0.3 (4.0.2 → 4.0.3)

### Highlights

- Reworked the diagnostic filesystem into a flatter layout with a combined root log, resource listings, and a `/v/` tree for vendor-kernel state.
- Fixed mount-namespace `fs_struct` handling and an error-pointer regression in the clone path.
- Reworked the user manuals in English, Simplified Chinese, and Traditional Chinese, with clearer Docker setup guidance.

### Compatibility

Release assets remain available for the same seven arm64 Android GKI KMIs as 4.0.2. Use `fuse-overlayfs` as Docker’s storage driver on affected casefold-capable backing filesystems. Network namespace isolation is not provided; use host networking.

### Changes

**feature:**
- Expose vendor-kernel state and resource diagnostics through the revised diagfs layout.

**fix:**
- Correct mount-namespace filesystem-state handling and clone-path error handling.

**docs:**
- Add localized user manuals and explain the `fuse-overlayfs` recommendation, cgroup limitations, and redroid property-area failures.

**chore:**
- Update checker, diagnostic helpers, and QEMU test integration to match the revised interfaces.

## 4.0.2 (4.0.1 → 4.0.2)

### Highlights

- Added real user-namespace ID mapping paths, including procfs map files and UID/GID translation needed by container runtimes.
- Improved PID and IPC namespace visibility through `/proc`, including namespace-file access required by `docker exec`.
- Added a vendored overlayfs implementation adapted across Android GKI kernels from 5.10 through 6.12.
- Improved namespace cleanup and container reboot routing, and fixed multiple module-load, CFI, IPC-lifecycle, and cross-KMI compatibility issues.

### Compatibility

Release assets are available for arm64 Android GKI KMIs: `android12-5.10`, `android13-5.10`, `android13-5.15`, `android14-5.15`, `android14-6.1`, `android15-6.6`, and `android16-6.12`. Load only the module built for the device’s exact KMI. Network namespace isolation is not provided; use host networking. Overlay mounts may fail on casefold-capable backing filesystems.

### Changes

**feature:**
- Add user-namespace ID mapping and improve PID/IPC namespace access for container runtimes.
- Vendor overlayfs with compatibility support across the seven release-build KMIs.

**fix:**
- Fix namespace and IPC teardown issues, container reboot routing, and several KMI-specific load/runtime failures.

**docs:**
- Document overlayfs backing-filesystem limitations and the supported behavior of namespace/procfs paths.

**chore:**
- Expand per-KMI build and QEMU testing; improve the QEMU harness’s USB-device readiness and container tests.

## 4.0.1 (initial release in available tag history)

### Highlights

- Namespace operations now install vendored namespace state on tasks during `unshare()` and `clone()`, rather than merely tracking namespace membership.
- Integrated namespace, SysV IPC, and POSIX message-queue support into the unified `lkm4ctr.ko` module. The `vendor_kernel` subsystem starts automatically when the module loads.
- Expanded `lkm4ctr_checker` to exercise runtime behavior and report observed results.

### Compatibility

Release assets are available for arm64 Android GKI KMIs: `android12-5.10`, `android13-5.10`, `android13-5.15`, `android14-5.15`, `android14-6.1`, `android15-6.6`, and `android16-6.12`. Load only the module built for the device’s exact KMI. Network namespace isolation is not provided; use host networking.

### Changes

**feature:**
- Install real vendored namespace state for supported namespace operations.
- Add vendored SysV IPC and POSIX message-queue paths to the module.

**fix:**
- Improve namespace object lifetime, refcounting, and task-exit cleanup.
- Expand the checker’s runtime syscall coverage.

**docs:**
- Update module and subsystem documentation to reflect the unified runtime architecture and implementation limits.

**chore:**
- Consolidate runtime-loadable components into one `lkm4ctr.ko` build and automate KMI release builds.

> The newly generated 4.0.5 section is Public Domain. Release-note text for 4.0.1–4.0.4 is reused from the project's published GitHub Releases.
