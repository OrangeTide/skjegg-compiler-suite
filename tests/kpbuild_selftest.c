/* kpbuild_selftest.c : exercise the in-memory module builder (kpbuild.c)
   against real modules, without the front end.

   Reads a .kpm, replays its every element through the kpb_* builder entry
   points to reconstruct the module in memory, then checks the assembly
   kp_aot_emit produces for the rebuilt module byte-matches the assembly for the
   module parsed straight from the file.  A match means the builder carries
   everything the backend reads, so the front-end cutover can target it with
   confidence.  Uses open_memstream to capture both emissions. */

#define _POSIX_C_SOURCE 200809L

#include "kpm.h"
#include "kpbuild.h"
#include "kp_aot.h"
#include "ir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *
base_name(int bt)
{
    switch (bt) {
    case IR_I8:  return "i8";
    case IR_I16: return "i16";
    case IR_I32: return "i32";
    case IR_I64: return "i64";
    case IR_F32: return "f32";
    case IR_F64: return "f64";
    default:     return "i32";
    }
}

static char *
rt_slurp(const char *path, size_t *lenp)
{
    FILE *f = fopen(path, "rb");
    long sz;
    char *buf;

    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    buf = malloc((size_t)sz + 1);
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    buf[sz] = '\0';
    *lenp = (size_t)sz;
    return buf;
}

/* Drive the builder from a parsed module, reconstructing it element by element. */
static void
replay(const struct kpm_module *m)
{
    kpb_begin();
    if (m->target)
        kpb_target(m->target);
    kpb_kind(m->kind, m->name);
    if (m->entry)
        kpb_entry(m->entry);
    if (m->source_file)
        kpb_file(m->source_file);
    for (int i = 0; i < m->ndeps; i++)
        kpb_dep(m->deps[i].name, m->deps[i].fingerprint);
    for (int i = 0; i < m->nimports; i++) {
        const struct kp_import *imp = &m->imports[i];
        kpb_import_begin(imp->kind, imp->name);
        if (imp->kind == KP_IMPORT_TYPE) {
            for (int j = 0; j < imp->nmethods; j++) {
                const struct kp_method *me = &imp->methods[j];
                kpb_method_begin(me->name);
                for (int k = 0; k < me->nparams; k++)
                    kpb_method_param(me->param_cls[k]);
                kpb_method_ret(me->ret_cls);
            }
        } else {
            for (int k = 0; k < imp->nparams; k++)
                kpb_import_param(imp->param_cls[k]);
            kpb_import_ret(imp->ret_cls);
        }
    }
    for (int i = 0; i < m->nexports; i++)
        kpb_export(m->exports[i]);
    for (struct ir_global *g = m->prog.globals; g; g = g->next) {
        int flags = 0;
        if (g->is_export) flags |= KPB_G_EXPORT;
        if (g->is_ptr)    flags |= KPB_G_PTR;
        if (g->is_rodata) flags |= KPB_G_RODATA;
        if (g->is_strobj) flags |= KPB_G_STROBJ;
        kpb_global_begin(g->name, base_name(g->base_type), g->arr_size, g->align, flags);
        for (int k = 0; k < g->init_count; k++) {
            if (g->init_syms && g->init_syms[k])
                kpb_global_init_sym(g->init_syms[k]);
            else
                kpb_global_init_int((long long)(g->init_ivals ? g->init_ivals[k] : 0));
        }
        if (g->init_string)
            kpb_global_string(g->init_string, g->init_strlen);
        kpb_global_end();
    }
    for (struct ir_func *fn = m->prog.funcs; fn; fn = fn->next) {
        kpb_func_begin(fn->name, fn->is_local, fn->is_variadic,
                       fn->nparams, fn->nslots, fn->ntemps, fn->nlabels);
        for (int i = 0; i < fn->nslots; i++)
            kpb_slotsize(fn->slot_size ? fn->slot_size[i] : 0);
        if (fn->param_class)
            for (int i = 0; i < fn->nparams; i++)
                kpb_paramclass(fn->param_class[i]);
        if (fn->param_fw) {
            int any = 0;
            for (int i = 0; i < fn->nparams; i++)
                if (fn->param_fw[i]) any = 1;
            if (any)
                for (int i = 0; i < fn->nparams; i++)
                    kpb_paramfw((int)fn->param_fw[i]);
        }
        for (struct ir_insn *in = fn->head; in; in = in->next) {
            if (in->op == IR_ASM) {
                if (!in->asm_info) {
                    kpb_asm_basic(in->sym);
                } else {
                    struct ir_asm_info *ai = in->asm_info;
                    int nops = ai->noutputs + ai->ninputs;
                    kpb_asm_ext_begin(ai->noutputs, ai->ninputs, ai->is_volatile);
                    for (int k = 0; k < nops; k++) {
                        struct ir_asm_op *op = &ai->ops[k];
                        int role = op->is_inout ? KPB_ASM_INOUT
                                 : (op->is_output ? KPB_ASM_OUT : KPB_ASM_IN);
                        kpb_asm_op(role, op->constraint, op->temp, op->size);
                    }
                    for (int k = 0; k < ai->nclobbers; k++)
                        kpb_asm_clobber(ai->clobbers[k]);
                    kpb_asm_ext_end(in->sym);
                }
            } else {
                kpb_insn(kpm_op_name(in->op), in->dst, in->a, in->b, in->imm,
                         in->slot, in->label, in->nargs, in->sym);
            }
        }
        kpb_func_end();
    }
}

static char *
emit_to_buf(struct kpm_module *m, size_t *lenp)
{
    char *buf = NULL;
    size_t sz = 0;
    FILE *f = open_memstream(&buf, &sz);
    if (!f)
        return NULL;
    kp_aot_emit(m, f);
    fclose(f);
    *lenp = sz;
    return buf;
}

int
main(int argc, char **argv)
{
    char *text, *asmA, *asmB;
    size_t len, lenA, lenB;
    struct kpm_module m;
    char err[256];

    if (argc < 2) {
        fprintf(stderr, "usage: kpbuild_selftest module.kpm\n");
        return 2;
    }
    text = rt_slurp(argv[1], &len);
    if (!text) {
        fprintf(stderr, "kpbuild_selftest: cannot read '%s'\n", argv[1]);
        return 1;
    }
    if (kpm_read(&m, text, len, err, sizeof err) != 0) {
        fprintf(stderr, "kpbuild_selftest: %s\n", err);
        return 1;
    }

    /* Rebuild from the pristine module first, then emit both. */
    replay(&m);
    asmB = emit_to_buf(kpb_module_ptr(), &lenB);
    asmA = emit_to_buf(&m, &lenA);

    if (!asmA || !asmB) {
        fprintf(stderr, "kpbuild_selftest: emit failed\n");
        return 1;
    }
    if (lenA != lenB || memcmp(asmA, asmB, lenA) != 0) {
        fprintf(stderr, "kpbuild_selftest: asm mismatch for %s\n", argv[1]);
        return 1;
    }
    return 0;
}
