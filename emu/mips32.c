/* mips32.c : MIPS I user-mode interpreter (see mips32.h) */

#include "mips32.h"

#include <math.h>

/* ABI register names, indexed by number. */
const char *const mips_r_names[32] = {
    "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
    "t0",   "t1", "t2", "t3", "t4", "t5", "t6", "t7",
    "s0",   "s1", "s2", "s3", "s4", "s5", "s6", "s7",
    "t8",   "t9", "k0", "k1", "gp", "sp", "fp", "ra",
};

/* Host signal numbers a kernel would raise, matching the RV32 and ColdFire
 * harnesses (and so the 128+signal exit codes qemu-user reports). */
#define SIG_ILL     4
#define SIG_TRAP    5
#define SIG_FPE     8
#define SIG_BUS     7

/* Primary opcodes. */
enum {
    OP_SPECIAL = 0x00, OP_REGIMM = 0x01, OP_J = 0x02, OP_JAL = 0x03,
    OP_BEQ = 0x04, OP_BNE = 0x05, OP_BLEZ = 0x06, OP_BGTZ = 0x07,
    OP_ADDI = 0x08, OP_ADDIU = 0x09, OP_SLTI = 0x0A, OP_SLTIU = 0x0B,
    OP_ANDI = 0x0C, OP_ORI = 0x0D, OP_XORI = 0x0E, OP_LUI = 0x0F,
    OP_COP1 = 0x11,
    OP_LB = 0x20, OP_LH = 0x21, OP_LWL = 0x22, OP_LW = 0x23,
    OP_LBU = 0x24, OP_LHU = 0x25, OP_LWR = 0x26,
    OP_SB = 0x28, OP_SH = 0x29, OP_SWL = 0x2A, OP_SW = 0x2B, OP_SWR = 0x2E,
    OP_LWC1 = 0x31, OP_SWC1 = 0x39,
};

/* SPECIAL functs. */
enum {
    FN_SLL = 0x00, FN_SRL = 0x02, FN_SRA = 0x03,
    FN_SLLV = 0x04, FN_SRLV = 0x06, FN_SRAV = 0x07,
    FN_JR = 0x08, FN_JALR = 0x09, FN_SYSCALL = 0x0C, FN_BREAK = 0x0D,
    FN_MFHI = 0x10, FN_MTHI = 0x11, FN_MFLO = 0x12, FN_MTLO = 0x13,
    FN_MULT = 0x18, FN_MULTU = 0x19, FN_DIV = 0x1A, FN_DIVU = 0x1B,
    FN_ADD = 0x20, FN_ADDU = 0x21, FN_SUB = 0x22, FN_SUBU = 0x23,
    FN_AND = 0x24, FN_OR = 0x25, FN_XOR = 0x26, FN_NOR = 0x27,
    FN_SLT = 0x2A, FN_SLTU = 0x2B,
};

void
mips_init(mips_cpu *cpu,
          mips_read_fn r8, mips_read_fn r16, mips_read_fn r32,
          mips_write_fn w8, mips_write_fn w16, mips_write_fn w32,
          void *bus_ctx)
{
    memset(cpu, 0, sizeof(*cpu));
    cpu->read8 = r8;   cpu->read16 = r16;  cpu->read32 = r32;
    cpu->write8 = w8;  cpu->write16 = w16; cpu->write32 = w32;
    cpu->bus_ctx = bus_ctx;
    mips_trace_init(&cpu->trace);
}

void
mips_reset(mips_cpu *cpu, uint32_t entry)
{
    int i;

    for (i = 0; i < 32; i++)
        cpu->r[i] = 0;
    cpu->pc = entry;
    cpu->next_pc = entry + 4;
    cpu->cur_pc = entry;
    cpu->hi = cpu->lo = 0;
    cpu->branch_taken = 0;
    cpu->in_delay = 0;
    cpu->load_reg = cpu->load_val = 0;
    cpu->halted = cpu->trapped = cpu->yielding = 0;
    cpu->trap_type = MIPS_TR_NONE;
    cpu->trap_signal = 0;
    cpu->trap_addr = 0;
    cpu->cycles = 0;
    for (i = 0; i < 32; i++)
        cpu->fpr[i] = 0;
    cpu->fcsr = 0;
}

void
mips_set_syscall(mips_cpu *cpu, mips_syscall_fn fn, void *ctx)
{
    cpu->syscall = fn;
    cpu->syscall_ctx = ctx;
}

void
mips_halt(mips_cpu *cpu)
{
    cpu->halted = 1;
}

/* Record a fault/trap and stop the CPU. */
static void
mips_trap(mips_cpu *cpu, int type, int signal, uint32_t addr,
          uint32_t insn, const char *note)
{
    cpu->trapped = 1;
    cpu->trap_type = type;
    cpu->trap_signal = signal;
    cpu->trap_addr = addr;
    mips_trace_push(&cpu->trace, (uint8_t)type, cpu->cur_pc, insn, addr, note);
}

/* The GPR an instruction writes, or 0 if it writes none.  Used to suppress a
 * pending load whose destination the delay-slot instruction overwrites (the
 * comparison is by register number, not value). */
static uint32_t
delay_slot_dest(uint32_t inst)
{
    uint32_t op = inst >> 26;
    uint32_t rt = (inst >> 16) & 0x1F;
    uint32_t rd = (inst >> 11) & 0x1F;
    uint32_t funct = inst & 0x3F;

    switch (op) {
    case OP_SPECIAL:
        switch (funct) {
        case FN_SLL: case FN_SRL: case FN_SRA:
        case FN_SLLV: case FN_SRLV: case FN_SRAV:
        case FN_JALR: case FN_MFHI: case FN_MFLO:
        case FN_ADD: case FN_ADDU: case FN_SUB: case FN_SUBU:
        case FN_AND: case FN_OR: case FN_XOR: case FN_NOR:
        case FN_SLT: case FN_SLTU:
            return rd;
        default:
            return 0;       /* JR, MULT/DIV, MTHI/MTLO, SYSCALL, BREAK */
        }
    case OP_ADDI: case OP_ADDIU: case OP_SLTI: case OP_SLTIU:
    case OP_ANDI: case OP_ORI: case OP_XORI: case OP_LUI:
    case OP_LB: case OP_LH: case OP_LWL: case OP_LW:
    case OP_LBU: case OP_LHU: case OP_LWR:
        return rt;
    case OP_REGIMM:
        return ((rt & 0x1E) == 0x10) ? 31u : 0u;   /* BLTZAL/BGEZAL link ra */
    case OP_JAL:
        return 31u;
    default:
        return 0;
    }
}

/* Take a branch: the instruction already at pc runs as the delay slot, then
 * control transfers to target. */
static void
do_branch(mips_cpu *cpu, uint32_t target)
{
    cpu->next_pc = target;
    cpu->branch_taken = 1;
}

/****************************************************************
 * COP1 (floating-point coprocessor 1)
 *
 * MIPS I FPU: 32 single-precision registers; a double occupies an even/odd
 * pair named by the even register (even = low word, odd = high word, matching
 * the little-endian lwc1/swc1 pair a double load expands to).  FCSR bit 23 is
 * the single condition flag C that c.cond sets and bc1t/bc1f test; bits 0-1 are
 * the rounding mode cvt.w honours.
 ****************************************************************/

#define FCSR_C_BIT  (1u << 23)

static float
fpr_s(const mips_cpu *cpu, uint32_t i)
{
    float f;
    memcpy(&f, &cpu->fpr[i], 4);
    return f;
}

static void
set_s(mips_cpu *cpu, uint32_t i, float f)
{
    memcpy(&cpu->fpr[i], &f, 4);
}

static double
fpr_d(const mips_cpu *cpu, uint32_t i)
{
    uint64_t bits = (uint64_t)cpu->fpr[i] | ((uint64_t)cpu->fpr[i + 1] << 32);
    double d;
    memcpy(&d, &bits, 8);
    return d;
}

static void
set_d(mips_cpu *cpu, uint32_t i, double d)
{
    uint64_t bits;
    memcpy(&bits, &d, 8);
    cpu->fpr[i] = (uint32_t)bits;
    cpu->fpr[i + 1] = (uint32_t)(bits >> 32);
}

/* Convert to a 32-bit signed integer honouring the FCSR rounding mode; NaN or
 * an out-of-range value yields INT_MAX, as MIPS cvt.w does. */
static uint32_t
fp_to_w(double v, uint32_t rm)
{
    double r;

    switch (rm & 3) {
    default:
    case 0: r = nearbyint(v); break;        /* RN: to nearest even (host RN) */
    case 1: r = trunc(v); break;            /* RZ: toward zero */
    case 2: r = ceil(v); break;             /* RP: toward +inf */
    case 3: r = floor(v); break;            /* RM: toward -inf */
    }
    if (!(r >= -2147483648.0 && r <= 2147483647.0))
        return 0x7fffffffu;
    return (uint32_t)(int32_t)r;
}

/* Execute a COP1 (opcode 0x11) instruction.  Returns 0, or -1 on a trap. */
static int
mips_cop1(mips_cpu *cpu, uint32_t inst)
{
    uint32_t fmt   = (inst >> 21) & 0x1F;
    uint32_t ft    = (inst >> 16) & 0x1F;   /* GPR for moves, FPR source 2 */
    uint32_t fs    = (inst >> 11) & 0x1F;
    uint32_t fd    = (inst >> 6) & 0x1F;
    uint32_t funct = inst & 0x3F;

    switch (fmt) {
    case 0x00:                              /* MFC1: GPR[ft] = FPR[fs] bits */
        if (ft) cpu->r[ft] = cpu->fpr[fs];
        return 0;
    case 0x04:                              /* MTC1: FPR[fs] = GPR[ft] bits */
        cpu->fpr[fs] = cpu->r[ft];
        return 0;
    case 0x02:                              /* CFC1: GPR[ft] = control[fs] */
        if (ft)
            cpu->r[ft] = (fs == 31) ? cpu->fcsr
                       : (fs == 0)  ? 0x00000300u   /* FIR: a MIPS I FPU */
                       : 0;
        return 0;
    case 0x06:                              /* CTC1: control[fs] = GPR[ft] */
        if (fs == 31)
            cpu->fcsr = cpu->r[ft];
        return 0;
    case 0x08: {                            /* BC1F / BC1T */
        int tf = ft & 1;                    /* bit 16: 0 = bc1f, 1 = bc1t */
        int cond = (cpu->fcsr & FCSR_C_BIT) != 0;
        int32_t off = (int32_t)(int16_t)(inst & 0xFFFF);
        if (tf == cond)
            do_branch(cpu, cpu->pc + ((uint32_t)off << 2));
        return 0;
    }
    case 0x10:                              /* single precision */
    case 0x11: {                            /* double precision */
        int d = (fmt == 0x11);
        switch (funct) {
        case 0x00:                          /* add */
            if (d) set_d(cpu, fd, fpr_d(cpu, fs) + fpr_d(cpu, ft));
            else   set_s(cpu, fd, fpr_s(cpu, fs) + fpr_s(cpu, ft));
            return 0;
        case 0x01:                          /* sub */
            if (d) set_d(cpu, fd, fpr_d(cpu, fs) - fpr_d(cpu, ft));
            else   set_s(cpu, fd, fpr_s(cpu, fs) - fpr_s(cpu, ft));
            return 0;
        case 0x02:                          /* mul */
            if (d) set_d(cpu, fd, fpr_d(cpu, fs) * fpr_d(cpu, ft));
            else   set_s(cpu, fd, fpr_s(cpu, fs) * fpr_s(cpu, ft));
            return 0;
        case 0x03:                          /* div */
            if (d) set_d(cpu, fd, fpr_d(cpu, fs) / fpr_d(cpu, ft));
            else   set_s(cpu, fd, fpr_s(cpu, fs) / fpr_s(cpu, ft));
            return 0;
        case 0x04:                          /* sqrt */
            if (d) set_d(cpu, fd, sqrt(fpr_d(cpu, fs)));
            else   set_s(cpu, fd, (float)sqrt((double)fpr_s(cpu, fs)));
            return 0;
        case 0x05:                          /* abs */
            if (d) set_d(cpu, fd, fabs(fpr_d(cpu, fs)));
            else   set_s(cpu, fd, fabsf(fpr_s(cpu, fs)));
            return 0;
        case 0x06:                          /* mov */
            cpu->fpr[fd] = cpu->fpr[fs];
            if (d) cpu->fpr[fd + 1] = cpu->fpr[fs + 1];
            return 0;
        case 0x07:                          /* neg */
            if (d) set_d(cpu, fd, -fpr_d(cpu, fs));
            else   set_s(cpu, fd, -fpr_s(cpu, fs));
            return 0;
        case 0x20:                          /* cvt.s */
            set_s(cpu, fd, d ? (float)fpr_d(cpu, fs) : fpr_s(cpu, fs));
            return 0;
        case 0x21:                          /* cvt.d */
            set_d(cpu, fd, d ? fpr_d(cpu, fs) : (double)fpr_s(cpu, fs));
            return 0;
        case 0x24:                          /* cvt.w: FP -> int, into FPR[fd] */
            cpu->fpr[fd] = fp_to_w(d ? fpr_d(cpu, fs) : (double)fpr_s(cpu, fs),
                                   cpu->fcsr);
            return 0;
        case 0x30: case 0x31: case 0x32: case 0x33:
        case 0x34: case 0x35: case 0x36: case 0x37:
        case 0x38: case 0x39: case 0x3a: case 0x3b:
        case 0x3c: case 0x3d: case 0x3e: case 0x3f: {
            /* c.cond.fmt : the low condition bits select the less / equal /
             * unordered contributions to the FP condition flag.  bit 3 marks
             * the signalling variant, which differs only in NaN exceptions
             * (not modelled). */
            double x = d ? fpr_d(cpu, fs) : (double)fpr_s(cpu, fs);
            double y = d ? fpr_d(cpu, ft) : (double)fpr_s(cpu, ft);
            int un = isunordered(x, y);
            int lt = !un && x < y;
            int eq = !un && x == y;
            int cc = (int)(funct & 0xF);
            int res = ((cc & 0x4) && lt) || ((cc & 0x2) && eq)
                   || ((cc & 0x1) && un);
            if (res) cpu->fcsr |= FCSR_C_BIT;
            else     cpu->fcsr &= ~FCSR_C_BIT;
            return 0;
        }
        default:
            break;
        }
        break;
    }
    case 0x14: {                            /* W: integer source, cvt to float */
        int32_t w = (int32_t)cpu->fpr[fs];
        switch (funct) {
        case 0x20: set_s(cpu, fd, (float)w);  return 0;   /* cvt.s.w */
        case 0x21: set_d(cpu, fd, (double)w); return 0;   /* cvt.d.w */
        default:   break;
        }
        break;
    }
    default:
        break;
    }

    mips_trap(cpu, MIPS_TR_ILLEGAL, SIG_ILL, 0, inst, "reserved COP1");
    return -1;
}

int
mips_step(mips_cpu *cpu)
{
    uint32_t inst, op, rs, rt, rd, sa, funct, imm_ze, target26;
    int32_t imm_se;
    uint32_t dreg, dval;
    uint32_t a, b, addr, aligned;

    if (cpu->halted || cpu->trapped)
        return -1;

    cpu->cur_pc = cpu->pc;
    cpu->in_delay = cpu->branch_taken;

    if (cpu->pc & 3) {
        mips_trap(cpu, MIPS_TR_FETCH_FAULT, SIG_BUS, cpu->pc, 0,
                  "misaligned instruction fetch");
        return -1;
    }
    inst = cpu->read32(cpu->bus_ctx, cpu->pc);

    /* Advance the PC pair before executing, so a link value is pc+4 (the
     * instruction after the delay slot) and a branch base is pc (the slot). */
    cpu->pc = cpu->next_pc;
    cpu->next_pc += 4;
    cpu->branch_taken = 0;

    /* Snapshot and clear the pending-load latch; a load this instruction
     * issues writes into the now-clear latch. */
    dreg = cpu->load_reg;
    dval = cpu->load_val;
    cpu->load_reg = 0;
    cpu->load_val = 0;

    op = inst >> 26;
    rs = (inst >> 21) & 0x1F;
    rt = (inst >> 16) & 0x1F;
    rd = (inst >> 11) & 0x1F;
    sa = (inst >> 6) & 0x1F;
    funct = inst & 0x3F;
    imm_ze = inst & 0xFFFF;
    imm_se = (int32_t)(int16_t)(inst & 0xFFFF);
    target26 = inst & 0x3FFFFFF;

    switch (op) {
    case OP_SPECIAL:
        switch (funct) {
        case FN_SLL:  cpu->r[rd] = cpu->r[rt] << sa; break;
        case FN_SRL:  cpu->r[rd] = cpu->r[rt] >> sa; break;
        case FN_SRA:  cpu->r[rd] = (uint32_t)((int32_t)cpu->r[rt] >> sa); break;
        case FN_SLLV: cpu->r[rd] = cpu->r[rt] << (cpu->r[rs] & 0x1F); break;
        case FN_SRLV: cpu->r[rd] = cpu->r[rt] >> (cpu->r[rs] & 0x1F); break;
        case FN_SRAV:
            cpu->r[rd] = (uint32_t)((int32_t)cpu->r[rt] >> (cpu->r[rs] & 0x1F));
            break;
        case FN_JR:
            do_branch(cpu, cpu->r[rs]);
            break;
        case FN_JALR:
            a = cpu->r[rs];             /* read target before writing rd */
            cpu->r[rd] = cpu->pc + 4;
            do_branch(cpu, a);
            break;
        case FN_SYSCALL:
            if (cpu->syscall)
                cpu->syscall(cpu, cpu->syscall_ctx);
            else
                mips_trap(cpu, MIPS_TR_SYSCALL, SIG_TRAP, 0, inst,
                          "syscall with no handler");
            break;
        case FN_BREAK: {
            uint32_t code = (inst >> 6) & 0xFFFFF;
            int sig = (code == 6 || code == 7) ? SIG_FPE : SIG_TRAP;
            mips_trap(cpu, MIPS_TR_BREAK, sig, code, inst, "break");
            break;
        }
        case FN_MFHI: cpu->r[rd] = cpu->hi; break;
        case FN_MTHI: cpu->hi = cpu->r[rs]; break;
        case FN_MFLO: cpu->r[rd] = cpu->lo; break;
        case FN_MTLO: cpu->lo = cpu->r[rs]; break;
        case FN_MULT: {
            int64_t p = (int64_t)(int32_t)cpu->r[rs] * (int64_t)(int32_t)cpu->r[rt];
            cpu->lo = (uint32_t)p;
            cpu->hi = (uint32_t)((uint64_t)p >> 32);
            break;
        }
        case FN_MULTU: {
            uint64_t p = (uint64_t)cpu->r[rs] * (uint64_t)cpu->r[rt];
            cpu->lo = (uint32_t)p;
            cpu->hi = (uint32_t)(p >> 32);
            break;
        }
        case FN_DIV: {
            int32_t n = (int32_t)cpu->r[rs], d = (int32_t)cpu->r[rt];
            if (d == 0) {
                cpu->lo = (n >= 0) ? 0xFFFFFFFFu : 1u;
                cpu->hi = (uint32_t)n;
            } else if ((uint32_t)n == 0x80000000u && d == -1) {
                cpu->lo = 0x80000000u;
                cpu->hi = 0;
            } else {
                cpu->lo = (uint32_t)(n / d);
                cpu->hi = (uint32_t)(n % d);
            }
            break;
        }
        case FN_DIVU: {
            uint32_t n = cpu->r[rs], d = cpu->r[rt];
            if (d == 0) {
                cpu->lo = 0xFFFFFFFFu;
                cpu->hi = n;
            } else {
                cpu->lo = n / d;
                cpu->hi = n % d;
            }
            break;
        }
        case FN_ADD: {
            a = cpu->r[rs]; b = cpu->r[rt];
            uint32_t r = a + b;
            if ((int32_t)((a ^ r) & (b ^ r)) < 0) {
                mips_trap(cpu, MIPS_TR_OVERFLOW, SIG_FPE, 0, inst,
                          "add overflow");
                return -1;
            }
            cpu->r[rd] = r;
            break;
        }
        case FN_ADDU: cpu->r[rd] = cpu->r[rs] + cpu->r[rt]; break;
        case FN_SUB: {
            a = cpu->r[rs]; b = cpu->r[rt];
            uint32_t r = a - b;
            if ((int32_t)((a ^ b) & (a ^ r)) < 0) {
                mips_trap(cpu, MIPS_TR_OVERFLOW, SIG_FPE, 0, inst,
                          "sub overflow");
                return -1;
            }
            cpu->r[rd] = r;
            break;
        }
        case FN_SUBU: cpu->r[rd] = cpu->r[rs] - cpu->r[rt]; break;
        case FN_AND:  cpu->r[rd] = cpu->r[rs] & cpu->r[rt]; break;
        case FN_OR:   cpu->r[rd] = cpu->r[rs] | cpu->r[rt]; break;
        case FN_XOR:  cpu->r[rd] = cpu->r[rs] ^ cpu->r[rt]; break;
        case FN_NOR:  cpu->r[rd] = ~(cpu->r[rs] | cpu->r[rt]); break;
        case FN_SLT:
            cpu->r[rd] = ((int32_t)cpu->r[rs] < (int32_t)cpu->r[rt]) ? 1 : 0;
            break;
        case FN_SLTU:
            cpu->r[rd] = (cpu->r[rs] < cpu->r[rt]) ? 1 : 0;
            break;
        default:
            mips_trap(cpu, MIPS_TR_ILLEGAL, SIG_ILL, 0, inst,
                      "reserved SPECIAL funct");
            return -1;
        }
        break;

    case OP_REGIMM: {
        int is_ge = rt & 1;
        int link = (rt & 0x1E) == 0x10;
        int take;

        if (link)
            cpu->r[31] = cpu->pc + 4;   /* link whether or not taken */
        take = is_ge ? ((int32_t)cpu->r[rs] >= 0) : ((int32_t)cpu->r[rs] < 0);
        if (take)
            do_branch(cpu, cpu->pc + ((uint32_t)imm_se << 2));
        break;
    }

    case OP_J:
        do_branch(cpu, (cpu->pc & 0xF0000000u) | (target26 << 2));
        break;
    case OP_JAL:
        cpu->r[31] = cpu->pc + 4;
        do_branch(cpu, (cpu->pc & 0xF0000000u) | (target26 << 2));
        break;
    case OP_BEQ:
        if (cpu->r[rs] == cpu->r[rt])
            do_branch(cpu, cpu->pc + ((uint32_t)imm_se << 2));
        break;
    case OP_BNE:
        if (cpu->r[rs] != cpu->r[rt])
            do_branch(cpu, cpu->pc + ((uint32_t)imm_se << 2));
        break;
    case OP_BLEZ:
        if ((int32_t)cpu->r[rs] <= 0)
            do_branch(cpu, cpu->pc + ((uint32_t)imm_se << 2));
        break;
    case OP_BGTZ:
        if ((int32_t)cpu->r[rs] > 0)
            do_branch(cpu, cpu->pc + ((uint32_t)imm_se << 2));
        break;

    case OP_ADDI: {
        a = cpu->r[rs]; b = (uint32_t)imm_se;
        uint32_t r = a + b;
        if ((int32_t)((a ^ r) & (b ^ r)) < 0) {
            mips_trap(cpu, MIPS_TR_OVERFLOW, SIG_FPE, 0, inst, "addi overflow");
            return -1;
        }
        cpu->r[rt] = r;
        break;
    }
    case OP_ADDIU: cpu->r[rt] = cpu->r[rs] + (uint32_t)imm_se; break;
    case OP_SLTI:
        cpu->r[rt] = ((int32_t)cpu->r[rs] < imm_se) ? 1 : 0;
        break;
    case OP_SLTIU:
        cpu->r[rt] = (cpu->r[rs] < (uint32_t)imm_se) ? 1 : 0;
        break;
    case OP_ANDI: cpu->r[rt] = cpu->r[rs] & imm_ze; break;
    case OP_ORI:  cpu->r[rt] = cpu->r[rs] | imm_ze; break;
    case OP_XORI: cpu->r[rt] = cpu->r[rs] ^ imm_ze; break;
    case OP_LUI:  cpu->r[rt] = imm_ze << 16; break;

    case OP_COP1:
        if (mips_cop1(cpu, inst) < 0)
            return -1;
        break;

    case OP_LWC1:                       /* FP load: FPR[ft] = mem[rs+imm] */
        addr = cpu->r[rs] + (uint32_t)imm_se;
        if (addr & 3) {
            mips_trap(cpu, MIPS_TR_LOAD_MISALIGN, SIG_BUS, addr, inst,
                      "misaligned lwc1");
            return -1;
        }
        cpu->fpr[rt] = cpu->read32(cpu->bus_ctx, addr);
        break;
    case OP_SWC1:                       /* FP store: mem[rs+imm] = FPR[ft] */
        addr = cpu->r[rs] + (uint32_t)imm_se;
        if (addr & 3) {
            mips_trap(cpu, MIPS_TR_STORE_MISALIGN, SIG_BUS, addr, inst,
                      "misaligned swc1");
            return -1;
        }
        cpu->write32(cpu->bus_ctx, addr, cpu->fpr[rt]);
        break;

    case OP_LB:
        addr = cpu->r[rs] + (uint32_t)imm_se;
        if (rt) { cpu->load_reg = rt;
            cpu->load_val = (uint32_t)(int32_t)(int8_t)cpu->read8(cpu->bus_ctx, addr); }
        break;
    case OP_LBU:
        addr = cpu->r[rs] + (uint32_t)imm_se;
        if (rt) { cpu->load_reg = rt;
            cpu->load_val = cpu->read8(cpu->bus_ctx, addr) & 0xFF; }
        break;
    case OP_LH:
        addr = cpu->r[rs] + (uint32_t)imm_se;
        if (addr & 1) {
            mips_trap(cpu, MIPS_TR_LOAD_MISALIGN, SIG_BUS, addr, inst,
                      "misaligned lh");
            return -1;
        }
        if (rt) { cpu->load_reg = rt;
            cpu->load_val = (uint32_t)(int32_t)(int16_t)cpu->read16(cpu->bus_ctx, addr); }
        break;
    case OP_LHU:
        addr = cpu->r[rs] + (uint32_t)imm_se;
        if (addr & 1) {
            mips_trap(cpu, MIPS_TR_LOAD_MISALIGN, SIG_BUS, addr, inst,
                      "misaligned lhu");
            return -1;
        }
        if (rt) { cpu->load_reg = rt;
            cpu->load_val = cpu->read16(cpu->bus_ctx, addr) & 0xFFFF; }
        break;
    case OP_LW:
        addr = cpu->r[rs] + (uint32_t)imm_se;
        if (addr & 3) {
            mips_trap(cpu, MIPS_TR_LOAD_MISALIGN, SIG_BUS, addr, inst,
                      "misaligned lw");
            return -1;
        }
        if (rt) { cpu->load_reg = rt;
            cpu->load_val = cpu->read32(cpu->bus_ctx, addr); }
        break;
    case OP_LWL: {
        uint32_t cur = (dreg == rt) ? dval : cpu->r[rt];
        addr = cpu->r[rs] + (uint32_t)imm_se;
        aligned = cpu->read32(cpu->bus_ctx, addr & ~3u);
        switch (addr & 3) {
        case 0: cur = (cur & 0x00FFFFFFu) | (aligned << 24); break;
        case 1: cur = (cur & 0x0000FFFFu) | (aligned << 16); break;
        case 2: cur = (cur & 0x000000FFu) | (aligned << 8); break;
        case 3: cur = aligned; break;
        }
        if (rt) { cpu->load_reg = rt; cpu->load_val = cur; }
        break;
    }
    case OP_LWR: {
        uint32_t cur = (dreg == rt) ? dval : cpu->r[rt];
        addr = cpu->r[rs] + (uint32_t)imm_se;
        aligned = cpu->read32(cpu->bus_ctx, addr & ~3u);
        switch (addr & 3) {
        case 0: cur = aligned; break;
        case 1: cur = (cur & 0xFF000000u) | (aligned >> 8); break;
        case 2: cur = (cur & 0xFFFF0000u) | (aligned >> 16); break;
        case 3: cur = (cur & 0xFFFFFF00u) | (aligned >> 24); break;
        }
        if (rt) { cpu->load_reg = rt; cpu->load_val = cur; }
        break;
    }

    case OP_SB:
        addr = cpu->r[rs] + (uint32_t)imm_se;
        cpu->write8(cpu->bus_ctx, addr, cpu->r[rt] & 0xFF);
        break;
    case OP_SH:
        addr = cpu->r[rs] + (uint32_t)imm_se;
        if (addr & 1) {
            mips_trap(cpu, MIPS_TR_STORE_MISALIGN, SIG_BUS, addr, inst,
                      "misaligned sh");
            return -1;
        }
        cpu->write16(cpu->bus_ctx, addr, cpu->r[rt] & 0xFFFF);
        break;
    case OP_SW:
        addr = cpu->r[rs] + (uint32_t)imm_se;
        if (addr & 3) {
            mips_trap(cpu, MIPS_TR_STORE_MISALIGN, SIG_BUS, addr, inst,
                      "misaligned sw");
            return -1;
        }
        cpu->write32(cpu->bus_ctx, addr, cpu->r[rt]);
        break;
    case OP_SWL: {
        uint32_t reg = cpu->r[rt], mem;
        addr = cpu->r[rs] + (uint32_t)imm_se;
        mem = cpu->read32(cpu->bus_ctx, addr & ~3u);
        switch (addr & 3) {
        case 0: mem = (mem & 0xFFFFFF00u) | (reg >> 24); break;
        case 1: mem = (mem & 0xFFFF0000u) | (reg >> 16); break;
        case 2: mem = (mem & 0xFF000000u) | (reg >> 8); break;
        case 3: mem = reg; break;
        }
        cpu->write32(cpu->bus_ctx, addr & ~3u, mem);
        break;
    }
    case OP_SWR: {
        uint32_t reg = cpu->r[rt], mem;
        addr = cpu->r[rs] + (uint32_t)imm_se;
        mem = cpu->read32(cpu->bus_ctx, addr & ~3u);
        switch (addr & 3) {
        case 0: mem = reg; break;
        case 1: mem = (mem & 0x000000FFu) | (reg << 8); break;
        case 2: mem = (mem & 0x0000FFFFu) | (reg << 16); break;
        case 3: mem = (mem & 0x00FFFFFFu) | (reg << 24); break;
        }
        cpu->write32(cpu->bus_ctx, addr & ~3u, mem);
        break;
    }

    default:
        mips_trap(cpu, MIPS_TR_ILLEGAL, SIG_ILL, 0, inst, "reserved opcode");
        return -1;
    }

    cpu->r[0] = 0;

    /* Complete the previous instruction's deferred load, unless the just
     * executed instruction wrote the same register (by number). */
    if (dreg != 0 && delay_slot_dest(inst) != dreg)
        cpu->r[dreg] = dval;
    cpu->r[0] = 0;

    cpu->cycles++;
    return (cpu->halted || cpu->trapped) ? -1 : 0;
}

enum skj_run_reason
mips_run(mips_cpu *cpu, int count, int *retired)
{
    enum skj_run_reason reason = SKJ_RUN_BUDGET;
    int n = 0;

    while (n < count) {
        if (cpu->halted) { reason = SKJ_RUN_HALT; break; }
        if (cpu->trapped) { reason = SKJ_RUN_TRAP; break; }
        if (cpu->yielding) { reason = SKJ_RUN_YIELD; break; }
        mips_step(cpu);
        n++;
        if (cpu->halted) { reason = SKJ_RUN_HALT; break; }
        if (cpu->trapped) { reason = SKJ_RUN_TRAP; break; }
        if (cpu->yielding) { reason = SKJ_RUN_YIELD; break; }
    }
    if (retired)
        *retired = n;
    return reason;
}
