# Declaring an interface: a keyword, a body, a terminator

Status: decided (2026-07); implemented (D1-D7). The surface pass on the
interface declaration, raised 2026-07
after object-slices.md landed with the declaration spelled `Openable is obj
with (open, close)`. Tier: Mechanics (tiers.md); a builder uses an interface,
a systems author declares one. Owns the *declaration surface*; object-slices.md
owns the semantics (structural conformance, the send restriction, the
capability reading) and is unchanged by this.

The interface arrived with the slice that motivated it, and took the slice's
spelling: a bare name, `is`, and an inline slice. That makes it the only
declaration in the language with no keyword and no terminator, which costs more
than it saves.

Made by a machine. PUBLIC DOMAIN (CC0-1.0)

## What the current form costs

Every other type declaration opens with a word that says what is being declared
and closes with a matching terminator:

    class Chest is Container ... endclass
    record Point ... endrecord
    shape dialog ... endshape
    capability Openable ... endcapability        (capabilities.md D2)

    Openable is obj with (open, close)           the interface, today

Three costs follow from being the odd one out.

**The parser must guess.** A type section that meets a bare identifier has to
assume an interface declaration is starting, because nothing else in a type
section begins that way. A typo'd `recrod Point` therefore does not report a
misspelled `record`; it reports a broken interface declaration, blaming a form
the author was not writing. That is the shape of the parse traps the language
has spent passes eliminating (parse-traps.md).

**It reads as an assignment, not a declaration.** `Openable is obj with (open,
close)` has the same shape as `var d is Openable`, which is an ascription. The
one that introduces a new contract into the world and the one that types a
local should not look alike.

**It has nowhere to put signatures.** The verb names are all it can hold, so the
signatures come from elsewhere: object-slices.md's implementation resolves them
through the program-global selector rule (a selector is interned by name and
dispatched by that id, so a verb name determines one signature). That rule is
sound and stays, but it means an interface cannot *state* the contract it
represents. A reader of `Openable` learns two verb names and must go find a
class to learn what they take and return.

## The form

An interface is declared like the other type declarations, and its body is a
list of verb signatures:

    type
        interface Openable
            verb open() returns str
            verb close() returns str
        endinterface

That body is not a new grammar. `verb_sig` already exists, as the bodyless verb
form an `.exi` file carries, and an interface body is exactly a list of them.
The declaration therefore reuses the production the language already has for
"a verb's contract without its implementation", which is what an interface is.

`interface` is the word capabilities.md D1 already set aside for this: it chose
`capability` for the behavior-bundle axis and said in the same breath that "the
type axis is the separate word `interface`". So the vocabulary was decided
before the feature arrived, and this note is spending it rather than choosing
it.

## The inline slice stays, and stays name-only

The anonymous form does not change:

    func unlockAll(d is obj with (open, close))

This is the parameter-position form, it parallels the record field slice it was
named after, and it is the terse case the block form would make heavy. It names
verbs only and keeps resolving signatures through the selector rule.

So the two forms divide by what they are for, not by taste:

- **`obj with (...)`**, inline and anonymous, names verbs. Use it where the
  restriction is local to one routine's parameter.
- **`interface Name ... endinterface`**, named and declared, states signatures.
  Use it when the contract has a name worth writing down, which is when it is
  shared.

A block body does not accept bare verb names. If names are all that is wanted,
that is the inline slice, and the language does not grow a second spelling of
it (the rule choosers.md applied when it retired `select`).

## What signatures buy at the conformance check

With signatures declared, conformance checks them. A class satisfies `Openable`
when it has `open` and `close` **and their signatures match what the interface
states**, which is object-slices.md D3 with the missing half supplied: the
interface says what to match against, instead of the check leaning entirely on
the selector rule.

The selector rule is not retired. It stays as the guarantee that a name means
one signature program-wide, and it remains how an inline slice is checked. What
changes is that a named interface no longer has to depend on it, and a class
that drifts out of contract is told which interface it broke.

## What is deferred

- **A verb-less interface body** (an interface that requires a field, or
  another interface). Fields are private to their object, so requiring one
  means something new rather than a subset of something existing, which is
  object-slices.md's deferred item and stays deferred.
- **Interface composition** (`interface A includes B`), which wants the
  capabilities.md merge rules and should be settled with them, not here.
- **Declared conformance.** Nothing changes: conformance stays structural and
  undeclared (object-slices.md D3), so a class never names the interfaces it
  satisfies. This note only changes how the interface itself is written.

- **A class listing the interfaces it supports** (raised 2026-07, worth its own
  pass). Once an interface states signatures, a class implementing it repeats
  them: `verb open() returns str` appears in `Openable` and again in `Chest`,
  `Door`, and every other opener. A class could instead say which interfaces it
  supports and inherit the signatures from them, writing only the bodies.

  This is not the declared conformance the item above refuses, and the
  distinction is the whole design question. Declared conformance makes the
  declaration *required*, which is what forecloses introducing an interface
  after the classes that satisfy it. A supports-list would be **optional and
  labor-saving**: structural conformance stays the rule, an undeclared class
  still satisfies an interface it happens to match, and naming one only lets
  the class stop repeating what the interface already says. The pass has to
  settle what a listed verb's body looks like without its signature, whether a
  class may narrow or extend an inherited signature, and how a supports-list
  interacts with the `.exi`, where the signature must appear either way.

## Survey

- **Java, C#**: `interface Name { signatures }`, the direct model for a named,
  signature-carrying declaration; conformance there is declared, which
  Excelsior does not follow (object-slices.md D3).
- **Object Pascal**: `type IFoo = interface ... end`, the same shape inside a
  type section, and the lineage for the section this declaration sits in.
- **Modula-2 definition modules**: a module's contract written as signatures
  with no bodies, physically separate from the implementation. Excelsior's
  `.exi` is that idea already, which is why an interface body and an `.exi`
  verb list are the same production.
- **Go**: a named interface and an inline `interface{ ... }` coexist, exactly
  the two-form split proposed here, with the same division of labor.
- **Oberon**: no interfaces, type extension instead. The reminder that this is
  a choice rather than a necessity; Excelsior wants it for cross-class handles
  that name no class (verbs.md D5).
- **Ada package specifications**: the contract as a separate declaration the
  compiler checks the body against, the tradition the `.exs`/`.exi` split comes
  from.

Excelsior's stance: an interface is declared `interface Name ... endinterface`
with a body of verb signatures, matching every other type declaration and
reusing the `.exi` signature form; the anonymous `obj with (...)` slice remains
the inline, name-only form.

## Decisions (confirmed)

The seven decisions are confirmed. The implementation is a reader change plus
one checker rule: `interface` and `endinterface` join the keyword table, the
type section parses the block through the existing `verb_sig` production into
the N_INTERFACE node that already exists, the bare-identifier form retires with
a teaching error (which also removes the parse guess a type section currently
makes), and conformance compares a class's verb signatures against the declared
ones instead of resolving them through the selector rule. The `ET_SLICE` type
gains the signatures; the inline slice keeps carrying names only, so both feed
one conformance check with different amounts of information.

The migration is the one test that declares an interface today
(`exs_object_slice`), since the form is a day old.

**D1. An interface is declared `interface Name ... endinterface`**, with the
keyword-and-terminator shape every other type declaration uses (`class`,
`record`, `shape`, `capability`). This removes the only declaration that opens
with a bare identifier, and with it the parse guess that made a misspelled
`record` report a broken interface.

**D2. The body is a list of verb signatures**, reusing the existing `verb_sig`
production, which is the bodyless verb form an `.exi` already carries. An
interface body and an `.exi` verb list are the same thing, so interfaces flow
into the `.exi` unchanged.

**D3. The word is `interface`**, as capabilities.md D1 already reserved when it
took `capability` for the behavior-bundle axis and named `interface` for the
type axis. Not `typedef`, which aliases an existing type rather than declaring
a contract.

**D4. The inline `obj with (...)` slice stays, unchanged and name-only.** It is
the parameter-position form, it parallels the record field slice, and it keeps
resolving signatures through the program-global selector rule.

**D5. A block body does not accept bare verb names.** Names-only is the inline
slice; the language does not grow a second spelling of one thing (choosers.md's
rule).

**D6. Declared signatures are what conformance checks against**, supplying the
half object-slices.md D3 was resolving through the selector rule alone. The
selector rule stays as the program-wide guarantee and as the inline slice's
check, but a named interface now states its own contract and a drifting class
is told which interface it broke.

**D7. The bare-identifier declaration form retires**, with a teaching error: a
bare identifier at the head of a type declaration names the interface form it
was probably reaching for.

## Implementation notes

**An interface sym is a class sym with only verbs.** Its verbs get a member
scope, so `sym_member` finds them and `check_args` takes them unchanged, which
is what lets a declared signature be the thing conformance compares and the
thing a send through the interface is checked against. The `ET_SLICE` type
points back at the interface sym, so a named interface and an inline slice are
one thing downstream; the named one just carries more.

**One conformance check serves both forms.** It walks the slice's verb list and
compares signatures when the entry is a declared `verb` (an interface) and only
names when it is a bare name (an inline slice). So the selector rule and the
declared signature are not two code paths, they are the same path with more or
less information.

**The retired form's error names the keywords, not the interface.** D7 said a
bare identifier should point at the interface form, but the likelier cause is a
misspelled declaration keyword, and the message cannot tell which. It lists
`class`, `record`, `enum`, `shape`, `interface` and then shows the interface
form, so a typo'd `recrod` is told the truth instead of being blamed for a
broken interface. That was the parse guess this pass existed to remove, so the
message should not reintroduce it in prose.
