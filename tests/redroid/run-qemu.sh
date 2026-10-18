#!/usr/bin/env bash
# Public Domain (AI-generated; no copyright subsists in this content).

set -euo pipefail

if [ "$#" -ne 7 ]; then
	echo "Usage: $0 <kernel> <disk-a.ext4> <disk-b.ext4> <busybox> <output-dir> <lkm4ctr.ko> <lkm4ctr_checker>" >&2
	exit 2
fi

kernel=$1
disk_a=$2
disk_b=$3
busybox=$4
output_dir=$5
module=$6
checker=$7
ramdisk_dir=$(mktemp -d)
mount_dir=$(mktemp -d)
trap 'rm -rf "$ramdisk_dir"; sudo umount "$mount_dir" 2>/dev/null || true; rmdir "$mount_dir"' EXIT

mkdir -p "$output_dir/disk-b-contents"
mkdir -p "$ramdisk_dir/dev" "$ramdisk_dir/proc" "$ramdisk_dir/sys"
install -m 0755 "$busybox" "$ramdisk_dir/busybox"
install -m 0755 tests/redroid/init "$ramdisk_dir/init"
install -m 0644 "$module" "$ramdisk_dir/lkm4ctr.ko"
install -m 0755 "$checker" "$ramdisk_dir/lkm4ctr_checker"
(cd "$ramdisk_dir" && find . | cpio -o -H newc > "$output_dir/redroid-initramfs.cpio")

set +e
timeout --signal=KILL 1200 qemu-system-aarch64 \
	-M virt -cpu max -m 4G -smp 4 -nographic -no-reboot \
	-kernel "$kernel" \
	-initrd "$output_dir/redroid-initramfs.cpio" \
	-device qemu-xhci,id=usb0 \
	-drive file="$disk_a",if=none,format=raw,id=disk_a \
	-device usb-storage,bus=usb0.0,drive=disk_a,id=redroid-rootfs \
	-drive file="$disk_b",if=none,format=raw,id=disk_b \
	-device usb-storage,bus=usb0.0,drive=disk_b,id=test-results \
	-append 'console=ttyAMA0 rdinit=/init earlycon panic=-1 androidboot.hardware=redroid androidboot.redroid_width=720 androidboot.redroid_height=1280 androidboot.redroid_dpi=240 androidboot.selinux=permissive' \
	2>&1 | tee "$output_dir/redroid-qemu.log"
qemu_status=${PIPESTATUS[0]}
set -e
echo "QEMU_EXIT_STATUS=$qemu_status"

sudo mount -o loop,ro,noload "$disk_b" "$mount_dir"
cp -a "$mount_dir/." "$output_dir/disk-b-contents/"
test_log="$mount_dir/lkm4ctr-test.log"
if [ -f "$test_log" ]; then
	cat "$test_log"
else
	echo "REDROID_TEST_FAILED: Disk B has no lkm4ctr-test.log" >&2
	exit 1
fi

if [ "$qemu_status" -ne 0 ]; then
	echo "REDROID_TEST_FAILED: QEMU exited with status $qemu_status" >&2
	exit 1
fi
if ! grep -Fq 'REDROID_TEST_RESULT=PASS' "$test_log"; then
	echo "REDROID_TEST_FAILED: Android test service did not report PASS" >&2
	exit 1
fi
if ! grep -Fq 'DISK_B_UMOUNT_STATUS=PASS' "$test_log"; then
	echo "REDROID_TEST_FAILED: Android test service did not unmount Disk B" >&2
	exit 1
fi
