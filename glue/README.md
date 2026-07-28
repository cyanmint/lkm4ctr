# lkm4ctr.ko

`glue/` (this directory) and `vendor/` (its sibling) together build the
single merged `lkm4ctr.ko` loadable kernel module.

## Layout

The unified module links these sources together:

* `glue/` — the code this project actually writes: the shared
  ftrace/kprobe hook engine (`shadow_hook.h`/`shadow_hijack.c`), the module
  entry/exit point (`lkm4ctr_main.c`), the `lkm4ctr` diagnostics
  pseudo-filesystem (`lkm4ctr_diagfs.c`), hot reload (`lkm4ctr_hotreload.c`/
  `.h`), logging (`lkm4ctr_log.c`/`.h`), the vendor/VFS-overlayfs
  compat glue (`vendor_kernel_*.c`/`.h`), and the generated `helper.sh`/
  `readme.txt` diagfs file contents (`helper.sh.c`/`readme.txt.c`)
* `kernel/`, `ipc/`, `fs/` — vendored namespace, SysV IPC, POSIX mqueue and
  overlayfs compatibility code (see `README.md` in this directory)

Everything outside `glue/` is vendored code kept close to its upstream
`kernel/common` layout for diffability; only `glue/` is hand-written.
They all build and load only as one module with the single entry point in
`glue/lkm4ctr_main.c`.

## Auto-loaded submodule

`insmod lkm4ctr.ko` brings up the shared hook engine, auto-loads
`vendor_kernel` (the module's only runtime-loadable subsystem now that
`shadow_cgdevices` has been removed and the hook engine has no separate
lifecycle of its own), and registers the `lkm4ctr` diagfs filesystem type.
Mount the diagfs to inspect state or manually unload/reload `vendor_kernel`
later:

```sh
mount -t lkm4ctr diag /mnt
cat /mnt/vendor_kernel/status          # active right after insmod
cat /mnt/vendor_kernel/namespaces
echo unload > /mnt/vendor_kernel/control
```

`vendor_kernel` exposes `control`, `status`, `log`, `hooks`, and its
live-state listing files directly under the mount root:

* `/mnt/global/{control,status,log,resources,references}`
* `/mnt/global/hotreload/{status,log,do-hot-reload}`
* `/mnt/helper.sh` (read-only, mode 0555, mount root)
* `/mnt/readme.txt` (read-only, mode 0444, mount root)
* `/mnt/vendor_kernel/{control,status,hooks,log,namespaces,msg,resources,references}`

`control` accepts `load`, `unload` (`remove`/`graceful` aliases), and
`forceunload` (`force`/`force_unload` aliases). `status` is read-only and
prints exactly one lifecycle state: `unloaded`, `loading`, `active`,
`graceful unloading`, or `force unloading`. The shared hook engine
(`shadow_hijack.c`) itself has no diagfs control surface: it is always
active for the entire lifetime of `lkm4ctr.ko`, started/stopped directly by
`lkm4ctr_main.c`.

`global` also exposes a `references` file that breaks `module_refcount()`
down into: the diagfs mount count, currently in-flight redirected hook
calls, and any remaining unaccounted "other" holders (most likely another
module that itself calls an `EXPORT_SYMBOL_GPL()` from `lkm4ctr.ko`). This
is meant to explain *why* an `rmmod`/unload attempt is stuck at "try again"
instead of just reporting that it is.

`rmmod lkm4ctr` (and `global/control`, below) always force-clean up
`vendor_kernel` if it is still active on the way out, so its own state can
never block module removal.

## rmmod safety

Every hooked call executed while a redirected call is in flight holds a module
reference for its duration (see `shadow_hook.h` and
`shadow_hijack.README.md`), so an ordinary `rmmod lkm4ctr` refuses to race
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
explicitly frees `vendor_kernel`'s resources, and finally launches a real
userspace `rmmod lkm4ctr` on its own. If in-flight calls don't drain within 30
seconds, the attempt is aborted, hooks resume normal operation, and the module
stays loaded. Writing `forceunload`/`force`/`force_unload` instead runs the
same sequence, except `vendor_kernel`'s resources are freed immediately after
quiescing rather than waiting until the very end.

Reading `./mnt/global/control` prints the accepted commands; reading
`./mnt/global/status` prints the current lifecycle state. `./mnt/global/log`
contains the combined log stream and `./mnt/global/resources` aggregates the
live resources still tracked by `vendor_kernel`.

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

lkm4ctr status                       # summarise global + vendor_kernel
lkm4ctr control vendor_kernel unload # write "unload" to vendor_kernel/control
lkm4ctr load                         # alias for: control global load
lkm4ctr forceunload vendor_kernel    # alias for: control vendor_kernel forceunload
lkm4ctr force2                       # alias for: control global force2
lkm4ctr logcat vendor_kernel         # cat vendor_kernel/log
lkm4ctr references global            # cat global/references
lkm4ctr hot-upgrade /path/to/new/lkm4ctr.ko
lkm4ctr help                         # full command list
```

Its contents are generated into `glue/helper.sh.c` as a plain C string
constant (`lkm4ctr_helper_sh_data`) and served verbatim by the diagfs; edit
the canonical script and regenerate that file rather than hand-editing the
C string. `./mnt/readme.txt` mirrors this in `glue/readme.txt.c`.

## Build

```sh
make -C /path/to/kernel/build M="$PWD/vendor" modules
```

For Android GKI, build against the matching DDK `vmlinux`/`Module.symvers`
sysroot exactly as described in `../README.md`.
