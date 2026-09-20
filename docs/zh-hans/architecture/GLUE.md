# lkm4ctr.ko：glue 与模块布局

`glue/` 与 `vendor/` 共同构建唯一的 `lkm4ctr.ko`。`glue/` 是项目实际维护的代码，负责模块入口、日志、诊断伪文件系统、hot reload、hook 引擎以及 vendor/VFS overlayfs 兼容层；`vendor/` 保存 namespace、SysV IPC、POSIX mqueue 和 overlayfs 的 vendored 内核代码。

加载模块后，vendor_kernel 会自动启动：

```sh
mount -t lkm4ctr diag /mnt
cat /mnt/status
cat /mnt/log
cat /mnt/v/status
cat /mnt/v/namespaces
cat /mnt/v/resources
```

诊断文件系统根目录包含 `/status`、`/log`、`/v/`、`/helper.sh`、`/readme.txt` 和 `/resources`。`/v/` 提供状态、hook、namespace、IPC、引用计数和 hot-reload 信息。

不要在容器或其他进程仍使用模块时卸载。普通 `rmmod lkm4ctr` 会等待或拒绝正在使用的模块；更换版本前应停止 Docker，并优先使用受控 hot reload 或重启设备。

构建必须针对匹配的 Android GKI DDK：

```sh
make -C /opt/ddk/kdir/<kmi> M="$PWD/vendor" modules
```

更多用户要求见 [`../USER_GUIDE.md`](../USER_GUIDE.md)。
