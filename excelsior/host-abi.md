# Excelsior Host ABI (sketch, v0)

The contract between Excelsior-compiled ColdFire code and the host runtime
(the game server acting as hypervisor). It defines how an actor's state is
laid out, how a message send reaches a receiver, how references and powers
work, and how the host drives compiled code. Companion to `core.md` (the
language) and `../doc/game-scripting.md` (the runtime brief); this document
is the boundary between them.

Status: sketch. The compiler now emits **per-instance field segments**, and it
took a different shape from the one sketched below: an object is a header (its
class pointer) followed by its own fields, and `self.field` is a load of the
running-actor handle plus a constant offset, rather than a fixed global address
into a segment the host swaps. See "Per-instance state" for the two models and
why the implemented one was chosen; the choice is open to revisit when the real
VM target lands, since it is the compiler/host seam this note exists to fix.
This document is the target the compiler and host converge on;
the "Mapping from today's codegen" section records the delta.

## The model in one paragraph

An actor is a running ColdFire VM context: a register file, a stack, and its
own fields. `self.field` reads the running-actor handle and adds the field's
constant offset (see "Per-instance state" for this and the swapped-segment
alternative it replaced). The host owns many such contexts and makes one live
at a time by setting that handle. A message send is a single host downcall that names the
receiver by handle and the verb by selector id; the host resolves it against
the receiver's actual class, which is why cross-object, inherited, overridden,
and hot-swapped dispatch all work through one path. References are unforgeable
handles; holding one is the authority to send to it; the receiving actor
validates what it will do.

## Object references

An `obj` value is a 32-bit **handle**: an opaque index into a host-owned
object table, not a pointer into memory. `nil` is handle 0. Handles are
unforgeable (a script obtains one only by being passed it, never by
construction or arithmetic) and carry no ambient authority: there is no global
handle to arbitrary objects. The data segment has no object header; the handle
lives host-side, so "no header on the object" and "references are handles" are
consistent. `err` values are handles into the same space with an error tag, or
a distinguished small-integer range (open).

## Per-instance state

There are two ways to give each actor its own fields, and the implemented one
is not the one sketched here.

**Sketched: a swapped segment.** `self.field` is a fixed global address, each
actor has its own image of the segment, and the host makes one current by
mapping its image into the addresses the code was compiled against. One load
per field, no base register, and it wants a VM that can remap a segment
cheaply.

**Implemented: a handle plus an offset.** An object is a header (its class
pointer) followed by its own field words, and `self.field` loads the running
actor handle and adds a constant offset. It costs one extra load per field
access and needs no mapping at all, so it works in one address space, which is
what the test host has. It also makes a send cheap (the host sets the running
handle and restores it on return, rather than swapping a segment), and it
leaves the door open to cross-object field access, which the swapped-segment
model forecloses entirely: with fixed addresses there is no way to name another
actor's field without making that actor current.

The layout is parent fields first, so an inherited field keeps its offset in a
subclass and the single-inheritance prefix still holds. Defaults live in a
per-class initial image (`Class__image`) that spawn copies into each new
object, inherited defaults included, with a str default carried as a relocation
to its interned descriptor.

The choice is worth revisiting against the real VM: if the target can remap a
segment for free, the sketched model is one instruction cheaper per field
access, and the freeze/thaw story below is written against it. Nothing above
the seam depends on which one is used. The multi-actor part is otherwise
host-side as described: idle actors freeze to storage and thaw on demand, so the
resident set is the working set, not the population. The compiler emits no
`self` pointer and passes none to verbs, because the live segment already is
this actor's fields.

Two reserved globals the host sets on every context entry:

    __exc_self          obj      handle of the running actor
    __exc_self_class    ptr      its class descriptor (see below)

`self.field` needs neither; only a self-send reads `__exc_self`.

## Class descriptor

The unit of dispatch and layout metadata. The compiler emits one per class;
the host reads it to build dispatch tables, to freeze/thaw, and to migrate.
Shape (fields illustrative, not final):

    struct class_desc {
        u32   class_id;          // stable id in the live image
        u32   shape_version;     // bumped on a layout change (migration)
        ptr   parent;            // parent class_desc, or 0
        u32   nwords;            // field-segment size, in words (implemented)
        ptr   image;             // initial field values, or 0 (implemented)
        u32   nverbs;
        struct { u32 selector; ptr code; } verbs[nverbs];  // this class's verbs
        u32   nfields;
        struct field_desc fields[nfields];  // name, offset, type, default,
                                             // tunable flag: freeze/thaw + migrate
    }

The host composes a class's full dispatch table by walking `parent` and
overlaying each class's `verbs` (a subclass entry for a selector overrides the
parent's). A per-instance verb override installs an entry in an instance-local
overlay the host consults first. Hot-swap replaces a `code` pointer; because
every send indexes this table indirectly, live sends pick up the new code
(running coroutines drain on the old code, per the turn model).

## Selectors

A **selector** is a globally-unique small integer id for a verb name, the
Objective-C `SEL`. Global (not per-class) so a send can index any receiver's
table by the same id. The live image owns an intern table mapping verb name to
id; incremental compilation of a new verb name allocates a new id, and ids are
stable across recompiles. The compiler references a selector by name and the
loader resolves it to the id (a relocation, or a generated selector-table
symbol). Content addressing keys on the name, not the numeric id, so the id
space can be image-local.

## Dispatch: the one send primitive

    value __exc_send(obj recv, u32 selector, u32 argc, word *argv);

The host, in order: checks the frame holds `recv` (reachability); thaws the
receiver if paged out; finds its class descriptor and resolves `selector`
through the descriptor chain plus any instance override; runs the target
actor's own admission policy for the verb (the confused-deputy answer, the
target owns its policy); establishes the receiver's context (swap segment and
the reserved globals); calls the verb `code`; restores the caller's context;
returns the value. A self-send is the same call with `recv = __exc_self`; it
skips the reachability check and, for a same-turn synchronous send, the host
may fast-path it (index `__exc_self_class` directly and call), but the
semantics are identical.

This one primitive subsumes what the compiler defers today: cross-object sends
(receiver is another actor), inherited-verb dispatch (resolved through the
parent chain at run time), and override dispatch (subclass or per-instance
overlay). The compiler stops emitting per-class vtables for dispatch; it emits
class descriptors and lowers every send to `__exc_send`.

Argument marshaling: `argv` is a flat array of 32-bit words; the host knows the
callee's signature from its descriptor, so it reinterprets slots. A `float`
(IEEE 754 double) and a 64-bit value occupy two slots; large value types
(`vec`, `mat`, records) pass by a segment-local reference. Exact float/64-bit
slot convention is open. The return value comes back in the standard result
location for its type (`d0`/`a0`, or the FPU register for `float`).

## Powers (hypercalls)

The genuinely dangerous operations are host downcalls, not compiled in-VM.
Each is a named routine; the set is small and matches the coarse `discloses`
manifest the compiler already computes:

    obj  __exc_spawn(ptr class_desc, u32 argc, word *argv);  // create an actor
    void __exc_recycle(obj target);                          // destroy
    void __exc_persist(obj target);                          // freeze to htree
    ...  __exc_hypercall(u32 service, ...);                  // geometry, other-lang kernels
    ...  network / trace-channel / etc.

`discloses` lists which of these a unit uses; the host checks coarsely at
attach time (owner/moderator review), not threaded per call. `spawn` and
friends are why bare names like `spawn` are currently unresolved externals in
the compiler; under the ABI they resolve to these downcalls.

**Output (settled 2026-07, output.md).** World text is a send: the test
host provides a native **console** object whose `tell(msg is str)` writes
the string plus a newline to stdout, handed to the entry verb when it
declares a player parameter (`main(player is obj)`; the compiler emits
`__exc_entry_argc` so the host knows). The old `log()` builtin retired
into `tell` (players) and `trace` (the author channel: `-t`-gated,
routed by static type to the `__exc_trace_*` stderr helpers). A str or
fixed argument is one word and rides the flat argv unchanged; only the
float slot convention stays open below. To resolve its native verbs the
host reads **`__exc_selnames`**, a compiler-emitted `{ count, &name...
}` table of selector names in id order, the seed of the selector intern
table this document's open question describes.

## Coroutines and the turn model

A running verb or effect is a green-thread coroutine: it runs, suspends at a
synchronization point, and resumes later, scheduled by the host's discrete-
event priority queue. A send that must cross a turn barrier or block suspends
the caller; the host captures the continuation and reschedules. The mechanism
already exists: the IR has `IR_MARK` / `IR_CAPTURE` / `IR_RESUME` and
`runtime/start.S` carries continuation capture/resume. Durative `effect`s
compile to coroutines the host schedules through the same queue. Builders never
see raw channels; the scheduler owns the select over the tick and subscribed
events. A per-task tick budget backstops runaway loops.

## Upcalls: host to compiled code

- **Verb entry.** The host establishes the actor context (segment plus the
  reserved globals), then calls the verb `code` from the class descriptor with
  `(argc, argv)`. The code is the ordinary compiled `Class__verb`; no shim
  beyond context setup. This is also how the top-level entry works once the
  provisional `main` convention is retired: a well-known bootstrap actor and
  selector replace it.
- **Freeze / thaw.** A generic host walker uses the descriptor's `fields` to
  serialize the delta-from-defaults (copy-on-write: only written slots) to a
  content-addressed blob, and to reconstruct a segment from one. The compiler
  emits the layout; the host walks it.
- **Migration.** On thaw, if `shape_version` differs, the host applies
  automatic compatible changes (add field with default, rename, widen) from
  the descriptors, and for a breaking retype calls a builder-provided
  `migrate old -> new` fragment, compiled to an ordinary callable.

## Mapping from today's codegen

What stays unchanged:

- `self.field` (and inherited `self.field`) as fixed globals in the segment.
- Verb and func bodies compiled as `Class__verb` / `Class__func` code.
- Direct calls to sibling funcs (`name(...)`), which are not sends.

What changes to reach the ABI:

- The per-class `Class__vtable` indexed by local verb order becomes a class
  descriptor keyed by global selector id, plus field-layout and parent
  metadata. The host, not the compiler, builds the resolved dispatch table.
- Every send lowers to `__exc_send(recv, selector, argc, argv)` instead of an
  in-VM `IR_CALLI` through a local vtable. `recv = __exc_self` for a self-send.
- The two deferred cases (cross-object dispatch, inherited/override dispatch)
  are then just `__exc_send` with the right receiver; the compiler no longer
  special-cases self.
- `spawn` and other powers resolve from unresolved externals to the named
  hypercalls.

## Incremental path

1. Define the selector intern table and emit class descriptors (selectors,
   parent, verb code pointers) alongside the current vtable.
2. Provide a minimal test host: an `__exc_send` that resolves a selector
   against a descriptor and calls the verb in-process (single address space,
   one segment), enough to run self-sends and same-address-space cross-object
   sends under qemu without the full scheduler.
3. Lower all sends to `__exc_send`; retire the local-index vtable and the
   inherited/cross-object diagnostics.
4. Add per-context segments and context switching in the host; add powers,
   freeze/thaw, and the coroutine scheduler as they are needed.

## Open questions

- Selector id interning in a live, incremental, content-addressed image
  (allocation, persistence, cross-shard agreement).
- Synchronous versus asynchronous send default, and where the turn barrier
  falls for a value-returning cross-object send.
- Exact `argv` slot convention for `float`, 64-bit, and value types.
- Handle representation for `err` and whether it shares the `obj` table.
- Resident context footprint (the concurrency-granularity number the brief
  leaves open): register file plus stack per suspended actor.
