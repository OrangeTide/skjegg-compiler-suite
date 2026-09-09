# Excelsior Host ABI: the two-layer contract

Status: decided (2026-07), v1. The pass that promoted the v0 sketch,
grounded in a survey of the three target hosts:
**boris** (https://gitlab.com/OrangeTide/boris),
**smolmoo** (https://github.com/OrangeTide/smolmoo), and
**aldeby** (https://github.com/OrangeTide/aldeby), plus the two
development tiers (the in-tree test host under qemu-m68k, and native
Linux). Layer 1 below is largely implemented: it documents the contract
`runtime/libexc.c` and the compiler already share, with the seam
declared in `runtime/libexc.h`. Layer 2's first implementation is the
native binding `runtime/exc_native.c`; no VM-host binding exists yet.
The boris mapping is provisional
(D8): its VM ABI is explicitly pre-freeze. The aldeby mapping waits on
its scripting spec; its constraints are recorded.

Companion to core.md (the language) and `../doc/game-scripting.md` (the
runtime brief). Each host is cited by its upstream URL; the survey facts
below were read from the repositories in 2026-07.

## The model in one paragraph

An Excelsior **world of actors lives inside one VM**. The compiler emits
host-independent code against **libexc**, the guest-side runtime that
owns the object table, dispatch, the arenas, and the value helpers; a
send is synchronous and local. libexc in turn stands on a **small host
binding**: output, persistence, scheduling, and cross-VM messaging,
spelled as hypercalls on a ColdFire VM host and as plain calls natively.
The host activates a verb by driving the binding's dispatch loop; the
guest calls up only through the binding. Everything that crosses the VM
wall is coarse; everything inside it is the language's own machinery.

## The survey in one table

What the hosts actually are (implementation, not aspiration):

- **test host**: `runtime/libexc.c` (layer 1) + `runtime/exc_native.c`
  (layer 2) + `start.S` under qemu-m68k, one address space, direct
  calls, no persistence. The reference implementation of both layers.
- **native Linux** (RISC-V / AArch64): the same shape as the test host,
  compiled natively. A lower-priority but real tier: developers try
  samples without qemu or a server install. Costs a backend build of
  skj-exc per target and a port of the source-coroutine helpers.
- **smolmoo**: 128KB ColdFire VM per verb invocation, freshly zeroed;
  ELF32 BE from a BLAKE2b content store; hypercalls are LINE_A
  (`0xA000|nr`) with the m68k gcc register convention (ints d0-d3,
  pointers a0-a1, return d0); persistent state is host-side string-named
  properties; single-threaded instruction-quantum scheduler; tasks may
  persist by blocking in `sys_wait`.
- **boris**: 60KB (configurable) long-lived ColdFire VM per room; ELF32
  BE; the same LINE_A trap scheme and register convention; the guest CRT
  registers verb handlers and parks in `HC_WAIT`, the host dispatches by
  writing a context block and setting the PC; messaging is fire-and-
  forget today, QNX-style SEND/RECV/REPLY in the spec; no persistence
  yet; the whole ABI is marked subject to change (D8).
- **aldeby**: scripting designed but not built. Programs will be
  content-addressed immutable ColdFire blobs versioned with the world,
  running on the server (authoritative) and on clients (prediction, with
  reconciliation); verbs are catalog slots with engine-owned arguments;
  confederation (SHOAL) is signed topics over the same content store.

Sizing, from the project owner: servers dedicate 2-4GB; each VM gets
128-512KB of address space (128KB is small); around 200 VMs live at
once, so VM memory totals near 100MB. Today's hosts sit at the small end
(60-128KB), which is the budget libexc's regions must fit (see Memory).

## D1. Two layers

**Layer 1, compiler to libexc,** is the fixed, host-independent
contract: object handles, per-instance layout, class descriptors,
selectors, `__exc_send`, the trap descriptors, the value helpers
(strings, lists, records, decimals, sets, sources), and the entry
convention. It is what the compiler emits calls to, and it never varies
by host. It is implemented: `runtime/libexc.c` plus the
`__exc_src_*` and `__moo_arena_*` pieces of `runtime/start.S` are its
reference body.

**Layer 2, libexc to host,** is the binding: the short list of services
only the host can provide (D5). On a ColdFire VM host each is a LINE_A
hypercall; on the native and test tiers each is an ordinary function.
Porting Excelsior to a host means writing this binding and nothing else;
the three hosts never reimplement dispatch or a string helper.

The seam between the layers is placed so that everything *portable* is
above it and everything *host-owned* is below it. The split is done
(2026-07): `runtime/libexc.c` with the seam header `runtime/libexc.h`,
and `runtime/exc_native.c` as the first binding (the `__exh_*` calls,
the console object, and the entry driver).

## Object references

An `obj` value is a 32-bit unforgeable **handle**; `nil` is handle 0.
Handles index libexc's object table inside the VM. The handle space is
partitioned: a reserved high range denotes **remote** objects (another
VM, another shard) so that cross-VM sends can arrive later (D2) without
retyping `obj`. In v1 no remote handle is ever minted; host-native
objects the binding injects (a player console) are ordinary local
entries whose verbs the binding resolves via `__exc_selnames`. Handles
carry no ambient authority; holding one is the authority to send to it,
and an object slice (object-slices.md) attenuates that authority at
compile time.

## D3. Per-instance state: handle plus offset, settled

An object is a header (its class descriptor pointer) followed by its own
field words; `self.field` is the running-actor handle plus a constant
offset. This is the implemented model, and this pass retires the v0
sketch's swapped-segment alternative permanently: handle-plus-offset
works in one address space on every tier, keeps cross-object field
access reachable, and makes a send one global swap rather than a
remapping. Layout is parent-first so an inherited field keeps its
offset; defaults live in the per-class `Class__image` that spawn copies.
The reserved global `__exc_self` names the running actor;
`__exc_self_class` stays reserved for a dispatch fast path but is not
part of v1 (nothing emits or sets it).

## Class descriptors and selectors (D4)

The compiler emits per-class descriptors `{ parent, nverbs, nwords,
image, (selector, code)..., nfields, (name, woffset, kind)... }` and
the selector-name table `__exc_selnames` (names in id order). Dispatch
resolves a selector through the descriptor chain, so self,
cross-object, inherited, and overridden sends share one path; hot-swap
replaces a code pointer. The trailing field table (own fields,
segment-relative offsets; the chain covers inherited ones) is the
freeze/thaw walker's map (D7): a field is a word, a str (serialized by
content), or a record (not walked yet). The descriptor symbol is
global: it is the host-facing metadata.

**The selector's identity is its name; the numeric id is image-local.**
This is the content-addressing stance (the build cache keys on names
over hashes, and aldeby's confederation shares programs by hash):
two shards agree on a verb by its name, never by an id, and every image
interns its own id space seeded from `__exc_selnames`. Cross-shard
agreement therefore needs no registry.

## D2. Sends: synchronous, local to the VM, in v1

    value __exc_send(obj recv, u32 selector, u32 argc, word *argv);

A send is synchronous and value-returning, resolved by libexc against
the receiver's descriptor chain within this VM; `argv` is a flat array
of 32-bit words (str, list, record, and decimal are one word; the float
and 64-bit slot convention is deferred with float-through-sends). This
matches both what the language implements and what the hosts implement:
neither boris nor smolmoo has synchronous guest-to-guest RPC today.

Cross-VM interaction in v1 is **not a send**: it is a binding power
(`post`, events, broadcast), fire-and-forget, matching boris's
`HC_MSG_POST` and smolmoo's broadcast. When boris's SEND/RECV/REPLY
rendezvous freezes, a synchronous remote send can be added as: a send
whose receiver is a remote-range handle marshals its argv through the
binding, blocks the turn QNX-style, and returns the reply. That
extension changes no v1 code path; the remote handle range (above) is
the door left open. Payload marshaling across the wall copies value
types (str, list, record) and forbids second-class values (a source
never crosses, sources.md D5); object handles do not cross shards, only
content-addressed data does.

## D5. The binding surface

The services libexc requires of a host, each a named routine with the
m68k gcc register convention on VM hosts (both target hosts already use
exactly this: LINE_A traps, ints in d0-d3, pointers in a0-a1, return in
d0) and an ordinary C function natively:

    __exh_emit(handle who, len, buf)     text to a player/console
    __exh_post(target, len, buf)         cross-VM fire-and-forget
    __exh_prop_get(key, buf, bufsz)      persistence read  (freeze/thaw)
    __exh_prop_put(key, buf, len)        persistence write (freeze/thaw)
    __exh_wait(handles, n, timeout)      park for the next activation
    __exh_yield() / __exh_sleep(ms)      scheduling courtesy
    __exh_fault(desc, a, b)              route a fault report
    __exh_spawn(image, arg)              start another task/VM (provisional)

The surface is deliberately coarse and **versioned** (an
`EXH_ABI_VERSION` the binding reports), so boris's post-freeze spec can
amend its mapping without touching the other tiers (D8). The native
binding implements these over write/poll/nanosleep and exits 70 on
`__exh_fault` (today's behavior); a VM binding maps them onto the host's
hypercall numbers. `discloses` remains the coarse attach-time manifest
of which powers a unit uses; the host checks it at attach, not per call.

Output stays a send at the language level (output.md): the binding
injects a console/player object whose `tell` routes to `__exh_emit`.
`trace` and the fault report route to the host's author channel through
`__exh_emit`/`__exh_fault` per debug-output.md when that lands.

## D6. Activation, the turn, and memory regions

The **binding owns `_start` and the dispatch loop**; compiled code never
contains an event loop. On a long-lived VM (boris): `_start` initializes
libexc, registers each public verb of the entry classes with the host
(the survey shows boris's guest-registered `verb:` handles), then parks
in `__exh_wait`; an activation reads the host's context, builds argv,
calls the verb through the descriptor, and re-parks. On a
per-invocation VM (smolmoo): `_start` thaws the actor's fields
(`__exh_prop_get` through the walker, D7), runs the one verb, freezes
them back, and exits. Natively, the test host's
`__exc_entry_class`/`__exc_entry_selector` convention is the same idea
with `main` as the loop.

A **turn is one activation**: verb entry to verb return. The VM's
memory is four regions: the program image; the **persistent heap**
(objects and their field segments, live across turns in a long-lived
VM); the **turn arena** (transients, reset at turn end); and stacks
(the machine stack plus 4KB per live source, sources.md). The regions'
sizing and the arena/heap split are memory.md's territory (its D6
cost-model pass settled 2026-07: a fault aborts the turn, no rollback
machinery, so no journal region exists); the budget they must fit is
the 60-512KB
figures above. Durative, suspended turns (the brief's effects) are a
later feature built on `__exh_wait`/`__exh_sleep`; when they land, the
suspension mechanism must follow sources.md's amendment (a private
stack per suspended activity, never a copied stack segment tied to an
address).

## D7. Freeze/thaw: the descriptor walker is the persistence bridge

A generic libexc walker (`exc_freeze`/`exc_thaw`, implemented 2026-07,
`make test-exc-walker`) serializes an object's field segment through
`__exh_prop_put` (a word as a decimal string, a str by content, capped
at a property's size; record and maybe fields are not walked yet) and
overlays found properties back onto an image-seeded segment, so an
absent property is the default. Freeze writes every walked field: the
v0 sketch's delta-from-defaults is unsound without a property-delete
(a field reverting to its default would leave the stale property to
resurrect the old value on the next thaw), so delta waits on
`__exh_prop_del` joining D5's surface. Keys are
the field name prefixed `x_`: visible and editable in a host's
property editor, and collision-proof against its reserved names. Per
host it becomes:

- **smolmoo**: fields marshal to host properties at activation
  boundaries (the binding's `_start` thaws from the verb's owning
  object, runs, freezes back); the object *is* the host
  object, and the VM is a stateless verb executor.
- **boris**: the walker is the room snapshot its spec defers: instead of
  checkpointing raw CPU/RAM, freeze the world of actors by descriptor
  and rebuild it on thaw. Parked-at-`__exh_wait` is the only freeze
  point, so no live stack is ever serialized.
- **aldeby**: object state versions with the world snapshot; the walker
  output is content-addressed like everything else there.
- **native/test**: a no-op (or a file, for a persistent dev world).

Migration rides the walker: keys are names, so an added field simply
finds no property and keeps its default, and a removed one leaves a
stale property behind; the shape-version machinery for a breaking
retype (a builder-provided migrate fragment) stays deferred.

## D8. Per-host status

- **test host / native**: the reference; both layers exist, split
  (`runtime/libexc.c` + `runtime/exc_native.c`).
- **smolmoo**: the first real binding, written (2026-07):
  `runtime/exc_smolmoo.c` implements the `__exh_*` surface over the
  LINE_A hypercalls (emit over write, fault as exit 70, the property
  pair, post over broadcast, yield/sleep over suspend), provides the
  arena in C and the ColdFire-legal source-coroutine helpers
  (`runtime/smolmoo_rt.S`), and owns `_start`: it resolves the invoked
  verb's name through `__exc_selnames`, so one program serves several
  verb slots, then spawns the bootstrap actor and sends. Linked with
  `runtime/smolmoo.ld` by `make smolmoo-demo` (the server runs the ELF;
  qemu cannot, its hypercalls have no Linux meaning). Writing it
  surfaced two 68k-isms in the ColdFire backend, predecrement `movem`
  and byte-size `neg`, both fixed, so compiled output now assembles
  strictly as ColdFire (`-mcpu=5475`); and the toolchain's libgcc being
  68020 code (no ColdFire multilib), so the 64-bit helpers come from
  `runtime/soft64.c` instead, C compiled the same strict way.
  **Server-validated (2026-07)** against a live smolmoo: the demo ELF
  (sources, the coroutine switch including its FPU saves, under the
  server's strict emulator) streamed its full expected output to a
  player session, and a stateful counter verb held `hits 1..3` across
  three fresh-VM invocations through the D7 walker, its `x_hits`
  property readable server-side. Two semantics learned there: the
  persistent actor is `this_obj`, which smolmoo sets to the
  resolution-chain ancestor the verb was found through, so a
  system-wide verb's state lives on the Verb Prototype (shared), and
  per-object state comes from attaching the verb to a world object;
  and `install` is an offline operation, since a running server's
  autosave overwrites a concurrent install's world root (its
  `SMOLMOO_DEPOT` override also applies to `serve` only, so install
  from the directory whose `depot/` you mean).
- **boris**: mapping **provisional**. Its spec is the closest match to
  the language's actor model (synchronous rendezvous is exactly the
  future remote send), but its ABI is pre-freeze and its implementation
  lags the spec; the binding tracks the spec and is revisited at its
  freeze. Nothing in layers 1 or 2 waits for it.
- **aldeby**: no mapping until its scripting spec exists. Constraints
  recorded now so layer 1 does not drift against them: programs are
  immutable content-addressed blobs versioned with the world; the same
  blob may run server-side and client-side (prediction), so compiled
  code must stay deterministic given identical inputs, with server
  authority and reconciliation absorbing the rest; verb arguments come
  from the engine's catalog, not free-form input; selector identity by
  name (D4) is what confederation needs.

## Mapping from today's codegen

Already true: descriptors, `__exc_selnames`, `__exc_send` for every
send, per-instance segments, `__exc_spawn`, the entry symbols, the trap
descriptors, and the source coroutine helpers; the libexc /
native-binding split with the `__exh_*` indirection, and the smolmoo
binding, the descriptor field tables, and the freeze/thaw walker (all
2026-07), validated end to end against a live smolmoo server
(execution, output, and walker persistence; see D8). The compiler
changed only to become strictly
ColdFire-legal (the `movem` and `neg.b` fixes the binding surfaced)
and to emit the field tables the walker reads.

## Open questions

- The float and 64-bit `argv` slot convention (waits on float through
  sends; numbers.md's decimal covers most game math meanwhile).
- The `err` value representation (waits on the `err` type's own pass).
- The synchronous remote send (waits on boris's messaging freeze; the
  remote handle range and marshaling rules above are its landing pad).
- Region sizing and the arena/heap split (memory.md; the rollback
  question is settled, no journal region, sizing numbers remain).
- The aldeby mapping (waits on its scripting spec).
- Live-edit/hot-swap flow through the content store (the build-cache
  brief owns the design; the descriptor's code-pointer swap is the
  runtime half).
