#!/busybox sh
# Public Domain (AI-generated; no copyright subsists in this content).

set -eu
busybox=${2:-${BUSYBOX:-/busybox}}

if [ "$#" -lt 1 ] || [ "$#" -gt 3 ]; then
	echo "Usage: $0 <dev-directory> [busybox-path] [--scsi]" >&2
	exit 2
fi
dev_root=$1
if [ "$#" -eq 3 ] && [ "$3" != --scsi ]; then
	echo "Usage: $0 <dev-directory> [busybox-path] [--scsi]" >&2
	exit 2
fi

fail() {
    message="REDROID_DEVICE_NODES_FAILED: $*"
    if [ -w /dev/kmsg ]; then
        echo "<3>$message" > /dev/kmsg
    fi
    echo "$message"
    exit 1
}

ensure_node() {
    path=$1
    type=$2
    major=$3
    minor=$4
    expected=$($busybox printf '%x:%x' "$major" "$minor") ||
        fail "could not format device number for $path"

    if { [ "$type" = c ] && [ -c "$path" ]; } ||
       { [ "$type" = b ] && [ -b "$path" ]; }; then
        actual=$($busybox stat -c '%t:%T' "$path") ||
            fail "could not inspect device node $path"
        if [ "$actual" = "$expected" ]; then
            return
        fi
    fi
    if [ -e "$path" ] || [ -L "$path" ]; then
        $busybox rm -f "$path" || fail "could not replace incorrect node $path"
    fi
    if [ -e "$path" ] || [ -L "$path" ]; then
        fail "unexpected existing node at $path"
    fi
    $busybox mknod -m 0600 "$path" "$type" "$major" "$minor" ||
        fail "could not create $type node $path"
}

$busybox mkdir -p "$dev_root/block"
ensure_node "$dev_root/loop-control" c 10 237
ensure_node "$dev_root/device-mapper" c 10 236
if [ "${3:-}" = --scsi ]; then
    ensure_node "$dev_root/sda" b 8 0
    ensure_node "$dev_root/sdb" b 8 16
    ensure_node "$dev_root/block/sda" b 8 0
    ensure_node "$dev_root/block/sdb" b 8 16
fi

loop_number=0
while [ "$loop_number" -lt 64 ]; do
    ensure_node "$dev_root/block/loop$loop_number" b 7 "$loop_number"
    loop_number=$((loop_number + 1))
done

if [ -w /dev/kmsg ]; then
    echo '<6>REDROID_DEVICE_NODES_READY' > /dev/kmsg
fi
echo "REDROID_DEVICE_NODES_READY"
