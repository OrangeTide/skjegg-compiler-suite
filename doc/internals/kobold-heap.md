# Kobold guest heap and its write-after-free instrument

The Kobold runtime allocates guest-owned managed values (strings, dynamic
arrays, maps, coroutines) from a guest heap. One allocator serves two platform
layers:

- `kobold/rt_free.c`, the freestanding guest tier, over a flat region the host
  hands in through `kp_rt_init`.
- `kobold/rt_host.c`, the hosted JIT/AOT driver, over a 256 MiB region it maps
  below 4 GB so a block address fits the 32-bit guest address model.

The allocator itself is `kobold/gheap_core.h`, which both files include. Each
file defines only its memory source (a `GH_SBRK` that bumps the region), the
guest-address wrappers (`gheap_alloc`/`gheap_free`/`gheap_realloc`, which convert
a native payload pointer to and from a 32-bit guest handle), and its own
out-of-memory trap.

## The allocator

A binned segregated-fit allocator. A request that matches an exact-size bin is an
O(1) pop; a larger request takes the single `large` list by first fit with a
split; anything else is a fresh block cut from the region. A freed block files
back into its bin (or the large list).

There is deliberately **no coalescing and no boundary-tag footer**. A block's
only metadata is its header, and the guest payload is never used by the
allocator, even while the block is free. That matters because the guest object
runtime currently tolerates a stray read or write of a just-freed payload; a
footer or a payload-resident free-list link (the layout `libc/malloc.c` uses)
would turn such a stray access into heap corruption. The trade-off is that a long
run with a varied size mix can fragment, which the fixed region bounds anyway.
Coalescing can be added once the stray accesses are fixed, or by moving the
boundary tags entirely into block headers.

## The write-after-free instrument (`GH_POISON`)

`GH_POISON` is a debug build mode, off by default, that finds those stray
accesses. It has two halves.

**Poison and verify (the reliable detector), in `gheap_core.h`.** On free a
block's payload is filled with the poison byte `0xDF` and its last-seen object
kind is recorded in a side table. When the block is next handed out, the payload
is checked to still be all poison. A disturbed byte means the guest wrote the
block while it was free, reported as:

```
GH_WAF bin block=0x41c00180 size=32 off=0 byte=0xde freed-kind=1 freed-seq=2
   bytes: de df df df df ...
```

`off` is the first disturbed byte and `freed-kind` is the `kp_obj_kind` of the
object that was freed there (1 = string). A single `0xde` at `off=0` over an
otherwise-intact `0xdf` fill is a decrement of the freed object's refcount low
byte, i.e. a release after free.

**Retain/release check (the backtrace), in `rt_host.c`.** `kp_retain` and
`kp_release` print `GH_UAF` with a backtrace when handed a value that is inside
the gheap region *and* on a free list. The region bound is essential: a static
string lives in the module image (refcount `0xFFFFFFFF`, `KP_RC_STATIC`), and
without the bound `gh_is_freed` would read its `.rodata` bytes as a block header
and false-positive. A real heap use-after-free shows refcount `0xDFDFDFDF` (the
poison).

### Running it

Build the hosted runtime with `-DGH_POISON` (or set it once in `rt_host.c`), then
run any hosted tier and grep for the markers:

```
skj-kp-jit prog.pas 2>&1 | grep GH_
```

Under the JIT the embedded compiler `kpcom` runs first and compiles the program,
so its own allocations are in the output too (even an empty program shows
hundreds). To look at **only** a user program's codegen, use the AOT path, which
runs the compiled program in its own process:

```
skj-kpc --emit-kpm prog.pas > prog.kpm        # kpcom's noise stays on this step
kobold/aot-run.sh prog.kpm 2>&1 | grep GH_    # GH_WAF here is user codegen only
```

## A finding it caught (resolved 2026-10-17)

The instrument first surfaced a systematic release-after-free of heap strings:
compiling even an empty program through a hosted tier reported hundreds of
`GH_WAF` markers, all a single `0xde` at `off=0` (a refcount decrement of an
already-freed string). The count did not scale with the compiled program's
runtime string work, and the native AOT `skj-kpc` over-released identically to
the JITed `skj-kp-jit`, which ruled out both the allocator and any one backend
(the JIT and AOT paths share the `backend/x86_select.c` selector, so a user
program's machine code is the same either way). The fault was in the compiler
itself, `kobold/kpcom.pas`.

`IsOwnedExpr` decides whether a handle-valued expression owns a fresh reference
(a temporary to release) or borrows one (left alone). A bare paramless function
name auto-calls and so transfers a fresh reference, so the `nkIdent` case
returned owned for a `syFunc` symbol. But inside that function's own body the
same name is the result variable, loaded from the result slot: a borrow. So in a
result-building loop like `lower := lower + s[i]`, the left operand `lower` was
released as a concat temporary and then released again as the assignment's old
value, freeing the live result string. The fix guards the `nkIdent` case with
`and (Nodes[node].Sym <> CurFuncSym)`, so the result-variable read is a borrow.
After it, every tier reports `GH_WAF 0` compiling the same programs (the whole
`kpcom.pas` included).
