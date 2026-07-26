# x86-16 Real Mode Feasibility Study

Exploring what it would take to support 16-bit real mode, multiple memory
models (tiny, small, large), overlays, and far attributes in skjegg's cc,
as, and ld.

Date: 2026-05-10


## Background: x86-16 Real Mode

Physical address = (segment << 4) + offset, yielding a 20-bit address space
(1 MB). Each segment register defines a 64 KB window. Segment registers pair
with offset registers by hardware convention:

| Segment | Offset(s)       | Purpose                              |
|---------|-----------------|--------------------------------------|
| CS      | IP              | instruction fetch (hardwired)        |
| SS      | SP, BP          | stack operations                     |
| DS      | SI, BX, others  | default data access                  |
| ES      | DI              | string destination (MOVSB, STOSB)    |

Explicit segment override prefixes (CS:, ES:, SS:) are possible but cost an
extra byte per instruction. Compilers assume the traditional pairings and
rarely override them.


## Memory Models

**Tiny** (CS = DS = SS, one 64 KB segment for everything):
All pointers are 16-bit near. Code + data + stack share one segment. Output
is a flat .COM binary. No segment arithmetic at all.

**Small** (CS != DS, each up to 64 KB):
Code in one segment, data+stack in another. All pointers are 16-bit near
within their respective segments. Default for DOS .EXE. No far pointers
needed.

**Large** (multiple code segments, multiple data segments):
Both code and data pointers are 32-bit far (segment:offset). Functions
called with CALL FAR, return with RETF. Data accessed via LDS/LES or
explicit segment register loads. Can address the full 1 MB.

Intermediate models (compact, medium) mix near and far pointers. A
production compiler would eventually want them, but tiny/small/large cover
the design space: one segment, two segments, many segments.


## Far Pointers

A far pointer is a 32-bit value: 16-bit offset followed by 16-bit segment
(in memory, little-endian). In registers it occupies a segment:offset pair,
for example DS:BX or ES:DI.

Far call: pushes CS then IP, loads new CS:IP from the operand.
Far return (RETF): pops IP then CS.
Far data: LDS reg, [mem] loads offset into reg and segment into DS.

Far pointer comparison is tricky: two different segment:offset pairs can
alias the same physical address. Compilers sidestep this by normalizing
pointers (huge model) or simply documenting the limitation.


## How Other Compilers Handle This

**Turbo C / Borland C**: `far` qualifier on pointer types. The compiler
treats far pointers as 32-bit values in its IR, splitting into
segment:offset only during code generation. Function prototypes carry a
near/far attribute that selects CALL vs CALL FAR.

**Open Watcom**: Same approach. Far pointers are 32-bit opaque values.
Register allocator assigns register pairs (e.g., DX:AX for return values).
Segment register management is the backend's responsibility.

**ia16-gcc**: Extends GCC's RTL with `__attribute__((far))` on functions and
explicit far pointer types. The core IR still treats pointers as integers;
segment:offset splitting happens in the machine description. This was a
painful retrofit because GCC's IR assumes flat pointers everywhere.

Common pattern: **defer segmentation to the backend**. The IR carries a
"near vs far" annotation on pointer-typed values but does not model segment
registers. The emitter produces the right instruction sequences.


## Skjegg's Current Architecture

### IR

110 opcodes in flat 3AC form. All addresses are single 32-bit temps. Key
structures:

```c
struct ir_insn {
    int op, dst, a, b;
    long imm;
    char *sym;
    int slot, label, nargs;
    struct ir_insn *next;
};

enum ir_basetype { IR_I32=1, IR_I8=2, IR_I16=3, IR_F64=4, IR_I64=5 };
```

No pointer-width field. No address-space annotation. IR_LEA produces a
symbol address in one temp. IR_CALLI expects a function pointer in one temp.
Memory ops (IR_LW, IR_SW, IR_LB, etc.) take an address in one temp.

### x86-32 Backend

Targets i686 protected mode (flat 32-bit). NASM syntax output.
3 allocatable integer regs (ebx, esi, edi), 7 SSE2 float regs (xmm1-7).
Calling convention: cdecl, args on stack, return in eax.
64-bit integers via esi:edi pair and libgcc-style helpers.

### Assembler (skj-as)

ColdFire/m68k only. 170+ hand-coded instruction encoders. Operand types,
register names, effective address encoding, and all opcodes are 68k-specific.
Two-pass design (pass 1: sizes, pass 2: emit) is architecturally sound but
the implementation is not parameterized. Reuse for x86-16: near zero.

### Linker (skj-ld)

ELF32 big-endian, EM_68K. Supports R_68K_32, R_68K_PC32, R_68K_PC16.
Linker script parser (MEMORY, PHDRS, SECTIONS) is target-independent.
Relocation application is a simple formula switch. Symbol table stores a
single `value` (flat address) per symbol. No segment concept.


## Fit Assessment

### What Fits Well

1. **Front end pattern**. The lex/parse/lower/main pipeline is
   language-specific, not target-specific. A C front end that parses `far`
   qualifiers and lowers them to annotated IR nodes would follow the existing
   pattern unchanged.

2. **Separate compilation of backends**. Adding a new backend means writing a
   regalloc + emitter pair. The x86-16 backend would be a new pair alongside
   the existing x86-32, ColdFire, and RISC-V backends.

3. **IR_I16 base type already exists**. The IR can represent 16-bit values.
   16-bit arithmetic, loads, stores all have opcodes (IR_LH, IR_SH, etc.).

4. **The IR already has a 64-bit pair concept**. IR_I64 values use register
   pairs in the ColdFire and x86-32 backends (esi:edi on x86). A far pointer
   is also a pair (segment:offset). The 64-bit machinery provides a template.

5. **Assuming traditional segment pairings** simplifies everything. If DS is
   always the data segment and CS is always the code segment, the backend
   does not need to allocate segment registers. They become implicit, like
   the stack pointer.

### What Does Not Fit

1. **No pointer annotation in the IR**. The IR has no way to say "this temp
   is a far pointer" vs "this temp is a near pointer." Every address is a
   typeless 32-bit value. A far pointer is also 32 bits, but it must be
   handled differently: split into segment:offset for loads, calls, and
   stores. Without an annotation, the backend cannot distinguish them.

2. **IR_LEA / IR_ADL produce flat addresses**. In real mode, a global
   symbol's address is a segment:offset pair. IR_LEA currently stores a
   symbol name and the backend emits a flat address load. For far data, the
   backend would need to emit a segment load (mov ax, SEG symbol) and an
   offset load (mov bx, OFFSET symbol) separately.

3. **IR_CALLI assumes a single-temp function pointer**. A far function
   pointer is a segment:offset pair. IR_CALLI would need to either accept a
   pair or the backend would need to know that a given temp contains a far
   pointer and split it before the indirect call.

4. **Calling convention differences**. CALL FAR pushes 4 bytes (CS:IP) vs
   CALL NEAR's 2 bytes (IP only). The stack frame layout changes. RETF vs
   RET. The IR's IR_CALL/IR_RET do not encode this distinction.

5. **The linker has no segment concept**. Symbols have one address. For real
   mode, each symbol needs a segment assignment. Relocations include segment
   relocations (fill in the segment selector of a far pointer) and offset
   relocations (fill in the offset). skj-ld's symbol table and relocation
   model would need extending.

6. **The assembler is ColdFire-only**. A new x86 assembler is needed. The
   two-pass architecture is reusable as a pattern but not as code.

7. **ELF is the wrong object format**. DOS toolchains use OMF (Object Module
   Format) or MZ executables. ELF can represent x86-16 code (ia16-gcc uses
   ELF) but it is unusual and most DOS tools do not understand it. OMF
   has native support for segment relocations, overlay records, and memory
   model metadata.


## What Would Need to Change

### IR Changes (small)

Add a `flags` field to `ir_insn` or extend the opcode set:

**Option A: flag bit.** Add `int flags` to ir_insn. Define IR_FAR = 1.
IR_CALL with IR_FAR means far call. IR_CALLI with IR_FAR means far indirect
call. IR_LEA with IR_FAR means load a far address (segment:offset pair).
IR_RET / IR_RETV with IR_FAR means RETF. Memory ops with IR_FAR mean "the
address temp is a far pointer; load segment register before access."

**Option B: new opcodes.** IR_CALL_FAR, IR_CALLI_FAR, IR_LEA_FAR, IR_RET_FAR,
IR_LW_FAR, IR_SW_FAR, etc. More explicit but adds ~15 opcodes.

**Option C: treat far pointers as IR_I32.** The front end packs
segment:offset into a 32-bit value. The backend knows that certain temps are
far pointers from context (function is declared far, pointer type is far).
This is closest to what Turbo C did. Requires the front end to communicate
"farness" through some side channel, perhaps an attribute on ir_func or a
type table.

Option A is the least invasive. The flag bit is ignored by backends that do
not care about it (ColdFire, RISC-V, x86-32).

### cc Changes (medium)

The C front end needs:

- `far` and `near` type qualifiers (or `__far` / `__near` for standards
  compliance).
- Memory model selection flag (-mtiny, -msmall, -mlarge) that sets defaults
  for pointer sizes and function calling conventions.
- In large model: all function pointers default to far, all data pointers
  default to far, unless explicitly `near`.
- In small model: all pointers default to near, `far` is an explicit
  override.
- sizeof(near pointer) = 2, sizeof(far pointer) = 4.
- Type checking: near-to-far promotion is implicit (just set segment to
  current DS/CS). far-to-near is a warning (loses segment).
- Lower phase: emit IR_FAR flag on calls/loads/stores involving far pointers.

### x86-16 Backend (large, new file)

A new backend pair: `regalloc_x86_16.c` + `x86_16_emit.c`.

Register allocation for 16-bit real mode:

| Register | Role                                    |
|----------|-----------------------------------------|
| AX       | return value, scratch, multiply lo      |
| BX       | allocatable, DS-relative addressing     |
| CX       | shift count, loop counter               |
| DX       | multiply hi, I/O port, scratch          |
| SI       | allocatable, DS-relative addressing     |
| DI       | allocatable, ES-relative addressing     |
| BP       | frame pointer                           |
| SP       | stack pointer                           |
| CS       | code segment (implicit, read-only)      |
| DS       | data segment (implicit, rarely changed) |
| SS       | stack segment (implicit)                |
| ES       | extra segment (scratch for far data)    |

Allocatable general regs: BX, SI, DI (3 regs, same count as x86-32).
Segment registers: not independently allocated; tied to memory model.

Calling convention (cdecl-16):
- Args pushed right-to-left, 16 bits each (far pointers pushed as 32-bit
  segment:offset pairs, segment first, offset second, so offset is at lower
  address).
- Near return: RET (pops 16-bit IP).
- Far return: RETF (pops 16-bit IP, then 16-bit CS).
- Return value: AX (16-bit int), DX:AX (32-bit long or far pointer).

Far pointer handling:
- Far call: `call far [seg:off]` or `call far symbol`.
- Far data load: `les di, [addr]` to load far pointer into ES:DI, then
  access via `es:[di+offset]`.
- Far data store: load segment into ES, offset into DI, then
  `mov es:[di], value`.
- Assuming traditional pairings: DS for near data, ES for far data
  temporaries, CS for code. No independent segment register allocation.

### x86-16 Assembler (large, new tool)

skj-as is ColdFire-only. A new `skj-as-x86` (or a mode flag) is needed.

Scope for x86-16 real mode:
- Intel syntax (matching NASM and the existing x86-32 emitter).
- Subset of x86-16 instructions: MOV, ADD, SUB, MUL, DIV, AND, OR, XOR,
  SHL, SHR, SAR, CMP, TEST, JMP, Jcc, CALL, RET, RETF, PUSH, POP, LEA,
  LDS, LES, INT, MOVS, STOS, CBW, CWD, NOP.
- Segment override prefixes (CS:, DS:, ES:, SS:).
- ModRM byte encoding (no SIB for 16-bit; 16-bit addressing uses a different
  ModRM table than 32-bit).
- Two-pass design can follow skj-as's architecture.
- Output: ELF32 little-endian EM_386, or OMF.

16-bit ModRM is simpler than 32-bit (no SIB byte). The 8 addressing modes
are: [BX+SI], [BX+DI], [BP+SI], [BP+DI], [SI], [DI], [disp16]/[BP], [BX].
Each with optional 8-bit or 16-bit displacement.

Estimated size: 1500-2500 lines (comparable to skj-as's ~2000 lines for
ColdFire).

### Object Format Between skj-as and skj-ld

ELF has no dedicated machine type for 16-bit x86. There is no EM_I8086 in
the specification. ia16-gcc works around this by reusing EM_386 (ELF32)
and adding non-standard relocations (R_386_16, R_386_PC16, R_386_SEG16).
This works but requires patched binutils; unpatched tools may silently
mishandle the objects.

Three options were considered:

**ELF32-LE with EM_386 (recommended).** skj-ld already parses ELF.
Changing endianness and machine type is mechanical: parameterize the
get/put helpers. Adding R_386_16/R_386_SEG16 relocations is a small
switch-case extension. The non-standard relocations are not a compatibility
problem because both ends of the pipeline (skj-as and skj-ld) are ours.
ELF's overhead (52-byte header, section headers, symbol/string tables) is
heavy for tiny 16-bit objects, but this is a development-time format, not
a shipping format.

**OMF (Object Module Format).** The native DOS format with first-class
segment records, overlay metadata, and memory model annotations. Designed
for exactly this use case. Compatible with WLINK, TLINK, and other
DOS-era linkers. However, it is completely different from ELF: a
record-oriented format with type bytes, length fields, and checksums.
Parsing OMF means a new reader in skj-ld (~500-800 lines) with no reuse
from the existing ELF reader. OMF is also complex and under-documented,
with multiple incompatible extensions (Microsoft, IBM, Borland).

**Custom minimal format.** A bespoke header, symbol table, relocation
table, and raw section data with no legacy baggage. Could be exactly as
simple as needed. However, no tool compatibility (objdump, readelf, nm
cannot inspect the objects), and designing a new object format is
maintenance burden that does not add user-facing value.

Decision: ELF32-LE with EM_386. We control both tools, so the non-standard
relocations are a private contract. If OMF output is ever needed for
compatibility with DOS-era linkers, that is an output format question for
skj-ld (like COM and MZ), not an intermediate object format question.

### Linker Changes (medium)

Extend skj-ld with:

- Little-endian support (parameterize get16/put16/get32/put32).
- x86-16 relocations in the relocation application switch:
  - R_386_16: 16-bit absolute offset within segment.
  - R_386_SEG16: 16-bit segment selector.
  - R_386_PC16: 16-bit PC-relative offset.
  - R_386_FAR32: 32-bit far pointer (offset:segment pair).
- Segment concept in the symbol table: add `int segment` to `ld_symbol`.
  The linker assigns segment base addresses during layout. Segment
  relocations fill in the segment base.
- Linker script SECTIONS block maps sections to segments.
- Machine type check in elf_read.c accepts both EM_68K and EM_386
  (or is parameterized by a target flag).

### Overlays (large, linker + runtime)

Overlay support requires:

1. **Linker**: overlay group definitions in the linker script. Sections
   assigned to the same overlay group share a memory region but are placed at
   different file offsets. The linker emits a relocation table for the
   overlay manager.

2. **Runtime**: an overlay manager loaded at startup. When code calls into an
   overlay that is not resident, the manager loads it from disk into the
   shared memory region, patches segment registers, and transfers control.

3. **Compiler**: functions in different overlays must use far calls.
   Functions in the same overlay can use near calls. The compiler needs
   overlay annotations (pragmas or attributes) so the lowering phase emits
   far calls at overlay boundaries.

4. **Calling convention**: overlay stubs. The linker generates a stub for
   each cross-overlay call. The stub calls the overlay manager with the
   overlay ID and entry index. The manager loads the overlay if needed and
   transfers control to the real entry point.

This is the most complex feature and is largely independent of the IR. It
lives in the linker and runtime. A minimal implementation could defer
overlays entirely and support only tiny/small/large models first.


## Output Formats

The linker currently outputs ELF32 big-endian executables via
`ld_write_exec()`. The linking phase (symbol resolution, relocation,
section layout) is separated from output writing, so adding new output
formats means writing new writer functions alongside the existing one.

### COM (flat binary)

No header. The entire program is a flat binary blob loaded at CS:0x0100.
The linker resolves all symbols to offsets from 0x0100, applies relocations,
and dumps raw bytes. No relocation table in the output. No segments.
Maximum size: 65280 bytes (64 KB minus 256-byte PSP).

Implementation: `ld_write_com()`, ~40 lines. Iterate output sections in
address order, write raw bytes. Validate total size fits in one segment.

### MZ (DOS .EXE)

A 28-byte header (padded to paragraph boundary), a relocation table, then
the load image:

```
Offset  Size  Field
0x00    2     'MZ' signature
0x02    2     bytes in last 512-byte page
0x04    2     total pages (512-byte blocks)
0x06    2     relocation entry count
0x08    2     header size in paragraphs (16-byte units)
0x0A    2     min extra paragraphs (BSS)
0x0C    2     max extra paragraphs
0x0E    2     initial SS (relative to load segment)
0x10    2     initial SP
0x12    2     checksum (usually 0)
0x14    2     initial IP
0x16    2     initial CS (relative to load segment)
0x18    2     relocation table file offset
0x1A    2     overlay number (0 for main program)
```

Each relocation entry is 4 bytes: a segment:offset pair pointing to a
16-bit word in the load image that holds a segment value. DOS adds the
actual load segment to that word at load time. This is the only relocation
type MZ supports: segment fixups.

Implementation: `ld_write_mz()`, ~120-150 lines. Build header, collect
segment fixups into the relocation table, write header + relocs + image.
The linking phase needs to record which relocations are segment fixups
(unresolved until load time) vs offset fixups (resolved at link time).

### OVL (overlay files)

There is no universal .OVL format. Borland, Microsoft, and custom overlay
managers each used their own. The common denominator: an .OVL is a
relocatable binary image with a small header.

```
Header:
    image size
    segment fixup count
    entry count (exported entry points)
    entry table (offset of each callable function)
Segment fixup table:
    array of offsets within the image where a segment value lives
Image:
    raw code + data
```

When the overlay manager loads an .OVL, it reads the image into memory at
a paragraph-aligned address, walks the fixup table adding the load segment
to each fixup location, and records the entry points.

The linker produces the root executable and one or more .OVL files. In the
root executable:

1. **Stubs** replace each overlay function's body. The stub calls the
   overlay manager with the overlay number and entry index.

2. **Overlay dispatch table** lists each overlay: filename, image size,
   number of entries, and current load state. The overlay manager indexes
   into this table.

3. **Overlay arena** is a memory region declared in the linker script where
   overlays are loaded. Its size determines how many overlays can be
   resident simultaneously.

The linker script would define overlay groups:

```
OVERLAY 0x30000 : AT(overlay_arena)
{
    .ovl_graphics { graphics.o(.text) graphics.o(.data) }
    .ovl_sound    { sound.o(.text) sound.o(.data) }
    .ovl_ui       { ui.o(.text) ui.o(.data) }
}
```

Sections in the same OVERLAY block share the same virtual address range
but are written to separate .OVL files.

Implementation: `ld_write_ovl()`, ~80-100 lines per overlay file. Simpler
than MZ because there is no EXE header, no SS/SP/CS/IP, no page structure.

The runtime overlay manager is ~200-300 lines of C, cross-compiled for the
target. It needs `ovl_call(int overlay_id, int entry_idx)` and file I/O
(DOS INT 21h: open, read, seek, close). Simplest policy: one overlay
resident at a time, evict on conflict. LRU caching across multiple arena
slots is an optimization for later.

### Stub Template Design

The linker should not contain machine code for any target. Hardcoding
x86-16 stub sequences in the linker would violate skjegg's retargetable
design and require linker changes for every new backend that wants overlay
support.

Instead, the backend provides stub templates as data in a special section
of the target's runtime object file, with relocations at the parameterized
holes. The linker instantiates the template once per overlay entry point,
patching the relocations with concrete values. This is the same
copy-bytes-and-apply-relocations operation the linker already performs on
normal code.

For x86-16, the runtime would include:

```asm
section .ovl_stub
    push word 0x0000    ; hole: overlay ID  (reloc at offset 1)
    push word 0x0000    ; hole: entry index (reloc at offset 4)
    jmp _ovl_dispatch   ; hole: manager     (reloc at offset 7)
```

A ColdFire overlay would provide a different template (`move.l #0, -(sp)`
+ `jsr _ovl_dispatch`). A RISC-V overlay would use `li` + `j`. The linker
mechanism is identical for all targets.

The linker needs to know three things, none of them target-specific:

1. A section named `.ovl_stub` is a stub template, not regular code.
2. Relocations against reserved symbols (`__ovl_id`, `__ovl_entry`,
   `__ovl_dispatch`) are the parameterized holes.
3. Instantiate once per overlay entry point, defining those symbols to the
   appropriate values each time.

This keeps all target-specific code in the backend runtime. The linker
performs mechanical copy and patch, which is already its core job. No
bytecode interpreter, no target-specific code in the linker, no new
mechanism beyond recognizing one special section name and three reserved
symbol names.

### Output format summary

| Format | Writer size  | Linker changes              | Runtime needed |
|--------|--------------|-----------------------------|----------------|
| COM    | ~40 lines    | none                        | none           |
| MZ     | ~120 lines   | segment fixup tracking      | none           |
| OVL    | ~100 lines   | overlay groups, stub instantiation | ~250 lines |


## Effort Estimate

| Component       | Scope              | Lines (est.) | Notes                       |
|-----------------|--------------------|-------------:|-----------------------------|
| IR changes      | flag bit + helpers | 50-100       | Option A (IR_FAR flag)      |
| cc front end    | far/near quals     | 300-500      | type system + lowering      |
| x86-16 backend  | new regalloc+emit  | 2000-3000    | comparable to x86-32        |
| x86-16 asm      | new tool           | 1500-2500    | 16-bit ModRM is simpler     |
| linker changes  | segments + relocs  | 500-800      | extend skj-ld               |
| output: COM     | writer             | 40           | flat binary, trivial        |
| output: MZ      | writer + fixups    | 150-200      | header + segment relocs     |
| output: OVL     | writer + stubs     | 200-300      | template instantiation      |
| start_x86_16.S  | new runtime        | 100-200      | DOS INT 21h or bare metal   |
| overlay manager | runtime            | 200-300      | optional, defer initially   |
| **Total**       |                    | **5000-9500**|                             |


## Optimization: Matching 1980s Compiler Quality

The current IR has no optimization passes. On a register-starved 8086 this
matters more than on ColdFire (6 regs) or RISC-V (11 regs). The question is
what it would take to match an average 1980s C compiler, not a modern one.

### The baseline: what 1980s compilers actually did

The reference point is Turbo C 2.0 (1988) or Microsoft C 5.0 (1987). These
were fast single-pass compilers, not optimizing powerhouses. Watcom was the
outlier on the high end. The average 1980s compiler did constant folding,
basic peephole optimization, reasonable register allocation, strength
reduction for obvious cases, and jump threading. It did not do
loop-invariant code motion, global CSE, induction variable elimination,
graph coloring, or interprocedural analysis.

### The core problem: LDL/STL round-tripping

The biggest gap is not a missing optimization; it is the "SSA within, memory
between" design. Every value that crosses a basic block boundary goes to the
stack (IR_STL) and back (IR_LDL). On an 8086 with 3-4 usable general
registers, this generates catastrophic memory traffic. A Turbo C-class
compiler keeps values in registers across blocks using simple liveness
tracking. This is where 80%+ of the code quality gap lives.

### Optimizations ranked by impact

**1. Reload elimination** (highest impact, medium difficulty)

Track which slots are already in registers at block entry. If
`t5 = LDL slot3` and slot3 was stored from a register that is still live,
skip the load. This is not a full global register allocator; it is a cheap
fixup pass over the existing linear scan output. It directly addresses the
LDL/STL round-trip problem without redesigning the IR.

**2. Peephole optimization on emitted assembly** (high impact, easy)

Pattern matching on the backend's output to eliminate mechanical waste:

- `mov ax, bx; mov bx, ax` → delete second
- `add ax, 0` → delete
- `mov ax, [bp-4]; mov [bp-4], ax` → delete second
- `push ax; pop ax` → delete both
- `jmp L1` where L1 is the next instruction → delete

Every 1980s compiler did this. It catches artifacts from the independent
regalloc + emission pipeline. Implementation: a small pattern table applied
to the assembly line buffer before writing output. 200-400 lines.

**3. Strength reduction for multiply/divide** (high impact, easy)

MUL on 8086 takes 70-150 cycles. Replace multiply by constant with
shift/add sequences:

- `* 2` → `shl ax, 1`
- `* 3` → `mov bx, ax; shl ax, 1; add ax, bx`
- `* 5` → `mov bx, ax; shl ax, 2; add ax, bx`
- `* 2^n` → `shl ax, n`

Divide by power of 2 → arithmetic shift right. This was universal even in
poor compilers. Can be done in the IR (transform IR_MUL with constant
operand) or in the backend during emission.

**4. Constant folding** (medium impact, easy)

Evaluate `IR_ADD t1, t2` where both operands are IR_LIC → single IR_LIC.
Straightforward single pass over each basic block. Every 1980s compiler did
this. Catches cases the front end did not fold (multi-step constant
expressions, sizeof arithmetic, enum values).

**5. Dead code elimination** (medium impact, easy)

Remove instructions whose destination temp is never read. Simple backward
liveness scan per basic block. Catches artifacts from lowering (temps
computed for side effects that were later removed, unused return values).

**6. Jump threading** (low impact, easy)

`BZ L1` where `L1: JMP L2` → `BZ L2`. Single pass over the IR. Eliminates
chains left by the front end's structured control flow lowering. Every 1980s
compiler did this.

### What you do NOT need

These optimizations are beyond what the average 1980s compiler did:

- Loop-invariant code motion
- Global common subexpression elimination
- Induction variable elimination
- Graph coloring register allocation
- SSA construction / phi nodes
- Interprocedural analysis
- Alias analysis

### Implementation order

Items 1-2 (reload elimination + peephole) close most of the gap. Items 3-6
are incremental polish. A reasonable plan:

1. Peephole on assembly output (easiest, immediate payoff)
2. Strength reduction for multiply/divide (easy, big cycle savings on 8086)
3. Constant folding in the IR (easy, enables further simplification)
4. Dead code elimination (easy, cleans up IR before regalloc)
5. Reload elimination (medium difficulty, largest single improvement)
6. Jump threading (easy, minor cleanup)

Estimated total: 800-1500 lines across IR and backend, plus the peephole
table. These optimizations are architecture-independent and would benefit
all backends, not just x86-16.


## Conclusions

**Is skjegg a good fit?**

Conditionally yes. The architecture has three properties that make this
feasible rather than absurd:

1. **Backend isolation.** Adding a new backend is the normal extension path.
   A 16-bit backend is unusual but follows the same pattern as the existing
   three. The IR and front ends do not need to know about segment registers.

2. **IR_I64 as precedent.** The IR already handles 64-bit values as register
   pairs. A far pointer is structurally similar: a 32-bit composite value
   that the backend splits into two 16-bit halves. The machinery for pair
   allocation, spilling, and argument passing exists.

3. **Assumed segment pairings.** By not independently allocating segment
   registers, the backend avoids the hardest problem. DS is always the data
   segment. CS is always the code segment. ES is a temporary for far data
   access. This matches what Turbo C and Watcom did, and it means the
   register allocator only deals with general-purpose registers.

The main risk is scope creep. A minimal implementation (tiny + small models,
no overlays, no far pointers) is roughly 3000-4000 lines. Adding large model
with far pointers doubles it. Overlays add another 1000-2000 on top. The
sensible order:

1. tiny model (no segments, .COM output, near everything)
2. small model (CS != DS, still all near)
3. large model (far pointers, CALL FAR, LDS/LES)
4. overlays (defer indefinitely unless there is a real use case)

**What does not fit:** if the goal is a general-purpose DOS compiler
competing with Turbo C or Watcom, skjegg's 3-register allocator and
no-optimization IR will produce poor 16-bit code. The 8086 has very few
registers and 16-bit code is extremely sensitive to register pressure.
For embedded, hobby OS, or retro-computing projects where code quality
matters less than having a working self-hosted toolchain, it is viable.

**Object format question:** ELF works (ia16-gcc proves it) but is unusual
for DOS. OMF is native to DOS but adds linker complexity. For a first
implementation, ELF32-LE with EM_386 is pragmatic; an MZ .EXE writer on top
of skj-ld is simpler than implementing full OMF.
