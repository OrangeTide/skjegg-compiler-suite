/* kpm_write.c : writer for the KPM1 text module format, the inverse of kpm.c.

   Serializes a kpm_module back to the KPM1 text image kpm_read parses.  The
   output is canonical: a field at its ir_emit default (index -1, imm/nargs 0,
   NULL sym) is left out, since the reader restores that default, and the
   operands emitted mirror the keys set_operands reads.  It need not match any
   front end's historical byte formatting; the contract is that kpm_read
   restores the same module and that the front end's unit loader reads the
   .interface block. */

#include "kpm.h"
#include "ir.h"

#include <stdio.h>
#include <string.h>

static const char *
base_type_name(int bt)
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

/* Write s as a KPM quoted string body, matching the reader's unescape: the
   backslash escapes it decodes, printable bytes verbatim, anything else \xHH. */
static void
write_quoted(FILE *out, const char *s, int len)
{
    putc('"', out);
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':  fputs("\\\"", out); break;
        case '\\': fputs("\\\\", out); break;
        case '\n': fputs("\\n", out); break;
        case '\t': fputs("\\t", out); break;
        case '\r': fputs("\\r", out); break;
        default:
            if (c >= 0x20 && c <= 0x7e)
                putc((int)c, out);
            else
                fprintf(out, "\\x%02x", c);
        }
    }
    putc('"', out);
}

/* The ( class... ) -> ret signature tail shared by imports and methods. */
static void
write_signature(FILE *out, char **param_cls, int nparams, const char *ret_cls)
{
    fputs("(", out);
    for (int i = 0; i < nparams; i++)
        fprintf(out, " %s", param_cls[i] ? param_cls[i] : "void");
    fprintf(out, " ) -> %s", ret_cls ? ret_cls : "void");
}

static void
write_import(FILE *out, const struct kp_import *imp)
{
    if (imp->kind == KP_IMPORT_TYPE) {
        fprintf(out, ".import type %s methods %d\n", imp->name, imp->nmethods);
        for (int i = 0; i < imp->nmethods; i++) {
            const struct kp_method *me = &imp->methods[i];
            fprintf(out, "    .method %d %s ", i, me->name);
            write_signature(out, me->param_cls, me->nparams, me->ret_cls);
            putc('\n', out);
        }
        return;
    }
    fprintf(out, ".import %s %s ",
            imp->kind == KP_IMPORT_HELPER ? "helper" : "func", imp->name);
    write_signature(out, imp->param_cls, imp->nparams, imp->ret_cls);
    putc('\n', out);
}

static void
write_global(FILE *out, const struct ir_global *g)
{
    fprintf(out, ".global %s %s", g->name, base_type_name(g->base_type));
    if (g->arr_size > 0)
        fprintf(out, " array %d", g->arr_size);
    if (g->align > 0)
        fprintf(out, " align %d", g->align);
    fputs(g->is_export ? " export" : " local", out);
    if (g->is_ptr)
        fputs(" ptr", out);
    if (g->is_rodata)
        fputs(" rodata", out);
    if (g->is_strobj)
        fputs(" strobj", out);
    if (g->from_unit)
        fprintf(out, " from %s", g->from_unit);
    putc('\n', out);
    for (int i = 0; i < g->init_count; i++) {
        if (g->init_syms && g->init_syms[i])
            fprintf(out, "    .init sym %s\n", g->init_syms[i]);
        else
            fprintf(out, "    .init %lld\n",
                    (long long)(g->init_ivals ? g->init_ivals[i] : 0));
    }
    if (g->init_string) {
        fputs("    .string ", out);
        write_quoted(out, g->init_string, g->init_strlen);
        putc('\n', out);
    }
    fputs(".endglobal\n", out);
}

static void
write_asm(FILE *out, const struct ir_insn *in)
{
    const struct ir_asm_info *info = in->asm_info;
    int nops;

    if (!info) {
        /* basic asm: the verbatim template is the whole quoted body */
        fputs("    ASM ", out);
        write_quoted(out, in->sym, in->sym ? (int)strlen(in->sym) : 0);
        putc('\n', out);
        return;
    }
    nops = info->noutputs + info->ninputs;
    fprintf(out, "    ASM nout=%d nin=%d vol=%d ",
            info->noutputs, info->ninputs, info->is_volatile);
    for (int i = 0; i < nops; i++) {
        const struct ir_asm_op *op = &info->ops[i];
        char role = op->is_inout ? 'b' : (op->is_output ? 'o' : 'i');
        fprintf(out, "o%d=%c,%s,%d,%d ", i, role,
                op->constraint ? op->constraint : "", op->temp, op->size);
    }
    for (int i = 0; i < info->nclobbers; i++)
        fprintf(out, "c%d=%s ", i, info->clobbers[i]);
    write_quoted(out, in->sym, in->sym ? (int)strlen(in->sym) : 0);
    putc('\n', out);
}

static void
write_insn(FILE *out, const struct ir_insn *in)
{
    const char *mn;

    if (in->op == IR_ASM) {
        write_asm(out, in);
        return;
    }
    mn = kpm_op_name(in->op);
    fprintf(out, "    %s", mn ? mn : "NOP");
    if (in->dst >= 0)
        fprintf(out, " dst=%d", in->dst);
    if (in->a >= 0)
        fprintf(out, " a=%d", in->a);
    if (in->b >= 0)
        fprintf(out, " b=%d", in->b);
    if (in->imm != 0)
        fprintf(out, " imm=%lld", (long long)in->imm);
    if (in->slot >= 0)
        fprintf(out, " slot=%d", in->slot);
    if (in->label >= 0)
        fprintf(out, " label=%d", in->label);
    if (in->nargs != 0)
        fprintf(out, " nargs=%d", in->nargs);
    if (in->sym)
        fprintf(out, " sym=%s", in->sym);
    putc('\n', out);
}

static void
write_func(FILE *out, const struct ir_func *fn)
{
    const struct ir_insn *in;

    fprintf(out, ".func %s", fn->name);
    if (fn->is_local)
        fputs(" local", out);
    if (fn->is_variadic)
        fputs(" variadic", out);
    if (fn->from_unit)
        fprintf(out, " from %s", fn->from_unit);
    fprintf(out, " params %d slots %d temps %d labels %d\n",
            fn->nparams, fn->nslots, fn->ntemps, fn->nlabels);
    if (fn->nslots > 0 && fn->slot_size) {
        fputs("    .slotsize", out);
        for (int i = 0; i < fn->nslots; i++)
            fprintf(out, " %d", fn->slot_size[i]);
        putc('\n', out);
    }
    if (fn->nparams > 0 && fn->param_class) {
        fputs("    .paramclass", out);
        for (int i = 0; i < fn->nparams; i++)
            fprintf(out, " %d", fn->param_class[i]);
        putc('\n', out);
    }
    if (fn->nparams > 0 && fn->param_fw) {
        int any = 0;
        for (int i = 0; i < fn->nparams; i++)
            if (fn->param_fw[i])
                any = 1;
        if (any) {
            fputs("    .paramfw", out);
            for (int i = 0; i < fn->nparams; i++)
                fprintf(out, " %d", (int)fn->param_fw[i]);
            putc('\n', out);
        }
    }
    for (in = fn->head; in; in = in->next)
        write_insn(out, in);
    fputs(".endfunc\n", out);
}

int
kpm_write(FILE *out, const struct kpm_module *m)
{
    const struct ir_global *g;
    const struct ir_func *fn;

    fputs(".kpm 1\n", out);
    if (m->target)
        fprintf(out, ".target %s\n", m->target);
    switch (m->kind) {
    case KP_MOD_LIBRARY:
        fprintf(out, ".module library%s%s\n", m->name ? " " : "",
                m->name ? m->name : "");
        break;
    case KP_MOD_EXECUTABLE:
        fputs(".module executable\n", out);
        break;
    default:
        fprintf(out, ".module object%s%s\n", m->name ? " " : "",
                m->name ? m->name : "");
        break;
    }
    if (m->entry)
        fprintf(out, ".entry %s\n", m->entry);
    for (int i = 0; i < m->ndeps; i++)
        fprintf(out, ".uses %s fingerprint %lu\n", m->deps[i].name,
                (unsigned long)m->deps[i].fingerprint);
    if (m->source_file) {
        fputs(".file ", out);
        write_quoted(out, m->source_file, (int)strlen(m->source_file));
        putc('\n', out);
    }
    if (m->nimports > 0) {
        putc('\n', out);
        for (int i = 0; i < m->nimports; i++)
            write_import(out, &m->imports[i]);
    }
    if (m->nexports > 0) {
        putc('\n', out);
        for (int i = 0; i < m->nexports; i++)
            fprintf(out, ".export %s\n", m->exports[i]);
    }
    if (m->prog.globals) {
        putc('\n', out);
        for (g = m->prog.globals; g; g = g->next)
            write_global(out, g);
    }
    if (m->niface_lines > 0 || m->iface_fp != 0) {
        putc('\n', out);
        fprintf(out, ".interface fingerprint %lu\n", (unsigned long)m->iface_fp);
        for (int i = 0; i < m->niface_lines; i++)
            fprintf(out, "%s\n", m->iface_lines[i]);
        fputs(".endinterface\n", out);
    }
    for (fn = m->prog.funcs; fn; fn = fn->next) {
        putc('\n', out);
        write_func(out, fn);
    }
    return ferror(out) ? -1 : 0;
}
