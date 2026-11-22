# WASM delimited continuations (shift/reset)

The TinScheme front end has delimited continuations: `reset`, `shift`, and
`resume`. On the five register backends they are implemented by copying a
segment of the native call stack into an arena and jumping, a setjmp/longjmp
shape (see the runtime's `__cont_capture` / `__cont_resume`). WASM cannot do
this: the call stack is not in linear memory and cannot be addressed or copied,
which is the whole reason this is a separate note rather than one more opcode in
the backend.

This note records how the WASM backend provides continuations instead, the
decision that led there, the runtime cost, and the phased plan.

## The IR it has to lower

`scheme/lower.c` emits four continuation opcodes, all setjmp/longjmp shaped:

- `IR_MARK` (at `reset`): a setjmp point. Saves a re-entry mark and returns 0 on
  first entry, nonzero (a captured buffer) when a `shift` unwinds back to it.
- `IR_CAPTURE` (at `shift`): copies the stack between the shift and the enclosing
  mark into a buffer, then longjmps to the mark carrying that buffer.
- `IR_RESUME` (at `resume`): copies a captured buffer back onto the stack and
  returns into it, with the resume value as the `shift` expression's result.
- `IR_CONT_UNWIND` (at the close of a `reset` extent): reclaims the arena to its
  mark-time high-water mark, giving the arena stack discipline across a loop
  that resets repeatedly.

A `reset` body can call through any number of ordinary (non-tail) function
frames before a `shift` fires. `(reset (deep 60) ...)` suspends sixty frames
deep. So the mechanism has to unwind and restore an arbitrary span of frames,
not just the reset function's own frame.

## Why stack switching, not Asyncify

Two mechanisms can give a WASM module resumable stacks:

- **Asyncify** (a Binaryen transform, reproducible in the backend) instruments
  every function to save its locals and resume point to a side stack in linear
  memory and return, then replay the call chain to rewind. It runs on any engine
  with no feature flags and supports multi-shot resumption, at the cost of a
  large whole-module transform and per-call overhead on every function.
- **Stack switching** (the WasmFX typed-continuations proposal: `cont.new`,
  `resume`, `suspend`, and control tags) gives first-class one-shot
  continuations as an engine primitive. It is delimited continuations in the
  instruction set, so it maps almost one to one onto `reset`/`shift`/`resume`,
  and needs no whole-module transform.

The design doc for the backend earlier recorded stack switching as "unshipped in
stable runtimes." That is no longer true for the pinned engine: wasmtime 42.0.1
runs the proposal behind `-W stack-switching=y`. All three TinScheme
continuation tests resume each captured continuation exactly once, so one-shot is
sufficient. Weighed against a large Asyncify pass, stack switching is the far
smaller and more faithful lowering, so the WASM continuation lane uses it.

The tradeoffs it accepts, recorded so they are not a surprise later:

- The module is not MVP. It requires the stack-switching proposal, so it runs
  under wasmtime with `-W stack-switching=y` (the test lane and CI pass the
  flag), and not yet under engines that have not shipped the proposal.
- One-shot only. Resuming a captured continuation twice is not supported; the
  continuation is consumed by its `resume`. The tests do not need multi-shot. If
  a future program does, it is a documented follow-up (clone via the proposal's
  facilities, or fall back to Asyncify).

## The mapping

Scheme values are the uniform ILP32 tagged word (`i32`). A WasmFX continuation
reference is an opaque `(ref $ct)` that cannot be bit-cast to `i32` or stored in
linear memory, so it cannot be a scheme value directly. The bridge is a
**continuation table**: captured continuation references live in a WASM table of
`(ref null $ct)`, and the `i32` index into that table is what scheme carries as
the continuation value. The table is bump-allocated per reset extent and its top
is reclaimed at `IR_CONT_UNWIND`, the same stack discipline the arena has
natively.

Module-level declarations, emitted only when a program uses continuations:

    (type $ft2 (func (param i32 i32) (result i32)))  ;; the fresh continuation's type
    (type $ft1 (func (param i32) (result i32)))      ;; the remainder's type
    (type $ct2 (cont $ft2))                           ;; from cont.new of the thunk
    (type $ct1 (cont $ft1))                           ;; after a suspend, and what resume uses
    (tag  $shift (param) (result i32))                ;; the shift control tag
    (table $conts N (ref null $ct1))                  ;; captured remainder continuations

Two continuation types are needed because the reset body thunk is a lambda
lifted by the front end, so it carries the hidden closure self pointer: its type
is `(i32 self, i32 arg) -> i32`, and `cont.new` builds a continuation of that
two-parameter type (`$ct2`). The first `resume` supplies both (a zero self and a
zero dummy arg, neither of which a reset body uses). Once the body suspends via
the `$shift` tag (`() -> i32`), the remainder continuation takes just the resumed
`i32` and yields the extent result, a one-parameter type (`$ct1`). That is the
type the handler receives, the `$conts` table holds, and `resume` consumes.

The `$shift` tag takes no parameters and produces an `i32`: `suspend $shift`
passes nothing to the handler, and when the handler resumes the continuation the
`i32` it supplies becomes the result of the `suspend`.

Lowering, per opcode:

- **`shift`** -> `suspend $shift`. The `i32` it yields is the value the
  continuation is later resumed with.
- **`reset(body, handler)`** -> lambda-lift `body` into a thunk `(func (param
  i32) (result i32))`, then

        (block $on_shift (result (ref $ct))
          (resume $ct (on $shift $on_shift)
                  (i32.const 0)                 ;; dummy initial value
                  (cont.new $ct (ref.func $thunk)))
          return)                               ;; body returned: its value is the result
        ;; control arrives here only on suspend, with the remainder cont on the stack
        local.set $k
        <store $k into the conts table at index idx, tag idx as a scheme value>
        <call handler(idx)>

- **`resume(k, val)`** -> load `(ref $ct)` from the conts table at index `k`,
  then `resume $ct (on $shift $on_shift') val k` under a fresh handler block, so
  a re-`shift` inside the resumed continuation is caught again. When the
  continuation runs to the end of its reset extent, its final value is the result
  of this `resume`.
- **`IR_CONT_UNWIND`** -> reset the conts-table bump top to the extent's
  mark-time value.

Because `suspend` tunnels through every frame between the `cont.new` and the
`shift` automatically, only the reset body is lifted into a thunk. The
intervening functions (`produce`, `deep`) are not outlined; their frames live on
the continuation's own stack and are suspended and resumed by the engine.

## Binary encodings

The proposal's opcodes and type forms, as the pinned wasm-tools and wasmtime
encode them (verified by round-tripping a module through `wasm-tools`):

    cont type form      0x5D <func-typeidx>            ;; in the type section
    tag                 0x00 <func-typeidx>            ;; tag section, id 13
    table element type  0x63 <heaptype>                ;; (ref null ht); 0x64 is (ref ht)
    block result reftype blocktype valtype 0x64 <typeidx>  ;; (ref $ct) as a blocktype
    ref.func            0xD2 <funcidx>
    cont.new            0xE0 <cont-typeidx>
    suspend             0xE2 <tagidx>
    resume              0xE3 <cont-typeidx> <nhandlers> <handler...>
    resume on-label     0x00 <tagidx> <labelidx>       ;; one handler entry

## Running: no proc_exit after a stack switch

The pinned wasmtime (42.0.1) runs the stack-switching proposal on an async
fiber. Once a program has performed a stack switch, raising any trap on that
execution panics the engine (an assertion in its trap handler, `core::ptr::eq`),
and `proc_exit` is raised as a trap. A benign host call such as `fd_write`, and a
normal return, are fine; only a trap is not. So a scheme program that used a
continuation cannot set its exit code through the WASI `proc_exit` seam the way
the other front ends do.

The test lane sidesteps the WASI exit path entirely: it invokes the exported
`main` and reads its returned value, the untagged top-level result, which is the
same number the exit code would have carried. This is uniform across the scheme
corpus (a program that never switches stacks returns its value just the same),
needs no change to the synthesized runtime, and avoids `proc_exit` on the one
path where it breaks. It is the reason `check-sc-wasm` runs `wasmtime run
--invoke main` rather than reading the process exit code.

If a future wasmtime fixes the trap-after-switch assertion, the lane can move
back to the ordinary exit-code path.

## Limitations

- One-shot. A captured continuation is resumed at most once; resuming twice is
  not supported.
- The handler resumes in tail position. WasmFX `resume` returns the extent's
  completion value to the resumer, so code in the handler after a `resume` runs,
  unlike the native jump model where it would not. Every test resumes in tail
  position, where the two models coincide.
- A re-`shift` inside a resumed continuation traps (the resume installs no
  handler). The tests do not re-shift a resumed continuation.
- A reset body that captures a local is rejected at lowering time (the thunk
  would need a closure, not a bare funcref for `cont.new`).

## Phase plan

1. Done. The WasmFX opcodes and the cont-type, tag, conts-table, conts-top and
   elem-declare emission, emitted only when a program uses continuations.
2. Done. `scm_shift` (one reset, one shift, one resume, no loop) end to end: the
   scheme WASM lowering lifts the reset body thunk and emits the reshaped ops,
   and the lane runs it under wasmtime with the proposal flags.
3. Done. The looping and reclamation tests `scm_reset_heap` and
   `scm_reset_reclaim`. `IR_MARK` saves `$conts_top` into a per-function scratch
   local, and `IR_CONT_UNWIND` restores it, so each reset extent reclaims the
   table slots its captures used. Without this the bump pointer only advances
   and a loop runs it off the end of the table; with it a program that resets
   repeatedly stays bounded (verified by shrinking `$conts` to a handful of
   slots and confirming both loops of hundreds of turns still run).
