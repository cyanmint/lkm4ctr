#!/usr/bin/env bash
# Public Domain (AI-generated; no copyright subsists in this content).

set -euo pipefail

if [ "$#" -ne 1 ]; then
	echo "Usage: $0 <output.ext4.zst>" >&2
	exit 2
fi

output=$1
rootfs=$(mktemp -d)
container="lkm4ctr-redroid-rootfs-${GITHUB_RUN_ID:-local}-${RANDOM}"
disk="${output%.zst}"
cleanup() {
	docker rm "$container" >/dev/null 2>&1 || true
	sudo rm -rf "$rootfs"
	rm -f "$disk"
}
trap cleanup EXIT

docker pull --platform linux/arm64 docker.io/redroid/redroid:latest
docker create --platform linux/arm64 --name "$container" \
	docker.io/redroid/redroid:latest >/dev/null
docker export "$container" |
	sudo tar --numeric-owner --same-owner -xpf - -C "$rootfs"
docker rm "$container" >/dev/null

if [ -L "$rootfs/init" ]; then
	init_target=$(readlink "$rootfs/init")
	case "$init_target" in
		/*) init_target="$rootfs$init_target" ;;
		*) init_target="$rootfs/$init_target" ;;
	esac
else
	init_target="$rootfs/init"
fi
if [ ! -x "$init_target" ]; then
	echo "Redroid /init target is not executable within the extracted rootfs: $init_target" >&2
	exit 1
fi
if [ ! -x "$rootfs/system/bin/sh" ]; then
	echo "Redroid /system/bin/sh is missing or not executable" >&2
	exit 1
fi
sudo install -D -o 0 -g 0 -m 0755 \
	tests/redroid/lkm4ctr-test.sh "$rootfs/system/bin/lkm4ctr-test.sh"
sudo install -D -o 0 -g 0 -m 0644 \
	tests/redroid/lkm4ctr-test.rc "$rootfs/system/etc/init/lkm4ctr-test.rc"
sudo install -D -o 0 -g 0 -m 0755 \
	tests/redroid/lkm4ctr-device-nodes.sh "$rootfs/system/bin/lkm4ctr-device-nodes.sh"
sudo bash tests/redroid/install-device-node-hook.sh "$rootfs"

mkdir -p "$(dirname "$output")"
truncate -s 8G "$disk"
sudo mkfs.ext4 -F -m 0 -L REDROID_ROOTFS -d "$rootfs" "$disk"
zstd -T0 -3 --force "$disk" -o "$output"
if [ "$(stat -c %s "$output")" -ge 2000000000 ]; then
	echo "Compressed Disk A exceeds GitHub's 2 GB release-asset limit" >&2
	exit 1
fi
ls -lh "$output"
