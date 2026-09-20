# lkm4ctr

`lkm4ctr` is an arm64 Android GKI kernel module that supplies selected container prerequisites without recompiling the Android kernel. It is intended for rooted devices and must match the device KMI exactly.

## Start here

1. Check the device kernel: `uname -a`
2. Download the matching `lkm4ctr-<kmi>-arm64.ko` release asset
3. Load it as root: `sudo insmod /path/to/lkm4ctr-<matching-kmi>-arm64.ko`
4. Start Docker: `sudo dockerd --bridge=none --experimental`
5. Test a container: `sudo docker run --network=host --rm -it docker.io/library/alpine:latest /bin/sh`

`NET_NS` is currently bookkeeping-only, so host networking is required for network access. Read the manual before loading a module on a real device.

## Documentation

- [English user manual](docs/en/USER_GUIDE.md) · [简体中文](docs/zh-hans/USER_GUIDE.md) · [繁體中文](docs/zh-hant/USER_GUIDE.md)
- [English architecture and developer documentation](docs/en/architecture/GLUE.md)
- [English checker documentation](docs/en/developer/CHECKER.md)
- [English vendor-kernel documentation](docs/en/developer/VENDOR_KERNEL.md)
- [简体中文文档索引](docs/zh-hans/README.md) · [繁體中文文件索引](docs/zh-hant/README.md)

`AGENTS.md` is the repository's contributor/agent contract and intentionally remains at the repository root.
