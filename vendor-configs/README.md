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
  - `CONFIG_VIVO_MODULE_VERMAGIC=y` → assumed to add a `vivo` vermagic token,
    BUT the public GPL source tree has NO implementation of this Kconfig (no
    Kconfig definition, no `vermagic.h` change) and the device's own vendor
    DLKMs carry no `vivo` token, so vermagic is plain `... modversions aarch64`.
  - `CONFIG_VIVO_VMET_BREAK_KMI=y` → vivo deliberately breaks the GKI KMI
  - `CONFIG_LTO_CLANG_FULL=y` + `CONFIG_CFI_CLANG=y` → full LTO + CFI
  - `CONFIG_CC_VERSION_TEXT="... clang version 14.0.7 ... r450784e"` →
    built with NDK r25c's clang; the vendor build must use the same toolchain
    (for struct-layout/ABI fidelity; symbol CRCs come from the device-real
    table below, not genksyms)
  - `CONFIG_LOCALVERSION_AUTO=y` (overridden to `n` at build time, see
    `scripts/build-vendor-iqoo-neo9.sh`, so the `-gaacdc35637c4-dirty` suffix
    is supplied explicitly instead of regenerated from git)
- `iqoo-neo9.device-symvers` — the REAL `Module.symvers` (symbol CRC table)
  captured from the device's RUNNING kernel, NOT from the public GPL source
  tree. This is mandatory: the vivo public GPL source produces CRCs that do
  NOT match the closed-source kernel actually running on the device (e.g.
  `module_layout` is `0x0222dd63` from the GPL tree vs `0x5ec51e3c` on the
  running device). vivo's own vendor DLKMs bypass `modversions`, so they load
  despite the mismatch; a third-party module built against the GPL CRCs fails
  with `disagrees about version of symbol module_layout`. Regenerate with
  `scripts/extract-symvers-from-device.sh` (run ON the rooted device — it reads
  `/proc/kallsyms` + the active boot slot's kernel image `__kcrctab` table).

## Capturing a new device's config

```sh
sudo zcat /proc/config.gz > vendor-configs/<device>.config
# then record in this README: model, platform, kernel release, key OEM options
```

## Capturing a new device's real symbol CRC table

For a vendor kernel whose public GPL source produces CRCs that don't match the
running binary (vivo iQOO Neo9 is one), you must also capture the real
`Module.symvers` from the running kernel, then build against it:

```sh
# On the rooted device (needs CONFIG_KALLSYMS_ALL=y so /proc/kallsyms lists
# __start___ksymtab etc. with real addresses):
sudo scripts/extract-symvers-from-device.sh
#  -> writes out/Module.symvers.<device>.device, move it to vendor-configs/
cp out/Module.symvers.iqoo-neo9.device vendor-configs/iqoo-neo9.device-symvers
# then build with it (the build script picks this up automatically):
DEVICE_SYMVERS=vendor-configs/iqoo-neo9.device-symvers ./scripts/build-vendor-iqoo-neo9.sh
```

Verify the build baked the right CRC: the build log prints
`baked module_layout CRC: 0x5ec51e3c` and `OK: ... matches device`.
