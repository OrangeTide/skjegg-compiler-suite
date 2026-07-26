# Platform calling convention for cc

Goal: `skj-cc-x86-64` and `skj-cc-arm64` should call C library functions, and
be called by the C runtime, using the platform psABI (**System V AMD64** on
x86-64, **AAPCS64** on arm64) instead of the toolkit's uniform stack
convention. This is what lets cc link against real glibc/musl and crt, the last
big ABI item after LP64 (`doc/lp64.md`).

## Decisions (scoped 2026-07)

1. **cc-specific, compile-time gated.** A flag (`CC_PSABI`) is defined only for
   the x86-64 and arm64 cc tools, exactly like `CC_LP64`. The backend picks the
   concrete ABI from its target (SysV on `X86_BITS==64`, AAPCS64 on arm64). The
   other front-ends (tinc, scheme, moo, pascal) on those backends keep the
   toolkit stack convention untouched; the calling convention is self-consistent
   within a program, so nothing else has to change.
2. **Phased** (each phase keeps the whole suite green):
   - **P1** ✓ scalar / pointer / float **fixed-argument** calls and returns
     (caller and callee) on x86-64 SysV, with a psABI-compatible freestanding
     runtime (`start_x86_64_sysv.asm`). `CC_PSABI` is defined for the x86-64 cc
     tool; a new `param_class` on `ir_func` (set by cc) tells the callee which
     params are float. The caller classifies args, stages float register args
     through a scratch stack block (xmm1-7 overlap the arg registers), loads int
     args straight into `rdi..r9`, pushes overflow args 16-aligned, and sets
     `al` to the SSE count; the prologue spills register params into home slots
     below `rbp` and keeps `rsp` 16-aligned. Green: `check-cc-x86-64` (69),
     `cc_t069`. Everything is gated so the other x86-64 front-ends keep the
     stack convention. A review pass then hardened two edges: an indirect
     target is loaded into `r11` before `al` is set (a spilled target reload
     must not clobber the SSE count), and the prologue alignment pad is
     `(frame + NSAVED*8) % 16` (self-adjusting rather than assuming `NSAVED`).
     A separate review also found and fixed a pre-existing float-temp-across-
     call miscompile in the x86 and arm64 register allocators (`cc_t070`).
   - **P2** ✓ varargs (callee side). A variadic function's prologue saves the
     six integer arg registers and eight xmm registers to a SysV register save
     area below the param homes; `IR_VA_START` (a new op the backend lowers,
     since it owns the frame) fills the `va_list` (`gp_offset`, `fp_offset`,
     `overflow_arg_area`, `reg_save_area`) from the named-param counts; and
     `va_arg` calls a runtime `__va_arg` helper (`runtime/va_x86_64.c`) that
     walks the save area then the overflow stack, cc loading the typed value.
     `__builtin_va_list` is a built-in type; the builtins are
     `__builtin_va_start` / `__builtin_va_end` and the typed accessors
     `__builtin_va_arg_int` / `_long` / `_dbl` (a standard `__builtin_va_arg(ap,
     type)` with a type argument is a later refinement for header stdarg.h).
     The caller side (placing args + `al`) was already done in P1. Green:
     `cc_t071` (int with register overflow, float in the SSE regs, 64-bit long),
     x86-64 only (excluded on the other cc targets, which have no register save
     area yet).
   - **P3** struct-by-value: the SysV eightbyte classification for register
     structs (1-2 eightbytes, INTEGER and SSE). Two foundations plus the
     classification. First, a pre-existing gap: aggregate **value-semantic
     copy** (`b = a`, `struct c = b`, a local init from another struct)
     previously truncated an 8-byte struct to a 4-byte store, or stored the
     source *address*. `emit_aggregate_copy` now emits a byte-accurate 4/2/1-
     byte copy loop, which is data-model-independent and fixes every cc target.
     Second, the register classification (`sysv_eightbytes`, gated to
     `CC_STRUCT_ABI`, only the x86-64 cc tool): a `struct`/`union` of 1..16
     bytes is 1 or 2 eightbytes, each
     classed INTEGER (a field walk; a gp register) or SSE (all-float; an
     `xmm`), with INTEGER dominating a mixed eightbyte. It reuses the i64/float
     arg machinery: an argument decomposes into one loaded value per eightbyte
     (`LD64`/`FLD`, placed into the next gp / xmm register by the P1 caller); a
     struct **parameter** homes each incoming register into contiguous home
     slots the fields read from (the backend's `sysv_ploc` tracks per-eightbyte
     class, register, and the all-or-nothing fit); a **return** packs the
     eightbytes into `rax:rdx` / `xmm0:xmm1` (`IR_RETV_AGG`, using
     `fn->ret_cls`); and a struct-returning **call** (`IR_CALL_AGG`) stores the
     returned registers into a hidden slot, so the call expression yields an
     address like every other struct value. The **MEMORY** case (a struct
     larger than 16 bytes) is also done: an argument is copied onto the
     outgoing stack (`IR_ARG_MEM`, the backend emits a per-size byte copy) and
     read in place from the incoming stack by the callee (a stack parameter
     spanning `ceil(size/8)` slots); a return uses a hidden pointer, an
     ordinary leading integer parameter cc adds, through which the callee
     copies the result and which it returns in `rax`. Verified against gcc in
     both directions (a gcc caller into a skj callee and back) for every class,
     INTEGER, SSE, mixed, and MEMORY, plus `cc_t072`; x86-64 only. The one
     remaining edge is the rare register-struct that straddles the last
     argument register (the all-or-nothing split is decided per eightbyte at
     the caller, so it could split rather than fall wholly to memory).
   - **P4** arm64 AAPCS64 (the same phases). **P4-P1** ✓ fixed scalar / pointer
     / float args and returns. The register calling convention lives behind
     `CC_PSABI` in the backend (both x86-64 and arm64 define it); the SysV
     struct decomposition in `cc` is now behind a separate `CC_STRUCT_ABI` (only
     x86-64 defines it), so enabling `CC_PSABI` for the arm64 cc tool gives the
     AAPCS64 scalar convention without the eightbyte struct handling the arm64
     backend does not carry yet. Integer/pointer args go in `x0..x7`, float in
     `d0..d7`, the rest on a 16-aligned stack; the return is in `x0` / `d0`; a
     register param homes into a slot below `x29` (mirroring the x86-64 home
     mechanism, with a `frame_reserve` region), and an overflow param reads from
     `x29+16+8*k`. The register arg targets (`x0-x7`/`d0-d7`) are disjoint from
     the allocatable registers (`x19-x28`/`d8-d15`), so args move straight into
     place with no staging. A new AAPCS64 freestanding runtime
     (`start_arm64_aapcs.S`) has register-convention `write`/`read`/`exit`.
     Verified against gcc (an aarch64 gcc caller into skj callees): 8 integer
     args, 64-bit longs, self-recursion, stack overflow (10 args), `d0..d7`
     floats, and interleaved int/float; `check-cc-arm64` is green.
     **P4-P2** ✓ varargs (callee side). The va lowering in `cc` is target-
     agnostic (it calls a runtime `__va_arg(ap, is_fp)` that returns the next
     slot pointer), so only three pieces are arm64-specific: the `va_list`
     layout (`{ void *__stack, *__gr_top, *__vr_top; int __gr_offs, __vr_offs; }`,
     32 bytes, behind `CC_ARM64` in `cc/parse.c`); the `IR_VA_START` lowering
     and the variadic prologue register save in the backend (all 8 `x`- and 8
     `d`-registers to a 192-byte save area below the param homes, the `d`-slots
     at a 16-byte stride); and the runtime helper (`va_aarch64.c`, which walks
     `__gr_offs`/`__vr_offs` up toward zero then the overflow stack). Verified
     against gcc (a gcc caller into a skj variadic callee): 10 int varargs with
     register-and-stack overflow, five `double` varargs, and 64-bit longs;
     `cc_t071` now runs on arm64.
     **P4-P3** (register cases) ✓ struct-by-value for the register classes. The
     cc struct machinery is generalized from 2 SysV eightbytes to 1-4 slots
     behind a target-neutral `abi_agg` (AAPCS64 `aapcs_agg` on arm64, SysV
     `sysv_eightbytes` elsewhere): an aggregate is a Homogeneous Floating-point
     Aggregate (1-4 members of one float type; the double case rides `d0..d3`),
     any other aggregate of 16 bytes or less (1-2 `x`-registers), or larger. A
     register aggregate decomposes into one arg per slot (`FLD`/`LD64`, placed by
     the P1 caller into `d`/`x` registers); a parameter homes each incoming
     register into contiguous slots (the backend's `aapcs_ploc` tracks per-slot
     class, register, and the all-or-nothing fit); a return packs into `x0:x1` /
     `d0..d3` (`IR_RETV_AGG`); and a struct-returning call (`IR_CALL_AGG`) stores
     the returned registers into a hidden slot. The `CALL_AGG` `imm` encoding
     widened to a 3-bit slot count plus a per-slot FP bit (x86-64 decode updated
     in lockstep). Verified against gcc both directions for `struct {int,int}`
     (one x-reg), `struct {long,long}` (`x0:x1`), a 2- and a 4-member double HFA
     (`d0:d1`, `d0..d3`); `cc_t074` runs on x86-64 and arm64.
     The **memory** cases followed: a struct larger than 16 bytes is passed as a
     pointer to a caller-made copy (an ordinary `x`-register argument; the callee
     copies it into a local at entry so field access stays direct, the copy
     slots allocated after the param slots so the indices are unshifted) and
     returned through the `x8` indirect-result register (a new `IR_ARG_X8` the
     caller routes to `x8`; the callee homes `x8` into the hidden result
     parameter, which does not consume a normal argument register, and writes
     the struct back through it). Verified against gcc both directions for a
     32-byte struct; `cc_t072` now runs on arm64, and arm64 is at full cc-suite
     parity with x86-64 (74).
     The callee-saved **`d8-d15`** are now preserved: the prologue saves the
     subset a function actually uses (scanned from the float defs allocated to
     `d8..d15`) at the bottom of the frame and the epilogue restores them,
     rounded to keep the frame 16-aligned. With them preserved, the earlier
     float-cross-call force-spill (a workaround for *not* saving them) is dropped
     for the psABI build, so a float temp may now keep a callee-saved register
     across a call. Verified against gcc: a caller holding live values in
     `d8-d15` across a call to a skj function that clobbers them gets them back
     intact. This closes the last float-interop gap.
     **Float HFAs** (a struct of 1-4 single-precision members) ride `s0..s3`:
     the classifier gives such a slot the float class, the argument loads a
     4-byte single and the caller places it in an `s`-register, and the callee
     homes it at a 4-byte stride (the home-slot count is decoupled from the
     register count, since two singles share one 8-byte home). The return
     (`s0..s3`) and the struct-returning call both follow the same 4-byte
     packing; the `CALL_AGG` class encoding widened to 2 bits per slot (integer
     / double / float single) with the x86-64 decode updated in lockstep.
     Verified against gcc for a 2- and a 4-member float HFA; `cc_t074` covers
     both (x86-64 reaches the same shapes through its SSE eightbytes). **AAPCS64
     struct-by-value is now complete.**
3. **x86-64 first** (SysV AMD64 is the best-documented), arm64 second.

## Current state (the stack convention)

Every front-end and backend uses one convention: args pushed right-to-left, the
caller pops, the return value in `rax`/`a0`/`x0`/`d0`. The callee reads each
param from a stack slot the caller pushed (`BP + 2*WORD + WORD*i`). The
freestanding runtime wrappers (`write`/`read`/`exit` in `start_x86_64.asm`)
read their args off that stack and translate to the syscall register
convention; `main` takes no args and a custom `_start` calls it.

## System V AMD64 (the P1 target)

- **Integer/pointer args** go in `rdi, rsi, rdx, rcx, r8, r9`, then the stack.
  A 64-bit integer is one register (native on x86-64).
- **Float/double args** go in `xmm0..xmm7`, then the stack.
- Stack args are pushed right-to-left; **`rsp` is 16-byte aligned at the
  `call`** (so the callee entry sees `rsp % 16 == 8`).
- **Return** in `rax` (integer/pointer) or `xmm0` (float) - already what the
  backend does, so returns barely change.
- Classification is by argument, in order: assign each to the next free
  register of its class, overflow to the stack.

### Caller (`emit_call_flush` under `CC_PSABI`)

Walk the pending args in order, tracking the next integer register and the next
SSE register. Place each arg in its register (or, once a class is exhausted, on
the stack, still right-to-left relative to the other stack args). Round the
stack-arg block so `rsp` is 16-aligned at the `call`. For P1 (fixed args) `al`
may stay 0; P2 sets it to the SSE count for a variadic callee.

### Callee (prologue under `CC_PSABI`)

Params now arrive in registers, not on the caller's stack. The prologue spills
each register param into its existing slot at entry (int params from
`rdi..r9`, float from `xmm0..7`), so the rest of the lowering - which reads
params from slots - is unchanged. A 7th+ integer or 9th+ float param arrives on
the incoming stack and is read from there. `main(argc, argv)` then just works:
`argc` in `rdi`, `argv` in `rsi`.

### Runtime

cc's output calls `write(fd, buf, n)` with SysV args (`rdi/rsi/rdx`), which
maps almost directly onto the Linux syscall (same registers, number in `rax`),
so a psABI freestanding `start` is simpler than the stack version. cc tests
link that psABI `start`; real programs link the system crt + libc, where `main`
is already SysV. The other front-ends keep linking the stack `start`. Concretely
this is a second start object selected for the cc-psabi tools (the tinc/scheme
runtime is unchanged).

## Interactions and risks

- **LP64.** P-args ride on the LP64 work: a pointer is a 64-bit register value,
  already how the backend holds it. No conflict.
- **The shared x86 backend.** The SysV paths live under `#if X86_BITS == 64`
  and are further gated so a stack-convention build is byte-identical; the
  32-bit backend is untouched.
- **Spills across the call.** The caller must materialize every arg into its
  target register just before the `call`, after any address/spill reloads, and
  must not clobber an already-placed argument register while computing a later
  one (classic parallel-move hazard). The simplest correct approach is to
  compute all args into temporaries first, then move them into the ABI
  registers in one final pass.
- **Indirect calls / function pointers.** The target address is in a register;
  under SysV a variadic indirect call still needs `al`. P1 handles the direct
  and indirect fixed-arg cases.
- **Callbacks.** A skj function whose address is taken and handed to libc (e.g.
  `qsort`) is entered by libc with SysV, so every cc-compiled function's
  prologue must be SysV (not just `main`). P1 makes all cc functions SysV, so
  callbacks work.

## Open questions (later phases)

- P2: the `va_list` layout (SysV `gp_offset`/`fp_offset`/overflow/reg-save) and
  where the 176-byte register save area lives in the frame.
- P3 (done): the recursive eightbyte classification, the `MEMORY` fallback, and
  returning a small struct in `rax:rdx` / `xmm0:xmm1` versus a large one through
  a hidden pointer, all verified against gcc.
- P4: AAPCS64. P4-P1 (fixed scalar/pointer/float args), P4-P2 (varargs), and
  P4-P3 (struct-by-value: register HFA-double / small-integer classes and the
  memory cases via a pointer-to-copy and the `x8` indirect return) are done and
  gcc-verified; arm64 is at full cc-suite parity with x86-64, the callee-saved
  `d8-d15` are preserved, and float HFAs (`s`-register aggregates) are handled.
  **The x86-64 SysV and arm64 AAPCS64 calling conventions are both complete.**
