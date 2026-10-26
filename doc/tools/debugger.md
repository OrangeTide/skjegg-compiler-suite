# A source-level debugger for skjegg guests

This note is the design for source-level debugging of skjegg-compiled
programs, C and Kobold first, and any other front end for free since they
all lower to the one IR. It is a **design, not an implementation**. Nothing
described here is built yet; the file records the decisions and the plan so
the work can start from a fixed target.

Two decisions are settled:

1. **Debug information is DWARF version 4**, emitted into the output ELF.
   The line table is produced once in a format-neutral form in the IR and
   serialized as a DWARF line-number program. gdb, `readelf`, `addr2line`
   and `qemu-riscv32 -g` are the oracles that validate it, the same
   golden-master method the assemblers use against GNU as.
2. **The debugger engine is a gdbserver-style RSP stub in `skj-run`.** The
   emulator becomes execution plus a GDB remote-serial-protocol server; the
   debugger front end is a separate process (a stock gdb, or a small in-tree
   client). This keeps a clean split between the sandboxed execution core and
   the debugger UI.

The two halves are independent. The DWARF work is validated end to end
through qemu's own gdbstub before the `skj-run` stub exists, so neither
blocks the other.

The deliverables, all binutils-free and in tree:

- **`skj-run --gdb`**: the RSP server built into the runner (Part 2).
- **`skj-dbg`**: a small in-tree RSP client, the debugger front end, grown
  from `tests/rv32/gdbclient.c`. Stock gdb is the other front end, and the
  oracle.
- **`skj-addr2line`**: a standalone address-to-`file:line` symbolizer over
  the DWARF a program carries, for use from the host or inside the sandbox
  (Part 3).
- **A zero-allocation backtrace** the host calls to dump a faulted sandbox
  process into a caller-provided buffer, shaped like Linux `backtrace()`
  (Part 3).

`skj-dbg`, `skj-addr2line`, and the backtrace share one DWARF reader, so the
line-program and symbol-table parsing is written once.

## Current state

**Execution control is already present in the RV32 core.** A host can
single-step (`rv_step`, `emu/rv32.c:2826`), run a bounded batch and learn why
it stopped (`rv_run` returning `RV_RUN_BUDGET/HALT/TRAP/YIELD`,
`emu/rv32.c:1930`, `emu/rv32.h:377`), read and write registers as plain
struct fields or through `rv_get_*`/`rv_set_*` (`emu/rv32.h:144`,
`emu/rv32.c:2835`), and read and write arbitrary guest memory, read-only text
included, through `gm_load`/`gm_store`/`gm_write`/`gm_read_out`
(`emu/guest.c:259` onward). The `ebreak` instruction already raises
`RV_CAUSE_BREAKPOINT` and makes `rv_run` return `RV_RUN_TRAP` with `mepc` at
the breakpoint (`emu/rv32.c:2601`, cause at `emu/rv32.h:284`). A GDB RSP
client and a RISC-V register layout already exist for lockstep testing
(`tests/rv32/gdbclient.{c,h}`).

**Debug information is the gap.** The only source-line mechanism is the
`IR_LOC` marker (`imm` carries the line, `ir/ir.h:185`), and `struct ir_insn`
has no line, column, or file field (`ir/ir.h:234`). `IR_LOC` is wired end to
end only on the kobold `-g` path into the JIT, where it builds an
address-to-line table that has no runtime consumer yet (`EmitLoc` at
`kobold/kpcom.pas:7004`, the JIT table at `jit/jit_common.c:123`). On the AOT
backends `IR_LOC` becomes a `; line N` comment (`backend/mc_text.c:271`) or a
no-op (`backend/arm64_mc_text.c:258`), and rv/mips/coldfire ignore it. The
assemblers swallow `.file` and know nothing of `.loc` or DWARF
(`as/rv_parse.c:441`, `as/mips_parse.c:369`). Object files carry a `.symtab`,
but every symbol is `STT_NOTYPE` with no size (`as/rv_elf.c:224`), and the
linker emits no symbol table at all in the final executable and no debug
sections (`ld/elf_write.c:199`, discard set at `ld/script.c:526`,
`ld/rv_link.c:425`).

So a program can be controlled and inspected today, but no address maps back
to a source line or a function name once it is linked. That is what Part 1
builds.

## Part 1: DWARF 4 line information

### The format-neutral line table in the IR

The compiler keeps one internal table, independent of how it is later
written. Per function it is a list of rows:

```
(code_offset, file_id, line, column, is_stmt)
```

The upstream work that produces this table is the same whatever the final
encoding would be. It is the bulk of the effort and the part missing today:

- **Front ends.** Every lexer already tracks a line (used only for
  diagnostics). Each front end must attach the current source position to the
  IR it lowers, not drop it. kobold already does this under `-g`; the others
  (cc, pascal, moo, tinc) do not.
- **The IR carries position.** Extend `IR_LOC` to carry a file id and column
  beside the line, or add position fields to the instruction node. A `-g`
  flag gates emission so a normal build is unchanged.
- **Backends record offsets.** Each backend, on seeing the position marker,
  records the current output offset against the row. Today only the x86 and
  arm64 selectors even observe `IR_LOC` (`backend/x86_select.c:855`,
  `backend/arm64_select.c:843`); every target needs to, through the shared
  sink interface.

The result is one collector feeding a single serializer. The serializer is
the only DWARF-specific stage.

### What gdb needs, and only that

gdb associates a line table with code through a compilation-unit DIE, so
three sections are required. Variable and type information (the expensive
`.debug_info` body) is out of scope for v1.

- **`.debug_line`**: the DWARF 4 line-number program. A small bytecode state
  machine (`DW_LNS_advance_pc`/`advance_line`, the special opcodes,
  `DW_LNE_set_address`, `DW_LNE_end_sequence`) that reproduces the row table.
  This is what `readelf` and gdb decode.
- **A skeleton `.debug_info` plus `.debug_abbrev`**: one compilation-unit DIE
  per object, carrying `DW_AT_name`, `DW_AT_comp_dir`, `DW_AT_low_pc`,
  `DW_AT_high_pc`, `DW_AT_stmt_list` (the offset of this CU's line program),
  `DW_AT_language`, and `DW_AT_producer`. This is the gotcha worth stating
  plainly: `readelf` and `addr2line` read `.debug_line` standalone, but **gdb
  will not attach a line table without a CU DIE pointing at it**. The CU is a
  fixed small skeleton, not per-variable DIEs, so it is cheap.
- **A real `.symtab`**: function names come from `STT_FUNC` symbols with
  correct `st_size`, which also give gdb backtraces by name. This avoids
  needing `DW_TAG_subprogram` DIEs in v1.

**DWARF version 4** is chosen over 5. The version 4 `.debug_line` header is
the simplest to hand-encode and is read by every tool used as an oracle.
Version 5 reworked the file and directory tables in the line-program header
and buys nothing here.

### Pipeline work beyond the collector

- **Assembler.** Emit the three debug sections. The `DW_LNE_set_address`
  entries and the CU's `low_pc`/`high_pc` are absolute addresses, so the
  assembler must emit relocations inside the debug sections (an `R_RISCV_32`
  against the code symbol), which it does not do for anything but code and
  data today.
- **Linker.** Keep `.debug_line`/`.debug_info`/`.debug_abbrev` instead of
  dropping unmatched sections, apply the relocations inside them, and re-emit
  a `.symtab` with `STT_FUNC` and sizes. None of this exists in `ld` now
  (`ld/elf_write.c:199`).

### Verification: the oracle ladder

The debug info is correct when reference tools agree with it, before any
skjegg debugger reads it. From cheapest to fullest:

```sh
readelf --debug-dump=decodedline prog   # rows match the source lines?
readelf --debug-dump=rawline   prog      # line program encoded correctly?
llvm-dwarfdump --debug-line     prog
addr2line -e prog 0x<addr>               # an address maps to the right line?
```

Then end to end through qemu's gdbstub, which needs no skjegg debugger at
all:

```sh
qemu-riscv32 -g 1234 ./build/prog &
gdb-multiarch ./build/prog        # or riscv64-linux-gnu-gdb
(gdb) target remote :1234
(gdb) break foo.c:42
(gdb) step / next / bt
```

This is the same golden-master discipline the project already applies to
instruction encoding (skj-as against GNU as) and to RV32 conformance (against
qemu signatures). It decouples debug-info correctness from the debugger
engine entirely.

## Part 2: the RSP stub in skj-run

### The split

`skj-run --gdb <port>` listens for a GDB remote-serial-protocol connection.
The emulator stays the execution core; the debugger front end is a separate
process. The front end is a stock gdb (reading the DWARF from Part 1) or a
small in-tree client grown from `tests/rv32/gdbclient.c`, which already has
the RISC-V register layout.

### The packet set

RSP maps almost one to one onto the API the core already exposes, so the stub
is thin. The v1 set, and what each one does:

| RSP packet    | Behavior and core call                                      |
| ------------- | ----------------------------------------------------------- |
| `?`           | Report the stop reason for the current state. Translate the last `rv_run` reason plus `mcause` into a stop reply (a trap, a breakpoint, a signal). |
| `g` / `G`     | Read and write registers over one fixed layout. `rv_get_*` / `rv_set_*`. `x0` reads as zero and writes to it are ignored (the core already hardwires this). |
| `m` / `M`     | Read and write memory. `gm_read_out` / `gm_write`, with the address and length bound-checked and an RSP error (`E`*nn*) returned for an invalid access. |
| `c`           | Continue. Resume from the current PC and send nothing until the target halts, then send a stop reply. The `rv_run` loop with a breakpoint check between batches. |
| `s`           | Single-step. `rv_run(cpu, 1, &n)`, then a stop reply. |
| `Z0` / `z0`   | Insert and remove a software breakpoint. Software breakpoints only to start; no hardware or watchpoint packets. This drives the host-side breakpoint set below. |

The register layout is advertised, not assumed. RV32 is the 32 integer
registers plus PC; `x0` is zero. The float registers (`f[32]` plus `fcsr`,
present in the core) are added only once the layout includes them, not
piecemeal. gdb learns the layout from a target description served over
`qXfer:features:read` (the `org.gnu.gdb.riscv.cpu` feature), and the
`gdb_state` register ordering in `tests/rv32/gdbclient.h:14` already matches
RISC-V gdb numbering, so the `g`/`G` encoding reuses it.

### Interrupt and feature negotiation

- **Ctrl-C (the raw `0x03` interrupt byte).** While the target is running
  under `c`, the stub polls the connection for the interrupt byte between
  `rv_run` batches and stops the target so gdb can regain control. This is the
  one place the stub reads the wire mid-run.
- **`qSupported`.** Advertise only the features actually implemented
  (`PacketSize`, `qXfer:features:read+` for the target description, `swbreak+`
  if the stub reports breakpoint stops that way). Every packet not implemented
  gets the empty reply, which the protocol reads as "unsupported", so gdb
  falls back correctly. A first version can answer the empty reply to anything
  outside the table above and still drive a usable session.

### Breakpoints: host-side set first

Because the debugger is the host, it can keep breakpoint addresses in a
host-side table and compare `pc` during a single-step loop, rather than
patching `ebreak` into guest memory. This is the v1 choice because it sidesteps
both hazards the core has:

- The decoded-instruction cache (`cpu->dec`, `emu/rv32.c:1902`, hit path at
  `:1964`) assumes guest code never changes and has no per-PC invalidator,
  only `fence.i`'s full flush (`emu/rv32.c:2279`). A planted `ebreak` would be
  shadowed by a stale cached decode. The scheduler enables the cache for every
  process (`emu/rv_user.c:381`), so this is the default configuration.
- `ebreak` vectors through the guest's `mtvec` and mutates `mepc`/`mcause`/
  `mstatus` (`rv_trap`, `emu/rv32.c:138`), so a planted breakpoint's trap must
  be caught and those side effects undone before the guest handler runs.

A host-side set touches neither guest memory nor the trap machinery. Its cost
is that `rv_run` cannot run a full batch across a region that contains a
breakpoint, so the stub single-steps there and runs full batches elsewhere.

The planted-`ebreak` scheme stays available as a later optimization for
full-speed running between breakpoints. It requires flushing the whole decode
cache on every plant and remove (reusing the `fence.i` loop) and intercepting
the `RV_RUN_TRAP` with `mcause == RV_CAUSE_BREAKPOINT` before the guest
handler, then restoring the original word and the trap registers.

### Hooking the sandbox

The scheduler's per-process slice is the hook. `proc_slice`
(`emu/rv_user.c:451`) today calls `rv_run` once for a quantum and dispatches
on the reason (`:477` onward). In debug mode it runs the step-and-check loop
instead, and intercepts the `RV_RUN_TRAP` case so a debugger breakpoint is
distinguished from a genuine guest fault before the scheduler would kill the
process. The per-process `rv_cpu` and guest memory are both in hand there
(`&p->cpu`, `p->g.mem`).

For v1 the stub debugs the root process. The sandbox is multi-process (each
child has its own `rv_cpu` and isolated memory, spawned over `RV_SYS_SPAWN`),
and RSP's multiprocess and multi-thread extensions can expose children later;
that is deferred. No new guest syscall is needed: control is entirely
host-driven, so the private ecall block (`emu/rv_user.c:21`, the `0x000f`
range) is untouched.

## Part 3: the reader, skj-addr2line, and the backtrace

Part 1 writes DWARF. This part reads it. One reader serves three consumers:
`skj-dbg`'s and gdb's symbolization, the standalone `skj-addr2line`, and the
host's fault backtrace. It parses the `.debug_line` program into a sorted
address-to-`(file, line)` row table and the `.symtab` into an
address-to-function table, both as a binary search over arrays, with no
per-lookup allocation.

### skj-addr2line

A standalone tool matching GNU `addr2line`: given an ELF and one or more
addresses, print `function` and `file:line` for each.

```sh
skj-addr2line -e prog 0x10234          # -> frob\nfoo.c:42
```

It is the reader plus a thin CLI, binutils-free. "For the host or the
sandbox" means two call sites share it: `skj-run` links the reader in to
format a guest fault without shelling out, and a guest can run a
sandbox-resident build of the same tool over an ELF image it holds. The tool
owns no state beyond the reader context it builds once from the ELF.

### Zero-allocation backtrace

The host dumps a faulted sandbox process into a caller-provided buffer,
shaped like Linux `backtrace()` / `backtrace_symbols()` but with no
allocation on the dump path. It is two steps, matching the Linux split:

1. **Unwind into a caller array.** `skj_backtrace(cpu, uint32_t *pcs, int
   max)` walks the guest's call frames and fills `pcs` with return addresses,
   returning the count. The caller owns `pcs`; the call allocates nothing.
   The frame walk reads guest memory through `gm_read_out`, following a
   frame-pointer chain: each frame stores the saved return address and the
   caller's frame pointer at fixed offsets. This requires the backend to
   maintain that chain under `-g` (a small prologue/epilogue change), which is
   the simplest zero-allocation unwind. The alternative, a DWARF
   `.debug_frame` CFI interpreter, needs no ABI change but is heavier and
   wants scratch state; the frame-pointer chain is the v1 choice, with CFI
   noted as the later option if a frameless `-g` build is wanted. **This is
   the one open prerequisite in the plan**: whether to spend the backend
   frame-pointer change or build a CFI reader.

2. **Symbolize into a caller buffer.** A formatter takes the `pcs` array and a
   caller-provided `char *buf, size_t len`, and writes bounded lines
   (`  #N  0xADDR  func at file:line`) through the reader, truncating with a
   marker when the buffer fills. No allocation; the DWARF reader context is
   built once by the host, not per backtrace.

On a guest fault the scheduler already reports cause, `mepc`, and `mtval` in
`proc_slice` (`emu/rv_user.c:499`). The backtrace extends that: seed the walk
from `mepc` and the faulting frame pointer, and append the symbolized frames
to the same report, bounded by the caller's buffer.

## Sequencing

1. The format-neutral line table: carry position from every front end into
   the IR, and have every backend record offsets. No DWARF yet.
2. The DWARF 4 serializer: `.debug_line`, the skeleton `.debug_info` and
   `.debug_abbrev`, and `STT_FUNC` symbols. Validate with `readelf` and
   `addr2line` (static), then `qemu-riscv32 -g` with gdb (end to end).
3. Assembler and linker: emit and relocate the debug sections, preserve them
   through the link, re-emit `.symtab`. This extends roadmap item 7 in
   `PLAN.md` (DWARF passthrough) with the emit side.
4. The shared DWARF reader and `skj-addr2line`. Validated against GNU
   `addr2line` on the same ELF.
5. The zero-allocation backtrace, after deciding the unwind mechanism
   (frame-pointer chain or CFI).
6. The RSP stub in `skj-run` and the `skj-dbg` client, host-side breakpoint
   set, hooking `proc_slice`. By this point the debug info is already
   gdb-validated, so the stub is the only new thing under test.

## Out of scope for v1

- Variable and type inspection (the `.debug_info` body: `DW_TAG_variable`,
  `DW_TAG_base_type`, location expressions). This is the expensive DWARF
  territory and a later milestone.
- DWARF for the native x86-64 and arm64 outputs so stock gdb debugs them
  directly. The guest path (RV32 under qemu or skj-run) is the controllable
  one and comes first.
- Multi-process and multi-thread RSP.
- A custom leaner line-table section for skj-run's own use. DWARF is the only
  format until there is a concrete need for a second one; if that need
  arrives, the format-neutral table from step 1 serializes to it with no
  upstream change.
