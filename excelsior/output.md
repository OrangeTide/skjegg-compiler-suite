# Output: text is a message to someone

Status: decided and v1 implemented (2026-07), the pass on
approachability.md's R10 (the output capability), folding in R16 (the
three debug-output forms). All five decisions at the end were
confirmed; the staging is in the tree (the console object handed to
`main(player is obj)`, `tell` end to end through `__exc_send`, `log`
retired with a teaching error, `trace` and `///` lowered under `-t` to
the stderr trace helpers, and `__exc_selnames` emitted per module).

Made by a machine. PUBLIC DOMAIN (CC0-1.0)


## The problem

The language has no `print`, `log()` is a provisional test affordance
scheduled to retire, and both string-plan.md (section 6) and
host-abi.md defer the real shape: a bare output power, or a send to a
well-known object? Meanwhile "show text to the player" is the first
thing every builder persona needs, so the design cannot be validated
against its audience until this lands. R16 is the same knot from the
diagnostics side: `trace` (a statement that parses but does not
lower), `///` trace comments, and `log()` are three spellings of
"show me something" with no rule for which to teach.

## Why there is no print

In a shared world, "print to the screen" has no referent: there are a
thousand screens, and an ambient `print` cannot answer *whose*. Every
piece of text has a reader: a player, a room's occupants, or the
author debugging their object. So output is not a missing global
function; it is a message to a reader you can name. Ambient print is
not banned for capability purity; it is unanswerable in this world.
That sentence is the teachable rule, and it is also exactly the
no-ambient-authority stance host-abi.md already takes.

## What exists

- core.md's worked example already writes the answer:
  `opener.tell("The barrow chest is locked fast.")`. World output is
  a verb send; the design below mostly ratifies the example.
- **The send path can carry strings today.** `lower_send_to` marshals
  arguments as flat 32-bit words, and a str value is one pointer word,
  so a str argument already fits the argv convention; fixed (an int32)
  fits the same way. CLAUDE.md's "str through sends pending" is a
  testing gap, not an ABI gap. Only `float` (8 bytes, FPU return) is
  genuinely open, and nothing here needs it.
- **Interpolation is the whole formatting story.** `${}` holes render
  int, float, fixed, and bool into the string at the sender, so
  `tell(msg is str)` needs no printf family, no per-type variants,
  and no format DSL. The string work already solved formatting.
- Selectors are module-local ids keyed by name (`sel_id`); the host
  knows only `__exc_entry_selector`. A host-native receiver (the
  console below) needs a name-to-id table.
- `log(x)` routes by static type to `__exc_log_*` in the test host;
  the `///` trace comment compiles only under `-t`; fault reports
  already target stderr in the test host and the author's trace
  channel in the real one (runtime-errors.md).

## Survey

- **LambdaMOO**: `player:tell()` is the output primitive; there is no
  print. The direct ancestor, and the model: output is a verb on the
  player object, and every MOO builder learned it as the first thing.
- **NWN**: `SendMessageToPC(oPC, msg)`, the same shape spelled as a
  function taking the player.
- **Inform 7**: `say` is ambient and central, and it works because
  there is exactly one player. The single-user assumption is precisely
  what a shared world lacks; `say` does not transplant.
- **Smalltalk Transcript, BASIC PRINT**: the ambient console, right
  for one user at one machine, unanswerable here.
- **E (object-capability)**: output is a writer you are handed, not a
  global you name. Supports handing the entry actor its player
  reference rather than letting it look one up.
- **Erlang**: `io` routes through the group leader, a per-process,
  re-bindable parameter. Precedent that "where output goes" is context
  the host supplies, not a global the code assumes.

## Design: one channel per reader

### 1. Players read world text: `recv.tell(msg is str)`

World output is a send, full stop. `tell` is an ordinary verb: the
authority to send it is the held reference (reachability), it appears
in the `discloses` manifest (`opener:tell`), and dispatch, hot swap,
per-instance overrides, and the admission policy all apply because
nothing about it is special. It is the first entry in the standard
world vocabulary (the core.md study refinement: one canonical library
of world verbs); rooms later add `announce` and friends from the same
list. Formatting happens at the sender via interpolation:

    opener.tell("You pry the lid open. ${self.opened_count} so far.")

### 2. Authors read diagnostics: `trace`

`trace expr` finally lowers. It compiles only under `-t` (exactly like
`///` and the R2 no-match event), costs nothing in release, and routes
to the author channel: stderr in the test host, the per-author trace
channel (the one faults already target) in the real one. `trace` takes
any type `log` accepted, reusing the type router (`__exc_log_*`
machinery renamed to the trace helpers); `///` stays its text-only
sibling. The rule that closes R16 has one sentence: **`tell` talks to
players, `trace` talks to you.**

### 3. The harness reads stdout: the console player

The test host spawns one host-native object, the **console**, whose
`tell(msg)` writes the string plus a newline to stdout, and hands it
to the entry verb:

    verb main(player is obj) returns int
        player.tell("Hello ${1 + 1}")
        return 0
    endverb

Nullary `main()` stays legal for pure exit-code tests (the host passes
argc 0 as today). `run-exc-tests.sh` changes not at all: `.expected`
diffs stdout as before. The console makes every output test exercise
the real path end to end: selector resolution, `__exc_send`, argv
marshaling of a str, dispatch to a (native) verb. It is also the
world-faithful shape: the entry actor is *given* its player, the E
lesson, and the beginning of host-abi.md's "well-known bootstrap actor
and selector" replacing the provisional `main` convention.

Mechanically the console needs one new emission: `__exc_selnames`, a
constant table of the module's selector names in id order (count, then
pointers to NUL-terminated blobs, the same `intern_cstr` machinery the
trap descriptors use). The host looks up "tell" once at boot and
installs a class descriptor whose verb code pointers are native C
functions (`call_verb`'s convention already matches C). This table is
deliberately the seed of host-abi.md's selector intern table; the
real image will need exactly this mapping.

### `log()` retires

With `tell` and `trace` real, `log` has no role: its player-ish use
becomes `player.tell(...)`, its debugging use becomes `trace ...`, and
its type-routing survives underneath `trace` and interpolation. The
builtin leaves the compiler with a teaching error ("there is no `log`;
send `tell` to a player, or use `trace` for your own eyes"), and the
existing `exs_log` tests migrate.

## What this settles beyond R10

- **str and fixed through sends are green-lit**: both are one-word
  values and the marshaling already carries them; the work is tests,
  not ABI. `float` through sends stays the open two-slot question and
  blocks nothing here, because interpolation renders floats into the
  string on the sender's side.
- **R16 closes**: one channel per reader, two spellings, one rule.
- The entry convention starts moving toward the real bootstrap shape.

## Staging

1. Prove str (and fixed) through sends with a cross-object test; strike
   the "str through sends" pending item.
2. Emit `__exc_selnames`; add the console object and the
   `main(player is obj)` entry convention to the test host.
3. Add `tell` output tests; migrate the `log` tests; remove the `log`
   builtin with its teaching error.
4. Lower `trace` under `-t` through the renamed type router to stderr.

Deferred: the room vocabulary (`announce`, ...), float/fixed argv
slots for sends, the real host's channel plumbing and player-object
implementation, and any richer text (styles, localization), which
belongs to the client protocol, not the language.

## Implementation notes (2026-07)

- The migration proved str through sends at scale: 17 test files'
  output now flows `player.tell(...)` through `__exc_send` into the
  native console, and every `.expected` file matched byte for byte
  (interpolation's formatters and the old `log` helpers shared
  `fmt_int`/`fmt_double`, so `log(x)` and `tell("${x}")` render
  identically). A typed cross-object test (`exs_send_str`) covers the
  checked-send path with str and fixed arguments and returns.
- The compiler emits `__exc_entry_argc` next to the entry class and
  selector; the host passes the console when it is 1 and nothing when
  0, so nullary `main()` tests run unchanged.
- `///` had been a silent no-op even under `-t`; it now prints its
  text through `__exc_trace_str` like the design always described.
- Threading `player` into private funcs (two tests needed it) is the
  capability discipline showing in miniature: a helper that talks to
  the player says so in its signature.

## Decisions (confirmed and implemented 2026-07)

1. The entry convention: `verb main(player is obj)` with the host
   handing in the console: YES. Capability discipline, and it
   dogfoods sends; the ambient well-known name was rejected. Nullary
   `main()` stays legal (`__exc_entry_argc` tells the host).
2. `tell`'s contract is str-only: YES; interpolation covers the
   types, so there are no per-type overloads.
3. `log` retired immediately with the migration: YES; the builtin
   answers with a teaching error ("there is no `log`; send `tell` to
   a player, or use `trace` for your own eyes").
4. `trace` compiled only under `-t`: YES, matching `///` and the R2
   event; release builds carry nothing, and trace-argument side
   effects vanish with them (the documented consequence).
5. The full `__exc_selnames` name table: YES; it seeds the selector
   intern table host-abi.md needs anyway.
