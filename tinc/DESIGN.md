# TinC Redesign

TinC is a minimal C-like language used as an exerciser for the
skjegg IR.  This document describes a redesign that replaces the
K&R C subset with a cleaner, more expressive language at roughly
the same implementation complexity.  It remains TinC — same name,
same role, same tool (`skj-tinc`).

The redesign was informed by research into the S-Lang scripting
language (jedsoft.org), particularly its stack-based parameter
model and multiple return values.

Made by a machine.  PUBLIC DOMAIN (CC0-1.0)

---

## Design Goals

1. **Same role as TinC** — a simple front end that exercises the
   shared IR and backends.  Not a production language.
2. **Roughly equal or less implementation complexity** — target
   ~2000 LOC (TinC is ~2000 LOC today).
3. **More expressive** — multiple return values, destructuring,
   array views, cleaner syntax.
4. **Fewer footguns** — no raw pointers, no implicit stack leaks,
   compile-time checking where possible.
5. **Still a systems language** — byte-level memory access,
   memory-mapped I/O, no GC, no runtime heap required.

---

## What Changes from TinC

### Removed

| TinC feature              | Why cut                                    |
|---------------------------|--------------------------------------------|
| K&R parameter declarations| Parsing complexity for no benefit           |
| Raw pointers (`*`, `&`)   | Replaced by array views and pass-by-ref     |
| Pointer arithmetic        | Replaced by array indexing                  |
| `char` as variable type   | Locals are always int-width; `byte` is an array storage class |

### Added

| New feature               | What it replaces                            |
|---------------------------|---------------------------------------------|
| `define` function syntax  | C return-type + K&R params                  |
| Multiple return values    | Out-parameters via pointer                  |
| Destructuring assignment  | Manual unpacking                            |
| Variable return ABI       | Varargs hacks                               |
| Array views / aliasing    | Pointer casting and arithmetic              |
| `foreach (x : arr)`       | Manual index loop over arrays               |
| `_` discard in patterns   | Forgetting to consume a return value        |
| `make_array` intrinsic  | Raw address casting                         |

### Kept

- `if`/`else`, `while`, `break`, `continue`, `return`
- `/* */` and `//` comments
- Integer arithmetic, bitwise, logical, comparison operators
- Compound assignment (`+=`, `-=`, etc.) — TinC lacked these;
  TinC adds them
- Global and local variables, arrays with initializers
- Function calls (direct and indirect)
- Continuation intrinsics (`mark`, `capture`, `resume`)
- `alloca` intrinsic
- String literals (desugar to global byte arrays)

---

## Types

TinC has one scalar type and two array element widths:

| Type    | Width  | Use                                         |
|---------|--------|---------------------------------------------|
| `int`   | 32-bit | All scalars, locals, parameters, returns     |
| `int`   | 32-bit | Array element type                           |
| `byte`  | 8-bit  | Array element type only                      |

There are no pointer types, no function types, no float types.
A variable is either an `int` or an array.  Everything that
isn't an array is `int` — numbers, booleans, function addresses,
characters, flags.  The machine doesn't distinguish them and
neither does the language.

A function name without `()` evaluates to its address as an
`int`.  A character literal evaluates to its codepoint as an
`int`.  Zero is false, non-zero is true.  The type system is
honest about what the hardware actually provides.

Loading a `byte` array element widens to `int`.  Storing to a
`byte` array truncates.

Closures, lambdas, and tagged types are out of scope — those
belong in TinScheme or other front ends.  TinC stays simple.

---

## Syntax

### Comments

```
/* block comment */
// line comment
```

### Variable Declarations

```
int x;                       // uninitialized local
int x = 42;                  // initialized local
int g = 10;                  // global (at file scope)
byte buf[256];               // byte array, 256 elements
int table[] = {1, 2, 3};    // int array, size inferred
byte msg[] = "hello\n";     // byte array from string literal
```

Locals are always int-width.  `byte` only appears in array
declarations.

### Functions

```
define gcd(int a, int b) -> int
{
    while (b) {
        int t = a % b;
        a = b;
        b = t;
    }
    return a;
}
```

- `define` keyword introduces a function.
- Parameter list declares types inline.
- `-> int` declares a single int return (optional; defaults to
  void).
- `-> 2` declares a fixed multi-return count.
- `-> int..` declares a variable return (returns count + values).

Void functions (no `->` clause) return nothing.  The compiler
enforces that every call site either captures or discards all
return values.

### Fixed Multiple Return

```
define divmod(int a, int b) -> 2
{
    return a / b, a % b;
}

(q, r) = divmod(17, 5);     // q=3, r=2
(q, _) = divmod(17, 5);     // discard remainder
```

The function signature declares the count (`-> 2`).  The caller
must provide exactly that many targets (names or `_`).

### Variable Return

```
define range(int lo, int hi) -> int..
{
    int i = lo;
    while (i < hi) {
        return.. i;          // append value to return area
        i += 1;
    }
    return;                  // finalize: send count + values to caller
}

(a, b, rest..) = range(0, 10);  // a=0, b=1, rest=[2..9]
(_, _, tail..) = range(5, 8);   // discard first two, tail=[7]
(count) = range(0, 0);          // count=0, no values
```

**`return..` appends a value** to the return area.  It does not
transfer control — execution continues after it.  **`return;`
(bare) finalizes** and transfers control to the caller.  The
count is derived automatically from how many `return..` calls
were made.

The signature `-> int..` means the function returns (count,
values...) where count >= 0.  A minimum can be declared:
`-> 2..` means at least 2 values plus count.  The compiler
checks at the call site that destructuring patterns are
compatible with the declared minimum.

`rest..` in the caller captures remaining values into a local
array.  The compiler allocates stack space based on the count.

#### Variable Return Implementation

The return area is a stack region that grows forward, like a
vector built on the stack frame.  No heap allocation.

```
Stack frame layout:

[ locals ][ return area -------> ]
           ^base        ^cursor
```

The lowering synthesizes this from existing IR primitives:

1. **Function entry**: allocate a base pointer and a count local
   (initialized to 0) for the return area.  The return area
   sits at the top of the frame, growing away from locals.
2. **`return.. expr`**: store value at `base + count * 4`,
   increment count.
3. **`return;`**: the epilogue makes count available to the
   caller (via register or stack slot), and the caller knows
   the base follows immediately.

No new IR opcodes are strictly required — the lowering emits
IR_STL / IR_LDL for the count, IR_ADD + IR_SW for the stores
into the return area, and the existing frame machinery handles
the rest.

#### Constraints

- **`return..` and `return expr` are mutually exclusive.**
  A function uses either the variable return protocol or the
  fixed return protocol, never both.  The signature (`-> int..`
  vs `-> int` vs `-> 2`) determines which.
- **The return area must not conflict with local allocations.**
  The compiler places it at the top of the frame so it can
  grow freely.

### Array Views

An array is a descriptor: (base address, length, element width).
Owned arrays allocate storage.  Views alias existing memory.

```
byte buf[256];                              // owned, stack-allocated
byte alias[] = buf;                         // view of buf
byte slice[] = buf[64:128];                 // view of buf[64..127]
byte hw[] = make_array(0xFFFF0000, 16);   // view of hardware regs
```

Views are immutable after declaration but may use runtime values:

```
define process(byte data[], int offset, int len)
{
    byte window[] = data[offset : offset + len];
    // window is fixed for this scope
    while (len) { len -= 1; ... }
}
```

Passing an owned array to a function promotes it to a view in
the callee — the callee receives (base, len) as two implicit
values.

### Control Flow

```
if (cond) { ... }
if (cond) { ... } else { ... }

while (cond) { ... }

foreach (x : arr) { ... }   // iterate array elements

break;
continue;
return;                     // void return (or finalize variable return)
return expr;                // fixed return
return a, b;                // fixed multi-return
return.. expr;              // append to variable return area
```

`foreach` declares a read-only loop variable and iterates every
element of an array (owned or view).  The compiler knows the
array's length and element width, so it emits a tight counted
loop with bounds-checked access — no manual index variable needed.

```
byte msg[] = "hello";
foreach (ch : msg) {
    write(1, ch);
}
```

The lowering is: load length, init hidden counter to 0, emit
`while (counter < len) { x = arr[counter]; body; counter += 1; }`.
`break` and `continue` work as expected.

### Operators

Same as TinC plus compound assignment:

```
// Arithmetic:    + - * / %
// Bitwise:       & | ^ ~ << >>
// Logical:       && || !
// Comparison:    == != < <= > >=
// Assignment:    = += -= *= /= %= &= |= ^= <<= >>=
// Array index:   a[i]
// Array slice:   a[lo:hi]
```

No `++`/`--`.  Use `x += 1`.

### Function Calls

```
result = gcd(24, 18);              // direct call
(q, r) = divmod(10, 3);            // multi-return capture
_ = write(1, msg, 6);             // discard return value

int cb = gcd;                      // function name without () is its address
result = cb(24, 18);               // call through variable — no special syntax
```

A function name without `()` evaluates to its address (an int).
Calling through a variable uses the same syntax as a direct
call — the compiler checks whether the name resolves to a known
function (emits `IR_CALL`) or a variable (emits `IR_CALLI`).

### Builtins

These look like function calls but compile inline — no runtime
library needed.  The namespace is small enough that reserving
these names is not a problem.

```
mark()                    // continuation mark
capture()                 // capture continuation
resume(buf, val)          // resume continuation
alloca(size)              // dynamic stack allocation
make_array(addr, len)     // construct array view from address
length(arr)               // array length (const for owned, load for views)
```

---

## Array Descriptor Details

Every array, whether owned or view, has three properties known
to the compiler:

- **base**: address of element 0
- **len**: number of elements
- **width**: element size in bytes (4 for `int`, 1 for `byte`)

For owned stack arrays, base is the stack offset and len is the
literal size — the compiler can optimize away the descriptor and
use direct stack-relative addressing.

For views passed as parameters, the caller passes base and len
as two implicit int arguments.  The callee treats them as
read-only locals.

`a[i]` generates: `base + i * width`, then load/store at the
appropriate width.

`a[lo:hi]` generates a new descriptor: `(base + lo * width,
hi - lo, width)`.  This is a view, not a copy.

---

## Calling Convention

Two ABIs, selected by the function signature:

### Fixed Return ABI

Used when the return count is known at compile time (`-> int`,
`-> 2`, or no return clause).

- Arguments pushed right-to-left on stack, caller pops.
- Return value(s) in register(s) or stack slots.
- Array arguments passed as (base, len) — two words per array.

### Variable Return ABI

Used when the signature is `-> int..` or `-> N..`.

- Arguments: same as fixed.
- Return: callee pushes values, then pushes count.
- Caller pops count first, then pops that many values.
- If a minimum is declared (`-> 2..`), the compiler checks at
  the call site that the destructuring pattern is compatible.

---

## Implementation Strategy

TinC reuses the existing `tinc/` directory structure:

| File      | Changes from TinC                               |
|-----------|-------------------------------------------------|
| `tinc.h`  | New token set, new AST nodes, remove pointer types |
| `lex.c`   | `define`/`byte`/`foreach` keywords, `..` token, `_` token, compound assignment operators |
| `parse.c` | New function syntax, destructuring LHS, `foreach`, array slice notation, remove K&R param parsing, remove unary `*`/`&` |
| `lower.c` | Multiple return lowering, view descriptor lowering, remove pointer arithmetic scaling, remove char/int width tracking |
| `main.c`  | Unchanged (same driver pipeline)                |

The IR and backends require no changes.  Fixed multiple returns
use existing `IR_ARG`/`IR_RETV`.  Variable returns are
synthesized from existing primitives: a base pointer and count
local, IR_ADD + IR_SW for appending to the return area, and
IR_STL/IR_LDL for count tracking.  No new IR opcodes needed.

### Estimated Complexity

| Component              | Current  | Redesign (est.)  | Delta   |
|------------------------|----------|-------------------|---------|
| lex.c                  | 304      | ~292              | -12     |
| parse.c                | 574      | ~578              | +4      |
| lower.c                | 987      | ~938              | -49     |
| tinc.h                 | 79       | ~81               | +2      |
| main.c                 | 56       | ~56               | 0       |
| **Total**              | **2000** | **~1945**         | **-55** |

The pointer/char removal saves more code than the new features
(`foreach`, multi-return, views, bounds checks) add.  Keeping
C-style comments and dropping `loop`/`forever` help offset
`foreach`'s parse and lowering cost.

---

## Test Plan

Port the 9 existing tests to the new syntax, plus new tests:

| Test           | Exercises                                   |
|----------------|---------------------------------------------|
| hello          | Byte array, string literal, write syscall   |
| fib            | Recursion, basic arithmetic                 |
| spill          | Register pressure (many params)             |
| mod            | Modulo operator under pressure              |
| bsearch        | Array indexing, loops, conditionals          |
| loop           | Pattern matching on byte arrays             |
| memcpy         | Byte array view, pass-by-reference          |
| cont           | Continuation intrinsics                     |
| alloca         | Dynamic stack allocation                    |
| multiret       | Fixed multi-return + destructuring          |
| varret         | Variable return ABI + rest-capture          |
| view           | Array view, slice, `make_array`           |
| discard        | `_` discard, compiler error on leak         |
| compound       | Compound assignment operators               |
| foreach        | `foreach` iteration over arrays             |

---

## Open Questions

1. ~~**Variable return loop syntax**~~ **Resolved:** `return..`
   appends to a forward-growing stack region (vector-like).
   `return;` finalizes.  Count is implicit.  No new IR opcodes
   needed — synthesized from existing store/load/add primitives.

2. ~~**Indirect call syntax**~~ **Resolved:** No special syntax.
   A function name without `()` is its address.  `cb(args)` where
   `cb` is a variable emits IR_CALLI.  Same as C minus the type
   ceremony — no `&`, no `*`, no `indirect` keyword.

3. ~~**Array length access**~~ **Resolved:** `length(arr)` is a
   compiler builtin.  For owned arrays, folds to a constant.
   For views, emits a load of the len slot.  Looks like a
   function call, compiled inline — same pattern as the
   continuation intrinsics.

4. ~~**Bounds checking**~~ **Resolved:** Always on.  Every
   array index emits a range check against len.  The escape
   hatch is `make_array(addr, large_len)` — an oversized
   view disables the check in practice without a compiler
   flag or special syntax.

5. **File extension** — `.tc` (unchanged).
