/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vendor_kernel_data_syms.h - lkm4ctr [BUILD-COMPAT]: module-owned stand-ins
 * for the DATA symbols init_user_ns/overflowuid/overflowgid. This is NEW code
 * (not vendored).
 *
 * Unlike ordinary kernel functions, these cannot be resolved via
 * shadow_hook_resolve()'s register_kprobe() trick (kprobes only attach to
 * code/executable addresses; see glue/shadow_hook.h), and
 * CONFIG_TRIM_UNUSED_KSYMS has been observed to drop their module symbol
 * table entries entirely on some GKI KMIs (insmod "Unknown symbol
 * init_user_ns"/"overflowgid"/"overflowuid").
 *
 * init_user_ns is fixed up without ever needing the real symbol at all:
 * vns_real_init_user_ns is populated once, at the very start of
 * vendor_kernel_init() (glue/vendor_kernel_module.c), from
 * current_user_ns() -- itself just a static-inline read of
 * current_cred()->user_ns, never an unresolved symbol -- which, since
 * insmod always runs from a real top-level process context, is the exact
 * same object the running kernel's own init_user_ns symbol would have
 * pointed at. Every source reference to init_user_ns is therefore
 * redirected to dereference that captured pointer instead, so no
 * relocation to the (possibly trimmed) real symbol is ever emitted.
 *
 * NOTE: because of this, init_user_ns must never be used in a static/global
 * initializer (a runtime pointer dereference isn't a compile-time constant)
 * -- assign such fields at runtime instead, once vns_real_init_user_ns has
 * been set.
 *
 * overflowuid/overflowgid are plain scalars (the kernel.overflow{u,g}id
 * sysctl defaults), so a module-owned constant standing in for it can't
 * have this constant-init problem; 65534 is the standard default on every
 * target kernel. They are declared as plain (non-const) int to exactly
 * match the type of the real extern int overflowuid/overflowgid
 * declarations in <linux/highuid.h>, since those declarations are still
 * textually visible (and macro-substituted) wherever this header has
 * already been included.
 *
 * IMPORTANT: several real kernel headers define static inline helpers that
 * dereference these bare names directly (e.g. <linux/seq_file.h>'s
 * seq_user_ns(), <linux/mnt_idmapping.h>'s initial_idmapping(), and the
 * high2lowuid()/high2lowgid() macros <linux/highuid.h> defines under
 * CONFIG_UID16). Because such inline bodies are expanded verbatim into
 * whichever object file calls them, the #define below only redirects a
 * given call site if it is already in effect by the time that system
 * header is first included in that translation unit -- the preprocessor
 * cannot retroactively rewrite text it already expanded. Every .c file that
 * (transitively) calls into such helpers must therefore include this header
 * before any other header, and every other file including it must not rely
 * on this header being included first (see vendor_kernel.h's inclusion of
 * this header at its own top for a further example).
 */
#ifndef _VENDOR_KERNEL_DATA_SYMS_H
#define _VENDOR_KERNEL_DATA_SYMS_H

struct user_namespace;

extern struct user_namespace *vns_real_init_user_ns;
#define init_user_ns (*vns_real_init_user_ns)

extern int vns_local_overflowgid;
extern int vns_local_overflowuid;
#define overflowgid vns_local_overflowgid
#define overflowuid vns_local_overflowuid

#endif /* _VENDOR_KERNEL_DATA_SYMS_H */
