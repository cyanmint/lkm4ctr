# SPDX-License-Identifier: GPL-2.0
#
# Unified kbuild Makefile for the merged lkm4ctr out-of-tree module.
#
# vendor/ is now the project-root module directory: it links the
# vendored namespace/IPC/mqueue/overlayfs subsystem sources (kernel/, ipc/,
# fs/) together with ../glue/ -- the only non-vendored source directory,
# containing the module entry/exit sequence (lkm4ctr_main.c), the unified
# lkm4ctr diagfs, hot-reload, logging, the shared ftrace/kprobe hook engine
# (shadow_hijack.c, formerly its own submodule) and every compat shim that
# bridges the vendored sources to a given KMI. shadow_cgdevices (the former
# cgroup-device compatibility submodule) has been removed entirely.

ifneq ($(KERNELRELEASE),)

obj-$(CONFIG_LKM4CTR) += lkm4ctr.o
ifeq ($(CONFIG_LKM4CTR),)
obj-m += lkm4ctr.o
endif

lkm4ctr-objs := \
	../glue/lkm4ctr_main.o \
	../glue/lkm4ctr_log.o \
	../glue/lkm4ctr_diagfs.o \
	../glue/lkm4ctr_hotreload.o \
	../glue/helper.sh.o \
	../glue/readme.txt.o \
	../glue/shadow_hijack.o \
	kernel/utsname.o \
	kernel/nsproxy.o \
	kernel/pid_namespace.o \
	kernel/user_namespace.o \
	ipc/namespace.o \
	ipc/util.o \
	ipc/msgutil.o \
	ipc/msg.o \
	ipc/sem.o \
	ipc/shm.o \
	ipc/mqueue.o \
	ipc/compat.o \
	../glue/vendor_kernel_ipc_syscalls.o \
	../glue/vendor_kernel_ipc_mount.o \
	../glue/vendor_kernel_ipc_compat.o \
	../glue/vendor_kernel_ipc_callbacks.o \
	fs/nsfs.o \
	fs/overlayfs/super.o \
	fs/overlayfs/namei.o \
	fs/overlayfs/util.o \
	fs/overlayfs/inode.o \
	fs/overlayfs/dir.o \
	fs/overlayfs/readdir.o \
	fs/overlayfs/copy_up.o \
	fs/overlayfs/export.o \
	fs/overlayfs/file.o \
	fs/overlayfs/params.o \
	fs/overlayfs/xattrs.o \
	kernel/cgroup/namespace.o \
	kernel/time/namespace.o \
	../glue/vendor_kernel_module.o \
	../glue/vendor_kernel_syscalls.o \
	../glue/vendor_kernel_procfs.o \
	../glue/vendor_kernel_procfs_setgroups.o \
	../glue/vendor_kernel_overlay.o \
	../glue/vendor_kernel_ovl_vfs_compat.o \
	../glue/vendor_kernel_diag.o \
	../glue/vendor_kernel_compat.o

OBJECT_FILES_NON_STANDARD_kernel/pid_namespace.o := y

ccflags-y += -I$(src)/../glue

# [BUILD-COMPAT] Disable Clang CFI (CONFIG_CFI_CLANG, -fsanitize=(k)cfi) for
# every object that either (a) resolves a kernel symbol at runtime via
# shadow_hook_resolve()'s register_kprobe() trick and then calls through the
# resulting raw function pointer (every ../glue/vendor_kernel_*_compat.c
# real-name wrapper's r_<name>(...) call, the vendored overlayfs sources'
# vns_ovl_vfsc_<name> macro-redirected calls via
# ../glue/vendor_kernel_ovl_vfs_compat.h), or (b) is itself the ftrace/kprobe
# hook engine redirecting control flow into a replacement function chosen at
# runtime (../glue/shadow_hijack.c's ftrace backend). None of these call sites can ever satisfy
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
# lkm4ctr_log.o, which never call a symbol resolved this way, and the
# objects listed below that are intentionally excluded (lkm4ctr_diagfs.o
# among them, see its bullet below).
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
#   - ../glue/shadow_hijack.o: defines shadow_hook_pre_handler()
#     (kprobe pre_handler) and shadow_hook_thunk() (ftrace_ops.func), both
#     real-kernel-invoked callbacks. The only genuine unsafe indirect calls
#     in this file (the ftrace-backend shadow_hook_install()/
#     shadow_hook_remove(), through the resolved
#     shadow_{,un}register_ftrace_function_fn/shadow_ftrace_set_filter_ip_fn
#     pointers) are marked __nocfi individually instead.
#   - kernel/nsproxy.o: defines vns_exit_kprobe_pre_handler()
#     (kprobe pre_handler on do_exit()); its shadow_hook_resolve()-based
#     init_cgroup_ns/proc_alloc_inum lookups are called through from
#     vendor_kernel_module.o, which stays in this list. The one genuine
#     unsafe indirect call in this file (create_new_namespaces(), through
#     the resolved vns_copy_mnt_ns_fn pointer) is marked __nocfi
#     individually instead -- CI observed a live "CFI failure ... (target:
#     copy_mnt_ns+...)" panic here when this call was left unmarked.
#   - ipc/util.o: registers sysvipc_proc_ops via
#     proc_create_data(); makes no unsafe indirect calls of its own.
#   - ipc/shm.o: defines shm_file_operations/
#     shm_file_operations_huge/shm_vm_ops; makes no unsafe indirect calls of
#     its own (unlike util.c/compat.c, which stay in this list).
#   - ipc/msg.o, ipc/sem.o: define newque()/
#     newary() (installed as msg_ops.getnew/sem_ops.getnew, ipc/msg.c,
#     ipc/sem.c) and sem.o's sem_more_checks() (sem_ops.more_checks), all
#     called back indirectly from ipc/util.c's ipcget()/ipcget_new(), which
#     is NOT in this list (see the util.o bullet above) and therefore stays
#     CFI-instrumented. CI observed a live "CFI failure at ipcget+...
#     (target: newque+...)" panic here when these objects were still in this
#     list. Neither file makes any unsafe resolved-pointer indirect call of
#     its own (their calls into vendor_kernel_ipc_compat.o's by-name compat
#     wrappers, e.g. security_msg_queue_free()/wake_q_add(), are ordinary
#     direct calls, not indirect ones -- no CFI check applies to those at
#     all), so removing them from this list is safe, mirroring shm.o above.
#   - fs/overlayfs/params.o: defines ovl_init_fs_context()
#     (installed as vns_ovl_fs_type.init_fs_context, fs/overlayfs/super.c),
#     called back via the real kernel's alloc_fs_context()
#     (fs/fs_context.c), and (indirectly, via the ovl_context_ops it
#     installs into fc->ops) ovl_parse_monolithic/ovl_parse_param/
#     ovl_get_tree/ovl_reconfigure/ovl_free, called back via
#     vfs_parse_fs_param()/vfs_get_tree()/reconfigure_super()/put_fs_context().
#     CI observed a live "CFI failure at alloc_fs_context+... (target:
#     ovl_init_fs_context+...)" panic here when this object was still in
#     this list. Unlike most other vendored overlayfs sources, only four of
#     this file's functions make a genuine unsafe resolved-pointer indirect
#     call of their own (ovl_parse_monolithic, ovl_mount_dir_noesc,
#     ovl_mount_dir_check, ovl_free_fs); each is marked __nocfi individually
#     (see the file's own header comment) so the object as a whole can keep
#     normal CFI instrumentation and be removed from this list.
#   - fs/overlayfs/super.o: defines ovl_fill_super() (installed
#     as fc->ops->get_tree, called back via the real kernel's
#     get_tree_nodev(), fs/super.c) plus ovl_dentry_operations/other
#     struct-installed callbacks. CI observed a live "CFI failure at
#     get_tree_nodev+... (target: ovl_fill_super+...)" panic here when this
#     object was still in this list. As with params.o above, only a small
#     subset of this file's functions make a genuine unsafe resolved-pointer
#     indirect call of their own (ovl_revalidate_real, ovl_statfs,
#     ovl_check_namelen, ovl_workdir_ok, ovl_get_upper,
#     ovl_check_rename_whiteout, ovl_make_workdir, ovl_get_fsid,
#     ovl_get_layers, ovl_check_layer, ovl_fill_super itself); each is
#     marked __nocfi individually (see the file's own header comment) so
#     the object as a whole can keep normal CFI instrumentation and be
#     removed from this list.
#   - fs/overlayfs/{namei,util,inode,dir,readdir,copy_up,
#     export,file,xattrs}.o: like super.o above, each of these defines
#     functions installed into real-kernel-invoked struct-of-function-
#     pointers callback tables (ovl_dir_inode_operations,
#     ovl_file_inode_operations, ovl_file_operations, ovl_dir_operations,
#     ovl_export_operations, xattr_handler.get/set, ovl_inode_operations,
#     etc., all set up in super.c/inode.c). CI observed a live "CFI failure
#     at inode_insert5+... (target: ovl_inode_set+...)" panic here (inode.o
#     was still in this list at the time). Every function across these nine
#     files that makes a genuine unsafe resolved-pointer indirect call of
#     its own is marked __nocfi individually (see each file's own header
#     comment for the specific list) so all nine objects can keep normal
#     CFI instrumentation and be removed from this list.
#   - ../glue/vendor_kernel_procfs_setgroups.o: defines
#     vns_setgroups_fops (the /proc/*/setgroups file_operations), split out
#     of vendor_kernel_procfs.o (see below) so it can keep normal CFI
#     instrumentation. The one genuine unsafe indirect call in this file
#     (vns_setgroups_create_fd(), through the resolved
#     anon_inode_getfd_secure_fn pointer) is marked __nocfi individually
#     instead.
#   - ipc/mqueue.o: registers mqueue_fs_type/mqueue_super_ops/
#     mqueue_file_operations, called back via e.g. alloc_fs_context()'s
#     fs_type->init_fs_context(fc) -> mqueue_init_fs_context(). Makes no
#     unsafe indirect calls of its own (fs_context_for_mount()/fc_mount()
#     etc. are ordinary direct calls into the by-name compat wrapper
#     functions defined in vendor_kernel_ipc_compat.o, which stays in this
#     list for the actual resolved-pointer call inside it).
#   - ipc/namespace.o: defines vns_ipcns_operations (a
#     proc_ns_operations, called back e.g. via /proc/pid/ns/ipc opens) and
#     free_ipc() (a work_struct callback invoked by process_one_work());
#     CI observed a live "CFI failure at process_one_work+... (target:
#     free_ipc+...)" panic here. Makes no unsafe indirect calls of its own
#     (vns_alloc_inum()/vns_free_inum()/vns_put_user_ns() are ordinary
#     direct calls into vendor_kernel_module.o, which stays in this list;
#     sem_exit_ns()/msg_exit_ns()/shm_exit_ns() are ordinary direct calls
#     into their own vendored .c files).
#   - kernel/utsname.o, kernel/pid_namespace.o,
#     kernel/user_namespace.o,
#     kernel/cgroup/namespace.o,
#     kernel/time/namespace.o, fs/nsfs.o: each
#     defines its own proc_ns_operations struct (vns_utsns_operations,
#     vns_pidns_operations/vns_pidns_for_children_operations,
#     vns_userns_operations, vns_cgroupns_operations,
#     vns_timens_operations/vns_timens_for_children_operations) and/or (for
#     nsfs.o) the nsfs pseudo-filesystem's own dentry/file callbacks, all
#     real-kernel-invoked via /proc/pid/ns/* opens and ns_get_path(). None
#     make any unsafe resolved-pointer indirect calls of their own
#     (user_namespace.o's bsearch() calls resolve to <linux/bsearch.h>'s
#     self-contained __inline_bsearch() via the macro redirect in
#     vendor_kernel.h, not a kallsyms-resolved pointer).
#   - ../glue/vendor_kernel_compat.o: defines
#     vns_kill_litter_super() (the mqueue_fs_type file_system_type's
#     kill_sb, macro-redirected from kill_litter_super via vendor_kernel.h)
#     -- CI observed a live "CFI failure at deactivate_locked_super+...
#     (target: vns_kill_litter_super+...)" panic here. Nearly every other
#     function in this file is a thin by-name-resolved compat wrapper
#     (vns_*_real() function pointers, resolved once at init via
#     shadow_hook_resolve()) with no real-kernel-invoked callback role of
#     its own; each of those (and vns_kill_litter_super itself, for its own
#     vns_kill_litter_super_real() call) is marked __nocfi individually so
#     the object as a whole can be removed from this list.
#   - ../glue/vendor_kernel_ipc_callbacks.o: defines simple_lookup()
#     (installed as mq_dir_inode_operations.lookup, ipc/mqueue.c), called
#     back via the real kernel's __lookup_slow() (fs/namei.c) on every mqueue
#     directory lookup, plus security_msg_queue_associate()/
#     security_sem_associate()/security_shm_associate() (installed as
#     msg_ops.associate/sem_ops.associate/shm_ops.associate, ipc/msg.c,
#     ipc/sem.c, ipc/shm.c), called back indirectly from this module's own
#     CFI-instrumented ipc/util.o ipcget()/ipcget_public(). CI observed live
#     "CFI failure at __lookup_slow+... (target: simple_lookup+...)" and
#     "CFI failure at ipcget+... (target: security_msg_queue_associate+...)"
#     panics here when these functions were still defined in
#     vendor_kernel_ipc_compat.o (which stays in this list): the whole-object
#     CFI removal that is safe for that file's other ~50 resolved-pointer
#     wrappers also strips these functions' own KCFI type-hash prefix,
#     breaking the indirect call into each of them. Split into their own
#     translation unit so they can keep normal CFI instrumentation; only
#     each function's own outgoing resolved-pointer call is marked __nocfi
#     individually, mirroring vns_kill_litter_super() above.
#   - ../glue/vendor_kernel_procfs.o: vendor_kernel_hook_newfstatat()/
#     vendor_kernel_hook_stat()/vendor_kernel_hook_lstat() each call through
#     real_sys_newfstatat/real_sys_stat/real_sys_lstat, by-name-resolved via
#     shadow_hook_resolve() in vns_procfs_hooks_init(). CI observed a live
#     "CFI failure (target: __arm64_sys_newfstatat+...)" panic here (called
#     back via the kretprobe_trampoline installed on the real syscall by
#     SHADOW_HOOK()), exactly the same KCFI-vs-kprobe-resolved-pointer
#     mismatch documented above for vendor_kernel_compat.o/
#     vendor_kernel_ipc_callbacks.o. vns_setgroups_open()/_read()/_write()
#     (real file_operations callbacks the kernel's own VFS calls back into
#     indirectly, unlike the resolved-pointer hooks above) were previously
#     also defined in this file; CI observed a live "CFI failure at
#     vfs_write+... (target: vns_setgroups_write+...)" panic in
#     lkm4ctr_checker's setgroups probe once this whole-object CFI removal
#     started applying to them too. They are now split into their own
#     ../glue/vendor_kernel_procfs_setgroups.o translation unit (see above,
#     kept out of this list) so they can keep normal CFI instrumentation,
#     mirroring vendor_kernel_ipc_callbacks.o's split from
#     vendor_kernel_ipc_compat.o.
#   - kernel/pid_namespace.o: vns_copy_pid_ns()/vns_put_pid_ns()
#     call through vns_real_copy_pid_ns_fn/vns_real_put_pid_ns_fn, resolved
#     by name (shadow_hook_resolve("copy_pid_ns")/("put_pid_ns")) so that on
#     a kernel that already has real pid-namespace support (CONFIG_PID_NS=y),
#     vendor_kernel hands pid namespace creation/teardown off to the real
#     kernel's own implementation instead of risking a "Wrong slab cache"
#     mismatch (see the comment on vns_copy_pid_ns() for the full rationale).
#   - lkm4ctr_diagfs.o: not part of the vendor_kernel/shadow_hook_resolve()
#     machinery above at all -- it resolves kill_litter_super/
#     generic_shutdown_super/d_alloc_name/simple_lookup/
#     generic_delete_inode/mount_nodev itself via its own, otherwise
#     identical, lkm4ctr_diagfs_resolve() (a register_kprobe() lookup,
#     same technique, separate implementation since diagfs.o has no
#     dependency on shadow_hijack.o). Registers file_system_type.mount/
#     kill_sb, inode_operations.lookup, and super_operations.drop_inode
#     (lkm4ctr_diagfs_mount/_kill_sb/_dir_lookup/_drop_inode, plus the
#     fill_super-time-only helpers lkm4ctr_diagfs_mkdir/_create_file),
#     each of which also makes one of the resolved-pointer calls above; all
#     are marked __nocfi individually so this object can keep normal CFI
#     instrumentation and its callback functions remain valid indirect-call
#     targets. CI observed a live "CFI failure at ... (target:
#     mount_nodev+...)" panic here on a KMI where insmod itself used to fail
#     first for an unrelated reason (see vendor_kernel_compat.c's
#     generic_shutdown_super/down_write_killable comments), which had left
#     this bug unreached until that other fix let insmod -- and thus this
#     mount -- succeed.
VNS_CFI_UNSAFE_OBJS := \
	../glue/lkm4ctr_hotreload.o \
	ipc/msgutil.o \
	ipc/compat.o \
	../glue/vendor_kernel_ipc_syscalls.o \
	../glue/vendor_kernel_ipc_mount.o \
	../glue/vendor_kernel_ipc_compat.o \
	../glue/vendor_kernel_module.o \
	../glue/vendor_kernel_syscalls.o \
	../glue/vendor_kernel_overlay.o \
	../glue/vendor_kernel_ovl_vfs_compat.o \
	../glue/vendor_kernel_diag.o \
	../glue/vendor_kernel_procfs.o \
	kernel/pid_namespace.o
$(foreach o,$(VNS_CFI_UNSAFE_OBJS),$(eval CFLAGS_REMOVE_$(o) += $(CC_FLAGS_CFI)))

# LKM4CTR_LOG_DMESG - build-time logging switch (see ../glue/lkm4ctr_log.h
# and Kconfig's CONFIG_LKM4CTR_LOG_DMESG for the full rationale).
# This is a standalone out-of-tree module: it is never actually built as
# part of a real kernel .config/Kconfig tree (nothing in this repository
# sources Kconfig from a top-level Kconfig -- CONFIG_LKM4CTR above
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
