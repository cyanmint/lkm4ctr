#!/busybox sh

set -x

lkm4ctr_run_qemu_mode() {
	IMAGE1="$1"
	KERNEL="$2"
	INIT="$3"
	RAMDISK="${4:-}"

	mkdir -p qemu-logs

	if [ "$IMAGE1" = "-l" ]; then
		if [ -z "$RAMDISK" ]; then
			echo "light mode (-l) requires a ramdisk path" >&2
			exit 2
		fi
		echo "=== booting kernel: $KERNEL (LIGHT MODE: no image1.ext4, ramdisk=$RAMDISK, rdinit=$INIT) ==="
		set -- \
			-M virt -cpu max -m 2G -smp 2 -nographic -no-reboot \
			-kernel "$KERNEL" \
			-initrd "$RAMDISK" \
			-append "console=ttyAMA0 rdinit=$INIT earlycon panic=-1"
	elif [ -n "$RAMDISK" ]; then
		echo "=== booting kernel: $KERNEL (ramdisk=$RAMDISK, rdinit=$INIT) ==="
		set -- \
			-M virt -cpu max -m 2G -smp 2 -nographic -no-reboot \
			-kernel "$KERNEL" \
			-initrd "$RAMDISK" \
			-drive file="$IMAGE1",if=none,id=image1,format=raw \
			-device nvme,serial=11451401,drive=image1 \
			-append "console=ttyAMA0 rdinit=$INIT earlycon panic=-1"
	else
		echo "=== booting kernel: $KERNEL (no ramdisk, init=$INIT) ==="
		set -- \
			-M virt -cpu max -m 2G -smp 2 -nographic -no-reboot \
			-kernel "$KERNEL" \
			-drive file="$IMAGE1",if=none,id=image1,format=raw \
			-device nvme,serial=11451401,drive=image1 \
			-append "console=ttyAMA0 root=/dev/nvme0n1 init=$INIT earlycon panic=-1"
	fi

	timeout --signal=KILL 120 qemu-system-aarch64 "$@" 2>&1
	status=$?
	echo "QEMU exited with status $status"
}

lkm4ctr_run_checker_mode() {
	CHECKER="$1"
	MODULE="$2"
	MNT=/lkm4ctr_diagfs
	# generous enough for the global unload worker's own auto-umount +
	# module_refcount()-drain polling (see lkm4ctr_diagfs.c) to finish on a
	# loaded QEMU VM, without hanging the whole boot test indefinitely if
	# it never does.
	SAFE_UNLOAD_TIMEOUT_SEC=30
	echo "=== LKM4CTR_QEMU_TEST: lkm4ctr_checker (pre-insmod) ==="
	"$CHECKER"
	echo "=== LKM4CTR_QEMU_TEST: inserting merged module ==="
	insmod "$MODULE"
	echo "=== LKM4CTR_QEMU_TEST: mounting lkm4ctr diagfs ==="
	mkdir -p "$MNT"
	mount -t lkm4ctr diag "$MNT"
	cat "$MNT/global/resources"
	cat "$MNT/global/log"
	echo "=== LKM4CTR_QEMU_TEST: lkm4ctr_checker (post-insmod) ==="
	"$CHECKER"
	echo "=== LKM4CTR_QEMU_TEST: checker mode DONE ==="
}

lkm4ctr_init_1() {

	mkdir -p /sys /dev /newroot /proc
	/busybox mount -t sysfs sysfs /sys
	/busybox mount -t proc proc /proc
	/busybox mdev -s
	echo "=== LKM4CTR_QEMU_TEST: stage1 (initramfs) ==="

	if test -e /dev/nvme0n1 && mount -t ext4 /dev/nvme0n1 /newroot; then
		echo "=== LKM4CTR_QEMU_TEST: nvme available, proceeding to stage 2 ==="

		cp /lkm4ctr_checker /newroot/
		cp /lkm4ctr.ko /newroot/
		cat /init > /newroot/second_init
		chmod 755 /newroot/second_init

		echo "=== LKM4CTR_QEMU_TEST: exec second init ==="
		# switch_root replaces PID 1 with the given command, run under the new
		# root; /busybox (copied onto image1.ext4 above) provides "env" here
		# since image1.ext4's own /system/bin/env may not exist yet at this
		# point, but /system/bin/sh (the real root's bionic-linked shell,
		# already baked into image1.ext4) is used to interpret /second_init so
		# stage 2 runs under the actual target userland's shell, not busybox's.
		exec switch_root /newroot /busybox env -i /system/bin/sh /second_init
	fi

	echo "=== LKM4CTR_QEMU_TEST: no nvme device, running LIGHT MODE checker directly ==="
	/busybox sh /init -t /lkm4ctr_checker /lkm4ctr.ko
}

lkm4ctr_init_2() {
	# All ramdisk utilities are invoked with absolute paths: bionic's dynamic
	# linker needs /proc/self/exe (or a resolvable argv[0]) to load shared
	# objects, which only holds once /proc is mounted and the path is absolute.

	# /dev is not preserved across switch_root (it was never a separate mount in
	# stage 1, just mdev-populated directory entries on the initramfs, which
	# switch_root discards along with the rest of the old root), so it must be
	# populated again here the same way: mount sysfs, then mdev -s. Re-point
	# stdio at /dev/kmsg immediately afterwards -- PID 1's original fds are
	# otherwise easy to lose track of once /dev is replaced, silently dropping
	# every echo/trace/command-output line for the rest of the script even
	# though it keeps executing. /dev/kmsg writes become kernel printk records,
	# which reliably reaches the QEMU console log.

	# /do-mounts.sh and /do-umounts.sh (sourced below) are baked into
	# image1.ext4's own root (not part of this ramdisk), so they only exist
	# once switch_root has actually landed here; they mount/unmount the
	# standard set of pseudo-filesystems (proc, /dev/pts, cgroups, etc.) that
	# image1.ext4's userland (dockerd/containerd/runc) expects at runtime.
	source /do-mounts.sh
	PATH=/:$PATH

	busybox mdev -s
	ifconfig lo up

	echo "=== LKM4CTR_QEMU_TEST: /lkm4ctr.ko + /lkm4ctr_checker copied onto the new root by stage1 ==="
	chmod 755 /lkm4ctr_checker

	echo "=== LKM4CTR_QEMU_TEST: running shared checker mode ==="
	/system/bin/sh /second_init -t /lkm4ctr_checker /lkm4ctr.ko

	# vendor_kernel's ipc/mqueue.c now registers its POSIX mqueue filesystem
	# type under the real name "mqueue" (see mqueue_fs_type in
	# vendor/ipc/mqueue.c), and vns_ipc_default_init() (via
	# glue/vendor_kernel_ipc_mount.c's vns_mqueue_dev_ensure()) already
	# mounts a real, working /dev/mqueue at module load time (insmod). An unmodified
	# runc/containerd's own mount("mqueue", "/dev/mqueue", "mqueue", ...)
	# during container init now
	# finds and uses that real filesystem directly, so no manual /dev/mqueue
	# premount or --ipc host workaround is needed here any more.

	echo "=== LKM4CTR_QEMU_TEST: starting dockerd (daemon, storage-driver=overlay2) ==="
	dockerd &
	for i in $(seq 1 30); do
		[ -S /var/run/docker.sock ] && break
		sleep 1
	done

	echo "=== LKM4CTR_QEMU_TEST: docker run (test container sanity) ==="
	docker run --privileged --rm --network host --name alpine -i docker.io/arm64v8/alpine:latest init &
	sleep 5
	docker exec -iu 0 alpine ps -e
	docker stop alpine
	docker run --privileged --rm --network host --name ubuntu -i docker.io/arm64v8/ubuntu:latest ps -e
	pkill -2 dockerd
	sleep 15

	echo "=== LKM4CTR_QEMU_TEST: DONE ==="
	# unmounts whatever /do-mounts.sh mounted above, so the following
	# remount,ro is clean.
	source /do-umounts.sh
}

lkm4ctr_init_tail(){
	exec /busybox sh
}

if [ "$$" != "1" ]; then
	# ==================================================================
	# mode 1/2: host-side QEMU launcher, or checker mode ("-t")
	# ==================================================================
	set -u
	if [ "${1:-}" = "-t" ]; then
		if [ "$#" -ne 3 ]; then
			echo "usage: $0 -t <checker path> <module path>" >&2
			exit 2
		else
			lkm4ctr_run_checker_mode "$2" "$3"
			exit 0
		fi
	else
		if [ "$#" -lt 3 ] || [ "$#" -gt 4 ]; then
			echo "usage: bash $0 <image1.ext4 path|-l> <kernel path> <init path> [ramdisk path]" >&2
			exit 2
		else
			lkm4ctr_run_qemu_mode "$@"
			exit 0
		fi
	fi
else
	# ==================================================================
	# mode 3/4: init mode
	# ==================================================================
	case "$(/busybox basename "$0")" in
		init)
			lkm4ctr_init_1
			;;
		*)
			lkm4ctr_init_2
			;;
	esac
	lkm4ctr_init_tail
fi
