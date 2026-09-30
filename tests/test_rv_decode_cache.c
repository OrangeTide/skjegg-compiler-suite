/* test_rv_decode_cache.c : the decoded-instruction cache in the RV32 core.
 *
 * A host-side test: it drives emu/rv32.c directly, needing no cross toolchain
 * and no qemu. The cache (rv_dec_enable) is a hint, so its only contract is
 * that it changes nothing about results. Each program runs on two cores in
 * lockstep, one with the cache and one without, and their state must match
 * after every instruction.
 *
 *   loop:    re-executes PCs (cache hits, not refills) and includes a
 *            compressed instruction (the two-byte fill/hit path) and a trap.
 *   selfmod: rewrites an instruction it already ran and executes fence.i, then
 *            runs it again. fence.i must drop the cache so the new instruction
 *            is fetched; without that the cached core would run the stale one
 *            and diverge from the plain core.
 *
 */

#include "rv32.h"

#include <stdio.h>
#include <string.h>

#define MEM_SIZE    0x4000
#define CODE_BASE   0x1000

static uint8_t mem[MEM_SIZE];
static int failures;

static uint32_t r8(void *c, uint32_t a){ (void)c; return a < MEM_SIZE ? mem[a] : 0; }
static uint32_t r16(void *c, uint32_t a){ (void)c; return a + 1 < MEM_SIZE ? (uint32_t)mem[a] | ((uint32_t)mem[a+1] << 8) : 0; }
static uint32_t r32(void *c, uint32_t a){ (void)c; return a + 3 < MEM_SIZE ? (uint32_t)mem[a] | ((uint32_t)mem[a+1]<<8) | ((uint32_t)mem[a+2]<<16) | ((uint32_t)mem[a+3]<<24) : 0; }
static void w8(void *c, uint32_t a, uint32_t v){ (void)c; if (a < MEM_SIZE) mem[a] = (uint8_t)v; }
static void w16(void *c, uint32_t a, uint32_t v){ (void)c; if (a + 1 < MEM_SIZE){ mem[a]=(uint8_t)v; mem[a+1]=(uint8_t)(v>>8); } }
static void w32(void *c, uint32_t a, uint32_t v){ (void)c; if (a + 3 < MEM_SIZE){ mem[a]=(uint8_t)v; mem[a+1]=(uint8_t)(v>>8); mem[a+2]=(uint8_t)(v>>16); mem[a+3]=(uint8_t)(v>>24); } }

static void
poke(uint32_t off, uint32_t insn)
{
    w32(NULL, CODE_BASE + off, insn);
}

/* sum = 10 + 9 + ... + 1 = 55 in x1, then a compressed nop and a trap.
 * Encodings verified against GNU as via skj-as-rv.
 *   0x00 00000093  addi x1, x0, 0
 *   0x04 00a00113  addi x2, x0, 10
 *   0x08 002080b3  add  x1, x1, x2     <- loop body, re-executed
 *   0x0c fff10113  addi x2, x2, -1
 *   0x10 fe011ce3  bne  x2, x0, -8     (back to 0x08)
 *   0x14 0001      c.nop               (two-byte instruction)
 *   0x16 00100073  ebreak */
static void
build_loop(void)
{
    memset(mem, 0, sizeof(mem));
    poke(0x00, 0x00000093u);
    poke(0x04, 0x00a00113u);
    poke(0x08, 0x002080b3u);
    poke(0x0c, 0xfff10113u);
    poke(0x10, 0xfe011ce3u);
    w16(NULL, CODE_BASE + 0x14, 0x0001u);       /* c.nop */
    poke(0x16, 0x00100073u);
}

/* Calls T (a0 += 1), rewrites T to "addi a0, a0, 16", runs fence.i, calls T
 * again. With the cache dropped on fence.i the second call adds 16 (a0 = 17);
 * a stale cache would add 1 again (a0 = 2). Encodings from skj-as-rv, loaded
 * at CODE_BASE = 0x1000, so T is at 0x1024 and the store targets it.
 *   0x00 024000ef  jal  ra, T
 *   0x04 000012b7  lui  t0, 0x1
 *   0x08 02428293  addi t0, t0, 0x24     ; t0 = 0x1024 (&T)
 *   0x0c 01050337  lui  t1, 0x1050
 *   0x10 51330313  addi t1, t1, 0x513    ; t1 = addi a0,a0,16 encoding
 *   0x14 0062a023  sw   t1, 0(t0)        ; overwrite T
 *   0x18 0000100f  fence.i
 *   0x1c 008000ef  jal  ra, T
 *   0x20 00100073  ebreak
 *   0x24 00150513  addi a0, a0, 1        ; T
 *   0x28 00008067  ret */
static void
build_selfmod(void)
{
    memset(mem, 0, sizeof(mem));
    poke(0x00, 0x024000efu);
    poke(0x04, 0x000012b7u);
    poke(0x08, 0x02428293u);
    poke(0x0c, 0x01050337u);
    poke(0x10, 0x51330313u);
    poke(0x14, 0x0062a023u);
    poke(0x18, 0x0000100fu);
    poke(0x1c, 0x008000efu);
    poke(0x20, 0x00100073u);
    poke(0x24, 0x00150513u);
    poke(0x28, 0x00008067u);
}

static void
setup(rv_cpu *cpu)
{
    rv_init(cpu, r8, r16, r32, w8, w16, w32, NULL);
    rv_reset(cpu, CODE_BASE);
    cpu->zcmp = 1;
    cpu->atomics = 1;
    cpu->bitmanip = 1;
    cpu->zcb = 1;
}

static void
mismatch(const char *prog, const char *what, int step, uint32_t got, uint32_t want)
{
    printf("FAIL %s/%s at step %d: cache 0x%08x, plain 0x%08x\n",
           prog, what, step, got, want);
    failures++;
}

/* Run one program on a plain core and a cached core in lockstep, comparing the
 * architectural state after every instruction, until the trap. Then confirm
 * the program's own result (register `reg` == `val`) and the breakpoint. */
static void
run_pair(const char *prog, int reg, uint32_t val)
{
    rv_cpu plain, cached;
    int step;

    setup(&plain);
    setup(&cached);
    rv_dec_enable(&cached);     /* the only difference between the two cores */

    for (step = 0; step < 200 && !plain.in_trap && !cached.in_trap; step++) {
        int r;

        rv_step(&plain);
        rv_step(&cached);

        if (plain.pc != cached.pc)
            mismatch(prog, "pc", step, cached.pc, plain.pc);
        for (r = 0; r < 32; r++)
            if (plain.x[r] != cached.x[r])
                mismatch(prog, "x", step, cached.x[r], plain.x[r]);
        if (plain.in_trap != cached.in_trap)
            mismatch(prog, "in_trap", step, cached.in_trap, plain.in_trap);
        if (plain.mcause != cached.mcause)
            mismatch(prog, "mcause", step, cached.mcause, plain.mcause);
    }

    if (plain.x[reg] != val)
        mismatch(prog, "result(plain)", -1, plain.x[reg], val);
    if (cached.x[reg] != val)
        mismatch(prog, "result(cached)", -1, cached.x[reg], val);
    if (!plain.in_trap || plain.mcause != RV_CAUSE_BREAKPOINT)
        mismatch(prog, "breakpoint", -1, plain.mcause, RV_CAUSE_BREAKPOINT);

    rv_dec_free(&cached);
}

int
main(void)
{
    build_loop();
    run_pair("loop", 1, 55);            /* x1 = sum(1..10) */

    build_selfmod();
    run_pair("selfmod", 10, 17);        /* a0 = 1 + 16, only if fence.i flushed */

    if (failures) {
        printf("test_rv_decode_cache: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_rv_decode_cache: cache and plain decode agree (loop and fence.i)\n");
    return 0;
}
