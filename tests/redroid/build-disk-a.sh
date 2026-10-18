#!/usr/bin/env bash
# Public Domain (AI-generated; no copyright subsists in this content).

set -euo pipefail

if [ "$#" -ne 3 ]; then
	echo "Usage: $0 <lkm4ctr.ko> <lkm4ctr_checker> <output.ext4.zst>" >&2
	exit 2
fi

module=$1
checker=$2
output=$3
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

test -x "$rootfs/init"
test -x "$rootfs/system/bin/sh"
sudo install -D -o 0 -g 0 -m 0755 \
	tests/redroid/lkm4ctr-test.sh "$rootfs/system/bin/lkm4ctr-test.sh"
sudo install -D -o 0 -g 0 -m 0644 \
	tests/redroid/lkm4ctr-test.rc "$rootfs/system/etc/init/lkm4ctr-test.rc"
sudo install -D -o 0 -g 0 -m 0644 "$module" "$rootfs/system/bin/lkm4ctr.ko"
sudo install -D -o 0 -g 0 -m 0755 "$checker" \
	"$rootfs/system/bin/lkm4ctr_checker"

mkdir -p "$(dirname "$output")"
truncate -s 8G "$disk"
sudo mkfs.ext4 -F -m 0 -L REDROID_ROOTFS -d "$rootfs" "$disk"
zstd -T0 -3 --force "$disk" -o "$output"
ls -lh "$output"
