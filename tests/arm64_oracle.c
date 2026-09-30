/* arm64_oracle.c : verify the jit/ AArch64 byte encoder against GNU as.

   The AArch64 golden-master, the counterpart of check-x86-oracle (vs nasm) and
   check-rvas / check-mipsas (vs GNU as) for the byte encoder.  Each case encodes
   one instruction two ways, through jit/emit_arm64.c and through
   aarch64-linux-gnu-as (its .text extracted with objcopy), and fails on any byte
   difference. */

#define _POSIX_C_SOURCE 200809L

#include "emit_arm64.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *AS = "aarch64-linux-gnu-as";
static const char *OBJCOPY = "aarch64-linux-gnu-objcopy";

/* Assemble one line to flat binary (the .text bytes).  Returns the byte count,
   or -1 on failure, writing the bytes into out (up to omax). */
static int
assemble(const char *line, uint8_t *out, int omax)
{
    char src[] = "/tmp/a64_oracsXXXXXX";
    char obj[] = "/tmp/a64_oracoXXXXXX";
    char bin[] = "/tmp/a64_oracbXXXXXX";
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
             "%s -o %s %s 2>/dev/null && "
             "%s -O binary --only-section=.text %s %s 2>/dev/null",
             AS, obj, src, OBJCOPY, obj, bin);
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

/* Compare the encoder's output in c against as's assembly of `line`. */
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

    /* reset the buffer to empty and hand it to the next encoder */
#define C (c.len = 0, &c)

    /* ---- move wide ---- */
    a_movz(C, 1, 0, 0x1234, 0);   check("movz x0, #0x1234", &c);
    a_movz(C, 1, 5, 0xabcd, 16);  check("movz x5, #0xabcd, lsl #16", &c);
    a_movz(C, 0, 9, 0xffff, 0);   check("movz w9, #0xffff", &c);
    a_movk(C, 1, 3, 0x00ff, 32);  check("movk x3, #0xff, lsl #32", &c);
    a_movk(C, 0, 10, 0x1000, 16); check("movk w10, #0x1000, lsl #16", &c);
    a_movn(C, 1, 1, 0x0001, 0);   check("movn x1, #1", &c);

    /* ---- add/sub immediate ---- */
    a_add_imm(C, 1, 0, 1, 42, 0);      check("add x0, x1, #42", &c);
    a_add_imm(C, 1, 31, 31, 16, 0);    check("add sp, sp, #16", &c);
    a_add_imm(C, 0, 2, 3, 4095, 0);    check("add w2, w3, #4095", &c);
    a_add_imm(C, 1, 4, 5, 2, 1);       check("add x4, x5, #2, lsl #12", &c);
    a_sub_imm(C, 1, 31, 31, 32, 0);    check("sub sp, sp, #32", &c);
    a_sub_imm(C, 1, 29, 31, 256, 0);   check("sub x29, sp, #256", &c);

    /* ---- data-processing, three-register ---- */
    a_add_reg(C, 1, 0, 1, 2);   check("add x0, x1, x2", &c);
    a_add_reg(C, 0, 9, 10, 11); check("add w9, w10, w11", &c);
    a_sub_reg(C, 1, 3, 4, 5);   check("sub x3, x4, x5", &c);
    a_and_reg(C, 0, 6, 7, 8);   check("and w6, w7, w8", &c);
    a_orr_reg(C, 0, 6, 7, 8);   check("orr w6, w7, w8", &c);
    a_eor_reg(C, 0, 6, 7, 8);   check("eor w6, w7, w8", &c);
    a_mul(C, 0, 1, 2, 3);       check("mul w1, w2, w3", &c);
    a_mul(C, 1, 1, 2, 3);       check("mul x1, x2, x3", &c);
    a_msub(C, 0, 0, 15, 3, 1);  check("msub w0, w15, w3, w1", &c);
    a_sdiv(C, 0, 1, 2, 3);      check("sdiv w1, w2, w3", &c);
    a_udiv(C, 0, 1, 2, 3);      check("udiv w1, w2, w3", &c);
    a_lslv(C, 0, 1, 2, 3);      check("lsl w1, w2, w3", &c);
    a_asrv(C, 0, 1, 2, 3);      check("asr w1, w2, w3", &c);
    a_lsrv(C, 0, 1, 2, 3);      check("lsr w1, w2, w3", &c);
    a_lslv(C, 1, 1, 2, 3);      check("lsl x1, x2, x3", &c);

    /* ---- two-register / moves ---- */
    a_neg(C, 0, 5, 6);          check("neg w5, w6", &c);
    a_neg(C, 1, 5, 6);          check("neg x5, x6", &c);
    a_mvn(C, 0, 5, 6);          check("mvn w5, w6", &c);
    a_mov_reg(C, 0, 3, 4);      check("mov w3, w4", &c);
    a_mov_reg(C, 1, 3, 4);      check("mov x3, x4", &c);
    a_sxtw(C, 0, 1);            check("sxtw x0, w1", &c);

    /* ---- compare / cset ---- */
    a_cmp_reg(C, 0, 1, 2);      check("cmp w1, w2", &c);
    a_cmp_reg(C, 1, 1, 2);      check("cmp x1, x2", &c);
    a_cset(C, 0, 0, A_EQ);      check("cset w0, eq", &c);
    a_cset(C, 0, 1, A_NE);      check("cset w1, ne", &c);
    a_cset(C, 0, 2, A_LT);      check("cset w2, lt", &c);
    a_cset(C, 0, 3, A_GE);      check("cset w3, ge", &c);
    a_cset(C, 0, 4, A_HI);      check("cset w4, hi", &c);
    a_cset(C, 0, 5, A_LS);      check("cset w5, ls", &c);
    a_cset(C, 0, 6, A_MI);      check("cset w6, mi", &c);

    /* ---- integer loads/stores (scaled) ---- */
    a_strx(C, 0, 1, 16);        check("str x0, [x1, #16]", &c);
    a_strw(C, 2, 3, 8);         check("str w2, [x3, #8]", &c);
    a_strb(C, 4, 5, 3);         check("strb w4, [x5, #3]", &c);
    a_strh(C, 4, 5, 6);         check("strh w4, [x5, #6]", &c);
    a_ldrx(C, 0, 1, 4088);      check("ldr x0, [x1, #4088]", &c);
    a_ldrw(C, 2, 3, 0);         check("ldr w2, [x3]", &c);
    a_ldrb(C, 4, 5, 1);         check("ldrb w4, [x5, #1]", &c);
    a_ldrsb(C, 4, 5, 1);        check("ldrsb w4, [x5, #1]", &c);
    a_ldrh(C, 4, 5, 2);         check("ldrh w4, [x5, #2]", &c);
    a_ldrsh(C, 4, 5, 2);        check("ldrsh w4, [x5, #2]", &c);
    /* unscaled (negative / unaligned) */
    a_ldrx(C, 0, 29, -8);       check("ldur x0, [x29, #-8]", &c);
    a_strx(C, 0, 29, -16);      check("stur x0, [x29, #-16]", &c);
    a_ldrw(C, 2, 3, -4);        check("ldur w2, [x3, #-4]", &c);
    a_ldrx(C, 0, 1, 7);         check("ldur x0, [x1, #7]", &c);   /* unaligned */

    /* ---- FP loads/stores ---- */
    a_str_fp(C, 3, 0, 1, 16);   check("str d0, [x1, #16]", &c);
    a_ldr_fp(C, 3, 0, 1, 16);   check("ldr d0, [x1, #16]", &c);
    a_str_fp(C, 2, 5, 6, 8);    check("str s5, [x6, #8]", &c);
    a_ldr_fp(C, 2, 5, 6, 0);    check("ldr s5, [x6]", &c);
    a_ldr_fp(C, 1, 0, 1, 2);    check("ldr h0, [x1, #2]", &c);
    a_ldr_fp(C, 3, 0, 29, -8);  check("ldur d0, [x29, #-8]", &c);

    /* ---- FP data processing ---- */
    a_fmov(C, 3, 0, 1);         check("fmov d0, d1", &c);
    a_fmov(C, 2, 0, 1);         check("fmov s0, s1", &c);
    a_fadd(C, 3, 0, 1, 2);      check("fadd d0, d1, d2", &c);
    a_fsub(C, 3, 0, 1, 2);      check("fsub d0, d1, d2", &c);
    a_fmul(C, 3, 0, 1, 2);      check("fmul d0, d1, d2", &c);
    a_fdiv(C, 3, 0, 1, 2);      check("fdiv d0, d1, d2", &c);
    a_fadd(C, 2, 0, 1, 2);      check("fadd s0, s1, s2", &c);
    a_fneg(C, 3, 0, 1);         check("fneg d0, d1", &c);
    a_fabs(C, 3, 0, 1);         check("fabs d0, d1", &c);
    a_fsqrt(C, 3, 0, 1);        check("fsqrt d0, d1", &c);
    a_fsqrt(C, 2, 0, 1);        check("fsqrt s0, s1", &c);
    a_fcmp(C, 3, 0, 1);         check("fcmp d0, d1", &c);
    a_fcmp(C, 2, 3, 4);         check("fcmp s3, s4", &c);
    a_fcvt(C, 3, 2, 0, 1);      check("fcvt d0, s1", &c);
    a_fcvt(C, 2, 3, 0, 1);      check("fcvt s0, d1", &c);
    a_fcvt(C, 3, 1, 0, 1);      check("fcvt d0, h1", &c);
    a_fcvt(C, 1, 3, 0, 1);      check("fcvt h0, d1", &c);
    a_scvtf(C, 3, 0, 1);        check("scvtf d0, w1", &c);
    a_scvtf(C, 2, 0, 1);        check("scvtf s0, w1", &c);
    a_fcvtzs(C, 3, 0, 1);       check("fcvtzs w0, d1", &c);
    a_fcvtzs(C, 2, 0, 1);       check("fcvtzs w0, s1", &c);
    a_fcvtns(C, 3, 0, 1);       check("fcvtns w0, d1", &c);
    a_fcvtns(C, 2, 0, 1);       check("fcvtns w0, s1", &c);

    /* ---- control flow (unpatched, imm 0 = self) ---- */
    a_b(C);                     check("b .", &c);
    a_bl(C);                    check("bl .", &c);
    a_bcond(C, A_EQ);           check("b.eq .", &c);
    a_bcond(C, A_NE);           check("b.ne .", &c);
    a_cbz(C, 0, 3);             check("cbz w3, .", &c);
    a_cbnz(C, 1, 4);            check("cbnz x4, .", &c);
    a_blr(C, 16);               check("blr x16", &c);
    a_br(C, 11);                check("br x11", &c);
    a_ret(C);                   check("ret", &c);
    a_nop(C);                   check("nop", &c);

    /* ---- branch patching (forward) ---- */
    { size_t s = a_b(C);    a_patch_imm26(&c, s, 16);  check("b . + 16", &c); }
    { size_t s = a_bl(C);   a_patch_imm26(&c, s, 32);  check("bl . + 32", &c); }
    { size_t s = a_bcond(C, A_LT); a_patch_imm19(&c, s, 20); check("b.lt . + 20", &c); }
    { size_t s = a_cbz(C, 0, 2);   a_patch_imm19(&c, s, 8);  check("cbz w2, . + 8", &c); }

    /* ---- branch patching (backward): a branch two words in targets word 0 ---- */
    { c.len = 0; a_nop(&c); a_nop(&c);
      size_t s = a_b(&c); a_patch_imm26(&c, s, 0);
      check("nop\n\tnop\n\tb . - 8", &c); }
    { c.len = 0; a_nop(&c);
      size_t s = a_bcond(&c, A_EQ); a_patch_imm19(&c, s, 0);
      check("nop\n\tb.eq . - 4", &c); }

    if (fails == 0)
        printf("oracle: all %s encodings match GNU as\n", "AArch64");
    code_free(&c);
    return fails ? 1 : 0;
}
