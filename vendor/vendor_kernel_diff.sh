#!/bin/sh
# vendor_kernel_diff.sh - diff vendored files against kernel-common originals.
# Usage: ./vendor_kernel_diff.sh [kernel-common-path]
# Default kernel path: $RUNNER_TEMP/kernel-common
#
# The vendored fs/overlayfs sources span 5 separate KMI eras (see "Vendored
# overlayfs" in README.md), each copied from a different upstream branch, so
# each era needs its own kernel-common checkout to diff against. The
# default KERNEL_BASE (arg 1 / $RUNNER_TEMP/kernel-common) is used for the
# non-overlayfs vendored files and the [6.1, 6.3) overlayfs era
# (fs/overlayfs/, from android14-6.1). The other 4 eras are diffed only if
# their matching env var below points at a real kernel-common checkout for
# that branch; otherwise they are skipped with a note.
#
#   KERNEL_BASE_5_10  - kernel-common checkout of android12-5.10 or android13-5.10
#   KERNEL_BASE_5_15  - kernel-common checkout of android13-5.15 or android14-5.15
#   KERNEL_BASE_6_6   - kernel-common checkout of android15-6.6
#   KERNEL_BASE_6_12  - kernel-common checkout of android16-6.12

set -e
KERNEL_BASE="${1:-${RUNNER_TEMP:-/home/runner/work/_temp}/kernel-common}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
VENDOR_KERNEL_DIR="$SCRIPT_DIR"

if [ ! -d "$KERNEL_BASE" ]; then
    echo "ERROR: kernel-common not found at $KERNEL_BASE" >&2
    exit 1
fi

diff_file() {
    base="$1"
    rel_path="$2"
    vendored_rel="${3:-$rel_path}"
    vendored="$VENDOR_KERNEL_DIR/$vendored_rel"
    original="$base/$rel_path"
    if [ ! -f "$original" ]; then
        echo "SKIP: $vendored_rel (no upstream file at $rel_path under $base)"
        return
    fi
    if [ ! -f "$vendored" ]; then
        echo "MISSING: $vendored_rel (vendored file not found)"
        return
    fi
    echo "=== diff $vendored_rel (vs $base) ==="
    diff -u "$original" "$vendored" && echo "(no diff)" || true
    echo
}

diff_file "$KERNEL_BASE" "kernel/utsname.c"
diff_file "$KERNEL_BASE" "kernel/nsproxy.c"
diff_file "$KERNEL_BASE" "kernel/pid_namespace.c"
diff_file "$KERNEL_BASE" "kernel/user_namespace.c"
diff_file "$KERNEL_BASE" "ipc/namespace.c"
diff_file "$KERNEL_BASE" "fs/nsfs.c"
diff_file "$KERNEL_BASE" "kernel/cgroup/namespace.c"
diff_file "$KERNEL_BASE" "kernel/time/namespace.c"

# [6.1, 6.3) era (android14-6.1) - diffed against the default KERNEL_BASE.
for f in super.c namei.c util.c inode.c dir.c readdir.c copy_up.c export.c file.c overlayfs.h ovl_entry.h; do
    diff_file "$KERNEL_BASE" "fs/overlayfs/$f"
done

# The remaining 4 overlayfs eras each need their own upstream branch's
# kernel-common checkout; only diffed when the matching KERNEL_BASE_* env
# var is set to an existing directory.
diff_era() {
    era_base="$1"
    era_dir="$2"
    era_label="$3"
    if [ -z "$era_base" ]; then
        echo "SKIP era $era_dir (no $era_label checkout configured)"
        echo
        return
    fi
    if [ ! -d "$era_base" ]; then
        echo "SKIP era $era_dir ($era_label not found at $era_base)"
        echo
        return
    fi
    files="super.c namei.c util.c inode.c dir.c readdir.c copy_up.c export.c file.c overlayfs.h ovl_entry.h"
    case "$era_dir" in
        overlayfs_6_6) files="$files params.c params.h" ;;
        overlayfs_6_12) files="$files params.c params.h xattrs.c" ;;
    esac
    for f in $files; do
        diff_file "$era_base" "fs/overlayfs/$f" "fs/$era_dir/$f"
    done
}

diff_era "$KERNEL_BASE_5_10" "overlayfs_5_10" "KERNEL_BASE_5_10 (android12-5.10/android13-5.10)"
diff_era "$KERNEL_BASE_5_15" "overlayfs_5_15" "KERNEL_BASE_5_15 (android13-5.15/android14-5.15)"
diff_era "$KERNEL_BASE_6_6" "overlayfs_6_6" "KERNEL_BASE_6_6 (android15-6.6)"
diff_era "$KERNEL_BASE_6_12" "overlayfs_6_12" "KERNEL_BASE_6_12 (android16-6.12)"
