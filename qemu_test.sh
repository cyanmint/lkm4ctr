#!/busybox sh

set -x

lkm4ctr_run_qemu_mode() {
	IMAGE1="$1"
	KERNEL="$2"
	INIT="$3"
	RAMDISK="${4:-}"
	case "${LKM4CTR_TEST_MODULE:-auto}" in
		1) MODULE_ARG=" lkm4ctr_test_module=1" ;;
		0) MODULE_ARG=" lkm4ctr_test_module=0" ;;
		*)
			case "${KERNEL##*/}" in
				*.patched|*.official) MODULE_ARG=" lkm4ctr_test_module=1" ;;
				*) MODULE_ARG=" lkm4ctr_test_module=0" ;;
			esac
			;;
	esac
	RMMOD_ARG=""
	if [ "${LKM4CTR_TEST_RMMOD:-0}" = "1" ]; then
		RMMOD_ARG=" lkm4ctr_test_rmmod=1"
	fi

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
			-append "console=ttyAMA0 rdinit=$INIT earlycon panic=-1$MODULE_ARG$RMMOD_ARG"
	elif [ -n "$RAMDISK" ]; then
		echo "=== booting kernel: $KERNEL (ramdisk=$RAMDISK, rdinit=$INIT) ==="
		set -- \
			-M virt -cpu max -m 2G -smp 2 -nographic -no-reboot \
			-kernel "$KERNEL" \
			-initrd "$RAMDISK" \
			-device qemu-xhci,id=usb0 \
			-drive file="$IMAGE1",if=none,format=raw,id=usb1 \
			-device usb-storage,bus=usb0.0,drive=usb1,id=disk1 \
			-append "console=ttyAMA0 rdinit=$INIT earlycon panic=-1$MODULE_ARG$RMMOD_ARG"
	else
		echo "=== booting kernel: $KERNEL (no ramdisk, init=$INIT) ==="
		set -- \
			-M virt -cpu max -m 2G -smp 2 -nographic -no-reboot \
			-kernel "$KERNEL" \
			-drive file="$IMAGE1",if=none,id=image1,format=raw \
			-device nvme,serial=11451401,drive=image1 \
			-append "console=ttyAMA0 root=/dev/nvme0n1 init=$INIT earlycon panic=-1$MODULE_ARG$RMMOD_ARG"
	fi

	timeout --signal=KILL 120 qemu-system-aarch64 "$@" 2>&1
	status=$?
	echo "QEMU exited with status $status"
}

lkm4ctr_module_test_enabled() {
	module_cmdline="$(cat /proc/cmdline 2>/dev/null)" || return 2
	case " $module_cmdline " in
		*" lkm4ctr_test_module=1 "*) return 0 ;;
		*" lkm4ctr_test_module=0 "*) return 1 ;;
	esac
	return 2
}

lkm4ctr_rmmod_test_enabled() {
	rmmod_test_cmdline="$(cat /proc/cmdline 2>/dev/null)" || return 2
	case " $rmmod_test_cmdline " in
		*" lkm4ctr_test_rmmod=1 "*) return 0 ;;
	esac
	return 1
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
	if [ "$MODULE" = "-" ]; then
		echo "=== LKM4CTR_QEMU_TEST: checker (module test intentionally disabled) ==="
		"$CHECKER" || true
		echo "=== LKM4CTR_QEMU_TEST: checker mode DONE (no module) ==="
		return 0
	fi

	echo "=== LKM4CTR_QEMU_TEST: lkm4ctr_checker (pre-insmod) ==="
	"$CHECKER" || true
	echo "=== LKM4CTR_QEMU_TEST: inserting merged module ==="
	if ! insmod "$MODULE"; then
		echo "lkm4ctr.ko failed to load"
		return 1
	fi
	echo "=== LKM4CTR_QEMU_TEST: mounting lkm4ctr diagfs ==="
	mkdir -p "$MNT"
	if ! mount -t lkm4ctr diag "$MNT"; then
		echo "failed to mount lkm4ctr diagfs"
		return 1
	fi
	cat "$MNT/v/resources" || return 1
	cat "$MNT/log" || return 1
	echo "=== LKM4CTR_QEMU_TEST: lkm4ctr_checker (post-insmod) ==="
	"$CHECKER"
	checker_status=$?
	if [ "$checker_status" -ne 0 ]; then
		echo "post-insmod checker reported failures"
		return "$checker_status"
	fi
	resource_wait=0
	while [ "$resource_wait" -lt 10 ]; do
		ipcns_live=$(awk '$1 == "ipcns_live:" { print $2 }' "$MNT/v/resources")
		ipcns_pending=$(awk '$1 == "ipcns_free_work_pending:" { print $2 }' "$MNT/v/resources")
		userns_live=$(awk '$1 == "userns_live:" { print $2 }' "$MNT/v/resources")
		userns_pending=$(awk '$1 == "userns_pending_work:" { print $2 }' "$MNT/v/resources")
		semundo_live=$(awk '$1 == "semundo_live:" { print $2 }' "$MNT/v/resources")
		sem_work_pending=$(awk '$1 == "sem_exit_work_pending:" { print $2 }' "$MNT/v/resources")
		for resource_value in "$ipcns_live" "$ipcns_pending" \
			"$userns_live" "$userns_pending" "$semundo_live" \
			"$sem_work_pending"; do
			case "$resource_value" in
				''|*[!0-9]*)
					echo "QEMU_TEST_FAILED: namespace resource diagnostics are incomplete"
					return 1
					;;
			esac
		done
		if [ "$ipcns_live" -eq 0 ] && [ "$ipcns_pending" -eq 0 ] &&
		   [ "$userns_live" -eq 0 ] && [ "$userns_pending" -eq 0 ] &&
		   [ "$semundo_live" -eq 0 ] && [ "$sem_work_pending" -eq 0 ]; then
			break
		fi
		sleep 1
		resource_wait=$((resource_wait + 1))
	done
	if [ "${ipcns_live:-missing}" != "0" ] ||
	   [ "${ipcns_pending:-missing}" != "0" ] ||
	   [ "${userns_live:-missing}" != "0" ] ||
	   [ "${userns_pending:-missing}" != "0" ] ||
	   [ "${semundo_live:-missing}" != "0" ] ||
	   [ "${sem_work_pending:-missing}" != "0" ]; then
		echo "QEMU_TEST_FAILED: namespace teardown work did not drain after checker exit"
		return 1
	fi
	echo "=== LKM4CTR_QEMU_TEST: checker mode DONE ==="
}

lkm4ctr_init_1() {
	# wait for usb /sda to insert
	sleep 4
	mkdir -p /sys /dev /newroot /proc
	/busybox mount -t sysfs sysfs /sys
	/busybox mount -t proc proc /proc
	lkm4ctr_module_test_enabled
	module_test_status=$?
	if [ "$module_test_status" -gt 1 ]; then
		echo "missing or unreadable lkm4ctr_test_module boot argument"
		return 1
	fi
	lkm4ctr_rmmod_test_enabled
	rmmod_test_status=$?
	if [ "$rmmod_test_status" -gt 1 ]; then
		echo "missing or unreadable lkm4ctr_test_rmmod boot argument"
		return 1
	fi
	if [ "$rmmod_test_status" -eq 0 ]; then
		echo "=== LKM4CTR_QEMU_TEST: direct insmod/rmmod safety test ==="
		if [ "$module_test_status" -ne 0 ] || [ ! -s /lkm4ctr.ko ]; then
			echo "RMMOD_TEST_FAILED: matching module artifact is missing"
			/busybox poweroff -f
			return 1
		fi
		native_mqueue_preexisting=$(/busybox awk '$NF == "mqueue" { found = 1 } END { print found + 0 }' /proc/filesystems)
		native_ipc_nsfs=0
		if /busybox unshare -i /busybox sh -c ':' >/dev/null 2>&1; then
			native_ipc_nsfs=1
		fi
		echo "=== LKM4CTR_QEMU_TEST: pre-insmod native mqueue=$native_mqueue_preexisting native IPC namespace operation=$native_ipc_nsfs ==="
		if ! /busybox insmod /lkm4ctr.ko; then
			echo "RMMOD_TEST_FAILED: insmod lkm4ctr.ko"
			/busybox poweroff -f
			return 1
		fi
		mount_ready=0
		mount_wait=0
		while [ "$mount_wait" -lt 10 ]; do
			if /busybox grep -q ' /dev/mqueue ' /proc/self/mountinfo; then
				mount_ready=1
				break
			fi
			/busybox sleep 1
			mount_wait=$((mount_wait + 1))
		done
		if [ "$mount_ready" -ne 1 ]; then
			# The proactive kernel-side mount is best-effort: some GKI builds
			# trim the VFS helpers it needs. Exercise the documented userspace
			# mount(2) fallback instead of treating that limitation as failure.
			if ! /busybox mkdir -p /dev/mqueue ||
			   ! /busybox mount -t mqueue mqueue /dev/mqueue; then
				echo "RMMOD_TEST_FAILED: proactive mount and reactive mqueue mount(2) fallback both failed"
				/busybox poweroff -f
				return 1
			fi
			if ! /busybox grep -q ' /dev/mqueue ' /proc/self/mountinfo; then
				echo "RMMOD_TEST_FAILED: reactive mount(2) did not mount /dev/mqueue"
				/busybox poweroff -f
				return 1
			fi
			echo "=== LKM4CTR_QEMU_TEST: proactive mount unavailable; reactive /dev/mqueue mount(2) fallback verified ==="
		fi
		if ! /busybox umount /dev/mqueue; then
			echo "RMMOD_TEST_FAILED: could not unmount the proactive /dev/mqueue mount before isolating the test mount"
			/busybox poweroff -f
			return 1
		fi
		/busybox mkdir -p /mnt/lkm4ctr-mqueue
		if ! /busybox mount -t mqueue mqueue /mnt/lkm4ctr-mqueue; then
			echo "RMMOD_TEST_FAILED: could not mount the user-facing mqueue type"
			/busybox poweroff -f
			return 1
		fi
		# Let the proactive mount worker drop its temporary module reference.
		/busybox sleep 1
		module_refs=$(/busybox awk '$1 == "lkm4ctr" { print $3 }' /proc/modules)
		if [ -z "$module_refs" ]; then
			echo "RMMOD_TEST_FAILED: lkm4ctr is missing from /proc/modules"
			/busybox poweroff -f
			return 1
		fi
		case "$module_refs" in
			*[!0-9]*)
				echo "RMMOD_TEST_FAILED: invalid lkm4ctr refcount: $module_refs"
				/busybox poweroff -f
				return 1
				;;
		esac
		echo "=== LKM4CTR_QEMU_TEST: module refcount with mqueue mounts: $module_refs ==="
		/busybox mkdir -p /mnt/lkm4ctr-mqueue-diag
		if ! /busybox mount -t lkm4ctr diag /mnt/lkm4ctr-mqueue-diag; then
			echo "RMMOD_TEST_FAILED: could not mount diagfs to identify mqueue filesystem ownership"
			/busybox poweroff -f
			return 1
		fi
		mqueue_public_registered=$(/busybox awk '$1 == "mqueue_public_fs_registered:" { print $2 }' /mnt/lkm4ctr-mqueue-diag/v/resources)
		if ! /busybox umount /mnt/lkm4ctr-mqueue-diag; then
			echo "RMMOD_TEST_FAILED: could not unmount mqueue ownership diagfs"
			/busybox poweroff -f
			return 1
		fi
		case "$mqueue_public_registered" in
			0|1) ;;
			*)
				echo "RMMOD_TEST_FAILED: invalid public mqueue registration state: ${mqueue_public_registered:-missing}"
				/busybox poweroff -f
				return 1
				;;
		esac
		if [ "$mqueue_public_registered" -eq 1 ]; then
			if [ "$module_refs" -eq 0 ]; then
				echo "RMMOD_TEST_FAILED: module-owned public mqueue mount did not pin lkm4ctr"
				/busybox poweroff -f
				return 1
			fi
			# BusyBox rmmod may return zero even when delete_module() reports
			# EBUSY; verify the kernel's module table, not the applet status.
			/busybox rmmod lkm4ctr
			if ! /busybox grep -q '^lkm4ctr ' /proc/modules; then
				echo "RMMOD_TEST_FAILED: module disappeared despite active mqueue mounts"
				/busybox poweroff -f
				return 1
			fi
			echo "=== LKM4CTR_QEMU_TEST: rmmod safely refused while mqueue refs are active ==="
			/busybox mkdir -p /mnt/lkm4ctr-diagfs
			if ! /busybox mount -t lkm4ctr diag /mnt/lkm4ctr-diagfs; then
				echo "RMMOD_TEST_FAILED: could not mount diagfs for controlled-stop guard test"
				/busybox poweroff -f
				return 1
			fi
			if echo force > /mnt/lkm4ctr-diagfs/v/control; then
				echo "RMMOD_TEST_FAILED: controlled vendor_kernel stop ignored active mqueue mounts"
				/busybox poweroff -f
				return 1
			fi
			if [ "$(/busybox cat /mnt/lkm4ctr-diagfs/v/status)" != "active" ]; then
				echo "RMMOD_TEST_FAILED: vendor_kernel state changed after refusing busy stop"
				/busybox poweroff -f
				return 1
			fi
			if ! /busybox umount /mnt/lkm4ctr-diagfs; then
				echo "RMMOD_TEST_FAILED: could not release diagfs after controlled-stop test"
				/busybox poweroff -f
				return 1
			fi
		else
			if [ "$native_mqueue_preexisting" -ne 1 ]; then
				echo "RMMOD_TEST_FAILED: module did not register mqueue and no pre-insmod filesystem collision was observed"
				/busybox poweroff -f
				return 1
			fi
			echo "=== LKM4CTR_QEMU_TEST: built-in mqueue collision confirmed before insmod; module mount guard not applicable ==="
		fi
		if ! /busybox umount /mnt/lkm4ctr-mqueue; then
			echo "RMMOD_TEST_FAILED: could not release the user mqueue mount"
			/busybox poweroff -f
			return 1
		fi
		if /busybox grep -q ' /dev/mqueue ' /proc/self/mountinfo; then
			if ! /busybox umount /dev/mqueue; then
				echo "RMMOD_TEST_FAILED: could not release /dev/mqueue"
				/busybox poweroff -f
				return 1
			fi
		fi
		/busybox sleep 1
		/busybox mkdir -p /mnt/lkm4ctr-nsproxy-diag
		if ! /busybox mount -t lkm4ctr diag /mnt/lkm4ctr-nsproxy-diag; then
			echo "RMMOD_TEST_FAILED: could not mount diagfs for live-nsproxy lifetime test"
			/busybox poweroff -f
			return 1
		fi
		nsproxy_live_before=$(/busybox awk '$1 == "nsproxy_live:" { print $2 }' /mnt/lkm4ctr-nsproxy-diag/v/resources)
		case "$nsproxy_live_before" in
			''|*[!0-9]*)
				echo "RMMOD_TEST_FAILED: diagfs does not report a numeric live nsproxy count"
				/busybox poweroff -f
				return 1
				;;
		esac
		if [ "$nsproxy_live_before" -ne 0 ]; then
			echo "RMMOD_TEST_FAILED: unexpected preexisting vendored nsproxy object(s): $nsproxy_live_before"
			/busybox poweroff -f
			return 1
		fi
		/busybox mkdir -p /tmp
		/busybox rm -f /tmp/lkm4ctr-nsproxy-ready
		/busybox unshare -i /busybox sh -c 'echo ready >/tmp/lkm4ctr-nsproxy-ready; exec /busybox sleep 60' &
		nsproxy_holder_pid=$!
		nsproxy_wait=0
		while [ "$nsproxy_wait" -lt 10 ] && [ ! -s /tmp/lkm4ctr-nsproxy-ready ]; do
			/busybox sleep 1
			nsproxy_wait=$((nsproxy_wait + 1))
		done
		if [ ! -s /tmp/lkm4ctr-nsproxy-ready ]; then
			echo "RMMOD_TEST_FAILED: could not hold an IPC namespace without an NSFS descriptor"
			/busybox kill "$nsproxy_holder_pid" 2>/dev/null || true
			/busybox poweroff -f
			return 1
		fi
		nsproxy_live=$(/busybox awk '$1 == "nsproxy_live:" { print $2 }' /mnt/lkm4ctr-nsproxy-diag/v/resources)
		case "$nsproxy_live" in
			''|*[!0-9]*)
				echo "RMMOD_TEST_FAILED: diagfs returned an invalid live nsproxy count"
				/busybox kill "$nsproxy_holder_pid" 2>/dev/null || true
				/busybox poweroff -f
				return 1
				;;
		esac
		if [ "$nsproxy_live" -gt 0 ]; then
			if echo force > /mnt/lkm4ctr-nsproxy-diag/v/control; then
				echo "RMMOD_TEST_FAILED: controlled stop ignored a live vendored nsproxy"
				/busybox kill "$nsproxy_holder_pid" 2>/dev/null || true
				/busybox poweroff -f
				return 1
			fi
			if [ "$(/busybox cat /mnt/lkm4ctr-nsproxy-diag/v/status)" != "active" ]; then
				echo "RMMOD_TEST_FAILED: vendor_kernel state changed after refusing live-nsproxy stop"
				/busybox kill "$nsproxy_holder_pid" 2>/dev/null || true
				/busybox poweroff -f
				return 1
			fi
			if ! /busybox umount /mnt/lkm4ctr-nsproxy-diag; then
				echo "RMMOD_TEST_FAILED: could not unmount diagfs before live-nsproxy rmmod check"
				/busybox kill "$nsproxy_holder_pid" 2>/dev/null || true
				/busybox poweroff -f
				return 1
			fi
			/busybox rmmod lkm4ctr
			if ! /busybox grep -q '^lkm4ctr ' /proc/modules; then
				echo "RMMOD_TEST_FAILED: module disappeared despite a live vendored nsproxy"
				/busybox poweroff -f
				return 1
			fi
			/busybox kill "$nsproxy_holder_pid" 2>/dev/null || true
			/busybox wait "$nsproxy_holder_pid" 2>/dev/null || true
			/busybox mount -t lkm4ctr diag /mnt/lkm4ctr-nsproxy-diag || {
				echo "RMMOD_TEST_FAILED: could not remount diagfs after live-nsproxy rmmod check"
				/busybox poweroff -f
				return 1
			}
			nsproxy_wait=0
			while [ "$nsproxy_wait" -lt 10 ]; do
				nsproxy_live_after=$(/busybox awk '$1 == "nsproxy_live:" { print $2 }' /mnt/lkm4ctr-nsproxy-diag/v/resources)
				[ "$nsproxy_live_after" -eq 0 ] && break
				/busybox sleep 1
				nsproxy_wait=$((nsproxy_wait + 1))
			done
			if [ "${nsproxy_live_after:-missing}" != "0" ]; then
				echo "RMMOD_TEST_FAILED: vendored nsproxy remained after holder exit: ${nsproxy_live_after:-missing}"
				/busybox poweroff -f
				return 1
			fi
			echo "=== LKM4CTR_QEMU_TEST: live vendored nsproxy pins module until task exit ==="
		else
			/busybox kill "$nsproxy_holder_pid" 2>/dev/null || true
			/busybox wait "$nsproxy_holder_pid" 2>/dev/null || true
			if [ "$native_ipc_nsfs" -ne 1 ]; then
				echo "RMMOD_TEST_FAILED: unshare produced no vendored nsproxy and native IPC namespace support was not present before insmod"
				/busybox poweroff -f
				return 1
			fi
			echo "=== LKM4CTR_QEMU_TEST: live nsproxy assertion skipped; pre-insmod unshare -i proved native IPC namespace support ==="
		fi
		if /busybox grep -q ' /mnt/lkm4ctr-nsproxy-diag ' /proc/self/mountinfo; then
			if ! /busybox umount /mnt/lkm4ctr-nsproxy-diag; then
				echo "RMMOD_TEST_FAILED: could not unmount live-nsproxy diagfs"
				/busybox poweroff -f
				return 1
			fi
		fi
		/busybox mkdir -p /mnt/lkm4ctr-nsfs-diag
		if ! /busybox mount -t lkm4ctr diag /mnt/lkm4ctr-nsfs-diag; then
			echo "RMMOD_TEST_FAILED: could not mount diagfs for NSFS lifetime test"
			/busybox poweroff -f
			return 1
		fi
		if [ -w /proc/sys/vm/drop_caches ]; then
			echo 2 > /proc/sys/vm/drop_caches
			/busybox sleep 1
		fi
		nsfs_pins_before=$(/busybox awk '$1 == "nsfs_inode_pins:" { print $2 }' /mnt/lkm4ctr-nsfs-diag/v/resources)
		case "$nsfs_pins_before" in
			''|*[!0-9]*)
				echo "RMMOD_TEST_FAILED: diagfs does not report a numeric NSFS inode pin count"
				/busybox poweroff -f
				return 1
				;;
		esac
		if [ "$nsfs_pins_before" -ne 0 ]; then
			echo "RMMOD_TEST_FAILED: unexpected preexisting NSFS inode pin(s): $nsfs_pins_before"
			/busybox poweroff -f
			return 1
		fi
		/busybox rm -f /tmp/lkm4ctr-nsfs-ready
		/busybox unshare -i /busybox sh -c 'exec 3</proc/self/ns/ipc || exit 20; echo ready >/tmp/lkm4ctr-nsfs-ready; exec /busybox sleep 60' &
		nsfs_holder_pid=$!
		nsfs_wait=0
		while [ "$nsfs_wait" -lt 10 ] && [ ! -s /tmp/lkm4ctr-nsfs-ready ]; do
			/busybox sleep 1
			nsfs_wait=$((nsfs_wait + 1))
		done
		if [ ! -s /tmp/lkm4ctr-nsfs-ready ]; then
			echo "RMMOD_TEST_FAILED: could not open /proc/self/ns/ipc for NSFS lifetime test"
			/busybox kill "$nsfs_holder_pid" 2>/dev/null || true
			/busybox poweroff -f
			return 1
		fi
		/busybox sleep 1
		nsfs_pins_open=$(/busybox awk '$1 == "nsfs_inode_pins:" { print $2 }' /mnt/lkm4ctr-nsfs-diag/v/resources)
		case "$nsfs_pins_open" in
			''|*[!0-9]*)
				echo "RMMOD_TEST_FAILED: diagfs returned an invalid NSFS inode pin count"
				/busybox kill "$nsfs_holder_pid" 2>/dev/null || true
				/busybox poweroff -f
				return 1
				;;
		esac
		if [ "$nsfs_pins_open" -gt 0 ]; then
			/busybox mkdir -p /tmp/lkm4ctr-nsfs-bind
			/busybox touch /tmp/lkm4ctr-nsfs-bind/ipc
			if ! /busybox mount -o bind "/proc/$nsfs_holder_pid/ns/ipc" /tmp/lkm4ctr-nsfs-bind/ipc; then
				echo "RMMOD_TEST_FAILED: could not bind-mount the vendored IPC namespace inode"
				/busybox kill "$nsfs_holder_pid" 2>/dev/null || true
				/busybox poweroff -f
				return 1
			fi
			/busybox kill "$nsfs_holder_pid" 2>/dev/null || true
			/busybox wait "$nsfs_holder_pid" 2>/dev/null || true
			if [ -w /proc/sys/vm/drop_caches ]; then
				echo 2 > /proc/sys/vm/drop_caches
			fi
			nsfs_pins_after_task_exit=$(/busybox awk '$1 == "nsfs_inode_pins:" { print $2 }' /mnt/lkm4ctr-nsfs-diag/v/resources)
			case "$nsfs_pins_after_task_exit" in
				''|*[!0-9]*)
					echo "RMMOD_TEST_FAILED: invalid NSFS pin count after namespace task exit"
					/busybox poweroff -f
					return 1
					;;
			esac
			if [ "$nsfs_pins_after_task_exit" -eq 0 ]; then
				echo "RMMOD_TEST_FAILED: bind-mounted NSFS inode did not retain its pin after namespace task exit"
				/busybox poweroff -f
				return 1
			fi
			if ! /busybox umount /mnt/lkm4ctr-nsfs-diag; then
				echo "RMMOD_TEST_FAILED: could not unmount NSFS-test diagfs before direct rmmod"
				/busybox poweroff -f
				return 1
			fi
			/busybox rmmod lkm4ctr
			if ! /busybox grep -q '^lkm4ctr ' /proc/modules; then
				echo "RMMOD_TEST_FAILED: module disappeared despite an active vendored NSFS inode"
				/busybox poweroff -f
				return 1
			fi
			if ! /busybox mount -t lkm4ctr diag /mnt/lkm4ctr-nsfs-diag; then
				echo "RMMOD_TEST_FAILED: could not remount diagfs after NSFS-blocked rmmod"
				/busybox poweroff -f
				return 1
			fi
			if echo force > /mnt/lkm4ctr-nsfs-diag/v/control; then
				echo "RMMOD_TEST_FAILED: controlled stop ignored an active vendored NSFS inode"
				/busybox poweroff -f
				return 1
			fi
			if [ "$(/busybox cat /mnt/lkm4ctr-nsfs-diag/v/status)" != "active" ]; then
				echo "RMMOD_TEST_FAILED: vendor_kernel state changed after refusing NSFS-busy stop"
				/busybox poweroff -f
				return 1
			fi
			echo "=== LKM4CTR_QEMU_TEST: direct rmmod and diagfs stop refused NSFS bind mount after task exit ==="
		else
			/busybox kill "$nsfs_holder_pid" 2>/dev/null || true
			/busybox wait "$nsfs_holder_pid" 2>/dev/null || true
			if [ "$native_ipc_nsfs" -ne 1 ]; then
				echo "RMMOD_TEST_FAILED: opening /proc/self/ns/ipc created no NSFS pin and native support was absent before insmod"
				/busybox poweroff -f
				return 1
			fi
			echo "=== LKM4CTR_QEMU_TEST: NSFS lifetime assertion skipped; pre-insmod unshare -i proved native IPC namespace support ==="
		fi
		if [ "$nsfs_pins_open" -gt 0 ]; then
			if ! /busybox umount /tmp/lkm4ctr-nsfs-bind/ipc; then
				echo "RMMOD_TEST_FAILED: could not unmount the persistent NSFS bind mount"
				/busybox poweroff -f
				return 1
			fi
			if [ -w /proc/sys/vm/drop_caches ]; then
				echo 2 > /proc/sys/vm/drop_caches
			fi
			nsfs_wait=0
			while [ "$nsfs_wait" -lt 10 ]; do
				nsfs_pins_after=$(/busybox awk '$1 == "nsfs_inode_pins:" { print $2 }' /mnt/lkm4ctr-nsfs-diag/v/resources)
				[ "$nsfs_pins_after" -eq 0 ] && break
				if [ -w /proc/sys/vm/drop_caches ]; then
					echo 2 > /proc/sys/vm/drop_caches
				fi
				/busybox sleep 1
				nsfs_wait=$((nsfs_wait + 1))
			done
			if [ "$nsfs_pins_after" -ne 0 ]; then
				echo "RMMOD_TEST_FAILED: NSFS inode pin remained after closing descriptor and pruning dcache: ${nsfs_pins_after:-missing}"
				/busybox cat /proc/modules
				/busybox poweroff -f
				return 1
			fi
		fi
		if /busybox grep -q ' /dev/mqueue ' /proc/self/mountinfo; then
			if ! /busybox umount /dev/mqueue; then
				echo "RMMOD_TEST_FAILED: could not release /dev/mqueue remounted after refused controlled stop"
				/busybox poweroff -f
				return 1
			fi
			/busybox sleep 1
		fi
		if ! /busybox umount /mnt/lkm4ctr-nsfs-diag; then
			echo "RMMOD_TEST_FAILED: could not unmount diagfs after NSFS lifetime test"
			/busybox poweroff -f
			return 1
		fi
		module_refs=$(/busybox awk '$1 == "lkm4ctr" { print $3 }' /proc/modules)
		if [ "$module_refs" != "0" ]; then
			echo "RMMOD_TEST_FAILED: module references remain after normal mqueue unmount: ${module_refs:-missing}"
			/busybox cat /proc/modules
			/busybox poweroff -f
			return 1
		fi
		if ! /busybox grep -q '^lkm4ctr ' /proc/modules; then
			echo "RMMOD_TEST_FAILED: module disappeared before the final rmmod"
			/busybox poweroff -f
			return 1
		fi
		if ! /busybox rmmod lkm4ctr; then
			echo "RMMOD_TEST_FAILED: rmmod after normal mqueue unmount"
			/busybox cat /proc/modules
			/busybox poweroff -f
			return 1
		fi
		if /busybox grep -q '^lkm4ctr ' /proc/modules; then
			echo "RMMOD_TEST_FAILED: module remains listed in /proc/modules"
			/busybox poweroff -f
			return 1
		fi
		echo "=== LKM4CTR_QEMU_TEST: direct insmod/rmmod safety test DONE ==="
		/busybox poweroff -f
		return 0
	fi
	/busybox mdev -s
	echo "=== LKM4CTR_QEMU_TEST: stage1 (initramfs) ==="

	if test -e /dev/nvme0n1 && mount -t ext4 /dev/nvme0n1 /newroot; then
		echo "=== LKM4CTR_QEMU_TEST: nvme available, proceeding to stage 2 ==="

		cp /lkm4ctr_checker /newroot/
		if [ "$module_test_status" -eq 0 ]; then
			if [ ! -f /lkm4ctr.ko ]; then
				echo "module test requested but /lkm4ctr.ko is missing"
				return 1
			fi
			cp /lkm4ctr.ko /newroot/ || return 1
		fi
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
	elif test -e /dev/sda && mount -t ext4 /dev/sda /newroot; then
		echo "=== LKM4CTR_QEMU_TEST: sda available, proceeding to stage 2 ==="

		cp /lkm4ctr_checker /newroot/
		if [ "$module_test_status" -eq 0 ]; then
			if [ ! -f /lkm4ctr.ko ]; then
				echo "module test requested but /lkm4ctr.ko is missing"
				return 1
			fi
			cp /lkm4ctr.ko /newroot/ || return 1
		fi
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
	if [ "$module_test_status" -eq 0 ]; then
		/busybox sh /init -t /lkm4ctr_checker /lkm4ctr.ko
	else
		/busybox sh /init -t /lkm4ctr_checker -
	fi
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
	lkm4ctr_module_test_enabled
	module_test_status=$?
	if [ "$module_test_status" -gt 1 ]; then
		echo "missing or unreadable lkm4ctr_test_module boot argument"
		exit 1
	fi

	busybox mdev -s
	ifconfig lo up

	echo "=== LKM4CTR_QEMU_TEST: /lkm4ctr.ko + /lkm4ctr_checker copied onto the new root by stage1 ==="
	chmod 755 /lkm4ctr_checker

	echo "=== LKM4CTR_QEMU_TEST: running shared checker mode ==="
	if [ "$module_test_status" -eq 0 ]; then
		if [ ! -f /lkm4ctr.ko ]; then
			echo "module test requested but /lkm4ctr.ko is missing"
			exit 1
		fi
		/system/bin/sh /second_init -t /lkm4ctr_checker /lkm4ctr.ko || exit 1
	else
		/system/bin/sh /second_init -t /lkm4ctr_checker - || exit 1
		echo "=== LKM4CTR_QEMU_TEST: module test disabled; skipping module and container-runtime tests ==="
		echo "=== LKM4CTR_QEMU_TEST: DONE ==="
		source /do-umounts.sh
		return 0
	fi

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
	if [ ! -S /var/run/docker.sock ]; then
		echo "dockerd did not create /var/run/docker.sock"
		exit 1
	fi
	if ! docker info; then
		echo "dockerd is not responding to Docker API requests"
		exit 1
	fi

	echo "=== LKM4CTR_QEMU_TEST: docker run (test container sanity) ==="
	docker run --privileged --rm --network host --name alpine -d docker.io/arm64v8/alpine:latest init || exit 1
	container_running=false
	for i in $(seq 1 30); do
		container_running="$(docker inspect -f '{{.State.Running}}' alpine 2>/dev/null || true)"
		[ "$container_running" = true ] && break
		sleep 1
	done
	if [ "$container_running" != true ]; then
		echo "alpine test container did not reach running state"
		exit 1
	fi
	docker exec -iu 0 alpine ps -e || exit 1
	docker stop alpine || exit 1
	echo "=== LKM4CTR_QEMU_TEST: docker run (Ubuntu container process sanity) ==="
	docker run --privileged --rm --network host --name ubuntu -i docker.io/arm64v8/ubuntu:latest ps -e || exit 1
	pkill -2 dockerd || exit 1
	sleep 15

	echo "=== LKM4CTR_QEMU_TEST: FULL MODULE + CONTAINER TESTS DONE ==="
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
			exit $?
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
