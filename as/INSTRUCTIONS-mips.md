# skj-as-mips MIPS Instruction Report

`skj-as-mips` is the in-tree MIPS assembler. It reads the GNU-assembler syntax
the MIPS backend emits and writes little-endian ELF32 `REL` relocatable objects
(`EM_MIPS`, MIPS I, o32). Linking is done by `skj-ld-mips` (which also writes the
PlayStation PS-EXE executable format) or an external linker.

It is a bounded subset that serves the toolkit's own output and hand-written
runtime, not a general-purpose assembler. The default assembler for the MIPS
build is the external `mipsel-none-elf-as`; `skj-as-mips` is validated against it
(`make check-mipsas`) and is a drop-in replacement. The encoder covers COP1
(hardware floating point); the backend's `MIPS_SOFTFLOAT` build (for the
coprocessor-less PlayStation R3051) routes float through `runtime/softfloat.c`
instead, so the FP instructions below are a property of the assembler, not of
that backend mode.

A `.set reorder` scheduler (a text pre-pass) fills branch and load delay slots;
`.set noreorder` regions are copied verbatim.

## Supported Instructions

106 mnemonics: 61 base instructions, 16 macros, and 29 COP1 forms.

### Integer ALU (18)
- 3-register: `addu subu add sub and or xor nor slt sltu`
- Immediate: `addiu addi slti sltiu andi ori xori` (form `rt, rs, imm`; `%hi`/
  `%lo` allowed)
- `lui`

### Shifts (6)
- Variable: `sllv srlv srav`
- Immediate: `sll srl sra` (shift amount masked to 5 bits)

### Multiply / divide and HI/LO (8)
- Raw: `mult multu div divu` (2-operand); HI/LO moves `mfhi mflo mthi mtlo`

### Loads and stores (12)
- Integer: `lb lh lw lbu lhu` / `sb sh sw`
- COP1: `lwc1 swc1`
- Form `rt, offset(base)`; `%lo(sym)(base)` allowed

### Control transfer (12)
- Branches: `beq bne` (`rs, rt, target`), `blez bgtz` (`rs, target`), and the
  REGIMM forms `bltz bgez` (`rs, target`) -- PC16
- Jumps: `j jal` (symbol, R_MIPS_26), `jr` (register), `jalr`
- System: `nop`, `syscall`, `break` (optional code)

### Floating point, COP1 (29)
- Moves/control: `mfc1 cfc1 mtc1 ctc1`
- Branches: `bc1f bc1t` (condition code 0)
- Arithmetic `.s`/`.d`: `add sub mul div`
- Unary `.s`/`.d`: `abs mov neg`
- Compare `.s`/`.d`: `c.eq c.lt c.le` (result to cc0)
- Conversions: the `cvt.{s,d,w}.{s,d,w}` family

### Macros expanded (16)
`move negu neg not`, `li la` (`li` up to 8 bytes; `la` always `lui`+`addiu`,
8 bytes), the branch macros `b beqz bnez`, the FP load/store macros `l.d s.d l.s
s.s`, and `mul` and the 3-operand `div divu rem remu` (with divide-by-zero and
signed `INT_MIN/-1` trap sequences; `$at` is clobbered).

## Operands and Addressing

- **Integer registers**: `$0`-`$31` and the o32 ABI names (`$zero $at $v0 $v1
  $a0-$a3 $t0-$t9 $s0-$s7 $k0 $k1 $gp $sp $fp $ra`, and `$s8` = 30). A leading
  `$` is required; bare `r4`/`4` is not a register.
- **FP registers**: `$f0`-`$f31` only (no named FP registers; no even-register
  check for doubles).
- **Immediates**: decimal/hex integers and `'c'` constants, optional leading `-`.
  Not range-checked for ALU/load/store/`lui` (masked to 16 bits) or
  `syscall`/`break` codes; `li` truncates to 32 bits.
- **Memory**: `offset(base)`, `(base)`, and `%lo(sym +/- N)(base)`. No indexed
  or register-plus-register addressing.
- **Relocation operators**: `%hi(sym +/- N)` and `%lo(sym +/- N)`.
- **Symbols**: bare identifiers and `.L` labels, as branch/jump targets and
  `.word` values.
- **Numeric local labels** (GAS `1:` / `1f` / `1b`): a digit label may be
  redefined; a forward reference (`1f`) binds to the next definition, a backward
  reference (`1b`) to the most recent one. Any non-negative number works, not
  just a single digit. Each definition gets a unique internal name, so they
  resolve through the ordinary local-label branch path.
- At most 4 operands. Comments start with `#`.

## Assembler Directives

- Sections: `.text .data .rodata .bss .sbss`, `.section NAME` (prefix match;
  unknown names fall to `.data`)
- Symbols: `.globl .global .weak`, `.type sym,function`, `.size`
- Data: `.word .long` (4, integer or symbol -> R_MIPS_32), `.short .half .2byte`
  (2), `.byte` (1), `.ascii .asciiz .asciz .string`, `.space .zero .skip`
- Alignment: `.align`/`.p2align` (exponent, clamped 0..12)
- DWARF: `.file`, `.loc`
- `.set` (its `reorder`/`noreorder` operand drives the scheduler; other operands
  are ignored). Accepted and ignored: `.ent .end .frame .mask .fmask .option
  .ident .local .gpword .cprestore .cfi_*`
- An unknown directive is a warning, not an error.

### Delay slots and `.set reorder`

- Default is `.set reorder`. The scheduler is a text pre-pass (`mips_sched.c`),
  separate from the encoder.
- **Branch delay slot**: the preceding instruction is hoisted into the slot when
  its mnemonic is hoistable (most ALU, shift-immediate, `move`, and `sw`/`sb`/
  `sh`) and the branch does not reference its destination register; otherwise a
  `nop` is inserted. `bc1f`/`bc1t` always take a `nop`.
- **Load delay slot**: a `nop` follows a load or COP1 move when the next
  instruction uses the loaded register.
- `.set noreorder` regions are emitted verbatim with no slot filling.
- Not handled by the scheduler: HI/LO (mult/div to `mfhi`/`mflo`) latency, and
  reordering across labels or branch targets.

## Relocations

Little-endian ELF32 `REL` (addend folded in place for local symbols). Emitted:
`R_MIPS_26` (`j`/`jal`, always, even same-section), `R_MIPS_PC16` (branches to an
external or cross-section target), `R_MIPS_HI16` (`%hi`, `lui`, `la`),
`R_MIPS_LO16` (`%lo`, `la`), `R_MIPS_32` (`.word sym` and DWARF address fields).

## Not Supported / Limitations

- **Rejected (hard error)**: unknown mnemonic or operand shape (`cannot encode
  '<mnem>'`); a malformed operand; more than 4 operands; a same-section branch
  displacement outside the signed 16-bit word range.
- **Silent acceptance / miscompile (not diagnosed)**: `%hi` on a memory operand,
  `%lo` on `lui`, and any reloc operator on the `l.d`/`s.d`/`l.s`/`s.s` macros
  encode offset 0 with no relocation; a symbol element in `.byte`/`.short` is
  dropped; operands on `nop`/`syscall`/`break` are ignored; FP double-register
  parity is unchecked; `.set` operands other than reorder/noreorder are ignored.
- **Not in the encoder** (absent from the dispatch): the REGIMM link branches
  `bltzal bgezal` (the plain `bltz`/`bgez` are supported); unaligned load/store
  (`lwl lwr swl swr`); atomics (`ll sc`), `sync`; coprocessor 0 (`mfc0 mtc0
  rfe`); trap-on-condition (`teq tne ...`); conditional move (`movf movt movz
  movn`); MIPS II+ FP (`sqrt floor ceil trunc round`); branch-likely (`beql bnel
  bc1fl bc1tl`); and `c.` conditions other than `eq`/`lt`/`le`.
- **Out of scope**: MIPS II and later, the full coprocessor and privileged sets,
  and any 64-bit (MIPS III+) forms. The assembler encodes MIPS I only.
- **PS-EXE**: the PlayStation executable format lives in the linker
  (`skj-ld-mips -f ps-exe`, base `0x80010000`), not the assembler.

## Summary

A MIPS I subset: 61 base instructions, 16 macros, and 29 COP1 forms, with a
`.set reorder` delay-slot scheduler and little-endian ELF32 `REL` output,
validated byte-for-byte against `mipsel-none-elf-as`. It covers the backend's
emitted set, including the REGIMM `bltz`/`bgez` the overflow-check path uses.
Unaligned access, atomics, and the coprocessor-0/privileged sets are out of
scope.
