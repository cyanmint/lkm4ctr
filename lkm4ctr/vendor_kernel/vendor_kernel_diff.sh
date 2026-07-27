#!/bin/sh
# vendor_kernel_diff.sh - diff vendored files against kernel-common originals.
# Usage: ./vendor_kernel_diff.sh [kernel-common-path]
# Default kernel path: $RUNNER_TEMP/kernel-common

set -e
KERNEL_BASE="${1:-${RUNNER_TEMP:-/home/runner/work/_temp}/kernel-common}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
VENDOR_KERNEL_DIR="$SCRIPT_DIR"

if [ ! -d "$KERNEL_BASE" ]; then
    echo "ERROR: kernel-common not found at $KERNEL_BASE" >&2
    exit 1
fi

diff_file() {
    rel_path="$1"
    vendored="$VENDOR_KERNEL_DIR/$rel_path"
    original="$KERNEL_BASE/$rel_path"
    if [ ! -f "$original" ]; then
        echo "SKIP: $rel_path (no upstream file)"
        return
    fi
    if [ ! -f "$vendored" ]; then
        echo "MISSING: $rel_path (vendored file not found)"
        return
    fi
    echo "=== diff $rel_path ==="
    diff -u "$original" "$vendored" && echo "(no diff)" || true
    echo
}

diff_file "kernel/utsname.c"
diff_file "kernel/nsproxy.c"
diff_file "kernel/pid_namespace.c"
diff_file "kernel/user_namespace.c"
diff_file "ipc/namespace.c"
diff_file "fs/nsfs.c"
diff_file "kernel/cgroup/namespace.c"
diff_file "kernel/time/namespace.c"
diff_file "fs/overlayfs/super.c"
diff_file "fs/overlayfs/namei.c"
diff_file "fs/overlayfs/util.c"
diff_file "fs/overlayfs/inode.c"
diff_file "fs/overlayfs/dir.c"
diff_file "fs/overlayfs/readdir.c"
diff_file "fs/overlayfs/copy_up.c"
diff_file "fs/overlayfs/export.c"
diff_file "fs/overlayfs/file.c"
diff_file "fs/overlayfs/overlayfs.h"
diff_file "fs/overlayfs/ovl_entry.h"
