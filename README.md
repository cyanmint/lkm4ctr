# lkm4ctr

`lkm4ctr` automatically loads `lkm4ctr.ko` to enable Docker and containers on GKI-compatible Android kernels with a matching arm64 KMI build, without flashing a custom kernel. The module is intended for rooted devices and must match the device KMI exactly.

See [LICENSES.md](LICENSES.md) for the separation between AI-generated Public Domain files and GPL-licensed sources.

## Start here

1. Check the device kernel: `uname -a`
2. Either download the matching `lkm4ctr-4.0.5-<kmi>-arm64.ko` release asset or install `lkm4ctr-kernelsu-module.zip` in KernelSU; the KernelSU module selects the matching KMI automatically
3. Install `fuse-overlayfs` and configure Docker to use it by default (see the user manual)
4. For a standalone `.ko` install, load it as root: `sudo insmod /path/to/lkm4ctr-4.0.5-<matching-kmi>-arm64.ko` (KernelSU installs and loads the bundled module itself)
5. Start Docker: `sudo dockerd --config-file="$PREFIX/etc/docker/daemon.json" --bridge=none --experimental`
6. Test a container: `sudo docker run --network=host --rm -it docker.io/library/alpine:latest /bin/sh`

`NET_NS` is currently bookkeeping-only, so host networking is required for network access. Read the manual before loading a module on a real device.

The KernelSU ZIP includes every KMI build and `lkm4ctr_checker`, enables boot auto-load by default, and provides an action button plus a multilingual WebUI for module control and checker output. Published releases include an update feed so KernelSU Manager can offer in-app updates when `versionCode` increases.

## Documentation

- [English user manual](docs/en/USER_GUIDE.md) · [简体中文](docs/zh-hans/USER_GUIDE.md) · [繁體中文](docs/zh-hant/USER_GUIDE.md)
- [English architecture and developer documentation](docs/en/architecture/GLUE.md)
- [English checker documentation](docs/en/developer/CHECKER.md)
- [English vendor-kernel documentation](docs/en/developer/VENDOR_KERNEL.md)
- [简体中文文档索引](docs/zh-hans/README.md) · [繁體中文文件索引](docs/zh-hant/README.md)

`AGENTS.md` is the repository's contributor/agent contract and intentionally remains at the repository root.
