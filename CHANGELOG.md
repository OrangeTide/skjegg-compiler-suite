# Changelog

All notable changes to this project are recorded here, newest first, grouped by
month. Releases up to and including 0.12.0 are reconstructed from their git tag
notes; the detail there is shallower than for recent releases.

## November 2026

### 0.67.0

A broad expansion of the Pascal front end (`skj-pc`) toward general-purpose
FPC-procedural Pascal, validated on the x86-64 host suite under the ASan+LSan
leak gate and on the ColdFire corpus under qemu-m68k.

#### Added
- Pointers: anonymous pointer types (`^T`), postfix dereference `p^` that chains
  with `.f` and `[i]`, `New`/`Dispose`, `nil`, and pointer equality at pointer
  width. A pointer is an unmanaged machine-word value, loaded and stored raw.
- File I/O: text files, typed files (`file of T`), and untyped files (`file`),
  each a runtime-owned control block the front end stores inline and passes by
  address. `Assign`/`Reset`/`Rewrite`/`Append`/`Close`; `Read`/`Write`/`ReadLn`/
  `WriteLn` redirected to a file; `Eof(f)`/`Eoln(f)`; `SeekEof`/`SeekEoln`
  (bare and file forms); `Erase`/`Rename`. Typed files add the ISO buffer
  variable `f^` with `Seek`/`FilePos`/`FileSize`/`Get`/`Put`; untyped files add
  `BlockRead`/`BlockWrite`. `{$I+}`/`{$I-}` selects trap-versus-`IOResult`. The
  freestanding build reaches files through the raw open/read/write/lseek
  syscalls.
- The `@` address-of operator on a variable, field or element.
- `Read` parses reals from the standard input, a text file, or a `file of real`.
- `Random`, `Random(n)`, and `Randomize` over a reproducible xorshift32
  generator (a fixed default seed, so a program that never calls `Randomize`
  runs the same sequence on every target).
- Explicit enumerated-type ordinals: `type Day = (Mon, Tue = 10, Wed, ...)`. An
  unnamed member continues from the previous value plus one, explicit values
  must ascend, and the type's ordinal range spans its lowest and highest member
  so `ord`/`low`/`high`/`succ`/`pred`, comparison, `for` loops and `array[E]`
  indexing all see the real values.
- Output formatting: `Write(x:width)` right-justifies integers, strings and
  chars; `Write(r:width:decimals)` prints reals in fixed or scientific form.
- `for x in a` over a static array (desugared to a counted loop, so `break` and
  `continue` work), `case` on a string selector, and the transcendentals
  `sin`/`cos`/`ln`/`exp`/`arctan` plus the `pi` constant.

#### Fixed
- `doc/languages/pascal.md` lagged the implementation, so `real`, `trunc`/
  `round`, `paramstr`, `high`/`low` and more were wrongly marked unimplemented.
  The reference now matches what `skj-pc` accepts.

### 0.66.0

#### Added
- `skj-ar` indexes WASM object members, so one archive tool serves WASM
  archives as it already does ELF32 and ELF64. It detects a member's `\0asm`
  magic and walks its `linking` custom section symbol table, writing the same
  GNU/SysV index for the defined, non-local symbols.
- `skj-ld-wasm` reads `ar` archives and pulls only the members that resolve a
  referenced, still-undefined symbol, to a fixpoint, so a WASM link dead-strips
  to the code a program actually calls. The self-hosted `skj-ld.wasm` does the
  same.
- The browser playground hands the C runtime archive (`rt/c.a.gz`) straight to
  `skj-ld.wasm` instead of splitting it into members in JavaScript; a C module
  then carries only the libc it calls (a hello program drops from about 136 KB
  to about 80 KB).

### 0.65.0

#### Added
- The browser playground compiles and runs C as a fifth language, beside
  TinScheme, Pascal, BASIC, and MooScript. `skj-cc` self-hosts to `skj-cc.wasm`;
  a C program compiles to a relocatable object that the self-hosted
  `skj-ld.wasm` links against the full WASM libc in the browser, so `printf` and
  friends reach the console. The libc ships as one gzipped `ar` archive the link
  worker unpacks, one compressed download instead of many object fetches.
- A file-bundle mechanism for the playground: a language can ship files (a JSON
  of path, content, and editor flags) that the compiler worker seeds into its
  in-memory filesystem, so the compiler can `#include` them, and that the editor
  can open read-only. C's bundle is the libc headers.
- Syntax highlighting in the playground editor for all five languages.

#### Fixed
- `skj-cc`: a comparison or logical operator applied to floating or 64-bit
  operands (`fval != 0.0`, `a && b`, `!x`) was typed as the operand type instead
  of `int`, so a `_Bool` or `int` context mis-stored the result. On the WASM and
  LP64 targets this produced an invalid module; on every backend it was a latent
  mistyping. The result type is now always `int`.
- `skj-cc`: a `long long` assigned or initialized into a 32-bit integer object
  was not narrowed to 32 bits, which the WASM and LP64 backends reject as an
  invalid module (the 32-bit pair-register backends dropped the high word
  implicitly). The store now truncates first.
- `skj-cc` and `skj-cpp`: a diagnosed preprocessor error, such as a missing
  `#include`, now fails the run. The tools printed the error but exited 0 with a
  silently-incomplete result.

### 0.64.0

#### Added
- The browser playground is now multi-language: it compiles and runs TinScheme,
  Pascal, BASIC, and MooScript, each with skjegg's own compiler self-hosted to
  WebAssembly. TinScheme emits a complete module; the other three emit a
  relocatable object that the self-hosted `skj-ld.wasm` links against the
  language runtime in the browser, so the whole compile, link, and run pipeline
  is client side. A new `check-playground-selfhost` lane drives that pipeline on
  the host, and the playground is published to GitHub Pages alongside the manual
  (at `/playground/`).

#### Fixed
- A file-scope `static` object was emitted with external linkage on every
  backend, so two translation units defining one of the same name collided at
  link time. It now has internal linkage. This only affected code that skjegg
  itself compiles across multiple translation units.
- A call through a file-scope function-pointer variable was lowered as a direct
  call to a function named after the variable, which does not exist, so the link
  failed. It is now an indirect call through the stored pointer.

### 0.63.0

#### Added
- TinScheme prints to stdout. `display` writes a fixnum (formatted in decimal),
  a boolean, nil, or a string literal; `newline` and `write-string` round out
  the output forms. Until now a program could only communicate through its exit
  code. The number formatter and the string output path are emitted as shared
  IR, so they work identically on every native backend and on the link-free WASM
  module.
- Binding and control forms: `let`, `let*`, `cond` (with `else`), `when`,
  `unless`, `and`, `or`, and `not`.
- Numeric primitives: `quotient` and `remainder` (truncating toward zero),
  `modulo` (flooring, result takes the divisor's sign), `abs`, `min`, and `max`.
  Each is lowered branchlessly over the tagged fixnum representation.
- Quoted data. `(quote datum)` and the `'` reader syntax now lower in codegen
  instead of erroring: immediate data becomes a constant, a quoted pair or list
  builds runtime cons cells, and a quoted symbol becomes an interned object (one
  per distinct name, so every occurrence shares one identity). The companion
  `eq?` tests identity, which is value equality for immediates and object
  identity for symbols and pairs.
- A TinScheme language reference (`doc/languages/tinscheme.md`) covering the
  value model, every supported form, and the intentional omissions.

#### Fixed
- The TinScheme WASM test lane now asserts real stdout. A test with a
  `.expected` file runs its module as a plain WASI command and diffs the output,
  rather than only reading the top-level value through `--invoke main`.

### 0.62.0

#### Added
- The WebAssembly target self-hosts. `skj-cc-wasm` and `skj-ld-wasm` build the
  TinScheme compiler's own C sources (front end, shared IR, WASM backend, and
  the WASI libc) into a single `skj-sc.wasm` that compiles `.scm` to a `.wasm`
  module under any WASI host, with no external toolchain. The new
  `check-selfhost-wasm` lane runs that module under `wasmtime` to compile each
  `tests/scm_*.scm` and requires the output to be byte-identical to the native
  cross compiler (7 of 7, including the `shift`/`reset` continuation programs);
  it joins the `wasm` CI job.
- A browser playground over that self-hosted compiler (`pages/playground`, built
  with `make playground`). A dependency-free page compiles a source file to a
  WebAssembly executable and runs it in a console, entirely client side: it loads
  `skj-sc.wasm` in a Web Worker driven by a small WASI preview1 shim with an
  in-memory filesystem, then runs the produced module in a second worker. The MVP
  covers TinScheme and a text console.
- WASI argv in the linker's synthesized `_start`. When a linked entry module's
  `main` takes `(argc, argv)`, the runtime now imports `args_sizes_get` and
  `args_get` and fills argv from WASI before calling `main`. This is what lets a
  CLI tool built as a WASM module see its command line. A `main` that takes no
  arguments is byte-identical to before, so every existing WASM program is
  unchanged.
- `setjmp`/`longjmp` in the WASM libc, covering the toolchain's one use (the
  fatal-error unwind, which runs its cleanup before jumping and then terminates).
  A resumable `setjmp` would need the stack-switching seam and is out of scope.

### 0.61.0

#### Added
- MooScript targets WebAssembly (`skj-mooc-wasm`), the last front end to do so.
  With this, **every** front end (C, BASIC, Pascal, TinScheme, MooScript)
  targets WebAssembly. The port reuses the shared WASM backend and follows the
  object-plus-link model: the compiler emits a relocatable `.o`, and
  `skj-ld-wasm` links the MooScript runtime (`str`, `list`, and a host) over the
  synthesized WASI runtime. The new `check-mooc-wasm` lane runs the full corpus
  under `wasmtime` (20 programs plus the compile-only `moo_room`), including the
  toy-host object and room tests, and joins the `wasm` CI job. The motivating use
  case is a browser-based MooScript playground.

#### Fixed
- The low-level `write(fd, buf, len)` builtin now passes a string's data pointer
  as the buffer. A MooScript `str` is a `moo_str *` (`{len, data}`), and the raw
  C `write` the builtin maps to takes a plain `char *`, so passing a string
  literal wrote the header bytes instead of the text. Declared externs taking a
  `str` still receive the `moo_str *` unchanged.
- A `float` parameter was not marked float-class, so the WASM backend typed it
  `i64` rather than `f64`. The soft-float stack backends ignore the class arrays,
  so this only affected the WASM lane (`moo_floats`).
- Void MooScript runtime helpers (`__moo_tofloat`, `__moo_verb_call`,
  `__moo_prop_set`, `__moo_obj_move`, `__moo_obj_recycle`) were called as if they
  returned `i32`, leaving the caller's `local.set` underflowing across the WASM
  object boundary. They are now marked `call_void`.
- The typed property-unbox lowering materialised its tag-check constant after the
  compare that used it, a use-before-def the stack backends tolerate but WASM
  rejects, so an unbox always failed. The constant is now emitted first. The verb
  call to `__moo_verb_call` also now flows its arguments through the variadic WASM
  ABI (the helper is variadic past its four fixed parameters).

### 0.60.1

#### Fixed
- `skj-as-aarch64` now accepts a symbol spelled like a condition code (`eq`,
  `ne`, `lt`, and the rest), so a program with a function named after one
  assembles. The operand was parsed as a condition before the branch-target
  fallback, so `bl eq` failed with "branch/adr operand is not a symbol". The
  parser now keeps the name as a symbol alongside the condition, the way it
  already does for register-spelled symbols, and promotes it to a plain symbol
  when a `@PAGE`/`@PAGEOFF` reloc suffix follows.

### 0.60.0

#### Added
- Three more front ends target WebAssembly: BASIC (`skj-bc-wasm`), Pascal
  (`skj-pc-wasm`), and TinScheme (`skj-sc-wasm`). Each reuses the shared WASM
  backend and links its runtime over the synthesized WASI runtime. With the C
  compiler already on WASM, every front end now targets WebAssembly except
  MooScript. New lanes `check-bc-wasm` (31 tests), `check-pc-wasm` (237), and
  `check-sc-wasm` (7) run each corpus under `wasmtime`, all three in the `wasm`
  CI job.
- Delimited continuations (`shift`/`reset`/`resume`) on WebAssembly for
  TinScheme, lowered to the WasmFX stack-switching proposal. The native backends
  implement these by copying a segment of the native call stack, which WASM
  cannot address, so the WASM lane maps a `reset` body to a continuation created
  with `cont.new`, a `shift` to a `suspend` on a control tag, and a `resume` to
  resuming the captured remainder. Captured continuations are opaque references
  held in a `$conts` table and bridged to TinScheme's `i32` values by their
  table index; each reset extent reclaims the table to its mark-time top, so a
  program that resets in a loop stays bounded. The lane runs under a wasmtime
  built with `-W stack-switching=y`. See `doc/internals/wasm-continuations.md`.
- An `IR_TRAP` opcode: a no-value terminator that aborts the program, which the
  WASM backend lowers to the `unreachable` instruction. The Pascal front end
  routes its range-check (`{$R+}`) and overflow-check (`{$Q+}`) aborts through it
  on WASM, so a failed check reports exit 134 (the native 128+SIGABRT code),
  which the WASI `proc_exit` seam cannot carry as an exit code. The stack
  backends keep `exit(134)`.

#### Changed
- The WASM output-format selection, text or binary module or relocatable object
  chosen by the output file's extension, is shared across every front-end driver
  rather than duplicated in each.

#### Fixed
- The WASM linker (`skj-ld-wasm`) now emits the indirect-function table whenever
  an object imports it, not only when a function address is taken. A module that
  makes a `call_indirect` but assigns no table slot (a TinScheme closure call,
  an indirect tail call, a dead-but-linked helper) linked to a table that did
  not exist and trapped with "table index out of bounds"; the table is now
  emitted sized to the slot count plus one, so the zero-slot case is a valid
  one-entry table.
- A `skj-cc` miscompile on every backend: an integer assigned through a
  non-local floating-point lvalue (a pointer store, an array element, a struct
  field, or a static variable) was stored without converting to the floating
  point type first, so the stored bits were the integer rather than its float
  value. This wrote a stale SSE register on x86 and an invalid `i32`-into-`f64`
  store that the WASM validator rejects. The local-variable path already
  converted; the general lvalue path now does too.

### 0.59.1

#### Changed
- The README now documents the WebAssembly target that 0.59.0 introduced: the
  `skj-cc-wasm` compiler and the `skj-ld-wasm` object linker in the tool tables,
  `backend/wasm_emit.c` in the architecture diagram, the C front end's WASM
  target and its WASI-hosted libc (the file, directory, and stat calls go through
  a WASI shim, and the coroutine and thread layer is the one part that does not
  link), and the `check-cc-wasm` lane. No code change; the 0.59.0 binaries are
  unaffected.

### 0.59.0

#### Added
- A WASM backend, the sixth code generator. It lowers the full C feature set to
  a WebAssembly module: the integer core, a control-flow structurer that
  reconstructs WASM's structured `block`/`loop`/`if` from the IR's arbitrary
  branches (with a `br_table` dispatch loop for an irreducible graph), direct
  and indirect calls, floating point and native 64-bit integer operations,
  globals in linear memory, variadic calls through a fixed-plus-buffer ABI, and
  struct-by-value passing and return. `skj-cc-wasm` emits a WAT text module or a
  binary module.
- An in-tree WASM object format and static linker, `skj-ld-wasm`. The backend
  emits a relocatable object (data and function-pointer relocations, an
  indirect-call table, extern data and function symbols), and the linker merges
  objects, resolves the memory-address and table relocations, and synthesizes
  the WASI runtime for an entry link: the three `wasi_snapshot_preview1`
  imports, a `_start`, the `write`/`read`/`exit`/`sbrk` seam over them, and the
  exit-time finalizer that flushes the stdio buffers and runs the `atexit`
  handlers. An object may declare its own WASI imports through the
  `__wasi_<field>` naming convention, which the linker carries through to real
  imports.
- A WASI runtime for the in-tree libc. A file, directory, and stat shim
  (`runtime/wasi_file.c`) maps the libc's `open`/`close`/`lseek`, directory
  reads, `stat`, and path operations onto WASI preview1 over its capability
  based preopen model, so `stdio.c`, `stat.c`, `dirent.c`, and `fileops.c` link
  and run under `wasmtime`. Coroutines and threads stay out: WASM cannot switch
  stacks without the Asyncify transform.
- The C compiler corpus runs on WASM. `check-cc-wasm` compiles each
  `tests/cc_*.c` with `skj-cc-wasm`, links it against the synthesized runtime,
  runs it under `wasmtime`, and compares the exit code against the same fixtures
  the cross-target suites use; 105 of 112 pass. New lanes `check-wasm`,
  `check-wasm-obj`, `check-wasm-ld`, `check-wasm-exec`, and `check-libc-wasm`
  cover the backend, the object format, the linker, a complete program, and the
  libc, with the stream tests feeding stdin and diffing stdout. A `wasm` CI job
  runs all six lanes.

#### Fixed
- A heap overflow in `skj-ld-wasm`: the global data-symbol map was sized by the
  function count but filled with data symbols, so an object with many static
  initializers corrupted the heap and aborted the link.
- Two unsound cases in scalar register promotion (new in 0.58.0). A slot read
  before its first store carries an entry value a register does not, which
  miscompiled the managed-reference release idiom; such slots are now excluded.
  A malformed module could also carry a slotted load or store with no temp
  operand, which promotion turned into an out-of-range move; a store source and
  a load destination must now be real temps.

### 0.58.0

#### Added
- An internals note in the manual, "An Optimization Adventure", recording the
  post-0.57.0 optimization exploration: the register-promotion pass, the dead
  ends that did not pay off, the measured results across the backends, and the
  debug-info analysis that cleared the way to enable the pass.

#### Changed
- Scalar register promotion is now on by default. A local variable whose address
  is never taken, and which is reached only through its own load and store, is
  kept in a register across its whole live range instead of round-tripping
  through a frame slot at every basic-block boundary. The pass covers word
  scalars, `char` and `short`, and pointers and longs. On the 64-bit targets a
  `long long` is promoted in a single register; on the 32-bit targets a 64-bit
  value is promoted as a register pair, except on MIPS, where the register
  pressure of the pair costs more than it saves and the value is left in memory.
  On the two-address targets, x86-64 and ColdFire, the promoted moves coalesce
  into the surrounding instructions, so instruction counts fall along with memory
  traffic, by up to about 3.6 percent on the ColdFire benchmarks. The
  three-address targets see the reduction in memory traffic alone. The pass is
  safe under `-g`, since the debug information is a line table that does not
  record where a variable lives. Set `SKJ_NO_PROMOTE` to disable it.

### 0.57.0

#### Added
- Two internals notes in the manual: one on the control-flow-aware liveness pass
  and why the previous textual liveness was unsound, and one on the optimization
  opportunities that remain on the three-address backends, grounded in the
  measured instruction mix.

#### Changed
- Register allocation now computes live intervals with a shared control-flow-aware
  pass. It builds a control-flow graph from the instruction list, solves liveness
  by backward dataflow, and extends each value's range to cover every block where
  it is live. This replaces the five near-identical textual scans the backends
  carried and fixes a latent case where a value live across a loop back-edge was
  given too short a range.
- Move-coalescing is re-enabled on the two-address targets, x86-64 and ColdFire.
  When a copy or a two-address operation has a source operand that dies at that
  point, the allocator reuses the source register for the result and the setup
  move is dropped. ColdFire retired instruction counts fall by up to five percent
  on the benchmarks, with reg-to-reg moves down about 38 percent on the C corpus;
  x86-64 reg-to-reg moves fall about 44 percent on the Kobold compiler. The
  three-address targets emit no setup move, so they are intentionally left
  unchanged.

#### Fixed
- A short-circuit `and` or `or` in Pascal now releases its right operand's managed
  string temporaries on the path that evaluated them, rather than at the merge.
  The previous unconditional release at the merge could free a string the
  short-circuit path never created, a use-after-free that stayed hidden until
  register pressure moved the temporary to the stack.

### 0.56.0

#### Added
- BASIC gains the `VECTOR OF T` growable list, where `T` is a scalar, a `STRING`,
  or a record. `APPEND`, `REMOVE` (which moves the last element into the gap, so it
  is cheap but does not keep order), `CLEAR`, and `COUNT` manage it; elements are
  reached by a zero-based index with a runtime bounds fault; `FOR EACH` walks it; a
  record element is used through a field (`v(i).field`) or copied whole; and a
  vector passes to a routine and returns from an `FN` by reference.
- BASIC `PRINT AT row, column` positions the console cursor, and `PRINT TAB(column)`
  pads to a column, with the runtime tracking the column as it writes.
- BASIC module-scope variables are now globals, visible inside every `PROC` and
  `FN` rather than only the main program. A routine's own `DIM` is still local and
  may shadow a global of the same name.

#### Changed
- Every BASIC variable now starts at its zero value: `0`, `0.0`, an empty `STRING`,
  a zero-filled array or record, an empty `VECTOR`. A global lives in zeroed `.bss`
  and a local is cleared at its declaration.
- The BASIC language reference now pins the observable semantics a separate
  implementation must match: scope and initial values, operand-type promotion,
  64-bit wraparound and `BYTE` truncation, the `^` power rules, integer
  division-by-zero and vector-index faults, `PRINT` spacing, `INPUT` field
  splitting, and `FOR` evaluation.
- The in-tree assembler indexes its symbol table by a hash instead of scanning it
  linearly, so assembling is linear in the symbol count rather than quadratic. A
  large input now assembles several times faster (the Kobold front end's output
  drops from about six seconds to under half a second), with byte-identical object
  output. All five target assemblers share the change.

#### Fixed
- A declared but unassigned BASIC `STRING` now reads as an empty string instead of
  faulting on a null handle. The same holds for an unassigned `STRING` array
  element, record field, or vector element.

### 0.55.0

#### Added
- Weak references. A record field of a handle type, an `interface` or a `process`,
  may be qualified `weak`, as in `weak parent: INode;`. A weak field holds its
  referent without counting the reference, so it does not keep the object alive and
  reads as nil once the last strong reference is released and the object reclaimed.
  This breaks a reference cycle that automatic reference counting cannot free: a
  parent that owns its children and a child that points back at its parent weakly is
  reclaimed as a whole when the parent drops. The qualifier is rejected on a field
  that is not a handle type, and on a computed field. It works on every backend and
  tier. A `weak process` field is supported on the hosted JIT and AOT paths and on
  the freestanding rv32 and ColdFire runtimes alike; `weak interface` is a
  hosted-tier feature, since the freestanding tier materializes no interface handles.
- The Kobold manual now states the memory model: automatic reference counting, the
  reference cycle it cannot reclaim, and `weak` as the tool to break a deliberate
  cycle, with the parent-pointer and observer back-edges as the worked examples.

### 0.54.0

#### Added
- Records cross the C FFI by reference as a documented contract: a `var` record is
  a C `T*`, a `const` record a `const T*`, with field layout matching the C struct
  (and a variant record overlaying like a C union). A field that is a managed type
  (a string, dynamic array, map, interface, or handle) is rejected, since it is a
  reference-counted descriptor, not inline struct data.
- Real sub-word integer types. `byte` and `shortint` are one byte, `word` and
  `smallint` two, each with its own storage width and signedness rather than being
  aliases of the 32-bit integer. A record of them now matches a C struct of `char`
  or `short` fields, so `SDL_Color` is `record r, g, b, a: byte; end`. Field and
  array storage narrows to the true width; register values and arithmetic are
  unchanged.
- Packed records and the `{$A n}` field-alignment directive, matching a C struct
  declared `#pragma pack(n)` or `__attribute__((packed))`. `packed record ... end`
  removes inter-field padding (field alignment capped at 1); `{$A n}` caps field
  alignment at n (1, 2, 4, 8, or 16) for the records that follow, and `{$A+}`
  restores natural alignment. `packed` stays usable as an ordinary identifier.
- A small plain-data record crosses a `cdecl` boundary **by value** in registers on
  x86-64, arm64, and rv32, following the C register ABI: up to 16 bytes on the
  64-bit targets (SysV eightbytes, or an arm64 floating-point HFA) and up to 8 bytes
  on rv32, with integer slots in general registers and float slots in SSE / HFA
  registers. A record whose slots do not fit the remaining argument registers passes
  on the stack. This holds both ways and for both call forms: a Kobold `cdecl`
  routine passes and returns such a record by value; a C library may call a Kobold
  `cdecl` callback with a record by value and take its record result in registers;
  and a call through a `cdecl` procedure value (a C function pointer) returns a
  record in registers just as a direct call does. A record too large for the
  register class, a record with a managed field, and the ColdFire / MIPS targets
  still cross by reference. See `doc/languages/kobold/kp-native-units.md`.
- `sdl3`, an optional native-backed unit gated on an external dependency: `uses
  sdl3` gives a Kobold program SDL3 windows, a 2D renderer, and polled input. It
  builds only when `./configure` found SDL3, so the core build is unaffected
  without it. Its binding rests on the FFI record support: `Color` has `SDL_Color`'s
  exact four-byte layout, and windows and renderers are integer ids into a glue
  table, never pointers.
- `stb_image`, a native-backed unit binding stb_image.h, the public-domain image
  loader (PNG, JPEG, BMP, TGA, GIF, and more), vendored at `third_party/stb_image.h`.
  `Load` and `LoadRGBA` decode a file and return an integer image id; `Width`,
  `Height`, `Channels`, and `Pixel` read the decoded image. Being header-only it has
  no external dependency and is always built. It is the worked example of a `string`
  path and an integer-id handle crossing the FFI together; its glue decodes the
  Kobold string path with `kp_str_bytes`.
- `stb_truetype`, a native-backed unit binding stb_truetype.h, the public-domain
  TrueType font loader and rasterizer, vendored at `third_party/stb_truetype.h`.
  `LoadFont` returns a font handle; `Ascent`, `Descent`, `LineGap`, and `Advance`
  report scaled metrics; `Render` rasterizes a code point into a glyph handle whose
  8-bit coverage bitmap is read with `GlyphWidth`, `GlyphHeight`, `GlyphXOff`,
  `GlyphYOff`, and `Coverage`. Also header-only, so always built.
- `netchan`, a native-backed unit binding netchan, a multiplexed
  reliable/unreliable channel protocol, over UDP. The netchan core and its UDP
  transport are vendored at `third_party/netchan/` (CC0), so the unit has no
  external dependency and is always built. A connection and a channel are integer
  ids; `OpenServer` / `OpenClient` create a connection, `Service` runs the socket
  pump, `OpenChannel` and `Send` transmit, and `Poll` plus `Recv` receive. It is
  the first unit over a vendored multi-file library rather than a single header.
- `curl`, a native-backed unit binding libcurl's easy interface for a simple fetch,
  built when `./configure` finds libcurl (`HAVE_LIBCURL`). `Get` returns a URL's
  body as a string, with `LastStatus` and `LastOk` for the outcome; the glue
  supplies the write callback libcurl delivers the body through. A `file://` URL
  reads a local file through the same path as an `http://` fetch.
- Unit routine names are namespaced. A unit compiles its public routines to symbols
  qualified by the unit name (`<unit>.<name>`), so a unit may reuse the bare names of
  the C library it binds without colliding with that library's symbols at link time.
  A program's routines, methods (qualified by receiver), and `external` / `cdecl`
  routines are unchanged, and a re-exported routine keeps its origin unit's qualifier.
- `@unit.routine` takes the procedural value of a routine exported by a used unit, so
  a qualified import can supply a callback. When the routine is overloaded, a value
  has no call arguments to resolve against, so the target proc type selects the
  overload: the one matching the variable it is assigned to or the parameter it is
  passed to. This applies to the bare `@routine` form as well, which previously bound
  the first candidate and failed to type-check against a mismatching target.
- `raylib`, a native-backed unit binding raylib for a window, immediate-mode 2D
  drawing, and color and vector helpers, built when `./configure` finds raylib
  (`HAVE_RAYLIB`). It is the worked example of the by-value record FFI: `Color`,
  `Vector2`, and `Rectangle` cross by value in registers as raylib's C ABI does. It
  keeps raylib's own routine names (`InitWindow`, `DrawRectangle`), which the unit
  namespacing keeps from colliding with raylib's C symbols. configure probes the
  header and static link line directly, since raylib often ships with no pkg-config
  file. Its test runs a real window and frame under `xvfb-run` with software GL.

#### Fixed
- A native-unit glue object compiled for a 64-bit target now gets the correct
  pointer width automatically. The glue carries no instruction-set selector, so
  `ir/target.h` defaulted its width to ILP32 and a guest pointer (a string
  handle) was truncated unless each unit's build hand-set `-DSKJ_LP64=1`. That
  flag was also a latent bug, since a future rv32 or ColdFire native unit would
  be forced to the wrong width. The header now derives the width from the
  compiling toolchain for a freestanding glue or runtime object: LP64 on an
  x86-64 or arm64 host, ILP32 from a 32-bit cross compiler. A contradicting
  `-DSKJ_LP64` is a build error rather than a silent wrong-width pointer.
- AArch64: a float argument's live range now extends to the call that consumes it,
  not just to the argument setup. Several float arguments staged for one call could
  otherwise collide in a register and be miscompiled.
- A qualified import resolves overloads across a unit. `uses U as u; u.Foo(x)` now
  chooses the overload of `Foo` that matches the argument types, the same as a bare
  import or a same-module call. A dotted reference previously bound the first
  candidate by name, so a call matching a later overload failed to type-check.

### 0.53.0

#### Added
- Kobold procedure values (function pointers). A routine taken by value is a bare
  code pointer emitted through the platform C ABI, so the same value serves a
  Kobold-to-Kobold indirect call and a C callback across the FFI boundary, with no
  trampoline. A bare routine name takes its value, except a parameterless function,
  whose bare name calls it; `f()` forces the call and `@f` takes the value.
- The `@` operator takes any routine's procedural value (`@routine`), the way to
  pass a parameterless function, since its bare name would call it.
- `cdecl` may be written on a procedure type, as in
  `type Op = function(x: integer): integer cdecl;`, marking a callback type that
  crosses the C ABI. It is signature-compatible with a plain procedure type.
- A procedure type is carried across a unit interface, so a unit may take a Kobold
  routine as a callback parameter and a consumer can pass `@routine` to it.
- Native-backed optional units: a `uses` unit whose implementation calls a thin C
  glue layer over an external C library, built ahead-of-time and shipped in the
  unit's own archive. See `doc/languages/kobold/kp-native-units.md`.
- `std.osthreads`, a standard-library unit that runs a Kobold routine on a real OS
  thread (`Spawn(@work, arg)` and `Join`), the first native-backed unit. It is
  shared-nothing: a worker takes one integer and returns one integer, and must not
  touch a value shared with another thread.

#### Fixed
- A bare parameterless function whose result type is itself a procedure type had
  its address taken instead of being called, a silent miscompile. It is now called,
  matching the rule that a parameterless function's bare name calls it.
- An installed compiler finds a shipped standard-library unit with no
  `KOBOLD_UNITS` set. It resolves its unit directory relative to its own binary,
  as `<bindir>/../lib/skjegg/kobold/<target>/`, matching where `make install`
  places the units.

### 0.52.0

#### Added
- Kobold string interpolation now accepts a `real`. A `${expr}` hole whose value
  is a real renders the way `write` prints it (its shortest `%g` form); it was
  previously a compile error, so a real had to be converted with `str` first.
- An opt-in CP0 system tier in the `skj-run` MIPS core: coprocessor-0 exceptions,
  interrupts, and the return-from-exception path, with a host-side test suite
  exercising them.
- A runnable BASIC example under `examples/basic/` that defines a module and
  consumes it from a program.

#### Changed
- The language is written "Kobold Pascal" (no hyphen) throughout the
  documentation and the sources.
- The Kobold guide and tutorial were refreshed: the backend list now names all
  five targets (adding ColdFire and MIPS), a stale WebAssembly reference is gone,
  and the `.kp` source-file convention is documented.

#### Fixed
- `make install` no longer aborts with "will not overwrite just-created" when the
  tool or SDK install list named the same binary twice.

### 0.51.0

#### Added
- BASIC binds a foreign C routine with an `EXTERNAL` clause. A
  `DECLARE PROC Name(params) EXTERNAL "csym"` or
  `DECLARE FN Name(params) AS T EXTERNAL "csym"` names the object symbol a BASIC
  routine resolves to, with no body, the way Pascal's `external` and Kobold's
  `external cdecl` do. An `EXTERNAL` routine may be `EXPORT`ed from a module,
  where its `.bmi` records the link symbol. A BASIC `INT` is 64-bit but a C
  `int` is 32-bit, so an `EXTERNAL` routine crosses `INT` as a 32-bit value. The
  caller narrows each argument and sign-extends a 32-bit result, which keeps the
  argument registers aligned on the o32 (MIPS) ABI and widens the return value
  correctly on the LP64 backends.
- `units/psx.bas`, a BASIC binding to the `libpsx` PlayStation SDK, built on the
  `EXTERNAL` clause. BASIC joins C, Pascal, and Kobold as the fourth language
  that drives the original PlayStation hardware. The binding covers the
  pointer-free scalar and integer-handle subset of libpsx: GPU and display,
  texture draws by handle, the pointer-free GTE helpers and ordering table,
  fixed-point, pad, SPU, CD, and timers. The pointer and struct entry points
  stay on the C side behind small accessors.
- BASIC ports of the `hello`, `cube`, and `sprite` PlayStation demos in
  `examples/psx/`, with a README and captured screenshots, so BASIC sits beside
  the other three languages. The demos build as MIPS PS-EXE binaries and were
  verified on the PCSX-Redux emulator.

### 0.50.0

#### Added
- A module system for BASIC, with a Modula-2 surface over `ar` archives. A
  source file headed by `MODULE M` is a library with no entry point; a
  per-declaration `EXPORT` publishes a `CONST`, `FN`, `PROC`, or `TYPE`.
  `skj-bc --emit-module --meta <file>` writes a text `.bmi` interface beside the
  object, and `skj-ar` stores it as a reserved archive member. A program brings a
  module in with either `FROM M IMPORT a, b`, which binds the names unqualified,
  or `IMPORT M`, which binds them dotted as `M.name`. The compiler resolves the
  module on the `-I` import path and checks its target tag. An `EXPORT TYPE` is
  opaque to importers: it crosses as a handle, constructed and reached only
  through the module's exported routines, with its fields private. A handle is
  nominally typed, so a call, assignment, or return that mixes two opaque types
  is a compile error. Both import forms carry opaque types, a handle variable
  declared `DIM c AS Circle` for an unqualified import or `DIM c AS M.Circle` for
  a qualified one. The reference is `doc/languages/basic-modules.md`.
- BASIC reaches the two LP64 backends, arm64 and x86-64, built from the same
  front end over the shared IR through the platform psABI selectors, the same
  path the C and Pascal front ends take. BASIC now runs on all five first-class
  backends. Pointers, string handles, and opaque handles are a full 64-bit word
  there, and a FLOAT argument passes in a floating-point register per the
  hard-float ABI.
- `skj-ar` indexes ELF64 archive members, so an x86-64 or arm64 module archive
  carries the symbol table the linker needs to resolve a module reference.

#### Changed
- A record is padded to its worst-case member alignment, and the shared moo bump
  arena is 8-aligned on the 32-bit targets, so a record field that is a FLOAT or
  a 64-bit integer is correctly aligned on every backend.

#### Fixed
- MooScript's `return..` list builder no longer assumes the bump arena packs its
  reservations contiguously. It tracks an explicit write pointer, so the list is
  built correctly whatever the arena's alignment granularity.

### 0.49.0

#### Added
- BASIC reaches three more backends, built from the same front end over the
  shared IR. `skj-bc-rv` compiles to RISC-V RV32 with hardware float and runs
  under qemu-riscv32, both hosted and confined inside the `skj-run` sandbox.
  `skj-bc-mips` compiles to MIPS with soft float for the R3051 and runs under
  qemu-mipsel, and links a PlayStation PS-EXE that runs on the console, verified
  on PCSX-Redux with a real BIOS. The BASIC runtime (`runtime/basic_rt.c`) and a
  new PlayStation seam (`runtime/basic_psx.c`, which drives root counter 2 for
  the clock, sleep, and random source) back the ports.

#### Changed
- `PRINT`, `STR$`, and the FLOAT formatter emit the shortest decimal that reads
  back to the same double, replacing the fixed six-place decimal. The digits come
  from the Dragon4 algorithm in exact integer arithmetic, so the output depends
  only on the IEEE 754 bit pattern and is identical on every backend. `VAL` and
  `INPUT` parse a FLOAT back with a correctly rounded decimal-to-double, so a
  value survives a print-and-read round trip. `SQR` and the trigonometric helpers
  round every step to double, so their full-precision output matches across
  backends; the trig polynomials stay accurate to about seven to ten digits, and
  `basic_t018_trig` runs on the strict-double suites (the ColdFire 68881
  double-rounds the last digit in 80-bit precision, so it is excluded there).
- The `skj-run` emulator services the rv32 time64 `clock_gettime` and
  `clock_nanosleep` calls. A modern 32-bit guest has no legacy 32-bit-timespec
  clock or sleep call, so this is the seam BASIC and the other languages use for
  `TIME` and `WAIT` under the sandbox.

#### Fixed
- The PlayStation crt (`runtime/start_psx.S`) now writes console output through
  the PCSX-Redux putchar register when it is present, falling back to the BIOS
  TTY call otherwise. The old path spun on the BIOS serial-ready poll and hung
  under the emulator, so no PS-EXE could print.

### 0.48.0

#### Added
- A BASIC front end, `skj-bc`, imports the core of the BASIDE structured-BASIC
  language as a new statically compiled front end over the shared IR. BASIC
  joins TinScheme, MooScript, Pascal, Kobold, and C on the same backends,
  assemblers, and linkers, following the standard front-end shape (`lex.c`,
  `parse.c`, `lower.c`, `main.c`). It takes the language, not the BASIDE
  bytecode VM or SDL runtime: the scope is the core language, with no VM ISA and
  no display or game statement library. The front end targets ColdFire/m68k and
  its programs run under qemu-m68k. The language reference is
  `doc/languages/basic.md`.
  - Types: a 64-bit `INT`, a `FLOAT` (IEEE 754 double) with numeric promotion, a
    `BYTE`, a dynamic `STRING`, records (`TYPE`...`ENDTYPE`, with fields, whole
    record assignment, and `REF` record parameters), and fixed arrays of scalar
    types. The full integer and float expression grammar, `CONST` and `ENUM`
    integer constants, and a predeclared `PI`.
  - Control flow: `IF`/`ELSEIF`/`ELSE` (single-line and block), `WHILE`,
    `REPEAT`/`UNTIL`, `FOR`/`NEXT`, `FOR EACH` over arrays, `CASE`/`WHEN` with
    value, range, and relational items, and `EXIT`.
  - `PROC` and `FN` routines with value and `REF` parameters, forward
    declaration, and mutual recursion.
  - Console and timing statements: `PRINT` with `;` and `,` separators, `INPUT`,
    and `WAIT`.
  - The core built-in library: the string functions `LEN`, `ASC`, `INSTR`,
    `CHR$`, `STR$`, `LEFT$`, `RIGHT$`, `MID$`, `UCASE$`, `LCASE$`, `SPACE$`,
    `STRING$`, `HEX$`, `BIN$`, `FIX$`, `ZERO$`, `RJUST$`, `LJUST$`; the numeric
    functions `ABS`, `SGN`, `MIN`, `MAX`, `CLAMP`, `FLOOR`, `CEIL`, `ROUND`,
    `LERP`, `SQR`, and the trigonometric family `SIN`, `COS`, `TAN`, `ATN`,
    `ATN2`, `SIND`, `COSD`, `TAND`; the conversions `INT`, `FLOAT`, `VAL`,
    `VALINT`; and `RND`, `SEED`, `TIME`.
  - A small freestanding BASIC runtime (`runtime/basic_rt.c`) over the shared
    syscall seam, with no libc or libm. It carries its own string operations,
    integer and float formatting, software square root and trigonometry, a
    linear congruential generator for `RND`, and a monotonic clock for `TIME`.
  - The `VECTOR` growable-list type and the `AT` and `TAB` console cursor
    statements are specified but not yet implemented.
  - A user manual, `doc/tools/skj-bc.md`, covers invoking the compiler, the
    compile/assemble/link/run pipeline, a tour of the language, a reference for
    every core built-in function, and a consolidated grammar. The normative
    language definition stays in `doc/languages/basic.md`.

### 0.47.1

#### Changed
- The Kobold `psx` unit (the PlayStation SDK binding) is split into per-subsystem
  leaves (`psx.gpu`, `psx.pad`, `psx.spu`, `psx.fixed`, `psx.tex`, `psx.cd`,
  `psx.gte`, `psx.ot`), with `psx.kp` now an umbrella that re-exports them. A game
  keeps its single `uses psx` and every name, but each leaf lowers to its own
  object in the binding archive `psx.a`, so the linker pulls only the subsystems
  the game calls instead of the whole binding. A Kobold PS1 binary that used to
  run about 3x its C and Pascal twins (the monolithic binding dragged all of
  libpsx in) now matches them: `cube.kp` and `game.kp` fall from roughly 100 KB to
  38 KB, `sprite.kp` from 108 KB to 34 KB. The installed SDK ships each leaf's
  interface archive under `ps1/psx/` beside `psx.a`. See `doc/tools/psx-api.md`
  and `doc/tools/psx-setup.md`.

#### Fixed
- A `reexport` clause now accepts a dotted unit name (`reexport psx.gpu`), so an
  umbrella can re-export a namespaced leaf. The clause previously read a single
  identifier and failed at the dot, though the `uses` clause already accepted
  dotted names. See the `ReexportItem` production in
  `doc/languages/kobold/kp-grammar.md`.

### 0.47.0

#### Changed
- A compiled Kobold unit on disk is now a GNU `ar` archive (`.a`) rather than a
  bare `.kpm` module. The archive carries the unit's interface in a reserved
  member named `.kpm`, so a unit can hold compiled object members beside its
  metadata; the front end and the loader read the archive and pull that member to
  resolve a `uses`. This is a hard cut: a unit is resolved only as `<name>.a`, and
  every unit build rule and test moves at once. A program module stays a bare
  `.kpm`. `skj-ar` gained a `p` operation that prints an archive member to stdout.
  See `doc/languages/kobold/kp-units.md`.

#### Added
- A Kobold host import may give its external symbol its own link name with
  `external cdecl name '<csym>'`, so the routine's Kobold name used in source and
  the C symbol it imports and links against can differ (for example a tidy `Emit`
  over the runtime's `io_write_str`). See the `HostImport` production in
  `doc/languages/kobold/kp-grammar.md`.

### 0.46.2

#### Fixed
- A no-argument function returning a record by value, called without parentheses
  (`r := make`), was addressed as if it were the enclosing function's own result
  rather than lowered as a call, so no call was emitted and the record came back
  uninitialized. The bare call now lowers exactly like the parenthesized form
  (`r := make()`), returning through the callee's result buffer. This also covers
  a bare call whose record result is immediately read (`make.field`) or passed by
  const-reference. The defect predates the single-step compiler (present since at
  least 0.45.0).

### 0.46.1

#### Changed
- `skj-kpc` now reads a prebuilt `.kpm` module as well as Kobold source, so the
  one tool covers every shape: source to native, source to `.kpm`, and `.kpm` to
  native. When handed a `.kpm` it loads the module and lowers it to assembly
  through the same AOT emit pipeline linked into the compiler. This finishes the
  single-step model: a `.kpm` is lowered through `skj-kpc`, in-tree and in a
  vendored tree.

#### Removed
- The separate `skj-kp-<arch>` AOT backend executables, and the host `skj-kp`.
  They were the last remnant of the two-stage model, a standalone reader of a
  KPM module that re-emitted assembly. That reader and the emit pipeline now live
  inside `skj-kpc`, so the standalone tools and `kobold/aot_main.c` are gone.

### 0.46.0

#### Changed
- Every Kobold compiler is now single-step. The front end builds a KPM1 module
  directly through an in-memory module builder (`kobold/kpbuild.c`) and the
  backend is assembled into the same tool at compile time, so a Kobold program
  compiles to native code in one invocation of `skj-kpc`. The old two-stage
  model, where the front end emitted KPM text and a separate backend read it
  back, is gone. The `skjcc` driver compiles Kobold source in a single step to
  match.
- The x86-64 `skj-kp-jit` links the native `kpcom` object directly instead of
  embedding a built seed. The cross JITs (`skj-kp-jit-rv`, `skj-kp-jit-arm64`)
  still carry a per-arch seed built from source at build time.
- Dynamic arrays grow geometrically with a tracked capacity, so repeated
  `SetLength` growth is amortized rather than reallocating on every step. The
  capacity is kept correct across `Delete` and `Insert` as well.
- `bootstrap-fpc` is now a dialect check: it confirms the FPC-built and
  skj-built front ends agree, with a portable external-declaration order.

#### Removed
- The KPM text emitter. Modules are only ever built in memory now, never written
  as text and parsed back.

#### Fixed
- The seeded Kobold JIT read its front end's return value as a full 64-bit word.
  `CompileModule` returns a 32-bit `LongInt`, and the high word is undefined on
  rv32 and arm64, so a clean compile could be misread as an error (and an error
  as success) on those cross JITs. The return is now masked to 32 bits.

### 0.45.0

#### Removed
- The TinC front end. `skj-tinc` and its per-backend variants (`-rv`, `-mips`,
  `-arm64`, `-x86-64`), the `.tc` test corpus, and the TinC-specific fuzz,
  sanitizer, and debug-line lanes are gone. TinC predated the C compiler and the
  Kobold dialect and overlapped both. The toolkit's front ends are now
  TinScheme, MooScript, Pascal, C, and Kobold, and TinScheme keeps the stack-ABI
  generic test lanes on every backend. `vendor.sh` no longer offers a `tinc`
  component.

### 0.44.4

#### Added
- The `skjcc` `ps1` target now builds Pascal and Kobold as well as C. The driver
  dispatches on the source file extension, so `skjcc -target ps1 -o game.exe
  game.pas` and `skjcc -target ps1 -o game.exe game.kp` build with one command,
  the same as a C source. A Kobold program resolves `uses psx` from the installed
  SDK and links the `psx_unit.o` binding automatically. The `.s` and `.kpm`
  intermediates stay in a temporary directory and are removed; `-save-temps`
  keeps them and `-S` stops at assembly.

#### Changed
- `doc/tools/psx-setup.md` now builds all three languages through the `skjcc`
  driver, with the raw tool pipelines kept only as a reference.

### 0.44.3

#### Added
- A `ps1` target (alias `psx`) in the `skjcc` driver: `skjcc -target ps1 -o
  game.exe game.c` runs the whole C PlayStation pipeline (soft-float MIPS
  compile, assemble, link the crt and `libpsx.a`, write a PS-EXE) and finds
  `psx.h` for you.
- `make install` now installs the PlayStation toolchain and the libpsx SDK: the
  MIPS compilers (`skj-cc-mips-sf`, `skj-pc-mips`, `skj-kpc-mips`,
  `skj-kp-mips-sf`), `skj-mkromfs`, and the asset converters into
  `$(PREFIX)/bin`, and `libpsx.a`, `start_psx.o`, `psx.h`, the units, and the
  Kobold unit module into `$(PREFIX)/lib/skjegg/ps1`.
- A setup guide, `doc/tools/psx-setup.md`: installing the toolchain, building the
  spinning-cube sample in C, Pascal, and Kobold, running it on an emulator, and
  an example project Makefile.

#### Changed
- Replaced the architecture diagram in `doc/tools/psx-sdk.md` with a layered
  model that states how each layer reaches the one below it.

### 0.44.2

#### Changed
- Removed the "Compact Pascal" name throughout the documentation, the source
  comments, the Makefile, and `vendor.sh`. The Pascal front end (`skj-pc`) is
  just Pascal, a Free Pascal-lineage dialect without the Delphi and Object
  Pascal object model. `doc/languages/pascal.md` is reworded to that framing,
  and `pascal/EXPANSION.md` is rewritten from a build journal into a
  current-state and roadmap document. The docs also note `skj-pc` builds for
  MIPS.
- Restored the PlayStation SDK architecture diagram in `doc/tools/psx-sdk.md` to
  the continuous-spine style and fixed its box alignment.

### 0.44.1

#### Added
- A complete `libpsx` API reference (`doc/tools/psx-api.md`): every public entry
  point with its C signature, the Pascal and Kobold calling conventions, and the
  Kobold unit name for each call.

#### Changed
- The README now describes the PlayStation SDK and points at both PSX docs.
- `doc/tools/psx-sdk.md` is refocused on the current state and the roadmap. The
  milestone-by-milestone build log is gone; the subsystem descriptions, the
  memory map, the asset path, and the testing approach stay.

### 0.44.0

#### Added
- The PlayStation 1 (R3051 / MIPS) game SDK, `libpsx`: C, Pascal, and Kobold now
  write PS1 games over the MIPS PS-EXE tier, sharing one C and assembly runtime.
  The driver set relies on documented hardware and revision-stable BIOS calls,
  not a specific BIOS. It covers the GPU (double-buffered display, a DMA-linked
  primitive chain, interrupt-driven vsync through an own exception vector), 2D
  primitives (flat, textured, and semi-transparent rectangles; sprites;
  sprite-sheet cells; rotated and scaled sprites), textures (TIM upload to VRAM),
  the SPU (ADPCM sample upload by DMA, voice key-on and key-off, ADSR envelopes,
  reverb presets, and the CD-audio mix), the CD-ROM (ISO 9660 reads through the
  shared `fs/` layer, plus CD-DA track playback and XA-ADPCM streaming),
  controllers (SIO0 digital pads and DualShock analog sticks), memory cards
  (frame I/O and a save-file manager over the card directory format), the three
  root-counter timers, fixed-point math, and 3D through the GTE (vertex
  projection, rotation-matrix builders, a depth-sorted ordering table, and flat,
  Gouraud, and textured faces with backface culling). A built-in 8x8 font draws
  on-screen debug text, and inline assembly reaches the MIPS target from C and
  Kobold.
- Pascal and Kobold units (`units/psx.pas`, `units/psx.kp`) that bind the whole
  API. Textures and the GTE 3D path are reached through integer handles and
  pointer-free helper calls, so a managed-language program draws 2D and projects
  3D with no pointer or array crossing the language boundary.
- Host asset tools, all in-tree C: `vagconv` (PCM to SPU-ADPCM), `timconv` (PPM
  to TIM, with a median-cut palette quantizer for paletted textures), `mkppm`
  (test patterns), and `mkcdda` (a two-track CD-DA test disc).
- A headless test tier on PCSX-Redux. Structural PS-EXE checks run always-on in
  `check-mips-all`; a golden-framebuffer tier captures the displayed frame with
  no window and checks pixels (`redux_shot.lua`, `shotcheck`), and a golden-audio
  tier captures the SPU output and checks its pitch (`wavcheck`). The functional
  runs are opt-in and need a BIOS.

#### Fixed
- cc: a static multidimensional array of a sub-word element type (for example
  `short v[8][3]`) is laid out at the element's width instead of word width, so
  indexing reads the intended values rather than padding. The fix is in the
  shared lowering, so it covers every backend.

### 0.43.1

#### Changed
- The sanitizer suite is split by target architecture so CI runs it in
  parallel. The Makefile's `check-san` is now the union of per-backend slices
  (`check-san-cf`, `-rv`, `-x86-64`, `-mips`, `-arm64`) plus an arch-neutral
  `check-san-fuzz`, and the CI sanitizer job fans those slices out as a matrix
  so no single slice is the critical path. `check-san` and the local gate are
  unchanged.
- Documentation refresh. The README and reference manual group the tool list
  (ahead-of-time compilers, in-process JITs, the binutils-free assembler and
  linker family, and the runner/sandbox/debug tools), add the x86-64 and
  AArch64 toolchains, and gain sections on the `skj-run` sandbox emulator and
  KVM/HVF hypervisor, the shared small libc, and the JIT runtime. The vendoring
  section documents the current `--arch`/`--frontend`/`--with` interface. The
  internals docs drop the retired 32-bit x86 backend and complete the
  per-backend register and ABI tables. Published docs no longer carry internal
  consumer-project names.

### 0.43.0

#### Changed
- `vendor.sh` builds the Kobold front end from `kpcom.pas` source for any
  target, in place of the single prebuilt x86-64 seed it used to ship. The
  Kobold component now vendors the Compact Pascal compiler and its host runtime,
  and the generated `skjegg.mk` builds `skj-pc` for the host and compiles
  `kpcom.pas` into a per-target `skj-kpc-<target>` (rv32, ColdFire, x86-64,
  arm64), each a host binary that emits its one target's KPM1. A consumer
  targeting a VM host now gets a correct-width front end built from source
  rather than a mismatched seed. The vendored host backend is x86-64; building
  on an arm64 host needs the arm64 selector vendored too.
- A MooScript verb call now carries a per-argument routing mask so a host bridge
  can place values into the verb's context. `__moo_verb_call` takes a new
  `typemask` argument, two bits per call argument (object, string, or anything
  else), letting the host route object and string arguments without guessing
  from the raw temporaries.

#### Fixed
- `vendor.sh` now ships the Kobold runtime glue that lives under `runtime/`
  (`rng_glue.c`, `sched_glue.c`, and the per-architecture `io_free_*.S`). A
  consumer building the freestanding Kobold runtime (the rv32 VM hosts)
  previously had to supply these by hand.

### 0.42.1

#### Fixed
- The host Pascal runtime build (`runtime/pascal_rt.c` with `-DPAS_HOST`) no
  longer warns about ignoring the result of `write()`, which the host libc
  marks `warn_unused_result`. Output now funnels through one helper that checks
  the result and advances over a short write; the freestanding paths share it
  too, so a partial write completes there as well.
- The PDF build of the runtime-layering manual page no longer fails: a few
  space-padded slash separators between inline code spans could wrap onto a
  line starting with a slash and a space, which the PDF typesetter reads as a
  term-list item. They are tightened so the manual's PDF target builds.

### 0.42.0

#### Added
- `make install` and `make uninstall` install the toolchain under a `PREFIX`.
  The `cc` driver resolves its runtime objects and headers relative to the
  installed executable, so an installed tree needs only `PATH` set, not a
  fixed layout.
- A host-served `proc:` pseudo filesystem exposes the `skj-run` sandbox
  process table. It is always mounted and read-only: the root lists one
  directory per live process (named by id) plus a `self` alias, and each
  `<id>/status` carries the process metadata the scheduler already tracks
  (pid, state, parent, retired instruction count, and exit code). A standalone
  `ps` tool reads it and prints the table. Content is generated on demand into
  a growable buffer, a reusable mechanism future pseudo files can build on.

#### Changed
- The hosted Kobold file and directory runtime now routes through the shared
  `fs/` layer in the in-process JIT and the ahead-of-time tiers too, completing
  the stream-primitive unification begun in 0.41.0. The dead native-libc branch
  is gone and the runtime layering, the shared bottom layer and its two
  deliberate seams, is documented.
- The three gdb remote-protocol stubs (RISC-V, MIPS, and the
  hardware-virtualization core) are factored onto one shared transport and
  watchpoint module, `emu/gdbstub.c`. Each stub is now a thin per-architecture
  adapter supplying a target vtable; behavior is unchanged.
- The RISC-V interpreter (`rv_run`) gains a decoded-operand cache and threaded
  (computed-goto) dispatch, about 9% faster on the kpcom self-compile.

#### Fixed
- The self-hosted C compiler build no longer requires `readlink` or `access`;
  `vendor.sh` ships `ir/exepath.h` and builds the vendored skjcc with its
  include path, so a vendored, self-hosted driver links.

### 0.41.0

#### Fixed
- The hardware-virtualization (HV) cores now park on a blocking syscall and let
  the scheduler run a sibling, instead of stalling the one host thread that every
  guest in a sandbox shares. A multi-guest HV program that pipes between its
  guests (the `jsh` shell and its coreutils running under KVM) no longer
  deadlocks when a reader waits on a writer, a writer fills a pipe, or a guest
  sleeps or yields. This also removes an intermittent hang in the HV test tier.
  The full-pipe write case is fixed on the emulated cores too.

#### Changed
- The hosted Kobold file and directory runtime is one shared source across the
  in-process JIT, ahead-of-time, and freestanding tiers, in place of three
  near-identical copies. Behavior is unchanged; only the per-tier differences
  (the allocation model and the filesystem-interpose seam) stay separate.

### 0.40.0

#### Added
- `vendor.sh` ships `setjmp`/`longjmp` in the per-target `libc.a`, so a vendored
  C program that uses them (the jsh shell) links.

#### Changed
- Host-native cross compilers are the default test path. The C in-process cross
  JITs that ran a compiler itself under qemu (`check-jit-rv`, `check-jit-arm64`,
  their disassembly siblings, and the arm64 JIT trap and high-memory tests)
  moved out of `check-all` into `check-emu-all`, matching what the Kobold side
  already does, so `check-all` now runs no compiler under emulation. The
  host-native `check-cc-rv` and `check-cc-arm64` keep the C corpus there.
- The Kobold tutorial (`doc/languages/kobold/kp-tutorial.md`) is reworked into a
  22-step path over the complete implemented language, each step a complete
  program verified under the in-process JIT (or the ahead-of-time path units and
  inline assembly require). New steps cover the scalar type set, constants,
  `case`, `out` parameters, overloading, forward declarations, nested routines,
  `with`, variant records, methods, dynamic arrays, open-array parameters, maps,
  string interpolation, `for`-`in`, properties, `defer`, failable routines,
  computed goto, units with the standard library, and the compiler directives.

#### Fixed
- The C compiler (`skj-cc`) folds three constant-expression cases in a
  file-scope or static-storage initializer that it previously got wrong, on
  every backend. `sizeof` of an expression operand no longer defaults to `int`
  (it was folding to 4); an array bound deduced from a string-literal
  initializer (`char x[] = "hello"`) is carried into the type, so `sizeof` is
  the string length plus the NUL rather than garbage; and an enumerator
  initializer evaluates the whole constant expression, so a computed enumerator
  (`1 << n`, `A | B`, a reference to a prior enumerator) is correct rather than
  0.
- A 9P frame-length integer overflow in both the server's batched message
  dispatcher and the `ninefs` client's reply cursor. A peer could choose a
  later frame's declared size to wrap past the 32-bit maximum and slip it past
  the length guard, reading past the buffer. Both now compare against the
  remaining length.
- A confined guest (one given any mount or grant) refuses the ambient
  `AT_FDCWD` syscall path, which previously let a hostile guest reach any host
  path with a raw `openat(AT_FDCWD, "/abs", ...)` despite the capability mounts.
- `hv_run` no longer arms the per-vcpu preemption timer or reads the fault PC on
  a null vcpu when hardware-virtualization startup failed, which crashed the
  whole `skj-run` process instead of degrading to a clean halt.
- Four correctness bugs across the RISC-V, MIPS and HV gdb remote stubs:
  single-step now checks write watchpoints; the register-write (`G`) packet
  reports an error on short or non-hex data instead of a false `OK`; a MIPS
  guest can be resumed after a trap; and the HV stub clears its sticky fault
  flag on resume, rather than reporting the same fault forever.
- Guest-side filesystem hardening. The romfs parser validates an untrusted
  image (every node's name, type, parent index and data range, with 64-bit
  arithmetic). ramfs fixes the unlink-while-open slot-reuse aliasing, an
  `int` write-offset truncation, a capacity-doubling overflow, and `rename` into
  a node's own subtree. ninefs enforces the target kind in `unlink` and `rmdir`.
- The scheduler records a child's spawner by handle, not by slot, so a
  reaped-and-reused parent slot is recognized and the orphan is reaped rather
  than leaked and falsely reparented; `wait` and `kill` ownership is exact
  across slot reuse.

### 0.39.0

#### Added
- The gdb remote stubs report the floating-point registers. The RISC-V stub
  advertises the 32 `f` registers and `fcsr` in its target description, the MIPS
  stub appends the 32 FP registers and `fcsr`/`fir` to its `g` packet, and the
  HV stub returns the x86-64 SSE / AArch64 SIMD register file, so a debugger can
  read float state on every tier, not only the integer registers.
- The gdb remote stubs honour write watchpoints (`Z2`/`z2`). A `watch`
  expression now stops the guest when the watched address is written, on the
  RISC-V, MIPS and HV stubs.
- A spawned child's fault backtrace is symbolized against the child's own ELF
  image, not the root process's. Each process keeps its own DWARF reader, so a
  crash in a program a guest spawned resolves to that program's file and line.
- Kobold gains `io.textfile`, a unit that provides the classic Pascal file
  verbs (`Assign`, `Reset`, `Rewrite`, `Read`, `ReadLn`, `Write`, `WriteLn`,
  `Eof`, `Eoln`, `CloseFile`) over the shared POSIX file seam, including a
  character-level `Read` built on a one-character look-ahead.

#### Changed
- Kobold console number output routes through the shared C-runtime formatters
  (`__fmt_udec` and `__fmt_greal` in `libc/numfmt.c`). The per-tier number
  formatting in `kp_io.c` is gone, so a Kobold program and a C program print an
  integer or a real the same way, with no libgcc 64-bit-divide or
  double-to-u64 helper pulled on the freestanding tier.
- The documentation set is swept current with the shipped debugger, the LP64
  migration, the shared number formatter, the ColdFire control-register decode,
  and the Pascal record field alignment.

#### Fixed
- Kobold console real output spells the non-finite values. A positive or
  negative infinity now prints as `inf` or `-inf` and a NaN as `nan`, rather
  than a garbage digit string from feeding a non-finite value to the finite
  formatter.
- The ColdFire core implements the `PULSE`, `WDDATA` and `WDEBUG` debug
  instructions instead of faulting on them, so code that emits them runs under
  the emulator.

### 0.38.0

#### Added
- Two in-tree ELF64 static linkers, `skj-ld-x86-64` and `skj-ld-arm64`, so the
  64-bit targets no longer need an external `ld`. They read the ELFCLASS64
  relocatable objects the in-tree assemblers emit, pull members from `ar`
  archives, and write a runnable `ET_EXEC`. Both share one ELF64 core over the
  existing layout engine, the script parser and the mapfile reader, differing
  only in the machine id and the relocation set: x86-64 patches absolute and
  PC-relative byte fields, while arm64 encodes the bit-packed instruction
  relocations (the 26-bit branch, the conditional branch, the ADRP page split
  and the ADD low-12). Executable addresses stay in the low 4 GiB, so the
  32-bit layout core is reused unchanged. `skjcc -target x86-64` and
  `-target arm64` now link with these, so the whole 64-bit compile-to-executable
  path runs on in-tree tools with no external assembler or linker.
- The scheduler can load a process-tree root from an in-memory ELF image, not
  only a file path. A host that already holds a guest executable in memory (a
  freshly built image, a buffer received over a connection) hands the bytes to
  `sched_add_root` through `image` and `image_len` on the root spec, and
  `skj-run --mem-root` drives it from the command line. This is the portable
  path a host reaches for instead of a non-portable in-memory file descriptor.
- Per-arch accessors for the CPU operation vtables (`skj_ops_rv32`,
  `skj_ops_coldfire`, `skj_ops_mips32`, `skj_ops_x86_64hv`, `skj_ops_arm64hv`),
  so a host embedding the scheduler can obtain a core's ops without reaching
  into emulator internals.

#### Fixed
- `skjcc -g` now emits debug information. The driver silently dropped `-g`
  along with the optimization and warning flags it does not forward, so a `-g`
  build through the driver produced a binary with no `.debug_*` sections at all,
  only symbol names. The flag now reaches the compile stage, so a driver-built
  `-g` program carries DWARF that `skj-addr2line` and a fault backtrace resolve
  to file and line.
- Compact Pascal record fields now align to their own natural boundary rather
  than to the target pointer width. On the LP64 targets (x86-64 and arm64) a
  4-byte `integer` field was aligned to 8, so a record of integers sized larger
  than it should and its field offsets diverged from the 32-bit targets. Each
  field now aligns to a power of two no larger than the field, capped by the
  `{$A}` alignment, so an integer field aligns to 4 on every target while a
  pointer, `Int64` or real still aligns to 8 where the ABI needs it.

#### Changed
- `vendor.sh` can ship more of the debugging tier: the gdb stubs, the symbolized
  fault backtrace and their docs, and `skj-addr2line` (the offline DWARF
  symbolizer) as a selectable `addr2line` component, so a consuming project can
  debug a guest and resolve a backtrace's addresses without the host toolchain.
- `vendor.sh` can ship the whole 64-bit C target. `--arch x86-64` and
  `--arch aarch64` select the LP64 cc backend (parallel to the 32-bit arches),
  `--with toolchain` adds the ELF64 assembler and linker (`as-x86-64`/`ld-x86-64`
  and `as-aarch64`/`ld-arm64`), and `--with runtime` stages the crt and
  varargs/`_Float16` helpers, so a vendored `skjcc -target x86-64` or
  `-target arm64` compiles, assembles and links C with no external toolchain.
  The runtime is self-hosted: the vendored build compiles the helpers with
  `skj-cc-*` and assembles them with the in-tree assemblers.

### 0.37.0

#### Changed
- The hardware-virtualization preemption timer is now per-vcpu, so several HV
  sandboxes can run at once, each on its own host thread. A compute-bound guest
  (one that never traps to a syscall) is bounded by a time-slice timer; that
  timer was a single process-global interval timer delivered to an arbitrary
  thread, so two sandboxes stomped each other's timer. Each vcpu now drives its
  own timer, targeted at the thread that runs it (a per-thread `timer_create` on
  Linux/KVM, a per-vcpu watcher thread on macOS/HVF). A host can run many
  sandboxes in parallel, one per connection, by calling the sandbox entry on its
  own threads. Single-sandbox behavior is unchanged.

### 0.36.0

#### Added
- The process scheduler is now a reusable, embeddable API. A host program can
  drive many sandbox process trees from its own event loop, each tree bound to
  its own input and output, which is what a shell server that runs one sandbox
  per telnet or websocket connection needs. `sched_init`, `sched_add_root`,
  `sched_step`, `sched_run`, and the wait and ready primitives are the surface,
  and `run_sandbox` is now a thin wrapper over them, so the command line behaves
  exactly as before. Each root carries its own argument vector, environment, and
  standard file descriptors.
- `skj-run --env KEY=VALUE` (repeatable) passes an environment to the guest. It
  reaches `getenv` on every target, little-endian and big-endian, 32-bit and
  64-bit, because the stack builder lays the environment pointers in the guest's
  own width and byte order. The default is still an empty environment, so an
  existing run is unchanged.
- The vDSO now runs on all three emulated cores. ColdFire and MIPS join RISC-V,
  and the x86-64 and arm64 hardware-virtualized guests already had it, so the
  feature now spans every target `skj-run` hosts. ColdFire is big-endian and
  MIPS little-endian, so the image loader and the system-page clock writer are
  proven in both byte orders.
- `vendor.sh` ships the vDSO image source and builds it for each emulated core
  whose toolchain is vendored, so a consuming project gets a working `--vdso`
  out of the box.

### 0.35.0

#### Added
- `skj-run --vdso IMG` injects a slice of runtime into every process, the way
  Linux hands a process its vDSO. The image is a small read-only block of pure
  leaf functions. The host advertises its export table through a private auxv
  tag, and the guest walks its auxv to find it and calls functions through an
  ordinal table. One image is built once and shared read-only across every
  sandbox: an emulated guest aliases the one host buffer through its software
  memory path, a hardware-virtualized guest backs a read-only hypervisor memory
  slot with it. A guest write faults, so one sandbox cannot corrupt the image
  another shares. Supported on the emulated RISC-V core and the x86-64 and arm64
  KVM/HVF guests.
- The vDSO image carries a system page the host keeps updated and the guest
  reads with no syscall or doorbell, in the spirit of a QNX system page: a
  monotonic and a realtime nanosecond clock the scheduler refreshes before each
  slice, the host page size and cpu count, and reserved room for more such
  state. A guest reads the clock through the vDSO functions or reads the page
  directly, whose address the export table carries.
- The `jsh` userland and its coreutils now run on the emulated MIPS tier too, so
  the shell runs on all five targets `skj-run` hosts: RISC-V, ColdFire, MIPS,
  and the x86-64 and arm64 hardware-virtualized guests. The MIPS runtime gained
  the `nanosleep` and `sched_yield` wrappers the shell needs.

### 0.34.0

#### Added
- `skj-run --mount-ro NAME:DIR` grants a confined host directory read-only, so a
  mutating operation returns `EROFS`. The `jsh` userland serves its tool images
  (`bin:`) this way, and the per-sandbox scratch uses the same capability path.

#### Changed
- The `skj-run` userland no longer carries an in-guest filesystem for its default
  mounts. `skj-run` owns them host-side through the confined capability path it
  already used for `--mount`: `scratch`, the spot a bare path resolves against,
  is an ephemeral host directory granted per sandbox, and `bin:` is served as a
  read-only host directory. The in-guest ramfs and romfs are now compile-time
  options (`FS_WITH_RAM`, `FS_WITH_ROM`) kept for the standalone VM tiers and
  bare-qemu, and are compiled out of the `skj-run` build. The guest fs code there
  is now just the mount switch and the hostcap adapter.
- The `jsh` userland links against a `libc.a` archive instead of a list of loose
  objects, so each program pulls only the archive members it references. With the
  host-owned filesystem, the coreutil set shrank about 65% (each fd-using tool
  from roughly 135 KB to 52 KB on RISC-V, and `sleep`, which opens no file, to
  under 5 KB).
- Each sandbox, whether the root or a spawned child, now gets its own fresh
  scratch directory. Scratch is never shared across the process tree, matching
  the per-process isolation the in-guest ramfs provided.

#### Fixed
- `run_sandbox` releases the root process's host fds and removes its scratch
  directory at teardown, not only a reaped child's.

### 0.33.0

#### Added
- A process model for the `skj-run` emulator: one sandbox per process. Four
  emulator-private syscalls build it up. `SKJ_SYS_SPAWNV` spawns a child from a
  size-prefixed descriptor in guest memory, carrying the child image, its argv,
  a flags word, and an fd map. `SKJ_SYS_WAIT` returns a child's exit status and
  reaps it. `SKJ_SYS_KILL` terminates a child by handle. `SKJ_SYS_PIPE` creates
  a host-backed pipe as two capability fds. Each spawned process gets its own
  guest memory and its own capability and filesystem view, so isolation is the
  default, not an accident. The spawn descriptor is a size-prefixed row of
  pointer-width words, so a single layout serves the ILP32 tiers (ColdFire,
  RISC-V, MIPS) and the LP64 tiers (x86-64, arm64).
- Mount and descriptor inheritance across a spawn. A parent hands a child its
  mount namespace (host and dev grants) and an fd map through the spawn
  descriptor, host-mediated so neither side marshals the other's private fd
  table. This is how a spawned command reaches the same files and how a shell
  wires a child's stdin, stdout, and stderr.
- A mini unix userland: the `jsh` shell plus coreutils (`cat`, `echo`, `ls`,
  `rm`, `sleep`, `cp`, `mv`), built as guest ELFs over skjegg's libc and the
  `fs/` layer. jsh resolves a bare command name in a romfs mounted at `bin:`,
  then spawns a fresh sandbox per command. With no fork and no exec, a redirect
  is "open the target and place its capability in the child's fd map", and a
  pipeline is a host pipe wired through the same map. It runs on all five tiers:
  ColdFire, RISC-V, and MIPS, and the x86-64 and arm64 hardware-virtualization
  tiers.
- The console descriptors (0, 1, 2) are bound into the one fd space as ordinary
  stream fds. Each sandbox owns its own duplicates, so close and inheritance are
  uniform with every other descriptor.
- LP64 `setjmp` / `longjmp` for the hardware-virtualization tiers (x86-64 SysV
  and aarch64 AAPCS), which the userland needs. [x86-64, arm64]

#### Fixed
- The LP64 initial stack the hardware-virtualization guests receive under-reserved
  its auxv by one slot pair, so the terminating entry could overrun the argv
  string area. Whether it corrupted an argument depended on alignment, so it
  struck only particular argv layouts. The reservation now matches the words
  written. [x86-64, arm64]
- The dynamic `mmap` area for a hardware-virtualization guest now stays inside
  the guest's RAM instead of starting at the paged default address, which lay
  past the end of that RAM. A rom mount, a 9P channel, a pipe buffer, or a guest
  `mmap` reached that address and faulted. [x86-64, arm64]

### 0.32.0

#### Added
- Cooperative yields for blocking guest syscalls in the `skj-run` emulator. A
  `read` on a would-block descriptor, `nanosleep`, and `sched_yield` now park
  the calling guest and hand the single host thread back to the scheduler, which
  blocks in `poll()` only when no guest can run. A blocking call no longer
  stalls the whole cooperative sandbox. The hardware-virtualization cores keep
  their plain blocking behavior. [ColdFire, RISC-V, MIPS]
- 9P request batching in the sandbox filesystem client. The hot `fs/` composites
  now cost one doorbell round trip each instead of two or three: `stat` /
  `size` / `exists` / `isdir`, `open`, `unlink` / `rmdir`, and `mkdir`. The 9P
  server processes a pipeline of framed messages on one session, so a `Twalk`
  and the operation on the fid it binds travel in the same round trip. A deep
  path past the 16-element walk limit keeps the stepwise path.
- `ROOT:` names the default root. Mount index 0 is the default root, and `ROOT:`
  (any case) selects it without a program having to know the real mount name,
  the DOS single-drive convention. It resolves only after an exact mount-name
  match fails, so it never shadows a mount actually named `ROOT`.
- `std.time` and `std.random` glue linked into the RISC-V and arm64 Kobold JITs,
  extending the hosted-tier coverage added in 0.31.0 to those JIT targets.

#### Fixed
- The 9P sandbox server now rejects a truncated request with `Rerror` instead of
  decoding its missing fields as zero and acting on them. Before, a short
  `Tclunk` clunked fid 0, a short `Tversion` reset the session, and a short
  `Twalk` bound a fid, all from malformed input.
- `skj-cc` grows its preprocessed-source buffer to 2 MiB, so a translation unit
  that embeds a large guest image as a byte array (the sandbox spawn demos)
  compiles again after the embedded Kobold child crossed 1 MiB.

### 0.31.0

#### Added
- Ordinal-keyed maps. A `map[K] of T` is keyed by an ordinal `K` (integer,
  int64, char, enum, or boolean) alongside the existing string-keyed `map of T`.
  Indexing, `in`, `Delete`, `TryGet`, `Length`, `MapFirst` / `MapNext`, and
  `for k, v in m` all work on both. The two are distinct types with distinct
  runtime representations, and value (copy-on-assign) semantics are unchanged.
- Handle-typed and aggregate-typed properties. A `property` may now carry a
  handle value (string, dynamic array, map, interface, or process) or an
  aggregate value (record, fixed array, or large set), extending the scalar
  properties shipped earlier. The accessor calls and uniform-access rules are
  the same.
- Extended inline assembly. The `asm(...)` statement accepts the GNU extended
  form `asm(template : outputs : inputs : clobbers)` in a simple subset: the
  general register class (`=r` output, `+r` read-write output, `r` input) over
  integer-class operands, plus a clobber list, with `%N` substitution in the
  template. The register allocator places each operand and reserves the
  clobbers. The basic verbatim form is unchanged. Ahead-of-time only.
- `std.time` and `std.random` link on the hosted AOT and JIT tiers, so `Now`,
  `Sleep`, and the random surface are available there as well as on the
  freestanding tiers.

### 0.30.0

#### Added
- Source-level debug information carried through the arm64 macOS toolchain.
  `skj-cc-arm64 -g` emits a DWARF 4 line table that `skj-as-aarch64` assembles
  into `__DWARF` sections and `skj-ld-aarch64` places in a `__DWARF` segment of
  the linked executable, inside the ad-hoc code signature. The table carries a
  multi-file file list, so a `static` function defined in a header is attributed
  to that header rather than the including source, and records source columns.
  Apple `dwarfdump` and `llvm-dwarfdump --verify` read it, and `lldb` resolves
  breakpoints and source lines against it.
- Full `make all` self-hosting on a Darwin arm64 host. Beyond the C toolchain,
  the Kobold front ends and the in-process JITs build and run natively on macOS
  with clang as the only external C compiler (no Apple `as`/`ld` and no cross
  tools); the Kobold AOT lane assembles with the in-tree `skj-as-aarch64`.

#### Changed
- The arm64 Mach-O linker emits dyld chained fixups (`LC_DYLD_CHAINED_FIXUPS`)
  in place of the classic bind and rebase opcode streams, and places the `__got`
  and the pointer-bearing const data in a read-only `__DATA_CONST` segment that
  dyld maps writable, fixes up, then protects.

#### Fixed
- C compiler `-g` line numbers were wrong for any translation unit with an
  `#include`: a statement was numbered by its position in the fully preprocessed
  stream, which counts the spliced-in header text, not by its source line. The
  preprocessor now records each emitted line's origin and the compiler numbers
  by it, so the line table is correct on every backend.
- `skj-as-aarch64` miscompiled a symbol spelled like a register, for example a
  function named `s8`: it was taken as the register, so a branch to it lost its
  target and an address calculation using it became a register operation.

### 0.29.0

#### Added
- A macOS Hypervisor.framework backend for the `skj-run` VMM, the peer of the
  Linux KVM backend. On an Apple Silicon host an arm64 guest runs natively under
  Hypervisor.framework, driven through the same backend-neutral VMM seam, syscall
  doorbell, and scheduler as the KVM path; the backend is chosen by the host OS.
- Native macOS (Mach-O) code generation and a complete in-tree Mach-O toolchain
  for arm64. `skj-cc-arm64` emits Mach-O-assemblable AArch64, `skj-as-aarch64`
  writes Mach-O `MH_OBJECT` relocatables, and `skj-ld-aarch64`, a new in-tree
  Mach-O static linker, lays out a PIE `MH_EXECUTE`: it resolves symbols, binds
  libSystem imports through arm64 stubs and a `__got` with dyld bind opcodes,
  applies the arm64 relocations, and self-signs with an ad-hoc SHA-256 code
  signature. The whole cc corpus and the computational libc phases compile and
  run natively on Apple Silicon through the fully in-tree pipeline (`skj-cc-arm64`
  then `skj-as-aarch64` then `skj-ld-aarch64`), with no Apple assembler or linker.
  Linking is hosted on libSystem.
- The Darwin AArch64 variadic ABI, on both the caller and the callee side: the
  variadic arguments of a call go on the stack, and a variadic function walks
  them there with no register save area, so C programs that call or define
  variadic functions build and run on macOS.
- macOS cc and libc test lanes, `check-cc-macho-arm64` and
  `check-libc-macho-arm64`, that compile, link, and run natively on a Darwin
  arm64 host through the in-tree toolchain and skip elsewhere.
- Host-architecture detection in the build, so a plain `make` on a non-x86_64
  host (arm64 Linux or macOS) builds the native in-process JITs and host tools
  for that architecture instead of the x86-64 ones.
- Documentation of the VMM sandbox prerequisites and setup for each platform and
  architecture.

#### Changed
- The AArch64 object format and ABI are fixed when a tool is built, not selected
  at run time (one binary per target): a native macOS build targets Mach-O and
  the Darwin ABI automatically, a cross build selects them with `-DSKJ_DARWIN`.
- The arm64 identity-MMU bring-up for the KVM backend brings the Linux x86-64 and
  AArch64 hardware-virtualization backends to full parity, so an AArch64 guest
  runs correctly under KVM on an AArch64 host.

#### Fixed
- The hardware-virtualization test targets skip cleanly, rather than failing,
  when `/dev/kvm` exists but is not accessible to the user.

### 0.28.0

#### Added
- A hardware-virtualization tier in `skj-run`: x86-64 and AArch64 guests run
  natively under the host hypervisor (Linux KVM today) instead of being
  interpreted, driven through the same process sandbox and scheduler as the
  emulated cores. The guest architecture equals the host's, so an x86-64 host
  runs x86-64 guests and an AArch64 host AArch64 guests; off a supported host
  the tier reports the VMM unavailable and fails cleanly. The VMM abstraction is
  backend-neutral, with the Linux KVM backend in place and a macOS
  Hypervisor.framework backend planned as a peer.
- The guest reaches the host through a syscall doorbell (one unbacked MMIO read
  carrying a Linux syscall in the platform's argument registers), so the whole
  freestanding C libc, string, formatted I/O, `malloc`, stdio, the math helpers,
  delimited continuations, cooperative threads, and file and directory I/O over
  the `fs/` layer, runs under the VMM, built by `skj-cc-x86-64` or
  `skj-cc-arm64` and linked with the hardware crt and the doorbell seam.
- A compute-bound guest under the VMM is preemptible. The hypervisor cannot
  count retired instructions, so the slice currency is wall-clock time: a
  periodic timer breaks into the guest every 10 ms, which keeps the round-robin
  scheduler fair and makes `--max-insns` bound a runaway guest.
- A GDB remote-protocol stub for the hardware-virtualization cores, served by
  `skj-run --gdb`: breakpoints, single-step, register and memory access over the
  same scheduler hook as the RISC-V and MIPS stubs, with the vcpu single-stepped
  through the hypervisor so guest memory is never patched.
- The `fs/` sandbox mount table is complete. A devfs backend (`null`, `zero`,
  `full`), a romfs backend (a read-only packed image), and a 9pfs backend (a
  9P2000 client over a transport) join ramfs and the confined host backend, each
  wired into the guest libc and the launcher through `--dev`, `--rom` and
  `--9p`.
- `std.threads`, a Kobold cooperative-thread unit that parallels C
  `<threads.h>`, over the shared runtime.
- `skj-as-x86-64` folds constant and label-difference expressions (for example
  `(end - start) * 4 + 1`), matching the GNU assembler byte for byte.

#### Fixed
- The guest `sched_yield` is routed through the host-platform seam. Its
  `#include <sched.h>` had resolved to the emulator's own scheduler header on
  the include path, so the C library prototype was never seen and the call
  compiled with an implicit declaration.

### 0.27.0

#### Added
- An in-tree MIPS I (R3000-class) user-mode core in `skj-run`, little-endian
  o32. It runs MIPS guests at parity with the RISC-V and ColdFire cores: the
  integer core, the Linux syscall surface, fault reporting with a symbolized
  DWARF backtrace, and the COP1 floating-point unit (single and double
  precision, the condition flag and rounding modes) that the hardware-float
  MIPS backend targets. The MIPS toolchain now self-hosts under `skj-run`, not
  only under qemu: the in-guest compiler builds a program through the sandbox
  syscalls and emits assembly byte-identical to a qemu run.
- The MIPS process sandbox: guarded `mmap`, `munmap` and `spawn`, a round-robin
  scheduler and a shared memory budget, at RISC-V parity.
- A confined `fs/` capability filesystem libc for MIPS: named mounts, the
  `hostcap` backend and `--mount` grants, matching RISC-V and ColdFire.
- A GDB remote stub for MIPS (`skj-run --gdb PORT`), the sibling of the RV32
  stub: registers, memory, breakpoints, continue, step and the fault signal
  over the remote-serial protocol.
- Numeric local labels (`1:` with `1f` and `1b` references) in `skj-as-mips`,
  and a wider encoding golden corpus checked against GNU `as`.
- Selection axes for `vendor.sh` (`--arch`, `--frontend`, `--with`) and a
  packaged `sandbox` component that builds a confined hosted libc. A vendored
  tree now builds the crt, driver and confined libc and runs a confined guest
  end to end. `make check-vendor` vendors the checkout and proves it, as a CI
  smoke test.

#### Changed
- The process sandbox and scheduler are one arch-neutral implementation
  (`emu/sched.c`) driven through a CPU-handle vtable, shared by the ColdFire,
  RISC-V and MIPS cores, in place of the earlier per-core copies.
- The ColdFire and MIPS cores use a standardized batched run-with-reason loop
  and a reserved host-call selector for cooperative yield, matching RISC-V.

#### Fixed
- `skj-as-mips` encodes the REGIMM `bgez` and `bltz` branches.

### 0.26.0

#### Added
- `skj-as-x86-64`, an in-tree x86-64 assembler. It reads GNU `.intel_syntax
  noprefix` and writes ELF64 relocatable objects, and is now the default
  assembler for the x86-64 toolchain, so a build no longer needs `nasm`.
- `skj-as-aarch64`, an in-tree AArch64 assembler. It reads the native GNU
  AArch64 assembler syntax and writes ELF64 relocatable objects, and is now the
  default AArch64 assembler, so assembling the toolkit's own AArch64 output no
  longer needs an external assembler.
- Inline assembly in TinC (`asm("text")`) and Kobold (`asm('text')`), joining
  the C front end. The one string literal is emitted verbatim into the generated
  assembly; there are no operand constraints and no interpolation. Inline
  assembly is ahead-of-time only, since the in-process JIT does not assemble
  text.

#### Changed
- The x86-64 code generators and the hand-written x86-64 runtime now emit GNU
  `.intel_syntax` assembly. `nasm` is retired: it is no longer used by the
  build, the test suite, or CI.
- The JIT reports a program that uses inline assembly with a clear message that
  it must be compiled ahead of time, instead of a misleading "unresolved
  symbol".

### 0.25.0

#### Added
- A `writev` wrapper on the shared freestanding syscall seam (number 66 on
  RV32, 146 on ColdFire) with `struct iovec` in `<sys/uio.h>`. The buffered
  stdio core uses it to send a stream's pending buffer and a large `fwrite`
  block in one call on the freestanding tiers, instead of draining the buffer
  and then the block as two writes. The other targets fall back to two `write`
  calls, which is still fewer syscalls than the character path. The deliberately
  unbuffered Kobold console is unchanged.
- The skjegg/boris hosting contract, `doc/skjegg-kobold-requirements.md`. It is
  the authoritative handoff: the raw Linux RV32 syscall seam skjegg commits to
  (kept at the real Linux numbers), the name-bound host-call seam boris owns, and
  the status of boris's follow-on wishlist. It records that full hosting of the
  compiler and toolchain inside the rv32 sandbox is confirmed, proven in-tree by
  `make check-kp-free`.

#### Changed
- Pointer width now derives from one axis. The `KP_LP64` build macro is gone;
  whether a tool is LP64 or ILP32 follows from its instruction-set selector
  through the single `SKJ_LP64` fact in `ir/target.h`, so a tool can no longer
  name a 64-bit backend yet stay ILP32, the mismatch that once bit the Kobold
  JIT. On AArch64 every build is LP64, so the backend now uses a uniform 64-bit
  integer-register home unconditionally rather than a per-build opt-in, which
  also lets the arm64 C JIT map its guest memory anywhere instead of only in the
  low 32-bit address space.

### 0.24.2

#### Fixed
- The continuous integration job that proves the compiler is independent of its
  build compiler now installs `nasm`. With the committed seed gone, that job
  rebuilds the reference RISC-V seed from source, which assembles the x86-64
  front end's NASM output, so it needs `nasm` as well as Free Pascal.

### 0.24.1

#### Fixed
- A clean build of the `bootstrap-fpc` path failed with `version.h: No such
  file or directory`. The `skj-pc` LP64 host variants (`skj-pc-x86-64` and
  `skj-pc-arm64`) compile a source that includes the generated `version.h` but
  were not registered as needing it, so a build that reached them without
  building the main tool set first (as `bootstrap-fpc` does when it generates
  the RISC-V seed from source) did not generate the header in time.

### 0.24.0

#### Added
- Per-architecture Kobold host front ends. Alongside `skj-kpc` (x86-64) there
  are now `skj-kpc-rv`, `skj-kpc-cf`, and `skj-kpc-arm64`. Each is the Kobold
  compiler built for one target: it accepts source and emits a module for that
  target only, the way the C compiler has one tool per target.

#### Changed
- A Kobold unit module (`.kpm`) now records its target architecture on the
  `.target` line (`rv32`, `cf`, `x86-64`, `arm64`), and the backend rejects a
  module built for another architecture. A unit carries IR, native code, and
  type definitions bound to one target, so it was never portable between
  architectures; the reader now enforces that instead of checking pointer width
  alone.
- The Kobold compiler no longer ships a committed bootstrap seed. Each
  architecture's seed is generated at build time from that architecture's front
  end, and `vendor.sh` generates the seed when it copies the compiler into a
  consumer project.
- Removed `skj-kpc-32`, the shared front end that emitted one module fed to both
  the RISC-V and ColdFire backends. RISC-V and ColdFire builds use their own
  per-architecture front ends.
- The documentation now states that Kobold source uses `.kp` and Compact Pascal
  uses `.pas`, with `kobold/kpcom.pas` (the self-hosting compiler, valid in both
  languages) the one dual-compilable exception.
- The libc file-operation diff tests compare against a native host oracle rather
  than a fixed expected file.
- Test scratch directories are isolated per mode and per runner so a parallel
  `make -j` run does not let one target's modules clobber another's.

#### Fixed
- A coroutine value word is width-tagged, so a coroutine that carries a 64-bit
  or pointer value through `yield` and resume is handled correctly on every
  target.

### 0.23.0

#### Added
- A shared `fs/` filesystem layer: a flat, DOS-style mount table of named
  namespaces (not a VFS), with a `ramfs` backend and a tier-portable
  multi-instance `hostfs` backend. It builds freestanding for the guest and is
  shared by the C libc and the Kobold runtime, so both reach files through one
  mount table and namespace model.
- A sandbox fd table in skj-run: each guest gets its own fd table, so host file
  descriptors never leak to the guest. A `--mount name:hostdir` grant opens the
  host directory as a capability and populates the guest's `fs/` mount table.
- A confined capability host backend for the freestanding guest. A `--mount`
  grant is a capability directory fd, and every open is confined beneath it:
  kernel-enforced with `openat2(RESOLVE_BENEATH)` on Linux, a portable
  component-walk (`cap_walk`, which also refuses in-tree symlinks and any `..`)
  on other POSIX hosts, and failing closed on a host with neither.
- macOS and other non-Linux host support for skj-run. The host-varying
  facilities sit behind the `emu/hostplat.{c,h}` shim (confined opens, directory
  enumeration, clock, sleep, random), the only file that branches on the host
  OS. `make macos` builds the emulator with the stock host compiler, no cross
  toolchain or qemu required. The Darwin code paths are written but not yet
  exercised on a real machine.
- A `check-slow` target for the heavy in-guest self-compiles (a full C
  translation unit and kpcom's own source compiled under the interpreter), split
  out of `check-all` so the routine suite stays fast.

#### Changed
- Scratch `ramfs` is now the default landing for a guest with no mount grant;
  the ambient host backend is opt-in (`FS_AMBIENT_DEFAULT`), a temporary
  convenience for the qemu harnesses that have no launcher.
- The freestanding file I/O for the rv32 and ColdFire C libc and the Kobold
  runtime now routes through the shared `fs/` layer instead of raw syscalls.
- skj-run's host-platform code is a pure-POSIX floor with a single Linux
  accelerator: directory enumeration, clock, sleep, and random bytes (through
  `/dev/urandom`) are shared POSIX across all hosts, and the one host branch is
  the `openat2` confinement accelerator over the portable `cap_walk`.
- On an x86-64 Linux host the x86-64 test suite runs the target binaries
  natively instead of under `qemu-x86_64`. Set `QEMU_X64=qemu-x86_64` to force
  the emulator.

#### Fixed
- skj-cc dropped the return type of a call whose callee was not a plain
  identifier (a struct or union member function pointer, an array element, a
  dereference, or a cast), so an `int64_t`, `float`, or struct return through
  such a callee was silently narrowed to a 32-bit `int`. A member function
  pointer returning `long long` came back as its high word alone. The call now
  keeps the callee's declared return type.

### 0.22.0

#### Added
- Kobold `defer`: `defer <statement>` registers block-scoped teardown that runs
  in last-in-first-out order on every orderly exit from the enclosing block,
  including a `return`, a `break` or `continue` that leaves the block, and a
  failable divert. It does not run when a trap aborts the program.
- Kobold `${}` string interpolation: a double-quoted string literal `"..."`
  interpolates `${expr}` holes, converting each expression to text in place; the
  single-quoted literal `'...'` is unchanged. Write `$${` for a literal `${`.
- Kobold `for`-`in` loops: `for x in seq do` walks the elements of a sequence
  directly, binding each to a pre-declared loop variable. The sources are a
  fixed or dynamic array, a string (over its bytes, each bound as a `char`), a
  set (members in ordinal order), a map (`for k, v in m do`, binding the key and
  the value), and any custom cursor type with a `function Next for (c): E fails`
  method, where a failure ends the iteration. Bind `_` to ignore an element.
  Array, string, and set loops lower to the counted loop with no iterator object
  and no per-element call.

#### Removed
- The Excelsior front end (`skj-exc`) and its runtime are removed. Excelsior was
  an in-progress scripting language that had converged with Kobold on its core
  runtime model, and no target host consumed it. Its distinct surface features
  (`defer`, `${}` interpolation, and `for`-`in`) are now part of Kobold, so the
  front end carried no capability Kobold lacks. The removal also drops the
  Excelsior smolmoo host binding and its demo link.

### 0.21.0

#### Changed
- The TinC and TinScheme compilers and the in-process C JIT now use a native
  64-bit pointer model (LP64) on x86-64 and AArch64, retiring the
  32-bit-on-64-bit guest model on those targets. Every front end is now LP64 on
  the 64-bit targets, in line with the C and Kobold compilers; RISC-V RV32,
  ColdFire, and MIPS stay 32-bit (ILP32). The in-process C JIT maps its guest
  across the full address space. Pointer width is unified on a single axis that
  derives from the target instruction set, so a tool can no longer be built with
  a 64-bit code generator yet a 32-bit pointer model.

### 0.20.0

#### Added
- Kobold standard-library units: `core.convert` (failable string-to-number
  `ValInt`/`ValReal` over the `Val` builtin), `io.files` (file metadata and path
  operations), and `io.buffered` (userland buffering).
- The RISC-V in-process JIT compiles GNU `&&label` label values and the computed
  `goto *p`, so threaded-dispatch programs run on the RV JIT as they already did
  on the x86-64 and AArch64 JITs.

#### Changed
- Kobold uses a native 64-bit pointer and handle model (LP64) on x86-64 and
  AArch64; RISC-V RV32 and ColdFire stay 32-bit (ILP32). This retires the
  32-bit-on-64-bit guest model on the 64-bit targets, so the JIT and AOT
  runtimes use the full address space there. `kpcom` bakes the pointer width in
  at build time and emits a module for that width class.
- A KPM module records its pointer-width class on the `.target` line
  (`ilp32`/`lp64`), and the backend rejects a module whose width does not match,
  instead of silently truncating pointers.
- Kobold console I/O is single-sourced over a POSIX file descriptor, and the C
  and Kobold freestanding runtimes share one break-based heap bump, shrinking the
  duplicated runtime surface.

#### Fixed
- A negative 32-bit `or else` fallback into a 64-bit result (`Int64` or real) is
  sign-extended rather than zero-extended; on a 64-bit backend `or else -1` had
  read back as a large positive value.
- A `label` (a threaded-dispatch code address) is sized at pointer width, so a
  stored `@label` keeps its full address when the JIT maps guest code high.
- The C compiler loads a narrow scalar local at its own width.

### 0.19.0

#### Added
- A major Compact Pascal expansion that brings the front end far enough to
  compile and run the Kobold compiler's own source (`kpcom.pas`). The in-tree
  acceptance suite compiles 88 corpus programs through the Pascal front end with
  byte-identical output, and the host test suite passes 209 programs.
- Pascal numeric and layout support: `Int64` with a scalar type-size layer, and
  `Real`, `Double`, and `Single` floating point. A host AOT pipeline and an LP64
  pointer-width layer build native x86-64 executables.
- Pascal heap strings: an `ansistring` representation with reference-counted
  assignment, reference-counted aggregate string members, scope-exit release
  under a leak gate, and in-place self-append.
- Pascal aggregates and routines: scalar and managed-element dynamic arrays,
  open-array parameters, nested dynamic arrays, registered enum types with
  ordinal `High`/`Low` and enum-indexed arrays, `out` parameters, overloading by
  arity and type, and typed constant aggregates with string members.
- Pascal host and build integration: external `cdecl` host imports,
  `ErrOutput`/`Output` stream selection with `Flush`, `{$I file}` include
  splicing, and `ParamStr`/`ParamCount` command-line access.
- A `make help` target that prints the common targets plus the full
  `check-*`/`test-*` catalog, generated from the Makefile so it stays complete.

#### Changed
- The Kobold compiler is documented as self-hosting inside the sandbox: `kpcom`
  runs as an RV32 or ColdFire guest ELF under `skj-run` and reproduces the host
  compiler's output byte for byte, including its own full source.

#### Fixed
- Several Pascal codegen correctness fixes surfaced by compiling `kpcom.pas`:
  call arguments evaluate right to left (FPC order), an indexed-store r-value is
  evaluated before its address (a reallocation hazard), nested-function frame
  corruption and integer `not`, and the string result of `Exit(value)` is
  retained.

### 0.18.0

#### Added
- Kobold can express both classic bytecode-dispatch shapes in the language. A
  procedural value may now be called through a computed designator
  (`table[i](args)`), which is the direct-threaded loop, and a new second-class
  `label` value type with the `@label` address-of operator and a computed
  `goto <expr>` is the indirect (token-threaded) inner loop. A bare
  `goto name` stays a compile error, so general structured goto remains omitted.
- The `label` value is statically second-class: it can only be made from a
  label in the current routine and stored in that routine's own frame, never in
  a global, a field, a return, a parameter, or a collection. A computed jump can
  therefore only land in a live frame, closing the escaping-label-address
  footgun without any runtime check.
- skj-cc gains the GNU C extensions labels as values (`&&label`) and computed
  goto (`goto *expr`), reusing the new dispatch IR opcodes, so a C bytecode
  interpreter can thread its inner loop on every backend.
- Two shared-IR opcodes carry threaded dispatch across all backends:
  `IR_LABADDR` (the address of a local code label) and `IR_IJMP` (an indirect
  jump through a register), with 64-bit twins for the LP64 C targets.

#### Changed
- ColdFire guest faults in the emulator are symbolized through DWARF line info,
  so a trap reports source file and line rather than a bare address.

### 0.17.0

#### Added
- The freestanding Kobold tier gains real number I/O and random units: io.console
  `ReadReal`/`WriteReal` and `ReadInt`/`WriteInt`, and std.random `RandomReal` and
  `SecureRandom`, reached through units with no change to the compiler.
- C guests can draw from the shared random generators. `rand`/`srand` and the BSD
  `arc4random` family (`arc4random`, `arc4random_buf`, `arc4random_uniform`) now
  run over the same xoshiro128** fast generator and ChaCha20 CSPRNG that Kobold's
  std.random uses.
- ColdFire gains the time, RNG, and yield syscall seam (`clock_gettime`,
  `getrandom`, `nanosleep`, `sched_yield`), matching the RV32 freestanding tier.
- A manual for the toolchain itself (the pipeline, the drivers, the assembler and
  linker, the targets and ABIs, building and vendoring), grammar references in the
  C and Compact Pascal language manuals, and a doc/abi note for the shared
  freestanding syscall seam.
- An rv32 guest fault now prints the integer register file at the trap point, and
  a backtrace that walks into unknown code is trimmed with a truncation marker
  instead of printing garbage frames.

#### Changed
- The C and Kobold freestanding runtimes share one bottom layer: a single `_start`
  and argv contract, one libgcc-free number formatter behind both libc `printf`
  and the Kobold numeric I/O, and the syscall-wrapper and `*at` prototypes
  collected in the libc headers instead of per-file externs.
- The x86-64 hand-written crt stacks are marked non-executable, clearing the
  linker's `.note.GNU-stack` warning.
- The Kobold reference manuals now describe the current language and compiler
  rather than the history of how the design was reached; the settled rationale is
  kept in a source-tree decision log (`kobold/DECISIONS.md`).

#### Fixed
- ColdFire file operations: big-endian `statx` and `getdents64` decoding, with
  ColdFire file-op test coverage to match the RV32 tier.

### 0.16.0

#### Added
- In-guest self-hosting: the whole skj toolchain (skj-cc, skj-as, skj-ld, and
  skj-kpc, the Kobold compiler) builds and runs as guest ELFs under skj-run on
  RV32 and ColdFire. The Kobold compiler self-compiles in-guest and its output
  matches the host byte for byte. New opt-in `make` targets prove each stage.
- skj-ld reads objects without mmap under the guest libc, and fchmodat/chmod
  were added to the emulator and the libc so the linker runs in-guest.

#### Changed
- The compiler does about 3.8x less work when self-compiling, with byte-identical
  output. EmittedRoutineName's overload scan is resolved once per routine instead
  of per call site, LowerStr builds its result in place, symbol comparisons use
  the precomputed lowercase name, and a no-allocation SameTextCI serves the
  parser's case-insensitive checks.
- skj-run caches store write permission in a page directory parallel to the main
  one, so a store to a committed writable page skips the per-access region check.

#### Fixed
- skj-cpp collects function-like macro arguments split across source lines.
- skj-as: a use-after-free in the lexer-rewind peek.

### 0.15.0

#### Added
- Kobold standard-library units with dotted namespaces (the core, io, std,
  experimental, and kobold tiers) and a `uses ns.*` wildcard. Runtime services
  (RNG, clock, sleep, console I/O, files) are now reached through units rather
  than compiler builtins. Adds the core.strings unit and a typed read/write
  surface for io.console.
- boris runtime glue (A1-A7): a freestanding heap backed by brk, a 32-bit
  xoshiro128** RNG with seed-driven and pure generators, a clock_gettime and
  getrandom sandbox seam, Sleep/Yield over nanosleep, a sticky EOF latch for the
  freestanding console, and the guest trap ecall aligned with boris VM_SYS_TRAP.

#### Changed
- Strings carry a byte capacity and grow in place: the self-append idiom
  `s := s + e` and left-leaning concat chains lower to amortized in-place appends
  instead of allocating and copying the whole left operand each step.
- A one-character string literal is accepted where a char argument is expected.

#### Tooling
- `check-kp-corpus-san`: the Kobold corpus under the GH_POISON guest-heap
  instrument plus ASan and UBSan.

### 0.14.0

#### Added
- Guest file I/O: openat and real lseek in the sandbox; whole-file and
  line/stream handles through a Kobold `files` unit; stat/fstat via statx across
  rv32, ColdFire, MIPS, x86-64, and arm64; directory reads via getdents64 with a
  `dirs` unit; and mkdir, rmdir, unlink, rename, and remove over the *at syscalls.
- One buffered-stdio core and one syscall seam shared between the C libc and the
  Kobold runtime; open/close/lseek on every C crt; open_memstream; and sbrk
  backed by real brk on ILP32 so a full translation unit self-hosts.
- DWARF for ColdFire and MIPS, with the serializer and reader parameterized by
  endianness; abort and assert are symbolized like a fault.
- skj-cc self-hosts under skj-run on RV32, filling the libc and runtime gaps it
  hit, and `-h`/`--help` is recognized uniformly across every skj-* tool.

#### Changed
- skj-as defaults to 32-bit long-form branches, with `-mshort-branches` to opt
  out, and gains an `.include` directive.

#### Fixed
- A Kobold string release-after-free in kpcom's IsOwnedExpr, found with the new
  GH_POISON instrument.
- cc/backends: a `%ld`/int64 vararg bug and continue inside a switch.

### 0.13.0

#### Added
- A source-level debugger for the toolchain. Source position is carried through
  the IR for `-g` across every front end (C, Pascal, MooScript, TinC, Excelsior,
  Kobold). A DWARF 4 `.debug_line`/`.debug_info`/`.debug_abbrev` serializer;
  skj-as-rv and skj-ld-rv build, carry, and relocate the `.debug_*` sections and
  emit sized STT_FUNC symbols; a shared DWARF reader with skj-addr2line; a
  zero-allocation frame-pointer backtrace that skj-run uses to symbolize faults;
  a GDB remote-protocol stub in skj-run (`--gdb PORT`); and skj-dbg, an
  interactive source-level debugger over that stub.

#### Changed
- Vendored trees copy the usage docs for each selected tool and language.
- CI runners are pinned to ubuntu-24.04.

### 0.12.0

#### Added
- Freestanding RISC-V guest runtime that builds with the skj toolchain alone:
  skj-as-rv gains the tail pseudo and skj-ld-rv resolves it, so no binutils or
  external riscv32 gcc is needed.
- Linker map output (`-Map`) for skj-ld, skj-ld-rv, and skj-ld-mips, listing
  memory regions, output sections with input contributions, and symbols by
  address.
- Kobold dialect features: properties, failable methods with wide results (real,
  int64, record), a set as a dynamic-array element and a map value, real and
  single-precision constants, and string case selectors.
- Near clean-room Kobold specification: static and dynamic semantics, a built-in
  routine signature reference, the host ABI across all four backends, and the
  KPM1 fingerprint and IR opcode reference.

#### Changed
- skj-cc models const qualifiers in its type system, distinguishing
  `int * const` from `const int *`, and rejects a braced scalar initializer with
  the wrong element count.
- Const globals and static-const locals are placed in `.rodata` on all five
  backends. skj-as gains a `.rodata` section for ColdFire, and an uninitialized
  const stays in `.bss`.

#### Fixed
- Integer divide by zero traps cleanly on rv32 and ColdFire, and `INT_MIN / -1`
  (signed division and modulo) traps on every backend, matching the documented
  exit-70 fault semantics.
- The KPM1 reader rejects a slot-less MARK or CONT_UNWIND instead of reading out
  of bounds.

#### Tooling
- A Kobold conformance corpus pins the documented semantics and runtime fault
  exit codes. ColdFire reaches full corpus parity as a backend
  (check-kp-corpus-cf) over the hosted libkpaot-cf.a.

### 0.11.0

#### Added
- Kobold on the ColdFire VM: skj-kp-cf compiles modules ahead of time, and a
  freestanding no-libc runtime (libkpaot-cf-free.a) provides guest file I/O, a
  link script, coroutine support, and the 64-bit and transcendental helpers.
- Extended inline assembly with outputs, inputs, and clobbers on all five
  backends (ColdFire, RISC-V, x86-64, AArch64, MIPS), with integer
  register-class constraints, operand pins, and MIPS named-register pins for
  syscalls.
- Overflow-checked add, subtract, multiply, and negate in the backends, each
  with a per-site trap target. Excelsior traps on integer and decimal overflow.
- Near clean-room Kobold language reference: static and dynamic semantics, a
  built-in routine signature reference, the host ABI across all four backends,
  and the pinned KPM1 interface fingerprint.

#### Changed
- The self-hosting seed is backend-neutral and endian-neutral, so one seed
  serves the big-endian ColdFire target.
- The fails-test error binder is scoped to its branch.
- skj-run provides host file open, read, and close for guest file I/O, and
  asm_lex accepts `$` in symbol names on non-MIPS targets.

#### Fixed
- Integer divide by zero traps cleanly on rv32 and ColdFire, where it previously
  returned a wrong answer or raised a raw SIGFPE.
- The most-negative integer divided by -1 (and the matching modulo) traps
  OVERFLOW on every backend.
- A ColdFire call-argument liveness bug and a 64-bit compare bug in the register
  allocator are fixed.
- The C compiler initializes a local char array from a string literal, and the
  lexer skips GNU `__attribute__`.

#### Tooling
- New `check-kp-corpus-cf` (full Kobold corpus on ColdFire under qemu-m68k) and
  `check-kp-free-cf` (freestanding AOT path on skj-run) targets, plus a
  conformance corpus that pins value semantics and the exit-70 fault code.

### 0.10.4

#### Fixed
- skj-cpp: `__FILE__` expansion overflowed a fixed 64-byte buffer, so a long
  source path was truncated and left an unterminated string literal.
- MIPS: the crts define `__start` so the linker enters at `_start`, the runtime
  builds with the backend's `-mno-abicalls -fno-pic -mfp32` flags, and the
  encoding golden master tolerates trailing section-alignment padding.
- cc: `expect()` parse errors name the file, the tokens, and the context, while
  keeping the pinned "parse error at line" text.

#### Tooling
- CI: the mips and rv32 jobs install the toolchains they need, and the matrix
  runs on `test/**` branches for pre-release dry runs.

### 0.10.3

#### Fixed
- MIPS runtime objects build with `-mfp32`, which the Debian mipsel-linux-gnu
  cross requires.
- The rv32 lockstep and fuzz harnesses compare the full 64-bit f-register,
  including the single-precision NaN-box, now that the core is FLEN=64 like qemu.
- The fuzzer NaN-boxes its single-precision seeds so fuzzed single-precision ops
  run on real values. No core bug was involved.

### 0.10.2

#### Fixed
- tests/test_rv_fp.c NaN-boxes its single-precision operands for the FLEN=64
  f-registers.
- The instruction-count baseline is re-accepted for the intended codegen
  changes, and `exc_walker_test` builds for the gate so an accept no longer drops
  it.

#### Tooling
- The build and mips CI jobs point MIPS_CC (and, for build, MIPS_OBJCOPY) at the
  Debian mipsel-linux-gnu cross. The rv32-reference job installs the riscv cross
  its guest ELF needs.

### 0.10.1

#### Fixed
- tests/test_rv_fp.c NaN-boxes its single-precision operands for the FLEN=64
  f-registers, so the emulator suite passes. The emulator core was correct.

### 0.10.0

#### Added
- A freestanding and hosted C standard library: string, ctype, printf, stdlib, a
  boundary-tag malloc family over a `__libc_sbrk` seam, buffered stdio, errno,
  float conversions, and strtod/sscanf and related functions.
- A stackful coroutine primitive (`co_create`/`co_resume`/`co_yield`) and a
  cooperative C11 `threads.h` subset built on it. The hosted libc, coroutines,
  and threads ship vendorable with the skjcc cross driver.
- skj-cc: by-value struct passing and return on RISC-V, ColdFire, and MIPS o32,
  C99 compound literals, anonymous struct and union members, `_Bool`,
  `_Static_assert`, and `_Generic`.
- skj-cc constant initializers: float and double statics, symbol-plus-addend
  pointers, aggregate leaves, and zero-filled globals in `.bss`.
- RV32 emulator: the D (double) extension, so double-precision guests run under
  skj-run.
- Kobold: `or else` recovers managed results, and `map of T` is structurally
  compatible.
- Documentation: a C language reference, a C standard library design note, a
  Kobold guide, and the Skjegg manual as a GitHub Pages site.

#### Changed
- The in-tree linkers read `ar` archives and pull only the members that resolve
  an undefined symbol. Weak symbols are supported across the assembler and both
  linkers, and the assembler encodes `negx`.

#### Fixed
- Code generation bugs found by a cross-backend differential torture corpus
  (host gcc as oracle against every skj backend, the emulator, and the JIT),
  including unsigned and 64-bit compound assignment, a `long long` switch control
  value, ColdFire signed and unsigned modulo, and the `*p++` load width.

### 0.9.0

#### Added
- C compiler (skj-cc): variadic functions on every target through a real
  `<stdarg.h>`, with per-ABI varargs helpers. The RISC-V varargs convention is
  self-consistent for skj-to-skj calls only and does not interoperate with gcc
  callers.
- Kobold is a first-class in-tree front end (moved to `kobold/`), with `-o`
  output on both the JIT and AOT compilers and a freestanding RV32 guest runtime
  tier that has a versioned entry ABI.
- Cross-language RV32 process sandbox demo, where a C root spawns both a C child
  and a Kobold child under a shared budget.
- Fuzz harnesses for the preprocessor, the KPM1 reader, the assemblers, and the
  TinC, Pascal, MooScript, cc, and Excelsior parsers, plus a code-generator
  coverage harness and a cross-backend differential torture test.

#### Changed
- C compiler: full integer promotions and usual arithmetic conversions,
  nested-brace initializers for local aggregates with incomplete-array
  inference, string literals typed as `char[len+1]` so `sizeof` measures storage,
  and a fix for reversed multidimensional array dimensions.
- Sanitizers (ASan and UBSan) now cover every front end, the archive tool, and
  the Kobold corpora. The undefined behavior they surfaced is fixed.
- The vendored tree ships `<stdarg.h>` and bakes the default include path into
  every cc tool.
- CI runs the MIPS, emulator, RV32-reference, and conformance tiers.

### 0.8.0

#### Added
- Kobold front end: a sixth front end that reads the KPM1 text module emitted by
  its self-hosting compiler, with an in-process JIT (x86-64, plus RV32 and
  AArch64) and an ahead-of-time compiler (RV32 and AArch64). It includes a guest
  runtime for strings, dynamic arrays, maps, coroutines, and refcounted handles.
- Multi-unit ahead-of-time linking with cross-module calls through ordinary
  external linkage.
- A deterministic instruction-count benchmark in `check-emu-all`, so changes in
  generated-code size are caught.

#### Changed
- Integer overflow traps on both the Kobold JIT and AOT paths.
- The KPM1 reader validates its input, including out-of-range operand indices.
- x86-16 is dropped as out of scope, and PLAN.md is corrected.

#### Fixed
- Undefined behavior surfaced by the sanitizers is fixed, and `check-san` builds
  the toolchain under ASan and UBSan in CI.

## September 2026

### 0.7.2

#### Fixed
- CI full test matrix: the arm64 QEMU loader argument was lost because the test
  runners quoted the qemu command. The test runners now word-split `$QEMU`.

### 0.7.1

#### Fixed
- CI for the in-process JIT test tiers: the AArch64 JIT suite runs under
  qemu-aarch64 with the cross toolchain's loader, and CI installs zig to
  cross-build the RV32 JIT. No code changes.

### 0.7.0

#### Added
- In-process JITs for AArch64 (`skj-jit-arm64`) and RV32 (`skj-jit-rv`), joining
  the x86-64 JIT over a shared arch-neutral JIT core. Excelsior runs in-process
  on the AArch64 JIT, including source coroutines.
- RV32 process sandbox (`skj-run`): a round-robin scheduler with spawnable child
  processes, per-process and aggregate memory budgets, guarded dynamic memory
  (`gm_mmap`/`gm_munmap`) so coroutine-stack overruns fault, and a
  decoded-instruction cache that `fence.i` invalidates.

#### Changed
- Each backend is a single instruction selector driving a byte sink (for the JIT)
  and a text sink (for AOT output). RISC-V uses `rv_select.c` in place of
  `rv_emit.c`.
- `rv_run` returns an enum reason (budget, halt, trap, or yield) and stops early,
  which is an API change to the emu-rv component.
- Redundant license notices are removed across the tree.

#### Fixed
- Undefined behavior in the RV32 S/B/J immediate decoders, found by a new ASan
  and UBSan pass, plus hardening of `gm_mmap`/`gm_munmap` against 32-bit wrap and
  two sandbox bugs (a spawn leaving the parent's fault flag set, and a scheduler
  slice not clearing the wfi wait).

#### Tooling
- `make check-emu-san` runs the host-only emulator tests under ASan and UBSan in
  CI, with new tests for the `rv_run` contract, guest mappings, the decode cache,
  and the sandbox.
- The byte encoders are checked against GNU as (RV32) and a disassembler
  (AArch64).

### 0.6.0

#### Added
- `skj-jit`, an in-process x86-64 JIT for the C compiler. It compiles C source to
  native code in the same process and runs it, with no assembler, linker, or
  emulator. It covers the integer core, single and double float, small-struct
  by-value arguments and returns, System V varargs, and calls that spill to the
  stack.
- A new `check-jit` test tier.
- New IR opcodes: `FSQRT`, `FROUND`, 64-bit divide and modulo, overflow-checked
  `ADDO`/`SUBO`/`MULO`, and `IR_LOC` for source-location tracking.

#### Changed
- The x86-64 code generator is one instruction selector (`backend/x86_select.c`)
  with a byte sink for the JIT and a NASM text sink for ahead-of-time output.
  `skj-cc-x86-64`, `skj-exc-x86-64`, and the JIT share it.
- The x86-32 target is removed: its tools, tests, runtime, vendoring, and
  `X86_BITS==32` branches.
- RISC-V and MIPS now match the platform psABI for every external call, and the
  toolkit's stack convention is retired there. `skj-cc-rv-psabi` merges into
  `skj-cc-rv`. ColdFire keeps its convention.
- MIPS fixes: single-precision float arguments take the same eight-byte footprint
  as doubles, and a tail call whose arguments fit in registers is a real tail
  call.
- Preprocessor constraint violations now fail `skj-cpp` instead of passing
  silently.

### 0.5.1

#### Fixed
- Calls with 17 to 32 arguments compile on every backend. The per-call cap rose
  from 16 to 32 to match the C front end. Calls of 33 are still rejected by the
  front end.
- RISC-V handles stack frames larger than 2047 bytes. Out-of-range frame offsets
  go through a scratch register.
- `skj-as-rv` rejects out-of-range 12-bit immediates with a diagnostic that names
  the mnemonic and value, instead of silently truncating them.

#### Tooling
- New cross-target regression test `cc_t078`, covering a 3200-byte frame and a
  20-argument call on every C backend.

### 0.5.0

#### Added
- `skj-as-mips` and `skj-ld-mips`, a binutils-free MIPS I assembler and static
  linker for ELF32 little-endian objects. The linker applies the o32 relocations,
  emits the MIPS ABI flags, and pulls only the archive members it needs.
- `skj-ar`, a target-neutral archive tool that creates GNU/SysV archives with a
  symbol index and lists or extracts members.
- A PlayStation target. `skj-ld-mips -f ps-exe` writes a PS-X EXE image, and
  `start_psx.S` is a BIOS-TTY runtime crt. A PS-EXE test validates the container,
  and an opt-in test runs programs on PCSX-Redux, including a soft-float program
  on the FPU-less R3051. This completes MIPS milestone 6.

#### Tooling
- `check-mipsas` checks encodings against GNU as and includes a delay-slot load
  test. `check-cc-mips-skj` runs the full C suite through the in-tree toolchain
  under qemu-mipsel.
- The MIPS toolchain vendors as `as-mips`, `ld-mips`, and `ar`, plus the `mips`
  backend component and the soft-float compiler `skj-cc-mips-sf`.
- `drop-to-main` takes an explicit version and names the main commit `vX.Y.Z`.

### 0.4.0

#### Added
- A MIPS I backend for the R2000 and R3000, little-endian. It has two calling
  tiers: the toolkit's stack convention for TinC, TinScheme, and the C compiler,
  and the o32 platform ABI for Excelsior. Floating point is a build choice, and a
  `.set noreorder` mode fills delay slots with a scheduler.
- `skj-run`, an in-tree guest runner for ColdFire and RV32 that replaces
  qemu-m68k and qemu-riscv32 for those targets. It reports the guest's exit
  status, and `--trace` and `--stats` expose emulator events. The RV32 core ships
  with its audit suites.
- `skj-as-rv` and `skj-ld-rv`, an in-tree RISC-V assembler and static linker for
  ELF32 that interoperate with gcc-built code.
- Excelsior sources: the fallible pump as the iteration protocol, `source of T`
  with `yield` on a stackful coroutine, `defer`, list comprehensions, and
  function values.
- Inline assembly (`asm` and `asm volatile`) in `skj-cc`, plus a MIPS I retarget.
- Runtime additions: software float (`softfloat.c`) and 64-bit integer helpers
  (`soft64.c`) for targets without hardware support, a smolmoo runtime, and psABI
  start files for MIPS and RISC-V.

#### Changed
- The Excelsior host runtime is renamed from `exc_host` to `libexc`, with a
  public `libexc.h`.
- The ColdFire assembler's lexer and object emission are split into `asm_lex` and
  `asm_obj`.

#### Tooling
- `make check-all` passes across ColdFire, RISC-V, x86-32, x86-64, AArch64, MIPS,
  and the emulator suites, including the new RISC-V assembler and linker suites.
  Documented in `doc/mips.md` and `doc/emulator.md`.

## July 2026

### 0.3.0

#### Added
- AArch64 (ARM64) backend, and an x86-64 target sharing the x86 backend. Both are
  at feature parity with x86-32: integer core, native 64-bit integers, float,
  `_Float16`, and delimited continuations.
- Native single-precision float (IR_F32) on every backend, and RISC-V hardware
  float (RV32FD + Zfh) in place of the soft-float path.
- skj-cc retargeted to x86-64, AArch64, and RISC-V alongside ColdFire. It has an
  LP64 data model on the 64-bit targets, SysV AMD64 and AAPCS64 calling
  conventions (including varargs and struct-by-value), bit-fields, designated
  initializers, `_Alignof` and `_Alignas`, and a `_Float16` storage type.
- Excelsior (skj-exc), a new statically typed, class-based in-world scripting
  front end. It covers per-instance object state, records, enums, `set of E`,
  base-10 decimal, UTF-8 strings, lists, fallible expressions with `otherwise`
  and `on fail`, teaching runtime faults, interfaces and object slices, `shared`
  parameters, and compile-time macros with record, class, and enum introspection.
- `make release-check`, which verifies that `version.mk` and the release tag
  agree.

#### Changed
- `version.mk` is the source of truth for the version, and git only refines it. A
  vendored build reports the Skjegg version plus the commit it was vendored from,
  not the consuming project's tags.
- `long double` is a distinct, detected type instead of causing silent ABI
  corruption.

#### Fixed
- A float temp live across a call is spilled on x86 and AArch64.
- RISC-V double slots are 8-byte aligned, so `fld` and `fsd` are never
  misaligned.
- Struct and union globals are sized and aligned correctly, and pointer globals
  initialized to addresses are relocated.
- Vendored trees now get the `version.h` rule, so vendored tools compile.

## May 2026

### 0.2.0

#### Added
- The initial public toolkit: a shared 3AC IR with ColdFire/m68k and RISC-V
  backends, and a built-in ColdFire assembler, linker, and C preprocessor.
- Front ends for TinC, TinScheme, MooScript, Compact Pascal, and an early C
  compiler (skj-cc), plus `vendor.sh` for embedding components in other projects.
