#!/system/bin/sh
# Public Domain (AI-generated; no copyright subsists in this content).

RESULTS=/mnt/lkm4ctr-test-results
mkdir -p "$RESULTS"
log_kernel() {
	echo "<6>$*" > /dev/kmsg
}
log_kernel REDROID_TEST_SERVICE_STARTED

disk_b=
attempt=0
while [ "$attempt" -lt 120 ] && [ -z "$disk_b" ]; do
	for candidate in /dev/block/sdb /dev/sdb; do
		if [ -b "$candidate" ] &&
		   /system/bin/toybox mount -t ext4 -o rw "$candidate" "$RESULTS"; then
			disk_b=$candidate
			break
		fi
	done
	if [ -z "$disk_b" ]; then
		/system/bin/toybox sleep 1
		attempt=$((attempt + 1))
	fi
done

if [ -z "$disk_b" ]; then
	log_kernel REDROID_TEST_DISKB_NOT_FOUND
	/system/bin/log -t lkm4ctr-test "Disk B was not available; tests were not run"
	/system/bin/setprop sys.powerctl shutdown
	exit 1
fi
log_kernel REDROID_TEST_DISKB_FOUND

exec >>"$RESULTS/lkm4ctr-test.log" 2>&1

echo "REDROID_TEST_STARTED"
echo "Disk B device: $disk_b"
echo "sys.boot_completed=$(/system/bin/getprop sys.boot_completed)"
echo "kernel=$(/system/bin/cat /proc/version)"
test_status=0

echo "=== lkm4ctr_checker before module load ==="
/system/bin/lkm4ctr_checker
pre_checker_status=$?
echo "PRE_CHECKER_STATUS=$pre_checker_status"

echo "=== inserting lkm4ctr.ko ==="
if /system/bin/toybox insmod /system/bin/lkm4ctr.ko; then
	module_loaded=1
	insmod_status=0
else
	insmod_status=$?
	module_loaded=0
	test_status=1
fi
echo "INSMOD_STATUS=$insmod_status"
if [ "$module_loaded" -eq 1 ]; then
	module_line=$(/system/bin/toybox grep '^lkm4ctr ' /proc/modules)
	set -- $module_line
	module_refcount=${3:-unavailable}
	echo "MODULE_REFCOUNT_AFTER_INSMOD=$module_refcount"
	log_kernel "LKM4CTR_MODULE_REFCOUNT_AFTER_INSMOD=$module_refcount"
fi

if [ "$module_loaded" -eq 1 ]; then
	echo "=== lkm4ctr_checker after module load ==="
	/system/bin/lkm4ctr_checker
	checker_status=$?
	echo "POST_CHECKER_STATUS=$checker_status"
	if [ "$checker_status" -ne 0 ]; then
		test_status=1
	fi
	module_line=$(/system/bin/toybox grep '^lkm4ctr ' /proc/modules)
	set -- $module_line
	module_refcount=${3:-unavailable}
	echo "MODULE_REFCOUNT_AFTER_POST_CHECKER=$module_refcount"
	log_kernel "LKM4CTR_MODULE_REFCOUNT_AFTER_POST_CHECKER=$module_refcount"
fi

if /system/bin/toybox grep -q ' /dev/mqueue ' /proc/self/mountinfo; then
	/system/bin/toybox umount /dev/mqueue
	echo "MQUEUE_UMOUNT_STATUS=$?"
fi
log_kernel LKM4CTR_TEST_MQUEUE_UNMOUNT_COMPLETE

echo "=== pre-rmmod module reference snapshot ==="
rmmod_safe=1
module_line=$(/system/bin/toybox grep '^lkm4ctr ' /proc/modules)
set -- $module_line
refcount=${3:-}
case "$refcount" in
	''|'-'|*[!0-9-]*|--*|-*-*)
		echo "RMMOD_SKIPPED_NO_VALID_PROC_MODULE_REFCOUNT=${refcount:-missing}"
		rmmod_safe=0
		test_status=1
		;;
	*)
		echo "PRE_RMMOD_PROC_MODULE_REFCOUNT=$refcount"
		log_kernel "LKM4CTR_MODULE_REFCOUNT_BEFORE_RMMOD=$refcount"
		if [ "$refcount" -lt 0 ]; then
			echo "RMMOD_SKIPPED_MODULE_REFCOUNT_UNDERFLOW=$refcount"
			rmmod_safe=0
			test_status=1
		fi
		;;
esac
if [ "$rmmod_safe" -eq 1 ]; then
	diag=/mnt/lkm4ctr-diag-pre-rmmod
	/system/bin/toybox mkdir -p "$diag"
	if /system/bin/toybox mount -t lkm4ctr diag "$diag"; then
		for item in references resources namespaces; do
			echo "--- pre-rmmod /v/$item ---"
			/system/bin/toybox cat "$diag/v/$item" 2>&1
		done
		if /system/bin/toybox umount "$diag"; then
			echo "PRE_RMMOD_DIAGFS_UMOUNT_STATUS=PASS"
		else
			echo "PRE_RMMOD_DIAGFS_UMOUNT_STATUS=FAIL"
			rmmod_safe=0
			test_status=1
		fi
	else
		echo "PRE_RMMOD_DIAGFS_MOUNT_STATUS=UNAVAILABLE"
	fi
else
	echo "PRE_RMMOD_DIAGFS_SKIPPED=1"
fi
/system/bin/toybox sync

echo "=== rmmod lkm4ctr ==="
if [ "$rmmod_safe" -eq 1 ]; then
	/system/bin/toybox rmmod lkm4ctr
	rmmod_status=$?
else
	# Avoid the kernel's fatal delete_module underflow BUG when a prior
	# module_put has already dropped the count below its base reference.
	rmmod_status=1
fi
echo "RMMOD_STATUS=$rmmod_status"
if /system/bin/toybox grep -q '^lkm4ctr ' /proc/modules; then
	echo "RMMOD_MODULE_REMAINS=1"
	test_status=1
	if [ "$rmmod_status" -ne 0 ]; then
		echo "=== module unload diagnostics ==="
		echo "--- /proc/modules ---"
		/system/bin/toybox cat /proc/modules
		echo "--- /proc/mounts ---"
		/system/bin/toybox cat /proc/mounts
		echo "--- active processes ---"
		/system/bin/toybox ps -A -o PID,PPID,NAME
		diag=/mnt/lkm4ctr-diag
		/system/bin/toybox mkdir -p "$diag"
		if /system/bin/toybox mount -t lkm4ctr diag "$diag"; then
			for item in references resources namespaces; do
				echo "--- /v/$item ---"
				/system/bin/toybox cat "$diag/v/$item" 2>&1
			done
			if /system/bin/toybox umount "$diag"; then
				echo "DIAGFS_UMOUNT_STATUS=PASS"
			else
				echo "DIAGFS_UMOUNT_STATUS=FAIL"
				test_status=1
			fi
		else
			echo "DIAGFS_MOUNT_STATUS=FAIL"
		fi
	fi
else
	echo "RMMOD_MODULE_REMAINS=0"
	if [ "$module_loaded" -ne 1 ] || [ "$rmmod_status" -ne 0 ]; then
		test_status=1
	fi
fi

echo "=== waiting for full Android boot ==="
log_kernel LKM4CTR_TEST_WAITING_FOR_ANDROID_BOOT
boot_completed=
attempt=0
while [ "$attempt" -lt 300 ]; do
	boot_completed=$(/system/bin/getprop sys.boot_completed)
	if [ "$boot_completed" = "1" ]; then
		break
	fi
	/system/bin/toybox sleep 1
	attempt=$((attempt + 1))
done
echo "ANDROID_BOOT_COMPLETED=$boot_completed"
log_kernel "ANDROID_BOOT_COMPLETED=$boot_completed"
log_kernel LKM4CTR_TEST_ANDROID_BOOT_WAIT_DONE
if [ "$boot_completed" != "1" ]; then
	test_status=1
fi

echo "=== collecting Android and kernel logs ==="
log_kernel LKM4CTR_TEST_LOG_COLLECTION_STARTED
/system/bin/logcat -b all -d -v threadtime >"$RESULTS/logcat.txt" 2>&1
log_kernel LKM4CTR_TEST_LOGCAT_COLLECTION_DONE
/system/bin/dmesg >"$RESULTS/dmesg.txt" 2>&1
log_kernel LKM4CTR_TEST_DMESG_COLLECTION_DONE
if [ "$test_status" -eq 0 ]; then
	echo "REDROID_MODULE_TEST_RESULT=PASS"
else
	echo "REDROID_MODULE_TEST_RESULT=FAIL"
fi
echo "TEST_EXIT_STATUS=$test_status"
/system/bin/toybox sync

log_kernel LKM4CTR_TEST_DISKB_UNMOUNT_STARTED
exec 1>&- 2>&-
if /system/bin/toybox umount "$RESULTS"; then
	log_kernel DISK_B_UMOUNT_STATUS=PASS
else
	log_kernel DISK_B_UMOUNT_STATUS=FAIL
	test_status=1
fi

/system/bin/toybox sync
if [ "$test_status" -eq 0 ]; then
	log_kernel REDROID_TEST_RESULT=PASS
else
	log_kernel REDROID_TEST_RESULT=FAIL
fi
/system/bin/setprop sys.powerctl shutdown
exit "$test_status"
