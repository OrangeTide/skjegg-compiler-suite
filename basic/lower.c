/* lower.c : lower the BASIC AST to the shared IR (doc/languages/basic.md).

   One `main` function holds the module-scope statements. Values carry a type
   (struct val): Phase 2 produces INT, a 64-bit signed integer per the spec, so
   INT scalars are 8-byte slots and arithmetic uses the IR's 64-bit opcodes
   (IR_LIC64, IR_ADD64, ... IR_DIVS64/IR_MODS64 via the runtime __divdi3 family on
   ColdFire). FLOAT, BYTE, and the aggregate types land in later phases; the
   struct-val model is the seam they slot into. */

#include <stdlib.h>
#include <string.h>

#include "basic.h"
#include "target.h"
#include "util.h"

/* Value types a lowered expression can have. A STRING is a handle: a pointer to
   a runtime object laid out as [int length][bytes]. The handle is pointer-width,
   4 bytes on an ILP32 target and 8 on an LP64 target, so a STRING temp is neither
   i64 nor float; it is a pointer, carried with the width-polymorphic pointer
   opcodes and marked with mark_ptr. */
/* TY_HANDLE is the value of an opaque exported TYPE: a pointer-width token to a
   heap record. A variable of an opaque type is a record variable whose slot holds
   this handle; reading it whole yields TY_HANDLE, while a field access (valid
   only inside the defining module, where the record's fields are known) reaches
   through it. Like a STRING, a handle is a pointer and follows the target width. */
enum { TY_INT, TY_FLOAT, TY_BYTE, TY_STRING, TY_RECORD, TY_HANDLE, TY_VECTOR };

/* Pointer width follows the target (ir/target.h): LP64 on x86-64/arm64, ILP32 on
   ColdFire/rv32/MIPS. A pointer value (STRING or HANDLE handle, a record/array
   address) uses the IR's *64 address opcodes under LP64 and the plain ones under
   ILP32, exactly as pascal/lower.c does for the same stack-ABI backends. INT
   (i64) and FLOAT (f64) are already full width on every target and are untouched. */
#define BAS_LP64      SKJ_LP64
#define BAS_PTR_BYTES SKJ_PTR_BYTES

/* A lowered expression: its IR temp and the BASIC type it holds. For a handle
   (ty == TY_HANDLE), rec is the opaque TYPE's rec_tbl index, so a handle's
   opaque type is checked nominally; rec is unused (and left 0) for every other
   type, since nothing reads it unless ty == TY_HANDLE. */
struct val { int temp; int ty; int rec; };

static struct arena      *lower_arena;
static struct ir_program *cur_prog;
static struct ir_func    *cur_fn;

/* Local-slot table for this compile (one function in Phase 1/2). */
static int  *slot_sizes;
static int   nslots;
static int   slot_cap;

/* Per-temp flag: the temp holds a 64-bit value, so it is passed as IR_ARG64 and
   stored/loaded with the 64-bit opcodes. Indexed by temp id, reset per compile. */
static unsigned char *temp_i64;
static int            temp_i64_cap;

/* A record type: ordered scalar fields with byte offsets, and the total size. */
struct rfield {
    const char *name;
    int         ty;             /* scalar field type */
    int         offset;
};
struct rec_type {
    const char   *name;
    int           nfields;
    struct rfield fields[32];
    int           size;
    int           align;        /* worst-case member alignment: a record with a
                                   FLOAT/INT (or an LP64 STRING/HANDLE) field is
                                   8-aligned, an all-BYTE record is 1-aligned. The
                                   size is padded to this, so the record is a whole
                                   number of its widest member and a small all-byte
                                   record is not over-padded. The LP64 arena hands
                                   back 8-aligned storage; the ILP32 moo arena is
                                   4-granular (its return.. push-back packs 4-byte
                                   slots), so an ILP32 heap record stays 4-aligned. */
    int           opaque;       /* an exported (or imported) opaque TYPE: its
                                   values are handles. A local one has fields and
                                   a known size (so a constructor can heap-allocate
                                   it); an imported one has neither. */
};
static struct rec_type *rec_tbl;
static int              nrec, rec_cap;

/* Variable name -> slot + type map. An array variable adds element type and
   per-dimension bounds; a record variable names a record type and, when it is a
   REF parameter, holds a pointer to the caller's record rather than the bytes. */
#define MAX_DIMS 4
struct var {
    const char *name;
    int         slot;
    int         ty;             /* scalar type; for an array, the element type */
    int         is_array;
    int         ndims;
    int         dims[MAX_DIMS];
    int         elem_size;      /* bytes per element (array or vector) */
    int         is_vector;      /* slot holds a growable-vector handle; ty is the
                                   element type and elem_size its byte width */
    int         elem_rec;       /* element record index when a vector's element
                                   type is TY_RECORD, else -1 */
    int         is_record;
    int         rec;            /* record type index (record only) */
    int         is_ref;         /* slot holds a pointer (REF record parameter) */
    int         is_global;      /* storage is a module-scope global at gsym, not a
                                   frame slot; every access uses gsym's address */
    char       *gsym;           /* the global's assembler symbol (is_global only) */
};
static struct var *vars;
static int         nvars, var_cap;

/* Module-scope variables are globals: they live in their own zeroed storage and
   are visible inside every PROC and FN, not only the main program. They are kept
   in a separate table that outlives the per-function `vars` scratch. A reference
   to a global from any function imports a local alias (tagged is_global) so the
   vars[] access paths keep working unchanged. */
static struct var *gvars;
static int         ngvars, gvar_cap;
static int         cur_scope_global;    /* 1 while lowering module-scope statements */

/* Routine signatures, for resolving calls (collected before any body is
   lowered, so a call may precede its definition). */
#define MAX_PARAMS 16
struct func_ent {
    const char *name;
    const char *link;               /* object symbol to call; = name for a local
                                       routine, the .bmi link symbol for an import */
    int         is_fn;              /* 1 = FN (returns a value), 0 = PROC */
    int         ret_ty;             /* TY_* of the return, for an FN */
    int         ret_rec;            /* opaque type index when ret_ty == TY_HANDLE,
                                       else -1 */
    int         nparams;
    int         param_ty[MAX_PARAMS];   /* TY_*, TY_RECORD for a record param */
    int         param_rec[MAX_PARAMS];  /* record/opaque type index (record or
                                           handle param), else -1 */
    int         param_ref[MAX_PARAMS];  /* REF flag (record param) */
    int         param_i32[MAX_PARAMS];  /* a foreign (EXTERNAL/imported) routine's
                                           INT parameter that crosses at the C
                                           `int` width: narrow the arg to 32 bits */
    int         ret_i32;                /* likewise for the FN result: widen the
                                           32-bit return to INT */
};
static struct func_ent *func_tbl;
static int              nfunc, func_cap;

static int rec_lookup(const char *name);
static int rec_register_opaque(const char *name);
static int fn_ret_ty(struct node *def);

/* The return type of the function being lowered: TY_* for an FN, -1 for a PROC
   or the module's main. cur_ret_rec is its opaque type index when cur_ret_ty is
   TY_HANDLE, else -1. */
static int   cur_ret_ty;
static int   cur_ret_rec;

static int   strctr;

static int
decl_to_ty(int decl)
{
    return decl == TY_DECL_FLOAT  ? TY_FLOAT
         : decl == TY_DECL_BYTE   ? TY_BYTE
         : decl == TY_DECL_STRING ? TY_STRING
         : TY_INT;
}

static struct func_ent *
func_lookup(const char *name)
{
    for (int i = 0; i < nfunc; i++)
        if (strcmp(func_tbl[i].name, name) == 0)
            return &func_tbl[i];
    return NULL;
}

/* Register a PROC/FN signature from its definition or forward declaration. A
   duplicate name (a DECLARE then its definition) keeps the first entry. */
static void
func_register(struct node *n)
{
    struct func_ent *f;
    int np = 0;

    if (func_lookup(n->name))
        return;
    if (nfunc == func_cap) {
        func_cap = func_cap ? func_cap * 2 : 16;
        func_tbl = realloc(func_tbl, func_cap * sizeof(*func_tbl));
        if (!func_tbl)
            die("oom");
    }
    f = &func_tbl[nfunc++];
    f->name = n->name;
    f->link = n->link ? n->link : n->name;   /* an EXTERNAL decl binds a C symbol */
    f->is_fn = (n->kind == N_FN);
    f->ret_ty = f->is_fn ? fn_ret_ty(n) : -1;
    /* fn_ret_ty has already resolved and validated the return type name, so the
       lookup here cannot fail when the return is a handle. */
    f->ret_rec = (f->ret_ty == TY_HANDLE) ? rec_lookup(n->sval) : -1;
    /* a vector return crosses as a handle; it is not a module value class, so an
       exported routine cannot carry one */
    if (f->ret_ty == TY_VECTOR && n->is_export)
        die("line %d: an exported FN cannot return a VECTOR", n->line);
    /* an EXTERNAL routine crosses INT at the C `int` width (i32); a BASIC-to-BASIC
       routine keeps the native 64-bit INT */
    f->ret_i32 = (n->link && n->kind == N_FN && n->op == TY_DECL_INT);
    for (struct node *pm = n->a; pm; pm = pm->next) {
        if (np >= MAX_PARAMS)
            die("line %d: routine '%s' has too many parameters",
                n->line, n->name);
        f->param_i32[np] = 0;           /* overridden for an EXTERNAL INT below */
        if (pm->op == TY_DECL_RECORD) {
            int ri = rec_lookup(pm->sval);
            if (ri < 0)
                die("line %d: unknown record type '%s'", n->line, pm->sval);
            if (rec_tbl[ri].opaque) {
                /* an opaque TYPE crosses as a handle; it is passed by reference
                   implicitly, so REF is neither needed nor written */
                f->param_ty[np] = TY_HANDLE;
                f->param_rec[np] = ri;
                f->param_ref[np] = 0;
            } else {
                if (!pm->ival)
                    die("line %d: a record parameter must be REF in this phase",
                        n->line);
                /* An exported routine must not name a private record type: the
                   .bmi would advertise it as a bare handle an importer can never
                   construct. Only an exported (opaque) TYPE crosses a boundary. */
                if (n->is_export)
                    die("line %d: exported routine '%s' takes the private record "
                        "type '%s'; EXPORT the TYPE to pass it as an opaque "
                        "handle", n->line, n->name, pm->sval);
                f->param_ty[np] = TY_RECORD;
                f->param_rec[np] = ri;
                f->param_ref[np] = 1;
            }
        } else if (pm->op == TY_DECL_VECTOR) {
            /* a vector passes by reference (its handle); it is not a module
               value class, so an exported routine cannot take one */
            if (n->is_export)
                die("line %d: an exported routine cannot take a VECTOR parameter",
                    n->line);
            f->param_ty[np] = TY_VECTOR;
            f->param_rec[np] = -1;
            f->param_ref[np] = 0;
        } else {
            f->param_ty[np] = decl_to_ty(pm->op);
            f->param_rec[np] = -1;
            f->param_ref[np] = 0;
            f->param_i32[np] = (n->link && pm->op == TY_DECL_INT);
        }
        np++;
    }
    f->nparams = np;
}

/* Map a .bmi primitive value class to a BASIC TY_*. A handle (opaque exported
   type) and a by-reference parameter have no step-2 lowering, so the caller
   rejects them before this runs. */
static int
bmi_class_to_ty(int cls)
{
    switch (cls) {
    case BMI_I8:                      return TY_BYTE;
    case BMI_I16: case BMI_I32: case BMI_I64: return TY_INT;
    case BMI_F32: case BMI_F64:       return TY_FLOAT;
    case BMI_STR:                     return TY_STRING;
    case BMI_HANDLE:                  return TY_HANDLE;
    default:                          return -1;   /* void */
    }
}

/* Build the dotted key `qual.name`, matching how a qualified import registers an
   opaque type, so a routine's handle parameter resolves to that same rec index. */
static const char *
qualify_type_name(const char *qual, const char *name)
{
    size_t ql = strlen(qual), nl = strlen(name);
    char *q = malloc(ql + nl + 2);

    if (!q)
        die("oom");
    memcpy(q, qual, ql);
    q[ql] = '.';
    memcpy(q + ql + 1, name, nl);
    q[ql + nl + 1] = '\0';
    return q;
}

/* Register an imported routine under `key` (the dotted name for a qualified
   import, the bare name for a FROM import). `qual` is the module name for a
   qualified import, so a handle type named in the .bmi signature can be resolved
   to its dotted rec key, or NULL for a FROM import whose type names stay bare.
   The parser has already filtered out the unsupported signatures (by-reference
   parameters), so the predicate at the import site is a safety assertion. */
static void
func_register_import(const char *key, struct bmi_sym *s, const char *qual)
{
    struct func_ent *f;
    int i;

    if (func_lookup(key))
        die("imported routine '%s' clashes with an existing name", key);
    if (s->nparam > MAX_PARAMS)
        die("imported routine '%s' has too many parameters", s->name);
    if (nfunc == func_cap) {
        func_cap = func_cap ? func_cap * 2 : 16;
        func_tbl = realloc(func_tbl, func_cap * sizeof(*func_tbl));
        if (!func_tbl)
            die("oom");
    }
    f = &func_tbl[nfunc++];
    f->name = key;
    f->link = s->link;
    f->is_fn = (s->kind == BMI_FUNC);
    f->ret_ty = f->is_fn ? bmi_class_to_ty(s->ret) : -1;
    /* A handle param/return names its opaque type in the .bmi; resolve it to the
       shared rec_tbl index (registering it if a routine was imported without its
       type), so the importer checks handle types against the same identity a
       handle variable of that type carries. */
    f->ret_rec = (f->ret_ty == TY_HANDLE)
                 ? rec_register_opaque(qual ? qualify_type_name(qual, s->ret_type)
                                            : s->ret_type) : -1;
    /* an i16/i32 result crosses at the C `int` width, so a 32-bit return widens
       to INT; i64 is native (a BASIC-to-BASIC routine) */
    f->ret_i32 = (s->ret == BMI_I32 || s->ret == BMI_I16);
    for (i = 0; i < s->nparam; i++) {
        f->param_ty[i] = bmi_class_to_ty(s->param[i]);
        f->param_rec[i] = (f->param_ty[i] == TY_HANDLE)
                          ? rec_register_opaque(qual
                                ? qualify_type_name(qual, s->param_type[i])
                                : s->param_type[i]) : -1;
        f->param_ref[i] = 0;
        f->param_i32[i] = (s->param[i] == BMI_I32 || s->param[i] == BMI_I16);
    }
    f->nparams = s->nparam;
}

static struct ir_insn *
emit(int op)
{
    return ir_emit(cur_fn, op);
}

static int
new_temp(void)
{
    return ir_new_temp(cur_fn);
}

static void
mark_i64(int t)
{
    if (t >= temp_i64_cap) {
        int ncap = temp_i64_cap ? temp_i64_cap * 2 : 64;
        while (t >= ncap)
            ncap *= 2;
        temp_i64 = realloc(temp_i64, ncap);
        if (!temp_i64)
            die("oom");
        memset(temp_i64 + temp_i64_cap, 0, ncap - temp_i64_cap);
        temp_i64_cap = ncap;
    }
    temp_i64[t] = 1;
}

static int
is_i64(int t)
{
    return t >= 0 && t < temp_i64_cap && temp_i64[t];
}

/* Per-temp flag: the temp holds a FLOAT (IEEE 754 double). A float temp is
   passed as IR_FARG and loaded/stored with the F-opcodes. BASIC FLOAT is always
   double, so the width tag (imm) on every float op is 0. */
static unsigned char *temp_float;
static int            temp_float_cap;

static void
mark_float(int t)
{
    if (t >= temp_float_cap) {
        int ncap = temp_float_cap ? temp_float_cap * 2 : 64;
        while (t >= ncap)
            ncap *= 2;
        temp_float = realloc(temp_float, ncap);
        if (!temp_float)
            die("oom");
        memset(temp_float + temp_float_cap, 0, ncap - temp_float_cap);
        temp_float_cap = ncap;
    }
    temp_float[t] = 1;
}

static int
is_float(int t)
{
    return t >= 0 && t < temp_float_cap && temp_float[t];
}

/* Per-temp flag: the temp holds a pointer (a STRING/HANDLE handle, or a
   record/array address). Under LP64 a pointer temp is passed as IR_ARG64 and
   returned via IR_RETV64/IR_CALL64; under ILP32 it is a plain 32-bit value. The
   pointer-emit helpers below set this; the arg/return boundaries read it. */
static unsigned char *temp_ptr;
static int            temp_ptr_cap;

static void
mark_ptr(int t)
{
    if (t >= temp_ptr_cap) {
        int ncap = temp_ptr_cap ? temp_ptr_cap * 2 : 64;
        while (t >= ncap)
            ncap *= 2;
        temp_ptr = realloc(temp_ptr, ncap);
        if (!temp_ptr)
            die("oom");
        memset(temp_ptr + temp_ptr_cap, 0, ncap - temp_ptr_cap);
        temp_ptr_cap = ncap;
    }
    temp_ptr[t] = 1;
}

static int
is_ptr(int t)
{
    return t >= 0 && t < temp_ptr_cap && temp_ptr[t];
}

static int
alloc_slot(int bytes)
{
    if (nslots == slot_cap) {
        slot_cap = slot_cap ? slot_cap * 2 : 16;
        slot_sizes = realloc(slot_sizes, slot_cap * sizeof(*slot_sizes));
        if (!slot_sizes)
            die("oom");
    }
    slot_sizes[nslots] = bytes;
    return nslots++;
}

static int
ty_slot_size(int ty)
{
    /* A STRING slot holds a pointer (BAS_PTR_BYTES: 4 on ILP32, 8 on LP64). INT
       and FLOAT are 8 bytes; a scalar BYTE also uses an 8-byte slot and the
       64-bit load/store opcodes, holding a value masked to 0..255. */
    return ty == TY_STRING ? BAS_PTR_BYTES : 8;
}

static struct var *
local_alloc(const char *name)           /* append a blank local var entry */
{
    if (nvars == var_cap) {
        var_cap = var_cap ? var_cap * 2 : 16;
        vars = realloc(vars, var_cap * sizeof(*vars));
        if (!vars)
            die("oom");
    }
    memset(&vars[nvars], 0, sizeof(vars[nvars]));
    vars[nvars].name = name;
    return &vars[nvars++];
}

static struct var *
global_alloc(const char *name)          /* append a blank module-scope global */
{
    if (ngvars == gvar_cap) {
        gvar_cap = gvar_cap ? gvar_cap * 2 : 16;
        gvars = realloc(gvars, gvar_cap * sizeof(*gvars));
        if (!gvars)
            die("oom");
    }
    memset(&gvars[ngvars], 0, sizeof(gvars[ngvars]));
    gvars[ngvars].name = name;
    return &gvars[ngvars++];
}

static int
var_find(const char *name)
{
    for (int i = 0; i < nvars; i++)
        if (strcmp(vars[i].name, name) == 0)
            return i;
    /* A module-scope global is visible from any function. Import a local alias
       (a copy, so is_global and gsym come with it) and the vars[] access paths
       below reach the global through its symbol without further change. */
    for (int i = 0; i < ngvars; i++)
        if (strcmp(gvars[i].name, name) == 0) {
            struct var *v = local_alloc(gvars[i].name);
            *v = gvars[i];
            return (int)(v - vars);
        }
    return -1;
}

/* Bytes for one element of the given scalar type, which doubles as the field's
   natural alignment in a record (STRING is a pointer-width handle, BYTE a single
   byte, INT and FLOAT eight bytes). */
static int
elem_bytes(int ty)
{
    return ty == TY_STRING ? BAS_PTR_BYTES : ty == TY_BYTE ? 1 : 8;
}

static struct var *
var_new(const char *name, int line)
{
    /* A declaration lands in the scope it is written in: a module-scope DIM makes
       a global, a DIM inside a routine makes a local (which may shadow a global
       of the same name). The duplicate check scans only that scope. */
    if (cur_scope_global) {
        for (int i = 0; i < ngvars; i++)
            if (strcmp(gvars[i].name, name) == 0)
                die("line %d: variable '%s' is already declared", line, name);
        return global_alloc(name);
    }
    for (int i = 0; i < nvars; i++)
        if (strcmp(vars[i].name, name) == 0)
            die("line %d: variable '%s' is already declared", line, name);
    return local_alloc(name);
}

/* Emit a zeroed module-scope global of `bytes` bytes: a .bss byte blob aligned
   for the widest access it holds. Scalars, arrays, records, and vector or STRING
   handles all live in such a blob, reached through its symbol's address. */
static void
emit_global_blob(const char *sym, int bytes)
{
    struct ir_global *g = arena_zalloc(lower_arena, sizeof(*g));
    g->name = arena_strdup(lower_arena, sym);
    g->base_type = IR_I8;
    g->arr_size = bytes;
    g->align = 8;
    g->is_local = 1;
    g->next = cur_prog->globals;
    cur_prog->globals = g;
}

/* Give a freshly declared variable its storage: a global blob at module scope, a
   frame slot inside a routine. */
static void
place_var(struct var *v, int bytes)
{
    if (cur_scope_global) {
        int n = snprintf(NULL, 0, "__bgv_%s", v->name);
        char *sym = arena_alloc(lower_arena, n + 1);
        snprintf(sym, n + 1, "__bgv_%s", v->name);
        v->is_global = 1;
        v->gsym = sym;
        emit_global_blob(sym, bytes);
    } else {
        v->slot = alloc_slot(bytes);
    }
}

static void
var_declare(const char *name, int ty, int line)
{
    struct var *v = var_new(name, line);
    v->ty = ty;
    place_var(v, ty_slot_size(ty));
}

/* Declare an array: one slot holds every element row-major. The slot's byte size
   is rounded up to a multiple of 8 so the following slot stays even-aligned for
   word and long memory accesses. */
static void
var_declare_array(const char *name, int elem_ty, int *dims, int ndims, int line)
{
    struct var *v = var_new(name, line);
    int total = 1, bytes;

    if (ndims > MAX_DIMS)
        die("line %d: array '%s' has too many dimensions", line, name);
    for (int i = 0; i < ndims; i++) {
        if (dims[i] <= 0)
            die("line %d: array '%s' bound must be positive", line, name);
        total *= dims[i];
        v->dims[i] = dims[i];
    }
    v->ty = elem_ty;
    v->is_array = 1;
    v->ndims = ndims;
    v->elem_size = elem_bytes(elem_ty);
    bytes = total * v->elem_size;
    bytes = (bytes + 7) & ~7;
    place_var(v, bytes);
}

/* Declare a vector: its slot holds a pointer-width handle to a growable-vector
   object that the runtime allocates (see __basic_vec_new). ty is the element
   type and elem_size its byte width, reused for the element load/store path. A
   record element carries its record index in elem_rec and is sized by the whole
   record; a scalar or STRING element carries elem_rec = -1. */
static void
var_declare_vector(const char *name, int elem_ty, int elem_rec, int line)
{
    struct var *v = var_new(name, line);
    v->ty = elem_ty;
    v->is_vector = 1;
    v->elem_rec = elem_rec;
    v->elem_size = (elem_ty == TY_RECORD) ? rec_tbl[elem_rec].size
                                          : elem_bytes(elem_ty);
    place_var(v, BAS_PTR_BYTES);
}

static int
align_up(int off, int a)
{
    return (off + a - 1) & ~(a - 1);
}

static int
rec_lookup(const char *name)
{
    for (int i = 0; i < nrec; i++)
        if (strcmp(rec_tbl[i].name, name) == 0)
            return i;
    return -1;
}

static struct rfield *
rec_field(struct rec_type *r, const char *name)
{
    for (int i = 0; i < r->nfields; i++)
        if (strcmp(r->fields[i].name, name) == 0)
            return &r->fields[i];
    return NULL;
}

/* Register a record type, laying out its fields with natural alignment (8 for
   INT/FLOAT, 4 for STRING, 1 for BYTE) and rounding the size up to 8. */
static void
rec_register(struct node *n)
{
    struct rec_type *r;
    int off = 0;

    if (rec_lookup(n->name) >= 0)
        die("line %d: record type '%s' is already defined", n->line, n->name);
    if (nrec == rec_cap) {
        rec_cap = rec_cap ? rec_cap * 2 : 8;
        rec_tbl = realloc(rec_tbl, rec_cap * sizeof(*rec_tbl));
        if (!rec_tbl)
            die("oom");
    }
    r = &rec_tbl[nrec++];
    r->name = n->name;
    r->nfields = 0;
    r->align = 1;
    r->opaque = n->is_export;       /* an exported TYPE is opaque at the boundary */
    for (struct node *f = n->a; f; f = f->next) {
        int ty, sz;
        if (f->op == TY_DECL_RECORD)
            die("line %d: a nested record field is not yet supported", n->line);
        if (r->nfields >= (int)(sizeof(r->fields) / sizeof(r->fields[0])))
            die("line %d: record '%s' has too many fields", n->line, n->name);
        ty = decl_to_ty(f->op);
        sz = elem_bytes(ty);
        off = align_up(off, sz);
        r->fields[r->nfields].name = f->name;
        r->fields[r->nfields].ty = ty;
        r->fields[r->nfields].offset = off;
        r->nfields++;
        off += sz;
        if (sz > r->align)
            r->align = sz;          /* worst-case member alignment */
    }
    r->size = align_up(off, r->align);
}

/* Register an imported opaque TYPE: a handle type with no visible fields and no
   known size (the importer never allocates or lays it out). Idempotent. */
static int
rec_register_opaque(const char *name)
{
    struct rec_type *r;
    int idx = rec_lookup(name);

    if (idx >= 0)
        return idx;
    if (nrec == rec_cap) {
        rec_cap = rec_cap ? rec_cap * 2 : 8;
        rec_tbl = realloc(rec_tbl, rec_cap * sizeof(*rec_tbl));
        if (!rec_tbl)
            die("oom");
    }
    r = &rec_tbl[nrec];
    r->name = name;
    r->nfields = 0;
    r->size = 0;
    r->align = 1;           /* a handle; never laid out by value here */
    r->opaque = 1;
    return nrec++;
}

/* The opaque type name for a handle's rec index, for a diagnostic. A negative
   index means the handle's type could not be resolved (a malformed .bmi). */
static const char *
handle_type_name(int rec)
{
    return rec >= 0 ? rec_tbl[rec].name : "<unknown>";
}

/* The return type of an FN: a scalar TY_*, or TY_HANDLE when it returns an
   exported opaque TYPE (a non-opaque record cannot be returned by value). */
static int
fn_ret_ty(struct node *def)
{
    int ri;

    if (def->op == TY_DECL_VECTOR)
        return TY_VECTOR;
    if (def->op != TY_DECL_RECORD)
        return decl_to_ty(def->op);
    ri = rec_lookup(def->sval);
    if (ri < 0)
        die("line %d: unknown record type '%s'", def->line, def->sval);
    if (!rec_tbl[ri].opaque)
        die("line %d: an FN may return a record only when it is an opaque "
            "exported TYPE", def->line);
    return TY_HANDLE;
}

/* Declare a record variable: its slot holds the record bytes, or a pointer when
   it is a REF parameter. */
static void
var_declare_record(const char *name, int rec, int is_ref, int line)
{
    struct var *v = var_new(name, line);

    v->ty = TY_RECORD;
    v->is_record = 1;
    v->rec = rec;
    v->is_ref = is_ref;
    /* a REF/handle slot holds a pointer (BAS_PTR_BYTES); a by-value record slot
       holds the record bytes */
    place_var(v, is_ref ? BAS_PTR_BYTES : rec_tbl[rec].size);
}

/* A 32-bit integer constant (for exit's status, the return value, and the
   byte-length argument of __basic_write_raw). */
static int
lower_const32(long v)
{
    struct ir_insn *ins = emit(IR_LIC);
    ins->dst = new_temp();
    ins->imm = v;
    return ins->dst;
}

/* ---- pointer / address helpers (width-polymorphic, mirroring pascal/lower.c) ----
   Each picks the LP64 *64 opcode or the ILP32 plain one off BAS_LP64, and marks
   the result a pointer so the arg/return boundary sends it at the right width. */

static int
emit_lea_sym(const char *name)          /* address of a global symbol */
{
    struct ir_insn *ins = emit(BAS_LP64 ? IR_LEA64 : IR_LEA);
    ins->dst = new_temp();
    ins->sym = arena_strdup(lower_arena, name);
    mark_ptr(ins->dst);
    return ins->dst;
}

static int
emit_adl(int slot)                      /* address of a local slot */
{
    struct ir_insn *ins = emit(BAS_LP64 ? IR_ADL64 : IR_ADL);
    ins->dst = new_temp();
    ins->slot = slot;
    mark_ptr(ins->dst);
    return ins->dst;
}

static int
emit_load_ptr(int slot)                 /* load a pointer value out of a slot */
{
    struct ir_insn *ins = emit(BAS_LP64 ? IR_LDL64 : IR_LDL);
    ins->dst = new_temp();
    ins->slot = slot;
    mark_ptr(ins->dst);
    return ins->dst;
}

static void
emit_store_ptr(int slot, int val)       /* store a pointer value into a slot */
{
    struct ir_insn *ins = emit(BAS_LP64 ? IR_STL64 : IR_STL);
    ins->slot = slot;
    ins->a = val;
}

static int
emit_load_ptr_at(int addr)              /* load a pointer value from an address */
{
    struct ir_insn *ins = emit(BAS_LP64 ? IR_LD64 : IR_LW);
    ins->dst = new_temp();
    ins->a = addr;
    mark_ptr(ins->dst);
    return ins->dst;
}

static void
emit_store_ptr_at(int addr, int val)    /* store a pointer value to an address */
{
    struct ir_insn *ins = emit(BAS_LP64 ? IR_ST64 : IR_SW);
    ins->a = addr;
    ins->b = val;
}

/* The three access wrappers below pick a variable's storage: a frame slot for a
   local, or the global symbol's address for a module-scope global. */
static int
var_ptr_addr(struct var *v)             /* address of v's storage (aggregate base) */
{
    return v->is_global ? emit_lea_sym(v->gsym) : emit_adl(v->slot);
}

static int
var_load_ptr(struct var *v)             /* load the pointer/handle held in v */
{
    return v->is_global ? emit_load_ptr_at(emit_lea_sym(v->gsym))
                        : emit_load_ptr(v->slot);
}

static void
var_store_ptr(struct var *v, int val)   /* store a pointer/handle into v */
{
    if (v->is_global)
        emit_store_ptr_at(emit_lea_sym(v->gsym), val);
    else
        emit_store_ptr(v->slot, val);
}

static int
emit_addp(int base, int off)            /* pointer + signed 32-bit offset -> pointer */
{
    struct ir_insn *ins;

    /* Under LP64 the offset is a signed 32-bit value (a field offset, or an array
       index times the element size). A 32-bit op zero-extends, so sign-extend the
       offset to 64 bits before adding it to the 64-bit base, or a small negative
       offset would become a huge address. */
    if (BAS_LP64) {
        ins = emit(IR_SEXT64);
        ins->dst = new_temp();
        ins->a = off;
        off = ins->dst;
    }
    ins = emit(BAS_LP64 ? IR_ADD64 : IR_ADD);
    ins->dst = new_temp();
    ins->a = base;
    ins->b = off;
    mark_ptr(ins->dst);
    return ins->dst;
}

/* A 64-bit INT constant. */
static struct val
lower_const64(long long v)
{
    struct ir_insn *ins = emit(IR_LIC64);
    ins->dst = new_temp();
    ins->imm = v;
    mark_i64(ins->dst);
    return (struct val){ ins->dst, TY_INT, -1 };
}

/* A FLOAT (double) constant: a read-only global holding the bit pattern, loaded
   with IR_FLD. */
static struct val
lower_const_float(double v)
{
    char label[32];
    struct ir_global *g;
    struct ir_insn *ins;
    int addr;
    union { double d; int64_t i; } u;

    snprintf(label, sizeof(label), "__bflt_%d", strctr++);
    u.d = v;
    g = arena_zalloc(lower_arena, sizeof(*g));
    g->name = arena_strdup(lower_arena, label);
    g->base_type = IR_F64;
    g->is_local = 1;
    g->is_rodata = 1;
    g->init_ivals = arena_alloc(lower_arena, sizeof(int64_t));
    g->init_ivals[0] = u.i;
    g->init_count = 1;
    g->next = cur_prog->globals;
    cur_prog->globals = g;

    addr = emit_lea_sym(label);      /* emit the address first (not inline) */
    ins = emit(IR_FLD);
    ins->dst = new_temp();
    ins->a = addr;
    ins->imm = 0;                    /* double */
    mark_float(ins->dst);
    return (struct val){ ins->dst, TY_FLOAT, -1 };
}

/* ---- control-flow and call helpers ---- */

static int
new_label(void)
{
    return ir_new_label(cur_fn);
}

static void
emit_bz(int t, int lab)
{
    struct ir_insn *i = emit(IR_BZ);
    i->a = t;
    i->label = lab;
}

static void
emit_bnz(int t, int lab)
{
    struct ir_insn *i = emit(IR_BNZ);
    i->a = t;
    i->label = lab;
}

static void
emit_jmp(int lab)
{
    struct ir_insn *i = emit(IR_JMP);
    i->label = lab;
}

static void
emit_label(int lab)
{
    struct ir_insn *i = emit(IR_LABEL);
    i->label = lab;
}

static struct val lower_expr(struct node *n);
static struct val lower_short_circuit(struct node *n, int is_and);
static struct val lower_call(struct node *n);
static struct val lower_builtin(struct node *n);
static struct val lower_str_value(struct node *s);
static struct val lower_index_read(struct node *n);
static struct val load_scalar(int addr, int ety);
static struct val lower_field_read(struct node *n);
static int        record_arg_addr(struct node *arg, int want_rec, int line);
static int        as_i32(struct val v);
static struct val int_from_i32(int c32);

/* Emit each argument with the opcode its width needs: a FLOAT as IR_FARG, a
   64-bit INT as IR_ARG64, anything else as IR_ARG. */
static void
emit_args(int *args, int nargs)
{
    struct ir_insn *ins;

    for (int i = 0; i < nargs; i++) {
        if (is_float(args[i])) {
            ins = emit(IR_FARG);
            ins->a = args[i];
            ins->imm = 0;            /* double */
        } else if (is_i64(args[i]) || (BAS_LP64 && is_ptr(args[i]))) {
            /* an i64 is 64-bit everywhere; a pointer is 64-bit under LP64 */
            ins = emit(IR_ARG64);
            ins->a = args[i];
        } else {
            ins = emit(IR_ARG);
            ins->a = args[i];
        }
    }
}

/* An i64-returning runtime call (IR_CALL64); the result is a 64-bit temp. */
static int
emit_call_ret64(const char *name, int *args, int nargs)
{
    struct ir_insn *ins;

    emit_args(args, nargs);
    ins = emit(IR_CALL64);
    ins->dst = new_temp();
    ins->sym = arena_strdup(lower_arena, name);
    ins->nargs = nargs;
    mark_i64(ins->dst);
    return ins->dst;
}

/* A call whose result is a 32-bit int (a small code or length from a runtime
   helper, e.g. LEN/ASC/INSTR, later sign-extended to INT). Read from IR_CALL's
   result register; not marked, so it passes as a plain IR_ARG. For a pointer
   result (a STRING/HANDLE handle) use emit_call_retptr instead. */
static int
emit_call_ret32(const char *name, int *args, int nargs)
{
    struct ir_insn *ins;

    emit_args(args, nargs);
    ins = emit(IR_CALL);
    ins->dst = new_temp();
    ins->sym = arena_strdup(lower_arena, name);
    ins->nargs = nargs;
    return ins->dst;
}

/* A call whose result is a pointer (a STRING/HANDLE handle, or a heap pointer
   from __moo_arena_alloc): IR_CALL64 under LP64 so the full 64-bit return
   register is read, IR_CALL under ILP32. The result is marked a pointer. */
static int
emit_call_retptr(const char *name, int *args, int nargs)
{
    struct ir_insn *ins;

    emit_args(args, nargs);
    ins = emit(BAS_LP64 ? IR_CALL64 : IR_CALL);
    ins->dst = new_temp();
    ins->sym = arena_strdup(lower_arena, name);
    ins->nargs = nargs;
    mark_ptr(ins->dst);
    return ins->dst;
}

/* A FLOAT-returning call (IR_FCALL); the result is a double temp. This matches
   an IR_FRETV callee (a BASIC FN), which uses the backend's own FP return
   convention. It is not for a C helper that returns a double (see to_float). */
static int
emit_call_retf(const char *name, int *args, int nargs)
{
    struct ir_insn *ins;

    emit_args(args, nargs);
    ins = emit(IR_FCALL);
    ins->dst = new_temp();
    ins->sym = arena_strdup(lower_arena, name);
    ins->nargs = nargs;
    ins->imm = 0;                    /* double */
    mark_float(ins->dst);
    return ins->dst;
}

/* Reinterpret a 64-bit integer temp's bits as a double, by storing it to a slot
   and reloading it as a float. A C helper that computes a double returns its
   bits as an i64 (the m68k/ColdFire C ABI returns a double in d0:d1, which does
   not match the backend's FP return convention for IR_FCALL, while an integer
   return does match), so the caller reinterprets them here. */
static struct val
bits_to_float(int bits)
{
    int slot = alloc_slot(8);
    struct ir_insn *ins;

    ins = emit(IR_STL64);
    ins->slot = slot;
    ins->a = bits;

    ins = emit(IR_FLDL);
    ins->dst = new_temp();
    ins->slot = slot;
    ins->imm = 0;                    /* double */
    mark_float(ins->dst);
    return (struct val){ ins->dst, TY_FLOAT, -1 };
}

/* Promote a value to FLOAT. A FLOAT is returned unchanged; an INT goes through
   the __basic_i64tof_bits helper (correct for the full 64-bit range, unlike a
   32-bit IR_ITOF) and is reinterpreted from its bits. */
static int
to_float(struct val v)
{
    int arg, bits;

    if (v.ty == TY_FLOAT)
        return v.temp;
    arg = v.temp;
    bits = emit_call_ret64("__basic_i64tof_bits", &arg, 1);
    return bits_to_float(bits).temp;
}

/* The truthiness of a 64-bit value as a 32-bit 0/1 (for a branch test). */
static int
truth_i32(int v64)
{
    struct val z = lower_const64(0);
    struct ir_insn *i = emit(IR_CMP64NE);
    i->dst = new_temp();
    i->a = v64;
    i->b = z.temp;
    return i->dst;   /* i32 */
}

/* Widen a 32-bit boolean (0/1 from a compare) to a 64-bit INT value. */
static struct val
bool_to_int(int b32)
{
    struct ir_insn *i = emit(IR_ZEXT64);
    i->dst = new_temp();
    i->a = b32;
    mark_i64(i->dst);
    return (struct val){ i->dst, TY_INT, -1 };
}

/* ---- loop context (for EXIT) ---- */

/* A stack of the enclosing loops, so EXIT WHILE / REPEAT / FOR can jump to the
   break label of the nearest loop of the named kind. */
struct loop_ctx { int kind; int brk; };
static struct loop_ctx loop_stack[64];
static int             loop_sp;

static void
loop_push(int kind, int brk)
{
    if (loop_sp >= (int)(sizeof(loop_stack) / sizeof(loop_stack[0])))
        die("lower: loops nested too deeply");
    loop_stack[loop_sp].kind = kind;
    loop_stack[loop_sp].brk = brk;
    loop_sp++;
}

static void
loop_pop(void)
{
    loop_sp--;
}

/* The break label of the nearest enclosing loop of kind EXIT_*; -1 if none. */
static int
loop_break_label(int exit_kind)
{
    for (int i = loop_sp - 1; i >= 0; i--)
        if (loop_stack[i].kind == exit_kind)
            return loop_stack[i].brk;
    return -1;
}

/* The i32 truthiness (0/1) of a value, for a branch test: nonzero is true. A
   FLOAT is true when it is not 0.0. */
static int
lower_truth(struct val v)
{
    struct ir_insn *ins;
    int eq, one, zero;

    if (v.ty != TY_FLOAT)
        return truth_i32(v.temp);

    /* float: truth = !(v == 0.0). The zero constant is lowered before the
       compare so its load does not land after the FCMPEQ that uses it. */
    zero = lower_const_float(0.0).temp;
    ins = emit(IR_FCMPEQ);
    ins->dst = new_temp();
    ins->a = v.temp;
    ins->b = zero;
    eq = ins->dst;
    one = lower_const32(1);
    ins = emit(IR_XOR);
    ins->dst = new_temp();
    ins->a = eq;
    ins->b = one;
    return ins->dst;
}

static void lower_stmt(struct node *n);

/* Lower a statement list (a compound statement's body). */
static void
lower_stmt_list(struct node *head)
{
    for (struct node *s = head; s; s = s->next)
        lower_stmt(s);
}

/* A FLOAT comparison, yielding an INT 0/1. The IR has three float predicates
   (EQ, LT, LE); NE negates EQ, and GT/GE swap the operands of LT/LE. */
static struct val
lower_fcmp(int bop, int af, int bf)
{
    struct ir_insn *ins;
    int cmp, swap = 0, neg = 0, r;

    switch (bop) {
    case BOP_EQ: cmp = IR_FCMPEQ;              break;
    case BOP_NE: cmp = IR_FCMPEQ; neg = 1;     break;
    case BOP_LT: cmp = IR_FCMPLT;              break;
    case BOP_LE: cmp = IR_FCMPLE;              break;
    case BOP_GT: cmp = IR_FCMPLT; swap = 1;    break;
    case BOP_GE: cmp = IR_FCMPLE; swap = 1;    break;
    default:     die("lower: bad float compare"); cmp = IR_FCMPEQ;
    }
    ins = emit(cmp);
    ins->dst = new_temp();
    ins->a = swap ? bf : af;
    ins->b = swap ? af : bf;
    r = ins->dst;                /* i32 0/1 */
    if (neg) {
        int one = lower_const32(1);
        ins = emit(IR_XOR);
        ins->dst = new_temp();
        ins->a = r;
        ins->b = one;
        r = ins->dst;
    }
    return bool_to_int(r);
}

/* ---- expressions ---- */

static int is_cmp(int op) { return op >= BOP_EQ && op <= BOP_GE; }

static struct val
lower_expr(struct node *n)
{
    struct ir_insn *ins;

    switch (n->kind) {
    case N_INT:
        return lower_const64(n->ival);

    case N_FLT:
        return lower_const_float(n->fval);

    case N_STR:
        return lower_str_value(n);

    case N_BUILTIN:
        return lower_builtin(n);

    case N_FIELD:
        return lower_field_read(n);

    case N_CALL:
        /* name(args) is a function call, or an array index if the name is a
           declared variable rather than a routine. */
        if (func_lookup(n->name)) {
            struct val r = lower_call(n);
            if (r.ty < 0)
                die("line %d: '%s' is a PROC and has no value",
                    n->line, n->name);
            return r;
        }
        return lower_index_read(n);

    case N_VAR: {
        int vi = var_find(n->name);
        if (vi < 0)
            die("line %d: undeclared variable '%s'", n->line, n->name);
        if (vars[vi].is_array)
            die("line %d: the array '%s' needs an index", n->line, n->name);
        if (vars[vi].is_vector)         /* a bare vector yields its handle */
            return (struct val){ var_load_ptr(&vars[vi]), TY_VECTOR, -1 };
        if (vars[vi].is_record) {
            /* an opaque-type variable used whole yields its handle (the pointer
               in its slot); a transparent record must be used through a field. */
            if (!rec_tbl[vars[vi].rec].opaque)
                die("line %d: the record '%s' needs a field", n->line, n->name);
            return (struct val){ var_load_ptr(&vars[vi]), TY_HANDLE,
                                 vars[vi].rec };
        }
        if (vars[vi].is_global)         /* a scalar global: load from its symbol */
            return load_scalar(emit_lea_sym(vars[vi].gsym), vars[vi].ty);
        if (vars[vi].ty == TY_FLOAT) {
            ins = emit(IR_FLDL);
            ins->dst = new_temp();
            ins->slot = vars[vi].slot;
            ins->imm = 0;            /* double */
            mark_float(ins->dst);
            return (struct val){ ins->dst, TY_FLOAT, -1 };
        }
        if (vars[vi].ty == TY_STRING)
            return (struct val){ var_load_ptr(&vars[vi]), TY_STRING, -1 };
        /* INT reads as INT; BYTE promotes to INT (its slot holds 0..255). */
        ins = emit(IR_LDL64);
        ins->dst = new_temp();
        ins->slot = vars[vi].slot;
        mark_i64(ins->dst);
        return (struct val){ ins->dst, TY_INT, -1 };
    }

    case N_NEG: {
        struct val a = lower_expr(n->a);
        if (a.ty == TY_FLOAT) {
            ins = emit(IR_FNEG);
            ins->dst = new_temp();
            ins->a = a.temp;
            ins->imm = 0;
            mark_float(ins->dst);
            return (struct val){ ins->dst, TY_FLOAT, -1 };
        }
        ins = emit(IR_NEG64);
        ins->dst = new_temp();
        ins->a = a.temp;
        mark_i64(ins->dst);
        return (struct val){ ins->dst, TY_INT, -1 };
    }

    case N_NOT: {
        /* logical NOT: zero -> 1, nonzero -> 0, i.e. (a == 0). INT only. */
        struct val a = lower_expr(n->a);
        struct val z;
        if (a.ty == TY_FLOAT)
            die("line %d: NOT requires an INT operand", n->line);
        z = lower_const64(0);
        ins = emit(IR_CMP64EQ);
        ins->dst = new_temp();
        ins->a = a.temp;
        ins->b = z.temp;
        return bool_to_int(ins->dst);
    }

    case N_BNOT: {
        struct val a = lower_expr(n->a);
        struct val c;
        if (a.ty == TY_FLOAT)
            die("line %d: BNOT requires an INT operand", n->line);
        c = lower_const64(-1);
        ins = emit(IR_XOR64);
        ins->dst = new_temp();
        ins->a = a.temp;
        ins->b = c.temp;
        mark_i64(ins->dst);
        return (struct val){ ins->dst, TY_INT, -1 };
    }

    case N_BINOP: {
        struct val a, b;
        int op, cmp, fl;

        /* AND/OR short-circuit, so their right operand must not be lowered
           eagerly: handle them before evaluating both sides. */
        if (n->op == BOP_AND)
            return lower_short_circuit(n, 1);
        if (n->op == BOP_OR)
            return lower_short_circuit(n, 0);

        a = lower_expr(n->a);
        b = lower_expr(n->b);

        /* STRING operands: '+' concatenates, the comparisons order by bytes. */
        if (a.ty == TY_STRING || b.ty == TY_STRING) {
            int args[2];
            if (a.ty != TY_STRING || b.ty != TY_STRING)
                die("line %d: a STRING cannot combine with a number here",
                    n->line);
            args[0] = a.temp;
            args[1] = b.temp;
            if (n->op == BOP_ADD)
                return (struct val){ emit_call_retptr("__basic_str_cat", args, 2),
                                     TY_STRING, -1 };
            if (is_cmp(n->op)) {
                int c32 = emit_call_ret32("__basic_str_cmp", args, 2);
                int c64, zero;
                ins = emit(IR_SEXT64);          /* widen cmp result to i64 */
                ins->dst = new_temp();
                ins->a = c32;
                mark_i64(ins->dst);
                c64 = ins->dst;
                zero = lower_const64(0).temp;
                switch (n->op) {
                case BOP_EQ: cmp = IR_CMP64EQ;  break;
                case BOP_NE: cmp = IR_CMP64NE;  break;
                case BOP_LT: cmp = IR_CMP64LTS; break;
                case BOP_LE: cmp = IR_CMP64LES; break;
                case BOP_GT: cmp = IR_CMP64GTS; break;
                default:     cmp = IR_CMP64GES; break;   /* BOP_GE */
                }
                ins = emit(cmp);
                ins->dst = new_temp();
                ins->a = c64;
                ins->b = zero;
                return bool_to_int(ins->dst);
            }
            die("line %d: that operator does not apply to a STRING", n->line);
        }

        fl = (a.ty == TY_FLOAT || b.ty == TY_FLOAT);

        /* comparisons yield an INT 0/1 */
        if (is_cmp(n->op)) {
            if (fl)
                return lower_fcmp(n->op, to_float(a), to_float(b));
            switch (n->op) {
            case BOP_EQ: cmp = IR_CMP64EQ;  break;
            case BOP_NE: cmp = IR_CMP64NE;  break;
            case BOP_LT: cmp = IR_CMP64LTS; break;
            case BOP_LE: cmp = IR_CMP64LES; break;
            case BOP_GT: cmp = IR_CMP64GTS; break;
            default:     cmp = IR_CMP64GES; break;   /* BOP_GE */
            }
            ins = emit(cmp);
            ins->dst = new_temp();
            ins->a = a.temp;
            ins->b = b.temp;
            return bool_to_int(ins->dst);
        }

        /* '/' is FLOAT division; its operands always promote to FLOAT. The
           promotions must be lowered before the IR_FDIV is emitted, or their
           instructions would land after it (operands used before defined). */
        if (n->op == BOP_SLASH) {
            int fa = to_float(a), fb = to_float(b);
            ins = emit(IR_FDIV);
            ins->dst = new_temp();
            ins->a = fa;
            ins->b = fb;
            ins->imm = 0;
            mark_float(ins->dst);
            return (struct val){ ins->dst, TY_FLOAT, -1 };
        }

        /* '^' power. INT operands use the integer helper; a FLOAT base is not
           yet supported (it needs a real pow, deferred to a later phase). */
        if (n->op == BOP_POW) {
            int args[2];
            if (fl)
                die("line %d: FLOAT '^' is not yet supported", n->line);
            args[0] = a.temp;
            args[1] = b.temp;
            return (struct val){ emit_call_ret64("__basic_ipow", args, 2), TY_INT, -1 };
        }

        /* + - * promote to FLOAT when either operand is FLOAT. As with '/', the
           promotions are lowered into locals before the float op is emitted. */
        if (fl) {
            int fa, fb;
            switch (n->op) {
            case BOP_ADD: op = IR_FADD; break;
            case BOP_SUB: op = IR_FSUB; break;
            case BOP_MUL: op = IR_FMUL; break;
            default:
                die("line %d: that operator requires INT operands", n->line);
                op = IR_FADD;
            }
            fa = to_float(a);
            fb = to_float(b);
            ins = emit(op);
            ins->dst = new_temp();
            ins->a = fa;
            ins->b = fb;
            ins->imm = 0;
            mark_float(ins->dst);
            return (struct val){ ins->dst, TY_FLOAT, -1 };
        }

        switch (n->op) {
        case BOP_ADD:  op = IR_ADD64;  break;
        case BOP_SUB:  op = IR_SUB64;  break;
        case BOP_MUL:  op = IR_MUL64;  break;
        case BOP_DIV:  op = IR_DIVS64; break;
        case BOP_MOD:  op = IR_MODS64; break;
        case BOP_BAND: op = IR_AND64;  break;
        case BOP_BOR:  op = IR_OR64;   break;
        case BOP_BXOR: op = IR_XOR64;  break;
        case BOP_SHL:  op = IR_SHL64;  break;
        case BOP_SHR:  op = IR_SHRS64; break;   /* arithmetic shift, keeps sign */
        default:       die("lower: bad binop %d", n->op); op = IR_ADD64;
        }
        ins = emit(op);
        ins->dst = new_temp();
        ins->a = a.temp;
        ins->b = b.temp;
        mark_i64(ins->dst);
        return (struct val){ ins->dst, TY_INT, -1 };
    }

    default:
        die("lower: unexpected expression node %d", n->kind);
        return (struct val){ 0, TY_INT, -1 };   /* not reached */
    }
}

/* Lower `a AND b` / `a OR b` with short-circuit evaluation, yielding an INT 0/1.
   The right operand is evaluated only when the left does not settle the result:
   a falsy left settles AND (to 0), a truthy left settles OR (to 1). */
static struct val
lower_short_circuit(struct node *n, int is_and)
{
    int slot = alloc_slot(8);
    int lshort = new_label();
    int lend = new_label();
    struct val a, b;
    int ta, tb;
    struct val tb64, settled;
    struct ir_insn *ins;

    a = lower_expr(n->a);
    ta = truth_i32(a.temp);
    if (is_and)
        emit_bz(ta, lshort);    /* left falsy: AND result is 0 */
    else
        emit_bnz(ta, lshort);   /* left truthy: OR result is 1 */

    /* left did not settle it: result is the truthiness of the right operand */
    b = lower_expr(n->b);
    tb = truth_i32(b.temp);
    tb64 = bool_to_int(tb);
    ins = emit(IR_STL64);
    ins->slot = slot;
    ins->a = tb64.temp;
    emit_jmp(lend);

    emit_label(lshort);
    settled = lower_const64(is_and ? 0 : 1);
    ins = emit(IR_STL64);
    ins->slot = slot;
    ins->a = settled.temp;

    emit_label(lend);
    ins = emit(IR_LDL64);
    ins->dst = new_temp();
    ins->slot = slot;
    mark_i64(ins->dst);
    return (struct val){ ins->dst, TY_INT, -1 };
}

/* ---- calls ---- */

/* Emit a void/int call, with each argument's width chosen by emit_args. */
static void
emit_call(const char *name, int *args, int nargs)
{
    struct ir_insn *ins;

    emit_args(args, nargs);
    ins = emit(IR_CALL);
    ins->dst = new_temp();
    ins->sym = arena_strdup(lower_arena, name);
    ins->nargs = nargs;
    /* These are statement-position calls (PRINT seams, PROC calls, exit): the
       callee returns no value and the dst temp is never read.  Marking the call
       void keeps a strictly typed backend (WASM) from declaring the seam as a
       value-returning import, which would clash with its void definition in the
       runtime.  On register backends the flag only drops an unused result. */
    ins->call_void = 1;
}

/* Lower a PROC or FN call. Each argument is coerced to its parameter's type
   (an INT promotes to a FLOAT parameter; a FLOAT into an integer parameter is
   an error). The result value carries the FN's return type, or ty = -1 for a
   PROC (which has no value). */
static struct val
lower_call(struct node *n)
{
    struct func_ent *f = func_lookup(n->name);
    int args[MAX_PARAMS];
    int nargs = 0;

    if (!f)
        die("line %d: call to undefined routine '%s'", n->line, n->name);

    for (struct node *a = n->a; a; a = a->next) {
        struct val v;
        if (nargs >= f->nparams)
            die("line %d: too many arguments to '%s'", n->line, n->name);
        if (f->param_ty[nargs] == TY_RECORD) {
            /* a record argument passes by reference: its address */
            args[nargs] = record_arg_addr(a, f->param_rec[nargs], n->line);
            nargs++;
            continue;
        }
        v = lower_expr(a);
        if (f->param_ty[nargs] == TY_STRING) {
            if (v.ty != TY_STRING)
                die("line %d: argument %d of '%s' must be a STRING",
                    n->line, nargs + 1, n->name);
            args[nargs] = v.temp;               /* 32-bit handle */
        } else if (f->param_ty[nargs] == TY_HANDLE) {
            if (v.ty != TY_HANDLE)
                die("line %d: argument %d of '%s' must be a handle (an opaque "
                    "TYPE value)", n->line, nargs + 1, n->name);
            if (v.rec != f->param_rec[nargs])
                die("line %d: argument %d of '%s' is a %s handle but the "
                    "parameter is a %s handle", n->line, nargs + 1, n->name,
                    handle_type_name(v.rec),
                    handle_type_name(f->param_rec[nargs]));
            args[nargs] = v.temp;               /* opaque handle, pointer-width */
        } else if (f->param_ty[nargs] == TY_VECTOR) {
            if (v.ty != TY_VECTOR)
                die("line %d: argument %d of '%s' must be a vector",
                    n->line, nargs + 1, n->name);
            args[nargs] = v.temp;               /* vector handle, pointer-width */
        } else if (f->param_ty[nargs] == TY_FLOAT) {
            if (v.ty == TY_STRING)
                die("line %d: argument %d of '%s' is a STRING but the parameter "
                    "is FLOAT", n->line, nargs + 1, n->name);
            args[nargs] = to_float(v);          /* promote INT, pass FLOAT */
        } else {
            if (v.ty != TY_INT && v.ty != TY_BYTE)
                die("line %d: argument %d of '%s' does not match the integer "
                    "parameter", n->line, nargs + 1, n->name);
            /* a foreign i32 parameter takes a 32-bit arg; a native INT stays i64 */
            args[nargs] = f->param_i32[nargs] ? as_i32(v) : v.temp;
        }
        nargs++;
    }
    if (nargs != f->nparams)
        die("line %d: too few arguments to '%s'", n->line, n->name);

    if (!f->is_fn) {
        emit_call(f->link, args, nargs);
        return (struct val){ 0, -1, -1 };           /* PROC: no value */
    }
    if (f->ret_ty == TY_FLOAT)
        return (struct val){ emit_call_retf(f->link, args, nargs), TY_FLOAT, -1 };
    if (f->ret_ty == TY_STRING)
        return (struct val){ emit_call_retptr(f->link, args, nargs), TY_STRING, -1 };
    if (f->ret_ty == TY_HANDLE)
        return (struct val){ emit_call_retptr(f->link, args, nargs), TY_HANDLE,
                             f->ret_rec };
    if (f->ret_ty == TY_VECTOR)
        return (struct val){ emit_call_retptr(f->link, args, nargs), TY_VECTOR,
                             -1 };
    /* INT or BYTE result: BYTE promotes to INT at the call site. A foreign i32
       result is read as a 32-bit value and sign-extended to INT. */
    if (f->ret_i32)
        return int_from_i32(emit_call_ret32(f->link, args, nargs));
    return (struct val){ emit_call_ret64(f->link, args, nargs), TY_INT, -1 };
}

/* Count the arguments on a builtin's argument chain. */
static int
arg_count(struct node *a)
{
    int n = 0;
    for (; a; a = a->next)
        n++;
    return n;
}

/* Truncate an INT value to a 32-bit temp, for a C helper that takes an int
   count or index (keeps builtin argument layout uniformly word-sized). */
static int
as_i32(struct val v)
{
    struct ir_insn *ins = emit(IR_TRUNC64);
    ins->dst = new_temp();
    ins->a = v.temp;
    return ins->dst;
}

/* Widen a 32-bit helper result (a length, code, or index) to an INT. */
static struct val
int_from_i32(int c32)
{
    struct ir_insn *ins = emit(IR_SEXT64);
    ins->dst = new_temp();
    ins->a = c32;
    mark_i64(ins->dst);
    return (struct val){ ins->dst, TY_INT, -1 };
}

/* A string argument of a builtin; errors if the argument is not a STRING. */
static int
str_arg(struct node *a, int bf)
{
    struct val v = lower_expr(a);
    if (v.ty != TY_STRING)
        die("line %d: this built-in needs a STRING argument", a->line);
    (void)bf;
    return v.temp;
}

/* An integer argument of a builtin, truncated to 32 bits for the helper. */
static int
int_arg(struct node *a)
{
    struct val v = lower_expr(a);
    if (v.ty != TY_INT && v.ty != TY_BYTE)
        die("line %d: this built-in needs an integer argument", a->line);
    return as_i32(v);
}

/* An integer argument kept at full 64-bit width, for a helper that takes a
   long long (HEX$, BIN$, ZERO$). */
static int
int_arg64(struct node *a)
{
    struct val v = lower_expr(a);
    if (v.ty != TY_INT && v.ty != TY_BYTE)
        die("line %d: this built-in needs an integer argument", a->line);
    return v.temp;
}

/* Lower a built-in function call (the string library). */
static struct val
lower_builtin(struct node *n)
{
    int nargs = arg_count(n->a);
    int args[3];

    switch (n->op) {
    case BF_LEN:
        if (nargs != 1) die("line %d: LEN takes one argument", n->line);
        args[0] = str_arg(n->a, n->op);
        return int_from_i32(emit_call_ret32("__basic_str_len", args, 1));

    case BF_COUNT: {
        int vi, h;
        if (nargs != 1 || !n->a || n->a->kind != N_VAR)
            die("line %d: COUNT takes one vector", n->line);
        vi = var_find(n->a->name);
        if (vi < 0 || !vars[vi].is_vector)
            die("line %d: COUNT needs a vector", n->line);
        h = var_load_ptr(&vars[vi]);
        return int_from_i32(emit_call_ret32("__basic_vec_count", &h, 1));
    }

    case BF_ASC:
        if (nargs != 1) die("line %d: ASC takes one argument", n->line);
        args[0] = str_arg(n->a, n->op);
        return int_from_i32(emit_call_ret32("__basic_asc", args, 1));

    case BF_INSTR:
        if (nargs != 2) die("line %d: INSTR takes two arguments", n->line);
        args[0] = str_arg(n->a, n->op);
        args[1] = str_arg(n->a->next, n->op);
        return int_from_i32(emit_call_ret32("__basic_instr", args, 2));

    case BF_CHR:
        if (nargs != 1) die("line %d: CHR$ takes one argument", n->line);
        args[0] = int_arg(n->a);
        return (struct val){ emit_call_retptr("__basic_chr", args, 1), TY_STRING, -1 };

    case BF_STR: {
        struct val x;
        if (nargs != 1) die("line %d: STR$ takes one argument", n->line);
        x = lower_expr(n->a);
        if (x.ty == TY_STRING)
            die("line %d: STR$ needs a number", n->line);
        if (x.ty == TY_FLOAT) {
            args[0] = x.temp;
            return (struct val){ emit_call_retptr("__basic_str_from_float",
                                                 args, 1), TY_STRING, -1 };
        }
        args[0] = x.temp;               /* i64, passed as ARG64 */
        return (struct val){ emit_call_retptr("__basic_str_from_int",
                                             args, 1), TY_STRING, -1 };
    }

    case BF_LEFT:
        if (nargs != 2) die("line %d: LEFT$ takes two arguments", n->line);
        args[0] = str_arg(n->a, n->op);
        args[1] = int_arg(n->a->next);
        return (struct val){ emit_call_retptr("__basic_left", args, 2),
                             TY_STRING, -1 };

    case BF_RIGHT:
        if (nargs != 2) die("line %d: RIGHT$ takes two arguments", n->line);
        args[0] = str_arg(n->a, n->op);
        args[1] = int_arg(n->a->next);
        return (struct val){ emit_call_retptr("__basic_right", args, 2),
                             TY_STRING, -1 };

    case BF_MID:
        if (nargs != 2 && nargs != 3)
            die("line %d: MID$ takes two or three arguments", n->line);
        args[0] = str_arg(n->a, n->op);
        args[1] = int_arg(n->a->next);
        if (nargs == 3)
            args[2] = int_arg(n->a->next->next);
        else
            args[2] = as_i32(lower_const64(-1));   /* -1 = to end of string */
        return (struct val){ emit_call_retptr("__basic_mid", args, 3),
                             TY_STRING, -1 };

    case BF_UCASE:
        if (nargs != 1) die("line %d: UCASE$ takes one argument", n->line);
        args[0] = str_arg(n->a, n->op);
        return (struct val){ emit_call_retptr("__basic_ucase", args, 1),
                             TY_STRING, -1 };

    case BF_LCASE:
        if (nargs != 1) die("line %d: LCASE$ takes one argument", n->line);
        args[0] = str_arg(n->a, n->op);
        return (struct val){ emit_call_retptr("__basic_lcase", args, 1),
                             TY_STRING, -1 };

    case BF_ABS: {
        struct val x;
        if (nargs != 1) die("line %d: ABS takes one argument", n->line);
        x = lower_expr(n->a);
        if (x.ty == TY_STRING) die("line %d: ABS needs a number", n->line);
        if (x.ty == TY_FLOAT) {
            args[0] = x.temp;
            return bits_to_float(emit_call_ret64("__basic_fabs_bits", args, 1));
        }
        args[0] = x.temp;
        return (struct val){ emit_call_ret64("__basic_iabs", args, 1), TY_INT, -1 };
    }

    case BF_SGN: {
        struct val x;
        if (nargs != 1) die("line %d: SGN takes one argument", n->line);
        x = lower_expr(n->a);
        if (x.ty == TY_STRING) die("line %d: SGN needs a number", n->line);
        args[0] = x.temp;
        if (x.ty == TY_FLOAT)
            return (struct val){ emit_call_ret64("__basic_fsgn", args, 1),
                                 TY_INT, -1 };
        return (struct val){ emit_call_ret64("__basic_isgn", args, 1), TY_INT, -1 };
    }

    case BF_MIN:
    case BF_MAX: {
        struct val a, b;
        int is_max = (n->op == BF_MAX);
        if (nargs != 2)
            die("line %d: %s takes two arguments", n->line,
                is_max ? "MAX" : "MIN");
        a = lower_expr(n->a);
        b = lower_expr(n->a->next);
        if (a.ty == TY_STRING || b.ty == TY_STRING)
            die("line %d: %s needs numbers", n->line, is_max ? "MAX" : "MIN");
        if (a.ty == TY_FLOAT || b.ty == TY_FLOAT) {
            args[0] = to_float(a);
            args[1] = to_float(b);
            return bits_to_float(emit_call_ret64(
                is_max ? "__basic_fmax_bits" : "__basic_fmin_bits", args, 2));
        }
        args[0] = a.temp;
        args[1] = b.temp;
        return (struct val){ emit_call_ret64(
            is_max ? "__basic_imax" : "__basic_imin", args, 2), TY_INT, -1 };
    }

    case BF_CLAMP: {
        struct val x, lo, hi;
        if (nargs != 3) die("line %d: CLAMP takes three arguments", n->line);
        x = lower_expr(n->a);
        lo = lower_expr(n->a->next);
        hi = lower_expr(n->a->next->next);
        if (x.ty == TY_STRING || lo.ty == TY_STRING || hi.ty == TY_STRING)
            die("line %d: CLAMP needs numbers", n->line);
        if (x.ty == TY_FLOAT || lo.ty == TY_FLOAT || hi.ty == TY_FLOAT) {
            args[0] = to_float(x);
            args[1] = to_float(lo);
            args[2] = to_float(hi);
            return bits_to_float(
                emit_call_ret64("__basic_fclamp_bits", args, 3));
        }
        args[0] = x.temp;
        args[1] = lo.temp;
        args[2] = hi.temp;
        return (struct val){ emit_call_ret64("__basic_iclamp", args, 3),
                             TY_INT, -1 };
    }

    case BF_INT: {
        struct val x;
        if (nargs != 1) die("line %d: INT takes one argument", n->line);
        x = lower_expr(n->a);
        if (x.ty == TY_STRING) die("line %d: INT needs a number", n->line);
        if (x.ty != TY_FLOAT)
            return (struct val){ x.temp, TY_INT, -1 };   /* already integral */
        args[0] = x.temp;
        return (struct val){ emit_call_ret64("__basic_ftoi", args, 1), TY_INT, -1 };
    }

    case BF_FLOAT: {
        struct val x;
        if (nargs != 1) die("line %d: FLOAT takes one argument", n->line);
        x = lower_expr(n->a);
        if (x.ty == TY_STRING) die("line %d: FLOAT needs a number", n->line);
        return (struct val){ to_float(x), TY_FLOAT, -1 };
    }

    case BF_FLOOR:
    case BF_CEIL:
    case BF_ROUND: {
        struct val x;
        const char *name = n->op == BF_FLOOR ? "__basic_floor"
                         : n->op == BF_CEIL  ? "__basic_ceil"
                                             : "__basic_round";
        const char *who = n->op == BF_FLOOR ? "FLOOR"
                        : n->op == BF_CEIL  ? "CEIL" : "ROUND";
        if (nargs != 1) die("line %d: %s takes one argument", n->line, who);
        x = lower_expr(n->a);
        if (x.ty == TY_STRING) die("line %d: %s needs a number", n->line, who);
        if (x.ty != TY_FLOAT)
            return (struct val){ x.temp, TY_INT, -1 };   /* already integral */
        args[0] = x.temp;
        return (struct val){ emit_call_ret64(name, args, 1), TY_INT, -1 };
    }

    case BF_LERP: {
        struct val a, b, t;
        if (nargs != 3) die("line %d: LERP takes three arguments", n->line);
        a = lower_expr(n->a);
        b = lower_expr(n->a->next);
        t = lower_expr(n->a->next->next);
        if (a.ty == TY_STRING || b.ty == TY_STRING || t.ty == TY_STRING)
            die("line %d: LERP needs numbers", n->line);
        args[0] = to_float(a);
        args[1] = to_float(b);
        args[2] = to_float(t);
        return bits_to_float(emit_call_ret64("__basic_lerp_bits", args, 3));
    }

    case BF_SQR: case BF_SIN: case BF_COS: case BF_TAN:
    case BF_ATN: case BF_SIND: case BF_COSD: case BF_TAND: {
        struct val x;
        const char *name;
        const char *who;
        switch (n->op) {
        case BF_SQR:  name = "__basic_sqr_bits";  who = "SQR";  break;
        case BF_SIN:  name = "__basic_sin_bits";  who = "SIN";  break;
        case BF_COS:  name = "__basic_cos_bits";  who = "COS";  break;
        case BF_TAN:  name = "__basic_tan_bits";  who = "TAN";  break;
        case BF_ATN:  name = "__basic_atn_bits";  who = "ATN";  break;
        case BF_SIND: name = "__basic_sind_bits"; who = "SIND"; break;
        case BF_COSD: name = "__basic_cosd_bits"; who = "COSD"; break;
        default:      name = "__basic_tand_bits"; who = "TAND"; break;
        }
        if (nargs != 1) die("line %d: %s takes one argument", n->line, who);
        x = lower_expr(n->a);
        if (x.ty == TY_STRING) die("line %d: %s needs a number", n->line, who);
        args[0] = to_float(x);
        return bits_to_float(emit_call_ret64(name, args, 1));
    }

    case BF_ATN2: {
        struct val y, x;
        if (nargs != 2) die("line %d: ATN2 takes two arguments", n->line);
        y = lower_expr(n->a);
        x = lower_expr(n->a->next);
        if (y.ty == TY_STRING || x.ty == TY_STRING)
            die("line %d: ATN2 needs numbers", n->line);
        args[0] = to_float(y);
        args[1] = to_float(x);
        return bits_to_float(emit_call_ret64("__basic_atn2_bits", args, 2));
    }

    case BF_VAL:
        if (nargs != 1) die("line %d: VAL takes one argument", n->line);
        args[0] = str_arg(n->a, n->op);
        return bits_to_float(emit_call_ret64("__basic_val_bits", args, 1));

    case BF_VALINT:
        if (nargs != 1) die("line %d: VALINT takes one argument", n->line);
        args[0] = str_arg(n->a, n->op);
        return (struct val){ emit_call_ret64("__basic_valint", args, 1),
                             TY_INT, -1 };

    case BF_SPACE:
        if (nargs != 1) die("line %d: SPACE$ takes one argument", n->line);
        args[0] = int_arg(n->a);
        return (struct val){ emit_call_retptr("__basic_space", args, 1),
                             TY_STRING, -1 };

    case BF_STRING:
        if (nargs != 2) die("line %d: STRING$ takes two arguments", n->line);
        args[0] = int_arg(n->a);
        args[1] = str_arg(n->a->next, n->op);
        return (struct val){ emit_call_retptr("__basic_string", args, 2),
                             TY_STRING, -1 };

    case BF_HEX:
        if (nargs != 1) die("line %d: HEX$ takes one argument", n->line);
        args[0] = int_arg64(n->a);
        return (struct val){ emit_call_retptr("__basic_hex", args, 1),
                             TY_STRING, -1 };

    case BF_BIN:
        if (nargs != 1) die("line %d: BIN$ takes one argument", n->line);
        args[0] = int_arg64(n->a);
        return (struct val){ emit_call_retptr("__basic_bin", args, 1),
                             TY_STRING, -1 };

    case BF_FIX: {
        struct val x;
        if (nargs != 2) die("line %d: FIX$ takes two arguments", n->line);
        x = lower_expr(n->a);
        if (x.ty == TY_STRING) die("line %d: FIX$ needs a number", n->line);
        args[0] = to_float(x);
        args[1] = int_arg(n->a->next);
        return (struct val){ emit_call_retptr("__basic_fix", args, 2),
                             TY_STRING, -1 };
    }

    case BF_ZERO:
        if (nargs != 2) die("line %d: ZERO$ takes two arguments", n->line);
        args[0] = int_arg64(n->a);
        args[1] = int_arg(n->a->next);
        return (struct val){ emit_call_retptr("__basic_zero", args, 2),
                             TY_STRING, -1 };

    case BF_RJUST:
        if (nargs != 2) die("line %d: RJUST$ takes two arguments", n->line);
        args[0] = str_arg(n->a, n->op);
        args[1] = int_arg(n->a->next);
        return (struct val){ emit_call_retptr("__basic_rjust", args, 2),
                             TY_STRING, -1 };

    case BF_LJUST:
        if (nargs != 2) die("line %d: LJUST$ takes two arguments", n->line);
        args[0] = str_arg(n->a, n->op);
        args[1] = int_arg(n->a->next);
        return (struct val){ emit_call_retptr("__basic_ljust", args, 2),
                             TY_STRING, -1 };

    case BF_RND:
        if (nargs != 0) die("line %d: RND takes no arguments", n->line);
        return bits_to_float(emit_call_ret64("__basic_rnd_bits", args, 0));

    case BF_TIME:
        if (nargs != 0) die("line %d: TIME takes no arguments", n->line);
        return bits_to_float(emit_call_ret64("__basic_time_bits", args, 0));

    case BF_SEED:
        if (nargs != 1) die("line %d: SEED takes one argument", n->line);
        args[0] = int_arg64(n->a);
        return (struct val){ emit_call_ret64("__basic_seed", args, 1), TY_INT, -1 };

    default:
        die("lower: unknown builtin %d", n->op);
        return (struct val){ 0, TY_INT, -1 };
    }
}

/* ---- aggregate element load/store ---- */

/* Load a scalar of type ety from a computed address. */
static struct val
load_scalar(int addr, int ety)
{
    struct ir_insn *ins;

    switch (ety) {
    case TY_INT:
        ins = emit(IR_LD64);
        ins->dst = new_temp();
        ins->a = addr;
        mark_i64(ins->dst);
        return (struct val){ ins->dst, TY_INT, -1 };
    case TY_BYTE: {
        int b;
        ins = emit(IR_LB);                  /* zero-extended byte -> i32 */
        ins->dst = new_temp();
        ins->a = addr;
        b = ins->dst;
        ins = emit(IR_ZEXT64);              /* promote 0..255 to INT */
        ins->dst = new_temp();
        ins->a = b;
        mark_i64(ins->dst);
        return (struct val){ ins->dst, TY_INT, -1 };
    }
    case TY_FLOAT:
        ins = emit(IR_FLD);
        ins->dst = new_temp();
        ins->a = addr;
        ins->imm = 0;
        mark_float(ins->dst);
        return (struct val){ ins->dst, TY_FLOAT, -1 };
    default:                                /* TY_STRING: a pointer-width handle */
        ins = emit(BAS_LP64 ? IR_LD64 : IR_LW);
        ins->dst = new_temp();
        ins->a = addr;
        mark_ptr(ins->dst);
        return (struct val){ ins->dst, TY_STRING, -1 };
    }
}

/* Store val (type-coerced to ety) to a computed address. `what` names the
   destination for diagnostics. */
static void
store_scalar(int addr, int ety, struct val v, int line, const char *what)
{
    struct ir_insn *ins;

    switch (ety) {
    case TY_INT:
        if (v.ty != TY_INT && v.ty != TY_BYTE)
            die("line %d: wrong value type for %s", line, what);
        ins = emit(IR_ST64);
        ins->a = addr;
        ins->b = v.temp;
        break;
    case TY_BYTE: {
        int i32;
        if (v.ty != TY_INT && v.ty != TY_BYTE)
            die("line %d: wrong value type for %s", line, what);
        i32 = as_i32(v);                    /* IR_SB keeps the low byte */
        ins = emit(IR_SB);
        ins->a = addr;
        ins->b = i32;
        break;
    }
    case TY_FLOAT: {
        int fv;
        if (v.ty == TY_STRING)
            die("line %d: wrong value type for %s", line, what);
        fv = to_float(v);
        ins = emit(IR_FSD);
        ins->a = addr;
        ins->b = fv;
        ins->imm = 0;
        break;
    }
    default:                                /* TY_STRING: a pointer-width handle */
        if (v.ty != TY_STRING)
            die("line %d: %s needs a STRING value", line, what);
        ins = emit(BAS_LP64 ? IR_ST64 : IR_SW);
        ins->a = addr;
        ins->b = v.temp;
        break;
    }
}

/* ---- arrays ---- */

/* The address (32-bit) of array element v(indices...). The row-major linear
   index and the byte offset are computed with 32-bit address arithmetic. */
static int
lower_elem_addr(struct var *v, struct node *args, int line)
{
    struct ir_insn *ins;
    int lin = -1, k = 0, off, base, esz;

    for (struct node *a = args; a; a = a->next, k++) {
        struct val iv;
        int idx32;
        if (k >= v->ndims)
            die("line %d: too many indices for '%s'", line, v->name);
        iv = lower_expr(a);
        if (iv.ty != TY_INT && iv.ty != TY_BYTE)
            die("line %d: an array index must be an integer", line);
        idx32 = as_i32(iv);
        if (lin < 0) {
            lin = idx32;
        } else {
            int dimc = lower_const32(v->dims[k]);
            ins = emit(IR_MUL);
            ins->dst = new_temp();
            ins->a = lin;
            ins->b = dimc;
            lin = ins->dst;
            ins = emit(IR_ADD);
            ins->dst = new_temp();
            ins->a = lin;
            ins->b = idx32;
            lin = ins->dst;
        }
    }
    if (k != v->ndims)
        die("line %d: '%s' needs %d indices", line, v->name, v->ndims);

    esz = lower_const32(v->elem_size);
    ins = emit(IR_MUL);
    ins->dst = new_temp();
    ins->a = lin;
    ins->b = esz;
    off = ins->dst;

    /* the base is the array slot's address (pointer-width); the index offset is a
       signed 32-bit byte count, added with emit_addp (sign-extended under LP64) */
    base = var_ptr_addr(v);
    return emit_addp(base, off);
}

/* The address of vector element v(index): the runtime resolves it from the
   handle, with a bounds check. A vector is one-dimensional. */
static int
lower_vec_elem_addr(struct var *v, struct node *args, int line)
{
    struct val iv;
    int a[2];

    if (!args || args->next)
        die("line %d: a vector takes exactly one index", line);
    iv = lower_expr(args);
    if (iv.ty != TY_INT && iv.ty != TY_BYTE)
        die("line %d: a vector index must be an integer", line);
    a[1] = as_i32(iv);
    a[0] = var_load_ptr(v);
    return emit_call_retptr("__basic_vec_elem", a, 2);
}

/* Read array or vector element n->name(n->a...). */
static struct val
lower_index_read(struct node *n)
{
    int vi = var_find(n->name);
    struct var *v;

    if (vi < 0)
        die("line %d: undefined '%s'", n->line, n->name);
    v = &vars[vi];
    if (v->ty == TY_RECORD)
        die("line %d: a record element of '%s' is used through a field "
            "(%s(i).field) or copied whole, not read as a value",
            n->line, n->name, n->name);
    if (v->is_vector)
        return load_scalar(lower_vec_elem_addr(v, n->a, n->line), v->ty);
    if (!v->is_array)
        die("line %d: '%s' is not an array", n->line, n->name);
    return load_scalar(lower_elem_addr(v, n->a, n->line), v->ty);
}

/* Store val into array element designated by desig (an N_CALL: name + indices).
   The value is lowered by the caller; the element type governs the coercion. */
static void
lower_index_store(struct node *desig, struct val val, int line)
{
    int vi = var_find(desig->name);
    struct var *v;

    if (vi < 0)
        die("line %d: undefined '%s'", line, desig->name);
    v = &vars[vi];
    if (v->is_vector) {
        store_scalar(lower_vec_elem_addr(v, desig->a, line), v->ty, val, line,
                     desig->name);
        return;
    }
    if (!v->is_array)
        die("line %d: '%s' is not an array", line, desig->name);
    store_scalar(lower_elem_addr(v, desig->a, line), v->ty, val, line,
                 desig->name);
}

/* ---- records ---- */

/* The base address (pointer-width) of a record variable: the slot's address for
   a by-value record, or the pointer the slot holds for a REF/handle parameter. */
static int
record_base(struct var *v)
{
    return v->is_ref ? var_load_ptr(v) : var_ptr_addr(v);
}

/* The address of a record designator and its record index: a record variable
   (its slot or, for a REF, the pointer it holds), or a record element of a
   vector or array (resolved through the element-address path). This is the one
   place that turns "where a record lives" into an address, so field access,
   whole-record copy, and record-argument passing all reach a vector element. */
static int
lower_record_desig(struct node *d, int *rec_out)
{
    int vi;
    struct var *v;

    if (d->kind == N_VAR) {
        vi = var_find(d->name);
        if (vi < 0)
            die("line %d: undefined '%s'", d->line, d->name);
        v = &vars[vi];
        if (!v->is_record)
            die("line %d: '%s' is not a record", d->line, d->name);
        *rec_out = v->rec;
        return record_base(v);
    }
    if (d->kind == N_CALL) {                 /* an indexed element */
        vi = var_find(d->name);
        if (vi < 0)
            die("line %d: undefined '%s'", d->line, d->name);
        v = &vars[vi];
        if (v->ty != TY_RECORD || (!v->is_vector && !v->is_array))
            die("line %d: '%s' is not a record collection", d->line, d->name);
        *rec_out = v->elem_rec;
        return v->is_vector ? lower_vec_elem_addr(v, d->a, d->line)
                            : lower_elem_addr(v, d->a, d->line);
    }
    die("line %d: expected a record", d->line);
    return 0;
}

/* Resolve a field designator (N_FIELD: a = record designator, name = field),
   returning the field's address and type. The base may be a record variable or
   an indexed record element (v(i).field). Fields are scalars (one level). */
static int
lower_field_addr(struct node *n, int *ty_out, int line)
{
    int rec, addr;
    struct rfield *fld;

    addr = lower_record_desig(n->a, &rec);
    if (rec_tbl[rec].opaque && rec_tbl[rec].nfields == 0)
        die("line %d: '%s' is an opaque TYPE; its fields are private to its "
            "module", line, rec_tbl[rec].name);
    fld = rec_field(&rec_tbl[rec], n->name);
    if (!fld)
        die("line %d: record '%s' has no field '%s'", line,
            rec_tbl[rec].name, n->name);
    *ty_out = fld->ty;

    if (fld->offset != 0)
        addr = emit_addp(addr, lower_const32(fld->offset));
    return addr;
}

static struct val
lower_field_read(struct node *n)
{
    int ety;
    int addr = lower_field_addr(n, &ety, n->line);
    return load_scalar(addr, ety);
}

/* The address of a record argument (a record variable or indexed record element
   of the wanted type), for passing a REF record parameter. */
static int
record_arg_addr(struct node *arg, int want_rec, int line)
{
    int rec, addr = lower_record_desig(arg, &rec);

    if (rec != want_rec)
        die("line %d: record argument is the wrong type", line);
    return addr;
}

/* Whole-record assignment dst = src (a block copy of the record bytes). The
   source is a record designator (a record variable or an indexed element) of
   the same type. */
static void
lower_record_assign(struct var *dst, struct node *srcnode, int line)
{
    int rec, sb, db, nb, args[3];

    sb = lower_record_desig(srcnode, &rec);
    if (rec != dst->rec)
        die("line %d: record assignment type mismatch", line);
    db = record_base(dst);
    nb = lower_const32(rec_tbl[dst->rec].size);
    args[0] = db;
    args[1] = sb;
    args[2] = nb;
    emit_call("__basic_blockcopy", args, 3);
}

/* A string literal becomes a private, read-only string object: a global laid
   out as [int length][bytes]. The runtime reads the length as a native int, so
   the 4-byte prefix is written in the target's byte order (big-endian on
   ColdFire, little-endian on RISC-V and the other little-endian targets). The
   STRING handle is the address of that object. */
static struct val
lower_str_value(struct node *s)
{
    char label[32];
    struct ir_global *g;
    int len = s->slen;
    unsigned char *buf;

    snprintf(label, sizeof(label), "__bstr_%d", strctr++);
    g = arena_zalloc(lower_arena, sizeof(*g));
    g->name = arena_strdup(lower_arena, label);
    g->base_type = IR_I8;
    g->arr_size = 4 + len;
    g->is_local = 1;
    g->is_rodata = 1;
    buf = arena_alloc(lower_arena, 4 + len);
#if SKJ_BIG_ENDIAN
    buf[0] = (unsigned char)((len >> 24) & 0xff);
    buf[1] = (unsigned char)((len >> 16) & 0xff);
    buf[2] = (unsigned char)((len >> 8) & 0xff);
    buf[3] = (unsigned char)(len & 0xff);
#else
    buf[0] = (unsigned char)(len & 0xff);
    buf[1] = (unsigned char)((len >> 8) & 0xff);
    buf[2] = (unsigned char)((len >> 16) & 0xff);
    buf[3] = (unsigned char)((len >> 24) & 0xff);
#endif
    memcpy(buf + 4, s->sval, len);
    g->init_string = (char *)buf;
    g->init_strlen = 4 + len;
    g->next = cur_prog->globals;
    cur_prog->globals = g;

    /* The handle is the address of that global object: a 4-byte length prefix
       (written above, matching struct bstr) then the bytes. Pointer-width. */
    return (struct val){ emit_lea_sym(label), TY_STRING, -1 };
}

/* ---- statements ---- */

static void
lower_print(struct node *n)
{
    struct node *item, *prev = NULL;

    for (item = n->a; item; item = item->next) {
        if (prev && prev->op == TK_COMMA)
            emit_call("__basic_write_sp", NULL, 0);

        if (item->ival == PI_AT) {
            struct val r = lower_expr(item->a);
            struct val c = lower_expr(item->b);
            int args[2];
            if ((r.ty != TY_INT && r.ty != TY_BYTE) ||
                (c.ty != TY_INT && c.ty != TY_BYTE))
                die("line %d: PRINT AT needs an integer row and column",
                    n->line);
            args[0] = r.temp;
            args[1] = c.temp;
            emit_call("__basic_at", args, 2);
        } else if (item->ival == PI_TAB) {
            struct val c = lower_expr(item->a);
            int arg;
            if (c.ty != TY_INT && c.ty != TY_BYTE)
                die("line %d: PRINT TAB needs an integer column", n->line);
            arg = c.temp;
            emit_call("__basic_tab", &arg, 1);
        } else {
            struct val v = lower_expr(item->a);
            int args[1];
            args[0] = v.temp;
            if (v.ty == TY_STRING)
                emit_call("__basic_write_str", args, 1);
            else if (v.ty == TY_FLOAT)
                emit_call("__basic_write_float", args, 1);
            else
                emit_call("__basic_write_int", args, 1);
        }
        prev = item;
    }

    if (!n->ival)
        emit_call("__basic_write_nl", NULL, 0);
}

/* Store a value into variable vi, applying the variable's type rule: an INT
   promotes to FLOAT for a FLOAT variable, a value masks to 0..255 for a BYTE,
   and a FLOAT cannot go into an INT or BYTE. STRING and numeric types do not
   mix. */
static void
store_var(int vi, struct val v, int line)
{
    struct ir_insn *ins;

    if (vars[vi].is_array)
        die("line %d: cannot assign to the whole array '%s'",
            line, vars[vi].name);
    if (vars[vi].ty == TY_STRING || v.ty == TY_STRING) {
        if (vars[vi].ty != TY_STRING || v.ty != TY_STRING)
            die("line %d: cannot mix STRING and a number in an assignment "
                "to '%s'", line, vars[vi].name);
        var_store_ptr(&vars[vi], v.temp);   /* pointer-width handle */
        return;
    }
    if (vars[vi].is_global) {    /* a numeric global: coerce then store to its symbol */
        store_scalar(emit_lea_sym(vars[vi].gsym), vars[vi].ty, v, line,
                     vars[vi].name);
        return;
    }
    if (vars[vi].ty == TY_FLOAT) {
        int fv = to_float(v);           /* promotion lowered before the store */
        ins = emit(IR_FSTL);
        ins->slot = vars[vi].slot;
        ins->a = fv;
        ins->imm = 0;                   /* double */
        return;
    }
    if (v.ty == TY_FLOAT)
        die("line %d: cannot assign a FLOAT to the integer variable '%s'",
            line, vars[vi].name);
    if (vars[vi].ty == TY_BYTE) {
        int mask = lower_const64(255).temp;
        ins = emit(IR_AND64);
        ins->dst = new_temp();
        ins->a = v.temp;
        ins->b = mask;
        mark_i64(ins->dst);
        v.temp = ins->dst;
    }
    ins = emit(IR_STL64);
    ins->slot = vars[vi].slot;
    ins->a = v.temp;
}

/* Compare two values with a bop comparison, yielding an i32 0/1. A FLOAT on
   either side promotes both and uses the float predicates. */
static int
lower_cmp_i32(int bop, struct val a, struct val b)
{
    struct ir_insn *ins;
    int cmp;

    if (a.ty == TY_FLOAT || b.ty == TY_FLOAT) {
        struct val r = lower_fcmp(bop, to_float(a), to_float(b));
        return truth_i32(r.temp);
    }
    switch (bop) {
    case BOP_EQ: cmp = IR_CMP64EQ;  break;
    case BOP_NE: cmp = IR_CMP64NE;  break;
    case BOP_LT: cmp = IR_CMP64LTS; break;
    case BOP_LE: cmp = IR_CMP64LES; break;
    case BOP_GT: cmp = IR_CMP64GTS; break;
    default:     cmp = IR_CMP64GES; break;   /* BOP_GE */
    }
    ins = emit(cmp);
    ins->dst = new_temp();
    ins->a = a.temp;
    ins->b = b.temp;
    return ins->dst;
}

/* Reload the CASE selector (kept in a slot so each WHEN test reads a fresh
   value). */
static struct val
reload_sel(int slot, int ty)
{
    struct ir_insn *ins;

    if (ty == TY_FLOAT) {
        ins = emit(IR_FLDL);
        ins->dst = new_temp();
        ins->slot = slot;
        ins->imm = 0;
        mark_float(ins->dst);
        return (struct val){ ins->dst, TY_FLOAT, -1 };
    }
    ins = emit(IR_LDL64);
    ins->dst = new_temp();
    ins->slot = slot;
    mark_i64(ins->dst);
    return (struct val){ ins->dst, TY_INT, -1 };
}

/* Lower one WHEN item against the selector in `slot`, yielding an i32 0/1. */
static int
lower_when_item(struct node *item, int slot, int ty)
{
    struct ir_insn *ins;
    int c1, c2;

    switch (item->op) {
    case WI_VALUE:
        return lower_cmp_i32(BOP_EQ, reload_sel(slot, ty), lower_expr(item->a));
    case WI_REL:
        return lower_cmp_i32((int)item->ival, reload_sel(slot, ty),
                             lower_expr(item->a));
    case WI_RANGE:
        /* lo <= sel <= hi */
        c1 = lower_cmp_i32(BOP_GE, reload_sel(slot, ty), lower_expr(item->a));
        c2 = lower_cmp_i32(BOP_LE, reload_sel(slot, ty), lower_expr(item->b));
        ins = emit(IR_AND);
        ins->dst = new_temp();
        ins->a = c1;
        ins->b = c2;
        return ins->dst;
    default:
        die("lower: bad WHEN item");
        return 0;
    }
}

/* CaseStmt: evaluate the selector once, keep it in a slot, test each WHEN item
   in order, and jump to the first matching WHEN body (no fall-through). */
static void
lower_case(struct node *n)
{
    struct ir_insn *ins;
    int lend = new_label();
    int lelse = n->c ? new_label() : lend;
    int slot = alloc_slot(8);
    struct val sel = lower_expr(n->a);
    int ty = sel.ty;
    struct node *w;

    if (ty == TY_FLOAT) {
        ins = emit(IR_FSTL);
        ins->slot = slot;
        ins->a = sel.temp;
        ins->imm = 0;
    } else {
        ins = emit(IR_STL64);
        ins->slot = slot;
        ins->a = sel.temp;
    }

    /* tests: a match jumps straight to that WHEN's body */
    for (w = n->b; w; w = w->next) {
        int bodylab = new_label();
        w->ival = bodylab;
        for (struct node *item = w->a; item; item = item->next)
            emit_bnz(lower_when_item(item, slot, ty), bodylab);
    }
    emit_jmp(lelse);                            /* no WHEN matched */

    /* bodies */
    for (w = n->b; w; w = w->next) {
        emit_label((int)w->ival);
        lower_stmt_list(w->b);
        emit_jmp(lend);
    }
    if (n->c) {
        emit_label(lelse);
        lower_stmt_list(n->c);
    }
    emit_label(lend);
}

/* Read a declared scalar variable's slot into a value of its own type. */
static struct val
read_scalar_var(int vi)
{
    struct ir_insn *ins;

    if (vars[vi].is_global) {           /* load a scalar global from its symbol */
        struct val r = load_scalar(emit_lea_sym(vars[vi].gsym), vars[vi].ty);
        if (vars[vi].ty == TY_BYTE)
            r.ty = TY_BYTE;
        return r;
    }
    if (vars[vi].ty == TY_FLOAT) {
        ins = emit(IR_FLDL);
        ins->dst = new_temp();
        ins->slot = vars[vi].slot;
        ins->imm = 0;
        mark_float(ins->dst);
        return (struct val){ ins->dst, TY_FLOAT, -1 };
    }
    if (vars[vi].ty == TY_STRING)
        return (struct val){ var_load_ptr(&vars[vi]), TY_STRING, -1 };
    ins = emit(IR_LDL64);                    /* INT, or BYTE held 0..255 */
    ins->dst = new_temp();
    ins->slot = vars[vi].slot;
    mark_i64(ins->dst);
    return (struct val){ ins->dst, vars[vi].ty == TY_BYTE ? TY_BYTE : TY_INT, -1 };
}

/* The address of the current FOR EACH element, given the 0-based index in islot:
   resolved through the vector runtime for a vector (so a grown data block is
   followed and the index is bounds-checked), or base + index*esz for an array. */
static int
foreach_elem_addr(struct var *arr, int islot, int is_vec, int esz)
{
    struct ir_insn *ins;
    int i, off, base, esz_c;

    ins = emit(IR_LDL); ins->dst = new_temp(); ins->slot = islot;
    i = ins->dst;
    if (is_vec) {
        int a[2];
        a[0] = var_load_ptr(arr);
        a[1] = i;
        return emit_call_retptr("__basic_vec_elem", a, 2);
    }
    esz_c = lower_const32(esz);
    ins = emit(IR_MUL); ins->dst = new_temp();
    ins->a = i; ins->b = esz_c; off = ins->dst;
    base = var_ptr_addr(arr);
    return emit_addp(base, off);
}

/* Lower a control-flow statement (IF / WHILE / REPEAT / FOR / EXIT / CASE). */
static void
lower_control(struct node *n)
{
    struct ir_insn *ins;

    switch (n->kind) {
    case N_IF: {
        int lelse = new_label();
        int lend = new_label();
        int t = lower_truth(lower_expr(n->a));
        emit_bz(t, n->c ? lelse : lend);        /* false -> else / past the IF */
        lower_stmt_list(n->b);
        if (n->c) {
            emit_jmp(lend);
            emit_label(lelse);
            lower_stmt_list(n->c);
        }
        emit_label(lend);
        break;
    }

    case N_WHILE: {
        int ltop = new_label();
        int lend = new_label();
        int t;
        emit_label(ltop);
        t = lower_truth(lower_expr(n->a));
        emit_bz(t, lend);                       /* condition false -> exit */
        loop_push(EXIT_WHILE, lend);
        lower_stmt_list(n->b);
        loop_pop();
        emit_jmp(ltop);
        emit_label(lend);
        break;
    }

    case N_REPEAT: {
        int ltop = new_label();
        int lend = new_label();
        int t;
        emit_label(ltop);
        loop_push(EXIT_REPEAT, lend);
        lower_stmt_list(n->b);
        loop_pop();
        t = lower_truth(lower_expr(n->a));      /* UNTIL: exit when true */
        emit_bz(t, ltop);                       /* false -> loop again */
        emit_label(lend);
        break;
    }

    case N_FOR: {
        int vi = var_find(n->name);
        int ltop = new_label();
        int lend = new_label();
        int ldesc = new_label();
        int lbody = new_label();
        int limslot, stepslot, ascslot;
        struct val lim, step;
        int v, a, cmp;

        if (vi < 0)
            die("line %d: FOR control variable '%s' is not declared",
                n->line, n->name);
        if (vars[vi].ty == TY_FLOAT)
            die("line %d: a FLOAT FOR control variable is not yet supported",
                n->line);

        /* start, limit, and step are evaluated once, before the loop. */
        store_var(vi, lower_expr(n->a), n->line);
        limslot = alloc_slot(8);
        lim = lower_expr(n->b);
        if (lim.ty == TY_FLOAT)
            die("line %d: a FLOAT FOR limit is not yet supported", n->line);
        ins = emit(IR_STL64); ins->slot = limslot; ins->a = lim.temp;

        stepslot = alloc_slot(8);
        if (n->c) {
            step = lower_expr(n->c);
            if (step.ty == TY_FLOAT)
                die("line %d: a FLOAT FOR step is not yet supported", n->line);
        } else {
            step = lower_const64(1);
        }
        ins = emit(IR_STL64); ins->slot = stepslot; ins->a = step.temp;

        /* ascending = (step >= 0), computed once and kept in a slot. */
        ascslot = alloc_slot(8);
        {
            int s, zero, ge, asc;
            ins = emit(IR_LDL64); ins->dst = new_temp(); ins->slot = stepslot;
            mark_i64(ins->dst); s = ins->dst;
            zero = lower_const64(0).temp;
            ins = emit(IR_CMP64GES); ins->dst = new_temp();
            ins->a = s; ins->b = zero; ge = ins->dst;
            ins = emit(IR_ZEXT64); ins->dst = new_temp(); ins->a = ge;
            mark_i64(ins->dst); asc = ins->dst;
            ins = emit(IR_STL64); ins->slot = ascslot; ins->a = asc;
        }

        emit_label(ltop);
        /* test: ascending -> exit when var > limit; else exit when var < limit */
        {
            int asc;
            ins = emit(IR_LDL64); ins->dst = new_temp(); ins->slot = ascslot;
            mark_i64(ins->dst); asc = ins->dst;
            emit_bz(truth_i32(asc), ldesc);     /* step < 0 -> descending test */
        }
        /* ascending test */
        v = read_scalar_var(vi).temp;           /* control var (local or global) */
        ins = emit(IR_LDL64); ins->dst = new_temp(); ins->slot = limslot;
        mark_i64(ins->dst); a = ins->dst;
        ins = emit(IR_CMP64GTS); ins->dst = new_temp(); ins->a = v; ins->b = a;
        cmp = ins->dst;
        emit_bnz(cmp, lend);                    /* var > limit -> exit */
        emit_jmp(lbody);
        /* descending test */
        emit_label(ldesc);
        v = read_scalar_var(vi).temp;           /* control var (local or global) */
        ins = emit(IR_LDL64); ins->dst = new_temp(); ins->slot = limslot;
        mark_i64(ins->dst); a = ins->dst;
        ins = emit(IR_CMP64LTS); ins->dst = new_temp(); ins->a = v; ins->b = a;
        cmp = ins->dst;
        emit_bnz(cmp, lend);                    /* var < limit -> exit */

        emit_label(lbody);
        loop_push(EXIT_FOR, lend);
        lower_stmt_list(n->d);
        loop_pop();

        /* increment: var = var + step */
        {
            int vv, ss, sum;
            vv = read_scalar_var(vi).temp;      /* control var (local or global) */
            ins = emit(IR_LDL64); ins->dst = new_temp(); ins->slot = stepslot;
            mark_i64(ins->dst); ss = ins->dst;
            ins = emit(IR_ADD64); ins->dst = new_temp(); ins->a = vv; ins->b = ss;
            mark_i64(ins->dst); sum = ins->dst;
            store_var(vi, (struct val){ sum, TY_INT, -1 }, n->line);
        }
        emit_jmp(ltop);
        emit_label(lend);
        break;
    }

    case N_FOREACH: {
        int ai = var_find(n->a->name);
        struct var *arr;
        int ety, total = 1, esz, islot, vi, is_vec, is_rec;
        int ltop = new_label(), lend = new_label();

        if (ai < 0)
            die("line %d: undeclared collection '%s'", n->line, n->a->name);
        arr = &vars[ai];
        is_vec = arr->is_vector;
        if (!arr->is_array && !is_vec)
            die("line %d: FOR EACH needs an array or vector; '%s' is not one",
                n->line, n->a->name);
        ety = arr->ty;
        esz = arr->elem_size;
        if (!is_vec)
            for (int k = 0; k < arr->ndims; k++)
                total *= arr->dims[k];

        /* The element variable is declared by the loop, with the element type.
           A record element binds the variable as an aliasing reference (its slot
           holds the element address), so a field write reaches the element in
           place; a scalar or STRING element binds by value and is written back
           after the body. The flat symbol table has no block scope, so a name
           reused by a later FOR EACH is accepted when it still matches. */
        is_rec = (ety == TY_RECORD);
        vi = var_find(n->name);
        if (vi < 0) {
            if (is_rec)
                var_declare_record(n->name, arr->elem_rec, 1, n->line);
            else
                var_declare(n->name, ety, n->line);
            vi = var_find(n->name);
        } else {
            struct var *lv = &vars[vi];
            int ok = is_rec
                     ? (lv->is_record && lv->is_ref && lv->rec == arr->elem_rec)
                     : (!lv->is_array && !lv->is_record && !lv->is_vector &&
                        lv->ty == ety);
            if (!ok)
                die("line %d: FOR EACH variable '%s' conflicts with an existing "
                    "declaration", n->line, n->name);
        }

        /* A 32-bit index counter, 0..count-1. */
        islot = alloc_slot(8);
        {
            int zero = lower_const32(0);
            ins = emit(IR_STL); ins->slot = islot; ins->a = zero;
        }

        emit_label(ltop);
        {
            int i, lim, cmp, addr, one_c;

            /* Operands that themselves emit instructions (lower_const32) are
               computed into locals before the consuming instruction, never
               inline on ins->a/b (that would emit the consumer first). The loop
               limit is the live element count for a vector (so appends and
               removes during the walk are seen) and the fixed total for an
               array. */
            ins = emit(IR_LDL); ins->dst = new_temp(); ins->slot = islot;
            i = ins->dst;
            if (is_vec) {
                int h = var_load_ptr(arr);
                lim = emit_call_ret32("__basic_vec_count", &h, 1);
            } else {
                lim = lower_const32(total);
            }
            ins = emit(IR_CMPGES); ins->dst = new_temp();
            ins->a = i; ins->b = lim; cmp = ins->dst;
            emit_bnz(cmp, lend);                /* index >= count -> exit */

            /* bind the element into the loop variable: alias a record in place,
               or load a scalar/STRING by value (written back after the body) */
            addr = foreach_elem_addr(arr, islot, is_vec, esz);
            if (is_rec)
                var_store_ptr(&vars[vi], addr);
            else
                store_var(vi, load_scalar(addr, ety), n->line);

            loop_push(EXIT_FOR, lend);
            lower_stmt_list(n->d);
            loop_pop();

            /* a by-value element is written back; a record alias needs no copy */
            if (!is_rec) {
                addr = foreach_elem_addr(arr, islot, is_vec, esz);
                store_scalar(addr, ety, read_scalar_var(vi), n->line, n->name);
            }

            /* index += 1 */
            ins = emit(IR_LDL); ins->dst = new_temp(); ins->slot = islot;
            i = ins->dst;
            one_c = lower_const32(1);
            ins = emit(IR_ADD); ins->dst = new_temp();
            ins->a = i; ins->b = one_c; i = ins->dst;
            ins = emit(IR_STL); ins->slot = islot; ins->a = i;
        }
        emit_jmp(ltop);
        emit_label(lend);
        break;
    }

    case N_EXIT: {
        int lab = loop_break_label(n->op);
        if (lab < 0)
            die("line %d: EXIT is not inside a matching loop", n->line);
        emit_jmp(lab);
        break;
    }

    case N_CASE:
        lower_case(n);
        break;

    default:
        die("lower: unexpected control node %d", n->kind);
    }
}

static void
lower_stmt(struct node *n)
{
    switch (n->kind) {
    case N_DIM:
        if (n->op == TY_DECL_VECTOR) {          /* a growable vector */
            int ety, erec = -1, esz, h, vi;
            if (n->a)
                die("line %d: a VECTOR has no fixed bound; drop the '(...)'",
                    n->line);
            if (n->velem == TY_DECL_RECORD) {
                erec = rec_lookup(n->sval);
                if (erec < 0)
                    die("line %d: unknown record type '%s'", n->line, n->sval);
                if (rec_tbl[erec].opaque && rec_tbl[erec].nfields == 0)
                    die("line %d: VECTOR OF an opaque TYPE is not supported",
                        n->line);
                ety = TY_RECORD;
            } else {
                ety = decl_to_ty(n->velem);
            }
            var_declare_vector(n->name, ety, erec, n->line);
            vi = var_find(n->name);
            esz = lower_const32(vars[vi].elem_size);
            h = emit_call_retptr("__basic_vec_new", &esz, 1);
            var_store_ptr(&vars[vi], h);
        } else if (n->op == TY_DECL_RECORD) {   /* a record variable */
            int ri = rec_lookup(n->sval);
            if (ri < 0)
                die("line %d: unknown record type '%s'", n->line, n->sval);
            if (n->a)
                die("line %d: arrays of records are not yet supported", n->line);
            if (rec_tbl[ri].opaque) {
                /* an opaque-type variable is handle-backed (its slot holds a
                   pointer). A local opaque TYPE (known size) is heap-allocated
                   here so a constructor can return it; an imported one has no
                   known size and simply holds a handle assigned from a call. */
                int vi;
                var_declare_record(n->name, ri, 1, n->line);
                vi = var_find(n->name);
                if (rec_tbl[ri].size > 0) {
                    int sz = lower_const32(rec_tbl[ri].size);
                    int h = emit_call_retptr("__moo_arena_alloc", &sz, 1);
                    var_store_ptr(&vars[vi], h);
                }
            } else {
                var_declare_record(n->name, ri, 0, n->line);
            }
        } else if (n->a) {                      /* bounds present: an array */
            int dims[MAX_DIMS], nd = 0;
            for (struct node *b = n->a; b; b = b->next) {
                if (nd >= MAX_DIMS)
                    die("line %d: array '%s' has too many dimensions",
                        n->line, n->name);
                dims[nd++] = (int)b->ival;
            }
            var_declare_array(n->name, decl_to_ty(n->op), dims, nd, n->line);
        } else {
            var_declare(n->name, decl_to_ty(n->op), n->line);
        }
        /* A local's frame slot is not cleared on entry, so zero it: every
           declared variable starts at its zero value (0, 0.0, "" for a STRING,
           a zero-filled array or record). A global sits in zeroed .bss, a vector
           is set by vec_new above, and an opaque handle by its allocation, so
           none of those need this. */
        {
            int vi = var_find(n->name);
            struct var *d = &vars[vi];
            int opaque_handle = d->is_record && rec_tbl[d->rec].opaque;
            if (!d->is_global && !d->is_vector && !opaque_handle) {
                int z[2];
                z[0] = emit_adl(d->slot);
                z[1] = lower_const32(slot_sizes[d->slot]);
                emit_call("__basic_bzero", z, 2);
            }
        }
        break;

    case N_ASSIGN:
        if (n->b && n->b->kind == N_FIELD) {    /* field target: r.f = expr */
            struct val v = lower_expr(n->a);
            int ety, addr = lower_field_addr(n->b, &ety, n->line);
            store_scalar(addr, ety, v, n->line, "the field");
        } else if (n->b) {                      /* indexed target: a(i) = expr */
            int tvi = var_find(n->b->name);
            if (tvi >= 0 && vars[tvi].ty == TY_RECORD &&
                (vars[tvi].is_vector || vars[tvi].is_array)) {
                /* whole-record element assignment v(i) = r (a block copy) */
                struct var *tv = &vars[tvi];
                int rec, src, dst, cp[3];
                dst = tv->is_vector
                      ? lower_vec_elem_addr(tv, n->b->a, n->line)
                      : lower_elem_addr(tv, n->b->a, n->line);
                src = lower_record_desig(n->a, &rec);
                if (rec != tv->elem_rec)
                    die("line %d: record assignment type mismatch", n->line);
                cp[0] = dst;
                cp[1] = src;
                cp[2] = lower_const32(tv->elem_size);
                emit_call("__basic_blockcopy", cp, 3);
            } else {
                struct val v = lower_expr(n->a);
                lower_index_store(n->b, v, n->line);
            }
        } else {
            int vi = var_find(n->name);
            if (vi < 0)
                die("line %d: undeclared variable '%s'", n->line, n->name);
            if (vars[vi].is_vector) {
                /* a vector variable holds a handle; assignment copies the handle
                   (both names then refer to the same data), like a STRING */
                struct val rv = lower_expr(n->a);
                if (rv.ty != TY_VECTOR)
                    die("line %d: '%s' is a vector and takes a vector value",
                        n->line, n->name);
                var_store_ptr(&vars[vi], rv.temp);
            } else if (vars[vi].is_record) {
                if (rec_tbl[vars[vi].rec].opaque) {
                    /* an opaque handle variable takes a handle value, stored as
                       the pointer in its slot (not a record block copy) */
                    struct val v = lower_expr(n->a);
                    if (v.ty != TY_HANDLE)
                        die("line %d: '%s' is an opaque handle and takes only a "
                            "handle value", n->line, n->name);
                    if (v.rec != vars[vi].rec)
                        die("line %d: '%s' is a %s handle but the value is a %s "
                            "handle", n->line, n->name,
                            handle_type_name(vars[vi].rec),
                            handle_type_name(v.rec));
                    var_store_ptr(&vars[vi], v.temp);
                } else {                        /* whole-record copy */
                    lower_record_assign(&vars[vi], n->a, n->line);
                }
            } else {
                store_var(vi, lower_expr(n->a), n->line);
            }
        }
        break;

    case N_IF:
    case N_WHILE:
    case N_REPEAT:
    case N_FOR:
    case N_FOREACH:
    case N_EXIT:
    case N_CASE:
        lower_control(n);
        break;

    case N_CALL:
        lower_call(n);                  /* an FN result in this position is dropped */
        break;

    case N_RETURN: {
        struct ir_insn *ins;
        if (n->a) {
            struct val v;
            if (cur_ret_ty < 0)
                die("line %d: RETURN with a value is only valid in an FN",
                    n->line);
            v = lower_expr(n->a);
            if (cur_ret_ty == TY_STRING) {
                if (v.ty != TY_STRING)
                    die("line %d: an FN of STRING must RETURN a STRING",
                        n->line);
                ins = emit(BAS_LP64 ? IR_RETV64 : IR_RETV);   /* pointer handle */
                ins->a = v.temp;
            } else if (cur_ret_ty == TY_HANDLE) {
                if (v.ty != TY_HANDLE)
                    die("line %d: an FN of an opaque TYPE must RETURN a handle "
                        "of that type", n->line);
                if (v.rec != cur_ret_rec)
                    die("line %d: this FN returns a %s handle but a %s handle is "
                        "returned", n->line, handle_type_name(cur_ret_rec),
                        handle_type_name(v.rec));
                ins = emit(BAS_LP64 ? IR_RETV64 : IR_RETV);   /* pointer handle */
                ins->a = v.temp;
            } else if (cur_ret_ty == TY_VECTOR) {
                if (v.ty != TY_VECTOR)
                    die("line %d: an FN of VECTOR must RETURN a vector", n->line);
                ins = emit(BAS_LP64 ? IR_RETV64 : IR_RETV);   /* pointer handle */
                ins->a = v.temp;
            } else if (cur_ret_ty == TY_FLOAT) {
                int fv = to_float(v);
                ins = emit(IR_FRETV);
                ins->a = fv;
                ins->imm = 0;
            } else {
                int rv;
                if (v.ty != TY_INT && v.ty != TY_BYTE)
                    die("line %d: cannot RETURN that type from an integer FN",
                        n->line);
                rv = v.temp;
                if (cur_ret_ty == TY_BYTE) {    /* a BYTE result masks to 0..255 */
                    int mask = lower_const64(255).temp;
                    ins = emit(IR_AND64);
                    ins->dst = new_temp();
                    ins->a = rv;
                    ins->b = mask;
                    mark_i64(ins->dst);
                    rv = ins->dst;
                }
                ins = emit(IR_RETV64);
                ins->a = rv;
            }
        } else {
            if (cur_ret_ty >= 0)
                die("line %d: RETURN in an FN must carry a value", n->line);
            emit(IR_RET);
        }
        break;
    }

    case N_INPUT: {
        /* optional prompt, then read one line and parse a field per target */
        if (n->a) {
            int ph = lower_str_value(n->a).temp;
            emit_call("__basic_write_str", &ph, 1);
        }
        emit_call("__basic_input_begin", NULL, 0);
        for (struct node *t = n->b; t; t = t->next) {
            int vi = var_find(t->name);
            if (vi < 0)
                die("line %d: undeclared variable '%s'", t->line, t->name);
            if (vars[vi].ty == TY_STRING) {
                int h = emit_call_retptr("__basic_input_str", NULL, 0);
                store_var(vi, (struct val){ h, TY_STRING, -1 }, t->line);
            } else if (vars[vi].ty == TY_FLOAT) {
                int bits = emit_call_ret64("__basic_input_float_bits", NULL, 0);
                store_var(vi, bits_to_float(bits), t->line);
            } else {
                int v = emit_call_ret64("__basic_input_int", NULL, 0);
                store_var(vi, (struct val){ v, TY_INT, -1 }, t->line);
            }
        }
        break;
    }

    case N_PRINT:
        lower_print(n);
        break;

    case N_WAIT: {
        struct val ms = lower_expr(n->a);
        int arg;
        if (ms.ty != TY_INT && ms.ty != TY_BYTE)
            die("line %d: WAIT needs an integer number of milliseconds",
                n->line);
        arg = as_i32(ms);
        emit_call("__basic_wait", &arg, 1);
        break;
    }

    case N_APPEND: {
        int vi = var_find(n->a->name);
        struct var *v;
        struct val val;
        int h, addr;

        if (vi < 0)
            die("line %d: undeclared vector '%s'", n->line, n->a->name);
        v = &vars[vi];
        if (!v->is_vector)
            die("line %d: APPEND needs a vector; '%s' is not one",
                n->line, n->a->name);
        if (v->ty == TY_RECORD) {
            /* Append a record element: grow the vector first, then resolve the
               source record address and block-copy its bytes into the new slot.
               Growing before resolving the source keeps the copy correct even
               when the source is an element of this same vector (the grow may
               move the data block, and the later resolve reads the new one). */
            int rec, src, cp[3];
            h = var_load_ptr(v);
            addr = emit_call_retptr("__basic_vec_push", &h, 1);
            src = lower_record_desig(n->b, &rec);
            if (rec != v->elem_rec)
                die("line %d: APPEND value is the wrong record type", n->line);
            cp[0] = addr;
            cp[1] = src;
            cp[2] = lower_const32(v->elem_size);
            emit_call("__basic_blockcopy", cp, 3);
            break;
        }
        /* Evaluate the value before growing the vector, so a side effect in the
           value expression cannot reallocate the storage under the slot address
           that push returns. */
        val = lower_expr(n->b);
        h = var_load_ptr(v);
        addr = emit_call_retptr("__basic_vec_push", &h, 1);
        store_scalar(addr, v->ty, val, n->line, n->a->name);
        break;
    }

    case N_REMOVE: {
        int vi = var_find(n->a->name);
        struct val iv;
        int args[2];

        if (vi < 0)
            die("line %d: undeclared vector '%s'", n->line, n->a->name);
        if (!vars[vi].is_vector)
            die("line %d: REMOVE needs a vector; '%s' is not one",
                n->line, n->a->name);
        iv = lower_expr(n->b);
        if (iv.ty != TY_INT && iv.ty != TY_BYTE)
            die("line %d: a vector index must be an integer", n->line);
        args[1] = as_i32(iv);
        args[0] = var_load_ptr(&vars[vi]);
        emit_call("__basic_vec_remove", args, 2);
        break;
    }

    case N_CLEAR: {
        int vi = var_find(n->a->name);
        int h;

        if (vi < 0)
            die("line %d: undeclared vector '%s'", n->line, n->a->name);
        if (!vars[vi].is_vector)
            die("line %d: CLEAR needs a vector; '%s' is not one",
                n->line, n->a->name);
        h = var_load_ptr(&vars[vi]);
        emit_call("__basic_vec_clear", &h, 1);
        break;
    }

    case N_END: {
        int args[1];
        args[0] = lower_const32(0);
        emit_call("exit", args, 1);
        break;
    }

    default:
        die("lower: unexpected statement node %d", n->kind);
    }
}

/* Is this top-level node a definition (not an executable module statement)? An
   exported CONST carries an N_CONST node for the interface writer but produces no
   code, so it counts as a definition. */
static int
is_def_node(struct node *n)
{
    return n->kind == N_PROC || n->kind == N_FN || n->kind == N_TYPE ||
           n->kind == N_CONST || n->kind == N_IMPORT;
}

/* Start a new function: fresh per-function scratch (slots, variables, temp
   flags, loop stack) and the IR function object. */
static struct ir_func *
begin_func(const char *name)
{
    struct ir_func *fn = ir_new_func(lower_arena, name);

    cur_fn = fn;
    slot_sizes = NULL; nslots = 0; slot_cap = 0;
    vars = NULL; nvars = 0; var_cap = 0;
    temp_i64 = NULL; temp_i64_cap = 0;
    temp_float = NULL; temp_float_cap = 0;
    temp_ptr = NULL; temp_ptr_cap = 0;
    loop_sp = 0;
    return fn;
}

/* Finish a function: copy the slot sizes into the arena and free the scratch. */
static void
end_func(struct ir_func *fn, int nparams)
{
    fn->nparams = nparams;
    fn->nslots = nslots;
    if (nslots > 0) {
        fn->slot_size = arena_alloc(lower_arena, nslots * sizeof(int));
        memcpy(fn->slot_size, slot_sizes, nslots * sizeof(int));
    }
    free(slot_sizes); slot_sizes = NULL; nslots = 0; slot_cap = 0;
    free(vars); vars = NULL; nvars = 0; var_cap = 0;
    free(temp_i64); temp_i64 = NULL; temp_i64_cap = 0;
    free(temp_float); temp_float = NULL; temp_float_cap = 0;
    free(temp_ptr); temp_ptr = NULL; temp_ptr_cap = 0;
}

/* Lower one PROC/FN definition. Parameters become the first nparams slots, so
   they are declared before the body (which may DIM further locals). */
static struct ir_func *
lower_user_func(struct node *def)
{
    struct ir_func *fn = begin_func(def->name);
    struct ir_insn *ins;
    int np = 0;
    int pfloat[MAX_PARAMS];
    int any_float = 0;

    cur_ret_ty = (def->kind == N_FN) ? fn_ret_ty(def) : -1;
    cur_ret_rec = (cur_ret_ty == TY_HANDLE) ? rec_lookup(def->sval) : -1;

    for (struct node *pm = def->a; pm; pm = pm->next) {
        int is_float_param = 0;
        if (pm->op == TY_DECL_RECORD) {
            int ri = rec_lookup(pm->sval);
            if (ri < 0)
                die("line %d: unknown record type '%s'", def->line, pm->sval);
            /* a record parameter is REF: the slot holds a pointer */
            var_declare_record(pm->name, ri, 1, def->line);
        } else if (pm->op == TY_DECL_VECTOR) {
            /* a vector parameter: its slot holds the handle the caller passes */
            int ety, erec = -1;
            if (pm->velem == TY_DECL_RECORD) {
                erec = rec_lookup(pm->sval);
                if (erec < 0)
                    die("line %d: unknown record type '%s'", def->line, pm->sval);
                ety = TY_RECORD;
            } else {
                ety = decl_to_ty(pm->velem);
            }
            var_declare_vector(pm->name, ety, erec, def->line);
        } else {
            int ty = decl_to_ty(pm->op);
            var_declare(pm->name, ty, def->line);
            is_float_param = (ty == TY_FLOAT);
        }
        if (np < MAX_PARAMS)
            pfloat[np] = is_float_param;
        any_float |= is_float_param;
        np++;
    }

    /* Thread the float parameter classes to the register psABI. A FLOAT
       parameter rides a hardware float register (xmm on x86-64 System V, d on
       AArch64 AAPCS), so the selector backends need its class; integer and
       pointer parameters default to class 0. The ILP32 soft-float targets
       (ColdFire, rv32, MIPS) pass every argument in integer registers or on the
       stack, so they ignore these arrays, but a BASIC FLOAT is always a double
       (param_fw 0), which is their default, so setting them is harmless there. */
    if (any_float && np <= MAX_PARAMS) {
        fn->param_class = arena_zalloc(lower_arena, np * sizeof(int));
        fn->param_neb = arena_zalloc(lower_arena, np * sizeof(signed char));
        fn->param_cls = arena_zalloc(lower_arena,
                                     4 * np * sizeof(signed char));
        fn->param_fw = arena_zalloc(lower_arena, np * sizeof(signed char));
        for (int i = 0; i < np; i++) {
            fn->param_neb[i] = 1;
            fn->param_class[i] = pfloat[i];
            fn->param_cls[4 * i] = (signed char)pfloat[i];
            fn->param_cls[4 * i + 1] = -1;
            fn->param_cls[4 * i + 2] = -1;
            fn->param_cls[4 * i + 3] = -1;
            fn->param_fw[i] = 0;        /* BASIC FLOAT is always a double */
        }
    }

    ins = emit(IR_FUNC);
    ins->sym = arena_strdup(lower_arena, def->name);
    ins->nargs = np;

    lower_stmt_list(def->b);

    /* A fallback return keeps codegen valid when control reaches the end. An FN
       that can fall off its end without RETURN is a spec error not yet
       diagnosed; it gets a defined zero result here. */
    if (cur_ret_ty == TY_FLOAT) {
        struct val z = lower_const_float(0.0);
        ins = emit(IR_FRETV); ins->a = z.temp; ins->imm = 0;
    } else if (cur_ret_ty == TY_STRING || cur_ret_ty == TY_HANDLE) {
        /* NULL handle, pointer-width */
        if (BAS_LP64) {
            ins = emit(IR_RETV64); ins->a = lower_const64(0).temp;
        } else {
            ins = emit(IR_RETV); ins->a = lower_const32(0);
        }
    } else if (cur_ret_ty >= 0) {
        struct val z = lower_const64(0);
        ins = emit(IR_RETV64); ins->a = z.temp;
    } else {
        emit(IR_RET);
    }
    emit(IR_ENDF);

    end_func(fn, np);
    return fn;
}

struct ir_program *
lower_program(struct arena *a, struct node *ast)
{
    struct ir_program *prog;
    struct ir_func *main_fn, **fn_tail;
    struct ir_insn *ins;
    struct node *s;
    int args[1];

    lower_arena = a;
    prog = arena_zalloc(a, sizeof(*prog));
    cur_prog = prog;

    func_tbl = NULL; nfunc = 0; func_cap = 0;
    rec_tbl = NULL; nrec = 0; rec_cap = 0;
    gvars = NULL; ngvars = 0; gvar_cap = 0;
    cur_scope_global = 0;
    strctr = 0;

    if (!ast || ast->kind != N_PROGRAM)
        die("lower: expected a program node");

    /* pass 1a: record types first (local, then imported opaque ones), so routine
       signatures and DIMs can name them. pass 1b: routine signatures, so a call
       may precede its definition (and forward DECLAREs are honoured), followed by
       the imported routines. */
    for (s = ast->a; s; s = s->next)
        if (s->kind == N_TYPE)
            rec_register(s);
    for (s = ast->a; s; s = s->next)
        if (s->kind == N_IMPORT && s->imp->kind == BMI_TYPE)
            rec_register_opaque(s->name);   /* key: dotted for a qualified import */
    for (s = ast->a; s; s = s->next)
        if (s->kind == N_PROC || s->kind == N_FN)
            func_register(s);
    for (s = ast->a; s; s = s->next)
        if (s->kind == N_IMPORT && s->imp->kind != BMI_TYPE)
            func_register_import(s->name, s->imp, s->sval);

    /* A file headed by MODULE is a library: it has no entry point, and only its
       exported routines are globally visible. A program instead forms a main
       function from its module-scope statements. */
    if (ast->name) {
        for (s = ast->a; s; s = s->next)
            if (!is_def_node(s))
                die("line %d: a MODULE has no top-level statements", s->line);
        prog->funcs = NULL;
        fn_tail = &prog->funcs;
    } else {
        main_fn = begin_func("main");
        cur_ret_ty = -1;
        cur_ret_rec = -1;
        ins = emit(IR_FUNC);
        ins->sym = arena_strdup(a, "main");
        ins->nargs = 0;
        /* Module-scope DIMs declare globals (visible in every routine); the main
           program's own statements run here in source order. */
        cur_scope_global = 1;
        for (s = ast->a; s; s = s->next)
            if (!is_def_node(s))
                lower_stmt(s);
        cur_scope_global = 0;
        args[0] = lower_const32(0);             /* implicit exit(0) */
        emit_call("exit", args, 1);
        ins = emit(IR_RETV);
        ins->a = lower_const32(0);
        emit(IR_ENDF);
        end_func(main_fn, 0);

        prog->funcs = main_fn;
        fn_tail = &main_fn->next;
    }

    /* each PROC/FN with a body becomes its own function (forward DECLAREs,
       marked ival = 1, have no body and emit nothing). In a module, only an
       EXPORTed routine is global; the rest are file-local. */
    for (s = ast->a; s; s = s->next)
        if ((s->kind == N_PROC || s->kind == N_FN) && !s->ival) {
            struct ir_func *fn = lower_user_func(s);
            if (ast->name)
                fn->is_local = !s->is_export;
            *fn_tail = fn;
            fn_tail = &fn->next;
        }
    *fn_tail = NULL;

    free(func_tbl); func_tbl = NULL; nfunc = 0; func_cap = 0;
    free(rec_tbl); rec_tbl = NULL; nrec = 0; rec_cap = 0;
    free(gvars); gvars = NULL; ngvars = 0; gvar_cap = 0;

    return prog;
}
