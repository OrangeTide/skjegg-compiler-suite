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
- **In-process JIT**, platform ABI: **AArch64** (`skj-exc-jit-arm64`,
  `make check-exc-jit-arm64`, run under qemu-aarch64). The exc front end lowers
  to IR, the `jit/` library JITs it to AArch64, and the entry verb runs
  in-process over `libexc.c` built `EXC_GUEST32` (see Roadmap 2). All 87
  end-to-end programs pass, including the source coroutine (its stack switch is
  `jit/exc_coro_arm64.S`).
- **Codegen only** (compile and assemble, no run): **x86-64** and the **AArch64
  static AOT link** (`skj-exc-x86-64` / `skj-exc-arm64`,
  `make check-exc-x86-64-asm` / `check-exc-arm64-asm`). These verify the
  full-parity claim for the two 64-bit backends at the front-end-to-backend
  path. Both assemble all 89 programs. The static AOT link has no run tier (see
  Roadmap 2); on AArch64 the JIT provides one instead.
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

- **Excelsior end-to-end on AArch64 runs through the in-process JIT
  (`skj-exc-jit-arm64`).** The two 64-bit backends emit an ILP32 address model
  (4-byte pointers, 4-byte struct fields, the `dd` class descriptors the C
  compiler's 64-bit targets also use), while the portable runtime is naturally
  LP64 on a 64-bit host: `typedef long word` is eight bytes and every shared
  struct (`class_desc`, `exc_obj`, the str and list descriptors) has eight-byte
  pointer and `long` fields. The two sides then disagree on the width of every
  field they exchange, so `__exc_send` reads a verb's code pointer at the wrong
  offset. This is why RV32 and MIPS work directly: those gcc runtimes are ILP32
  (32-bit target, `long` and pointer both four bytes), matching the codegen.
  - **The static AOT link cannot bridge it under qemu-user.** The stock ABIs
    that give a 64-bit host 32-bit pointers, x86 x32 (`-mx32`) and AArch64 ilp32
    (`-mabi=ilp32`), both compile, but `qemu-x86_64` and `qemu-aarch64` user mode
    reject the resulting ELF ("Invalid ELF image for this architecture"):
    qemu-user does not emulate the x32 or ilp32 syscall ABI. So a statically
    linked 64-bit exc binary has no run tier, and `check-exc-{x86-64,arm64}-asm`
    stay codegen-only.
  - **The JIT bridges it a different way.** `skj-exc-jit-arm64` is an ordinary
    LP64 AArch64 binary (qemu runs it fine) that JITs the guest to native code in
    a low-memory (< 4GB) mapping and links `libexc.c` compiled `EXC_GUEST32`.
    That mode is not a fork: it is the same source under a compile-time switch
    that narrows `word` to `int32_t`, holds each shared pointer field at guest
    width, and routes every guest deref through one accessor macro
    (`EXC_G` / `EXC_TOG` in `libexc.h`). Because the guest arena is below 4GB, a
    guest 32-bit pointer zero-extends to a valid host pointer, so the LP64 host
    code reads and writes the guest's 4-byte object model correctly. The driver
    is non-PIE (so host libexc's `__exc_self` and code sit below 4GB, the guest's
    reach) and `-rdynamic` (so the guest's runtime imports resolve by name
    through `dlsym`). A `source of T` coroutine switches stacks through
    `jit/exc_coro_arm64.S`, the AArch64 counterpart of the RISC-V pair in
    `start_rv.S`. Verified by `make test-libexc-guest32` (the guest-32 data
    model on the host) and `make check-exc-jit-arm64` (all 87 end to end).
  - Landing the run tier surfaced one real lowering gap the codegen-only tiers
    could not: exc did not populate `ir_func.param_cls`, so the AAPCS64/SysV
    selectors homed a float parameter from an integer register. `set_param_classes`
    in `lower.c` now fills it (the 32-bit backends infer float parameters
    elsewhere, so they were unaffected).
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

