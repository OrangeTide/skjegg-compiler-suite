# Closures and continuations

This is the authoritative note for the toolkit's closure and continuation
support. It records the design decisions, their rationale, and their status.
Where a claim is a contract other code must honor, it is marked **decided**;
what is built is **implemented**; what is intentionally not built is
**deferred**. The IR opcode table in `doc/ir.md` is the reference for the
three continuation opcodes; this note explains the whole mechanism and pins
the semantics.

## The four features and how they relate

The word "closure" gets used loosely, so this note keeps four distinct things
apart:

1. **TinScheme flat closures** (implemented). A `lambda` captures its free
   variables by value into a heap record and can be stored, passed, and called
   later. This is closure support proper.
2. **Delimited continuations** (implemented), exposed by TinScheme
   (`reset` / `shift` / `resume`) and by TinC (the `mark` / `capture` /
   `resume` intrinsics). A capture takes the current continuation up to the
   enclosing delimiter as a resumable value. Both front ends drive the same IR
   opcodes and the same runtime.
3. **Excelsior func values** (implemented, non-capturing by decision). A
   `func(...) ... endfunc` is a first-class function value, but capture is
   **forbidden**, so it is a bare code pointer, not a closure. See
   `excelsior/function-values.md`.
4. **Excelsior sources** (implemented). A `source of T` is a stackful
   coroutine on a private stack, a separate mechanism from the continuation
   runtime, kept separate by decision. See `excelsior/sources.md`.

Features 3 and 4 are covered here for contrast, because they are routinely
mistaken for closures and continuations, and the reasons they are not are
themselves design decisions.

## TinScheme flat closures

TinScheme lowers a `lambda` to a heap-allocated closure record plus a lifted
top-level function. Everything is in `scheme/lower.c`.

### Closure record (implemented)

A closure is bump-allocated from the guest heap `__heap` with this layout
(`emit_closure`):

```
offset 0:  type byte (OBJ_CLOSURE = 3)
offset 4:  function pointer (address of the lifted __lambda_N)
offset 8:  free[0]
offset 12: free[1]
...
```

The record is `8 + 4 * nfree` bytes.

**Decided: capture is by value, at closure-creation time.** `emit_closure`
stores the current value of each free variable into the record. There are no
shared mutable cells (no boxing), which keeps a closure a flat, self-contained
record, and building one does not extend the lifetime of any stack frame. This
is a clean fit for TinScheme, which has no variable-rebinding `set!` (only
`set-car!`/`set-cdr!`, which mutate the contents of a pair, not a binding). A
captured value that happens to be a pair pointer still refers to the one shared
heap pair, so a `set-car!` through it is seen by everyone holding that pair; it
is only the binding that is snapshotted. If a future `set!` on a captured
variable were wanted, it would need boxing, the deferred item below.

A named global function used as a value is wrapped as an env-less closure
(`nfree = 0`), so a plain function name and a `lambda` are the same kind of
runtime value.

### Free-variable analysis (implemented)

`find_free` walks the lambda body and collects each symbol that is not one of
the lambda's own parameters, not a global (a global is reached directly, not
captured), and is a local in the enclosing scope. It stops at a nested
`lambda`, which does its own analysis. The cap is 16 free variables per lambda.

### Lambda lifting and the env parameter (implemented)

`lower_lambda` lifts the body to a fresh top-level `__lambda_N` and appends it
to a worklist that `lower_program`'s pass 4 drains after the ordinary
functions. Every lowered function, lifted or not, has a hidden first parameter
`__env` in slot 0 (`lower_function`). A function with free variables reads them
back in its prologue: it loads the env pointer from slot 0, then for each free
variable loads `env + 8 + 4*i` into that variable's local slot. From there a
captured variable is treated exactly like a local.

### Calling convention (implemented)

**Decided: the closure record is the hidden argument 0 (the env), real
arguments follow at 1..n.** A closure is invoked by an indirect call
(`lower_call`):

1. `closure_fn(t)` loads the function pointer from `env + 4`.
2. `IR_ARG` 0 is the closure itself.
3. `IR_ARG` 1..n are the real arguments.
4. `IR_CALLI` through the function pointer.

A direct call to a named function passes `0` as the env (nothing to read). Tail
positions use `IR_TAILCALLI` / `IR_TAILCALL` when the total argument count fits
`cur_fn->nparams`.

## Delimited continuations

Two front ends expose the same three IR opcodes.

**TinScheme** (`scheme/lower.c`) wraps them in the classic delimited forms:

- `(reset body handler)` marks a delimiter.
- `(shift)` captures the continuation from the `shift` up to the enclosing
  `reset` and returns it as a buffer value.
- `(resume buf value)` re-enters a captured continuation, delivering `value`.

**TinC** (`tinc/lower.c`, documented in `tinc/DESIGN.md`) exposes the raw
intrinsics as ordinary-looking calls:

- `mark()` places a delimiter, returning 0 on first entry and nonzero (the
  buffer) on re-entry.
- `capture()` captures the current continuation, returning the buffer.
- `resume(buf, value)` re-enters a captured continuation.

The two surfaces are independent (neither front end calls the other's code),
but they lower to the same three IR opcodes and the same runtime. TinScheme's
`reset` bundles the mark with the handler dispatch, where TinC's `mark` exposes
that raw and leaves the branch to the programmer.

### IR opcodes (implemented)

| Opcode    | Operands   | Meaning |
|-----------|------------|---------|
| `MARK`    | d, slot, L | Save the current frame to a 12-byte slot (fp, sp, resume PC). `d` is 0 on first entry, nonzero (the captured buffer) when re-entered via a capture. |
| `CAPTURE` | d          | Capture the current continuation; `d` is the buffer pointer. |
| `RESUME`  | a, b       | Resume continuation `a` with value `b`; does not return. |

`lower_reset` emits `IR_MARK` and branches on its result: 0 is first entry (run
the body), nonzero means a `shift` longjmped back to the mark, so it calls
`handler(buffer)`. `lower_shift` is a single `IR_CAPTURE`; `lower_resume` a
single `IR_RESUME`.

### Runtime (implemented, at parity across all backends)

The runtime provides `__cont_capture`, `__cont_resume`, and the globals
`__cont_mark_sp` and `__cont_arena`. They exist in every crt: `runtime/start.S`
(ColdFire), `start_x86.asm` (x86-32), `start_x86_64.asm`, `start_arm64.S`,
`start_rv.S` (RISC-V), `start_mips.S`, and `start_psx.S` (the PlayStation MIPS
variant). All five backend emitters lower the three opcodes
(`backend/{cf,x86,arm64,rv,mips}_emit.c`). The `start_x86_64_sysv.asm` host
variant does **not** carry these routines; only the toolkit's own crt does.

The mechanism is stack-segment copying:

- `IR_MARK` writes the delimiter's `{ saved_fp, saved_sp, re-entry PC }` into a
  12-byte slot and publishes the slot address through the global
  `__cont_mark_sp`.
- `__cont_capture` runs at the capture site. It reads the mark slot, computes
  the live segment size as `mark_sp - capture_sp` (the stack grows down),
  bump-allocates a buffer in `__cont_arena` with a 12-byte header
  `{ capture_sp, capture_fp, size }`, copies the segment into the buffer, then
  restores fp/sp to the delimiter and jumps to the mark re-entry PC with the
  buffer pointer in the return-value register. The callee-saved registers are
  part of the copied segment (the `IR_CAPTURE` inline code pushes them first),
  so they ride along.
- `__cont_resume(buf, value)` copies the saved segment back to its original
  stack addresses, restores fp/sp, puts `value` in the return-value register,
  and returns into the restored frame.

**Decided: metadata is 32-bit on every target.** The mark slot's
`{ fp, sp, PC }` and the buffer header are 32-bit words even on the 64-bit
backends, because those run ILP32-style with every live address below 4 GB (see
`doc/lp64.md` and `doc/calling-convention.md`). A 32-bit word therefore holds a
pointer, and the segment copy runs in dwords (`rep movsd` on x86).

### Resume semantics: one-shot per buffer (decided)

**A captured continuation is resumed at most once.** `__cont_resume` writes the
saved segment back to the exact stack addresses it was captured from, so a
resume re-enters the stack region the capture came from. Resuming the same
buffer a second time, or resuming after the surrounding computation has reused
that stack region, is not supported and corrupts the stack. This is a
deliberate scope choice, not an accident of the current code: the copy-back
model is chosen because it is small, allocation-light, and identical across six
crts, and one-shot delimited continuations are enough for the intended uses
(generators, early exit, cooperative turn scripting; see
`doc/game-scripting.md`). Multi-shot resume would require re-copying to a fresh
region on each resume and is not planned.

The buffer is a plain `__cont_arena` allocation, so within that arena's
lifetime a captured continuation is a first-class value: it can be stored and
resumed after its `reset` has returned, subject to the one-shot rule above.

## Garbage collection and the two heaps

TinScheme has a mark-sweep collector, and it is easy to assume it manages
compiled closures. It does not. There are two separate heaps, and only the
first is collected.

**The host-side value heap (collected).** `scheme/gc.c` is a mark-sweep GC over
the `val_t` S-expression graph the **compiler itself** reads and manipulates at
compile time (`scm_read_all` in `scheme/main.c`). Its object kinds include
`OBJ_CLOSURE` (`struct gc_closure { func_id, nfree, free[] }`), and `mark_val`
traces a closure by marking each captured free value, so a closure keeps its
captured values alive. This collector is exercised host-only by
`scheme/test_gc.c` (`make test-gc`, no cross toolchain, no emulator). It runs
inside `skj-scheme` while it compiles; it is **not** emitted into the target
program. `gc_closure` is called only from `gc.c` and `test_gc.c`, never from
`lower.c`.

**The compiled-guest heap (not collected).** The closures the generated program
allocates come from `__heap`, a fixed BSS bump region (8192 bytes, emitted by
`lower_program` alongside `__heap_ptr`). Cons pairs share it (they are the only
other guest heap object; the guest has no runtime string values). It is a bump
allocator with no free and no collection: a guest closure lives for the whole
run, exactly like a cons pair. This is consistent with the rest of the runtime,
which is arena- and bump-based by design. The limitation is real, see below.

So `OBJ_CLOSURE` tracing in `gc.c` describes how the compiler keeps its own
reader data alive, and says nothing about the runtime lifetime of a compiled
closure, which is "forever, until the program exits."

## Excelsior func values are not closures (decided)

Excelsior's `func(...) returns T ... endfunc` is a first-class function value
but deliberately **not** a closure. `lower_funclit_fn` (`excelsior/lower.c`)
lifts the literal to its own top-level function; the value is an `IR_LEA` of
that label, a bare code pointer with no environment record, and calling one is
a plain `IR_CALLI`.

Capture is rejected at resolve time by `funclit_capture_check`
(`excelsior/resolve.c`): the body may name its own parameters and module-level
constants, but naming an enclosing local, parameter, `self`, or a field is a
teaching error ("a captured closure is deferred, function-values.md D4"). A
lambda may call a sibling `func` (the defining class is stashed on the literal
so the mangled call resolves) but not a sibling verb (a self-send needs
`self`), and a func-typed field is rejected (that would be a stored closure).

The rationale (`excelsior/function-values.md` D4): Excelsior wants first-class
functions and higher-order builtins (`map`, `filter`, `sort`, `reduce`) without
committing to a closure runtime and its lifetime questions in a language with
a refcounted acyclic heap and per-turn arenas (`excelsior/memory.md`). Escaping
closures are deferred, not refused forever.

## Excelsior sources are coroutines, not continuations (decided)

A func that `returns source of T` lowers to a stackful coroutine, a different
mechanism from `__cont_capture`. `__exc_src_new` allocates a source struct plus
a private stack (4 KB from the arena); `__exc_src_next` launches or resumes the
body on that private stack; `yield` boxes a value through the null-word `maybe`
protocol and switches back (`__exc_src_yield`). The helpers sit in each crt
beside the continuation routines (for example the "Excelsior sources" section
of `runtime/start.S`).

**Decided: a source does not reuse the continuation runtime.** The reason is
lifetime. A continuation buffer is copied stack written back to the addresses
it was captured from, so it is tied to the first pump's stack region and would
corrupt the stack if a source were pumped from a deeper frame. A source body
runs on its own private stack, so its frames live at fixed addresses and it can
be pumped safely from any caller depth. `excelsior/sources.md` records that the
original plan to reuse `__cont_capture`/`__cont_resume` was amended away (D1)
for exactly this reason. Sources are also second-class and turn-local (D5),
enforced in `excelsior/typecheck.c`: a source value never crosses a store or
send boundary.

## Limitations and future direction

- **No shared mutable capture of a binding (TinScheme).** Closures capture by
  value. TinScheme has no variable-rebinding `set!` today, so this is not a
  current gap; but if `set!` were added, a `lambda` closing over a `set!`-ed
  variable would need that binding boxed into a shared heap cell. Deferred.
- **Compiled-guest heap is a fixed 8 KB bump region, never collected.**
  Closures and cons pairs both draw from `__heap` and are never freed; a
  long-running or allocation-heavy guest overruns it, and the overrun is
  unchecked (`__heap_ptr` has no limit compared against it). A collecting or
  larger/configurable guest heap is future work; the in-tree GC (`gc.c`) is the
  host-side collector, not this.
- **Continuations are one-shot per buffer** (see the contract above).
  Multi-shot resume is not planned.
- **Excelsior capturing/escaping closures are deferred** by decision
  (`function-values.md` D4); today func values are non-capturing code pointers.
- **Float through Excelsior func values is not lowered.** A float argument or a
  float result passed through a func value is not yet handled in
  `excelsior/lower.c`; only word-sized arguments and results go through the
  indirect call.
- **Excelsior sources are second-class and turn-local** (`sources.md` D5); a
  first-class or persisted source is out of scope.

## File map

| Concern | Files |
|---------|-------|
| Flat closures (analysis, record, lifting, calling) | `scheme/lower.c` (`find_free`, `emit_closure`, `closure_fn`, `lower_lambda`, `lower_function`, `lower_call`) |
| Host-side closure GC (compile-time only) | `scheme/gc.c` (`gc_closure`, `mark_val` `OBJ_CLOSURE` arm), `scheme/gc.h`, `scheme/test_gc.c` (`make test-gc`) |
| Continuation surface (TinScheme) | `scheme/lower.c` (`lower_reset`, `lower_shift`, `lower_resume`) |
| Continuation surface (TinC) | `tinc/lower.c` (the `mark` / `capture` / `resume` intrinsics), `tinc/DESIGN.md` |
| Continuation IR opcodes | `ir/ir.h`, documented in `doc/ir.md` |
| Continuation opcode emission | `backend/{cf,x86,arm64,rv,mips}_emit.c` |
| Continuation runtime | `runtime/start.S`, `start_x86.asm`, `start_x86_64.asm`, `start_arm64.S`, `start_rv.S`, `start_mips.S`, `start_psx.S` (`__cont_capture`, `__cont_resume`, `__cont_mark_sp`, `__cont_arena`) |
| Excelsior func values (non-capturing) | `excelsior/lower.c` (`lower_funclit_fn`), `excelsior/resolve.c` (`funclit_capture_check`), `excelsior/function-values.md` |
| Excelsior sources (coroutines) | `excelsior/lower.c`, the crt "Excelsior sources" sections, `runtime/libexc.c`, `excelsior/sources.md` |
