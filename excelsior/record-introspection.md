# Record introspection: asking a record about its fields

Status: decided (2026-07); implemented as a vertical slice (`typeof`,
`fieldsof` with `.name`/`.type`/`.tags`, `fieldtype`, plus the siblings
`verbsof` and `membersof`, all compile-time only inside a `macro` body; see
`tests/exs_macro_introspect.exs`). Deferred: `hasfield`, `fieldtype`
fallibility, and inherited verbs in `verbsof`.
The fourth and final pass of the data-model spine
(backlog.md, TODO line 46), and the follow-on records.md D8 and meta.md D5
both defer to. Tier: Meta (tier 3); a builder never writes these primitives, a
runtime-library author's macro does (tiers.md).

records.md D8 fixed the *requirement*: a record's fields are a first-class,
named, ordered set the compiler can be asked about, with three minimal
capabilities (field presence, field type, field iteration), compile-time and
zero runtime cost, on the Zig / D / Nim model. meta.md D5 fixed the *home*:
these are meta-layer primitives a procedural macro calls over the builder-stack
record layout. This pass settles the last open piece, the concrete spelling of
the primitives and how they thread through a macro body.

Made by a machine. PUBLIC DOMAIN (CC0-1.0)

## Why introspection at all

A macro that writes a serializer (freeze/thaw), a pretty-printer, a
field-by-field diff, or a `match` skeleton must ask a record about its own
shape rather than have the author hand-list the fields. The canonical case is
freeze/thaw: a record crosses a send by copy (records.md D5) and its
serialization walks the fields, so a `freeze(Point)` macro reads `Point`'s
fields and emits the write calls, and adding a field to the record updates the
serializer with no further edit. Without introspection every such macro is
hand-maintained against the record and drifts. So records expose their
structure to compile-time code, exactly as Zig, D, and Nim do, and unlike Go's
runtime `reflect`.

## The primitives

Three bare-name, compile-time meta primitives, in the `length` / `spawn` /
`append` style (built-ins the compiler knows, not prelude macros and not new
types). They are invoked only from meta-layer code (a macro body); a builder
never sees them.

- **`typeof(expr)`** yields the static type of an expression as a compile-time
  type value. It is the bridge from a value to a type: a macro invoked as
  `freeze(p)` gets the record type with `typeof(p)`. It always succeeds (an
  expression has a static type). It also underpins the `_Generic` tier
  (core.md), which selects on an argument's static type.
- **`fieldtype(T, "name")`** is a **fallible** expression (fallible.md): it
  yields the field's type as a compile-time type value on success, and **fails
  when `T` has no field named `name`**. The field name is data that may or may
  not match, so a miss is a failure to consume, not a hard error, exactly as a
  keyed lookup (`s[i]`, `find`) is fallible while a type error is not. Consuming
  the failure is the presence test, so this one primitive covers both "does the
  field exist" and "what is its type" (Zig's separate `@hasField` and
  `@FieldType`, folded into one keyed lookup):

        // presence and type in one lookup:
        if var t = fieldtype(T, "hp")
            // reached only if hp exists; t is its type
            if t is int  ...
        endif

        // a fallback type when the field may be absent:
        var kt = fieldtype(T, "key") otherwise str

- **`fieldsof(T)`** yields the record's fields in declaration order as a
  compile-time sequence of **field descriptors**, each exposing `.name` (a
  compile-time text) and `.type` (a type value). This is the primitive a
  serialize or map-over-fields macro is built from, iterated by the
  compile-time `for` below. (Zig `@typeInfo(T).Struct.fields`, Nim
  `fieldPairs`, D `__traits(allMembers)` / `.tupleof`.)

These three cover records.md D8's capabilities: presence and type both come
from the fallible `fieldtype`, iteration is `fieldsof`, and `typeof` bridges a
value to a type. A separate boolean `hasfield` is not a primitive; presence is
consuming `fieldtype`'s failure, which is stronger than a boolean because
`if var t = fieldtype(...)` binds the type in the same step a bool would only
answer yes/no. A bare boolean presence, if a macro ever wants one, is a
one-line derived helper (`fieldtype(T, n) otherwise nulltype; ...`), not a core
primitive.

### Fallibility lifts into the meta layer

`fieldtype` being fallible means the fallibility machinery (fallible.md) applies
to compile-time code: a failure propagates Icon-style to the nearest consumer
(`otherwise`, `if var`, `while var`, capture into a `maybe type`), and **an
unconsumed failure at a macro boundary is a compile error**, the compile-time
analog of fallible.md's runtime trap. So a macro author who writes `var t =
fieldtype(T, "hp")` on a field that might be absent, with no consumer, gets a
compile error steering them to `if var` or `otherwise`, the same teaching the
runtime path gives. This is a reason to prefer the fallible `fieldtype` over a
hard-erroring one: the neophyte-facing failure discipline the language already
has does the work, at compile time, with no new rule.

### Saving and binding a result: the same surface as the runtime layer

A macro saves a `fieldtype` result exactly as any code saves a fallible one,
because the meta layer is the same language and reuses the whole binding surface
(there is no meta-specific capture rule). Three forms, all fallible.md verbatim:

- **Plain capture of the success value.** `var t = fieldtype(T, "abc")` binds
  `t` to the field's type and, per fallible.md, *infers the success type and
  does not capture failure*: a miss propagates out. This is the right form when
  the name is known present, the common case inside `for f in fieldsof(T)`
  where every `f.name` exists by construction, so the bare capture never fails.
  If the field might be absent and nothing consumes the failure, it reaches the
  macro boundary and is a compile error, steering you to one of the next two.
- **Explicit `maybe` capture, storable.** `var t is maybe type = fieldtype(T,
  "abc")` captures the type-or-absence into a `maybe type` temporary you inspect
  later, the explicit `is maybe T` capture (fallible.md: inference never
  captures, the `maybe` is written). Here `type` is the meta layer's kind of
  compile-time type values.
- **Inline consume-and-bind in a condition.** `if var t = fieldtype(T, "abc")`
  binds `t` on success and takes the branch, skipping on absence; `while var t =
  ...` is the loop form. These are fallible.md's `if var` / `while var`
  consumers, and this is the presence-and-type test in one step.

So `var tmp = fieldtype(...)` works, and inline `if var` / `while var` bindings
work, because they are the existing surface, not additions. The one caveat is
the inference-never-captures rule: a plain `var`/`const` binds the *success*
value, and if you want to hold the possible-absence you write `is maybe type`
or consume it. `if var` / `while var` are specifically the fallible
success-test binders (a plain bool `if`/`while` tests a bool and rejects a
fallible, fallible-consumers.md); an ordinary non-fallible value is bound by a
plain `var`/`const` statement in the macro body, then used.

### The names: words run together, no sigil, no underscore

The precedents mark their reflection with sigils Excelsior does not use: Zig's
`@hasField` builtin-`@`, D's `__traits` double-underscore. Both are ruled out
by settled conventions, no `$ @` sigil soup (core.md) and no reserved name with
an underscore anywhere (core.md, the `setof` precedent). So each primitive is a
plain lowercase identifier with its words run together: `typeof`, `fieldtype`,
`fieldsof`. They read as English and are searchable, the same test the whole
series applies to names, and `fieldsof` uses the revived small word `of` run
into the name exactly as `setof` would (union-types.md revived `of` as a
connecting word; here it is glued into a builtin name, not a free operator).

### Field iteration is a bounded compile-time `for`

Walking `fieldsof(T)` needs a compile-time loop, which general compile-time
control flow (a def-region `if`) is deferred and called complexity-balloon
territory in core.md. Field iteration does **not** need that general power: a
record has a fixed, known, small field count, so `for f in fieldsof(T)` is a
**bounded static-foreach**, fully unrolled at expansion into one copy of the
body per field, always terminating. It is the same synthesize-a-body-per-item
machinery as the inline-`for` backlog item (TODO line 26) and mixed-lists.md's
deferred implicit type-dispatch `for`, and this pass shares that mechanism
rather than inventing a second. Each unrolled copy is **monomorphic**: in the
copy for field `f`, `f.type` is one concrete type, so the emitted body
type-checks against a real type, not a dynamic one. This is Zig's `inline for`
over `@typeInfo` fields and Nim's `for name, val in fieldPairs`.

### Depth is recursion, not machinery

The single-level `fieldsof` walk is all the iteration machinery this pass
needs, because a macro may **call another macro, including itself**, and deep
introspection is that recursion. A macro walking a record's fields, on reaching
a field whose type is itself a record (its `.type` a composite `fieldsof` also
accepts), calls itself on that type; a nested tree of records serializes to any
depth with no deep-walk primitive, just one level of `fieldsof` and ordinary
recursion. This is why field iteration does not need a "recurse into nested
aggregates" mode a runtime reflector would carry; the meta layer is a
programming language, so it recurses.

The bound is a **compile-time recursion-depth cap**, an extreme maximum the
compiler's compile-time stack can hold (the same stack the meta-stack
definitions and macro expansion already run on). A traversal that exceeds it,
a pathological type nesting or an accidental non-terminating macro, is a
compile error naming the type chain, not a compiler stack overflow. The cap is
a safety backstop set far above any real record nesting, the meta-layer twin of
a runtime recursion limit, so ordinary deep introspection is unbounded in
practice and only a runaway is stopped.

### The field descriptor is minimal

A descriptor carries `.name` and `.type` and nothing more in v1, the minimum
records.md D8 named. Deferred extensions, each additive when a use appears: a
field's default-value presence (for a constructor-generating macro), a class
field's `public` / `tunable` visibility (visibility.md, when introspection
extends to classes), and layout offset (rarely needed, since the macro emits
`get(v, name)` and lets the compiler place it). Keeping the descriptor small
keeps the compile-time surface small.

## How it threads through a macro

Introspection lives in a **procedural macro** (meta.md D3, the power case): the
body runs at compile time, reads the record's shape, and assembles output with
`quote` / `quasi` and `${}` holes. A freeze macro, sketched:

    macro freeze(v)
        // v is the auto-quoted argument form; its static type is the record
        var body = quote seq()
        for f in fieldsof(typeof(v))
            // one unrolled copy per field; f.name is text, f.type a type
            body = append(body, quasi writefield(${f.name}, get(${v}, ${f.name})))
        endfor
        body
    endmacro

At the call `freeze(p)` where `p is Point`, `fieldsof(typeof(p))` is the two
descriptors `(x, decimal)` and `(y, decimal)`, the loop unrolls to two
`writefield` forms, and the macro splices them into the call site as ordinary
core forms the base checker then validates. Nothing about the introspection
survives to runtime: the field walk happened at compile time and expanded to
plain `writefield` calls. A macro that must branch on a named field's type
consumes `fieldtype`'s fallibility, `if var t = fieldtype(T, "key") ... t is str
...`, compile-time code choosing what to emit, with the absent-field case
handled by the `otherwise` / `if var` consumer rather than a special-cased
error.

## Scope: records now, the same shape later

The primitives are settled over **records**, the pass's subject and the pure
data case. The same shape generalizes, additively, when a use appears:

- a **class** also has fields (introspectable the same way) and **verbs**; a
  `verbsof(T)` companion for dispatch-table or proxy macros is the natural
  extension. Implemented (2026-07): `verbsof(T)` yields a class's own verbs as
  descriptors with `.name`, `.returns`, and `.params` (a name/type descriptor
  sequence); inherited verbs are still deferred.
- an **enum** has members; a `membersof(T)` reading the small closed member set
  (enums.md) is the parallel for an enum-driven macro. Implemented (2026-07):
  `membersof(T)` yields the members as `.name` descriptors (a member walk needs
  only the type, so it works ahead of the enum runtime); a member's `.value`
  ordinal is deferred until an enum-dispatch macro needs it.
- a **shape** has node kinds and slots (typed-data.md); introspecting a shape
  is how a shape-over-records desugaring would read its own schema.

This pass does not build those; it fixes the record primitives and the naming
pattern (`Xof(T)` for the field/member sequence, `Xtype(T, "name")` for the
keyed, fallible per-item lookup) they would follow, so the family stays
coherent.

## Guardrails

The meta.md D6 walls hold, and introspection does not widen them:

- **Meta-tier, library authors only.** These appear only in macro bodies; a
  builder writing world or mechanics code never types `fieldsof`. A stray use
  in ordinary code is a teaching error pointing at the macro context.
- **Compile-time, zero runtime, no metadata.** The introspection is a read over
  static structure at expansion; it emits no runtime type table and there is no
  runtime `reflect`. This is the Zig / D / Nim stance, chosen over Go's runtime
  reflection precisely so nothing survives to cost the running actor.
- **No new power.** A macro reads a record's shape and emits base-layer forms
  the base checker validates (meta.md D6); introspection lets it *rearrange*
  over the fields, not mint access it did not have. Reading a field's type is
  not reading another actor's state, the actor boundary is unchanged.
- **Teaching errors.** An unconsumed `fieldtype` failure at a macro boundary is
  a compile error naming the field and pointing at `if var` / `otherwise` (the
  fallible.md discipline, lifted to compile time); `fieldsof` on a
  non-composite type is a category error that says so; exceeding the recursion
  depth cap (below) names the type chain that ran away.

## Survey

- **Zig**: `@hasField(T, "n")`, `@FieldType(T, "n")`, `@typeInfo(T)` with
  `inline for` over `.Struct.fields`, all `comptime`, zero runtime. The direct
  model; Excelsior takes the capabilities, folds the separate presence and type
  builtins into one fallible `fieldtype`, and drops the `@` sigil.
- **D**: `__traits(hasMember, ...)`, `__traits(allMembers, ...)`, `.tupleof`
  for field iteration, `static foreach`, compile-time. The bounded-foreach
  precedent; Excelsior drops the `__traits` double-underscore.
- **Nim**: `for name, val in fieldPairs(x)` and `fields`, compile-time field
  iteration, plus macro-level `getType`. The (name, value/type) pair-walk shape.
- **Rust**: no built-in field reflection; a derive proc-macro parses the
  struct's tokens (syn) and walks the parsed fields. The same generated-code
  outcome by a different route; Excelsior's macro reads the type environment
  directly rather than re-parsing tokens.
- **Julia**: `fieldnames(T)`, `fieldtype(T, i)`, `nfields`, constant-folded and
  usable in `@generated` functions. The name `fieldtype` is Julia's.
- **Go `reflect`**: runtime field walking with a runtime type table. The model
  explicitly **not** taken, because it costs the running program.
- **Crystal / Elixir**: `{{ T.instance_vars }}` and `__struct__`, compile-time
  macro introspection in the same spirit.

Excelsior's stance: Zig's capabilities and Nim's pair-walk, spelled as plain
run-together builtins (`typeof` / `fieldtype` / `fieldsof`) with no sigil and no
underscore, presence folded into a fallible `fieldtype`, one-level iteration by
the shared bounded static-foreach with depth by recursive macro calls,
compile-time with zero runtime metadata, called only from procedural macros.

## Decisions (confirmed)

The eight decisions are confirmed. The implementation (the three primitives over
the builder-stack record layout, the fallible `fieldtype` reusing the fail-label
machinery at compile time, the bounded static-foreach shared with the inline-
`for` work, and the recursion-depth cap) is the follow-up, and depends on the
meta layer (meta.md) being built. This closes the data-model spine.

**D1. Three bare-name compile-time meta primitives:** `typeof(expr)` (the
value-to-type bridge, infallible), `fieldtype(T, "name")` (a **fallible** keyed
lookup yielding the field's type, failing if absent), and `fieldsof(T)` (the
fields in declaration order as descriptors). They cover records.md D8's
capabilities: presence and type both come from consuming `fieldtype`, iteration
is `fieldsof`, and `typeof` bridges a value to a type.

**D2. `fieldtype` is fallible, subsuming a separate `hasfield`.** A missing
field is a failure (the field name is data that may miss, like `s[i]` / `find`,
fallible.md), consumed by `if var` / `otherwise` / capture into `maybe type`,
so presence and type come from one lookup and `if var t = fieldtype(...)` binds
the type a boolean would only answer yes/no about. Fallibility lifts into the
meta layer: an unconsumed failure at a macro boundary is a compile error (the
compile-time analog of the runtime trap). A boolean `hasfield`, if ever wanted,
is a one-line derived helper, not a primitive. Saving a result reuses
fallible.md's binding surface verbatim, no meta-specific rule: `var t =
fieldtype(...)` infers the success type and does not capture failure (right when
the name is known present, as inside `fieldsof`), `var t is maybe type = ...`
captures the possible absence, and `if var` / `while var` consume-and-bind.

**D3. Names run their words together, with no sigil and no underscore.** Zig's
`@` and D's `__traits` are ruled out by core.md's conventions; `fieldsof` glues
the revived `of` into the name (the `setof` precedent).

**D4. Field iteration is a bounded static-foreach; depth is recursion.** `for f
in fieldsof(T)` is fully unrolled at expansion, one monomorphic copy per field
(`f.type` a concrete type in each), sharing the inline-`for` machinery (TODO
line 26). A single level is all the machinery needed, since a macro calls
another macro (or itself) to descend into a nested record's `fieldsof`, so deep
introspection is ordinary recursion bounded by a compile-time recursion-depth
cap (a runaway is a compile error naming the type chain, not a stack overflow).

**D5. A field descriptor carries `.name` (compile-time text) and `.type` (a
type value), minimal.** Default-presence, class-field visibility, and layout
offset are additive extensions deferred until a use appears.

**D6. The "field of type T" predicate is a composition, not a further
primitive:** consume `fieldtype` and test the bound type, `if var t =
fieldtype(T, "n") ... t is int ...`, reusing `is` lifted to compile-time
type values.

**D7. The primitives are settled over records; the same `Xof(T)` sequence and
`Xtype(T, "name")` keyed-fallible-lookup pattern extends to classes
(`verbsof`), enums (`membersof`), and shapes later, additively.** This pass
builds only the record primitives.

**D8. The guardrails hold (meta.md D6):** Meta-tier and library-authors-only,
compile-time with zero runtime metadata (the Zig/D/Nim stance, not Go
`reflect`), emitting base forms the base checker validates so it mints no new
power and does not cross the actor boundary, with teaching errors on misuse.
