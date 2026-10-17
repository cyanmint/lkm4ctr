# License and provenance inventory

This inventory separates upstream Linux sources from project-owned AI-generated glue. Project policy is to identify AI-generated files as Public Domain from creation because no copyright subsists in them; this is not a CC0 dedication. This statement records the project's provenance and classification policy, not legal advice.

## Audit method and scope

The audit covered all refs available in the non-shallow repository checkout, including the `lkm4ctr` branch, its origin-tracking branch, and release tags. The audit enumerated tracked files and the GPL SPDX headers present before reclassification, then traced each non-vendor header with `git blame --follow`, inspected the introducing commit/co-author metadata, and checked conversation history where code was committed under `cyanmint` credentials. A Copilot/Copilot-SWE author or co-author is positive AI evidence; a `cyanmint` author alone is not proof.

Before reclassification, 31 tracked project-owned files in `glue/` and `lkm4ctr_checker/` carried GPL SPDX headers. Twenty-nine had their GPL header introduced in Copilot-authored or Copilot-co-authored changes. Conversation history confirms the other two were created by the assistant under `cyanmint` credentials: `glue/lkm4ctr_diagfs.h` and `glue/vendor_kernel_nsfs_lifecycle.c`. The project owner also confirms `glue/lkm4ctr_diagfs.c` is AI-generated despite its `cyanmint` attribution. All 31 are therefore included in the Public Domain set below.

## AI-generated project code — Public Domain

The following 31 non-vendor files had GPL SPDX lines added to AI-generated code. Their headers now state Public Domain from creation, not CC0:

- `glue/helper.sh.c`
- `glue/lkm4ctr_compat.h`
- `glue/lkm4ctr_diagfs.c`
- `glue/lkm4ctr_diagfs.h`
- `glue/lkm4ctr_hotreload.c`
- `glue/lkm4ctr_hotreload.h`
- `glue/lkm4ctr_log.c`
- `glue/lkm4ctr_log.h`
- `glue/lkm4ctr_main.c`
- `glue/readme.txt.c`
- `glue/shadow_hijack.c`
- `glue/shadow_hook.h`
- `glue/vendor_kernel_compat.c`
- `glue/vendor_kernel_data_syms.h`
- `glue/vendor_kernel_diag.c`
- `glue/vendor_kernel_diag.h`
- `glue/vendor_kernel_ipc_callbacks.c`
- `glue/vendor_kernel_ipc_compat.c`
- `glue/vendor_kernel_ipc_mount.c`
- `glue/vendor_kernel_ipc_syscalls.c`
- `glue/vendor_kernel_module.c`
- `glue/vendor_kernel_nsfs_lifecycle.c`
- `glue/vendor_kernel_overlay.c`
- `glue/vendor_kernel_ovl_vfs_compat.c`
- `glue/vendor_kernel_ovl_vfs_compat.h`
- `glue/vendor_kernel_procfs.c`
- `glue/vendor_kernel_procfs_userns.c`
- `glue/vendor_kernel_syscalls.c`
- `glue/vendor_kernel_syscalls_userns.c`
- `lkm4ctr_checker/Makefile`
- `lkm4ctr_checker/lkm4ctr_checker.c`

Examples of the introducing AI-attributed history include `d4ee701` (initial hook/checker code), `092947b` and `73a2f4a` (vendor-kernel glue), and the Copilot-co-authored `e6ed8fde` change. Conversation history resolves the two `cyanmint`-attributed files whose Git commits lack AI co-author metadata. Keep any kernel `MODULE_LICENSE("GPL")` tag required for GPL-only symbol access; it is a kernel-loader compatibility declaration, not an SPDX copyright license for the source file.

## Vendored source tree — preserve as-is

- All of `vendor/` is excluded from the AI reclassification per project instruction. Preserve its current SPDX and copyright notices, including the upstream Linux kernel, IPC, nsfs, and overlayfs sources and the files in that build tree.

## Other known AI-generated files — Public Domain

- `kernelsu/customize.sh` is AI-generated and identified as Public Domain from creation; not CC0.
- `module.yaml` and this inventory are AI-generated metadata/documentation and identified as Public Domain from creation; not CC0.

## Mixed or unverified provenance

Files such as `.github/workflows/build-lkm4ctr.yml`, `README.md`, `kernelsu/module.prop`, and `kernelsu/tests/test_module.py` contain pre-existing material plus AI-assisted edits. They are not relabeled file-wide. Git authorship and source style alone do not establish AI provenance; keep their existing notices and classify only when the creation history provides evidence.
