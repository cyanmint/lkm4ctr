# vendor_kernel

`vendor_kernel` 是 `lkm4ctr` 中 vendored 的 namespace、IPC/mqueue 和 overlayfs 实现。模块通过真实 syscall hook 将它们接入未修改的 `containerd`、`runc` 和 `dockerd`。

## 能力分类

- **真实实现**：UTS、PID、USER、IPC、POSIX mqueue，以及支持范围内的 overlayfs；
- **仅 bookkeeping**：NET namespace、CGROUP namespace 隔离；
- **依赖运行内核**：MNT namespace，以及 cgroup 文件系统和控制器。

项目不提供 cgroup 文件系统或控制器，因此 systemd 容器要求主机内核真正提供可用 cgroup v2。NET namespace 当前刻意不创建真实新 `struct net`，以避免已观察到的核心 `percpu_counters` 崩溃。

## 支持范围与构建

统一 overlayfs 源码通过兼容层覆盖 Linux 5.10 至 6.18 左右的 Android KMI；当前发布矩阵见用户手册。必须使用匹配 DDK 的真实 `vmlinux` 和 `Module.symvers`：

```sh
make -C /opt/ddk/kdir/<kmi> M="$PWD/vendor" modules
```

不要把普通 `modules_prepare` 树产生的模块加载到真实设备。

## 已知限制

casefold-capable 的 backing filesystem 可能拒绝 overlay 挂载；Android `/data` 上的 F2FS 可能触发此限制。可将 Docker 数据根目录放到非 casefold ext4 loopback 文件系统，或使用 `vfs`。`SHM_HUGETLB` 不受支持，普通 SysV shared memory 受支持。redroid 的 property area 错误通常属于 tmpfs/xattr/SELinux 用户空间问题。

完整实现细节、符号解析、缓存一致性和 vendoring 规则请参阅英文原文 [`../../en/developer/VENDOR_KERNEL.md`](../../en/developer/VENDOR_KERNEL.md)。
