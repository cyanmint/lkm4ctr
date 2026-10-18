#!/system/bin/sh
# Public Domain (AI-generated; no copyright subsists in this content).

RESULTS=/mnt/lkm4ctr-test-results
mkdir -p "$RESULTS"

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
	/system/bin/log -t lkm4ctr-test "Disk B was not available; tests were not run"
	/system/bin/setprop sys.powerctl shutdown
	exit 1
fi

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
	echo "=== lkm4ctr_checker after module load ==="
	/system/bin/lkm4ctr_checker
	checker_status=$?
	echo "POST_CHECKER_STATUS=$checker_status"
	if [ "$checker_status" -ne 0 ]; then
		test_status=1
	fi
fi

if /system/bin/toybox grep -q ' /dev/mqueue ' /proc/self/mountinfo; then
	/system/bin/toybox umount /dev/mqueue
	echo "MQUEUE_UMOUNT_STATUS=$?"
fi

echo "=== rmmod lkm4ctr ==="
/system/bin/toybox rmmod lkm4ctr
rmmod_status=$?
echo "RMMOD_STATUS=$rmmod_status"
if /system/bin/toybox grep -q '^lkm4ctr ' /proc/modules; then
	echo "RMMOD_MODULE_REMAINS=1"
	test_status=1
else
	echo "RMMOD_MODULE_REMAINS=0"
	if [ "$module_loaded" -ne 1 ] || [ "$rmmod_status" -ne 0 ]; then
		test_status=1
	fi
fi

echo "=== collecting Android and kernel logs ==="
/system/bin/logcat -b all -d -v threadtime >"$RESULTS/logcat.txt" 2>&1
/system/bin/dmesg >"$RESULTS/dmesg.txt" 2>&1
if [ "$test_status" -eq 0 ]; then
	echo "REDROID_TEST_RESULT=PASS"
else
	echo "REDROID_TEST_RESULT=FAIL"
fi
echo "TEST_EXIT_STATUS=$test_status"
/system/bin/toybox sync

if ! /system/bin/toybox umount "$RESULTS"; then
	echo "DISK_B_UMOUNT_STATUS=FAIL"
	test_status=1
else
	echo "DISK_B_UMOUNT_STATUS=PASS"
fi

/system/bin/toybox sync
/system/bin/setprop sys.powerctl shutdown
exit "$test_status"
