# skj-as-rv RISC-V Instruction Report

`skj-as-rv` is the in-tree RISC-V assembler. It reads the GNU-assembler syntax
the RISC-V backend emits and writes little-endian ELF32 relocatable objects
(`EM_RISCV`). Linking is done by `skj-ld-rv` or an external linker.

It is a bounded subset that serves the toolkit's own output and hand-written
runtime, not a general-purpose assembler. The default assembler for the RISC-V
build is the external `riscv64-linux-gnu-as`; `skj-as-rv` is validated
byte-for-byte against it (`make check-rvas`, assembled `-mno-relax`) and is a
drop-in replacement.

ISA target: RV32 **I M A F D** plus **Zfh** (half precision), **Zicsr**,
**Zifencei** (`fence.i`), and the bit-manipulation sets **Zba / Zbb / Zbs**.
Mnemonics are matched case-sensitively in lower case.

## Supported Instructions

177 real instructions plus 32 pseudo-instructions. Base instructions are a
fixed 4 bytes; a few pseudo-ops expand to 8 bytes.

### Loads and stores (14)
- Integer loads: `lb lh lw lbu lhu`
- Integer stores: `sb sh sw`
- FP loads: `flh flw fld` -- FP stores: `fsh fsw fsd`

### Integer base, RV32I (24)
- Register-register: `add sub sll slt sltu xor srl sra or and`
- Register-immediate: `addi slti sltiu xori ori andi slli srli srai`
- Upper immediate: `lui auipc`
- Shift-immediate rotate: `rori`

### M extension (8)
`mul mulh mulhsu mulhu div divu rem remu`

### Bit manipulation
- Zba (3): `sh1add sh2add sh3add`
- Zbb register (9): `andn orn xnor min minu max maxu rol ror`
- Zbb unary (8): `clz ctz cpop sext.b sext.h orc.b rev8 zext.h`
- Zbs (8): `bclr bext binv bset` and the immediate forms `bclri bexti binvi bseti`

### Control transfer (10)
- Branches: `beq bne blt bge bltu bgeu`
- Jumps: `jal jalr`
- Also the upper-immediate pair `lui auipc` (listed above)

### Atomics, word only (11)
`lr.w sc.w amoswap.w amoadd.w amoxor.w amoand.w amoor.w amomin.w amomax.w
amominu.w amomaxu.w`

### System and CSR (10)
- CSR (Zicsr): `csrrw csrrs csrrc csrrwi csrrsi csrrci`
- System: `ecall ebreak fence fence.i` (operandless; `fence` is fixed to
  `iorw,iorw`)

### Floating point, F / D / Zfh (76)
Each arithmetic form exists in single (`.s`), double (`.d`), and half (`.h`):
- Arithmetic: `fadd fsub fmul fdiv fsqrt`
- Sign injection: `fsgnj fsgnjn fsgnjx`
- Min/max: `fmin fmax`
- Compare to integer register: `feq flt fle`
- Classify: `fclass`
- Bit moves: `fmv.x.w fmv.x.h fmv.w.x fmv.h.x`
- Conversions (18): the `fcvt.*` family between int and FP and across FP widths
- Fused multiply-add (12): `fmadd fmsub fnmsub fnmadd` in each width

### Pseudo-instructions expanded (32)
`mv neg not seqz snez nop li la call tail` (`li`/`la`/`call`/`tail` may expand
to two words); the zero-compare branches `beqz bnez blez bgez bltz bgtz`; the
swapped branches `bgt ble bgtu bleu`; the jumps `j jr ret`; and the FP moves
`fmv.{s,d,h} fneg.{s,d,h} fabs.{s,d,h}`.

## Operands and Addressing

- **Integer registers**: `x0`-`x31` and the ABI names (`zero ra sp gp tp t0-t6
  s0-s11 a0-a7`); `fp` is an alias for `s0`.
- **Float registers**: `f0`-`f31` and ABI names (`ft0-ft11 fs0-fs11 fa0-fa7`).
- **CSRs**: 22 named CSRs (`fflags frm fcsr cycle time instret ... mhartid`) or
  a numeric CSR number.
- **Rounding modes** (third operand of `fcvt` only): `rne rtz rdn rup rmm dyn`,
  or an integer. Other FP ops always encode `dyn`.
- **Immediates**: decimal, hex, `'c'` character constants, optional leading `-`.
- **Memory**: `imm(reg)` and `(reg)`; `%lo(sym)(reg)` and `%pcrel_lo(sym)(reg)`
  on loads and stores. Atomics take `(reg)` / zero offset only.
- **Relocation operators**: `%hi`, `%lo`, `%pcrel_hi`, `%pcrel_lo`, each with an
  optional `+/- N` addend. Any other `%name` is rejected.
- **Symbols**: bare identifiers, `.L` local labels, and numeric local labels
  (`1f`, `1b`).
- Up to 4 operands per instruction.

## Assembler Directives

- Sections: `.text .data .rodata .bss .sbss`, `.section NAME` (prefix match;
  unknown names fall to `.data`)
- Symbols: `.globl .global .weak`, `.type sym,@function`, `.size`, `.local`
  (ignored)
- Data: `.word .long` (4 bytes, integer or symbol), `.half .short .2byte` (2),
  `.byte` (1), `.ascii .asciz .string`, `.space .zero .skip`
- Alignment: `.align`/`.p2align` (power-of-two exponent, clamped 0..12),
  `.balign` (byte count)
- Macros: `.macro`/`.endm` (positional `\name` parameters) and `.include "file"`
- DWARF: `.file`, `.loc` (drive the generated `.debug_line`/`.debug_info`)
- Accepted and ignored: `.option .ident .attribute .cfi_*`
- Comments: `#`, `//`, and `/* */`
- An unknown directive is a warning, not an error.

## Relocations

Emitted: `R_RISCV_32` (`.word`/`.long` of a symbol), `R_RISCV_BRANCH`,
`R_RISCV_JAL`, `R_RISCV_CALL_PLT` (`call`/`tail`), `R_RISCV_PCREL_HI20` /
`PCREL_LO12_I` / `PCREL_LO12_S` (`auipc`+`addi`, `la`, `%pcrel_*`),
`R_RISCV_HI20` / `LO12_I` / `LO12_S` (`%hi`/`%lo`). A branch or jump to a
defined, non-global, same-section target resolves directly; globals, weak
symbols, and cross-section targets keep a relocation. The assembler emits no
linker-relaxation records.

## Not Supported / Limitations

- **Branch relaxation** exists for conditional branches only and only within one
  section: a same-section branch beyond +/-4 KiB becomes an inverted branch over
  a `jal` (8 bytes), sized to a fixpoint. `jal`/`j` is never relaxed;
  cross-section branches keep `R_RISCV_BRANCH` for the linker.
- **Rejected (hard error)**: unknown mnemonic or operand shape; more than 4
  operands; an I-type immediate/offset outside `[-2048, 2047]`; a shift amount
  outside `0..31`; a same-section `jal`/`j` beyond +/-1 MiB; an atomic with a
  non-zero offset.
- **Silent truncation (no diagnostic)**: `lui`/`auipc` immediates masked to 20
  bits, CSR immediates to 5 bits, `li` to 32 bits, `.byte`/`.half` to width. A
  symbol element in `.half`/`.byte`, or a `%reloc` in `.word`, ends the list
  early and drops the rest of the line.
- **Explicit rounding mode** is accepted only on `fcvt`; other FP ops are fixed
  to `dyn`. **Atomic ordering suffixes** `.aq`/`.rl` are not parsed (bits always
  zero). `fence` has only the fixed `iorw,iorw` form.
- **Pseudo-ops not expanded**: `sltz sgtz`; the CSR shorthands `csrr csrw csrs
  csrc csrwi csrsi csrci`; `rdcycle rdtime rdinstret`; the `frcsr/fscsr/frrm/...`
  family; `unimp`; and any RV64-only pseudo.
- **Out of scope** (absent from the dispatch; the toolkit never emits them):
  RV64I and its `*w` forms and 64-bit `fcvt`/`fmv`; compressed RVC (`c.*`); the
  privileged instructions (`mret sret wfi sfence.vma` ...); the vector (V)
  extension; scalar crypto (Zk*, Zbc/clmul); and the other optional extensions.

## Summary

A RV32IMAFD + Zfh + Zicsr + Zifencei + Zba/Zbb/Zbs subset: 177 instructions and
32 pseudo-ops, fixed 4-byte encodings with conditional-branch relaxation,
little-endian ELF32 output. It covers the backend's full emitted set plus the
hand-written runtime, and is byte-for-byte equal to `riscv64-linux-gnu-as` over
the test corpus.
