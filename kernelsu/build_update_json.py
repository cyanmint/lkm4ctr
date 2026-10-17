#!/usr/bin/env python3
"""Generate the KernelSU in-app update feed from module.prop."""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path
from typing import NoReturn

MODULE_ID = "lkm4ctr"
ZIP_URL = (
    "https://github.com/cyanmint/lkm4ctr/releases/latest/download/"
    "lkm4ctr-kernelsu-module.zip"
)
MAX_VERSION_CODE = 2_147_483_647


def fail(message: str) -> NoReturn:
    raise SystemExit(message)


def read_properties(path: Path) -> dict[str, str]:
    properties: dict[str, str] = {}
    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        stripped = line.strip()
        if not stripped or stripped.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        key = key.strip()
        if key in properties:
            fail(f"Duplicate module.prop key on line {line_number}: {key}")
        properties[key] = value.strip()
    return properties


def main() -> None:
    if len(sys.argv) != 3:
        fail("Usage: build_update_json.py <module.prop> <output.json>")

    module_prop = Path(sys.argv[1])
    output = Path(sys.argv[2])
    properties = read_properties(module_prop)
    if properties.get("id") != MODULE_ID:
        fail(f"module.prop id must be {MODULE_ID!r}")

    version = properties.get("version", "")
    version_code = properties.get("versionCode", "")
    if not version:
        fail("module.prop version is required")
    if not re.fullmatch(r"[0-9]+", version_code):
        fail("module.prop versionCode must be a positive decimal integer")
    parsed_version_code = int(version_code)
    if not 0 < parsed_version_code <= MAX_VERSION_CODE:
        fail(f"module.prop versionCode must be between 1 and {MAX_VERSION_CODE}")

    update = {
        "version": version,
        "versionCode": parsed_version_code,
        "zipUrl": ZIP_URL,
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", encoding="utf-8", newline="\n") as stream:
        json.dump(update, stream, ensure_ascii=False, indent=2)
        stream.write("\n")
    print(f"Created {output}")


if __name__ == "__main__":
    main()
