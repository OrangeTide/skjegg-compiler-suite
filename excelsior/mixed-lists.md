# Mixed lists: `list<any>` of boxed values

Status: decided (2026-07), not yet implemented. The third pass of the
data-model spine (backlog.md, TODO line 30), building on data-model.md's
`any`. Tier: Mechanics
(tiers.md); the tutorial uses homogeneous lists, and a mixed `list<any>` with
runtime type dispatch is a systems tool.

Note (2026-07): union-types.md D7 later revised this note's D1/D2/D5 to
the closed-union form (a mixed list's element type is a closed `any of
(...)`), leaving `obj` as the one intentionally open dynamic type. Read
those decisions with that revision in mind.

A homogeneous list already lowers (a `list<int>` is a constant global of raw
words, with copy-on-write mutators). A heterogeneous or symbolic literal types
`any` in the checker but does not lower, so a mixed list has a compile-time
type and no runtime form. This pass gives it one: a mixed list is `list<any>`
of boxed values, opt-in so homogeneous lists stay fast, narrowed by `match`
and `is`.

Made by a machine. PUBLIC DOMAIN (CC0-1.0)

## The design

### A mixed list is `list<any>` of boxed values

Each element of a `list<any>` is an `any`: a pointer to a boxed `{ tag,
payload }` (data-model.md). Because the box is pointer-sized, `list<any>`
**reuses the word-sized list machinery unchanged**, the element is a word like
any other, with no per-list tag sidecar; the tag rides inside each box. A
`list<int>`, `list<str>`, and every other homogeneous list stay untagged and
fast, their elements raw words, exactly as today.

### Mixed-ness is declared, never inferred

A homogeneous literal is a homogeneous `list<T>`, as now. A **heterogeneous
literal is a compile error unless its context type is `list<any>`** (or
`any`), which boxes each element at construction:

    var xs is list<int>  = [1 2 3]          // fast, untagged
    var ys is list<any>  = [1 "two" 3.0]    // boxed: int, str, float
    var zs = [1 "two"]                       // error: mixed, and no `any` asked for

So a builder never gets a slow, tagged list by accident. The tagged
representation is opt-in, requested by writing `any`, which is also where the
reader sees that the list is dynamic. This is TODO line 30's "mixed-ness is
declared", now spelled with `any` instead of the retired `prop`.

### Boxing at construction; constant boxes fold

Elements are boxed where the `list<any>` is built. A **constant** element
folds its box into the constant global (the same constant-list global a
homogeneous literal already emits, now holding boxed words); a **runtime**
element (a `${expr}` hole) boxes at construction, reusing the existing box
helper (the word-payload boxing already used for `maybe` capture, fallible.md).
The copy-on-write mutators (`append`, `set`) box the incoming element the same
way. So `[1 "two" 3.0]` is a constant global of three boxes, and `[${n}
"two"]` folds the string box and patches the `${n}` box at construction.

### `any` carries what a homogeneous list cannot

Because a box is a pointer to its payload, a `list<any>` can hold values a
homogeneous list cannot fit in a word: a **float** (8 bytes, boxed by
pointer), a **record** (multi-word, boxed), and a nested **list**. So `any` is
not only the heterogeneous escape, it is the way to put an oversized element
in a list at all; `list<float>` stays unsupported, but a `list<any>` of boxed
floats works. The self-describing box also suits freeze/thaw, whose
serialization needs exactly the tag the box carries (TODO line 30).

### Use is by narrowing: `match` and `is`

An `any` cannot be operated on until its concrete type is known, so using a
mixed list is narrowing each element:

    for x in mixed
        match x
            int     then total = total + x        // x is int in this arm
            str     then say(x)                    // x is str here
            decimal then money = money + x
            otherwise skip
        endmatch
    endfor

`match x` over **type-name labels** narrows `x` to that type in the arm, and
lowers to tag compares on the existing match machinery (the value-label match
already lowers to a compare chain or jump table; a type match compares the
box tag). This is the new piece: match arms may be type names over an `any`
subject, where today they are constant values over a typed subject. The
single-type test `is` already exists (`x is int` yields a bool) and is the
one-off form. The tag universe is closed and small: `int`, `bool`, `decimal`,
`float`, `str`, `list`, a record type, and an `obj` (class), plus
`nothing`/`nil`. A record or class arm matches the box's type descriptor, so
`match x / Point then ...` narrows to a concrete record.

Equality and membership follow the tag: `==` on two `any` values compares the
tag then the payload, and `x in xs` uses that compare.

### The implicit type-dispatch `for` is deferred

TODO line 30 also sketches an implicit form, a `for x in xs` over `list<any>`
whose body is synthesized once per reachable tag (a type-case with no explicit
`match`), bounded by the closed ~8-tag universe, limited to one nesting level,
naming the failing tag in diagnostics. That is a compile-time-expansion
convenience, and it rides the inline-`for`-over-heterogeneous-literals work
(backlog.md, TODO line 26), which is the same synthesize-a-body-per-type
machinery. The core here is the **explicit** `match` inside an ordinary `for`,
which needs no expansion analysis; the implicit form is a later, Meta-flavored
stage on top.

### Connection to symbolic data

`list<any>` is the runtime form that quoted and symbolic data (typed-data.md)
can lower to when it needs one: a symbolic literal `[choice "..." [a b]]`
types `any` today and does not lower, and a boxed nested `list<any>` is the
representation it takes on the day it must. This pass does not change
typed-data.md's checker-only shape validation or its host-passed stance; it
only records that `list<any>` is where a lowered symbolic value would land, so
the two are one representation, not two.

## Survey

- **Go `[]any`** (`[]interface{}`) with the type switch `switch v := x.(type)`:
  boxed interface values narrowed by a type switch. The direct model; `match`
  over type labels is Go's type switch, and `any` is Go's name.
- **Java `List<Object>` + `instanceof`, C# `List<object>` + `is` patterns**:
  boxed elements, runtime type test. The mainstream OO shape.
- **TypeScript `any[]` and discriminated unions**: narrowing by a tag check is
  the `match`/`is` discipline; TS is where `any` gets its name.
- **Lisp / Scheme lists**: naturally heterogeneous tagged values, the dynamic
  ancestor. Excelsior keeps the tagged value but makes the heterogeneity
  opt-in and the dispatch checked.
- **Rust `Vec<Box<dyn Any>>` or `Vec<Enum>`**: the explicit-boxing precedent,
  and the closed-tag enum alternative; Excelsior's tag universe is closed like
  the enum but the surface is one `any`.

Excelsior's stance: Go's `[]any` with type-switch narrowing, made opt-in so
homogeneous lists stay fast and untagged, boxed elements reusing the
word-sized list machinery, and `match`/`is` as the type switch.

## Decisions (confirmed)

The seven decisions are confirmed; the implementation (the `list<any>` box
representation, the declared-mixed-ness check, construction-time boxing,
type-name match labels lowering to box-tag compares) is the follow-up, and
depends on records (records.md) for the record payload.


**D1. A mixed list is `list<any>`, each element a boxed `{ tag, payload }`.**
The pointer-sized box reuses the word-sized list machinery with no per-list
tag sidecar; homogeneous lists stay untagged and fast.

**D2. Mixed-ness is declared, never inferred.** A heterogeneous literal is a
compile error unless its context type is `list<any>` (or `any`), which boxes
each element. The tagged representation is opt-in, asked for by writing `any`.

**D3. Boxing happens at construction; constant boxes fold.** Constant elements
fold their boxes into the constant global; runtime holes and copy-on-write
mutators box through the existing box helper (fallible.md).

**D4. `any` carries oversized and heterogeneous elements alike.** Because a
box is a pointer, a `list<any>` can hold a float, a record, or a nested list,
which a homogeneous word-element list cannot; the self-describing box also
serves freeze/thaw.

**D5. Use is by narrowing: `match` over type-name labels, and `is`.** A
`match x` whose arms are type names narrows `x` per arm and lowers to box-tag
compares on the match machinery; `is` is the single-type test. The tag
universe is closed (`int`/`bool`/`decimal`/`float`/`str`/`list`, a record
type, an `obj` class, `nothing`/`nil`); a record/class arm matches the box's
type descriptor. Equality and membership compare tag then payload.

**D6. The implicit type-dispatch `for` is deferred.** The `for x in xs` that
synthesizes a body per reachable tag rides the inline-`for` compile-time-
expansion work (TODO line 26); the core is an explicit `match` inside an
ordinary `for`.

**D7. `list<any>` is the runtime form of symbolic data.** A lowered
symbolic/quoted value (typed-data.md) lands as a boxed nested `list<any>`;
this pass does not change typed-data's checker-only stance, only records that
the two share one representation.
