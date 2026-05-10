# skj-as ColdFire Instruction Report

Inventory of instructions supported by `skj-as` and notable omissions
from the ColdFire ISA. Source of truth: `as/encode.c`, function `do_encode()`.

## Supported Instructions

### Data Movement (8)

| Mnemonic | Operands              | Sizes     | Notes                        |
|----------|-----------------------|-----------|------------------------------|
| move     | ea, ea                | .b .w .l  | general-purpose move         |
| movea    | ea, An                | .w .l     | move to address register     |
| moveq    | #imm8, Dn            | (long)    | 8-bit signed quick move      |
| movem    | reglist, -(sp) / (sp)+, reglist | (long) | push/pop register sets |
| lea      | ea, An                | (long)    | load effective address       |
| pea      | ea                    | (long)    | push effective address       |
| link     | An, #disp             | (word)    | create stack frame           |
| unlk     | An                    | --        | destroy stack frame          |

### Arithmetic (14)

| Mnemonic | Operands              | Sizes     | Notes                        |
|----------|-----------------------|-----------|------------------------------|
| add      | ea, Dn / Dn, ea      | .b .w .l  | integer add                  |
| adda     | ea, An                | .w .l     | add to address register      |
| addq     | #1-8, ea              | .b .w .l  | add quick                    |
| addx     | Dn, Dn                | .l        | add with extend              |
| sub      | ea, Dn / Dn, ea      | .b .w .l  | integer subtract             |
| suba     | ea, An                | .w .l     | subtract from address reg    |
| subq     | #1-8, ea              | .b .w .l  | subtract quick               |
| subx     | Dn, Dn                | .l        | subtract with extend         |
| muls     | ea, Dn                | .l        | signed 32x32->32 multiply   |
| mulu     | ea, Dn                | .l        | unsigned 32x32->32 multiply |
| divs     | ea, Dn                | .l        | signed 32/32 divide          |
| divu     | ea, Dn                | .l        | unsigned 32/32 divide        |
| neg      | ea                    | .b .w .l  | two's complement negate      |
| clr      | ea                    | .b .w .l  | clear to zero                |

### Logic (4)

| Mnemonic | Operands              | Sizes     | Notes                        |
|----------|-----------------------|-----------|------------------------------|
| and      | ea, Dn / Dn, ea      | .b .w .l  | bitwise AND                  |
| or       | ea, Dn / Dn, ea      | .b .w .l  | bitwise OR                   |
| eor      | Dn, ea                | .b .w .l  | bitwise XOR (src must be Dn) |
| not      | ea                    | .b .w .l  | bitwise complement           |

### Shift (3)

| Mnemonic | Operands              | Sizes     | Notes                        |
|----------|-----------------------|-----------|------------------------------|
| lsl      | #imm/Dn, Dn          | .l        | logical shift left           |
| lsr      | #imm/Dn, Dn          | .l        | logical shift right          |
| asr      | #imm/Dn, Dn          | .l        | arithmetic shift right       |

### Compare and Test (3)

| Mnemonic | Operands              | Sizes     | Notes                        |
|----------|-----------------------|-----------|------------------------------|
| cmp      | ea, Dn                | .b .w .l  | compare                      |
| cmpa     | ea, An                | .w .l     | compare to address register  |
| tst      | ea                    | .b .w .l  | test (sets flags, no result) |

### Sign Extension (2)

| Mnemonic | Operands              | Sizes     | Notes                        |
|----------|-----------------------|-----------|------------------------------|
| ext      | Dn                    | .w .l     | sign extend byte->word or word->long |
| extb     | Dn                    | .l        | sign extend byte->long (CF extension) |

### Branch (13)

| Mnemonic | Condition                          |
|----------|------------------------------------|
| bra      | always                             |
| beq      | equal (Z=1)                        |
| bne      | not equal (Z=0)                    |
| blt      | less than, signed (N!=V)           |
| bgt      | greater than, signed (Z=0, N=V)   |
| ble      | less or equal, signed              |
| bge      | greater or equal, signed (N=V)    |
| bmi      | minus (N=1)                        |
| bpl      | plus (N=0)                         |
| bcs      | carry set (C=1)                    |
| bcc      | carry clear (C=0)                  |
| bhi      | higher, unsigned (C=0, Z=0)       |
| bls      | lower or same, unsigned            |

All branches use 16-bit displacement (4 bytes total). No 8-bit short
or 32-bit long branch forms.

### Set Conditional (12, dynamic)

Scc instructions are matched dynamically via `cc_code()`:
seq, sne, slt, sgt, sle, sge, smi, spl, scs, scc, shi, sls.
Destination is a byte-sized effective address.

### Control (4)

| Mnemonic | Operands              | Notes                        |
|----------|-----------------------|------------------------------|
| jsr      | ea                    | jump to subroutine           |
| jmp      | ea                    | unconditional jump           |
| rts      | --                    | return from subroutine       |
| trap     | #0-15                 | software trap                |
| nop      | --                    | no operation                 |

### FPU (15)

| Mnemonic | Operands              | Sizes         | Notes                     |
|----------|-----------------------|---------------|---------------------------|
| fmove    | ea, FPn / FPn, ea     | .s .d .x      | FP data move              |
| fadd     | FPn, FPn              | .x            | FP add                    |
| fsub     | FPn, FPn              | .x            | FP subtract               |
| fmul     | FPn, FPn              | .x            | FP multiply               |
| fdiv     | FPn, FPn              | .x            | FP divide                 |
| fneg     | FPn                   | .x            | FP negate                 |
| fabs     | FPn                   | .x            | FP absolute value         |
| fintrz   | FPn, FPn              | .x            | FP truncate to integer    |
| fcmp     | FPn, FPn              | .x            | FP compare                |
| fbeq     | label                 | --            | branch if FP equal        |
| fbne     | label                 | --            | branch if FP not equal    |
| fblt     | label                 | --            | branch if FP less         |
| fbgt     | label                 | --            | branch if FP greater      |
| fble     | label                 | --            | branch if FP less/equal   |
| fbge     | label                 | --            | branch if FP greater/equal|

FPU arithmetic operates register-to-register only. Memory operands
are supported only in `fmove`.

## Addressing Modes

| Mode         | Syntax     | EA mode/reg | Example         |
|--------------|------------|-------------|-----------------|
| Data reg     | Dn         | 0/n         | d3              |
| Address reg  | An         | 1/n         | a0, sp, fp      |
| FP reg       | FPn        | (FPU only)  | fp2             |
| Indirect     | (An)       | 2/n         | (a0)            |
| Post-inc     | (An)+      | 3/n         | (sp)+           |
| Pre-dec      | -(An)      | 4/n         | -(sp)           |
| Displacement | d16(An)    | 5/n         | -8(fp), 4(a1)   |
| Absolute     | symbol     | 7/1         | _start          |
| Immediate    | #value     | 7/4         | #42, #sym       |
| Register list| Dn-Dn/An-An| (movem)    | d2-d7/a2-a6    |

Not supported: index with displacement `d8(An,Xn)`, program-counter
relative `d16(PC)` or `d8(PC,Xn)`, absolute long (mode 7/0).

## Assembler Directives

| Directive        | Description                              |
|------------------|------------------------------------------|
| .text            | switch to text section                   |
| .data            | switch to data section                   |
| .bss             | switch to BSS section                    |
| .globl NAME      | mark symbol as global                    |
| .long val,...     | emit 32-bit values (relocatable)        |
| .short val,...    | emit 16-bit values                      |
| .byte val,...     | emit 8-bit values                       |
| .ascii "str"     | emit string (no terminator)              |
| .asciz "str"     | emit null-terminated string              |
| .string "str"    | alias for .asciz                         |
| .align N         | align to N-byte boundary                 |
| .space N         | emit N zero bytes                        |

Labels: `name:` (global), `.Lname:` (local), `0:`-`9:` (numeric local).
Comments: `|` (GAS pipe syntax).

## Missing from ColdFire ISA

These are ColdFire instructions documented in the Programmer's Reference
Manual that `skj-as` does not support. Grouped by practical importance.

### Would be useful (the backends or runtime could plausibly use these)

| Mnemonic | Description                              | ISA     |
|----------|------------------------------------------|---------|
| asl      | arithmetic shift left                    | ISA_A   |
| swap     | swap register halves (hi/lo word)        | ISA_A   |
| btst     | test a bit                               | ISA_A   |
| bset     | test and set a bit                       | ISA_A   |
| bclr     | test and clear a bit                     | ISA_A   |
| bchg     | test and change a bit                    | ISA_A   |
| andi     | AND immediate to Dn                      | ISA_A   |
| ori      | OR immediate to Dn                       | ISA_A   |
| eori     | XOR immediate to Dn                      | ISA_A   |
| addi     | add immediate to Dn                      | ISA_A   |
| subi     | subtract immediate to Dn                 | ISA_A   |
| cmpi     | compare immediate to Dn                  | ISA_A   |
| negx     | negate with extend                       | ISA_A   |
| move CCR | move to/from condition code register     | ISA_A   |
| move SR  | move to/from status register (supervisor)| ISA_A   |
| remsl    | signed remainder (32/32->32:32)          | ISA_A+  |
| remul    | unsigned remainder                       | ISA_A+  |
| tpf      | trap false (variable-length NOP)         | ISA_A   |
| mvs      | move with sign extend (byte/word->long)  | ISA_B   |
| mvz      | move with zero extend (byte/word->long)  | ISA_B   |
| mov3q    | move 3-bit quick (-1, 1-7 to ea)         | ISA_C   |
| sats     | signed saturate                          | ISA_B   |
| ff1      | find first one (leading-zero count)      | ISA_C   |

### Supervisor / system (unlikely to need for user-mode code)

| Mnemonic | Description                              | ISA     |
|----------|------------------------------------------|---------|
| halt     | halt processor                           | ISA_A   |
| stop     | stop and load SR                         | ISA_A   |
| pulse    | pulse for debug                          | ISA_A   |
| wddata   | write debug data                         | ISA_A   |
| wdebug   | write debug register                     | ISA_A   |
| movec    | move to/from control register            | ISA_A   |
| cpushl   | cache push and invalidate                | ISA_A   |
| intouch  | instruction cache touch                  | ISA_B   |
| stldsr   | store then load status register          | ISA_C   |
| rte      | return from exception                    | ISA_A   |
| frestore | FPU restore state                        | ISA_A   |
| fsave    | FPU save state                           | ISA_A   |

### MAC unit (multiply-accumulate, specialized DSP)

| Mnemonic | Description                              | ISA     |
|----------|------------------------------------------|---------|
| mac      | multiply-accumulate                      | ISA_B   |
| msac     | multiply-subtract-accumulate             | ISA_B   |
| macl     | MAC long                                 | ISA_B   |
| msacl    | MSAC long                                | ISA_B   |
| move ACC | move accumulator                         | ISA_B   |
| move MACSR | move MAC status register               | ISA_B   |
| move MASK| move MAC mask register                   | ISA_B   |

### FPU instructions not yet supported

| Mnemonic | Description                              |
|----------|------------------------------------------|
| fsqrt    | square root                              |
| fsin     | sine (68881/68882 only, not HW on CF)    |
| fcos     | cosine (same caveat)                     |
| ftan     | tangent (same caveat)                    |
| fmod     | modulo                                   |
| frem     | IEEE remainder                           |
| fint     | round to integer (current rounding mode) |
| fmovem   | move multiple FP registers               |
| fmovecr  | move FP constant ROM                     |
| ftst     | FP test                                  |
| fbun     | branch if unordered                      |
| fbnge    | branch if not greater or equal           |
| fbngt    | branch if not greater than               |
| fbnle    | branch if not less or equal              |
| fbnlt    | branch if not less than                  |
| fscc     | set byte on FP condition                 |

## Summary

**Supported:** 65 integer instructions + 15 FPU instructions = 80 total,
covering the subset that the compiler backends actually emit.

**Missing but useful:** 22 instructions, mostly the immediate-form ALU
operations (andi/ori/eori/addi/subi/cmpi), bit manipulation (btst/bset/bclr/bchg),
asl, swap, and the ISA_B/C convenience instructions (mvs/mvz/mov3q/ff1).

**Missing, low priority:** 12 supervisor/system, 7 MAC unit, 16 extended FPU.

The assembler covers what the compiler needs today. The immediate-form
ALU instructions and bit operations would be the most useful additions
for hand-written assembly or future compiler optimizations.
