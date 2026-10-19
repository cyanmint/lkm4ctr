#!/usr/bin/env bash
# Public Domain (AI-generated; no copyright subsists in this content).

set -euo pipefail

if [ "$#" -ne 1 ]; then
	echo "Usage: $0 <mounted-rootfs>" >&2
	exit 2
fi

rootfs=$1
if [ ! -d "$rootfs" ] || [ ! -d "$rootfs/dev" ] ||
	[ ! -x "$rootfs/system/bin/sh" ] ||
	[ ! -d "$rootfs/system/etc/init" ]; then
	echo "Redroid rootfs is missing /dev or Android init files: $rootfs" >&2
	exit 1
fi

sudo install -D -o 0 -g 0 -m 0644 \
	tests/redroid/fstab.redroid "$rootfs/fstab.redroid"
sudo install -D -o 0 -g 0 -m 0755 \
	tests/redroid/lkm4ctr-test.sh "$rootfs/system/bin/lkm4ctr-test.sh"
sudo install -D -o 0 -g 0 -m 0644 \
	tests/redroid/lkm4ctr-test.rc "$rootfs/system/etc/init/lkm4ctr-test.rc"
sudo install -d -o 0 -g 0 -m 0755 "$rootfs/mnt/lkm4ctr-fstab"
sudo rm -rf "$rootfs/dev/__properties__"
