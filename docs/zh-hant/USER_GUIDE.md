# lkm4ctr 使用手冊

`lkm4ctr` 是面向已 root、arm64 Android GKI 裝置的樹外 Linux 核心模組。它在模組載入後提供部分容器所需的核心能力，讓未修改的 `containerd`/`runc`/`dockerd` 可以執行，而不必重新編譯或刷寫 Android 核心。

這是一份使用手冊，不代表完整的 Linux 容器相容性。請先閱讀限制，再在重要裝置上使用。

## 1. 專案提供什麼

專案產生兩個彼此獨立的產物：

- `lkm4ctr-<kmi>-arm64.ko`：核心模組。它是合併模組，包含 vendored namespace、IPC/mqueue、overlayfs 實作以及 hook/glue 程式碼。
- `lkm4ctr_checker-arm64`：可選的使用者空間診斷程式。它不是核心模組，不需要 `insmod`，會執行真實系統呼叫並回報 `PASS`、`STUB` 或 `FAIL`。

模組的目標是讓原版容器執行時直接工作，不需要專案專用的執行時補丁。

## 2. 要求

### 裝置

- 已 root 的 **arm64** Android 裝置。範例中的 `sudo` 表示 Termux 中可取得 root 的設定，也可以改用 `su -c`。
- 目前支援的 Android GKI KMI 系列：`android12-5.10`、`android13-5.10`、`android13-5.15`、`android14-5.15`、`android14-6.1`、`android15-6.6`、`android16-6.12`。
- 與裝置 **完全匹配** 的 KMI 和核心 ABI 模組。`android14-5.15` 不能替代 `android14-6.1`。
- 裝置核心仍需具備專案要求的其他能力，尤其是 `CONFIG_NAMESPACES=y`、`CONFIG_NET_NS=y` 和 `CONFIG_TIME_NS=y`。樹外模組不能安全地補出所有核心子系統。
- 足夠的儲存空間與記憶體。

不需要重新編譯或刷寫 Android 核心，但必須具備可載入核心模組的環境。Android 安全政策、KernelSU/Magisk 政策、模組簽章強制或 SELinux 仍可能拒絕 `insmod`。

### Termux 與使用者空間

從可信來源安裝 Termux，更新套件，並安裝目前架構可用的 root 工具與容器工具：

```sh
pkg update
pkg install root-repo sudo
apt install dockerd docker-cli docker-compose
```

Termux 軟體源的套件名稱和可用性可能變化，請確認命令存在：

```sh
command -v sudo dockerd docker
```

模組內建 vendored overlayfs，單純載入模組不要求 `fuse-overlayfs`。但 Docker 資料根目錄必須位於 overlay 實作接受的檔案系統上。Android `/data` 常見支援 casefold 的 F2FS，可能導致 `overlay2` 失敗。優先使用未啟用 casefold 的 ext4 loopback 映像；或在了解效能與儲存代價後使用 Docker `vfs` 驅動程式。

## 3. 確認 KMI

在裝置上執行：

```sh
uname -a
getprop ro.build.version.release
```

例如核心字串包含 `android14-6.1`，對應 KMI 就是 `android14-6.1`。只看 Android 版本號不夠；如果 `uname -a` 不明確，請結合核心 release/vendor 建置資訊，選擇檔名中 KMI 完全一致的模組。

## 4. 取得產物

最簡單的方式是從專案的 [Releases 頁面](https://github.com/cyanmint/lkm4ctr/releases) 下載：

```text
lkm4ctr-android14-6.1-arm64.ko
lkm4ctr_checker-arm64
```

下載與裝置 `<kmi>` 完全一致的 `.ko`。不要透過改名掩蓋不匹配，也不要載入其他 KMI 的模組。

```sh
mkdir -p "$HOME/lkm4ctr"
cp /sdcard/Download/lkm4ctr-android14-6.1-arm64.ko "$HOME/lkm4ctr/lkm4ctr.ko"
cp /sdcard/Download/lkm4ctr_checker-arm64 "$HOME/lkm4ctr/"
chmod 755 "$HOME/lkm4ctr/lkm4ctr_checker"
```

## 5. 載入前執行診斷

以 root 或擁有 `CAP_SYS_ADMIN` 的身份執行：

```sh
sudo "$HOME/lkm4ctr/lkm4ctr_checker"
printf 'exit status: %s\n' "$?"
```

`PASS` 表示觀察到真實效果，`STUB` 表示系統呼叫被接受但沒有真實隔離，`FAIL` 表示系統呼叫失敗。載入前執行可作為基線；載入後再次執行並比較 namespace、IPC 和 overlay 結果。

非 root 執行通常會得到 `EPERM`，這表示權限不足，不代表模組損壞。

## 6. 載入模組

更換已載入模組前先停止容器工作負載，然後載入完全匹配的檔案：

```sh
sudo insmod "$HOME/lkm4ctr/lkm4ctr.ko"
cat /proc/modules | grep '^lkm4ctr '
sudo "$HOME/lkm4ctr/lkm4ctr_checker"
```

如果 `insmod` 失敗，請保留完整錯誤並檢查核心日誌。常見原因包括 KMI/ABI 不匹配、unknown symbol、模組簽章限制、root 能力不足或核心版本不在支援範圍。不要強行繞過 unknown-symbol 或版本錯誤。

## 7. 啟動 Docker 並執行容器

在 Termux 中啟動 daemon：

```sh
sudo dockerd --bridge=none --experimental
```

在另一個 Termux 會話等待 socket，然後執行第一個容器：

```sh
until [ -S /data/data/com.termux/files/usr/var/run/docker.sock ] || [ -S /var/run/docker.sock ]; do sleep 1; done
sudo docker info
sudo docker run --network=host --rm -it docker.io/library/alpine:latest /bin/sh
```

目前文件化可用的網路模式是 host networking。專案沒有提供真正的 network namespace 實作，因此 bridge 等隔離網路模式不受支援，容器可能啟動但無法聯網。

## 8. 可選的診斷檔案系統

模組提供診斷檔案系統，可用於定位 `insmod` 後的容器執行時錯誤：

```sh
sudo mkdir -p /mnt/lkm4ctr
sudo mount -t lkm4ctr diag /mnt/lkm4ctr
sudo cat /mnt/lkm4ctr/status
sudo cat /mnt/lkm4ctr/log
sudo cat /mnt/lkm4ctr/v/status
sudo cat /mnt/lkm4ctr/v/resources
sudo cat /mnt/lkm4ctr/v/namespaces
```

這不是 Docker 的必要步驟。停止 Docker 並卸載診斷檔案系統後，才請求一般卸載：

```sh
sudo umount /mnt/lkm4ctr
sudo rmmod lkm4ctr
```

模組會追蹤正在執行的 hook，並可能拒絕不安全卸載。進階使用者可使用 diagfs 的 hot-reload：

```sh
sudo sh -c 'printf "%s\n" "/absolute/path/to/new/lkm4ctr.ko" > /mnt/lkm4ctr/v/hotreload/do-hot-reload'
sudo cat /mnt/lkm4ctr/v/hotreload/status
```

只有在停止容器工作負載後才使用。裝置狀態不確定時，恢復方式是重新啟動，並確保不自動載入舊模組。

## 9. 已知限制

- 這不是所有 Android 核心容器能力的通用替代品。
- `NET_NS` 目前只有 bookkeeping，使用 `--network=host`。
- `CGROUP_NS` 只有 bookkeeping；專案不提供 cgroup 檔案系統或控制器。因此 systemd 作為容器 PID 1 需要主機提供可用的 cgroup v2 掛載，仍可能失敗。
- `MNT_NS` 依賴執行核心自己的 mount namespace 支援。
- `redroid` 的 `init: Failed to initialize property area` 通常是使用者空間 tmpfs/xattr/SELinux 問題，不是本模組解決的問題。
- casefold 檔案系統上的 overlay 掛載可能失敗。將 Docker 資料根目錄移到合適的非 casefold ext4 檔案系統，或使用 `vfs`。
- 不支援 `SHM_HUGETLB`；普通 SysV 共用記憶體受支援。
- Release 產物綁定建置時的 KMI，下載成功不代表可以用於其他核心。

權威實作限制見 [`vendor/README.md`](../../vendor/README.md)。

## 10. 自行建置

自行建置裝置模組需要匹配的 Android GKI DDK。只有 `modules_prepare` 的普通核心原始碼樹不夠用：它沒有安全符號解析所需的真實 `vmlinux`/`Module.symvers` 資料，產生的模組可能在 `insmod` 時失敗甚至使核心崩潰。

在匹配的 DDK 容器中：

```sh
make -C /opt/ddk/kdir/<kmi> M="$PWD/vendor" modules
```

CI 使用 `ghcr.io/ylarod/ddk-min:<kmi>-20260313`。診斷程式獨立建置：

```sh
make -C lkm4ctr_checker CC=aarch64-linux-gnu-gcc
```

不要把使用無關主機標頭的建置結果當成裝置可用模組。

## 11. 提交問題

請提供：`uname -a` 和裝置資訊、完整 `.ko` 檔名及 release/tag、載入前後 checker 結果、完整 `insmod` 錯誤和相關核心日誌、Docker 輸出及 diagfs 輸出（如有），以及最小重現命令。發布日誌前請移除不必要的裝置識別碼、序號和隱私資訊。
