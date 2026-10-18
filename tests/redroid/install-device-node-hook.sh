#!/usr/bin/env bash
# Public Domain (AI-generated; no copyright subsists in this content).

set -euo pipefail

if [ "$#" -ne 1 ]; then
	echo "Usage: $0 <rootfs-directory>" >&2
	exit 2
fi

python3 - "$1" <<'PY'
from pathlib import Path
import re
import sys

path = Path(sys.argv[1]) / "init.rc"
rootfs = Path(sys.argv[1])
if not rootfs.is_dir():
    raise SystemExit(f"{rootfs}: rootfs directory does not exist")

marker = b"# lkm4ctr device-node setup"
command = b"exec -- /system/bin/sh /system/bin/lkm4ctr-device-nodes.sh"
needle = b"    exec_start apexd-bootstrap"
fragment = b"    " + marker + b"\n    " + command + b"\n"

if not path.exists():
    path.write_bytes(
        b"# Public Domain (AI-generated; no copyright subsists in this content).\n"
        b"on early-init\n"
        + fragment
    )
    print(f"created {path} with an early-init device-node hook")
    raise SystemExit(0)

content = path.read_bytes()
if marker in content:
    if command not in content:
        raise SystemExit(f"{path}: lkm4ctr hook marker exists without its command")
    print(f"lkm4ctr device-node hook already installed in {path}")
    raise SystemExit(0)

if content.count(needle) == 1:
    line_start = content.rfind(b"\n", 0, content.index(needle)) + 1
    content = content[:line_start] + fragment + content[line_start:]
else:
    events = list(re.finditer(rb"(?m)^on early-init\s*$", content))
    if len(events) != 1 or content.count(needle) > 1:
        raise SystemExit(
            f"{path}: cannot safely place early-init hook "
            f"(apexd command count={content.count(needle)}, "
            f"early-init action count={len(events)})"
        )
    line_end = content.find(b"\n", events[0].end())
    if line_end < 0:
        line_end = len(content)
    else:
        line_end += 1
    content = content[:line_end] + fragment + content[line_end:]

path.write_bytes(content)
print(f"installed lkm4ctr device-node hook in {path}")
PY
