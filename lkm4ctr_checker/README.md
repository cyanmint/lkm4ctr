# lkm4ctr_checker

`lkm4ctr_checker` is a **plain userspace diagnostic binary** — it is
**not** a kernel module and installs no `/dev` node. It performs the same
kind of system calls `runc`'s `nsexec` does (`unshare(2)`/`fork(2)`/
`setns(2)`, plus `mq_*`/`msg*`/`mount(2)` for the other container-relevant
subsystems) and reports, for each feature, whether it observed:

* **PASS** — the syscall succeeded *and* a real, verifiable side effect
  proves genuine isolation (e.g. a child's hostname change after
  `unshare(CLONE_NEWUTS)` is never visible to the parent).
* **STUB** — the syscall succeeded, but no isolation was actually
  observed: a "bookkeeping-only" fallback accepted the flag/call and did
  nothing behind it.
* **FAIL** — the syscall itself failed (`EINVAL`/`ENOSYS`/`EPERM`/…). This
  is exactly the failure class that surfaces to a container runtime as
  errors like:

  ```
  failed to create task for container: failed to create shim task: OCI
  runtime create failed: runc create failed: unable to start container
  process: can't get final child's PID from pipe: EOF; runc init error(s):
  nsexec-1[19437]: failed to unshare remaining namespaces: Invalid
  argument; nsexec-0[19436]: failed to sync with stage-1: next state (got
  0 of 4 bytes): unknown
  ```

  If `lkm4ctr_checker` reports **FAIL** for `ns_uts`/`ns_pid`/`ns_ipc`/
  `ns_net`/`ns_user`/`ns_mnt`/`ns_cgroup`, that is the concrete, reproducible
  root cause of a `runc`/`nsexec` "failed to unshare remaining namespaces"
  error for that namespace type: the kernel refused the exact syscall
  `nsexec` needs, on this device, right now — independent of whatever
  `lkm4ctr.ko` happens to be loaded. A prior revision of
  this tool could only report compile-time `IS_ENABLED(CONFIG_*)` facts,
  which cannot detect this: a kernel can be built with `CONFIG_*_NS=y` and
  still refuse the syscall at runtime (e.g. a container/sandbox that itself
  lacks `CAP_SYS_ADMIN`, a seccomp filter, an LSM policy, or — on some
  Android GKI builds — the syscall wrapper being present but a namespace
  subsystem it depends on being incompletely wired up). Only actually
  attempting the syscall, from the exact environment `runc` will run in,
  tells you the truth.

Every syscall requiring `CLONE_NEW*` is attempted inside a throwaway
`fork(2)`'d child (and further forked grandchildren where needed, e.g. for
the PID-namespace test), so a `FAIL`/`STUB` result never disturbs this
process's own namespaces, mounts, or hostname.

## Usage

```sh
./lkm4ctr_checker            # human-readable report on stdout
./lkm4ctr_checker -q          # no report; exit status only
echo $?                          # 0 = no FAILs, 1 = at least one FAIL
```

Run it as `root` (or with `CAP_SYS_ADMIN`) — the same privilege level
`runc`/`containerd` itself runs `nsexec` with — for a meaningful result;
unprivileged `unshare(2)` calls will legitimately report `FAIL` with
`EPERM`, which is expected and not a bug in the checker.

Example output (privileged, on a kernel with full native namespace support):

```
lkm4ctr_checker v4.0 - userspace container-isolation diagnostics
PASS = real isolation observed, STUB = bookkeeping only (no real isolation), FAIL = syscall itself failed
--------------------------------------------------------------------------------------------------------
ns_uts (UTS)                 PASS  child set 'shadowchk-4733', parent still 'myhost'
ns_pid (PID)                 PASS  grandchild became pid 1 in new namespace
ns_pid (kill)                PASS  kill(2) targeting a namespace-local pid delivered SIGUSR1 correctly
ns_pid (pgid/session)        PASS  pid/pgid/sid of the namespace's own init all report 1
pidfd_open                   PASS  pidfd became readable on child exit
ptrace                       PASS  PTRACE_TRACEME/PTRACE_CONT round-tripped successfully
wait4/waitid                 PASS  wait4() and waitid() both correctly reaped pid/exit status
ns_user (USER)               PASS  uid remapped 1 -> 65534 inside namespace
ns_user (id mapping)         PASS  getuid/getgid/getresuid/getresgid all report mapped id 0 (real 1/1)
ns_ipc (IPC)                 PASS  namespace id changed (ipc:[4026531839] -> ipc:[4026532267])
ns_net (NET)                 PASS  namespace id changed (net:[4026531833] -> net:[4026532268])
ns_mnt (MNT)                 PASS  namespace id changed (mnt:[4026531832] -> mnt:[4026532327])
ns_cgroup (CGROUP)           PASS  namespace id changed (cgroup:[4026531835] -> cgroup:[4026532327])
mqueue (POSIX)               PASS  message round-tripped through the queue
mqueue (mq_getsetattr)       PASS  mq_getattr/mq_setattr round-tripped maxmsg=4 and O_NONBLOCK
sysvipc (SysV msg)           PASS  message round-tripped through msgget/msgsnd/msgrcv
sysvipc (SysV sem)           PASS  semget/semctl(SETVAL,GETVAL)/semop round-tripped a decrement
sysvipc (SysV shm)           PASS  shmget/shmat/shmdt round-tripped a write through shared memory
overlay2                     PASS  mount(2) of an overlay filesystem succeeded
--------------------------------------------------------------------------------------------------------
summary: 0 FAIL, 0 STUB (bookkeeping-only)
```

## What it checks, and how

| Line                  | Test methodology |
|-----------------------|-------------------|
| `ns_uts (UTS)`        | Fork a child, child `unshare(CLONE_NEWUTS)` then `sethostname()`s a unique name; PASS only if the **parent's own** `gethostname()` never observes that change. |
| `ns_pid (PID)`        | Fork a child that `unshare(CLONE_NEWPID)`s, then forks a grandchild; PASS only if the grandchild's own `getpid()` is `1` (a genuinely new, empty pid namespace always numbers its first task 1). |
| `ns_pid (/proc isolation)` | From inside the pid-namespace grandchild above (after mounting a fresh `/proc`): `/proc/<own vpid>` must open, the host's real pid must be hidden from `open()`, and `readdir("/proc")` must not list it. |
| `ns_pid (/proc stat content)` | From a fresh pid-namespace init (after calling `setsid(2)` for the same reason as `ns_pid (pgid/session)`, then mounting a fresh `/proc`): reads `/proc/self/stat` and `/proc/self/status` and parses the `pid`/`ppid`/`pgrp`/`session` fields and `Pid:`/`PPid:` lines; PASS only if **all** of them report the virtualized values (`pid=1 ppid=0 pgrp=1 session=1`), directly exercising `glue/vendor_kernel_procfs.c`'s procfs content rewriting rather than just checking directory-entry visibility. |
| `ns_pid (kill)`       | A grandchild inside a fresh `CLONE_NEWPID` namespace announces its own (namespace-local) pid and blocks in a signal handler loop; the namespace's "init" targets that exact pid with `kill(2)`. PASS only if the signal is actually delivered — exercises vendor_kernel's vendored PID-translation path with a real, observable effect, not just a `0` return. |
| `ns_pid (pgid/session)` | From the namespace's own pid-1 task: `setsid(2)` (making it the leader of a brand new process group/session, needed since the pre-namespace group/session's leader lives outside the new namespace and would otherwise legitimately read back as `0` even with a genuine remap) then `getpgid(0)`/`getsid(0)` should both report `1`, consistent with the `ns_pid (PID)` vpid remap. |
| `pidfd_open`          | `pidfd_open()` a real child, then `poll(2)` the pidfd for `POLLIN`; PASS only if it fires exactly when the child exits. |
| `ptrace`              | `PTRACE_TRACEME` in a child + `raise(SIGSTOP)`, then the parent `PTRACE_CONT`s it; a full accept/attach/continue round trip. |
| `wait4/waitid`        | Two throwaway children with distinct exit codes, reaped via `wait4()` and `waitid(P_PID, ..., WEXITED)` respectively; PASS only if both report the correct pid *and* exit status. |
| `ns_user (USER)`      | Fork a child that `unshare(CLONE_NEWUSER)`s; PASS if `geteuid()` inside changed from the caller's real (non-root) uid (either remapped to `0`, matching a docker-style single mapping, or to the kernel's overflow uid `65534` when unmapped) — both are real, observable isolation. If the checker itself is running as uid `0` (where a remap-to-0 can't be told apart from "not remapped, still 0"), it instead forks a child, `setuid(1)`s it, and re-runs this same test as uid `1` so it stays conclusive instead of being `SKIP`ped. |
| `ns_user (id mapping)` | Same `unshare(CLONE_NEWUSER)`, but then writes an explicit `"0 <real> 1"` mapping to `/proc/self/uid_map`/`gid_map` (denying `setgroups(2)` first, exactly like every container runtime does) and checks that `getuid`/`getgid`/`getresuid`/`getresgid` **all** report the mapped id `0` — a strictly stronger check of vendor_kernel's vendored user-namespace credential paths than `ns_user (USER)`'s single `geteuid()` sample. Same uid-`0` → `setuid(1)`-and-retry fallback as `ns_user (USER)` above. |
| `ns_ipc`              | Fork a child that `unshare(CLONE_NEWIPC)`s, then compares `readlink("/proc/self/ns/ipc")` between parent and child (an appearing/changing `ipc:[id]` counts as PASS even if the "before" readlink itself failed with `-ENOENT`, which happens whenever `CONFIG_IPC_NS` is genuinely absent and no real `/proc/<pid>/ns/ipc` entry exists at all). This is now backed by vendor_kernel's own real isolation path: `glue/vendor_kernel_procfs.c` fabricates the distinct `/proc/<pid>/ns/ipc` entry when needed, and the vendored SysV IPC/mqueue paths operate on a namespace-scoped `ipc_namespace` instead of the host default — see [`../vendor/README.md`](../vendor/README.md). |
| `ns_net`/`ns_mnt`/`ns_cgroup` | Fork a child that `unshare()`s the corresponding `CLONE_NEW*` flag, then compares `readlink("/proc/self/ns/<type>")` between parent and child. A genuinely new kernel namespace object always gets a distinct `type:[inode]` id; an accepted-but-inert fallback leaves it unchanged. These three have no cheap additional functional test the way UTS/PID/USER/IPC do — see [`../vendor/README.md`](../vendor/README.md) for the current NET/MNT/CGROUP caveats. |
| `mqueue (POSIX)`      | Real `mq_open()` + `mq_send()` + `mq_receive()` round trip of an actual payload, not just checking `mq_open()`'s return value. |
| `mqueue (mq_getsetattr)` | `mq_getattr()`, then `mq_setattr()` to flip `O_NONBLOCK`, then `mq_getattr()` again; PASS only if both `mq_maxmsg` and the new `O_NONBLOCK` flag are reflected back. |
| `sysvipc (SysV msg)`  | Real `msgget()` + `msgsnd()` + `msgrcv()` round trip of an actual payload. |
| `sysvipc (SysV sem)`  | `semget()` + `semctl(SETVAL, 1)` + `semop()` (decrement by 1) + `semctl(GETVAL)`; PASS only if the value actually reaches `0`. |
| `sysvipc (SysV shm)`  | `shmget()` + `shmat()` + write a payload + `shmdt()` + read it back through a second `shmat()`-free `memcpy()`; PASS only if the payload round-trips. |
| `overlay2`            | A real `mount(2)` of an `overlay` filesystem (lower/upper/work dirs under a fresh `mkdtemp()`), performed inside a private `unshare(CLONE_NEWNS)`'d mount namespace so nothing is ever left mounted on the host — cleaned up with `umount2(MNT_DETACH)` regardless of the result. |

## Why this replaced the old kernel-module version

Earlier revisions of `lkm4ctr_checker` were a small out-of-tree kernel
module (`lkm4ctr_checker.ko`) exposing a read-only `/dev/lkm4ctr_checker`
character device whose `cat`'able report was built purely from compile-time
`IS_ENABLED(CONFIG_*)` checks. That could only ever answer "was this kernel
*built* with this feature configured on" — never "does this actually work,
right now, in the environment `containerd`/`runc` will run in". A kernel can
have `CONFIG_UTS_NS=y`/`CONFIG_PID_NS=y`/… (as this project's
`CONTAINERD_CONFIG` in `kernel_builder.py` forces) and container start can
still fail with `unshare(2)` returning `EINVAL`/`EPERM` for reasons the
Kconfig snapshot alone cannot see (missing capabilities, seccomp/LSM
restrictions, a namespace subsystem that's compiled in but not fully wired
up, etc.). A plain userspace binary that just makes the syscalls and checks
their *effects* answers the real question directly, needs no `insmod`, and
requires no kernel-module build toolchain (DDK image, `Module.symvers`,
matching KMI, …) at all — it is a normal C program buildable with any C
compiler for the target's libc/ABI.

## Build

```sh
make                                   # host toolchain
make CC="clang --target=aarch64-linux-gnu"   # cross build for arm64
make LDFLAGS=-static                   # static binary (handy for a bare
                                        # ramdisk/rootfs with no shared libc)
```

Produces a single `lkm4ctr_checker` executable; no kernel headers,
`KDIR`, `Module.symvers`, or DDK image are required — this is unrelated to
the rest of the `lkm4ctr` module family's kernel-module build pipeline
(see `../README.md`), and can be run on any device (rooted or with
`CAP_SYS_ADMIN`) independent of whether any `lkm4ctr.ko` subsystem is
loaded.
