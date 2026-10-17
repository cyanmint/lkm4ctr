# lkm4ctr user manual

`lkm4ctr` is an out-of-tree Linux kernel module for rooted, arm64 Android GKI devices. It supplies selected container prerequisites at module load time so an unmodified `containerd`/`runc`/`dockerd` can run without recompiling or flashing the Android kernel.

This is a user guide, not a promise of full Linux-container compatibility. Read the limitations before using it on a device that matters.

## 1. What the project provides

The project produces three artifacts:

- `lkm4ctr-<kmi>-arm64.ko`: the kernel module. It is one merged module containing the vendored namespace, IPC/mqueue, and overlayfs paths plus the hook/glue code.
- `lkm4ctr_checker-arm64`: an optional userspace diagnostic program. It is not a kernel module and does not need `insmod`; it performs real syscalls and reports `PASS`, `STUB`, or `FAIL`.
- `lkm4ctr-kernelsu-module.zip`: a KernelSU module containing every KMI build, `lkm4ctr_checker`, an action script, a boot service, and a multilingual WebUI with command output.

The module is intended to make stock container runtimes work. They do not need a project-specific runtime patch.

## 2. Requirements

### Device

- A rooted **arm64** Android device. `sudo` in the examples means a root-capable Termux setup; `su -c` can be used instead.
- A stock Android GKI kernel in a supported KMI family. The current build matrix covers:
  - `android12-5.10`
  - `android13-5.10`
  - `android13-5.15`
  - `android14-5.15`
  - `android14-6.1`
  - `android15-6.6`
  - `android16-6.12`
  - `android17-6.18`
- A module built for the **exact** KMI and kernel ABI. A module for `android14-5.15` is not interchangeable with one for `android14-6.1`.
- A device kernel with the remaining prerequisites required by the project, notably `CONFIG_NAMESPACES=y`, `CONFIG_NET_NS=y`, and `CONFIG_TIME_NS=y`. The module cannot make every kernel subsystem exist safely from outside the kernel.
- Sufficient free storage and memory for the container images and Docker data root.

You do not need to recompile or flash the Android kernel. You do need a compatible, signed/allowed-to-load kernel-module environment. Android security policy, KernelSU/Magisk policy, module signature enforcement, or SELinux can still reject `insmod`.

### Termux and userspace

Install Termux from a trusted source, update packages, and install a root helper plus the container tools available for your architecture:

```sh
pkg update
pkg install root-repo sudo
apt install dockerd docker-cli docker-compose
```

Package names and availability can vary between Termux repositories. Confirm the commands exist:

```sh
command -v sudo dockerd docker
```

The module includes a vendored overlayfs implementation, but `overlay2` can still fail when Docker's data root is on Android `/data` or another casefold-capable F2FS filesystem. Install and use `fuse-overlayfs` as Docker's default storage driver; this is the recommended storage path for affected devices.

## 3. Identify the KMI

Run this on the target device:

```sh
uname -a
getprop ro.build.version.release
```

For example, a kernel string containing `android14-6.1` belongs to the `android14-6.1` KMI. The Android release number alone is not enough. If `uname -a` is ambiguous, inspect the kernel release/vendor build information and choose the module whose filename exactly matches the KMI.

## 4. Obtain the artifacts

The easiest path is a GitHub release asset from the project's [releases page](https://github.com/cyanmint/lkm4ctr/releases):

```text
lkm4ctr-android14-6.1-arm64.ko
lkm4ctr_checker-arm64
lkm4ctr-kernelsu-module.zip
update.json
```

Download the `.ko` whose `<kmi>` matches the device. Do not rename a module to hide a mismatch, and do not load a module built for another KMI.

To use KernelSU, install `lkm4ctr-kernelsu-module.zip` from the KernelSU Manager. It includes all KMI builds and `lkm4ctr_checker`, detects the matching KMI from the kernel release, and enables boot auto-load by default. Its Action button waits up to 30 seconds for a volume key: Volume Up toggles immediate load/unload, Volume Down toggles boot auto-load, and Volume Up is the fallback if key input is unavailable or no key is detected. The WebUI provides four module controls and a checker button; a read-only output box displays load/unload and checker results. English, Simplified Chinese, and Traditional Chinese labels are auto-selected from the device language, with a language selector. Releases also include `update.json`; after a release with a higher `versionCode` is published, KernelSU Manager can offer an in-app update to the latest module ZIP.

Copy the files to a readable location, for example:

```sh
mkdir -p "$HOME/lkm4ctr"
cp /sdcard/Download/lkm4ctr-android14-6.1-arm64.ko "$HOME/lkm4ctr/lkm4ctr.ko"
cp /sdcard/Download/lkm4ctr_checker-arm64 "$HOME/lkm4ctr/"
chmod 755 "$HOME/lkm4ctr/lkm4ctr_checker"
```

## 5. Run the checker before loading

Run as root or with `CAP_SYS_ADMIN`:

```sh
sudo "$HOME/lkm4ctr/lkm4ctr_checker"
printf 'exit status: %s\n' "$?"
```

`PASS` means the checker observed a real effect, `STUB` means the syscall was accepted without real isolation, and `FAIL` means the syscall failed. Before loading, failures are useful as a baseline. After loading, run it again and compare the namespace, IPC, and overlay results.

A non-root run commonly reports `EPERM`; that is a privilege failure, not evidence that the module is broken.

## 6. Load the module

Stop any existing container workload before changing the loaded module. Then load the exact matching file:

```sh
sudo insmod "$HOME/lkm4ctr/lkm4ctr.ko"
```

Check the result immediately:

```sh
cat /proc/modules | grep '^lkm4ctr '
sudo "$HOME/lkm4ctr/lkm4ctr_checker"
```

If `insmod` fails, preserve the complete error and inspect the kernel log. Common causes are a KMI/ABI mismatch, an unknown symbol, module signature enforcement, missing root capability, or a kernel outside the supported range. Never work around an unknown-symbol or version error by forcing the load.

## 7. Configure `fuse-overlayfs` as Docker's default

Download the arm64 `fuse-overlayfs` binary from the [official releases page](https://github.com/containers/fuse-overlayfs/releases). Copy it into Termux's executable path and make it executable:

```sh
termux-setup-storage
mkdir -p "$PREFIX/bin"
cp /sdcard/Download/fuse-overlayfs-aarch64 "$PREFIX/bin/fuse-overlayfs"
chmod 755 "$PREFIX/bin/fuse-overlayfs"
command -v fuse-overlayfs
fuse-overlayfs --version
```

The downloaded filename may differ between releases; use the arm64/aarch64 asset and adjust the `cp` source path. Do not download an x86 or x86_64 binary.

Configure Docker to select it by default:

```sh
mkdir -p "$PREFIX/etc/docker"
printf '%s\n' '{"storage-driver":"fuse-overlayfs"}' > "$PREFIX/etc/docker/daemon.json"
cat "$PREFIX/etc/docker/daemon.json"
```

If the Termux Docker package does not automatically use `$PREFIX/etc/docker/daemon.json`, pass the file explicitly when starting the daemon with `--config-file` as shown below.

## 8. Start Docker and run a container

Start the daemon in Termux:

```sh
sudo dockerd --config-file="$PREFIX/etc/docker/daemon.json" --bridge=none --experimental
```

In another Termux session, wait for the socket and run a first container:

```sh
until [ -S /data/data/com.termux/files/usr/var/run/docker.sock ] || [ -S /var/run/docker.sock ]; do sleep 1; done
sudo docker info
# Confirm that Docker selected fuse-overlayfs before creating containers.
sudo docker info | grep -E 'Storage Driver|fuse-overlayfs'
sudo docker run --network=host --rm -it docker.io/library/alpine:latest /bin/sh
```

The documented working network mode is host networking. Without a real network-namespace implementation, bridge networking and other isolated network modes are not supported by this project; a container may start but have no usable network.

## 9. Optional diagnostics filesystem

The module exposes a diagnostic filesystem. It is useful when a container runtime fails after `insmod`:

```sh
sudo mkdir -p /mnt/lkm4ctr
sudo mount -t lkm4ctr diag /mnt/lkm4ctr
sudo cat /mnt/lkm4ctr/status
sudo cat /mnt/lkm4ctr/log
sudo cat /mnt/lkm4ctr/v/status
sudo cat /mnt/lkm4ctr/v/resources
sudo cat /mnt/lkm4ctr/v/namespaces
```

The mount is optional and is not required for Docker. `/log` is the combined log; `/v/` contains vendor-kernel state and live namespace/IPC information. Keep the output when reporting an issue, together with `uname -a`, the exact asset filename, and the `insmod`/Docker error.

Do not attempt to unload while containers or other processes are using the module. If `/dev/mqueue` is mounted, normally unmount it first; its active superblock pins the module, so `rmmod` safely refuses until the mount is gone. Processes holding a vendored namespace proxy also pin the module until they exit; check `nsproxy_live` in `/mnt/lkm4ctr/v/resources`. Vendored IPC and user namespaces are shown by `ipcns_live` and `userns_live`; their teardown queues are shown by `ipcns_free_work_pending` and `userns_pending_work`. On kernels without native USER_NS, credentials referencing vendored user namespaces are released after the RCU credential-free callback. Descriptors for vendored namespaces also pin the module while their NSFS inode remains cached; closing a descriptor may not release that pin immediately. Check `nsfs_inode_pins` if unload is refused, and do not force removal. Vendored SysV shared-memory segments keep the module busy; remove them with `ipcrm -m`. On kernels built without native `CONFIG_SYSVIPC`, vendored semaphore `SEM_UNDO` state and `sem_exit_work_pending` keep the module busy until cleanup completes. Controlled stop refuses while tracked IPC resources or task-exit work remain. `mqueue_public_fs_registered` in `/v/resources` indicates whether this module owns the public mqueue filesystem name or the running kernel already registered it. After stopping Docker and other namespace-using processes, removing SysV shared-memory segments, closing namespace descriptors, and unmounting both filesystems, a normal unload may be requested:

```sh
sudo umount /dev/mqueue  # if mounted
sudo umount /mnt/lkm4ctr
sudo rmmod lkm4ctr
```

The module tracks in-flight hooks, live vendored namespace proxies, namespace-inode pins, and active IPC objects/work, and can refuse an unsafe unload. The same `/dev/mqueue`, SysV IPC, namespace-process, and namespace-descriptor requirements apply before hot-reload. The diagfs also provides a controlled hot-reload path for advanced users:

```sh
sudo sh -c 'printf "%s\n" "/absolute/path/to/new/lkm4ctr.ko" > /mnt/lkm4ctr/v/hotreload/do-hot-reload'
sudo cat /mnt/lkm4ctr/v/hotreload/status
```

Use hot reload only after stopping container workloads. A reboot with no module loaded is the recovery path if the device is left in an uncertain state.

## 10. Known limitations

- This is not a general-purpose replacement for all Android kernel container support.
- `NET_NS` is bookkeeping-only in the current implementation. Use `--network=host`.
- `CGROUP_NS` is bookkeeping-only; the project does not provide a cgroup filesystem or controllers. `systemd` as container PID 1 therefore requires a working host cgroup v2 mount and may still fail.
- `MNT_NS` depends on the running kernel's mount-namespace support.
- `redroid` failures such as `init: Failed to initialize property area` are generally userspace tmpfs/xattr/SELinux issues, not fixed by this module.
- `overlay2` mounts can fail on casefold-capable backing filesystems. Install `fuse-overlayfs` and make it the Docker storage driver as described above; a suitable non-casefold ext4 filesystem remains an alternative.
- `SHM_HUGETLB` is not supported; ordinary SysV shared memory is the supported path.
- A release asset is tied to the KMI it was built for. A successful download does not make it safe to use on another kernel.

The authoritative implementation caveats are in [`docs/en/developer/VENDOR_KERNEL.md`](../../docs/en/developer/VENDOR_KERNEL.md).

## 11. Building instead of downloading

Building a device module is for developers who have the matching Android GKI DDK. A plain kernel source tree with `modules_prepare` is not sufficient: it lacks the real `vmlinux`/`Module.symvers` data needed for safe symbol resolution and can produce a module that fails or panics on `insmod`.

Inside a matching DDK image:

```sh
make -C /opt/ddk/kdir/<kmi> M="$PWD/vendor" modules
```

For example, the CI workflow uses `ghcr.io/ylarod/ddk-min:<kmi>-20260313` and copies `vendor/lkm4ctr.ko` as the KMI-specific release asset. The checker is independent:

```sh
make -C lkm4ctr_checker CC=aarch64-linux-gnu-gcc
```

Do not treat a host build against unrelated headers as a device-ready module.

## 12. Reporting a problem

Include:

1. `uname -a` and Android/device details;
2. the exact `.ko` filename and release/tag;
3. whether the checker was run before and after `insmod`;
4. the complete `insmod` error and relevant kernel log;
5. Docker daemon output and the diagnostic filesystem output, if available;
6. the smallest reproducible container command.

Avoid posting private device identifiers, serial numbers, or full unredacted kernel logs when they are not needed.
