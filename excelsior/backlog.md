# Backlog: pending Excelsior design passes

Status: living backlog (2026-07). The approachability review
(approachability.md, R1 to R19) is complete, and each finding has its design
note. This note collects the language-design questions that still need a pass,
mined from TODO.md, so they live in the design-note collection instead of a
flat checklist. Each entry names its origin, its relation to existing notes,
and its dependencies. An item becomes its own note when its pass runs; this
backlog is the queue, not the design.

Two things this backlog is deliberately not:

- **The implementation queue.** Several notes are decided but not yet
  implemented (slice-clamp, choosers, tiers, visibility, debug-output,
  boolean-ops, quote, function-values, type-parameters, memory, blob,
  string-literals, inline-for), plus string-repr's deferred
  owned/view half. Those are designed; they await code, not a pass. Each note's
  own `Status:` line is authoritative; the pointers below can lag it.
- **The approachability findings.** R1 to R19 are all settled and decided
  (R7, text-encoding.md, was the last confirmed); they await code, not a pass.

Made by a machine. PUBLIC DOMAIN (CC0-1.0)

## The data-model cluster (dependency spine)

The largest thread, and ordered: each pass depends on the one before it.

1. **Records.** A record type: plain data with named fields, value semantics,
   and behavior by UFCS (Compact Pascal / Go style). The foundation of the
   spine, since verbs.md's UFCS-for-records and several items below say "when
   records land". **records.md (decided, implemented)** settles this: value semantics, the
   `Point(...)` type-name constructor, `p.field` access with UFCS `p.norm()`,
   nominal value-equal typing, inline no-GC storage, and the record-versus-
   object (data-versus-actor) split; it activates verbs.md decision 4.

2. **Unify records, props, and objects** (TODO line 48). **data-model.md
   (decided)** settles this: there are two composite poles, records (value)
   and objects (reference actors), and no third kind, "props" was an
   overloaded word. The dynamic tagged value becomes `any` (the `prop` keyword
   retires, folding in the internal `ET_ANY` leniency), the storable-reference
   reservation on `prop` is withdrawn (a reference, if ever, is `ref`), and
   the dot is the one surface (field / verb on actors / UFCS func on values).
   Resolves the `prop` word-collision by retiring the word.

3. **Runtime mixed `list<any>`** (TODO line 30). **mixed-lists.md (decided)**
   settles this: each element is a boxed `{ tag, payload }` `any`, so
   `list<any>` reuses the word-sized list machinery (no per-list sidecar) while
   `list<int>` stays untagged and fast; mixed-ness is declared, never inferred
   (a heterogeneous literal errors unless the context is `list<any>`, which
   boxes at construction, constant boxes folding); `any` also carries oversized
   elements (float, record, nested list) a homogeneous list cannot; use is by
   narrowing, `match` over type-name labels (new: type labels over an `any`
   subject, lowering to box-tag compares) and `is`; the implicit
   type-dispatch `for` is deferred to the inline-`for` work; and `list<any>` is
   the runtime form symbolic data (typed-data.md) can lower to.

4. **Record introspection for macros** (TODO line 46). **record-introspection.md
   (decided, implemented as a slice)** settles this, the spine's final pass: four bare-name
   compile-time meta primitives a procedural macro calls, `typeof(expr)` (the
   value-to-type bridge), `hasfield(T, "name")` (presence), `fieldtype(T,
   "name")` (the field's type; the "field of type T" predicate is
   `fieldtype(...) is int`), and `fieldsof(T)` (the fields in declaration order
   as `.name`/`.type` descriptors); names run their words together with no
   sigil and no underscore (Zig's `@` and D's `__traits` ruled out; `fieldsof`
   per the `setof` precedent); field iteration is a bounded static-foreach
   sharing the inline-`for` machinery; settled over records with the pattern
   extending to classes (`verbsof`), enums (`membersof`), and shapes; Meta-tier,
   compile-time zero-runtime (the Zig/D/Nim stance, not Go `reflect`). This
   closes the data-model spine.

- **Record slicing** (raised 2026-07). **record-slicing.md (decided,
  implemented)** settles
  this: `p with (x, y)` names a record restricted to a field subset (`with` is
  Pascal's record-field word; `[...]` is not reused, being positional).
  Same-type only (records stay nominal). The subset is a **compile-time access
  rule with no runtime form**: in place, the compiler emits the concrete field
  operations, so `p with (x, y) = source` is a subset assignment that leaves the
  rest untouched and `a with (x, y) = b with (x, y)` compares only those fields.
  It is first-class only as a **field-restricted by-reference parameter** (`p is
  Point with (x, y)`, a `shared` view whose body may access only the named
  fields, checked at compile time, no copy), call-scoped and non-escaping like
  `shared`. Deferred: cross-type structural slices (the interface/row-poly work),
  a stored/returned slice (needs the memory model), and nested-field slices.
  Tier: Mechanics.

- **Object slices as interfaces** (raised 2026-07, from the record/object layout
  unification). **object-slices.md (decided, implemented except D6)** settles this: `o with (open,
  close)` is the object counterpart of the record field slice, a **verb**
  subset, and it turns out to be the interface type verbs.md D5 deferred.
  Unlike the record slice it is **structural and cross-type**, and the note
  argues that is not a change of taste but of exposure: a record slice exposes
  layout (offsets, which cannot be shared across types without a runtime map),
  while a verb slice exposes dispatch, which is already dynamic and
  selector-keyed, so it needs no runtime representation at all. Conformance is
  by full signature and is never declared, so an interface may be introduced
  after the classes that satisfy it (Go's stance, accidental conformance the
  accepted cost). A slice is a type anywhere (parameter, field, list, return),
  represented at rest by the plain object handle, and is not call-scoped, since
  it owns and views nothing. Its holder may send only the named verbs, which
  makes it an **attenuated capability** (the object-capability facet pattern,
  complementing what `discloses` audits). Narrowing an untyped `obj` is a
  fallible cast. Revises verbs.md D5 from explicit CP-style conformance to
  structural. Deferred: a bool `is Openable` predicate, field requirements,
  variance/generic slices, and slices as the cross-module `.exi` handle type.
  Tier: Mechanics to write, World to use.

- **The interface declaration syntax** (raised 2026-07, after object-slices
  landed). **interface-decl.md (decided, implemented)** settles this: an interface is
  declared `interface Name ... endinterface` with a body of verb signatures,
  matching every other type declaration (`class`, `record`, `shape`,
  `capability`) instead of being the one that opens with a bare identifier and
  has no terminator. The body reuses the existing `verb_sig` production, the
  bodyless verb form an `.exi` already carries, so an interface body and an
  `.exi` verb list are the same thing and interfaces flow into the `.exi`
  unchanged. The word is the one capabilities.md D1 already reserved when it
  took `capability` for the behavior axis. The inline `obj with (...)` slice is
  unchanged and stays name-only, so the two forms divide by purpose: inline for
  a parameter restriction, named when the contract is shared. Declared
  signatures become what conformance checks against, supplying the half
  object-slices.md D3 was resolving through the program-global selector rule
  alone (the rule stays, as the program-wide guarantee and the inline slice's
  check). Deferred: verb-less bodies (field requirements), interface
  composition (wants the capabilities.md merge rules), and any change to
  structural undeclared conformance, which is unaffected. Tier: Mechanics.

- **A class listing the interfaces it supports** (raised 2026-07).
  **interface-support.md (decided, implemented)** settles this: `supports Name` is a
  class-body directive, parallel to `include` (capabilities.md D3) rather than
  a header clause, so the header keeps `is Parent` alone and the two
  non-inheritance axes read alike. It is **optional and creates no
  conformance**, which is what separates it from Java's `implements` and keeps
  object-slices.md D3 intact: an unlisted class still satisfies every interface
  it matches, so an interface may still be introduced after the classes that
  satisfy it. A verb may then be written with no signature, taken from the
  interface with its parameter names (named arguments already made those
  contractual, records.md), but only when exactly one source declares it, two
  interfaces disagreeing being capabilities.md D4's peers-error case. The other
  half of the feature is the check: a class naming an interface it does not
  satisfy is an error **at the class**, so drift is reported where it happens
  rather than at some distant use. The three axes stay distinct because
  interface and capability are duals, a type without implementation and an
  implementation without a type: `is` brings both, `include` brings bodies,
  `supports` brings signatures and no members. The `.exi` is never abbreviated
  (the Modula-2 / Ada split, signature on the contract side). Deferred: a
  capability declaring `supports`, interface composition, and variance in an
  inherited signature. Tier: World to write, Mechanics to declare.

## Types: a `set of` flags type

- **A `set of` type for flags.** **set-of.md (decided; single-word core
  implemented)** settles this: `set of
  E` is a compile-time-sized bitmask value type over a small universe (an enum,
  the main case, or `char`), value-semantic like a record (inline, no GC,
  copied, crossing a send by copy), the universe capped at 256 members / 32
  bytes. It is `set of Enum` (reusing the enum member list, the one-of enum
  paired with the many-of set), not a parallel `flags` declaration; a flag type
  is normally named (`EffectsFlags is set of Element`); members are bare
  singleton sets in set-pinned contexts (activating enums.md's contextual bare
  names). Value-building is symbols (`+`/`+=` union, `-`/`-=` difference,
  `^`/`^=` symmetric-difference/toggle, no constructor word, `empty`/`full`
  identity literals), and the two boolean tests are words: `in` (all-present,
  subset) and `overlaps` (any-present, non-empty intersection, settling the
  earlier open any-present question). A set is a `match` subject by
  set-constant equality. Tier World to use, Mechanics to declare; enum-
  dependent, so it follows enums in implementation.

## Collections: list operations

- **Composing a value list.** **list-ops.md (decided, implemented)** settles the operation
  set on the copy-on-write `list of T`: the placement rule is that a
  value-returning composition is a list op while a repeated in-place mutation is
  a buffer op (buffer.md), keeping a hidden O(N^2) off the list. Composition
  returns a fresh CoW list, adding `prepend`, `insert(xs, i, x)`, and `reverse`
  beside the shipped `append`/`set`/`delete`; concatenation is `+` (with `+=`
  rebind sugar), the same operator string concat uses (one "join two sequences"
  rule, no `concat`/`join` word); decomposition reuses slice-clamp.md's
  point-versus-interval rule (`first`/`last` fallible point access, `rest`
  clamping to `[]`); repeated push/pop (a stack/queue) is the buffer, not the
  value list (the functional stack is `prepend`/`first`/`rest`); higher-order
  `map`/`filter`/`reduce` (need a lambda/block design), `sort` (needs a
  comparator), and `take`/`drop` conveniences are deferred. Tier World.

## Functions: a lambda / block design

- **A first-class function or block value.** **function-values.md (decided, not
  yet implemented)** settles this. The guiding constraint is that every construct should be an
  ordinary function or a binding (macro-synthesizable), not a special form frozen
  into the compiler. So `func(params) returns T ... endfunc` is the one anonymous
  primitive; a named function keeps its readable `func name(...) ... endfunc`
  declaration, which is a prelude macro over one uniform core (binding a name to
  a func value) that the lambda and macros also target, like `class`/`record`/
  `verb` (so it maps to the macro system without deleting the readable form);
  types are explicit in the
  base layer (an untyped func is macro-layer); a captured func is non-escaping and
  call-scoped like `shared` (a free func captures nothing). The combinators
  `map`/`filter`/`reduce`/`sort` are ordinary functions taking func values, so
  `sort(xs, cmp)` takes a comparator/key (a baked `by` clause was declined as an
  unextendable frozen algorithm). Map and filter also get a comprehension
  `[EXPR for x in xs if COND]` (loop-lowered sugar, multiple `for` clauses nest,
  a nested comprehension is a 2-D result, `zip` is parallel iteration). Deferred:
  escaping/stored closures (needs the memory work below), named aggregates
  (`sum`/`count`/...) and a concise func-body form (a lambda is bound to a local
  name by reusing `const f = func(...)`).
  Tier: comprehensions World, function values Mechanics.

- **`.exs` / `.exi` interface consistency** (raised 2026-07 while implementing
  parameter defaults). A signature's full contract, including a parameter's
  default value, must be visible and consistent in the `.exi` interface file so a
  caller in another module sees it (which is why a parameter default is required
  to be a compile-time constant, not a runtime expression). Interface files
  should not be forced to be computer-generated, so the requirement is a
  **checker that verifies a `.exs` and its `.exi` match perfectly** (signatures,
  types, defaults, visibility), whether the `.exi` was hand-written or generated.
  Depends on the compiler actually emitting/reading `.exi` (host-abi.md);
  currently the implementation is single-module, so this is a pass for when
  cross-module compilation lands. Tier: toolchain.

## Surface and readability

- **The equality operator should not be `==`.** **equality.md (decided,
  implemented)** settles this: equality is spelled `=` and inequality `<>` (retiring `==`/`!=`
  with a lexer migration hint), reclaiming math's own equality symbol from
  binding per symbols-do-math (the current `=`-binds/`==`-compares is backwards).
  `is` is not reused because it is already the pervasive type word (`var x is
  integer`, `x is T`). Binding keeps `=`: a `var`/`const`/assignment statement
  eats its `=` before the expression parser runs, so a `=` inside an expression
  is always equality and C's `if (x = 5)` bug cannot occur (a complex lvalue
  embedding a comparison needs parentheses, rare; if an explicit assignment
  operator is ever wanted, a left arrow `<-`, math's/APL/Smalltalk's `←` in
  ASCII, is the recorded alternative, preferred over Pascal's `:=`). `=` means "the same thing": value equality for a value (records
  equal when fields are, records.md D5), identity for an `obj` (same actor, so a
  record's obj field compares by handle). Ordering `<`/`>`/`<=`/`>=` stays
  symbolic; `=` is binary (no chaining). Records use `=` for value comparison.
  Tier: World.

- **`then` to close an if-*statement* condition** (TODO line 24). **then-in-if.md
  (decided, implemented)** settles this: `if`/`elseif` close their condition with a required
  `then` (the same `then` as the if-expression, so statement and expression
  share one shape), loops close their header with `do` (`while C do`, `for ...
  do`, since `then` reads one-shot and `do` reads repeat; `match` stays
  self-delimiting), the condition between the head keyword and `then`/`do` is a
  newline-transparent region like `()` (compound conditions wrap with no
  continuation trick), a one-line guard is `if C then STMT endif`, an
  if-expression condition is disambiguated by its mandatory `else`, and a
  missing `then`/`do` is a local teaching error with a lexer migration hint.
  Tier World.

- **A local shadows its own type name (case-insensitive collision)** (raised
  2026-07, needs a pass). Excelsior folds case, so a local `var box is Box`
  binds the name `box`, which is the *same* name as the record type `Box`. The
  local shadows the type in that scope, so a later constructor call `Box(...)`
  resolves to the local, not the record, and fails at lowering with a confusing
  "not a callable" error. The idiomatic "lowercase the variable after its type"
  habit (a `Box` named `box`) walks straight into it. Candidate resolutions for
  the pass, roughly in preference order: (a) in call position `Name(...)`,
  prefer a type/record/class sym over a same-named local, since a bare-name call
  on a value is not otherwise meaningful (the Pascal / most-languages behavior,
  construction reads through the shadow); (b) failing that, a teaching error at
  the *shadowing declaration* or the call site ("`box` shadows the type `Box`;
  the constructor `Box(...)` is unreachable here, rename the variable"); (c) a
  naming rule, weak since case-folding erases the capital-letter convention
  other languages lean on. Relates to the case-insensitive surface (core.md).
  Tier: World (a beginner hits it first).

- **The element-type syntax `list<T>` needs another look** (raised 2026-07).
  **type-parameters.md (decided, not yet implemented)** already replaces the
  angle-bracket generic with the word `of` (`list<T>` becomes `list of T`,
  `buffer<T>` becomes `buffer of T`), on the words-over-symbols principle. That
  decision is not final: revisit the whole element-type surface before it is
  implemented, since the implementation still parses `list<T>` and the `of`
  spelling has not been exercised against the harder cases (nested elements like
  `list of list of T`, a record element `list of Point`, and how `of` reads next
  to `set of` / `buffer of` / a type parameter). Reopen type-parameters.md when
  this pass runs. Tier World.

## Control flow and lifetime

- **`defer`** (TODO line 52). **defer.md (decided)** settles this: `defer STMT`
  registers a statement to run when its enclosing lexical block exits (block
  scope, LIFO, run at exit reading current values). The key finding is that
  `defer` is **not** a memory tool here (the arena reclaims memory) but the
  teardown complement to Icon-style fallibility, the restore/release that
  survives the invisible exit a propagating fallible creates; it runs on every
  structured exit (fall-through, `return`, `break`/`continue`, fallible
  propagation) but not on a hard fault (which rolls back the turn), so it is not
  a general `finally`. This **decouples `defer` from the memory work below**: it
  needs nothing from the memory-management design. `errdefer` (a failure-only
  teardown) is deferred. Tier Mechanics.
- **Memory management from Compact Pascal** (TODO lines 42 and 44).
  **memory.md (decided, not yet implemented)** settles this, revisiting CP's lifetime question and
  diverging for Excelsior's audience: fully automatic with no tracing GC in the
  turn, region-partitioned on the escape boundary prior passes already drew. A
  per-turn arena reclaims transients wholesale (reset at turn end, reset to the
  start mark on a fault); an escaped immutable value lives in a reference-counted
  heap that needs no cycle collector (values are acyclic by construction) and is
  shared O(1) across actors; actors have an explicit lifecycle (`spawn`/`destroy`,
  a destroyed reference failing safely) so object cycles do not leak. The turn is
  transactional (settling runtime-errors.md's open question: journaled field
  writes commit or roll back), memory is bounded by a quota (exhaustion an
  `OVERFLOW`-class fault), and persist/freeze/refcount are the same type set
  (buffers and closures excluded, so escaping closures stay forbidden and a
  persistent buffer is uniquely-owned). Deferred: the coarse object cycle sweep,
  quota values, heap compaction, and weak references. Feeds the buffer type and
  actor lifetime. Tier: invisible at World, `spawn`/`destroy` World, mechanism
  host/runtime.
- **Growable buffer / array type** (TODO line 50). **buffer.md (decided)**
  settles this: `buffer<T>` is the mutable, growable counterpart to the
  copy-on-write `list<T>` value (building a list incrementally with CoW
  `append` is O(N^2); a buffer appends amortized O(1)). It is a third kind
  beside values and actors, a mutable, identity-bearing, actor-local scratch
  container, the transient to the list's persistent value (Clojure's
  persistent/transient, `tolist` = `persistent!`). It mutates in place (`b[i] =
  x`, `push`, `pop`, `clear`, `reserve`/`capacity` = the max/grow), aliases on
  assignment (so no `shared`), and escapes only by freezing to a value
  (`tolist`, `tostr` producing an owned string per string-repr.md).
  `buffer<char>` is the StringBuilder. Only **persistent (field) buffers**
  depend on the memory-management work below; the local builder needs nothing
  beyond the arena. Tier Mechanics.

## Strings and text

- **A `blob` binary-data type.** **blob.md (decided, not yet implemented)**
  settles this (raised
  2026-07 while implementing R7): a `blob` is the v1 answer for manipulating
  binary data (packing TELNET/ANSI escapes, parsing PNG/JPEG headers for a
  texture size, small protocol frames), a mutable, reference-counted, bounded
  byte **view** that reads and writes typed fields at offsets. Its backing is
  script-allocated or host-granted (a host power). It is memory-safe by
  construction, the backing is refcounted so a blob keeps it alive (no
  use-after-free) and access is bounded to the view length (no arbitrary read,
  cannot be forged), while staying mutable and aliasing (a slice is a range-limit
  not a copy, so a write shows through every slice); the residual looseness is
  semantic, not memory corruption, and the one cost is pinning (a script holds
  its host backing alive, a quota-bounded resource concern). Typed access is a
  decode/encode **macro** over literal width/sign/endian keywords (`read_int(off,
  16, unsigned, little)`); because a macro reads its arguments as forms (meta.md),
  `unsigned`/`little` are bare keywords needing no global symbol and the macro
  expands to the one constant-folded load/store, terser named aliases
  (`read_be32u`) being wrappers, the whole matrix contained in the type. The
  signed/unsigned boundary is a reinterpret to signed `int` for v1 (a `u32`
  `0xFFFFFFFF` reads as `-1`, no fault), with a proper unsigned type (returning
  unsigned, refusing an out-of-range store at compile time) and a 64-bit `int`
  deferred. `str` and blob convert at their edges; `buffer of byte` stays the
  growable builder. Deferred: the unsigned type, 64-bit `int`, named-alias form,
  blob literals, bit-level access, alignment, and the host grant/revoke
  lifecycle. Tier: Mechanics (a host-backed blob additionally a host power).

- **Regex / string pattern matching** (TODO line 54). **patterns.md (decided)**
  settles this: raw regex is rejected as the surface (cryptic, unsearchable, and
  a runtime engine is a ReDoS hazard); a pattern is a readable template with
  typed named holes (`"put ${item} in ${container}"`, `${count is int}`,
  `${dir is Direction}`), reusing the `${}` hole in the capture direction. A
  pattern is a **compile-time literal**, syntax-checked and compiled to a static
  matcher (a runtime pattern is a compile error, which is the compile-time-vs-
  runtime string distinction and the safety story). Matching is fallible
  (Icon/SNOBOL, binding captures or failing, captures being str views per
  string-repr.md), and command dispatch is `match` over a string with `when
  PATTERN then` arms (Inform 7's Understand grammar). Deferred: in-pattern
  optionals/alternation and a compile-time-literal raw-regex escape. Tier World.

- **Alternative string literal syntax.** **string-literals.md (decided, not yet
  implemented)** settles this: `"..."` forces an author to escape every internal double quote,
  the punctuation game dialog needs most, so a second delimiter `{...}` (a
  brace-delimited string) is added, chosen because prose almost never contains a
  brace, `${}` already trains braces-hold-text, and `{` is otherwise unused on
  the surface (non-breaking). It interpolates `${}` and escapes `\$` like
  `"..."` (the same string, a different delimiter); the close is brace-counted
  so balanced inner braces and all double quotes are literal, only a lone brace
  escaping (Tcl's rule); the same form is the multi-line string with
  common-indentation dedent (Java/Swift text blocks), so no third construct is
  needed. The Markdown fenced ``` block is declined (redundant, info-string
  baggage) and a raw string (no interpolation) is deferred with a backtick its
  natural home. Braces are claimed for strings, so a future map literal uses a
  constructor or `[...]`. Tier World.

## Compile-time and meta

The meta-layer surface is now settled (meta.md): a `macro name(params) ...
endmacro` def keyword in template and procedural tiers, reification by
`quote`/`quasi`. These follow-ons build on it:

- **Mixin / trait spelling** (shared-params.md). **capabilities.md (decided)**
  settles this: the word is `capability` (plain English for what an object can
  do), chosen over `mixin` (coined jargon), `trait` (programming-jargon naming a
  type), and `role` (Raku's readable pick over trait, but the RPG-role reading
  collides in a game); the type axis keeps the separate word `interface`.
  Declared `capability Name ... endcapability` (fields/verbs/funcs plus
  `requires`, the declarative form Mechanics, a computed capability Meta); a
  class includes it with the body directive `include Name`, splicing members in
  as class-level members on an axis separate from `is` inheritance; collisions
  are more-specific-wins/peers-error (class-own > capability > super, two
  capabilities colliding an error); `requires` states what the includer must
  provide, checked at the include site; a capability is reuse not a type
  (polymorphism is the deferred interface work, verbs.md D5). Tier: including
  World, declaring Mechanics, computed Meta (refining tiers.md's tier-3
  placement).
- **Record introspection primitives** (data-model spine item 4, above).
  **Settled: record-introspection.md** names the primitives (`typeof` /
  `hasfield` / `fieldtype` / `fieldsof`), the field-iteration static-foreach,
  and the guardrails. What remains is implementation, not a pass. Tier: Meta.
  A first vertical slice is implemented (`typeof` / `fieldsof` / `fieldtype`,
  quote/quasi, a compile-time `for`, and `get(rec, "field")`), enough to run a
  serializer macro.
- **Record field annotations** (raised 2026-07). **record-annotations.md
  (decided and implemented, D1-D6)** settles this: Go-style struct tags a macro
  reads, so a JSON (or
  freeze/save) serializer keys, renames, or skips a field from declarative
  metadata rather than hand-maintained lists. A field line becomes a comma list
  of entries (the same shape enum members and disclosures already have, and it
  reuses the existing comma-or-newline expression termination unchanged), and a
  `tags [...]` clause decorates the immediately preceding field (the builder-
  stack "decorate the top" op). Each tag is a symbol followed by an arbitrary
  number of atoms, comma separated inside the brackets, so a macro matches the
  leading symbol and discards an entry it does not handle in one step. Tags are
  free-form and unchecked (opaque quoted forms, Lisp-macro style), compile-time
  only (zero runtime), and surfaced as `fieldsof`'s `.tags` accessor beside
  `.name`/`.type`. Consuming them waits on the next macro-interpreter increment
  (a meta `if` plus head/rest or membership over a tag form). Tier: declaring
  World, consuming Meta. Relates to record-introspection.md, typed-macros.md.
- **Typed macros and the `_Generic` tier** (core.md, typed-data.md stage 4).
  **typed-macros.md (decided, implemented)** settles this. The base layer has no parametric
  polymorphism; genericity is a Meta-tier facility built from typed macros. The
  ordering problem (macros expand before typechecking, but a typed macro needs
  types) is solved by a second phase: a typed macro resolves *during*
  typechecking, when its already-checked arguments' types are known (type flows
  only inward, keeping typing acyclic). `typeof(expr)` bridges a value to its
  type; `match typeof(x)` (reusing match-when) is compile-time multi-way dispatch,
  the `_Generic`, choosing one branch with zero runtime cost (the compile-time
  twin of runtime `match x` over a union); `${T}` typed holes make a template
  type-parametric (completing typed-data.md stage 4). A generic op is a macro
  monomorphized per call site, composing with `fieldsof`/inline-`for` into
  compile-time reflection. No formal bounds system (requirements checked on the
  expansion, C++/Zig structural not Rust bounded); meta.md guardrails hold.
  Deferred: a constraint/bounds system, richer type reflection, instantiation
  caching. Tier: Meta.
- **Inline `for` over heterogeneous literals** (TODO line 26).
  **inline-for.md (decided, not yet implemented)** settles this: a `for` unrolls (expands its body
  once per element, each statically typed) exactly when its head is a
  compile-time-known sequence of statically-typed elements (a heterogeneous
  literal used directly, a const-bound one, or a meta-sequence like
  `fieldsof(T)`), because such a fixed mixed-type sequence has no single element
  type so a runtime loop cannot type its body; everything else stays a runtime
  loop. There is no `inline`/`static` keyword (unlike Zig/D): the unroll is
  forced by the data, not an optimization. Each copy is monomorphic (the loop
  variable at that element's concrete type), a copy invalid for its type is a
  compile error naming the element and its type, and the unroll is fully static
  and bounded (size cap, one-level nesting, `break`/`continue` as compile-time
  jumps). Iterating a mixed literal directly unrolls; materializing it into
  `list of any` boxes and loops with `match`. The runtime sibling lands
  mixed-lists.md D6 (a `for` over a runtime `list of any` synthesizing a body
  per reachable tag), so static unroll and runtime tag-dispatch are the two
  faces of one body-synthesis machinery. Tier Mechanics, the machinery under
  Meta (the `fieldsof` iteration); relates to record-introspection.md,
  mixed-lists.md, tiers.md.

- **A literal is not a meta value** (raised 2026-07). **meta-values.md
  (decided, implemented)** settles this. The symptom was `typeof(1)` reporting the generic
  "this form is not valid in meta-layer code", and the pass found the split was
  accidental: a **str and a bool literal already are** meta values (they were
  added for tag reading, record-annotations.md D6), only numbers were missing,
  and `typeof` rejects an atom even when it is one. The answer names the value
  domain as **atoms** (number, text, bool: self-evaluating, comparable,
  computable, spliceable as their literal), **forms** (an auto-quoted argument
  or a `quote` / `quasi`, carried and emitted), and **descriptions** (a type, a
  sequence, a descriptor from the introspection primitives), in one sentence:
  an atom is a value a macro computes with, a form is one it can only carry.
  `typeof` accepts either, so `typeof(1)` is `int` and `typeof("x")` is `str`;
  `${}` splices an atom as its literal (a literal inside `quote` stays part of
  the form, so no existing macro changes). Arithmetic is bounded: `+ - * / %`
  and the ordered comparisons on int numbers, `+` on text for a generated name,
  `=` / `<>` widened from text to numbers and bools, overflow and divide-by-
  zero compile errors, decimal deferred. What stays out is general compile-time
  execution: no meta `while`, no recursion, no user meta functions, so the only
  meta loop remains the bounded `for` and expansion terminates structurally.
  Depends on meta.md, and typed-macros.md wants the constants. Tier: Meta.

## Folded and closed

- **Two-member enums stringify as bool word pairs** (TODO line 28) is
  **folded into enums.md**. A two-member enum (`enum Lockness [locked
  unlocked]`) printing its member word in a string hole is enums.md's D6
  (per-enum name table) applied to a two-member set, the reusable declare-once
  form of the `cond then A else B` pick. No separate pass; it lands when enums
  are implemented.

## Not design passes

Docs, tracked in TODO: the language and toolchain manuals website (line 3) and
the Excelsior tutorial (line 16). The tutorial is already named as tiers.md's
D2 follow-up ("what the tutorial teaches and what it deliberately omits").
