# lkm4ctr.ko

`lkm4ctr/lkm4ctr/` builds the single merged `lkm4ctr.ko` loadable
kernel module.

## Layout

The unified module links these internal subsystem source trees together:

* `shadow_hijack/` — shared ftrace/kprobe hook implementation
* `vendor_kernel/` — vendored namespace, SysV IPC and POSIX mqueue compatibility
* `shadow_cgdevices/` — device-open compatibility hooks
* `lkm4ctr_diagfs.c` — the `lkm4ctr` diagnostics pseudo-filesystem: runtime
  control/status plus hooks/namespaces/log/resource/reference introspection,
  the `force2` aggressive-unload escalation, and the `helper.sh`/hot-reload
  file wiring
* `lkm4ctr_hotreload.c`/`.h` — the hot-reload subsystem: observational
  `finit_module(2)` hook, path validation, and the worker thread that hands
  off to a detached `rmmod && insmod <new.ko> hotreload=1` shell
* `helper.sh.c` — the `./mnt/helper.sh` diagfs file contents, generated as a
  plain C string

Their sources stay split by subsystem for maintainability, but they now build
and load only as one module with the single entry point in
`lkm4ctr_main.c`.

## Auto-loaded vs manual submodules

`insmod lkm4ctr.ko` brings up the shared hook engine (`shadow_hijack`),
auto-loads `vendor_kernel`, and registers the `lkm4ctr` diagfs filesystem
type. `shadow_cgdevices` remains manual. Mount the diagfs to inspect state,
load `shadow_cgdevices`, or manually unload/reload `vendor_kernel` later:

```sh
mount -t lkm4ctr diag /mnt
cat /mnt/vendor_kernel/status          # active right after insmod
cat /mnt/vendor_kernel/namespaces
echo load > /mnt/cgroupdevices/control # start the manual subsystem
```

Each runtime-loadable submodule exposes `control`, `status`, `log`, and (where
relevant) `hooks` or a live-state listing file directly under the mount root:

* `/mnt/global/{control,status,log,resources,references}`
* `/mnt/global/hotreload/{status,log,do-hot-reload}`
* `/mnt/helper.sh` (read-only, mode 0555, mount root)
* `/mnt/hijack/{control,status,log,functions,references}`
* `/mnt/vendor_kernel/{control,status,hooks,log,namespaces,msg,resources,references}`
* `/mnt/cgroupdevices/{control,status,hooks,log,references}`

Per-submodule `control` accepts `load`, `unload` (`remove`/`graceful` aliases),
and `forceunload` (`force`/`force_unload` aliases). `status` is read-only and
prints exactly one lifecycle state: `unloaded`, `loading`, `active`,
`graceful unloading`, or `force unloading`. `shadow_hijack` keeps the same
layout for symmetry, but its control file only reports help because the shared
hook engine itself is always active while `lkm4ctr.ko` is loaded.

Every submodule (and `global`) also exposes a `references` file that breaks
`module_refcount()` down into: the diagfs mount count, currently in-flight
redirected hook calls, and any remaining unaccounted "other" holders (most
likely another module that itself calls an `EXPORT_SYMBOL_GPL()` from
`lkm4ctr.ko`). This is meant to explain *why* an `rmmod`/unload attempt is
stuck at "try again" instead of just reporting that it is.

`rmmod lkm4ctr` (and `global/control`, below) always force-clean up whatever
submodules are still active on the way out, so a submodule's own state can
never block module removal.

## rmmod safety

Every hooked call executed while a redirected call is in flight holds a module
reference for its duration (see `common/shadow_hook.h` and
`shadow_hijack/README.md`), so an ordinary `rmmod lkm4ctr` refuses to race
with in-flight calls: the kernel returns `-EBUSY` ("Module lkm4ctr is in use")
until they finish, instead of panicking once their code is freed out from under
them.

## Global unload via the diagfs

Writing `unload` (or `1`/`remove`/`graceful`) to `./mnt/global/control`
triggers the module to unload itself with no further operator action:

```sh
echo unload > /mnt/global/control
```

This spawns a worker thread that quiesces every hook (stopping new redirected
calls from starting), waits for any already in-flight calls to finish, then
explicitly frees every submodule's resources, and finally launches a real
userspace `rmmod lkm4ctr` on its own. If in-flight calls don't drain within 30
seconds, the attempt is aborted, hooks resume normal operation, and the module
stays loaded. Writing `forceunload`/`force`/`force_unload` instead runs the
same sequence, except every submodule's resources are freed immediately after
quiescing rather than waiting until the very end.

Reading `./mnt/global/control` prints the accepted commands; reading
`./mnt/global/status` prints the current lifecycle state. `./mnt/global/log`
contains the combined log stream and `./mnt/global/resources` aggregates the
live resources still tracked across the linked subsystems.

## `force2`: aggressive forced unload

Writing `force2` to `./mnt/global/control` starts (or, if a graceful/force
unload is already waiting on in-flight calls to drain, immediately escalates
it to) a more aggressive unload path, for the rare case where even
`forceunload` still leaves `rmmod` reporting "try again" because something
keeps re-acquiring the module reference:

```sh
echo force2 > /mnt/global/control
```

Unlike `forceunload`, `force2` does not wait for `module_refcount()` to drain
on its own. Instead it directly loops calling `module_put(THIS_MODULE)` until
the refcount reaches its unloaded floor (bounded, and every iteration is
logged to `global/log`), then runs `rmmod -f` instead of a plain `rmmod`
(requires the target kernel to have `CONFIG_MODULE_FORCE_UNLOAD=y` for `-f` to
actually bypass the kernel's own refcount/version checks). Every step -- which
hooks were quiesced, the mount/in-flight/other reference breakdown at each
poll, and the final `rmmod`/`rmmod -f` invocation -- is logged verbosely to
`global/log` (see `global/references` for the same breakdown on demand).

## Hot reload

`./mnt/global/hotreload/` lets a new build of `lkm4ctr.ko` replace the
currently-loaded one without an external `rmmod`/`insmod` sequence run by
hand:

```sh
echo /path/to/new/lkm4ctr.ko > /mnt/global/hotreload/do-hot-reload
cat /mnt/global/hotreload/status   # progress / first-load vs hot-reloaded
cat /mnt/global/hotreload/log      # this subsystem's own log
```

The target path must be absolute, end in `.ko`, stay within a restricted
`[A-Za-z0-9/._-]` charset, and exist and be readable at the time it is
written. Once accepted, a worker thread quiesces every hook (same as a
graceful unload), waits for in-flight calls to drain, unmounts the diagfs,
then hands off to a detached shell that retries `rmmod lkm4ctr` until it
succeeds and then runs `insmod <path> hotreload=1`, before finally calling
`module_put_and_kthread_exit()` on the old module. The reloaded module's
`lkm4ctr_hotreload_init()` reads its own `hotreload` module parameter to tell
whether it is a first load or a hot-reloaded restart, and reports that in
`global/hotreload/status` and the log.

## `helper.sh`

`./mnt/helper.sh` is a read-only (mode `0555`) POSIX-sh script, served
directly from the diagfs mount root, wrapping the commands above into shell
functions:

```sh
export lkm4ctr_diagfs=/mnt
. "$lkm4ctr_diagfs/helper.sh"

lkm4ctr status                       # summarise global + every submodule
lkm4ctr control vendor_kernel unload # write "unload" to vendor_kernel/control
lkm4ctr load                         # alias for: control global load
lkm4ctr forceunload hijack           # alias for: control hijack forceunload
lkm4ctr force2                       # alias for: control global force2
lkm4ctr logcat vendor_kernel         # cat vendor_kernel/log
lkm4ctr references global            # cat global/references
lkm4ctr hot-upgrade /path/to/new/lkm4ctr.ko
lkm4ctr help                         # full command list
```

Its contents are generated into `lkm4ctr/helper.sh.c` as a plain C string
constant (`lkm4ctr_helper_sh_data`) and served verbatim by the diagfs; edit
the canonical script and regenerate that file rather than hand-editing the
C string.

## Build

```sh
make -C /path/to/kernel/build M="$PWD/lkm4ctr/lkm4ctr" modules
```

For Android GKI, build against the matching DDK `vmlinux`/`Module.symvers`
sysroot exactly as described in `../README.md`.
