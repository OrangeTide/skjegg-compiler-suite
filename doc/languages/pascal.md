# Pascal Language Reference (skj-pc)

`skj-pc` is a Pascal compiler targeting the Skjegg shared IR. The language is a
Pascal dialect in the Free Pascal lineage: managed (reference-counted) strings
and dynamic arrays, and the common procedural extensions, but without the Delphi
and Object Pascal object model (no classes, objects, interfaces, or RTTI). This
document records what is and is not supported.

Per-backend variants cover ColdFire, x86-64 (`skj-pc-x86-64`), AArch64
(`skj-pc-arm64`), MIPS, and WebAssembly (`skj-pc-wasm`, which emits a WASI
module or a relocatable WASM object and links with `skj-ld-wasm`). The WASM
lane runs the integer corpus; real-number output waits on a freestanding float
formatter in the runtime.

## Types

| Type | Status |
|------|--------|
| `integer` (32-bit signed) | supported |
| `boolean` | supported |
| `char` | supported |
| `byte` (unsigned 8-bit) | supported |
| `word` (unsigned 16-bit) | supported |
| `string`, `string[N]` | supported |
| `array[lo..hi] of T` (1D, 2D) | supported |
| `record...end` | supported |
| variant records (tagged, untagged) | supported |
| `set of T` (`+` `-` `*` `><`, `=` `<>` `<=` `>=`, `in`; element universe 0..255) | supported |
| enumerated types (incl. explicit ordinals `Tue = 10`) | supported |
| `real` / `double` / `single` / floating point | supported |
| pointer types (`^T`), `new`/`dispose`, `nil` | supported |
| file types (`text`, `file of T`, untyped `file`) | supported |
| subrange types (`1..100`) | supported as standalone declarations |
| procedural types (`procedure(...)` / `function(...): T`, `@routine`, indirect call) | supported |

Subrange syntax works in array bounds, set definitions, case ranges,
and as a standalone type declaration (`type X = lo..hi`), registered as
an ordinal type.

Enumerated types parse and work, but values are plain integer
constants; they are not a distinct type for range checking.

## Declarations

| Form | Status |
|------|--------|
| `const` (with optional type annotation) | supported |
| `var` (with optional initialization) | supported |
| `type` (alias, enum, record, array, set) | supported |
| `procedure` / `function` | supported |
| `forward` | supported |
| nested procedures (capture-argument passing) | supported |
| typed constants (scalar, array, matrix, record, set) | supported |
| `label` | **not implemented** |
| `uses` / `unit` | supported (see [Units](#units)) |
| `external` | supported |

## Parameters

| Form | Status |
|------|--------|
| value parameters | supported |
| `var` parameters (by reference) | supported |
| `const` parameters | supported |
| array parameters | supported |
| record parameters | supported |
| string parameters | supported |
| procedural parameters | supported |

## Statements

| Statement | Status |
|-----------|--------|
| assignment (`:=`) | supported |
| `if...then...else` | supported |
| `while...do` | supported |
| `for...to/downto...do` | supported |
| `for...in` (static arrays) | supported |
| `repeat...until` | supported |
| `case...of...end` (with `else`, ranges) | supported |
| `case` on a string selector (string labels) | supported |
| `with...do` (single and multiple records) | supported |
| `begin...end` | supported |
| procedure calls | supported |
| `break` | supported |
| `continue` | supported |
| `exit` / `exit(value)` | supported |
| `halt` / `halt(code)` | supported |
| `goto` | **not implemented** |
| inline assembly | **not implemented** |

## Expressions and Operators

| Category | Supported |
|----------|-----------|
| Arithmetic: `+`, `-`, `*`, `/`, `div`, `mod` | yes |
| Comparison: `=`, `<>`, `<`, `<=`, `>`, `>=` | yes |
| Boolean: `and`, `or`, `not`, `xor` | yes |
| Short-circuit: `and then`, `or else` | yes |
| Bitwise: `and`, `or`, `xor`, `shl`, `shr` | yes |
| Set membership: `in` | yes |
| Set operations: `+` (union), `*` (intersection), `-` (difference) | yes |
| Set comparison: `=`, `<>`, `<=` (subset), `>=` (superset) | yes |
| String concatenation: `+` | yes |
| Unary minus, unary `not` | yes |
| Array indexing: `a[i]`, `a[i,j]` | yes |
| Record field access: `r.field` | yes |
| String indexing: `s[i]` | yes |
| Type casts: `chr()`, `ord()`, `integer()`, `char()`, `byte()`, `word()` | yes |
| Address-of: `@x` (variable, field, element) | yes |
| Pointer dereference: `p^` (chains with `.f`, `[i]`) | yes |

## Built-in Procedures and Functions

### I/O

| Name | Status |
|------|--------|
| `write(args...)` | supported (integer, char, boolean, string) |
| `writeln(args...)` | supported |
| `write(x:width)`, `write(x:width:decimals)` | supported |
| `read(vars...)` | supported (integer, char, string, real) |
| `readln(vars...)` | supported |
| `eof`, `eof(f)` | supported |
| `eoln`, `eoln(f)` | supported |
| `seekeof`, `seekeof(f)` | supported |
| `seekeoln`, `seekeoln(f)` | supported |
| `assign`, `reset`, `rewrite`, `close` | supported (text, typed, untyped files) |
| `append` (text file, write at end) | supported |
| `read(f, ...)`, `write(f, ...)`, `readln(f, ...)`, `writeln(f, ...)` | supported |
| typed files: `seek`, `filepos`, `filesize`, `get`, `put` | supported |
| untyped files: `blockread`, `blockwrite` | supported |
| `erase`, `rename` | supported |
| `ioresult`, `{$I+}`/`{$I-}` | supported |

### Arithmetic and Conversion

| Name | Status |
|------|--------|
| `abs(x)` | supported |
| `sqr(x)` | supported |
| `odd(x)` | supported |
| `succ(x)` | supported |
| `pred(x)` | supported |
| `ord(x)` | supported |
| `chr(x)` | supported |
| `inc(x)`, `inc(x, n)` | supported |
| `dec(x)`, `dec(x, n)` | supported |
| `lo(x)`, `hi(x)` | supported |
| `swap(x)` | supported |
| `upcase(ch)` | supported |
| `lowercase(ch)` | supported |
| `sizeof(T)` | supported |
| `sqrt(x)`, `int(x)` | supported |
| `sin(x)`, `cos(x)`, `arctan(x)`, `ln(x)`, `exp(x)` | supported |
| `trunc(x)`, `round(x)` | supported |
| `pi` | supported |
| `random(n)` (integer), `random` (real), `randomize` | supported |
| `high(T)`, `low(T)` | supported (ordinal types/values and arrays) |
| `paramcount`, `paramstr(n)` | supported |

### String Operations

| Name | Status |
|------|--------|
| `length(s)` | supported |
| `copy(s, idx, count)` | supported |
| `pos(substr, str)` | supported |
| `concat(s1, s2, ...)` | supported |
| `delete(s, idx, count)` | supported |
| `insert(src, dst, idx)` | supported |
| `str(val, s)` | supported |
| `val(s, v, code)` | supported |

### Memory

| Name | Status |
|------|--------|
| `fillchar(x, count, val)` | supported |
| `move(src, dst, count)` | supported |
| `new(ptr)` | supported |
| `dispose(ptr)` | supported |
| `getmem(ptr, size)` | supported |
| `freemem(ptr [, size])` | supported |
| `mark`, `release` | **not implemented** |

## Compiler Directives

| Directive | Status |
|-----------|--------|
| `{$R+}` / `{$R-}` (range checking) | supported |
| `{$Q+}` / `{$Q-}` (overflow checking) | supported |
| `{$ALIGN n}` | supported |
| `{$DEFINE name}` | supported |
| `{$UNDEF name}` | supported |
| `{$IFDEF name}` / `{$IFNDEF name}` | supported |
| `{$ELSE}` / `{$ENDIF}` | supported |
| `{$I filename}` (include) | **not implemented** |
| `{$M stacksize}` | **not implemented** |

Record fields are laid out with **natural per-field alignment**: each field is
aligned to its own size, capped at the target pointer width (4 on the ILP32
targets, 8 on the LP64 x86-64/arm64 targets). So a 4-byte `integer` aligns to 4
on every target, while a pointer or 64-bit field aligns to 8 on LP64. The
pointer-width cap matters on LP64 because a pointer relocation must land on an
8-aligned offset (the Mach-O static linker rejects an unaligned one). `{$ALIGN
n}` lowers the cap for the records that follow it, down to `{$ALIGN 1}` for a
fully packed record. The record's own size is padded up to its alignment so an
array of records keeps every element aligned.

## Literals

| Form | Status |
|------|--------|
| Decimal integers | supported |
| Hex: `$FF`, `0xFF` | supported |
| Octal: `0o77` | supported |
| Binary: `0b1010` | supported |
| String: `'text'` | supported |
| Char: `#65`, `#$41` | supported |
| Real/float literals | **not implemented** |

## Comments

`{ block }`, `(* block *)`, and `// line` comments are all
supported.  Shebang lines (`#!/...`) are skipped.

## Grammar

skj-pc has no separate grammar specification file; the grammar is the
one the front end under `pascal/` parses. The EBNF below describes the
implemented subset (it omits the features listed as not implemented above). It
follows the usual conventions: `{ ... }` is zero or more, `[ ... ]` is optional,
`|` separates alternatives, and quoted text is literal.

```ebnf
compilation   = program | unit ;

program       = "program" ident [ "(" ident { "," ident } ")" ] ";"
                [ uses_clause ] block "." ;

unit          = "unit" ident ";"
                "interface" [ uses_clause ] { declaration }
                "implementation" [ uses_clause ] { declaration }
                "end" "." ;                 (* interface routines are headings *)

uses_clause   = "uses" ident { "," ident } ";" ;

block         = { declaration } compound_stmt ;

declaration   = const_decl | type_decl | var_decl | proc_decl ;

const_decl    = "const" { ident [ ":" type ] "=" const_expr ";" } ;
type_decl     = "type" { ident "=" type ";" } ;
var_decl      = "var" { ident { "," ident } ":" type [ "=" const_expr ] ";" } ;

proc_decl     = ( "procedure" ident [ params ]
                | "function"  ident [ params ] ":" type ) ";"
                ( "forward" ";" | block ";" ) ;

params        = "(" param_group { ";" param_group } ")" ;
param_group   = [ "var" | "const" ] ident { "," ident } ":" type ;

type          = ident
              | "string" [ "[" const_expr "]" ]
              | "array" "[" subrange { "," subrange } "]" "of" type
              | "record" field_list "end"
              | "set" "of" type
              | "(" ident { "," ident } ")" ;         (* enumerated *)
subrange      = const_expr ".." const_expr ;
field_list    = field { ";" field } [ variant_part ] ;
field         = ident { "," ident } ":" type ;
variant_part  = "case" [ ident ":" ] ident "of"
                { const_expr { "," const_expr } ":" "(" field_list ")" ";" } ;

compound_stmt = "begin" [ statement { ";" statement } ] "end" ;

statement     = compound_stmt
              | lvalue ":=" expr
              | ident [ "(" [ expr { "," expr } ] ")" ]    (* proc call *)
              | "if" expr "then" statement [ "else" statement ]
              | "while" expr "do" statement
              | "for" ident ":=" expr ( "to" | "downto" ) expr "do" statement
              | "repeat" statement { ";" statement } "until" expr
              | "case" expr "of" case_arm { ";" case_arm }
                [ "else" statement { ";" statement } ] "end"
              | "with" lvalue { "," lvalue } "do" statement
              | "break" | "continue"
              | "exit" [ "(" expr ")" ] | "halt" [ "(" expr ")" ]
              | (* empty *) ;
case_arm      = const_expr { ( "," | ".." ) const_expr } ":" statement ;

lvalue        = ident { "." ident | "[" expr { "," expr } "]" } ;

expr          = simple_expr [ rel_op simple_expr ] ;
rel_op        = "=" | "<>" | "<" | "<=" | ">" | ">=" | "in" ;
simple_expr   = [ "+" | "-" ] term { add_op term } ;
add_op        = "+" | "-" | "or" | "xor" | "or" "else" ;
term          = factor { mul_op factor } ;
mul_op        = "*" | "/" | "div" | "mod" | "and" | "shl" | "shr"
              | "and" "then" ;
factor        = literal
              | lvalue
              | ident "(" [ expr { "," expr } ] ")"       (* func/cast call *)
              | "(" expr ")"
              | "not" factor
              | "[" [ set_elem { "," set_elem } ] "]" ;   (* set literal *)
set_elem      = expr [ ".." expr ] ;

literal       = integer | string | char | ident ;         (* ident: const/enum *)
```

## Units

A program can be split across units. A unit groups declarations behind an
interface, is compiled on its own, and is pulled in by `uses`.

```pascal
unit mathu;
interface
const Answer = 42;
type TPoint = record x, y: integer; end;
function AddOne(n: integer): integer;
implementation
function AddOne(n: integer): integer;
begin
  AddOne := n + 1;
end;
end.
```

```pascal
program useit;
uses mathu;
var p: TPoint;
begin
  p.x := AddOne(Answer);   { 43 }
end.
```

The `interface` section declares what the unit exports (consts, types, `var`s,
and routine headings); the `implementation` section holds the routine bodies and
any private declarations. A `uses` clause may appear after the program heading,
and in both a unit's interface and its implementation. An interface `uses` names
units whose declarations the interface itself refers to; an implementation
`uses` is private to the unit's bodies.

### On-disk form

A compiled unit is a GNU `ar` archive named `<unit>.a`. It carries the unit's
compiled object beside a reserved text member named `.ppm` that holds the
interface: a one-line header (`skj-pascal-unit`, format version, unit name,
target architecture, and the interface `uses` list) followed by the
interface-section source verbatim. A consumer re-parses that source, so a unit's
exported record is laid out identically in the program that uses it. The arch
tag binds a unit to one target (its object is native code); a consumer refuses a
unit built for a different architecture. The reserved member is dot-named so the
archiver does not index it and a symbol-driven linker never pulls it as code.

A used routine links by its plain name against the unit's object member, so the
linker resolves a `uses` call with no special support: it pulls the needed
object members from the archive by symbol, exactly as for any static library.
The same `<unit>.a` therefore also links into a C program that declares the
routines, as an ordinary cross-language library.

### Building a unit

```sh
skj-pc --emit-unit --meta mathu.ppm -o mathu.s mathu.pas
skj-as -o mathu.o mathu.s
sh tests/pascal-archive.sh skj-ar mathu.a mathu.ppm mathu.o
skj-pc -U <dir> -o useit.s useit.pas      # -U adds a unit search directory
```

`skj-pc` searches for `<unit>.a` in the source file's directory, then each `-U`
directory, then the current directory. `--emit-unit` requires `--meta <file>`
for the interface member, and writes the object through the usual `-o`.

### Current limits

Overloaded exported routines, qualified or selective `uses` (`uses U as Q`,
`uses U (a, b)`), wildcard `uses`, and a unit initialization section are not yet
supported. A unit ends at `end.` with no initialization block.

## Summary of Major Gaps

These features are absent from the implementation and would require
substantial work to add:

1. **Floating point** (`real` type, math builtins) -- requires
   integrating the existing IR_F64 support into the Pascal front end
   and adding sin/cos/sqrt/trunc/round either as IR opcodes or
   runtime library calls.

2. **Pointers and dynamic allocation** (`^Type`, `@`, `new`,
   `dispose`) -- requires adding pointer type tracking to the type
   system, lowering dereferences to IR_LW/IR_SW, and providing a
   heap allocator in the runtime.

3. **File I/O** (`file of T`, `text`, assign/reset/rewrite/close) --
   requires file descriptor tracking in the runtime and new built-in
   procedure lowering.

4. **Overloaded exports and qualified/selective `uses`** -- units are
   supported (see [Units](#units)); still missing are overloaded exported
   routines, `uses U as Q` / `uses U (a, b)` / wildcard `uses`, and a unit
   initialization section.
