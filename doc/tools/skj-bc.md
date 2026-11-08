# skj-bc: the BASIC compiler manual

`skj-bc` compiles the skjegg dialect of structured BASIC to native code through
the shared IR, the same path the other front ends take. This manual covers how
to invoke the compiler, how to turn a `.bas` source into a running program, a
tour of the language with worked examples, a reference for the built-in
functions, and a consolidated grammar.

The normative language definition is `doc/languages/basic.md`. This manual is
the user-facing companion: where the specification states the grammar and the
rules, this document shows how to use the tool and the language. Where the two
differ, the specification wins.

## Overview

- Source files use the `.bas` extension.
- The language is fully structured: no line numbers, no `GOTO`.
- There are four value types, `INT`, `FLOAT`, `BYTE`, and `STRING`, plus records
  and fixed arrays.
- Keywords and identifiers are case-insensitive. `PRINT`, `Print`, and `print`
  are the same keyword, and `count` and `Count` are the same variable.
- The compiler targets ColdFire/m68k. Programs run under `qemu-m68k`.

## Invoking the compiler

```
skj-bc [options] <file.bas>
```

| Option | Meaning |
|--------|---------|
| `-o <file>` | Write the assembly to `<file>`. Without it, the assembly goes to standard output. |
| `-V` | Print the version and exit. |
| `-h`, `--help` | Print the usage and exit. |
| `--tokens` | Dump the token stream and exit, for debugging the lexer. |

`skj-bc` emits GAS-syntax ColdFire assembly. It does not assemble or link; those
are separate steps. A complete build runs the compiler, the assembler, and the
linker, then links the BASIC runtime and the program entry stub.

### From source to a running program

The toolchain has a built-in assembler (`skj-as`) and linker (`skj-ld`) for
ColdFire, so no external binutils are required to produce a binary. The runtime
(`runtime/basic_rt.c`) is cross-compiled once with `m68k-linux-gnu-gcc`.

```sh
# 1. compile the BASIC source to assembly
./build/skj-bc -o build/prog.s prog.bas

# 2. assemble the program and the entry stub
./build/skj-as -o build/prog.o  build/prog.s
./build/skj-as -o build/start.o runtime/start.S

# 3. build the runtime once (reusable across programs)
m68k-linux-gnu-gcc -std=c99 -O2 -Wall -ffreestanding \
    -c -o build/basic_rt.o runtime/basic_rt.c

# 4. link the entry stub, the runtime, and the program
./build/skj-ld -o build/prog build/start.o build/basic_rt.o build/prog.o

# 5. run it
qemu-m68k ./build/prog
```

The program's exit status is its process exit code. `END` exits with 0.

In the skjegg tree, the test harness wires this up for you: a file placed at
`tests/basic_tNNN_name.bas` builds and runs under `make check`, compared against
`tests/basic_tNNN_name.expected` (expected standard output) and an optional
`tests/basic_tNNN_name.exitcode`.

## A tour of the language

### Hello, output

```basic
PRINT "Hello, world"
END
```

`PRINT` writes its items to the console. A `;` joins items with no space, a `,`
separates them with a space, and a trailing separator suppresses the newline.

```basic
PRINT "x = "; 42          ' prints: x = 42
PRINT "a", "b"            ' prints: a b
PRINT "no newline here";  ' the next PRINT continues the line
```

A comment starts with `REM` or a single quote and runs to the end of the line.

### Variables and types

Variables are declared with `DIM name AS type`. There are no type sigils.

```basic
DIM score AS INT          ' 64-bit signed integer
DIM ratio AS FLOAT        ' IEEE 754 double
DIM flags AS BYTE         ' 0..255, promotes to INT in arithmetic
DIM name  AS STRING       ' dynamic string

score = 10 * 4
ratio = 3.5
name  = "player one"
```

`INT` arithmetic is 64-bit. `/` always produces a `FLOAT`; `DIV` is integer
division and `MOD` is the integer remainder. An `INT` promotes to `FLOAT` when
the other operand is a `FLOAT`.

```basic
PRINT 7 / 2               ' 3.5   (FLOAT division)
PRINT 7 DIV 2             ' 3     (integer division)
PRINT 7 MOD 2             ' 1
```

Named constants come from `CONST` and `ENUM`. `PI` is predeclared.

```basic
CONST WIDTH = 320, HEIGHT = 240
ENUM SKY, GRASS, DIRT             ' 0, 1, 2
ENUM RED = 10, GREEN, BLUE        ' 10, 11, 12
```

### Control flow

```basic
IF score > 100 THEN PRINT "high" ELSE PRINT "low"      ' single line

IF score > 100 THEN                                    ' block form
    PRINT "high"
ELSEIF score > 50 THEN
    PRINT "medium"
ELSE
    PRINT "low"
ENDIF

WHILE score > 0
    score = score - 1
ENDWHILE

REPEAT
    score = score + 1
UNTIL score >= 10

FOR i = 0 TO 9
    PRINT i
NEXT i

FOR i = 10 TO 0 STEP -2
    PRINT i
NEXT

CASE score
    WHEN 0
        PRINT "none"
    WHEN 1, 2, 3
        PRINT "low"
    WHEN 4 TO 9
        PRINT "mid"
    WHEN >= 10
        PRINT "high"
    ELSE
        PRINT "other"
ENDCASE
```

`EXIT WHILE`, `EXIT REPEAT`, and `EXIT FOR` leave the nearest enclosing loop of
that kind.

### Procedures and functions

A `PROC` runs for its effect and returns nothing. An `FN` returns a value.
Parameters are typed. Variables declared inside a routine are local to it.

```basic
PROC greet(who AS STRING)
    PRINT "Hello, "; who
ENDPROC

FN square(x AS INT) AS INT
    RETURN x * x
ENDFN

greet("world")
PRINT square(9)           ' 81
```

A routine must be defined before it is called. For mutual recursion, give the
signature ahead of the body with `DECLARE`.

```basic
DECLARE FN is_odd(n AS INT) AS INT

FN is_even(n AS INT) AS INT
    IF n = 0 THEN RETURN 1
    RETURN is_odd(n - 1)
ENDFN

FN is_odd(n AS INT) AS INT
    IF n = 0 THEN RETURN 0
    RETURN is_even(n - 1)
ENDFN
```

An `FN` may return several values, used where each value has a place:

```basic
FN minmax(a AS INT, b AS INT) AS INT, INT
    IF a < b THEN RETURN a, b
    RETURN b, a
ENDFN

DIM lo AS INT : DIM hi AS INT
lo, hi = minmax(8, 3)     ' lo = 3, hi = 8
```

### Arrays

`DIM name(bounds) AS type` declares a fixed array. Bounds are constant
expressions; indices run from 0 to the bound minus one.

```basic
DIM row(8) AS INT             ' 8 elements, row(0) .. row(7)
DIM grid(8, 8) AS BYTE        ' two-dimensional

DIM i AS INT
FOR i = 0 TO 7
    row(i) = i * i
NEXT i

FOR EACH v IN row             ' walk every element in order
    PRINT v
NEXT
```

`FOR EACH v IN array` binds `v` to each element in turn. `v` is a reference, so
assigning to it inside the loop updates the array element.

### Strings

Strings are dynamic. `+` concatenates. The comparison operators order strings
by their bytes.

```basic
DIM a AS STRING
DIM b AS STRING
a = "foo"
b = "bar"
PRINT a + b               ' foobar
PRINT LEN(a + b)          ' 6
PRINT UCASE$(a)           ' FOO
PRINT MID$("abcdef", 2, 3)' bcd    (1-based start)
IF a > b THEN PRINT "foo sorts after bar"
```

### Records

`TYPE ... ENDTYPE` groups named fields. A record of scalars is a value, copied on
assignment. A record parameter is passed by value unless marked `REF`.

```basic
TYPE Point
    x AS INT
    y AS INT
    name AS STRING
ENDTYPE

PROC shift(REF p AS Point, dx AS INT, dy AS INT)
    p.x = p.x + dx
    p.y = p.y + dy
ENDPROC

DIM a AS Point
a.x = 3 : a.y = 4 : a.name = "origin"

DIM b AS Point
b = a                     ' copies the whole record
b.x = 100                 ' does not touch a.x

shift(a, 10, 20)          ' REF: updates a in place
PRINT a.x; " "; a.y       ' 13 24
```

### Input

`INPUT` reads a line and parses one value per target.

```basic
DIM n AS INT
INPUT "how many? "; n
PRINT "you said "; n
```

### Timing

`WAIT ms` pauses for the given number of milliseconds. `TIME` returns
monotonic-clock seconds as a `FLOAT`, so a program measures elapsed time by
taking a difference.

```basic
DIM t0 AS FLOAT
t0 = TIME
WAIT 500
PRINT "waited "; TIME - t0; " seconds"
```

## Built-in function reference

The core library is always available. A `$`-suffixed name returns a `STRING`.
The `$` is part of the reserved spelling, not an identifier sigil, so ordinary
variables never carry one.

### Numeric

| Function | Result | Notes |
|----------|--------|-------|
| `ABS(x)` | same as `x` | Absolute value; keeps `INT` or `FLOAT`. |
| `SGN(x)` | `INT` | -1, 0, or 1. |
| `MIN(a, b)` | numeric | `FLOAT` if either argument is a `FLOAT`. |
| `MAX(a, b)` | numeric | As `MIN`. |
| `CLAMP(x, lo, hi)` | numeric | `x` held to `[lo, hi]`; `FLOAT` if any argument is. |
| `LERP(a, b, t)` | `FLOAT` | `a + (b - a) * t`. |
| `FLOOR(x)` | `INT` | Round down. |
| `CEIL(x)` | `INT` | Round up. |
| `ROUND(x)` | `INT` | Round halves away from zero. |
| `SQR(x)` | `FLOAT` | Square root; a non-positive argument gives 0. |

`INT`, `FLOOR`, `CEIL`, and `ROUND` are exact over the signed 32-bit range,
matching the `FLOAT` print limit.

### Trigonometry

Angles are in radians, except the `D`-suffixed forms, which take degrees. All
return a `FLOAT`. The runtime computes them without libm, accurate past the six
decimal places `PRINT` shows.

| Function | Result | Notes |
|----------|--------|-------|
| `SIN(x)`, `COS(x)`, `TAN(x)` | `FLOAT` | Radians. |
| `ATN(x)` | `FLOAT` | Arctangent. |
| `ATN2(y, x)` | `FLOAT` | Arctangent of `y/x`, resolved to the correct quadrant. |
| `SIND(d)`, `COSD(d)`, `TAND(d)` | `FLOAT` | Degrees. |

### Conversions

| Function | Result | Notes |
|----------|--------|-------|
| `INT(x)` | `INT` | Truncate a `FLOAT` toward zero. |
| `FLOAT(x)` | `FLOAT` | Widen an `INT`. |
| `VAL(s)` | `FLOAT` | Parse a leading number; trailing text is ignored. |
| `VALINT(s)` | `INT` | Parse a leading integer. |
| `ASC(s)` | `INT` | Code of the first character. |

### Strings

| Function | Result | Notes |
|----------|--------|-------|
| `LEN(s)` | `INT` | Length in bytes. |
| `INSTR(haystack, needle)` | `INT` | 1-based position, 0 if not found. |
| `CHR$(n)` | `STRING` | One character of code `n`. |
| `STR$(x)` | `STRING` | A number as text. |
| `LEFT$(s, n)` | `STRING` | The first `n` characters. |
| `RIGHT$(s, n)` | `STRING` | The last `n` characters. |
| `MID$(s, start [, len])` | `STRING` | Substring; 1-based `start`, `len` defaults to the rest. |
| `UCASE$(s)`, `LCASE$(s)` | `STRING` | Change case. |
| `SPACE$(n)` | `STRING` | `n` spaces. |
| `STRING$(n, s)` | `STRING` | `s` repeated `n` times. |
| `HEX$(n)`, `BIN$(n)` | `STRING` | The 64-bit unsigned bit pattern in base 16 or 2. |
| `FIX$(x, d)` | `STRING` | `x` with exactly `d` decimal places. |
| `ZERO$(n, w)` | `STRING` | `n` zero-padded to width `w`, sign ahead of the zeros. |
| `RJUST$(s, w)`, `LJUST$(s, w)` | `STRING` | Space-pad to width `w`; a string that already fills `w` is returned unchanged. |

### Random and time

| Function | Result | Notes |
|----------|--------|-------|
| `RND` | `FLOAT` | A value in `[0, 1)`. Auto-seeded from the kernel on first use. |
| `SEED(n)` | `INT` | Reset the generator and return `n`. Assign it to a throwaway to reseed. |
| `TIME` | `FLOAT` | Monotonic-clock seconds; take differences for elapsed time. |

`RND` and `TIME` may be written bare, without parentheses.

### Constant

`PI` is a predeclared `FLOAT`.

## Runtime and limits

- The runtime (`runtime/basic_rt.c`) is freestanding over the shared syscall
  seam, with no libc or libm. It carries its own string operations, number
  formatting, software square root and trigonometry, a random generator, and a
  clock.
- `PRINT` renders a `FLOAT` with a fixed six-place decimal, trailing zeros
  trimmed. The shortest-round-trip form is not yet implemented.
- `INT`, `FLOOR`, `CEIL`, `ROUND`, and the integer part printed for a `FLOAT`
  are exact over the signed 32-bit range. A larger magnitude is out of range.
- `HEX$` and `BIN$` render the full 64-bit unsigned bit pattern, so a negative
  argument prints its two's-complement form.
- Not yet implemented: the `VECTOR` growable-list type, the `AT` and `TAB`
  console cursor positioning items, and the mapped-array `DIM ... AT addr` form.

## Grammar

This is the collected grammar, in the EBNF the specification uses. See
`doc/languages/basic.md` for the per-production notes and the authoritative
version.

```ebnf
Program     = { ProgramItem } .
ProgramItem = TypeDecl | ProcDecl | FnDecl | ForwardDecl | Statement .

TypeDecl    = 'TYPE' IDENTIFIER { Field } 'ENDTYPE' .
Field       = IDENTIFIER [ '(' ConstExprList ')' ] 'AS' Type ( ':' | NEWLINE ) .

ProcDecl    = 'PROC' IDENTIFIER '(' [ ParamList ] ')' StatementList 'ENDPROC' .
FnDecl      = 'FN' IDENTIFIER '(' [ ParamList ] ')' 'AS' RetList
              StatementList 'ENDFN' .
ForwardDecl = 'DECLARE' ( 'PROC' IDENTIFIER '(' [ ParamList ] ')'
                        | 'FN' IDENTIFIER '(' [ ParamList ] ')' 'AS' RetList ) .
RetList     = Type { ',' Type } .
ParamList   = Param { ',' Param } .
Param       = [ 'REF' ] IDENTIFIER [ '(' ConstExprList ')' ] 'AS' Type .

Type        = ScalarType | VectorType | RecordType .
ScalarType  = 'INT' | 'FLOAT' | 'BYTE' | 'STRING' .
VectorType  = 'VECTOR' 'OF' Type .
RecordType  = IDENTIFIER .

StatementList = { Statement Terminator } .
Terminator    = ':' | NEWLINE .
Statement     = [ SimpleStmt | CompoundStmt ] .

SimpleStmt  = AssignOrCall
            | DimStmt | ConstStmt | EnumStmt
            | ReturnStmt | ExitStmt
            | PrintStmt | InputStmt
            | WaitStmt
            | 'END' .
CompoundStmt = IfStmt | WhileStmt | RepeatStmt | ForStmt | ForEachStmt | CaseStmt .

AssignOrCall = Designator [ { ',' Designator } '=' ExprList ] .

DimStmt     = 'DIM' DimItem { ',' DimItem } .
DimItem     = IDENTIFIER [ '(' ConstExprList ')' ] 'AS' Type .
ConstStmt   = 'CONST' IDENTIFIER '=' ConstExpr { ',' IDENTIFIER '=' ConstExpr } .
EnumStmt    = 'ENUM' EnumItem { ',' EnumItem }
            | 'ENUM' NEWLINE { EnumItem { ',' EnumItem } NEWLINE } 'ENDENUM' .
EnumItem    = IDENTIFIER [ '=' ConstExpr ] .

ReturnStmt  = 'RETURN' [ ExprList ] .
ExitStmt    = 'EXIT' ( 'WHILE' | 'REPEAT' | 'FOR' ) .
WaitStmt    = 'WAIT' Expression .

IfStmt      = 'IF' Expression 'THEN' ( SingleLineTail | BlockTail ) .
SingleLineTail = Statement [ 'ELSE' Statement ] .
BlockTail      = NEWLINE StatementList
                 { 'ELSEIF' Expression 'THEN' NEWLINE StatementList }
                 [ 'ELSE' NEWLINE StatementList ]
                 'ENDIF' .

WhileStmt   = 'WHILE' Expression NEWLINE StatementList 'ENDWHILE' .
RepeatStmt  = 'REPEAT' NEWLINE StatementList 'UNTIL' Expression .
ForStmt     = 'FOR' IDENTIFIER '=' Expression 'TO' Expression
              [ 'STEP' Expression ] NEWLINE StatementList 'NEXT' [ IDENTIFIER ] .
ForEachStmt = 'FOR' 'EACH' IDENTIFIER 'IN' Designator NEWLINE
              StatementList 'NEXT' [ IDENTIFIER ] .

CaseStmt    = 'CASE' Expression NEWLINE
              { 'WHEN' WhenItem { ',' WhenItem } NEWLINE StatementList }
              [ 'ELSE' NEWLINE StatementList ]
              'ENDCASE' .
WhenItem    = Expression [ 'TO' Expression ] | RelOp Expression .

PrintStmt   = 'PRINT' [ PrintItem { PrintSep PrintItem } [ PrintSep ] ] .
PrintItem   = Expression
            | 'AT' Expression ',' Expression
            | 'TAB' '(' Expression ')' .
PrintSep    = ',' | ';' .
InputStmt   = 'INPUT' [ STRING_LITERAL ';' ] Designator { ',' Designator } .

Expression  = AndExpr { 'OR' AndExpr } .
AndExpr     = NotExpr { 'AND' NotExpr } .
NotExpr     = 'NOT' NotExpr | RelExpr .
RelExpr     = AddExpr [ RelOp AddExpr ] .
AddExpr     = MulExpr { AddOp MulExpr } .
MulExpr     = SignExpr { MulOp SignExpr } .
SignExpr    = [ '+' | '-' | 'BNOT' ] PowExpr .
PowExpr     = Primary [ '^' SignExpr ] .
Primary     = Literal | '(' Expression ')' | Designator .

Designator  = IDENTIFIER { Selector } .
Selector    = '(' [ ExprList ] ')' | '.' IDENTIFIER .
ExprList    = Expression { ',' Expression } .
ConstExprList = ConstExpr { ',' ConstExpr } .

RelOp = '=' | '<>' | '<' | '>' | '<=' | '>=' .
AddOp = '+' | '-' .
MulOp = '*' | '/' | 'MOD' | 'DIV' | 'BAND' | 'BOR' | 'BXOR' | 'SHL' | 'SHR' .

Comment = 'REM' { CHAR } NEWLINE | "'" { CHAR } NEWLINE .
```

Operator precedence runs from lowest to highest: `OR`; `AND`; prefix `NOT`; the
relational operators (non-associative); `+` and `-`; the multiplicative and
bitwise operators; unary `+`, `-`, and `BNOT`; and `^`, which associates to the
right. `AND`, `OR`, and `NOT` are logical and short-circuit, and a comparison
yields 1 or 0.

### Lexical elements

```ebnf
IDENTIFIER      = LETTER { LETTER | DIGIT | '_' } .
INTEGER_LITERAL = DEC_DIGITS
                | ( '0x' | '0X' ) HEX_DIGIT { HEX_DIGIT | '_' }
                | ( '0b' | '0B' ) BIN_DIGIT { BIN_DIGIT | '_' } .
FLOAT_LITERAL   = DEC_DIGITS '.' { DIGIT | '_' } [ Exponent ]
                | DEC_DIGITS Exponent .
Exponent        = ( 'E' | 'e' ) [ '+' | '-' ] DEC_DIGITS .
DEC_DIGITS      = DIGIT { DIGIT | '_' } .
STRING_LITERAL  = '"' { CHAR | '""' } '"' .
```

An underscore separates digit groups and is ignored in the value. A `""` inside
a string literal encodes one `"` character. A `BYTE` has no distinct literal; an
integer literal in 0..255 assigns directly to a `BYTE`.
