# String literals: a brace-delimited alternative for quote-heavy prose

Status: decided (2026-07), not yet implemented. A strings-and-text pass. Tier:
World (tier 1); an author
writes prose constantly, so the literal syntax is the most basic tutorial
surface. Builds on the shipped `"..."` string with `${}` interpolation
(string-plan.md), the `\$` escape, the owned-string builder (string-repr.md),
and the surface convention that words and `end<kind>` do structure so the
brackets are free for data.

The double-quote string `"..."` is fine until the text itself contains double
quotes, which in this language is often: the content is game prose, and prose is
full of spoken dialog. `say("The guard shouts \"Halt!\" and blocks your path.")`
makes an author escape every quotation mark, the punctuation their writing needs
most. This pass adds a second delimiter for exactly that text, and folds the
multi-line case into it, while keeping `"..."` as the everyday default.

Made by a machine. PUBLIC DOMAIN (CC0-1.0)

## The problem is the delimiter, not the string

A string literal has two jobs, mark where the text starts and stops, and let a
value be spliced in with `${}`. The `"..."` form does both well until the text
contains the delimiter. Then every internal `"` must become `\"`, and a
paragraph of dialog turns into a thicket of backslashes an author reading their
own prose cannot see through. The fix is not a new kind of string, it is a
**second delimiter** the author picks when the first one is inconvenient, the
`q{...}` / `%w[]` idea from Perl and Ruby, chosen for a delimiter that prose
almost never contains.

## Braces are that delimiter

The alternative is **`{...}`**, a brace-delimited string:

    say({The guard shouts "Halt! Who goes there?" and blocks your path.})

Braces are the right choice for three reasons:

- **Prose almost never contains a brace**, so the escaping that plagued `"..."`
  effectively disappears.
- **The reader already knows braces hold text**, because `${}` interpolation has
  trained exactly that association; a `{...}` string is the same idea with the
  `$` dropped.
- **Braces are otherwise unused on the surface.** Blocks close with `end<kind>`,
  grouping and parameters use `()`, data literals and ranges use `[]`; the only
  place `{` appears today is right after `$`. So claiming `{...}` for a string
  is purely additive, a bare `{` in a value position was a syntax error before.

Inside `{...}`, `${expr}` still interpolates and `\$` still escapes a literal
dollar, so a braced string is the *same string* as `"..."`, differing only in
which delimiter it escapes. An author picks the delimiter that their text makes
convenient, and nothing else changes.

## Braces nest, so most text needs no escaping at all

The close is found by **counting braces**: a `{` opens, a `}` closes, and a
balanced inner `{...}` is literal text. So a braced string holds not only free
double quotes but balanced braces too, and only a *lone*, unbalanced brace needs
`\{` or `\}`. This is Tcl's braced-word rule, and it means the common case (any
prose, any dialog, even text describing balanced braces) needs no escaping.
Interpolation composes cleanly: the braces of an inner `${...}` hole and of a
nested `{...}` string are themselves balanced, so brace-counting stays sound
through any nesting, with a lone brace the only thing an author ever escapes.

## A hole holds an expression, so it holds either string

A `${...}` hole holds an expression, not string text, so the outer string's
scanner is suspended inside the hole and either string form may appear there:

    "quotes ${"inside"} and braces ${{could be}} both work."

`${"inside"}` is a nested double-quoted string; it can use `"` freely because
the outer scanner is paused for the hole, which is exactly why a `"..."` can sit
inside a `"..."` here (written without the hole, the inner quotes would close the
outer string). `${{could be}}` is a nested braced string: `${` opens the hole,
`{...}` is the string, and the final `}` closes the hole, the braces balancing.
Both examples interpolate a constant and so are only illustrative; the real use
of a string in a hole is an expression result, `${cond then "yes" else "no"}`.

Finding the end of a hole is therefore **string-aware**, not naive
brace-counting: a delimiter inside a nested string is literal, so `${"a}b"}`
interpolates `a}b` (the `}` inside the nested string does not close the hole).
The hole scanner lexes nested string literals and skips their contents, the same
recursion the reader already does for any nested expression.

## Escapes are accepted even where redundant

Both delimiters accept the **same** escape set, regardless of which escapes each
strictly needs. A backslash before a structural character (`\"`, `\{`, `\}`,
`\$`) yields that character literally, a backslash before any other ordinary
character yields the character (`\c` is `c`), and the control escapes (`\n`,
`\t`, `\\`, and the rest) keep their usual meaning. So a `\"` is legal inside a
`{...}` string even though `"` is already literal there, and a `\}` is legal
inside a `"..."` even though `}` is already literal there:

    ${"a\}b"}        // the } needs no escape here, but \} is accepted and means }

The lexer does not require these, but accepting them is deliberate. A redundant
escape lets an author state intent (this brace is literal text, not structure),
and it lets a simple editor syntax highlighter that only tracks backslashes color
the string correctly without reimplementing the brace-counting and string-aware
hole scan. The permissiveness costs nothing and removes a class of "why is this
an error" surprises for a character the author reasonably thought needed
escaping.

## The same form is the multi-line string

Braces do not care about newlines, so `{...}` **is** the multi-line string, and
the language needs no third construct for a long description:

    look = {
        A cavernous hall stretches before you. Banners reading "Victory"
        hang from the rafters. A voice echoes: "${greeting}, traveler."
    }

To keep the author's indentation out of the text, a multi-line braced string
**strips the common leading whitespace** of its lines and drops the newline
right after the opening `{` and right before the closing `}`, the Java text-block
and Swift `"""` behavior. So the block above is three lines of prose with no
leading spaces, and the author indents for code layout freely. The exact
whitespace rules (trailing spaces, tabs-versus-spaces) are an implementation
sub-decision; the principle is that visual indentation is layout, not content.

## Why not a fenced ``` block

A Markdown-style triple-backtick fence was the other candidate for long text,
and it is declined:

- **Multi-line `{...}` already covers the long-text case**, so a fence would be
  a third string form for no new capability, against the small-surface goal
  (tiers.md's remove-don't-tier).
- **A fence carries Markdown baggage.** The info-string convention (```` ```lang ````)
  would read as a language tag, and a backtick says "code, verbatim" to anyone
  from Markdown, the opposite of a string that interpolates.
- **Three characters each side is heavy** for what is often a short line.

The verbatim reading a backtick carries is genuinely useful for a *different*
string, a **raw** one that does no interpolation and no escaping (for text that
literally contains `${...}` or a lone backslash, a rare need). That is deferred,
and if it lands a backtick is its natural home, because there the "code,
verbatim" connotation is correct. It is a separate axis from this delimiter
choice, both `"..."` and `{...}` interpolate.

## Which string when

Two forms, one guideline:

- **`"..."`** is the default, for short text with no embedded double quotes.
- **`{...}`** is for text that contains double quotes (dialog) or spans multiple
  lines.

Single quotes `'...'` were considered as the quote-heavy alternative and
rejected: English prose is full of apostrophes (`don't`, `it's`, possessives),
so a single-quote delimiter would need *more* escaping than the double-quote
form it replaced, not less. Braces are rare in prose in a way no quote character
is, which is the whole reason they win.

## Lowering

A braced string produces the **same AST** as `"..."`: the reader folds it into a
concat chain with each `${}` hole wrapped in an `N_TOSTR` (string-plan.md), so
only the lexer's entry differs (a brace-counting reader beside the quote reader,
sharing the interpolation sub-lexer and the escape handling). Nothing downstream
changes, the type router, the owned-buffer builder, and the fold to a constant
global are all reused. The multi-line dedent is a post-pass over the collected
literal text before it becomes the constant.

## Survey

- **Tcl**: `{...}` as a braced word with brace-counting and no substitution
  inside; Excelsior takes the brace-counting and the lone-brace escape but keeps
  `${}` interpolation, since the delimiter choice is orthogonal to substitution.
- **Perl `q{}` / `qq{}`, Ruby `%{}` / `%w[]`**: user-chosen delimiters so the
  author avoids escaping the one their text contains; the same motivation, fixed
  to braces here rather than a general delimiter-picking syntax (which is more
  surface than the audience needs).
- **Java text blocks, Swift `"""`, Nim/Scala triple-quotes**: multi-line strings
  with common-indentation stripping; Excelsior folds the multi-line case into
  `{...}` and adopts the dedent, without a separate triple-quote form.
- **Markdown fenced code blocks**: the triple-backtick candidate, declined for
  its info-string baggage and verbatim connotation.
- **Shell/Lua long brackets `[[...]]`**: another balanced multi-line delimiter;
  brackets are taken by data literals here, so braces serve the role.

Excelsior's stance: keep `"..."` as the default, add `{...}` (Tcl-style
brace-counted, `${}`-interpolating, multi-line with dedent) as the alternative
for quote-heavy and multi-line prose, decline the Markdown fence as redundant,
and reserve a backtick raw string for a separate deferred need.

## Decisions (confirmed)

The nine decisions are confirmed. The implementation (a brace-counting reader
beside the quote reader, sharing the interpolation sub-lexer and a common
permissive escape handler, plus the multi-line dedent post-pass) follows; the
braced string produces the same AST as `"..."`, so the checker and lowering are
unchanged.

**D1. Add `{...}`, a brace-delimited string, beside the shipped `"..."`.** The
double-quote form stays the default; the brace form is the alternative an author
picks when the text contains double quotes. Braces are chosen because prose
almost never contains them, `${}` already trains braces-hold-text, and `{` is
otherwise unused on the surface (so the addition is non-breaking, a bare `{` in
a value position was previously invalid).

**D2. `{...}` interpolates `${}` and escapes `\$` exactly like `"..."`.** It is
the same string with a different delimiter, not a different kind of string; the
author picks the form that minimizes escaping for their text and nothing else
changes.

**D3. The close is found by brace-counting; braces nest.** A balanced inner
`{...}` is literal text, so free double quotes and balanced braces need no
escaping, and only a lone unbalanced brace takes `\{` / `\}` (Tcl's rule).
Interpolation and nested braced strings stay balanced, so counting is sound
through any nesting.

**D4. `{...}` is also the multi-line string, with common-indentation dedent.** A
braced string may span lines; a multi-line one strips the common leading
whitespace and the newline just inside each brace (Java text-block / Swift
behavior), so indentation is layout not content. The exact whitespace rules are
an implementation sub-decision.

**D5. The Markdown fenced ``` block is declined; a backtick raw string is
deferred.** Multi-line `{...}` already covers long text, and a fence carries
info-string and verbatim baggage. A separate *raw* string (no interpolation, no
escapes) is a distinct future need, and a backtick is its natural home if it
lands; both `"..."` and `{...}` interpolate.

**D6. Braces are claimed for strings, so a future map/record literal uses a
constructor or `[...]`, not `{key: value}`.** This keeps `{` unambiguously a
string start on the surface, consistent with records constructing via
`Point(...)` (records.md) rather than a brace literal.

**D7. Guidance: `"..."` for short text without embedded double quotes, `{...}`
for dialog (quote-heavy) or multi-line text.** Single quotes `'...'` are rejected
as the alternative, since apostrophes pervade English prose and would need more
escaping than double quotes, not less.

**D8. A `${}` hole holds an expression, so either string form may appear inside
it, and the hole scan is string-aware.** A nested `"..."` or `{...}` in a hole is
an ordinary expression (`${cond then "yes" else "no"}`); the outer scanner is
suspended, which is what lets a `"..."` nest inside a `"..."`. Finding the hole's
end lexes nested strings and skips their contents, so a delimiter inside a nested
string is literal (`${"a}b"}` interpolates `a}b`), not naive brace-counting.

**D9. Both delimiters accept the same permissive escape set, even where
redundant.** A backslash before a structural character (`\"`, `\{`, `\}`, `\$`)
or any other ordinary character yields that character literally (`\c` is `c`);
control escapes (`\n`, `\t`, `\\`, ...) keep their meaning. A redundant escape
(`\}` inside `"..."`, `\"` inside `{...}`) is accepted, not an error, so an author
can state intent and a simple backslash-tracking syntax highlighter stays correct
without the full brace and hole scan.
