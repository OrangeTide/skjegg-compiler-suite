# The skjegg/boris hosting contract

This is the handoff contract between skjegg and the boris MUD server. boris
vendors skjegg's RV32 toolchain and freestanding runtime to host Kobold-Pascal
and C programs as sandboxed guests. The document records what boris asked skjegg
to provide, and what skjegg guarantees in return, so a re-vendor reads one
accurate contract rather than reconstructing it from two trees.

It has three parts: the original requirements (R1 to R8, resolved, kept as
history), the current seam contract (the authoritative part), and the status of
boris's follow-on wishlist.

## Definition of done

From a clean skjegg checkout, with no external trees and without an
already-working Kobold compiler, a downstream builds this pipeline:

```
hello.pas -> kpcom -> hello.kpm -> skj-kp-rv -> hello.s
          -> skj-as-rv -> hello.o -> skj-ld-rv -> hello.elf
```

and runs `hello.elf` on a host that supplies only a small, documented syscall
seam (write, read, a program break, exit), with `writeln('...')` output reaching
the host. skjegg supplies the whole freestanding runtime (heap, reference
counter, strings, Str/Val, console I/O, crt, ParamStr); the host supplies only
the seam.

This is proven in-tree by `make check-kp-free` (RV32, under qemu-riscv32) and
`make check-kp-free-cf` (ColdFire, under the in-tree skj-run), which run the
whole `tests/kp-src` corpus through the freestanding pipeline. The front end
runs in process rather than as a separate `kpcom` binary: `skj-kpc-rv` JITs the
committed kpcom seed and emits the module, so `.pas -> .kpm` needs nothing past
the skj tools.

## The seam contract (authoritative)

boris reaches the host with the RISC-V `ecall` instruction and keeps two
conventions at one `a7` selector. skjegg targets only the first; the second is
authored in boris's own guest SDK.

### The raw Linux RV32 seam (skjegg emits these)

A program built against the vendored runtime needs only the standard RV32 Linux
syscalls, kept at their real Linux numbers. boris's ask was explicit: keep the
Linux numbers, do not renumber them. The seam lives in `runtime/syscall_rv.S`
(the ColdFire twin is `runtime/syscall_cf.S` at the Linux/m68k numbers), with
`a0..a5` the arguments and `a0` the result (a negative Linux errno on failure,
passed through unchanged).

| Call           | a7  | Notes                                              |
|----------------|-----|----------------------------------------------------|
| `read`         | 63  | fd 0 for stdin; a file fd after `openat`           |
| `write`        | 64  | fd 1/2 for the session; a file fd after `openat`   |
| `openat`       | 56  | `open` is a 3-arg shim over `openat(AT_FDCWD, ...)`|
| `close`        | 57  |                                                    |
| `_llseek`      | 62  | rv32 has no simple lseek; 5-arg `_llseek`          |
| `getdents64`   | 61  | directory iteration                                |
| `statx`        | 291 | sized stat (no bare stat/fstat on rv32)            |
| `mkdirat`      | 34  |                                                    |
| `unlinkat`     | 35  |                                                    |
| `fchmodat`     | 53  | the linker marks its output executable             |
| `renameat2`    | 276 |                                                    |
| `clock_gettime`| 113 | the one clock (`std.time` `Now`)                   |
| `getrandom`    | 278 | the one RNG seed (`std.random`, `arc4random`)      |
| `nanosleep`    | 101 | `std.time` `Sleep`                                 |
| `sched_yield`  | 124 | cooperative yield                                  |
| `brk`          | 214 | the guest heap grows the program break             |
| `_exit`        | 93  | boris may alias `exit_group`=94 onto it            |

A runtime fault traps through `ecall` selector `0x000f0000` (`RV_SYS_FATAL`),
which is boris's `VM_SYS_TRAP`, so an abort looks the same on skj-run and in the
VM.

The entry and argv contract is the Linux initial stack: `argc` at the entry
`sp`, the `argv` pointers above it, a NULL, then `envp`. The shared
`runtime/crt_entry_rv.S` reads it, so C's `argv` and Kobold's
`ParamStr`/`ParamCount` are the same bytes. boris seeds the same stack.

### The name-bound host-call seam (boris owns these)

boris reaches its rich services (object and world access, the mailbox, the
authority calls) through the same `ecall`, with `a7 = 0x00100000 + import_index`
resolved by name from a `KPIMPT1` import table scanned out of the image. skjegg
does not emit this table: it is authored in boris's guest SDK (`sdk/vm/stubs.S`
and `imports.S`) as a few `.asciz` lines, which the skj assembler assembles
without a custom section or symbol table. A guest that stays on the raw seam
above needs none of it. skjegg must only avoid colliding with, or renumbering,
the raw seam, which this contract fixes.

## Requirements R1 to R8 (resolved at v0.12.0, history)

The original v0.8.0 handoff listed eight gaps. All were closed by v0.11.0, and
the follow-on `tail`-pseudo gap by v0.12.0:

- **R1** a buildable Kobold front end (source to KPM1), without an existing
  Kobold: the in-process kpcom seed path (the JIT runs a kpcom seed built from
  source; since 0.24.0 no seed is committed, see `kobold.md`).
- **R2 to R7** the shipped freestanding runtime: the platform layer (heap over
  `brk`, retain/release, trap over `_exit`, mem routines), the no-libc Str/Val,
  the freestanding console I/O, the crt and `kp_guest_entry` contract, ParamStr
  and ParamCount, and the rv32 i64/real helper and bridge set.
- **R8** `vendor.sh` selecting the Kobold AOT path as one set.
- The `tail` gap: `skj-as-rv` now encodes the `tail` pseudo, so the whole
  freestanding runtime assembles with the skj tools alone, with no external
  riscv32 gcc.

## The follow-on wishlist (2026-10-17): status

boris's wishlist, prompted by skjegg expanding its sandbox syscalls and unifying
the C and Kobold runtimes. Essentially all of it shipped in skjegg 0.23.x and
0.24.x.

### Resolved

- **B1 one syscall seam, not two.** Kobold's freestanding I/O and the C libc now
  link one seam (`runtime/syscall_{rv,cf}.S`). The wrappers are weak with
  `__sys_*` aliases; the fs layer interposes fd >= 3 through `fs_*` and routes
  0/1/2 to `__sys_*`, under `SKJ_FS_INTERPOSE`. boris can drop its `kp_sys_rv.c`
  shadow and bind one seam for both languages.
- **B2 non-faulting Str/Val.** The whole freestanding Str/Val surface reports a
  bad or out-of-range parse through a status, never a trap (`kobold/io_free.c`).
- **B3 one shared bottom layer.** crt, brk allocator, buffered stdio core,
  Str/Val, console I/O, number formatting, time/rng/yield glue, all shared and
  parameterized by the seam. See `doc/abi/README.md`.
- **B4 one argv/initial-stack contract.** The shared `crt_entry_{rv,cf}.S`, as
  above.
- **A1 `_llseek` and a sized stat.** `_llseek`=62 and `statx`=291; the file
  runtime sizes a file instead of over-allocating a fixed buffer.
- **A2 `getdents64`.** `getdents64`=61, with `opendir`/`readdir` (C) and the
  Kobold `dirs` unit over it.
- **A3 a brk-backed allocator.** `brk`=214 backs `sbrk`/`malloc` on the hosted
  ILP32 targets and the Kobold freestanding gheap.
- **A4 one clock and one RNG.** `clock_gettime`=113 and `getrandom`=278; both
  languages read one clock and one RNG (`runtime/rng.c`).
- **A7 errno convention.** The raw seam returns a negative Linux errno,
  unchanged.
- **C2 `writev` in the buffered output path.** The C stdio core coalesces a bulk
  `fwrite` (a block that will not fit the stream buffer) with the pending buffer
  into one `writev` on the freestanding tiers, instead of two syscalls.
  `writev`=66 is on the seam (`runtime/syscall_rv.S`; ColdFire #146 in
  `syscall_cf.S`), with `struct iovec` in `include/sys/uio.h`; the emulator's
  `GSYS_WRITEV` reads the iovec honoring guest byte order. The other targets fall
  back to two `write` calls. The deliberately unbuffered Kobold console
  (`kobold/kp_io.c`) is unchanged.

### Host-side (no skjegg change)

- **A5 a yield/sleep that parks.** skjegg maps `Sleep` onto `nanosleep` and
  exposes `sched_yield`. Whether a wait parks the task or spins is the host
  scheduler's behavior behind the seam, which boris owns; skjegg only issues the
  call.
- **A6 blocking-read vs EOF.** skjegg keeps Linux-faithful read semantics: a
  zero-byte `read` on fd 0 is end of input, and the Kobold console latches it as
  a sticky EOF sentinel (`kobold/kp_io.c`; the sentinel is deliberate because
  Kobold cannot detect an unhandled return). boris's cooperative scheduler, which
  wants a zero-length return to mean "would block, retry after a park" with real
  end-of-input only on an explicit close, must not return 0 from `read` unless
  the stream is truly closed: it parks the task and resumes it when input
  arrives, so the guest only ever sees a blocking read that eventually yields
  bytes or a genuine EOF. The distinction lives in the host, not in skjegg's
  guest runtime.
- **C1 a feature/version probe.** skjegg's Kobold runtime exports a readable
  `kp_guest_abi_version` (currently 3; `kobold/kp_abi.h`), so a host can check
  the entry-ABI version. A per-call `sys_probe(name) -> bool` is a name-bound
  host-call in boris's SDK seam, not a raw-seam skjegg concern.

## Notes for the skjegg maintainers

- boris does not use skjegg's JIT, its hosted runtime, or the coroutine
  machinery. It uses the AOT RV32 path plus the freestanding runtime that binds
  to the raw seam above.
- The raw seam numbers are an ABI boris depends on. Do not renumber them. Adding
  a call is fine; changing an existing number breaks a vendored guest.
- The end-to-end guarantee is `check-kp-free` / `check-kp-free-cf`. Keep them
  green on any change to the seam, the crt, or the freestanding runtime.
