# Excelsior on smolmoo: v1 milestones

This is the critical path from the current state (the smolmoo binding links
but has never executed against a real server) to a first shippable target: a
compiled Excelsior verb that runs under smolmoo and keeps per-object state
across invocations.

The scope is deliberately narrow. smolmoo runs each verb invocation in a
fresh, zeroed 128KB VM, arena-resets between invocations, and persists state
by freezing fields to the host object's properties. That stateless-executor
model removes three of Excelsior's largest deferred pieces from the v1 path,
see "Explicitly out of v1" below.

## Target: the current smolmoo is RV32, not ColdFire

The current smolmoo server (checked 2026-09) is a RISC-V RV32 machine
(`rv32.c`, `vm_rv.ld`); there is no ColdFire VM in its tree. Its hypercalls
are RISC-V `ecall` under the ILP32 psABI: syscall number in `a7`, arguments
in `a0`-`a5`, result in `a0`.

The Excelsior binding was written against a ColdFire assumption and still
emits ColdFire LINE_A hypercalls, so `make smolmoo-demo` builds a ColdFire
ELF that cannot execute on the RV32 server. This is the reframing of M0
below: it is not "run the existing ELF" but "retarget the binding to RV32."

Everything above the instruction set already matches the RV32 server:

- The seven syscall numbers in `runtime/exc_smolmoo.c` (exit 0, write 4,
  broadcast 6, getprop 7, setprop 8, suspend 11) match smolmoo's `vm_ecall`
  dispatch, with the same semantics.
- The `vm_args` struct at the fixed address `0x380`
  (`runtime/exc_smolmoo.c:43`) matches the layout smolmoo pre-writes there
  (player, room, argstr, arglen, this_obj, dobj, iobj, then the string
  pointers).
- The memory map matches `vm_rv.ld`: code at `0x400`, 128KB, the arena after
  bss.
- `skj-exc-rv` already emits the RISC-V ILP32 psABI that smolmoo verbs use,
  and the full skj RV32 toolchain (`skj-as-rv`, `skj-ld-rv`) already builds
  Excelsior end to end (`make check-exc-rv-skj`).

So the arch-neutral parts of the binding (the `__exh_*` services, the `tell`
console, the `_start` thaw/send/freeze driver, and libexc's freeze/thaw)
carry over to RV32 unchanged. The retarget is confined to the seven
hypercall stubs, the coroutine assembly shim, and the linker script.

## Current state

Working today (verified only by linking and by the qemu/skj-run native tier,
not against a smolmoo server):

- The seven `__exh_*` host services are wired to hypercalls in
  `runtime/exc_smolmoo.c` (`__exh_emit`, `__exh_fault`, `__exh_prop_get`,
  `__exh_prop_put`, `__exh_post`, `__exh_yield`, `__exh_sleep`). The stubs
  still emit ColdFire LINE_A opwords and must be retargeted to RV32 `ecall`,
  see M0.
- A 64KB C arena (`runtime/exc_smolmoo.c:144`, there is no `start.S` in this
  binding).
- A `tell` console object dispatched through the ordinary `__exc_send` path
  (`runtime/exc_smolmoo.c:226`).
- The full entry lifecycle in `_start` (`runtime/exc_smolmoo.c:245`): resolve
  the invoked verb by name against `__exc_selnames`, `__exc_spawn` the
  bootstrap actor, `exc_thaw` its fields from the host object, `__exc_send`
  the verb, `exc_freeze` back to the host object, exit.
- The freeze/thaw walker (`runtime/libexc.c:1096` `exc_freeze`,
  `runtime/libexc.c:1133` `exc_thaw`): it walks the class field table up the
  parent chain and persists `EXC_FK_WORD` and `EXC_FK_STR` fields through
  `__exh_prop_put` / `__exh_prop_get`.
- `make smolmoo-demo` links a compiled `.exs` into a verb ELF. This is
  link-only: the ELF is meant to load under the smolmoo server, not qemu.
- Excelsior itself runs end to end on both VMs through the native binding
  (`make check-exc`, `make check-exc-rv`, 174 tests each).

The gap is that none of the smolmoo path has been executed. It is written,
not run.

## Milestones

### M0: retarget the binding to RV32 and run one verb

The proving milestone. Everything below is unverified until this exists.
Because the current server is RV32 (see "Target" above), M0 is a retarget,
not just an invocation.

**Status (2026-09): the retarget is done and links** into a valid RV32
executable (`make smolmoo-demo`); steps 1-3 below are implemented. What
remains is running it on a real server (steps 4-5), the external
dependency. The binding's `sys_*` stubs now emit `ecall`
(`runtime/exc_smolmoo.c`), the coroutine helpers are RV32
(`runtime/smolmoo_rt_rv.S`), the linker script is `runtime/smolmoo_rv.ld`,
and `make smolmoo-demo` builds the ELF through
skj-exc-rv/skj-as-rv/skj-ld-rv. The demo verb is still
`tests/exs_source_yield.exs` (it also exercises the coroutine shim, so the
link proves those symbols resolve); step 4's trivial verb is for the actual
run.

The concrete steps:

1. **RV32 hypercall stubs.** Swap the seven `sys_*` inline-asm stubs in
   `runtime/exc_smolmoo.c` from m68k LINE_A (`.word 0xA00n`, `d0`/`a0`/`a1`)
   to RISC-V `ecall` (number in `a7`, arguments in `a0`-`a5`, result in
   `a0`). The syscall numbers and the code above the stubs do not change.
2. **RV32 crt and linker script.** `smolmoo_rt.S` is ColdFire; provide the
   RV32 source-coroutine helpers (skj already ships these in the psABI RV32
   runtime) and link against `vm_rv.ld` (code at `0x400`, arena after bss).
   The binding keeps its own `_start`.
3. **A `smolmoo-demo-rv` make target** that builds through `skj-exc-rv` ->
   `skj-as-rv` -> `skj-ld-rv` against the RV32 binding, with `libexc`,
   `utf8`, and `soft64` compiled for RV32. Flip `make smolmoo-demo` to this
   path so the tree stops building an unrunnable ColdFire artifact.
4. **A trivial M0 verb.** The current demo compiles `exs_source_yield.exs`, a
   coroutine test, which is a poor first target. Use a one-line
   `verb hello(player is obj) ... player.tell("hi") ... endverb` so M0
   exercises only entry dispatch and `tell` (write to fd 1). Persistence is
   M1.
5. **Register and run.** `verbs.conf` maps `obj_id parent verb_name
   source_file`, and `smolmoo install verbs.conf` compiles each source into
   the content-addressed `depot/`. Two frictions: the install path only
   handles `.c` today, and smolmoo's vendored SDK (`sdk/`) carries `moo` but
   not `excelsior`. Lower-friction for M0: build the verb ELF in the skjegg
   tree and inject it into smolmoo's `depot/` plus register the verb object.
   The clean long-term path is re-vendoring smolmoo's SDK with `excelsior` +
   `as-rv`/`ld-rv` and teaching `verbs.conf`/install to compile `.exs`.

Then invoke the verb and confirm `tell` reaches the player (fd 1) and fault
text reaches the server log (fd 2).

The gating dependency is external: a runnable smolmoo server
(OrangeTide/smolmoo), not Excelsior code. Standing that up is the item on the
critical path; the milestones after it are largely verification of code that
already exists.

### M1: round-trip persistent state (WORD and STR)

This is the literal goal, a persistent verb. Prove that an int field (for
example `opened_count`) and a str field survive across two invocations of the
same object.

The code path exists (`exc_freeze` at `runtime/libexc.c:1096`, `exc_thaw` at
`runtime/libexc.c:1134`, driven from `_start` at
`runtime/exc_smolmoo.c:271` and `:278`). Running it flushes the field key
naming (`field_key`), the `EXC_KEYMAX` / `EXC_VALMAX` buffer caps, and the
negative-return contract of `__exh_prop_get` (a missing property returns
`< 0`, which `exc_thaw` treats as "keep the default").

Small work, high signal.

### M2: list runtime under the smolmoo arena

CLAUDE.md flags list runtime as still pending, but the compiler already lowers
`append` / `prepend` / `insert` / `set` / `+` and the rest to `__exc_list_*`
helpers. The scope is to confirm those helpers are present in libexc and run
against smolmoo's C arena, which is their only dependency. Verify a verb that
builds and iterates a list at runtime.

This may be far smaller than "pending" suggests. M0 running will tell you.

### M3: persist the field kinds real scripts use

`exc_freeze` and `exc_thaw` deliberately skip three field kinds today
(`EXC_FK_REC`, `EXC_FK_MAYBE`, `EXC_FK_OBJ`); see the comment at
`runtime/libexc.c:1128`. They stay at their defaults across a freeze/thaw.

For v1:

- **Record fields (`EXC_FK_REC`).** Add walking that flattens the record's
  fields to per-field properties. Load-bearing if scripts use record-typed
  fields.
- **maybe fields (`EXC_FK_MAYBE`).** Add the null-word sentinel plus payload
  case.
- **obj-handle fields (`EXC_FK_OBJ`).** This is the object-graph persistence
  problem: a VM object handle is meaningless across a fresh VM and must be
  mapped to a host object id. Defer for v1. Document that cross-invocation
  object references stored in fields are not persisted yet. Most single-verb
  scripts hold object references only within a turn.

This is the only milestone that is real build work rather than verification,
and its heaviest part (obj-handle graphs) is deferred.

### M4: fault UX on the server

`__exh_fault` (`runtime/exc_smolmoo.c:180`) currently just calls `exit(70)`
after libexc has emitted the fault report on channel 0. Verify that the report
renders cleanly in the smolmoo log and that the player sees a clean message
rather than a dead VM. This confirms the trap UX from `runtime-errors.md`
works in world.

### M5: multi-verb program and selector dispatch

`_start` already resolves the invoked verb by name against `__exc_selnames`
(`runtime/exc_smolmoo.c:261`), so one compiled program can carry several
public verb slots. Verify that a single `.exs` with several public verbs (for
example `on_open` and `unlock`) dispatches to the right one per invocation.
This is what makes one compiled program an object's full behavior rather than
a single entry point.

## Shape of the work

- M0, M1, M4, and M5 are mostly verification of code that is already written
  but has never run.
- M2 is likely verification plus a small fill.
- M3 is the only substantial build, and its heaviest part is deferred.

The binding constraint is M0's external dependency. None of this can be
verified without a smolmoo server to run against.

## Explicitly out of v1

smolmoo's fresh-VM-per-invocation model removes these from the path, so they
stay deferred at no cost to v1:

- **The escaped-value heap** (`excelsior/memory.md`). Not needed. The arena
  resets each invocation, and anything that must outlive the turn goes through
  freeze to host properties. Turn-local is the whole model.
- **Concurrency and the durative effect.** Not needed. Each invocation is one
  synchronous verb. `__exh_yield` / `__exh_sleep` exist for suspension but the
  durative-effect macro is deferrable.
- **`.exi` generation and the disclosure manifest.** Deferrable for a trusted
  single-shard proof of concept.
- **Hotfix (behavior swap, shape migration, tunable defaults), the
  interpreter tier, and lazy migration.**
- **The ergonomics design notes still marked decided-not-implemented**
  (choosers, visibility, boolean-ops, string-literals, inline-for,
  slice-clamp, buffer, blob, patterns, capabilities, type-parameters,
  union-types, mixed-lists, quote, debug-output).
