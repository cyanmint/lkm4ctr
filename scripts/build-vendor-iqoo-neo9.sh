#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# build-vendor-iqoo-neo9.sh — build lkm4ctr.ko for the vivo iQOO Neo9 vendor kernel.
#
# The iQOO Neo9 (V2338A / PD2338, SM8550 / kalama) runs a vivo vendor kernel
# `5.15.178-gaacdc35637c4-dirty` whose vermagic carries an extra `vivo` token
# (CONFIG_VIVO_MODULE_VERMAGIC) and which deliberately breaks the GKI KMI
# (CONFIG_VIVO_VMET_BREAK_KMI). A stock GKI `android13-5.15` lkm4ctr build
# therefore cannot load: plain insmod rejects the vermagic, and a forced load
# (e.g. ksud) panics because the real ABI no longer matches.
#
# This script rebuilds the kernel from vivo's GPL source with the device's
# exact /proc/config.gz so Module.symvers carries the real symbol CRCs, forces
# the vermagic release string to match the running kernel, and then builds
# lkm4ctr against that tree. It is intended to run on an x86_64 Linux host
# (CI runner or local WSL — the same environment the vivo KernelSU LKM uses).
#
# Required inputs (env vars):
#   VIVO_KERNEL_TARBALL       local path to android_15.0_kernel_SM8550.tar.gz
#   or VIVO_KERNEL_TARBALL_URL URL to download it from
#   or VIVO_KERNEL_SRC         pre-extracted kernel source tree (top-level Makefile)
# Optional:
#   NDK_DIR                   path to an extracted NDK r25c (clang 14.0.7)
#   NDK_URL                   URL to download NDK from (default: r25c)
#   DEVICE_SYMVERS             path to a REAL Module.symvers captured from the
#                             device's RUNNING kernel (see
#                             extract-symvers-from-device.sh). When set, this
#                             OVERRIDES the vmlinux.symvers the build itself
#                             produces, because the vivo public GPL source tree
#                             yields CRCs that do NOT match the closed-source
#                             kernel actually running on the device (vivo's own
#                             DLKMs are loaded via a CRC-bypass path, so they
#                             pass despite mismatched CRCs; a third-party module
#                             is not, and fails with
#                             "disagrees about version of symbol module_layout").
#                             vmlinux is still built (for modules_prepare host
#                             tools), only its CRC table is discarded.
#                             STRONGLY RECOMMENDED for this device.
#   DEVICE_CONFIG              path to the device .config
#                             (default: <repo>/vendor-configs/iqoo-neo9.config)
#   TARGET_RELEASE             vermagic release string to force
#                             (default: 5.15.178-gaacdc35637c4-dirty)
#   OUT_DIR                    where to write the .ko (default: out/)
#   LKM4CTR_REPO               repo root (default: parent of this script's dir)
#   JOBS                       -j value (default: 2; see note on LTO memory peak)
#
# The vivo GPL source tarball is NOT hosted by this project and must be obtained
# from https://opensource.vivo.com/ (project 139, file android_15.0_kernel_SM8550.tar.gz).
set -uo pipefail

# ─── defaults ────────────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
: "${LKM4CTR_REPO:="$(cd "$SCRIPT_DIR/.." && pwd)"}"
: "${DEVICE_CONFIG:="$LKM4CTR_REPO/vendor-configs/iqoo-neo9.config"}"
# DEVICE_SYMVERS default is resolved in step 5 (it needs the repo path and has
# none|gpl|- disable semantics); no static default here.
: "${TARGET_RELEASE:="5.15.178-gaacdc35637c4-dirty"}"
# LOCALVERSION is appended to KERNELRELEASE (already "5.15.178" from the
# source's VERSION/PATCHLEVEL/SUBLEVEL), so it must be ONLY the suffix that
# follows the base version in the running kernel's release string — NOT the
# full release. With LOCALVERSION_AUTO=n this yields UTS_RELEASE=$TARGET_RELEASE.
: "${TARGET_LOCALVERSION:="-gaacdc35637c4-dirty"}"
: "${OUT_DIR:="$LKM4CTR_REPO/out"}"
: "${JOBS:="2"}"
# Default to -j2, not nproc: the LTO_CLANG_FULL + CFI_CLANG vmlinux.o link is a
# single-process memory peak (lld loads all bitcode at once). On memory-lean
# hosts (e.g. GitHub 16GB runners) -j4 makes the compile phase eat RAM early,
# leaving no headroom for the link peak, which then OOM-kills the runner. -j2
# keeps the compile phase lighter; the link step isn't parallelized by -j anyway.
# Override with JOBS=<n> for beefier hosts.
: "${NDK_URL:="https://dl.google.com/android/repository/android-ndk-r25c-linux.zip"}"

# vermagic flags the running kernel expects (after the release token).
# NOTE: a `vivo` token was previously assumed here based on
# CONFIG_VIVO_MODULE_VERMAGIC=y, but that Kconfig has NO implementation in the
# public GPL source tree (no Kconfig definition, no vermagic.h change), and the
# device's own vendor DLKMs carry the plain flags below with NO vivo token
# (verified: strings /vendor_dlkm/lib/modules/5.15-gki/adsp_loader_dlkm.ko).
# So the real expected vermagic has no `vivo` token.
EXPECTED_VERMAGIC="$TARGET_RELEASE SMP preempt mod_unload modversions aarch64"

log() { printf '\033[1;34m[build-vendor]\033[0m %s\n' "$*" >&2; }
err() { printf '\033[1;31m[build-vendor]\033[0m %s\n' "$*" >&2; }
die() { err "$*"; exit 1; }

# ─── workspace ───────────────────────────────────────────────────────────────
WORK="$(mktemp -d -t vivo-iqoo-neo9-XXXXXX)"
trap 'rm -rf "$WORK"' EXIT
log "workspace: $WORK"

need_cmd() { command -v "$1" >/dev/null 2>&1 || die "missing required command: $1 (install it on the build host)"; }
for c in make gcc bc flex bison openssl cpio tar unzip wget curl python3; do need_cmd "$c"; done

# ─── 1. toolchain: NDK r25c (clang 14.0.7, matches device r450784e) ──────────
setup_ndk() {
  if [ -n "${NDK_DIR:-}" ] && [ -x "$NDK_DIR/toolchains/llvm/prebuilt/linux-x86_64/bin/clang" ]; then
    log "using provided NDK_DIR=$NDK_DIR"
    return
  fi
  if [ -n "${NDK_DIR:-}" ] && [ -x "$NDK_DIR/toolchains/llvm/prebuilt/*/bin/clang" ]; then :; fi
  local ndk_root="$WORK/ndk"
  mkdir -p "$ndk_root"
  if [ ! -d "$ndk_root/android-ndk-r25c" ]; then
    local zip="$ndk_root/ndk.zip"
    log "downloading NDK r25c from $NDK_URL"
    ( curl -fsSL "$NDK_URL" -o "$zip" || wget -O "$zip" "$NDK_URL" ) || die "NDK download failed"
    log "extracting NDK"
    unzip -q "$zip" -d "$ndk_root" || die "NDK unzip failed"
  fi
  NDK_DIR="$ndk_root/android-ndk-r25c"
  [ -x "$NDK_DIR/toolchains/llvm/prebuilt/linux-x86_64/bin/clang" ] \
    || die "NDK clang not found under $NDK_DIR"
  log "NDK ready: $NDK_DIR"
}
setup_ndk
CLANG_BIN="$NDK_DIR/toolchains/llvm/prebuilt/linux-x86_64/bin"
export PATH="$CLANG_BIN:$PATH"
clang_v="$(clang --version | head -1)"; log "clang: $clang_v"
echo "$clang_v" | grep -q "14\.0\.7" || err "WARNING: clang is not 14.0.7 — CRCs may not match the device (device built with r450784e/14.0.7)"

# ─── 2. acquire vivo kernel source ────────────────────────────────────────────
setup_source() {
  if [ -n "${VIVO_KERNEL_SRC:-}" ]; then
    [ -f "$VIVO_KERNEL_SRC/Makefile" ] || die "VIVO_KERNEL_SRC has no Makefile: $VIVO_KERNEL_SRC"
    SRC="$VIVO_KERNEL_SRC"; log "using provided source: $SRC"; return
  fi
  local tb=""
  if [ -n "${VIVO_KERNEL_TARBALL:-}" ] && [ -f "$VIVO_KERNEL_TARBALL" ]; then
    tb="$VIVO_KERNEL_TARBALL"
  elif [ -n "${VIVO_KERNEL_TARBALL_URL:-}" ]; then
    tb="$WORK/vivo-src.tar.gz"
    log "downloading vivo kernel source from $VIVO_KERNEL_TARBALL_URL"
    ( curl -fsSL "$VIVO_KERNEL_TARBALL_URL" -o "$tb" || wget -O "$tb" "$VIVO_KERNEL_TARBALL_URL" ) \
      || die "vivo source download failed"
  else
    die "provide VIVO_KERNEL_TARBALL, VIVO_KERNEL_TARBALL_URL, or VIVO_KERNEL_SRC"
  fi
  log "extracting vivo kernel source"
  mkdir -p "$WORK/src-extract"
  tar -xf "$tb" -C "$WORK/src-extract" || die "vivo source extract failed"
  # find the top-level kernel Makefile (VERSION + PATCHLEVEL)
  SRC="$(find "$WORK/src-extract" -maxdepth 3 -name Makefile -print0 \
    | xargs -0 grep -l '^VERSION *=' \
    | head -1 | xargs dirname)"
  [ -n "$SRC" ] && [ -f "$SRC/Makefile" ] || die "could not locate kernel source root in tarball"
  log "kernel source: $SRC"
}
setup_source

# ─── 3. apply device .config ──────────────────────────────────────────────────
[ -f "$DEVICE_CONFIG" ] || die "device config not found: $DEVICE_CONFIG"
log "using device config: $DEVICE_CONFIG"
cp -f "$DEVICE_CONFIG" "$SRC/.config"

# ─── 4. normalize config (silent; device config already matches this source) ──
log "make olddefconfig (ARCH=arm64 LLVM=1)"
make -C "$SRC" ARCH=arm64 LLVM=1 LLVM_IAS=1 olddefconfig \
  || die "olddefconfig failed"

# ─── 5. build vmlinux → vmlinux.symvers (core symbol CRCs) ─────────────────────
# LOCALVERSION=-gaacdc35637c4-dirty + LOCALVERSION_AUTO=n forces the exact
# UTS_RELEASE the running kernel reports, without relying on the source tree's
# git state. CONFIG_LOCALVERSION_AUTO=y in the device config is overridden here.
#
# We build ONLY vmlinux, not the in-tree `modules` target. Crucially, on 5.15
# `make vmlinux` produces `vmlinux.symvers` (the CRC table of every symbol
# exported by vmlinux proper), NOT `Module.symvers`. The final `Module.symvers`
# is only assembled by `make modules`, which cats `vmlinux.symvers` +
# `modules-only.symvers` (CRCs of symbols exported by in-tree DLKMs). Since
# lkm4ctr hooks core kernel symbols and depends on no in-tree DLKM's exports,
# vmlinux.symvers is exactly the CRC set it needs. We copy it to Module.symvers
# so the external module build's modpost finds the table in the place it looks.
# Building vmlinux alone skips compiling vivo's large pile of DLKMs
# (camera/audio/modem/etc.), cutting build time drastically while leaving the
# CRC table complete for the symbols that actually matter.
log "building vmlinux (LOCALVERSION=$TARGET_LOCALVERSION -> UTS_RELEASE=$TARGET_RELEASE, LTO+CFI per config)"
# Live output (no tail buffering) so a long LTO+CFI link doesn't look stuck.
make -C "$SRC" ARCH=arm64 LLVM=1 LLVM_IAS=1 \
  LOCALVERSION="$TARGET_LOCALVERSION" LOCALVERSION_AUTO=n \
  -j"$JOBS" vmlinux 2>&1 | tee "$WORK/build.log"
log "vmlinux build finished"

# `make vmlinux` emits vmlinux.symvers, not Module.symvers (see comment above).
# Mirror it to Module.symvers so step 6's `make M=... modules` modpost resolves.
[ -s "$SRC/vmlinux.symvers" ] || die "vmlinux.symvers missing/empty — vmlinux build failed; see $WORK/build.log"
cp -f "$SRC/vmlinux.symvers" "$SRC/Module.symvers"
sym_count="$(wc -l < "$SRC/Module.symvers")"
log "Module.symvers (from vmlinux.symvers): $sym_count entries"
[ "$sym_count" -gt 100 ] || err "WARNING: Module.symvers has only $sym_count entries — CRCs may be incomplete"

# Override with the device's REAL CRC table if provided. The public GPL source
# tree produces CRCs that do NOT match the closed-source kernel actually
# running on the device (e.g. module_layout is 0x0222dd63 from GPL vs
# 0x5ec51e3c on the running device). vivo's own DLKMs bypass modversions, so
# they load despite the mismatch; a third-party module does not, and fails with
# "disagrees about version of symbol module_layout". The device-extracted
# Module.symvers (from the running kernel's __kcrctab) carries the TRUE CRCs.
# We still keep the vmlinux build above: modules_prepare needs its host tools
# (scripts/module.lds etc.), only the CRC table is replaced here.
#
# DEVICE_SYMVERS semantics:
#   unset/empty  -> use the committed default (vendor-configs/iqoo-neo9.device-symvers)
#                  if present, else no override (fall back to GPL vmlinux.symvers)
#   none|gpl|-   -> explicitly DISABLE the override (use GPL vmlinux.symvers)
#   <path>       -> use that file
DEVICE_SYMVERS="${DEVICE_SYMVERS:-}"
DEVICE_SYMVERS="${DEVICE_SYMVERS%%[[:space:]]*}"  # trim trailing whitespace / detect ' '
case "$DEVICE_SYMVERS" in
  ""|none|gpl|-)
    if [ -z "$DEVICE_SYMVERS" ] && [ -f "$LKM4CTR_REPO/vendor-configs/iqoo-neo9.device-symvers" ]; then
      DEVICE_SYMVERS="$LKM4CTR_REPO/vendor-configs/iqoo-neo9.device-symvers"
    else
      DEVICE_SYMVERS=""
      log "DEVICE_SYMVERS disabled — using GPL vmlinux.symvers (will NOT match running device)"
    fi
    ;;
esac
if [ -n "$DEVICE_SYMVERS" ]; then
  [ -f "$DEVICE_SYMVERS" ] || die "DEVICE_SYMVERS set but file not found: $DEVICE_SYMVERS"
  log "OVERRIDING Module.symvers with device-extracted real CRCs: $DEVICE_SYMVERS"
  cp -f "$DEVICE_SYMVERS" "$SRC/Module.symvers"
  dev_count="$(wc -l < "$SRC/Module.symvers")"
  log "Module.symvers (device-real): $dev_count entries"
  # Spot-check: the device-real module_layout CRC should differ from the GPL one.
  dev_ml="$(grep -w module_layout "$SRC/Module.symvers" | awk '{print $1}')"
  log "  device-real module_layout CRC = $dev_ml (GPL vmlinux.symvers would be 0x0222dd63)"
  [ -n "$dev_ml" ] || err "WARNING: module_layout not found in device symvers — override may be wrong"
fi

# ─── 6. build lkm4ctr against the vendor tree ─────────────────────────────────
# External module linking into .ko needs scripts/module.lds (the linker script
# for modules) and other host tools that are only generated by `make
# modules_prepare`. `make vmlinux` alone does NOT produce them, so the build
# dies at the final link step with "No rule to make target 'scripts/module.lds'".
# modules_prepare is cheap (host scripts only — it does not compile the vendor
# DLKMs) and is the standard prerequisite for any out-of-tree `make M=` build.
log "make modules_prepare (generates scripts/module.lds + host tools)"
make -C "$SRC" ARCH=arm64 LLVM=1 LLVM_IAS=1 \
  LOCALVERSION="$TARGET_LOCALVERSION" LOCALVERSION_AUTO=n \
  modules_prepare 2>&1 | tee -a "$WORK/build.log" \
  || die "modules_prepare failed; see $WORK/build.log"
[ -f "$SRC/scripts/module.lds" ] \
  || err "WARNING: scripts/module.lds still missing — external module link may fail"

# Re-pass the same LOCALVERSION override so the external module build never
# recomputes a different UTS_RELEASE. The SMP/preempt/mod_unload/modversions
# flags come from the vivo source's vermagic.h + the device .config, so the
# module inherits them automatically. The symbol CRCs baked into the .ko come
# from $SRC/Module.symvers (the device-real table if DEVICE_SYMVERS was set,
# else the GPL vmlinux.symvers — the latter will NOT match the running device).
log "building lkm4ctr against vendor tree"
make -C "$SRC" ARCH=arm64 LLVM=1 LLVM_IAS=1 \
  LOCALVERSION="$TARGET_LOCALVERSION" LOCALVERSION_AUTO=n \
  M="$LKM4CTR_REPO/vendor" LKM4CTR_LOG_DMESG=y \
  -j"$JOBS" modules 2>&1 | tee -a "$WORK/build.log"
log "lkm4ctr build finished"
KO="$LKM4CTR_REPO/vendor/lkm4ctr.ko"
[ -s "$KO" ] || die "lkm4ctr.ko was not produced; see $WORK/build.log"

# ─── 7. verify vermagic + symbol CRCs ───────────────────────────────────────

got_vermagic="$(strings "$KO" | grep -m1 '^vermagic=')"
log "produced vermagic: ${got_vermagic#vermagic=}"
if [ "$got_vermagic" != "vermagic=$EXPECTED_VERMAGIC" ]; then
  err "WARNING: vermagic mismatch"
  err "  expected: $EXPECTED_VERMAGIC"
  err "  got:      ${got_vermagic#vermagic=}"
  err "If only the release token differs, set TARGET_RELEASE correctly."
  err "If flags differ, the device config or vivo source's vermagic.h does not match."
fi

# Verify the baked-in symbol CRCs match what the device's running kernel
# expects. On arm64 each __versions entry is 64 bytes: 8-byte CRC (unsigned
# long) + 56-byte name. modpost (CONFIG_MODVERSIONS) bakes these from
# Module.symvers; if DEVICE_SYMVERS was used they must equal the device table.
ko_ml_crc="$(objdump -s -j __versions "$KO" 2>/dev/null \
  | grep -m1 -A1 'module_l' | head -1 | awk '{print $2}')"
# objdump prints CRC as 8 hex bytes little-endian across 2 words; rebuild u32.
if [ -n "$ko_ml_crc" ]; then
  # '63dd2202 00000000' -> take first word, byte-swap to big-endian u32 hex
  w1="$(printf '%s' "$ko_ml_crc" | awk '{print $1}')"
  ko_ml="0x$(echo "$w1" | sed 's/\(..\)/\1\n/g' | tac | tr -d '\n')"
  log "baked module_layout CRC: $ko_ml"
  if [ -n "${DEVICE_SYMVERS:-}" ]; then
    dev_ml="$(grep -w module_layout "$DEVICE_SYMVERS" | awk '{print $1}')"
    if [ -n "$dev_ml" ]; then
      log "device  module_layout CRC: $dev_ml"
      if [ "$ko_ml" = "$dev_ml" ]; then
        log "OK: baked module_layout CRC matches device — modversions should pass"
      else
        err "WARNING: baked module_layout CRC ($ko_ml) != device ($dev_ml)"
        err "  the .ko will fail to load with 'disagrees about version of symbol module_layout'"
        err "  re-extract the device symvers (scripts/extract-symvers-from-device.sh) and rebuild"
      fi
    fi
  fi
fi

# ─── 8. stage output ──────────────────────────────────────────────────────────
mkdir -p "$OUT_DIR"
# lkm4ctr.ko has no debug deps stripped; keep build-id for traceability.
sha="$(sha256sum "$KO" | cut -c1-12)"
out_ko="$OUT_DIR/lkm4ctr-4.0.5-iqoo-neo9-vendor-arm64-$sha.ko"
cp -f "$KO" "$out_ko"
cp -f "$KO" "$OUT_DIR/lkm4ctr-4.0.5-iqoo-neo9-vendor-arm64.ko"
cp -f "$SRC/Module.symvers" "$OUT_DIR/Module.symvers.iqoo-neo9"
log "DONE"
log "  module:  $out_ko"
log "  symvers: $OUT_DIR/Module.symvers.iqoo-neo9"
log ""
log "On the device, verify before relying on it:"
log "  sudo insmod $out_ko        # should load cleanly now (vermagic + CRCs match)"
log "  sudo dmesg | tail           # look for the lkm4ctr init banner, no panics"
log "  lsmod | grep lkm4ctr"
