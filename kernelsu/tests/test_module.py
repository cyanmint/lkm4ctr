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
UPDATE_BUILDER = ROOT / "kernelsu" / "build_update_json.py"
WEBUI = ROOT / "kernelsu" / "webroot" / "index.html"
ACTION = ROOT / "kernelsu" / "action.sh"
CUSTOMIZE = ROOT / "kernelsu" / "customize.sh"
KERNELSU_DESCRIPTION = (
    "Auto-load lkm4ctr.ko enables Docker and containers on all currently released "
    "arm64 Android GKI KMIs; no custom kernel flash."
)
AI_PUBLIC_DOMAIN_FILES = (
    "glue/helper.sh.c",
    "glue/lkm4ctr_compat.h",
    "glue/lkm4ctr_diagfs.c",
    "glue/lkm4ctr_diagfs.h",
    "glue/lkm4ctr_hotreload.c",
    "glue/lkm4ctr_hotreload.h",
    "glue/lkm4ctr_log.c",
    "glue/lkm4ctr_log.h",
    "glue/lkm4ctr_main.c",
    "glue/readme.txt.c",
    "glue/shadow_hijack.c",
    "glue/shadow_hook.h",
    "glue/vendor_kernel_compat.c",
    "glue/vendor_kernel_data_syms.h",
    "glue/vendor_kernel_diag.c",
    "glue/vendor_kernel_diag.h",
    "glue/vendor_kernel_ipc_callbacks.c",
    "glue/vendor_kernel_ipc_compat.c",
    "glue/vendor_kernel_ipc_mount.c",
    "glue/vendor_kernel_ipc_syscalls.c",
    "glue/vendor_kernel_module.c",
    "glue/vendor_kernel_nsfs_lifecycle.c",
    "glue/vendor_kernel_overlay.c",
    "glue/vendor_kernel_ovl_vfs_compat.c",
    "glue/vendor_kernel_ovl_vfs_compat.h",
    "glue/vendor_kernel_procfs.c",
    "glue/vendor_kernel_procfs_userns.c",
    "glue/vendor_kernel_syscalls.c",
    "glue/vendor_kernel_syscalls_userns.c",
    "lkm4ctr_checker/Makefile",
    "lkm4ctr_checker/lkm4ctr_checker.c",
)



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
    def test_module_prop_points_to_latest_release_update_json(self) -> None:
        properties = dict(
            line.split("=", 1)
            for line in (ROOT / "kernelsu" / "module.prop").read_text(encoding="utf-8").splitlines()
            if "=" in line
        )
        self.assertEqual(
            properties.get("updateJson"),
            "https://github.com/cyanmint/lkm4ctr/releases/latest/download/update.json",
        )
        self.assertEqual(properties.get("version"), "4.0.5")
        self.assertEqual(properties.get("versionCode"), "40005")
        self.assertEqual(
            properties.get("description"), KERNELSU_DESCRIPTION
        )

    def test_update_json_matches_release_version_and_latest_kernelSU_zip(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            temp = Path(directory)
            module_prop = temp / "module.prop"
            module_prop.write_text(
                "id=lkm4ctr\nversion=2.4.1\nversionCode=42\n", encoding="utf-8"
            )
            output = temp / "update.json"
            result = subprocess.run(
                [
                    "python3",
                    portable(UPDATE_BUILDER),
                    portable(module_prop),
                    "2.4.1",
                    portable(output),
                ],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(
                json.loads(output.read_text(encoding="utf-8")),
                {
                    "version": "2.4.1",
                    "versionCode": 42,
                    "zipUrl": "https://github.com/cyanmint/lkm4ctr/releases/latest/download/lkm4ctr-kernelsu-module.zip",
                },
            )

    def test_update_json_rejects_release_tag_mismatch(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            temp = Path(directory)
            module_prop = temp / "module.prop"
            module_prop.write_text(
                "id=lkm4ctr\nversion=4.0.5\nversionCode=40005\n", encoding="utf-8"
            )
            result = subprocess.run(
                [
                    "python3",
                    portable(UPDATE_BUILDER),
                    portable(module_prop),
                    "4.0.6",
                    portable(temp / "update.json"),
                ],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("must match release version", result.stderr)

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

    def test_action_shows_control_result_after_operation_finishes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            module_dir = Path(directory)
            action = module_dir / "action.sh"
            action.write_bytes(ACTION.read_bytes())
            control = module_dir / "control.sh"
            control.write_text(
                "#!/bin/sh\n"
                "printf 'control finished: %s\\n' \"$1\"\n"
                "exit \"${TEST_CONTROL_STATUS:-0}\"\n",
                encoding="utf-8",
            )
            control.chmod(0o755)

            for status, expected_message in (
                ("0", "Volume Up action completed successfully."),
                ("7", "Action failed with exit status 7."),
            ):
                with self.subTest(status=status):
                    env = os.environ.copy()
                    env["LKM4CTR_BUSYBOX"] = portable(module_dir / "missing-busybox")
                    env["TEST_CONTROL_STATUS"] = status
                    result = subprocess.run(
                        ["sh", portable(action)], env=env, capture_output=True, text=True
                    )
                    self.assertIn("control finished: toggle-load", result.stdout)
                    self.assertGreater(
                        result.stdout.index(expected_message),
                        result.stdout.index("control finished: toggle-load"),
                    )
                    if status == "0":
                        self.assertEqual(result.returncode, 0, result.stderr)
                    else:
                        self.assertEqual(result.returncode, 7)

    def test_install_customize_makes_module_files_executable(self) -> None:
        customize_text = CUSTOMIZE.read_text(encoding="utf-8")
        self.assertIn("Public Domain since creation", customize_text)
        self.assertIn("not CC0", customize_text)
        self.assertNotIn("CC0-1.0", customize_text)
        with tempfile.TemporaryDirectory() as directory:
            module_dir = Path(directory)
            service = module_dir / "service.sh"
            service.write_bytes((ROOT / "kernelsu" / "service.sh").read_bytes())
            action = module_dir / "action.sh"
            action.write_bytes(ACTION.read_bytes())
            control = module_dir / "control.sh"
            control.write_text(
                "#!/bin/sh\n"
                "MODDIR=${0%/*}\n"
                "if [ \"$1\" = checker ]; then \"$MODDIR/bin/lkm4ctr_checker\"; else printf 'control finished: %s\\n' \"$1\"; fi\n",
                encoding="utf-8",
            )
            checker = module_dir / "bin" / "lkm4ctr_checker"
            checker.parent.mkdir()
            checker.write_text("#!/bin/sh\nprintf 'checker ran\\n'\n", encoding="utf-8")
            module_binary = module_dir / "kmi" / "lkm4ctr-android14-6.1-arm64.ko"
            module_binary.parent.mkdir()
            module_binary.write_bytes(b"module fixture")
            for path in (service, action, control, checker, module_binary):
                path.chmod(0o644)

            source_script = (
                'MODPATH="$1"; '
                'abort() { printf "%s\\n" "$*" >&2; exit 1; }; '
                '. "$2"'
            )
            customize_result = subprocess.run(
                ["sh", "-c", source_script, "test-customize", portable(module_dir), portable(CUSTOMIZE)],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(customize_result.returncode, 0, customize_result.stderr)

            result = subprocess.run(
                ["sh", portable(service)], check=False, capture_output=True, text=True
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.strip(), "control finished: boot")
            if os.name != "nt":
                for path in (service, action, control, checker, module_binary):
                    with self.subTest(path=path.relative_to(module_dir)):
                        self.assertTrue(path.stat().st_mode & 0o111, f"{path} is not executable")

            env = os.environ.copy()
            env["LKM4CTR_BUSYBOX"] = portable(module_dir / "missing-busybox")
            action_result = subprocess.run(
                ["sh", portable(action)], env=env, check=False, capture_output=True, text=True
            )
            self.assertEqual(action_result.returncode, 0, action_result.stdout + action_result.stderr)
            self.assertIn("control finished: toggle-load", action_result.stdout)

            checker_result = subprocess.run(
                ["sh", "-c", '"$1" checker', "test-checker", portable(control)],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(checker_result.returncode, 0, checker_result.stderr)
            self.assertEqual(checker_result.stdout.strip(), "checker ran")

    def test_both_volume_keys_use_the_postoperation_result_handler(self) -> None:
        action = ACTION.read_text(encoding="utf-8")
        self.assertIn("run_control 'Volume Up' toggle-load", action)
        self.assertIn("run_control 'Volume Down' toggle-autoload", action)

    def test_release_asset_names_include_module_version(self) -> None:
        workflow = (ROOT / ".github" / "workflows" / "build-lkm4ctr.yml").read_text(
            encoding="utf-8"
        )
        self.assertIn("lkm4ctr-${module_version}-${kmi}-arm64.ko", workflow)
        self.assertIn("lkm4ctr_checker-${module_version}-arm64", workflow)

    def test_release_notes_url_is_downloaded_and_used_for_release(self) -> None:
        workflow = (ROOT / ".github" / "workflows" / "build-lkm4ctr.yml").read_text(
            encoding="utf-8"
        )
        self.assertIn("release_note_url:", workflow)
        self.assertIn("RELEASE_NOTE_URL: ${{ inputs.release_note_url }}", workflow)
        self.assertIn("release_note_url is required when create_release is enabled", workflow)
        self.assertIn("--proto '=https' --proto-redir '=https'", workflow)
        self.assertIn('--output "$notes_file" "$RELEASE_NOTE_URL"', workflow)
        self.assertIn('--notes-file "$notes_file"', workflow)

    def test_kernelsu_manifest_points_to_the_module_and_zip_asset(self) -> None:
        manifest = (ROOT / "module.yaml").read_text(encoding="utf-8")
        self.assertIn("schemaVersion: 1", manifest)
        self.assertIn("id: lkm4ctr", manifest)
        self.assertIn(f"summary: {KERNELSU_DESCRIPTION}", manifest)
        self.assertIn(
            'license: "Public Domain (AI-generated); GPL (vendor/); see copying.txt"',
            manifest,
        )
        self.assertIn("tags: [android, containers, docker, kernel, root]", manifest)
        self.assertIn('assetPattern: "lkm4ctr-kernelsu-module.zip"', manifest)

    def test_copying_file_summarizes_project_license_scope(self) -> None:
        copying = (ROOT / "copying.txt").read_text(encoding="utf-8")
        self.assertEqual(len(AI_PUBLIC_DOMAIN_FILES), 31)
        self.assertIn("vendor/", copying)
        self.assertIn("glue/", copying)
        self.assertIn("kernelsu/", copying)
        self.assertIn("lkm4ctr_checker/", copying)
        self.assertIn("AI-generated", copying)
        self.assertIn("no copyright subsists", copying)
        self.assertIn("CC0", copying)
        self.assertIn("WTFPL", copying)
        self.assertIn("0BSD", copying)
        self.assertIn("GNU GENERAL PUBLIC LICENSE", copying)
        self.assertIn("Version 2, June 1991", copying)
        self.assertIn("END OF TERMS AND CONDITIONS", copying)
        self.assertNotIn("glue/vendor_kernel_nsfs_lifecycle.c", copying)
        for relative_path in ("kernelsu/customize.sh", "module.yaml"):
            with self.subTest(ai_metadata=relative_path):
                source = (ROOT / relative_path).read_text(encoding="utf-8")
                header_line = 0 if relative_path == "module.yaml" else 1
                self.assertIn(
                    "Public Domain since creation", source.splitlines()[header_line]
                )
        for relative_path in AI_PUBLIC_DOMAIN_FILES:
            with self.subTest(path=relative_path):
                source = (ROOT / relative_path).read_text(encoding="utf-8")
                self.assertIn(
                    "Public Domain since creation",
                    source.splitlines()[0],
                )
                self.assertNotIn("SPDX-License-Identifier: GPL", source)

        remaining_project_gpl = subprocess.run(
            [
                "git",
                "grep",
                "-l",
                "SPDX-License-Identifier: GPL",
                "--",
                "glue/",
                "lkm4ctr_checker/",
            ],
            cwd=ROOT,
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertEqual(remaining_project_gpl.returncode, 1)
        self.assertEqual(remaining_project_gpl.stdout, "")

        for relative_path in (
            "vendor/fs/nsfs.c",
            "vendor/fs/overlayfs/copy_up.c",
            "vendor/ipc/msg.c",
        ):
            with self.subTest(vendored_source=relative_path):
                source = (ROOT / relative_path).read_text(encoding="utf-8")
                self.assertIn("SPDX-License-Identifier: GPL", source.splitlines()[0])

    def test_kernel_module_and_checker_report_release_version(self) -> None:
        kernel_module = (ROOT / "glue" / "lkm4ctr_main.c").read_text(encoding="utf-8")
        checker = (ROOT / "lkm4ctr_checker" / "lkm4ctr_checker.c").read_text(encoding="utf-8")
        self.assertIn('#define LKM4CTR_VERSION "4.0.5"', kernel_module)
        self.assertIn('#define LKM4CTR_CHECKER_VERSION "4.0.5"', checker)

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
                module_prop_contents = archive.read("module.prop").decode("utf-8")
                self.assertIn(
                    "version=4.0.5\nversionCode=40005", module_prop_contents
                )
                self.assertIn(f"description={KERNELSU_DESCRIPTION}", module_prop_contents)
                self.assertIn(
                    "updateJson=https://github.com/cyanmint/lkm4ctr/releases/latest/download/update.json",
                    module_prop_contents,
                )
                self.assertIn("action.sh", names)
                self.assertIn("customize.sh", names)
                self.assertIn("copying.txt", names)
                self.assertEqual(
                    archive.read("copying.txt"), (ROOT / "copying.txt").read_bytes()
                )
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
        for script in (CONTROL, ACTION, ROOT / "kernelsu" / "service.sh", CUSTOMIZE):
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
