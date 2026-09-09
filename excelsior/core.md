# Excelsior: Core Language Design

The design of the Excelsior language core: how code is represented, how
it evaluates, how it is extended, and how the readable surface maps onto
it. This is the authoritative language spec. The companion
`../doc/game-scripting.md` is the game and runtime brief (distribution,
sandbox, concurrency runtime, permissions, client architecture); it holds
the rationale for decisions the language inherits, and this document
copies the language-relevant parts so it stands on its own.

Status: converged design, core implemented (2026-07): the reader,
resolver, checker, and ColdFire lowering cover the integer/bool/decimal/
str/list core, the object model with one-dot sends, match, the
quotation-boundary literals, and fallibility (see the design-note index
below and CLAUDE.md for the current inventory). Sections marked
**REVISIT** are pending the AppleScript / NewtonScript / Inform study.

## What Excelsior is

A statically typed, class-based in-world scripting language for
non-technical builders in a cooperative graphical MMORPG. It is a fork
informed by LambdaMOO, not a MooScript revision, and compiles through
skjegg's shared IR to ColdFire VM binaries. Three builder personas share
one substrate and differ only in authoring surface: map/object layers,
story/dialog writers, and mechanics experts. See the brief for the game
context.

**Status (target-server review, 2026-07-10).** Excelsior is the candidate
design for a *deferred* in-world authoring layer, not the near-term runtime.
The target server's near-term rules are portable C; a scripting VM and
authoring language come later (see the brief's "Review reconciliation").
Excelsior is favored among the candidates because it is static and compiled,
which the dynamic options (Lua, JavaScript) are not: unit testing,
interactive debugging, coverage, and compile-time error catching are hard
requirements, not preferences. It already compiles to ColdFire (it forks
MooScript), so IR to ColdFire is the working path; a parallel WASM backend
(skjegg work, not Excelsior's) would let a ColdFire server and a WASM client
run the same source, sidestepping any nested-VM cost. The object model and
concurrency below apply to the **scripted-logic layer** (entities that carry
behavior), not to plain world objects, which are data under native system
code.

Source files, Ada-style in role: `.exs` body, `.exi` interface. Unlike Ada,
the `.exi` is **compiler-generated**, not hand-written: a derived,
content-addressed contract artifact holding the public signatures plus a
generated **`discloses`** block (the dangerous powers a script uses and the
cross-object messages it sends, merging what were separate authored
permissions and a touches manifest). Because it is generated, an interface
change surfaces as a diff to the `.exi` hash, which is exactly the signal the
extend-yes/mutate-no authorship layer and lazy migration pin against. A
builder never maintains it; an optional in-`.exs` assertion (`deny network`)
is the only place a builder states a self-imposed cap, checked by the
compiler. See "The .exs / .exi split" below for a worked pair.

## Design lineage

Excelsior puts a **readable Algol-family surface** (BASIC/Pascal/JavaScript
familiarity, block terminators) over a **uniform, manipulable core**, so
the language is extensible the way Scheme, Logo, Forth, and Io are without
exposing that machinery to ordinary builders.

- **Algol / Pascal / BASIC / JavaScript:** the surface a non-programmer
  reads. Block structure with word terminators, infix expressions.
- **Io:** operators and keywords are sugar that desugars to a uniform core
  of message/call forms; every expression is one representation. Io's
  Message object is a first-class, inspectable code tree. We take the
  uniform core and leave Io's prototype object model (we chose classes).
- **Scheme:** homoiconicity, code and data share one form; extension by
  compile-time macros rather than Io's runtime message rewriting.
- **Logo:** low-punctuation nested lists (`[a b c]`, no commas) as the data
  literal, which is also the data face of the core.
- **Forth:** a definition keyword opens a delimited compile region (the
  `:` ... `;` colon definition); the words inside are compiled, except
  IMMEDIATE words, which run at compile time and shape the compilation.
  IMMEDIATE words are Forth's macro system; our macros are the same
  mechanism. Definitions are built on a compile-time stack.
- **StrongForth:** static typing over a concatenative core via stack-effect
  signatures, proof that the type discipline is independent of the grammar.
  We use a stack-effect check with source annotations for the meta layer.
- **Inform 7 / AppleScript:** readable-for-the-layperson precedents, and
  the reason the language is case-insensitive and uses the word `is` for
  type ascription. (Deeper lessons pending the study, see REVISIT.)

Added during the 2026-07 design series (each with its note):

- **Icon:** success/failure as the evaluation model, imported without
  the generators or backtracking. A fallible expression produces a
  value or fails, failure propagates to the nearest consumer, and
  `if var i = find("or", line)` is Icon's success-testing `if` in
  Excelsior keywords (fallible.md, else-operator.md).
- **Pony:** the actor-language precedent for one-dot calls with the
  sync/async distinction at the declaration (`be` versus `fun`), which
  is exactly the verb/func split; sends read as calls, and the
  declaration carries the semantics (verbs.md).
- **Compact Pascal / Go:** receiver methods on plain data and
  structural interfaces with explicit conformance, the input behind
  the deferred UFCS-for-records and interface-types directions; Go's
  lexer-level semicolon insertion is also the family our newline
  policy belongs to, published as CONT_TOKEN (verbs.md, sequences.md).
- **Rust / Zig / Erlang:** one match construct legal in statement and
  expression position, rather than C#'s two spellings of one idea;
  Erlang supplied the arm shape, Ruby's `when ... then` the arm word,
  and Ada/Pascal the constant labels and ranges (case-select.md).
- **BASIC:** `for i in 1 to 10`; the range word replaced `..` when the
  arrow family left the language (the de-arrow decision).
- **Python / JavaScript:** cautionary calibration. Python's walrus and
  JS's ASI are both dodged by construction: no assignment expression
  (binding forms instead), and join-by-default newline handling was
  evaluated and rejected (sequences.md).

## Design principles (distilled from the 2026-07 series)

The decisions of the design series compose into a small set of rules
that later work should preserve:

- **Symbols do math, words do structure.** `+ - * / %` and comparisons
  are the only operator symbols; `then`, `else`, `to`, `returns`,
  `maybe`, `can fail`, and the dot carry everything structural. `->`,
  `..`, `?:`, and the `:` send were all removed, each leaving a lexer
  migration hint. An arrow cannot be web-searched; words read aloud.
- **The quotation boundary** (sequences.md): commas belong to code,
  spaces belong to data, newlines belong to statements. A comma list
  always runs to a bracket or a keyword, never to a newline, and
  computation enters quoted contexts (strings, data literals) only
  through `${}` holes, whose constant cases fold to nothing at runtime.
- **Verbs are the boundary** (verbs.md): a verb is public, dispatched,
  contracted in the `.exi`, and capability-checked at the send; a func
  is a private helper. The dot asks; the declaration carries the
  distinction. Five server-design pillars each independently require
  this element, and nothing else crosses actors.
- **`else` means "otherwise", everywhere** (else-operator.md,
  case-select.md): the if arm, the match arm, the select default, and
  the fallback operator are one concept with one lowering spine (the
  fail/else label).
- **Failure is not a value** (fallible.md): fallible expressions
  propagate Icon-style to the nearest consumer (`else`, `if var`,
  `while var`, capture into `maybe T`); plain boundaries trap; `maybe`
  is the explicit, storable capture; `nothing` is not `nil`.
- **Exact or an error:** a decimal literal of up to four fraction
  digits is exact by construction (`decimal` is base-10, numbers.md);
  one with nonzero digits past the fourth is a compile error, and
  float is the named escape. (The earlier form of this principle, the
  `0f` literal and `fixed(num, den)`, dissolved with binary fixed.)
- **Errors teach.** A removed or mistaken form answers with the rule
  and the fix ("there is no `->`; a match arm uses `then`...", "a verb
  is a message others can send; it belongs under `public`"), because
  neophytes learn the boundaries from first contact, not from specs.
- **Generation is data, a turn-local source, or an actor** (the
  sources design in else-operator.md): never hidden call-site state,
  never suspended control flow across the freeze boundary.
- **Small words earn their place when they load the model faster**
  (union-types.md, and a refinement of "words do structure"). `of`,
  `is`, and `as` are cheap to read and let a declaration state its
  shape the way it reads aloud (`any of (int, str)`, `x is Point`,
  `n as decimal`). The aim is that a newcomer receives the high-level
  model quickly; once it is in their head, the precise vocabulary and
  syntax remove the ambiguities. Connecting words are welcome for that,
  symbols are not, because a word reads and searches and a symbol does
  neither.

The design-note series. Each note is authoritative for its own decisions,
rationale, and status (proposed / decided / implemented); this index is
one-line pointers, so read the note for detail. The marker on each line
mirrors the note's own status line as of 2026-07: (study) an exploration
or survey, (proposed) a drafted design awaiting decision, (decided)
decided but not yet implemented, (implemented) at
least a v1 in the tree, (superseded) kept for the rationale trail. The
note stays authoritative when the two drift. approachability.md is the
R1-R19 beginner/content-creator review the series answers, and the R-number on
a note below marks which finding it settles.

- `grammar.ebnf` — the surface grammar
- `host-abi.md` (decided) — the two-layer compiler/libexc/host contract: dispatch, powers, freeze/thaw, the binding surface; grounded in the three target hosts, boris provisional
- `verbs.md` (study) — the verb/func boundary, dispatch, and the dot
- `string-plan.md` (study) — string values, `${}` interpolation, the concat-chain lowering
- `sequences.md` (study) — the quotation boundary (commas code, spaces data, newlines statements)
- `fallible.md` (implemented) — Icon-style success/failure propagation and its consumers
- `else-operator.md` (superseded) — the origin of `A otherwise B` and the sources sketch
- `sources.md` (implemented) — the fallible pump as the iteration protocol: cursor records and `source of T` continuations behind one `next` face, `yield` produces, the continuation source second-class and turn-local
- `case-select.md` (superseded) — the value-chooser survey (largely superseded by choosers.md)
- `approachability.md` (study) — the R1-R19 beginner/content-creator design review
- `runtime-errors.md` (implemented) — faults, the trap UX, exit 70 (R1/R2/R8)
- `output.md` (implemented) — `tell` to players and the `///` trace channel (R10/R16; the author channel is partly superseded by debug-output.md)
- `numbers.md` (implemented) — base-10 `decimal` replacing binary fixed (R3/R17)
- `typed-data.md` (implemented) — `shape` schemas for symbolic data literals (R5)
- `parse-traps.md` (implemented) — teaching errors for the two documented misparses (R4)
- `nil-nothing.md` (implemented) — the two absence words, closing the ref soundness hole (R6)
- `text-encoding.md` (implemented) — UTF-8 storage, code-point character operations, a low-level `bytes` size accessor (R7)
- `string-repr.md` (decided) — the owned-vs-view string and the trailing-NUL invariant
- `can-fail.md` (implemented) — `can fail` as a valueless signal, not a bool (R14)
- `fallible-consumers.md` (implemented) — bool-only `if`/`while` and the `on fail` handler
- `fallback-words.md` (implemented) — `else`/`otherwise`/`on fail` sorted one word per role
- `slice-clamp.md` (decided) — point-versus-interval access (R9)
- `shared-params.md` (implemented) — the `shared` by-reference parameter mode
- `enums.md` (implemented) — first-class enums, qualified members, exhaustive match (R11)
- `choosers.md` (decided) — the three value-choosers, retiring `select` (R13)
- `tiers.md` (decided) — the three tiers World / Mechanics / Meta (R12)
- `visibility.md` (decided) — per-member visibility, retiring the section headers (R15)
- `debug-output.md` (decided) — `///` as the one author trace channel (R16)
- `boolean-ops.md` (decided) — retiring `xor`, bitwise as compiler intrinsics (R18)
- `quote.md` (decided) — `quote` leaves the base grammar for the meta layer (R19)
- `backlog.md` (living) — the queue of pending design passes
- `records.md` (implemented) — value-semantic records (the data pole)
- `data-model.md` (decided) — records / objects / `any`, retiring `prop`
- `mixed-lists.md` (decided) — `list of any` with boxed elements
- `meta.md` (implemented) — the macro and meta-layer surface
- `union-types.md` (decided) — closed `any of (...)` unions
- `record-introspection.md` (implemented) — `typeof` / `fieldtype` / `fieldsof` meta primitives
- `meta-values.md` (implemented) — what a macro holds and computes: atoms (number, text, bool) beside forms and descriptions, `typeof` over either, bounded arithmetic, no compile-time execution
- `set-of.md` (implemented) — `set of E` flag bitmasks
- `then-in-if.md` (implemented) — `then` / `do` close conditions and loop headers
- `match-when.md` (implemented) — `when` opens each match arm
- `defer.md` (implemented) — `defer` block-scoped teardown
- `buffer.md` (decided) — `buffer of T`, the mutable growable transient
- `type-parameters.md` (decided) — the word `of` replaces `<T>`
- `patterns.md` (decided) — typed-hole command templates, not raw regex
- `capabilities.md` (decided) — `capability` / `include` reusable behavior
- `inline-for.md` (decided) — `for` unrolls over a compile-time mixed sequence
- `list-ops.md` (implemented) — value-list composition, and `len` renamed `length`
- `string-literals.md` (decided) — the `{...}` brace-delimited string for prose
- `function-values.md` (decided) — `func` the primitive; named declaration and lambda are two surfaces over one macro-targetable core; comprehensions for map/filter; combinators (incl. `sort`) take func values
- `memory.md` (decided) — no tracing GC: a per-turn arena for transients, a refcounted acyclic heap for escaped values, explicit `spawn`/`destroy` for actors, a fault-aborted turn (rollback retired by the cost pass), a quota fault
- `typed-macros.md` (implemented) — the `_Generic` tier: a typed macro resolves at typecheck time, dispatches with `match typeof(x)`, splices types with `${T}`, monomorphizes per call; Meta-tier generics, no base-layer `<T>`
- `blob.md` (decided) — the v1 binary-data type: a mutable, refcounted, bounded byte view (memory-safe yet aliasing); typed access is a decode/encode macro over literal width/sign/endian keywords, the matrix contained in the type
- `equality.md` (implemented) — equality is `=`, inequality `<>` (retiring `==`/`!=`), reclaiming math's own symbol from binding per symbols-do-math; `=` is value equality or obj identity; binding keeps `=` (the statement eats it before expression parsing); `is` stays the type word
- `record-slicing.md` (implemented) — `p with (x, y)` slices a record to a field subset (same-type, compile-time, no runtime mask): in-place subset compare/assign, and a field-restricted by-reference parameter (a `shared` view with a field mask)
- `record-annotations.md` (implemented) — Go-style field tags: a field line is a comma list of entries, a `tags [...]` clause decorates the preceding field, each tag a symbol plus atoms; free-form, compile-time only, surfaced as `fieldsof`'s `.tags` for a serializer macro to walk
- `object-slices.md` (implemented) — `o with (open, close)` slices an object to a verb subset: structural (cross-type) where the record slice is nominal, since a verb slice exposes dispatch not layout; a type usable anywhere, represented by the plain handle, restricting its holder to the verbs it names (an attenuated capability)
- `interface-decl.md` (implemented) — `interface Name ... endinterface` with a body of verb signatures (the `.exi` form), replacing the bare-identifier declaration; the inline `obj with (...)` slice stays as the name-only parameter form
- `interface-support.md` (implemented) — `supports Name` as a class-body directive: optional (conformance stays structural), brings signatures and no members, lets a verb be written without its signature, and checks the class at its declaration

## Two layers that meet at expansion

Excelsior is two languages sharing one syntax of forms.

- **Runtime layer (applicative).** What builders write: the Algol/Io
  surface, `head(arg...)` calls and message sends, `if ... endif`,
  expressions. Compiles to ColdFire and runs.
- **Meta layer (concatenative).** A tiny Forth over compile-time builder
  stacks: the type-introduction primitive, the definition keywords, their
  `end<kind>` terminators, stack-effect checking. This is the extension
  seam, and it runs only at compile time. Access is restricted to
  runtime-library authors (the
  LPMud / MUD-OS tier), never ordinary builders.

They meet at macro expansion: a surface construct expands (meta layer) into
core forms (runtime layer), which are then type-checked and lowered.

## The form representation

After parsing and desugaring, every program is a tree of **forms**. A form
is one of:

- an **atom**: a literal (int, float, decimal, str, bool, nil) or a **symbol**
  (an identifier or operator name);
- a **compound**: `head(arg...)`, where `head` is a symbol and each `arg`
  is a form.

Written in `head(arg...)` call-notation to match the surface, isomorphic to
Scheme s-expressions and Io Message chains.

### Evaluation versus quoting

`head(arg...)` evaluates: it is a call or a message send. To hand the engine
a form as **data** instead, quoting is needed. Quoting is almost always
implicit, so the explicit form is a readable keyword, not a sigil:

- **Implicit (the common case).** A definition body between a member keyword
  (`verb`, `func`) and its `end<kind>` is auto-quoted (compiled, not run):
  the Forth colon-definition effect. A macro's arguments are auto-quoted by
  the macro. Builders rely on this and never write a quote.
- **Explicit:** `quote foo(a, b, c)` reifies the whole form; `quote(+)`
  reifies an operator's underlying symbol. Rare, mostly lib-author code.
- **Immediate holes:** inside a quoted template, `${expr}` splices a value
  at construction time (no capture needed). `quote tell(who, "hi ${name}")`.

Backtick is deliberately **not** used for quoting; it is reserved for
possible future quasiquote work. Multilevel defer is not a v1 feature.

## Core special forms (baked)

The irreducible set the compiler knows and lowers straight to IR. Kept
small so the meta layer carries everything else.

    seq(f...)                block; value is the last form
    let(sym, type, init)     local binding; type may be `infer`
    const(sym, type, init)   constant binding
    set(place, value)        assignment; place is sym | get(...) | index(...)
    if(cond, then, else)     conditional; else may be nil
    while(cond, body)        loop
    loop(sym, iter, body)    iterate sym over a range or list (surface `for`)
    case(subj, arm..., else) multiway; arm(matchforms, body) (surface `match`)
    send(recv, sel, arg...)  verb / message send   (surface `recv.verb(...)`)
    call(callee, arg...)     function call
    get(obj, name)           field / property read
    index(obj, key) / slice(obj, lo, hi)
    add/sub/mul/lt/and/...    operator intrinsics (Io's operator table)
    mktype(builder)          materialize a type   (the one type primitive)
    quote(form) / quasi(form, hole...)   code as data

Everything above `mktype` is a runtime-layer form. `mktype` is the single
bridge into the type system. `quote`/`quasi` produce the data face.

## Surface principles

- **Newline-terminated, auto-continuation.** A newline ends a statement; no
  visible terminator, and `;` is not used. A statement continues when a
  bracket or paren is open, when the line ends on a binary operator, or with
  an explicit trailing `\`. This is a Lua-like feel without Lua's fully
  terminator-free ambiguity, which our `[ ... ]` data literal (also the
  index operator) and `( ... )` would otherwise reintroduce.
- **Explicit blocks, per-kind terminators.** Every block opens with a
  distinct keyword (`class`, `record`, `verb`, `func`, `if`, `for`,
  `while`, `match`) and closes with a matching self-describing terminator
  (`endclass`, `endverb`, `endif`, ...), optionally name-echoed on the big
  ones (`endclass BarrowChest`). This is robustness first: the closer is
  explicit, so a body's end never depends on reserving the keywords used
  inside it (a word in a body is content, never a terminator), and a
  mismatch is a local error checked against the opener. Errors resync at the
  nearest terminator or section rather than sliding far down the file. It is
  also MooScript's existing convention, so the fork keeps it. Visibility and
  declaration groups (`public`, `private`, `use`, `const`, `type`) are flat
  sections, bounded by the next section or the enclosing `end<kind>`, no
  terminator of their own; single-line declarations (a field, an `enum`
  word list, which wraps via its open bracket) are newline-terminated
  with no closer.
- **Case-insensitive (ASCII fold).** Matches Inform 7 and AppleScript, the
  non-programmer authoring precedents, and forgives `Fireball` versus
  `fireball`. Types are PascalCase by convention, not enforced.
- **Reserve delimiters, not vocabulary.** The hard-reserved set (parser
  level) is tiny and readable: the sigils, the `end<kind>` block
  terminators, and the small words (`is`, `then`, `to`, `returns`).
  Control words (`if`, `endif`, `for`, `while`, ...) are
  a **standard prelude** of macros, universally present, so a builder treats
  them as keywords, which is correct and costs the compiler nothing.
- **Leading `_` is the user's, never the language's.** No keyword, prelude
  word, macro, or internal primitive ever starts with `_`. So a leading
  underscore always names a user identifier and can never collide with the
  language, now or in any future version. That makes it the collision
  escape hatch: a builder who wants an identifier named `quote` or `mktype`
  writes `_quote` or `_mktype`, and the error message on the bare-name
  collision can recommend exactly that. Internal primitives (`mktype`) are
  ordinary reserved meta-words, not sigil-marked; the `_` escape is what
  keeps them out of the builder's way. No `$ @` sigil soup anywhere.
  Stronger still: **no reserved name (keyword, prelude word, builtin, or
  intrinsic) contains an underscore anywhere**, not just at the front. A
  compound-concept name runs its words together (`bitand`, not `bit_and`;
  `setof` if a word were ever needed), which keeps the entire underscore
  character in the user's namespace and keeps the surface free of the
  compromise underscored builtins would be.
- **`is` for type ascription.** `x is int`, `contents is list<obj>`,
  `class Chest is Container`, `if opener is Goblin`. One word for "is a
  kind of," covering ascription, inheritance, and type-test; context
  disambiguates. `is` forces the spaces a bare `:` does not, and follows
  Inform 7. `returns` spells "produces" (return type; the `->` arrow was
  dropped: symbols do math, words do structure, and an arrow cannot be
  web-searched); `as` stays for the narrowing cast (coercion, per
  AppleScript/Rust/C#); the dot is both the verb send and the field read
  (the one-dot rule, see the verb-send note below), and `:` no longer
  appears in the expression language at all.
- **Word boolean operators, fixing Pascal's footguns.** `and`, `or`, `not`,
  `xor` are the boolean operators (readable for the audience, the Compact
  Pascal choice). Two corrections to classic Pascal: they have low
  precedence, below comparisons, so `hp > 0 and mana > 0` parses right with
  no parens (Pascal binds them like `*`/`+`, which mis-parses `a == b and c
  == d`); and `and`/`or` short-circuit by default, dropping Pascal's
  two-word `and then`/`or else`. Bitwise is not overloaded onto the words
  (Pascal's other footgun): it is mechanics-tier and rare, so it gets
  distinct spelling (`band`/`bor`/`bxor`/`shl`/`shr` or intrinsics).
- **`use` clause with aliasing.** `use world` brings in a module (names
  qualified: `world.Container`); `use items as it` binds a short per-file
  alias (`it.Sunstone`); `use combat.spells (Fireball)` selectively imports
  a name unqualified. Module paths are dotted logical names that resolve
  through the htree/build cache to content-addressed modules. Compact Pascal
  left `uses`/`unit` unimplemented, so there is no in-tree precedent; `use`
  is chosen over Pascal's `uses` because `uses` does not carry an `as` alias
  naturally.
- **One dot (re-settled 2026-07; supersedes the earlier `:`-sends rule;
  see verbs.md).** A verb send is `receiver.verb(...)` and a field read is
  `receiver.field`. The dispatch-versus-data-call distinction is real but
  lives at the declaration, not the call site: verbs and fields never
  collide (a class scope forbids two members with the same folded name),
  so the checker resolves a dot-call to a send when the name is a verb of
  the receiver's class, to a call-through-field when it is a field, and
  always to a send on an untyped `obj` (cross-object field access is not
  part of the actor model). The earlier rule's other leg, coexistence
  with the `num:den` ratio, vanished when that literal was retired
  (its successors `fixed()`/`0f` later dissolved into the base-10
  `decimal` type, numbers.md). Pony is the precedent: an actor
  language whose async
  behaviors are called with plain dot and distinguished at the
  declaration (`be` versus `fun`), exactly the verb/func split. A bare
  `name(...)` with no receiver calls a sibling: a func directly, a verb
  as a dispatched self-send (so overrides take effect). A later library
  macro may add an AppleScript-style `tell <recv> ... end tell` block to
  group several sends to one receiver. Relatedly re-settled: a verb is
  the boundary, so `verb` may appear only under `public` and `func` only
  under `private`; a private verb would still sit in the dispatch
  descriptor and be sendable, a capability leak the enforcement makes
  unrepresentable.

## Definitions and the meta-stack

There is no `class`/`enum`/`record` primitive. There is one baked `mktype`,
and the definition keywords are **macros** over it that push onto a
compile-time builder stack; each block's `end<kind>` terminator resolves its
builder. The terminators are per-kind (`endclass`, `endrecord`, `endverb`,
`endfunc`), which is what makes a mismatch a checked local error rather than
a silent slide. (`enum` is the exception: its members are a bracketed word
list, so the bracket closes it and it has no terminator; see Data literals.)

A record read as postfix operations on the builder stack:

    record Point            push builder{kind:record, name:Point, members:[]}
        x is float          field-push: members += field(x, float)
        y is float          field-push: members += field(y, float)
    endrecord               pop -> mktype: layout, register type, emit .exi sig

Nesting is stack depth. A class with a verb:

    class Chest is Container       push class-builder(Chest, parent=Container)
        public
            contents is list<obj>  field-push onto class-builder
            verb on_open(opener is obj)   push verb-builder; body auto-quoted
                ...
            endverb            mkverb -> attach as member of class-builder
    endclass                   mktype the class

`end<kind>` resolves the builder on top and checks its kind against the
opener. A compile-time `if` inside a def region (conditional field, platform
variant) is Forth's compile-time control-flow stack: the opener pushes a
patch site, the resolver closes it. That is **deferred**; it is where
complexity balloons.

**Checking and errors, StrongForth-style.** Each meta-word has a stack
effect over builder/type cells; the checker symbolically executes the def
region. A malformed definition (an `end<kind>` with no matching open
builder, a field after close) is a stack-effect violation, reported through
the `(file,
line)` span carried on the offending cell. The annotations are what buy
legible errors and are built in from the start, not bolted on. This is also
the stack the type checker reuses for the `_Generic`-tier selection below.

## The .exs / .exi split

The `.exs` body holds everything: `use` clauses, public and private fields,
field defaults, verb and function bodies, and private helpers. The `.exi` is
generated from it and holds only the public shape: public and `tunable`
field signatures (no default values), verb signatures (empty bodies), and
the generated `discloses` block. Private fields, all defaults, all bodies,
and private `func` helpers never cross into the `.exi`.

Visibility is by `public`/`private` section (Delphi-style): members and
fields under `public` are exported to the `.exi`, those under `private` are
not. A `verb` is an entry-point message; a `func` is a plain function; both
obey their section. `discloses` lists the dangerous powers used (`spawn`,
`persist`, ...) and the cross-object sends made (`opener:tell`, ...), all
computed by the compiler.

The body a builder writes:

    // barrow_chest.exs
    use
        world
        items as it

    type
        enum Element [fire frost poison arcane]

        class BarrowChest is Container
            private
                locked       is bool = true
                opened_count is int  = 0

                func reward(opener is obj)
                    var prize = spawn(it.Sunstone)
                    opener.receive(prize)
                    opener.grant_gold(self.gold_reward)
                endfunc
            public
                tunable gold_reward is int = 50

                verb on_open(opener is obj)
                    if self.locked
                        opener.tell("The barrow chest is locked fast.")
                        return
                    endif
                    self.opened_count = self.opened_count + 1
                    reward(opener)
                endverb

                verb unlock(key is obj) returns bool
                    if not (key is it.BarrowKey)
                        return false
                    endif
                    self.locked = false
                    return true
                endverb
        endclass

The interface the compiler generates (public section only, empty verb
bodies read as signatures, `discloses` computed):

    // barrow_chest.exi : GENERATED, do not edit
    use world

    type
        enum Element [fire frost poison arcane]

        class BarrowChest is Container
            public
                tunable gold_reward is int
                verb on_open(opener is obj)
                verb unlock(key is obj) returns bool
            discloses spawn, persist, opener:tell, opener:receive, opener:grant_gold
        endclass

## Where each construct lives

The test: **does it introduce a new type or change static layout?** If yes,
it touches `mktype` and the type environment; if it only rearranges
existing forms, it is a pure macro.

| Construct | Lives in | Why |
|---|---|---|
| Core special forms; surface sugar for `if/while/for/match` | Baked (parser) | 1:1 with a primitive; best error messages |
| `mktype` | Baked | the single type-introduction primitive |
| `class/enum/record/verb/func` and their `end<kind>` terminators (enum's word list needs none) | Prelude macros over `mktype` | universal, but still macros |
| `struct/tuple/bitfield`, `effect/dialog/after/when`, stat-tables, new operators | Lib macros | additive; no new type-checker vocabulary |

Extension power (defining new macros, editing the operator table) belongs to
runtime-library authors only. Story and map builders never see the
concatenative layer.

## Type system

Static, monomorphic (option A). Each verb resolves to one concrete
signature; local inference removes annotation noise where the type is
obvious (`var x = 5` resolves `int`); no runtime tagging of atomic values,
no per-call-site polymorphism.

Types: `int` and `float` are untagged machine values (`float` is hardware
IEEE 754 double). `decimal` is **base-10 fixed-point** (numbers.md):
value x 10^-4 stored in an int32, four fraction digits, range about
+/- 214,748.3647, with multiply and divide through a double-width
intermediate (the runtime's 64-bit helpers, round half away from zero).
The rule for builders: whole numbers are int, decimals are decimal,
float is asked for by name. A decimal literal is an untyped constant
that adapts to decimal or float context and defaults to decimal at
inference boundaries; any literal of up to four fraction digits is
exact by construction (`0.1 * 3 == 0.3`, exactly), and one with
nonzero digits past the fourth is a compile error (float is the
escape). It is drift-free for stat math because the arithmetic is exact
integer math with deterministic rounding, unlike float, and unlike its
binary predecessor it matches base-10 intuition. The lineage: the
parameterized Q-format (`fixed 16.16`), the `num:den` ratio literal,
the `0f` exact literal, the `fixed(num, den)` dyadic constructor, and
finally the binary representation and the `fixed` name itself were each
removed as the type converged on base 10; the lexer answers the retired
spellings with migration hints. `vec` and
`mat` are small vector/matrix value types so 3D data passes cleanly (heavy
geometry is a hypervisor call, not VM work). Plus `str`, `bool`, `obj`,
`err`, `list of T`, class names, and enum names.

The one dynamic point is **`any`**: a value whose runtime type is not known at
compile time. It is a compiler-level tagged union, not hand-written
boilerplate, preserving the useful part of LambdaMOO without whole-language
dynamic typing. (`any` was formerly the surface type `prop`; data-model.md
retired that name, folding it and the internal `ET_ANY` leniency into one
`any`, narrowed by `match`/`is`.)

**Generics, two tiers.** The `_Generic` tier (type-directed selection: a
typed macro inspects an argument's static type and emits the matching
monomorphic body) is cheap and fits option A; it covers `max`, `abs`, list
helpers. Full parametric polymorphism (option C, one source monomorphized
per type argument) is template instantiation plus per-type checking, the
"mountain," deferred and additive. No distribution of generic IR blobs (that
implies a per-snippet runtime linker, rejected). Typed macros are the
enabler for both, which is the eventual argument for having them; the first
implementation uses **expand-then-typecheck** (untyped rewrite, check the
emitted core forms), reserving typed macros for when the `_Generic` tier is
actually built.

## Object model (class-based, Strongtalk-flavored)

Scope (per D5, see Status): this models the **scripted-logic layer**,
entities that carry behavior. Plain world objects (a rock, terrain) are
native data under system code, not Excelsior class instances, and do not each
get a VM context. Full rationale in the brief; the language essentials:

- Class-based, single inheritance, statically typed, and fully live
  (Strongtalk: class browser, runtime subclassing, `copy` for
  clone-and-tweak, per-instance verb overrides for one-off scripts). Chosen
  over prototypes because static typing wants a fixed field layout and one
  `.exi`, and the actor model wants `self`-state local rather than reached
  through parent delegation.
- **Fields compile to the VM static segment.** Static layout means `self.x`
  is a fixed global address: no object header, no delegation lookup. The
  running VM context is the actor.
- **Copy-on-write class defaults.** An instance reads the class default
  until it writes, then gets its own slot; per-instance storage is the diff.
  Freezing only the delta-from-defaults gives small, dedup-friendly,
  content-addressed blobs.
- **Freeze/thaw.** A generated runtime walks the known layout to serialize an
  actor to JSON or compact binary, so an actor is live (running VM) or
  archived (serialized globals) interchangeably, and idle actors page out.
- **Migration is lazy and per-instance.** Each instance carries its
  shape-version; compatible changes (add field with default, rename, widen)
  apply automatically, a breaking retype needs a builder `migrate old to new`
  fragment. Schema-evolution in the Protobuf/Avro/Erlang `code_change` style.

## Data literals

Logo-flavored: bracketed, named-head, space-separated, no commas. The data
face of the core, so a dialog tree and the substrate a macro manipulates are
one form.

    [greeting
      [text "Hello traveler"]
      [choice "About the barrow" to barrow_info]
      [choice "Goodbye" to end]]

`[` at the start of a primary is a data literal; `[` after an expression is
the index/slice operator. Position disambiguates, and newline-termination
keeps a line-leading `[` from being read as an index of the previous line.

Separators follow the quotation-boundary rule (see sequences.md): commas
belong to code, spaces belong to data, newlines belong to statements.
Computation enters a quoted context only through a `${expr}` hole, the same
mechanism strings use, so `[10 20 ${limit}]` is a `list<int>` with one
computed element. Enums ride the same rule: an enum's members are atoms, so
`enum Color [red green blue]` is a word list, and the open bracket carries a
large enum across lines with no block terminator.

## Code-as-data spectrum

From cheap and safe to powerful and risky:

1. **Compile-time quoted data literals.** Dialog trees, stat tables, item
   defs known at compile time. Compile to static structures, no runtime
   eval, fully deterministic. Covers most builder value.
2. **Compile-time macro expansion (this document's meta layer).** New
   constructors and control forms expand into typed core forms at compile
   time, then lower to native code. Between items 1 and 3: extensible like
   Scheme, but no runtime interpreter.
3. **Quoted/deferred code fragments.** A condition or action stored on an
   object, compiled ahead of time and linked into the owning object's
   module, dispatched by the host when an event fires (NWN's model: a dialog
   node references its guard/action by hash). No general interpreter.
4. **Full runtime eval.** Construct and run new code at runtime. Most
   powerful, needs an interpreter in the VM, riskiest; gated or a non-goal
   at first.

A dialog is a static content-addressable tree whose dynamic parts are
compiled lambdas embedded by hash: a `when` guard is `fn(ctx) -> bool`, a
`${...}` hole is `fn(ctx) -> value`, a `do` action is `fn(ctx) -> void`. A
small generic walker interprets the static skeleton and calls these
fragments. Determinism is the callee's responsibility: RNG lives in the
called fragment and draws from the server's authoritative stream.

## Concurrency surface (macro over the core)

The single builder-facing concurrency construct is the **durative effect**,
a prelude macro that expands into a coroutine actor plus scheduling glue.
Every clause is optional (`on start`, `each turn`, `on <obj>.<event>`,
`on end`), the lifetime is bounded (`for N turns` / `while <cond>`), and
`stop` ends it early. `apply` starts one instance; `after <delay> do ...`
and `when <obj>.<event> do ...` are degenerate one-shots. Raw spawn/select/
channels stay runtime-internal, so builders cannot deadlock or starve a
turn. Full rationale, ordering (the timestamped priority queue and the
hash tiebreak), and the actor model are in the brief.

This composes with the native sim rather than driving it (per D4, see
Status): the fixed movement tick is the heartbeat and the source of movement
determinism, while the durative model's game-logic events ride the priority
queue, decoupled from the frame, possibly at finer (microsecond) resolution,
batch-dequeued every N frames, with late events tolerated as load. A "turn"
is a game-logic tick, not the 30 Hz frame.

## Permission model (reachability)

Authority is reference reachability (object-capability discipline in its
lightweight form): a frame can only message an object it holds a reference
to, references are unforgeable, there is no ambient authority. Per D5 (see
Status) this is enforced by how the logic-tier host exposes objects to script
code, not ridden free off a universal actor model (there is none); it is
scoped to the scripted-logic layer. Authorship-ownership (who may edit source) is a
separate edit-time layer in the htree, extend-yes / mutate-no. Coarse
declared permissions live in the `.exi`; the disclosure manifest is
compiler-generated and advisory. The fine-grained effect-metering system was
dropped as a debugging burden. Full rationale in the brief.

## Study findings: AppleScript, NewtonScript, Inform 7

Three deep studies reconciled the design against the closest prior art.
Most of it validates choices already made; the rest is a short list of
refinements, noted here as deltas to the sections above.

**Validated, no change**

- Structured Algol surface over full natural language. Inform 7's core
  failure is the "reads like English so it should work" trap: authors
  assume prose flexibility the parser lacks, then hit cryptic errors off the
  happy path. AppleScript's own designers reached the same verdict (readable
  to modify, hard to write). Explicit keywords are the right call.
- The durative effect over per-turn imperative rules. Inform 7 does ongoing
  behavior with Every-Turn rules plus manual state checks; the
  enter/each-turn/on-event/end lifecycle is the explicit construct they
  lacked.
- Class plus copy-on-write over NewtonScript's prototypes. A class with
  CoW defaults recovers NewtonScript's differential-inheritance memory win
  without the dual `_proto`/`_parent` chains (which existed only because its
  templates and views were indistinguishable frames). Template/view maps
  onto class/instance. Freeze/thaw plus lazy migration is the fix for the
  fragility NewtonScript hit with schema-less soups.
- Case-insensitivity. Both non-programmer precedents fold case.

**Refinements adopted**

- *Case rule (refines Surface principles):* case-insensitive match, but
  canonicalize each identifier's display to its first-seen spelling, as
  AppleScript does, so the source stays visually consistent.
- *Coercion (refines Type system):* no implicit lossy coercion. `as` is the
  only coercion and is explicit; a lossy `as` (dropping labels, a narrowing
  that can fail) is a compile warning or a checked runtime error, never
  silent. AppleScript's silent record-to-list loss is the anti-pattern.
- *Ambient self and explicit targeting (refines Object model):* a bare verb
  call runs on the ambient `self` (the object the event is about); any
  cross-object send needs an explicitly held receiver (`other.move()`, never
  a bare `move` that silently retargets). Where contexts nest (a session
  inside a verb), explicit qualifiers name the outer object, the lesson from
  AppleScript's `tell` nesting and `my`/`its`.
- *Explicit dispatch order (reinforces Concurrency surface):* never reorder
  handlers by implicit specificity the way Inform 7's rulebooks do (its
  worst debugging trap). Order is the timestamped priority queue, explicit
  and inspectable.
- *Standard world vocabulary (refines the prelude):* ship one canonical
  library of world-interaction verbs (`move`, `attack`, `play_sound`, ...);
  shards extend it, they do not each redefine it. AppleScript's per-app AETE
  dictionaries fragmented (no standard `move` versus `remove`); a shared
  prelude avoids that.
- *Static introspection index (new tooling):* the build emits a static index
  of what an object handles, in what order, and what each effect touches,
  merged with the compiler-generated disclosure manifest. Inform 7's authors
  most wanted visibility into rule order; the same static analysis gives it
  cheaply, feeding the class browser and the brief's trace channels.
- *Dialog split (for the dialog constructor spec):* keep the dialog
  constructor choice-based and non-programmer-facing; any utterance or
  command-grammar parsing (Inform 7's `Understand ... as ...`) is a separate
  mechanics-tier surface, not required to author a dialog.

## Execution and liveness

Static type checking is kept; it is a front-end property (it runs before
code generation) and is the source of the early-error detection and
reasoning that motivated a static language. Everything below preserves it.

**Substrate: ColdFire (decided).** The object model runs on the ColdFire VM,
not a Smalltalk-style object VM. The Strongtalk alignment is at the
object-model and UX layer (class browser, static-and-live, `copy`,
per-instance override), already mapped onto ColdFire. The distribution model
decides it: Excelsior's persistence is content-addressed, distributed,
per-object freeze/thaw over the htree and SHOAL, which fits ColdFire's
stateless-emulator-plus-serialized-globals shape and clashes with a
Smalltalk VM's monolithic image and pointer identity. Freeze/thaw and lazy
migration are needed regardless of VM (persistence is content-addressed, not
an image), so an object VM would fight them rather than provide them.
ColdFire also keeps multi-language targeting through the shared IR and hosts
the sandboxed compiler. The "ColdFire sandbox as a service" is the existing
hypervisor boundary: heavy or untrusted native kernels (geometry,
other-language content) are a hypercall to a sandboxed ColdFire service from
the object layer, a two-tier structure with one VM technology, not two.

**Single-pass compilation is a non-goal.** Compact Pascal (`skj-pc`) targeted
single-pass compilation; Excelsior deliberately does not. Macro expansion
over the meta-stack, type checking of expanded forms, the disclosure
manifest, the static introspection index, and building the hotswap dispatch
tables all want more than one pass over a unit. This costs nothing we care
about, because compilation is incremental and per-unit (per verb, per class)
into a live image, not a batch build whose speed a single pass would
optimize.

**Tiered execution.** In the live-edit loop: type-check, lower to IR, and
interpret the IR immediately, for instant feedback with no compile latency.
On the hot path: compile that same IR to native ColdFire in the background.
Both run the identical IR, so semantics and determinism match. The
interpreter runs typechecked, lowered IR, not arbitrary source, so it is not
the general `eval` of spectrum item 4; it is a second executor for the same
IR. This is HotSpot's interpret-then-compile, and it is how static checking
is kept without a batch-compile wall.

**Hotfix (decided), three tiers, gated by the authorship-ownership layer.**
Editing live behavior or shape is exactly the extend-yes / mutate-no
boundary: you hotfix your own classes; touching another's is an authorship
act.

- *Behavior swap.* Redefine a verb; new sends get new code, running
  coroutines finish on the old hash (drain plus by-path re-resolution at the
  turn barrier, already specced). Cheap because `send` dispatch is indirect,
  the Obj-C `objc_msgSend` / Java HotSwap model.
- *Shape migration.* Add, rename, or widen a field applies automatically; a
  breaking retype needs a builder `migrate old to new` fragment. Lazy and
  per-instance (already specced), the part standard Java HotSwap cannot do.
- *Live defaults, per-field `tunable` opt-in.* A field marked `tunable`
  keeps its default in a class cell resolved at read while still unwritten,
  so changing the default propagates to every non-overriding instance at
  once, recovering NewtonScript's live-default win inside the class model.
  Plain fields stay compile-time constants and read as a local global; a
  `tunable` field pays one indirection only until an instance writes it,
  after which it is local and fast. Opt-in, so only fields meant for live
  rebalancing (base stats, drop rates, tints) pay anything.

---

Made by a machine. PUBLIC DOMAIN (CC0-1.0)
