# skj-as-x86-64 x86-64 Instruction Report

`skj-as-x86-64` is the in-tree x86-64 assembler. It reads GNU
`.intel_syntax noprefix` and writes little-endian ELF64 RELA relocatable objects
(`EM_X86_64`, ELFCLASS64). It replaced `nasm` in 0.26.0 and is the default
assembler for the x86-64 build (`X64_ASM`). Linking is external (GNU `ld`, or
`gcc -no-pie`); there is no in-tree x86-64 linker.

It is a bounded subset that encodes exactly what the x86-64 backend and the
hand-written runtime emit, not a general-purpose x86 assembler. The byte recipes
mirror the JIT encoder (`jit/emit_x86.c`) and were checked byte-for-byte against
GNU `as` in intel mode (`make check-x86-64as`). Mnemonics match exactly with
`strcmp`, except the `j<cc>` and `set<cc>` prefix families.

## Supported Instructions

58 named mnemonics, plus the `j<cc>` and `set<cc>` condition-code families
(30 suffix aliases over 16 codes).

### Data movement (6)
- `mov` -- reg,reg / reg,imm / reg,`offset sym` / reg,[mem] / [mem],reg /
  [mem],imm (the 64-bit immediate `movabs` form is selected automatically)
- `movzx`, `movsx` -- from an 8/16-bit register or byte/word memory
- `movsxd` -- reg64 from r/m32
- `lea` -- always REX.W (64-bit destination)
- `movq`, `movd` -- between xmm and gpr

### Integer ALU (7)
- `add or and sub xor cmp` -- reg,reg / reg,imm / [mem],reg / reg,[mem]
- `test` -- reg,reg
- `imul` -- 2-operand reg,reg and reg,[mem]

### Unary and divide (4)
`neg not div idiv` (F6/F7 group)

### Shifts (4)
`shl sal shr sar` -- count by `cl`, by 1, or by imm8

### Stack (2)
`push pop` -- 64-bit register form

### Control (2 named + 2 families)
- `call jmp` -- rel32 symbol, r64, or `[mem]`
- `j<cc>` -- always rel32 (0F 80+cc)
- `set<cc>` -- r/m8
- Condition suffixes: `o no b c nae ae nb nc e z ne nz be na a nbe s ns p pe np
  po l nge ge nl le ng g nle`

### Zero-operand and misc (7)
`ret leave nop cqo cdq syscall`, and `rep` (emits `rep movsq` only)

### SSE scalar and packed (26)
`movsd addsd subsd mulsd divsd sqrtsd ucomisd`, `movss addss subss mulss divss
ucomiss`, `cvtss2sd cvtsd2ss cvtsi2sd cvtsi2ss cvttsd2si cvttss2si cvtsd2si`,
`xorps xorpd andps andpd`, plus `movq movd` above. (`movsd`/`movss` with a memory
destination use the 0F 11 store opcode.)

## Operands and Addressing

- **Registers**: `rax`-`rdi`, `r8`-`r15` (64-bit); the 32-bit (`eax`.., `r8d`..),
  16-bit (`ax`.., `r8w`..), and 8-bit (`al cl dl bl spl bpl sil dil`, `r8b`..)
  views; `xmm0`-`xmm15`. The legacy high-byte registers `ah ch dh bh` are not
  recognised. `rip` is valid only inside `[...]`.
- **Memory**: `[base]`, `[base+disp]`, `[base+index*scale+disp]`, `[rip+sym(+N)]`,
  and absolute `[sym(+N)]`. Index `rsp` means "no index".
- **Size qualifiers**: `byte`/`word`/`dword`/`qword`/`xmmword`, optionally with
  `ptr`, before `[`. Honoured by `mov`, `movzx`, `movsx`, `movsxd`, and
  `mov`-immediate.
- **Immediates**: decimal, hex, `-N`; `offset sym(+N)` as an address immediate.
- **Labels**: `name:`, `.Lname:`, numeric `N:` with `Nf`/`Nb`. `#` begins a
  comment.
- At most 3 operands.

## Assembler Directives

- Sections: `.text .data .rodata .bss`, `.section NAME` (prefix match;
  `.note.GNU-stack` is a no-op; unknown names keep the current section)
- `.intel_syntax` (arguments ignored)
- Symbols: `.globl .global .weak`, `.type NAME, function`, `.size`
- Data: `.byte`, `.word`/`.short`/`.value`/`.2byte`, `.long`/`.int`, `.quad`,
  `.ascii .asciz .string`, `.space .zero .skip`
- Alignment: `.align`/`.balign` (byte count), `.p2align` (exponent, clamped 0..12)
- `.include "file"`; accepted and ignored: `.file .loc .ident .local .set
  .cfi_*`
- An unknown directive is a warning, not an error.

Command line: `skj-as-x86-64 [-o out.o] input.s` (one input; defaults to stdout).
`-f FMT` / `-felf64` / `--64` / `--32` are accepted and ignored (output is always
ELF64).

## Relocations

| Constant | Value | Emitted for |
|---|---|---|
| `R_X86_64_64` | 1 | `.quad sym` |
| `R_X86_64_PC32` | 2 | `[rip+sym]` to a non-local target (addend -4) |
| `R_X86_64_PLT32` | 4 | `call`/`jmp`/`jCC` to a symbol (addend -4) |
| `R_X86_64_32` | 10 | `offset sym` into a 32-bit register; `.long sym` |
| `R_X86_64_32S` | 11 | absolute `[sym]`; `mov r64, offset sym`; qword `[mem],sym` |

Branches are always rel32 with no relaxation, so both passes agree on size. A
branch or `[rip+sym]` to a local symbol defined in the same section resolves
directly; a global or weak symbol keeps a relocation (so `call` to a same-file
global still gets PLT32).

## Not Supported / Limitations

- **Syntax**: Intel `.intel_syntax noprefix` only. AT&T syntax is not read; no
  macros, no conditional assembly, no expressions beyond `sym +/- N`.
- **Branches**: always rel32 (`jmp`/`jCC` 5-6 bytes, `call` 5 bytes); no short
  forms. Immediates and displacements are not range-checked and are truncated
  silently beyond 32 bits.
- **Rejected (hard error)**: unknown mnemonic or `jCC`/`setCC` suffix; an
  unsupported operand shape (e.g. `add [mem], imm`, `cmp qword ptr [mem], imm`);
  a missing symbol in a branch or rip-relative operand; more than 3 operands.
- **Absent from the dispatch** (die as unknown mnemonic): `movabs` (chosen
  automatically); x87, AVX/VEX/EVEX, segment, system/privileged; `cmov* bsr bsf
  xchg xadd cmpxchg lock pause hlt ud2 int3 mfence sfence lfence cdqe cwde bswap`;
  the aligned/packed SSE moves `movaps movapd movups movdqa pxor addps`; `comisd
  comiss sqrtss`; and every string form other than `rep movsq`.
- **Silent misencodes (not diagnosed)**: several paths read a register field
  where the backend never supplies memory or an immediate, so hand-written input
  can misencode without warning. Known cases: `push`/`neg`/etc. with a memory
  operand encode the `rax` register form; `imul reg,reg,imm` and `test reg,imm`
  drop the immediate; a memory operand with no base (`[idx*8+disp]`) or an extra
  symbol (`[rbx+sym]`) drops the index/symbol; `add rax, offset sym` drops the
  relocation; `rep stosq` encodes as `rep movsq`; `cvtsi2sd xmm,r64` hard-wires
  W=0. A symbol element in `.word`/`.byte` is dropped. These shapes are never
  emitted by the backend, so the risk is confined to hand-written assembly.

## Summary

A bounded x86-64 subset: 58 named mnemonics plus the `jCC`/`setCC` families, SSE
scalar and the handful of packed bitwise ops the backend uses, GNU intel syntax
in and ELF64 RELA out, always-rel32 branches. It covers the backend's full
emitted set and the runtime, byte-for-byte equal to GNU `as` over the test
corpus, and replaced the former `nasm` dependency.
