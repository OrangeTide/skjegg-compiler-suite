# Memory management: regions on the escape boundary, no tracing GC

Status: decided (2026-07), not yet implemented. The memory-management pass from the backlog (backlog.md, the
Compact Pascal lifetime item revisited for Excelsior), the model several decided
notes wait on. Tier: **invisible at World** (an author never frees, annotates a
lifetime, or sees a pause), the object lifecycle (`spawn`/`destroy`) at World, the
mechanism a host/runtime concern. Builds on the arena and turn model
(runtime-errors.md, which left turn transactionality open), the value/actor split
(records.md, data-model.md), CoW lists and owned/view strings (buffer.md,
string-repr.md), the non-escape rules (shared-params.md, function-values.md), and
freeze/thaw (host-abi.md).

Compact Pascal weighed manual and scope-based reclaim against a collector for a
systems audience that can reason about lifetime. Excelsior's audience cannot, and
its server is a multi-tenant sandbox that cannot tolerate a stop-the-world pause,
so it **diverges toward full automation while keeping CP's aversion to a tracing
collector**. The resolution is that the language already drew the only line the
allocator needs: the **escape boundary** between a turn's transient scratch and an
actor's persistent state. Memory management is naming the regions that boundary
implies and the cheapest mechanism each needs.

Made by a machine. PUBLIC DOMAIN (CC0-1.0)

## The forces

- **The author cannot manage memory.** No `free`, no ownership annotations, no
  lifetime keywords in the surface (tier World). Reclamation is automatic.
- **The server is a sandboxed, multi-tenant, latency-sensitive host.** Many
  untrusted actors share it, so memory must be safe, bounded (a script cannot
  exhaust the server), and free of stop-the-world pauses.
- **The turn and the value/actor split already exist.** A verb call is a turn
  with a bump arena; a fault rolls the turn back (runtime-errors.md). Records are
  values, lists are CoW, strings are immutable, buffers are transient, closures
  do not escape. These are already an escape discipline; they only lacked a
  memory model to sit on.

Automatic, region-partitioned, no tracing GC in the turn: that is the whole
stance, and the regions fall out of the escape boundary.

## The escape boundary is the model

A value is **transient** until it **escapes** the turn, by being stored in an
actor field or handed across a send; then it is **persistent**. Every prior pass
drew this same line: a `shared` parameter is call-scoped and never escapes; a
buffer is turn-local and must freeze to escape; a string view is copied when it
would escape; a captured closure may not escape at all. Transient memory and
persistent memory are two regions, reclaimed differently, and a third region
holds the actors themselves.

## Region 1: the turn arena (transient)

All transient allocation, expression intermediates, a comprehension's working
buffer, a non-escaping closure's frame, a string view, a scratch `buffer`, is
**bump-allocated in the turn arena and reclaimed wholesale by resetting the arena
at turn end**. There is no per-object bookkeeping and no collector: the turn ends,
the pointer resets. A fault resets the arena to the **turn-start mark**, which is
exactly the cheap rollback runtime-errors.md relies on. Most allocation in a
typical turn is transient and costs nothing to reclaim.

## Region 2: the persistent value heap (reference-counted, acyclic)

A value that escapes into a field or a send outlives the arena and moves to the
**persistent value heap**, which is **reference-counted**. This is safe without a
cycle collector because the value world is **acyclic by construction**: strings
and CoW lists and records are immutable once built, so a value can never be made
to contain itself (closing a cycle would need mutation the value types forbid).
Immutability also lets a value be **shared across actors** safely, so assigning a
big list to a field or sending it to another actor is O(1), a refcount bump, not a
copy, preserving what CoW buys. When the last reference drops (a field
overwritten, an actor destroyed), the value frees promptly. A string view that
escapes materializes into an owned, refcounted string (string-repr.md); a record
in a field carries refcounts on its own heap fields.

This is the CPython/Swift refcount, minus their cycle collector, which the value
world does not need. Cycles exist only among actors, the next region.

## Region 3: actors (explicit lifecycle, cycles kept out of the hot path)

An actor has an **explicit lifecycle**: `spawn(Class)` creates one and
**`destroy(obj)`** reclaims it (lowering to the host's `__exc_recycle`), which is
**game logic, not memory management**, a mob dies, an item is consumed, a room is
torn down. A reference to a destroyed actor **fails safely**: a send to it takes
the fault/absence path (nil-nothing.md's absent-object handling), so a dangling
handle can never read freed memory. Because actors are destroyed explicitly,
**object cycles do not leak through refcounts**, the one place cycles exist is
handled by the domain's own object lifecycle, not a per-turn collector. An
optional **coarse world-level sweep** may reclaim orphaned object cycles
(unreachable from any root) as a host background job, never inside a turn. Objects
are the only cyclic region, and they are kept entirely out of the hot path.

## The turn is transactional (settling runtime-errors.md's open question)

runtime-errors.md left open "what complete means for actor state mid-turn, abort
and roll back or keep the partial writes." This pass settles it: **the turn is
transactional, and a fault rolls it back.** Persistent field writes within a turn
are **journaled**; on success the turn **commits** (the journal drops and
refcount reclamation of overwritten values happens), on a fault the journal
**undoes** the writes and restores the overwritten values. Transient rollback is
the arena reset; persistent rollback is the journal undo. This is why `defer`
runs on committing exits but not on a hard fault (defer.md), and it is the
LambdaMOO transactional-task model. Reclamation is tied to commit precisely so a
rollback can restore a value the turn was about to free.

## Bounded by quota (the sandbox guarantee)

Memory is **bounded by a per-turn and per-actor quota**. Exhausting it is an
**`OVERFLOW`-class fault that rolls the turn back**, not a crash or an OOM kill
(runtime-errors.md). A script cannot exhaust the shared server: an unbounded
allocation faults, the turn rolls back, and the actor is unharmed. This makes
memory safety a sandbox guarantee, the same shape as arithmetic overflow.

## One line: persist, freeze, and refcount are the same set

The types that may live in a **persistent field**, the types that **freeze/thaw**
to storage (host-abi.md), and the types the **refcounted heap** holds are the
*same set*: immutable values (str, list, record, decimal, enum, set) plus object
handles. The types excluded from all three are the same too: a **buffer** (mutable,
identity-bearing) and a **closure** (captures the arena) are turn-local, do not
persist, do not freeze, and never reach the persistent heap. Memory management,
persistence, and the escape boundary are one distinction wearing three hats, which
is why the model needs no new author-facing concept.

## Resolving the deferred items

- **Escaping closures stay forbidden** (function-values.md). The arena model is
  *why*: a persisted closure would capture frame state the turn reset reclaims, so
  it cannot be stored, returned, or sent. The actor-safe way to store behavior is
  a verb or capability, which lives in the class descriptor, not the heap.
- **A persistent buffer field**, when genuinely needed, is a **uniquely-owned
  mutable heap object**: one field owns it, assignment moves rather than shares
  (a mutable thing must not alias across owners), and it frees when the field is
  overwritten or the actor destroyed. It is Mechanics-tier and rare; the common
  path is to build in a turn-local buffer and **freeze it into a CoW `list` or
  owned `str` field** (buffer.md), which enters the ordinary refcounted heap.

## What the author sees

Nothing, almost. No `free`, no ownership keyword, no lifetime annotation, no GC
pause. The only explicit memory-shaped act is the object lifecycle, `spawn` and
`destroy`, and that reads as game logic (creating and removing world objects),
not as allocation. Everything else, a list grown, a string built, a record
passed, is reclaimed by the region it lives in, invisibly.

## What is deferred

- The **coarse object cycle sweep**'s trigger, algorithm, and cadence (a host
  background concern, not surface).
- Exact **quota values** and whether they are per-actor, per-turn, or both,
  tuned against the real server.
- **Persistent-heap compaction and fragmentation** (a long-running server
  concern) and **weak references** (for caches and back-pointers), additive.
- Whether the persistent value heap's refcount is **deferred to commit** in all
  cases or eager with an undo-log, an implementation sub-decision under D6.

## Survey

- **Erlang**: the closest model, isolated per-actor heaps with message-passing;
  Excelsior's actor is the process, its turn arena the transient heap, and a
  value crossing a send is shared-immutable (refcounted) rather than deep-copied.
  Excelsior avoids Erlang's per-process tracing GC because most allocation is
  turn-scoped and the rest is acyclic and refcounted.
- **LambdaMOO**: transactional tasks that roll back on error, explicit
  `recycle()`, single-threaded execution; the turn transactionality and explicit
  object lifecycle are taken directly.
- **Region / arena allocators (per-request in web servers)**: the turn arena is
  the per-turn arena, reset wholesale.
- **CPython / Swift refcounting**: the persistent value heap, minus the cycle
  collector, which the acyclic value world does not need.
- **Rust ownership / Compact Pascal scope reclaim**: correct and pause-free but
  *exposed* to the programmer; rejected as the Excelsior surface because the
  audience cannot carry lifetime, though the region discipline is shared under
  the hood.
- **Tracing / generational GC (Java, Go)**: rejected in the turn hot path for its
  pauses and its unbounded, hard-to-sandbox cost.

Excelsior's stance: automatic and pause-free, a per-turn arena for transients, a
reference-counted acyclic heap for escaped immutable values shared across actors,
an explicit object lifecycle for the only cyclic region, a transactional turn, and
a quota that makes exhaustion a safe fault, with lifetime never exposed to the
author.

## Decisions (confirmed)

The eight decisions are confirmed. The implementation (the turn arena with a
start-mark reset, the reference-counted acyclic value heap shared across actors,
the explicit `spawn`/`destroy` object lifecycle with safe dangling access, the
journaled transactional turn, and the quota fault) is a runtime and host concern
that lands with the object model; the coarse object cycle sweep, quota values,
heap compaction, and weak references are deferred follow-ons.

**D1. Memory is fully automatic with no tracing GC in the turn hot path.** The
audience cannot manage memory and the sandbox cannot tolerate pauses, so
reclamation is region-partitioned by the escape boundary rather than traced. This
revisits Compact Pascal's lifetime question and diverges toward full automation,
keeping its aversion to a stop-the-world collector.

**D2. The model is the escape boundary already drawn by prior passes:** a value is
transient (arena) until it escapes into a field or a send, then persistent. This
is the same line `shared`, `buffer` freeze, string views, and non-escaping
closures already draw.

**D3. Region 1, the turn arena:** all transients are bump-allocated and reclaimed
by resetting the arena at turn end; a fault resets to the turn-start mark
(runtime-errors.md). No collector, no per-object cost.

**D4. Region 2, the persistent value heap, is reference-counted and needs no cycle
collector.** Escaped immutable/CoW values (str, list, record, and the rest) are
refcounted; the value world is acyclic by construction (immutability forbids a
value containing itself), and immutability lets values be shared across actors, so
storing or sending a value is an O(1) refcount bump, freed promptly at zero.

**D5. Region 3, actors, have an explicit lifecycle:** `spawn(Class)` and
`destroy(obj)` (game logic, lowering to `__exc_recycle`); a reference to a
destroyed actor fails safely (nil-nothing.md), so object cycles do not leak.
Object cycles are the only cyclic region, handled by explicit destruction and an
optional coarse world-level sweep that never runs inside a turn.

**D6. The turn is transactional (settling runtime-errors.md's open question):**
persistent field writes are journaled, committed at turn end or undone on a fault,
and refcount reclamation of overwritten values happens at commit so a rollback
restores them. Arena reset is the transient rollback; journal undo is the
persistent one. This is why `defer` runs on commit but not on a hard fault.

**D7. Memory is bounded by a per-turn/per-actor quota, and exhaustion is an
`OVERFLOW`-class fault that rolls the turn back**, not a crash. A script cannot
exhaust the shared server; memory safety is a sandbox guarantee like arithmetic
overflow.

**D8. Persist, freeze/thaw, and the refcounted heap are the same type set**
(immutable values plus object handles); buffers and closures are turn-local and
excluded from all three. Escaping closures stay forbidden (a verb or capability is
the substitute); a persistent buffer field, when needed, is a uniquely-owned
mutable heap object (moved, not shared, freed on overwrite/destroy), the common
path being to freeze a turn-local buffer into a CoW `list`/`str` field.
