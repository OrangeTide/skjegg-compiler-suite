# Kobold-Pascal front end (`kp/`)

Kobold-Pascal reaches the shared IR through a different seam from the other
front ends. Its compiler, `kpcom`, is a separate self-hosting Pascal program
that lives upstream in the Kobold tree, so skjegg has no `lex.c`/`parse.c`/
`lower.c` for it. The bridge into this tree is the **KPM1 text module** that
`kpcom` emits: `kp/kpm.c` reads a KPM1 image into an `ir_program`, and from
there the same selector and sinks the C compiler uses emit the code. Every
other front end is a source-to-IR pipeline in `backend/`'s sense; Kobold is an
IR-in-a-text-format reader in front of the same backend.

The authoritative design log for the port (`doc/port-to-skjegg.md`) and the
authoritative KPM1 format spec (`kp-module-format.md`) are in the Kobold tree,
not here. This file is the in-tree reference for what skjegg consumes and how
it runs it; `kp/kpm.c` is the practical source of truth for the format.

## Tools

All built by `make all`; none is in `check-all` except the reader regression
(see Testing).

- **`skj-kp-jit`** (`kp/jit_main.c`): the in-process JIT. Reads a KPM1 module,
  JITs it over the byte sink, and runs it in the current process. Maps a
  low-memory guest heap (the ILP32 address model), searches the program
  directory, `KOBOLD_UNITS`, and a stdlib path for `uses` dependencies, and
  passes guest `argv` through. `skj-kp-jit-rv` and `skj-kp-jit-arm64` are the
  cross-arch counterparts over the same driver and runtime.
- **`skj-kp`** (`kp/aot_main.c`): the ahead-of-time compiler. Emits native
  assembly over the text sink for a C start file to assemble and link.
  `skj-kp-rv` and `skj-kp-arm64` emit GNU-as assembly for those targets.
- **`skj-kp-patrol`** (`kp/patrol_main.c`): a sample embedding host that
  registers an opaque handle type of its own beside the standard runtime, the
  counterpart of the C JIT's embedding sample.

The JIT and AOT paths are kept at byte-identical-stdout parity with Kobold's
own `kprun` across the upstream runcheck corpus, on x86-64, arm64, and rv32,
for single-module and multi-unit programs, overflow traps included.

## The KPM1 text module format

A module is a sequence of lines. Blank lines and `;` comment lines are ignored;
leading and trailing whitespace is trimmed. The first non-empty line must be
the header `.kpm 1`.

### Top-level directives

- `.kpm 1` — header and version (required, first).
- `.target <name>` — a target hint (optional).
- `.module <kind> [name]` — `kind` is `object` (the default: a compiled unit
  for linking), `library`, or `executable`. An `object` carries its own unit
  name so a consumer's `.uses` can be matched against it.
- `.entry <routine>` — the entry routine of an executable.
- `.uses <name> [fingerprint <n>]` — a direct unit dependency and the interface
  fingerprint it was compiled against. The linker checks the fingerprint
  against the loaded dependency's own, rejecting interface skew.
- `.interface fingerprint <n> ... .endinterface` — a self-delimiting interface
  block a consumer type-checks against. The reader records only the fingerprint
  and skips the body.
- `.file "<path>"` — the debug source hint (`-g`).
- `.import <kind> <name> ...` — a symbol the host must provide. `kind` is
  `func` (a host function), `helper` (a compiler runtime helper), or `type` (a
  host handle type; carries `methods <n>` and a signature per vtable slot, as
  ABI class tokens).
- `.export <name>` — a symbol this module exports.
- `.global ...` and `.func ...` — see below.

### Globals

```
.global <name> <base> [array <n>] [align <a>] [local] [ptr] [export] [rodata] [from <unit>]
    .init <value>            ; a scalar initializer word
    .init sym <symbol>       ; an address-of-symbol relocation
    .string "<text>"         ; a string/byte initializer
.endglobal
```

`<base>` is `i8`, `i16`, `i32`, `i64`, `f32`, or `f64`. `array <n>` makes it an
array; `align <a>` sets the alignment; `ptr`, `export`, `rodata`, `local`, and
`from <unit>` set the corresponding flags.

### Functions and instructions

```
.func <name> [local] [from <unit>] [variadic] params <n> slots <n> temps <n> labels <n>
    .slotsize <s0> <s1> ...  ; per-slot byte sizes
    .paramclass <c0> ...     ; per-param class: 0 = int/pointer, 1 = float
    .paramfw <f0> ...        ; per-param: 1 = single-precision float scalar
    <OP> [key=value] ...     ; one IR instruction per line
    ...
.endfunc
```

`<OP>` is an `ir_op` mnemonic without the `IR_` prefix (e.g. `ADD`, `LW`,
`FCALL`; the full table is `op_table` in `kp/kpm.c`). The operand keys are:

| key     | meaning                                             |
|---------|-----------------------------------------------------|
| `dst`   | destination temp index (`0 <= dst < temps`)         |
| `a`,`b` | operand temp indices (`0 <= a,b < temps`)           |
| `imm`   | 64-bit immediate (a `LIC64` value, an f64's bits)   |
| `slot`  | local-slot index (`0 <= slot < slots`)              |
| `label` | label index (`0 <= label < labels`)                 |
| `nargs` | argument count for a call                           |
| `sym`   | a symbol name (call target, address-of)             |

The psABI hint lines `.paramneb`, `.paramcls`, `.retneb`, and `.retcls` are
accepted and ignored for the v1 subset; the reader derives the psABI fields a
backend needs from `.paramclass` (one register slot per parameter, SSE for a
float).

### Input validation

The format is read from untrusted text, so the reader bounds everything it will
later use as an allocation size or an array index. Declared counts (a
function's `params`/`slots`/`temps`/`labels`, a global's `array` size, an
imported type's `methods`) are rejected if negative or above `KPM_MAX_COUNT`
(`1 << 20`). Every temp, slot, and label index an instruction names is checked
against the enclosing function's declared counts, so a module cannot drive an
out-of-bounds access into `temp_reg[]`, the slot table, or the label table
downstream. A field a line does not provide is left at its default and not
checked, so a legitimate instruction that uses no temp (a `JMP` in a function
with `temps 0`) still reads. This path is exercised by `make check-kp`.

## The guest runtime (`kp/`)

The runtime is the ported Kobold runtime, compiled freestanding with f64
reals. It is shared between the JIT (bound as host symbols) and the AOT path
(resolved by the linker).

- `str_core.c`, `str_fmt.c` — the guest-owned string object and number
  formatting/parsing (`Str`, `Val`).
- `dynarr.c` — the guest-owned dynamic array (`array of T`).
- `map.c` — the guest-owned string-keyed associative array (`map of T`).
- `co.c`, `co_switch_{x86,arm64,rv}.S`, `co_shim.c` — stackful coroutines
  (the Muxx runtime) and the host primitives they need.
- `handle.h` — refcounted (ARC) opaque handles.
- `rt_host.c` — the platform layer: the guest heap over a low-memory mmap (so a
  guest address fits the 32 bits the front end models), plus a live-bytes
  counter.
- `kp_host.c`, `kp_host.h` — the standard host binding surface. An embedder
  calls `kp_install_runtime` to fill a `kp_binding` table (console I/O,
  strings, arrays, maps, coroutines, ARC, traps, and the 64-bit divide
  helpers), then binds its own host types, then calls `kp_jit`.
  `kp_io_set_args` records the command line for `ParamStr`/`ParamCount`, and
  `kp_handle_sweep` runs live handles' finalizers at teardown.
- `aot_rt.c` — the C start file and link-time runtime surface for `skj-kp`
  output: process entry, a low execution stack, the traps, and the
  `ParamStr`/`ParamCount` surface.

## The binding model

A guest reaches the runtime by name. The two paths resolve those names
differently.

- **JIT**: `kp_install_runtime` binds each guest-facing name to a host symbol
  in a `kp_binding` table, and `kp_jit` resolves the IR's call and data
  relocations against it.
- **AOT**: the names are resolved by the system linker. `aot_main.c`'s
  `remap_sym` mirrors `kp_install_runtime` for the names that appear as call
  symbols in the IR (the coroutine operators, `ParamCount`/`ParamStr`/
  `heapLive`, the transcendentals, and on rv32 the real-valued I/O and `Val`,
  which cross a call as integer-register bits under the soft-float ABI and so
  go through a bits bridge). `-DKP_AOT_RV` selects the rv branch.
  `rv_i64_aot_alias.S` defines the libgcc-named 64-bit multiply/shift helpers
  as tail-jumps into the runtime's `skj_jit_*` routines, since the rv backend
  writes those names directly rather than as call symbols `remap_sym` can see.

## Multi-unit linking

A program that `uses` units links across modules.

- **JIT** (`jit/jit_common.c`): each unit is compiled per-module and its
  exports are published as bindings. A cross-module call lands on a per-export
  stub that tail-jumps through a writable slot (`kp_stubs_*`), so linking is
  order-independent: a slot can be filled after the caller is compiled. The
  stub touches only a scratch register, so the guest's argument registers and
  return address pass through untouched.
- **AOT**: `aot_main.c` gives every name in the module's `.export` list
  external linkage before emitting, so the system linker resolves it; a private
  symbol a unit does not export stays local, so two units' same-named private
  globals do not collide. `kp/aot-run.sh` follows the main module's `.uses`
  transitively, compiles each dependency unit (resolving `<dir>/<name>.kpm`
  then `KOBOLD_UNITS`, as `kprun` does), and links the whole set, the system
  linker standing in for the JIT's in-memory link.

## Overflow-trap policy

Kobold's semantics trap on integer overflow, and its JIT does. The shared AOT
text sinks default to the hardware-trap policy they share with the C compiler
(a checked arithmetic op, which C never emits, would otherwise wrap). So
`aot_main.c` raises `target_aot_trap_guarded` (a mode flag in `ir.c`, default
off) before `target_emit`, which makes the x86-64 and arm64 text sinks choose
the selector's guarded trap policy; rv always guards and ignores the flag. The
C compiler is unaffected: the flag defaults to hardware and only `aot_main.c`
raises it, and the checked ops never appear in C.

## Testing

- **In-tree**: `make check-kp` runs `tests/kp/` through `skj-kp`, exercising the
  KPM1 reader's input validation (malformed modules rejected with a pinned,
  line-numbered message; a well-formed module compiles). It needs no external
  repo, qemu, assembler, or linker. The shared JIT the Kobold tools drive is
  also covered by `make check-jit` / `check-jit-arm64` / `check-jit-rv` through
  the C suite.
- **Upstream**: the behavioral runcheck (byte-identical stdout against the
  reference `kprun`, 74-to-76 cases, single-module and multi-unit) lives in the
  Kobold tree. `kp/aot-check.sh` runs skjegg's AOT path against it, and
  `kp/aot-run.sh` stands in for `kprun` in the upstream runcheck, when `KPDIR`
  and `KPRUN` point at that tree. Neither is wired into skjegg CI, since the
  corpus is not in this tree.

## See also

- `doc/jit.md` — the shared in-process JIT the Kobold tools build on.
- `doc/ir.md` — the IR a KPM1 module is read into.
- `PLAN.md`, "Kobold-Pascal in-process linking and lazy compilation" — the
  deferred lazy-compilation work (Steps C and D) and why it waits.
