# lkm4ctr.ko：glue 與模組配置

`glue/` 與 `vendor/` 共同建置唯一的 `lkm4ctr.ko`。`glue/` 是專案實際維護的程式碼，負責模組入口、日誌、診斷虛擬檔案系統、hot reload、hook 引擎以及 vendor/VFS overlayfs 相容層；`vendor/` 保存 namespace、SysV IPC、POSIX mqueue 和 overlayfs 的 vendored 核心程式碼。

載入模組後，vendor_kernel 會自動啟動：

```sh
mount -t lkm4ctr diag /mnt
cat /mnt/status
cat /mnt/log
cat /mnt/v/status
cat /mnt/v/namespaces
cat /mnt/v/resources
```

診斷檔案系統包含 `/status`、`/log`、`/v/`、`/helper.sh`、`/readme.txt` 和 `/resources`。不要在容器或其他程序仍使用模組時卸載。建置必須針對匹配的 Android GKI DDK：

```sh
make -C /opt/ddk/kdir/<kmi> M="$PWD/vendor" modules
```

更多使用要求見 [`../USER_GUIDE.md`](../USER_GUIDE.md)。
