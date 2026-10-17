# lkm4ctr 用户手册

`lkm4ctr` 是面向已 root、arm64 Android GKI 设备的树外 Linux 内核模块。它在模块加载后提供部分容器运行所需的内核能力，使未修改的 `containerd`/`runc`/`dockerd` 可以运行，而不必重新编译或刷写 Android 内核。

这是一份使用手册，不代表完整的 Linux 容器兼容性。请先阅读限制，再在重要设备上使用。

## 1. 项目提供什么

项目产生三个产物：

- `lkm4ctr-<version>-<kmi>-arm64.ko`：带版本号的内核模块。它是一个合并模块，包含 vendored namespace、IPC/mqueue、overlayfs 实现以及 hook/glue 代码。
- `lkm4ctr_checker-<version>-arm64`：带版本号的用户空间诊断程序。它不是内核模块，不需要 `insmod`，会执行真实系统调用并报告 `PASS`、`STUB` 或 `FAIL`。
- `lkm4ctr-kernelsu-module.zip`：KernelSU 模组，包含所有 KMI 版本、`lkm4ctr_checker`、action 脚本、开机服务和带命令输出框的多语言 WebUI。

模块的目标是让原版容器运行时直接工作，不需要项目专用的运行时补丁。

## 2. 要求

### 设备

- 已 root 的 **arm64** Android 设备。示例中的 `sudo` 表示 Termux 中可获得 root 的配置，也可以改用 `su -c`。
- 当前支持的 Android GKI KMI 系列：`android12-5.10`、`android13-5.10`、`android13-5.15`、`android14-5.15`、`android14-6.1`、`android15-6.6`、`android16-6.12`、`android17-6.18`。
- 与设备 **完全匹配** 的 KMI 和内核 ABI 模块。`android14-5.15` 不能替代 `android14-6.1`。
- 设备内核仍需具备项目要求的其他能力，尤其是 `CONFIG_NAMESPACES=y`、`CONFIG_NET_NS=y` 和 `CONFIG_TIME_NS=y`。树外模块不能安全地补出所有内核子系统。
- 足够的存储空间和内存。

不需要重新编译或刷写 Android 内核，但必须具备可加载内核模块的环境。Android 安全策略、KernelSU/Magisk 策略、模块签名强制或 SELinux 仍可能拒绝 `insmod`。

### Termux 和用户空间

从可信来源安装 Termux，更新软件包，并安装当前架构可用的 root 工具和容器工具：

```sh
pkg update
pkg install root-repo sudo
apt install dockerd docker-cli docker-compose
```

Termux 软件源中的包名和可用性可能变化，请确认命令存在：

```sh
command -v sudo dockerd docker
```

模块内置 vendored overlayfs，但当 Docker 数据根目录位于 Android `/data` 或其他支持 casefold 的 F2FS 文件系统时，`overlay2` 仍可能失败。请安装并将 `fuse-overlayfs` 设置为 Docker 的默认存储驱动；这是受影响设备的推荐方案。

## 3. 确认 KMI

在设备上运行：

```sh
uname -a
getprop ro.build.version.release
```

例如内核字符串包含 `android14-6.1`，对应 KMI 就是 `android14-6.1`。仅看 Android 版本号不够；如果 `uname -a` 不明确，请结合内核 release/vendor 构建信息选择文件名中 KMI 完全一致的模块。

## 4. 获取产物

最简单的方式是从项目的 [Releases 页面](https://github.com/cyanmint/lkm4ctr/releases) 下载：

```text
lkm4ctr-4.0.5-android14-6.1-arm64.ko
lkm4ctr_checker-4.0.5-arm64
lkm4ctr-kernelsu-module.zip
update.json
```

下载与设备 `<kmi>` 完全一致的 `.ko`。不要通过改名掩盖不匹配，也不要加载其他 KMI 的模块。

也可以在 KernelSU Manager 中安装 `lkm4ctr-kernelsu-module.zip`。ZIP 包含所有 KMI 版本和 `lkm4ctr_checker`，会根据内核 release 自动选择匹配版本；默认开启开机自动加载。点击模块的 Action 按钮后，等待音量键：音量上切换立即加载/卸载，音量下切换开机自动加载；如果无法读取音量键或等待期间没有按键，默认执行音量上操作。所选操作完成后，Action 输出会显示执行结果。WebUI 提供四个模组控制按钮和一个 checker 按钮，下方只读文本框显示 insmod/rmmod 与 checker 输出；界面会根据系统语言选择简体中文、繁体中文或英文，也可手动切换。Release 会同时发布 `update.json`；当后续版本提高 `versionCode` 后，KernelSU Manager 可显示更新按钮并下载最新模组 ZIP。

```sh
mkdir -p "$HOME/lkm4ctr"
cp /sdcard/Download/lkm4ctr-4.0.5-android14-6.1-arm64.ko "$HOME/lkm4ctr/lkm4ctr.ko"
cp /sdcard/Download/lkm4ctr_checker-4.0.5-arm64 "$HOME/lkm4ctr/lkm4ctr_checker"
chmod 755 "$HOME/lkm4ctr/lkm4ctr_checker"
```

## 5. 加载前运行诊断

以 root 或拥有 `CAP_SYS_ADMIN` 的身份运行：

```sh
sudo "$HOME/lkm4ctr/lkm4ctr_checker"
printf 'exit status: %s\n' "$?"
```

`PASS` 表示观察到了真实效果，`STUB` 表示系统调用被接受但没有真实隔离，`FAIL` 表示系统调用失败。加载前运行可作为基线；加载后再次运行并比较 namespace、IPC 和 overlay 结果。

非 root 运行通常会得到 `EPERM`，这表示权限不足，不代表模块损坏。

## 6. 加载模块

更换已加载模块前先停止容器工作负载，然后加载完全匹配的文件：

```sh
sudo insmod "$HOME/lkm4ctr/lkm4ctr.ko"
cat /proc/modules | grep '^lkm4ctr '
sudo "$HOME/lkm4ctr/lkm4ctr_checker"
```

如果 `insmod` 失败，请保留完整错误并检查内核日志。常见原因包括 KMI/ABI 不匹配、unknown symbol、模块签名限制、root 能力不足或内核版本不在支持范围。不要强行绕过 unknown-symbol 或版本错误。

## 7. 将 `fuse-overlayfs` 设置为 Docker 默认驱动

从 [`fuse-overlayfs` 官方 Releases 页面](https://github.com/containers/fuse-overlayfs/releases) 下载 arm64 的 `fuse-overlayfs` 二进制文件，复制到 Termux 的可执行路径并赋予执行权限：

```sh
termux-setup-storage
mkdir -p "$PREFIX/bin"
cp /sdcard/Download/fuse-overlayfs-aarch64 "$PREFIX/bin/fuse-overlayfs"
chmod 755 "$PREFIX/bin/fuse-overlayfs"
command -v fuse-overlayfs
fuse-overlayfs --version
```

不同版本的下载文件名可能不同；请使用 arm64/aarch64 资产并相应调整 `cp` 的源路径，不要下载 x86 或 x86_64 二进制。

配置 Docker 默认使用它：

```sh
mkdir -p "$PREFIX/etc/docker"
printf '%s\n' '{"storage-driver":"fuse-overlayfs"}' > "$PREFIX/etc/docker/daemon.json"
cat "$PREFIX/etc/docker/daemon.json"
```

如果 Termux Docker 软件包没有自动读取 `$PREFIX/etc/docker/daemon.json`，请按下方命令使用 `--config-file` 显式指定。

## 8. 启动 Docker 并运行容器

在 Termux 中启动 daemon：

```sh
sudo dockerd --config-file="$PREFIX/etc/docker/daemon.json" --bridge=none --experimental
```


在另一个 Termux 会话等待 socket，然后运行第一个容器：

```sh
until [ -S /data/data/com.termux/files/usr/var/run/docker.sock ] || [ -S /var/run/docker.sock ]; do sleep 1; done
sudo docker info
# 确认 Docker 已选择 fuse-overlayfs。
sudo docker info | grep -E 'Storage Driver|fuse-overlayfs'
sudo docker run --network=host --rm -it docker.io/library/alpine:latest /bin/sh
```

当前文档化可用的网络模式是 host networking。项目没有提供真实的 network namespace 实现，因此 bridge 等隔离网络模式不受支持，容器可能启动但无法联网。

## 9. 可选的诊断文件系统

模块提供诊断文件系统，可用于定位 `insmod` 后的容器运行时错误：

```sh
sudo mkdir -p /mnt/lkm4ctr
sudo mount -t lkm4ctr diag /mnt/lkm4ctr
sudo cat /mnt/lkm4ctr/status
sudo cat /mnt/lkm4ctr/log
sudo cat /mnt/lkm4ctr/v/status
sudo cat /mnt/lkm4ctr/v/resources
sudo cat /mnt/lkm4ctr/v/namespaces
```

这不是 Docker 的必需步骤。停止 Docker 并卸载诊断文件系统后，如果 `/dev/mqueue` 已挂载，也要先正常卸载；活动 superblock 会固定模块，`rmmod` 会安全地拒绝，直到挂载消失。仍持有 vendored namespace proxy 的进程会固定模块直到退出；请查看 `/v/resources` 的 `nsproxy_live`。vendored IPC 与 user namespace 数量分别见 `ipcns_live`、`userns_live`，对应释放 work 见 `ipcns_free_work_pending`、`userns_pending_work`。指向 vendored namespace 的 NSFS descriptor 也可能在 inode 缓存期间固定模块。vendored SysV 共享内存 segment 会令模块保持忙碌，请用 `ipcrm -m` 移除；无原生 `CONFIG_SYSVIPC` 时，SEM_UNDO 状态及 `sem_exit_work_pending` 会固定模块直到清理完成。受控停止在受跟踪的 IPC 资源或 task-exit work 尚存时会拒绝；不要强制移除：

```sh
sudo umount /mnt/lkm4ctr
sudo rmmod lkm4ctr
```

模块会追踪正在执行的 hook、vendored namespace/NSFS 引用与 IPC 对象/work，并可能拒绝不安全卸载。高级用户可使用 diagfs 的 hot-reload：

```sh
sudo sh -c 'printf "%s\n" "/absolute/path/to/new/lkm4ctr.ko" > /mnt/lkm4ctr/v/hotreload/do-hot-reload'
sudo cat /mnt/lkm4ctr/v/hotreload/status
```

只有在停止容器工作负载后才使用。设备状态不确定时，恢复方式是重启，并确保不自动加载旧模块。

## 10. 已知限制

- 这不是所有 Android 内核容器能力的通用替代品。
- `NET_NS` 当前只有 bookkeeping，使用 `--network=host`。
- `CGROUP_NS` 只有 bookkeeping；项目不提供 cgroup 文件系统或控制器。因此 systemd 作为容器 PID 1 需要主机提供可用的 cgroup v2 挂载，仍可能失败。
- `MNT_NS` 依赖运行内核自己的 mount namespace 支持。
- `redroid` 的 `init: Failed to initialize property area` 通常是用户空间 tmpfs/xattr/SELinux 问题，不是本模块解决的问题。
- casefold 文件系统上的 `overlay2` 挂载可能失败。请按上文安装 `fuse-overlayfs` 并将其设为 Docker 存储驱动；合适的非 casefold ext4 文件系统仍是替代方案。
- 不支持 `SHM_HUGETLB`；普通 SysV 共享内存受支持。
- Release 产物绑定构建时的 KMI，下载成功不代表可以用于其他内核。

权威实现限制见 [`docs/en/developer/VENDOR_KERNEL.md`](../../docs/en/developer/VENDOR_KERNEL.md)。

## 11. 自行构建

自行构建设备模块需要匹配的 Android GKI DDK。只有 `modules_prepare` 的普通内核源码树不够用：它没有安全符号解析所需的真实 `vmlinux`/`Module.symvers` 数据，生成的模块可能在 `insmod` 时失败甚至使内核崩溃。

在匹配的 DDK 容器中：

```sh
make -C /opt/ddk/kdir/<kmi> M="$PWD/vendor" modules
```

CI 使用 `ghcr.io/ylarod/ddk-min:<kmi>-20260313`。诊断程序独立构建：

```sh
make -C lkm4ctr_checker CC=aarch64-linux-gnu-gcc
```

不要把使用无关主机头文件的构建结果当成设备可用模块。

## 12. 提交问题

请提供：`uname -a` 和设备信息、完整 `.ko` 文件名及 release/tag、加载前后 checker 结果、完整 `insmod` 错误和相关内核日志、Docker 输出及 diagfs 输出（如有），以及最小复现命令。发布日志前请移除不必要的设备标识符、序列号和隐私信息。
