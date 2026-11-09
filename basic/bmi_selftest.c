/* bmi_selftest.c : a host unit test for the .bmi reader (bmi_read). It parses a
   sample module interface and checks every field, so the reader the module
   consumer will depend on is exercised on its own. Run via `make test-basic-bmi`. */

#include "basic.h"
#include "util.h"

#include <setjmp.h>
#include <string.h>
#include <stdio.h>

static const char sample[] =
    ".bmi 1\n"
    "; a comment line\n"
    ".target lp64 little\n"
    ".module Geometry\n"
    ".const MaxSides i64 = 32\n"
    ".const Depth i64 = -3\n"
    ".type Circle handle\n"
    ".func Make link geom_make ( f64 ) -> handle Circle\n"
    ".func Area link geom_area ( handle Circle ) -> f64\n"
    ".proc Scale link geom_scale ( handle Circle ref f64 )\n"
    ".func Count link geom_count ( ) -> i64\n";

#define CHECK(c) \
    do { if (!(c)) { fprintf(stderr, "bmi selftest failed: %s\n", #c); return 1; } } while (0)

int
main(void)
{
    struct arena a;
    struct bmi *m;
    struct bmi_sym *s;

    if (setjmp(util_die_env) != 0)
        return 1;
    util_die_active = 1;
    util_set_progname("bmi_selftest");

    arena_init(&a);
    m = bmi_read(&a, sample, sizeof(sample) - 1, "sample");

    CHECK(strcmp(m->module, "Geometry") == 0);
    CHECK(m->lp64 == 1);
    CHECK(m->big_endian == 0);

    s = m->syms;
    CHECK(s && s->kind == BMI_CONST && strcmp(s->name, "MaxSides") == 0 && s->cval == 32);
    s = s->next;
    CHECK(s && s->kind == BMI_CONST && strcmp(s->name, "Depth") == 0 && s->cval == -3);
    s = s->next;
    CHECK(s && s->kind == BMI_TYPE && strcmp(s->name, "Circle") == 0);
    s = s->next;
    CHECK(s && s->kind == BMI_FUNC && strcmp(s->name, "Make") == 0);
    CHECK(s->nparam == 1 && s->param[0] == BMI_F64);
    CHECK(s->ret == BMI_HANDLE && s->ret_type && strcmp(s->ret_type, "Circle") == 0);
    s = s->next;
    CHECK(s && s->kind == BMI_FUNC && strcmp(s->name, "Area") == 0);
    CHECK(strcmp(s->link, "geom_area") == 0);
    CHECK(s->nparam == 1 && s->param[0] == BMI_HANDLE && s->param_ref[0] == 0);
    CHECK(s->param_type[0] && strcmp(s->param_type[0], "Circle") == 0);
    CHECK(s->ret == BMI_F64);
    s = s->next;
    CHECK(s && s->kind == BMI_PROC && strcmp(s->name, "Scale") == 0);
    CHECK(s->nparam == 2);
    CHECK(s->param[0] == BMI_HANDLE && s->param_ref[0] == 0);
    CHECK(s->param_type[0] && strcmp(s->param_type[0], "Circle") == 0);
    CHECK(s->param[1] == BMI_F64 && s->param_ref[1] == 1);
    CHECK(s->ret == BMI_VOID);
    s = s->next;
    CHECK(s && s->kind == BMI_FUNC && strcmp(s->name, "Count") == 0);
    CHECK(s->nparam == 0 && s->ret == BMI_I64);
    s = s->next;
    CHECK(s == NULL);

    arena_free(&a);
    printf("bmi selftest: ok\n");
    return 0;
}
