# TinScheme language specification

TinScheme is a small Scheme subset compiled statically to native code through
the shared IR, the same path the other front ends take (`lex.c`, `parse.c`,
`lower.c`, `main.c`). The compiler is `skj-sc`; source files use the `.scm`
extension. Per-backend variants cover the native targets (`skj-sc-rv`,
`skj-sc-mips`, `skj-sc-x86-64`, `skj-sc-arm64`) and WebAssembly (`skj-sc-wasm`,
which emits a complete WASI module with the runtime inlined, no separate link
step). The WASM build also self-hosts: `skj-cc-wasm` and `skj-ld-wasm` compile
TinScheme's own sources into a `skj-sc.wasm` that runs in the browser playground
(see `pages/playground`).

TinScheme is a subset aimed at teaching the toolchain's closure and delimited
continuation machinery, not a full R7RS implementation. It has first-class
procedures with lexical capture, garbage-collected pairs, `shift`/`reset`
delimited continuations, stdout printing, and the common binding, control, and
numeric forms. The deliberate omissions are listed at the end.

## Value model

A value is a single tagged machine word. The two low bits are the tag:

| Tag bits | Kind         | Representation                          |
|----------|--------------|-----------------------------------------|
| `01`     | fixnum       | the integer shifted left two bits        |
| `10`     | immediate    | `#f` = `2`, `#t` = `6`                    |
| `00`     | heap pointer | aligned address; `0` is the empty list   |

Pairs, closures, and interned symbols are heap objects. Pairs and closures are
bump-allocated from a fixed heap; the empty list `()` is the null pointer. The
tag arithmetic stays 32-bit even on the LP64 backends (x86-64, AArch64), where
the pointer word is 64 bits wide but the upper half of a fixnum or immediate is
zero.

Only `#f` is false. Every other value, including `0`, the empty list, and `0.0`
if it existed, is true in a conditional.

## Program structure

A program is a sequence of top-level forms. A top-level `define` introduces a
global; every other form is an expression, and the value of the last top-level
expression becomes the process exit code (after untagging). Printing with
`display` is the normal way to produce output; the exit-code convention predates
it and is kept for the test harness.

```scheme
(define (square x) (* x x))
(display (square 9)) (newline)   ; prints 81
```

### define

```scheme
(define (name arg ...) body ...)   ; a procedure
(define name init-expr)            ; a global bound to a value
```

A procedure `define` may be recursive. Mutual recursion works when each callee
is defined before the call site is evaluated at run time, which top-level order
guarantees.

## Expressions

### Procedures and application

`lambda` builds a closure that captures its free variables lexically:

```scheme
(define (adder n) (lambda (x) (+ x n)))
(display ((adder 10) 5)) (newline)   ; 15
```

Application is the usual `(operator operand ...)`.

### Arithmetic and comparison

Binary `+`, `-`, `*` and the comparisons `=`, `<`, `>`, `<=`, `>=` operate on
fixnums. The comparisons return `#t` or `#f`.

### Numeric primitives

- `(quotient a b)` and `(remainder a b)` truncate toward zero.
- `(modulo a b)` floors; its result takes the sign of the divisor.
- `(abs x)` is the absolute value.
- `(min a b)` and `(max a b)` are the two-argument minimum and maximum.

```scheme
(display (modulo -17 5)) (newline)     ; 3
(display (remainder -17 5)) (newline)  ; -2
(display (max 3 8)) (newline)          ; 8
```

### Binding and control forms

- `(let ((v init) ...) body ...)` binds in parallel; the inits see the outer
  scope. `(let* ...)` binds sequentially; each init sees the earlier bindings.
- `(if test then else)` and `(begin expr ...)`.
- `(cond (test body ...) ... (else body ...))`.
- `(when test body ...)` and `(unless test body ...)`.
- `(and expr ...)` and `(or expr ...)` short-circuit and return the deciding
  value. `(not x)` returns `#t` when `x` is `#f`, else `#f`.

```scheme
(let ((a 2) (b 3))
  (display (cond ((> a b) 'bigger)
                 ((< a b) (* a b))
                 (else 0)))
  (newline))                           ; 6
```

### Pairs and lists

`cons`, `car`, `cdr`, `null?`, `pair?`, `set-car!`, `set-cdr!`, and `list`
build and inspect pairs. Pairs are garbage-collected.

### Quotation

`(quote datum)`, written `'datum`, produces the datum as runtime data:

- a quoted number, boolean, or `()` is the corresponding constant;
- a quoted pair or list builds runtime cons cells;
- a quoted symbol is an interned object, one per distinct name, so every
  occurrence of the same symbol shares one identity.

```scheme
(display (car '(10 20 30))) (newline)        ; 10
(display (if (eq? 'red 'red) 1 0)) (newline) ; 1
```

`(eq? a b)` tests identity: value equality for immediates, and object identity
for symbols and pairs.

### Printing

- `(display x)` writes a fixnum, boolean, nil, or a string literal to stdout.
  A fixnum is formatted in decimal; `#t`, `#f`, and `()` print as themselves; a
  heap object other than a string literal prints a placeholder.
- `(newline)` writes a line feed.
- `(write-string "literal")` writes the bytes of a string literal.

Printing works over a string *literal* argument, not a first-class runtime
string value; see the omissions below.

### Delimited continuations

`reset`, `shift`, and `resume` provide delimited continuations. On WASM they
lower to the stack-switching proposal; see `doc/internals/wasm-continuations.md`
and `doc/internals/closures.md`.

## Omissions

These are intentionally out of scope in this version:

- First-class runtime strings and string operations. String *literals* work only
  as arguments to the printing forms.
- Generic recursive `display` of pairs, lists, and symbols. Displaying a symbol
  or a pair prints a placeholder.
- `define` inside a body, variadic `lambda`, named `let`, and vectors.
- Floating-point numbers, `read`, and ports.

## Playground

`make playground` builds the self-hosted `skj-sc.wasm` and serves a browser
editor that compiles a `.scm` program to a WASM module and runs it, with output
in a console pane. It is the quickest way to try the forms above.
