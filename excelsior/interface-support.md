# A class naming the interfaces it supports

Status: decided (2026-07); implemented (D1-D6; D7 is the `.exi` generator, which
does not exist yet). The follow-on interface-decl.md
deferred, raised 2026-07 when
declaring signatures in an interface made every implementing class repeat them.
Tier: World to write `supports`, Mechanics to declare the interface (tiers.md).
Builds on interface-decl.md (the declaration and its signatures),
object-slices.md (structural conformance, which this does not change), and
capabilities.md (the other non-inheritance axis, and its collision rule).

An interface now states its verbs' signatures, so a class implementing one
writes them twice: `verb open() returns str` in `Openable`, and again in
`Chest`, `Door`, and every other opener. A class should be able to say which
interfaces it supports and take the signatures from them.

Made by a machine. PUBLIC DOMAIN (CC0-1.0)

## Two things this must not become

The obvious design is Java's `implements`, and two of its properties are wrong
here.

**It must not make conformance declared.** object-slices.md D3 settled that a
class never declares the interfaces it satisfies, because a declared interface
cannot be introduced after the classes that satisfy it, which is the common
case in a world whose content outlives its type definitions. That decision
stands. `supports` is **optional**: a class that names nothing still satisfies
every interface it structurally matches, and naming one adds a check rather
than creating conformance.

**It must not become a third inheritance axis.** The language already has two
ways for a class to acquire members: `is Parent` (inheritance) and `include
Capability` (reuse, capabilities.md D3). A third would need its own collision
story and would leave a reader asking which of three mechanisms a member came
from. `supports` acquires **no members**. It is the one axis that brings
signatures and nothing else.

That last point is the clean way to see the whole area, because interface and
capability are duals:

- A **capability** is implementation without a type: `include` splices bodies
  in, and a capability is explicitly not a type (capabilities.md D6).
- An **interface** is a type without implementation: `supports` takes
  signatures, and an interface has no bodies to give.

So `include` brings bodies, `supports` brings signatures, and `is` brings both.
Three words, three distinct things, no overlap to adjudicate.

## The form

`supports` is a class-body directive, like `include`:

    class Chest
        supports Openable

        private
            shut is bool = true
        public
            verb open
                self.shut = false
                return "it creaks open"
            endverb
            verb close
                self.shut = true
                return "it thuds shut"
            endverb
    endclass

Not a header clause. The header carries `is Parent` and stacking a second list
there makes the longest line in the class the least interesting one;
capabilities.md already set the precedent that a non-inheritance axis is a body
directive, and following it keeps the two parallel.

The verbs are written without their signatures, which is the point of the
exercise. A class may still write a signature in full, and it must then match.

## Where the parameter names come from

A verb with parameters, written body-only, still needs its parameters bound:

    interface Damageable
        verb hurt(amount is int, kind is str)
    endinterface

    class Orc
        supports Damageable
        public
            verb hurt                      -- amount and kind are in scope
                self.hp = self.hp - amount
            endverb
    endclass

The names come from the interface. That is a real consequence, and it is
already the language's position: named arguments (records.md) let a caller
write `hurt(amount: 3, kind: "fire")`, so a verb's parameter names are part of
its contract, not private to its body. Taking them from the interface makes
that explicit rather than adding a new commitment.

## Only when there is one answer

A signature may be omitted only when exactly **one** source declares that verb:
a supported interface, or the parent. If two supported interfaces declare
`open` with different signatures, the class must write the signature it means,
which is capabilities.md D4's rule (more-specific wins, peers error) applied to
signatures instead of members. Nothing new is invented for the collision case,
and the class always has an escape: write it out.

The parent is the more-specific source, since inheritance already gives the
class that verb, so a class supporting an interface its parent already
satisfies takes the parent's signature and the interface check simply passes.

## The check is the other half of the feature

Naming an interface is not only a labor saving. A class that says `supports
Openable` and does not satisfy it is a compile error **at the class**, naming
the verb that is missing or the signature that differs.

Without the declaration, that mismatch surfaces at some distant place where a
`Chest` is passed to an `Openable` parameter, which may be in another file and
may not exist yet. With it, the class states its intent and hears about drift
where the drift is. This is what makes `supports` worth writing even for a
class that spells its signatures out in full.

## The `.exi` is never abbreviated

An abbreviated verb raises a fair objection: a reader of the class cannot see
what `hurt` takes without opening `Damageable`. The answer is that the
abbreviation is an implementation-file convenience only. The `.exi`, the
generated contract artifact (core.md), carries every verb's full signature as
it always has, because it is the file that exists to be read without its
implementation.

That is the Modula-2 and Ada arrangement, definition apart from
implementation, with the halves assigned as those languages assign them: the
signature lives with the contract, the body lives with the class. What is new
is only that the contract may be an interface shared by several classes rather
than one module's own header.

## What is deferred

- **A capability declaring `supports`.** A capability is not a type
  (capabilities.md D6), but its included verbs could satisfy an interface on
  the includer's behalf. That wants the capability merge rules, not this note.
- **Interface composition** (`interface A includes B`), still deferred by
  interface-decl.md, and orthogonal: it changes what an interface holds, not
  how a class names one.
- **A supports-list on the inline slice.** The inline `obj with (...)` form
  names verbs at a use site and has no declaration to attach a list to.
- **Narrowing or widening an inherited signature** (a class implementing
  `hurt(amount is int)` as `hurt(amount is decimal)`). Variance is its own
  pass; for now an omitted signature is exactly the interface's and a written
  one must match it.

## Survey

- **Java, C# `implements`**: the model, and the source of both properties this
  rejects. Conformance there is created by the declaration, and the class still
  repeats every signature.
- **Go**: no declaration at all, so nothing to abbreviate and no check at the
  type. Excelsior keeps Go's conformance rule and adds the optional declaration
  Go's users ask for with the `var _ I = T{}` idiom.
- **Object Pascal**: a class lists its interfaces in the header and repeats the
  signatures; the header-versus-body-directive choice here goes the other way,
  following `include`.
- **Modula-2 definition modules, Ada package specs**: signature in the
  contract, body in the implementation, which is the split this note applies to
  interfaces rather than modules.
- **Rust `impl Trait for T`**: the bodies live in a block naming the trait, so
  the association is explicit and the signature is still written. Excelsior
  puts the association on the class rather than in a separate block, since a
  class body is already where its verbs live.
- **Eiffel**: conformance is declared and signature inheritance is real
  (a redefinition may omit what it does not change), the closest precedent for
  taking a signature from a named contract.

Excelsior's stance: `supports Name` is an optional class-body directive that
takes verb signatures from an interface and checks the class against it. It
creates no conformance, brings no members, and abbreviates only the
implementation file, never the `.exi`.

## Decisions (confirmed)

The seven decisions are confirmed. The implementation is a reader change and a
checker rule: `supports` joins the class-body directives beside `include`, a
verb may parse with no parameter list or return type, the checker fills an
omitted signature from the single source that declares it and reports the
peers case, and a class naming an interface is checked against it where it is
declared. Nothing reaches lowering: a signature taken from an interface is the
same signature, and the class emits exactly what it emits today.

**Where an error lands with an omitted signature**, since it is the one
consequence of D4 worth stating plainly. Change a verb's signature in an
interface, and a class that wrote the signature out gets a mismatch at its own
declaration (D3), naming the interface it broke; a class that omitted it
silently adopts the new signature, and the error moves to the *body*, wherever
the old parameter is used. Abbreviation does not lose the error, it relocates
it from the declaration to the code. For a rename that is the better place;
for a type change it reads less directly, which is the cost of the
convenience.

**D1. `supports Name` is a class-body directive**, parallel to `include`
(capabilities.md D3), not a header clause. The header keeps `is Parent` alone,
and the two non-inheritance axes read alike.

**D2. It is optional and creates no conformance.** Structural conformance is
unchanged (object-slices.md D3): an unlisted class still satisfies every
interface it matches, so an interface may still be introduced after the classes
that satisfy it. Naming one adds a check.

**D3. A class that names an interface it does not satisfy is a compile error at
the class**, naming the missing verb or the differing signature. This is the
half of the feature that pays even when the class writes its signatures out:
drift is reported where the drift is, not at a distant use site.

**D4. A verb may be written without its signature**, which the class takes from
the interface, parameter names included. Named arguments already make a verb's
parameter names part of its contract (records.md), so this commits to nothing
new.

**D5. Omission is legal only when exactly one source declares the verb**, a
supported interface or the parent. Two interfaces disagreeing is the peers-error
case of capabilities.md D4, and the class resolves it by writing the signature
it means. The parent is the more-specific source.

**D6. `supports` brings signatures and no members.** `is` brings both, `include`
brings bodies (capabilities.md D3), `supports` brings signatures. Interface and
capability are duals, a type without implementation and an implementation
without a type, so the three axes stay distinct and no merge rule spans them.

**D7. The `.exi` carries full signatures always.** Abbreviation is an
implementation-file convenience; the generated contract is never abbreviated,
which is the Modula-2 / Ada split with the signature on the contract side.

## Implementation notes

**Abbreviation is all or nothing**, and this note's own first example got it
wrong: it showed `verb open returns str`, a signature with the parentheses
dropped but the return kept. That is a third form, and D4 does not describe
one. A verb either states its whole signature or states none of it, so
`verb open` takes everything from the interface while `verb open()` still
means "takes nothing". The example is corrected.

**`supports` takes a comma list.** D1 writes `supports Name`, and the note's
title says "the interfaces it supports", so `supports Openable, Lockable` on
one line is accepted alongside repeated directives. Both read; refusing the
list would have been a distinction with no argument behind it.

**Filling runs between the parent link and body resolution.** An omitted
signature is filled in a pass after `link_parents` (so the parent can be the
source, per D5) and before bodies resolve (so a filled parameter is in scope
like any other). Because `collect_members` has already run by then, the fill
re-stamps the verb symbol's type as well as the node's.

**Two error sites, deliberately.** A missing source for an omitted signature
is a resolve-time error at the verb; a class that does not satisfy an
interface it names is a checker error at the `supports` line, or at the
offending verb when a written signature disagrees. The first is about the verb
lacking a signature, the second about the class lacking a contract, and they
read differently on purpose.

**D7 has nothing to implement yet.** The `.exi` generator does not exist, so
"the `.exi` is never abbreviated" is a constraint recorded for whoever writes
it rather than code. Every signature is on the verb node by then, filled, so
the generator has nothing special to do.
