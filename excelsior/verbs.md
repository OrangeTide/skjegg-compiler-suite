# Verbs: one dot, one rule

Status: design study (2026-07). The verb system is the language's weakest
link on two fronts: a builder cannot say when to write `verb` versus
`func`, and the `recv:verb()` receiver syntax carries the last structure
symbol left in the language. Both problems resolve from one observation:
the server design needs a verb to be the boundary element and nothing
else, and each confusion below comes from letting it be less. Inputs: the
Compact Pascal review (receiver methods, explicit interface conformance)
and the de-arrow principle (symbols do math, words do structure).

Made by a machine. PUBLIC DOMAIN (CC0-1.0)


## Do we need verbs at all?

Asked before deciding the surface, answered from the game brief
(doc/game-scripting.md): yes, and the brief needs them narrowly, as the
boundary element and nothing else. Five commitments of the server design
each independently require an element with a verb's properties:

1. **Distribution and hot swap.** Content-addressed per-object
   freeze/thaw and live behavior swap forbid static linking of
   cross-object calls; binding must be late and name-keyed through a
   runtime-owned lookup (`__exc_send`'s selector resolution).
2. **Authority is reachability.** The permission model's enforcement
   surface is the send: hold a reference, you may send. One funnel needs
   one kind of crossing thing; extra crossing forms (remote calls, field
   access) would each be a capability hole.
3. **Extend-yes, mutate-no.** Overriding another author's `on_open`
   without editing the original requires dispatch by definition. The
   NWN event-slot alternative has no extension story across authors.
4. **The `.exi` is a contract of verbs.** Interface, disclosure, and
   moderation reason over a declared, stable protocol. Closures-in-slots
   cannot produce that contract (and freezing closures is the problem
   the design already avoids elsewhere).
5. **A verb activation is a coroutine.** The brief's own heading:
   "verbs are entry points, not the whole interaction." Sessions,
   dialogs, and durative effects live behind the entry as straight-line
   coroutine code.

The alternatives rebuild it: Erlang's raw receive is why gen_server
exists, and gen_server callbacks are verbs; ECS-with-native-systems is
already the plan for plain world objects, and the scripting layer exists
precisely for the residue needing author-owned, permission-checked,
per-entity behavior. Commit verbs fully to the boundary role: the
verb/func confusion and the private-verb leak below are both symptoms of
a verb pretending to be an interior helper. The brief also
wants verbs sparse (one `talk` entry hiding a whole session), which the
enforcement below encourages.

This also colors the send-syntax question: if cross-actor sends later
queue or suspend (the brief's open concurrency-granularity question),
Erlang's "sends look different" argument gains weight. The
counter-precedent is Pony, an actor language whose async behaviors are called
with plain dot and distinguished at the declaration (`be` versus `fun`),
which is exactly the verb/func split proposed here: distinction at the
declaration, one dot at the call.

## What a verb and a func actually are

Strip the syntax and the semantic difference is crisp:

- A **verb** is in the class descriptor: dynamically dispatched via
  `__exc_send`, inherited, overridable, sendable by other objects, listed
  in the generated `.exi`. It is the object's message protocol, and in
  the actor future it is the unit that scheduling, permissions, and
  `discloses` reason about.
- A **func** is statically bound code: callable bare by siblings, no
  dispatch, no descriptor entry, invisible to other objects.

So the real distinction is protocol versus implementation detail, which
is exactly what `public`/`private` already means. Today the two axes are
independent, giving four quadrants, and two of them are incoherent:

| | public | private |
|---|---|---|
| verb | the normal case | a leak: still in the descriptor, so still sendable cross-object (verified in `emit_class_desc`, which counts every `N_VERB` regardless of section); `private` only hides it from the `.exi` |
| func | callable by whom? nothing outside the class can call a func, so public adds nothing | the normal case |

The confusion a builder feels ("is `greet` a verb or a func?") is the
language making them choose twice for one decision. Notably, core.md's
own worked example (BarrowChest) never uses the incoherent quadrants:
its funcs are private helpers, its verbs are public entry points.

## The three surface problems

1. **verb versus func** has no teachable rule, and the incoherent
   quadrants make wrong choices representable (the leak in the table
   above).
2. **`recv:verb(args)`** uses the last structure symbol. The original
   rationale in core.md (marked settled) had two legs: the semantic
   distinction between dispatch and call-through-field, and coexistence
   with the `num:den` ratio. The ratio leg is gone (the literal was
   removed). The semantic leg is real and addressed below.
3. **Records need behavior** (`point.norm()`) without becoming classes,
   and sends on typed handles should be checked. The Compact Pascal
   review supplied both shapes.

## Survey, briefly

- **LambdaMOO** is where `obj:verb()` comes from; the colon earned its
  keep in a dynamic language where `o.f` and `o:f()` were both legal on
  any object and only syntax could tell fetch from dispatch.
- **Smalltalk / Objective-C**: everything is a message; no call-site
  distinction between kinds of callable. The uniformity is the
  teachability.
- **Go / Compact Pascal**: behavior attaches to plain data via receiver
  declarations; no dispatch unless an interface is involved. CP adds the
  explicit `implement I for T` block, single-pass verifiable.
- **D / Nim (UFCS)**: `x.f(args)` is sugar for `f(x, args)`; methods on
  value types need no new declaration form at all.
- **AppleScript**: `tell x to f()`; reads aloud, heavy for the common
  case. core.md already floats a `tell` block macro for send grouping.
- **Erlang/Pony (actors)**: sends are syntactically distinct because
  they are semantically heavy (queues, failure). The counterargument to
  full uniformity.

## Proposal

### 1. One rule: verbs are public, funcs are private

`verb` may appear only under `public`; `func` only under `private`. Both
of the incoherent quadrants become compile errors, with teaching
messages ("a verb is a message others can send; it belongs in `public`.
For a private helper, write `func`"). The builder's decision collapses
to one question they already understand: is this something others can
ask me to do, or my own recipe? The `.exi`, the descriptor, and the
permission model all align for free, and the private-verb reach leak
becomes unrepresentable. Sections keep their independent meaning for
fields (public and private state both exist legitimately).

The cost: no protected override hooks (a dispatched verb that outsiders
cannot send). That is a real pattern in mature OO code and a
sophistication v1 builders don't need; if it earns its way in later, it
can arrive as an explicit annotation rather than a quadrant accident.

### 2. One dot: `recv.verb(args)` is the send; `:` leaves expressions

Unify on the dot. Resolution is static and unambiguous:

- Verbs and fields already cannot collide in a class (one folded
  namespace), so on a receiver of known class type, `x.name(args)` is a
  send if `name` is a verb and a call-through-field if it is a field.
  The declaration carries the distinction; the call site doesn't need
  to repeat it.
- On an untyped `obj`, dot-call is always a send. Cross-object field
  access is not part of the actor model (state is private to the actor;
  reading another object's data is a message), so there is nothing else
  a dot on a foreign object could mean.
- `self.field` and record field access are unchanged.

What is genuinely lost: at a glance, `a.f()` no longer says whether it
dispatches or invokes a stored closure. Three mitigations: the checker
always knows and error messages say which; the `.exi`/`discloses`
manifest remains the authoritative list of outbound sends (computed by
the compiler, not by grepping for colons); and the AppleScript-style
`tell` block stays available as a later macro for making send-heavy code
read as sends. The colon then survives only inside the generated
disclosure syntax, out of the expression language entirely, finishing
what the de-arrow started. (The string-pick DSL, its other refuge when
this was written, has since been retired for the general `cond then A
else B` if-expression; see string-plan.md.) The lexer keeps a
migration hint at expression-position colons ("sends use `.`").

This supersedes the "settled" core.md note with eyes open: one of that
note's two legs (ratio coexistence) no longer exists, and the other
(dispatch versus data-call) moves from call-site punctuation to
declaration-site fact plus checker knowledge.

### 3. Bare sibling calls: verbs dispatch, funcs are direct

`bump()` with no receiver, where `bump` is a sibling verb, lowers as a
dispatched self-send, so subclass overrides take effect (the classic OO
correctness rule). A bare call to a sibling func stays a direct static
call. Builders never think about this; it just makes overriding work.

### 4. Records get behavior by UFCS

When records land, `p.norm()` resolves as the func call `norm(p)` when
`p` is a record (or other value type) and `norm` is a visible func whose
first parameter matches. This is CP's standalone receiver method with
zero new grammar: no `for` clause, no receiver declaration form, still
single-pass checkable. Dot resolution order, fixed and documented:
member field, then member verb (classes only), then UFCS func;
declaration-time collisions between fields and verbs remain errors, and
a UFCS func shadowed by a member is simply not considered.

### 5. Checked sends: class types today, interfaces later

`check_send` already fully checks sends on class-typed receivers (verb
exists, argument types); `spawn(ClassName)` returns the class type, so
this works now, and `obj` remains the dynamic escape. The interface
type (a named verb-set with CP-style explicit conformance, verified
single-pass at its `end`) is the eventual way to type handles across
modules without naming concrete classes; it is what `discloses` already
gestures at from the consumer side. Deferred until `.exi` files
actually flow between modules; nothing in 1 to 4 forecloses it.


## Impact when implemented

- Grammar: the `:` send leaves `postfix_op`; keyword section rules for
  verb/func placement; UFCS note under calls; no new tokens.
- Compiler: parser drops the colon send (with a migration hint);
  resolver/typechecker enforce section rules and dot resolution order;
  lowering routes dot-sends through the existing `__exc_send` path and
  bare sibling verb calls through it too.
- Tests: every `target:apply(...)` style send becomes `target.apply(...)`;
  any public func moves to a private section (e.g. exs_match's
  `classify`).
- Docs: core.md's verb-send note and the barrow example, host-abi.md is
  unchanged (the ABI never knew about the colon), CLAUDE.md, tutorial
  TODO gets its one-sentence rule: "public verbs are what others can
  ask you to do; private funcs are how you do it; the dot asks."


## Decisions (confirmed and implemented 2026-07)

1. One dot, colon leaves the expression language: YES. The checker
   morphs a dot-call into a send when the name is a verb of the
   receiver's class or the receiver is an untyped obj; call-through-
   field survives for fields holding callables; the lexer answers an
   expression-position `:` with a migration hint. Colon now exists only
   in the generated disclosure notation (the string-pick DSL that also
   used it has since been retired for `cond then A else B`).
2. Verbs are public, funcs are private: YES, enforced in the parser
   with teaching errors in both directions. The private-verb descriptor
   leak is now unrepresentable.
3. Bare sibling verb calls dispatch: YES. Lowered as a self-send
   through `__exc_send` (receiver `__exc_self`), so overrides take
   effect; bare func calls stay direct static calls.

UFCS for records and interface types remain direction items, to be
decided when records lower and when `.exi` files flow between modules.
