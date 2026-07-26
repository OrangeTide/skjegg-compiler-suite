# Typed data literals: shapes for dialog trees

Status: decided and implemented (2026-07), the pass on
approachability.md's R5 (the story persona gets the least type
checking). All six decisions at the end were confirmed. Stages 1 to 3
are in the tree: the `shape` declaration, structural checking of a
symbolic data literal against a shape reached by context type, scalar
leaf types, and intra-tree `to`/label resolution. Stage 4 (typed holes
and guard/action fragments) is deferred with the code-fragment work, as
staged. A symbolic literal with no shape in context keeps its prior
behavior (types `any`, does not lower). The surface D6 left open settled
during implementation to a bracketed body (`Kind [ slots ]`); see the
implementation notes at the end.

Made by a machine. PUBLIC DOMAIN (CC0-1.0)

## The problem, restated

Dialog trees are the flagship content-creator artifact and the story
writer's main surface. A dialog is a Logo-flavored data literal:

    [greeting
      [text "Hello traveler"]
      [choice "About the barrow" to barrow_info]
      [choice "Goodbye" to end]]

Today the checker types any literal that contains words or nesting as
`any` (typecheck.c, the `symbolic` branch of `N_DATALIT`). It resolves
and checks the `${...}` holes inside, but the tree itself is unchecked.
So the persona the static-typing pitch aims at is the one whose content
flows through the untyped corner. Three mistakes that static checking
exists to catch all pass silently:

- a misspelled head word: `[choise "Goodbye" to end]`;
- a malformed node: `[choice "Goodbye"]` with no target, or `[text]`
  with no line;
- a dangling reference: `to barow_info` when the labelled node is
  `barrow_info`.

The other personas get real checking. The story writer, the one least
able to read a stack trace, gets the least. R5's recommendation: a
schema mechanism for quoted data, proportional to that persona's
importance, so a per-head shape declaration catches these at compile
time.

## What exists today

- The reader parses symbolic literals (`data_literal`, `data_item` in
  grammar.ebnf): a bracketed, named-head, comma-free tree of atoms,
  scalars, nested literals, and `${expr}` holes.
- `to` is not grammar. It is an ordinary atom by convention
  (`[choice "..." to barrow_info]` is four items: a str, the atom `to`,
  and the atom `barrow_info`, under the head `choice`). The link from a
  head to a target is spelled with a word, deliberately.
- The name resolver and checker walk into the holes only. A symbolic
  literal's head words and structure are never inspected; its type is
  `any`.
- Nothing symbolic lowers. At runtime such a literal is host-passed
  data (host-abi.md), not a native value. The "prop world will type it
  for real" TODO is exactly this note.

So the machinery to *reach* every node already runs (hole checking
descends the tree). What is missing is a declared shape to check each
node against, and a context that names which shape applies.

## Survey

The problem is validating a tree of named nodes against an
author-declared grammar. Prior art, nearest first:

- **Racket `syntax-parse` / syntax classes.** The closest fit: named
  classes describe the shape of a symbolic form, the matcher checks a
  literal against them, and the payoff is specific error messages
  ("expected a `choice` with a target here") instead of a structural
  mismatch deep in a macro. This is "a per-head shape declaration
  checked before use," in a code-as-data language. It is the model to
  copy.
- **RelaxNG (compact syntax).** A regular-tree grammar: element
  patterns, sequences, repetition (`+`/`*`/`?`), alternation (`|`),
  references between named patterns. Exactly the expressive class a
  dialog schema needs, and no more (not a full CFG). The compact syntax
  reads close to the data it constrains.
- **Algebraic data types (ML, Rust `enum`, Haskell).** A dialog node
  kind *is* a variant: `text(str)`, `choice(str, target)`,
  `greeting(child+)`. A schema is a sum of node kinds. This is the
  semantic model underneath whatever surface we pick, and it is what
  makes the check decidable and total.
- **Inform 7 tables and "kinds of value."** The precedent aimed at the
  same persona. Tables give typed columns; a kind-of-value declaration
  names a domain. It validates, but the surface is prose and the errors
  are the cryptic ones R5's whole thrust is reacting against. Take the
  goal (typed content for a non-programmer), not the delivery.
- **Clojure spec / JSON Schema.** Registry of named shapes, structural
  validation after the fact. The registry idea (a shape is a named,
  reusable declaration) is worth keeping; the "validate untyped data at
  runtime" framing is not what we want. Our check is at compile time,
  against a literal whose structure is known.
- **NWN / editor-baked dialog formats.** A fixed schema inside a tool,
  not author-declared. We want the mechanics author to *write* the
  schema (a new node kind, a new stat table) without touching the
  compiler, so a baked-in schema is the anti-goal.

The consensus across the useful precedents: a named, author-declared,
regular-tree grammar; a sum of node kinds with typed, possibly repeated
slots and cross-node references; checked structurally with node-specific
error messages.

## Design

**A shape is a named type: a sum of node kinds.** A mechanics author (or
architect) declares it once; the story writer's literals are checked
against it. The declaration is a dedicated form whose lines are
bracketed like the Logo data they constrain (and like `enum Name
[...]`):

    shape dialog
        dialog [node+]                   -- one or more labelled nodes
        node   [label line+]             -- a label, then one or more lines
        line   [text or choice]          -- a line is text or a choice
        text   [str]                     -- a line of spoken text
        choice [str to ref]              -- a prompt, then a jump target
    endshape

Each line is either a **node kind** (a head word and the sequence of
body slots it requires) or a **group** (an alternation of kind names,
written with `or`). A slot is one of:

- a **scalar type**: `str`, `int`, `decimal`, `bool` (a leaf atom of
  that type);
- a **literal keyword atom**, written bare (`to`): the word must appear
  verbatim, which is how the `[choice "..." to X]` convention gets
  pinned by the schema rather than by grammar;
- a **nested node**, named by kind or by a group (`node`, `line`);
- `label`, a node's declared name, and `ref`, a use of one, both bare
  atoms resolved against each other within the same tree;
- a **hole** `${T}`: a `${...}` whose spliced expression must have type
  `T` (stage 4). This is where the code-as-data lambdas (core.md: a
  `when` guard is `fn(ctx) -> bool`, a hole is `fn(ctx) -> value`) get
  their static contract.

with `*` / `+` repetition suffixes (RelaxNG's regular-tree vocabulary)
and `or` for alternation. The first node kind declared is the **root**
the top literal's head must match.

**Binding is by context type; `any` when there is none.** A symbolic
literal checks against a shape exactly when the context supplies one:

    verb converse(script is dialog) ...      -- a typed parameter
    dialog opening = [greeting ...]          -- a field default
    let s: dialog = [greeting ...]           -- an ascription
    [greeting ...] as dialog                 -- an explicit cast

This reuses the path the empty list already takes ("`[]` takes its type
from context"). With no shape in context, a symbolic literal stays
`any`, which is today's behavior, so nothing existing breaks. Opting a
dialog into checking is opting it into a typed parameter or field, which
is where a dialog wants to live anyway.

**The check is structural and total.** Given a literal and a shape, the
checker walks the tree once against the node-kind grammar:

1. the head word must name a declared node kind (catches `choise`);
2. the body must match that kind's slot sequence, arity and scalar
   types included (catches the missing target and `[text]`);
3. references resolve against the labels collected from the tree
   (catches `barow_info`);
4. each `${...}` hole is checked at its declared slot type (this already
   half-happens: holes are checked today, but against nothing; now they
   are checked against `T`).

Every failure is a node-specific compile error in the runtime-errors.md
teaching voice ("`choice` needs a target: `[choice \"...\" to <label>]`;
found only a prompt"), not a structural type mismatch.

### Reframing "checked by the reader"

R5 said "checked by the reader." The reader cannot do it: it does not
know which shape a bare `[...]` belongs to until a context type names
one, and context types are a checker-phase fact. So the check lives in
the **type checker**, at the same `N_DATALIT` site that today returns
`any`, once a shape is known from context. The reader's job is
unchanged (parse the tree, register holes). This is a deliberate
reinterpretation of the finding, called out as decision D3.

### What stays out

The shape is a **static check only**. Symbolic literals still do not
lower to native values; at runtime a checked dialog is the same
host-passed tree it is today (host-abi.md), now with a compile-time
guarantee that its skeleton is well-formed and its holes are
well-typed. This keeps the whole feature independent of the open ABI
question of how symbolic data marshals through sends (the pending list
in CLAUDE.md), and independent of the eventual content-addressed
runtime representation (core.md, the code-as-data spectrum). The schema
is the front-end property R5 asked for; the runtime is a separate pass.

## Staging

1. **The `shape` declaration and structural check.** Parse `shape`,
   build the node-kind grammar, check head words and slot sequences
   (arity + literal keyword atoms + nested-node kinds) when a literal
   flows into a shape-typed context. This alone catches the misspelled
   head and the malformed node, the two highest-frequency mistakes, and
   is the bulk of the value.
2. **Scalar leaf types.** Check `str`/`int`/`decimal`/`bool` slots
   against the atoms filling them.
3. **Intra-tree reference resolution.** Collect labels, resolve `to`
   references, report a dangling target with the near-miss suggestion
   the fault UX already knows how to phrase.
4. **Typed holes and deferred fragments.** Check `${T}` holes at their
   slot type; when the code-fragment work (spectrum item 3, lambdas by
   hash) lands, this is where a `when`-guard's `fn(ctx) -> bool` and a
   `do`-action's `fn(ctx) -> void` contracts attach. Defer with that
   work.

Stages 1 to 3 are the R5 fix and are self-contained. Stage 4 waits on
the code-as-data runtime it shares a contract with.

## Decisions (confirmed and implemented)

**D1. Bind by context type; `any` with no context.** A symbolic literal
checks against a shape when a param, field, ascription, or return
supplies one, and stays `any` otherwise. Non-breaking, reuses the
empty-list "type from context" path, and puts checked dialogs where they
already want to live.

**D2. A dedicated `shape` form, ADT underneath.** The surface is a new
`shape ... endshape` declaration whose lines read like the data they
constrain, over the alternative of making each node kind a variant
record and a shape a declared sum. The ADT (a sum of node kinds, a
regular-tree grammar) is the semantics; the Logo-flavored surface is the
skin, because the story writer reads the literal, not the record
constructor. Alternative kept on record: if the record/variant machinery
lands first for other reasons, a shape could be sugar over it.

**D3. Check in the checker, not the raw reader.** Honors R5's intent
(catch it before the content ships) while placing the check where the
context type is actually known. The reader stays as is.

**D4. Static-only; no lowering change.** The shape is a front-end check;
symbolic literals keep their current runtime treatment (host-passed, not
lowered). Decouples R5 from the ABI-marshaling and
content-addressed-runtime questions.

**D5. Ship stages 1 to 3 now, defer stage 4.** Structure, scalar leaves,
and reference resolution are the R5 fix and stand alone. Typed holes and
guard/action contracts ride the later code-fragment work, since that is
where the `fn(ctx) -> T` lambdas they type come from.

**D6. Exact repetition/alternation grammar settled at stage 1.** The note
fixed the vocabulary; the precise surface was settled during
implementation against a real dialog (`tests/exs_shape_dialog.exs`), and
is recorded below.

## Implementation notes

The pieces are in the front end (`excelsior/`): the `shape`/`endshape`
keywords (`lex.c`), the `parse_shape` declaration (`parse.c`), a
`SYM_SHAPE` named type resolved like a class or enum (`resolve.c`), and
the checker `check_shape` reached from the type-boundary function
`widen_to` and from the `as`-cast path (`typecheck.c`). A shape lowers to
nothing; symbolic literals still do not lower (D4).

What the surface settled to (D6):

- **Bracketed bodies.** A kind line is `Kind [ slots ]`, not `Kind =
  slots`. The reason is concrete: `+` and `*` are continuation tokens
  (a line ending in one splices the next line), so a trailing repetition
  suffix at a line's end swallowed the newline. Inside `[ ]` the layout
  rule suppresses newlines and the `]` ends the line cleanly, and the
  line now visually mirrors the `[Kind ...]` node it constrains, the same
  Logo shape as `enum Name [...]`.
- **`or`, not `|`, for alternation**, matching the language's rule that
  words carry structure and symbols carry math (the de-arrow decision).
  A group line is `line [text or choice]`.
- **`label` and `ref` are contextual slot words**, not global keywords;
  they are ordinary identifiers the checker recognizes only inside a
  shape, so they cost nothing elsewhere. `label` declares a node's name
  (unique per tree), `ref` uses one; unresolved `ref`s and duplicate
  `label`s are the reported errors.
- **`to` now parses as a data-literal atom.** It is a keyword elsewhere
  but reads as a plain word inside `[...]` data (the documented
  `[choice "..." to end]` convention, which had never actually parsed);
  a latent gap this pass closed.
- **The body matcher is greedy with no backtracking.** It is adequate
  while a kind's slots are type-distinguishable, which dialog and
  stat-table grammars are. A pathological ambiguous schema is not
  diagnosed as such; if a real one appears, the matcher gets a
  backtracking or first-set check then.
- **`?` (optional-single) was not added.** `*` and `+` cover the dialog
  and stat-table needs; `?` waits for a use.

The messages are in the runtime-errors.md teaching voice and name the
offending node: a misspelled head is "`choise` is not a known node kind
(in shape `dialog`)", a missing target "a `choice` node needs a `to`
here", a dangling jump "`nowhere` points to no label in this data". The
misspelled head is R5's flagship catch and gets the most specific
message.
