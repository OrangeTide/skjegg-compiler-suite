/* main.c : Excelsior reader driver (lex + parse + AST dump).
 *
 * The type checker and IR lowering are a later phase; this driver
 * exercises the reader only.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "excelsior.h"
#include "ir.h"
#include "version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
cleanup_arena(void *p)
{
    arena_free(p);
}

static void
cleanup_free(void *p)
{
    free(p);
}

/****************************************************************
 * Token dump
 ****************************************************************/

static void
dump_tokens(struct arena *a, const char *src, const char *filename, int trace)
{
    struct token t;

    lex_init(a, src, filename, trace);
    for (;;) {
        t = lex_next();
        if (t.kind == T_EOF)
            break;
        printf("%4d  %-12s", t.line, tok_str(t.kind));
        if (t.kind == T_IDENT || t.kind == T_STRING || t.kind == T_TRACE_COMMENT)
            printf("  %s", t.sval ? t.sval : "");
        else if (t.kind == T_NUMBER)
            printf("  %ld", t.nval);
        else if (t.kind == T_FLOAT_LIT)
            printf("  %g", t.fval);
        putchar('\n');
    }
}

/****************************************************************
 * AST dump
 ****************************************************************/

static void
print_type(struct ex_type *t)
{
    if (!t) {
        printf("?");
        return;
    }
    switch (t->kind) {
    case ET_ANY:   printf("any"); break;
    case ET_NIL:   printf("nil"); break;
    case T_TINT:   printf("int"); break;
    case T_TFLOAT: printf("float"); break;
    case T_TDEC:   printf("decimal"); break;
    case T_TVEC:   printf("vec"); break;
    case T_TMAT:   printf("mat"); break;
    case T_TSTR:   printf("str"); break;
    case T_TOBJ:   printf("obj"); break;
    case T_TBOOL:  printf("bool"); break;
    case T_TERR:   printf("err"); break;
    case T_TPROP:  printf("prop"); break;
    case T_TLIST:  printf("list<"); print_type(t->inner); printf(">"); break;
    case ET_MAYBE: printf("maybe "); print_type(t->inner); break;
    case T_IDENT:  printf("%s", t->name); break;
    default:       printf("?type?");
    }
}

static void
ind(int depth)
{
    for (int i = 0; i < depth; i++)
        printf("  ");
}

static void dump(struct node *n, int depth);

static void
dump_list(struct node *n, int depth)
{
    for (; n; n = n->next)
        dump(n, depth);
}

static void
dump(struct node *n, int depth)
{
    if (!n)
        return;

    ind(depth);
    switch (n->kind) {
    case N_FILE:
        printf("FILE\n");
        dump_list(n->a, depth + 1);
        break;
    case N_USE:
        printf("USE %s", n->name);
        if (n->alias)
            printf(" as %s", n->alias);
        printf("\n");
        if (n->a) {
            ind(depth + 1); printf("names:");
            for (struct node *c = n->a; c; c = c->next)
                printf(" %s", c->name);
            printf("\n");
        }
        break;
    case N_CONST:
        printf("CONST %s", n->name);
        if (n->type) { printf(" is "); print_type(n->type); }
        printf("\n");
        dump(n->a, depth + 1);
        break;
    case N_CLASS:
        printf("CLASS %s", n->name);
        if (n->alias)
            printf(" is %s", n->alias);
        printf("\n");
        dump_list(n->a, depth + 1);
        break;
    case N_ENUM:
        printf("ENUM %s:", n->name);
        for (struct node *c = n->a; c; c = c->next)
            printf(" %s", c->name);
        printf("\n");
        break;
    case N_RECORD:
        printf("RECORD %s\n", n->name);
        dump_list(n->a, depth + 1);
        break;
    case N_FIELD:
        printf("FIELD %s%s%s is ",
               (n->flags & NF_PUBLIC) ? "pub " : "priv ",
               (n->flags & NF_TUNABLE) ? "tunable " : "", n->name);
        print_type(n->type);
        printf("\n");
        if (n->a) { ind(depth + 1); printf("default:\n"); dump(n->a, depth + 2); }
        break;
    case N_VERB:
    case N_FUNC:
        printf("%s %s%s", n->kind == N_VERB ? "VERB" : "FUNC",
               (n->flags & NF_PUBLIC) ? "" : "(priv) ", n->name);
        if (n->type) { printf(" -> "); print_type(n->type); }
        printf("\n");
        dump_list(n->a, depth + 1);          /* params */
        if (n->b) { ind(depth + 1); printf("body:\n"); dump_list(n->b, depth + 2); }
        else { ind(depth + 1); printf("(signature)\n"); }
        break;
    case N_PARAM:
        printf("PARAM %s is ", n->name);
        print_type(n->type);
        printf("\n");
        break;
    case N_DISCLOSE:
        printf("DISCLOSE:");
        for (struct node *c = n->a; c; c = c->next) {
            printf(" %s", c->name);
            if (c->alias)
                printf(":%s", c->alias);
        }
        printf("\n");
        break;
    case N_VAR:
        printf("VAR %s", n->name);
        if (n->type) { printf(" is "); print_type(n->type); }
        printf("\n");
        dump(n->a, depth + 1);
        break;
    case N_ASSIGN:
        printf("ASSIGN %s\n", tok_str(n->op));
        dump(n->a, depth + 1);
        dump(n->b, depth + 1);
        break;
    case N_IF:
        printf("IF\n");
        ind(depth + 1); printf("cond:\n"); dump(n->a, depth + 2);
        ind(depth + 1); printf("then:\n"); dump_list(n->b, depth + 2);
        if (n->c) {
            ind(depth + 1);
            printf(n->c->kind == N_IF ? "elseif:\n" : "else:\n");
            if (n->c->kind == N_IF)
                dump(n->c, depth + 2);
            else
                dump_list(n->c, depth + 2);
        }
        break;
    case N_FOR:
        printf("FOR %s %s\n", n->name, n->b ? "(range)" : "(iter)");
        dump(n->a, depth + 1);
        if (n->b) dump(n->b, depth + 1);
        ind(depth + 1); printf("body:\n"); dump_list(n->c, depth + 2);
        break;
    case N_WHILE:
        printf("WHILE\n");
        ind(depth + 1); printf("cond:\n"); dump(n->a, depth + 2);
        ind(depth + 1); printf("body:\n"); dump_list(n->b, depth + 2);
        break;
    case N_MATCH:
    case N_MATCHEXPR:
        printf(n->kind == N_MATCH ? "MATCH\n" : "MATCHEXPR\n");
        ind(depth + 1); printf("subject:\n"); dump(n->a, depth + 2);
        dump_list(n->b, depth + 1);
        if (n->c) { ind(depth + 1); printf("else:\n"); dump_list(n->c, depth + 2); }
        break;
    case N_MATCHARM:
        printf("ARM\n");
        ind(depth + 1); printf("vals:\n"); dump_list(n->a, depth + 2);
        ind(depth + 1); printf("body:\n"); dump_list(n->b, depth + 2);
        break;
    case N_RANGE:
        printf("RANGE\n");
        dump(n->a, depth + 1);
        dump(n->b, depth + 1);
        break;
    case N_RETURN:
        printf("RETURN\n");
        dump(n->a, depth + 1);
        break;
    case N_NOTHING:  printf("NOTHING\n"); break;
    case N_FAIL:     printf("FAIL\n"); break;
    case N_BREAK:    printf("BREAK\n"); break;
    case N_CONTINUE: printf("CONTINUE\n"); break;
    case N_TRACE:
        printf("TRACE\n");
        dump(n->a, depth + 1);
        break;
    case N_TRACE_CMT:
        printf("/// %s\n", n->sval);
        break;
    case N_EXPR_STMT:
        printf("EXPR\n");
        dump(n->a, depth + 1);
        break;
    case N_BINOP:
        printf("BINOP %s\n", tok_str(n->op));
        dump(n->a, depth + 1);
        dump(n->b, depth + 1);
        break;
    case N_CMPCHAIN:
        printf("CMPCHAIN\n");
        dump(n->a, depth + 1);
        for (struct node *l = n->b; l; l = l->next) {
            ind(depth + 1); printf("%s\n", tok_str(l->op));
            dump(l->a, depth + 2);
        }
        break;
    case N_UNOP:
        printf("UNOP %s\n", tok_str(n->op));
        dump(n->a, depth + 1);
        break;
    case N_FIELD_ACC:
        printf("FIELD .%s", n->name);
        if (n->sym)
            printf(" [%s]", sym_kind_name(n->sym->kind));
        printf("\n");
        dump(n->a, depth + 1);
        break;
    case N_SEND:
        printf("SEND :%s\n", n->name);
        ind(depth + 1); printf("recv:\n"); dump(n->a, depth + 2);
        if (n->b) { ind(depth + 1); printf("args:\n"); dump_list(n->b, depth + 2); }
        break;
    case N_CALL:
        printf("CALL\n");
        ind(depth + 1); printf("callee:\n"); dump(n->a, depth + 2);
        if (n->b) { ind(depth + 1); printf("args:\n"); dump_list(n->b, depth + 2); }
        break;
    case N_INDEX:
        printf("INDEX\n");
        dump(n->a, depth + 1);
        dump(n->b, depth + 1);
        break;
    case N_SLICE:
        printf("SLICE\n");
        dump(n->a, depth + 1);
        dump(n->b, depth + 1);
        dump(n->c, depth + 1);
        break;
    case N_CAST:
        printf("CAST as ");
        print_type(n->type);
        printf("\n");
        dump(n->a, depth + 1);
        break;
    case N_ISTEST:
        printf("IS %s\n", n->name);
        dump(n->a, depth + 1);
        break;
    case N_NAME:
        printf("NAME %s", n->name);
        if (n->sym)
            printf(" [%s]", sym_kind_name(n->sym->kind));
        printf("\n");
        break;
    case N_NUM:   printf("NUM %ld\n", n->ival); break;
    case N_FLOAT: printf("DEC %g\n", n->fval); break;
    case N_STR:   printf("STR \"%s\"\n", n->sval); break;
    case N_BOOL:  printf("BOOL %s\n", n->ival ? "true" : "false"); break;
    case N_NIL:   printf("NIL\n"); break;
    case N_SELF:  printf("SELF\n"); break;
    case N_DATALIT:
        printf("DATA\n");
        dump_list(n->a, depth + 1);
        break;
    case N_DATAITEM:
        printf("ITEM %s\n", tok_str(n->op));
        break;
    case N_HOLE:
        printf("HOLE\n");
        dump(n->a, depth + 1);
        break;
    case N_QUOTE:
        printf("QUOTE\n");
        dump(n->a, depth + 1);
        break;
    case N_SHAPE:
        printf("SHAPE %s\n", n->name);
        for (struct node *k = n->a; k; k = k->next) {
            ind(depth + 1);
            printf("%s%s:", k->name,
                   (k->flags & NF_SHAPE_GROUP) ? " (group)" : "");
            for (struct node *s = k->a; s; s = s->next)
                printf(" %s%s", s->name,
                       s->op == T_PLUS ? "+" : s->op == T_STAR ? "*" : "");
            printf("\n");
        }
        break;
    default:
        printf("?node(%d)?\n", n->kind);
    }
}

/****************************************************************
 * Usage and main
 ****************************************************************/

static void
usage(const char *prog)
{
    fprintf(stderr, "usage: %s [options] <file.exs>\n", prog);
    fprintf(stderr, "  -t          enable trace comments\n");
    fprintf(stderr, "  -V          show version\n");
    fprintf(stderr, "  --tokens    dump the token stream and exit\n");
    fprintf(stderr, "  --ast       parse, resolve, and dump the AST\n");
    fprintf(stderr, "  --symbols   dump the resolved symbol table\n");
    fprintf(stderr, "  -S          compile and emit assembly\n");
    fprintf(stderr, "  -o <file>   write output to <file> (implies -S)\n");
    exit(1);
}

int
main(int argc, char **argv)
{
    /* declared after setjmp so longjmp cannot clobber them (-Wclobbered) */
    const char *filename;
    const char *outfile;
    char *src;
    struct node *ast;
    int trace, tokens_only, ast_dump, symbols_only, emit_asm;

    util_set_progname("skj-exc");

    if (setjmp(util_die_env) != 0)
        return 1;
    util_die_active = 1;

    filename = NULL;
    outfile = NULL;
    trace = 0;
    tokens_only = 0;
    ast_dump = 0;
    symbols_only = 0;
    emit_asm = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-V") == 0) {
            printf("skj-exc %s\n", SKJ_VERSION);
            return 0;
        } else if (strcmp(argv[i], "-t") == 0) {
            trace = 1;
        } else if (strcmp(argv[i], "--tokens") == 0) {
            tokens_only = 1;
        } else if (strcmp(argv[i], "--ast") == 0) {
            ast_dump = 1;
        } else if (strcmp(argv[i], "--symbols") == 0) {
            symbols_only = 1;
        } else if (strcmp(argv[i], "-S") == 0) {
            emit_asm = 1;
        } else if (strcmp(argv[i], "-o") == 0) {
            if (++i >= argc)
                usage(argv[0]);
            outfile = argv[i];
            emit_asm = 1;
        } else if (argv[i][0] == '-') {
            usage(argv[0]);
        } else {
            if (filename)
                usage(argv[0]);
            filename = argv[i];
        }
    }
    if (!filename)
        usage(argv[0]);

    src = slurp(filename);
    util_cleanup_push(cleanup_free, src);

    {
        struct arena a;

        arena_init(&a);
        util_cleanup_push(cleanup_arena, &a);

        if (tokens_only) {
            dump_tokens(&a, src, filename, trace);
            util_cleanup_run();
            return 0;
        }

        lex_init(&a, src, filename, trace);
        ast = parse_program(&a);
        resolve_program(&a, ast);
        typecheck_program(&a, ast);

        if (ast_dump) {
            dump(ast, 0);
        } else if (symbols_only) {
            dump_symbols(ast);
        } else if (emit_asm) {
            struct ir_program *prog = lower_program(&a, ast);
            FILE *out = stdout;

            for (struct ir_func *fn = prog->funcs; fn; fn = fn->next)
                regalloc(fn);
            if (outfile) {
                out = fopen(outfile, "w");
                if (!out)
                    die("cannot open '%s' for writing", outfile);
            }
            target_emit(out, prog);
            if (outfile)
                fclose(out);
        } else {
            printf("%s: parsed, resolved, and type-checked ok\n", filename);
        }

        util_cleanup_run();
    }
    return 0;
}
