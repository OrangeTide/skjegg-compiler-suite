# skj-as-aarch64 AArch64 Instruction Report

`skj-as-aarch64` is the in-tree AArch64 assembler. It reads the native GNU
AArch64 assembler syntax and writes little-endian ELF64 relocatable objects
(`EM_AARCH64`). It is the default assembler for the AArch64 build (`A64_AS`),
added in 0.26.0. Linking is external (GNU `ld`); there is no in-tree AArch64
linker.

It is a bounded subset that encodes exactly what the AArch64 backend and the
hand-written runtime emit, not a general-purpose assembler. The byte recipes
mirror the JIT encoder (`jit/emit_arm64.c`) and were checked byte-for-byte
against `aarch64-linux-gnu-as` (`make check-aarch64as`). Every instruction is one
fixed 32-bit word; there is no relaxation.

Addresses are formed through **literal pools** (`ldr Rt, =const|=sym` with
`.ltorg`), not `adrp`/`:lo12:`. There is no `b.cond`: the backend compares with
`cmp`/`fcmp`, materialises a flag with `cset`, and branches with `cbz`/`cbnz`.

## Supported Instructions

52 dispatched mnemonics (any other is a hard error). Loads and stores also have
a catch-all for any mnemonic beginning `ldr`/`str`.

### Control and system (5)
`nop`, `ret` (always x30), `svc #imm`, `br Xn`, `blr Xn`

### Integer arithmetic (4)
- `add`, `sub` -- immediate (`#imm`, optional `lsl #12`), extended-register when
  an operand is `sp` (UXTX/UXTW), or shifted-register
- `cmp` (alias for `subs` to zr, register form), `neg` (sub from zr)

### Logical (5)
`and`, `orr`, `eor`, `bic` (immediate form stored as `and` of `~imm`), `mvn`
(register form)

### Move (1)
`mov` -- resolves to `movz`, `movn`, or an `orr` bitmask for an immediate; `orr`
with zr for register-register; `add #0` to/from `sp`; `fmov` between FP
registers. An immediate that is none of those is a hard error (use `ldr =v`).

### Multiply, divide, shift, extend (9)
`mul` (madd with zr), `msub`, `sdiv`, `udiv`, `lsl`, `lsr`, `asr` (register
variable forms), `sxtw`, `cset Rd, cond`

### Loads and stores (10 named + catch-all)
`ldr str ldur stur ldrb ldrsb ldrh ldrsh strb strh`, and any other `ldr*`/`str*`
encodes as a plain load/store of the register width.

### Floating point (13)
`fmov fabs fneg fsqrt fmul fdiv fadd fsub fcmp fcvt scvtf fcvtzs fcvtns`

### Branch and address (5)
`b bl cbz cbnz`, and `adr` (dispatched; the backend never emits it)

Pool load: `ldr Rt, =const` / `ldr Rt, =sym` emits an LDR-literal word and
queues a deduplicated pool entry (4-byte entries before 8-byte; `=sym` becomes an
8-byte `ABS64`).

## Operands and Addressing

- **GPR**: `x0`-`x30`, `w0`-`w30`, `sp`/`wsp`, `xzr`/`wzr`. The names `fp` and
  `lr` are not accepted (use `x29`/`x30`).
- **FP**: `d0`-`d31`, `s0`-`s31`, `h0`-`h31`. `v`/`q` (vector) registers are not
  accepted.
- **Immediates**: `#imm` with optional `-`. Out-of-range values are not
  range-checked at the instruction (see Limitations); large constants go through
  the literal pool with `ldr =v`. There are no floating-point literals.
- **Memory**: `[Xn]`, `[Xn, #imm]`, `[sp, #imm]`. A load/store uses the scaled
  `imm12` form when the offset is a non-negative multiple of the access size that
  fits, otherwise the unscaled signed `imm9` form; `ldur`/`stur` force unscaled.
- **Shift modifier**: `lsl #12` on an add/sub immediate.
- **Condition codes** (for `cset`): `eq ne cs hs cc lo mi pl vs vc hi ls ge lt gt
  le al nv`.
- **Literal syntax**: `=<int>` or `=<sym>(+/-N)`.
- **Symbols**: bare identifiers, `.L` labels, numeric local labels (`1f`/`1b`),
  and `sym +/- N` addends on branch targets.
- At most 4 operands.

## Assembler Directives

- Sections: `.text .data .rodata .bss`, `.section NAME` (names beginning
  `.text`/`.rodata`/`.data`/`.bss` switch; others are ignored)
- Literal pool: `.ltorg`, `.pool`
- Symbols: `.globl .global .weak`, `.type sym, function`, `.size`
- Alignment: `.align`/`.p2align` (exponent, clamped 0..16), `.balign` (bytes)
- Data: `.quad`/`.xword` (8, symbol -> ABS64), `.word`/`.long`/`.int` (4, symbol
  -> ABS32), `.short`/`.hword`/`.2byte` (2), `.byte` (1), `.ascii .asciz
  .string`, `.space .zero .skip`
- `.include "file"`; accepted and ignored: `.file .loc .ident .local .arch
  .arch_extension .cpu .cfi_*`
- An unknown directive is a warning, not an error.
- Comments: `//` and `/* */`. `#` is the immediate sigil, not a comment.

Command line: `skj-as-aarch64 [-h] [-V] [-o out] in.s` (one input).

## Relocations

Emitted: `R_AARCH64_CALL26` (`bl`), `JUMP26` (`b`), `CONDBR19` (`cbz`/`cbnz`),
`ADR_PREL_LO21` (`adr`), `ABS64` (`.quad sym`, `ldr =sym`), `ABS32`
(`.word sym`). A branch or `cbz`/`cbnz` to a defined, non-global, non-weak,
same-section target resolves directly; globals, weak symbols, and cross-section
targets keep a relocation.

## Not Supported / Limitations

- **Fixed 32-bit encodings, no relaxation.** No `adrp`/`:lo12:`/`:got:`
  (addresses use literal pools). No `b.cond`/`tbz`/`tbnz` (the backend uses
  `cmp`/`fcmp` + `cset` + `cbz`/`cbnz`).
- **Rejected (hard error)**: unknown mnemonic (including `b.cond`, `adrp`,
  `ldp`/`stp`, `movk`, `tst`); a `mov #imm` that is not a single movz/movn/orr
  value; a logical immediate that is not a valid bitmask; a bad `=literal`
  (including a float); a non-symbol branch target; more than 4 operands.
- **Silent misencodes (not diagnosed)** -- the dispatch does not model these
  shapes, and the backend never emits them, so the risk is confined to
  hand-written assembly:
  - An immediate in a register-only alias (`cmp x0, #5`, and `neg`/`mvn`/`mul`/
    `sdiv`/`udiv`/`lsl`/`lsr`/`asr`, and the register form of add/sub) reads reg
    field 0, so `cmp x0, #5` encodes `cmp x0, x0`.
  - A negative or >4095 add/sub immediate is masked, not turned into `sub` or
    `lsl #12` (the backend chooses add/sub and the shift itself).
  - Shift-by-immediate (`lsl x0, x1, #3`) encodes the register-variable form
    shifting by x0; a shifted/extended register operand (`..., lsl #3`) is
    dropped.
  - A register-offset memory operand (`[Xn, Xm]`) drops the index; pre-index
    (`[Xn, #i]!`) and post-index (`[Xn], #i`) forms are silently discarded.
  - Load/store offsets are not range-checked (the unscaled imm9 wraps).
  - `fmov` with a GPR operand encodes an FP-to-FP move; `ldrsw` (and other
    unlisted `ldr*`) encodes a plain load with no sign extension.
- **Out of scope** (absent; the backend never emits them): pair loads/stores
  (`ldp`/`stp`), register-offset and pre/post-index addressing; atomics and
  exclusives (`ldxr`/`stxr`/`ldar`/...), barriers (`dmb`/`dsb`/`isb`); system
  (`mrs`/`msr`); NEON/SIMD and the `v`/`q` registers; scalar FP beyond the 13
  listed (`fsqrt` is dispatched but not emitted; no `fmadd`, `frint*`, `fcvtzu`,
  `fcsel`); and integer extras (`csel`/`csinc`, `ccmp`, `ubfx`/`sbfx`, the other
  extends). There is no in-tree AArch64 linker and no `.loc` DWARF emission.

## Summary

A bounded AArch64 subset: 52 dispatched mnemonics with a load/store catch-all,
fixed 32-bit words, literal-pool addressing instead of `adrp`, and no `b.cond`.
It covers the backend's full emitted set and the runtime, byte-for-byte equal to
`aarch64-linux-gnu-as` over the test corpus, and is the default AArch64 assembler
as of 0.26.0.
