#!/system/bin/sh
# Public Domain (AI-generated; no copyright subsists in this content).

set -eu

ensure_character_device() {
	path=$1
	major=$2
	minor=$3
	if [ ! -c "$path" ]; then
		if [ -e "$path" ]; then
			echo "REDROID_TEST_FAILED: unexpected existing node at $path" >&2
			exit 1
		fi
		/system/bin/toybox mknod -m 0600 "$path" c "$major" "$minor"
	fi
	if [ ! -c "$path" ]; then
		echo "REDROID_TEST_FAILED: failed to create character device $path" >&2
		exit 1
	fi
}

ensure_block_device() {
	path=$1
	minor=$2
	if [ ! -b "$path" ]; then
		if [ -e "$path" ]; then
			echo "REDROID_TEST_FAILED: unexpected existing node at $path" >&2
			exit 1
		fi
		/system/bin/toybox mknod -m 0600 "$path" b 7 "$minor"
	fi
	if [ ! -b "$path" ]; then
		echo "REDROID_TEST_FAILED: failed to create block device $path" >&2
		exit 1
	fi
}

mkdir -p /dev/block
ensure_character_device /dev/loop-control 10 237
ensure_character_device /dev/device-mapper 10 236
loop_number=0
while [ "$loop_number" -lt 64 ]; do
	ensure_block_device "/dev/block/loop$loop_number" "$loop_number"
	loop_number=$((loop_number + 1))
done

echo '<6>REDROID_DEVICE_NODES_READY' > /dev/kmsg
