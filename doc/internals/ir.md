# Skjegg IR Reference

Flat three-address code (3AC) intermediate representation shared by all
front ends and backends.

## Design

SSA within, memory between. Each temp is written once inside a basic
block. Cross-block values round-trip through local slots via
`IR_LDL`/`IR_STL`. There are no phi nodes, no dominance trees, and no
optimization passes. The IR is lowered directly to machine instructions
by the backend.

## Base Types

```
IR_I8    8-bit integer
IR_I16   16-bit integer
IR_I32   32-bit integer (default)
IR_I64   64-bit integer
IR_F64   64-bit IEEE 754 double
```

Used in `ir_global.base_type` to specify the element type of global
variables and arrays. IR instructions do not carry explicit type tags;
width is implicit in the opcode (e.g. `IR_LB` vs `IR_LW`, `IR_ADD` vs
`IR_ADD64`).

## Data Structures

### `ir_insn`

A single instruction node in a linked list.

| Field   | Type   | Meaning                                       |
|---------|--------|-----------------------------------------------|
| `op`    | int    | Opcode (enum `ir_op`)                         |
| `dst`   | int    | Destination temp (-1 if none)                 |
| `a`     | int    | First source temp (-1 if unused)              |
| `b`     | int    | Second source temp (-1 if unused)             |
| `imm`   | long   | Immediate constant                            |
| `sym`   | char * | Symbol name (for `IR_LEA`, `IR_CALL`, etc.)   |
| `slot`  | int    | Local slot index (-1 if unused)               |
| `label` | int    | Label index (-1 if unused)                    |
| `nargs` | int    | Argument count (for calls and `IR_FUNC`)      |
| `next`  | ptr    | Next instruction in the list                  |

### `ir_func`

A function, containing a linked list of instructions and metadata filled
in by register allocation.

| Field       | Type  | Meaning                                       |
|-------------|-------|-----------------------------------------------|
| `name`      | char* | Symbol name                                   |
| `is_local`  | int   | 1 = file-local (static), 0 = global           |
| `nparams`   | int   | Number of parameter slots                     |
| `nslots`    | int   | Total slots (params + locals)                 |
| `slot_size` | int*  | Byte size of each slot                        |
| `ntemps`    | int   | Number of temps allocated                     |
| `nlabels`   | int   | Number of labels allocated                    |
| `nspills`   | int   | Integer spill count (set by regalloc)         |
| `nfspills`  | int   | Float spill count (set by regalloc)           |
| `ni64spills`| int   | I64 spill count (set by regalloc)             |
| `temp_reg`  | int*  | Temp-to-register map (set by regalloc)        |
| `temp_spill`| int*  | Temp-to-spill-offset map (set by regalloc)    |
| `head/tail` | ptr   | Instruction list                              |

Slots 0 through `nparams-1` are parameters (positive offsets from the
frame pointer). Slots `nparams` through `nslots-1` are locals (negative
offsets).

### `ir_global`

A global variable or array.

| Field         | Type    | Meaning                                   |
|---------------|---------|-------------------------------------------|
| `name`        | char*   | Symbol name                               |
| `base_type`   | int     | Element type (enum `ir_basetype`)         |
| `arr_size`    | int     | Array length (0 = scalar)                 |
| `is_ptr`      | int     | 1 = pointer-sized element                 |
| `is_local`    | int     | 1 = file-local (static)                   |
| `init_ivals`  | int64*  | Integer initializer values                |
| `init_syms`   | char**  | Symbol initializers (for address slots)   |
| `init_count`  | int     | Number of initialized elements            |
| `init_string` | char*   | String literal initializer                |
| `init_strlen` | int     | Length of string literal                   |

When `init_string` is set, the global is a byte blob emitted as `.ascii`.
Otherwise, `init_count` elements are emitted, with the rest zero-filled
up to `arr_size`.

### `ir_program`

Top-level container: a linked list of `ir_func` and a linked list of
`ir_global`.

## Builder API

```c
struct ir_func *ir_new_func(struct arena *a, const char *name);
int             ir_new_temp(struct ir_func *fn);
int             ir_new_label(struct ir_func *fn);
struct ir_insn *ir_emit(struct ir_func *fn, int op);
int             ir_op_is_float_def(int op);
int             ir_op_is_i64_def(int op);
```

`ir_emit()` appends an instruction to the function's list. All fields
default to -1 (dst, a, b, slot, label). The caller fills in whichever
fields the opcode requires. Example:

```c
ins = ir_emit(fn, IR_ADD);
ins->dst = ir_new_temp(fn);
ins->a = t1;
ins->b = t2;
```

`ir_op_is_float_def()` and `ir_op_is_i64_def()` return true if the
opcode produces a float or i64 result. Register allocation uses these to
route temps into the correct register class.

## Opcode Reference

Notation: `d` = `dst`, `a` = `a` operand, `b` = `b` operand,
`imm` = immediate, `sym` = symbol name, `slot` = slot index,
`L` = label index. `[addr]` means "memory at address in temp".

### Miscellaneous

| Opcode   | Operands        | Description                           |
|----------|-----------------|---------------------------------------|
| `NOP`    | (none)          | No operation                          |
| `FUNC`   | nargs           | Function entry marker                 |
| `ENDF`   | (none)          | Function end marker                   |
| `LABEL`  | L               | Branch target                         |

### Constants and Addresses

| Opcode   | Operands        | Description                           |
|----------|-----------------|---------------------------------------|
| `LIC`    | d, imm          | Load 32-bit immediate constant        |
| `LEA`    | d, sym          | Load address of global symbol         |
| `ADL`    | d, slot         | Load address of local slot            |
| `MOV`    | d, a            | Copy temp (d = a)                     |

### Integer Arithmetic

| Opcode   | Operands        | Description                           |
|----------|-----------------|---------------------------------------|
| `ADD`    | d, a, b         | d = a + b                             |
| `SUB`    | d, a, b         | d = a - b                             |
| `MUL`    | d, a, b         | d = a * b (signed)                    |
| `DIVS`   | d, a, b         | d = a / b (signed)                    |
| `DIVU`   | d, a, b         | d = a / b (unsigned)                  |
| `MODS`   | d, a, b         | d = a % b (signed)                    |
| `MODU`   | d, a, b         | d = a % b (unsigned)                  |
| `AND`    | d, a, b         | d = a & b                             |
| `OR`     | d, a, b         | d = a \| b                            |
| `XOR`    | d, a, b         | d = a ^ b                             |
| `SHL`    | d, a, b         | d = a << b                            |
| `SHRS`   | d, a, b         | d = a >> b (arithmetic)               |
| `SHRU`   | d, a, b         | d = a >> b (logical)                  |
| `NEG`    | d, a            | d = -a                                |
| `NOT`    | d, a            | d = ~a (bitwise)                      |

### Memory Access

| Opcode   | Operands        | Description                           |
|----------|-----------------|---------------------------------------|
| `LB`     | d, a            | d = zero-extend byte at [a]           |
| `LBS`    | d, a            | d = sign-extend byte at [a]           |
| `LH`     | d, a            | d = zero-extend halfword at [a]       |
| `LHS`    | d, a            | d = sign-extend halfword at [a]       |
| `LW`     | d, a            | d = word at [a]                       |
| `SB`     | a, b            | store byte b at [a]                   |
| `SH`     | a, b            | store halfword b at [a]               |
| `SW`     | a, b            | store word b at [a]                   |

### Local Slot Access

| Opcode   | Operands        | Description                           |
|----------|-----------------|---------------------------------------|
| `LDL`    | d, slot         | d = load word from local slot         |
| `STL`    | a, slot         | store word a into local slot          |
| `ALLOCA` | d, a            | d = stack-allocate a bytes (aligned)  |

### Integer Comparison

All comparisons produce a full-width boolean: -1 (all bits set) for
true, 0 for false.

| Opcode   | Operands        | Description                           |
|----------|-----------------|---------------------------------------|
| `CMPEQ`  | d, a, b         | d = (a == b)                          |
| `CMPNE`  | d, a, b         | d = (a != b)                          |
| `CMPLTS` | d, a, b         | d = (a < b) signed                    |
| `CMPLES` | d, a, b         | d = (a <= b) signed                   |
| `CMPGTS` | d, a, b         | d = (a > b) signed                    |
| `CMPGES` | d, a, b         | d = (a >= b) signed                   |
| `CMPLTU` | d, a, b         | d = (a < b) unsigned                  |
| `CMPLEU` | d, a, b         | d = (a <= b) unsigned                 |
| `CMPGTU` | d, a, b         | d = (a > b) unsigned                  |
| `CMPGEU` | d, a, b         | d = (a >= b) unsigned                 |

### Control Flow

| Opcode   | Operands        | Description                           |
|----------|-----------------|---------------------------------------|
| `JMP`    | L               | Unconditional jump to label L         |
| `BZ`     | a, L            | Branch to L if a == 0                 |
| `BNZ`    | a, L            | Branch to L if a != 0                 |

### Calls and Returns

Arguments are staged with `IR_ARG` instructions, then flushed by the
call instruction. Arguments are pushed right-to-left onto the stack;
the caller pops them after the call returns. Maximum 16 arguments.

| Opcode      | Operands        | Description                        |
|-------------|------------------|------------------------------------|
| `ARG`       | a               | Stage integer argument              |
| `FARG`      | a               | Stage float argument                |
| `CALL`      | d, sym, nargs   | Call symbol, result in d            |
| `CALLI`     | d, a, nargs     | Indirect call through a, result in d|
| `FCALL`     | d, sym, nargs   | Call symbol, float result in d      |
| `FCALLI`    | d, a, nargs     | Indirect call, float result in d    |
| `TAILCALL`  | sym, nargs      | Tail call to symbol                 |
| `TAILCALLI` | a, nargs        | Indirect tail call through a        |
| `RET`       | (none)          | Return void                         |
| `RETV`      | a               | Return integer value in a           |
| `FRETV`     | a               | Return float value in a             |

The `d` field of call instructions is -1 when the return value is
discarded.

### Continuations

Used by TinScheme for delimited continuations (shift/reset). The runtime
provides `__cont_capture` and `__cont_resume`.

| Opcode    | Operands         | Description                         |
|-----------|------------------|-------------------------------------|
| `MARK`    | d, slot, L       | Save frame to slot (12 bytes: fp, sp, resume PC); d = 0 on first entry, nonzero on resume via shift |
| `CAPTURE` | d                | Capture current continuation; d = buffer pointer |
| `RESUME`  | a, b             | Resume continuation a with value b (does not return) |

### Floating-Point Arithmetic

Float temps occupy a separate register class (fp2-fp7 on ColdFire,
xmm1-xmm7 on x86-32; not yet implemented on RISC-V).

| Opcode    | Operands        | Description                          |
|-----------|-----------------|--------------------------------------|
| `FADD`    | d, a, b         | d = a + b (f64)                      |
| `FSUB`    | d, a, b         | d = a - b (f64)                      |
| `FMUL`    | d, a, b         | d = a * b (f64)                      |
| `FDIV`    | d, a, b         | d = a / b (f64)                      |
| `FNEG`    | d, a            | d = -a (f64)                         |
| `FABS`    | d, a            | d = |a| (f64)                        |

### Floating-Point Comparison

Produce 1 for true, 0 for false (unlike integer compares which produce
-1/0).

| Opcode    | Operands        | Description                          |
|-----------|-----------------|--------------------------------------|
| `FCMPEQ`  | d, a, b         | d = (a == b) (f64)                   |
| `FCMPLT`  | d, a, b         | d = (a < b) (f64)                    |
| `FCMPLE`  | d, a, b         | d = (a <= b) (f64)                   |

`FCMPNE`, `FCMPGT`, `FCMPGE` are synthesized by the front end using
`FCMPEQ`+`NOT` or by swapping operands.

### Floating-Point Conversion

| Opcode    | Operands        | Description                          |
|-----------|-----------------|--------------------------------------|
| `ITOF`    | d, a            | d = (double)a (i32 to f64)           |
| `FTOI`    | d, a            | d = (int)a (f64 to i32, truncate)    |

### Floating-Point Memory

| Opcode    | Operands        | Description                          |
|-----------|-----------------|--------------------------------------|
| `FLS`     | d, a            | d = load float32 from [a], widen to f64 |
| `FLD`     | d, a            | d = load float64 from [a]            |
| `FSS`     | a, b            | store b as float32 to [a]            |
| `FSD`     | a, b            | store b as float64 to [a]            |
| `FLDL`    | d, slot         | d = load float64 from local slot     |
| `FSTL`    | a, slot         | store float64 a into local slot      |

### 64-bit Integer Operations

I64 temps occupy a third register class. On 32-bit targets, the backend
allocates register pairs (ColdFire: d6/d7, d4/d5, d2/d3; x86-32:
esi/edi). Complex operations (multiply, shifts) call library helpers
(`__muldi3`, `__ashldi3`, `__ashrdi3`, `__lshrdi3`).

| Opcode      | Operands        | Description                        |
|-------------|------------------|------------------------------------|
| `LIC64`     | d, imm          | Load 64-bit immediate constant     |
| `ADD64`     | d, a, b         | d = a + b (i64)                    |
| `SUB64`     | d, a, b         | d = a - b (i64)                    |
| `MUL64`     | d, a, b         | d = a * b (i64, via __muldi3)      |
| `AND64`     | d, a, b         | d = a & b (i64)                    |
| `OR64`      | d, a, b         | d = a \| b (i64)                   |
| `XOR64`     | d, a, b         | d = a ^ b (i64)                    |
| `SHL64`     | d, a, b         | d = a << b (i64, b is i32)         |
| `SHRS64`    | d, a, b         | d = a >> b arithmetic (i64, b is i32) |
| `SHRU64`    | d, a, b         | d = a >> b logical (i64, b is i32) |
| `NEG64`     | d, a            | d = -a (i64)                       |

### 64-bit Comparison

Produce 1 for true, 0 for false.

| Opcode       | Operands       | Description                        |
|--------------|----------------|------------------------------------|
| `CMP64EQ`    | d, a, b        | d = (a == b)                       |
| `CMP64NE`    | d, a, b        | d = (a != b)                       |
| `CMP64LTS`   | d, a, b        | d = (a < b) signed                 |
| `CMP64LES`   | d, a, b        | d = (a <= b) signed                |
| `CMP64GTS`   | d, a, b        | d = (a > b) signed                 |
| `CMP64GES`   | d, a, b        | d = (a >= b) signed                |
| `CMP64LTU`   | d, a, b        | d = (a < b) unsigned               |
| `CMP64LEU`   | d, a, b        | d = (a <= b) unsigned              |
| `CMP64GTU`   | d, a, b        | d = (a > b) unsigned               |
| `CMP64GEU`   | d, a, b        | d = (a >= b) unsigned              |

### 64-bit Memory and Slots

| Opcode    | Operands        | Description                          |
|-----------|-----------------|--------------------------------------|
| `LD64`    | d, a            | d = load i64 from [a]               |
| `ST64`    | a, b            | store i64 b at [a]                   |
| `LDL64`   | d, slot         | d = load i64 from local slot         |
| `STL64`   | a, slot         | store i64 a into local slot          |

### 64-bit Width Conversion

| Opcode    | Operands        | Description                          |
|-----------|-----------------|--------------------------------------|
| `SEXT64`  | d, a            | d = sign-extend i32 a to i64         |
| `ZEXT64`  | d, a            | d = zero-extend i32 a to i64         |
| `TRUNC64` | d, a            | d = truncate i64 a to i32            |

### 64-bit Calls and Returns

| Opcode     | Operands         | Description                         |
|------------|------------------|-------------------------------------|
| `ARG64`    | a                | Stage i64 argument (8 bytes)        |
| `RETV64`   | a                | Return i64 value (ColdFire: d0:d1)  |
| `CALL64`   | d, sym, nargs    | Call symbol, i64 result in d        |
| `CALLI64`  | d, a, nargs      | Indirect call, i64 result in d      |

## Backend Interface

Each backend provides two functions:

```c
void regalloc(struct ir_func *fn);
void target_emit(FILE *out, struct ir_program *prog);
```

`regalloc()` runs Poletto-Sarkar linear scan over the function's temps,
filling `temp_reg[]` (physical register or -1 if spilled) and
`temp_spill[]` (spill slot offset). It handles three register classes
independently: integer, float, and i64.

`target_emit()` walks the program's functions and globals, emitting
assembly. Instruction selection is direct: one IR opcode becomes a short
burst of machine instructions.

### Backend Register Budgets

| Backend  | Integer           | Float             | I64 (pairs)       |
|----------|-------------------|-------------------|-------------------|
| ColdFire | d2-d7 (6)         | fp2-fp7 (6)       | 3 pairs           |
| RISC-V   | s1-s11 (11)       | (not implemented) | (not implemented) |
| x86-32   | ebx, esi, edi (3) | xmm1-xmm7 (7)    | 1 pair (esi/edi)  |

### Calling Convention

All backends use the same stack-based calling convention:

- Arguments pushed right-to-left, 32 bits each (64-bit values take two
  slots). Caller pops after return.
- Return: ColdFire d0, RISC-V a0, x86-32 eax. Float return: ColdFire
  fp0, x86-32 xmm0. I64 return: ColdFire d0:d1, x86-32 eax:edx.
