/* jit_common.c : the architecture-neutral core of the in-process JIT (see
   jit_common.h).  Data layout, symbol resolution, relocation bookkeeping, the
   low-memory mappings, and the kp_jit() driver live here; the per-target byte
   sink and the two relocation appliers arrive through kp_target. */

#define _POSIX_C_SOURCE 200809L

#include "jit_common.h"
#include "jit_arena.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* The JIT that kp_jit() last set up.  One module runs per process, so the
   __moo_arena_alloc binding serves this one. */
static struct kp_jit *kp_active;

int
kp_fail(struct emitctx *e, const char *fmt, const char *a, const char *b)
{
    if (e->err && e->errlen)
        snprintf(e->err, e->errlen, fmt, a, b);
    return -1;
}

/****************************************************************
 * Layout helpers
 ****************************************************************/

static int
base_size(int bt)
{
    switch (bt) {
    case IR_I8: return 1;
    case IR_I16: return 2;
    case IR_I32: case IR_F32: return 4;
    case IR_I64: case IR_F64: return 8;
    default: return 4;
    }
}

uint8_t *
kp_global_addr(struct kp_jit *j, const char *name)
{
    for (int i = 0; i < j->nglobals; i++)
        if (strcmp(j->globals[i].name, name) == 0)
            return j->globals[i].addr;
    return NULL;
}

void *
kp_import_addr(struct emitctx *e, const char *name)
{
    for (int i = 0; i < e->nbinds; i++)
        if (strcmp(e->binds[i].name, name) == 0)
            return e->binds[i].addr;
    return NULL;
}

/* Resolve a data-relocation target to its runtime address: a global's data
   home, a function's code entry, or a host binding.  Returns 1 and sets *out
   on success.  Called after data and code are laid out. */
static int
resolve_data_sym(struct kp_jit *j, const struct kp_binding *binds, int nbinds,
                 const char *name, uint64_t *out)
{
    for (int k = 0; k < j->nglobals; k++)
        if (strcmp(j->globals[k].name, name) == 0) {
            *out = (uint64_t)(uintptr_t)j->globals[k].addr;
            return 1;
        }
    for (int k = 0; k < j->nfuncs; k++)
        if (strcmp(j->funcs[k].name, name) == 0) {
            *out = (uint64_t)(uintptr_t)(j->code + j->funcs[k].off);
            return 1;
        }
    for (int k = 0; k < nbinds; k++)
        if (strcmp(binds[k].name, name) == 0) {
            *out = (uint64_t)(uintptr_t)binds[k].addr;
            return 1;
        }
    return 0;
}

int
kp_is_module_func(struct ir_program *prog, const char *name)
{
    for (struct ir_func *fn = prog->funcs; fn; fn = fn->next)
        if (strcmp(fn->name, name) == 0)
            return 1;
    return 0;
}

void
kp_add_reloc(struct emitctx *e, size_t site, const char *target)
{
    if (e->nrelocs == e->crelocs) {
        e->crelocs = e->crelocs ? e->crelocs * 2 : 32;
        e->relocs = realloc(e->relocs, (size_t)e->crelocs * sizeof(*e->relocs));
    }
    e->relocs[e->nrelocs].site = site;
    e->relocs[e->nrelocs].target = target;
    e->nrelocs++;
}

/* Record an absolute reference to a function's code address, patched into the
   mapped code once its final address is known. */
void
kp_add_absreloc(struct emitctx *e, size_t site, const char *target)
{
    if (e->nabs == e->cabs) {
        e->cabs = e->cabs ? e->cabs * 2 : 16;
        e->absrelocs = realloc(e->absrelocs, (size_t)e->cabs * sizeof(*e->absrelocs));
    }
    e->absrelocs[e->nabs].site = site;
    e->absrelocs[e->nabs].target = target;
    e->nabs++;
}

/* Record that code from offset `off` onward comes from source `line`.  Called
   for each IR_LOC marker; offsets grow monotonically, so the table stays sorted
   and a trap maps its address to the last entry at or before it. */
void
kp_add_line(struct emitctx *e, size_t off, int line)
{
    if (e->nlines == e->clines) {
        e->clines = e->clines ? e->clines * 2 : 64;
        e->lines = realloc(e->lines, (size_t)e->clines * sizeof(*e->lines));
    }
    e->lines[e->nlines].off = off;
    e->lines[e->nlines].line = line;
    e->nlines++;
}


static int
layout_data(struct kp_jit *j, struct ir_program *prog, char *err, size_t errlen)
{
    int ng = 0;
    size_t off = 0;

    for (struct ir_global *g = prog->globals; g; g = g->next)
        ng++;
    j->globals = calloc((size_t)(ng > 0 ? ng : 1), sizeof(*j->globals));
    j->nglobals = ng;

    /* assign 8-aligned offsets */
    int i = 0;
    for (struct ir_global *g = prog->globals; g; g = g->next, i++) {
        size_t sz = (size_t)(g->arr_size > 0 ? g->arr_size : 1)
                    * (size_t)base_size(g->base_type);
        off = (off + 7) & ~(size_t)7;
        j->globals[i].name = g->name;
        j->globals[i].addr = (uint8_t *)off;   /* provisional: byte offset */
        off += sz;
    }
    j->data_len = off;
    j->data = NULL;
    if (off) {
        /* Map the data region low so a global's address fits the 32 bits the
           front end models addresses with; a receiver or array pointer then
           round-trips through its i32 slot.  Anonymous pages come back zeroed,
           which is the default initializer. */
        long pagesz = sysconf(_SC_PAGESIZE);
        j->data_map = ((off + (size_t)pagesz - 1) / (size_t)pagesz)
                      * (size_t)pagesz;
        j->data = kp_map_low(j->data_map);
        if (j->data == MAP_FAILED) {
            j->data = NULL;
            snprintf(err, errlen, "mmap for data region failed");
            return -1;
        }
    }
    /* rebase provisional offsets to real addresses, then initialize */
    i = 0;
    for (struct ir_global *g = prog->globals; g; g = g->next, i++) {
        uint8_t *a = j->data + (size_t)j->globals[i].addr;
        j->globals[i].addr = a;
        if (g->inits) {
            /* An aggregate byte image (structs, designated/mixed initializers).
               Gaps stay zero from the anonymous mapping.  A scalar entry is
               written here little-endian; a symbol reference (it->sym) is a
               relocation resolved after code layout. */
            for (struct ir_init *it = g->inits; it; it = it->next) {
                if (it->sym)
                    continue;
                memcpy(a + (size_t)it->offset, &it->ival, (size_t)it->size);
            }
        } else if (g->init_string) {
            memcpy(a, g->init_string, (size_t)g->init_strlen);
        } else if (g->init_count > 0) {
            int es = base_size(g->base_type);
            for (int k = 0; k < g->init_count; k++)
                memcpy(a + (size_t)k * es, &g->init_ivals[k], (size_t)es);
        }
    }
    return 0;
}

/****************************************************************
 * Public API
 ****************************************************************/

int
kp_jit(struct kp_jit *j, struct ir_program *prog,
       const struct kp_binding *binds, int nbinds,
       char *err, size_t errlen)
{
    struct emitctx e;
    struct code c;
    int nf = 0, i, rc = 0;
    long pagesz;

    memset(j, 0, sizeof(*j));
    if (err && errlen)
        err[0] = '\0';

    if (layout_data(j, prog, err, errlen) != 0)
        return -1;

    memset(&e, 0, sizeof(e));
    e.j = j;
    e.prog = prog;
    e.binds = binds;
    e.nbinds = nbinds;
    e.err = err;
    e.errlen = errlen;

    for (struct ir_func *fn = prog->funcs; fn; fn = fn->next)
        nf++;
    j->funcs = calloc((size_t)(nf > 0 ? nf : 1), sizeof(*j->funcs));
    j->nfuncs = nf;

    code_init(&c);
    i = 0;
    for (struct ir_func *fn = prog->funcs; fn; fn = fn->next, i++) {
        regalloc(fn);
        j->funcs[i].name = fn->name;
        j->funcs[i].off = c.len;
        if (kp_target.emit_func(&e, &c, fn) != 0) {
            rc = -1;
            break;
        }
    }

    /* Hand the address-to-line table to the jit, which now owns e.lines;
       kp_jit_free releases it on any later error. */
    j->lines = e.lines;
    j->nlines = e.nlines;
    j->source_file = NULL;   /* -g line info not wired to the driver yet */
    e.lines = NULL;

    /* resolve deferred intra-module calls (target-relative, on the buffer) */
    for (int r = 0; r < e.nrelocs && rc == 0; r++) {
        size_t target = 0;
        int found = 0;
        for (int k = 0; k < j->nfuncs; k++)
            if (strcmp(j->funcs[k].name, e.relocs[r].target) == 0) {
                target = j->funcs[k].off;
                found = 1;
                break;
            }
        if (!found)
            rc = kp_fail(&e, "call to undefined function '%s'",
                         e.relocs[r].target, 0);
        else if (kp_target.apply_call_reloc(&c, e.relocs[r].site, target) != 0)
            rc = kp_fail(&e, "call target out of range for '%s'",
                         e.relocs[r].target, 0);
    }
    free(e.relocs);

    if (rc != 0) {
        free(e.absrelocs);
        code_free(&c);
        kp_jit_free(j);
        return -1;
    }

    /* copy the finished code into a low read-write mapping */
    pagesz = sysconf(_SC_PAGESIZE);
    j->code_len = c.len;
    j->code_map = ((c.len + (size_t)pagesz - 1) / (size_t)pagesz) * (size_t)pagesz;
    if (j->code_map == 0)
        j->code_map = (size_t)pagesz;
    j->code = kp_map_low(j->code_map);
    if (j->code == MAP_FAILED) {
        j->code = NULL;
        free(e.absrelocs);
        code_free(&c);
        kp_jit_free(j);
        snprintf(err, errlen, "mmap for code failed");
        return -1;
    }
    memcpy(j->code, c.buf, c.len);
    code_free(&c);
    /* patch absolute function-code references now that the base is known */
    for (int r = 0; r < e.nabs; r++) {
        uint64_t addr = 0;
        for (int k = 0; k < j->nfuncs; k++)
            if (strcmp(j->funcs[k].name, e.absrelocs[r].target) == 0) {
                addr = (uint64_t)(uintptr_t)(j->code + j->funcs[k].off);
                break;
            }
        kp_target.apply_abs_reloc(j->code, e.absrelocs[r].site, addr);
    }
    free(e.absrelocs);

    /* Data relocations: a global initialized with the address of another symbol
       (a pointer global, a function-pointer table).  The target address is
       known only now, so patch the image here.  Resolve against the globals,
       then the functions, then the host bindings, and write the low bytes (a
       32-bit address in the ILP32 model). */
    {
        int gi = 0;
        for (struct ir_global *g = prog->globals; g; g = g->next, gi++) {
            uint8_t *base = j->globals[gi].addr;
            uint64_t addr;

            for (struct ir_init *it = g->inits; it; it = it->next) {
                if (!it->sym)
                    continue;
                if (!resolve_data_sym(j, binds, nbinds, it->sym, &addr)) {
                    snprintf(err, errlen,
                             "data init references undefined symbol '%s'", it->sym);
                    kp_jit_free(j);
                    return -1;
                }
                memcpy(base + it->offset, &addr, (size_t)it->size);
            }
            if (g->init_count > 0 && g->init_syms) {
                int es = base_size(g->base_type);
                for (int k = 0; k < g->init_count; k++) {
                    if (!g->init_syms[k])
                        continue;
                    if (!resolve_data_sym(j, binds, nbinds, g->init_syms[k], &addr)) {
                        snprintf(err, errlen,
                                 "data init references undefined symbol '%s'",
                                 g->init_syms[k]);
                        kp_jit_free(j);
                        return -1;
                    }
                    memcpy(base + (size_t)k * es, &addr, (size_t)es);
                }
            }
        }
    }

    if (mprotect(j->code, j->code_map, PROT_READ | PROT_EXEC) != 0) {
        kp_jit_free(j);
        snprintf(err, errlen, "mprotect read-exec failed");
        return -1;
    }

    /* a 1 MB execution stack in low memory, so every frame address fits the 32
       bits the front end models addresses with */
    j->stack_len = 1u << 20;
    j->stack = kp_map_low(j->stack_len);
    if (j->stack == MAP_FAILED) {
        j->stack = NULL;
        kp_jit_free(j);
        snprintf(err, errlen, "mmap for stack failed");
        return -1;
    }
    kp_active = j;
    return 0;
}

/* The runtime arena a guest reaches through __moo_arena_alloc.  A driver that
   runs an Excelsior program binds the name "__moo_arena_alloc" to the function
   below; it serves the active JIT (kp_active).  A C-only guest never calls it,
   so the arena stays unmapped. */
void *
__moo_arena_alloc(int size)
{
    if (!kp_active)
        return NULL;
    return kp_arena_alloc(&kp_active->arena, size);
}

void *
kp_jit_entry(struct kp_jit *j, const char *name)
{
    for (int i = 0; i < j->nfuncs; i++)
        if (strcmp(j->funcs[i].name, name) == 0)
            return j->code + j->funcs[i].off;
    return NULL;
}

void
kp_jit_free(struct kp_jit *j)
{
    if (j->code)
        munmap(j->code, j->code_map);
    if (j->data)
        munmap(j->data, j->data_map);
    if (j->stack)
        munmap(j->stack, j->stack_len);
    kp_arena_free(&j->arena);
    if (kp_active == j)
        kp_active = NULL;
    free(j->globals);
    free(j->funcs);
    free(j->lines);
    memset(j, 0, sizeof(*j));
}
