# SPDX-License-Identifier: GPL-2.0
#
# Unified kbuild Makefile for the merged lkm4ctr out-of-tree module.
#
# Builds a single lkm4ctr.ko that links the shadow_hijack, vendor_kernel
# and shadow_cgdevices subsystem sources together, plus lkm4ctr_main.c for
# the module entry/exit sequence.

ifneq ($(KERNELRELEASE),)

obj-$(CONFIG_LKM4CTR) += lkm4ctr.o
ifeq ($(CONFIG_LKM4CTR),)
obj-m += lkm4ctr.o
endif

lkm4ctr-objs := \
	lkm4ctr_main.o \
	lkm4ctr_log.o \
	lkm4ctr_diagfs.o \
	lkm4ctr_hotreload.o \
	helper.sh.o \
	readme.txt.o \
	shadow_hijack/shadow_hijack.o \
	shadow_cgdevices/shadow_cgdevices.o \
	vendor_kernel/kernel/utsname.o \
	vendor_kernel/kernel/nsproxy.o \
	vendor_kernel/kernel/pid_namespace.o \
	vendor_kernel/kernel/user_namespace.o \
	vendor_kernel/ipc/namespace.o \
	vendor_kernel/ipc/util.o \
	vendor_kernel/ipc/msgutil.o \
	vendor_kernel/ipc/msg.o \
	vendor_kernel/ipc/sem.o \
	vendor_kernel/ipc/shm.o \
	vendor_kernel/ipc/mqueue.o \
	vendor_kernel/ipc/compat.o \
	vendor_kernel/glue/vendor_kernel_ipc_syscalls.o \
	vendor_kernel/glue/vendor_kernel_ipc_mount.o \
	vendor_kernel/glue/vendor_kernel_ipc_compat.o \
	vendor_kernel/fs/nsfs.o \
	vendor_kernel/fs/overlayfs/super.o \
	vendor_kernel/fs/overlayfs/namei.o \
	vendor_kernel/fs/overlayfs/util.o \
	vendor_kernel/fs/overlayfs/inode.o \
	vendor_kernel/fs/overlayfs/dir.o \
	vendor_kernel/fs/overlayfs/readdir.o \
	vendor_kernel/fs/overlayfs/copy_up.o \
	vendor_kernel/fs/overlayfs/export.o \
	vendor_kernel/fs/overlayfs/file.o \
	vendor_kernel/fs/overlayfs/params.o \
	vendor_kernel/fs/overlayfs/xattrs.o \
	vendor_kernel/kernel/cgroup/namespace.o \
	vendor_kernel/kernel/time/namespace.o \
	vendor_kernel/glue/vendor_kernel_module.o \
	vendor_kernel/glue/vendor_kernel_syscalls.o \
	vendor_kernel/glue/vendor_kernel_procfs.o \
	vendor_kernel/glue/vendor_kernel_overlay.o \
	vendor_kernel/glue/vendor_kernel_ovl_vfs_compat.o \
	vendor_kernel/glue/vendor_kernel_diag.o \
	vendor_kernel/glue/vendor_kernel_compat.o

OBJECT_FILES_NON_STANDARD_vendor_kernel/kernel/pid_namespace.o := y

ccflags-y += -I$(src)/../common

# [BUILD-COMPAT] Disable Clang CFI (CONFIG_CFI_CLANG, -fsanitize=(k)cfi) for
# every object that either (a) resolves a kernel symbol at runtime via
# shadow_hook_resolve()'s register_kprobe() trick and then calls through the
# resulting raw function pointer (every glue/vendor_kernel_*_compat.c
# real-name wrapper's r_<name>(...) call, the vendored overlayfs sources'
# vns_ovl_vfsc_<name> macro-redirected calls via
# glue/vendor_kernel_ovl_vfs_compat.h), or (b) is itself the ftrace/kprobe
# hook engine redirecting control flow into a replacement function chosen at
# runtime (shadow_cgdevices.c). None of these call sites can ever satisfy
# kernel CFI's compile-time type-hash check (the callee's real compiled
# identity is looked up by name at runtime, not known to the compiler at the
# call site), so the check is a guaranteed, fatal false positive here --
# observed as "Kernel panic - not syncing: CFI failure" during
# vendor_kernel_init() on CONFIG_CFI_CLANG=y GKI kernels (5.15+). This
# mirrors upstream's own CFLAGS_REMOVE_ftrace.o/insn.o (arch/arm64/
# kernel/Makefile): code that must jump to an address only known at runtime
# is expected to opt out of CFI instrumentation for itself, not to defeat
# CFI's protection for the rest of the kernel, which is untouched. Applied
# to every real source object below except the two embedded-text blobs
# (helper.sh.o/readme.txt.o, plain string data), lkm4ctr_main.o/
# lkm4ctr_log.o/lkm4ctr_diagfs.o, which never call a symbol resolved this
# way, and the objects listed below that are intentionally excluded.
#
# The following are intentionally EXCLUDED from this list even though they
# would otherwise qualify by (a)/(b) above, because doing so strips the KCFI
# type-hash prefix from a function DEFINED in that object that the real,
# CFI-instrumented kernel itself calls back into indirectly (via a
# kprobe/kretprobe/ftrace_ops/file_operations/proc_ops/file_system_type
# struct this module registers) -- breaking the *caller's* (real kernel's)
# CFI check on that indirect call the moment the callback fires, rather than
# the module's own unsafe outgoing call (which the check is meant to guard
# against). Observed panics of this shape: "CFI failure at
# alloc_fs_context+... (target: mqueue_init_fs_context+...)" and "CFI
# failure at kprobe_breakpoint_handler+... (target:
# shadow_hook_pre_handler+...)". Each such object either has no unsafe
# indirect call of its own at all, or has the specific function(s) that do
# marked with the kernel's own __nocfi function attribute
# (<linux/compiler_types.h>) instead, so the object as a whole keeps
# ordinary CFI instrumentation and its callback functions remain valid
# indirect-call targets:
#   - shadow_hijack/shadow_hijack.o: defines shadow_hook_pre_handler()
#     (kprobe pre_handler) and shadow_hook_thunk() (ftrace_ops.func), both
#     real-kernel-invoked callbacks. The only genuine unsafe indirect calls
#     in this file (the ftrace-backend shadow_hook_install()/
#     shadow_hook_remove(), through the resolved
#     shadow_{,un}register_ftrace_function_fn/shadow_ftrace_set_filter_ip_fn
#     pointers) are marked __nocfi individually instead.
#   - vendor_kernel/kernel/nsproxy.o: defines vns_exit_kprobe_pre_handler()
#     (kprobe pre_handler on do_exit()); makes no unsafe indirect calls of
#     its own (its shadow_hook_resolve()-based init_cgroup_ns/proc_alloc_inum
#     lookups are called through from vendor_kernel_module.o, which stays in
#     this list).
#   - vendor_kernel/ipc/util.o: registers sysvipc_proc_ops via
#     proc_create_data(); makes no unsafe indirect calls of its own.
#   - vendor_kernel/ipc/shm.o: defines shm_file_operations/
#     shm_file_operations_huge/shm_vm_ops; makes no unsafe indirect calls of
#     its own (unlike msg.c/sem.c/util.c/compat.c, which stay in this list).
#   - vendor_kernel/glue/vendor_kernel_procfs.o: defines vns_setgroups_fops
#     (the /proc/*/setgroups file_operations). The one genuine unsafe
#     indirect call in this file (vns_setgroups_create_fd(), through the
#     resolved anon_inode_getfd_secure_fn pointer) is marked __nocfi
#     individually instead.
#   - vendor_kernel/ipc/mqueue.o: registers mqueue_fs_type/mqueue_super_ops/
#     mqueue_file_operations, called back via e.g. alloc_fs_context()'s
#     fs_type->init_fs_context(fc) -> mqueue_init_fs_context(). Makes no
#     unsafe indirect calls of its own (fs_context_for_mount()/fc_mount()
#     etc. are ordinary direct calls into the by-name compat wrapper
#     functions defined in vendor_kernel_ipc_compat.o, which stays in this
#     list for the actual resolved-pointer call inside it).
VNS_CFI_UNSAFE_OBJS := \
	lkm4ctr_hotreload.o \
	shadow_cgdevices/shadow_cgdevices.o \
	vendor_kernel/kernel/utsname.o \
	vendor_kernel/kernel/pid_namespace.o \
	vendor_kernel/kernel/user_namespace.o \
	vendor_kernel/ipc/namespace.o \
	vendor_kernel/ipc/msgutil.o \
	vendor_kernel/ipc/msg.o \
	vendor_kernel/ipc/sem.o \
	vendor_kernel/ipc/compat.o \
	vendor_kernel/glue/vendor_kernel_ipc_syscalls.o \
	vendor_kernel/glue/vendor_kernel_ipc_mount.o \
	vendor_kernel/glue/vendor_kernel_ipc_compat.o \
	vendor_kernel/fs/nsfs.o \
	vendor_kernel/fs/overlayfs/super.o \
	vendor_kernel/fs/overlayfs/namei.o \
	vendor_kernel/fs/overlayfs/util.o \
	vendor_kernel/fs/overlayfs/inode.o \
	vendor_kernel/fs/overlayfs/dir.o \
	vendor_kernel/fs/overlayfs/readdir.o \
	vendor_kernel/fs/overlayfs/copy_up.o \
	vendor_kernel/fs/overlayfs/export.o \
	vendor_kernel/fs/overlayfs/file.o \
	vendor_kernel/fs/overlayfs/params.o \
	vendor_kernel/fs/overlayfs/xattrs.o \
	vendor_kernel/kernel/cgroup/namespace.o \
	vendor_kernel/kernel/time/namespace.o \
	vendor_kernel/glue/vendor_kernel_module.o \
	vendor_kernel/glue/vendor_kernel_syscalls.o \
	vendor_kernel/glue/vendor_kernel_overlay.o \
	vendor_kernel/glue/vendor_kernel_ovl_vfs_compat.o \
	vendor_kernel/glue/vendor_kernel_diag.o \
	vendor_kernel/glue/vendor_kernel_compat.o

$(foreach o,$(VNS_CFI_UNSAFE_OBJS),$(eval CFLAGS_REMOVE_$(o) += $(CC_FLAGS_CFI)))

# LKM4CTR_LOG_DMESG - build-time logging switch (see common/lkm4ctr_log.h
# and lkm4ctr/Kconfig's CONFIG_LKM4CTR_LOG_DMESG for the full rationale).
# This is a standalone out-of-tree module: it is never actually built as
# part of a real kernel .config/Kconfig tree (nothing in this repository
# sources lkm4ctr/Kconfig from a top-level Kconfig -- CONFIG_LKM4CTR above
# is only ever empty in every build this Makefile is used for, so the
# obj-m fallback just above always applies), so CONFIG_LKM4CTR_LOG_DMESG
# has no real Kconfig entry point to come from either. This Makefile
# variable is the standalone-module equivalent: it defaults to "n" so
# lkm4ctr.ko is completely silent in dmesg out of the box, logging only
# to the lkm4ctr diagfs's `log` files; override via
# `make LKM4CTR_LOG_DMESG=y` to also mirror every log line to the kernel
# log, matching this module's behaviour before the diagfs/ring-buffer log
# existed.
LKM4CTR_LOG_DMESG ?= n
ifeq ($(LKM4CTR_LOG_DMESG),y)
ccflags-y += -DCONFIG_LKM4CTR_LOG_DMESG=1
endif

else

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

modules_install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

.PHONY: all modules_install clean

endif
