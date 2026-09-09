/* test_rv_expand.c : compressed-instruction expansion, offset bits
 *
 * A host-side test on rv_expand_c(), which turns a 16-bit compressed
 * encoding into the 32-bit one the interpreter executes.  The rig's
 * other suites reach the expander through programs, so they only ever
 * present the offsets a compiler happened to emit, and a compiler
 * emits small ones: stack slots near the frame pointer.  The high bit
 * of a compressed load offset is therefore exercised by nothing.
 *
 * Mutation testing found the gap.  Dropping bit 6 of the c.lw immediate
 * changes 2048 encodings, turning `lw s0, 64(s0)` into `lw s0, 0(s0)`,
 * and it survived 443 unit checks, 268 compliance tests, four lockstep
 * guests and 120 rounds of fuzzing against qemu.
 *
 * The expected encodings here are what the same instruction assembles
 * to with a real assembler, so this checks the expander against the
 * architecture rather than against itself.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "rv32.h"

#include <stdio.h>

static int failures;

static void
check(const char *what, uint32_t got, uint32_t want)
{
    if (got == want)
        return;
    printf("FAIL %s: got 0x%08x, want 0x%08x\n", what, got, want);
    failures++;
}

/* The compressed loads and stores scatter their offset across the
 * encoding, and the top bit of it sits apart from the rest.  Each case
 * below is a real encoding with its offset written out, so a bit lost
 * anywhere in the assembly shows up as a wrong displacement. */
static void
test_lw_sw_offsets(void)
{
    /* c.lw rd', offset(rs1') : 010 uimm[5:3] rs1' uimm[2|6] rd' 00
     * c.sw rs2', offset(rs1'): 110 uimm[5:3] rs1' uimm[2|6] rs2' 00
     *
     * lw s0, 0(s0)   / lw s0, 64(s0): the second differs from the first
     * only in the bit that carries offset 64. */
    check("c.lw offset 0",  rv_expand_c(0x4000), 0x00042403u);
    check("c.lw offset 64", rv_expand_c(0x4020), 0x04042403u);
    check("c.lw offset 4",  rv_expand_c(0x4040), 0x00442403u);
    check("c.lw offset 68", rv_expand_c(0x4060), 0x04442403u);
    check("c.lw offset 8",  rv_expand_c(0x4400), 0x00842403u);
    check("c.lw offset 72", rv_expand_c(0x4420), 0x04842403u);

    check("c.sw offset 0",  rv_expand_c(0xc000), 0x00842023u);
    check("c.sw offset 64", rv_expand_c(0xc020), 0x04842023u);

    /* the float pair share the immediate assembly */
    check("c.flw offset 64", rv_expand_c(0x6020), 0x04042407u);
    check("c.fsw offset 64", rv_expand_c(0xe020), 0x04842027u);
}

/* c.lwsp and c.swsp use a different scatter again, with the top of the
 * offset above the register field. */
static void
test_sp_offsets(void)
{
    /* lw s0, 0(sp) / lw s0, 192(sp) */
    check("c.lwsp offset 0",   rv_expand_c(0x4402), 0x00012403u);
    check("c.lwsp offset 192", rv_expand_c(0x440e), 0x0c012403u);
    check("c.swsp offset 0",   rv_expand_c(0xc022), 0x00812023u);
    check("c.swsp offset 192", rv_expand_c(0xc1a2), 0x0c812023u);
}

int
main(void)
{
    test_lw_sw_offsets();
    test_sp_offsets();

    if (failures) {
        printf("FAIL: %d check%s failed\n", failures,
               failures == 1 ? "" : "s");
        return 1;
    }
    printf("PASS: rv32 compressed expansion offsets\n");
    return 0;
}
