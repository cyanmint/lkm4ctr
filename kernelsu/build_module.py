#!/usr/bin/env python3
"""Assemble a KernelSU ZIP containing KMI modules and the userspace checker."""

from __future__ import annotations

import json
import os
import stat
import sys
import zipfile
from pathlib import Path
from typing import NoReturn

ROOT = Path(__file__).resolve().parents[1]
MODULE = ROOT / "kernelsu"
SUPPORTED_FILE = MODULE / "supported_kmis.txt"
MODULE_FILES = (
    "module.prop",
    "control.sh",
    "action.sh",
    "service.sh",
    "webroot/index.html",
)
EXECUTABLE_FILES = {"control.sh", "action.sh", "service.sh", "bin/lkm4ctr_checker"}
ZIP_TIMESTAMP = (1980, 1, 1, 0, 0, 0)


def fail(message: str) -> NoReturn:
    raise SystemExit(message)


def main() -> None:
    if len(sys.argv) != 5:
        fail(
            "Usage: build_module.py <artifact-root> <checker-binary> "
            "<kmi-matrix-json> <output.zip>"
        )

    artifact_root = Path(sys.argv[1])
    checker = Path(sys.argv[2])
    try:
        requested = json.loads(sys.argv[3])
    except json.JSONDecodeError as error:
        fail(f"Invalid KMI matrix JSON: {error}")
    if not isinstance(requested, list) or not requested or not all(
        isinstance(item, str) for item in requested
    ):
        fail("The KMI matrix must be a non-empty JSON array of strings")
    if len(requested) != len(set(requested)):
        fail("The KMI matrix contains duplicate entries")

    supported = [
        line.strip()
        for line in SUPPORTED_FILE.read_text(encoding="utf-8").splitlines()
        if line.strip()
    ]
    requested_set = set(requested)
    unknown = sorted(requested_set - set(supported))
    if unknown:
        fail(f"Unsupported KMI(s) in build matrix: {', '.join(unknown)}")
    missing = [kmi for kmi in supported if kmi not in requested_set]
    if missing:
        fail(
            "KernelSU ZIP must include every supported KMI; missing: "
            + ", ".join(missing)
        )

    if not checker.is_file() or checker.stat().st_size == 0:
        fail(f"Checker binary is missing or empty: {checker}")
    checker_data = checker.read_bytes()
    if checker_data[:4] != b"\x7fELF":
        fail(f"Checker binary is not an ELF file: {checker}")

    output = Path(sys.argv[4]).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix(output.suffix + ".tmp")
    entries: dict[str, bytes] = {}
    for relative in MODULE_FILES:
        source = MODULE / relative
        if not source.is_file():
            fail(f"Required module file is missing: {source}")
        entries[relative] = source.read_bytes()
    entries["bin/lkm4ctr_checker"] = checker_data
    entries["supported_kmis.txt"] = ("\n".join(requested) + "\n").encode("utf-8")

    for kmi in requested:
        source = artifact_root / f"lkm4ctr-modules-{kmi}-arm64" / "lkm4ctr.ko"
        if not source.is_file() or source.stat().st_size == 0:
            fail(f"Built module is missing or empty: {source}")
        module_data = source.read_bytes()
        if module_data[:4] != b"\x7fELF":
            fail(f"Built module is not an ELF file: {source}")
        entries[f"kmi/lkm4ctr-{kmi}-arm64.ko"] = module_data

    try:
        with zipfile.ZipFile(temporary, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            for name, data in entries.items():
                info = zipfile.ZipInfo(name, date_time=ZIP_TIMESTAMP)
                info.create_system = 3
                mode = 0o755 if name in EXECUTABLE_FILES else 0o644
                info.external_attr = (stat.S_IFREG | mode) << 16
                info.compress_type = zipfile.ZIP_DEFLATED
                archive.writestr(info, data)
        with zipfile.ZipFile(temporary, "r") as archive:
            bad_entry = archive.testzip()
            if bad_entry is not None:
                fail(f"ZIP verification failed at entry: {bad_entry}")
            if set(archive.namelist()) != set(entries):
                fail("ZIP contents do not match the staged module files")
        os.replace(temporary, output)
    finally:
        if temporary.exists():
            temporary.unlink()

    print(f"Created {output} with {len(requested)} KMI module(s)")
    print("Included KMIs: " + ", ".join(requested))


if __name__ == "__main__":
    main()
