# One author channel: `///` becomes the trace/log facility

Status: decided (2026-07), not yet implemented. The pass on
approachability.md's R16 (three debug-output forms). Tier: World (tier 1);
`///` is the one diagnostic form a builder meets, `tell` is the separate world
channel (tiers.md, output.md). The four decisions are confirmed; the
implementation (interpolation in the `///` lexer path, the default-on
host-routed trace primitive with its listening guard, and retiring the
`trace` keyword) is the follow-up.

output.md already retired `log` and set the rule "`tell` talks to players,
`trace` talks to you", but it left two author-facing spellings, `trace expr`
and the `///` trace comment, where R16 asks for exactly one. This note closes
that residual the other way from a first reading: it keeps `///`, promotes it
into a real logging facility (default-on, host-routed, with `${}`
interpolation), and folds `trace expr` into it. The result is one author
channel, `///`, and the sharpened rule **`tell` talks to players, `///` talks
to the trace channel**.

Made by a machine. PUBLIC DOMAIN (CC0-1.0)

## What exists today

- **`trace expr`** (T_TRACE / N_TRACE): a statement compiled only under `-t`
  that routes a value by static type through the trace helpers. Shows one
  value: `trace hp`.
- **`///` trace comment** (T_TRACE_COMMENT / N_TRACE_CMT): a comment-shaped
  line compiled only under `-t` that emits its **text**. Static text only, no
  values. Without `-t` it lexes as an ordinary comment.
- **`log()`**: already retired (output.md), leaving a teaching error.

`///` is the friendlier surface (comment-shaped, no quotes, reads as a note
that shows up), but today it is the weaker one: static text, `-t`-gated. The
design below removes both limits and makes it the primary form.

## The design

**`///` is the one author-facing trace and log form. It is default-on,
host-routed, and interpolated.** Three moves.

### 1. `///` is a louder comment, and it is the log line

The comment family gains a level: `//` is an inert note to a human, `///` is
a note that also goes to the trace channel. The escalation is visible in the
source (one more slash), so nothing about a plain `//` changes and there is no
"my comment printed?" surprise: a builder opts in by writing the third slash.
This is the teaching hook, "add a slash and your note shows up in the trace
log", and it is exactly the ergonomic that makes `///` approachable.

### 2. `${}` interpolation turns it into a logging facility

`///` text becomes an interpolated string, the same `${}` holes and `\$`
escape a `"..."` literal and a `tell` message already use, folding each hole
through the same `N_TOSTR` type router (`__exc_str_from_int` / `_float` /
`_dec`, str holes identity). So a `///` line is, mechanically, "an
interpolated string handed to the trace channel":

    /// checking the door                 // static text
    /// hp is ${hp}, mp is ${mp}           // text plus values
    /// ${door.state}                      // a bare value dump

The third line is what subsumes `trace expr`: `/// ${expr}` is the value dump
`trace expr` used to be, so the two forms become one. Because `///` runs to
end of line like a comment, it can stand on its own line or trail a
statement, which makes lightweight annotation cheap (`x = step() /// x is now
${x}`).

### 3. The trace channel is default-on and host-routed

`///` is not `-t`-gated. It always compiles and writes to **the trace
channel**, and the host decides where that channel goes and whether anyone is
listening:

- a command-line or test host routes it to **stderr** (what the current `-t`
  path does, now always on).
- a MUD routes an object's trace to **the object's owner**, so the builder
  debugging their own creation sees its diagnostics live, without attaching
  anything. This is the MUD-native "wizard sees the object's log" pattern.
- a production or headless context routes it to **a discarded sink** (no
  reader).

This is how real logging works: the statement is always in the program, and a
runtime-configured sink decides emission and destination, rather than the
diagnostic being compiled out. The efficiency the `-t` strip used to buy is
recovered with a runtime guard: the host exposes "is anyone listening on this
object's channel", and the lowering checks it before building the interpolated
string, so a discarded channel costs one branch, not a formatted message. The
trace primitive is object-aware (it receives the emitting `self`) so the host
can route by the object's owner.

## The reconciled surface

The whole "show me something" surface, after this pass:

- **to a player, always**: `player.tell(msg)`. The world channel.
- **to the trace channel, always (host-routed)**: `/// text ${values}`. The
  one author channel, a logging facility.
- **`trace expr`**: retired, folded into `/// ${expr}`.
- **`log`**: retired (output.md).

"`tell` talks to players, `///` talks to the trace channel", and a plain `//`
talks to nobody. Exactly one form on each side, which is the reconciliation
R16 asked the tutorial to be able to teach.

## Migration

The `trace` keyword retires with a teaching hint: "`trace expr` is now `///
${expr}`; `///` is the trace channel and takes `${}` holes like a string." A
plain `//` comment is unchanged. Existing `///` lines keep working and gain
interpolation and default-on routing; no `///` source changes meaning except
that it now emits without `-t`.

## Survey

Where logging facilities and debug output sit, and why the default-on,
host-routed model fits:

- **Log frameworks** (Python `logging`, Rust `log` + `env_logger`, Go
  `slog`, log4j): the log call is always compiled; a runtime sink and level
  decide destination and whether it emits. `///` is this model with a
  comment-shaped surface, and the host-configured channel is the sink.
- **MUD / MOO tradition**: LambdaMOO routes an object's runtime errors and
  tracebacks to programmers and owners; `notify()` targets a connected
  player. Routing a scripted object's `///` to its owner is the direct
  descendant, and the reason the default channel is object-aware.
- **`dbg!` / `eprintln!` / `printf` debugging**: the ephemeral value dump
  these give is now `/// ${expr}`, one form instead of a second construct.
- **Doc comments** (`///` in Rust and C#, processed but never emitted at
  runtime): a false friend to note, but here `///` deliberately owns the
  emits-to-trace meaning, and the `//` versus `///` escalation makes the
  opt-in explicit.

Excelsior's stance: one world channel (`tell`) and one author channel
(`///`), the author channel a real logging facility (default-on, host-routed
sink, string interpolation), with `//` the inert comment beneath it.

## Decisions (confirmed)

**D1. `///` is the one author-facing trace and log form** (tier 1, World). It
is a comment-family line: `//` is an inert human note, `///` also writes to
the trace channel. The value-dump role of `trace expr` folds into `///
${expr}`.

**D2. `///` gets `${}` interpolation and `\$` escaping,** reusing the string
interpolation machinery and the `N_TOSTR` type router, so a `///` line is an
interpolated string handed to the trace channel. This turns static trace text
into a logging facility and is what lets it subsume `trace expr`.

**D3. `///` is default-on and host-routed, not `-t`-gated.** It always
compiles and writes to the trace channel; the host routes it (stderr for a CLI
or test host, the object's owner on a MUD, or a discarded sink) and exposes an
"is anyone listening" guard the lowering checks before formatting, so a
discarded channel costs a branch, not a message. The trace primitive is
object-aware for owner routing.

**D4. `trace expr` retires, folded into `/// ${expr}`; the `trace` keyword
gets a migration hint. `log` stays retired.** The trio collapses to one author
form, `///`, alongside the one world channel `tell`. This supersedes
output.md's "`trace` is the survivor, `///` its text-only sibling."
