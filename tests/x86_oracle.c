/* x86_oracle.c : verify the jit/ x86-64 byte encoder against nasm.

   The x86 golden-master, the counterpart of check-rvas (skj-as-rv vs GNU as)
   and check-mipsas (skj-as-mips vs GNU as) for the byte encoder.  Each case
   encodes one instruction two ways, through jit/emit_x86.c and through
   `nasm -O0 -f bin`, and fails on any byte difference. */

#define _POSIX_C_SOURCE 200809L

#include "emit_x86.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Assemble one line of NASM `bits 64` source to flat binary. Returns the
   byte count, or -1 on failure, writing the bytes into out (up to omax). */
static int
assemble(const char *line, uint8_t *out, int omax)
{
    char src[] = "/tmp/kp_oracleXXXXXX";
    char bin[] = "/tmp/kp_oraclbXXXXXX";
    char cmd[512];
    int sfd, bfd, n;
    FILE *f, *b;

    sfd = mkstemp(src);
    bfd = mkstemp(bin);
    if (sfd < 0 || bfd < 0)
        return -1;
    close(bfd);
    f = fdopen(sfd, "w");
    fprintf(f, "bits 64\n%s\n", line);
    fclose(f);

    /* -O0 disables nasm's form optimization so it emits the same canonical
       long forms (imm32, rel32, full imm64) the encoder uses. */
    snprintf(cmd, sizeof(cmd), "nasm -O0 -f bin -o %s %s 2>/dev/null", bin, src);
    if (system(cmd) != 0) {
        unlink(src);
        unlink(bin);
        return -1;
    }
    b = fopen(bin, "rb");
    n = b ? (int)fread(out, 1, (size_t)omax, b) : -1;
    if (b)
        fclose(b);
    unlink(src);
    unlink(bin);
    return n;
}

static int fails;

/* Compare the encoder's output in c against nasm's assembly of `line`. */
static void
check(const char *line, struct code *c)
{
    uint8_t ref[64];
    int rn = assemble(line, ref, sizeof(ref));
    int ok = (rn == (int)c->len && rn >= 0 &&
              memcmp(ref, c->buf, (size_t)rn) == 0);

    if (!ok) {
        fails++;
        printf("FAIL  %-28s\n", line);
        printf("        mine:");
        for (size_t i = 0; i < c->len; i++)
            printf(" %02x", c->buf[i]);
        printf("\n        nasm:");
        for (int i = 0; i < rn; i++)
            printf(" %02x", ref[i]);
        printf("\n");
    }
    code_free(c);
}

/* Encode `line` with the given encoder call, then check it. Each use opens a
   fresh buffer, runs the encoder, and diffs against nasm. */
#define CASE(line, ...) do { \
        struct code c; code_init(&c); __VA_ARGS__; check(line, &c); \
    } while (0)

int
main(void)
{
    CASE("mov ebx, 0x2a", x_mov_ri32(&c, X_RBX, 0x2a));
    CASE("mov r12d, 0xdeadbeef", x_mov_ri32(&c, X_R12, 0xdeadbeef));
    CASE("mov rbx, 0x1122334455667788",
         x_mov_ri64(&c, X_RBX, 0x1122334455667788ULL));
    CASE("mov r15, 0x4030201", x_mov_ri64(&c, X_R15, 0x4030201ULL));

    CASE("mov rbx, r12", x_mov_rr(&c, X_RBX, X_R12));
    CASE("mov r13, rax", x_mov_rr(&c, X_R13, X_RAX));
    CASE("add rbx, r14", x_add_rr(&c, X_RBX, X_R14));
    CASE("sub r12, rbx", x_sub_rr(&c, X_R12, X_RBX));
    CASE("and rbx, r15", x_and_rr(&c, X_RBX, X_R15));
    CASE("or r13, r14", x_or_rr(&c, X_R13, X_R14));
    CASE("xor rax, rax", x_xor_rr(&c, X_RAX, X_RAX));
    CASE("imul rbx, r12", x_imul_rr(&c, X_RBX, X_R12));
    CASE("neg r13", x_neg_r(&c, X_R13));
    CASE("not rbx", x_not_r(&c, X_RBX));

    CASE("add rbx, 0x100", x_add_ri32(&c, X_RBX, 0x100));
    CASE("sub rsp, 0x28", x_sub_ri32(&c, X_RSP, 0x28));
    CASE("add rsp, 0x28", x_add_ri32(&c, X_RSP, 0x28));

    CASE("cqo", x_cqo(&c));
    CASE("idiv r12", x_idiv_r(&c, X_R12));
    CASE("shl rbx, cl", x_shl_cl(&c, X_RBX));
    CASE("sar r13, cl", x_sar_cl(&c, X_R13));
    CASE("shr r14, cl", x_shr_cl(&c, X_R14));

    /* 32-bit value forms */
    CASE("mov ebx, r12d", x_mov32_rr(&c, X_RBX, X_R12));
    CASE("add ebx, r14d", x_add32_rr(&c, X_RBX, X_R14));
    CASE("sub r12d, ebx", x_sub32_rr(&c, X_R12, X_RBX));
    CASE("and ebx, r15d", x_and32_rr(&c, X_RBX, X_R15));
    CASE("or r13d, r14d", x_or32_rr(&c, X_R13, X_R14));
    CASE("xor ebx, ebx", x_xor32_rr(&c, X_RBX, X_RBX));
    CASE("imul ebx, r12d", x_imul32_rr(&c, X_RBX, X_R12));
    CASE("neg r13d", x_neg32_r(&c, X_R13));
    CASE("not ebx", x_not32_r(&c, X_RBX));
    CASE("cmp ebx, r12d", x_cmp32_rr(&c, X_RBX, X_R12));
    CASE("test eax, eax", x_test32_rr(&c, X_RAX, X_RAX));
    CASE("cdq", x_cdq(&c));
    CASE("idiv r12d", x_idiv32_r(&c, X_R12));
    CASE("shl ebx, cl", x_shl32_cl(&c, X_RBX));
    CASE("sar r13d, cl", x_sar32_cl(&c, X_R13));
    CASE("shr r14d, cl", x_shr32_cl(&c, X_R14));

    CASE("cmp rbx, r12", x_cmp_rr(&c, X_RBX, X_R12));
    CASE("test rax, rax", x_test_rr(&c, X_RAX, X_RAX));
    CASE("setle bl", x_setcc_r(&c, X_LE, X_RBX));
    CASE("setl r13b", x_setcc_r(&c, X_L, X_R13));
    CASE("sete sil", x_setcc_r(&c, X_E, X_RSI));
    CASE("movzx rbx, bl", x_movzx_rb(&c, X_RBX, X_RBX));
    CASE("movzx r12, r13b", x_movzx_rb(&c, X_R12, X_R13));
    CASE("movsxd rbx, r12d", x_movsxd_rr(&c, X_RBX, X_R12));
    CASE("movsxd r13, eax", x_movsxd_rr(&c, X_R13, X_RAX));

    CASE("mov ebx, [rbp-4]", x_load32(&c, X_RBX, X_RBP, -4));
    CASE("mov r12d, [rsp+0x10]", x_load32(&c, X_R12, X_RSP, 0x10));
    CASE("mov [rbp-8], r13d", x_store32(&c, X_RBP, -8, X_R13));
    CASE("mov [r12+0x100], ebx", x_store32(&c, X_R12, 0x100, X_RBX));
    CASE("mov ebx, [r13]", x_load32(&c, X_RBX, X_R13, 0));
    CASE("mov rbx, [rbp-0x10]", x_load64(&c, X_RBX, X_RBP, -0x10));
    CASE("mov [rsp], rbx", x_store64(&c, X_RSP, 0, X_RBX));
    CASE("mov rbx, [rax]", x_load64(&c, X_RBX, X_RAX, 0));
    CASE("lea rbx, [rbp-0x10]", x_lea(&c, X_RBX, X_RBP, -0x10));
    CASE("lea r12, [rsp+8]", x_lea(&c, X_R12, X_RSP, 8));
    CASE("lea r13, [r13-0x40]", x_lea(&c, X_R13, X_R13, -0x40));

    CASE("push rbx", x_push_r(&c, X_RBX));
    CASE("push r14", x_push_r(&c, X_R14));
    CASE("pop rbp", x_pop_r(&c, X_RBP));
    CASE("pop r15", x_pop_r(&c, X_R15));

    CASE("ret", x_ret(&c));
    CASE("leave", x_leave(&c));
    CASE("nop", x_nop(&c));
    CASE("call rax", x_call_r(&c, X_RAX));
    CASE("call r11", x_call_r(&c, X_R11));
    CASE("jmp rax", x_jmp_r(&c, X_RAX));
    CASE("jmp r11", x_jmp_r(&c, X_R11));

    /* rel32 forms: nasm resolves "$" to this instruction, so a target of the
       instruction's own start is the displacement nasm computes for "jmp $"
       etc. Encode with a zero field, then patch to offset 0. */
    CASE("call $", { size_t a = x_call_rel32(&c); x_patch_rel32(&c, a, 0); });
    CASE("jmp near $", { size_t a = x_jmp_rel32(&c); x_patch_rel32(&c, a, 0); });
    CASE("jz near $", { size_t a = x_jcc_rel32(&c, X_E); x_patch_rel32(&c, a, 0); });
    CASE("jle near $", { size_t a = x_jcc_rel32(&c, X_LE); x_patch_rel32(&c, a, 0); });

    /* unsigned divide (div, /6) */
    CASE("div r12", x_div_r(&c, X_R12));
    CASE("div rbx", x_div_r(&c, X_RBX));
    CASE("div r12d", x_div32_r(&c, X_R12));
    CASE("div ebx", x_div32_r(&c, X_RBX));

    /* sub-word loads (byte/half, sign- or zero-extended into r32) */
    CASE("movsx ebx, byte [rbp-4]", x_load8s(&c, X_RBX, X_RBP, -4));
    CASE("movsx r12d, byte [r13+0x10]", x_load8s(&c, X_R12, X_R13, 0x10));
    CASE("movzx ebx, word [rbp-4]", x_load16(&c, X_RBX, X_RBP, -4));
    CASE("movzx r12d, word [rsp+0x10]", x_load16(&c, X_R12, X_RSP, 0x10));
    CASE("movsx ebx, word [rbp-4]", x_load16s(&c, X_RBX, X_RBP, -4));
    CASE("movsx r12d, word [r13]", x_load16s(&c, X_R12, X_R13, 0));

    /* sub-word stores (low byte / low 16 bits) */
    CASE("mov [rbp-8], r13b", x_store8(&c, X_RBP, -8, X_R13));
    CASE("mov [r12+0x100], bl", x_store8(&c, X_R12, 0x100, X_RBX));
    CASE("mov [rbx], sil", x_store8(&c, X_RBX, 0, X_RSI));
    CASE("mov [rbp-8], r13w", x_store16(&c, X_RBP, -8, X_R13));
    CASE("mov [r12+0x100], bx", x_store16(&c, X_R12, 0x100, X_RBX));

    /* double-precision SSE */
    CASE("movsd xmm1, xmm8", x_movsd_rr(&c, X_XMM1, X_XMM8));
    CASE("movsd xmm0, xmm7", x_movsd_rr(&c, X_XMM0, X_XMM7));
    CASE("movsd xmm1, [rbp-8]", x_movsd_load(&c, X_XMM1, X_RBP, -8));
    CASE("movsd xmm7, [r12+0x20]", x_movsd_load(&c, X_XMM7, X_R12, 0x20));
    CASE("movsd [rbp-0x10], xmm2", x_movsd_store(&c, X_RBP, -0x10, X_XMM2));
    CASE("movsd [rbx], xmm8", x_movsd_store(&c, X_RBX, 0, X_XMM8));
    CASE("addsd xmm0, xmm1", x_addsd_rr(&c, X_XMM0, X_XMM1));
    CASE("subsd xmm1, xmm2", x_subsd_rr(&c, X_XMM1, X_XMM2));
    CASE("mulsd xmm3, xmm8", x_mulsd_rr(&c, X_XMM3, X_XMM8));
    CASE("divsd xmm0, xmm4", x_divsd_rr(&c, X_XMM0, X_XMM4));
    CASE("ucomisd xmm0, xmm1", x_ucomisd_rr(&c, X_XMM0, X_XMM1));
    CASE("cvtsi2sd xmm0, ebx", x_cvtsi2sd(&c, X_XMM0, X_RBX));
    CASE("cvtsi2sd xmm8, r12d", x_cvtsi2sd(&c, X_XMM8, X_R12));
    CASE("movss [rbp-0x10], xmm2", x_movss_store(&c, X_RBP, -0x10, X_XMM2));
    CASE("movss [r12], xmm8", x_movss_store(&c, X_R12, 0, X_XMM8));
    CASE("movss xmm1, [rbp-8]", x_movss_load(&c, X_XMM1, X_RBP, -8));
    CASE("movss xmm8, [r12+0x20]", x_movss_load(&c, X_XMM8, X_R12, 0x20));
    CASE("cvtss2sd xmm1, xmm2", x_cvtss2sd_rr(&c, X_XMM1, X_XMM2));
    CASE("cvtss2sd xmm8, xmm0", x_cvtss2sd_rr(&c, X_XMM8, X_XMM0));
    CASE("cvtsd2ss xmm1, xmm2", x_cvtsd2ss_rr(&c, X_XMM1, X_XMM2));
    CASE("cvtss2sd xmm1, dword [rbp-8]", x_cvtss2sd_load(&c, X_XMM1, X_RBP, -8));
    CASE("cvtss2sd xmm7, dword [r12+0x20]", x_cvtss2sd_load(&c, X_XMM7, X_R12, 0x20));
    CASE("addss xmm0, xmm1", x_addss_rr(&c, X_XMM0, X_XMM1));
    CASE("subss xmm1, xmm2", x_subss_rr(&c, X_XMM1, X_XMM2));
    CASE("mulss xmm3, xmm8", x_mulss_rr(&c, X_XMM3, X_XMM8));
    CASE("divss xmm0, xmm4", x_divss_rr(&c, X_XMM0, X_XMM4));
    CASE("ucomiss xmm0, xmm1", x_ucomiss_rr(&c, X_XMM0, X_XMM1));
    CASE("ucomiss xmm8, xmm3", x_ucomiss_rr(&c, X_XMM8, X_XMM3));
    CASE("cvtsi2ss xmm0, ebx", x_cvtsi2ss(&c, X_XMM0, X_RBX));
    CASE("cvtsi2ss xmm8, r12d", x_cvtsi2ss(&c, X_XMM8, X_R12));
    CASE("cvttss2si eax, xmm1", x_cvttss2si(&c, X_RAX, X_XMM1));
    CASE("cvttss2si r10d, xmm8", x_cvttss2si(&c, X_R10, X_XMM8));
    CASE("sqrtsd xmm0, xmm1", x_sqrtsd_rr(&c, X_XMM0, X_XMM1));
    CASE("sqrtsd xmm8, xmm3", x_sqrtsd_rr(&c, X_XMM8, X_XMM3));
    CASE("cvttsd2si eax, xmm1", x_cvttsd2si(&c, X_RAX, X_XMM1));
    CASE("cvttsd2si r10d, xmm8", x_cvttsd2si(&c, X_R10, X_XMM8));
    CASE("cvtsd2si ebx, xmm2", x_cvtsd2si(&c, X_RBX, X_XMM2));
    CASE("cvtsd2si r10d, xmm0", x_cvtsd2si(&c, X_R10, X_XMM0));
    CASE("movq r10, xmm0", x_movq_from_xmm(&c, X_R10, X_XMM0));
    CASE("movq rbx, xmm8", x_movq_from_xmm(&c, X_RBX, X_XMM8));
    CASE("movq xmm0, r10", x_movq_to_xmm(&c, X_XMM0, X_R10));
    CASE("movq xmm8, rbx", x_movq_to_xmm(&c, X_XMM8, X_RBX));

    if (fails == 0)
        printf("oracle: all encodings match nasm\n");
    else
        printf("oracle: %d encoding(s) differ from nasm\n", fails);
    return fails ? 1 : 0;
}
