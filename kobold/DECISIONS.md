# Kobold-Pascal design decisions

This is the decision log for Kobold-Pascal: the settled choices behind the
language, the runtime, and the module format, with the reasoning that led to
each. It is an internal design record kept next to the sources, not part of the
published manual under `doc/languages/kobold/`. The reference manuals describe
the language as it is; this file records why it is that way.

The rationale below was moved here from the reference manuals so those read as
current-state reference. The manuals remain the authority on current behavior;
where this log and a manual ever disagree about what the compiler does, the
manual (and the compiler) win.

## Error model

**Two axes, kept apart.** Expected failures are values; unexpected failures are
traps. An ordinary failable result (a bad lookup, a parse miss) is returned as a
value and tested with a branch. An unexpected condition (divide by zero, a bad
index, an explicit `panic`) raises a trap that unwinds to the host's landing
pad. This mirrors the split in other value-error languages: Zig's `error` versus
`@panic`, Swift's `throws` versus `fatalError`, Rust's `Result` versus `panic!`.
The split is load-bearing for coroutines: the trap mechanism relies on a single
VM-wide landing pad and the invariant that no landing pad spans a `yield`. A
value-returned error installs no pad and does no `longjmp`, so it composes with
coroutines for free. A guest-side `try`/`except` that caught a `panic` would put
a landing pad on the guest stack, break that invariant, and force per-coroutine
pad chains, so it is a non-goal.

**An error is a second return value.** A failable routine hands back its normal
return value in the usual return register and an error signal in a second
register beside it. The caller tests the second register and branches. There is
no hidden control flow: every call-site form is thin sugar over make-the-call,
test-the-error-register, branch, and a reader can look at the generated IR and
see an ordinary `if`. Exceptions hide the test and the branch inside a runtime;
Kobold does not.

**`error` is opaque.** It is accessed only through operations, never compared as
a raw integer. Keeping it opaque is what lets its representation grow without
breaking source, under the invariant that success is always "the error register
is zero" (`reg <> 0` regardless of what the register holds).

- First (current) representation: the error register holds an integer code. Zero
  is success; any other value is a code the program chose. No allocation, no
  reference counting, no heap object. The whole mechanism is a register and a
  branch.
- Later, when a human-readable message earns its place: the register holds a
  managed handle (reference counted like a `string`) carrying a code and a
  message, `nil` (still zero) for success, with `fail(msg)` / `fail(code, msg)`
  and `errormessage(e)` joining `fail(code)` / `errorcode(e)`. An allocation
  happens only on an actual failure.

Because `error` is opaque and the success test never changes, moving from the
int to the handle is a recompile, not a source edit: it bumps the interface
fingerprint (the register goes from an int to a pointer), which the unit system
already handles, and existing call sites, `or else`, and `fails` forms are
byte-identical across the change.

**Go's error shape, not Rust's `Result`.** One canonical error value with nil
for success, reached by a cheap first step, rather than `Result<T, E>`, which
would need the sum types and generics the language has ruled out.

**Must-consume.** Calling a failable routine and doing nothing with the error is
a compile error; the result cannot reach the rest of the program until the error
is dealt with. This is stricter than Go, which lets an error drop into `_`, and
stricter than Rust, which only warns on an unused result. It matches Swift,
where `try` is mandatory, and Zig, where an unused error union is an error.

**The `or` shorthand is limited to reserved words.** The parser reaches `or`
without type information and cannot tell a failable call from a boolean, so it
decides from the token after `or`. `else`, `exit`, `break`, `continue`, and
`fail` can never begin a boolean operand, so `a or else`, `a or exit`,
`a or fail`, and the rest are unambiguous, while a general `or someCall()` stays
boolean `or` of a function call. Reserving those words as keywords was considered
but not needed, since the one-token peek is unambiguous without a lexer change.

**No implicit cross-frame propagation.** Each routine that wants to forward a
failure writes `or fail` for itself; nothing propagates implicitly across
frames. Call chains in game scripts are shallow, so the ergonomic win of an
implicit propagation operator is small, and marking each hop keeps every place a
failure travels visible, which is the transparency the language is built on. It
is also the easiest form for the compiler to reason about: `or fail` is a local
divert plus a single check that the enclosing routine is failable, with no
analysis that threads failability across frames. If deep chains ever show up in
real scripts, an implicit operator can be added later as sugar over this same
marked forward. An `or else e do` binder (keep the value and inspect the error in
one statement) is worth revisiting later; the common case of forwarding the error
as is needs neither a binding nor a repeated call.

**The error signal is flag-independent.** The error rides a reserved return
register, and the caller does a plain compare-and-branch. Of the three targets,
RISC-V has no condition flags at all, and on x86-64 and aarch64 a callee's
epilogue clobbers the flags, so encoding the error in a condition flag would be
unimplementable on RISC-V and fragile elsewhere.

**Failable is the only exposed multiple-return.** The ABI is built generically
(an N-ary, class-tagged return), but only the failable form is exposed in the
source. General tuple returns in the source (`function f: (integer, integer)`
with `a, b := f()`) are a separate language question; the 2026-09-25 decision to
prefer out-parameters stands.

**Coroutines are not failable in this version.** A coroutine body may call
failable routines and must handle them locally, and uses `panic` for an
unrecoverable condition. This does not foreclose failable coroutines later:
adding an error channel to the resume protocol is additive, the same
second-register idea on the suspend path.

**Implementation order (all done).** The feature was built in this order, each
step independently landable:

1. Wire the second return register on all three backends, with the IR return as
   an N-ary, class-tagged operand list (the prerequisite, independent of the
   surface).
2. The `error` type, `fail(code)`, and `errorcode(e)`, with the int-code
   representation.
3. The `fails` signature marker, serialized into the interface and the
   fingerprint, with the strict no-overload rule. Contextual at the parser (so
   `fails` stays an ordinary identifier until step 4) and inert at lowering until
   the call-site forms land.
4. The call-site forms (`fails [e]`, `or else EXPR`, `or exit`/`or break`/
   `or continue`/`or fail`, `or else ignore`) and the must-consume check. The
   front end emits `RETV2` for a failable routine and `CALL2` at a failable call;
   `exit`, `fail`, and `fails` stay contextual (a one-token peek), so `Exit()`
   and `fail()` keep their builtin call path and the self-hosting fixed point is
   undisturbed.
5. Fixtures and docs: lex, parse, sema, emit, and a runcheck program exercising
   the value, error, divert, and discard paths.

## Coroutines and the host ABI

**Stackful coroutines over a private stack.** A coroutine is a runtime object
with its own heap-allocated private stack, reference counted like a handle.
Suspend and resume are a context switch between stacks, not a copy of the native
stack. Because the coroutine's frames live at fixed private-stack addresses, it
can be resumed from any caller depth and stored between ticks. Kobold does not
use Skjegg's copy-back `__cont_*` runtime, whose restored segments are tied to
the capture-time stack addresses (the Excelsior runtime is the precedent for the
private-stack approach).

**Drop by cancel-by-unwind.** Releasing the handles a suspended frame still owns
resumes the coroutine in a cancel mode that runs the front end's ordinary
scope-exit release code and returns, rather than walking a per-suspend-point
stack map. So no stack-map side-table is added to the module format. When a trap
unwinds out of a coroutine mid-flight, its C frames are abandoned rather than
cancel-unwound; the handles those register-allocated frames held cannot be
identified, so they stay live until `kp_vm_free` sweeps the whole table as a
teardown backstop.

**Untagged value stack.** The VM's value stack is an array of raw 64-bit cells.
Because Kobold is statically typed, cells carry no tags; the guest and host agree
on each argument's type from the routine's declared signature. This is where
Kobold departs from Lua, which tags every stack value.

**x86-64 float-argument staging.** Float arguments are staged through scratch
memory before the `xmm` argument registers are loaded, costing a memory round
trip on float-heavy calls. A register-level parallel move (order the moves so
each source is read before being overwritten, break any cycle with one scratch
register) would keep everything in registers and is worth doing if float-argument
throughput ever matters; the staging approach was chosen first for being
obviously correct.

## Console I/O

**The console seam is a POSIX file descriptor, not a stdio `FILE*`.** `write`,
`writeln`, `read`, and `readln` lower to a small set of `io_*` host functions that
carry the stream as an `int` file descriptor (1 standard output, 2 standard error,
0 standard input). A descriptor was chosen over a stdio `FILE*` because the sandbox
and VM boundary is itself descriptor- and handle-based (boris and smolmoo expose
`read`/`write` hypercalls, not a C stdio), so an fd is the natural runtime boundary
and stdio buffering is a layer that belongs above it, in an application or a
library, not in the language primitives. An fd is also a small integer, so it is
width-neutral and needs no guest pointer; a `FILE*` would have had to be a guest
value, which is the pointer-width coupling the descriptor model avoids.

**One byte of input pushback lives in the runtime.** The read side needs exactly
one byte of lookahead: a `read` of a number or a line scans a token and must stop
at the delimiter without consuming it (`read(a); read(ch)` on `"3x"` gives `a=3`,
`ch='x'`), and a `read` of a string detects the line end the same way. That one
byte is irreducible because `read`/`readln` are builtins and cannot require an
application `uses`. Everything larger (line buffering, a general `ungetc`, write
batching) is optional and belongs to an application or a library, so it is not in
the runtime. The pushback is a single static byte in the one shared console
implementation, not synthesized into guest code, so the VM and syscall layer still
implement only `read` and `write`.

**One implementation for both tiers.** The hosted runtime (JIT and native AOT) and
the freestanding runtime (the no-libc VM tier) share one `io_*` implementation over
`read`/`write`. Only real-number output is per tier, because the `%g` formatter
differs (the host C library's versus the freestanding `numfmt.c`'s, with
byte-identical output); its bytes still reach the fd with a plain write like every
other console write. This collapsed the earlier double buffering (a stdio `BUFSIZ`
on one tier, a separate pushback byte on the other) to the single byte, and removed
two divergences the two copies had: a `read` of one character returns -1 at end of
input (guard it with the end-of-input query), and a `read` of a string leaves the
terminating newline on the stream (standard Pascal `read`), which `readln` then
consumes.

## Module format (KPM1)

**Version 1 is a text, line-oriented format.** Text was chosen for early
bring-up: it is simple to write in Pascal and parse in C, and it is inspectable
and diff-able, which matches how Skjegg cross-checks its emitters. A binary
version 2 is indefinitely deferred, not planned: modules are small and loaded
once at startup, so parse speed and byte size are not bottlenecks, and a binary
form would add a second serializer and reader to maintain and fuzz for wins that
do not matter at this scale. Format growth within version 1 rides the reserved
section framework and the extension opcodes (for example `LOC`, `ADDO`), neither
of which needs a version bump. The version gate stays only as an escape hatch:
should a concrete need ever arise, a binary version 2 could be added with no
break to version 1.

## Pascal conformance choices

**Operand evaluation is right to left.** Fixed right to left to match the
bootstrap compiler, so the self-hosted and native compilers emit identical code.

**`round` is round-to-nearest-even**, matching Free Pascal; `trunc` is toward
zero.

**No `write`/`read` field-width and precision formatting** (`x:width[:decimals]`).
The field-width and precision syntax is a clumsy mini-language rarely needed in a
script; a script formats through the string builtins (`str`, `copy`) or a host
`external` instead.

**Free Pascal subset.** Most of Kobold's day-to-day ergonomics come from the
Borland lineage that Free Pascal continues, and `fpc` is the bootstrap compiler,
so the accepted source is a Free Pascal subset. The Kobold-original and
Compact-Pascal extensions on top are drawn partly from the Compact Pascal design,
partly Go-inspired, with the coroutine surface Kobold's own.

**No reference dynamic arrays (`ref array of T`).** Dropped to keep the array
surface to two cases, value `array of T` and host `buffer of T`; whole-array
alias-sharing was more surface and footgun than it earned. `ref` stays a reserved
word, and `slice` is held for a possible future windowed view (a subrange
`a[lo..hi]` sharing backing).

**`insert` opens a gap** rather than taking the value to insert, unlike Delphi's
`Insert`; the caller fills the opened slots by assignment.

## Units and separate compilation

**The Borland-family interface/implementation split.** A name is exported by
appearing in the `interface` part; its body lives in `implementation`. This was
chosen over per-declaration export markers and over a two-file definition/
implementation layout because it keeps the exported surface readable in one place
while staying a single file per unit.

**No `initialization`/`finalization` section.** Run-once setup is a deferred
question, and auto-run unit setup is hard for an end user to reason about (link
order, partial init on error). Setup is an exported routine the program or host
calls explicitly.

**Opaque-type sizes are computed, never hand-written.** A user-declared size
(`OPAQUE RECORD(32)`) was considered and rejected as fragile, because a
hand-written size drifts out of step with the real definition. The definition in
the implementation is the single source of truth, and the compiler derives the
size from it.

**A tunable scalar is a link-time const, not a property.** A value that should
change with only a relink (not a recompile of consumers) is a link-time constant
symbol, value-less in the interface. An earlier draft that expressed a changeable
value as a read-only property was retracted, because a plain scalar constant does
not need an accessor.

**One private dependency list; explicit re-export.** A unit has a single `uses`
clause, private by default, and states its public surface explicitly with
`reexport`. This decouples what a unit depends on from what it exposes, which the
Borland interface/implementation split conflates. Re-export is selective to avoid
the transitive-surface bloat that has caused trouble in other module systems: a
unit re-exports only the names its own interface genuinely requires a consumer to
name. This is close to Rust's `pub use`; classic Modula-2 has no such
convenience, relying only on automatic resolution. Kobold keeps that automatic
resolution and adds selective re-export as the curated middle.

**Definition folding is identical-or-error.** When a definition arrives at the
link twice (a diamond dependency packaged two ways), the two copies are folded
by content hash: byte-identical bodies fold to one, differing bodies are an error
that names both, never a silent choice of one. This is why traditional linkers
carry COMDAT and weak-symbol folding, and it is where the design departs from Go,
which would carry several versions of one dependency and treat them as unrelated.
The rule is order-independent by construction, and must never relax to "take the
first even if different," which would reintroduce a silent wrong-body hazard.

**Runtime glue through units, not builtins.** The standard-library units wrap
runtime glue (`kp_random_*`, the console and file primitives) declared
`external cdecl` in each unit's implementation, so the compiler core carries no
knowledge of them and no builtin grows. An operation stays a builtin only when it
needs special lowering; anything that is an ordinary call moves into a unit.

## Property and uniform access

**`property` is its own feature, not units-specific.** It is a general
type-system feature, a member that presents as a value but is backed by
accessors, applying to a unit's exported surface and to record fields alike.
Keeping it separate let units land with a small, clear export model (`var` for
state, `const` for compile-time values). It was originally designed alongside
units because it reuses the interface-inlining mechanism units already need for
an inline `const`. Units do not require it.

**Uniform access.** At the use site a property reads and writes exactly like a
variable, so `var` and `property` are interchangeable at the call site: a unit
can start with a plain `var` and later replace it with a `property` of the same
name, and consumers recompile but do not edit. For a trivial accessor the object
format can carry the accessor body in the interface so the consumer inlines it
across the unit boundary, lowering a property read to the same load a `var`
access would, with no call. An inlined accessor body is consumer-observable, so
it is part of the interface fingerprint.

## Semantics

**`map of T` matches structurally** (by value type `T`), made structural so that
an `or else` fallback, written as a separate declaration, can supply a map result
for a failable map-returning call.

## Future directions and open questions

- **A cell as a first-class value.** Shared mutable state that wants identity and
  later synchronization could be a `ref`/cell handle built on the existing handle
  and ARC machinery (the Rust `Arc<Mutex>` or Clojure atom shape), worth
  investigating as an alternative or complement to `shared var`.
- **Whether `kpcom` itself is ever split into units.** The compiler is a single
  Pascal file today, and the self-hosting fixed point does not depend on units;
  splitting it is not proposed.
