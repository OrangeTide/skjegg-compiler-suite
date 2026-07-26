# Object slices: a verb subset is an interface

Status: decided (2026-07); implemented (D1-D5, D7; D6's fallible `as` cast from
an untyped `obj` is not built, see the implementation notes). The object
counterpart of record-slicing.md, raised 2026-07
while unifying the record and object layout engines. Tier: Mechanics to write a
slice type, World to use one (tiers.md). Builds on record-slicing.md (the
`with` word and the restricted-parameter form), verbs.md (dispatch, and D5's
deferred interface type), and data-model.md (the record-versus-object split).

A record can be restricted to a subset of its fields. The same question asked
of an object has a different answer, and a more useful one: an object
restricted to a subset of its **verbs** is an interface. `chest with (open,
close)` names not a narrower chest but *anything that opens and closes*, which
is the type the language has been missing between `obj` (anything at all) and a
class name (exactly one implementation).

Made by a machine. PUBLIC DOMAIN (CC0-1.0)

## Why this slice is structural when the record slice is not

record-slicing.md D2 is emphatic that a record slice is **same-type only**: two
different records that happen to share `x` and `y` do not compare or assign,
because records are nominal and slicing opens no structural back door. The
object slice proposed here is the opposite, structural and cross-type, and the
reason is not a change of taste. It is that the two slices expose different
things.

A **record slice exposes layout.** `p with (x, y)` is a compile-time access
rule over field offsets, and two records that share field names may lay them
out at different offsets. A cross-type field view would therefore need either a
runtime offset map or a guarantee that layouts agree, which is exactly the
machinery records.md avoided.

A **verb slice exposes dispatch**, and dispatch is already dynamic. A send
resolves a selector against the receiver's own class at run time, and a
selector id is global to the program. So a verb slice needs no runtime
representation whatsoever: the object is passed exactly as it is, and the only
question the compiler answers is which selectors the holder is permitted to
send. Nothing about layout is revealed, nothing needs to agree, and the check
is a set-containment test over verb signatures.

That asymmetry is the whole design. Structural typing is cheap on the verb axis
and expensive on the field axis, so the language takes it where it is cheap.

## The form

A slice is written with the same word a record slice uses:

    func unlockAll(d is obj with (open, close))
        d.open()
        d.close()
        // d.smash() here is a compile error: not in the slice
    endfunc

and, because an interface wants a name, it may have one:

    type
        Openable is obj with (open, close)

    func describe(d is Openable) returns str
        return d.open() then "opened" else "stuck"
    endfunc

Both forms mean the same thing. The named form is the one a world model
reaches for, since a room holding `list of Openable` is the point.

The two words divide cleanly and the vocabulary is deliberate. A **slice** is
the form, `obj with (...)`, anonymous and usable inline, named by analogy with
the record field slice it parallels. An **interface** is a named slice, and
naming one declares a contract that other classes satisfy. That is the word
the interface-and-module languages use (Java's `interface`, Modula-2 and
Oberon's definition modules, Object Pascal's interface section), and it is the
right one here because the declaration is not a type alias. C's `typedef`
would be the wrong borrowing: an alias renames a type that already exists,
while this names a contract nothing has yet claimed to meet.

## Conformance is by signature, and nobody declares it

A class conforms to `obj with (open, close)` when it has verbs named `open` and
`close` **with matching signatures**: parameter count, parameter types, and
return type. Name alone would not do, since a send passes arguments and a
mismatch would be a type error discovered at the worst possible moment.

Conformance is never declared. A class does not name the interfaces it
satisfies, and an interface may be introduced long after the classes that
happen to satisfy it. That matters for a live world: a builder who writes a
`Cupboard` class today should not have to revisit it when someone later defines
`Openable`, and the person defining `Openable` should not need to edit every
class in the world. This is Go's stance, and it is the one that suits content
written by many hands over a long time.

The cost is accidental conformance: a class with an `open` verb that means
something unrelated will satisfy `Openable`. That is the acknowledged price of
structural typing, and the mitigation is the same as Go's, a slice with a
useful set of verbs rather than a single generic one. The language does not add
a declaration to prevent it, because the declaration would cost exactly the
after-the-fact flexibility the feature exists for.

## A slice is a type, and at rest it is just a handle

A verb slice is a type, usable wherever a type is: a parameter, a field, a
list element, a return type. Its runtime representation is the object handle
itself, unchanged, so a slice costs nothing to store, pass, or return. There is
no fat pointer, no vtable, no wrapper object. The whole feature is a
compile-time restriction over a value that was already being passed.

This is what makes it different from the record slice's D5 lifetime rule.
A record slice is call-scoped because it is a view into somebody else's
storage. A verb slice owns nothing and views nothing; it is a handle with a
narrower type, so it stores and escapes as freely as the `obj` it came from.

A slice may be narrowed further. `d with (open)` where `d` is `Openable` is a
subset of a subset, checked the same way.

## The holder may send only what the slice names

Inside a routine holding `d is obj with (open, close)`, sending `d.smash()` is
a compile error naming the slice, even when the runtime object would understand
it. That is the restriction the slice buys, and it is what makes a slice a
**capability**: an attenuated reference that carries exactly the authority it
names.

For a multi-user world this is the interesting half. Handing a builder's script
a `Chest` hands it every verb the chest has, including whatever the next
release adds. Handing it `chest with (open, close)` hands over two verbs and
nothing else, checked before the script runs. This is the object-capability
"facet" pattern, and it composes with the `discloses` audit block (core.md),
which reports the dangerous powers a script actually uses: `discloses` tells
you what a script asked for, a slice bounds what it could have asked for.

## Narrowing an untyped `obj` is fallible

A statically-typed source narrows for free: a `Chest`-typed value assigned to
an `Openable` is checked at compile time, since the class is known. An untyped
`obj` is not known, so narrowing it is a **fallible cast**:

    var d is maybe Openable = thing as Openable

which fails when the runtime object does not respond to the named verbs, and is
consumed by the ordinary consumers (fallible.md: `otherwise`, `if var`, a
`maybe` capture). The runtime test is a selector-set lookup against the
receiver's class chain, which is the walk `__exc_send` already performs.

This keeps the escape hatch honest. `obj` remains the dynamic door, and coming
back through it is a checked operation rather than an assertion.

## This revises verbs.md D5

verbs.md deferred an interface type and leaned toward "a named verb-set with
CP-style explicit conformance, verified single-pass at its end". This pass
proposes the structural form instead, for the reasons above: an interface that
must be declared by the class cannot be introduced after the fact, which is the
common case in a world whose content outlives its type definitions. The rest of
D5 stands unchanged, including that `obj` remains the dynamic escape and that
class-typed sends are checked as they are today.

## What is deferred

- **A runtime `is Openable` predicate** as a bool test, distinct from the
  fallible cast. The cast covers the need; a bare predicate can follow if a use
  appears.
- **Field requirements in a slice.** A slice names verbs only. Fields are
  already private to the object (cross-object field access is not a thing), so
  a field requirement would mean something new, not a subset of something
  existing.
- **Variance and generic slices** (a slice over a verb whose parameter is
  itself a slice), which needs the type-parameter work.
- **Slices in the `.exi`** as the cross-module handle type, which is what
  verbs.md D5 wanted them for; it waits on `.exi` files actually flowing
  between modules.

## Survey

- **Go interfaces**: structural, satisfied without declaration, method sets
  checked by signature. The direct model, including the accepted cost of
  accidental conformance.
- **E and object-capability languages**: a *facet* is a narrowed reference
  handing out part of an object's authority. This is where the security reading
  comes from, and it is the reason to prefer attenuation over trust.
- **Java / C# interfaces**: nominal, declared by the implementing class. What
  verbs.md D5 leaned toward and this note argues against, because declaration
  must happen before the fact.
- **TypeScript structural types**: structural conformance over a whole object
  shape, fields included. Excelsior takes the verb half only, since fields are
  private and layout is the thing the record slice deliberately kept nominal.
- **OCaml object types**: structural, row-polymorphic, inferred. The same
  stance with heavier machinery than a game-scripting language wants.
- **Ruby, and MOO itself**: duck typing with no static check at all. The
  baseline this improves on: the same flexibility, but the mistake is reported
  at compile time.
- **Compact Pascal**: explicit interface conformance, the lineage verbs.md D5
  cited. Its checking is single-pass and declaration-first, which suits a
  systems language and not a world edited by many authors.

Excelsior's stance: an object slice `o with (open, close)` is a structural,
signature-checked verb subset, usable as a type anywhere, represented at run
time by the plain object handle, restricting its holder to the verbs it names.
It is the interface the language deferred, and it doubles as an attenuated
capability.

## Decisions (confirmed)

The seven decisions are confirmed. The implementation is a checker feature with
no runtime part: the reader accepts `obj with (...)` as a type and a named
`Name is obj with (...)` declaration, the checker records the verb set and tests
a class against it by signature, a send through a slice is checked against that
set, and the fallible `as` cast from `obj` reuses the selector walk the send
already performs. Nothing is emitted for a slice itself, since its value is the
object handle.

Records are unaffected. Behavior on a record stays UFCS on plain data
(records.md D4), Go-style receiver methods rather than verbs, so this note adds
nothing to the record pole and the record slice keeps its nominal, same-type
rule (record-slicing.md D2).

**D1. An object slice is `o with (open, close)`, a verb subset**, the object
counterpart of record-slicing.md's field subset and spelled with the same word.
The compiler knows whether the base is a record or an object, so the two
readings never collide.

**D2. An object slice is structural and cross-type**, where a record slice is
nominal and same-type. A record slice exposes layout, which cannot be shared
across types without a runtime offset map; a verb slice exposes dispatch, which
is already dynamic and selector-keyed, so it needs no runtime representation at
all. Structural typing is taken on the axis where it is free.

**D3. Conformance is by full signature and is never declared.** A class
satisfies a slice when it has verbs of those names with matching parameter and
return types. No class names the interfaces it satisfies, so an interface may
be defined after the classes that satisfy it. Accidental conformance is the
accepted cost, as in Go.

**D4. A slice is a type, inline (`obj with (open, close)`) or named (`Openable
is obj with (open, close)`), usable in a parameter, field, list, or return.**
At rest it is the plain object handle, so it costs nothing to store or pass, and
unlike a record slice it is not call-scoped, owning and viewing nothing. A slice
may be narrowed to a smaller slice.

**D5. The holder may send only the verbs the slice names**, and any other send
is a compile error naming the slice, even when the runtime object would
understand it. A slice is therefore an attenuated reference, the capability
"facet" pattern, and the bound complements what `discloses` audits.

**D6. Narrowing an untyped `obj` is a fallible cast** (`thing as Openable`),
failing when the object does not respond to the named verbs and consumed by the
ordinary fallible consumers. A statically-typed source narrows at compile time
with no runtime test. The runtime test reuses the selector walk `__exc_send`
already does.

**D7. This revises verbs.md D5** from explicit, CP-style conformance to
structural conformance, on the ground that a declared interface cannot be
introduced after the classes it describes, which is the common case in a world
whose content outlives its type definitions. The rest of D5 stands.

## Implementation notes

**D3's signature question had an answer already in the runtime.** The note says
conformance is by full signature, but `with (open, close)` names only verbs, so
where does a signature come from? From the selector. A selector is interned by
name and dispatched by that id, one id per name across the whole program, so a
verb name already determines one signature at run time. The checker therefore
looks the declaration up program-wide (`verb_by_selector`) and checks and types
a slice send exactly like a class-typed one. No signatures in the slice, no new
syntax, and the static story matches what dispatch already assumes.

That also made slices worth having rather than a typed-looking hole. Before
this, a send on a dynamic receiver typed `any`, which the next line quietly
mishandled:

    return d.open() + " / " + d.close()   -- with `d is obj`

The first `+` concatenated, the second lowered to an **integer add of two
string pointers**, because `any + str` picked the arithmetic path. A slice send
is typed, so this reads as a concat. The same bug is still reachable through a
plain `obj` receiver and wants its own fix; it is a silent miscompile rather
than an error, which is the worse kind.

**D6 is deferred.** Narrowing an untyped `obj` (`thing as Openable`) needs a
runtime selector-set test and a fallible result, so it is a host helper plus
lowering rather than a checker rule. Everything else is checker-only: a slice
has no runtime representation, so a slice-typed local, parameter, or return
lowers exactly as `obj` does. A slice-typed **field** does not lower yet, for
the same reason an `obj` field does not (`field_is_scalar` rejects both), which
is the object-graph gap already on the pending list.

**What the tests pin**: two unrelated classes satisfying one slice without
either mentioning it, a slice narrowing to a smaller slice, the D5 restriction
on an unnamed verb, and a class that satisfies only part of a slice being
rejected where it crosses into the parameter.
