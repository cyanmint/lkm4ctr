#!/usr/bin/env python3
"""Host-side checks for the KernelSU module's packaging and KMI selection."""

from __future__ import annotations

import json
import os
import subprocess
import tempfile
import unittest
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CONTROL = ROOT / "kernelsu" / "control.sh"
BUILDER = ROOT / "kernelsu" / "build_module.py"
WEBUI = ROOT / "kernelsu" / "webroot" / "index.html"
ACTION = ROOT / "kernelsu" / "action.sh"


def portable(path: Path) -> str:
    return str(path).replace("\\", "/")


class KmiSelectionTests(unittest.TestCase):
    def resolve(self, release: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            ["sh", portable(CONTROL), "detect-kmi", release],
            check=False,
            capture_output=True,
            text=True,
        )

    def test_supported_android_kernel_releases_select_exact_kmi(self) -> None:
        cases = {
            "5.10.198-android12-9-gabc": "android12-5.10",
            "5.10.198-android13-7-gabc": "android13-5.10",
            "5.15.123-android13-9-gabc": "android13-5.15",
            "5.15.148-android14-2-gabc": "android14-5.15",
            "6.1.75-android14-11-gabc": "android14-6.1",
            "6.6.30-android15-5-gabc": "android15-6.6",
            "6.12.20-android16-3-gabc": "android16-6.12",
            "6.18.1-android17-1-gabc": "android17-6.18",
        }
        for release, expected in cases.items():
            with self.subTest(release=release):
                result = self.resolve(release)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout.strip(), expected)

    def test_unknown_or_ambiguous_kernel_release_is_rejected(self) -> None:
        for release in (
            "6.1.75-android15-5-gabc",
            "6.1.75-custom-kernel",
            "not-a-kernel-release",
        ):
            with self.subTest(release=release):
                result = self.resolve(release)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, "")


class KernelSUModuleTests(unittest.TestCase):
    def test_autoload_setting_defaults_on_and_can_be_disabled(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            env = os.environ.copy()
            env["LKM4CTR_STATE_DIR"] = directory
            control = ["sh", portable(CONTROL)]

            status = subprocess.run(control + ["status"], env=env, capture_output=True, text=True)
            self.assertEqual(status.returncode, 0, status.stderr)
            self.assertIn("AUTOLOAD=enabled", status.stdout)

            disabled = subprocess.run(control + ["disable-autoload"], env=env, capture_output=True, text=True)
            self.assertEqual(disabled.returncode, 0, disabled.stderr)
            self.assertEqual((Path(directory) / "auto_load").read_text(encoding="utf-8").strip(), "0")

            enabled = subprocess.run(control + ["enable-autoload"], env=env, capture_output=True, text=True)
            self.assertEqual(enabled.returncode, 0, enabled.stderr)
            self.assertEqual((Path(directory) / "auto_load").read_text(encoding="utf-8").strip(), "1")

    def test_status_detects_loaded_module_through_lsmod(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            temp = Path(directory)
            busybox = temp / "busybox"
            busybox.write_text(
                "#!/bin/sh\n"
                "[ \"$1\" = lsmod ] || exit 2\n"
                "printf 'Module Size Used by\\n'\n"
                "printf 'lkm4ctr 123 0\\n'\n",
                encoding="utf-8",
            )
            busybox.chmod(0o755)
            env = os.environ.copy()
            env["LKM4CTR_BUSYBOX"] = portable(busybox)
            env["LKM4CTR_STATE_DIR"] = portable(temp / "state")
            status = subprocess.run(
                ["sh", portable(CONTROL), "status"], env=env, capture_output=True, text=True
            )
            self.assertEqual(status.returncode, 0, status.stderr)
            self.assertIn("STATE=loaded", status.stdout)

    def test_lsmod_failure_reports_unknown_and_refuses_load_toggle(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            temp = Path(directory)
            busybox = temp / "busybox"
            calls = temp / "busybox-calls"
            busybox.write_text(
                "#!/bin/sh\n"
                "case \"$1\" in\n"
                "  lsmod) exit 1 ;;\n"
                "  *) printf '%s\\n' \"$1\" >> \"$LKM4CTR_BUSYBOX_CALLS\" ;;\n"
                "esac\n",
                encoding="utf-8",
            )
            busybox.chmod(0o755)
            env = os.environ.copy()
            env["LKM4CTR_BUSYBOX"] = portable(busybox)
            env["LKM4CTR_BUSYBOX_CALLS"] = portable(calls)
            env["LKM4CTR_STATE_DIR"] = portable(temp / "state")

            status = subprocess.run(
                ["sh", portable(CONTROL), "status"], env=env, capture_output=True, text=True
            )
            self.assertEqual(status.returncode, 0, status.stderr)
            self.assertIn("STATE=unknown", status.stdout)

            toggle = subprocess.run(
                ["sh", portable(CONTROL), "toggle-load"], env=env, capture_output=True, text=True
            )
            self.assertNotEqual(toggle.returncode, 0)
            self.assertIn("lsmod", toggle.stderr)
            self.assertFalse(calls.exists())

    def test_checker_command_runs_bundled_binary_and_returns_its_report(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            module_dir = Path(directory)
            checker = module_dir / "bin" / "lkm4ctr_checker"
            checker.parent.mkdir()
            checker.write_text("#!/bin/sh\nprintf 'checker report line\\n'\n", encoding="utf-8")
            checker.chmod(0o755)
            control = module_dir / "control.sh"
            control.write_bytes(CONTROL.read_bytes())
            result = subprocess.run(
                ["sh", portable(control), "checker"],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.strip(), "checker report line")

    def test_zip_contains_root_module_and_every_supported_kmi(self) -> None:
        kmis = [
            line.strip()
            for line in (ROOT / "kernelsu" / "supported_kmis.txt")
            .read_text(encoding="utf-8")
            .splitlines()
            if line.strip()
        ]
        with tempfile.TemporaryDirectory() as directory:
            temp = Path(directory)
            artifacts = temp / "artifacts"
            checker = temp / "checker" / "lkm4ctr_checker"
            checker.parent.mkdir()
            checker_data = b"\x7fELFchecker executable"
            checker.write_bytes(checker_data)
            for kmi in kmis:
                artifact = artifacts / f"lkm4ctr-modules-{kmi}-arm64"
                artifact.mkdir(parents=True)
                (artifact / "lkm4ctr.ko").write_bytes(b"\x7fELF" + kmi.encode("ascii"))
            output = temp / "lkm4ctr-kernelsu.zip"
            result = subprocess.run(
                [
                    "python3",
                    portable(BUILDER),
                    portable(artifacts),
                    portable(checker),
                    json.dumps(kmis),
                    portable(output),
                ],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            with zipfile.ZipFile(output) as archive:
                names = set(archive.namelist())
                self.assertIn("module.prop", names)
                self.assertIn("action.sh", names)
                self.assertIn("service.sh", names)
                self.assertIn("control.sh", names)
                self.assertIn("webroot/index.html", names)
                self.assertIn("bin/lkm4ctr_checker", names)
                self.assertEqual(archive.read("bin/lkm4ctr_checker"), checker_data)
                checker_mode = archive.getinfo("bin/lkm4ctr_checker").external_attr >> 16 & 0o777
                self.assertEqual(checker_mode, 0o755)
                for kmi in kmis:
                    self.assertIn(f"kmi/lkm4ctr-{kmi}-arm64.ko", names)
                self.assertEqual(archive.read("supported_kmis.txt").decode().splitlines(), kmis)
                self.assertEqual(sum(name.endswith(".ko") for name in names), len(kmis))
                self.assertIsNone(archive.testzip())

            partial_output = temp / "partial-kernelsu.zip"
            partial = subprocess.run(
                [
                    "python3",
                    portable(BUILDER),
                    portable(artifacts),
                    portable(checker),
                    json.dumps(kmis[:-1]),
                    portable(partial_output),
                ],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(partial.returncode, 0)
            self.assertIn("every supported KMI", partial.stderr)
            self.assertFalse(partial_output.exists())

    def test_webui_exposes_four_module_controls_checker_and_output(self) -> None:
        page = WEBUI.read_text(encoding="utf-8")
        for command in ("load", "unload", "enable-autoload", "disable-autoload", "checker"):
            with self.subTest(command=command):
                self.assertIn(f'data-command="{command}"', page)
        self.assertIn("navigator.language", page)
        self.assertIn("zh-Hans", page)
        self.assertIn("zh-Hant", page)
        self.assertIn("window.ksu.exec(command, '{}', callbackName)", page)
        self.assertIn('<textarea id="operation-output" rows="9" readonly', page)
        self.assertIn("operationOutput.value", page)

    def test_shell_scripts_are_ascii_english(self) -> None:
        for script in (CONTROL, ACTION, ROOT / "kernelsu" / "service.sh"):
            with self.subTest(script=script.name):
                self.assertTrue(script.read_bytes().isascii())

    def test_loaded_state_uses_lsmod_and_action_defaults_to_load_toggle(self) -> None:
        control = CONTROL.read_text(encoding="utf-8")
        action = ACTION.read_text(encoding="utf-8")
        self.assertIn('"$BUSYBOX" lsmod', control)
        self.assertNotIn("/proc/modules", control)
        self.assertIn("selection=up", action)
        self.assertIn("toggle-load", action)
        self.assertIn("Volume-key input is unavailable; defaulting to Volume Up.", action)


if __name__ == "__main__":
    unittest.main()
