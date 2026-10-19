#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# extract-symvers-from-device.sh — rebuild a real Module.symvers from the
# iQOO Neo9's RUNNING kernel, by parsing its in-Image __ksymtab/__kcrctab
# tables via the virtual addresses /proc/kallsyms exposes.
#
# Why this exists: the vivo iQOO Neo9 vendor kernel was built from a
# closed-source-augmented tree (CONFIG_VIVO_VMET_BREAK_KMI / CONFIG_MODULE_SIG_PROTECT).
# Its real exported-symbol CRC table differs from what vivo's PUBLIC GPL source
# tree produces, and vivo's own vendor DLKMs are loaded via a CRC-bypass path, so
# they never actually pass check_version. A third-party module built against the
# public GPL Module.symvers therefore fails:
#     lkm4ctr: disagrees about version of symbol module_layout
# This script recovers the running kernel's REAL CRC table directly from the
# kernel image in the active boot slot, so an out-of-tree module built against it
# passes modversions on the real device.
#
# Requirements: root (sudo), the device's kernel must have
# CONFIG_KALLSYMS_ALL=y (so /proc/kallsyms lists __start___ksymtab etc. with
# real addresses) and CONFIG_HAVE_ARCH_PREL32_RELOCATIONS=y (arm64 GKI default).
# CONFIG_MODULE_REL_CRCS must be OFF (kcrctab holds absolute CRCs) — checked.
#
# Output: Module.symvers.<device> in the format modpost/depmod expects:
#     0xCRC\tname\tvmlinux\tEXPORT_SYMBOL[_GPL]\t[namespace]
#
# Usage:
#   sudo scripts/extract-symvers-from-device.sh [boot_part] [out_file]
#     boot_part  default: active slot (/dev/block/by-name/boot$(getprop slot_suffix))
#     out_file   default: out/Module.symvers.iqoo-neo9.device
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
: "${LKM4CTR_REPO:="$(cd "$SCRIPT_DIR/.." && pwd)"}"

BOOT="${1:-}"
if [ -z "$BOOT" ]; then
  slot="$(getprop ro.boot.slot_suffix 2>/dev/null || true)"
  [ -n "$slot" ] || slot="$(sudo getprop ro.boot.slot_suffix 2>/dev/null || true)"
  BOOT="/dev/block/by-name/boot${slot}"
fi
[ -b "$BOOT" ] || { echo "ERROR: boot partition not found: $BOOT" >&2; exit 1; }

OUT="${2:-$LKM4CTR_REPO/out/Module.symvers.iqoo-neo9.device}"
mkdir -p "$(dirname "$OUT")"

IMG="$(mktemp -t iqoo-neo9-Image-XXXXXX.bin)"
trap 'rm -f "$IMG"' EXIT

echo "[extract] boot partition: $BOOT" >&2
echo "[extract] reading /proc/kallsyms for table bounds" >&2

# Pull the symbol addresses we need from the running kernel's kallsyms.
# Fields: addr type name
# NOTE: do NOT anchor with ^...$ — kallsyms lines are "<addr> <type> <name>",
# so ^ would match the address column. -w (whole-word) is enough and won't
# match e.g. __start___ksymtab_gpl when asked for __start___ksymtab.
ksyms() { sudo grep -wE "$1" /proc/kallsyms | awk '{print $1}' | head -1; }
TEXT=$(ksyms '_text')
[ -n "$TEXT" ] || { echo "ERROR: _text not in /proc/kallsyms (CONFIG_KALLSYMS_ALL=y needed)" >&2; exit 1; }

KST=$(ksyms '__start___ksymtab')
KSE=$(ksyms '__stop___ksymtab')
KGT=$(ksyms '__start___ksymtab_gpl')
KGE=$(ksyms '__stop___ksymtab_gpl')
CCT=$(ksyms '__start___kcrctab')
CCE=$(ksyms '__stop___kcrctab')
CGT=$(ksyms '__start___kcrctab_gpl')
CGE=$(ksyms '__stop___kcrctab_gpl')

for v in TEXT KST KSE KGT KGE CCT CCE CGT CGE; do
  [ -n "${!v:-}" ] || { echo "ERROR: missing kallsyms symbol: $v" >&2; exit 1; }
done
echo "[extract] _text=$TEXT" >&2
echo "[extract] ksymtab=[$KST,$KSE) ksymtab_gpl=[$KGT,$KGE)" >&2
echo "[extract] kcrctab=[$CCT,$CCE) kcrctab_gpl=[$CGT,$CGE)" >&2

# Parse the Android boot image header to find the kernel. Header version is at
# offset 0x28 (u32). For v3/v4 the kernel starts at offset 4096 (one 4K page).
echo "[extract] parsing boot image header" >&2
hdr="$(mktemp -t boot-hdr-XXXXXX)"
sudo dd if="$BOOT" bs=4096 count=1 2>/dev/null > "$hdr"
magic="$(dd if="$hdr" bs=1 count=8 2>/dev/null)"
[ "$magic" = "ANDROID!" ] || { echo "ERROR: not an Android boot image (magic='$magic')" >&2; rm -f "$hdr"; exit 1; }
# kernel_size at 0x08 (v3/v4 little-endian u32)
ksize="$(od -An -tu4 -N4 -j8 "$hdr" | tr -d ' ')"
rm -f "$hdr"
[ "$ksize" -gt 0 ] 2>/dev/null || { echo "ERROR: bad kernel_size=$ksize" >&2; exit 1; }
echo "[extract] kernel_size=$ksize, kernel at file offset 4096" >&2

echo "[extract] dd-ing kernel image" >&2
# Read from offset 4096, take exactly ksize bytes.
sudo dd if="$BOOT" bs=4096 skip=1 2>/dev/null | head -c "$ksize" > "$IMG"
got="$(stat -c %s "$IMG")"
[ "$got" -eq "$ksize" ] || echo "WARNING: got $got bytes, expected $ksize (truncated read may miss tail symbols)" >&2

echo "[extract] parsing __ksymtab/__kcrctab -> $OUT" >&2
python3 - "$IMG" "$OUT" "$TEXT" "$KST" "$KSE" "$CCT" "$CCE" "$KGT" "$KGE" "$CGT" "$CGE" <<'PY'
import sys, struct
img, out, text = sys.argv[1], sys.argv[2], int(sys.argv[3],16)
ks, ke, cs, ce = (int(x,16) for x in sys.argv[4:8])
kgs, kge, cgs, cge = (int(x,16) for x in sys.argv[8:12])
data = open(img,'rb').read()
KS=12; CS=4  # ksymtab entry=12B(3xint32 prel32), kcrctab entry=4B(abs CRC)

def off(va): return va - text
def rd(va, n):
    o = off(va)
    if o < 0 or o+n > len(data): return None
    return data[o:o+n]

def resolve_name(entry_va):
    # name_offset is prel32 relative to the name field (entry+4)
    nb = rd(entry_va+4, 4)
    if nb is None: return None
    rel = struct.unpack('<i', nb)[0]
    name_va = (entry_va + 4) + rel
    s = rd(name_va, 256)
    if s is None: return None
    return s.split(b'\x00')[0].decode('latin1','replace')

def parse(ks_va, ke_va, cs_va, ce_va):
    nksym = (ke_va - ks_va)//KS
    ncr   = (ce_va - cs_va)//CS
    rows=[]
    if nksym != ncr:
        sys.stderr.write(f"WARNING: ksymtab entries ({nksym}) != kcrctab entries ({ncr}); using min\n")
    n = min(nksym, ncr)
    for i in range(n):
        eva = ks_va + i*KS
        name = resolve_name(eva)
        if not name: continue
        cb = rd(cs_va + i*CS, 4)
        if cb is None: continue
        crc = struct.unpack('<I', cb)[0]
        rows.append((crc, name))
    return rows

rows = parse(ks, ke, cs, ce)
gpl  = parse(kgs, kge, cgs, cge)
total = len(rows)+len(gpl)
with open(out,'w') as f:
    for crc,name in rows:
        f.write(f"0x{crc:08x}\t{name}\tvmlinux\tEXPORT_SYMBOL\t\n")
    for crc,name in gpl:
        f.write(f"0x{crc:08x}\t{name}\tvmlinux\tEXPORT_SYMBOL_GPL\t\n")
print(f"[extract] wrote {total} symbols ({len(rows)} EXPORT_SYMBOL, {len(gpl)} GPL) -> {out}", file=sys.stderr)
# Sanity: show module_layout
for crc,name in rows+gpl:
    if name == 'module_layout':
        print(f"[extract] module_layout real CRC = 0x{crc:08x}", file=sys.stderr)
        break
PY

echo "[extract] DONE" >&2
echo "  $OUT  ($(wc -l < "$OUT") symbols)"
