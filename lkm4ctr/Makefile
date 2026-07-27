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
	vendor_kernel/kernel/cgroup/namespace.o \
	vendor_kernel/kernel/time/namespace.o \
	vendor_kernel/glue/vendor_kernel_module.o \
	vendor_kernel/glue/vendor_kernel_syscalls.o \
	vendor_kernel/glue/vendor_kernel_procfs.o \
	vendor_kernel/glue/vendor_kernel_overlay.o \
	vendor_kernel/glue/vendor_kernel_diag.o \
	vendor_kernel/glue/vendor_kernel_compat.o

OBJECT_FILES_NON_STANDARD_vendor_kernel/kernel/pid_namespace.o := y

ccflags-y += -I$(src)/../common

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
