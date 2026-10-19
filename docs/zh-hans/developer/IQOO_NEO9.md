# 适配 vivo iQOO Neo9（厂商内核）

> 本文为 `lkm4ctr` 在 vivo iQOO Neo9 上的适配说明。这台机器的内核是 vivo
> 厂商内核，不是纯 GKI，所以**不能用项目默认的纯 GKI 产物**，必须按本文用
> vivo 官方 GPL 源码重建。配套文件：
> - 设备内核配置：`vendor-configs/iqoo-neo9.config`
> - 构建脚本：`scripts/build-vendor-iqoo-neo9.sh`
> - CI：`.github/workflows/build-vendor-iqoo-neo9.yml`

## 1. 设备与内核信息

| 项目 | 值 |
| --- | --- |
| 机型 | vivo iQOO Neo9，V2338A / PD2338 |
| 平台 | kalama（SM8550，Snapdragon 8 Gen 2） |
| 系统 | Android 15（SDK 35） |
| 内核 | `5.15.178-gaacdc35637c4-dirty`，aarch64，原厂 vivo 内核 |
| 工具链 | `Android (8508608, based on r450784e) clang 14.0.7`（即 NDK r25c 的 clang） |
| Root 方式 | KernelSU 以 LKM 形式加载在原厂内核之上（未刷机，`kernelsu`/`vrpatch`/`unpatch` 均为模块） |

## 2. 为什么纯 GKI 的 `android13-5.15` 产物加载失败

直接 `insmod` 项目默认构建的 `lkm4ctr-android13-5.15-arm64.ko`，内核报错：

```
lkm4ctr: version magic '5.15.202-android13-5.15.202_r00-dirty SMP preempt mod_unload modversions aarch64'
        should be '5.15.178-gaacdc35637c4-dirty SMP preempt mod_unload modversions vivo aarch64'
```

根因有三点（均来自 `/proc/config.gz`）：

1. **vermagic 字符串对不上**：默认产物按纯 GKI `android13-5.15.202` 构建，
   而设备是 vivo 改过的 `5.15.178`，版本号、git 后缀都不同。
2. **`CONFIG_VIVO_MODULE_VERMAGIC=y`**：vivo 在 `vermagic.h` 里追加了 `vivo`
   标志位，vermagic 里那个 `vivo` token 纯 GKI 模块永远不会有。
3. **`CONFIG_VIVO_VMET_BREAK_KMI=y`**（决定性）：vivo **故意打破了 GKI KMI**。
   这意味着内核导出符号的 CRC、甚至部分结构体布局都和纯 GKI 不一样。

普通 `insmod` 因为第 1、2 点直接被拒。`ksud` 强制 `insmod`（绕过 vermagic +
CRC 校验）时，模块按 GKI 的 ABI 去调用 vivo 内核里**布局已经变了的**符号
（nsproxy、task_struct、ftrace/kprobe hook 目标等），于是直接 **panic 重启**。
这跟 `VENDOR_KERNEL.md` 反复警告的“对厂商内核不能用纯 GKI 产物强刷”完全吻合。

## 3. 正确修法：用 vivo 官方源码重建

设备跑的是**原厂 vivo 内核**（root 走 LKM、未刷机），所以用 vivo 官方 GPL
源码 + 设备自己的 `.config` 重建，得到的 `Module.symvers`（符号 CRC）与原厂
内核一致，再强制 vermagic 字符串与设备完全相同，模块即可干净加载，且 ABI
匹配、不会 panic。

### 3.1 获取 vivo 内核源码

vivo 官方 GPL 源码发布在 <https://opensource.vivo.com/>（无检索栏，需手动浏览）：

- 文件名：`android_15.0_kernel_SM8550.tar.gz`
- 项目：project 139（对应 PD2338 / SM8550）

下载后供构建使用（CI 里填 URL，本地构建填路径）。

> 参考：`linux-tools/vivo_iqoo_neo_9_root_research_on_CVE-2025-21479`
> 证实了同一内核 `5.15.178-gaacdc35637c4-dirty` 即由该源码构建，且 KernelSU
> 以 LKM 加载、未刷机。

### 3.2 关键构建要点

构建脚本 `scripts/build-vendor-iqoo-neo9.sh` 已经处理好这些细节：

1. **用设备自己的 `.config`**（`vendor-configs/iqoo-neo9.config`，由
   `zcat /proc/config.gz` 抓取），保证 CRC 与原厂一致。
2. **工具链对齐**：用 NDK r25c 的 clang 14.0.7（`r450784e`，与设备完全一致）。
3. **强制 vermagic 版本字符串**：
   `LOCALVERSION=-gaacdc35637c4-dirty LOCALVERSION_AUTO=n`。
   `LOCALVERSION` 会被追加到 `KERNELRELEASE`（已经是 `5.15.178`），所以只填
   后缀 `-gaacdc35637c4-dirty`，最终 `UTS_RELEASE` =
   `5.15.178-gaacdc35637c4-dirty`，与设备完全相同；同时关掉
   `LOCALVERSION_AUTO` 避免源码树 git 状态再追加错误的 `-g<hash>`。
4. **`vivo` token 与 flags 自动继承**：构建用的是 vivo 源码树，其
   `vermagic.h` 已追加 `vivo` 标志；`SMP preempt mod_unload modversions`
   由设备 `.config` 里 `CONFIG_SMP/PREEMPT/MODULE_UNLOAD/MODVERSIONS=y`
   提供。模块编译时自动从该树继承，无需手动改。
5. **`make ... -k vmlinux modules`**：先建 `vmlinux` 得到核心符号 CRC
   （写入 `Module.symvers`，这是 `lkm4ctr` 实际依赖的全部），`-k` 让个别
   厂商 DLKM 编译失败也不中断，核心 CRC 仍完整。

## 4. 怎么跑

### GitHub Actions（推荐）

在仓库 Actions 页选 “Build lkm4ctr for iQOO Neo9 (vendor kernel)”，
`vivo_tarball_url` 填上第 3.1 步拿到的 `android_15.0_kernel_SM8550.tar.gz`
直链，其余留默认即可。产物 `lkm4ctr-iqoo-neo9-vendor-arm64` 含
`lkm4ctr-4.0.5-iqoo-neo9-vendor-arm64*.ko` 与 `Module.symvers.iqoo-neo9`。

> 没有 vivo URL 的 push/PR 触发时，workflow 只做脚本语法检查，不会失败。

### 本地构建（WSL / Linux，与 vivo KernelSU LKM 相同环境）

```sh
# 1. 准备 vivo 源码包（从 opensource.vivo.com 下载）
export VIVO_KERNEL_TARBALL=/path/to/android_15.0_kernel_SM8550.tar.gz
#  或：export VIVO_KERNEL_TARBALL_URL=https://.../android_15.0_kernel_SM8550.tar.gz
#  或：export VIVO_KERNEL_SRC=/path/to/already-extracted-vivo-source

# 2. 跑脚本（会自动下载 NDK r25c 作为工具链）
./scripts/build-vendor-iqoo-neo9.sh
# 产物在 out/lkm4ctr-4.0.5-iqoo-neo9-vendor-arm64*.ko
```

## 5. 设备上验证

```sh
# 推到设备后，先用普通 insmod 验证（现在 vermagic + CRC 都对得上，应能干净加载）
sudo insmod /data/local/tmp/lkm4ctr-4.0.5-iqoo-neo9-vendor-arm64.ko
sudo dmesg | tail        # 应看到 lkm4ctr init 横幅，无 panic
lsmod | grep lkm4ctr
# 用完卸载
sudo rmmod lkm4ctr
```

> **hook 数量提示**：vivo 构建因第 7 节原因禁用了 `openat`/`open`/`openat2`
> 三个 hook，加载横幅应显示 `loaded (31 hook(s) installed)`，**不会**出现
> `__arm64_sys_openat` / `__arm64_sys_open` / `__arm64_sys_openat2` 三行
> `hooked ... at 0x...`。这是预期行为，不是安装失败。

## 6. 万一 CRC 仍对不上（迭代指引）

理论上原厂内核 + 官方源码 + 设备 `.config` + 同款 clang 14.0.7 → CRC 一致。
若 `insmod` 报 `disagrees about version of symbol X`，说明该符号 CRC 仍有
差异，按序排查：

1. 确认 vivo 源码版本与设备一致（`/proc/version` 的 git 后缀 `gaacdc35637c4`
   对应源码 commit；`-dirty` 是 vivo 自身构建时未提交改动）。
2. 确认 `.config` 就是这台机器的（`zcat /proc/config.gz`，不要用别人的）。
3. 确认工具链是 clang 14.0.7（脚本会警告非 14.0.7）。
4. 若设备刷过**非原厂**内核（自编译带额外 patch），则需把同样的 patch 应用
   到源码树再重建；纯原厂内核不会有此问题。
5. 个别符号缺失可在 `out/Module.symvers.iqoo-neo9` 里 grep 确认；`lkm4ctr`
   的非导出内核函数符号不依赖 CRC（运行时 `shadow_hook_resolve()` 按名字经
   kallsyms 解析，本机 `CONFIG_KALLSYMS_ALL=y` 已保证可解析）。

## 7. openat hook 在 vivo LTO+CFI 内核上会触发 panic（已用构建开关禁用）

实测：lkm4ctr 4.0.5 能在本机干净加载（34 个 hook 全装上），但加载后**任何
密集文件系统遍历**（`find /`、`ls -R /`、`chroot info` 等）会在 ~2 分钟内
panic 重启。崩溃栈固定落在：

```
vendor_kernel_hook_openat+0x24/0x44 [lkm4ctr]
kretprobe_trampoline.cfi_jt+0x0/0x8
```

根因：vivo 内核 `CONFIG_LTO_CLANG_FULL=y + CONFIG_CFI_CLANG=y`。lkm4ctr 的
`openat`/`open`/`openat2` hook 通过函数指针 `real_sys_openat(regs)` 间接
调用原始 syscall（地址由 `shadow_hook_resolve()` 按名字经 kallsyms 解析），
该间接调用在 vivo 的 LTO+CFI 下不可靠；`openat` 是全系统最高频 syscall，
密集遍历下统计上很快命中坏跳转 → panic。本机又**无 `CONFIG_USER_NS`**，
这三个 hook 的 `/proc/<pid>/{uid_map,gid_map,...}` ENOENT 兑底本就无意义。

**处置**：用构建开关 `LKM4CTR_DISABLE_OPENAT_HOOK` 禁用这三个 hook，保留其余
31 个（`proc_ns_dir_lookup`、IPC syscalls、`copy_namespaces`、cred、
`getuid`/`setuid` 等）。开关默认 `n`（GKI 矩阵构建保留 openat hook，那些内核
有 `CONFIG_USER_NS` 且无此 CFI 问题），仅 vivo 构建脚本
`scripts/build-vendor-iqoo-neo9.sh` 传 `LKM4CTR_DISABLE_OPENAT_HOOK=y`。门控在
`glue/vendor_kernel_procfs.c` 的 `#ifndef CONFIG_LKM4CTR_DISABLE_OPENAT_HOOK`，
开关定义见 `vendor/Makefile`。

> 此改动**未解决** `# CONFIG_CGROUP_DEVICE is not set`（`mount -t cgroup -o
> devices` 返回 `Invalid argument`，Termux `dockerd` 启动失败）。禁 openat
> hook 只是让模块本身不再 panic；要让 Docker 跑起来还需另行处理 cgroup
> devices（本项目 4.0.5 已移除 `shadow_cgdevices` 子模块，不补）。

## 8. 相关文档

- 厂商内核整体设计：[`architecture/GLUE.md`](../architecture/GLUE.md)、
  [`VENDOR_KERNEL.md`](./VENDOR_KERNEL.md)
- 项目默认 GKI 构建方式（与本机不同）：仓库根 `README.md`、`AGENTS.md`
