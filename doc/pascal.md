# Compact Pascal Language Reference (skj-pc)

`skj-pc` implements a subset of Compact Pascal targeting the Skjegg
shared IR.  This document records what is and is not supported.

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
| `set of T` | supported |
| enumerated types | partial (constants only, not distinct types) |
| `real` / floating point | **not implemented** |
| pointer types (`^T`) | **not implemented** |
| file types (`file of T`, `text`) | **not implemented** |
| subrange types (`1..100`) | **not implemented** as standalone declarations |
| procedural types | **not implemented** |

Subrange syntax works in array bounds, set definitions, and case
ranges, but cannot be used as a standalone type declaration.

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
| nested procedures with static links | supported |
| typed constants (scalar, array, matrix, record, set) | supported |
| `label` | **not implemented** |
| `uses` / `unit` | **not implemented** |
| `external` | **not implemented** |

## Parameters

| Form | Status |
|------|--------|
| value parameters | supported |
| `var` parameters (by reference) | supported |
| `const` parameters | supported |
| array parameters | supported |
| record parameters | supported |
| string parameters | supported |
| procedural parameters | **not implemented** |

## Statements

| Statement | Status |
|-----------|--------|
| assignment (`:=`) | supported |
| `if...then...else` | supported |
| `while...do` | supported |
| `for...to/downto...do` | supported |
| `repeat...until` | supported |
| `case...of...end` (with `else`, ranges) | supported |
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
| Address-of: `@` | **no** |
| Pointer dereference: `^` | **no** |

## Built-in Procedures and Functions

### I/O

| Name | Status |
|------|--------|
| `write(args...)` | supported (integer, char, boolean, string) |
| `writeln(args...)` | supported |
| `write(x:width)`, `write(x:width:decimals)` | supported |
| `read(vars...)` | supported |
| `readln(vars...)` | supported |
| `eof` | supported |
| `eoln` | **not implemented** |
| `assign`, `reset`, `rewrite`, `close` | **not implemented** (no file types) |
| `ioresult` | **not implemented** |

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
| `sqrt(x)` | **not implemented** |
| `sin(x)`, `cos(x)` | **not implemented** |
| `trunc(x)`, `round(x)` | **not implemented** |
| `random(n)`, `randomize` | **not implemented** |
| `high(T)`, `low(T)` | **not implemented** |
| `paramcount`, `paramstr(n)` | **not implemented** |

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
| `new(ptr)` | **not implemented** (no pointers) |
| `dispose(ptr)` | **not implemented** (no pointers) |
| `getmem(ptr, size)` | **not implemented** |
| `freemem(ptr)` | **not implemented** |
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

4. **Modules** (`uses`, `unit`) -- requires separate compilation and
   a symbol import/export mechanism.
