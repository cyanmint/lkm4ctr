# shadow_hook — shared syscall hijacking ABI

`shadow_hook.h` is a small, purely declarative header that defines the ABI
shared by every syscall-hooking subsystem in the `lkm4ctr` family
(`vendor_kernel` and `shadow_cgdevices`; see `../README.md` for the umbrella
overview). It is what
turns those modules from an ioctl API that a *patched* container runtime must
opt into, into a **transparent MITM layer**: a stock, unpatched
`containerd`/`runc`/`dockerd` calls the real syscalls (`unshare(2)`,
`setns(2)`, `msgget(2)`, `mq_open(2)`, ...) and observes working behaviour
instead of `-ENOSYS`, because the module has redirected the kernel's own
(missing/stubbed) entry points into its shadow implementation.

## Where the implementation lives

This header used to be intentionally include-only, with every helper marked
`static inline` so each caller translation unit got its own private copy.
That is no longer the case: the single source of truth for
`shadow_hook_resolve()`, `shadow_hook_install()`, `shadow_hook_remove()`,
`shadow_hook_install_all()` and `shadow_hook_remove_all()` now lives in the
`shadow_hijack` subsystem source inside the merged **`lkm4ctr.ko`** module
(`../lkm4ctr/shadow_hijack/`). This header is now purely declarative: it
defines the ABI (`struct shadow_hook`, the `SHADOW_HOOK()` initialiser macro,
and the `extern` function prototypes) that the unified module's subsystems
share. See [`../lkm4ctr/shadow_hijack/README.md`](../lkm4ctr/shadow_hijack/README.md)
for the full rationale, including the compile-time ftrace-vs-kprobe backend
selection and the recursion guard.

## How it works

Each hooked symbol is described by a `struct shadow_hook` (candidate names to
resolve, replacement function, storage for the original function pointer,
and the owning module). `shadow_hook_install()`:

1. Resolves the target symbol's address via the well-known
   `register_kprobe()`/`unregister_kprobe()` trick (works regardless of
   whether `kallsyms_lookup_name()` is exported).
2. Picks its backend at **compile time**: an `ftrace_ops`/`IPMODIFY` hook
   when `CONFIG_FUNCTION_TRACER`/`CONFIG_DYNAMIC_FTRACE` are available,
   otherwise a `kprobe` `pre_handler` hook — several "certified"/production
   Android GKI boot images ship with `CONFIG_FUNCTION_TRACER` disabled
   entirely, so the kprobe backend is what actually runs on stock GKI.
3. When the traced/probed function is entered, the backend rewrites the
   saved program counter (`regs->pc` on arm64, `regs->ip` on x86-64) so
   control flow jumps into our replacement instead of the original body.
4. The replacement keeps the original address so it can call through to
   genuine kernel behaviour (e.g. to transparently no-op on a kernel where
   the subsystem is natively present, or to chain into it after doing shadow
   bookkeeping). An owner-based recursion guard distinguishes that
   pass-through call from a fresh external call — see
   [`../lkm4ctr/shadow_hijack/README.md`](../lkm4ctr/shadow_hijack/README.md) for details.

Both the pre- and post-`CONFIG_DYNAMIC_FTRACE_WITH_ARGS` ftrace callback
signatures are supported so the exact same source builds unmodified across
every Android GKI kernel we target (5.10 through 6.12).

## Usage

```c
#include "shadow_hook.h"   /* ../common/shadow_hook.h, via -I../common */

static long (*real_sys_unshare)(const struct pt_regs *regs);

static long hook_sys_unshare(const struct pt_regs *regs)
{
	unsigned long flags = regs->regs[0];
	/* ... shadow bookkeeping ... */
	return real_sys_unshare(regs); /* or synthesize a result without calling through */
}

static const char * const unshare_names[] = { "__arm64_sys_unshare", "sys_unshare", NULL };
static struct shadow_hook unshare_hook =
	SHADOW_HOOK(unshare_names, hook_sys_unshare, &real_sys_unshare);

static struct shadow_hook *all_hooks[] = { &unshare_hook, NULL };

/* module_init: */ shadow_hook_install_all(all_hooks, "vendor_kernel");
/* module_exit: */ shadow_hook_remove_all(all_hooks);
```

## Honest limitations

* A hook redirects *entry* to a syscall wrapper; it cannot fabricate struct
  layout support (e.g. `task_struct::nsproxy`) that was never compiled into
  `vmlinux`. Each module still only provides the level of behaviour documented
  in its own README (e.g. `vendor_kernel` provides the real vendored
  UTS/PID/USER/IPC/mqueue paths but still documents NET/MNT/CGROUP caveats) —
  `shadow_hook` only removes
  the *userspace patch* requirement to reach that behaviour, it does not
  upgrade the underlying simulation fidelity.
* If a kernel is built *with* the corresponding native subsystem
  (`CONFIG_SYSVIPC=y`, ...), installing these hooks is unnecessary and the
  modules will simply not find a real syscall for their own shadow numbers to
  register a hook for; skip loading them, or leave them loaded — they call
  through to the real implementation.
* Only one hook (ftrace `IPMODIFY` owner, or kprobe) may target a given
  symbol at a time; do not load two shadow modules that hook the same
  syscall.
