# WASM backend

The WASM backend is the sixth code generator, standing beside ColdFire, RISC-V,
MIPS, x86-64, and AArch64. It is structurally different from the five
register-machine backends: WASM is a stack machine with unlimited typed locals
and structured control flow, so there is no register allocator and no flat
label-and-branch output. The backend is still front-end agnostic: it implements
the shared `target_emit()` over an `ir_program`, so every front end reaches it
unchanged.

This note records the design and the phased implementation. The one-paragraph
index lives in `PLAN.md` under "Later: WASM backend"; the control-flow decision
was taken 2026-11-13 and is recorded there.

## Why WASM, not JavaScript

WASM reaches both the browser and the server runtimes (Node, Deno, Bun) with a
faithful memory model. It gives native `i32`, `i64`, `f32`, and `f64`; linear
memory as an `ArrayBuffer` that maps directly onto the flat IR address model;
and defined trap semantics. A JavaScript target would emulate all of that (the
asm.js approach) or compromise it. JavaScript also runs in both environments, so
"browser and server" does not by itself favor WASM. The memory-model fit does.

## Output form

The backend emits WAT (the WebAssembly text format), which `wat2wasm` from WABT
assembles to a binary module. This matches the established pattern: front end to
IR to assembly text to an external assembler. An in-tree binary encoder is a
possible later step, the analogue of the in-tree assemblers for the other
targets, but it is not needed for bring-up.

## Width and endianness

WASM32 is ILP32: a pointer is a 32-bit linear-memory offset. It is
little-endian. Both match the IR's default, so no 64-bit-pointer path is needed
and the front ends run in their ordinary 32-bit configuration. A tool selects
the target with `-DCC_WASM`, which `ir/target.h` treats as an ISA selector that
forces ILP32.

## Memory model: temps in locals, slots in a shadow frame

This is the shape a C-to-wasm compiler uses, and it fits the IR cleanly.

- An IR temp becomes a typed WASM local. The type follows the temp's class:
  `i64` for a 64-bit def, `f32` or `f64` for a float def (by its width tag), and
  `i32` otherwise. WASM has unlimited locals, so each temp gets its own and there
  is no allocation or spilling. `regalloc()` is therefore a near-nop: it only
  records the per-temp type the emitter needs.

- An IR local slot becomes a frame in linear memory, reached through a global
  stack pointer. A slot can be address-taken (`IR_ADL`, `IR_ALLOCA`), so its
  storage has to be real addressable memory, not a local. The prologue subtracts
  the frame size from the `$sp` global and the epilogue restores it, exactly as a
  register backend adjusts the hardware stack pointer. `IR_LDL` and `IR_STL` are
  a load and store against the frame; `IR_ADL` is a frame-pointer-plus-offset
  address.

Keeping temps in locals and slots in the frame means there is no phi placement to
solve. The "SSA within a block, memory between blocks" model already round-trips
every cross-block value through a slot, and a slot is now a frame location, so a
value that outlives its block travels through memory the way it already does on
every other backend.

## Stack-machine instruction selection

Each three-address IR op lowers to: push its operands with `local.get`, emit the
WASM operator, store the result with `local.set`. So `IR_ADD dst, a, b` becomes
`local.get $a` then `local.get $b` then `i32.add` then `local.set $dst`. A load
pushes the address and emits `i32.load`; a store pushes address then value and
emits `i32.store`. This naive form leaves many `local.get`/`local.set` pairs that
a stack-scheduling pass could fold into stack traffic, but correctness does not
need it and the WASM engines fold most of it anyway.

## Control flow: keep the flat IR, structure it in the backend

WASM has no arbitrary branch to a label. It has structured `block`, `loop`, and
`if`, with `br` and `br_if` that target an enclosing construct by depth, and
`br_table` for a computed branch. The flat IR discards structure into labels and
`IR_JMP`/`IR_BZ`/`IR_BNZ`, so the backend reconstructs it.

The decision (2026-11-13) is to keep the flat IR and give the WASM backend a
structurer pass, the stack-machine analogue of the register backends' register
allocator. Carrying structured control flow in the IR would tax all five front
ends and all five register backends to serve the one structured target, against
the "IR and backends reused unchanged" invariant. The structurer follows Norman
Ramsey's "Beyond Relooper" construction. The front ends emit reducible control
flow almost everywhere (Pascal, BASIC, Scheme, MooScript, and non-`goto` C), so
the common path reconstructs clean `block`/`loop`/`if` with no node splitting.
The one irreducibility source is C `goto`, whose residue lowers to a `br_table`
dispatch loop rather than node splitting.

Only if a second structured target ever appears (a readable-JavaScript or
readable-C source emitter) should the IR gain optional, inert region markers that
the register backends ignore and a structured backend prefers. Until then that is
premature, and the relooper is needed as the fallback regardless.

## Phased implementation

Each phase is a commit on full-history and leaves the tree green. Phases 1
through 5, and the self-test lane of phase 6, are implemented and verified.

1. **Done.** Skeleton and the straight-line integer core: the module shell
   (memory, the `$sp` global, exports), function signatures from the parameter
   list, temps as typed locals, slots as a shadow frame, and the non-branching
   integer ops.
2. **Done.** The control-flow structurer: `IR_JMP`/`IR_BZ`/`IR_BNZ` and labels
   reconstructed into `block`/`loop`/`if`/`br` by the dominator-tree relooper,
   with irreducible control flow (a `goto` into a loop) handled by a `br_table`
   dispatch loop.
3. **Done.** Calls: direct calls (to defined and imported functions), external
   imports with an inferred signature, indirect calls through a function table,
   `IR_LEA` of a function as a table slot, and the tail-call form.
4. **Done.** Floating point (`f32` and `f64`) and the 64-bit integer ops, all
   native in WASM. `_Float16` (`FLH`/`FSH`) stays deferred.
5. **Done.** Globals: each global placed at a fixed linear-memory address below
   the shadow stack, with data segments for the initialized ones and an
   initializer symbol reference resolved to the target's address or table slot.
6. **In progress.** Front-end breadth. The `check-wasm` self-test lane is done:
   `tests/wasm_selftest.c` exercises every feature area as self-checks and runs
   under `wasmtime` with no I/O runtime. What remains is the larger runtime work
   below.

## In-tree WASM object format and linker

To run a program that spans more than one translation unit (any program that
uses the in-tree libc), the objects have to be linked. The toolkit links its own
ELF for every other target through `skj-as-*` and `skj-ld-*`, so WASM gets the
same: an in-tree relocatable object and an in-tree linker, no external tool. This
keeps the self-sufficiency the rest of the toolchain has.

The object format is the standard WebAssembly linking convention (the one
`wasm-ld` consumes), so the objects interoperate with LLVM's linker as a bonus,
but the in-tree `skj-ld-wasm` is what the toolkit uses. A relocatable object is a
valid WASM module with three additions:

- A `linking` custom section with a symbol table: each function and data symbol,
  its name, and flags (defined, imported, exported, local).
- `reloc.CODE` and `reloc.DATA` custom sections: a list of relocations, each a
  type, a byte offset into the section, a symbol index, and an addend. The core
  types are the function index and type index in code
  (`R_WASM_FUNCTION_INDEX_LEB`, `R_WASM_TYPE_INDEX_LEB`), the data address in
  code (`R_WASM_MEMORY_ADDR_LEB`/`_SLEB`), and the data-held pointer and function
  pointer (`R_WASM_MEMORY_ADDR_I32`, `R_WASM_TABLE_INDEX_I32`/`_SLEB`).
- Each cross-object reference the emitter would otherwise resolve to a concrete
  index or address (a `call`, a `call_indirect` type, a global's address, a
  function pointer) is emitted as a placeholder with a relocation against the
  symbol, left for the linker to patch.

So the backend grows a second sink. Instruction emission already flows through a
sink interface (`struct wsink`): one instruction selector and one control-flow
structurer feed either the WAT text writer or a binary writer, so the two share
all of their logic. The binary writer first emits a complete, self-contained
module (concrete indices and addresses, byte-identical to what `wat2wasm` makes
from the same WAT), and then, with the linking and relocation sections layered
on top, a relocatable object: a relocation wherever it would otherwise write a
resolved index or address. The linker, `skj-ld-wasm`, reads the objects, builds
one symbol table, concatenates the type, function, code, and data sections while
renumbering indices, assigns each data symbol a linear-memory address, applies
the relocations, builds the function table and its `elem`, synthesizes the
`_start` entry and the WASI seam (whole-program, so it moves here from the
backend), and writes the final module.

Phases, each committable and testable:

1. A WASM binary encoding library (LEB128 and section framing), with a
   round-trip self-test. (Done: `wasm/enc.c`, `make test-wasm-enc`.)
2. The binary sink and the module writer.
   - 2a: route instruction emission through the sink, keeping the WAT output
     byte-identical. (Done.)
   - 2b: the binary writer emits a complete module (all sections, bodies through
     the binary sink). Selected by a `.wasm` output name; validated against the
     reference assembler (byte-identical to `wat2wasm`) and run under `wasmtime`.
     (Done for modules without a synthesized runtime.)
   - 2c: the synthesized WASI runtime (the seam and `_start`) in the binary
     module, so a program with `main` also emits a runnable `.wasm`. The seam
     bodies are single-sourced through the sink, shared by the text and binary
     paths. (Done: byte-identical to `wat2wasm` for the main-defining programs
     too; `check-wasm` runs a `tests/wasm_hello.c` binary under `wasmtime`.)
   - 2d: the relocatable-object form (a `.o` output), adding the linking symbol
     table and the `reloc.CODE`/`reloc.DATA` sections with relocations against
     fixed-width (5-byte) placeholders, following tool-conventions so the object
     interoperates with `wasm-ld`.
     - 2d-1: the calls-only object (functions and the stack pointer): the
       function-index, type-index, and global-index code relocations, the symbol
       table, and `reloc.CODE`; it imports `env.__linear_memory` and
       `env.__stack_pointer` and leaves the rest to the linker. (Done:
       `check-wasm-obj` links `tests/wasm_obj.c` with `wasm-ld` and runs it;
       cross-object calls between two of these objects resolve and run.)
     - 2d-2: data symbols, the `MEMORY_ADDR` relocations, the data segment, and
       the segment-info subsection. Every global goes into one object-local data
       segment; an address-of a global becomes a `MEMORY_ADDR_SLEB` relocation
       against its data symbol. (Done: a program reading a global array links and
       runs; `check-wasm-obj` covers it. An address-of an external data symbol
       such as `&errno` becomes an undefined `DATA` symbol plus a
       `MEMORY_ADDR_SLEB` relocation, bound to the defining object at link; the
       `sym_is_data` hint on the `IR_LEA` tells it from a function address, and
       `check-wasm-ld` covers it. A pointer initializer in data, a global whose
       value is another symbol's address, still needs the complete-module path.)
     - 2d-3: function pointers and `call_indirect`. Taking a function's address
       becomes a `TABLE_INDEX_SLEB` relocation against its symbol, the object
       imports `env.__indirect_function_table`, and `call_indirect` carries a
       `TYPE_INDEX_LEB` relocation; the linker builds the table from the
       relocations (no elem segment needed). (Done for function pointers to
       functions whose signature is known in the object: callbacks, dispatch
       tables, and the recursive and mutual cases all link and run. Taking the
       address of an external function that is never also called directly needs
       its signature, which the IR does not expose at the address-of, so that
       case is deferred.)
       A pointer initializer in data, a global whose value is a string, another
       global, or a defined function, becomes a `reloc.DATA` entry
       (`MEMORY_ADDR_I32` or `TABLE_INDEX_I32`) against the placeholder bytes.
       (Done: string-pointer tables and a data function-pointer table link and
       run. A data initializer pointing at an external symbol is deferred.)
3. `skj-ld-wasm`, the in-tree linker, links two or more objects: merge and
   renumber the type and function and code sections, resolve the symbols, apply
   the code relocations. (Done for the calls-only link: it reads the objects
   (`wasm/wobj.c`), deduplicates the types, resolves functions across objects
   (local symbols stay object-private), applies the function-index, type-index,
   and global-index relocations in place over the 5-byte placeholders, defines
   the memory and the `__stack_pointer` global, and writes a complete module.
   `check-wasm-ld` links two objects where one calls into the other with the
   in-tree linker, not `wasm-ld`, and runs the result. The indirect table is a
   later phase, so a table relocation stops the link with a clear message.)
4. Data and globals: the data address relocations, the data segment merge, and
   address assignment. (Done: the reader parses the data segment, the segment
   info (alignment), and `reloc.DATA`; the linker lays each object's data out
   above `WLD_DATA_BASE` honoring its alignment, builds a data-symbol address
   map, patches the memory-address relocations (`MEMORY_ADDR_SLEB` in code,
   `MEMORY_ADDR_I32` in data), merges the images into one active segment, and
   places the shadow stack above the merged data. `check-wasm-ld` adds a second
   link where the data half passes a pointer, held in data, into the compute
   half, so the data address has to be right across objects. The same objects
   also link under LLVM `wasm-ld`.)
5. Function pointers and the table: the table-index relocations and
   `call_indirect` across objects. (Done: the linker assigns each
   table-referenced function a slot, starting at 1 so slot 0 stays the
   reserved null entry, patches `TABLE_INDEX_SLEB` in code and
   `TABLE_INDEX_I32` in data, and emits the table and a single active elem
   segment placing each function at its slot. `call_indirect` keeps referring
   to table 0, which the module now defines rather than imports. `check-wasm-ld`
   adds a link where one object takes a function's address, both in a data
   dispatch table and as a direct argument, and the other calls it indirectly.
   The same objects link under LLVM `wasm-ld`.)
6. Move the `_start` and WASI seam synthesis into the linker, link the actual
   in-tree libc, and run the stdout-compared cc corpus. (Runtime synthesis is
   done: with an entry (the default, which `--no-entry` turns off) the linker
   imports the three `wasi_snapshot_preview1` functions at the low indices,
   defines a `_start` that calls `main`, runs the exit finalizer, and
   `proc_exit`s its result, and defines the `write`/`read`/`_exit`/`exit` seam
   over the imports. A seam is defined only when it is referenced undefined, so a
   libc that provides its own wins; `_start` is always defined. When the libc
   provides `__libc_fini` (the atexit + stdio-flush finalizer in `libc/fini.c`),
   `_start` calls it after `main` returns and before `proc_exit`; a freestanding
   link omits it and `_start` exits straight away. `check-wasm-exec` links a
   `main`-defining program that calls `write` and returns a code, with no
   `--no-entry`, and checks both its stdout and its exit status under `wasmtime`.
   The libc core now links: `check-libc-wasm` compiles the WASI-seam-linkable
   libc with `skj-cc-wasm`, links each self-checking libc test against it (plus
   `runtime/va_wasm.c` for `__va_arg`) with `skj-ld-wasm`, and runs it under
   `wasmtime`. A test is fed a `.input` fixture on stdin when one exists, and has
   its stdout diffed against a `.expected` fixture when one exists, so the stream
   tests verify output and not just the exit code: `libc_phase_c` echoes stdin
   through buffered stdio, and `libc_phase_g` checks the exit-time flush and
   atexit ordering. The file, directory, and stat layer links too, through
   `runtime/wasi_file.c` over the `__wasi_` import convention; `wasm_fileio`
   exercises it under `wasmtime --dir`. Coroutines and threads need Asyncify, not
   a seam, and stay out. The cc corpus runs too: `check-cc-wasm` links each
   `tests/cc_*.c` against the synthesized runtime and compares its exit code, 105
   of 112 passing (the rest are the WASI exit-code cap, three unimplemented
   features, and the x86-64-only varargs-accessor test).)

## Remaining work

The backend lowers the full C feature set for a self-contained program (one that
computes and returns, with no host calls). Running the I/O-driven test corpus,
and reaching the other front ends, needs more than code generation:

- **A WASM runtime.** The toolkit's programs call the in-tree libc, whose bottom
  layer is a syscall seam (`write`, `read`, `exit`, `sbrk`) implemented per
  target in a `start` file. WASM has none. The decision was WASI, and the seam
  now exists two ways: the backend synthesizes it for a single-TU `.wasm`
  (`emit_wasi_runtime`), and `skj-ld-wasm` synthesizes it when linking objects.
  Both map `write`/`read` onto a one-iovec `fd_write`/`fd_read` and `exit` onto
  `proc_exit`. The linker also synthesizes `__libc_sbrk`: a break pointer lives
  in a second global starting just past the shadow stack, and the seam bumps it
  and calls `memory.grow` when the break passes the current size, so the in-tree
  `malloc` (which sits on `__libc_sbrk`) has its backing. `libc/sbrk.c` is not
  linked on WASM; its `__sys_brk` path has no WASM backing, so the seam stands
  in for it. Taking the address of an extern data symbol (such as `errno`) now
  compiles: a C data reference carries a `sym_is_data` hint on its `IR_LEA`
  (every function is registered in cc's globals table, so a non-function name is
  data), and the object path emits an undefined `DATA` symbol plus a
  `MEMORY_ADDR` relocation for it. `skj-ld-wasm` and LLVM's `wasm-ld` both bind
  the undefined symbol to the data definition in another object. The real
  `libc/malloc.c` compiles to an object this way (its remaining undefined symbols
  are ordinary resolution: `memset`, `memcpy`, `__libc_sbrk`, and `errno`).
- **The libc core links and runs.** `check-libc-wasm` compiles the
  string, ctype, stdlib, stdio, malloc, printf, strtod, scanf and numeric parts
  of the in-tree libc with `skj-cc-wasm`, links each self-checking libc test
  against them (plus `runtime/va_wasm.c`) with `skj-ld-wasm`, and runs it under
  `wasmtime`; phases a, b, e, f and h self-check to 42. Getting here needed four
  codegen fixes that a self-contained program never exercised: an i64 or float
  branch condition is now reduced to the i32 that `if` consumes; a void function
  falls off its end with a value-less return (an `IR_RETV 0` would have given it
  an i32 result that mismatches its void prototype across objects); a call whose
  value result is discarded is dropped by the caller (cc marks a void callee with
  `call_void`, since the callee's real return type, not an unused result temp,
  decides whether the stack carries a result); and a single-precision `IR_FLD`
  or `IR_FSD` honours its width tag.
- **Object-declared WASI imports.** An external function whose name begins with
  `__wasi_` is emitted as an import of `wasi_snapshot_preview1.<field>` rather
  than `env.<name>`, in the text, binary, and object output. This is the
  wasi-libc naming convention (cc drops `__attribute__` in the lexer, so a name
  is the tag that needs no front-end surgery). The object reader keeps each
  function import's module and type, and the linker collects every undefined
  function symbol whose import module is not `env`, dedups by (module, field),
  and gives each an output function index just past the fixed seam imports
  (`fd_write`/`fd_read`/`proc_exit`); the Import section emits a real import for
  it, and a reference resolves straight to that index. This carries a C-level
  shim to the host instead of a hand-encoded linker body.
- **The file, directory, and stat shim.** `runtime/wasi_file.c` provides the raw
  syscall wrappers the libc's file layer uses (`open`, `close`, `lseek`,
  `getdents64`, `statx`, and the `*at` path operations) by calling WASI preview1
  through that import convention. WASI is capability based: a path opens against
  a preopened directory fd, not a global namespace, so the shim enumerates the
  preopens the host granted (`wasmtime --dir`) once and resolves each path
  against the longest matching one. It also translates the two record layouts
  the kernel syscalls use but WASI does not: `struct statx` from a WASI
  `filestat`, and `struct linux_dirent64` from a WASI directory read (keeping a
  per-fd resume cookie, since `fd_readdir` is cookie-driven where `getdents64` is
  stateful). A directory open needs the directory rights and `O_DIRECTORY`
  (requesting `fd_read` on a directory is `ENOTCAPABLE`), so `open` asks for the
  file rights first and falls back to the directory rights. With the shim,
  `libc/stdio.c`, `libc/stat.c`, `libc/dirent.c`, and `libc/fileops.c` link;
  `check-libc-wasm` adds them plus a `wasm_fileio` test that creates, reads,
  stats, lists, renames, and removes within a preopened scratch directory and
  returns 42. `libc/time.c` needs no seam (it returns a fixed epoch).
- **Coroutines and threads do not link on WASM.** `libc/coro.c` is built on
  `co_switch` (a per-target assembly routine that saves the callee-saved
  registers and swaps the stack pointer), and `libc/threads.c` is a cooperative
  scheduler over it. WASM MVP cannot express this: the call stack is not in
  linear memory and cannot be switched. There is no WASI import for it, so it is
  not a syscall seam to synthesize; it needs the stack-switching proposal (which
  the pinned wasmtime now runs behind `-W stack-switching=y`) or the Asyncify
  post-link transform. The TinScheme `shift`/`reset` continuations take the
  stack-switching route; see `wasm-continuations.md`. The libc coroutine and
  thread layer could follow the same path but is still deferred.
- **Variadic calls.** A WASM function signature is fixed, so a variadic call
  cannot pass a varying operand list. The backend uses the fixed-plus-buffer
  convention: a variadic function takes one hidden trailing `i32` parameter, the
  pointer to the caller's varargs buffer. A caller of a variadic function (cc
  marks the call with `call_variadic`) reserves a buffer in its frame, stores the
  unnamed arguments into it end to end (each 8-byte slot 8-aligned, matching the
  `__va_arg` cursor), and passes its address as that trailing argument.
  `IR_VA_START` points the `va_list`'s `overflow_arg_area` at the hidden
  parameter, and `runtime/va_wasm.c` provides `__va_arg`. Integer, pointer,
  64-bit, and floating arguments work (`check-wasm-exec` covers `sum`, `mix`, and
  a float average); the real `libc/printf.c` compiles and links, resolving every
  varargs symbol. A `float` argument is promoted to double by cc's default
  argument promotions, so the stored slot and the `va_arg(..., double)` read
  agree.
- **64-bit division.** cc emits the native `IR_DIVS64`/`DIVU64`/`MODS64`/`MODU64`
  ops for the WASM target instead of the `__divdi3` family of soft-division
  helper calls it uses elsewhere, since WASM has hardware `i64.div_s/u` and
  `i64.rem_s/u`. This was the last soft-division dependency in the libc: with it
  gone, `libc/printf.c` links with no undefined symbol beyond `__stack_pointer`
  and the `__va_arg` runtime. `check-wasm` covers signed and unsigned i64
  division and remainder.
- **Struct by value.** WASM has no argument registers and a WASM parameter is a
  single typed scalar, so a struct cannot ride the operand stack as its bytes.
  cc gives the WASM target the MEMORY-class ABI it already uses for a RISC-V
  ilp32 memory struct: `abi_agg` classifies every struct as MEMORY, so a struct
  argument is passed as an `i32` pointer to a caller-made copy (value semantics),
  and a struct return goes through a hidden leading `i32` pointer the caller
  provides and the callee copies the result into. The callee copies each
  by-reference struct parameter into a local on entry. The backend then only ever
  sees `i32` pointers and aggregate copies, both of which it already emits, so no
  WASM-specific struct lowering is needed. This is gated on `CC_WASM` next to the
  existing `CC_RISCV` ilp32 paths (both are 4-byte-pointer targets); the other
  backends are untouched. `check-cc-wasm` covers the small-struct, two-eightbyte,
  straddling, aligned, and by-value-argument struct tests.
- **The C compiler corpus runs on WASM.** `check-cc-wasm` compiles each
  `tests/cc_*.c` with `skj-cc-wasm`, links it against the synthesized WASI
  runtime with `skj-ld-wasm`, runs it under `wasmtime`, and compares the exit
  code against the `.exitcode` fixture, the same corpus the cross-target
  `check-cc-*` suites run. 105 of the 112 exit-code tests pass. The 7 excluded
  fall in three groups. Three return an exit code past WASI `proc_exit`'s 126
  cap (`cc_t008` returns 128, `cc_t056` 255, `cc_t058` 127); the programs are
  correct, only the exit-code transport cannot carry the value. Three use a
  feature the backend does not implement (`_Float16`, inline asm, computed
  goto). One (`cc_t071`) uses the x86-64 `__builtin_va_arg_*` accessors and is
  excluded on every target; the portable `<stdarg.h>` varargs test `cc_t081`
  passes. (The static-initializer tests passed once a heap overflow in the
  linker's data-symbol map was fixed: it was sized by the function count but
  filled with data symbols.)
- **Other front ends.** The backend is front-end agnostic, so `skj-pc-wasm`,
  `skj-bc-wasm`, and a Scheme tool are each only Makefile wiring plus their
  runtime on WASM.
- **CI.** Done: the `wasm` job in `.github/workflows/ci.yml` runs all six lanes
  (`check-wasm`, `check-wasm-obj`, `check-wasm-ld`, `check-wasm-exec`,
  `check-libc-wasm`, `check-cc-wasm`). It installs `wabt` (wat2wasm) and `lld`
  (wasm-ld) from apt and a pinned `wasmtime` release, and verifies all three are
  on PATH first, so a broken tool install fails the job rather than letting a
  lane skip to a vacuous pass.
