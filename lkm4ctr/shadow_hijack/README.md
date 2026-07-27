# shadow_hijack — shared function-hijacking implementation

`shadow_hijack/` is the internal hook-engine subsystem source linked into the
merged `lkm4ctr.ko`. It hosts the
`shadow_hook_resolve()/install()/remove()/install_all()/remove_all()`
implementation shared by every syscall-hooking subsystem in the `lkm4ctr`
family: `vendor_kernel` and `shadow_cgdevices`. See `../../common/shadow_hook.h` for the full ABI
contract (struct layout, `SHADOW_HOOK()` initialiser macro, function
prototypes) that this subsystem implements and every caller includes.

## Why a separate subsystem?

Earlier revisions kept this logic as `static inline` helpers directly in
`shadow_hook.h`, so each module got its own private copy compiled in and no
shared `.ko` was required. Now that multiple subsystems hook syscalls,
that duplication was wasteful and, more importantly, made the recursion guard
(see below) fragile. The implementation now lives in exactly one place —
this subsystem — and `shadow_hook.h` is a purely declarative ABI header shared
by the unified module's callers.

Every hooking subsystem now links against this code inside the same final
`lkm4ctr.ko`, so there is no separate `Module.symvers` or `insmod` ordering
edge anymore. The init/exit functions in `shadow_hijack.c` are simple no-ops
called first/last by `lkm4ctr_main.c`.

## Backend selection (ftrace vs. kprobe)

The hooking backend is chosen at **compile time**, from what the target
kernel's own Kconfig enables:

* the `ftrace_ops`/`IPMODIFY` backend when `CONFIG_FUNCTION_TRACER` and
  `CONFIG_DYNAMIC_FTRACE` are both available, otherwise
* a `kprobe` `pre_handler` backend that redirects control flow by rewriting
  the trapped `pt_regs` program counter (`regs->pc` on arm64, `regs->ip` on
  x86-64) and returning 1 to tell the kprobes core it must not single-step
  the original instruction.

Several "certified"/production Android GKI boot images ship with
`CONFIG_FUNCTION_TRACER` compiled out entirely, so the kprobe backend is what
actually runs on stock GKI; `CONFIG_KPROBES` is a hard requirement of the
wider KernelSU/SUSFS ecosystem and is effectively always present. Both
backends expose the identical `shadow_hook_install()`/`shadow_hook_remove()`
API, so callers do not know or care which one is active.

## The owner-based recursion guard

A hook sits at the very first instruction of the target function, so it also
fires when the caller's own replacement function (`hook->function`) calls
through `hook->original` — which resolves to the exact same hooked address —
to invoke genuine kernel behaviour. Without a guard, that pass-through call
would be redirected straight back into `hook->function`, recursing until the
kernel stack overflows.

The guard detects the pass-through by checking whether the caller's return
address lies within the module that *owns* the hook
(`within_module(caller_pc, hook->owner)`). In the merged build
`THIS_MODULE` resolves to the unified `lkm4ctr.ko`, which is exactly what
we want: the pass-through call is always made from code inside that same
module (e.g. `vendor_kernel`'s
`vns_sys_msgget()` calling `real_sys_msgget()`), never from code inside
some unrelated external module. `struct shadow_hook::owner` is therefore
populated with the caller's `THIS_MODULE` by the `SHADOW_HOOK()` initialiser
macro (see `shadow_hook.h`), and both backends here check
`within_module(caller_pc, hook->owner)` against that value.

## Building

This subsystem builds only as part of the unified `lkm4ctr.ko`:

```sh
make -C /path/to/kernel/build M="$PWD/lkm4ctr/lkm4ctr" modules
```

Out-of-tree modules for GKI **must** be built inside the matching
`ghcr.io/ylarod/ddk-min:<kmi>-<release>` DDK image against its real
`vmlinux`/`Module.symvers`, not a bare `gki_defconfig` tree — see
[`../README.md`](../README.md) and
[`../../.github/workflows/build-lkm4ctr.yml`](../../.github/workflows/build-lkm4ctr.yml).

Load `lkm4ctr.ko`; do not try to build or load `shadow_hijack` separately.

## Diagfs paths

With `mount -t lkm4ctr diag <mnt>`, the shared hook engine is exposed at
`/<mnt>/hijack/{control,status,log,functions}`. `functions` lists the live hook
registry across all submodules. `control` is informational only here: the shared
hook engine is always active while `lkm4ctr.ko` is loaded, so unload commands
are intentionally unsupported on that path.
