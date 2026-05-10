# Skjegg Implementation Plan

## Current State

Shared 3AC IR (`ir/`), two backends (ColdFire/m68k and RISC-V
RV32IM), four front ends (skj-tinc for TinC, skj-sc for TinScheme,
skj-mooc for MooScript, skj-pc for Compact Pascal), and a built-in
ColdFire assembler (`skj-as`).  All code compiles to assembly,
assembles (with `skj-as` or GNU binutils), links, and runs under
qemu.  121 test source files (100 Pascal, 9 TinC, 6 TinScheme,
6 MooScript).  119 ColdFire tests passing (both assemblers),
12 RISC-V tests passing.

## Target Front Ends

| Tool       | Language        | Source                    | Status          |
|------------|-----------------|---------------------------|-----------------|
| `skj-tinc` | TinC            | in-tree                   | working         |
| `skj-sc`   | TinScheme       | in-tree                   | working         |
| `skj-mooc` | MooScript       | ~/DEVEL/mooscript         | already uses same IR |
| `skj-pc`   | Compact Pascal  | ~/DEVEL/rust/pascaline    | working (100 tests) |
| `skj-cc`   | C (C89 + C99 subset) | —                    | not started     |

---

## Phase 0: Consolidate

Bring MooScript into the tree.  It already shares `ir/ir.h`,
`backend/cf_emit.c`, and `backend/regalloc_cf.c` — identical copies.
The work is purely organizational.

### 0.1 Import MooScript front end ✓

Copied `moo/` directory (lex.c, parse.c, lower.c, main.c, moo.h) into
skjegg.  Replaced `read_file()` with shared `slurp()`, `cf_emit()`
with `target_emit()`.  Added `init_syms` field to `ir_global` in both
backends.  Added `__moo_arena_alloc`/`__moo_arena_reset` to ColdFire
and RISC-V runtimes.  Imported `runtime/list.c` and `runtime/rope.c`.
Tests imported as `moo_*.moo` (5 passing; `moo_room` excluded — verb
calls not yet implemented).  Makefile uses `M68K_CC` for runtime
cross-compilation.

### 0.2 Factor out shared driver code ✓

Moved `slurp()` to `ir/util.c`, removed copies from both `main.c` files.

### 0.3 Test harness ✓

`run-tests.sh` already skips `skj-*` binaries.  All 18 ColdFire tests
pass (8 TinC, 5 TinScheme, 5 MooScript) and all 11 RISC-V tests pass
from `make check` / `make check-rv`.

---

## Phase 1: IR Extensions

The current IR handles 32-bit integers and byte-addressed memory.
Several planned front ends need more.

### 1.1 Tail calls ✓

Added `IR_TAILCALL` / `IR_TAILCALLI`.  Backend emits frame teardown +
`jmp`.  TinScheme `lower.c` threads a `tail` flag through
`lower_expr`, `lower_if`, `lower_call`, and `begin` blocks.  Fires
when `nargs <= nparams` (covers self-recursion and equal-arity calls).

### 1.2 16-bit integer type ✓

Added `IR_I16` to `enum ir_basetype`.  Backend emits `.short` for
16-bit globals.  Frame layout already rounds slots to 4-byte
alignment, so 2-byte locals work automatically.

### 1.3 Floating point ✓

Added `IR_F64` base type and 17 float opcodes: arithmetic (FADD, FSUB,
FMUL, FDIV), unary (FNEG, FABS), comparison (FCMPEQ, FCMPLT, FCMPLE),
conversion (ITOF, FTOI), memory (FLS, FLD, FSS, FSD), and local slot
(FLDL, FSTL).  ColdFire backend has full codegen with dual-class
register allocation (integer d2-d7, float fp2-fp7).  RISC-V backend
has die() stubs.  FPU integration test (`make test-fpu`) verified
under qemu-m68k.

### 1.4 Structured types ✓

**Decision: lower early (Option A).**  Front ends compute field
offsets themselves and emit plain `IR_ADD` + `IR_LW`/`IR_SW` with
calculated offsets.  The IR stays flat — no struct or record types,
no `IR_GETFIELD`/`IR_SETFIELD`.  This is what TinC already does for
arrays.  Pascal and Co front ends compute offsets during type
checking and lower to loads/stores with constants.

All three backends (ColdFire, RISC-V, WASM linear memory) work with
flat offset calculations.  WASM-GC structured types are out of scope.

### 1.5 String support ✓

**Decision: no string type in the IR.**  Each front end handles its
own string representation using existing IR primitives (memory ops +
runtime library calls).  MooScript ropes, Pascal short strings, and
QBasic dynamic strings are all front end / runtime concerns — the
backends don't need to know about them.

### 1.6 Closures ✓

**Decision: front end concern, no IR changes.**  Each front end
implements its own closure strategy using existing IR primitives.
TinScheme uses flat closures (heap-allocated, env as hidden first
parameter).  Pascal uses static links (pointer to enclosing frame).
These are different mechanisms — a unified IR op would only fit one.

---

## Phase 2: Additional Backends

The register allocator is nearly parameterized already (change
`NUM_REGS` and `FIRST_REG`).  Each new backend needs an instruction
emitter (~500 LOC) and a trivial regalloc copy.

### 2.1 Backend interface ✓

Renamed `cf_emit()` to `target_emit()` — both front ends now call the
generic name.  The Makefile uses `BE_CF` for the ColdFire backend
files.  Compile-time target selection (Option A): link a different
backend's `target_emit()` + `regalloc()` to retarget.

### 2.2 RISC-V backend ✓

RV32IM backend with stack-based calling convention matching ColdFire.
Files: `backend/regalloc_rv.c` (11 callee-save regs s1-s11),
`backend/rv_emit.c`, `runtime/start_rv.S`.  Makefile provides
`skj-tinc-rv`, `skj-sc-rv`, and `make check-rv`.  9 tests pass
(cont.tc and scm_shift.scm excluded — continuations not yet ported).
The continuation runtime still needs a RISC-V port.

### 2.2a RISC-V gaps

The RV32IM backend is functional but incomplete.  Known gaps:

- Continuations (IR_MARK, IR_CAPTURE, IR_RESUME) — die() stubs
- Floating point (all float opcodes) — die() stubs

These should be addressed before adding new backends.

### 2.3 WASM backend

Emit WAT (text format), assembled to binary by `wat2wasm` from WABT.
This matches the existing pattern: front end → IR → assembly text →
external assembler.  No register allocator needed — WASM has infinite
locals.  No runtime/start.S — WASM has its own entry conventions.

Files: `backend/wasm_emit.c` (WAT output).

The emitter is structurally different from register-machine backends
(stack machine, structured control flow).  ~800-1000 LOC.

### 2.4 x86-64 backend

Deferred.  The RISC-V backend has gaps to close first, and x86
encoding complexity doesn't teach anything new about the toolkit's
architecture.

---

## Phase 3: New Front Ends

### 3.1 Compact Pascal (`skj-pc`)

Pascal front end in C.  `pascal/` directory (lex.c, parse.c,
lower.c, main.c, pascal.h) targeting the shared IR directly.
The Compact Pascal project (~/Vibe/compact-pascal) owns the
Pascal-in-Pascal self-hosting effort; skj-pc is a separate
implementation sharing only the language definition and test suite.

4999 LOC, 100 of 223 Compact Pascal tests passing.

**Phase 3.1a — Minimal Pascal:** ✓ integer, boolean, char,
if/while/for/repeat, procedures and functions with value and var
parameters, forward declarations, write/writeln builtins, const
declarations, break/continue, global and local variables.

**Phase 3.1b — Extended Pascal:** ✓ nested procedures with static
links, sets (basic, char, int/char/enum subranges, comparisons,
ops), enumerated types, strings (assign, compare, concat, length,
funcs, params), case statements, typed constants (scalar, array,
matrix, record, set), short-circuit evaluation, var/const/procedural
parameters, records (basic, nested, as params), arrays (1D, 2D, as
params), with statements, inc/dec, read/readln, compiler directives
($I+/$I-, $R+/$R-, $M, $DEFINE/$IFDEF), extended literals (hex,
octal, binary), range and overflow checking, halt, chr/ord casts.

**Phase 3.1c — Full Compact Pascal:** variant records (tagged,
untagged, typed constants).  Validate against the full 223-test
Compact Pascal suite.

### 3.2 C compiler (`skj-cc`)

Goal: a C compiler targeting ColdFire (and other backends),
sufficient to compile real command-line applications, old software,
and embedded software.  Targets full C89 (minus K&R prototypes and
bit-fields) plus the practical parts of C99 (minus VLAs).

TinC (`skj-tinc`) remains as a separate, minimal C-like language —
useful for experimentation and constrained environments where a real
C compiler won't run.  `skj-cc` is a new front end, not a TinC
replacement.

**IR prerequisites:** `float` and `double` require `IR_F32` (new)
and `IR_F64` (exists).  `long long` requires `IR_I64` (new).  These
IR extensions can be developed in parallel.  Until they land, the
front end can parse these types and emit a diagnostic.

**Architecture:** the preprocessor is a library (`cpp/`).  `skj-cpp`
is a thin CLI that feeds lines into the preprocessor and prints the
result.  `skj-cc` calls the same preprocessor functions as a
pipe-like layer between file I/O and the lexer — no child process,
no temp files.

**Freestanding headers:** the compiler ships a minimal set of
headers it owns: `<stddef.h>`, `<stdarg.h>`, `<stdint.h>`,
`<stdbool.h>`, `<limits.h>`, `<float.h>`.  Any libc headers
(`<stdio.h>`, `<string.h>`, etc.) are the runtime's responsibility.

**Phase 3.2a — Preprocessor library + `skj-cpp`:**
`#include` with search paths, `#define` (object-like, function-like,
and variadic macros with `__VA_ARGS__`), `#if`/`#ifdef`/`#ifndef`/
`#elif`/`#else`/`#endif`, `#undef`, `#error`, `#pragma` (ignored),
`#line`.  Token pasting (`##`) and stringification (`#`).  Predefined
macros: `__FILE__`, `__LINE__`, `__DATE__`, `__TIME__`, `__STDC__`.
Testable independently via `skj-cpp` CLI.

**Phase 3.2b — Core C89:** ANSI function prototypes and definitions.
All integer types (`char`, `short`, `int`, `long`, `signed`/
`unsigned`), `float`, `double`, `long long` (deferred until IR
support lands).  Structs (lowered early per Phase 1.4), unions,
enums, `typedef`.  Pointers, pointer arithmetic, function pointers.
Arrays, multi-dimensional arrays, string literals.  All control
flow: `for`, `do-while`, `switch`/`case`/`default`, `goto`/labels.
All operators: ternary, comma, compound assignment, `sizeof`.
Implicit/explicit casts, integer promotions, usual arithmetic
conversions.  Static local variables.  Initializer lists with nested
braces.  Separate compilation via `extern` declarations.

**Phase 3.2c — C99 additions:** `//` comments.  Mixed declarations
and statements.  `for`-loop initializer declarations (`for (int i =
0; ...)`).  Designated initializers (`{.field = val, [idx] = val}`).
Compound literals (`(struct point){1, 2}`).  `_Bool` /
`<stdbool.h>`.  `inline` (parsed, treated as hint).  `restrict`
(parsed, ignored).  Flexible array members.  Variadic functions
(`va_start`, `va_arg`, `va_end`, `va_copy` via `<stdarg.h>`).
`__func__` predefined identifier.

**Intentionally skipped:** VLAs, bit-fields, `_Complex`,
`_Imaginary`, K&R-style prototypes, inline assembly,
digraphs/trigraphs, thread-local storage, atomics, `_Generic`,
`_Static_assert` (C11).

---

## Phase 4: Language Completions

### 4.1 TinScheme: tail-call optimization ✓

Completed as part of Phase 1.1.

### 4.2 TinScheme: closures with captured variables ✓

Uniform closure calling convention: all user-defined functions receive
`__env` as first parameter (slot 0).  Lambdas with free variables get
a heap-allocated closure struct (type byte 3 at offset 0, fn_ptr at 4,
captured values at 8+4*i).  `lower_function()` loads captured values
from `__env` into local slots at function entry.  Named functions
called directly pass 0 as env; function references (used as values)
are wrapped in 0-capture closures.  ~200 LOC in `scheme/lower.c`,
test: `scm_closure.scm`.

### 4.3 TinScheme: pairs and lists at runtime ✓

Added `cons`, `car`, `cdr`, `null?`, `pair?`, `set-car!`, `set-cdr!`,
and `list` to the compiler.  Pairs are bump-allocated from an 8KB heap
(`__heap` global, initialized in main).  Layout matches `gc_pair` on
32-bit targets: car at offset 8, cdr at offset 12, 16 bytes per cell.
`car`/`cdr` inline as loads at known offsets, `cons` inlines the bump
allocator.  ~200 LOC in `scheme/lower.c`, test: `scm_pairs.scm`.

### 4.4 MooScript: runtime library

MooScript verbs call host-provided operations (property access, verb
dispatch, string operations).  The compiler emits `IR_CALL` to
named symbols; a runtime library must provide them.  This is
MooScript-specific and depends on the boris engine integration.

---

## Suggested Order

```
Phase 0  ──  Consolidate (MooScript import, shared utilities)     ✓
Phase 1.1 ── Tail calls                                           ✓
Phase 1.2 ── IR_I16                                               ✓
Phase 2.1 ── Backend interface                                    ✓
Phase 2.2 ── RISC-V backend                                       ✓
Phase 3.1a ─ Compact Pascal (minimal)                              ✓
Phase 4.1 ── TinScheme tail calls                                  ✓
Phase 4.2 ── TinScheme closures                                    ✓
Phase 4.3 ── TinScheme lists                                       ✓
Phase 1.3 ── Floating point IR                                     ✓
Phase 3.1b ─ Compact Pascal (extended)                             ✓
    │
Phase 3.1c ─ Compact Pascal (full — variant records, 223 tests)    in progress
Phase 2.2a ─ RISC-V gaps (continuations, float)
    │
Phase 1.7 ── IR_I64, IR_F32              ──┐
Phase 2.3 ── WASM backend (WAT output)    │
Phase 3.2a ─ Preprocessor library + skj-cpp│
    │                                      │
Phase 3.2b ─ C compiler core C89 (skj-cc) │
    │                                      │
Phase 3.2c ─ C compiler C99 additions  ◄───┘  (needs IR_I64/IR_F32)
    │
Phase 2.4 ── x86-64 backend (deferred)
```

Phases within a row can be done in parallel.  Vertical arrows are
hard dependencies.

---

## Non-Goals

- Optimization passes beyond peephole.  The toolkit favors simplicity.
- SSA form.  The IR uses named temps assigned once by convention, but
  there is no SSA dominance tree or phi-node infrastructure.
- Linking multiple compilation units.  Each front end produces a
  single assembly file per source file.  The system linker handles
  multi-file programs.
- C standard library.  Each language provides its own minimal runtime.
- Self-hosting.  The compilers are written in C, compiled by the host
  toolchain.  None of the front end languages need to compile
  themselves.
