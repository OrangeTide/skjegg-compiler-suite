# The skj-run process model

This document describes the process model skj-run is growing toward: a small
unix-like system where each process runs in its own sandbox, and a userland of a
shell (`jsh`) and small tools is spawned on top of it. It records the design and
the host-call ABI, and marks what is implemented today versus planned, so the
pieces can land one at a time without the contract drifting.

Status is marked inline: **(done)** ships today, **(planned)** is designed here
but not yet built.

## One sandbox per process

`skj-run` runs a root process and lets a guest spawn children. Every process has
its own CPU, its own guest memory, its own heap and stack, its own open-file
table, and its own mount table. A child shares no memory with its parent. A
round-robin scheduler (`emu/sched.c`) runs the live processes a `SKJ_QUANTUM` of
instructions at a time and reaps each as it exits, halts, or faults. One copy of
the scheduler serves every core through the `skj_cpu_ops` vtable.

Isolation is the default, not an afterthought. A freshly spawned process comes up
with its own empty scratch and nothing else, so it reaches the host only
through a capability its parent deliberately hands it. This is the opposite of
`fork`: nothing is inherited unless it is passed.

A process **handle** is a table slot plus a generation, so a handle kept past a
child's death never matches a later child that reuses the slot.

## The spawn request

Spawn takes a size-prefixed, extensible descriptor in guest memory. The host
reads the leading `size` (the byte count the guest filled) and honors only the
fields that fit, so later fields are added without a new selector and an older
guest that fills fewer fields simply gets host defaults for the rest. Every word
is a 32-bit value in the guest's byte order.

```
offset  field       meaning                                   status
0       size        bytes the guest filled                    done
4       image       guest address of the ELF image            done
8       image_len   its length (<= 16 MiB)                     done
12      argv        guest address of argc string pointers      done
16      argc        argument count                            done
20      flags       SKJ_SPAWN_FLAG_SHARE_MOUNTS, ...           done
24      fdmap       guest address of {gfd, dst} fd-map pairs   done
28      fdmap_n     number of fd-map entries                   done
```

The fields past `argc` are present when the request reaches
`SKJ_SPAWN_WITH_FDS` bytes; an older guest that fills only through `argc` gets
host defaults (its own `0/1/2`, no shared mounts). Environment passing is not a
descriptor field: a process holds its own environment, and the shell has no
cross-process `setenv` to propagate.

The host-side marshalling is bounded: at most `SKJ_SPAWN_MAX_ARGC` arguments,
`SKJ_SPAWN_ARG_MAX` bytes each, `SKJ_SPAWN_ARGBUF` total. A read out of mapped
memory fails the spawn cleanly rather than faulting the caller.

## Lifecycle host calls

These sit in the emulator-private selector space (see "How guest syscalls work"
in `emulator.md`); every core routes them to the shared `sched_private` handler,
so one build serves all five targets.

| selector | value | meaning | status |
|---|---|---|---|
| `SKJ_SYS_SPAWN`  | `0x000f0003` | spawn from an image pointer + length (no argv) | done |
| `SKJ_SYS_SPAWNV` | `0x000f0006` | spawn from the size-prefixed request above | done |
| `SKJ_SYS_WAIT`   | `0x000f0007` | wait on a child handle, return its status, reap it | done |
| `SKJ_SYS_KILL`   | `0x000f0008` | terminate a child handle with a given status | done |
| `SKJ_SYS_PIPE`   | `0x000f0009` | create a host-backed pipe, return two handles | done |

`SKJ_SYS_WAIT` parks the caller if the child is still running, reusing the
scheduler's existing blocked-process path, and wakes it when the child exits. A
child that has exited but not been waited on is kept as a zombie (its resources
freed, its status retained) until a wait reaps it or its parent is gone.
`SKJ_SYS_KILL` reaches only the caller's own children, matching the capability
model: there is no system-wide process namespace, and a "pid" is a spawn handle.

## Arguments and environment

The initial stack frame the loader builds already carries the full Linux-style
layout: `argc`, the `argv` pointers, a NULL, the `envp` pointers, a NULL, then a
minimal auxv. Argument passing through spawn fills the `argv` vector; environment
passing fills `envp` the same way **(planned)**. The crt delivers `argc`/`argv`
(and sets `environ` for `getenv`) to `main` **(in progress; the rv crt dropped
them historically)**.

A process holds its own environment. There is no shared global environment and no
cross-process `setenv`: a shell keeps its own `environ`, mutates it with
`export`, and passes a copy to each child it spawns.

## File descriptors, streams, pipes, and redirection

`fs/` keeps one open-file table per process (`fd_tab`). There are no special
descriptors: 0, 1, and 2 are ordinary stream slots (`FD_STREAM`), the same kind
a pipe end or an inherited descriptor binds. `fs_init` binds them to the
sandbox's own stdin/out/err (gated on `FS_WITH_STREAMS`, so only a guest build
has them; the hosted `fs_test` does not). A stream slot reaches the host
directly through the raw crt seam, bypassing the mount backend, so it is not
seekable (`lseek` reports `ESPIPE`); closing it closes the host fd, since the
sandbox owns it. The libc routes every op through `fs_*`, with no `fd < 3`
shortcut, so 0/1/2 travel the same path as every other fd.

Each sandbox gets its **own** `dup` of the real 0/1/2 when its capability table
is first used, so there is nothing shared to protect: a guest closing its fd 1
closes only its own dup, never the terminal, and process teardown closes every
host fd the sandbox still holds (which is also how a writer's exit closes a pipe
and the reader sees EOF).

A **pipe** is `SKJ_SYS_PIPE`: the host creates a host pipe and hands the sandbox
two capability indices, one per end, which the libc `pipe()` adopts as two
stream fds. Because there is no `fork`, there is no `dup2`; descriptors are
handed to a child **at spawn** through an fd map: `{gfd, dst}` pairs past `argc`
in the spawn request (a flags word, the map pointer, and its count, present when
the request reaches `SKJ_SPAWN_WITH_FDS`). The guest resolves one of its fds to
the capability index backing it (`fs_fd_backing`) and names the id the child
should see it as. The host dups that resource into the child and records the
child's `fs_inherit_fds` table for its `fs_init` to install. That is how a pipe
end or a redirect reaches a child; with no map the child just gets its own
0/1/2. A redirect to a file is the same mechanism once the target is a
host-backed fd.

## Passing capabilities: the mounts

A capability in this model is a mount: a `struct fs_mount` entry plus the backend
handle its root fd names. The `SKJ_SPAWN_FLAG_SHARE_MOUNTS` request hands a child
the parent's namespace. With the flag unset the child still gets its own scratch
(a fresh per-sandbox host directory `skj-run` grants, see below) and nothing the
parent held.

Delivery is **host-mediated**, not a guest-side table copy, for two reasons.
The guest's `fd_tab` is a `static` the host cannot read by symbol, and the
guest's live `fs_mounts` is post-adoption (its `fd` fields are `fd_tab` slot
indices, not the capability indices a fresh `fs_init` expects). So the host
keeps the inheritable form itself: each process records its installed mounts as
a `guest_grant` list as it receives them, holding each host-backed grant's real
resource (a `FS_KIND_HOST` directory fd). At spawn the host replays that list
into the child, resolving the child's `fs_mounts` from its in-memory ELF image,
dup'ing each host dir fd so the child owns an independent capability, and writing
the child's table in launcher form for the child's own `fs_init` to adopt
unchanged.

Only **host-backed** capabilities can be shared this way: hostcap (a host
directory fd) and devfs (stateless), with pipe ends to follow. Their backing is
a host resource the host can replicate into the child. A ramfs, romfs, or 9P
client keeps its backend state in the parent's guest memory (or a server bound to
the parent), which the child does not share, so those mounts stay private to the
process that holds them; they are recorded for an accurate count but not yet
inherited.

### Host-backing the filesystem where a host is present **(done)**

When a host is present, as it always is under `skj-run` (the emulated tiers and
the KVM/HVF ones alike), the host can own the filesystem and the guest needs
almost no `fs/` code. The backends were most of a guest binary's size, so this
is the main lever on guest size, and it is now the default for the `skj-run`
userland. It is a per-deployment choice, not a wholesale conversion: the
in-guest backends stay available for the hosts that need them.

The mechanism is the capability-fd path that already existed for a `--mount`
host directory (`hostcap`): the guest issues ordinary `openat`/`read`/`write`/
`statx`/`getdents` on an opaque directory fd, and `skj-run` services them
against the real host fd it holds. Two default mounts move onto it:

- **scratch**, the spot a bare path resolves against, is an ephemeral host
  directory `skj-run` grants per sandbox (`mkdtemp`, removed at teardown). Each
  process gets its own, so it is as private as the in-guest ramfs it replaced,
  with no cross-process shared state.
- **bin:**, the tool images `jsh` reads, is served as a read-only host directory
  (`--mount-ro`, which sets `FS_RO`). One host copy serves `jsh` and every
  child, instead of a romfs image parsed into each guest's memory.

The in-guest ramfs and romfs are kept behind `FS_WITH_RAM` and `FS_WITH_ROM` for
the cases that still need them, and are compiled out of the `skj-run` build:

- The standalone VM hosts. `fs/` runs *in the guest* so the Excelsior ColdFire
  and RISC-V hosts, which run the guest as the operating system with no host
  underneath, have a filesystem at all. Those deployments keep the in-guest
  ramfs and romfs; a bare-qemu guest likewise lands on the in-guest ramfs.
- The cost of host-backing is a doorbell round trip per `open`/`read`/`write`/
  `stat`, where an in-guest ramfs read is a memory access. That is fine for
  scratch and for loading a program image, which is why those move; a backend on
  a hot inner loop would not.

So the `fs/` switch, a vtable over backend kinds, carries both forms, and the
deployment chooses by what it links and what the launcher grants. This went
through the capability-fd mechanism rather than a new 9P-style served kind, so
it needs no 9P and no round-trip batching for scratch or bin.

## The userland **(done)**

The userland that rides on this is a shell, `jsh`, and a set of small tools
(`cat`, `echo`, `ls`, `rm`, `sleep`, `cp`, `mv`), reimplemented in C over
skjegg's libc and the `fs/` layer. They live under `examples/` (`examples/jsh`
and `examples/bin`) and run on every tier `skj-run` hosts, the emulated RISC-V
and ColdFire cores and the KVM/HVF x86-64 and arm64 guests, with `skj-run`
owning scratch and `bin:` host-side so the guest links only the `fs/` switch and
the hostcap adapter.

`jsh` is a Bourne-subset shell (variables, `if`/`while`/`for`/`case`, functions,
`&&`/`||`, pipelines, redirection, command substitution). Its parser and
execution are shared with the standalone build; only the process seam is
retargeted here. The shell resolves a command name to a program image: a bare
name is read from the `bin:` mount, which `skj-run` grants as a read-only host
directory of the tool images (`--mount-ro`). It reads the image into a buffer,
spawns a fresh sandbox for it with argv and an fd map, and waits on the handle.

Because a child inherits no descriptors, pipelines and redirection build the fd
map rather than mutating the shell's own fds. A redirect opens the target in the
shell and places the opened fd at the child's `0`, `1`, or `2`; a pipeline
creates a host pipe per stage boundary and places each end in the adjacent
stage's map. With `SKJ_SPAWN_FLAG_SHARE_MOUNTS` the children see the shell's
namespace, so a tool can reach the same files the shell can.

`kill` is a shell builtin, not one of the external tools: a spawn handle is
private to the process that holds it (there is no system-wide process
namespace), so only the shell that spawned a child can name it. `kill HANDLE`
calls `SKJ_SYS_KILL` with the handle the shell printed when it backgrounded the
job.

## See also

- `emulator.md`, "The process sandbox" and "How guest syscalls work", for the
  scheduler, the batched-run reason contract, and the selector mechanics.
- `emulator.md`, "The filesystem layer (`fs/`)", for mounts, the fd space, and
  the guest-versus-host split.
