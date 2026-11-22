/* wasm_emit.c : WebAssembly (WAT text) backend.

   The sixth code generator, a stack machine rather than a register machine.
   See doc/internals/wasm-backend.md for the design. WASM has unlimited typed
   locals, so there is no register allocator (regalloc is a nop) and no spill
   handling. An IR temp becomes a typed WASM local; an IR local slot becomes a
   frame in linear memory reached through the $sp global, because a slot can be
   address-taken and so needs real addressable memory.

   WASM has no arbitrary branch to a label: control flow is the structured
   block / loop / if with a depth-indexed br. The flat IR carries labels and
   branches, so a structurer reconstructs the structure, the stack-machine
   analogue of the register backends' register allocator. It follows Norman
   Ramsey's "Beyond Relooper": a dominator-tree walk that wraps a loop around
   each loop header and a block around each merge node, translating every IR
   branch to a br to the enclosing construct, or to an inline emission of a
   dominated successor. The front ends emit reducible control flow, so the
   common path needs no node splitting.

   Phases still ahead: calls, floating point, the 64-bit integer ops, and
   globals. An IR op from one of those stops the emitter with a clear message
   rather than emit wrong code. WASM32 is ILP32 little-endian, matching the IR
   default width. */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "ir.h"
#include "util.h"
#include "enc.h"

enum wtype { WT_I32, WT_I64, WT_F32, WT_F64 };

/* Output format, chosen by the driver from the output file extension: 0 emits
   WAT text (assembled by wat2wasm), 1 emits a complete binary module.  A later
   phase adds a relocatable object.  The driver sets it before target_emit. */
int wasm_output_binary;

/* Per-function working state, rebuilt in emit_function. */
static int         nparams;
static int         ntemps;
static enum wtype *temp_type;   /* [ntemps] */
static int        *g_use;       /* [ntemps] operand use counts, for void-call detection */
static int        *slot_off;    /* [nslots] byte offset of each slot in the frame */
static int         frame_size;  /* total frame bytes, 16-aligned */
/* Varargs (fixed-plus-buffer ABI).  A variadic function takes one hidden i32
   parameter after its declared parameters, the pointer to the caller's varargs
   buffer; IR_VA_START reads it.  A function that makes a variadic call reserves
   one buffer region in its frame (sized to the largest such call), into which
   the caller stores the unnamed arguments before passing its address. */
static int         g_cur_variadic;  /* the current function takes `...` */
static int         g_nwparams;      /* WASM params = nparams + g_cur_variadic */
static int         g_va_buf_off;    /* frame offset of the outgoing varargs buffer */
static int         g_va_buf_size;   /* its size (0 if the function makes no variadic call) */

static const char *
wtype_name(enum wtype t)
{
    switch (t) {
    case WT_I32: return "i32";
    case WT_I64: return "i64";
    case WT_F32: return "f32";
    case WT_F64: return "f64";
    }
    return "i32";
}

/* the current function's result type, for a bare IR_RET that must still leave a
   value when the function returns one (cc accepts `return;` in an int function) */
static int        g_has_result;
static enum wtype g_result;

/* The WASM local index of IR temp t: the parameters (the declared ones plus the
   hidden varargs-buffer pointer of a variadic function) occupy indices
   [0, g_nwparams), the temps follow. */
static int
temp_local(int t)
{
    return g_nwparams + t;
}

/* Classify every temp of fn by its defining op into out[0..ntemps): i64 for a
   64-bit def, f32/f64 for a float def (by its width tag), i32 otherwise. */
static void
classify_into(struct ir_func *fn, enum wtype *out)
{
    unsigned char *isf32;
    struct ir_insn *i;
    int nt = fn->ntemps, t;

    for (t = 0; t < nt; t++)
        out[t] = WT_I32;
    isf32 = arena_zalloc(fn->arena, (size_t)(nt > 0 ? nt : 1));
    ir_classify_f32(fn, isf32);
    for (i = fn->head; i; i = i->next) {
        t = i->dst;
        if (t < 0 || t >= nt)
            continue;
        if (ir_op_is_float_def(i->op))
            out[t] = isf32[t] ? WT_F32 : WT_F64;
        else if (ir_op_is_i64_def(i->op))
            out[t] = WT_I64;
    }
}

static void
classify_temps(struct ir_func *fn)
{
    temp_type = arena_zalloc(fn->arena, (size_t)(ntemps > 0 ? ntemps : 1) *
                             sizeof(*temp_type));
    classify_into(fn, temp_type);
}

/* The byte size an unnamed (variadic) argument occupies in the varargs buffer,
   by its ARG op and the temp width.  It matches the per-ABI __va_arg cursor: a
   32-bit int or pointer is 4 bytes, a double or 64-bit integer is 8 and
   8-aligned.  An unnamed float is promoted to double by the front end. */
static void
va_slot(int op, enum wtype wt, int *size, int *align)
{
    if (op == IR_ARG64 || (op == IR_FARG && wt == WT_F64)) {
        *size = 8; *align = 8;
    } else if (op == IR_FARG && wt == WT_F32) {
        *size = 4; *align = 4;
    } else {                            /* IR_ARG: i32 / pointer */
        *size = 4; *align = 4;
    }
}

/* The number of bytes of varargs buffer the largest variadic call in fn needs:
   the unnamed arguments laid out end to end with each 8-byte slot 8-aligned. */
static int
va_buf_bytes(struct ir_func *fn)
{
    struct ir_insn *in;
    enum wtype *lt = temp_type;
    int cur = 0, max = 0;

    for (in = fn->head; in; in = in->next) {
        switch (in->op) {
        case IR_ARG: case IR_ARG64: case IR_FARG:
            if (in->vararg) {
                enum wtype wt = (in->a >= 0 && in->a < ntemps) ? lt[in->a] : WT_I32;
                int sz, al;
                va_slot(in->op, wt, &sz, &al);
                cur = (cur + al - 1) & ~(al - 1);
                cur += sz;
            }
            break;
        case IR_CALL: case IR_FCALL: case IR_CALL64:
        case IR_CALLI: case IR_FCALLI: case IR_CALLI64:
            if (in->call_variadic && cur > max)
                max = cur;
            cur = 0;
            break;
        default:
            break;
        }
    }
    return max;
}

/* Lay the local slots out in a frame: each at its natural alignment, then, after
   them, the outgoing varargs buffer (when the function makes a variadic call).
   The total is rounded up to 16 bytes.  The parameters are slots too (the
   prologue spills the incoming WASM params into them), so [0, nparams) are part
   of the frame. */
static void
layout_frame(struct ir_func *fn)
{
    int ns = fn->nslots;
    int off = 0, s;

    slot_off = ns > 0 ? arena_zalloc(fn->arena, (size_t)ns * sizeof(int)) : NULL;
    for (s = 0; s < ns; s++) {
        int sz = fn->slot_size ? fn->slot_size[s] : 4;
        int al = (fn->slot_align && fn->slot_align[s]) ? fn->slot_align[s] : sz;
        if (sz <= 0)
            sz = 4;
        if (al <= 0)
            al = sz;
        off = (off + al - 1) & ~(al - 1);
        slot_off[s] = off;
        off += sz;
    }
    g_va_buf_size = va_buf_bytes(fn);
    if (g_va_buf_size > 0) {
        off = (off + 7) & ~7;           /* the buffer is 8-aligned */
        g_va_buf_off = off;
        off += g_va_buf_size;
    } else {
        g_va_buf_off = 0;
    }
    frame_size = (off + 15) & ~15;
}

static void
unsupported(const char *what)
{
    die("wasm: %s not yet lowered (later phase)", what);
}

/****************************************************************
 * Call signatures
 *
 * WASM call is strongly typed: a call names a function whose declared type
 * fixes the operand and result types.  So the emitter needs each callee's
 * signature.  A pre-pass over the program records every defined function's
 * result type (its parameters come from its own header at its call site).
 ****************************************************************/

#define WASM_MAX_ARGS 64

struct callsig {
    int        nparams;
    enum wtype params[WASM_MAX_ARGS];
    int        has_result;
    enum wtype result;
};

/* a defined function: its result type (for direct calls) and its table slot
   (for a function pointer and the indirect-call table) */
struct fnsig {
    const char *name;
    int         has_result;
    enum wtype  result;
    int         index;          /* table slot = position in g_sigs */
};

static struct fnsig *g_sigs;
static int           g_nsigs;

/* external (imported) functions, signature inferred from the call sites */
struct extsym {
    const char    *name;
    struct callsig sig;
};
static struct extsym *g_ext;
static int            g_next;

/* distinct indirect-call signatures, each getting a WASM type index */
static struct callsig *g_itypes;
static int             g_nitypes;

/* Binary module plan, built by build_module_plan before the binary sections are
   written.  g_nimports is the count of imported functions (they occupy the low
   function indices); g_itype_typeidx maps each indirect-call signature to its
   index in the module's type section.  $sp is the only global, so index 0. */
struct functype {
    int        np;
    enum wtype p[WASM_MAX_ARGS];
    int        nr;                  /* 0 or 1 results */
    enum wtype r;
};
static struct functype *g_types;    /* deduped type table = the type section */
static int              g_ntypes;
static int  g_nimports;
static int  g_ndef;                 /* count of program-defined functions */
static int *g_ext_tidx;             /* [g_next]  env import -> type index */
static int  g_wasi_tidx[3];         /* fd_write, fd_read, proc_exit type indices */
static int *g_def_tidx;             /* [g_nsigs] defined func -> type index */
static int  g_start_tidx;           /* _start's type index */
static int *g_itype_typeidx;        /* [g_nitypes] indirect sig -> type index */
#define WASM_SP_GLOBAL 0
static int module_funcidx(const char *name);

/* Delimited continuations via the WasmFX stack-switching proposal (binary
   complete-module path only; see doc/internals/wasm-continuations.md).  Set up
   when a program uses shift/reset (IR_MARK/IR_CAPTURE/IR_RESUME). */
static int  g_uses_cont;             /* program uses shift/reset */
/* A reset body thunk is a lifted lambda, so its type carries the hidden closure
   self pointer: (i32 self, i32 arg)->i32.  cont.new builds a continuation of
   that 2-param type (g_cont2).  After the body suspends via the $shift tag
   (()->i32), the remainder continuation takes just the resumed i32 and yields
   the extent result: a 1-param type (g_cont1), what the handler and resume use. */
static int  g_ft1_tidx;              /* (i32)->i32: the remainder's function type */
static int  g_ft2_tidx;              /* (i32 i32)->i32: the thunk's function type */
static int  g_tag_tidx;              /* ()->i32: the $shift tag's type */
static int  g_cont1_tidx;            /* cont g_ft1: the remainder continuation */
static int  g_cont2_tidx;            /* cont g_ft2: the fresh continuation (cont.new) */
static const char **g_thunks;        /* reset body thunk names, for elem declare */
static int  g_nthunks;
#define WASM_CONTS_TOP_GLOBAL 1       /* $conts_top: global index 1 (after $sp) */
#define WASM_CONTS_TABLE      1       /* $conts: table index 1 (after the funcref table) */
#define WASM_SHIFT_TAG        0       /* the one control tag */
#define WASM_CONTS_SIZE       4096    /* $conts capacity; reclaimed per reset extent */
/* per-function, set in prepare_function for a function that contains IR_MARK */
static int  g_fn_has_mark;
static int  g_cont_kref_local;        /* a (ref null $ct) scratch local */
static int  g_cont_vtmp_local;        /* an i32 scratch local */
static int  g_cont_savedtop_local;    /* an i32 holding the mark-time $conts_top */

/* Whether a program defining main gets the synthesized WASI runtime (the seam
   and _start).  The complete-module path synthesizes it; the relocatable-object
   path does not (the linker will, a later phase), so main is just a function. */
static int g_synth_runtime = 1;

/* A relocation against the code or data section: a standard WASM object reloc.
   The offset is relative to the section's contents; for the code section it is
   filled in body-relative and biased to the section offset when the body is
   spliced into the section payload. */
struct wreloc {
    unsigned char type;
    size_t        off;
    int           index;            /* symbol index, or a type index for TYPE */
    long          addend;
};
struct wrelocs {
    struct wreloc *v;
    int            n, cap;
};

/* WASM object relocation types (tool-conventions Linking.md) */
#define R_WASM_FUNCTION_INDEX_LEB 0
#define R_WASM_TABLE_INDEX_SLEB   1
#define R_WASM_TABLE_INDEX_I32    2
#define R_WASM_MEMORY_ADDR_LEB    3
#define R_WASM_MEMORY_ADDR_SLEB   4
#define R_WASM_MEMORY_ADDR_I32    5
#define R_WASM_TYPE_INDEX_LEB     6
#define R_WASM_GLOBAL_INDEX_LEB   7

/* WASM linking symbol-table flags and kinds */
#define WASM_SYM_BINDING_WEAK      0x01
#define WASM_SYM_BINDING_LOCAL     0x02
#define WASM_SYM_VISIBILITY_HIDDEN 0x04
#define WASM_SYM_UNDEFINED         0x10
#define WASM_SYM_EXPORTED          0x20
#define WASM_SYMTAB_FUNCTION 0
#define WASM_SYMTAB_DATA     1
#define WASM_SYMTAB_GLOBAL   2

static int object_funcidx(const char *name);
static int sym_index_func(const char *name);
static int sym_index_data(const char *name);
static int table_index_of(const char *name);
static int g_sp_symidx;             /* __stack_pointer symbol index, -1 if unused */

/* WASI runtime: when the program defines main, the backend synthesizes a small
   runtime (a _start entry and the libc syscall seam write/read/_exit/exit) that
   maps onto WASI imports, so the module runs as a WASI command under wasmtime or
   Node.  The mapping lives in one place (emit_wasi_runtime), so a later custom
   import host is a second mapping, not a change elsewhere. */
static int g_has_main;
static int g_main_nparams;
static int g_seam_syn[4];        /* which of write/read/_exit/exit were synthesized */

/* a function's result type (void unless a value-return op is present), computed
   without touching the module-level per-function state */
static int
compute_result(struct ir_func *fn, enum wtype *out)
{
    struct ir_insn *i;
    for (i = fn->head; i; i = i->next) {
        switch (i->op) {
        case IR_RETV:   *out = WT_I32; return 1;
        case IR_RETV64: *out = WT_I64; return 1;
        case IR_FRETV: {
            unsigned char *isf32 = arena_zalloc(fn->arena,
                (size_t)(fn->ntemps > 0 ? fn->ntemps : 1));
            ir_classify_f32(fn, isf32);
            *out = (i->a >= 0 && i->a < fn->ntemps && isf32[i->a]) ? WT_F32 : WT_F64;
            return 1;
        }
        case IR_TAILCALL:
        case IR_TAILCALLI:
            /* A function whose only exit is a tail call returns whatever the
               callee returns; its own WASM result type must match, or the
               call_indirect at the tail (and every direct caller) mismatches.
               The sole producer of tail calls is the Scheme front end, whose
               values are the uniform ILP32 tagged word, so the result is i32.
               A function that also has an explicit value return is caught by
               the cases above, which precede any tail call in layout order. */
            *out = WT_I32;
            return 1;
        default:
            break;
        }
    }
    return 0;
}

static struct fnsig *
lookup_sig(const char *name)
{
    int k;
    for (k = 0; k < g_nsigs; k++)
        if (g_sigs[k].name && name && strcmp(g_sigs[k].name, name) == 0)
            return &g_sigs[k];
    return NULL;
}

static struct extsym *
lookup_ext(const char *name)
{
    int k;
    for (k = 0; k < g_next; k++)
        if (name && strcmp(g_ext[k].name, name) == 0)
            return &g_ext[k];
    return NULL;
}

/* The result type implied by a call op.  IR_CALL64/IR_FCALL always return (the
   op fixes the width).  A plain IR_CALL returns an i32 unless the callee is void:
   cc allocates a result temp even for a void callee, so the void-ness must come
   from cc's call_void flag, not from whether the temp is used.  A discarded value
   return still leaves an i32 that the caller must drop, and the WASM call has to
   match the callee's real signature across an object boundary. */
static void
call_result(int op, int dst, int call_void, enum wtype *lt, int *has, enum wtype *rt)
{
    switch (op) {
    case IR_CALL64: case IR_CALLI64:
        *has = 1; *rt = WT_I64; return;
    case IR_FCALL: case IR_FCALLI:
        *has = 1; *rt = (dst >= 0) ? lt[dst] : WT_F64; return;
    default: /* IR_CALL / IR_CALLI */
        *has = !call_void; *rt = WT_I32; return;
    }
}

/* count each temp's uses as an operand (fn's own temps) */
static void
count_uses(struct ir_func *fn, int *use)
{
    struct ir_insn *in;
    int nt = fn->ntemps, t;
    for (t = 0; t < nt; t++)
        use[t] = 0;
    for (in = fn->head; in; in = in->next) {
        if (in->a >= 0 && in->a < nt)
            use[in->a]++;
        if (in->b >= 0 && in->b < nt)
            use[in->b]++;
    }
}

static int
callsig_eq(const struct callsig *a, const struct callsig *b)
{
    int k;
    if (a->nparams != b->nparams || a->has_result != b->has_result)
        return 0;
    if (a->has_result && a->result != b->result)
        return 0;
    for (k = 0; k < a->nparams; k++)
        if (a->params[k] != b->params[k])
            return 0;
    return 1;
}

/* An external named __wasi_<field> is a WASI preview1 import: it imports
   wasi_snapshot_preview1.<field>.  Every other external is an env import whose
   field is its own name.  This is the wasi-libc naming convention; the in-tree
   libc's WASI shim (runtime/wasi_file.c) declares e.g. __wasi_path_open, which
   the backend emits as an import of wasi_snapshot_preview1.path_open.  The
   linker (skj-ld-wasm) carries the module through and emits a real import. */
#define WASI_PREFIX "__wasi_"
#define WASI_PREFIX_LEN 7

static int
is_wasi_ext(const char *name)
{
    return strncmp(name, WASI_PREFIX, WASI_PREFIX_LEN) == 0;
}

static const char *
ext_import_module(const char *name)
{
    return is_wasi_ext(name) ? "wasi_snapshot_preview1" : "env";
}

static const char *
ext_import_field(const char *name)
{
    return is_wasi_ext(name) ? name + WASI_PREFIX_LEN : name;
}

static void
add_external(struct ir_program *prog, const char *name, const struct callsig *sig)
{
    struct extsym *e = lookup_ext(name);
    if (e) {
        /* prefer the richer signature if the program is inconsistent */
        if (!e->sig.has_result && sig->has_result)
            e->sig = *sig;
        else if (sig->nparams > e->sig.nparams)
            e->sig = *sig;
        return;
    }
    g_ext[g_next].name = name;
    g_ext[g_next].sig = *sig;
    g_next++;
    (void)prog;
}

static int
add_itype(const struct callsig *sig)
{
    int k;
    for (k = 0; k < g_nitypes; k++)
        if (callsig_eq(&g_itypes[k], sig))
            return k;
    g_itypes[g_nitypes] = *sig;
    return g_nitypes++;
}

/* Build a call's WASM operand signature from the accumulated argument types.
   A variadic call keeps only the named arguments (pva[c] == 0) and appends one
   i32, the varargs buffer pointer, matching the fixed-plus-buffer ABI. */
static void
collect_sig(struct callsig *sig, const enum wtype *parg, const int *pva,
            int np, int variadic)
{
    int c, n = 0;

    for (c = 0; c < np && c < WASM_MAX_ARGS; c++) {
        if (variadic && pva[c])
            continue;
        sig->params[n++] = parg[c];
    }
    if (variadic && n < WASM_MAX_ARGS)
        sig->params[n++] = WT_I32;
    sig->nparams = n;
}

/* Walk the program: record each defined function's signature and table slot,
   infer an import signature for every external call, and gather the distinct
   indirect-call signatures. */
static void
collect_calls(struct ir_program *prog)
{
    struct ir_func *fn;
    struct arena *ar;
    int ndef = 0, ncall = 0, nlea = 0, k;

    for (fn = prog->funcs; fn; fn = fn->next) {
        struct ir_insn *in;
        ndef++;
        for (in = fn->head; in; in = in->next) {
            if (in->op == IR_CALL || in->op == IR_CALLI || in->op == IR_FCALL ||
                in->op == IR_FCALLI || in->op == IR_CALL64 || in->op == IR_CALLI64 ||
                in->op == IR_TAILCALL || in->op == IR_TAILCALLI)
                ncall++;
            /* an addressed function may become an external (collect_ext_funcs) */
            else if (in->op == IR_LEA && in->sym && !in->sym_is_data)
                nlea++;
        }
    }
    if (!prog->funcs)
        return;
    ar = prog->funcs->arena;

    g_nsigs = ndef;
    /* +4 slack for the synthesized seam functions (write/read/_exit/exit) */
    g_sigs = arena_alloc(ar, (size_t)(ndef + 4) * sizeof(*g_sigs));
    /* g_ext holds call-target externals plus any addressed-only external
       functions collect_ext_funcs discovers, so size it for both */
    g_ext = arena_alloc(ar, (size_t)(ncall + nlea > 0 ? ncall + nlea : 1) * sizeof(*g_ext));
    g_itypes = arena_alloc(ar, (size_t)(ncall > 0 ? ncall : 1) * sizeof(*g_itypes));
    g_next = 0;
    g_nitypes = 0;

    k = 0;
    for (fn = prog->funcs; fn; fn = fn->next) {
        g_sigs[k].name = fn->name;
        g_sigs[k].has_result = compute_result(fn, &g_sigs[k].result);
        g_sigs[k].index = k;
        if (strcmp(fn->name, "main") == 0) {
            g_has_main = 1;
            g_main_nparams = fn->nparams;
        }
        k++;
    }

    /* a complete program (defines main) gets the synthesized seam functions,
       registered here so a call to them resolves like a defined function; a
       relocatable object does not (the linker synthesizes the runtime) */
    if (g_has_main && g_synth_runtime) {
        static const struct { const char *name; int has_result; } seam[] = {
            { "write", 1 }, { "read", 1 }, { "_exit", 0 }, { "exit", 0 },
        };
        int s;
        for (s = 0; s < 4; s++) {
            if (lookup_sig(seam[s].name))
                continue;               /* the program defines it itself */
            g_sigs[g_nsigs].name = seam[s].name;
            g_sigs[g_nsigs].has_result = seam[s].has_result;
            g_sigs[g_nsigs].result = WT_I32;
            g_sigs[g_nsigs].index = g_nsigs;
            g_nsigs++;
            g_seam_syn[s] = 1;
        }
    }

    for (fn = prog->funcs; fn; fn = fn->next) {
        enum wtype *lt = arena_alloc(ar,
            (size_t)(fn->ntemps > 0 ? fn->ntemps : 1) * sizeof(enum wtype));
        struct ir_insn *in;
        struct callsig sig;
        int np = 0;
        enum wtype parg[WASM_MAX_ARGS];
        int pva[WASM_MAX_ARGS];

        classify_into(fn, lt);
        for (in = fn->head; in; in = in->next) {
            switch (in->op) {
            case IR_ARG: case IR_ARG64:
                if (np < WASM_MAX_ARGS) {
                    parg[np] = (in->a >= 0 && in->a < fn->ntemps) ? lt[in->a] : WT_I32;
                    pva[np] = in->vararg;
                }
                np++;
                break;
            case IR_FARG:
                if (np < WASM_MAX_ARGS) {
                    parg[np] = (in->a >= 0 && in->a < fn->ntemps) ? lt[in->a] : WT_F64;
                    pva[np] = in->vararg;
                }
                np++;
                break;
            case IR_CALL: case IR_FCALL: case IR_CALL64:
                if (!lookup_sig(in->sym)) {
                    collect_sig(&sig, parg, pva, np, in->call_variadic);
                    call_result(in->op, in->dst, in->call_void, lt, &sig.has_result, &sig.result);
                    add_external(prog, in->sym, &sig);
                }
                np = 0;
                break;
            case IR_CALLI: case IR_FCALLI: case IR_CALLI64:
                collect_sig(&sig, parg, pva, np, in->call_variadic);
                call_result(in->op, in->dst, in->call_void, lt, &sig.has_result, &sig.result);
                add_itype(&sig);
                np = 0;
                break;
            case IR_TAILCALL:
                if (!lookup_sig(in->sym)) {
                    collect_sig(&sig, parg, pva, np, in->call_variadic);
                    sig.has_result = compute_result(fn, &sig.result);
                    add_external(prog, in->sym, &sig);
                }
                np = 0;
                break;
            case IR_TAILCALLI:
                collect_sig(&sig, parg, pva, np, in->call_variadic);
                sig.has_result = compute_result(fn, &sig.result);
                add_itype(&sig);
                np = 0;
                break;
            default:
                break;
            }
        }
    }
}

/****************************************************************
 * Globals: data in linear memory
 *
 * Each global is placed at a fixed linear-memory address in a data region that
 * sits below the shadow stack.  IR_LEA of a global yields that address, so a
 * load or store through it reaches the global's bytes.  A symbol reference
 * inside an initializer resolves to the target's address (a data address for a
 * global, a table slot for a function), written as a 4-byte little-endian word.
 ****************************************************************/

#define WASM_DATA_BASE   1024           /* leave the low addresses as a null guard */
#define WASM_STACK_BYTES (1 << 20)      /* 1 MiB shadow stack above the data */

struct gaddr {
    const char *name;
    int         addr;
};
static struct gaddr *g_gaddr;
static int           g_ngaddr;
static int           g_stack_top;
static int           g_mem_pages;

static int
global_elsz(struct ir_global *g)
{
    switch (g->base_type) {
    case IR_I8:  return 1;
    case IR_I16: return 2;
    case IR_F64: case IR_I64: return 8;
    default:     return 4;
    }
}

static int
global_is_bss(struct ir_global *g)
{
    return !g->inits && !g->init_string && g->init_count == 0;
}

/* total byte size of a global's storage */
static int
global_total(struct ir_global *g)
{
    int elsz = global_elsz(g);
    if (g->is_strobj)
        return 16 + g->init_strlen + 1;             /* header + text + NUL */
    if (g->inits)
        return g->arr_size;
    if (g->init_string)
        return g->arr_size > g->init_strlen ? g->arr_size : g->init_strlen;
    {   /* init_ivals, or bss */
        int n = g->arr_size > 0 ? g->arr_size
              : (g->init_count > 0 ? g->init_count : 1);
        return n * elsz;
    }
}

static int
global_align(struct ir_global *g)
{
    int al = 4;
    if (g->align > al)
        al = g->align;
    if ((g->base_type == IR_I64 || g->base_type == IR_F64) && al < 8)
        al = 8;
    return al;
}

/* assign a linear-memory address to every global and size the memory + stack */
static void
assign_globals(struct ir_program *prog)
{
    struct ir_global *g;
    int n = 0, addr = WASM_DATA_BASE, data_end;

    for (g = prog->globals; g; g = g->next)
        n++;
    g_gaddr = n ? malloc((size_t)n * sizeof(*g_gaddr)) : NULL;
    g_ngaddr = 0;
    for (g = prog->globals; g; g = g->next) {
        int al = global_align(g);
        addr = (addr + al - 1) & ~(al - 1);
        g_gaddr[g_ngaddr].name = g->name;
        g_gaddr[g_ngaddr].addr = addr;
        g_ngaddr++;
        addr += global_total(g);
    }
    data_end = (addr + 15) & ~15;
    g_stack_top = data_end + WASM_STACK_BYTES;
    g_mem_pages = (g_stack_top + 65535) / 65536;
}

static int
global_addr(const char *name)
{
    int k;
    for (k = 0; k < g_ngaddr; k++)
        if (name && strcmp(g_gaddr[k].name, name) == 0)
            return g_gaddr[k].addr;
    return -1;
}

/* resolve an initializer symbol reference to a concrete address: a global's
   data address, or a function's table slot */
static int
resolve_sym(const char *name)
{
    int a = global_addr(name);
    struct fnsig *f;
    if (a >= 0)
        return a;
    f = lookup_sig(name);
    if (f)
        return f->index;
    die("wasm: initializer references unknown symbol %s", name ? name : "(null)");
    return 0;
}

static void
put_le(unsigned char *buf, int off, uint64_t val, int size)
{
    int j;
    for (j = 0; j < size; j++)
        buf[off + j] = (unsigned char)((val >> (8 * j)) & 0xff);
}

/* build a global's initialized byte image into buf[0..global_total) */
static void
build_image(struct ir_global *g, unsigned char *buf, int total)
{
    int elsz = global_elsz(g), k;

    memset(buf, 0, (size_t)total);
    if (g->is_strobj) {
        put_le(buf, 0, 0xffffffffu, 4);             /* KP_RC_STATIC */
        put_le(buf, 4, 1u, 4);                      /* KP_OBJ_STRING */
        put_le(buf, 8, (uint64_t)g->init_strlen, 4);
        put_le(buf, 12, (uint64_t)g->init_strlen, 4);
        if (g->init_strlen > 0)
            memcpy(buf + 16, g->init_string, (size_t)g->init_strlen);
        return;
    }
    if (g->inits) {
        struct ir_init *it;
        for (it = g->inits; it; it = it->next) {
            if (it->offset < 0 || it->offset + it->size > total)
                continue;
            if (it->sym)
                put_le(buf, it->offset,
                       (uint64_t)(uint32_t)(resolve_sym(it->sym) + (int)it->ival), 4);
            else
                put_le(buf, it->offset, (uint64_t)it->ival, it->size);
        }
        return;
    }
    if (g->init_string) {
        if (g->init_strlen > 0)
            memcpy(buf, g->init_string, (size_t)g->init_strlen);
        return;
    }
    for (k = 0; k < g->init_count; k++) {
        if (g->init_syms && g->init_syms[k])
            put_le(buf, k * elsz,
                   (uint64_t)(uint32_t)(resolve_sym(g->init_syms[k]) +
                                        (int)(g->init_ivals ? g->init_ivals[k] : 0)), 4);
        else if (g->init_ivals)
            put_le(buf, k * elsz, (uint64_t)g->init_ivals[k], elsz);
    }
}

/* emit the data segments for all initialized globals */
static void
emit_globals(FILE *out, struct ir_program *prog)
{
    struct ir_global *g;
    int gi = 0;

    for (g = prog->globals; g; g = g->next, gi++) {
        int total, j;
        unsigned char *buf;
        if (global_is_bss(g))                       /* zero-filled: memory is zero */
            continue;
        total = global_total(g);
        if (total <= 0)
            continue;
        buf = malloc((size_t)total);
        build_image(g, buf, total);
        fprintf(out, "  (data (i32.const %d) \"", g_gaddr[gi].addr);
        for (j = 0; j < total; j++)
            fprintf(out, "\\%02x", buf[j]);
        fprintf(out, "\")\n");
        free(buf);
    }
}

/* pending outgoing call arguments, collected from IR_ARG/IR_FARG/IR_ARG64 */
static int arg_temps[WASM_MAX_ARGS];
static int arg_is_va[WASM_MAX_ARGS];    /* 1 if the pending arg is unnamed (variadic) */
static int arg_op[WASM_MAX_ARGS];       /* the arg's IR op (IR_ARG/IR_ARG64/IR_FARG) */
static int narg;

/****************************************************************
 * Emission sink
 *
 * Instruction-stream emission goes through a sink so the same selection and
 * control-flow structuring serve two outputs: the WAT text (assembled by
 * wat2wasm, used for the self-test and debugging) and, in a later phase, a
 * relocatable binary object for the in-tree linker.  Only the body of a
 * function flows through the sink; the module and function framing, the data
 * segments, and the synthesized runtime stay plain text for now.  The binary
 * sink (and the per-instruction opcodes recorded in the table below) are the
 * seam the object emitter plugs into.
 ****************************************************************/

/* every WASM instruction the backend emits, with its mnemonic and binary
   opcode (the opcode is unused by the text sink, present for the binary one). */
enum wop {
    WI_I32_ADD, WI_I32_SUB, WI_I32_MUL, WI_I32_DIV_S, WI_I32_DIV_U,
    WI_I32_REM_S, WI_I32_REM_U, WI_I32_AND, WI_I32_OR, WI_I32_XOR,
    WI_I32_SHL, WI_I32_SHR_S, WI_I32_SHR_U,
    WI_I32_EQ, WI_I32_NE, WI_I32_LT_S, WI_I32_LE_S, WI_I32_GT_S, WI_I32_GE_S,
    WI_I32_LT_U, WI_I32_LE_U, WI_I32_GT_U, WI_I32_GE_U,
    WI_I32_EXTEND8_S, WI_I32_EXTEND16_S, WI_I32_WRAP_I64,
    WI_I32_TRUNC_F32_S, WI_I32_TRUNC_F64_S,
    WI_I64_ADD, WI_I64_SUB, WI_I64_MUL, WI_I64_DIV_S, WI_I64_DIV_U,
    WI_I64_REM_S, WI_I64_REM_U, WI_I64_AND, WI_I64_OR, WI_I64_XOR,
    WI_I64_SHL, WI_I64_SHR_S, WI_I64_SHR_U,
    WI_I64_EQ, WI_I64_NE, WI_I64_LT_S, WI_I64_LE_S, WI_I64_GT_S, WI_I64_GE_S,
    WI_I64_LT_U, WI_I64_LE_U, WI_I64_GT_U, WI_I64_GE_U,
    WI_I64_EXTEND_I32_S, WI_I64_EXTEND_I32_U,
    WI_F32_ADD, WI_F32_SUB, WI_F32_MUL, WI_F32_DIV, WI_F32_NEG, WI_F32_ABS,
    WI_F32_SQRT, WI_F32_EQ, WI_F32_NE, WI_F32_LT, WI_F32_LE,
    WI_F32_CONVERT_I32_S, WI_F32_DEMOTE_F64,
    WI_F64_ADD, WI_F64_SUB, WI_F64_MUL, WI_F64_DIV, WI_F64_NEG, WI_F64_ABS,
    WI_F64_SQRT, WI_F64_NEAREST, WI_F64_EQ, WI_F64_NE, WI_F64_LT, WI_F64_LE,
    WI_F64_CONVERT_I32_S, WI_F64_PROMOTE_F32,
    WI_DROP, WI_RETURN, WI_UNREACHABLE,
    /* memory ops (take a memarg in the binary form; align is the natural one) */
    WI_I32_LOAD, WI_I32_LOAD8_U, WI_I32_LOAD8_S, WI_I32_LOAD16_U, WI_I32_LOAD16_S,
    WI_I64_LOAD, WI_F32_LOAD, WI_F64_LOAD,
    WI_I32_STORE, WI_I32_STORE8, WI_I32_STORE16,
    WI_I64_STORE, WI_F32_STORE, WI_F64_STORE,
    WI_NOP_LAST
};

static const struct { const char *mnem; unsigned char opcode; signed char align; } woptab[] = {
    { "i32.add", 0x6A, -1 }, { "i32.sub", 0x6B, -1 }, { "i32.mul", 0x6C, -1 },
    { "i32.div_s", 0x6D, -1 }, { "i32.div_u", 0x6E, -1 }, { "i32.rem_s", 0x6F, -1 },
    { "i32.rem_u", 0x70, -1 }, { "i32.and", 0x71, -1 }, { "i32.or", 0x72, -1 },
    { "i32.xor", 0x73, -1 }, { "i32.shl", 0x74, -1 }, { "i32.shr_s", 0x75, -1 },
    { "i32.shr_u", 0x76, -1 },
    { "i32.eq", 0x46, -1 }, { "i32.ne", 0x47, -1 }, { "i32.lt_s", 0x48, -1 },
    { "i32.le_s", 0x4C, -1 }, { "i32.gt_s", 0x4A, -1 }, { "i32.ge_s", 0x4E, -1 },
    { "i32.lt_u", 0x49, -1 }, { "i32.le_u", 0x4D, -1 }, { "i32.gt_u", 0x4B, -1 },
    { "i32.ge_u", 0x4F, -1 },
    { "i32.extend8_s", 0xC0, -1 }, { "i32.extend16_s", 0xC1, -1 },
    { "i32.wrap_i64", 0xA7, -1 },
    { "i32.trunc_f32_s", 0xA8, -1 }, { "i32.trunc_f64_s", 0xAA, -1 },
    { "i64.add", 0x7C, -1 }, { "i64.sub", 0x7D, -1 }, { "i64.mul", 0x7E, -1 },
    { "i64.div_s", 0x7F, -1 }, { "i64.div_u", 0x80, -1 }, { "i64.rem_s", 0x81, -1 },
    { "i64.rem_u", 0x82, -1 }, { "i64.and", 0x83, -1 }, { "i64.or", 0x84, -1 },
    { "i64.xor", 0x85, -1 }, { "i64.shl", 0x86, -1 }, { "i64.shr_s", 0x87, -1 },
    { "i64.shr_u", 0x88, -1 },
    { "i64.eq", 0x51, -1 }, { "i64.ne", 0x52, -1 }, { "i64.lt_s", 0x53, -1 },
    { "i64.le_s", 0x57, -1 }, { "i64.gt_s", 0x55, -1 }, { "i64.ge_s", 0x59, -1 },
    { "i64.lt_u", 0x54, -1 }, { "i64.le_u", 0x58, -1 }, { "i64.gt_u", 0x56, -1 },
    { "i64.ge_u", 0x5A, -1 },
    { "i64.extend_i32_s", 0xAC, -1 }, { "i64.extend_i32_u", 0xAD, -1 },
    { "f32.add", 0x92, -1 }, { "f32.sub", 0x93, -1 }, { "f32.mul", 0x94, -1 },
    { "f32.div", 0x95, -1 }, { "f32.neg", 0x8C, -1 }, { "f32.abs", 0x8B, -1 },
    { "f32.sqrt", 0x91, -1 }, { "f32.eq", 0x5B, -1 }, { "f32.ne", 0x5C, -1 },
    { "f32.lt", 0x5D, -1 }, { "f32.le", 0x5F, -1 },
    { "f32.convert_i32_s", 0xB2, -1 }, { "f32.demote_f64", 0xB6, -1 },
    { "f64.add", 0xA0, -1 }, { "f64.sub", 0xA1, -1 }, { "f64.mul", 0xA2, -1 },
    { "f64.div", 0xA3, -1 }, { "f64.neg", 0x9A, -1 }, { "f64.abs", 0x99, -1 },
    { "f64.sqrt", 0x9F, -1 }, { "f64.nearest", 0x9E, -1 }, { "f64.eq", 0x61, -1 },
    { "f64.ne", 0x62, -1 }, { "f64.lt", 0x63, -1 }, { "f64.le", 0x65, -1 },
    { "f64.convert_i32_s", 0xB7, -1 }, { "f64.promote_f32", 0xBB, -1 },
    { "drop", 0x1A, -1 }, { "return", 0x0F, -1 }, { "unreachable", 0x00, -1 },
    { "i32.load", 0x28, 2 }, { "i32.load8_u", 0x2D, 0 }, { "i32.load8_s", 0x2C, 0 },
    { "i32.load16_u", 0x2F, 1 }, { "i32.load16_s", 0x2E, 1 },
    { "i64.load", 0x29, 3 }, { "f32.load", 0x2A, 2 }, { "f64.load", 0x2B, 3 },
    { "i32.store", 0x36, 2 }, { "i32.store8", 0x3A, 0 }, { "i32.store16", 0x3B, 1 },
    { "i64.store", 0x37, 3 }, { "f32.store", 0x38, 2 }, { "f64.store", 0x39, 3 },
};

#define WS_TEXT 0
#define WS_BIN  1       /* a complete binary module (concrete indices) */
#define WS_OBJ  2       /* a relocatable object (padded indices + relocations) */
struct wsink {
    int            kind;
    FILE          *out;         /* WS_TEXT: the WAT output */
    struct wbuf   *code;        /* WS_BIN / WS_OBJ: the instruction byte stream */
    struct wrelocs *rc;         /* WS_OBJ: relocations, offsets relative to code */
};

/* the WASM blocktype for a void block/loop/if (no result) */
#define WASM_BT_VOID 0x40

/* append a relocation to a list */
static void
reloc_push(struct wrelocs *rc, unsigned char type, size_t off, int index, long addend)
{
    if (rc->n == rc->cap) {
        rc->cap = rc->cap ? rc->cap * 2 : 16;
        rc->v = realloc(rc->v, (size_t)rc->cap * sizeof(*rc->v));
    }
    rc->v[rc->n].type = type;
    rc->v[rc->n].off = off;
    rc->v[rc->n].index = index;
    rc->v[rc->n].addend = addend;
    rc->n++;
}

/* record a relocation at the current code offset (its target LEB follows) */
static void
reloc_add(struct wsink *s, unsigned char type, int index, long addend)
{
    reloc_push(s->rc, type, s->code->len, index, addend);
}

static unsigned char
wt_byte(enum wtype t)
{
    switch (t) {
    case WT_I32: return 0x7F;
    case WT_I64: return 0x7E;
    case WT_F32: return 0x7D;
    case WT_F64: return 0x7C;
    }
    return 0x7F;
}

static void
w_op(struct wsink *s, enum wop op)
{
    if (s->kind == WS_TEXT)
        fprintf(s->out, "\t\t%s\n", woptab[op].mnem);
    else
        wbuf_byte(s->code, woptab[op].opcode);
}

/* a memory op; `offset` is the static address offset (0 here, the address is on
   the stack).  The binary form emits the memarg (natural alignment + offset);
   text keeps the bare mnemonic since the offset is always 0 today. */
static void
w_mem(struct wsink *s, enum wop op, int offset)
{
    if (s->kind == WS_TEXT) {
        (void)offset;
        fprintf(s->out, "\t\t%s\n", woptab[op].mnem);
    } else {
        wbuf_byte(s->code, woptab[op].opcode);
        wbuf_uleb(s->code, (uint64_t)woptab[op].align);
        wbuf_uleb(s->code, (uint64_t)(uint32_t)offset);
    }
}

static void
w_const_i32(struct wsink *s, int32_t v)
{
    if (s->kind == WS_TEXT)
        fprintf(s->out, "\t\ti32.const %d\n", v);
    else {
        wbuf_byte(s->code, 0x41);
        wbuf_sleb(s->code, v);
    }
}

static void
w_const_i64(struct wsink *s, int64_t v)
{
    if (s->kind == WS_TEXT)
        fprintf(s->out, "\t\ti64.const %lld\n", (long long)v);
    else {
        wbuf_byte(s->code, 0x42);
        wbuf_sleb(s->code, v);
    }
}

/* a zero constant of the given type (a bare return in a value function) */
static void
w_zero(struct wsink *s, enum wtype t)
{
    if (s->kind == WS_TEXT) {
        fprintf(s->out, "\t\t%s.const 0\n", wtype_name(t));
        return;
    }
    switch (t) {
    case WT_I32: wbuf_byte(s->code, 0x41); wbuf_sleb(s->code, 0); break;
    case WT_I64: wbuf_byte(s->code, 0x42); wbuf_sleb(s->code, 0); break;
    case WT_F32: wbuf_byte(s->code, 0x43); wbuf_u32(s->code, 0); break;
    case WT_F64: wbuf_byte(s->code, 0x44);
                 wbuf_u32(s->code, 0); wbuf_u32(s->code, 0); break;
    }
}

static void
w_local_get(struct wsink *s, int idx)
{
    if (s->kind == WS_TEXT)
        fprintf(s->out, "\t\tlocal.get %d\n", idx);
    else {
        wbuf_byte(s->code, 0x20);
        wbuf_uleb(s->code, (uint64_t)idx);
    }
}

static void
w_local_set(struct wsink *s, int idx)
{
    if (s->kind == WS_TEXT)
        fprintf(s->out, "\t\tlocal.set %d\n", idx);
    else {
        wbuf_byte(s->code, 0x21);
        wbuf_uleb(s->code, (uint64_t)idx);
    }
}

static void
w_sp_get(struct wsink *s)
{
    if (s->kind == WS_TEXT)
        fprintf(s->out, "\t\tglobal.get $sp\n");
    else if (s->kind == WS_BIN) {
        wbuf_byte(s->code, 0x23);
        wbuf_uleb(s->code, WASM_SP_GLOBAL);
    } else {                            /* WS_OBJ: imported __stack_pointer */
        wbuf_byte(s->code, 0x23);
        reloc_add(s, R_WASM_GLOBAL_INDEX_LEB, g_sp_symidx, 0);
        wbuf_uleb_fixed(s->code, 0, 5);
    }
}

static void
w_sp_set(struct wsink *s)
{
    if (s->kind == WS_TEXT)
        fprintf(s->out, "\t\tglobal.set $sp\n");
    else if (s->kind == WS_BIN) {
        wbuf_byte(s->code, 0x24);
        wbuf_uleb(s->code, WASM_SP_GLOBAL);
    } else {                            /* WS_OBJ: imported __stack_pointer */
        wbuf_byte(s->code, 0x24);
        reloc_add(s, R_WASM_GLOBAL_INDEX_LEB, g_sp_symidx, 0);
        wbuf_uleb_fixed(s->code, 0, 5);
    }
}

/* --- WasmFX delimited-continuation primitives (binary module path) --- */

/* a reference type to a continuation type: 0x63 (ref null $ct) or 0x64
   (ref $ct), then the type index as a heaptype */
static void
w_cont_reftype(struct wbuf *b, int nullable, int conttype)
{
    wbuf_byte(b, nullable ? 0x63 : 0x64);
    wbuf_sleb(b, conttype);
}

static void
w_block_i32(struct wsink *s)
{
    if (s->kind == WS_TEXT)
        fprintf(s->out, "\t\tblock (result i32)\n");
    else {
        wbuf_byte(s->code, 0x02);
        wbuf_byte(s->code, 0x7F);
    }
}

/* block whose result is the remainder continuation (ref $ct1) the shift
   handler receives */
static void
w_block_remcont(struct wsink *s)
{
    if (s->kind == WS_TEXT)
        fprintf(s->out, "\t\tblock (result (ref $ct1))\n");
    else {
        wbuf_byte(s->code, 0x02);
        w_cont_reftype(s->code, 0, g_cont1_tidx);
    }
}

static void
w_global_get(struct wsink *s, int idx)
{
    if (s->kind == WS_TEXT)
        fprintf(s->out, "\t\tglobal.get %d\n", idx);
    else {
        wbuf_byte(s->code, 0x23);
        wbuf_uleb(s->code, (uint64_t)idx);
    }
}

static void
w_global_set(struct wsink *s, int idx)
{
    if (s->kind == WS_TEXT)
        fprintf(s->out, "\t\tglobal.set %d\n", idx);
    else {
        wbuf_byte(s->code, 0x24);
        wbuf_uleb(s->code, (uint64_t)idx);
    }
}

static void
w_table_get(struct wsink *s, int table)
{
    if (s->kind == WS_TEXT)
        fprintf(s->out, "\t\ttable.get %d\n", table);
    else {
        wbuf_byte(s->code, 0x25);
        wbuf_uleb(s->code, (uint64_t)table);
    }
}

static void
w_table_set(struct wsink *s, int table)
{
    if (s->kind == WS_TEXT)
        fprintf(s->out, "\t\ttable.set %d\n", table);
    else {
        wbuf_byte(s->code, 0x26);
        wbuf_uleb(s->code, (uint64_t)table);
    }
}

static void
w_ref_func(struct wsink *s, const char *name)
{
    if (s->kind == WS_TEXT)
        fprintf(s->out, "\t\tref.func %s\n", name);
    else {
        wbuf_byte(s->code, 0xD2);
        wbuf_uleb(s->code, (uint64_t)module_funcidx(name));
    }
}

static void
w_cont_new(struct wsink *s, int conttype)
{
    if (s->kind == WS_TEXT)
        fprintf(s->out, "\t\tcont.new %d\n", conttype);
    else {
        wbuf_byte(s->code, 0xE0);
        wbuf_uleb(s->code, (uint64_t)conttype);
    }
}

static void
w_suspend(struct wsink *s)
{
    if (s->kind == WS_TEXT)
        fprintf(s->out, "\t\tsuspend $shift\n");
    else {
        wbuf_byte(s->code, 0xE2);
        wbuf_uleb(s->code, WASM_SHIFT_TAG);
    }
}

/* resume the continuation on the stack (of type `conttype`).  With handler != 0,
   install a single on-label handler for the $shift tag branching to `label` (a
   relative block depth); otherwise the resume takes no handlers. */
static void
w_resume(struct wsink *s, int conttype, int handler, int label)
{
    if (s->kind == WS_TEXT) {
        if (handler)
            fprintf(s->out, "\t\tresume %d (on $shift %d)\n", conttype, label);
        else
            fprintf(s->out, "\t\tresume %d\n", conttype);
    } else {
        wbuf_byte(s->code, 0xE3);
        wbuf_uleb(s->code, (uint64_t)conttype);
        wbuf_uleb(s->code, (uint64_t)(handler ? 1 : 0));
        if (handler) {
            wbuf_byte(s->code, 0x00);        /* on-label handler variant */
            wbuf_uleb(s->code, WASM_SHIFT_TAG);
            wbuf_uleb(s->code, (uint64_t)label);
        }
    }
}

/* a call to a function by name.  Binary resolves the name to a function index
   (a FUNCTION_INDEX_LEB relocation will replace the concrete index in the
   relocatable-object phase). */
static void
w_call(struct wsink *s, const char *name)
{
    if (s->kind == WS_TEXT)
        fprintf(s->out, "\t\tcall $%s\n", name);
    else if (s->kind == WS_BIN) {
        wbuf_byte(s->code, 0x10);
        wbuf_uleb(s->code, (uint64_t)module_funcidx(name));
    } else {                            /* WS_OBJ */
        wbuf_byte(s->code, 0x10);
        reloc_add(s, R_WASM_FUNCTION_INDEX_LEB, sym_index_func(name), 0);
        wbuf_uleb_fixed(s->code, (uint64_t)object_funcidx(name), 5);
    }
}

/* an indirect call against an indirect-type index (a g_itypes slot); the type
   index references the module/object type section (TYPE_INDEX reloc in an
   object), and table 0 is the indirect function table */
static void
w_call_indirect(struct wsink *s, int typeidx)
{
    if (s->kind == WS_TEXT) {
        /* WAT names the type directly; g_itype_typeidx is a binary/object-only
           map and is not built on the text path, so do not read it here. */
        fprintf(s->out, "\t\tcall_indirect (type $wt%d)\n", typeidx);
    } else if (s->kind == WS_BIN) {
        int ti = g_itype_typeidx[typeidx];
        wbuf_byte(s->code, 0x11);
        wbuf_uleb(s->code, (uint64_t)ti);
        wbuf_uleb(s->code, 0);          /* table index */
    } else {                            /* WS_OBJ */
        int ti = g_itype_typeidx[typeidx];
        wbuf_byte(s->code, 0x11);
        reloc_add(s, R_WASM_TYPE_INDEX_LEB, ti, 0);
        wbuf_uleb_fixed(s->code, (uint64_t)ti, 5);
        wbuf_uleb(s->code, 0);          /* table index */
    }
}

/* a constant that is an address: a global's data address or a function's table
   slot.  Text and the complete binary module emit the resolved value; an object
   emits a relocated placeholder.  The data-symbol and function-pointer object
   relocations arrive in a later phase; this path is unreached for now. */
static void
w_addr_const(struct wsink *s, int value, const char *sym, int is_func)
{
    if (s->kind == WS_TEXT)
        fprintf(s->out, "\t\ti32.const %d\n", value);
    else if (s->kind == WS_BIN) {
        wbuf_byte(s->code, 0x41);
        wbuf_sleb(s->code, value);
    } else {                            /* WS_OBJ */
        (void)value;
        wbuf_byte(s->code, 0x41);        /* i32.const */
        if (is_func) {                   /* a function pointer: a table slot */
            reloc_add(s, R_WASM_TABLE_INDEX_SLEB, sym_index_func(sym), 0);
            wbuf_sleb_fixed(s->code, table_index_of(sym), 5);
        } else {                         /* a data address */
            reloc_add(s, R_WASM_MEMORY_ADDR_SLEB, sym_index_data(sym), 0);
            wbuf_sleb_fixed(s->code, 0, 5);
        }
    }
}

static void
w_br(struct wsink *s, int depth)
{
    if (s->kind == WS_TEXT)
        fprintf(s->out, "\t\tbr %d\n", depth);
    else {
        wbuf_byte(s->code, 0x0C);
        wbuf_uleb(s->code, (uint64_t)depth);
    }
}

/* a br_table over `n` entries (targets[0..n)) plus the default depth.  Binary:
   opcode 0x0E, a vector of label depths, then the default. */
static void
w_br_table(struct wsink *s, const int *targets, int n, int deflt)
{
    int k;
    if (s->kind == WS_TEXT) {
        fprintf(s->out, "\t\tbr_table");
        for (k = 0; k < n; k++)
            fprintf(s->out, " %d", targets[k]);
        fprintf(s->out, " %d\n", deflt);
    } else {
        wbuf_byte(s->code, 0x0E);
        wbuf_uleb(s->code, (uint64_t)n);
        for (k = 0; k < n; k++)
            wbuf_uleb(s->code, (uint64_t)targets[k]);
        wbuf_uleb(s->code, (uint64_t)deflt);
    }
}

static void
w_structured(struct wsink *s, const char *mnem, unsigned char opcode)
{
    if (s->kind == WS_TEXT)
        fprintf(s->out, "\t\t%s\n", mnem);
    else {
        wbuf_byte(s->code, opcode);
        if (opcode != 0x05 && opcode != 0x0B)   /* else / end take no blocktype */
            wbuf_byte(s->code, WASM_BT_VOID);
    }
}

static void
w_block(struct wsink *s) { w_structured(s, "block", 0x02); }
static void
w_loop(struct wsink *s)  { w_structured(s, "loop",  0x03); }
static void
w_if(struct wsink *s)    { w_structured(s, "if",    0x04); }
static void
w_else(struct wsink *s)  { w_structured(s, "else",  0x05); }
static void
w_end(struct wsink *s)   { w_structured(s, "end",   0x0B); }

/****************************************************************
 * Straight-line instruction selection (within one basic block)
 ****************************************************************/

/* push IR temp t onto the WASM operand stack */
static void
push_temp(struct wsink *s, int t)
{
    if (t < 0 || t >= ntemps)
        die("wasm: operand temp %d out of range", t);
    w_local_get(s, temp_local(t));
}

/* store the top-of-stack value into IR temp t */
static void
set_temp(struct wsink *s, int t)
{
    if (t < 0 || t >= ntemps)
        die("wasm: result temp %d out of range", t);
    w_local_set(s, temp_local(t));
}

/* push the linear-memory address of local slot `slot` (frame base + offset) */
static void
push_slot_addr(struct wsink *s, int slot)
{
    if (!slot_off || slot < 0)
        die("wasm: slot %d out of range", slot);
    w_sp_get(s);
    if (slot_off[slot] != 0) {
        w_const_i32(s, slot_off[slot]);
        w_op(s, WI_I32_ADD);
    }
}

static void
binop_i32(struct wsink *s, struct ir_insn *i, enum wop op)
{
    push_temp(s, i->a);
    push_temp(s, i->b);
    w_op(s, op);
    set_temp(s, i->dst);
}

static void
binop_i64(struct wsink *s, struct ir_insn *i, enum wop op)
{
    push_temp(s, i->a);
    push_temp(s, i->b);
    w_op(s, op);
    set_temp(s, i->dst);
}

/* an i64 shift: WASM wants both operands i64, but the shift count may be i32 */
static void
shift_i64(struct wsink *s, struct ir_insn *i, enum wop op)
{
    push_temp(s, i->a);
    push_temp(s, i->b);
    if (i->b >= 0 && i->b < ntemps && temp_type[i->b] != WT_I64)
        w_op(s, WI_I64_EXTEND_I32_U);
    w_op(s, op);
    set_temp(s, i->dst);
}

/* whether temp t is single precision */
static int
is_f32_temp(int t)
{
    return t >= 0 && t < ntemps && temp_type[t] == WT_F32;
}

/* a float binary op whose width follows the result temp */
static void
fbinop(struct wsink *s, struct ir_insn *i, enum wop f32op, enum wop f64op)
{
    push_temp(s, i->a);
    push_temp(s, i->b);
    w_op(s, is_f32_temp(i->dst) ? f32op : f64op);
    set_temp(s, i->dst);
}

/* a float unary op whose width follows the result temp */
static void
funop(struct wsink *s, struct ir_insn *i, enum wop f32op, enum wop f64op)
{
    push_temp(s, i->a);
    w_op(s, is_f32_temp(i->dst) ? f32op : f64op);
    set_temp(s, i->dst);
}

/* a float comparison: operands are float (width from operand a), result i32 */
static void
fcmp(struct wsink *s, struct ir_insn *i, enum wop f32op, enum wop f64op)
{
    push_temp(s, i->a);
    push_temp(s, i->b);
    w_op(s, is_f32_temp(i->a) ? f32op : f64op);
    set_temp(s, i->dst);
}

/* keep or drop a call's result per whether the callee returns one */
static void
take_result(struct wsink *s, struct ir_insn *i, int has_result)
{
    if (!has_result)
        return;
    if (i->dst >= 0)
        set_temp(s, i->dst);
    else
        w_op(s, WI_DROP);
}

/* push the address of the outgoing varargs buffer plus `off` (frame base + the
   buffer offset + off) */
static void
push_va_buf_addr(struct wsink *s, int off)
{
    int a = g_va_buf_off + off;
    w_sp_get(s);
    if (a != 0) {
        w_const_i32(s, a);
        w_op(s, WI_I32_ADD);
    }
}

/* the store op for an unnamed argument of the given op and width */
static enum wop
va_store_op(int op, enum wtype wt)
{
    if (op == IR_ARG64)
        return WI_I64_STORE;
    if (op == IR_FARG)
        return wt == WT_F32 ? WI_F32_STORE : WI_F64_STORE;
    return WI_I32_STORE;
}

/* Push a variadic call's arguments under the fixed-plus-buffer ABI: the named
   arguments as ordinary operands, then the unnamed arguments stored into the
   frame's varargs buffer, then the buffer's address as the trailing argument.
   The callee's va_start reads that pointer, and __va_arg walks the buffer. */
static void
push_variadic_args(struct wsink *s)
{
    int k, cur = 0;

    for (k = 0; k < narg; k++)
        if (!arg_is_va[k])
            push_temp(s, arg_temps[k]);
    for (k = 0; k < narg; k++) {
        if (!arg_is_va[k])
            continue;
        enum wtype wt = (arg_temps[k] >= 0 && arg_temps[k] < ntemps)
                        ? temp_type[arg_temps[k]] : WT_I32;
        int sz, al;
        va_slot(arg_op[k], wt, &sz, &al);
        cur = (cur + al - 1) & ~(al - 1);
        push_va_buf_addr(s, cur);
        push_temp(s, arg_temps[k]);
        w_mem(s, va_store_op(arg_op[k], wt), 0);
        cur += sz;
    }
    push_va_buf_addr(s, 0);             /* the trailing buffer pointer */
}

/* emit a direct call (to a defined or imported function): push the pending
   args, call, then keep or drop the result.  narg resets for the next call. */
static void
emit_call(struct wsink *s, struct ir_insn *i)
{
    struct fnsig *def = lookup_sig(i->sym);
    struct extsym *ext = def ? NULL : lookup_ext(i->sym);
    int has_result, k;

    if (!def && !ext)
        die("wasm: call to unknown symbol %s", i->sym ? i->sym : "(null)");
    has_result = def ? def->has_result : ext->sig.has_result;
    if (i->call_variadic)
        push_variadic_args(s);
    else
        for (k = 0; k < narg; k++)
            push_temp(s, arg_temps[k]);
    w_call(s, i->sym);
    take_result(s, i, has_result);
    narg = 0;
}

/* the WASM signature of the pending call's operands: for a variadic call, the
   named arguments followed by one i32 (the varargs buffer pointer); otherwise
   every pending argument.  Mirrors how the operands are pushed. */
static void
pending_call_sig(struct callsig *sig, int variadic)
{
    int k, n = 0;

    for (k = 0; k < narg && n < WASM_MAX_ARGS; k++) {
        if (variadic && arg_is_va[k])
            continue;
        sig->params[n++] = (arg_temps[k] >= 0 && arg_temps[k] < ntemps)
                           ? temp_type[arg_temps[k]] : WT_I32;
    }
    if (variadic && n < WASM_MAX_ARGS)
        sig->params[n++] = WT_I32;      /* the trailing buffer pointer */
    sig->nparams = n;
}

/* emit an indirect call through a function pointer (a table slot in i->a): push
   the args, push the table index, call_indirect against the matched type */
static void
emit_calli(struct wsink *s, struct ir_insn *i)
{
    struct callsig sig;
    int k, ti;

    pending_call_sig(&sig, i->call_variadic);
    call_result(i->op, i->dst, i->call_void, temp_type, &sig.has_result, &sig.result);
    ti = add_itype(&sig);               /* already present from collect_calls */

    if (i->call_variadic)
        push_variadic_args(s);
    else
        for (k = 0; k < narg; k++)
            push_temp(s, arg_temps[k]);
    push_temp(s, i->a);                 /* the function pointer: a table slot */
    w_call_indirect(s, ti);
    take_result(s, i, sig.has_result);
    narg = 0;
}

/* Emit one non-terminator instruction.  The structurer calls this for a block's
   straight-line body; terminators (branches, returns) are its own concern. */
static void
emit_insn(struct wsink *s, struct ir_func *fn, struct ir_insn *i)
{
    (void)fn;
    /* The WasmFX module declarations (cont types, $shift tag, $conts table) are
       emitted only on the binary complete-module path, so a program that uses
       delimited continuations must be built to a .wasm module, not WAT text or a
       relocatable object. */
    if (s->kind != WS_BIN &&
        (i->op == IR_MARK || i->op == IR_CAPTURE ||
         i->op == IR_RESUME || i->op == IR_CONT_UNWIND))
        die("wasm: delimited continuations require a .wasm (binary module) output");
    switch (i->op) {
    case IR_FUNC:
    case IR_ENDF:
    case IR_NOP:
    case IR_LOC:
    case IR_LABEL:
        break;

    case IR_LIC:
        w_const_i32(s, (int)i->imm);
        set_temp(s, i->dst);
        break;

    case IR_MOV:
        push_temp(s, i->a);
        set_temp(s, i->dst);
        break;

    case IR_ADD:  binop_i32(s, i, WI_I32_ADD);   break;
    case IR_SUB:  binop_i32(s, i, WI_I32_SUB);   break;
    case IR_MUL:  binop_i32(s, i, WI_I32_MUL);   break;
    case IR_DIVS: binop_i32(s, i, WI_I32_DIV_S); break;
    case IR_DIVU: binop_i32(s, i, WI_I32_DIV_U); break;
    case IR_MODS: binop_i32(s, i, WI_I32_REM_S); break;
    case IR_MODU: binop_i32(s, i, WI_I32_REM_U); break;
    case IR_AND:  binop_i32(s, i, WI_I32_AND);   break;
    case IR_OR:   binop_i32(s, i, WI_I32_OR);    break;
    case IR_XOR:  binop_i32(s, i, WI_I32_XOR);   break;
    case IR_SHL:  binop_i32(s, i, WI_I32_SHL);   break;
    case IR_SHRS: binop_i32(s, i, WI_I32_SHR_S); break;
    case IR_SHRU: binop_i32(s, i, WI_I32_SHR_U); break;

    case IR_NEG:
        w_const_i32(s, 0);
        push_temp(s, i->a);
        w_op(s, WI_I32_SUB);
        set_temp(s, i->dst);
        break;

    case IR_NOT:
        push_temp(s, i->a);
        w_const_i32(s, -1);
        w_op(s, WI_I32_XOR);
        set_temp(s, i->dst);
        break;

    case IR_CMPEQ:  binop_i32(s, i, WI_I32_EQ);   break;
    case IR_CMPNE:  binop_i32(s, i, WI_I32_NE);   break;
    case IR_CMPLTS: binop_i32(s, i, WI_I32_LT_S); break;
    case IR_CMPLES: binop_i32(s, i, WI_I32_LE_S); break;
    case IR_CMPGTS: binop_i32(s, i, WI_I32_GT_S); break;
    case IR_CMPGES: binop_i32(s, i, WI_I32_GE_S); break;
    case IR_CMPLTU: binop_i32(s, i, WI_I32_LT_U); break;
    case IR_CMPLEU: binop_i32(s, i, WI_I32_LE_U); break;
    case IR_CMPGTU: binop_i32(s, i, WI_I32_GT_U); break;
    case IR_CMPGEU: binop_i32(s, i, WI_I32_GE_U); break;

    case IR_SEXTB: push_temp(s, i->a); w_op(s, WI_I32_EXTEND8_S);  set_temp(s, i->dst); break;
    case IR_SEXTH: push_temp(s, i->a); w_op(s, WI_I32_EXTEND16_S); set_temp(s, i->dst); break;
    case IR_ZEXTB: push_temp(s, i->a); w_const_i32(s, 255);   w_op(s, WI_I32_AND); set_temp(s, i->dst); break;
    case IR_ZEXTH: push_temp(s, i->a); w_const_i32(s, 65535); w_op(s, WI_I32_AND); set_temp(s, i->dst); break;

    /* loads: address in a, result in dst */
    case IR_LB:  push_temp(s, i->a); w_mem(s, WI_I32_LOAD8_U, 0);  set_temp(s, i->dst); break;
    case IR_LBS: push_temp(s, i->a); w_mem(s, WI_I32_LOAD8_S, 0);  set_temp(s, i->dst); break;
    case IR_LH:  push_temp(s, i->a); w_mem(s, WI_I32_LOAD16_U, 0); set_temp(s, i->dst); break;
    case IR_LHS: push_temp(s, i->a); w_mem(s, WI_I32_LOAD16_S, 0); set_temp(s, i->dst); break;
    case IR_LW:  push_temp(s, i->a); w_mem(s, WI_I32_LOAD, 0);     set_temp(s, i->dst); break;

    /* stores: address in a, value in b */
    case IR_SB: push_temp(s, i->a); push_temp(s, i->b); w_mem(s, WI_I32_STORE8, 0);  break;
    case IR_SH: push_temp(s, i->a); push_temp(s, i->b); w_mem(s, WI_I32_STORE16, 0); break;
    case IR_SW: push_temp(s, i->a); push_temp(s, i->b); w_mem(s, WI_I32_STORE, 0);   break;

    /* slot ops: the frame in linear memory */
    case IR_LDL:
        push_slot_addr(s, i->slot);
        w_mem(s, WI_I32_LOAD, 0);
        set_temp(s, i->dst);
        break;
    case IR_STL:
        push_slot_addr(s, i->slot);
        push_temp(s, i->a);
        w_mem(s, WI_I32_STORE, 0);
        break;
    case IR_ADL:
        push_slot_addr(s, i->slot);
        set_temp(s, i->dst);
        break;

    /* --- floating point (native f32/f64) --- */
    case IR_FADD: fbinop(s, i, WI_F32_ADD, WI_F64_ADD); break;
    case IR_FSUB: fbinop(s, i, WI_F32_SUB, WI_F64_SUB); break;
    case IR_FMUL: fbinop(s, i, WI_F32_MUL, WI_F64_MUL); break;
    case IR_FDIV: fbinop(s, i, WI_F32_DIV, WI_F64_DIV); break;
    case IR_FNEG: funop(s, i, WI_F32_NEG, WI_F64_NEG);  break;
    case IR_FABS: funop(s, i, WI_F32_ABS, WI_F64_ABS);  break;
    case IR_FSQRT: funop(s, i, WI_F32_SQRT, WI_F64_SQRT); break;
    case IR_FCMPEQ: fcmp(s, i, WI_F32_EQ, WI_F64_EQ); break;
    case IR_FCMPLT: fcmp(s, i, WI_F32_LT, WI_F64_LT); break;
    case IR_FCMPLE: fcmp(s, i, WI_F32_LE, WI_F64_LE); break;

    case IR_ITOF:   /* signed int -> float */
        push_temp(s, i->a);
        w_op(s, is_f32_temp(i->dst) ? WI_F32_CONVERT_I32_S : WI_F64_CONVERT_I32_S);
        set_temp(s, i->dst);
        break;
    case IR_FTOI:   /* float -> signed int (truncating) */
        push_temp(s, i->a);
        w_op(s, is_f32_temp(i->a) ? WI_I32_TRUNC_F32_S : WI_I32_TRUNC_F64_S);
        set_temp(s, i->dst);
        break;
    case IR_FROUND: /* f64 -> i32, round to nearest */
        push_temp(s, i->a);
        w_op(s, WI_F64_NEAREST);
        w_op(s, WI_I32_TRUNC_F64_S);
        set_temp(s, i->dst);
        break;
    case IR_F32TOF64:
        push_temp(s, i->a);
        w_op(s, WI_F64_PROMOTE_F32);
        set_temp(s, i->dst);
        break;
    case IR_F64TOF32:
        push_temp(s, i->a);
        w_op(s, WI_F32_DEMOTE_F64);
        set_temp(s, i->dst);
        break;

    /* IR_FLD/IR_FSD carry the access width in imm: FWIDTH_F32 is a single that
       stays f32 in its temp (a later IR_F32TOF64 widens it), anything else is a
       double.  Loading or storing the wrong width mis-sizes a float literal or a
       float through a pointer. */
    case IR_FLD:
        push_temp(s, i->a);
        w_mem(s, i->imm == FWIDTH_F32 ? WI_F32_LOAD : WI_F64_LOAD, 0);
        set_temp(s, i->dst);
        break;
    case IR_FLS: push_temp(s, i->a); w_mem(s, WI_F32_LOAD, 0); set_temp(s, i->dst); break;
    case IR_FSD:
        push_temp(s, i->a);
        push_temp(s, i->b);
        w_mem(s, i->imm == FWIDTH_F32 ? WI_F32_STORE : WI_F64_STORE, 0);
        break;
    case IR_FSS: push_temp(s, i->a); push_temp(s, i->b); w_mem(s, WI_F32_STORE, 0); break;
    case IR_FLDL:
        push_slot_addr(s, i->slot);
        w_mem(s, is_f32_temp(i->dst) ? WI_F32_LOAD : WI_F64_LOAD, 0);
        set_temp(s, i->dst);
        break;
    case IR_FSTL:
        push_slot_addr(s, i->slot);
        push_temp(s, i->a);
        w_mem(s, is_f32_temp(i->a) ? WI_F32_STORE : WI_F64_STORE, 0);
        break;

    /* --- 64-bit integers (native i64) --- */
    case IR_LIC64:
        w_const_i64(s, i->imm);
        set_temp(s, i->dst);
        break;
    case IR_ADD64:  binop_i64(s, i, WI_I64_ADD);   break;
    case IR_SUB64:  binop_i64(s, i, WI_I64_SUB);   break;
    case IR_MUL64:  binop_i64(s, i, WI_I64_MUL);   break;
    case IR_DIVS64: binop_i64(s, i, WI_I64_DIV_S); break;
    case IR_DIVU64: binop_i64(s, i, WI_I64_DIV_U); break;
    case IR_MODS64: binop_i64(s, i, WI_I64_REM_S); break;
    case IR_MODU64: binop_i64(s, i, WI_I64_REM_U); break;
    case IR_AND64:  binop_i64(s, i, WI_I64_AND);   break;
    case IR_OR64:   binop_i64(s, i, WI_I64_OR);    break;
    case IR_XOR64:  binop_i64(s, i, WI_I64_XOR);   break;
    case IR_SHL64:  shift_i64(s, i, WI_I64_SHL);   break;
    case IR_SHRS64: shift_i64(s, i, WI_I64_SHR_S); break;
    case IR_SHRU64: shift_i64(s, i, WI_I64_SHR_U); break;
    case IR_NEG64:
        w_const_i64(s, 0);
        push_temp(s, i->a);
        w_op(s, WI_I64_SUB);
        set_temp(s, i->dst);
        break;

    /* i64 comparisons produce an i32 (0/1) */
    case IR_CMP64EQ:  binop_i64(s, i, WI_I64_EQ);   break;
    case IR_CMP64NE:  binop_i64(s, i, WI_I64_NE);   break;
    case IR_CMP64LTS: binop_i64(s, i, WI_I64_LT_S); break;
    case IR_CMP64LES: binop_i64(s, i, WI_I64_LE_S); break;
    case IR_CMP64GTS: binop_i64(s, i, WI_I64_GT_S); break;
    case IR_CMP64GES: binop_i64(s, i, WI_I64_GE_S); break;
    case IR_CMP64LTU: binop_i64(s, i, WI_I64_LT_U); break;
    case IR_CMP64LEU: binop_i64(s, i, WI_I64_LE_U); break;
    case IR_CMP64GTU: binop_i64(s, i, WI_I64_GT_U); break;
    case IR_CMP64GEU: binop_i64(s, i, WI_I64_GE_U); break;

    case IR_LD64: push_temp(s, i->a); w_mem(s, WI_I64_LOAD, 0); set_temp(s, i->dst); break;
    case IR_ST64: push_temp(s, i->a); push_temp(s, i->b); w_mem(s, WI_I64_STORE, 0); break;
    case IR_LDL64:
        push_slot_addr(s, i->slot);
        w_mem(s, WI_I64_LOAD, 0);
        set_temp(s, i->dst);
        break;
    case IR_STL64:
        push_slot_addr(s, i->slot);
        push_temp(s, i->a);
        w_mem(s, WI_I64_STORE, 0);
        break;

    case IR_SEXT64:
        push_temp(s, i->a);
        w_op(s, WI_I64_EXTEND_I32_S);
        set_temp(s, i->dst);
        break;
    case IR_ZEXT64:
        push_temp(s, i->a);
        w_op(s, WI_I64_EXTEND_I32_U);
        set_temp(s, i->dst);
        break;
    case IR_TRUNC64:
        push_temp(s, i->a);
        w_op(s, WI_I32_WRAP_I64);
        set_temp(s, i->dst);
        break;
    case IR_MOV64:
        push_temp(s, i->a);
        set_temp(s, i->dst);
        break;

    case IR_ARG:
    case IR_FARG:
    case IR_ARG64:
        if (narg >= WASM_MAX_ARGS)
            die("wasm: too many call arguments");
        arg_op[narg] = i->op;
        arg_is_va[narg] = i->vararg;
        arg_temps[narg++] = i->a;
        break;

    case IR_CALL:
    case IR_FCALL:
    case IR_CALL64:
        emit_call(s, i);
        break;

    case IR_CALLI:
    case IR_FCALLI:
    case IR_CALLI64:
        emit_calli(s, i);
        break;

    case IR_LEA: {
        /* the address of a global is its data address; the address of a
           function is its table slot (a callable function pointer) */
        int a = global_addr(i->sym);
        int is_func = 0;
        struct fnsig *f;
        if (a < 0) {
            f = lookup_sig(i->sym);
            if (f) {
                a = f->index;
                is_func = 1;
            } else if (s->kind == WS_OBJ && i->sym_is_data) {
                /* an external data symbol's address (e.g. &errno): a
                   MEMORY_ADDR relocation against its undefined data symbol
                   carries the value, so the concrete address is unused here */
                a = 0;
            } else if (s->kind == WS_OBJ && lookup_ext(i->sym)) {
                /* an external function's address: the linker assigns its table
                   slot, so the concrete value is unused here */
                a = 0;
                is_func = 1;
            } else {
                die("wasm: address of unknown symbol %s", i->sym ? i->sym : "(null)");
            }
        }
        w_addr_const(s, a, i->sym, is_func);
        set_temp(s, i->dst);
        break;
    }
    case IR_VA_START:
        /* point the va_list's overflow_arg_area (offset 8 in __va_list_tag) at
           this function's hidden varargs-buffer pointer, the WASM param after
           the declared ones.  __va_arg then walks the buffer from there. */
        if (!g_cur_variadic)
            die("wasm: va_start in a non-variadic function");
        push_temp(s, i->a);
        w_const_i32(s, 8);
        w_op(s, WI_I32_ADD);
        w_local_get(s, nparams);
        w_mem(s, WI_I32_STORE, 0);
        break;
    case IR_CALL2: case IR_CALLI2:
        unsupported("failable calls");
        break;

    /* --- delimited continuations, lowered to WasmFX stack switching --- */
    case IR_MARK: {
        /* (reset body handler): run the lambda-lifted body thunk (i->sym) as a
           continuation under a handler for the $shift tag.  Produce i->dst = 0
           when the body returns normally (its value is stored to the i->slot
           shadow slot for the caller to read), or the nonzero $conts index of
           the captured continuation when the body suspended via shift.  See
           doc/internals/wasm-continuations.md. */
        w_global_get(s, WASM_CONTS_TOP_GLOBAL);  /* save the mark-time top so */
        w_local_set(s, g_cont_savedtop_local);   /* IR_CONT_UNWIND can reclaim */
        w_block_i32(s);                         /* $merge (result i32): the dst */
        w_block_remcont(s);                     /* $on_shift (result (ref $ct1)) */
        w_const_i32(s, 0);                      /* the thunk's hidden closure self */
        w_const_i32(s, 0);                      /* the thunk's dummy initial arg */
        w_ref_func(s, i->sym);
        w_cont_new(s, g_cont2_tidx);
        w_resume(s, g_cont2_tidx, 1, 0);         /* on $shift -> $on_shift (depth 0) */
        /* normal completion: the body's i32 value is on the stack */
        w_local_set(s, g_cont_vtmp_local);
        push_slot_addr(s, i->slot);
        w_local_get(s, g_cont_vtmp_local);
        w_mem(s, WI_I32_STORE, 0);               /* slot = body value */
        w_const_i32(s, 0);                       /* dst = 0 (normal) */
        w_br(s, 1);                              /* br $merge */
        w_end(s);                                /* end $on_shift */
        /* shift path: the remainder continuation (ref $ct) is on the stack */
        w_local_set(s, g_cont_kref_local);
        w_global_get(s, WASM_CONTS_TOP_GLOBAL);  /* idx: the dst (nonzero) */
        w_global_get(s, WASM_CONTS_TOP_GLOBAL);  /* idx: the table slot */
        w_local_get(s, g_cont_kref_local);
        w_table_set(s, WASM_CONTS_TABLE);        /* conts[idx] = continuation */
        w_global_get(s, WASM_CONTS_TOP_GLOBAL);
        w_const_i32(s, 1);
        w_op(s, WI_I32_ADD);
        w_global_set(s, WASM_CONTS_TOP_GLOBAL);  /* conts_top++ */
        w_end(s);                                /* end $merge: i32 (dst) on stack */
        set_temp(s, i->dst);
        break;
    }
    case IR_CAPTURE:                             /* (shift): suspend to the handler */
        w_suspend(s);                            /* -> the resumed-with value */
        set_temp(s, i->dst);
        break;
    case IR_RESUME:                              /* (resume k val): resume conts[k] */
        push_temp(s, i->b);                      /* val -> the shift's result */
        push_temp(s, i->a);                      /* k   -> the conts index */
        w_table_get(s, WASM_CONTS_TABLE);        /* the remainder continuation */
        w_resume(s, g_cont1_tidx, 0, 0);         /* no handler: a re-shift traps */
        set_temp(s, i->dst);
        break;
    case IR_CONT_UNWIND:
        /* The reset extent is closing: every continuation captured within it is
           dead (resumed to completion or dropped).  Restore $conts_top to the
           mark-time value IR_MARK saved, reclaiming those table slots.  This
           gives the table stack discipline, so a program that resets in a loop
           (scm_reset_heap, scm_reset_reclaim) does not grow it without bound. */
        w_local_get(s, g_cont_savedtop_local);
        w_global_set(s, WASM_CONTS_TOP_GLOBAL);
        break;

    default:
        unsupported("opcode");
        break;
    }
}

/****************************************************************
 * Control-flow structurer (the relooper)
 ****************************************************************/

struct wblock {
    struct ir_insn *first;      /* first instruction of the block */
    struct ir_insn *sl_end;     /* straight-line code is [first, sl_end) */
    struct ir_insn *term;       /* terminator instruction, or NULL (fall through) */
    int label;                  /* label id if the block opens with IR_LABEL, else -1 */
    int succ[2];
    int nsucc;
    int rpo;                    /* reverse-postorder index, -1 if unreachable */
    int idom;                   /* immediate dominator block, -1 if undefined */
    int is_loop_header;
    int is_merge;
    int fwdpred;
};

static struct wblock *blk;
static int             nblk;
static int            *rpo_order;       /* [nblk] block ids in RPO sequence */
static int             n_rpo;

/* context stack for the structured emission.  CF_IF is a placeholder for an
   if/else construct: nothing branches to it, but it occupies a br-depth slot so
   a br emitted inside an arm counts it. */
#define CF_LOOP  1
#define CF_BLOCK 2
#define CF_IF    3
static int *ctx_kind;
static int *ctx_block;
static int  ctxn;

static int
is_term_op(int op)
{
    switch (op) {
    case IR_JMP: case IR_BZ: case IR_BNZ:
    case IR_RET: case IR_RETV: case IR_RETV64: case IR_FRETV:
    case IR_TAILCALL: case IR_TAILCALLI: case IR_IJMP: case IR_IJMP64:
    case IR_RETV2: case IR_RETV_AGG: case IR_TRAP:
        return 1;
    default:
        return 0;
    }
}

/* Partition the instruction stream into basic blocks (layout order).  A leader
   begins a block: the first instruction, any IR_LABEL, and the instruction
   after a terminator.  Since every label is a leader and every terminator is
   followed by a leader, a block's terminator is always its last instruction. */
static void
build_blocks(struct ir_func *fn)
{
    struct ir_insn *in;
    int count = 0;
    int bi, *lbl2blk;

    /* count leaders */
    {
        struct ir_insn *prev = NULL;
        for (in = fn->head; in; in = in->next) {
            int leader = (prev == NULL) || (in->op == IR_LABEL) ||
                         is_term_op(prev->op);
            if (leader)
                count++;
            prev = in;
        }
    }
    nblk = count;
    blk = arena_zalloc(fn->arena, (size_t)(nblk > 0 ? nblk : 1) * sizeof(*blk));

    /* fill blocks */
    bi = -1;
    {
        struct ir_insn *prev = NULL;
        for (in = fn->head; in; in = in->next) {
            int leader = (prev == NULL) || (in->op == IR_LABEL) ||
                         (prev && is_term_op(prev->op));
            if (leader) {
                bi++;
                blk[bi].first = in;
                blk[bi].label = (in->op == IR_LABEL) ? in->label : -1;
                blk[bi].term = NULL;
                blk[bi].sl_end = NULL;
                blk[bi].idom = -1;
                blk[bi].rpo = -1;
            }
            if (is_term_op(in->op))
                blk[bi].term = in;
            prev = in;
        }
    }

    /* straight-line end of each block: the terminator if present, else the next
       block's first instruction (fall through) */
    for (bi = 0; bi < nblk; bi++) {
        if (blk[bi].term)
            blk[bi].sl_end = blk[bi].term;
        else
            blk[bi].sl_end = (bi + 1 < nblk) ? blk[bi + 1].first : NULL;
    }

    /* label id -> block index */
    lbl2blk = arena_alloc(fn->arena,
                          (size_t)(fn->nlabels > 0 ? fn->nlabels : 1) * sizeof(int));
    for (bi = 0; bi < fn->nlabels; bi++)
        lbl2blk[bi] = -1;
    for (bi = 0; bi < nblk; bi++)
        if (blk[bi].label >= 0 && blk[bi].label < fn->nlabels)
            lbl2blk[blk[bi].label] = bi;

    /* successor edges */
    for (bi = 0; bi < nblk; bi++) {
        struct ir_insn *t = blk[bi].term;
        blk[bi].nsucc = 0;
        if (!t) {
            if (bi + 1 < nblk) {
                blk[bi].succ[0] = bi + 1;
                blk[bi].nsucc = 1;
            }
            continue;
        }
        switch (t->op) {
        case IR_JMP:
            blk[bi].succ[0] = lbl2blk[t->label];
            blk[bi].nsucc = 1;
            break;
        case IR_BZ:
        case IR_BNZ:
            blk[bi].succ[0] = lbl2blk[t->label];        /* taken */
            blk[bi].succ[1] = (bi + 1 < nblk) ? bi + 1 : -1;  /* fall through */
            blk[bi].nsucc = 2;
            break;
        case IR_RET:
        case IR_RETV:
            blk[bi].nsucc = 0;
            break;
        default:
            /* a terminator from a later phase; leave it to the emitter to stop */
            blk[bi].nsucc = 0;
            break;
        }
    }
}

/* reverse postorder from the entry (block 0) */
static void
compute_rpo(struct ir_func *fn)
{
    int *stk, *ci, sp = 0;
    int *post, npost = 0;
    char *vis;
    int b, k;

    stk = arena_alloc(fn->arena, (size_t)nblk * sizeof(int));
    ci = arena_zalloc(fn->arena, (size_t)nblk * sizeof(int));
    post = arena_alloc(fn->arena, (size_t)nblk * sizeof(int));
    vis = arena_zalloc(fn->arena, (size_t)nblk);
    rpo_order = arena_alloc(fn->arena, (size_t)nblk * sizeof(int));

    if (nblk == 0)
        return;
    stk[sp++] = 0;
    vis[0] = 1;
    while (sp > 0) {
        b = stk[sp - 1];
        if (ci[b] < blk[b].nsucc) {
            int c = blk[b].succ[ci[b]++];
            if (c >= 0 && !vis[c]) {
                vis[c] = 1;
                stk[sp++] = c;
            }
        } else {
            post[npost++] = b;
            sp--;
        }
    }
    n_rpo = npost;
    for (k = 0; k < npost; k++) {
        int blkid = post[npost - 1 - k];
        rpo_order[k] = blkid;
        blk[blkid].rpo = k;
    }
}

/* immediate dominators, Cooper/Harvey/Kennedy over the RPO */
static int
dom_intersect(int a, int b)
{
    while (a != b) {
        while (blk[a].rpo > blk[b].rpo)
            a = blk[a].idom;
        while (blk[b].rpo > blk[a].rpo)
            b = blk[b].idom;
    }
    return a;
}

static void
compute_dominators(struct ir_func *fn)
{
    int **preds, *npred;
    int b, k, changed;

    npred = arena_zalloc(fn->arena, (size_t)nblk * sizeof(int));
    for (b = 0; b < nblk; b++) {
        int s;
        for (s = 0; s < blk[b].nsucc; s++)
            if (blk[b].succ[s] >= 0)
                npred[blk[b].succ[s]]++;
    }
    preds = arena_alloc(fn->arena, (size_t)nblk * sizeof(int *));
    for (b = 0; b < nblk; b++) {
        preds[b] = npred[b] ? arena_alloc(fn->arena, (size_t)npred[b] * sizeof(int)) : NULL;
        npred[b] = 0;
    }
    for (b = 0; b < nblk; b++) {
        int s;
        for (s = 0; s < blk[b].nsucc; s++) {
            int d = blk[b].succ[s];
            if (d >= 0)
                preds[d][npred[d]++] = b;
        }
    }

    for (b = 0; b < nblk; b++)
        blk[b].idom = -1;
    if (nblk > 0)
        blk[0].idom = 0;        /* entry dominates itself */

    do {
        changed = 0;
        for (k = 0; k < n_rpo; k++) {
            int ni = -1, p;
            b = rpo_order[k];
            if (b == 0)
                continue;
            for (p = 0; p < npred[b]; p++) {
                int pr = preds[b][p];
                if (blk[pr].idom != -1)
                    ni = (ni == -1) ? pr : dom_intersect(pr, ni);
            }
            if (ni != -1 && blk[b].idom != ni) {
                blk[b].idom = ni;
                changed = 1;
            }
        }
    } while (changed);
}

/* a dominates b (reflexive) */
static int
dominates(int a, int b)
{
    int x = b;
    if (b < 0 || blk[b].rpo < 0)
        return 0;
    for (;;) {
        if (x == a)
            return 1;
        if (x == blk[x].idom)   /* reached the entry's self-loop */
            return 0;
        x = blk[x].idom;
    }
}

/* classify loop headers (back-edge targets) and merge nodes (>= 2 forward
   predecessors) */
static void
classify_blocks(void)
{
    int b, s;
    for (b = 0; b < nblk; b++) {
        blk[b].is_loop_header = 0;
        blk[b].is_merge = 0;
        blk[b].fwdpred = 0;
    }
    for (b = 0; b < nblk; b++) {
        if (blk[b].rpo < 0)
            continue;
        for (s = 0; s < blk[b].nsucc; s++) {
            int d = blk[b].succ[s];
            if (d < 0)
                continue;
            if (dominates(d, b))        /* back edge b -> d */
                blk[d].is_loop_header = 1;
            else
                blk[d].fwdpred++;
        }
    }
    for (b = 0; b < nblk; b++)
        blk[b].is_merge = (blk[b].fwdpred >= 2);
}

/* br depth for the enclosing construct of `blkid` of the given kind */
static int
br_depth(int kind, int blkid)
{
    int i;
    for (i = ctxn - 1; i >= 0; i--)
        if (ctx_kind[i] == kind && ctx_block[i] == blkid)
            return ctxn - 1 - i;
    die("wasm: no enclosing %s for block %d",
        kind == CF_LOOP ? "loop" : "block", blkid);
    return 0;
}

static void do_tree(struct wsink *s, struct ir_func *fn, int x);

/* Push a branch condition as an i32 boolean (what WASM's `if` consumes).  C
   allows a branch on any scalar, so the condition temp may be i64 or float; an
   i32 is already a usable truth value, a wider type is reduced with a != 0 that
   yields i32 (NaN compares not-equal, i.e. true, as C requires). */
static void
push_cond(struct wsink *s, int t)
{
    enum wtype wt = (t >= 0 && t < ntemps) ? temp_type[t] : WT_I32;

    push_temp(s, t);
    switch (wt) {
    case WT_I64:
        w_zero(s, WT_I64);
        w_op(s, WI_I64_NE);
        break;
    case WT_F32:
        w_zero(s, WT_F32);
        w_op(s, WI_F32_NE);
        break;
    case WT_F64:
        w_zero(s, WT_F64);
        w_op(s, WI_F64_NE);
        break;
    default:                                    /* WT_I32: already a boolean */
        break;
    }
}

/* translate a control transfer from `from` to `target` */
static void
do_branch(struct wsink *s, struct ir_func *fn, int from, int target)
{
    if (target < 0)
        return;
    if (dominates(target, from))                /* back edge: continue a loop */
        w_br(s, br_depth(CF_LOOP, target));
    else if (blk[target].is_merge)              /* forward to a merge: break out */
        w_br(s, br_depth(CF_BLOCK, target));
    else                                        /* dominated: emit inline */
        do_tree(s, fn, target);
}

/* emit a block's straight-line body (no terminator) */
static void
emit_straightline(struct wsink *s, struct ir_func *fn, int b)
{
    struct ir_insn *in;
    for (in = blk[b].first; in && in != blk[b].sl_end; in = in->next)
        emit_insn(s, fn, in);
}

/* emit the frame restore at a return site */
static void
emit_sp_restore(struct wsink *s)
{
    if (frame_size) {
        w_sp_get(s);
        w_const_i32(s, frame_size);
        w_op(s, WI_I32_ADD);
        w_sp_set(s);
    }
}

/* a tail call: run the call and return its result as ours.  Not a WASM
   return_call (kept simple and portable): restore the frame, push args, call,
   return. */
static void
emit_tailcall(struct wsink *s, struct ir_insn *t)
{
    int k;

    if (!lookup_sig(t->sym) && !lookup_ext(t->sym))
        die("wasm: tail call to unknown symbol %s", t->sym ? t->sym : "(null)");
    emit_sp_restore(s);
    for (k = 0; k < narg; k++)
        push_temp(s, arg_temps[k]);
    w_call(s, t->sym);
    w_op(s, WI_RETURN);
    narg = 0;
}

/* An indirect tail call (IR_TAILCALLI): the indirect counterpart of
   emit_tailcall.  There is no indirect tail-call instruction in the baseline
   ISA, so it lowers the same way, a call_indirect whose result (if any) is
   returned straight out of this function.  The call-type index mirrors the one
   collect_calls registered: the parameter signature from the pending args and
   the result arity from this function's own return (compute_result), so the
   type is already interned. */
static void
emit_tailcalli(struct wsink *s, struct ir_func *fn, struct ir_insn *t)
{
    struct callsig sig;
    int k, ti;

    pending_call_sig(&sig, t->call_variadic);
    sig.has_result = compute_result(fn, &sig.result);
    ti = add_itype(&sig);
    emit_sp_restore(s);
    for (k = 0; k < narg; k++)
        push_temp(s, arg_temps[k]);
    push_temp(s, t->a);                 /* the function pointer: a table slot */
    w_call_indirect(s, ti);
    w_op(s, WI_RETURN);
    narg = 0;
}

/* emit a block's straight-line body and then its terminator */
static void
emit_block_body(struct wsink *s, struct ir_func *fn, int b)
{
    struct ir_insn *t = blk[b].term;

    emit_straightline(s, fn, b);

    if (!t) {                                   /* fall through to the next block */
        do_branch(s, fn, b, blk[b].nsucc ? blk[b].succ[0] : -1);
        return;
    }
    switch (t->op) {
    case IR_JMP:
        do_branch(s, fn, b, blk[b].succ[0]);
        break;
    case IR_BZ:
    case IR_BNZ: {
        int taken = blk[b].succ[0];
        int fall = blk[b].succ[1];
        int then_t = (t->op == IR_BNZ) ? taken : fall;
        int else_t = (t->op == IR_BNZ) ? fall : taken;
        push_cond(s, t->a);
        w_if(s);
        ctx_kind[ctxn] = CF_IF;
        ctx_block[ctxn] = -1;
        ctxn++;
        do_branch(s, fn, b, then_t);
        w_else(s);
        do_branch(s, fn, b, else_t);
        ctxn--;
        w_end(s);
        break;
    }
    case IR_RET:
        emit_sp_restore(s);
        if (g_has_result)       /* a bare return in a value function: yield zero */
            w_zero(s, g_result);
        w_op(s, WI_RETURN);
        break;
    case IR_RETV:
    case IR_RETV64:
    case IR_FRETV:
        emit_sp_restore(s);
        push_temp(s, t->a);
        w_op(s, WI_RETURN);
        break;
    case IR_TAILCALL:
        emit_tailcall(s, t);
        break;
    case IR_TAILCALLI:
        emit_tailcalli(s, fn, t);
        break;
    case IR_TRAP:
        w_op(s, WI_UNREACHABLE);
        break;
    default:
        unsupported("terminator");
        break;
    }
}

/****************************************************************
 * Irreducible fallback: a br_table dispatch loop
 *
 * When the CFG is irreducible (a loop entered at two points, which only C goto
 * can produce), the dominator-tree relooper cannot nest the blocks.  The whole
 * function is then emitted as one dispatch loop: a dispatch local holds the id
 * of the block to run next, a br_table at the top jumps to that block's code,
 * and each block ends by setting the dispatch local to its successor and
 * branching back to re-dispatch (or by returning).  Always correct, structure
 * ignored; used only on the rare irreducible function.
 ****************************************************************/

static int disp_local;      /* the dispatch local's WASM index */

/* true if any edge is retreating in RPO but not a dominating back edge */
static int
is_irreducible(void)
{
    int b, s;
    for (b = 0; b < nblk; b++) {
        if (blk[b].rpo < 0)
            continue;
        for (s = 0; s < blk[b].nsucc; s++) {
            int d = blk[b].succ[s];
            if (d < 0 || blk[d].rpo < 0)
                continue;
            if (blk[d].rpo <= blk[b].rpo && !dominates(d, b))
                return 1;
        }
    }
    return 0;
}

/* set the dispatch local to `target` and branch back to the loop at `loopdepth` */
static void
dispatch_to(struct wsink *s, int target, int loopdepth)
{
    if (target < 0) {                   /* no successor: fall off (treated as trap) */
        w_op(s, WI_UNREACHABLE);
        return;
    }
    w_const_i32(s, target);
    w_local_set(s, disp_local);
    w_br(s, loopdepth);
}

/* emit block b's code inside the dispatch loop; `loopdepth` is the br depth of
   the enclosing loop at this point in the nesting */
static void
emit_dispatch_block(struct wsink *s, struct ir_func *fn, int b, int loopdepth)
{
    struct ir_insn *t = blk[b].term;

    emit_straightline(s, fn, b);

    if (!t) {
        dispatch_to(s, blk[b].nsucc ? blk[b].succ[0] : -1, loopdepth);
        return;
    }
    switch (t->op) {
    case IR_JMP:
        dispatch_to(s, blk[b].succ[0], loopdepth);
        break;
    case IR_BZ:
    case IR_BNZ: {
        int taken = blk[b].succ[0];
        int fall = blk[b].succ[1];
        int then_t = (t->op == IR_BNZ) ? taken : fall;
        int else_t = (t->op == IR_BNZ) ? fall : taken;
        push_cond(s, t->a);
        w_if(s);
        dispatch_to(s, then_t, loopdepth + 1);   /* +1: inside the if frame */
        w_else(s);
        dispatch_to(s, else_t, loopdepth + 1);
        w_end(s);
        break;
    }
    case IR_RET:
        emit_sp_restore(s);
        if (g_has_result)       /* a bare return in a value function: yield zero */
            w_zero(s, g_result);
        w_op(s, WI_RETURN);
        break;
    case IR_RETV:
    case IR_RETV64:
    case IR_FRETV:
        emit_sp_restore(s);
        push_temp(s, t->a);
        w_op(s, WI_RETURN);
        break;
    case IR_TAILCALL:
        emit_tailcall(s, t);
        break;
    case IR_TAILCALLI:
        emit_tailcalli(s, fn, t);
        break;
    case IR_TRAP:
        w_op(s, WI_UNREACHABLE);
        break;
    default:
        unsupported("terminator");
        break;
    }
}

/* the whole function as one br_table dispatch loop over the nblk blocks */
static void
emit_dispatch_loop(struct wsink *s, struct ir_func *fn)
{
    int id, *tbl;

    /* enter at block 0 */
    w_const_i32(s, 0);
    w_local_set(s, disp_local);
    w_loop(s);

    /* nblk blocks plus a default, br_table dispatching on the dispatch local.
       br_table label for block v is depth (nblk - v); the default is depth 0. */
    for (id = 0; id < nblk; id++)
        w_block(s);
    w_block(s);                                 /* the default block */
    w_local_get(s, disp_local);
    tbl = arena_alloc(fn->arena, (size_t)(nblk > 0 ? nblk : 1) * sizeof(int));
    for (id = 0; id < nblk; id++)
        tbl[id] = nblk - id;
    w_br_table(s, tbl, nblk, 0);                /* default depth 0 */
    w_end(s);                                   /* end default block */
    w_op(s, WI_UNREACHABLE);

    /* close block v (from nblk-1 down to 0) and emit its code; at that point the
       enclosing loop is at depth v (v inner blocks still open, then the loop) */
    for (id = nblk - 1; id >= 0; id--) {
        w_end(s);
        emit_dispatch_block(s, fn, id, id);
    }

    w_end(s);                                   /* end loop */
}

/* the dominator-tree children of X that are merge nodes, sorted by descending
   RPO (the highest-RPO merge child wraps the outermost block) */
static int
merge_children(int x, int *out_ids)
{
    int b, n = 0, i, j;
    for (b = 0; b < nblk; b++)
        if (blk[b].rpo >= 0 && blk[b].idom == x && b != x && blk[b].is_merge)
            out_ids[n++] = b;
    /* insertion sort, descending RPO */
    for (i = 1; i < n; i++) {
        int v = out_ids[i];
        for (j = i - 1; j >= 0 && blk[out_ids[j]].rpo < blk[v].rpo; j--)
            out_ids[j + 1] = out_ids[j];
        out_ids[j + 1] = v;
    }
    return n;
}

/* emit X's own code wrapped in a block for each remaining merge child */
static void
node_within(struct wsink *s, struct ir_func *fn, int x, int *ys, int n)
{
    if (n == 0) {
        emit_block_body(s, fn, x);
        return;
    }
    {
        int y = ys[0];
        w_block(s);
        ctx_kind[ctxn] = CF_BLOCK;
        ctx_block[ctxn] = y;
        ctxn++;
        node_within(s, fn, x, ys + 1, n - 1);
        ctxn--;
        w_end(s);
        do_tree(s, fn, y);
    }
}

static void
do_tree(struct wsink *s, struct ir_func *fn, int x)
{
    int *ys = arena_alloc(fn->arena, (size_t)(nblk > 0 ? nblk : 1) * sizeof(int));
    int n = merge_children(x, ys);

    if (blk[x].is_loop_header) {
        w_loop(s);
        ctx_kind[ctxn] = CF_LOOP;
        ctx_block[ctxn] = x;
        ctxn++;
        node_within(s, fn, x, ys, n);
        ctxn--;
        w_end(s);
    } else {
        node_within(s, fn, x, ys, n);
    }
}

/****************************************************************
 * Function and module
 ****************************************************************/

static enum wtype
param_wtype(struct ir_func *fn, int p)
{
    int sz = fn->slot_size ? fn->slot_size[p] : 4;
    if (fn->param_class && fn->param_class[p] == 1)   /* float */
        return (fn->param_fw && fn->param_fw[p]) ? WT_F32 : WT_F64;
    return sz == 8 ? WT_I64 : WT_I32;
}

/* result type: void unless a value-return op is present */
static int
func_result(struct ir_func *fn, enum wtype *out)
{
    struct ir_insn *i;
    for (i = fn->head; i; i = i->next) {
        switch (i->op) {
        case IR_RETV:   *out = WT_I32; return 1;
        case IR_RETV64: *out = WT_I64; return 1;
        case IR_FRETV:
            *out = (i->a >= 0 && i->a < ntemps) ? temp_type[i->a] : WT_F64;
            return 1;
        case IR_TAILCALL:
        case IR_TAILCALLI:
            /* Same reasoning as compute_result: a function whose only exit is a
               tail call returns the callee's result, the uniform i32 word of the
               Scheme front end (the sole tail-call producer).  This declared
               type must match the call_indirect emit_tailcalli builds from
               compute_result, and the result every direct caller expects. */
            *out = WT_I32;
            return 1;
        default:
            break;
        }
    }
    return 0;
}

/* Build all per-function working state (temps, frame, CFG, dominators, the ctx
   stack, g_has_result/g_result, and disp_local for an irreducible CFG).  It must
   run before emit_body or before reading the per-function globals.  Returns
   whether the CFG is irreducible. */
static int
prepare_function(struct ir_func *fn)
{
    int irreducible;

    nparams = fn->nparams;
    g_cur_variadic = fn->is_variadic ? 1 : 0;
    g_nwparams = nparams + g_cur_variadic;
    ntemps = fn->ntemps;
    narg = 0;
    classify_temps(fn);
    g_use = arena_alloc(fn->arena, (size_t)(ntemps > 0 ? ntemps : 1) * sizeof(int));
    count_uses(fn, g_use);
    layout_frame(fn);

    build_blocks(fn);
    compute_rpo(fn);
    compute_dominators(fn);
    classify_blocks();
    irreducible = is_irreducible();

    ctx_kind = arena_alloc(fn->arena, (size_t)(2 * nblk + 16) * sizeof(int));
    ctx_block = arena_alloc(fn->arena, (size_t)(2 * nblk + 16) * sizeof(int));
    ctxn = 0;

    g_has_result = func_result(fn, &g_result);
    if (irreducible)
        disp_local = g_nwparams + ntemps;       /* one more local for dispatch */

    /* A function containing IR_MARK (a reset) needs three scratch locals past
       the temps (and the dispatch local): a (ref null $ct) for the captured
       continuation, an i32 to reorder the body-value store, and an i32 holding
       the mark-time $conts_top so IR_CONT_UNWIND can reclaim the table. */
    g_fn_has_mark = 0;
    {
        struct ir_insn *in;
        for (in = fn->head; in; in = in->next)
            if (in->op == IR_MARK) {
                g_fn_has_mark = 1;
                break;
            }
    }
    if (g_fn_has_mark) {
        int base = g_nwparams + ntemps + (irreducible ? 1 : 0);
        g_cont_kref_local = base;
        g_cont_vtmp_local = base + 1;
        g_cont_savedtop_local = base + 2;
    }
    return irreducible;
}

/* Emit a function body through the sink: the frame prologue, the structured
   body (or the irreducible dispatch loop), and the trailing fall-off handling.
   prepare_function(fn) must have run.  The locals declaration and the function
   framing around this are format-specific and emitted by the caller. */
static void
emit_body(struct wsink *s, struct ir_func *fn, int irreducible)
{
    int p;

    /* prologue: open the frame and spill the incoming params into their slots */
    if (frame_size > 0) {
        w_sp_get(s);
        w_const_i32(s, frame_size);
        w_op(s, WI_I32_SUB);
        w_sp_set(s);
        for (p = 0; p < nparams && p < fn->nslots; p++) {
            enum wtype pt = param_wtype(fn, p);
            push_slot_addr(s, p);
            w_local_get(s, p);
            w_mem(s, pt == WT_I64 ? WI_I64_STORE :
                     pt == WT_F32 ? WI_F32_STORE :
                     pt == WT_F64 ? WI_F64_STORE : WI_I32_STORE, 0);
        }
    }

    if (nblk > 0) {
        if (irreducible)
            emit_dispatch_loop(s, fn);
        else
            do_tree(s, fn, 0);
    }

    /* a value-returning function that reaches the end has fallen off a path C
       leaves undefined; keep the validator happy.  A void function restores the
       frame on a fall-through. */
    if (g_has_result)
        w_op(s, WI_UNREACHABLE);
    else if (frame_size > 0)
        emit_sp_restore(s);
}

static void
emit_function(FILE *out, struct ir_func *fn)
{
    struct wsink sink;
    int p, t, irreducible;

    sink.kind = WS_TEXT;
    sink.out = out;
    sink.code = NULL;

    irreducible = prepare_function(fn);

    fprintf(out, "  (func $%s", fn->name);
    for (p = 0; p < nparams; p++)
        fprintf(out, " (param %s)", wtype_name(param_wtype(fn, p)));
    if (g_cur_variadic)
        fprintf(out, " (param i32)");   /* the hidden varargs buffer pointer */
    if (g_has_result)
        fprintf(out, " (result %s)", wtype_name(g_result));
    fprintf(out, "\n");

    for (t = 0; t < ntemps; t++)
        fprintf(out, "    (local %s)\n", wtype_name(temp_type[t]));
    if (irreducible)
        fprintf(out, "    (local i32)\n");

    emit_body(&sink, fn, irreducible);

    fprintf(out, "  )\n");
    if (!fn->is_local)
        fprintf(out, "  (export \"%s\" (func $%s))\n", fn->name, fn->name);
}

/* the WASI imports the synthesized runtime calls (preview1) */
static void
emit_wasi_imports(FILE *out)
{
    if (!g_has_main)
        return;
    fprintf(out, "  (import \"wasi_snapshot_preview1\" \"fd_write\""
                 " (func $__wasi_fd_write (param i32 i32 i32 i32) (result i32)))\n");
    fprintf(out, "  (import \"wasi_snapshot_preview1\" \"fd_read\""
                 " (func $__wasi_fd_read (param i32 i32 i32 i32) (result i32)))\n");
    fprintf(out, "  (import \"wasi_snapshot_preview1\" \"proc_exit\""
                 " (func $__wasi_proc_exit (param i32)))\n");
}

/* The synthesized runtime's bodies, emitted through the sink so the WAT text
   and the binary module share one source.  A single iovec plus a result word
   live at fixed low scratch addresses (iov.base=8, iov.len=12, result=16, below
   the data region), used only transiently inside these functions.

   write(fd,buf,n) / read(fd,buf,n): one-iovec fd_write / fd_read.  write returns
   n; read returns the result word (the bytes read). */
static void
emit_seam_rw(struct wsink *s, int is_read)
{
    w_const_i32(s, 8);  w_local_get(s, 1); w_mem(s, WI_I32_STORE, 0);
    w_const_i32(s, 12); w_local_get(s, 2); w_mem(s, WI_I32_STORE, 0);
    w_local_get(s, 0);                  /* fd */
    w_const_i32(s, 8);                  /* iovs */
    w_const_i32(s, 1);                  /* iovs_len */
    w_const_i32(s, 16);                 /* result pointer */
    w_call(s, is_read ? "__wasi_fd_read" : "__wasi_fd_write");
    w_op(s, WI_DROP);
    if (is_read) {
        w_const_i32(s, 16);
        w_mem(s, WI_I32_LOAD, 0);
    } else {
        w_local_get(s, 2);
    }
}

/* _exit(code) / exit(code): proc_exit, then an unreachable to satisfy the
   validator (proc_exit does not return) */
static void
emit_seam_exit(struct wsink *s)
{
    w_local_get(s, 0);
    w_call(s, "__wasi_proc_exit");
    w_op(s, WI_UNREACHABLE);
}

/* _start: call main (argc=argv=0 when it takes them) and exit with its result
   (0 when main returns void) */
static void
emit_start_body(struct wsink *s)
{
    struct fnsig *mainsig = lookup_sig("main");
    if (g_main_nparams >= 2) {
        w_const_i32(s, 0);
        w_const_i32(s, 0);
    }
    w_call(s, "main");
    if (!mainsig || !mainsig->has_result)
        w_const_i32(s, 0);
    w_call(s, "__wasi_proc_exit");
}

/* the synthesized runtime as WAT text: the function framing in text, the bodies
   through the shared sink emitters above */
static void
emit_wasi_runtime(FILE *out)
{
    struct wsink sink;

    if (!g_has_main)
        return;
    sink.kind = WS_TEXT;
    sink.out = out;
    sink.code = NULL;

    if (g_seam_syn[0]) {
        fprintf(out, "  (func $write (param i32 i32 i32) (result i32)\n");
        emit_seam_rw(&sink, 0);
        fprintf(out, "  )\n");
    }
    if (g_seam_syn[1]) {
        fprintf(out, "  (func $read (param i32 i32 i32) (result i32)\n");
        emit_seam_rw(&sink, 1);
        fprintf(out, "  )\n");
    }
    if (g_seam_syn[2]) {
        fprintf(out, "  (func $_exit (param i32)\n");
        emit_seam_exit(&sink);
        fprintf(out, "  )\n");
    }
    if (g_seam_syn[3]) {
        fprintf(out, "  (func $exit (param i32)\n");
        emit_seam_exit(&sink);
        fprintf(out, "  )\n");
    }

    fprintf(out, "  (func $_start\n");
    emit_start_body(&sink);
    fprintf(out, "  )\n");
    fprintf(out, "  (export \"_start\" (func $_start))\n");
}

static void emit_module_text(FILE *out, struct ir_program *prog);
static void emit_module_binary(FILE *out, struct ir_program *prog);
static void emit_module_object(FILE *out, struct ir_program *prog);

/* output format values for wasm_output_binary */
#define WASM_FMT_TEXT   0
#define WASM_FMT_MODULE 1
#define WASM_FMT_OBJECT 2

void
regalloc(struct ir_func *fn)
{
    /* WASM has unlimited typed locals: no allocation, no spilling. */
    (void)fn;
}

/* Pick the output format from the -o path's extension: a .wasm file is a
   complete binary module, a .o file a relocatable object, anything else (or
   stdout) WAT text.  The front-end drivers call this in place of poking
   wasm_output_binary directly, so the extension rule is not copied per driver. */
void
wasm_set_output_format(const char *outpath)
{
    const char *dot = outpath ? strrchr(outpath, '.') : NULL;

    if (dot && strcmp(dot, ".wasm") == 0)
        wasm_output_binary = WASM_FMT_MODULE;
    else if (dot && strcmp(dot, ".o") == 0)
        wasm_output_binary = WASM_FMT_OBJECT;
    else
        wasm_output_binary = WASM_FMT_TEXT;
}

void
target_emit(FILE *out, struct ir_program *prog)
{
    g_synth_runtime = (wasm_output_binary != WASM_FMT_OBJECT);
    collect_calls(prog);
    assign_globals(prog);
    if (wasm_output_binary == WASM_FMT_OBJECT)
        emit_module_object(out, prog);
    else if (wasm_output_binary == WASM_FMT_MODULE)
        emit_module_binary(out, prog);
    else
        emit_module_text(out, prog);
}

/* emit the whole program as WAT text (assembled by wat2wasm) */
static void
emit_module_text(FILE *out, struct ir_program *prog)
{
    struct ir_func *fn;

    fprintf(out, "(module\n");

    /* function types for the indirect calls */
    {
        int k, p;
        for (k = 0; k < g_nitypes; k++) {
            fprintf(out, "  (type $wt%d (func", k);
            for (p = 0; p < g_itypes[k].nparams; p++)
                fprintf(out, " (param %s)", wtype_name(g_itypes[k].params[p]));
            if (g_itypes[k].has_result)
                fprintf(out, " (result %s)", wtype_name(g_itypes[k].result));
            fprintf(out, "))\n");
        }
    }

    /* imported (external) functions */
    {
        int k, p;
        for (k = 0; k < g_next; k++) {
            fprintf(out, "  (import \"%s\" \"%s\" (func $%s",
                    ext_import_module(g_ext[k].name),
                    ext_import_field(g_ext[k].name), g_ext[k].name);
            for (p = 0; p < g_ext[k].sig.nparams; p++)
                fprintf(out, " (param %s)", wtype_name(g_ext[k].sig.params[p]));
            if (g_ext[k].sig.has_result)
                fprintf(out, " (result %s)", wtype_name(g_ext[k].sig.result));
            fprintf(out, "))\n");
        }
    }

    emit_wasi_imports(out);

    fprintf(out, "  (memory (export \"memory\") %d)\n", g_mem_pages);
    fprintf(out, "  (global $sp (mut i32) (i32.const %d))\n", g_stack_top);

    emit_globals(out, prog);

    /* a function table so a function pointer (a table slot) can be called
       indirectly; every defined function is an element, in g_sigs order */
    if (g_nsigs > 0) {
        int k;
        fprintf(out, "  (table %d funcref)\n", g_nsigs);
        fprintf(out, "  (elem (i32.const 0)");
        for (k = 0; k < g_nsigs; k++)
            fprintf(out, " $%s", g_sigs[k].name);
        fprintf(out, ")\n");
    }

    for (fn = prog->funcs; fn; fn = fn->next)
        emit_function(out, fn);

    emit_wasi_runtime(out);

    fprintf(out, ")\n");
}

/****************************************************************
 * Binary module emission
 *
 * The same instruction sink that writes WAT also writes a complete binary
 * module: the sections are built with the wasm/enc.c encoding primitives and
 * the function bodies flow through the sink in its binary mode.  This is the
 * substrate for the relocatable object (a later phase adds the linking and
 * relocation custom sections and fixed-width index placeholders on top).
 ****************************************************************/

/* the function index of a name in the module's function index space: imported
   functions first (env imports, then the WASI imports), then the defined
   functions (the program functions and synthesized seam functions in g_sigs
   order, then _start) */
static int
module_funcidx(const char *name)
{
    int k;
    for (k = 0; k < g_next; k++)
        if (strcmp(g_ext[k].name, name) == 0)
            return k;
    if (g_has_main && g_synth_runtime) {
        if (strcmp(name, "__wasi_fd_write") == 0) return g_next + 0;
        if (strcmp(name, "__wasi_fd_read") == 0)  return g_next + 1;
        if (strcmp(name, "__wasi_proc_exit") == 0) return g_next + 2;
    }
    for (k = 0; k < g_nsigs; k++)
        if (g_sigs[k].name && strcmp(g_sigs[k].name, name) == 0)
            return g_nimports + k;
    if (strcmp(name, "_start") == 0)
        return g_nimports + g_nsigs;
    die("wasm: binary: reference to unknown function %s", name ? name : "(null)");
    return 0;
}

static int
functype_eq(const struct functype *a, const struct functype *b)
{
    int i;
    if (a->np != b->np || a->nr != b->nr)
        return 0;
    if (a->nr && a->r != b->r)
        return 0;
    for (i = 0; i < a->np; i++)
        if (a->p[i] != b->p[i])
            return 0;
    return 1;
}

static int
add_type(const struct functype *ft)
{
    int k;
    for (k = 0; k < g_ntypes; k++)
        if (functype_eq(&g_types[k], ft))
            return k;
    g_types[g_ntypes] = *ft;
    return g_ntypes++;
}

static struct functype
ft_from_sig(const struct callsig *cs)
{
    struct functype ft;
    int i;
    ft.np = cs->nparams;
    for (i = 0; i < ft.np; i++)
        ft.p[i] = cs->params[i];
    ft.nr = cs->has_result ? 1 : 0;
    ft.r = cs->result;
    return ft;
}

/* Assign a type index to every function (imports, defined, indirect sigs) and
   build the deduped type table.  Also fixes g_nimports and g_ndef. */
static void
build_module_plan(struct ir_program *prog)
{
    struct ir_func *fn;
    struct functype ft;
    int k, maxtypes;

    g_ndef = 0;
    for (fn = prog->funcs; fn; fn = fn->next)
        g_ndef++;
    g_nimports = g_next + (g_has_main && g_synth_runtime ? 3 : 0);

    /* Detect delimited-continuation use and collect the reset body thunks
       (IR_MARK carries the thunk name), so the module can declare the cont
       type, the $shift tag, the $conts table, and elem-declare the thunks. */
    g_uses_cont = 0;
    g_nthunks = 0;
    g_thunks = NULL;
    {
        struct ir_insn *in;
        int nmark = 0;
        for (fn = prog->funcs; fn; fn = fn->next)
            for (in = fn->head; in; in = in->next)
                if (in->op == IR_MARK)
                    nmark++;
                else if (in->op == IR_CAPTURE || in->op == IR_RESUME)
                    g_uses_cont = 1;
        if (nmark > 0) {
            g_uses_cont = 1;
            g_thunks = malloc((size_t)nmark * sizeof(*g_thunks));
            for (fn = prog->funcs; fn; fn = fn->next)
                for (in = fn->head; in; in = in->next)
                    if (in->op == IR_MARK && in->sym)
                        g_thunks[g_nthunks++] = in->sym;
        }
    }

    maxtypes = g_next + 3 + g_nsigs + 1 + g_nitypes + 1;
    g_types = malloc((size_t)maxtypes * sizeof(*g_types));
    g_ntypes = 0;
    g_ext_tidx = g_next ? malloc((size_t)g_next * sizeof(int)) : NULL;
    g_def_tidx = g_nsigs ? malloc((size_t)g_nsigs * sizeof(int)) : NULL;
    g_itype_typeidx = g_nitypes ? malloc((size_t)g_nitypes * sizeof(int)) : NULL;

    /* env imports, in funcidx order */
    for (k = 0; k < g_next; k++) {
        ft = ft_from_sig(&g_ext[k].sig);
        g_ext_tidx[k] = add_type(&ft);
    }
    /* WASI imports: fd_write/fd_read (i32 i32 i32 i32)->i32, proc_exit (i32)->() */
    if (g_has_main && g_synth_runtime) {
        struct functype fw, pe;
        fw.np = 4; fw.p[0] = fw.p[1] = fw.p[2] = fw.p[3] = WT_I32;
        fw.nr = 1; fw.r = WT_I32;
        g_wasi_tidx[0] = add_type(&fw);
        g_wasi_tidx[1] = add_type(&fw);
        pe.np = 1; pe.p[0] = WT_I32; pe.nr = 0; pe.r = WT_I32;
        g_wasi_tidx[2] = add_type(&pe);
    }
    /* defined program functions (g_sigs[0..g_ndef)) */
    k = 0;
    for (fn = prog->funcs; fn; fn = fn->next) {
        int i;
        ft.np = fn->nparams;
        for (i = 0; i < ft.np; i++)
            ft.p[i] = param_wtype(fn, i);
        if (fn->is_variadic && ft.np < WASM_MAX_ARGS)
            ft.p[ft.np++] = WT_I32;     /* the hidden varargs buffer pointer */
        ft.nr = g_sigs[k].has_result ? 1 : 0;
        ft.r = g_sigs[k].result;
        g_def_tidx[k] = add_type(&ft);
        k++;
    }
    /* synthesized seam functions (g_sigs[g_ndef..g_nsigs)) */
    for (k = g_ndef; k < g_nsigs; k++) {
        const char *nm = g_sigs[k].name;
        if (strcmp(nm, "write") == 0 || strcmp(nm, "read") == 0) {
            ft.np = 3; ft.p[0] = ft.p[1] = ft.p[2] = WT_I32; ft.nr = 1; ft.r = WT_I32;
        } else {        /* _exit, exit: (i32)->() */
            ft.np = 1; ft.p[0] = WT_I32; ft.nr = 0; ft.r = WT_I32;
        }
        g_def_tidx[k] = add_type(&ft);
    }
    if (g_has_main && g_synth_runtime) {
        struct functype vv;
        vv.np = 0; vv.nr = 0; vv.r = WT_I32;
        g_start_tidx = add_type(&vv);
    }
    /* indirect-call signatures */
    for (k = 0; k < g_nitypes; k++) {
        ft = ft_from_sig(&g_itypes[k]);
        g_itype_typeidx[k] = add_type(&ft);
    }

    /* continuation types: the (i32)->i32 function type a continuation runs, the
       ()->i32 type of the $shift tag, and the cont type itself (appended to the
       type section after every function type, so its index is g_ntypes). */
    if (g_uses_cont) {
        struct functype f1, f2, tft;
        f1.np = 1; f1.p[0] = WT_I32; f1.nr = 1; f1.r = WT_I32;
        g_ft1_tidx = add_type(&f1);                     /* (i32)->i32 */
        f2.np = 2; f2.p[0] = f2.p[1] = WT_I32; f2.nr = 1; f2.r = WT_I32;
        g_ft2_tidx = add_type(&f2);                     /* (i32 i32)->i32 */
        tft.np = 0; tft.nr = 1; tft.r = WT_I32;
        g_tag_tidx = add_type(&tft);                    /* ()->i32 */
        /* the two cont types are written after every function type */
        g_cont1_tidx = g_ntypes;
        g_cont2_tidx = g_ntypes + 1;
    }
}

static void
functype_write(struct wbuf *b, const struct functype *ft)
{
    int i;
    wbuf_byte(b, 0x60);
    wbuf_uleb(b, (uint64_t)ft->np);
    for (i = 0; i < ft->np; i++)
        wbuf_byte(b, wt_byte(ft->p[i]));
    wbuf_uleb(b, (uint64_t)ft->nr);
    for (i = 0; i < ft->nr; i++)
        wbuf_byte(b, wt_byte(ft->r));
}

/* the type of local index i in the function body: a temp, or the trailing i32
   dispatch local of an irreducible CFG */
static enum wtype
local_type(int i)
{
    return i < ntemps ? temp_type[i] : WT_I32;
}

/* the locals declaration of a function body: runs of equal types coalesced over
   the temps plus the extra i32 dispatch local for an irreducible CFG */
static void
emit_locals_binary(struct wbuf *b, int irreducible)
{
    int total = ntemps + (irreducible ? 1 : 0);
    int i, nentries = 0;

    for (i = 0; i < total; ) {
        enum wtype t = local_type(i);
        int j = i + 1;
        while (j < total && local_type(j) == t)
            j++;
        nentries++;
        i = j;
    }

    if (g_fn_has_mark)
        nentries += 2;          /* two groups: the (ref null $ct) and a 2-wide i32 run */

    wbuf_uleb(b, (uint64_t)nentries);
    for (i = 0; i < total; ) {
        enum wtype t = local_type(i);
        int j = i + 1;
        while (j < total && local_type(j) == t)
            j++;
        wbuf_uleb(b, (uint64_t)(j - i));
        wbuf_byte(b, wt_byte(t));
        i = j;
    }
    if (g_fn_has_mark) {
        wbuf_uleb(b, 1); w_cont_reftype(b, 1, g_cont1_tidx);  /* kref: (ref null $ct1) */
        wbuf_uleb(b, 2); wbuf_byte(b, 0x7F);                  /* vtmp, saved-top: i32 */
    }
}

/* append one function's code entry (size-prefixed locals + body) to the code
   section payload */
static void
emit_code_entry(struct wbuf *sec, struct ir_func *fn)
{
    struct wsink bs;
    struct wbuf body;
    int irr;

    irr = prepare_function(fn);
    wbuf_init(&body);
    emit_locals_binary(&body, irr);
    bs.kind = WS_BIN;
    bs.out = NULL;
    bs.code = &body;
    emit_body(&bs, fn, irr);
    wbuf_byte(&body, 0x0B);          /* end of the function body */

    wbuf_uleb(sec, (uint64_t)body.len);
    wbuf_bytes(sec, body.data, body.len);
    wbuf_free(&body);
}

/* append a synthesized function's code entry (no locals) to the code section */
static void
emit_synth_code_entry(struct wbuf *sec, void (*body_fn)(struct wsink *))
{
    struct wsink bs;
    struct wbuf body;

    wbuf_init(&body);
    wbuf_uleb(&body, 0);             /* no locals */
    bs.kind = WS_BIN;
    bs.out = NULL;
    bs.code = &body;
    body_fn(&bs);
    wbuf_byte(&body, 0x0B);          /* end of the function body */

    wbuf_uleb(sec, (uint64_t)body.len);
    wbuf_bytes(sec, body.data, body.len);
    wbuf_free(&body);
}

/* a seam function body selected by name, for emit_synth_code_entry */
static void seam_write_body(struct wsink *s) { emit_seam_rw(s, 0); }
static void seam_read_body(struct wsink *s)  { emit_seam_rw(s, 1); }

static void
emit_seam_code_entry(struct wbuf *sec, const char *name)
{
    if (strcmp(name, "write") == 0)
        emit_synth_code_entry(sec, seam_write_body);
    else if (strcmp(name, "read") == 0)
        emit_synth_code_entry(sec, seam_read_body);
    else                             /* _exit, exit */
        emit_synth_code_entry(sec, emit_seam_exit);
}

static void
emit_module_binary(FILE *out, struct ir_program *prog)
{
    struct wbuf mod, sec;
    struct ir_func *fn;
    struct ir_global *g;
    int k, ki, gi, ndefined, nexp, ndata;

    build_module_plan(prog);

    wbuf_init(&mod);
    wbuf_byte(&mod, 0x00); wbuf_byte(&mod, 0x61);
    wbuf_byte(&mod, 0x73); wbuf_byte(&mod, 0x6D);   /* \0asm */
    wbuf_u32(&mod, 1);                               /* version 1 */

    /* Type section (1).  When the program uses continuations, two extra types,
       the remainder cont (g_cont1 = cont g_ft1) and the fresh cont (g_cont2 =
       cont g_ft2), follow all the function types; their indices are g_ntypes and
       g_ntypes+1.  Each cont type is the form 0x5D plus its function type. */
    wbuf_init(&sec);
    wbuf_uleb(&sec, (uint64_t)(g_ntypes + (g_uses_cont ? 2 : 0)));
    for (k = 0; k < g_ntypes; k++)
        functype_write(&sec, &g_types[k]);
    if (g_uses_cont) {
        wbuf_byte(&sec, 0x5D); wbuf_sleb(&sec, g_ft1_tidx);
        wbuf_byte(&sec, 0x5D); wbuf_sleb(&sec, g_ft2_tidx);
    }
    wbuf_section(&mod, 1, &sec);
    wbuf_free(&sec);

    /* Import section (2) */
    if (g_nimports > 0) {
        wbuf_init(&sec);
        wbuf_uleb(&sec, (uint64_t)g_nimports);
        for (k = 0; k < g_next; k++) {
            wbuf_name(&sec, ext_import_module(g_ext[k].name));
            wbuf_name(&sec, ext_import_field(g_ext[k].name));
            wbuf_byte(&sec, 0x00);                   /* func import */
            wbuf_uleb(&sec, (uint64_t)g_ext_tidx[k]);
        }
        if (g_has_main) {
            static const char *const wn[3] = { "fd_write", "fd_read", "proc_exit" };
            for (k = 0; k < 3; k++) {
                wbuf_name(&sec, "wasi_snapshot_preview1");
                wbuf_name(&sec, wn[k]);
                wbuf_byte(&sec, 0x00);
                wbuf_uleb(&sec, (uint64_t)g_wasi_tidx[k]);
            }
        }
        wbuf_section(&mod, 2, &sec);
        wbuf_free(&sec);
    }

    /* Function section (3): a type index per defined function */
    ndefined = g_nsigs + (g_has_main ? 1 : 0);
    wbuf_init(&sec);
    wbuf_uleb(&sec, (uint64_t)ndefined);
    for (k = 0; k < g_nsigs; k++)
        wbuf_uleb(&sec, (uint64_t)g_def_tidx[k]);
    if (g_has_main)
        wbuf_uleb(&sec, (uint64_t)g_start_tidx);
    wbuf_section(&mod, 3, &sec);
    wbuf_free(&sec);

    /* Table section (4): the funcref table (0) for indirect calls, then, when
       the program uses continuations, the $conts table (1) holding captured
       continuation references that scheme carries by i32 index. */
    if (g_nsigs > 0 || g_uses_cont) {
        if (g_uses_cont && g_nsigs == 0)
            die("wasm: continuations need the funcref table (table 0)");
        wbuf_init(&sec);
        wbuf_uleb(&sec, (uint64_t)(1 + (g_uses_cont ? 1 : 0)));
        wbuf_byte(&sec, 0x70);                       /* funcref */
        wbuf_byte(&sec, 0x00);                       /* limits: min only */
        wbuf_uleb(&sec, (uint64_t)g_nsigs);
        if (g_uses_cont) {
            w_cont_reftype(&sec, 1, g_cont1_tidx);   /* (ref null $ct1) */
            wbuf_byte(&sec, 0x00);                   /* limits: min only */
            wbuf_uleb(&sec, WASM_CONTS_SIZE);
        }
        wbuf_section(&mod, 4, &sec);
        wbuf_free(&sec);
    }

    /* Memory section (5) */
    wbuf_init(&sec);
    wbuf_uleb(&sec, 1);
    wbuf_byte(&sec, 0x00);
    wbuf_uleb(&sec, (uint64_t)g_mem_pages);
    wbuf_section(&mod, 5, &sec);
    wbuf_free(&sec);

    /* Tag section (13): the one $shift control tag, ()->i32.  It is ordered
       after memory and before global. */
    if (g_uses_cont) {
        wbuf_init(&sec);
        wbuf_uleb(&sec, 1);
        wbuf_byte(&sec, 0x00);                        /* attribute: control/exception */
        wbuf_uleb(&sec, (uint64_t)g_tag_tidx);
        wbuf_section(&mod, 13, &sec);
        wbuf_free(&sec);
    }

    /* Global section (6): the shadow-stack pointer $sp (0), then, for
       continuations, $conts_top (1), the $conts bump pointer.  It starts at 1
       so a captured continuation's index is always nonzero (index 0 is the
       "body returned normally" sentinel IR_MARK yields). */
    wbuf_init(&sec);
    wbuf_uleb(&sec, (uint64_t)(1 + (g_uses_cont ? 1 : 0)));
    wbuf_byte(&sec, 0x7F);                            /* i32 */
    wbuf_byte(&sec, 0x01);                            /* mutable */
    wbuf_byte(&sec, 0x41); wbuf_sleb(&sec, g_stack_top); wbuf_byte(&sec, 0x0B);
    if (g_uses_cont) {
        wbuf_byte(&sec, 0x7F);                        /* i32 */
        wbuf_byte(&sec, 0x01);                        /* mutable */
        wbuf_byte(&sec, 0x41); wbuf_sleb(&sec, 1); wbuf_byte(&sec, 0x0B);
    }
    wbuf_section(&mod, 6, &sec);
    wbuf_free(&sec);

    /* Export section (7): memory, the non-local program functions, and _start */
    nexp = 1;                                        /* memory */
    for (fn = prog->funcs; fn; fn = fn->next)
        if (!fn->is_local)
            nexp++;
    if (g_has_main)
        nexp++;
    wbuf_init(&sec);
    wbuf_uleb(&sec, (uint64_t)nexp);
    wbuf_name(&sec, "memory");
    wbuf_byte(&sec, 0x02);                            /* memory export */
    wbuf_uleb(&sec, 0);
    ki = 0;
    for (fn = prog->funcs; fn; fn = fn->next) {
        if (!fn->is_local) {
            wbuf_name(&sec, fn->name);
            wbuf_byte(&sec, 0x00);                    /* func export */
            wbuf_uleb(&sec, (uint64_t)(g_nimports + ki));
        }
        ki++;
    }
    if (g_has_main) {
        wbuf_name(&sec, "_start");
        wbuf_byte(&sec, 0x00);
        wbuf_uleb(&sec, (uint64_t)(g_nimports + g_nsigs));
    }
    wbuf_section(&mod, 7, &sec);
    wbuf_free(&sec);

    /* Element section (9): every defined function into the funcref table, in
       g_sigs order at offset 0.  When the program uses continuations, a second,
       declarative segment lists the reset body thunks so ref.func may take their
       address (a ref.func operand must be declared). */
    if (g_nsigs > 0) {
        wbuf_init(&sec);
        wbuf_uleb(&sec, (uint64_t)(1 + (g_nthunks > 0 ? 1 : 0)));
        wbuf_byte(&sec, 0x00);                        /* active, table 0, funcref */
        wbuf_byte(&sec, 0x41); wbuf_sleb(&sec, 0); wbuf_byte(&sec, 0x0B);
        wbuf_uleb(&sec, (uint64_t)g_nsigs);
        for (k = 0; k < g_nsigs; k++)
            wbuf_uleb(&sec, (uint64_t)(g_nimports + k));
        if (g_nthunks > 0) {
            wbuf_byte(&sec, 0x03);                    /* declarative, func indices */
            wbuf_byte(&sec, 0x00);                    /* elemkind: funcref */
            wbuf_uleb(&sec, (uint64_t)g_nthunks);
            for (k = 0; k < g_nthunks; k++)
                wbuf_uleb(&sec, (uint64_t)module_funcidx(g_thunks[k]));
        }
        wbuf_section(&mod, 9, &sec);
        wbuf_free(&sec);
    }

    /* Code section (10): the program functions, then the synthesized seam
       functions (g_sigs[g_ndef..g_nsigs)) and _start, in function-index order */
    wbuf_init(&sec);
    wbuf_uleb(&sec, (uint64_t)ndefined);
    for (fn = prog->funcs; fn; fn = fn->next)
        emit_code_entry(&sec, fn);
    for (k = g_ndef; k < g_nsigs; k++)
        emit_seam_code_entry(&sec, g_sigs[k].name);
    if (g_has_main)
        emit_synth_code_entry(&sec, emit_start_body);
    wbuf_section(&mod, 10, &sec);
    wbuf_free(&sec);

    /* Data section (11): the initialized globals */
    ndata = 0;
    for (g = prog->globals; g; g = g->next)
        if (!global_is_bss(g) && global_total(g) > 0)
            ndata++;
    if (ndata > 0) {
        wbuf_init(&sec);
        wbuf_uleb(&sec, (uint64_t)ndata);
        gi = 0;
        for (g = prog->globals; g; g = g->next, gi++) {
            int total;
            unsigned char *buf;
            if (global_is_bss(g))
                continue;
            total = global_total(g);
            if (total <= 0)
                continue;
            buf = malloc((size_t)total);
            build_image(g, buf, total);
            wbuf_byte(&sec, 0x00);                    /* active, memory 0 */
            wbuf_byte(&sec, 0x41); wbuf_sleb(&sec, g_gaddr[gi].addr); wbuf_byte(&sec, 0x0B);
            wbuf_uleb(&sec, (uint64_t)total);
            wbuf_bytes(&sec, buf, (size_t)total);
            free(buf);
        }
        wbuf_section(&mod, 11, &sec);
        wbuf_free(&sec);
    }

    fwrite(mod.data, 1, mod.len, out);
    wbuf_free(&mod);
}

/****************************************************************
 * Relocatable object emission
 *
 * A standard WASM object (tool-conventions Linking.md), so the in-tree linker
 * and LLVM's wasm-ld can each read the other's objects.  Unlike a complete
 * module it has no memory, table, globals, exports, or elem of its own: it
 * imports env.__linear_memory and env.__stack_pointer, leaves every function
 * index, type index, and address as a relocation, and carries a linking symbol
 * table plus a reloc.CODE section.  The linker resolves it all.
 ****************************************************************/

/* a function's index in the object's function index space: the env imports
   first, then the program-defined functions (no synthesized runtime here) */
static int
object_funcidx(const char *name)
{
    int k;
    for (k = 0; k < g_next; k++)
        if (strcmp(g_ext[k].name, name) == 0)
            return k;
    for (k = 0; k < g_ndef; k++)
        if (g_sigs[k].name && strcmp(g_sigs[k].name, name) == 0)
            return g_next + k;
    die("wasm: object: reference to unknown function %s", name ? name : "(null)");
    return 0;
}

/* the object's symbol table */
struct wsym {
    const char *name;
    int         kind;           /* WASM_SYMTAB_FUNCTION / _GLOBAL / _DATA */
    int         flags;
    int         index;          /* function/global index, or a data segment offset */
    int         size;           /* DATA only: the symbol's byte size */
    int         emit_name;      /* 1 for a defined symbol; 0 takes the import name */
};
static struct wsym *g_syms;
static int          g_nsyms;

/* the object's data: all globals laid out in one segment at object-local
   offsets, the linker assigning the final base */
static int          *g_obj_off;      /* [nglobals] object-local offset */
static unsigned char *g_data_image;  /* the combined segment bytes */
static int           g_data_size;
static int           g_data_p2align; /* the segment's log2 alignment */
static int           g_nglobals;

/* the functions whose address is taken: each gets a slot in the indirect
   function table, slot 0 reserved (the null function pointer) */
static const char  **g_tabfuncs;
static int           g_ntabfuncs;

/* external data symbols whose address is taken (e.g. &errno): each gets an
   undefined DATA symbol so the linker resolves its memory address */
static const char  **g_extdata;
static int           g_nextdata;

/* 1-based presence of an external data symbol (0 means not recorded) */
static int
extdata_index_of(const char *name)
{
    int k;
    for (k = 0; k < g_nextdata; k++)
        if (strcmp(g_extdata[k], name) == 0)
            return k + 1;
    return 0;
}

/* the table slot of an addressed function (1-based; 0 means not addressed) */
static int
table_index_of(const char *name)
{
    int k;
    for (k = 0; k < g_ntabfuncs; k++)
        if (strcmp(g_tabfuncs[k], name) == 0)
            return k + 1;               /* slot 0 is the reserved null */
    return 0;
}

/* note a function address: if sym is a defined function not yet seen, give it a
   table slot */
static void
note_table_func(const char *sym)
{
    if (sym && global_addr(sym) < 0 && lookup_sig(sym) && table_index_of(sym) == 0)
        g_tabfuncs[g_ntabfuncs++] = sym;
}

/* collect the functions whose address is taken: in code (IR_LEA of a function)
   and in data initializers (a global whose value is a function's address) */
static void
collect_table_funcs(struct ir_program *prog)
{
    struct ir_func *fn;
    struct ir_insn *in;
    struct ir_global *g;
    int k;

    g_tabfuncs = malloc((size_t)(g_ndef + g_next + 1) * sizeof(*g_tabfuncs));
    g_ntabfuncs = 0;
    for (fn = prog->funcs; fn; fn = fn->next)
        for (in = fn->head; in; in = in->next)
            if (in->op == IR_LEA && in->sym && !in->sym_is_data &&
                global_addr(in->sym) < 0 && table_index_of(in->sym) == 0)
                g_tabfuncs[g_ntabfuncs++] = in->sym;
    for (g = prog->globals; g; g = g->next) {
        struct ir_init *it;
        for (it = g->inits; it; it = it->next)
            note_table_func(it->sym);
        if (g->init_syms)
            for (k = 0; k < g->init_count; k++)
                note_table_func(g->init_syms[k]);
    }
}

/* collect the external data symbols whose address is taken in code: an IR_LEA
   marked as a data reference (sym_is_data) to a symbol this object neither
   defines as a global nor defines as a function.  Each becomes an undefined
   DATA symbol resolved by a MEMORY_ADDR relocation at link time. */
static void
collect_extdata(struct ir_program *prog)
{
    struct ir_func *fn;
    struct ir_insn *in;
    int ncand = 0;

    for (fn = prog->funcs; fn; fn = fn->next)
        for (in = fn->head; in; in = in->next)
            if (in->op == IR_LEA && in->sym && in->sym_is_data)
                ncand++;
    g_extdata = malloc((size_t)(ncand > 0 ? ncand : 1) * sizeof(*g_extdata));
    g_nextdata = 0;
    for (fn = prog->funcs; fn; fn = fn->next)
        for (in = fn->head; in; in = in->next)
            if (in->op == IR_LEA && in->sym && in->sym_is_data &&
                global_addr(in->sym) < 0 && !lookup_sig(in->sym) &&
                extdata_index_of(in->sym) == 0)
                g_extdata[g_nextdata++] = in->sym;
}

/* collect the external functions whose address is taken but which are never
   called in this object: an IR_LEA of a function symbol this object neither
   defines (lookup_sig) nor lays out as a global, and that no call already
   registered as an external.  A called external already has an entry with its
   real signature; this adds the addressed-only ones so they get an undefined
   function symbol, an env import, and a table slot (via collect_table_funcs).
   The real signature lives with the definition in another object, so the
   placeholder import type here is the empty signature: the linker binds the
   symbol by name and never emits this object's env import. */
static void
collect_ext_funcs(struct ir_program *prog)
{
    struct ir_func *fn;
    struct ir_insn *in;

    for (fn = prog->funcs; fn; fn = fn->next)
        for (in = fn->head; in; in = in->next)
            if (in->op == IR_LEA && in->sym && !in->sym_is_data &&
                global_addr(in->sym) < 0 && !lookup_sig(in->sym) &&
                !lookup_ext(in->sym)) {
                struct callsig esig;
                esig.nparams = 0;
                esig.has_result = 0;
                add_external(prog, in->sym, &esig);
            }
}

static int
sym_index_func(const char *name)
{
    int k;
    for (k = 0; k < g_nsyms; k++)
        if (g_syms[k].kind == WASM_SYMTAB_FUNCTION && g_syms[k].name &&
            strcmp(g_syms[k].name, name) == 0)
            return k;
    die("wasm: object: no symbol for function %s", name ? name : "(null)");
    return 0;
}

static int
sym_index_data(const char *name)
{
    int k;
    for (k = 0; k < g_nsyms; k++)
        if (g_syms[k].kind == WASM_SYMTAB_DATA && g_syms[k].name &&
            strcmp(g_syms[k].name, name) == 0)
            return k;
    die("wasm: object: no symbol for data %s", name ? name : "(null)");
    return 0;
}

/* whether a global initializer references an external symbol's address (one that
   is neither a global here nor a function defined here): resolving it needs the
   symbol's data vs function nature, which this object does not know, so it is
   deferred */
static int
init_ref_is_extern(const char *sym)
{
    return sym && global_addr(sym) < 0 && !lookup_sig(sym);
}

static int
global_refs_extern(struct ir_global *g)
{
    struct ir_init *it;
    int k;
    for (it = g->inits; it; it = it->next)
        if (init_ref_is_extern(it->sym))
            return 1;
    if (g->init_syms)
        for (k = 0; k < g->init_count; k++)
            if (init_ref_is_extern(g->init_syms[k]))
                return 1;
    return 0;
}

/* lay the globals out in one object-local data segment and build its bytes */
static void
build_object_data(struct ir_program *prog)
{
    struct ir_global *g;
    int off = 0, maxal = 1, gi, a;

    g_nglobals = 0;
    for (g = prog->globals; g; g = g->next)
        g_nglobals++;
    g_obj_off = g_nglobals ? malloc((size_t)g_nglobals * sizeof(int)) : NULL;

    gi = 0;
    for (g = prog->globals; g; g = g->next, gi++) {
        int al = global_align(g);
        if (al > maxal)
            maxal = al;
        off = (off + al - 1) & ~(al - 1);
        g_obj_off[gi] = off;
        off += global_total(g);
    }
    g_data_size = off;
    g_data_p2align = 0;
    for (a = maxal; a > 1; a >>= 1)
        g_data_p2align++;

    g_data_image = g_data_size ? calloc(1, (size_t)g_data_size) : NULL;
    gi = 0;
    for (g = prog->globals; g; g = g->next, gi++) {
        int total = global_total(g);
        if (total <= 0 || global_is_bss(g))        /* bss stays zero */
            continue;
        build_image(g, g_data_image + g_obj_off[gi], total);
    }
}

/* build the symbol table: the defined program functions, the __stack_pointer
   global (when any function uses the shadow stack), and the undefined env
   function imports */
static void
build_object_symtab(struct ir_program *prog)
{
    struct ir_func *fn;
    struct ir_global *g;
    int needs_sp = 0, k, gi;

    for (fn = prog->funcs; fn; fn = fn->next)
        if (fn->nslots > 0)
            needs_sp = 1;

    g_syms = malloc((size_t)(g_ndef + g_nglobals + g_next + g_nextdata + 1) *
                    sizeof(*g_syms));
    g_nsyms = 0;
    g_sp_symidx = -1;

    k = 0;
    for (fn = prog->funcs; fn; fn = fn->next) {
        g_syms[g_nsyms].name = fn->name;
        g_syms[g_nsyms].kind = WASM_SYMTAB_FUNCTION;
        g_syms[g_nsyms].flags = fn->is_local ? WASM_SYM_BINDING_LOCAL : 0;
        g_syms[g_nsyms].index = g_next + k;
        g_syms[g_nsyms].emit_name = 1;
        g_nsyms++;
        k++;
    }
    /* a data symbol per global: segment 0 at its object-local offset */
    gi = 0;
    for (g = prog->globals; g; g = g->next, gi++) {
        g_syms[g_nsyms].name = g->name;
        g_syms[g_nsyms].kind = WASM_SYMTAB_DATA;
        g_syms[g_nsyms].flags = g->is_local ? WASM_SYM_BINDING_LOCAL : 0;
        g_syms[g_nsyms].index = g_obj_off[gi];      /* the segment offset */
        g_syms[g_nsyms].size = global_total(g);
        g_syms[g_nsyms].emit_name = 1;
        g_nsyms++;
    }
    if (needs_sp) {
        g_sp_symidx = g_nsyms;
        g_syms[g_nsyms].name = "__stack_pointer";
        g_syms[g_nsyms].kind = WASM_SYMTAB_GLOBAL;
        g_syms[g_nsyms].flags = WASM_SYM_UNDEFINED;
        g_syms[g_nsyms].index = 0;      /* the imported stack-pointer global */
        g_syms[g_nsyms].emit_name = 0;
        g_nsyms++;
    }
    for (k = 0; k < g_next; k++) {
        g_syms[g_nsyms].name = g_ext[k].name;
        g_syms[g_nsyms].kind = WASM_SYMTAB_FUNCTION;
        g_syms[g_nsyms].flags = WASM_SYM_UNDEFINED;
        g_syms[g_nsyms].index = k;      /* the imported function */
        g_syms[g_nsyms].emit_name = 0;
        g_nsyms++;
    }
    /* an undefined data symbol per external data address (e.g. &errno): it
       carries its name (no segment or offset), the linker binding it */
    for (k = 0; k < g_nextdata; k++) {
        g_syms[g_nsyms].name = g_extdata[k];
        g_syms[g_nsyms].kind = WASM_SYMTAB_DATA;
        g_syms[g_nsyms].flags = WASM_SYM_UNDEFINED;
        g_syms[g_nsyms].index = 0;
        g_syms[g_nsyms].size = 0;
        g_syms[g_nsyms].emit_name = 1;
        g_nsyms++;
    }
}

/* patch one pointer slot in the data image and record its data relocation: the
   address of a global (MEMORY_ADDR_I32, the addend being the placeholder) or of
   a defined function (TABLE_INDEX_I32, the placeholder being the table slot).
   Offsets are image-relative; the caller biases them to the data section. */
static void
data_ptr_slot(struct wrelocs *drc, int off, const char *sym, long addend)
{
    if (global_addr(sym) >= 0) {
        put_le(g_data_image, off, (uint64_t)(uint32_t)addend, 4);
        reloc_push(drc, R_WASM_MEMORY_ADDR_I32, (size_t)off, sym_index_data(sym), addend);
    } else if (lookup_sig(sym)) {
        put_le(g_data_image, off, (uint64_t)(uint32_t)table_index_of(sym), 4);
        reloc_push(drc, R_WASM_TABLE_INDEX_I32, (size_t)off, sym_index_func(sym), 0);
    } else {
        die("wasm: object: a data initializer referencing the external symbol %s "
            "is not yet supported; emit a complete module (.wasm) for now", sym);
    }
}

/* record the data relocations for every pointer initializer, overwriting the
   placeholder bytes build_image left with the addend (globals) or table slot
   (functions) */
static void
build_object_data_relocs(struct ir_program *prog, struct wrelocs *drc)
{
    struct ir_global *g;
    int gi = 0;

    for (g = prog->globals; g; g = g->next, gi++) {
        struct ir_init *it;
        int elsz = global_elsz(g), k;
        for (it = g->inits; it; it = it->next)
            if (it->sym)
                data_ptr_slot(drc, g_obj_off[gi] + it->offset, it->sym, (long)it->ival);
        if (g->init_syms)
            for (k = 0; k < g->init_count; k++)
                if (g->init_syms[k])
                    data_ptr_slot(drc, g_obj_off[gi] + k * elsz, g->init_syms[k],
                                  g->init_ivals ? (long)g->init_ivals[k] : 0);
    }
}

/* the byte length of an unsigned LEB128 encoding of v */
static int
uleb_len(uint64_t v)
{
    int n = 1;
    while (v >>= 7)
        n++;
    return n;
}

/* append one function's code entry (size-prefixed locals + body) to the code
   section payload, collecting its relocations (biased to section-relative) */
static void
emit_object_code_entry(struct wbuf *code, struct wrelocs *rc, struct ir_func *fn)
{
    struct wsink bs;
    struct wbuf body;
    struct wrelocs brc;
    int irr, i;
    size_t base;

    irr = prepare_function(fn);
    wbuf_init(&body);
    brc.v = NULL; brc.n = 0; brc.cap = 0;
    emit_locals_binary(&body, irr);
    bs.kind = WS_OBJ;
    bs.out = NULL;
    bs.code = &body;
    bs.rc = &brc;
    emit_body(&bs, fn, irr);
    wbuf_byte(&body, 0x0B);              /* end of the function body */

    wbuf_uleb(code, (uint64_t)body.len);
    base = code->len;                   /* the body starts here in the payload */
    wbuf_bytes(code, body.data, body.len);

    for (i = 0; i < brc.n; i++)
        reloc_push(rc, brc.v[i].type, brc.v[i].off + base,
                   brc.v[i].index, brc.v[i].addend);
    free(brc.v);
    wbuf_free(&body);
}

/* whether a relocation type carries an addend (the memory-address relocations) */
static int
reloc_has_addend(unsigned char type)
{
    return type == R_WASM_MEMORY_ADDR_LEB || type == R_WASM_MEMORY_ADDR_SLEB ||
           type == R_WASM_MEMORY_ADDR_I32;
}

static void
emit_module_object(FILE *out, struct ir_program *prog)
{
    struct wbuf mod, sec, code, sub;
    struct wrelocs rc, drc;
    struct ir_func *fn;
    struct ir_global *g;
    int k, secidx, code_secidx, data_secidx, needs_sp, has_data, needs_table;

    for (g = prog->globals; g; g = g->next)
        if (global_refs_extern(g))
            die("wasm: object output with a data initializer referencing an "
                "external symbol is not yet supported; emit a complete module "
                "(.wasm) for now");

    drc.v = NULL; drc.n = 0; drc.cap = 0;
    collect_ext_funcs(prog);
    build_module_plan(prog);
    build_object_data(prog);
    collect_table_funcs(prog);
    collect_extdata(prog);
    build_object_symtab(prog);
    build_object_data_relocs(prog, &drc);

    needs_sp = (g_sp_symidx >= 0);
    has_data = (g_data_size > 0);
    needs_table = (g_ntabfuncs > 0 || g_nitypes > 0);
    rc.v = NULL; rc.n = 0; rc.cap = 0;

    wbuf_init(&mod);
    wbuf_byte(&mod, 0x00); wbuf_byte(&mod, 0x61);
    wbuf_byte(&mod, 0x73); wbuf_byte(&mod, 0x6D);
    wbuf_u32(&mod, 1);
    secidx = 0;

    /* Type section (1) */
    wbuf_init(&sec);
    wbuf_uleb(&sec, (uint64_t)g_ntypes);
    for (k = 0; k < g_ntypes; k++)
        functype_write(&sec, &g_types[k]);
    wbuf_section(&mod, 1, &sec);
    wbuf_free(&sec);
    secidx++;

    /* Import section (2): __linear_memory, __stack_pointer, the indirect
       function table, and the env functions */
    wbuf_init(&sec);
    wbuf_uleb(&sec, (uint64_t)(1 + (needs_sp ? 1 : 0) + (needs_table ? 1 : 0) + g_next));
    wbuf_name(&sec, "env"); wbuf_name(&sec, "__linear_memory");
    wbuf_byte(&sec, 0x02); wbuf_byte(&sec, 0x00); wbuf_uleb(&sec, 0);  /* memory, min 0 */
    if (needs_sp) {
        wbuf_name(&sec, "env"); wbuf_name(&sec, "__stack_pointer");
        wbuf_byte(&sec, 0x03); wbuf_byte(&sec, 0x7F); wbuf_byte(&sec, 0x01);  /* global i32 mut */
    }
    if (needs_table) {
        wbuf_name(&sec, "env"); wbuf_name(&sec, "__indirect_function_table");
        wbuf_byte(&sec, 0x01); wbuf_byte(&sec, 0x70);   /* table, funcref */
        wbuf_byte(&sec, 0x00); wbuf_uleb(&sec, (uint64_t)(g_ntabfuncs + 1));  /* min */
    }
    for (k = 0; k < g_next; k++) {
        wbuf_name(&sec, ext_import_module(g_ext[k].name));
        wbuf_name(&sec, ext_import_field(g_ext[k].name));
        wbuf_byte(&sec, 0x00); wbuf_uleb(&sec, (uint64_t)g_ext_tidx[k]);
    }
    wbuf_section(&mod, 2, &sec);
    wbuf_free(&sec);
    secidx++;

    /* Function section (3): a type index per defined function */
    wbuf_init(&sec);
    wbuf_uleb(&sec, (uint64_t)g_ndef);
    for (k = 0; k < g_ndef; k++)
        wbuf_uleb(&sec, (uint64_t)g_def_tidx[k]);
    wbuf_section(&mod, 3, &sec);
    wbuf_free(&sec);
    secidx++;

    /* DataCount section (12): required before Code when data segments exist */
    if (has_data) {
        wbuf_init(&sec);
        wbuf_uleb(&sec, 1);              /* one data segment */
        wbuf_section(&mod, 12, &sec);
        wbuf_free(&sec);
        secidx++;
    }

    /* Code section (10): the bodies, collecting relocations */
    code_secidx = secidx;
    wbuf_init(&code);
    wbuf_uleb(&code, (uint64_t)g_ndef);
    for (fn = prog->funcs; fn; fn = fn->next)
        emit_object_code_entry(&code, &rc, fn);
    wbuf_section(&mod, 10, &code);
    secidx++;

    /* Data section (11): one active segment holding every global at its
       object-local offset (the linker assigns the final base) */
    data_secidx = -1;
    if (has_data) {
        data_secidx = secidx;
        wbuf_init(&sec);
        wbuf_uleb(&sec, 1);                      /* one segment */
        wbuf_byte(&sec, 0x00);                   /* active, memory 0 */
        wbuf_byte(&sec, 0x41); wbuf_sleb(&sec, 0); wbuf_byte(&sec, 0x0B);
        wbuf_uleb(&sec, (uint64_t)g_data_size);
        wbuf_bytes(&sec, g_data_image, (size_t)g_data_size);
        wbuf_section(&mod, 11, &sec);
        wbuf_free(&sec);
        secidx++;
    }

    /* "linking" custom section: the symbol table, then the segment info */
    wbuf_init(&sec);
    wbuf_name(&sec, "linking");
    wbuf_uleb(&sec, 2);                 /* metadata version */

    wbuf_init(&sub);
    wbuf_uleb(&sub, (uint64_t)g_nsyms);
    for (k = 0; k < g_nsyms; k++) {
        wbuf_byte(&sub, (unsigned char)g_syms[k].kind);
        wbuf_uleb(&sub, (uint64_t)g_syms[k].flags);
        if (g_syms[k].kind == WASM_SYMTAB_DATA) {
            wbuf_name(&sub, g_syms[k].name);     /* data: name first */
            /* an undefined data symbol carries only its name; a defined one
               also names its segment, offset, and size */
            if (!(g_syms[k].flags & WASM_SYM_UNDEFINED)) {
                wbuf_uleb(&sub, 0);              /* segment index (the one segment) */
                wbuf_uleb(&sub, (uint64_t)g_syms[k].index);  /* offset */
                wbuf_uleb(&sub, (uint64_t)g_syms[k].size);
            }
        } else {
            wbuf_uleb(&sub, (uint64_t)g_syms[k].index);
            if (g_syms[k].emit_name)
                wbuf_name(&sub, g_syms[k].name);
        }
    }
    wbuf_section(&sec, 8, &sub);        /* WASM_SYMBOL_TABLE subsection */
    wbuf_free(&sub);

    if (has_data) {                     /* WASM_SEGMENT_INFO subsection */
        wbuf_init(&sub);
        wbuf_uleb(&sub, 1);             /* one segment */
        wbuf_name(&sub, ".data");
        wbuf_uleb(&sub, (uint64_t)g_data_p2align);
        wbuf_uleb(&sub, 0);             /* flags */
        wbuf_section(&sec, 5, &sub);    /* WASM_SEGMENT_INFO subsection */
        wbuf_free(&sub);
    }

    wbuf_section(&mod, 0, &sec);        /* custom section */
    wbuf_free(&sec);

    /* "reloc.CODE" custom section */
    wbuf_init(&sec);
    wbuf_name(&sec, "reloc.CODE");
    wbuf_uleb(&sec, (uint64_t)code_secidx);
    wbuf_uleb(&sec, (uint64_t)rc.n);
    for (k = 0; k < rc.n; k++) {
        wbuf_byte(&sec, rc.v[k].type);
        wbuf_uleb(&sec, (uint64_t)rc.v[k].off);
        wbuf_uleb(&sec, (uint64_t)rc.v[k].index);
        if (reloc_has_addend(rc.v[k].type))
            wbuf_sleb(&sec, rc.v[k].addend);
    }
    wbuf_section(&mod, 0, &sec);
    wbuf_free(&sec);

    /* "reloc.DATA" custom section: the pointer initializers.  Each data reloc's
       offset is image-relative; bias it past the segment's framing bytes (the
       count, flags, offset expression, and size LEB) to the section contents. */
    if (drc.n > 0) {
        size_t prefix = 5 + (size_t)uleb_len((uint64_t)g_data_size);
        wbuf_init(&sec);
        wbuf_name(&sec, "reloc.DATA");
        wbuf_uleb(&sec, (uint64_t)data_secidx);
        wbuf_uleb(&sec, (uint64_t)drc.n);
        for (k = 0; k < drc.n; k++) {
            wbuf_byte(&sec, drc.v[k].type);
            wbuf_uleb(&sec, (uint64_t)(drc.v[k].off + prefix));
            wbuf_uleb(&sec, (uint64_t)drc.v[k].index);
            if (reloc_has_addend(drc.v[k].type))
                wbuf_sleb(&sec, drc.v[k].addend);
        }
        wbuf_section(&mod, 0, &sec);
        wbuf_free(&sec);
    }

    wbuf_free(&code);
    free(rc.v);
    free(drc.v);
    free(g_syms);
    free(g_obj_off);
    free(g_data_image);
    free(g_tabfuncs);
    free(g_extdata);

    fwrite(mod.data, 1, mod.len, out);
    wbuf_free(&mod);
}
