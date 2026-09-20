# lkm4ctr 用户手册

`lkm4ctr` 是面向已 root、arm64 Android GKI 设备的树外 Linux 内核模块。它在模块加载后提供部分容器运行所需的内核能力，使未修改的 `containerd`/`runc`/`dockerd` 可以运行，而不必重新编译或刷写 Android 内核。

这是一份使用手册，不代表完整的 Linux 容器兼容性。请先阅读限制，再在重要设备上使用。

## 1. 项目提供什么

项目产生两个相互独立的产物：

- `lkm4ctr-<kmi>-arm64.ko`：内核模块。它是一个合并模块，包含 vendored namespace、IPC/mqueue、overlayfs 实现以及 hook/glue 代码。
- `lkm4ctr_checker-arm64`：可选的用户空间诊断程序。它不是内核模块，不需要 `insmod`，会执行真实系统调用并报告 `PASS`、`STUB` 或 `FAIL`。

模块的目标是让原版容器运行时直接工作，不需要项目专用的运行时补丁。

## 2. 要求

### 设备

- 已 root 的 **arm64** Android 设备。示例中的 `sudo` 表示 Termux 中可获得 root 的配置，也可以改用 `su -c`。
- 当前支持的 Android GKI KMI 系列：`android12-5.10`、`android13-5.10`、`android13-5.15`、`android14-5.15`、`android14-6.1`、`android15-6.6`、`android16-6.12`。
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

模块内置 vendored overlayfs，单纯加载模块不要求 `fuse-overlayfs`。但 Docker 数据根目录必须位于 overlay 实现接受的文件系统上。Android `/data` 常见的支持 casefold 的 F2FS 可能导致 `overlay2` 失败。优先使用不启用 casefold 的 ext4 loopback 镜像；或者在了解性能和存储代价后使用 Docker `vfs` 驱动。

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
lkm4ctr-android14-6.1-arm64.ko
lkm4ctr_checker-arm64
```

下载与设备 `<kmi>` 完全一致的 `.ko`。不要通过改名掩盖不匹配，也不要加载其他 KMI 的模块。

```sh
mkdir -p "$HOME/lkm4ctr"
cp /sdcard/Download/lkm4ctr-android14-6.1-arm64.ko "$HOME/lkm4ctr/lkm4ctr.ko"
cp /sdcard/Download/lkm4ctr_checker-arm64 "$HOME/lkm4ctr/"
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

## 7. 启动 Docker 并运行容器

在 Termux 中启动 daemon：

```sh
sudo dockerd --bridge=none --experimental
```

在另一个 Termux 会话等待 socket，然后运行第一个容器：

```sh
until [ -S /data/data/com.termux/files/usr/var/run/docker.sock ] || [ -S /var/run/docker.sock ]; do sleep 1; done
sudo docker info
sudo docker run --network=host --rm -it docker.io/library/alpine:latest /bin/sh
```

当前文档化可用的网络模式是 host networking。项目没有提供真实的 network namespace 实现，因此 bridge 等隔离网络模式不受支持，容器可能启动但无法联网。

## 8. 可选的诊断文件系统

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

这不是 Docker 的必需步骤。停止 Docker 并卸载诊断文件系统后，才请求普通卸载：

```sh
sudo umount /mnt/lkm4ctr
sudo rmmod lkm4ctr
```

模块会追踪正在执行的 hook，并可能拒绝不安全卸载。高级用户可使用 diagfs 的 hot-reload：

```sh
sudo sh -c 'printf "%s\n" "/absolute/path/to/new/lkm4ctr.ko" > /mnt/lkm4ctr/v/hotreload/do-hot-reload'
sudo cat /mnt/lkm4ctr/v/hotreload/status
```

只有在停止容器工作负载后才使用。设备状态不确定时，恢复方式是重启，并确保不自动加载旧模块。

## 9. 已知限制

- 这不是所有 Android 内核容器能力的通用替代品。
- `NET_NS` 当前只有 bookkeeping，使用 `--network=host`。
- `CGROUP_NS` 只有 bookkeeping；项目不提供 cgroup 文件系统或控制器。因此 systemd 作为容器 PID 1 需要主机提供可用的 cgroup v2 挂载，仍可能失败。
- `MNT_NS` 依赖运行内核自己的 mount namespace 支持。
- `redroid` 的 `init: Failed to initialize property area` 通常是用户空间 tmpfs/xattr/SELinux 问题，不是本模块解决的问题。
- casefold 文件系统上的 overlay 挂载可能失败。将 Docker 数据根目录移到合适的非 casefold ext4 文件系统，或使用 `vfs`。
- 不支持 `SHM_HUGETLB`；普通 SysV 共享内存受支持。
- Release 产物绑定构建时的 KMI，下载成功不代表可以用于其他内核。

权威实现限制见 [`vendor/README.md`](../../vendor/README.md)。

## 10. 自行构建

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

## 11. 提交问题

请提供：`uname -a` 和设备信息、完整 `.ko` 文件名及 release/tag、加载前后 checker 结果、完整 `insmod` 错误和相关内核日志、Docker 输出及 diagfs 输出（如有），以及最小复现命令。发布日志前请移除不必要的设备标识符、序列号和隐私信息。
