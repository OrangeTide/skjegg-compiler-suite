# By-reference parameters: `shared`

Status: decided (2026-07); implemented (D1-D7). How a routine holds an
indirect reference to a caller's variable and changes it in place, spelled
for a beginner who may not know another language. The answer is a
by-reference parameter mode, Pascal's `var` parameter reworked with a
plain-English keyword and scoped to the actor model, not a first-class
pointer or reference type. All seven decisions are confirmed; the
implementation waits on per-instance field storage for the self-field
case (a `shared` local works before it).

Made by a machine. PUBLIC DOMAIN (CC0-1.0)

## The problem

"Hold an indirect reference, like a C or Pascal pointer" bundles several
unrelated needs, and Excelsior already answers most of them:

- **Refer to a thing in the world** (a room, an item, another actor): an
  `obj` or class-typed variable already is a reference. Assigning shares
  it, `nil` is absent, sends go through it. No pointer syntax, no
  dereference, no crash on a bad address (a located fault instead).
- **Change another actor's state**: send it a verb. Cross-object field
  access is not part of the actor model (verbs.md); an actor changes its
  own fields, prompted by a message.
- **Remember an action to run later**: a callable-valued field (a
  call-through-field) is the function-pointer need.

What is left is the one genuinely missing capability: **let a routine
operate on a caller's own slot**, a local or a `self` field, so a helper
can read and write it in place. `clamp`, `normalize`, `roll_stats`: the
"compute, return, and reassign" dance a by-reference parameter removes.

The low-level answers do not fit the audience. C and Pascal pointers
(`^T`, `*T`) and Zig pointers (`*T`, `.*`) put memory addresses and manual
dereference in the surface; C++ `&&` and Rust borrows put ownership and
lifetimes there. None of that belongs in a language whose runtime owns
memory (arena now, GC and freeze/thaw later) and whose aesthetic is words,
not sigils.

## The design

**A by-reference parameter mode, not a reference type.** A parameter
marked `shared` binds to the caller's variable for the duration of the
call: it is the caller's own slot, not a copy, so the routine's changes
are the caller's changes. There is no `shared int` *value*, only a
`shared` parameter, so the reference can never be stored in a field or
returned, and the whole lifetime and dangling-reference problem is gone by
construction.

    func clamp(shared value is int, low is int, high is int)
        if value < low
            value = low          // writes through to the caller's slot
        endif
        if value > high
            value = high
        endif
    endfunc

    clamp(self.health, 0, 100)   // self.health: my own field, ok
    var score is int = raw
    clamp(score, 0, 999)         // a local, ok

**The keyword is `shared`, chosen to read off its own surface.** A
neophyte reads "the value is shared between you and the routine, so a
change the routine makes is a change you keep." It carries none of the
memory or concurrency baggage a non-programmer would have to un-learn.
`inout` was rejected as a coined token a beginner cannot decode, and
`mutable` as naming the wrong axis: a by-value parameter can be mutable
too (you reassign your own copy), so mutability is not the distinguishing
property. The distinguishing property is that the change flows back out,
and `shared` (the caller's actual variable) captures that. `var` (Pascal's
own word) is familiar from local declarations but is an abbreviation and,
in a signature, looks like a local declaration and hides the aliasing.

### Value categories: the argument must be a place

The mechanism is the classic lvalue / rvalue split, kept inside the
compiler. A `shared` argument is bound in *place* mode: the compiler
captures the argument's location, it does not evaluate it to a value.
Inside the callee, reading the parameter decays to a load through the
location and writing stores through it. So:

    clamp(a + 123, 0, 999)       // error: a + 123 is a computed value

`a + 123` is an rvalue, a computed value with no home to write back to, so
it cannot be shared. The check is the predicate the compiler already uses
to vet an assignment target: **if it cannot sit on the left of `=`, it
cannot be `shared`.** The teaching error says so in plain terms ("you can
share a variable, not a computed value like `a + 123`"), which is itself
an explanation of the rule.

### Which places qualify

The qualifying lvalues are the stable, settable, non-fallible slots:

- **local variables**: always (a stack slot; works today).
- **`self` fields**: yes (couples to per-instance field storage, so this
  case waits on that work; a local works before it).
- **a forwarded `shared` parameter**: yes, it is already a place, so a
  helper may pass its own `shared` parameter onward.

And the exclusions, each for a concrete reason:

- **`str` / `list` elements** (`s[i]`, `xs[i]`): no. Strings are
  immutable and list mutation is copy-on-write (a fresh list), so there is
  no stable slot to write through; a `shared` element would write into a
  copy no one else sees.
- **`const`**: no, it is not settable.
- **fallible places** (an index): no, the slot might not exist; this
  falls out of the copy-on-write exclusion and keeps the qualifying set to
  places that always have a home.
- **rvalues** (a binop, a literal, a call result, an `otherwise`
  result): no, the error above.

### It stays inside one actor

`shared` is allowed on **funcs, not verbs**. A verb is the actor's public
interface and is reached by a send, which crosses the actor boundary; a
`shared` argument is a location in the sender's own storage, meaningless
on the far side of a send. Funcs are the intra-object helpers, called
directly, in the same storage. So a `shared` reference never leaves the
actor that made it, which is why the cross-object "who may write this
field" question never arises: you do not pass another actor's field to a
helper, you send that actor a verb, and its verb uses `shared` helpers on
its own state. The restriction is the actor model, not an extra rule.

### The cross-object utility is not a gap: it lives in the meta layer

Excluding cross-object mutation from the base layer raises the fair
question of where reusable "operate on an object's field" utilities go.
They belong to the meta layer, as behavior a class opts into, not as an
outsider reaching in. The two are the difference between declared
conformance (traits, mixins, roles) and duck-typed reach-in: a cross-
object `shared` reference imposes on any object whose field can be named,
while a mixin the class *includes* is taken on with consent, at definition
time.

The mechanism keeps it honest. A macro expands *into* the class body, so
every field access it generates is `self`-access in the including class,
legitimate intra-object code. And the meta layer cannot escalate past the
base layer's encapsulation: a macro that tried to expand to `other.field =
x` would produce code the base language rejects. Macros give reuse, not
new powers. An outsider still reaches the behavior only through the verb
the mixin added, which is a send, and which exists only because the class
chose to include the mixin. The object offered the verb; the caller did
not force it.

So "change an object's state" has three coherent tiers, and `shared`
being func-only (D5) is the correct boundary rather than a gap:

- **in place, in one object**: a func with a `shared` parameter (base
  layer, this note).
- **reusable behavior across classes**: a mixin the class includes (meta
  layer), expanding to self-access plus a sendable verb.
- **change another object**: send it a verb (always).

Even a two-object utility decomposes this way: "transfer from A to B" is
not a function holding two foreign fields, it is a `wallet` mixin that A
and B both include (each getting `balance` and `add` / `remove`), and the
transfer is `from.remove(n)` then `to.add(n)`, two sends, each object
mutating its own field.

The actual spelling of a mixin (`include`, the macro form, how members
merge, name collisions) is the meta layer's own design and is deferred to
a future mixin / trait design pass; this note only records that the
cross-object utility need has that home, so the func-only rule closes
cleanly instead of reading as a missing feature.

### One mode for v1

`shared` is read-write: the routine may read the current value and change
it. A separate write-only `out` mode (Ada / C#) would add only one thing
mechanically, definite-assignment analysis (proving the callee writes the
parameter on every path before returning, and forbidding a read before
that write). Its use, producing a result, is already served by `returns
maybe T` (the try-pattern, `if var x = parse(s)`) and by returning a
record for several outputs. So v1 ships one read-write mode and leaves
`out` as a later refinement if a concrete need appears, rather than pay
for the analysis speculatively.

## Survey

Every language with this feature requires an lvalue argument and rejects a
computed value; they differ only in spelling and in whether the reference
can also be a first-class value.

- **Pascal `var` parameter**: the direct ancestor. `procedure Drain(var
  stat: Integer)`; `Drain(x + 1)` is "variable identifier expected". A
  parameter mode, not a type; not storable. This is exactly the model
  here, respelled.
- **Ada `in out` / `out`**: names the direction; `out` adds the
  definite-assignment discipline discussed above.
- **C++ `T&`**: aliases without a sigil at the use site; `T&&` adds move,
  which is ownership transfer, a manual-memory concern the runtime
  removes here.
- **Swift `inout`, C# `ref` / `out`**: the same mode with coined
  keywords; `inout` is the token rejected here for a beginner audience.
- **Rust `&mut`**: safe by borrow-checked lifetimes, the heavy version of
  the safety that `shared` gets for free by not being storable.

Excelsior's contribution is not the mechanism, which is settled prior art,
but the two audience-driven choices: the plain-English keyword `shared`,
and the actor-model scoping (func-only) that keeps the reference from ever
crossing a send.

## Implementation sketch

Under the hood a `shared` parameter is an address: the call site passes
the location of the argument's slot (a local slot, or a field), and inside
the callee each read is a load through that address and each write a store
through it. The surface never shows the address. Reuse:

- the assignment-target resolver (`resolve_lval` in lower.c) to both
  validate the argument is a place and produce its location;
- the existing load/store-through-location lowering (the same paths a
  field or slot store already uses).

The one new IR need is taking the address of a local slot or a field
global if the builder does not already have it. The self-field case is
coupled to per-instance field storage (a `shared` reference to `self.hp`
must be this instance's slot, not the shared global), so it lands after
that; a `shared` local works before it.

`prop` is freed from being this mechanism. It stays reserved for a
possible future storable, first-class reference, which would reintroduce
the lifetime question and is not needed for the "operate on a caller's
slot" case that `shared` covers.

## Decisions (confirmed)

**D1. The mechanism is a by-reference parameter mode, not a first-class
reference type.** Pascal's `var` parameter, not a `prop` value. This
deletes the lifetime and storability problems by construction.

**D2. The keyword is `shared`.** A real word a beginner can decode ("the
caller's own variable, shared with the routine"), over the coined `inout`,
the wrong-axis `mutable`, and the abbreviation `var`.

**D3. A `shared` argument must be an lvalue.** It reuses the
assignment-target predicate: what cannot sit on the left of `=` cannot be
`shared`. A computed value is a teaching error explaining why.

**D4. The qualifying places are locals, `self` fields, and forwarded
`shared` parameters.** Not `str` / `list` elements (immutable /
copy-on-write), not `const`, not fallible places.

**D5. `shared` is func-only, not on verbs.** A verb crosses the actor
boundary via a send; a `shared` location is meaningless there. So a
`shared` reference never leaves the actor, and the cross-object write
question does not arise.

**D6. One read-write mode for v1.** `out` (write-only, with
definite-assignment) is deferred; producing values is `returns maybe T`
or a record return.

**D7. Lifetime is safe by construction.** A `shared` parameter is
call-scoped and cannot be stored in a field or returned. The storable
first-class reference (`prop`) stays deferred behind its own lifetime
work.
