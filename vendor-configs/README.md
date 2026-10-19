# vendor-configs

Device kernel `.config` files captured from real devices, used to build `lkm4ctr`
against a **vendor** kernel (not a stock GKI KMI).

A stock GKI `android13-5.15` build does **not** load on these devices: their
vendor kernels carry OEM Kconfig options that change the vermagic string
(e.g. `CONFIG_VIVO_MODULE_VERMAGIC=y` appends a `vivo` token) and deliberately
break the GKI KMI (e.g. `CONFIG_VIVO_VMET_BREAK_KMI=y`), so both the vermagic
string and every exported-symbol CRC differ from stock GKI. Force-loading a
stock GKI module past those checks panics, because the actual ABI no longer
matches. The only safe build is against the device's own kernel source tree
using the exact `.config` here (captured via `zcat /proc/config.gz`).

## Files

- `iqoo-neo9.config` — vivo iQOO Neo9 (V2338A / PD2338, SM8550 / kalama),
  kernel `5.15.178-gaacdc35637c4-dirty`, Android 15. Captured from the running
  kernel's `/proc/config.gz`. Key options:
  - `CONFIG_VIVO_MODULE_VERMAGIC=y` → vermagic carries a `vivo` token
  - `CONFIG_VIVO_VMET_BREAK_KMI=y` → vivo intentionally breaks the GKI KMI
  - `CONFIG_LTO_CLANG_FULL=y` + `CONFIG_CFI_CLANG=y` → full LTO + CFI
  - `CONFIG_CC_VERSION_TEXT="... clang version 14.0.7 ... r450784e"` →
    built with NDK r25c's clang; the vendor build must use the same toolchain
  - `CONFIG_LOCALVERSION_AUTO=y` (overridden to `n` at build time, see
    `scripts/build-vendor-iqoo-neo9.sh`, so the `-gaacdc35637c4-dirty` suffix
    is supplied explicitly instead of regenerated from git)

## Capturing a new device's config

```sh
sudo zcat /proc/config.gz > vendor-configs/<device>.config
# then record in this README: model, platform, kernel release, key OEM options
```
