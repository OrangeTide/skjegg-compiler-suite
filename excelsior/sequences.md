# Sequences, separators, and line endings

Status: focused study (2026-07). Excelsior has three ways to write "several
things in a row": space-separated data literals, comma-separated lists, and
newline-separated statements and declarations. The boundaries between them
are felt rather than stated, and the newline-termination policy leans on an
unpublished token set inside the lexer. One principle, already latent in
the language, covers all of it (the quotation boundary, below). All three
decisions were confirmed (2026-07): enum word lists and data-literal holes
are adopted, and the newline policy is option A, with the continuation set
published as CONT_TOKEN in grammar.ebnf.

Made by a machine. PUBLIC DOMAIN (CC0-1.0)


## Inventory

Every sequence in the language today:

| sequence            | separator | runs until      | element kind        |
|---------------------|-----------|-----------------|---------------------|
| call/send arguments | `,`       | `)`             | expressions         |
| parameters          | `,`       | `)`             | `name is type`      |
| use import names    | `,`       | `)`             | names               |
| select branches     | `,`       | `else` or NL    | expressions         |
| match labels        | `,`       | `then`          | constants           |
| enum members        | `,`       | NL              | names               |
| disclosures         | `,`       | NL              | names               |
| data literal items  | space     | `]`             | atoms, nested `[ ]` |
| statements          | NL        | block keyword   | statements          |
| members, fields     | NL        | section/end kw  | declarations        |

Two observations fall out of the table.

First, the separator choice is not arbitrary. Space separation appears
exactly once, in the data literal, and it is the only sequence whose
elements are guaranteed to be atoms (literals, bare words, nested
brackets). Every comma list has elements that can contain spaces
(expressions, typed parameters), so space cannot separate them. The rule
is already latent:

> Atoms can be separated by space; expressions need commas.

Second, the fragile rows are the two comma lists that run until NL (enum
members, disclosures). Every other comma list ends at a bracket or a
keyword, so it can wrap across lines for free (open bracket) or cheaply
(trailing comma). A comma list that ends at NL can only wrap via the
trailing-comma continuation rule, which is exactly the kind of invisible
rule a new user trips on. Disclosures are machine-generated, so only enum
hurts a human. The second latent rule:

> A comma list should run to a bracket or a keyword, never to a newline.


## Prior art, briefly

- **Logo, Tcl, Lisp**: space-separated works because everything is an atom
  or a bracketed/parenthesized subform. Confirms the atom rule.
- **Python**: newline terminates; only brackets and `\` continue. One
  teachable sentence, and errors point at the right line. The cost is
  parenthesizing long conditions.
- **Go**: semicolon insertion keyed on the last token of the line (a line
  ending in a token that can end a statement, ends). Same family as our
  rule, inverted; also maintains a token set, but a published one.
- **Ruby, Swift**: continue when the line is visibly incomplete (trailing
  binary operator). Reads naturally; the token set grows with the
  language, which is the maintenance cost we just paid with `else`.
- **JavaScript ASI, Lua**: cautionary tales. Rules keyed on "does the next
  line happen to start with `(` or `[`" produce action-at-a-distance
  errors. Our data literal already dodged this (core.md notes that
  newline-termination keeps a line-leading `[` from reading as an index).
- **C#, VB enums**: block-form enums are universally tolerated but nobody
  praises them; small enums want one line.


## The principle: the quotation boundary

The latent rules above compose into one statement:

> **Inside quoted contexts (strings, `[ ]` data literals) things are
> written as atoms, separated by adjacency, and computation enters only
> through `${ }` holes. In code contexts, sequences of expressions are
> comma-separated and always run to a bracket or a keyword. Newlines
> separate statements and declarations, never list elements.**

This is teachable in one breath: "commas belong to code, spaces belong to
data, newlines belong to statements." Strings already follow it (`${}`
holes). Data literals already follow the adjacency half, and core.md
already commits to `${expr}` holes inside quoted templates, so the
computation half is a planned extension, not a new invention. The code
side follows it everywhere except enum and disclosures.


## Consequences

### 1. Enum becomes a word list

    enum Color [red green blue]

    enum Direction [
        north south east west
        up down
    ]

An enum's members are atoms, which is precisely what a data literal of
bare words is. The bracket makes multiline wrapping free (open-bracket
continuation), so small enums stay one-liners and large ones need no
block terminator and no trailing commas. `is` drops from four meanings to
three, and the reading is honest: the enum is defined by this word list.
`record` stays a block because fields carry types, which are not atoms.

This resolves the enum item by stopping the list at a bracket instead of
a newline; the newline policy itself is untouched, and no block form is
needed.

### 2. Computed lists come from holes

The gap today: `[1 2 3]` is constants-only, and a computed list is built
by `append` onto `[]`. When list construction from expressions arrives,
the quotation-boundary rule says the mechanism is the one strings already
use:

    [10 20 ${limit}]        // list<int> with a computed element
    [greeting [text "Hi ${name}"] ...]   // same mechanism, nested data

One mechanism (the hole) covers string templates, data templates, and
computed lists; nothing new to learn. A `list(a, b, c)` constructor in
the `fixed()`/`vec()` family would also fit the type-name-as-constructor
pattern and can be added later as sugar if all-computed lists turn out to
be common, but it shouldn't be the primary mechanism, because holes are
needed anyway for dialog templates (core.md, immediate holes).

### 3. The NL-terminated comma lists are eliminated

Enum is fixed by (1). Disclosures are generated, so their NL-terminated
comma list is invisible to builders; when the `.exi` writer is real, emit
one disclosure per line and the case disappears. After that, every comma
list in the language runs to `)`, `]`, `then`, or `else`, and the rule in
the grammar header can say so.

### 4. The newline policy: keep it, but publish it

Current rule: NL terminates a statement, and a line does not end when a
bracket or paren is open, when it ends on a symbolic operator, `,`, or a
word operator, or with a trailing `\`. The problems found while writing
the grammar: the continuation token set lives only in `lex.c`, `else` is
a binary operator that must not continue (it closes `if` clauses), and
`from` is mid-expression in a `select` but does not continue.

Options considered:

- **A. Keep the completeness rule, publish the set.** A line that cannot
  be a complete statement does not end. Formalize NL in the grammar with
  the exact list and the two keyword exceptions (`else` and `of` head
  clauses whose grammar requires the newline). Add `from` to the set for
  consistency.
- **B. Python-strict.** Only brackets and `\` continue. Simplest to
  teach and gives the best error locality, but breaks the natural
  spelling of long conditions (`if a and <NL> b`), which is the one
  wrap builders actually write.
- **C. Leading-token continuation.** Join when the next line starts with
  a token that cannot begin a statement (`and`, `or`, `+`). Elegant, but
  it turns a typo at the start of a line into a silent join with the
  line above; worst error locality of the three.

Recommendation: **A.** In-world scripts are short, the set is small once
published, and after change (1) no declaration depends on trailing-comma
continuation for ergonomics; it becomes a convenience, not a load-bearing
rule. Revisit toward B only if playtesting shows surprise continuations.
Concretely: define NL formally in grammar.ebnf, list the continuation
tokens there (symbolic operators, `,`, `(`, `[`, and the
words `and or xor not in is as from to returns`), state the `else`/`then` exceptions,
and keep `\` as the escape hatch.


## Decisions (confirmed 2026-07)

1. Enum as a bracketed word list, `enum Color [red green blue]`: YES.
   Implemented (grammar, parser, tests).
2. Holes in data literals as the computed-list mechanism (with `list()`
   deferred as possible sugar): YES. Specified in the grammar;
   implementation is a TODO.
3. Newline policy: OPTION A, keep the completeness rule and publish the
   set. Done: NL, LINE_BREAK, and CONT_TOKEN are formal in grammar.ebnf,
   `from` was added to the set, and the `else` exception is
   documented there. A JS-style parser-level ASI was considered and
   rejected: it inverts the default to join-unless-error, which silently
   misparses our line-leading `[` data literals, line-leading `(` calls,
   and any `if` whose then-block ends in an expression followed by
   `else`; the restricted productions needed to patch those rebuild this
   same token set with worse failure modes.


## Plan once decided

1. Grammar: formal NL definition with the continuation set; enum_def
   rewritten to the word-list form; data_item gains HOLE; the
   quotation-boundary rule stated in the header conventions.
2. Lexer: add `from` to the continuation set; enum word list needs no
   lexer work (brackets already continue).
3. Parser and checker: enum_def; typing rule for holes in data literals
   (homogeneous element type yields list<T>).
4. Tests: enum samples, a multiline enum, a hole-in-list end-to-end test.
5. Update core.md's data-literal section to name the quotation-boundary
   rule, and retire the enum bullet from TODO.md.
