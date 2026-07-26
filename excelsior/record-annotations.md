# Record field annotations: `tags [...]`

Status: decided (2026-07); implemented (D1-D6): storage, the `.tags`
accessor, and the meta `if` / tag inspection that lets a macro read tags. A records and introspection follow-on (records.md,
record-introspection.md), the Go-struct-tag counterpart for Excelsior. Tier:
declaring World (a builder annotates a field), consuming Meta (a library macro
reads the tags). Builds on records (records.md), the field descriptor
(record-introspection.md), and the meta layer (meta.md).

A record field can carry declarative metadata that the compiler stores but never
interprets, for a macro to read at expansion. The motivating case is a
serializer: a JSON macro that reads a field's key name, whether it is required,
and whether to omit an empty value, from the field itself rather than a
hand-maintained parallel list. This is Go's struct tags, refit to Excelsior's
grammar and its Lisp-style macros. The metadata is a list of **tags** attached
to a field with a `tags [...]` clause, and each tag is a symbol followed by its
own atoms, so a macro walks the list and dispatches on the leading symbol.

Made by a machine. PUBLIC DOMAIN (CC0-1.0)

## A field line is a comma list of entries

The field syntax extends from "one field per line" to "a comma list of entries,
terminated by a newline", the same shape enum members and disclosures already
have (sequences.md). An entry is either a field definition (`name is type [=
default]`) or a `tags [...]` clause. So both of these are legal:

    record Point
        x is int = 0, y is int = 0
    endrecord

    record Point3D
        x is int = 0, tags [json "x", required]
        y is int = 0, tags [json "y", required]
        z is maybe int, tags [json "z", omitempty]
    endrecord

The decisive reason for the comma, over a keyword that ends the field
definition, is the parser. An expression already terminates at a comma or a
newline (a comma is not an operator, so `parse_expr` stops at one on its own).
Making a field line a comma list therefore reuses the existing expression
grammar with no change. The alternative, letting the `tags` keyword end the
`= default` expression, would force the expression parser to learn a new stop
token, and every later trailing clause would add another one. That is a
different termination rule for each kind of clause. One rule, "an entry runs to
a comma or a newline", keeps a single shared grammar, which is why the field
default and a list element and an enum member can all be parsed the same way.

## A `tags` clause annotates the preceding entry

`tags` is a keyword, so after a comma it cannot be mistaken for a new field
named `tags`; any other token after a comma starts a new field. A `tags [...]`
clause attaches to the **immediately preceding field entry** on the line. On a
`x is int = 0, tags [...]` line the tags bind to `x`; on a bare `x is int, y is
int` line both are plain untagged fields.

Binding to the preceding entry is the concatenative reading of a record body
(meta.md: `record` is a prelude macro over `mktype`, each field pushed onto a
compile-time builder stack). A field def pushes a field; `tags` is a postfix op
that decorates the top of the stack. This is the same "operate on what was just
built" shape as Forth, and it is why the tag list sits after the field it
describes rather than before.

## A tag is a symbol and its atoms

Inside the brackets, tags are comma separated, because each tag entry can hold
spaces and "expressions need commas, atoms take spaces" (sequences.md). Each tag
is a **symbol followed by an arbitrary number of atoms**:

    tags [json "x", required]
    tags [json "z", omitempty]
    tags [db "point_x" indexed, doc "the x coordinate"]

The leading symbol is the tag name (`json`, `required`, `db`, `doc`); the atoms
after it are its arguments (a string, more symbols, a number). A bare symbol
with no atoms (`required`, `omitempty`) is a flag. This shape is chosen for the
consumer: a macro loops over the tags, matches the first item, and if the symbol
is not one it handles it discards the whole entry in one step, without parsing
the arguments of a tag it does not care about.

The atoms are stored as an opaque quoted form, the way a Lisp macro receives a
pointer to a parse subtree. The compiler does not check that `json` takes one
string or that `required` takes none. Meaning lives entirely in the consuming
macro, exactly as Go leaves tag semantics to the library. An unrecognized tag is
ignored by any macro that does not look for it, so tags are open-ended and adding
a new one never touches the compiler.

## Reading tags: the introspection hook

Tags are **compile-time only**. They live on the field descriptor, not in the
record's runtime layout, so a tagged record has the same size and the same
`__exc_rec_new` block as an untagged one. Nothing about a tag reaches runtime.

The field descriptor `fieldsof(T)` yields (record-introspection.md) gains a
third accessor beside `.name` and `.type`: **`.tags`**, the field's tag list as
a compile-time sequence of tag forms. A serializer macro walks it:

    macro tojson(v)
        // sketch: emit a JSON object field per record field, honoring tags
        for f in fieldsof(typeof(v))
            var key = f.name
            for t in f.tags
                if t.head = json then key = t.rest.first endif
            endfor
            // ... emit "key": value, unless an omitempty tag says to skip ...
        endfor
    endmacro

This needs two things the macro interpreter does not have yet: a meta `if` and
membership or head/rest access over a tag form (`t.head`, `t.rest`, or `symbol
in t`). Those are the next macro-interpreter increment, so annotations land
after it. The tag storage and the `.tags` accessor can be built first; the
consuming macro waits on the increment.

## Survey

- **Go**: struct tags are a raw string literal after the field, `` `json:"x,omitempty"` ``,
  with a conventional `key:"value"` micro-syntax the reflect package parses at
  runtime. Excelsior takes the idea (per-field declarative metadata a serializer
  reads) but not the form: the tag is structured atoms the grammar already has,
  not a string with a nested convention, and it is read at compile time by a
  macro, not at runtime by reflection.
- **Rust**: attributes `#[serde(rename = "x")]` are structured and checked by
  the deriving macro, compile-time like this proposal, but they are a separate
  attribute grammar with its own `#[...]` surface. Excelsior reuses its own
  bracket-and-atom data shape instead of a new attribute language.
- **Java / C#**: annotations are types, validated and often read by runtime
  reflection. Rejected on both counts: a tag here is opaque data, not a type,
  and it is compile-time, not runtime.
- **Lisp**: a macro receives the form as data and inspects it. This proposal is
  that model applied to the tag list: `.tags` is a parse form the macro walks,
  which is why an arbitrary-length symbol-led entry is natural rather than a
  fixed key/value pair.

## Decisions (confirmed)

The six decisions are confirmed and settled. The surface extends the field line
to a comma list of entries, a `tags [...]` clause decorates the preceding field
with a symbol-led atom list, and the tags are free-form compile-time metadata a
macro reads through `fieldsof`'s new `.tags` accessor. Implementation waits on
the macro-interpreter increment (D6); the storage and accessor can land first.


**D1. A field line is a comma list of entries, terminated by a newline.** An
entry is a field definition (`name is type [= default]`) or a `tags [...]`
clause. Multiple fields on a line (`x is int = 0, y is int = 0`) are legal. The
reason is parser uniformity: an expression already stops at a comma or a
newline, so this reuses one shared expression grammar rather than giving each
clause its own terminator keyword.

**D2. A `tags [...]` clause attaches to the immediately preceding field entry.**
`tags` is a reserved keyword, so it is unambiguous after a comma. Binding to the
preceding entry is the builder-stack "decorate the top" op (meta.md), the
concatenative reading of a record body.

**D3. A tag is a symbol followed by an arbitrary number of atoms; tags are comma
separated inside the brackets.** The leading symbol is the tag name, the atoms
are its arguments, and a bare symbol is a flag. Comma separation follows from
tag entries holding spaces (sequences.md). The shape is chosen so a macro can
match the leading symbol and discard an entry it does not handle in one step.

**D4. Tags are free-form and unchecked.** The compiler stores the atoms as an
opaque quoted form and never validates a tag's name or arity. Meaning lives in
the consuming macro; an unrecognized tag is ignored. Adding a new tag never
touches the compiler.

**D5. Tags are compile-time only, surfaced as `fieldsof`'s `.tags`.** They live
on the field descriptor, not the runtime record, so a tagged record has the same
layout and zero runtime cost. `fieldsof(T)` descriptors gain `.tags` beside
`.name` and `.type`, a compile-time sequence of tag forms.

**D6. Consuming tags waits on the macro-interpreter increment.** Walking `.tags`
needs a meta `if` and head/rest or membership over a tag form, which the current
interpreter does not have. The storage and `.tags` accessor can be implemented
first; a serializer macro follows the increment.
