/* x86_aot.c : the x86-64 AOT driver.

   Provides target_emit for the LP64 x86-64 tools.  It drives the shared
   instruction selector (backend/x86_select.c) over the NASM text sink
   (backend/mc_text.c): the per-function machine code is the selector's, and
   this file supplies the surrounding assembly a standalone object needs, the
   extern declarations, the data globals, and the section framing.  The
   selector emits the prologue, body, and epilogue of each function; this driver
   emits the label and section directives around it.

   The 32-bit x86 tools keep backend/x86_emit.c; this driver is the x86-64 (LP64)
   replacement for that file's per-opcode switch. */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir.h"
#include "mc_text.h"
#include "x86_select.h"

/****************************************************************
 * Globals
 ****************************************************************/

static void
emit_string_bytes(FILE *out, const char *s, int n)
{
    int k;

    fputs("\tdb ", out);
    for (k = 0; k < n; k++) {
        unsigned char c = (unsigned char)s[k];
        if (k > 0)
            fputs(", ", out);
        if (c >= 0x20 && c < 0x7f && c != '\'' && c != '\\')
            fprintf(out, "'%c'", c);
        else
            fprintf(out, "0x%02x", c);
    }
    fputc('\n', out);
}

/* an aggregate global's byte image: pad to each item's offset, emit the sized
   value (8-byte low dword first, little-endian), pad to the full size */
static void
emit_init_image(FILE *out, struct ir_global *g)
{
    struct ir_init *it;
    int cur = 0;

    for (it = g->inits; it; it = it->next) {
        if (it->offset > cur) {
            fprintf(out, "times %d db 0\n", it->offset - cur);
            cur = it->offset;
        }
        if (it->sym)
            fprintf(out, "\t%s %s\n", it->size == 8 ? "dq" : "dd", it->sym);
        else if (it->size == 8) {
            uint64_t b = (uint64_t)it->ival;
            fprintf(out, "\tdd 0x%08x\n", (unsigned)(b & 0xFFFFFFFF));
            fprintf(out, "\tdd 0x%08x\n", (unsigned)(b >> 32));
        } else if (it->size == 2)
            fprintf(out, "\tdw 0x%04x\n", (unsigned)(it->ival & 0xFFFF));
        else if (it->size == 1)
            fprintf(out, "\tdb 0x%02x\n", (unsigned)(it->ival & 0xFF));
        else
            fprintf(out, "\tdd 0x%08x\n", (unsigned)it->ival);
        cur += it->size;
    }
    if (g->arr_size > cur)
        fprintf(out, "times %d db 0\n", g->arr_size - cur);
}

static void
emit_globals(FILE *out, struct ir_program *prog)
{
    struct ir_global *g;

    fputs("\nsection .data\n", out);
    for (g = prog->globals; g; g = g->next) {
        int elsz;

        switch (g->base_type) {
        case IR_I8:  elsz = 1; break;
        case IR_I16: elsz = 2; break;
        case IR_F64: elsz = 8; break;
        case IR_I64: elsz = 8; break;
        default:     elsz = 4; break;
        }

        {
            /* natural alignment, raised by an _Alignas request (g->align) */
            int alb = (g->base_type == IR_F64 || g->base_type == IR_I64)
                      ? 8 : 4;
            if (g->align > alb)
                alb = g->align;
            fprintf(out, "align %d\n", alb);
        }
        if (!g->is_local)
            fprintf(out, "global %s\n", g->name);
        fprintf(out, "%s:\n", g->name);
        if (g->inits) {
            emit_init_image(out, g);
        } else if (g->init_string) {
            emit_string_bytes(out, g->init_string, g->init_strlen);
        } else if (g->init_count > 0) {
            int k;
            for (k = 0; k < g->init_count; k++) {
                if (g->init_syms && g->init_syms[k])
                    fprintf(out, "\t%s %s\n",
                        elsz == 8 ? "dq" : "dd", g->init_syms[k]);
                else if (g->base_type == IR_F64 || g->base_type == IR_I64) {
                    uint64_t bits = (uint64_t)g->init_ivals[k];
                    fprintf(out, "\tdd 0x%08x\n", (unsigned)(bits & 0xFFFFFFFF));
                    fprintf(out, "\tdd 0x%08x\n", (unsigned)(bits >> 32));
                } else if (elsz == 1)
                    fprintf(out, "\tdb %" PRId64 "\n", g->init_ivals[k]);
                else if (elsz == 2)
                    fprintf(out, "\tdw %" PRId64 "\n", g->init_ivals[k]);
                else
                    fprintf(out, "\tdd %" PRId64 "\n", g->init_ivals[k]);
            }
            if (g->arr_size > g->init_count)
                fprintf(out, "\ttimes %d db 0\n",
                    (g->arr_size - g->init_count) * elsz);
        } else {
            int sz = (g->arr_size > 0) ? g->arr_size * elsz : elsz;
            fprintf(out, "\ttimes %d db 0\n", sz);
        }
    }
}

/****************************************************************
 * Extern declarations (NASM requires explicit extern for undefined symbols)
 ****************************************************************/

static int
is_defined(struct ir_program *prog, const char *sym)
{
    struct ir_func *fn;
    struct ir_global *g;

    for (fn = prog->funcs; fn; fn = fn->next)
        if (strcmp(fn->name, sym) == 0)
            return 1;
    for (g = prog->globals; g; g = g->next)
        if (strcmp(g->name, sym) == 0)
            return 1;
    return 0;
}

static void
emit_extern_once(FILE *out, const char **seen, int *nseen, const char *sym)
{
    int k;

    for (k = 0; k < *nseen; k++)
        if (strcmp(seen[k], sym) == 0)
            return;
    if (*nseen < 256)
        seen[(*nseen)++] = sym;
    fprintf(out, "extern %s\n", sym);
}

static void
emit_externs(FILE *out, struct ir_program *prog)
{
    struct ir_func *fn;
    struct ir_insn *i;
    const char *seen[256];
    int nseen = 0;

    for (fn = prog->funcs; fn; fn = fn->next) {
        for (i = fn->head; i; i = i->next) {
            if (i->sym &&
                (i->op == IR_CALL || i->op == IR_TAILCALL ||
                 i->op == IR_FCALL || i->op == IR_CALL64 ||
                 i->op == IR_CALL_AGG ||
                 i->op == IR_LEA || i->op == IR_LEA64)) {
                /* a symbol-address load (IR_LEA/LEA64) can reference an external
                   data symbol, which NASM needs declared extern; is_defined
                   filters out the program's own globals and functions */
                if (!is_defined(prog, i->sym))
                    emit_extern_once(out, seen, &nseen, i->sym);
            }
            switch (i->op) {
            case IR_FLH:
                emit_extern_once(out, seen, &nseen, "__skj_extendhfsf");
                break;
            case IR_FSH:
                emit_extern_once(out, seen, &nseen, "__skj_truncsfhf");
                break;
            case IR_CAPTURE:
                emit_extern_once(out, seen, &nseen, "__cont_capture");
                break;
            case IR_RESUME:
                emit_extern_once(out, seen, &nseen, "__cont_resume");
                break;
            case IR_MARK:
                emit_extern_once(out, seen, &nseen, "__cont_mark_sp");
                emit_extern_once(out, seen, &nseen, "__cont_arena_ptr");
                break;
            case IR_CONT_UNWIND:
                emit_extern_once(out, seen, &nseen, "__cont_arena_ptr");
                break;
            default:
                break;
            }
        }
    }
}

/****************************************************************
 * Entry point
 ****************************************************************/

void
target_emit(FILE *out, struct ir_program *prog)
{
    struct ir_func *fn;
    struct mc *m;
    int serial = 0;

    fputs("; generated x86-64 (LP64) assembly, NASM syntax\n", out);
    emit_externs(out, prog);

    m = mct_new(out);
    if (!m) {
        fputs("x86_aot: out of memory\n", stderr);
        exit(1);
    }
    for (fn = prog->funcs; fn; fn = fn->next) {
        char err[128];
        int rc;

        fputs("\nsection .text\n", out);
        if (!fn->is_local)
            fprintf(out, "global %s\n", fn->name);
        fprintf(out, "%s:\n", fn->name);

        mct_begin_func(m, ++serial);
        rc = x86_select_func(m, fn, TRAP_HARDWARE, err, sizeof err);
        if (rc != 0) {
            fprintf(stderr, "x86_aot: %s\n", err);
            exit(1);
        }
        if (mct_errsym(m)) {
            fprintf(stderr, "x86_aot: unresolved symbol '%s'\n", mct_errsym(m));
            exit(1);
        }
    }
    mct_free(m);

    emit_globals(out, prog);
    /* mark the stack non-executable so the GNU linker does not warn (and
       default to an executable stack) about the missing note */
    fputs("\nsection .note.GNU-stack noalloc noexec nowrite progbits\n", out);
}
