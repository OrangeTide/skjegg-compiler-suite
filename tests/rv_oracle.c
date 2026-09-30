/* rv_oracle.c : verify the jit/ RV32 byte encoder against GNU as.

   The RISC-V counterpart of arm64_oracle.c (vs aarch64 as) and x86_oracle.c (vs
   nasm).  Each case encodes one instruction two ways, through jit/emit_rv.c and
   through riscv64-linux-gnu-as (its .text extracted with objcopy), and fails on
   any byte difference. */

#define _POSIX_C_SOURCE 200809L

#include "emit_rv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *AS = "riscv64-linux-gnu-as";
static const char *OBJCOPY = "riscv64-linux-gnu-objcopy";
static const char *ARCH = "-march=rv32imafd_zfh_zba_zbb_zbs -mabi=ilp32 -mno-relax";

static int
assemble(const char *line, uint8_t *out, int omax)
{
    char src[] = "/tmp/rv_oracsXXXXXX";
    char obj[] = "/tmp/rv_oracoXXXXXX";
    char bin[] = "/tmp/rv_oracbXXXXXX";
    char cmd[1024];
    int sfd, ofd, bfd, n;
    FILE *f, *b;

    sfd = mkstemp(src);
    ofd = mkstemp(obj);
    bfd = mkstemp(bin);
    if (sfd < 0 || ofd < 0 || bfd < 0)
        return -1;
    close(ofd);
    close(bfd);
    f = fdopen(sfd, "w");
    fprintf(f, "%s\n", line);
    fclose(f);

    snprintf(cmd, sizeof(cmd),
             "%s %s -o %s %s 2>/dev/null && "
             "%s -O binary --only-section=.text %s %s 2>/dev/null",
             AS, ARCH, obj, src, OBJCOPY, obj, bin);
    if (system(cmd) != 0) {
        unlink(src); unlink(obj); unlink(bin);
        return -1;
    }
    b = fopen(bin, "rb");
    n = b ? (int)fread(out, 1, (size_t)omax, b) : -1;
    if (b)
        fclose(b);
    unlink(src); unlink(obj); unlink(bin);
    return n;
}

static int fails;

static void
check(const char *line, struct code *c)
{
    uint8_t ref[64];
    int n = assemble(line, ref, sizeof ref);

    if (n < 0) {
        printf("FAIL %-40s (assembler error)\n", line);
        fails++;
        return;
    }
    if ((size_t)n != c->len || memcmp(ref, c->buf, c->len) != 0) {
        printf("FAIL %-40s\n     enc:", line);
        for (size_t i = 0; i < c->len; i++) printf(" %02x", c->buf[i]);
        printf("\n     as: ");
        for (int i = 0; i < n; i++) printf(" %02x", ref[i]);
        printf("\n");
        fails++;
    }
}

int
main(int argc, char **argv)
{
    struct code c;

    if (argc > 1) AS = argv[1];
    if (argc > 2) OBJCOPY = argv[2];
    code_init(&c);
#define C (c.len = 0, &c)

    /* ---- RV32I / M register ops ---- */
    rv_add(C, 10, 11, 12);   check("add a0, a1, a2", &c);
    rv_sub(C, 9, 18, 19);    check("sub s1, s2, s3", &c);
    rv_sll(C, 5, 6, 7);      check("sll t0, t1, t2", &c);
    rv_slt(C, 5, 6, 7);      check("slt t0, t1, t2", &c);
    rv_sltu(C, 5, 6, 7);     check("sltu t0, t1, t2", &c);
    rv_xor(C, 5, 6, 7);      check("xor t0, t1, t2", &c);
    rv_srl(C, 5, 6, 7);      check("srl t0, t1, t2", &c);
    rv_sra(C, 5, 6, 7);      check("sra t0, t1, t2", &c);
    rv_or(C, 5, 6, 7);       check("or t0, t1, t2", &c);
    rv_and(C, 5, 6, 7);      check("and t0, t1, t2", &c);
    rv_mul(C, 10, 11, 12);   check("mul a0, a1, a2", &c);
    rv_mulh(C, 10, 11, 12);  check("mulh a0, a1, a2", &c);
    rv_mulhu(C, 10, 11, 12); check("mulhu a0, a1, a2", &c);
    rv_mulhsu(C, 10, 11, 12);check("mulhsu a0, a1, a2", &c);
    rv_div(C, 10, 11, 12);   check("div a0, a1, a2", &c);
    rv_divu(C, 10, 11, 12);  check("divu a0, a1, a2", &c);
    rv_rem(C, 10, 11, 12);   check("rem a0, a1, a2", &c);
    rv_remu(C, 10, 11, 12);  check("remu a0, a1, a2", &c);

    /* ---- OP-IMM ---- */
    rv_addi(C, 10, 11, 42);     check("addi a0, a1, 42", &c);
    rv_addi(C, 2, 2, -16);      check("addi sp, sp, -16", &c);
    rv_slti(C, 5, 6, 100);      check("slti t0, t1, 100", &c);
    rv_sltiu(C, 5, 6, 7);       check("sltiu t0, t1, 7", &c);
    rv_xori(C, 5, 6, -1);       check("xori t0, t1, -1", &c);
    rv_ori(C, 5, 6, 255);       check("ori t0, t1, 255", &c);
    rv_andi(C, 5, 6, 15);       check("andi t0, t1, 15", &c);
    rv_slli(C, 5, 6, 3);        check("slli t0, t1, 3", &c);
    rv_srli(C, 5, 6, 31);       check("srli t0, t1, 31", &c);
    rv_srai(C, 5, 6, 1);        check("srai t0, t1, 1", &c);

    /* ---- loads / stores ---- */
    rv_lb(C, 10, 11, 4);        check("lb a0, 4(a1)", &c);
    rv_lh(C, 10, 11, 4);        check("lh a0, 4(a1)", &c);
    rv_lw(C, 10, 11, -8);       check("lw a0, -8(a1)", &c);
    rv_lbu(C, 10, 11, 0);       check("lbu a0, 0(a1)", &c);
    rv_lhu(C, 10, 11, 2);       check("lhu a0, 2(a1)", &c);
    rv_sb(C, 12, 11, 4);        check("sb a2, 4(a1)", &c);
    rv_sh(C, 12, 11, 4);        check("sh a2, 4(a1)", &c);
    rv_sw(C, 12, 11, -8);       check("sw a2, -8(a1)", &c);

    /* ---- U-type and jumps ---- */
    rv_lui(C, 10, 0x12345);     check("lui a0, 0x12345", &c);
    rv_auipc(C, 10, 0x10);      check("auipc a0, 0x10", &c);
    rv_jalr(C, 1, 6, 0);        check("jalr ra, 0(t1)", &c);
    rv_jalr(C, 0, 1, 0);        check("jalr zero, 0(ra)", &c);
    /* jal/branch displacements are patched later, so just the zero-disp form */
    rv_jal(C, 1, 0);            check("jal ra, .", &c);
    rv_beq(C, 5, 6, 0);         check("beq t0, t1, .", &c);
    rv_bne(C, 5, 6, 0);         check("bne t0, t1, .", &c);
    rv_blt(C, 5, 6, 0);         check("blt t0, t1, .", &c);
    rv_bge(C, 5, 6, 0);         check("bge t0, t1, .", &c);
    rv_bltu(C, 5, 6, 0);        check("bltu t0, t1, .", &c);
    rv_bgeu(C, 5, 6, 0);        check("bgeu t0, t1, .", &c);

    /* ---- float loads / stores ---- */
    rv_flw(C, 0, 11, 4);        check("flw ft0, 4(a1)", &c);
    rv_fld(C, 0, 11, 8);        check("fld ft0, 8(a1)", &c);
    rv_flh(C, 0, 11, 2);        check("flh ft0, 2(a1)", &c);
    rv_fsw(C, 0, 11, 4);        check("fsw ft0, 4(a1)", &c);
    rv_fsd(C, 0, 11, 8);        check("fsd ft0, 8(a1)", &c);
    rv_fsh(C, 0, 11, 2);        check("fsh ft0, 2(a1)", &c);

    /* ---- float arithmetic (dyn rounding) ---- */
    rv_fadd_s(C, 0, 1, 2);      check("fadd.s ft0, ft1, ft2", &c);
    rv_fsub_s(C, 0, 1, 2);      check("fsub.s ft0, ft1, ft2", &c);
    rv_fmul_s(C, 0, 1, 2);      check("fmul.s ft0, ft1, ft2", &c);
    rv_fdiv_s(C, 0, 1, 2);      check("fdiv.s ft0, ft1, ft2", &c);
    rv_fadd_d(C, 8, 9, 10);     check("fadd.d fs0, fs1, fa0", &c);
    rv_fsub_d(C, 8, 9, 10);     check("fsub.d fs0, fs1, fa0", &c);
    rv_fmul_d(C, 8, 9, 10);     check("fmul.d fs0, fs1, fa0", &c);
    rv_fdiv_d(C, 8, 9, 10);     check("fdiv.d fs0, fs1, fa0", &c);
    rv_fadd_h(C, 0, 1, 2);      check("fadd.h ft0, ft1, ft2", &c);

    /* ---- sign-inject, min/max, compares, sqrt ---- */
    rv_fsgnj_d(C, 0, 1, 1);     check("fsgnj.d ft0, ft1, ft1", &c);
    rv_fsgnjn_d(C, 0, 1, 1);    check("fsgnjn.d ft0, ft1, ft1", &c);
    rv_fsgnjx_d(C, 0, 1, 1);    check("fsgnjx.d ft0, ft1, ft1", &c);
    rv_fmin_d(C, 0, 1, 2);      check("fmin.d ft0, ft1, ft2", &c);
    rv_fmax_d(C, 0, 1, 2);      check("fmax.d ft0, ft1, ft2", &c);
    rv_feq_d(C, 10, 1, 2);      check("feq.d a0, ft1, ft2", &c);
    rv_flt_d(C, 10, 1, 2);      check("flt.d a0, ft1, ft2", &c);
    rv_fle_d(C, 10, 1, 2);      check("fle.d a0, ft1, ft2", &c);
    rv_feq_s(C, 10, 1, 2);      check("feq.s a0, ft1, ft2", &c);
    rv_fsqrt_d(C, 0, 1);        check("fsqrt.d ft0, ft1", &c);
    rv_fsqrt_s(C, 0, 1);        check("fsqrt.s ft0, ft1", &c);

    /* ---- int/float moves ---- */
    rv_fmv_x_w(C, 10, 1);       check("fmv.x.w a0, ft1", &c);
    rv_fmv_w_x(C, 0, 10);       check("fmv.w.x ft0, a0", &c);
    rv_fmv_x_h(C, 10, 1);       check("fmv.x.h a0, ft1", &c);
    rv_fmv_h_x(C, 0, 10);       check("fmv.h.x ft0, a0", &c);

    /* ---- conversions (dyn for rounding, rne where exact) ---- */
    rv_fcvt_s_w(C, 0, 10, RV_RM_DYN);   check("fcvt.s.w ft0, a0", &c);
    rv_fcvt_s_wu(C, 0, 10, RV_RM_DYN);  check("fcvt.s.wu ft0, a0", &c);
    rv_fcvt_d_w(C, 0, 10, RV_RM_RNE);   check("fcvt.d.w ft0, a0", &c);
    rv_fcvt_w_s(C, 10, 0, RV_RM_DYN);   check("fcvt.w.s a0, ft0", &c);
    rv_fcvt_w_d(C, 10, 0, RV_RM_DYN);   check("fcvt.w.d a0, ft0", &c);
    rv_fcvt_wu_d(C, 10, 0, RV_RM_DYN);  check("fcvt.wu.d a0, ft0", &c);
    rv_fcvt_s_d(C, 0, 1, RV_RM_DYN);    check("fcvt.s.d ft0, ft1", &c);
    rv_fcvt_d_s(C, 0, 1, RV_RM_RNE);    check("fcvt.d.s ft0, ft1", &c);
    rv_fcvt_s_h(C, 0, 1, RV_RM_RNE);    check("fcvt.s.h ft0, ft1", &c);
    rv_fcvt_h_s(C, 0, 1, RV_RM_DYN);    check("fcvt.h.s ft0, ft1", &c);
    rv_fcvt_d_h(C, 0, 1, RV_RM_RNE);    check("fcvt.d.h ft0, ft1", &c);
    rv_fcvt_h_d(C, 0, 1, RV_RM_DYN);    check("fcvt.h.d ft0, ft1", &c);

    /* ---- branch/jal displacement patching ---- */
    c.len = 0;
    rv_beq(&c, 5, 6, 0);
    if (rv_patch_b(&c, 0, 2044) != 0) { printf("FAIL patch_b in range\n"); fails++; }
    check("beq t0, t1, . + 2044", &c);
    c.len = 0;
    rv_beq(&c, 5, 6, 0);
    if (rv_patch_b(&c, 0, -2048) != 0) { printf("FAIL patch_b neg\n"); fails++; }
    check("beq t0, t1, . - 2048", &c);
    c.len = 0;
    rv_jal(&c, 1, 0);
    if (rv_patch_j(&c, 0, 0x1000) != 0) { printf("FAIL patch_j in range\n"); fails++; }
    check("jal ra, . + 0x1000", &c);
    if (rv_patch_b(&c, 0, 4096) != -1) { printf("FAIL patch_b range guard\n"); fails++; }
    if (rv_patch_j(&c, 0, 1 << 21) != -1) { printf("FAIL patch_j range guard\n"); fails++; }

    code_free(&c);
    if (fails) {
        printf("\n%d encoder check(s) FAILED\n", fails);
        return 1;
    }
    printf("all RV32 encoder checks passed\n");
    return 0;
}
