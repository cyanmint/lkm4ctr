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

# vermagic flags the running kernel expects (after the release token):
#   SMP preempt mod_unload modversions vivo aarch64
EXPECTED_VERMAGIC="$TARGET_RELEASE SMP preempt mod_unload modversions vivo aarch64"

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

# ─── 5. build vmlinux → real Module.symvers (core symbol CRCs) ───────────────
# LOCALVERSION=-gaacdc35637c4-dirty + LOCALVERSION_AUTO=n forces the exact
# UTS_RELEASE the running kernel reports, without relying on the source tree's
# git state. CONFIG_LOCALVERSION_AUTO=y in the device config is overridden here.
#
# We build ONLY vmlinux, not the in-tree `modules` target. Module.symvers is
# produced by the vmlinux build and carries every vmlinux-exported (core)
# symbol's CRC — which is all lkm4ctr needs: its non-exported kernel functions
# are resolved at runtime by name via shadow_hook_resolve()/kallsyms, not via
# CRC. Building vmlinux alone skips compiling vivo's large pile of DLKMs
# (camera/audio/modem/etc.), cutting build time drastically while leaving the
# CRC table complete for the symbols that actually matter.
log "building vmlinux (LOCALVERSION=$TARGET_LOCALVERSION -> UTS_RELEASE=$TARGET_RELEASE, LTO+CFI per config)"
# Live output (no tail buffering) so a long LTO+CFI link doesn't look stuck.
make -C "$SRC" ARCH=arm64 LLVM=1 LLVM_IAS=1 \
  LOCALVERSION="$TARGET_LOCALVERSION" LOCALVERSION_AUTO=n \
  -j"$JOBS" vmlinux 2>&1 | tee "$WORK/build.log"
log "vmlinux build finished"

[ -s "$SRC/Module.symvers" ] || die "Module.symvers missing/empty — vmlinux build failed; see $WORK/build.log"
sym_count="$(wc -l < "$SRC/Module.symvers")"
log "Module.symvers: $sym_count entries"
[ "$sym_count" -gt 100 ] || err "WARNING: Module.symvers has only $sym_count entries — CRCs may be incomplete"

# ─── 6. build lkm4ctr against the vendor tree ─────────────────────────────────
# Re-pass the same LOCALVERSION override so the external module build never
# recomputes a different UTS_RELEASE. The `vivo` vermagic token and the
# SMP/preempt/mod_unload/modversions flags come from the vivo source's vermagic.h
# + the device .config, so the module inherits them automatically.
log "building lkm4ctr against vendor tree"
make -C "$SRC" ARCH=arm64 LLVM=1 LLVM_IAS=1 \
  LOCALVERSION="$TARGET_LOCALVERSION" LOCALVERSION_AUTO=n \
  M="$LKM4CTR_REPO/vendor" LKM4CTR_LOG_DMESG=y \
  -j"$JOBS" modules 2>&1 | tee -a "$WORK/build.log"
log "lkm4ctr build finished"
KO="$LKM4CTR_REPO/vendor/lkm4ctr.ko"
[ -s "$KO" ] || die "lkm4ctr.ko was not produced; see $WORK/build.log"

# ─── 7. verify vermagic ───────────────────────────────────────────────────────
got_vermagic="$(strings "$KO" | grep -m1 '^vermagic=')"
log "produced vermagic: ${got_vermagic#vermagic=}"
if [ "$got_vermagic" != "vermagic=$EXPECTED_VERMAGIC" ]; then
  err "WARNING: vermagic mismatch"
  err "  expected: $EXPECTED_VERMAGIC"
  err "  got:      ${got_vermagic#vermagic=}"
  err "If only the release token differs, set TARGET_RELEASE correctly."
  err "If flags differ, the device config or vivo source's vermagic.h does not match."
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
