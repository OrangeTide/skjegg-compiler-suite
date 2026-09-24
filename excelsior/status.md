# Excelsior: implementation status and roadmap

Status: living (2026-09-21). This note is the code-side view of Excelsior:
what is built, what runs where, and what is next. It is the counterpart to
`backlog.md` (the design-pass queue) and `core.md` (the design). For per-feature
detail the authoritative sources stay each note's own `Status:` line, the
`core.md` index, and the Excelsior section of the top-level `CLAUDE.md`, which
tracks IR lowering in full. This note consolidates; it does not replace them.

Excelsior is the sixth front end: a statically typed, class-based, in-world
scripting language forked from MooScript. It shares the toolkit's IR and
backends, and adds a two-layer guest runtime (`libexc.c` plus a host binding),
grounded in `host-abi.md`.

## What runs today

The pipeline is the standard one: reader to resolver to type checker to IR
lowering to a backend. The **front end is complete**: the reader, name
resolver, and type checker cover the whole language surface.

**Backend reach.**

- **End to end** (compile, assemble, link, run):
  - **ColdFire** (`skj-exc`, `make check-exc`, qemu-m68k). The original tier.
  - **RISC-V RV32**, platform ABI (`skj-exc-rv`, `-DCC_PSABI`, `make
    check-exc-rv`, qemu-riscv32). Links a gcc-built guest runtime. The tier is
    split by the emulator's F-only floating point: qemu-riscv32 runs anything
    with a `double`, `skj-run` runs the rest (see `../doc/emulator.md`).
  - **MIPS I o32**, platform ABI (`skj-exc-mips`, `make check-exc-mips`,
    qemu-mipsel).
- **Codegen only** (compile and assemble, no run): **x86-64** and **AArch64**
  (`skj-exc-x86-64` / `skj-exc-arm64`, `make check-exc-x86-64-asm` /
  `check-exc-arm64-asm`). These verify the full-parity claim for the two 64-bit
  backends at the level where the risk lives, the front-end-to-backend path,
  since the runtime C is shared and portable. Both assemble all 89 programs.
  There is no run tier on these two, and that is a decision, not a gap (see
  the note under Roadmap 2).
- **Not yet**: x86-32 (there is no `skj-exc-x86`).

**Tests.** The 174-test suite (`make check-exc`) runs on every end-to-end
backend. It includes 87 compile-error `.experror` tests, the mechanism that
pins a teaching error as a feature (a silent reword fails the test). Separately,
`test-exc-walker` checks the freeze/thaw walker, and `smolmoo-demo` links a
compiled verb for the smolmoo host (link-only in this tree).

**Hosts and deployment.** The runtime is two layers (`host-abi.md`): the
portable `libexc.c` (dispatch, object table, fault/trace formatting, the
value helpers, and the freeze/thaw walker) plus a host binding that provides
the `__exh_*` services. Bindings today: `exc_native.c` (standalone, over
write/exit, for the test tiers) and `exc_smolmoo.c` (the smolmoo RV32 binding,
`__exh_*` over `ecall` hypercalls, link-only). `../doc/smolmoo-v1.md` is the
milestone path from that link-only state to a persistent verb on a real
smolmoo server. boris is a provisional third host.

## Language features lowered end to end

One line each; `CLAUDE.md` has the detail and the deferred edges.

- **Core**: int/bool arithmetic, control flow, local calls, recursion.
- **Object model**: per-instance field segments, class descriptors, `spawn`,
  self/cross/inherited/overridden dispatch through `__exc_send`, obj-typed
  fields as reference-semantic handles (object graphs with cycles).
- **Records**: value semantics, positional and named construction, defaults,
  nested-flat layout, UFCS, value equality, `list<Record>`.
- **Numbers**: `float` (IEEE 754 double), `decimal` (base-10 fixed point), int,
  with the inference and truncation-trap rules of `numbers.md`.
- **Strings**: UTF-8, code-point character operations, `${}` interpolation.
- **Lists**: homogeneous literals, copy-on-write builtins, `for`-in,
  membership, slicing, and list comprehensions.
- **Function values**: anonymous `func` literals as code pointers, func-typed
  parameters, the `map`/`filter`/`sort`/`reduce` prelude; capture is forbidden.
- **Sources**: cursor sources and `source of T` continuation coroutines
  (stackful, on a private arena stack) behind one `next`/`for` face, `yield`.
- **Control**: `if`/`match`/`select` expressions and statements, `then`/`do`
  header closers, `defer`, chained comparisons.
- **Fallibility**: `maybe T`, Icon-style propagation to the nearest consumer
  (`otherwise`, `if var`, `while var`, capture), `on fail`, faults (exit 70).
- **Enums**: ordinals, qualified members, exhaustive `match`, `set of E` masks.
- **Parameters**: `shared` by-reference, record-view (`p with (x, y)`).
- **Output**: `tell` to players, the `///` author trace channel (under `-t`).
- **Meta layer**: `macro`, compile-time introspection (`typeof`, `fieldsof`,
  `verbsof`, `membersof`), typed macros (the `_Generic` tier), field
  annotations. Nothing from the meta layer survives to runtime, by design.

## Designed and type-checked, not yet lowered

These are complete in the checker and have a runtime representation decision,
but emit no dedicated runtime yet.

- **Object slices and interfaces** (`object-slices.md`, `interface-decl.md`,
  `interface-support.md`): checked structurally, typed, and dispatch-checked;
  at runtime a slice is just the plain object handle, so a slice-typed local,
  parameter, or return lowers exactly as `obj`. Deferred: a fallible `as` cast
  from untyped `obj`, and a slice-typed field.
- **Typed data / `shape` schemas** (`typed-data.md`): symbolic literals are
  checked structurally against a schema but do not lower; a symbolic literal
  with no shape in context types `any`.

## Roadmap

Three tracks. Per-feature status stays in each note; treat `CLAUDE.md`'s
implemented list as authoritative where a `core.md` index status has lagged.

### 1. Runtime and lowering completeness

The near-term "Still pending" set from `CLAUDE.md`, in rough priority:

- **Float through sends** (the two-slot argv convention; str and decimal are
  one word and already cross a send).
- **Float (8-byte) fields**, and **float-to-decimal / decimal-to-float casts**.
- **Expression-valued field defaults** (needs constant folding; today a field
  default must be a literal constant).
- **List runtime** work, and the **remaining host powers** of `host-abi.md`.

### 2. Backend reach

- **Excelsior end-to-end on x86-64 and AArch64 is not pursued, by decision.**
  A crt was the expected blocker, but the real one is deeper and structural.
  The two 64-bit backends emit an ILP32 address model (4-byte pointers, 4-byte
  struct fields, the `dd` class descriptors the C compiler's 64-bit targets
  also use), while the portable runtime (`libexc.c`, `exc_native.c`) is
  gcc-built LP64: `typedef long word` is eight bytes and every shared struct
  (`class_desc`, `exc_obj`, the str and list descriptors) has eight-byte
  pointer and `long` fields. libexc is written assuming
  `sizeof(word) == sizeof(void *)`. The two sides disagree on the width of
  every field they exchange, so `__exc_send` reads a verb's code pointer at the
  wrong offset, gets zero, and `call 0` faults at NULL on the first send. This
  is why RV32 and MIPS work and x86-64 does not: those gcc runtimes are ILP32
  (32-bit target, `long` and pointer both four bytes), matching the codegen; on
  a 64-bit host the runtime would need `sizeof(void *) == 4`.
  - The stock ABIs that give a 64-bit host 32-bit pointers, x86 x32 (`-mx32`)
    and AArch64 ilp32 (`-mabi=ilp32`), both compile, but `qemu-x86_64` and
    `qemu-aarch64` user mode reject the resulting ELF ("Invalid ELF image for
    this architecture"): qemu-user does not emulate the x32 or ilp32 syscall
    ABI. So the clean toolchain path does not run under the emulators the suite
    uses.
  - The only remaining route is to fork libexc/exc_native into a narrow variant
    (`word` an `int32_t`, every shared pointer field a `uint32_t` handle
    dereferenced through `(T *)(uintptr_t)h`) that runs LP64 but lays memory out
    ILP32. That touches nearly all of the 1150-line portable runtime and forks
    the layer host-abi.md keeps shared verbatim, for a host that is not a
    deployment target (all three Excelsior hosts are 32-bit ColdFire VMs).
  - The value does not pay for the fork. Codegen parity is the claim that
    mattered for these two backends, and the `-asm` tiers prove it (89/89 each).
    The run tier stays where the risk does not: on the 32-bit end-to-end
    backends. This mirrors the RISC-V double decision (`../doc/emulator.md`):
    a split by design, not a missing feature.
- **x86-32 Excelsior** (`skj-exc-x86`), if there is demand.

### 3. Design decided, awaiting code

Language features with a settled design note but no implementation. See each
note's `Status:` line; the clear members of this set are `blob.md` (the v1
binary-data type), `string-literals.md` (the `{...}` prose string),
`inline-for.md` (`for` unrolled over a heterogeneous compile-time literal),
`mixed-lists.md` (`list of any`), `patterns.md` (typed-hole command templates),
`capabilities.md` (`capability` / `include`), and `type-parameters.md` (`of`).
`backlog.md` holds the remaining design passes that have not yet run at all.

### 4. Deployment milestones

- **smolmoo v1**: from the current link-only verb to a persistent verb running
  on a real smolmoo server (`../doc/smolmoo-v1.md`).
- **boris**: firm up the provisional binding.

Made by a machine. PUBLIC DOMAIN (CC0-1.0)
