/* ir.h : portable compiler IR data structures, builder API, and shared declarations */

#ifndef IR_H
#define IR_H

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "arena.h"
#include "util.h"

/****************************************************************
 * Base types
 ****************************************************************/

enum ir_basetype {
    IR_I32 = 1,
    IR_I8  = 2,
    IR_I16 = 3,
    IR_F64 = 4,
    IR_I64 = 5,
};

/****************************************************************
 * IR opcodes
 ****************************************************************/

enum ir_op {
    IR_NOP,
    IR_LIC,
    IR_LEA,
    IR_ADL,
    IR_MOV,

    IR_ADD, IR_SUB, IR_MUL,
    IR_DIVS, IR_DIVU, IR_MODS, IR_MODU,
    IR_AND, IR_OR, IR_XOR,
    IR_SHL, IR_SHRS, IR_SHRU,
    IR_NEG, IR_NOT,

    IR_LB, IR_LBS, IR_LH, IR_LHS, IR_LW,
    IR_SB, IR_SH, IR_SW,

    IR_LDL, IR_STL,
    IR_ALLOCA,

    IR_CMPEQ, IR_CMPNE,
    IR_CMPLTS, IR_CMPLES, IR_CMPGTS, IR_CMPGES,
    IR_CMPLTU, IR_CMPLEU, IR_CMPGTU, IR_CMPGEU,

    IR_JMP, IR_BZ, IR_BNZ,

    IR_ARG,
    IR_FARG,
    IR_CALL,
    IR_CALLI,
    IR_TAILCALL,
    IR_TAILCALLI,
    IR_RET, IR_RETV,

    IR_FUNC, IR_ENDF, IR_LABEL,

    IR_MARK, IR_CAPTURE, IR_RESUME,

    IR_FADD, IR_FSUB, IR_FMUL, IR_FDIV,
    IR_FNEG, IR_FABS,

    IR_FCMPEQ, IR_FCMPLT, IR_FCMPLE,

    IR_ITOF, IR_FTOI,

    IR_FLS, IR_FLD,
    IR_FSS, IR_FSD,
    IR_FLDL, IR_FSTL,

    IR_FRETV,
    IR_FCALL, IR_FCALLI,

    IR_LIC64,
    IR_ADD64, IR_SUB64, IR_MUL64,
    IR_AND64, IR_OR64, IR_XOR64,
    IR_SHL64, IR_SHRS64, IR_SHRU64,
    IR_NEG64,

    IR_CMP64EQ, IR_CMP64NE,
    IR_CMP64LTS, IR_CMP64LES, IR_CMP64GTS, IR_CMP64GES,
    IR_CMP64LTU, IR_CMP64LEU, IR_CMP64GTU, IR_CMP64GEU,

    IR_LD64, IR_ST64,
    IR_LDL64, IR_STL64,

    IR_SEXT64, IR_ZEXT64, IR_TRUNC64,

    IR_ARG64,
    IR_RETV64,
    IR_CALL64, IR_CALLI64,
};

/****************************************************************
 * IR data structures
 ****************************************************************/

struct ir_insn {
    int op;
    int dst;
    int a, b;
    long imm;
    char *sym;
    int slot;
    int label;
    int nargs;
    struct ir_insn *next;
};

struct ir_func {
    struct arena *arena;
    char *name;
    int is_local;
    int nparams;
    int nslots;
    int *slot_size;
    int ntemps;
    int nlabels;
    int nspills;
    int nfspills;
    int ni64spills;
    int *temp_reg;
    int *temp_spill;
    struct ir_insn *head;
    struct ir_insn *tail;
    struct ir_func *next;
};

struct ir_global {
    char *name;
    int base_type;
    int arr_size;
    int is_ptr;
    int is_local;
    int64_t *init_ivals;
    char **init_syms;
    int init_count;
    char *init_string;
    int init_strlen;
    struct ir_global *next;
};

struct ir_program {
    struct ir_func *funcs;
    struct ir_global *globals;
};

/****************************************************************
 * IR builder API
 ****************************************************************/

struct ir_func *ir_new_func(struct arena *a, const char *name);
int ir_new_temp(struct ir_func *fn);
int ir_new_label(struct ir_func *fn);
struct ir_insn *ir_emit(struct ir_func *fn, int op);
int ir_op_is_float_def(int op);
int ir_op_is_i64_def(int op);

/****************************************************************
 * Register allocation
 ****************************************************************/

void regalloc(struct ir_func *fn);

/****************************************************************
 * Back-end interface
 *
 * regalloc() and target_emit() are provided by the linked backend.
 * For compile-time target selection, link a different backend:
 *   ColdFire: backend/regalloc_cf.c + backend/cf_emit.c
 *   RISC-V:   backend/regalloc_rv.c + backend/rv_emit.c  (planned)
 ****************************************************************/

void target_emit(FILE *out, struct ir_program *prog);

#endif /* IR_H */
