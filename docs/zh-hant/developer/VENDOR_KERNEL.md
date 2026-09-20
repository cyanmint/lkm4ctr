# vendor_kernel

`vendor_kernel` 是 `lkm4ctr` 中 vendored 的 namespace、IPC/mqueue 和 overlayfs 實作，透過 syscall hook 接入未修改的 `containerd`、`runc` 和 `dockerd`。

- **真實實作**：UTS、PID、USER、IPC、POSIX mqueue，以及支援範圍內的 overlayfs；
- **僅 bookkeeping**：NET namespace、CGROUP namespace 隔離；
- **依賴執行核心**：MNT namespace，以及 cgroup 檔案系統與控制器。

專案不提供 cgroup 檔案系統或控制器，所以 systemd 容器需要主機核心真正提供可用 cgroup v2。NET namespace 目前刻意不建立真實新 `struct net`。

必須使用匹配 DDK 的真實 `vmlinux` 和 `Module.symvers`：

```sh
make -C /opt/ddk/kdir/<kmi> M="$PWD/vendor" modules
```

casefold backing filesystem 可能拒絕 overlay 掛載；Android `/data` 上的 F2FS 可能觸發此限制。可改用非 casefold ext4 loopback，或使用 `vfs`。`SHM_HUGETLB` 不支援，普通 SysV shared memory 支援。完整細節請參閱英文原文 [`../../en/developer/VENDOR_KERNEL.md`](../../en/developer/VENDOR_KERNEL.md)。
