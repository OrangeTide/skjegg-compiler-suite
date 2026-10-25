# The shared freestanding runtime seam

This directory documents the bottom layer the C libc crt and the Kobold
freestanding runtime share, so a host (boris is the reference) sees one contract
rather than two. The freestanding tier is ColdFire and RV32 only; the other
targets are hosted dev and test. The matching host asks are B1 to B4 in
`doc/skjegg-kobold-requirements.md` (vendored into boris).

The pieces below were unified across three steps: one entry crt, one number
formatter, then the ColdFire time/rng/yield seam. Each is independently
testable and ships in both the C driver libraries and the Kobold freestanding
archives (`libkpaot-{rv,cf}-free.a`).

## The syscall seam

`runtime/syscall_rv.S` and `runtime/syscall_cf.S` are the one seam both
languages link. Each wrapper is a single trap (`ecall` on RV32, `trap #0` on
ColdFire) with the Linux call number and register-bound arguments, and returns
a negative Linux errno on failure, passed through unchanged (B1, A7). The C crt
(`runtime/start_rv.S`, `start.S`) and the Kobold crt
(`kobold/kp_crt_{rv,cf}.S`) both `.include` the seam, so the wrappers are
written once.

The calls on the seam: `read`, `write`, `open`, `close`, `lseek`,
`getdents64`, the `*at` file-metadata and path calls (`statx`, `mkdirat`,
`unlinkat`, `renameat2`, `fchmodat`), `clock_gettime`, `getrandom`,
`nanosleep`, `sched_yield`, `__sys_brk`, and `_exit` (with a weak `exit`). The
`*at` calls back both the hosted libc (`libc/stat.c`, `libc/fileops.c`) and the
Kobold freestanding `io.files` metadata/path operations, so they live on the
shared seam rather than in the crt. The RV32 numbers already match boris
exactly; the ColdFire numbers are the
Linux/m68k ones. `RV_SYS_FATAL` is `0x000f0000`, boris's `VM_SYS_TRAP`, so a
runtime fault looks the same on skj-run and in the VM.

## The entry and argv contract (B4)

`runtime/crt_entry_{rv,cf}.S` is the shared `_start`: it reads `argc` and the
argv vector at the entry stack pointer, switches to the larger runtime stack,
runs the finalizer, and exits. The only language-specific piece is the entry
symbol it calls (`main` for C, `kp_guest_entry` for Kobold) and whether the
arguments reach `main(argc, argv)` or `kp_set_raw_args`. So Pascal's `ParamStr`
and C's `argv` are the same bytes.

## The heap (A3)

Both heaps grow the program break through the one `__sys_brk` wrapper:
`libc/sbrk.c` on the hosted ILP32 targets and the Kobold freestanding gheap
(`kobold/rt_free.c`) on the freestanding tier. The crt reserves no fixed heap
region and `kp_rt_init` is a no-op.

## Buffered stdio

`libc/stdio_core.c` is the one buffered-stream core (the three standard streams
and the character and block I/O over them), built on the `write`/`read` seam
with no malloc and no format engine. It serves C programs on the freestanding
tier. The Kobold console does not use it: it writes and reads the `write`/`read`
seam directly from `kobold/kp_io.c`, keeping one byte of input pushback rather
than a stream buffer.

## The number formatter

`libc/numfmt.c` holds the two libgcc-free formatters both runtimes call:

- `__fmt_udec` writes an unsigned value's decimal digits, dividing by ten with
  16-bit-at-a-time 32-bit divides, so it needs no 64-bit divide helper.
- `__fmt_greal` renders a finite non-negative double like C's `%g`, extracting
  digits with double-to-int steps so it emits no `double`-to-`u64` conversion
  (`__fixunsdfdi`), which the RV32 freestanding tier has no libgcc for.

The C libc printf `%g` path and the Kobold `io_write_int` / `io_write_real` /
`kp_str_from_int*` all route through these. The libc `%f` and `%e`
decomposition is unchanged; it is hosted only.

## Non-faulting Str and Val (B2)

The whole freestanding `Str`/`Val` surface (`kobold/io_free.c`) reports a bad or
out-of-range parse through a status, never a trap. The integer scans do bounded
arithmetic with a nonzero constant base and set a 1-based error offset;
`kp_str_valcode` returns it; the real scan routes to libc's lenient `strtod`
over a bounded copy; the formatters divide only by nonzero constants. So a guest
can parse untrusted input without a fault.

## Reals on the RV32 soft-float tier

A real crosses a call the way the compiler's own `io_*_real` builtins do: in a
float register on ColdFire, and as its 64-bit bits in an integer register pair
on RV32 soft-float, which the backend emits directly for a freestanding native
call. No bridge is needed there. The `brv_*` bits-bridge in `io_free.c` and
`kp_host.c` is for the hosted JIT/AOT value-stack FFI only, which cannot carry a
double through its generic integer-slot dispatch.

## Time, RNG, and yield

`runtime/rng.c` holds the generators (splitmix32 seeding xoshiro128** for the
fast path, ChaCha20 for the CSPRNG, all 32-bit so no 64-bit helper is pulled on
a 32-bit guest). `runtime/rng_glue.c` and `runtime/sched_glue.c` are the
guest-facing glue over the `clock_gettime` / `getrandom` / `nanosleep` seam:
`kp_now_ms`, `kp_random_seed/randomize/real/below`, `kp_secure_bytes`,
`kp_secure_below`, `kp_sleep_ms`. They are reached through the FFI, the way
`ReadFileStr` is, and ship in both freestanding archives.

The C library draws from the same `runtime/rng.c` generators, so a C guest and a
Kobold guest behave alike. `rand`/`srand` (`libc/rand.c`) drive the xoshiro128**
fast generator; they stay deterministic as the C standard requires (seeded to 1
on first use, `srand` pins the sequence), so they do not touch host entropy. The
`arc4random` family (`libc/arc4random.c`, declared in `<stdlib.h>`) draws from
the ChaCha20 CSPRNG and seeds itself once from the `getrandom` seam, so it needs
no seed API. `rng.o` ships in the `libc.a` archives; `arc4random` links only
where the `getrandom` wrapper does (the RV32 and ColdFire crts), so a hosted
target that never calls it needs no wrapper.

## The shipped units

Kobold exposes the seam through units a program `uses`, not compiler builtins,
so none of this needs a front-end change or a seed regeneration
(`doc/languages/kobold/kp-units.md`):

- `io.console`: `Eof`, `ReadLine`/`ReadChar`/`ReadReal`/`ReadInt`, and
  `WriteStr`/`WriteLine`/`WriteChar`/`WriteReal`/`WriteInt`.
- `std.time`: `Now` (monotonic milliseconds) and `Sleep`.
- `std.random`: `SeedRandom`/`Randomize`/`RandomBelow`/`RandomReal` and
  `SecureRandom`.

## The emulator

`emu/guest.c` implements the host side of each call once; `emu/rv_user.c` and
`emu/cf_user.c` map each arch's Linux call numbers onto it. A result written to
guest memory honors the guest's byte order (`store64` and the `be` flag on
`gm_store`), so statx, getdents64, and the timespec read back correctly on the
big-endian ColdFire guest as well as little-endian RV32.

## Tests

- `check-kp-free`, `check-kp-free-cf`: the Kobold freestanding pipeline, source
  to a static ELF, run under qemu-riscv32 and skj-run.
- `check-kp-stdlib-free`, `check-kp-stdlib-free-cf`: the shipped units
  (`std.random`, `std.time`, `io.console`) on both tiers.
- `check-libc` (all five arches), and `test-rv-stat`/`-fileops`/`-dirs` plus
  `test-cf-stat`/`-fileops`/`-dirs`: the libc file operations, comparing skj-run
  against the real kernel under qemu.
- `test-rng-glue`, `test-sched-glue`: the glue, host-tested against known
  vectors with a stubbed seam.
- `check-libc` phase F covers the C `rand`/`srand` over the shared fast
  generator (deterministic, every arch); `test-rv-arc4random` /
  `test-cf-arc4random` cover the C `arc4random` family over the shared CSPRNG on
  the two freestanding tiers, self-checking under both skj-run and qemu.
- `test-rv-selfhost-kpc`, `test-cf-selfhost-kpc`: the compiler self-compiles
  in-guest, byte-matching the host build.
