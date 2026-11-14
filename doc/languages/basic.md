# BASIC language specification

Skjegg's BASIC is a fully structured BASIC dialect compiled statically to native
code through the shared IR, the same path the other front ends take (`lex.c`,
`parse.c`, `lower.c`, `main.c`). The compiler is `skj-bc`; source files use the
`.bas` extension.

The language is imported from BASIDE (a structured-BASIC dialect aimed at simple
games). Skjegg takes the core language only: the four value types, declarations,
structured control flow, procedures and functions, aggregates, and the core
built-in functions. BASIDE's bytecode VM and its display and game statement
library are out of scope. Graphics or other host services, if wanted later, come
through a unit such as `libpsx` or a separate runtime, not through built-in
statements. Skjegg does add a module system (`MODULE`, `EXPORT`, `FROM`,
`IMPORT`, and the `EXTERNAL` clause that binds a foreign C routine) for building
and consuming shared libraries; it is specified separately in `basic-modules.md`,
not here.

This document is the grammar specification. As with the other front ends, the
grammar is the source of truth and is kept ahead of the implementation; a
production the compiler does not yet accept is noted where it matters.

## Design summary

- Fully structured. There are no line numbers and no `GOTO`.
- Four value types: `INT`, `FLOAT`, `BYTE`, `STRING`.
- Variables are declared with `DIM name AS type`. There are no type sigils.
- Procedures (`PROC`) and functions (`FN`) take typed parameters. Variables
  declared inside a routine are local to it.
- Statements are one per line, or several per line separated by `:`.
- Keywords and identifiers are case-insensitive. `Print`, `PRINT`, and `print`
  are the same token, and `count` and `Count` are the same variable.

## One-pass notes

The compiler makes a single pass over the source, which shapes the grammar the
same way it does in BASIDE:

- Because there is no `GOTO`, every forward branch comes from a structured block
  (`IF`, `WHILE`, `FOR`, `REPEAT`, `CASE`), so the forward distance is bounded by
  the block and the jump is backpatched at the block terminator.
- A `PROC` or `FN` is defined before it is called, so its signature is known at
  every call. Mutual recursion uses `DECLARE` to give a routine's signature ahead
  of its body; a declared routine never defined is an error reported at end of
  input.

## Program structure

```ebnf
Program     = { ProgramItem } .
ProgramItem = TypeDecl
            | ProcDecl
            | FnDecl
            | ForwardDecl
            | Statement .
```

A program is a sequence of type and routine definitions and statements at module
scope. `DIM`, `CONST`, and `ENUM` are statements (see `SimpleStmt`), so
declarations are just statements that happen to declare. The statements at module
scope form the main program and run in source order. `PROC` and `FN` definitions
are not executed inline; they are entered only when called. A `TYPE` must be
defined before it is used, the same rule `PROC` and `FN` follow.

## Types

```ebnf
Type       = ScalarType | VectorType | RecordType .
RecordType = IDENTIFIER .                 (* a record type declared with TYPE *)
ScalarType = 'INT'       (* 64-bit signed integer *)
           | 'FLOAT'     (* 64-bit IEEE double *)
           | 'BYTE'      (* 8-bit unsigned, 0..255 *)
           | 'STRING' .
VectorType = 'VECTOR' 'OF' Type .
```

`BYTE` exists mainly for buffer and array work. A `BYTE` promotes to `INT` in
arithmetic and truncates on assignment back to a `BYTE`.

The name `INT` is both this type and the truncation function `INT(x)`; which is
meant is clear from context, a type after `AS`, a function in an expression.

`STRING` and `VECTOR OF T` values are handles: a variable of that type holds a
reference to data that lives elsewhere. The scalar types and records built only
from scalars are plain values.

## Declarations

```ebnf
DimStmt   = 'DIM' DimItem { ',' DimItem } .
DimItem   = IDENTIFIER [ '(' ConstExprList ')' ] 'AS' Type .
ConstStmt = 'CONST' IDENTIFIER '=' ConstExpr { ',' IDENTIFIER '=' ConstExpr } .
EnumStmt  = 'ENUM' EnumItem { ',' EnumItem }
          | 'ENUM' NEWLINE { EnumItem { ',' EnumItem } NEWLINE } 'ENDENUM' .
EnumItem  = IDENTIFIER [ '=' ConstExpr ] .
ConstExprList = ConstExpr { ',' ConstExpr } .
```

- `DIM count AS INT` declares a scalar. `DIM buf(64) AS BYTE` declares an array.
  `DIM grid(80, 25) AS BYTE` declares a two-dimensional array. Array bounds are
  integer constant expressions folded at compile time; indices run from 0 to
  bound minus one.
- A `CONST` value is a constant expression folded at compile time: numbers, other
  constants already defined (or the predeclared `PI`), and the usual operators. A
  `CONST` has no runtime storage.
- `ENUM` declares a run of `INT` constants that count up: `ENUM SKY, GRASS, DIRT`
  is 0, 1, 2 and `ENUM SKY = 20, GRASS, DIRT` is 20, 21, 22. An item with `=`
  restarts the count from its value. The block form takes one item or comma list
  per line.
- At module scope `DIM` declares a global. Inside a `PROC` or `FN`, `DIM` declares
  a variable local to that routine. The `Type` may be a record type or
  `VECTOR OF T`, so `DIM e AS Enemy`, `DIM enemies(64) AS Enemy`, and
  `DIM bullets AS VECTOR OF Bullet` are all declarations.

BASIDE's `AT addr` mapped-array form (binding a `DIM` onto a memory address) is a
host-memory facility and is not part of skjegg's core BASIC.

## Aggregate types

### Records

```ebnf
TypeDecl = 'TYPE' IDENTIFIER { Field } 'ENDTYPE' .
Field    = IDENTIFIER [ '(' ConstExprList ')' ] 'AS' Type ( ':' | NEWLINE ) .
```

A record groups named fields. A field may be a scalar, a `STRING`, an array, a
nested record, or a vector. A record built only from scalars is a plain value,
copied on assignment and when passed without `REF`.

### Vectors

A `VECTOR OF T` is a growable typed list of `T`. Elements are reached by a
zero-based index like an array. `APPEND v, x` adds an element, `REMOVE v, i`
drops one, `CLEAR v` empties it, and `COUNT(v)` is the element count. (The vector
runtime lands in a later phase; see the roadmap in `PLAN.md`.)

## Procedures and functions

```ebnf
ProcDecl    = 'PROC' IDENTIFIER '(' [ ParamList ] ')' StatementList 'ENDPROC' .
FnDecl      = 'FN' IDENTIFIER '(' [ ParamList ] ')' 'AS' RetList StatementList 'ENDFN' .
RetList     = Type { ',' Type } .
ParamList   = Param { ',' Param } .
Param       = [ 'REF' ] IDENTIFIER [ '(' ConstExprList ')' ] 'AS' Type .
ForwardDecl = 'DECLARE' ( 'PROC' IDENTIFIER '(' [ ParamList ] ')'
                        | 'FN' IDENTIFIER '(' [ ParamList ] ')' 'AS' RetList ) .
```

- A `PROC` has no return value. An `FN` returns a value of its declared type: a
  scalar, `STRING`, `VECTOR`, or record. An `FN` may return several values
  (`FN minmax(a AS INT, b AS INT) AS INT, INT`), each a scalar or `STRING`, used
  only where every value has a place (a multiple assignment, an argument list, a
  `PRINT` item, or statement position). At most 8 values.
- `REF` applies only to a record parameter, passing it by reference. A record
  parameter without `REF` is passed by value. Arrays and vectors are always by
  reference, so `REF` on them is not allowed. Scalars and `STRING` are passed by
  value.
- `RETURN` leaves a routine early. In an `FN` it carries an expression of the
  return type, or one per value. In a `PROC` it carries none. An `FN` that reaches
  `ENDFN` without a `RETURN` is an error.

## Statements

```ebnf
StatementList = { Statement Terminator } .
Terminator    = ':' | NEWLINE .
Statement     = [ SimpleStmt | CompoundStmt ] .
                (* the empty production allows blank lines and a trailing ':' *)

SimpleStmt = AssignOrCall
           | DimStmt | ConstStmt | EnumStmt
           | ReturnStmt | ExitStmt
           | PrintStmt | InputStmt
           | WaitStmt
           | 'END' .

CompoundStmt = IfStmt | WhileStmt | RepeatStmt | ForStmt | ForEachStmt | CaseStmt .

AssignOrCall = Designator [ { ',' Designator } '=' ExprList ] .
               (* a bare Designator is a PROC call; with '=' it is an assignment.
                  Which one is resolved from the symbol table, not the grammar. *)

ReturnStmt = 'RETURN' [ ExprList ] .
ExitStmt   = 'EXIT' ( 'WHILE' | 'REPEAT' | 'FOR' ) .
WaitStmt   = 'WAIT' Expression .   (* pause for Expression milliseconds *)
```

`END` halts the program. `WAIT` pauses for the given number of milliseconds (a
non-positive count returns at once). `AT` and `TAB` console cursor positioning
are not yet implemented.

### IF

```ebnf
IfStmt = 'IF' Expression 'THEN' ( SingleLineTail | BlockTail ) .

SingleLineTail = Statement [ 'ELSE' Statement ] .
BlockTail      = NEWLINE StatementList
                 { 'ELSEIF' Expression 'THEN' NEWLINE StatementList }
                 [ 'ELSE' NEWLINE StatementList ]
                 'ENDIF' .
```

The two forms are told apart by one token of lookahead after `THEN`: a `NEWLINE`
selects the block form, anything else the single-line form. The block form closes
with `ENDIF`, so there is no dangling-else problem.

### Loops

```ebnf
WhileStmt   = 'WHILE' Expression NEWLINE StatementList 'ENDWHILE' .
RepeatStmt  = 'REPEAT' NEWLINE StatementList 'UNTIL' Expression .
ForStmt     = 'FOR' IDENTIFIER '=' Expression 'TO' Expression [ 'STEP' Expression ]
              NEWLINE StatementList 'NEXT' [ IDENTIFIER ] .
ForEachStmt = 'FOR' 'EACH' IDENTIFIER 'IN' Designator NEWLINE
              StatementList 'NEXT' [ IDENTIFIER ] .
```

- `WHILE` tests before the body; `REPEAT` tests after, so its body runs at least
  once.
- The `FOR` control variable is a declared numeric variable (`INT`, `FLOAT`, or
  `BYTE`). `STEP` defaults to 1 and may be negative. An identifier after `NEXT`
  must match the control variable.
- `FOR EACH v IN coll` walks an array or vector, binding `v` as a reference to
  each element in turn. `v` is declared by the loop and scoped to it.
- `FOR` and `FOR EACH` are told apart by one token of lookahead: `EACH` after
  `FOR` selects `ForEachStmt`.

### CASE

```ebnf
CaseStmt = 'CASE' Expression NEWLINE
           { 'WHEN' WhenItem { ',' WhenItem } NEWLINE StatementList }
           [ 'ELSE' NEWLINE StatementList ]
           'ENDCASE' .
WhenItem = Expression [ 'TO' Expression ]     (* a value, or an inclusive range *)
         | RelOp Expression .                 (* a relation to the selector *)
ExprList = Expression { ',' Expression } .
```

The `CASE` expression is evaluated once. Each `WHEN` lists items tested against
it in order: a value matches by equality, `lo TO hi` matches within the inclusive
bounds, and `< v`, `<=`, `>`, `>=`, or `<> v` compares the selector to `v`. The
first matching `WHEN` runs and control jumps past `ENDCASE`; there is no
fall-through. `ELSE` runs when no `WHEN` matches.

### Console I/O

```ebnf
PrintStmt = 'PRINT' [ PrintItem { PrintSep PrintItem } [ PrintSep ] ] .
PrintItem = Expression
          | 'AT' Expression ',' Expression    (* move the cursor *)
          | 'TAB' '(' Expression ')' .        (* pad to a column *)
PrintSep  = ',' | ';' .
            (* ';' joins items with no space; ',' advances to the next column.
               A trailing separator suppresses the newline. *)

InputStmt = 'INPUT' [ STRING_LITERAL ';' ] Designator { ',' Designator } .
```

`PRINT` writes values at the console cursor. An `INT` prints in full; a `FLOAT`
prints the shortest digits that read back to the same value; a `STRING` prints
its characters. The `FLOAT` form is fixed-point with at least one fractional
digit, turning to scientific notation for a decimal exponent outside the range
[-4, 17). The shortest digits come from the Dragon4 algorithm and `VAL`/`INPUT`
round the digits back correctly, so a value survives a print-and-read round trip;
both run in exact integer arithmetic and give identical output on every target.
`INPUT` reads one line and parses a value per designator. BASIDE's
`INK`/`PAPER` print items set display colours and belong to its console module,
so they are not part of core BASIC.

## Expressions

Precedence runs from lowest (`OR`) to highest (`^` and primary terms). Each
production handles one level, giving left-associative binary operators; `NOT`,
unary sign, and `^` associate to the right.

```ebnf
Expression = AndExpr { 'OR' AndExpr } .
AndExpr    = NotExpr { 'AND' NotExpr } .
NotExpr    = 'NOT' NotExpr | RelExpr .
RelExpr    = AddExpr [ RelOp AddExpr ] .
AddExpr    = MulExpr { AddOp MulExpr } .
MulExpr    = SignExpr { MulOp SignExpr } .
SignExpr   = [ '+' | '-' | 'BNOT' ] PowExpr .
PowExpr    = Primary [ '^' SignExpr ] .
Primary    = Literal | '(' Expression ')' | Designator .

Designator = IDENTIFIER { Selector } .
Selector   = '(' [ ExprList ] ')'      (* array or vector index, or call args *)
           | '.' IDENTIFIER .          (* record field *)

RelOp = '=' | '<>' | '<' | '>' | '<=' | '>=' .
AddOp = '+' | '-' .            (* '+' also concatenates STRING operands *)
MulOp = '*' | '/' | 'MOD' | 'DIV'
      | 'BAND' | 'BOR' | 'BXOR' | 'SHL' | 'SHR' .
```

`=` is assignment only in statement position; inside an expression it is the
equality operator. `DIV` is integer division; `/` produces a `FLOAT`.

| Level | Operators | Associativity |
|-------|-----------|---------------|
| 1 (lowest) | `OR` | left |
| 2 | `AND` | left |
| 3 | `NOT` | right (prefix) |
| 4 | `=` `<>` `<` `>` `<=` `>=` | non-associative |
| 5 | `+` `-` | left |
| 6 | `*` `/` `MOD` `DIV` `BAND` `BOR` `BXOR` `SHL` `SHR` | left |
| 7 | unary `+` `-` `BNOT` | right (prefix) |
| 8 (highest) | `^` | right |

`AND`, `OR`, `NOT` are logical and short-circuit. A comparison yields 1 or 0.
`BAND`, `BOR`, `BXOR`, `SHL`, `SHR`, and prefix `BNOT` work on the bits of an
`INT` and bind at the multiplicative level. `BNOT x` is `x BXOR -1`.

### Multiple assignment

`a, b = e1, e2` assigns several scalar or `STRING` places at once. The right-hand
side is an expression list in which a call to an `FN` returning several values
counts for each of them, so `lo, hi = minmax(x, y)` balances. Every right-hand
value is computed before any store, so `x, y = y, x` swaps.

## Core built-in functions

The core library is available without any import: the math functions `ABS`,
`SGN`, `SQR`, `SIN`, `COS`, `TAN`, `ATN`, `ATN2`, `SIND`, `COSD`, `TAND`,
`FLOOR`, `CEIL`, `ROUND`, `MIN`, `MAX`, `CLAMP`, `LERP`, `RND`, `SEED`, `TIME`,
the conversions `INT`, `FLOAT`, `VAL`, `VALINT`, `ASC`, and the string functions
`LEN`, `LEFT$`, `RIGHT$`, `MID$`, `INSTR`, `CHR$`, `STR$`, `UCASE$`, `LCASE$`,
`SPACE$`, `STRING$`, `HEX$`, `BIN$`, and the formatting helpers `FIX$`, `ZERO$`,
`RJUST$`, `LJUST$`. A `$`-suffixed name returns a `STRING`.

(The current `skj-bc` implements the string functions `LEN`, `ASC`, `INSTR`,
`CHR$`, `STR$`, `LEFT$`, `RIGHT$`, `MID$`, `UCASE$`, and `LCASE$`; the numeric
built-ins `ABS`, `SGN`, `MIN`, `MAX`, `CLAMP`, `FLOOR`, `CEIL`, `ROUND`,
`LERP`, `SQR`, and the trigonometric functions `SIN`, `COS`, `TAN`, `ATN`,
`ATN2`, `SIND`, `COSD`, and `TAND`; and the conversions `INT` and `FLOAT`.
`ABS` keeps its argument's type, `SGN` yields an `INT`, and `MIN`, `MAX`,
`CLAMP`, and `LERP` promote to `FLOAT` when any argument is a `FLOAT`. `INT`,
`FLOOR`, `CEIL`, and `ROUND` yield an `INT`: `INT` truncates toward zero,
`ROUND` rounds halves away from zero, and all four are exact over the signed
32-bit range. `SQR` and the trigonometric functions return a `FLOAT`; the runtime
computes them without libm. `SQR` is correctly rounded, while the trigonometric
functions use short polynomials accurate to about seven to ten significant
digits, so the shortest-round-trip printer shows that limit in the trailing
digits. The results are identical on every target. The string and formatting helpers
`SPACE$`, `STRING$`, `HEX$`, `BIN$`, `FIX$`, `ZERO$`, `RJUST$`, and `LJUST$`
are implemented, along with the conversions `VAL` (to `FLOAT`) and `VALINT`
(to `INT`), which parse a leading number and ignore any trailing text.
`STRING$(n, s)` repeats s n times; `HEX$` and `BIN$` render the 64-bit
unsigned bit pattern; `FIX$(x, d)` gives d decimal places; `ZERO$(n, w)`
zero-pads to width w with the sign ahead of the zeros; `RJUST$`/`LJUST$` pad
with spaces and return the string unchanged when it already fills the width.
`RND` returns a `FLOAT` in [0, 1) from a linear congruential generator,
auto-seeded from the kernel on first use; `SEED(n)` resets that generator and
returns n, so a program reseeds with a throwaway assignment; `TIME` returns
monotonic-clock seconds as a `FLOAT`, so programs take differences to measure
elapsed time. `RND` and `TIME` may be written bare, without parentheses. All
the core built-in functions are now implemented. An `INSTR` match is 1-based
and 0 means not found; `MID$` takes a 1-based start and an optional length that
defaults to the rest of the string.)

## Constants

`PI` is a predeclared `FLOAT` constant. A program may define its own with `CONST`
and `ENUM`.

## Lexical elements

```ebnf
IDENTIFIER      = LETTER { LETTER | DIGIT | '_' } .     (* not a reserved word *)

INTEGER_LITERAL = DEC_DIGITS
                | ( '0x' | '0X' ) HEX_DIGIT { HEX_DIGIT | '_' }
                | ( '0b' | '0B' ) BIN_DIGIT { BIN_DIGIT | '_' } .

FLOAT_LITERAL   = DEC_DIGITS '.' { DIGIT | '_' } [ Exponent ]
                | DEC_DIGITS Exponent .
Exponent        = ( 'E' | 'e' ) [ '+' | '-' ] DEC_DIGITS .
DEC_DIGITS      = DIGIT { DIGIT | '_' } .   (* '_' separates digits, ignored in value *)

STRING_LITERAL  = '"' { CHAR | '""' } '"' .  (* '""' encodes one '"' character *)

Literal         = INTEGER_LITERAL | FLOAT_LITERAL | STRING_LITERAL .
LETTER          = 'A'..'Z' | 'a'..'z' .
DIGIT           = '0'..'9' .
```

A `BYTE` has no distinct literal; an `INTEGER_LITERAL` in 0..255 assigns directly
to a `BYTE`.

A `$` may suffix the built-in function names that return a `STRING` (`LEFT$`,
`MID$`, and the rest); it is part of those reserved spellings, not an identifier
sigil, so ordinary variables carry no sigil.

## Comments and layout

```ebnf
Comment = 'REM' { CHAR } NEWLINE | "'" { CHAR } NEWLINE .
```

`REM` starts a comment statement to end of line; `'` starts a comment anywhere a
statement could begin. A comment always extends to the end of the physical line.

## Reserved words

The keywords, type names, and built-in operation names are reserved and cannot be
used as identifiers:

```
ABS     AND     APPEND  AS      ASC     AT      ATN     ATN2
BAND    BIN$    BNOT    BOR     BXOR    BYTE
CASE    CEIL    CHR$    CLAMP   CLEAR   CONST   COS     COSD    COUNT
DECLARE DIM     DIV     EACH    ELSE    ELSEIF  END     ENDCASE ENDENUM ENDFN
ENDIF   ENDPROC ENDTYPE ENDWHILE ENUM   EXIT
FIX$    FLOAT   FLOOR   FN      FOR     HEX$    IF      IN      INPUT
INSTR   INT     LCASE$  LEFT$   LEN     LERP    LJUST$
MAX     MID$    MIN     MOD     NEXT    NOT     OF      OR      PRINT   PROC
REF     REM     REMOVE  REPEAT  RETURN  RIGHT$  RJUST$  RND     ROUND
SEED    SGN     SHL     SHR     SIN     SIND    SPACE$  SQR     STEP    STR$
STRING  STRING$ TAB     TAN     TAND    THEN    TIME    TO      TYPE
UCASE$  UNTIL   VAL     VALINT  VECTOR  WAIT    WHEN    WHILE   ZERO$
MODULE  EXPORT  IMPORT  FROM    EXTERNAL
```

`MODULE`, `EXPORT`, `IMPORT`, `FROM`, and `EXTERNAL` are the module-system
keywords; the module grammar, the `EXTERNAL` C-binding clause, and the on-disk
interface are specified in `basic-modules.md`.

The words of BASIDE's display, console, controls, sound, card, asset, timer, and
matrix statement libraries, and of its game extension modules, are not reserved
here: skjegg's BASIC is the core language, and those statement families are out
of scope, so names like `line`, `box`, `map`, `sprite`, and `sound` are ordinary
identifiers.

## Lookahead profile

The grammar is LL(1) except for three points, each resolved with one extra token
of lookahead and no backtracking:

- `IfStmt`: after `THEN`, a `NEWLINE` selects the block form, anything else the
  single-line form.
- `FOR`: `EACH` after `FOR` selects `ForEachStmt`, an identifier selects
  `ForStmt`.
- `AssignOrCall` and the `Designator` inside an expression: the parser accepts
  the designator, then branches on the following `=` or `,`, and semantic
  analysis distinguishes an array or vector element from a call using the symbol
  table.

## Example

```basic
DIM score AS INT
DIM name AS STRING
DIM board(8, 8) AS BYTE

CONST WIDTH = 320, HEIGHT = 240

PROC clearboard()
    DIM x AS INT : DIM y AS INT
    FOR y = 0 TO 7
        FOR x = 0 TO 7
            board(x, y) = 0
        NEXT x
    NEXT y
ENDPROC

FN scaled(v AS INT) AS INT
    RETURN v * WIDTH DIV 100
ENDFN

clearboard()
score = 0
PRINT "Player: "; name

IF score > 0 THEN PRINT "You scored "; score ELSE PRINT "No score"

CASE score
    WHEN 0
        PRINT "none"
    WHEN 1, 2, 3
        PRINT "low"
    ELSE
        PRINT "high"
ENDCASE

END
```
