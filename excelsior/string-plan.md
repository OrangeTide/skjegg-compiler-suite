# Excelsior string support: research, design, and plan

Status: planning note (2026-07). Float and fixed lowering are done; string is
the remaining piece of the "float/fixed/string runtime" work. This documents
what already exists, the representation and runtime decisions, and a slicing
into landable increments, so the implementation choices are made on paper
first.

Made by a machine. PUBLIC DOMAIN (CC0-1.0)


## 1. What already exists (so scope is small)

- **The type checker is complete for `str`.** `check_binop` already accepts
  `str + str` (concatenation, `typecheck.c` arith path), `str == str` and
  `str != str`, and `check_index` returns `str` for `s[i]`. `N_STR` types as
  `ty_str`. So no type-checker work is needed for the core; strings are a
  lowering-plus-runtime job.

- **A working reference implementation exists in-tree.** MooScript lowers
  string literals in `moo/lower.c:lower_strlit` and its runtime lives in
  `runtime/str.c`. The representation and helper set below are lifted from
  there, adjusted for Excelsior naming.

- **The IR and ColdFire backend already emit what a literal needs.** Globals
  support `init_string`/`init_strlen` (a byte blob, `IR_I8` array) and
  `init_syms[]` (a slot filled with another global's address, i.e. a
  relocation). `moo` uses exactly these, so nothing new is required at the IR
  or backend layer.

- **The runtime arena already exists.** `start.S` provides
  `__moo_arena_alloc` / `__moo_arena_reset` (bump allocator), which
  `runtime/str.c` builds on. `exc_host.c` links `start.S`, so the allocator is
  already reachable.


## 2. Representation

A string value is a **pointer to a two-word descriptor**:

    struct exc_str {
        int         len;    /* byte length, not counting a terminator */
        const char *data;   /* pointer to the bytes */
    };

Rationale:

- **A string is a pointer, so it reuses int storage.** This is the same trick
  fixed used (a fixed is an int32, so it reused int slots/ops). A `str` local,
  param, field, argument, and return value are all 4-byte pointers and travel
  through the existing int paths in `lower.c` with no new slot kind, no new
  calling-convention case, and no backend change. Only the *operations* on
  strings need new code.

- **Length-prefixed, not NUL-terminated as the source of truth.** Embedded NUL
  is representable, length is O(1), and slices can point into a parent buffer.
  `length` is authoritative. Beyond that, an **owned** buffer also carries a
  trailing NUL (string-repr.md, the owned invariant, implemented): literals,
  concatenations, and the interpolation-hole builders all terminate, so an
  owned string can demote to a C `char*` for free later. Only **views**
  (slices) are length-only, since a view's trailing byte belongs to its
  parent. The owned/view distinction is not yet tracked in the type; that and
  any `char*` boundary wait for a foreign interface that needs them (D7).

- **Immutable / copy-on-write.** Concatenation and slicing allocate a new
  descriptor and (for concat) a new buffer; nothing mutates in place. This
  matches the language's value semantics and sidesteps aliasing questions.

Alternative considered and rejected for now: a small-string-optimised inline
buffer, or a rope. Neither earns its complexity at this stage; the descriptor
is what moo shipped and what the game content needs.


## 3. Literal lowering

Port `moo/lower.c:lower_strlit` into `excelsior/lower.c` as `lower_strlit`:

1. Emit `__exc_str_N` : an `IR_I8` array global, `init_string` = the bytes,
   `arr_size` = len+1 (trailing NUL), `is_local`.
2. Emit `__exc_shdr_N` : an `IR_I32` array of 2, `init_ivals[0] = len`,
   `init_syms[1] = "__exc_str_N"` (the relocation that fills in the data
   pointer).
3. `N_STR` lowers to `IR_LEA __exc_shdr_N` — the descriptor address is the
   string value.

Escapes are already handled in the lexer (`read_escape`), so `sval`/`slen`
carry decoded bytes; the literal lowering just copies them.


## 4. Operations and their runtime helpers

Strings reuse the int binop dispatch shape used for fixed. In `lower_binop`,
after the float and fixed checks, add a string check on the operands' static
type and route to helper calls. All helpers are ordinary C in the test host,
cross-compiled and linked like the fixed helpers.

The type checker permits these on `str` today (verified in `check_binop` /
`check_index` / `N_SLICE`): `+`, `==`, `!=`, the four orderings `< <= > >=`,
`s[i]`, and `s[lo to hi]`.

| Surface               | Lowers to                                     |
|-----------------------|-----------------------------------------------|
| `a + b`  (str)        | `__exc_str_concat(a, b)` -> str               |
| `a == b` (str)        | `__exc_str_eq(a, b)` -> int (0/1)             |
| `a != b` (str)        | `xor 1` over `__exc_str_eq(a, b)`             |
| `a < b` etc. (str)    | `__exc_str_cmp(a, b)` -> sign, then int compare|
| `s[i]`                | `__exc_str_at(s, i)` -> str (1-char)          |
| `s[lo to hi]`         | `__exc_str_slice(s, lo, hi)` -> str           |

(Since fallible.md, `s[i]` is a fallible producer: the lowering
bounds-checks before the `__exc_str_at` call and branches to the active
fail label when out of range, so a bare out-of-range index traps and
`s[i] else d` catches it. The slice stays clamped.)

Runtime (in a new `runtime/exc_str.c`, or appended to `exc_host.c`), thin
adaptations of `runtime/str.c`:

    struct exc_str *__exc_str_concat(struct exc_str *a, struct exc_str *b);
    int             __exc_str_eq(struct exc_str *a, struct exc_str *b);
    int             __exc_str_cmp(struct exc_str *a, struct exc_str *b);
    struct exc_str *__exc_str_slice(struct exc_str *s, int a, int b);
    struct exc_str *__exc_str_at(struct exc_str *s, int i);

Comparison note: the `!=` `xor 1` over an equality temp, and the ordering
`__exc_str_cmp(...) < 0` etc., mirror how the float `!=`/`>`/`>=` paths already
build their results; reuse that shape.

Length, `toint`/`tostr`, `index`/`find`, and `strsub` exist in `runtime/str.c`
but have **no surface syntax in Excelsior today** (no `.len`, no method calls
on primitives). They are out of scope until the language grows a way to spell
them, and are listed here only so the runtime port can bring them across
opportunistically.


## 5. Memory management

- **Bump allocation, no free.** Concat/slice allocate from the arena. This is
  what moo does and is fine for the test host and short-lived VM contexts.
- **A real allocator / GC is deferred.** The host-abi freeze/thaw and
  per-context segment work (host-abi.md) is where lifetime management belongs;
  strings will follow whatever that decides. Do not build a collector for this
  slice.


## 6. Output (RESOLVED 2026-07 by output.md)

The decision this section framed was settled by output.md: output is a
send to a reader (`player.tell(msg is str)`, the console object in the
test host), the author channel is `trace` under `-t`, and the `log`
builtin described below shipped, served, and retired into those two.
The section is kept for the rationale trail.

The language has **no `print`**. Output is a message send or a power
(`opener.tell("...")`, and core.md's `tell <recv> ... end tell` blocks). There
is no output power in `host-abi.md` yet. This matters because a string program
has no observable effect through the current exit-code test harness unless it
reduces a string to an int.

Two independent ways to make strings testable, in increasing cost:

1. **Exit-code tests, no output.** A program builds and compares strings and
   `return`s an int: `var s is str = "ab" + "cd"`, then
   `if s == "abcd" ... return 1` (on their own lines; there is no `;`).
   Verifiable today with zero host or harness changes. This is enough to prove
   literals, concat, and comparison end to end.

2. **A host output power + stdout-diff tests.** Add a provisional test-host
   power, e.g. `__exc_say(str)` that `write`s to fd 1, and extend
   `run-exc-tests.sh` to diff `.expected` stdout (the moo and pascal harnesses
   already do stdout diffing; the pattern is established). This is what any
   real content (dialog, `tell`) ultimately needs, but it commits to a shape
   for the output power that host-abi.md has not settled.

Recommendation: do (1) first (it needs nothing new and de-risks the runtime),
then (2) as its own slice once we decide whether output is a bare power or a
send to a well-known `player`/`log` object.


## 7. Not in scope for the first string work

- ~~**Interpolation `"...${expr}..."`.**~~ **Done** (slice 5 below): the
  lexer emits `T_ISTR_PIECE` literal chunks around holes, the parser folds
  them into a concat chain, and each hole becomes an `N_TOSTR` that lowers to
  the `__exc_str_from_*` helpers. No user-facing `tostr` was needed.
- **`tostr` / `toint` / `.len` and other string methods.** No surface syntax.
- ~~**String fields.**~~ **Done.** A `str` field is a pointer word global
  like an int field; its literal default is a relocation to an interned
  descriptor (`init_syms[0]`), null when absent. `field_is_scalar` accepts
  `str`; `emit_field_globals` special-cases the pointer default; reads/writes
  reuse the int `IR_LW`/`IR_SW` path. Test `exs_str_field`.
- **`str` through sends.** Same open ABI question as float/fixed through
  sends (argv marshaling); strings are pointers so they marshal as one word,
  which is actually simpler than float, but the decision is shared.


## 8. Proposed increments

1. **Literals + concat + comparison** (`+`, `==`, `!=`), exit-code tested.
   Ports `lower_strlit` and the `concat`/`eq` helpers; adds a str branch to
   `lower_binop`. No output, no harness change. Smallest slice that proves the
   representation end to end. **Done** (test `exs_str`): a string is a pointer
   to a `{ len, data }` descriptor, literals emit two globals, and
   `__exc_str_concat`/`__exc_str_eq` live in `exc_host.c`.
2. **Ordering + indexing + slicing** (`< <= > >=`, `s[i]`, `s[lo to hi]`),
   still exit-code tested via comparison of the result. **Done** (test
   `exs_str_ops`): ordering is `__exc_str_cmp` against 0; `s[i]` and
   `s[lo to hi]` are 1-based inclusive, sharing the parent's buffer
   (immutable strings), via `__exc_str_at`/`__exc_str_slice`. (Both
   originally clamped to range; fallible.md later made `s[i]` a
   bounds-checked fallible producer that traps bare out-of-range, while
   the slice still clamps. The `lo..hi` spelling became `lo to hi` with
   the de-arrow decision.)
3. **Output power + stdout tests. Done** (test `exs_log`) as a provisional
   test affordance: a `log(x)` builtin routes by the static type of `x` to
   `__exc_log_str` / `__exc_log_int` / `__exc_log_float` / `__exc_log_fixed`
   in the test host, each writing one value plus a newline to stdout.
   `run-exc-tests.sh` now diffs a `.expected` file when present (exit code
   still checked, default 0). This gives observability for str, float, and
   fixed, and exercises verbs/funcs with typed parameters end to end. It is
   deliberately a bare builtin, not the final capability: the real output
   power (a disclosed capability, or a send to a well-known log/player
   object) is still a host-abi decision, and float/fixed *through sends* stays
   blocked on the argv-marshaling question, so `log` uses direct host calls
   (where float/fixed args already work) rather than a send.
4. **String fields. Done** (test `exs_str_field`).
5. **Interpolation. Done** (test `exs_str_interp`). The lexer is now aware of
   `${...}` holes: it feeds each literal chunk before a hole as a
   `T_ISTR_PIECE` token, the hole as ordinary tokens, and the final chunk as
   `T_STRING` (`${`/`}` are unambiguous because the language uses no braces;
   a nested string in a hole just re-enters string lexing, so
   `"${"x" + y}"` works). The parser folds the stream into a `+`-concat
   chain, wrapping each hole in a new `N_TOSTR` node. `N_TOSTR` types as
   `str` and lowers via `emit_value_call` to `__exc_str_from_int` /
   `__exc_str_from_float` / `__exc_str_from_fixed` (str holes are identity).
   That retires the `tostr`-surface concern this plan raised: interpolation
   is the only `tostr` producer and needs no user-facing spelling. A `bool`
   hole (and `log(bool)`) renders `true`/`false` via `__exc_str_from_bool` /
   `__exc_log_bool`, so comparison holes like `${a < b}` read naturally.
   Multi-line holes are unsupported.
6. **Hole string picker. Superseded (2026-07).** The `${cond ? word :
   word}` pick shipped and was then replaced by the general if-expression
   `cond then A else B` (any type, anywhere, else required, N_THENELSE;
   test `exs_then_else`). The pick's confinement to holes and its
   bare-word branches both died with it: branches are ordinary
   expressions, words are quoted like all text, and `?` left the
   language. Bool word pairs (locked/unlocked, wet/dry) are the driving
   use; declared pairs arrive later as two-member enums whose members
   stringify.


## 9. Decisions needed before coding

- **Output shape (blocks slice 3, not slice 1):** bare host power
  `__exc_say(str)`, or a send to a well-known `log`/`player` object? host-abi
  leans toward "no ambient authority", which argues against a global say power
  and toward a disclosed capability. For a *test* host a bare power is fine;
  the question is what the real ABI commits to.
- **Runtime file:** extend `exc_host.c`, or a separate `runtime/exc_str.c`
  linked alongside it (cleaner, mirrors moo's split)?
- **Helper naming:** `__exc_str_*` (chosen here) vs. reusing `__moo_str_*`
  wholesale by linking `runtime/str.c` directly. Reusing avoids a copy but
  couples Excelsior's runtime to moo's; a thin `exc_str.c` is the cleaner
  long-term boundary.
