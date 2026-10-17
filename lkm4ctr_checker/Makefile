# Public Domain since creation (AI-generated; no copyright subsists; not CC0).
#
# lkm4ctr_checker is a plain userspace diagnostic binary (NOT a kernel
# module): it performs real syscalls (unshare/fork/setns, mq_*, msg*,
# mount(2)) to observe whether container-relevant isolation actually works
# on the running kernel, rather than only reporting compile-time
# IS_ENABLED(CONFIG_*) facts. See lkm4ctr_checker.c and README.md.
#
# Built statically by default (no dynamic libc dependency at runtime): the
# binary is routinely cross-compiled on one libc (e.g. glibc on an x86_64 CI
# runner) and then copied onto a device running a different one (e.g.
# Android's bionic), so it must not rely on a dynamic linker/libc.so being
# present or ABI-compatible on the target at all.
#
# Native build (host toolchain, static):
#   make
#
# Cross build for an Android/arm64 device target, e.g. via the NDK or the
# same clang toolchain used to build the other lkm4ctr kernel modules:
#   make CC=aarch64-linux-android30-clang
#   make CC="clang --target=aarch64-linux-gnu"
#
# Dynamic build (only if a matching libc/dynamic linker is guaranteed to be
# present on the target and a static one isn't available/desired):
#   make LDFLAGS=

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=gnu11
LDFLAGS ?= -static
LDLIBS  ?= -lrt

TARGET  := lkm4ctr_checker
SRC     := lkm4ctr_checker.c

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)/usr/bin/$(TARGET)

clean:
	rm -f $(TARGET)
